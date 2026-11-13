/*
 * ArcradientJS 1.0 - js/vm/ajs_gc.c
 * Tracing Mark-Sweep Garbage Collector with AjsHandleScope, Ephemeron WeakMap, WeakRef,
 * FinalizationRegistry, Host Opaque Cycle Hooks, and Heap Limit Enforcement
 */
#include "ajs_internal.h"

void ajs_temp_root_push(AjsVm *vm, AjsValue v) {
    if (!ajs_val_is_ptr(v)) return;
    if (vm->temp_top >= vm->temp_cap) {
        size_t ncap = vm->temp_cap ? vm->temp_cap * 2u : 128u;
        AjsValue *nbuf = (AjsValue *)realloc(vm->temp_roots, ncap * sizeof(AjsValue));
        if (!nbuf) return;
        vm->temp_roots = nbuf;
        vm->temp_cap = ncap;
    }
    vm->temp_roots[vm->temp_top++] = v;
}

void ajs_handle_record(AjsVm *vm, AjsValue v) {
    if (!vm->current_handle_scope || !ajs_val_is_ptr(v)) return;
    if (vm->handle_top >= vm->handle_cap) {
        size_t ncap = vm->handle_cap ? vm->handle_cap * 2u : 256u;
        AjsValue *nbuf = (AjsValue *)realloc(vm->handle_stack, ncap * sizeof(AjsValue));
        if (!nbuf) return;
        vm->handle_stack = nbuf;
        vm->handle_cap = ncap;
    }
    vm->handle_stack[vm->handle_top++] = v;
}

void *ajs_gc_alloc(AjsVm *vm, size_t size, AjsGcKind kind) {
    if (size < sizeof(AjsGcHeader)) size = sizeof(AjsGcHeader);
    size_t limit = vm->cfg.heap_limit_bytes ? vm->cfg.heap_limit_bytes : (32u * 1024u * 1024u);

    if (!vm->gc_running && vm->gc_suppress == 0) {
        if (vm->gc_bytes + size > vm->gc_threshold || vm->gc_bytes + size > limit) {
            ajs_gc_collect(vm);
        }
        if (vm->gc_bytes + size > limit) {
            if (vm->oom_error != AJS_VAL_UNDEFINED) {
                ajs_throw(vm, vm->oom_error);
            } else {
                vm->has_exception = true;
                vm->exc_phase = AJS_ERR_RUNTIME;
                vm->exception = AJS_VAL_NULL;
            }
            return NULL;
        }
    }

    /* Guarantee 8-byte alignment on both 32-bit and 64-bit platforms */
    uint8_t *raw = (uint8_t *)calloc(1, size + 7u);
    if (!raw) {
        if (vm->oom_error != AJS_VAL_UNDEFINED) ajs_throw(vm, vm->oom_error);
        return NULL;
    }
    uintptr_t u = (uintptr_t)raw;
    uintptr_t aligned = (u + 7u) & ~(uintptr_t)7u;
    uint8_t shift = (uint8_t)(aligned - u);

    AjsGcHeader *hdr = (AjsGcHeader *)aligned;
    hdr->gc_kind = (uint8_t)kind;
    hdr->gc_marked = 0;
    hdr->alloc_shift = shift;
    hdr->gc_size = size;
    hdr->gc_next = vm->gc_head;
    vm->gc_head = hdr;
    vm->gc_bytes += size;

    return hdr;
}

static void ajs_gc_mark_realm(AjsVm *vm, AjsRealm *r) {
    if (!r) return;
    ajs_gc_mark_value(vm, r->global_obj);
    ajs_gc_mark_ptr(vm, r->global_lex_env);
    ajs_gc_mark_value(vm, r->object_proto);
    ajs_gc_mark_value(vm, r->object_ctor);
    ajs_gc_mark_value(vm, r->function_proto);
    ajs_gc_mark_value(vm, r->function_ctor);
    ajs_gc_mark_value(vm, r->array_proto);
    ajs_gc_mark_value(vm, r->array_ctor);
    ajs_gc_mark_value(vm, r->array_iter_proto);
    ajs_gc_mark_value(vm, r->string_proto);
    ajs_gc_mark_value(vm, r->string_ctor);
    ajs_gc_mark_value(vm, r->string_iter_proto);
    ajs_gc_mark_value(vm, r->boolean_proto);
    ajs_gc_mark_value(vm, r->boolean_ctor);
    ajs_gc_mark_value(vm, r->number_proto);
    ajs_gc_mark_value(vm, r->number_ctor);
    ajs_gc_mark_value(vm, r->bigint_proto);
    ajs_gc_mark_value(vm, r->bigint_ctor);
    ajs_gc_mark_value(vm, r->symbol_proto);
    ajs_gc_mark_value(vm, r->symbol_ctor);
    ajs_gc_mark_value(vm, r->regexp_proto);
    ajs_gc_mark_value(vm, r->regexp_ctor);
    ajs_gc_mark_value(vm, r->regexp_iter_proto);
    ajs_gc_mark_value(vm, r->date_proto);
    ajs_gc_mark_value(vm, r->date_ctor);
    ajs_gc_mark_value(vm, r->promise_proto);
    ajs_gc_mark_value(vm, r->promise_ctor);
    ajs_gc_mark_value(vm, r->map_proto);
    ajs_gc_mark_value(vm, r->map_ctor);
    ajs_gc_mark_value(vm, r->map_iter_proto);
    ajs_gc_mark_value(vm, r->set_proto);
    ajs_gc_mark_value(vm, r->set_ctor);
    ajs_gc_mark_value(vm, r->set_iter_proto);
    ajs_gc_mark_value(vm, r->weakmap_proto);
    ajs_gc_mark_value(vm, r->weakmap_ctor);
    ajs_gc_mark_value(vm, r->weakset_proto);
    ajs_gc_mark_value(vm, r->weakset_ctor);
    ajs_gc_mark_value(vm, r->weakref_proto);
    ajs_gc_mark_value(vm, r->weakref_ctor);
    ajs_gc_mark_value(vm, r->finreg_proto);
    ajs_gc_mark_value(vm, r->finreg_ctor);
    ajs_gc_mark_value(vm, r->iterator_proto);
    ajs_gc_mark_value(vm, r->iterator_ctor);
    ajs_gc_mark_value(vm, r->iter_helper_proto);
    ajs_gc_mark_value(vm, r->async_iterator_proto);
    ajs_gc_mark_value(vm, r->gen_func_proto);
    ajs_gc_mark_value(vm, r->gen_func_ctor);
    ajs_gc_mark_value(vm, r->gen_proto);
    ajs_gc_mark_value(vm, r->async_func_proto);
    ajs_gc_mark_value(vm, r->async_func_ctor);
    ajs_gc_mark_value(vm, r->async_gen_func_proto);
    ajs_gc_mark_value(vm, r->async_gen_func_ctor);
    ajs_gc_mark_value(vm, r->async_gen_proto);
    for (int i = 0; i < AJS_ERR_KIND_COUNT; i++) {
        ajs_gc_mark_value(vm, r->error_proto[i]);
        ajs_gc_mark_value(vm, r->error_ctor[i]);
    }
    ajs_gc_mark_value(vm, r->ab_proto);
    ajs_gc_mark_value(vm, r->ab_ctor);
    ajs_gc_mark_value(vm, r->sab_proto);
    ajs_gc_mark_value(vm, r->sab_ctor);
    ajs_gc_mark_value(vm, r->dv_proto);
    ajs_gc_mark_value(vm, r->dv_ctor);
    ajs_gc_mark_value(vm, r->ta_proto);
    ajs_gc_mark_value(vm, r->ta_ctor);
    for (int i = 0; i < 12; i++) {
        ajs_gc_mark_value(vm, r->ta_sub_proto[i]);
        ajs_gc_mark_value(vm, r->ta_sub_ctor[i]);
    }
    ajs_gc_mark_value(vm, r->intl_locale_proto);
    ajs_gc_mark_value(vm, r->intl_collator_proto);
    ajs_gc_mark_value(vm, r->intl_numfmt_proto);
    ajs_gc_mark_value(vm, r->intl_dtfmt_proto);
    ajs_gc_mark_value(vm, r->intl_plural_proto);
    ajs_gc_mark_value(vm, r->intl_reltime_proto);
    ajs_gc_mark_value(vm, r->intl_segmenter_proto);
    ajs_gc_mark_value(vm, r->intl_segments_proto);
    ajs_gc_mark_value(vm, r->intl_dispnames_proto);
    ajs_gc_mark_value(vm, r->intl_listfmt_proto);
    ajs_gc_mark_value(vm, r->intrinsic_eval);
    ajs_gc_mark_value(vm, r->throw_type_error_fn);
    ajs_gc_mark_value(vm, r->array_proto_values_fn);
    for (uint32_t i = 0; i < r->tpl_cache_count; i++) {
        ajs_gc_mark_ptr(vm, (void *)(uintptr_t)r->tpl_cache[i].bc);
        ajs_gc_mark_value(vm, r->tpl_cache[i].tpl_array);
    }
}

void ajs_gc_mark_value(AjsVm *vm, AjsValue v) {
    if (!ajs_val_is_ptr(v)) return;
    ajs_gc_mark_ptr(vm, ajs_val_to_gc(v));
}

void ajs_gc_mark_ptr(AjsVm *vm, void *ptr) {
    if (!ptr) return;
    AjsGcHeader *hdr = (AjsGcHeader *)ptr;
    if (hdr->gc_marked) return;
    hdr->gc_marked = 1;

    switch ((AjsGcKind)hdr->gc_kind) {
        case AJS_GC_STRING:
        case AJS_GC_BIGINT:
            break;
        case AJS_GC_SYMBOL: {
            AjsSymbol *sym = (AjsSymbol *)hdr;
            ajs_gc_mark_ptr(vm, sym->desc);
            break;
        }
        case AJS_GC_ENV: {
            AjsEnv *env = (AjsEnv *)hdr;
            ajs_gc_mark_ptr(vm, env->parent);
            ajs_gc_mark_ptr(vm, env->with_obj);
            for (uint32_t i = 0; i < env->count; i++) {
                ajs_gc_mark_value(vm, env->slots[i]);
                if (env->names) ajs_gc_mark_ptr(vm, env->names[i]);
                if (env->import_links && env->import_links[i].target_mod) {
                    ajs_gc_mark_ptr(vm, env->import_links[i].target_mod);
                }
            }
            break;
        }
        case AJS_GC_BYTECODE: {
            AjsBytecode *bc = (AjsBytecode *)hdr;
            ajs_gc_mark_ptr(vm, bc->name);
            ajs_gc_mark_ptr(vm, bc->source_url);
            ajs_gc_mark_ptr(vm, bc->source_text);
            if (bc->env_slot_names) {
                for (uint16_t i = 0; i < bc->env_slot_count; i++) {
                    ajs_gc_mark_ptr(vm, bc->env_slot_names[i]);
                }
            }
            for (uint32_t i = 0; i < bc->const_count; i++) {
                ajs_gc_mark_value(vm, bc->consts[i]);
            }
            for (uint32_t i = 0; i < bc->tpl_count; i++) {
                for (uint32_t j = 0; j < bc->tpls[i].count; j++) {
                    ajs_gc_mark_ptr(vm, bc->tpls[i].cooked[j]);
                    ajs_gc_mark_ptr(vm, bc->tpls[i].raw[j]);
                }
            }
            for (uint32_t i = 0; i < bc->env_desc_count; i++) {
                for (uint16_t j = 0; j < bc->env_descs[i].count; j++) {
                    ajs_gc_mark_ptr(vm, bc->env_descs[i].names[j]);
                }
            }
            break;
        }
        case AJS_GC_FRAME: {
            AjsExecFrame *f = (AjsExecFrame *)hdr;
            ajs_gc_mark_ptr(vm, f->func_obj);
            ajs_gc_mark_ptr(vm, f->bc);
            ajs_gc_mark_ptr(vm, f->base_env);
            ajs_gc_mark_ptr(vm, f->env);
            ajs_gc_mark_value(vm, f->this_val);
            ajs_gc_mark_value(vm, f->new_target);
            ajs_gc_mark_ptr(vm, f->args_obj);
            ajs_gc_mark_ptr(vm, f->module);
            for (uint32_t i = 0; i < f->sp; i++) {
                ajs_gc_mark_value(vm, f->stack[i]);
            }
            for (uint16_t i = 0; i < f->try_sp; i++) {
                ajs_gc_mark_value(vm, f->try_stack[i].pending_val);
            }
            if (f->in_yield_star) {
                ajs_gc_mark_value(vm, f->ys_iter);
                ajs_gc_mark_value(vm, f->ys_next);
            }
            break;
        }
        case AJS_GC_MODULE: {
            AjsModuleRecord *m = (AjsModuleRecord *)hdr;
            ajs_gc_mark_ptr(vm, m->bc);
            ajs_gc_mark_ptr(vm, m->env);
            ajs_gc_mark_value(vm, m->namespace_obj);
            ajs_gc_mark_value(vm, m->import_meta_obj);
            for (uint32_t i = 0; i < m->req_count; i++) {
                ajs_gc_mark_ptr(vm, m->requested_specifiers[i]);
                if (m->loaded_modules) ajs_gc_mark_ptr(vm, m->loaded_modules[i]);
            }
            for (uint32_t i = 0; i < m->import_count; i++) {
                ajs_gc_mark_ptr(vm, m->imports[i].module_specifier);
                ajs_gc_mark_ptr(vm, m->imports[i].import_name);
                ajs_gc_mark_ptr(vm, m->imports[i].local_name);
            }
            for (uint32_t i = 0; i < m->export_count; i++) {
                ajs_gc_mark_ptr(vm, m->exports[i].export_name);
                ajs_gc_mark_ptr(vm, m->exports[i].module_specifier);
                ajs_gc_mark_ptr(vm, m->exports[i].import_name);
                ajs_gc_mark_ptr(vm, m->exports[i].local_name);
            }
            for (uint32_t i = 0; i < m->hoisted_fn_count; i++) {
                ajs_gc_mark_ptr(vm, m->hoisted_fns[i].fn_bc);
            }
            ajs_gc_mark_ptr(vm, m->cycle_root);
            for (uint32_t i = 0; i < m->async_parent_count; i++) {
                ajs_gc_mark_ptr(vm, m->async_parents[i]);
            }
            ajs_gc_mark_value(vm, m->top_capability_promise);
            ajs_gc_mark_value(vm, m->top_resolve_fn);
            ajs_gc_mark_value(vm, m->top_reject_fn);
            ajs_gc_mark_value(vm, m->eval_error);
            break;
        }
        case AJS_GC_OBJECT: {
            AjsObject *obj = (AjsObject *)hdr;
            ajs_gc_mark_value(vm, obj->proto);
            for (uint32_t i = 0; i < obj->prop_count; i++) {
                if (obj->props[i].key != AJS_VAL_EMPTY) {
                    ajs_gc_mark_value(vm, obj->props[i].key);
                    ajs_gc_mark_value(vm, obj->props[i].value);
                    ajs_gc_mark_value(vm, obj->props[i].setter);
                }
            }
            for (uint32_t i = 0; i < obj->priv_count; i++) {
                ajs_gc_mark_ptr(vm, obj->privs[i].sym);
                ajs_gc_mark_value(vm, obj->privs[i].value);
                ajs_gc_mark_value(vm, obj->privs[i].setter);
            }
            if (obj->gc_mark && obj->opaque_ptr) {
                obj->gc_mark(vm, obj->opaque_ptr, ajs_gc_mark_value);
            }
            switch (obj->kind) {
                case AJS_OBJ_ARRAY:
                    for (uint32_t i = 0; i < obj->u.arr.dense_len; i++) {
                        ajs_gc_mark_value(vm, obj->u.arr.dense[i]);
                    }
                    break;
                case AJS_OBJ_FUNCTION:
                    ajs_gc_mark_ptr(vm, obj->u.fn.bc);
                    ajs_gc_mark_ptr(vm, obj->u.fn.closure_env);
                    ajs_gc_mark_value(vm, obj->u.fn.home_object);
                    ajs_gc_mark_value(vm, obj->u.fn.inst_init_fn);
                    ajs_gc_mark_ptr(vm, obj->u.fn.source_text);
                    ajs_gc_mark_value(vm, obj->u.fn.bound_target);
                    ajs_gc_mark_value(vm, obj->u.fn.bound_this);
                    for (int i = 0; i < obj->u.fn.bound_argc; i++) {
                        ajs_gc_mark_value(vm, obj->u.fn.bound_argv[i]);
                    }
                    ajs_gc_mark_ptr(vm, obj->u.fn.owner_module);
                    break;
                case AJS_OBJ_ARGUMENTS:
                    ajs_gc_mark_ptr(vm, obj->u.args.env);
                    break;
                case AJS_OBJ_BOOLEAN_OBJ:
                case AJS_OBJ_NUMBER_OBJ:
                case AJS_OBJ_STRING_OBJ:
                case AJS_OBJ_SYMBOL_OBJ:
                case AJS_OBJ_BIGINT_OBJ:
                    ajs_gc_mark_value(vm, obj->u.prim_val);
                    break;
                case AJS_OBJ_PROMISE: {
                    ajs_gc_mark_value(vm, obj->u.promise.result);
                    AjsPromiseReaction *rx = obj->u.promise.reactions_head;
                    while (rx) {
                        ajs_gc_mark_value(vm, rx->on_fulfilled);
                        ajs_gc_mark_value(vm, rx->on_rejected);
                        ajs_gc_mark_value(vm, rx->resolve_fn);
                        ajs_gc_mark_value(vm, rx->reject_fn);
                        ajs_gc_mark_value(vm, rx->child_promise);
                        rx = rx->next;
                    }
                    break;
                }
                case AJS_OBJ_MAP:
                case AJS_OBJ_SET:
                    for (uint32_t i = 0; i < obj->u.col.entries_len; i++) {
                        if (obj->u.col.entries[i].key != AJS_VAL_EMPTY) {
                            ajs_gc_mark_value(vm, obj->u.col.entries[i].key);
                            ajs_gc_mark_value(vm, obj->u.col.entries[i].val);
                        }
                    }
                    break;
                case AJS_OBJ_WEAKMAP:
                case AJS_OBJ_WEAKSET:
                case AJS_OBJ_WEAKREF:
                    /* Weak keys/targets handled during ephemeron & sweep phases */
                    break;
                case AJS_OBJ_FINREG:
                    ajs_gc_mark_value(vm, obj->u.finreg.cleanup_cb);
                    for (uint32_t i = 0; i < obj->u.finreg.count; i++) {
                        if (obj->u.finreg.entries[i].alive) {
                            ajs_gc_mark_value(vm, obj->u.finreg.entries[i].held_value);
                        }
                    }
                    break;
                case AJS_OBJ_ITERATOR:
                    ajs_gc_mark_value(vm, obj->u.iter.target);
                    ajs_gc_mark_value(vm, obj->u.iter.next_method);
                    ajs_gc_mark_value(vm, obj->u.iter.helper_fn);
                    ajs_gc_mark_value(vm, obj->u.iter.inner_iter);
                    ajs_gc_mark_value(vm, obj->u.iter.inner_next);
                    break;
                case AJS_OBJ_GENERATOR:
                case AJS_OBJ_ASYNC_GENERATOR: {
                    ajs_gc_mark_ptr(vm, obj->u.gen.frame);
                    ajs_gc_mark_value(vm, obj->u.gen.async_promise);
                    ajs_gc_mark_value(vm, obj->u.gen.async_resolve);
                    ajs_gc_mark_value(vm, obj->u.gen.async_reject);
                    AjsAsyncGenReq *req = obj->u.gen.req_head;
                    while (req) {
                        ajs_gc_mark_value(vm, req->arg);
                        ajs_gc_mark_value(vm, req->promise);
                        ajs_gc_mark_value(vm, req->resolve_fn);
                        ajs_gc_mark_value(vm, req->reject_fn);
                        req = req->next;
                    }
                    break;
                }
                case AJS_OBJ_TYPED_ARRAY:
                case AJS_OBJ_DATA_VIEW:
                    ajs_gc_mark_ptr(vm, obj->u.view.buffer);
                    break;
                case AJS_OBJ_PROXY:
                    ajs_gc_mark_value(vm, obj->u.proxy.target);
                    ajs_gc_mark_value(vm, obj->u.proxy.handler);
                    break;
                case AJS_OBJ_MODULE_NS:
                    ajs_gc_mark_ptr(vm, obj->u.mod_ns.module);
                    for (uint32_t i = 0; i < obj->u.mod_ns.export_count; i++) {
                        ajs_gc_mark_ptr(vm, obj->u.mod_ns.export_names[i]);
                    }
                    break;
                case AJS_OBJ_INTL:
                    if (obj->u.intl) {
                        ajs_gc_mark_value(vm, obj->u.intl->bound_fn);
                        ajs_gc_mark_value(vm, obj->u.intl->seg_input);
                        ajs_gc_mark_value(vm, obj->u.intl->segmenter_obj);
                    }
                    break;
                default:
                    break;
            }
            break;
        }
    }
}

void ajs_gc_free_header(AjsVm *vm, AjsGcHeader *hdr) {
    switch ((AjsGcKind)hdr->gc_kind) {
        case AJS_GC_STRING:
        case AJS_GC_SYMBOL:
        case AJS_GC_BIGINT:
            break;
        case AJS_GC_ENV: {
            AjsEnv *env = (AjsEnv *)hdr;
            free(env->slots);
            free(env->flags);
            free(env->names);
            free(env->import_links);
            break;
        }
        case AJS_GC_BYTECODE: {
            AjsBytecode *bc = (AjsBytecode *)hdr;
            free(bc->mapped_param_slots);
            free(bc->env_slot_flags);
            free(bc->env_slot_names);
            free(bc->code);
            free(bc->consts);
            for (uint32_t i = 0; i < bc->tpl_count; i++) {
                free(bc->tpls[i].cooked);
                free(bc->tpls[i].raw);
            }
            free(bc->tpls);
            for (uint32_t i = 0; i < bc->env_desc_count; i++) {
                free(bc->env_descs[i].slot_flags);
                free(bc->env_descs[i].names);
            }
            free(bc->env_descs);
            free(bc->lines);
            break;
        }
        case AJS_GC_FRAME: {
            AjsExecFrame *f = (AjsExecFrame *)hdr;
            free(f->stack);
            break;
        }
        case AJS_GC_MODULE: {
            AjsModuleRecord *m = (AjsModuleRecord *)hdr;
            free(m->url);
            free(m->requested_specifiers);
            free(m->loaded_modules);
            free(m->imports);
            free(m->exports);
            free(m->hoisted_fns);
            free(m->async_parents);
            break;
        }
        case AJS_GC_OBJECT: {
            AjsObject *obj = (AjsObject *)hdr;
            if (obj->finalizer && obj->opaque_ptr) {
                obj->finalizer(vm, obj->opaque_ptr);
                obj->opaque_ptr = NULL;
            }
            free(obj->props);
            free(obj->hash_tab);
            free(obj->privs);
            switch (obj->kind) {
                case AJS_OBJ_ARRAY:
                    free(obj->u.arr.dense);
                    break;
                case AJS_OBJ_FUNCTION:
                    free(obj->u.fn.bound_argv);
                    break;
                case AJS_OBJ_ARGUMENTS:
                    free(obj->u.args.param_slots);
                    break;
                case AJS_OBJ_ERROR:
                    free(obj->u.err.stack_str);
                    break;
                case AJS_OBJ_REGEXP:
                    if (obj->u.regexp) ajs_regexp_free(obj->u.regexp);
                    break;
                case AJS_OBJ_PROMISE: {
                    AjsPromiseReaction *rx = obj->u.promise.reactions_head;
                    while (rx) {
                        AjsPromiseReaction *nxt = rx->next;
                        free(rx);
                        rx = nxt;
                    }
                    break;
                }
                case AJS_OBJ_MAP:
                case AJS_OBJ_SET:
                case AJS_OBJ_WEAKMAP:
                case AJS_OBJ_WEAKSET:
                    free(obj->u.col.entries);
                    free(obj->u.col.buckets);
                    break;
                case AJS_OBJ_FINREG:
                    free(obj->u.finreg.entries);
                    break;
                case AJS_OBJ_GENERATOR:
                case AJS_OBJ_ASYNC_GENERATOR: {
                    AjsAsyncGenReq *req = obj->u.gen.req_head;
                    while (req) {
                        AjsAsyncGenReq *nxt = req->next;
                        free(req);
                        req = nxt;
                    }
                    break;
                }
                case AJS_OBJ_ARRAY_BUFFER:
                case AJS_OBJ_SHARED_ARRAY_BUFFER:
                    free(obj->u.buf.bytes);
                    break;
                case AJS_OBJ_MODULE_NS:
                    free(obj->u.mod_ns.export_names);
                    break;
                case AJS_OBJ_INTL:
                    free(obj->u.intl);
                    break;
                default:
                    break;
            }
            break;
        }
    }
    if (vm->gc_bytes >= hdr->gc_size) vm->gc_bytes -= hdr->gc_size;
    else vm->gc_bytes = 0;

    uint8_t *raw = (uint8_t *)hdr - hdr->alloc_shift;
    free(raw);
}

static bool ajs_val_is_marked(AjsValue v) {
    if (!ajs_val_is_ptr(v)) return true;
    return ajs_val_to_gc(v)->gc_marked != 0;
}

void ajs_gc_collect(AjsVm *vm) {
    if (!vm || vm->gc_running || vm->destroying) return;
    vm->gc_running = true;

    /* 1. Mark strong roots */
    ajs_gc_mark_value(vm, vm->oom_error);
    for (size_t i = 0; i < vm->handle_top; i++) {
        ajs_gc_mark_value(vm, vm->handle_stack[i]);
    }
    for (size_t i = 0; i < vm->temp_top; i++) {
        ajs_gc_mark_value(vm, vm->temp_roots[i]);
    }
    for (uint32_t i = 0; i < vm->pins_cap; i++) {
        if (vm->pin_used[i]) ajs_gc_mark_value(vm, vm->pins[i]);
    }
    for (AjsRealm *r = vm->realms; r; r = r->next) {
        ajs_gc_mark_realm(vm, r);
    }
    ajs_gc_mark_ptr(vm, vm->str_empty);
    ajs_gc_mark_ptr(vm, vm->str_length);
    ajs_gc_mark_ptr(vm, vm->str_prototype);
    ajs_gc_mark_ptr(vm, vm->str_constructor);
    ajs_gc_mark_ptr(vm, vm->str_name);
    ajs_gc_mark_ptr(vm, vm->str_message);
    ajs_gc_mark_ptr(vm, vm->str_stack);
    ajs_gc_mark_ptr(vm, vm->str_cause);
    ajs_gc_mark_ptr(vm, vm->str_errors);
    ajs_gc_mark_ptr(vm, vm->str_value);
    ajs_gc_mark_ptr(vm, vm->str_done);
    ajs_gc_mark_ptr(vm, vm->str_next);
    ajs_gc_mark_ptr(vm, vm->str_return);
    ajs_gc_mark_ptr(vm, vm->str_throw);
    ajs_gc_mark_ptr(vm, vm->str_then);
    ajs_gc_mark_ptr(vm, vm->str_toJSON);
    ajs_gc_mark_ptr(vm, vm->str_toString);
    ajs_gc_mark_ptr(vm, vm->str_valueOf);
    ajs_gc_mark_ptr(vm, vm->str_default);
    ajs_gc_mark_ptr(vm, vm->str_raw);

    for (int i = 0; i < AJS_SYM_COUNT; i++) {
        ajs_gc_mark_ptr(vm, vm->well_known_syms[i]);
    }
    for (uint32_t i = 0; i < vm->sym_reg_count; i++) {
        ajs_gc_mark_ptr(vm, vm->sym_reg[i].key);
        ajs_gc_mark_ptr(vm, vm->sym_reg[i].sym);
    }
    for (size_t i = 0; i < vm->call_depth; i++) {
        ajs_gc_mark_ptr(vm, vm->call_stack[i].frame);
        ajs_gc_mark_ptr(vm, vm->call_stack[i].native_fn_obj);
    }
    if (vm->has_exception) {
        ajs_gc_mark_value(vm, vm->exception);
    }
    for (AjsJobNode *job = vm->job_head; job; job = job->next) {
        ajs_gc_mark_value(vm, job->fn);
        for (int i = 0; i < job->argc; i++) ajs_gc_mark_value(vm, job->argv[i]);
    }
    for (uint32_t i = 0; i < vm->module_count; i++) {
        ajs_gc_mark_ptr(vm, vm->modules[i]);
    }

    /* 2. Ephemeron fixed-point loop for WeakMap */
    bool changed = true;
    while (changed) {
        changed = false;
        for (AjsGcHeader *h = vm->gc_head; h; h = h->gc_next) {
            if (h->gc_marked && h->gc_kind == AJS_GC_OBJECT) {
                AjsObject *obj = (AjsObject *)h;
                if (obj->kind == AJS_OBJ_WEAKMAP) {
                    for (uint32_t i = 0; i < obj->u.col.entries_len; i++) {
                        AjsMapEntry *e = &obj->u.col.entries[i];
                        if (e->key != AJS_VAL_EMPTY && ajs_val_is_marked(e->key) && !ajs_val_is_marked(e->val)) {
                            ajs_gc_mark_value(vm, e->val);
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    /* 3. Update WeakRef, WeakMap/WeakSet, FinalizationRegistry, and Intern Table before sweep */
    for (AjsGcHeader *h = vm->gc_head; h; h = h->gc_next) {
        if (!h->gc_marked || h->gc_kind != AJS_GC_OBJECT) continue;
        AjsObject *obj = (AjsObject *)h;
        if (obj->kind == AJS_OBJ_WEAKREF) {
            if (obj->u.prim_val != AJS_VAL_UNDEFINED && !ajs_val_is_marked(obj->u.prim_val)) {
                obj->u.prim_val = AJS_VAL_UNDEFINED;
            }
        } else if (obj->kind == AJS_OBJ_WEAKMAP || obj->kind == AJS_OBJ_WEAKSET) {
            for (uint32_t i = 0; i < obj->u.col.entries_len; i++) {
                AjsMapEntry *e = &obj->u.col.entries[i];
                if (e->key != AJS_VAL_EMPTY && !ajs_val_is_marked(e->key)) {
                    e->key = AJS_VAL_EMPTY;
                    e->val = AJS_VAL_UNDEFINED;
                    if (obj->u.col.live_count > 0) obj->u.col.live_count--;
                }
            }
        } else if (obj->kind == AJS_OBJ_FINREG) {
            for (uint32_t i = 0; i < obj->u.finreg.count; i++) {
                AjsFinRegEntry *fe = &obj->u.finreg.entries[i];
                if (!fe->alive) continue;
                if (!ajs_val_is_marked(fe->target)) {
                    fe->alive = false;
                    fe->target = AJS_VAL_UNDEFINED;
                    fe->unregister_token = AJS_VAL_UNDEFINED;
                    ajs_finreg_schedule_cleanup(vm, obj, fe->held_value);
                } else if (fe->unregister_token != AJS_VAL_UNDEFINED && !ajs_val_is_marked(fe->unregister_token)) {
                    fe->unregister_token = AJS_VAL_UNDEFINED;
                }
            }
        }
    }

    for (uint32_t i = 0; i < vm->intern_cap; i++) {
        AjsString *s = vm->intern_tab[i];
        if (s && s != (AjsString *)(uintptr_t)1 && !s->gc.gc_marked) {
            vm->intern_tab[i] = (AjsString *)(uintptr_t)1; /* Tombstone */
        }
    }

    /* 4. Sweep unmarked heap objects */
    AjsGcHeader **prev = &vm->gc_head;
    AjsGcHeader *cur = vm->gc_head;
    while (cur) {
        AjsGcHeader *next = cur->gc_next;
        if (cur->gc_marked) {
            cur->gc_marked = 0;
            prev = &cur->gc_next;
        } else {
            *prev = next;
            ajs_gc_free_header(vm, cur);
        }
        cur = next;
    }

    size_t limit = vm->cfg.heap_limit_bytes ? vm->cfg.heap_limit_bytes : (32u * 1024u * 1024u);
    size_t next_thresh = vm->gc_bytes * 2u;
    if (next_thresh < 256u * 1024u) next_thresh = 256u * 1024u;
    if (next_thresh > limit) next_thresh = limit;
    vm->gc_threshold = next_thresh;

    vm->gc_running = false;
}

void ajs_gc_free_all(AjsVm *vm) {
    vm->destroying = true;
    AjsGcHeader *cur = vm->gc_head;
    vm->gc_head = NULL;
    while (cur) {
        AjsGcHeader *next = cur->gc_next;
        ajs_gc_free_header(vm, cur);
        cur = next;
    }
}
