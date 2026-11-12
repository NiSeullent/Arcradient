/*
 * Arcradient (MSHTML-Next)
 * js/arcr_js_portable.c - On-Demand Portable JS Module (Loaded ONLY when page uses JS)
 *
 * Designed as the Pure C99/C11 adapter slot for QuickJS-NG / Portable interpreter,
 * enforcing browser sandbox isolation (zero filesystem/process access) and
 * instant bytecode cache eviction under memory pressure.
 */
#include "arcr_js.h"
#include "../base/arcr_base.h"
#include <stdlib.h>
#include <string.h>

struct ArcrJsVm {
    ArcrArena           vm_arena;
    size_t              heap_limit;
    size_t              bytecode_cache_bytes;
    ArcrJsHostCallbacks host_cb;
    uint32_t            executed_scripts;
};

static ArcrJsVm *portable_create_vm(size_t heap_limit_bytes,
                                    const ArcrJsHostCallbacks *host_cb) {
    ArcrJsVm *vm = (ArcrJsVm *)calloc(1u, sizeof(ArcrJsVm));
    if (!vm) return NULL;
    arcr_arena_init(&vm->vm_arena, 16384u);
    vm->heap_limit = (heap_limit_bytes > 0u) ? heap_limit_bytes : (8u * 1024u * 1024u);
    if (host_cb) vm->host_cb = *host_cb;
    return vm;
}

static void portable_destroy_vm(ArcrJsVm *vm) {
    if (!vm) return;
    arcr_arena_destroy(&vm->vm_arena);
    free(vm);
}

/* Minimal bootstrap interpreter hook recognizing `document.title = "..."` and `console.log("...")`
 * before full QuickJS-NG amalgamation vendor drop-in. */
static bool extract_quoted_arg(const char *src, size_t len, const char *pattern,
                               char *out_buf, size_t out_cap) {
    const char *hit = strstr(src, pattern);
    if (!hit) return false;
    size_t offset = (size_t)(hit - src) + strlen(pattern);
    while (offset < len && (src[offset] == ' ' || src[offset] == '=' || src[offset] == '(')) {
        ++offset;
    }
    if (offset >= len || (src[offset] != '"' && src[offset] != '\'')) return false;
    char quote = src[offset++];
    size_t k = 0u;
    while (offset < len && src[offset] != quote && k + 1u < out_cap) {
        out_buf[k++] = src[offset++];
    }
    out_buf[k] = '\0';
    return (offset < len && src[offset] == quote);
}

static bool portable_eval_script(ArcrJsVm *vm, const char *utf8_code, size_t code_len) {
    if (!vm || !utf8_code) return false;
    if (vm->vm_arena.total_used + code_len > vm->heap_limit) return false;

    /* Simulate bytecode cache entry */
    void *bc = arcr_arena_alloc(&vm->vm_arena, ARCR_MAX(64u, code_len / 2u), 8u);
    if (!bc) return false;
    vm->bytecode_cache_bytes += ARCR_MAX(64u, code_len / 2u);
    vm->executed_scripts++;

    char *terminated = arcr_arena_strdup(&vm->vm_arena, utf8_code, code_len);
    if (!terminated) return false;

    char arg[256];
    if (extract_quoted_arg(terminated, code_len, "document.title", arg, sizeof(arg))) {
        if (vm->host_cb.set_document_title) {
            vm->host_cb.set_document_title(vm->host_cb.userdata, arg);
        }
    }
    if (extract_quoted_arg(terminated, code_len, "console.log", arg, sizeof(arg))) {
        if (vm->host_cb.console_log) {
            vm->host_cb.console_log(vm->host_cb.userdata, arg);
        }
    }
    return true;
}

static size_t portable_get_used_heap_bytes(const ArcrJsVm *vm) {
    if (!vm) return 0u;
    return sizeof(ArcrJsVm) + vm->vm_arena.total_used + vm->bytecode_cache_bytes;
}

static void portable_release_caches_on_pressure(ArcrJsVm *vm, int pressure_level) {
    if (!vm) return;
    if (pressure_level >= 1) {
        vm->bytecode_cache_bytes = 0u;
        if (pressure_level >= 2) {
            arcr_arena_reset(&vm->vm_arena);
        }
    }
}

static const ArcrJsVTable kPortableJsVTable = {
    .struct_size                = sizeof(ArcrJsVTable),
    .engine_backend_name        = "ArcradientJS-Portable (Pure C Interpreter)",
    .create_vm                  = portable_create_vm,
    .destroy_vm                 = portable_destroy_vm,
    .eval_script                = portable_eval_script,
    .get_used_heap_bytes        = portable_get_used_heap_bytes,
    .release_caches_on_pressure = portable_release_caches_on_pressure
};

static ArcrModuleDescriptor g_js_desc = {
    .magic              = ARCR_MODULE_MAGIC,
    .struct_size        = sizeof(ArcrModuleDescriptor),
    .abi_version        = ARCR_ABI_VERSION,
    .engine_build_id    = ARCR_ENGINE_BUILD_ID,
    .arch_id            = 0,
    .min_os_major_minor = ARCR_OS_POSIX_ANY,
    .required_cpu_flags = ARCR_CPU_SCALAR, /* Runs on Win2000 x86 SSE1 or Scalar! */
    .module_name        = "arcr-js-portable",
    .vtable             = &kPortableJsVTable
};

ARCR_API const ArcrModuleDescriptor *arcr_module_query(void) {
    g_js_desc.arch_id = (uint16_t)arcr_host_compile_arch();
#if defined(_WIN32)
    g_js_desc.min_os_major_minor = ARCR_OS_WIN2000;
#endif
    return &g_js_desc;
}
