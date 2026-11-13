/*
 * ArcradientJS 1.0 (Clean-Room Pure C11 ECMAScript 2026 Virtual Machine)
 * js/builtins/ajs_promise.c - Promise (ES2026 incl. withResolvers & Promise.try),
 * Thenable Assimilation, Unhandled Rejection Hook & FIFO Microtask Job Queue
 */
#include "../vm/ajs_internal.h"

#define AJS_METHOD_FLAGS (AJS_PROP_WRITABLE | AJS_PROP_CONFIGURABLE)

/* ============================================================================
 * 1. Internal Closure Helpers (GC-traced via fn->u.fn.closure_env)
 * ============================================================================ */

static AjsObject *prom_make_closure_fn(AjsVm *vm, AjsRealm *r, AjsNativeFn fn, const char *name,
                                       int length, bool is_ctor, int32_t magic, AjsEnv *env) {
    size_t rs = ajs_temp_root_save(vm);
    if (env) ajs_temp_root_push(vm, ajs_val_from_gc(env));
    AjsObject *f = ajs_obj_new(vm, r, AJS_OBJ_FUNCTION, r->function_proto);
    if (!f) { ajs_temp_root_restore(vm, rs); return NULL; }
    ajs_temp_root_push(vm, ajs_val_from_gc(f));
    f->u.fn.fn_kind = AJS_FN_NATIVE;
    f->u.fn.is_constructor = is_ctor;
    f->u.fn.strict = true;
    f->u.fn.native_fn = fn;
    f->u.fn.native_magic = magic;
    f->u.fn.closure_env = env;

    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
              AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    d.value = ajs_val_number((double)(length < 0 ? 0 : length));
    ajs_obj_define_own_prop(vm, f, ajs_val_from_gc(vm->str_length), &d, false);
    d.value = ajs_val_from_gc(ajs_str_from_cstr(vm, name ? name : "", true));
    ajs_obj_define_own_prop(vm, f, ajs_val_from_gc(vm->str_name), &d, false);
    ajs_temp_root_restore(vm, rs);
    return f;
}

static AjsEnv *current_fn_env(const AjsVm *vm) {
    if (vm->call_depth == 0) return NULL;
    AjsObject *fn = vm->call_stack[vm->call_depth - 1u].native_fn_obj;
    return fn ? fn->u.fn.closure_env : NULL;
}

static int32_t current_fn_magic(const AjsVm *vm) {
    if (vm->call_depth == 0) return 0;
    AjsObject *fn = vm->call_stack[vm->call_depth - 1u].native_fn_obj;
    return fn ? fn->u.fn.native_magic : 0;
}

/* ============================================================================
 * 2. FIFO Microtask Job Queue
 * ============================================================================ */

void ajs_enqueue_job_ex(AjsVm *vm, AjsRealm *realm, AjsValue fn, int argc, const AjsValue *argv) {
    if (!vm || !ajs_is_callable(vm, fn)) return;
    AjsJobNode *node = (AjsJobNode *)calloc(1, sizeof(AjsJobNode));
    if (!node) return;
    node->fn = fn;
    node->argc = argc > 3 ? 3 : (argc < 0 ? 0 : argc);
    for (int i = 0; i < node->argc; i++) node->argv[i] = argv[i];
    node->realm = realm ? realm : vm->active_realm;
    node->next = NULL;
    if (vm->job_tail) {
        vm->job_tail->next = node;
        vm->job_tail = node;
    } else {
        vm->job_head = node;
        vm->job_tail = node;
    }
}

void ajs_enqueue_microtask(AjsVm *vm, AjsValue callback_fn) {
    if (!vm) return;
    ajs_enqueue_job_ex(vm, vm->active_realm, callback_fn, 0, NULL);
}

bool ajs_has_pending_jobs(const AjsVm *vm) {
    return vm && vm->job_head != NULL;
}

bool ajs_drain_microtasks(AjsVm *vm) {
    if (!vm) return false;
    if (vm->draining_jobs) return true;
    vm->draining_jobs = true;
    bool ok = true;
    AjsValue uncaught_exc = AJS_VAL_UNDEFINED;
    bool has_uncaught = false;

    while (vm->job_head != NULL) {
        AjsJobNode *job = vm->job_head;
        vm->job_head = job->next;
        if (!vm->job_head) vm->job_tail = NULL;

        size_t rs = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, job->fn);
        for (int i = 0; i < job->argc; i++) ajs_temp_root_push(vm, job->argv[i]);
        if (has_uncaught) ajs_temp_root_push(vm, uncaught_exc);

        AjsRealm *saved_realm = vm->active_realm;
        if (job->realm) vm->active_realm = job->realm;

        ajs_call_internal(vm, job->fn, AJS_VAL_UNDEFINED, job->argc, job->argv);
        vm->active_realm = saved_realm;
        free(job);

        if (vm->has_exception) {
            ok = false;
            if (!has_uncaught) {
                uncaught_exc = vm->exception;
                has_uncaught = true;
            }
            ajs_clear_exception(vm);
        }
        ajs_temp_root_restore(vm, rs);
    }

    vm->draining_jobs = false;
    if (has_uncaught) {
        ajs_throw(vm, uncaught_exc);
    }
    return ok;
}

/* ============================================================================
 * 3. Promise Reaction & Thenable Assimilation Jobs
 * ============================================================================ */

static void create_resolving_functions(AjsVm *vm, AjsRealm *r, AjsValue prom_val,
                                       AjsValue *out_resolve, AjsValue *out_reject);

/* Reaction job: env slots = [handler, resolve_fn, reject_fn, argument], magic = is_reject (0 or 1) */
static AjsValue promise_reaction_job_fn(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)argc; (void)argv; (void)ud;
    AjsEnv *env = current_fn_env(vm);
    if (!env || env->count < 4) return AJS_VAL_UNDEFINED;
    bool is_reject = (current_fn_magic(vm) != 0);

    AjsValue handler = env->slots[0];
    AjsValue res_fn  = env->slots[1];
    AjsValue rej_fn  = env->slots[2];
    AjsValue arg     = env->slots[3];

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, handler);
    ajs_temp_root_push(vm, res_fn);
    ajs_temp_root_push(vm, rej_fn);
    ajs_temp_root_push(vm, arg);

    if (ajs_is_undefined(handler)) {
        /* Fallback Identity (fulfill) or Thrower (reject) */
        if (is_reject) {
            if (ajs_is_callable(vm, rej_fn)) {
                ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &arg);
                ajs_clear_exception(vm);
            }
        } else {
            if (ajs_is_callable(vm, res_fn)) {
                ajs_call_internal(vm, res_fn, AJS_VAL_UNDEFINED, 1, &arg);
                ajs_clear_exception(vm);
            }
        }
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_UNDEFINED;
    }

    AjsValue hres = ajs_call_internal(vm, handler, AJS_VAL_UNDEFINED, 1, &arg);
    if (vm->has_exception) {
        AjsValue exc = vm->exception;
        ajs_temp_root_push(vm, exc);
        ajs_clear_exception(vm);
        if (ajs_is_callable(vm, rej_fn)) {
            ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &exc);
            ajs_clear_exception(vm);
        }
    } else {
        ajs_temp_root_push(vm, hres);
        if (ajs_is_callable(vm, res_fn)) {
            ajs_call_internal(vm, res_fn, AJS_VAL_UNDEFINED, 1, &hres);
            ajs_clear_exception(vm);
        }
    }
    ajs_temp_root_restore(vm, rs);
    return AJS_VAL_UNDEFINED;
}

static void enqueue_reaction(AjsVm *vm, AjsRealm *r, const AjsPromiseReaction *rx, AjsValue val, bool is_reject) {
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, val);
    AjsEnv *env = ajs_env_new(vm, NULL, 4, NULL);
    if (!env) { ajs_temp_root_restore(vm, rs); return; }
    env->slots[0] = is_reject ? rx->on_rejected : rx->on_fulfilled;
    env->slots[1] = rx->resolve_fn;
    env->slots[2] = rx->reject_fn;
    env->slots[3] = val;
    AjsObject *jfn = prom_make_closure_fn(vm, r, promise_reaction_job_fn, "", 0, false, is_reject ? 1 : 0, env);
    if (jfn) {
        ajs_enqueue_job_ex(vm, r, ajs_val_from_gc(jfn), 0, NULL);
    }
    ajs_temp_root_restore(vm, rs);
}

static void trigger_promise_reactions(AjsVm *vm, AjsRealm *r, AjsPromiseReaction *head, AjsValue val, bool is_reject) {
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, val);
    AjsPromiseReaction *cur = head;
    while (cur) {
        AjsPromiseReaction *nxt = cur->next;
        enqueue_reaction(vm, r, cur, val, is_reject);
        free(cur);
        cur = nxt;
    }
    ajs_temp_root_restore(vm, rs);
}

static void fulfill_promise_internal(AjsVm *vm, AjsObject *p, AjsValue val) {
    if (p->u.promise.state != AJS_PROMISE_PENDING) return;
    AjsPromiseReaction *rxs = p->u.promise.reactions_head;
    p->u.promise.reactions_head = NULL;
    p->u.promise.reactions_tail = NULL;
    p->u.promise.result = val;
    p->u.promise.state = AJS_PROMISE_FULFILLED;
    trigger_promise_reactions(vm, p->realm ? p->realm : vm->active_realm, rxs, val, false);
}

static void reject_promise_internal(AjsVm *vm, AjsObject *p, AjsValue reason) {
    if (p->u.promise.state != AJS_PROMISE_PENDING) return;
    AjsPromiseReaction *rxs = p->u.promise.reactions_head;
    p->u.promise.reactions_head = NULL;
    p->u.promise.reactions_tail = NULL;
    p->u.promise.result = reason;
    p->u.promise.state = AJS_PROMISE_REJECTED;
    if (!p->u.promise.is_handled && vm->cfg.rejection_fn) {
        vm->cfg.rejection_fn(vm, ajs_val_from_gc(p), reason, false, vm->cfg.rejection_userdata);
    }
    trigger_promise_reactions(vm, p->realm ? p->realm : vm->active_realm, rxs, reason, true);
}

/* PromiseResolveThenableJob: argv = [promiseToResolve, thenable, thenAction] */
static AjsValue promise_resolve_thenable_job_fn(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 3) return AJS_VAL_UNDEFINED;
    AjsValue prom_val = argv[0];
    AjsValue thenable = argv[1];
    AjsValue then_fn  = argv[2];

    size_t rs = ajs_temp_root_save(vm);
    AjsValue res_fn = AJS_VAL_UNDEFINED, rej_fn = AJS_VAL_UNDEFINED;
    create_resolving_functions(vm, vm->active_realm, prom_val, &res_fn, &rej_fn);
    ajs_temp_root_push(vm, res_fn);
    ajs_temp_root_push(vm, rej_fn);

    AjsValue cargs[2] = { res_fn, rej_fn };
    ajs_call_internal(vm, then_fn, thenable, 2, cargs);
    if (vm->has_exception) {
        AjsValue exc = vm->exception;
        ajs_temp_root_push(vm, exc);
        ajs_clear_exception(vm);
        ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &exc);
        ajs_clear_exception(vm);
    }
    ajs_temp_root_restore(vm, rs);
    return AJS_VAL_UNDEFINED;
}

static void resolve_promise_internal(AjsVm *vm, AjsObject *p, AjsValue resolution) {
    if (p->u.promise.state != AJS_PROMISE_PENDING) return;
    AjsValue pv = ajs_val_from_gc(p);
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, pv);
    ajs_temp_root_push(vm, resolution);

    if (ajs_same_value(vm, resolution, pv)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Chaining cycle detected for promise");
        AjsValue self_err = vm->exception;
        ajs_temp_root_push(vm, self_err);
        ajs_clear_exception(vm);
        reject_promise_internal(vm, p, self_err);
        ajs_temp_root_restore(vm, rs);
        return;
    }

    if (!ajs_is_object(vm, resolution)) {
        fulfill_promise_internal(vm, p, resolution);
        ajs_temp_root_restore(vm, rs);
        return;
    }

    AjsValue then_prop = ajs_obj_get(vm, resolution, ajs_val_from_gc(vm->str_then), resolution);
    if (vm->has_exception) {
        AjsValue exc = vm->exception;
        ajs_temp_root_push(vm, exc);
        ajs_clear_exception(vm);
        reject_promise_internal(vm, p, exc);
        ajs_temp_root_restore(vm, rs);
        return;
    }
    if (!ajs_is_callable(vm, then_prop)) {
        fulfill_promise_internal(vm, p, resolution);
        ajs_temp_root_restore(vm, rs);
        return;
    }

    ajs_temp_root_push(vm, then_prop);
    AjsRealm *r = p->realm ? p->realm : vm->active_realm;
    AjsObject *jfn = prom_make_closure_fn(vm, r, promise_resolve_thenable_job_fn, "", 0, false, 0, NULL);
    if (jfn) {
        AjsValue jargs[3] = { pv, resolution, then_prop };
        ajs_enqueue_job_ex(vm, r, ajs_val_from_gc(jfn), 3, jargs);
    }
    ajs_temp_root_restore(vm, rs);
}

/* Built-in Resolving Function: env slots = [promise, already_resolved_bool], magic = 0 (resolve) or 1 (reject) */
static AjsValue resolving_fn_cb(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsEnv *env = current_fn_env(vm);
    if (!env || env->count < 2) return AJS_VAL_UNDEFINED;
    if (env->slots[1] == AJS_VAL_TRUE) return AJS_VAL_UNDEFINED;
    env->slots[1] = AJS_VAL_TRUE;

    AjsValue pv = env->slots[0];
    if (!ajs_val_is_gc_kind(pv, AJS_GC_OBJECT)) return AJS_VAL_UNDEFINED;
    AjsObject *p = (AjsObject *)ajs_val_to_gc(pv);
    if (p->kind != AJS_OBJ_PROMISE) return AJS_VAL_UNDEFINED;

    AjsValue arg = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (current_fn_magic(vm) == 0) {
        resolve_promise_internal(vm, p, arg);
    } else {
        reject_promise_internal(vm, p, arg);
    }
    return AJS_VAL_UNDEFINED;
}

static void create_resolving_functions(AjsVm *vm, AjsRealm *r, AjsValue prom_val,
                                       AjsValue *out_resolve, AjsValue *out_reject) {
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, prom_val);
    AjsEnv *env = ajs_env_new(vm, NULL, 2, NULL);
    env->slots[0] = prom_val;
    env->slots[1] = AJS_VAL_FALSE;
    ajs_temp_root_push(vm, ajs_val_from_gc(env));

    AjsObject *res = prom_make_closure_fn(vm, r, resolving_fn_cb, "", 1, false, 0, env);
    ajs_temp_root_push(vm, ajs_val_from_gc(res));
    AjsObject *rej = prom_make_closure_fn(vm, r, resolving_fn_cb, "", 1, false, 1, env);
    *out_resolve = ajs_val_from_gc(res);
    *out_reject  = ajs_val_from_gc(rej);
    ajs_temp_root_restore(vm, rs);
}

void ajs_promise_then_internal(AjsVm *vm, AjsObject *prom_obj, AjsValue on_fulfill,
                               AjsValue on_reject, AjsValue res_fn, AjsValue rej_fn, AjsValue child_prom) {
    if (!prom_obj || prom_obj->kind != AJS_OBJ_PROMISE) return;
    if (!ajs_is_callable(vm, on_fulfill)) on_fulfill = AJS_VAL_UNDEFINED;
    if (!ajs_is_callable(vm, on_reject))  on_reject  = AJS_VAL_UNDEFINED;

    AjsRealm *r = prom_obj->realm ? prom_obj->realm : vm->active_realm;
    if (prom_obj->u.promise.state == AJS_PROMISE_PENDING) {
        AjsPromiseReaction *rx = (AjsPromiseReaction *)calloc(1, sizeof(AjsPromiseReaction));
        if (!rx) return;
        rx->on_fulfilled  = on_fulfill;
        rx->on_rejected   = on_reject;
        rx->resolve_fn    = res_fn;
        rx->reject_fn     = rej_fn;
        rx->child_promise = child_prom;
        if (prom_obj->u.promise.reactions_tail) {
            prom_obj->u.promise.reactions_tail->next = rx;
            prom_obj->u.promise.reactions_tail = rx;
        } else {
            prom_obj->u.promise.reactions_head = rx;
            prom_obj->u.promise.reactions_tail = rx;
        }
        prom_obj->u.promise.is_handled = true;
    } else if (prom_obj->u.promise.state == AJS_PROMISE_FULFILLED) {
        AjsPromiseReaction rx = { on_fulfill, on_reject, res_fn, rej_fn, child_prom, NULL };
        enqueue_reaction(vm, r, &rx, prom_obj->u.promise.result, false);
    } else {
        if (!prom_obj->u.promise.is_handled && vm->cfg.rejection_fn) {
            vm->cfg.rejection_fn(vm, ajs_val_from_gc(prom_obj), prom_obj->u.promise.result,
                                 true, vm->cfg.rejection_userdata);
        }
        prom_obj->u.promise.is_handled = true;
        AjsPromiseReaction rx = { on_fulfill, on_reject, res_fn, rej_fn, child_prom, NULL };
        enqueue_reaction(vm, r, &rx, prom_obj->u.promise.result, true);
    }
}

/* ============================================================================
 * 4. Public C API: ajs_promise_create & Internal NewPromiseCapability
 * ============================================================================ */

AjsValue ajs_promise_create(AjsVm *vm, AjsValue *out_resolve_fn, AjsValue *out_reject_fn) {
    if (!vm) return AJS_VAL_UNDEFINED;
    AjsRealm *r = vm->active_realm ? vm->active_realm : vm->main_realm;
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *p = ajs_obj_new(vm, r, AJS_OBJ_PROMISE, r->promise_proto);
    if (!p) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    p->u.promise.state = AJS_PROMISE_PENDING;
    p->u.promise.result = AJS_VAL_UNDEFINED;
    AjsValue pv = ajs_val_from_gc(p);
    ajs_temp_root_push(vm, pv);

    AjsValue res_fn = AJS_VAL_UNDEFINED, rej_fn = AJS_VAL_UNDEFINED;
    create_resolving_functions(vm, r, pv, &res_fn, &rej_fn);
    ajs_temp_root_restore(vm, rs);

    ajs_handle_record(vm, pv);
    if (out_resolve_fn) { ajs_handle_record(vm, res_fn); *out_resolve_fn = res_fn; }
    if (out_reject_fn)  { ajs_handle_record(vm, rej_fn); *out_reject_fn  = rej_fn; }
    return pv;
}

typedef struct AjsPromiseCap {
    AjsValue promise;
    AjsValue resolve;
    AjsValue reject;
} AjsPromiseCap;

static AjsValue cap_executor_cb(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsEnv *env = current_fn_env(vm);
    if (!env || env->count < 2) return AJS_VAL_UNDEFINED;
    if (!ajs_is_undefined(env->slots[0]) || !ajs_is_undefined(env->slots[1])) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Promise executor already invoked");
    }
    env->slots[0] = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    env->slots[1] = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    return AJS_VAL_UNDEFINED;
}

/* Note: caller keeps an active temp_root frame; pushes cap.promise, cap.resolve, cap.reject onto temp_roots */
static bool new_promise_capability(AjsVm *vm, AjsValue ctor, AjsPromiseCap *out) {
    if (!ajs_is_constructor_val(ctor)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Promise capability target is not a constructor");
        return false;
    }
    AjsRealm *r = vm->active_realm;
    if (ctor == r->promise_ctor) {
        AjsObject *p = ajs_obj_new(vm, r, AJS_OBJ_PROMISE, r->promise_proto);
        if (!p) return false;
        p->u.promise.state = AJS_PROMISE_PENDING;
        p->u.promise.result = AJS_VAL_UNDEFINED;
        out->promise = ajs_val_from_gc(p);
        ajs_temp_root_push(vm, out->promise);
        create_resolving_functions(vm, r, out->promise, &out->resolve, &out->reject);
        ajs_temp_root_push(vm, out->resolve);
        ajs_temp_root_push(vm, out->reject);
        return true;
    }

    AjsEnv *env = ajs_env_new(vm, NULL, 2, NULL);
    env->slots[0] = AJS_VAL_UNDEFINED;
    env->slots[1] = AJS_VAL_UNDEFINED;
    ajs_temp_root_push(vm, ajs_val_from_gc(env));

    AjsObject *exec = prom_make_closure_fn(vm, r, cap_executor_cb, "", 2, false, 0, env);
    AjsValue exec_v = ajs_val_from_gc(exec);
    ajs_temp_root_push(vm, exec_v);

    AjsValue prom = ajs_construct_internal(vm, ctor, 1, &exec_v, ctor);
    if (vm->has_exception) return false;
    if (!ajs_is_callable(vm, env->slots[0]) || !ajs_is_callable(vm, env->slots[1])) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Promise resolve or reject function is not callable");
        return false;
    }
    out->promise = prom;
    out->resolve = env->slots[0];
    out->reject  = env->slots[1];
    ajs_temp_root_push(vm, out->promise);
    ajs_temp_root_push(vm, out->resolve);
    ajs_temp_root_push(vm, out->reject);
    return true;
}

AjsValue ajs_promise_resolve_val(AjsVm *vm, AjsRealm *realm, AjsValue ctor, AjsValue val) {
    (void)realm;
    if (!ajs_is_object(vm, ctor)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "PromiseResolve called on non-object");
    }
    if (ajs_val_is_gc_kind(val, AJS_GC_OBJECT) && ((AjsObject *)ajs_val_to_gc(val))->kind == AJS_OBJ_PROMISE) {
        AjsValue x_ctor = ajs_obj_get(vm, val, ajs_val_from_gc(vm->str_constructor), val);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        if (ajs_same_value(vm, x_ctor, ctor)) return val;
    }
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ctor);
    ajs_temp_root_push(vm, val);
    AjsPromiseCap cap;
    if (!new_promise_capability(vm, ctor, &cap)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    ajs_call_internal(vm, cap.resolve, AJS_VAL_UNDEFINED, 1, &val);
    ajs_temp_root_restore(vm, rs);
    return vm->has_exception ? AJS_VAL_EXCEPTION : cap.promise;
}

static AjsValue species_constructor(AjsVm *vm, AjsValue obj, AjsValue default_ctor) {
    AjsValue c = ajs_obj_get(vm, obj, ajs_val_from_gc(vm->str_constructor), obj);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (ajs_is_undefined(c)) return default_ctor;
    if (!ajs_is_object(vm, c)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object.constructor is not an object");
    }
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, c);
    AjsValue s = ajs_obj_get(vm, c, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_SPECIES]), c);
    ajs_temp_root_restore(vm, rs);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (ajs_is_nullish(s)) return default_ctor;
    if (ajs_is_constructor_val(s)) return s;
    return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Species is not a constructor");
}

/* ============================================================================
 * 5. Promise Constructor & Prototype Methods
 * ============================================================================ */

static AjsValue js_promise_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt == AJS_VAL_UNDEFINED) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Promise constructor cannot be invoked without 'new'");
    }
    AjsValue executor = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_is_callable(vm, executor)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Promise resolver is not a function");
    }
    AjsValue proto = vm->active_realm->promise_proto;
    if (ajs_val_is_gc_kind(nt, AJS_GC_OBJECT)) {
        AjsValue p = ajs_obj_get(vm, nt, ajs_val_from_gc(vm->str_prototype), nt);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        if (ajs_val_is_gc_kind(p, AJS_GC_OBJECT)) proto = p;
    }

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, proto);
    AjsObject *prom = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_PROMISE, proto);
    if (!prom) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    prom->u.promise.state = AJS_PROMISE_PENDING;
    prom->u.promise.result = AJS_VAL_UNDEFINED;
    AjsValue pv = ajs_val_from_gc(prom);
    ajs_temp_root_push(vm, pv);

    AjsValue res_fn = AJS_VAL_UNDEFINED, rej_fn = AJS_VAL_UNDEFINED;
    create_resolving_functions(vm, vm->active_realm, pv, &res_fn, &rej_fn);
    ajs_temp_root_push(vm, res_fn);
    ajs_temp_root_push(vm, rej_fn);

    AjsValue cargs[2] = { res_fn, rej_fn };
    ajs_call_internal(vm, executor, AJS_VAL_UNDEFINED, 2, cargs);
    if (vm->has_exception) {
        AjsValue exc = vm->exception;
        ajs_temp_root_push(vm, exc);
        ajs_clear_exception(vm);
        ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &exc);
        if (vm->has_exception) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    ajs_temp_root_restore(vm, rs);
    return pv;
}

static AjsValue js_promise_then(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT) || ((AjsObject *)ajs_val_to_gc(this_val))->kind != AJS_OBJ_PROMISE) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Method Promise.prototype.then called on incompatible receiver");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue c = species_constructor(vm, this_val, vm->active_realm->promise_ctor);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, c);

    AjsPromiseCap cap;
    if (!new_promise_capability(vm, c, &cap)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsValue on_f = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue on_r = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    ajs_promise_then_internal(vm, (AjsObject *)ajs_val_to_gc(this_val),
                              on_f, on_r, cap.resolve, cap.reject, cap.promise);
    ajs_temp_root_restore(vm, rs);
    return cap.promise;
}

static AjsValue js_promise_catch(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsValue on_r = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue then_fn = ajs_obj_get(vm, this_val, ajs_val_from_gc(vm->str_then), this_val);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, then_fn);
    AjsValue cargs[2] = { AJS_VAL_UNDEFINED, on_r };
    AjsValue res = ajs_call_internal(vm, then_fn, this_val, 2, cargs);
    ajs_temp_root_restore(vm, rs);
    return res;
}

/* Helpers for Promise.prototype.finally */
static AjsValue finally_value_thunk(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)argc; (void)argv; (void)ud;
    AjsEnv *env = current_fn_env(vm);
    if (!env || env->count < 1) return AJS_VAL_UNDEFINED;
    if (current_fn_magic(vm) != 0) return ajs_throw(vm, env->slots[0]);
    return env->slots[0];
}

/* env slots = [C, onFinally], magic = 0 (thenFinally) or 1 (catchFinally) */
static AjsValue finally_wrapper_cb(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsEnv *env = current_fn_env(vm);
    if (!env || env->count < 2) return AJS_VAL_UNDEFINED;
    bool is_catch = (current_fn_magic(vm) != 0);
    AjsValue c_ctor = env->slots[0];
    AjsValue on_fin = env->slots[1];
    AjsValue val_or_reason = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;

    size_t rs = ajs_temp_root_save(vm);
    AjsValue fres = ajs_call_internal(vm, on_fin, AJS_VAL_UNDEFINED, 0, NULL);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, fres);

    AjsValue p = ajs_promise_resolve_val(vm, vm->active_realm, c_ctor, fres);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, p);

    AjsEnv *tenv = ajs_env_new(vm, NULL, 1, NULL);
    tenv->slots[0] = val_or_reason;
    AjsObject *thunk = prom_make_closure_fn(vm, vm->active_realm, finally_value_thunk, "", 0, false, is_catch ? 1 : 0, tenv);
    AjsValue thunk_v = ajs_val_from_gc(thunk);
    ajs_temp_root_push(vm, thunk_v);

    AjsValue then_fn = ajs_obj_get(vm, p, ajs_val_from_gc(vm->str_then), p);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, then_fn);
    AjsValue out = ajs_call_internal(vm, then_fn, p, 1, &thunk_v);
    ajs_temp_root_restore(vm, rs);
    return out;
}

static AjsValue js_promise_finally(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_is_object(vm, this_val)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Promise.prototype.finally called on non-object");
    }
    AjsValue on_fin = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue c = species_constructor(vm, this_val, vm->active_realm->promise_ctor);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, c);

    AjsValue then_fin = on_fin, catch_fin = on_fin;
    if (ajs_is_callable(vm, on_fin)) {
        AjsEnv *env = ajs_env_new(vm, NULL, 2, NULL);
        env->slots[0] = c;
        env->slots[1] = on_fin;
        ajs_temp_root_push(vm, ajs_val_from_gc(env));
        then_fin  = ajs_val_from_gc(prom_make_closure_fn(vm, vm->active_realm, finally_wrapper_cb, "", 1, false, 0, env));
        ajs_temp_root_push(vm, then_fin);
        catch_fin = ajs_val_from_gc(prom_make_closure_fn(vm, vm->active_realm, finally_wrapper_cb, "", 1, false, 1, env));
        ajs_temp_root_push(vm, catch_fin);
    }

    AjsValue then_fn = ajs_obj_get(vm, this_val, ajs_val_from_gc(vm->str_then), this_val);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, then_fn);
    AjsValue cargs[2] = { then_fin, catch_fin };
    AjsValue res = ajs_call_internal(vm, then_fn, this_val, 2, cargs);
    ajs_temp_root_restore(vm, rs);
    return res;
}

/* ============================================================================
 * 6. Static Combinators: resolve, reject, withResolvers, try, race, all, allSettled, any
 * ============================================================================ */

static AjsValue js_promise_resolve_static(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_is_object(vm, this_val)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Promise.resolve called on non-object");
    }
    return ajs_promise_resolve_val(vm, vm->active_realm, this_val, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
}

static AjsValue js_promise_reject_static(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_is_object(vm, this_val)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Promise.reject called on non-object");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsPromiseCap cap;
    if (!new_promise_capability(vm, this_val, &cap)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsValue reason = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    ajs_call_internal(vm, cap.reject, AJS_VAL_UNDEFINED, 1, &reason);
    ajs_temp_root_restore(vm, rs);
    return vm->has_exception ? AJS_VAL_EXCEPTION : cap.promise;
}

static AjsValue js_promise_withResolvers(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    size_t rs = ajs_temp_root_save(vm);
    AjsPromiseCap cap;
    if (!new_promise_capability(vm, this_val, &cap)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsValue obj = ajs_new_object(vm);
    ajs_temp_root_push(vm, obj);
    ajs_define_prop_str(vm, obj, "promise", cap.promise, AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, obj, "resolve", cap.resolve, AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, obj, "reject",  cap.reject,  AJS_PROP_DEFAULT);
    ajs_temp_root_restore(vm, rs);
    return obj;
}

/* ES2025/2026 Promise.try(callbackfn, ...args) */
static AjsValue js_promise_try(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_is_object(vm, this_val)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Promise.try called on non-object");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsPromiseCap cap;
    if (!new_promise_capability(vm, this_val, &cap)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsValue cb = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    int fn_argc = argc > 1 ? (argc - 1) : 0;
    const AjsValue *fn_argv = argc > 1 ? &argv[1] : NULL;
    AjsValue status = ajs_call_internal(vm, cb, AJS_VAL_UNDEFINED, fn_argc, fn_argv);
    if (vm->has_exception) {
        AjsValue exc = vm->exception;
        ajs_temp_root_push(vm, exc);
        ajs_clear_exception(vm);
        ajs_call_internal(vm, cap.reject, AJS_VAL_UNDEFINED, 1, &exc);
    } else {
        ajs_temp_root_push(vm, status);
        ajs_call_internal(vm, cap.resolve, AJS_VAL_UNDEFINED, 1, &status);
    }
    ajs_temp_root_restore(vm, rs);
    return vm->has_exception ? AJS_VAL_EXCEPTION : cap.promise;
}

static AjsValue reject_cap_with_current_exc(AjsVm *vm, const AjsPromiseCap *cap) {
    size_t rs = ajs_temp_root_save(vm);
    AjsValue exc = vm->exception;
    ajs_temp_root_push(vm, exc);
    ajs_clear_exception(vm);
    ajs_call_internal(vm, cap->reject, AJS_VAL_UNDEFINED, 1, &exc);
    ajs_temp_root_restore(vm, rs);
    return vm->has_exception ? AJS_VAL_EXCEPTION : cap->promise;
}

static void close_iter_on_exc(AjsVm *vm, AjsValue iter) {
    size_t rs = ajs_temp_root_save(vm);
    AjsValue saved = vm->exception;
    ajs_temp_root_push(vm, saved);
    ajs_temp_root_push(vm, iter);
    ajs_clear_exception(vm);
    AjsValue rfn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_return), iter);
    if (!vm->has_exception && ajs_is_callable(vm, rfn)) {
        ajs_call_internal(vm, rfn, iter, 0, NULL);
    }
    ajs_throw(vm, saved);
    ajs_temp_root_restore(vm, rs);
}

/* Shared state for all / allSettled / any element callbacks:
 * shared_env: [values_arr, remaining_num, finish_fn, reject_fn]
 * elem_env:   [shared_env, index_num, already_called_bool]
 * magic: 0 = all_resolve, 1 = allSettled_fulfill, 2 = allSettled_reject, 3 = any_reject */
static AjsValue make_aggregate_err(AjsVm *vm, AjsValue errors_arr) {
    AjsValue ctor = vm->active_realm->error_ctor[AJS_ERR_KIND_AGGREGATE];
    AjsValue msg = ajs_string_cstr(vm, "All promises were rejected");
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, errors_arr);
    ajs_temp_root_push(vm, msg);
    AjsValue cargs[2] = { errors_arr, msg };
    AjsValue err = ajs_construct_internal(vm, ctor, 2, cargs, ctor);
    ajs_temp_root_restore(vm, rs);
    return err;
}

static AjsValue combinator_elem_cb(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsEnv *eenv = current_fn_env(vm);
    if (!eenv || eenv->count < 3) return AJS_VAL_UNDEFINED;
    if (eenv->slots[2] == AJS_VAL_TRUE) return AJS_VAL_UNDEFINED;
    eenv->slots[2] = AJS_VAL_TRUE;

    AjsEnv *senv = (AjsEnv *)ajs_val_to_gc(eenv->slots[0]);
    uint32_t idx = (uint32_t)ajs_val_to_number_unchecked(eenv->slots[1]);
    int32_t mode = current_fn_magic(vm);
    AjsValue x = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, x);

    AjsValue store_val = x;
    if (mode == 1 || mode == 2) {
        AjsValue obj = ajs_new_object(vm);
        ajs_temp_root_push(vm, obj);
        ajs_define_prop_str(vm, obj, "status",
                            ajs_string_cstr(vm, mode == 1 ? "fulfilled" : "rejected"), AJS_PROP_DEFAULT);
        ajs_define_prop_str(vm, obj, mode == 1 ? "value" : "reason", x, AJS_PROP_DEFAULT);
        store_val = obj;
    }
    ajs_set_prop_index(vm, senv->slots[0], idx, store_val);
    ajs_clear_exception(vm);

    double rem = ajs_val_to_number_unchecked(senv->slots[1]) - 1.0;
    senv->slots[1] = ajs_val_number(rem);
    if (rem <= 0.0) {
        if (mode == 3) {
            AjsValue agg = make_aggregate_err(vm, senv->slots[0]);
            if (!vm->has_exception) {
                ajs_temp_root_push(vm, agg);
                ajs_call_internal(vm, senv->slots[3], AJS_VAL_UNDEFINED, 1, &agg);
            }
        } else {
            AjsValue arr = senv->slots[0];
            ajs_call_internal(vm, senv->slots[2], AJS_VAL_UNDEFINED, 1, &arr);
        }
        ajs_clear_exception(vm);
    }
    ajs_temp_root_restore(vm, rs);
    return AJS_VAL_UNDEFINED;
}

/* magic: 0 = race, 1 = all, 2 = allSettled, 3 = any */
static AjsValue js_promise_combinator(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    int32_t comb_kind = current_fn_magic(vm);
    size_t rs = ajs_temp_root_save(vm);

    AjsPromiseCap cap;
    if (!new_promise_capability(vm, this_val, &cap)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }

    AjsValue resolve_method = ajs_obj_get(vm, this_val, ajs_val_from_gc(ajs_str_from_cstr(vm, "resolve", true)), this_val);
    if (vm->has_exception || !ajs_is_callable(vm, resolve_method)) {
        if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Promise.resolve is not callable");
        AjsValue out = reject_cap_with_current_exc(vm, &cap);
        ajs_temp_root_restore(vm, rs);
        return out;
    }
    ajs_temp_root_push(vm, resolve_method);

    AjsValue iterable = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (ajs_is_nullish(iterable)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Promise combinator argument is not iterable");
        AjsValue out = reject_cap_with_current_exc(vm, &cap);
        ajs_temp_root_restore(vm, rs);
        return out;
    }
    AjsValue it_fn = ajs_obj_get(vm, iterable, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), iterable);
    if (vm->has_exception || !ajs_is_callable(vm, it_fn)) {
        if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Argument is not iterable");
        AjsValue out = reject_cap_with_current_exc(vm, &cap);
        ajs_temp_root_restore(vm, rs);
        return out;
    }
    AjsValue iter = ajs_call_internal(vm, it_fn, iterable, 0, NULL);
    if (vm->has_exception || !ajs_is_object(vm, iter)) {
        if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator must be an object");
        AjsValue out = reject_cap_with_current_exc(vm, &cap);
        ajs_temp_root_restore(vm, rs);
        return out;
    }
    ajs_temp_root_push(vm, iter);
    AjsValue next_fn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_next), iter);
    if (vm->has_exception) {
        AjsValue out = reject_cap_with_current_exc(vm, &cap);
        ajs_temp_root_restore(vm, rs);
        return out;
    }
    ajs_temp_root_push(vm, next_fn);

    AjsEnv *senv = NULL;
    if (comb_kind != 0) {
        senv = ajs_env_new(vm, NULL, 4, NULL);
        ajs_temp_root_push(vm, ajs_val_from_gc(senv));
        senv->slots[0] = ajs_new_array(vm, 0);
        senv->slots[1] = ajs_val_number(1.0); /* Initial remainingElementsCount = 1 */
        senv->slots[2] = cap.resolve;
        senv->slots[3] = cap.reject;
    }

    uint32_t index = 0;
    for (;;) {
        size_t ls = ajs_temp_root_save(vm);
        AjsValue step = ajs_call_internal(vm, next_fn, iter, 0, NULL);
        if (vm->has_exception || !ajs_is_object(vm, step)) {
            if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator result is not an object");
            AjsValue out = reject_cap_with_current_exc(vm, &cap);
            ajs_temp_root_restore(vm, rs);
            return out;
        }
        ajs_temp_root_push(vm, step);
        AjsValue dn = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_done), step);
        if (vm->has_exception) {
            AjsValue out = reject_cap_with_current_exc(vm, &cap);
            ajs_temp_root_restore(vm, rs);
            return out;
        }
        if (ajs_to_bool(vm, dn)) {
            ajs_temp_root_restore(vm, ls);
            if (comb_kind != 0) {
                double rem = ajs_val_to_number_unchecked(senv->slots[1]) - 1.0;
                senv->slots[1] = ajs_val_number(rem);
                if (rem <= 0.0) {
                    if (comb_kind == 3) {
                        AjsValue agg = make_aggregate_err(vm, senv->slots[0]);
                        if (vm->has_exception) {
                            AjsValue out = reject_cap_with_current_exc(vm, &cap);
                            ajs_temp_root_restore(vm, rs);
                            return out;
                        }
                        ajs_temp_root_push(vm, agg);
                        ajs_call_internal(vm, cap.reject, AJS_VAL_UNDEFINED, 1, &agg);
                    } else {
                        AjsValue arr = senv->slots[0];
                        ajs_call_internal(vm, cap.resolve, AJS_VAL_UNDEFINED, 1, &arr);
                    }
                    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                }
            }
            ajs_temp_root_restore(vm, rs);
            return cap.promise;
        }

        AjsValue item = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
        if (vm->has_exception) {
            AjsValue out = reject_cap_with_current_exc(vm, &cap);
            ajs_temp_root_restore(vm, rs);
            return out;
        }
        ajs_temp_root_push(vm, item);

        /* Remaining steps must close `iter` on abrupt completion! */
        AjsValue next_prom = ajs_call_internal(vm, resolve_method, this_val, 1, &item);
        if (vm->has_exception) {
            close_iter_on_exc(vm, iter);
            AjsValue out = reject_cap_with_current_exc(vm, &cap);
            ajs_temp_root_restore(vm, rs);
            return out;
        }
        ajs_temp_root_push(vm, next_prom);

        AjsValue on_f = cap.resolve, on_r = cap.reject;
        if (comb_kind != 0) {
            ajs_set_prop_index(vm, senv->slots[0], index, AJS_VAL_UNDEFINED);
            senv->slots[1] = ajs_val_number(ajs_val_to_number_unchecked(senv->slots[1]) + 1.0);

            AjsEnv *eenv = ajs_env_new(vm, NULL, 3, NULL);
            eenv->slots[0] = ajs_val_from_gc(senv);
            eenv->slots[1] = ajs_val_number((double)index);
            eenv->slots[2] = AJS_VAL_FALSE;
            ajs_temp_root_push(vm, ajs_val_from_gc(eenv));

            if (comb_kind == 1) { /* all */
                on_f = ajs_val_from_gc(prom_make_closure_fn(vm, vm->active_realm, combinator_elem_cb, "", 1, false, 0, eenv));
                ajs_temp_root_push(vm, on_f);
            } else if (comb_kind == 2) { /* allSettled */
                on_f = ajs_val_from_gc(prom_make_closure_fn(vm, vm->active_realm, combinator_elem_cb, "", 1, false, 1, eenv));
                ajs_temp_root_push(vm, on_f);
                on_r = ajs_val_from_gc(prom_make_closure_fn(vm, vm->active_realm, combinator_elem_cb, "", 1, false, 2, eenv));
                ajs_temp_root_push(vm, on_r);
            } else { /* any */
                on_r = ajs_val_from_gc(prom_make_closure_fn(vm, vm->active_realm, combinator_elem_cb, "", 1, false, 3, eenv));
                ajs_temp_root_push(vm, on_r);
            }
            index++;
        }

        AjsValue then_m = ajs_obj_get(vm, next_prom, ajs_val_from_gc(vm->str_then), next_prom);
        if (vm->has_exception) {
            close_iter_on_exc(vm, iter);
            AjsValue out = reject_cap_with_current_exc(vm, &cap);
            ajs_temp_root_restore(vm, rs);
            return out;
        }
        ajs_temp_root_push(vm, then_m);
        AjsValue targs[2] = { on_f, on_r };
        ajs_call_internal(vm, then_m, next_prom, 2, targs);
        if (vm->has_exception) {
            close_iter_on_exc(vm, iter);
            AjsValue out = reject_cap_with_current_exc(vm, &cap);
            ajs_temp_root_restore(vm, rs);
            return out;
        }
        ajs_temp_root_restore(vm, ls);
    }
}

static AjsValue js_queue_microtask(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue cb = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_is_callable(vm, cb)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "queueMicrotask requires a callable function");
    }
    ajs_enqueue_job_ex(vm, vm->active_realm, cb, 0, NULL);
    return AJS_VAL_UNDEFINED;
}

static AjsValue js_prom_species(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)vm; (void)c; (void)v; (void)u;
    return this_val;
}

void ajs_init_promise_builtins(AjsVm *vm, AjsRealm *r) {
    size_t rs = ajs_temp_root_save(vm);

    r->promise_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->promise_ctor  = ajs_val_from_gc(prom_make_closure_fn(vm, r, js_promise_ctor, "Promise", 1, true, -36, NULL));

    ajs_define_prop_str(vm, r->promise_ctor, "prototype", r->promise_proto, 0);
    ajs_define_prop_str(vm, r->promise_proto, "constructor", r->promise_ctor, AJS_METHOD_FLAGS);
    ajs_define_prop_str(vm, r->global_obj, "Promise", r->promise_ctor, AJS_METHOD_FLAGS);

    /* Promise[Symbol.species] */
    {
        AjsValue sp_fn = ajs_val_from_gc(prom_make_closure_fn(vm, r, js_prom_species, "get [Symbol.species]", 0, false, 0, NULL));
        ajs_temp_root_push(vm, sp_fn);
        AjsPropDesc d;
        memset(&d, 0, sizeof(d));
        d.flags = AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_GET | AJS_DESC_HAS_SET |
                  AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
        d.getter = sp_fn;
        d.setter = AJS_VAL_UNDEFINED;
        ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(r->promise_ctor),
                                ajs_val_from_gc(vm->well_known_syms[AJS_SYM_SPECIES]), &d, false);
    }

    /* Static methods */
    ajs_def_native_method(vm, r, r->promise_ctor, "resolve", js_promise_resolve_static, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->promise_ctor, "reject", js_promise_reject_static, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->promise_ctor, "withResolvers", js_promise_withResolvers, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->promise_ctor, "try", js_promise_try, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->promise_ctor, "race", js_promise_combinator, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->promise_ctor, "all", js_promise_combinator, 1, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->promise_ctor, "allSettled", js_promise_combinator, 1, AJS_METHOD_FLAGS, 2);
    ajs_def_native_method(vm, r, r->promise_ctor, "any", js_promise_combinator, 1, AJS_METHOD_FLAGS, 3);

    /* Prototype methods & [Symbol.toStringTag] */
    ajs_def_native_method(vm, r, r->promise_proto, "then", js_promise_then, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->promise_proto, "catch", js_promise_catch, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->promise_proto, "finally", js_promise_finally, 1, AJS_METHOD_FLAGS, 0);
    {
        AjsPropDesc td;
        memset(&td, 0, sizeof(td));
        td.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
                   AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
        td.value = ajs_val_from_gc(ajs_str_from_cstr(vm, "Promise", true));
        ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(r->promise_proto),
                                ajs_val_from_gc(vm->well_known_syms[AJS_SYM_TO_STRING_TAG]), &td, false);
    }

    if (!ajs_has_prop_str(vm, r->global_obj, "queueMicrotask")) {
        ajs_def_native_method(vm, r, r->global_obj, "queueMicrotask", js_queue_microtask, 1, AJS_METHOD_FLAGS, 0);
    }

    ajs_temp_root_restore(vm, rs);
}
