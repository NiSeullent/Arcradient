/*
 * ArcradientJS 1.0 (Clean-Room Pure C11 ECMAScript 2026 Virtual Machine)
 * js/modules/ajs_module.c - ECMA-262 §16.2 Source Text Module Record
 *
 * Implements:
 * - Parse & static export validation + per-Realm canonical URL module caching
 * - LoadRequestedModules + Tarjan SCC InnerModuleLinking (circular & diamond graphs)
 * - ResolveExport (cycle detection, indirect re-exports, `*namespace*`, `export *` ambiguity)
 * - Live import bindings (`AJS_SLOT_IMPORT_LINK`), hoisted module functions & lexical TDZ
 * - Tarjan SCC InnerModuleEvaluation + Top-Level Await (§16.2.1.5.2 async graph ordering)
 * - Module Namespace Exotic Object (`AJS_OBJ_MODULE_NS`), `import.meta`, dynamic `import()`
 */
#include "../vm/ajs_internal.h"

/* ============================================================================
 * 1. Small Allocation & Diagnostic Helpers
 * ============================================================================ */

static char *mod_strdup(const char *s) {
    if (!s) s = "";
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1u);
    if (!copy) return NULL;
    ajs_memcpy(copy, s, len + 1u);
    return copy;
}

static AjsValue mod_make_error_obj(AjsVm *vm, AjsRealm *realm, AjsErrKind kind,
                                   const char *url, uint32_t line, uint32_t col,
                                   const char *fmt, ...) {
    char msg[512];
    msg[0] = '\0';
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
    }

    AjsRealm *r = realm ? realm : (vm->active_realm ? vm->active_realm : vm->main_realm);
    vm->gc_suppress++;
    AjsValue proto = (r && kind < AJS_ERR_KIND_COUNT && r->error_proto[kind] != AJS_VAL_UNDEFINED)
                     ? r->error_proto[kind]
                     : (r ? r->object_proto : AJS_VAL_NULL);
    AjsObject *eobj = ajs_obj_new(vm, r, AJS_OBJ_ERROR, proto);
    if (!eobj) {
        vm->gc_suppress--;
        return vm->oom_error;
    }

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(eobj));

    static const char *const k_names[AJS_ERR_KIND_COUNT] = {
        "Error", "EvalError", "RangeError", "ReferenceError",
        "SyntaxError", "TypeError", "URIError", "AggregateError", "SuppressedError"
    };
    snprintf(eobj->u.err.error_name, sizeof(eobj->u.err.error_name), "%s",
             kind < AJS_ERR_KIND_COUNT ? k_names[kind] : "Error");

    AjsString *s_msg = ajs_str_from_cstr(vm, msg, false);
    if (s_msg && vm->str_message) {
        ajs_temp_root_push(vm, ajs_val_from_gc(s_msg));
        AjsPropDesc md;
        memset(&md, 0, sizeof(md));
        md.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                   AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
        md.value = ajs_val_from_gc(s_msg);
        ajs_obj_define_own_prop(vm, eobj, ajs_val_from_gc(vm->str_message), &md, false);
    }

    ajs_capture_stack_trace(vm, eobj, url, line, col);
    vm->gc_suppress--;
    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(eobj);
}

static void mod_set_err_info(AjsVm *vm, AjsErrorPhase phase, const char *err_name,
                             const char *url, uint32_t line, uint32_t col,
                             AjsErrorInfo *out_err, const char *fmt, ...) {
    AjsErrorInfo info;
    memset(&info, 0, sizeof(info));
    info.phase = phase;
    snprintf(info.name, sizeof(info.name), "%s", err_name ? err_name : "SyntaxError");
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(info.message, sizeof(info.message), fmt, ap);
        va_end(ap);
    }
    if (url) snprintf(info.url, sizeof(info.url), "%s", url);
    info.line = line ? line : 1u;
    info.column = col ? col : 1u;
    snprintf(info.stack, sizeof(info.stack), "%s: %s\n    at %s:%u:%u",
             info.name, info.message,
             info.url[0] ? info.url : "<module>", info.line, info.column);
    vm->last_err_info = info;
    if (out_err) *out_err = info;
}

static AjsObject *mod_make_closure_fn(AjsVm *vm, AjsRealm *r, AjsNativeFn fn,
                                      int32_t magic, AjsEnv *env) {
    size_t rs = ajs_temp_root_save(vm);
    if (env) ajs_temp_root_push(vm, ajs_val_from_gc(env));
    AjsObject *f = ajs_obj_new(vm, r, AJS_OBJ_FUNCTION, r->function_proto);
    if (!f) {
        ajs_temp_root_restore(vm, rs);
        return NULL;
    }
    ajs_temp_root_push(vm, ajs_val_from_gc(f));
    f->u.fn.fn_kind = AJS_FN_NATIVE;
    f->u.fn.is_constructor = false;
    f->u.fn.strict = true;
    f->u.fn.native_fn = fn;
    f->u.fn.native_magic = magic;
    f->u.fn.closure_env = env;

    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
              AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    d.value = ajs_val_number(0.0);
    ajs_obj_define_own_prop(vm, f, ajs_val_from_gc(vm->str_length), &d, false);
    d.value = ajs_val_from_gc(vm->str_empty);
    ajs_obj_define_own_prop(vm, f, ajs_val_from_gc(vm->str_name), &d, false);
    ajs_temp_root_restore(vm, rs);
    return f;
}

static AjsEnv *mod_current_fn_env(const AjsVm *vm) {
    if (vm->call_depth == 0) return NULL;
    AjsObject *fn = vm->call_stack[vm->call_depth - 1u].native_fn_obj;
    return fn ? fn->u.fn.closure_env : NULL;
}

static int32_t mod_current_fn_magic(const AjsVm *vm) {
    if (vm->call_depth == 0) return 0;
    AjsObject *fn = vm->call_stack[vm->call_depth - 1u].native_fn_obj;
    return fn ? fn->u.fn.native_magic : 0;
}

/* ============================================================================
 * 2. VM Module Registry / Cache
 * ============================================================================ */

static bool mod_cache_register(AjsVm *vm, AjsModuleRecord *mod) {
    for (uint32_t i = 0; i < vm->module_count; i++) {
        if (vm->modules[i] == mod) return true;
    }
    if (vm->module_count >= vm->module_cap) {
        uint32_t ncap = vm->module_cap ? vm->module_cap * 2u : 16u;
        AjsModuleRecord **narr = (AjsModuleRecord **)realloc(
            vm->modules, (size_t)ncap * sizeof(AjsModuleRecord *));
        if (!narr) return false;
        vm->modules = narr;
        vm->module_cap = ncap;
    }
    vm->modules[vm->module_count++] = mod;
    return true;
}

static AjsModuleRecord *mod_cache_find(const AjsVm *vm, const AjsRealm *realm, const char *url) {
    if (!vm || !url || !url[0] || url[0] == '<') return NULL;
    for (uint32_t i = vm->module_count; i > 0; i--) {
        AjsModuleRecord *m = vm->modules[i - 1u];
        if (m && m->realm == realm && m->url && strcmp(m->url, url) == 0) {
            return m;
        }
    }
    return NULL;
}

static AjsModuleRecord *mod_find_loaded_child(const AjsModuleRecord *mod, const AjsString *spec) {
    if (!mod || !spec || !mod->loaded_modules) return NULL;
    for (uint32_t i = 0; i < mod->req_count; i++) {
        if (ajs_str_eq(mod->requested_specifiers[i], spec)) {
            return mod->loaded_modules[i];
        }
    }
    return NULL;
}

/* ============================================================================
 * 3. Parse & Early Export Validation (ECMA-262 §16.2.1.6.1)
 * ============================================================================ */

const char *ajs_module_url(const AjsModuleRecord *mod) {
    return (mod && mod->url) ? mod->url : "";
}

AjsModuleRecord *ajs_module_parse(AjsVm *vm, const char *utf8_code, size_t code_len,
                                  const char *module_url, AjsErrorInfo *out_err_or_null) {
    if (out_err_or_null) memset(out_err_or_null, 0, sizeof(*out_err_or_null));
    if (!vm) return NULL;

    AjsRealm *realm = vm->active_realm ? vm->active_realm : vm->main_realm;
    size_t rs = ajs_temp_root_save(vm);

    AjsModuleRecord *mod = (AjsModuleRecord *)ajs_gc_alloc(vm, sizeof(AjsModuleRecord), AJS_GC_MODULE);
    if (!mod) {
        if (vm->has_exception) ajs_take_exception(vm, out_err_or_null);
        ajs_temp_root_restore(vm, rs);
        return NULL;
    }
    ajs_temp_root_push(vm, ajs_val_from_gc(mod));

    mod->realm = realm;
    mod->status = AJS_MOD_UNLINKED;
    mod->url = mod_strdup(module_url ? module_url : "<module>");
    mod->namespace_obj = AJS_VAL_UNDEFINED;
    mod->import_meta_obj = AJS_VAL_UNDEFINED;
    mod->top_capability_promise = AJS_VAL_UNDEFINED;
    mod->top_resolve_fn = AJS_VAL_UNDEFINED;
    mod->top_reject_fn = AJS_VAL_UNDEFINED;
    mod->eval_error = AJS_VAL_UNDEFINED;

    AjsErrorInfo perr;
    memset(&perr, 0, sizeof(perr));
    if (!ajs_parse_and_compile_module(vm, realm, mod, utf8_code ? utf8_code : "", code_len, &perr)) {
        if (vm->has_exception) {
            AjsErrorInfo tmp;
            ajs_take_exception(vm, &tmp);
            if (perr.phase == AJS_ERR_NONE) perr = tmp;
        }
        if (perr.phase == AJS_ERR_NONE) {
            perr.phase = AJS_ERR_PARSE;
            snprintf(perr.name, sizeof(perr.name), "SyntaxError");
            snprintf(perr.message, sizeof(perr.message), "Module parse error");
        } else {
            perr.phase = AJS_ERR_PARSE;
        }
        vm->last_err_info = perr;
        if (out_err_or_null) *out_err_or_null = perr;
        ajs_temp_root_restore(vm, rs);
        return NULL;
    }

    /* ECMA-262 §16.2.1.1 Static Early Errors:
     * 1. No duplicate ExportedNames across local/indirect named export entries.
     * 2. Local exports (`module_specifier == NULL`) must reference a declared binding in `mod->bc`. */
    for (uint32_t i = 0; i < mod->export_count; i++) {
        const AjsExportEntry *ei = &mod->exports[i];
        if (!ei->module_specifier && ei->local_slot >= mod->bc->env_slot_count) {
            mod_set_err_info(vm, AJS_ERR_PARSE, "SyntaxError", mod->url, ei->line, ei->col,
                             out_err_or_null, "Export '%s' is not defined in module",
                             ei->local_name ? ei->local_name->utf8 : "");
            ajs_temp_root_restore(vm, rs);
            return NULL;
        }
        if (!ei->export_name) continue;
        for (uint32_t j = 0; j < i; j++) {
            const AjsExportEntry *ej = &mod->exports[j];
            if (ej->export_name && ajs_str_eq(ei->export_name, ej->export_name)) {
                mod_set_err_info(vm, AJS_ERR_PARSE, "SyntaxError", mod->url, ei->line, ei->col,
                                 out_err_or_null, "Duplicate export of '%s'", ei->export_name->utf8);
                ajs_temp_root_restore(vm, rs);
                return NULL;
            }
        }
    }

    mod_cache_register(vm, mod);
    ajs_temp_root_restore(vm, rs);
    return mod;
}

/* ============================================================================
 * 4. ResolveExport & GetExportedNames (ECMA-262 §16.2.1.6.2 – §16.2.1.6.3)
 * ============================================================================ */

typedef enum ModResolveStatus {
    MOD_RES_NOT_FOUND = 0,
    MOD_RES_AMBIGUOUS = 1,
    MOD_RES_FOUND     = 2
} ModResolveStatus;

typedef struct ModResolvedBinding {
    AjsModuleRecord *mod;
    uint32_t         slot;
    bool             is_namespace;
} ModResolvedBinding;

typedef struct ModResolvePair {
    const AjsModuleRecord *mod;
    const AjsString       *export_name;
} ModResolvePair;

typedef struct ModResolveSet {
    ModResolvePair *items;
    uint32_t        count;
    uint32_t        cap;
} ModResolveSet;

static bool resolve_set_contains_or_add(ModResolveSet *set, const AjsModuleRecord *mod,
                                        const AjsString *export_name, bool *out_already) {
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->items[i].mod == mod && ajs_str_eq(set->items[i].export_name, export_name)) {
            *out_already = true;
            return true;
        }
    }
    *out_already = false;
    if (set->count >= set->cap) {
        uint32_t ncap = set->cap ? set->cap * 2u : 16u;
        ModResolvePair *ni = (ModResolvePair *)realloc(set->items, (size_t)ncap * sizeof(ModResolvePair));
        if (!ni) return false;
        set->items = ni;
        set->cap = ncap;
    }
    set->items[set->count].mod = mod;
    set->items[set->count].export_name = export_name;
    set->count++;
    return true;
}

static bool same_resolved_binding(const ModResolvedBinding *a, const ModResolvedBinding *b) {
    if (a->mod != b->mod || a->is_namespace != b->is_namespace) return false;
    if (a->is_namespace) return true;
    return a->slot == b->slot;
}

static ModResolveStatus mod_resolve_export_rec(AjsVm *vm, AjsModuleRecord *mod,
                                               const AjsString *export_name,
                                               ModResolveSet *set,
                                               ModResolvedBinding *out_binding) {
    if (!mod || !export_name) return MOD_RES_NOT_FOUND;

    bool already = false;
    if (!resolve_set_contains_or_add(set, mod, export_name, &already)) {
        return MOD_RES_NOT_FOUND;
    }
    if (already) {
        /* Circular import/re-export without concrete binding resolves to null per §16.2.1.6.3 step 3 */
        return MOD_RES_NOT_FOUND;
    }

    /* 1. Direct local exports (`module_specifier == NULL`) */
    for (uint32_t i = 0; i < mod->export_count; i++) {
        const AjsExportEntry *ee = &mod->exports[i];
        if (!ee->module_specifier && ee->export_name && ajs_str_eq(ee->export_name, export_name)) {
            /* Check if `local_name` was actually an `import * as ns from "child"` re-exported via `export { ns }` */
            if (ee->local_name) {
                for (uint32_t k = 0; k < mod->import_count; k++) {
                    const AjsImportEntry *ie = &mod->imports[k];
                    if (ajs_str_eq(ie->local_name, ee->local_name) &&
                        ie->import_name && ajs_str_eq_cstr(ie->import_name, "*")) {
                        AjsModuleRecord *cmod = mod_find_loaded_child(mod, ie->module_specifier);
                        if (!cmod) return MOD_RES_NOT_FOUND;
                        out_binding->mod = cmod;
                        out_binding->slot = UINT32_MAX;
                        out_binding->is_namespace = true;
                        return MOD_RES_FOUND;
                    }
                }
            }
            if (!mod->bc || ee->local_slot >= mod->bc->env_slot_count) {
                return MOD_RES_NOT_FOUND;
            }
            out_binding->mod = mod;
            out_binding->slot = ee->local_slot;
            out_binding->is_namespace = false;
            return MOD_RES_FOUND;
        }
    }

    /* 2. Indirect named re-exports (`export { a as b } from "m"` or `export * as ns from "m"`) */
    for (uint32_t i = 0; i < mod->export_count; i++) {
        const AjsExportEntry *ee = &mod->exports[i];
        if (ee->module_specifier && ee->export_name && ajs_str_eq(ee->export_name, export_name)) {
            AjsModuleRecord *cmod = mod_find_loaded_child(mod, ee->module_specifier);
            if (!cmod) return MOD_RES_NOT_FOUND;
            if (ee->import_name && ajs_str_eq_cstr(ee->import_name, "*")) {
                out_binding->mod = cmod;
                out_binding->slot = UINT32_MAX;
                out_binding->is_namespace = true;
                return MOD_RES_FOUND;
            }
            return mod_resolve_export_rec(vm, cmod, ee->import_name, set, out_binding);
        }
    }

    /* 3. `"default"` is NEVER resolved through `export * from "m"` (§16.2.1.6.3 step 7) */
    if (ajs_str_eq(export_name, vm->str_default)) {
        return MOD_RES_NOT_FOUND;
    }

    /* 4. Star re-exports (`export * from "m"`) */
    bool has_star = false;
    ModResolvedBinding star_res = { NULL, 0, false };
    for (uint32_t i = 0; i < mod->export_count; i++) {
        const AjsExportEntry *ee = &mod->exports[i];
        if (ee->module_specifier && !ee->export_name) {
            AjsModuleRecord *cmod = mod_find_loaded_child(mod, ee->module_specifier);
            if (!cmod) continue;
            ModResolvedBinding sub = { NULL, 0, false };
            ModResolveStatus st = mod_resolve_export_rec(vm, cmod, export_name, set, &sub);
            if (st == MOD_RES_AMBIGUOUS) return MOD_RES_AMBIGUOUS;
            if (st == MOD_RES_FOUND) {
                if (!has_star) {
                    has_star = true;
                    star_res = sub;
                } else if (!same_resolved_binding(&star_res, &sub)) {
                    return MOD_RES_AMBIGUOUS;
                }
            }
        }
    }

    if (has_star) {
        *out_binding = star_res;
        return MOD_RES_FOUND;
    }
    return MOD_RES_NOT_FOUND;
}

static ModResolveStatus mod_resolve_export(AjsVm *vm, AjsModuleRecord *mod,
                                           const AjsString *export_name,
                                           ModResolvedBinding *out_binding) {
    ModResolveSet set = { NULL, 0, 0 };
    ModResolveStatus st = mod_resolve_export_rec(vm, mod, export_name, &set, out_binding);
    free(set.items);
    return st;
}

typedef struct ModStarSet {
    const AjsModuleRecord **mods;
    uint32_t                count;
    uint32_t                cap;
} ModStarSet;

typedef struct ModNameList {
    AjsString **names;
    uint32_t    count;
    uint32_t    cap;
} ModNameList;

static bool name_list_add_unique(ModNameList *list, AjsString *s) {
    for (uint32_t i = 0; i < list->count; i++) {
        if (ajs_str_eq(list->names[i], s)) return true;
    }
    if (list->count >= list->cap) {
        uint32_t ncap = list->cap ? list->cap * 2u : 16u;
        AjsString **nn = (AjsString **)realloc(list->names, (size_t)ncap * sizeof(AjsString *));
        if (!nn) return false;
        list->names = nn;
        list->cap = ncap;
    }
    list->names[list->count++] = s;
    return true;
}

static void mod_collect_exported_names(AjsVm *vm, const AjsModuleRecord *mod,
                                       ModStarSet *star_set, ModNameList *out_names) {
    if (!mod) return;
    for (uint32_t i = 0; i < star_set->count; i++) {
        if (star_set->mods[i] == mod) return;
    }
    if (star_set->count >= star_set->cap) {
        uint32_t ncap = star_set->cap ? star_set->cap * 2u : 16u;
        const AjsModuleRecord **nm = (const AjsModuleRecord **)realloc(
            star_set->mods, (size_t)ncap * sizeof(const AjsModuleRecord *));
        if (!nm) return;
        star_set->mods = nm;
        star_set->cap = ncap;
    }
    star_set->mods[star_set->count++] = mod;

    for (uint32_t i = 0; i < mod->export_count; i++) {
        const AjsExportEntry *ee = &mod->exports[i];
        if (ee->export_name) {
            name_list_add_unique(out_names, ee->export_name);
        }
    }
    for (uint32_t i = 0; i < mod->export_count; i++) {
        const AjsExportEntry *ee = &mod->exports[i];
        if (ee->module_specifier && !ee->export_name) {
            AjsModuleRecord *cmod = mod_find_loaded_child(mod, ee->module_specifier);
            if (!cmod) continue;
            ModNameList child_names = { NULL, 0, 0 };
            mod_collect_exported_names(vm, cmod, star_set, &child_names);
            for (uint32_t j = 0; j < child_names.count; j++) {
                if (!ajs_str_eq(child_names.names[j], vm->str_default)) {
                    name_list_add_unique(out_names, child_names.names[j]);
                }
            }
            free(child_names.names);
        }
    }
}

static int cmp_str_ptr_utf16(const void *pa, const void *pb) {
    const AjsString *a = *(const AjsString *const *)pa;
    const AjsString *b = *(const AjsString *const *)pb;
    return ajs_str_cmp_utf16(a, b);
}

/* ============================================================================
 * 5. Module Namespace Exotic Object (ECMA-262 §10.4.6 & §16.2.1.10)
 * ============================================================================ */

AjsValue ajs_module_namespace(AjsVm *vm, AjsModuleRecord *mod) {
    if (!vm || !mod) return AJS_VAL_UNDEFINED;
    if (mod->namespace_obj != AJS_VAL_UNDEFINED) {
        ajs_handle_record(vm, mod->namespace_obj);
        return mod->namespace_obj;
    }

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(mod));

    ModStarSet star_set = { NULL, 0, 0 };
    ModNameList all_names = { NULL, 0, 0 };
    mod_collect_exported_names(vm, mod, &star_set, &all_names);
    free(star_set.mods);

    AjsString **unambiguous = NULL;
    uint32_t ucount = 0;
    if (all_names.count > 0) {
        unambiguous = (AjsString **)malloc((size_t)all_names.count * sizeof(AjsString *));
        if (unambiguous) {
            for (uint32_t i = 0; i < all_names.count; i++) {
                ModResolvedBinding rb;
                if (mod_resolve_export(vm, mod, all_names.names[i], &rb) == MOD_RES_FOUND) {
                    unambiguous[ucount++] = ajs_str_intern(vm, all_names.names[i]);
                }
            }
            if (ucount > 1) {
                qsort(unambiguous, (size_t)ucount, sizeof(AjsString *), cmp_str_ptr_utf16);
            }
        }
    }
    free(all_names.names);

    AjsRealm *r = mod->realm ? mod->realm : (vm->active_realm ? vm->active_realm : vm->main_realm);
    AjsObject *ns = ajs_obj_new(vm, r, AJS_OBJ_MODULE_NS, AJS_VAL_NULL);
    if (!ns) {
        free(unambiguous);
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    ns->extensible = false;
    ns->u.mod_ns.module = mod;
    ns->u.mod_ns.export_count = ucount;
    ns->u.mod_ns.export_names = unambiguous;

    mod->namespace_obj = ajs_val_from_gc(ns);
    ajs_temp_root_restore(vm, rs);
    ajs_handle_record(vm, mod->namespace_obj);
    return mod->namespace_obj;
}

static int32_t mod_ns_find_export(const AjsObject *ns_obj, const AjsString *key_str) {
    uint32_t lo = 0, hi = ns_obj->u.mod_ns.export_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        int cmp = ajs_str_cmp_utf16(ns_obj->u.mod_ns.export_names[mid], key_str);
        if (cmp == 0) return (int32_t)mid;
        if (cmp < 0) lo = mid + 1u;
        else hi = mid;
    }
    return -1;
}

bool ajs_module_ns_has(AjsVm *vm, AjsObject *ns_obj, AjsValue key, bool *out_has) {
    (void)vm;
    *out_has = false;
    if (!ns_obj || ns_obj->kind != AJS_OBJ_MODULE_NS) return true;
    if (!ajs_val_is_gc_kind(key, AJS_GC_STRING)) return true;
    const AjsString *ks = (const AjsString *)ajs_val_to_gc(key);
    *out_has = (mod_ns_find_export(ns_obj, ks) >= 0);
    return true;
}

AjsValue ajs_module_ns_get(AjsVm *vm, AjsObject *ns_obj, AjsValue key) {
    if (!ns_obj || ns_obj->kind != AJS_OBJ_MODULE_NS) return AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(key, AJS_GC_STRING)) return AJS_VAL_UNDEFINED;
    const AjsString *ks = (const AjsString *)ajs_val_to_gc(key);
    if (mod_ns_find_export(ns_obj, ks) < 0) return AJS_VAL_UNDEFINED;

    ModResolvedBinding rb;
    if (mod_resolve_export(vm, ns_obj->u.mod_ns.module, ks, &rb) != MOD_RES_FOUND) {
        return AJS_VAL_UNDEFINED;
    }
    if (rb.is_namespace) {
        return ajs_module_namespace(vm, rb.mod);
    }
    if (!rb.mod || !rb.mod->env || rb.slot >= rb.mod->env->count) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE,
                                  "Cannot access '%s' before initialization", ks->utf8);
    }
    AjsValue val = rb.mod->env->slots[rb.slot];
    if (val == AJS_VAL_TDZ) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_REFERENCE,
                                  "Cannot access '%s' before initialization", ks->utf8);
    }
    return val;
}

/* ============================================================================
 * 6. LoadRequestedModules & Tarjan SCC Link() (ECMA-262 §16.2.1.5.1)
 * ============================================================================ */

typedef struct ModStack {
    AjsModuleRecord **items;
    uint32_t          count;
    uint32_t          cap;
} ModStack;

static bool mod_stack_push(ModStack *st, AjsModuleRecord *m) {
    for (uint32_t i = 0; i < st->count; i++) {
        if (st->items[i] == m) return true;
    }
    if (st->count >= st->cap) {
        uint32_t ncap = st->cap ? st->cap * 2u : 16u;
        AjsModuleRecord **ni = (AjsModuleRecord **)realloc(
            st->items, (size_t)ncap * sizeof(AjsModuleRecord *));
        if (!ni) return false;
        st->items = ni;
        st->cap = ncap;
    }
    st->items[st->count++] = m;
    return true;
}

static bool mod_load_requested_dfs(AjsVm *vm, AjsModuleRecord *mod,
                                   ModStack *visited, AjsErrorInfo *out_err) {
    if (!mod) return false;
    for (uint32_t i = 0; i < visited->count; i++) {
        if (visited->items[i] == mod) return true;
    }
    if (!mod_stack_push(visited, mod)) return false;

    if (mod->req_count == 0) return true;
    if (!mod->loaded_modules) {
        mod->loaded_modules = (AjsModuleRecord **)calloc(
            (size_t)mod->req_count, sizeof(AjsModuleRecord *));
        if (!mod->loaded_modules) return false;
    }

    for (uint32_t i = 0; i < mod->req_count; i++) {
        if (mod->loaded_modules[i]) {
            if (!mod_load_requested_dfs(vm, mod->loaded_modules[i], visited, out_err)) return false;
            continue;
        }

        const char *spec = mod->requested_specifiers[i]->utf8;
        if (!vm->cfg.module_resolve_fn || !vm->cfg.module_load_fn) {
            mod_set_err_info(vm, AJS_ERR_RESOLUTION, "SyntaxError", mod->url, 1, 1, out_err,
                             "Cannot resolve module '%s': no host module loader configured", spec);
            return false;
        }

        char resolved_url[1024];
        char err_msg[512];
        resolved_url[0] = '\0';
        err_msg[0] = '\0';
        if (!vm->cfg.module_resolve_fn(vm, spec, mod->url ? mod->url : "",
                                       resolved_url, sizeof(resolved_url),
                                       err_msg, sizeof(err_msg), vm->cfg.module_userdata)) {
            mod_set_err_info(vm, AJS_ERR_RESOLUTION, "SyntaxError", mod->url, 1, 1, out_err,
                             "%s", err_msg[0] ? err_msg : "Cannot resolve module specifier");
            return false;
        }

        AjsModuleRecord *child = mod_cache_find(vm, mod->realm, resolved_url);
        if (!child) {
            vm->last_err_info.phase = AJS_ERR_NONE;
            err_msg[0] = '\0';
            child = vm->cfg.module_load_fn(vm, resolved_url, mod->url ? mod->url : "",
                                           err_msg, sizeof(err_msg), vm->cfg.module_userdata);
            if (!child) {
                if (vm->has_exception) {
                    AjsErrorInfo tmp;
                    ajs_take_exception(vm, &tmp);
                    if (tmp.phase == AJS_ERR_NONE) tmp.phase = AJS_ERR_RESOLUTION;
                    vm->last_err_info = tmp;
                    if (out_err) *out_err = tmp;
                } else if (vm->last_err_info.phase != AJS_ERR_NONE) {
                    /* Preserve parse error vs resolution error from nested ajs_module_parse */
                    if (out_err) *out_err = vm->last_err_info;
                } else {
                    mod_set_err_info(vm, AJS_ERR_RESOLUTION, "SyntaxError", mod->url, 1, 1, out_err,
                                     "%s", err_msg[0] ? err_msg : "Failed to load module");
                }
                return false;
            }
            mod_cache_register(vm, child);
        }

        mod->loaded_modules[i] = child;
        if (!mod_load_requested_dfs(vm, child, visited, out_err)) return false;
    }
    return true;
}

static bool mod_initialize_environment(AjsVm *vm, AjsModuleRecord *mod, AjsErrorInfo *out_err) {
    /* 1. Validate all indirect exports (`export { x as y } from "m"`) */
    for (uint32_t i = 0; i < mod->export_count; i++) {
        const AjsExportEntry *ee = &mod->exports[i];
        if (ee->export_name && ee->module_specifier) {
            AjsModuleRecord *cmod = mod_find_loaded_child(mod, ee->module_specifier);
            if (!cmod) {
                mod_set_err_info(vm, AJS_ERR_RESOLUTION, "SyntaxError", mod->url, ee->line, ee->col,
                                 out_err, "Unloaded module '%s'", ee->module_specifier->utf8);
                return false;
            }
            if (ee->import_name && ajs_str_eq_cstr(ee->import_name, "*")) continue;
            ModResolvedBinding rb;
            ModResolveStatus st = mod_resolve_export(vm, cmod, ee->import_name, &rb);
            if (st == MOD_RES_NOT_FOUND) {
                mod_set_err_info(vm, AJS_ERR_RESOLUTION, "SyntaxError", mod->url, ee->line, ee->col,
                                 out_err, "The requested module '%s' does not provide an export named '%s'",
                                 ee->module_specifier->utf8, ee->import_name ? ee->import_name->utf8 : "");
                return false;
            }
            if (st == MOD_RES_AMBIGUOUS) {
                mod_set_err_info(vm, AJS_ERR_RESOLUTION, "SyntaxError", mod->url, ee->line, ee->col,
                                 out_err, "The requested module '%s' contains conflicting star exports for '%s'",
                                 ee->module_specifier->utf8, ee->import_name ? ee->import_name->utf8 : "");
                return false;
            }
        }
    }

    /* 2. Allocate module lexical environment (`mod->env`) */
    AjsBytecode *bc = mod->bc;
    uint32_t scnt = bc ? bc->env_slot_count : 0;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(mod));

    AjsEnv *env = ajs_env_new(vm, NULL, scnt, bc ? bc->env_slot_names : NULL);
    if (!env) {
        ajs_temp_root_restore(vm, rs);
        if (vm->has_exception) ajs_take_exception(vm, out_err);
        return false;
    }
    mod->env = env;
    ajs_temp_root_push(vm, ajs_val_from_gc(env));

    if (bc && bc->env_slot_flags) {
        for (uint32_t i = 0; i < scnt; i++) {
            uint8_t fl = bc->env_slot_flags[i];
            env->flags[i] = (uint8_t)(fl & (AJS_SLOT_CONST | AJS_SLOT_FUNC_NAME));
            env->slots[i] = (fl & AJS_SLOT_INIT_TDZ) ? AJS_VAL_TDZ : AJS_VAL_UNDEFINED;
        }
    }
    if (bc && bc->this_slot != AJS_NO_SLOT && bc->this_slot < scnt) {
        env->slots[bc->this_slot] = AJS_VAL_UNDEFINED;
    }

    /* 3. Resolve & link all imports (`mod->imports`) */
    if (mod->import_count > 0 && scnt > 0) {
        env->import_links = (AjsImportLink *)calloc((size_t)scnt, sizeof(AjsImportLink));
        if (!env->import_links) {
            ajs_temp_root_restore(vm, rs);
            return false;
        }
        for (uint32_t i = 0; i < mod->import_count; i++) {
            const AjsImportEntry *ie = &mod->imports[i];
            if (!ie->import_name || ie->local_slot >= scnt) continue;
            AjsModuleRecord *cmod = mod_find_loaded_child(mod, ie->module_specifier);
            if (!cmod) {
                ajs_temp_root_restore(vm, rs);
                mod_set_err_info(vm, AJS_ERR_RESOLUTION, "SyntaxError", mod->url, ie->line, ie->col,
                                 out_err, "Unloaded module '%s'", ie->module_specifier->utf8);
                return false;
            }
            if (ajs_str_eq_cstr(ie->import_name, "*")) {
                AjsValue ns_val = ajs_module_namespace(vm, cmod);
                if (vm->has_exception) {
                    ajs_take_exception(vm, out_err);
                    ajs_temp_root_restore(vm, rs);
                    return false;
                }
                env->flags[ie->local_slot] |= (AJS_SLOT_CONST | AJS_SLOT_IMPORT_LINK);
                env->slots[ie->local_slot] = ns_val;
                env->import_links[ie->local_slot].target_mod = cmod;
                env->import_links[ie->local_slot].target_slot = UINT32_MAX;
                env->import_links[ie->local_slot].is_namespace = true;
            } else {
                ModResolvedBinding rb;
                ModResolveStatus st = mod_resolve_export(vm, cmod, ie->import_name, &rb);
                if (st == MOD_RES_NOT_FOUND) {
                    ajs_temp_root_restore(vm, rs);
                    mod_set_err_info(vm, AJS_ERR_RESOLUTION, "SyntaxError", mod->url, ie->line, ie->col,
                                     out_err, "The requested module '%s' does not provide an export named '%s'",
                                     ie->module_specifier->utf8, ie->import_name->utf8);
                    return false;
                }
                if (st == MOD_RES_AMBIGUOUS) {
                    ajs_temp_root_restore(vm, rs);
                    mod_set_err_info(vm, AJS_ERR_RESOLUTION, "SyntaxError", mod->url, ie->line, ie->col,
                                     out_err, "The requested module '%s' contains conflicting star exports for '%s'",
                                     ie->module_specifier->utf8, ie->import_name->utf8);
                    return false;
                }
                env->flags[ie->local_slot] |= (AJS_SLOT_CONST | AJS_SLOT_IMPORT_LINK);
                env->slots[ie->local_slot] = AJS_VAL_UNDEFINED;
                env->import_links[ie->local_slot].target_mod = rb.mod;
                env->import_links[ie->local_slot].target_slot = rb.is_namespace ? UINT32_MAX : rb.slot;
                env->import_links[ie->local_slot].is_namespace = rb.is_namespace;
            }
        }
    }

    /* 4. Instantiate hoisted module function declarations (`mod->hoisted_fns`) */
    AjsRealm *r = mod->realm ? mod->realm : (vm->active_realm ? vm->active_realm : vm->main_realm);
    for (uint32_t i = 0; i < mod->hoisted_fn_count; i++) {
        uint32_t slot = mod->hoisted_fns[i].slot;
        AjsBytecode *fn_bc = mod->hoisted_fns[i].fn_bc;
        if (fn_bc && slot < scnt) {
            AjsObject *fn_obj = ajs_closure_new(vm, r, fn_bc, env, mod);
            if (!fn_obj) {
                ajs_temp_root_restore(vm, rs);
                if (vm->has_exception) ajs_take_exception(vm, out_err);
                return false;
            }
            env->slots[slot] = ajs_val_from_gc(fn_obj);
        }
    }

    ajs_temp_root_restore(vm, rs);
    return true;
}

static bool mod_inner_linking(AjsVm *vm, AjsModuleRecord *mod, ModStack *stack,
                              uint32_t *io_index, AjsErrorInfo *out_err) {
    if (mod->status == AJS_MOD_LINKING ||
        mod->status == AJS_MOD_LINKED ||
        mod->status == AJS_MOD_EVALUATING_ASYNC ||
        mod->status == AJS_MOD_EVALUATED) {
        return true;
    }

    mod->status = AJS_MOD_LINKING;
    mod->dfs_index = *io_index;
    mod->dfs_ancestor_index = *io_index;
    (*io_index)++;

    if (stack->count >= stack->cap) {
        uint32_t ncap = stack->cap ? stack->cap * 2u : 16u;
        AjsModuleRecord **ni = (AjsModuleRecord **)realloc(
            stack->items, (size_t)ncap * sizeof(AjsModuleRecord *));
        if (!ni) return false;
        stack->items = ni;
        stack->cap = ncap;
    }
    stack->items[stack->count++] = mod;

    for (uint32_t i = 0; i < mod->req_count; i++) {
        AjsModuleRecord *child = mod->loaded_modules[i];
        if (!mod_inner_linking(vm, child, stack, io_index, out_err)) {
            return false;
        }
        if (child->status == AJS_MOD_LINKING &&
            child->dfs_ancestor_index < mod->dfs_ancestor_index) {
            mod->dfs_ancestor_index = child->dfs_ancestor_index;
        }
    }

    if (!mod_initialize_environment(vm, mod, out_err)) {
        return false;
    }

    if (mod->dfs_ancestor_index == mod->dfs_index) {
        while (stack->count > 0) {
            AjsModuleRecord *w = stack->items[--stack->count];
            w->status = AJS_MOD_LINKED;
            if (w == mod) break;
        }
    }
    return true;
}

bool ajs_module_link(AjsVm *vm, AjsModuleRecord *mod, AjsErrorInfo *out_err_or_null) {
    if (out_err_or_null) memset(out_err_or_null, 0, sizeof(*out_err_or_null));
    if (!vm || !mod) return false;

    if (mod->status == AJS_MOD_LINKED ||
        mod->status == AJS_MOD_EVALUATING_ASYNC ||
        mod->status == AJS_MOD_EVALUATED) {
        return true;
    }
    if (mod->status == AJS_MOD_LINKING || mod->status == AJS_MOD_EVALUATING) {
        mod_set_err_info(vm, AJS_ERR_RESOLUTION, "TypeError", mod->url, 1, 1,
                         out_err_or_null, "Cannot re-enter Link on active module");
        return false;
    }

    mod_cache_register(vm, mod);

    /* Phase 1: LoadRequestedModules across reachable graph */
    ModStack visited = { NULL, 0, 0 };
    if (!mod_load_requested_dfs(vm, mod, &visited, out_err_or_null)) {
        free(visited.items);
        return false;
    }
    free(visited.items);

    /* Phase 2: Tarjan SCC InnerModuleLinking */
    ModStack stack = { NULL, 0, 0 };
    uint32_t index = 0;
    bool ok = mod_inner_linking(vm, mod, &stack, &index, out_err_or_null);
    if (!ok) {
        for (uint32_t i = 0; i < stack.count; i++) {
            AjsModuleRecord *w = stack.items[i];
            w->status = AJS_MOD_UNLINKED;
            w->env = NULL;
            w->dfs_index = 0;
            w->dfs_ancestor_index = 0;
        }
    }
    free(stack.items);
    return ok;
}

/* ============================================================================
 * 7. Tarjan SCC Evaluate() & Top-Level Await (ECMA-262 §16.2.1.5.2)
 * ============================================================================ */

static void mod_async_exec_fulfilled(AjsVm *vm, AjsModuleRecord *mod);
static void mod_async_exec_rejected(AjsVm *vm, AjsModuleRecord *mod, AjsValue error);
static void mod_execute_async_module(AjsVm *vm, AjsModuleRecord *mod);

static bool mod_add_async_parent(AjsModuleRecord *child, AjsModuleRecord *parent) {
    for (uint32_t i = 0; i < child->async_parent_count; i++) {
        if (child->async_parents[i] == parent) return true;
    }
    if (child->async_parent_count >= child->async_parent_cap) {
        uint32_t ncap = child->async_parent_cap ? child->async_parent_cap * 2u : 4u;
        AjsModuleRecord **np = (AjsModuleRecord **)realloc(
            child->async_parents, (size_t)ncap * sizeof(AjsModuleRecord *));
        if (!np) return false;
        child->async_parents = np;
        child->async_parent_cap = ncap;
    }
    child->async_parents[child->async_parent_count++] = parent;
    return true;
}

typedef struct ModExecList {
    AjsModuleRecord **items;
    uint32_t          count;
    uint32_t          cap;
} ModExecList;

static void mod_gather_available_ancestors(AjsModuleRecord *mod, ModExecList *out) {
    for (uint32_t i = 0; i < mod->async_parent_count; i++) {
        AjsModuleRecord *p = mod->async_parents[i];
        if (!p || p->status == AJS_MOD_EVALUATED) continue;
        if (p->cycle_root && p->cycle_root->has_eval_error) continue;
        if (p->pending_async_deps > 0) p->pending_async_deps--;
        if (p->pending_async_deps == 0) {
            bool dup = false;
            for (uint32_t j = 0; j < out->count; j++) {
                if (out->items[j] == p) { dup = true; break; }
            }
            if (!dup) {
                if (out->count >= out->cap) {
                    uint32_t ncap = out->cap ? out->cap * 2u : 8u;
                    AjsModuleRecord **ni = (AjsModuleRecord **)realloc(
                        out->items, (size_t)ncap * sizeof(AjsModuleRecord *));
                    if (!ni) return;
                    out->items = ni;
                    out->cap = ncap;
                }
                out->items[out->count++] = p;
            }
            if (!p->has_tla) {
                mod_gather_available_ancestors(p, out);
            }
        }
    }
}

static int cmp_async_eval_order(const void *pa, const void *pb) {
    const AjsModuleRecord *a = *(const AjsModuleRecord *const *)pa;
    const AjsModuleRecord *b = *(const AjsModuleRecord *const *)pb;
    if (a->async_eval_order < b->async_eval_order) return -1;
    if (a->async_eval_order > b->async_eval_order) return 1;
    return 0;
}

static void mod_async_exec_rejected(AjsVm *vm, AjsModuleRecord *mod, AjsValue error) {
    if (!mod || mod->status == AJS_MOD_EVALUATED) return;

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(mod));
    ajs_temp_root_push(vm, error);

    mod->async_evaluation = false;
    mod->status = AJS_MOD_EVALUATED;
    mod->has_eval_error = true;
    mod->eval_error = error;

    if (mod->cycle_root && mod->cycle_root != mod) {
        mod->cycle_root->has_eval_error = true;
        mod->cycle_root->eval_error = error;
    }

    if (ajs_is_callable(vm, mod->top_reject_fn)) {
        ajs_call_internal(vm, mod->top_reject_fn, AJS_VAL_UNDEFINED, 1, &error);
        ajs_clear_exception(vm);
    }

    for (uint32_t i = 0; i < mod->async_parent_count; i++) {
        mod_async_exec_rejected(vm, mod->async_parents[i], error);
    }

    ajs_temp_root_restore(vm, rs);
}

static void mod_async_exec_fulfilled(AjsVm *vm, AjsModuleRecord *mod) {
    if (!mod || mod->status == AJS_MOD_EVALUATED) return;

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(mod));

    mod->async_evaluation = false;
    mod->status = AJS_MOD_EVALUATED;

    if (ajs_is_callable(vm, mod->top_resolve_fn)) {
        AjsValue u = AJS_VAL_UNDEFINED;
        ajs_call_internal(vm, mod->top_resolve_fn, AJS_VAL_UNDEFINED, 1, &u);
        ajs_clear_exception(vm);
    }

    ModExecList exec_list = { NULL, 0, 0 };
    mod_gather_available_ancestors(mod, &exec_list);
    if (exec_list.count > 1) {
        qsort(exec_list.items, (size_t)exec_list.count, sizeof(AjsModuleRecord *), cmp_async_eval_order);
    }

    for (uint32_t i = 0; i < exec_list.count; i++) {
        AjsModuleRecord *m = exec_list.items[i];
        if (m->status == AJS_MOD_EVALUATED) continue;
        if (m->has_tla) {
            mod_execute_async_module(vm, m);
        } else {
            bool ok = ajs_exec_module_bc(vm, m);
            if (!ok || vm->has_exception) {
                AjsValue exc = vm->exception;
                ajs_clear_exception(vm);
                mod_async_exec_rejected(vm, m, exc);
            } else {
                m->async_evaluation = false;
                m->status = AJS_MOD_EVALUATED;
                if (ajs_is_callable(vm, m->top_resolve_fn)) {
                    AjsValue u = AJS_VAL_UNDEFINED;
                    ajs_call_internal(vm, m->top_resolve_fn, AJS_VAL_UNDEFINED, 1, &u);
                    ajs_clear_exception(vm);
                }
            }
        }
    }

    free(exec_list.items);
    ajs_temp_root_restore(vm, rs);
}

/* Callback wired into `gobj->u.gen.async_resolve` / `async_reject` during TLA `ajs_exec_module_bc`:
 * env->slots = [ajs_val_from_gc(mod), orig_top_resolve_fn, orig_top_reject_fn],
 * magic = 0 (fulfilled) or 1 (rejected). */
static AjsValue mod_tla_settle_cb(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsEnv *env = mod_current_fn_env(vm);
    if (!env || env->count < 3) return AJS_VAL_UNDEFINED;
    bool is_reject = (mod_current_fn_magic(vm) != 0);
    AjsModuleRecord *mod = (AjsModuleRecord *)ajs_val_to_gc(env->slots[0]);
    AjsValue arg = (argc > 0) ? argv[0] : AJS_VAL_UNDEFINED;

    /* Ensure `mod->top_resolve_fn` / `top_reject_fn` hold the outer top-level capability */
    mod->top_resolve_fn = env->slots[1];
    mod->top_reject_fn  = env->slots[2];

    if (mod->status == AJS_MOD_EVALUATING) {
        /* Settled synchronously before Tarjan SCC popped: defer until microtask checkpoint! */
        AjsObject *self_fn = vm->call_stack[vm->call_depth - 1u].native_fn_obj;
        if (self_fn) {
            ajs_enqueue_job_ex(vm, mod->realm, ajs_val_from_gc(self_fn), is_reject ? 1 : 0, &arg);
        }
        return AJS_VAL_UNDEFINED;
    }

    if (is_reject) {
        mod_async_exec_rejected(vm, mod, arg);
    } else {
        mod_async_exec_fulfilled(vm, mod);
    }
    return AJS_VAL_UNDEFINED;
}

static void mod_execute_async_module(AjsVm *vm, AjsModuleRecord *mod) {
    AjsRealm *r = mod->realm ? mod->realm : (vm->active_realm ? vm->active_realm : vm->main_realm);
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(mod));

    AjsValue saved_res = mod->top_resolve_fn;
    AjsValue saved_rej = mod->top_reject_fn;
    ajs_temp_root_push(vm, saved_res);
    ajs_temp_root_push(vm, saved_rej);

    AjsEnv *cb_env = ajs_env_new(vm, NULL, 3, NULL);
    if (!cb_env) {
        ajs_temp_root_restore(vm, rs);
        return;
    }
    cb_env->slots[0] = ajs_val_from_gc(mod);
    cb_env->slots[1] = saved_res;
    cb_env->slots[2] = saved_rej;
    ajs_temp_root_push(vm, ajs_val_from_gc(cb_env));

    AjsObject *ok_cb = mod_make_closure_fn(vm, r, mod_tla_settle_cb, 0, cb_env);
    AjsObject *err_cb = mod_make_closure_fn(vm, r, mod_tla_settle_cb, 1, cb_env);
    if (!ok_cb || !err_cb) {
        ajs_temp_root_restore(vm, rs);
        return;
    }
    ajs_temp_root_push(vm, ajs_val_from_gc(ok_cb));
    ajs_temp_root_push(vm, ajs_val_from_gc(err_cb));

    /* Temporarily place `ok_cb` / `err_cb` into `mod->top_resolve_fn` / `mod->top_reject_fn`
     * so `ajs_exec_module_bc` captures them in `gobj->u.gen.async_resolve` / `async_reject`. */
    mod->top_resolve_fn = ajs_val_from_gc(ok_cb);
    mod->top_reject_fn  = ajs_val_from_gc(err_cb);

    bool ok = ajs_exec_module_bc(vm, mod);
    mod->top_resolve_fn = saved_res;
    mod->top_reject_fn  = saved_rej;

    if (!ok || vm->has_exception) {
        AjsValue exc = vm->exception;
        ajs_temp_root_push(vm, exc);
        ajs_clear_exception(vm);
        if (mod->status == AJS_MOD_EVALUATING) {
            ajs_enqueue_job_ex(vm, r, ajs_val_from_gc(err_cb), 1, &exc);
        } else {
            mod_async_exec_rejected(vm, mod, exc);
        }
    }

    ajs_temp_root_restore(vm, rs);
}

static bool mod_inner_evaluation(AjsVm *vm, AjsModuleRecord *mod, ModStack *stack,
                                 uint32_t *io_index, AjsValue *out_err_val) {
    if (mod->status == AJS_MOD_EVALUATING_ASYNC || mod->status == AJS_MOD_EVALUATED) {
        if (mod->has_eval_error) {
            *out_err_val = mod->eval_error;
            return false;
        }
        return true;
    }
    if (mod->status == AJS_MOD_EVALUATING) {
        return true;
    }

    mod->status = AJS_MOD_EVALUATING;
    mod->dfs_index = *io_index;
    mod->dfs_ancestor_index = *io_index;
    mod->pending_async_deps = 0;
    (*io_index)++;

    if (stack->count >= stack->cap) {
        uint32_t ncap = stack->cap ? stack->cap * 2u : 16u;
        AjsModuleRecord **ni = (AjsModuleRecord **)realloc(
            stack->items, (size_t)ncap * sizeof(AjsModuleRecord *));
        if (!ni) return false;
        stack->items = ni;
        stack->cap = ncap;
    }
    stack->items[stack->count++] = mod;

    for (uint32_t i = 0; i < mod->req_count; i++) {
        AjsModuleRecord *req = mod->loaded_modules[i];
        if (!mod_inner_evaluation(vm, req, stack, io_index, out_err_val)) {
            return false;
        }
        if (req->status == AJS_MOD_EVALUATING) {
            if (req->dfs_ancestor_index < mod->dfs_ancestor_index) {
                mod->dfs_ancestor_index = req->dfs_ancestor_index;
            }
        } else {
            AjsModuleRecord *root = req->cycle_root ? req->cycle_root : req;
            if (root->has_eval_error) {
                *out_err_val = root->eval_error;
                return false;
            }
            if (root->async_evaluation) {
                mod->pending_async_deps++;
                mod_add_async_parent(root, mod);
            }
        }
    }

    if (mod->pending_async_deps > 0 || mod->has_tla) {
        mod->async_evaluation = true;
        mod->async_eval_order = ++vm->next_async_eval_order;
        if (mod->pending_async_deps == 0) {
            mod_execute_async_module(vm, mod);
        }
    } else {
        if (!ajs_exec_module_bc(vm, mod) || vm->has_exception) {
            *out_err_val = vm->exception;
            ajs_clear_exception(vm);
            return false;
        }
    }

    if (mod->dfs_ancestor_index == mod->dfs_index) {
        while (stack->count > 0) {
            AjsModuleRecord *w = stack->items[--stack->count];
            w->status = mod->async_evaluation ? AJS_MOD_EVALUATING_ASYNC : AJS_MOD_EVALUATED;
            w->cycle_root = mod;
            if (w == mod) break;
        }
    }
    return true;
}

bool ajs_module_evaluate(AjsVm *vm, AjsModuleRecord *mod,
                         AjsValue *out_promise_or_val, AjsErrorInfo *out_err_or_null) {
    if (out_promise_or_val) *out_promise_or_val = AJS_VAL_UNDEFINED;
    if (out_err_or_null) memset(out_err_or_null, 0, sizeof(*out_err_or_null));
    if (!vm || !mod) return false;

    if (mod->status == AJS_MOD_UNLINKED) {
        if (!ajs_module_link(vm, mod, out_err_or_null)) return false;
    }

    if ((mod->status == AJS_MOD_EVALUATING_ASYNC || mod->status == AJS_MOD_EVALUATED) && mod->cycle_root) {
        mod = mod->cycle_root;
    }

    if (mod->top_capability_promise != AJS_VAL_UNDEFINED) {
        if (out_promise_or_val) {
            ajs_handle_record(vm, mod->top_capability_promise);
            *out_promise_or_val = mod->top_capability_promise;
        }
        if (mod->has_eval_error) {
            ajs_fill_error_info(vm, mod->eval_error, AJS_ERR_RUNTIME, out_err_or_null);
            if (out_err_or_null) vm->last_err_info = *out_err_or_null;
            return false;
        }
        return true;
    }

    AjsRealm *saved_realm = vm->active_realm;
    if (mod->realm) vm->active_realm = mod->realm;

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(mod));

    AjsValue res_fn = AJS_VAL_UNDEFINED, rej_fn = AJS_VAL_UNDEFINED;
    AjsValue cap_prom = ajs_promise_create(vm, &res_fn, &rej_fn);
    if (vm->has_exception || cap_prom == AJS_VAL_EXCEPTION) {
        ajs_take_exception(vm, out_err_or_null);
        ajs_temp_root_restore(vm, rs);
        vm->active_realm = saved_realm;
        return false;
    }

    mod->top_capability_promise = cap_prom;
    mod->top_resolve_fn = res_fn;
    mod->top_reject_fn  = rej_fn;

    ModStack stack = { NULL, 0, 0 };
    uint32_t index = 0;
    AjsValue err_val = AJS_VAL_UNDEFINED;

    bool ok = mod_inner_evaluation(vm, mod, &stack, &index, &err_val);
    if (!ok) {
        ajs_temp_root_push(vm, err_val);
        for (uint32_t i = 0; i < stack.count; i++) {
            AjsModuleRecord *w = stack.items[i];
            w->status = AJS_MOD_EVALUATED;
            w->async_evaluation = false;
            w->has_eval_error = true;
            w->eval_error = err_val;
            w->cycle_root = mod;
        }
        mod->status = AJS_MOD_EVALUATED;
        mod->async_evaluation = false;
        mod->has_eval_error = true;
        mod->eval_error = err_val;

        if (ajs_val_is_gc_kind(cap_prom, AJS_GC_OBJECT)) {
            ((AjsObject *)ajs_val_to_gc(cap_prom))->u.promise.is_handled = true;
        }
        ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &err_val);
        ajs_clear_exception(vm);

        ajs_fill_error_info(vm, err_val, AJS_ERR_RUNTIME, out_err_or_null);
        if (out_err_or_null) vm->last_err_info = *out_err_or_null;
    } else if (!mod->async_evaluation) {
        AjsValue u = AJS_VAL_UNDEFINED;
        ajs_call_internal(vm, res_fn, AJS_VAL_UNDEFINED, 1, &u);
        ajs_clear_exception(vm);
    }

    free(stack.items);
    ajs_temp_root_restore(vm, rs);
    vm->active_realm = saved_realm;

    if (out_promise_or_val) {
        ajs_handle_record(vm, cap_prom);
        *out_promise_or_val = cap_prom;
    }
    return ok;
}

/* ============================================================================
 * 8. `import.meta` & Dynamic `import()` (ECMA-262 §13.3.10 & §13.3.12)
 * ============================================================================ */

AjsValue ajs_module_get_import_meta(AjsVm *vm, AjsModuleRecord *mod) {
    if (!vm) return AJS_VAL_UNDEFINED;
    if (mod && mod->import_meta_obj != AJS_VAL_UNDEFINED) {
        return mod->import_meta_obj;
    }

    AjsRealm *r = (mod && mod->realm) ? mod->realm : (vm->active_realm ? vm->active_realm : vm->main_realm);
    size_t rs = ajs_temp_root_save(vm);
    if (mod) ajs_temp_root_push(vm, ajs_val_from_gc(mod));

    AjsObject *meta = ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, AJS_VAL_NULL);
    if (!meta) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    AjsValue meta_val = ajs_val_from_gc(meta);
    ajs_temp_root_push(vm, meta_val);

    AjsString *s_url = ajs_str_from_cstr(vm, (mod && mod->url) ? mod->url : "", false);
    if (s_url) {
        ajs_temp_root_push(vm, ajs_val_from_gc(s_url));
        ajs_define_prop_str(vm, meta_val, "url", ajs_val_from_gc(s_url), AJS_PROP_DEFAULT);
    }

    if (mod) mod->import_meta_obj = meta_val;
    ajs_temp_root_restore(vm, rs);
    return meta_val;
}

/* Dynamic import post-evaluation continuation:
 * env->slots = [ajs_val_from_gc(child_mod), resolve_fn, reject_fn], magic = 0 (ok) or 1 (err) */
static AjsValue dyn_import_eval_done_cb(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsEnv *env = mod_current_fn_env(vm);
    if (!env || env->count < 3) return AJS_VAL_UNDEFINED;
    bool is_reject = (mod_current_fn_magic(vm) != 0);
    AjsModuleRecord *child = (AjsModuleRecord *)ajs_val_to_gc(env->slots[0]);
    AjsValue res_fn = env->slots[1];
    AjsValue rej_fn = env->slots[2];

    size_t rs = ajs_temp_root_save(vm);
    if (is_reject) {
        AjsValue reason = (argc > 0) ? argv[0] : AJS_VAL_UNDEFINED;
        ajs_temp_root_push(vm, reason);
        ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &reason);
        ajs_clear_exception(vm);
    } else {
        AjsValue ns = ajs_module_namespace(vm, child);
        if (vm->has_exception) {
            AjsValue exc = vm->exception;
            ajs_temp_root_push(vm, exc);
            ajs_clear_exception(vm);
            ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &exc);
            ajs_clear_exception(vm);
        } else {
            ajs_temp_root_push(vm, ns);
            ajs_call_internal(vm, res_fn, AJS_VAL_UNDEFINED, 1, &ns);
            ajs_clear_exception(vm);
        }
    }
    ajs_temp_root_restore(vm, rs);
    return AJS_VAL_UNDEFINED;
}

/* Dynamic import async microtask job:
 * env->slots = [spec_str, ref_url_str, resolve_fn, reject_fn] */
static AjsValue dyn_import_job_cb(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)argc; (void)argv; (void)ud;
    AjsEnv *env = mod_current_fn_env(vm);
    if (!env || env->count < 4) return AJS_VAL_UNDEFINED;

    AjsString *spec_s = (AjsString *)ajs_val_to_gc(env->slots[0]);
    AjsString *ref_s  = (AjsString *)ajs_val_to_gc(env->slots[1]);
    AjsValue res_fn   = env->slots[2];
    AjsValue rej_fn   = env->slots[3];
    AjsRealm *realm   = vm->active_realm ? vm->active_realm : vm->main_realm;

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, res_fn);
    ajs_temp_root_push(vm, rej_fn);

    if (!vm->cfg.module_resolve_fn || !vm->cfg.module_load_fn) {
        AjsValue err = mod_make_error_obj(vm, realm, AJS_ERR_KIND_TYPE, ref_s->utf8, 1, 1,
                                          "Dynamic import('%s') is not supported by host", spec_s->utf8);
        ajs_temp_root_push(vm, err);
        ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &err);
        ajs_clear_exception(vm);
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_UNDEFINED;
    }

    char resolved_url[1024];
    char err_msg[512];
    resolved_url[0] = '\0';
    err_msg[0] = '\0';
    if (!vm->cfg.module_resolve_fn(vm, spec_s->utf8, ref_s->utf8,
                                   resolved_url, sizeof(resolved_url),
                                   err_msg, sizeof(err_msg), vm->cfg.module_userdata)) {
        AjsValue err = mod_make_error_obj(vm, realm, AJS_ERR_KIND_TYPE, ref_s->utf8, 1, 1,
                                          "%s", err_msg[0] ? err_msg : "Cannot resolve dynamic import");
        ajs_temp_root_push(vm, err);
        ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &err);
        ajs_clear_exception(vm);
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_UNDEFINED;
    }

    AjsModuleRecord *child = mod_cache_find(vm, realm, resolved_url);
    if (!child) {
        vm->last_err_info.phase = AJS_ERR_NONE;
        err_msg[0] = '\0';
        child = vm->cfg.module_load_fn(vm, resolved_url, ref_s->utf8,
                                       err_msg, sizeof(err_msg), vm->cfg.module_userdata);
        if (!child) {
            AjsValue err = AJS_VAL_UNDEFINED;
            if (vm->has_exception) {
                err = vm->exception;
                ajs_clear_exception(vm);
            } else if (vm->last_err_info.phase == AJS_ERR_PARSE) {
                err = mod_make_error_obj(vm, realm, AJS_ERR_KIND_SYNTAX,
                                         vm->last_err_info.url, vm->last_err_info.line, vm->last_err_info.column,
                                         "%s", vm->last_err_info.message);
            } else {
                err = mod_make_error_obj(vm, realm, AJS_ERR_KIND_TYPE, ref_s->utf8, 1, 1,
                                         "%s", err_msg[0] ? err_msg : "Failed to load dynamic module");
            }
            ajs_temp_root_push(vm, err);
            ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &err);
            ajs_clear_exception(vm);
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_UNDEFINED;
        }
        mod_cache_register(vm, child);
    }
    ajs_temp_root_push(vm, ajs_val_from_gc(child));

    AjsErrorInfo link_err;
    if (!ajs_module_link(vm, child, &link_err)) {
        AjsErrKind ek = (strcmp(link_err.name, "TypeError") == 0) ? AJS_ERR_KIND_TYPE : AJS_ERR_KIND_SYNTAX;
        AjsValue err = mod_make_error_obj(vm, realm, ek, link_err.url, link_err.line, link_err.column,
                                          "%s", link_err.message);
        ajs_temp_root_push(vm, err);
        ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &err);
        ajs_clear_exception(vm);
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_UNDEFINED;
    }

    AjsValue eval_prom = AJS_VAL_UNDEFINED;
    AjsErrorInfo eval_err;
    (void)ajs_module_evaluate(vm, child, &eval_prom, &eval_err);
    ajs_temp_root_push(vm, eval_prom);

    if (!ajs_val_is_gc_kind(eval_prom, AJS_GC_OBJECT)) {
        AjsValue err = mod_make_error_obj(vm, realm, AJS_ERR_KIND_ERROR, child->url, 1, 1,
                                          "%s", eval_err.message[0] ? eval_err.message : "Module evaluation failed");
        ajs_temp_root_push(vm, err);
        ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &err);
        ajs_clear_exception(vm);
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_UNDEFINED;
    }

    AjsEnv *cont_env = ajs_env_new(vm, NULL, 3, NULL);
    if (cont_env) {
        cont_env->slots[0] = ajs_val_from_gc(child);
        cont_env->slots[1] = res_fn;
        cont_env->slots[2] = rej_fn;
        ajs_temp_root_push(vm, ajs_val_from_gc(cont_env));
        AjsObject *on_ok  = mod_make_closure_fn(vm, realm, dyn_import_eval_done_cb, 0, cont_env);
        AjsObject *on_err = mod_make_closure_fn(vm, realm, dyn_import_eval_done_cb, 1, cont_env);
        if (on_ok && on_err) {
            ajs_temp_root_push(vm, ajs_val_from_gc(on_ok));
            ajs_temp_root_push(vm, ajs_val_from_gc(on_err));
            ajs_promise_then_internal(vm, (AjsObject *)ajs_val_to_gc(eval_prom),
                                      ajs_val_from_gc(on_ok), ajs_val_from_gc(on_err),
                                      AJS_VAL_UNDEFINED, AJS_VAL_UNDEFINED, AJS_VAL_UNDEFINED);
        }
    }

    ajs_temp_root_restore(vm, rs);
    return AJS_VAL_UNDEFINED;
}

AjsValue ajs_module_dynamic_import(AjsVm *vm, AjsRealm *realm,
                                   const char *referrer_url, AjsValue specifier_val) {
    if (!vm) return AJS_VAL_UNDEFINED;
    AjsRealm *r = realm ? realm : (vm->active_realm ? vm->active_realm : vm->main_realm);

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, specifier_val);

    AjsRealm *saved_realm = vm->active_realm;
    vm->active_realm = r;

    AjsValue res_fn = AJS_VAL_UNDEFINED, rej_fn = AJS_VAL_UNDEFINED;
    AjsValue prom = ajs_promise_create(vm, &res_fn, &rej_fn);
    if (vm->has_exception || prom == AJS_VAL_EXCEPTION) {
        vm->active_realm = saved_realm;
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_push(vm, prom);
    ajs_temp_root_push(vm, res_fn);
    ajs_temp_root_push(vm, rej_fn);

    /* ECMA-262 §13.3.10.1 step 6: If ToString(specifier) is an abrupt completion, reject capability & return! */
    AjsString *spec_s = ajs_to_js_string(vm, specifier_val);
    if (vm->has_exception || !spec_s) {
        AjsValue exc = vm->exception;
        ajs_temp_root_push(vm, exc);
        ajs_clear_exception(vm);
        ajs_call_internal(vm, rej_fn, AJS_VAL_UNDEFINED, 1, &exc);
        ajs_clear_exception(vm);
        vm->active_realm = saved_realm;
        ajs_temp_root_restore(vm, rs);
        return prom;
    }
    ajs_temp_root_push(vm, ajs_val_from_gc(spec_s));

    AjsString *ref_s = ajs_str_from_cstr(vm, referrer_url ? referrer_url : "", false);
    if (!ref_s) {
        vm->active_realm = saved_realm;
        ajs_temp_root_restore(vm, rs);
        return prom;
    }
    ajs_temp_root_push(vm, ajs_val_from_gc(ref_s));

    AjsEnv *jenv = ajs_env_new(vm, NULL, 4, NULL);
    if (jenv) {
        jenv->slots[0] = ajs_val_from_gc(spec_s);
        jenv->slots[1] = ajs_val_from_gc(ref_s);
        jenv->slots[2] = res_fn;
        jenv->slots[3] = rej_fn;
        ajs_temp_root_push(vm, ajs_val_from_gc(jenv));
        AjsObject *jfn = mod_make_closure_fn(vm, r, dyn_import_job_cb, 0, jenv);
        if (jfn) {
            ajs_enqueue_job_ex(vm, r, ajs_val_from_gc(jfn), 0, NULL);
        }
    }

    vm->active_realm = saved_realm;
    ajs_temp_root_restore(vm, rs);
    return prom;
}
