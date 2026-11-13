/*
 * ArcradientJS 1.0 (Clean-Room Pure C11 ECMAScript 2026 Virtual Machine)
 * js/builtins/ajs_collections.c - Map (incl. Map.groupBy), Set (incl. ES2025/2026
 * Set Methods), WeakMap, WeakSet, WeakRef & FinalizationRegistry
 */
#include "../vm/ajs_internal.h"

#define AJS_METHOD_FLAGS (AJS_PROP_WRITABLE | AJS_PROP_CONFIGURABLE)

static AjsValue col_make_fn(AjsVm *vm, AjsRealm *r, AjsNativeFn fn, const char *name,
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

static void col_def_prop(AjsVm *vm, AjsValue obj, const char *name, AjsValue val, uint32_t desc_flags) {
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

static void col_def_tag(AjsVm *vm, AjsValue obj, const char *tag) {
    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
              AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    d.value = ajs_val_from_gc(ajs_str_from_cstr(vm, tag, true));
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(obj),
                            ajs_val_from_gc(vm->well_known_syms[AJS_SYM_TO_STRING_TAG]), &d, false);
}

static AjsValue col_get_proto_from_ctor(AjsVm *vm, AjsValue nt, AjsValue fallback) {
    if (!ajs_val_is_gc_kind(nt, AJS_GC_OBJECT)) return fallback;
    AjsValue p = ajs_obj_get(vm, nt, ajs_val_from_gc(vm->str_prototype), nt);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    return ajs_val_is_gc_kind(p, AJS_GC_OBJECT) ? p : fallback;
}

/* ============================================================================
 * 1. Deterministic SameValueZero Hash & Core Hash Table Operations
 * ============================================================================ */

static AjsValue canonicalize_map_key(AjsValue k) {
    if (ajs_val_is_number(k)) {
        double d = ajs_val_to_number_unchecked(k);
        if (d == 0.0) return ajs_val_number(0.0); /* -0 -> +0 */
        if (isnan(d)) return AJS_VAL_NAN;
    }
    return k;
}

uint32_t ajs_map_hash_value(AjsValue v) {
    v = canonicalize_map_key(v);
    if (ajs_val_is_ptr(v)) {
        AjsGcHeader *hdr = ajs_val_to_gc(v);
        if (hdr->gc_kind == AJS_GC_STRING) {
            return ((const AjsString *)hdr)->hash;
        }
        if (hdr->gc_kind == AJS_GC_BIGINT) {
            const AjsBigInt *bi = (const AjsBigInt *)hdr;
            uint32_t h = 2166136261u ^ (uint32_t)(bi->sign + 2);
            for (uint32_t i = 0; i < bi->len; i++) {
                h = (h ^ bi->limbs[i]) * 16777619u;
            }
            return h;
        }
    }
    uint64_t x = v;
    x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
    x = x ^ (x >> 31);
    return (uint32_t)x;
}

static bool col_rehash(AjsCollectionData *c, uint32_t new_bcap) {
    int32_t *nb = (int32_t *)malloc((size_t)new_bcap * sizeof(int32_t));
    if (!nb) return false;
    for (uint32_t i = 0; i < new_bcap; i++) nb[i] = -1;
    uint32_t mask = new_bcap - 1u;
    for (uint32_t i = 0; i < c->entries_len; i++) {
        if (c->entries[i].key != AJS_VAL_EMPTY) {
            uint32_t b = c->entries[i].hash & mask;
            c->entries[i].bucket_next = nb[b];
            nb[b] = (int32_t)i;
        } else {
            c->entries[i].bucket_next = -1;
        }
    }
    free(c->buckets);
    c->buckets = nb;
    c->bucket_cap = new_bcap;
    return true;
}

static int32_t col_find_entry(const AjsCollectionData *c, AjsValue key, uint32_t hash) {
    if (c->bucket_cap == 0 || !c->buckets) return -1;
    int32_t idx = c->buckets[hash & (c->bucket_cap - 1u)];
    while (idx >= 0) {
        const AjsMapEntry *e = &c->entries[idx];
        if (e->key != AJS_VAL_EMPTY && e->hash == hash && ajs_same_value_zero(e->key, key)) {
            return idx;
        }
        idx = e->bucket_next;
    }
    return -1;
}

static bool col_put(AjsVm *vm, AjsCollectionData *c, AjsValue key, AjsValue val) {
    key = canonicalize_map_key(key);
    uint32_t h = ajs_map_hash_value(key);
    int32_t ex = col_find_entry(c, key, h);
    if (ex >= 0) {
        c->entries[ex].val = val;
        return true;
    }
    if (c->bucket_cap == 0) {
        if (!col_rehash(c, 16u)) { ajs_throw(vm, vm->oom_error); return false; }
    } else if ((c->entries_len + 1u) * 4u >= c->bucket_cap * 3u) {
        if (!col_rehash(c, c->bucket_cap * 2u)) { ajs_throw(vm, vm->oom_error); return false; }
    }
    if (c->entries_len >= c->entries_cap) {
        uint32_t ncap = c->entries_cap ? c->entries_cap * 2u : 8u;
        AjsMapEntry *ne = (AjsMapEntry *)realloc(c->entries, (size_t)ncap * sizeof(AjsMapEntry));
        if (!ne) { ajs_throw(vm, vm->oom_error); return false; }
        c->entries = ne;
        c->entries_cap = ncap;
    }
    uint32_t idx = c->entries_len++;
    uint32_t b = h & (c->bucket_cap - 1u);
    c->entries[idx].key = key;
    c->entries[idx].val = val;
    c->entries[idx].hash = h;
    c->entries[idx].bucket_next = c->buckets[b];
    c->buckets[b] = (int32_t)idx;
    c->live_count++;
    return true;
}

static bool col_delete(AjsCollectionData *c, AjsValue key) {
    key = canonicalize_map_key(key);
    uint32_t h = ajs_map_hash_value(key);
    if (c->bucket_cap == 0 || !c->buckets) return false;
    uint32_t b = h & (c->bucket_cap - 1u);
    int32_t *prev = &c->buckets[b];
    int32_t cur = *prev;
    while (cur >= 0) {
        AjsMapEntry *e = &c->entries[cur];
        if (e->key != AJS_VAL_EMPTY && e->hash == h && ajs_same_value_zero(e->key, key)) {
            *prev = e->bucket_next;
            e->key = AJS_VAL_EMPTY;
            e->val = AJS_VAL_UNDEFINED;
            e->bucket_next = -1;
            if (c->live_count > 0) c->live_count--;
            return true;
        }
        prev = &e->bucket_next;
        cur = *prev;
    }
    return false;
}

static void col_clear(AjsCollectionData *c) {
    for (uint32_t i = 0; i < c->entries_len; i++) {
        c->entries[i].key = AJS_VAL_EMPTY;
        c->entries[i].val = AJS_VAL_UNDEFINED;
        c->entries[i].bucket_next = -1;
    }
    for (uint32_t i = 0; i < c->bucket_cap; i++) c->buckets[i] = -1;
    c->live_count = 0;
}

static bool can_be_held_weakly(AjsValue v) {
    if (ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return true;
    if (ajs_val_is_gc_kind(v, AJS_GC_SYMBOL)) {
        return !((const AjsSymbol *)ajs_val_to_gc(v))->is_registered;
    }
    return false;
}

static AjsObject *require_col_kind(AjsVm *vm, AjsValue this_val, AjsObjKind want, const char *name) {
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT) || ((AjsObject *)ajs_val_to_gc(this_val))->kind != want) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "%s method called on incompatible receiver", name);
        return NULL;
    }
    return (AjsObject *)ajs_val_to_gc(this_val);
}

static bool populate_from_iterable(AjsVm *vm, AjsValue target, AjsValue adder, AjsValue iterable, bool pair_mode) {
    if (ajs_is_nullish(iterable)) return true;
    if (!ajs_is_callable(vm, adder)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Adder is not a function");
        return false;
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue it_fn = ajs_obj_get(vm, iterable, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), iterable);
    if (vm->has_exception || !ajs_is_callable(vm, it_fn)) {
        if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object is not iterable");
        return false;
    }
    AjsValue iter = ajs_call_internal(vm, it_fn, iterable, 0, NULL);
    if (vm->has_exception || !ajs_is_object(vm, iter)) {
        if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator must be an object");
        return false;
    }
    ajs_temp_root_push(vm, iter);
    AjsValue next_fn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_next), iter);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
    ajs_temp_root_push(vm, next_fn);

    for (;;) {
        size_t ls = ajs_temp_root_save(vm);
        AjsValue step = ajs_call_internal(vm, next_fn, iter, 0, NULL);
        if (vm->has_exception || !ajs_is_object(vm, step)) {
            ajs_temp_root_restore(vm, rs);
            if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator result is not an object");
            return false;
        }
        ajs_temp_root_push(vm, step);
        AjsValue dn = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_done), step);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
        if (ajs_to_bool(vm, dn)) { ajs_temp_root_restore(vm, rs); return true; }
        AjsValue item = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
        ajs_temp_root_push(vm, item);

        if (pair_mode) {
            if (!ajs_val_is_gc_kind(item, AJS_GC_OBJECT)) {
                ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator value is not an entry object");
                goto close_abrupt;
            }
            AjsValue k = ajs_get_prop_index(vm, item, 0);
            if (vm->has_exception) goto close_abrupt;
            ajs_temp_root_push(vm, k);
            AjsValue v = ajs_get_prop_index(vm, item, 1);
            if (vm->has_exception) goto close_abrupt;
            ajs_temp_root_push(vm, v);
            AjsValue cargs[2] = { k, v };
            ajs_call_internal(vm, adder, target, 2, cargs);
            if (vm->has_exception) goto close_abrupt;
        } else {
            ajs_call_internal(vm, adder, target, 1, &item);
            if (vm->has_exception) goto close_abrupt;
        }
        ajs_temp_root_restore(vm, ls);
        continue;
    close_abrupt: {
            AjsValue saved = vm->exception;
            ajs_temp_root_push(vm, saved);
            ajs_clear_exception(vm);
            AjsValue rfn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_return), iter);
            if (!vm->has_exception && ajs_is_callable(vm, rfn)) ajs_call_internal(vm, rfn, iter, 0, NULL);
            ajs_throw(vm, saved);
            ajs_temp_root_restore(vm, rs);
            return false;
        }
    }
}

/* ============================================================================
 * 2. Map & Map.groupBy
 * ============================================================================ */

static AjsValue js_map_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt == AJS_VAL_UNDEFINED) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Constructor Map requires 'new'");
    AjsValue proto = col_get_proto_from_ctor(vm, nt, vm->active_realm->map_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, proto);
    AjsObject *m = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_MAP, proto);
    if (!m) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsValue mv = ajs_val_from_gc(m);
    ajs_temp_root_push(vm, mv);
    if (argc > 0 && !ajs_is_nullish(argv[0])) {
        AjsValue adder = ajs_obj_get(vm, mv, ajs_val_from_gc(ajs_str_from_cstr(vm, "set", true)), mv);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, adder);
        if (!populate_from_iterable(vm, mv, adder, argv[0], true)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    ajs_temp_root_restore(vm, rs);
    return mv;
}

static AjsValue js_map_get(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *m = require_col_kind(vm, this_val, AJS_OBJ_MAP, "Map.prototype.get");
    if (!m) return AJS_VAL_EXCEPTION;
    AjsValue k = canonicalize_map_key(argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    int32_t idx = col_find_entry(&m->u.col, k, ajs_map_hash_value(k));
    return idx >= 0 ? m->u.col.entries[idx].val : AJS_VAL_UNDEFINED;
}
static AjsValue js_map_set(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *m = require_col_kind(vm, this_val, AJS_OBJ_MAP, "Map.prototype.set");
    if (!m) return AJS_VAL_EXCEPTION;
    AjsValue k = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue v = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    return col_put(vm, &m->u.col, k, v) ? this_val : AJS_VAL_EXCEPTION;
}
static AjsValue js_map_has(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *m = require_col_kind(vm, this_val, AJS_OBJ_MAP, "Map.prototype.has");
    if (!m) return AJS_VAL_EXCEPTION;
    AjsValue k = canonicalize_map_key(argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    return ajs_bool(col_find_entry(&m->u.col, k, ajs_map_hash_value(k)) >= 0);
}
static AjsValue js_map_delete(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *m = require_col_kind(vm, this_val, AJS_OBJ_MAP, "Map.prototype.delete");
    if (!m) return AJS_VAL_EXCEPTION;
    return ajs_bool(col_delete(&m->u.col, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED));
}
static AjsValue js_map_clear(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    AjsObject *m = require_col_kind(vm, this_val, AJS_OBJ_MAP, "Map.prototype.clear");
    if (!m) return AJS_VAL_EXCEPTION;
    col_clear(&m->u.col);
    return AJS_VAL_UNDEFINED;
}
static AjsValue js_map_size(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    AjsObject *m = require_col_kind(vm, this_val, AJS_OBJ_MAP, "get Map.prototype.size");
    return m ? ajs_val_number((double)m->u.col.live_count) : AJS_VAL_EXCEPTION;
}
static AjsValue js_map_forEach(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *m = require_col_kind(vm, this_val, AJS_OBJ_MAP, "Map.prototype.forEach");
    if (!m) return AJS_VAL_EXCEPTION;
    AjsValue cb = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_is_callable(vm, cb)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Callback must be callable");
    AjsValue this_arg = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    for (uint32_t i = 0; i < m->u.col.entries_len; i++) {
        if (m->u.col.entries[i].key == AJS_VAL_EMPTY) continue;
        AjsValue cargs[3] = { m->u.col.entries[i].val, m->u.col.entries[i].key, this_val };
        ajs_call_internal(vm, cb, this_arg, 3, cargs);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
    }
    return AJS_VAL_UNDEFINED;
}

/* ES2024+ Map.groupBy(items, callbackfn) */
static AjsValue js_map_groupBy(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue items = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue cb = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    if (ajs_is_nullish(items)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Map.groupBy called on null or undefined");
    if (!ajs_is_callable(vm, cb)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Callback must be callable");

    size_t rs = ajs_temp_root_save(vm);
    AjsObject *m = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_MAP, vm->active_realm->map_proto);
    AjsValue mv = ajs_val_from_gc(m);
    ajs_temp_root_push(vm, mv);

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
        AjsValue val = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, val);

        AjsValue cargs[2] = { val, ajs_val_number((double)k_idx++) };
        AjsValue gkey = ajs_call_internal(vm, cb, AJS_VAL_UNDEFINED, 2, cargs);
        if (vm->has_exception) {
            AjsValue saved = vm->exception;
            ajs_temp_root_push(vm, saved);
            ajs_clear_exception(vm);
            AjsValue rfn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_return), iter);
            if (!vm->has_exception && ajs_is_callable(vm, rfn)) ajs_call_internal(vm, rfn, iter, 0, NULL);
            ajs_throw(vm, saved);
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
        ajs_temp_root_push(vm, gkey);
        gkey = canonicalize_map_key(gkey);
        int32_t eidx = col_find_entry(&m->u.col, gkey, ajs_map_hash_value(gkey));
        AjsValue bucket;
        if (eidx < 0) {
            bucket = ajs_new_array(vm, 0);
            ajs_temp_root_push(vm, bucket);
            col_put(vm, &m->u.col, gkey, bucket);
        } else {
            bucket = m->u.col.entries[eidx].val;
        }
        AjsObject *barr = (AjsObject *)ajs_val_to_gc(bucket);
        ajs_set_prop_index(vm, bucket, barr->u.arr.length, val);
        ajs_temp_root_restore(vm, ls);
    }
    ajs_temp_root_restore(vm, rs);
    return mv;
}

/* Map & Set Iterators */
static AjsValue js_col_iter_create(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    int32_t kind = vm->call_stack[vm->call_depth - 1u].native_fn_obj->u.fn.native_magic;
    bool is_set = (kind == AJS_ITER_SET_VALUES || kind == AJS_ITER_SET_ENTRIES);
    AjsObject *col = require_col_kind(vm, this_val, is_set ? AJS_OBJ_SET : AJS_OBJ_MAP,
                                      is_set ? "Set.prototype" : "Map.prototype");
    if (!col) return AJS_VAL_EXCEPTION;
    AjsObject *it = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ITERATOR,
                                is_set ? vm->active_realm->set_iter_proto : vm->active_realm->map_iter_proto);
    if (!it) return AJS_VAL_EXCEPTION;
    it->u.iter.kind = (AjsIterKind)kind;
    it->u.iter.target = this_val;
    it->u.iter.index = 0;
    it->u.iter.done = false;
    return ajs_val_from_gc(it);
}

static AjsValue js_col_iter_next(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    int32_t want_set = vm->call_stack[vm->call_depth - 1u].native_fn_obj->u.fn.native_magic;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator.prototype.next incompatible receiver");
    }
    AjsObject *it = (AjsObject *)ajs_val_to_gc(this_val);
    if (it->kind != AJS_OBJ_ITERATOR) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Incompatible iterator receiver");
    bool is_set_it = (it->u.iter.kind == AJS_ITER_SET_VALUES || it->u.iter.kind == AJS_ITER_SET_ENTRIES);
    bool is_map_it = (it->u.iter.kind >= AJS_ITER_MAP_KEYS && it->u.iter.kind <= AJS_ITER_MAP_ENTRIES);
    if ((want_set && !is_set_it) || (!want_set && !is_map_it)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Incompatible iterator receiver");
    }
    if (it->u.iter.done || !ajs_val_is_gc_kind(it->u.iter.target, AJS_GC_OBJECT)) {
        return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
    }
    AjsObject *col = (AjsObject *)ajs_val_to_gc(it->u.iter.target);
    while (it->u.iter.index < col->u.col.entries_len) {
        uint32_t i = it->u.iter.index++;
        AjsMapEntry *e = &col->u.col.entries[i];
        if (e->key == AJS_VAL_EMPTY) continue;
        if (it->u.iter.kind == AJS_ITER_MAP_KEYS || it->u.iter.kind == AJS_ITER_SET_VALUES) {
            return ajs_make_iter_result(vm, e->key, false);
        }
        if (it->u.iter.kind == AJS_ITER_MAP_VALUES) {
            return ajs_make_iter_result(vm, e->val, false);
        }
        size_t rs = ajs_temp_root_save(vm);
        AjsValue pair = ajs_new_array(vm, 2);
        ajs_temp_root_push(vm, pair);
        ajs_set_prop_index(vm, pair, 0, e->key);
        ajs_set_prop_index(vm, pair, 1, is_set_it ? e->key : e->val);
        AjsValue res = ajs_make_iter_result(vm, pair, false);
        ajs_temp_root_restore(vm, rs);
        return res;
    }
    it->u.iter.done = true;
    it->u.iter.target = AJS_VAL_UNDEFINED;
    return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
}
/* ============================================================================
 * 3. Set & ES2025/2026 Set Methods
 * ============================================================================ */

static AjsValue js_set_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt == AJS_VAL_UNDEFINED) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Constructor Set requires 'new'");
    AjsValue proto = col_get_proto_from_ctor(vm, nt, vm->active_realm->set_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, proto);
    AjsObject *s = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_SET, proto);
    if (!s) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    s->u.col.is_set = true;
    AjsValue sv = ajs_val_from_gc(s);
    ajs_temp_root_push(vm, sv);
    if (argc > 0 && !ajs_is_nullish(argv[0])) {
        AjsValue adder = ajs_obj_get(vm, sv, ajs_val_from_gc(ajs_str_from_cstr(vm, "add", true)), sv);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, adder);
        if (!populate_from_iterable(vm, sv, adder, argv[0], false)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    ajs_temp_root_restore(vm, rs);
    return sv;
}

static AjsValue js_set_add(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *s = require_col_kind(vm, this_val, AJS_OBJ_SET, "Set.prototype.add");
    if (!s) return AJS_VAL_EXCEPTION;
    AjsValue v = canonicalize_map_key(argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    return col_put(vm, &s->u.col, v, v) ? this_val : AJS_VAL_EXCEPTION;
}
static AjsValue js_set_has(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *s = require_col_kind(vm, this_val, AJS_OBJ_SET, "Set.prototype.has");
    if (!s) return AJS_VAL_EXCEPTION;
    AjsValue v = canonicalize_map_key(argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    return ajs_bool(col_find_entry(&s->u.col, v, ajs_map_hash_value(v)) >= 0);
}
static AjsValue js_set_delete(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *s = require_col_kind(vm, this_val, AJS_OBJ_SET, "Set.prototype.delete");
    if (!s) return AJS_VAL_EXCEPTION;
    return ajs_bool(col_delete(&s->u.col, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED));
}
static AjsValue js_set_clear(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    AjsObject *s = require_col_kind(vm, this_val, AJS_OBJ_SET, "Set.prototype.clear");
    if (!s) return AJS_VAL_EXCEPTION;
    col_clear(&s->u.col);
    return AJS_VAL_UNDEFINED;
}
static AjsValue js_set_size(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    AjsObject *s = require_col_kind(vm, this_val, AJS_OBJ_SET, "get Set.prototype.size");
    return s ? ajs_val_number((double)s->u.col.live_count) : AJS_VAL_EXCEPTION;
}
static AjsValue js_set_forEach(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *s = require_col_kind(vm, this_val, AJS_OBJ_SET, "Set.prototype.forEach");
    if (!s) return AJS_VAL_EXCEPTION;
    AjsValue cb = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_is_callable(vm, cb)) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Callback must be callable");
    AjsValue this_arg = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    for (uint32_t i = 0; i < s->u.col.entries_len; i++) {
        if (s->u.col.entries[i].key == AJS_VAL_EMPTY) continue;
        AjsValue kv = s->u.col.entries[i].key;
        AjsValue cargs[3] = { kv, kv, this_val };
        ajs_call_internal(vm, cb, this_arg, 3, cargs);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
    }
    return AJS_VAL_UNDEFINED;
}

/* ES2025/2026 GetSetRecord(obj) */
typedef struct AjsSetRecord {
    AjsValue set_obj;
    double   size;
    AjsValue has_fn;
    AjsValue keys_fn;
} AjsSetRecord;

static bool get_set_record(AjsVm *vm, AjsValue obj, AjsSetRecord *out) {
    if (!ajs_is_object(vm, obj)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Set operation argument must be an object");
        return false;
    }
    AjsValue raw_sz = ajs_obj_get(vm, obj, ajs_val_from_gc(ajs_str_from_cstr(vm, "size", true)), obj);
    if (vm->has_exception) return false;
    double num_sz = 0.0;
    if (!ajs_to_number(vm, raw_sz, &num_sz)) return false;
    if (isnan(num_sz)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Set-like object .size cannot be NaN");
        return false;
    }
    double int_sz = ajs_double_to_integer_or_inf(num_sz);
    if (int_sz < 0.0) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Set-like object .size must be non-negative");
        return false;
    }
    AjsValue has_fn = ajs_obj_get(vm, obj, ajs_val_from_gc(ajs_str_from_cstr(vm, "has", true)), obj);
    if (vm->has_exception) return false;
    if (!ajs_is_callable(vm, has_fn)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Set-like object .has must be callable");
        return false;
    }
    ajs_temp_root_push(vm, has_fn);
    AjsValue keys_fn = ajs_obj_get(vm, obj, ajs_val_from_gc(ajs_str_from_cstr(vm, "keys", true)), obj);
    if (vm->has_exception) return false;
    if (!ajs_is_callable(vm, keys_fn)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Set-like object .keys must be callable");
        return false;
    }
    ajs_temp_root_push(vm, keys_fn);
    out->set_obj = obj;
    out->size = int_sz;
    out->has_fn = has_fn;
    out->keys_fn = keys_fn;
    return true;
}

static bool set_rec_open_keys(AjsVm *vm, const AjsSetRecord *rec, AjsValue *out_iter, AjsValue *out_next) {
    AjsValue it = ajs_call_internal(vm, rec->keys_fn, rec->set_obj, 0, NULL);
    if (vm->has_exception) return false;
    if (!ajs_is_object(vm, it)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Set-like .keys() must return an iterator object");
        return false;
    }
    ajs_temp_root_push(vm, it);
    AjsValue nfn = ajs_obj_get(vm, it, ajs_val_from_gc(vm->str_next), it);
    if (vm->has_exception) return false;
    if (!ajs_is_callable(vm, nfn)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator .next is not callable");
        return false;
    }
    ajs_temp_root_push(vm, nfn);
    *out_iter = it;
    *out_next = nfn;
    return true;
}

static void set_iter_close_normal(AjsVm *vm, AjsValue iter) {
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, iter);
    AjsValue rfn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_return), iter);
    if (!vm->has_exception && !ajs_is_nullish(rfn)) {
        if (!ajs_is_callable(vm, rfn)) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator .return is not callable");
        } else {
            AjsValue rres = ajs_call_internal(vm, rfn, iter, 0, NULL);
            if (!vm->has_exception && !ajs_is_object(vm, rres)) {
                ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator .return() result is not an object");
            }
        }
    }
    ajs_temp_root_restore(vm, rs);
}

static AjsObject *clone_set_entries(AjsVm *vm, AjsObject *src) {
    AjsObject *res = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_SET, vm->active_realm->set_proto);
    if (!res) return NULL;
    res->u.col.is_set = true;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(res));
    for (uint32_t i = 0; i < src->u.col.entries_len; i++) {
        if (src->u.col.entries[i].key != AJS_VAL_EMPTY) {
            AjsValue k = src->u.col.entries[i].key;
            if (!col_put(vm, &res->u.col, k, k)) {
                ajs_temp_root_restore(vm, rs);
                return NULL;
            }
        }
    }
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_set_union(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *s = require_col_kind(vm, this_val, AJS_OBJ_SET, "Set.prototype.union");
    if (!s) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsSetRecord rec;
    if (!get_set_record(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &rec)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsValue iter, next_fn;
    if (!set_rec_open_keys(vm, &rec, &iter, &next_fn)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsObject *res = clone_set_entries(vm, s);
    if (!res) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsValue rv = ajs_val_from_gc(res);
    ajs_temp_root_push(vm, rv);

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
        AjsValue v = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        v = canonicalize_map_key(v);
        if (!col_put(vm, &res->u.col, v, v)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_restore(vm, ls);
    }
    ajs_temp_root_restore(vm, rs);
    return rv;
}

static AjsValue js_set_intersection(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *s = require_col_kind(vm, this_val, AJS_OBJ_SET, "Set.prototype.intersection");
    if (!s) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsSetRecord rec;
    if (!get_set_record(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &rec)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsObject *res = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_SET, vm->active_realm->set_proto);
    if (!res) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    res->u.col.is_set = true;
    AjsValue rv = ajs_val_from_gc(res);
    ajs_temp_root_push(vm, rv);

    if ((double)s->u.col.live_count <= rec.size) {
        for (uint32_t i = 0; i < s->u.col.entries_len; i++) {
            AjsValue e = s->u.col.entries[i].key;
            if (e == AJS_VAL_EMPTY) continue;
            size_t ls = ajs_temp_root_save(vm);
            ajs_temp_root_push(vm, e);
            AjsValue in_other = ajs_call_internal(vm, rec.has_fn, rec.set_obj, 1, &e);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            if (ajs_to_bool(vm, in_other)) {
                if (!col_put(vm, &res->u.col, e, e)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            }
            ajs_temp_root_restore(vm, ls);
        }
    } else {
        AjsValue iter, next_fn;
        if (!set_rec_open_keys(vm, &rec, &iter, &next_fn)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
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
            AjsValue v = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            v = canonicalize_map_key(v);
            if (col_find_entry(&s->u.col, v, ajs_map_hash_value(v)) >= 0) {
                if (!col_put(vm, &res->u.col, v, v)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            }
            ajs_temp_root_restore(vm, ls);
        }
    }
    ajs_temp_root_restore(vm, rs);
    return rv;
}

static AjsValue js_set_difference(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *s = require_col_kind(vm, this_val, AJS_OBJ_SET, "Set.prototype.difference");
    if (!s) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsSetRecord rec;
    if (!get_set_record(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &rec)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsObject *res = clone_set_entries(vm, s);
    if (!res) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsValue rv = ajs_val_from_gc(res);
    ajs_temp_root_push(vm, rv);

    if ((double)s->u.col.live_count <= rec.size) {
        uint32_t init_len = res->u.col.entries_len;
        for (uint32_t i = 0; i < init_len; i++) {
            AjsValue e = res->u.col.entries[i].key;
            if (e == AJS_VAL_EMPTY) continue;
            size_t ls = ajs_temp_root_save(vm);
            ajs_temp_root_push(vm, e);
            AjsValue in_other = ajs_call_internal(vm, rec.has_fn, rec.set_obj, 1, &e);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            if (ajs_to_bool(vm, in_other)) col_delete(&res->u.col, e);
            ajs_temp_root_restore(vm, ls);
        }
    } else {
        AjsValue iter, next_fn;
        if (!set_rec_open_keys(vm, &rec, &iter, &next_fn)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
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
            AjsValue v = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            col_delete(&res->u.col, v);
            ajs_temp_root_restore(vm, ls);
        }
    }
    ajs_temp_root_restore(vm, rs);
    return rv;
}

static AjsValue js_set_symmetricDifference(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *s = require_col_kind(vm, this_val, AJS_OBJ_SET, "Set.prototype.symmetricDifference");
    if (!s) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsSetRecord rec;
    if (!get_set_record(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &rec)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsValue iter, next_fn;
    if (!set_rec_open_keys(vm, &rec, &iter, &next_fn)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsObject *res = clone_set_entries(vm, s);
    if (!res) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsValue rv = ajs_val_from_gc(res);
    ajs_temp_root_push(vm, rv);

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
        AjsValue v = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        v = canonicalize_map_key(v);
        uint32_t h = ajs_map_hash_value(v);
        bool in_this = (col_find_entry(&s->u.col, v, h) >= 0);
        bool in_res  = (col_find_entry(&res->u.col, v, h) >= 0);
        if (in_this) {
            if (in_res) col_delete(&res->u.col, v);
        } else {
            if (!in_res) {
                if (!col_put(vm, &res->u.col, v, v)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            }
        }
        ajs_temp_root_restore(vm, ls);
    }
    ajs_temp_root_restore(vm, rs);
    return rv;
}

static AjsValue js_set_isSubsetOf(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *s = require_col_kind(vm, this_val, AJS_OBJ_SET, "Set.prototype.isSubsetOf");
    if (!s) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsSetRecord rec;
    if (!get_set_record(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &rec)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    if ((double)s->u.col.live_count > rec.size) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_FALSE;
    }
    for (uint32_t i = 0; i < s->u.col.entries_len; i++) {
        AjsValue e = s->u.col.entries[i].key;
        if (e == AJS_VAL_EMPTY) continue;
        size_t ls = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, e);
        AjsValue in_other = ajs_call_internal(vm, rec.has_fn, rec.set_obj, 1, &e);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        bool ok = ajs_to_bool(vm, in_other);
        ajs_temp_root_restore(vm, ls);
        if (!ok) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_FALSE;
        }
    }
    ajs_temp_root_restore(vm, rs);
    return AJS_VAL_TRUE;
}

static AjsValue js_set_isSupersetOf(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *s = require_col_kind(vm, this_val, AJS_OBJ_SET, "Set.prototype.isSupersetOf");
    if (!s) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsSetRecord rec;
    if (!get_set_record(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &rec)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    if ((double)s->u.col.live_count < rec.size) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_FALSE;
    }
    AjsValue iter, next_fn;
    if (!set_rec_open_keys(vm, &rec, &iter, &next_fn)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
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
        AjsValue v = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        v = canonicalize_map_key(v);
        if (col_find_entry(&s->u.col, v, ajs_map_hash_value(v)) < 0) {
            set_iter_close_normal(vm, iter);
            ajs_temp_root_restore(vm, rs);
            return vm->has_exception ? AJS_VAL_EXCEPTION : AJS_VAL_FALSE;
        }
        ajs_temp_root_restore(vm, ls);
    }
    ajs_temp_root_restore(vm, rs);
    return AJS_VAL_TRUE;
}

static AjsValue js_set_isDisjointFrom(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *s = require_col_kind(vm, this_val, AJS_OBJ_SET, "Set.prototype.isDisjointFrom");
    if (!s) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsSetRecord rec;
    if (!get_set_record(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &rec)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    if ((double)s->u.col.live_count <= rec.size) {
        for (uint32_t i = 0; i < s->u.col.entries_len; i++) {
            AjsValue e = s->u.col.entries[i].key;
            if (e == AJS_VAL_EMPTY) continue;
            size_t ls = ajs_temp_root_save(vm);
            ajs_temp_root_push(vm, e);
            AjsValue in_other = ajs_call_internal(vm, rec.has_fn, rec.set_obj, 1, &e);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            bool hit = ajs_to_bool(vm, in_other);
            ajs_temp_root_restore(vm, ls);
            if (hit) {
                ajs_temp_root_restore(vm, rs);
                return AJS_VAL_FALSE;
            }
        }
    } else {
        AjsValue iter, next_fn;
        if (!set_rec_open_keys(vm, &rec, &iter, &next_fn)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
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
            AjsValue v = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            v = canonicalize_map_key(v);
            if (col_find_entry(&s->u.col, v, ajs_map_hash_value(v)) >= 0) {
                set_iter_close_normal(vm, iter);
                ajs_temp_root_restore(vm, rs);
                return vm->has_exception ? AJS_VAL_EXCEPTION : AJS_VAL_FALSE;
            }
            ajs_temp_root_restore(vm, ls);
        }
    }
    ajs_temp_root_restore(vm, rs);
    return AJS_VAL_TRUE;
}

/* ============================================================================
 * 4. WeakMap & WeakSet
 * ============================================================================ */

static AjsValue js_weakmap_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt == AJS_VAL_UNDEFINED) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Constructor WeakMap requires 'new'");
    AjsValue proto = col_get_proto_from_ctor(vm, nt, vm->active_realm->weakmap_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, proto);
    AjsObject *wm = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_WEAKMAP, proto);
    if (!wm) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    wm->u.col.is_weak = true;
    AjsValue wmv = ajs_val_from_gc(wm);
    ajs_temp_root_push(vm, wmv);
    if (argc > 0 && !ajs_is_nullish(argv[0])) {
        AjsValue adder = ajs_obj_get(vm, wmv, ajs_val_from_gc(ajs_str_from_cstr(vm, "set", true)), wmv);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, adder);
        if (!populate_from_iterable(vm, wmv, adder, argv[0], true)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    ajs_temp_root_restore(vm, rs);
    return wmv;
}

static AjsValue js_weakmap_get(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *wm = require_col_kind(vm, this_val, AJS_OBJ_WEAKMAP, "WeakMap.prototype.get");
    if (!wm) return AJS_VAL_EXCEPTION;
    AjsValue k = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!can_be_held_weakly(k)) return AJS_VAL_UNDEFINED;
    int32_t idx = col_find_entry(&wm->u.col, k, ajs_map_hash_value(k));
    return idx >= 0 ? wm->u.col.entries[idx].val : AJS_VAL_UNDEFINED;
}
static AjsValue js_weakmap_set(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *wm = require_col_kind(vm, this_val, AJS_OBJ_WEAKMAP, "WeakMap.prototype.set");
    if (!wm) return AJS_VAL_EXCEPTION;
    AjsValue k = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!can_be_held_weakly(k)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Invalid value used as weak map key");
    }
    AjsValue v = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    return col_put(vm, &wm->u.col, k, v) ? this_val : AJS_VAL_EXCEPTION;
}
static AjsValue js_weakmap_has(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *wm = require_col_kind(vm, this_val, AJS_OBJ_WEAKMAP, "WeakMap.prototype.has");
    if (!wm) return AJS_VAL_EXCEPTION;
    AjsValue k = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!can_be_held_weakly(k)) return AJS_VAL_FALSE;
    return ajs_bool(col_find_entry(&wm->u.col, k, ajs_map_hash_value(k)) >= 0);
}
static AjsValue js_weakmap_delete(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *wm = require_col_kind(vm, this_val, AJS_OBJ_WEAKMAP, "WeakMap.prototype.delete");
    if (!wm) return AJS_VAL_EXCEPTION;
    AjsValue k = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!can_be_held_weakly(k)) return AJS_VAL_FALSE;
    return ajs_bool(col_delete(&wm->u.col, k));
}

static AjsValue js_weakset_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt == AJS_VAL_UNDEFINED) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Constructor WeakSet requires 'new'");
    AjsValue proto = col_get_proto_from_ctor(vm, nt, vm->active_realm->weakset_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, proto);
    AjsObject *ws = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_WEAKSET, proto);
    if (!ws) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ws->u.col.is_set = true;
    ws->u.col.is_weak = true;
    AjsValue wsv = ajs_val_from_gc(ws);
    ajs_temp_root_push(vm, wsv);
    if (argc > 0 && !ajs_is_nullish(argv[0])) {
        AjsValue adder = ajs_obj_get(vm, wsv, ajs_val_from_gc(ajs_str_from_cstr(vm, "add", true)), wsv);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, adder);
        if (!populate_from_iterable(vm, wsv, adder, argv[0], false)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    ajs_temp_root_restore(vm, rs);
    return wsv;
}

static AjsValue js_weakset_add(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *ws = require_col_kind(vm, this_val, AJS_OBJ_WEAKSET, "WeakSet.prototype.add");
    if (!ws) return AJS_VAL_EXCEPTION;
    AjsValue v = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!can_be_held_weakly(v)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Invalid value used in weak set");
    }
    return col_put(vm, &ws->u.col, v, v) ? this_val : AJS_VAL_EXCEPTION;
}
static AjsValue js_weakset_has(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *ws = require_col_kind(vm, this_val, AJS_OBJ_WEAKSET, "WeakSet.prototype.has");
    if (!ws) return AJS_VAL_EXCEPTION;
    AjsValue v = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!can_be_held_weakly(v)) return AJS_VAL_FALSE;
    return ajs_bool(col_find_entry(&ws->u.col, v, ajs_map_hash_value(v)) >= 0);
}
static AjsValue js_weakset_delete(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *ws = require_col_kind(vm, this_val, AJS_OBJ_WEAKSET, "WeakSet.prototype.delete");
    if (!ws) return AJS_VAL_EXCEPTION;
    AjsValue v = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!can_be_held_weakly(v)) return AJS_VAL_FALSE;
    return ajs_bool(col_delete(&ws->u.col, v));
}

/* ============================================================================
 * 5. WeakRef & FinalizationRegistry
 * ============================================================================ */

static AjsValue js_weakref_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt == AJS_VAL_UNDEFINED) return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Constructor WeakRef requires 'new'");
    AjsValue target = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!can_be_held_weakly(target)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "WeakRef: invalid target");
    }
    AjsValue proto = col_get_proto_from_ctor(vm, nt, vm->active_realm->weakref_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, proto);
    ajs_temp_root_push(vm, target);
    AjsObject *wr = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_WEAKREF, proto);
    ajs_temp_root_restore(vm, rs);
    if (!wr) return AJS_VAL_EXCEPTION;
    wr->u.prim_val = target;
    return ajs_val_from_gc(wr);
}

static AjsValue js_weakref_deref(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    AjsObject *wr = require_col_kind(vm, this_val, AJS_OBJ_WEAKREF, "WeakRef.prototype.deref");
    return wr ? wr->u.prim_val : AJS_VAL_EXCEPTION;
}

static AjsValue js_finreg_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt == AJS_VAL_UNDEFINED) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Constructor FinalizationRegistry requires 'new'");
    }
    AjsValue cb = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_is_callable(vm, cb)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "FinalizationRegistry: cleanupCallback must be callable");
    }
    AjsValue proto = col_get_proto_from_ctor(vm, nt, vm->active_realm->finreg_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, proto);
    ajs_temp_root_push(vm, cb);
    AjsObject *fr = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_FINREG, proto);
    ajs_temp_root_restore(vm, rs);
    if (!fr) return AJS_VAL_EXCEPTION;
    fr->u.finreg.cleanup_cb = cb;
    return ajs_val_from_gc(fr);
}

static AjsValue js_finreg_register(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *fr = require_col_kind(vm, this_val, AJS_OBJ_FINREG, "FinalizationRegistry.prototype.register");
    if (!fr) return AJS_VAL_EXCEPTION;
    AjsValue target = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue held = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    AjsValue token = argc > 2 ? argv[2] : AJS_VAL_UNDEFINED;
    if (!can_be_held_weakly(target)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "FinalizationRegistry.prototype.register: invalid target");
    }
    if (ajs_same_value(vm, target, held)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                  "FinalizationRegistry.prototype.register: target and heldValue must not be the same");
    }
    if (!ajs_is_undefined(token) && !can_be_held_weakly(token)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                  "FinalizationRegistry.prototype.register: invalid unregisterToken");
    }
    for (uint32_t i = 0; i < fr->u.finreg.count; i++) {
        if (!fr->u.finreg.entries[i].alive) {
            fr->u.finreg.entries[i].target = target;
            fr->u.finreg.entries[i].held_value = held;
            fr->u.finreg.entries[i].unregister_token = token;
            fr->u.finreg.entries[i].alive = true;
            return AJS_VAL_UNDEFINED;
        }
    }
    if (fr->u.finreg.count >= fr->u.finreg.cap) {
        uint32_t ncap = fr->u.finreg.cap ? fr->u.finreg.cap * 2u : 8u;
        AjsFinRegEntry *ne = (AjsFinRegEntry *)realloc(fr->u.finreg.entries, (size_t)ncap * sizeof(AjsFinRegEntry));
        if (!ne) return ajs_throw(vm, vm->oom_error);
        fr->u.finreg.entries = ne;
        fr->u.finreg.cap = ncap;
    }
    uint32_t idx = fr->u.finreg.count++;
    fr->u.finreg.entries[idx].target = target;
    fr->u.finreg.entries[idx].held_value = held;
    fr->u.finreg.entries[idx].unregister_token = token;
    fr->u.finreg.entries[idx].alive = true;
    return AJS_VAL_UNDEFINED;
}

static AjsValue js_finreg_unregister(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsObject *fr = require_col_kind(vm, this_val, AJS_OBJ_FINREG, "FinalizationRegistry.prototype.unregister");
    if (!fr) return AJS_VAL_EXCEPTION;
    AjsValue token = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!can_be_held_weakly(token)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                  "FinalizationRegistry.prototype.unregister: invalid unregisterToken");
    }
    bool removed = false;
    for (uint32_t i = 0; i < fr->u.finreg.count; i++) {
        AjsFinRegEntry *e = &fr->u.finreg.entries[i];
        if (e->alive && ajs_same_value(vm, e->unregister_token, token)) {
            e->alive = false;
            e->target = AJS_VAL_UNDEFINED;
            e->held_value = AJS_VAL_UNDEFINED;
            e->unregister_token = AJS_VAL_UNDEFINED;
            removed = true;
        }
    }
    return ajs_bool(removed);
}

void ajs_finreg_schedule_cleanup(AjsVm *vm, AjsObject *finreg_obj, AjsValue held_value) {
    if (!vm || !finreg_obj || finreg_obj->kind != AJS_OBJ_FINREG) return;
    AjsValue cb = finreg_obj->u.finreg.cleanup_cb;
    if (!ajs_is_callable(vm, cb)) return;
    ajs_enqueue_job_ex(vm, finreg_obj->realm ? finreg_obj->realm : vm->main_realm, cb, 1, &held_value);
}

static AjsValue js_species_this(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)vm; (void)c; (void)v; (void)u;
    return this_val;
}

/* ============================================================================
 * 6. Realm Initialization for Collections & Weak References
 * ============================================================================ */

static void link_ctor_proto(AjsVm *vm, AjsRealm *r, AjsValue ctor, AjsValue proto, const char *global_name) {
    col_def_prop(vm, ctor, "prototype", proto, 0);
    col_def_prop(vm, proto, "constructor", ctor, AJS_DESC_WRITABLE | AJS_DESC_CONFIGURABLE);
    col_def_prop(vm, r->global_obj, global_name, ctor, AJS_DESC_WRITABLE | AJS_DESC_CONFIGURABLE);
}

static void def_species_getter(AjsVm *vm, AjsRealm *r, AjsValue ctor) {
    size_t rs = ajs_temp_root_save(vm);
    AjsValue g = col_make_fn(vm, r, js_species_this, "get [Symbol.species]", 0, false, 0, r->function_proto);
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

void ajs_init_collections_builtins(AjsVm *vm, AjsRealm *r) {
    size_t rs = ajs_temp_root_save(vm);

    /* Iterator Prototypes */
    r->map_iter_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->iterator_proto));
    ajs_def_native_method(vm, r, r->map_iter_proto, "next", js_col_iter_next, 0, AJS_METHOD_FLAGS, 0);
    col_def_tag(vm, r->map_iter_proto, "Map Iterator");

    r->set_iter_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->iterator_proto));
    ajs_def_native_method(vm, r, r->set_iter_proto, "next", js_col_iter_next, 0, AJS_METHOD_FLAGS, 1);
    col_def_tag(vm, r->set_iter_proto, "Set Iterator");

    /* Map */
    r->map_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->map_ctor = col_make_fn(vm, r, js_map_ctor, "Map", 0, true, -30, r->function_proto);
    link_ctor_proto(vm, r, r->map_ctor, r->map_proto, "Map");
    def_species_getter(vm, r, r->map_ctor);
    ajs_def_native_method(vm, r, r->map_ctor, "groupBy", js_map_groupBy, 2, AJS_METHOD_FLAGS, 0);

    ajs_def_native_method(vm, r, r->map_proto, "get", js_map_get, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->map_proto, "set", js_map_set, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->map_proto, "has", js_map_has, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->map_proto, "delete", js_map_delete, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->map_proto, "clear", js_map_clear, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->map_proto, "forEach", js_map_forEach, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_accessor(vm, r, r->map_proto, "size", js_map_size, NULL, AJS_PROP_CONFIGURABLE);
    ajs_def_native_method(vm, r, r->map_proto, "keys", js_col_iter_create, 0, AJS_METHOD_FLAGS, AJS_ITER_MAP_KEYS);
    ajs_def_native_method(vm, r, r->map_proto, "values", js_col_iter_create, 0, AJS_METHOD_FLAGS, AJS_ITER_MAP_VALUES);
    AjsValue map_entries_fn = col_make_fn(vm, r, js_col_iter_create, "entries", 0, false, AJS_ITER_MAP_ENTRIES, r->function_proto);
    ajs_temp_root_push(vm, map_entries_fn);
    col_def_prop(vm, r->map_proto, "entries", map_entries_fn, AJS_DESC_WRITABLE | AJS_DESC_CONFIGURABLE);
    {
        AjsPropDesc sd;
        memset(&sd, 0, sizeof(sd));
        sd.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                   AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
        sd.value = map_entries_fn;
        ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(r->map_proto),
                                ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), &sd, false);
    }
    col_def_tag(vm, r->map_proto, "Map");

    /* Set */
    r->set_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->set_ctor = col_make_fn(vm, r, js_set_ctor, "Set", 0, true, -31, r->function_proto);
    link_ctor_proto(vm, r, r->set_ctor, r->set_proto, "Set");
    def_species_getter(vm, r, r->set_ctor);

    ajs_def_native_method(vm, r, r->set_proto, "add", js_set_add, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->set_proto, "has", js_set_has, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->set_proto, "delete", js_set_delete, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->set_proto, "clear", js_set_clear, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->set_proto, "forEach", js_set_forEach, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_accessor(vm, r, r->set_proto, "size", js_set_size, NULL, AJS_PROP_CONFIGURABLE);
    ajs_def_native_method(vm, r, r->set_proto, "entries", js_col_iter_create, 0, AJS_METHOD_FLAGS, AJS_ITER_SET_ENTRIES);
    /* Set.prototype.values === Set.prototype.keys === Set.prototype[Symbol.iterator] */
    AjsValue set_vals_fn = col_make_fn(vm, r, js_col_iter_create, "values", 0, false, AJS_ITER_SET_VALUES, r->function_proto);
    ajs_temp_root_push(vm, set_vals_fn);
    col_def_prop(vm, r->set_proto, "values", set_vals_fn, AJS_DESC_WRITABLE | AJS_DESC_CONFIGURABLE);
    col_def_prop(vm, r->set_proto, "keys", set_vals_fn, AJS_DESC_WRITABLE | AJS_DESC_CONFIGURABLE);
    {
        AjsPropDesc sd;
        memset(&sd, 0, sizeof(sd));
        sd.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                   AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
        sd.value = set_vals_fn;
        ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(r->set_proto),
                                ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), &sd, false);
    }
    /* ES2025/2026 Set Methods */
    ajs_def_native_method(vm, r, r->set_proto, "union", js_set_union, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->set_proto, "intersection", js_set_intersection, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->set_proto, "difference", js_set_difference, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->set_proto, "symmetricDifference", js_set_symmetricDifference, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->set_proto, "isSubsetOf", js_set_isSubsetOf, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->set_proto, "isSupersetOf", js_set_isSupersetOf, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->set_proto, "isDisjointFrom", js_set_isDisjointFrom, 1, AJS_METHOD_FLAGS, 0);
    col_def_tag(vm, r->set_proto, "Set");

    /* WeakMap */
    r->weakmap_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->weakmap_ctor = col_make_fn(vm, r, js_weakmap_ctor, "WeakMap", 0, true, -32, r->function_proto);
    link_ctor_proto(vm, r, r->weakmap_ctor, r->weakmap_proto, "WeakMap");
    ajs_def_native_method(vm, r, r->weakmap_proto, "get", js_weakmap_get, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->weakmap_proto, "set", js_weakmap_set, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->weakmap_proto, "has", js_weakmap_has, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->weakmap_proto, "delete", js_weakmap_delete, 1, AJS_METHOD_FLAGS, 0);
    col_def_tag(vm, r->weakmap_proto, "WeakMap");

    /* WeakSet */
    r->weakset_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->weakset_ctor = col_make_fn(vm, r, js_weakset_ctor, "WeakSet", 0, true, -33, r->function_proto);
    link_ctor_proto(vm, r, r->weakset_ctor, r->weakset_proto, "WeakSet");
    ajs_def_native_method(vm, r, r->weakset_proto, "add", js_weakset_add, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->weakset_proto, "has", js_weakset_has, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->weakset_proto, "delete", js_weakset_delete, 1, AJS_METHOD_FLAGS, 0);
    col_def_tag(vm, r->weakset_proto, "WeakSet");

    /* WeakRef */
    r->weakref_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->weakref_ctor = col_make_fn(vm, r, js_weakref_ctor, "WeakRef", 1, true, -34, r->function_proto);
    link_ctor_proto(vm, r, r->weakref_ctor, r->weakref_proto, "WeakRef");
    ajs_def_native_method(vm, r, r->weakref_proto, "deref", js_weakref_deref, 0, AJS_METHOD_FLAGS, 0);
    col_def_tag(vm, r->weakref_proto, "WeakRef");

    /* FinalizationRegistry */
    r->finreg_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->finreg_ctor = col_make_fn(vm, r, js_finreg_ctor, "FinalizationRegistry", 1, true, -35, r->function_proto);
    link_ctor_proto(vm, r, r->finreg_ctor, r->finreg_proto, "FinalizationRegistry");
    ajs_def_native_method(vm, r, r->finreg_proto, "register", js_finreg_register, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->finreg_proto, "unregister", js_finreg_unregister, 1, AJS_METHOD_FLAGS, 0);
    col_def_tag(vm, r->finreg_proto, "FinalizationRegistry");

    ajs_temp_root_restore(vm, rs);
}
