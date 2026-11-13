/*
 * ArcradientJS 1.0 - js/vm/ajs_interp.c
 * Clean-Room Pure C11 ECMAScript 2026 Bytecode Interpreter
 *
 * Implements:
 * - Lexical environments, closures, arguments exotic objects, sloppy `with` & direct `eval`
 * - Call / Construct / Bound Functions / Proxy `apply` & `construct`
 * - Bytecode dispatch loop with try/catch/finally & IteratorClose unwinding
 * - Derived class `super()`, `super.prop`, `#private` fields/methods/accessors
 * - Generators, Async Functions, Async Generators (`AjsAsyncGenReq` queue), `yield*` delegation
 */
#include "ajs_internal.h"

/* Forward declaration of core frame execution loop */
typedef enum AjsFrameRunStatus {
    AJS_RUN_RETURNED  = 0,
    AJS_RUN_SUSPENDED = 1,
    AJS_RUN_THREW     = 2,
    AJS_RUN_AWAITED   = 3
} AjsFrameRunStatus;
static bool attach_await_continuation(AjsVm *vm, AjsRealm *realm, AjsObject *gen_obj, AjsValue awaited_val);

static AjsFrameRunStatus ajs_run_frame(AjsVm *vm, AjsExecFrame *frame, AjsValue *out_val);
static void async_gen_drain_queue(AjsVm *vm, AjsObject *gen_obj);

/* ============================================================================
 * 1. Lexical Environment Helpers
 * ============================================================================ */

static void env_rebuild_name_hash(AjsEnv *env, uint32_t ncap) {
    if (!env || !env->names) return;
    int32_t *nt = (int32_t *)malloc((size_t)ncap * sizeof(int32_t));
    if (!nt) return;
    for (uint32_t i = 0; i < ncap; i++) nt[i] = -1;
    uint32_t mask = ncap - 1u;
    for (uint32_t i = 0; i < env->count; i++) {
        if (!env->names[i]) continue;
        uint32_t pos = env->names[i]->hash & mask;
        while (nt[pos] >= 0) pos = (pos + 1u) & mask;
        nt[pos] = (int32_t)i;
    }
    free(env->name_hash);
    env->name_hash = nt;
    env->name_hash_cap = ncap;
}

AjsEnv *ajs_env_new(AjsVm *vm, AjsEnv *parent, uint32_t slot_count, AjsString **names) {
    AjsEnv *env = (AjsEnv *)ajs_gc_alloc(vm, sizeof(AjsEnv), AJS_GC_ENV);
    if (!env) return NULL;
    env->parent = parent;
    env->with_obj = NULL;
    env->count = slot_count;
    env->cap = slot_count;
    if (slot_count > 0) {
        env->slots = (AjsValue *)malloc((size_t)slot_count * sizeof(AjsValue));
        env->flags = (uint8_t *)calloc(slot_count, sizeof(uint8_t));
        if (!env->slots || !env->flags) {
            free(env->slots);
            free(env->flags);
            env->slots = NULL;
            env->flags = NULL;
            env->count = 0;
            env->cap = 0;
            ajs_throw(vm, vm->oom_error);
            return NULL;
        }
        for (uint32_t i = 0; i < slot_count; i++) {
            env->slots[i] = AJS_VAL_UNDEFINED;
        }
        if (names) {
            env->names = (AjsString **)malloc((size_t)slot_count * sizeof(AjsString *));
            if (env->names) {
                for (uint32_t i = 0; i < slot_count; i++) {
                    env->names[i] = names[i];
                }
                if (slot_count >= 16u) {
                    uint32_t hcap = 32u;
                    while (hcap < slot_count * 2u && hcap < (1u << 30)) hcap *= 2u;
                    env_rebuild_name_hash(env, hcap);
                }
            }
        }
    }
    return env;
}

static int32_t env_find_named_slot(const AjsEnv *env, const AjsString *name) {
    if (!env || !env->names || !name) return -1;
    if (env->name_hash && env->name_hash_cap > 0) {
        uint32_t mask = env->name_hash_cap - 1u;
        uint32_t pos = name->hash & mask;
        while (env->name_hash[pos] >= 0) {
            int32_t idx = env->name_hash[pos];
            if (env->names[idx] && ajs_str_eq(env->names[idx], name)) {
                return idx;
            }
            pos = (pos + 1u) & mask;
        }
        return -1;
    }
    for (uint32_t i = 0; i < env->count; i++) {
        if (env->names[i] && ajs_str_eq(env->names[i], name)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t env_append_named_slot(AjsEnv *env, AjsString *name, AjsValue init_val, uint8_t flags) {
    uint32_t idx = env->count;
    if (idx >= env->cap || !env->names) {
        uint32_t ncap = env->cap ? env->cap * 2u : 8u;
        if (ncap <= idx) ncap = idx + 8u;
        AjsValue *ns = (AjsValue *)realloc(env->slots, (size_t)ncap * sizeof(AjsValue));
        uint8_t *nf = (uint8_t *)realloc(env->flags, (size_t)ncap * sizeof(uint8_t));
        AjsString **nn = env->names
                         ? (AjsString **)realloc(env->names, (size_t)ncap * sizeof(AjsString *))
                         : (AjsString **)calloc(ncap, sizeof(AjsString *));
        if (!ns || !nf || !nn) {
            if (ns) env->slots = ns;
            if (nf) env->flags = nf;
            if (nn) env->names = nn;
            return -1;
        }
        env->slots = ns;
        env->flags = nf;
        env->names = nn;
        env->cap = ncap;
    }
    env->slots[idx] = init_val;
    env->flags[idx] = flags;
    env->names[idx] = name;
    env->count = idx + 1u;
    if (name) {
        if (env->name_hash) {
            if ((env->count + 1u) * 10u >= env->name_hash_cap * 7u) {
                env_rebuild_name_hash(env, env->name_hash_cap * 2u);
            } else {
                uint32_t mask = env->name_hash_cap - 1u;
                uint32_t pos = name->hash & mask;
                while (env->name_hash[pos] >= 0) pos = (pos + 1u) & mask;
                env->name_hash[pos] = (int32_t)idx;
            }
        } else if (env->count >= 16u) {
            env_rebuild_name_hash(env, 64u);
        }
    }
    return (int32_t)idx;
}

static AjsEnv *env_at_depth(AjsEnv *cur, uint16_t depth) {
    while (depth > 0 && cur) {
        cur = cur->parent;
        depth--;
    }
    return cur;
}

static bool env_read_slot(AjsVm *vm, AjsEnv *env, uint32_t slot, AjsValue *out_val) {
    if (!env || slot >= env->count) {
        *out_val = AJS_VAL_UNDEFINED;
        return true;
    }
    if ((env->flags[slot] & AJS_SLOT_IMPORT_LINK) && env->import_links) {
        AjsImportLink *il = &env->import_links[slot];
        if (il->is_namespace) {
            *out_val = ajs_module_namespace(vm, il->target_mod);
            return !vm->has_exception;
        }
        if (!il->target_mod || !il->target_mod->env || il->target_slot >= il->target_mod->env->count) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE, "Cannot access unlinked module import");
            return false;
        }
        AjsValue tv = il->target_mod->env->slots[il->target_slot];
        if (tv == AJS_VAL_TDZ) {
            const char *nm = (env->names && env->names[slot]) ? env->names[slot]->utf8 : "<import>";
            ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE, "Cannot access '%s' before initialization", nm);
            return false;
        }
        *out_val = tv;
        return true;
    }
    AjsValue v = env->slots[slot];
    if (v == AJS_VAL_TDZ) {
        const char *nm = (env->names && env->names[slot]) ? env->names[slot]->utf8 : "<lexical>";
        ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE, "Cannot access '%s' before initialization", nm);
        return false;
    }
    *out_val = v;
    return true;
}

static bool env_write_slot(AjsVm *vm, AjsEnv *env, uint32_t slot, AjsValue val, bool strict) {
    if (!env || slot >= env->count) return true;
    if (env->slots[slot] == AJS_VAL_TDZ) {
        const char *nm = (env->names && env->names[slot]) ? env->names[slot]->utf8 : "<lexical>";
        ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE, "Cannot access '%s' before initialization", nm);
        return false;
    }
    if (env->flags[slot] & AJS_SLOT_CONST) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Assignment to constant variable.");
        return false;
    }
    if (env->flags[slot] & AJS_SLOT_FUNC_NAME) {
        if (strict) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Assignment to constant variable.");
            return false;
        }
        return true; /* Sloppy mode ignores writes to named FunctionExpression self-binding */
    }
    env->slots[slot] = val;
    return true;
}

static bool with_obj_has_binding(AjsVm *vm, AjsObject *wobj, AjsValue key_val, bool *out_has) {
    bool found = false;
    if (!ajs_obj_has_prop(vm, wobj, key_val, &found)) return false;
    if (!found) {
        *out_has = false;
        return true;
    }
    AjsValue wval = ajs_val_from_gc(wobj);
    AjsValue sym_unscop = ajs_val_from_gc(vm->well_known_syms[AJS_SYM_UNSCOPABLES]);
    AjsValue unscop = ajs_obj_get(vm, wval, sym_unscop, wval);
    if (vm->has_exception) return false;
    if (ajs_is_object(vm, unscop)) {
        size_t rs = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, unscop);
        AjsValue blocked = ajs_obj_get(vm, unscop, key_val, unscop);
        ajs_temp_root_restore(vm, rs);
        if (vm->has_exception) return false;
        if (ajs_to_bool(vm, blocked)) {
            *out_has = false;
            return true;
        }
    }
    *out_has = true;
    return true;
}

/* ============================================================================
 * 2. Closures, Execution Frames & Tagged Template Objects
 * ============================================================================ */

AjsString *ajs_bytecode_get_source_text(AjsVm *vm, AjsBytecode *bc) {
    if (!bc) return NULL;
    if (bc->source_text) return bc->source_text;
    if (bc->source_buf && (size_t)bc->src_offset + (size_t)bc->src_len <= bc->source_buf->len) {
        bc->source_text = ajs_str_from_utf8(vm, bc->source_buf->utf8 + bc->src_offset, bc->src_len, false);
    }
    return bc->source_text;
}

AjsObject *ajs_closure_new(AjsVm *vm, AjsRealm *realm, AjsBytecode *bc, AjsEnv *env, AjsModuleRecord *mod) {
    size_t rs = ajs_temp_root_save(vm);
    if (bc) ajs_temp_root_push(vm, ajs_val_from_gc(bc));
    if (env) ajs_temp_root_push(vm, ajs_val_from_gc(env));

    AjsValue fn_proto = realm->function_proto;
    if (bc->fn_kind == AJS_FN_BC_GENERATOR) fn_proto = realm->gen_func_proto;
    else if (bc->fn_kind == AJS_FN_BC_ASYNC || bc->fn_kind == AJS_FN_BC_ASYNC_ARROW) fn_proto = realm->async_func_proto;
    else if (bc->fn_kind == AJS_FN_BC_ASYNC_GENERATOR) fn_proto = realm->async_gen_func_proto;

    AjsObject *fn = ajs_obj_new(vm, realm, AJS_OBJ_FUNCTION, fn_proto);
    if (!fn) {
        ajs_temp_root_restore(vm, rs);
        return NULL;
    }
    ajs_temp_root_push(vm, ajs_val_from_gc(fn));

    fn->u.fn.fn_kind = bc->fn_kind;
    fn->u.fn.strict = bc->strict;
    fn->u.fn.is_derived_ctor = bc->is_derived_ctor;
    fn->u.fn.is_constructor = (bc->fn_kind == AJS_FN_BC_NORMAL || bc->fn_kind == AJS_FN_BC_CLASS_CTOR);
    fn->u.fn.bc = bc;
    fn->u.fn.closure_env = env;
    fn->u.fn.home_object = AJS_VAL_UNDEFINED;
    fn->u.fn.inst_init_fn = AJS_VAL_UNDEFINED;
    fn->u.fn.source_text = bc->source_text;
    fn->u.fn.bound_target = AJS_VAL_UNDEFINED;
    fn->u.fn.bound_this = AJS_VAL_UNDEFINED;
    fn->u.fn.owner_module = mod;

    /* Fast own-slot insertion for `length` & `name` (configurable, non-writable, non-enumerable) */
    ajs_obj_insert_own_slot(fn, ajs_val_from_gc(vm->str_length), AJS_DESC_CONFIGURABLE,
                            ajs_val_number((double)bc->arity), AJS_VAL_UNDEFINED);
    if (bc->name) {
        ajs_obj_insert_own_slot(fn, ajs_val_from_gc(vm->str_name), AJS_DESC_CONFIGURABLE,
                                ajs_val_from_gc(bc->name), AJS_VAL_UNDEFINED);
    }

    /* Defer `.prototype` object allocation until first access */
    if (bc->fn_kind == AJS_FN_BC_NORMAL ||
        bc->fn_kind == AJS_FN_BC_GENERATOR ||
        bc->fn_kind == AJS_FN_BC_ASYNC_GENERATOR) {
        fn->u.fn.has_lazy_proto = true;
    }

    ajs_temp_root_restore(vm, rs);
    return fn;
}

static bool frame_stack_push(AjsExecFrame *f, AjsValue v) {
    if (f->sp >= f->stack_cap) {
        uint32_t ncap = f->stack_cap ? f->stack_cap * 2u : 32u;
        AjsValue *ns = (AjsValue *)realloc(f->stack, ncap * sizeof(AjsValue));
        if (!ns) return false;
        f->stack = ns;
        f->stack_cap = ncap;
    }
    f->stack[f->sp++] = v;
    return true;
}

AjsExecFrame *ajs_frame_new(AjsVm *vm, AjsRealm *realm, AjsObject *func_obj, AjsBytecode *bc,
                            AjsEnv *env, AjsValue this_val, AjsValue new_target, AjsModuleRecord *mod) {
    size_t rs = ajs_temp_root_save(vm);
    if (func_obj) ajs_temp_root_push(vm, ajs_val_from_gc(func_obj));
    if (bc) ajs_temp_root_push(vm, ajs_val_from_gc(bc));
    if (env) ajs_temp_root_push(vm, ajs_val_from_gc(env));
    ajs_temp_root_push(vm, this_val);
    ajs_temp_root_push(vm, new_target);

    AjsExecFrame *f = (AjsExecFrame *)ajs_gc_alloc(vm, sizeof(AjsExecFrame), AJS_GC_FRAME);
    if (!f) {
        ajs_temp_root_restore(vm, rs);
        return NULL;
    }
    ajs_temp_root_push(vm, ajs_val_from_gc(f));

    f->realm = realm;
    f->func_obj = func_obj;
    f->bc = bc;
    f->this_val = this_val;
    f->new_target = new_target;
    f->module = mod;
    f->stack_cap = 32u;
    f->stack = (AjsValue *)malloc(f->stack_cap * sizeof(AjsValue));
    f->ys_iter = AJS_VAL_UNDEFINED;
    f->ys_next = AJS_VAL_UNDEFINED;

    /* For ES Modules, `mod->env` was already instantiated during `link`! */
    AjsEnv *base_env = NULL;
    if (bc->is_module && mod && mod->env) {
        base_env = mod->env;
    } else {
        base_env = ajs_env_new(vm, env, bc->env_slot_count, bc->env_slot_names);
        if (!base_env) {
            ajs_temp_root_restore(vm, rs);
            return NULL;
        }
        if (bc->env_slot_flags) {
            for (uint32_t i = 0; i < bc->env_slot_count; i++) {
                uint8_t fl = bc->env_slot_flags[i];
                base_env->flags[i] = (uint8_t)(fl & (AJS_SLOT_CONST | AJS_SLOT_FUNC_NAME));
                if (fl & AJS_SLOT_INIT_TDZ) {
                    base_env->slots[i] = AJS_VAL_TDZ;
                } else if ((fl & AJS_SLOT_FUNC_NAME) && func_obj) {
                    base_env->slots[i] = ajs_val_from_gc(func_obj);
                } else {
                    base_env->slots[i] = AJS_VAL_UNDEFINED;
                }
            }
        }
    }
    f->base_env = base_env;
    f->env = base_env;
    f->env_depth = 0;

    if (base_env) {
        if (bc->this_slot != AJS_NO_SLOT && bc->this_slot < base_env->count) {
            base_env->slots[bc->this_slot] = bc->is_derived_ctor ? AJS_VAL_TDZ : this_val;
        }
        if (bc->new_target_slot != AJS_NO_SLOT && bc->new_target_slot < base_env->count) {
            base_env->slots[bc->new_target_slot] = new_target;
        }
        if (bc->active_fn_slot != AJS_NO_SLOT && bc->active_fn_slot < base_env->count) {
            base_env->slots[bc->active_fn_slot] = func_obj ? ajs_val_from_gc(func_obj) : AJS_VAL_UNDEFINED;
        }
    }

    ajs_temp_root_restore(vm, rs);
    return f;
}

static bool setup_call_frame_args(AjsVm *vm, AjsExecFrame *f, int argc, const AjsValue *argv) {
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(f));
    AjsBytecode *bc = f->bc;
    AjsRealm *realm = f->realm;

    /* Create `arguments` exotic object when needed */
    if (bc->needs_arguments) {
        AjsObject *aobj = ajs_obj_new(vm, realm, AJS_OBJ_ARGUMENTS, realm->object_proto);
        if (!aobj) { ajs_temp_root_restore(vm, rs); return false; }
        f->args_obj = aobj;
        ajs_temp_root_push(vm, ajs_val_from_gc(aobj));

        /* Populate indexed properties 0..argc-1 BEFORE wiring mapped param slots (params are still TDZ!) */
        for (int i = 0; i < argc; i++) {
            AjsString *ks = ajs_number_to_str(vm, (double)i, 10);
            ks = ajs_str_intern(vm, ks);
            AjsPropDesc idesc = {
                .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                         AJS_DESC_HAS_ENUMERABLE | AJS_DESC_ENUMERABLE |
                         AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
                .value = argv[i]
            };
            ajs_obj_define_own_prop(vm, aobj, ajs_val_from_gc(ks), &idesc, false);
        }

        AjsPropDesc len_desc = {
            .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                     AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
            .value = ajs_val_number((double)argc)
        };
        ajs_obj_define_own_prop(vm, aobj, ajs_val_from_gc(vm->str_length), &len_desc, false);

        if (realm->array_proto_values_fn != AJS_VAL_UNDEFINED) {
            AjsPropDesc it_desc = {
                .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                         AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
                .value = realm->array_proto_values_fn
            };
            ajs_obj_define_own_prop(vm, aobj, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), &it_desc, false);
        }

        if (!bc->strict && bc->has_simple_params && bc->mapped_param_slots) {
            uint32_t mcnt = (uint32_t)argc < bc->param_count ? (uint32_t)argc : bc->param_count;
            if (mcnt > 0) {
                aobj->u.args.mapped = true;
                aobj->u.args.env = f->base_env;
                aobj->u.args.param_count = mcnt;
                aobj->u.args.param_slots = (uint32_t *)malloc((size_t)mcnt * sizeof(uint32_t));
                if (aobj->u.args.param_slots) {
                    for (uint32_t i = 0; i < mcnt; i++) {
                        aobj->u.args.param_slots[i] = bc->mapped_param_slots[i];
                    }
                }
            }
            if (f->func_obj) {
                AjsPropDesc cal_desc = {
                    .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                             AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
                    .value = ajs_val_from_gc(f->func_obj)
                };
                AjsString *s_callee = ajs_str_from_cstr(vm, "callee", true);
                ajs_obj_define_own_prop(vm, aobj, ajs_val_from_gc(s_callee), &cal_desc, false);
            }
        } else if (!bc->strict && bc->has_simple_params && bc->param_count == 0) {
            if (f->func_obj) {
                AjsPropDesc cal_desc = {
                    .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                             AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
                    .value = ajs_val_from_gc(f->func_obj)
                };
                AjsString *s_callee = ajs_str_from_cstr(vm, "callee", true);
                ajs_obj_define_own_prop(vm, aobj, ajs_val_from_gc(s_callee), &cal_desc, false);
            }
        } else if (realm->throw_type_error_fn != AJS_VAL_UNDEFINED) {
            AjsPropDesc tte_desc = {
                .flags = AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_GET | AJS_DESC_HAS_SET |
                         AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE,
                .getter = realm->throw_type_error_fn,
                .setter = realm->throw_type_error_fn
            };
            AjsString *s_callee = ajs_str_from_cstr(vm, "callee", true);
            ajs_obj_define_own_prop(vm, aobj, ajs_val_from_gc(s_callee), &tte_desc, false);
        }
        if (bc->arguments_slot != AJS_NO_SLOT && f->base_env && bc->arguments_slot < f->base_env->count) {
            f->base_env->slots[bc->arguments_slot] = ajs_val_from_gc(aobj);
        }
    }

    /* Push incoming parameters onto `f->stack` in REVERSE order so `arg[0]` is on TOP of stack! */
    for (int32_t i = (int32_t)bc->param_count - 1; i >= 0; i--) {
        if (bc->has_rest_param && i == (int32_t)bc->param_count - 1) {
            uint32_t rlen = (argc > i) ? (uint32_t)(argc - i) : 0u;
            AjsObject *rarr = ajs_obj_new(vm, realm, AJS_OBJ_ARRAY, realm->array_proto);
            if (!rarr) { ajs_temp_root_restore(vm, rs); return false; }
            ajs_temp_root_push(vm, ajs_val_from_gc(rarr));
            rarr->u.arr.length_writable = true;
            rarr->u.arr.length = rlen;
            if (rlen > 0) {
                rarr->u.arr.dense_cap = rlen;
                rarr->u.arr.dense_len = rlen;
                rarr->u.arr.dense = (AjsValue *)malloc(rlen * sizeof(AjsValue));
                for (uint32_t k = 0; k < rlen; k++) {
                    rarr->u.arr.dense[k] = argv[i + (int)k];
                }
            }
            frame_stack_push(f, ajs_val_from_gc(rarr));
        } else {
            AjsValue av = (i < argc) ? argv[i] : AJS_VAL_UNDEFINED;
            frame_stack_push(f, av);
        }
    }

    ajs_temp_root_restore(vm, rs);
    return true;
}

static AjsValue get_template_object(AjsVm *vm, AjsRealm *realm, AjsBytecode *bc, uint32_t tpl_idx) {
    if (tpl_idx >= bc->tpl_count) return AJS_VAL_UNDEFINED;
    AjsTplSiteDesc *td = &bc->tpls[tpl_idx];

    for (uint32_t i = 0; i < realm->tpl_cache_count; i++) {
        if (realm->tpl_cache[i].bc == bc && realm->tpl_cache[i].tpl_id == td->id) {
            return realm->tpl_cache[i].tpl_array;
        }
    }

    size_t rs = ajs_temp_root_save(vm);
    AjsObject *cooked_arr = ajs_obj_new(vm, realm, AJS_OBJ_ARRAY, realm->array_proto);
    AjsObject *raw_arr = ajs_obj_new(vm, realm, AJS_OBJ_ARRAY, realm->array_proto);
    if (!cooked_arr || !raw_arr) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_UNDEFINED;
    }
    ajs_temp_root_push(vm, ajs_val_from_gc(cooked_arr));
    ajs_temp_root_push(vm, ajs_val_from_gc(raw_arr));

    cooked_arr->u.arr.length_writable = true;
    raw_arr->u.arr.length_writable = true;

    for (uint32_t i = 0; i < td->count; i++) {
        AjsString *ks = ajs_number_to_str(vm, (double)i, 10);
        ks = ajs_str_intern(vm, ks);
        AjsValue kval = ajs_val_from_gc(ks);
        ajs_temp_root_push(vm, kval);

        AjsPropDesc cdesc = {
            .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE |
                     AJS_DESC_HAS_ENUMERABLE | AJS_DESC_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE,
            .value = td->cooked[i] ? ajs_val_from_gc(td->cooked[i]) : AJS_VAL_UNDEFINED
        };
        ajs_obj_define_own_prop(vm, cooked_arr, kval, &cdesc, false);

        AjsPropDesc rdesc = {
            .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE |
                     AJS_DESC_HAS_ENUMERABLE | AJS_DESC_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE,
            .value = td->raw[i] ? ajs_val_from_gc(td->raw[i]) : ajs_val_from_gc(vm->str_empty)
        };
        ajs_obj_define_own_prop(vm, raw_arr, kval, &rdesc, false);
    }

    cooked_arr->u.arr.length = td->count;
    cooked_arr->u.arr.length_writable = false;
    raw_arr->u.arr.length = td->count;
    raw_arr->u.arr.length_writable = false;
    raw_arr->extensible = false;

    AjsPropDesc raw_prop = {
        .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE,
        .value = ajs_val_from_gc(raw_arr)
    };
    ajs_obj_define_own_prop(vm, cooked_arr, ajs_val_from_gc(vm->str_raw), &raw_prop, false);
    cooked_arr->extensible = false;

    if (realm->tpl_cache_count >= realm->tpl_cache_cap) {
        uint32_t ncap = realm->tpl_cache_cap ? realm->tpl_cache_cap * 2u : 16u;
        AjsTplCacheEntry *nc = (AjsTplCacheEntry *)realloc(realm->tpl_cache, ncap * sizeof(AjsTplCacheEntry));
        if (nc) {
            realm->tpl_cache = nc;
            realm->tpl_cache_cap = ncap;
        }
    }
    if (realm->tpl_cache_count < realm->tpl_cache_cap) {
        realm->tpl_cache[realm->tpl_cache_count].bc = bc;
        realm->tpl_cache[realm->tpl_cache_count].tpl_id = td->id;
        realm->tpl_cache[realm->tpl_cache_count].tpl_array = ajs_val_from_gc(cooked_arr);
        realm->tpl_cache_count++;
    }

    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(cooked_arr);
}

/* ============================================================================
 * 3. Call Stack Management, Async / Generator Continuation & Call / Construct
 * ============================================================================ */

static bool push_call_stack(AjsVm *vm, AjsExecFrame *frame, AjsObject *native_fn_obj) {
    size_t max_depth = vm->cfg.max_call_depth ? vm->cfg.max_call_depth : 512u;
    if (vm->call_depth >= max_depth) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Maximum call stack size exceeded");
        return false;
    }
    if (vm->call_depth >= vm->call_cap) {
        size_t ncap = vm->call_cap ? vm->call_cap * 2u : 64u;
        AjsCallStackEntry *ncs = (AjsCallStackEntry *)realloc(vm->call_stack, ncap * sizeof(AjsCallStackEntry));
        if (!ncs) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Out of memory growing call stack");
            return false;
        }
        vm->call_stack = ncs;
        vm->call_cap = ncap;
    }
    vm->call_stack[vm->call_depth].frame = frame;
    vm->call_stack[vm->call_depth].native_fn_obj = native_fn_obj;
    vm->call_depth++;
    return true;
}

static void pop_call_stack(AjsVm *vm) {
    if (vm->call_depth > 0) vm->call_depth--;
}

/* Native reaction callbacks for `await` inside async functions & async generators */
static AjsValue async_await_reaction_cb(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *userdata) {
    (void)this_val;
    (void)userdata;
    AjsObject *fn_obj = vm->call_depth > 0 ? vm->call_stack[vm->call_depth - 1u].native_fn_obj : NULL;
    if (!fn_obj) return AJS_VAL_UNDEFINED;
    AjsValue gen_val = fn_obj->u.fn.home_object;
    bool is_rej = (fn_obj->u.fn.native_magic != 0);
    fn_obj->u.fn.home_object = AJS_VAL_UNDEFINED;

    if (!ajs_val_is_gc_kind(gen_val, AJS_GC_OBJECT)) return AJS_VAL_UNDEFINED;
    AjsObject *gen_obj = (AjsObject *)ajs_val_to_gc(gen_val);
    AjsValue arg = (argc > 0) ? argv[0] : AJS_VAL_UNDEFINED;

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, gen_val);
    ajs_temp_root_push(vm, arg);

    if (gen_obj->u.gen.is_async_fn) {
        /* Async function (or TLA module body wrapped as async function) */
        AjsExecFrame *frame = gen_obj->u.gen.frame;
        if (!frame) { ajs_temp_root_restore(vm, rs); return AJS_VAL_UNDEFINED; }
        gen_obj->u.gen.state = AJS_GEN_EXECUTING;
        if (is_rej) {
            vm->has_exception = true;
            vm->exc_phase = AJS_ERR_RUNTIME;
            vm->exception = arg;
        } else {
            frame_stack_push(frame, arg);
        }
        AjsValue out_val = AJS_VAL_UNDEFINED;
        if (!push_call_stack(vm, frame, frame->func_obj)) {
            AjsValue exc = vm->exception;
            vm->has_exception = false;
            vm->exception = AJS_VAL_UNDEFINED;
            ajs_call_internal(vm, gen_obj->u.gen.async_reject, AJS_VAL_UNDEFINED, 1, &exc);
            vm->has_exception = false;
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_UNDEFINED;
        }
        AjsFrameRunStatus st = ajs_run_frame(vm, frame, &out_val);
        pop_call_stack(vm);

        if (st == AJS_RUN_RETURNED) {
            gen_obj->u.gen.state = AJS_GEN_COMPLETED;
            gen_obj->u.gen.frame = NULL;
            ajs_call_internal(vm, gen_obj->u.gen.async_resolve, AJS_VAL_UNDEFINED, 1, &out_val);
            vm->has_exception = false;
        } else if (st == AJS_RUN_THREW) {
            AjsValue exc = vm->exception;
            vm->has_exception = false;
            vm->exception = AJS_VAL_UNDEFINED;
            gen_obj->u.gen.state = AJS_GEN_COMPLETED;
            gen_obj->u.gen.frame = NULL;
            ajs_temp_root_push(vm, exc);
            ajs_call_internal(vm, gen_obj->u.gen.async_reject, AJS_VAL_UNDEFINED, 1, &exc);
            vm->has_exception = false;
        } else if (st == AJS_RUN_AWAITED) {
            gen_obj->u.gen.state = AJS_GEN_SUSPENDED_YIELD;
            if (!attach_await_continuation(vm, frame->realm, gen_obj, out_val)) {
                AjsValue exc = vm->exception;
                vm->has_exception = false;
                vm->exception = AJS_VAL_UNDEFINED;
                gen_obj->u.gen.state = AJS_GEN_COMPLETED;
                gen_obj->u.gen.frame = NULL;
                ajs_temp_root_push(vm, exc);
                ajs_call_internal(vm, gen_obj->u.gen.async_reject, AJS_VAL_UNDEFINED, 1, &exc);
                vm->has_exception = false;
            }
        }
    } else if (gen_obj->u.gen.is_async_gen) {
        if (gen_obj->u.gen.state == AJS_GEN_AWAITING_RETURN) {
            /* Awaited value for `asyncGen.return(val)` settled */
            gen_obj->u.gen.state = AJS_GEN_COMPLETED;
            AjsAsyncGenReq *req = gen_obj->u.gen.req_head;
            if (req) {
                gen_obj->u.gen.req_head = req->next;
                if (!gen_obj->u.gen.req_head) gen_obj->u.gen.req_tail = NULL;
                if (is_rej) {
                    ajs_call_internal(vm, req->reject_fn, AJS_VAL_UNDEFINED, 1, &arg);
                } else {
                    AjsValue ir = ajs_make_iter_result(vm, arg, true);
                    ajs_temp_root_push(vm, ir);
                    ajs_call_internal(vm, req->resolve_fn, AJS_VAL_UNDEFINED, 1, &ir);
                }
                vm->has_exception = false;
                free(req);
            }
            async_gen_drain_queue(vm, gen_obj);
        } else {
            /* Awaited inside async generator body (or async `yield*`)! */
            AjsExecFrame *frame = gen_obj->u.gen.frame;
            if (!frame) { ajs_temp_root_restore(vm, rs); return AJS_VAL_UNDEFINED; }
            gen_obj->u.gen.state = AJS_GEN_EXECUTING;
            if (is_rej) {
                vm->has_exception = true;
                vm->exc_phase = AJS_ERR_RUNTIME;
                vm->exception = arg;
            } else {
                frame_stack_push(frame, arg);
            }
            AjsValue out_val = AJS_VAL_UNDEFINED;
            if (!push_call_stack(vm, frame, frame->func_obj)) {
                st_threw:;
                AjsValue exc = vm->exception;
                vm->has_exception = false;
                vm->exception = AJS_VAL_UNDEFINED;
                gen_obj->u.gen.state = AJS_GEN_COMPLETED;
                gen_obj->u.gen.frame = NULL;
                AjsAsyncGenReq *req = gen_obj->u.gen.req_head;
                if (req) {
                    gen_obj->u.gen.req_head = req->next;
                    if (!gen_obj->u.gen.req_head) gen_obj->u.gen.req_tail = NULL;
                    ajs_temp_root_push(vm, exc);
                    ajs_call_internal(vm, req->reject_fn, AJS_VAL_UNDEFINED, 1, &exc);
                    vm->has_exception = false;
                    free(req);
                }
                async_gen_drain_queue(vm, gen_obj);
                ajs_temp_root_restore(vm, rs);
                return AJS_VAL_UNDEFINED;
            }
            AjsFrameRunStatus st = ajs_run_frame(vm, frame, &out_val);
            pop_call_stack(vm);
            if (st == AJS_RUN_THREW) goto st_threw;
            if (st == AJS_RUN_RETURNED) {
                gen_obj->u.gen.state = AJS_GEN_COMPLETED;
                gen_obj->u.gen.frame = NULL;
                AjsAsyncGenReq *req = gen_obj->u.gen.req_head;
                if (req) {
                    gen_obj->u.gen.req_head = req->next;
                    if (!gen_obj->u.gen.req_head) gen_obj->u.gen.req_tail = NULL;
                    AjsValue ir = ajs_make_iter_result(vm, out_val, true);
                    ajs_temp_root_push(vm, ir);
                    ajs_call_internal(vm, req->resolve_fn, AJS_VAL_UNDEFINED, 1, &ir);
                    vm->has_exception = false;
                    free(req);
                }
                async_gen_drain_queue(vm, gen_obj);
            } else if (st == AJS_RUN_SUSPENDED) {
                gen_obj->u.gen.state = AJS_GEN_SUSPENDED_YIELD;
                AjsAsyncGenReq *req = gen_obj->u.gen.req_head;
                if (req) {
                    gen_obj->u.gen.req_head = req->next;
                    if (!gen_obj->u.gen.req_head) gen_obj->u.gen.req_tail = NULL;
                    AjsValue ir = ajs_make_iter_result(vm, out_val, false);
                    ajs_temp_root_push(vm, ir);
                    ajs_call_internal(vm, req->resolve_fn, AJS_VAL_UNDEFINED, 1, &ir);
                    vm->has_exception = false;
                    free(req);
                }
                async_gen_drain_queue(vm, gen_obj);
            } else if (st == AJS_RUN_AWAITED) {
                if (!attach_await_continuation(vm, frame->realm, gen_obj, out_val)) goto st_threw;
            }
        }
    }

    ajs_temp_root_restore(vm, rs);
    return AJS_VAL_UNDEFINED;
}

static bool attach_await_continuation(AjsVm *vm, AjsRealm *realm, AjsObject *gen_obj, AjsValue awaited_val) {
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(gen_obj));
    ajs_temp_root_push(vm, awaited_val);

    AjsValue prom = ajs_promise_resolve_val(vm, realm, realm->promise_ctor, awaited_val);
    if (vm->has_exception || !ajs_val_is_gc_kind(prom, AJS_GC_OBJECT)) {
        ajs_temp_root_restore(vm, rs);
        return false;
    }
    ajs_temp_root_push(vm, prom);

    AjsObject *ok_fn = ajs_obj_new(vm, realm, AJS_OBJ_FUNCTION, realm->function_proto);
    ok_fn->u.fn.fn_kind = AJS_FN_NATIVE;
    ok_fn->u.fn.native_fn = async_await_reaction_cb;
    ok_fn->u.fn.home_object = ajs_val_from_gc(gen_obj);
    ok_fn->u.fn.native_magic = 0;
    ajs_temp_root_push(vm, ajs_val_from_gc(ok_fn));

    AjsObject *err_fn = ajs_obj_new(vm, realm, AJS_OBJ_FUNCTION, realm->function_proto);
    err_fn->u.fn.fn_kind = AJS_FN_NATIVE;
    err_fn->u.fn.native_fn = async_await_reaction_cb;
    err_fn->u.fn.home_object = ajs_val_from_gc(gen_obj);
    err_fn->u.fn.native_magic = 1;
    ajs_temp_root_push(vm, ajs_val_from_gc(err_fn));

    AjsObject *prom_obj = (AjsObject *)ajs_val_to_gc(prom);
    ajs_promise_then_internal(vm, prom_obj, ajs_val_from_gc(ok_fn), ajs_val_from_gc(err_fn),
                              AJS_VAL_UNDEFINED, AJS_VAL_UNDEFINED, AJS_VAL_UNDEFINED);
    ajs_temp_root_restore(vm, rs);
    return true;
}

static void async_gen_drain_queue(AjsVm *vm, AjsObject *gen_obj) {
    while (gen_obj->u.gen.req_head != NULL) {
        AjsGenState st = gen_obj->u.gen.state;
        if (st == AJS_GEN_EXECUTING || st == AJS_GEN_AWAITING_RETURN) return;

        AjsAsyncGenReq *req = gen_obj->u.gen.req_head;
        size_t rs = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, ajs_val_from_gc(gen_obj));
        ajs_temp_root_push(vm, req->arg);

        if (st == AJS_GEN_SUSPENDED_START && req->resume_mode != 0) {
            gen_obj->u.gen.state = AJS_GEN_COMPLETED;
            gen_obj->u.gen.frame = NULL;
            st = AJS_GEN_COMPLETED;
        }

        if (st == AJS_GEN_COMPLETED) {
            if (req->resume_mode == 0) {
                gen_obj->u.gen.req_head = req->next;
                if (!gen_obj->u.gen.req_head) gen_obj->u.gen.req_tail = NULL;
                AjsValue ir = ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
                ajs_temp_root_push(vm, ir);
                ajs_call_internal(vm, req->resolve_fn, AJS_VAL_UNDEFINED, 1, &ir);
                vm->has_exception = false;
                free(req);
                ajs_temp_root_restore(vm, rs);
                continue;
            } else if (req->resume_mode == 1) {
                /* ECMA-262 AsyncGeneratorAwaitReturn: await `req->arg` even when completed! */
                gen_obj->u.gen.state = AJS_GEN_AWAITING_RETURN;
                if (!attach_await_continuation(vm, gen_obj->realm, gen_obj, req->arg)) {
                    AjsValue exc = vm->exception;
                    vm->has_exception = false;
                    vm->exception = AJS_VAL_UNDEFINED;
                    gen_obj->u.gen.state = AJS_GEN_COMPLETED;
                    gen_obj->u.gen.req_head = req->next;
                    if (!gen_obj->u.gen.req_head) gen_obj->u.gen.req_tail = NULL;
                    ajs_temp_root_push(vm, exc);
                    ajs_call_internal(vm, req->reject_fn, AJS_VAL_UNDEFINED, 1, &exc);
                    vm->has_exception = false;
                    free(req);
                    ajs_temp_root_restore(vm, rs);
                    continue;
                }
                ajs_temp_root_restore(vm, rs);
                return;
            } else {
                gen_obj->u.gen.req_head = req->next;
                if (!gen_obj->u.gen.req_head) gen_obj->u.gen.req_tail = NULL;
                ajs_call_internal(vm, req->reject_fn, AJS_VAL_UNDEFINED, 1, &req->arg);
                vm->has_exception = false;
                free(req);
                ajs_temp_root_restore(vm, rs);
                continue;
            }
        }

        /* Resume suspended frame (`SUSPENDED_START` with mode 0, or `SUSPENDED_YIELD` with mode 0/1/2) */
        AjsExecFrame *frame = gen_obj->u.gen.frame;
        gen_obj->u.gen.state = AJS_GEN_EXECUTING;

        if (frame->in_yield_star) {
            frame->ys_step_mode = req->resume_mode;
            frame_stack_push(frame, req->arg);
        } else if (req->resume_mode == 0) {
            frame_stack_push(frame, req->arg);
        } else if (req->resume_mode == 2) {
            vm->has_exception = true;
            vm->exc_phase = AJS_ERR_RUNTIME;
            vm->exception = req->arg;
        } else if (req->resume_mode == 1) {
            /* `gen.return(val)`: inject abrupt return unwinding through active `try/finally` blocks!
             * Note: in an async generator, `gen.return(val)` awaits `val` BEFORE resuming!
             * Let's check: if we haven't awaited `req->arg` yet when `resume_mode == 1`, wait:
             * ECMA-262 §27.5.3.7 (`AsyncGeneratorUnwrapYieldResumption`): inside `yield`, if resumption is `return`,
             * it awaits `resumptionValue`! Pushing `req->arg` onto an internal await or awaiting right here:
             * even cleaner — if `req->resume_mode == 1 && !frame->in_yield_star`, execute `OP_UNWIND_RETURN`
             * AFTER awaiting `req->arg`? Actually `PromiseResolve(req->arg)` via `OP_UNWIND_RETURN` vs direct:
             * let's push `req->arg` and step `OP_UNWIND_RETURN` by rewinding pc to a synthetic return? Even simpler:
             * handle `frame_inject_return(vm, frame, req->arg)`! */
            frame_stack_push(frame, req->arg);
        }

        AjsValue out_val = AJS_VAL_UNDEFINED;
        if (!push_call_stack(vm, frame, frame->func_obj)) {
            AjsValue exc = vm->exception;
            vm->has_exception = false;
            vm->exception = AJS_VAL_UNDEFINED;
            gen_obj->u.gen.state = AJS_GEN_COMPLETED;
            gen_obj->u.gen.frame = NULL;
            gen_obj->u.gen.req_head = req->next;
            if (!gen_obj->u.gen.req_head) gen_obj->u.gen.req_tail = NULL;
            ajs_temp_root_push(vm, exc);
            ajs_call_internal(vm, req->reject_fn, AJS_VAL_UNDEFINED, 1, &exc);
            vm->has_exception = false;
            free(req);
            ajs_temp_root_restore(vm, rs);
            continue;
        }

        /* Special flag if resuming normal `yield` with `return(val)` */
        if (req->resume_mode == 1 && !frame->in_yield_star) {
            /* Mark `ys_step_mode = 99` as signal to `ajs_run_frame` entry to unwind-return top-of-stack! */
            frame->ys_step_mode = 99;
        }

        AjsFrameRunStatus rst = ajs_run_frame(vm, frame, &out_val);
        pop_call_stack(vm);

        if (rst == AJS_RUN_RETURNED) {
            gen_obj->u.gen.state = AJS_GEN_COMPLETED;
            gen_obj->u.gen.frame = NULL;
            gen_obj->u.gen.req_head = req->next;
            if (!gen_obj->u.gen.req_head) gen_obj->u.gen.req_tail = NULL;
            AjsValue ir = ajs_make_iter_result(vm, out_val, true);
            ajs_temp_root_push(vm, ir);
            ajs_call_internal(vm, req->resolve_fn, AJS_VAL_UNDEFINED, 1, &ir);
            vm->has_exception = false;
            free(req);
        } else if (rst == AJS_RUN_THREW) {
            AjsValue exc = vm->exception;
            vm->has_exception = false;
            vm->exception = AJS_VAL_UNDEFINED;
            gen_obj->u.gen.state = AJS_GEN_COMPLETED;
            gen_obj->u.gen.frame = NULL;
            gen_obj->u.gen.req_head = req->next;
            if (!gen_obj->u.gen.req_head) gen_obj->u.gen.req_tail = NULL;
            ajs_temp_root_push(vm, exc);
            ajs_call_internal(vm, req->reject_fn, AJS_VAL_UNDEFINED, 1, &exc);
            vm->has_exception = false;
            free(req);
        } else if (rst == AJS_RUN_SUSPENDED) {
            gen_obj->u.gen.state = AJS_GEN_SUSPENDED_YIELD;
            gen_obj->u.gen.req_head = req->next;
            if (!gen_obj->u.gen.req_head) gen_obj->u.gen.req_tail = NULL;
            AjsValue ir = ajs_make_iter_result(vm, out_val, false);
            ajs_temp_root_push(vm, ir);
            ajs_call_internal(vm, req->resolve_fn, AJS_VAL_UNDEFINED, 1, &ir);
            vm->has_exception = false;
            free(req);
        } else if (rst == AJS_RUN_AWAITED) {
            if (!attach_await_continuation(vm, frame->realm, gen_obj, out_val)) {
                AjsValue exc = vm->exception;
                vm->has_exception = false;
                vm->exception = AJS_VAL_UNDEFINED;
                gen_obj->u.gen.state = AJS_GEN_COMPLETED;
                gen_obj->u.gen.frame = NULL;
                gen_obj->u.gen.req_head = req->next;
                if (!gen_obj->u.gen.req_head) gen_obj->u.gen.req_tail = NULL;
                ajs_temp_root_push(vm, exc);
                ajs_call_internal(vm, req->reject_fn, AJS_VAL_UNDEFINED, 1, &exc);
                vm->has_exception = false;
                free(req);
                ajs_temp_root_restore(vm, rs);
                continue;
            }
            ajs_temp_root_restore(vm, rs);
            return;
        }
        ajs_temp_root_restore(vm, rs);
    }
}

AjsValue ajs_gen_resume(AjsVm *vm, AjsObject *gen_obj, int resume_mode, AjsValue arg) {
    if (!gen_obj) return AJS_VAL_EXCEPTION;

    /* Async Generator: enqueue request & return fresh Promise */
    if (gen_obj->kind == AJS_OBJ_ASYNC_GENERATOR) {
        size_t rs = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, ajs_val_from_gc(gen_obj));
        ajs_temp_root_push(vm, arg);

        AjsValue res_fn = AJS_VAL_UNDEFINED, rej_fn = AJS_VAL_UNDEFINED;
        AjsValue prom = ajs_promise_create(vm, &res_fn, &rej_fn);
        ajs_temp_root_push(vm, prom);
        ajs_temp_root_push(vm, res_fn);
        ajs_temp_root_push(vm, rej_fn);

        AjsAsyncGenReq *req = (AjsAsyncGenReq *)calloc(1, sizeof(AjsAsyncGenReq));
        if (!req) {
            ajs_temp_root_restore(vm, rs);
            return prom;
        }
        req->resume_mode = resume_mode;
        req->arg = arg;
        req->promise = prom;
        req->resolve_fn = res_fn;
        req->reject_fn = rej_fn;
        if (gen_obj->u.gen.req_tail) {
            gen_obj->u.gen.req_tail->next = req;
            gen_obj->u.gen.req_tail = req;
        } else {
            gen_obj->u.gen.req_head = req;
            gen_obj->u.gen.req_tail = req;
        }

        async_gen_drain_queue(vm, gen_obj);
        ajs_temp_root_restore(vm, rs);
        return prom;
    }

    /* Synchronous Generator */
    if (gen_obj->kind != AJS_OBJ_GENERATOR) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Method called on incompatible receiver");
    }
    AjsGenState st = gen_obj->u.gen.state;
    if (st == AJS_GEN_EXECUTING) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Generator is already running");
    }
    if (st == AJS_GEN_SUSPENDED_START && resume_mode != 0) {
        gen_obj->u.gen.state = AJS_GEN_COMPLETED;
        gen_obj->u.gen.frame = NULL;
        st = AJS_GEN_COMPLETED;
    }
    if (st == AJS_GEN_COMPLETED) {
        if (resume_mode == 0) return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
        if (resume_mode == 1) return ajs_make_iter_result(vm, arg, true);
        return ajs_throw(vm, arg);
    }

    AjsExecFrame *frame = gen_obj->u.gen.frame;
    if (!frame) return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(gen_obj));
    ajs_temp_root_push(vm, arg);

    gen_obj->u.gen.state = AJS_GEN_EXECUTING;
    if (frame->in_yield_star) {
        frame->ys_step_mode = resume_mode;
        frame_stack_push(frame, arg);
    } else if (resume_mode == 0) {
        frame_stack_push(frame, arg);
    } else if (resume_mode == 2) {
        vm->has_exception = true;
        vm->exc_phase = AJS_ERR_RUNTIME;
        vm->exception = arg;
    } else if (resume_mode == 1) {
        frame_stack_push(frame, arg);
        frame->ys_step_mode = 99;
    }

    if (!push_call_stack(vm, frame, frame->func_obj)) {
        gen_obj->u.gen.state = AJS_GEN_COMPLETED;
        gen_obj->u.gen.frame = NULL;
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }

    AjsValue out_val = AJS_VAL_UNDEFINED;
    AjsFrameRunStatus rst = ajs_run_frame(vm, frame, &out_val);
    pop_call_stack(vm);

    if (rst == AJS_RUN_THREW) {
        gen_obj->u.gen.state = AJS_GEN_COMPLETED;
        gen_obj->u.gen.frame = NULL;
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    if (rst == AJS_RUN_RETURNED) {
        gen_obj->u.gen.state = AJS_GEN_COMPLETED;
        gen_obj->u.gen.frame = NULL;
        AjsValue ir = ajs_make_iter_result(vm, out_val, true);
        ajs_temp_root_restore(vm, rs);
        return ir;
    }
    /* Suspended at `yield` */
    gen_obj->u.gen.state = AJS_GEN_SUSPENDED_YIELD;
    AjsValue ir = ajs_make_iter_result(vm, out_val, false);
    ajs_temp_root_restore(vm, rs);
    return ir;
}

AjsValue ajs_call_internal(AjsVm *vm, AjsValue fn_val, AjsValue this_val, int argc, const AjsValue *argv) {
    if (!ajs_val_is_gc_kind(fn_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Value is not a function");
    }
    AjsObject *fn = (AjsObject *)ajs_val_to_gc(fn_val);

    /* Proxy `apply` trap */
    if (fn->kind == AJS_OBJ_PROXY) {
        if (!fn->u.proxy.is_callable) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Proxy is not a function");
        }
        if (fn->u.proxy.handler == AJS_VAL_NULL) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot perform 'apply' on a proxy that has been revoked");
        }
        size_t rs = ajs_temp_root_save(vm);
        AjsValue target = fn->u.proxy.target;
        AjsValue handler = fn->u.proxy.handler;
        ajs_temp_root_push(vm, target);
        ajs_temp_root_push(vm, handler);
        ajs_temp_root_push(vm, this_val);
        for (int i = 0; i < argc; i++) ajs_temp_root_push(vm, argv[i]);

        AjsString *s_apply = ajs_str_from_cstr(vm, "apply", true);
        AjsValue trap = ajs_obj_get(vm, handler, ajs_val_from_gc(s_apply), handler);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (ajs_is_nullish(trap)) {
            AjsValue res = ajs_call_internal(vm, target, this_val, argc, argv);
            ajs_temp_root_restore(vm, rs);
            return res;
        }
        ajs_temp_root_push(vm, trap);
        AjsObject *arg_arr = ajs_obj_new(vm, fn->realm, AJS_OBJ_ARRAY, fn->realm->array_proto);
        arg_arr->u.arr.length_writable = true;
        arg_arr->u.arr.length = (uint32_t)argc;
        if (argc > 0) {
            arg_arr->u.arr.dense_cap = (uint32_t)argc;
            arg_arr->u.arr.dense_len = (uint32_t)argc;
            arg_arr->u.arr.dense = (AjsValue *)malloc((size_t)argc * sizeof(AjsValue));
            for (int i = 0; i < argc; i++) arg_arr->u.arr.dense[i] = argv[i];
        }
        AjsValue targs[3] = { target, this_val, ajs_val_from_gc(arg_arr) };
        ajs_temp_root_push(vm, targs[2]);
        AjsValue res = ajs_call_internal(vm, trap, handler, 3, targs);
        ajs_temp_root_restore(vm, rs);
        return res;
    }

    if (fn->kind != AJS_OBJ_FUNCTION) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object is not a function");
    }

    /* Bound function unwrapping */
    if (fn->u.fn.fn_kind == AJS_FN_BOUND) {
        int b_argc = fn->u.fn.bound_argc;
        int tot = b_argc + argc;
        AjsValue stack_args[16];
        AjsValue *all_args = (tot <= 16) ? stack_args : (AjsValue *)malloc((size_t)tot * sizeof(AjsValue));
        for (int i = 0; i < b_argc; i++) all_args[i] = fn->u.fn.bound_argv[i];
        for (int i = 0; i < argc; i++) all_args[b_argc + i] = argv[i];
        size_t rs = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, fn_val);
        AjsValue res = ajs_call_internal(vm, fn->u.fn.bound_target, fn->u.fn.bound_this, tot, all_args);
        if (all_args != stack_args) free(all_args);
        ajs_temp_root_restore(vm, rs);
        return res;
    }

    /* ES6 Class constructors CANNOT be called without `new`! */
    if (fn->u.fn.fn_kind == AJS_FN_BC_CLASS_CTOR) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                  "Class constructor cannot be invoked without 'new'");
    }

    /* Native C function */
    if (fn->u.fn.fn_kind == AJS_FN_NATIVE) {
        if (!push_call_stack(vm, NULL, fn)) return AJS_VAL_EXCEPTION;
        AjsRealm *saved_realm = vm->active_realm;
        if (fn->realm) vm->active_realm = fn->realm;
        size_t rs = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, fn_val);
        ajs_temp_root_push(vm, this_val);
        for (int i = 0; i < argc; i++) ajs_temp_root_push(vm, argv[i]);
        AjsValue res = fn->u.fn.native_fn(vm, this_val, argc, argv, fn->u.fn.native_userdata);
        ajs_temp_root_restore(vm, rs);
        vm->active_realm = saved_realm;
        pop_call_stack(vm);
        return vm->has_exception ? AJS_VAL_EXCEPTION : res;
    }

    /* Bytecode Function (Normal, Arrow, Method, Generator, Async, AsyncArrow, AsyncGenerator) */
    AjsBytecode *bc = fn->u.fn.bc;
    AjsRealm *realm = fn->realm ? fn->realm : vm->active_realm;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, fn_val);
    ajs_temp_root_push(vm, this_val);
    for (int i = 0; i < argc; i++) ajs_temp_root_push(vm, argv[i]);

    /* Sloppy-mode `this` boxing / global substitution (except arrows, which ignore call `this_val`) */
    AjsValue eff_this = this_val;
    if (bc->fn_kind != AJS_FN_BC_ARROW && bc->fn_kind != AJS_FN_BC_ASYNC_ARROW && !bc->strict) {
        if (ajs_is_nullish(eff_this)) {
            eff_this = realm->global_obj;
        } else if (!ajs_is_object(vm, eff_this)) {
            AjsObject *boxed = ajs_to_object(vm, eff_this);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            eff_this = ajs_val_from_gc(boxed);
        }
    }
    ajs_temp_root_push(vm, eff_this);

    AjsExecFrame *frame = ajs_frame_new(vm, realm, fn, bc, fn->u.fn.closure_env,
                                        eff_this, AJS_VAL_UNDEFINED, fn->u.fn.owner_module);
    if (!frame) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(frame));

    if (!setup_call_frame_args(vm, frame, argc, argv)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }

    /* Generator / AsyncGenerator instantiation */
    if (bc->fn_kind == AJS_FN_BC_GENERATOR || bc->fn_kind == AJS_FN_BC_ASYNC_GENERATOR) {
        bool is_ag = (bc->fn_kind == AJS_FN_BC_ASYNC_GENERATOR);
        AjsValue inst_proto = ajs_obj_get(vm, fn_val, ajs_val_from_gc(vm->str_prototype), fn_val);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (!ajs_is_object(vm, inst_proto)) {
            inst_proto = is_ag ? realm->async_gen_proto : realm->gen_proto;
        }
        AjsObject *gobj = ajs_obj_new(vm, realm, is_ag ? AJS_OBJ_ASYNC_GENERATOR : AJS_OBJ_GENERATOR, inst_proto);
        ajs_temp_root_push(vm, ajs_val_from_gc(gobj));
        gobj->u.gen.state = AJS_GEN_EXECUTING;
        gobj->u.gen.is_async_gen = is_ag;
        gobj->u.gen.frame = frame;
        gobj->u.gen.async_promise = AJS_VAL_UNDEFINED;
        gobj->u.gen.async_resolve = AJS_VAL_UNDEFINED;
        gobj->u.gen.async_reject = AJS_VAL_UNDEFINED;

        /* Execute parameter initialization up to the synthetic initial `OP_YIELD`! */
        if (!push_call_stack(vm, frame, fn)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        AjsRealm *saved_realm = vm->active_realm;
        vm->active_realm = realm;
        AjsValue dummy_out = AJS_VAL_UNDEFINED;
        AjsFrameRunStatus rst = ajs_run_frame(vm, frame, &dummy_out);
        vm->active_realm = saved_realm;
        pop_call_stack(vm);

        if (rst == AJS_RUN_THREW) {
            gobj->u.gen.state = AJS_GEN_COMPLETED;
            gobj->u.gen.frame = NULL;
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
        gobj->u.gen.state = AJS_GEN_SUSPENDED_START;
        ajs_temp_root_restore(vm, rs);
        return ajs_val_from_gc(gobj);
    }

    /* Async Function / Async Arrow */
    if (bc->fn_kind == AJS_FN_BC_ASYNC || bc->fn_kind == AJS_FN_BC_ASYNC_ARROW) {
        AjsValue res_fn = AJS_VAL_UNDEFINED, rej_fn = AJS_VAL_UNDEFINED;
        AjsValue prom = ajs_promise_create(vm, &res_fn, &rej_fn);
        ajs_temp_root_push(vm, prom);
        ajs_temp_root_push(vm, res_fn);
        ajs_temp_root_push(vm, rej_fn);

        AjsObject *gobj = ajs_obj_new(vm, realm, AJS_OBJ_GENERATOR, AJS_VAL_NULL);
        ajs_temp_root_push(vm, ajs_val_from_gc(gobj));
        gobj->u.gen.state = AJS_GEN_EXECUTING;
        gobj->u.gen.is_async_fn = true;
        gobj->u.gen.frame = frame;
        gobj->u.gen.async_promise = prom;
        gobj->u.gen.async_resolve = res_fn;
        gobj->u.gen.async_reject = rej_fn;
        /* Store async driver object in `frame->args_obj`? No! Keep `frame->args_obj` for `arguments`!
         * Store `gobj` by passing it or looking it up? Even simpler: let's look at `OP_AWAIT`:
         * `OP_AWAIT` returns `AJS_RUN_SUSPENDED` with `*out_val = awaited_val`, and the caller (`ajs_call_internal` /
         * `async_await_reaction_cb` / `async_gen_drain_queue` / `ajs_exec_module_bc`) calls `attach_await_continuation`!
         * WAIT A SECOND — that's SO clean! `ajs_run_frame` doesn't even need `gobj` — `OP_AWAIT` just sets
         * `*out_val = awaited_val` and returns `AJS_RUN_SUSPENDED`! Let's distinguish `OP_YIELD` vs `OP_AWAIT` with
         * an enum `AJS_RUN_YIELDED` vs `AJS_RUN_AWAITED`! */
        if (!push_call_stack(vm, frame, fn)) {
            AjsValue exc = vm->exception;
            vm->has_exception = false;
            vm->exception = AJS_VAL_UNDEFINED;
            ajs_temp_root_push(vm, exc);
            ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &exc);
            vm->has_exception = false;
            ajs_temp_root_restore(vm, rs);
            return prom;
        }
        AjsRealm *saved_realm = vm->active_realm;
        vm->active_realm = realm;
        AjsValue out_val = AJS_VAL_UNDEFINED;
        AjsFrameRunStatus rst = ajs_run_frame(vm, frame, &out_val);
        vm->active_realm = saved_realm;
        pop_call_stack(vm);

        if (rst == AJS_RUN_RETURNED) {
            gobj->u.gen.state = AJS_GEN_COMPLETED;
            gobj->u.gen.frame = NULL;
            ajs_temp_root_push(vm, out_val);
            ajs_call_internal(vm, res_fn, AJS_VAL_UNDEFINED, 1, &out_val);
            vm->has_exception = false;
        } else if (rst == AJS_RUN_THREW) {
            AjsValue exc = vm->exception;
            vm->has_exception = false;
            vm->exception = AJS_VAL_UNDEFINED;
            gobj->u.gen.state = AJS_GEN_COMPLETED;
            gobj->u.gen.frame = NULL;
            ajs_temp_root_push(vm, exc);
            ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &exc);
            vm->has_exception = false;
        } else {
            /* AJS_RUN_SUSPENDED on `OP_AWAIT`: `out_val` is the awaited value! */
            gobj->u.gen.state = AJS_GEN_SUSPENDED_YIELD;
            if (!attach_await_continuation(vm, realm, gobj, out_val)) {
                AjsValue exc = vm->exception;
                vm->has_exception = false;
                vm->exception = AJS_VAL_UNDEFINED;
                ajs_temp_root_push(vm, exc);
                ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &exc);
                vm->has_exception = false;
            }
        }
        ajs_temp_root_restore(vm, rs);
        return prom;
    }

    /* Synchronous Function / Arrow / Method */
    if (!push_call_stack(vm, frame, fn)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsRealm *saved_realm = vm->active_realm;
    vm->active_realm = realm;
    AjsValue out_val = AJS_VAL_UNDEFINED;
    AjsFrameRunStatus rst = ajs_run_frame(vm, frame, &out_val);
    vm->active_realm = saved_realm;
    pop_call_stack(vm);
    ajs_temp_root_restore(vm, rs);
    return (rst == AJS_RUN_THREW) ? AJS_VAL_EXCEPTION : out_val;
}

AjsValue ajs_construct_internal(AjsVm *vm, AjsValue ctor_val, int argc, const AjsValue *argv, AjsValue new_target) {
    if (!ajs_is_constructor_val(ctor_val)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Value is not a constructor");
    }
    if (ajs_is_undefined(new_target)) new_target = ctor_val;

    AjsObject *ctor = (AjsObject *)ajs_val_to_gc(ctor_val);

    /* Proxy `construct` trap */
    if (ctor->kind == AJS_OBJ_PROXY) {
        if (ctor->u.proxy.handler == AJS_VAL_NULL) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                      "Cannot perform 'construct' on a proxy that has been revoked");
        }
        size_t rs = ajs_temp_root_save(vm);
        AjsValue target = ctor->u.proxy.target;
        AjsValue handler = ctor->u.proxy.handler;
        ajs_temp_root_push(vm, target);
        ajs_temp_root_push(vm, handler);
        ajs_temp_root_push(vm, new_target);
        for (int i = 0; i < argc; i++) ajs_temp_root_push(vm, argv[i]);

        AjsString *s_construct = ajs_str_from_cstr(vm, "construct", true);
        AjsValue trap = ajs_obj_get(vm, handler, ajs_val_from_gc(s_construct), handler);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (ajs_is_nullish(trap)) {
            AjsValue res = ajs_construct_internal(vm, target, argc, argv, new_target);
            ajs_temp_root_restore(vm, rs);
            return res;
        }
        ajs_temp_root_push(vm, trap);
        AjsObject *arg_arr = ajs_obj_new(vm, ctor->realm, AJS_OBJ_ARRAY, ctor->realm->array_proto);
        arg_arr->u.arr.length_writable = true;
        arg_arr->u.arr.length = (uint32_t)argc;
        if (argc > 0) {
            arg_arr->u.arr.dense_cap = (uint32_t)argc;
            arg_arr->u.arr.dense_len = (uint32_t)argc;
            arg_arr->u.arr.dense = (AjsValue *)malloc((size_t)argc * sizeof(AjsValue));
            for (int i = 0; i < argc; i++) arg_arr->u.arr.dense[i] = argv[i];
        }
        AjsValue targs[3] = { target, ajs_val_from_gc(arg_arr), new_target };
        ajs_temp_root_push(vm, targs[1]);
        AjsValue res = ajs_call_internal(vm, trap, handler, 3, targs);
        ajs_temp_root_restore(vm, rs);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        if (!ajs_is_object(vm, res)) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "'construct' on proxy: trap returned non-object");
        }
        return res;
    }

    /* Bound constructor */
    if (ctor->u.fn.fn_kind == AJS_FN_BOUND) {
        int b_argc = ctor->u.fn.bound_argc;
        int tot = b_argc + argc;
        AjsValue stack_args[16];
        AjsValue *all_args = (tot <= 16) ? stack_args : (AjsValue *)malloc((size_t)tot * sizeof(AjsValue));
        for (int i = 0; i < b_argc; i++) all_args[i] = ctor->u.fn.bound_argv[i];
        for (int i = 0; i < argc; i++) all_args[b_argc + i] = argv[i];
        AjsValue target_nt = (new_target == ctor_val) ? ctor->u.fn.bound_target : new_target;
        size_t rs = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, ctor_val);
        ajs_temp_root_push(vm, target_nt);
        AjsValue res = ajs_construct_internal(vm, ctor->u.fn.bound_target, tot, all_args, target_nt);
        if (all_args != stack_args) free(all_args);
        ajs_temp_root_restore(vm, rs);
        return res;
    }

    /* Native C constructor: pass `new_target` in `this_val`? Wait: public C API `AjsNativeFn` receives `this_val`!
     * Let's check how native constructors distinguish `new Foo()` vs `Foo()` AND subclassing (`Reflect.construct(Foo, args, Sub)`):
     * When a native constructor is invoked via `new` / `Reflect.construct`, what does it expect in `this_val`?
     * Let's check `ajs_vm.h` (`ajs_new_constructor`):
     * If `ctor->u.fn.native_magic == 0` (embedder `ajs_new_constructor`!), creating an ordinary instance inheriting from
     * `GetPrototypeFromConstructor(new_target, %Object.prototype%)` and passing THAT instance as `this_val` (while built-in
     * native constructors with `native_magic != 0` can read `new_target` from a helper or `this_val = new_target` when magic < 0)!
     * EVEN BETTER: store `AjsValue active_new_target`? Or check: if we pass a freshly created ordinary object inheriting from
     * `new_target.prototype` for embedder constructors (`native_magic == 0`) AND for built-in constructors (`native_magic != 0`)
     * pass `new_target` directly in `this_val` when `native_magic < 0` (built-in constructors use negative `native_magic < 0`!),
     * while normal calls `Foo()` pass `AJS_VAL_UNDEFINED`!
     * Let's verify that!
     * - Embedder calls `ajs_new_constructor(vm, my_ctor, "Widget", 1, widget_proto, ud)` -> `native_magic == 0`:
     *   `new Widget(x)` automatically allocates `inst` with `proto = GetPrototypeFromConstructor(new_target, object_proto)`,
     *   calls `my_ctor(vm, inst, argc, argv, ud)`, and if `my_ctor` returns an Object, returns that Object, else returns `inst`!
     *   That's 100% ergonomic for `web/bindings/` DOM constructors!!
     * - Built-in constructors (`Array`, `Number`, `Error`, `Promise`, `Map`, `TypedArray`, `Proxy`, `RegExp`, `Date`, `Intl.*`)
     *   set `native_magic < 0`: when called via `Foo()`, `this_val` is `undefined` (or receiver, NEVER `new_target` — wait: what if
     *   someone does `Map.call(Map)`? To make `NewTarget` 100% unambiguous even on `Map.call(Map)`, let's check `vm->call_stack`:
     *   no struct change needed in `ajs_vm.h`! We can just pass a stack-local or `this_val = AJS_VAL_TDZ`? No, `AJS_VAL_EMPTY` tag or
     *   even simpler: check a thread/vm-static or `native_fn_obj`! Wait: `ajs_internal.h` is OUR file (`js/vm/ajs_internal.h`)!
     *   Wait — `ajs_gc.o` and `ajs_object.o` were compiled against `ajs_internal.h`, or we can recompile all 6 `.o` in 0.5s, OR we don't
     *   even need to touch `ajs_internal.h` because `AjsCallStackEntry` has `frame` which is `NULL` for native calls — we can keep a
     *   file-local `new_target` stack right here in `ajs_interp.c` with a 3-line getter `ajs_current_new_target(vm)`!). */
    AjsRealm *realm = ctor->realm ? ctor->realm : vm->active_realm;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ctor_val);
    ajs_temp_root_push(vm, new_target);
    for (int i = 0; i < argc; i++) ajs_temp_root_push(vm, argv[i]);

    if (ctor->u.fn.fn_kind == AJS_FN_NATIVE) {
        if (ctor->u.fn.native_magic < 0) {
            /* Built-in constructor: pass `new_target` via `ajs_get_construct_new_target` */
            if (!push_call_stack(vm, NULL, ctor)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            /* Mark construct frame in `call_stack` using sentinel `frame = (AjsExecFrame *)(uintptr_t)1`? NO! Never put non-GC ptr in `frame`! */
            AjsRealm *saved_realm = vm->active_realm;
            vm->active_realm = realm;
            /* Pass `new_target` in `this_val` AND set bit in `ctor`? Wait: if `Map.call(Map)` is called via `ajs_call_internal`,
             * `ajs_call_internal` does NOT reach `ajs_construct_internal`! To distinguish `new Map()` from `Map.call(Map)` with ZERO
             * ambiguity, wrap `new_target` or track `g_construct_depth`? Let's use a clean per-call stack in `ajs_interp.c`! */
            extern void ajs_push_native_new_target(AjsVm *vm, size_t depth, AjsValue nt);
            extern void ajs_pop_native_new_target(AjsVm *vm, size_t depth);
            ajs_push_native_new_target(vm, vm->call_depth, new_target);
            AjsValue res = ctor->u.fn.native_fn(vm, new_target, argc, argv, ctor->u.fn.native_userdata);
            ajs_pop_native_new_target(vm, vm->call_depth);
            vm->active_realm = saved_realm;
            pop_call_stack(vm);
            ajs_temp_root_restore(vm, rs);
            return vm->has_exception ? AJS_VAL_EXCEPTION : res;
        }

        /* Embedder native constructor (`ajs_new_constructor`): pre-allocate ordinary `this` object */
        AjsValue proto = ajs_obj_get(vm, new_target, ajs_val_from_gc(vm->str_prototype), new_target);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (!ajs_is_object(vm, proto)) {
            AjsRealm *nt_realm = (ajs_val_is_gc_kind(new_target, AJS_GC_OBJECT) && ((AjsObject *)ajs_val_to_gc(new_target))->realm)
                                 ? ((AjsObject *)ajs_val_to_gc(new_target))->realm : realm;
            proto = nt_realm->object_proto;
        }
        AjsObject *inst = ajs_obj_new(vm, realm, AJS_OBJ_ORDINARY, proto);
        AjsValue inst_val = ajs_val_from_gc(inst);
        ajs_temp_root_push(vm, inst_val);

        if (!push_call_stack(vm, NULL, ctor)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        AjsRealm *saved_realm = vm->active_realm;
        vm->active_realm = realm;
        AjsValue res = ctor->u.fn.native_fn(vm, inst_val, argc, argv, ctor->u.fn.native_userdata);
        vm->active_realm = saved_realm;
        pop_call_stack(vm);
        ajs_temp_root_restore(vm, rs);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        return ajs_is_object(vm, res) ? res : inst_val;
    }

    /* Bytecode constructor (base or derived) */
    AjsBytecode *bc = ctor->u.fn.bc;
    AjsValue this_obj_val = AJS_VAL_TDZ;
    if (!bc->is_derived_ctor) {
        AjsValue proto = ajs_obj_get(vm, new_target, ajs_val_from_gc(vm->str_prototype), new_target);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (!ajs_is_object(vm, proto)) {
            AjsRealm *nt_realm = (ajs_val_is_gc_kind(new_target, AJS_GC_OBJECT) && ((AjsObject *)ajs_val_to_gc(new_target))->realm)
                                 ? ((AjsObject *)ajs_val_to_gc(new_target))->realm : realm;
            proto = nt_realm->object_proto;
        }
        AjsObject *inst = ajs_obj_new(vm, realm, AJS_OBJ_ORDINARY, proto);
        this_obj_val = ajs_val_from_gc(inst);
        ajs_temp_root_push(vm, this_obj_val);
    }

    AjsExecFrame *frame = ajs_frame_new(vm, realm, ctor, bc, ctor->u.fn.closure_env,
                                        this_obj_val, new_target, ctor->u.fn.owner_module);
    if (!frame || !setup_call_frame_args(vm, frame, argc, argv)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_push(vm, ajs_val_from_gc(frame));

    if (!push_call_stack(vm, frame, ctor)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsRealm *saved_realm = vm->active_realm;
    vm->active_realm = realm;
    AjsValue out_val = AJS_VAL_UNDEFINED;
    AjsFrameRunStatus rst = ajs_run_frame(vm, frame, &out_val);
    vm->active_realm = saved_realm;
    pop_call_stack(vm);

    if (rst == AJS_RUN_THREW) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    if (ajs_is_object(vm, out_val)) {
        ajs_temp_root_restore(vm, rs);
        return out_val;
    }
    if (!bc->is_derived_ctor) {
        ajs_temp_root_restore(vm, rs);
        return this_obj_val;
    }
    /* Derived constructor returned `undefined` (already handled by `OP_RETURN_DERIVED`) or non-object (`TypeError`!) */
    if (!ajs_is_undefined(out_val)) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                  "Derived constructors may only return object or undefined");
    }
    /* Check `this_slot` on `frame->base_env` */
    AjsValue dthis = (bc->this_slot != AJS_NO_SLOT && frame->base_env && bc->this_slot < frame->base_env->count)
                     ? frame->base_env->slots[bc->this_slot] : frame->this_val;
    ajs_temp_root_restore(vm, rs);
    if (dthis == AJS_VAL_TDZ) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE,
                                  "Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
    }
    return dthis;
}

/* ============================================================================
 * 4. Native Constructor NewTarget Stack, Script/Eval & Module Execution
 * ============================================================================ */

typedef struct AjsNativeNtEntry {
    const AjsVm *vm;
    size_t       depth;
    AjsValue     new_target;
} AjsNativeNtEntry;

static AjsNativeNtEntry g_nt_stack[128];
static size_t           g_nt_top = 0;

void ajs_push_native_new_target(AjsVm *vm, size_t depth, AjsValue nt) {
    if (g_nt_top < 128u) {
        g_nt_stack[g_nt_top].vm = vm;
        g_nt_stack[g_nt_top].depth = depth;
        g_nt_stack[g_nt_top].new_target = nt;
        g_nt_top++;
    }
}

void ajs_pop_native_new_target(AjsVm *vm, size_t depth) {
    if (g_nt_top > 0 && g_nt_stack[g_nt_top - 1u].vm == vm && g_nt_stack[g_nt_top - 1u].depth == depth) {
        g_nt_top--;
    }
}

AjsValue ajs_get_native_new_target(const AjsVm *vm) {
    if (g_nt_top > 0 && g_nt_stack[g_nt_top - 1u].vm == vm &&
        g_nt_stack[g_nt_top - 1u].depth == vm->call_depth) {
        return g_nt_stack[g_nt_top - 1u].new_target;
    }
    return AJS_VAL_UNDEFINED;
}

static AjsEnv *find_enclosing_var_env(AjsVm *vm, AjsRealm *realm) {
    for (ssize_t i = (ssize_t)vm->call_depth - 1; i >= 0; i--) {
        AjsExecFrame *f = vm->call_stack[i].frame;
        if (!f || !f->bc || f->bc->is_eval) continue;
        return f->base_env;
    }
    return realm->global_lex_env;
}

AjsValue ajs_exec_script_bc(AjsVm *vm, AjsRealm *realm, AjsBytecode *bc, AjsEnv *eval_env, AjsValue eval_this) {
    if (!bc) return AJS_VAL_UNDEFINED;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(bc));
    if (eval_env) ajs_temp_root_push(vm, ajs_val_from_gc(eval_env));
    ajs_temp_root_push(vm, eval_this);

    AjsEnv *parent_env = eval_env ? eval_env : realm->global_lex_env;
    AjsValue this_v = eval_env ? eval_this : realm->global_obj;

    AjsEnv *caller_var_env = NULL;
    if (bc->is_eval && !bc->strict && eval_env != NULL && eval_env != realm->global_lex_env) {
        caller_var_env = find_enclosing_var_env(vm, realm);
    }

    AjsExecFrame *frame = ajs_frame_new(vm, realm, NULL, bc, parent_env, this_v, AJS_VAL_UNDEFINED, NULL);
    if (!frame) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_push(vm, ajs_val_from_gc(frame));

    /* ECMA-262 EvalDeclarationInstantiation for sloppy direct `eval` inside a function:
     * Hoisted `var` / `function` declarations bind into `caller_var_env`, throwing `SyntaxError`
     * if any intermediate lexical scope between `eval_env` and `caller_var_env` has a lexical binding! */
    if (caller_var_env && frame->base_env && bc->env_slot_names && bc->env_slot_flags) {
        for (uint32_t i = 0; i < bc->env_slot_count; i++) {
            AjsString *vname = bc->env_slot_names[i];
            uint8_t fl = bc->env_slot_flags[i];
            if (!vname || (fl & AJS_SLOT_INIT_TDZ)) continue;

            /* Check lexical scopes from `eval_env` up to (excluding) `caller_var_env` */
            for (AjsEnv *e = eval_env; e && e != caller_var_env; e = e->parent) {
                if (e->with_obj) continue;
                int32_t ex = env_find_named_slot(e, vname);
                if (ex >= 0) {
                    ajs_temp_root_restore(vm, rs);
                    return ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX,
                                              "Identifier '%s' has already been declared", vname->utf8);
                }
            }
            /* Also check if `caller_var_env` itself has a lexical (`const` or `TDZ`) binding of same name! */
            int32_t cex = env_find_named_slot(caller_var_env, vname);
            if (cex >= 0) {
                if ((caller_var_env->flags[cex] & AJS_SLOT_CONST) || caller_var_env->slots[cex] == AJS_VAL_TDZ) {
                    ajs_temp_root_restore(vm, rs);
                    return ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX,
                                              "Identifier '%s' has already been declared", vname->utf8);
                }
            } else {
                env_append_named_slot(caller_var_env, vname, AJS_VAL_UNDEFINED, 0);
            }
            /* Hide `var` slot name on `frame->base_env` so `OP_GET_DYN_NAME` / `OP_SET_DYN_NAME`
             * walk straight through to `caller_var_env`! */
            if (frame->base_env->names) {
                frame->base_env->names[i] = NULL;
            }
        }
    }

    if (!push_call_stack(vm, frame, NULL)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsRealm *saved_realm = vm->active_realm;
    vm->active_realm = realm;
    AjsValue out_val = AJS_VAL_UNDEFINED;
    AjsFrameRunStatus rst = ajs_run_frame(vm, frame, &out_val);
    vm->active_realm = saved_realm;
    pop_call_stack(vm);
    ajs_temp_root_restore(vm, rs);
    return (rst == AJS_RUN_THREW) ? AJS_VAL_EXCEPTION : out_val;
}

bool ajs_exec_module_bc(AjsVm *vm, AjsModuleRecord *mod) {
    if (!mod || !mod->bc) return true;
    AjsRealm *realm = mod->realm ? mod->realm : vm->active_realm;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(mod));

    AjsExecFrame *frame = ajs_frame_new(vm, realm, NULL, mod->bc, mod->env,
                                        AJS_VAL_UNDEFINED, AJS_VAL_UNDEFINED, mod);
    if (!frame) {
        ajs_temp_root_restore(vm, rs);
        return false;
    }
    ajs_temp_root_push(vm, ajs_val_from_gc(frame));

    if (mod->has_tla) {
        /* Async module evaluation (Top-Level Await): wrap frame in an async driver resolving
         * `mod->top_resolve_fn` / `mod->top_reject_fn`! */
        AjsObject *gobj = ajs_obj_new(vm, realm, AJS_OBJ_GENERATOR, AJS_VAL_NULL);
        ajs_temp_root_push(vm, ajs_val_from_gc(gobj));
        gobj->u.gen.state = AJS_GEN_EXECUTING;
        gobj->u.gen.is_async_fn = true;
        gobj->u.gen.frame = frame;
        gobj->u.gen.async_promise = mod->top_capability_promise;
        gobj->u.gen.async_resolve = mod->top_resolve_fn;
        gobj->u.gen.async_reject = mod->top_reject_fn;

        if (!push_call_stack(vm, frame, NULL)) {
            ajs_temp_root_restore(vm, rs);
            return false;
        }
        AjsRealm *saved_realm = vm->active_realm;
        vm->active_realm = realm;
        AjsValue out_val = AJS_VAL_UNDEFINED;
        AjsFrameRunStatus rst = ajs_run_frame(vm, frame, &out_val);
        vm->active_realm = saved_realm;
        pop_call_stack(vm);

        if (rst == AJS_RUN_RETURNED) {
            gobj->u.gen.state = AJS_GEN_COMPLETED;
            gobj->u.gen.frame = NULL;
            if (!ajs_is_undefined(mod->top_resolve_fn)) {
                AjsValue u = AJS_VAL_UNDEFINED;
                ajs_call_internal(vm, mod->top_resolve_fn, AJS_VAL_UNDEFINED, 1, &u);
                vm->has_exception = false;
            }
            ajs_temp_root_restore(vm, rs);
            return true;
        }
        if (rst == AJS_RUN_THREW) {
            gobj->u.gen.state = AJS_GEN_COMPLETED;
            gobj->u.gen.frame = NULL;
            ajs_temp_root_restore(vm, rs);
            return false;
        }
        /* Suspended at Top-Level `await`! */
        gobj->u.gen.state = AJS_GEN_SUSPENDED_YIELD;
        if (!attach_await_continuation(vm, realm, gobj, out_val)) {
            ajs_temp_root_restore(vm, rs);
            return false;
        }
        ajs_temp_root_restore(vm, rs);
        return true;
    }

    if (!push_call_stack(vm, frame, NULL)) {
        ajs_temp_root_restore(vm, rs);
        return false;
    }
    AjsRealm *saved_realm = vm->active_realm;
    vm->active_realm = realm;
    AjsValue out_val = AJS_VAL_UNDEFINED;
    AjsFrameRunStatus rst = ajs_run_frame(vm, frame, &out_val);
    vm->active_realm = saved_realm;
    pop_call_stack(vm);
    ajs_temp_root_restore(vm, rs);
    return rst != AJS_RUN_THREW;
}

/* ============================================================================
 * 5. Iterator, For-In, Object Spread/Rest & Yield* Helpers
 * ============================================================================ */

static AjsValue for_in_next_native(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *userdata) {
    (void)argc; (void)argv; (void)userdata;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
    }
    AjsObject *it = (AjsObject *)ajs_val_to_gc(this_val);
    if (it->kind != AJS_OBJ_ITERATOR || it->u.iter.done) {
        return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
    }
    /* `target` is the snapshot array of candidate keys; `inner_iter` is the original object to verify live existence! */
    if (!ajs_val_is_gc_kind(it->u.iter.target, AJS_GC_OBJECT)) {
        it->u.iter.done = true;
        return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
    }
    AjsObject *keys_arr = (AjsObject *)ajs_val_to_gc(it->u.iter.target);
    AjsObject *orig_obj = ajs_val_is_gc_kind(it->u.iter.inner_iter, AJS_GC_OBJECT)
                          ? (AjsObject *)ajs_val_to_gc(it->u.iter.inner_iter) : NULL;

    while (it->u.iter.index < keys_arr->u.arr.dense_len) {
        AjsValue kval = keys_arr->u.arr.dense[it->u.iter.index++];
        if (!orig_obj) return ajs_make_iter_result(vm, kval, false);
        /* Verify property still exists and is enumerable somewhere on prototype chain (if not shadowed by non-enum!) */
        AjsObject *cur = orig_obj;
        bool visible = false;
        while (cur) {
            AjsPropDesc desc;
            bool found = false;
            if (!ajs_obj_get_own_desc(vm, cur, kval, &desc, &found)) return AJS_VAL_EXCEPTION;
            if (found) {
                visible = (desc.flags & AJS_DESC_ENUMERABLE) != 0;
                break;
            }
            AjsValue pv = AJS_VAL_NULL;
            if (!ajs_obj_get_proto(vm, cur, &pv)) return AJS_VAL_EXCEPTION;
            if (!ajs_val_is_gc_kind(pv, AJS_GC_OBJECT)) break;
            cur = (AjsObject *)ajs_val_to_gc(pv);
        }
        if (visible) {
            return ajs_make_iter_result(vm, kval, false);
        }
    }
    it->u.iter.done = true;
    return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
}

static bool build_for_in_iterator(AjsVm *vm, AjsRealm *realm, AjsValue target_val,
                                  AjsValue *out_iter, AjsValue *out_next_fn) {
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *obj = ajs_to_object(vm, target_val);
    if (vm->has_exception || !obj) { ajs_temp_root_restore(vm, rs); return false; }
    ajs_temp_root_push(vm, ajs_val_from_gc(obj));

    AjsObject *keys_arr = ajs_obj_new(vm, realm, AJS_OBJ_ARRAY, realm->array_proto);
    ajs_temp_root_push(vm, ajs_val_from_gc(keys_arr));
    keys_arr->u.arr.length_writable = true;

    /* Collect unique string keys across prototype chain (recording shadowed non-enumerable keys too!) */
    uint32_t seen_cap = 16u, seen_len = 0u;
    AjsValue *seen = (AjsValue *)malloc(seen_cap * sizeof(AjsValue));

    AjsObject *cur = obj;
    for (int depth = 0; cur && depth < 128; depth++) {
        ajs_temp_root_push(vm, ajs_val_from_gc(cur));
        AjsValue *own_keys = NULL;
        uint32_t own_cnt = 0;
        if (!ajs_obj_own_keys(vm, cur, false, true, false, &own_keys, &own_cnt)) {
            free(seen);
            ajs_temp_root_restore(vm, rs);
            return false;
        }
        for (uint32_t i = 0; i < own_cnt; i++) {
            AjsValue kv = own_keys[i];
            bool dup = false;
            for (uint32_t j = 0; j < seen_len; j++) {
                if (ajs_strict_eq(seen[j], kv)) { dup = true; break; }
            }
            if (dup) continue;
            if (seen_len >= seen_cap) {
                seen_cap *= 2u;
                seen = (AjsValue *)realloc(seen, seen_cap * sizeof(AjsValue));
            }
            seen[seen_len++] = kv;

            AjsPropDesc desc;
            bool found = false;
            if (!ajs_obj_get_own_desc(vm, cur, kv, &desc, &found)) {
                free(own_keys); free(seen); ajs_temp_root_restore(vm, rs); return false;
            }
            if (found && (desc.flags & AJS_DESC_ENUMERABLE)) {
                if (keys_arr->u.arr.dense_len >= keys_arr->u.arr.dense_cap) {
                    uint32_t ncap = keys_arr->u.arr.dense_cap ? keys_arr->u.arr.dense_cap * 2u : 16u;
                    keys_arr->u.arr.dense = (AjsValue *)realloc(keys_arr->u.arr.dense, ncap * sizeof(AjsValue));
                    keys_arr->u.arr.dense_cap = ncap;
                }
                keys_arr->u.arr.dense[keys_arr->u.arr.dense_len++] = kv;
                keys_arr->u.arr.length = keys_arr->u.arr.dense_len;
            }
        }
        free(own_keys);
        AjsValue pv = AJS_VAL_NULL;
        if (!ajs_obj_get_proto(vm, cur, &pv)) {
            free(seen); ajs_temp_root_restore(vm, rs); return false;
        }
        cur = ajs_val_is_gc_kind(pv, AJS_GC_OBJECT) ? (AjsObject *)ajs_val_to_gc(pv) : NULL;
    }
    free(seen);

    AjsObject *it = ajs_obj_new(vm, realm, AJS_OBJ_ITERATOR, realm->iterator_proto);
    ajs_temp_root_push(vm, ajs_val_from_gc(it));
    it->u.iter.kind = AJS_ITER_ARRAY_KEYS;
    it->u.iter.target = ajs_val_from_gc(keys_arr);
    it->u.iter.inner_iter = ajs_val_from_gc(obj);
    it->u.iter.index = 0;

    AjsObject *nfn = ajs_obj_new(vm, realm, AJS_OBJ_FUNCTION, realm->function_proto);
    nfn->u.fn.fn_kind = AJS_FN_NATIVE;
    nfn->u.fn.native_fn = for_in_next_native;
    ajs_temp_root_push(vm, ajs_val_from_gc(nfn));

    *out_iter = ajs_val_from_gc(it);
    *out_next_fn = ajs_val_from_gc(nfn);
    ajs_temp_root_restore(vm, rs);
    return true;
}

/* Async-from-Sync Iterator wrapper per ECMA-262 §27.1.4 */
static AjsValue async_from_sync_method(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *userdata) {
    int mode = (int)(intptr_t)userdata; /* 0=next, 1=return, 2=throw */
    AjsRealm *realm = vm->active_realm;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue res_fn = AJS_VAL_UNDEFINED, rej_fn = AJS_VAL_UNDEFINED;
    AjsValue prom = ajs_promise_create(vm, &res_fn, &rej_fn);
    ajs_temp_root_push(vm, prom);
    ajs_temp_root_push(vm, res_fn);
    ajs_temp_root_push(vm, rej_fn);

    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Invalid AsyncFromSyncIterator");
        goto reject_exc;
    }
    AjsObject *w = (AjsObject *)ajs_val_to_gc(this_val);
    AjsValue sync_it = w->u.iter.target;
    AjsValue sync_next = w->u.iter.next_method;
    ajs_temp_root_push(vm, sync_it);

    AjsValue step_res = AJS_VAL_UNDEFINED;
    if (mode == 0) {
        step_res = ajs_call_internal(vm, sync_next, sync_it, argc, argv);
    } else {
        AjsString *mname = (mode == 1) ? vm->str_return : vm->str_throw;
        AjsValue mfn = ajs_obj_get(vm, sync_it, ajs_val_from_gc(mname), sync_it);
        if (vm->has_exception) goto reject_exc;
        if (ajs_is_nullish(mfn)) {
            if (mode == 1) {
                AjsValue ir = ajs_make_iter_result(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, true);
                ajs_temp_root_push(vm, ir);
                ajs_call_internal(vm, res_fn, AJS_VAL_UNDEFINED, 1, &ir);
                vm->has_exception = false;
                ajs_temp_root_restore(vm, rs);
                return prom;
            }
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Sync iterator has no throw method");
            goto reject_exc;
        }
        ajs_temp_root_push(vm, mfn);
        step_res = ajs_call_internal(vm, mfn, sync_it, argc, argv);
    }
    if (vm->has_exception) goto reject_exc;
    if (!ajs_is_object(vm, step_res)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator result is not an object");
        goto reject_exc;
    }
    ajs_temp_root_push(vm, step_res);

    AjsValue done_v = ajs_obj_get(vm, step_res, ajs_val_from_gc(vm->str_done), step_res);
    if (vm->has_exception) goto reject_exc;
    bool done = ajs_to_bool(vm, done_v);

    AjsValue val_v = ajs_obj_get(vm, step_res, ajs_val_from_gc(vm->str_value), step_res);
    if (vm->has_exception) goto reject_exc;
    ajs_temp_root_push(vm, val_v);

    /* Await `val_v` and wrap into `{ value: unwrapped, done }` */
    AjsValue vprom = ajs_promise_resolve_val(vm, realm, realm->promise_ctor, val_v);
    if (vm->has_exception) goto reject_exc;
    ajs_temp_root_push(vm, vprom);

    /* Attach fulfillment wrapper */
    AjsObject *wrap_fn = ajs_obj_new(vm, realm, AJS_OBJ_FUNCTION, realm->function_proto);
    wrap_fn->u.fn.fn_kind = AJS_FN_NATIVE;
    wrap_fn->u.fn.native_magic = done ? 1 : 0;
    /* Use inline static helper below via closure_env slot 0 = res_fn */
    extern AjsValue ajs_afs_unwrap_cb(AjsVm *vm, AjsValue this_v, int ac, const AjsValue *av, void *ud);
    wrap_fn->u.fn.native_fn = ajs_afs_unwrap_cb;
    wrap_fn->u.fn.native_userdata = (void *)(uintptr_t)(done ? 1 : 0);
    ajs_temp_root_push(vm, ajs_val_from_gc(wrap_fn));

    ajs_promise_then_internal(vm, (AjsObject *)ajs_val_to_gc(vprom),
                              ajs_val_from_gc(wrap_fn), AJS_VAL_UNDEFINED,
                              res_fn, rej_fn, prom);
    ajs_temp_root_restore(vm, rs);
    return prom;

reject_exc:;
    AjsValue exc = vm->exception;
    vm->has_exception = false;
    vm->exception = AJS_VAL_UNDEFINED;
    ajs_temp_root_push(vm, exc);
    ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &exc);
    vm->has_exception = false;
    ajs_temp_root_restore(vm, rs);
    return prom;
}

AjsValue ajs_afs_unwrap_cb(AjsVm *vm, AjsValue this_v, int ac, const AjsValue *av, void *ud) {
    (void)this_v;
    bool done = ((uintptr_t)ud) != 0;
    return ajs_make_iter_result(vm, ac > 0 ? av[0] : AJS_VAL_UNDEFINED, done);
}

static bool get_iterator_internal(AjsVm *vm, AjsRealm *realm, AjsValue iterable, bool is_async,
                                  AjsValue *out_iter, AjsValue *out_next) {
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, iterable);

    if (is_async) {
        AjsValue m_async = ajs_obj_get(vm, iterable, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ASYNC_ITERATOR]), iterable);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
        if (!ajs_is_nullish(m_async)) {
            ajs_temp_root_push(vm, m_async);
            AjsValue it = ajs_call_internal(vm, m_async, iterable, 0, NULL);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
            if (!ajs_is_object(vm, it)) {
                ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Async iterator is not an object");
                ajs_temp_root_restore(vm, rs);
                return false;
            }
            ajs_temp_root_push(vm, it);
            AjsValue nfn = ajs_obj_get(vm, it, ajs_val_from_gc(vm->str_next), it);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
            *out_iter = it;
            *out_next = nfn;
            ajs_temp_root_restore(vm, rs);
            return true;
        }
        /* Fall back to sync iterator wrapped in Async-from-Sync Iterator! */
        AjsValue sync_it = AJS_VAL_UNDEFINED, sync_next = AJS_VAL_UNDEFINED;
        if (!get_iterator_internal(vm, realm, iterable, false, &sync_it, &sync_next)) {
            ajs_temp_root_restore(vm, rs);
            return false;
        }
        ajs_temp_root_push(vm, sync_it);
        ajs_temp_root_push(vm, sync_next);

        AjsObject *w = ajs_obj_new(vm, realm, AJS_OBJ_ITERATOR, realm->async_iterator_proto);
        ajs_temp_root_push(vm, ajs_val_from_gc(w));
        w->u.iter.target = sync_it;
        w->u.iter.next_method = sync_next;
        ajs_def_native_method(vm, realm, ajs_val_from_gc(w), "next", async_from_sync_method, 1,
                              AJS_PROP_WRITABLE | AJS_PROP_CONFIGURABLE, 0);
        /* Set userdata on next/return/throw */
        AjsValue wval = ajs_val_from_gc(w);
        AjsObject *fn_next = ajs_obj_new(vm, realm, AJS_OBJ_FUNCTION, realm->function_proto);
        fn_next->u.fn.fn_kind = AJS_FN_NATIVE;
        fn_next->u.fn.native_fn = async_from_sync_method;
        fn_next->u.fn.native_userdata = (void *)(intptr_t)0;
        ajs_temp_root_push(vm, ajs_val_from_gc(fn_next));

        AjsObject *fn_ret = ajs_obj_new(vm, realm, AJS_OBJ_FUNCTION, realm->function_proto);
        fn_ret->u.fn.fn_kind = AJS_FN_NATIVE;
        fn_ret->u.fn.native_fn = async_from_sync_method;
        fn_ret->u.fn.native_userdata = (void *)(intptr_t)1;
        ajs_temp_root_push(vm, ajs_val_from_gc(fn_ret));

        AjsObject *fn_thr = ajs_obj_new(vm, realm, AJS_OBJ_FUNCTION, realm->function_proto);
        fn_thr->u.fn.fn_kind = AJS_FN_NATIVE;
        fn_thr->u.fn.native_fn = async_from_sync_method;
        fn_thr->u.fn.native_userdata = (void *)(intptr_t)2;
        ajs_temp_root_push(vm, ajs_val_from_gc(fn_thr));

        ajs_define_prop_str(vm, wval, "next", ajs_val_from_gc(fn_next), AJS_PROP_WRITABLE | AJS_PROP_CONFIGURABLE);
        ajs_define_prop_str(vm, wval, "return", ajs_val_from_gc(fn_ret), AJS_PROP_WRITABLE | AJS_PROP_CONFIGURABLE);
        ajs_define_prop_str(vm, wval, "throw", ajs_val_from_gc(fn_thr), AJS_PROP_WRITABLE | AJS_PROP_CONFIGURABLE);

        *out_iter = wval;
        *out_next = ajs_val_from_gc(fn_next);
        ajs_temp_root_restore(vm, rs);
        return true;
    }

    AjsValue m_sync = ajs_obj_get(vm, iterable, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), iterable);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
    if (!ajs_is_callable(vm, m_sync)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Value is not iterable");
        ajs_temp_root_restore(vm, rs);
        return false;
    }
    ajs_temp_root_push(vm, m_sync);
    AjsValue it = ajs_call_internal(vm, m_sync, iterable, 0, NULL);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
    if (!ajs_is_object(vm, it)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Result of Symbol.iterator method is not an object");
        ajs_temp_root_restore(vm, rs);
        return false;
    }
    ajs_temp_root_push(vm, it);
    AjsValue nfn = ajs_obj_get(vm, it, ajs_val_from_gc(vm->str_next), it);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
    *out_iter = it;
    *out_next = nfn;
    ajs_temp_root_restore(vm, rs);
    return true;
}

static bool iterator_close_sync(AjsVm *vm, AjsValue iter, bool suppress_errors) {
    if (!ajs_is_object(vm, iter)) return true;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, iter);

    AjsValue saved_exc = vm->exception;
    bool had_exc = vm->has_exception;
    AjsErrorPhase saved_phase = vm->exc_phase;
    if (suppress_errors) {
        vm->has_exception = false;
        vm->exception = AJS_VAL_UNDEFINED;
    }

    AjsValue ret_fn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_return), iter);
    if (vm->has_exception) {
        if (suppress_errors) {
            vm->has_exception = had_exc;
            vm->exception = saved_exc;
            vm->exc_phase = saved_phase;
            ajs_temp_root_restore(vm, rs);
            return true;
        }
        ajs_temp_root_restore(vm, rs);
        return false;
    }
    if (ajs_is_nullish(ret_fn)) {
        if (suppress_errors) {
            vm->has_exception = had_exc;
            vm->exception = saved_exc;
            vm->exc_phase = saved_phase;
        }
        ajs_temp_root_restore(vm, rs);
        return true;
    }
    ajs_temp_root_push(vm, ret_fn);
    AjsValue inner_res = ajs_call_internal(vm, ret_fn, iter, 0, NULL);
    if (suppress_errors) {
        vm->has_exception = had_exc;
        vm->exception = saved_exc;
        vm->exc_phase = saved_phase;
        ajs_temp_root_restore(vm, rs);
        return true;
    }
    if (vm->has_exception) {
        ajs_temp_root_restore(vm, rs);
        return false;
    }
    if (!ajs_is_object(vm, inner_res)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator.return() result is not an object");
        ajs_temp_root_restore(vm, rs);
        return false;
    }
    ajs_temp_root_restore(vm, rs);
    return true;
}

/* ============================================================================
 * 6. Bytecode Interpreter Loop (`ajs_run_frame`) - Part A
 * ============================================================================ */

static inline uint8_t bc_u8(const uint8_t *code, uint32_t *pc) {
    return code[(*pc)++];
}

static inline uint16_t bc_u16(const uint8_t *code, uint32_t *pc) {
    uint32_t p = *pc;
    uint16_t v = (uint16_t)code[p] | ((uint16_t)code[p + 1u] << 8);
    *pc = p + 2u;
    return v;
}

static inline uint32_t bc_u32(const uint8_t *code, uint32_t *pc) {
    uint32_t p = *pc;
    uint32_t v = (uint32_t)code[p] |
                 ((uint32_t)code[p + 1u] << 8) |
                 ((uint32_t)code[p + 2u] << 16) |
                 ((uint32_t)code[p + 3u] << 24);
    *pc = p + 4u;
    return v;
}

static inline int32_t bc_i32(const uint8_t *code, uint32_t *pc) {
    return (int32_t)bc_u32(code, pc);
}

static void unwind_frame_env(AjsExecFrame *frame, uint16_t target_depth) {
    while (frame->env_depth > target_depth && frame->env && frame->env->parent) {
        frame->env = frame->env->parent;
        frame->env_depth--;
    }
}

static bool set_inferred_fn_name(AjsVm *vm, AjsValue fn_val, AjsValue key_val, uint8_t prefix) {
    if (!ajs_val_is_gc_kind(fn_val, AJS_GC_OBJECT)) return true;
    AjsObject *fn = (AjsObject *)ajs_val_to_gc(fn_val);
    if (fn->kind != AJS_OBJ_FUNCTION) return true;

    /* Only set `name` if not already non-empty own property (or if missing) */
    AjsPropDesc existing;
    bool found = false;
    if (!ajs_obj_get_own_desc(vm, fn, ajs_val_from_gc(vm->str_name), &existing, &found)) return false;
    if (found && (existing.flags & AJS_DESC_HAS_VALUE) &&
        ajs_val_is_gc_kind(existing.value, AJS_GC_STRING)) {
        AjsString *es = (AjsString *)ajs_val_to_gc(existing.value);
        if (es->len16 > 0 && prefix == 0) return true;
    }

    const char *pfx = (prefix == 1) ? "get " : ((prefix == 2) ? "set " : "");
    size_t pfx_len = strlen(pfx);

    AjsString *final_name = NULL;
    if (ajs_val_is_gc_kind(key_val, AJS_GC_SYMBOL)) {
        AjsSymbol *sym = (AjsSymbol *)ajs_val_to_gc(key_val);
        if (!sym->has_desc || !sym->desc) {
            final_name = ajs_str_from_cstr(vm, pfx, true);
        } else if (sym->desc->len16 == 0) {
            final_name = (pfx_len > 0) ? ajs_str_from_cstr(vm, pfx, true) : vm->str_empty;
        } else {
            size_t blen = pfx_len + (size_t)sym->desc->len8 + 2u;
            char *buf = (char *)malloc(blen + 1u);
            if (!buf) return false;
            ajs_memcpy(buf, pfx, pfx_len);
            buf[pfx_len] = '[';
            ajs_memcpy(buf + pfx_len + 1u, sym->desc->utf8, sym->desc->len8);
            buf[pfx_len + 1u + sym->desc->len8] = ']';
            buf[blen] = '\0';
            final_name = ajs_str_from_utf8(vm, buf, blen, true);
            free(buf);
        }
    } else {
        AjsString *ks = ajs_to_js_string(vm, key_val);
        if (vm->has_exception || !ks) return false;
        if (pfx_len == 0) {
            final_name = ks;
        } else {
            size_t blen = pfx_len + (size_t)ks->len8;
            char *buf = (char *)malloc(blen + 1u);
            if (!buf) return false;
            ajs_memcpy(buf, pfx, pfx_len);
            ajs_memcpy(buf + pfx_len, ks->utf8, ks->len8);
            buf[blen] = '\0';
            final_name = ajs_str_from_utf8(vm, buf, blen, true);
            free(buf);
        }
    }

    AjsPropDesc ndesc = {
        .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
                 AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
        .value = ajs_val_from_gc(final_name)
    };
    return ajs_obj_define_own_prop(vm, fn, ajs_val_from_gc(vm->str_name), &ndesc, false);
}

static bool extract_array_args(AjsVm *vm, AjsValue arr_val, AjsValue **out_argv, int *out_argc) {
    if (!ajs_val_is_gc_kind(arr_val, AJS_GC_OBJECT)) {
        *out_argv = NULL;
        *out_argc = 0;
        return true;
    }
    AjsObject *a = (AjsObject *)ajs_val_to_gc(arr_val);
    if (a->kind == AJS_OBJ_ARRAY && a->u.arr.dense_len == a->u.arr.length) {
        uint32_t n = a->u.arr.length;
        if (n == 0) { *out_argv = NULL; *out_argc = 0; return true; }
        AjsValue *av = (AjsValue *)malloc(n * sizeof(AjsValue));
        if (!av) return false;
        for (uint32_t i = 0; i < n; i++) {
            av[i] = (a->u.arr.dense[i] == AJS_VAL_EMPTY) ? AJS_VAL_UNDEFINED : a->u.arr.dense[i];
        }
        *out_argv = av;
        *out_argc = (int)n;
        return true;
    }
    uint64_t len64 = 0;
    AjsValue lv = ajs_obj_get(vm, arr_val, ajs_val_from_gc(vm->str_length), arr_val);
    if (vm->has_exception || !ajs_to_length(vm, lv, &len64)) return false;
    if (len64 == 0) { *out_argv = NULL; *out_argc = 0; return true; }
    if (len64 > 65535u) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Too many arguments in function call");
        return false;
    }
    uint32_t n = (uint32_t)len64;
    AjsValue *av = (AjsValue *)malloc(n * sizeof(AjsValue));
    if (!av) return false;
    for (uint32_t i = 0; i < n; i++) {
        AjsString *ks = ajs_number_to_str(vm, (double)i, 10);
        av[i] = ajs_obj_get(vm, arr_val, ajs_val_from_gc(ks), arr_val);
        if (vm->has_exception) { free(av); return false; }
    }
    *out_argv = av;
    *out_argc = (int)n;
    return true;
}

static AjsFrameRunStatus ajs_run_frame(AjsVm *vm, AjsExecFrame *frame, AjsValue *out_val) {
    AjsBytecode *bc = frame->bc;
    AjsRealm *realm = frame->realm;
    const uint8_t *code = bc->code;
    AjsValue ret_candidate = AJS_VAL_UNDEFINED;

    #define PUSH(v) do { if (!frame_stack_push(frame, (v))) { ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Operand stack overflow"); goto handle_exception; } } while (0)
    #define POP()   (frame->stack[--frame->sp])
    #define TOP()   (frame->stack[frame->sp - 1u])
    #define CHECK_EXC() do { if (vm->has_exception) goto handle_exception; } while (0)

    /* Handle injected exception (e.g. `gen.throw(e)` or rejected `await`) */
    if (vm->has_exception) {
        frame->ys_awaiting_step = false;
        if (frame->in_yield_star) {
            frame->in_yield_star = false;
            frame->ys_iter = AJS_VAL_UNDEFINED;
            frame->ys_next = AJS_VAL_UNDEFINED;
        }
        goto handle_exception;
    }

    /* Handle injected `gen.return(val)` on a normal suspended `yield` */
    if (frame->ys_step_mode == 99) {
        AjsValue rval = POP();
        if (bc->fn_kind == AJS_FN_BC_ASYNC_GENERATOR && !frame->ys_awaiting_step) {
            /* ECMA-262 AsyncGeneratorUnwrapYieldResumption: await `rval` first! */
            frame->ys_awaiting_step = true;
            *out_val = rval;
            return AJS_RUN_AWAITED;
        }
        frame->ys_awaiting_step = false;
        frame->ys_step_mode = 0;
        ret_candidate = rval;
        goto do_unwind_return_label;
    }

    /* Resume active `yield*` delegation if suspended inside `OP_YIELD_STAR` */
    if (frame->in_yield_star) {
        goto resume_yield_star;
    }

    for (;;) {
        /* Periodic interrupt / deadline check every 4096 instructions */
        if (((++vm->op_counter) & 0xFFFu) == 0 && vm->cfg.interrupt_fn) {
            if (vm->cfg.interrupt_fn(vm, vm->cfg.interrupt_userdata)) {
                if (!vm->has_exception) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_ERROR, "Script execution interrupted");
                }
                goto handle_exception;
            }
        }

        if (frame->pc >= bc->code_len) {
            *out_val = AJS_VAL_UNDEFINED;
            return AJS_RUN_RETURNED;
        }

        AjsOpcode op = (AjsOpcode)code[frame->pc++];
        switch (op) {
            case OP_NOP:
                break;
            case OP_UNDEF:
                PUSH(AJS_VAL_UNDEFINED);
                break;
            case OP_NULL:
                PUSH(AJS_VAL_NULL);
                break;
            case OP_TRUE:
                PUSH(AJS_VAL_TRUE);
                break;
            case OP_FALSE:
                PUSH(AJS_VAL_FALSE);
                break;
            case OP_TDZ:
                PUSH(AJS_VAL_TDZ);
                break;
            case OP_EMPTY:
                PUSH(AJS_VAL_EMPTY);
                break;
            case OP_ZERO:
                PUSH(ajs_val_number(0.0));
                break;
            case OP_ONE:
                PUSH(ajs_val_number(1.0));
                break;
            case OP_INT32: {
                int32_t iv = bc_i32(code, &frame->pc);
                PUSH(ajs_val_number((double)iv));
                break;
            }
            case OP_CONST: {
                uint32_t idx = bc_u32(code, &frame->pc);
                PUSH(idx < bc->const_count ? bc->consts[idx] : AJS_VAL_UNDEFINED);
                break;
            }
            case OP_THIS:
                if (frame->this_val == AJS_VAL_TDZ) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE,
                                       "Must call super constructor in derived class before accessing 'this'");
                    goto handle_exception;
                }
                PUSH(frame->this_val);
                break;
            case OP_NEW_TARGET:
                PUSH(frame->new_target);
                break;
            case OP_ARGUMENTS:
                PUSH(frame->args_obj ? ajs_val_from_gc(frame->args_obj) : AJS_VAL_UNDEFINED);
                break;
            case OP_IMPORT_META: {
                AjsModuleRecord *m = frame->module;
                if (!m && frame->func_obj) m = frame->func_obj->u.fn.owner_module;
                AjsValue im = ajs_module_get_import_meta(vm, m);
                CHECK_EXC();
                PUSH(im);
                break;
            }
            case OP_ACTIVE_FUNC:
                PUSH(frame->func_obj ? ajs_val_from_gc(frame->func_obj) : AJS_VAL_UNDEFINED);
                break;

            case OP_POP:
                (void)POP();
                break;
            case OP_DUP: {
                AjsValue v = TOP();
                PUSH(v);
                break;
            }
            case OP_DUP2: {
                AjsValue a = frame->stack[frame->sp - 2u];
                AjsValue b = frame->stack[frame->sp - 1u];
                PUSH(a);
                PUSH(b);
                break;
            }
            case OP_SWAP: {
                AjsValue a = frame->stack[frame->sp - 2u];
                frame->stack[frame->sp - 2u] = frame->stack[frame->sp - 1u];
                frame->stack[frame->sp - 1u] = a;
                break;
            }
            case OP_ROT3: {
                /* [a, b, c] -> [c, a, b] */
                AjsValue a = frame->stack[frame->sp - 3u];
                AjsValue b = frame->stack[frame->sp - 2u];
                AjsValue c = frame->stack[frame->sp - 1u];
                frame->stack[frame->sp - 3u] = c;
                frame->stack[frame->sp - 2u] = a;
                frame->stack[frame->sp - 1u] = b;
                break;
            }
            case OP_ROT3L: {
                /* [a, b, c] -> [b, c, a] */
                AjsValue a = frame->stack[frame->sp - 3u];
                AjsValue b = frame->stack[frame->sp - 2u];
                AjsValue c = frame->stack[frame->sp - 1u];
                frame->stack[frame->sp - 3u] = b;
                frame->stack[frame->sp - 2u] = c;
                frame->stack[frame->sp - 1u] = a;
                break;
            }
            case OP_NIP: {
                /* [a, b] -> [b] */
                frame->stack[frame->sp - 2u] = frame->stack[frame->sp - 1u];
                frame->sp--;
                break;
            }

            case OP_GET_LEX: {
                uint16_t d = bc_u16(code, &frame->pc);
                uint16_t s = bc_u16(code, &frame->pc);
                AjsEnv *e = env_at_depth(frame->env, d);
                AjsValue val = AJS_VAL_UNDEFINED;
                if (!env_read_slot(vm, e, s, &val)) goto handle_exception;
                PUSH(val);
                break;
            }
            case OP_GET_LOCAL8: {
                uint8_t s = bc_u8(code, &frame->pc);
                AjsValue val = AJS_VAL_UNDEFINED;
                if (!env_read_slot(vm, frame->env, s, &val)) goto handle_exception;
                PUSH(val);
                break;
            }
            case OP_SET_LOCAL8: {
                uint8_t s = bc_u8(code, &frame->pc);
                uint8_t st = bc_u8(code, &frame->pc);
                AjsValue val = TOP();
                if (!env_write_slot(vm, frame->env, s, val, st != 0)) goto handle_exception;
                break;
            }
            case OP_INIT_LOCAL8: {
                uint8_t s = bc_u8(code, &frame->pc);
                AjsValue val = TOP();
                AjsEnv *e = frame->env;
                if (e && s < e->count) {
                    e->slots[s] = val;
                    if (bc->is_eval && !bc->strict && e == frame->base_env &&
                        bc->env_slot_names && bc->env_slot_names[s] &&
                        !(bc->env_slot_flags[s] & AJS_SLOT_INIT_TDZ) &&
                        (!e->names || e->names[s] == NULL)) {
                        for (AjsEnv *pe = e->parent; pe; pe = pe->parent) {
                            if (pe->with_obj) continue;
                            int32_t pslot = env_find_named_slot(pe, bc->env_slot_names[s]);
                            if (pslot >= 0) { pe->slots[pslot] = val; break; }
                        }
                    }
                }
                break;
            }
            case OP_GET_LEX32: {
                uint16_t d = bc_u16(code, &frame->pc);
                uint32_t s = bc_u32(code, &frame->pc);
                AjsEnv *e = env_at_depth(frame->env, d);
                AjsValue val = AJS_VAL_UNDEFINED;
                if (!env_read_slot(vm, e, s, &val)) goto handle_exception;
                PUSH(val);
                break;
            }
            case OP_SET_LEX: {
                uint16_t d = bc_u16(code, &frame->pc);
                uint16_t s = bc_u16(code, &frame->pc);
                uint8_t st = bc_u8(code, &frame->pc);
                AjsValue val = TOP();
                AjsEnv *e = env_at_depth(frame->env, d);
                if (!env_write_slot(vm, e, s, val, st != 0)) goto handle_exception;
                break;
            }
            case OP_SET_LEX32: {
                uint16_t d = bc_u16(code, &frame->pc);
                uint32_t s = bc_u32(code, &frame->pc);
                uint8_t st = bc_u8(code, &frame->pc);
                AjsValue val = TOP();
                AjsEnv *e = env_at_depth(frame->env, d);
                if (!env_write_slot(vm, e, s, val, st != 0)) goto handle_exception;
                break;
            }
            case OP_INIT_LEX:
            case OP_INIT_LEX32: {
                uint16_t d = bc_u16(code, &frame->pc);
                uint32_t s = (op == OP_INIT_LEX32) ? bc_u32(code, &frame->pc) : (uint32_t)bc_u16(code, &frame->pc);
                AjsValue val = TOP();
                AjsEnv *e = env_at_depth(frame->env, d);
                if (e && s < e->count) {
                    e->slots[s] = val;
                    /* Sloppy direct `eval` hoisted function sync to caller var env */
                    if (bc->is_eval && !bc->strict && e == frame->base_env &&
                        bc->env_slot_names && bc->env_slot_names[s] &&
                        !(bc->env_slot_flags[s] & AJS_SLOT_INIT_TDZ) &&
                        (!e->names || e->names[s] == NULL)) {
                        for (AjsEnv *pe = e->parent; pe; pe = pe->parent) {
                            if (pe->with_obj) continue;
                            int32_t pslot = env_find_named_slot(pe, bc->env_slot_names[s]);
                            if (pslot >= 0) { pe->slots[pslot] = val; break; }
                        }
                    }
                }
                break;
            }
            case OP_CHECK_LEX_TDZ:
            case OP_CHECK_LEX_TDZ32: {
                uint16_t d = bc_u16(code, &frame->pc);
                uint32_t s = (op == OP_CHECK_LEX_TDZ32) ? bc_u32(code, &frame->pc) : (uint32_t)bc_u16(code, &frame->pc);
                AjsEnv *e = env_at_depth(frame->env, d);
                AjsValue dummy;
                if (!env_read_slot(vm, e, s, &dummy)) goto handle_exception;
                break;
            }

            case OP_DEF_GLOBAL_VAR: {
                uint32_t nidx = bc_u32(code, &frame->pc);
                uint8_t is_fn = bc_u8(code, &frame->pc);
                AjsValue key = bc->consts[nidx];
                AjsString *nm = (AjsString *)ajs_val_to_gc(key);
                if (env_find_named_slot(realm->global_lex_env, nm) >= 0) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX,
                                       "Identifier '%s' has already been declared", nm->utf8);
                    goto handle_exception;
                }
                AjsObject *gobj = (AjsObject *)ajs_val_to_gc(realm->global_obj);
                AjsPropDesc desc;
                bool found = false;
                if (!ajs_obj_get_own_desc(vm, gobj, key, &desc, &found)) goto handle_exception;
                if (is_fn) {
                    if (!found || (desc.flags & AJS_DESC_CONFIGURABLE)) {
                        if (!found && !gobj->extensible) {
                            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Global object is not extensible");
                            goto handle_exception;
                        }
                        uint32_t cfg_fl = (!found && bc->is_eval) || (found && (desc.flags & AJS_DESC_CONFIGURABLE))
                                          ? AJS_DESC_CONFIGURABLE : 0u;
                        AjsPropDesc nd = {
                            .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                                     AJS_DESC_HAS_ENUMERABLE | AJS_DESC_ENUMERABLE |
                                     AJS_DESC_HAS_CONFIGURABLE | cfg_fl,
                            .value = AJS_VAL_UNDEFINED
                        };
                        if (!ajs_obj_define_own_prop(vm, gobj, key, &nd, true)) goto handle_exception;
                    } else if ((desc.flags & AJS_DESC_IS_ACCESSOR) ||
                               !(desc.flags & AJS_DESC_WRITABLE) ||
                               !(desc.flags & AJS_DESC_ENUMERABLE)) {
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                           "Cannot redeclare non-configurable global '%s'", nm->utf8);
                        goto handle_exception;
                    }
                } else if (!found) {
                    if (!gobj->extensible) {
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Global object is not extensible");
                        goto handle_exception;
                    }
                    AjsPropDesc nd = {
                        .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                                 AJS_DESC_HAS_ENUMERABLE | AJS_DESC_ENUMERABLE |
                                 AJS_DESC_HAS_CONFIGURABLE | (bc->is_eval ? AJS_DESC_CONFIGURABLE : 0u),
                        .value = AJS_VAL_UNDEFINED
                    };
                    if (!ajs_obj_define_own_prop(vm, gobj, key, &nd, true)) goto handle_exception;
                }
                break;
            }
            case OP_DEF_GLOBAL_LEX: {
                uint32_t nidx = bc_u32(code, &frame->pc);
                uint8_t is_const = bc_u8(code, &frame->pc);
                AjsValue key = bc->consts[nidx];
                AjsString *nm = (AjsString *)ajs_val_to_gc(key);
                if (env_find_named_slot(realm->global_lex_env, nm) >= 0) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX,
                                       "Identifier '%s' has already been declared", nm->utf8);
                    goto handle_exception;
                }
                AjsObject *gobj = (AjsObject *)ajs_val_to_gc(realm->global_obj);
                AjsPropDesc desc;
                bool found = false;
                if (!ajs_obj_get_own_desc(vm, gobj, key, &desc, &found)) goto handle_exception;
                if (found && !(desc.flags & AJS_DESC_CONFIGURABLE)) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX,
                                       "Identifier '%s' has already been declared", nm->utf8);
                    goto handle_exception;
                }
                if (env_append_named_slot(realm->global_lex_env, nm, AJS_VAL_TDZ, is_const ? AJS_SLOT_CONST : 0u) < 0) {
                    ajs_throw(vm, vm->oom_error);
                    goto handle_exception;
                }
                break;
            }
            case OP_INIT_GLOBAL_LEX: {
                uint32_t nidx = bc_u32(code, &frame->pc);
                AjsString *nm = (AjsString *)ajs_val_to_gc(bc->consts[nidx]);
                int32_t slot = env_find_named_slot(realm->global_lex_env, nm);
                if (slot >= 0) realm->global_lex_env->slots[slot] = TOP();
                break;
            }
            case OP_GET_GLOBAL: {
                uint32_t nidx = bc_u32(code, &frame->pc);
                uint8_t typeof_mode = bc_u8(code, &frame->pc);
                AjsValue key = bc->consts[nidx];
                AjsString *nm = (AjsString *)ajs_val_to_gc(key);
                int32_t lslot = env_find_named_slot(realm->global_lex_env, nm);
                if (lslot >= 0) {
                    AjsValue v = AJS_VAL_UNDEFINED;
                    if (!env_read_slot(vm, realm->global_lex_env, (uint32_t)lslot, &v)) goto handle_exception;
                    PUSH(v);
                    break;
                }
                AjsObject *gobj = (AjsObject *)ajs_val_to_gc(realm->global_obj);
                bool has = false;
                if (!ajs_obj_has_prop(vm, gobj, key, &has)) goto handle_exception;
                if (!has) {
                    if (typeof_mode) {
                        PUSH(AJS_VAL_UNDEFINED);
                        break;
                    }
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE, "%s is not defined", nm->utf8);
                    goto handle_exception;
                }
                AjsValue v = ajs_obj_get(vm, realm->global_obj, key, realm->global_obj);
                CHECK_EXC();
                PUSH(v);
                break;
            }
            case OP_SET_GLOBAL: {
                uint32_t nidx = bc_u32(code, &frame->pc);
                uint8_t st = bc_u8(code, &frame->pc);
                AjsValue val = TOP();
                AjsValue key = bc->consts[nidx];
                AjsString *nm = (AjsString *)ajs_val_to_gc(key);
                int32_t lslot = env_find_named_slot(realm->global_lex_env, nm);
                if (lslot >= 0) {
                    if (!env_write_slot(vm, realm->global_lex_env, (uint32_t)lslot, val, st != 0)) goto handle_exception;
                    break;
                }
                AjsObject *gobj = (AjsObject *)ajs_val_to_gc(realm->global_obj);
                if (st) {
                    bool has = false;
                    if (!ajs_obj_has_prop(vm, gobj, key, &has)) goto handle_exception;
                    if (!has) {
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE, "%s is not defined", nm->utf8);
                        goto handle_exception;
                    }
                }
                if (!ajs_obj_set(vm, realm->global_obj, key, val, realm->global_obj, st != 0)) goto handle_exception;
                break;
            }
            case OP_CHECK_GLOBAL_REF: {
                uint32_t nidx = bc_u32(code, &frame->pc);
                AjsValue key = bc->consts[nidx];
                AjsString *nm = (AjsString *)ajs_val_to_gc(key);
                if (env_find_named_slot(realm->global_lex_env, nm) < 0) {
                    AjsObject *gobj = (AjsObject *)ajs_val_to_gc(realm->global_obj);
                    bool has = false;
                    if (!ajs_obj_has_prop(vm, gobj, key, &has)) goto handle_exception;
                    if (!has) {
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE, "%s is not defined", nm->utf8);
                        goto handle_exception;
                    }
                }
                break;
            }
            case OP_GET_DYN_NAME: {
                uint32_t nidx = bc_u32(code, &frame->pc);
                uint8_t typeof_mode = bc_u8(code, &frame->pc);
                AjsValue key = bc->consts[nidx];
                AjsString *nm = (AjsString *)ajs_val_to_gc(key);
                bool resolved = false;
                for (AjsEnv *e = frame->env; e; e = e->parent) {
                    if (e->with_obj) {
                        bool has = false;
                        if (!with_obj_has_binding(vm, e->with_obj, key, &has)) goto handle_exception;
                        if (has) {
                            AjsValue wv = ajs_val_from_gc(e->with_obj);
                            AjsValue v = ajs_obj_get(vm, wv, key, wv);
                            CHECK_EXC();
                            PUSH(v);
                            resolved = true;
                            break;
                        }
                    } else {
                        int32_t sl = env_find_named_slot(e, nm);
                        if (sl >= 0) {
                            AjsValue v = AJS_VAL_UNDEFINED;
                            if (!env_read_slot(vm, e, (uint32_t)sl, &v)) goto handle_exception;
                            PUSH(v);
                            resolved = true;
                            break;
                        }
                    }
                }
                if (!resolved) {
                    int32_t lslot = env_find_named_slot(realm->global_lex_env, nm);
                    if (lslot >= 0) {
                        AjsValue v = AJS_VAL_UNDEFINED;
                        if (!env_read_slot(vm, realm->global_lex_env, (uint32_t)lslot, &v)) goto handle_exception;
                        PUSH(v);
                        break;
                    }
                    AjsObject *gobj = (AjsObject *)ajs_val_to_gc(realm->global_obj);
                    bool has = false;
                    if (!ajs_obj_has_prop(vm, gobj, key, &has)) goto handle_exception;
                    if (!has) {
                        if (typeof_mode) { PUSH(AJS_VAL_UNDEFINED); break; }
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE, "%s is not defined", nm->utf8);
                        goto handle_exception;
                    }
                    AjsValue v = ajs_obj_get(vm, realm->global_obj, key, realm->global_obj);
                    CHECK_EXC();
                    PUSH(v);
                }
                break;
            }
            case OP_SET_DYN_NAME: {
                uint32_t nidx = bc_u32(code, &frame->pc);
                uint8_t st = bc_u8(code, &frame->pc);
                AjsValue val = TOP();
                AjsValue key = bc->consts[nidx];
                AjsString *nm = (AjsString *)ajs_val_to_gc(key);
                bool resolved = false;
                for (AjsEnv *e = frame->env; e; e = e->parent) {
                    if (e->with_obj) {
                        bool has = false;
                        if (!with_obj_has_binding(vm, e->with_obj, key, &has)) goto handle_exception;
                        if (has) {
                            AjsValue wv = ajs_val_from_gc(e->with_obj);
                            if (!ajs_obj_set(vm, wv, key, val, wv, st != 0)) goto handle_exception;
                            resolved = true;
                            break;
                        }
                    } else {
                        int32_t sl = env_find_named_slot(e, nm);
                        if (sl >= 0) {
                            if (!env_write_slot(vm, e, (uint32_t)sl, val, st != 0)) goto handle_exception;
                            resolved = true;
                            break;
                        }
                    }
                }
                if (!resolved) {
                    int32_t lslot = env_find_named_slot(realm->global_lex_env, nm);
                    if (lslot >= 0) {
                        if (!env_write_slot(vm, realm->global_lex_env, (uint32_t)lslot, val, st != 0)) goto handle_exception;
                        break;
                    }
                    AjsObject *gobj = (AjsObject *)ajs_val_to_gc(realm->global_obj);
                    if (st) {
                        bool has = false;
                        if (!ajs_obj_has_prop(vm, gobj, key, &has)) goto handle_exception;
                        if (!has) {
                            ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE, "%s is not defined", nm->utf8);
                            goto handle_exception;
                        }
                    }
                    if (!ajs_obj_set(vm, realm->global_obj, key, val, realm->global_obj, st != 0)) goto handle_exception;
                }
                break;
            }
            case OP_DELETE_NAME: {
                uint32_t nidx = bc_u32(code, &frame->pc);
                uint8_t st = bc_u8(code, &frame->pc);
                (void)st;
                AjsValue key = bc->consts[nidx];
                AjsString *nm = (AjsString *)ajs_val_to_gc(key);
                bool handled = false;
                for (AjsEnv *e = frame->env; e; e = e->parent) {
                    if (e->with_obj) {
                        bool has = false;
                        if (!with_obj_has_binding(vm, e->with_obj, key, &has)) goto handle_exception;
                        if (has) {
                            bool ok = ajs_obj_delete(vm, e->with_obj, key, false);
                            CHECK_EXC();
                            PUSH(ok ? AJS_VAL_TRUE : AJS_VAL_FALSE);
                            handled = true;
                            break;
                        }
                    } else if (env_find_named_slot(e, nm) >= 0) {
                        PUSH(AJS_VAL_FALSE);
                        handled = true;
                        break;
                    }
                }
                if (!handled) {
                    if (env_find_named_slot(realm->global_lex_env, nm) >= 0) {
                        PUSH(AJS_VAL_FALSE);
                    } else {
                        AjsObject *gobj = (AjsObject *)ajs_val_to_gc(realm->global_obj);
                        bool ok = ajs_obj_delete(vm, gobj, key, false);
                        CHECK_EXC();
                        PUSH(ok ? AJS_VAL_TRUE : AJS_VAL_FALSE);
                    }
                }
                break;
            }

            case OP_PUSH_ENV:
            case OP_PUSH_ENV32: {
                uint32_t cnt = (op == OP_PUSH_ENV32) ? bc_u32(code, &frame->pc) : (uint32_t)bc_u16(code, &frame->pc);
                uint32_t ed_idx = AJS_NO_SLOT;
                if (op == OP_PUSH_ENV32) {
                    ed_idx = bc_u32(code, &frame->pc);
                } else {
                    uint16_t ed16 = bc_u16(code, &frame->pc);
                    ed_idx = (ed16 == 0xFFFFu) ? AJS_NO_SLOT : (uint32_t)ed16;
                }
                AjsString **names = (ed_idx != AJS_NO_SLOT && ed_idx < bc->env_desc_count)
                                    ? bc->env_descs[ed_idx].names : NULL;
                AjsEnv *ne = ajs_env_new(vm, frame->env, cnt, names);
                if (!ne || vm->has_exception) goto handle_exception;
                if (ed_idx != AJS_NO_SLOT && ed_idx < bc->env_desc_count) {
                    AjsEnvNamesDesc *ed = &bc->env_descs[ed_idx];
                    for (uint32_t i = 0; i < cnt && i < ed->count; i++) {
                        uint8_t fl = ed->slot_flags[i];
                        ne->flags[i] = (uint8_t)(fl & (AJS_SLOT_CONST | AJS_SLOT_FUNC_NAME));
                        if (fl & AJS_SLOT_INIT_TDZ) ne->slots[i] = AJS_VAL_TDZ;
                    }
                }
                frame->env = ne;
                frame->env_depth++;
                break;
            }
            case OP_POP_ENV:
                if (frame->env && frame->env->parent && frame->env_depth > 0) {
                    frame->env = frame->env->parent;
                    frame->env_depth--;
                }
                break;
            case OP_CLONE_ENV:
            case OP_CLONE_ENV32: {
                uint32_t cnt = (op == OP_CLONE_ENV32) ? bc_u32(code, &frame->pc) : (uint32_t)bc_u16(code, &frame->pc);
                AjsEnv *old_e = frame->env;
                if (old_e) {
                    AjsEnv *ce = ajs_env_new(vm, old_e->parent, cnt, old_e->names);
                    if (!ce || vm->has_exception) goto handle_exception;
                    uint32_t copy_n = cnt < old_e->count ? cnt : old_e->count;
                    for (uint32_t i = 0; i < copy_n; i++) {
                        ce->slots[i] = old_e->slots[i];
                        ce->flags[i] = old_e->flags[i];
                    }
                    frame->env = ce;
                }
                break;
            }
            case OP_PUSH_WITH: {
                AjsValue wv = POP();
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, wv);
                AjsObject *wobj = ajs_to_object(vm, wv);
                CHECK_EXC();
                ajs_temp_root_push(vm, ajs_val_from_gc(wobj));
                AjsEnv *we = ajs_env_new(vm, frame->env, 0, NULL);
                ajs_temp_root_restore(vm, rs);
                if (!we || vm->has_exception) goto handle_exception;
                we->with_obj = wobj;
                frame->env = we;
                frame->env_depth++;
                break;
            }

            case OP_NEW_OBJECT: {
                AjsObject *o = ajs_obj_new(vm, realm, AJS_OBJ_ORDINARY, realm->object_proto);
                if (!o || vm->has_exception) goto handle_exception;
                PUSH(ajs_val_from_gc(o));
                break;
            }
            case OP_NEW_OBJECT_PROTO: {
                AjsValue pv = POP();
                if (!ajs_is_null(pv) && !ajs_is_object(vm, pv)) pv = realm->object_proto;
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, pv);
                AjsObject *o = ajs_obj_new(vm, realm, AJS_OBJ_ORDINARY, pv);
                ajs_temp_root_restore(vm, rs);
                if (!o || vm->has_exception) goto handle_exception;
                PUSH(ajs_val_from_gc(o));
                break;
            }
            case OP_NEW_ARRAY: {
                uint32_t cap = bc_u32(code, &frame->pc);
                AjsObject *a = ajs_obj_new(vm, realm, AJS_OBJ_ARRAY, realm->array_proto);
                if (!a || vm->has_exception) goto handle_exception;
                a->u.arr.length_writable = true;
                if (cap > 0 && cap <= 4096u) {
                    a->u.arr.dense = (AjsValue *)malloc((size_t)cap * sizeof(AjsValue));
                    if (a->u.arr.dense) a->u.arr.dense_cap = cap;
                }
                PUSH(ajs_val_from_gc(a));
                break;
            }
            case OP_ARRAY_PUSH: {
                AjsValue val = POP();
                AjsObject *a = (AjsObject *)ajs_val_to_gc(TOP());
                uint32_t idx = a->u.arr.length;
                if (idx == a->u.arr.dense_len) {
                    if (a->u.arr.dense_len >= a->u.arr.dense_cap) {
                        uint32_t ncap = a->u.arr.dense_cap ? a->u.arr.dense_cap * 2u : 8u;
                        a->u.arr.dense = (AjsValue *)realloc(a->u.arr.dense, ncap * sizeof(AjsValue));
                        a->u.arr.dense_cap = ncap;
                    }
                    a->u.arr.dense[a->u.arr.dense_len++] = val;
                }
                a->u.arr.length = idx + 1u;
                break;
            }
            case OP_ARRAY_SPREAD: {
                AjsValue iterable = POP();
                AjsObject *a = (AjsObject *)ajs_val_to_gc(TOP());
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, iterable);
                AjsValue it = AJS_VAL_UNDEFINED, nfn = AJS_VAL_UNDEFINED;
                if (!get_iterator_internal(vm, realm, iterable, false, &it, &nfn)) {
                    ajs_temp_root_restore(vm, rs);
                    goto handle_exception;
                }
                ajs_temp_root_push(vm, it);
                ajs_temp_root_push(vm, nfn);
                for (;;) {
                    AjsValue sres = ajs_call_internal(vm, nfn, it, 0, NULL);
                    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                    if (!ajs_is_object(vm, sres)) {
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator result is not an object");
                        ajs_temp_root_restore(vm, rs);
                        goto handle_exception;
                    }
                    size_t isave = ajs_temp_root_save(vm);
                    ajs_temp_root_push(vm, sres);
                    AjsValue dv = ajs_obj_get(vm, sres, ajs_val_from_gc(vm->str_done), sres);
                    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                    if (ajs_to_bool(vm, dv)) {
                        ajs_temp_root_restore(vm, isave);
                        break;
                    }
                    AjsValue vv = ajs_obj_get(vm, sres, ajs_val_from_gc(vm->str_value), sres);
                    ajs_temp_root_restore(vm, isave);
                    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                    if (a->u.arr.dense_len >= a->u.arr.dense_cap) {
                        uint32_t ncap = a->u.arr.dense_cap ? a->u.arr.dense_cap * 2u : 8u;
                        a->u.arr.dense = (AjsValue *)realloc(a->u.arr.dense, ncap * sizeof(AjsValue));
                        a->u.arr.dense_cap = ncap;
                    }
                    a->u.arr.dense[a->u.arr.dense_len++] = vv;
                    a->u.arr.length = a->u.arr.dense_len;
                }
                ajs_temp_root_restore(vm, rs);
                break;
            }
            case OP_OBJECT_SPREAD: {
                AjsValue src = POP();
                if (ajs_is_nullish(src)) break;
                AjsObject *dst = (AjsObject *)ajs_val_to_gc(TOP());
                size_t rs = ajs_temp_root_save(vm);
                AjsObject *sobj = ajs_to_object(vm, src);
                if (vm->has_exception || !sobj) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                ajs_temp_root_push(vm, ajs_val_from_gc(sobj));
                AjsValue *keys = NULL;
                uint32_t kcnt = 0;
                if (!ajs_obj_own_keys(vm, sobj, false, true, true, &keys, &kcnt)) {
                    ajs_temp_root_restore(vm, rs);
                    goto handle_exception;
                }
                AjsValue sval = ajs_val_from_gc(sobj);
                for (uint32_t i = 0; i < kcnt; i++) {
                    AjsPropDesc pd;
                    bool fnd = false;
                    if (!ajs_obj_get_own_desc(vm, sobj, keys[i], &pd, &fnd)) {
                        free(keys); ajs_temp_root_restore(vm, rs); goto handle_exception;
                    }
                    if (fnd && (pd.flags & AJS_DESC_ENUMERABLE)) {
                        AjsValue pv = ajs_obj_get(vm, sval, keys[i], sval);
                        if (vm->has_exception) { free(keys); ajs_temp_root_restore(vm, rs); goto handle_exception; }
                        AjsPropDesc dd = {
                            .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                                     AJS_DESC_HAS_ENUMERABLE | AJS_DESC_ENUMERABLE |
                                     AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
                            .value = pv
                        };
                        if (!ajs_obj_define_own_prop(vm, dst, keys[i], &dd, true)) {
                            free(keys); ajs_temp_root_restore(vm, rs); goto handle_exception;
                        }
                    }
                }
                free(keys);
                ajs_temp_root_restore(vm, rs);
                break;
            }
            case OP_OBJECT_REST: {
                AjsValue excl_val = POP();
                AjsValue src = POP();
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, excl_val);
                ajs_temp_root_push(vm, src);
                AjsObject *rest = ajs_obj_new(vm, realm, AJS_OBJ_ORDINARY, realm->object_proto);
                if (!rest || vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                ajs_temp_root_push(vm, ajs_val_from_gc(rest));
                if (!ajs_is_nullish(src)) {
                    AjsObject *sobj = ajs_to_object(vm, src);
                    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                    ajs_temp_root_push(vm, ajs_val_from_gc(sobj));
                    AjsObject *earr = (AjsObject *)ajs_val_to_gc(excl_val);
                    AjsValue *keys = NULL;
                    uint32_t kcnt = 0;
                    if (!ajs_obj_own_keys(vm, sobj, false, true, true, &keys, &kcnt)) {
                        ajs_temp_root_restore(vm, rs); goto handle_exception;
                    }
                    AjsValue sval = ajs_val_from_gc(sobj);
                    for (uint32_t i = 0; i < kcnt; i++) {
                        bool excluded = false;
                        for (uint32_t j = 0; j < earr->u.arr.dense_len; j++) {
                            if (ajs_strict_eq(earr->u.arr.dense[j], keys[i])) { excluded = true; break; }
                        }
                        if (excluded) continue;
                        AjsPropDesc pd;
                        bool fnd = false;
                        if (!ajs_obj_get_own_desc(vm, sobj, keys[i], &pd, &fnd)) {
                            free(keys); ajs_temp_root_restore(vm, rs); goto handle_exception;
                        }
                        if (fnd && (pd.flags & AJS_DESC_ENUMERABLE)) {
                            AjsValue pv = ajs_obj_get(vm, sval, keys[i], sval);
                            if (vm->has_exception) { free(keys); ajs_temp_root_restore(vm, rs); goto handle_exception; }
                            AjsPropDesc dd = {
                                .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                                         AJS_DESC_HAS_ENUMERABLE | AJS_DESC_ENUMERABLE |
                                         AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
                                .value = pv
                            };
                            if (!ajs_obj_define_own_prop(vm, rest, keys[i], &dd, true)) {
                                free(keys); ajs_temp_root_restore(vm, rs); goto handle_exception;
                            }
                        }
                    }
                    free(keys);
                }
                ajs_temp_root_restore(vm, rs);
                PUSH(ajs_val_from_gc(rest));
                break;
            }

            case OP_GET_PROP: {
                uint32_t nidx = bc_u32(code, &frame->pc);
                AjsValue obj_v = POP();
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, obj_v);
                AjsValue res = ajs_obj_get(vm, obj_v, bc->consts[nidx], obj_v);
                ajs_temp_root_restore(vm, rs);
                CHECK_EXC();
                PUSH(res);
                break;
            }
            case OP_GET_ELEM: {
                AjsValue key_v = POP();
                AjsValue obj_v = POP();
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, obj_v);
                ajs_temp_root_push(vm, key_v);
                /* Fast path for dense Array integer index */
                if (ajs_val_is_gc_kind(obj_v, AJS_GC_OBJECT) && ajs_val_is_number(key_v)) {
                    AjsObject *ao = (AjsObject *)ajs_val_to_gc(obj_v);
                    double kd = ajs_val_to_number_unchecked(key_v);
                    if (ao->kind == AJS_OBJ_ARRAY && kd >= 0.0 && kd < (double)ao->u.arr.dense_len) {
                        uint32_t idx = (uint32_t)kd;
                        if ((double)idx == kd && ao->u.arr.dense[idx] != AJS_VAL_EMPTY) {
                            AjsValue fv = ao->u.arr.dense[idx];
                            ajs_temp_root_restore(vm, rs);
                            PUSH(fv);
                            break;
                        }
                    }
                }
                AjsValue pk = ajs_to_property_key(vm, key_v);
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                ajs_temp_root_push(vm, pk);
                AjsValue res = ajs_obj_get(vm, obj_v, pk, obj_v);
                ajs_temp_root_restore(vm, rs);
                CHECK_EXC();
                PUSH(res);
                break;
            }
            case OP_SET_PROP: {
                uint32_t nidx = bc_u32(code, &frame->pc);
                uint8_t st = bc_u8(code, &frame->pc);
                AjsValue val = POP();
                AjsValue obj_v = POP();
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, obj_v);
                ajs_temp_root_push(vm, val);
                bool ok = ajs_obj_set(vm, obj_v, bc->consts[nidx], val, obj_v, st != 0);
                ajs_temp_root_restore(vm, rs);
                if (!ok || vm->has_exception) goto handle_exception;
                PUSH(val);
                break;
            }
            case OP_SET_ELEM: {
                uint8_t st = bc_u8(code, &frame->pc);
                AjsValue val = POP();
                AjsValue key_v = POP();
                AjsValue obj_v = POP();
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, obj_v);
                ajs_temp_root_push(vm, key_v);
                ajs_temp_root_push(vm, val);
                AjsValue pk = ajs_to_property_key(vm, key_v);
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                ajs_temp_root_push(vm, pk);
                bool ok = ajs_obj_set(vm, obj_v, pk, val, obj_v, st != 0);
                ajs_temp_root_restore(vm, rs);
                if (!ok || vm->has_exception) goto handle_exception;
                PUSH(val);
                break;
            }
            case OP_DEFINE_PROP:
            case OP_DEFINE_ELEM: {
                AjsValue val = POP();
                AjsValue key = AJS_VAL_UNDEFINED;
                uint8_t kind = 0;
                if (op == OP_DEFINE_PROP) {
                    uint32_t nidx = bc_u32(code, &frame->pc);
                    kind = bc_u8(code, &frame->pc);
                    key = bc->consts[nidx];
                } else {
                    kind = bc_u8(code, &frame->pc);
                    key = POP();
                }
                AjsValue tgt_v = TOP();
                if (!ajs_val_is_gc_kind(tgt_v, AJS_GC_OBJECT)) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "DefineOwnProperty target is not an object");
                    goto handle_exception;
                }
                AjsObject *tgt = (AjsObject *)ajs_val_to_gc(tgt_v);
                bool is_cls = (kind & 0x10u) != 0;
                uint8_t kbase = (uint8_t)(kind & 0x0Fu);
                uint32_t enum_fl = is_cls ? 0u : AJS_DESC_ENUMERABLE;
                AjsPropDesc desc;
                memset(&desc, 0, sizeof(desc));
                if (kbase == 1u) {
                    desc.flags = AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_GET |
                                 AJS_DESC_HAS_ENUMERABLE | enum_fl |
                                 AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
                    desc.getter = val;
                } else if (kbase == 2u) {
                    desc.flags = AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_SET |
                                 AJS_DESC_HAS_ENUMERABLE | enum_fl |
                                 AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
                    desc.setter = val;
                } else {
                    desc.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                                 AJS_DESC_HAS_ENUMERABLE | enum_fl |
                                 AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
                    desc.value = val;
                }
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, key);
                ajs_temp_root_push(vm, val);
                bool ok = ajs_obj_define_own_prop(vm, tgt, key, &desc, true);
                ajs_temp_root_restore(vm, rs);
                if (!ok || vm->has_exception) goto handle_exception;
                break;
            }
            case OP_SET_PROTO_LIT: {
                AjsValue pv = POP();
                AjsObject *o = (AjsObject *)ajs_val_to_gc(TOP());
                if (ajs_is_null(pv) || ajs_is_object(vm, pv)) {
                    if (!ajs_obj_set_proto(vm, o, pv, true)) goto handle_exception;
                }
                break;
            }
            case OP_DELETE_PROP: {
                uint32_t nidx = bc_u32(code, &frame->pc);
                uint8_t st = bc_u8(code, &frame->pc);
                AjsValue obj_v = POP();
                if (ajs_is_nullish(obj_v)) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot convert undefined or null to object");
                    goto handle_exception;
                }
                size_t rs = ajs_temp_root_save(vm);
                AjsObject *o = ajs_to_object(vm, obj_v);
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                ajs_temp_root_push(vm, ajs_val_from_gc(o));
                bool ok = ajs_obj_delete(vm, o, bc->consts[nidx], st != 0);
                ajs_temp_root_restore(vm, rs);
                CHECK_EXC();
                PUSH(ok ? AJS_VAL_TRUE : AJS_VAL_FALSE);
                break;
            }
            case OP_DELETE_ELEM: {
                uint8_t st = bc_u8(code, &frame->pc);
                AjsValue key_v = POP();
                AjsValue obj_v = POP();
                if (ajs_is_nullish(obj_v)) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot convert undefined or null to object");
                    goto handle_exception;
                }
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, key_v);
                AjsObject *o = ajs_to_object(vm, obj_v);
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                ajs_temp_root_push(vm, ajs_val_from_gc(o));
                AjsValue pk = ajs_to_property_key(vm, key_v);
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                ajs_temp_root_push(vm, pk);
                bool ok = ajs_obj_delete(vm, o, pk, st != 0);
                ajs_temp_root_restore(vm, rs);
                CHECK_EXC();
                PUSH(ok ? AJS_VAL_TRUE : AJS_VAL_FALSE);
                break;
            }

            case OP_GET_SUPER_PROP:
            case OP_GET_SUPER_ELEM:
            case OP_SET_SUPER_PROP:
            case OP_SET_SUPER_ELEM: {
                uint16_t d = bc_u16(code, &frame->pc);
                uint32_t fn_slot = bc_u32(code, &frame->pc);
                AjsValue key = AJS_VAL_UNDEFINED, rhs = AJS_VAL_UNDEFINED, recv = AJS_VAL_UNDEFINED;
                uint8_t st = 0;
                if (op == OP_GET_SUPER_PROP) {
                    uint32_t nidx = bc_u32(code, &frame->pc);
                    key = bc->consts[nidx];
                    recv = POP();
                } else if (op == OP_GET_SUPER_ELEM) {
                    AjsValue kv = POP();
                    recv = POP();
                    size_t r0 = ajs_temp_root_save(vm);
                    ajs_temp_root_push(vm, recv);
                    key = ajs_to_property_key(vm, kv);
                    ajs_temp_root_restore(vm, r0);
                    CHECK_EXC();
                } else if (op == OP_SET_SUPER_PROP) {
                    uint32_t nidx = bc_u32(code, &frame->pc);
                    st = bc_u8(code, &frame->pc);
                    key = bc->consts[nidx];
                    rhs = POP();
                    recv = POP();
                } else {
                    st = bc_u8(code, &frame->pc);
                    rhs = POP();
                    key = POP();
                    recv = POP();
                }
                if (recv == AJS_VAL_TDZ) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE,
                                       "Must call super constructor in derived class before accessing 'this'");
                    goto handle_exception;
                }
                AjsObject *fn_obj = frame->func_obj;
                if (fn_slot != AJS_NO_SLOT) {
                    AjsEnv *fe = env_at_depth(frame->env, d);
                    if (fe && fn_slot < fe->count && ajs_val_is_gc_kind(fe->slots[fn_slot], AJS_GC_OBJECT)) {
                        fn_obj = (AjsObject *)ajs_val_to_gc(fe->slots[fn_slot]);
                    }
                }
                if (!fn_obj || !ajs_val_is_gc_kind(fn_obj->u.fn.home_object, AJS_GC_OBJECT)) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "'super' referenced outside method with home object");
                    goto handle_exception;
                }
                AjsObject *home = (AjsObject *)ajs_val_to_gc(fn_obj->u.fn.home_object);
                AjsValue super_base = AJS_VAL_NULL;
                if (!ajs_obj_get_proto(vm, home, &super_base)) goto handle_exception;

                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, super_base);
                ajs_temp_root_push(vm, recv);
                ajs_temp_root_push(vm, key);
                ajs_temp_root_push(vm, rhs);
                if (op == OP_GET_SUPER_PROP || op == OP_GET_SUPER_ELEM) {
                    if (ajs_is_nullish(super_base)) {
                        ajs_temp_root_restore(vm, rs);
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot read properties of null (reading super)");
                        goto handle_exception;
                    }
                    AjsValue res = ajs_obj_get(vm, super_base, key, recv);
                    ajs_temp_root_restore(vm, rs);
                    CHECK_EXC();
                    PUSH(res);
                } else {
                    if (ajs_is_nullish(super_base)) {
                        ajs_temp_root_restore(vm, rs);
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot set properties of null (setting super)");
                        goto handle_exception;
                    }
                    bool ok = ajs_obj_set(vm, super_base, key, rhs, recv, st != 0);
                    ajs_temp_root_restore(vm, rs);
                    if (!ok || vm->has_exception) goto handle_exception;
                    PUSH(rhs);
                }
                break;
            }

            case OP_NEW_PRIV_SYM: {
                uint32_t d_idx = bc_u32(code, &frame->pc);
                AjsSymbol *sym = (AjsSymbol *)ajs_gc_alloc(vm, sizeof(AjsSymbol), AJS_GC_SYMBOL);
                if (!sym || vm->has_exception) goto handle_exception;
                sym->id = ++vm->next_symbol_id;
                sym->is_private = true;
                if (d_idx < bc->const_count && ajs_val_is_gc_kind(bc->consts[d_idx], AJS_GC_STRING)) {
                    sym->has_desc = true;
                    sym->desc = (AjsString *)ajs_val_to_gc(bc->consts[d_idx]);
                }
                PUSH(ajs_val_from_gc(sym));
                break;
            }
            case OP_DEF_PRIVATE: {
                uint16_t d = bc_u16(code, &frame->pc);
                uint32_t s = bc_u32(code, &frame->pc);
                uint8_t pk = bc_u8(code, &frame->pc);
                AjsValue val = POP();
                AjsValue tgt_v = TOP();
                if (!ajs_val_is_gc_kind(tgt_v, AJS_GC_OBJECT)) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Private member target is not an object");
                    goto handle_exception;
                }
                AjsEnv *e = env_at_depth(frame->env, d);
                AjsSymbol *sym = (AjsSymbol *)ajs_val_to_gc(e->slots[s]);
                AjsObject *tgt = (AjsObject *)ajs_val_to_gc(tgt_v);
                /* Propagate [[HomeObject]] for instance private methods/accessors installed in <instance_init> */
                if (pk >= 2 && ajs_val_is_gc_kind(val, AJS_GC_OBJECT)) {
                    AjsObject *mfn = (AjsObject *)ajs_val_to_gc(val);
                    if (mfn->kind == AJS_OBJ_FUNCTION && mfn->u.fn.home_object == AJS_VAL_UNDEFINED &&
                        frame->func_obj && frame->func_obj->u.fn.home_object != AJS_VAL_UNDEFINED) {
                        mfn->u.fn.home_object = frame->func_obj->u.fn.home_object;
                    }
                }
                bool ok = false;
                if (pk == 1) ok = ajs_priv_define(vm, tgt, sym, AJS_PRIV_FIELD, val, AJS_VAL_UNDEFINED);
                else if (pk == 2) ok = ajs_priv_define(vm, tgt, sym, AJS_PRIV_METHOD, val, AJS_VAL_UNDEFINED);
                else if (pk == 3) ok = ajs_priv_define(vm, tgt, sym, AJS_PRIV_ACCESSOR, val, AJS_VAL_UNDEFINED);
                else ok = ajs_priv_define(vm, tgt, sym, AJS_PRIV_ACCESSOR, AJS_VAL_UNDEFINED, val);
                if (!ok || vm->has_exception) goto handle_exception;
                break;
            }
            case OP_GET_PRIVATE: {
                uint16_t d = bc_u16(code, &frame->pc);
                uint32_t s = bc_u32(code, &frame->pc);
                AjsValue tgt_v = POP();
                AjsEnv *e = env_at_depth(frame->env, d);
                AjsSymbol *sym = (AjsSymbol *)ajs_val_to_gc(e->slots[s]);
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, tgt_v);
                AjsValue res = ajs_priv_get(vm, tgt_v, sym);
                ajs_temp_root_restore(vm, rs);
                CHECK_EXC();
                PUSH(res);
                break;
            }
            case OP_SET_PRIVATE: {
                uint16_t d = bc_u16(code, &frame->pc);
                uint32_t s = bc_u32(code, &frame->pc);
                AjsValue val = POP();
                AjsValue tgt_v = POP();
                AjsEnv *e = env_at_depth(frame->env, d);
                AjsSymbol *sym = (AjsSymbol *)ajs_val_to_gc(e->slots[s]);
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, tgt_v);
                ajs_temp_root_push(vm, val);
                bool ok = ajs_priv_set(vm, tgt_v, sym, val);
                ajs_temp_root_restore(vm, rs);
                if (!ok || vm->has_exception) goto handle_exception;
                PUSH(val);
                break;
            }
            case OP_IN_PRIVATE: {
                uint16_t d = bc_u16(code, &frame->pc);
                uint32_t s = bc_u32(code, &frame->pc);
                AjsValue tgt_v = POP();
                AjsEnv *e = env_at_depth(frame->env, d);
                AjsSymbol *sym = (AjsSymbol *)ajs_val_to_gc(e->slots[s]);
                bool has = false;
                if (!ajs_priv_has(vm, tgt_v, sym, &has)) goto handle_exception;
                PUSH(has ? AJS_VAL_TRUE : AJS_VAL_FALSE);
                break;
            }

            case OP_CLOSURE: {
                uint32_t bc_idx = bc_u32(code, &frame->pc);
                AjsBytecode *cbc = (AjsBytecode *)ajs_val_to_gc(bc->consts[bc_idx]);
                AjsModuleRecord *m = frame->module;
                if (!m && frame->func_obj) m = frame->func_obj->u.fn.owner_module;
                AjsObject *fn = ajs_closure_new(vm, realm, cbc, frame->env, m);
                if (!fn || vm->has_exception) goto handle_exception;
                PUSH(ajs_val_from_gc(fn));
                break;
            }
            case OP_SET_FN_NAME: {
                uint8_t pfx = bc_u8(code, &frame->pc);
                AjsValue key_v = POP();
                AjsValue fn_v = TOP();
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, key_v);
                bool ok = set_inferred_fn_name(vm, fn_v, key_v, pfx);
                ajs_temp_root_restore(vm, rs);
                if (!ok || vm->has_exception) goto handle_exception;
                break;
            }
            case OP_SET_HOME_OBJ: {
                AjsValue fn_v = frame->stack[frame->sp - 1u];
                AjsValue home_v = frame->stack[frame->sp - 2u];
                if (ajs_val_is_gc_kind(fn_v, AJS_GC_OBJECT)) {
                    AjsObject *fn = (AjsObject *)ajs_val_to_gc(fn_v);
                    if (fn->kind == AJS_OBJ_FUNCTION) fn->u.fn.home_object = home_v;
                }
                break;
            }
            case OP_BUILD_CLASS: {
                uint32_t name_idx = bc_u32(code, &frame->pc);
                uint8_t has_super = bc_u8(code, &frame->pc);
                AjsValue inst_init_v = POP();
                AjsValue ctor_v = POP();
                AjsValue super_v = POP();

                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, inst_init_v);
                ajs_temp_root_push(vm, ctor_v);
                ajs_temp_root_push(vm, super_v);

                AjsValue proto_parent = realm->object_proto;
                AjsValue ctor_parent = realm->function_proto;
                if (has_super) {
                    if (ajs_is_null(super_v)) {
                        proto_parent = AJS_VAL_NULL;
                        ctor_parent = realm->function_proto;
                    } else if (!ajs_is_constructor_val(super_v)) {
                        ajs_temp_root_restore(vm, rs);
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                           "Class extends value is not a constructor or null");
                        goto handle_exception;
                    } else {
                        proto_parent = ajs_obj_get(vm, super_v, ajs_val_from_gc(vm->str_prototype), super_v);
                        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                        if (!ajs_is_null(proto_parent) && !ajs_is_object(vm, proto_parent)) {
                            ajs_temp_root_restore(vm, rs);
                            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                               "Class extends value does not have valid prototype");
                            goto handle_exception;
                        }
                        ctor_parent = super_v;
                    }
                }
                ajs_temp_root_push(vm, proto_parent);

                AjsObject *proto = ajs_obj_new(vm, realm, AJS_OBJ_ORDINARY, proto_parent);
                if (!proto || vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                AjsValue proto_v = ajs_val_from_gc(proto);
                ajs_temp_root_push(vm, proto_v);

                AjsObject *ctor = (AjsObject *)ajs_val_to_gc(ctor_v);
                ctor->proto = ctor_parent;
                ctor->u.fn.is_constructor = true;
                ctor->u.fn.is_derived_ctor = (has_super != 0);
                ctor->u.fn.home_object = proto_v;
                ctor->u.fn.inst_init_fn = inst_init_v;

                if (ajs_val_is_gc_kind(inst_init_v, AJS_GC_OBJECT)) {
                    AjsObject *iif = (AjsObject *)ajs_val_to_gc(inst_init_v);
                    if (iif->kind == AJS_OBJ_FUNCTION) iif->u.fn.home_object = proto_v;
                }

                /* Set class `name` on constructor if missing/empty */
                set_inferred_fn_name(vm, ctor_v, bc->consts[name_idx], 0);

                AjsPropDesc cdesc = {
                    .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                             AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
                    .value = ctor_v
                };
                ajs_obj_define_own_prop(vm, proto, ajs_val_from_gc(vm->str_constructor), &cdesc, false);

                AjsPropDesc pdesc = {
                    .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE |
                             AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE,
                    .value = proto_v
                };
                ajs_obj_define_own_prop(vm, ctor, ajs_val_from_gc(vm->str_prototype), &pdesc, false);

                ajs_temp_root_restore(vm, rs);
                PUSH(ctor_v);
                PUSH(proto_v);
                break;
            }
            case OP_SUPER_CALL: {
                uint16_t d = bc_u16(code, &frame->pc);
                uint32_t this_slot = bc_u32(code, &frame->pc);
                uint32_t nt_slot = bc_u32(code, &frame->pc);
                uint32_t fn_slot = bc_u32(code, &frame->pc);
                AjsValue args_arr_v = POP();

                AjsEnv *fe = env_at_depth(frame->env, d);
                AjsValue active_fn_v = (fe && fn_slot < fe->count) ? fe->slots[fn_slot] :
                                       (frame->func_obj ? ajs_val_from_gc(frame->func_obj) : AJS_VAL_UNDEFINED);
                AjsValue nt_v = (fe && nt_slot < fe->count) ? fe->slots[nt_slot] : frame->new_target;
                if (!ajs_val_is_gc_kind(active_fn_v, AJS_GC_OBJECT)) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Invalid super() active constructor");
                    goto handle_exception;
                }
                AjsObject *active_fn = (AjsObject *)ajs_val_to_gc(active_fn_v);
                AjsValue super_ctor = AJS_VAL_NULL;
                if (!ajs_obj_get_proto(vm, active_fn, &super_ctor)) goto handle_exception;
                if (!ajs_is_constructor_val(super_ctor)) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Super constructor is not a constructor");
                    goto handle_exception;
                }

                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, args_arr_v);
                ajs_temp_root_push(vm, super_ctor);
                ajs_temp_root_push(vm, active_fn_v);
                ajs_temp_root_push(vm, nt_v);

                AjsValue *sargv = NULL;
                int sargc = 0;
                if (!extract_array_args(vm, args_arr_v, &sargv, &sargc)) {
                    ajs_temp_root_restore(vm, rs);
                    goto handle_exception;
                }
                for (int i = 0; i < sargc; i++) ajs_temp_root_push(vm, sargv[i]);
                AjsValue new_this = ajs_construct_internal(vm, super_ctor, sargc, sargv, nt_v);
                free(sargv);
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                ajs_temp_root_push(vm, new_this);

                /* Bind `this` BEFORE running derived `<instance_init>` (checking duplicate `super()` call!) */
                if (fe && this_slot < fe->count) {
                    if (fe->slots[this_slot] != AJS_VAL_TDZ) {
                        ajs_temp_root_restore(vm, rs);
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE, "Super constructor may only be called once");
                        goto handle_exception;
                    }
                    fe->slots[this_slot] = new_this;
                }
                if (fe == frame->base_env) frame->this_val = new_this;

                if (active_fn->u.fn.inst_init_fn != AJS_VAL_UNDEFINED) {
                    ajs_call_internal(vm, active_fn->u.fn.inst_init_fn, new_this, 0, NULL);
                    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                }

                ajs_temp_root_restore(vm, rs);
                PUSH(new_this);
                break;
            }
            case OP_RUN_INST_INIT: {
                if (frame->func_obj && frame->func_obj->u.fn.inst_init_fn != AJS_VAL_UNDEFINED) {
                    ajs_call_internal(vm, frame->func_obj->u.fn.inst_init_fn, frame->this_val, 0, NULL);
                    CHECK_EXC();
                }
                break;
            }
            case OP_CHECK_THIS_TDZ: {
                if (TOP() == AJS_VAL_TDZ) {
                    (void)POP();
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE,
                                       "Cannot access uninitialized 'this' or delete super property");
                    goto handle_exception;
                }
                break;
            }
            case OP_REGEXP: {
                uint32_t pidx = bc_u32(code, &frame->pc);
                uint32_t fidx = bc_u32(code, &frame->pc);
                AjsString *ps = (AjsString *)ajs_val_to_gc(bc->consts[pidx]);
                AjsString *fs = (AjsString *)ajs_val_to_gc(bc->consts[fidx]);
                AjsValue rx = ajs_regexp_new_obj(vm, realm, ps, fs);
                CHECK_EXC();
                PUSH(rx);
                break;
            }
            case OP_TEMPLATE_OBJ: {
                uint32_t tid = bc_u32(code, &frame->pc);
                AjsValue to = get_template_object(vm, realm, bc, tid);
                CHECK_EXC();
                PUSH(to);
                break;
            }

/* ============================================================================
 * 7. Bytecode Interpreter Loop (`ajs_run_frame`) - Part B
 * ============================================================================ */

            case OP_ADD: {
                AjsValue rv = POP();
                AjsValue lv = POP();
                if (ajs_val_is_number(lv) && ajs_val_is_number(rv)) {
                    double ld = ajs_val_to_number_unchecked(lv);
                    double rd = ajs_val_to_number_unchecked(rv);
                    PUSH(ajs_val_number(ajs_f64_round(ld + rd)));
                    break;
                }
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, lv);
                ajs_temp_root_push(vm, rv);
                AjsValue lp = ajs_to_primitive(vm, lv, 0);
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                ajs_temp_root_push(vm, lp);
                AjsValue rp = ajs_to_primitive(vm, rv, 0);
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                ajs_temp_root_push(vm, rp);

                if (ajs_val_is_gc_kind(lp, AJS_GC_STRING) || ajs_val_is_gc_kind(rp, AJS_GC_STRING)) {
                    AjsString *ls = ajs_to_js_string(vm, lp);
                    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                    ajs_temp_root_push(vm, ajs_val_from_gc(ls));
                    AjsString *rs_str = ajs_to_js_string(vm, rp);
                    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                    ajs_temp_root_push(vm, ajs_val_from_gc(rs_str));
                    AjsString *cat = ajs_str_concat(vm, ls, rs_str);
                    ajs_temp_root_restore(vm, rs);
                    PUSH(ajs_val_from_gc(cat));
                    break;
                }

                AjsValue ln = AJS_VAL_UNDEFINED, rn = AJS_VAL_UNDEFINED;
                if (!ajs_to_numeric(vm, lp, &ln)) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                ajs_temp_root_push(vm, ln);
                if (!ajs_to_numeric(vm, rp, &rn)) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                ajs_temp_root_push(vm, rn);

                bool lb = ajs_val_is_gc_kind(ln, AJS_GC_BIGINT);
                bool rb = ajs_val_is_gc_kind(rn, AJS_GC_BIGINT);
                if (lb != rb) {
                    ajs_temp_root_restore(vm, rs);
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                       "Cannot mix BigInt and other types, use explicit conversions");
                    goto handle_exception;
                }
                if (lb) {
                    AjsBigInt *bi = ajs_bigint_add(vm, (const AjsBigInt *)ajs_val_to_gc(ln),
                                                   (const AjsBigInt *)ajs_val_to_gc(rn));
                    ajs_temp_root_restore(vm, rs);
                    CHECK_EXC();
                    PUSH(ajs_val_from_gc(bi));
                } else {
                    double res = ajs_f64_round(ajs_val_to_number_unchecked(ln) + ajs_val_to_number_unchecked(rn));
                    ajs_temp_root_restore(vm, rs);
                    PUSH(ajs_val_number(res));
                }
                break;
            }

            case OP_SUB:
            case OP_MUL:
            case OP_DIV:
            case OP_MOD:
            case OP_EXP:
            case OP_BIT_AND:
            case OP_BIT_OR:
            case OP_BIT_XOR:
            case OP_SHL:
            case OP_SHR:
            case OP_USHR: {
                AjsValue rv = POP();
                AjsValue lv = POP();
                if (ajs_val_is_number(lv) && ajs_val_is_number(rv)) {
                    double ld = ajs_val_to_number_unchecked(lv);
                    double rd = ajs_val_to_number_unchecked(rv);
                    double res = 0.0;
                    switch (op) {
                        case OP_SUB: res = ajs_f64_round(ld - rd); break;
                        case OP_MUL: res = ajs_f64_round(ld * rd); break;
                        case OP_DIV: res = ajs_f64_round(ld / rd); break;
                        case OP_MOD: res = ajs_f64_round(fmod(ld, rd)); break;
                        case OP_EXP:
                            if (isnan(rd)) res = NAN;
                            else if (rd == 0.0) res = 1.0;
                            else if (isnan(ld)) res = NAN;
                            else if (fabs(ld) == 1.0 && isinf(rd)) res = NAN;
                            else res = ajs_f64_round(pow(ld, rd));
                            break;
                        case OP_BIT_AND: res = (double)(ajs_double_to_int32(ld) & ajs_double_to_int32(rd)); break;
                        case OP_BIT_OR:  res = (double)(ajs_double_to_int32(ld) | ajs_double_to_int32(rd)); break;
                        case OP_BIT_XOR: res = (double)(ajs_double_to_int32(ld) ^ ajs_double_to_int32(rd)); break;
                        case OP_SHL: {
                            int32_t li = ajs_double_to_int32(ld);
                            uint32_t sh = ajs_double_to_uint32(rd) & 0x1Fu;
                            res = (double)((int32_t)((uint32_t)li << sh));
                            break;
                        }
                        case OP_SHR: {
                            int32_t li = ajs_double_to_int32(ld);
                            uint32_t sh = ajs_double_to_uint32(rd) & 0x1Fu;
                            res = (double)(li >> sh);
                            break;
                        }
                        case OP_USHR: {
                            uint32_t lu = ajs_double_to_uint32(ld);
                            uint32_t sh = ajs_double_to_uint32(rd) & 0x1Fu;
                            res = (double)(lu >> sh);
                            break;
                        }
                        default: break;
                    }
                    PUSH(ajs_val_number(res));
                    break;
                }

                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, lv);
                ajs_temp_root_push(vm, rv);
                AjsValue ln = AJS_VAL_UNDEFINED, rn = AJS_VAL_UNDEFINED;
                if (!ajs_to_numeric(vm, lv, &ln)) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                ajs_temp_root_push(vm, ln);
                if (!ajs_to_numeric(vm, rv, &rn)) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                ajs_temp_root_push(vm, rn);

                bool lb = ajs_val_is_gc_kind(ln, AJS_GC_BIGINT);
                bool rb = ajs_val_is_gc_kind(rn, AJS_GC_BIGINT);
                if (lb != rb) {
                    ajs_temp_root_restore(vm, rs);
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                       "Cannot mix BigInt and other types, use explicit conversions");
                    goto handle_exception;
                }
                if (lb) {
                    const AjsBigInt *ba = (const AjsBigInt *)ajs_val_to_gc(ln);
                    const AjsBigInt *bb = (const AjsBigInt *)ajs_val_to_gc(rn);
                    AjsBigInt *bres = NULL;
                    switch (op) {
                        case OP_SUB: bres = ajs_bigint_sub(vm, ba, bb); break;
                        case OP_MUL: bres = ajs_bigint_mul(vm, ba, bb); break;
                        case OP_DIV: bres = ajs_bigint_div(vm, ba, bb); break;
                        case OP_MOD: bres = ajs_bigint_mod(vm, ba, bb); break;
                        case OP_EXP: bres = ajs_bigint_exp(vm, ba, bb); break;
                        case OP_BIT_AND: bres = ajs_bigint_bitop(vm, ba, bb, '&'); break;
                        case OP_BIT_OR:  bres = ajs_bigint_bitop(vm, ba, bb, '|'); break;
                        case OP_BIT_XOR: bres = ajs_bigint_bitop(vm, ba, bb, '^'); break;
                        case OP_SHL:     bres = ajs_bigint_shl(vm, ba, bb); break;
                        case OP_SHR:     bres = ajs_bigint_shr(vm, ba, bb); break;
                        case OP_USHR:
                            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                               "BigInts have no unsigned right shift, use >> instead");
                            break;
                        default: break;
                    }
                    ajs_temp_root_restore(vm, rs);
                    if (vm->has_exception || !bres) goto handle_exception;
                    PUSH(ajs_val_from_gc(bres));
                } else {
                    double ld = ajs_val_to_number_unchecked(ln);
                    double rd = ajs_val_to_number_unchecked(rn);
                    ajs_temp_root_restore(vm, rs);
                    double res = 0.0;
                    switch (op) {
                        case OP_SUB: res = ajs_f64_round(ld - rd); break;
                        case OP_MUL: res = ajs_f64_round(ld * rd); break;
                        case OP_DIV: res = ajs_f64_round(ld / rd); break;
                        case OP_MOD: res = ajs_f64_round(fmod(ld, rd)); break;
                        case OP_EXP:
                            if (isnan(rd)) res = NAN;
                            else if (rd == 0.0) res = 1.0;
                            else if (isnan(ld)) res = NAN;
                            else if (fabs(ld) == 1.0 && isinf(rd)) res = NAN;
                            else res = ajs_f64_round(pow(ld, rd));
                            break;
                        case OP_BIT_AND: res = (double)(ajs_double_to_int32(ld) & ajs_double_to_int32(rd)); break;
                        case OP_BIT_OR:  res = (double)(ajs_double_to_int32(ld) | ajs_double_to_int32(rd)); break;
                        case OP_BIT_XOR: res = (double)(ajs_double_to_int32(ld) ^ ajs_double_to_int32(rd)); break;
                        case OP_SHL:     res = (double)((int32_t)((uint32_t)ajs_double_to_int32(ld) << (ajs_double_to_uint32(rd) & 0x1Fu))); break;
                        case OP_SHR:     res = (double)(ajs_double_to_int32(ld) >> (ajs_double_to_uint32(rd) & 0x1Fu)); break;
                        case OP_USHR:    res = (double)(ajs_double_to_uint32(ld) >> (ajs_double_to_uint32(rd) & 0x1Fu)); break;
                        default: break;
                    }
                    PUSH(ajs_val_number(res));
                }
                break;
            }

            case OP_NEG: {
                AjsValue v = POP();
                if (ajs_val_is_number(v)) {
                    PUSH(ajs_val_number(-ajs_val_to_number_unchecked(v)));
                    break;
                }
                AjsValue nv = AJS_VAL_UNDEFINED;
                if (!ajs_to_numeric(vm, v, &nv)) goto handle_exception;
                if (ajs_val_is_gc_kind(nv, AJS_GC_BIGINT)) {
                    AjsBigInt *nb = ajs_bigint_neg(vm, (const AjsBigInt *)ajs_val_to_gc(nv));
                    CHECK_EXC();
                    PUSH(ajs_val_from_gc(nb));
                } else {
                    PUSH(ajs_val_number(-ajs_val_to_number_unchecked(nv)));
                }
                break;
            }
            case OP_POS: {
                AjsValue v = POP();
                double d = 0.0;
                if (!ajs_to_number(vm, v, &d)) goto handle_exception;
                PUSH(ajs_val_number(d));
                break;
            }
            case OP_TO_NUMERIC: {
                AjsValue v = POP();
                AjsValue nv = AJS_VAL_UNDEFINED;
                if (!ajs_to_numeric(vm, v, &nv)) goto handle_exception;
                PUSH(nv);
                break;
            }
            case OP_INC_NUM:
            case OP_DEC_NUM: {
                AjsValue v = POP();
                double step = (op == OP_INC_NUM) ? 1.0 : -1.0;
                if (ajs_val_is_number(v)) {
                    PUSH(ajs_val_number(ajs_f64_round(ajs_val_to_number_unchecked(v) + step)));
                } else if (ajs_val_is_gc_kind(v, AJS_GC_BIGINT)) {
                    size_t rs = ajs_temp_root_save(vm);
                    ajs_temp_root_push(vm, v);
                    AjsBigInt *one = ajs_bigint_new_i64(vm, 1);
                    ajs_temp_root_push(vm, ajs_val_from_gc(one));
                    AjsBigInt *res = (op == OP_INC_NUM)
                        ? ajs_bigint_add(vm, (const AjsBigInt *)ajs_val_to_gc(v), one)
                        : ajs_bigint_sub(vm, (const AjsBigInt *)ajs_val_to_gc(v), one);
                    ajs_temp_root_restore(vm, rs);
                    CHECK_EXC();
                    PUSH(ajs_val_from_gc(res));
                } else {
                    double d = 0.0;
                    if (!ajs_to_number(vm, v, &d)) goto handle_exception;
                    PUSH(ajs_val_number(ajs_f64_round(d + step)));
                }
                break;
            }
            case OP_BIT_NOT: {
                AjsValue v = POP();
                AjsValue nv = AJS_VAL_UNDEFINED;
                if (!ajs_to_numeric(vm, v, &nv)) goto handle_exception;
                if (ajs_val_is_gc_kind(nv, AJS_GC_BIGINT)) {
                    AjsBigInt *nb = ajs_bigint_not(vm, (const AjsBigInt *)ajs_val_to_gc(nv));
                    CHECK_EXC();
                    PUSH(ajs_val_from_gc(nb));
                } else {
                    int32_t iv = ajs_double_to_int32(ajs_val_to_number_unchecked(nv));
                    PUSH(ajs_val_number((double)(~iv)));
                }
                break;
            }
            case OP_NOT: {
                AjsValue v = POP();
                PUSH(ajs_to_bool(vm, v) ? AJS_VAL_FALSE : AJS_VAL_TRUE);
                break;
            }
            case OP_VOID: {
                (void)POP();
                PUSH(AJS_VAL_UNDEFINED);
                break;
            }
            case OP_TYPEOF: {
                AjsValue v = POP();
                const char *ts = "undefined";
                switch (ajs_typeof(vm, v)) {
                    case AJS_TYPE_UNDEFINED: ts = "undefined"; break;
                    case AJS_TYPE_NULL:      ts = "object";    break;
                    case AJS_TYPE_BOOLEAN:   ts = "boolean";   break;
                    case AJS_TYPE_NUMBER:    ts = "number";    break;
                    case AJS_TYPE_STRING:    ts = "string";    break;
                    case AJS_TYPE_SYMBOL:    ts = "symbol";    break;
                    case AJS_TYPE_BIGINT:    ts = "bigint";    break;
                    case AJS_TYPE_OBJECT:    ts = "object";    break;
                    case AJS_TYPE_FUNCTION:  ts = "function";  break;
                }
                PUSH(ajs_val_from_gc(ajs_str_from_cstr(vm, ts, true)));
                break;
            }

            case OP_EQ:
            case OP_NEQ: {
                AjsValue rv = POP();
                AjsValue lv = POP();
                bool eq = false;
                if (!ajs_abstract_eq(vm, lv, rv, &eq)) goto handle_exception;
                if (op == OP_NEQ) eq = !eq;
                PUSH(eq ? AJS_VAL_TRUE : AJS_VAL_FALSE);
                break;
            }
            case OP_STRICT_EQ: {
                AjsValue rv = POP();
                AjsValue lv = POP();
                PUSH(ajs_strict_eq(lv, rv) ? AJS_VAL_TRUE : AJS_VAL_FALSE);
                break;
            }
            case OP_STRICT_NEQ: {
                AjsValue rv = POP();
                AjsValue lv = POP();
                PUSH(ajs_strict_eq(lv, rv) ? AJS_VAL_FALSE : AJS_VAL_TRUE);
                break;
            }
            case OP_LT:
            case OP_LTE:
            case OP_GT:
            case OP_GTE: {
                AjsValue rv = POP();
                AjsValue lv = POP();
                int r = 0;
                bool res = false;
                if (op == OP_LT) {
                    r = ajs_abstract_rel_cmp(vm, lv, rv, true);
                    CHECK_EXC();
                    res = (r == 1);
                } else if (op == OP_GT) {
                    r = ajs_abstract_rel_cmp(vm, rv, lv, false);
                    CHECK_EXC();
                    res = (r == 1);
                } else if (op == OP_LTE) {
                    r = ajs_abstract_rel_cmp(vm, rv, lv, false);
                    CHECK_EXC();
                    res = (r == 0);
                } else {
                    r = ajs_abstract_rel_cmp(vm, lv, rv, true);
                    CHECK_EXC();
                    res = (r == 0);
                }
                PUSH(res ? AJS_VAL_TRUE : AJS_VAL_FALSE);
                break;
            }
            case OP_IN: {
                AjsValue rv = POP();
                AjsValue lv = POP();
                if (!ajs_is_object(vm, rv)) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                       "Cannot use 'in' operator to search for property in non-object");
                    goto handle_exception;
                }
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, rv);
                AjsValue pk = ajs_to_property_key(vm, lv);
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                bool has = false;
                bool ok = ajs_obj_has_prop(vm, (AjsObject *)ajs_val_to_gc(rv), pk, &has);
                ajs_temp_root_restore(vm, rs);
                if (!ok || vm->has_exception) goto handle_exception;
                PUSH(has ? AJS_VAL_TRUE : AJS_VAL_FALSE);
                break;
            }
            case OP_INSTANCEOF: {
                AjsValue target = POP();
                AjsValue inst = POP();
                if (!ajs_is_object(vm, target)) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Right-hand side of 'instanceof' is not an object");
                    goto handle_exception;
                }
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, target);
                ajs_temp_root_push(vm, inst);
                AjsValue hi_fn = ajs_obj_get(vm, target, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_HAS_INSTANCE]), target);
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                if (!ajs_is_nullish(hi_fn)) {
                    ajs_temp_root_push(vm, hi_fn);
                    AjsValue res = ajs_call_internal(vm, hi_fn, target, 1, &inst);
                    ajs_temp_root_restore(vm, rs);
                    CHECK_EXC();
                    PUSH(ajs_to_bool(vm, res) ? AJS_VAL_TRUE : AJS_VAL_FALSE);
                    break;
                }
                if (!ajs_is_callable(vm, target)) {
                    ajs_temp_root_restore(vm, rs);
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Right-hand side of 'instanceof' is not callable");
                    goto handle_exception;
                }
                /* OrdinaryHasInstance (unwrapping bound functions!) */
                while (ajs_val_is_gc_kind(target, AJS_GC_OBJECT)) {
                    AjsObject *to = (AjsObject *)ajs_val_to_gc(target);
                    if (to->kind == AJS_OBJ_FUNCTION && to->u.fn.fn_kind == AJS_FN_BOUND) {
                        target = to->u.fn.bound_target;
                    } else {
                        break;
                    }
                }
                if (!ajs_is_object(vm, inst)) {
                    ajs_temp_root_restore(vm, rs);
                    PUSH(AJS_VAL_FALSE);
                    break;
                }
                AjsValue tproto = ajs_obj_get(vm, target, ajs_val_from_gc(vm->str_prototype), target);
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                if (!ajs_is_object(vm, tproto)) {
                    ajs_temp_root_restore(vm, rs);
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                       "Function has non-object prototype in instanceof check");
                    goto handle_exception;
                }
                AjsObject *cur = (AjsObject *)ajs_val_to_gc(inst);
                bool match = false;
                for (int d = 0; cur && d < 256; d++) {
                    AjsValue pv = AJS_VAL_NULL;
                    if (!ajs_obj_get_proto(vm, cur, &pv)) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                    if (ajs_is_null(pv)) break;
                    if (pv == tproto) { match = true; break; }
                    cur = ajs_val_is_gc_kind(pv, AJS_GC_OBJECT) ? (AjsObject *)ajs_val_to_gc(pv) : NULL;
                }
                ajs_temp_root_restore(vm, rs);
                PUSH(match ? AJS_VAL_TRUE : AJS_VAL_FALSE);
                break;
            }
            case OP_TO_PROP_KEY: {
                AjsValue v = POP();
                AjsValue pk = ajs_to_property_key(vm, v);
                CHECK_EXC();
                PUSH(pk);
                break;
            }
            case OP_REQ_OBJ_COERCIBLE: {
                if (ajs_is_nullish(TOP())) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot destructure or access property of null or undefined");
                    goto handle_exception;
                }
                break;
            }

            case OP_JMP: {
                frame->pc = bc_u32(code, &frame->pc);
                break;
            }
            case OP_JMP_FALSE: {
                uint32_t tgt = bc_u32(code, &frame->pc);
                AjsValue v = POP();
                if (!ajs_to_bool(vm, v)) frame->pc = tgt;
                break;
            }
            case OP_JMP_TRUE: {
                uint32_t tgt = bc_u32(code, &frame->pc);
                AjsValue v = POP();
                if (ajs_to_bool(vm, v)) frame->pc = tgt;
                break;
            }
            case OP_JMP_NULLISH: {
                uint32_t tgt = bc_u32(code, &frame->pc);
                AjsValue v = POP();
                if (ajs_is_nullish(v)) frame->pc = tgt;
                break;
            }
            case OP_JMP_NOT_NULLISH: {
                uint32_t tgt = bc_u32(code, &frame->pc);
                AjsValue v = POP();
                if (!ajs_is_nullish(v)) frame->pc = tgt;
                break;
            }
            case OP_JMP_UNDEF: {
                uint32_t tgt = bc_u32(code, &frame->pc);
                AjsValue v = POP();
                if (ajs_is_undefined(v)) frame->pc = tgt;
                break;
            }

            case OP_CALL: {
                uint16_t argc = bc_u16(code, &frame->pc);
                uint32_t base = frame->sp - (uint32_t)argc - 1u;
                AjsValue fn_v = frame->stack[base];
                AjsValue stack_args[16];
                AjsValue *av = (argc <= 16) ? stack_args : (AjsValue *)malloc((size_t)argc * sizeof(AjsValue));
                for (uint16_t i = 0; i < argc; i++) av[i] = frame->stack[base + 1u + i];
                frame->sp = base;
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, fn_v);
                for (uint16_t i = 0; i < argc; i++) ajs_temp_root_push(vm, av[i]);
                AjsValue res = ajs_call_internal(vm, fn_v, AJS_VAL_UNDEFINED, (int)argc, av);
                if (av != stack_args) free(av);
                ajs_temp_root_restore(vm, rs);
                CHECK_EXC();
                PUSH(res);
                break;
            }
            case OP_CALL_METHOD: {
                uint16_t argc = bc_u16(code, &frame->pc);
                uint32_t base = frame->sp - (uint32_t)argc - 2u;
                AjsValue recv = frame->stack[base];
                AjsValue fn_v = frame->stack[base + 1u];
                AjsValue stack_args[16];
                AjsValue *av = (argc <= 16) ? stack_args : (AjsValue *)malloc((size_t)argc * sizeof(AjsValue));
                for (uint16_t i = 0; i < argc; i++) av[i] = frame->stack[base + 2u + i];
                frame->sp = base;
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, recv);
                ajs_temp_root_push(vm, fn_v);
                for (uint16_t i = 0; i < argc; i++) ajs_temp_root_push(vm, av[i]);
                AjsValue res = ajs_call_internal(vm, fn_v, recv, (int)argc, av);
                if (av != stack_args) free(av);
                ajs_temp_root_restore(vm, rs);
                CHECK_EXC();
                PUSH(res);
                break;
            }
            case OP_CALL_SPREAD: {
                AjsValue arr_v = POP();
                AjsValue fn_v = POP();
                AjsValue recv = POP();
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, recv);
                ajs_temp_root_push(vm, fn_v);
                ajs_temp_root_push(vm, arr_v);
                AjsValue *av = NULL;
                int argc = 0;
                if (!extract_array_args(vm, arr_v, &av, &argc)) {
                    ajs_temp_root_restore(vm, rs);
                    goto handle_exception;
                }
                for (int i = 0; i < argc; i++) ajs_temp_root_push(vm, av[i]);
                AjsValue res = ajs_call_internal(vm, fn_v, recv, argc, av);
                free(av);
                ajs_temp_root_restore(vm, rs);
                CHECK_EXC();
                PUSH(res);
                break;
            }
            case OP_CALL_EVAL: {
                uint16_t argc = bc_u16(code, &frame->pc);
                uint8_t st = bc_u8(code, &frame->pc);
                uint32_t base = frame->sp - (uint32_t)argc - 1u;
                AjsValue fn_v = frame->stack[base];
                AjsValue stack_args[16];
                AjsValue *av = (argc <= 16) ? stack_args : (AjsValue *)malloc((size_t)argc * sizeof(AjsValue));
                for (uint16_t i = 0; i < argc; i++) av[i] = frame->stack[base + 1u + i];
                frame->sp = base;

                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, fn_v);
                for (uint16_t i = 0; i < argc; i++) ajs_temp_root_push(vm, av[i]);

                AjsValue res = AJS_VAL_UNDEFINED;
                if (fn_v == realm->intrinsic_eval) {
                    /* Direct eval! */
                    if (argc == 0) {
                        res = AJS_VAL_UNDEFINED;
                    } else if (!ajs_val_is_gc_kind(av[0], AJS_GC_STRING)) {
                        res = av[0];
                    } else {
                        AjsString *src_str = (AjsString *)ajs_val_to_gc(av[0]);
                        AjsErrorInfo eerr;
                        AjsBytecode *ebc = ajs_compile_script(vm, realm, src_str->utf8, src_str->len8,
                                                              "<eval>", st != 0, true, frame->env, &eerr);
                        if (!ebc) {
                            if (av != stack_args) free(av);
                            ajs_temp_root_restore(vm, rs);
                            ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "%s", eerr.message);
                            goto handle_exception;
                        }
                        ajs_temp_root_push(vm, ajs_val_from_gc(ebc));
                        AjsValue ethis = frame->this_val;
                        if (bc->this_slot != AJS_NO_SLOT && frame->base_env && bc->this_slot < frame->base_env->count) {
                            ethis = frame->base_env->slots[bc->this_slot];
                        }
                        res = ajs_exec_script_bc(vm, realm, ebc, frame->env, ethis);
                    }
                } else {
                    res = ajs_call_internal(vm, fn_v, AJS_VAL_UNDEFINED, (int)argc, av);
                }
                if (av != stack_args) free(av);
                ajs_temp_root_restore(vm, rs);
                CHECK_EXC();
                PUSH(res);
                break;
            }
            case OP_NEW: {
                uint16_t argc = bc_u16(code, &frame->pc);
                uint32_t base = frame->sp - (uint32_t)argc - 1u;
                AjsValue ctor_v = frame->stack[base];
                AjsValue stack_args[16];
                AjsValue *av = (argc <= 16) ? stack_args : (AjsValue *)malloc((size_t)argc * sizeof(AjsValue));
                for (uint16_t i = 0; i < argc; i++) av[i] = frame->stack[base + 1u + i];
                frame->sp = base;
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, ctor_v);
                for (uint16_t i = 0; i < argc; i++) ajs_temp_root_push(vm, av[i]);
                AjsValue res = ajs_construct_internal(vm, ctor_v, (int)argc, av, ctor_v);
                if (av != stack_args) free(av);
                ajs_temp_root_restore(vm, rs);
                CHECK_EXC();
                PUSH(res);
                break;
            }
            case OP_NEW_SPREAD: {
                AjsValue arr_v = POP();
                AjsValue ctor_v = POP();
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, ctor_v);
                ajs_temp_root_push(vm, arr_v);
                AjsValue *av = NULL;
                int argc = 0;
                if (!extract_array_args(vm, arr_v, &av, &argc)) {
                    ajs_temp_root_restore(vm, rs);
                    goto handle_exception;
                }
                for (int i = 0; i < argc; i++) ajs_temp_root_push(vm, av[i]);
                AjsValue res = ajs_construct_internal(vm, ctor_v, argc, av, ctor_v);
                free(av);
                ajs_temp_root_restore(vm, rs);
                CHECK_EXC();
                PUSH(res);
                break;
            }
            case OP_DYNAMIC_IMPORT: {
                AjsValue spec_v = POP();
                const char *ref_url = NULL;
                if (frame->module && frame->module->url) ref_url = frame->module->url;
                else if (frame->func_obj && frame->func_obj->u.fn.owner_module)
                    ref_url = frame->func_obj->u.fn.owner_module->url;
                else if (bc->source_url) ref_url = bc->source_url->utf8;
                AjsValue prom = ajs_module_dynamic_import(vm, realm, ref_url, spec_v);
                CHECK_EXC();
                PUSH(prom);
                break;
            }

            case OP_RETURN: {
                *out_val = POP();
                return AJS_RUN_RETURNED;
            }
            case OP_RETURN_DERIVED: {
                uint32_t this_slot = bc_u32(code, &frame->pc);
                AjsValue rv = POP();
                if (ajs_is_object(vm, rv)) {
                    *out_val = rv;
                    return AJS_RUN_RETURNED;
                }
                if (!ajs_is_undefined(rv)) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                       "Derived constructors may only return object or undefined");
                    goto handle_exception;
                }
                AjsValue dthis = (this_slot != AJS_NO_SLOT && frame->base_env && this_slot < frame->base_env->count)
                                 ? frame->base_env->slots[this_slot] : frame->this_val;
                if (dthis == AJS_VAL_TDZ) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE,
                                       "Must call super constructor in derived class before returning from derived constructor");
                    goto handle_exception;
                }
                *out_val = dthis;
                return AJS_RUN_RETURNED;
            }
            case OP_THROW: {
                AjsValue ev = POP();
                ajs_throw(vm, ev);
                goto handle_exception;
            }

            case OP_TRY_PUSH: {
                uint32_t c_pc = bc_u32(code, &frame->pc);
                uint32_t f_pc = bc_u32(code, &frame->pc);
                if (frame->try_sp >= AJS_MAX_TRY_DEPTH) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Nested try/finally depth exceeded");
                    goto handle_exception;
                }
                AjsTryState *ts = &frame->try_stack[frame->try_sp++];
                memset(ts, 0, sizeof(*ts));
                ts->catch_pc = c_pc;
                ts->finally_pc = f_pc;
                ts->saved_sp = frame->sp;
                ts->saved_env_depth = frame->env_depth;
                ts->pending_val = AJS_VAL_UNDEFINED;
                break;
            }
            case OP_TRY_POP: {
                if (frame->try_sp > 0) frame->try_sp--;
                break;
            }
            case OP_ENTER_FINALLY: {
                if (frame->try_sp > 0) {
                    AjsTryState *ts = &frame->try_stack[frame->try_sp - 1u];
                    if (!ts->in_finally) {
                        ts->in_finally = true;
                        ts->pending_kind = AJS_COMP_NORMAL;
                        ts->pending_val = AJS_VAL_UNDEFINED;
                        ts->catch_pc = UINT32_MAX;
                        ts->finally_pc = UINT32_MAX;
                    }
                }
                break;
            }
            case OP_UNWIND_JUMP: {
                uint16_t tgt_try = bc_u16(code, &frame->pc);
                uint16_t tgt_env = bc_u16(code, &frame->pc);
                uint32_t tgt_pc = bc_u32(code, &frame->pc);
                while (frame->try_sp > tgt_try) {
                    AjsTryState *ts = &frame->try_stack[frame->try_sp - 1u];
                    if (!ts->in_finally && ts->finally_pc != UINT32_MAX) {
                        frame->sp = ts->saved_sp;
                        unwind_frame_env(frame, ts->saved_env_depth);
                        ts->in_finally = true;
                        ts->catch_pc = UINT32_MAX;
                        uint32_t fpc = ts->finally_pc;
                        ts->finally_pc = UINT32_MAX;
                        ts->pending_kind = AJS_COMP_JUMP;
                        ts->pending_try_sp = tgt_try;
                        ts->pending_env_depth = tgt_env;
                        ts->pending_pc = tgt_pc;
                        frame->pc = fpc;
                        goto next_opcode;
                    }
                    frame->try_sp--;
                }
                unwind_frame_env(frame, tgt_env);
                frame->pc = tgt_pc;
                break;
            }
            case OP_UNWIND_RETURN: {
                ret_candidate = POP();
                do_unwind_return_label:;
                while (frame->try_sp > 0) {
                    AjsTryState *ts = &frame->try_stack[frame->try_sp - 1u];
                    if (!ts->in_finally && ts->finally_pc != UINT32_MAX) {
                        frame->sp = ts->saved_sp;
                        unwind_frame_env(frame, ts->saved_env_depth);
                        ts->in_finally = true;
                        ts->catch_pc = UINT32_MAX;
                        uint32_t fpc = ts->finally_pc;
                        ts->finally_pc = UINT32_MAX;
                        ts->pending_kind = AJS_COMP_RETURN;
                        ts->pending_val = ret_candidate;
                        frame->pc = fpc;
                        goto next_opcode;
                    }
                    frame->try_sp--;
                }
                if (bc->is_derived_ctor) {
                    if (ajs_is_object(vm, ret_candidate)) {
                        *out_val = ret_candidate;
                        return AJS_RUN_RETURNED;
                    }
                    if (!ajs_is_undefined(ret_candidate)) {
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                           "Derived constructors may only return object or undefined");
                        goto handle_exception;
                    }
                    AjsValue dthis = (bc->this_slot != AJS_NO_SLOT && frame->base_env && bc->this_slot < frame->base_env->count)
                                     ? frame->base_env->slots[bc->this_slot] : frame->this_val;
                    if (dthis == AJS_VAL_TDZ) {
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE,
                                           "Must call super constructor in derived class before returning from derived constructor");
                        goto handle_exception;
                    }
                    *out_val = dthis;
                    return AJS_RUN_RETURNED;
                }
                *out_val = ret_candidate;
                return AJS_RUN_RETURNED;
            }
            case OP_FINALLY_END: {
                if (frame->try_sp == 0) break;
                AjsTryState ts = frame->try_stack[--frame->try_sp];
                if (ts.pending_kind == AJS_COMP_NORMAL) {
                    break;
                }
                if (ts.pending_kind == AJS_COMP_THROW) {
                    ajs_throw(vm, ts.pending_val);
                    goto handle_exception;
                }
                if (ts.pending_kind == AJS_COMP_JUMP) {
                    uint16_t tgt_try = ts.pending_try_sp;
                    uint16_t tgt_env = ts.pending_env_depth;
                    uint32_t tgt_pc = ts.pending_pc;
                    while (frame->try_sp > tgt_try) {
                        AjsTryState *ots = &frame->try_stack[frame->try_sp - 1u];
                        if (!ots->in_finally && ots->finally_pc != UINT32_MAX) {
                            frame->sp = ots->saved_sp;
                            unwind_frame_env(frame, ots->saved_env_depth);
                            ots->in_finally = true;
                            ots->catch_pc = UINT32_MAX;
                            uint32_t fpc = ots->finally_pc;
                            ots->finally_pc = UINT32_MAX;
                            ots->pending_kind = AJS_COMP_JUMP;
                            ots->pending_try_sp = tgt_try;
                            ots->pending_env_depth = tgt_env;
                            ots->pending_pc = tgt_pc;
                            frame->pc = fpc;
                            goto next_opcode;
                        }
                        frame->try_sp--;
                    }
                    unwind_frame_env(frame, tgt_env);
                    frame->pc = tgt_pc;
                    break;
                }
                if (ts.pending_kind == AJS_COMP_RETURN) {
                    ret_candidate = ts.pending_val;
                    goto do_unwind_return_label;
                }
                break;
            }

            case OP_GET_ITER: {
                uint8_t is_async = bc_u8(code, &frame->pc);
                AjsValue iterable = POP();
                AjsValue it = AJS_VAL_UNDEFINED, nfn = AJS_VAL_UNDEFINED;
                if (!get_iterator_internal(vm, realm, iterable, is_async != 0, &it, &nfn)) {
                    goto handle_exception;
                }
                PUSH(it);
                PUSH(nfn);
                break;
            }
            case OP_ITER_NEXT: {
                AjsValue nfn = frame->stack[frame->sp - 1u];
                AjsValue it = frame->stack[frame->sp - 2u];
                AjsValue res = ajs_call_internal(vm, nfn, it, 0, NULL);
                CHECK_EXC();
                PUSH(res);
                break;
            }
            case OP_ITER_UNPACK: {
                AjsValue r_obj = POP();
                if (!ajs_is_object(vm, r_obj)) {
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator result is not an object");
                    goto handle_exception;
                }
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, r_obj);
                AjsValue dv = ajs_obj_get(vm, r_obj, ajs_val_from_gc(vm->str_done), r_obj);
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                bool done = ajs_to_bool(vm, dv);
                AjsValue vv = done ? AJS_VAL_UNDEFINED : ajs_obj_get(vm, r_obj, ajs_val_from_gc(vm->str_value), r_obj);
                ajs_temp_root_restore(vm, rs);
                CHECK_EXC();
                PUSH(vv);
                PUSH(done ? AJS_VAL_TRUE : AJS_VAL_FALSE);
                break;
            }
            case OP_ITER_CLOSE: {
                AjsValue it = POP();
                bool suppress = (frame->try_sp > 0 &&
                                 frame->try_stack[frame->try_sp - 1u].in_finally &&
                                 frame->try_stack[frame->try_sp - 1u].pending_kind == AJS_COMP_THROW);
                if (!iterator_close_sync(vm, it, suppress)) goto handle_exception;
                break;
            }
            case OP_FOR_IN_INIT: {
                AjsValue tgt = POP();
                AjsValue it = AJS_VAL_UNDEFINED, nfn = AJS_VAL_UNDEFINED;
                if (!build_for_in_iterator(vm, realm, tgt, &it, &nfn)) goto handle_exception;
                PUSH(it);
                PUSH(nfn);
                break;
            }
            case OP_REST_FROM_ITER: {
                AjsValue nfn = POP();
                AjsValue it = POP();
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, it);
                ajs_temp_root_push(vm, nfn);
                AjsObject *arr = ajs_obj_new(vm, realm, AJS_OBJ_ARRAY, realm->array_proto);
                ajs_temp_root_push(vm, ajs_val_from_gc(arr));
                arr->u.arr.length_writable = true;
                for (;;) {
                    AjsValue sres = ajs_call_internal(vm, nfn, it, 0, NULL);
                    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                    if (!ajs_is_object(vm, sres)) {
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator result is not an object");
                        ajs_temp_root_restore(vm, rs);
                        goto handle_exception;
                    }
                    size_t isave = ajs_temp_root_save(vm);
                    ajs_temp_root_push(vm, sres);
                    AjsValue dv = ajs_obj_get(vm, sres, ajs_val_from_gc(vm->str_done), sres);
                    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                    if (ajs_to_bool(vm, dv)) {
                        ajs_temp_root_restore(vm, isave);
                        break;
                    }
                    AjsValue vv = ajs_obj_get(vm, sres, ajs_val_from_gc(vm->str_value), sres);
                    ajs_temp_root_restore(vm, isave);
                    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); goto handle_exception; }
                    if (arr->u.arr.dense_len >= arr->u.arr.dense_cap) {
                        uint32_t ncap = arr->u.arr.dense_cap ? arr->u.arr.dense_cap * 2u : 8u;
                        arr->u.arr.dense = (AjsValue *)realloc(arr->u.arr.dense, ncap * sizeof(AjsValue));
                        arr->u.arr.dense_cap = ncap;
                    }
                    arr->u.arr.dense[arr->u.arr.dense_len++] = vv;
                    arr->u.arr.length = arr->u.arr.dense_len;
                }
                ajs_temp_root_restore(vm, rs);
                PUSH(ajs_val_from_gc(arr));
                break;
            }

            case OP_YIELD: {
                AjsValue yv = POP();
                /* ECMA-262 AsyncGeneratorYield: await `yv` inside body before yielding!
                 * Note: the synthetic start yield is right after params (before any body ops) and `yv == undefined`. */
                if (bc->fn_kind == AJS_FN_BC_ASYNC_GENERATOR && !ajs_is_undefined(yv) && !frame->ys_awaiting_step) {
                    frame->ys_awaiting_step = true;
                    frame->pc--; /* Re-enter OP_YIELD once `yv` is unwrapped */
                    *out_val = yv;
                    return AJS_RUN_AWAITED;
                }
                frame->ys_awaiting_step = false;
                *out_val = yv;
                return AJS_RUN_SUSPENDED;
            }
            case OP_YIELD_STAR: {
                uint8_t is_async = bc_u8(code, &frame->pc);
                AjsValue iterable = POP();
                AjsValue it = AJS_VAL_UNDEFINED, nfn = AJS_VAL_UNDEFINED;
                if (!get_iterator_internal(vm, realm, iterable, is_async != 0, &it, &nfn)) {
                    goto handle_exception;
                }
                frame->in_yield_star = true;
                frame->ys_iter = it;
                frame->ys_next = nfn;
                frame->ys_step_mode = 0;
                frame->ys_awaiting_step = false;
                PUSH(AJS_VAL_UNDEFINED);

                resume_yield_star:;
                {
                    bool is_ag = (bc->fn_kind == AJS_FN_BC_ASYNC_GENERATOR);
                    AjsValue step_res = AJS_VAL_UNDEFINED;
                    if (!frame->ys_awaiting_step) {
                        AjsValue sent = POP();
                        int mode = frame->ys_step_mode;
                        size_t rs = ajs_temp_root_save(vm);
                        ajs_temp_root_push(vm, sent);

                        if (mode == 0) {
                            step_res = ajs_call_internal(vm, frame->ys_next, frame->ys_iter, 1, &sent);
                        } else if (mode == 2) {
                            AjsValue thr_m = ajs_obj_get(vm, frame->ys_iter, ajs_val_from_gc(vm->str_throw), frame->ys_iter);
                            if (vm->has_exception) {
                                frame->in_yield_star = false;
                                ajs_temp_root_restore(vm, rs);
                                goto handle_exception;
                            }
                            if (ajs_is_nullish(thr_m)) {
                                AjsValue cit = frame->ys_iter;
                                frame->in_yield_star = false;
                                frame->ys_iter = AJS_VAL_UNDEFINED;
                                frame->ys_next = AJS_VAL_UNDEFINED;
                                iterator_close_sync(vm, cit, false);
                                ajs_temp_root_restore(vm, rs);
                                if (!vm->has_exception) {
                                    ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                                       "Delegated iterator does not provide a 'throw' method");
                                }
                                goto handle_exception;
                            }
                            ajs_temp_root_push(vm, thr_m);
                            step_res = ajs_call_internal(vm, thr_m, frame->ys_iter, 1, &sent);
                        } else {
                            /* mode == 1: `return(sent)` */
                            AjsValue ret_m = ajs_obj_get(vm, frame->ys_iter, ajs_val_from_gc(vm->str_return), frame->ys_iter);
                            if (vm->has_exception) {
                                frame->in_yield_star = false;
                                ajs_temp_root_restore(vm, rs);
                                goto handle_exception;
                            }
                            if (ajs_is_nullish(ret_m)) {
                                frame->in_yield_star = false;
                                frame->ys_iter = AJS_VAL_UNDEFINED;
                                frame->ys_next = AJS_VAL_UNDEFINED;
                                ajs_temp_root_restore(vm, rs);
                                if (is_ag) {
                                    PUSH(sent);
                                    frame->ys_awaiting_step = true;
                                    frame->ys_step_mode = 99;
                                    (void)POP();
                                    *out_val = sent;
                                    return AJS_RUN_AWAITED;
                                }
                                ret_candidate = sent;
                                goto do_unwind_return_label;
                            }
                            ajs_temp_root_push(vm, ret_m);
                            step_res = ajs_call_internal(vm, ret_m, frame->ys_iter, 1, &sent);
                        }
                        ajs_temp_root_restore(vm, rs);
                        if (vm->has_exception) {
                            frame->in_yield_star = false;
                            frame->ys_iter = AJS_VAL_UNDEFINED;
                            frame->ys_next = AJS_VAL_UNDEFINED;
                            goto handle_exception;
                        }
                        if (is_ag) {
                            frame->ys_awaiting_step = true;
                            *out_val = step_res;
                            return AJS_RUN_AWAITED;
                        }
                    } else {
                        frame->ys_awaiting_step = false;
                        step_res = POP();
                    }

                    if (!ajs_is_object(vm, step_res)) {
                        frame->in_yield_star = false;
                        frame->ys_iter = AJS_VAL_UNDEFINED;
                        frame->ys_next = AJS_VAL_UNDEFINED;
                        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Delegated iterator result is not an object");
                        goto handle_exception;
                    }
                    size_t rs2 = ajs_temp_root_save(vm);
                    ajs_temp_root_push(vm, step_res);
                    AjsValue dv = ajs_obj_get(vm, step_res, ajs_val_from_gc(vm->str_done), step_res);
                    if (vm->has_exception) {
                        frame->in_yield_star = false;
                        ajs_temp_root_restore(vm, rs2);
                        goto handle_exception;
                    }
                    bool done = ajs_to_bool(vm, dv);
                    AjsValue vv = ajs_obj_get(vm, step_res, ajs_val_from_gc(vm->str_value), step_res);
                    ajs_temp_root_restore(vm, rs2);
                    if (vm->has_exception) {
                        frame->in_yield_star = false;
                        goto handle_exception;
                    }
                    if (done) {
                        int fin_mode = frame->ys_step_mode;
                        frame->in_yield_star = false;
                        frame->ys_iter = AJS_VAL_UNDEFINED;
                        frame->ys_next = AJS_VAL_UNDEFINED;
                        frame->ys_step_mode = 0;
                        if (fin_mode == 1) {
                            ret_candidate = vv;
                            goto do_unwind_return_label;
                        }
                        PUSH(vv);
                        break;
                    }
                    *out_val = vv;
                    return AJS_RUN_SUSPENDED;
                }
            }
            case OP_AWAIT: {
                AjsValue av = POP();
                *out_val = av;
                return AJS_RUN_AWAITED;
            }
            case OP_DISPOSE_NEW: {
                AjsValue sv = ajs_erm_disp_stack_new_internal(vm, realm, false);
                CHECK_EXC();
                PUSH(sv);
                break;
            }
            case OP_DISPOSE_ADD: {
                uint8_t is_async = bc_u8(code, &frame->pc);
                AjsValue val = POP();
                AjsValue sv = POP();
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, sv);
                ajs_temp_root_push(vm, val);
                bool ok = ajs_val_is_gc_kind(sv, AJS_GC_OBJECT) &&
                          ajs_erm_add_disposable(vm, (AjsObject *)ajs_val_to_gc(sv), val, is_async != 0);
                ajs_temp_root_restore(vm, rs);
                if (!ok || vm->has_exception) goto handle_exception;
                PUSH(val);
                break;
            }
            case OP_DISPOSE_STEP: {
                AjsValue sv = POP();
                if (!ajs_val_is_gc_kind(sv, AJS_GC_OBJECT)) {
                    PUSH(AJS_VAL_UNDEFINED);
                    PUSH(ajs_val_number(0.0));
                    break;
                }
                AjsObject *sobj = (AjsObject *)ajs_val_to_gc(sv);
                /* Seed pending throw completion into disposal state on first step so
                 * SuppressedError(disposeErr, prevErr) associates left-to-right per ES2026! */
                for (uint32_t ti = frame->try_sp; ti > 0; ti--) {
                    AjsTryState *ts = &frame->try_stack[ti - 1u];
                    if (ts->in_finally) {
                        if (ts->pending_kind == AJS_COMP_THROW) {
                            ajs_erm_dispose_record_error(vm, sobj, ts->pending_val);
                            ts->pending_kind = AJS_COMP_NORMAL;
                            ts->pending_val = AJS_VAL_UNDEFINED;
                        }
                        break;
                    }
                }
                AjsValue await_val = AJS_VAL_UNDEFINED;
                uint8_t mode = 0;
                size_t rs = ajs_temp_root_save(vm);
                ajs_temp_root_push(vm, sv);
                bool ok = ajs_erm_dispose_step(vm, sobj, &await_val, &mode);
                ajs_temp_root_restore(vm, rs);
                if (!ok || vm->has_exception) goto handle_exception;
                PUSH(await_val);
                PUSH(ajs_val_number((double)mode));
                break;
            }
            case OP_DISPOSE_CATCH: {
                AjsValue err = POP();
                AjsValue sv = POP();
                if (ajs_val_is_gc_kind(sv, AJS_GC_OBJECT)) {
                    ajs_erm_dispose_record_error(vm, (AjsObject *)ajs_val_to_gc(sv), err);
                }
                break;
            }
            case OP_DISPOSE_FINISH: {
                AjsValue sv = POP();
                if (ajs_val_is_gc_kind(sv, AJS_GC_OBJECT)) {
                    AjsObject *sobj = (AjsObject *)ajs_val_to_gc(sv);
                    bool has_pending_throw = false;
                    AjsValue pending_err = AJS_VAL_UNDEFINED;
                    AjsTryState *ts = (frame->try_sp > 0 && frame->try_stack[frame->try_sp - 1u].in_finally)
                                      ? &frame->try_stack[frame->try_sp - 1u] : NULL;
                    if (ts && ts->pending_kind == AJS_COMP_THROW) {
                        has_pending_throw = true;
                        pending_err = ts->pending_val;
                    }
                    bool threw = !ajs_erm_dispose_finish(vm, sobj, has_pending_throw, &pending_err);
                    if (threw) {
                        if (ts) {
                            ts->pending_kind = AJS_COMP_THROW;
                            ts->pending_val = pending_err;
                            vm->has_exception = false;
                            vm->exception = AJS_VAL_UNDEFINED;
                        } else {
                            ajs_throw(vm, pending_err);
                            goto handle_exception;
                        }
                    }
                }
                break;
            }
        }
        next_opcode:;
        continue;

        handle_exception:;
        {
            AjsValue exc = vm->exception;
            while (frame->try_sp > 0) {
                AjsTryState *ts = &frame->try_stack[frame->try_sp - 1u];
                if (ts->in_finally) {
                    /* New exception inside `finally` overrides pending completion! */
                    frame->try_sp--;
                    continue;
                }
                frame->sp = ts->saved_sp;
                unwind_frame_env(frame, ts->saved_env_depth);
                if (ts->catch_pc != UINT32_MAX) {
                    uint32_t cpc = ts->catch_pc;
                    if (ts->finally_pc == UINT32_MAX) {
                        frame->try_sp--;
                    } else {
                        ts->catch_pc = UINT32_MAX;
                    }
                    vm->has_exception = false;
                    vm->exception = AJS_VAL_UNDEFINED;
                    PUSH(exc);
                    frame->pc = cpc;
                    goto next_opcode;
                }
                if (ts->finally_pc != UINT32_MAX) {
                    uint32_t fpc = ts->finally_pc;
                    ts->in_finally = true;
                    ts->catch_pc = UINT32_MAX;
                    ts->finally_pc = UINT32_MAX;
                    ts->pending_kind = AJS_COMP_THROW;
                    ts->pending_val = exc;
                    vm->has_exception = false;
                    vm->exception = AJS_VAL_UNDEFINED;
                    frame->pc = fpc;
                    goto next_opcode;
                }
                frame->try_sp--;
            }
            return AJS_RUN_THREW;
        }
    }

    #undef PUSH
    #undef POP
    #undef TOP
    #undef CHECK_EXC
}
