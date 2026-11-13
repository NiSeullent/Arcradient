/*
 * ArcradientJS 1.0 (Clean-Room Pure C11 ECMAScript 2026 Virtual Machine)
 * tools/arcr_js_cli.c - Standalone Pure C11 CLI & Test262 Host ($262)
 *
 * Links ONLY ArcradientJS (js/vm, js/builtins, js/modules)
 * plus standard C11 library (-lm). Zero browser/DOM headers, zero Win2000-incompatible APIs.
 */
#include "js/vm/ajs_vm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define CLI_GETCWD _getcwd
#else
#include <unistd.h>
#define CLI_GETCWD getcwd
#endif

#define CLI_MAX_FILES   64
#define CLI_MAX_HARNESS 32
#define CLI_PATH_MAX    2048

typedef struct CliContext {
    uint64_t deadline_ms;
    bool     has_deadline;
    uint32_t async_rej_pin;
    bool     has_async_rej;
} CliContext;

static uint64_t cli_now_ms(void) {
#if defined(CLOCK_MONOTONIC) && !defined(_WIN32)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * UINT64_C(1000) + (uint64_t)(ts.tv_nsec / 1000000L);
    }
#endif
    return (uint64_t)((double)clock() * 1000.0 / (double)CLOCKS_PER_SEC);
}

static bool cli_interrupt_poll(AjsVm *vm, void *userdata) {
    (void)vm;
    CliContext *ctx = (CliContext *)userdata;
    if (!ctx || !ctx->has_deadline) return false;
    return cli_now_ms() >= ctx->deadline_ms;
}

/* ============================================================================
 * 1. Portable Lexical Path Canonicalization & File Loader
 * ============================================================================ */

static void cli_join2(char *dst, size_t cap, const char *a, const char *sep, const char *b) {
    if (!dst || cap == 0) return;
    size_t pos = 0;
    const char *parts[3] = { a ? a : "", sep ? sep : "", b ? b : "" };
    for (int i = 0; i < 3; i++) {
        size_t l = strlen(parts[i]);
        if (pos + l >= cap) l = cap - 1u - pos;
        if (l > 0) memcpy(dst + pos, parts[i], l);
        pos += l;
    }
    dst[pos] = '\0';
}

static void cli_normalize_path(const char *raw, char *out, size_t cap) {
    if (!out || cap == 0) return;
    char tmp[CLI_PATH_MAX];
    size_t rlen = raw ? strlen(raw) : 0;
    if (rlen >= sizeof(tmp)) rlen = sizeof(tmp) - 1u;
    for (size_t i = 0; i < rlen; i++) {
        tmp[i] = (raw[i] == '\\') ? '/' : raw[i];
    }
    tmp[rlen] = '\0';

    bool is_abs = (tmp[0] == '/') ||
                  (rlen >= 2 && ((tmp[0] >= 'A' && tmp[0] <= 'Z') || (tmp[0] >= 'a' && tmp[0] <= 'z')) && tmp[1] == ':');

    char joined[CLI_PATH_MAX];
    if (!is_abs) {
        char cwd[CLI_PATH_MAX];
        if (!CLI_GETCWD(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), ".");
        for (size_t i = 0; cwd[i]; i++) {
            if (cwd[i] == '\\') cwd[i] = '/';
        }
        cli_join2(joined, sizeof(joined), cwd, "/", tmp);
    } else {
        cli_join2(joined, sizeof(joined), tmp, "", "");
    }

    char prefix[8];
    prefix[0] = '\0';
    const char *p = joined;
    if (p[0] && p[1] == ':') {
        prefix[0] = p[0]; prefix[1] = ':'; prefix[2] = '\0';
        p += 2;
    }
    bool leading_slash = (*p == '/');
    while (*p == '/') p++;

    const char *segs[256];
    size_t seg_lens[256];
    int scnt = 0;

    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t sl = (size_t)(p - start);
        while (*p == '/') p++;

        if (sl == 0 || (sl == 1 && start[0] == '.')) continue;
        if (sl == 2 && start[0] == '.' && start[1] == '.') {
            if (scnt > 0) scnt--;
            continue;
        }
        if (scnt < 256) {
            segs[scnt] = start;
            seg_lens[scnt] = sl;
            scnt++;
        }
    }

    size_t pos = 0;
    if (prefix[0]) pos += (size_t)snprintf(out + pos, cap - pos, "%s", prefix);
    if (leading_slash && pos + 1u < cap) { out[pos++] = '/'; out[pos] = '\0'; }
    for (int i = 0; i < scnt; i++) {
        if (i > 0 && pos + 1u < cap) out[pos++] = '/';
        size_t sl = seg_lens[i];
        if (pos + sl >= cap) sl = cap - 1u - pos;
        if (sl > 0) memcpy(out + pos, segs[i], sl);
        pos += sl;
        out[pos] = '\0';
    }
    if (pos == 0) snprintf(out, cap, ".");
}

static void cli_dirname(const char *path, char *out_dir, size_t cap) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out_dir, cap, ".");
        return;
    }
    size_t len = (size_t)(slash - path);
    if (len == 0) {
        snprintf(out_dir, cap, "/");
        return;
    }
    if (len >= cap) len = cap - 1u;
    memcpy(out_dir, path, len);
    out_dir[len] = '\0';
}

static char *cli_read_file(const char *path, size_t *out_len) {
    if (out_len) *out_len = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }

    size_t ulen = (size_t)sz;
    char *buf = (char *)malloc(ulen + 1u);
    if (!buf) { fclose(fp); return NULL; }
    size_t rd = (ulen > 0) ? fread(buf, 1u, ulen, fp) : 0u;
    fclose(fp);
    if (rd != ulen) { free(buf); return NULL; }
    buf[ulen] = '\0';

    /* Strip UTF-8 BOM if present; hashbang `#!...` is handled natively by lexer or stripped */
    if (ulen >= 3u && (unsigned char)buf[0] == 0xEFu &&
        (unsigned char)buf[1] == 0xBBu && (unsigned char)buf[2] == 0xBFu) {
        memmove(buf, buf + 3, ulen - 2u);
        ulen -= 3u;
    }
    if (out_len) *out_len = ulen;
    return buf;
}

static bool cli_file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fclose(fp);
    return true;
}

/* ============================================================================
 * 2. Filesystem ES Module Resolve & Load Hooks
 * ============================================================================ */

static bool cli_module_resolve(AjsVm *vm, const char *specifier, const char *referrer_url,
                               char *out_resolved_url, size_t out_cap,
                               char *out_err_msg, size_t err_cap, void *userdata) {
    (void)vm; (void)userdata;
    if (!specifier || !specifier[0]) {
        snprintf(out_err_msg, err_cap, "Empty module specifier");
        return false;
    }
    const char *spec = specifier;
    if (strncmp(spec, "file://", 7) == 0) spec += 7;

    char combined[CLI_PATH_MAX];
    if (spec[0] == '/' || (spec[0] && spec[1] == ':')) {
        snprintf(combined, sizeof(combined), "%s", spec);
    } else {
        char base_dir[CLI_PATH_MAX];
        if (referrer_url && referrer_url[0] && referrer_url[0] != '<') {
            const char *ref = (strncmp(referrer_url, "file://", 7) == 0) ? referrer_url + 7 : referrer_url;
            cli_dirname(ref, base_dir, sizeof(base_dir));
        } else {
            if (!CLI_GETCWD(base_dir, sizeof(base_dir))) snprintf(base_dir, sizeof(base_dir), ".");
        }
        cli_join2(combined, sizeof(combined), base_dir, "/", spec);
    }

    char norm[CLI_PATH_MAX];
    cli_normalize_path(combined, norm, sizeof(norm));
    if (cli_file_exists(norm)) {
        cli_join2(out_resolved_url, out_cap, norm, "", "");
        return true;
    }

    /* Probe .js / .mjs extensions when omitted */
    static const char *const k_exts[2] = { ".js", ".mjs" };
    for (int i = 0; i < 2; i++) {
        char probe[CLI_PATH_MAX];
        if (strlen(norm) + strlen(k_exts[i]) + 1u < sizeof(probe)) {
            cli_join2(probe, sizeof(probe), norm, "", k_exts[i]);
            if (cli_file_exists(probe)) {
                cli_join2(out_resolved_url, out_cap, probe, "", "");
                return true;
            }
        }
    }

    /* Still return canonical path so module_load produces a clear SyntaxError/IO message,
     * UNLESS bare specifier doesn't start with './' or '../' or '/' and file wasn't found */
    if (spec[0] != '.' && spec[0] != '/' && !(spec[0] && spec[1] == ':')) {
        snprintf(out_err_msg, err_cap, "Cannot resolve bare module specifier '%s'", specifier);
        return false;
    }
    cli_join2(out_resolved_url, out_cap, norm, "", "");
    return true;
}

static AjsModuleRecord *cli_module_load(AjsVm *vm, const char *resolved_url,
                                        const char *referrer_url,
                                        char *out_err_msg, size_t err_cap, void *userdata) {
    (void)referrer_url; (void)userdata;
    size_t code_len = 0;
    char *code = cli_read_file(resolved_url, &code_len);
    if (!code) {
        snprintf(out_err_msg, err_cap, "Cannot load module '%s'", resolved_url);
        return NULL;
    }
    AjsErrorInfo perr;
    memset(&perr, 0, sizeof(perr));
    AjsModuleRecord *mod = ajs_module_parse(vm, code, code_len, resolved_url, &perr);
    free(code);
    if (!mod) {
        snprintf(out_err_msg, err_cap, "%s: %s",
                 perr.name[0] ? perr.name : "SyntaxError",
                 perr.message[0] ? perr.message : "Module parse error");
        return NULL;
    }
    return mod;
}

/* ============================================================================
 * 3. Host Built-ins (`print` and Test262 `$262` Object)
 * ============================================================================ */

static AjsValue js_cli_print(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    for (int i = 0; i < argc; i++) {
        char *s = ajs_to_cstring(vm, argv[i]);
        if (!s) return ajs_undefined();
        if (i > 0) fputc(' ', stdout);
        fputs(s, stdout);
        ajs_free_cstring(vm, s);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return ajs_undefined();
}

static void cli_install_test262_in_realm(AjsVm *vm, AjsRealm *realm);

static AjsValue js_262_createRealm(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)argc; (void)argv; (void)ud;
    AjsRealm *child = ajs_realm_create(vm);
    if (!child) return ajs_throw_error(vm, "Error", "Failed to create realm");
    cli_install_test262_in_realm(vm, child);
    AjsValue child_global = ajs_realm_global_object(child);
    return ajs_get_prop_str(vm, child_global, "$262");
}

static AjsValue js_262_evalScript(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    AjsRealm *target = (AjsRealm *)ud;
    if (!target) target = ajs_vm_main_realm(vm);
    AjsValue code_arg = (argc > 0) ? argv[0] : ajs_undefined();
    size_t code_len = 0;
    char *code = ajs_to_cstring_len(vm, code_arg, &code_len);
    if (!code) return ajs_undefined();

    AjsValue res = ajs_undefined();
    AjsErrorInfo err;
    memset(&err, 0, sizeof(err));
    bool ok = ajs_eval_script_in_realm(target, code, code_len, "<evalScript>", false, &res, &err);
    ajs_free_cstring(vm, code);

    if (!ok) {
        const char *ename = err.name[0] ? err.name : "Error";
        AjsValue tglob = ajs_realm_global_object(target);
        AjsValue ector = ajs_get_prop_str(vm, tglob, ename);
        if (!ajs_has_exception(vm) && ajs_is_callable(vm, ector)) {
            AjsValue msg_v = ajs_string_cstr(vm, err.message);
            AjsValue einstance = ajs_construct(vm, ector, 1, &msg_v);
            if (!ajs_has_exception(vm)) {
                return ajs_throw(vm, einstance);
            }
        }
        return ajs_throw_error(vm, ename, "%s", err.message);
    }
    return res;
}

static AjsValue js_262_detachArrayBuffer(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue ab = (argc > 0) ? argv[0] : ajs_undefined();
    if (!ajs_detach_array_buffer(vm, ab) && !ajs_has_exception(vm)) {
        return ajs_throw_error(vm, "TypeError", "Cannot detach non-ArrayBuffer");
    }
    return ajs_undefined();
}

static AjsValue js_262_gc(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)argc; (void)argv; (void)ud;
    ajs_gc_collect(vm);
    return ajs_undefined();
}

extern AjsValue ajs_module_dynamic_import(AjsVm *vm, AjsRealm *realm,
                                          const char *referrer_url, AjsValue specifier_val);

static AjsValue js_262_destroy(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)vm; (void)this_val; (void)argc; (void)argv; (void)ud;
    return ajs_undefined();
}

static AjsValue js_262_importModule(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    AjsRealm *target = (AjsRealm *)ud;
    if (!target) target = ajs_vm_main_realm(vm);
    AjsValue spec_v = (argc > 0) ? argv[0] : ajs_undefined();
    char *ref_cstr = NULL;
    if (argc > 1 && ajs_typeof(vm, argv[1]) == AJS_TYPE_STRING) {
        ref_cstr = ajs_to_cstring(vm, argv[1]);
    }
    AjsValue prom = ajs_module_dynamic_import(vm, target, ref_cstr ? ref_cstr : "", spec_v);
    if (ref_cstr) ajs_free_cstring(vm, ref_cstr);
    return prom;
}

static AjsValue js_262_is_html_dda(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)vm; (void)this_val; (void)argc; (void)argv; (void)ud;
    return ajs_null();
}

static AjsValue js_262_agent_noop(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)vm; (void)this_val; (void)argc; (void)argv; (void)ud;
    return ajs_undefined();
}

static AjsValue js_262_agent_getReport(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)vm; (void)this_val; (void)argc; (void)argv; (void)ud;
    return ajs_null();
}

static AjsValue js_262_agent_monotonicNow(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)vm; (void)this_val; (void)argc; (void)argv; (void)ud;
    return ajs_number((double)cli_now_ms());
}

static void cli_install_print_in_realm(AjsVm *vm, AjsRealm *realm) {
    AjsHandleScope hs;
    ajs_handle_scope_open(vm, &hs);
    AjsValue glob = ajs_realm_global_object(realm);
    AjsValue pfn = ajs_new_function(vm, js_cli_print, "print", 1, NULL);
    ajs_define_prop_str(vm, glob, "print", pfn, AJS_PROP_WRITABLE | AJS_PROP_CONFIGURABLE);
    ajs_handle_scope_close(vm, &hs);
}

static void cli_install_test262_in_realm(AjsVm *vm, AjsRealm *realm) {
    AjsHandleScope hs;
    ajs_handle_scope_open(vm, &hs);
    cli_install_print_in_realm(vm, realm);

    AjsValue glob = ajs_realm_global_object(realm);
    AjsValue obj262 = ajs_new_object(vm);

    ajs_define_prop_str(vm, obj262, "global", glob, AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, obj262, "createRealm",
                        ajs_new_function(vm, js_262_createRealm, "createRealm", 0, realm), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, obj262, "evalScript",
                        ajs_new_function(vm, js_262_evalScript, "evalScript", 1, realm), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, obj262, "detachArrayBuffer",
                        ajs_new_function(vm, js_262_detachArrayBuffer, "detachArrayBuffer", 1, NULL), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, obj262, "gc",
                        ajs_new_function(vm, js_262_gc, "gc", 0, NULL), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, obj262, "destroy",
                        ajs_new_function(vm, js_262_destroy, "destroy", 0, NULL), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, obj262, "importModule",
                        ajs_new_function(vm, js_262_importModule, "importModule", 2, realm), AJS_PROP_DEFAULT);

    AjsValue dda_proto = ajs_new_object(vm);
    AjsValue dda = ajs_new_constructor(vm, js_262_is_html_dda, "IsHTMLDDA", 0, dda_proto, NULL);
    ajs_define_prop_str(vm, obj262, "IsHTMLDDA", dda, AJS_PROP_DEFAULT);

    AjsValue agent = ajs_new_object(vm);
    ajs_define_prop_str(vm, agent, "start",        ajs_new_function(vm, js_262_agent_noop, "start", 1, NULL), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, agent, "broadcast",    ajs_new_function(vm, js_262_agent_noop, "broadcast", 1, NULL), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, agent, "sleep",        ajs_new_function(vm, js_262_agent_noop, "sleep", 1, NULL), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, agent, "leaving",      ajs_new_function(vm, js_262_agent_noop, "leaving", 0, NULL), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, agent, "getReport",    ajs_new_function(vm, js_262_agent_getReport, "getReport", 0, NULL), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, agent, "monotonicNow", ajs_new_function(vm, js_262_agent_monotonicNow, "monotonicNow", 0, NULL), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, obj262, "agent", agent, AJS_PROP_DEFAULT);

    ajs_define_prop_str(vm, glob, "$262", obj262, AJS_PROP_WRITABLE | AJS_PROP_CONFIGURABLE);
    ajs_handle_scope_close(vm, &hs);
}

/* ============================================================================
 * 4. Error Formatting & Async Module Rejection Capture
 * ============================================================================ */

static int cli_report_error(const AjsErrorInfo *err) {
    const char *name = err->name[0] ? err->name : "Error";
    if (err->url[0] && err->line > 0) {
        fprintf(stderr, "%s: %s (%s:%u:%u)\n", name, err->message, err->url, err->line, err->column);
    } else {
        fprintf(stderr, "%s: %s\n", name, err->message);
    }
    if (err->stack[0]) {
        fprintf(stderr, "%s\n", err->stack);
    }
    fflush(stderr);
    return (err->phase == AJS_ERR_PARSE || err->phase == AJS_ERR_RESOLUTION) ? 2 : 1;
}

static AjsValue js_cli_on_mod_reject(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    CliContext *ctx = (CliContext *)ud;
    if (ctx && !ctx->has_async_rej) {
        ctx->has_async_rej = true;
        ctx->async_rej_pin = ajs_pin_value(vm, argc > 0 ? argv[0] : ajs_undefined());
    }
    return ajs_undefined();
}

static bool cli_ends_with_mjs(const char *path) {
    if (!path) return false;
    size_t l = strlen(path);
    return l >= 4u && strcmp(path + l - 4u, ".mjs") == 0;
}

static int cli_run_source(AjsVm *vm, CliContext *ctx, const char *code, size_t code_len,
                          const char *url, bool is_module, bool force_strict) {
    AjsHandleScope hs;
    ajs_handle_scope_open(vm, &hs);

    AjsErrorInfo err;
    memset(&err, 0, sizeof(err));

    if (is_module) {
        AjsModuleRecord *mod = ajs_module_parse(vm, code, code_len, url, &err);
        if (!mod) {
            ajs_handle_scope_close(vm, &hs);
            if (err.phase == AJS_ERR_NONE) err.phase = AJS_ERR_PARSE;
            return cli_report_error(&err);
        }
        if (!ajs_module_link(vm, mod, &err)) {
            ajs_handle_scope_close(vm, &hs);
            if (err.phase == AJS_ERR_NONE) err.phase = AJS_ERR_RESOLUTION;
            return cli_report_error(&err);
        }
        AjsValue prom = ajs_undefined();
        if (!ajs_module_evaluate(vm, mod, &prom, &err)) {
            ajs_handle_scope_close(vm, &hs);
            return cli_report_error(&err);
        }
        if (ajs_is_promise(vm, prom)) {
            AjsValue then_fn = ajs_get_prop_str(vm, prom, "then");
            if (ajs_is_callable(vm, then_fn)) {
                AjsValue args[2];
                args[0] = ajs_undefined();
                args[1] = ajs_new_function(vm, js_cli_on_mod_reject, "onReject", 1, ctx);
                (void)ajs_call(vm, then_fn, prom, 2, args);
                ajs_clear_exception(vm);
            }
        }
    } else {
        AjsValue res = ajs_undefined();
        if (!ajs_eval_script(vm, code, code_len, url, force_strict, &res, &err)) {
            ajs_handle_scope_close(vm, &hs);
            return cli_report_error(&err);
        }
    }

    if (!ajs_drain_microtasks(vm)) {
        if (ajs_has_exception(vm)) {
            (void)ajs_take_exception(vm, &err);
            ajs_handle_scope_close(vm, &hs);
            return cli_report_error(&err);
        }
    }

    if (ctx->has_async_rej) {
        AjsValue reason = ajs_get_pinned(vm, ctx->async_rej_pin);
        ajs_unpin_value(vm, ctx->async_rej_pin);
        ctx->async_rej_pin = 0;
        ctx->has_async_rej = false;
        (void)ajs_throw(vm, reason);
        (void)ajs_take_exception(vm, &err);
        err.phase = AJS_ERR_RUNTIME;
        ajs_handle_scope_close(vm, &hs);
        return cli_report_error(&err);
    }

    ajs_handle_scope_close(vm, &hs);
    return 0;
}

static void cli_print_usage(const char *prog) {
    printf("Usage: %s [options] [file ...]\n"
           "Options:\n"
           "  -e, --eval <code>       Evaluate inline JS script (or module with -m)\n"
           "  -m, --module            Evaluate entry as ES Module\n"
           "  --strict                Force strict mode for classic scripts\n"
           "  --harness <file.js>     Preload harness script before entry (repeatable)\n"
           "  --test262               Install $262 host object in main & child realms\n"
           "  --heap-limit <MB>       Set GC heap limit in megabytes (alias: --max-heap-mb)\n"
           "  --max-time-ms <MS>      Abort execution after <MS> milliseconds\n"
           "  -v, --version           Print engine version\n"
           "  -h, --help              Print this help message\n", prog);
}

int main(int argc, char **argv) {
    const char *eval_code = NULL;
    bool has_eval = false;
    bool flag_module = false;
    bool flag_strict = false;
    bool flag_test262 = false;
    size_t heap_mb = 0;
    uint64_t max_time_ms = 0;

    const char *harness_files[CLI_MAX_HARNESS];
    int harness_cnt = 0;
    const char *input_files[CLI_MAX_FILES];
    int file_cnt = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            cli_print_usage(argv[0]);
            return 0;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            printf("%s\n", AJS_ENGINE_NAME);
            return 0;
        } else if (strcmp(a, "-m") == 0 || strcmp(a, "--module") == 0) {
            flag_module = true;
        } else if (strcmp(a, "--strict") == 0) {
            flag_strict = true;
        } else if (strcmp(a, "--test262") == 0) {
            flag_test262 = true;
        } else if (strcmp(a, "-e") == 0 || strcmp(a, "--eval") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Missing argument for %s\n", a); return 2; }
            eval_code = argv[++i];
            has_eval = true;
        } else if (strcmp(a, "--harness") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Missing argument for %s\n", a); return 2; }
            if (harness_cnt < CLI_MAX_HARNESS) harness_files[harness_cnt++] = argv[++i];
            else i++;
        } else if (strcmp(a, "--heap-limit") == 0 || strcmp(a, "--max-heap-mb") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Missing argument for %s\n", a); return 2; }
            long v = strtol(argv[++i], NULL, 10);
            if (v > 0) heap_mb = (size_t)v;
        } else if (strcmp(a, "--max-time-ms") == 0 || strcmp(a, "--timeout-ms") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "Missing argument for %s\n", a); return 2; }
            long long v = strtoll(argv[++i], NULL, 10);
            if (v > 0) max_time_ms = (uint64_t)v;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "Unknown option: %s\n", a);
            return 2;
        } else {
            if (file_cnt < CLI_MAX_FILES) input_files[file_cnt++] = a;
        }
    }

    if (!has_eval && file_cnt == 0) {
        cli_print_usage(argv[0]);
        return 0;
    }

    CliContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (max_time_ms > 0) {
        ctx.has_deadline = true;
        ctx.deadline_ms = cli_now_ms() + max_time_ms;
    }

    AjsVmConfig cfg;
    ajs_default_config(&cfg);
    if (heap_mb > 0) cfg.heap_limit_bytes = heap_mb * (size_t)1024u * (size_t)1024u;
    if (ctx.has_deadline) {
        cfg.interrupt_fn = cli_interrupt_poll;
        cfg.interrupt_userdata = &ctx;
    }
    cfg.module_resolve_fn = cli_module_resolve;
    cfg.module_load_fn = cli_module_load;
    cfg.module_userdata = &ctx;

    AjsVm *vm = ajs_vm_create(&cfg);
    if (!vm) {
        fprintf(stderr, "Fatal: Failed to initialize ArcradientJS VM\n");
        return 1;
    }

    if (flag_test262) {
        cli_install_test262_in_realm(vm, ajs_vm_main_realm(vm));
    } else {
        cli_install_print_in_realm(vm, ajs_vm_main_realm(vm));
    }

    int exit_code = 0;

    /* 1. Preload harness classic scripts */
    for (int i = 0; i < harness_cnt && exit_code == 0; i++) {
        char norm[CLI_PATH_MAX];
        cli_normalize_path(harness_files[i], norm, sizeof(norm));
        size_t len = 0;
        char *src = cli_read_file(norm, &len);
        if (!src) {
            fprintf(stderr, "Error: Cannot read harness file '%s'\n", harness_files[i]);
            exit_code = 2;
            break;
        }
        exit_code = cli_run_source(vm, &ctx, src, len, norm, false, false);
        free(src);
    }

    /* 2. Positional files (if multiple files and not `-m`, preload earlier files as classic scripts) */
    for (int i = 0; i < file_cnt && exit_code == 0; i++) {
        char norm[CLI_PATH_MAX];
        cli_normalize_path(input_files[i], norm, sizeof(norm));
        size_t len = 0;
        char *src = cli_read_file(norm, &len);
        if (!src) {
            fprintf(stderr, "Error: Cannot read input file '%s'\n", input_files[i]);
            exit_code = 2;
            break;
        }
        bool is_last = (i + 1 == file_cnt) && !has_eval;
        bool as_mod = cli_ends_with_mjs(norm) || (flag_module && is_last);
        bool as_strict = flag_strict && is_last && !as_mod;
        exit_code = cli_run_source(vm, &ctx, src, len, norm, as_mod, as_strict);
        free(src);
    }

    /* 3. Inline `-e` / `--eval` */
    if (has_eval && exit_code == 0) {
        char eval_url[CLI_PATH_MAX];
        char cwd[CLI_PATH_MAX];
        if (!CLI_GETCWD(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), ".");
        cli_join2(eval_url, sizeof(eval_url), cwd, "/", "<eval>");
        exit_code = cli_run_source(vm, &ctx, eval_code, strlen(eval_code),
                                   flag_module ? eval_url : "<eval>", flag_module, flag_strict && !flag_module);
    }

    ajs_vm_destroy(vm);
    return exit_code;
}
