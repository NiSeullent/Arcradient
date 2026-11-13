/*
 * ArcradientJS 1.0 - js/vm/ajs_object.c
 * Ordinary & Exotic Objects, Descriptors, OwnPropertyKeys Order, Proxies, Private Elements & Conversions
 */
#include "ajs_internal.h"

AjsObject *ajs_obj_new(AjsVm *vm, AjsRealm *realm, AjsObjKind kind, AjsValue proto) {
    if (!realm) realm = vm->active_realm ? vm->active_realm : vm->main_realm;
    AjsObject *obj = (AjsObject *)ajs_gc_alloc(vm, sizeof(AjsObject), AJS_GC_OBJECT);
    if (!obj) return NULL;
    obj->kind = kind;
    obj->extensible = true;
    obj->realm = realm;
    obj->proto = proto;
    return obj;
}

static uint32_t ajs_prop_key_hash(AjsValue key) {
    AjsGcHeader *hdr = ajs_val_to_gc(key);
    if (hdr->gc_kind == AJS_GC_STRING) return ((AjsString *)hdr)->hash;
    if (hdr->gc_kind == AJS_GC_SYMBOL) {
        uint32_t id = ((AjsSymbol *)hdr)->id;
        return id * 2654435761u + 1u;
    }
    return 1u;
}

static bool ajs_prop_key_eq(AjsValue a, AjsValue b) {
    if (a == b) return true;
    if (!ajs_val_is_ptr(a) || !ajs_val_is_ptr(b)) return false;
    AjsGcHeader *ha = ajs_val_to_gc(a);
    AjsGcHeader *hb = ajs_val_to_gc(b);
    if (ha->gc_kind != hb->gc_kind) return false;
    if (ha->gc_kind == AJS_GC_STRING) return ajs_str_eq((AjsString *)ha, (AjsString *)hb);
    return ha == hb;
}

static void ajs_obj_rebuild_hash(AjsObject *obj, uint32_t new_cap) {
    int32_t *ntab = (int32_t *)malloc(new_cap * sizeof(int32_t));
    if (!ntab) return;
    for (uint32_t i = 0; i < new_cap; i++) ntab[i] = -1;
    uint32_t mask = new_cap - 1u;
    for (uint32_t i = 0; i < obj->prop_count; i++) {
        if (obj->props[i].key != AJS_VAL_EMPTY) {
            uint32_t pos = ajs_prop_key_hash(obj->props[i].key) & mask;
            while (ntab[pos] >= 0) pos = (pos + 1u) & mask;
            ntab[pos] = (int32_t)i;
        }
    }
    free(obj->hash_tab);
    obj->hash_tab = ntab;
    obj->hash_cap = new_cap;
}

static int32_t ajs_obj_find_own_slot(AjsObject *obj, AjsValue key) {
    if (obj->hash_tab && obj->hash_cap > 0) {
        uint32_t mask = obj->hash_cap - 1u;
        uint32_t pos = ajs_prop_key_hash(key) & mask;
        for (uint32_t step = 0; step < obj->hash_cap; step++) {
            int32_t slot = obj->hash_tab[pos];
            if (slot == -1) return -1;
            if (slot >= 0 && ajs_prop_key_eq(obj->props[slot].key, key)) return slot;
            pos = (pos + 1u) & mask;
        }
        return -1;
    }
    for (uint32_t i = 0; i < obj->prop_count; i++) {
        if (obj->props[i].key != AJS_VAL_EMPTY && ajs_prop_key_eq(obj->props[i].key, key)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static AjsPropSlot *ajs_obj_insert_own_slot(AjsObject *obj, AjsValue key, uint32_t flags,
                                            AjsValue val, AjsValue setter) {
    if (obj->prop_count >= obj->prop_cap) {
        uint32_t ncap = obj->prop_cap ? obj->prop_cap * 2u : 8u;
        AjsPropSlot *np = (AjsPropSlot *)realloc(obj->props, ncap * sizeof(AjsPropSlot));
        if (!np) return NULL;
        obj->props = np;
        obj->prop_cap = ncap;
    }
    uint32_t idx = obj->prop_count++;
    obj->live_props++;
    AjsPropSlot *slot = &obj->props[idx];
    slot->key = key;
    slot->flags = flags;
    slot->seq = obj->next_seq++;
    slot->value = val;
    slot->setter = setter;

    if (obj->hash_tab) {
        if ((obj->prop_count + 1u) * 10u >= obj->hash_cap * 7u) {
            ajs_obj_rebuild_hash(obj, obj->hash_cap * 2u);
        } else {
            uint32_t mask = obj->hash_cap - 1u;
            uint32_t pos = ajs_prop_key_hash(key) & mask;
            while (obj->hash_tab[pos] >= 0) pos = (pos + 1u) & mask;
            obj->hash_tab[pos] = (int32_t)idx;
        }
    } else if (obj->live_props >= 12u) {
        ajs_obj_rebuild_hash(obj, 32u);
    }
    return slot;
}

static void ajs_obj_remove_own_slot(AjsObject *obj, int32_t slot_idx) {
    AjsValue key = obj->props[slot_idx].key;
    obj->props[slot_idx].key = AJS_VAL_EMPTY;
    obj->props[slot_idx].value = AJS_VAL_UNDEFINED;
    obj->props[slot_idx].setter = AJS_VAL_UNDEFINED;
    if (obj->live_props > 0) obj->live_props--;
    if (obj->hash_tab && obj->hash_cap > 0) {
        uint32_t mask = obj->hash_cap - 1u;
        uint32_t pos = ajs_prop_key_hash(key) & mask;
        for (uint32_t step = 0; step < obj->hash_cap; step++) {
            int32_t s = obj->hash_tab[pos];
            if (s == -1) break;
            if (s == slot_idx) {
                obj->hash_tab[pos] = -2;
                break;
            }
            pos = (pos + 1u) & mask;
        }
    }
}

/* ============================================================================
 * Conversions & Equality (ECMA-262 §7.1 / §7.2)
 * ============================================================================ */

bool ajs_is_constructor_val(AjsValue v) {
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return false;
    AjsObject *obj = (AjsObject *)ajs_val_to_gc(v);
    if (obj->kind == AJS_OBJ_FUNCTION) return obj->u.fn.is_constructor;
    if (obj->kind == AJS_OBJ_PROXY) return obj->u.proxy.is_constructor && obj->u.proxy.handler != AJS_VAL_NULL;
    return false;
}

AjsValue ajs_to_primitive(AjsVm *vm, AjsValue v, int hint) {
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return v;
    size_t rsave = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, v);

    AjsSymbol *to_prim_sym = vm->well_known_syms[AJS_SYM_TO_PRIMITIVE];
    if (to_prim_sym) {
        AjsValue exotic = ajs_obj_get(vm, v, ajs_val_from_gc(to_prim_sym), v);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rsave); return AJS_VAL_EXCEPTION; }
        if (exotic != AJS_VAL_UNDEFINED && exotic != AJS_VAL_NULL) {
            if (!ajs_is_callable(vm, exotic)) {
                ajs_temp_root_restore(vm, rsave);
                return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "@@toPrimitive is not callable");
            }
            ajs_temp_root_push(vm, exotic);
            const char *hstr = (hint == 1) ? "string" : ((hint == 2) ? "number" : "default");
            AjsString *hs = ajs_str_from_cstr(vm, hstr, true);
            AjsValue arg = ajs_val_from_gc(hs);
            AjsValue res = ajs_call_internal(vm, exotic, v, 1, &arg);
            ajs_temp_root_restore(vm, rsave);
            if (vm->has_exception) return AJS_VAL_EXCEPTION;
            if (ajs_val_is_gc_kind(res, AJS_GC_OBJECT)) {
                return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "@@toPrimitive must return a primitive value");
            }
            return res;
        }
    }

    if (hint == 0) hint = 2; /* OrdinaryToPrimitive: default behaves like number */
    AjsString *m0 = (hint == 1) ? vm->str_toString : vm->str_valueOf;
    AjsString *m1 = (hint == 1) ? vm->str_valueOf : vm->str_toString;
    AjsString *methods[2] = { m0, m1 };
    for (int i = 0; i < 2; i++) {
        if (!methods[i]) continue;
        AjsValue fn = ajs_obj_get(vm, v, ajs_val_from_gc(methods[i]), v);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rsave); return AJS_VAL_EXCEPTION; }
        if (ajs_is_callable(vm, fn)) {
            ajs_temp_root_push(vm, fn);
            AjsValue res = ajs_call_internal(vm, fn, v, 0, NULL);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rsave); return AJS_VAL_EXCEPTION; }
            if (!ajs_val_is_gc_kind(res, AJS_GC_OBJECT)) {
                ajs_temp_root_restore(vm, rsave);
                return res;
            }
        }
    }
    ajs_temp_root_restore(vm, rsave);
    return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot convert object to primitive value");
}

AjsString *ajs_to_js_string(AjsVm *vm, AjsValue v) {
    if (ajs_val_is_gc_kind(v, AJS_GC_STRING)) return (AjsString *)ajs_val_to_gc(v);
    if (v == AJS_VAL_UNDEFINED) return ajs_str_from_cstr(vm, "undefined", true);
    if (v == AJS_VAL_NULL) return ajs_str_from_cstr(vm, "null", true);
    if (v == AJS_VAL_TRUE) return ajs_str_from_cstr(vm, "true", true);
    if (v == AJS_VAL_FALSE) return ajs_str_from_cstr(vm, "false", true);
    if (ajs_val_is_number(v)) return ajs_number_to_str(vm, ajs_val_to_number_unchecked(v), 10);
    if (ajs_val_is_gc_kind(v, AJS_GC_BIGINT)) return ajs_bigint_to_str(vm, (AjsBigInt *)ajs_val_to_gc(v), 10);
    if (ajs_val_is_gc_kind(v, AJS_GC_SYMBOL)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot convert a Symbol value to a string");
        return NULL;
    }
    if (ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) {
        AjsValue prim = ajs_to_primitive(vm, v, 1);
        if (vm->has_exception) return NULL;
        return ajs_to_js_string(vm, prim);
    }
    return vm->str_empty;
}

AjsValue ajs_to_property_key(AjsVm *vm, AjsValue key_val) {
    if (ajs_val_is_gc_kind(key_val, AJS_GC_SYMBOL)) return key_val;
    if (ajs_val_is_gc_kind(key_val, AJS_GC_STRING)) {
        AjsString *s = ajs_str_intern(vm, (AjsString *)ajs_val_to_gc(key_val));
        return ajs_val_from_gc(s);
    }
    AjsValue prim = ajs_to_primitive(vm, key_val, 1);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (ajs_val_is_gc_kind(prim, AJS_GC_SYMBOL)) return prim;
    AjsString *s = ajs_to_js_string(vm, prim);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    return ajs_val_from_gc(ajs_str_intern(vm, s));
}

bool ajs_to_numeric(AjsVm *vm, AjsValue v, AjsValue *out_num_or_bi) {
    AjsValue prim = ajs_to_primitive(vm, v, 2);
    if (vm->has_exception) return false;
    if (ajs_val_is_gc_kind(prim, AJS_GC_BIGINT)) {
        *out_num_or_bi = prim;
        return true;
    }
    double d = 0.0;
    if (!ajs_to_number(vm, prim, &d)) return false;
    *out_num_or_bi = ajs_val_number(d);
    return true;
}

bool ajs_to_length(AjsVm *vm, AjsValue v, uint64_t *out_len) {
    double d = 0.0;
    if (!ajs_to_number(vm, v, &d)) return false;
    d = ajs_double_to_integer_or_inf(d);
    if (d <= 0.0) *out_len = 0;
    else if (d >= 9007199254740991.0) *out_len = UINT64_C(9007199254740991);
    else *out_len = (uint64_t)d;
    return true;
}

bool ajs_to_index(AjsVm *vm, AjsValue v, size_t *out_idx) {
    if (v == AJS_VAL_UNDEFINED) {
        *out_idx = 0;
        return true;
    }
    double d = 0.0;
    if (!ajs_to_number(vm, v, &d)) return false;
    double int_d = ajs_double_to_integer_or_inf(d);
    if (int_d < 0.0 || int_d > 9007199254740991.0 || int_d > (double)SIZE_MAX) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid index");
        return false;
    }
    *out_idx = (size_t)int_d;
    return true;
}

AjsObject *ajs_to_object(AjsVm *vm, AjsValue v) {
    if (ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return (AjsObject *)ajs_val_to_gc(v);
    if (v == AJS_VAL_UNDEFINED || v == AJS_VAL_NULL) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot convert undefined or null to object");
        return NULL;
    }
    AjsRealm *r = vm->active_realm ? vm->active_realm : vm->main_realm;
    if (v == AJS_VAL_TRUE || v == AJS_VAL_FALSE) {
        AjsObject *o = ajs_obj_new(vm, r, AJS_OBJ_BOOLEAN_OBJ, r->boolean_proto);
        if (o) o->u.prim_val = v;
        return o;
    }
    if (ajs_val_is_number(v)) {
        AjsObject *o = ajs_obj_new(vm, r, AJS_OBJ_NUMBER_OBJ, r->number_proto);
        if (o) o->u.prim_val = v;
        return o;
    }
    if (ajs_val_is_gc_kind(v, AJS_GC_STRING)) {
        AjsObject *o = ajs_obj_new(vm, r, AJS_OBJ_STRING_OBJ, r->string_proto);
        if (o) o->u.prim_val = v;
        return o;
    }
    if (ajs_val_is_gc_kind(v, AJS_GC_SYMBOL)) {
        AjsObject *o = ajs_obj_new(vm, r, AJS_OBJ_SYMBOL_OBJ, r->symbol_proto);
        if (o) o->u.prim_val = v;
        return o;
    }
    if (ajs_val_is_gc_kind(v, AJS_GC_BIGINT)) {
        AjsObject *o = ajs_obj_new(vm, r, AJS_OBJ_BIGINT_OBJ, r->bigint_proto);
        if (o) o->u.prim_val = v;
        return o;
    }
    return NULL;
}

bool ajs_same_value_zero(AjsValue a, AjsValue b) {
    if (ajs_val_is_number(a) && ajs_val_is_number(b)) {
        double da = ajs_val_to_number_unchecked(a);
        double db = ajs_val_to_number_unchecked(b);
        if (isnan(da) && isnan(db)) return true;
        return da == db;
    }
    if (a == b) return true;
    if (ajs_val_is_gc_kind(a, AJS_GC_STRING) && ajs_val_is_gc_kind(b, AJS_GC_STRING)) {
        return ajs_str_eq((AjsString *)ajs_val_to_gc(a), (AjsString *)ajs_val_to_gc(b));
    }
    if (ajs_val_is_gc_kind(a, AJS_GC_BIGINT) && ajs_val_is_gc_kind(b, AJS_GC_BIGINT)) {
        return ajs_bigint_eq((AjsBigInt *)ajs_val_to_gc(a), (AjsBigInt *)ajs_val_to_gc(b));
    }
    return false;
}

bool ajs_strict_eq(AjsValue a, AjsValue b) {
    if (ajs_val_is_number(a) && ajs_val_is_number(b)) {
        double da = ajs_val_to_number_unchecked(a);
        double db = ajs_val_to_number_unchecked(b);
        return da == db; /* Handles +0 === -0 true, NaN === NaN false! */
    }
    if (a == b) return true;
    if (ajs_val_is_gc_kind(a, AJS_GC_STRING) && ajs_val_is_gc_kind(b, AJS_GC_STRING)) {
        return ajs_str_eq((AjsString *)ajs_val_to_gc(a), (AjsString *)ajs_val_to_gc(b));
    }
    if (ajs_val_is_gc_kind(a, AJS_GC_BIGINT) && ajs_val_is_gc_kind(b, AJS_GC_BIGINT)) {
        return ajs_bigint_eq((AjsBigInt *)ajs_val_to_gc(a), (AjsBigInt *)ajs_val_to_gc(b));
    }
    return false;
}

bool ajs_abstract_eq(AjsVm *vm, AjsValue a, AjsValue b, bool *out_res) {
    for (;;) {
        if ((a == AJS_VAL_UNDEFINED || a == AJS_VAL_NULL) &&
            (b == AJS_VAL_UNDEFINED || b == AJS_VAL_NULL)) {
            *out_res = true;
            return true;
        }
        /* Same type */
        bool a_num = ajs_val_is_number(a), b_num = ajs_val_is_number(b);
        if (a_num && b_num) { *out_res = ajs_strict_eq(a, b); return true; }
        bool a_str = ajs_val_is_gc_kind(a, AJS_GC_STRING), b_str = ajs_val_is_gc_kind(b, AJS_GC_STRING);
        if (a_str && b_str) { *out_res = ajs_strict_eq(a, b); return true; }
        bool a_bool = (a == AJS_VAL_TRUE || a == AJS_VAL_FALSE);
        bool b_bool = (b == AJS_VAL_TRUE || b == AJS_VAL_FALSE);
        if (a_bool && b_bool) { *out_res = (a == b); return true; }
        bool a_sym = ajs_val_is_gc_kind(a, AJS_GC_SYMBOL), b_sym = ajs_val_is_gc_kind(b, AJS_GC_SYMBOL);
        if (a_sym && b_sym) { *out_res = (a == b); return true; }
        bool a_bi = ajs_val_is_gc_kind(a, AJS_GC_BIGINT), b_bi = ajs_val_is_gc_kind(b, AJS_GC_BIGINT);
        if (a_bi && b_bi) { *out_res = ajs_strict_eq(a, b); return true; }
        bool a_obj = ajs_val_is_gc_kind(a, AJS_GC_OBJECT), b_obj = ajs_val_is_gc_kind(b, AJS_GC_OBJECT);
        if (a_obj && b_obj) { *out_res = (a == b); return true; }

        if (a == AJS_VAL_UNDEFINED || a == AJS_VAL_NULL || b == AJS_VAL_UNDEFINED || b == AJS_VAL_NULL) {
            *out_res = false;
            return true;
        }
        if (a_num && b_str) {
            b = ajs_val_number(ajs_str_to_number((AjsString *)ajs_val_to_gc(b)));
            continue;
        }
        if (a_str && b_num) {
            a = ajs_val_number(ajs_str_to_number((AjsString *)ajs_val_to_gc(a)));
            continue;
        }
        if (a_bi && b_str) {
            AjsString *sb = (AjsString *)ajs_val_to_gc(b);
            bool ok = false;
            AjsBigInt *bi = ajs_bigint_from_cstr(vm, sb->utf8, sb->len8, true, &ok);
            if (!ok || !bi) { *out_res = false; return true; }
            *out_res = ajs_bigint_eq((AjsBigInt *)ajs_val_to_gc(a), bi);
            return true;
        }
        if (a_str && b_bi) {
            AjsValue tmp = a; a = b; b = tmp;
            continue;
        }
        if (a_bool) { a = ajs_val_number(a == AJS_VAL_TRUE ? 1.0 : 0.0); continue; }
        if (b_bool) { b = ajs_val_number(b == AJS_VAL_TRUE ? 1.0 : 0.0); continue; }
        if ((a_str || a_num || a_bi || a_sym) && b_obj) {
            size_t rs = ajs_temp_root_save(vm);
            ajs_temp_root_push(vm, a);
            b = ajs_to_primitive(vm, b, 0);
            ajs_temp_root_restore(vm, rs);
            if (vm->has_exception) return false;
            continue;
        }
        if (a_obj && (b_str || b_num || b_bi || b_sym)) {
            size_t rs = ajs_temp_root_save(vm);
            ajs_temp_root_push(vm, b);
            a = ajs_to_primitive(vm, a, 0);
            ajs_temp_root_restore(vm, rs);
            if (vm->has_exception) return false;
            continue;
        }
        if ((a_bi && b_num) || (a_num && b_bi)) {
            AjsBigInt *bi = (AjsBigInt *)ajs_val_to_gc(a_bi ? a : b);
            double d = ajs_val_to_number_unchecked(a_num ? a : b);
            if (isnan(d) || isinf(d)) { *out_res = false; return true; }
            *out_res = (ajs_bigint_cmp_double(bi, d) == 0);
            return true;
        }
        *out_res = false;
        return true;
    }
}

int ajs_abstract_rel_cmp(AjsVm *vm, AjsValue a, AjsValue b, bool left_first) {
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, a);
    ajs_temp_root_push(vm, b);
    AjsValue px, py;
    if (left_first) {
        px = ajs_to_primitive(vm, a, 2);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return -1; }
        ajs_temp_root_push(vm, px);
        py = ajs_to_primitive(vm, b, 2);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return -1; }
    } else {
        py = ajs_to_primitive(vm, b, 2);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return -1; }
        ajs_temp_root_push(vm, py);
        px = ajs_to_primitive(vm, a, 2);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return -1; }
    }
    ajs_temp_root_restore(vm, rs);

    if (ajs_val_is_gc_kind(px, AJS_GC_STRING) && ajs_val_is_gc_kind(py, AJS_GC_STRING)) {
        return ajs_str_cmp_utf16((AjsString *)ajs_val_to_gc(px), (AjsString *)ajs_val_to_gc(py)) < 0 ? 1 : 0;
    }
    if (ajs_val_is_gc_kind(px, AJS_GC_BIGINT) && ajs_val_is_gc_kind(py, AJS_GC_STRING)) {
        AjsString *sy = (AjsString *)ajs_val_to_gc(py);
        bool ok = false;
        AjsBigInt *by = ajs_bigint_from_cstr(vm, sy->utf8, sy->len8, true, &ok);
        if (!ok || !by) return -1;
        return ajs_bigint_cmp((AjsBigInt *)ajs_val_to_gc(px), by) < 0 ? 1 : 0;
    }
    if (ajs_val_is_gc_kind(px, AJS_GC_STRING) && ajs_val_is_gc_kind(py, AJS_GC_BIGINT)) {
        AjsString *sx = (AjsString *)ajs_val_to_gc(px);
        bool ok = false;
        AjsBigInt *bx = ajs_bigint_from_cstr(vm, sx->utf8, sx->len8, true, &ok);
        if (!ok || !bx) return -1;
        return ajs_bigint_cmp(bx, (AjsBigInt *)ajs_val_to_gc(py)) < 0 ? 1 : 0;
    }

    AjsValue nx, ny;
    if (!ajs_to_numeric(vm, px, &nx) || !ajs_to_numeric(vm, py, &ny)) return -1;
    bool x_bi = ajs_val_is_gc_kind(nx, AJS_GC_BIGINT);
    bool y_bi = ajs_val_is_gc_kind(ny, AJS_GC_BIGINT);
    if (x_bi && y_bi) {
        return ajs_bigint_cmp((AjsBigInt *)ajs_val_to_gc(nx), (AjsBigInt *)ajs_val_to_gc(ny)) < 0 ? 1 : 0;
    }
    if (!x_bi && !y_bi) {
        double dx = ajs_val_to_number_unchecked(nx);
        double dy = ajs_val_to_number_unchecked(ny);
        if (isnan(dx) || isnan(dy)) return -1;
        return dx < dy ? 1 : 0;
    }
    if (x_bi) {
        double dy = ajs_val_to_number_unchecked(ny);
        if (isnan(dy)) return -1;
        int c = ajs_bigint_cmp_double((AjsBigInt *)ajs_val_to_gc(nx), dy);
        return c < 0 ? 1 : 0;
    } else {
        double dx = ajs_val_to_number_unchecked(nx);
        if (isnan(dx)) return -1;
        int c = ajs_bigint_cmp_double((AjsBigInt *)ajs_val_to_gc(ny), dx);
        return c > 0 ? 1 : 0;
    }
}

/* ============================================================================
 * Proxy Trap Helpers & Descriptor Conversion
 * ============================================================================ */

static bool ajs_proxy_check(AjsVm *vm, AjsObject *p, const char *trap_name) {
    if (p->u.proxy.handler == AJS_VAL_NULL) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot perform '%s' on a proxy that has been revoked", trap_name);
        return false;
    }
    return true;
}

static AjsValue ajs_proxy_get_trap(AjsVm *vm, AjsObject *p, const char *trap_name) {
    if (!ajs_proxy_check(vm, p, trap_name)) return AJS_VAL_EXCEPTION;
    AjsString *s = ajs_str_from_cstr(vm, trap_name, true);
    AjsValue trap = ajs_obj_get(vm, p->u.proxy.handler, ajs_val_from_gc(s), p->u.proxy.handler);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (trap == AJS_VAL_UNDEFINED || trap == AJS_VAL_NULL) return AJS_VAL_UNDEFINED;
    if (!ajs_is_callable(vm, trap)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy trap '%s' is not callable", trap_name);
    }
    return trap;
}

static AjsValue ajs_from_prop_desc(AjsVm *vm, const AjsPropDesc *d) {
    AjsRealm *r = vm->active_realm ? vm->active_realm : vm->main_realm;
    AjsObject *o = ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto);
    if (!o) return AJS_VAL_EXCEPTION;
    AjsValue ov = ajs_val_from_gc(o);
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ov);
    if (d->flags & AJS_DESC_IS_ACCESSOR) {
        ajs_define_prop_str(vm, ov, "get", d->getter, AJS_PROP_DEFAULT);
        ajs_define_prop_str(vm, ov, "set", d->setter, AJS_PROP_DEFAULT);
    } else {
        ajs_define_prop_str(vm, ov, "value", d->value, AJS_PROP_DEFAULT);
        ajs_define_prop_str(vm, ov, "writable",
                            (d->flags & AJS_DESC_WRITABLE) ? AJS_VAL_TRUE : AJS_VAL_FALSE, AJS_PROP_DEFAULT);
    }
    ajs_define_prop_str(vm, ov, "enumerable",
                        (d->flags & AJS_DESC_ENUMERABLE) ? AJS_VAL_TRUE : AJS_VAL_FALSE, AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, ov, "configurable",
                        (d->flags & AJS_DESC_CONFIGURABLE) ? AJS_VAL_TRUE : AJS_VAL_FALSE, AJS_PROP_DEFAULT);
    ajs_temp_root_restore(vm, rs);
    return ov;
}

static bool ajs_to_prop_desc_internal(AjsVm *vm, AjsValue desc_obj, AjsPropDesc *out) {
    if (!ajs_val_is_gc_kind(desc_obj, AJS_GC_OBJECT)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Property description must be an object");
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->value = AJS_VAL_UNDEFINED;
    out->getter = AJS_VAL_UNDEFINED;
    out->setter = AJS_VAL_UNDEFINED;

    AjsObject *o = (AjsObject *)ajs_val_to_gc(desc_obj);
    bool has = false;

    AjsString *k_enum = ajs_str_from_cstr(vm, "enumerable", true);
    if (!ajs_obj_has_prop(vm, o, ajs_val_from_gc(k_enum), &has)) return false;
    if (has) {
        AjsValue v = ajs_obj_get(vm, desc_obj, ajs_val_from_gc(k_enum), desc_obj);
        if (vm->has_exception) return false;
        out->flags |= AJS_DESC_HAS_ENUMERABLE | (ajs_to_bool(vm, v) ? AJS_DESC_ENUMERABLE : 0u);
    }

    AjsString *k_conf = ajs_str_from_cstr(vm, "configurable", true);
    if (!ajs_obj_has_prop(vm, o, ajs_val_from_gc(k_conf), &has)) return false;
    if (has) {
        AjsValue v = ajs_obj_get(vm, desc_obj, ajs_val_from_gc(k_conf), desc_obj);
        if (vm->has_exception) return false;
        out->flags |= AJS_DESC_HAS_CONFIGURABLE | (ajs_to_bool(vm, v) ? AJS_DESC_CONFIGURABLE : 0u);
    }

    AjsString *k_val = vm->str_value ? vm->str_value : ajs_str_from_cstr(vm, "value", true);
    if (!ajs_obj_has_prop(vm, o, ajs_val_from_gc(k_val), &has)) return false;
    if (has) {
        out->value = ajs_obj_get(vm, desc_obj, ajs_val_from_gc(k_val), desc_obj);
        if (vm->has_exception) return false;
        out->flags |= AJS_DESC_HAS_VALUE;
    }

    AjsString *k_writ = ajs_str_from_cstr(vm, "writable", true);
    if (!ajs_obj_has_prop(vm, o, ajs_val_from_gc(k_writ), &has)) return false;
    if (has) {
        AjsValue v = ajs_obj_get(vm, desc_obj, ajs_val_from_gc(k_writ), desc_obj);
        if (vm->has_exception) return false;
        out->flags |= AJS_DESC_HAS_WRITABLE | (ajs_to_bool(vm, v) ? AJS_DESC_WRITABLE : 0u);
    }

    AjsString *k_get = ajs_str_from_cstr(vm, "get", true);
    if (!ajs_obj_has_prop(vm, o, ajs_val_from_gc(k_get), &has)) return false;
    if (has) {
        AjsValue g = ajs_obj_get(vm, desc_obj, ajs_val_from_gc(k_get), desc_obj);
        if (vm->has_exception) return false;
        if (g != AJS_VAL_UNDEFINED && !ajs_is_callable(vm, g)) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Getter must be a function");
            return false;
        }
        out->getter = g;
        out->flags |= AJS_DESC_HAS_GET | AJS_DESC_IS_ACCESSOR;
    }

    AjsString *k_set = ajs_str_from_cstr(vm, "set", true);
    if (!ajs_obj_has_prop(vm, o, ajs_val_from_gc(k_set), &has)) return false;
    if (has) {
        AjsValue s = ajs_obj_get(vm, desc_obj, ajs_val_from_gc(k_set), desc_obj);
        if (vm->has_exception) return false;
        if (s != AJS_VAL_UNDEFINED && !ajs_is_callable(vm, s)) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Setter must be a function");
            return false;
        }
        out->setter = s;
        out->flags |= AJS_DESC_HAS_SET | AJS_DESC_IS_ACCESSOR;
    }

    if ((out->flags & AJS_DESC_IS_ACCESSOR) &&
        (out->flags & (AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE))) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Invalid property descriptor: cannot mix accessor and data");
        return false;
    }
    return true;
}

static bool ajs_ta_canonical_index(const AjsString *s, bool *out_is_canon_num, uint32_t *out_valid_idx, size_t ta_len) {
    *out_is_canon_num = false;
    if (s->is_array_idx) {
        *out_is_canon_num = true;
        if ((size_t)s->array_idx < ta_len) {
            *out_valid_idx = s->array_idx;
            return true;
        }
        return false;
    }
    if (ajs_str_eq_cstr(s, "-0")) {
        *out_is_canon_num = true;
        return false;
    }
    double d = ajs_str_to_number(s);
    if (isnan(d) && !ajs_str_eq_cstr(s, "NaN")) return false;
    /* Check if canonical string representation matches */
    if (s->len16 > 0 && ((s->units[0] >= '0' && s->units[0] <= '9') || s->units[0] == '-' ||
        ajs_str_eq_cstr(s, "Infinity") || ajs_str_eq_cstr(s, "NaN"))) {
        if (ajs_str_eq_cstr(s, "Infinity") || ajs_str_eq_cstr(s, "-Infinity") || ajs_str_eq_cstr(s, "NaN")) {
            *out_is_canon_num = true;
            return false;
        }
        if (s->units[0] != ' ' && s->units[s->len16 - 1] != ' ') {
            if (d < 0.0 || floor(d) != d) {
                /* e.g. "-1", "1.5" */
                if (!(s->len16 > 1 && s->units[0] == '0' && s->units[1] >= '0' && s->units[1] <= '9')) {
                    *out_is_canon_num = true;
                    return false;
                }
            } else if (d >= (double)ta_len) {
                if (!(s->len16 > 1 && s->units[0] == '0')) {
                    *out_is_canon_num = true;
                    return false;
                }
            }
        }
    }
    return false;
}

bool ajs_obj_get_own_desc(AjsVm *vm, AjsObject *obj, AjsValue key, AjsPropDesc *out_desc, bool *out_found) {
    *out_found = false;
    if (!obj) return true;

    if (obj->kind == AJS_OBJ_PROXY) {
        AjsValue trap = ajs_proxy_get_trap(vm, obj, "getOwnPropertyDescriptor");
        if (vm->has_exception) return false;
        AjsObject *target_obj = (AjsObject *)ajs_val_to_gc(obj->u.proxy.target);
        if (trap == AJS_VAL_UNDEFINED) {
            return ajs_obj_get_own_desc(vm, target_obj, key, out_desc, out_found);
        }
        AjsValue args[2] = { obj->u.proxy.target, key };
        AjsValue res = ajs_call_internal(vm, trap, obj->u.proxy.handler, 2, args);
        if (vm->has_exception) return false;
        if (res == AJS_VAL_UNDEFINED) {
            AjsPropDesc tdesc;
            bool tfound = false;
            if (!ajs_obj_get_own_desc(vm, target_obj, key, &tdesc, &tfound)) return false;
            if (tfound && !(tdesc.flags & AJS_DESC_CONFIGURABLE)) {
                ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy getOwnPropertyDescriptor cannot hide non-configurable property");
                return false;
            }
            bool ext = true;
            if (!ajs_obj_is_extensible(vm, target_obj, &ext)) return false;
            if (tfound && !ext) {
                ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy getOwnPropertyDescriptor cannot hide property of non-extensible target");
                return false;
            }
            return true;
        }
        if (!ajs_val_is_gc_kind(res, AJS_GC_OBJECT)) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy getOwnPropertyDescriptor must return object or undefined");
            return false;
        }
        size_t rs = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, res);
        bool ok = ajs_to_prop_desc_internal(vm, res, out_desc);
        ajs_temp_root_restore(vm, rs);
        if (!ok) return false;
        /* Complete descriptor defaults */
        if (!(out_desc->flags & AJS_DESC_IS_ACCESSOR)) {
            out_desc->flags |= AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE;
        } else {
            out_desc->flags |= AJS_DESC_HAS_GET | AJS_DESC_HAS_SET;
        }
        out_desc->flags |= AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
        *out_found = true;
        return true;
    }

    if (obj->kind == AJS_OBJ_MODULE_NS) {
        if (ajs_val_is_gc_kind(key, AJS_GC_SYMBOL)) {
            if (ajs_val_to_gc(key) == (AjsGcHeader *)vm->well_known_syms[AJS_SYM_TO_STRING_TAG]) {
                memset(out_desc, 0, sizeof(*out_desc));
                out_desc->flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
                out_desc->value = ajs_val_from_gc(ajs_str_from_cstr(vm, "Module", true));
                *out_found = true;
            }
            return true;
        }
        bool has = false;
        if (!ajs_module_ns_has(vm, obj, key, &has)) return false;
        if (!has) return true;
        AjsValue val = ajs_module_ns_get(vm, obj, key);
        if (vm->has_exception) return false;
        memset(out_desc, 0, sizeof(*out_desc));
        out_desc->flags = AJS_DESC_WRITABLE | AJS_DESC_ENUMERABLE | AJS_DESC_HAS_VALUE |
                          AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
        out_desc->value = val;
        *out_found = true;
        return true;
    }

    if (obj->kind == AJS_OBJ_ARRAY && ajs_val_is_gc_kind(key, AJS_GC_STRING)) {
        AjsString *sk = (AjsString *)ajs_val_to_gc(key);
        if (ajs_str_eq(sk, vm->str_length)) {
            memset(out_desc, 0, sizeof(*out_desc));
            out_desc->flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
                              AJS_DESC_HAS_CONFIGURABLE | (obj->u.arr.length_writable ? AJS_DESC_WRITABLE : 0u);
            out_desc->value = ajs_val_number((double)obj->u.arr.length);
            *out_found = true;
            return true;
        }
        if (sk->is_array_idx && sk->array_idx < obj->u.arr.dense_len) {
            AjsValue v = obj->u.arr.dense[sk->array_idx];
            if (v != AJS_VAL_EMPTY) {
                memset(out_desc, 0, sizeof(*out_desc));
                out_desc->flags = AJS_DESC_WRITABLE | AJS_DESC_ENUMERABLE | AJS_DESC_CONFIGURABLE |
                                  AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
                out_desc->value = v;
                *out_found = true;
                return true;
            }
        }
    }

    if (obj->kind == AJS_OBJ_STRING_OBJ && ajs_val_is_gc_kind(key, AJS_GC_STRING)) {
        AjsString *sk = (AjsString *)ajs_val_to_gc(key);
        AjsString *prim = (AjsString *)ajs_val_to_gc(obj->u.prim_val);
        if (ajs_str_eq(sk, vm->str_length)) {
            memset(out_desc, 0, sizeof(*out_desc));
            out_desc->flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
            out_desc->value = ajs_val_number((double)prim->len16);
            *out_found = true;
            return true;
        }
        if (sk->is_array_idx && sk->array_idx < prim->len16) {
            AjsString *ch = ajs_str_from_utf16(vm, &prim->units[sk->array_idx], 1, true);
            memset(out_desc, 0, sizeof(*out_desc));
            out_desc->flags = AJS_DESC_ENUMERABLE | AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE |
                              AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
            out_desc->value = ajs_val_from_gc(ch);
            *out_found = true;
            return true;
        }
    }

    if (obj->kind == AJS_OBJ_TYPED_ARRAY && ajs_val_is_gc_kind(key, AJS_GC_STRING)) {
        AjsString *sk = (AjsString *)ajs_val_to_gc(key);
        size_t ta_len = 0, byte_off = 0;
        bool valid_buf = ajs_ta_check_valid_len(vm, obj, &ta_len, &byte_off);
        vm->has_exception = false; /* Out-of-bounds/detached returns undefined in CanonicalNumericIndex GetOwnProperty */
        bool is_canon = false;
        uint32_t valid_idx = 0;
        if (ajs_ta_canonical_index(sk, &is_canon, &valid_idx, valid_buf ? ta_len : 0)) {
            AjsValue val = AJS_VAL_UNDEFINED;
            if (!ajs_ta_get_index(vm, obj, valid_idx, &val)) return false;
            memset(out_desc, 0, sizeof(*out_desc));
            out_desc->flags = AJS_DESC_WRITABLE | AJS_DESC_ENUMERABLE | AJS_DESC_CONFIGURABLE |
                              AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
            out_desc->value = val;
            *out_found = true;
            return true;
        }
        if (is_canon) return true;
    }

    int32_t slot = ajs_obj_find_own_slot(obj, key);
    if (slot < 0) return true;

    AjsPropSlot *ps = &obj->props[slot];
    memset(out_desc, 0, sizeof(*out_desc));
    out_desc->flags = ps->flags | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
    if (ps->flags & AJS_DESC_IS_ACCESSOR) {
        out_desc->flags |= AJS_DESC_HAS_GET | AJS_DESC_HAS_SET;
        out_desc->getter = ps->value;
        out_desc->setter = ps->setter;
    } else {
        out_desc->flags |= AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE;
        out_desc->value = ps->value;
        if (obj->kind == AJS_OBJ_ARGUMENTS && obj->u.args.mapped && ajs_val_is_gc_kind(key, AJS_GC_STRING)) {
            AjsString *sk = (AjsString *)ajs_val_to_gc(key);
            if (sk->is_array_idx && sk->array_idx < obj->u.args.param_count) {
                uint16_t env_slot = obj->u.args.param_slots[sk->array_idx];
                if (env_slot != UINT16_MAX) {
                    out_desc->value = obj->u.args.env->slots[env_slot];
                }
            }
        }
    }
    *out_found = true;
    return true;
}

static void ajs_array_ensure_dense(AjsObject *obj, uint32_t idx) {
    if (idx >= obj->u.arr.dense_cap) {
        uint32_t ncap = obj->u.arr.dense_cap ? obj->u.arr.dense_cap * 2u : 8u;
        while (ncap <= idx) ncap *= 2u;
        AjsValue *nd = (AjsValue *)realloc(obj->u.arr.dense, ncap * sizeof(AjsValue));
        if (!nd) return;
        for (uint32_t i = obj->u.arr.dense_cap; i < ncap; i++) nd[i] = AJS_VAL_EMPTY;
        obj->u.arr.dense = nd;
        obj->u.arr.dense_cap = ncap;
    }
    while (obj->u.arr.dense_len <= idx) {
        obj->u.arr.dense[obj->u.arr.dense_len++] = AJS_VAL_EMPTY;
    }
}

static bool ajs_define_fail(AjsVm *vm, bool throw_on_err, const char *msg) {
    if (throw_on_err) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "%s", msg);
    return false;
}

bool ajs_obj_define_own_prop(AjsVm *vm, AjsObject *obj, AjsValue key, const AjsPropDesc *desc, bool throw_on_err) {
    if (!obj) return false;
    key = ajs_to_property_key(vm, key);
    if (vm->has_exception) return false;

    if (obj->kind == AJS_OBJ_PROXY) {
        AjsValue trap = ajs_proxy_get_trap(vm, obj, "defineProperty");
        if (vm->has_exception) return false;
        AjsObject *target_obj = (AjsObject *)ajs_val_to_gc(obj->u.proxy.target);
        if (trap == AJS_VAL_UNDEFINED) {
            return ajs_obj_define_own_prop(vm, target_obj, key, desc, throw_on_err);
        }
        size_t rs = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, key);
        ajs_temp_root_push(vm, trap);
        AjsValue desc_obj = ajs_from_prop_desc(vm, desc);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
        ajs_temp_root_push(vm, desc_obj);
        AjsValue args[3] = { obj->u.proxy.target, key, desc_obj };
        AjsValue res = ajs_call_internal(vm, trap, obj->u.proxy.handler, 3, args);
        ajs_temp_root_restore(vm, rs);
        if (vm->has_exception) return false;
        if (!ajs_to_bool(vm, res)) return ajs_define_fail(vm, throw_on_err, "Proxy defineProperty trap returned falsy");
        return true;
    }

    if (obj->kind == AJS_OBJ_MODULE_NS) {
        return ajs_define_fail(vm, throw_on_err, "Cannot define property on Module Namespace");
    }

    /* Array exotic [[DefineOwnProperty]] */
    if (obj->kind == AJS_OBJ_ARRAY && ajs_val_is_gc_kind(key, AJS_GC_STRING)) {
        AjsString *sk = (AjsString *)ajs_val_to_gc(key);
        if (ajs_str_eq(sk, vm->str_length)) {
            if (desc->flags & AJS_DESC_IS_ACCESSOR) {
                return ajs_define_fail(vm, throw_on_err, "Cannot redefine Array length as accessor");
            }
            if ((desc->flags & AJS_DESC_HAS_CONFIGURABLE) && (desc->flags & AJS_DESC_CONFIGURABLE)) {
                return ajs_define_fail(vm, throw_on_err, "Cannot redefine Array length configurable");
            }
            if ((desc->flags & AJS_DESC_HAS_ENUMERABLE) && (desc->flags & AJS_DESC_ENUMERABLE)) {
                return ajs_define_fail(vm, throw_on_err, "Cannot redefine Array length enumerable");
            }
            if (!(desc->flags & AJS_DESC_HAS_VALUE)) {
                if ((desc->flags & AJS_DESC_HAS_WRITABLE) && !(desc->flags & AJS_DESC_WRITABLE)) {
                    obj->u.arr.length_writable = false;
                } else if ((desc->flags & AJS_DESC_HAS_WRITABLE) && (desc->flags & AJS_DESC_WRITABLE) && !obj->u.arr.length_writable) {
                    return ajs_define_fail(vm, throw_on_err, "Cannot make non-writable Array length writable");
                }
                return true;
            }
            uint32_t new_len = 0;
            double d = 0.0;
            if (!ajs_to_number(vm, desc->value, &d)) return false;
            new_len = ajs_double_to_uint32(d);
            double d2 = 0.0;
            if (!ajs_to_number(vm, desc->value, &d2)) return false;
            if ((double)new_len != d2) {
                ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid array length");
                return false;
            }
            if (!obj->u.arr.length_writable && new_len != obj->u.arr.length) {
                return ajs_define_fail(vm, throw_on_err, "Cannot modify non-writable Array length");
            }
            bool succeeded = true;
            if (new_len < obj->u.arr.length) {
                /* Check sparse non-configurable elements >= new_len */
                uint32_t max_non_conf = 0;
                bool found_non_conf = false;
                for (uint32_t i = 0; i < obj->prop_count; i++) {
                    if (obj->props[i].key != AJS_VAL_EMPTY && ajs_val_is_gc_kind(obj->props[i].key, AJS_GC_STRING)) {
                        AjsString *pk = (AjsString *)ajs_val_to_gc(obj->props[i].key);
                        if (pk->is_array_idx && pk->array_idx >= new_len && !(obj->props[i].flags & AJS_DESC_CONFIGURABLE)) {
                            if (!found_non_conf || pk->array_idx + 1u > max_non_conf) {
                                max_non_conf = pk->array_idx + 1u;
                                found_non_conf = true;
                            }
                        }
                    }
                }
                uint32_t actual_len = found_non_conf ? max_non_conf : new_len;
                for (uint32_t i = actual_len; i < obj->u.arr.dense_len; i++) {
                    obj->u.arr.dense[i] = AJS_VAL_EMPTY;
                }
                if (obj->u.arr.dense_len > actual_len) obj->u.arr.dense_len = actual_len;
                for (uint32_t i = 0; i < obj->prop_count; i++) {
                    if (obj->props[i].key != AJS_VAL_EMPTY && ajs_val_is_gc_kind(obj->props[i].key, AJS_GC_STRING)) {
                        AjsString *pk = (AjsString *)ajs_val_to_gc(obj->props[i].key);
                        if (pk->is_array_idx && pk->array_idx >= actual_len) {
                            ajs_obj_remove_own_slot(obj, (int32_t)i);
                        }
                    }
                }
                obj->u.arr.length = actual_len;
                if (found_non_conf) succeeded = false;
            } else {
                obj->u.arr.length = new_len;
            }
            if ((desc->flags & AJS_DESC_HAS_WRITABLE) && !(desc->flags & AJS_DESC_WRITABLE)) {
                obj->u.arr.length_writable = false;
            }
            if (!succeeded) return ajs_define_fail(vm, throw_on_err, "Cannot delete non-configurable array element");
            return true;
        }

        if (sk->is_array_idx) {
            uint32_t idx = sk->array_idx;
            if (idx >= obj->u.arr.length && !obj->u.arr.length_writable) {
                return ajs_define_fail(vm, throw_on_err, "Cannot add element beyond non-writable Array length");
            }
            int32_t sp_slot = ajs_obj_find_own_slot(obj, key);
            bool can_dense = (sp_slot < 0) && idx < 65536u &&
                             !(desc->flags & AJS_DESC_IS_ACCESSOR) &&
                             (!(desc->flags & AJS_DESC_HAS_WRITABLE) || (desc->flags & AJS_DESC_WRITABLE)) &&
                             (!(desc->flags & AJS_DESC_HAS_ENUMERABLE) || (desc->flags & AJS_DESC_ENUMERABLE)) &&
                             (!(desc->flags & AJS_DESC_HAS_CONFIGURABLE) || (desc->flags & AJS_DESC_CONFIGURABLE));
            bool existed_dense = (idx < obj->u.arr.dense_len && obj->u.arr.dense[idx] != AJS_VAL_EMPTY);
            if (!existed_dense && sp_slot < 0 && !obj->extensible) {
                return ajs_define_fail(vm, throw_on_err, "Cannot add property to non-extensible Array");
            }
            if (can_dense && (!existed_dense ||
                (((desc->flags & AJS_DESC_HAS_WRITABLE) ? (desc->flags & AJS_DESC_WRITABLE) : 1) &&
                 ((desc->flags & AJS_DESC_HAS_ENUMERABLE) ? (desc->flags & AJS_DESC_ENUMERABLE) : 1) &&
                 ((desc->flags & AJS_DESC_HAS_CONFIGURABLE) ? (desc->flags & AJS_DESC_CONFIGURABLE) : 1)))) {
                ajs_array_ensure_dense(obj, idx);
                obj->u.arr.dense[idx] = (desc->flags & AJS_DESC_HAS_VALUE) ? desc->value :
                                        (existed_dense ? obj->u.arr.dense[idx] : AJS_VAL_UNDEFINED);
                if (idx >= obj->u.arr.length) obj->u.arr.length = idx + 1u;
                return true;
            }
            /* Demote dense element to property table if custom descriptor flags/accessor requested */
            if (existed_dense) {
                AjsValue cur_v = obj->u.arr.dense[idx];
                obj->u.arr.dense[idx] = AJS_VAL_EMPTY;
                ajs_obj_insert_own_slot(obj, key, AJS_DESC_WRITABLE | AJS_DESC_ENUMERABLE | AJS_DESC_CONFIGURABLE,
                                        cur_v, AJS_VAL_UNDEFINED);
            }
            if (idx >= obj->u.arr.length) obj->u.arr.length = idx + 1u;
        }
    }

    if (obj->kind == AJS_OBJ_TYPED_ARRAY && ajs_val_is_gc_kind(key, AJS_GC_STRING)) {
        AjsString *sk = (AjsString *)ajs_val_to_gc(key);
        size_t ta_len = 0, byte_off = 0;
        bool valid_buf = ajs_ta_check_valid_len(vm, obj, &ta_len, &byte_off);
        vm->has_exception = false;
        bool is_canon = false;
        uint32_t valid_idx = 0;
        if (ajs_ta_canonical_index(sk, &is_canon, &valid_idx, valid_buf ? ta_len : 0) || is_canon) {
            if (!is_canon || valid_idx >= (valid_buf ? ta_len : 0) || !valid_buf) {
                return ajs_define_fail(vm, throw_on_err, "Invalid TypedArray index in defineProperty");
            }
            if ((desc->flags & AJS_DESC_IS_ACCESSOR) ||
                ((desc->flags & AJS_DESC_HAS_CONFIGURABLE) && !(desc->flags & AJS_DESC_CONFIGURABLE)) ||
                ((desc->flags & AJS_DESC_HAS_ENUMERABLE) && !(desc->flags & AJS_DESC_ENUMERABLE)) ||
                ((desc->flags & AJS_DESC_HAS_WRITABLE) && !(desc->flags & AJS_DESC_WRITABLE))) {
                return ajs_define_fail(vm, throw_on_err, "Invalid TypedArray element descriptor");
            }
            if (desc->flags & AJS_DESC_HAS_VALUE) {
                if (!ajs_ta_set_index(vm, obj, valid_idx, desc->value)) return false;
            }
            return true;
        }
    }

    /* ValidateAndApplyPropertyDescriptor */
    AjsPropDesc current;
    bool found = false;
    if (!ajs_obj_get_own_desc(vm, obj, key, &current, &found)) return false;

    if (!found) {
        if (!obj->extensible) {
            return ajs_define_fail(vm, throw_on_err, "Cannot define property on non-extensible object");
        }
        uint32_t f = 0;
        if (desc->flags & AJS_DESC_ENUMERABLE) f |= AJS_DESC_ENUMERABLE;
        if (desc->flags & AJS_DESC_CONFIGURABLE) f |= AJS_DESC_CONFIGURABLE;
        if (desc->flags & AJS_DESC_IS_ACCESSOR) {
            f |= AJS_DESC_IS_ACCESSOR;
            ajs_obj_insert_own_slot(obj, key, f,
                                    (desc->flags & AJS_DESC_HAS_GET) ? desc->getter : AJS_VAL_UNDEFINED,
                                    (desc->flags & AJS_DESC_HAS_SET) ? desc->setter : AJS_VAL_UNDEFINED);
        } else {
            if (desc->flags & AJS_DESC_WRITABLE) f |= AJS_DESC_WRITABLE;
            ajs_obj_insert_own_slot(obj, key, f,
                                    (desc->flags & AJS_DESC_HAS_VALUE) ? desc->value : AJS_VAL_UNDEFINED,
                                    AJS_VAL_UNDEFINED);
        }
        return true;
    }

    if (!(current.flags & AJS_DESC_CONFIGURABLE)) {
        if ((desc->flags & AJS_DESC_HAS_CONFIGURABLE) && (desc->flags & AJS_DESC_CONFIGURABLE)) {
            return ajs_define_fail(vm, throw_on_err, "Cannot redefine non-configurable property");
        }
        if ((desc->flags & AJS_DESC_HAS_ENUMERABLE) &&
            ((desc->flags & AJS_DESC_ENUMERABLE) != (current.flags & AJS_DESC_ENUMERABLE))) {
            return ajs_define_fail(vm, throw_on_err, "Cannot change enumerable of non-configurable property");
        }
        bool cur_acc = (current.flags & AJS_DESC_IS_ACCESSOR) != 0;
        bool req_acc = (desc->flags & AJS_DESC_IS_ACCESSOR) != 0;
        bool req_data = (desc->flags & (AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE)) != 0;
        if ((cur_acc && req_data) || (!cur_acc && req_acc)) {
            return ajs_define_fail(vm, throw_on_err, "Cannot change descriptor kind of non-configurable property");
        }
        if (!cur_acc) {
            if (!(current.flags & AJS_DESC_WRITABLE)) {
                if ((desc->flags & AJS_DESC_HAS_WRITABLE) && (desc->flags & AJS_DESC_WRITABLE)) {
                    return ajs_define_fail(vm, throw_on_err, "Cannot make non-writable non-configurable property writable");
                }
                if ((desc->flags & AJS_DESC_HAS_VALUE) && !ajs_same_value(vm, desc->value, current.value)) {
                    return ajs_define_fail(vm, throw_on_err, "Cannot change value of non-configurable non-writable property");
                }
            }
        } else {
            if ((desc->flags & AJS_DESC_HAS_GET) && !ajs_same_value(vm, desc->getter, current.getter)) {
                return ajs_define_fail(vm, throw_on_err, "Cannot change getter of non-configurable accessor");
            }
            if ((desc->flags & AJS_DESC_HAS_SET) && !ajs_same_value(vm, desc->setter, current.setter)) {
                return ajs_define_fail(vm, throw_on_err, "Cannot change setter of non-configurable accessor");
            }
        }
    }

    int32_t slot = ajs_obj_find_own_slot(obj, key);
    if (slot < 0) {
        /* String exotic own index/length reached here -> non-configurable read-only no-op */
        return true;
    }
    AjsPropSlot *ps = &obj->props[slot];
    if (desc->flags & AJS_DESC_HAS_ENUMERABLE) {
        if (desc->flags & AJS_DESC_ENUMERABLE) ps->flags |= AJS_DESC_ENUMERABLE;
        else ps->flags &= ~AJS_DESC_ENUMERABLE;
    }
    if (desc->flags & AJS_DESC_HAS_CONFIGURABLE) {
        if (desc->flags & AJS_DESC_CONFIGURABLE) ps->flags |= AJS_DESC_CONFIGURABLE;
        else ps->flags &= ~AJS_DESC_CONFIGURABLE;
    }

    bool cur_acc = (ps->flags & AJS_DESC_IS_ACCESSOR) != 0;
    if (desc->flags & AJS_DESC_IS_ACCESSOR) {
        if (!cur_acc) {
            ps->flags = (ps->flags & ~(AJS_DESC_WRITABLE)) | AJS_DESC_IS_ACCESSOR;
            ps->value = AJS_VAL_UNDEFINED;
            ps->setter = AJS_VAL_UNDEFINED;
        }
        if (desc->flags & AJS_DESC_HAS_GET) ps->value = desc->getter;
        if (desc->flags & AJS_DESC_HAS_SET) ps->setter = desc->setter;
    } else if (desc->flags & (AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE)) {
        if (cur_acc) {
            ps->flags &= ~AJS_DESC_IS_ACCESSOR;
            ps->value = AJS_VAL_UNDEFINED;
            ps->setter = AJS_VAL_UNDEFINED;
        }
        if (desc->flags & AJS_DESC_HAS_WRITABLE) {
            if (desc->flags & AJS_DESC_WRITABLE) ps->flags |= AJS_DESC_WRITABLE;
            else ps->flags &= ~AJS_DESC_WRITABLE;
        }
        if (desc->flags & AJS_DESC_HAS_VALUE) {
            ps->value = desc->value;
            if (obj->kind == AJS_OBJ_ARGUMENTS && obj->u.args.mapped && ajs_val_is_gc_kind(key, AJS_GC_STRING)) {
                AjsString *sk = (AjsString *)ajs_val_to_gc(key);
                if (sk->is_array_idx && sk->array_idx < obj->u.args.param_count) {
                    uint16_t eslot = obj->u.args.param_slots[sk->array_idx];
                    if (eslot != UINT16_MAX) obj->u.args.env->slots[eslot] = desc->value;
                }
            }
        }
        if ((desc->flags & AJS_DESC_HAS_WRITABLE) && !(desc->flags & AJS_DESC_WRITABLE) &&
            obj->kind == AJS_OBJ_ARGUMENTS && obj->u.args.mapped && ajs_val_is_gc_kind(key, AJS_GC_STRING)) {
            AjsString *sk = (AjsString *)ajs_val_to_gc(key);
            if (sk->is_array_idx && sk->array_idx < obj->u.args.param_count) {
                obj->u.args.param_slots[sk->array_idx] = UINT16_MAX;
            }
        }
    }
    return true;
}

/* ============================================================================
 * Prototype, Extensibility, Get, Set, Has, Delete & OwnPropertyKeys
 * ============================================================================ */

bool ajs_obj_get_proto(AjsVm *vm, AjsObject *obj, AjsValue *out_proto) {
    if (!obj) { *out_proto = AJS_VAL_NULL; return true; }
    if (obj->kind == AJS_OBJ_PROXY) {
        AjsValue trap = ajs_proxy_get_trap(vm, obj, "getPrototypeOf");
        if (vm->has_exception) return false;
        AjsObject *target_obj = (AjsObject *)ajs_val_to_gc(obj->u.proxy.target);
        if (trap == AJS_VAL_UNDEFINED) return ajs_obj_get_proto(vm, target_obj, out_proto);
        AjsValue res = ajs_call_internal(vm, trap, obj->u.proxy.handler, 1, &obj->u.proxy.target);
        if (vm->has_exception) return false;
        if (res != AJS_VAL_NULL && !ajs_val_is_gc_kind(res, AJS_GC_OBJECT)) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy getPrototypeOf must return object or null");
            return false;
        }
        bool ext = true;
        if (!ajs_obj_is_extensible(vm, target_obj, &ext)) return false;
        if (!ext) {
            AjsValue tproto = AJS_VAL_NULL;
            if (!ajs_obj_get_proto(vm, target_obj, &tproto)) return false;
            if (!ajs_same_value(vm, res, tproto)) {
                ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy getPrototypeOf invariant violated on non-extensible target");
                return false;
            }
        }
        *out_proto = res;
        return true;
    }
    if (obj->kind == AJS_OBJ_MODULE_NS) {
        *out_proto = AJS_VAL_NULL;
        return true;
    }
    *out_proto = obj->proto;
    return true;
}

bool ajs_obj_set_proto(AjsVm *vm, AjsObject *obj, AjsValue proto, bool throw_on_err) {
    if (!obj) return false;
    if (proto != AJS_VAL_NULL && !ajs_val_is_gc_kind(proto, AJS_GC_OBJECT)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object prototype may only be an Object or null");
        return false;
    }
    if (obj->kind == AJS_OBJ_PROXY) {
        AjsValue trap = ajs_proxy_get_trap(vm, obj, "setPrototypeOf");
        if (vm->has_exception) return false;
        AjsObject *target_obj = (AjsObject *)ajs_val_to_gc(obj->u.proxy.target);
        if (trap == AJS_VAL_UNDEFINED) return ajs_obj_set_proto(vm, target_obj, proto, throw_on_err);
        AjsValue args[2] = { obj->u.proxy.target, proto };
        AjsValue res = ajs_call_internal(vm, trap, obj->u.proxy.handler, 2, args);
        if (vm->has_exception) return false;
        if (!ajs_to_bool(vm, res)) return ajs_define_fail(vm, throw_on_err, "Proxy setPrototypeOf returned false");
        bool ext = true;
        if (!ajs_obj_is_extensible(vm, target_obj, &ext)) return false;
        if (!ext) {
            AjsValue tproto = AJS_VAL_NULL;
            if (!ajs_obj_get_proto(vm, target_obj, &tproto)) return false;
            if (!ajs_same_value(vm, proto, tproto)) {
                ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy setPrototypeOf invariant violated");
                return false;
            }
        }
        return true;
    }
    if (obj->kind == AJS_OBJ_MODULE_NS) {
        if (proto == AJS_VAL_NULL) return true;
        return ajs_define_fail(vm, throw_on_err, "Cannot set prototype of Module Namespace");
    }
    if (ajs_same_value(vm, proto, obj->proto)) return true;
    if (!obj->extensible) {
        return ajs_define_fail(vm, throw_on_err, "Object is not extensible");
    }
    /* Cycle check along ordinary prototype chain */
    AjsValue p = proto;
    int depth = 0;
    while (p != AJS_VAL_NULL && ajs_val_is_gc_kind(p, AJS_GC_OBJECT) && depth++ < 1024) {
        AjsObject *po = (AjsObject *)ajs_val_to_gc(p);
        if (po == obj) {
            return ajs_define_fail(vm, throw_on_err, "Cyclic __proto__ value");
        }
        if (po->kind == AJS_OBJ_PROXY) break;
        p = po->proto;
    }
    obj->proto = proto;
    return true;
}

bool ajs_obj_is_extensible(AjsVm *vm, AjsObject *obj, bool *out_ext) {
    if (!obj) { *out_ext = false; return true; }
    if (obj->kind == AJS_OBJ_PROXY) {
        AjsValue trap = ajs_proxy_get_trap(vm, obj, "isExtensible");
        if (vm->has_exception) return false;
        AjsObject *target_obj = (AjsObject *)ajs_val_to_gc(obj->u.proxy.target);
        if (trap == AJS_VAL_UNDEFINED) return ajs_obj_is_extensible(vm, target_obj, out_ext);
        AjsValue res = ajs_call_internal(vm, trap, obj->u.proxy.handler, 1, &obj->u.proxy.target);
        if (vm->has_exception) return false;
        bool bres = ajs_to_bool(vm, res);
        bool text = true;
        if (!ajs_obj_is_extensible(vm, target_obj, &text)) return false;
        if (bres != text) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy isExtensible invariant violated");
            return false;
        }
        *out_ext = bres;
        return true;
    }
    if (obj->kind == AJS_OBJ_MODULE_NS) {
        *out_ext = false;
        return true;
    }
    *out_ext = obj->extensible;
    return true;
}

bool ajs_obj_prevent_extensions(AjsVm *vm, AjsObject *obj, bool throw_on_err) {
    if (!obj) return false;
    if (obj->kind == AJS_OBJ_PROXY) {
        AjsValue trap = ajs_proxy_get_trap(vm, obj, "preventExtensions");
        if (vm->has_exception) return false;
        AjsObject *target_obj = (AjsObject *)ajs_val_to_gc(obj->u.proxy.target);
        if (trap == AJS_VAL_UNDEFINED) return ajs_obj_prevent_extensions(vm, target_obj, throw_on_err);
        AjsValue res = ajs_call_internal(vm, trap, obj->u.proxy.handler, 1, &obj->u.proxy.target);
        if (vm->has_exception) return false;
        if (!ajs_to_bool(vm, res)) return ajs_define_fail(vm, throw_on_err, "Proxy preventExtensions returned false");
        bool text = true;
        if (!ajs_obj_is_extensible(vm, target_obj, &text)) return false;
        if (text) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy preventExtensions returned true while target is extensible");
            return false;
        }
        return true;
    }
    obj->extensible = false;
    return true;
}

bool ajs_obj_has_prop(AjsVm *vm, AjsObject *obj, AjsValue key, bool *out_has) {
    *out_has = false;
    if (!obj) return true;
    key = ajs_to_property_key(vm, key);
    if (vm->has_exception) return false;

    int depth = 0;
    AjsObject *cur = obj;
    while (cur && depth++ < 1024) {
        if (cur->kind == AJS_OBJ_PROXY) {
            AjsValue trap = ajs_proxy_get_trap(vm, cur, "has");
            if (vm->has_exception) return false;
            AjsObject *target_obj = (AjsObject *)ajs_val_to_gc(cur->u.proxy.target);
            if (trap == AJS_VAL_UNDEFINED) {
                cur = target_obj;
                continue;
            }
            AjsValue args[2] = { cur->u.proxy.target, key };
            AjsValue res = ajs_call_internal(vm, trap, cur->u.proxy.handler, 2, args);
            if (vm->has_exception) return false;
            bool bres = ajs_to_bool(vm, res);
            if (!bres) {
                AjsPropDesc tdesc;
                bool tfound = false;
                if (!ajs_obj_get_own_desc(vm, target_obj, key, &tdesc, &tfound)) return false;
                if (tfound) {
                    if (!(tdesc.flags & AJS_DESC_CONFIGURABLE)) {
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy has trap cannot hide non-configurable property");
                        return false;
                    }
                    bool text = true;
                    if (!ajs_obj_is_extensible(vm, target_obj, &text)) return false;
                    if (!text) {
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy has trap cannot hide property of non-extensible target");
                        return false;
                    }
                }
            }
            *out_has = bres;
            return true;
        }
        if (cur->kind == AJS_OBJ_MODULE_NS) {
            if (ajs_val_is_gc_kind(key, AJS_GC_SYMBOL)) {
                *out_has = (ajs_val_to_gc(key) == (AjsGcHeader *)vm->well_known_syms[AJS_SYM_TO_STRING_TAG]);
                return true;
            }
            return ajs_module_ns_has(vm, cur, key, out_has);
        }
        if (cur->kind == AJS_OBJ_TYPED_ARRAY && ajs_val_is_gc_kind(key, AJS_GC_STRING)) {
            AjsString *sk = (AjsString *)ajs_val_to_gc(key);
            size_t ta_len = 0, byte_off = 0;
            bool valid_buf = ajs_ta_check_valid_len(vm, cur, &ta_len, &byte_off);
            vm->has_exception = false;
            bool is_canon = false;
            uint32_t valid_idx = 0;
            if (ajs_ta_canonical_index(sk, &is_canon, &valid_idx, valid_buf ? ta_len : 0)) {
                *out_has = true;
                return true;
            }
            if (is_canon) {
                *out_has = false;
                return true;
            }
        }
        AjsPropDesc desc;
        bool found = false;
        if (!ajs_obj_get_own_desc(vm, cur, key, &desc, &found)) return false;
        if (found) {
            *out_has = true;
            return true;
        }
        AjsValue p = AJS_VAL_NULL;
        if (!ajs_obj_get_proto(vm, cur, &p)) return false;
        if (p == AJS_VAL_NULL || !ajs_val_is_gc_kind(p, AJS_GC_OBJECT)) break;
        cur = (AjsObject *)ajs_val_to_gc(p);
    }
    return true;
}

AjsValue ajs_obj_get(AjsVm *vm, AjsValue target_val, AjsValue key, AjsValue receiver) {
    if (target_val == AJS_VAL_UNDEFINED || target_val == AJS_VAL_NULL) {
        AjsString *ks = ajs_val_is_gc_kind(key, AJS_GC_STRING) ? (AjsString *)ajs_val_to_gc(key) : NULL;
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot read properties of %s (reading '%s')",
                                  target_val == AJS_VAL_UNDEFINED ? "undefined" : "null",
                                  ks ? ks->utf8 : "symbol");
    }
    key = ajs_to_property_key(vm, key);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;

    AjsRealm *r = vm->active_realm ? vm->active_realm : vm->main_realm;
    AjsObject *cur = NULL;
    if (ajs_val_is_gc_kind(target_val, AJS_GC_OBJECT)) {
        cur = (AjsObject *)ajs_val_to_gc(target_val);
    } else {
        /* Primitive property access: check String.length & index fast path, then prototype */
        if (ajs_val_is_gc_kind(target_val, AJS_GC_STRING) && ajs_val_is_gc_kind(key, AJS_GC_STRING)) {
            AjsString *ps = (AjsString *)ajs_val_to_gc(target_val);
            AjsString *ks = (AjsString *)ajs_val_to_gc(key);
            if (ajs_str_eq(ks, vm->str_length)) return ajs_val_number((double)ps->len16);
            if (ks->is_array_idx && ks->array_idx < ps->len16) {
                return ajs_val_from_gc(ajs_str_from_utf16(vm, &ps->units[ks->array_idx], 1, true));
            }
        }
        AjsValue pval = AJS_VAL_NULL;
        if (target_val == AJS_VAL_TRUE || target_val == AJS_VAL_FALSE) pval = r->boolean_proto;
        else if (ajs_val_is_number(target_val)) pval = r->number_proto;
        else if (ajs_val_is_gc_kind(target_val, AJS_GC_STRING)) pval = r->string_proto;
        else if (ajs_val_is_gc_kind(target_val, AJS_GC_SYMBOL)) pval = r->symbol_proto;
        else if (ajs_val_is_gc_kind(target_val, AJS_GC_BIGINT)) pval = r->bigint_proto;
        if (!ajs_val_is_gc_kind(pval, AJS_GC_OBJECT)) return AJS_VAL_UNDEFINED;
        cur = (AjsObject *)ajs_val_to_gc(pval);
    }

    int depth = 0;
    while (cur && depth++ < 1024) {
        if (cur->kind == AJS_OBJ_PROXY) {
            AjsValue trap = ajs_proxy_get_trap(vm, cur, "get");
            if (vm->has_exception) return AJS_VAL_EXCEPTION;
            AjsObject *target_obj = (AjsObject *)ajs_val_to_gc(cur->u.proxy.target);
            if (trap == AJS_VAL_UNDEFINED) {
                cur = target_obj;
                continue;
            }
            AjsValue args[3] = { cur->u.proxy.target, key, receiver };
            AjsValue res = ajs_call_internal(vm, trap, cur->u.proxy.handler, 3, args);
            if (vm->has_exception) return AJS_VAL_EXCEPTION;
            /* Proxy [[Get]] invariant check */
            AjsPropDesc tdesc;
            bool tfound = false;
            if (!ajs_obj_get_own_desc(vm, target_obj, key, &tdesc, &tfound)) return AJS_VAL_EXCEPTION;
            if (tfound && !(tdesc.flags & AJS_DESC_CONFIGURABLE)) {
                if (!(tdesc.flags & AJS_DESC_IS_ACCESSOR) && !(tdesc.flags & AJS_DESC_WRITABLE)) {
                    if (!ajs_same_value(vm, res, tdesc.value)) {
                        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy get invariant violated for non-writable non-configurable property");
                    }
                } else if ((tdesc.flags & AJS_DESC_IS_ACCESSOR) && tdesc.getter == AJS_VAL_UNDEFINED && res != AJS_VAL_UNDEFINED) {
                    return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy get invariant violated for getter-less accessor");
                }
            }
            return res;
        }
        if (cur->kind == AJS_OBJ_MODULE_NS) {
            if (ajs_val_is_gc_kind(key, AJS_GC_SYMBOL)) {
                if (ajs_val_to_gc(key) == (AjsGcHeader *)vm->well_known_syms[AJS_SYM_TO_STRING_TAG]) {
                    return ajs_val_from_gc(ajs_str_from_cstr(vm, "Module", true));
                }
                return AJS_VAL_UNDEFINED;
            }
            return ajs_module_ns_get(vm, cur, key);
        }
        if (cur->kind == AJS_OBJ_TYPED_ARRAY && ajs_val_is_gc_kind(key, AJS_GC_STRING)) {
            AjsString *sk = (AjsString *)ajs_val_to_gc(key);
            size_t ta_len = 0, byte_off = 0;
            bool valid_buf = ajs_ta_check_valid_len(vm, cur, &ta_len, &byte_off);
            vm->has_exception = false;
            bool is_canon = false;
            uint32_t valid_idx = 0;
            if (ajs_ta_canonical_index(sk, &is_canon, &valid_idx, valid_buf ? ta_len : 0)) {
                AjsValue v = AJS_VAL_UNDEFINED;
                if (!ajs_ta_get_index(vm, cur, valid_idx, &v)) return AJS_VAL_EXCEPTION;
                return v;
            }
            if (is_canon) return AJS_VAL_UNDEFINED; /* Never walks prototype for canonical numeric key! */
        }

        AjsPropDesc desc;
        bool found = false;
        if (!ajs_obj_get_own_desc(vm, cur, key, &desc, &found)) return AJS_VAL_EXCEPTION;
        if (found) {
            if (!(desc.flags & AJS_DESC_IS_ACCESSOR)) return desc.value;
            if (desc.getter == AJS_VAL_UNDEFINED) return AJS_VAL_UNDEFINED;
            return ajs_call_internal(vm, desc.getter, receiver, 0, NULL);
        }
        AjsValue p = AJS_VAL_NULL;
        if (!ajs_obj_get_proto(vm, cur, &p)) return AJS_VAL_EXCEPTION;
        if (p == AJS_VAL_NULL || !ajs_val_is_gc_kind(p, AJS_GC_OBJECT)) break;
        cur = (AjsObject *)ajs_val_to_gc(p);
    }
    return AJS_VAL_UNDEFINED;
}

bool ajs_obj_set(AjsVm *vm, AjsValue target_val, AjsValue key, AjsValue val, AjsValue receiver, bool strict) {
    if (target_val == AJS_VAL_UNDEFINED || target_val == AJS_VAL_NULL) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot set properties of %s",
                           target_val == AJS_VAL_UNDEFINED ? "undefined" : "null");
        return false;
    }
    key = ajs_to_property_key(vm, key);
    if (vm->has_exception) return false;

    if (!ajs_val_is_gc_kind(target_val, AJS_GC_OBJECT)) {
        /* Setting property on primitive: check prototype setter, else fail in strict mode */
        AjsRealm *r = vm->active_realm ? vm->active_realm : vm->main_realm;
        AjsValue pval = AJS_VAL_NULL;
        if (target_val == AJS_VAL_TRUE || target_val == AJS_VAL_FALSE) pval = r->boolean_proto;
        else if (ajs_val_is_number(target_val)) pval = r->number_proto;
        else if (ajs_val_is_gc_kind(target_val, AJS_GC_STRING)) pval = r->string_proto;
        else if (ajs_val_is_gc_kind(target_val, AJS_GC_SYMBOL)) pval = r->symbol_proto;
        else if (ajs_val_is_gc_kind(target_val, AJS_GC_BIGINT)) pval = r->bigint_proto;
        if (ajs_val_is_gc_kind(pval, AJS_GC_OBJECT)) {
            AjsObject *cur = (AjsObject *)ajs_val_to_gc(pval);
            int d = 0;
            while (cur && d++ < 128) {
                AjsPropDesc pd;
                bool f = false;
                if (!ajs_obj_get_own_desc(vm, cur, key, &pd, &f)) return false;
                if (f && (pd.flags & AJS_DESC_IS_ACCESSOR)) {
                    if (pd.setter == AJS_VAL_UNDEFINED) {
                        return ajs_define_fail(vm, strict, "Cannot set property with only a getter");
                    }
                    ajs_call_internal(vm, pd.setter, receiver, 1, &val);
                    return !vm->has_exception;
                }
                if (!ajs_val_is_gc_kind(cur->proto, AJS_GC_OBJECT)) break;
                cur = (AjsObject *)ajs_val_to_gc(cur->proto);
            }
        }
        if (strict) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot create property on primitive value");
            return false;
        }
        return true;
    }

    AjsObject *cur = (AjsObject *)ajs_val_to_gc(target_val);
    int depth = 0;
    while (cur && depth++ < 1024) {
        if (cur->kind == AJS_OBJ_PROXY) {
            AjsValue trap = ajs_proxy_get_trap(vm, cur, "set");
            if (vm->has_exception) return false;
            AjsObject *target_obj = (AjsObject *)ajs_val_to_gc(cur->u.proxy.target);
            if (trap == AJS_VAL_UNDEFINED) {
                cur = target_obj;
                continue;
            }
            AjsValue args[4] = { cur->u.proxy.target, key, val, receiver };
            AjsValue res = ajs_call_internal(vm, trap, cur->u.proxy.handler, 4, args);
            if (vm->has_exception) return false;
            if (!ajs_to_bool(vm, res)) {
                return ajs_define_fail(vm, strict, "Proxy set trap returned falsy");
            }
            AjsPropDesc tdesc;
            bool tfound = false;
            if (!ajs_obj_get_own_desc(vm, target_obj, key, &tdesc, &tfound)) return false;
            if (tfound && !(tdesc.flags & AJS_DESC_CONFIGURABLE)) {
                if (!(tdesc.flags & AJS_DESC_IS_ACCESSOR) && !(tdesc.flags & AJS_DESC_WRITABLE)) {
                    if (!ajs_same_value(vm, val, tdesc.value)) {
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy set invariant violated");
                        return false;
                    }
                } else if ((tdesc.flags & AJS_DESC_IS_ACCESSOR) && tdesc.setter == AJS_VAL_UNDEFINED) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy set invariant violated on setter-less accessor");
                    return false;
                }
            }
            return true;
        }

        if (cur->kind == AJS_OBJ_MODULE_NS) {
            return ajs_define_fail(vm, strict, "Cannot assign to read only Module Namespace");
        }

        if (cur->kind == AJS_OBJ_TYPED_ARRAY && ajs_val_is_gc_kind(key, AJS_GC_STRING)) {
            AjsString *sk = (AjsString *)ajs_val_to_gc(key);
            size_t ta_len = 0, byte_off = 0;
            bool valid_buf = ajs_ta_check_valid_len(vm, cur, &ta_len, &byte_off);
            vm->has_exception = false;
            bool is_canon = false;
            uint32_t valid_idx = 0;
            bool in_bounds = ajs_ta_canonical_index(sk, &is_canon, &valid_idx, valid_buf ? ta_len : 0);
            if (is_canon) {
                /* Must still convert value to numeric/bigint first per ECMA-262 TypedArraySetElement! */
                if (!ajs_same_value(vm, ajs_val_from_gc(cur), receiver)) break;
                if (in_bounds && valid_buf) {
                    return ajs_ta_set_index(vm, cur, valid_idx, val);
                }
                /* Convert value for side effects even if OOB/detached */
                if (cur->u.view.kind == AJS_TA_BIGINT64 || cur->u.view.kind == AJS_TA_BIGUINT64) {
                    AjsValue num;
                    if (!ajs_to_numeric(vm, val, &num)) return false;
                    if (!ajs_val_is_gc_kind(num, AJS_GC_BIGINT)) {
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot convert value to BigInt");
                        return false;
                    }
                } else {
                    double d = 0.0;
                    if (!ajs_to_number(vm, val, &d)) return false;
                }
                return true;
            }
        }

        AjsPropDesc own_desc;
        bool found = false;
        if (!ajs_obj_get_own_desc(vm, cur, key, &own_desc, &found)) return false;
        if (found) {
            if (own_desc.flags & AJS_DESC_IS_ACCESSOR) {
                if (own_desc.setter == AJS_VAL_UNDEFINED) {
                    return ajs_define_fail(vm, strict, "Cannot set property which has only a getter");
                }
                ajs_call_internal(vm, own_desc.setter, receiver, 1, &val);
                return !vm->has_exception;
            }
            if (!(own_desc.flags & AJS_DESC_WRITABLE)) {
                return ajs_define_fail(vm, strict, "Cannot assign to read only property");
            }
            break;
        }
        AjsValue p = AJS_VAL_NULL;
        if (!ajs_obj_get_proto(vm, cur, &p)) return false;
        if (p == AJS_VAL_NULL || !ajs_val_is_gc_kind(p, AJS_GC_OBJECT)) break;
        cur = (AjsObject *)ajs_val_to_gc(p);
    }

    if (!ajs_val_is_gc_kind(receiver, AJS_GC_OBJECT)) {
        return ajs_define_fail(vm, strict, "Receiver is not an object");
    }
    AjsObject *recv_obj = (AjsObject *)ajs_val_to_gc(receiver);
    AjsPropDesc existing;
    bool ex_found = false;
    if (!ajs_obj_get_own_desc(vm, recv_obj, key, &existing, &ex_found)) return false;
    if (ex_found) {
        if (existing.flags & AJS_DESC_IS_ACCESSOR) {
            return ajs_define_fail(vm, strict, "Receiver property is an accessor");
        }
        if (!(existing.flags & AJS_DESC_WRITABLE)) {
            return ajs_define_fail(vm, strict, "Receiver property is not writable");
        }
        AjsPropDesc upd;
        memset(&upd, 0, sizeof(upd));
        upd.flags = AJS_DESC_HAS_VALUE;
        upd.value = val;
        return ajs_obj_define_own_prop(vm, recv_obj, key, &upd, strict);
    }
    AjsPropDesc create_desc;
    memset(&create_desc, 0, sizeof(create_desc));
    create_desc.flags = AJS_DESC_WRITABLE | AJS_DESC_ENUMERABLE | AJS_DESC_CONFIGURABLE |
                        AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
    create_desc.value = val;
    return ajs_obj_define_own_prop(vm, recv_obj, key, &create_desc, strict);
}

bool ajs_obj_delete(AjsVm *vm, AjsObject *obj, AjsValue key, bool strict) {
    if (!obj) return true;
    key = ajs_to_property_key(vm, key);
    if (vm->has_exception) return false;

    if (obj->kind == AJS_OBJ_PROXY) {
        AjsValue trap = ajs_proxy_get_trap(vm, obj, "deleteProperty");
        if (vm->has_exception) return false;
        AjsObject *target_obj = (AjsObject *)ajs_val_to_gc(obj->u.proxy.target);
        if (trap == AJS_VAL_UNDEFINED) return ajs_obj_delete(vm, target_obj, key, strict);
        AjsValue args[2] = { obj->u.proxy.target, key };
        AjsValue res = ajs_call_internal(vm, trap, obj->u.proxy.handler, 2, args);
        if (vm->has_exception) return false;
        if (!ajs_to_bool(vm, res)) return ajs_define_fail(vm, strict, "Proxy deleteProperty returned false");
        AjsPropDesc tdesc;
        bool tfound = false;
        if (!ajs_obj_get_own_desc(vm, target_obj, key, &tdesc, &tfound)) return false;
        if (tfound) {
            if (!(tdesc.flags & AJS_DESC_CONFIGURABLE)) {
                ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy deleteProperty cannot delete non-configurable property");
                return false;
            }
            bool text = true;
            if (!ajs_obj_is_extensible(vm, target_obj, &text)) return false;
            if (!text) {
                ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy deleteProperty cannot delete property on non-extensible target");
                return false;
            }
        }
        return true;
    }

    if (obj->kind == AJS_OBJ_MODULE_NS && ajs_val_is_gc_kind(key, AJS_GC_STRING)) {
        bool has = false;
        if (!ajs_module_ns_has(vm, obj, key, &has)) return false;
        if (has) return ajs_define_fail(vm, strict, "Cannot delete export from Module Namespace");
        return true;
    }

    if (obj->kind == AJS_OBJ_ARRAY && ajs_val_is_gc_kind(key, AJS_GC_STRING)) {
        AjsString *sk = (AjsString *)ajs_val_to_gc(key);
        if (sk->is_array_idx && sk->array_idx < obj->u.arr.dense_len && obj->u.arr.dense[sk->array_idx] != AJS_VAL_EMPTY) {
            obj->u.arr.dense[sk->array_idx] = AJS_VAL_EMPTY;
            return true;
        }
    }

    AjsPropDesc desc;
    bool found = false;
    if (!ajs_obj_get_own_desc(vm, obj, key, &desc, &found)) return false;
    if (!found) return true;
    if (!(desc.flags & AJS_DESC_CONFIGURABLE)) {
        return ajs_define_fail(vm, strict, "Cannot delete non-configurable property");
    }
    int32_t slot = ajs_obj_find_own_slot(obj, key);
    if (slot >= 0) {
        if (obj->kind == AJS_OBJ_ARGUMENTS && obj->u.args.mapped && ajs_val_is_gc_kind(key, AJS_GC_STRING)) {
            AjsString *sk = (AjsString *)ajs_val_to_gc(key);
            if (sk->is_array_idx && sk->array_idx < obj->u.args.param_count) {
                obj->u.args.param_slots[sk->array_idx] = UINT16_MAX;
            }
        }
        ajs_obj_remove_own_slot(obj, slot);
    }
    return true;
}

typedef struct AjsIdxPair {
    uint32_t idx;
    AjsValue key;
} AjsIdxPair;

static int ajs_cmp_idx_pair(const void *pa, const void *pb) {
    const AjsIdxPair *a = (const AjsIdxPair *)pa;
    const AjsIdxPair *b = (const AjsIdxPair *)pb;
    return (a->idx < b->idx) ? -1 : ((a->idx > b->idx) ? 1 : 0);
}

bool ajs_obj_own_keys(AjsVm *vm, AjsObject *obj, bool enumerable_only, bool include_strings,
                      bool include_symbols, AjsValue **out_keys, uint32_t *out_count) {
    *out_keys = NULL;
    *out_count = 0;
    if (!obj) return true;

    if (obj->kind == AJS_OBJ_PROXY) {
        AjsValue trap = ajs_proxy_get_trap(vm, obj, "ownKeys");
        if (vm->has_exception) return false;
        AjsObject *target_obj = (AjsObject *)ajs_val_to_gc(obj->u.proxy.target);
        if (trap == AJS_VAL_UNDEFINED) {
            return ajs_obj_own_keys(vm, target_obj, enumerable_only, include_strings, include_symbols, out_keys, out_count);
        }
        AjsValue res = ajs_call_internal(vm, trap, obj->u.proxy.handler, 1, &obj->u.proxy.target);
        if (vm->has_exception) return false;
        if (!ajs_val_is_gc_kind(res, AJS_GC_OBJECT)) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy ownKeys must return an object");
            return false;
        }
        size_t rs = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, res);
        AjsValue len_val = ajs_get_prop_str(vm, res, "length");
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
        uint64_t raw_len = 0;
        if (!ajs_to_length(vm, len_val, &raw_len) || raw_len > 1048576u) {
            ajs_temp_root_restore(vm, rs);
            if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Proxy ownKeys length too large");
            return false;
        }
        AjsValue *raw_keys = raw_len ? (AjsValue *)malloc((size_t)raw_len * sizeof(AjsValue)) : NULL;
        uint32_t out_n = 0;
        for (uint64_t i = 0; i < raw_len; i++) {
            AjsValue elem = ajs_get_prop_index(vm, res, (uint32_t)i);
            if (vm->has_exception) { free(raw_keys); ajs_temp_root_restore(vm, rs); return false; }
            if (!ajs_val_is_gc_kind(elem, AJS_GC_STRING) && !ajs_val_is_gc_kind(elem, AJS_GC_SYMBOL)) {
                free(raw_keys);
                ajs_temp_root_restore(vm, rs);
                ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy ownKeys elements must be String or Symbol");
                return false;
            }
            if (ajs_val_is_gc_kind(elem, AJS_GC_STRING)) {
                elem = ajs_val_from_gc(ajs_str_intern(vm, (AjsString *)ajs_val_to_gc(elem)));
            }
            /* Check duplicate keys (ECMA-262 Proxy ownKeys invariant) */
            for (uint32_t j = 0; j < out_n; j++) {
                if (ajs_prop_key_eq(raw_keys[j], elem)) {
                    free(raw_keys);
                    ajs_temp_root_restore(vm, rs);
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy ownKeys returned duplicate keys");
                    return false;
                }
            }
            bool is_sym = ajs_val_is_gc_kind(elem, AJS_GC_SYMBOL);
            if ((is_sym && !include_symbols) || (!is_sym && !include_strings)) continue;
            if (enumerable_only) {
                AjsPropDesc d;
                bool f = false;
                if (!ajs_obj_get_own_desc(vm, obj, elem, &d, &f)) {
                    free(raw_keys);
                    ajs_temp_root_restore(vm, rs);
                    return false;
                }
                if (!f || !(d.flags & AJS_DESC_ENUMERABLE)) continue;
            }
            raw_keys[out_n++] = elem;
        }
        ajs_temp_root_restore(vm, rs);
        *out_keys = raw_keys;
        *out_count = out_n;
        return true;
    }

    if (obj->kind == AJS_OBJ_MODULE_NS) {
        uint32_t cap = obj->u.mod_ns.export_count + 1u;
        AjsValue *keys = (AjsValue *)malloc(cap * sizeof(AjsValue));
        if (!keys) return false;
        uint32_t n = 0;
        if (include_strings) {
            for (uint32_t i = 0; i < obj->u.mod_ns.export_count; i++) {
                keys[n++] = ajs_val_from_gc(obj->u.mod_ns.export_names[i]);
            }
        }
        if (include_symbols && !enumerable_only && vm->well_known_syms[AJS_SYM_TO_STRING_TAG]) {
            keys[n++] = ajs_val_from_gc(vm->well_known_syms[AJS_SYM_TO_STRING_TAG]);
        }
        *out_keys = keys;
        *out_count = n;
        return true;
    }

    /* 1. Collect ascending array index keys */
    uint32_t idx_cap = 16u, idx_count = 0;
    AjsIdxPair *indices = NULL;
    if (include_strings) {
        if (obj->kind == AJS_OBJ_ARRAY) {
            for (uint32_t i = 0; i < obj->u.arr.dense_len; i++) {
                if (obj->u.arr.dense[i] != AJS_VAL_EMPTY) {
                    if (idx_count >= idx_cap) {
                        idx_cap *= 2u;
                        indices = (AjsIdxPair *)realloc(indices, idx_cap * sizeof(AjsIdxPair));
                    } else if (!indices) {
                        indices = (AjsIdxPair *)malloc(idx_cap * sizeof(AjsIdxPair));
                    }
                    AjsString *ks = ajs_number_to_str(vm, (double)i, 10);
                    indices[idx_count].idx = i;
                    indices[idx_count].key = ajs_val_from_gc(ajs_str_intern(vm, ks));
                    idx_count++;
                }
            }
        } else if (obj->kind == AJS_OBJ_STRING_OBJ) {
            AjsString *prim = (AjsString *)ajs_val_to_gc(obj->u.prim_val);
            for (uint32_t i = 0; i < prim->len16; i++) {
                if (idx_count >= idx_cap) {
                    idx_cap *= 2u;
                    indices = (AjsIdxPair *)realloc(indices, idx_cap * sizeof(AjsIdxPair));
                } else if (!indices) {
                    indices = (AjsIdxPair *)malloc(idx_cap * sizeof(AjsIdxPair));
                }
                AjsString *ks = ajs_number_to_str(vm, (double)i, 10);
                indices[idx_count].idx = i;
                indices[idx_count].key = ajs_val_from_gc(ajs_str_intern(vm, ks));
                idx_count++;
            }
        } else if (obj->kind == AJS_OBJ_TYPED_ARRAY) {
            size_t ta_len = 0, byte_off = 0;
            if (ajs_ta_check_valid_len(vm, obj, &ta_len, &byte_off)) {
                for (size_t i = 0; i < ta_len; i++) {
                    if (idx_count >= idx_cap) {
                        idx_cap *= 2u;
                        indices = (AjsIdxPair *)realloc(indices, idx_cap * sizeof(AjsIdxPair));
                    } else if (!indices) {
                        indices = (AjsIdxPair *)malloc(idx_cap * sizeof(AjsIdxPair));
                    }
                    AjsString *ks = ajs_number_to_str(vm, (double)i, 10);
                    indices[idx_count].idx = (uint32_t)i;
                    indices[idx_count].key = ajs_val_from_gc(ajs_str_intern(vm, ks));
                    idx_count++;
                }
            } else {
                vm->has_exception = false;
            }
        }

        for (uint32_t i = 0; i < obj->prop_count; i++) {
            AjsPropSlot *ps = &obj->props[i];
            if (ps->key == AJS_VAL_EMPTY) continue;
            if (enumerable_only && !(ps->flags & AJS_DESC_ENUMERABLE)) continue;
            if (ajs_val_is_gc_kind(ps->key, AJS_GC_STRING)) {
                AjsString *sk = (AjsString *)ajs_val_to_gc(ps->key);
                if (sk->is_array_idx) {
                    if (obj->kind == AJS_OBJ_STRING_OBJ &&
                        sk->array_idx < ((AjsString *)ajs_val_to_gc(obj->u.prim_val))->len16) {
                        continue;
                    }
                    if (idx_count >= idx_cap) {
                        idx_cap *= 2u;
                        indices = (AjsIdxPair *)realloc(indices, idx_cap * sizeof(AjsIdxPair));
                    } else if (!indices) {
                        indices = (AjsIdxPair *)malloc(idx_cap * sizeof(AjsIdxPair));
                    }
                    indices[idx_count].idx = sk->array_idx;
                    indices[idx_count].key = ps->key;
                    idx_count++;
                }
            }
        }
        if (idx_count > 1) {
            qsort(indices, idx_count, sizeof(AjsIdxPair), ajs_cmp_idx_pair);
        }
    }

    size_t max_total = (size_t)idx_count + (size_t)obj->live_props + 2u;
    AjsValue *keys = (AjsValue *)malloc(max_total * sizeof(AjsValue));
    if (!keys) { free(indices); return false; }
    uint32_t n = 0;

    for (uint32_t i = 0; i < idx_count; i++) {
        keys[n++] = indices[i].key;
    }
    free(indices);

    /* 2. Insertion-order non-index string keys */
    if (include_strings) {
        bool has_own_len_slot = false;
        for (uint32_t i = 0; i < obj->prop_count; i++) {
            AjsPropSlot *ps = &obj->props[i];
            if (ps->key == AJS_VAL_EMPTY) continue;
            if (ajs_val_is_gc_kind(ps->key, AJS_GC_STRING)) {
                AjsString *sk = (AjsString *)ajs_val_to_gc(ps->key);
                if (sk->is_array_idx) continue;
                if (ajs_str_eq(sk, vm->str_length)) has_own_len_slot = true;
                if (enumerable_only && !(ps->flags & AJS_DESC_ENUMERABLE)) continue;
                keys[n++] = ps->key;
            }
        }
        if (!enumerable_only && !has_own_len_slot &&
            (obj->kind == AJS_OBJ_ARRAY || obj->kind == AJS_OBJ_STRING_OBJ)) {
            keys[n++] = ajs_val_from_gc(vm->str_length);
        }
    }

    /* 3. Insertion-order symbol keys */
    if (include_symbols) {
        for (uint32_t i = 0; i < obj->prop_count; i++) {
            AjsPropSlot *ps = &obj->props[i];
            if (ps->key == AJS_VAL_EMPTY) continue;
            if (enumerable_only && !(ps->flags & AJS_DESC_ENUMERABLE)) continue;
            if (ajs_val_is_gc_kind(ps->key, AJS_GC_SYMBOL)) {
                keys[n++] = ps->key;
            }
        }
    }

    *out_keys = keys;
    *out_count = n;
    return true;
}

/* ============================================================================
 * Class Private Fields, Methods & Accessors (#name)
 * ============================================================================ */

static int32_t ajs_priv_find(const AjsObject *obj, const AjsSymbol *sym) {
    for (uint32_t i = 0; i < obj->priv_count; i++) {
        if (obj->privs[i].sym == sym) return (int32_t)i;
    }
    return -1;
}

bool ajs_priv_define(AjsVm *vm, AjsObject *obj, AjsSymbol *sym, uint8_t kind, AjsValue val, AjsValue setter) {
    int32_t idx = ajs_priv_find(obj, sym);
    if (idx >= 0) {
        if (kind == AJS_PRIV_ACCESSOR && obj->privs[idx].kind == AJS_PRIV_ACCESSOR) {
            if (val != AJS_VAL_UNDEFINED && obj->privs[idx].value == AJS_VAL_UNDEFINED) {
                obj->privs[idx].value = val;
                return true;
            }
            if (setter != AJS_VAL_UNDEFINED && obj->privs[idx].setter == AJS_VAL_UNDEFINED) {
                obj->privs[idx].setter = setter;
                return true;
            }
        }
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot initialize private member %s twice on the same object",
                           sym->desc ? sym->desc->utf8 : "#private");
        return false;
    }
    if (obj->priv_count >= obj->priv_cap) {
        uint32_t ncap = obj->priv_cap ? obj->priv_cap * 2u : 4u;
        AjsPrivateSlot *np = (AjsPrivateSlot *)realloc(obj->privs, ncap * sizeof(AjsPrivateSlot));
        if (!np) return false;
        obj->privs = np;
        obj->priv_cap = ncap;
    }
    AjsPrivateSlot *ps = &obj->privs[obj->priv_count++];
    ps->sym = sym;
    ps->kind = kind;
    ps->value = val;
    ps->setter = setter;
    return true;
}

AjsValue ajs_priv_get(AjsVm *vm, AjsValue target, AjsSymbol *sym) {
    if (!ajs_val_is_gc_kind(target, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot read private member %s from non-object",
                                  sym->desc ? sym->desc->utf8 : "#private");
    }
    AjsObject *obj = (AjsObject *)ajs_val_to_gc(target);
    int32_t idx = ajs_priv_find(obj, sym);
    if (idx < 0) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot read private member %s from an object whose class did not declare it",
                                  sym->desc ? sym->desc->utf8 : "#private");
    }
    AjsPrivateSlot *ps = &obj->privs[idx];
    if (ps->kind == AJS_PRIV_ACCESSOR) {
        if (ps->value == AJS_VAL_UNDEFINED) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Private member %s was defined without a getter",
                                      sym->desc ? sym->desc->utf8 : "#private");
        }
        return ajs_call_internal(vm, ps->value, target, 0, NULL);
    }
    return ps->value;
}

bool ajs_priv_set(AjsVm *vm, AjsValue target, AjsSymbol *sym, AjsValue val) {
    if (!ajs_val_is_gc_kind(target, AJS_GC_OBJECT)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot write private member %s to non-object",
                           sym->desc ? sym->desc->utf8 : "#private");
        return false;
    }
    AjsObject *obj = (AjsObject *)ajs_val_to_gc(target);
    int32_t idx = ajs_priv_find(obj, sym);
    if (idx < 0) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot write private member %s to an object whose class did not declare it",
                           sym->desc ? sym->desc->utf8 : "#private");
        return false;
    }
    AjsPrivateSlot *ps = &obj->privs[idx];
    if (ps->kind == AJS_PRIV_METHOD) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Private method '%s' is not writable",
                           sym->desc ? sym->desc->utf8 : "#private");
        return false;
    }
    if (ps->kind == AJS_PRIV_ACCESSOR) {
        if (ps->setter == AJS_VAL_UNDEFINED) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Private member %s was defined without a setter",
                               sym->desc ? sym->desc->utf8 : "#private");
            return false;
        }
        ajs_call_internal(vm, ps->setter, target, 1, &val);
        return !vm->has_exception;
    }
    ps->value = val;
    return true;
}

bool ajs_priv_has(AjsVm *vm, AjsValue target, AjsSymbol *sym, bool *out_has) {
    if (!ajs_val_is_gc_kind(target, AJS_GC_OBJECT)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot use 'in' operator to search for '%s' in non-object",
                           sym->desc ? sym->desc->utf8 : "#private");
        return false;
    }
    AjsObject *obj = (AjsObject *)ajs_val_to_gc(target);
    *out_has = (ajs_priv_find(obj, sym) >= 0);
    return true;
}

/* ============================================================================
 * Native Function / Method / Accessor Registration Helpers
 * ============================================================================ */

static AjsValue ajs_make_native_fn_internal(AjsVm *vm, AjsRealm *realm, AjsNativeFn fn,
                                            const char *name, int length, int32_t magic, void *userdata) {
    AjsObject *fobj = ajs_obj_new(vm, realm, AJS_OBJ_FUNCTION, realm->function_proto);
    if (!fobj) return AJS_VAL_UNDEFINED;
    fobj->u.fn.fn_kind = AJS_FN_NATIVE;
    fobj->u.fn.is_constructor = false;
    fobj->u.fn.native_fn = fn;
    fobj->u.fn.native_userdata = userdata;
    fobj->u.fn.native_magic = magic;

    AjsValue fv = ajs_val_from_gc(fobj);
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, fv);

    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_CONFIGURABLE | AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE |
              AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
    d.value = ajs_val_number((double)(length < 0 ? 0 : length));
    ajs_obj_define_own_prop(vm, fobj, ajs_val_from_gc(vm->str_length), &d, false);

    d.value = ajs_val_from_gc(ajs_str_from_cstr(vm, name ? name : "", true));
    ajs_obj_define_own_prop(vm, fobj, ajs_val_from_gc(vm->str_name), &d, false);

    ajs_temp_root_restore(vm, rs);
    return fv;
}

void ajs_def_native_method(AjsVm *vm, AjsRealm *realm, AjsValue obj, const char *name,
                           AjsNativeFn fn, int length, uint32_t flags, int32_t magic) {
    if (!ajs_val_is_gc_kind(obj, AJS_GC_OBJECT)) return;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, obj);
    AjsValue fv = ajs_make_native_fn_internal(vm, realm, fn, name, length, magic, NULL);
    ajs_temp_root_push(vm, fv);
    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
    if (flags & AJS_PROP_WRITABLE) d.flags |= AJS_DESC_WRITABLE;
    if (flags & AJS_PROP_ENUMERABLE) d.flags |= AJS_DESC_ENUMERABLE;
    if (flags & AJS_PROP_CONFIGURABLE) d.flags |= AJS_DESC_CONFIGURABLE;
    d.value = fv;
    AjsString *ks = ajs_str_from_cstr(vm, name, true);
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(obj), ajs_val_from_gc(ks), &d, false);
    ajs_temp_root_restore(vm, rs);
}

void ajs_def_native_sym_method(AjsVm *vm, AjsRealm *realm, AjsValue obj, AjsSymbol *sym,
                               const char *fn_name, AjsNativeFn fn, int length, uint32_t flags) {
    if (!ajs_val_is_gc_kind(obj, AJS_GC_OBJECT) || !sym) return;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, obj);
    AjsValue fv = ajs_make_native_fn_internal(vm, realm, fn, fn_name, length, 0, NULL);
    ajs_temp_root_push(vm, fv);
    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
    if (flags & AJS_PROP_WRITABLE) d.flags |= AJS_DESC_WRITABLE;
    if (flags & AJS_PROP_ENUMERABLE) d.flags |= AJS_DESC_ENUMERABLE;
    if (flags & AJS_PROP_CONFIGURABLE) d.flags |= AJS_DESC_CONFIGURABLE;
    d.value = fv;
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(obj), ajs_val_from_gc(sym), &d, false);
    ajs_temp_root_restore(vm, rs);
}

void ajs_def_native_accessor(AjsVm *vm, AjsRealm *realm, AjsValue obj, const char *name,
                             AjsNativeFn getter, AjsNativeFn setter, uint32_t flags) {
    if (!ajs_val_is_gc_kind(obj, AJS_GC_OBJECT)) return;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, obj);
    char gname[128], sname[128];
    snprintf(gname, sizeof(gname), "get %s", name);
    snprintf(sname, sizeof(sname), "set %s", name);
    AjsValue gv = getter ? ajs_make_native_fn_internal(vm, realm, getter, gname, 0, 0, NULL) : AJS_VAL_UNDEFINED;
    ajs_temp_root_push(vm, gv);
    AjsValue sv = setter ? ajs_make_native_fn_internal(vm, realm, setter, sname, 1, 0, NULL) : AJS_VAL_UNDEFINED;
    ajs_temp_root_push(vm, sv);
    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_GET | AJS_DESC_HAS_SET |
              AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
    if (flags & AJS_PROP_ENUMERABLE) d.flags |= AJS_DESC_ENUMERABLE;
    if (flags & AJS_PROP_CONFIGURABLE) d.flags |= AJS_DESC_CONFIGURABLE;
    d.getter = gv;
    d.setter = sv;
    AjsString *ks = ajs_str_from_cstr(vm, name, true);
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(obj), ajs_val_from_gc(ks), &d, false);
    ajs_temp_root_restore(vm, rs);
}

AjsValue ajs_make_iter_result(AjsVm *vm, AjsValue value, bool done) {
    AjsRealm *r = vm->active_realm ? vm->active_realm : vm->main_realm;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, value);
    AjsObject *o = ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto);
    if (!o) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsValue ov = ajs_val_from_gc(o);
    ajs_temp_root_push(vm, ov);
    ajs_obj_insert_own_slot(o, ajs_val_from_gc(vm->str_value),
                            AJS_DESC_WRITABLE | AJS_DESC_ENUMERABLE | AJS_DESC_CONFIGURABLE, value, AJS_VAL_UNDEFINED);
    ajs_obj_insert_own_slot(o, ajs_val_from_gc(vm->str_done),
                            AJS_DESC_WRITABLE | AJS_DESC_ENUMERABLE | AJS_DESC_CONFIGURABLE,
                            done ? AJS_VAL_TRUE : AJS_VAL_FALSE, AJS_VAL_UNDEFINED);
    ajs_temp_root_restore(vm, rs);
    return ov;
}
