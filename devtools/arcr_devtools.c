/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * devtools/arcr_devtools.c - On-Demand Diagnostics, Telemetry & Memory Profiler Module
 */
#include "../include/arcr_module.h"
#include "../include/arcradient_embed.h"
#include <stdio.h>

typedef struct ArcrDevToolsVTable {
    uint32_t struct_size;
    size_t (*format_stats_report)(const ArcrEngineStats *stats, char *buf, size_t cap);
} ArcrDevToolsVTable;

static const char *devtools_simd_tier_str(const ArcrEngineStats *s) {
    switch (s->active_simd_level) {
        case 3u: return "AVX2";
        case 2u: return "SSE2";
        case 1u: return "SSE1+SWAR";
        case 0u:
            if ((s->detected_cpu_flags & ARCR_CPU_AVX2) != 0u && s->active_simd_level > 3u) {
                return "AVX2";
            }
            return "Scalar";
        default:
            if ((s->active_simd_level & ARCR_CPU_AVX2) != 0u) return "AVX2";
            if ((s->active_simd_level & ARCR_CPU_SSE2) != 0u) return "SSE2";
            if ((s->active_simd_level & ARCR_CPU_SSE1) != 0u) return "SSE1+SWAR";
            return "Scalar";
    }
}

static const char *devtools_gpu_backend_str(const ArcrEngineStats *s) {
    if (s->gpu_fallback_triggered && !s->gpu_available) {
        return "CPU-Fallback";
    }
    switch (s->active_gpu_backend) {
        case 8u: return s->gpu_available ? "Vulkan" : "Vulkan(Inactive)";
        case 7u: return s->gpu_available ? "Direct3D11" : "Direct3D11(Inactive)";
        case 6u: return s->gpu_available ? "OpenGL-ICD" : "OpenGL-ICD(Inactive)";
        case 5u: return s->gpu_available ? "Direct3D9" : "Direct3D9(Inactive)";
        case 4u: return s->gpu_available ? "DirectDraw7" : "DirectDraw7(Inactive)";
        default:
            return s->gpu_available ? "GPU-Active" : "CPU-Software";
    }
}

static size_t devtools_format_stats(const ArcrEngineStats *s, char *buf, size_t cap) {
    if (!s || !buf || cap == 0u) return 0u;

    const char *simd_name = devtools_simd_tier_str(s);
    const char *gpu_name  = devtools_gpu_backend_str(s);
    uint32_t    workers   = (s->worker_thread_count > 0u) ? s->worker_thread_count : 1u;

    /* Use %lu instead of %zu/%llu so legacy Windows 2000 SP4 msvcrt.dll formats cleanly */
    int n = snprintf(
        buf, cap,
        "[Waterscape DevTools] SIMD=%s GPU=%s%s workers=%u tiles=%u "
        "layout=%luus paint=%luus | DOM=%u boxes=%u cmds=%u atoms=%lu "
        "style_shared=%lu glyph_hits=%lu(%lu cached) "
        "arena=%lu/%luB js_heap=%luB(js_loaded=%s) "
        "net_loaded=%s media_loaded=%s blocked_ads=%u",
        simd_name,
        gpu_name,
        s->gpu_fallback_triggered ? "[FALLBACK]" : "",
        workers,
        s->tiles_rendered_last_frame,
        (unsigned long)s->last_layout_usec,
        (unsigned long)s->last_paint_usec,
        s->dom_node_count,
        s->layout_box_count,
        s->display_cmd_count,
        (unsigned long)s->interned_atom_count,
        (unsigned long)s->style_shared_hits,
        (unsigned long)s->glyph_cache_hits,
        (unsigned long)s->glyph_cache_entries,
        (unsigned long)s->arena_used_bytes,
        (unsigned long)s->arena_allocated_bytes,
        (unsigned long)s->js_heap_bytes,
        s->js_module_loaded ? "YES" : "NO",
        s->net_module_loaded ? "YES" : "NO",
        s->media_module_loaded ? "YES" : "NO",
        s->blocked_requests_count
    );
    if (n <= 0) {
        buf[0] = '\0';
        return 0u;
    }
    return ((size_t)n < cap) ? (size_t)n : (cap - 1u);
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

const ArcrModuleDescriptor *arcr_devtools_module_query(void);
const ArcrModuleDescriptor *arcr_devtools_module_query(void) {
    g_devtools_desc.arch_id = (uint16_t)arcr_host_compile_arch();
#if defined(_WIN32)
    g_devtools_desc.min_os_major_minor = ARCR_OS_WIN2000;
#endif
    return &g_devtools_desc;
}

#if !defined(ARCR_DEVTOOLS_STATIC_LINK)
ARCR_API const ArcrModuleDescriptor *arcr_module_query(void) {
    return arcr_devtools_module_query();
}
#endif
