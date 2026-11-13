/*
 * ArcradientJS 1.0 (Clean-Room Pure C11 ECMAScript 2026 Virtual Machine)
 * js/builtins/ajs_typedarray.c - Resizable ArrayBuffer, SharedArrayBuffer, DataView
 * (incl. Float16), %TypedArray% + 11 Views, Uint8Array Base64/Hex & Atomics
 */
#include "../vm/ajs_internal.h"

#define AJS_METHOD_FLAGS (AJS_PROP_WRITABLE | AJS_PROP_CONFIGURABLE)

/* ============================================================================
 * 1. Element Sizes, Endianness, Float16 & Clamping Helpers
 * ============================================================================ */

size_t ajs_ta_elem_size(AjsTypedArrayKind kind) {
    switch (kind) {
        case AJS_TA_INT8:
        case AJS_TA_UINT8:
        case AJS_TA_UINT8_CLAMPED: return 1u;
        case AJS_TA_INT16:
        case AJS_TA_UINT16:        return 2u;
        case AJS_TA_INT32:
        case AJS_TA_UINT32:
        case AJS_TA_FLOAT32:       return 4u;
        case AJS_TA_FLOAT64:
        case AJS_TA_BIGINT64:
        case AJS_TA_BIGUINT64:     return 8u;
        default:                   return 0u;
    }
}

static const char *ta_kind_name(AjsTypedArrayKind kind) {
    switch (kind) {
        case AJS_TA_INT8:          return "Int8Array";
        case AJS_TA_UINT8:         return "Uint8Array";
        case AJS_TA_UINT8_CLAMPED: return "Uint8ClampedArray";
        case AJS_TA_INT16:         return "Int16Array";
        case AJS_TA_UINT16:        return "Uint16Array";
        case AJS_TA_INT32:         return "Int32Array";
        case AJS_TA_UINT32:        return "Uint32Array";
        case AJS_TA_FLOAT32:       return "Float32Array";
        case AJS_TA_FLOAT64:       return "Float64Array";
        case AJS_TA_BIGINT64:      return "BigInt64Array";
        case AJS_TA_BIGUINT64:     return "BigUint64Array";
        default:                   return "TypedArray";
    }
}

static bool ta_is_bigint_kind(AjsTypedArrayKind kind) {
    return kind == AJS_TA_BIGINT64 || kind == AJS_TA_BIGUINT64;
}

static bool host_is_little_endian(void) {
    uint16_t one = 1u;
    unsigned char b;
    memcpy(&b, &one, 1);
    return b == 1u;
}

static void read_endian(void *dst, const uint8_t *src, size_t n, bool little_endian) {
    uint8_t *d = (uint8_t *)dst;
    if (host_is_little_endian() == little_endian) {
        memcpy(d, src, n);
    } else {
        for (size_t i = 0; i < n; i++) d[i] = src[n - 1u - i];
    }
}

static void write_endian(uint8_t *dst, const void *src, size_t n, bool little_endian) {
    const uint8_t *s = (const uint8_t *)src;
    if (host_is_little_endian() == little_endian) {
        memcpy(dst, s, n);
    } else {
        for (size_t i = 0; i < n; i++) dst[i] = s[n - 1u - i];
    }
}

/* IEEE-754 binary16 <-> binary64 exact round-to-nearest-even */
static double f16_bits_to_f64(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) & 1u;
    uint32_t exp  = (uint32_t)(h >> 10) & 0x1Fu;
    uint32_t frac = (uint32_t)h & 0x3FFu;
    double val;
    if (exp == 0x1Fu) {
        if (frac != 0) return ajs_bits_to_double(AJS_VAL_NAN);
        return sign ? -INFINITY : INFINITY;
    }
    if (exp == 0) {
        val = ldexp((double)frac, -24);
    } else {
        val = ldexp((double)(frac | 0x400u), (int)exp - 25);
    }
    return sign ? -val : val;
}

static uint16_t f64_to_f16_bits(double d) {
    uint64_t bits = ajs_double_to_bits(d);
    uint16_t sign = (uint16_t)((bits >> 63) << 15);
    if (isnan(d)) return (uint16_t)(sign | 0x7E00u);
    double ad = fabs(d);
    if (isinf(ad)) return (uint16_t)(sign | 0x7C00u);
    if (ad == 0.0) return sign;

    int exp2 = 0;
    double m = frexp(ad, &exp2); /* ad = m * 2^exp2, m in [0.5, 1.0) */
    int e = exp2 - 1;            /* unbiased exponent: ad = (2*m) * 2^e */
    if (e > 15) {
        /* Check boundary 65520 = (2 - 2^-11) * 2^15 */
        return (uint16_t)(sign | 0x7C00u);
    }
    if (e < -25) return sign;
    if (e < -14) {
        /* Subnormal binary16: scale by 2^24 */
        double scaled = ldexp(ad, 24);
        double fl = floor(scaled);
        double rem = scaled - fl;
        uint32_t f = (uint32_t)fl;
        if (rem > 0.5 || (rem == 0.5 && (f & 1u) != 0)) f++;
        if (f >= 0x400u) return (uint16_t)(sign | 0x0400u);
        return (uint16_t)(sign | (uint16_t)f);
    }
    /* Normal binary16: mantissa = (2*m - 1) * 1024 */
    double scaled = ldexp(m * 2.0 - 1.0, 10);
    double fl = floor(scaled);
    double rem = scaled - fl;
    uint32_t f = (uint32_t)fl;
    if (rem > 0.5 || (rem == 0.5 && (f & 1u) != 0)) f++;
    uint32_t he = (uint32_t)(e + 15);
    if (f >= 0x400u) {
        f = 0;
        he++;
        if (he >= 31u) return (uint16_t)(sign | 0x7C00u);
    }
    return (uint16_t)(sign | (uint16_t)(he << 10) | (uint16_t)f);
}

static uint8_t clamp_u8_even(double d) {
    if (isnan(d) || d <= 0.0) return 0u;
    if (d >= 255.0) return 255u;
    double fl = floor(d);
    double diff = d - fl;
    if (diff < 0.5) return (uint8_t)fl;
    if (diff > 0.5) return (uint8_t)(fl + 1.0);
    uint8_t base = (uint8_t)fl;
    return (base & 1u) ? (uint8_t)(base + 1u) : base;
}

static AjsBigInt *ta_to_bigint(AjsVm *vm, AjsValue v) {
    size_t rs = ajs_temp_root_save(vm);
    AjsValue prim = ajs_to_primitive(vm, v, 2);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return NULL; }
    ajs_temp_root_push(vm, prim);
    AjsBigInt *out = NULL;
    if (ajs_val_is_gc_kind(prim, AJS_GC_BIGINT)) {
        out = (AjsBigInt *)ajs_val_to_gc(prim);
    } else if (prim == AJS_VAL_FALSE) {
        out = ajs_bigint_new_i64(vm, 0);
    } else if (prim == AJS_VAL_TRUE) {
        out = ajs_bigint_new_i64(vm, 1);
    } else if (ajs_val_is_gc_kind(prim, AJS_GC_STRING)) {
        const AjsString *s = (const AjsString *)ajs_val_to_gc(prim);
        bool ok = false;
        out = ajs_bigint_from_cstr(vm, s->utf8, s->len8, true, &ok);
        if (!ok || !out) {
            if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Cannot convert string to a BigInt");
            out = NULL;
        }
    } else {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot convert value to a BigInt");
    }
    ajs_temp_root_restore(vm, rs);
    return out;
}

/* ============================================================================
 * 2. Internal TypedArray Validation & Element Read/Write Hooks
 * ============================================================================ */

bool ajs_ta_check_valid_len(AjsVm *vm, AjsObject *ta_obj, size_t *out_elem_len, size_t *out_byte_offset) {
    if (out_elem_len) *out_elem_len = 0;
    if (out_byte_offset) *out_byte_offset = 0;
    if (!ta_obj || ta_obj->kind != AJS_OBJ_TYPED_ARRAY) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Incompatible TypedArray receiver");
        return false;
    }
    AjsObject *buf = ta_obj->u.view.buffer;
    if (!buf || buf->u.buf.detached) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot perform operation on a detached ArrayBuffer");
        return false;
    }
    size_t esz = ajs_ta_elem_size(ta_obj->u.view.kind);
    size_t boff = ta_obj->u.view.byte_offset;
    size_t blen = buf->u.buf.byte_len;
    if (ta_obj->u.view.auto_length) {
        if (boff > blen) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "TypedArray view is out of bounds of resized ArrayBuffer");
            return false;
        }
        if (out_elem_len) *out_elem_len = (blen - boff) / esz;
        if (out_byte_offset) *out_byte_offset = boff;
        return true;
    }
    size_t elen = ta_obj->u.view.elem_or_byte_len;
    if (boff > blen || elen > (blen - boff) / esz) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "TypedArray view is out of bounds of resized ArrayBuffer");
        return false;
    }
    if (out_elem_len) *out_elem_len = elen;
    if (out_byte_offset) *out_byte_offset = boff;
    return true;
}

static AjsValue read_typed_raw(AjsVm *vm, AjsTypedArrayKind kind, const uint8_t *ptr, bool le) {
    switch (kind) {
        case AJS_TA_INT8:          return ajs_val_number((double)(int8_t)ptr[0]);
        case AJS_TA_UINT8:
        case AJS_TA_UINT8_CLAMPED: return ajs_val_number((double)ptr[0]);
        case AJS_TA_INT16:  { int16_t v;  read_endian(&v, ptr, 2, le); return ajs_val_number((double)v); }
        case AJS_TA_UINT16: { uint16_t v; read_endian(&v, ptr, 2, le); return ajs_val_number((double)v); }
        case AJS_TA_INT32:  { int32_t v;  read_endian(&v, ptr, 4, le); return ajs_val_number((double)v); }
        case AJS_TA_UINT32: { uint32_t v; read_endian(&v, ptr, 4, le); return ajs_val_number((double)v); }
        case AJS_TA_FLOAT32: { float v;   read_endian(&v, ptr, 4, le); return ajs_val_number((double)v); }
        case AJS_TA_FLOAT64: { double v;  read_endian(&v, ptr, 8, le); return ajs_val_number(v); }
        case AJS_TA_BIGINT64: {
            int64_t v;
            read_endian(&v, ptr, 8, le);
            return ajs_val_from_gc(ajs_bigint_new_i64(vm, v));
        }
        case AJS_TA_BIGUINT64: {
            uint64_t v;
            read_endian(&v, ptr, 8, le);
            return ajs_val_from_gc(ajs_bigint_new_u64(vm, v, v ? 1 : 0));
        }
        default: return AJS_VAL_UNDEFINED;
    }
}

static void write_typed_num_raw(AjsTypedArrayKind kind, uint8_t *ptr, double d, bool le) {
    switch (kind) {
        case AJS_TA_INT8:          ptr[0] = (uint8_t)(int8_t)ajs_double_to_int32(d); break;
        case AJS_TA_UINT8:         ptr[0] = (uint8_t)ajs_double_to_uint32(d); break;
        case AJS_TA_UINT8_CLAMPED: ptr[0] = clamp_u8_even(d); break;
        case AJS_TA_INT16:  { int16_t v  = (int16_t)ajs_double_to_int32(d);  write_endian(ptr, &v, 2, le); break; }
        case AJS_TA_UINT16: { uint16_t v = (uint16_t)ajs_double_to_uint32(d); write_endian(ptr, &v, 2, le); break; }
        case AJS_TA_INT32:  { int32_t v  = ajs_double_to_int32(d);           write_endian(ptr, &v, 4, le); break; }
        case AJS_TA_UINT32: { uint32_t v = ajs_double_to_uint32(d);          write_endian(ptr, &v, 4, le); break; }
        case AJS_TA_FLOAT32: { float v   = ajs_f32_round(d);                 write_endian(ptr, &v, 4, le); break; }
        case AJS_TA_FLOAT64: { double v  = ajs_f64_round(d);                 write_endian(ptr, &v, 8, le); break; }
        default: break;
    }
}

bool ajs_ta_get_index(AjsVm *vm, AjsObject *ta_obj, uint32_t idx, AjsValue *out_val) {
    size_t elen = 0, boff = 0;
    if (!ajs_ta_check_valid_len(vm, ta_obj, &elen, &boff)) {
        vm->has_exception = false;
        *out_val = AJS_VAL_UNDEFINED;
        return true;
    }
    if ((size_t)idx >= elen) {
        *out_val = AJS_VAL_UNDEFINED;
        return true;
    }
    size_t esz = ajs_ta_elem_size(ta_obj->u.view.kind);
    const uint8_t *p = ta_obj->u.view.buffer->u.buf.bytes + boff + (size_t)idx * esz;
    *out_val = read_typed_raw(vm, ta_obj->u.view.kind, p, host_is_little_endian());
    return !vm->has_exception;
}

bool ajs_ta_set_index(AjsVm *vm, AjsObject *ta_obj, uint32_t idx, AjsValue val) {
    if (!ta_obj || ta_obj->kind != AJS_OBJ_TYPED_ARRAY) return false;
    AjsTypedArrayKind kind = ta_obj->u.view.kind;
    double num = 0.0;
    AjsBigInt *bi = NULL;
    if (ta_is_bigint_kind(kind)) {
        bi = ta_to_bigint(vm, val);
        if (vm->has_exception || !bi) return false;
    } else {
        if (!ajs_to_number(vm, val, &num)) return false;
    }
    /* Re-validate after coercion (user valueOf() may have detached/resized!) */
    size_t elen = 0, boff = 0;
    if (!ajs_ta_check_valid_len(vm, ta_obj, &elen, &boff)) {
        vm->has_exception = false;
        return true;
    }
    if ((size_t)idx >= elen) return true;

    size_t esz = ajs_ta_elem_size(kind);
    uint8_t *p = ta_obj->u.view.buffer->u.buf.bytes + boff + (size_t)idx * esz;
    bool le = host_is_little_endian();
    if (kind == AJS_TA_BIGINT64) {
        int64_t iv = ajs_bigint_to_i64_wrap(bi);
        write_endian(p, &iv, 8, le);
    } else if (kind == AJS_TA_BIGUINT64) {
        uint64_t uv = ajs_bigint_to_u64_wrap(bi);
        write_endian(p, &uv, 8, le);
    } else {
        write_typed_num_raw(kind, p, num, le);
    }
    return true;
}

/* ============================================================================
 * 3. Public C APIs for ArrayBuffer & TypedArray
 * ============================================================================ */

static AjsObject *alloc_array_buffer_internal(AjsVm *vm, AjsRealm *r, AjsValue proto,
                                              size_t byte_len, size_t max_byte_len,
                                              bool resizable, bool shared) {
    size_t limit = vm->cfg.heap_limit_bytes ? vm->cfg.heap_limit_bytes : (32u * 1024u * 1024u);
    if (byte_len > limit || (resizable && max_byte_len > limit)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "ArrayBuffer allocation exceeds maximum size");
        return NULL;
    }
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, proto);
    AjsObject *ab = ajs_obj_new(vm, r, shared ? AJS_OBJ_SHARED_ARRAY_BUFFER : AJS_OBJ_ARRAY_BUFFER, proto);
    ajs_temp_root_restore(vm, rs);
    if (!ab) return NULL;

    uint8_t *data = NULL;
    if (byte_len > 0) {
        data = (uint8_t *)calloc(1, byte_len);
        if (!data) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Out of memory allocating ArrayBuffer");
            return NULL;
        }
    }
    ab->u.buf.bytes = data;
    ab->u.buf.byte_len = byte_len;
    ab->u.buf.max_byte_len = resizable ? max_byte_len : byte_len;
    ab->u.buf.resizable = resizable;
    ab->u.buf.detached = false;
    ab->u.buf.shared = shared;
    return ab;
}

AjsValue ajs_new_array_buffer_copy(AjsVm *vm, const uint8_t *bytes, size_t byte_len) {
    if (!vm) return AJS_VAL_UNDEFINED;
    AjsRealm *r = vm->active_realm ? vm->active_realm : vm->main_realm;
    AjsObject *ab = alloc_array_buffer_internal(vm, r, r->ab_proto, byte_len, byte_len, false, false);
    if (!ab) return AJS_VAL_EXCEPTION;
    if (bytes && byte_len > 0) ajs_memcpy(ab->u.buf.bytes, bytes, byte_len);
    AjsValue v = ajs_val_from_gc(ab);
    ajs_handle_record(vm, v);
    return v;
}

bool ajs_detach_array_buffer(AjsVm *vm, AjsValue ab_val) {
    if (!ajs_val_is_gc_kind(ab_val, AJS_GC_OBJECT)) {
        if (vm) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Argument is not an ArrayBuffer");
        return false;
    }
    AjsObject *ab = (AjsObject *)ajs_val_to_gc(ab_val);
    if (ab->kind != AJS_OBJ_ARRAY_BUFFER) {
        if (vm) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot detach non-ArrayBuffer");
        return false;
    }
    free(ab->u.buf.bytes);
    ab->u.buf.bytes = NULL;
    ab->u.buf.byte_len = 0;
    ab->u.buf.max_byte_len = 0;
    ab->u.buf.detached = true;
    return true;
}

uint8_t *ajs_get_array_buffer_data(AjsVm *vm, AjsValue ab_val, size_t *out_byte_len) {
    (void)vm;
    if (out_byte_len) *out_byte_len = 0;
    if (!ajs_val_is_gc_kind(ab_val, AJS_GC_OBJECT)) return NULL;
    AjsObject *ab = (AjsObject *)ajs_val_to_gc(ab_val);
    if (ab->kind != AJS_OBJ_ARRAY_BUFFER && ab->kind != AJS_OBJ_SHARED_ARRAY_BUFFER) return NULL;
    if (ab->u.buf.detached) return NULL;
    if (out_byte_len) *out_byte_len = ab->u.buf.byte_len;
    return ab->u.buf.bytes;
}

AjsValue ajs_new_typed_array(AjsVm *vm, AjsTypedArrayKind kind, const uint8_t *bytes, size_t elem_count) {
    if (!vm || kind < AJS_TA_INT8 || kind > AJS_TA_BIGUINT64) return AJS_VAL_UNDEFINED;
    AjsRealm *r = vm->active_realm ? vm->active_realm : vm->main_realm;
    size_t esz = ajs_ta_elem_size(kind);
    if (elem_count > SIZE_MAX / esz) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "TypedArray length too large");
    }
    size_t blen = elem_count * esz;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *ab = alloc_array_buffer_internal(vm, r, r->ab_proto, blen, blen, false, false);
    if (!ab) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (bytes && blen > 0) ajs_memcpy(ab->u.buf.bytes, bytes, blen);
    ajs_temp_root_push(vm, ajs_val_from_gc(ab));

    AjsObject *ta = ajs_obj_new(vm, r, AJS_OBJ_TYPED_ARRAY, r->ta_sub_proto[(int)kind]);
    ajs_temp_root_restore(vm, rs);
    if (!ta) return AJS_VAL_EXCEPTION;
    ta->u.view.kind = kind;
    ta->u.view.buffer = ab;
    ta->u.view.byte_offset = 0;
    ta->u.view.elem_or_byte_len = elem_count;
    ta->u.view.auto_length = false;

    AjsValue v = ajs_val_from_gc(ta);
    ajs_handle_record(vm, v);
    return v;
}

uint8_t *ajs_get_buffer_source_bytes(AjsVm *vm, AjsValue ab_or_view,
                                     size_t *out_byte_len, AjsTypedArrayKind *out_kind) {
    if (out_byte_len) *out_byte_len = 0;
    if (out_kind) *out_kind = AJS_TA_NONE;
    if (!ajs_val_is_gc_kind(ab_or_view, AJS_GC_OBJECT)) return NULL;
    AjsObject *o = (AjsObject *)ajs_val_to_gc(ab_or_view);
    if (o->kind == AJS_OBJ_ARRAY_BUFFER || o->kind == AJS_OBJ_SHARED_ARRAY_BUFFER) {
        if (o->u.buf.detached) return NULL;
        if (out_byte_len) *out_byte_len = o->u.buf.byte_len;
        return o->u.buf.bytes;
    }
    if (o->kind == AJS_OBJ_TYPED_ARRAY) {
        size_t elen = 0, boff = 0;
        if (!ajs_ta_check_valid_len(vm, o, &elen, &boff)) {
            if (vm) vm->has_exception = false;
            return NULL;
        }
        if (out_kind) *out_kind = o->u.view.kind;
        if (out_byte_len) *out_byte_len = elen * ajs_ta_elem_size(o->u.view.kind);
        return o->u.view.buffer->u.buf.bytes ? (o->u.view.buffer->u.buf.bytes + boff) : NULL;
    }
    if (o->kind == AJS_OBJ_DATA_VIEW) {
        AjsObject *b = o->u.view.buffer;
        if (!b || b->u.buf.detached || o->u.view.byte_offset > b->u.buf.byte_len) return NULL;
        size_t blen = o->u.view.auto_length ? (b->u.buf.byte_len - o->u.view.byte_offset) : o->u.view.elem_or_byte_len;
        if (o->u.view.byte_offset + blen > b->u.buf.byte_len) return NULL;
        if (out_byte_len) *out_byte_len = blen;
        return b->u.buf.bytes ? (b->u.buf.bytes + o->u.view.byte_offset) : NULL;
    }
    return NULL;
}

/* ============================================================================
 * 4. ArrayBuffer & SharedArrayBuffer Built-ins
 * ============================================================================ */

static AjsValue ta_make_fn(AjsVm *vm, AjsRealm *r, AjsNativeFn fn, const char *name,
                           int length, bool is_ctor, int32_t magic, AjsValue proto) {
    AjsObject *f = ajs_obj_new(vm, r, AJS_OBJ_FUNCTION, proto);
    if (!f) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue fv = ajs_val_from_gc(f);
    ajs_temp_root_push(vm, fv);
    f->u.fn.fn_kind = AJS_FN_NATIVE;
    f->u.fn.is_constructor = is_ctor;
    f->u.fn.strict = true;
    f->u.fn.native_fn = fn;
    f->u.fn.native_magic = magic;

    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
              AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    d.value = ajs_val_number((double)(length < 0 ? 0 : length));
    ajs_obj_define_own_prop(vm, f, ajs_val_from_gc(vm->str_length), &d, false);
    d.value = ajs_val_from_gc(ajs_str_from_cstr(vm, name ? name : "", true));
    ajs_obj_define_own_prop(vm, f, ajs_val_from_gc(vm->str_name), &d, false);
    ajs_temp_root_restore(vm, rs);
    return fv;
}

static int32_t ta_cur_magic(const AjsVm *vm) {
    if (vm->call_depth == 0) return 0;
    AjsObject *fn = vm->call_stack[vm->call_depth - 1u].native_fn_obj;
    return fn ? fn->u.fn.native_magic : 0;
}

static AjsValue ta_proto_from_nt(AjsVm *vm, AjsValue nt, AjsValue fallback) {
    if (!ajs_val_is_gc_kind(nt, AJS_GC_OBJECT)) return fallback;
    AjsValue p = ajs_obj_get(vm, nt, ajs_val_from_gc(vm->str_prototype), nt);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    return ajs_val_is_gc_kind(p, AJS_GC_OBJECT) ? p : fallback;
}

static AjsObject *require_ab(AjsVm *vm, AjsValue v, bool shared, const char *name) {
    AjsObjKind want = shared ? AJS_OBJ_SHARED_ARRAY_BUFFER : AJS_OBJ_ARRAY_BUFFER;
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT) || ((AjsObject *)ajs_val_to_gc(v))->kind != want) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "%s called on incompatible receiver", name);
        return NULL;
    }
    return (AjsObject *)ajs_val_to_gc(v);
}

static AjsValue js_ab_sab_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    bool shared = (ta_cur_magic(vm) == -38);
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt == AJS_VAL_UNDEFINED) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Constructor %s requires 'new'",
                                  shared ? "SharedArrayBuffer" : "ArrayBuffer");
    }
    size_t blen = 0;
    if (argc > 0 && !ajs_is_undefined(argv[0])) {
        if (!ajs_to_index(vm, argv[0], &blen)) return AJS_VAL_EXCEPTION;
    }
    bool resizable = false;
    size_t max_blen = blen;
    if (argc > 1 && ajs_is_object(vm, argv[1])) {
        AjsValue mopt = ajs_obj_get(vm, argv[1], ajs_val_from_gc(ajs_str_from_cstr(vm, "maxByteLength", true)), argv[1]);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        if (!ajs_is_undefined(mopt)) {
            if (!ajs_to_index(vm, mopt, &max_blen)) return AJS_VAL_EXCEPTION;
            if (blen > max_blen) {
                return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "byteLength exceeds maxByteLength");
            }
            resizable = true;
        }
    }
    AjsValue proto = ta_proto_from_nt(vm, nt, shared ? vm->active_realm->sab_proto : vm->active_realm->ab_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    AjsObject *ab = alloc_array_buffer_internal(vm, vm->active_realm, proto, blen, max_blen, resizable, shared);
    return ab ? ajs_val_from_gc(ab) : AJS_VAL_EXCEPTION;
}

static AjsValue js_ab_isView(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)vm; (void)this_val; (void)ud;
    AjsValue v = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return AJS_VAL_FALSE;
    AjsObjKind k = ((const AjsObject *)ajs_val_to_gc(v))->kind;
    return ajs_bool(k == AJS_OBJ_TYPED_ARRAY || k == AJS_OBJ_DATA_VIEW);
}

static AjsValue js_ab_byteLength(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    bool shared = (ta_cur_magic(vm) != 0);
    AjsObject *ab = require_ab(vm, this_val, shared, "byteLength");
    if (!ab) return AJS_VAL_EXCEPTION;
    return ajs_val_number((double)(ab->u.buf.detached ? 0u : ab->u.buf.byte_len));
}
static AjsValue js_ab_maxByteLength(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    bool shared = (ta_cur_magic(vm) != 0);
    AjsObject *ab = require_ab(vm, this_val, shared, "maxByteLength");
    if (!ab) return AJS_VAL_EXCEPTION;
    return ajs_val_number((double)(ab->u.buf.detached ? 0u : ab->u.buf.max_byte_len));
}
static AjsValue js_ab_resizable(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    bool shared = (ta_cur_magic(vm) != 0);
    AjsObject *ab = require_ab(vm, this_val, shared, shared ? "growable" : "resizable");
    return ab ? ajs_bool(ab->u.buf.resizable) : AJS_VAL_EXCEPTION;
}
static AjsValue js_ab_detached(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    AjsObject *ab = require_ab(vm, this_val, false, "detached");
    return ab ? ajs_bool(ab->u.buf.detached) : AJS_VAL_EXCEPTION;
}

static AjsValue js_ab_resize_or_grow(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    bool shared = (ta_cur_magic(vm) != 0);
    AjsObject *ab = require_ab(vm, this_val, shared, shared ? "SharedArrayBuffer.prototype.grow" : "ArrayBuffer.prototype.resize");
    if (!ab) return AJS_VAL_EXCEPTION;
    if (!ab->u.buf.resizable) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Buffer is not resizable");
    }
    size_t new_len = 0;
    if (!ajs_to_index(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &new_len)) return AJS_VAL_EXCEPTION;
    if (ab->u.buf.detached) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "ArrayBuffer is detached");
    }
    if (new_len > ab->u.buf.max_byte_len || (shared && new_len < ab->u.buf.byte_len)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid new byteLength");
    }
    if (new_len == ab->u.buf.byte_len) return AJS_VAL_UNDEFINED;
    if (new_len == 0) {
        free(ab->u.buf.bytes);
        ab->u.buf.bytes = NULL;
        ab->u.buf.byte_len = 0;
        return AJS_VAL_UNDEFINED;
    }
    uint8_t *nb = (uint8_t *)realloc(ab->u.buf.bytes, new_len);
    if (!nb) return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Out of memory resizing buffer");
    if (new_len > ab->u.buf.byte_len) {
        memset(nb + ab->u.buf.byte_len, 0, new_len - ab->u.buf.byte_len);
    }
    ab->u.buf.bytes = nb;
    ab->u.buf.byte_len = new_len;
    return AJS_VAL_UNDEFINED;
}

static AjsValue js_ab_transfer(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    bool preserve_resizability = (ta_cur_magic(vm) == 0);
    AjsObject *ab = require_ab(vm, this_val, false, "ArrayBuffer.prototype.transfer");
    if (!ab) return AJS_VAL_EXCEPTION;
    size_t new_len = ab->u.buf.byte_len;
    if (argc > 0 && !ajs_is_undefined(argv[0])) {
        if (!ajs_to_index(vm, argv[0], &new_len)) return AJS_VAL_EXCEPTION;
    }
    if (ab->u.buf.detached) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot transfer a detached ArrayBuffer");
    }
    bool new_res = preserve_resizability && ab->u.buf.resizable;
    size_t new_max = new_res ? ab->u.buf.max_byte_len : new_len;
    if (new_res && new_len > new_max) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "newLength exceeds maxByteLength");
    }
    AjsObject *nab = alloc_array_buffer_internal(vm, vm->active_realm, vm->active_realm->ab_proto,
                                                 new_len, new_max, new_res, false);
    if (!nab) return AJS_VAL_EXCEPTION;
    size_t copy_len = new_len < ab->u.buf.byte_len ? new_len : ab->u.buf.byte_len;
    ajs_memcpy(nab->u.buf.bytes, ab->u.buf.bytes, copy_len);
    ajs_detach_array_buffer(vm, this_val);
    return ajs_val_from_gc(nab);
}

static AjsValue js_ab_slice(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    bool shared = (ta_cur_magic(vm) != 0);
    AjsObject *ab = require_ab(vm, this_val, shared, "slice");
    if (!ab) return AJS_VAL_EXCEPTION;
    if (ab->u.buf.detached) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "ArrayBuffer is detached");

    double len = (double)ab->u.buf.byte_len;
    double rs_in = 0.0;
    if (argc > 0 && !ajs_is_undefined(argv[0])) {
        if (!ajs_to_number(vm, argv[0], &rs_in)) return AJS_VAL_EXCEPTION;
        rs_in = ajs_double_to_integer_or_inf(rs_in);
    }
    double first = rs_in < 0.0 ? fmax(len + rs_in, 0.0) : fmin(rs_in, len);

    double re_in = len;
    if (argc > 1 && !ajs_is_undefined(argv[1])) {
        if (!ajs_to_number(vm, argv[1], &re_in)) return AJS_VAL_EXCEPTION;
        re_in = ajs_double_to_integer_or_inf(re_in);
    }
    double fin = re_in < 0.0 ? fmax(len + re_in, 0.0) : fmin(re_in, len);
    size_t new_len = fin > first ? (size_t)(fin - first) : 0u;

    /* SpeciesConstructor(O, defaultCtor) */
    AjsValue def_ctor = shared ? vm->active_realm->sab_ctor : vm->active_realm->ab_ctor;
    AjsValue ctor = def_ctor;
    AjsValue cprop = ajs_obj_get(vm, this_val, ajs_val_from_gc(vm->str_constructor), this_val);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (!ajs_is_undefined(cprop)) {
        if (!ajs_is_object(vm, cprop)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Invalid constructor");
        AjsValue sp = ajs_obj_get(vm, cprop, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_SPECIES]), cprop);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        if (!ajs_is_nullish(sp)) ctor = sp;
    }
    size_t tr = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ctor);
    AjsValue arg_len = ajs_val_number((double)new_len);
    AjsValue new_val = ajs_construct_internal(vm, ctor, 1, &arg_len, ctor);
    ajs_temp_root_restore(vm, tr);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;

    AjsObject *nab = require_ab(vm, new_val, shared, "slice species result");
    if (!nab) return AJS_VAL_EXCEPTION;
    if (nab == ab) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Same ArrayBuffer returned from species");
    if (nab->u.buf.detached || nab->u.buf.byte_len < new_len) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Species buffer is detached or too small");
    }
    if (ab->u.buf.detached || (size_t)first + new_len > ab->u.buf.byte_len) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Source ArrayBuffer detached or shrunk during slice");
    }
    ajs_memcpy(nab->u.buf.bytes, ab->u.buf.bytes + (size_t)first, new_len);
    return new_val;
}

/* ============================================================================
 * 5. DataView (including ES2025/2026 Float16)
 * ============================================================================ */

static AjsObject *require_dv(AjsVm *vm, AjsValue v, size_t *out_boff, size_t *out_blen) {
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT) || ((AjsObject *)ajs_val_to_gc(v))->kind != AJS_OBJ_DATA_VIEW) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Method called on incompatible DataView receiver");
        return NULL;
    }
    AjsObject *dv = (AjsObject *)ajs_val_to_gc(v);
    AjsObject *b = dv->u.view.buffer;
    if (!b || b->u.buf.detached) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "DataView buffer is detached");
        return NULL;
    }
    size_t boff = dv->u.view.byte_offset;
    size_t blen = b->u.buf.byte_len;
    if (dv->u.view.auto_length) {
        if (boff > blen) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "DataView is out of bounds of resized ArrayBuffer");
            return NULL;
        }
        if (out_boff) *out_boff = boff;
        if (out_blen) *out_blen = blen - boff;
        return dv;
    }
    if (boff > blen || dv->u.view.elem_or_byte_len > blen - boff) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "DataView is out of bounds of resized ArrayBuffer");
        return NULL;
    }
    if (out_boff) *out_boff = boff;
    if (out_blen) *out_blen = dv->u.view.elem_or_byte_len;
    return dv;
}

static AjsValue js_dv_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt == AJS_VAL_UNDEFINED) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Constructor DataView requires 'new'");
    AjsValue bval = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(bval, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "First argument to DataView must be an ArrayBuffer");
    }
    AjsObject *buf = (AjsObject *)ajs_val_to_gc(bval);
    if (buf->kind != AJS_OBJ_ARRAY_BUFFER && buf->kind != AJS_OBJ_SHARED_ARRAY_BUFFER) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "First argument to DataView must be an ArrayBuffer");
    }
    size_t boff = 0;
    if (argc > 1 && !ajs_is_undefined(argv[1])) {
        if (!ajs_to_index(vm, argv[1], &boff)) return AJS_VAL_EXCEPTION;
    }
    if (buf->u.buf.detached) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "ArrayBuffer is detached");
    if (boff > buf->u.buf.byte_len) return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Offset is outside the bounds of the DataView");

    bool auto_len = false;
    size_t vlen = 0;
    if (argc <= 2 || ajs_is_undefined(argv[2])) {
        if (buf->u.buf.resizable) auto_len = true;
        else vlen = buf->u.buf.byte_len - boff;
    } else {
        if (!ajs_to_index(vm, argv[2], &vlen)) return AJS_VAL_EXCEPTION;
    }
    AjsValue proto = ta_proto_from_nt(vm, nt, vm->active_realm->dv_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (buf->u.buf.detached) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "ArrayBuffer is detached");
    if (boff > buf->u.buf.byte_len || (!auto_len && vlen > buf->u.buf.byte_len - boff)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid DataView length");
    }

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, proto);
    AjsObject *dv = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_DATA_VIEW, proto);
    ajs_temp_root_restore(vm, rs);
    if (!dv) return AJS_VAL_EXCEPTION;
    dv->u.view.kind = AJS_TA_NONE;
    dv->u.view.buffer = buf;
    dv->u.view.byte_offset = boff;
    dv->u.view.elem_or_byte_len = vlen;
    dv->u.view.auto_length = auto_len;
    return ajs_val_from_gc(dv);
}

static AjsValue js_dv_prop(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    int32_t m = ta_cur_magic(vm);
    if (m == 0) {
        if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT) || ((AjsObject *)ajs_val_to_gc(this_val))->kind != AJS_OBJ_DATA_VIEW) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Incompatible DataView receiver");
        }
        return ajs_val_from_gc(((AjsObject *)ajs_val_to_gc(this_val))->u.view.buffer);
    }
    size_t boff = 0, blen = 0;
    if (!require_dv(vm, this_val, &boff, &blen)) return AJS_VAL_EXCEPTION;
    return ajs_val_number((double)(m == 1 ? boff : blen));
}

/* DataView get/set magic: 1..11 = AjsTypedArrayKind, 12 = Float16 */
static size_t dv_magic_size(int32_t m) {
    return (m == 12) ? 2u : ajs_ta_elem_size((AjsTypedArrayKind)m);
}

static AjsValue js_dv_get(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    int32_t m = ta_cur_magic(vm);
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT) || ((AjsObject *)ajs_val_to_gc(this_val))->kind != AJS_OBJ_DATA_VIEW) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Incompatible DataView receiver");
    }
    size_t get_idx = 0;
    if (!ajs_to_index(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &get_idx)) return AJS_VAL_EXCEPTION;
    bool le = (argc > 1) ? ajs_to_bool(vm, argv[1]) : false;

    size_t boff = 0, vlen = 0;
    AjsObject *dv = require_dv(vm, this_val, &boff, &vlen);
    if (!dv) return AJS_VAL_EXCEPTION;
    size_t esz = dv_magic_size(m);
    if (get_idx > vlen || esz > vlen - get_idx) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Offset is outside the bounds of the DataView");
    }
    const uint8_t *p = dv->u.view.buffer->u.buf.bytes + boff + get_idx;
    if (m == 12) {
        uint16_t h;
        read_endian(&h, p, 2, le);
        return ajs_val_number(f16_bits_to_f64(h));
    }
    return read_typed_raw(vm, (AjsTypedArrayKind)m, p, le);
}

static AjsValue js_dv_set(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    int32_t m = ta_cur_magic(vm);
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT) || ((AjsObject *)ajs_val_to_gc(this_val))->kind != AJS_OBJ_DATA_VIEW) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Incompatible DataView receiver");
    }
    size_t get_idx = 0;
    if (!ajs_to_index(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &get_idx)) return AJS_VAL_EXCEPTION;

    double num = 0.0;
    AjsBigInt *bi = NULL;
    AjsValue varg = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    if (m == AJS_TA_BIGINT64 || m == AJS_TA_BIGUINT64) {
        bi = ta_to_bigint(vm, varg);
        if (vm->has_exception || !bi) return AJS_VAL_EXCEPTION;
    } else {
        if (!ajs_to_number(vm, varg, &num)) return AJS_VAL_EXCEPTION;
    }
    bool le = (argc > 2) ? ajs_to_bool(vm, argv[2]) : false;

    size_t boff = 0, vlen = 0;
    AjsObject *dv = require_dv(vm, this_val, &boff, &vlen);
    if (!dv) return AJS_VAL_EXCEPTION;
    size_t esz = dv_magic_size(m);
    if (get_idx > vlen || esz > vlen - get_idx) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Offset is outside the bounds of the DataView");
    }
    uint8_t *p = dv->u.view.buffer->u.buf.bytes + boff + get_idx;
    if (m == 12) {
        uint16_t h = f64_to_f16_bits(num);
        write_endian(p, &h, 2, le);
    } else if (m == AJS_TA_BIGINT64) {
        int64_t iv = ajs_bigint_to_i64_wrap(bi);
        write_endian(p, &iv, 8, le);
    } else if (m == AJS_TA_BIGUINT64) {
        uint64_t uv = ajs_bigint_to_u64_wrap(bi);
        write_endian(p, &uv, 8, le);
    } else {
        write_typed_num_raw((AjsTypedArrayKind)m, p, num, le);
    }
    return AJS_VAL_UNDEFINED;
}
/* ============================================================================
 * 6. %TypedArray% & Concrete TypedArray Views (11 Types)
 * ============================================================================ */

static AjsValue ta_species_create(AjsVm *vm, AjsObject *exemplar, int argc, const AjsValue *argv) {
    AjsRealm *r = vm->active_realm;
    AjsValue def_ctor = r->ta_sub_ctor[(int)exemplar->u.view.kind];
    AjsValue ctor = def_ctor;
    AjsValue this_val = ajs_val_from_gc(exemplar);
    AjsValue cprop = ajs_obj_get(vm, this_val, ajs_val_from_gc(vm->str_constructor), this_val);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (!ajs_is_undefined(cprop)) {
        if (!ajs_is_object(vm, cprop)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Invalid constructor");
        AjsValue sp = ajs_obj_get(vm, cprop, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_SPECIES]), cprop);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        if (!ajs_is_nullish(sp)) ctor = sp;
    }
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ctor);
    AjsValue res = ajs_construct_internal(vm, ctor, argc, argv, ctor);
    ajs_temp_root_restore(vm, rs);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (!ajs_val_is_gc_kind(res, AJS_GC_OBJECT) || ((AjsObject *)ajs_val_to_gc(res))->kind != AJS_OBJ_TYPED_ARRAY) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Species constructor did not return a TypedArray");
    }
    size_t nlen = 0;
    if (!ajs_ta_check_valid_len(vm, (AjsObject *)ajs_val_to_gc(res), &nlen, NULL)) return AJS_VAL_EXCEPTION;
    if (argc == 1 && ajs_val_is_number(argv[0])) {
        double want = ajs_val_to_number_unchecked(argv[0]);
        if ((double)nlen < want) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Derived TypedArray is smaller than requested length");
        }
    }
    return res;
}

static AjsValue js_ta_abstract_ctor(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)t; (void)c; (void)v; (void)u;
    return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Abstract class TypedArray not directly constructable");
}

/* Concrete TypedArray constructor: magic = -(40 + AjsTypedArrayKind) */
static AjsValue js_ta_sub_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt == AJS_VAL_UNDEFINED) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "TypedArray constructor requires 'new'");
    }
    AjsTypedArrayKind kind = (AjsTypedArrayKind)(-ta_cur_magic(vm) - 40);
    size_t esz = ajs_ta_elem_size(kind);
    AjsRealm *r = vm->active_realm;

    AjsValue proto = ta_proto_from_nt(vm, nt, r->ta_sub_proto[(int)kind]);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, proto);

    /* Case 1: new TypedArray() or new TypedArray(nonObjectLength) */
    if (argc == 0 || !ajs_is_object(vm, argv[0])) {
        size_t elen = 0;
        if (argc > 0 && !ajs_is_undefined(argv[0])) {
            if (!ajs_to_index(vm, argv[0], &elen)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
        if (elen > SIZE_MAX / esz) {
            ajs_temp_root_restore(vm, rs);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "TypedArray length too large");
        }
        size_t blen = elen * esz;
        AjsObject *ab = alloc_array_buffer_internal(vm, r, r->ab_proto, blen, blen, false, false);
        if (!ab) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(ab));
        AjsObject *ta = ajs_obj_new(vm, r, AJS_OBJ_TYPED_ARRAY, proto);
        ajs_temp_root_restore(vm, rs);
        if (!ta) return AJS_VAL_EXCEPTION;
        ta->u.view.kind = kind;
        ta->u.view.buffer = ab;
        ta->u.view.byte_offset = 0;
        ta->u.view.elem_or_byte_len = elen;
        return ajs_val_from_gc(ta);
    }

    AjsObject *arg0 = (AjsObject *)ajs_val_to_gc(argv[0]);

    /* Case 2: new TypedArray(arrayBuffer [, byteOffset [, length]]) */
    if (arg0->kind == AJS_OBJ_ARRAY_BUFFER || arg0->kind == AJS_OBJ_SHARED_ARRAY_BUFFER) {
        size_t boff = 0;
        if (argc > 1 && !ajs_is_undefined(argv[1])) {
            if (!ajs_to_index(vm, argv[1], &boff)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
        if (boff % esz != 0) {
            ajs_temp_root_restore(vm, rs);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "start offset of %s should be a multiple of %zu",
                                      ta_kind_name(kind), esz);
        }
        bool auto_len = false;
        size_t elen = 0;
        if (argc > 2 && !ajs_is_undefined(argv[2])) {
            if (!ajs_to_index(vm, argv[2], &elen)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        } else if (arg0->u.buf.resizable) {
            auto_len = true;
        }
        if (arg0->u.buf.detached) {
            ajs_temp_root_restore(vm, rs);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "ArrayBuffer is detached");
        }
        size_t blen = arg0->u.buf.byte_len;
        if (auto_len) {
            if (boff > blen) {
                ajs_temp_root_restore(vm, rs);
                return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "byteOffset out of bounds");
            }
        } else if (argc <= 2 || ajs_is_undefined(argv[2])) {
            if (boff > blen || (blen - boff) % esz != 0) {
                ajs_temp_root_restore(vm, rs);
                return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "byte length of %s should be a multiple of %zu",
                                          ta_kind_name(kind), esz);
            }
            elen = (blen - boff) / esz;
        } else {
            if (boff > blen || elen > (blen - boff) / esz) {
                ajs_temp_root_restore(vm, rs);
                return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid typed array length");
            }
        }
        AjsObject *ta = ajs_obj_new(vm, r, AJS_OBJ_TYPED_ARRAY, proto);
        ajs_temp_root_restore(vm, rs);
        if (!ta) return AJS_VAL_EXCEPTION;
        ta->u.view.kind = kind;
        ta->u.view.buffer = arg0;
        ta->u.view.byte_offset = boff;
        ta->u.view.elem_or_byte_len = elen;
        ta->u.view.auto_length = auto_len;
        return ajs_val_from_gc(ta);
    }

    /* Case 3: new TypedArray(typedArray) */
    if (arg0->kind == AJS_OBJ_TYPED_ARRAY) {
        size_t src_len = 0, src_off = 0;
        if (!ajs_ta_check_valid_len(vm, arg0, &src_len, &src_off)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
        if (ta_is_bigint_kind(kind) != ta_is_bigint_kind(arg0->u.view.kind)) {
            ajs_temp_root_restore(vm, rs);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot mix BigInt and Number TypedArrays");
        }
        size_t blen = src_len * esz;
        AjsObject *ab = alloc_array_buffer_internal(vm, r, r->ab_proto, blen, blen, false, false);
        if (!ab) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(ab));
        AjsObject *ta = ajs_obj_new(vm, r, AJS_OBJ_TYPED_ARRAY, proto);
        if (!ta) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ta->u.view.kind = kind;
        ta->u.view.buffer = ab;
        ta->u.view.byte_offset = 0;
        ta->u.view.elem_or_byte_len = src_len;
        ajs_temp_root_push(vm, ajs_val_from_gc(ta));

        if (kind == arg0->u.view.kind) {
            ajs_memcpy(ab->u.buf.bytes, arg0->u.view.buffer->u.buf.bytes + src_off, blen);
        } else {
            for (uint32_t i = 0; i < (uint32_t)src_len; i++) {
                AjsValue v = AJS_VAL_UNDEFINED;
                ajs_ta_get_index(vm, arg0, i, &v);
                if (!ajs_ta_set_index(vm, ta, i, v)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            }
        }
        ajs_temp_root_restore(vm, rs);
        return ajs_val_from_gc(ta);
    }

    /* Case 4: new TypedArray(iterableOrArrayLike) */
    AjsValue src = argv[0];
    AjsValue it_fn = ajs_obj_get(vm, src, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), src);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (!ajs_is_nullish(it_fn)) {
        if (!ajs_is_callable(vm, it_fn)) {
            ajs_temp_root_restore(vm, rs);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "[Symbol.iterator] is not callable");
        }
        AjsValue iter = ajs_call_internal(vm, it_fn, src, 0, NULL);
        if (vm->has_exception || !ajs_is_object(vm, iter)) {
            ajs_temp_root_restore(vm, rs);
            if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator is not an object");
            return AJS_VAL_EXCEPTION;
        }
        ajs_temp_root_push(vm, iter);
        AjsValue next_fn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_next), iter);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, next_fn);

        AjsValue tmp_arr = ajs_new_array(vm, 0);
        ajs_temp_root_push(vm, tmp_arr);
        uint32_t cnt = 0;
        for (;;) {
            size_t ls = ajs_temp_root_save(vm);
            AjsValue step = ajs_call_internal(vm, next_fn, iter, 0, NULL);
            if (vm->has_exception || !ajs_is_object(vm, step)) {
                ajs_temp_root_restore(vm, rs);
                if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator result is not an object");
                return AJS_VAL_EXCEPTION;
            }
            ajs_temp_root_push(vm, step);
            AjsValue dn = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_done), step);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            if (ajs_to_bool(vm, dn)) { ajs_temp_root_restore(vm, ls); break; }
            AjsValue vv = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            ajs_set_prop_index(vm, tmp_arr, cnt++, vv);
            ajs_temp_root_restore(vm, ls);
        }

        size_t blen = (size_t)cnt * esz;
        AjsObject *ab = alloc_array_buffer_internal(vm, r, r->ab_proto, blen, blen, false, false);
        if (!ab) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(ab));
        AjsObject *ta = ajs_obj_new(vm, r, AJS_OBJ_TYPED_ARRAY, proto);
        if (!ta) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ta->u.view.kind = kind;
        ta->u.view.buffer = ab;
        ta->u.view.elem_or_byte_len = cnt;
        ajs_temp_root_push(vm, ajs_val_from_gc(ta));
        for (uint32_t i = 0; i < cnt; i++) {
            AjsValue iv = ajs_get_prop_index(vm, tmp_arr, i);
            if (!ajs_ta_set_index(vm, ta, i, iv)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
        ajs_temp_root_restore(vm, rs);
        return ajs_val_from_gc(ta);
    }

    /* Array-like object */
    AjsValue lenv = ajs_obj_get(vm, src, ajs_val_from_gc(vm->str_length), src);
    uint64_t ulen = 0;
    if (vm->has_exception || !ajs_to_length(vm, lenv, &ulen)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    if (ulen > SIZE_MAX / esz) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "TypedArray length too large");
    }
    size_t elen = (size_t)ulen;
    size_t blen = elen * esz;
    AjsObject *ab = alloc_array_buffer_internal(vm, r, r->ab_proto, blen, blen, false, false);
    if (!ab) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(ab));
    AjsObject *ta = ajs_obj_new(vm, r, AJS_OBJ_TYPED_ARRAY, proto);
    if (!ta) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ta->u.view.kind = kind;
    ta->u.view.buffer = ab;
    ta->u.view.elem_or_byte_len = elen;
    ajs_temp_root_push(vm, ajs_val_from_gc(ta));
    for (uint32_t i = 0; i < (uint32_t)elen; i++) {
        size_t ls = ajs_temp_root_save(vm);
        AjsValue kv = ajs_get_prop_index(vm, src, i);
        if (vm->has_exception || !ajs_ta_set_index(vm, ta, i, kv)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
        ajs_temp_root_restore(vm, ls);
    }
    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(ta);
}

static AjsValue js_ta_of(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_is_constructor_val(this_val)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "TypedArray.of requires a constructor receiver");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue lnum = ajs_val_number((double)argc);
    AjsValue nv = ajs_construct_internal(vm, this_val, 1, &lnum, this_val);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, nv);
    if (!ajs_val_is_gc_kind(nv, AJS_GC_OBJECT) || ((AjsObject *)ajs_val_to_gc(nv))->kind != AJS_OBJ_TYPED_ARRAY) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "TypedArray.of constructor did not return a TypedArray");
    }
    AjsObject *ta = (AjsObject *)ajs_val_to_gc(nv);
    size_t elen = 0;
    if (!ajs_ta_check_valid_len(vm, ta, &elen, NULL)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (elen < (size_t)argc) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "TypedArray.of target too small");
    }
    for (int i = 0; i < argc; i++) {
        if (!ajs_ta_set_index(vm, ta, (uint32_t)i, argv[i])) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    ajs_temp_root_restore(vm, rs);
    return nv;
}

static AjsValue js_ta_from(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_is_constructor_val(this_val)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "TypedArray.from requires a constructor receiver");
    }
    AjsValue src = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue mapfn = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    AjsValue this_arg = argc > 2 ? argv[2] : AJS_VAL_UNDEFINED;
    bool mapping = !ajs_is_undefined(mapfn);
    if (mapping && !ajs_is_callable(vm, mapfn)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "TypedArray.from mapfn must be callable");
    }
    if (ajs_is_nullish(src)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "TypedArray.from source cannot be null or undefined");
    }
    size_t rs = ajs_temp_root_save(vm);
    /* Materialize intermediate JS Array via Array.from(src) then construct & map */
    AjsValue arr_from = ajs_get_prop_str(vm, vm->active_realm->array_ctor, "from");
    ajs_temp_root_push(vm, arr_from);
    AjsValue fargs[3] = { src, mapfn, this_arg };
    AjsValue list = ajs_call_internal(vm, arr_from, vm->active_realm->array_ctor, mapping ? 3 : 1, fargs);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, list);

    uint32_t len = ((AjsObject *)ajs_val_to_gc(list))->u.arr.length;
    AjsValue lnum = ajs_val_number((double)len);
    AjsValue nv = ajs_construct_internal(vm, this_val, 1, &lnum, this_val);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, nv);
    if (!ajs_val_is_gc_kind(nv, AJS_GC_OBJECT) || ((AjsObject *)ajs_val_to_gc(nv))->kind != AJS_OBJ_TYPED_ARRAY) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "TypedArray.from constructor did not return a TypedArray");
    }
    AjsObject *ta = (AjsObject *)ajs_val_to_gc(nv);
    size_t elen = 0;
    if (!ajs_ta_check_valid_len(vm, ta, &elen, NULL) || elen < len) {
        ajs_temp_root_restore(vm, rs);
        if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "TypedArray.from target too small");
        return AJS_VAL_EXCEPTION;
    }
    for (uint32_t i = 0; i < len; i++) {
        AjsValue iv = ajs_get_prop_index(vm, list, i);
        if (!ajs_ta_set_index(vm, ta, i, iv)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    }
    ajs_temp_root_restore(vm, rs);
    return nv;
}

/* %TypedArray%.prototype getters: magic 0=buffer, 1=byteLength, 2=byteOffset, 3=length, 4=@@toStringTag */
static AjsValue js_ta_getter(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    int32_t m = ta_cur_magic(vm);
    if (m == 4) {
        if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) return AJS_VAL_UNDEFINED;
        AjsObject *o = (AjsObject *)ajs_val_to_gc(this_val);
        if (o->kind != AJS_OBJ_TYPED_ARRAY) return AJS_VAL_UNDEFINED;
        return ajs_val_from_gc(ajs_str_from_cstr(vm, ta_kind_name(o->u.view.kind), true));
    }
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT) || ((AjsObject *)ajs_val_to_gc(this_val))->kind != AJS_OBJ_TYPED_ARRAY) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Method called on incompatible TypedArray receiver");
    }
    AjsObject *ta = (AjsObject *)ajs_val_to_gc(this_val);
    if (m == 0) return ajs_val_from_gc(ta->u.view.buffer);
    size_t elen = 0, boff = 0;
    if (!ajs_ta_check_valid_len(vm, ta, &elen, &boff)) {
        vm->has_exception = false; /* Detached / out-of-bounds getters return 0 per ECMA-262! */
        return ajs_val_number(0.0);
    }
    if (m == 1) return ajs_val_number((double)(elen * ajs_ta_elem_size(ta->u.view.kind)));
    if (m == 2) return ajs_val_number((double)boff);
    return ajs_val_number((double)elen);
}

static AjsValue js_ta_set(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT) || ((AjsObject *)ajs_val_to_gc(this_val))->kind != AJS_OBJ_TYPED_ARRAY) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "TypedArray.prototype.set incompatible receiver");
    }
    AjsObject *target = (AjsObject *)ajs_val_to_gc(this_val);
    double doff = 0.0;
    if (argc > 1 && !ajs_is_undefined(argv[1])) {
        if (!ajs_to_number(vm, argv[1], &doff)) return AJS_VAL_EXCEPTION;
        doff = ajs_double_to_integer_or_inf(doff);
    }
    if (doff < 0.0) return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Offset is out of bounds");

    size_t tlen = 0, tboff = 0;
    if (!ajs_ta_check_valid_len(vm, target, &tlen, &tboff)) return AJS_VAL_EXCEPTION;
    AjsValue src = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;

    if (ajs_val_is_gc_kind(src, AJS_GC_OBJECT) && ((AjsObject *)ajs_val_to_gc(src))->kind == AJS_OBJ_TYPED_ARRAY) {
        AjsObject *sta = (AjsObject *)ajs_val_to_gc(src);
        size_t slen = 0, sboff = 0;
        if (!ajs_ta_check_valid_len(vm, sta, &slen, &sboff)) return AJS_VAL_EXCEPTION;
        if (ta_is_bigint_kind(target->u.view.kind) != ta_is_bigint_kind(sta->u.view.kind)) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot mix BigInt and Number TypedArrays");
        }
        if (doff > (double)tlen || slen > tlen - (size_t)doff) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Source is too large");
        }
        size_t toff_el = (size_t)doff;
        if (target->u.view.kind == sta->u.view.kind) {
            size_t esz = ajs_ta_elem_size(target->u.view.kind);
            ajs_memmove(target->u.view.buffer->u.buf.bytes + tboff + toff_el * esz,
                        sta->u.view.buffer->u.buf.bytes + sboff, slen * esz);
            return AJS_VAL_UNDEFINED;
        }
        /* Copy via temp buffer if same underlying ArrayBuffer */
        AjsValue *tmp = slen > 0 ? (AjsValue *)malloc(slen * sizeof(AjsValue)) : NULL;
        if (slen > 0 && !tmp) return ajs_throw(vm, vm->oom_error);
        size_t rs = ajs_temp_root_save(vm);
        for (size_t i = 0; i < slen; i++) {
            ajs_ta_get_index(vm, sta, (uint32_t)i, &tmp[i]);
            ajs_temp_root_push(vm, tmp[i]);
        }
        for (size_t i = 0; i < slen; i++) {
            ajs_ta_set_index(vm, target, (uint32_t)(toff_el + i), tmp[i]);
        }
        ajs_temp_root_restore(vm, rs);
        free(tmp);
        return AJS_VAL_EXCEPTION == 0 ? AJS_VAL_UNDEFINED : (vm->has_exception ? AJS_VAL_EXCEPTION : AJS_VAL_UNDEFINED);
    }

    size_t rs = ajs_temp_root_save(vm);
    AjsObject *sobj = ajs_to_object(vm, src);
    if (!sobj) return AJS_VAL_EXCEPTION;
    AjsValue sv = ajs_val_from_gc(sobj);
    ajs_temp_root_push(vm, sv);
    AjsValue lenv = ajs_obj_get(vm, sv, ajs_val_from_gc(vm->str_length), sv);
    uint64_t slen = 0;
    if (vm->has_exception || !ajs_to_length(vm, lenv, &slen)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    if (doff > (double)tlen || slen > (uint64_t)(tlen - (size_t)doff)) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Source is too large");
    }
    size_t toff_el = (size_t)doff;
    for (uint64_t i = 0; i < slen; i++) {
        size_t ls = ajs_temp_root_save(vm);
        AjsValue kv = ajs_get_prop_index(vm, sv, (uint32_t)i);
        if (vm->has_exception || !ajs_ta_set_index(vm, target, (uint32_t)(toff_el + i), kv)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
        ajs_temp_root_restore(vm, ls);
    }
    ajs_temp_root_restore(vm, rs);
    return AJS_VAL_UNDEFINED;
}

static size_t clamp_rel_ta(double rel, size_t len) {
    if (rel < 0.0) {
        double v = (double)len + rel;
        return v > 0.0 ? (size_t)v : 0u;
    }
    return rel < (double)len ? (size_t)rel : len;
}

static AjsValue js_ta_subarray(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT) || ((AjsObject *)ajs_val_to_gc(this_val))->kind != AJS_OBJ_TYPED_ARRAY) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "TypedArray.prototype.subarray incompatible receiver");
    }
    AjsObject *ta = (AjsObject *)ajs_val_to_gc(this_val);
    size_t src_len = 0, src_boff = 0;
    if (!ajs_ta_check_valid_len(vm, ta, &src_len, &src_boff)) {
        vm->has_exception = false; /* subarray allows out-of-bounds as 0 length, unless detached? Wait: spec gets srcLength = MakeTypedArrayWithBufferWitnessRecord: if OOB -> 0! */
        if (ta->u.view.buffer->u.buf.detached) src_len = 0;
    }
    double b_in = 0.0;
    if (argc > 0 && !ajs_is_undefined(argv[0])) {
        if (!ajs_to_number(vm, argv[0], &b_in)) return AJS_VAL_EXCEPTION;
        b_in = ajs_double_to_integer_or_inf(b_in);
    }
    size_t begin_idx = clamp_rel_ta(b_in, src_len);

    bool end_undef = (argc <= 1 || ajs_is_undefined(argv[1]));
    size_t end_idx = src_len;
    if (!end_undef) {
        double e_in = 0.0;
        if (!ajs_to_number(vm, argv[1], &e_in)) return AJS_VAL_EXCEPTION;
        e_in = ajs_double_to_integer_or_inf(e_in);
        end_idx = clamp_rel_ta(e_in, src_len);
    }
    size_t esz = ajs_ta_elem_size(ta->u.view.kind);
    size_t new_boff = ta->u.view.byte_offset + begin_idx * esz;

    if (ta->u.view.auto_length && end_undef) {
        AjsValue cargs[2] = { ajs_val_from_gc(ta->u.view.buffer), ajs_val_number((double)new_boff) };
        return ta_species_create(vm, ta, 2, cargs);
    }
    size_t new_len = end_idx > begin_idx ? (end_idx - begin_idx) : 0u;
    AjsValue cargs[3] = {
        ajs_val_from_gc(ta->u.view.buffer),
        ajs_val_number((double)new_boff),
        ajs_val_number((double)new_len)
    };
    return ta_species_create(vm, ta, 3, cargs);
}

static AjsValue js_ta_slice(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t len = 0;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Not a TypedArray");
    AjsObject *ta = (AjsObject *)ajs_val_to_gc(this_val);
    if (!ajs_ta_check_valid_len(vm, ta, &len, NULL)) return AJS_VAL_EXCEPTION;

    double k_in = 0.0, f_in = (double)len;
    if (argc > 0 && !ajs_is_undefined(argv[0])) {
        if (!ajs_to_number(vm, argv[0], &k_in)) return AJS_VAL_EXCEPTION;
        k_in = ajs_double_to_integer_or_inf(k_in);
    }
    if (argc > 1 && !ajs_is_undefined(argv[1])) {
        if (!ajs_to_number(vm, argv[1], &f_in)) return AJS_VAL_EXCEPTION;
        f_in = ajs_double_to_integer_or_inf(f_in);
    }
    size_t k = clamp_rel_ta(k_in, len);
    size_t fin = clamp_rel_ta(f_in, len);
    size_t count = fin > k ? (fin - k) : 0u;

    AjsValue cnum = ajs_val_number((double)count);
    AjsValue A = ta_species_create(vm, ta, 1, &cnum);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (count == 0) return A;

    size_t cur_len = 0, cur_boff = 0;
    if (!ajs_ta_check_valid_len(vm, ta, &cur_len, &cur_boff)) return AJS_VAL_EXCEPTION;
    AjsObject *dst = (AjsObject *)ajs_val_to_gc(A);
    size_t dst_boff = dst->u.view.byte_offset;
    size_t copy_cnt = fin <= cur_len ? count : (cur_len > k ? cur_len - k : 0u);

    if (ta->u.view.kind == dst->u.view.kind) {
        size_t esz = ajs_ta_elem_size(ta->u.view.kind);
        ajs_memcpy(dst->u.view.buffer->u.buf.bytes + dst_boff,
                   ta->u.view.buffer->u.buf.bytes + cur_boff + k * esz, copy_cnt * esz);
    } else {
        size_t rs = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, A);
        for (size_t i = 0; i < copy_cnt; i++) {
            AjsValue v = AJS_VAL_UNDEFINED;
            ajs_ta_get_index(vm, ta, (uint32_t)(k + i), &v);
            if (!ajs_ta_set_index(vm, dst, (uint32_t)i, v)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
        ajs_temp_root_restore(vm, rs);
    }
    return A;
}

static AjsValue js_ta_copyWithin(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t len = 0, boff = 0;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Not a TypedArray");
    AjsObject *ta = (AjsObject *)ajs_val_to_gc(this_val);
    if (!ajs_ta_check_valid_len(vm, ta, &len, &boff)) return AJS_VAL_EXCEPTION;

    double t_in = 0.0, s_in = 0.0, e_in = (double)len;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &t_in)) return AJS_VAL_EXCEPTION;
    t_in = ajs_double_to_integer_or_inf(t_in);
    if (argc > 1 && !ajs_to_number(vm, argv[1], &s_in)) return AJS_VAL_EXCEPTION;
    s_in = ajs_double_to_integer_or_inf(s_in);
    if (argc > 2 && !ajs_is_undefined(argv[2])) {
        if (!ajs_to_number(vm, argv[2], &e_in)) return AJS_VAL_EXCEPTION;
        e_in = ajs_double_to_integer_or_inf(e_in);
    }
    size_t to = clamp_rel_ta(t_in, len);
    size_t from = clamp_rel_ta(s_in, len);
    size_t fin = clamp_rel_ta(e_in, len);
    size_t count = fin > from ? (fin - from) : 0u;
    if (count > len - to) count = len - to;
    if (count > 0) {
        size_t cur_len = 0;
        if (!ajs_ta_check_valid_len(vm, ta, &cur_len, &boff)) return AJS_VAL_EXCEPTION;
        if (from + count > cur_len) count = cur_len > from ? cur_len - from : 0u;
        if (to + count > cur_len) count = cur_len > to ? cur_len - to : 0u;
        size_t esz = ajs_ta_elem_size(ta->u.view.kind);
        ajs_memmove(ta->u.view.buffer->u.buf.bytes + boff + to * esz,
                    ta->u.view.buffer->u.buf.bytes + boff + from * esz, count * esz);
    }
    return this_val;
}

static AjsValue js_ta_fill(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t len = 0;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Not a TypedArray");
    AjsObject *ta = (AjsObject *)ajs_val_to_gc(this_val);
    if (!ajs_ta_check_valid_len(vm, ta, &len, NULL)) return AJS_VAL_EXCEPTION;

    AjsValue fill_val = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (ta_is_bigint_kind(ta->u.view.kind)) {
        AjsBigInt *bi = ta_to_bigint(vm, fill_val);
        if (!bi) return AJS_VAL_EXCEPTION;
        fill_val = ajs_val_from_gc(bi);
    } else {
        double d = 0.0;
        if (!ajs_to_number(vm, fill_val, &d)) return AJS_VAL_EXCEPTION;
        fill_val = ajs_val_number(d);
    }
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, fill_val);

    double s_in = 0.0, e_in = (double)len;
    if (argc > 1 && !ajs_is_undefined(argv[1])) {
        if (!ajs_to_number(vm, argv[1], &s_in)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        s_in = ajs_double_to_integer_or_inf(s_in);
    }
    if (argc > 2 && !ajs_is_undefined(argv[2])) {
        if (!ajs_to_number(vm, argv[2], &e_in)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        e_in = ajs_double_to_integer_or_inf(e_in);
    }
    if (!ajs_ta_check_valid_len(vm, ta, &len, NULL)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    size_t k = clamp_rel_ta(s_in, len);
    size_t fin = clamp_rel_ta(e_in, len);
    while (k < fin) {
        ajs_ta_set_index(vm, ta, (uint32_t)k++, fill_val);
    }
    ajs_temp_root_restore(vm, rs);
    return this_val;
}

static AjsValue js_ta_reverse(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    bool copy_mode = (ta_cur_magic(vm) != 0);
    size_t len = 0, boff = 0;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Not a TypedArray");
    AjsObject *ta = (AjsObject *)ajs_val_to_gc(this_val);
    if (!ajs_ta_check_valid_len(vm, ta, &len, &boff)) return AJS_VAL_EXCEPTION;

    if (copy_mode) {
        AjsRealm *r = vm->active_realm;
        AjsValue lnum = ajs_val_number((double)len);
        AjsValue out = ajs_construct_internal(vm, r->ta_sub_ctor[(int)ta->u.view.kind], 1, &lnum, r->ta_sub_ctor[(int)ta->u.view.kind]);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        AjsObject *dst = (AjsObject *)ajs_val_to_gc(out);
        for (size_t i = 0; i < len; i++) {
            AjsValue ev = AJS_VAL_UNDEFINED;
            ajs_ta_get_index(vm, ta, (uint32_t)(len - 1u - i), &ev);
            ajs_ta_set_index(vm, dst, (uint32_t)i, ev);
        }
        return out;
    }

    size_t esz = ajs_ta_elem_size(ta->u.view.kind);
    uint8_t *base = ta->u.view.buffer->u.buf.bytes + boff;
    uint8_t tmp[8];
    for (size_t l = 0, r = len; l < r / 2u; l++) {
        size_t ri = len - 1u - l;
        memcpy(tmp, base + l * esz, esz);
        memcpy(base + l * esz, base + ri * esz, esz);
        memcpy(base + ri * esz, tmp, esz);
    }
    return this_val;
}

static int ta_cmp_elem(AjsVm *vm, AjsValue x, AjsValue y, AjsValue cmpfn) {
    if (!ajs_is_undefined(cmpfn)) {
        AjsValue cargs[2] = { x, y };
        AjsValue rv = ajs_call_internal(vm, cmpfn, AJS_VAL_UNDEFINED, 2, cargs);
        if (vm->has_exception) return 0;
        double d = 0.0;
        if (!ajs_to_number(vm, rv, &d) || isnan(d) || d == 0.0) return 0;
        return d < 0.0 ? -1 : 1;
    }
    if (ajs_val_is_gc_kind(x, AJS_GC_BIGINT) && ajs_val_is_gc_kind(y, AJS_GC_BIGINT)) {
        return ajs_bigint_cmp((const AjsBigInt *)ajs_val_to_gc(x), (const AjsBigInt *)ajs_val_to_gc(y));
    }
    double dx = ajs_val_to_number_unchecked(x);
    double dy = ajs_val_to_number_unchecked(y);
    if (isnan(dx) && isnan(dy)) return 0;
    if (isnan(dx)) return 1;
    if (isnan(dy)) return -1;
    if (dx < dy) return -1;
    if (dx > dy) return 1;
    if (dx == 0.0 && dy == 0.0) {
        bool sx = signbit(dx) != 0, sy = signbit(dy) != 0;
        if (sx && !sy) return -1;
        if (!sx && sy) return 1;
    }
    return 0;
}

static AjsValue js_ta_sort(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    bool copy_mode = (ta_cur_magic(vm) != 0);
    AjsValue cmpfn = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_is_undefined(cmpfn) && !ajs_is_callable(vm, cmpfn)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Comparator must be a function or undefined");
    }
    size_t len = 0;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Not a TypedArray");
    AjsObject *ta = (AjsObject *)ajs_val_to_gc(this_val);
    if (!ajs_ta_check_valid_len(vm, ta, &len, NULL)) return AJS_VAL_EXCEPTION;

    size_t rs = ajs_temp_root_save(vm);
    AjsValue *items = len > 0 ? (AjsValue *)malloc(len * sizeof(AjsValue)) : NULL;
    AjsValue *tmp   = len > 0 ? (AjsValue *)malloc(len * sizeof(AjsValue)) : NULL;
    if (len > 0 && (!items || !tmp)) { free(items); free(tmp); return ajs_throw(vm, vm->oom_error); }

    for (size_t i = 0; i < len; i++) {
        ajs_ta_get_index(vm, ta, (uint32_t)i, &items[i]);
        ajs_temp_root_push(vm, items[i]);
    }
    for (size_t width = 1; width < len; width *= 2u) {
        for (size_t i = 0; i < len; i += 2u * width) {
            size_t mid = i + width < len ? i + width : len;
            size_t right = i + 2u * width < len ? i + 2u * width : len;
            size_t l = i, r = mid, k = i;
            while (l < mid && r < right) {
                int c = ta_cmp_elem(vm, items[l], items[r], cmpfn);
                if (vm->has_exception) { free(items); free(tmp); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                if (c <= 0) tmp[k++] = items[l++];
                else tmp[k++] = items[r++];
            }
            while (l < mid) tmp[k++] = items[l++];
            while (r < right) tmp[k++] = items[r++];
            for (size_t m = i; m < right; m++) items[m] = tmp[m];
        }
    }

    AjsValue out = this_val;
    AjsObject *dst = ta;
    if (copy_mode) {
        AjsRealm *r = vm->active_realm;
        AjsValue lnum = ajs_val_number((double)len);
        out = ajs_construct_internal(vm, r->ta_sub_ctor[(int)ta->u.view.kind], 1, &lnum, r->ta_sub_ctor[(int)ta->u.view.kind]);
        if (vm->has_exception) { free(items); free(tmp); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        dst = (AjsObject *)ajs_val_to_gc(out);
    }
    for (size_t i = 0; i < len; i++) {
        ajs_ta_set_index(vm, dst, (uint32_t)i, items[i]);
    }
    free(items); free(tmp);
    ajs_temp_root_restore(vm, rs);
    return out;
}

static AjsValue js_ta_with(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t len = 0;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Not a TypedArray");
    AjsObject *ta = (AjsObject *)ajs_val_to_gc(this_val);
    if (!ajs_ta_check_valid_len(vm, ta, &len, NULL)) return AJS_VAL_EXCEPTION;

    double rel = 0.0;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &rel)) return AJS_VAL_EXCEPTION;
    rel = ajs_double_to_integer_or_inf(rel);
    double actual = rel >= 0.0 ? rel : ((double)len + rel);

    AjsValue val = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    if (ta_is_bigint_kind(ta->u.view.kind)) {
        AjsBigInt *bi = ta_to_bigint(vm, val);
        if (!bi) return AJS_VAL_EXCEPTION;
        val = ajs_val_from_gc(bi);
    } else {
        double d = 0.0;
        if (!ajs_to_number(vm, val, &d)) return AJS_VAL_EXCEPTION;
        val = ajs_val_number(d);
    }
    size_t cur_len = 0;
    bool ok = ajs_ta_check_valid_len(vm, ta, &cur_len, NULL);
    vm->has_exception = false;
    if (!ok || actual < 0.0 || actual >= (double)cur_len) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid TypedArray.prototype.with index");
    }

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, val);
    AjsRealm *r = vm->active_realm;
    AjsValue lnum = ajs_val_number((double)len);
    AjsValue out = ajs_construct_internal(vm, r->ta_sub_ctor[(int)ta->u.view.kind], 1, &lnum, r->ta_sub_ctor[(int)ta->u.view.kind]);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsObject *dst = (AjsObject *)ajs_val_to_gc(out);
    size_t act_idx = (size_t)actual;
    for (size_t i = 0; i < len; i++) {
        if (i == act_idx) {
            ajs_ta_set_index(vm, dst, (uint32_t)i, val);
        } else {
            AjsValue ev = AJS_VAL_UNDEFINED;
            ajs_ta_get_index(vm, ta, (uint32_t)i, &ev);
            ajs_ta_set_index(vm, dst, (uint32_t)i, ev);
        }
    }
    ajs_temp_root_restore(vm, rs);
    return out;
}

/* Generic %TypedArray%.prototype methods delegating after validation or executing directly */
static AjsValue js_ta_at(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t len = 0;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Not a TypedArray");
    AjsObject *ta = (AjsObject *)ajs_val_to_gc(this_val);
    if (!ajs_ta_check_valid_len(vm, ta, &len, NULL)) return AJS_VAL_EXCEPTION;
    double rel = 0.0;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &rel)) return AJS_VAL_EXCEPTION;
    rel = ajs_double_to_integer_or_inf(rel);
    double k = rel >= 0.0 ? rel : ((double)len + rel);
    if (k < 0.0 || k >= (double)len) return AJS_VAL_UNDEFINED;
    AjsValue out = AJS_VAL_UNDEFINED;
    ajs_ta_get_index(vm, ta, (uint32_t)k, &out);
    return out;
}

/* Delegated Array.prototype methods (validated on TypedArray first; map/filter convert result via ta_species_create!) */
static AjsValue js_ta_delegate_arr(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    static const char *const k_names[] = {
        "indexOf", "lastIndexOf", "includes", "find", "findIndex", "findLast", "findLastIndex",
        "every", "some", "forEach", "reduce", "reduceRight", "join", "toLocaleString", "map", "filter"
    };
    int32_t m = ta_cur_magic(vm);
    size_t len = 0;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Not a TypedArray");
    AjsObject *ta = (AjsObject *)ajs_val_to_gc(this_val);
    if (!ajs_ta_check_valid_len(vm, ta, &len, NULL)) return AJS_VAL_EXCEPTION;

    size_t rs = ajs_temp_root_save(vm);
    if (m == 14 || m == 15) { /* map or filter */
        AjsValue cb = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
        if (!ajs_is_callable(vm, cb)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Callback must be callable");
        AjsValue this_arg = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
        AjsValue kept = ajs_new_array(vm, 0);
        ajs_temp_root_push(vm, kept);
        uint32_t kcnt = 0;
        for (uint32_t i = 0; i < (uint32_t)len; i++) {
            size_t ls = ajs_temp_root_save(vm);
            AjsValue ev = AJS_VAL_UNDEFINED;
            ajs_ta_get_index(vm, ta, i, &ev);
            ajs_temp_root_push(vm, ev);
            AjsValue cargs[3] = { ev, ajs_val_number((double)i), this_val };
            AjsValue rv = ajs_call_internal(vm, cb, this_arg, 3, cargs);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            if (m == 14) {
                ajs_set_prop_index(vm, kept, kcnt++, rv);
            } else if (ajs_to_bool(vm, rv)) {
                ajs_set_prop_index(vm, kept, kcnt++, ev);
            }
            ajs_temp_root_restore(vm, ls);
        }
        AjsValue lnum = ajs_val_number((double)kcnt);
        AjsValue out = ta_species_create(vm, ta, 1, &lnum);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, out);
        AjsObject *dst = (AjsObject *)ajs_val_to_gc(out);
        for (uint32_t i = 0; i < kcnt; i++) {
            AjsValue iv = ajs_get_prop_index(vm, kept, i);
            if (!ajs_ta_set_index(vm, dst, i, iv)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
        ajs_temp_root_restore(vm, rs);
        return out;
    }

    /* Materialize view wrapper or call Array.prototype.<method> directly?
     * Note: Array.prototype.<method> reads `this.length`! On TypedArray, `length` is an accessor on %TypedArray%.prototype,
     * so `ajs_obj_get(this_val, "length")` returns `elen`, AND `ajs_get_prop_index(this_val, k)` reads TypedArray element k!
     * That means Array.prototype methods work natively on TypedArray receivers! */
    AjsValue fn = ajs_get_prop_str(vm, vm->active_realm->array_proto, k_names[m]);
    ajs_temp_root_push(vm, fn);
    AjsValue res = ajs_call_internal(vm, fn, this_val, argc, argv);
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_ta_iter_create(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    int32_t kind = ta_cur_magic(vm);
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT) || ((AjsObject *)ajs_val_to_gc(this_val))->kind != AJS_OBJ_TYPED_ARRAY) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Method called on incompatible TypedArray receiver");
    }
    if (!ajs_ta_check_valid_len(vm, (AjsObject *)ajs_val_to_gc(this_val), NULL, NULL)) return AJS_VAL_EXCEPTION;
    AjsObject *it = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ITERATOR, vm->active_realm->array_iter_proto);
    if (!it) return AJS_VAL_EXCEPTION;
    it->u.iter.kind = (AjsIterKind)kind;
    it->u.iter.target = this_val;
    it->u.iter.index = 0;
    it->u.iter.done = false;
    return ajs_val_from_gc(it);
}
/* ============================================================================
 * 7. ES2026 Uint8Array Base64 & Hex Methods
 * ============================================================================ */

static AjsObject *require_uint8_ta(AjsVm *vm, AjsValue v, size_t *out_len, size_t *out_boff) {
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Method requires a Uint8Array receiver");
        return NULL;
    }
    AjsObject *ta = (AjsObject *)ajs_val_to_gc(v);
    if (ta->kind != AJS_OBJ_TYPED_ARRAY || ta->u.view.kind != AJS_TA_UINT8) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Method requires a Uint8Array receiver");
        return NULL;
    }
    if (!ajs_ta_check_valid_len(vm, ta, out_len, out_boff)) return NULL;
    return ta;
}

static bool parse_b64_opts(AjsVm *vm, AjsValue opts, bool for_encode,
                           bool *out_url, int *out_last_chunk, bool *out_omit_pad) {
    *out_url = false;
    *out_last_chunk = 0; /* 0 = loose, 1 = strict, 2 = stop-before-partial */
    *out_omit_pad = false;
    if (ajs_is_undefined(opts)) return true;
    if (!ajs_is_object(vm, opts)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Base64 options must be an object");
        return false;
    }
    AjsValue alph = ajs_get_prop_str(vm, opts, "alphabet");
    if (vm->has_exception) return false;
    if (!ajs_is_undefined(alph)) {
        if (!ajs_val_is_gc_kind(alph, AJS_GC_STRING)) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Base64 alphabet option must be a string");
            return false;
        }
        const AjsString *as = (const AjsString *)ajs_val_to_gc(alph);
        if (ajs_str_eq_cstr(as, "base64")) *out_url = false;
        else if (ajs_str_eq_cstr(as, "base64url")) *out_url = true;
        else {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Invalid alphabet option");
            return false;
        }
    }
    if (for_encode) {
        AjsValue op = ajs_get_prop_str(vm, opts, "omitPadding");
        if (vm->has_exception) return false;
        *out_omit_pad = ajs_to_bool(vm, op);
    } else {
        AjsValue lc = ajs_get_prop_str(vm, opts, "lastChunkHandling");
        if (vm->has_exception) return false;
        if (!ajs_is_undefined(lc)) {
            if (!ajs_val_is_gc_kind(lc, AJS_GC_STRING)) {
                ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "lastChunkHandling must be a string");
                return false;
            }
            const AjsString *ls = (const AjsString *)ajs_val_to_gc(lc);
            if (ajs_str_eq_cstr(ls, "loose")) *out_last_chunk = 0;
            else if (ajs_str_eq_cstr(ls, "strict")) *out_last_chunk = 1;
            else if (ajs_str_eq_cstr(ls, "stop-before-partial")) *out_last_chunk = 2;
            else {
                ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Invalid lastChunkHandling option");
                return false;
            }
        }
    }
    return true;
}

static bool is_ascii_ws_b64(uint16_t u) {
    return u == 0x09u || u == 0x0Au || u == 0x0Cu || u == 0x0Du || u == 0x20u;
}

static int b64_char_val(uint16_t u, bool url_mode) {
    if (u >= 'A' && u <= 'Z') return (int)(u - 'A');
    if (u >= 'a' && u <= 'z') return (int)(u - 'a' + 26);
    if (u >= '0' && u <= '9') return (int)(u - '0' + 52);
    if (!url_mode) {
        if (u == '+') return 62;
        if (u == '/') return 63;
    } else {
        if (u == '-') return 62;
        if (u == '_') return 63;
    }
    return -1;
}

static bool decode_base64_core(AjsVm *vm, const AjsString *s, bool url_mode, int last_chunk_mode,
                               uint8_t *dst, size_t max_write, uint32_t *out_read, size_t *out_written) {
    uint32_t i = 0, len = s->len16;
    size_t written = 0;
    uint32_t chunk_vals[4];
    uint32_t chunk_len = 0;
    uint32_t chunk_start_pos = 0;

    while (written < max_write) {
        size_t rem_space = max_write - written;
        while (i < len && is_ascii_ws_b64(s->units[i])) i++;
        if (i >= len) break;

        chunk_start_pos = (chunk_len == 0) ? i : chunk_start_pos;
        uint16_t ch = s->units[i];
        if (ch == '=') break;
        int v = b64_char_val(ch, url_mode);
        if (v < 0) {
            *out_read = i;
            *out_written = written;
            ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Invalid character in base64 string");
            return false;
        }
        chunk_vals[chunk_len++] = (uint32_t)v;
        i++;

        if (chunk_len == 4u) {
            if (rem_space < 3u) {
                /* Rewind partial write into bounded target per setFromBase64 */
                if (rem_space == 1u) {
                    uint32_t triple = (chunk_vals[0] << 18) | (chunk_vals[1] << 12);
                    dst[written++] = (uint8_t)(triple >> 16);
                    /* Rewind i to right after 2nd char! Wait: track exact indices */
                }
                /* Actually check space BEFORE reading 4 chars! Handled cleanly below. */
            }
            uint32_t triple = (chunk_vals[0] << 18) | (chunk_vals[1] << 12) | (chunk_vals[2] << 6) | chunk_vals[3];
            if (rem_space >= 3u) {
                dst[written++] = (uint8_t)((triple >> 16) & 0xFFu);
                dst[written++] = (uint8_t)((triple >> 8) & 0xFFu);
                dst[written++] = (uint8_t)(triple & 0xFFu);
                chunk_len = 0;
                chunk_start_pos = i;
            } else {
                /* Rewind to chunk_start_pos and take exact prefix for setFromBase64 */
                i = chunk_start_pos;
                chunk_len = 0;
                while (i < len && chunk_len < rem_space + 1u) {
                    while (i < len && is_ascii_ws_b64(s->units[i])) i++;
                    chunk_vals[chunk_len++] = (uint32_t)b64_char_val(s->units[i++], url_mode);
                }
                if (rem_space == 1u) {
                    dst[written++] = (uint8_t)((chunk_vals[0] << 2) | (chunk_vals[1] >> 4));
                } else if (rem_space == 2u) {
                    dst[written++] = (uint8_t)((chunk_vals[0] << 2) | (chunk_vals[1] >> 4));
                    dst[written++] = (uint8_t)(((chunk_vals[1] & 0xFu) << 4) | (chunk_vals[2] >> 2));
                }
                *out_read = i;
                *out_written = written;
                return true;
            }
        }
    }

    if (written >= max_write && chunk_len == 0) {
        while (i < len && is_ascii_ws_b64(s->units[i])) i++;
        *out_read = i;
        *out_written = written;
        return true;
    }

    /* Handle padding '=' or final partial chunk (< 4 chars) */
    uint32_t pad = 0;
    if (i < len && s->units[i] == '=') {
        if (chunk_len < 2u) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Unexpected '=' padding in base64");
            return false;
        }
        i++;
        pad = 1u;
        if (chunk_len == 2u) {
            while (i < len && is_ascii_ws_b64(s->units[i])) i++;
            if (i >= len || s->units[i] != '=') {
                if (last_chunk_mode == 2) {
                    *out_read = chunk_start_pos;
                    *out_written = written;
                    return true;
                }
                ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Incomplete '=' padding in base64");
                return false;
            }
            i++;
            pad = 2u;
        }
        while (i < len && is_ascii_ws_b64(s->units[i])) i++;
        if (i < len) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Unexpected characters after '=' padding in base64");
            return false;
        }
    }

    if (chunk_len == 0) {
        *out_read = i;
        *out_written = written;
        return true;
    }
    if (chunk_len == 1u) {
        if (last_chunk_mode == 2 && pad == 0) {
            *out_read = chunk_start_pos;
            *out_written = written;
            return true;
        }
        ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Invalid 1-character base64 tail");
        return false;
    }
    if (pad == 0 && last_chunk_mode == 1) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Missing padding in strict base64 mode");
        return false;
    }
    if (pad == 0 && last_chunk_mode == 2) {
        *out_read = chunk_start_pos;
        *out_written = written;
        return true;
    }
    if (chunk_len == 2u) {
        if (last_chunk_mode == 1 && (chunk_vals[1] & 0xFu) != 0) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Non-zero padding bits in strict base64 mode");
            return false;
        }
        if (written + 1u <= max_write) {
            dst[written++] = (uint8_t)((chunk_vals[0] << 2) | (chunk_vals[1] >> 4));
        } else {
            *out_read = chunk_start_pos;
            *out_written = written;
            return true;
        }
    } else if (chunk_len == 3u) {
        if (last_chunk_mode == 1 && (chunk_vals[2] & 0x3u) != 0) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Non-zero padding bits in strict base64 mode");
            return false;
        }
        if (written + 2u <= max_write) {
            dst[written++] = (uint8_t)((chunk_vals[0] << 2) | (chunk_vals[1] >> 4));
            dst[written++] = (uint8_t)(((chunk_vals[1] & 0xFu) << 4) | (chunk_vals[2] >> 2));
        } else if (written + 1u <= max_write) {
            dst[written++] = (uint8_t)((chunk_vals[0] << 2) | (chunk_vals[1] >> 4));
            /* Advance chunk_start_pos past 2nd base64 char */
            uint32_t p = chunk_start_pos, seen = 0;
            while (p < len && seen < 2u) {
                if (!is_ascii_ws_b64(s->units[p])) seen++;
                p++;
            }
            *out_read = p;
            *out_written = written;
            return true;
        }
    }
    *out_read = i;
    *out_written = written;
    return true;
}

static AjsValue js_u8_fromBase64(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue sv = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(sv, AJS_GC_STRING)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Uint8Array.fromBase64 requires a string");
    }
    bool url_mode = false, omit_pad = false;
    int lc_mode = 0;
    if (!parse_b64_opts(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, false, &url_mode, &lc_mode, &omit_pad)) {
        return AJS_VAL_EXCEPTION;
    }
    const AjsString *s = (const AjsString *)ajs_val_to_gc(sv);
    size_t cap = ((size_t)s->len16 / 4u + 1u) * 3u;
    uint8_t *buf = (uint8_t *)malloc(cap > 0 ? cap : 1u);
    if (!buf) return ajs_throw(vm, vm->oom_error);
    uint32_t read_n = 0;
    size_t written_n = 0;
    if (!decode_base64_core(vm, s, url_mode, lc_mode, buf, cap, &read_n, &written_n)) {
        free(buf);
        return AJS_VAL_EXCEPTION;
    }
    AjsValue out = ajs_new_typed_array(vm, AJS_TA_UINT8, buf, written_n);
    free(buf);
    return out;
}

static AjsValue js_u8_setFromBase64(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t tlen = 0, tboff = 0;
    AjsObject *ta = require_uint8_ta(vm, this_val, &tlen, &tboff);
    if (!ta) return AJS_VAL_EXCEPTION;
    AjsValue sv = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(sv, AJS_GC_STRING)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Uint8Array.prototype.setFromBase64 requires a string");
    }
    bool url_mode = false, omit_pad = false;
    int lc_mode = 0;
    if (!parse_b64_opts(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, false, &url_mode, &lc_mode, &omit_pad)) {
        return AJS_VAL_EXCEPTION;
    }
    if (!ajs_ta_check_valid_len(vm, ta, &tlen, &tboff)) return AJS_VAL_EXCEPTION;

    const AjsString *s = (const AjsString *)ajs_val_to_gc(sv);
    uint8_t *tmp = tlen > 0 ? (uint8_t *)malloc(tlen) : NULL;
    if (tlen > 0 && !tmp) return ajs_throw(vm, vm->oom_error);
    uint32_t read_n = 0;
    size_t written_n = 0;
    bool ok = decode_base64_core(vm, s, url_mode, lc_mode, tmp, tlen, &read_n, &written_n);
    if (!ok) {
        free(tmp);
        return AJS_VAL_EXCEPTION;
    }
    if (written_n > 0) ajs_memcpy(ta->u.view.buffer->u.buf.bytes + tboff, tmp, written_n);
    free(tmp);

    size_t rs = ajs_temp_root_save(vm);
    AjsValue res = ajs_new_object(vm);
    ajs_temp_root_push(vm, res);
    ajs_define_prop_str(vm, res, "read", ajs_val_number((double)read_n), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "written", ajs_val_number((double)written_n), AJS_PROP_DEFAULT);
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_u8_toBase64(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t len = 0, boff = 0;
    AjsObject *ta = require_uint8_ta(vm, this_val, &len, &boff);
    if (!ta) return AJS_VAL_EXCEPTION;
    bool url_mode = false, omit_pad = false;
    int lc_mode = 0;
    if (!parse_b64_opts(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, true, &url_mode, &lc_mode, &omit_pad)) {
        return AJS_VAL_EXCEPTION;
    }
    if (!ajs_ta_check_valid_len(vm, ta, &len, &boff)) return AJS_VAL_EXCEPTION;

    static const char std_tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char url_tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char *tab = url_mode ? url_tab : std_tab;
    const uint8_t *src = ta->u.view.buffer->u.buf.bytes + boff;

    size_t cap = (len / 3u + 1u) * 4u + 1u;
    char *out = (char *)malloc(cap);
    if (!out) return ajs_throw(vm, vm->oom_error);
    size_t o = 0, i = 0;
    while (i + 3u <= len) {
        uint32_t t = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1u] << 8) | src[i + 2u];
        out[o++] = tab[(t >> 18) & 63u];
        out[o++] = tab[(t >> 12) & 63u];
        out[o++] = tab[(t >> 6) & 63u];
        out[o++] = tab[t & 63u];
        i += 3u;
    }
    if (len - i == 1u) {
        uint32_t t = (uint32_t)src[i];
        out[o++] = tab[(t >> 2) & 63u];
        out[o++] = tab[(t & 3u) << 4];
        if (!omit_pad) { out[o++] = '='; out[o++] = '='; }
    } else if (len - i == 2u) {
        uint32_t t = ((uint32_t)src[i] << 8) | src[i + 1u];
        out[o++] = tab[(t >> 10) & 63u];
        out[o++] = tab[(t >> 4) & 63u];
        out[o++] = tab[(t & 15u) << 2];
        if (!omit_pad) out[o++] = '=';
    }
    AjsString *str = ajs_str_from_utf8(vm, out, o, false);
    free(out);
    return str ? ajs_val_from_gc(str) : AJS_VAL_EXCEPTION;
}

static int hex_u16_val(uint16_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

static AjsValue js_u8_fromHex(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue sv = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(sv, AJS_GC_STRING)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Uint8Array.fromHex requires a string");
    }
    const AjsString *s = (const AjsString *)ajs_val_to_gc(sv);
    if (s->len16 % 2u != 0) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Hex string must have an even length");
    }
    size_t blen = s->len16 / 2u;
    uint8_t *buf = (uint8_t *)malloc(blen > 0 ? blen : 1u);
    if (!buf) return ajs_throw(vm, vm->oom_error);
    for (size_t i = 0; i < blen; i++) {
        int hi = hex_u16_val(s->units[i * 2u]);
        int lo = hex_u16_val(s->units[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            free(buf);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Invalid hex digit in Uint8Array.fromHex");
        }
        buf[i] = (uint8_t)((hi << 4) | lo);
    }
    AjsValue out = ajs_new_typed_array(vm, AJS_TA_UINT8, buf, blen);
    free(buf);
    return out;
}

static AjsValue js_u8_setFromHex(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t tlen = 0, tboff = 0;
    AjsObject *ta = require_uint8_ta(vm, this_val, &tlen, &tboff);
    if (!ta) return AJS_VAL_EXCEPTION;
    AjsValue sv = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(sv, AJS_GC_STRING)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Uint8Array.prototype.setFromHex requires a string");
    }
    const AjsString *s = (const AjsString *)ajs_val_to_gc(sv);
    if (s->len16 % 2u != 0) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Hex string must have an even length");
    }
    size_t pairs = s->len16 / 2u;
    if (pairs > tlen) pairs = tlen;
    uint8_t *tmp = pairs > 0 ? (uint8_t *)malloc(pairs) : NULL;
    if (pairs > 0 && !tmp) return ajs_throw(vm, vm->oom_error);
    for (size_t i = 0; i < pairs; i++) {
        int hi = hex_u16_val(s->units[i * 2u]);
        int lo = hex_u16_val(s->units[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            free(tmp);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Invalid hex digit");
        }
        tmp[i] = (uint8_t)((hi << 4) | lo);
    }
    if (pairs > 0) ajs_memcpy(ta->u.view.buffer->u.buf.bytes + tboff, tmp, pairs);
    free(tmp);

    size_t rs = ajs_temp_root_save(vm);
    AjsValue res = ajs_new_object(vm);
    ajs_temp_root_push(vm, res);
    ajs_define_prop_str(vm, res, "read", ajs_val_number((double)(pairs * 2u)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "written", ajs_val_number((double)pairs), AJS_PROP_DEFAULT);
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_u8_toHex(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    size_t len = 0, boff = 0;
    AjsObject *ta = require_uint8_ta(vm, this_val, &len, &boff);
    if (!ta) return AJS_VAL_EXCEPTION;
    static const char hex[] = "0123456789abcdef";
    char *out = (char *)malloc(len * 2u + 1u);
    if (!out) return ajs_throw(vm, vm->oom_error);
    const uint8_t *src = ta->u.view.buffer->u.buf.bytes + boff;
    for (size_t i = 0; i < len; i++) {
        out[i * 2u]      = hex[(src[i] >> 4) & 0xFu];
        out[i * 2u + 1u] = hex[src[i] & 0xFu];
    }
    AjsString *str = ajs_str_from_utf8(vm, out, len * 2u, false);
    free(out);
    return str ? ajs_val_from_gc(str) : AJS_VAL_EXCEPTION;
}

/* ============================================================================
 * 8. Atomics Built-in Object
 * ============================================================================ */

static AjsObject *validate_integer_ta(AjsVm *vm, AjsValue v, bool waitable_only, size_t *out_len, size_t *out_boff) {
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Atomics requires an integer TypedArray");
        return NULL;
    }
    AjsObject *ta = (AjsObject *)ajs_val_to_gc(v);
    if (ta->kind != AJS_OBJ_TYPED_ARRAY) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Atomics requires an integer TypedArray");
        return NULL;
    }
    AjsTypedArrayKind k = ta->u.view.kind;
    if (waitable_only) {
        if (k != AJS_TA_INT32 && k != AJS_TA_BIGINT64) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Atomics.wait/notify requires Int32Array or BigInt64Array");
            return NULL;
        }
    } else {
        if (k == AJS_TA_UINT8_CLAMPED || k == AJS_TA_FLOAT32 || k == AJS_TA_FLOAT64) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Atomics does not support Float or Clamped TypedArrays");
            return NULL;
        }
    }
    if (!ajs_ta_check_valid_len(vm, ta, out_len, out_boff)) return NULL;
    return ta;
}

static bool validate_atomic_access(AjsVm *vm, AjsObject *ta, size_t len, AjsValue idx_val, uint32_t *out_idx) {
    (void)ta;
    size_t idx = 0;
    if (!ajs_to_index(vm, idx_val, &idx)) return false;
    if (idx >= len) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Atomics index out of bounds");
        return false;
    }
    *out_idx = (uint32_t)idx;
    return true;
}

/* Atomics RMW: magic 0=load, 1=store, 2=add, 3=sub, 4=and, 5=or, 6=xor, 7=exchange, 8=compareExchange */
static AjsValue js_atomics_op(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    int32_t op = ta_cur_magic(vm);
    size_t len = 0, boff = 0;
    AjsObject *ta = validate_integer_ta(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, false, &len, &boff);
    if (!ta) return AJS_VAL_EXCEPTION;
    uint32_t idx = 0;
    if (!validate_atomic_access(vm, ta, len, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, &idx)) return AJS_VAL_EXCEPTION;

    if (op == 0) {
        AjsValue out = AJS_VAL_UNDEFINED;
        ajs_ta_get_index(vm, ta, idx, &out);
        return out;
    }

    AjsTypedArrayKind kind = ta->u.view.kind;
    bool is_bi = ta_is_bigint_kind(kind);
    AjsValue v1 = argc > 2 ? argv[2] : AJS_VAL_UNDEFINED;
    AjsValue v2 = argc > 3 ? argv[3] : AJS_VAL_UNDEFINED;

    size_t rs = ajs_temp_root_save(vm);
    AjsBigInt *bi1 = NULL, *bi2 = NULL;
    double d1 = 0.0, d2 = 0.0;
    if (is_bi) {
        bi1 = ta_to_bigint(vm, v1);
        if (!bi1) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(bi1));
        if (op == 8) {
            bi2 = ta_to_bigint(vm, v2);
            if (!bi2) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            ajs_temp_root_push(vm, ajs_val_from_gc(bi2));
        }
    } else {
        if (!ajs_to_number(vm, v1, &d1)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        d1 = ajs_double_to_integer_or_inf(d1);
        if (isinf(d1)) d1 = 0.0;
        if (op == 8) {
            if (!ajs_to_number(vm, v2, &d2)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            d2 = ajs_double_to_integer_or_inf(d2);
            if (isinf(d2)) d2 = 0.0;
        }
    }

    /* Re-validate buffer after user valueOf */
    if (!ajs_ta_check_valid_len(vm, ta, &len, &boff) || idx >= len) {
        ajs_temp_root_restore(vm, rs);
        if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Atomics index out of bounds after resize");
        return AJS_VAL_EXCEPTION;
    }

    if (op == 1) { /* Atomics.store returns coerced integer value! */
        AjsValue coerced = is_bi ? ajs_val_from_gc(bi1) : ajs_val_number(d1 == 0.0 ? 0.0 : d1);
        ajs_ta_set_index(vm, ta, idx, coerced);
        ajs_temp_root_restore(vm, rs);
        return coerced;
    }

    AjsValue old_val = AJS_VAL_UNDEFINED;
    ajs_ta_get_index(vm, ta, idx, &old_val);
    ajs_temp_root_push(vm, old_val);

    if (is_bi) {
        uint64_t old_u = ajs_bigint_to_u64_wrap((const AjsBigInt *)ajs_val_to_gc(old_val));
        uint64_t a_u   = ajs_bigint_to_u64_wrap(bi1);
        uint64_t new_u = old_u;
        switch (op) {
            case 2: new_u = old_u + a_u; break;
            case 3: new_u = old_u - a_u; break;
            case 4: new_u = old_u & a_u; break;
            case 5: new_u = old_u | a_u; break;
            case 6: new_u = old_u ^ a_u; break;
            case 7: new_u = a_u; break;
            case 8: {
                uint64_t rep_u = ajs_bigint_to_u64_wrap(bi2);
                if (old_u == a_u) new_u = rep_u;
                break;
            }
            default: break;
        }
        AjsValue nv = (kind == AJS_TA_BIGINT64)
                      ? ajs_val_from_gc(ajs_bigint_new_i64(vm, (int64_t)new_u))
                      : ajs_val_from_gc(ajs_bigint_new_u64(vm, new_u, new_u ? 1 : 0));
        ajs_ta_set_index(vm, ta, idx, nv);
    } else {
        uint32_t old_u = ajs_double_to_uint32(ajs_val_to_number_unchecked(old_val));
        uint32_t a_u   = ajs_double_to_uint32(d1);
        uint32_t new_u = old_u;
        switch (op) {
            case 2: new_u = old_u + a_u; break;
            case 3: new_u = old_u - a_u; break;
            case 4: new_u = old_u & a_u; break;
            case 5: new_u = old_u | a_u; break;
            case 6: new_u = old_u ^ a_u; break;
            case 7: new_u = a_u; break;
            case 8: {
                uint32_t exp_u = a_u;
                /* Mask expected value to element width per ECMA-262 */
                if (kind == AJS_TA_INT8)        exp_u = (uint32_t)(int32_t)(int8_t)exp_u;
                else if (kind == AJS_TA_UINT8)  exp_u = exp_u & 0xFFu;
                else if (kind == AJS_TA_INT16)  exp_u = (uint32_t)(int32_t)(int16_t)exp_u;
                else if (kind == AJS_TA_UINT16) exp_u = exp_u & 0xFFFFu;
                if (old_u == exp_u) new_u = ajs_double_to_uint32(d2);
                break;
            }
            default: break;
        }
        ajs_ta_set_index(vm, ta, idx, ajs_val_number((double)(int32_t)new_u));
    }

    ajs_temp_root_restore(vm, rs);
    return old_val;
}

static AjsValue js_atomics_isLockFree(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    double d = 0.0;
    if (!ajs_to_number(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &d)) return AJS_VAL_EXCEPTION;
    d = ajs_double_to_integer_or_inf(d);
    return ajs_bool(d == 1.0 || d == 2.0 || d == 4.0 || d == 8.0);
}

static AjsValue js_atomics_wait(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    size_t len = 0, boff = 0;
    AjsObject *ta = validate_integer_ta(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, true, &len, &boff);
    if (!ta) return AJS_VAL_EXCEPTION;
    if (!ta->u.view.buffer->u.buf.shared) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Atomics.wait requires a SharedArrayBuffer");
    }
    uint32_t idx = 0;
    if (!validate_atomic_access(vm, ta, len, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, &idx)) return AJS_VAL_EXCEPTION;

    bool is_bi = (ta->u.view.kind == AJS_TA_BIGINT64);
    AjsBigInt *bi_exp = NULL;
    int32_t i32_exp = 0;
    if (is_bi) {
        bi_exp = ta_to_bigint(vm, argc > 2 ? argv[2] : AJS_VAL_UNDEFINED);
        if (!bi_exp) return AJS_VAL_EXCEPTION;
    } else {
        if (!ajs_to_int32(vm, argc > 2 ? argv[2] : AJS_VAL_UNDEFINED, &i32_exp)) return AJS_VAL_EXCEPTION;
    }
    double timeout = INFINITY;
    if (argc > 3 && !ajs_is_undefined(argv[3])) {
        if (!ajs_to_number(vm, argv[3], &timeout)) return AJS_VAL_EXCEPTION;
        if (isnan(timeout)) timeout = INFINITY;
        else if (timeout < 0.0) timeout = 0.0;
    }
    AjsValue cur = AJS_VAL_UNDEFINED;
    ajs_ta_get_index(vm, ta, idx, &cur);
    if (is_bi) {
        int64_t cv = ajs_bigint_to_i64_wrap((const AjsBigInt *)ajs_val_to_gc(cur));
        if (cv != ajs_bigint_to_i64_wrap(bi_exp)) return ajs_string_cstr(vm, "not-equal");
    } else {
        if ((int32_t)ajs_val_to_number_unchecked(cur) != i32_exp) return ajs_string_cstr(vm, "not-equal");
    }
    if (timeout == 0.0) return ajs_string_cstr(vm, "timed-out");
    return ajs_string_cstr(vm, "timed-out");
}

static AjsValue js_atomics_notify(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    size_t len = 0, boff = 0;
    AjsObject *ta = validate_integer_ta(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, true, &len, &boff);
    if (!ta) return AJS_VAL_EXCEPTION;
    uint32_t idx = 0;
    if (!validate_atomic_access(vm, ta, len, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, &idx)) return AJS_VAL_EXCEPTION;
    if (argc > 2 && !ajs_is_undefined(argv[2])) {
        double cnt = 0.0;
        if (!ajs_to_number(vm, argv[2], &cnt)) return AJS_VAL_EXCEPTION;
    }
    return ajs_val_number(0.0);
}

static AjsValue js_atomics_pause(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    if (argc > 0 && !ajs_is_undefined(argv[0])) {
        if (!ajs_val_is_number(argv[0])) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Atomics.pause iterationNumber must be an integral Number");
        }
        double d = ajs_val_to_number_unchecked(argv[0]);
        if (!isfinite(d) || floor(d) != d) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Atomics.pause iterationNumber must be an integral Number");
        }
    }
    return AJS_VAL_UNDEFINED;
}

/* ============================================================================
 * 9. Realm Initialization for Buffers, Views & Atomics
 * ============================================================================ */

static AjsValue js_ta_species_this(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)vm; (void)c; (void)v; (void)u;
    return this_val;
}

static void ta_def_species(AjsVm *vm, AjsRealm *r, AjsValue ctor) {
    size_t rs = ajs_temp_root_save(vm);
    AjsValue g = ta_make_fn(vm, r, js_ta_species_this, "get [Symbol.species]", 0, false, 0, r->function_proto);
    ajs_temp_root_push(vm, g);
    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_GET | AJS_DESC_HAS_SET |
              AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    d.getter = g;
    d.setter = AJS_VAL_UNDEFINED;
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(ctor),
                            ajs_val_from_gc(vm->well_known_syms[AJS_SYM_SPECIES]), &d, false);
    ajs_temp_root_restore(vm, rs);
}

static void ta_def_tag(AjsVm *vm, AjsValue obj, const char *tag) {
    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
              AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    d.value = ajs_val_from_gc(ajs_str_from_cstr(vm, tag, true));
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(obj),
                            ajs_val_from_gc(vm->well_known_syms[AJS_SYM_TO_STRING_TAG]), &d, false);
}

static void ta_def_getter_magic(AjsVm *vm, AjsRealm *r, AjsValue obj, const char *name, AjsNativeFn fn, int32_t magic) {
    size_t rs = ajs_temp_root_save(vm);
    char gname[64];
    snprintf(gname, sizeof(gname), "get %s", name);
    AjsValue g = ta_make_fn(vm, r, fn, gname, 0, false, magic, r->function_proto);
    ajs_temp_root_push(vm, g);
    ajs_define_accessor_str(vm, obj, name, g, AJS_VAL_UNDEFINED, AJS_PROP_CONFIGURABLE);
    ajs_temp_root_restore(vm, rs);
}

void ajs_init_typedarray_builtins(AjsVm *vm, AjsRealm *r) {
    size_t rs = ajs_temp_root_save(vm);

    /* 1. ArrayBuffer */
    r->ab_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->ab_ctor  = ta_make_fn(vm, r, js_ab_sab_ctor, "ArrayBuffer", 1, true, -37, r->function_proto);
    ajs_define_prop_str(vm, r->ab_ctor, "prototype", r->ab_proto, 0);
    ajs_define_prop_str(vm, r->ab_proto, "constructor", r->ab_ctor, AJS_METHOD_FLAGS);
    ajs_define_prop_str(vm, r->global_obj, "ArrayBuffer", r->ab_ctor, AJS_METHOD_FLAGS);
    ta_def_species(vm, r, r->ab_ctor);
    ajs_def_native_method(vm, r, r->ab_ctor, "isView", js_ab_isView, 1, AJS_METHOD_FLAGS, 0);
    ta_def_getter_magic(vm, r, r->ab_proto, "byteLength", js_ab_byteLength, 0);
    ta_def_getter_magic(vm, r, r->ab_proto, "maxByteLength", js_ab_maxByteLength, 0);
    ta_def_getter_magic(vm, r, r->ab_proto, "resizable", js_ab_resizable, 0);
    ta_def_getter_magic(vm, r, r->ab_proto, "detached", js_ab_detached, 0);
    ajs_def_native_method(vm, r, r->ab_proto, "slice", js_ab_slice, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ab_proto, "resize", js_ab_resize_or_grow, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ab_proto, "transfer", js_ab_transfer, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ab_proto, "transferToFixedLength", js_ab_transfer, 0, AJS_METHOD_FLAGS, 1);
    ta_def_tag(vm, r->ab_proto, "ArrayBuffer");

    /* 2. SharedArrayBuffer */
    r->sab_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->sab_ctor  = ta_make_fn(vm, r, js_ab_sab_ctor, "SharedArrayBuffer", 1, true, -38, r->function_proto);
    ajs_define_prop_str(vm, r->sab_ctor, "prototype", r->sab_proto, 0);
    ajs_define_prop_str(vm, r->sab_proto, "constructor", r->sab_ctor, AJS_METHOD_FLAGS);
    ajs_define_prop_str(vm, r->global_obj, "SharedArrayBuffer", r->sab_ctor, AJS_METHOD_FLAGS);
    ta_def_species(vm, r, r->sab_ctor);
    ta_def_getter_magic(vm, r, r->sab_proto, "byteLength", js_ab_byteLength, 1);
    ta_def_getter_magic(vm, r, r->sab_proto, "maxByteLength", js_ab_maxByteLength, 1);
    ta_def_getter_magic(vm, r, r->sab_proto, "growable", js_ab_resizable, 1);
    ajs_def_native_method(vm, r, r->sab_proto, "slice", js_ab_slice, 2, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->sab_proto, "grow", js_ab_resize_or_grow, 1, AJS_METHOD_FLAGS, 1);
    ta_def_tag(vm, r->sab_proto, "SharedArrayBuffer");

    /* 3. DataView */
    r->dv_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->dv_ctor  = ta_make_fn(vm, r, js_dv_ctor, "DataView", 1, true, -39, r->function_proto);
    ajs_define_prop_str(vm, r->dv_ctor, "prototype", r->dv_proto, 0);
    ajs_define_prop_str(vm, r->dv_proto, "constructor", r->dv_ctor, AJS_METHOD_FLAGS);
    ajs_define_prop_str(vm, r->global_obj, "DataView", r->dv_ctor, AJS_METHOD_FLAGS);
    ta_def_getter_magic(vm, r, r->dv_proto, "buffer", js_dv_prop, 0);
    ta_def_getter_magic(vm, r, r->dv_proto, "byteOffset", js_dv_prop, 1);
    ta_def_getter_magic(vm, r, r->dv_proto, "byteLength", js_dv_prop, 2);

    static const struct { const char *get_n; const char *set_n; int32_t m; int get_len; int set_len; } k_dv_ops[] = {
        {"getInt8", "setInt8", AJS_TA_INT8, 1, 2},
        {"getUint8", "setUint8", AJS_TA_UINT8, 1, 2},
        {"getInt16", "setInt16", AJS_TA_INT16, 1, 2},
        {"getUint16", "setUint16", AJS_TA_UINT16, 1, 2},
        {"getInt32", "setInt32", AJS_TA_INT32, 1, 2},
        {"getUint32", "setUint32", AJS_TA_UINT32, 1, 2},
        {"getFloat16", "setFloat16", 12, 1, 2},
        {"getFloat32", "setFloat32", AJS_TA_FLOAT32, 1, 2},
        {"getFloat64", "setFloat64", AJS_TA_FLOAT64, 1, 2},
        {"getBigInt64", "setBigInt64", AJS_TA_BIGINT64, 1, 2},
        {"getBigUint64", "setBigUint64", AJS_TA_BIGUINT64, 1, 2}
    };
    for (size_t i = 0; i < sizeof(k_dv_ops) / sizeof(k_dv_ops[0]); i++) {
        ajs_def_native_method(vm, r, r->dv_proto, k_dv_ops[i].get_n, js_dv_get, k_dv_ops[i].get_len, AJS_METHOD_FLAGS, k_dv_ops[i].m);
        ajs_def_native_method(vm, r, r->dv_proto, k_dv_ops[i].set_n, js_dv_set, k_dv_ops[i].set_len, AJS_METHOD_FLAGS, k_dv_ops[i].m);
    }
    ta_def_tag(vm, r->dv_proto, "DataView");

    /* 4. %TypedArray% Intrinsic Base */
    r->ta_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->ta_ctor  = ta_make_fn(vm, r, js_ta_abstract_ctor, "TypedArray", 0, true, -40, r->function_proto);
    ajs_define_prop_str(vm, r->ta_ctor, "prototype", r->ta_proto, 0);
    ajs_define_prop_str(vm, r->ta_proto, "constructor", r->ta_ctor, AJS_METHOD_FLAGS);
    ta_def_species(vm, r, r->ta_ctor);
    ajs_def_native_method(vm, r, r->ta_ctor, "of", js_ta_of, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ta_ctor, "from", js_ta_from, 1, AJS_METHOD_FLAGS, 0);

    ta_def_getter_magic(vm, r, r->ta_proto, "buffer", js_ta_getter, 0);
    ta_def_getter_magic(vm, r, r->ta_proto, "byteLength", js_ta_getter, 1);
    ta_def_getter_magic(vm, r, r->ta_proto, "byteOffset", js_ta_getter, 2);
    ta_def_getter_magic(vm, r, r->ta_proto, "length", js_ta_getter, 3);
    {
        AjsValue tg = ta_make_fn(vm, r, js_ta_getter, "get [Symbol.toStringTag]", 0, false, 4, r->function_proto);
        ajs_temp_root_push(vm, tg);
        AjsPropDesc td;
        memset(&td, 0, sizeof(td));
        td.flags = AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_GET | AJS_DESC_HAS_SET |
                   AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
        td.getter = tg;
        td.setter = AJS_VAL_UNDEFINED;
        ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(r->ta_proto),
                                ajs_val_from_gc(vm->well_known_syms[AJS_SYM_TO_STRING_TAG]), &td, false);
    }

    ajs_def_native_method(vm, r, r->ta_proto, "at", js_ta_at, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ta_proto, "set", js_ta_set, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ta_proto, "subarray", js_ta_subarray, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ta_proto, "slice", js_ta_slice, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ta_proto, "copyWithin", js_ta_copyWithin, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ta_proto, "fill", js_ta_fill, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ta_proto, "reverse", js_ta_reverse, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ta_proto, "toReversed", js_ta_reverse, 0, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->ta_proto, "sort", js_ta_sort, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ta_proto, "toSorted", js_ta_sort, 1, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->ta_proto, "with", js_ta_with, 2, AJS_METHOD_FLAGS, 0);

    static const struct { const char *name; int len; int32_t m; } k_ta_del[] = {
        {"indexOf", 1, 0}, {"lastIndexOf", 1, 1}, {"includes", 1, 2},
        {"find", 1, 3}, {"findIndex", 1, 4}, {"findLast", 1, 5}, {"findLastIndex", 1, 6},
        {"every", 1, 7}, {"some", 1, 8}, {"forEach", 1, 9},
        {"reduce", 1, 10}, {"reduceRight", 1, 11},
        {"join", 1, 12}, {"toLocaleString", 0, 13},
        {"map", 1, 14}, {"filter", 1, 15}
    };
    for (size_t i = 0; i < sizeof(k_ta_del) / sizeof(k_ta_del[0]); i++) {
        ajs_def_native_method(vm, r, r->ta_proto, k_ta_del[i].name, js_ta_delegate_arr,
                              k_ta_del[i].len, AJS_METHOD_FLAGS, k_ta_del[i].m);
    }
    /* %TypedArray%.prototype.toString === %Array.prototype.toString% */
    ajs_define_prop_str(vm, r->ta_proto, "toString", ajs_get_prop_str(vm, r->array_proto, "toString"), AJS_METHOD_FLAGS);

    ajs_def_native_method(vm, r, r->ta_proto, "keys", js_ta_iter_create, 0, AJS_METHOD_FLAGS, AJS_ITER_ARRAY_KEYS);
    ajs_def_native_method(vm, r, r->ta_proto, "entries", js_ta_iter_create, 0, AJS_METHOD_FLAGS, AJS_ITER_ARRAY_ENTRIES);
    AjsValue ta_vals = ta_make_fn(vm, r, js_ta_iter_create, "values", 0, false, AJS_ITER_ARRAY_VALUES, r->function_proto);
    ajs_temp_root_push(vm, ta_vals);
    ajs_define_prop_str(vm, r->ta_proto, "values", ta_vals, AJS_METHOD_FLAGS);
    {
        AjsPropDesc sd;
        memset(&sd, 0, sizeof(sd));
        sd.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                   AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
        sd.value = ta_vals;
        ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(r->ta_proto),
                                ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), &sd, false);
    }

    /* 5. 11 Concrete TypedArray Constructors */
    for (int k = AJS_TA_INT8; k <= AJS_TA_BIGUINT64; k++) {
        const char *cname = ta_kind_name((AjsTypedArrayKind)k);
        AjsValue bpe = ajs_val_number((double)ajs_ta_elem_size((AjsTypedArrayKind)k));
        r->ta_sub_proto[k] = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->ta_proto));
        r->ta_sub_ctor[k]  = ta_make_fn(vm, r, js_ta_sub_ctor, cname, 3, true, -(40 + k), r->ta_ctor);
        ajs_define_prop_str(vm, r->ta_sub_ctor[k], "prototype", r->ta_sub_proto[k], 0);
        ajs_define_prop_str(vm, r->ta_sub_ctor[k], "BYTES_PER_ELEMENT", bpe, 0);
        ajs_define_prop_str(vm, r->ta_sub_proto[k], "constructor", r->ta_sub_ctor[k], AJS_METHOD_FLAGS);
        ajs_define_prop_str(vm, r->ta_sub_proto[k], "BYTES_PER_ELEMENT", bpe, 0);
        ajs_define_prop_str(vm, r->global_obj, cname, r->ta_sub_ctor[k], AJS_METHOD_FLAGS);
    }

    /* ES2026 Uint8Array Base64 & Hex methods */
    ajs_def_native_method(vm, r, r->ta_sub_ctor[AJS_TA_UINT8], "fromBase64", js_u8_fromBase64, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ta_sub_ctor[AJS_TA_UINT8], "fromHex", js_u8_fromHex, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ta_sub_proto[AJS_TA_UINT8], "toBase64", js_u8_toBase64, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ta_sub_proto[AJS_TA_UINT8], "setFromBase64", js_u8_setFromBase64, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ta_sub_proto[AJS_TA_UINT8], "toHex", js_u8_toHex, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->ta_sub_proto[AJS_TA_UINT8], "setFromHex", js_u8_setFromHex, 1, AJS_METHOD_FLAGS, 0);

    /* 6. Atomics */
    AjsValue atomics = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    ajs_temp_root_push(vm, atomics);
    ajs_def_native_method(vm, r, atomics, "load", js_atomics_op, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, atomics, "store", js_atomics_op, 3, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, atomics, "add", js_atomics_op, 3, AJS_METHOD_FLAGS, 2);
    ajs_def_native_method(vm, r, atomics, "sub", js_atomics_op, 3, AJS_METHOD_FLAGS, 3);
    ajs_def_native_method(vm, r, atomics, "and", js_atomics_op, 3, AJS_METHOD_FLAGS, 4);
    ajs_def_native_method(vm, r, atomics, "or", js_atomics_op, 3, AJS_METHOD_FLAGS, 5);
    ajs_def_native_method(vm, r, atomics, "xor", js_atomics_op, 3, AJS_METHOD_FLAGS, 6);
    ajs_def_native_method(vm, r, atomics, "exchange", js_atomics_op, 3, AJS_METHOD_FLAGS, 7);
    ajs_def_native_method(vm, r, atomics, "compareExchange", js_atomics_op, 4, AJS_METHOD_FLAGS, 8);
    ajs_def_native_method(vm, r, atomics, "isLockFree", js_atomics_isLockFree, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, atomics, "wait", js_atomics_wait, 4, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, atomics, "notify", js_atomics_notify, 3, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, atomics, "pause", js_atomics_pause, 0, AJS_METHOD_FLAGS, 0);
    ta_def_tag(vm, atomics, "Atomics");
    ajs_define_prop_str(vm, r->global_obj, "Atomics", atomics, AJS_METHOD_FLAGS);

    ajs_temp_root_restore(vm, rs);
}
