/*
 * ArcradientJS 1.0 (Clean-Room Pure C11 ECMAScript 2026 Virtual Machine)
 * js/builtins/ajs_string_array.c - String (UTF-16, Normalize, WellFormed, Annex B),
 * Array (ES2023-2026 Immutable Copy Methods + Array.fromAsync), & ES2025/2026 Iterator Helpers
 */
#include "../vm/ajs_internal.h"

#define AJS_METHOD_FLAGS (AJS_PROP_WRITABLE | AJS_PROP_CONFIGURABLE)

static AjsValue sa_make_fn(AjsVm *vm, AjsRealm *r, AjsNativeFn fn, const char *name,
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

static void sa_def_prop(AjsVm *vm, AjsValue obj, const char *name, AjsValue val, uint32_t desc_flags) {
    if (!ajs_val_is_gc_kind(obj, AJS_GC_OBJECT)) return;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, obj);
    ajs_temp_root_push(vm, val);
    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | desc_flags;
    d.value = val;
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(obj),
                            ajs_val_from_gc(ajs_str_from_cstr(vm, name, true)), &d, false);
    ajs_temp_root_restore(vm, rs);
}

static void sa_link_ctor_proto(AjsVm *vm, AjsValue ctor, AjsValue proto) {
    sa_def_prop(vm, ctor, "prototype", proto, 0);
    sa_def_prop(vm, proto, "constructor", ctor, AJS_DESC_WRITABLE | AJS_DESC_CONFIGURABLE);
}

static void sa_def_tag(AjsVm *vm, AjsValue obj, const char *tag) {
    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
              AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    d.value = ajs_val_from_gc(ajs_str_from_cstr(vm, tag, true));
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(obj),
                            ajs_val_from_gc(vm->well_known_syms[AJS_SYM_TO_STRING_TAG]), &d, false);
}

static AjsValue sa_get_proto_from_ctor(AjsVm *vm, AjsValue nt, AjsValue fallback) {
    if (!ajs_val_is_gc_kind(nt, AJS_GC_OBJECT)) return fallback;
    AjsValue p = ajs_obj_get(vm, nt, ajs_val_from_gc(vm->str_prototype), nt);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    return ajs_val_is_gc_kind(p, AJS_GC_OBJECT) ? p : fallback;
}

static AjsString *req_coercible_str(AjsVm *vm, AjsValue this_val) {
    if (ajs_is_nullish(this_val)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "String.prototype method called on null or undefined");
        return NULL;
    }
    return ajs_to_js_string(vm, this_val);
}

static bool this_str_val(AjsVm *vm, AjsValue this_val, AjsString **out_s) {
    if (ajs_val_is_gc_kind(this_val, AJS_GC_STRING)) {
        *out_s = (AjsString *)ajs_val_to_gc(this_val);
        return true;
    }
    if (ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        AjsObject *o = (AjsObject *)ajs_val_to_gc(this_val);
        if (o->kind == AJS_OBJ_STRING_OBJ && ajs_val_is_gc_kind(o->u.prim_val, AJS_GC_STRING)) {
            *out_s = (AjsString *)ajs_val_to_gc(o->u.prim_val);
            return true;
        }
    }
    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "String.prototype.valueOf requires a String");
    return false;
}

static int u16_find(const uint16_t *hay, uint32_t hlen, const uint16_t *ndl, uint32_t nlen, uint32_t from) {
    if (nlen == 0) return (int)(from <= hlen ? from : hlen);
    if (nlen > hlen || from > hlen - nlen) return -1;
    for (uint32_t i = from; i <= hlen - nlen; i++) {
        if (memcmp(hay + i, ndl, (size_t)nlen * sizeof(uint16_t)) == 0) return (int)i;
    }
    return -1;
}

/* ============================================================================
 * 1. String Constructor, Static Methods & Prototype Methods
 * ============================================================================ */

static AjsValue js_string_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = NULL;
    if (argc == 0) {
        s = vm->str_empty;
    } else if (nt == AJS_VAL_UNDEFINED && ajs_val_is_gc_kind(argv[0], AJS_GC_SYMBOL)) {
        AjsSymbol *sym = (AjsSymbol *)ajs_val_to_gc(argv[0]);
        const char *d = (sym->has_desc && sym->desc) ? sym->desc->utf8 : "";
        size_t blen = strlen(d) + 9u;
        char *buf = (char *)malloc(blen);
        if (!buf) return ajs_throw(vm, vm->oom_error);
        snprintf(buf, blen, "Symbol(%s)", d);
        s = ajs_str_from_cstr(vm, buf, false);
        free(buf);
    } else {
        s = ajs_to_js_string(vm, argv[0]);
    }
    if (!s || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsValue sv = ajs_val_from_gc(s);
    if (nt == AJS_VAL_UNDEFINED) { ajs_temp_root_restore(vm, rs); return sv; }

    ajs_temp_root_push(vm, sv);
    AjsValue proto = sa_get_proto_from_ctor(vm, nt, vm->active_realm->string_proto);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, proto);
    AjsObject *box = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_STRING_OBJ, proto);
    if (!box) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    box->u.prim_val = sv;
    AjsValue bv = ajs_val_from_gc(box);
    sa_def_prop(vm, bv, "length", ajs_val_number((double)s->len16), 0);
    ajs_temp_root_restore(vm, rs);
    return bv;
}

static AjsValue js_string_fromCharCode(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    uint16_t *buf = argc > 0 ? (uint16_t *)malloc((size_t)argc * sizeof(uint16_t)) : NULL;
    if (argc > 0 && !buf) return ajs_throw(vm, vm->oom_error);
    for (int i = 0; i < argc; i++) {
        uint32_t u32 = 0;
        if (!ajs_to_uint32(vm, argv[i], &u32)) { free(buf); return AJS_VAL_EXCEPTION; }
        buf[i] = (uint16_t)(u32 & 0xFFFFu);
    }
    AjsString *s = ajs_str_from_utf16(vm, buf, (size_t)argc, false);
    free(buf);
    return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
}

static AjsValue js_string_fromCodePoint(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    uint16_t *buf = argc > 0 ? (uint16_t *)malloc((size_t)argc * 2u * sizeof(uint16_t)) : NULL;
    if (argc > 0 && !buf) return ajs_throw(vm, vm->oom_error);
    size_t pos = 0;
    for (int i = 0; i < argc; i++) {
        double d = 0.0;
        if (!ajs_to_number(vm, argv[i], &d)) { free(buf); return AJS_VAL_EXCEPTION; }
        if (!isfinite(d) || floor(d) != d || d < 0.0 || d > 0x10FFFF) {
            free(buf);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid code point");
        }
        uint16_t cu[2];
        size_t n = ajs_code_point_to_utf16((uint32_t)d, cu);
        for (size_t j = 0; j < n; j++) buf[pos++] = cu[j];
    }
    AjsString *s = ajs_str_from_utf16(vm, buf, pos, false);
    free(buf);
    return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
}

static AjsValue js_string_raw(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    AjsObject *cooked = ajs_to_object(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!cooked) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue cv = ajs_val_from_gc(cooked);
    ajs_temp_root_push(vm, cv);
    AjsValue rv = ajs_obj_get(vm, cv, ajs_val_from_gc(vm->str_raw), cv);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsObject *raw = ajs_to_object(vm, rv);
    if (!raw) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    rv = ajs_val_from_gc(raw);
    ajs_temp_root_push(vm, rv);

    AjsValue lv = ajs_obj_get(vm, rv, ajs_val_from_gc(vm->str_length), rv);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    uint64_t lit_seg = 0;
    if (!ajs_to_length(vm, lv, &lit_seg)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (lit_seg == 0) { ajs_temp_root_restore(vm, rs); return ajs_val_from_gc(vm->str_empty); }

    AjsString *acc = vm->str_empty;
    for (uint64_t k = 0; ; k++) {
        size_t ls = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, ajs_val_from_gc(acc));
        AjsValue kv = ajs_get_prop_index(vm, rv, (uint32_t)k);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        AjsString *seg = ajs_to_js_string(vm, kv);
        if (!seg || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(seg));
        acc = ajs_str_concat(vm, acc, seg);
        if (!acc) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (k + 1u == lit_seg) { ajs_temp_root_restore(vm, ls); break; }
        if (k + 1u < (uint64_t)argc) {
            ajs_temp_root_push(vm, ajs_val_from_gc(acc));
            AjsString *sub = ajs_to_js_string(vm, argv[k + 1u]);
            if (!sub || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            ajs_temp_root_push(vm, ajs_val_from_gc(sub));
            acc = ajs_str_concat(vm, acc, sub);
            if (!acc) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
        ajs_temp_root_restore(vm, ls);
    }
    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(acc);
}

static AjsValue js_str_toString(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    AjsString *s = NULL;
    return this_str_val(vm, this_val, &s) ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
}

static AjsValue js_str_charAt(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    double pos = 0.0;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &pos)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    pos = ajs_double_to_integer_or_inf(pos);
    ajs_temp_root_restore(vm, rs);
    if (pos < 0.0 || pos >= (double)s->len16) return ajs_val_from_gc(vm->str_empty);
    uint16_t ch = s->units[(uint32_t)pos];
    return ajs_val_from_gc(ajs_str_from_utf16(vm, &ch, 1, false));
}

static AjsValue js_str_charCodeAt(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    double pos = 0.0;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &pos)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    pos = ajs_double_to_integer_or_inf(pos);
    ajs_temp_root_restore(vm, rs);
    if (pos < 0.0 || pos >= (double)s->len16) return AJS_VAL_NAN;
    return ajs_val_number((double)s->units[(uint32_t)pos]);
}

static AjsValue js_str_codePointAt(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    double pos = 0.0;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &pos)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    pos = ajs_double_to_integer_or_inf(pos);
    ajs_temp_root_restore(vm, rs);
    if (pos < 0.0 || pos >= (double)s->len16) return AJS_VAL_UNDEFINED;
    uint32_t adv = 1;
    uint32_t cp = ajs_utf16_code_point_at(s->units, s->len16, (uint32_t)pos, &adv);
    return ajs_val_number((double)cp);
}

static AjsValue js_str_at(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    double idx = 0.0;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &idx)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    idx = ajs_double_to_integer_or_inf(idx);
    if (idx < 0.0) idx += (double)s->len16;
    ajs_temp_root_restore(vm, rs);
    if (idx < 0.0 || idx >= (double)s->len16) return AJS_VAL_UNDEFINED;
    uint16_t ch = s->units[(uint32_t)idx];
    return ajs_val_from_gc(ajs_str_from_utf16(vm, &ch, 1, false));
}

static AjsValue js_str_concat(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *acc = req_coercible_str(vm, this_val);
    if (!acc) return AJS_VAL_EXCEPTION;
    for (int i = 0; i < argc; i++) {
        ajs_temp_root_restore(vm, rs);
        ajs_temp_root_push(vm, ajs_val_from_gc(acc));
        AjsString *arg = ajs_to_js_string(vm, argv[i]);
        if (!arg || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(arg));
        acc = ajs_str_concat(vm, acc, arg);
        if (!acc) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    }
    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(acc);
}

static AjsValue js_str_indexOf(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    AjsString *ndl = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!ndl || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(ndl));
    double pos = 0.0;
    if (argc > 1 && argv[1] != AJS_VAL_UNDEFINED) {
        if (!ajs_to_number(vm, argv[1], &pos)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        pos = ajs_double_to_integer_or_inf(pos);
    }
    uint32_t from = pos < 0.0 ? 0u : (pos > (double)s->len16 ? s->len16 : (uint32_t)pos);
    int idx = u16_find(s->units, s->len16, ndl->units, ndl->len16, from);
    ajs_temp_root_restore(vm, rs);
    return ajs_val_number((double)idx);
}

static AjsValue js_str_lastIndexOf(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    AjsString *ndl = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!ndl || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(ndl));
    double num_pos = INFINITY;
    if (argc > 1 && argv[1] != AJS_VAL_UNDEFINED) {
        if (!ajs_to_number(vm, argv[1], &num_pos)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (isnan(num_pos)) num_pos = INFINITY;
        else num_pos = ajs_double_to_integer_or_inf(num_pos);
    }
    uint32_t start = num_pos < 0.0 ? 0u : (num_pos > (double)s->len16 ? s->len16 : (uint32_t)num_pos);
    int res = -1;
    if (ndl->len16 <= s->len16) {
        uint32_t max_i = s->len16 - ndl->len16;
        if (start > max_i) start = max_i;
        for (int64_t i = (int64_t)start; i >= 0; i--) {
            if (ndl->len16 == 0 || memcmp(s->units + i, ndl->units, (size_t)ndl->len16 * sizeof(uint16_t)) == 0) {
                res = (int)i;
                break;
            }
        }
    }
    ajs_temp_root_restore(vm, rs);
    return ajs_val_number((double)res);
}

static bool check_not_regexp(AjsVm *vm, AjsValue arg, const char *mname) {
    bool is_rx = false;
    if (!ajs_is_regexp_like(vm, arg, &is_rx)) return false;
    if (is_rx) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "First argument to String.prototype.%s must not be a regular expression", mname);
        return false;
    }
    return true;
}

static AjsValue js_str_includes(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    AjsValue q = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!check_not_regexp(vm, q, "includes")) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsString *ndl = ajs_to_js_string(vm, q);
    if (!ndl || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(ndl));
    double pos = 0.0;
    if (argc > 1 && argv[1] != AJS_VAL_UNDEFINED) {
        if (!ajs_to_number(vm, argv[1], &pos)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        pos = ajs_double_to_integer_or_inf(pos);
    }
    uint32_t from = pos < 0.0 ? 0u : (pos > (double)s->len16 ? s->len16 : (uint32_t)pos);
    int idx = u16_find(s->units, s->len16, ndl->units, ndl->len16, from);
    ajs_temp_root_restore(vm, rs);
    return ajs_bool(idx >= 0);
}

static AjsValue js_str_startsWith(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    AjsValue q = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!check_not_regexp(vm, q, "startsWith")) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsString *ndl = ajs_to_js_string(vm, q);
    if (!ndl || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(ndl));
    double pos = 0.0;
    if (argc > 1 && argv[1] != AJS_VAL_UNDEFINED) {
        if (!ajs_to_number(vm, argv[1], &pos)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        pos = ajs_double_to_integer_or_inf(pos);
    }
    uint32_t start = pos < 0.0 ? 0u : (pos > (double)s->len16 ? s->len16 : (uint32_t)pos);
    bool ok = (start + ndl->len16 <= s->len16) &&
              (ndl->len16 == 0 || memcmp(s->units + start, ndl->units, (size_t)ndl->len16 * sizeof(uint16_t)) == 0);
    ajs_temp_root_restore(vm, rs);
    return ajs_bool(ok);
}

static AjsValue js_str_endsWith(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    AjsValue q = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!check_not_regexp(vm, q, "endsWith")) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsString *ndl = ajs_to_js_string(vm, q);
    if (!ndl || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(ndl));
    double ep = (double)s->len16;
    if (argc > 1 && argv[1] != AJS_VAL_UNDEFINED) {
        if (!ajs_to_number(vm, argv[1], &ep)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ep = ajs_double_to_integer_or_inf(ep);
    }
    uint32_t end = ep < 0.0 ? 0u : (ep > (double)s->len16 ? s->len16 : (uint32_t)ep);
    bool ok = (ndl->len16 <= end) &&
              (ndl->len16 == 0 || memcmp(s->units + end - ndl->len16, ndl->units, (size_t)ndl->len16 * sizeof(uint16_t)) == 0);
    ajs_temp_root_restore(vm, rs);
    return ajs_bool(ok);
}

static AjsValue js_str_slice(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    double len = (double)s->len16;
    double st = 0.0, en = len;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &st)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    st = ajs_double_to_integer_or_inf(st);
    if (argc > 1 && argv[1] != AJS_VAL_UNDEFINED) {
        if (!ajs_to_number(vm, argv[1], &en)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        en = ajs_double_to_integer_or_inf(en);
    }
    uint32_t from = st < 0.0 ? ((len + st > 0.0) ? (uint32_t)(len + st) : 0u) : (st < len ? (uint32_t)st : s->len16);
    uint32_t to   = en < 0.0 ? ((len + en > 0.0) ? (uint32_t)(len + en) : 0u) : (en < len ? (uint32_t)en : s->len16);
    AjsString *res = (to > from) ? ajs_str_from_utf16(vm, s->units + from, to - from, false) : vm->str_empty;
    ajs_temp_root_restore(vm, rs);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}

static AjsValue js_str_substring(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    double len = (double)s->len16, st = 0.0, en = len;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &st)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    st = ajs_double_to_integer_or_inf(st);
    if (argc > 1 && argv[1] != AJS_VAL_UNDEFINED) {
        if (!ajs_to_number(vm, argv[1], &en)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        en = ajs_double_to_integer_or_inf(en);
    }
    uint32_t f1 = st < 0.0 ? 0u : (st > len ? s->len16 : (uint32_t)st);
    uint32_t f2 = en < 0.0 ? 0u : (en > len ? s->len16 : (uint32_t)en);
    uint32_t from = f1 < f2 ? f1 : f2;
    uint32_t to   = f1 < f2 ? f2 : f1;
    AjsString *res = (to > from) ? ajs_str_from_utf16(vm, s->units + from, to - from, false) : vm->str_empty;
    ajs_temp_root_restore(vm, rs);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}

/* Annex B String.prototype.substr */
static AjsValue js_str_substr(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    double size = (double)s->len16, st = 0.0, ln = INFINITY;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &st)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    st = ajs_double_to_integer_or_inf(st);
    if (argc > 1 && argv[1] != AJS_VAL_UNDEFINED) {
        if (!ajs_to_number(vm, argv[1], &ln)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ln = ajs_double_to_integer_or_inf(ln);
    }
    if (isinf(st) && st < 0.0) st = 0.0;
    else if (st < 0.0) st = (size + st > 0.0) ? (size + st) : 0.0;
    else if (st > size) st = size;
    double rlen = ln < 0.0 ? 0.0 : (ln < (size - st) ? ln : (size - st));
    AjsString *res = rlen > 0.0 ? ajs_str_from_utf16(vm, s->units + (uint32_t)st, (size_t)rlen, false) : vm->str_empty;
    ajs_temp_root_restore(vm, rs);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}

static AjsValue js_str_case(AjsVm *vm, AjsValue this_val, bool upper) {
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    uint16_t *out = (uint16_t *)malloc(((size_t)s->len16 * 3u + 2u) * sizeof(uint16_t));
    if (!out) return ajs_throw(vm, vm->oom_error);
    size_t pos = 0;
    uint32_t i = 0;
    while (i < s->len16) {
        uint32_t adv = 1;
        uint32_t cp = ajs_utf16_code_point_at(s->units, s->len16, i, &adv);
        i += adv;
        if (upper && cp == 0x00DF) { /* German sharp s -> SS */
            out[pos++] = 'S'; out[pos++] = 'S'; continue;
        }
        uint32_t m = upper ? ajs_unicode_to_upper(cp) : ajs_unicode_to_lower(cp);
        uint16_t cu[2];
        size_t n = ajs_code_point_to_utf16(m, cu);
        for (size_t j = 0; j < n; j++) out[pos++] = cu[j];
    }
    AjsString *res = ajs_str_from_utf16(vm, out, pos, false);
    free(out);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}
static AjsValue js_str_toLower(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)c; (void)v; (void)u; return js_str_case(vm, t, false); }
static AjsValue js_str_toUpper(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)c; (void)v; (void)u; return js_str_case(vm, t, true); }

static AjsValue js_str_trim_mode(AjsVm *vm, AjsValue this_val, int mode) {
    /* mode: 3=both, 1=start, 2=end */
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    uint32_t st = 0, en = s->len16;
    if (mode & 1) {
        while (st < en && ajs_unicode_is_whitespace(s->units[st])) st++;
    }
    if (mode & 2) {
        while (en > st && ajs_unicode_is_whitespace(s->units[en - 1u])) en--;
    }
    if (st == 0 && en == s->len16) return ajs_val_from_gc(s);
    AjsString *res = ajs_str_from_utf16(vm, s->units + st, en - st, false);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}
static AjsValue js_str_trim(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u)      { (void)c; (void)v; (void)u; return js_str_trim_mode(vm, t, 3); }
static AjsValue js_str_trimStart(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)c; (void)v; (void)u; return js_str_trim_mode(vm, t, 1); }
static AjsValue js_str_trimEnd(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u)   { (void)c; (void)v; (void)u; return js_str_trim_mode(vm, t, 2); }

static AjsValue js_str_pad_mode(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, bool pad_start) {
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    uint64_t max_len = 0;
    if (!ajs_to_length(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &max_len)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (max_len <= s->len16) { ajs_temp_root_restore(vm, rs); return ajs_val_from_gc(s); }
    AjsString *filler = NULL;
    if (argc > 1 && argv[1] != AJS_VAL_UNDEFINED) {
        filler = ajs_to_js_string(vm, argv[1]);
        if (!filler || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    } else {
        filler = ajs_str_from_cstr(vm, " ", true);
    }
    if (filler->len16 == 0) { ajs_temp_root_restore(vm, rs); return ajs_val_from_gc(s); }
    if (max_len > (1u << 27)) { ajs_temp_root_restore(vm, rs); return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid string length"); }

    uint32_t pad_len = (uint32_t)max_len - s->len16;
    uint16_t *out = (uint16_t *)malloc((size_t)max_len * sizeof(uint16_t));
    if (!out) { ajs_temp_root_restore(vm, rs); return ajs_throw(vm, vm->oom_error); }
    uint32_t pad_off = pad_start ? 0u : s->len16;
    uint32_t str_off = pad_start ? pad_len : 0u;
    ajs_memcpy(out + str_off, s->units, (size_t)s->len16 * sizeof(uint16_t));
    for (uint32_t i = 0; i < pad_len; i++) {
        out[pad_off + i] = filler->units[i % filler->len16];
    }
    AjsString *res = ajs_str_from_utf16(vm, out, (size_t)max_len, false);
    free(out);
    ajs_temp_root_restore(vm, rs);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}
static AjsValue js_str_padStart(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)u; return js_str_pad_mode(vm, t, c, v, true); }
static AjsValue js_str_padEnd(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u)   { (void)u; return js_str_pad_mode(vm, t, c, v, false); }

static AjsValue js_str_repeat(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    double n = 0.0;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &n)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    n = ajs_double_to_integer_or_inf(n);
    if (n < 0.0 || isinf(n)) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid count value");
    }
    uint64_t cnt = (uint64_t)n;
    if (cnt == 0 || s->len16 == 0) { ajs_temp_root_restore(vm, rs); return ajs_val_from_gc(vm->str_empty); }
    if (cnt * (uint64_t)s->len16 > (1u << 27)) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid string length");
    }
    size_t total = (size_t)(cnt * (uint64_t)s->len16);
    uint16_t *out = (uint16_t *)malloc(total * sizeof(uint16_t));
    if (!out) { ajs_temp_root_restore(vm, rs); return ajs_throw(vm, vm->oom_error); }
    for (uint64_t i = 0; i < cnt; i++) {
        memcpy(out + i * s->len16, s->units, (size_t)s->len16 * sizeof(uint16_t));
    }
    AjsString *res = ajs_str_from_utf16(vm, out, total, false);
    free(out);
    ajs_temp_root_restore(vm, rs);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}

/* ES2024+ isWellFormed & toWellFormed */
static AjsValue js_str_isWellFormed(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    for (uint32_t i = 0; i < s->len16; i++) {
        uint16_t cu = s->units[i];
        if (cu >= 0xDC00 && cu <= 0xDFFF) return AJS_VAL_FALSE;
        if (cu >= 0xD800 && cu <= 0xDBFF) {
            if (i + 1u >= s->len16 || s->units[i + 1u] < 0xDC00 || s->units[i + 1u] > 0xDFFF) return AJS_VAL_FALSE;
            i++;
        }
    }
    return AJS_VAL_TRUE;
}

static AjsValue js_str_toWellFormed(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    uint16_t *out = (uint16_t *)malloc(((size_t)s->len16 + 1u) * sizeof(uint16_t));
    if (!out) return ajs_throw(vm, vm->oom_error);
    for (uint32_t i = 0; i < s->len16; i++) {
        uint16_t cu = s->units[i];
        if (cu >= 0xDC00 && cu <= 0xDFFF) {
            out[i] = 0xFFFDu;
        } else if (cu >= 0xD800 && cu <= 0xDBFF) {
            if (i + 1u < s->len16 && s->units[i + 1u] >= 0xDC00 && s->units[i + 1u] <= 0xDFFF) {
                out[i] = cu;
                out[i + 1u] = s->units[i + 1u];
                i++;
            } else {
                out[i] = 0xFFFDu;
            }
        } else {
            out[i] = cu;
        }
    }
    AjsString *res = ajs_str_from_utf16(vm, out, s->len16, false);
    free(out);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}

static AjsValue js_str_localeCompare(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    AjsString *that = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!that || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    int cmp = ajs_str_cmp_utf16(s, that);
    ajs_temp_root_restore(vm, rs);
    return ajs_val_number(cmp < 0 ? -1.0 : (cmp > 0 ? 1.0 : 0.0));
}

/* Clean-room Unicode Normalize (NFC/NFD/NFKC/NFKD): Hangul algorithmic + Latin/Common canonical & compat tables */
typedef struct { uint16_t pre; uint16_t base; uint16_t comb; } NormPair;
static const NormPair k_canon_pairs[] = {
    {0x00C0,'A',0x0300},{0x00C1,'A',0x0301},{0x00C2,'A',0x0302},{0x00C3,'A',0x0303},{0x00C4,'A',0x0308},{0x00C5,'A',0x030A},
    {0x00C7,'C',0x0327},{0x00C8,'E',0x0300},{0x00C9,'E',0x0301},{0x00CA,'E',0x0302},{0x00CB,'E',0x0308},
    {0x00CC,'I',0x0300},{0x00CD,'I',0x0301},{0x00CE,'I',0x0302},{0x00CF,'I',0x0308},{0x00D1,'N',0x0303},
    {0x00D2,'O',0x0300},{0x00D3,'O',0x0301},{0x00D4,'O',0x0302},{0x00D5,'O',0x0303},{0x00D6,'O',0x0308},
    {0x00D9,'U',0x0300},{0x00DA,'U',0x0301},{0x00DB,'U',0x0302},{0x00DC,'U',0x0308},{0x00DD,'Y',0x0301},
    {0x00E0,'a',0x0300},{0x00E1,'a',0x0301},{0x00E2,'a',0x0302},{0x00E3,'a',0x0303},{0x00E4,'a',0x0308},{0x00E5,'a',0x030A},
    {0x00E7,'c',0x0327},{0x00E8,'e',0x0300},{0x00E9,'e',0x0301},{0x00EA,'e',0x0302},{0x00EB,'e',0x0308},
    {0x00EC,'i',0x0300},{0x00ED,'i',0x0301},{0x00EE,'i',0x0302},{0x00EF,'i',0x0308},{0x00F1,'n',0x0303},
    {0x00F2,'o',0x0300},{0x00F3,'o',0x0301},{0x00F4,'o',0x0302},{0x00F5,'o',0x0303},{0x00F6,'o',0x0308},
    {0x00F9,'u',0x0300},{0x00FA,'u',0x0301},{0x00FB,'u',0x0302},{0x00FC,'u',0x0308},{0x00FD,'y',0x0301},{0x00FF,'y',0x0308}
};

static int combining_class(uint32_t cp) {
    if (cp == 0x0327 || cp == 0x0328) return 202;
    if (cp >= 0x0300 && cp <= 0x0314) return 230;
    if (cp >= 0x0323 && cp <= 0x0332) return 220;
    return 0;
}

static AjsValue js_str_normalize(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));

    bool compose = true, compat = false;
    if (argc > 0 && argv[0] != AJS_VAL_UNDEFINED) {
        AjsString *fs = ajs_to_js_string(vm, argv[0]);
        if (!fs || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (ajs_str_eq_cstr(fs, "NFC"))       { compose = true;  compat = false; }
        else if (ajs_str_eq_cstr(fs, "NFD"))  { compose = false; compat = false; }
        else if (ajs_str_eq_cstr(fs, "NFKC")) { compose = true;  compat = true;  }
        else if (ajs_str_eq_cstr(fs, "NFKD")) { compose = false; compat = true;  }
        else {
            ajs_temp_root_restore(vm, rs);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "The normalization form should be one of NFC, NFD, NFKC, NFKD.");
        }
    }

    uint32_t *cps = (uint32_t *)malloc(((size_t)s->len16 * 4u + 4u) * sizeof(uint32_t));
    if (!cps) { ajs_temp_root_restore(vm, rs); return ajs_throw(vm, vm->oom_error); }
    size_t n = 0;

    /* 1. Decompose */
    for (uint32_t i = 0; i < s->len16; ) {
        uint32_t adv = 1;
        uint32_t cp = ajs_utf16_code_point_at(s->units, s->len16, i, &adv);
        i += adv;
        if (compat) {
            if (cp == 0x00A0) { cps[n++] = ' '; continue; }
            if (cp == 0x00B2) { cps[n++] = '2'; continue; }
            if (cp == 0x00B3) { cps[n++] = '3'; continue; }
            if (cp == 0x00B9) { cps[n++] = '1'; continue; }
            if (cp == 0xFB01) { cps[n++] = 'f'; cps[n++] = 'i'; continue; }
            if (cp == 0xFB02) { cps[n++] = 'f'; cps[n++] = 'l'; continue; }
            if (cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0u; /* Fullwidth ASCII */
            if (cp == 0x212B) cp = 0x00C5; /* Angstrom */
            if (cp == 0x2126) cp = 0x03A9; /* Ohm */
        } else {
            if (cp == 0x212B) cp = 0x00C5;
            if (cp == 0x2126) cp = 0x03A9;
        }
        /* Hangul syllable SBase..SBase+SCount-1 decomposition */
        if (cp >= 0xAC00u && cp <= 0xD7A3u) {
            uint32_t si = cp - 0xAC00u;
            cps[n++] = 0x1100u + si / 588u;
            cps[n++] = 0x1161u + (si % 588u) / 28u;
            uint32_t ti = si % 28u;
            if (ti > 0) cps[n++] = 0x11A7u + ti;
            continue;
        }
        bool matched = false;
        for (size_t k = 0; k < sizeof(k_canon_pairs) / sizeof(k_canon_pairs[0]); k++) {
            if (cp == k_canon_pairs[k].pre) {
                cps[n++] = k_canon_pairs[k].base;
                cps[n++] = k_canon_pairs[k].comb;
                matched = true;
                break;
            }
        }
        if (!matched) cps[n++] = cp;
    }

    /* 2. Canonical ordering of combining marks */
    for (size_t i = 1; i < n; i++) {
        int cc = combining_class(cps[i]);
        if (cc > 0) {
            size_t j = i;
            while (j > 0 && combining_class(cps[j - 1u]) > cc) {
                uint32_t tmp = cps[j]; cps[j] = cps[j - 1u]; cps[j - 1u] = tmp;
                j--;
            }
        }
    }

    /* 3. Canonical Composition (if NFC / NFKC) */
    if (compose && n > 1) {
        size_t w = 0;
        for (size_t i = 0; i < n; i++) {
            uint32_t cur = cps[i];
            if (w > 0) {
                uint32_t prev = cps[w - 1u];
                /* Hangul L + V -> LV, and LV + T -> LVT */
                if (prev >= 0x1100u && prev < 0x1100u + 19u && cur >= 0x1161u && cur < 0x1161u + 21u) {
                    cps[w - 1u] = 0xAC00u + ((prev - 0x1100u) * 21u + (cur - 0x1161u)) * 28u;
                    continue;
                }
                if (prev >= 0xAC00u && prev <= 0xD7A3u && ((prev - 0xAC00u) % 28u) == 0 &&
                    cur > 0x11A7u && cur < 0x11A7u + 28u) {
                    cps[w - 1u] = prev + (cur - 0x11A7u);
                    continue;
                }
                bool comp = false;
                for (size_t k = 0; k < sizeof(k_canon_pairs) / sizeof(k_canon_pairs[0]); k++) {
                    if (prev == k_canon_pairs[k].base && cur == k_canon_pairs[k].comb) {
                        cps[w - 1u] = k_canon_pairs[k].pre;
                        comp = true;
                        break;
                    }
                }
                if (comp) continue;
            }
            cps[w++] = cur;
        }
        n = w;
    }

    uint16_t *u16 = (uint16_t *)malloc((n * 2u + 2u) * sizeof(uint16_t));
    if (!u16) { free(cps); ajs_temp_root_restore(vm, rs); return ajs_throw(vm, vm->oom_error); }
    size_t ulen = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t cu[2];
        size_t cn = ajs_code_point_to_utf16(cps[i], cu);
        for (size_t j = 0; j < cn; j++) u16[ulen++] = cu[j];
    }
    free(cps);
    AjsString *res = ajs_str_from_utf16(vm, u16, ulen, false);
    free(u16);
    ajs_temp_root_restore(vm, rs);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}

/* String.prototype RegExp-aware methods: split, search, match, matchAll, replace, replaceAll */
static AjsString *get_substitution(AjsVm *vm, const AjsString *matched, const AjsString *str,
                                   uint32_t position, const AjsValue *captures, int ncap,
                                   AjsValue named_groups, const AjsString *rep_tpl) {
    size_t cap = (size_t)rep_tpl->len16 + (size_t)str->len16 + 32u;
    uint16_t *out = (uint16_t *)malloc(cap * sizeof(uint16_t));
    if (!out) { ajs_throw(vm, vm->oom_error); return NULL; }
    size_t pos = 0;

    #define ENSURE_OUT(add) do { \
        if (pos + (size_t)(add) + 4u > cap) { \
            cap = (pos + (size_t)(add) + 32u) * 2u; \
            uint16_t *nb = (uint16_t *)realloc(out, cap * sizeof(uint16_t)); \
            if (!nb) { free(out); ajs_throw(vm, vm->oom_error); return NULL; } \
            out = nb; \
        } \
    } while (0)

    for (uint32_t i = 0; i < rep_tpl->len16; i++) {
        uint16_t ch = rep_tpl->units[i];
        if (ch == '$' && i + 1u < rep_tpl->len16) {
            uint16_t nx = rep_tpl->units[i + 1u];
            if (nx == '$') {
                ENSURE_OUT(1); out[pos++] = '$'; i++; continue;
            }
            if (nx == '&') {
                ENSURE_OUT(matched->len16);
                ajs_memcpy(out + pos, matched->units, (size_t)matched->len16 * sizeof(uint16_t));
                pos += matched->len16; i++; continue;
            }
            if (nx == '`') {
                uint32_t pre = position <= str->len16 ? position : str->len16;
                ENSURE_OUT(pre);
                ajs_memcpy(out + pos, str->units, (size_t)pre * sizeof(uint16_t));
                pos += pre; i++; continue;
            }
            if (nx == '\'') {
                uint32_t tail_pos = position + matched->len16;
                if (tail_pos < str->len16) {
                    uint32_t tlen = str->len16 - tail_pos;
                    ENSURE_OUT(tlen);
                    ajs_memcpy(out + pos, str->units + tail_pos, (size_t)tlen * sizeof(uint16_t));
                    pos += tlen;
                }
                i++; continue;
            }
            if (nx >= '0' && nx <= '9') {
                int idx1 = nx - '0';
                int idx2 = (i + 2u < rep_tpl->len16 && rep_tpl->units[i + 2u] >= '0' && rep_tpl->units[i + 2u] <= '9')
                           ? (idx1 * 10 + (rep_tpl->units[i + 2u] - '0')) : -1;
                int chosen = -1, adv = 0;
                if (idx2 >= 1 && idx2 <= ncap) { chosen = idx2; adv = 2; }
                else if (idx1 >= 1 && idx1 <= ncap) { chosen = idx1; adv = 1; }
                if (chosen >= 1) {
                    AjsValue cv = captures[chosen - 1];
                    if (ajs_val_is_gc_kind(cv, AJS_GC_STRING)) {
                        const AjsString *cs = (const AjsString *)ajs_val_to_gc(cv);
                        ENSURE_OUT(cs->len16);
                        ajs_memcpy(out + pos, cs->units, (size_t)cs->len16 * sizeof(uint16_t));
                        pos += cs->len16;
                    }
                    i += (uint32_t)adv;
                    continue;
                }
            }
            if (nx == '<' && ajs_val_is_gc_kind(named_groups, AJS_GC_OBJECT)) {
                int gt = -1;
                for (uint32_t k = i + 2u; k < rep_tpl->len16; k++) {
                    if (rep_tpl->units[k] == '>') { gt = (int)k; break; }
                }
                if (gt > 0) {
                    AjsString *gname = ajs_str_from_utf16(vm, rep_tpl->units + i + 2u, (size_t)gt - (i + 2u), true);
                    if (gname) {
                        AjsValue gv = ajs_obj_get(vm, named_groups, ajs_val_from_gc(gname), named_groups);
                        if (vm->has_exception) { free(out); return NULL; }
                        if (gv != AJS_VAL_UNDEFINED) {
                            AjsString *gs = ajs_to_js_string(vm, gv);
                            if (!gs || vm->has_exception) { free(out); return NULL; }
                            ENSURE_OUT(gs->len16);
                            ajs_memcpy(out + pos, gs->units, (size_t)gs->len16 * sizeof(uint16_t));
                            pos += gs->len16;
                        }
                    }
                    i = (uint32_t)gt;
                    continue;
                }
            }
        }
        ENSURE_OUT(1);
        out[pos++] = ch;
    }
    #undef ENSURE_OUT
    AjsString *res = ajs_str_from_utf16(vm, out, pos, false);
    free(out);
    return res;
}

static AjsValue js_str_replace_impl(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, bool replace_all) {
    if (ajs_is_nullish(this_val)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "String.prototype.replace called on null or undefined");
    }
    AjsValue search_val = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue rep_val = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    size_t rs = ajs_temp_root_save(vm);

    if (!ajs_is_nullish(search_val)) {
        if (replace_all) {
            bool is_rx = false;
            if (!ajs_is_regexp_like(vm, search_val, &is_rx)) return AJS_VAL_EXCEPTION;
            if (is_rx) {
                AjsValue fv = ajs_obj_get(vm, search_val, ajs_val_from_gc(ajs_str_from_cstr(vm, "flags", true)), search_val);
                if (vm->has_exception) return AJS_VAL_EXCEPTION;
                if (ajs_is_nullish(fv)) {
                    return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "String.prototype.replaceAll called with non-global RegExp");
                }
                AjsString *fs = ajs_to_js_string(vm, fv);
                if (!fs || vm->has_exception) return AJS_VAL_EXCEPTION;
                if (!strchr(fs->utf8, 'g')) {
                    return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "String.prototype.replaceAll called with a non-global RegExp argument");
                }
            }
        }
        AjsValue replacer = ajs_obj_get(vm, search_val, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_REPLACE]), search_val);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        if (!ajs_is_nullish(replacer)) {
            if (!ajs_is_callable(vm, replacer)) {
                return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "[Symbol.replace] is not callable");
            }
            AjsValue cargs[2] = { this_val, rep_val };
            return ajs_call_internal(vm, replacer, search_val, 2, cargs);
        }
    }

    AjsString *s = ajs_to_js_string(vm, this_val);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    AjsString *search_str = ajs_to_js_string(vm, search_val);
    if (!search_str || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(search_str));

    bool fn_rep = ajs_is_callable(vm, rep_val);
    AjsString *rep_tpl = NULL;
    if (!fn_rep) {
        rep_tpl = ajs_to_js_string(vm, rep_val);
        if (!rep_tpl || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(rep_tpl));
    }

    int pos = u16_find(s->units, s->len16, search_str->units, search_str->len16, 0);
    if (pos < 0) {
        ajs_temp_root_restore(vm, rs);
        return ajs_val_from_gc(s);
    }

    AjsString *acc = vm->str_empty;
    uint32_t end_of_last = 0;
    uint32_t adv_by = search_str->len16 > 0 ? search_str->len16 : 1u;

    while (pos >= 0) {
        size_t ls = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, ajs_val_from_gc(acc));
        uint32_t mpos = (uint32_t)pos;
        AjsString *replacement = NULL;
        if (fn_rep) {
            AjsValue fargs[3] = {
                ajs_val_from_gc(search_str),
                ajs_val_number((double)mpos),
                ajs_val_from_gc(s)
            };
            AjsValue rv = ajs_call_internal(vm, rep_val, AJS_VAL_UNDEFINED, 3, fargs);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            replacement = ajs_to_js_string(vm, rv);
            if (!replacement || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        } else {
            replacement = get_substitution(vm, search_str, s, mpos, NULL, 0, AJS_VAL_UNDEFINED, rep_tpl);
            if (!replacement) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
        ajs_temp_root_push(vm, ajs_val_from_gc(replacement));
        if (mpos > end_of_last) {
            AjsString *pre = ajs_str_from_utf16(vm, s->units + end_of_last, mpos - end_of_last, false);
            ajs_temp_root_push(vm, ajs_val_from_gc(pre));
            acc = ajs_str_concat(vm, acc, pre);
            ajs_temp_root_push(vm, ajs_val_from_gc(acc));
        }
        acc = ajs_str_concat(vm, acc, replacement);
        end_of_last = mpos + search_str->len16;
        ajs_temp_root_restore(vm, ls);

        if (!replace_all) break;
        if (mpos + adv_by > s->len16) break;
        pos = u16_find(s->units, s->len16, search_str->units, search_str->len16, mpos + adv_by);
    }

    if (end_of_last < s->len16) {
        ajs_temp_root_push(vm, ajs_val_from_gc(acc));
        AjsString *tail = ajs_str_from_utf16(vm, s->units + end_of_last, s->len16 - end_of_last, false);
        ajs_temp_root_push(vm, ajs_val_from_gc(tail));
        acc = ajs_str_concat(vm, acc, tail);
    }
    ajs_temp_root_restore(vm, rs);
    return acc ? ajs_val_from_gc(acc) : AJS_VAL_EXCEPTION;
}
static AjsValue js_str_replace(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u)    { (void)u; return js_str_replace_impl(vm, t, c, v, false); }
static AjsValue js_str_replaceAll(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)u; return js_str_replace_impl(vm, t, c, v, true); }

static AjsValue js_str_split(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (ajs_is_nullish(this_val)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "String.prototype.split called on null or undefined");
    }
    AjsValue sep_val = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue lim_val = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;

    if (!ajs_is_nullish(sep_val)) {
        AjsValue sp_fn = ajs_obj_get(vm, sep_val, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_SPLIT]), sep_val);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        if (!ajs_is_nullish(sp_fn)) {
            if (!ajs_is_callable(vm, sp_fn)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "[Symbol.split] is not callable");
            AjsValue cargs[2] = { this_val, lim_val };
            return ajs_call_internal(vm, sp_fn, sep_val, 2, cargs);
        }
    }

    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = ajs_to_js_string(vm, this_val);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));

    uint32_t lim = UINT32_MAX;
    if (lim_val != AJS_VAL_UNDEFINED) {
        if (!ajs_to_uint32(vm, lim_val, &lim)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    }

    AjsString *sep = NULL;
    if (sep_val != AJS_VAL_UNDEFINED) {
        sep = ajs_to_js_string(vm, sep_val);
        if (!sep || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(sep));
    }

    AjsValue arr = ajs_new_array(vm, 0);
    ajs_temp_root_push(vm, arr);
    if (lim == 0) { ajs_temp_root_restore(vm, rs); return arr; }
    if (sep_val == AJS_VAL_UNDEFINED) {
        ajs_set_prop_index(vm, arr, 0, ajs_val_from_gc(s));
        ajs_temp_root_restore(vm, rs);
        return arr;
    }

    uint32_t out_n = 0;
    if (sep->len16 == 0) {
        for (uint32_t i = 0; i < s->len16 && out_n < lim; i++) {
            AjsString *ch = ajs_str_from_utf16(vm, s->units + i, 1, false);
            size_t ls = ajs_temp_root_save(vm);
            ajs_temp_root_push(vm, ajs_val_from_gc(ch));
            ajs_set_prop_index(vm, arr, out_n++, ajs_val_from_gc(ch));
            ajs_temp_root_restore(vm, ls);
        }
        ajs_temp_root_restore(vm, rs);
        return arr;
    }

    if (s->len16 == 0) {
        ajs_set_prop_index(vm, arr, 0, ajs_val_from_gc(vm->str_empty));
        ajs_temp_root_restore(vm, rs);
        return arr;
    }

    uint32_t p = 0, q = 0;
    while (q + sep->len16 <= s->len16) {
        int found = u16_find(s->units, s->len16, sep->units, sep->len16, q);
        if (found < 0) break;
        uint32_t e = (uint32_t)found;
        AjsString *sub = ajs_str_from_utf16(vm, s->units + p, e - p, false);
        size_t ls = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, ajs_val_from_gc(sub));
        ajs_set_prop_index(vm, arr, out_n++, ajs_val_from_gc(sub));
        ajs_temp_root_restore(vm, ls);
        if (out_n == lim) { ajs_temp_root_restore(vm, rs); return arr; }
        p = e + sep->len16;
        q = p;
    }
    AjsString *rest = ajs_str_from_utf16(vm, s->units + p, s->len16 - p, false);
    ajs_temp_root_push(vm, ajs_val_from_gc(rest));
    ajs_set_prop_index(vm, arr, out_n, ajs_val_from_gc(rest));
    ajs_temp_root_restore(vm, rs);
    return arr;
}

static AjsValue js_str_match_or_search(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, AjsWellKnownSym sym_id, bool is_match_all) {
    if (ajs_is_nullish(this_val)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "String.prototype method called on null or undefined");
    }
    AjsValue rx = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_is_nullish(rx)) {
        if (is_match_all) {
            bool is_re = false;
            if (!ajs_is_regexp_like(vm, rx, &is_re)) return AJS_VAL_EXCEPTION;
            if (is_re) {
                AjsValue fv = ajs_obj_get(vm, rx, ajs_val_from_gc(ajs_str_from_cstr(vm, "flags", true)), rx);
                if (vm->has_exception) return AJS_VAL_EXCEPTION;
                if (ajs_is_nullish(fv)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "String.prototype.matchAll non-global RegExp");
                AjsString *fs = ajs_to_js_string(vm, fv);
                if (!fs || vm->has_exception) return AJS_VAL_EXCEPTION;
                if (!strchr(fs->utf8, 'g')) {
                    return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "String.prototype.matchAll called with a non-global RegExp argument");
                }
            }
        }
        AjsValue mfn = ajs_obj_get(vm, rx, ajs_val_from_gc(vm->well_known_syms[sym_id]), rx);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        if (!ajs_is_nullish(mfn)) {
            if (!ajs_is_callable(vm, mfn)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp symbol method is not callable");
            return ajs_call_internal(vm, mfn, rx, 1, &this_val);
        }
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = ajs_to_js_string(vm, this_val);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    AjsString *pat = (rx == AJS_VAL_UNDEFINED) ? vm->str_empty : ajs_to_js_string(vm, rx);
    if (!pat || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(pat));
    AjsString *fl = is_match_all ? ajs_str_from_cstr(vm, "g", true) : vm->str_empty;
    AjsValue re = ajs_regexp_new_obj(vm, vm->active_realm, pat, fl);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, re);
    AjsValue mfn = ajs_obj_get(vm, re, ajs_val_from_gc(vm->well_known_syms[sym_id]), re);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsValue s_val = ajs_val_from_gc(s);
    AjsValue res = ajs_call_internal(vm, mfn, re, 1, &s_val);
    ajs_temp_root_restore(vm, rs);
    return res;
}
static AjsValue js_str_match(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u)    { (void)u; return js_str_match_or_search(vm, t, c, v, AJS_SYM_MATCH, false); }
static AjsValue js_str_matchAll(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)u; return js_str_match_or_search(vm, t, c, v, AJS_SYM_MATCH_ALL, true); }
static AjsValue js_str_search(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u)   { (void)u; return js_str_match_or_search(vm, t, c, v, AJS_SYM_SEARCH, false); }

/* String Iterator */
static AjsValue js_str_iter_init(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    AjsObject *it = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ITERATOR, vm->active_realm->string_iter_proto);
    ajs_temp_root_restore(vm, rs);
    if (!it) return AJS_VAL_EXCEPTION;
    it->u.iter.kind = AJS_ITER_STRING;
    it->u.iter.target = ajs_val_from_gc(s);
    it->u.iter.index = 0;
    it->u.iter.done = false;
    return ajs_val_from_gc(it);
}

static AjsValue js_str_iter_next(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "String Iterator.prototype.next called on incompatible receiver");
    }
    AjsObject *it = (AjsObject *)ajs_val_to_gc(this_val);
    if (it->kind != AJS_OBJ_ITERATOR || it->u.iter.kind != AJS_ITER_STRING) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "String Iterator.prototype.next called on incompatible receiver");
    }
    if (it->u.iter.done || !ajs_val_is_gc_kind(it->u.iter.target, AJS_GC_STRING)) {
        return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
    }
    AjsString *s = (AjsString *)ajs_val_to_gc(it->u.iter.target);
    if (it->u.iter.index >= s->len16) {
        it->u.iter.done = true;
        it->u.iter.target = AJS_VAL_UNDEFINED;
        return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
    }
    uint32_t adv = 1;
    (void)ajs_utf16_code_point_at(s->units, s->len16, it->u.iter.index, &adv);
    AjsString *ch = ajs_str_from_utf16(vm, s->units + it->u.iter.index, adv, false);
    it->u.iter.index += adv;
    return ch ? ajs_make_iter_result(vm, ajs_val_from_gc(ch), false) : AJS_VAL_EXCEPTION;
}

/* Annex B CreateHTML wrapper methods */
static const struct { const char *tag; const char *attr; } k_html_wrappers[] = {
    {"a", "name"}, {"big", NULL}, {"blink", NULL}, {"b", NULL}, {"tt", NULL},
    {"font", "color"}, {"font", "size"}, {"i", NULL}, {"a", "href"},
    {"small", NULL}, {"strike", NULL}, {"sub", NULL}, {"sup", NULL}
};
static AjsValue js_str_html(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    int32_t idx = vm->call_stack[vm->call_depth - 1u].native_fn_obj->u.fn.native_magic;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = req_coercible_str(vm, this_val);
    if (!s) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    const char *tag = k_html_wrappers[idx].tag;
    const char *attr = k_html_wrappers[idx].attr;
    AjsString *aval = NULL;
    if (attr) {
        aval = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
        if (!aval || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(aval));
    }
    size_t cap = s->len8 + (aval ? aval->len8 * 6u : 0u) + 64u;
    char *buf = (char *)malloc(cap);
    if (!buf) { ajs_temp_root_restore(vm, rs); return ajs_throw(vm, vm->oom_error); }
    size_t pos = (size_t)snprintf(buf, cap, "<%s", tag);
    if (attr) {
        pos += (size_t)snprintf(buf + pos, cap - pos, " %s=\"", attr);
        for (uint32_t i = 0; i < aval->len8; i++) {
            if (aval->utf8[i] == '"') { memcpy(buf + pos, "&quot;", 6); pos += 6; }
            else buf[pos++] = aval->utf8[i];
        }
        buf[pos++] = '"';
    }
    buf[pos++] = '>';
    memcpy(buf + pos, s->utf8, s->len8);
    pos += s->len8;
    pos += (size_t)snprintf(buf + pos, cap - pos, "</%s>", tag);
    AjsString *res = ajs_str_from_utf8(vm, buf, pos, false);
    free(buf);
    ajs_temp_root_restore(vm, rs);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}

/* ============================================================================
 * 2. Array Constructor, Static Methods (incl. Array.fromAsync) & Prototype
 * ============================================================================ */

static AjsValue array_species_create(AjsVm *vm, AjsValue orig, uint64_t len) {
    if (len > UINT32_MAX) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid array length");
    }
    bool is_arr = ajs_is_array(vm, orig);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (!is_arr) return ajs_new_array(vm, (uint32_t)len);

    AjsValue c = ajs_obj_get(vm, orig, ajs_val_from_gc(vm->str_constructor), orig);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (ajs_is_constructor_val(c) && ajs_val_is_gc_kind(c, AJS_GC_OBJECT)) {
        AjsObject *co = (AjsObject *)ajs_val_to_gc(c);
        if (co->realm && co->realm != vm->active_realm && c == co->realm->array_ctor) {
            c = AJS_VAL_UNDEFINED;
        }
    }
    if (ajs_val_is_gc_kind(c, AJS_GC_OBJECT)) {
        c = ajs_obj_get(vm, c, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_SPECIES]), c);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        if (c == AJS_VAL_NULL) c = AJS_VAL_UNDEFINED;
    }
    if (c == AJS_VAL_UNDEFINED) return ajs_new_array(vm, (uint32_t)len);
    if (!ajs_is_constructor_val(c)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Species constructor is not a constructor");
    }
    AjsValue arg = ajs_val_number((double)len);
    return ajs_construct_internal(vm, c, 1, &arg, c);
}

static bool create_data_prop_or_throw(AjsVm *vm, AjsValue obj, uint32_t idx, AjsValue val) {
    if (!ajs_val_is_gc_kind(obj, AJS_GC_OBJECT)) return false;
    AjsObject *o = (AjsObject *)ajs_val_to_gc(obj);
    if (o->kind == AJS_OBJ_ARRAY && o->extensible && idx < 65536u && o->live_props == 0) {
        if (idx >= o->u.arr.dense_cap) {
            uint32_t ncap = o->u.arr.dense_cap ? o->u.arr.dense_cap * 2u : 8u;
            if (ncap <= idx) ncap = idx + 1u;
            AjsValue *nd = (AjsValue *)realloc(o->u.arr.dense, (size_t)ncap * sizeof(AjsValue));
            if (nd) {
                for (uint32_t i = o->u.arr.dense_cap; i < ncap; i++) nd[i] = AJS_VAL_EMPTY;
                o->u.arr.dense = nd;
                o->u.arr.dense_cap = ncap;
            }
        }
        if (idx < o->u.arr.dense_cap) {
            if (idx >= o->u.arr.dense_len) {
                for (uint32_t i = o->u.arr.dense_len; i < idx; i++) o->u.arr.dense[i] = AJS_VAL_EMPTY;
                o->u.arr.dense_len = idx + 1u;
            }
            o->u.arr.dense[idx] = val;
            if (idx >= o->u.arr.length) o->u.arr.length = idx + 1u;
            return true;
        }
    }
    char ibuf[16];
    snprintf(ibuf, sizeof(ibuf), "%u", idx);
    AjsPropDesc d = {
        .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                 AJS_DESC_HAS_ENUMERABLE | AJS_DESC_ENUMERABLE |
                 AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
        .value = val
    };
    return ajs_obj_define_own_prop(vm, o, ajs_val_from_gc(ajs_str_from_cstr(vm, ibuf, true)), &d, true);
}

static bool arr_has_elem(AjsVm *vm, AjsObject *o, uint32_t idx, AjsValue *out_val) {
    if (o->kind == AJS_OBJ_ARRAY && idx < o->u.arr.dense_len && o->u.arr.dense[idx] != AJS_VAL_EMPTY) {
        if (out_val) *out_val = o->u.arr.dense[idx];
        return true;
    }
    char ibuf[16];
    snprintf(ibuf, sizeof(ibuf), "%u", idx);
    AjsString *ks = ajs_str_from_cstr(vm, ibuf, true);
    bool has = false;
    if (!ajs_obj_has_prop(vm, o, ajs_val_from_gc(ks), &has) || !has) return false;
    if (out_val) {
        *out_val = ajs_obj_get(vm, ajs_val_from_gc(o), ajs_val_from_gc(ks), ajs_val_from_gc(o));
        if (vm->has_exception) return false;
    }
    return true;
}

static AjsValue js_array_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsRealm *r = vm->active_realm;
    AjsValue nt = ajs_get_native_new_target(vm);
    AjsValue proto = r->array_proto;
    if (nt != AJS_VAL_UNDEFINED) {
        proto = sa_get_proto_from_ctor(vm, nt, r->array_proto);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
    }
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, proto);
    AjsObject *arr = ajs_obj_new(vm, r, AJS_OBJ_ARRAY, proto);
    if (!arr) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    arr->u.arr.length_writable = true;
    AjsValue av = ajs_val_from_gc(arr);
    ajs_temp_root_push(vm, av);

    if (argc == 1 && ajs_val_is_number(argv[0])) {
        double d = ajs_val_to_number_unchecked(argv[0]);
        uint32_t u = ajs_double_to_uint32(d);
        if ((double)u != d) {
            ajs_temp_root_restore(vm, rs);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid array length");
        }
        arr->u.arr.length = u;
    } else if (argc > 0) {
        for (int i = 0; i < argc; i++) {
            create_data_prop_or_throw(vm, av, (uint32_t)i, argv[i]);
        }
    }
    ajs_temp_root_restore(vm, rs);
    return av;
}

static AjsValue js_array_isArray(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) {
    (void)t; (void)u;
    bool b = ajs_is_array(vm, c > 0 ? v[0] : AJS_VAL_UNDEFINED);
    return vm->has_exception ? AJS_VAL_EXCEPTION : ajs_bool(b);
}

static AjsValue js_array_of(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue arr = AJS_VAL_UNDEFINED;
    if (ajs_is_constructor_val(this_val)) {
        AjsValue arg = ajs_val_number((double)argc);
        arr = ajs_construct_internal(vm, this_val, 1, &arg, this_val);
    } else {
        arr = ajs_new_array(vm, (uint32_t)argc);
    }
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, arr);
    for (int i = 0; i < argc; i++) {
        if (!create_data_prop_or_throw(vm, arr, (uint32_t)i, argv[i])) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    if (!ajs_set_prop_str(vm, arr, "length", ajs_val_number((double)argc))) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_restore(vm, rs);
    return arr;
}

static AjsValue js_array_from(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsValue items = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue mapfn = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    AjsValue this_arg = argc > 2 ? argv[2] : AJS_VAL_UNDEFINED;
    bool mapping = (mapfn != AJS_VAL_UNDEFINED);
    if (mapping && !ajs_is_callable(vm, mapfn)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Array.from mapFn must be a function");
    }
    if (ajs_is_nullish(items)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Array.from requires an array-like or iterable object");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue using_iter = ajs_obj_get(vm, items, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), items);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;

    if (!ajs_is_nullish(using_iter)) {
        if (!ajs_is_callable(vm, using_iter)) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "[Symbol.iterator] is not callable");
        }
        AjsValue A = ajs_is_constructor_val(this_val)
                     ? ajs_construct_internal(vm, this_val, 0, NULL, this_val)
                     : ajs_new_array(vm, 0);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        ajs_temp_root_push(vm, A);
        AjsValue iter = ajs_call_internal(vm, using_iter, items, 0, NULL);
        if (vm->has_exception || !ajs_is_object(vm, iter)) {
            ajs_temp_root_restore(vm, rs);
            if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator must be an object");
            return AJS_VAL_EXCEPTION;
        }
        ajs_temp_root_push(vm, iter);
        AjsValue next_fn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_next), iter);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, next_fn);
        uint32_t k = 0;
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
            if (ajs_to_bool(vm, dn)) {
                ajs_set_prop_str(vm, A, "length", ajs_val_number((double)k));
                ajs_temp_root_restore(vm, rs);
                return vm->has_exception ? AJS_VAL_EXCEPTION : A;
            }
            AjsValue kv = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            ajs_temp_root_push(vm, kv);
            if (mapping) {
                AjsValue margs[2] = { kv, ajs_val_number((double)k) };
                kv = ajs_call_internal(vm, mapfn, this_arg, 2, margs);
                if (vm->has_exception) goto close_af;
                ajs_temp_root_push(vm, kv);
            }
            if (!create_data_prop_or_throw(vm, A, k, kv)) goto close_af;
            k++;
            ajs_temp_root_restore(vm, ls);
            continue;
        close_af: {
                AjsValue exc = vm->exception;
                ajs_temp_root_push(vm, exc);
                ajs_clear_exception(vm);
                AjsValue rfn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_return), iter);
                if (!vm->has_exception && ajs_is_callable(vm, rfn)) ajs_call_internal(vm, rfn, iter, 0, NULL);
                ajs_throw(vm, exc);
                ajs_temp_root_restore(vm, rs);
                return AJS_VAL_EXCEPTION;
            }
        }
    }

    AjsObject *arr_like = ajs_to_object(vm, items);
    if (!arr_like) return AJS_VAL_EXCEPTION;
    AjsValue alv = ajs_val_from_gc(arr_like);
    ajs_temp_root_push(vm, alv);
    AjsValue lenv = ajs_obj_get(vm, alv, ajs_val_from_gc(vm->str_length), alv);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    uint64_t len = 0;
    if (!ajs_to_length(vm, lenv, &len)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (len > UINT32_MAX) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid array length");
    }
    AjsValue len_num = ajs_val_number((double)len);
    AjsValue A = ajs_is_constructor_val(this_val)
                 ? ajs_construct_internal(vm, this_val, 1, &len_num, this_val)
                 : ajs_new_array(vm, (uint32_t)len);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, A);

    for (uint32_t k = 0; k < (uint32_t)len; k++) {
        size_t ls = ajs_temp_root_save(vm);
        AjsValue kv = ajs_get_prop_index(vm, alv, k);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, kv);
        if (mapping) {
            AjsValue margs[2] = { kv, ajs_val_number((double)k) };
            kv = ajs_call_internal(vm, mapfn, this_arg, 2, margs);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            ajs_temp_root_push(vm, kv);
        }
        if (!create_data_prop_or_throw(vm, A, k, kv)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_restore(vm, ls);
    }
    ajs_set_prop_str(vm, A, "length", len_num);
    ajs_temp_root_restore(vm, rs);
    return vm->has_exception ? AJS_VAL_EXCEPTION : A;
}

/* ES2024/2026 Array.fromAsync (compiled cleanly onto async generator / async function intrinsic!) */
static AjsValue js_array_fromAsync(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    static const char k_src[] =
        "const C = this, items = arguments[0], mapfn = arguments[1], thisArg = arguments[2];"
        "if (items === null || items === undefined) throw new TypeError('Array.fromAsync requires an object');"
        "const mapping = mapfn !== undefined;"
        "if (mapping && typeof mapfn !== 'function') throw new TypeError('mapFn is not callable');"
        "const asyncIt = items[Symbol.asyncIterator];"
        "const syncIt = asyncIt == null ? items[Symbol.iterator] : null;"
        "if (asyncIt != null || syncIt != null) {"
        "  const A = typeof C === 'function' ? new C() : [];"
        "  let k = 0;"
        "  if (asyncIt != null) {"
        "    for await (let v of items) {"
        "      if (mapping) v = await mapfn.call(thisArg, v, k);"
        "      Object.defineProperty(A, k, {value: v, writable: true, enumerable: true, configurable: true});"
        "      k++;"
        "    }"
        "  } else {"
        "    for (let v of items) {"
        "      v = await v;"
        "      if (mapping) v = await mapfn.call(thisArg, v, k);"
        "      Object.defineProperty(A, k, {value: v, writable: true, enumerable: true, configurable: true});"
        "      k++;"
        "    }"
        "  }"
        "  A.length = k;"
        "  return A;"
        "}"
        "const O = Object(items);"
        "const len = O.length >>> 0;"
        "const A = typeof C === 'function' ? new C(len) : new Array(len);"
        "for (let k = 0; k < len; k++) {"
        "  let v = await O[k];"
        "  if (mapping) v = await mapfn.call(thisArg, v, k);"
        "  Object.defineProperty(A, k, {value: v, writable: true, enumerable: true, configurable: true});"
        "}"
        "A.length = len;"
        "return A;";
    AjsRealm *r = vm->active_realm;
    size_t rs = ajs_temp_root_save(vm);
    AjsBytecode *bc = ajs_compile_function_dynamic(vm, r, "asyncItems", 10, k_src, sizeof(k_src) - 1u, AJS_FN_BC_ASYNC);
    if (!bc) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(bc));
    AjsObject *fn = ajs_closure_new(vm, r, bc, r->global_lex_env, NULL);
    ajs_temp_root_push(vm, ajs_val_from_gc(fn));
    AjsValue res = ajs_call_internal(vm, ajs_val_from_gc(fn), this_val, argc, argv);
    ajs_temp_root_restore(vm, rs);
    return res;
}

/* Array.prototype Methods */
static bool obj_and_len(AjsVm *vm, AjsValue this_val, AjsObject **out_o, AjsValue *out_ov, uint64_t *out_len) {
    AjsObject *o = ajs_to_object(vm, this_val);
    if (!o) return false;
    *out_o = o;
    *out_ov = ajs_val_from_gc(o);
    if (o->kind == AJS_OBJ_ARRAY) {
        *out_len = o->u.arr.length;
        return true;
    }
    AjsValue lv = ajs_obj_get(vm, *out_ov, ajs_val_from_gc(vm->str_length), *out_ov);
    if (vm->has_exception) return false;
    return ajs_to_length(vm, lv, out_len);
}

static AjsValue js_arr_push(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    if (len + (uint64_t)argc > UINT64_C(9007199254740991)) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Array length exceeded MAX_SAFE_INTEGER");
    }
    for (int i = 0; i < argc; i++) {
        char kbuf[32];
        snprintf(kbuf, sizeof(kbuf), "%llu", (unsigned long long)(len + (uint64_t)i));
        if (!ajs_set_prop_str(vm, ov, kbuf, argv[i])) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    }
    len += (uint64_t)argc;
    if (!ajs_set_prop_str(vm, ov, "length", ajs_val_number((double)len))) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_restore(vm, rs);
    return ajs_val_number((double)len);
}

static AjsValue js_arr_pop(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    if (len == 0) {
        if (!ajs_set_prop_str(vm, ov, "length", ajs_val_number(0.0))) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_UNDEFINED;
    }
    uint64_t nlen = len - 1u;
    char kbuf[32];
    snprintf(kbuf, sizeof(kbuf), "%llu", (unsigned long long)nlen);
    AjsValue elem = ajs_get_prop_str(vm, ov, kbuf);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, elem);
    if (!ajs_delete_prop_str(vm, ov, kbuf) || !ajs_set_prop_str(vm, ov, "length", ajs_val_number((double)nlen))) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_restore(vm, rs);
    return elem;
}

static AjsValue js_arr_shift(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    if (len == 0) {
        if (!ajs_set_prop_str(vm, ov, "length", ajs_val_number(0.0))) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_UNDEFINED;
    }
    AjsValue first = ajs_get_prop_index(vm, ov, 0);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, first);
    for (uint32_t k = 1; k < (uint32_t)len; k++) {
        AjsValue ev = AJS_VAL_UNDEFINED;
        if (arr_has_elem(vm, o, k, &ev)) {
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            size_t ls = ajs_temp_root_save(vm);
            ajs_temp_root_push(vm, ev);
            if (!ajs_set_prop_index(vm, ov, k - 1u, ev)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            ajs_temp_root_restore(vm, ls);
        } else {
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            char kb[16]; snprintf(kb, sizeof(kb), "%u", k - 1u);
            if (!ajs_delete_prop_str(vm, ov, kb)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
    }
    char last_kb[32]; snprintf(last_kb, sizeof(last_kb), "%llu", (unsigned long long)(len - 1u));
    if (!ajs_delete_prop_str(vm, ov, last_kb) || !ajs_set_prop_str(vm, ov, "length", ajs_val_number((double)(len - 1u)))) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_restore(vm, rs);
    return first;
}

static AjsValue js_arr_unshift(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    if (argc > 0) {
        if (len + (uint64_t)argc > UINT64_C(9007199254740991)) {
            ajs_temp_root_restore(vm, rs);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Array length exceeded MAX_SAFE_INTEGER");
        }
        for (uint64_t k = len; k > 0; k--) {
            uint32_t from = (uint32_t)(k - 1u);
            uint32_t to = from + (uint32_t)argc;
            AjsValue ev = AJS_VAL_UNDEFINED;
            if (arr_has_elem(vm, o, from, &ev)) {
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                size_t ls = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, ev);
                if (!ajs_set_prop_index(vm, ov, to, ev)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                ajs_temp_root_restore(vm, ls);
            } else {
                char kb[16]; snprintf(kb, sizeof(kb), "%u", to);
                if (!ajs_delete_prop_str(vm, ov, kb)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            }
        }
        for (int j = 0; j < argc; j++) {
            if (!ajs_set_prop_index(vm, ov, (uint32_t)j, argv[j])) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
    }
    if (!ajs_set_prop_str(vm, ov, "length", ajs_val_number((double)(len + (uint64_t)argc)))) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_restore(vm, rs);
    return ajs_val_number((double)(len + (uint64_t)argc));
}

static uint64_t rel_idx_u64(double rel, uint64_t len) {
    double ti = ajs_double_to_integer_or_inf(rel);
    if (isinf(ti)) return ti < 0.0 ? 0u : len;
    if (ti < 0.0) {
        double s = (double)len + ti;
        return s > 0.0 ? (uint64_t)s : 0u;
    }
    return ti < (double)len ? (uint64_t)ti : len;
}

static AjsValue js_arr_slice(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    double st = 0.0, en = (double)len;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &st)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (argc > 1 && argv[1] != AJS_VAL_UNDEFINED) {
        if (!ajs_to_number(vm, argv[1], &en)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    }
    uint64_t k = rel_idx_u64(st, len);
    uint64_t fin = rel_idx_u64(en, len);
    uint64_t count = fin > k ? fin - k : 0u;
    AjsValue A = array_species_create(vm, ov, count);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, A);
    uint32_t n = 0;
    while (k < fin) {
        AjsValue kv = AJS_VAL_UNDEFINED;
        if (arr_has_elem(vm, o, (uint32_t)k, &kv)) {
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            size_t ls = ajs_temp_root_save(vm);
            ajs_temp_root_push(vm, kv);
            if (!create_data_prop_or_throw(vm, A, n, kv)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            ajs_temp_root_restore(vm, ls);
        }
        k++; n++;
    }
    if (!ajs_set_prop_str(vm, A, "length", ajs_val_number((double)n))) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_restore(vm, rs);
    return A;
}

static AjsValue js_arr_splice(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    double rel_st = 0.0;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &rel_st)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    uint64_t actual_start = rel_idx_u64(rel_st, len);
    uint64_t insert_count = 0, actual_del = 0;
    if (argc == 0) {
        insert_count = 0; actual_del = 0;
    } else if (argc == 1) {
        insert_count = 0; actual_del = len - actual_start;
    } else {
        insert_count = (uint64_t)(argc - 2);
        double dc = 0.0;
        if (!ajs_to_number(vm, argv[1], &dc)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        dc = ajs_double_to_integer_or_inf(dc);
        if (dc < 0.0) dc = 0.0;
        if (dc > (double)(len - actual_start)) dc = (double)(len - actual_start);
        actual_del = (uint64_t)dc;
    }
    AjsValue A = array_species_create(vm, ov, actual_del);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, A);
    for (uint64_t k = 0; k < actual_del; k++) {
        AjsValue kv = AJS_VAL_UNDEFINED;
        if (arr_has_elem(vm, o, (uint32_t)(actual_start + k), &kv)) {
            size_t ls = ajs_temp_root_save(vm);
            ajs_temp_root_push(vm, kv);
            if (!create_data_prop_or_throw(vm, A, (uint32_t)k, kv)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            ajs_temp_root_restore(vm, ls);
        }
    }
    ajs_set_prop_str(vm, A, "length", ajs_val_number((double)actual_del));

    if (insert_count < actual_del) {
        for (uint64_t k = actual_start; k < len - actual_del; k++) {
            AjsValue fv = AJS_VAL_UNDEFINED;
            uint32_t to = (uint32_t)(k + insert_count);
            if (arr_has_elem(vm, o, (uint32_t)(k + actual_del), &fv)) {
                size_t ls = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, fv);
                if (!ajs_set_prop_index(vm, ov, to, fv)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                ajs_temp_root_restore(vm, ls);
            } else {
                char kb[16]; snprintf(kb, sizeof(kb), "%u", to);
                if (!ajs_delete_prop_str(vm, ov, kb)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            }
        }
        for (uint64_t k = len; k > len - actual_del + insert_count; k--) {
            char kb[32]; snprintf(kb, sizeof(kb), "%llu", (unsigned long long)(k - 1u));
            if (!ajs_delete_prop_str(vm, ov, kb)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
    } else if (insert_count > actual_del) {
        for (uint64_t k = len - actual_del; k > actual_start; k--) {
            uint32_t from = (uint32_t)(k + actual_del - 1u);
            uint32_t to = (uint32_t)(k + insert_count - 1u);
            AjsValue fv = AJS_VAL_UNDEFINED;
            if (arr_has_elem(vm, o, from, &fv)) {
                size_t ls = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, fv);
                if (!ajs_set_prop_index(vm, ov, to, fv)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                ajs_temp_root_restore(vm, ls);
            } else {
                char kb[16]; snprintf(kb, sizeof(kb), "%u", to);
                if (!ajs_delete_prop_str(vm, ov, kb)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            }
        }
    }
    for (uint64_t j = 0; j < insert_count; j++) {
        if (!ajs_set_prop_index(vm, ov, (uint32_t)(actual_start + j), argv[2 + j])) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    uint64_t new_len = len - actual_del + insert_count;
    if (!ajs_set_prop_str(vm, ov, "length", ajs_val_number((double)new_len))) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_restore(vm, rs);
    return A;
}

/* ES2023 Array.prototype.toSpliced */
static AjsValue js_arr_toSpliced(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    double rel_st = 0.0;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &rel_st)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    uint64_t actual_start = rel_idx_u64(rel_st, len);
    uint64_t insert_count = argc > 2 ? (uint64_t)(argc - 2) : 0u;
    uint64_t skip_count = 0;
    if (argc == 1) skip_count = len - actual_start;
    else if (argc >= 2) {
        double sc = 0.0;
        if (!ajs_to_number(vm, argv[1], &sc)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        sc = ajs_double_to_integer_or_inf(sc);
        if (sc < 0.0) sc = 0.0;
        if (sc > (double)(len - actual_start)) sc = (double)(len - actual_start);
        skip_count = (uint64_t)sc;
    }
    uint64_t new_len = len + insert_count - skip_count;
    if (new_len > UINT32_MAX) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid array length");
    }
    AjsValue A = ajs_new_array(vm, (uint32_t)new_len);
    ajs_temp_root_push(vm, A);
    uint32_t i = 0;
    for (; i < (uint32_t)actual_start; i++) {
        AjsValue v = ajs_get_prop_index(vm, ov, i);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        size_t ls = ajs_temp_root_save(vm); ajs_temp_root_push(vm, v);
        create_data_prop_or_throw(vm, A, i, v);
        ajs_temp_root_restore(vm, ls);
    }
    for (uint32_t j = 0; j < (uint32_t)insert_count; j++, i++) {
        create_data_prop_or_throw(vm, A, i, argv[2 + j]);
    }
    uint32_t r = (uint32_t)(actual_start + skip_count);
    for (; i < (uint32_t)new_len; i++, r++) {
        AjsValue v = ajs_get_prop_index(vm, ov, r);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        size_t ls = ajs_temp_root_save(vm); ajs_temp_root_push(vm, v);
        create_data_prop_or_throw(vm, A, i, v);
        ajs_temp_root_restore(vm, ls);
    }
    ajs_temp_root_restore(vm, rs);
    return A;
}

static bool is_concat_spreadable(AjsVm *vm, AjsValue v, bool *out_sp) {
    *out_sp = false;
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return true;
    AjsValue sp = ajs_obj_get(vm, v, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_IS_CONCAT_SPREADABLE]), v);
    if (vm->has_exception) return false;
    if (sp != AJS_VAL_UNDEFINED) {
        *out_sp = ajs_to_bool(vm, sp);
        return true;
    }
    *out_sp = ajs_is_array(vm, v);
    return !vm->has_exception;
}

static AjsValue js_arr_concat(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *o = ajs_to_object(vm, this_val);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue ov = ajs_val_from_gc(o);
    ajs_temp_root_push(vm, ov);
    AjsValue A = array_species_create(vm, ov, 0);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, A);

    uint32_t n = 0;
    for (int idx = -1; idx < argc; idx++) {
        AjsValue E = (idx < 0) ? ov : argv[idx];
        bool spread = false;
        if (!is_concat_spreadable(vm, E, &spread)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (spread) {
            AjsObject *eo = (AjsObject *)ajs_val_to_gc(E);
            uint64_t elen = 0;
            AjsValue edummy = AJS_VAL_UNDEFINED;
            if (!obj_and_len(vm, E, &eo, &edummy, &elen)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            for (uint32_t k = 0; k < (uint32_t)elen; k++, n++) {
                AjsValue sub = AJS_VAL_UNDEFINED;
                if (arr_has_elem(vm, eo, k, &sub)) {
                    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                    size_t ls = ajs_temp_root_save(vm); ajs_temp_root_push(vm, sub);
                    if (!create_data_prop_or_throw(vm, A, n, sub)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                    ajs_temp_root_restore(vm, ls);
                }
            }
        } else {
            if (!create_data_prop_or_throw(vm, A, n++, E)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
    }
    if (!ajs_set_prop_str(vm, A, "length", ajs_val_number((double)n))) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_restore(vm, rs);
    return A;
}

static AjsValue js_arr_join(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);

    /* Guard against circular Array.prototype.join recursion */
    static AjsObject *s_join_stack[64];
    static int s_join_depth = 0;
    for (int i = 0; i < s_join_depth; i++) {
        if (s_join_stack[i] == o) { ajs_temp_root_restore(vm, rs); return ajs_val_from_gc(vm->str_empty); }
    }
    if (s_join_depth >= 64) { ajs_temp_root_restore(vm, rs); return ajs_val_from_gc(vm->str_empty); }

    AjsString *sep = (argc > 0 && argv[0] != AJS_VAL_UNDEFINED)
                     ? ajs_to_js_string(vm, argv[0])
                     : ajs_str_from_cstr(vm, ",", true);
    if (!sep || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(sep));

    s_join_stack[s_join_depth++] = o;
    AjsString *acc = vm->str_empty;
    for (uint32_t k = 0; k < (uint32_t)len; k++) {
        size_t ls = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, ajs_val_from_gc(acc));
        if (k > 0 && sep->len16 > 0) {
            acc = ajs_str_concat(vm, acc, sep);
            ajs_temp_root_push(vm, ajs_val_from_gc(acc));
        }
        AjsValue el = ajs_get_prop_index(vm, ov, k);
        if (vm->has_exception) { s_join_depth--; ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (!ajs_is_nullish(el)) {
            AjsString *es = ajs_to_js_string(vm, el);
            if (!es || vm->has_exception) { s_join_depth--; ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            ajs_temp_root_push(vm, ajs_val_from_gc(es));
            acc = ajs_str_concat(vm, acc, es);
        }
        ajs_temp_root_restore(vm, ls);
    }
    s_join_depth--;
    ajs_temp_root_restore(vm, rs);
    return acc ? ajs_val_from_gc(acc) : AJS_VAL_EXCEPTION;
}

static AjsValue js_arr_toString(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    AjsObject *o = ajs_to_object(vm, this_val);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue ov = ajs_val_from_gc(o);
    ajs_temp_root_push(vm, ov);
    AjsValue jfn = ajs_obj_get(vm, ov, ajs_val_from_gc(ajs_str_from_cstr(vm, "join", true)), ov);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (!ajs_is_callable(vm, jfn)) {
        jfn = ajs_obj_get(vm, vm->active_realm->object_proto, ajs_val_from_gc(vm->str_toString), vm->active_realm->object_proto);
    }
    AjsValue res = ajs_call_internal(vm, jfn, ov, 0, NULL);
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_arr_toLocaleString(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    AjsString *comma = ajs_str_from_cstr(vm, ",", true);
    AjsString *acc = vm->str_empty;
    for (uint32_t k = 0; k < (uint32_t)len; k++) {
        size_t ls = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, ajs_val_from_gc(acc));
        if (k > 0) { acc = ajs_str_concat(vm, acc, comma); ajs_temp_root_push(vm, ajs_val_from_gc(acc)); }
        AjsValue el = ajs_get_prop_index(vm, ov, k);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (!ajs_is_nullish(el)) {
            ajs_temp_root_push(vm, el);
            AjsValue fn = ajs_obj_get(vm, el, ajs_val_from_gc(ajs_str_from_cstr(vm, "toLocaleString", true)), el);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            if (!ajs_is_callable(vm, fn)) { ajs_temp_root_restore(vm, rs); return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "toLocaleString is not callable"); }
            AjsValue sv = ajs_call_internal(vm, fn, el, 0, NULL);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            AjsString *es = ajs_to_js_string(vm, sv);
            if (!es || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            ajs_temp_root_push(vm, ajs_val_from_gc(es));
            acc = ajs_str_concat(vm, acc, es);
        }
        ajs_temp_root_restore(vm, ls);
    }
    ajs_temp_root_restore(vm, rs);
    return acc ? ajs_val_from_gc(acc) : AJS_VAL_EXCEPTION;
}

static AjsValue js_arr_at(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    double idx = 0.0;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &idx)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    idx = ajs_double_to_integer_or_inf(idx);
    if (idx < 0.0) idx += (double)len;
    if (idx < 0.0 || idx >= (double)len) { ajs_temp_root_restore(vm, rs); return AJS_VAL_UNDEFINED; }
    AjsValue res = ajs_get_prop_index(vm, ov, (uint32_t)idx);
    ajs_temp_root_restore(vm, rs);
    return res;
}

/* ES2023 Array.prototype.with */
static AjsValue js_arr_with(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    double idx = 0.0;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &idx)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    idx = ajs_double_to_integer_or_inf(idx);
    if (idx < 0.0) idx += (double)len;
    if (idx < 0.0 || idx >= (double)len || len > UINT32_MAX) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid index");
    }
    uint32_t actual = (uint32_t)idx;
    AjsValue val = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    AjsValue A = ajs_new_array(vm, (uint32_t)len);
    ajs_temp_root_push(vm, A);
    for (uint32_t k = 0; k < (uint32_t)len; k++) {
        size_t ls = ajs_temp_root_save(vm);
        AjsValue kv = (k == actual) ? val : ajs_get_prop_index(vm, ov, k);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, kv);
        create_data_prop_or_throw(vm, A, k, kv);
        ajs_temp_root_restore(vm, ls);
    }
    ajs_temp_root_restore(vm, rs);
    return A;
}

static AjsValue js_arr_reverse(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    uint64_t mid = len / 2u;
    for (uint64_t lo = 0; lo < mid; lo++) {
        uint64_t hi = len - lo - 1u;
        AjsValue lv = AJS_VAL_UNDEFINED, hv = AJS_VAL_UNDEFINED;
        bool has_l = arr_has_elem(vm, o, (uint32_t)lo, &lv);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        size_t ls = ajs_temp_root_save(vm); ajs_temp_root_push(vm, lv);
        bool has_h = arr_has_elem(vm, o, (uint32_t)hi, &hv);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, hv);
        if (has_l && has_h) {
            if (!ajs_set_prop_index(vm, ov, (uint32_t)lo, hv) || !ajs_set_prop_index(vm, ov, (uint32_t)hi, lv)) {
                ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
            }
        } else if (!has_l && has_h) {
            char kb[16]; snprintf(kb, sizeof(kb), "%u", (uint32_t)hi);
            if (!ajs_set_prop_index(vm, ov, (uint32_t)lo, hv) || !ajs_delete_prop_str(vm, ov, kb)) {
                ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
            }
        } else if (has_l && !has_h) {
            char kb[16]; snprintf(kb, sizeof(kb), "%u", (uint32_t)lo);
            if (!ajs_delete_prop_str(vm, ov, kb) || !ajs_set_prop_index(vm, ov, (uint32_t)hi, lv)) {
                ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
            }
        }
        ajs_temp_root_restore(vm, ls);
    }
    ajs_temp_root_restore(vm, rs);
    return ov;
}

static AjsValue js_arr_toReversed(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    if (len > UINT32_MAX) return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid array length");
    ajs_temp_root_push(vm, ov);
    AjsValue A = ajs_new_array(vm, (uint32_t)len);
    ajs_temp_root_push(vm, A);
    for (uint32_t k = 0; k < (uint32_t)len; k++) {
        size_t ls = ajs_temp_root_save(vm);
        AjsValue kv = ajs_get_prop_index(vm, ov, (uint32_t)len - 1u - k);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, kv);
        create_data_prop_or_throw(vm, A, k, kv);
        ajs_temp_root_restore(vm, ls);
    }
    ajs_temp_root_restore(vm, rs);
    return A;
}

static AjsValue js_arr_fill(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    AjsValue val = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    double st = 0.0, en = (double)len;
    if (argc > 1 && !ajs_to_number(vm, argv[1], &st)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (argc > 2 && argv[2] != AJS_VAL_UNDEFINED && !ajs_to_number(vm, argv[2], &en)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    uint64_t k = rel_idx_u64(st, len), fin = rel_idx_u64(en, len);
    while (k < fin) {
        if (!ajs_set_prop_index(vm, ov, (uint32_t)k, val)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        k++;
    }
    ajs_temp_root_restore(vm, rs);
    return ov;
}

static AjsValue js_arr_copyWithin(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    double tg = 0.0, st = 0.0, en = (double)len;
    if (argc > 0 && !ajs_to_number(vm, argv[0], &tg)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (argc > 1 && !ajs_to_number(vm, argv[1], &st)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (argc > 2 && argv[2] != AJS_VAL_UNDEFINED && !ajs_to_number(vm, argv[2], &en)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    int64_t to = (int64_t)rel_idx_u64(tg, len);
    int64_t from = (int64_t)rel_idx_u64(st, len);
    int64_t fin = (int64_t)rel_idx_u64(en, len);
    int64_t count = fin - from;
    if ((int64_t)len - to < count) count = (int64_t)len - to;
    int64_t dir = 1;
    if (from < to && to < from + count) { dir = -1; from += count - 1; to += count - 1; }
    while (count > 0) {
        AjsValue fv = AJS_VAL_UNDEFINED;
        if (arr_has_elem(vm, o, (uint32_t)from, &fv)) {
            size_t ls = ajs_temp_root_save(vm); ajs_temp_root_push(vm, fv);
            if (!ajs_set_prop_index(vm, ov, (uint32_t)to, fv)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            ajs_temp_root_restore(vm, ls);
        } else {
            char kb[16]; snprintf(kb, sizeof(kb), "%u", (uint32_t)to);
            if (!ajs_delete_prop_str(vm, ov, kb)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
        from += dir; to += dir; count--;
    }
    ajs_temp_root_restore(vm, rs);
    return ov;
}

static AjsValue js_arr_search_elem(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, int mode) {
    /* mode: 0=indexOf, 1=lastIndexOf, 2=includes */
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    if (len == 0) { ajs_temp_root_restore(vm, rs); return mode == 2 ? AJS_VAL_FALSE : ajs_val_number(-1.0); }
    ajs_temp_root_push(vm, ov);
    AjsValue target = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;

    if (mode == 1) {
        double n = argc > 1 ? 0.0 : (double)(len - 1u);
        if (argc > 1 && !ajs_to_number(vm, argv[1], &n)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        n = ajs_double_to_integer_or_inf(n);
        int64_t k = (n >= 0.0) ? ((n < (double)(len - 1u)) ? (int64_t)n : (int64_t)(len - 1u))
                               : ((int64_t)len + (int64_t)n);
        for (; k >= 0; k--) {
            AjsValue ev = AJS_VAL_UNDEFINED;
            if (arr_has_elem(vm, o, (uint32_t)k, &ev)) {
                if (ajs_strict_eq(target, ev)) { ajs_temp_root_restore(vm, rs); return ajs_val_number((double)k); }
            }
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
        ajs_temp_root_restore(vm, rs);
        return ajs_val_number(-1.0);
    }

    double n = 0.0;
    if (argc > 1 && !ajs_to_number(vm, argv[1], &n)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    uint64_t k = rel_idx_u64(n, len);
    for (; k < len; k++) {
        if (mode == 2) {
            AjsValue ev = ajs_get_prop_index(vm, ov, (uint32_t)k);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            if (ajs_same_value_zero(target, ev)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_TRUE; }
        } else {
            AjsValue ev = AJS_VAL_UNDEFINED;
            if (arr_has_elem(vm, o, (uint32_t)k, &ev)) {
                if (ajs_strict_eq(target, ev)) { ajs_temp_root_restore(vm, rs); return ajs_val_number((double)k); }
            }
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
    }
    ajs_temp_root_restore(vm, rs);
    return mode == 2 ? AJS_VAL_FALSE : ajs_val_number(-1.0);
}
static AjsValue js_arr_indexOf(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u)     { (void)u; return js_arr_search_elem(vm, t, c, v, 0); }
static AjsValue js_arr_lastIndexOf(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)u; return js_arr_search_elem(vm, t, c, v, 1); }
static AjsValue js_arr_includes(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u)    { (void)u; return js_arr_search_elem(vm, t, c, v, 2); }

/* Higher-Order Array.prototype Methods, Stable Sort & Array Iterators */
static AjsValue js_arr_iter_hof(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    int32_t op = vm->call_stack[vm->call_depth - 1u].native_fn_obj->u.fn.native_magic;
    /* op: 0=forEach, 1=map, 2=filter, 3=some, 4=every, 5=find, 6=findIndex, 7=findLast, 8=findLastIndex */
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    AjsValue cb = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_is_callable(vm, cb)) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Callback must be a function");
    }
    AjsValue this_arg = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;

    AjsValue out_arr = AJS_VAL_UNDEFINED;
    if (op == 1 || op == 2) {
        out_arr = array_species_create(vm, ov, op == 1 ? len : 0u);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, out_arr);
    }

    if (op == 7 || op == 8) {
        for (int64_t k = (int64_t)len - 1; k >= 0; k--) {
            size_t ls = ajs_temp_root_save(vm);
            AjsValue kv = ajs_get_prop_index(vm, ov, (uint32_t)k);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            ajs_temp_root_push(vm, kv);
            AjsValue cargs[3] = { kv, ajs_val_number((double)k), ov };
            AjsValue r = ajs_call_internal(vm, cb, this_arg, 3, cargs);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            if (ajs_to_bool(vm, r)) {
                ajs_temp_root_restore(vm, rs);
                return op == 7 ? kv : ajs_val_number((double)k);
            }
            ajs_temp_root_restore(vm, ls);
        }
        ajs_temp_root_restore(vm, rs);
        return op == 7 ? AJS_VAL_UNDEFINED : ajs_val_number(-1.0);
    }

    uint32_t to_idx = 0;
    for (uint32_t k = 0; k < (uint32_t)len; k++) {
        size_t ls = ajs_temp_root_save(vm);
        AjsValue kv = AJS_VAL_UNDEFINED;
        bool present = (op == 5 || op == 6) ? true : arr_has_elem(vm, o, k, &kv);
        if (op == 5 || op == 6) kv = ajs_get_prop_index(vm, ov, k);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (!present) { ajs_temp_root_restore(vm, ls); continue; }
        ajs_temp_root_push(vm, kv);

        AjsValue cargs[3] = { kv, ajs_val_number((double)k), ov };
        AjsValue r = ajs_call_internal(vm, cb, this_arg, 3, cargs);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, r);

        if (op == 1) {
            if (!create_data_prop_or_throw(vm, out_arr, k, r)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        } else if (op == 2) {
            if (ajs_to_bool(vm, r)) {
                if (!create_data_prop_or_throw(vm, out_arr, to_idx++, kv)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            }
        } else if (op == 3 && ajs_to_bool(vm, r)) {
            ajs_temp_root_restore(vm, rs); return AJS_VAL_TRUE;
        } else if (op == 4 && !ajs_to_bool(vm, r)) {
            ajs_temp_root_restore(vm, rs); return AJS_VAL_FALSE;
        } else if ((op == 5 || op == 6) && ajs_to_bool(vm, r)) {
            ajs_temp_root_restore(vm, rs);
            return op == 5 ? kv : ajs_val_number((double)k);
        }
        ajs_temp_root_restore(vm, ls);
    }

    ajs_temp_root_restore(vm, rs);
    if (op == 0) return AJS_VAL_UNDEFINED;
    if (op == 1 || op == 2) return out_arr;
    if (op == 3) return AJS_VAL_FALSE;
    if (op == 4) return AJS_VAL_TRUE;
    return op == 5 ? AJS_VAL_UNDEFINED : ajs_val_number(-1.0);
}

static AjsValue js_arr_reduce_dir(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, bool right) {
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    AjsValue cb = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_is_callable(vm, cb)) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Callback must be a function");
    }
    bool has_acc = (argc > 1);
    AjsValue acc = has_acc ? argv[1] : AJS_VAL_UNDEFINED;
    int64_t k = right ? (int64_t)len - 1 : 0;
    int64_t step = right ? -1 : 1;

    while (right ? (k >= 0) : (k < (int64_t)len)) {
        AjsValue kv = AJS_VAL_UNDEFINED;
        if (arr_has_elem(vm, o, (uint32_t)k, &kv)) {
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            if (!has_acc) {
                acc = kv;
                has_acc = true;
            } else {
                ajs_temp_root_restore(vm, rs);
                ajs_temp_root_push(vm, ov);
                ajs_temp_root_push(vm, acc);
                ajs_temp_root_push(vm, kv);
                AjsValue cargs[4] = { acc, kv, ajs_val_number((double)k), ov };
                acc = ajs_call_internal(vm, cb, AJS_VAL_UNDEFINED, 4, cargs);
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            }
        } else if (vm->has_exception) {
            ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
        }
        k += step;
    }
    ajs_temp_root_restore(vm, rs);
    if (!has_acc) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Reduce of empty array with no initial value");
    return acc;
}
static AjsValue js_arr_reduce(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u)      { (void)u; return js_arr_reduce_dir(vm, t, c, v, false); }
static AjsValue js_arr_reduceRight(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)u; return js_arr_reduce_dir(vm, t, c, v, true); }

static bool flatten_into_array(AjsVm *vm, AjsValue target, AjsValue source, uint64_t src_len,
                               uint32_t *io_target_idx, double depth, AjsValue mapfn, AjsValue this_arg) {
    AjsObject *so = (AjsObject *)ajs_val_to_gc(source);
    for (uint32_t k = 0; k < (uint32_t)src_len; k++) {
        size_t ls = ajs_temp_root_save(vm);
        AjsValue el = AJS_VAL_UNDEFINED;
        if (!arr_has_elem(vm, so, k, &el)) {
            if (vm->has_exception) return false;
            continue;
        }
        ajs_temp_root_push(vm, el);
        if (mapfn != AJS_VAL_UNDEFINED) {
            AjsValue cargs[3] = { el, ajs_val_number((double)k), source };
            el = ajs_call_internal(vm, mapfn, this_arg, 3, cargs);
            if (vm->has_exception) { ajs_temp_root_restore(vm, ls); return false; }
            ajs_temp_root_push(vm, el);
        }
        bool should_flat = false;
        if (depth > 0.0) {
            should_flat = ajs_is_array(vm, el);
            if (vm->has_exception) { ajs_temp_root_restore(vm, ls); return false; }
        }
        if (should_flat) {
            AjsObject *eo = NULL; AjsValue eov = AJS_VAL_UNDEFINED; uint64_t el_len = 0;
            if (!obj_and_len(vm, el, &eo, &eov, &el_len)) { ajs_temp_root_restore(vm, ls); return false; }
            if (!flatten_into_array(vm, target, eov, el_len, io_target_idx, depth - 1.0, AJS_VAL_UNDEFINED, AJS_VAL_UNDEFINED)) {
                ajs_temp_root_restore(vm, ls); return false;
            }
        } else {
            if (!create_data_prop_or_throw(vm, target, (*io_target_idx)++, el)) {
                ajs_temp_root_restore(vm, ls); return false;
            }
        }
        ajs_temp_root_restore(vm, ls);
    }
    return true;
}

static AjsValue js_arr_flat(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    double depth = 1.0;
    if (argc > 0 && argv[0] != AJS_VAL_UNDEFINED) {
        if (!ajs_to_number(vm, argv[0], &depth)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        depth = ajs_double_to_integer_or_inf(depth);
        if (depth < 0.0) depth = 0.0;
    }
    AjsValue A = array_species_create(vm, ov, 0);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, A);
    uint32_t tidx = 0;
    if (!flatten_into_array(vm, A, ov, len, &tidx, depth, AJS_VAL_UNDEFINED, AJS_VAL_UNDEFINED)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_restore(vm, rs);
    return A;
}

static AjsValue js_arr_flatMap(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ov);
    AjsValue mapfn = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_is_callable(vm, mapfn)) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Mapper function is not callable");
    }
    AjsValue this_arg = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    AjsValue A = array_species_create(vm, ov, 0);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, A);
    uint32_t tidx = 0;
    if (!flatten_into_array(vm, A, ov, len, &tidx, 1.0, mapfn, this_arg)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_restore(vm, rs);
    return A;
}

/* Guaranteed Stable Merge Sort for Array.prototype.sort & toSorted */
static int sort_compare_vals(AjsVm *vm, AjsValue x, AjsValue y, AjsValue cmpfn) {
    if (x == AJS_VAL_UNDEFINED && y == AJS_VAL_UNDEFINED) return 0;
    if (x == AJS_VAL_UNDEFINED) return 1;
    if (y == AJS_VAL_UNDEFINED) return -1;
    if (cmpfn != AJS_VAL_UNDEFINED) {
        AjsValue cargs[2] = { x, y };
        AjsValue rv = ajs_call_internal(vm, cmpfn, AJS_VAL_UNDEFINED, 2, cargs);
        if (vm->has_exception) return 0;
        double d = 0.0;
        if (!ajs_to_number(vm, rv, &d) || isnan(d) || d == 0.0) return 0;
        return d < 0.0 ? -1 : 1;
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsString *sx = ajs_to_js_string(vm, x);
    if (!sx || vm->has_exception) { ajs_temp_root_restore(vm, rs); return 0; }
    ajs_temp_root_push(vm, ajs_val_from_gc(sx));
    AjsString *sy = ajs_to_js_string(vm, y);
    if (!sy || vm->has_exception) { ajs_temp_root_restore(vm, rs); return 0; }
    int c = ajs_str_cmp_utf16(sx, sy);
    ajs_temp_root_restore(vm, rs);
    return c;
}

static bool merge_sort_values(AjsVm *vm, AjsValue *arr, AjsValue *tmp, uint32_t n, AjsValue cmpfn) {
    for (uint32_t width = 1; width < n; width *= 2u) {
        for (uint32_t i = 0; i < n; i += 2u * width) {
            uint32_t mid = (i + width < n) ? (i + width) : n;
            uint32_t right = (i + 2u * width < n) ? (i + 2u * width) : n;
            uint32_t l = i, r = mid, k = i;
            while (l < mid && r < right) {
                int c = sort_compare_vals(vm, arr[l], arr[r], cmpfn);
                if (vm->has_exception) return false;
                if (c <= 0) tmp[k++] = arr[l++]; /* <= guarantees stability! */
                else tmp[k++] = arr[r++];
            }
            while (l < mid) tmp[k++] = arr[l++];
            while (r < right) tmp[k++] = arr[r++];
            for (uint32_t m = i; m < right; m++) arr[m] = tmp[m];
        }
    }
    return true;
}

static AjsValue arr_sort_impl(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, bool copy_mode) {
    AjsValue cmpfn = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (cmpfn != AJS_VAL_UNDEFINED && !ajs_is_callable(vm, cmpfn)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "The comparison function must be either a function or undefined");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *o = NULL; AjsValue ov = AJS_VAL_UNDEFINED; uint64_t len = 0;
    if (!obj_and_len(vm, this_val, &o, &ov, &len)) return AJS_VAL_EXCEPTION;
    if (len > UINT32_MAX) return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid array length");
    ajs_temp_root_push(vm, ov);

    uint32_t cap = (uint32_t)len;
    AjsValue *items = cap > 0 ? (AjsValue *)malloc((size_t)cap * sizeof(AjsValue)) : NULL;
    AjsValue *tmp   = cap > 0 ? (AjsValue *)malloc((size_t)cap * sizeof(AjsValue)) : NULL;
    if (cap > 0 && (!items || !tmp)) { free(items); free(tmp); ajs_temp_root_restore(vm, rs); return ajs_throw(vm, vm->oom_error); }

    uint32_t count = 0;
    for (uint32_t k = 0; k < cap; k++) {
        if (copy_mode) {
            AjsValue kv = ajs_get_prop_index(vm, ov, k);
            if (vm->has_exception) { free(items); free(tmp); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            items[count++] = kv;
            ajs_temp_root_push(vm, kv);
        } else {
            AjsValue kv = AJS_VAL_UNDEFINED;
            if (arr_has_elem(vm, o, k, &kv)) {
                if (vm->has_exception) { free(items); free(tmp); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                items[count++] = kv;
                ajs_temp_root_push(vm, kv);
            } else if (vm->has_exception) {
                free(items); free(tmp); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
            }
        }
    }

    if (count > 1 && !merge_sort_values(vm, items, tmp, count, cmpfn)) {
        free(items); free(tmp); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }

    AjsValue out = copy_mode ? ajs_new_array(vm, cap) : ov;
    if (copy_mode) ajs_temp_root_push(vm, out);

    for (uint32_t j = 0; j < count; j++) {
        if (copy_mode) create_data_prop_or_throw(vm, out, j, items[j]);
        else if (!ajs_set_prop_index(vm, out, j, items[j])) { free(items); free(tmp); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    }
    if (!copy_mode) {
        for (uint32_t j = count; j < cap; j++) {
            char kb[16]; snprintf(kb, sizeof(kb), "%u", j);
            if (!ajs_delete_prop_str(vm, out, kb)) { free(items); free(tmp); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
    }
    free(items); free(tmp);
    ajs_temp_root_restore(vm, rs);
    return out;
}
static AjsValue js_arr_sort(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u)     { (void)u; return arr_sort_impl(vm, t, c, v, false); }
static AjsValue js_arr_toSorted(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)u; return arr_sort_impl(vm, t, c, v, true); }

/* Array Iterator */
static AjsValue js_arr_iter_create(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    int32_t kind = vm->call_stack[vm->call_depth - 1u].native_fn_obj->u.fn.native_magic;
    AjsObject *o = ajs_to_object(vm, this_val);
    if (!o) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(o));
    AjsObject *it = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ITERATOR, vm->active_realm->array_iter_proto);
    ajs_temp_root_restore(vm, rs);
    if (!it) return AJS_VAL_EXCEPTION;
    it->u.iter.kind = (AjsIterKind)kind;
    it->u.iter.target = ajs_val_from_gc(o);
    it->u.iter.index = 0;
    it->u.iter.done = false;
    return ajs_val_from_gc(it);
}

static AjsValue js_arr_iter_next(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Array Iterator.prototype.next called on incompatible receiver");
    }
    AjsObject *it = (AjsObject *)ajs_val_to_gc(this_val);
    if (it->kind != AJS_OBJ_ITERATOR || it->u.iter.kind > AJS_ITER_ARRAY_ENTRIES) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Array Iterator.prototype.next called on incompatible receiver");
    }
    if (it->u.iter.done || !ajs_val_is_gc_kind(it->u.iter.target, AJS_GC_OBJECT)) {
        return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
    }
    AjsObject *t = (AjsObject *)ajs_val_to_gc(it->u.iter.target);
    uint64_t len = 0;
    if (t->kind == AJS_OBJ_TYPED_ARRAY) {
        size_t elen = 0, boff = 0;
        if (!ajs_ta_check_valid_len(vm, t, &elen, &boff)) return AJS_VAL_EXCEPTION;
        len = elen;
    } else {
        AjsValue tv = it->u.iter.target;
        if (!obj_and_len(vm, tv, &t, &tv, &len)) return AJS_VAL_EXCEPTION;
    }
    uint32_t idx = it->u.iter.index;
    if (idx >= len) {
        it->u.iter.done = true;
        it->u.iter.target = AJS_VAL_UNDEFINED;
        return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
    }
    it->u.iter.index = idx + 1u;
    if (it->u.iter.kind == AJS_ITER_ARRAY_KEYS) {
        return ajs_make_iter_result(vm, ajs_val_number((double)idx), false);
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue ev = ajs_get_prop_index(vm, it->u.iter.target, idx);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (it->u.iter.kind == AJS_ITER_ARRAY_VALUES) {
        return ajs_make_iter_result(vm, ev, false);
    }
    ajs_temp_root_push(vm, ev);
    AjsValue pair = ajs_new_array(vm, 2);
    ajs_temp_root_push(vm, pair);
    create_data_prop_or_throw(vm, pair, 0, ajs_val_number((double)idx));
    create_data_prop_or_throw(vm, pair, 1, ev);
    AjsValue res = ajs_make_iter_result(vm, pair, false);
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_species_this(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)vm; (void)c; (void)v; (void)u;
    return this_val;
}

/* ============================================================================
 * 3. ES2025/2026 Iterator Constructor & Iterator Helpers
 * ============================================================================ */

static void iter_close_quiet(AjsVm *vm, AjsValue iter, bool preserve_exc) {
    AjsValue saved_exc = vm->exception;
    bool had_exc = vm->has_exception;
    size_t rs = ajs_temp_root_save(vm);
    if (had_exc) ajs_temp_root_push(vm, saved_exc);
    ajs_clear_exception(vm);
    AjsValue rfn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_return), iter);
    if (!vm->has_exception && !ajs_is_nullish(rfn) && ajs_is_callable(vm, rfn)) {
        AjsValue rv = ajs_call_internal(vm, rfn, iter, 0, NULL);
        if (!preserve_exc && !vm->has_exception && !ajs_is_object(vm, rv)) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator.return() result is not an object");
        }
    }
    if (preserve_exc && had_exc) {
        ajs_throw(vm, saved_exc);
    }
    ajs_temp_root_restore(vm, rs);
}

static bool get_iter_direct(AjsVm *vm, AjsValue obj, AjsValue *out_next) {
    if (!ajs_val_is_gc_kind(obj, AJS_GC_OBJECT)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator method called on non-object");
        return false;
    }
    *out_next = ajs_obj_get(vm, obj, ajs_val_from_gc(vm->str_next), obj);
    return !vm->has_exception;
}

static AjsValue js_iterator_ctor(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)this_val; (void)c; (void)v; (void)u;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt == AJS_VAL_UNDEFINED || nt == vm->active_realm->iterator_ctor) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Abstract class Iterator not directly constructable");
    }
    AjsValue proto = sa_get_proto_from_ctor(vm, nt, vm->active_realm->iterator_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    AjsObject *o = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ORDINARY, proto);
    return o ? ajs_val_from_gc(o) : AJS_VAL_EXCEPTION;
}

static AjsValue js_iterator_from(AjsVm *vm, AjsValue t, int argc, const AjsValue *argv, void *u) {
    (void)t; (void)u;
    AjsValue O = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    size_t rs = ajs_temp_root_save(vm);
    if (ajs_val_is_gc_kind(O, AJS_GC_STRING)) {
        AjsValue fn = ajs_obj_get(vm, O, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), O);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        return ajs_call_internal(vm, fn, O, 0, NULL);
    }
    if (!ajs_val_is_gc_kind(O, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator.from called on non-object");
    }
    AjsValue using_it = ajs_obj_get(vm, O, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), O);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    AjsValue iter = O;
    if (!ajs_is_nullish(using_it)) {
        if (!ajs_is_callable(vm, using_it)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "[Symbol.iterator] is not callable");
        iter = ajs_call_internal(vm, using_it, O, 0, NULL);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        if (!ajs_val_is_gc_kind(iter, AJS_GC_OBJECT)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator is not an object");
    }
    ajs_temp_root_push(vm, iter);
    AjsValue next_m = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_next), iter);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, next_m);

    /* Check if iter already inherits from %IteratorPrototype% */
    AjsValue cur = iter;
    for (int d = 0; d < 128 && ajs_val_is_gc_kind(cur, AJS_GC_OBJECT); d++) {
        if (cur == vm->active_realm->iterator_proto) {
            ajs_temp_root_restore(vm, rs);
            return iter;
        }
        AjsValue p = AJS_VAL_NULL;
        if (!ajs_obj_get_proto(vm, (AjsObject *)ajs_val_to_gc(cur), &p)) break;
        cur = p;
    }

    AjsObject *wrap = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ITERATOR, vm->active_realm->iter_helper_proto);
    ajs_temp_root_restore(vm, rs);
    if (!wrap) return AJS_VAL_EXCEPTION;
    wrap->u.iter.kind = AJS_ITER_WRAP_VALID;
    wrap->u.iter.target = iter;
    wrap->u.iter.next_method = next_m;
    wrap->u.iter.done = false;
    return ajs_val_from_gc(wrap);
}

static AjsValue js_iter_create_helper(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    int32_t kind = vm->call_stack[vm->call_depth - 1u].native_fn_obj->u.fn.native_magic;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator helper called on non-object");
    }
    AjsValue arg0 = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    uint64_t lim = 0;
    if (kind == AJS_ITER_HELPER_MAP || kind == AJS_ITER_HELPER_FILTER || kind == AJS_ITER_HELPER_FLATMAP) {
        if (!ajs_is_callable(vm, arg0)) {
            iter_close_quiet(vm, this_val, false);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator helper callback must be callable");
        }
    } else {
        double d = 0.0;
        if (!ajs_to_number(vm, arg0, &d)) {
            iter_close_quiet(vm, this_val, true);
            return AJS_VAL_EXCEPTION;
        }
        if (isnan(d) || d < 0.0) {
            iter_close_quiet(vm, this_val, false);
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Iterator limit must be non-negative");
        }
        double ti = ajs_double_to_integer_or_inf(d);
        lim = isinf(ti) ? UINT64_MAX : (uint64_t)ti;
    }
    AjsValue next_m = AJS_VAL_UNDEFINED;
    if (!get_iter_direct(vm, this_val, &next_m)) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, next_m);
    AjsObject *h = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ITERATOR, vm->active_realm->iter_helper_proto);
    ajs_temp_root_restore(vm, rs);
    if (!h) return AJS_VAL_EXCEPTION;
    h->u.iter.kind = (AjsIterKind)kind;
    h->u.iter.target = this_val;
    h->u.iter.next_method = next_m;
    h->u.iter.helper_fn = arg0;
    h->u.iter.limit = lim;
    h->u.iter.counter = 0;
    h->u.iter.inner_iter = AJS_VAL_UNDEFINED;
    h->u.iter.inner_next = AJS_VAL_UNDEFINED;
    h->u.iter.done = false;
    return ajs_val_from_gc(h);
}

static AjsValue js_iter_helper_next(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator Helper.prototype.next incompatible receiver");
    }
    AjsObject *h = (AjsObject *)ajs_val_to_gc(this_val);
    if (h->kind != AJS_OBJ_ITERATOR || h->u.iter.kind < AJS_ITER_HELPER_MAP) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator Helper.prototype.next incompatible receiver");
    }
    if (h->u.iter.done) return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);

    if (h->u.iter.kind == AJS_ITER_WRAP_VALID) {
        return ajs_call_internal(vm, h->u.iter.next_method, h->u.iter.target, 0, NULL);
    }

    size_t rs = ajs_temp_root_save(vm);
    for (;;) {
        if (h->u.iter.kind == AJS_ITER_HELPER_FLATMAP && h->u.iter.inner_iter != AJS_VAL_UNDEFINED) {
            AjsValue istep = ajs_call_internal(vm, h->u.iter.inner_next, h->u.iter.inner_iter, 0, NULL);
            if (vm->has_exception || !ajs_is_object(vm, istep)) {
                if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator result is not an object");
                h->u.iter.done = true;
                iter_close_quiet(vm, h->u.iter.target, true);
                ajs_temp_root_restore(vm, rs);
                return AJS_VAL_EXCEPTION;
            }
            ajs_temp_root_push(vm, istep);
            AjsValue idn = ajs_obj_get(vm, istep, ajs_val_from_gc(vm->str_done), istep);
            if (vm->has_exception) { h->u.iter.done = true; iter_close_quiet(vm, h->u.iter.target, true); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            if (!ajs_to_bool(vm, idn)) {
                AjsValue iv = ajs_obj_get(vm, istep, ajs_val_from_gc(vm->str_value), istep);
                ajs_temp_root_restore(vm, rs);
                if (vm->has_exception) { h->u.iter.done = true; iter_close_quiet(vm, h->u.iter.target, true); return AJS_VAL_EXCEPTION; }
                return ajs_make_iter_result(vm, iv, false);
            }
            h->u.iter.inner_iter = AJS_VAL_UNDEFINED;
            h->u.iter.inner_next = AJS_VAL_UNDEFINED;
            ajs_temp_root_restore(vm, rs);
        }

        if (h->u.iter.kind == AJS_ITER_HELPER_TAKE && h->u.iter.counter >= h->u.iter.limit) {
            h->u.iter.done = true;
            iter_close_quiet(vm, h->u.iter.target, false);
            ajs_temp_root_restore(vm, rs);
            return vm->has_exception ? AJS_VAL_EXCEPTION : ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
        }

        AjsValue step = ajs_call_internal(vm, h->u.iter.next_method, h->u.iter.target, 0, NULL);
        if (vm->has_exception || !ajs_is_object(vm, step)) {
            h->u.iter.done = true;
            ajs_temp_root_restore(vm, rs);
            if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator result is not an object");
            return AJS_VAL_EXCEPTION;
        }
        ajs_temp_root_push(vm, step);
        AjsValue dn = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_done), step);
        if (vm->has_exception) { h->u.iter.done = true; ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (ajs_to_bool(vm, dn)) {
            h->u.iter.done = true;
            ajs_temp_root_restore(vm, rs);
            return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
        }
        AjsValue val = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
        if (vm->has_exception) { h->u.iter.done = true; ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, val);

        uint64_t idx = h->u.iter.counter++;
        if (h->u.iter.kind == AJS_ITER_HELPER_TAKE) {
            ajs_temp_root_restore(vm, rs);
            return ajs_make_iter_result(vm, val, false);
        }
        if (h->u.iter.kind == AJS_ITER_HELPER_DROP) {
            if (idx < h->u.iter.limit) { ajs_temp_root_restore(vm, rs); continue; }
            ajs_temp_root_restore(vm, rs);
            return ajs_make_iter_result(vm, val, false);
        }
        AjsValue cargs[2] = { val, ajs_val_number((double)idx) };
        AjsValue mapped = ajs_call_internal(vm, h->u.iter.helper_fn, AJS_VAL_UNDEFINED, 2, cargs);
        if (vm->has_exception) {
            h->u.iter.done = true;
            iter_close_quiet(vm, h->u.iter.target, true);
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
        if (h->u.iter.kind == AJS_ITER_HELPER_MAP) {
            ajs_temp_root_restore(vm, rs);
            return ajs_make_iter_result(vm, mapped, false);
        }
        if (h->u.iter.kind == AJS_ITER_HELPER_FILTER) {
            if (ajs_to_bool(vm, mapped)) {
                ajs_temp_root_restore(vm, rs);
                return ajs_make_iter_result(vm, val, false);
            }
            ajs_temp_root_restore(vm, rs);
            continue;
        }
        if (h->u.iter.kind == AJS_ITER_HELPER_FLATMAP) {
            ajs_temp_root_push(vm, mapped);
            if (!ajs_val_is_gc_kind(mapped, AJS_GC_OBJECT)) {
                h->u.iter.done = true;
                iter_close_quiet(vm, h->u.iter.target, false);
                ajs_temp_root_restore(vm, rs);
                return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator.flatMap mapper must return an iterable object");
            }
            AjsValue it_m = ajs_obj_get(vm, mapped, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), mapped);
            if (vm->has_exception) { h->u.iter.done = true; iter_close_quiet(vm, h->u.iter.target, true); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            AjsValue inner = !ajs_is_nullish(it_m) ? ajs_call_internal(vm, it_m, mapped, 0, NULL) : mapped;
            if (vm->has_exception || !ajs_val_is_gc_kind(inner, AJS_GC_OBJECT)) {
                h->u.iter.done = true;
                iter_close_quiet(vm, h->u.iter.target, true);
                ajs_temp_root_restore(vm, rs);
                if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "flatMap inner iterator is not an object");
                return AJS_VAL_EXCEPTION;
            }
            ajs_temp_root_push(vm, inner);
            AjsValue inext = ajs_obj_get(vm, inner, ajs_val_from_gc(vm->str_next), inner);
            if (vm->has_exception) { h->u.iter.done = true; iter_close_quiet(vm, h->u.iter.target, true); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            h->u.iter.inner_iter = inner;
            h->u.iter.inner_next = inext;
            ajs_temp_root_restore(vm, rs);
        }
    }
}

static AjsValue js_iter_helper_return(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator Helper.prototype.return incompatible receiver");
    }
    AjsObject *h = (AjsObject *)ajs_val_to_gc(this_val);
    if (h->kind != AJS_OBJ_ITERATOR || h->u.iter.kind < AJS_ITER_HELPER_MAP) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator Helper.prototype.return incompatible receiver");
    }
    if (!h->u.iter.done) {
        h->u.iter.done = true;
        if (h->u.iter.inner_iter != AJS_VAL_UNDEFINED) {
            iter_close_quiet(vm, h->u.iter.inner_iter, false);
            h->u.iter.inner_iter = AJS_VAL_UNDEFINED;
            if (vm->has_exception) { iter_close_quiet(vm, h->u.iter.target, true); return AJS_VAL_EXCEPTION; }
        }
        iter_close_quiet(vm, h->u.iter.target, false);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
    }
    return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
}

/* Eager Iterator.prototype consuming helpers: toArray, forEach, some, every, find, reduce */
static AjsValue js_iter_consume(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    int32_t op = vm->call_stack[vm->call_depth - 1u].native_fn_obj->u.fn.native_magic;
    /* op: 0=toArray, 1=forEach, 2=some, 3=every, 4=find, 5=reduce */
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator method called on non-object");
    }
    AjsValue cb = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (op >= 1 && !ajs_is_callable(vm, cb)) {
        iter_close_quiet(vm, this_val, false);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Callback must be callable");
    }
    AjsValue next_m = AJS_VAL_UNDEFINED;
    if (!get_iter_direct(vm, this_val, &next_m)) return AJS_VAL_EXCEPTION;

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, next_m);
    AjsValue arr = (op == 0) ? ajs_new_array(vm, 0) : AJS_VAL_UNDEFINED;
    if (op == 0) ajs_temp_root_push(vm, arr);

    bool has_acc = (op == 5 && argc > 1);
    AjsValue acc = has_acc ? argv[1] : AJS_VAL_UNDEFINED;
    uint64_t counter = 0;

    for (;;) {
        size_t ls = ajs_temp_root_save(vm);
        if (op == 5) ajs_temp_root_push(vm, acc);
        AjsValue step = ajs_call_internal(vm, next_m, this_val, 0, NULL);
        if (vm->has_exception || !ajs_is_object(vm, step)) {
            ajs_temp_root_restore(vm, rs);
            if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator result is not an object");
            return AJS_VAL_EXCEPTION;
        }
        ajs_temp_root_push(vm, step);
        AjsValue dn = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_done), step);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (ajs_to_bool(vm, dn)) {
            ajs_temp_root_restore(vm, rs);
            if (op == 0) return arr;
            if (op == 1 || op == 4) return AJS_VAL_UNDEFINED;
            if (op == 2) return AJS_VAL_FALSE;
            if (op == 3) return AJS_VAL_TRUE;
            if (!has_acc) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Reduce of empty iterator with no initial value");
            return acc;
        }
        AjsValue val = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, val);

        if (op == 0) {
            create_data_prop_or_throw(vm, arr, (uint32_t)counter++, val);
        } else if (op == 5 && !has_acc) {
            acc = val;
            has_acc = true;
            counter++;
        } else if (op == 5) {
            AjsValue cargs[3] = { acc, val, ajs_val_number((double)counter++) };
            acc = ajs_call_internal(vm, cb, AJS_VAL_UNDEFINED, 3, cargs);
            if (vm->has_exception) { iter_close_quiet(vm, this_val, true); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        } else {
            AjsValue cargs[2] = { val, ajs_val_number((double)counter++) };
            AjsValue r = ajs_call_internal(vm, cb, AJS_VAL_UNDEFINED, 2, cargs);
            if (vm->has_exception) { iter_close_quiet(vm, this_val, true); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            bool b = ajs_to_bool(vm, r);
            if (op == 2 && b)  { iter_close_quiet(vm, this_val, false); ajs_temp_root_restore(vm, rs); return vm->has_exception ? AJS_VAL_EXCEPTION : AJS_VAL_TRUE; }
            if (op == 3 && !b) { iter_close_quiet(vm, this_val, false); ajs_temp_root_restore(vm, rs); return vm->has_exception ? AJS_VAL_EXCEPTION : AJS_VAL_FALSE; }
            if (op == 4 && b)  { iter_close_quiet(vm, this_val, false); ajs_temp_root_restore(vm, rs); return vm->has_exception ? AJS_VAL_EXCEPTION : val; }
        }
        ajs_temp_root_restore(vm, ls);
    }
}

void ajs_init_string_array_builtins(AjsVm *vm, AjsRealm *r) {
    uint32_t gflags = AJS_DESC_WRITABLE | AJS_DESC_CONFIGURABLE;

    /* 1. String & %StringIteratorPrototype% */
    AjsObject *sproto = ajs_obj_new(vm, r, AJS_OBJ_STRING_OBJ, r->object_proto);
    sproto->u.prim_val = ajs_val_from_gc(vm->str_empty);
    r->string_proto = ajs_val_from_gc(sproto);
    sa_def_prop(vm, r->string_proto, "length", ajs_val_number(0.0), 0);

    r->string_ctor = sa_make_fn(vm, r, js_string_ctor, "String", 1, true, -20, r->function_proto);
    sa_link_ctor_proto(vm, r->string_ctor, r->string_proto);
    sa_def_prop(vm, r->global_obj, "String", r->string_ctor, gflags);

    ajs_def_native_method(vm, r, r->string_ctor, "fromCharCode", js_string_fromCharCode, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_ctor, "fromCodePoint", js_string_fromCodePoint, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_ctor, "raw", js_string_raw, 1, AJS_METHOD_FLAGS, 0);

    ajs_def_native_method(vm, r, r->string_proto, "toString", js_str_toString, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "valueOf", js_str_toString, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "charAt", js_str_charAt, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "charCodeAt", js_str_charCodeAt, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "codePointAt", js_str_codePointAt, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "at", js_str_at, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "concat", js_str_concat, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "indexOf", js_str_indexOf, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "lastIndexOf", js_str_lastIndexOf, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "includes", js_str_includes, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "startsWith", js_str_startsWith, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "endsWith", js_str_endsWith, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "slice", js_str_slice, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "substring", js_str_substring, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "substr", js_str_substr, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "toLowerCase", js_str_toLower, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "toLocaleLowerCase", js_str_toLower, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "toUpperCase", js_str_toUpper, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "toLocaleUpperCase", js_str_toUpper, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "trim", js_str_trim, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "trimStart", js_str_trimStart, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "trimLeft", js_str_trimStart, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "trimEnd", js_str_trimEnd, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "trimRight", js_str_trimEnd, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "padStart", js_str_padStart, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "padEnd", js_str_padEnd, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "repeat", js_str_repeat, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "isWellFormed", js_str_isWellFormed, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "toWellFormed", js_str_toWellFormed, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "localeCompare", js_str_localeCompare, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "normalize", js_str_normalize, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "replace", js_str_replace, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "replaceAll", js_str_replaceAll, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "split", js_str_split, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "match", js_str_match, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "matchAll", js_str_matchAll, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->string_proto, "search", js_str_search, 1, AJS_METHOD_FLAGS, 0);

    {
        static const char *const k_hnames[13] = {
            "anchor","big","blink","bold","fixed","fontcolor","fontsize",
            "italics","link","small","strike","sub","sup"
        };
        for (int i = 0; i < 13; i++) {
            int ar = (k_html_wrappers[i].attr != NULL) ? 1 : 0;
            ajs_def_native_method(vm, r, r->string_proto, k_hnames[i], js_str_html, ar, AJS_METHOD_FLAGS, i);
        }
    }

    r->string_iter_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->iterator_proto));
    sa_def_tag(vm, r->string_iter_proto, "String Iterator");
    ajs_def_native_method(vm, r, r->string_iter_proto, "next", js_str_iter_next, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_sym_method(vm, r, r->string_proto, vm->well_known_syms[AJS_SYM_ITERATOR],
                              "[Symbol.iterator]", js_str_iter_init, 0, AJS_METHOD_FLAGS);

    /* 2. Array & %ArrayIteratorPrototype% */
    AjsObject *aproto = ajs_obj_new(vm, r, AJS_OBJ_ARRAY, r->object_proto);
    aproto->u.arr.length = 0;
    aproto->u.arr.length_writable = true;
    r->array_proto = ajs_val_from_gc(aproto);

    r->array_ctor = sa_make_fn(vm, r, js_array_ctor, "Array", 1, true, -21, r->function_proto);
    sa_link_ctor_proto(vm, r->array_ctor, r->array_proto);
    sa_def_prop(vm, r->global_obj, "Array", r->array_ctor, gflags);

    ajs_def_native_method(vm, r, r->array_ctor, "isArray", js_array_isArray, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_ctor, "of", js_array_of, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_ctor, "from", js_array_from, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_ctor, "fromAsync", js_array_fromAsync, 1, AJS_METHOD_FLAGS, 0);
    {
        AjsValue sp_get = sa_make_fn(vm, r, js_species_this, "get [Symbol.species]", 0, false, 0, r->function_proto);
        AjsPropDesc sd = {
            .flags = AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_GET | AJS_DESC_HAS_SET |
                     AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
            .getter = sp_get,
            .setter = AJS_VAL_UNDEFINED
        };
        ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(r->array_ctor),
                                ajs_val_from_gc(vm->well_known_syms[AJS_SYM_SPECIES]), &sd, false);
    }

    ajs_def_native_method(vm, r, r->array_proto, "push", js_arr_push, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "pop", js_arr_pop, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "shift", js_arr_shift, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "unshift", js_arr_unshift, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "slice", js_arr_slice, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "splice", js_arr_splice, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "toSpliced", js_arr_toSpliced, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "concat", js_arr_concat, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "join", js_arr_join, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "toString", js_arr_toString, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "toLocaleString", js_arr_toLocaleString, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "at", js_arr_at, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "with", js_arr_with, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "reverse", js_arr_reverse, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "toReversed", js_arr_toReversed, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "fill", js_arr_fill, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "copyWithin", js_arr_copyWithin, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "indexOf", js_arr_indexOf, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "lastIndexOf", js_arr_lastIndexOf, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "includes", js_arr_includes, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "forEach", js_arr_iter_hof, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "map", js_arr_iter_hof, 1, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->array_proto, "filter", js_arr_iter_hof, 1, AJS_METHOD_FLAGS, 2);
    ajs_def_native_method(vm, r, r->array_proto, "some", js_arr_iter_hof, 1, AJS_METHOD_FLAGS, 3);
    ajs_def_native_method(vm, r, r->array_proto, "every", js_arr_iter_hof, 1, AJS_METHOD_FLAGS, 4);
    ajs_def_native_method(vm, r, r->array_proto, "find", js_arr_iter_hof, 1, AJS_METHOD_FLAGS, 5);
    ajs_def_native_method(vm, r, r->array_proto, "findIndex", js_arr_iter_hof, 1, AJS_METHOD_FLAGS, 6);
    ajs_def_native_method(vm, r, r->array_proto, "findLast", js_arr_iter_hof, 1, AJS_METHOD_FLAGS, 7);
    ajs_def_native_method(vm, r, r->array_proto, "findLastIndex", js_arr_iter_hof, 1, AJS_METHOD_FLAGS, 8);
    ajs_def_native_method(vm, r, r->array_proto, "reduce", js_arr_reduce, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "reduceRight", js_arr_reduceRight, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "flat", js_arr_flat, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "flatMap", js_arr_flatMap, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "sort", js_arr_sort, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->array_proto, "toSorted", js_arr_toSorted, 1, AJS_METHOD_FLAGS, 0);

    r->array_iter_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->iterator_proto));
    sa_def_tag(vm, r->array_iter_proto, "Array Iterator");
    ajs_def_native_method(vm, r, r->array_iter_proto, "next", js_arr_iter_next, 0, AJS_METHOD_FLAGS, 0);

    ajs_def_native_method(vm, r, r->array_proto, "keys", js_arr_iter_create, 0, AJS_METHOD_FLAGS, AJS_ITER_ARRAY_KEYS);
    r->array_proto_values_fn = sa_make_fn(vm, r, js_arr_iter_create, "values", 0, false, AJS_ITER_ARRAY_VALUES, r->function_proto);
    sa_def_prop(vm, r->array_proto, "values", r->array_proto_values_fn, gflags);
    ajs_def_native_method(vm, r, r->array_proto, "entries", js_arr_iter_create, 0, AJS_METHOD_FLAGS, AJS_ITER_ARRAY_ENTRIES);
    {
        AjsPropDesc id = {
            .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                     AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
            .value = r->array_proto_values_fn
        };
        ajs_obj_define_own_prop(vm, aproto, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), &id, false);
    }

    /* Array.prototype[Symbol.unscopables] */
    {
        AjsValue unscop = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, AJS_VAL_NULL));
        static const char *const k_unscop[] = {
            "at","copyWithin","entries","fill","find","findIndex","findLast","findLastIndex",
            "flat","flatMap","includes","keys","toReversed","toSorted","toSpliced","values"
        };
        uint32_t uf = AJS_DESC_WRITABLE | AJS_DESC_ENUMERABLE | AJS_DESC_CONFIGURABLE;
        for (size_t i = 0; i < sizeof(k_unscop) / sizeof(k_unscop[0]); i++) {
            sa_def_prop(vm, unscop, k_unscop[i], AJS_VAL_TRUE, uf);
        }
        AjsPropDesc ud = {
            .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
                     AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
            .value = unscop
        };
        ajs_obj_define_own_prop(vm, aproto, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_UNSCOPABLES]), &ud, false);
    }

    /* 3. ES2025/2026 Iterator & Iterator Helpers */
    r->iterator_ctor = sa_make_fn(vm, r, js_iterator_ctor, "Iterator", 0, true, -22, r->function_proto);
    sa_link_ctor_proto(vm, r->iterator_ctor, r->iterator_proto);
    sa_def_prop(vm, r->global_obj, "Iterator", r->iterator_ctor, gflags);
    ajs_def_native_method(vm, r, r->iterator_ctor, "from", js_iterator_from, 1, AJS_METHOD_FLAGS, 0);

    r->iter_helper_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->iterator_proto));
    sa_def_tag(vm, r->iter_helper_proto, "Iterator Helper");
    ajs_def_native_method(vm, r, r->iter_helper_proto, "next", js_iter_helper_next, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->iter_helper_proto, "return", js_iter_helper_return, 0, AJS_METHOD_FLAGS, 0);

    ajs_def_native_method(vm, r, r->iterator_proto, "map", js_iter_create_helper, 1, AJS_METHOD_FLAGS, AJS_ITER_HELPER_MAP);
    ajs_def_native_method(vm, r, r->iterator_proto, "filter", js_iter_create_helper, 1, AJS_METHOD_FLAGS, AJS_ITER_HELPER_FILTER);
    ajs_def_native_method(vm, r, r->iterator_proto, "take", js_iter_create_helper, 1, AJS_METHOD_FLAGS, AJS_ITER_HELPER_TAKE);
    ajs_def_native_method(vm, r, r->iterator_proto, "drop", js_iter_create_helper, 1, AJS_METHOD_FLAGS, AJS_ITER_HELPER_DROP);
    ajs_def_native_method(vm, r, r->iterator_proto, "flatMap", js_iter_create_helper, 1, AJS_METHOD_FLAGS, AJS_ITER_HELPER_FLATMAP);
    ajs_def_native_method(vm, r, r->iterator_proto, "toArray", js_iter_consume, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->iterator_proto, "forEach", js_iter_consume, 1, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->iterator_proto, "some", js_iter_consume, 1, AJS_METHOD_FLAGS, 2);
    ajs_def_native_method(vm, r, r->iterator_proto, "every", js_iter_consume, 1, AJS_METHOD_FLAGS, 3);
    ajs_def_native_method(vm, r, r->iterator_proto, "find", js_iter_consume, 1, AJS_METHOD_FLAGS, 4);
    ajs_def_native_method(vm, r, r->iterator_proto, "reduce", js_iter_consume, 1, AJS_METHOD_FLAGS, 5);
    sa_def_tag(vm, r->iterator_proto, "Iterator");
}
