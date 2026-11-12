/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * js/arcr_js_portable.c - Clean-Room Pure C11 Bytecode Compiler, Stack VM & Live DOM/Canvas Bridge
 *
 * Zero external library dependencies (links solely against arcr_base; zero -lm or core symbol deps).
 * Compatible with Windows 2000 SP4 x86 SSE1 through modern x86_64/ARM64 Linux & Windows.
 */
#include "arcr_js.h"
#include "../base/arcr_base.h"
#include "../platform/arcr_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JS_FUEL_BUDGET_PER_CALL 500000u
#define JS_MAX_CALL_DEPTH       64u
#define JS_MAX_STACK_SLOTS      1024u
#define JS_MAX_TRY_DEPTH        32u
#define JS_MAX_LOCALS           128u
#define JS_MAX_STORAGE_ENTRIES  64u
#define JS_MAX_EVENT_LISTENERS  128u
#define JS_PI_CONST             3.14159265358979323846

/* =========================================================================
 * 1. Self-Contained IEEE-754 Math Helpers (Zero -lm Shared Library Risk)
 * ========================================================================= */
static bool js_num_is_nan(double x) {
    return x != x;
}

static bool js_num_is_inf(double x) {
    return !js_num_is_nan(x) && (x - x != 0.0);
}

static double js_make_nan(void) {
    volatile double z = 0.0;
    return z / z;
}

static double js_math_abs(double x) {
    return (x < 0.0) ? -x : x;
}

static double js_math_trunc(double x) {
    if (js_num_is_nan(x) || js_num_is_inf(x) || x >= 4503599627370496.0 || x <= -4503599627370496.0) {
        return x;
    }
    int64_t i = (int64_t)x;
    return (double)i;
}

static double js_math_floor(double x) {
    if (js_num_is_nan(x) || js_num_is_inf(x)) return x;
    double t = js_math_trunc(x);
    return (x < t) ? (t - 1.0) : t;
}

static double js_math_ceil(double x) {
    if (js_num_is_nan(x) || js_num_is_inf(x)) return x;
    double t = js_math_trunc(x);
    return (x > t) ? (t + 1.0) : t;
}

static double js_math_round(double x) {
    if (js_num_is_nan(x) || js_num_is_inf(x)) return x;
    return js_math_floor(x + 0.5);
}

static double js_math_fmod(double x, double y) {
    if (js_num_is_nan(x) || js_num_is_nan(y) || js_num_is_inf(x) || y == 0.0) {
        return js_make_nan();
    }
    if (js_num_is_inf(y)) return x;
    double q = js_math_trunc(x / y);
    return x - q * y;
}

static double js_math_sqrt(double x) {
    if (js_num_is_nan(x) || x < 0.0) return js_make_nan();
    if (x == 0.0 || js_num_is_inf(x)) return x;
    uint64_t bits = 0u;
    memcpy(&bits, &x, sizeof(bits));
    bits = (bits >> 1) + 0x1FF8000000000000ULL;
    double g = 0.0;
    memcpy(&g, &bits, sizeof(g));
    for (int i = 0; i < 6; ++i) {
        g = 0.5 * (g + x / g);
    }
    return g;
}

static double js_math_sin(double x) {
    if (js_num_is_nan(x) || js_num_is_inf(x)) return js_make_nan();
    double two_pi = 2.0 * JS_PI_CONST;
    x = js_math_fmod(x, two_pi);
    if (x > JS_PI_CONST) x -= two_pi;
    else if (x < -JS_PI_CONST) x += two_pi;

    if (x > JS_PI_CONST * 0.5) x = JS_PI_CONST - x;
    else if (x < -JS_PI_CONST * 0.5) x = -JS_PI_CONST - x;

    double x2   = x * x;
    double term = x;
    double sum  = x;
    for (int k = 1; k <= 8; ++k) {
        term *= -x2 / (double)((2 * k) * (2 * k + 1));
        sum  += term;
    }
    return sum;
}

static double js_math_cos(double x) {
    return js_math_sin(x + JS_PI_CONST * 0.5);
}

static double js_math_exp(double x) {
    if (js_num_is_nan(x)) return x;
    if (x > 709.0) return 1e308 * 10.0;
    if (x < -709.0) return 0.0;
    const double ln2 = 0.69314718055994530942;
    int k = (int)js_math_floor(x / ln2 + 0.5);
    double r = x - (double)k * ln2;
    double term = 1.0, sum = 1.0;
    for (int i = 1; i <= 14; ++i) {
        term *= r / (double)i;
        sum  += term;
    }
    double scale = 1.0;
    int ak = (k < 0) ? -k : k;
    double b = 2.0;
    while (ak > 0) {
        if (ak & 1) scale *= b;
        b *= b;
        ak >>= 1;
    }
    return (k < 0) ? (sum / scale) : (sum * scale);
}

static double js_math_ln(double x) {
    if (js_num_is_nan(x) || x < 0.0) return js_make_nan();
    if (x == 0.0) return -1e308 * 10.0;
    uint64_t bits = 0u;
    memcpy(&bits, &x, sizeof(bits));
    int exp2 = (int)((bits >> 52) & 0x7FFu) - 1023;
    bits = (bits & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
    double m = 1.0;
    memcpy(&m, &bits, sizeof(m));
    if (m > 1.41421356237) { m *= 0.5; ++exp2; }
    double y  = (m - 1.0) / (m + 1.0);
    double y2 = y * y;
    double t  = y, s = y;
    for (int k = 3; k <= 21; k += 2) {
        t *= y2;
        s += t / (double)k;
    }
    return 2.0 * s + (double)exp2 * 0.69314718055994530942;
}

static double js_math_pow(double base, double exp) {
    if (exp == 0.0) return 1.0;
    if (js_num_is_nan(base) || js_num_is_nan(exp)) return js_make_nan();
    if (base == 0.0) return (exp > 0.0) ? 0.0 : (1e308 * 10.0);
    double ie = js_math_trunc(exp);
    if (exp == ie && ie >= -1024.0 && ie <= 1024.0) {
        int n = (int)ie;
        bool neg = (n < 0);
        if (neg) n = -n;
        double res = 1.0, b = base;
        while (n > 0) {
            if (n & 1) res *= b;
            b *= b;
            n >>= 1;
        }
        return neg ? (1.0 / res) : res;
    }
    if (base < 0.0) return js_make_nan();
    return js_math_exp(exp * js_math_ln(base));
}

/* =========================================================================
 * 2. Bytecode Opcodes & Runtime Value Model
 * ========================================================================= */
typedef enum JsOpCode {
    OP_CONST = 0,
    OP_POP,
    OP_DUP,
    OP_DUP2,
    OP_SWAP,
    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_GET_GLOBAL,
    OP_SET_GLOBAL,
    OP_GET_PROP,
    OP_SET_PROP,
    OP_GET_INDEX,
    OP_SET_INDEX,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_NEG,
    OP_POS,
    OP_NOT,
    OP_TYPEOF,
    OP_EQ,
    OP_NEQ,
    OP_STRICT_EQ,
    OP_STRICT_NEQ,
    OP_LT,
    OP_LTE,
    OP_GT,
    OP_GTE,
    OP_BIT_AND,
    OP_BIT_OR,
    OP_BIT_XOR,
    OP_SHL,
    OP_SHR,
    OP_JMP,
    OP_JMP_IF_FALSE,
    OP_JMP_IF_TRUE,
    OP_CALL,
    OP_CALL_METHOD,
    OP_CLOSURE,
    OP_NEW_ARRAY,
    OP_NEW_OBJECT,
    OP_TRY_PUSH,
    OP_TRY_POP,
    OP_THROW,
    OP_THIS,
    OP_RETURN,
    OP_HALT
} JsOpCode;

typedef struct JsInst {
    uint8_t  op;
    uint8_t  aux;   /* argc for OP_CALL_METHOD, or flags */
    uint32_t arg;   /* constant index, jump target, or (depth<<16)|slot */
} JsInst;

typedef enum JsBuiltinId {
    JS_BUILTIN_CONSOLE = 1,
    JS_BUILTIN_MATH,
    JS_BUILTIN_JSON,
    JS_BUILTIN_PERFORMANCE,
    JS_BUILTIN_DATE,
    JS_BUILTIN_LOCALSTORAGE,
    JS_BUILTIN_DOCUMENT,
    JS_BUILTIN_WINDOW,
    JS_BUILTIN_LOCATION,
    JS_BUILTIN_NUMBER_CTOR,
    JS_BUILTIN_STRING_CTOR,
    JS_BUILTIN_ARRAY_CTOR,
    JS_BUILTIN_OBJECT_CTOR
} JsBuiltinId;

typedef enum JsNativeFnId {
    JS_NAT_PARSE_INT = 1,
    JS_NAT_PARSE_FLOAT,
    JS_NAT_IS_NAN,
    JS_NAT_IS_FINITE,
    JS_NAT_NUMBER,
    JS_NAT_STRING,
    JS_NAT_BOOLEAN,
    JS_NAT_EVENT_PREVENT_DEFAULT,
    JS_NAT_EVENT_STOP_PROP
} JsNativeFnId;

typedef enum JsValKind {
    JS_VAL_UNDEFINED = 0,
    JS_VAL_NULL,
    JS_VAL_BOOL,
    JS_VAL_NUMBER,
    JS_VAL_STRING,
    JS_VAL_ARRAY,
    JS_VAL_OBJECT,
    JS_VAL_FUNCTION,
    JS_VAL_NATIVE_FN,
    JS_VAL_BUILTIN_OBJ,
    JS_VAL_DOM_NODE,
    JS_VAL_DOM_STYLE,
    JS_VAL_DOM_CLASSLIST,
    JS_VAL_CANVAS_CTX2D
} JsValKind;

typedef struct JsString {
    size_t len;
    char   data[];
} JsString;

struct JsArray;
struct JsObject;
struct JsFunc;
struct JsEnv;
struct JsCanvasCtx2D;

typedef struct JsVal {
    JsValKind kind;
    union {
        bool                  b;
        double                num;
        uint32_t              u32;
        JsString             *str;
        struct JsArray       *arr;
        struct JsObject      *obj;
        struct JsFunc        *fn;
        struct JsCanvasCtx2D *ctx2d;
    } as;
} JsVal;

typedef struct JsArray {
    JsVal  *items;
    size_t  len;
    size_t  cap;
} JsArray;

typedef struct JsProp {
    const char *key;
    JsVal       val;
} JsProp;

typedef struct JsObject {
    JsProp *props;
    size_t  len;
    size_t  cap;
} JsObject;

typedef struct JsChunk {
    JsInst *code;
    size_t  code_len;
    size_t  code_cap;
    JsVal  *consts;
    size_t  const_len;
    size_t  const_cap;
    uint16_t param_count;
    uint16_t local_count;
} JsChunk;

typedef struct JsEnv {
    struct JsEnv *parent;
    size_t        slot_count;
    JsVal         slots[];
} JsEnv;

typedef struct JsFunc {
    JsChunk *chunk;
    JsEnv   *captured_env;
} JsFunc;

typedef struct JsCanvasCtx2D {
    uint32_t node_uid;
    uint32_t fill_argb;
    uint32_t stroke_argb;
    float    line_width;
    float    path_x[256];
    float    path_y[256];
    uint8_t  path_move[256]; /* 1 = moveTo subpath start, 0 = lineTo */
    size_t   path_len;
} JsCanvasCtx2D;

typedef struct JsStorageKV {
    bool in_use;
    char key[64];
    char val[512];
} JsStorageKV;

typedef struct JsEventListener {
    bool     active;
    uint32_t node_uid;
    char     event_type[32];
    JsVal    callback;
} JsEventListener;

typedef struct JsTryFrame {
    size_t   frame_depth;
    size_t   stack_sp;
    JsChunk *chunk;
    JsEnv   *env;
    size_t   catch_ip;
} JsTryFrame;

struct ArcrJsVm {
    ArcrArena           vm_arena;
    ArcrArena           temp_arena; /* Reclaimed on memory pressure */
    size_t              heap_limit;
    size_t              bytecode_cache_bytes;
    ArcrJsHostCallbacks host_cb;
    uint64_t            start_monotonic_usec;
    uint64_t            rng_state;
    uint32_t            fuel;
    uint32_t            executed_scripts;

    JsObject           *globals;
    char                doc_title[256];
    char                location_href[512];
    JsStorageKV         local_storage[JS_MAX_STORAGE_ENTRIES];
    JsEventListener     listeners[JS_MAX_EVENT_LISTENERS];

    /* Active event dispatch flags for preventDefault / stopPropagation */
    bool               *active_ev_prevented;
    bool               *active_ev_stopped;

    /* Detached DOM node registry (createElement / createTextNode / remove) */
    ArcrDomNode        *detached_nodes[512];
    const ArcrDocument *detached_docs[512];
    size_t              detached_cursor;

    JsVal               stack[JS_MAX_STACK_SLOTS];
    size_t              sp;
    JsTryFrame          try_stack[JS_MAX_TRY_DEPTH];
    size_t              try_depth;
    JsVal               thrown_val;
    bool                has_exception;
};

/* Value constructors */
static JsVal js_undef(void) { JsVal v; memset(&v, 0, sizeof(v)); v.kind = JS_VAL_UNDEFINED; return v; }
static JsVal js_null(void)  { JsVal v; memset(&v, 0, sizeof(v)); v.kind = JS_VAL_NULL; return v; }
static JsVal js_bool(bool b){ JsVal v; memset(&v, 0, sizeof(v)); v.kind = JS_VAL_BOOL; v.as.b = b; return v; }
static JsVal js_num(double n){ JsVal v; memset(&v, 0, sizeof(v)); v.kind = JS_VAL_NUMBER; v.as.num = n; return v; }
static JsVal js_dom_node(uint32_t uid) {
    if (uid == 0u) return js_null();
    JsVal v; memset(&v, 0, sizeof(v)); v.kind = JS_VAL_DOM_NODE; v.as.u32 = uid; return v;
}

static void *vm_alloc(ArcrJsVm *vm, size_t sz, size_t align) {
    if (vm->vm_arena.total_used + sz > vm->heap_limit) return NULL;
    return arcr_arena_alloc(&vm->vm_arena, sz, align);
}

static JsVal js_mk_str_n(ArcrJsVm *vm, const char *s, size_t len) {
    JsString *st = (JsString *)vm_alloc(vm, sizeof(JsString) + len + 1u, sizeof(void *));
    if (!st) return js_undef();
    st->len = len;
    if (len > 0u && s) memcpy(st->data, s, len);
    st->data[len] = '\0';
    JsVal v; memset(&v, 0, sizeof(v));
    v.kind   = JS_VAL_STRING;
    v.as.str = st;
    return v;
}

static JsVal js_mk_str(ArcrJsVm *vm, const char *s) {
    return js_mk_str_n(vm, s ? s : "", s ? strlen(s) : 0u);
}

static JsArray *js_alloc_array(ArcrJsVm *vm, size_t init_cap) {
    JsArray *a = (JsArray *)vm_alloc(vm, sizeof(JsArray), sizeof(void *));
    if (!a) return NULL;
    a->len = 0u;
    a->cap = (init_cap > 4u) ? init_cap : 4u;
    a->items = (JsVal *)vm_alloc(vm, a->cap * sizeof(JsVal), sizeof(void *));
    if (!a->items) return NULL;
    return a;
}

static bool js_array_push(ArcrJsVm *vm, JsArray *a, JsVal item) {
    if (!a) return false;
    if (a->len >= a->cap) {
        size_t ncap = a->cap * 2u;
        JsVal *nitems = (JsVal *)vm_alloc(vm, ncap * sizeof(JsVal), sizeof(void *));
        if (!nitems) return false;
        memcpy(nitems, a->items, a->len * sizeof(JsVal));
        a->items = nitems;
        a->cap   = ncap;
    }
    a->items[a->len++] = item;
    return true;
}

static JsObject *js_alloc_object(ArcrJsVm *vm, size_t init_cap) {
    JsObject *o = (JsObject *)vm_alloc(vm, sizeof(JsObject), sizeof(void *));
    if (!o) return NULL;
    o->len = 0u;
    o->cap = (init_cap > 4u) ? init_cap : 4u;
    o->props = (JsProp *)vm_alloc(vm, o->cap * sizeof(JsProp), sizeof(void *));
    if (!o->props) return NULL;
    return o;
}

static bool js_obj_get(const JsObject *o, const char *key, JsVal *out_val) {
    if (!o || !key) return false;
    for (size_t i = 0u; i < o->len; ++i) {
        if (strcmp(o->props[i].key, key) == 0) {
            if (out_val) *out_val = o->props[i].val;
            return true;
        }
    }
    return false;
}

static bool js_obj_set(ArcrJsVm *vm, JsObject *o, const char *key, JsVal val) {
    if (!o || !key) return false;
    for (size_t i = 0u; i < o->len; ++i) {
        if (strcmp(o->props[i].key, key) == 0) {
            o->props[i].val = val;
            return true;
        }
    }
    if (o->len >= o->cap) {
        size_t ncap = o->cap * 2u;
        JsProp *nprops = (JsProp *)vm_alloc(vm, ncap * sizeof(JsProp), sizeof(void *));
        if (!nprops) return false;
        memcpy(nprops, o->props, o->len * sizeof(JsProp));
        o->props = nprops;
        o->cap   = ncap;
    }
    char *kcopy = arcr_arena_strdup(&vm->vm_arena, key, strlen(key));
    if (!kcopy) return false;
    o->props[o->len].key = kcopy;
    o->props[o->len].val = val;
    o->len++;
    return true;
}

/* =========================================================================
 * 3. JS Type Coercions & Comparisons
 * ========================================================================= */
static bool js_is_truthy(JsVal v) {
    switch (v.kind) {
        case JS_VAL_UNDEFINED:
        case JS_VAL_NULL:
            return false;
        case JS_VAL_BOOL:
            return v.as.b;
        case JS_VAL_NUMBER:
            return v.as.num != 0.0 && !js_num_is_nan(v.as.num);
        case JS_VAL_STRING:
            return v.as.str && v.as.str->len > 0u;
        default:
            return true;
    }
}

static double js_parse_float_str(const char *s, size_t len, bool allow_trailing) {
    if (!s) return js_make_nan();
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return allow_trailing ? js_make_nan() : 0.0;

    size_t i = 0u;
    double sign = 1.0;
    if (s[i] == '-') { sign = -1.0; ++i; }
    else if (s[i] == '+') { ++i; }

    if (i + 2u < len && s[i] == '0' && (s[i + 1u] == 'x' || s[i + 1u] == 'X')) {
        i += 2u;
        double val = 0.0;
        bool any = false;
        while (i < len) {
            char c = s[i];
            int d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            if (d < 0) { if (!allow_trailing) return js_make_nan(); break; }
            val = val * 16.0 + (double)d;
            any = true;
            ++i;
        }
        return any ? (sign * val) : js_make_nan();
    }

    double res = 0.0;
    bool any_digit = false;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        res = res * 10.0 + (double)(s[i] - '0');
        any_digit = true;
        ++i;
    }
    if (i < len && s[i] == '.') {
        ++i;
        double frac = 0.1;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            res += (double)(s[i] - '0') * frac;
            frac *= 0.1;
            any_digit = true;
            ++i;
        }
    }
    if (!any_digit) return js_make_nan();

    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        size_t save_e = i++;
        int esign = 1;
        if (i < len && s[i] == '-') { esign = -1; ++i; }
        else if (i < len && s[i] == '+') { ++i; }
        int exp_val = 0;
        bool exp_ok = false;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            if (exp_val < 1000) exp_val = exp_val * 10 + (s[i] - '0');
            exp_ok = true;
            ++i;
        }
        if (exp_ok) {
            res *= js_math_pow(10.0, (double)(esign * exp_val));
        } else {
            i = save_e;
        }
    }
    if (!allow_trailing && i < len) return js_make_nan();
    return sign * res;
}

static double js_to_number(JsVal v) {
    switch (v.kind) {
        case JS_VAL_NUMBER:    return v.as.num;
        case JS_VAL_BOOL:      return v.as.b ? 1.0 : 0.0;
        case JS_VAL_NULL:      return 0.0;
        case JS_VAL_UNDEFINED: return js_make_nan();
        case JS_VAL_STRING:
            return v.as.str ? js_parse_float_str(v.as.str->data, v.as.str->len, false) : 0.0;
        default:
            return js_make_nan();
    }
}

static void js_format_number(double n, char *buf, size_t cap) {
    if (js_num_is_nan(n)) { arcr_str_copy(buf, cap, "NaN"); return; }
    if (js_num_is_inf(n)) { arcr_str_copy(buf, cap, (n < 0.0) ? "-Infinity" : "Infinity"); return; }
    if (n == 0.0)         { arcr_str_copy(buf, cap, "0"); return; }
    double tr = js_math_trunc(n);
    if (n == tr && n >= -9007199254740991.0 && n <= 9007199254740991.0 &&
        n >= -2147483648.0 && n <= 2147483647.0) {
        snprintf(buf, cap, "%ld", (long)(int32_t)n);
        return;
    }
    snprintf(buf, cap, "%.14g", n);
}

static const char *js_to_cstr_buf(ArcrJsVm *vm, JsVal v, char *tmp, size_t cap) {
    switch (v.kind) {
        case JS_VAL_UNDEFINED: arcr_str_copy(tmp, cap, "undefined"); return tmp;
        case JS_VAL_NULL:      arcr_str_copy(tmp, cap, "null"); return tmp;
        case JS_VAL_BOOL:      arcr_str_copy(tmp, cap, v.as.b ? "true" : "false"); return tmp;
        case JS_VAL_NUMBER:    js_format_number(v.as.num, tmp, cap); return tmp;
        case JS_VAL_STRING:    return v.as.str ? v.as.str->data : "";
        case JS_VAL_ARRAY: {
            size_t pos = 0u;
            tmp[0] = '\0';
            if (v.as.arr) {
                for (size_t i = 0u; i < v.as.arr->len; ++i) {
                    if (i > 0u && pos + 1u < cap) tmp[pos++] = ',';
                    char sub[128];
                    const char *s = js_to_cstr_buf(vm, v.as.arr->items[i], sub, sizeof(sub));
                    size_t sl = strlen(s);
                    if (pos + sl >= cap) sl = (cap - 1u > pos) ? (cap - 1u - pos) : 0u;
                    if (sl > 0u) { memcpy(tmp + pos, s, sl); pos += sl; }
                }
                tmp[pos] = '\0';
            }
            return tmp;
        }
        case JS_VAL_DOM_NODE:  arcr_str_copy(tmp, cap, "[object HTMLElement]"); return tmp;
        case JS_VAL_FUNCTION:
        case JS_VAL_NATIVE_FN: arcr_str_copy(tmp, cap, "function() { [code] }"); return tmp;
        default:               arcr_str_copy(tmp, cap, "[object Object]"); return tmp;
    }
}

static JsVal js_to_string_val(ArcrJsVm *vm, JsVal v) {
    if (v.kind == JS_VAL_STRING) return v;
    char buf[512];
    const char *s = js_to_cstr_buf(vm, v, buf, sizeof(buf));
    return js_mk_str(vm, s);
}

static bool js_strict_equal(JsVal a, JsVal b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case JS_VAL_UNDEFINED:
        case JS_VAL_NULL:
            return true;
        case JS_VAL_BOOL:
            return a.as.b == b.as.b;
        case JS_VAL_NUMBER:
            return a.as.num == b.as.num;
        case JS_VAL_STRING:
            if (a.as.str == b.as.str) return true;
            if (!a.as.str || !b.as.str || a.as.str->len != b.as.str->len) return false;
            return memcmp(a.as.str->data, b.as.str->data, a.as.str->len) == 0;
        case JS_VAL_ARRAY:
            return a.as.arr == b.as.arr;
        case JS_VAL_OBJECT:
            return a.as.obj == b.as.obj;
        case JS_VAL_FUNCTION:
            return a.as.fn == b.as.fn;
        case JS_VAL_CANVAS_CTX2D:
            return a.as.ctx2d == b.as.ctx2d;
        default:
            return a.as.u32 == b.as.u32;
    }
}

static bool js_loose_equal(JsVal a, JsVal b) {
    if (a.kind == b.kind) return js_strict_equal(a, b);
    if ((a.kind == JS_VAL_NULL && b.kind == JS_VAL_UNDEFINED) ||
        (a.kind == JS_VAL_UNDEFINED && b.kind == JS_VAL_NULL)) {
        return true;
    }
    if ((a.kind == JS_VAL_NUMBER || a.kind == JS_VAL_BOOL) &&
        (b.kind == JS_VAL_NUMBER || b.kind == JS_VAL_BOOL || b.kind == JS_VAL_STRING)) {
        return js_to_number(a) == js_to_number(b);
    }
    if (a.kind == JS_VAL_STRING && (b.kind == JS_VAL_NUMBER || b.kind == JS_VAL_BOOL)) {
        return js_to_number(a) == js_to_number(b);
    }
    return false;
}

/* =========================================================================
 * 4. Self-Contained Live DOM, CSS Selector, innerHTML & <canvas> 2D Bridge
 *    (Operates directly on ArcrDocument + arcr_base; zero arcr_core link deps!)
 * ========================================================================= */
static uint64_t js_bloom_bits(ArcrAtom atom) {
    if (atom == ARCR_ATOM_NULL) return 0ULL;
    uint32_t x  = (uint32_t)atom * 0x9E3779B1u;
    uint32_t b1 = (x >> 26) & 63u;
    uint32_t b2 = ((x * 0x85EBCA6Bu) >> 26) & 63u;
    return (1ULL << b1) | (1ULL << b2);
}

static uint64_t js_node_self_bloom(const ArcrDomNode *n) {
    if (!n || n->kind != ARCR_NODE_ELEMENT) return 0ULL;
    uint64_t bits = js_bloom_bits(n->tag) | js_bloom_bits(n->id);
    for (uint32_t i = 0u; i < n->class_count; ++i) {
        bits |= js_bloom_bits(n->classes[i]);
    }
    return bits;
}

static void js_recompute_subtree_bloom(ArcrDomNode *n) {
    if (!n) return;
    if (n->parent) {
        n->ancestor_bloom = n->parent->ancestor_bloom | js_node_self_bloom(n->parent);
    } else {
        n->ancestor_bloom = 0ULL;
    }
    for (ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        js_recompute_subtree_bloom(ch);
    }
}

static void js_dom_ensure_uids_rec(ArcrDocument *doc, ArcrDomNode *n) {
    if (!n) return;
    if (n->node_uid == 0u) {
        if (doc->next_uid == 0u) doc->next_uid = 1u;
        n->node_uid = doc->next_uid++;
    } else if (n->node_uid >= doc->next_uid) {
        doc->next_uid = n->node_uid + 1u;
    }
    for (ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        js_dom_ensure_uids_rec(doc, ch);
    }
}

static void js_dom_ensure_uids(ArcrDocument *doc) {
    if (doc && doc->root) {
        js_dom_ensure_uids_rec(doc, doc->root);
    }
}

static ArcrDomNode *js_dom_find_uid_rec(ArcrDomNode *n, uint32_t uid) {
    if (!n || uid == 0u) return NULL;
    if (n->node_uid == uid) return n;
    for (ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        ArcrDomNode *hit = js_dom_find_uid_rec(ch, uid);
        if (hit) return hit;
    }
    return NULL;
}

static void js_dom_register_detached(ArcrJsVm *vm, const ArcrDocument *doc, ArcrDomNode *node) {
    if (!vm || !doc || !node) return;
    for (size_t i = 0u; i < 512u; ++i) {
        if (vm->detached_nodes[i] == node && vm->detached_docs[i] == doc) return;
    }
    size_t slot = vm->detached_cursor % 512u;
    vm->detached_nodes[slot] = node;
    vm->detached_docs[slot]  = doc;
    vm->detached_cursor++;
}

static ArcrDomNode *js_dom_find_uid(ArcrJsVm *vm, ArcrDocument *doc, uint32_t uid) {
    if (!doc || uid == 0u) return NULL;
    if (doc->root) {
        js_dom_ensure_uids(doc);
        ArcrDomNode *hit = js_dom_find_uid_rec(doc->root, uid);
        if (hit) return hit;
    }
    if (vm) {
        size_t lim = (vm->detached_cursor < 512u) ? vm->detached_cursor : 512u;
        for (size_t i = 0u; i < lim; ++i) {
            if (vm->detached_docs[i] == doc && vm->detached_nodes[i] != NULL) {
                ArcrDomNode *hit = js_dom_find_uid_rec(vm->detached_nodes[i], uid);
                if (hit) return hit;
            }
        }
    }
    return NULL;
}

static const char *js_dom_get_attr(const ArcrDocument *doc, const ArcrDomNode *el, const char *name) {
    if (!doc || !el || el->kind != ARCR_NODE_ELEMENT || !name) return NULL;
    ArcrAtom a = arcr_atom_intern_lower(doc->atoms, name, strlen(name));
    for (const ArcrAttr *at = el->first_attr; at != NULL; at = at->next) {
        if (at->name == a) return at->value;
    }
    return NULL;
}

static void js_dom_sync_class_attr(ArcrDocument *doc, ArcrDomNode *el) {
    char buf[512];
    size_t pos = 0u;
    buf[0] = '\0';
    for (uint32_t i = 0u; i < el->class_count; ++i) {
        const char *cs = arcr_atom_cstr(doc->atoms, el->classes[i]);
        if (!cs) continue;
        size_t cl = strlen(cs);
        if (pos > 0u && pos + 1u < sizeof(buf)) buf[pos++] = ' ';
        if (pos + cl < sizeof(buf)) {
            memcpy(buf + pos, cs, cl);
            pos += cl;
        }
    }
    buf[pos] = '\0';
    ArcrAtom ca = arcr_atom_intern_lower(doc->atoms, "class", 5u);
    char *stored = arcr_arena_strdup(&doc->arena, buf, pos);
    if (!stored) return;
    for (ArcrAttr *at = el->first_attr; at != NULL; at = at->next) {
        if (at->name == ca) {
            at->value = stored;
            doc->dom_dirty = true;
            return;
        }
    }
    ArcrAttr *na = (ArcrAttr *)arcr_arena_alloc(&doc->arena, sizeof(ArcrAttr), sizeof(void *));
    if (na) {
        na->name = ca;
        na->value = stored;
        na->next = el->first_attr;
        el->first_attr = na;
    }
    doc->dom_dirty = true;
}

static void js_dom_set_attr(ArcrDocument *doc, ArcrDomNode *el,
                            const char *name, size_t nlen,
                            const char *val, size_t vlen) {
    if (!doc || !el || el->kind != ARCR_NODE_ELEMENT || !name || nlen == 0u) return;
    ArcrAtom na = arcr_atom_intern_lower(doc->atoms, name, nlen);
    char *stored = arcr_arena_strdup(&doc->arena, val ? val : "", vlen);
    if (!stored) return;

    bool updated = false;
    for (ArcrAttr *at = el->first_attr; at != NULL; at = at->next) {
        if (at->name == na) {
            at->value = stored;
            updated   = true;
            break;
        }
    }
    if (!updated) {
        ArcrAttr *at = (ArcrAttr *)arcr_arena_alloc(&doc->arena, sizeof(ArcrAttr), sizeof(void *));
        if (!at) return;
        at->name       = na;
        at->value      = stored;
        at->next       = el->first_attr;
        el->first_attr = at;
    }

    if (arcr_str_eq_nocase_n(name, nlen, "id", 2u)) {
        el->id = (vlen > 0u) ? arcr_atom_intern(doc->atoms, val, vlen) : ARCR_ATOM_NULL;
        js_recompute_subtree_bloom(el);
    } else if (arcr_str_eq_nocase_n(name, nlen, "class", 5u)) {
        el->class_count = 0u;
        size_t i = 0u;
        while (i < vlen && el->class_count < ARCR_MAX_INLINE_CLASSES) {
            while (i < vlen && (val[i] == ' ' || val[i] == '\t' || val[i] == '\n' || val[i] == '\r')) ++i;
            size_t st = i;
            while (i < vlen && val[i] != ' ' && val[i] != '\t' && val[i] != '\n' && val[i] != '\r') ++i;
            if (i > st) {
                el->classes[el->class_count++] = arcr_atom_intern(doc->atoms, val + st, i - st);
            }
        }
        js_recompute_subtree_bloom(el);
    } else if (arcr_str_eq_nocase_n(name, nlen, "checked", 7u)) {
        el->state_flags |= ARCR_STATE_CHECKED;
    }
    doc->dom_dirty = true;
}

static void js_dom_unlink(ArcrDomNode *node) {
    if (!node || !node->parent) return;
    ArcrDomNode *p = node->parent;
    ArcrDomNode *prev = NULL;
    for (ArcrDomNode *cur = p->first_child; cur != NULL; cur = cur->next_sibling) {
        if (cur == node) {
            if (prev) prev->next_sibling = cur->next_sibling;
            else p->first_child = cur->next_sibling;
            if (p->last_child == cur) p->last_child = prev;
            break;
        }
        prev = cur;
    }
    node->parent       = NULL;
    node->next_sibling = NULL;
}

static void js_dom_append_child(ArcrDocument *doc, ArcrDomNode *parent, ArcrDomNode *child) {
    if (!doc || !parent || !child || parent == child) return;
    for (ArcrDomNode *anc = parent; anc != NULL; anc = anc->parent) {
        if (anc == child) return; /* Prevent DOM cycle */
    }
    js_dom_unlink(child);
    child->parent       = parent;
    child->next_sibling = NULL;
    if (!parent->first_child) {
        parent->first_child = child;
        parent->last_child  = child;
    } else {
        parent->last_child->next_sibling = child;
        parent->last_child               = child;
    }
    js_recompute_subtree_bloom(child);
    doc->dom_dirty = true;
}

static ArcrDomNode *js_dom_create_element(ArcrDocument *doc, const char *tag, size_t tlen) {
    if (!doc || !tag || tlen == 0u) return NULL;
    ArcrDomNode *n = (ArcrDomNode *)arcr_slab_alloc(&doc->node_slab);
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    if (doc->next_uid == 0u) doc->next_uid = 1u;
    n->kind     = ARCR_NODE_ELEMENT;
    n->node_uid = doc->next_uid++;
    n->tag      = arcr_atom_intern_lower(doc->atoms, tag, tlen);
    doc->node_count++;
    doc->dom_dirty = true;
    return n;
}

static ArcrDomNode *js_dom_create_text(ArcrDocument *doc, const char *txt, size_t len) {
    if (!doc) return NULL;
    ArcrDomNode *n = (ArcrDomNode *)arcr_slab_alloc(&doc->node_slab);
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    if (doc->next_uid == 0u) doc->next_uid = 1u;
    n->kind     = ARCR_NODE_TEXT;
    n->node_uid = doc->next_uid++;
    n->text     = arcr_arena_strdup(&doc->arena, txt ? txt : "", len);
    n->text_len = len;
    doc->node_count++;
    doc->dom_dirty = true;
    return n;
}

static void js_dom_clear_children(ArcrDocument *doc, ArcrDomNode *el) {
    if (!doc || !el) return;
    ArcrDomNode *ch = el->first_child;
    while (ch) {
        ArcrDomNode *nx = ch->next_sibling;
        ch->parent       = NULL;
        ch->next_sibling = NULL;
        ch = nx;
    }
    el->first_child = NULL;
    el->last_child  = NULL;
    doc->dom_dirty  = true;
}

static size_t js_dom_collect_text(const ArcrDomNode *n, char *out, size_t cap, size_t pos) {
    if (!n) return pos;
    if (n->kind == ARCR_NODE_TEXT && n->text && n->text_len > 0u) {
        if (out && cap > 0u && pos + 1u < cap) {
            size_t cn = ARCR_MIN(n->text_len, cap - 1u - pos);
            memcpy(out + pos, n->text, cn);
        }
        pos += n->text_len;
    }
    for (const ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        pos = js_dom_collect_text(ch, out, cap, pos);
    }
    return pos;
}

static void js_dom_parse_fragment(ArcrDocument *doc, ArcrDomNode *target, const char *html, size_t len) {
    if (!doc || !target) return;
    js_dom_clear_children(doc, target);
    if (!html || len == 0u) return;

    ArcrDomNode *stack[32];
    size_t depth = 0u;
    stack[depth++] = target;

    size_t i = 0u;
    while (i < len) {
        if (html[i] == '<') {
            if (i + 4u <= len && memcmp(html + i, "<!--", 4u) == 0) {
                size_t j = i + 4u;
                while (j + 3u <= len && memcmp(html + j, "-->", 3u) != 0) ++j;
                i = (j + 3u <= len) ? (j + 3u) : len;
                continue;
            }
            if (i + 2u < len && html[i + 1u] == '/') {
                size_t j = i + 2u;
                size_t ts = j;
                while (j < len && html[j] != '>' && html[j] != ' ') ++j;
                size_t te = j;
                while (j < len && html[j] != '>') ++j;
                i = (j < len) ? (j + 1u) : len;
                if (te > ts) {
                    ArcrAtom ca = arcr_atom_intern_lower(doc->atoms, html + ts, te - ts);
                    for (size_t k = depth; k > 1u; --k) {
                        if (stack[k - 1u]->tag == ca) { depth = k - 1u; break; }
                    }
                }
                continue;
            }
            if (i + 1u < len && ((html[i + 1u] >= 'a' && html[i + 1u] <= 'z') ||
                                 (html[i + 1u] >= 'A' && html[i + 1u] <= 'Z'))) {
                size_t j = i + 1u;
                size_t ts = j;
                while (j < len && html[j] != ' ' && html[j] != '\t' && html[j] != '\n' &&
                       html[j] != '\r' && html[j] != '>' && html[j] != '/') ++j;
                const char *tptr = html + ts;
                size_t tlen = j - ts;
                ArcrDomNode *el = js_dom_create_element(doc, tptr, tlen);
                if (el) js_dom_append_child(doc, stack[depth - 1u], el);

                bool self_close = false;
                while (j < len) {
                    while (j < len && (html[j] == ' ' || html[j] == '\t' || html[j] == '\n' || html[j] == '\r')) ++j;
                    if (j >= len) break;
                    if (html[j] == '>') { ++j; break; }
                    if (html[j] == '/' && j + 1u < len && html[j + 1u] == '>') { self_close = true; j += 2u; break; }
                    size_t as = j;
                    while (j < len && html[j] != '=' && html[j] != '>' && html[j] != '/' &&
                           html[j] != ' ' && html[j] != '\t') ++j;
                    size_t ae = j;
                    while (j < len && (html[j] == ' ' || html[j] == '\t')) ++j;
                    const char *vp = "";
                    size_t vl = 0u;
                    if (j < len && html[j] == '=') {
                        ++j;
                        while (j < len && (html[j] == ' ' || html[j] == '\t')) ++j;
                        if (j < len && (html[j] == '"' || html[j] == '\'')) {
                            char q = html[j++];
                            size_t vs = j;
                            while (j < len && html[j] != q) ++j;
                            vp = html + vs;
                            vl = j - vs;
                            if (j < len) ++j;
                        } else {
                            size_t vs = j;
                            while (j < len && html[j] != ' ' && html[j] != '>') ++j;
                            vp = html + vs;
                            vl = j - vs;
                        }
                    }
                    if (ae > as && el) js_dom_set_attr(doc, el, html + as, ae - as, vp, vl);
                    else if (j == as) ++j;
                }
                i = j;
                bool is_void = arcr_str_eq_nocase_n(tptr, tlen, "br", 2u) ||
                               arcr_str_eq_nocase_n(tptr, tlen, "hr", 2u) ||
                               arcr_str_eq_nocase_n(tptr, tlen, "img", 3u) ||
                               arcr_str_eq_nocase_n(tptr, tlen, "input", 5u);
                if (!self_close && !is_void && el && depth + 1u < ARCR_ARRAY_LEN(stack)) {
                    stack[depth++] = el;
                }
                continue;
            }
        }
        size_t ts = i;
        while (i < len && html[i] != '<') ++i;
        if (i > ts) {
            ArcrDomNode *tn = js_dom_create_text(doc, html + ts, i - ts);
            if (tn) js_dom_append_child(doc, stack[depth - 1u], tn);
        }
    }
}

static size_t js_dom_serialize_children(const ArcrDocument *doc, const ArcrDomNode *parent,
                                        char *out, size_t cap, size_t pos) {
    for (const ArcrDomNode *ch = parent ? parent->first_child : NULL; ch != NULL; ch = ch->next_sibling) {
        if (ch->kind == ARCR_NODE_TEXT && ch->text) {
            if (out && pos + ch->text_len < cap) memcpy(out + pos, ch->text, ch->text_len);
            pos += ch->text_len;
        } else if (ch->kind == ARCR_NODE_ELEMENT) {
            const char *tg = arcr_atom_cstr(doc->atoms, ch->tag);
            if (!tg) tg = "div";
            size_t tl = strlen(tg);
            if (out && pos + tl + 2u < cap) {
                out[pos] = '<';
                memcpy(out + pos + 1u, tg, tl);
                out[pos + 1u + tl] = '>';
            }
            pos += tl + 2u;
            pos = js_dom_serialize_children(doc, ch, out, cap, pos);
            if (out && pos + tl + 3u < cap) {
                out[pos] = '<';
                out[pos + 1u] = '/';
                memcpy(out + pos + 2u, tg, tl);
                out[pos + 2u + tl] = '>';
            }
            pos += tl + 3u;
        }
    }
    if (out && cap > 0u) out[(pos < cap) ? pos : (cap - 1u)] = '\0';
    return pos;
}

/* CSS Selector matching for getElementById / querySelector / querySelectorAll */
typedef struct JsSimpleSel {
    ArcrAtom    tag;
    ArcrAtom    id;
    ArcrAtom    classes[4];
    uint32_t    class_cnt;
    ArcrAtom    attr_name;
    const char *attr_val;
    size_t      attr_vlen;
    bool        has_attr_val;
} JsSimpleSel;

static void js_parse_simple_sel(ArcrAtomTable *atoms, const char *s, size_t len, JsSimpleSel *out) {
    memset(out, 0, sizeof(*out));
    arcr_str_trim_view(&s, &len);
    if (len == 0u || (len == 1u && s[0] == '*')) return;

    size_t i = 0u;
    if (s[0] != '#' && s[0] != '.' && s[0] != '[') {
        size_t st = 0u;
        while (i < len && s[i] != '#' && s[i] != '.' && s[i] != '[' && s[i] != ':') ++i;
        if (i > st) out->tag = arcr_atom_intern_lower(atoms, s + st, i - st);
    }
    while (i < len) {
        char ch = s[i];
        if (ch == '#' || ch == '.') {
            ++i;
            size_t st = i;
            while (i < len && s[i] != '#' && s[i] != '.' && s[i] != '[' && s[i] != ':') ++i;
            if (i > st) {
                if (ch == '#') out->id = arcr_atom_intern(atoms, s + st, i - st);
                else if (out->class_cnt < 4u)
                    out->classes[out->class_cnt++] = arcr_atom_intern(atoms, s + st, i - st);
            }
        } else if (ch == '[') {
            ++i;
            size_t st = i;
            while (i < len && s[i] != '=' && s[i] != ']') ++i;
            if (i > st) {
                const char *an = s + st;
                size_t al = i - st;
                arcr_str_trim_view(&an, &al);
                out->attr_name = arcr_atom_intern_lower(atoms, an, al);
            }
            if (i < len && s[i] == '=') {
                ++i;
                size_t vs = i;
                while (i < len && s[i] != ']') ++i;
                const char *av = s + vs;
                size_t avl = i - vs;
                arcr_str_trim_view(&av, &avl);
                if (avl >= 2u && (av[0] == '"' || av[0] == '\'') && av[avl - 1u] == av[0]) {
                    ++av;
                    avl -= 2u;
                }
                out->attr_val     = av;
                out->attr_vlen    = avl;
                out->has_attr_val = true;
            }
            if (i < len && s[i] == ']') ++i;
        } else {
            ++i;
        }
    }
}

static bool js_match_simple_sel(const JsSimpleSel *sel, const ArcrDomNode *el) {
    if (!el || el->kind != ARCR_NODE_ELEMENT) return false;
    if (sel->tag != ARCR_ATOM_NULL && el->tag != sel->tag) return false;
    if (sel->id != ARCR_ATOM_NULL && el->id != sel->id) return false;
    for (uint32_t c = 0u; c < sel->class_cnt; ++c) {
        bool found = false;
        for (uint32_t j = 0u; j < el->class_count; ++j) {
            if (el->classes[j] == sel->classes[c]) { found = true; break; }
        }
        if (!found) return false;
    }
    if (sel->attr_name != ARCR_ATOM_NULL) {
        const char *av = NULL;
        for (const ArcrAttr *at = el->first_attr; at != NULL; at = at->next) {
            if (at->name == sel->attr_name) { av = at->value; break; }
        }
        if (!av) return false;
        if (sel->has_attr_val && (strlen(av) != sel->attr_vlen ||
                                  memcmp(av, sel->attr_val, sel->attr_vlen) != 0)) {
            return false;
        }
    }
    return true;
}

static bool js_match_compound_sel(ArcrAtomTable *atoms, const char *sel_str, size_t sel_len,
                                  const ArcrDomNode *el, const ArcrDomNode *scope_root) {
    arcr_str_trim_view(&sel_str, &sel_len);
    if (sel_len == 0u) return false;

    /* Split whitespace/'>' combinators right-to-left */
    JsSimpleSel chain[8];
    bool direct_parent[8];
    size_t count = 0u;
    memset(direct_parent, 0, sizeof(direct_parent));

    size_t i = 0u;
    bool next_direct = false;
    while (i < sel_len && count < 8u) {
        while (i < sel_len && (sel_str[i] == ' ' || sel_str[i] == '\t')) ++i;
        if (i < sel_len && sel_str[i] == '>') {
            next_direct = true;
            ++i;
            while (i < sel_len && (sel_str[i] == ' ' || sel_str[i] == '\t')) ++i;
        }
        size_t st = i;
        int brk = 0;
        while (i < sel_len) {
            if (sel_str[i] == '[') ++brk;
            else if (sel_str[i] == ']' && brk > 0) --brk;
            else if (brk == 0 && (sel_str[i] == ' ' || sel_str[i] == '\t' || sel_str[i] == '>')) break;
            ++i;
        }
        if (i > st) {
            js_parse_simple_sel(atoms, sel_str + st, i - st, &chain[count]);
            direct_parent[count] = next_direct;
            next_direct = false;
            ++count;
        }
    }
    if (count == 0u) return false;
    if (!js_match_simple_sel(&chain[count - 1u], el)) return false;

    const ArcrDomNode *cur = el;
    for (size_t k = count - 1u; k > 0u; --k) {
        if (direct_parent[k]) {
            cur = cur->parent;
            if (!cur || cur == scope_root->parent || !js_match_simple_sel(&chain[k - 1u], cur)) {
                return false;
            }
        } else {
            cur = cur->parent;
            bool matched = false;
            while (cur && cur != scope_root->parent) {
                if (js_match_simple_sel(&chain[k - 1u], cur)) { matched = true; break; }
                cur = cur->parent;
            }
            if (!matched) return false;
        }
    }
    return true;
}

static bool js_match_selector_list(ArcrAtomTable *atoms, const char *sel, const ArcrDomNode *el,
                                   const ArcrDomNode *scope_root) {
    size_t len = strlen(sel);
    size_t pos = 0u;
    while (pos < len) {
        size_t comma = pos;
        int brk = 0;
        while (comma < len) {
            if (sel[comma] == '[') ++brk;
            else if (sel[comma] == ']' && brk > 0) --brk;
            else if (brk == 0 && sel[comma] == ',') break;
            ++comma;
        }
        if (js_match_compound_sel(atoms, sel + pos, comma - pos, el, scope_root)) return true;
        pos = (comma < len) ? (comma + 1u) : len;
    }
    return false;
}

static void js_query_walk(ArcrDocument *doc, ArcrDomNode *node, ArcrDomNode *scope_root,
                          const char *sel, ArcrDomNode **out, size_t *io_cnt, size_t max_out) {
    if (!node || *io_cnt >= max_out) return;
    if (node != scope_root && node->kind == ARCR_NODE_ELEMENT) {
        if (js_match_selector_list(doc->atoms, sel, node, scope_root)) {
            out[(*io_cnt)++] = node;
            if (*io_cnt >= max_out) return;
        }
    }
    for (ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        js_query_walk(doc, ch, scope_root, sel, out, io_cnt, max_out);
        if (*io_cnt >= max_out) return;
    }
}

static ArcrDomNode *js_dom_find_by_id_rec(ArcrDomNode *n, ArcrAtom id_atom) {
    if (!n) return NULL;
    if (n->kind == ARCR_NODE_ELEMENT && n->id == id_atom) return n;
    for (ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        ArcrDomNode *hit = js_dom_find_by_id_rec(ch, id_atom);
        if (hit) return hit;
    }
    return NULL;
}

/* Inline style camelCase <-> kebab-case updater */
static void camel_to_kebab(const char *src, char *dst, size_t cap) {
    if (strcmp(src, "cssFloat") == 0) { arcr_str_copy(dst, cap, "float"); return; }
    size_t j = 0u;
    for (size_t i = 0u; src[i] != '\0' && j + 2u < cap; ++i) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') {
            dst[j++] = '-';
            dst[j++] = (char)(c - 'A' + 'a');
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

static void js_style_set_prop(ArcrDocument *doc, ArcrDomNode *el,
                              const char *camel_prop, const char *new_val) {
    if (!doc || !el) return;
    if (strcmp(camel_prop, "cssText") == 0) {
        js_dom_set_attr(doc, el, "style", 5u, new_val, strlen(new_val));
        return;
    }
    char kebab[64];
    camel_to_kebab(camel_prop, kebab, sizeof(kebab));
    size_t klen = strlen(kebab);

    const char *old_css = js_dom_get_attr(doc, el, "style");
    char rebuilt[1024];
    size_t rpos = 0u;
    rebuilt[0] = '\0';

    if (old_css && old_css[0] != '\0') {
        size_t olen = strlen(old_css);
        size_t i = 0u;
        while (i < olen) {
            size_t semi = i;
            while (semi < olen && old_css[semi] != ';') ++semi;
            const char *decl = old_css + i;
            size_t dlen = semi - i;
            i = (semi < olen) ? (semi + 1u) : olen;
            arcr_str_trim_view(&decl, &dlen);
            if (dlen == 0u) continue;

            size_t colon = 0u;
            while (colon < dlen && decl[colon] != ':') ++colon;
            if (colon < dlen) {
                const char *pk = decl;
                size_t pkl = colon;
                arcr_str_trim_view(&pk, &pkl);
                if (arcr_str_eq_nocase_n(pk, pkl, kebab, klen)) continue;
            }
            if (rpos + dlen + 3u < sizeof(rebuilt)) {
                if (rpos > 0u) rebuilt[rpos++] = ' ';
                memcpy(rebuilt + rpos, decl, dlen);
                rpos += dlen;
                rebuilt[rpos++] = ';';
            }
        }
    }
    if (new_val && new_val[0] != '\0') {
        int n = snprintf(rebuilt + rpos, sizeof(rebuilt) - rpos,
                         "%s%s: %s;", (rpos > 0u) ? " " : "", kebab, new_val);
        if (n > 0 && rpos + (size_t)n < sizeof(rebuilt)) rpos += (size_t)n;
    }
    rebuilt[rpos] = '\0';
    js_dom_set_attr(doc, el, "style", 5u, rebuilt, strlen(rebuilt));
}

static void js_style_get_prop(const ArcrDocument *doc, const ArcrDomNode *el,
                              const char *camel_prop, char *out_val, size_t cap) {
    out_val[0] = '\0';
    if (!doc || !el) return;
    const char *css = js_dom_get_attr(doc, el, "style");
    if (!css) return;
    if (strcmp(camel_prop, "cssText") == 0) {
        arcr_str_copy(out_val, cap, css);
        return;
    }
    char kebab[64];
    camel_to_kebab(camel_prop, kebab, sizeof(kebab));
    size_t klen = strlen(kebab);
    size_t clen = strlen(css);
    size_t i = 0u;
    while (i < clen) {
        size_t semi = i;
        while (semi < clen && css[semi] != ';') ++semi;
        const char *decl = css + i;
        size_t dlen = semi - i;
        i = (semi < clen) ? (semi + 1u) : clen;
        size_t colon = 0u;
        while (colon < dlen && decl[colon] != ':') ++colon;
        if (colon < dlen) {
            const char *pk = decl;
            size_t pkl = colon;
            const char *pv = decl + colon + 1u;
            size_t pvl = dlen - colon - 1u;
            arcr_str_trim_view(&pk, &pkl);
            arcr_str_trim_view(&pv, &pvl);
            if (arcr_str_eq_nocase_n(pk, pkl, kebab, klen)) {
                size_t cn = ARCR_MIN(pvl, cap - 1u);
                memcpy(out_val, pv, cn);
                out_val[cn] = '\0';
            }
        }
    }
}

/* HTML5 <canvas> 2D Color Parser & Software Rasterizer */
static int js_hex_nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint32_t js_parse_css_color(const char *s, uint32_t fallback) {
    if (!s) return fallback;
    size_t len = strlen(s);
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return fallback;

    if (s[0] == '#') {
        if (len == 4u) {
            int r = js_hex_nib(s[1]), g = js_hex_nib(s[2]), b = js_hex_nib(s[3]);
            if (r >= 0 && g >= 0 && b >= 0)
                return 0xFF000000u | ((uint32_t)(r * 17) << 16) |
                       ((uint32_t)(g * 17) << 8) | (uint32_t)(b * 17);
        } else if (len == 7u || len == 9u) {
            int r = (js_hex_nib(s[1]) << 4) | js_hex_nib(s[2]);
            int g = (js_hex_nib(s[3]) << 4) | js_hex_nib(s[4]);
            int b = (js_hex_nib(s[5]) << 4) | js_hex_nib(s[6]);
            int a = (len == 9u) ? ((js_hex_nib(s[7]) << 4) | js_hex_nib(s[8])) : 255;
            if (r >= 0 && g >= 0 && b >= 0 && a >= 0)
                return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
        return fallback;
    }

    if ((len > 5u && arcr_str_eq_nocase_n(s, 4u, "rgb(", 4u)) ||
        (len > 6u && arcr_str_eq_nocase_n(s, 5u, "rgba(", 5u))) {
        bool has_a = (s[3] == 'a' || s[3] == 'A');
        const char *p = s + (has_a ? 5u : 4u);
        size_t rem = len - (has_a ? 6u : 5u);
        double c[4] = {0.0, 0.0, 0.0, 1.0};
        int cnt = has_a ? 4 : 3;
        for (int idx = 0; idx < cnt; ++idx) {
            size_t cm = 0u;
            while (cm < rem && p[cm] != ',') ++cm;
            c[idx] = js_parse_float_str(p, cm, true);
            if (js_num_is_nan(c[idx])) c[idx] = (idx == 3) ? 1.0 : 0.0;
            if (cm < rem) { p += cm + 1u; rem -= cm + 1u; }
            else rem = 0u;
        }
        uint32_t r = (uint32_t)ARCR_CLAMP((int32_t)c[0], 0, 255);
        uint32_t g = (uint32_t)ARCR_CLAMP((int32_t)c[1], 0, 255);
        uint32_t b = (uint32_t)ARCR_CLAMP((int32_t)c[2], 0, 255);
        uint32_t a = (uint32_t)ARCR_CLAMP((int32_t)js_math_round(c[3] * 255.0), 0, 255);
        return (a << 24) | (r << 16) | (g << 8) | b;
    }

    static const struct { const char *n; uint32_t c; } kNames[] = {
        {"transparent", 0x00000000u}, {"black", 0xFF000000u}, {"white", 0xFFFFFFFFu},
        {"red", 0xFFEF4444u}, {"green", 0xFF22C55Eu}, {"blue", 0xFF3B82F6u},
        {"yellow", 0xFFEAB308u}, {"orange", 0xFFF97316u}, {"purple", 0xFFA855F7u},
        {"cyan", 0xFF06B6D4u}, {"magenta", 0xFFEC4899u}, {"gray", 0xFF6B7280u},
        {"grey", 0xFF6B7280u}, {"navy", 0xFF1E3A8Au}, {"silver", 0xFFCBD5E1u},
        {"teal", 0xFF14B8A6u}, {"gold", 0xFFFACC15u}, {"tomato", 0xFFF87171u},
        {"dodgerblue", 0xFF1E90FFu}, {"cornflowerblue", 0xFF6495EDu}
    };
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kNames); ++i) {
        if (arcr_str_eq_nocase_n(s, len, kNames[i].n, strlen(kNames[i].n))) return kNames[i].c;
    }
    return fallback;
}

static ArcrDomSurface *js_ensure_canvas_surface(ArcrDocument *doc, ArcrDomNode *el) {
    if (!doc || !el) return NULL;
    int32_t w = 300, h = 150;
    const char *ws = js_dom_get_attr(doc, el, "width");
    const char *hs = js_dom_get_attr(doc, el, "height");
    if (ws) { int32_t pw = 0; if (arcr_parse_i32(ws, strlen(ws), &pw) && pw > 0 && pw <= 4096) w = pw; }
    if (hs) { int32_t ph = 0; if (arcr_parse_i32(hs, strlen(hs), &ph) && ph > 0 && ph <= 4096) h = ph; }

    if (el->surface && el->surface->width == w && el->surface->height == h && el->surface->argb_pixels) {
        return el->surface;
    }
    ArcrDomSurface *sf = (ArcrDomSurface *)arcr_arena_alloc(&doc->arena, sizeof(ArcrDomSurface), sizeof(void *));
    if (!sf) return NULL;
    uint32_t *px = (uint32_t *)arcr_arena_calloc(&doc->arena, (size_t)w * (size_t)h, sizeof(uint32_t), 16u);
    if (!px) return NULL;
    sf->width       = w;
    sf->height      = h;
    sf->argb_pixels = px;
    sf->heap_owned  = false;
    el->surface     = sf;
    doc->dom_dirty  = true;
    return sf;
}

static void canvas_put_pixel(ArcrDomSurface *sf, int32_t x, int32_t y, uint32_t src) {
    if (x < 0 || y < 0 || x >= sf->width || y >= sf->height) return;
    uint32_t sa = (src >> 24) & 0xFFu;
    if (sa == 0u) return;
    uint32_t *dst_p = &sf->argb_pixels[(size_t)y * (size_t)sf->width + (size_t)x];
    if (sa == 255u) {
        *dst_p = src;
        return;
    }
    uint32_t d = *dst_p;
    uint32_t inv = 255u - sa;
    uint32_t r = (((src >> 16) & 0xFFu) * sa + ((d >> 16) & 0xFFu) * inv + 127u) / 255u;
    uint32_t g = (((src >> 8)  & 0xFFu) * sa + ((d >> 8)  & 0xFFu) * inv + 127u) / 255u;
    uint32_t b = (( src        & 0xFFu) * sa + ( d        & 0xFFu) * inv + 127u) / 255u;
    uint32_t a = sa + (((d >> 24) & 0xFFu) * inv + 127u) / 255u;
    if (a > 255u) a = 255u;
    *dst_p = (a << 24) | (r << 16) | (g << 8) | b;
}

static void canvas_fill_rect(ArcrDomSurface *sf, double rx, double ry, double rw, double rh,
                             uint32_t color, bool clear_mode) {
    if (!sf || !sf->argb_pixels) return;
    if (rw < 0.0) { rx += rw; rw = -rw; }
    if (rh < 0.0) { ry += rh; rh = -rh; }
    int32_t x0 = ARCR_CLAMP((int32_t)js_math_round(rx), 0, sf->width);
    int32_t y0 = ARCR_CLAMP((int32_t)js_math_round(ry), 0, sf->height);
    int32_t x1 = ARCR_CLAMP((int32_t)js_math_round(rx + rw), 0, sf->width);
    int32_t y1 = ARCR_CLAMP((int32_t)js_math_round(ry + rh), 0, sf->height);
    for (int32_t y = y0; y < y1; ++y) {
        uint32_t *row = sf->argb_pixels + (size_t)y * (size_t)sf->width;
        if (clear_mode) {
            for (int32_t x = x0; x < x1; ++x) row[x] = 0x00000000u;
        } else if ((color >> 24) == 0xFFu) {
            for (int32_t x = x0; x < x1; ++x) row[x] = color;
        } else {
            for (int32_t x = x0; x < x1; ++x) canvas_put_pixel(sf, x, y, color);
        }
    }
}

static void canvas_draw_segment(ArcrDomSurface *sf, float x0, float y0, float x1, float y1,
                                float line_w, uint32_t color) {
    float dx = x1 - x0;
    float dy = y1 - y0;
    float steps = (float)js_math_sqrt((double)(dx * dx + dy * dy)) * 2.0f;
    if (steps < 1.0f) steps = 1.0f;
    int n = (int)steps;
    int r = (int)((line_w - 1.0f) * 0.5f);
    for (int i = 0; i <= n; ++i) {
        float t  = (float)i / (float)n;
        int32_t cx = (int32_t)js_math_round((double)(x0 + dx * t));
        int32_t cy = (int32_t)js_math_round((double)(y0 + dy * t));
        for (int oy = -r; oy <= r; ++oy) {
            for (int ox = -r; ox <= r; ++ox) {
                canvas_put_pixel(sf, cx + ox, cy + oy, color);
            }
        }
    }
}

static void canvas_stroke_path(ArcrDomSurface *sf, const JsCanvasCtx2D *ctx) {
    if (!sf || !ctx || ctx->path_len < 2u) return;
    float lw = (ctx->line_width >= 1.0f) ? ctx->line_width : 1.0f;
    for (size_t i = 1u; i < ctx->path_len; ++i) {
        if (ctx->path_move[i] == 0u) {
            canvas_draw_segment(sf, ctx->path_x[i - 1u], ctx->path_y[i - 1u],
                                ctx->path_x[i], ctx->path_y[i], lw, ctx->stroke_argb);
        }
    }
}

static void canvas_fill_path(ArcrDomSurface *sf, const JsCanvasCtx2D *ctx) {
    if (!sf || !ctx || ctx->path_len < 3u) return;
    float min_y = ctx->path_y[0], max_y = ctx->path_y[0];
    for (size_t i = 1u; i < ctx->path_len; ++i) {
        if (ctx->path_y[i] < min_y) min_y = ctx->path_y[i];
        if (ctx->path_y[i] > max_y) max_y = ctx->path_y[i];
    }
    int32_t y0 = ARCR_CLAMP((int32_t)js_math_floor((double)min_y), 0, sf->height - 1);
    int32_t y1 = ARCR_CLAMP((int32_t)js_math_ceil((double)max_y), 0, sf->height - 1);

    for (int32_t y = y0; y <= y1; ++y) {
        float scan_y = (float)y + 0.5f;
        float x_hits[64];
        size_t hit_cnt = 0u;
        size_t sub_start = 0u;

        for (size_t i = 0u; i < ctx->path_len; ++i) {
            if (ctx->path_move[i] != 0u) sub_start = i;
            bool is_sub_end = (i + 1u == ctx->path_len) || (ctx->path_move[i + 1u] != 0u);
            size_t j = is_sub_end ? sub_start : (i + 1u);
            if (i == j) continue;

            float ay = ctx->path_y[i], by = ctx->path_y[j];
            float ax = ctx->path_x[i], bx = ctx->path_x[j];
            if ((ay <= scan_y && by > scan_y) || (by <= scan_y && ay > scan_y)) {
                if (hit_cnt < ARCR_ARRAY_LEN(x_hits)) {
                    x_hits[hit_cnt++] = ax + (scan_y - ay) * (bx - ax) / (by - ay);
                }
            }
        }
        /* Insertion sort intersections */
        for (size_t a = 1u; a < hit_cnt; ++a) {
            float kv = x_hits[a];
            size_t b = a;
            while (b > 0u && x_hits[b - 1u] > kv) { x_hits[b] = x_hits[b - 1u]; --b; }
            x_hits[b] = kv;
        }
        for (size_t k = 0u; k + 1u < hit_cnt; k += 2u) {
            int32_t sx0 = ARCR_CLAMP((int32_t)js_math_round((double)x_hits[k]), 0, sf->width);
            int32_t sx1 = ARCR_CLAMP((int32_t)js_math_round((double)x_hits[k + 1u]), 0, sf->width);
            for (int32_t x = sx0; x < sx1; ++x) {
                canvas_put_pixel(sf, x, y, ctx->fill_argb);
            }
        }
    }
}

/* =========================================================================
 * 5. Pure C11 JSON.parse / JSON.stringify, PRNG & localStorage Helpers
 * ========================================================================= */
static uint64_t js_next_rand_u64(ArcrJsVm *vm) {
    if (vm->rng_state == 0ULL) {
        uint8_t seed[8] = {0};
        if (arcr_platform_csprng_bytes(seed, sizeof(seed))) {
            memcpy(&vm->rng_state, seed, sizeof(uint64_t));
        }
        if (vm->rng_state == 0ULL) vm->rng_state = 0x9E3779B97F4A7C15ULL;
    }
    uint64_t x = vm->rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    vm->rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

typedef struct JsonBuf {
    char  *buf;
    size_t len;
    size_t cap;
} JsonBuf;

static void jbuf_append(ArcrArena *arena, JsonBuf *jb, const char *s, size_t n) {
    if (n == 0u) return;
    if (jb->len + n + 1u > jb->cap) {
        size_t ncap = (jb->cap == 0u) ? 128u : (jb->cap * 2u);
        while (ncap < jb->len + n + 1u) ncap *= 2u;
        char *nb = (char *)arcr_arena_alloc(arena, ncap, 1u);
        if (!nb) return;
        if (jb->len > 0u && jb->buf) memcpy(nb, jb->buf, jb->len);
        jb->buf = nb;
        jb->cap = ncap;
    }
    memcpy(jb->buf + jb->len, s, n);
    jb->len += n;
    jb->buf[jb->len] = '\0';
}

static void jbuf_append_escaped_str(ArcrArena *arena, JsonBuf *jb, const char *s, size_t len) {
    jbuf_append(arena, jb, "\"", 1u);
    for (size_t i = 0u; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"')       jbuf_append(arena, jb, "\\\"", 2u);
        else if (c == '\\') jbuf_append(arena, jb, "\\\\", 2u);
        else if (c == '\n') jbuf_append(arena, jb, "\\n", 2u);
        else if (c == '\r') jbuf_append(arena, jb, "\\r", 2u);
        else if (c == '\t') jbuf_append(arena, jb, "\\t", 2u);
        else if (c < 0x20u) {
            char u[8];
            snprintf(u, sizeof(u), "\\u%04x", (unsigned)c);
            jbuf_append(arena, jb, u, 6u);
        } else {
            char ch = (char)c;
            jbuf_append(arena, jb, &ch, 1u);
        }
    }
    jbuf_append(arena, jb, "\"", 1u);
}

static void json_stringify_rec(ArcrJsVm *vm, JsVal v, JsonBuf *jb, int depth) {
    if (depth > 16) { jbuf_append(&vm->temp_arena, jb, "null", 4u); return; }
    switch (v.kind) {
        case JS_VAL_NULL:
        case JS_VAL_UNDEFINED:
            jbuf_append(&vm->temp_arena, jb, "null", 4u);
            break;
        case JS_VAL_BOOL:
            jbuf_append(&vm->temp_arena, jb, v.as.b ? "true" : "false", v.as.b ? 4u : 5u);
            break;
        case JS_VAL_NUMBER:
            if (js_num_is_nan(v.as.num) || js_num_is_inf(v.as.num)) {
                jbuf_append(&vm->temp_arena, jb, "null", 4u);
            } else {
                char nb[64];
                js_format_number(v.as.num, nb, sizeof(nb));
                jbuf_append(&vm->temp_arena, jb, nb, strlen(nb));
            }
            break;
        case JS_VAL_STRING:
            jbuf_append_escaped_str(&vm->temp_arena, jb,
                                    v.as.str ? v.as.str->data : "",
                                    v.as.str ? v.as.str->len : 0u);
            break;
        case JS_VAL_ARRAY:
            jbuf_append(&vm->temp_arena, jb, "[", 1u);
            if (v.as.arr) {
                for (size_t i = 0u; i < v.as.arr->len; ++i) {
                    if (i > 0u) jbuf_append(&vm->temp_arena, jb, ",", 1u);
                    json_stringify_rec(vm, v.as.arr->items[i], jb, depth + 1);
                }
            }
            jbuf_append(&vm->temp_arena, jb, "]", 1u);
            break;
        case JS_VAL_OBJECT:
            jbuf_append(&vm->temp_arena, jb, "{", 1u);
            if (v.as.obj) {
                bool first = true;
                for (size_t i = 0u; i < v.as.obj->len; ++i) {
                    JsVal pv = v.as.obj->props[i].val;
                    if (pv.kind == JS_VAL_UNDEFINED || pv.kind == JS_VAL_FUNCTION ||
                        pv.kind == JS_VAL_NATIVE_FN) continue;
                    if (!first) jbuf_append(&vm->temp_arena, jb, ",", 1u);
                    first = false;
                    const char *k = v.as.obj->props[i].key;
                    jbuf_append_escaped_str(&vm->temp_arena, jb, k, strlen(k));
                    jbuf_append(&vm->temp_arena, jb, ":", 1u);
                    json_stringify_rec(vm, pv, jb, depth + 1);
                }
            }
            jbuf_append(&vm->temp_arena, jb, "}", 1u);
            break;
        default:
            jbuf_append(&vm->temp_arena, jb, "null", 4u);
            break;
    }
}

typedef struct JsonParser {
    ArcrJsVm   *vm;
    const char *s;
    size_t      len;
    size_t      pos;
    bool        ok;
} JsonParser;

static void jp_skip_ws(JsonParser *p) {
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p->pos;
        else break;
    }
}

static JsVal jp_parse_value(JsonParser *p, int depth);

static JsVal jp_parse_string(JsonParser *p) {
    if (p->pos >= p->len || p->s[p->pos] != '"') { p->ok = false; return js_undef(); }
    ++p->pos;
    JsonBuf jb = {0};
    while (p->pos < p->len) {
        char c = p->s[p->pos++];
        if (c == '"') {
            return js_mk_str_n(p->vm, jb.buf ? jb.buf : "", jb.len);
        }
        if (c == '\\') {
            if (p->pos >= p->len) break;
            char esc = p->s[p->pos++];
            char outc = esc;
            if (esc == 'n') outc = '\n';
            else if (esc == 'r') outc = '\r';
            else if (esc == 't') outc = '\t';
            else if (esc == 'b') outc = '\b';
            else if (esc == 'f') outc = '\f';
            else if (esc == 'u' && p->pos + 4u <= p->len) {
                uint32_t cp = 0u;
                for (int k = 0; k < 4; ++k) {
                    int h = js_hex_nib(p->s[p->pos + (size_t)k]);
                    if (h < 0) { p->ok = false; return js_undef(); }
                    cp = (cp << 4) | (uint32_t)h;
                }
                p->pos += 4u;
                char u8[4];
                size_t un = arcr_utf8_encode_one(cp, u8);
                jbuf_append(&p->vm->temp_arena, &jb, u8, un);
                continue;
            }
            jbuf_append(&p->vm->temp_arena, &jb, &outc, 1u);
        } else {
            jbuf_append(&p->vm->temp_arena, &jb, &c, 1u);
        }
    }
    p->ok = false;
    return js_undef();
}

static JsVal jp_parse_value(JsonParser *p, int depth) {
    if (!p->ok || depth > 32) { p->ok = false; return js_undef(); }
    jp_skip_ws(p);
    if (p->pos >= p->len) { p->ok = false; return js_undef(); }
    char c = p->s[p->pos];
    if (c == '"') return jp_parse_string(p);
    if (c == '[') {
        ++p->pos;
        JsArray *arr = js_alloc_array(p->vm, 4u);
        if (!arr) { p->ok = false; return js_undef(); }
        jp_skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ']') {
            ++p->pos;
            JsVal v; memset(&v, 0, sizeof(v)); v.kind = JS_VAL_ARRAY; v.as.arr = arr;
            return v;
        }
        while (p->pos < p->len && p->ok) {
            JsVal elem = jp_parse_value(p, depth + 1);
            if (!p->ok) return js_undef();
            js_array_push(p->vm, arr, elem);
            jp_skip_ws(p);
            if (p->pos < p->len && p->s[p->pos] == ',') { ++p->pos; continue; }
            if (p->pos < p->len && p->s[p->pos] == ']') {
                ++p->pos;
                JsVal v; memset(&v, 0, sizeof(v)); v.kind = JS_VAL_ARRAY; v.as.arr = arr;
                return v;
            }
            break;
        }
        p->ok = false;
        return js_undef();
    }
    if (c == '{') {
        ++p->pos;
        JsObject *obj = js_alloc_object(p->vm, 4u);
        if (!obj) { p->ok = false; return js_undef(); }
        jp_skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == '}') {
            ++p->pos;
            JsVal v; memset(&v, 0, sizeof(v)); v.kind = JS_VAL_OBJECT; v.as.obj = obj;
            return v;
        }
        while (p->pos < p->len && p->ok) {
            jp_skip_ws(p);
            JsVal kval = jp_parse_string(p);
            if (!p->ok || kval.kind != JS_VAL_STRING) { p->ok = false; return js_undef(); }
            jp_skip_ws(p);
            if (p->pos >= p->len || p->s[p->pos] != ':') { p->ok = false; return js_undef(); }
            ++p->pos;
            JsVal vval = jp_parse_value(p, depth + 1);
            if (!p->ok) return js_undef();
            js_obj_set(p->vm, obj, kval.as.str->data, vval);
            jp_skip_ws(p);
            if (p->pos < p->len && p->s[p->pos] == ',') { ++p->pos; continue; }
            if (p->pos < p->len && p->s[p->pos] == '}') {
                ++p->pos;
                JsVal v; memset(&v, 0, sizeof(v)); v.kind = JS_VAL_OBJECT; v.as.obj = obj;
                return v;
            }
            break;
        }
        p->ok = false;
        return js_undef();
    }
    if (p->pos + 4u <= p->len && memcmp(p->s + p->pos, "true", 4u) == 0) {
        p->pos += 4u; return js_bool(true);
    }
    if (p->pos + 5u <= p->len && memcmp(p->s + p->pos, "false", 5u) == 0) {
        p->pos += 5u; return js_bool(false);
    }
    if (p->pos + 4u <= p->len && memcmp(p->s + p->pos, "null", 4u) == 0) {
        p->pos += 4u; return js_null();
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t st = p->pos;
        if (p->s[p->pos] == '-') ++p->pos;
        while (p->pos < p->len && ((p->s[p->pos] >= '0' && p->s[p->pos] <= '9') ||
               p->s[p->pos] == '.' || p->s[p->pos] == 'e' || p->s[p->pos] == 'E' ||
               p->s[p->pos] == '+' || p->s[p->pos] == '-')) {
            ++p->pos;
        }
        double num = js_parse_float_str(p->s + st, p->pos - st, false);
        if (js_num_is_nan(num)) { p->ok = false; return js_undef(); }
        return js_num(num);
    }
    p->ok = false;
    return js_undef();
}

/* =========================================================================
 * 6. Lexer & Recursive-Descent / Pratt Bytecode Compiler
 * ========================================================================= */
typedef enum JsTokKind {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_STRING,
    TOK_BACKTICK,
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_SEMI, TOK_COMMA, TOK_DOT, TOK_COLON, TOK_QUESTION, TOK_ARROW,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_INC, TOK_DEC,
    TOK_ASSIGN, TOK_ADD_ASSIGN, TOK_SUB_ASSIGN, TOK_MUL_ASSIGN, TOK_DIV_ASSIGN, TOK_MOD_ASSIGN,
    TOK_EQ, TOK_NEQ, TOK_SEQ, TOK_SNEQ,
    TOK_LT, TOK_LTE, TOK_GT, TOK_GTE,
    TOK_AND, TOK_OR, TOK_NULLISH, TOK_BANG,
    TOK_BIT_AND, TOK_BIT_OR, TOK_BIT_XOR, TOK_SHL, TOK_SHR,
    TOK_KW_VAR, TOK_KW_LET, TOK_KW_CONST, TOK_KW_FUNCTION, TOK_KW_RETURN,
    TOK_KW_IF, TOK_KW_ELSE, TOK_KW_WHILE, TOK_KW_DO, TOK_KW_FOR, TOK_KW_OF,
    TOK_KW_BREAK, TOK_KW_CONTINUE, TOK_KW_TRY, TOK_KW_CATCH, TOK_KW_FINALLY, TOK_KW_THROW,
    TOK_KW_TYPEOF, TOK_KW_NEW, TOK_KW_THIS,
    TOK_KW_TRUE, TOK_KW_FALSE, TOK_KW_NULL, TOK_KW_UNDEFINED, TOK_KW_VOID
} JsTokKind;

typedef struct JsToken {
    JsTokKind   kind;
    const char *start;
    size_t      len;
    double      num_val;
    JsVal       str_val;
    bool        newline_before;
} JsToken;

typedef struct JsLexer {
    ArcrJsVm   *vm;
    const char *src;
    size_t      len;
    size_t      pos;
    JsToken     cur;
} JsLexer;

static bool is_ident_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80u;
}

static bool is_ident_cont(unsigned char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static JsTokKind match_keyword(const char *s, size_t len) {
    static const struct { const char *kw; JsTokKind tk; } kKw[] = {
        {"var", TOK_KW_VAR}, {"let", TOK_KW_LET}, {"const", TOK_KW_CONST},
        {"function", TOK_KW_FUNCTION}, {"return", TOK_KW_RETURN},
        {"if", TOK_KW_IF}, {"else", TOK_KW_ELSE}, {"while", TOK_KW_WHILE},
        {"do", TOK_KW_DO}, {"for", TOK_KW_FOR}, {"of", TOK_KW_OF},
        {"break", TOK_KW_BREAK}, {"continue", TOK_KW_CONTINUE},
        {"try", TOK_KW_TRY}, {"catch", TOK_KW_CATCH}, {"finally", TOK_KW_FINALLY},
        {"throw", TOK_KW_THROW}, {"typeof", TOK_KW_TYPEOF}, {"new", TOK_KW_NEW},
        {"this", TOK_KW_THIS}, {"true", TOK_KW_TRUE}, {"false", TOK_KW_FALSE},
        {"null", TOK_KW_NULL}, {"undefined", TOK_KW_UNDEFINED}, {"void", TOK_KW_VOID}
    };
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kKw); ++i) {
        if (strlen(kKw[i].kw) == len && memcmp(s, kKw[i].kw, len) == 0) return kKw[i].tk;
    }
    return TOK_IDENT;
}

static void lex_next(JsLexer *lx) {
    bool saw_nl = false;
    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        if (c == '\n' || c == '\r') { saw_nl = true; ++lx->pos; continue; }
        if (c == ' ' || c == '\t' || c == '\f' || c == '\v') { ++lx->pos; continue; }
        if (c == '/' && lx->pos + 1u < lx->len && lx->src[lx->pos + 1u] == '/') {
            lx->pos += 2u;
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n') ++lx->pos;
            continue;
        }
        if (c == '/' && lx->pos + 1u < lx->len && lx->src[lx->pos + 1u] == '*') {
            lx->pos += 2u;
            while (lx->pos + 1u < lx->len &&
                   !(lx->src[lx->pos] == '*' && lx->src[lx->pos + 1u] == '/')) {
                if (lx->src[lx->pos] == '\n') saw_nl = true;
                ++lx->pos;
            }
            lx->pos = (lx->pos + 1u < lx->len) ? (lx->pos + 2u) : lx->len;
            continue;
        }
        break;
    }

    JsToken tk;
    memset(&tk, 0, sizeof(tk));
    tk.newline_before = saw_nl;
    tk.start = lx->src + lx->pos;

    if (lx->pos >= lx->len) {
        tk.kind = TOK_EOF;
        lx->cur = tk;
        return;
    }

    unsigned char ch = (unsigned char)lx->src[lx->pos];

    /* Identifiers & keywords */
    if (is_ident_start(ch)) {
        size_t st = lx->pos++;
        while (lx->pos < lx->len && is_ident_cont((unsigned char)lx->src[lx->pos])) ++lx->pos;
        tk.len  = lx->pos - st;
        tk.kind = match_keyword(tk.start, tk.len);
        lx->cur = tk;
        return;
    }

    /* Numeric literals */
    if ((ch >= '0' && ch <= '9') ||
        (ch == '.' && lx->pos + 1u < lx->len && lx->src[lx->pos + 1u] >= '0' && lx->src[lx->pos + 1u] <= '9')) {
        size_t st = lx->pos;
        if (ch == '0' && lx->pos + 1u < lx->len && (lx->src[lx->pos + 1u] == 'x' || lx->src[lx->pos + 1u] == 'X')) {
            lx->pos += 2u;
            while (lx->pos < lx->len && js_hex_nib(lx->src[lx->pos]) >= 0) ++lx->pos;
        } else {
            while (lx->pos < lx->len && lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9') ++lx->pos;
            if (lx->pos < lx->len && lx->src[lx->pos] == '.') {
                ++lx->pos;
                while (lx->pos < lx->len && lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9') ++lx->pos;
            }
            if (lx->pos < lx->len && (lx->src[lx->pos] == 'e' || lx->src[lx->pos] == 'E')) {
                ++lx->pos;
                if (lx->pos < lx->len && (lx->src[lx->pos] == '+' || lx->src[lx->pos] == '-')) ++lx->pos;
                while (lx->pos < lx->len && lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9') ++lx->pos;
            }
        }
        tk.len     = lx->pos - st;
        tk.kind    = TOK_NUMBER;
        tk.num_val = js_parse_float_str(tk.start, tk.len, false);
        lx->cur    = tk;
        return;
    }

    /* Single / Double Quoted String Literals */
    if (ch == '\'' || ch == '"') {
        char quote = (char)ch;
        ++lx->pos;
        JsonBuf sb = {0};
        while (lx->pos < lx->len && lx->src[lx->pos] != quote) {
            char c = lx->src[lx->pos++];
            if (c == '\\' && lx->pos < lx->len) {
                char esc = lx->src[lx->pos++];
                char outc = esc;
                if (esc == 'n') outc = '\n';
                else if (esc == 'r') outc = '\r';
                else if (esc == 't') outc = '\t';
                else if (esc == '0') outc = '\0';
                else if (esc == 'x' && lx->pos + 2u <= lx->len) {
                    int h1 = js_hex_nib(lx->src[lx->pos]), h2 = js_hex_nib(lx->src[lx->pos + 1u]);
                    if (h1 >= 0 && h2 >= 0) {
                        outc = (char)((h1 << 4) | h2);
                        lx->pos += 2u;
                    }
                } else if (esc == 'u' && lx->pos + 4u <= lx->len) {
                    uint32_t cp = 0u;
                    bool ok = true;
                    for (int k = 0; k < 4; ++k) {
                        int h = js_hex_nib(lx->src[lx->pos + (size_t)k]);
                        if (h < 0) { ok = false; break; }
                        cp = (cp << 4) | (uint32_t)h;
                    }
                    if (ok) {
                        lx->pos += 4u;
                        char u8[4];
                        size_t un = arcr_utf8_encode_one(cp, u8);
                        jbuf_append(&lx->vm->temp_arena, &sb, u8, un);
                        continue;
                    }
                }
                jbuf_append(&lx->vm->temp_arena, &sb, &outc, 1u);
            } else {
                jbuf_append(&lx->vm->temp_arena, &sb, &c, 1u);
            }
        }
        if (lx->pos < lx->len && lx->src[lx->pos] == quote) ++lx->pos;
        tk.kind    = TOK_STRING;
        tk.str_val = js_mk_str_n(lx->vm, sb.buf ? sb.buf : "", sb.len);
        lx->cur    = tk;
        return;
    }

    if (ch == '`') {
        ++lx->pos;
        tk.kind = TOK_BACKTICK;
        tk.len  = 1u;
        lx->cur = tk;
        return;
    }

    /* Punctuation & Multi-char operators */
    char n1 = (lx->pos + 1u < lx->len) ? lx->src[lx->pos + 1u] : '\0';
    char n2 = (lx->pos + 2u < lx->len) ? lx->src[lx->pos + 2u] : '\0';
    ++lx->pos;
    tk.len = 1u;

    switch (ch) {
        case '(': tk.kind = TOK_LPAREN; break;
        case ')': tk.kind = TOK_RPAREN; break;
        case '{': tk.kind = TOK_LBRACE; break;
        case '}': tk.kind = TOK_RBRACE; break;
        case '[': tk.kind = TOK_LBRACKET; break;
        case ']': tk.kind = TOK_RBRACKET; break;
        case ';': tk.kind = TOK_SEMI; break;
        case ',': tk.kind = TOK_COMMA; break;
        case '.': tk.kind = TOK_DOT; break;
        case ':': tk.kind = TOK_COLON; break;
        case '^': tk.kind = TOK_BIT_XOR; break;
        case '?':
            if (n1 == '?') { ++lx->pos; tk.len = 2u; tk.kind = TOK_NULLISH; }
            else tk.kind = TOK_QUESTION;
            break;
        case '+':
            if (n1 == '+') { ++lx->pos; tk.len = 2u; tk.kind = TOK_INC; }
            else if (n1 == '=') { ++lx->pos; tk.len = 2u; tk.kind = TOK_ADD_ASSIGN; }
            else tk.kind = TOK_PLUS;
            break;
        case '-':
            if (n1 == '-') { ++lx->pos; tk.len = 2u; tk.kind = TOK_DEC; }
            else if (n1 == '=') { ++lx->pos; tk.len = 2u; tk.kind = TOK_SUB_ASSIGN; }
            else tk.kind = TOK_MINUS;
            break;
        case '*':
            if (n1 == '=') { ++lx->pos; tk.len = 2u; tk.kind = TOK_MUL_ASSIGN; }
            else tk.kind = TOK_STAR;
            break;
        case '/':
            if (n1 == '=') { ++lx->pos; tk.len = 2u; tk.kind = TOK_DIV_ASSIGN; }
            else tk.kind = TOK_SLASH;
            break;
        case '%':
            if (n1 == '=') { ++lx->pos; tk.len = 2u; tk.kind = TOK_MOD_ASSIGN; }
            else tk.kind = TOK_PERCENT;
            break;
        case '=':
            if (n1 == '>' ) { ++lx->pos; tk.len = 2u; tk.kind = TOK_ARROW; }
            else if (n1 == '=' && n2 == '=') { lx->pos += 2u; tk.len = 3u; tk.kind = TOK_SEQ; }
            else if (n1 == '=') { ++lx->pos; tk.len = 2u; tk.kind = TOK_EQ; }
            else tk.kind = TOK_ASSIGN;
            break;
        case '!':
            if (n1 == '=' && n2 == '=') { lx->pos += 2u; tk.len = 3u; tk.kind = TOK_SNEQ; }
            else if (n1 == '=') { ++lx->pos; tk.len = 2u; tk.kind = TOK_NEQ; }
            else tk.kind = TOK_BANG;
            break;
        case '<':
            if (n1 == '=') { ++lx->pos; tk.len = 2u; tk.kind = TOK_LTE; }
            else if (n1 == '<') { ++lx->pos; tk.len = 2u; tk.kind = TOK_SHL; }
            else tk.kind = TOK_LT;
            break;
        case '>':
            if (n1 == '=') { ++lx->pos; tk.len = 2u; tk.kind = TOK_GTE; }
            else if (n1 == '>' && n2 == '>') { lx->pos += 2u; tk.len = 3u; tk.kind = TOK_SHR; }
            else if (n1 == '>') { ++lx->pos; tk.len = 2u; tk.kind = TOK_SHR; }
            else tk.kind = TOK_GT;
            break;
        case '&':
            if (n1 == '&') { ++lx->pos; tk.len = 2u; tk.kind = TOK_AND; }
            else tk.kind = TOK_BIT_AND;
            break;
        case '|':
            if (n1 == '|') { ++lx->pos; tk.len = 2u; tk.kind = TOK_OR; }
            else tk.kind = TOK_BIT_OR;
            break;
        default:
            tk.kind = TOK_SEMI; /* Tolerate stray char cleanly */
            break;
    }
    lx->cur = tk;
}

/* Compiler Structures */
typedef struct JsLocalVar {
    const char *name;
    size_t      len;
    int         scope_depth;
} JsLocalVar;

typedef struct JsLoopPatch {
    size_t continue_ip;
    size_t break_patches[32];
    size_t break_count;
    size_t cont_patches[32];
    size_t cont_count;
    bool   cont_uses_patches;
} JsLoopPatch;

typedef struct JsCompiler {
    ArcrJsVm          *vm;
    JsLexer           *lx;
    struct JsCompiler *parent;
    JsChunk           *chunk;
    bool               is_function;
    int                scope_depth;
    JsLocalVar         locals[JS_MAX_LOCALS];
    uint16_t           local_count;
    JsLoopPatch        loops[16];
    size_t             loop_depth;
    bool               had_error;
} JsCompiler;

static JsChunk *alloc_chunk(ArcrJsVm *vm) {
    JsChunk *ch = (JsChunk *)vm_alloc(vm, sizeof(JsChunk), sizeof(void *));
    if (!ch) return NULL;
    memset(ch, 0, sizeof(*ch));
    return ch;
}

static uint32_t chunk_add_const(ArcrJsVm *vm, JsChunk *ch, JsVal val) {
    if (val.kind == JS_VAL_STRING && val.as.str) {
        for (size_t i = 0u; i < ch->const_len; ++i) {
            if (ch->consts[i].kind == JS_VAL_STRING && ch->consts[i].as.str &&
                ch->consts[i].as.str->len == val.as.str->len &&
                memcmp(ch->consts[i].as.str->data, val.as.str->data, val.as.str->len) == 0) {
                return (uint32_t)i;
            }
        }
    }
    if (ch->const_len >= ch->const_cap) {
        size_t ncap = (ch->const_cap == 0u) ? 16u : (ch->const_cap * 2u);
        JsVal *nc = (JsVal *)vm_alloc(vm, ncap * sizeof(JsVal), sizeof(void *));
        if (!nc) return 0u;
        if (ch->const_len > 0u) memcpy(nc, ch->consts, ch->const_len * sizeof(JsVal));
        ch->consts    = nc;
        ch->const_cap = ncap;
    }
    ch->consts[ch->const_len] = val;
    return (uint32_t)(ch->const_len++);
}

static size_t emit_op(JsCompiler *c, JsOpCode op, uint8_t aux, uint32_t arg) {
    JsChunk *ch = c->chunk;
    if (ch->code_len >= ch->code_cap) {
        size_t ncap = (ch->code_cap == 0u) ? 64u : (ch->code_cap * 2u);
        JsInst *ni = (JsInst *)vm_alloc(c->vm, ncap * sizeof(JsInst), sizeof(void *));
        if (!ni) { c->had_error = true; return 0u; }
        if (ch->code_len > 0u) memcpy(ni, ch->code, ch->code_len * sizeof(JsInst));
        ch->code     = ni;
        ch->code_cap = ncap;
        c->vm->bytecode_cache_bytes += (ncap / 2u) * sizeof(JsInst);
    }
    ch->code[ch->code_len].op  = (uint8_t)op;
    ch->code[ch->code_len].aux = aux;
    ch->code[ch->code_len].arg = arg;
    return ch->code_len++;
}

static void patch_jump(JsCompiler *c, size_t inst_idx) {
    if (inst_idx < c->chunk->code_len) {
        c->chunk->code[inst_idx].arg = (uint32_t)c->chunk->code_len;
    }
}

static bool resolve_lexical(JsCompiler *c, const char *name, size_t len, uint32_t *out_encoded) {
    uint16_t depth = 0u;
    for (JsCompiler *cur = c; cur != NULL; cur = cur->parent, ++depth) {
        for (int i = (int)cur->local_count - 1; i >= 0; --i) {
            if (cur->locals[i].len == len && memcmp(cur->locals[i].name, name, len) == 0) {
                *out_encoded = ((uint32_t)depth << 16) | (uint32_t)i;
                return true;
            }
        }
    }
    return false;
}

static int declare_local(JsCompiler *c, const char *name, size_t len) {
    if (c->local_count >= JS_MAX_LOCALS) { c->had_error = true; return 0; }
    int slot = (int)c->local_count++;
    c->locals[slot].name        = name;
    c->locals[slot].len         = len;
    c->locals[slot].scope_depth = c->scope_depth;
    if (c->local_count > c->chunk->local_count) {
        c->chunk->local_count = c->local_count;
    }
    return slot;
}

static void pop_locals_to_depth(JsCompiler *c, int target_depth) {
    while (c->local_count > 0u && c->locals[c->local_count - 1u].scope_depth > target_depth) {
        --c->local_count;
    }
}

static bool match_tok(JsCompiler *c, JsTokKind k) {
    if (c->lx->cur.kind == k) { lex_next(c->lx); return true; }
    return false;
}

static void consume_semi_optional(JsCompiler *c) {
    if (c->lx->cur.kind == TOK_SEMI) lex_next(c->lx);
}

static void compile_expr(JsCompiler *c);
static void compile_assign(JsCompiler *c);
static void compile_stmt(JsCompiler *c);

static void compile_function_body(JsCompiler *outer, const char *params_src[], size_t params_len[],
                                  size_t param_cnt, bool concise_expr_body) {
    JsChunk *fn_chunk = alloc_chunk(outer->vm);
    if (!fn_chunk) { outer->had_error = true; return; }
    fn_chunk->param_count = (uint16_t)param_cnt;

    JsCompiler sub;
    memset(&sub, 0, sizeof(sub));
    sub.vm          = outer->vm;
    sub.lx          = outer->lx;
    sub.parent      = outer;
    sub.chunk       = fn_chunk;
    sub.is_function = true;
    sub.scope_depth = 1;

    for (size_t i = 0u; i < param_cnt; ++i) {
        declare_local(&sub, params_src[i], params_len[i]);
    }

    if (concise_expr_body) {
        compile_assign(&sub);
        emit_op(&sub, OP_RETURN, 0u, 0u);
    } else {
        match_tok(&sub, TOK_LBRACE);
        while (sub.lx->cur.kind != TOK_RBRACE && sub.lx->cur.kind != TOK_EOF && !sub.had_error) {
            compile_stmt(&sub);
        }
        match_tok(&sub, TOK_RBRACE);
        uint32_t u_idx = chunk_add_const(sub.vm, fn_chunk, js_undef());
        emit_op(&sub, OP_CONST, 0u, u_idx);
        emit_op(&sub, OP_RETURN, 0u, 0u);
    }
    if (sub.had_error) outer->had_error = true;

    JsFunc *proto = (JsFunc *)vm_alloc(outer->vm, sizeof(JsFunc), sizeof(void *));
    if (!proto) { outer->had_error = true; return; }
    proto->chunk        = fn_chunk;
    proto->captured_env = NULL;

    JsVal fv; memset(&fv, 0, sizeof(fv));
    fv.kind  = JS_VAL_FUNCTION;
    fv.as.fn = proto;
    uint32_t cidx = chunk_add_const(outer->vm, outer->chunk, fv);
    emit_op(outer, OP_CLOSURE, 0u, cidx);
}

/* Lookahead helper: checks if current '(' starts an arrow function `(a, b) => ...` */
static bool is_paren_arrow_ahead(const JsLexer *lx_in) {
    JsLexer probe = *lx_in;
    if (probe.cur.kind != TOK_LPAREN) return false;
    lex_next(&probe);
    int depth = 1;
    while (probe.cur.kind != TOK_EOF && depth > 0) {
        if (probe.cur.kind == TOK_LPAREN) ++depth;
        else if (probe.cur.kind == TOK_RPAREN) --depth;
        lex_next(&probe);
    }
    return probe.cur.kind == TOK_ARROW;
}

static void compile_template_literal(JsCompiler *c) {
    /* Lexer just consumed opening '`'; lx->pos points to first template character */
    JsLexer *lx = c->lx;
    uint32_t empty_idx = chunk_add_const(c->vm, c->chunk, js_mk_str_n(c->vm, "", 0u));
    emit_op(c, OP_CONST, 0u, empty_idx);

    JsonBuf seg = {0};
    while (lx->pos < lx->len) {
        char ch = lx->src[lx->pos];
        if (ch == '`') {
            ++lx->pos;
            if (seg.len > 0u) {
                uint32_t sidx = chunk_add_const(c->vm, c->chunk, js_mk_str_n(c->vm, seg.buf, seg.len));
                emit_op(c, OP_CONST, 0u, sidx);
                emit_op(c, OP_ADD, 0u, 0u);
            }
            lex_next(lx);
            return;
        }
        if (ch == '\\' && lx->pos + 1u < lx->len) {
            ++lx->pos;
            char esc = lx->src[lx->pos++];
            char outc = (esc == 'n') ? '\n' : (esc == 'r') ? '\r' : (esc == 't') ? '\t' : esc;
            jbuf_append(&c->vm->temp_arena, &seg, &outc, 1u);
            continue;
        }
        if (ch == '$' && lx->pos + 1u < lx->len && lx->src[lx->pos + 1u] == '{') {
            if (seg.len > 0u) {
                uint32_t sidx = chunk_add_const(c->vm, c->chunk, js_mk_str_n(c->vm, seg.buf, seg.len));
                emit_op(c, OP_CONST, 0u, sidx);
                emit_op(c, OP_ADD, 0u, 0u);
                seg.len = 0u;
            }
            lx->pos += 2u;
            size_t expr_start = lx->pos;
            int bdepth = 1;
            while (lx->pos < lx->len && bdepth > 0) {
                char ec = lx->src[lx->pos];
                if (ec == '\'' || ec == '"') {
                    char q = ec; ++lx->pos;
                    while (lx->pos < lx->len && lx->src[lx->pos] != q) {
                        if (lx->src[lx->pos] == '\\') ++lx->pos;
                        if (lx->pos < lx->len) ++lx->pos;
                    }
                    if (lx->pos < lx->len) ++lx->pos;
                    continue;
                }
                if (ec == '{') ++bdepth;
                else if (ec == '}') --bdepth;
                if (bdepth > 0) ++lx->pos;
            }
            size_t expr_len = lx->pos - expr_start;
            if (lx->pos < lx->len && lx->src[lx->pos] == '}') ++lx->pos;

            JsLexer sub_lx;
            memset(&sub_lx, 0, sizeof(sub_lx));
            sub_lx.vm  = c->vm;
            sub_lx.src = lx->src + expr_start;
            sub_lx.len = expr_len;
            lex_next(&sub_lx);

            JsLexer *saved = c->lx;
            c->lx = &sub_lx;
            compile_expr(c);
            c->lx = saved;
            emit_op(c, OP_ADD, 0u, 0u);
            continue;
        }
        jbuf_append(&c->vm->temp_arena, &seg, &ch, 1u);
        ++lx->pos;
    }
    lex_next(lx);
}

static uint8_t compile_arg_list(JsCompiler *c) {
    uint8_t argc = 0u;
    if (c->lx->cur.kind != TOK_RPAREN) {
        do {
            if (c->lx->cur.kind == TOK_RPAREN) break;
            compile_assign(c);
            if (argc < 250u) ++argc;
        } while (match_tok(c, TOK_COMMA));
    }
    match_tok(c, TOK_RPAREN);
    return argc;
}

static void compile_primary(JsCompiler *c) {
    JsToken tk = c->lx->cur;
    switch (tk.kind) {
        case TOK_NUMBER: {
            lex_next(c->lx);
            uint32_t idx = chunk_add_const(c->vm, c->chunk, js_num(tk.num_val));
            emit_op(c, OP_CONST, 0u, idx);
            return;
        }
        case TOK_STRING: {
            lex_next(c->lx);
            uint32_t idx = chunk_add_const(c->vm, c->chunk, tk.str_val);
            emit_op(c, OP_CONST, 0u, idx);
            return;
        }
        case TOK_BACKTICK:
            compile_template_literal(c);
            return;
        case TOK_KW_TRUE: {
            lex_next(c->lx);
            uint32_t idx = chunk_add_const(c->vm, c->chunk, js_bool(true));
            emit_op(c, OP_CONST, 0u, idx);
            return;
        }
        case TOK_KW_FALSE: {
            lex_next(c->lx);
            uint32_t idx = chunk_add_const(c->vm, c->chunk, js_bool(false));
            emit_op(c, OP_CONST, 0u, idx);
            return;
        }
        case TOK_KW_NULL: {
            lex_next(c->lx);
            uint32_t idx = chunk_add_const(c->vm, c->chunk, js_null());
            emit_op(c, OP_CONST, 0u, idx);
            return;
        }
        case TOK_KW_UNDEFINED: {
            lex_next(c->lx);
            uint32_t idx = chunk_add_const(c->vm, c->chunk, js_undef());
            emit_op(c, OP_CONST, 0u, idx);
            return;
        }
        case TOK_KW_THIS:
            lex_next(c->lx);
            emit_op(c, OP_THIS, 0u, 0u);
            return;
        case TOK_KW_FUNCTION: {
            lex_next(c->lx);
            if (c->lx->cur.kind == TOK_IDENT) lex_next(c->lx); /* optional expr name */
            match_tok(c, TOK_LPAREN);
            const char *ps[16];
            size_t pl[16];
            size_t pcnt = 0u;
            while (c->lx->cur.kind == TOK_IDENT && pcnt < 16u) {
                ps[pcnt] = c->lx->cur.start;
                pl[pcnt] = c->lx->cur.len;
                ++pcnt;
                lex_next(c->lx);
                if (!match_tok(c, TOK_COMMA)) break;
            }
            match_tok(c, TOK_RPAREN);
            compile_function_body(c, ps, pl, pcnt, false);
            return;
        }
        case TOK_LPAREN: {
            if (is_paren_arrow_ahead(c->lx)) {
                lex_next(c->lx); /* '(' */
                const char *ps[16];
                size_t pl[16];
                size_t pcnt = 0u;
                while (c->lx->cur.kind == TOK_IDENT && pcnt < 16u) {
                    ps[pcnt] = c->lx->cur.start;
                    pl[pcnt] = c->lx->cur.len;
                    ++pcnt;
                    lex_next(c->lx);
                    if (!match_tok(c, TOK_COMMA)) break;
                }
                match_tok(c, TOK_RPAREN);
                match_tok(c, TOK_ARROW);
                bool concise = (c->lx->cur.kind != TOK_LBRACE);
                compile_function_body(c, ps, pl, pcnt, concise);
                return;
            }
            lex_next(c->lx);
            compile_expr(c);
            match_tok(c, TOK_RPAREN);
            return;
        }
        case TOK_LBRACKET: {
            lex_next(c->lx);
            uint32_t cnt = 0u;
            while (c->lx->cur.kind != TOK_RBRACKET && c->lx->cur.kind != TOK_EOF) {
                compile_assign(c);
                ++cnt;
                if (!match_tok(c, TOK_COMMA)) break;
            }
            match_tok(c, TOK_RBRACKET);
            emit_op(c, OP_NEW_ARRAY, 0u, cnt);
            return;
        }
        case TOK_LBRACE: {
            lex_next(c->lx);
            uint32_t pairs = 0u;
            while (c->lx->cur.kind != TOK_RBRACE && c->lx->cur.kind != TOK_EOF) {
                JsToken kt = c->lx->cur;
                JsVal key_str = js_undef();
                if (kt.kind == TOK_STRING) {
                    key_str = kt.str_val;
                    lex_next(c->lx);
                } else if (kt.kind == TOK_NUMBER) {
                    char nb[64];
                    js_format_number(kt.num_val, nb, sizeof(nb));
                    key_str = js_mk_str(c->vm, nb);
                    lex_next(c->lx);
                } else {
                    /* Identifier or keyword property name */
                    key_str = js_mk_str_n(c->vm, kt.start, kt.len);
                    lex_next(c->lx);
                }
                uint32_t kidx = chunk_add_const(c->vm, c->chunk, key_str);
                emit_op(c, OP_CONST, 0u, kidx);

                if (match_tok(c, TOK_COLON)) {
                    compile_assign(c);
                } else if (c->lx->cur.kind == TOK_LPAREN) {
                    /* Method shorthand: { foo(a) { ... } } */
                    lex_next(c->lx);
                    const char *ps[16]; size_t pl[16]; size_t pcnt = 0u;
                    while (c->lx->cur.kind == TOK_IDENT && pcnt < 16u) {
                        ps[pcnt] = c->lx->cur.start; pl[pcnt] = c->lx->cur.len; ++pcnt;
                        lex_next(c->lx);
                        if (!match_tok(c, TOK_COMMA)) break;
                    }
                    match_tok(c, TOK_RPAREN);
                    compile_function_body(c, ps, pl, pcnt, false);
                } else {
                    /* Shorthand property: { x } */
                    uint32_t enc = 0u;
                    if (resolve_lexical(c, kt.start, kt.len, &enc)) {
                        emit_op(c, OP_GET_LOCAL, 0u, enc);
                    } else {
                        emit_op(c, OP_GET_GLOBAL, 0u, kidx);
                    }
                }
                ++pairs;
                if (!match_tok(c, TOK_COMMA)) break;
            }
            match_tok(c, TOK_RBRACE);
            emit_op(c, OP_NEW_OBJECT, 0u, pairs);
            return;
        }
        case TOK_IDENT:
        case TOK_KW_OF: {
            lex_next(c->lx);
            /* Single-param arrow function: `x => expr` */
            if (c->lx->cur.kind == TOK_ARROW) {
                lex_next(c->lx);
                const char *ps[1] = { tk.start };
                size_t pl[1]      = { tk.len };
                bool concise      = (c->lx->cur.kind != TOK_LBRACE);
                compile_function_body(c, ps, pl, 1u, concise);
                return;
            }
            uint32_t enc = 0u;
            if (resolve_lexical(c, tk.start, tk.len, &enc)) {
                emit_op(c, OP_GET_LOCAL, 0u, enc);
            } else {
                uint32_t gidx = chunk_add_const(c->vm, c->chunk, js_mk_str_n(c->vm, tk.start, tk.len));
                emit_op(c, OP_GET_GLOBAL, 0u, gidx);
            }
            return;
        }
        default: {
            lex_next(c->lx);
            uint32_t idx = chunk_add_const(c->vm, c->chunk, js_undef());
            emit_op(c, OP_CONST, 0u, idx);
            return;
        }
    }
}

/* Helper to convert the just-emitted lvalue load at top of `chunk->code` into an assignment */
static JsOpCode map_compound_op(JsTokKind tk) {
    switch (tk) {
        case TOK_ADD_ASSIGN: return OP_ADD;
        case TOK_SUB_ASSIGN: return OP_SUB;
        case TOK_MUL_ASSIGN: return OP_MUL;
        case TOK_DIV_ASSIGN: return OP_DIV;
        case TOK_MOD_ASSIGN: return OP_MOD;
        default: return OP_ADD;
    }
}

static void compile_postfix_and_call(JsCompiler *c) {
    compile_primary(c);
    for (;;) {
        if (match_tok(c, TOK_DOT)) {
            JsToken prop = c->lx->cur;
            lex_next(c->lx);
            uint32_t pidx = chunk_add_const(c->vm, c->chunk, js_mk_str_n(c->vm, prop.start, prop.len));
            if (match_tok(c, TOK_LPAREN)) {
                uint8_t argc = compile_arg_list(c);
                emit_op(c, OP_CALL_METHOD, argc, pidx);
            } else {
                emit_op(c, OP_GET_PROP, 0u, pidx);
            }
        } else if (match_tok(c, TOK_LBRACKET)) {
            compile_expr(c);
            match_tok(c, TOK_RBRACKET);
            emit_op(c, OP_GET_INDEX, 0u, 0u);
        } else if (match_tok(c, TOK_LPAREN)) {
            uint8_t argc = compile_arg_list(c);
            emit_op(c, OP_CALL, argc, argc);
        } else if (!c->lx->cur.newline_before &&
                   (c->lx->cur.kind == TOK_INC || c->lx->cur.kind == TOK_DEC)) {
            bool is_inc = (c->lx->cur.kind == TOK_INC);
            lex_next(c->lx);
            if (c->chunk->code_len == 0u) break;
            JsInst last = c->chunk->code[c->chunk->code_len - 1u];
            uint32_t one_idx = chunk_add_const(c->vm, c->chunk, js_num(1.0));
            if (last.op == OP_GET_LOCAL || last.op == OP_GET_GLOBAL) {
                emit_op(c, OP_POS, 0u, 0u);
                emit_op(c, OP_DUP, 0u, 0u);
                emit_op(c, OP_CONST, 0u, one_idx);
                emit_op(c, is_inc ? OP_ADD : OP_SUB, 0u, 0u);
                emit_op(c, (last.op == OP_GET_LOCAL) ? OP_SET_LOCAL : OP_SET_GLOBAL, 0u, last.arg);
                emit_op(c, OP_POP, 0u, 0u);
            } else if (last.op == OP_GET_PROP) {
                c->chunk->code_len--; /* pop OP_GET_PROP; receiver is on stack */
                emit_op(c, OP_DUP, 0u, 0u);
                emit_op(c, OP_GET_PROP, 0u, last.arg);
                emit_op(c, OP_POS, 0u, 0u);
                emit_op(c, OP_SWAP, 0u, 0u); /* [old_num, obj] */
                emit_op(c, OP_DUP2, 0u, 0u); /* [old_num, obj, old_num, obj] */
                emit_op(c, OP_POP, 0u, 0u);  /* [old_num, obj, old_num] */
                emit_op(c, OP_CONST, 0u, one_idx);
                emit_op(c, is_inc ? OP_ADD : OP_SUB, 0u, 0u);
                emit_op(c, OP_SET_PROP, 0u, last.arg);
                emit_op(c, OP_POP, 0u, 0u);
            } else if (last.op == OP_GET_INDEX) {
                c->chunk->code_len--; /* [obj, idx] on stack */
                emit_op(c, OP_DUP2, 0u, 0u);
                emit_op(c, OP_GET_INDEX, 0u, 0u);
                emit_op(c, OP_POS, 0u, 0u);
                emit_op(c, OP_CONST, 0u, one_idx);
                emit_op(c, is_inc ? OP_ADD : OP_SUB, 0u, 0u);
                emit_op(c, OP_SET_INDEX, 0u, 0u);
                emit_op(c, OP_CONST, 0u, one_idx);
                emit_op(c, is_inc ? OP_SUB : OP_ADD, 0u, 0u); /* return previous value */
            }
            break;
        } else {
            break;
        }
    }
}

static void compile_unary(JsCompiler *c) {
    JsTokKind k = c->lx->cur.kind;
    if (k == TOK_BANG || k == TOK_MINUS || k == TOK_PLUS || k == TOK_KW_TYPEOF || k == TOK_KW_VOID) {
        lex_next(c->lx);
        if (k == TOK_KW_TYPEOF && c->lx->cur.kind == TOK_IDENT) {
            /* Safe `typeof undeclaredVar` -> "undefined" */
            JsToken id = c->lx->cur;
            uint32_t enc = 0u;
            if (!resolve_lexical(c, id.start, id.len, &enc)) {
                JsLexer save = *c->lx;
                lex_next(&save);
                if (save.cur.kind != TOK_DOT && save.cur.kind != TOK_LBRACKET && save.cur.kind != TOK_LPAREN) {
                    *c->lx = save;
                    uint32_t gidx = chunk_add_const(c->vm, c->chunk, js_mk_str_n(c->vm, id.start, id.len));
                    emit_op(c, OP_GET_GLOBAL, 1u /* silent_undef */, gidx);
                    emit_op(c, OP_TYPEOF, 0u, 0u);
                    return;
                }
            }
        }
        compile_unary(c);
        if (k == TOK_BANG)      emit_op(c, OP_NOT, 0u, 0u);
        else if (k == TOK_MINUS)emit_op(c, OP_NEG, 0u, 0u);
        else if (k == TOK_PLUS) emit_op(c, OP_POS, 0u, 0u);
        else if (k == TOK_KW_TYPEOF) emit_op(c, OP_TYPEOF, 0u, 0u);
        else if (k == TOK_KW_VOID) {
            emit_op(c, OP_POP, 0u, 0u);
            emit_op(c, OP_CONST, 0u, chunk_add_const(c->vm, c->chunk, js_undef()));
        }
        return;
    }
    if (k == TOK_INC || k == TOK_DEC) {
        bool is_inc = (k == TOK_INC);
        lex_next(c->lx);
        compile_postfix_and_call(c);
        if (c->chunk->code_len > 0u) {
            JsInst last = c->chunk->code[c->chunk->code_len - 1u];
            uint32_t one_idx = chunk_add_const(c->vm, c->chunk, js_num(1.0));
            if (last.op == OP_GET_LOCAL || last.op == OP_GET_GLOBAL) {
                emit_op(c, OP_CONST, 0u, one_idx);
                emit_op(c, is_inc ? OP_ADD : OP_SUB, 0u, 0u);
                emit_op(c, (last.op == OP_GET_LOCAL) ? OP_SET_LOCAL : OP_SET_GLOBAL, 0u, last.arg);
            } else if (last.op == OP_GET_PROP) {
                c->chunk->code_len--;
                emit_op(c, OP_DUP, 0u, 0u);
                emit_op(c, OP_GET_PROP, 0u, last.arg);
                emit_op(c, OP_CONST, 0u, one_idx);
                emit_op(c, is_inc ? OP_ADD : OP_SUB, 0u, 0u);
                emit_op(c, OP_SET_PROP, 0u, last.arg);
            } else if (last.op == OP_GET_INDEX) {
                c->chunk->code_len--;
                emit_op(c, OP_DUP2, 0u, 0u);
                emit_op(c, OP_GET_INDEX, 0u, 0u);
                emit_op(c, OP_CONST, 0u, one_idx);
                emit_op(c, is_inc ? OP_ADD : OP_SUB, 0u, 0u);
                emit_op(c, OP_SET_INDEX, 0u, 0u);
            }
        }
        return;
    }
    if (match_tok(c, TOK_KW_NEW)) {
        compile_primary(c);
        while (match_tok(c, TOK_DOT)) {
            JsToken prop = c->lx->cur; lex_next(c->lx);
            emit_op(c, OP_GET_PROP, 0u, chunk_add_const(c->vm, c->chunk, js_mk_str_n(c->vm, prop.start, prop.len)));
        }
        uint8_t argc = 0u;
        if (match_tok(c, TOK_LPAREN)) argc = compile_arg_list(c);
        emit_op(c, OP_CALL, 1u /* is_new */, argc);
        return;
    }
    compile_postfix_and_call(c);
}

static int binop_prec(JsTokKind k) {
    switch (k) {
        case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: return 10;
        case TOK_PLUS: case TOK_MINUS: return 9;
        case TOK_SHL:  case TOK_SHR:   return 8;
        case TOK_LT:   case TOK_LTE: case TOK_GT: case TOK_GTE: return 7;
        case TOK_EQ:   case TOK_NEQ: case TOK_SEQ: case TOK_SNEQ: return 6;
        case TOK_BIT_AND: return 5;
        case TOK_BIT_XOR: return 4;
        case TOK_BIT_OR:  return 3;
        case TOK_AND:     return 2;
        case TOK_OR: case TOK_NULLISH: return 1;
        default: return 0;
    }
}

static void compile_binary_prec(JsCompiler *c, int min_prec) {
    compile_unary(c);
    for (;;) {
        JsTokKind op = c->lx->cur.kind;
        int prec = binop_prec(op);
        if (prec < min_prec || prec == 0) break;
        lex_next(c->lx);

        if (op == TOK_AND) {
            size_t j_end = emit_op(c, OP_JMP_IF_FALSE, 0u, 0u);
            emit_op(c, OP_POP, 0u, 0u);
            compile_binary_prec(c, prec + 1);
            patch_jump(c, j_end);
            continue;
        }
        if (op == TOK_OR || op == TOK_NULLISH) {
            size_t j_end = emit_op(c, OP_JMP_IF_TRUE, 0u, 0u);
            emit_op(c, OP_POP, 0u, 0u);
            compile_binary_prec(c, prec + 1);
            patch_jump(c, j_end);
            continue;
        }

        compile_binary_prec(c, prec + 1);
        switch (op) {
            case TOK_STAR:    emit_op(c, OP_MUL, 0u, 0u); break;
            case TOK_SLASH:   emit_op(c, OP_DIV, 0u, 0u); break;
            case TOK_PERCENT: emit_op(c, OP_MOD, 0u, 0u); break;
            case TOK_PLUS:    emit_op(c, OP_ADD, 0u, 0u); break;
            case TOK_MINUS:   emit_op(c, OP_SUB, 0u, 0u); break;
            case TOK_SHL:     emit_op(c, OP_SHL, 0u, 0u); break;
            case TOK_SHR:     emit_op(c, OP_SHR, 0u, 0u); break;
            case TOK_LT:      emit_op(c, OP_LT,  0u, 0u); break;
            case TOK_LTE:     emit_op(c, OP_LTE, 0u, 0u); break;
            case TOK_GT:      emit_op(c, OP_GT,  0u, 0u); break;
            case TOK_GTE:     emit_op(c, OP_GTE, 0u, 0u); break;
            case TOK_EQ:      emit_op(c, OP_EQ,  0u, 0u); break;
            case TOK_NEQ:     emit_op(c, OP_NEQ, 0u, 0u); break;
            case TOK_SEQ:     emit_op(c, OP_STRICT_EQ, 0u, 0u); break;
            case TOK_SNEQ:    emit_op(c, OP_STRICT_NEQ, 0u, 0u); break;
            case TOK_BIT_AND: emit_op(c, OP_BIT_AND, 0u, 0u); break;
            case TOK_BIT_XOR: emit_op(c, OP_BIT_XOR, 0u, 0u); break;
            case TOK_BIT_OR:  emit_op(c, OP_BIT_OR,  0u, 0u); break;
            default: break;
        }
    }
}

static bool is_assign_tok(JsTokKind k) {
    return k == TOK_ASSIGN || k == TOK_ADD_ASSIGN || k == TOK_SUB_ASSIGN ||
           k == TOK_MUL_ASSIGN || k == TOK_DIV_ASSIGN || k == TOK_MOD_ASSIGN;
}

static void compile_assign(JsCompiler *c) {
    compile_binary_prec(c, 1);

    if (match_tok(c, TOK_QUESTION)) {
        size_t j_false = emit_op(c, OP_JMP_IF_FALSE, 0u, 0u);
        emit_op(c, OP_POP, 0u, 0u);
        compile_assign(c);
        size_t j_done = emit_op(c, OP_JMP, 0u, 0u);
        patch_jump(c, j_false);
        emit_op(c, OP_POP, 0u, 0u);
        match_tok(c, TOK_COLON);
        compile_assign(c);
        patch_jump(c, j_done);
        return;
    }

    JsTokKind ak = c->lx->cur.kind;
    if (is_assign_tok(ak) && c->chunk->code_len > 0u) {
        lex_next(c->lx);
        JsInst lhs = c->chunk->code[c->chunk->code_len - 1u];
        if (ak == TOK_ASSIGN) {
            c->chunk->code_len--;
            compile_assign(c);
            if (lhs.op == OP_GET_LOCAL)       emit_op(c, OP_SET_LOCAL, 0u, lhs.arg);
            else if (lhs.op == OP_GET_GLOBAL) emit_op(c, OP_SET_GLOBAL, 0u, lhs.arg);
            else if (lhs.op == OP_GET_PROP)   emit_op(c, OP_SET_PROP, 0u, lhs.arg);
            else if (lhs.op == OP_GET_INDEX)  emit_op(c, OP_SET_INDEX, 0u, 0u);
        } else {
            JsOpCode math_op = map_compound_op(ak);
            if (lhs.op == OP_GET_LOCAL || lhs.op == OP_GET_GLOBAL) {
                compile_assign(c);
                emit_op(c, math_op, 0u, 0u);
                emit_op(c, (lhs.op == OP_GET_LOCAL) ? OP_SET_LOCAL : OP_SET_GLOBAL, 0u, lhs.arg);
            } else if (lhs.op == OP_GET_PROP) {
                c->chunk->code_len--;
                emit_op(c, OP_DUP, 0u, 0u);
                emit_op(c, OP_GET_PROP, 0u, lhs.arg);
                compile_assign(c);
                emit_op(c, math_op, 0u, 0u);
                emit_op(c, OP_SET_PROP, 0u, lhs.arg);
            } else if (lhs.op == OP_GET_INDEX) {
                c->chunk->code_len--;
                emit_op(c, OP_DUP2, 0u, 0u);
                emit_op(c, OP_GET_INDEX, 0u, 0u);
                compile_assign(c);
                emit_op(c, math_op, 0u, 0u);
                emit_op(c, OP_SET_INDEX, 0u, 0u);
            }
        }
    }
}

static void compile_expr(JsCompiler *c) {
    compile_assign(c);
    while (match_tok(c, TOK_COMMA)) {
        emit_op(c, OP_POP, 0u, 0u);
        compile_assign(c);
    }
}

static void compile_var_decl(JsCompiler *c, bool consume_semi) {
    lex_next(c->lx); /* var / let / const */
    do {
        if (c->lx->cur.kind != TOK_IDENT) break;
        JsToken id = c->lx->cur;
        lex_next(c->lx);

        bool use_local = (c->is_function || c->scope_depth > 0);
        int slot = -1;
        uint32_t gidx = 0u;
        if (use_local) {
            slot = declare_local(c, id.start, id.len);
        } else {
            gidx = chunk_add_const(c->vm, c->chunk, js_mk_str_n(c->vm, id.start, id.len));
        }

        if (match_tok(c, TOK_ASSIGN)) {
            compile_assign(c);
        } else {
            emit_op(c, OP_CONST, 0u, chunk_add_const(c->vm, c->chunk, js_undef()));
        }

        if (use_local) {
            emit_op(c, OP_SET_LOCAL, 0u, (uint32_t)slot);
            emit_op(c, OP_POP, 0u, 0u);
        } else {
            emit_op(c, OP_SET_GLOBAL, 0u, gidx);
            emit_op(c, OP_POP, 0u, 0u);
        }
    } while (match_tok(c, TOK_COMMA));
    if (consume_semi) consume_semi_optional(c);
}

static void compile_stmt(JsCompiler *c) {
    if (c->had_error) return;
    JsTokKind k = c->lx->cur.kind;

    if (k == TOK_SEMI) { lex_next(c->lx); return; }

    if (k == TOK_LBRACE) {
        lex_next(c->lx);
        int prev_depth = c->scope_depth++;
        while (c->lx->cur.kind != TOK_RBRACE && c->lx->cur.kind != TOK_EOF && !c->had_error) {
            compile_stmt(c);
        }
        match_tok(c, TOK_RBRACE);
        pop_locals_to_depth(c, prev_depth);
        c->scope_depth = prev_depth;
        return;
    }

    if (k == TOK_KW_VAR || k == TOK_KW_LET || k == TOK_KW_CONST) {
        compile_var_decl(c, true);
        return;
    }

    if (k == TOK_KW_FUNCTION) {
        lex_next(c->lx);
        JsToken name = c->lx->cur;
        bool has_name = (name.kind == TOK_IDENT);
        if (has_name) lex_next(c->lx);

        bool use_local = (c->is_function || c->scope_depth > 0);
        int slot = -1;
        uint32_t gidx = 0u;
        if (has_name) {
            if (use_local) slot = declare_local(c, name.start, name.len);
            else gidx = chunk_add_const(c->vm, c->chunk, js_mk_str_n(c->vm, name.start, name.len));
        }

        match_tok(c, TOK_LPAREN);
        const char *ps[16]; size_t pl[16]; size_t pcnt = 0u;
        while (c->lx->cur.kind == TOK_IDENT && pcnt < 16u) {
            ps[pcnt] = c->lx->cur.start; pl[pcnt] = c->lx->cur.len; ++pcnt;
            lex_next(c->lx);
            if (!match_tok(c, TOK_COMMA)) break;
        }
        match_tok(c, TOK_RPAREN);
        compile_function_body(c, ps, pl, pcnt, false);

        if (has_name) {
            if (use_local) emit_op(c, OP_SET_LOCAL, 0u, (uint32_t)slot);
            else           emit_op(c, OP_SET_GLOBAL, 0u, gidx);
            emit_op(c, OP_POP, 0u, 0u);
        } else {
            emit_op(c, OP_POP, 0u, 0u);
        }
        return;
    }

    if (k == TOK_KW_IF) {
        lex_next(c->lx);
        match_tok(c, TOK_LPAREN);
        compile_expr(c);
        match_tok(c, TOK_RPAREN);

        size_t j_else = emit_op(c, OP_JMP_IF_FALSE, 0u, 0u);
        emit_op(c, OP_POP, 0u, 0u);
        compile_stmt(c);

        if (match_tok(c, TOK_KW_ELSE)) {
            size_t j_end = emit_op(c, OP_JMP, 0u, 0u);
            patch_jump(c, j_else);
            emit_op(c, OP_POP, 0u, 0u);
            compile_stmt(c);
            patch_jump(c, j_end);
        } else {
            size_t j_end = emit_op(c, OP_JMP, 0u, 0u);
            patch_jump(c, j_else);
            emit_op(c, OP_POP, 0u, 0u);
            patch_jump(c, j_end);
        }
        return;
    }

    if (k == TOK_KW_WHILE) {
        lex_next(c->lx);
        size_t loop_start = c->chunk->code_len;
        match_tok(c, TOK_LPAREN);
        compile_expr(c);
        match_tok(c, TOK_RPAREN);

        size_t j_exit = emit_op(c, OP_JMP_IF_FALSE, 0u, 0u);
        emit_op(c, OP_POP, 0u, 0u);

        if (c->loop_depth < ARCR_ARRAY_LEN(c->loops)) {
            JsLoopPatch *lp = &c->loops[c->loop_depth++];
            memset(lp, 0, sizeof(*lp));
            lp->continue_ip = loop_start;
            compile_stmt(c);
            emit_op(c, OP_JMP, 0u, (uint32_t)loop_start);
            patch_jump(c, j_exit);
            emit_op(c, OP_POP, 0u, 0u);
            for (size_t i = 0u; i < lp->break_count; ++i) patch_jump(c, lp->break_patches[i]);
            --c->loop_depth;
        }
        return;
    }

    if (k == TOK_KW_FOR) {
        lex_next(c->lx);
        match_tok(c, TOK_LPAREN);
        int prev_depth = c->scope_depth++;

        /* Check for `for (const/let/var x of iterable)` */
        bool is_for_of = false;
        JsToken iter_var = {0};
        if (c->lx->cur.kind == TOK_KW_CONST || c->lx->cur.kind == TOK_KW_LET || c->lx->cur.kind == TOK_KW_VAR) {
            JsLexer probe = *c->lx;
            lex_next(&probe);
            if (probe.cur.kind == TOK_IDENT) {
                iter_var = probe.cur;
                lex_next(&probe);
                if (probe.cur.kind == TOK_KW_OF) {
                    is_for_of = true;
                    *c->lx = probe;
                    lex_next(c->lx); /* consume 'of' */
                }
            }
        }

        if (is_for_of) {
            int slot_arr  = declare_local(c, "$arr", 4u);
            int slot_idx  = declare_local(c, "$idx", 4u);
            int slot_item = declare_local(c, iter_var.start, iter_var.len);

            compile_expr(c);
            emit_op(c, OP_SET_LOCAL, 0u, (uint32_t)slot_arr);
            emit_op(c, OP_POP, 0u, 0u);
            match_tok(c, TOK_RPAREN);

            emit_op(c, OP_CONST, 0u, chunk_add_const(c->vm, c->chunk, js_num(0.0)));
            emit_op(c, OP_SET_LOCAL, 0u, (uint32_t)slot_idx);
            emit_op(c, OP_POP, 0u, 0u);

            uint32_t len_pidx = chunk_add_const(c->vm, c->chunk, js_mk_str_n(c->vm, "length", 6u));
            uint32_t one_cidx = chunk_add_const(c->vm, c->chunk, js_num(1.0));

            size_t cond_ip = c->chunk->code_len;
            emit_op(c, OP_GET_LOCAL, 0u, (uint32_t)slot_idx);
            emit_op(c, OP_GET_LOCAL, 0u, (uint32_t)slot_arr);
            emit_op(c, OP_GET_PROP, 0u, len_pidx);
            emit_op(c, OP_LT, 0u, 0u);
            size_t j_exit = emit_op(c, OP_JMP_IF_FALSE, 0u, 0u);
            emit_op(c, OP_POP, 0u, 0u);

            /* item = arr[idx] */
            emit_op(c, OP_GET_LOCAL, 0u, (uint32_t)slot_arr);
            emit_op(c, OP_GET_LOCAL, 0u, (uint32_t)slot_idx);
            emit_op(c, OP_GET_INDEX, 0u, 0u);
            emit_op(c, OP_SET_LOCAL, 0u, (uint32_t)slot_item);
            emit_op(c, OP_POP, 0u, 0u);

            if (c->loop_depth < ARCR_ARRAY_LEN(c->loops)) {
                JsLoopPatch *lp = &c->loops[c->loop_depth++];
                memset(lp, 0, sizeof(*lp));
                lp->cont_uses_patches = true;
                compile_stmt(c);

                for (size_t i = 0u; i < lp->cont_count; ++i) patch_jump(c, lp->cont_patches[i]);
                emit_op(c, OP_GET_LOCAL, 0u, (uint32_t)slot_idx);
                emit_op(c, OP_CONST, 0u, one_cidx);
                emit_op(c, OP_ADD, 0u, 0u);
                emit_op(c, OP_SET_LOCAL, 0u, (uint32_t)slot_idx);
                emit_op(c, OP_POP, 0u, 0u);
                emit_op(c, OP_JMP, 0u, (uint32_t)cond_ip);

                patch_jump(c, j_exit);
                emit_op(c, OP_POP, 0u, 0u);
                for (size_t i = 0u; i < lp->break_count; ++i) patch_jump(c, lp->break_patches[i]);
                --c->loop_depth;
            }
            pop_locals_to_depth(c, prev_depth);
            c->scope_depth = prev_depth;
            return;
        }

        /* Standard C-style `for (init; cond; step)` */
        if (c->lx->cur.kind == TOK_SEMI) {
            lex_next(c->lx);
        } else if (c->lx->cur.kind == TOK_KW_VAR || c->lx->cur.kind == TOK_KW_LET || c->lx->cur.kind == TOK_KW_CONST) {
            compile_var_decl(c, false);
            match_tok(c, TOK_SEMI);
        } else {
            compile_expr(c);
            emit_op(c, OP_POP, 0u, 0u);
            match_tok(c, TOK_SEMI);
        }

        size_t cond_ip = c->chunk->code_len;
        size_t j_exit  = 0u;
        bool   has_cond = (c->lx->cur.kind != TOK_SEMI);
        if (has_cond) {
            compile_expr(c);
            j_exit = emit_op(c, OP_JMP_IF_FALSE, 0u, 0u);
            emit_op(c, OP_POP, 0u, 0u);
        }
        match_tok(c, TOK_SEMI);

        size_t step_ip = cond_ip;
        if (c->lx->cur.kind != TOK_RPAREN) {
            size_t j_body = emit_op(c, OP_JMP, 0u, 0u);
            step_ip = c->chunk->code_len;
            compile_expr(c);
            emit_op(c, OP_POP, 0u, 0u);
            emit_op(c, OP_JMP, 0u, (uint32_t)cond_ip);
            patch_jump(c, j_body);
        }
        match_tok(c, TOK_RPAREN);

        if (c->loop_depth < ARCR_ARRAY_LEN(c->loops)) {
            JsLoopPatch *lp = &c->loops[c->loop_depth++];
            memset(lp, 0, sizeof(*lp));
            lp->continue_ip = step_ip;
            compile_stmt(c);
            emit_op(c, OP_JMP, 0u, (uint32_t)step_ip);
            if (has_cond) {
                patch_jump(c, j_exit);
                emit_op(c, OP_POP, 0u, 0u);
            }
            for (size_t i = 0u; i < lp->break_count; ++i) patch_jump(c, lp->break_patches[i]);
            --c->loop_depth;
        }
        pop_locals_to_depth(c, prev_depth);
        c->scope_depth = prev_depth;
        return;
    }

    if (k == TOK_KW_BREAK) {
        lex_next(c->lx);
        consume_semi_optional(c);
        if (c->loop_depth > 0u) {
            JsLoopPatch *lp = &c->loops[c->loop_depth - 1u];
            if (lp->break_count < ARCR_ARRAY_LEN(lp->break_patches)) {
                lp->break_patches[lp->break_count++] = emit_op(c, OP_JMP, 0u, 0u);
            }
        }
        return;
    }

    if (k == TOK_KW_CONTINUE) {
        lex_next(c->lx);
        consume_semi_optional(c);
        if (c->loop_depth > 0u) {
            JsLoopPatch *lp = &c->loops[c->loop_depth - 1u];
            if (lp->cont_uses_patches) {
                if (lp->cont_count < ARCR_ARRAY_LEN(lp->cont_patches)) {
                    lp->cont_patches[lp->cont_count++] = emit_op(c, OP_JMP, 0u, 0u);
                }
            } else {
                emit_op(c, OP_JMP, 0u, (uint32_t)lp->continue_ip);
            }
        }
        return;
    }

    if (k == TOK_KW_RETURN) {
        lex_next(c->lx);
        if (c->lx->cur.kind == TOK_SEMI || c->lx->cur.kind == TOK_RBRACE ||
            c->lx->cur.kind == TOK_EOF || c->lx->cur.newline_before) {
            emit_op(c, OP_CONST, 0u, chunk_add_const(c->vm, c->chunk, js_undef()));
        } else {
            compile_expr(c);
        }
        consume_semi_optional(c);
        emit_op(c, OP_RETURN, 0u, 0u);
        return;
    }

    if (k == TOK_KW_THROW) {
        lex_next(c->lx);
        compile_expr(c);
        consume_semi_optional(c);
        emit_op(c, OP_THROW, 0u, 0u);
        return;
    }

    if (k == TOK_KW_TRY) {
        lex_next(c->lx);
        size_t try_inst = emit_op(c, OP_TRY_PUSH, 0u, 0u);
        compile_stmt(c); /* try block */
        emit_op(c, OP_TRY_POP, 0u, 0u);
        size_t j_after_catch = emit_op(c, OP_JMP, 0u, 0u);

        patch_jump(c, try_inst);
        if (match_tok(c, TOK_KW_CATCH)) {
            int prev_depth = c->scope_depth++;
            if (match_tok(c, TOK_LPAREN)) {
                if (c->lx->cur.kind == TOK_IDENT) {
                    int eslot = declare_local(c, c->lx->cur.start, c->lx->cur.len);
                    lex_next(c->lx);
                    emit_op(c, OP_SET_LOCAL, 0u, (uint32_t)eslot);
                }
                match_tok(c, TOK_RPAREN);
            }
            emit_op(c, OP_POP, 0u, 0u);
            compile_stmt(c);
            pop_locals_to_depth(c, prev_depth);
            c->scope_depth = prev_depth;
        } else {
            emit_op(c, OP_POP, 0u, 0u);
        }
        patch_jump(c, j_after_catch);
        if (match_tok(c, TOK_KW_FINALLY)) {
            compile_stmt(c);
        }
        return;
    }

    /* Expression statement */
    compile_expr(c);
    emit_op(c, OP_POP, 0u, 0u);
    consume_semi_optional(c);
}

static JsChunk *compile_script_to_chunk(ArcrJsVm *vm, const char *code, size_t code_len) {
    JsChunk *ch = alloc_chunk(vm);
    if (!ch) return NULL;

    JsLexer lx;
    memset(&lx, 0, sizeof(lx));
    lx.vm  = vm;
    lx.src = code;
    lx.len = code_len;
    lex_next(&lx);

    JsCompiler comp;
    memset(&comp, 0, sizeof(comp));
    comp.vm          = vm;
    comp.lx          = &lx;
    comp.chunk       = ch;
    comp.is_function = false;
    comp.scope_depth = 0;

    while (lx.cur.kind != TOK_EOF && !comp.had_error) {
        compile_stmt(&comp);
    }
    emit_op(&comp, OP_HALT, 0u, 0u);
    return comp.had_error ? NULL : ch;
}

/* =========================================================================
 * 7. Built-in Globals, Array/String Methods, Live DOM & <canvas> 2D Dispatch
 * ========================================================================= */
static bool vm_invoke_callable(ArcrJsVm *vm, JsVal callee, JsVal this_val,
                               const JsVal *args, size_t argc, JsVal *out_ret);

static JsEnv *alloc_env(ArcrJsVm *vm, JsEnv *parent, size_t slots) {
    size_t n = (slots > 0u) ? slots : 1u;
    JsEnv *e = (JsEnv *)vm_alloc(vm, sizeof(JsEnv) + n * sizeof(JsVal), sizeof(void *));
    if (!e) return NULL;
    e->parent     = parent;
    e->slot_count = n;
    for (size_t i = 0u; i < n; ++i) e->slots[i] = js_undef();
    return e;
}

static JsVal *env_slot_ptr(JsEnv *env, uint32_t encoded) {
    uint16_t depth = (uint16_t)(encoded >> 16);
    uint16_t slot  = (uint16_t)(encoded & 0xFFFFu);
    JsEnv *cur = env;
    while (depth > 0u && cur) { cur = cur->parent; --depth; }
    if (!cur || slot >= cur->slot_count) return NULL;
    return &cur->slots[slot];
}

static void vm_storage_set_local(ArcrJsVm *vm, const char *k, const char *v) {
    if (!k) return;
    if (vm->host_cb.storage_set) {
        vm->host_cb.storage_set(vm->host_cb.userdata, k, v);
    }
    size_t free_i = JS_MAX_STORAGE_ENTRIES;
    for (size_t i = 0u; i < JS_MAX_STORAGE_ENTRIES; ++i) {
        if (!vm->local_storage[i].in_use) {
            if (free_i == JS_MAX_STORAGE_ENTRIES) free_i = i;
            continue;
        }
        if (strcmp(vm->local_storage[i].key, k) == 0) {
            if (!v) { vm->local_storage[i].in_use = false; return; }
            arcr_str_copy(vm->local_storage[i].val, sizeof(vm->local_storage[i].val), v);
            return;
        }
    }
    if (!v) return;
    if (free_i == JS_MAX_STORAGE_ENTRIES) free_i = 0u;
    vm->local_storage[free_i].in_use = true;
    arcr_str_copy(vm->local_storage[free_i].key, sizeof(vm->local_storage[free_i].key), k);
    arcr_str_copy(vm->local_storage[free_i].val, sizeof(vm->local_storage[free_i].val), v);
}

static bool vm_storage_get_local(ArcrJsVm *vm, const char *k, char *out_v, size_t cap) {
    if (!k || !out_v || cap == 0u) return false;
    if (vm->host_cb.storage_get && vm->host_cb.storage_get(vm->host_cb.userdata, k, out_v, cap)) {
        return true;
    }
    for (size_t i = 0u; i < JS_MAX_STORAGE_ENTRIES; ++i) {
        if (vm->local_storage[i].in_use && strcmp(vm->local_storage[i].key, k) == 0) {
            arcr_str_copy(out_v, cap, vm->local_storage[i].val);
            return true;
        }
    }
    return false;
}

static JsVal vm_get_prop(ArcrJsVm *vm, JsVal obj, const char *prop) {
    ArcrDocument *doc = vm->host_cb.doc;

    if (obj.kind == JS_VAL_STRING && obj.as.str) {
        if (strcmp(prop, "length") == 0) return js_num((double)obj.as.str->len);
        return js_undef();
    }
    if (obj.kind == JS_VAL_ARRAY && obj.as.arr) {
        if (strcmp(prop, "length") == 0) return js_num((double)obj.as.arr->len);
        return js_undef();
    }
    if (obj.kind == JS_VAL_OBJECT && obj.as.obj) {
        JsVal out = js_undef();
        if (js_obj_get(obj.as.obj, prop, &out)) return out;
        return js_undef();
    }

    if (obj.kind == JS_VAL_BUILTIN_OBJ) {
        JsBuiltinId bid = (JsBuiltinId)obj.as.u32;
        if (bid == JS_BUILTIN_MATH) {
            if (strcmp(prop, "PI") == 0)    return js_num(JS_PI_CONST);
            if (strcmp(prop, "E") == 0)     return js_num(2.718281828459045);
            if (strcmp(prop, "SQRT2") == 0) return js_num(1.4142135623730951);
            return js_undef();
        }
        if (bid == JS_BUILTIN_WINDOW) {
            if (strcmp(prop, "document") == 0)     { JsVal v = {JS_VAL_BUILTIN_OBJ, {.u32 = JS_BUILTIN_DOCUMENT}}; return v; }
            if (strcmp(prop, "location") == 0)     { JsVal v = {JS_VAL_BUILTIN_OBJ, {.u32 = JS_BUILTIN_LOCATION}}; return v; }
            if (strcmp(prop, "localStorage") == 0) { JsVal v = {JS_VAL_BUILTIN_OBJ, {.u32 = JS_BUILTIN_LOCALSTORAGE}}; return v; }
            if (strcmp(prop, "console") == 0)      { JsVal v = {JS_VAL_BUILTIN_OBJ, {.u32 = JS_BUILTIN_CONSOLE}}; return v; }
            if (strcmp(prop, "performance") == 0)  { JsVal v = {JS_VAL_BUILTIN_OBJ, {.u32 = JS_BUILTIN_PERFORMANCE}}; return v; }
            if (strcmp(prop, "innerWidth") == 0)   return js_num(1024.0);
            if (strcmp(prop, "innerHeight") == 0)  return js_num(768.0);
            JsVal gv = js_undef();
            if (js_obj_get(vm->globals, prop, &gv)) return gv;
            return js_undef();
        }
        if (bid == JS_BUILTIN_LOCATION) {
            if (strcmp(prop, "href") == 0) return js_mk_str(vm, vm->location_href);
            return js_undef();
        }
        if (bid == JS_BUILTIN_DOCUMENT) {
            if (strcmp(prop, "title") == 0) {
                if (doc && doc->title && doc->title[0] != '\0') return js_mk_str(vm, doc->title);
                return js_mk_str(vm, vm->doc_title);
            }
            if (strcmp(prop, "body") == 0) {
                if (!doc) return js_null();
                js_dom_ensure_uids(doc);
                return doc->body ? js_dom_node(doc->body->node_uid) : js_null();
            }
            if (strcmp(prop, "documentElement") == 0) {
                if (!doc || !doc->root) return js_null();
                js_dom_ensure_uids(doc);
                for (ArcrDomNode *ch = doc->root->first_child; ch != NULL; ch = ch->next_sibling) {
                    if (ch->kind == ARCR_NODE_ELEMENT) return js_dom_node(ch->node_uid);
                }
                return js_null();
            }
            if (strcmp(prop, "location") == 0) {
                JsVal v = {JS_VAL_BUILTIN_OBJ, {.u32 = JS_BUILTIN_LOCATION}};
                return v;
            }
            return js_undef();
        }
    }

    if (obj.kind == JS_VAL_DOM_NODE && doc) {
        ArcrDomNode *el = js_dom_find_uid(vm, doc, obj.as.u32);
        if (!el) return js_undef();
        if (strcmp(prop, "id") == 0) {
            const char *idv = (el->id != ARCR_ATOM_NULL) ? arcr_atom_cstr(doc->atoms, el->id) : "";
            return js_mk_str(vm, idv ? idv : "");
        }
        if (strcmp(prop, "tagName") == 0 || strcmp(prop, "nodeName") == 0) {
            const char *tg = arcr_atom_cstr(doc->atoms, el->tag);
            char up[64];
            size_t i = 0u;
            if (tg) {
                for (; tg[i] != '\0' && i + 1u < sizeof(up); ++i) {
                    char c = tg[i];
                    up[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
                }
            }
            up[i] = '\0';
            return js_mk_str(vm, up);
        }
        if (strcmp(prop, "className") == 0) {
            const char *cv = js_dom_get_attr(doc, el, "class");
            return js_mk_str(vm, cv ? cv : "");
        }
        if (strcmp(prop, "textContent") == 0 || strcmp(prop, "innerText") == 0) {
            size_t tlen = js_dom_collect_text(el, NULL, 0u, 0u);
            char *buf = (char *)arcr_arena_alloc(&vm->temp_arena, tlen + 1u, 1u);
            if (!buf) return js_mk_str(vm, "");
            js_dom_collect_text(el, buf, tlen + 1u, 0u);
            buf[tlen] = '\0';
            return js_mk_str_n(vm, buf, tlen);
        }
        if (strcmp(prop, "innerHTML") == 0) {
            size_t hlen = js_dom_serialize_children(doc, el, NULL, 0u, 0u);
            char *buf = (char *)arcr_arena_alloc(&vm->temp_arena, hlen + 1u, 1u);
            if (!buf) return js_mk_str(vm, "");
            js_dom_serialize_children(doc, el, buf, hlen + 1u, 0u);
            return js_mk_str_n(vm, buf, hlen);
        }
        if (strcmp(prop, "value") == 0) {
            if (el->input_value) return js_mk_str(vm, el->input_value);
            const char *va = js_dom_get_attr(doc, el, "value");
            return js_mk_str(vm, va ? va : "");
        }
        if (strcmp(prop, "checked") == 0) {
            return js_bool((el->state_flags & ARCR_STATE_CHECKED) != 0u);
        }
        if (strcmp(prop, "style") == 0) {
            JsVal sv; memset(&sv, 0, sizeof(sv)); sv.kind = JS_VAL_DOM_STYLE; sv.as.u32 = el->node_uid;
            return sv;
        }
        if (strcmp(prop, "classList") == 0) {
            JsVal cv; memset(&cv, 0, sizeof(cv)); cv.kind = JS_VAL_DOM_CLASSLIST; cv.as.u32 = el->node_uid;
            return cv;
        }
        if (strcmp(prop, "parentElement") == 0 || strcmp(prop, "parentNode") == 0) {
            if (el->parent && el->parent->kind == ARCR_NODE_ELEMENT) {
                return js_dom_node(el->parent->node_uid);
            }
            return js_null();
        }
        if (strcmp(prop, "children") == 0) {
            JsArray *ca = js_alloc_array(vm, 4u);
            if (!ca) return js_undef();
            for (ArcrDomNode *ch = el->first_child; ch != NULL; ch = ch->next_sibling) {
                if (ch->kind == ARCR_NODE_ELEMENT) js_array_push(vm, ca, js_dom_node(ch->node_uid));
            }
            JsVal av; memset(&av, 0, sizeof(av)); av.kind = JS_VAL_ARRAY; av.as.arr = ca;
            return av;
        }
        if (strcmp(prop, "width") == 0 || strcmp(prop, "height") == 0) {
            const char *av = js_dom_get_attr(doc, el, prop);
            int32_t iv = (strcmp(prop, "width") == 0) ? 300 : 150;
            if (av) arcr_parse_i32(av, strlen(av), &iv);
            return js_num((double)iv);
        }
    }

    if (obj.kind == JS_VAL_DOM_STYLE && doc) {
        ArcrDomNode *el = js_dom_find_uid(vm, doc, obj.as.u32);
        char val_buf[256];
        js_style_get_prop(doc, el, prop, val_buf, sizeof(val_buf));
        return js_mk_str(vm, val_buf);
    }

    if (obj.kind == JS_VAL_DOM_CLASSLIST && doc) {
        ArcrDomNode *el = js_dom_find_uid(vm, doc, obj.as.u32);
        if (el && strcmp(prop, "length") == 0) return js_num((double)el->class_count);
    }

    if (obj.kind == JS_VAL_CANVAS_CTX2D && obj.as.ctx2d) {
        if (strcmp(prop, "lineWidth") == 0) return js_num((double)obj.as.ctx2d->line_width);
        if (strcmp(prop, "fillStyle") == 0 || strcmp(prop, "strokeStyle") == 0) {
            uint32_t c = (prop[0] == 'f') ? obj.as.ctx2d->fill_argb : obj.as.ctx2d->stroke_argb;
            char hex[16];
            snprintf(hex, sizeof(hex), "#%02x%02x%02x",
                     (unsigned)((c >> 16) & 0xFFu), (unsigned)((c >> 8) & 0xFFu), (unsigned)(c & 0xFFu));
            return js_mk_str(vm, hex);
        }
    }
    return js_undef();
}

static void vm_set_prop(ArcrJsVm *vm, JsVal obj, const char *prop, JsVal val) {
    ArcrDocument *doc = vm->host_cb.doc;
    char tmp[512];

    if (obj.kind == JS_VAL_OBJECT && obj.as.obj) {
        js_obj_set(vm, obj.as.obj, prop, val);
        return;
    }
    if (obj.kind == JS_VAL_ARRAY && obj.as.arr && strcmp(prop, "length") == 0) {
        int32_t nl = (int32_t)js_to_number(val);
        if (nl >= 0 && (size_t)nl < obj.as.arr->len) obj.as.arr->len = (size_t)nl;
        return;
    }

    if (obj.kind == JS_VAL_BUILTIN_OBJ) {
        JsBuiltinId bid = (JsBuiltinId)obj.as.u32;
        if (bid == JS_BUILTIN_DOCUMENT && strcmp(prop, "title") == 0) {
            const char *s = js_to_cstr_buf(vm, val, tmp, sizeof(tmp));
            arcr_str_copy(vm->doc_title, sizeof(vm->doc_title), s);
            if (doc) {
                doc->title     = arcr_arena_strdup(&doc->arena, s, strlen(s));
                doc->dom_dirty = true;
            }
            if (vm->host_cb.set_document_title) {
                vm->host_cb.set_document_title(vm->host_cb.userdata, s);
            }
            return;
        }
        if ((bid == JS_BUILTIN_LOCATION && strcmp(prop, "href") == 0) ||
            (bid == JS_BUILTIN_WINDOW && strcmp(prop, "location") == 0)) {
            const char *s = js_to_cstr_buf(vm, val, tmp, sizeof(tmp));
            arcr_str_copy(vm->location_href, sizeof(vm->location_href), s);
            if (vm->host_cb.navigate_to) {
                vm->host_cb.navigate_to(vm->host_cb.userdata, s);
            }
            return;
        }
        if (bid == JS_BUILTIN_WINDOW) {
            js_obj_set(vm, vm->globals, prop, val);
            return;
        }
    }

    if (obj.kind == JS_VAL_DOM_NODE && doc) {
        ArcrDomNode *el = js_dom_find_uid(vm, doc, obj.as.u32);
        if (!el) return;
        const char *s = js_to_cstr_buf(vm, val, tmp, sizeof(tmp));
        if (strcmp(prop, "id") == 0) {
            js_dom_set_attr(doc, el, "id", 2u, s, strlen(s));
        } else if (strcmp(prop, "className") == 0) {
            js_dom_set_attr(doc, el, "class", 5u, s, strlen(s));
        } else if (strcmp(prop, "textContent") == 0 || strcmp(prop, "innerText") == 0) {
            js_dom_clear_children(doc, el);
            size_t slen = strlen(s);
            if (slen > 0u) {
                ArcrDomNode *tn = js_dom_create_text(doc, s, slen);
                if (tn) js_dom_append_child(doc, el, tn);
            }
            doc->dom_dirty = true;
        } else if (strcmp(prop, "innerHTML") == 0) {
            js_dom_parse_fragment(doc, el, s, strlen(s));
        } else if (strcmp(prop, "value") == 0) {
            size_t slen = strlen(s);
            if (!el->input_value || slen + 1u > el->input_cap) {
                size_t ncap = (slen + 64u);
                char *nb = (char *)arcr_arena_alloc(&doc->arena, ncap, 1u);
                if (nb) { el->input_value = nb; el->input_cap = ncap; }
            }
            if (el->input_value) arcr_str_copy(el->input_value, el->input_cap, s);
            js_dom_set_attr(doc, el, "value", 5u, s, slen);
        } else if (strcmp(prop, "checked") == 0) {
            if (js_is_truthy(val)) el->state_flags |= ARCR_STATE_CHECKED;
            else                   el->state_flags &= ~ARCR_STATE_CHECKED;
            doc->dom_dirty = true;
        } else if (strcmp(prop, "width") == 0 || strcmp(prop, "height") == 0) {
            js_dom_set_attr(doc, el, prop, strlen(prop), s, strlen(s));
        }
        return;
    }

    if (obj.kind == JS_VAL_DOM_STYLE && doc) {
        ArcrDomNode *el = js_dom_find_uid(vm, doc, obj.as.u32);
        const char *s = js_to_cstr_buf(vm, val, tmp, sizeof(tmp));
        js_style_set_prop(doc, el, prop, s);
        return;
    }

    if (obj.kind == JS_VAL_CANVAS_CTX2D && obj.as.ctx2d) {
        if (strcmp(prop, "fillStyle") == 0) {
            const char *s = js_to_cstr_buf(vm, val, tmp, sizeof(tmp));
            obj.as.ctx2d->fill_argb = js_parse_css_color(s, obj.as.ctx2d->fill_argb);
        } else if (strcmp(prop, "strokeStyle") == 0) {
            const char *s = js_to_cstr_buf(vm, val, tmp, sizeof(tmp));
            obj.as.ctx2d->stroke_argb = js_parse_css_color(s, obj.as.ctx2d->stroke_argb);
        } else if (strcmp(prop, "lineWidth") == 0) {
            double lw = js_to_number(val);
            if (!js_num_is_nan(lw) && lw > 0.0) obj.as.ctx2d->line_width = (float)lw;
        }
    }
}

static JsVal vm_call_method(ArcrJsVm *vm, JsVal recv, const char *mname,
                            const JsVal *args, size_t argc) {
    ArcrDocument *doc = vm->host_cb.doc;
    char tmp1[512], tmp2[512];

    /* 1. Built-in singleton objects */
    if (recv.kind == JS_VAL_BUILTIN_OBJ) {
        JsBuiltinId bid = (JsBuiltinId)recv.as.u32;
        if (bid == JS_BUILTIN_CONSOLE) {
            if (strcmp(mname, "log") == 0 || strcmp(mname, "warn") == 0 ||
                strcmp(mname, "error") == 0 || strcmp(mname, "info") == 0) {
                char line[1024];
                size_t pos = 0u;
                line[0] = '\0';
                for (size_t i = 0u; i < argc; ++i) {
                    if (i > 0u && pos + 1u < sizeof(line)) line[pos++] = ' ';
                    const char *s = js_to_cstr_buf(vm, args[i], tmp1, sizeof(tmp1));
                    size_t sl = strlen(s);
                    if (pos + sl >= sizeof(line)) sl = (sizeof(line) - 1u > pos) ? (sizeof(line) - 1u - pos) : 0u;
                    if (sl > 0u) { memcpy(line + pos, s, sl); pos += sl; }
                }
                line[pos] = '\0';
                if (vm->host_cb.console_log) {
                    vm->host_cb.console_log(vm->host_cb.userdata, line);
                }
            }
            return js_undef();
        }
        if (bid == JS_BUILTIN_MATH) {
            double x = (argc > 0u) ? js_to_number(args[0]) : js_make_nan();
            double y = (argc > 1u) ? js_to_number(args[1]) : js_make_nan();
            if (strcmp(mname, "floor") == 0)  return js_num(js_math_floor(x));
            if (strcmp(mname, "ceil") == 0)   return js_num(js_math_ceil(x));
            if (strcmp(mname, "round") == 0)  return js_num(js_math_round(x));
            if (strcmp(mname, "trunc") == 0)  return js_num(js_math_trunc(x));
            if (strcmp(mname, "abs") == 0)    return js_num(js_math_abs(x));
            if (strcmp(mname, "sqrt") == 0)   return js_num(js_math_sqrt(x));
            if (strcmp(mname, "sin") == 0)    return js_num(js_math_sin(x));
            if (strcmp(mname, "cos") == 0)    return js_num(js_math_cos(x));
            if (strcmp(mname, "pow") == 0)    return js_num(js_math_pow(x, y));
            if (strcmp(mname, "log") == 0)    return js_num(js_math_ln(x));
            if (strcmp(mname, "exp") == 0)    return js_num(js_math_exp(x));
            if (strcmp(mname, "random") == 0) {
                uint64_t r = js_next_rand_u64(vm) >> 11;
                return js_num((double)r * (1.0 / 9007199254740992.0));
            }
            if (strcmp(mname, "min") == 0) {
                if (argc == 0u) return js_num(1e308 * 10.0);
                double best = x;
                for (size_t i = 1u; i < argc; ++i) {
                    double v = js_to_number(args[i]);
                    if (js_num_is_nan(v)) return js_num(v);
                    if (v < best) best = v;
                }
                return js_num(best);
            }
            if (strcmp(mname, "max") == 0) {
                if (argc == 0u) return js_num(-1e308 * 10.0);
                double best = x;
                for (size_t i = 1u; i < argc; ++i) {
                    double v = js_to_number(args[i]);
                    if (js_num_is_nan(v)) return js_num(v);
                    if (v > best) best = v;
                }
                return js_num(best);
            }
            return js_undef();
        }
        if (bid == JS_BUILTIN_JSON) {
            if (strcmp(mname, "stringify") == 0) {
                if (argc == 0u || args[0].kind == JS_VAL_UNDEFINED) return js_undef();
                JsonBuf jb = {0};
                json_stringify_rec(vm, args[0], &jb, 0);
                return js_mk_str_n(vm, jb.buf ? jb.buf : "null", jb.len);
            }
            if (strcmp(mname, "parse") == 0 && argc > 0u) {
                const char *src = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
                if (args[0].kind == JS_VAL_STRING && args[0].as.str) src = args[0].as.str->data;
                JsonParser jp = { vm, src, strlen(src), 0u, true };
                JsVal res = jp_parse_value(&jp, 0);
                jp_skip_ws(&jp);
                if (!jp.ok || jp.pos != jp.len) {
                    vm->thrown_val = js_mk_str(vm, "SyntaxError: Invalid JSON");
                    vm->has_exception = true;
                    return js_undef();
                }
                return res;
            }
        }
        if (bid == JS_BUILTIN_PERFORMANCE && strcmp(mname, "now") == 0) {
            uint64_t now_us = arcr_platform_monotonic_usec();
            return js_num((double)(now_us - vm->start_monotonic_usec) / 1000.0);
        }
        if (bid == JS_BUILTIN_DATE && strcmp(mname, "now") == 0) {
            return js_num((double)arcr_platform_wall_epoch_sec() * 1000.0);
        }
        if (bid == JS_BUILTIN_LOCALSTORAGE) {
            if (strcmp(mname, "getItem") == 0 && argc > 0u) {
                const char *k = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
                if (vm_storage_get_local(vm, k, tmp2, sizeof(tmp2))) return js_mk_str(vm, tmp2);
                return js_null();
            }
            if (strcmp(mname, "setItem") == 0 && argc >= 2u) {
                const char *k = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
                const char *v = js_to_cstr_buf(vm, args[1], tmp2, sizeof(tmp2));
                vm_storage_set_local(vm, k, v);
                return js_undef();
            }
            if (strcmp(mname, "removeItem") == 0 && argc > 0u) {
                const char *k = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
                vm_storage_set_local(vm, k, NULL);
                return js_undef();
            }
            if (strcmp(mname, "clear") == 0) {
                memset(vm->local_storage, 0, sizeof(vm->local_storage));
                return js_undef();
            }
        }
        if (bid == JS_BUILTIN_LOCATION && (strcmp(mname, "assign") == 0 || strcmp(mname, "replace") == 0) && argc > 0u) {
            const char *u = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
            arcr_str_copy(vm->location_href, sizeof(vm->location_href), u);
            if (vm->host_cb.navigate_to) vm->host_cb.navigate_to(vm->host_cb.userdata, u);
            return js_undef();
        }
        if (bid == JS_BUILTIN_ARRAY_CTOR && strcmp(mname, "isArray") == 0) {
            return js_bool(argc > 0u && args[0].kind == JS_VAL_ARRAY);
        }
        if (bid == JS_BUILTIN_OBJECT_CTOR && strcmp(mname, "keys") == 0 && argc > 0u) {
            JsArray *ka = js_alloc_array(vm, 4u);
            if (ka && args[0].kind == JS_VAL_OBJECT && args[0].as.obj) {
                for (size_t i = 0u; i < args[0].as.obj->len; ++i) {
                    js_array_push(vm, ka, js_mk_str(vm, args[0].as.obj->props[i].key));
                }
            }
            JsVal v; memset(&v, 0, sizeof(v)); v.kind = JS_VAL_ARRAY; v.as.arr = ka;
            return v;
        }
        if (bid == JS_BUILTIN_DOCUMENT) {
            if (!doc) return js_null();
            js_dom_ensure_uids(doc);
            if (strcmp(mname, "getElementById") == 0 && argc > 0u) {
                const char *id_s = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
                if (!id_s || id_s[0] == '\0') return js_null();
                ArcrAtom ia = arcr_atom_intern(doc->atoms, id_s, strlen(id_s));
                ArcrDomNode *hit = js_dom_find_by_id_rec(doc->root, ia);
                return hit ? js_dom_node(hit->node_uid) : js_null();
            }
            if (strcmp(mname, "querySelector") == 0 && argc > 0u) {
                const char *sel = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
                ArcrDomNode *hit = NULL;
                size_t cnt = 0u;
                js_query_walk(doc, doc->root, doc->root, sel, &hit, &cnt, 1u);
                return (cnt > 0u && hit) ? js_dom_node(hit->node_uid) : js_null();
            }
            if (strcmp(mname, "querySelectorAll") == 0 && argc > 0u) {
                const char *sel = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
                ArcrDomNode *hits[128];
                size_t cnt = 0u;
                js_query_walk(doc, doc->root, doc->root, sel, hits, &cnt, ARCR_ARRAY_LEN(hits));
                JsArray *arr = js_alloc_array(vm, cnt);
                for (size_t i = 0u; arr && i < cnt; ++i) {
                    js_array_push(vm, arr, js_dom_node(hits[i]->node_uid));
                }
                JsVal av; memset(&av, 0, sizeof(av)); av.kind = JS_VAL_ARRAY; av.as.arr = arr;
                return av;
            }
            if (strcmp(mname, "createElement") == 0 && argc > 0u) {
                const char *tg = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
                ArcrDomNode *el = js_dom_create_element(doc, tg, strlen(tg));
                if (el) js_dom_register_detached(vm, doc, el);
                return el ? js_dom_node(el->node_uid) : js_null();
            }
            if (strcmp(mname, "createTextNode") == 0 && argc > 0u) {
                const char *tx = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
                ArcrDomNode *tn = js_dom_create_text(doc, tx, strlen(tx));
                if (tn) js_dom_register_detached(vm, doc, tn);
                return tn ? js_dom_node(tn->node_uid) : js_null();
            }
            return js_undef();
        }
    }

    /* 2. DOM Element Methods */
    if (recv.kind == JS_VAL_DOM_NODE && doc) {
        ArcrDomNode *el = js_dom_find_uid(vm, doc, recv.as.u32);
        if (!el) return js_undef();
        if (strcmp(mname, "getAttribute") == 0 && argc > 0u) {
            const char *k = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
            const char *v = js_dom_get_attr(doc, el, k);
            return v ? js_mk_str(vm, v) : js_null();
        }
        if (strcmp(mname, "setAttribute") == 0 && argc >= 2u) {
            const char *k = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
            const char *v = js_to_cstr_buf(vm, args[1], tmp2, sizeof(tmp2));
            js_dom_set_attr(doc, el, k, strlen(k), v, strlen(v));
            return js_undef();
        }
        if (strcmp(mname, "hasAttribute") == 0 && argc > 0u) {
            const char *k = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
            return js_bool(js_dom_get_attr(doc, el, k) != NULL);
        }
        if (strcmp(mname, "appendChild") == 0 && argc > 0u && args[0].kind == JS_VAL_DOM_NODE) {
            ArcrDomNode *ch = js_dom_find_uid(vm, doc, args[0].as.u32);
            if (ch) js_dom_append_child(doc, el, ch);
            return args[0];
        }
        if (strcmp(mname, "remove") == 0) {
            js_dom_unlink(el);
            js_dom_register_detached(vm, doc, el);
            doc->dom_dirty = true;
            return js_undef();
        }
        if (strcmp(mname, "querySelector") == 0 && argc > 0u) {
            const char *sel = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
            ArcrDomNode *hit = NULL; size_t cnt = 0u;
            js_query_walk(doc, el, el, sel, &hit, &cnt, 1u);
            return (cnt > 0u && hit) ? js_dom_node(hit->node_uid) : js_null();
        }
        if (strcmp(mname, "querySelectorAll") == 0 && argc > 0u) {
            const char *sel = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
            ArcrDomNode *hits[128]; size_t cnt = 0u;
            js_query_walk(doc, el, el, sel, hits, &cnt, ARCR_ARRAY_LEN(hits));
            JsArray *arr = js_alloc_array(vm, cnt);
            for (size_t i = 0u; arr && i < cnt; ++i) js_array_push(vm, arr, js_dom_node(hits[i]->node_uid));
            JsVal av; memset(&av, 0, sizeof(av)); av.kind = JS_VAL_ARRAY; av.as.arr = arr;
            return av;
        }
        if (strcmp(mname, "addEventListener") == 0 && argc >= 2u) {
            const char *ev = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
            for (size_t i = 0u; i < JS_MAX_EVENT_LISTENERS; ++i) {
                if (!vm->listeners[i].active) {
                    vm->listeners[i].active   = true;
                    vm->listeners[i].node_uid = el->node_uid;
                    arcr_str_copy(vm->listeners[i].event_type, sizeof(vm->listeners[i].event_type), ev);
                    vm->listeners[i].callback = args[1];
                    break;
                }
            }
            return js_undef();
        }
        if (strcmp(mname, "getContext") == 0 && argc > 0u) {
            const char *kind = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
            if (arcr_str_eq_nocase(kind, "2d")) {
                js_ensure_canvas_surface(doc, el);
                JsCanvasCtx2D *ctx = (JsCanvasCtx2D *)vm_alloc(vm, sizeof(JsCanvasCtx2D), sizeof(void *));
                if (!ctx) return js_null();
                memset(ctx, 0, sizeof(*ctx));
                ctx->node_uid    = el->node_uid;
                ctx->fill_argb   = 0xFF000000u;
                ctx->stroke_argb = 0xFF000000u;
                ctx->line_width  = 1.0f;
                JsVal cv; memset(&cv, 0, sizeof(cv)); cv.kind = JS_VAL_CANVAS_CTX2D; cv.as.ctx2d = ctx;
                return cv;
            }
            return js_null();
        }
        return js_undef();
    }

    /* 3. DOM classList Methods */
    if (recv.kind == JS_VAL_DOM_CLASSLIST && doc) {
        ArcrDomNode *el = js_dom_find_uid(vm, doc, recv.as.u32);
        if (!el || argc == 0u) return js_undef();
        const char *cls = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
        if (cls[0] == '\0') return js_bool(false);
        ArcrAtom ca = arcr_atom_intern(doc->atoms, cls, strlen(cls));

        int idx = -1;
        for (uint32_t i = 0u; i < el->class_count; ++i) {
            if (el->classes[i] == ca) { idx = (int)i; break; }
        }
        if (strcmp(mname, "contains") == 0) return js_bool(idx >= 0);
        if (strcmp(mname, "add") == 0) {
            if (idx < 0 && el->class_count < ARCR_MAX_INLINE_CLASSES) {
                el->classes[el->class_count++] = ca;
                js_dom_sync_class_attr(doc, el);
                js_recompute_subtree_bloom(el);
            }
            return js_undef();
        }
        if (strcmp(mname, "remove") == 0) {
            if (idx >= 0) {
                for (uint32_t k = (uint32_t)idx + 1u; k < el->class_count; ++k) {
                    el->classes[k - 1u] = el->classes[k];
                }
                el->class_count--;
                js_dom_sync_class_attr(doc, el);
                js_recompute_subtree_bloom(el);
            }
            return js_undef();
        }
        if (strcmp(mname, "toggle") == 0) {
            if (idx >= 0) {
                for (uint32_t k = (uint32_t)idx + 1u; k < el->class_count; ++k) {
                    el->classes[k - 1u] = el->classes[k];
                }
                el->class_count--;
                js_dom_sync_class_attr(doc, el);
                js_recompute_subtree_bloom(el);
                return js_bool(false);
            }
            if (el->class_count < ARCR_MAX_INLINE_CLASSES) {
                el->classes[el->class_count++] = ca;
                js_dom_sync_class_attr(doc, el);
                js_recompute_subtree_bloom(el);
                return js_bool(true);
            }
            return js_bool(false);
        }
    }

    /* 4. HTML5 <canvas> 2D Context Methods */
    if (recv.kind == JS_VAL_CANVAS_CTX2D && recv.as.ctx2d && doc) {
        JsCanvasCtx2D *ctx = recv.as.ctx2d;
        ArcrDomNode *cel   = js_dom_find_uid(vm, doc, ctx->node_uid);
        ArcrDomSurface *sf = js_ensure_canvas_surface(doc, cel);
        if (!sf) return js_undef();
        doc->dom_dirty = true;

        if (strcmp(mname, "fillRect") == 0 && argc >= 4u) {
            canvas_fill_rect(sf, js_to_number(args[0]), js_to_number(args[1]),
                             js_to_number(args[2]), js_to_number(args[3]), ctx->fill_argb, false);
        } else if (strcmp(mname, "clearRect") == 0 && argc >= 4u) {
            canvas_fill_rect(sf, js_to_number(args[0]), js_to_number(args[1]),
                             js_to_number(args[2]), js_to_number(args[3]), 0u, true);
        } else if (strcmp(mname, "strokeRect") == 0 && argc >= 4u) {
            float x = (float)js_to_number(args[0]), y = (float)js_to_number(args[1]);
            float w = (float)js_to_number(args[2]), h = (float)js_to_number(args[3]);
            canvas_draw_segment(sf, x, y, x + w, y, ctx->line_width, ctx->stroke_argb);
            canvas_draw_segment(sf, x + w, y, x + w, y + h, ctx->line_width, ctx->stroke_argb);
            canvas_draw_segment(sf, x + w, y + h, x, y + h, ctx->line_width, ctx->stroke_argb);
            canvas_draw_segment(sf, x, y + h, x, y, ctx->line_width, ctx->stroke_argb);
        } else if (strcmp(mname, "beginPath") == 0) {
            ctx->path_len = 0u;
        } else if (strcmp(mname, "moveTo") == 0 && argc >= 2u) {
            if (ctx->path_len < ARCR_ARRAY_LEN(ctx->path_x)) {
                ctx->path_x[ctx->path_len]    = (float)js_to_number(args[0]);
                ctx->path_y[ctx->path_len]    = (float)js_to_number(args[1]);
                ctx->path_move[ctx->path_len] = 1u;
                ctx->path_len++;
            }
        } else if (strcmp(mname, "lineTo") == 0 && argc >= 2u) {
            if (ctx->path_len < ARCR_ARRAY_LEN(ctx->path_x)) {
                uint8_t mv = (ctx->path_len == 0u) ? 1u : 0u;
                ctx->path_x[ctx->path_len]    = (float)js_to_number(args[0]);
                ctx->path_y[ctx->path_len]    = (float)js_to_number(args[1]);
                ctx->path_move[ctx->path_len] = mv;
                ctx->path_len++;
            }
        } else if (strcmp(mname, "closePath") == 0) {
            if (ctx->path_len > 1u && ctx->path_len < ARCR_ARRAY_LEN(ctx->path_x)) {
                size_t sub = ctx->path_len - 1u;
                while (sub > 0u && ctx->path_move[sub] == 0u) --sub;
                ctx->path_x[ctx->path_len]    = ctx->path_x[sub];
                ctx->path_y[ctx->path_len]    = ctx->path_y[sub];
                ctx->path_move[ctx->path_len] = 0u;
                ctx->path_len++;
            }
        } else if (strcmp(mname, "arc") == 0 && argc >= 5u) {
            double cx = js_to_number(args[0]), cy = js_to_number(args[1]);
            double r  = js_to_number(args[2]);
            double a0 = js_to_number(args[3]), a1 = js_to_number(args[4]);
            if (r > 0.0) {
                int segs = 32;
                for (int i = 0; i <= segs && ctx->path_len < ARCR_ARRAY_LEN(ctx->path_x); ++i) {
                    double t = a0 + (a1 - a0) * ((double)i / (double)segs);
                    ctx->path_x[ctx->path_len]    = (float)(cx + r * js_math_cos(t));
                    ctx->path_y[ctx->path_len]    = (float)(cy + r * js_math_sin(t));
                    ctx->path_move[ctx->path_len] = (i == 0 && ctx->path_len == 0u) ? 1u : 0u;
                    ctx->path_len++;
                }
            }
        } else if (strcmp(mname, "stroke") == 0) {
            canvas_stroke_path(sf, ctx);
        } else if (strcmp(mname, "fill") == 0) {
            canvas_fill_path(sf, ctx);
        }
        return js_undef();
    }

    /* 5. Array Prototype Methods */
    if (recv.kind == JS_VAL_ARRAY && recv.as.arr) {
        JsArray *a = recv.as.arr;
        if (strcmp(mname, "push") == 0) {
            for (size_t i = 0u; i < argc; ++i) js_array_push(vm, a, args[i]);
            return js_num((double)a->len);
        }
        if (strcmp(mname, "pop") == 0) {
            return (a->len > 0u) ? a->items[--a->len] : js_undef();
        }
        if (strcmp(mname, "join") == 0) {
            const char *sep = (argc > 0u && args[0].kind != JS_VAL_UNDEFINED)
                                  ? js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1))
                                  : ",";
            size_t seplen = strlen(sep);
            JsonBuf jb = {0};
            for (size_t i = 0u; i < a->len; ++i) {
                if (i > 0u) jbuf_append(&vm->temp_arena, &jb, sep, seplen);
                if (a->items[i].kind != JS_VAL_NULL && a->items[i].kind != JS_VAL_UNDEFINED) {
                    const char *is = js_to_cstr_buf(vm, a->items[i], tmp2, sizeof(tmp2));
                    jbuf_append(&vm->temp_arena, &jb, is, strlen(is));
                }
            }
            return js_mk_str_n(vm, jb.buf ? jb.buf : "", jb.len);
        }
        if (strcmp(mname, "slice") == 0) {
            int32_t len = (int32_t)a->len;
            int32_t s = (argc > 0u) ? (int32_t)js_to_number(args[0]) : 0;
            int32_t e = (argc > 1u && args[1].kind != JS_VAL_UNDEFINED) ? (int32_t)js_to_number(args[1]) : len;
            if (s < 0) s = ARCR_MAX(0, len + s);
            if (e < 0) e = ARCR_MAX(0, len + e);
            if (e > len) e = len;
            JsArray *na = js_alloc_array(vm, (e > s) ? (size_t)(e - s) : 4u);
            for (int32_t i = s; na && i < e; ++i) js_array_push(vm, na, a->items[i]);
            JsVal v; memset(&v, 0, sizeof(v)); v.kind = JS_VAL_ARRAY; v.as.arr = na;
            return v;
        }
        if (strcmp(mname, "indexOf") == 0 && argc > 0u) {
            for (size_t i = 0u; i < a->len; ++i) {
                if (js_strict_equal(a->items[i], args[0])) return js_num((double)i);
            }
            return js_num(-1.0);
        }
        if (strcmp(mname, "includes") == 0 && argc > 0u) {
            for (size_t i = 0u; i < a->len; ++i) {
                if (js_strict_equal(a->items[i], args[0])) return js_bool(true);
            }
            return js_bool(false);
        }
        if ((strcmp(mname, "forEach") == 0 || strcmp(mname, "map") == 0 || strcmp(mname, "filter") == 0) && argc > 0u) {
            int mode = (mname[0] == 'm') ? 1 : (mname[1] == 'i') ? 2 : 0;
            JsArray *out_arr = (mode > 0) ? js_alloc_array(vm, a->len) : NULL;
            size_t snap_len = a->len;
            for (size_t i = 0u; i < snap_len; ++i) {
                JsVal cb_args[3] = { a->items[i], js_num((double)i), recv };
                JsVal rval = js_undef();
                if (!vm_invoke_callable(vm, args[0], js_undef(), cb_args, 3u, &rval)) break;
                if (mode == 1 && out_arr) js_array_push(vm, out_arr, rval);
                else if (mode == 2 && out_arr && js_is_truthy(rval)) js_array_push(vm, out_arr, a->items[i]);
            }
            if (mode == 0) return js_undef();
            JsVal rv; memset(&rv, 0, sizeof(rv)); rv.kind = JS_VAL_ARRAY; rv.as.arr = out_arr;
            return rv;
        }
    }

    /* 6. String Prototype Methods */
    if (recv.kind == JS_VAL_STRING && recv.as.str) {
        const char *s = recv.as.str->data;
        size_t slen   = recv.as.str->len;
        if (strcmp(mname, "slice") == 0 || strcmp(mname, "substring") == 0) {
            bool is_sub = (mname[1] == 'u');
            int32_t len = (int32_t)slen;
            int32_t st  = (argc > 0u) ? (int32_t)js_to_number(args[0]) : 0;
            int32_t en  = (argc > 1u && args[1].kind != JS_VAL_UNDEFINED) ? (int32_t)js_to_number(args[1]) : len;
            if (is_sub) {
                if (st < 0) st = 0;
                if (en < 0) en = 0;
                if (st > en) { int32_t t = st; st = en; en = t; }
            } else {
                if (st < 0) st = ARCR_MAX(0, len + st);
                if (en < 0) en = ARCR_MAX(0, len + en);
            }
            st = ARCR_CLAMP(st, 0, len);
            en = ARCR_CLAMP(en, 0, len);
            return (en > st) ? js_mk_str_n(vm, s + st, (size_t)(en - st)) : js_mk_str_n(vm, "", 0u);
        }
        if (strcmp(mname, "indexOf") == 0 && argc > 0u) {
            const char *sub = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
            const char *hit = strstr(s, sub);
            return hit ? js_num((double)(hit - s)) : js_num(-1.0);
        }
        if (strcmp(mname, "includes") == 0 && argc > 0u) {
            const char *sub = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
            return js_bool(strstr(s, sub) != NULL);
        }
        if (strcmp(mname, "startsWith") == 0 && argc > 0u) {
            const char *pre = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
            size_t pl = strlen(pre);
            return js_bool(pl <= slen && memcmp(s, pre, pl) == 0);
        }
        if (strcmp(mname, "endsWith") == 0 && argc > 0u) {
            const char *suf = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
            size_t fl = strlen(suf);
            return js_bool(fl <= slen && memcmp(s + slen - fl, suf, fl) == 0);
        }
        if (strcmp(mname, "trim") == 0) {
            const char *p = s; size_t l = slen;
            arcr_str_trim_view(&p, &l);
            return js_mk_str_n(vm, p, l);
        }
        if (strcmp(mname, "toLowerCase") == 0 || strcmp(mname, "toUpperCase") == 0) {
            bool up = (mname[2] == 'U');
            char *nb = (char *)arcr_arena_alloc(&vm->temp_arena, slen + 1u, 1u);
            if (!nb) return recv;
            for (size_t i = 0u; i < slen; ++i) {
                char c = s[i];
                if (up && c >= 'a' && c <= 'z') nb[i] = (char)(c - 'a' + 'A');
                else if (!up && c >= 'A' && c <= 'Z') nb[i] = (char)(c - 'A' + 'a');
                else nb[i] = c;
            }
            nb[slen] = '\0';
            return js_mk_str_n(vm, nb, slen);
        }
        if ((strcmp(mname, "replace") == 0 || strcmp(mname, "replaceAll") == 0) && argc >= 2u) {
            bool all = (mname[7] == 'A');
            const char *pat = js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1));
            const char *rep = js_to_cstr_buf(vm, args[1], tmp2, sizeof(tmp2));
            size_t plen = strlen(pat), rlen = strlen(rep);
            if (plen == 0u) return recv;
            JsonBuf jb = {0};
            const char *cur = s;
            for (;;) {
                const char *hit = strstr(cur, pat);
                if (!hit) { jbuf_append(&vm->temp_arena, &jb, cur, strlen(cur)); break; }
                jbuf_append(&vm->temp_arena, &jb, cur, (size_t)(hit - cur));
                jbuf_append(&vm->temp_arena, &jb, rep, rlen);
                cur = hit + plen;
                if (!all) { jbuf_append(&vm->temp_arena, &jb, cur, strlen(cur)); break; }
            }
            return js_mk_str_n(vm, jb.buf ? jb.buf : "", jb.len);
        }
        if (strcmp(mname, "split") == 0) {
            JsArray *arr = js_alloc_array(vm, 4u);
            if (!arr) return js_undef();
            const char *sep = (argc > 0u) ? js_to_cstr_buf(vm, args[0], tmp1, sizeof(tmp1)) : NULL;
            if (!sep) {
                js_array_push(vm, arr, recv);
            } else if (sep[0] == '\0') {
                for (size_t i = 0u; i < slen; ++i) js_array_push(vm, arr, js_mk_str_n(vm, s + i, 1u));
            } else {
                size_t seplen = strlen(sep);
                const char *cur = s;
                for (;;) {
                    const char *hit = strstr(cur, sep);
                    if (!hit) { js_array_push(vm, arr, js_mk_str(vm, cur)); break; }
                    js_array_push(vm, arr, js_mk_str_n(vm, cur, (size_t)(hit - cur)));
                    cur = hit + seplen;
                }
            }
            JsVal av; memset(&av, 0, sizeof(av)); av.kind = JS_VAL_ARRAY; av.as.arr = arr;
            return av;
        }
        if (strcmp(mname, "charAt") == 0) {
            int32_t idx = (argc > 0u) ? (int32_t)js_to_number(args[0]) : 0;
            if (idx < 0 || (size_t)idx >= slen) return js_mk_str_n(vm, "", 0u);
            return js_mk_str_n(vm, s + idx, 1u);
        }
        if (strcmp(mname, "charCodeAt") == 0) {
            int32_t idx = (argc > 0u) ? (int32_t)js_to_number(args[0]) : 0;
            if (idx < 0 || (size_t)idx >= slen) return js_num(js_make_nan());
            return js_num((double)(unsigned char)s[idx]);
        }
    }

    /* 7. User Object Method Call (`obj.fn(...)`) */
    if (recv.kind == JS_VAL_OBJECT && recv.as.obj) {
        JsVal fn_val = js_undef();
        if (js_obj_get(recv.as.obj, mname, &fn_val)) {
            JsVal ret = js_undef();
            (void)vm_invoke_callable(vm, fn_val, recv, args, argc, &ret);
            return ret;
        }
    }
    if (strcmp(mname, "toString") == 0) {
        return js_to_string_val(vm, recv);
    }
    return js_undef();
}

/* =========================================================================
 * 8. Bytecode Virtual Machine Executor, Event Dispatcher & Module VTable
 * ========================================================================= */
typedef struct JsCallFrame {
    JsChunk *chunk;
    JsEnv   *env;
    JsVal    this_val;
    size_t   ip;
    size_t   base_sp;
} JsCallFrame;

static JsVal vm_call_native_fn(ArcrJsVm *vm, JsNativeFnId nid, const JsVal *args, size_t argc) {
    char tmp[256];
    switch (nid) {
        case JS_NAT_PARSE_INT: {
            if (argc == 0u) return js_num(js_make_nan());
            const char *s = js_to_cstr_buf(vm, args[0], tmp, sizeof(tmp));
            size_t len = strlen(s);
            arcr_str_trim_view(&s, &len);
            if (len == 0u) return js_num(js_make_nan());
            int radix = (argc > 1u) ? (int)js_to_number(args[1]) : 0;
            size_t i = 0u;
            double sign = 1.0;
            if (s[i] == '-') { sign = -1.0; ++i; }
            else if (s[i] == '+') { ++i; }
            if ((radix == 0 || radix == 16) && i + 2u <= len &&
                s[i] == '0' && (s[i + 1u] == 'x' || s[i + 1u] == 'X')) {
                radix = 16;
                i += 2u;
            }
            if (radix == 0) radix = 10;
            if (radix < 2 || radix > 36) return js_num(js_make_nan());
            double val = 0.0;
            bool any = false;
            while (i < len) {
                char c = s[i++];
                int d = -1;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
                if (d < 0 || d >= radix) break;
                val = val * (double)radix + (double)d;
                any = true;
            }
            return any ? js_num(sign * val) : js_num(js_make_nan());
        }
        case JS_NAT_PARSE_FLOAT: {
            if (argc == 0u) return js_num(js_make_nan());
            const char *s = js_to_cstr_buf(vm, args[0], tmp, sizeof(tmp));
            return js_num(js_parse_float_str(s, strlen(s), true));
        }
        case JS_NAT_IS_NAN:
            return js_bool(argc == 0u || js_num_is_nan(js_to_number(args[0])));
        case JS_NAT_IS_FINITE: {
            if (argc == 0u) return js_bool(false);
            double n = js_to_number(args[0]);
            return js_bool(!js_num_is_nan(n) && !js_num_is_inf(n));
        }
        case JS_NAT_NUMBER:
            return (argc == 0u) ? js_num(0.0) : js_num(js_to_number(args[0]));
        case JS_NAT_STRING:
            return (argc == 0u) ? js_mk_str_n(vm, "", 0u) : js_to_string_val(vm, args[0]);
        case JS_NAT_BOOLEAN:
            return js_bool(argc > 0u && js_is_truthy(args[0]));
        case JS_NAT_EVENT_PREVENT_DEFAULT:
            if (vm->active_ev_prevented) *vm->active_ev_prevented = true;
            return js_undef();
        case JS_NAT_EVENT_STOP_PROP:
            if (vm->active_ev_stopped) *vm->active_ev_stopped = true;
            return js_undef();
    }
    return js_undef();
}

static bool vm_run_frames(ArcrJsVm *vm, JsChunk *entry_chunk, JsEnv *entry_env,
                          JsVal entry_this, JsVal *out_ret) {
    JsCallFrame frames[JS_MAX_CALL_DEPTH];
    size_t fdepth = 0u;
    size_t saved_try_depth = vm->try_depth;
    size_t entry_sp        = vm->sp;

    frames[0].chunk    = entry_chunk;
    frames[0].env      = entry_env;
    frames[0].this_val = entry_this;
    frames[0].ip       = 0u;
    frames[0].base_sp  = entry_sp;
    fdepth             = 1u;

    #define VM_PUSH(v) do { JsVal _vp = (v); if (vm->sp >= JS_MAX_STACK_SLOTS) { vm->sp = entry_sp; vm->try_depth = saved_try_depth; return false; } vm->stack[vm->sp++] = _vp; } while (0)
    #define VM_POP()   ((vm->sp > 0u) ? vm->stack[--vm->sp] : js_undef())
    #define VM_PEEK(k) ((vm->sp > (k)) ? vm->stack[vm->sp - 1u - (k)] : js_undef())

    while (fdepth > 0u) {
        if (vm->has_exception) {
            if (vm->try_depth > saved_try_depth) {
                JsTryFrame tf = vm->try_stack[--vm->try_depth];
                fdepth = tf.frame_depth;
                vm->sp = tf.stack_sp;
                frames[fdepth - 1u].chunk = tf.chunk;
                frames[fdepth - 1u].env   = tf.env;
                frames[fdepth - 1u].ip    = tf.catch_ip;
                vm->has_exception         = false;
                VM_PUSH(vm->thrown_val);
                continue;
            }
            vm->sp        = entry_sp;
            vm->try_depth = saved_try_depth;
            return false;
        }

        if (vm->fuel == 0u) {
            vm->sp        = entry_sp;
            vm->try_depth = saved_try_depth;
            return false; /* Instruction fuel exhausted: abort infinite loop safely! */
        }
        --vm->fuel;

        JsCallFrame *fr = &frames[fdepth - 1u];
        if (fr->ip >= fr->chunk->code_len) {
            if (fdepth == 1u) break;
            --fdepth;
            continue;
        }
        JsInst ins = fr->chunk->code[fr->ip++];

        switch ((JsOpCode)ins.op) {
            case OP_CONST:
                VM_PUSH((ins.arg < fr->chunk->const_len) ? fr->chunk->consts[ins.arg] : js_undef());
                break;
            case OP_POP:
                (void)VM_POP();
                break;
            case OP_DUP:
                VM_PUSH(VM_PEEK(0u));
                break;
            case OP_DUP2: {
                JsVal b = VM_PEEK(0u);
                JsVal a = VM_PEEK(1u);
                VM_PUSH(a);
                VM_PUSH(b);
                break;
            }
            case OP_SWAP:
                if (vm->sp >= 2u) {
                    JsVal t = vm->stack[vm->sp - 1u];
                    vm->stack[vm->sp - 1u] = vm->stack[vm->sp - 2u];
                    vm->stack[vm->sp - 2u] = t;
                }
                break;
            case OP_THIS:
                VM_PUSH(fr->this_val);
                break;
            case OP_GET_LOCAL: {
                JsVal *sl = env_slot_ptr(fr->env, ins.arg);
                VM_PUSH(sl ? *sl : js_undef());
                break;
            }
            case OP_SET_LOCAL: {
                JsVal v = VM_PEEK(0u);
                JsVal *sl = env_slot_ptr(fr->env, ins.arg);
                if (sl) *sl = v;
                break;
            }
            case OP_GET_GLOBAL: {
                const char *gname = (ins.arg < fr->chunk->const_len && fr->chunk->consts[ins.arg].kind == JS_VAL_STRING)
                                        ? fr->chunk->consts[ins.arg].as.str->data : "";
                JsVal gv = js_undef();
                if (!js_obj_get(vm->globals, gname, &gv) && ins.aux == 0u) {
                    gv = js_undef();
                }
                VM_PUSH(gv);
                break;
            }
            case OP_SET_GLOBAL: {
                const char *gname = (ins.arg < fr->chunk->const_len && fr->chunk->consts[ins.arg].kind == JS_VAL_STRING)
                                        ? fr->chunk->consts[ins.arg].as.str->data : "";
                js_obj_set(vm, vm->globals, gname, VM_PEEK(0u));
                break;
            }
            case OP_GET_PROP: {
                JsVal recv = VM_POP();
                const char *pname = (ins.arg < fr->chunk->const_len && fr->chunk->consts[ins.arg].kind == JS_VAL_STRING)
                                        ? fr->chunk->consts[ins.arg].as.str->data : "";
                VM_PUSH(vm_get_prop(vm, recv, pname));
                break;
            }
            case OP_SET_PROP: {
                JsVal val  = VM_POP();
                JsVal recv = VM_POP();
                const char *pname = (ins.arg < fr->chunk->const_len && fr->chunk->consts[ins.arg].kind == JS_VAL_STRING)
                                        ? fr->chunk->consts[ins.arg].as.str->data : "";
                vm_set_prop(vm, recv, pname, val);
                VM_PUSH(val);
                break;
            }
            case OP_GET_INDEX: {
                JsVal idx = VM_POP();
                JsVal obj = VM_POP();
                if (obj.kind == JS_VAL_ARRAY && obj.as.arr) {
                    int32_t i = (int32_t)js_to_number(idx);
                    if (i >= 0 && (size_t)i < obj.as.arr->len) VM_PUSH(obj.as.arr->items[i]);
                    else VM_PUSH(js_undef());
                } else if (obj.kind == JS_VAL_STRING && obj.as.str) {
                    int32_t i = (int32_t)js_to_number(idx);
                    if (i >= 0 && (size_t)i < obj.as.str->len)
                        VM_PUSH(js_mk_str_n(vm, obj.as.str->data + i, 1u));
                    else VM_PUSH(js_undef());
                } else {
                    char kb[128];
                    const char *ks = js_to_cstr_buf(vm, idx, kb, sizeof(kb));
                    VM_PUSH(vm_get_prop(vm, obj, ks));
                }
                break;
            }
            case OP_SET_INDEX: {
                JsVal val = VM_POP();
                JsVal idx = VM_POP();
                JsVal obj = VM_POP();
                if (obj.kind == JS_VAL_ARRAY && obj.as.arr) {
                    int32_t i = (int32_t)js_to_number(idx);
                    if (i >= 0 && i < 65536) {
                        while (obj.as.arr->len <= (size_t)i) {
                            if (!js_array_push(vm, obj.as.arr, js_undef())) break;
                        }
                        if ((size_t)i < obj.as.arr->len) obj.as.arr->items[i] = val;
                    }
                } else {
                    char kb[128];
                    const char *ks = js_to_cstr_buf(vm, idx, kb, sizeof(kb));
                    vm_set_prop(vm, obj, ks, val);
                }
                VM_PUSH(val);
                break;
            }
            case OP_ADD: {
                JsVal b = VM_POP();
                JsVal a = VM_POP();
                if (a.kind == JS_VAL_STRING || b.kind == JS_VAL_STRING ||
                    a.kind == JS_VAL_ARRAY  || b.kind == JS_VAL_ARRAY) {
                    char ta[512], tb[512];
                    const char *sa = js_to_cstr_buf(vm, a, ta, sizeof(ta));
                    const char *sb = js_to_cstr_buf(vm, b, tb, sizeof(tb));
                    size_t la = strlen(sa), lb = strlen(sb);
                    JsString *st = (JsString *)vm_alloc(vm, sizeof(JsString) + la + lb + 1u, sizeof(void *));
                    if (st) {
                        st->len = la + lb;
                        if (la > 0u) memcpy(st->data, sa, la);
                        if (lb > 0u) memcpy(st->data + la, sb, lb);
                        st->data[la + lb] = '\0';
                        JsVal sv; memset(&sv, 0, sizeof(sv)); sv.kind = JS_VAL_STRING; sv.as.str = st;
                        VM_PUSH(sv);
                    } else {
                        VM_PUSH(js_undef());
                    }
                } else {
                    VM_PUSH(js_num(js_to_number(a) + js_to_number(b)));
                }
                break;
            }
            case OP_SUB: { JsVal b = VM_POP(), a = VM_POP(); VM_PUSH(js_num(js_to_number(a) - js_to_number(b))); break; }
            case OP_MUL: { JsVal b = VM_POP(), a = VM_POP(); VM_PUSH(js_num(js_to_number(a) * js_to_number(b))); break; }
            case OP_DIV: { JsVal b = VM_POP(), a = VM_POP(); VM_PUSH(js_num(js_to_number(a) / js_to_number(b))); break; }
            case OP_MOD: { JsVal b = VM_POP(), a = VM_POP(); VM_PUSH(js_num(js_math_fmod(js_to_number(a), js_to_number(b)))); break; }
            case OP_NEG: { JsVal a = VM_POP(); VM_PUSH(js_num(-js_to_number(a))); break; }
            case OP_POS: { JsVal a = VM_POP(); VM_PUSH(js_num(js_to_number(a))); break; }
            case OP_NOT: { JsVal a = VM_POP(); VM_PUSH(js_bool(!js_is_truthy(a))); break; }
            case OP_TYPEOF: {
                JsVal a = VM_POP();
                const char *ts = "object";
                switch (a.kind) {
                    case JS_VAL_UNDEFINED: ts = "undefined"; break;
                    case JS_VAL_BOOL:      ts = "boolean"; break;
                    case JS_VAL_NUMBER:    ts = "number"; break;
                    case JS_VAL_STRING:    ts = "string"; break;
                    case JS_VAL_FUNCTION:
                    case JS_VAL_NATIVE_FN: ts = "function"; break;
                    default:               ts = "object"; break;
                }
                VM_PUSH(js_mk_str(vm, ts));
                break;
            }
            case OP_EQ:         { JsVal b = VM_POP(), a = VM_POP(); VM_PUSH(js_bool(js_loose_equal(a, b))); break; }
            case OP_NEQ:        { JsVal b = VM_POP(), a = VM_POP(); VM_PUSH(js_bool(!js_loose_equal(a, b))); break; }
            case OP_STRICT_EQ:  { JsVal b = VM_POP(), a = VM_POP(); VM_PUSH(js_bool(js_strict_equal(a, b))); break; }
            case OP_STRICT_NEQ: { JsVal b = VM_POP(), a = VM_POP(); VM_PUSH(js_bool(!js_strict_equal(a, b))); break; }
            case OP_LT:
            case OP_LTE:
            case OP_GT:
            case OP_GTE: {
                JsVal b = VM_POP(), a = VM_POP();
                bool res = false;
                if (a.kind == JS_VAL_STRING && b.kind == JS_VAL_STRING && a.as.str && b.as.str) {
                    int cmp = strcmp(a.as.str->data, b.as.str->data);
                    if (ins.op == OP_LT) res = (cmp < 0);
                    else if (ins.op == OP_LTE) res = (cmp <= 0);
                    else if (ins.op == OP_GT) res = (cmp > 0);
                    else res = (cmp >= 0);
                } else {
                    double na = js_to_number(a), nb = js_to_number(b);
                    if (!js_num_is_nan(na) && !js_num_is_nan(nb)) {
                        if (ins.op == OP_LT) res = (na < nb);
                        else if (ins.op == OP_LTE) res = (na <= nb);
                        else if (ins.op == OP_GT) res = (na > nb);
                        else res = (na >= nb);
                    }
                }
                VM_PUSH(js_bool(res));
                break;
            }
            case OP_BIT_AND: { JsVal b = VM_POP(), a = VM_POP(); VM_PUSH(js_num((double)((int32_t)js_to_number(a) & (int32_t)js_to_number(b)))); break; }
            case OP_BIT_OR:  { JsVal b = VM_POP(), a = VM_POP(); VM_PUSH(js_num((double)((int32_t)js_to_number(a) | (int32_t)js_to_number(b)))); break; }
            case OP_BIT_XOR: { JsVal b = VM_POP(), a = VM_POP(); VM_PUSH(js_num((double)((int32_t)js_to_number(a) ^ (int32_t)js_to_number(b)))); break; }
            case OP_SHL:     { JsVal b = VM_POP(), a = VM_POP(); VM_PUSH(js_num((double)((int32_t)js_to_number(a) << ((uint32_t)(int32_t)js_to_number(b) & 31u)))); break; }
            case OP_SHR:     { JsVal b = VM_POP(), a = VM_POP(); VM_PUSH(js_num((double)((int32_t)js_to_number(a) >> ((uint32_t)(int32_t)js_to_number(b) & 31u)))); break; }
            case OP_JMP:
                fr->ip = (size_t)ins.arg;
                break;
            case OP_JMP_IF_FALSE:
                if (!js_is_truthy(VM_PEEK(0u))) fr->ip = (size_t)ins.arg;
                break;
            case OP_JMP_IF_TRUE:
                if (js_is_truthy(VM_PEEK(0u))) fr->ip = (size_t)ins.arg;
                break;
            case OP_CLOSURE: {
                JsVal proto_v = (ins.arg < fr->chunk->const_len) ? fr->chunk->consts[ins.arg] : js_undef();
                if (proto_v.kind == JS_VAL_FUNCTION && proto_v.as.fn) {
                    JsFunc *nf = (JsFunc *)vm_alloc(vm, sizeof(JsFunc), sizeof(void *));
                    if (nf) {
                        nf->chunk        = proto_v.as.fn->chunk;
                        nf->captured_env = fr->env;
                        JsVal fv; memset(&fv, 0, sizeof(fv)); fv.kind = JS_VAL_FUNCTION; fv.as.fn = nf;
                        VM_PUSH(fv);
                    } else {
                        VM_PUSH(js_undef());
                    }
                } else {
                    VM_PUSH(js_undef());
                }
                break;
            }
            case OP_NEW_ARRAY: {
                uint32_t cnt = ins.arg;
                JsArray *arr = js_alloc_array(vm, cnt);
                if (arr && vm->sp >= cnt) {
                    size_t start = vm->sp - cnt;
                    for (uint32_t i = 0u; i < cnt; ++i) js_array_push(vm, arr, vm->stack[start + i]);
                    vm->sp = start;
                }
                JsVal av; memset(&av, 0, sizeof(av)); av.kind = JS_VAL_ARRAY; av.as.arr = arr;
                VM_PUSH(av);
                break;
            }
            case OP_NEW_OBJECT: {
                uint32_t pairs = ins.arg;
                JsObject *obj = js_alloc_object(vm, pairs);
                if (obj && vm->sp >= pairs * 2u) {
                    size_t start = vm->sp - pairs * 2u;
                    for (uint32_t i = 0u; i < pairs; ++i) {
                        JsVal kv = vm->stack[start + i * 2u];
                        JsVal vv = vm->stack[start + i * 2u + 1u];
                        if (kv.kind == JS_VAL_STRING && kv.as.str) {
                            js_obj_set(vm, obj, kv.as.str->data, vv);
                        }
                    }
                    vm->sp = start;
                }
                JsVal ov; memset(&ov, 0, sizeof(ov)); ov.kind = JS_VAL_OBJECT; ov.as.obj = obj;
                VM_PUSH(ov);
                break;
            }
            case OP_CALL: {
                size_t argc = (size_t)ins.arg;
                bool is_new = (ins.aux == 1u);
                if (vm->sp < argc + 1u) { VM_PUSH(js_undef()); break; }
                size_t fn_pos = vm->sp - argc - 1u;
                JsVal callee  = vm->stack[fn_pos];
                const JsVal *call_args = &vm->stack[fn_pos + 1u];

                if (callee.kind == JS_VAL_FUNCTION && callee.as.fn && !is_new &&
                    fdepth < JS_MAX_CALL_DEPTH) {
                    JsChunk *fch = callee.as.fn->chunk;
                    size_t nslots = ARCR_MAX((size_t)fch->local_count, (size_t)fch->param_count);
                    JsEnv *fenv = alloc_env(vm, callee.as.fn->captured_env, nslots);
                    if (!fenv) { vm->sp = fn_pos; VM_PUSH(js_undef()); break; }
                    for (size_t i = 0u; i < argc && i < fch->param_count; ++i) {
                        fenv->slots[i] = call_args[i];
                    }
                    vm->sp = fn_pos;
                    frames[fdepth].chunk    = fch;
                    frames[fdepth].env      = fenv;
                    frames[fdepth].this_val = js_undef();
                    frames[fdepth].ip       = 0u;
                    frames[fdepth].base_sp  = fn_pos;
                    ++fdepth;
                } else {
                    JsVal args_copy[16];
                    size_t c_argc = ARCR_MIN(argc, 16u);
                    for (size_t i = 0u; i < c_argc; ++i) args_copy[i] = call_args[i];
                    vm->sp = fn_pos;
                    JsVal this_obj = js_undef();
                    if (is_new) {
                        JsObject *no = js_alloc_object(vm, 4u);
                        this_obj.kind = JS_VAL_OBJECT;
                        this_obj.as.obj = no;
                    }
                    JsVal ret = js_undef();
                    (void)vm_invoke_callable(vm, callee, this_obj, args_copy, c_argc, &ret);
                    if (is_new && ret.kind == JS_VAL_UNDEFINED && this_obj.kind == JS_VAL_OBJECT) {
                        ret = this_obj;
                    }
                    VM_PUSH(ret);
                }
                break;
            }
            case OP_CALL_METHOD: {
                size_t argc = (size_t)ins.aux;
                const char *mname = (ins.arg < fr->chunk->const_len && fr->chunk->consts[ins.arg].kind == JS_VAL_STRING)
                                        ? fr->chunk->consts[ins.arg].as.str->data : "";
                if (vm->sp < argc + 1u) { VM_PUSH(js_undef()); break; }
                size_t recv_pos = vm->sp - argc - 1u;
                JsVal recv      = vm->stack[recv_pos];
                JsVal args_copy[16];
                size_t c_argc = ARCR_MIN(argc, 16u);
                for (size_t i = 0u; i < c_argc; ++i) args_copy[i] = vm->stack[recv_pos + 1u + i];
                vm->sp = recv_pos;
                JsVal res = vm_call_method(vm, recv, mname, args_copy, c_argc);
                VM_PUSH(res);
                break;
            }
            case OP_TRY_PUSH:
                if (vm->try_depth < JS_MAX_TRY_DEPTH) {
                    JsTryFrame *tf = &vm->try_stack[vm->try_depth++];
                    tf->frame_depth = fdepth;
                    tf->stack_sp    = vm->sp;
                    tf->chunk       = fr->chunk;
                    tf->env         = fr->env;
                    tf->catch_ip    = (size_t)ins.arg;
                }
                break;
            case OP_TRY_POP:
                if (vm->try_depth > saved_try_depth) --vm->try_depth;
                break;
            case OP_THROW:
                vm->thrown_val    = VM_POP();
                vm->has_exception = true;
                break;
            case OP_RETURN: {
                JsVal rv = VM_POP();
                vm->sp = fr->base_sp;
                --fdepth;
                if (fdepth == 0u) {
                    if (out_ret) *out_ret = rv;
                    vm->try_depth = saved_try_depth;
                    return true;
                }
                VM_PUSH(rv);
                break;
            }
            case OP_HALT:
                vm->sp        = entry_sp;
                vm->try_depth = saved_try_depth;
                if (out_ret) *out_ret = js_undef();
                return true;
        }
    }

    #undef VM_PUSH
    #undef VM_POP
    #undef VM_PEEK

    vm->sp        = entry_sp;
    vm->try_depth = saved_try_depth;
    if (out_ret) *out_ret = js_undef();
    return !vm->has_exception;
}

static bool vm_invoke_callable(ArcrJsVm *vm, JsVal callee, JsVal this_val,
                               const JsVal *args, size_t argc, JsVal *out_ret) {
    if (out_ret) *out_ret = js_undef();
    if (callee.kind == JS_VAL_NATIVE_FN) {
        JsVal r = vm_call_native_fn(vm, (JsNativeFnId)callee.as.u32, args, argc);
        if (out_ret) *out_ret = r;
        return !vm->has_exception;
    }
    if (callee.kind == JS_VAL_BUILTIN_OBJ) {
        JsBuiltinId bid = (JsBuiltinId)callee.as.u32;
        if (bid == JS_BUILTIN_NUMBER_CTOR) {
            if (out_ret) *out_ret = (argc > 0u) ? js_num(js_to_number(args[0])) : js_num(0.0);
            return true;
        }
        if (bid == JS_BUILTIN_STRING_CTOR) {
            if (out_ret) *out_ret = (argc > 0u) ? js_to_string_val(vm, args[0]) : js_mk_str_n(vm, "", 0u);
            return true;
        }
        if (bid == JS_BUILTIN_ARRAY_CTOR) {
            JsArray *a = js_alloc_array(vm, (argc == 1u && args[0].kind == JS_VAL_NUMBER)
                                                ? (size_t)ARCR_CLAMP((int32_t)args[0].as.num, 0, 4096) : argc);
            if (a) {
                if (argc == 1u && args[0].kind == JS_VAL_NUMBER) {
                    int32_t cnt = ARCR_CLAMP((int32_t)args[0].as.num, 0, 4096);
                    for (int32_t i = 0; i < cnt; ++i) js_array_push(vm, a, js_undef());
                } else {
                    for (size_t i = 0u; i < argc; ++i) js_array_push(vm, a, args[i]);
                }
            }
            if (out_ret) { out_ret->kind = JS_VAL_ARRAY; out_ret->as.arr = a; }
            return true;
        }
        if (bid == JS_BUILTIN_OBJECT_CTOR) {
            JsObject *o = js_alloc_object(vm, 4u);
            if (out_ret) { out_ret->kind = JS_VAL_OBJECT; out_ret->as.obj = o; }
            return true;
        }
        return false;
    }
    if (callee.kind == JS_VAL_FUNCTION && callee.as.fn) {
        JsChunk *ch = callee.as.fn->chunk;
        size_t nslots = ARCR_MAX((size_t)ch->local_count, (size_t)ch->param_count);
        JsEnv *env = alloc_env(vm, callee.as.fn->captured_env, nslots);
        if (!env) return false;
        for (size_t i = 0u; i < argc && i < ch->param_count; ++i) {
            env->slots[i] = args[i];
        }
        return vm_run_frames(vm, ch, env, this_val, out_ret);
    }
    return false;
}

static void bind_builtin_obj(ArcrJsVm *vm, const char *name, JsBuiltinId id) {
    JsVal v; memset(&v, 0, sizeof(v));
    v.kind   = JS_VAL_BUILTIN_OBJ;
    v.as.u32 = (uint32_t)id;
    js_obj_set(vm, vm->globals, name, v);
}

static void bind_native_fn(ArcrJsVm *vm, const char *name, JsNativeFnId id) {
    JsVal v; memset(&v, 0, sizeof(v));
    v.kind   = JS_VAL_NATIVE_FN;
    v.as.u32 = (uint32_t)id;
    js_obj_set(vm, vm->globals, name, v);
}

static ArcrJsVm *portable_create_vm(size_t heap_limit_bytes,
                                    const ArcrJsHostCallbacks *host_cb) {
    ArcrJsVm *vm = (ArcrJsVm *)calloc(1u, sizeof(ArcrJsVm));
    if (!vm) return NULL;
    arcr_arena_init(&vm->vm_arena, 32768u);
    arcr_arena_init(&vm->temp_arena, 8192u);
    vm->heap_limit = (heap_limit_bytes > 0u) ? heap_limit_bytes : (8u * 1024u * 1024u);
    if (host_cb) vm->host_cb = *host_cb;
    vm->start_monotonic_usec = arcr_platform_monotonic_usec();
    arcr_str_copy(vm->location_href, sizeof(vm->location_href), "about:blank");

    if (vm->host_cb.doc && vm->host_cb.doc->title) {
        arcr_str_copy(vm->doc_title, sizeof(vm->doc_title), vm->host_cb.doc->title);
    }

    vm->globals = js_alloc_object(vm, 32u);
    if (!vm->globals) {
        arcr_arena_destroy(&vm->temp_arena);
        arcr_arena_destroy(&vm->vm_arena);
        free(vm);
        return NULL;
    }

    bind_builtin_obj(vm, "console",      JS_BUILTIN_CONSOLE);
    bind_builtin_obj(vm, "Math",         JS_BUILTIN_MATH);
    bind_builtin_obj(vm, "JSON",         JS_BUILTIN_JSON);
    bind_builtin_obj(vm, "performance",  JS_BUILTIN_PERFORMANCE);
    bind_builtin_obj(vm, "Date",         JS_BUILTIN_DATE);
    bind_builtin_obj(vm, "localStorage", JS_BUILTIN_LOCALSTORAGE);
    bind_builtin_obj(vm, "document",     JS_BUILTIN_DOCUMENT);
    bind_builtin_obj(vm, "window",       JS_BUILTIN_WINDOW);
    bind_builtin_obj(vm, "globalThis",   JS_BUILTIN_WINDOW);
    bind_builtin_obj(vm, "location",     JS_BUILTIN_LOCATION);
    bind_builtin_obj(vm, "Array",        JS_BUILTIN_ARRAY_CTOR);
    bind_builtin_obj(vm, "Object",       JS_BUILTIN_OBJECT_CTOR);

    bind_native_fn(vm, "parseInt",   JS_NAT_PARSE_INT);
    bind_native_fn(vm, "parseFloat", JS_NAT_PARSE_FLOAT);
    bind_native_fn(vm, "isNaN",      JS_NAT_IS_NAN);
    bind_native_fn(vm, "isFinite",   JS_NAT_IS_FINITE);
    bind_native_fn(vm, "Number",     JS_NAT_NUMBER);
    bind_native_fn(vm, "String",     JS_NAT_STRING);
    bind_native_fn(vm, "Boolean",    JS_NAT_BOOLEAN);

    js_obj_set(vm, vm->globals, "NaN",      js_num(js_make_nan()));
    js_obj_set(vm, vm->globals, "Infinity", js_num(1e308 * 10.0));
    return vm;
}

static void portable_destroy_vm(ArcrJsVm *vm) {
    if (!vm) return;
    arcr_arena_destroy(&vm->temp_arena);
    arcr_arena_destroy(&vm->vm_arena);
    free(vm);
}

static bool portable_eval_script(ArcrJsVm *vm, const char *utf8_code, size_t code_len) {
    if (!vm || !utf8_code) return false;
    if (vm->vm_arena.total_used + code_len > vm->heap_limit) return false;

    vm->has_exception = false;
    vm->sp            = 0u;
    vm->try_depth     = 0u;
    vm->fuel          = JS_FUEL_BUDGET_PER_CALL;

    JsChunk *chunk = compile_script_to_chunk(vm, utf8_code, code_len);
    if (!chunk) return false;

    vm->bytecode_cache_bytes += ARCR_MAX(64u, code_len / 2u);
    vm->executed_scripts++;

    JsEnv *top_env = alloc_env(vm, NULL, ARCR_MAX(1u, (size_t)chunk->local_count));
    if (!top_env) return false;

    JsVal win_this = { JS_VAL_BUILTIN_OBJ, { .u32 = JS_BUILTIN_WINDOW } };
    JsVal ret      = js_undef();
    return vm_run_frames(vm, chunk, top_env, win_this, &ret);
}

static bool portable_dispatch_dom_event(ArcrJsVm *vm, uint32_t target_node_uid,
                                        const char *event_type) {
    if (!vm || !event_type || target_node_uid == 0u) return false;
    ArcrDocument *doc = vm->host_cb.doc;
    ArcrDomNode *target = doc ? js_dom_find_uid(vm, doc, target_node_uid) : NULL;

    bool prevented = false;
    bool stopped   = false;
    vm->active_ev_prevented = &prevented;
    vm->active_ev_stopped   = &stopped;
    vm->has_exception       = false;
    vm->fuel                = JS_FUEL_BUDGET_PER_CALL;

    JsObject *ev_obj = js_alloc_object(vm, 8u);
    if (!ev_obj) {
        vm->active_ev_prevented = NULL;
        vm->active_ev_stopped   = NULL;
        return false;
    }
    js_obj_set(vm, ev_obj, "type",   js_mk_str(vm, event_type));
    js_obj_set(vm, ev_obj, "target", js_dom_node(target_node_uid));
    JsVal pd_fn = { JS_VAL_NATIVE_FN, { .u32 = JS_NAT_EVENT_PREVENT_DEFAULT } };
    JsVal sp_fn = { JS_VAL_NATIVE_FN, { .u32 = JS_NAT_EVENT_STOP_PROP } };
    js_obj_set(vm, ev_obj, "preventDefault",  pd_fn);
    js_obj_set(vm, ev_obj, "stopPropagation", sp_fn);

    JsVal ev_val; memset(&ev_val, 0, sizeof(ev_val));
    ev_val.kind   = JS_VAL_OBJECT;
    ev_val.as.obj = ev_obj;
    js_obj_set(vm, vm->globals, "event", ev_val);

    char attr_name[40];
    snprintf(attr_name, sizeof(attr_name), "on%s", event_type);

    bool handled = false;
    if (target) {
        for (ArcrDomNode *cur = target; cur != NULL && !stopped; cur = cur->parent) {
            if (cur->kind != ARCR_NODE_ELEMENT) continue;
            js_obj_set(vm, ev_obj, "currentTarget", js_dom_node(cur->node_uid));

            /* 1. Inline attribute handler (e.g., onclick="...") */
            const char *inline_js = js_dom_get_attr(doc, cur, attr_name);
            if (inline_js && inline_js[0] != '\0') {
                JsChunk *ich = compile_script_to_chunk(vm, inline_js, strlen(inline_js));
                if (ich) {
                    JsEnv *ienv = alloc_env(vm, NULL, ARCR_MAX(1u, (size_t)ich->local_count));
                    if (ienv) {
                        JsVal dummy = js_undef();
                        if (vm_run_frames(vm, ich, ienv, js_dom_node(cur->node_uid), &dummy)) {
                            handled = true;
                        }
                    }
                }
            }

            /* 2. Registered addEventListener callbacks on `cur->node_uid` */
            for (size_t i = 0u; i < JS_MAX_EVENT_LISTENERS; ++i) {
                if (vm->listeners[i].active &&
                    vm->listeners[i].node_uid == cur->node_uid &&
                    arcr_str_eq_nocase(vm->listeners[i].event_type, event_type)) {
                    JsVal ret = js_undef();
                    if (vm_invoke_callable(vm, vm->listeners[i].callback,
                                           js_dom_node(cur->node_uid), &ev_val, 1u, &ret)) {
                        handled = true;
                    }
                }
            }
            js_obj_set(vm, ev_obj, "defaultPrevented", js_bool(prevented));
        }
    } else {
        /* Fallback when VM has no bound ArcrDocument */
        for (size_t i = 0u; i < JS_MAX_EVENT_LISTENERS; ++i) {
            if (vm->listeners[i].active &&
                vm->listeners[i].node_uid == target_node_uid &&
                arcr_str_eq_nocase(vm->listeners[i].event_type, event_type)) {
                JsVal ret = js_undef();
                if (vm_invoke_callable(vm, vm->listeners[i].callback,
                                       js_dom_node(target_node_uid), &ev_val, 1u, &ret)) {
                    handled = true;
                }
            }
        }
    }

    vm->active_ev_prevented = NULL;
    vm->active_ev_stopped   = NULL;
    return handled || prevented;
}

static size_t portable_get_used_heap_bytes(const ArcrJsVm *vm) {
    if (!vm) return 0u;
    return sizeof(ArcrJsVm) + vm->vm_arena.total_used +
           vm->temp_arena.total_used + vm->bytecode_cache_bytes;
}

static void portable_release_caches_on_pressure(ArcrJsVm *vm, int pressure_level) {
    if (!vm) return;
    if (pressure_level >= 1) {
        vm->bytecode_cache_bytes = 0u;
        arcr_arena_reset(&vm->temp_arena);
    }
}

static const ArcrJsVTable kPortableJsVTable = {
    .struct_size                = sizeof(ArcrJsVTable),
    .engine_backend_name        = "ArcradientJS-Portable (Pure C11 Bytecode VM)",
    .create_vm                  = portable_create_vm,
    .destroy_vm                 = portable_destroy_vm,
    .eval_script                = portable_eval_script,
    .dispatch_dom_event         = portable_dispatch_dom_event,
    .get_used_heap_bytes        = portable_get_used_heap_bytes,
    .release_caches_on_pressure = portable_release_caches_on_pressure
};

static ArcrModuleDescriptor g_js_desc = {
    .magic              = ARCR_MODULE_MAGIC,
    .struct_size        = sizeof(ArcrModuleDescriptor),
    .abi_version        = ARCR_ABI_VERSION,
    .engine_build_id    = ARCR_ENGINE_BUILD_ID,
    .arch_id            = 0,
    .min_os_major_minor = ARCR_OS_POSIX_ANY,
    .required_cpu_flags = ARCR_CPU_SCALAR, /* Runs on Win2000 x86 SSE1 or Scalar! */
    .module_name        = "arcr-js-portable",
    .vtable             = &kPortableJsVTable
};

const ArcrModuleDescriptor *arcr_js_portable_module_query(void);
const ArcrModuleDescriptor *arcr_js_portable_module_query(void) {
    g_js_desc.arch_id = (uint16_t)arcr_host_compile_arch();
#if defined(_WIN32)
    g_js_desc.min_os_major_minor = ARCR_OS_WIN2000;
#endif
    return &g_js_desc;
}

#if !defined(ARCR_JS_STATIC_LINK)
ARCR_API const ArcrModuleDescriptor *arcr_module_query(void) {
    return arcr_js_portable_module_query();
}
#endif
