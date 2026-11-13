/*
 * ArcradientJS 1.0 (Clean-Room Pure C11 ECMAScript 2026 Virtual Machine)
 * js/vm/ajs_internal.h - Internal VM, GC, Object, Compiler, Builtin & Module Structures
 */
#ifndef AJS_INTERNAL_H
#define AJS_INTERNAL_H

#include "ajs_vm.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * 1. Portable 32/64-bit IEEE-754 & NaN-Boxed AjsValue Representation
 * ============================================================================ */

#define AJS_VAL_POS_INF   UINT64_C(0x7FF0000000000000)
#define AJS_VAL_NEG_INF   UINT64_C(0xFFF0000000000000)
#define AJS_VAL_NAN       UINT64_C(0x7FF8000000000000)

#define AJS_VAL_UNDEFINED UINT64_C(0x7FF9000000000000)
#define AJS_VAL_NULL      UINT64_C(0x7FF9000000000001)
#define AJS_VAL_FALSE     UINT64_C(0x7FF9000000000002)
#define AJS_VAL_TRUE      UINT64_C(0x7FF9000000000003)
#define AJS_VAL_TDZ       UINT64_C(0x7FF9000000000004)
#define AJS_VAL_EMPTY     UINT64_C(0x7FF9000000000005)
#define AJS_VAL_EXCEPTION UINT64_C(0x7FF9000000000006)

#define AJS_PTR_TAG       UINT64_C(0xFFF0000000000000)
#define AJS_PTR_MASK      UINT64_C(0x000FFFFFFFFFFFFF)

/* Forces 64-bit IEEE-754 rounding even on 32-bit x86 with x87 FPU (-mno-sse2) */
static inline double ajs_f64_round(double x) {
    volatile double vx = x;
    return vx;
}

static inline float ajs_f32_round(double x) {
    volatile float vf = (float)x;
    return vf;
}

static inline uint64_t ajs_double_to_bits(double d) {
    volatile double vd = d;
    double clean = vd;
    uint64_t bits;
    memcpy(&bits, &clean, sizeof(bits));
    return bits;
}

static inline double ajs_bits_to_double(uint64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static inline AjsValue ajs_val_number(double d) {
    uint64_t bits = ajs_double_to_bits(d);
    if ((bits & AJS_VAL_POS_INF) == AJS_VAL_POS_INF &&
        (bits & UINT64_C(0x000FFFFFFFFFFFFF)) != 0) {
        return AJS_VAL_NAN;
    }
    return bits;
}

static inline bool ajs_val_is_ptr(AjsValue v) {
    return v > AJS_VAL_NEG_INF;
}

static inline bool ajs_val_is_number(AjsValue v) {
    if (v > AJS_VAL_NEG_INF) return false;
    if (v >= AJS_VAL_UNDEFINED && v < UINT64_C(0x8000000000000000)) return false;
    return true;
}

static inline double ajs_val_to_number_unchecked(AjsValue v) {
    return ajs_bits_to_double(v);
}

/* Safe C11 memcpy/memmove helpers avoiding UBSan NULL-with-zero-length */
static inline void ajs_memcpy(void *dst, const void *src, size_t n) {
    if (n > 0) memcpy(dst, src, n);
}

static inline void ajs_memmove(void *dst, const void *src, size_t n) {
    if (n > 0) memmove(dst, src, n);
}

/* ============================================================================
 * 2. Garbage Collector Header & Core Heap Types
 * ============================================================================ */

typedef enum AjsGcKind {
    AJS_GC_STRING   = 1,
    AJS_GC_SYMBOL   = 2,
    AJS_GC_BIGINT   = 3,
    AJS_GC_OBJECT   = 4,
    AJS_GC_BYTECODE = 5,
    AJS_GC_ENV      = 6,
    AJS_GC_MODULE   = 7,
    AJS_GC_FRAME    = 8
} AjsGcKind;

typedef struct AjsGcHeader {
    struct AjsGcHeader *gc_next;
    size_t              gc_size;
    uint8_t             gc_kind;
    uint8_t             gc_marked;
    uint8_t             alloc_shift;
    uint8_t             gc_flags;
    uint32_t            gc_pad;
} AjsGcHeader;

static inline AjsValue ajs_val_from_gc(const void *ptr) {
    uintptr_t u = (uintptr_t)ptr;
    return AJS_PTR_TAG | (((uint64_t)u) >> 3);
}

static inline AjsGcHeader *ajs_val_to_gc(AjsValue v) {
    uintptr_t u = (uintptr_t)((v & AJS_PTR_MASK) << 3);
    return (AjsGcHeader *)u;
}

static inline bool ajs_val_is_gc_kind(AjsValue v, AjsGcKind kind) {
    return ajs_val_is_ptr(v) && ajs_val_to_gc(v)->gc_kind == (uint8_t)kind;
}

typedef struct AjsString    AjsString;
typedef struct AjsSymbol    AjsSymbol;
typedef struct AjsBigInt    AjsBigInt;
typedef struct AjsObject    AjsObject;
typedef struct AjsBytecode  AjsBytecode;
typedef struct AjsEnv       AjsEnv;
typedef struct AjsExecFrame AjsExecFrame;
typedef struct AjsRegExp    AjsRegExp;

/* UTF-16 code-unit observable JS string with cached UTF-8 C-string */
struct AjsString {
    AjsGcHeader gc;
    uint32_t    len16;
    uint32_t    len8;
    uint32_t    hash;
    uint32_t    array_idx;      /* Valid when is_array_idx == true (0..0xFFFFFFFE) */
    bool        is_array_idx;
    bool        is_interned;
    uint16_t   *units;          /* Points into trailing payload */
    char       *utf8;           /* NUL-terminated UTF-8 in trailing payload */
};

/* Symbol (standard, global registry, well-known, or class private #name) */
struct AjsSymbol {
    AjsGcHeader gc;
    uint32_t    id;
    bool        has_desc;
    bool        is_registered;
    bool        is_private;
    AjsString  *desc;
};

/* Arbitrary-precision signed BigInt (little-endian 32-bit limbs) */
struct AjsBigInt {
    AjsGcHeader gc;
    int32_t     sign;           /* 0 = zero, 1 = positive, -1 = negative */
    uint32_t    len;            /* Number of active limbs (0 if sign == 0) */
    uint32_t    limbs[];
};

/* ============================================================================
 * 3. Well-Known Symbols Enumeration
 * ============================================================================ */

typedef enum AjsWellKnownSym {
    AJS_SYM_ITERATOR = 0,
    AJS_SYM_ASYNC_ITERATOR,
    AJS_SYM_HAS_INSTANCE,
    AJS_SYM_IS_CONCAT_SPREADABLE,
    AJS_SYM_MATCH,
    AJS_SYM_MATCH_ALL,
    AJS_SYM_REPLACE,
    AJS_SYM_SEARCH,
    AJS_SYM_SPECIES,
    AJS_SYM_SPLIT,
    AJS_SYM_TO_PRIMITIVE,
    AJS_SYM_TO_STRING_TAG,
    AJS_SYM_UNSCOPABLES,
    AJS_SYM_DISPOSE,
    AJS_SYM_ASYNC_DISPOSE,
    AJS_SYM_COUNT
} AjsWellKnownSym;

/* ============================================================================
 * 4. Property Descriptors & Ordinary/Exotic Object Model
 * ============================================================================ */

#define AJS_DESC_WRITABLE         (1u << 0)
#define AJS_DESC_ENUMERABLE       (1u << 1)
#define AJS_DESC_CONFIGURABLE     (1u << 2)
#define AJS_DESC_IS_ACCESSOR      (1u << 3)
#define AJS_DESC_HAS_VALUE        (1u << 4)
#define AJS_DESC_HAS_WRITABLE     (1u << 5)
#define AJS_DESC_HAS_ENUMERABLE   (1u << 6)
#define AJS_DESC_HAS_CONFIGURABLE (1u << 7)
#define AJS_DESC_HAS_GET          (1u << 8)
#define AJS_DESC_HAS_SET          (1u << 9)

typedef struct AjsPropDesc {
    uint32_t flags;
    AjsValue value;
    AjsValue getter;
    AjsValue setter;
} AjsPropDesc;

typedef struct AjsPropSlot {
    AjsValue key;               /* Interned AjsString or AjsSymbol, or AJS_VAL_EMPTY if deleted */
    uint32_t flags;             /* AJS_DESC_WRITABLE | ENUMERABLE | CONFIGURABLE | IS_ACCESSOR */
    uint32_t seq;               /* Monotonic insertion sequence */
    AjsValue value;             /* Data value OR getter */
    AjsValue setter;            /* Setter when IS_ACCESSOR */
} AjsPropSlot;

typedef enum AjsPrivateKind {
    AJS_PRIV_FIELD    = 1,
    AJS_PRIV_METHOD   = 2,
    AJS_PRIV_ACCESSOR = 3
} AjsPrivateKind;

typedef struct AjsPrivateSlot {
    AjsSymbol *sym;
    uint8_t    kind;
    AjsValue   value;           /* Field value, method, or getter */
    AjsValue   setter;
} AjsPrivateSlot;

typedef enum AjsObjKind {
    AJS_OBJ_ORDINARY = 0,
    AJS_OBJ_ARRAY,
    AJS_OBJ_FUNCTION,
    AJS_OBJ_ARGUMENTS,
    AJS_OBJ_BOOLEAN_OBJ,
    AJS_OBJ_NUMBER_OBJ,
    AJS_OBJ_STRING_OBJ,
    AJS_OBJ_SYMBOL_OBJ,
    AJS_OBJ_BIGINT_OBJ,
    AJS_OBJ_ERROR,
    AJS_OBJ_DATE,
    AJS_OBJ_REGEXP,
    AJS_OBJ_PROMISE,
    AJS_OBJ_MAP,
    AJS_OBJ_SET,
    AJS_OBJ_WEAKMAP,
    AJS_OBJ_WEAKSET,
    AJS_OBJ_WEAKREF,
    AJS_OBJ_FINREG,
    AJS_OBJ_ITERATOR,
    AJS_OBJ_GENERATOR,
    AJS_OBJ_ASYNC_GENERATOR,
    AJS_OBJ_ARRAY_BUFFER,
    AJS_OBJ_SHARED_ARRAY_BUFFER,
    AJS_OBJ_TYPED_ARRAY,
    AJS_OBJ_DATA_VIEW,
    AJS_OBJ_PROXY,
    AJS_OBJ_MODULE_NS,
    AJS_OBJ_INTL
} AjsObjKind;

typedef enum AjsFuncKind {
    AJS_FN_NATIVE = 0,
    AJS_FN_BC_NORMAL,
    AJS_FN_BC_ARROW,
    AJS_FN_BC_METHOD,
    AJS_FN_BC_CLASS_CTOR,
    AJS_FN_BC_GENERATOR,
    AJS_FN_BC_ASYNC,
    AJS_FN_BC_ASYNC_ARROW,
    AJS_FN_BC_ASYNC_GENERATOR,
    AJS_FN_BOUND
} AjsFuncKind;

typedef struct AjsFuncData {
    AjsFuncKind   fn_kind;
    bool          is_constructor;
    bool          is_derived_ctor;
    bool          strict;
    AjsNativeFn   native_fn;
    void         *native_userdata;
    int32_t       native_magic; /* Optional helper dispatch ID for built-ins */
    AjsBytecode  *bc;
    AjsEnv       *closure_env;
    AjsValue      home_object;  /* [[HomeObject]] for super.prop */
    AjsValue      inst_init_fn; /* Class <instance_init> closure */
    AjsString    *source_text;  /* For Function.prototype.toString */
    /* Bound function fields */
    AjsValue      bound_target;
    AjsValue      bound_this;
    int           bound_argc;
    AjsValue     *bound_argv;
    /* Module link context (for import.meta / dynamic import) */
    AjsModuleRecord *owner_module;
} AjsFuncData;

typedef struct AjsArrayData {
    uint32_t  length;
    bool      length_writable;
    uint32_t  dense_cap;
    uint32_t  dense_len;
    AjsValue *dense;            /* Fast dense prefix (AJS_VAL_EMPTY for holes) */
} AjsArrayData;

typedef struct AjsArgsData {
    bool      mapped;
    AjsEnv   *env;
    uint32_t  param_count;
    uint16_t *param_slots;      /* UINT16_MAX if unmapped slot */
} AjsArgsData;

typedef struct AjsErrorData {
    char     error_name[32];
    char     url[512];
    uint32_t line;
    uint32_t col;
    char    *stack_str;
} AjsErrorData;

typedef enum AjsPromiseState {
    AJS_PROMISE_PENDING   = 0,
    AJS_PROMISE_FULFILLED = 1,
    AJS_PROMISE_REJECTED  = 2
} AjsPromiseState;

typedef struct AjsPromiseReaction {
    AjsValue                   on_fulfilled;
    AjsValue                   on_rejected;
    AjsValue                   resolve_fn;
    AjsValue                   reject_fn;
    AjsValue                   child_promise;
    struct AjsPromiseReaction *next;
} AjsPromiseReaction;

typedef struct AjsPromiseData {
    AjsPromiseState     state;
    bool                is_handled;
    AjsValue            result;
    AjsPromiseReaction *reactions_head;
    AjsPromiseReaction *reactions_tail;
} AjsPromiseData;

typedef struct AjsMapEntry {
    AjsValue key;               /* AJS_VAL_EMPTY when deleted (preserves live iterator!) */
    AjsValue val;
    uint32_t hash;
    int32_t  bucket_next;
} AjsMapEntry;

typedef struct AjsCollectionData {
    bool         is_set;
    bool         is_weak;
    uint32_t     live_count;
    uint32_t     entries_len;
    uint32_t     entries_cap;
    AjsMapEntry *entries;
    uint32_t     bucket_cap;    /* Power of 2 */
    int32_t     *buckets;       /* -1 for empty */
} AjsCollectionData;

typedef struct AjsFinRegEntry {
    AjsValue target;
    AjsValue held_value;
    AjsValue unregister_token;
    bool     alive;
} AjsFinRegEntry;

typedef struct AjsFinRegData {
    AjsValue        cleanup_cb;
    uint32_t        count;
    uint32_t        cap;
    AjsFinRegEntry *entries;
} AjsFinRegData;

typedef enum AjsIterKind {
    AJS_ITER_ARRAY_KEYS = 0,
    AJS_ITER_ARRAY_VALUES,
    AJS_ITER_ARRAY_ENTRIES,
    AJS_ITER_STRING,
    AJS_ITER_MAP_KEYS,
    AJS_ITER_MAP_VALUES,
    AJS_ITER_MAP_ENTRIES,
    AJS_ITER_SET_VALUES,
    AJS_ITER_SET_ENTRIES,
    AJS_ITER_REGEXP_STRING,
    AJS_ITER_HELPER_MAP,
    AJS_ITER_HELPER_FILTER,
    AJS_ITER_HELPER_TAKE,
    AJS_ITER_HELPER_DROP,
    AJS_ITER_HELPER_FLATMAP,
    AJS_ITER_WRAP_VALID
} AjsIterKind;

typedef struct AjsIterData {
    AjsIterKind kind;
    bool        done;
    bool        started;
    uint32_t    index;
    uint64_t    counter;
    uint64_t    limit;
    AjsValue    target;
    AjsValue    next_method;
    AjsValue    helper_fn;
    AjsValue    inner_iter;
    AjsValue    inner_next;
    bool        global;
    bool        unicode;
} AjsIterData;

typedef enum AjsGenState {
    AJS_GEN_SUSPENDED_START = 0,
    AJS_GEN_SUSPENDED_YIELD = 1,
    AJS_GEN_EXECUTING       = 2,
    AJS_GEN_AWAITING_RETURN = 3,
    AJS_GEN_COMPLETED       = 4
} AjsGenState;

typedef struct AjsAsyncGenReq {
    int                    resume_mode; /* 0=next, 1=return, 2=throw */
    AjsValue               arg;
    AjsValue               promise;
    AjsValue               resolve_fn;
    AjsValue               reject_fn;
    struct AjsAsyncGenReq *next;
} AjsAsyncGenReq;

typedef struct AjsGenData {
    AjsGenState     state;
    bool            is_async_fn;
    bool            is_async_gen;
    AjsExecFrame   *frame;
    AjsValue        async_promise;
    AjsValue        async_resolve;
    AjsValue        async_reject;
    AjsAsyncGenReq *req_head;
    AjsAsyncGenReq *req_tail;
} AjsGenData;

typedef struct AjsBufferData {
    uint8_t *bytes;
    size_t   byte_len;
    size_t   max_byte_len;
    bool     resizable;
    bool     detached;
    bool     shared;
} AjsBufferData;

typedef struct AjsViewData {
    AjsTypedArrayKind kind;     /* AJS_TA_NONE for DataView */
    AjsObject        *buffer;
    size_t            byte_offset;
    size_t            elem_or_byte_len;
    bool              auto_length;
} AjsViewData;

typedef struct AjsProxyData {
    AjsValue target;            /* AJS_VAL_NULL when revoked */
    AjsValue handler;           /* AJS_VAL_NULL when revoked */
    bool     is_callable;
    bool     is_constructor;
} AjsProxyData;

typedef struct AjsModuleNsData {
    AjsModuleRecord *module;
    uint32_t         export_count;
    AjsString      **export_names; /* Sorted lexicographically by UTF-16 code units */
} AjsModuleNsData;

typedef enum AjsIntlKind {
    AJS_INTL_LOCALE = 1,
    AJS_INTL_COLLATOR,
    AJS_INTL_NUMBER_FORMAT,
    AJS_INTL_DATETIME_FORMAT,
    AJS_INTL_PLURAL_RULES,
    AJS_INTL_RELATIVE_TIME_FORMAT,
    AJS_INTL_SEGMENTER,
    AJS_INTL_SEGMENTS,
    AJS_INTL_DISPLAY_NAMES,
    AJS_INTL_LIST_FORMAT
} AjsIntlKind;

typedef struct AjsIntlData {
    AjsIntlKind kind;
    char        locale[64];
    char        language[16];
    char        script[16];
    char        region[16];
    char        calendar[32];
    char        collation[32];
    char        numbering_system[32];
    char        hour_cycle[16];
    char        case_first[16];
    char        style[32];
    char        currency[16];
    char        currency_display[16];
    char        unit[32];
    char        unit_display[16];
    char        notation[16];
    char        sign_display[16];
    char        sensitivity[16];
    char        time_zone[64];
    char        date_style[16];
    char        time_style[16];
    char        weekday_opt[16];
    char        year_opt[16];
    char        month_opt[16];
    char        day_opt[16];
    char        hour_opt[16];
    char        minute_opt[16];
    char        second_opt[16];
    char        plural_type[16];
    char        granularity[16];
    char        disp_type[24];
    char        list_type[24];
    char        fallback[16];
    int32_t     tz_offset_min;
    int32_t     min_int_digits;
    int32_t     min_frac_digits;
    int32_t     max_frac_digits;
    int32_t     min_sig_digits;
    int32_t     max_sig_digits;
    bool        numeric;
    bool        has_numeric;
    bool        ignore_punct;
    bool        use_grouping;
    bool        hour12;
    bool        has_hour12;
    AjsValue    bound_fn;
    AjsValue    seg_input;
    AjsValue    segmenter_obj;
} AjsIntlData;

struct AjsObject {
    AjsGcHeader     gc;
    AjsObjKind      kind;
    bool            extensible;
    AjsRealm       *realm;
    AjsValue        proto;      /* AJS_VAL_NULL or AjsObject */

    /* Own property storage + adaptive open-addressing hash index */
    uint32_t        prop_count; /* Includes tombstones */
    uint32_t        live_props;
    uint32_t        prop_cap;
    uint32_t        next_seq;
    AjsPropSlot    *props;
    uint32_t        hash_cap;   /* 0 or power-of-2 >= 16 */
    int32_t        *hash_tab;   /* -1 empty, -2 tombstone, >=0 props index */

    /* Private fields/methods (#name) */
    uint32_t        priv_count;
    uint32_t        priv_cap;
    AjsPrivateSlot *privs;

    /* Host C embedding opaque data (ajs_set_opaque / ajs_get_opaque) */
    uint32_t        opaque_tag;
    void           *opaque_ptr;
    AjsFinalizerFn  finalizer;
    AjsGcMarkFn     gc_mark;

    union {
        AjsValue          prim_val;     /* Boxed Boolean/Number/String/Symbol/BigInt, WeakRef target */
        double            date_ms;      /* Date epoch ms */
        AjsArrayData      arr;
        AjsFuncData       fn;
        AjsArgsData       args;
        AjsErrorData      err;
        AjsRegExp        *regexp;
        AjsPromiseData    promise;
        AjsCollectionData col;
        AjsFinRegData     finreg;
        AjsIterData       iter;
        AjsGenData        gen;
        AjsBufferData     buf;
        AjsViewData       view;
        AjsProxyData      proxy;
        AjsModuleNsData   mod_ns;
        AjsIntlData      *intl;
    } u;
};

/* ============================================================================
 * 5. Lexical Environments, Bytecode & Suspended Execution Frames
 * ============================================================================ */

#define AJS_SLOT_CONST       (1u << 0)
#define AJS_SLOT_FUNC_NAME   (1u << 1) /* Immutable named function expression binding */
#define AJS_SLOT_IMPORT_LINK (1u << 2) /* Indirect ES module live binding */

typedef struct AjsImportLink {
    AjsModuleRecord *target_mod;
    uint32_t         target_slot;   /* UINT32_MAX for import * as ns */
    bool             is_namespace;
} AjsImportLink;

struct AjsEnv {
    AjsGcHeader    gc;
    AjsEnv        *parent;
    AjsObject     *with_obj;        /* Non-NULL for sloppy `with (obj)` or global obj */
    uint32_t       count;
    AjsValue      *slots;
    uint8_t       *flags;
    AjsString    **names;           /* Optional slot names for direct eval / globals */
    AjsImportLink *import_links;    /* Non-NULL only for module environments with imports */
};

typedef enum AjsOpcode {
    OP_NOP = 0,
    OP_UNDEF,
    OP_NULL,
    OP_TRUE,
    OP_FALSE,
    OP_TDZ,
    OP_EMPTY,
    OP_ZERO,
    OP_ONE,
    OP_INT32,           /* i32 */
    OP_CONST,           /* u32 idx */
    OP_THIS,
    OP_NEW_TARGET,
    OP_ARGUMENTS,
    OP_IMPORT_META,
    OP_ACTIVE_FUNC,

    OP_POP,
    OP_DUP,
    OP_DUP2,
    OP_SWAP,
    OP_ROT3,            /* [a, b, c] -> [c, a, b] */
    OP_ROT3L,           /* [a, b, c] -> [b, c, a] */
    OP_NIP,             /* [a, b] -> [b] */

    OP_GET_LEX,         /* u16 depth, u16 slot */
    OP_SET_LEX,         /* u16 depth, u16 slot, u8 strict */
    OP_INIT_LEX,        /* u16 depth, u16 slot */
    OP_CHECK_LEX_TDZ,   /* u16 depth, u16 slot */
    OP_GET_GLOBAL,      /* u32 name_idx, u8 typeof_mode */
    OP_SET_GLOBAL,      /* u32 name_idx, u8 strict */
    OP_CHECK_GLOBAL_REF,/* u32 name_idx — throws ReferenceError if binding missing prior to RHS eval */
    OP_DEF_GLOBAL_VAR,  /* u32 name_idx, u8 is_func */
    OP_DEF_GLOBAL_LEX,  /* u32 name_idx, u8 is_const */
    OP_INIT_GLOBAL_LEX, /* u32 name_idx */
    OP_DELETE_NAME,     /* u32 name_idx, u8 strict */
    OP_GET_DYN_NAME,    /* u32 name_idx, u8 typeof_mode */
    OP_SET_DYN_NAME,    /* u32 name_idx, u8 strict */

    OP_PUSH_ENV,        /* u16 count, u16 names_const_idx (0xFFFF = none) */
    OP_POP_ENV,
    OP_CLONE_ENV,       /* u16 count */
    OP_PUSH_WITH,

    OP_NEW_OBJECT,
    OP_NEW_OBJECT_PROTO,/* [proto] -> [obj] */
    OP_NEW_ARRAY,       /* u32 cap */
    OP_ARRAY_PUSH,      /* [arr, val] -> [arr] */
    OP_ARRAY_SPREAD,    /* [arr, iterable] -> [arr] */
    OP_OBJECT_SPREAD,   /* [obj, src] -> [obj] */
    OP_OBJECT_REST,     /* [src, excl_arr] -> [rest_obj] */

    OP_GET_PROP,        /* u32 name_idx */
    OP_GET_ELEM,        /* [obj, key] -> [val] */
    OP_SET_PROP,        /* u32 name_idx, u8 strict: [obj, val] -> [val] */
    OP_SET_ELEM,        /* u8 strict: [obj, key, val] -> [val] */
    OP_DEFINE_PROP,     /* u32 name_idx, u8 kind (0=data,1=get,2=set,3=method,4=static_mask): [target, val] -> [target] */
    OP_DEFINE_ELEM,     /* u8 kind: [target, key, val] -> [target] */
    OP_SET_PROTO_LIT,   /* [obj, proto] -> [obj] (__proto__: expr in object literal) */
    OP_DELETE_PROP,     /* u32 name_idx, u8 strict: [obj] -> [bool] */
    OP_DELETE_ELEM,     /* u8 strict: [obj, key] -> [bool] */

    OP_GET_SUPER_PROP,  /* u16 depth, u16 fn_slot, u32 name_idx: [this] -> [val] */
    OP_GET_SUPER_ELEM,  /* u16 depth, u16 fn_slot: [this, key] -> [val] */
    OP_SET_SUPER_PROP,  /* u16 depth, u16 fn_slot, u32 name_idx, u8 strict: [this, val] -> [val] */
    OP_SET_SUPER_ELEM,  /* u16 depth, u16 fn_slot, u8 strict: [this, key, val] -> [val] */

    OP_NEW_PRIV_SYM,    /* u32 desc_name_idx: [] -> [priv_sym] */
    OP_DEF_PRIVATE,     /* u16 depth, u16 slot, u8 kind: [target, val] -> [target] */
    OP_GET_PRIVATE,     /* u16 depth, u16 slot: [target] -> [val] */
    OP_SET_PRIVATE,     /* u16 depth, u16 slot: [target, val] -> [val] */
    OP_IN_PRIVATE,      /* u16 depth, u16 slot: [target] -> [bool] */

    OP_CLOSURE,         /* u32 bc_idx */
    OP_SET_FN_NAME,     /* u8 prefix (0=none,1=get,2=set): [fn, key] -> [fn] */
    OP_SET_HOME_OBJ,    /* [home, fn] -> [home, fn] */
    OP_BUILD_CLASS,     /* u32 name_idx, u8 has_super: [super_or_undef, ctor_fn, inst_init_or_undef] -> [ctor, proto] */
    OP_SUPER_CALL,      /* u16 depth, u16 this_slot, u16 nt_slot, u16 fn_slot: [args_array] -> [this] */
    OP_RUN_INST_INIT,   /* [] -> [] */
    OP_CHECK_THIS_TDZ,  /* [this_val] -> [this_val] */
    OP_REGEXP,          /* u32 pat_idx, u32 flags_idx */
    OP_TEMPLATE_OBJ,    /* u32 tpl_idx */

    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_EXP,
    OP_NEG,
    OP_POS,
    OP_TO_NUMERIC,
    OP_INC_NUM,         /* Number -> +1, BigInt -> +1n */
    OP_DEC_NUM,         /* Number -> -1, BigInt -> -1n */
    OP_BIT_AND,
    OP_BIT_OR,
    OP_BIT_XOR,
    OP_BIT_NOT,
    OP_SHL,
    OP_SHR,
    OP_USHR,
    OP_NOT,
    OP_VOID,
    OP_TYPEOF,
    OP_EQ,
    OP_NEQ,
    OP_STRICT_EQ,
    OP_STRICT_NEQ,
    OP_LT,
    OP_LTE,
    OP_GT,
    OP_GTE,
    OP_IN,
    OP_INSTANCEOF,
    OP_TO_PROP_KEY,
    OP_REQ_OBJ_COERCIBLE,

    OP_JMP,             /* u32 pc */
    OP_JMP_FALSE,       /* u32 pc */
    OP_JMP_TRUE,        /* u32 pc */
    OP_JMP_NULLISH,     /* u32 pc (pops!) */
    OP_JMP_NOT_NULLISH, /* u32 pc (pops!) */
    OP_JMP_UNDEF,       /* u32 pc (pops!) */

    OP_CALL,            /* u16 argc: [fn, arg0..argN-1] -> [res] */
    OP_CALL_METHOD,     /* u16 argc: [recv, fn, arg0..argN-1] -> [res] */
    OP_CALL_SPREAD,     /* [recv, fn, args_arr] -> [res] */
    OP_CALL_EVAL,       /* u16 argc, u8 strict: [fn, arg0..argN-1] -> [res] */
    OP_NEW,             /* u16 argc: [ctor, arg0..argN-1] -> [res] */
    OP_NEW_SPREAD,      /* [ctor, args_arr] -> [res] */
    OP_DYNAMIC_IMPORT,  /* [specifier] -> [promise] */

    OP_RETURN,
    OP_RETURN_DERIVED,  /* u16 this_slot */
    OP_THROW,

    OP_TRY_PUSH,        /* u32 catch_pc, u32 finally_pc */
    OP_TRY_POP,
    OP_ENTER_FINALLY,
    OP_UNWIND_JUMP,     /* u16 target_try_sp, u16 target_env_depth, u32 target_pc */
    OP_UNWIND_RETURN,   /* [ret_val] -> unwinds all finally blocks then returns! */
    OP_FINALLY_END,

    OP_GET_ITER,        /* u8 is_async: [iterable] -> [iter, next_fn] */
    OP_ITER_NEXT,       /* [iter, next_fn] -> [iter, next_fn, res_obj] */
    OP_ITER_UNPACK,     /* [res_obj] -> [value, done_bool] */
    OP_ITER_CLOSE,      /* [iter] -> [] */
    OP_FOR_IN_INIT,     /* [target] -> [iter, next_fn] */
    OP_REST_FROM_ITER,  /* [iter, next_fn] -> [arr] */

    OP_YIELD,           /* [val] -> [resumed_val] */
    OP_YIELD_STAR,      /* u8 is_async: [iterable] -> [final_val] */
    OP_AWAIT            /* [val] -> [awaited_val] */
} AjsOpcode;

typedef struct AjsLineSpan {
    uint32_t bc_offset;
    uint32_t line;
    uint32_t col;
} AjsLineSpan;

typedef struct AjsTplSiteDesc {
    uint32_t    id;
    uint32_t    count;
    AjsString **cooked;         /* NULL entry when invalid escape in tagged template! */
    AjsString **raw;
} AjsTplSiteDesc;

typedef struct AjsEnvNamesDesc {
    uint16_t    count;
    uint8_t    *slot_flags;
    AjsString **names;
} AjsEnvNamesDesc;

#define AJS_SLOT_INIT_TDZ    (1u << 7)

struct AjsBytecode {
    AjsGcHeader      gc;
    AjsFuncKind      fn_kind;
    bool             strict;
    bool             is_derived_ctor;
    bool             has_simple_params;
    bool             has_rest_param;
    bool             needs_arguments;
    bool             is_module;
    bool             is_eval;
    uint16_t         arity;             /* Expected Function.length */
    uint16_t         param_count;
    uint16_t         env_slot_count;    /* Top-level frame lexical/var slots */
    uint16_t         this_slot;         /* 0xFFFF if none */
    uint16_t         new_target_slot;   /* 0xFFFF if none */
    uint16_t         arguments_slot;    /* 0xFFFF if none */
    uint16_t         active_fn_slot;    /* 0xFFFF if none */
    uint16_t        *mapped_param_slots;/* For sloppy mapped arguments */
    uint8_t         *env_slot_flags;    /* Initial slot flags + AJS_SLOT_INIT_TDZ */
    AjsString      **env_slot_names;

    AjsString       *name;
    AjsString       *source_url;
    AjsString       *source_text;

    uint32_t         code_len;
    uint32_t         code_cap;
    uint8_t         *code;

    uint32_t         const_count;
    uint32_t         const_cap;
    AjsValue        *consts;

    uint32_t         tpl_count;
    AjsTplSiteDesc  *tpls;

    uint32_t         env_desc_count;
    AjsEnvNamesDesc *env_descs;

    uint32_t         line_count;
    uint32_t         line_cap;
    AjsLineSpan     *lines;
};

#define AJS_MAX_TRY_DEPTH 32

typedef enum AjsCompletionKind {
    AJS_COMP_NORMAL = 0,
    AJS_COMP_THROW  = 1,
    AJS_COMP_RETURN = 2,
    AJS_COMP_JUMP   = 3
} AjsCompletionKind;

typedef struct AjsTryState {
    uint32_t          catch_pc;         /* UINT32_MAX if none */
    uint32_t          finally_pc;       /* UINT32_MAX if none */
    uint32_t          saved_sp;
    uint16_t          saved_env_depth;
    bool              in_finally;
    AjsCompletionKind pending_kind;
    AjsValue          pending_val;
    uint32_t          pending_pc;
    uint16_t          pending_try_sp;
    uint16_t          pending_env_depth;
} AjsTryState;

struct AjsExecFrame {
    AjsGcHeader       gc;
    AjsRealm         *realm;
    AjsObject        *func_obj;         /* May be NULL for top-level script/module */
    AjsBytecode      *bc;
    AjsEnv           *base_env;
    AjsEnv           *env;
    uint16_t          env_depth;
    AjsValue          this_val;
    AjsValue          new_target;
    AjsObject        *args_obj;
    AjsModuleRecord  *module;

    uint32_t          pc;
    uint32_t          sp;
    uint32_t          stack_cap;
    AjsValue         *stack;

    uint16_t          try_sp;
    AjsTryState       try_stack[AJS_MAX_TRY_DEPTH];

    /* Active yield* delegation state when suspended inside OP_YIELD_STAR */
    bool              in_yield_star;
    AjsValue          ys_iter;
    AjsValue          ys_next;
    bool              ys_awaiting_step;
    int               ys_step_mode;     /* 0=next, 1=return, 2=throw */
};

/* ============================================================================
 * 6. ECMAScript Source Text Module Record
 * ============================================================================ */

typedef enum AjsModStatus {
    AJS_MOD_UNLINKED = 0,
    AJS_MOD_LINKING,
    AJS_MOD_LINKED,
    AJS_MOD_EVALUATING,
    AJS_MOD_EVALUATING_ASYNC,
    AJS_MOD_EVALUATED
} AjsModStatus;

typedef struct AjsImportEntry {
    AjsString *module_specifier;
    AjsString *import_name;     /* NULL for `import "mod"`, "*" for `import * as ns` */
    AjsString *local_name;
    uint16_t   local_slot;
    uint32_t   line;
    uint32_t   col;
} AjsImportEntry;

typedef struct AjsExportEntry {
    AjsString *export_name;     /* NULL for `export * from "mod"` */
    AjsString *module_specifier;/* NULL for local export */
    AjsString *import_name;     /* "*" for `export * as ns from "mod"` or `export * from "mod"` */
    AjsString *local_name;      /* Local binding name when module_specifier == NULL */
    uint16_t   local_slot;
    uint32_t   line;
    uint32_t   col;
} AjsExportEntry;

typedef struct AjsFuncInitEntry {
    uint16_t     slot;
    AjsBytecode *fn_bc;
} AjsFuncInitEntry;

struct AjsModuleRecord {
    AjsGcHeader       gc;
    AjsRealm         *realm;
    AjsModStatus      status;
    char             *url;
    AjsBytecode      *bc;
    AjsEnv           *env;
    AjsValue          namespace_obj;
    AjsValue          import_meta_obj;

    /* Requested module specifiers & linked child modules */
    uint32_t          req_count;
    AjsString       **requested_specifiers;
    AjsModuleRecord **loaded_modules;

    uint32_t          import_count;
    AjsImportEntry   *imports;

    uint32_t          export_count;
    AjsExportEntry   *exports;

    uint32_t          hoisted_fn_count;
    AjsFuncInitEntry *hoisted_fns;

    /* Tarjan SCC & Top-Level Await (ECMA-262 §16.2.1.6) */
    bool              has_tla;
    bool              async_evaluation;
    uint64_t          async_eval_order;
    uint32_t          dfs_index;
    uint32_t          dfs_ancestor_index;
    uint32_t          pending_async_deps;
    AjsModuleRecord  *cycle_root;
    uint32_t          async_parent_count;
    uint32_t          async_parent_cap;
    AjsModuleRecord **async_parents;

    AjsValue          top_capability_promise;
    AjsValue          top_resolve_fn;
    AjsValue          top_reject_fn;

    bool              has_eval_error;
    AjsValue          eval_error;
};

/* ============================================================================
 * 7. Realm & VM Structures
 * ============================================================================ */

typedef struct AjsTplCacheEntry {
    const AjsBytecode *bc;
    uint32_t           tpl_id;
    AjsValue           tpl_array;
} AjsTplCacheEntry;

typedef enum AjsErrKind {
    AJS_ERR_KIND_ERROR = 0,
    AJS_ERR_KIND_EVAL,
    AJS_ERR_KIND_RANGE,
    AJS_ERR_KIND_REFERENCE,
    AJS_ERR_KIND_SYNTAX,
    AJS_ERR_KIND_TYPE,
    AJS_ERR_KIND_URI,
    AJS_ERR_KIND_AGGREGATE,
    AJS_ERR_KIND_SUPPRESSED,
    AJS_ERR_KIND_COUNT
} AjsErrKind;

struct AjsRealm {
    AjsVm            *vm;
    AjsRealm         *next;
    AjsValue          global_obj;
    AjsEnv           *global_lex_env;

    /* Core Prototypes & Constructors */
    AjsValue          object_proto, object_ctor;
    AjsValue          function_proto, function_ctor;
    AjsValue          array_proto, array_ctor, array_iter_proto;
    AjsValue          string_proto, string_ctor, string_iter_proto;
    AjsValue          boolean_proto, boolean_ctor;
    AjsValue          number_proto, number_ctor;
    AjsValue          bigint_proto, bigint_ctor;
    AjsValue          symbol_proto, symbol_ctor;
    AjsValue          regexp_proto, regexp_ctor, regexp_iter_proto;
    AjsValue          date_proto, date_ctor;
    AjsValue          promise_proto, promise_ctor;
    AjsValue          map_proto, map_ctor, map_iter_proto;
    AjsValue          set_proto, set_ctor, set_iter_proto;
    AjsValue          weakmap_proto, weakmap_ctor;
    AjsValue          weakset_proto, weakset_ctor;
    AjsValue          weakref_proto, weakref_ctor;
    AjsValue          finreg_proto, finreg_ctor;
    AjsValue          iterator_proto, iterator_ctor, iter_helper_proto, async_iterator_proto;
    AjsValue          gen_func_proto, gen_func_ctor, gen_proto;
    AjsValue          async_func_proto, async_func_ctor;
    AjsValue          async_gen_func_proto, async_gen_func_ctor, async_gen_proto;
    AjsValue          error_proto[AJS_ERR_KIND_COUNT];
    AjsValue          error_ctor[AJS_ERR_KIND_COUNT];
    AjsValue          ab_proto, ab_ctor;
    AjsValue          sab_proto, sab_ctor;
    AjsValue          dv_proto, dv_ctor;
    AjsValue          ta_proto, ta_ctor;
    AjsValue          ta_sub_proto[12];
    AjsValue          ta_sub_ctor[12];
    AjsValue          intl_locale_proto, intl_collator_proto, intl_numfmt_proto;
    AjsValue          intl_dtfmt_proto, intl_plural_proto, intl_reltime_proto;
    AjsValue          intl_segmenter_proto, intl_segments_proto, intl_dispnames_proto, intl_listfmt_proto;

    AjsValue          intrinsic_eval;
    AjsValue          throw_type_error_fn;
    AjsValue          array_proto_values_fn;

    /* Tagged template call-site cache */
    uint32_t          tpl_cache_count;
    uint32_t          tpl_cache_cap;
    AjsTplCacheEntry *tpl_cache;
};

typedef struct AjsJobNode {
    AjsValue           fn;
    int                argc;
    AjsValue           argv[3];
    AjsRealm          *realm;
    struct AjsJobNode *next;
} AjsJobNode;

typedef struct AjsSymRegEntry {
    AjsString *key;
    AjsSymbol *sym;
} AjsSymRegEntry;

typedef struct AjsCallStackEntry {
    AjsExecFrame *frame;
    AjsObject    *native_fn_obj;
} AjsCallStackEntry;

struct AjsVm {
    AjsVmConfig        cfg;
    void              *userdata;

    /* Realms */
    AjsRealm          *main_realm;
    AjsRealm          *realms;
    AjsRealm          *active_realm;

    /* GC state */
    AjsGcHeader       *gc_head;
    size_t             gc_bytes;
    size_t             gc_threshold;
    uint32_t           gc_suppress;
    bool               gc_running;
    bool               destroying;
    AjsValue           oom_error;

    /* Explicit AjsHandleScope stack */
    AjsHandleScope    *current_handle_scope;
    size_t             handle_top;
    size_t             handle_cap;
    AjsValue          *handle_stack;

    /* Internal temporary GC root stack */
    size_t             temp_top;
    size_t             temp_cap;
    AjsValue          *temp_roots;

    /* Persistent pin slots (ajs_pin_value) */
    uint32_t           pins_cap;
    AjsValue          *pins;
    bool              *pin_used;

    /* String intern table (weakly swept during GC) */
    uint32_t           intern_count;
    uint32_t           intern_cap;
    AjsString        **intern_tab;

    /* Common cached strings */
    AjsString         *str_empty;
    AjsString         *str_length;
    AjsString         *str_prototype;
    AjsString         *str_constructor;
    AjsString         *str_name;
    AjsString         *str_message;
    AjsString         *str_stack;
    AjsString         *str_cause;
    AjsString         *str_errors;
    AjsString         *str_value;
    AjsString         *str_done;
    AjsString         *str_next;
    AjsString         *str_return;
    AjsString         *str_throw;
    AjsString         *str_then;
    AjsString         *str_toJSON;
    AjsString         *str_toString;
    AjsString         *str_valueOf;
    AjsString         *str_default;
    AjsString         *str_raw;

    /* Symbols & global Symbol.for registry */
    uint32_t           next_symbol_id;
    AjsSymbol         *well_known_syms[AJS_SYM_COUNT];
    uint32_t           sym_reg_count;
    uint32_t           sym_reg_cap;
    AjsSymRegEntry    *sym_reg;

    /* Active call stack */
    size_t             call_depth;
    size_t             call_cap;
    AjsCallStackEntry *call_stack;
    uint64_t           op_counter;

    /* Exception state */
    bool               has_exception;
    AjsErrorPhase      exc_phase;
    AjsValue           exception;
    AjsErrorInfo       last_err_info;

    /* Microtask job queue */
    AjsJobNode        *job_head;
    AjsJobNode        *job_tail;
    bool               draining_jobs;

    /* ES Module cache & TLA ordering */
    uint32_t           module_count;
    uint32_t           module_cap;
    AjsModuleRecord  **modules;
    uint32_t           tarjan_index;
    uint64_t           next_async_eval_order;

    /* Deterministic PRNG state for Math.random() */
    uint64_t           rng_state[2];
};

/* Temporary internal GC root guard helpers */
static inline size_t ajs_temp_root_save(AjsVm *vm) {
    return vm->temp_top;
}

static inline void ajs_temp_root_restore(AjsVm *vm, size_t saved) {
    vm->temp_top = saved;
}

void ajs_temp_root_push(AjsVm *vm, AjsValue v);
void ajs_handle_record(AjsVm *vm, AjsValue v);

/* ============================================================================
 * 8. Internal Cross-Module Function Prototypes
 * ============================================================================ */

/* ajs_gc.c */
void *ajs_gc_alloc(AjsVm *vm, size_t size, AjsGcKind kind);
void  ajs_gc_free_header(AjsVm *vm, AjsGcHeader *hdr);
void  ajs_gc_mark_value(AjsVm *vm, AjsValue v);
void  ajs_gc_mark_ptr(AjsVm *vm, void *ptr);
void  ajs_gc_free_all(AjsVm *vm);

/* ajs_utf16.c */
uint32_t   ajs_hash_utf16(const uint16_t *units, uint32_t len);
AjsString *ajs_str_from_utf8(AjsVm *vm, const char *utf8, size_t byte_len, bool intern);
AjsString *ajs_str_from_cstr(AjsVm *vm, const char *cstr, bool intern);
AjsString *ajs_str_from_utf16(AjsVm *vm, const uint16_t *units, size_t unit_len, bool intern);
AjsString *ajs_str_intern(AjsVm *vm, AjsString *str);
AjsString *ajs_str_concat(AjsVm *vm, const AjsString *a, const AjsString *b);
int        ajs_str_cmp_utf16(const AjsString *a, const AjsString *b);
bool       ajs_str_eq(const AjsString *a, const AjsString *b);
bool       ajs_str_eq_cstr(const AjsString *a, const char *cstr);
uint32_t   ajs_utf16_code_point_at(const uint16_t *units, uint32_t len, uint32_t idx, uint32_t *out_adv);
size_t     ajs_code_point_to_utf16(uint32_t cp, uint16_t out_units[2]);
size_t     ajs_code_point_to_utf8(uint32_t cp, char out_utf8[4]);
uint32_t   ajs_utf8_decode_one(const char *s, size_t len, size_t *out_adv);
bool       ajs_unicode_is_whitespace(uint32_t cp);
bool       ajs_unicode_is_line_terminator(uint32_t cp);
bool       ajs_unicode_is_id_start(uint32_t cp);
bool       ajs_unicode_is_id_continue(uint32_t cp);
uint32_t   ajs_unicode_to_lower(uint32_t cp);
uint32_t   ajs_unicode_to_upper(uint32_t cp);
uint32_t   ajs_unicode_fold_case(uint32_t cp, bool unicode_mode);
bool       ajs_unicode_match_prop(const char *prop_name, const char *prop_val_or_null, uint32_t cp);
AjsString *ajs_number_to_str(AjsVm *vm, double d, int radix);
double     ajs_str_to_number(const AjsString *s);
double     ajs_parse_int_str(const uint16_t *units, uint32_t len, int32_t radix);
double     ajs_parse_float_str(const uint16_t *units, uint32_t len);
int32_t    ajs_double_to_int32(double d);
uint32_t   ajs_double_to_uint32(double d);
double     ajs_double_to_integer_or_inf(double d);

/* ajs_object.c */
AjsObject *ajs_obj_new(AjsVm *vm, AjsRealm *realm, AjsObjKind kind, AjsValue proto);
AjsValue   ajs_to_property_key(AjsVm *vm, AjsValue key_val);
AjsValue   ajs_to_primitive(AjsVm *vm, AjsValue v, int hint); /* 0=default, 1=string, 2=number */
AjsString *ajs_to_js_string(AjsVm *vm, AjsValue v);
bool       ajs_to_numeric(AjsVm *vm, AjsValue v, AjsValue *out_num_or_bi);
bool       ajs_to_length(AjsVm *vm, AjsValue v, uint64_t *out_len);
bool       ajs_to_index(AjsVm *vm, AjsValue v, size_t *out_idx);
AjsObject *ajs_to_object(AjsVm *vm, AjsValue v);
bool       ajs_same_value_zero(AjsValue a, AjsValue b);
bool       ajs_strict_eq(AjsValue a, AjsValue b);
bool       ajs_abstract_eq(AjsVm *vm, AjsValue a, AjsValue b, bool *out_res);
int        ajs_abstract_rel_cmp(AjsVm *vm, AjsValue a, AjsValue b, bool left_first); /* -1=undef,0=false,1=true */

bool       ajs_obj_get_own_desc(AjsVm *vm, AjsObject *obj, AjsValue key, AjsPropDesc *out_desc, bool *out_found);
bool       ajs_obj_define_own_prop(AjsVm *vm, AjsObject *obj, AjsValue key, const AjsPropDesc *desc, bool throw_on_err);
bool       ajs_obj_has_prop(AjsVm *vm, AjsObject *obj, AjsValue key, bool *out_has);
AjsValue   ajs_obj_get(AjsVm *vm, AjsValue target_val, AjsValue key, AjsValue receiver);
bool       ajs_obj_set(AjsVm *vm, AjsValue target_val, AjsValue key, AjsValue val, AjsValue receiver, bool strict);
bool       ajs_obj_delete(AjsVm *vm, AjsObject *obj, AjsValue key, bool strict);
bool       ajs_obj_own_keys(AjsVm *vm, AjsObject *obj, bool enumerable_only, bool include_strings,
                            bool include_symbols, AjsValue **out_keys, uint32_t *out_count);
bool       ajs_obj_get_proto(AjsVm *vm, AjsObject *obj, AjsValue *out_proto);
bool       ajs_obj_set_proto(AjsVm *vm, AjsObject *obj, AjsValue proto, bool throw_on_err);
bool       ajs_obj_is_extensible(AjsVm *vm, AjsObject *obj, bool *out_ext);
bool       ajs_obj_prevent_extensions(AjsVm *vm, AjsObject *obj, bool throw_on_err);
bool       ajs_is_constructor_val(AjsValue v);

bool       ajs_priv_define(AjsVm *vm, AjsObject *obj, AjsSymbol *sym, uint8_t kind, AjsValue val, AjsValue setter);
AjsValue   ajs_priv_get(AjsVm *vm, AjsValue target, AjsSymbol *sym);
bool       ajs_priv_set(AjsVm *vm, AjsValue target, AjsSymbol *sym, AjsValue val);
bool       ajs_priv_has(AjsVm *vm, AjsValue target, AjsSymbol *sym, bool *out_has);

void       ajs_def_native_method(AjsVm *vm, AjsRealm *realm, AjsValue obj, const char *name,
                                 AjsNativeFn fn, int length, uint32_t flags, int32_t magic);
void       ajs_def_native_sym_method(AjsVm *vm, AjsRealm *realm, AjsValue obj, AjsSymbol *sym,
                                     const char *fn_name, AjsNativeFn fn, int length, uint32_t flags);
void       ajs_def_native_accessor(AjsVm *vm, AjsRealm *realm, AjsValue obj, const char *name,
                                   AjsNativeFn getter, AjsNativeFn setter, uint32_t flags);
AjsValue   ajs_make_iter_result(AjsVm *vm, AjsValue value, bool done);

/* ajs_api.c */
AjsValue   ajs_throw_err_kind(AjsVm *vm, AjsErrKind kind, const char *fmt, ...);
void       ajs_capture_stack_trace(AjsVm *vm, AjsObject *err_obj, const char *override_url,
                                   uint32_t override_line, uint32_t override_col);
void       ajs_fill_error_info(AjsVm *vm, AjsValue exc, AjsErrorPhase phase, AjsErrorInfo *out_info);

/* ajs_compiler.c & ajs_parser.c */
AjsBytecode *ajs_compile_script(AjsVm *vm, AjsRealm *realm, const char *utf8_code, size_t code_len,
                                const char *source_url, bool force_strict, bool is_eval,
                                AjsEnv *eval_lex_env, AjsErrorInfo *out_err);
AjsBytecode *ajs_compile_function_dynamic(AjsVm *vm, AjsRealm *realm, const char *params_utf8,
                                          size_t params_len, const char *body_utf8, size_t body_len,
                                          AjsFuncKind fn_kind);
bool         ajs_parse_and_compile_module(AjsVm *vm, AjsRealm *realm, AjsModuleRecord *mod,
                                          const char *utf8_code, size_t code_len, AjsErrorInfo *out_err);

/* ajs_interp.c */
AjsEnv      *ajs_env_new(AjsVm *vm, AjsEnv *parent, uint32_t slot_count, AjsString **names);
AjsObject   *ajs_closure_new(AjsVm *vm, AjsRealm *realm, AjsBytecode *bc, AjsEnv *env, AjsModuleRecord *mod);
AjsExecFrame *ajs_frame_new(AjsVm *vm, AjsRealm *realm, AjsObject *func_obj, AjsBytecode *bc,
                            AjsEnv *env, AjsValue this_val, AjsValue new_target, AjsModuleRecord *mod);
AjsValue     ajs_call_internal(AjsVm *vm, AjsValue fn, AjsValue this_val, int argc, const AjsValue *argv);
AjsValue     ajs_construct_internal(AjsVm *vm, AjsValue ctor, int argc, const AjsValue *argv, AjsValue new_target);
AjsValue     ajs_exec_script_bc(AjsVm *vm, AjsRealm *realm, AjsBytecode *bc, AjsEnv *eval_env, AjsValue eval_this);
AjsValue     ajs_gen_resume(AjsVm *vm, AjsObject *gen_obj, int resume_mode, AjsValue arg);
bool         ajs_exec_module_bc(AjsVm *vm, AjsModuleRecord *mod);
AjsValue     ajs_get_native_new_target(const AjsVm *vm);

/* Built-ins initialization hooks */
void     ajs_realm_init_intrinsics(AjsVm *vm, AjsRealm *realm);
void     ajs_init_string_array_builtins(AjsVm *vm, AjsRealm *realm);
void     ajs_init_collections_builtins(AjsVm *vm, AjsRealm *realm);
void     ajs_init_regexp_builtins(AjsVm *vm, AjsRealm *realm);
void     ajs_init_json_builtins(AjsVm *vm, AjsRealm *realm);
void     ajs_init_promise_builtins(AjsVm *vm, AjsRealm *realm);
void     ajs_init_typedarray_builtins(AjsVm *vm, AjsRealm *realm);
void     ajs_init_intl_builtins(AjsVm *vm, AjsRealm *realm);

/* BigInt operations (ajs_builtins.c) */
AjsBigInt *ajs_bigint_new_i64(AjsVm *vm, int64_t v);
AjsBigInt *ajs_bigint_new_u64(AjsVm *vm, uint64_t u, int sign);
AjsBigInt *ajs_bigint_from_cstr(AjsVm *vm, const char *str, size_t len, bool allow_radix_prefix, bool *out_ok);
AjsString *ajs_bigint_to_str(AjsVm *vm, const AjsBigInt *bi, int radix);
int64_t    ajs_bigint_to_i64_wrap(const AjsBigInt *bi);
uint64_t   ajs_bigint_to_u64_wrap(const AjsBigInt *bi);
int        ajs_bigint_cmp(const AjsBigInt *a, const AjsBigInt *b);
int        ajs_bigint_cmp_double(const AjsBigInt *a, double b); /* -2 if NaN */
bool       ajs_bigint_eq(const AjsBigInt *a, const AjsBigInt *b);
AjsBigInt *ajs_bigint_neg(AjsVm *vm, const AjsBigInt *a);
AjsBigInt *ajs_bigint_not(AjsVm *vm, const AjsBigInt *a);
AjsBigInt *ajs_bigint_add(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b);
AjsBigInt *ajs_bigint_sub(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b);
AjsBigInt *ajs_bigint_mul(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b);
AjsBigInt *ajs_bigint_div(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b);
AjsBigInt *ajs_bigint_mod(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b);
AjsBigInt *ajs_bigint_exp(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b);
AjsBigInt *ajs_bigint_shl(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b);
AjsBigInt *ajs_bigint_shr(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b);
AjsBigInt *ajs_bigint_bitop(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b, char op); /* '&', '|', '^' */

/* RegExp helpers (ajs_regexp.c) */
AjsValue   ajs_regexp_new_obj(AjsVm *vm, AjsRealm *realm, const AjsString *pattern, const AjsString *flags);
bool       ajs_regexp_validate_syntax(AjsVm *vm, const uint16_t *pat, uint32_t pat_len,
                                      const uint16_t *flags, uint32_t flags_len, char *err_buf, size_t err_cap);
void       ajs_regexp_free(AjsRegExp *re);
AjsValue   ajs_regexp_exec_builtin(AjsVm *vm, AjsValue rx_val, AjsString *str);
bool       ajs_is_regexp_like(AjsVm *vm, AjsValue v, bool *out_is_rx);

/* Promise & Microtask helpers (ajs_promise.c) */
AjsValue   ajs_promise_resolve_val(AjsVm *vm, AjsRealm *realm, AjsValue ctor, AjsValue val);
void       ajs_promise_then_internal(AjsVm *vm, AjsObject *prom_obj, AjsValue on_fulfill,
                                     AjsValue on_reject, AjsValue res_fn, AjsValue rej_fn, AjsValue child_prom);
void       ajs_enqueue_job_ex(AjsVm *vm, AjsRealm *realm, AjsValue fn, int argc, const AjsValue *argv);

/* TypedArray helpers (ajs_typedarray.c) */
size_t     ajs_ta_elem_size(AjsTypedArrayKind kind);
bool       ajs_ta_get_index(AjsVm *vm, AjsObject *ta_obj, uint32_t idx, AjsValue *out_val);
bool       ajs_ta_set_index(AjsVm *vm, AjsObject *ta_obj, uint32_t idx, AjsValue val);
bool       ajs_ta_check_valid_len(AjsVm *vm, AjsObject *ta_obj, size_t *out_elem_len, size_t *out_byte_offset);

/* Collections helpers (ajs_collections.c) */
uint32_t   ajs_map_hash_value(AjsValue v);
void       ajs_finreg_schedule_cleanup(AjsVm *vm, AjsObject *finreg_obj, AjsValue held_value);

/* Module dynamic hooks (ajs_module.c) */
AjsValue   ajs_module_dynamic_import(AjsVm *vm, AjsRealm *realm, const char *referrer_url, AjsValue specifier_val);
AjsValue   ajs_module_get_import_meta(AjsVm *vm, AjsModuleRecord *mod);
AjsValue   ajs_module_ns_get(AjsVm *vm, AjsObject *ns_obj, AjsValue key);
bool       ajs_module_ns_has(AjsVm *vm, AjsObject *ns_obj, AjsValue key, bool *out_has);

#endif /* AJS_INTERNAL_H */
