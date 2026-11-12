/*
 * Arcradient (MSHTML-Next)
 * devtools/arcr_devtools.c - On-Demand Diagnostics & Memory Profiler Module
 */
#include "../include/arcr_module.h"
#include "../include/arcradient_embed.h"
#include <stdio.h>

typedef struct ArcrDevToolsVTable {
    uint32_t struct_size;
    size_t (*format_stats_report)(const ArcrEngineStats *stats, char *buf, size_t cap);
} ArcrDevToolsVTable;

static size_t devtools_format_stats(const ArcrEngineStats *s, char *buf, size_t cap) {
    if (!s || !buf || cap == 0u) return 0u;
    /* Use %lu instead of %zu so legacy Windows 2000 msvcrt.dll formats cleanly */
    int n = snprintf(
        buf, cap,
        "[Arcradient DevTools] DOM=%u boxes=%u cmds=%u atoms=%lu style_shared=%lu "
        "arena=%lu/%luB js_loaded=%s(%luB) net_loaded=%s media_loaded=%s",
        s->dom_node_count, s->layout_box_count, s->display_cmd_count,
        (unsigned long)s->interned_atom_count,
        (unsigned long)s->style_shared_hits,
        (unsigned long)s->arena_used_bytes,
        (unsigned long)s->arena_allocated_bytes,
        s->js_module_loaded ? "YES" : "NO",
        (unsigned long)s->js_heap_bytes,
        s->net_module_loaded ? "YES" : "NO",
        s->media_module_loaded ? "YES" : "NO"
    );
    return (n > 0) ? (size_t)n : 0u;
}

static const ArcrDevToolsVTable kDevToolsVTable = {
    .struct_size         = sizeof(ArcrDevToolsVTable),
    .format_stats_report = devtools_format_stats
};

static ArcrModuleDescriptor g_devtools_desc = {
    .magic              = ARCR_MODULE_MAGIC,
    .struct_size        = sizeof(ArcrModuleDescriptor),
    .abi_version        = ARCR_ABI_VERSION,
    .engine_build_id    = ARCR_ENGINE_BUILD_ID,
    .arch_id            = 0,
    .min_os_major_minor = ARCR_OS_POSIX_ANY,
    .required_cpu_flags = ARCR_CPU_SCALAR,
    .module_name        = "arcr-devtools",
    .vtable             = &kDevToolsVTable
};

ARCR_API const ArcrModuleDescriptor *arcr_module_query(void) {
    g_devtools_desc.arch_id = (uint16_t)arcr_host_compile_arch();
#if defined(_WIN32)
    g_devtools_desc.min_os_major_minor = ARCR_OS_WIN2000;
#endif
    return &g_devtools_desc;
}
