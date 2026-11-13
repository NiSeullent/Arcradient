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

/* ============================================================================
 * 7. ES2026 Explicit Resource Management (DisposableStack & AsyncDisposableStack)
 * ============================================================================ */

#define AJS_DISP_STACK_TAG       0x4453544Bu /* 'DSTK' */
#define AJS_ASYNC_DISP_STACK_TAG 0x4144534Bu /* 'ADSK' */

typedef struct AjsDispRecord {
    AjsValue val;
    AjsValue method;
    bool     is_async;
    bool     is_adopt;
    bool     sync_fallback;
} AjsDispRecord;

typedef struct AjsDispStackData {
    bool           disposed;
    bool           has_err;
    AjsValue       err;
    uint32_t       count;
    uint32_t       cap;
    AjsDispRecord *entries;
} AjsDispStackData;

static void disp_stack_gc_mark(AjsVm *vm, void *opaque, void (*mark_val)(AjsVm *, AjsValue)) {
    AjsDispStackData *d = (AjsDispStackData *)opaque;
    if (!d) return;
    if (d->has_err) mark_val(vm, d->err);
    for (uint32_t i = 0; i < d->count; i++) {
        mark_val(vm, d->entries[i].val);
        mark_val(vm, d->entries[i].method);
    }
}

static void disp_stack_finalizer(AjsVm *vm, void *opaque) {
    (void)vm;
    AjsDispStackData *d = (AjsDispStackData *)opaque;
    if (!d) return;
    free(d->entries);
    free(d);
}

static bool disp_stack_push_rec(AjsVm *vm, AjsDispStackData *d, AjsDispRecord rec) {
    if (d->count >= d->cap) {
        uint32_t ncap = d->cap ? d->cap * 2u : 4u;
        AjsDispRecord *ne = (AjsDispRecord *)realloc(d->entries, (size_t)ncap * sizeof(AjsDispRecord));
        if (!ne) {
            ajs_throw(vm, vm->oom_error);
            return false;
        }
        d->entries = ne;
        d->cap = ncap;
    }
    d->entries[d->count++] = rec;
    return true;
}

static AjsValue make_suppressed_err(AjsVm *vm, AjsValue new_err, AjsValue suppressed_err) {
    AjsRealm *r = vm->active_realm ? vm->active_realm : vm->main_realm;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, new_err);
    ajs_temp_root_push(vm, suppressed_err);
    AjsString *msg = ajs_str_from_cstr(vm, "An error was suppressed during disposal", false);
    AjsValue msg_v = msg ? ajs_val_from_gc(msg) : AJS_VAL_UNDEFINED;
    ajs_temp_root_push(vm, msg_v);
    AjsValue args[3] = { new_err, suppressed_err, msg_v };
    AjsValue ctor = r->error_ctor[AJS_ERR_KIND_SUPPRESSED];
    AjsValue se = ajs_construct_internal(vm, ctor, 3, args, ctor);
    if (vm->has_exception) {
        ajs_clear_exception(vm);
        se = new_err;
    }
    ajs_temp_root_restore(vm, rs);
    return se;
}

static AjsValue disp_stack_alloc_with_proto(AjsVm *vm, AjsRealm *realm, AjsValue proto, bool is_async) {
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, proto);
    AjsObject *obj = ajs_obj_new(vm, realm, AJS_OBJ_ORDINARY, proto);
    if (!obj) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsDispStackData *d = (AjsDispStackData *)calloc(1, sizeof(AjsDispStackData));
    if (!d) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw(vm, vm->oom_error);
    }
    d->err = AJS_VAL_UNDEFINED;
    obj->opaque_tag = is_async ? AJS_ASYNC_DISP_STACK_TAG : AJS_DISP_STACK_TAG;
    obj->opaque_ptr = d;
    obj->gc_mark = disp_stack_gc_mark;
    obj->finalizer = disp_stack_finalizer;
    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(obj);
}

AjsValue ajs_erm_disp_stack_new_internal(AjsVm *vm, AjsRealm *realm, bool is_async) {
    AjsRealm *r = realm ? realm : (vm->active_realm ? vm->active_realm : vm->main_realm);
    AjsValue proto = is_async ? r->async_disp_stack_proto : r->disp_stack_proto;
    return disp_stack_alloc_with_proto(vm, r, proto, is_async);
}

bool ajs_erm_add_disposable(AjsVm *vm, AjsObject *stack_obj, AjsValue val, bool is_async) {
    if (!stack_obj || !stack_obj->opaque_ptr) return false;
    AjsDispStackData *d = (AjsDispStackData *)stack_obj->opaque_ptr;
    if (ajs_is_nullish(val)) return true;
    if (!ajs_is_object(vm, val)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Disposable resource is not an object");
        return false;
    }

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(stack_obj));
    ajs_temp_root_push(vm, val);

    AjsDispRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.val = val;
    rec.is_async = is_async;
    rec.is_adopt = false;
    rec.sync_fallback = false;

    if (is_async) {
        AjsValue m = ajs_obj_get(vm, val, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ASYNC_DISPOSE]), val);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
        if (!ajs_is_nullish(m)) {
            if (!ajs_is_callable(vm, m)) {
                ajs_temp_root_restore(vm, rs);
                ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "[Symbol.asyncDispose] is not a function");
                return false;
            }
            rec.method = m;
        } else {
            AjsValue sm = ajs_obj_get(vm, val, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_DISPOSE]), val);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
            if (ajs_is_nullish(sm) || !ajs_is_callable(vm, sm)) {
                ajs_temp_root_restore(vm, rs);
                ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Object is not async-disposable or disposable");
                return false;
            }
            rec.method = sm;
            rec.sync_fallback = true;
        }
    } else {
        AjsValue sm = ajs_obj_get(vm, val, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_DISPOSE]), val);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
        if (ajs_is_nullish(sm) || !ajs_is_callable(vm, sm)) {
            ajs_temp_root_restore(vm, rs);
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "[Symbol.dispose] is not a function");
            return false;
        }
        rec.method = sm;
    }

    ajs_temp_root_push(vm, rec.method);
    bool ok = disp_stack_push_rec(vm, d, rec);
    ajs_temp_root_restore(vm, rs);
    return ok;
}

bool ajs_erm_dispose_step(AjsVm *vm, AjsObject *stack_obj, AjsValue *out_await_val, uint8_t *out_mode) {
    *out_await_val = AJS_VAL_UNDEFINED;
    *out_mode = 0;
    if (!stack_obj || !stack_obj->opaque_ptr) return true;
    AjsDispStackData *d = (AjsDispStackData *)stack_obj->opaque_ptr;
    d->disposed = true;
    if (d->count == 0) {
        free(d->entries);
        d->entries = NULL;
        d->cap = 0;
        return true;
    }

    AjsDispRecord rec = d->entries[--d->count];
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(stack_obj));
    ajs_temp_root_push(vm, rec.val);
    ajs_temp_root_push(vm, rec.method);

    AjsValue res = rec.is_adopt
                   ? ajs_call_internal(vm, rec.method, AJS_VAL_UNDEFINED, 1, &rec.val)
                   : ajs_call_internal(vm, rec.method, rec.val, 0, NULL);
    ajs_temp_root_restore(vm, rs);
    if (vm->has_exception) return false;

    if (rec.is_async) {
        *out_await_val = rec.sync_fallback ? AJS_VAL_UNDEFINED : res;
        *out_mode = 1u;
    } else {
        *out_await_val = AJS_VAL_UNDEFINED;
        *out_mode = 2u;
    }
    return true;
}

void ajs_erm_dispose_record_error(AjsVm *vm, AjsObject *stack_obj, AjsValue err) {
    if (!stack_obj || !stack_obj->opaque_ptr) return;
    AjsDispStackData *d = (AjsDispStackData *)stack_obj->opaque_ptr;
    if (!d->has_err) {
        d->has_err = true;
        d->err = err;
    } else {
        size_t rs = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, ajs_val_from_gc(stack_obj));
        d->err = make_suppressed_err(vm, err, d->err);
        ajs_temp_root_restore(vm, rs);
    }
}

bool ajs_erm_dispose_finish(AjsVm *vm, AjsObject *stack_obj, bool has_pending_throw, AjsValue *io_pending_err) {
    if (!stack_obj || !stack_obj->opaque_ptr) return !has_pending_throw;
    AjsDispStackData *d = (AjsDispStackData *)stack_obj->opaque_ptr;
    d->disposed = true;
    free(d->entries);
    d->entries = NULL;
    d->count = 0;
    d->cap = 0;
    if (d->has_err) {
        AjsValue ferr = d->err;
        d->has_err = false;
        d->err = AJS_VAL_UNDEFINED;
        if (has_pending_throw) {
            *io_pending_err = make_suppressed_err(vm, ferr, *io_pending_err);
        } else {
            *io_pending_err = ferr;
        }
        return false;
    }
    return !has_pending_throw;
}

static AjsObject *require_disp_stack(AjsVm *vm, AjsValue this_val, bool is_async, const char *fn_name, AjsDispStackData **out_d) {
    uint32_t tag = is_async ? AJS_ASYNC_DISP_STACK_TAG : AJS_DISP_STACK_TAG;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "%s called on non-object", fn_name);
        return NULL;
    }
    AjsObject *obj = (AjsObject *)ajs_val_to_gc(this_val);
    if (obj->opaque_tag != tag || !obj->opaque_ptr) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "%s called on incompatible receiver", fn_name);
        return NULL;
    }
    *out_d = (AjsDispStackData *)obj->opaque_ptr;
    return obj;
}

static AjsValue js_disp_stack_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)argc; (void)argv; (void)ud;
    bool is_async = (current_fn_magic(vm) == -38);
    AjsValue nt = ajs_get_native_new_target(vm);
    if (nt == AJS_VAL_UNDEFINED) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE,
                                  is_async ? "Constructor AsyncDisposableStack requires 'new'"
                                           : "Constructor DisposableStack requires 'new'");
    }
    AjsRealm *def_r = vm->active_realm ? vm->active_realm : vm->main_realm;
    if (ajs_val_is_gc_kind(nt, AJS_GC_OBJECT) && ((AjsObject *)ajs_val_to_gc(nt))->realm) {
        def_r = ((AjsObject *)ajs_val_to_gc(nt))->realm;
    }
    AjsValue proto = is_async ? def_r->async_disp_stack_proto : def_r->disp_stack_proto;
    if (ajs_val_is_gc_kind(nt, AJS_GC_OBJECT)) {
        AjsValue p = ajs_obj_get(vm, nt, ajs_val_from_gc(vm->str_prototype), nt);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        if (ajs_val_is_gc_kind(p, AJS_GC_OBJECT)) proto = p;
    }
    return disp_stack_alloc_with_proto(vm, vm->active_realm ? vm->active_realm : def_r, proto, is_async);
}

static AjsValue js_disp_stack_get_disposed(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    bool is_async = (current_fn_magic(vm) == 1);
    AjsDispStackData *d = NULL;
    if (!require_disp_stack(vm, this_val, is_async,
                            is_async ? "get AsyncDisposableStack.prototype.disposed"
                                     : "get DisposableStack.prototype.disposed", &d)) {
        return AJS_VAL_EXCEPTION;
    }
    return ajs_bool(d->disposed);
}

static AjsValue js_disp_stack_use(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    bool is_async = (current_fn_magic(vm) == 1);
    AjsDispStackData *d = NULL;
    AjsObject *obj = require_disp_stack(vm, this_val, is_async,
                                        is_async ? "AsyncDisposableStack.prototype.use"
                                                 : "DisposableStack.prototype.use", &d);
    if (!obj) return AJS_VAL_EXCEPTION;
    if (d->disposed) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE,
                                  is_async ? "Cannot call use on an already-disposed AsyncDisposableStack"
                                           : "Cannot call use on an already-disposed DisposableStack");
    }
    AjsValue val = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_erm_add_disposable(vm, obj, val, is_async)) return AJS_VAL_EXCEPTION;
    return val;
}

static AjsValue js_disp_stack_adopt(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    bool is_async = (current_fn_magic(vm) == 1);
    AjsDispStackData *d = NULL;
    AjsObject *obj = require_disp_stack(vm, this_val, is_async,
                                        is_async ? "AsyncDisposableStack.prototype.adopt"
                                                 : "DisposableStack.prototype.adopt", &d);
    if (!obj) return AJS_VAL_EXCEPTION;
    if (d->disposed) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE,
                                  is_async ? "Cannot call adopt on an already-disposed AsyncDisposableStack"
                                           : "Cannot call adopt on an already-disposed DisposableStack");
    }
    AjsValue val = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue on_disp = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    if (!ajs_is_callable(vm, on_disp)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "onDispose callback is not callable");
    }
    AjsDispRecord rec = {
        .val = val,
        .method = on_disp,
        .is_async = is_async,
        .is_adopt = true,
        .sync_fallback = false
    };
    if (!disp_stack_push_rec(vm, d, rec)) return AJS_VAL_EXCEPTION;
    return val;
}

static AjsValue js_disp_stack_defer(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    bool is_async = (current_fn_magic(vm) == 1);
    AjsDispStackData *d = NULL;
    AjsObject *obj = require_disp_stack(vm, this_val, is_async,
                                        is_async ? "AsyncDisposableStack.prototype.defer"
                                                 : "DisposableStack.prototype.defer", &d);
    if (!obj) return AJS_VAL_EXCEPTION;
    if (d->disposed) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE,
                                  is_async ? "Cannot call defer on an already-disposed AsyncDisposableStack"
                                           : "Cannot call defer on an already-disposed DisposableStack");
    }
    AjsValue on_disp = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_is_callable(vm, on_disp)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "onDispose callback is not callable");
    }
    AjsDispRecord rec = {
        .val = AJS_VAL_UNDEFINED,
        .method = on_disp,
        .is_async = is_async,
        .is_adopt = false,
        .sync_fallback = false
    };
    if (!disp_stack_push_rec(vm, d, rec)) return AJS_VAL_EXCEPTION;
    return AJS_VAL_UNDEFINED;
}

static AjsValue js_disp_stack_move(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    bool is_async = (current_fn_magic(vm) == 1);
    AjsDispStackData *d = NULL;
    AjsObject *obj = require_disp_stack(vm, this_val, is_async,
                                        is_async ? "AsyncDisposableStack.prototype.move"
                                                 : "DisposableStack.prototype.move", &d);
    if (!obj) return AJS_VAL_EXCEPTION;
    if (d->disposed) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE,
                                  is_async ? "Cannot call move on an already-disposed AsyncDisposableStack"
                                           : "Cannot call move on an already-disposed DisposableStack");
    }
    AjsRealm *r = vm->active_realm ? vm->active_realm : (obj->realm ? obj->realm : vm->main_realm);
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, this_val);
    AjsValue nv = ajs_erm_disp_stack_new_internal(vm, r, is_async);
    ajs_temp_root_restore(vm, rs);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;

    AjsObject *nobj = (AjsObject *)ajs_val_to_gc(nv);
    AjsDispStackData *nd = (AjsDispStackData *)nobj->opaque_ptr;
    nd->entries = d->entries;
    nd->count = d->count;
    nd->cap = d->cap;
    d->entries = NULL;
    d->count = 0;
    d->cap = 0;
    d->disposed = true;
    return nv;
}

static AjsValue js_disp_stack_dispose(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsDispStackData *d = NULL;
    AjsObject *obj = require_disp_stack(vm, this_val, false, "DisposableStack.prototype.dispose", &d);
    if (!obj) return AJS_VAL_EXCEPTION;
    if (d->disposed) return AJS_VAL_UNDEFINED;
    d->disposed = true;

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, this_val);
    while (d->count > 0) {
        AjsValue await_val = AJS_VAL_UNDEFINED;
        uint8_t mode = 0;
        if (!ajs_erm_dispose_step(vm, obj, &await_val, &mode)) {
            AjsValue exc = vm->exception;
            ajs_temp_root_push(vm, exc);
            ajs_clear_exception(vm);
            ajs_erm_dispose_record_error(vm, obj, exc);
        }
    }
    AjsValue ferr = AJS_VAL_UNDEFINED;
    bool ok = ajs_erm_dispose_finish(vm, obj, false, &ferr);
    ajs_temp_root_restore(vm, rs);
    if (!ok) {
        return ajs_throw(vm, ferr);
    }
    return AJS_VAL_UNDEFINED;
}

static void async_disp_drive(AjsVm *vm, AjsRealm *r, AjsValue stack_val, AjsValue out_prom_val);

static AjsValue async_disp_step_reaction(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsEnv *env = current_fn_env(vm);
    if (!env || env->count < 2) return AJS_VAL_UNDEFINED;
    AjsValue stack_val = env->slots[0];
    AjsValue out_prom_val = env->slots[1];
    bool is_reject = (current_fn_magic(vm) == 1);
    if (is_reject && ajs_val_is_gc_kind(stack_val, AJS_GC_OBJECT)) {
        AjsValue reason = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
        ajs_erm_dispose_record_error(vm, (AjsObject *)ajs_val_to_gc(stack_val), reason);
    }
    AjsRealm *r = vm->active_realm ? vm->active_realm : vm->main_realm;
    async_disp_drive(vm, r, stack_val, out_prom_val);
    return AJS_VAL_UNDEFINED;
}

static void async_disp_drive(AjsVm *vm, AjsRealm *r, AjsValue stack_val, AjsValue out_prom_val) {
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, stack_val);
    ajs_temp_root_push(vm, out_prom_val);

    AjsObject *sobj = (AjsObject *)ajs_val_to_gc(stack_val);
    AjsObject *pobj = (AjsObject *)ajs_val_to_gc(out_prom_val);
    AjsDispStackData *d = (AjsDispStackData *)sobj->opaque_ptr;

    while (d && d->count > 0) {
        AjsValue await_val = AJS_VAL_UNDEFINED;
        uint8_t mode = 0;
        if (!ajs_erm_dispose_step(vm, sobj, &await_val, &mode)) {
            AjsValue exc = vm->exception;
            ajs_temp_root_push(vm, exc);
            ajs_clear_exception(vm);
            ajs_erm_dispose_record_error(vm, sobj, exc);
            continue;
        }
        ajs_temp_root_push(vm, await_val);
        AjsValue prom = ajs_promise_resolve_val(vm, r, r->promise_ctor, await_val);
        if (vm->has_exception || !ajs_val_is_gc_kind(prom, AJS_GC_OBJECT)) {
            AjsValue exc = vm->exception;
            ajs_temp_root_push(vm, exc);
            ajs_clear_exception(vm);
            ajs_erm_dispose_record_error(vm, sobj, exc);
            continue;
        }
        ajs_temp_root_push(vm, prom);
        AjsEnv *cenv = ajs_env_new(vm, NULL, 2, NULL);
        if (!cenv) {
            ajs_clear_exception(vm);
            reject_promise_internal(vm, pobj, vm->oom_error);
            ajs_temp_root_restore(vm, rs);
            return;
        }
        cenv->slots[0] = stack_val;
        cenv->slots[1] = out_prom_val;
        ajs_temp_root_push(vm, ajs_val_from_gc(cenv));
        AjsObject *on_f = prom_make_closure_fn(vm, r, async_disp_step_reaction, "", 1, false, 0, cenv);
        ajs_temp_root_push(vm, ajs_val_from_gc(on_f));
        AjsObject *on_r = prom_make_closure_fn(vm, r, async_disp_step_reaction, "", 1, false, 1, cenv);
        ajs_temp_root_push(vm, ajs_val_from_gc(on_r));
        ajs_promise_then_internal(vm, (AjsObject *)ajs_val_to_gc(prom),
                                  ajs_val_from_gc(on_f), ajs_val_from_gc(on_r),
                                  AJS_VAL_UNDEFINED, AJS_VAL_UNDEFINED, AJS_VAL_UNDEFINED);
        ajs_temp_root_restore(vm, rs);
        return;
    }

    AjsValue ferr = AJS_VAL_UNDEFINED;
    bool ok = ajs_erm_dispose_finish(vm, sobj, false, &ferr);
    if (!ok) {
        reject_promise_internal(vm, pobj, ferr);
    } else {
        fulfill_promise_internal(vm, pobj, AJS_VAL_UNDEFINED);
    }
    ajs_temp_root_restore(vm, rs);
}

static AjsValue js_async_disp_stack_disposeAsync(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsRealm *r = vm->active_realm ? vm->active_realm : vm->main_realm;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, this_val);

    AjsObject *pobj = ajs_obj_new(vm, r, AJS_OBJ_PROMISE, r->promise_proto);
    if (!pobj) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    pobj->u.promise.state = AJS_PROMISE_PENDING;
    pobj->u.promise.result = AJS_VAL_UNDEFINED;
    AjsValue pval = ajs_val_from_gc(pobj);
    ajs_temp_root_push(vm, pval);

    AjsDispStackData *d = NULL;
    if (!require_disp_stack(vm, this_val, true, "AsyncDisposableStack.prototype.disposeAsync", &d)) {
        AjsValue exc = vm->exception;
        ajs_temp_root_push(vm, exc);
        ajs_clear_exception(vm);
        reject_promise_internal(vm, pobj, exc);
        ajs_temp_root_restore(vm, rs);
        return pval;
    }
    if (d->disposed) {
        fulfill_promise_internal(vm, pobj, AJS_VAL_UNDEFINED);
        ajs_temp_root_restore(vm, rs);
        return pval;
    }
    d->disposed = true;
    async_disp_drive(vm, r, this_val, pval);
    ajs_temp_root_restore(vm, rs);
    return pval;
}

static void erm_def_to_string_tag(AjsVm *vm, AjsValue obj_v, const char *tag) {
    AjsPropDesc td;
    memset(&td, 0, sizeof(td));
    td.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
               AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    td.value = ajs_val_from_gc(ajs_str_from_cstr(vm, tag, true));
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(obj_v),
                            ajs_val_from_gc(vm->well_known_syms[AJS_SYM_TO_STRING_TAG]), &td, false);
}

static void erm_def_disposed_getter(AjsVm *vm, AjsRealm *r, AjsValue proto_v, int32_t magic) {
    AjsObject *gfn = prom_make_closure_fn(vm, r, js_disp_stack_get_disposed, "get disposed", 0, false, magic, NULL);
    if (!gfn) return;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue gv = ajs_val_from_gc(gfn);
    ajs_temp_root_push(vm, gv);
    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_GET | AJS_DESC_HAS_SET |
              AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    d.getter = gv;
    d.setter = AJS_VAL_UNDEFINED;
    AjsString *k = ajs_str_from_cstr(vm, "disposed", true);
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(proto_v), ajs_val_from_gc(k), &d, false);
    ajs_temp_root_restore(vm, rs);
}

static AjsValue js_iter_proto_dispose(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, this_val);
    AjsValue ret_fn = ajs_obj_get(vm, this_val, ajs_val_from_gc(vm->str_return), this_val);
    if (vm->has_exception) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    if (ajs_is_nullish(ret_fn)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_UNDEFINED;
    }
    if (!ajs_is_callable(vm, ret_fn)) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator return property is not callable");
    }
    ajs_temp_root_push(vm, ret_fn);
    ajs_call_internal(vm, ret_fn, this_val, 0, NULL);
    ajs_temp_root_restore(vm, rs);
    return vm->has_exception ? AJS_VAL_EXCEPTION : AJS_VAL_UNDEFINED;
}

static AjsValue async_iter_dispose_undef_cb(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)vm; (void)this_val; (void)argc; (void)argv; (void)ud;
    return AJS_VAL_UNDEFINED;
}

static AjsValue js_async_iter_proto_asyncDispose(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsRealm *r = vm->active_realm ? vm->active_realm : vm->main_realm;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, this_val);

    AjsObject *pobj = ajs_obj_new(vm, r, AJS_OBJ_PROMISE, r->promise_proto);
    if (!pobj) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    pobj->u.promise.state = AJS_PROMISE_PENDING;
    pobj->u.promise.result = AJS_VAL_UNDEFINED;
    AjsValue pval = ajs_val_from_gc(pobj);
    ajs_temp_root_push(vm, pval);

    AjsValue ret_fn = ajs_obj_get(vm, this_val, ajs_val_from_gc(vm->str_return), this_val);
    if (vm->has_exception) {
        AjsValue exc = vm->exception;
        ajs_temp_root_push(vm, exc);
        ajs_clear_exception(vm);
        reject_promise_internal(vm, pobj, exc);
        ajs_temp_root_restore(vm, rs);
        return pval;
    }
    if (ajs_is_nullish(ret_fn)) {
        fulfill_promise_internal(vm, pobj, AJS_VAL_UNDEFINED);
        ajs_temp_root_restore(vm, rs);
        return pval;
    }
    if (!ajs_is_callable(vm, ret_fn)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "AsyncIterator return property is not callable");
        AjsValue exc = vm->exception;
        ajs_temp_root_push(vm, exc);
        ajs_clear_exception(vm);
        reject_promise_internal(vm, pobj, exc);
        ajs_temp_root_restore(vm, rs);
        return pval;
    }

    ajs_temp_root_push(vm, ret_fn);
    AjsValue res = ajs_call_internal(vm, ret_fn, this_val, 0, NULL);
    if (vm->has_exception) {
        AjsValue exc = vm->exception;
        ajs_temp_root_push(vm, exc);
        ajs_clear_exception(vm);
        reject_promise_internal(vm, pobj, exc);
        ajs_temp_root_restore(vm, rs);
        return pval;
    }
    ajs_temp_root_push(vm, res);

    AjsValue inner_p = ajs_promise_resolve_val(vm, r, r->promise_ctor, res);
    if (vm->has_exception || !ajs_val_is_gc_kind(inner_p, AJS_GC_OBJECT)) {
        AjsValue exc = vm->exception;
        ajs_temp_root_push(vm, exc);
        ajs_clear_exception(vm);
        reject_promise_internal(vm, pobj, exc);
        ajs_temp_root_restore(vm, rs);
        return pval;
    }
    ajs_temp_root_push(vm, inner_p);

    AjsValue res_fn = AJS_VAL_UNDEFINED, rej_fn = AJS_VAL_UNDEFINED;
    create_resolving_functions(vm, r, pval, &res_fn, &rej_fn);
    ajs_temp_root_push(vm, res_fn);
    ajs_temp_root_push(vm, rej_fn);

    AjsObject *on_f = prom_make_closure_fn(vm, r, async_iter_dispose_undef_cb, "", 0, false, 0, NULL);
    if (!on_f) {
        ajs_clear_exception(vm);
        reject_promise_internal(vm, pobj, vm->oom_error);
        ajs_temp_root_restore(vm, rs);
        return pval;
    }
    ajs_temp_root_push(vm, ajs_val_from_gc(on_f));

    ajs_promise_then_internal(vm, (AjsObject *)ajs_val_to_gc(inner_p),
                              ajs_val_from_gc(on_f), AJS_VAL_UNDEFINED,
                              res_fn, rej_fn, pval);
    ajs_temp_root_restore(vm, rs);
    return pval;
}

void ajs_init_erm_builtins(AjsVm *vm, AjsRealm *r) {
    size_t rs = ajs_temp_root_save(vm);

    /* 1. DisposableStack */
    r->disp_stack_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->disp_stack_ctor  = ajs_val_from_gc(prom_make_closure_fn(vm, r, js_disp_stack_ctor, "DisposableStack", 0, true, -37, NULL));
    ajs_define_prop_str(vm, r->disp_stack_ctor, "prototype", r->disp_stack_proto, 0);
    ajs_define_prop_str(vm, r->disp_stack_proto, "constructor", r->disp_stack_ctor, AJS_METHOD_FLAGS);
    ajs_define_prop_str(vm, r->global_obj, "DisposableStack", r->disp_stack_ctor, AJS_METHOD_FLAGS);

    ajs_def_native_method(vm, r, r->disp_stack_proto, "use",   js_disp_stack_use,   1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->disp_stack_proto, "adopt", js_disp_stack_adopt, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->disp_stack_proto, "defer", js_disp_stack_defer, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->disp_stack_proto, "move",  js_disp_stack_move,  0, AJS_METHOD_FLAGS, 0);
    erm_def_disposed_getter(vm, r, r->disp_stack_proto, 0);
    erm_def_to_string_tag(vm, r->disp_stack_proto, "DisposableStack");
    {
        AjsObject *dfn = prom_make_closure_fn(vm, r, js_disp_stack_dispose, "dispose", 0, false, 0, NULL);
        if (dfn) {
            AjsValue dv = ajs_val_from_gc(dfn);
            ajs_temp_root_push(vm, dv);
            ajs_define_prop_str(vm, r->disp_stack_proto, "dispose", dv, AJS_METHOD_FLAGS);
            AjsPropDesc sd;
            memset(&sd, 0, sizeof(sd));
            sd.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                       AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
            sd.value = dv;
            ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(r->disp_stack_proto),
                                    ajs_val_from_gc(vm->well_known_syms[AJS_SYM_DISPOSE]), &sd, false);
        }
    }

    /* 2. AsyncDisposableStack */
    r->async_disp_stack_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    r->async_disp_stack_ctor  = ajs_val_from_gc(prom_make_closure_fn(vm, r, js_disp_stack_ctor, "AsyncDisposableStack", 0, true, -38, NULL));
    ajs_define_prop_str(vm, r->async_disp_stack_ctor, "prototype", r->async_disp_stack_proto, 0);
    ajs_define_prop_str(vm, r->async_disp_stack_proto, "constructor", r->async_disp_stack_ctor, AJS_METHOD_FLAGS);
    ajs_define_prop_str(vm, r->global_obj, "AsyncDisposableStack", r->async_disp_stack_ctor, AJS_METHOD_FLAGS);

    ajs_def_native_method(vm, r, r->async_disp_stack_proto, "use",   js_disp_stack_use,   1, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->async_disp_stack_proto, "adopt", js_disp_stack_adopt, 2, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->async_disp_stack_proto, "defer", js_disp_stack_defer, 1, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->async_disp_stack_proto, "move",  js_disp_stack_move,  0, AJS_METHOD_FLAGS, 1);
    erm_def_disposed_getter(vm, r, r->async_disp_stack_proto, 1);
    erm_def_to_string_tag(vm, r->async_disp_stack_proto, "AsyncDisposableStack");
    {
        AjsObject *adfn = prom_make_closure_fn(vm, r, js_async_disp_stack_disposeAsync, "disposeAsync", 0, false, 1, NULL);
        if (adfn) {
            AjsValue adv = ajs_val_from_gc(adfn);
            ajs_temp_root_push(vm, adv);
            ajs_define_prop_str(vm, r->async_disp_stack_proto, "disposeAsync", adv, AJS_METHOD_FLAGS);
            AjsPropDesc sd;
            memset(&sd, 0, sizeof(sd));
            sd.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                       AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
            sd.value = adv;
            ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(r->async_disp_stack_proto),
                                    ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ASYNC_DISPOSE]), &sd, false);
        }
    }

    /* 3. %IteratorPrototype%[Symbol.dispose] & %AsyncIteratorPrototype%[Symbol.asyncDispose] */
    if (ajs_val_is_gc_kind(r->iterator_proto, AJS_GC_OBJECT)) {
        ajs_def_native_sym_method(vm, r, r->iterator_proto, vm->well_known_syms[AJS_SYM_DISPOSE],
                                  "[Symbol.dispose]", js_iter_proto_dispose, 0, AJS_METHOD_FLAGS);
    }
    if (ajs_val_is_gc_kind(r->async_iterator_proto, AJS_GC_OBJECT)) {
        ajs_def_native_sym_method(vm, r, r->async_iterator_proto, vm->well_known_syms[AJS_SYM_ASYNC_DISPOSE],
                                  "[Symbol.asyncDispose]", js_async_iter_proto_asyncDispose, 0, AJS_METHOD_FLAGS);
    }

    ajs_temp_root_restore(vm, rs);
}

