/*
 * ArcradientJS 1.0 (Clean-Room Pure C11 ECMAScript 2026 Virtual Machine)
 * js/vm/ajs_api.c - Public C11 Embedding API, Realm Lifecycle, HandleScopes & Diagnostics
 */
#include "ajs_internal.h"

static const char *const k_err_kind_names[AJS_ERR_KIND_COUNT] = {
    "Error",
    "EvalError",
    "RangeError",
    "ReferenceError",
    "SyntaxError",
    "TypeError",
    "URIError",
    "AggregateError",
    "SuppressedError"
};

static const char *const k_well_known_sym_descs[AJS_SYM_COUNT] = {
    "Symbol.iterator",
    "Symbol.asyncIterator",
    "Symbol.hasInstance",
    "Symbol.isConcatSpreadable",
    "Symbol.match",
    "Symbol.matchAll",
    "Symbol.replace",
    "Symbol.search",
    "Symbol.species",
    "Symbol.split",
    "Symbol.toPrimitive",
    "Symbol.toStringTag",
    "Symbol.unscopables",
    "Symbol.dispose",
    "Symbol.asyncDispose"
};

static AjsErrKind err_kind_from_name(const char *name) {
    if (!name) return AJS_ERR_KIND_ERROR;
    for (int i = 0; i < AJS_ERR_KIND_COUNT; i++) {
        if (strcmp(name, k_err_kind_names[i]) == 0) return (AjsErrKind)i;
    }
    return AJS_ERR_KIND_ERROR;
}

static void find_bc_line_col(const AjsBytecode *bc, uint32_t pc_off, uint32_t *out_line, uint32_t *out_col) {
    *out_line = 1;
    *out_col = 1;
    if (!bc || bc->line_count == 0 || !bc->lines) return;
    uint32_t lo = 0, hi = bc->line_count;
    while (lo + 1u < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (bc->lines[mid].bc_offset <= pc_off) lo = mid;
        else hi = mid;
    }
    *out_line = bc->lines[lo].line ? bc->lines[lo].line : 1u;
    *out_col = bc->lines[lo].col ? bc->lines[lo].col : 1u;
}

void ajs_capture_stack_trace(AjsVm *vm, AjsObject *err_obj, const char *override_url,
                             uint32_t override_line, uint32_t override_col) {
    if (!vm || !err_obj || err_obj->kind != AJS_OBJ_ERROR) return;

    char buf[2048];
    size_t pos = 0;
    buf[0] = '\0';

    const char *ename = err_obj->u.err.error_name[0] ? err_obj->u.err.error_name : "Error";
    AjsPropDesc mdesc;
    bool has_msg = false;
    const char *emsg = "";
    if (vm->str_message && ajs_obj_get_own_desc(vm, err_obj, ajs_val_from_gc(vm->str_message), &mdesc, &has_msg) &&
        has_msg && !(mdesc.flags & AJS_DESC_IS_ACCESSOR) && ajs_val_is_gc_kind(mdesc.value, AJS_GC_STRING)) {
        emsg = ((AjsString *)ajs_val_to_gc(mdesc.value))->utf8;
    }

    if (emsg && emsg[0]) {
        int n = snprintf(buf + pos, sizeof(buf) - pos, "%s: %s", ename, emsg);
        if (n > 0) pos += (size_t)n < (sizeof(buf) - pos) ? (size_t)n : (sizeof(buf) - pos - 1u);
    } else {
        int n = snprintf(buf + pos, sizeof(buf) - pos, "%s", ename);
        if (n > 0) pos += (size_t)n < (sizeof(buf) - pos) ? (size_t)n : (sizeof(buf) - pos - 1u);
    }

    bool captured_top_loc = false;
    if (override_url && override_url[0]) {
        snprintf(err_obj->u.err.url, sizeof(err_obj->u.err.url), "%s", override_url);
        err_obj->u.err.line = override_line ? override_line : 1u;
        err_obj->u.err.col = override_col ? override_col : 1u;
        captured_top_loc = true;
        int n = snprintf(buf + pos, sizeof(buf) - pos, "\n    at %s:%u:%u",
                         err_obj->u.err.url, err_obj->u.err.line, err_obj->u.err.col);
        if (n > 0) pos += (size_t)n < (sizeof(buf) - pos) ? (size_t)n : (sizeof(buf) - pos - 1u);
    }

    for (size_t idx = vm->call_depth; idx > 0 && pos + 64u < sizeof(buf); idx--) {
        const AjsCallStackEntry *ce = &vm->call_stack[idx - 1u];
        if (ce->frame && ce->frame->bc) {
            const AjsBytecode *bc = ce->frame->bc;
            uint32_t pc_off = ce->frame->pc > 0 ? ce->frame->pc - 1u : 0u;
            uint32_t ln = 1, col = 1;
            find_bc_line_col(bc, pc_off, &ln, &col);
            const char *url = (bc->source_url && bc->source_url->utf8[0]) ? bc->source_url->utf8 : "<anonymous>";
            const char *fn_name = (bc->name && bc->name->utf8[0]) ? bc->name->utf8 : "<anonymous>";
            if (!captured_top_loc) {
                snprintf(err_obj->u.err.url, sizeof(err_obj->u.err.url), "%s", url);
                err_obj->u.err.line = ln;
                err_obj->u.err.col = col;
                captured_top_loc = true;
            }
            int n = snprintf(buf + pos, sizeof(buf) - pos, "\n    at %s (%s:%u:%u)", fn_name, url, ln, col);
            if (n > 0) pos += (size_t)n < (sizeof(buf) - pos) ? (size_t)n : (sizeof(buf) - pos - 1u);
        } else if (ce->native_fn_obj) {
            const char *fn_name = "<native>";
            AjsPropDesc nd;
            bool nf = false;
            if (vm->str_name && ajs_obj_get_own_desc(vm, ce->native_fn_obj, ajs_val_from_gc(vm->str_name), &nd, &nf) &&
                nf && !(nd.flags & AJS_DESC_IS_ACCESSOR) && ajs_val_is_gc_kind(nd.value, AJS_GC_STRING)) {
                const char *s = ((AjsString *)ajs_val_to_gc(nd.value))->utf8;
                if (s[0]) fn_name = s;
            }
            int n = snprintf(buf + pos, sizeof(buf) - pos, "\n    at %s (native)", fn_name);
            if (n > 0) pos += (size_t)n < (sizeof(buf) - pos) ? (size_t)n : (sizeof(buf) - pos - 1u);
        }
    }

    free(err_obj->u.err.stack_str);
    size_t slen = strlen(buf);
    err_obj->u.err.stack_str = (char *)malloc(slen + 1u);
    if (err_obj->u.err.stack_str) {
        memcpy(err_obj->u.err.stack_str, buf, slen + 1u);
    }

    if (vm->str_stack) {
        AjsString *s_stack = ajs_str_from_utf8(vm, buf, slen, false);
        if (s_stack) {
            size_t saved = ajs_temp_root_save(vm);
            ajs_temp_root_push(vm, ajs_val_from_gc(s_stack));
            AjsPropDesc sd = {
                .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                         AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
                .value = ajs_val_from_gc(s_stack)
            };
            ajs_obj_define_own_prop(vm, err_obj, ajs_val_from_gc(vm->str_stack), &sd, false);
            ajs_temp_root_restore(vm, saved);
        }
    }
}

void ajs_fill_error_info(AjsVm *vm, AjsValue exc, AjsErrorPhase phase, AjsErrorInfo *out_info) {
    if (!out_info) return;
    memset(out_info, 0, sizeof(*out_info));
    out_info->phase = phase;

    if (!vm) return;

    /* Preserve saved exceptions while extracting properties */
    bool saved_has = vm->has_exception;
    AjsErrorPhase saved_phase = vm->exc_phase;
    AjsValue saved_exc = vm->exception;
    vm->has_exception = false;
    vm->exception = AJS_VAL_UNDEFINED;

    size_t tsave = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, saved_exc);
    ajs_temp_root_push(vm, exc);

    if (ajs_val_is_gc_kind(exc, AJS_GC_OBJECT)) {
        AjsObject *obj = (AjsObject *)ajs_val_to_gc(exc);
        if (obj->kind == AJS_OBJ_ERROR) {
            snprintf(out_info->name, sizeof(out_info->name), "%s",
                     obj->u.err.error_name[0] ? obj->u.err.error_name : "Error");
            snprintf(out_info->url, sizeof(out_info->url), "%s", obj->u.err.url);
            out_info->line = obj->u.err.line;
            out_info->column = obj->u.err.col;
            if (obj->u.err.stack_str) {
                snprintf(out_info->stack, sizeof(out_info->stack), "%s", obj->u.err.stack_str);
            }
        }
        /* Read .name and .message via ordinary property lookup */
        if (vm->str_name) {
            AjsValue nv = ajs_obj_get(vm, exc, ajs_val_from_gc(vm->str_name), exc);
            if (!vm->has_exception && nv != AJS_VAL_UNDEFINED) {
                AjsString *ns = ajs_to_js_string(vm, nv);
                if (!vm->has_exception && ns && ns->utf8[0]) {
                    snprintf(out_info->name, sizeof(out_info->name), "%s", ns->utf8);
                }
            }
            vm->has_exception = false;
            vm->exception = AJS_VAL_UNDEFINED;
        }
        if (vm->str_message) {
            AjsValue mv = ajs_obj_get(vm, exc, ajs_val_from_gc(vm->str_message), exc);
            if (!vm->has_exception && mv != AJS_VAL_UNDEFINED) {
                AjsString *ms = ajs_to_js_string(vm, mv);
                if (!vm->has_exception && ms) {
                    snprintf(out_info->message, sizeof(out_info->message), "%s", ms->utf8);
                }
            }
            vm->has_exception = false;
            vm->exception = AJS_VAL_UNDEFINED;
        }
        if (!out_info->stack[0] && vm->str_stack) {
            AjsValue sv = ajs_obj_get(vm, exc, ajs_val_from_gc(vm->str_stack), exc);
            if (!vm->has_exception && ajs_val_is_gc_kind(sv, AJS_GC_STRING)) {
                snprintf(out_info->stack, sizeof(out_info->stack), "%s",
                         ((AjsString *)ajs_val_to_gc(sv))->utf8);
            }
            vm->has_exception = false;
            vm->exception = AJS_VAL_UNDEFINED;
        }
        /* Also fall back to ctor.name if out_info->name is still empty */
        if (!out_info->name[0] && vm->str_constructor && vm->str_name) {
            AjsValue cv = ajs_obj_get(vm, exc, ajs_val_from_gc(vm->str_constructor), exc);
            if (!vm->has_exception && ajs_val_is_gc_kind(cv, AJS_GC_OBJECT)) {
                AjsValue cnv = ajs_obj_get(vm, cv, ajs_val_from_gc(vm->str_name), cv);
                if (!vm->has_exception && ajs_val_is_gc_kind(cnv, AJS_GC_STRING)) {
                    const char *cn = ((AjsString *)ajs_val_to_gc(cnv))->utf8;
                    if (cn[0]) snprintf(out_info->name, sizeof(out_info->name), "%s", cn);
                }
            }
            vm->has_exception = false;
            vm->exception = AJS_VAL_UNDEFINED;
        }
        if (!out_info->name[0]) {
            snprintf(out_info->name, sizeof(out_info->name), "Error");
        }
    } else {
        snprintf(out_info->name, sizeof(out_info->name), "ThrownValue");
        if (ajs_val_is_gc_kind(exc, AJS_GC_SYMBOL)) {
            AjsSymbol *sym = (AjsSymbol *)ajs_val_to_gc(exc);
            snprintf(out_info->message, sizeof(out_info->message), "Symbol(%s)",
                     (sym->desc && sym->desc->utf8) ? sym->desc->utf8 : "");
        } else {
            AjsString *s = ajs_to_js_string(vm, exc);
            if (!vm->has_exception && s) {
                snprintf(out_info->message, sizeof(out_info->message), "%s", s->utf8);
            }
            vm->has_exception = false;
            vm->exception = AJS_VAL_UNDEFINED;
        }
    }

    ajs_temp_root_restore(vm, tsave);
    vm->has_exception = saved_has;
    vm->exc_phase = saved_phase;
    vm->exception = saved_exc;
}

static AjsValue throw_err_vfmt(AjsVm *vm, AjsErrKind kind, const char *fmt, va_list ap) {
    if (!vm) return AJS_VAL_EXCEPTION;
    char msg[512];
    msg[0] = '\0';
    if (fmt) {
        vsnprintf(msg, sizeof(msg), fmt, ap);
    }

    /* Suppress GC while constructing error object so OOM cannot recursively throw */
    vm->gc_suppress++;
    AjsRealm *realm = vm->active_realm ? vm->active_realm : vm->main_realm;
    AjsValue proto = (realm && kind < AJS_ERR_KIND_COUNT && realm->error_proto[kind] != AJS_VAL_UNDEFINED)
                     ? realm->error_proto[kind]
                     : (realm ? realm->object_proto : AJS_VAL_NULL);

    AjsObject *eobj = ajs_obj_new(vm, realm, AJS_OBJ_ERROR, proto);
    if (!eobj) {
        vm->gc_suppress--;
        return ajs_throw(vm, vm->oom_error);
    }
    size_t tsave = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(eobj));

    const char *ename = (kind < AJS_ERR_KIND_COUNT) ? k_err_kind_names[kind] : "Error";
    snprintf(eobj->u.err.error_name, sizeof(eobj->u.err.error_name), "%s", ename);

    AjsString *s_msg = ajs_str_from_cstr(vm, msg, false);
    if (s_msg && vm->str_message) {
        ajs_temp_root_push(vm, ajs_val_from_gc(s_msg));
        AjsPropDesc md = {
            .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                     AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
            .value = ajs_val_from_gc(s_msg)
        };
        ajs_obj_define_own_prop(vm, eobj, ajs_val_from_gc(vm->str_message), &md, false);
    }

    ajs_capture_stack_trace(vm, eobj, NULL, 0, 0);
    vm->gc_suppress--;
    ajs_temp_root_restore(vm, tsave);

    return ajs_throw(vm, ajs_val_from_gc(eobj));
}

AjsValue ajs_throw_err_kind(AjsVm *vm, AjsErrKind kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    AjsValue r = throw_err_vfmt(vm, kind, fmt, ap);
    va_end(ap);
    return r;
}

/* ============================================================================
 * 1. VM & Realm Lifecycle
 * ============================================================================ */

void ajs_default_config(AjsVmConfig *out_cfg) {
    if (!out_cfg) return;
    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->heap_limit_bytes = AJS_DEFAULT_HEAP_LIMIT_BYTES;
    out_cfg->max_call_depth = 512u;
}

AjsRealm *ajs_realm_create(AjsVm *vm) {
    if (!vm) return NULL;
    AjsRealm *r = (AjsRealm *)calloc(1, sizeof(AjsRealm));
    if (!r) return NULL;
    r->vm = vm;
    r->global_obj = AJS_VAL_UNDEFINED;
    r->object_proto = AJS_VAL_UNDEFINED;
    r->object_ctor = AJS_VAL_UNDEFINED;
    r->function_proto = AJS_VAL_UNDEFINED;
    r->function_ctor = AJS_VAL_UNDEFINED;
    r->array_proto = AJS_VAL_UNDEFINED;
    r->array_ctor = AJS_VAL_UNDEFINED;
    r->array_iter_proto = AJS_VAL_UNDEFINED;
    r->string_proto = AJS_VAL_UNDEFINED;
    r->string_ctor = AJS_VAL_UNDEFINED;
    r->string_iter_proto = AJS_VAL_UNDEFINED;
    r->boolean_proto = AJS_VAL_UNDEFINED;
    r->boolean_ctor = AJS_VAL_UNDEFINED;
    r->number_proto = AJS_VAL_UNDEFINED;
    r->number_ctor = AJS_VAL_UNDEFINED;
    r->bigint_proto = AJS_VAL_UNDEFINED;
    r->bigint_ctor = AJS_VAL_UNDEFINED;
    r->symbol_proto = AJS_VAL_UNDEFINED;
    r->symbol_ctor = AJS_VAL_UNDEFINED;
    r->regexp_proto = AJS_VAL_UNDEFINED;
    r->regexp_ctor = AJS_VAL_UNDEFINED;
    r->regexp_iter_proto = AJS_VAL_UNDEFINED;
    r->date_proto = AJS_VAL_UNDEFINED;
    r->date_ctor = AJS_VAL_UNDEFINED;
    r->promise_proto = AJS_VAL_UNDEFINED;
    r->promise_ctor = AJS_VAL_UNDEFINED;
    r->map_proto = AJS_VAL_UNDEFINED;
    r->map_ctor = AJS_VAL_UNDEFINED;
    r->map_iter_proto = AJS_VAL_UNDEFINED;
    r->set_proto = AJS_VAL_UNDEFINED;
    r->set_ctor = AJS_VAL_UNDEFINED;
    r->set_iter_proto = AJS_VAL_UNDEFINED;
    r->weakmap_proto = AJS_VAL_UNDEFINED;
    r->weakmap_ctor = AJS_VAL_UNDEFINED;
    r->weakset_proto = AJS_VAL_UNDEFINED;
    r->weakset_ctor = AJS_VAL_UNDEFINED;
    r->weakref_proto = AJS_VAL_UNDEFINED;
    r->weakref_ctor = AJS_VAL_UNDEFINED;
    r->finreg_proto = AJS_VAL_UNDEFINED;
    r->finreg_ctor = AJS_VAL_UNDEFINED;
    r->iterator_proto = AJS_VAL_UNDEFINED;
    r->iterator_ctor = AJS_VAL_UNDEFINED;
    r->iter_helper_proto = AJS_VAL_UNDEFINED;
    r->async_iterator_proto = AJS_VAL_UNDEFINED;
    r->gen_func_proto = AJS_VAL_UNDEFINED;
    r->gen_func_ctor = AJS_VAL_UNDEFINED;
    r->gen_proto = AJS_VAL_UNDEFINED;
    r->async_func_proto = AJS_VAL_UNDEFINED;
    r->async_func_ctor = AJS_VAL_UNDEFINED;
    r->async_gen_func_proto = AJS_VAL_UNDEFINED;
    r->async_gen_func_ctor = AJS_VAL_UNDEFINED;
    r->async_gen_proto = AJS_VAL_UNDEFINED;
    for (int i = 0; i < AJS_ERR_KIND_COUNT; i++) {
        r->error_proto[i] = AJS_VAL_UNDEFINED;
        r->error_ctor[i] = AJS_VAL_UNDEFINED;
    }
    r->ab_proto = AJS_VAL_UNDEFINED;
    r->ab_ctor = AJS_VAL_UNDEFINED;
    r->sab_proto = AJS_VAL_UNDEFINED;
    r->sab_ctor = AJS_VAL_UNDEFINED;
    r->dv_proto = AJS_VAL_UNDEFINED;
    r->dv_ctor = AJS_VAL_UNDEFINED;
    r->ta_proto = AJS_VAL_UNDEFINED;
    r->ta_ctor = AJS_VAL_UNDEFINED;
    for (int i = 0; i < 12; i++) {
        r->ta_sub_proto[i] = AJS_VAL_UNDEFINED;
        r->ta_sub_ctor[i] = AJS_VAL_UNDEFINED;
    }
    r->intl_locale_proto = AJS_VAL_UNDEFINED;
    r->intl_collator_proto = AJS_VAL_UNDEFINED;
    r->intl_numfmt_proto = AJS_VAL_UNDEFINED;
    r->intl_dtfmt_proto = AJS_VAL_UNDEFINED;
    r->intl_plural_proto = AJS_VAL_UNDEFINED;
    r->intl_reltime_proto = AJS_VAL_UNDEFINED;
    r->intl_segmenter_proto = AJS_VAL_UNDEFINED;
    r->intl_segments_proto = AJS_VAL_UNDEFINED;
    r->intl_dispnames_proto = AJS_VAL_UNDEFINED;
    r->intl_listfmt_proto = AJS_VAL_UNDEFINED;
    r->disp_stack_proto = AJS_VAL_UNDEFINED;
    r->disp_stack_ctor = AJS_VAL_UNDEFINED;
    r->async_disp_stack_proto = AJS_VAL_UNDEFINED;
    r->async_disp_stack_ctor = AJS_VAL_UNDEFINED;
    r->intrinsic_eval = AJS_VAL_UNDEFINED;
    r->throw_type_error_fn = AJS_VAL_UNDEFINED;
    r->array_proto_values_fn = AJS_VAL_UNDEFINED;

    /* Link into vm->realms immediately so GC marks everything allocated during init */
    r->next = vm->realms;
    vm->realms = r;

    AjsRealm *prev_active = vm->active_realm;
    vm->active_realm = r;
    vm->gc_suppress++;

    r->global_lex_env = ajs_env_new(vm, NULL, 0, NULL);
    ajs_realm_init_intrinsics(vm, r);
    ajs_init_string_array_builtins(vm, r);
    ajs_init_collections_builtins(vm, r);
    ajs_init_regexp_builtins(vm, r);
    ajs_init_json_builtins(vm, r);
    ajs_init_promise_builtins(vm, r);
    ajs_init_typedarray_builtins(vm, r);
    ajs_init_intl_builtins(vm, r);
    ajs_init_erm_builtins(vm, r);

    vm->gc_suppress--;
    vm->active_realm = prev_active ? prev_active : r;
    return r;
}

AjsVm *ajs_vm_create(const AjsVmConfig *cfg) {
    AjsVm *vm = (AjsVm *)calloc(1, sizeof(AjsVm));
    if (!vm) return NULL;

    if (cfg) {
        vm->cfg = *cfg;
    } else {
        ajs_default_config(&vm->cfg);
    }
    if (vm->cfg.heap_limit_bytes == 0) vm->cfg.heap_limit_bytes = AJS_DEFAULT_HEAP_LIMIT_BYTES;
    if (vm->cfg.max_call_depth == 0) vm->cfg.max_call_depth = 512u;

    vm->gc_threshold = 512u * 1024u;
    if (vm->gc_threshold > vm->cfg.heap_limit_bytes) {
        vm->gc_threshold = vm->cfg.heap_limit_bytes;
    }
    vm->oom_error = AJS_VAL_UNDEFINED;
    vm->exception = AJS_VAL_UNDEFINED;
    vm->next_symbol_id = 1u;
    vm->rng_state[0] = UINT64_C(0x9E3779B97F4A7C15);
    vm->rng_state[1] = UINT64_C(0xBF58476D1CE4E5B9);

    vm->gc_suppress++;

    /* Intern core cached strings */
    vm->str_empty       = ajs_str_from_cstr(vm, "", true);
    vm->str_length      = ajs_str_from_cstr(vm, "length", true);
    vm->str_prototype   = ajs_str_from_cstr(vm, "prototype", true);
    vm->str_constructor = ajs_str_from_cstr(vm, "constructor", true);
    vm->str_name        = ajs_str_from_cstr(vm, "name", true);
    vm->str_message     = ajs_str_from_cstr(vm, "message", true);
    vm->str_stack       = ajs_str_from_cstr(vm, "stack", true);
    vm->str_cause       = ajs_str_from_cstr(vm, "cause", true);
    vm->str_errors      = ajs_str_from_cstr(vm, "errors", true);
    vm->str_value       = ajs_str_from_cstr(vm, "value", true);
    vm->str_done        = ajs_str_from_cstr(vm, "done", true);
    vm->str_next        = ajs_str_from_cstr(vm, "next", true);
    vm->str_return      = ajs_str_from_cstr(vm, "return", true);
    vm->str_throw       = ajs_str_from_cstr(vm, "throw", true);
    vm->str_then        = ajs_str_from_cstr(vm, "then", true);
    vm->str_toJSON      = ajs_str_from_cstr(vm, "toJSON", true);
    vm->str_toString    = ajs_str_from_cstr(vm, "toString", true);
    vm->str_valueOf     = ajs_str_from_cstr(vm, "valueOf", true);
    vm->str_default     = ajs_str_from_cstr(vm, "default", true);
    vm->str_raw         = ajs_str_from_cstr(vm, "raw", true);

    /* Allocate well-known symbols */
    for (int i = 0; i < AJS_SYM_COUNT; i++) {
        AjsSymbol *sym = (AjsSymbol *)ajs_gc_alloc(vm, sizeof(AjsSymbol), AJS_GC_SYMBOL);
        if (sym) {
            sym->id = vm->next_symbol_id++;
            sym->has_desc = true;
            sym->is_registered = false;
            sym->is_private = false;
            sym->desc = ajs_str_from_cstr(vm, k_well_known_sym_descs[i], true);
        }
        vm->well_known_syms[i] = sym;
    }

    vm->main_realm = ajs_realm_create(vm);
    vm->active_realm = vm->main_realm;

    /* Pre-allocate out-of-memory RangeError object */
    if (vm->main_realm) {
        AjsObject *oom = ajs_obj_new(vm, vm->main_realm, AJS_OBJ_ERROR,
                                     vm->main_realm->error_proto[AJS_ERR_KIND_RANGE]);
        if (oom) {
            snprintf(oom->u.err.error_name, sizeof(oom->u.err.error_name), "RangeError");
            AjsString *s_oom = ajs_str_from_cstr(vm, "Out of memory", true);
            if (s_oom && vm->str_message) {
                AjsPropDesc d = {
                    .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                             AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
                    .value = ajs_val_from_gc(s_oom)
                };
                ajs_obj_define_own_prop(vm, oom, ajs_val_from_gc(vm->str_message), &d, false);
            }
            vm->oom_error = ajs_val_from_gc(oom);
        }
    }

    vm->gc_suppress--;
    return vm;
}

void ajs_vm_destroy(AjsVm *vm) {
    if (!vm) return;

    AjsJobNode *jn = vm->job_head;
    while (jn) {
        AjsJobNode *nx = jn->next;
        free(jn);
        jn = nx;
    }
    vm->job_head = vm->job_tail = NULL;

    ajs_gc_free_all(vm);

    AjsRealm *r = vm->realms;
    while (r) {
        AjsRealm *nx = r->next;
        free(r->tpl_cache);
        free(r);
        r = nx;
    }

    free(vm->handle_stack);
    free(vm->temp_roots);
    free(vm->pins);
    free(vm->pin_used);
    free(vm->intern_tab);
    free(vm->sym_reg);
    free(vm->call_stack);
    free(vm->modules);
    free(vm);
}

void ajs_vm_set_userdata(AjsVm *vm, void *userdata) {
    if (vm) vm->userdata = userdata;
}

void *ajs_vm_get_userdata(const AjsVm *vm) {
    return vm ? vm->userdata : NULL;
}

AjsRealm *ajs_vm_main_realm(AjsVm *vm) {
    return vm ? vm->main_realm : NULL;
}

AjsValue ajs_realm_global_object(AjsRealm *realm) {
    if (!realm) return AJS_VAL_UNDEFINED;
    if (realm->vm) ajs_handle_record(realm->vm, realm->global_obj);
    return realm->global_obj;
}

AjsValue ajs_global_object(AjsVm *vm) {
    if (!vm || !vm->active_realm) return AJS_VAL_UNDEFINED;
    ajs_handle_record(vm, vm->active_realm->global_obj);
    return vm->active_realm->global_obj;
}

void ajs_vm_set_interrupt_handler(AjsVm *vm, AjsInterruptFn fn, void *userdata) {
    if (!vm) return;
    vm->cfg.interrupt_fn = fn;
    vm->cfg.interrupt_userdata = userdata;
}

void ajs_vm_set_module_hooks(AjsVm *vm, AjsModuleResolveFn resolve_fn,
                             AjsModuleLoadFn load_fn, void *userdata) {
    if (!vm) return;
    vm->cfg.module_resolve_fn = resolve_fn;
    vm->cfg.module_load_fn = load_fn;
    vm->cfg.module_userdata = userdata;
}

/* ============================================================================
 * 2. Garbage Collection & Explicit Root Handles
 * ============================================================================ */

void ajs_handle_scope_open(AjsVm *vm, AjsHandleScope *scope) {
    if (!vm || !scope) return;
    scope->vm = vm;
    scope->saved_top = vm->handle_top;
    scope->prev = vm->current_handle_scope;
    vm->current_handle_scope = scope;
}

void ajs_handle_scope_close(AjsVm *vm, AjsHandleScope *scope) {
    if (!vm || !scope) return;
    vm->handle_top = scope->saved_top;
    vm->current_handle_scope = scope->prev;
}

AjsValue ajs_handle_escape(AjsVm *vm, AjsHandleScope *scope, AjsValue val) {
    if (!vm || !scope) return val;
    vm->handle_top = scope->saved_top;
    vm->current_handle_scope = scope->prev;
    ajs_handle_record(vm, val);
    return val;
}

uint32_t ajs_pin_value(AjsVm *vm, AjsValue val) {
    if (!vm) return 0;
    for (uint32_t i = 0; i < vm->pins_cap; i++) {
        if (!vm->pin_used[i]) {
            vm->pin_used[i] = true;
            vm->pins[i] = val;
            return i + 1u;
        }
    }
    uint32_t ncap = vm->pins_cap ? vm->pins_cap * 2u : 32u;
    AjsValue *npins = (AjsValue *)realloc(vm->pins, ncap * sizeof(AjsValue));
    if (!npins) return 0;
    vm->pins = npins;
    bool *nused = (bool *)realloc(vm->pin_used, ncap * sizeof(bool));
    if (!nused) return 0;
    vm->pin_used = nused;
    memset(vm->pin_used + vm->pins_cap, 0, (ncap - vm->pins_cap) * sizeof(bool));
    uint32_t idx = vm->pins_cap;
    vm->pins_cap = ncap;
    vm->pin_used[idx] = true;
    vm->pins[idx] = val;
    return idx + 1u;
}

AjsValue ajs_get_pinned(const AjsVm *vm, uint32_t pin_id) {
    if (!vm || pin_id == 0 || pin_id > vm->pins_cap) return AJS_VAL_UNDEFINED;
    if (!vm->pin_used[pin_id - 1u]) return AJS_VAL_UNDEFINED;
    return vm->pins[pin_id - 1u];
}

void ajs_unpin_value(AjsVm *vm, uint32_t pin_id) {
    if (!vm || pin_id == 0 || pin_id > vm->pins_cap) return;
    vm->pin_used[pin_id - 1u] = false;
    vm->pins[pin_id - 1u] = AJS_VAL_UNDEFINED;
}

size_t ajs_gc_used_bytes(const AjsVm *vm) {
    return vm ? vm->gc_bytes : 0;
}

/* ============================================================================
 * 3. Primitive & Value Constructors / Type Predicates / Conversions
 * ============================================================================ */

AjsValue ajs_undefined(void) { return AJS_VAL_UNDEFINED; }
AjsValue ajs_null(void)      { return AJS_VAL_NULL; }
AjsValue ajs_bool(bool b)    { return b ? AJS_VAL_TRUE : AJS_VAL_FALSE; }
AjsValue ajs_number(double d){ return ajs_val_number(d); }
AjsValue ajs_int32(int32_t i){ return ajs_val_number((double)i); }
AjsValue ajs_uint32(uint32_t u){ return ajs_val_number((double)u); }

AjsValue ajs_string_utf8(AjsVm *vm, const char *utf8, size_t byte_len) {
    if (!vm) return AJS_VAL_UNDEFINED;
    AjsString *s = ajs_str_from_utf8(vm, utf8 ? utf8 : "", byte_len, false);
    if (!s) return AJS_VAL_EXCEPTION;
    AjsValue v = ajs_val_from_gc(s);
    ajs_handle_record(vm, v);
    return v;
}

AjsValue ajs_string_cstr(AjsVm *vm, const char *cstr) {
    if (!vm) return AJS_VAL_UNDEFINED;
    AjsString *s = ajs_str_from_cstr(vm, cstr ? cstr : "", false);
    if (!s) return AJS_VAL_EXCEPTION;
    AjsValue v = ajs_val_from_gc(s);
    ajs_handle_record(vm, v);
    return v;
}

AjsValue ajs_string_utf16(AjsVm *vm, const uint16_t *units, size_t unit_len) {
    if (!vm) return AJS_VAL_UNDEFINED;
    AjsString *s = ajs_str_from_utf16(vm, units, unit_len, false);
    if (!s) return AJS_VAL_EXCEPTION;
    AjsValue v = ajs_val_from_gc(s);
    ajs_handle_record(vm, v);
    return v;
}

AjsValue ajs_symbol_new(AjsVm *vm, const char *desc_or_null) {
    if (!vm) return AJS_VAL_UNDEFINED;
    AjsString *desc = NULL;
    size_t tsave = ajs_temp_root_save(vm);
    if (desc_or_null) {
        desc = ajs_str_from_cstr(vm, desc_or_null, false);
        if (!desc) return AJS_VAL_EXCEPTION;
        ajs_temp_root_push(vm, ajs_val_from_gc(desc));
    }
    AjsSymbol *sym = (AjsSymbol *)ajs_gc_alloc(vm, sizeof(AjsSymbol), AJS_GC_SYMBOL);
    ajs_temp_root_restore(vm, tsave);
    if (!sym) return AJS_VAL_EXCEPTION;
    sym->id = vm->next_symbol_id++;
    sym->has_desc = (desc_or_null != NULL);
    sym->is_registered = false;
    sym->is_private = false;
    sym->desc = desc;
    AjsValue v = ajs_val_from_gc(sym);
    ajs_handle_record(vm, v);
    return v;
}

AjsValue ajs_bigint_from_i64(AjsVm *vm, int64_t v) {
    if (!vm) return AJS_VAL_UNDEFINED;
    AjsBigInt *bi = ajs_bigint_new_i64(vm, v);
    if (!bi) return AJS_VAL_EXCEPTION;
    AjsValue val = ajs_val_from_gc(bi);
    ajs_handle_record(vm, val);
    return val;
}

AjsType ajs_typeof(AjsVm *vm, AjsValue v) {
    (void)vm;
    if (v == AJS_VAL_UNDEFINED) return AJS_TYPE_UNDEFINED;
    if (v == AJS_VAL_NULL) return AJS_TYPE_NULL;
    if (v == AJS_VAL_TRUE || v == AJS_VAL_FALSE) return AJS_TYPE_BOOLEAN;
    if (ajs_val_is_number(v)) return AJS_TYPE_NUMBER;
    if (!ajs_val_is_ptr(v)) return AJS_TYPE_UNDEFINED;
    AjsGcHeader *hdr = ajs_val_to_gc(v);
    switch ((AjsGcKind)hdr->gc_kind) {
        case AJS_GC_STRING: return AJS_TYPE_STRING;
        case AJS_GC_SYMBOL: return AJS_TYPE_SYMBOL;
        case AJS_GC_BIGINT: return AJS_TYPE_BIGINT;
        case AJS_GC_OBJECT: {
            AjsObject *obj = (AjsObject *)hdr;
            if (obj->kind == AJS_OBJ_FUNCTION) return AJS_TYPE_FUNCTION;
            if (obj->kind == AJS_OBJ_PROXY && obj->u.proxy.is_callable) return AJS_TYPE_FUNCTION;
            return AJS_TYPE_OBJECT;
        }
        default: return AJS_TYPE_UNDEFINED;
    }
}

bool ajs_is_undefined(AjsValue v) { return v == AJS_VAL_UNDEFINED; }
bool ajs_is_null(AjsValue v)      { return v == AJS_VAL_NULL; }
bool ajs_is_nullish(AjsValue v)   { return v == AJS_VAL_UNDEFINED || v == AJS_VAL_NULL; }
bool ajs_is_bool(AjsValue v)      { return v == AJS_VAL_TRUE || v == AJS_VAL_FALSE; }
bool ajs_is_number(AjsValue v)    { return ajs_val_is_number(v); }

bool ajs_is_string(AjsVm *vm, AjsValue v) {
    (void)vm;
    return ajs_val_is_gc_kind(v, AJS_GC_STRING);
}

bool ajs_is_object(AjsVm *vm, AjsValue v) {
    (void)vm;
    return ajs_val_is_gc_kind(v, AJS_GC_OBJECT);
}

bool ajs_is_callable(AjsVm *vm, AjsValue v) {
    (void)vm;
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return false;
    AjsObject *obj = (AjsObject *)ajs_val_to_gc(v);
    if (obj->kind == AJS_OBJ_FUNCTION) return true;
    if (obj->kind == AJS_OBJ_PROXY) return obj->u.proxy.is_callable;
    return false;
}

static bool is_array_depth(AjsVm *vm, AjsValue v, int depth) {
    if (depth > 64) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Maximum call stack size exceeded");
        return false;
    }
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return false;
    AjsObject *obj = (AjsObject *)ajs_val_to_gc(v);
    if (obj->kind == AJS_OBJ_ARRAY) return true;
    if (obj->kind == AJS_OBJ_PROXY) {
        if (obj->u.proxy.handler == AJS_VAL_NULL) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot perform 'IsArray' on a proxy that has been revoked");
            return false;
        }
        return is_array_depth(vm, obj->u.proxy.target, depth + 1);
    }
    return false;
}

bool ajs_is_array(AjsVm *vm, AjsValue v) {
    return is_array_depth(vm, v, 0);
}

bool ajs_is_promise(AjsVm *vm, AjsValue v) {
    (void)vm;
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return false;
    return ((AjsObject *)ajs_val_to_gc(v))->kind == AJS_OBJ_PROMISE;
}

bool ajs_same_value(AjsVm *vm, AjsValue a, AjsValue b) {
    (void)vm;
    if (a == b) return true;
    if (ajs_val_is_number(a) && ajs_val_is_number(b)) {
        double da = ajs_val_to_number_unchecked(a);
        double db = ajs_val_to_number_unchecked(b);
        if (isnan(da) && isnan(db)) return true;
        return a == b; /* Distinguishes +0.0 (0x0) from -0.0 (0x8000000000000000)! */
    }
    if (ajs_val_is_gc_kind(a, AJS_GC_STRING) && ajs_val_is_gc_kind(b, AJS_GC_STRING)) {
        return ajs_str_eq((const AjsString *)ajs_val_to_gc(a), (const AjsString *)ajs_val_to_gc(b));
    }
    if (ajs_val_is_gc_kind(a, AJS_GC_BIGINT) && ajs_val_is_gc_kind(b, AJS_GC_BIGINT)) {
        return ajs_bigint_eq((const AjsBigInt *)ajs_val_to_gc(a), (const AjsBigInt *)ajs_val_to_gc(b));
    }
    return false;
}

bool ajs_to_bool(AjsVm *vm, AjsValue v) {
    (void)vm;
    if (v == AJS_VAL_UNDEFINED || v == AJS_VAL_NULL || v == AJS_VAL_FALSE) return false;
    if (v == AJS_VAL_TRUE) return true;
    if (ajs_val_is_number(v)) {
        double d = ajs_val_to_number_unchecked(v);
        return d != 0.0 && !isnan(d);
    }
    if (!ajs_val_is_ptr(v)) return false;
    AjsGcHeader *hdr = ajs_val_to_gc(v);
    if (hdr->gc_kind == AJS_GC_STRING) return ((const AjsString *)hdr)->len16 > 0;
    if (hdr->gc_kind == AJS_GC_BIGINT) return ((const AjsBigInt *)hdr)->sign != 0;
    return true; /* Symbols and all Objects (no document.all) are truthy */
}

bool ajs_to_number(AjsVm *vm, AjsValue v, double *out_num) {
    if (out_num) *out_num = 0.0;
    if (!vm) return false;
    if (ajs_val_is_number(v)) {
        if (out_num) *out_num = ajs_val_to_number_unchecked(v);
        return true;
    }
    if (v == AJS_VAL_UNDEFINED) {
        if (out_num) *out_num = NAN;
        return true;
    }
    if (v == AJS_VAL_NULL || v == AJS_VAL_FALSE) {
        if (out_num) *out_num = 0.0;
        return true;
    }
    if (v == AJS_VAL_TRUE) {
        if (out_num) *out_num = 1.0;
        return true;
    }
    if (ajs_val_is_gc_kind(v, AJS_GC_STRING)) {
        if (out_num) *out_num = ajs_str_to_number((const AjsString *)ajs_val_to_gc(v));
        return true;
    }
    if (ajs_val_is_gc_kind(v, AJS_GC_SYMBOL)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot convert a Symbol value to a number");
        return false;
    }
    if (ajs_val_is_gc_kind(v, AJS_GC_BIGINT)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot convert a BigInt value to a number");
        return false;
    }
    if (ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) {
        AjsValue prim = ajs_to_primitive(vm, v, 2); /* hint number */
        if (vm->has_exception || prim == AJS_VAL_EXCEPTION) return false;
        return ajs_to_number(vm, prim, out_num);
    }
    return false;
}

bool ajs_to_int32(AjsVm *vm, AjsValue v, int32_t *out_i32) {
    double d = 0.0;
    if (!ajs_to_number(vm, v, &d)) return false;
    if (out_i32) *out_i32 = ajs_double_to_int32(d);
    return true;
}

bool ajs_to_uint32(AjsVm *vm, AjsValue v, uint32_t *out_u32) {
    double d = 0.0;
    if (!ajs_to_number(vm, v, &d)) return false;
    if (out_u32) *out_u32 = ajs_double_to_uint32(d);
    return true;
}

char *ajs_to_cstring_len(AjsVm *vm, AjsValue v, size_t *out_byte_len) {
    if (out_byte_len) *out_byte_len = 0;
    if (!vm) return NULL;
    AjsString *s = ajs_to_js_string(vm, v);
    if (!s || vm->has_exception) return NULL;
    char *copy = (char *)malloc((size_t)s->len8 + 1u);
    if (!copy) {
        ajs_throw(vm, vm->oom_error);
        return NULL;
    }
    memcpy(copy, s->utf8, (size_t)s->len8 + 1u);
    if (out_byte_len) *out_byte_len = (size_t)s->len8;
    return copy;
}

char *ajs_to_cstring(AjsVm *vm, AjsValue v) {
    return ajs_to_cstring_len(vm, v, NULL);
}

void ajs_free_cstring(AjsVm *vm, char *str) {
    (void)vm;
    free(str);
}

/* ============================================================================
 * 4. Objects, Prototypes, Descriptors, Native Functions & Host Opaque Wrappers
 * ============================================================================ */

AjsValue ajs_new_object(AjsVm *vm) {
    if (!vm || !vm->active_realm) return AJS_VAL_UNDEFINED;
    AjsObject *obj = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ORDINARY, vm->active_realm->object_proto);
    if (!obj) return AJS_VAL_EXCEPTION;
    AjsValue v = ajs_val_from_gc(obj);
    ajs_handle_record(vm, v);
    return v;
}

AjsValue ajs_new_object_with_proto(AjsVm *vm, AjsValue proto) {
    if (!vm || !vm->active_realm) return AJS_VAL_UNDEFINED;
    if (proto != AJS_VAL_NULL && !ajs_val_is_gc_kind(proto, AJS_GC_OBJECT)) {
        proto = vm->active_realm->object_proto;
    }
    AjsObject *obj = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ORDINARY, proto);
    if (!obj) return AJS_VAL_EXCEPTION;
    AjsValue v = ajs_val_from_gc(obj);
    ajs_handle_record(vm, v);
    return v;
}

AjsValue ajs_new_array(AjsVm *vm, uint32_t initial_len) {
    if (!vm || !vm->active_realm) return AJS_VAL_UNDEFINED;
    AjsObject *arr = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ARRAY, vm->active_realm->array_proto);
    if (!arr) return AJS_VAL_EXCEPTION;
    arr->u.arr.length = initial_len;
    arr->u.arr.length_writable = true;
    if (initial_len > 0 && initial_len <= 65536u) {
        arr->u.arr.dense = (AjsValue *)malloc((size_t)initial_len * sizeof(AjsValue));
        if (arr->u.arr.dense) {
            for (uint32_t i = 0; i < initial_len; i++) arr->u.arr.dense[i] = AJS_VAL_EMPTY;
            arr->u.arr.dense_cap = initial_len;
            arr->u.arr.dense_len = initial_len;
        }
    }
    AjsValue v = ajs_val_from_gc(arr);
    ajs_handle_record(vm, v);
    return v;
}

static AjsObject *alloc_embedder_fn(AjsVm *vm, AjsNativeFn fn, const char *name,
                                    int length, bool is_ctor, void *userdata) {
    AjsRealm *r = vm->active_realm ? vm->active_realm : vm->main_realm;
    AjsObject *fobj = ajs_obj_new(vm, r, AJS_OBJ_FUNCTION, r->function_proto);
    if (!fobj) return NULL;
    size_t tsave = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(fobj));

    fobj->u.fn.fn_kind = AJS_FN_NATIVE;
    fobj->u.fn.is_constructor = is_ctor;
    fobj->u.fn.native_fn = fn;
    fobj->u.fn.native_userdata = userdata;
    fobj->u.fn.native_magic = 0; /* 0 = embedder native constructor / function */

    AjsPropDesc ldesc = {
        .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
        .value = ajs_val_number(length < 0 ? 0.0 : (double)length)
    };
    ajs_obj_define_own_prop(vm, fobj, ajs_val_from_gc(vm->str_length), &ldesc, false);

    AjsString *sname = ajs_str_from_cstr(vm, name ? name : "", true);
    if (sname) {
        ajs_temp_root_push(vm, ajs_val_from_gc(sname));
        AjsPropDesc ndesc = {
            .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
            .value = ajs_val_from_gc(sname)
        };
        ajs_obj_define_own_prop(vm, fobj, ajs_val_from_gc(vm->str_name), &ndesc, false);
    }

    ajs_temp_root_restore(vm, tsave);
    return fobj;
}

AjsValue ajs_new_function(AjsVm *vm, AjsNativeFn fn, const char *name,
                          int length, void *userdata) {
    if (!vm || !fn) return AJS_VAL_UNDEFINED;
    AjsObject *fobj = alloc_embedder_fn(vm, fn, name, length, false, userdata);
    if (!fobj) return AJS_VAL_EXCEPTION;
    AjsValue v = ajs_val_from_gc(fobj);
    ajs_handle_record(vm, v);
    return v;
}

AjsValue ajs_new_constructor(AjsVm *vm, AjsNativeFn ctor_fn, const char *name,
                             int length, AjsValue prototype_obj, void *userdata) {
    if (!vm || !ctor_fn) return AJS_VAL_UNDEFINED;
    size_t tsave = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, prototype_obj);

    AjsObject *fobj = alloc_embedder_fn(vm, ctor_fn, name, length, true, userdata);
    if (!fobj) {
        ajs_temp_root_restore(vm, tsave);
        return AJS_VAL_EXCEPTION;
    }
    AjsValue fv = ajs_val_from_gc(fobj);
    ajs_temp_root_push(vm, fv);

    if (!ajs_val_is_gc_kind(prototype_obj, AJS_GC_OBJECT)) {
        AjsObject *p = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ORDINARY, vm->active_realm->object_proto);
        if (p) {
            prototype_obj = ajs_val_from_gc(p);
            ajs_temp_root_push(vm, prototype_obj);
        }
    }

    if (ajs_val_is_gc_kind(prototype_obj, AJS_GC_OBJECT)) {
        AjsPropDesc pdesc = {
            .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE,
            .value = prototype_obj
        };
        ajs_obj_define_own_prop(vm, fobj, ajs_val_from_gc(vm->str_prototype), &pdesc, false);

        AjsObject *pobj = (AjsObject *)ajs_val_to_gc(prototype_obj);
        AjsPropDesc cdesc = {
            .flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                     AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE,
            .value = fv
        };
        ajs_obj_define_own_prop(vm, pobj, ajs_val_from_gc(vm->str_constructor), &cdesc, false);
    }

    ajs_temp_root_restore(vm, tsave);
    ajs_handle_record(vm, fv);
    return fv;
}

void ajs_set_opaque(AjsVm *vm, AjsValue obj, uint32_t class_tag,
                    void *opaque, AjsFinalizerFn finalizer, AjsGcMarkFn gc_mark) {
    (void)vm;
    if (!ajs_val_is_gc_kind(obj, AJS_GC_OBJECT)) return;
    AjsObject *o = (AjsObject *)ajs_val_to_gc(obj);
    o->opaque_tag = class_tag;
    o->opaque_ptr = opaque;
    o->finalizer = finalizer;
    o->gc_mark = gc_mark;
}

void *ajs_get_opaque(AjsVm *vm, AjsValue obj, uint32_t expected_class_tag) {
    (void)vm;
    if (!ajs_val_is_gc_kind(obj, AJS_GC_OBJECT)) return NULL;
    AjsObject *o = (AjsObject *)ajs_val_to_gc(obj);
    if (expected_class_tag != 0 && o->opaque_tag != expected_class_tag) return NULL;
    return o->opaque_ptr;
}

bool ajs_set_prototype(AjsVm *vm, AjsValue obj, AjsValue proto) {
    if (!vm || !ajs_val_is_gc_kind(obj, AJS_GC_OBJECT)) return false;
    if (proto != AJS_VAL_NULL && !ajs_val_is_gc_kind(proto, AJS_GC_OBJECT)) return false;
    return ajs_obj_set_proto(vm, (AjsObject *)ajs_val_to_gc(obj), proto, false);
}

AjsValue ajs_get_prototype(AjsVm *vm, AjsValue obj) {
    if (!vm || !ajs_val_is_gc_kind(obj, AJS_GC_OBJECT)) return AJS_VAL_NULL;
    AjsValue proto = AJS_VAL_NULL;
    if (!ajs_obj_get_proto(vm, (AjsObject *)ajs_val_to_gc(obj), &proto)) return AJS_VAL_EXCEPTION;
    ajs_handle_record(vm, proto);
    return proto;
}

static uint32_t prop_flags_to_desc(uint32_t prop_flags) {
    uint32_t df = AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
    if (prop_flags & AJS_PROP_ENUMERABLE) df |= AJS_DESC_ENUMERABLE;
    if (prop_flags & AJS_PROP_CONFIGURABLE) df |= AJS_DESC_CONFIGURABLE;
    return df;
}

AjsValue ajs_get_prop_str(AjsVm *vm, AjsValue obj, const char *name) {
    if (!vm || !name) return AJS_VAL_UNDEFINED;
    AjsString *k = ajs_str_from_cstr(vm, name, true);
    if (!k) return AJS_VAL_EXCEPTION;
    size_t tsave = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(k));
    AjsValue v = ajs_obj_get(vm, obj, ajs_val_from_gc(k), obj);
    ajs_temp_root_restore(vm, tsave);
    ajs_handle_record(vm, v);
    return v;
}

AjsValue ajs_get_prop_index(AjsVm *vm, AjsValue obj, uint32_t idx) {
    if (!vm) return AJS_VAL_UNDEFINED;
    char ibuf[16];
    snprintf(ibuf, sizeof(ibuf), "%u", idx);
    return ajs_get_prop_str(vm, obj, ibuf);
}

bool ajs_set_prop_str(AjsVm *vm, AjsValue obj, const char *name, AjsValue val) {
    if (!vm || !name) return false;
    AjsString *k = ajs_str_from_cstr(vm, name, true);
    if (!k) return false;
    size_t tsave = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(k));
    ajs_temp_root_push(vm, val);
    bool ok = ajs_obj_set(vm, obj, ajs_val_from_gc(k), val, obj, true);
    ajs_temp_root_restore(vm, tsave);
    return ok && !vm->has_exception;
}

bool ajs_set_prop_index(AjsVm *vm, AjsValue obj, uint32_t idx, AjsValue val) {
    if (!vm) return false;
    char ibuf[16];
    snprintf(ibuf, sizeof(ibuf), "%u", idx);
    return ajs_set_prop_str(vm, obj, ibuf, val);
}

bool ajs_define_prop_str(AjsVm *vm, AjsValue obj, const char *name,
                         AjsValue val, uint32_t prop_flags) {
    if (!vm || !name || !ajs_val_is_gc_kind(obj, AJS_GC_OBJECT)) return false;
    AjsString *k = ajs_str_from_cstr(vm, name, true);
    if (!k) return false;
    size_t tsave = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(k));
    ajs_temp_root_push(vm, val);
    AjsPropDesc desc = {
        .flags = prop_flags_to_desc(prop_flags) | AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE |
                 ((prop_flags & AJS_PROP_WRITABLE) ? AJS_DESC_WRITABLE : 0u),
        .value = val
    };
    bool ok = ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(obj), ajs_val_from_gc(k), &desc, false);
    ajs_temp_root_restore(vm, tsave);
    return ok && !vm->has_exception;
}

bool ajs_define_accessor_str(AjsVm *vm, AjsValue obj, const char *name,
                             AjsValue getter_fn, AjsValue setter_fn, uint32_t prop_flags) {
    if (!vm || !name || !ajs_val_is_gc_kind(obj, AJS_GC_OBJECT)) return false;
    AjsString *k = ajs_str_from_cstr(vm, name, true);
    if (!k) return false;
    size_t tsave = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(k));
    ajs_temp_root_push(vm, getter_fn);
    ajs_temp_root_push(vm, setter_fn);
    AjsPropDesc desc = {
        .flags = prop_flags_to_desc(prop_flags) | AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_GET | AJS_DESC_HAS_SET,
        .getter = getter_fn,
        .setter = setter_fn
    };
    bool ok = ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(obj), ajs_val_from_gc(k), &desc, false);
    ajs_temp_root_restore(vm, tsave);
    return ok && !vm->has_exception;
}

bool ajs_has_prop_str(AjsVm *vm, AjsValue obj, const char *name) {
    if (!vm || !name || !ajs_val_is_gc_kind(obj, AJS_GC_OBJECT)) return false;
    AjsString *k = ajs_str_from_cstr(vm, name, true);
    if (!k) return false;
    bool has = false;
    size_t tsave = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(k));
    bool ok = ajs_obj_has_prop(vm, (AjsObject *)ajs_val_to_gc(obj), ajs_val_from_gc(k), &has);
    ajs_temp_root_restore(vm, tsave);
    return ok && has && !vm->has_exception;
}

bool ajs_delete_prop_str(AjsVm *vm, AjsValue obj, const char *name) {
    if (!vm || !name || !ajs_val_is_gc_kind(obj, AJS_GC_OBJECT)) return false;
    AjsString *k = ajs_str_from_cstr(vm, name, true);
    if (!k) return false;
    size_t tsave = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(k));
    bool ok = ajs_obj_delete(vm, (AjsObject *)ajs_val_to_gc(obj), ajs_val_from_gc(k), true);
    ajs_temp_root_restore(vm, tsave);
    return ok && !vm->has_exception;
}

/* ============================================================================
 * 5. Exceptions, Invocation & Classic Script Evaluation
 * ============================================================================ */

bool ajs_has_exception(const AjsVm *vm) {
    return vm ? vm->has_exception : false;
}

AjsValue ajs_throw(AjsVm *vm, AjsValue exc_val) {
    if (!vm) return AJS_VAL_EXCEPTION;
    vm->has_exception = true;
    vm->exc_phase = AJS_ERR_RUNTIME;
    vm->exception = exc_val;
    return AJS_VAL_EXCEPTION;
}

AjsValue ajs_throw_error(AjsVm *vm, const char *error_ctor_name, const char *fmt, ...) {
    if (!vm) return AJS_VAL_EXCEPTION;
    AjsErrKind kind = err_kind_from_name(error_ctor_name);
    va_list ap;
    va_start(ap, fmt);
    AjsValue r = throw_err_vfmt(vm, kind, fmt, ap);
    va_end(ap);
    return r;
}

AjsValue ajs_take_exception(AjsVm *vm, AjsErrorInfo *out_info_or_null) {
    if (!vm || !vm->has_exception) {
        if (out_info_or_null) memset(out_info_or_null, 0, sizeof(*out_info_or_null));
        return AJS_VAL_UNDEFINED;
    }
    AjsValue exc = vm->exception;
    AjsErrorPhase phase = vm->exc_phase ? vm->exc_phase : AJS_ERR_RUNTIME;
    vm->has_exception = false;
    vm->exception = AJS_VAL_UNDEFINED;
    vm->exc_phase = AJS_ERR_NONE;

    size_t tsave = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, exc);
    if (out_info_or_null) {
        ajs_fill_error_info(vm, exc, phase, out_info_or_null);
    }
    ajs_temp_root_restore(vm, tsave);
    ajs_handle_record(vm, exc);
    return exc;
}

void ajs_clear_exception(AjsVm *vm) {
    if (!vm) return;
    vm->has_exception = false;
    vm->exception = AJS_VAL_UNDEFINED;
    vm->exc_phase = AJS_ERR_NONE;
}

AjsValue ajs_call(AjsVm *vm, AjsValue fn, AjsValue this_val, int argc, const AjsValue *argv) {
    if (!vm) return AJS_VAL_UNDEFINED;
    AjsValue res = ajs_call_internal(vm, fn, this_val, argc, argv);
    if (!vm->has_exception && res != AJS_VAL_EXCEPTION) {
        ajs_handle_record(vm, res);
    }
    return res;
}

AjsValue ajs_construct(AjsVm *vm, AjsValue ctor, int argc, const AjsValue *argv) {
    if (!vm) return AJS_VAL_UNDEFINED;
    AjsValue res = ajs_construct_internal(vm, ctor, argc, argv, ctor);
    if (!vm->has_exception && res != AJS_VAL_EXCEPTION) {
        ajs_handle_record(vm, res);
    }
    return res;
}

bool ajs_eval_script_in_realm(AjsRealm *realm, const char *utf8_code, size_t code_len,
                              const char *source_url, bool force_strict,
                              AjsValue *out_result_or_null, AjsErrorInfo *out_err_or_null) {
    if (out_result_or_null) *out_result_or_null = AJS_VAL_UNDEFINED;
    if (out_err_or_null) memset(out_err_or_null, 0, sizeof(*out_err_or_null));
    if (!realm || !realm->vm) return false;

    AjsVm *vm = realm->vm;
    AjsRealm *prev_realm = vm->active_realm;
    vm->active_realm = realm;

    AjsErrorInfo parse_err;
    memset(&parse_err, 0, sizeof(parse_err));
    AjsBytecode *bc = ajs_compile_script(vm, realm, utf8_code ? utf8_code : "", code_len,
                                         source_url ? source_url : "<script>", force_strict,
                                         false, NULL, &parse_err);
    if (!bc) {
        if (vm->has_exception) {
            AjsErrorInfo tmp_info;
            ajs_take_exception(vm, &tmp_info);
            if (parse_err.phase == AJS_ERR_NONE) parse_err = tmp_info;
        }
        if (parse_err.phase == AJS_ERR_NONE) {
            parse_err.phase = AJS_ERR_PARSE;
            snprintf(parse_err.name, sizeof(parse_err.name), "SyntaxError");
            snprintf(parse_err.message, sizeof(parse_err.message), "Syntax error");
        }
        if (out_err_or_null) *out_err_or_null = parse_err;
        vm->last_err_info = parse_err;
        vm->active_realm = prev_realm ? prev_realm : vm->main_realm;
        return false;
    }

    size_t tsave = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(bc));

    AjsValue res = ajs_exec_script_bc(vm, realm, bc, NULL, realm->global_obj);
    ajs_temp_root_restore(vm, tsave);

    if (vm->has_exception || res == AJS_VAL_EXCEPTION) {
        AjsErrorInfo rt_err;
        ajs_take_exception(vm, &rt_err);
        if (out_err_or_null) *out_err_or_null = rt_err;
        vm->last_err_info = rt_err;
        vm->active_realm = prev_realm ? prev_realm : vm->main_realm;
        return false;
    }

    vm->active_realm = prev_realm ? prev_realm : vm->main_realm;
    if (out_result_or_null) {
        ajs_handle_record(vm, res);
        *out_result_or_null = res;
    }
    return true;
}

bool ajs_eval_script(AjsVm *vm, const char *utf8_code, size_t code_len,
                     const char *source_url, bool force_strict,
                     AjsValue *out_result_or_null, AjsErrorInfo *out_err_or_null) {
    if (!vm) return false;
    AjsRealm *r = vm->active_realm ? vm->active_realm : vm->main_realm;
    return ajs_eval_script_in_realm(r, utf8_code, code_len, source_url, force_strict,
                                    out_result_or_null, out_err_or_null);
}
