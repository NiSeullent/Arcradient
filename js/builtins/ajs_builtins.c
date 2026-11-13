/*
 * ArcradientJS 1.0 (Clean-Room Pure C11 ECMAScript 2026 Virtual Machine)
 * js/builtins/ajs_builtins.c - Core Intrinsics: BigInt, Object, Function, Boolean,
 * Number, Math, Symbol, Date, Error (+Subclasses), Reflect, Proxy, Global Functions
 */
#include "../vm/ajs_internal.h"
#include <time.h>

#define AJS_METHOD_FLAGS (AJS_PROP_WRITABLE | AJS_PROP_CONFIGURABLE)

/* ============================================================================
 * 0. Intrinsic Construction & Descriptor Conversion Helpers
 * ============================================================================ */

static AjsValue get_proto_from_ctor(AjsVm *vm, AjsValue new_target, AjsValue fallback_proto) {
    if (!ajs_val_is_gc_kind(new_target, AJS_GC_OBJECT)) return fallback_proto;
    AjsValue p = ajs_obj_get(vm, new_target, ajs_val_from_gc(vm->str_prototype), new_target);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (ajs_val_is_gc_kind(p, AJS_GC_OBJECT)) return p;
    return fallback_proto;
}

static AjsValue make_builtin_fn(AjsVm *vm, AjsRealm *realm, AjsNativeFn fn, const char *name,
                                int length, bool is_ctor, int32_t magic, AjsValue fn_proto) {
    AjsObject *fobj = ajs_obj_new(vm, realm, AJS_OBJ_FUNCTION, fn_proto);
    if (!fobj) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue fv = ajs_val_from_gc(fobj);
    ajs_temp_root_push(vm, fv);

    fobj->u.fn.fn_kind = AJS_FN_NATIVE;
    fobj->u.fn.is_constructor = is_ctor;
    fobj->u.fn.strict = true;
    fobj->u.fn.native_fn = fn;
    fobj->u.fn.native_magic = magic;

    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
              AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    d.value = ajs_val_number((double)(length < 0 ? 0 : length));
    ajs_obj_define_own_prop(vm, fobj, ajs_val_from_gc(vm->str_length), &d, false);

    AjsString *ns = ajs_str_from_cstr(vm, name ? name : "", true);
    d.value = ajs_val_from_gc(ns);
    ajs_obj_define_own_prop(vm, fobj, ajs_val_from_gc(vm->str_name), &d, false);

    ajs_temp_root_restore(vm, rs);
    return fv;
}

static void link_ctor_proto(AjsVm *vm, AjsValue ctor, AjsValue proto, bool proto_writable) {
    if (!ajs_val_is_gc_kind(ctor, AJS_GC_OBJECT) || !ajs_val_is_gc_kind(proto, AJS_GC_OBJECT)) return;
    AjsPropDesc pd;
    memset(&pd, 0, sizeof(pd));
    pd.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
    if (proto_writable) pd.flags |= AJS_DESC_WRITABLE;
    pd.value = proto;
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(ctor), ajs_val_from_gc(vm->str_prototype), &pd, false);

    AjsPropDesc cd;
    memset(&cd, 0, sizeof(cd));
    cd.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
               AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    cd.value = ctor;
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(proto), ajs_val_from_gc(vm->str_constructor), &cd, false);
}

static void def_data_prop(AjsVm *vm, AjsValue obj, const char *name, AjsValue val, uint32_t desc_flags) {
    if (!ajs_val_is_gc_kind(obj, AJS_GC_OBJECT)) return;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, obj);
    ajs_temp_root_push(vm, val);
    AjsString *k = ajs_str_from_cstr(vm, name, true);
    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | desc_flags;
    d.value = val;
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(obj), ajs_val_from_gc(k), &d, false);
    ajs_temp_root_restore(vm, rs);
}

static void def_to_string_tag(AjsVm *vm, AjsValue obj, const char *tag_cstr) {
    if (!ajs_val_is_gc_kind(obj, AJS_GC_OBJECT)) return;
    AjsSymbol *sym = vm->well_known_syms[AJS_SYM_TO_STRING_TAG];
    if (!sym) return;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, obj);
    AjsString *ts = ajs_str_from_cstr(vm, tag_cstr, true);
    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
              AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    d.value = ajs_val_from_gc(ts);
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(obj), ajs_val_from_gc(sym), &d, false);
    ajs_temp_root_restore(vm, rs);
}

static AjsValue from_prop_desc(AjsVm *vm, AjsRealm *realm, const AjsPropDesc *desc) {
    AjsObject *o = ajs_obj_new(vm, realm, AJS_OBJ_ORDINARY, realm->object_proto);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue ov = ajs_val_from_gc(o);
    ajs_temp_root_push(vm, ov);
    uint32_t f = AJS_DESC_WRITABLE | AJS_DESC_ENUMERABLE | AJS_DESC_CONFIGURABLE;
    if (desc->flags & AJS_DESC_IS_ACCESSOR) {
        def_data_prop(vm, ov, "get", desc->getter, f);
        def_data_prop(vm, ov, "set", desc->setter, f);
    } else {
        def_data_prop(vm, ov, "value", desc->value, f);
        def_data_prop(vm, ov, "writable", ajs_bool((desc->flags & AJS_DESC_WRITABLE) != 0), f);
    }
    def_data_prop(vm, ov, "enumerable", ajs_bool((desc->flags & AJS_DESC_ENUMERABLE) != 0), f);
    def_data_prop(vm, ov, "configurable", ajs_bool((desc->flags & AJS_DESC_CONFIGURABLE) != 0), f);
    ajs_temp_root_restore(vm, rs);
    return ov;
}

static bool to_prop_desc(AjsVm *vm, AjsValue obj_val, AjsPropDesc *out_desc) {
    memset(out_desc, 0, sizeof(*out_desc));
    if (!ajs_val_is_gc_kind(obj_val, AJS_GC_OBJECT)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Property description must be an object");
        return false;
    }
    AjsObject *o = (AjsObject *)ajs_val_to_gc(obj_val);
    bool has = false;
    AjsString *k_enum = ajs_str_from_cstr(vm, "enumerable", true);
    if (!ajs_obj_has_prop(vm, o, ajs_val_from_gc(k_enum), &has)) return false;
    if (has) {
        AjsValue v = ajs_obj_get(vm, obj_val, ajs_val_from_gc(k_enum), obj_val);
        if (vm->has_exception) return false;
        out_desc->flags |= AJS_DESC_HAS_ENUMERABLE | (ajs_to_bool(vm, v) ? AJS_DESC_ENUMERABLE : 0u);
    }
    AjsString *k_conf = ajs_str_from_cstr(vm, "configurable", true);
    if (!ajs_obj_has_prop(vm, o, ajs_val_from_gc(k_conf), &has)) return false;
    if (has) {
        AjsValue v = ajs_obj_get(vm, obj_val, ajs_val_from_gc(k_conf), obj_val);
        if (vm->has_exception) return false;
        out_desc->flags |= AJS_DESC_HAS_CONFIGURABLE | (ajs_to_bool(vm, v) ? AJS_DESC_CONFIGURABLE : 0u);
    }
    AjsString *k_val = ajs_str_from_cstr(vm, "value", true);
    if (!ajs_obj_has_prop(vm, o, ajs_val_from_gc(k_val), &has)) return false;
    if (has) {
        AjsValue v = ajs_obj_get(vm, obj_val, ajs_val_from_gc(k_val), obj_val);
        if (vm->has_exception) return false;
        out_desc->flags |= AJS_DESC_HAS_VALUE;
        out_desc->value = v;
    }
    AjsString *k_wr = ajs_str_from_cstr(vm, "writable", true);
    if (!ajs_obj_has_prop(vm, o, ajs_val_from_gc(k_wr), &has)) return false;
    if (has) {
        AjsValue v = ajs_obj_get(vm, obj_val, ajs_val_from_gc(k_wr), obj_val);
        if (vm->has_exception) return false;
        out_desc->flags |= AJS_DESC_HAS_WRITABLE | (ajs_to_bool(vm, v) ? AJS_DESC_WRITABLE : 0u);
    }
    AjsString *k_get = ajs_str_from_cstr(vm, "get", true);
    if (!ajs_obj_has_prop(vm, o, ajs_val_from_gc(k_get), &has)) return false;
    if (has) {
        AjsValue v = ajs_obj_get(vm, obj_val, ajs_val_from_gc(k_get), obj_val);
        if (vm->has_exception) return false;
        if (v != AJS_VAL_UNDEFINED && !ajs_is_callable(vm, v)) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Getter must be a function");
            return false;
        }
        out_desc->flags |= AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_GET;
        out_desc->getter = v;
    }
    AjsString *k_set = ajs_str_from_cstr(vm, "set", true);
    if (!ajs_obj_has_prop(vm, o, ajs_val_from_gc(k_set), &has)) return false;
    if (has) {
        AjsValue v = ajs_obj_get(vm, obj_val, ajs_val_from_gc(k_set), obj_val);
        if (vm->has_exception) return false;
        if (v != AJS_VAL_UNDEFINED && !ajs_is_callable(vm, v)) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Setter must be a function");
            return false;
        }
        out_desc->flags |= AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_SET;
        out_desc->setter = v;
    }
    if ((out_desc->flags & AJS_DESC_IS_ACCESSOR) &&
        (out_desc->flags & (AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE))) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                           "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
        return false;
    }
    return true;
}

/* ============================================================================
 * 1. Arbitrary-Precision BigInt Engine (Little-Endian 32-bit Limbs)
 * ============================================================================ */

static AjsBigInt *bi_alloc_limbs(AjsVm *vm, uint32_t cap, int32_t sign) {
    if (cap > (1u << 20)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Maximum BigInt size exceeded");
        return NULL;
    }
    size_t sz = sizeof(AjsBigInt) + (size_t)cap * sizeof(uint32_t);
    AjsBigInt *bi = (AjsBigInt *)ajs_gc_alloc(vm, sz, AJS_GC_BIGINT);
    if (!bi) return NULL;
    bi->sign = sign;
    bi->len = cap;
    return bi;
}

static void bi_normalize(AjsBigInt *bi) {
    if (!bi) return;
    while (bi->len > 0 && bi->limbs[bi->len - 1u] == 0) bi->len--;
    if (bi->len == 0) bi->sign = 0;
}

AjsBigInt *ajs_bigint_new_u64(AjsVm *vm, uint64_t u, int sign) {
    if (u == 0 || sign == 0) {
        return bi_alloc_limbs(vm, 0, 0);
    }
    uint32_t n = (u > UINT32_MAX) ? 2u : 1u;
    AjsBigInt *bi = bi_alloc_limbs(vm, n, sign < 0 ? -1 : 1);
    if (!bi) return NULL;
    bi->limbs[0] = (uint32_t)(u & 0xFFFFFFFFu);
    if (n == 2u) bi->limbs[1] = (uint32_t)(u >> 32);
    bi_normalize(bi);
    return bi;
}

AjsBigInt *ajs_bigint_new_i64(AjsVm *vm, int64_t v) {
    if (v == 0) return bi_alloc_limbs(vm, 0, 0);
    if (v < 0) {
        uint64_t mag = (v == INT64_MIN) ? (UINT64_C(1) << 63) : (uint64_t)(-v);
        return ajs_bigint_new_u64(vm, mag, -1);
    }
    return ajs_bigint_new_u64(vm, (uint64_t)v, 1);
}

static int bi_cmp_mag(const uint32_t *a, uint32_t alen, const uint32_t *b, uint32_t blen) {
    while (alen > 0 && a[alen - 1u] == 0) alen--;
    while (blen > 0 && b[blen - 1u] == 0) blen--;
    if (alen != blen) return alen < blen ? -1 : 1;
    for (uint32_t i = alen; i > 0; i--) {
        if (a[i - 1u] != b[i - 1u]) return a[i - 1u] < b[i - 1u] ? -1 : 1;
    }
    return 0;
}

int ajs_bigint_cmp(const AjsBigInt *a, const AjsBigInt *b) {
    if (a->sign != b->sign) return a->sign < b->sign ? -1 : 1;
    if (a->sign == 0) return 0;
    int m = bi_cmp_mag(a->limbs, a->len, b->limbs, b->len);
    return a->sign > 0 ? m : -m;
}

bool ajs_bigint_eq(const AjsBigInt *a, const AjsBigInt *b) {
    return ajs_bigint_cmp(a, b) == 0;
}

int ajs_bigint_cmp_double(const AjsBigInt *a, double b) {
    if (isnan(b)) return -2;
    if (isinf(b)) return b > 0.0 ? -1 : 1;
    if (b == 0.0) return a->sign < 0 ? -1 : (a->sign > 0 ? 1 : 0);
    int bsign = b < 0.0 ? -1 : 1;
    if (a->sign != bsign) return a->sign < bsign ? -1 : 1;
    double ab = fabs(b);
    if (ab < 1.0) {
        /* |a| >= 1 since a->sign != 0, so |a| > |b| */
        return a->sign > 0 ? 1 : -1;
    }
    int exp2 = 0;
    double mant = frexp(ab, &exp2); /* ab = mant * 2^exp2, 0.5 <= mant < 1.0 */
    /* Highest set bit of floor(ab) is at bit index (exp2 - 1) */
    uint32_t a_bits = (a->len - 1u) * 32u;
    uint32_t top = a->limbs[a->len - 1u];
    while (top > 0) { a_bits++; top >>= 1; }
    if ((int)a_bits != exp2) {
        int m = ((int)a_bits < exp2) ? -1 : 1;
        return a->sign > 0 ? m : -m;
    }
    /* Same bit length! Extract 53 mantissa bits of ab */
    uint64_t m53 = (uint64_t)ldexp(mant, 53); /* Exact 53-bit integer */
    /* Compare bits from MSB (exp2 - 1) down to 0 */
    for (int bit = exp2 - 1; bit >= 0; bit--) {
        uint32_t abit = (a->limbs[(uint32_t)bit / 32u] >> ((uint32_t)bit % 32u)) & 1u;
        int m_idx = 52 - (exp2 - 1 - bit);
        uint32_t bbit = (m_idx >= 0) ? (uint32_t)((m53 >> m_idx) & 1u) : 0u;
        if (abit != bbit) {
            int m = abit < bbit ? -1 : 1;
            return a->sign > 0 ? m : -m;
        }
    }
    /* Integer part matches floor(ab); check if ab has a fractional part */
    if (floor(ab) != ab) {
        /* |a| == floor(|b|) < |b| */
        return a->sign > 0 ? -1 : 1;
    }
    return 0;
}

AjsBigInt *ajs_bigint_from_cstr(AjsVm *vm, const char *str, size_t len, bool allow_radix_prefix, bool *out_ok) {
    if (out_ok) *out_ok = false;
    size_t i = 0;
    while (i < len) {
        size_t adv = 1;
        uint32_t cp = ajs_utf8_decode_one(str + i, len - i, &adv);
        if (!ajs_unicode_is_whitespace(cp)) break;
        i += adv;
    }
    while (len > i) {
        unsigned char c = (unsigned char)str[len - 1u];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') { len--; continue; }
        /* Check trailing Unicode whitespace */
        size_t p = i, last_start = i;
        uint32_t last_cp = 0;
        while (p < len) {
            last_start = p;
            size_t adv = 1;
            last_cp = ajs_utf8_decode_one(str + p, len - p, &adv);
            p += adv;
        }
        if (ajs_unicode_is_whitespace(last_cp)) { len = last_start; continue; }
        break;
    }
    if (i == len) {
        if (out_ok) *out_ok = true;
        return bi_alloc_limbs(vm, 0, 0);
    }

    int sign = 1;
    int radix = 10;
    if (str[i] == '+' || str[i] == '-') {
        if (allow_radix_prefix && i + 2u < len && str[i + 1u] == '0' &&
            (str[i + 2u] == 'x' || str[i + 2u] == 'X' || str[i + 2u] == 'b' ||
             str[i + 2u] == 'B' || str[i + 2u] == 'o' || str[i + 2u] == 'O')) {
            return NULL; /* Sign + radix prefix is invalid in StringToBigInt */
        }
        if (str[i] == '-') sign = -1;
        i++;
        if (i == len) return NULL;
    }
    if (allow_radix_prefix && i + 1u < len && str[i] == '0') {
        char p = str[i + 1u];
        if (p == 'x' || p == 'X') { radix = 16; i += 2u; }
        else if (p == 'b' || p == 'B') { radix = 2; i += 2u; }
        else if (p == 'o' || p == 'O') { radix = 8; i += 2u; }
        if (radix != 10 && i == len) return NULL;
    }

    size_t ndigits = len - i;
    uint32_t cap = (uint32_t)((ndigits * 4u) / 32u + 2u);
    uint32_t *limbs = (uint32_t *)calloc(cap, sizeof(uint32_t));
    if (!limbs) return NULL;
    uint32_t used = 0;

    for (; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c == '_' && !allow_radix_prefix) { /* Numeric separator handled in lexer before calling cstr */ }
        int d = -1;
        if (c >= '0' && c <= '9') d = (int)(c - '0');
        else if (c >= 'a' && c <= 'f') d = 10 + (int)(c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (int)(c - 'A');
        if (d < 0 || d >= radix) {
            free(limbs);
            return NULL;
        }
        uint64_t carry = (uint64_t)d;
        for (uint32_t j = 0; j < used; j++) {
            uint64_t cur = (uint64_t)limbs[j] * (uint64_t)radix + carry;
            limbs[j] = (uint32_t)(cur & 0xFFFFFFFFu);
            carry = cur >> 32;
        }
        if (carry > 0) limbs[used++] = (uint32_t)carry;
    }

    AjsBigInt *bi = bi_alloc_limbs(vm, used, used > 0 ? sign : 0);
    if (bi && used > 0) memcpy(bi->limbs, limbs, (size_t)used * sizeof(uint32_t));
    free(limbs);
    if (!bi) return NULL;
    bi_normalize(bi);
    if (out_ok) *out_ok = true;
    return bi;
}

AjsString *ajs_bigint_to_str(AjsVm *vm, const AjsBigInt *bi, int radix) {
    if (radix < 2 || radix > 36) radix = 10;
    if (!bi || bi->sign == 0 || bi->len == 0) {
        return ajs_str_from_cstr(vm, "0", true);
    }
    uint32_t n = bi->len;
    uint32_t *tmp = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    if (!tmp) { ajs_throw(vm, vm->oom_error); return NULL; }
    memcpy(tmp, bi->limbs, (size_t)n * sizeof(uint32_t));

    size_t max_chars = (size_t)n * 33u + 3u;
    char *buf = (char *)malloc(max_chars);
    if (!buf) { free(tmp); ajs_throw(vm, vm->oom_error); return NULL; }

    size_t pos = 0;
    static const char k_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    while (n > 0) {
        uint64_t rem = 0;
        for (uint32_t i = n; i > 0; i--) {
            uint64_t cur = (rem << 32) | (uint64_t)tmp[i - 1u];
            tmp[i - 1u] = (uint32_t)(cur / (uint64_t)radix);
            rem = cur % (uint64_t)radix;
        }
        buf[pos++] = k_digits[rem];
        while (n > 0 && tmp[n - 1u] == 0) n--;
    }
    if (bi->sign < 0) buf[pos++] = '-';
    for (size_t l = 0, r = pos - 1u; l < r; l++, r--) {
        char c = buf[l]; buf[l] = buf[r]; buf[r] = c;
    }
    AjsString *s = ajs_str_from_utf8(vm, buf, pos, false);
    free(buf);
    free(tmp);
    return s;
}

uint64_t ajs_bigint_to_u64_wrap(const AjsBigInt *bi) {
    if (!bi || bi->sign == 0 || bi->len == 0) return 0;
    uint64_t mag = (uint64_t)bi->limbs[0];
    if (bi->len >= 2u) mag |= ((uint64_t)bi->limbs[1] << 32);
    return bi->sign < 0 ? (0u - mag) : mag;
}

int64_t ajs_bigint_to_i64_wrap(const AjsBigInt *bi) {
    uint64_t u = ajs_bigint_to_u64_wrap(bi);
    int64_t s;
    memcpy(&s, &u, sizeof(s));
    return s;
}

AjsBigInt *ajs_bigint_neg(AjsVm *vm, const AjsBigInt *a) {
    if (!a || a->sign == 0) return bi_alloc_limbs(vm, 0, 0);
    AjsBigInt *r = bi_alloc_limbs(vm, a->len, -a->sign);
    if (!r) return NULL;
    memcpy(r->limbs, a->limbs, (size_t)a->len * sizeof(uint32_t));
    return r;
}

static AjsBigInt *bi_add_mag(AjsVm *vm, const uint32_t *a, uint32_t alen,
                             const uint32_t *b, uint32_t blen, int32_t sign) {
    uint32_t m = alen > blen ? alen : blen;
    AjsBigInt *r = bi_alloc_limbs(vm, m + 1u, sign);
    if (!r) return NULL;
    uint64_t carry = 0;
    for (uint32_t i = 0; i < m; i++) {
        uint64_t sum = carry + (i < alen ? a[i] : 0u) + (i < blen ? b[i] : 0u);
        r->limbs[i] = (uint32_t)(sum & 0xFFFFFFFFu);
        carry = sum >> 32;
    }
    r->limbs[m] = (uint32_t)carry;
    bi_normalize(r);
    return r;
}

static AjsBigInt *bi_sub_mag(AjsVm *vm, const uint32_t *a, uint32_t alen,
                             const uint32_t *b, uint32_t blen, int32_t sign) {
    /* Assumes |a| >= |b| */
    AjsBigInt *r = bi_alloc_limbs(vm, alen, sign);
    if (!r) return NULL;
    int64_t borrow = 0;
    for (uint32_t i = 0; i < alen; i++) {
        int64_t diff = (int64_t)a[i] - (int64_t)(i < blen ? b[i] : 0u) - borrow;
        if (diff < 0) { diff += (INT64_C(1) << 32); borrow = 1; }
        else borrow = 0;
        r->limbs[i] = (uint32_t)diff;
    }
    bi_normalize(r);
    return r;
}

AjsBigInt *ajs_bigint_add(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b) {
    if (a->sign == 0) return bi_add_mag(vm, b->limbs, b->len, NULL, 0, b->sign);
    if (b->sign == 0) return bi_add_mag(vm, a->limbs, a->len, NULL, 0, a->sign);
    if (a->sign == b->sign) return bi_add_mag(vm, a->limbs, a->len, b->limbs, b->len, a->sign);
    int cmp = bi_cmp_mag(a->limbs, a->len, b->limbs, b->len);
    if (cmp == 0) return bi_alloc_limbs(vm, 0, 0);
    if (cmp > 0) return bi_sub_mag(vm, a->limbs, a->len, b->limbs, b->len, a->sign);
    return bi_sub_mag(vm, b->limbs, b->len, a->limbs, a->len, b->sign);
}

AjsBigInt *ajs_bigint_sub(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b) {
    if (b->sign == 0) return bi_add_mag(vm, a->limbs, a->len, NULL, 0, a->sign);
    if (a->sign == 0) return bi_add_mag(vm, b->limbs, b->len, NULL, 0, -b->sign);
    if (a->sign != b->sign) return bi_add_mag(vm, a->limbs, a->len, b->limbs, b->len, a->sign);
    int cmp = bi_cmp_mag(a->limbs, a->len, b->limbs, b->len);
    if (cmp == 0) return bi_alloc_limbs(vm, 0, 0);
    if (cmp > 0) return bi_sub_mag(vm, a->limbs, a->len, b->limbs, b->len, a->sign);
    return bi_sub_mag(vm, b->limbs, b->len, a->limbs, a->len, -a->sign);
}

AjsBigInt *ajs_bigint_not(AjsVm *vm, const AjsBigInt *a) {
    /* ~x == -(x + 1) */
    uint32_t one = 1u;
    if (a->sign == 0) return ajs_bigint_new_i64(vm, -1);
    if (a->sign > 0) return bi_add_mag(vm, a->limbs, a->len, &one, 1u, -1);
    return bi_sub_mag(vm, a->limbs, a->len, &one, 1u, 1);
}

AjsBigInt *ajs_bigint_mul(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b) {
    if (a->sign == 0 || b->sign == 0) return bi_alloc_limbs(vm, 0, 0);
    AjsBigInt *r = bi_alloc_limbs(vm, a->len + b->len, a->sign * b->sign);
    if (!r) return NULL;
    for (uint32_t i = 0; i < a->len; i++) {
        uint64_t carry = 0;
        uint64_t ai = a->limbs[i];
        for (uint32_t j = 0; j < b->len; j++) {
            uint64_t cur = (uint64_t)r->limbs[i + j] + ai * (uint64_t)b->limbs[j] + carry;
            r->limbs[i + j] = (uint32_t)(cur & 0xFFFFFFFFu);
            carry = cur >> 32;
        }
        r->limbs[i + b->len] += (uint32_t)carry;
    }
    bi_normalize(r);
    return r;
}

static bool bi_divmod_mag(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b,
                          AjsBigInt **out_q, AjsBigInt **out_r) {
    if (b->sign == 0 || b->len == 0) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Division by zero");
        return false;
    }
    if (a->sign == 0 || bi_cmp_mag(a->limbs, a->len, b->limbs, b->len) < 0) {
        if (out_q) *out_q = bi_alloc_limbs(vm, 0, 0);
        if (out_r) {
            *out_r = bi_add_mag(vm, a->limbs, a->len, NULL, 0, a->sign);
        }
        return (!out_q || *out_q) && (!out_r || *out_r);
    }
    /* Bit-by-bit shift-subtract division (fast and exact for arbitrary limb sizes) */
    uint32_t qlen = a->len;
    uint32_t rcap = b->len + 1u;
    uint32_t *qlimbs = (uint32_t *)calloc(qlen, sizeof(uint32_t));
    uint32_t *rlimbs = (uint32_t *)calloc(rcap, sizeof(uint32_t));
    if (!qlimbs || !rlimbs) { free(qlimbs); free(rlimbs); ajs_throw(vm, vm->oom_error); return false; }

    uint32_t total_bits = a->len * 32u;
    while (total_bits > 0 && ((a->limbs[(total_bits - 1u) / 32u] >> ((total_bits - 1u) % 32u)) & 1u) == 0) {
        total_bits--;
    }
    uint32_t rused = 0;
    for (uint32_t bit = total_bits; bit > 0; bit--) {
        uint32_t b_idx = bit - 1u;
        uint32_t abit = (a->limbs[b_idx / 32u] >> (b_idx % 32u)) & 1u;
        /* r = (r << 1) | abit */
        uint32_t carry = abit;
        for (uint32_t i = 0; i < rused; i++) {
            uint32_t next_c = rlimbs[i] >> 31;
            rlimbs[i] = (rlimbs[i] << 1) | carry;
            carry = next_c;
        }
        if (carry && rused < rcap) rlimbs[rused++] = carry;
        if (bi_cmp_mag(rlimbs, rused, b->limbs, b->len) >= 0) {
            int64_t borrow = 0;
            for (uint32_t i = 0; i < rused; i++) {
                int64_t d = (int64_t)rlimbs[i] - (int64_t)(i < b->len ? b->limbs[i] : 0u) - borrow;
                if (d < 0) { d += (INT64_C(1) << 32); borrow = 1; } else borrow = 0;
                rlimbs[i] = (uint32_t)d;
            }
            while (rused > 0 && rlimbs[rused - 1u] == 0) rused--;
            qlimbs[b_idx / 32u] |= (1u << (b_idx % 32u));
        }
    }

    bool ok = true;
    if (out_q) {
        *out_q = bi_alloc_limbs(vm, qlen, a->sign * b->sign);
        if (*out_q) { memcpy((*out_q)->limbs, qlimbs, (size_t)qlen * sizeof(uint32_t)); bi_normalize(*out_q); }
        else ok = false;
    }
    if (out_r && ok) {
        *out_r = bi_alloc_limbs(vm, rused, rused > 0 ? a->sign : 0);
        if (*out_r) { ajs_memcpy((*out_r)->limbs, rlimbs, (size_t)rused * sizeof(uint32_t)); bi_normalize(*out_r); }
        else ok = false;
    }
    free(qlimbs);
    free(rlimbs);
    return ok;
}

AjsBigInt *ajs_bigint_div(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b) {
    AjsBigInt *q = NULL;
    return bi_divmod_mag(vm, a, b, &q, NULL) ? q : NULL;
}

AjsBigInt *ajs_bigint_mod(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b) {
    AjsBigInt *r = NULL;
    return bi_divmod_mag(vm, a, b, NULL, &r) ? r : NULL;
}

AjsBigInt *ajs_bigint_exp(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b) {
    if (b->sign < 0) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Exponent must be positive");
        return NULL;
    }
    if (b->sign == 0) return ajs_bigint_new_i64(vm, 1);
    if (a->sign == 0) return bi_alloc_limbs(vm, 0, 0);
    if (a->len == 1 && a->limbs[0] == 1) {
        int s = (a->sign < 0 && (b->limbs[0] & 1u)) ? -1 : 1;
        return ajs_bigint_new_i64(vm, s);
    }
    if (b->len > 1 || b->limbs[0] > (1u << 22)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Maximum BigInt size exceeded");
        return NULL;
    }
    uint32_t exp = b->limbs[0];
    size_t rs = ajs_temp_root_save(vm);
    AjsBigInt *res = ajs_bigint_new_i64(vm, 1);
    AjsBigInt *base = bi_add_mag(vm, a->limbs, a->len, NULL, 0, a->sign);
    while (exp > 0 && res && base) {
        if (exp & 1u) {
            ajs_temp_root_restore(vm, rs);
            ajs_temp_root_push(vm, ajs_val_from_gc(res));
            ajs_temp_root_push(vm, ajs_val_from_gc(base));
            res = ajs_bigint_mul(vm, res, base);
            if (!res) break;
        }
        exp >>= 1;
        if (exp > 0) {
            ajs_temp_root_restore(vm, rs);
            ajs_temp_root_push(vm, ajs_val_from_gc(res));
            ajs_temp_root_push(vm, ajs_val_from_gc(base));
            base = ajs_bigint_mul(vm, base, base);
            if (!base) { res = NULL; break; }
        }
    }
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsBigInt *bi_shl_bits(AjsVm *vm, const AjsBigInt *a, uint64_t bits) {
    if (a->sign == 0 || bits == 0) return bi_add_mag(vm, a->limbs, a->len, NULL, 0, a->sign);
    if (bits > (1u << 24)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Maximum BigInt size exceeded");
        return NULL;
    }
    uint32_t wshift = (uint32_t)(bits / 32u);
    uint32_t bshift = (uint32_t)(bits % 32u);
    AjsBigInt *r = bi_alloc_limbs(vm, a->len + wshift + 1u, a->sign);
    if (!r) return NULL;
    uint32_t carry = 0;
    for (uint32_t i = 0; i < a->len; i++) {
        uint64_t v = ((uint64_t)a->limbs[i] << bshift) | carry;
        r->limbs[i + wshift] = (uint32_t)(v & 0xFFFFFFFFu);
        carry = (uint32_t)(v >> 32);
    }
    r->limbs[a->len + wshift] = carry;
    bi_normalize(r);
    return r;
}

static AjsBigInt *bi_shr_bits(AjsVm *vm, const AjsBigInt *a, uint64_t bits) {
    /* Arithmetic right shift (floor toward -Infinity!) */
    if (a->sign == 0 || bits == 0) return bi_add_mag(vm, a->limbs, a->len, NULL, 0, a->sign);
    uint64_t wshift64 = bits / 32u;
    if (wshift64 >= a->len) {
        return a->sign < 0 ? ajs_bigint_new_i64(vm, -1) : bi_alloc_limbs(vm, 0, 0);
    }
    uint32_t wshift = (uint32_t)wshift64;
    uint32_t bshift = (uint32_t)(bits % 32u);
    uint32_t nlen = a->len - wshift;
    bool dropped_nonzero = false;
    if (a->sign < 0) {
        for (uint32_t i = 0; i < wshift; i++) if (a->limbs[i] != 0) dropped_nonzero = true;
        if (bshift > 0 && (a->limbs[wshift] & ((1u << bshift) - 1u)) != 0) dropped_nonzero = true;
    }
    AjsBigInt *r = bi_alloc_limbs(vm, nlen, a->sign);
    if (!r) return NULL;
    for (uint32_t i = 0; i < nlen; i++) {
        uint32_t lo = a->limbs[i + wshift] >> bshift;
        uint32_t hi = (bshift > 0 && i + wshift + 1u < a->len) ? (a->limbs[i + wshift + 1u] << (32u - bshift)) : 0u;
        r->limbs[i] = lo | hi;
    }
    bi_normalize(r);
    if (dropped_nonzero) {
        size_t rs = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, ajs_val_from_gc(r));
        uint32_t one = 1u;
        AjsBigInt *adj = (r->sign == 0) ? ajs_bigint_new_i64(vm, -1) : bi_add_mag(vm, r->limbs, r->len, &one, 1u, -1);
        ajs_temp_root_restore(vm, rs);
        return adj;
    }
    return r;
}

AjsBigInt *ajs_bigint_shl(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b) {
    if (b->sign == 0) return bi_add_mag(vm, a->limbs, a->len, NULL, 0, a->sign);
    uint64_t bits = (b->len > 2u) ? (UINT64_C(1) << 30) : (b->limbs[0] | (b->len == 2u ? ((uint64_t)b->limbs[1] << 32) : 0u));
    return b->sign > 0 ? bi_shl_bits(vm, a, bits) : bi_shr_bits(vm, a, bits);
}

AjsBigInt *ajs_bigint_shr(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b) {
    if (b->sign == 0) return bi_add_mag(vm, a->limbs, a->len, NULL, 0, a->sign);
    uint64_t bits = (b->len > 2u) ? (UINT64_C(1) << 30) : (b->limbs[0] | (b->len == 2u ? ((uint64_t)b->limbs[1] << 32) : 0u));
    return b->sign > 0 ? bi_shr_bits(vm, a, bits) : bi_shl_bits(vm, a, bits);
}

static void bi_to_twos_comp(const AjsBigInt *a, uint32_t *out, uint32_t n) {
    if (a->sign >= 0) {
        for (uint32_t i = 0; i < n; i++) out[i] = (i < a->len) ? a->limbs[i] : 0u;
    } else {
        uint64_t carry = 1u;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t inv = ~((i < a->len) ? a->limbs[i] : 0u);
            uint64_t s = (uint64_t)inv + carry;
            out[i] = (uint32_t)(s & 0xFFFFFFFFu);
            carry = s >> 32;
        }
    }
}

AjsBigInt *ajs_bigint_bitop(AjsVm *vm, const AjsBigInt *a, const AjsBigInt *b, char op) {
    uint32_t max_len = (a->len > b->len) ? a->len : b->len;
    uint32_t n = (max_len < UINT32_MAX - 2u) ? (max_len + 2u) : 2u;
    uint32_t *ta = (uint32_t *)calloc((size_t)n, sizeof(uint32_t));
    uint32_t *tb = (uint32_t *)calloc((size_t)n, sizeof(uint32_t));
    if (!ta || !tb) { free(ta); free(tb); ajs_throw(vm, vm->oom_error); return NULL; }
    bi_to_twos_comp(a, ta, n);
    bi_to_twos_comp(b, tb, n);
    for (uint32_t i = 0; i < n; i++) {
        if (op == '&') ta[i] &= tb[i];
        else if (op == '|') ta[i] |= tb[i];
        else ta[i] ^= tb[i];
    }
    int32_t sign = (ta[n - 1u] & 0x80000000u) ? -1 : 1;
    if (sign < 0) {
        uint64_t carry = 1u;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t s = (uint64_t)(~ta[i]) + carry;
            ta[i] = (uint32_t)(s & 0xFFFFFFFFu);
            carry = s >> 32;
        }
    }
    AjsBigInt *r = bi_alloc_limbs(vm, n, sign);
    if (r) { memcpy(r->limbs, ta, (size_t)n * sizeof(uint32_t)); bi_normalize(r); }
    free(ta);
    free(tb);
    return r;
}

static bool this_bigint_value(AjsVm *vm, AjsValue this_val, const AjsBigInt **out_bi) {
    if (ajs_val_is_gc_kind(this_val, AJS_GC_BIGINT)) {
        *out_bi = (const AjsBigInt *)ajs_val_to_gc(this_val);
        return true;
    }
    if (ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        AjsObject *o = (AjsObject *)ajs_val_to_gc(this_val);
        if (o->kind == AJS_OBJ_BIGINT_OBJ && ajs_val_is_gc_kind(o->u.prim_val, AJS_GC_BIGINT)) {
            *out_bi = (const AjsBigInt *)ajs_val_to_gc(o->u.prim_val);
            return true;
        }
    }
    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "BigInt.prototype method called on incompatible receiver");
    return false;
}

static AjsBigInt *val_to_bigint(AjsVm *vm, AjsValue v) {
    AjsValue prim = ajs_to_primitive(vm, v, 2);
    if (vm->has_exception) return NULL;
    if (ajs_val_is_gc_kind(prim, AJS_GC_BIGINT)) return (AjsBigInt *)ajs_val_to_gc(prim);
    if (prim == AJS_VAL_TRUE) return ajs_bigint_new_i64(vm, 1);
    if (prim == AJS_VAL_FALSE) return ajs_bigint_new_i64(vm, 0);
    if (ajs_val_is_gc_kind(prim, AJS_GC_STRING)) {
        const AjsString *s = (const AjsString *)ajs_val_to_gc(prim);
        bool ok = false;
        AjsBigInt *bi = ajs_bigint_from_cstr(vm, s->utf8, s->len8, true, &ok);
        if (!ok || !bi) {
            if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Cannot convert %s to a BigInt", s->utf8);
            return NULL;
        }
        return bi;
    }
    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot convert value to a BigInt");
    return NULL;
}

static AjsValue js_bigint_call(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (ajs_get_native_new_target(vm) != AJS_VAL_UNDEFINED) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "BigInt is not a constructor");
    }
    AjsValue arg = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue prim = ajs_to_primitive(vm, arg, 2);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (ajs_val_is_number(prim)) {
        double d = ajs_val_to_number_unchecked(prim);
        if (!isfinite(d) || floor(d) != d) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "The number cannot be converted to a BigInt because it is not an integer");
        }
        if (d == 0.0) return ajs_val_from_gc(ajs_bigint_new_i64(vm, 0));
        int sign = d < 0.0 ? -1 : 1;
        double ad = fabs(d);
        int exp2 = 0;
        double mant = frexp(ad, &exp2);
        uint64_t m53 = (uint64_t)ldexp(mant, 53);
        size_t rs = ajs_temp_root_save(vm);
        AjsBigInt *bi = ajs_bigint_new_u64(vm, m53, sign);
        ajs_temp_root_push(vm, ajs_val_from_gc(bi));
        if (exp2 > 53) bi = bi_shl_bits(vm, bi, (uint64_t)(exp2 - 53));
        else if (exp2 < 53) bi = bi_shr_bits(vm, bi, (uint64_t)(53 - exp2));
        ajs_temp_root_restore(vm, rs);
        return bi ? ajs_val_from_gc(bi) : AJS_VAL_EXCEPTION;
    }
    AjsBigInt *bi = val_to_bigint(vm, prim);
    return bi ? ajs_val_from_gc(bi) : AJS_VAL_EXCEPTION;
}

static AjsValue bigint_as_n(AjsVm *vm, int argc, const AjsValue *argv, bool is_signed) {
    size_t bits = 0;
    if (!ajs_to_index(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &bits)) return AJS_VAL_EXCEPTION;
    AjsBigInt *bi = val_to_bigint(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED);
    if (!bi) return AJS_VAL_EXCEPTION;
    if (bits == 0) return ajs_val_from_gc(ajs_bigint_new_i64(vm, 0));
    if (bits > (1u << 22)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Maximum BigInt size exceeded");
    }
    uint32_t nlimbs = (uint32_t)((bits + 31u) / 32u);
    uint32_t *tc = (uint32_t *)malloc((size_t)nlimbs * sizeof(uint32_t));
    if (!tc) return ajs_throw(vm, vm->oom_error);
    bi_to_twos_comp(bi, tc, nlimbs);
    uint32_t rem_bits = (uint32_t)(bits % 32u);
    if (rem_bits != 0) tc[nlimbs - 1u] &= (1u << rem_bits) - 1u;

    int32_t sign = 1;
    if (is_signed) {
        uint32_t msb_limb = (uint32_t)((bits - 1u) / 32u);
        uint32_t msb_mask = 1u << ((bits - 1u) % 32u);
        if (tc[msb_limb] & msb_mask) {
            sign = -1;
            if (rem_bits != 0) tc[nlimbs - 1u] |= ~((1u << rem_bits) - 1u);
            uint64_t carry = 1u;
            for (uint32_t i = 0; i < nlimbs; i++) {
                uint64_t s = (uint64_t)(~tc[i]) + carry;
                tc[i] = (uint32_t)(s & 0xFFFFFFFFu);
                carry = s >> 32;
            }
        }
    }
    AjsBigInt *r = bi_alloc_limbs(vm, nlimbs, sign);
    if (r) { memcpy(r->limbs, tc, (size_t)nlimbs * sizeof(uint32_t)); bi_normalize(r); }
    free(tc);
    return r ? ajs_val_from_gc(r) : AJS_VAL_EXCEPTION;
}

static AjsValue js_bigint_asIntN(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    return bigint_as_n(vm, argc, argv, true);
}

static AjsValue js_bigint_asUintN(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    return bigint_as_n(vm, argc, argv, false);
}

static AjsValue js_bigint_proto_toString(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    const AjsBigInt *bi = NULL;
    if (!this_bigint_value(vm, this_val, &bi)) return AJS_VAL_EXCEPTION;
    int radix = 10;
    if (argc > 0 && argv[0] != AJS_VAL_UNDEFINED) {
        double d = 0;
        if (!ajs_to_number(vm, argv[0], &d)) return AJS_VAL_EXCEPTION;
        double rd = ajs_double_to_integer_or_inf(d);
        if (rd < 2.0 || rd > 36.0) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "toString() radix argument must be between 2 and 36");
        }
        radix = (int)rd;
    }
    AjsString *s = ajs_bigint_to_str(vm, bi, radix);
    return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
}

static AjsValue js_bigint_proto_valueOf(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    const AjsBigInt *bi = NULL;
    if (!this_bigint_value(vm, this_val, &bi)) return AJS_VAL_EXCEPTION;
    return ajs_val_from_gc(bi);
}

/* ============================================================================
 * 2. Object Built-ins (Static & Prototype + Annex B)
 * ============================================================================ */

static AjsValue js_object_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsRealm *r = vm->active_realm;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt != AJS_VAL_UNDEFINED && nt != r->object_ctor) {
        AjsValue proto = get_proto_from_ctor(vm, nt, r->object_proto);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        AjsObject *o = ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, proto);
        return o ? ajs_val_from_gc(o) : AJS_VAL_EXCEPTION;
    }
    AjsValue arg = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (arg == AJS_VAL_UNDEFINED || arg == AJS_VAL_NULL) {
        AjsObject *o = ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto);
        return o ? ajs_val_from_gc(o) : AJS_VAL_EXCEPTION;
    }
    AjsObject *box = ajs_to_object(vm, arg);
    return box ? ajs_val_from_gc(box) : AJS_VAL_EXCEPTION;
}

static AjsValue js_object_assign(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsObject *to = ajs_to_object(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!to) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue to_val = ajs_val_from_gc(to);
    ajs_temp_root_push(vm, to_val);
    for (int i = 1; i < argc; i++) {
        if (ajs_is_nullish(argv[i])) continue;
        AjsObject *from = ajs_to_object(vm, argv[i]);
        if (!from) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        AjsValue from_val = ajs_val_from_gc(from);
        ajs_temp_root_push(vm, from_val);
        AjsValue *keys = NULL;
        uint32_t kcount = 0;
        if (!ajs_obj_own_keys(vm, from, false, true, true, &keys, &kcount)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
        for (uint32_t k = 0; k < kcount; k++) {
            ajs_temp_root_push(vm, keys[k]);
        }
        for (uint32_t k = 0; k < kcount; k++) {
            AjsPropDesc desc;
            bool found = false;
            if (!ajs_obj_get_own_desc(vm, from, keys[k], &desc, &found)) {
                free(keys); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
            }
            if (found && (desc.flags & AJS_DESC_ENUMERABLE)) {
                AjsValue pv = ajs_obj_get(vm, from_val, keys[k], from_val);
                if (vm->has_exception) { free(keys); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                size_t inner = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, pv);
                bool ok = ajs_obj_set(vm, to_val, keys[k], pv, to_val, true);
                ajs_temp_root_restore(vm, inner);
                if (!ok || vm->has_exception) { free(keys); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            }
        }
        free(keys);
    }
    ajs_temp_root_restore(vm, rs);
    return to_val;
}

static bool define_props_from_obj(AjsVm *vm, AjsObject *target, AjsValue props_val) {
    AjsObject *props = ajs_to_object(vm, props_val);
    if (!props) return false;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue pv = ajs_val_from_gc(props);
    ajs_temp_root_push(vm, pv);
    AjsValue *keys = NULL;
    uint32_t kcount = 0;
    if (!ajs_obj_own_keys(vm, props, false, true, true, &keys, &kcount)) {
        ajs_temp_root_restore(vm, rs);
        return false;
    }
    for (uint32_t i = 0; i < kcount; i++) ajs_temp_root_push(vm, keys[i]);
    AjsPropDesc *descs = (AjsPropDesc *)calloc(kcount ? kcount : 1u, sizeof(AjsPropDesc));
    AjsValue *sel_keys = (AjsValue *)calloc(kcount ? kcount : 1u, sizeof(AjsValue));
    uint32_t sel_n = 0;
    if (!descs || !sel_keys) {
        free(keys); free(descs); free(sel_keys);
        ajs_temp_root_restore(vm, rs);
        ajs_throw(vm, vm->oom_error);
        return false;
    }
    for (uint32_t i = 0; i < kcount; i++) {
        AjsPropDesc kd;
        bool found = false;
        if (!ajs_obj_get_own_desc(vm, props, keys[i], &kd, &found)) goto fail;
        if (found && (kd.flags & AJS_DESC_ENUMERABLE)) {
            AjsValue dv = ajs_obj_get(vm, pv, keys[i], pv);
            if (vm->has_exception) goto fail;
            ajs_temp_root_push(vm, dv);
            if (!to_prop_desc(vm, dv, &descs[sel_n])) goto fail;
            if (descs[sel_n].flags & AJS_DESC_HAS_VALUE) ajs_temp_root_push(vm, descs[sel_n].value);
            if (descs[sel_n].flags & AJS_DESC_HAS_GET) ajs_temp_root_push(vm, descs[sel_n].getter);
            if (descs[sel_n].flags & AJS_DESC_HAS_SET) ajs_temp_root_push(vm, descs[sel_n].setter);
            sel_keys[sel_n++] = keys[i];
        }
    }
    for (uint32_t i = 0; i < sel_n; i++) {
        if (!ajs_obj_define_own_prop(vm, target, sel_keys[i], &descs[i], true)) goto fail;
    }
    free(keys); free(descs); free(sel_keys);
    ajs_temp_root_restore(vm, rs);
    return true;
fail:
    free(keys); free(descs); free(sel_keys);
    ajs_temp_root_restore(vm, rs);
    return false;
}

static AjsValue js_object_create(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue proto = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (proto != AJS_VAL_NULL && !ajs_val_is_gc_kind(proto, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object prototype may only be an Object or null");
    }
    AjsObject *o = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ORDINARY, proto);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue ov = ajs_val_from_gc(o);
    ajs_temp_root_push(vm, ov);
    if (argc > 1 && argv[1] != AJS_VAL_UNDEFINED) {
        if (!define_props_from_obj(vm, o, argv[1])) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    ajs_temp_root_restore(vm, rs);
    return ov;
}

static AjsValue js_object_defineProperty(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 1 || !ajs_val_is_gc_kind(argv[0], AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object.defineProperty called on non-object");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue key = ajs_to_property_key(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, key);
    AjsPropDesc desc;
    if (!to_prop_desc(vm, argc > 2 ? argv[2] : AJS_VAL_UNDEFINED, &desc)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    if (desc.flags & AJS_DESC_HAS_VALUE) ajs_temp_root_push(vm, desc.value);
    if (desc.flags & AJS_DESC_HAS_GET) ajs_temp_root_push(vm, desc.getter);
    if (desc.flags & AJS_DESC_HAS_SET) ajs_temp_root_push(vm, desc.setter);
    bool ok = ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(argv[0]), key, &desc, true);
    ajs_temp_root_restore(vm, rs);
    return ok ? argv[0] : AJS_VAL_EXCEPTION;
}

static AjsValue js_object_defineProperties(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 1 || !ajs_val_is_gc_kind(argv[0], AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object.defineProperties called on non-object");
    }
    if (!define_props_from_obj(vm, (AjsObject *)ajs_val_to_gc(argv[0]), argc > 1 ? argv[1] : AJS_VAL_UNDEFINED)) {
        return AJS_VAL_EXCEPTION;
    }
    return argv[0];
}

static AjsValue js_object_getOwnPropertyDescriptor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsObject *o = ajs_to_object(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(o));
    AjsValue key = ajs_to_property_key(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, key);
    AjsPropDesc d;
    bool found = false;
    if (!ajs_obj_get_own_desc(vm, o, key, &d, &found)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsValue res = found ? from_prop_desc(vm, vm->active_realm, &d) : AJS_VAL_UNDEFINED;
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_object_getOwnPropertyDescriptors(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsObject *o = ajs_to_object(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(o));
    AjsValue *keys = NULL;
    uint32_t kcount = 0;
    if (!ajs_obj_own_keys(vm, o, false, true, true, &keys, &kcount)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    for (uint32_t i = 0; i < kcount; i++) ajs_temp_root_push(vm, keys[i]);
    AjsObject *out = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ORDINARY, vm->active_realm->object_proto);
    if (!out) { free(keys); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsValue out_val = ajs_val_from_gc(out);
    ajs_temp_root_push(vm, out_val);
    for (uint32_t i = 0; i < kcount; i++) {
        AjsPropDesc d;
        bool found = false;
        if (!ajs_obj_get_own_desc(vm, o, keys[i], &d, &found)) {
            free(keys); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
        }
        if (found) {
            AjsValue dobj = from_prop_desc(vm, vm->active_realm, &d);
            ajs_temp_root_push(vm, dobj);
            AjsPropDesc cd = {
                .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                         AJS_DESC_HAS_ENUMERABLE | AJS_DESC_ENUMERABLE |
                         AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
                .value = dobj
            };
            ajs_obj_define_own_prop(vm, out, keys[i], &cd, false);
        }
    }
    free(keys);
    ajs_temp_root_restore(vm, rs);
    return out_val;
}

static AjsValue obj_key_list(AjsVm *vm, AjsValue target, int mode) {
    /* mode: 0=keys, 1=values, 2=entries, 3=getOwnPropertyNames, 4=getOwnPropertySymbols */
    AjsObject *o = ajs_to_object(vm, target);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue ov = ajs_val_from_gc(o);
    ajs_temp_root_push(vm, ov);
    AjsValue *keys = NULL;
    uint32_t kcount = 0;
    bool inc_str = (mode != 4);
    bool inc_sym = (mode == 4);
    if (!ajs_obj_own_keys(vm, o, false, inc_str, inc_sym, &keys, &kcount)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    for (uint32_t i = 0; i < kcount; i++) ajs_temp_root_push(vm, keys[i]);
    AjsValue arr_val = ajs_new_array(vm, 0);
    ajs_temp_root_push(vm, arr_val);
    uint32_t out_idx = 0;
    for (uint32_t i = 0; i < kcount; i++) {
        if (mode <= 2) {
            AjsPropDesc d;
            bool found = false;
            if (!ajs_obj_get_own_desc(vm, o, keys[i], &d, &found)) {
                free(keys); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
            }
            if (!found || !(d.flags & AJS_DESC_ENUMERABLE)) continue;
        }
        AjsValue elem = keys[i];
        if (mode == 1 || mode == 2) {
            AjsValue v = ajs_obj_get(vm, ov, keys[i], ov);
            if (vm->has_exception) { free(keys); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            if (mode == 1) {
                elem = v;
            } else {
                ajs_temp_root_push(vm, v);
                AjsValue pair = ajs_new_array(vm, 2);
                ajs_temp_root_push(vm, pair);
                ajs_set_prop_index(vm, pair, 0, keys[i]);
                ajs_set_prop_index(vm, pair, 1, v);
                elem = pair;
            }
        }
        ajs_temp_root_push(vm, elem);
        ajs_set_prop_index(vm, arr_val, out_idx++, elem);
    }
    free(keys);
    ajs_temp_root_restore(vm, rs);
    return arr_val;
}

static AjsValue js_object_keys(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    return obj_key_list(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, 0);
}
static AjsValue js_object_values(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    return obj_key_list(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, 1);
}
static AjsValue js_object_entries(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    return obj_key_list(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, 2);
}
static AjsValue js_object_getOwnPropertyNames(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    return obj_key_list(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, 3);
}
static AjsValue js_object_getOwnPropertySymbols(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    return obj_key_list(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, 4);
}

static AjsValue js_object_fromEntries(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue iterable = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (ajs_is_nullish(iterable)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object.fromEntries requires an iterable");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *out = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ORDINARY, vm->active_realm->object_proto);
    AjsValue out_val = ajs_val_from_gc(out);
    ajs_temp_root_push(vm, out_val);

    AjsValue it_fn = ajs_obj_get(vm, iterable, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), iterable);
    if (vm->has_exception || !ajs_is_callable(vm, it_fn)) {
        ajs_temp_root_restore(vm, rs);
        if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object is not iterable");
        return AJS_VAL_EXCEPTION;
    }
    AjsValue iter = ajs_call_internal(vm, it_fn, iterable, 0, NULL);
    if (vm->has_exception || !ajs_is_object(vm, iter)) {
        ajs_temp_root_restore(vm, rs);
        if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator must be an object");
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_push(vm, iter);
    AjsValue next_fn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_next), iter);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, next_fn);

    for (;;) {
        size_t lsave = ajs_temp_root_save(vm);
        AjsValue step = ajs_call_internal(vm, next_fn, iter, 0, NULL);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (!ajs_is_object(vm, step)) {
            ajs_temp_root_restore(vm, rs);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator result is not an object");
        }
        ajs_temp_root_push(vm, step);
        AjsValue done = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_done), step);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (ajs_to_bool(vm, done)) { ajs_temp_root_restore(vm, lsave); break; }
        AjsValue item = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (!ajs_is_object(vm, item)) {
            /* IteratorClose on abrupt completion */
            AjsValue ret_fn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_return), iter);
            if (!vm->has_exception && ajs_is_callable(vm, ret_fn)) ajs_call_internal(vm, ret_fn, iter, 0, NULL);
            ajs_clear_exception(vm);
            ajs_temp_root_restore(vm, rs);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator value is not an entry object");
        }
        ajs_temp_root_push(vm, item);
        AjsValue k = ajs_get_prop_index(vm, item, 0);
        if (vm->has_exception) goto close_iter;
        ajs_temp_root_push(vm, k);
        AjsValue v = ajs_get_prop_index(vm, item, 1);
        if (vm->has_exception) goto close_iter;
        ajs_temp_root_push(vm, v);
        AjsValue pk = ajs_to_property_key(vm, k);
        if (vm->has_exception) goto close_iter;
        ajs_temp_root_push(vm, pk);
        AjsPropDesc d = {
            .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                     AJS_DESC_HAS_ENUMERABLE | AJS_DESC_ENUMERABLE |
                     AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
            .value = v
        };
        if (!ajs_obj_define_own_prop(vm, out, pk, &d, true)) goto close_iter;
        ajs_temp_root_restore(vm, lsave);
        continue;
    close_iter: {
            AjsValue saved_exc = vm->exception;
            ajs_temp_root_push(vm, saved_exc);
            ajs_clear_exception(vm);
            AjsValue ret_fn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_return), iter);
            if (!vm->has_exception && ajs_is_callable(vm, ret_fn)) ajs_call_internal(vm, ret_fn, iter, 0, NULL);
            ajs_throw(vm, saved_exc);
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    ajs_temp_root_restore(vm, rs);
    return out_val;
}

static bool set_integrity_level(AjsVm *vm, AjsObject *o, bool freeze) {
    if (!ajs_obj_prevent_extensions(vm, o, true)) return false;
    AjsValue *keys = NULL;
    uint32_t kcount = 0;
    if (!ajs_obj_own_keys(vm, o, false, true, true, &keys, &kcount)) return false;
    size_t rs = ajs_temp_root_save(vm);
    for (uint32_t i = 0; i < kcount; i++) ajs_temp_root_push(vm, keys[i]);
    for (uint32_t i = 0; i < kcount; i++) {
        AjsPropDesc cur;
        bool found = false;
        if (!ajs_obj_get_own_desc(vm, o, keys[i], &cur, &found)) { free(keys); ajs_temp_root_restore(vm, rs); return false; }
        if (!found) continue;
        AjsPropDesc upd;
        memset(&upd, 0, sizeof(upd));
        upd.flags = AJS_DESC_HAS_CONFIGURABLE;
        if (freeze && !(cur.flags & AJS_DESC_IS_ACCESSOR) && (cur.flags & AJS_DESC_WRITABLE)) {
            upd.flags |= AJS_DESC_HAS_WRITABLE;
        }
        if (!ajs_obj_define_own_prop(vm, o, keys[i], &upd, true)) { free(keys); ajs_temp_root_restore(vm, rs); return false; }
    }
    free(keys);
    ajs_temp_root_restore(vm, rs);
    return true;
}

static bool test_integrity_level(AjsVm *vm, AjsObject *o, bool frozen, bool *out_res) {
    *out_res = false;
    bool ext = true;
    if (!ajs_obj_is_extensible(vm, o, &ext)) return false;
    if (ext) return true;
    AjsValue *keys = NULL;
    uint32_t kcount = 0;
    if (!ajs_obj_own_keys(vm, o, false, true, true, &keys, &kcount)) return false;
    size_t rs = ajs_temp_root_save(vm);
    for (uint32_t i = 0; i < kcount; i++) ajs_temp_root_push(vm, keys[i]);
    for (uint32_t i = 0; i < kcount; i++) {
        AjsPropDesc cur;
        bool found = false;
        if (!ajs_obj_get_own_desc(vm, o, keys[i], &cur, &found)) { free(keys); ajs_temp_root_restore(vm, rs); return false; }
        if (!found) continue;
        if (cur.flags & AJS_DESC_CONFIGURABLE) { free(keys); ajs_temp_root_restore(vm, rs); return true; }
        if (frozen && !(cur.flags & AJS_DESC_IS_ACCESSOR) && (cur.flags & AJS_DESC_WRITABLE)) {
            free(keys); ajs_temp_root_restore(vm, rs); return true;
        }
    }
    free(keys);
    ajs_temp_root_restore(vm, rs);
    *out_res = true;
    return true;
}

static AjsValue js_object_freeze(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue v = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return v;
    return set_integrity_level(vm, (AjsObject *)ajs_val_to_gc(v), true) ? v : AJS_VAL_EXCEPTION;
}
static AjsValue js_object_seal(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue v = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return v;
    return set_integrity_level(vm, (AjsObject *)ajs_val_to_gc(v), false) ? v : AJS_VAL_EXCEPTION;
}
static AjsValue js_object_isFrozen(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue v = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return AJS_VAL_TRUE;
    bool res = false;
    return test_integrity_level(vm, (AjsObject *)ajs_val_to_gc(v), true, &res) ? ajs_bool(res) : AJS_VAL_EXCEPTION;
}
static AjsValue js_object_isSealed(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue v = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return AJS_VAL_TRUE;
    bool res = false;
    return test_integrity_level(vm, (AjsObject *)ajs_val_to_gc(v), false, &res) ? ajs_bool(res) : AJS_VAL_EXCEPTION;
}
static AjsValue js_object_isExtensible(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue v = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return AJS_VAL_FALSE;
    bool ext = false;
    return ajs_obj_is_extensible(vm, (AjsObject *)ajs_val_to_gc(v), &ext) ? ajs_bool(ext) : AJS_VAL_EXCEPTION;
}
static AjsValue js_object_preventExtensions(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue v = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return v;
    return ajs_obj_prevent_extensions(vm, (AjsObject *)ajs_val_to_gc(v), true) ? v : AJS_VAL_EXCEPTION;
}
static AjsValue js_object_getPrototypeOf(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsObject *o = ajs_to_object(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!o) return AJS_VAL_EXCEPTION;
    AjsValue proto = AJS_VAL_NULL;
    return ajs_obj_get_proto(vm, o, &proto) ? proto : AJS_VAL_EXCEPTION;
}
static AjsValue js_object_setPrototypeOf(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue o = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue proto = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    if (ajs_is_nullish(o)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object.setPrototypeOf called on null or undefined");
    if (proto != AJS_VAL_NULL && !ajs_val_is_gc_kind(proto, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object prototype may only be an Object or null");
    }
    if (!ajs_val_is_gc_kind(o, AJS_GC_OBJECT)) return o;
    return ajs_obj_set_proto(vm, (AjsObject *)ajs_val_to_gc(o), proto, true) ? o : AJS_VAL_EXCEPTION;
}
static AjsValue js_object_is(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    return ajs_bool(ajs_same_value(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED,
                                       argc > 1 ? argv[1] : AJS_VAL_UNDEFINED));
}
static AjsValue js_object_hasOwn(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsObject *o = ajs_to_object(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(o));
    AjsValue k = ajs_to_property_key(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsPropDesc d;
    bool found = false;
    bool ok = ajs_obj_get_own_desc(vm, o, k, &d, &found);
    ajs_temp_root_restore(vm, rs);
    return ok ? ajs_bool(found) : AJS_VAL_EXCEPTION;
}

/* ES2024+ Object.groupBy(items, callbackfn) */
static AjsValue js_object_groupBy(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue items = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue cb = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    if (ajs_is_nullish(items)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object.groupBy called on null or undefined");
    if (!ajs_is_callable(vm, cb)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Callback must be callable");

    size_t rs = ajs_temp_root_save(vm);
    AjsObject *out = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ORDINARY, AJS_VAL_NULL);
    AjsValue out_val = ajs_val_from_gc(out);
    ajs_temp_root_push(vm, out_val);

    AjsValue it_fn = ajs_obj_get(vm, items, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), items);
    if (vm->has_exception || !ajs_is_callable(vm, it_fn)) {
        ajs_temp_root_restore(vm, rs);
        if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object is not iterable");
        return AJS_VAL_EXCEPTION;
    }
    AjsValue iter = ajs_call_internal(vm, it_fn, items, 0, NULL);
    if (vm->has_exception || !ajs_is_object(vm, iter)) {
        ajs_temp_root_restore(vm, rs);
        if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator must be an object");
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_push(vm, iter);
    AjsValue next_fn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_next), iter);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, next_fn);

    uint64_t k_idx = 0;
    for (;;) {
        size_t lsave = ajs_temp_root_save(vm);
        AjsValue step = ajs_call_internal(vm, next_fn, iter, 0, NULL);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (!ajs_is_object(vm, step)) {
            ajs_temp_root_restore(vm, rs);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator result is not an object");
        }
        ajs_temp_root_push(vm, step);
        AjsValue done = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_done), step);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (ajs_to_bool(vm, done)) { ajs_temp_root_restore(vm, lsave); break; }
        AjsValue val = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, val);

        AjsValue cargs[2] = { val, ajs_val_number((double)k_idx) };
        AjsValue gkey = ajs_call_internal(vm, cb, AJS_VAL_UNDEFINED, 2, cargs);
        if (vm->has_exception) goto close_gb;
        ajs_temp_root_push(vm, gkey);
        AjsValue pk = ajs_to_property_key(vm, gkey);
        if (vm->has_exception) goto close_gb;
        ajs_temp_root_push(vm, pk);

        AjsPropDesc gd;
        bool found = false;
        ajs_obj_get_own_desc(vm, out, pk, &gd, &found);
        AjsValue bucket;
        if (!found) {
            bucket = ajs_new_array(vm, 0);
            ajs_temp_root_push(vm, bucket);
            AjsPropDesc nd = {
                .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                         AJS_DESC_HAS_ENUMERABLE | AJS_DESC_ENUMERABLE |
                         AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
                .value = bucket
            };
            ajs_obj_define_own_prop(vm, out, pk, &nd, false);
        } else {
            bucket = gd.value;
        }
        AjsObject *barr = (AjsObject *)ajs_val_to_gc(bucket);
        ajs_set_prop_index(vm, bucket, barr->u.arr.length, val);
        k_idx++;
        ajs_temp_root_restore(vm, lsave);
        continue;
    close_gb: {
            AjsValue saved_exc = vm->exception;
            ajs_temp_root_push(vm, saved_exc);
            ajs_clear_exception(vm);
            AjsValue ret_fn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_return), iter);
            if (!vm->has_exception && ajs_is_callable(vm, ret_fn)) ajs_call_internal(vm, ret_fn, iter, 0, NULL);
            ajs_throw(vm, saved_exc);
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    ajs_temp_root_restore(vm, rs);
    return out_val;
}

static AjsValue js_object_proto_toString(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    if (this_val == AJS_VAL_UNDEFINED) return ajs_val_from_gc(ajs_str_from_cstr(vm, "[object Undefined]", true));
    if (this_val == AJS_VAL_NULL)      return ajs_val_from_gc(ajs_str_from_cstr(vm, "[object Null]", true));
    AjsObject *o = ajs_to_object(vm, this_val);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue ov = ajs_val_from_gc(o);
    ajs_temp_root_push(vm, ov);

    bool is_arr = ajs_is_array(vm, ov);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    const char *builtin_tag = "Object";
    if (is_arr) builtin_tag = "Array";
    else if (o->kind == AJS_OBJ_ARGUMENTS) builtin_tag = "Arguments";
    else if (ajs_is_callable(vm, ov)) builtin_tag = "Function";
    else if (o->kind == AJS_OBJ_ERROR) builtin_tag = "Error";
    else if (o->kind == AJS_OBJ_BOOLEAN_OBJ) builtin_tag = "Boolean";
    else if (o->kind == AJS_OBJ_NUMBER_OBJ) builtin_tag = "Number";
    else if (o->kind == AJS_OBJ_STRING_OBJ) builtin_tag = "String";
    else if (o->kind == AJS_OBJ_DATE) builtin_tag = "Date";
    else if (o->kind == AJS_OBJ_REGEXP) builtin_tag = "RegExp";

    AjsValue tag_v = ajs_obj_get(vm, ov, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_TO_STRING_TAG]), ov);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    const char *tag = ajs_val_is_gc_kind(tag_v, AJS_GC_STRING) ? ((AjsString *)ajs_val_to_gc(tag_v))->utf8 : builtin_tag;

    char buf[256];
    snprintf(buf, sizeof(buf), "[object %s]", tag);
    AjsString *s = ajs_str_from_cstr(vm, buf, false);
    ajs_temp_root_restore(vm, rs);
    return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
}

static AjsValue js_object_proto_toLocaleString(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    if (ajs_is_nullish(this_val)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot convert undefined or null to object");
    AjsValue fn = ajs_obj_get(vm, this_val, ajs_val_from_gc(vm->str_toString), this_val);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (!ajs_is_callable(vm, fn)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "toString is not callable");
    return ajs_call_internal(vm, fn, this_val, 0, NULL);
}

static AjsValue js_object_proto_valueOf(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsObject *o = ajs_to_object(vm, this_val);
    return o ? ajs_val_from_gc(o) : AJS_VAL_EXCEPTION;
}

static AjsValue js_object_proto_hasOwnProperty(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue k = ajs_to_property_key(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, k);
    AjsObject *o = ajs_to_object(vm, this_val);
    if (!o) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsPropDesc d;
    bool found = false;
    bool ok = ajs_obj_get_own_desc(vm, o, k, &d, &found);
    ajs_temp_root_restore(vm, rs);
    return ok ? ajs_bool(found) : AJS_VAL_EXCEPTION;
}

static AjsValue js_object_proto_isPrototypeOf(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsValue v = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return AJS_VAL_FALSE;
    AjsObject *o = ajs_to_object(vm, this_val);
    if (!o) return AJS_VAL_EXCEPTION;
    AjsValue ov = ajs_val_from_gc(o);
    AjsValue cur = v;
    for (int depth = 0; depth < 256; depth++) {
        AjsValue p = AJS_VAL_NULL;
        if (!ajs_obj_get_proto(vm, (AjsObject *)ajs_val_to_gc(cur), &p)) return AJS_VAL_EXCEPTION;
        if (p == AJS_VAL_NULL) return AJS_VAL_FALSE;
        if (p == ov) return AJS_VAL_TRUE;
        cur = p;
    }
    return AJS_VAL_FALSE;
}

static AjsValue js_object_proto_propertyIsEnumerable(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue k = ajs_to_property_key(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, k);
    AjsObject *o = ajs_to_object(vm, this_val);
    if (!o) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsPropDesc d;
    bool found = false;
    bool ok = ajs_obj_get_own_desc(vm, o, k, &d, &found);
    ajs_temp_root_restore(vm, rs);
    if (!ok) return AJS_VAL_EXCEPTION;
    return ajs_bool(found && (d.flags & AJS_DESC_ENUMERABLE) != 0);
}

/* Annex B Object.prototype.__proto__ & legacy getter/setter methods */
static AjsValue js_object_proto_get_dunder(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsObject *o = ajs_to_object(vm, this_val);
    if (!o) return AJS_VAL_EXCEPTION;
    AjsValue p = AJS_VAL_NULL;
    return ajs_obj_get_proto(vm, o, &p) ? p : AJS_VAL_EXCEPTION;
}
static AjsValue js_object_proto_set_dunder(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (ajs_is_nullish(this_val)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object.prototype.__proto__ called on null or undefined");
    AjsValue proto = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (proto != AJS_VAL_NULL && !ajs_val_is_gc_kind(proto, AJS_GC_OBJECT)) return AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) return AJS_VAL_UNDEFINED;
    if (!ajs_obj_set_proto(vm, (AjsObject *)ajs_val_to_gc(this_val), proto, true)) return AJS_VAL_EXCEPTION;
    return AJS_VAL_UNDEFINED;
}
static AjsValue js_object_defineGetterSetter(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, bool is_set) {
    AjsObject *o = ajs_to_object(vm, this_val);
    if (!o) return AJS_VAL_EXCEPTION;
    AjsValue fn = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    if (!ajs_is_callable(vm, fn)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Getter/setter must be callable");
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(o));
    AjsValue k = ajs_to_property_key(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_ENUMERABLE |
              AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE |
              (is_set ? AJS_DESC_HAS_SET : AJS_DESC_HAS_GET);
    if (is_set) d.setter = fn; else d.getter = fn;
    bool ok = ajs_obj_define_own_prop(vm, o, k, &d, true);
    ajs_temp_root_restore(vm, rs);
    return ok ? AJS_VAL_UNDEFINED : AJS_VAL_EXCEPTION;
}
static AjsValue js_object_defineGetter(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud; return js_object_defineGetterSetter(vm, this_val, argc, argv, false);
}
static AjsValue js_object_defineSetter(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud; return js_object_defineGetterSetter(vm, this_val, argc, argv, true);
}
static AjsValue js_object_lookupGetterSetter(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, bool is_set) {
    AjsObject *o = ajs_to_object(vm, this_val);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(o));
    AjsValue k = ajs_to_property_key(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, k);
    for (int depth = 0; depth < 256 && o; depth++) {
        AjsPropDesc d;
        bool found = false;
        if (!ajs_obj_get_own_desc(vm, o, k, &d, &found)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (found) {
            ajs_temp_root_restore(vm, rs);
            if (!(d.flags & AJS_DESC_IS_ACCESSOR)) return AJS_VAL_UNDEFINED;
            return is_set ? d.setter : d.getter;
        }
        AjsValue p = AJS_VAL_NULL;
        if (!ajs_obj_get_proto(vm, o, &p) || !ajs_val_is_gc_kind(p, AJS_GC_OBJECT)) break;
        o = (AjsObject *)ajs_val_to_gc(p);
    }
    ajs_temp_root_restore(vm, rs);
    return AJS_VAL_UNDEFINED;
}
static AjsValue js_object_lookupGetter(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud; return js_object_lookupGetterSetter(vm, this_val, argc, argv, false);
}
static AjsValue js_object_lookupSetter(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud; return js_object_lookupGetterSetter(vm, this_val, argc, argv, true);
}

/* ============================================================================
 * 3. Function, Generator, Boolean, Number, Math & Symbol Built-ins
 * ============================================================================ */

static AjsValue js_func_proto_noop(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)vm; (void)this_val; (void)argc; (void)argv; (void)ud;
    return AJS_VAL_UNDEFINED;
}

static AjsValue js_throw_type_error(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)argc; (void)argv; (void)ud;
    return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                              "'caller', 'callee', and 'arguments' properties may not be accessed on strict mode functions");
}

static AjsValue create_dynamic_function(AjsVm *vm, int argc, const AjsValue *argv,
                                        AjsFuncKind fn_kind, AjsValue default_proto) {
    AjsRealm *r = vm->active_realm;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue nt = ajs_get_native_new_target(vm);

    /* Build parameter string from argv[0..argc-2] and body from argv[argc-1] */
    size_t p_cap = 64, p_len = 0;
    char *params = (char *)malloc(p_cap);
    if (!params) return ajs_throw(vm, vm->oom_error);
    params[0] = '\0';

    int pcount = argc > 1 ? argc - 1 : 0;
    for (int i = 0; i < pcount; i++) {
        AjsString *ps = ajs_to_js_string(vm, argv[i]);
        if (!ps || vm->has_exception) { free(params); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        size_t need = p_len + ps->len8 + 3u;
        if (need > p_cap) {
            p_cap = need * 2u;
            char *nb = (char *)realloc(params, p_cap);
            if (!nb) { free(params); ajs_temp_root_restore(vm, rs); return ajs_throw(vm, vm->oom_error); }
            params = nb;
        }
        if (i > 0) params[p_len++] = ',';
        memcpy(params + p_len, ps->utf8, ps->len8);
        p_len += ps->len8;
        params[p_len] = '\0';
    }

    const char *body_c = "";
    size_t body_len = 0;
    if (argc >= 1) {
        AjsString *bs = ajs_to_js_string(vm, argv[argc - 1]);
        if (!bs || vm->has_exception) { free(params); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(bs));
        body_c = bs->utf8;
        body_len = bs->len8;
    }

    AjsBytecode *bc = ajs_compile_function_dynamic(vm, r, params, p_len, body_c, body_len, fn_kind);
    free(params);
    if (!bc || vm->has_exception) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_push(vm, ajs_val_from_gc(bc));

    AjsValue proto = default_proto;
    if (nt != AJS_VAL_UNDEFINED) {
        proto = get_proto_from_ctor(vm, nt, default_proto);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    }
    ajs_temp_root_push(vm, proto);

    AjsObject *fn = ajs_closure_new(vm, r, bc, r->global_lex_env, NULL);
    if (!fn) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    fn->proto = proto;
    AjsValue fv = ajs_val_from_gc(fn);
    ajs_temp_root_restore(vm, rs);
    return fv;
}

static AjsValue js_function_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    return create_dynamic_function(vm, argc, argv, AJS_FN_BC_NORMAL, vm->active_realm->function_proto);
}
static AjsValue js_gen_func_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    return create_dynamic_function(vm, argc, argv, AJS_FN_BC_GENERATOR, vm->active_realm->gen_func_proto);
}
static AjsValue js_async_func_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    return create_dynamic_function(vm, argc, argv, AJS_FN_BC_ASYNC, vm->active_realm->async_func_proto);
}
static AjsValue js_async_gen_func_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    return create_dynamic_function(vm, argc, argv, AJS_FN_BC_ASYNC_GENERATOR, vm->active_realm->async_gen_func_proto);
}

static AjsValue js_func_proto_call(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_is_callable(vm, this_val)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Function.prototype.call called on non-callable");
    }
    AjsValue recv = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    return ajs_call_internal(vm, this_val, recv, argc > 1 ? argc - 1 : 0, argc > 1 ? argv + 1 : NULL);
}

static AjsValue create_list_from_array_like_call(AjsVm *vm, AjsValue fn, AjsValue recv, AjsValue arg_array, bool is_construct, AjsValue new_target) {
    if (ajs_is_nullish(arg_array)) {
        return is_construct ? ajs_construct_internal(vm, fn, 0, NULL, new_target)
                            : ajs_call_internal(vm, fn, recv, 0, NULL);
    }
    if (!ajs_val_is_gc_kind(arg_array, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "CreateListFromArrayLike called on non-object");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue len_v = ajs_obj_get(vm, arg_array, ajs_val_from_gc(vm->str_length), arg_array);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    uint64_t len64 = 0;
    if (!ajs_to_length(vm, len_v, &len64)) return AJS_VAL_EXCEPTION;
    if (len64 > 65535u) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Too many arguments in function apply");
    }
    int n = (int)len64;
    AjsValue *args = n > 0 ? (AjsValue *)malloc((size_t)n * sizeof(AjsValue)) : NULL;
    if (n > 0 && !args) return ajs_throw(vm, vm->oom_error);
    for (int i = 0; i < n; i++) {
        args[i] = ajs_get_prop_index(vm, arg_array, (uint32_t)i);
        if (vm->has_exception) { free(args); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, args[i]);
    }
    AjsValue res = is_construct ? ajs_construct_internal(vm, fn, n, args, new_target)
                                : ajs_call_internal(vm, fn, recv, n, args);
    free(args);
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_func_proto_apply(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_is_callable(vm, this_val)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Function.prototype.apply called on non-callable");
    }
    AjsValue recv = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue arg_arr = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    return create_list_from_array_like_call(vm, this_val, recv, arg_arr, false, AJS_VAL_UNDEFINED);
}

static AjsValue js_func_proto_bind(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_is_callable(vm, this_val)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Bind must be called on a function");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *target_obj = (AjsObject *)ajs_val_to_gc(this_val);
    AjsValue proto = AJS_VAL_NULL;
    if (!ajs_obj_get_proto(vm, target_obj, &proto)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, proto);

    AjsObject *bfn = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_FUNCTION, proto);
    if (!bfn) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsValue bval = ajs_val_from_gc(bfn);
    ajs_temp_root_push(vm, bval);

    bfn->u.fn.fn_kind = AJS_FN_BOUND;
    bfn->u.fn.is_constructor = ajs_is_constructor_val(this_val);
    bfn->u.fn.bound_target = this_val;
    bfn->u.fn.bound_this = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    int bargc = argc > 1 ? argc - 1 : 0;
    bfn->u.fn.bound_argc = bargc;
    if (bargc > 0) {
        bfn->u.fn.bound_argv = (AjsValue *)malloc((size_t)bargc * sizeof(AjsValue));
        if (!bfn->u.fn.bound_argv) { ajs_temp_root_restore(vm, rs); return ajs_throw(vm, vm->oom_error); }
        memcpy(bfn->u.fn.bound_argv, argv + 1, (size_t)bargc * sizeof(AjsValue));
    }

    /* Compute bound length per ECMA-262 §10.4.1.3 */
    double L = 0.0;
    bool has_len = false;
    if (ajs_obj_has_prop(vm, target_obj, ajs_val_from_gc(vm->str_length), &has_len) && has_len) {
        AjsValue tlen = ajs_obj_get(vm, this_val, ajs_val_from_gc(vm->str_length), this_val);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (ajs_val_is_number(tlen)) {
            double d = ajs_val_to_number_unchecked(tlen);
            if (isinf(d) && d > 0.0) L = INFINITY;
            else if (isfinite(d)) {
                double ti = ajs_double_to_integer_or_inf(d);
                if (ti > (double)bargc) L = ti - (double)bargc;
            }
        }
    }
    def_data_prop(vm, bval, "length", ajs_val_number(L), AJS_DESC_CONFIGURABLE);

    AjsValue tname = ajs_obj_get(vm, this_val, ajs_val_from_gc(vm->str_name), this_val);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    const char *tn = ajs_val_is_gc_kind(tname, AJS_GC_STRING) ? ((AjsString *)ajs_val_to_gc(tname))->utf8 : "";
    size_t nlen = strlen(tn) + 7u;
    char *nbuf = (char *)malloc(nlen);
    if (nbuf) {
        snprintf(nbuf, nlen, "bound %s", tn);
        def_data_prop(vm, bval, "name", ajs_val_from_gc(ajs_str_from_cstr(vm, nbuf, false)), AJS_DESC_CONFIGURABLE);
        free(nbuf);
    }

    ajs_temp_root_restore(vm, rs);
    return bval;
}

static AjsValue js_func_proto_toString(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    if (!ajs_is_callable(vm, this_val)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Function.prototype.toString requires a callable receiver");
    }
    AjsObject *o = (AjsObject *)ajs_val_to_gc(this_val);
    if (o->kind == AJS_OBJ_FUNCTION) {
        if (o->u.fn.source_text) return ajs_val_from_gc(o->u.fn.source_text);
        if (o->u.fn.bc && o->u.fn.bc->source_text) return ajs_val_from_gc(o->u.fn.bc->source_text);
    }
    AjsValue nv = ajs_obj_get(vm, this_val, ajs_val_from_gc(vm->str_name), this_val);
    if (vm->has_exception) ajs_clear_exception(vm);
    const char *nm = ajs_val_is_gc_kind(nv, AJS_GC_STRING) ? ((AjsString *)ajs_val_to_gc(nv))->utf8 : "";
    char buf[256];
    snprintf(buf, sizeof(buf), "function %s() { [native code] }", nm);
    return ajs_val_from_gc(ajs_str_from_cstr(vm, buf, false));
}

static AjsValue js_func_proto_hasInstance(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_is_callable(vm, this_val)) return AJS_VAL_FALSE;
    AjsValue v = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsObject *fn = (AjsObject *)ajs_val_to_gc(this_val);
    while (fn->kind == AJS_OBJ_FUNCTION && fn->u.fn.fn_kind == AJS_FN_BOUND) {
        this_val = fn->u.fn.bound_target;
        if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) return AJS_VAL_FALSE;
        fn = (AjsObject *)ajs_val_to_gc(this_val);
    }
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return AJS_VAL_FALSE;
    AjsValue p = ajs_obj_get(vm, this_val, ajs_val_from_gc(vm->str_prototype), this_val);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (!ajs_val_is_gc_kind(p, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Function has non-object prototype in instanceof check");
    }
    AjsValue cur = v;
    for (int depth = 0; depth < 256; depth++) {
        AjsValue cp = AJS_VAL_NULL;
        if (!ajs_obj_get_proto(vm, (AjsObject *)ajs_val_to_gc(cur), &cp)) return AJS_VAL_EXCEPTION;
        if (cp == AJS_VAL_NULL) return AJS_VAL_FALSE;
        if (cp == p) return AJS_VAL_TRUE;
        cur = cp;
    }
    return AJS_VAL_FALSE;
}

/* Generator & AsyncGenerator prototype methods */
static AjsValue gen_proto_step(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, int mode, bool is_async) {
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        if (is_async) {
            AjsValue rej = AJS_VAL_UNDEFINED, res = AJS_VAL_UNDEFINED;
            AjsValue p = ajs_promise_create(vm, &res, &rej);
            size_t rs = ajs_temp_root_save(vm);
            ajs_temp_root_push(vm, p); ajs_temp_root_push(vm, rej);
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "AsyncGenerator method called on incompatible receiver");
            AjsValue exc = ajs_take_exception(vm, NULL);
            ajs_call_internal(vm, rej, AJS_VAL_UNDEFINED, 1, &exc);
            ajs_temp_root_restore(vm, rs);
            return p;
        }
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Generator method called on incompatible receiver");
    }
    AjsObject *g = (AjsObject *)ajs_val_to_gc(this_val);
    AjsObjKind want = is_async ? AJS_OBJ_ASYNC_GENERATOR : AJS_OBJ_GENERATOR;
    if (g->kind != want) {
        if (is_async) {
            AjsValue rej = AJS_VAL_UNDEFINED, res = AJS_VAL_UNDEFINED;
            AjsValue p = ajs_promise_create(vm, &res, &rej);
            size_t rs = ajs_temp_root_save(vm);
            ajs_temp_root_push(vm, p); ajs_temp_root_push(vm, rej);
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "AsyncGenerator method called on incompatible receiver");
            AjsValue exc = ajs_take_exception(vm, NULL);
            ajs_call_internal(vm, rej, AJS_VAL_UNDEFINED, 1, &exc);
            ajs_temp_root_restore(vm, rs);
            return p;
        }
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Generator method called on incompatible receiver");
    }
    return ajs_gen_resume(vm, g, mode, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
}

static AjsValue js_gen_next(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u)  { (void)u; return gen_proto_step(vm, t, c, v, 0, false); }
static AjsValue js_gen_return(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u){ (void)u; return gen_proto_step(vm, t, c, v, 1, false); }
static AjsValue js_gen_throw(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)u; return gen_proto_step(vm, t, c, v, 2, false); }
static AjsValue js_agen_next(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u)  { (void)u; return gen_proto_step(vm, t, c, v, 0, true); }
static AjsValue js_agen_return(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u){ (void)u; return gen_proto_step(vm, t, c, v, 1, true); }
static AjsValue js_agen_throw(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)u; return gen_proto_step(vm, t, c, v, 2, true); }

/* Boolean */
static bool this_bool_val(AjsVm *vm, AjsValue this_val, bool *out_b) {
    if (this_val == AJS_VAL_TRUE || this_val == AJS_VAL_FALSE) {
        *out_b = (this_val == AJS_VAL_TRUE);
        return true;
    }
    if (ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        AjsObject *o = (AjsObject *)ajs_val_to_gc(this_val);
        if (o->kind == AJS_OBJ_BOOLEAN_OBJ) {
            *out_b = (o->u.prim_val == AJS_VAL_TRUE);
            return true;
        }
    }
    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Boolean.prototype method called on incompatible receiver");
    return false;
}

static AjsValue js_boolean_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    bool b = ajs_to_bool(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt == AJS_VAL_UNDEFINED) return ajs_bool(b);
    AjsValue proto = get_proto_from_ctor(vm, nt, vm->active_realm->boolean_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    AjsObject *o = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_BOOLEAN_OBJ, proto);
    if (!o) return AJS_VAL_EXCEPTION;
    o->u.prim_val = ajs_bool(b);
    return ajs_val_from_gc(o);
}
static AjsValue js_boolean_proto_toString(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    bool b = false;
    if (!this_bool_val(vm, this_val, &b)) return AJS_VAL_EXCEPTION;
    return ajs_val_from_gc(ajs_str_from_cstr(vm, b ? "true" : "false", true));
}
static AjsValue js_boolean_proto_valueOf(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    bool b = false;
    if (!this_bool_val(vm, this_val, &b)) return AJS_VAL_EXCEPTION;
    return ajs_bool(b);
}

/* Number */
static bool this_num_val(AjsVm *vm, AjsValue this_val, double *out_d) {
    if (ajs_val_is_number(this_val)) {
        *out_d = ajs_val_to_number_unchecked(this_val);
        return true;
    }
    if (ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        AjsObject *o = (AjsObject *)ajs_val_to_gc(this_val);
        if (o->kind == AJS_OBJ_NUMBER_OBJ && ajs_val_is_number(o->u.prim_val)) {
            *out_d = ajs_val_to_number_unchecked(o->u.prim_val);
            return true;
        }
    }
    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Number.prototype method called on incompatible receiver");
    return false;
}

static double bigint_to_double(const AjsBigInt *bi) {
    if (!bi || bi->sign == 0 || bi->len == 0) return 0.0;
    double v = 0.0;
    for (uint32_t i = bi->len; i > 0; i--) {
        v = v * 4294967296.0 + (double)bi->limbs[i - 1u];
    }
    return bi->sign < 0 ? -v : v;
}

static AjsValue js_number_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    double n = 0.0;
    if (argc > 0) {
        AjsValue prim = argv[0];
        if (ajs_val_is_gc_kind(prim, AJS_GC_OBJECT)) {
            prim = ajs_to_primitive(vm, prim, 2);
            if (vm->has_exception) return AJS_VAL_EXCEPTION;
        }
        if (ajs_val_is_gc_kind(prim, AJS_GC_BIGINT)) {
            n = bigint_to_double((const AjsBigInt *)ajs_val_to_gc(prim));
        } else if (!ajs_to_number(vm, prim, &n)) {
            return AJS_VAL_EXCEPTION;
        }
    }
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt == AJS_VAL_UNDEFINED) return ajs_val_number(n);
    AjsValue proto = get_proto_from_ctor(vm, nt, vm->active_realm->number_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    AjsObject *o = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_NUMBER_OBJ, proto);
    if (!o) return AJS_VAL_EXCEPTION;
    o->u.prim_val = ajs_val_number(n);
    return ajs_val_from_gc(o);
}

static AjsValue js_number_isFinite(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)vm; (void)t; (void)u;
    if (c < 1 || !ajs_val_is_number(v[0])) return AJS_VAL_FALSE;
    return ajs_bool(isfinite(ajs_val_to_number_unchecked(v[0])));
}
static AjsValue js_number_isNaN(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)vm; (void)t; (void)u;
    if (c < 1 || !ajs_val_is_number(v[0])) return AJS_VAL_FALSE;
    return ajs_bool(isnan(ajs_val_to_number_unchecked(v[0])));
}
static AjsValue js_number_isInteger(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)vm; (void)t; (void)u;
    if (c < 1 || !ajs_val_is_number(v[0])) return AJS_VAL_FALSE;
    double d = ajs_val_to_number_unchecked(v[0]);
    return ajs_bool(isfinite(d) && floor(d) == d);
}
static AjsValue js_number_isSafeInteger(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)vm; (void)t; (void)u;
    if (c < 1 || !ajs_val_is_number(v[0])) return AJS_VAL_FALSE;
    double d = ajs_val_to_number_unchecked(v[0]);
    return ajs_bool(isfinite(d) && floor(d) == d && fabs(d) <= 9007199254740991.0);
}

static AjsValue js_number_proto_toString(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    double d = 0.0;
    if (!this_num_val(vm, this_val, &d)) return AJS_VAL_EXCEPTION;
    int radix = 10;
    if (argc > 0 && argv[0] != AJS_VAL_UNDEFINED) {
        double rd = 0.0;
        if (!ajs_to_number(vm, argv[0], &rd)) return AJS_VAL_EXCEPTION;
        rd = ajs_double_to_integer_or_inf(rd);
        if (rd < 2.0 || rd > 36.0) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "toString() radix argument must be between 2 and 36");
        }
        radix = (int)rd;
    }
    AjsString *s = ajs_number_to_str(vm, d, radix);
    return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
}
static AjsValue js_number_proto_valueOf(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    double d = 0.0;
    if (!this_num_val(vm, this_val, &d)) return AJS_VAL_EXCEPTION;
    return ajs_val_number(d);
}
static AjsValue js_number_proto_toFixed(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    double x = 0.0;
    if (!this_num_val(vm, this_val, &x)) return AJS_VAL_EXCEPTION;
    double fd = 0.0;
    if (argc > 0 && argv[0] != AJS_VAL_UNDEFINED) {
        if (!ajs_to_number(vm, argv[0], &fd)) return AJS_VAL_EXCEPTION;
        fd = ajs_double_to_integer_or_inf(fd);
    }
    if (isnan(fd) || fd < 0.0 || fd > 100.0) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "toFixed() digits argument must be between 0 and 100");
    }
    if (!isfinite(x) || fabs(x) >= 1e21) {
        AjsString *s = ajs_number_to_str(vm, x, 10);
        return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
    }
    if (x == 0.0) x = 0.0; /* -0 -> "0" per ECMA-262 */
    char buf[256];
    snprintf(buf, sizeof(buf), "%.*f", (int)fd, x);
    return ajs_val_from_gc(ajs_str_from_cstr(vm, buf, false));
}
static AjsValue js_number_proto_toExponential(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    double x = 0.0;
    if (!this_num_val(vm, this_val, &x)) return AJS_VAL_EXCEPTION;
    double fd = 6.0;
    bool has_fd = (argc > 0 && argv[0] != AJS_VAL_UNDEFINED);
    if (has_fd) {
        if (!ajs_to_number(vm, argv[0], &fd)) return AJS_VAL_EXCEPTION;
        fd = ajs_double_to_integer_or_inf(fd);
    }
    if (!isfinite(x)) {
        AjsString *s = ajs_number_to_str(vm, x, 10);
        return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
    }
    if (has_fd && (isnan(fd) || fd < 0.0 || fd > 100.0)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "toExponential() argument must be between 0 and 100");
    }
    if (x == 0.0) x = 0.0;
    char raw[256], out[256];
    snprintf(raw, sizeof(raw), "%.*e", has_fd ? (int)fd : 15, x);
    /* Strip leading zero on 2-digit exponents like e+02 -> e+2 per ECMA-262 */
    char *ep = strchr(raw, 'e');
    if (ep) {
        if (!has_fd) {
            /* Trim trailing zeros before 'e' */
            char *p = ep - 1;
            while (p > raw && *p == '0') p--;
            if (*p == '.') p--;
            size_t mlen = (size_t)(p - raw + 1);
            memcpy(out, raw, mlen);
            long eval = strtol(ep + 1, NULL, 10);
            snprintf(out + mlen, sizeof(out) - mlen, "e%+ld", eval);
        } else {
            size_t mlen = (size_t)(ep - raw);
            memcpy(out, raw, mlen);
            long eval = strtol(ep + 1, NULL, 10);
            snprintf(out + mlen, sizeof(out) - mlen, "e%+ld", eval);
        }
    } else {
        snprintf(out, sizeof(out), "%s", raw);
    }
    return ajs_val_from_gc(ajs_str_from_cstr(vm, out, false));
}
static AjsValue js_number_proto_toPrecision(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    double x = 0.0;
    if (!this_num_val(vm, this_val, &x)) return AJS_VAL_EXCEPTION;
    if (argc == 0 || argv[0] == AJS_VAL_UNDEFINED) {
        AjsString *s = ajs_number_to_str(vm, x, 10);
        return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
    }
    double pd = 0.0;
    if (!ajs_to_number(vm, argv[0], &pd)) return AJS_VAL_EXCEPTION;
    pd = ajs_double_to_integer_or_inf(pd);
    if (!isfinite(x)) {
        AjsString *s = ajs_number_to_str(vm, x, 10);
        return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
    }
    if (isnan(pd) || pd < 1.0 || pd > 100.0) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "toPrecision() argument must be between 1 and 100");
    }
    if (x == 0.0) x = 0.0;
    int p = (int)pd;
    char buf[256];
    int e10 = (x == 0.0) ? 0 : (int)floor(log10(fabs(x)));
    if (e10 < -6 || e10 >= p) {
        char raw[256];
        snprintf(raw, sizeof(raw), "%.*e", p - 1, x);
        char *ep = strchr(raw, 'e');
        if (ep) {
            size_t mlen = (size_t)(ep - raw);
            memcpy(buf, raw, mlen);
            long eval = strtol(ep + 1, NULL, 10);
            snprintf(buf + mlen, sizeof(buf) - mlen, "e%+ld", eval);
        } else {
            snprintf(buf, sizeof(buf), "%s", raw);
        }
    } else {
        int frac = p - 1 - e10;
        if (frac < 0) frac = 0;
        snprintf(buf, sizeof(buf), "%.*f", frac, x);
    }
    return ajs_val_from_gc(ajs_str_from_cstr(vm, buf, false));
}

/* Math */
static double f64_to_f16_to_f64(double x) {
    if (isnan(x)) return NAN;
    if (isinf(x)) return x;
    if (x == 0.0) return x;
    uint64_t bits = ajs_double_to_bits(x);
    uint16_t sign = (uint16_t)((bits >> 48) & 0x8000u);
    int exp = (int)((bits >> 52) & 0x7FFu) - 1023;
    uint64_t mant = bits & UINT64_C(0x000FFFFFFFFFFFFF);
    uint16_t h_exp = 0, h_mant = 0;
    if (exp > 15) {
        return sign ? -INFINITY : INFINITY;
    } else if (exp < -24) {
        return sign ? -0.0 : 0.0;
    } else if (exp < -14) {
        /* Subnormal float16 */
        mant |= UINT64_C(0x0010000000000000);
        int shift = -14 - exp + 42;
        uint64_t shifted = mant >> shift;
        uint64_t rem = mant & ((UINT64_C(1) << shift) - 1u);
        uint64_t half = UINT64_C(1) << (shift - 1);
        if (rem > half || (rem == half && (shifted & 1u))) shifted++;
        h_mant = (uint16_t)shifted;
        if (h_mant >= 1024u) { h_exp = 1u; h_mant = 0; }
    } else {
        h_exp = (uint16_t)(exp + 15);
        uint64_t shifted = mant >> 42;
        uint64_t rem = mant & ((UINT64_C(1) << 42) - 1u);
        uint64_t half = UINT64_C(1) << 41;
        if (rem > half || (rem == half && (shifted & 1u))) {
            shifted++;
            if (shifted >= 1024u) {
                shifted = 0;
                h_exp++;
                if (h_exp >= 31u) return sign ? -INFINITY : INFINITY;
            }
        }
        h_mant = (uint16_t)shifted;
    }
    double val;
    if (h_exp == 0) val = ldexp((double)h_mant, -24);
    else val = ldexp((double)(1024u + h_mant), (int)h_exp - 25);
    return sign ? -val : val;
}

static AjsValue js_math_1(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    int32_t op = 0;
    if (vm->call_depth > 0 && vm->call_stack[vm->call_depth - 1u].native_fn_obj) {
        op = vm->call_stack[vm->call_depth - 1u].native_fn_obj->u.fn.native_magic;
    }
    double x = NAN;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &x)) return AJS_VAL_EXCEPTION;
    double r = NAN;
    switch (op) {
        case 1:  r = fabs(x); break;
        case 2:  r = acos(x); break;
        case 3:  r = acosh(x); break;
        case 4:  r = asin(x); break;
        case 5:  r = asinh(x); break;
        case 6:  r = atan(x); break;
        case 7:  r = atanh(x); break;
        case 8:  r = cbrt(x); break;
        case 9:  r = ceil(x); break;
        case 10: {
            uint32_t u = ajs_double_to_uint32(x);
            int lz = 32;
            if (u != 0) { lz = 0; while ((u & 0x80000000u) == 0) { lz++; u <<= 1; } }
            r = (double)lz;
            break;
        }
        case 11: r = cos(x); break;
        case 12: r = cosh(x); break;
        case 13: r = exp(x); break;
        case 14: r = expm1(x); break;
        case 15: r = floor(x); break;
        case 16: r = (double)ajs_f32_round(x); break;
        case 17: r = f64_to_f16_to_f64(x); break;
        case 18: r = log(x); break;
        case 19: r = log1p(x); break;
        case 20: r = log10(x); break;
        case 21: r = log2(x); break;
        case 22: {
            if (isnan(x) || isinf(x) || x == 0.0) r = x;
            else if (x >= -0.5 && x < 0.0) r = -0.0;
            else {
                double fl = floor(x + 0.5);
                /* Exact tie for negative numbers: -1.5 + 0.5 == -1.0 -> -1.0; for -0.5000000000000001 -> -1.0 */
                if (x < 0.0 && (fl - x) > 0.5) fl -= 1.0;
                r = fl;
            }
            break;
        }
        case 23: r = isnan(x) ? NAN : (x == 0.0 ? x : (x < 0.0 ? -1.0 : 1.0)); break;
        case 24: r = sin(x); break;
        case 25: r = sinh(x); break;
        case 26: r = sqrt(x); break;
        case 27: r = tan(x); break;
        case 28: r = tanh(x); break;
        case 29: r = trunc(x); break;
        default: break;
    }
    return ajs_val_number(r);
}

static AjsValue js_math_atan2(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)t; (void)u;
    double y = NAN, x = NAN;
    if (c > 0 && !ajs_to_number(vm, v[0], &y)) return AJS_VAL_EXCEPTION;
    if (c > 1 && !ajs_to_number(vm, v[1], &x)) return AJS_VAL_EXCEPTION;
    return ajs_val_number(atan2(y, x));
}

static AjsValue js_math_imul(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)t; (void)u;
    uint32_t a = 0, b = 0;
    if (c > 0 && !ajs_to_uint32(vm, v[0], &a)) return AJS_VAL_EXCEPTION;
    if (c > 1 && !ajs_to_uint32(vm, v[1], &b)) return AJS_VAL_EXCEPTION;
    uint32_t prod = a * b;
    int32_t sprod;
    memcpy(&sprod, &prod, sizeof(sprod));
    return ajs_val_number((double)sprod);
}

static AjsValue js_math_pow(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)t; (void)u;
    double base = NAN, exp = NAN;
    if (c > 0 && !ajs_to_number(vm, v[0], &base)) return AJS_VAL_EXCEPTION;
    if (c > 1 && !ajs_to_number(vm, v[1], &exp)) return AJS_VAL_EXCEPTION;
    if (isnan(exp)) return AJS_VAL_NAN;
    if (exp == 0.0) return ajs_val_number(1.0);
    if (isnan(base)) return AJS_VAL_NAN;
    if (isinf(exp) && fabs(base) == 1.0) return AJS_VAL_NAN;
    return ajs_val_number(pow(base, exp));
}

static AjsValue js_math_minmax(AjsVm *vm, int argc, const AjsValue *argv, bool is_max) {
    double best = is_max ? -INFINITY : INFINITY;
    bool saw_nan = false;
    for (int i = 0; i < argc; i++) {
        double d = 0.0;
        if (!ajs_to_number(vm, argv[i], &d)) return AJS_VAL_EXCEPTION;
        if (isnan(d)) saw_nan = true;
        else if (!saw_nan) {
            if (d == 0.0 && best == 0.0) {
                bool d_neg = signbit(d) != 0;
                bool b_neg = signbit(best) != 0;
                if (is_max ? (!d_neg && b_neg) : (d_neg && !b_neg)) best = d;
            } else if (is_max ? (d > best) : (d < best)) {
                best = d;
            }
        }
    }
    return saw_nan ? AJS_VAL_NAN : ajs_val_number(best);
}
static AjsValue js_math_max(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)t; (void)u; return js_math_minmax(vm, c, v, true); }
static AjsValue js_math_min(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)t; (void)u; return js_math_minmax(vm, c, v, false); }

static AjsValue js_math_hypot(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    double max_v = 0.0;
    bool saw_inf = false, saw_nan = false;
    double *vals = argc > 0 ? (double *)malloc((size_t)argc * sizeof(double)) : NULL;
    if (argc > 0 && !vals) return ajs_throw(vm, vm->oom_error);
    for (int i = 0; i < argc; i++) {
        if (!ajs_to_number(vm, argv[i], &vals[i])) { free(vals); return AJS_VAL_EXCEPTION; }
        if (isinf(vals[i])) saw_inf = true;
        else if (isnan(vals[i])) saw_nan = true;
        else {
            double a = fabs(vals[i]);
            if (a > max_v) max_v = a;
        }
    }
    if (saw_inf) { free(vals); return ajs_val_number(INFINITY); }
    if (saw_nan) { free(vals); return AJS_VAL_NAN; }
    if (max_v == 0.0) { free(vals); return ajs_val_number(0.0); }
    double sum = 0.0;
    for (int i = 0; i < argc; i++) {
        double s = vals[i] / max_v;
        sum += s * s;
    }
    free(vals);
    return ajs_val_number(max_v * sqrt(sum));
}

static AjsValue js_math_random(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)t; (void)c; (void)v; (void)u;
    /* xorshift128+ PRNG yielding uniform IEEE-754 double in [0, 1) */
    uint64_t s1 = vm->rng_state[0];
    const uint64_t s0 = vm->rng_state[1];
    vm->rng_state[0] = s0;
    s1 ^= s1 << 23;
    vm->rng_state[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);
    uint64_t rnd = vm->rng_state[1] + s0;
    return ajs_val_number((double)(rnd >> 11) * (1.0 / 9007199254740992.0));
}

/* Symbol */
static bool this_sym_val(AjsVm *vm, AjsValue this_val, AjsSymbol **out_sym) {
    if (ajs_val_is_gc_kind(this_val, AJS_GC_SYMBOL)) {
        *out_sym = (AjsSymbol *)ajs_val_to_gc(this_val);
        return true;
    }
    if (ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        AjsObject *o = (AjsObject *)ajs_val_to_gc(this_val);
        if (o->kind == AJS_OBJ_SYMBOL_OBJ && ajs_val_is_gc_kind(o->u.prim_val, AJS_GC_SYMBOL)) {
            *out_sym = (AjsSymbol *)ajs_val_to_gc(o->u.prim_val);
            return true;
        }
    }
    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Symbol.prototype method called on incompatible receiver");
    return false;
}

static AjsValue js_symbol_call(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (ajs_get_native_new_target(vm) != AJS_VAL_UNDEFINED) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Symbol is not a constructor");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsString *desc = NULL;
    bool has_desc = (argc > 0 && argv[0] != AJS_VAL_UNDEFINED);
    if (has_desc) {
        desc = ajs_to_js_string(vm, argv[0]);
        if (!desc || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(desc));
    }
    AjsSymbol *sym = (AjsSymbol *)ajs_gc_alloc(vm, sizeof(AjsSymbol), AJS_GC_SYMBOL);
    ajs_temp_root_restore(vm, rs);
    if (!sym) return AJS_VAL_EXCEPTION;
    sym->id = vm->next_symbol_id++;
    sym->has_desc = has_desc;
    sym->desc = desc;
    return ajs_val_from_gc(sym);
}

static AjsValue js_symbol_for(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *key = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!key || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    key = ajs_str_intern(vm, key);
    ajs_temp_root_push(vm, ajs_val_from_gc(key));

    for (uint32_t i = 0; i < vm->sym_reg_count; i++) {
        if (ajs_str_eq(vm->sym_reg[i].key, key)) {
            ajs_temp_root_restore(vm, rs);
            return ajs_val_from_gc(vm->sym_reg[i].sym);
        }
    }
    AjsSymbol *sym = (AjsSymbol *)ajs_gc_alloc(vm, sizeof(AjsSymbol), AJS_GC_SYMBOL);
    if (!sym) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    sym->id = vm->next_symbol_id++;
    sym->has_desc = true;
    sym->is_registered = true;
    sym->desc = key;

    if (vm->sym_reg_count >= vm->sym_reg_cap) {
        uint32_t ncap = vm->sym_reg_cap ? vm->sym_reg_cap * 2u : 16u;
        AjsSymRegEntry *nb = (AjsSymRegEntry *)realloc(vm->sym_reg, (size_t)ncap * sizeof(AjsSymRegEntry));
        if (!nb) { ajs_temp_root_restore(vm, rs); return ajs_throw(vm, vm->oom_error); }
        vm->sym_reg = nb;
        vm->sym_reg_cap = ncap;
    }
    vm->sym_reg[vm->sym_reg_count].key = key;
    vm->sym_reg[vm->sym_reg_count].sym = sym;
    vm->sym_reg_count++;
    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(sym);
}

static AjsValue js_symbol_keyFor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 1 || !ajs_val_is_gc_kind(argv[0], AJS_GC_SYMBOL)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Symbol.keyFor requires a symbol argument");
    }
    AjsSymbol *sym = (AjsSymbol *)ajs_val_to_gc(argv[0]);
    if (!sym->is_registered || !sym->desc) return AJS_VAL_UNDEFINED;
    return ajs_val_from_gc(sym->desc);
}

static AjsValue js_symbol_proto_toString(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsSymbol *sym = NULL;
    if (!this_sym_val(vm, this_val, &sym)) return AJS_VAL_EXCEPTION;
    const char *d = (sym->has_desc && sym->desc) ? sym->desc->utf8 : "";
    size_t len = strlen(d) + 9u;
    char *buf = (char *)malloc(len);
    if (!buf) return ajs_throw(vm, vm->oom_error);
    snprintf(buf, len, "Symbol(%s)", d);
    AjsString *s = ajs_str_from_cstr(vm, buf, false);
    free(buf);
    return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
}
static AjsValue js_symbol_proto_valueOf(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsSymbol *sym = NULL;
    if (!this_sym_val(vm, this_val, &sym)) return AJS_VAL_EXCEPTION;
    return ajs_val_from_gc(sym);
}
static AjsValue js_symbol_proto_desc(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsSymbol *sym = NULL;
    if (!this_sym_val(vm, this_val, &sym)) return AJS_VAL_EXCEPTION;
    if (!sym->has_desc || !sym->desc) return AJS_VAL_UNDEFINED;
    return ajs_val_from_gc(sym->desc);
}

/* ============================================================================
 * 4. Date, Error (+ Subclasses), Reflect & Proxy Built-ins
 * ============================================================================ */

static double date_time_clip(double t) {
    if (!isfinite(t) || fabs(t) > 8.64e15) return NAN;
    double z = ajs_double_to_integer_or_inf(t);
    return z == 0.0 ? 0.0 : z;
}

static int64_t floor_div_i64(int64_t a, int64_t b) {
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) q--;
    return q;
}

static int64_t day_from_year(int64_t y) {
    return 365 * (y - 1970) + floor_div_i64(y - 1969, 4) - floor_div_i64(y - 1901, 100) + floor_div_i64(y - 1601, 400);
}

static bool is_leap_year(int64_t y) {
    return (y % 4 == 0) && ((y % 100 != 0) || (y % 400 == 0));
}

static void break_down_epoch_ms(double ms, int64_t *out_y, int *out_m, int *out_d,
                                int *out_wd, int *out_h, int *out_min, int *out_s, int *out_ms_rem) {
    int64_t t = (int64_t)ms;
    int64_t day = floor_div_i64(t, 86400000);
    int64_t tod = t - day * 86400000;
    *out_h = (int)(tod / 3600000);
    *out_min = (int)((tod / 60000) % 60);
    *out_s = (int)((tod / 1000) % 60);
    *out_ms_rem = (int)(tod % 1000);
    int64_t wd = (day + 4) % 7;
    if (wd < 0) wd += 7;
    *out_wd = (int)wd;

    /* Civil date from days since 1970-01-01 (Howard Hinnant exact proleptic Gregorian formula) */
    int64_t z = day + 719468;
    int64_t era = floor_div_i64(z, 146097);
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t y = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint64_t mp = (5u * doy + 2u) / 153u;
    unsigned d = (unsigned)(doy - (153u * mp + 2u) / 5u + 1u);
    unsigned m = (unsigned)(mp < 10u ? mp + 3u : mp - 9u);
    y += (m <= 2u) ? 1 : 0;
    *out_y = y;
    *out_m = (int)m - 1; /* 0-based month */
    *out_d = (int)d;
}

static double make_utc_ms(double yr, double mo, double dt, double hr, double mn, double sc, double ms, bool two_digit_1900) {
    if (!isfinite(yr) || !isfinite(mo) || !isfinite(dt) || !isfinite(hr) || !isfinite(mn) || !isfinite(sc) || !isfinite(ms)) {
        return NAN;
    }
    int64_t y = (int64_t)ajs_double_to_integer_or_inf(yr);
    if (two_digit_1900 && y >= 0 && y <= 99) y += 1900;
    int64_t m = (int64_t)ajs_double_to_integer_or_inf(mo);
    int64_t ym = y + floor_div_i64(m, 12);
    int64_t mm = m - floor_div_i64(m, 12) * 12;
    static const int k_mdays[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int64_t day = day_from_year(ym) + k_mdays[mm] + ((mm >= 2 && is_leap_year(ym)) ? 1 : 0) +
                  ((int64_t)ajs_double_to_integer_or_inf(dt) - 1);
    double time_ms = ajs_double_to_integer_or_inf(hr) * 3600000.0 +
                     ajs_double_to_integer_or_inf(mn) * 60000.0 +
                     ajs_double_to_integer_or_inf(sc) * 1000.0 +
                     ajs_double_to_integer_or_inf(ms);
    return (double)day * 86400000.0 + time_ms;
}

/* Deterministic UTC local offset (0 min in pure C11 UTC default realm) */
static double parse_date_string(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (!*s) return NAN;
    /* ISO 8601: [+-]YYYYYY-MM-DD or YYYY-MM-DD[THH:mm[:ss[.sss]][Z|+HH:mm|-HH:mm]] */
    const char *p = s;
    int ysign = 1;
    bool ext_year = false;
    if (*p == '+' || *p == '-') {
        ext_year = true;
        if (*p == '-') ysign = -1;
        p++;
    }
    int ydigits = 0;
    int64_t yr = 0;
    while (*p >= '0' && *p <= '9' && ydigits < 7) {
        yr = yr * 10 + (*p - '0');
        p++; ydigits++;
    }
    if ((ext_year && ydigits == 6 && !(ysign == -1 && yr == 0)) || (!ext_year && ydigits == 4)) {
        yr *= ysign;
        int mo = 1, dt = 1, hr = 0, mn = 0, sc = 0, ms = 0;
        if (*p == '-') {
            p++;
            if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return NAN;
            mo = (p[0] - '0') * 10 + (p[1] - '0');
            p += 2;
            if (mo < 1 || mo > 12) return NAN;
            if (*p == '-') {
                p++;
                if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return NAN;
                dt = (p[0] - '0') * 10 + (p[1] - '0');
                p += 2;
                if (dt < 1 || dt > 31) return NAN;
            }
        }
        int tz_min = 0;
        if (*p == 'T' || *p == 't' || *p == ' ') {
            p++;
            if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9' || p[2] != ':' ||
                p[3] < '0' || p[3] > '9' || p[4] < '0' || p[4] > '9') return NAN;
            hr = (p[0] - '0') * 10 + (p[1] - '0');
            mn = (p[3] - '0') * 10 + (p[4] - '0');
            p += 5;
            if (hr > 24 || mn > 59 || (hr == 24 && mn > 0)) return NAN;
            if (*p == ':') {
                p++;
                if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return NAN;
                sc = (p[0] - '0') * 10 + (p[1] - '0');
                p += 2;
                if (sc > 59 || (hr == 24 && sc > 0)) return NAN;
                if (*p == '.') {
                    p++;
                    if (*p < '0' || *p > '9') return NAN;
                    int fdig = 0;
                    while (*p >= '0' && *p <= '9') {
                        if (fdig == 0) ms += (*p - '0') * 100;
                        else if (fdig == 1) ms += (*p - '0') * 10;
                        else if (fdig == 2) ms += (*p - '0');
                        fdig++; p++;
                    }
                    if (hr == 24 && ms > 0) return NAN;
                }
            }
            if (*p == 'Z' || *p == 'z') {
                p++;
            } else if (*p == '+' || *p == '-') {
                int tzs = (*p == '-') ? -1 : 1;
                p++;
                if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return NAN;
                int tzh = (p[0] - '0') * 10 + (p[1] - '0');
                p += 2;
                if (*p == ':') p++;
                int tzm = 0;
                if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9') {
                    tzm = (p[0] - '0') * 10 + (p[1] - '0');
                    p += 2;
                }
                if (tzh > 23 || tzm > 59) return NAN;
                tz_min = tzs * (tzh * 60 + tzm);
            }
        }
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0') {
            double v = make_utc_ms((double)yr, (double)(mo - 1), (double)dt,
                                   (double)hr, (double)(mn - tz_min), (double)sc, (double)ms, false);
            return date_time_clip(v);
        }
    }

    /* Fallback: RFC 2822 / US slash or month-name date e.g. "Thu, 01 Jan 1970 00:00:00 GMT" or "1/15/2026" */
    static const char *const k_months[12] = {
        "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
    };
    int m1 = 0, d1 = 0, y1 = 0;
    if (sscanf(s, "%d/%d/%d", &m1, &d1, &y1) == 3 && m1 >= 1 && m1 <= 12 && d1 >= 1 && d1 <= 31) {
        return date_time_clip(make_utc_ms(y1, m1 - 1, d1, 0, 0, 0, 0, y1 >= 0 && y1 < 100));
    }
    for (int m = 0; m < 12; m++) {
        const char *mp = strstr(s, k_months[m]);
        if (mp) {
            /* Check "DD Mon YYYY HH:MM:SS" or "Mon DD YYYY HH:MM:SS" */
            int day = 1, yr2 = 1970, hh = 0, mm = 0, ss = 0;
            if (mp > s && sscanf(s, "%*[^0-9]%d", &day) == 1 && day >= 1 && day <= 31 && mp > s + 2) {
                if (sscanf(mp + 3, " %d %d:%d:%d", &yr2, &hh, &mm, &ss) >= 1) {
                    return date_time_clip(make_utc_ms(yr2, m, day, hh, mm, ss, 0, false));
                }
            }
            if (sscanf(mp + 3, " %d %d %d:%d:%d", &day, &yr2, &hh, &mm, &ss) >= 2 ||
                sscanf(mp + 3, " %d, %d %d:%d:%d", &day, &yr2, &hh, &mm, &ss) >= 2) {
                return date_time_clip(make_utc_ms(yr2, m, day, hh, mm, ss, 0, false));
            }
        }
    }
    return NAN;
}

static double current_epoch_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        return floor((double)ts.tv_sec * 1000.0 + (double)(ts.tv_nsec / 1000000L));
    }
    return (double)time(NULL) * 1000.0;
}

static void format_date_str(double ms, int style, char *buf, size_t cap) {
    /* style: 0=toString, 1=toUTCString, 2=toDateString, 3=toTimeString */
    if (isnan(ms)) {
        snprintf(buf, cap, "Invalid Date");
        return;
    }
    static const char *const k_wd[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    static const char *const k_mo[12] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
    int64_t y; int m, d, wd, h, mn, s, rem;
    break_down_epoch_ms(ms, &y, &m, &d, &wd, &h, &mn, &s, &rem);
    if (style == 1) {
        snprintf(buf, cap, "%s, %02d %s %04lld %02d:%02d:%02d GMT",
                 k_wd[wd], d, k_mo[m], (long long)y, h, mn, s);
    } else if (style == 2) {
        snprintf(buf, cap, "%s %s %02d %04lld", k_wd[wd], k_mo[m], d, (long long)y);
    } else if (style == 3) {
        snprintf(buf, cap, "%02d:%02d:%02d GMT+0000 (UTC)", h, mn, s);
    } else {
        snprintf(buf, cap, "%s %s %02d %04lld %02d:%02d:%02d GMT+0000 (UTC)",
                 k_wd[wd], k_mo[m], d, (long long)y, h, mn, s);
    }
}

static bool this_date_ms(AjsVm *vm, AjsValue this_val, AjsObject **out_obj, double *out_ms) {
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Date.prototype method called on non-Date");
        return false;
    }
    AjsObject *o = (AjsObject *)ajs_val_to_gc(this_val);
    if (o->kind != AJS_OBJ_DATE) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Date.prototype method called on non-Date");
        return false;
    }
    if (out_obj) *out_obj = o;
    if (out_ms) *out_ms = o->u.date_ms;
    return true;
}

static AjsValue js_date_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt == AJS_VAL_UNDEFINED) {
        char buf[128];
        format_date_str(current_epoch_ms(), 0, buf, sizeof(buf));
        return ajs_val_from_gc(ajs_str_from_cstr(vm, buf, false));
    }
    double dv = NAN;
    if (argc == 0) {
        dv = current_epoch_ms();
    } else if (argc == 1) {
        if (ajs_val_is_gc_kind(argv[0], AJS_GC_OBJECT) &&
            ((AjsObject *)ajs_val_to_gc(argv[0]))->kind == AJS_OBJ_DATE) {
            dv = ((AjsObject *)ajs_val_to_gc(argv[0]))->u.date_ms;
        } else {
            AjsValue prim = ajs_to_primitive(vm, argv[0], 0);
            if (vm->has_exception) return AJS_VAL_EXCEPTION;
            if (ajs_val_is_gc_kind(prim, AJS_GC_STRING)) {
                dv = parse_date_string(((AjsString *)ajs_val_to_gc(prim))->utf8);
            } else {
                double n = 0.0;
                if (!ajs_to_number(vm, prim, &n)) return AJS_VAL_EXCEPTION;
                dv = date_time_clip(n);
            }
        }
    } else {
        double f[7] = { 0, 0, 1, 0, 0, 0, 0 };
        int lim = argc < 7 ? argc : 7;
        for (int i = 0; i < lim; i++) {
            if (!ajs_to_number(vm, argv[i], &f[i])) return AJS_VAL_EXCEPTION;
        }
        dv = date_time_clip(make_utc_ms(f[0], f[1], f[2], f[3], f[4], f[5], f[6], true));
    }
    AjsValue proto = get_proto_from_ctor(vm, nt, vm->active_realm->date_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    AjsObject *o = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_DATE, proto);
    if (!o) return AJS_VAL_EXCEPTION;
    o->u.date_ms = dv;
    return ajs_val_from_gc(o);
}

static AjsValue js_date_now(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)vm; (void)t; (void)c; (void)v; (void)u;
    return ajs_val_number(current_epoch_ms());
}
static AjsValue js_date_parse(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)t; (void)u;
    AjsString *s = ajs_to_js_string(vm, c > 0 ? v[0] : AJS_VAL_UNDEFINED);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    return ajs_val_number(parse_date_string(s->utf8));
}
static AjsValue js_date_UTC(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    double f[7] = { NAN, 0, 1, 0, 0, 0, 0 };
    int lim = argc < 7 ? argc : 7;
    for (int i = 0; i < lim; i++) {
        if (!ajs_to_number(vm, argv[i], &f[i])) return AJS_VAL_EXCEPTION;
    }
    if (argc == 0) return AJS_VAL_NAN;
    return ajs_val_number(date_time_clip(make_utc_ms(f[0], f[1], f[2], f[3], f[4], f[5], f[6], true)));
}

static AjsValue js_date_getter(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    double ms = 0.0;
    if (!this_date_ms(vm, this_val, NULL, &ms)) return AJS_VAL_EXCEPTION;
    int32_t op = vm->call_stack[vm->call_depth - 1u].native_fn_obj->u.fn.native_magic;
    if (op == 0) return ajs_val_number(ms); /* getTime / valueOf */
    if (isnan(ms)) return AJS_VAL_NAN;
    if (op == 9) return ajs_val_number(0.0); /* getTimezoneOffset (UTC = +0) */
    int64_t y; int m, d, wd, h, mn, s, rem;
    break_down_epoch_ms(ms, &y, &m, &d, &wd, &h, &mn, &s, &rem);
    switch (op) {
        case 1: return ajs_val_number((double)y);
        case 2: return ajs_val_number((double)m);
        case 3: return ajs_val_number((double)d);
        case 4: return ajs_val_number((double)wd);
        case 5: return ajs_val_number((double)h);
        case 6: return ajs_val_number((double)mn);
        case 7: return ajs_val_number((double)s);
        case 8: return ajs_val_number((double)rem);
        case 10: return ajs_val_number((double)(y - 1900)); /* Annex B getYear */
        default: return AJS_VAL_NAN;
    }
}

static AjsValue js_date_setter(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *o = NULL;
    double ms = 0.0;
    if (!this_date_ms(vm, this_val, &o, &ms)) return AJS_VAL_EXCEPTION;
    int32_t op = vm->call_stack[vm->call_depth - 1u].native_fn_obj->u.fn.native_magic;
    if (op == 0) { /* setTime */
        double t = NAN;
        if (argc > 0 && !ajs_to_number(vm, argv[0], &t)) return AJS_VAL_EXCEPTION;
        o->u.date_ms = date_time_clip(t);
        return ajs_val_number(o->u.date_ms);
    }
    int64_t y = 1970; int m = 0, d = 1, wd = 4, h = 0, mn = 0, s = 0, rem = 0;
    if (!isnan(ms)) break_down_epoch_ms(ms, &y, &m, &d, &wd, &h, &mn, &s, &rem);
    else if (op != 7 && op != 8) {
        /* Per ECMA-262: still evaluate arguments ToNumber before returning NaN */
        double dummy = 0.0;
        for (int i = 0; i < argc && i < 4; i++) if (!ajs_to_number(vm, argv[i], &dummy)) return AJS_VAL_EXCEPTION;
        return AJS_VAL_NAN;
    }
    double yf = (double)y, mf = (double)m, df = (double)d, hf = (double)h, mnf = (double)mn, sf = (double)s, msf = (double)rem;
    switch (op) {
        case 1: /* setMilliseconds */
            if (!ajs_to_number(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &msf)) return AJS_VAL_EXCEPTION;
            break;
        case 2: /* setSeconds(sec [, ms]) */
            if (!ajs_to_number(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &sf)) return AJS_VAL_EXCEPTION;
            if (argc > 1 && !ajs_to_number(vm, argv[1], &msf)) return AJS_VAL_EXCEPTION;
            break;
        case 3: /* setMinutes(min [, sec [, ms]]) */
            if (!ajs_to_number(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &mnf)) return AJS_VAL_EXCEPTION;
            if (argc > 1 && !ajs_to_number(vm, argv[1], &sf)) return AJS_VAL_EXCEPTION;
            if (argc > 2 && !ajs_to_number(vm, argv[2], &msf)) return AJS_VAL_EXCEPTION;
            break;
        case 4: /* setHours(hr [, min [, sec [, ms]]]) */
            if (!ajs_to_number(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &hf)) return AJS_VAL_EXCEPTION;
            if (argc > 1 && !ajs_to_number(vm, argv[1], &mnf)) return AJS_VAL_EXCEPTION;
            if (argc > 2 && !ajs_to_number(vm, argv[2], &sf)) return AJS_VAL_EXCEPTION;
            if (argc > 3 && !ajs_to_number(vm, argv[3], &msf)) return AJS_VAL_EXCEPTION;
            break;
        case 5: /* setDate(date) */
            if (!ajs_to_number(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &df)) return AJS_VAL_EXCEPTION;
            break;
        case 6: /* setMonth(month [, date]) */
            if (!ajs_to_number(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &mf)) return AJS_VAL_EXCEPTION;
            if (argc > 1 && !ajs_to_number(vm, argv[1], &df)) return AJS_VAL_EXCEPTION;
            break;
        case 7: /* setFullYear(year [, month [, date]]) */
            if (!ajs_to_number(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &yf)) return AJS_VAL_EXCEPTION;
            if (argc > 1 && !ajs_to_number(vm, argv[1], &mf)) return AJS_VAL_EXCEPTION;
            if (argc > 2 && !ajs_to_number(vm, argv[2], &df)) return AJS_VAL_EXCEPTION;
            break;
        case 8: /* Annex B setYear(year) */
            if (!ajs_to_number(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &yf)) return AJS_VAL_EXCEPTION;
            if (isnan(yf)) { o->u.date_ms = NAN; return AJS_VAL_NAN; }
            {
                double yi = ajs_double_to_integer_or_inf(yf);
                yf = (yi >= 0.0 && yi <= 99.0) ? (1900.0 + yi) : yi;
            }
            break;
        default: break;
    }
    o->u.date_ms = date_time_clip(make_utc_ms(yf, mf, df, hf, mnf, sf, msf, false));
    return ajs_val_number(o->u.date_ms);
}

static AjsValue js_date_toISOString(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    double ms = 0.0;
    if (!this_date_ms(vm, this_val, NULL, &ms)) return AJS_VAL_EXCEPTION;
    if (!isfinite(ms)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid time value");
    int64_t y; int m, d, wd, h, mn, s, rem;
    break_down_epoch_ms(ms, &y, &m, &d, &wd, &h, &mn, &s, &rem);
    char buf[64];
    if (y < 0) {
        snprintf(buf, sizeof(buf), "-%06lld-%02d-%02dT%02d:%02d:%02d.%03dZ",
                 (long long)(-y), m + 1, d, h, mn, s, rem);
    } else if (y > 9999) {
        snprintf(buf, sizeof(buf), "+%06lld-%02d-%02dT%02d:%02d:%02d.%03dZ",
                 (long long)y, m + 1, d, h, mn, s, rem);
    } else {
        snprintf(buf, sizeof(buf), "%04lld-%02d-%02dT%02d:%02d:%02d.%03dZ",
                 (long long)y, m + 1, d, h, mn, s, rem);
    }
    return ajs_val_from_gc(ajs_str_from_cstr(vm, buf, false));
}

static AjsValue js_date_format(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    double ms = 0.0;
    if (!this_date_ms(vm, this_val, NULL, &ms)) return AJS_VAL_EXCEPTION;
    int32_t style = vm->call_stack[vm->call_depth - 1u].native_fn_obj->u.fn.native_magic;
    char buf[128];
    format_date_str(ms, style, buf, sizeof(buf));
    return ajs_val_from_gc(ajs_str_from_cstr(vm, buf, false));
}

static AjsValue js_date_toJSON(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsObject *o = ajs_to_object(vm, this_val);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue ov = ajs_val_from_gc(o);
    ajs_temp_root_push(vm, ov);
    AjsValue tv = ajs_to_primitive(vm, ov, 2);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (ajs_val_is_number(tv) && !isfinite(ajs_val_to_number_unchecked(tv))) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_NULL;
    }
    AjsString *k = ajs_str_from_cstr(vm, "toISOString", true);
    AjsValue fn = ajs_obj_get(vm, ov, ajs_val_from_gc(k), ov);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (!ajs_is_callable(vm, fn)) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "toISOString is not a function");
    }
    AjsValue res = ajs_call_internal(vm, fn, ov, 0, NULL);
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_date_toPrimitive(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Date.prototype[Symbol.toPrimitive] called on non-object");
    }
    if (argc < 1 || !ajs_val_is_gc_kind(argv[0], AJS_GC_STRING)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Invalid hint in Date.prototype[Symbol.toPrimitive]");
    }
    const AjsString *hs = (const AjsString *)ajs_val_to_gc(argv[0]);
    int hint = -1;
    if (ajs_str_eq_cstr(hs, "string") || ajs_str_eq_cstr(hs, "default")) hint = 1;
    else if (ajs_str_eq_cstr(hs, "number")) hint = 2;
    if (hint < 0) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Invalid hint in Date.prototype[Symbol.toPrimitive]");

    /* OrdinaryToPrimitive(O, hint) */
    AjsString *m0 = (hint == 1) ? vm->str_toString : vm->str_valueOf;
    AjsString *m1 = (hint == 1) ? vm->str_valueOf  : vm->str_toString;
    AjsString *ms[2] = { m0, m1 };
    for (int i = 0; i < 2; i++) {
        AjsValue fn = ajs_obj_get(vm, this_val, ajs_val_from_gc(ms[i]), this_val);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        if (ajs_is_callable(vm, fn)) {
            AjsValue r = ajs_call_internal(vm, fn, this_val, 0, NULL);
            if (vm->has_exception) return AJS_VAL_EXCEPTION;
            if (!ajs_val_is_gc_kind(r, AJS_GC_OBJECT)) return r;
        }
    }
    return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot convert object to primitive value");
}

/* Error + All Standard Subclasses + AggregateError + SuppressedError + Error.isError */
static const char *const k_err_names_tbl[AJS_ERR_KIND_COUNT] = {
    "Error", "EvalError", "RangeError", "ReferenceError",
    "SyntaxError", "TypeError", "URIError", "AggregateError", "SuppressedError"
};

static void install_error_cause(AjsVm *vm, AjsObject *eobj, AjsValue options_val) {
    if (!ajs_val_is_gc_kind(options_val, AJS_GC_OBJECT)) return;
    bool has = false;
    if (!ajs_obj_has_prop(vm, (AjsObject *)ajs_val_to_gc(options_val), ajs_val_from_gc(vm->str_cause), &has) || !has) return;
    AjsValue cv = ajs_obj_get(vm, options_val, ajs_val_from_gc(vm->str_cause), options_val);
    if (vm->has_exception) return;
    def_data_prop(vm, ajs_val_from_gc(eobj), "cause", cv, AJS_DESC_WRITABLE | AJS_DESC_CONFIGURABLE);
}

static AjsValue js_error_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    int32_t magic = vm->call_stack[vm->call_depth - 1u].native_fn_obj->u.fn.native_magic;
    int kind_idx = (-magic) - 100;
    if (kind_idx < 0 || kind_idx >= AJS_ERR_KIND_COUNT) kind_idx = 0;

    AjsRealm *r = vm->active_realm;
    AjsValue nt = ajs_get_native_new_target(vm);
    AjsValue proto = r->error_proto[kind_idx];
    if (nt != AJS_VAL_UNDEFINED) {
        proto = get_proto_from_ctor(vm, nt, r->error_proto[kind_idx]);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
    }

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, proto);
    AjsObject *eobj = ajs_obj_new(vm, r, AJS_OBJ_ERROR, proto);
    if (!eobj) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsValue ev = ajs_val_from_gc(eobj);
    ajs_temp_root_push(vm, ev);
    snprintf(eobj->u.err.error_name, sizeof(eobj->u.err.error_name), "%s", k_err_names_tbl[kind_idx]);

    if (kind_idx == AJS_ERR_KIND_AGGREGATE) {
        /* AggregateError(errors, message [, options]) */
        AjsValue errs_arg = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
        if (argc > 1 && argv[1] != AJS_VAL_UNDEFINED) {
            AjsString *ms = ajs_to_js_string(vm, argv[1]);
            if (!ms || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            def_data_prop(vm, ev, "message", ajs_val_from_gc(ms), AJS_DESC_WRITABLE | AJS_DESC_CONFIGURABLE);
        }
        if (argc > 2) {
            install_error_cause(vm, eobj, argv[2]);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
        /* IterableToList(errors) */
        AjsValue list = ajs_new_array(vm, 0);
        ajs_temp_root_push(vm, list);
        if (ajs_is_nullish(errs_arg)) {
            ajs_temp_root_restore(vm, rs);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "AggregateError errors must be iterable");
        }
        AjsValue it_fn = ajs_obj_get(vm, errs_arg, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), errs_arg);
        if (vm->has_exception || !ajs_is_callable(vm, it_fn)) {
            ajs_temp_root_restore(vm, rs);
            if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "AggregateError errors is not iterable");
            return AJS_VAL_EXCEPTION;
        }
        AjsValue iter = ajs_call_internal(vm, it_fn, errs_arg, 0, NULL);
        if (vm->has_exception || !ajs_is_object(vm, iter)) {
            ajs_temp_root_restore(vm, rs);
            if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator must be an object");
            return AJS_VAL_EXCEPTION;
        }
        ajs_temp_root_push(vm, iter);
        AjsValue next_fn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_next), iter);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, next_fn);
        uint32_t idx = 0;
        for (;;) {
            AjsValue step = ajs_call_internal(vm, next_fn, iter, 0, NULL);
            if (vm->has_exception || !ajs_is_object(vm, step)) {
                ajs_temp_root_restore(vm, rs);
                if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator result is not an object");
                return AJS_VAL_EXCEPTION;
            }
            size_t ls = ajs_temp_root_save(vm);
            ajs_temp_root_push(vm, step);
            AjsValue dn = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_done), step);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            if (ajs_to_bool(vm, dn)) { ajs_temp_root_restore(vm, ls); break; }
            AjsValue iv = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            ajs_temp_root_push(vm, iv);
            ajs_set_prop_index(vm, list, idx++, iv);
            ajs_temp_root_restore(vm, ls);
        }
        def_data_prop(vm, ev, "errors", list, AJS_DESC_WRITABLE | AJS_DESC_CONFIGURABLE);
    } else if (kind_idx == AJS_ERR_KIND_SUPPRESSED) {
        /* SuppressedError(error, suppressed [, message]) */
        if (argc > 2 && argv[2] != AJS_VAL_UNDEFINED) {
            AjsString *ms = ajs_to_js_string(vm, argv[2]);
            if (!ms || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            def_data_prop(vm, ev, "message", ajs_val_from_gc(ms), AJS_DESC_WRITABLE | AJS_DESC_CONFIGURABLE);
        }
        def_data_prop(vm, ev, "error", argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, AJS_DESC_WRITABLE | AJS_DESC_CONFIGURABLE);
        def_data_prop(vm, ev, "suppressed", argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, AJS_DESC_WRITABLE | AJS_DESC_CONFIGURABLE);
    } else {
        if (argc > 0 && argv[0] != AJS_VAL_UNDEFINED) {
            AjsString *ms = ajs_to_js_string(vm, argv[0]);
            if (!ms || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            def_data_prop(vm, ev, "message", ajs_val_from_gc(ms), AJS_DESC_WRITABLE | AJS_DESC_CONFIGURABLE);
        }
        if (argc > 1) {
            install_error_cause(vm, eobj, argv[1]);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
    }

    ajs_capture_stack_trace(vm, eobj, NULL, 0, 0);
    ajs_temp_root_restore(vm, rs);
    return ev;
}

static AjsValue js_error_isError(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)vm; (void)t; (void)u;
    if (c < 1 || !ajs_val_is_gc_kind(v[0], AJS_GC_OBJECT)) return AJS_VAL_FALSE;
    return ajs_bool(((AjsObject *)ajs_val_to_gc(v[0]))->kind == AJS_OBJ_ERROR);
}

static AjsValue js_error_proto_toString(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Error.prototype.toString called on non-object");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue nv = ajs_obj_get(vm, this_val, ajs_val_from_gc(vm->str_name), this_val);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    AjsString *ns = (nv == AJS_VAL_UNDEFINED) ? ajs_str_from_cstr(vm, "Error", true) : ajs_to_js_string(vm, nv);
    if (!ns || vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(ns));

    AjsValue mv = ajs_obj_get(vm, this_val, ajs_val_from_gc(vm->str_message), this_val);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsString *ms = (mv == AJS_VAL_UNDEFINED) ? vm->str_empty : ajs_to_js_string(vm, mv);
    if (!ms || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(ms));

    AjsString *res = NULL;
    if (ns->len16 == 0) res = ms;
    else if (ms->len16 == 0) res = ns;
    else {
        AjsString *sep = ajs_str_from_cstr(vm, ": ", true);
        AjsString *t1 = ajs_str_concat(vm, ns, sep);
        ajs_temp_root_push(vm, ajs_val_from_gc(t1));
        res = ajs_str_concat(vm, t1, ms);
    }
    ajs_temp_root_restore(vm, rs);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}

/* Reflect (all 13 methods) */
static AjsObject *require_reflect_target(AjsVm *vm, int argc, const AjsValue *argv) {
    if (argc < 1 || !ajs_val_is_gc_kind(argv[0], AJS_GC_OBJECT)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Reflect target must be an Object");
        return NULL;
    }
    return (AjsObject *)ajs_val_to_gc(argv[0]);
}

static AjsValue js_reflect_apply(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    AjsValue fn = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_is_callable(vm, fn)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Reflect.apply target is not callable");
    AjsValue recv = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    if (argc < 3 || !ajs_val_is_gc_kind(argv[2], AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Reflect.apply argumentsList must be an Object");
    }
    return create_list_from_array_like_call(vm, fn, recv, argv[2], false, AJS_VAL_UNDEFINED);
}

static AjsValue js_reflect_construct(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    AjsValue target = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_is_constructor_val(target)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Reflect.construct target is not a constructor");
    }
    AjsValue nt = (argc > 2) ? argv[2] : target;
    if (!ajs_is_constructor_val(nt)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Reflect.construct newTarget is not a constructor");
    }
    if (argc < 2 || !ajs_val_is_gc_kind(argv[1], AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Reflect.construct argumentsList must be an Object");
    }
    return create_list_from_array_like_call(vm, target, AJS_VAL_UNDEFINED, argv[1], true, nt);
}

static AjsValue js_reflect_defineProperty(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    AjsObject *o = require_reflect_target(vm, argc, argv);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue k = ajs_to_property_key(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, k);
    AjsPropDesc d;
    if (!to_prop_desc(vm, argc > 2 ? argv[2] : AJS_VAL_UNDEFINED, &d)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (d.flags & AJS_DESC_HAS_VALUE) ajs_temp_root_push(vm, d.value);
    if (d.flags & AJS_DESC_HAS_GET) ajs_temp_root_push(vm, d.getter);
    if (d.flags & AJS_DESC_HAS_SET) ajs_temp_root_push(vm, d.setter);
    bool ok = ajs_obj_define_own_prop(vm, o, k, &d, false);
    ajs_temp_root_restore(vm, rs);
    return vm->has_exception ? AJS_VAL_EXCEPTION : ajs_bool(ok);
}

static AjsValue js_reflect_deleteProperty(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    AjsObject *o = require_reflect_target(vm, argc, argv);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue k = ajs_to_property_key(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, k);
    bool ok = ajs_obj_delete(vm, o, k, false);
    ajs_temp_root_restore(vm, rs);
    return vm->has_exception ? AJS_VAL_EXCEPTION : ajs_bool(ok);
}

static AjsValue js_reflect_get(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    if (!require_reflect_target(vm, argc, argv)) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue k = ajs_to_property_key(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, k);
    AjsValue recv = argc > 2 ? argv[2] : argv[0];
    AjsValue res = ajs_obj_get(vm, argv[0], k, recv);
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_reflect_getOwnPropertyDescriptor(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    AjsObject *o = require_reflect_target(vm, argc, argv);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue k = ajs_to_property_key(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, k);
    AjsPropDesc d;
    bool found = false;
    if (!ajs_obj_get_own_desc(vm, o, k, &d, &found)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsValue res = found ? from_prop_desc(vm, vm->active_realm, &d) : AJS_VAL_UNDEFINED;
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_reflect_getPrototypeOf(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    AjsObject *o = require_reflect_target(vm, argc, argv);
    if (!o) return AJS_VAL_EXCEPTION;
    AjsValue p = AJS_VAL_NULL;
    return ajs_obj_get_proto(vm, o, &p) ? p : AJS_VAL_EXCEPTION;
}

static AjsValue js_reflect_has(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    AjsObject *o = require_reflect_target(vm, argc, argv);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue k = ajs_to_property_key(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, k);
    bool has = false;
    bool ok = ajs_obj_has_prop(vm, o, k, &has);
    ajs_temp_root_restore(vm, rs);
    return ok ? ajs_bool(has) : AJS_VAL_EXCEPTION;
}

static AjsValue js_reflect_isExtensible(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    AjsObject *o = require_reflect_target(vm, argc, argv);
    if (!o) return AJS_VAL_EXCEPTION;
    bool ext = false;
    return ajs_obj_is_extensible(vm, o, &ext) ? ajs_bool(ext) : AJS_VAL_EXCEPTION;
}

static AjsValue js_reflect_ownKeys(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    AjsObject *o = require_reflect_target(vm, argc, argv);
    if (!o) return AJS_VAL_EXCEPTION;
    AjsValue *keys = NULL;
    uint32_t kcount = 0;
    if (!ajs_obj_own_keys(vm, o, false, true, true, &keys, &kcount)) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    for (uint32_t i = 0; i < kcount; i++) ajs_temp_root_push(vm, keys[i]);
    AjsValue arr = ajs_new_array(vm, kcount);
    ajs_temp_root_push(vm, arr);
    for (uint32_t i = 0; i < kcount; i++) ajs_set_prop_index(vm, arr, i, keys[i]);
    free(keys);
    ajs_temp_root_restore(vm, rs);
    return arr;
}

static AjsValue js_reflect_preventExtensions(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    AjsObject *o = require_reflect_target(vm, argc, argv);
    if (!o) return AJS_VAL_EXCEPTION;
    bool ok = ajs_obj_prevent_extensions(vm, o, false);
    return vm->has_exception ? AJS_VAL_EXCEPTION : ajs_bool(ok);
}

static AjsValue js_reflect_set(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    if (!require_reflect_target(vm, argc, argv)) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue k = ajs_to_property_key(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, k);
    AjsValue val = argc > 2 ? argv[2] : AJS_VAL_UNDEFINED;
    AjsValue recv = argc > 3 ? argv[3] : argv[0];
    bool ok = ajs_obj_set(vm, argv[0], k, val, recv, false);
    ajs_temp_root_restore(vm, rs);
    return vm->has_exception ? AJS_VAL_EXCEPTION : ajs_bool(ok);
}

static AjsValue js_reflect_setPrototypeOf(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    AjsObject *o = require_reflect_target(vm, argc, argv);
    if (!o) return AJS_VAL_EXCEPTION;
    AjsValue proto = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    if (proto != AJS_VAL_NULL && !ajs_val_is_gc_kind(proto, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object prototype may only be an Object or null");
    }
    bool ok = ajs_obj_set_proto(vm, o, proto, false);
    return vm->has_exception ? AJS_VAL_EXCEPTION : ajs_bool(ok);
}

/* Proxy & Proxy.revocable */
static AjsValue create_proxy_obj(AjsVm *vm, AjsValue target, AjsValue handler) {
    if (!ajs_val_is_gc_kind(target, AJS_GC_OBJECT) || !ajs_val_is_gc_kind(handler, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot create proxy with a non-object as target or handler");
    }
    AjsObject *p = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_PROXY, AJS_VAL_NULL);
    if (!p) return AJS_VAL_EXCEPTION;
    p->u.proxy.target = target;
    p->u.proxy.handler = handler;
    p->u.proxy.is_callable = ajs_is_callable(vm, target);
    p->u.proxy.is_constructor = ajs_is_constructor_val(target);
    return ajs_val_from_gc(p);
}

static AjsValue js_proxy_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (ajs_get_native_new_target(vm) == AJS_VAL_UNDEFINED) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Constructor Proxy requires 'new'");
    }
    return create_proxy_obj(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED,
                            argc > 1 ? argv[1] : AJS_VAL_UNDEFINED);
}

static AjsValue js_proxy_revoke_cb(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)argc; (void)argv; (void)ud;
    AjsObject *fn = vm->call_stack[vm->call_depth - 1u].native_fn_obj;
    if (!fn) return AJS_VAL_UNDEFINED;
    AjsString *k = ajs_str_from_cstr(vm, "[[RevocableProxy]]", true);
    AjsPropDesc d;
    bool found = false;
    if (ajs_obj_get_own_desc(vm, fn, ajs_val_from_gc(k), &d, &found) && found &&
        ajs_val_is_gc_kind(d.value, AJS_GC_OBJECT)) {
        AjsObject *p = (AjsObject *)ajs_val_to_gc(d.value);
        if (p->kind == AJS_OBJ_PROXY) {
            p->u.proxy.target = AJS_VAL_NULL;
            p->u.proxy.handler = AJS_VAL_NULL;
        }
        AjsPropDesc nd = { .flags = AJS_DESC_HAS_VALUE, .value = AJS_VAL_NULL };
        ajs_obj_define_own_prop(vm, fn, ajs_val_from_gc(k), &nd, false);
    }
    return AJS_VAL_UNDEFINED;
}

static AjsValue js_proxy_revocable(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue p = create_proxy_obj(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED,
                                  argc > 1 ? argv[1] : AJS_VAL_UNDEFINED);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, p);

    AjsValue rev_fn = make_builtin_fn(vm, vm->active_realm, js_proxy_revoke_cb, "", 0, false, 0,
                                      vm->active_realm->function_proto);
    ajs_temp_root_push(vm, rev_fn);
    def_data_prop(vm, rev_fn, "[[RevocableProxy]]", p, AJS_DESC_WRITABLE);

    AjsObject *res = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ORDINARY, vm->active_realm->object_proto);
    AjsValue rv = ajs_val_from_gc(res);
    ajs_temp_root_push(vm, rv);
    uint32_t f = AJS_DESC_WRITABLE | AJS_DESC_ENUMERABLE | AJS_DESC_CONFIGURABLE;
    def_data_prop(vm, rv, "proxy", p, f);
    def_data_prop(vm, rv, "revoke", rev_fn, f);
    ajs_temp_root_restore(vm, rs);
    return rv;
}

/* ============================================================================
 * 5. Global Functions (eval, parseInt/Float, URI, Annex B) & Realm Init
 * ============================================================================ */

static AjsValue js_global_eval(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc == 0) return AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(argv[0], AJS_GC_STRING)) return argv[0];
    AjsString *src = (AjsString *)ajs_val_to_gc(argv[0]);
    /* Indirect eval: executes in global scope of realm owning this intrinsic eval */
    AjsRealm *r = vm->active_realm ? vm->active_realm : vm->main_realm;
    if (vm->call_depth > 0 && vm->call_stack[vm->call_depth - 1u].native_fn_obj &&
        vm->call_stack[vm->call_depth - 1u].native_fn_obj->realm) {
        r = vm->call_stack[vm->call_depth - 1u].native_fn_obj->realm;
    }
    AjsErrorInfo err;
    memset(&err, 0, sizeof(err));
    AjsBytecode *bc = ajs_compile_script(vm, r, src->utf8, src->len8, "<eval>", false, true, NULL, &err);
    if (!bc) {
        if (!vm->has_exception) {
            ajs_throw_error(vm, err.name[0] ? err.name : "SyntaxError", "%s",
                            err.message[0] ? err.message : "Syntax error in eval");
        }
        return AJS_VAL_EXCEPTION;
    }
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(bc));
    AjsValue res = ajs_exec_script_bc(vm, r, bc, NULL, r->global_obj);
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_global_isNaN(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)t; (void)u;
    double d = NAN;
    if (c > 0 && !ajs_to_number(vm, v[0], &d)) return AJS_VAL_EXCEPTION;
    return ajs_bool(isnan(d));
}
static AjsValue js_global_isFinite(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)t; (void)u;
    double d = NAN;
    if (c > 0 && !ajs_to_number(vm, v[0], &d)) return AJS_VAL_EXCEPTION;
    return ajs_bool(isfinite(d));
}
static AjsValue js_global_parseInt(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)t; (void)u;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = ajs_to_js_string(vm, c > 0 ? v[0] : AJS_VAL_UNDEFINED);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    int32_t radix = 0;
    if (c > 1 && v[1] != AJS_VAL_UNDEFINED) {
        if (!ajs_to_int32(vm, v[1], &radix)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    }
    double res = ajs_parse_int_str(s->units, s->len16, radix);
    ajs_temp_root_restore(vm, rs);
    return ajs_val_number(res);
}
static AjsValue js_global_parseFloat(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)t; (void)u;
    AjsString *s = ajs_to_js_string(vm, c > 0 ? v[0] : AJS_VAL_UNDEFINED);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    return ajs_val_number(ajs_parse_float_str(s->units, s->len16));
}

/* URI Encode / Decode (ECMA-262 §19.2.6) */
static bool uri_unescaped(uint32_t cp, bool is_component) {
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) return true;
    if (cp == '-' || cp == '_' || cp == '.' || cp == '!' || cp == '~' || cp == '*' || cp == '\'' || cp == '(' || cp == ')') return true;
    if (!is_component) {
        if (cp == ';' || cp == '/' || cp == '?' || cp == ':' || cp == '@' || cp == '&' ||
            cp == '=' || cp == '+' || cp == '$' || cp == ',' || cp == '#') return true;
    }
    return false;
}
static bool uri_reserved_or_hash(uint32_t cp) {
    return cp == ';' || cp == '/' || cp == '?' || cp == ':' || cp == '@' || cp == '&' ||
           cp == '=' || cp == '+' || cp == '$' || cp == ',' || cp == '#';
}
static int hex_val_char(uint16_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static AjsValue uri_encode_impl(AjsVm *vm, int argc, const AjsValue *argv, bool is_component) {
    AjsString *s = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    size_t cap = (size_t)s->len16 * 12u + 4u;
    char *out = (char *)malloc(cap);
    if (!out) return ajs_throw(vm, vm->oom_error);
    size_t pos = 0;
    static const char k_hex[] = "0123456789ABCDEF";
    uint32_t k = 0;
    while (k < s->len16) {
        uint16_t c = s->units[k];
        uint32_t cp = c;
        if (c >= 0xDC00 && c <= 0xDFFF) {
            free(out);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_URI, "URI malformed");
        }
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (k + 1u >= s->len16 || s->units[k + 1u] < 0xDC00 || s->units[k + 1u] > 0xDFFF) {
                free(out);
                return ajs_throw_err_kind(vm, AJS_ERR_KIND_URI, "URI malformed");
            }
            cp = 0x10000u + (((uint32_t)(c - 0xD800u)) << 10) + (uint32_t)(s->units[k + 1u] - 0xDC00u);
            k += 2u;
        } else {
            k++;
        }
        if (uri_unescaped(cp, is_component)) {
            out[pos++] = (char)cp;
        } else {
            char u8[4];
            size_t n = ajs_code_point_to_utf8(cp, u8);
            for (size_t j = 0; j < n; j++) {
                unsigned char b = (unsigned char)u8[j];
                out[pos++] = '%';
                out[pos++] = k_hex[b >> 4];
                out[pos++] = k_hex[b & 0x0Fu];
            }
        }
    }
    AjsString *res = ajs_str_from_utf8(vm, out, pos, false);
    free(out);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}

static AjsValue uri_decode_impl(AjsVm *vm, int argc, const AjsValue *argv, bool is_component) {
    AjsString *s = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    uint16_t *out = (uint16_t *)malloc(((size_t)s->len16 + 2u) * sizeof(uint16_t));
    if (!out) return ajs_throw(vm, vm->oom_error);
    size_t pos = 0;
    uint32_t k = 0;
    while (k < s->len16) {
        uint16_t c = s->units[k];
        if (c != '%') {
            out[pos++] = c;
            k++;
            continue;
        }
        uint32_t start = k;
        if (k + 2u >= s->len16) goto malformed;
        int h1 = hex_val_char(s->units[k + 1u]);
        int h2 = hex_val_char(s->units[k + 2u]);
        if (h1 < 0 || h2 < 0) goto malformed;
        uint8_t b0 = (uint8_t)((h1 << 4) | h2);
        k += 3u;
        if ((b0 & 0x80u) == 0) {
            if (!is_component && uri_reserved_or_hash(b0)) {
                for (uint32_t j = start; j < k; j++) out[pos++] = s->units[j];
            } else {
                out[pos++] = (uint16_t)b0;
            }
            continue;
        }
        int n = 0;
        if ((b0 & 0xE0u) == 0xC0u) n = 2;
        else if ((b0 & 0xF0u) == 0xE0u) n = 3;
        else if ((b0 & 0xF8u) == 0xF0u) n = 4;
        else goto malformed;
        uint32_t cp = b0 & (uint32_t)((1 << (7 - n)) - 1);
        for (int j = 1; j < n; j++) {
            if (k + 2u >= s->len16 || s->units[k] != '%') goto malformed;
            int ha = hex_val_char(s->units[k + 1u]);
            int hb = hex_val_char(s->units[k + 2u]);
            if (ha < 0 || hb < 0) goto malformed;
            uint8_t bx = (uint8_t)((ha << 4) | hb);
            if ((bx & 0xC0u) != 0x80u) goto malformed;
            cp = (cp << 6) | (bx & 0x3Fu);
            k += 3u;
        }
        /* Reject overlong UTF-8 and surrogates and > U+10FFFF */
        if ((n == 2 && cp < 0x80u) || (n == 3 && cp < 0x800u) || (n == 4 && cp < 0x10000u) ||
            (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
            goto malformed;
        }
        uint16_t u16[2];
        size_t un = ajs_code_point_to_utf16(cp, u16);
        for (size_t j = 0; j < un; j++) out[pos++] = u16[j];
    }
    {
        AjsString *res = ajs_str_from_utf16(vm, out, pos, false);
        free(out);
        return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
    }
malformed:
    free(out);
    return ajs_throw_err_kind(vm, AJS_ERR_KIND_URI, "URI malformed");
}

static AjsValue js_encodeURI(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)t; (void)u; return uri_encode_impl(vm, c, v, false); }
static AjsValue js_encodeURIComponent(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)t; (void)u; return uri_encode_impl(vm, c, v, true); }
static AjsValue js_decodeURI(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)t; (void)u; return uri_decode_impl(vm, c, v, false); }
static AjsValue js_decodeURIComponent(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)t; (void)u; return uri_decode_impl(vm, c, v, true); }

/* Annex B escape / unescape */
static AjsValue js_annexb_escape(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)t; (void)u;
    AjsString *s = ajs_to_js_string(vm, c > 0 ? v[0] : AJS_VAL_UNDEFINED);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    uint16_t *out = (uint16_t *)malloc(((size_t)s->len16 * 6u + 2u) * sizeof(uint16_t));
    if (!out) return ajs_throw(vm, vm->oom_error);
    size_t pos = 0;
    static const char k_hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < s->len16; i++) {
        uint16_t ch = s->units[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '@' || ch == '*' || ch == '_' || ch == '+' || ch == '-' || ch == '.' || ch == '/') {
            out[pos++] = ch;
        } else if (ch < 256u) {
            out[pos++] = '%';
            out[pos++] = (uint16_t)k_hex[ch >> 4];
            out[pos++] = (uint16_t)k_hex[ch & 0x0Fu];
        } else {
            out[pos++] = '%'; out[pos++] = 'u';
            out[pos++] = (uint16_t)k_hex[(ch >> 12) & 0x0Fu];
            out[pos++] = (uint16_t)k_hex[(ch >> 8) & 0x0Fu];
            out[pos++] = (uint16_t)k_hex[(ch >> 4) & 0x0Fu];
            out[pos++] = (uint16_t)k_hex[ch & 0x0Fu];
        }
    }
    AjsString *res = ajs_str_from_utf16(vm, out, pos, false);
    free(out);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}

static AjsValue js_annexb_unescape(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)t; (void)u;
    AjsString *s = ajs_to_js_string(vm, c > 0 ? v[0] : AJS_VAL_UNDEFINED);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    uint16_t *out = (uint16_t *)malloc(((size_t)s->len16 + 2u) * sizeof(uint16_t));
    if (!out) return ajs_throw(vm, vm->oom_error);
    size_t pos = 0;
    for (uint32_t i = 0; i < s->len16; i++) {
        uint16_t ch = s->units[i];
        if (ch == '%') {
            if (i + 5u < s->len16 && s->units[i + 1u] == 'u') {
                int h0 = hex_val_char(s->units[i + 2u]), h1 = hex_val_char(s->units[i + 3u]);
                int h2 = hex_val_char(s->units[i + 4u]), h3 = hex_val_char(s->units[i + 5u]);
                if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
                    out[pos++] = (uint16_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                    i += 5u;
                    continue;
                }
            } else if (i + 2u < s->len16) {
                int h0 = hex_val_char(s->units[i + 1u]), h1 = hex_val_char(s->units[i + 2u]);
                if (h0 >= 0 && h1 >= 0) {
                    out[pos++] = (uint16_t)((h0 << 4) | h1);
                    i += 2u;
                    continue;
                }
            }
        }
        out[pos++] = ch;
    }
    AjsString *res = ajs_str_from_utf16(vm, out, pos, false);
    free(out);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}

static AjsValue js_return_this(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)vm; (void)argc; (void)argv; (void)ud;
    return this_val;
}

void ajs_realm_init_intrinsics(AjsVm *vm, AjsRealm *r) {
    uint32_t gflags = AJS_DESC_WRITABLE | AJS_DESC_CONFIGURABLE;

    /* 1. %Object.prototype%, %Function.prototype%, and Global Object */
    AjsObject *obj_proto = ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, AJS_VAL_NULL);
    r->object_proto = ajs_val_from_gc(obj_proto);

    AjsObject *fn_proto = ajs_obj_new(vm, r, AJS_OBJ_FUNCTION, r->object_proto);
    fn_proto->u.fn.fn_kind = AJS_FN_NATIVE;
    fn_proto->u.fn.is_constructor = false;
    fn_proto->u.fn.strict = true;
    fn_proto->u.fn.native_fn = js_func_proto_noop;
    r->function_proto = ajs_val_from_gc(fn_proto);
    def_data_prop(vm, r->function_proto, "length", ajs_val_number(0.0), AJS_DESC_CONFIGURABLE);
    def_data_prop(vm, r->function_proto, "name", ajs_val_from_gc(vm->str_empty), AJS_DESC_CONFIGURABLE);

    AjsObject *gobj = ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto);
    r->global_obj = ajs_val_from_gc(gobj);
    def_to_string_tag(vm, r->global_obj, "global");

    /* %ThrowTypeError% */
    r->throw_type_error_fn = make_builtin_fn(vm, r, js_throw_type_error, "", 0, false, 0, r->function_proto);
    set_integrity_level(vm, (AjsObject *)ajs_val_to_gc(r->throw_type_error_fn), true);

    /* %IteratorPrototype% & %AsyncIteratorPrototype% */
    r->iterator_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    ajs_def_native_sym_method(vm, r, r->iterator_proto, vm->well_known_syms[AJS_SYM_ITERATOR],
                              "[Symbol.iterator]", js_return_this, 0, AJS_METHOD_FLAGS);
    r->async_iterator_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    ajs_def_native_sym_method(vm, r, r->async_iterator_proto, vm->well_known_syms[AJS_SYM_ASYNC_ITERATOR],
                              "[Symbol.asyncIterator]", js_return_this, 0, AJS_METHOD_FLAGS);

    /* 2. Object */
    r->object_ctor = make_builtin_fn(vm, r, js_object_ctor, "Object", 1, true, -1, r->function_proto);
    link_ctor_proto(vm, r->object_ctor, r->object_proto, false);
    def_data_prop(vm, r->global_obj, "Object", r->object_ctor, gflags);

    ajs_def_native_method(vm, r, r->object_ctor, "assign", js_object_assign, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "create", js_object_create, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "defineProperty", js_object_defineProperty, 3, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "defineProperties", js_object_defineProperties, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "entries", js_object_entries, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "freeze", js_object_freeze, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "fromEntries", js_object_fromEntries, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "getOwnPropertyDescriptor", js_object_getOwnPropertyDescriptor, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "getOwnPropertyDescriptors", js_object_getOwnPropertyDescriptors, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "getOwnPropertyNames", js_object_getOwnPropertyNames, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "getOwnPropertySymbols", js_object_getOwnPropertySymbols, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "getPrototypeOf", js_object_getPrototypeOf, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "groupBy", js_object_groupBy, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "hasOwn", js_object_hasOwn, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "is", js_object_is, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "isExtensible", js_object_isExtensible, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "isFrozen", js_object_isFrozen, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "isSealed", js_object_isSealed, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "keys", js_object_keys, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "preventExtensions", js_object_preventExtensions, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "seal", js_object_seal, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "setPrototypeOf", js_object_setPrototypeOf, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_ctor, "values", js_object_values, 1, AJS_METHOD_FLAGS, 0);

    ajs_def_native_method(vm, r, r->object_proto, "toString", js_object_proto_toString, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_proto, "toLocaleString", js_object_proto_toLocaleString, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_proto, "valueOf", js_object_proto_valueOf, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_proto, "hasOwnProperty", js_object_proto_hasOwnProperty, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_proto, "isPrototypeOf", js_object_proto_isPrototypeOf, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_proto, "propertyIsEnumerable", js_object_proto_propertyIsEnumerable, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_accessor(vm, r, r->object_proto, "__proto__", js_object_proto_get_dunder, js_object_proto_set_dunder, AJS_PROP_CONFIGURABLE);
    ajs_def_native_method(vm, r, r->object_proto, "__defineGetter__", js_object_defineGetter, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_proto, "__defineSetter__", js_object_defineSetter, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_proto, "__lookupGetter__", js_object_lookupGetter, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->object_proto, "__lookupSetter__", js_object_lookupSetter, 1, AJS_METHOD_FLAGS, 0);

    /* 3. Function & Generator / Async Function Intrinsics */
    r->function_ctor = make_builtin_fn(vm, r, js_function_ctor, "Function", 1, true, -2, r->function_proto);
    link_ctor_proto(vm, r->function_ctor, r->function_proto, false);
    def_data_prop(vm, r->global_obj, "Function", r->function_ctor, gflags);

    ajs_def_native_method(vm, r, r->function_proto, "call", js_func_proto_call, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->function_proto, "apply", js_func_proto_apply, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->function_proto, "bind", js_func_proto_bind, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->function_proto, "toString", js_func_proto_toString, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_sym_method(vm, r, r->function_proto, vm->well_known_syms[AJS_SYM_HAS_INSTANCE],
                              "[Symbol.hasInstance]", js_func_proto_hasInstance, 1, 0);
    {
        AjsPropDesc tte = {
            .flags = AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_GET | AJS_DESC_HAS_SET |
                     AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
            .getter = r->throw_type_error_fn,
            .setter = r->throw_type_error_fn
        };
        ajs_obj_define_own_prop(vm, fn_proto, ajs_val_from_gc(ajs_str_from_cstr(vm, "caller", true)), &tte, false);
        ajs_obj_define_own_prop(vm, fn_proto, ajs_val_from_gc(ajs_str_from_cstr(vm, "arguments", true)), &tte, false);
    }

    r->gen_func_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->function_proto));
    r->gen_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->iterator_proto));
    r->gen_func_ctor = make_builtin_fn(vm, r, js_gen_func_ctor, "GeneratorFunction", 1, true, -3, r->function_ctor);
    link_ctor_proto(vm, r->gen_func_ctor, r->gen_func_proto, false);
    def_data_prop(vm, r->gen_func_proto, "prototype", r->gen_proto, AJS_DESC_CONFIGURABLE);
    def_data_prop(vm, r->gen_proto, "constructor", r->gen_func_proto, AJS_DESC_CONFIGURABLE);
    def_to_string_tag(vm, r->gen_func_proto, "GeneratorFunction");
    def_to_string_tag(vm, r->gen_proto, "Generator");
    ajs_def_native_method(vm, r, r->gen_proto, "next", js_gen_next, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->gen_proto, "return", js_gen_return, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->gen_proto, "throw", js_gen_throw, 1, AJS_METHOD_FLAGS, 0);

    r->async_func_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->function_proto));
    r->async_func_ctor = make_builtin_fn(vm, r, js_async_func_ctor, "AsyncFunction", 1, true, -4, r->function_ctor);
    link_ctor_proto(vm, r->async_func_ctor, r->async_func_proto, false);
    def_to_string_tag(vm, r->async_func_proto, "AsyncFunction");

    r->async_gen_func_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->function_proto));
    r->async_gen_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->async_iterator_proto));
    r->async_gen_func_ctor = make_builtin_fn(vm, r, js_async_gen_func_ctor, "AsyncGeneratorFunction", 1, true, -5, r->function_ctor);
    link_ctor_proto(vm, r->async_gen_func_ctor, r->async_gen_func_proto, false);
    def_data_prop(vm, r->async_gen_func_proto, "prototype", r->async_gen_proto, AJS_DESC_CONFIGURABLE);
    def_data_prop(vm, r->async_gen_proto, "constructor", r->async_gen_func_proto, AJS_DESC_CONFIGURABLE);
    def_to_string_tag(vm, r->async_gen_func_proto, "AsyncGeneratorFunction");
    def_to_string_tag(vm, r->async_gen_proto, "AsyncGenerator");
    ajs_def_native_method(vm, r, r->async_gen_proto, "next", js_agen_next, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->async_gen_proto, "return", js_agen_return, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->async_gen_proto, "throw", js_agen_throw, 1, AJS_METHOD_FLAGS, 0);

    /* 4. Boolean */
    AjsObject *bproto = ajs_obj_new(vm, r, AJS_OBJ_BOOLEAN_OBJ, r->object_proto);
    bproto->u.prim_val = AJS_VAL_FALSE;
    r->boolean_proto = ajs_val_from_gc(bproto);
    r->boolean_ctor = make_builtin_fn(vm, r, js_boolean_ctor, "Boolean", 1, true, -6, r->function_proto);
    link_ctor_proto(vm, r->boolean_ctor, r->boolean_proto, false);
    def_data_prop(vm, r->global_obj, "Boolean", r->boolean_ctor, gflags);
    ajs_def_native_method(vm, r, r->boolean_proto, "toString", js_boolean_proto_toString, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->boolean_proto, "valueOf", js_boolean_proto_valueOf, 0, AJS_METHOD_FLAGS, 0);

    /* 5. Number & Global Numeric Functions */
    AjsObject *nproto = ajs_obj_new(vm, r, AJS_OBJ_NUMBER_OBJ, r->object_proto);
    nproto->u.prim_val = ajs_val_number(0.0);
    r->number_proto = ajs_val_from_gc(nproto);
    r->number_ctor = make_builtin_fn(vm, r, js_number_ctor, "Number", 1, true, -7, r->function_proto);
    link_ctor_proto(vm, r->number_ctor, r->number_proto, false);
    def_data_prop(vm, r->global_obj, "Number", r->number_ctor, gflags);

    def_data_prop(vm, r->number_ctor, "EPSILON", ajs_val_number(DBL_EPSILON), 0);
    def_data_prop(vm, r->number_ctor, "MAX_SAFE_INTEGER", ajs_val_number(9007199254740991.0), 0);
    def_data_prop(vm, r->number_ctor, "MIN_SAFE_INTEGER", ajs_val_number(-9007199254740991.0), 0);
    def_data_prop(vm, r->number_ctor, "MAX_VALUE", ajs_val_number(DBL_MAX), 0);
    def_data_prop(vm, r->number_ctor, "MIN_VALUE", ajs_val_number(5e-324), 0);
    def_data_prop(vm, r->number_ctor, "NaN", AJS_VAL_NAN, 0);
    def_data_prop(vm, r->number_ctor, "POSITIVE_INFINITY", ajs_val_number(INFINITY), 0);
    def_data_prop(vm, r->number_ctor, "NEGATIVE_INFINITY", ajs_val_number(-INFINITY), 0);
    ajs_def_native_method(vm, r, r->number_ctor, "isFinite", js_number_isFinite, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->number_ctor, "isInteger", js_number_isInteger, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->number_ctor, "isNaN", js_number_isNaN, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->number_ctor, "isSafeInteger", js_number_isSafeInteger, 1, AJS_METHOD_FLAGS, 0);

    AjsValue fn_parseInt = make_builtin_fn(vm, r, js_global_parseInt, "parseInt", 2, false, 0, r->function_proto);
    AjsValue fn_parseFloat = make_builtin_fn(vm, r, js_global_parseFloat, "parseFloat", 1, false, 0, r->function_proto);
    def_data_prop(vm, r->number_ctor, "parseInt", fn_parseInt, gflags);
    def_data_prop(vm, r->number_ctor, "parseFloat", fn_parseFloat, gflags);

    ajs_def_native_method(vm, r, r->number_proto, "toString", js_number_proto_toString, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->number_proto, "toLocaleString", js_number_proto_toString, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->number_proto, "valueOf", js_number_proto_valueOf, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->number_proto, "toFixed", js_number_proto_toFixed, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->number_proto, "toExponential", js_number_proto_toExponential, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->number_proto, "toPrecision", js_number_proto_toPrecision, 1, AJS_METHOD_FLAGS, 0);

    /* 6. Math */
    AjsValue math_obj = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    def_data_prop(vm, r->global_obj, "Math", math_obj, gflags);
    def_to_string_tag(vm, math_obj, "Math");
    def_data_prop(vm, math_obj, "E",       ajs_val_number(2.71828182845904523536), 0);
    def_data_prop(vm, math_obj, "LN10",    ajs_val_number(2.30258509299404568402), 0);
    def_data_prop(vm, math_obj, "LN2",     ajs_val_number(0.693147180559945309417), 0);
    def_data_prop(vm, math_obj, "LOG10E",  ajs_val_number(0.434294481903251827651), 0);
    def_data_prop(vm, math_obj, "LOG2E",   ajs_val_number(1.44269504088896340736), 0);
    def_data_prop(vm, math_obj, "PI",      ajs_val_number(3.14159265358979323846), 0);
    def_data_prop(vm, math_obj, "SQRT1_2", ajs_val_number(0.707106781186547524401), 0);
    def_data_prop(vm, math_obj, "SQRT2",   ajs_val_number(1.41421356237309504880), 0);
    ajs_def_native_method(vm, r, math_obj, "abs",      js_math_1, 1, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, math_obj, "acos",     js_math_1, 1, AJS_METHOD_FLAGS, 2);
    ajs_def_native_method(vm, r, math_obj, "acosh",    js_math_1, 1, AJS_METHOD_FLAGS, 3);
    ajs_def_native_method(vm, r, math_obj, "asin",     js_math_1, 1, AJS_METHOD_FLAGS, 4);
    ajs_def_native_method(vm, r, math_obj, "asinh",    js_math_1, 1, AJS_METHOD_FLAGS, 5);
    ajs_def_native_method(vm, r, math_obj, "atan",     js_math_1, 1, AJS_METHOD_FLAGS, 6);
    ajs_def_native_method(vm, r, math_obj, "atan2",    js_math_atan2, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, math_obj, "atanh",    js_math_1, 1, AJS_METHOD_FLAGS, 7);
    ajs_def_native_method(vm, r, math_obj, "cbrt",     js_math_1, 1, AJS_METHOD_FLAGS, 8);
    ajs_def_native_method(vm, r, math_obj, "ceil",     js_math_1, 1, AJS_METHOD_FLAGS, 9);
    ajs_def_native_method(vm, r, math_obj, "clz32",    js_math_1, 1, AJS_METHOD_FLAGS, 10);
    ajs_def_native_method(vm, r, math_obj, "cos",      js_math_1, 1, AJS_METHOD_FLAGS, 11);
    ajs_def_native_method(vm, r, math_obj, "cosh",     js_math_1, 1, AJS_METHOD_FLAGS, 12);
    ajs_def_native_method(vm, r, math_obj, "exp",      js_math_1, 1, AJS_METHOD_FLAGS, 13);
    ajs_def_native_method(vm, r, math_obj, "expm1",    js_math_1, 1, AJS_METHOD_FLAGS, 14);
    ajs_def_native_method(vm, r, math_obj, "floor",    js_math_1, 1, AJS_METHOD_FLAGS, 15);
    ajs_def_native_method(vm, r, math_obj, "fround",   js_math_1, 1, AJS_METHOD_FLAGS, 16);
    ajs_def_native_method(vm, r, math_obj, "f16round", js_math_1, 1, AJS_METHOD_FLAGS, 17);
    ajs_def_native_method(vm, r, math_obj, "hypot",    js_math_hypot, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, math_obj, "imul",     js_math_imul, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, math_obj, "log",      js_math_1, 1, AJS_METHOD_FLAGS, 18);
    ajs_def_native_method(vm, r, math_obj, "log1p",    js_math_1, 1, AJS_METHOD_FLAGS, 19);
    ajs_def_native_method(vm, r, math_obj, "log10",    js_math_1, 1, AJS_METHOD_FLAGS, 20);
    ajs_def_native_method(vm, r, math_obj, "log2",     js_math_1, 1, AJS_METHOD_FLAGS, 21);
    ajs_def_native_method(vm, r, math_obj, "max",      js_math_max, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, math_obj, "min",      js_math_min, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, math_obj, "pow",      js_math_pow, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, math_obj, "random",   js_math_random, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, math_obj, "round",    js_math_1, 1, AJS_METHOD_FLAGS, 22);
    ajs_def_native_method(vm, r, math_obj, "sign",     js_math_1, 1, AJS_METHOD_FLAGS, 23);
    ajs_def_native_method(vm, r, math_obj, "sin",      js_math_1, 1, AJS_METHOD_FLAGS, 24);
    ajs_def_native_method(vm, r, math_obj, "sinh",     js_math_1, 1, AJS_METHOD_FLAGS, 25);
    ajs_def_native_method(vm, r, math_obj, "sqrt",     js_math_1, 1, AJS_METHOD_FLAGS, 26);
    ajs_def_native_method(vm, r, math_obj, "tan",      js_math_1, 1, AJS_METHOD_FLAGS, 27);
    ajs_def_native_method(vm, r, math_obj, "tanh",     js_math_1, 1, AJS_METHOD_FLAGS, 28);
    ajs_def_native_method(vm, r, math_obj, "trunc",    js_math_1, 1, AJS_METHOD_FLAGS, 29);

    /* 7. Symbol & BigInt */
    r->symbol_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->symbol_ctor = make_builtin_fn(vm, r, js_symbol_call, "Symbol", 0, false, 0, r->function_proto);
    link_ctor_proto(vm, r->symbol_ctor, r->symbol_proto, false);
    def_data_prop(vm, r->global_obj, "Symbol", r->symbol_ctor, gflags);
    def_to_string_tag(vm, r->symbol_proto, "Symbol");
    ajs_def_native_method(vm, r, r->symbol_ctor, "for", js_symbol_for, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->symbol_ctor, "keyFor", js_symbol_keyFor, 1, AJS_METHOD_FLAGS, 0);
    {
        static const char *const k_wks_props[AJS_SYM_COUNT] = {
            "iterator", "asyncIterator", "hasInstance", "isConcatSpreadable",
            "match", "matchAll", "replace", "search", "species", "split",
            "toPrimitive", "toStringTag", "unscopables", "dispose", "asyncDispose"
        };
        for (int i = 0; i < AJS_SYM_COUNT; i++) {
            def_data_prop(vm, r->symbol_ctor, k_wks_props[i], ajs_val_from_gc(vm->well_known_syms[i]), 0);
        }
    }
    ajs_def_native_method(vm, r, r->symbol_proto, "toString", js_symbol_proto_toString, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->symbol_proto, "valueOf", js_symbol_proto_valueOf, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_accessor(vm, r, r->symbol_proto, "description", js_symbol_proto_desc, NULL, AJS_PROP_CONFIGURABLE);
    ajs_def_native_sym_method(vm, r, r->symbol_proto, vm->well_known_syms[AJS_SYM_TO_PRIMITIVE],
                              "[Symbol.toPrimitive]", js_symbol_proto_valueOf, 1, AJS_PROP_CONFIGURABLE);

    r->bigint_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->bigint_ctor = make_builtin_fn(vm, r, js_bigint_call, "BigInt", 1, false, 0, r->function_proto);
    link_ctor_proto(vm, r->bigint_ctor, r->bigint_proto, false);
    def_data_prop(vm, r->global_obj, "BigInt", r->bigint_ctor, gflags);
    def_to_string_tag(vm, r->bigint_proto, "BigInt");
    ajs_def_native_method(vm, r, r->bigint_ctor, "asIntN", js_bigint_asIntN, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->bigint_ctor, "asUintN", js_bigint_asUintN, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->bigint_proto, "toString", js_bigint_proto_toString, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->bigint_proto, "toLocaleString", js_bigint_proto_toString, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->bigint_proto, "valueOf", js_bigint_proto_valueOf, 0, AJS_METHOD_FLAGS, 0);

    /* 8. Date */
    r->date_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->date_ctor = make_builtin_fn(vm, r, js_date_ctor, "Date", 7, true, -8, r->function_proto);
    link_ctor_proto(vm, r->date_ctor, r->date_proto, false);
    def_data_prop(vm, r->global_obj, "Date", r->date_ctor, gflags);
    ajs_def_native_method(vm, r, r->date_ctor, "now", js_date_now, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->date_ctor, "parse", js_date_parse, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->date_ctor, "UTC", js_date_UTC, 7, AJS_METHOD_FLAGS, 0);

    ajs_def_native_method(vm, r, r->date_proto, "getTime", js_date_getter, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->date_proto, "valueOf", js_date_getter, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->date_proto, "getFullYear", js_date_getter, 0, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->date_proto, "getUTCFullYear", js_date_getter, 0, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->date_proto, "getMonth", js_date_getter, 0, AJS_METHOD_FLAGS, 2);
    ajs_def_native_method(vm, r, r->date_proto, "getUTCMonth", js_date_getter, 0, AJS_METHOD_FLAGS, 2);
    ajs_def_native_method(vm, r, r->date_proto, "getDate", js_date_getter, 0, AJS_METHOD_FLAGS, 3);
    ajs_def_native_method(vm, r, r->date_proto, "getUTCDate", js_date_getter, 0, AJS_METHOD_FLAGS, 3);
    ajs_def_native_method(vm, r, r->date_proto, "getDay", js_date_getter, 0, AJS_METHOD_FLAGS, 4);
    ajs_def_native_method(vm, r, r->date_proto, "getUTCDay", js_date_getter, 0, AJS_METHOD_FLAGS, 4);
    ajs_def_native_method(vm, r, r->date_proto, "getHours", js_date_getter, 0, AJS_METHOD_FLAGS, 5);
    ajs_def_native_method(vm, r, r->date_proto, "getUTCHours", js_date_getter, 0, AJS_METHOD_FLAGS, 5);
    ajs_def_native_method(vm, r, r->date_proto, "getMinutes", js_date_getter, 0, AJS_METHOD_FLAGS, 6);
    ajs_def_native_method(vm, r, r->date_proto, "getUTCMinutes", js_date_getter, 0, AJS_METHOD_FLAGS, 6);
    ajs_def_native_method(vm, r, r->date_proto, "getSeconds", js_date_getter, 0, AJS_METHOD_FLAGS, 7);
    ajs_def_native_method(vm, r, r->date_proto, "getUTCSeconds", js_date_getter, 0, AJS_METHOD_FLAGS, 7);
    ajs_def_native_method(vm, r, r->date_proto, "getMilliseconds", js_date_getter, 0, AJS_METHOD_FLAGS, 8);
    ajs_def_native_method(vm, r, r->date_proto, "getUTCMilliseconds", js_date_getter, 0, AJS_METHOD_FLAGS, 8);
    ajs_def_native_method(vm, r, r->date_proto, "getTimezoneOffset", js_date_getter, 0, AJS_METHOD_FLAGS, 9);
    ajs_def_native_method(vm, r, r->date_proto, "getYear", js_date_getter, 0, AJS_METHOD_FLAGS, 10);

    ajs_def_native_method(vm, r, r->date_proto, "setTime", js_date_setter, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->date_proto, "setMilliseconds", js_date_setter, 1, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->date_proto, "setUTCMilliseconds", js_date_setter, 1, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->date_proto, "setSeconds", js_date_setter, 2, AJS_METHOD_FLAGS, 2);
    ajs_def_native_method(vm, r, r->date_proto, "setUTCSeconds", js_date_setter, 2, AJS_METHOD_FLAGS, 2);
    ajs_def_native_method(vm, r, r->date_proto, "setMinutes", js_date_setter, 3, AJS_METHOD_FLAGS, 3);
    ajs_def_native_method(vm, r, r->date_proto, "setUTCMinutes", js_date_setter, 3, AJS_METHOD_FLAGS, 3);
    ajs_def_native_method(vm, r, r->date_proto, "setHours", js_date_setter, 4, AJS_METHOD_FLAGS, 4);
    ajs_def_native_method(vm, r, r->date_proto, "setUTCHours", js_date_setter, 4, AJS_METHOD_FLAGS, 4);
    ajs_def_native_method(vm, r, r->date_proto, "setDate", js_date_setter, 1, AJS_METHOD_FLAGS, 5);
    ajs_def_native_method(vm, r, r->date_proto, "setUTCDate", js_date_setter, 1, AJS_METHOD_FLAGS, 5);
    ajs_def_native_method(vm, r, r->date_proto, "setMonth", js_date_setter, 2, AJS_METHOD_FLAGS, 6);
    ajs_def_native_method(vm, r, r->date_proto, "setUTCMonth", js_date_setter, 2, AJS_METHOD_FLAGS, 6);
    ajs_def_native_method(vm, r, r->date_proto, "setFullYear", js_date_setter, 3, AJS_METHOD_FLAGS, 7);
    ajs_def_native_method(vm, r, r->date_proto, "setUTCFullYear", js_date_setter, 3, AJS_METHOD_FLAGS, 7);
    ajs_def_native_method(vm, r, r->date_proto, "setYear", js_date_setter, 1, AJS_METHOD_FLAGS, 8);

    ajs_def_native_method(vm, r, r->date_proto, "toISOString", js_date_toISOString, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->date_proto, "toJSON", js_date_toJSON, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->date_proto, "toString", js_date_format, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->date_proto, "toLocaleString", js_date_format, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->date_proto, "toUTCString", js_date_format, 0, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->date_proto, "toGMTString", js_date_format, 0, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->date_proto, "toDateString", js_date_format, 0, AJS_METHOD_FLAGS, 2);
    ajs_def_native_method(vm, r, r->date_proto, "toLocaleDateString", js_date_format, 0, AJS_METHOD_FLAGS, 2);
    ajs_def_native_method(vm, r, r->date_proto, "toTimeString", js_date_format, 0, AJS_METHOD_FLAGS, 3);
    ajs_def_native_method(vm, r, r->date_proto, "toLocaleTimeString", js_date_format, 0, AJS_METHOD_FLAGS, 3);
    ajs_def_native_sym_method(vm, r, r->date_proto, vm->well_known_syms[AJS_SYM_TO_PRIMITIVE],
                              "[Symbol.toPrimitive]", js_date_toPrimitive, 1, AJS_PROP_CONFIGURABLE);

    /* 9. Error & Subclasses */
    r->error_proto[0] = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->error_ctor[0] = make_builtin_fn(vm, r, js_error_ctor, "Error", 1, true, -100, r->function_proto);
    link_ctor_proto(vm, r->error_ctor[0], r->error_proto[0], false);
    def_data_prop(vm, r->error_proto[0], "name", ajs_val_from_gc(ajs_str_from_cstr(vm, "Error", true)), gflags);
    def_data_prop(vm, r->error_proto[0], "message", ajs_val_from_gc(vm->str_empty), gflags);
    ajs_def_native_method(vm, r, r->error_proto[0], "toString", js_error_proto_toString, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->error_ctor[0], "isError", js_error_isError, 1, AJS_METHOD_FLAGS, 0);
    def_data_prop(vm, r->global_obj, "Error", r->error_ctor[0], gflags);

    for (int i = 1; i < AJS_ERR_KIND_COUNT; i++) {
        int arity = (i == AJS_ERR_KIND_AGGREGATE) ? 2 : ((i == AJS_ERR_KIND_SUPPRESSED) ? 3 : 1);
        r->error_proto[i] = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->error_proto[0]));
        r->error_ctor[i] = make_builtin_fn(vm, r, js_error_ctor, k_err_names_tbl[i], arity, true, -(100 + i), r->error_ctor[0]);
        link_ctor_proto(vm, r->error_ctor[i], r->error_proto[i], false);
        def_data_prop(vm, r->error_proto[i], "name", ajs_val_from_gc(ajs_str_from_cstr(vm, k_err_names_tbl[i], true)), gflags);
        def_data_prop(vm, r->error_proto[i], "message", ajs_val_from_gc(vm->str_empty), gflags);
        def_data_prop(vm, r->global_obj, k_err_names_tbl[i], r->error_ctor[i], gflags);
    }

    /* 10. Reflect & Proxy */
    AjsValue reflect_obj = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    def_data_prop(vm, r->global_obj, "Reflect", reflect_obj, gflags);
    def_to_string_tag(vm, reflect_obj, "Reflect");
    ajs_def_native_method(vm, r, reflect_obj, "apply", js_reflect_apply, 3, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, reflect_obj, "construct", js_reflect_construct, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, reflect_obj, "defineProperty", js_reflect_defineProperty, 3, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, reflect_obj, "deleteProperty", js_reflect_deleteProperty, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, reflect_obj, "get", js_reflect_get, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, reflect_obj, "getOwnPropertyDescriptor", js_reflect_getOwnPropertyDescriptor, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, reflect_obj, "getPrototypeOf", js_reflect_getPrototypeOf, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, reflect_obj, "has", js_reflect_has, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, reflect_obj, "isExtensible", js_reflect_isExtensible, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, reflect_obj, "ownKeys", js_reflect_ownKeys, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, reflect_obj, "preventExtensions", js_reflect_preventExtensions, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, reflect_obj, "set", js_reflect_set, 3, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, reflect_obj, "setPrototypeOf", js_reflect_setPrototypeOf, 2, AJS_METHOD_FLAGS, 0);

    AjsValue proxy_ctor = make_builtin_fn(vm, r, js_proxy_ctor, "Proxy", 2, true, -9, r->function_proto);
    ajs_def_native_method(vm, r, proxy_ctor, "revocable", js_proxy_revocable, 2, AJS_METHOD_FLAGS, 0);
    def_data_prop(vm, r->global_obj, "Proxy", proxy_ctor, gflags);

    /* 11. Global Value Properties & Functions */
    def_data_prop(vm, r->global_obj, "globalThis", r->global_obj, gflags);
    def_data_prop(vm, r->global_obj, "Infinity", ajs_val_number(INFINITY), 0);
    def_data_prop(vm, r->global_obj, "NaN", AJS_VAL_NAN, 0);
    def_data_prop(vm, r->global_obj, "undefined", AJS_VAL_UNDEFINED, 0);

    r->intrinsic_eval = make_builtin_fn(vm, r, js_global_eval, "eval", 1, false, 0, r->function_proto);
    def_data_prop(vm, r->global_obj, "eval", r->intrinsic_eval, gflags);
    def_data_prop(vm, r->global_obj, "parseInt", fn_parseInt, gflags);
    def_data_prop(vm, r->global_obj, "parseFloat", fn_parseFloat, gflags);
    ajs_def_native_method(vm, r, r->global_obj, "isNaN", js_global_isNaN, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->global_obj, "isFinite", js_global_isFinite, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->global_obj, "encodeURI", js_encodeURI, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->global_obj, "encodeURIComponent", js_encodeURIComponent, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->global_obj, "decodeURI", js_decodeURI, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->global_obj, "decodeURIComponent", js_decodeURIComponent, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->global_obj, "escape", js_annexb_escape, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->global_obj, "unescape", js_annexb_unescape, 1, AJS_METHOD_FLAGS, 0);
}
