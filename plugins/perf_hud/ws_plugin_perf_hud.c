/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * plugins/perf_hud/ws_plugin_perf_hud.c - Official Native C11 Hardware Telemetry HUD Plugin
 *
 * Compatible with Windows 2000 SP4 (`ARCR_OS_WIN2000`, `ARCR_CPU_SCALAR`) through
 * modern 64-bit Windows 11 & Linux (`ARCR_OS_POSIX_ANY`).
 * Exports both `arcr_module_query` and `arcr_module_entry` (plus `ws_plugin_perf_hud_descriptor`
 * for static-link host configurations).
 */
#include "../../include/waterscape_plugin.h"
#include "../../accel/arcr_accel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct WsPerfHudInstance {
    uint64_t smoothed_render_usec;
    uint64_t peak_render_usec;
    uint32_t sample_count;
} WsPerfHudInstance;

static bool perf_hud_on_init(void **out_instance) {
    if (!out_instance) return false;
    WsPerfHudInstance *inst = (WsPerfHudInstance *)calloc(1u, sizeof(WsPerfHudInstance));
    if (!inst) return false;
    *out_instance = inst;
    return true;
}

static void perf_hud_on_shutdown(void *instance) {
    free(instance);
}

static const char *simd_backend_short_label(uint32_t simd_backend, uint32_t cpu_flags) {
    switch (simd_backend) {
        case ARCR_BACKEND_CPU_AVX2:      return "AVX2";
        case ARCR_BACKEND_CPU_SSE2_SSE3: return (cpu_flags & ARCR_CPU_SSE3) ? "SSE3" : "SSE2";
        case ARCR_BACKEND_CPU_SSE1_SWAR: return "SSE1+SWAR";
        case ARCR_BACKEND_CPU_SCALAR:    return "Scalar";
        default:
            if (cpu_flags & ARCR_CPU_AVX2) return "AVX2";
            if (cpu_flags & ARCR_CPU_SSE3) return "SSE3";
            if (cpu_flags & ARCR_CPU_SSE2) return "SSE2";
            if (cpu_flags & ARCR_CPU_SSE1) return "SSE1";
            return "Scalar";
    }
}

static const char *gpu_backend_short_label(uint32_t gpu_backend) {
    switch (gpu_backend) {
        case ARCR_BACKEND_GPU_VULKAN: return "Vulkan";
        case ARCR_BACKEND_GPU_D3D11:  return "D3D11";
        case ARCR_BACKEND_GPU_OPENGL: return "OpenGL";
        case ARCR_BACKEND_GPU_D3D9:   return "D3D9";
        case ARCR_BACKEND_GPU_DDRAW7: return "DDraw7";
        default:                      return NULL;
    }
}

static void perf_hud_format_badge(void *instance, const WaterscapePluginHostContext *host,
                                  char *out_text, size_t out_cap) {
    if (!out_text || out_cap == 0u) return;
    out_text[0] = '\0';
    if (!host) return;

    WsPerfHudInstance *inst = (WsPerfHudInstance *)instance;
    uint64_t usec = host->last_frame_render_usec;
    if (inst) {
        if (inst->sample_count == 0u) {
            inst->smoothed_render_usec = usec;
        } else {
            inst->smoothed_render_usec = (inst->smoothed_render_usec * 3ULL + usec) / 4ULL;
        }
        if (usec > inst->peak_render_usec) {
            inst->peak_render_usec = usec;
        }
        inst->sample_count++;
    }

    const char *simd_lbl = simd_backend_short_label(host->active_simd_backend, host->detected_cpu_flags);
    const char *gpu_lbl  = gpu_backend_short_label(host->active_gpu_backend);
    unsigned long threads = (unsigned long)(host->worker_threads > 0u ? host->worker_threads : 1u);
    unsigned long tiles   = (unsigned long)host->last_frame_tiles;

    /* Locale-invariant integer fixed-point conversion from microseconds to 0.00ms */
    unsigned long ms_whole = (unsigned long)(usec / 1000ULL);
    unsigned long ms_centi = (unsigned long)(((usec % 1000ULL) + 5ULL) / 10ULL);
    if (ms_centi >= 100UL) {
        ms_whole += 1UL;
        ms_centi -= 100UL;
    }

    if (gpu_lbl) {
        snprintf(out_text, out_cap, "SIMD:%s+%s | %luT | %lu tiles | %lu.%02lums",
                 simd_lbl, gpu_lbl, threads, tiles, ms_whole, ms_centi);
    } else {
        snprintf(out_text, out_cap, "SIMD:%s | %luT | %lu tiles | %lu.%02lums",
                 simd_lbl, threads, tiles, ms_whole, ms_centi);
    }
}

static const WaterscapePluginVTable kPerfHudVTable = {
    .struct_size        = sizeof(WaterscapePluginVTable),
    .plugin_id          = "ws-plugin-perf-hud",
    .plugin_name        = "Arcradient Performance HUD",
    .plugin_version     = "1.0.0",
    .plugin_description = "Real-time SIMD, GPU compositing, worker thread & 64x64 tile render telemetry badge",
    .on_init            = perf_hud_on_init,
    .on_shutdown        = perf_hud_on_shutdown,
    .should_block_url   = NULL,
    .get_injected_css   = NULL,
    .get_injected_js    = NULL,
    .format_hud_badge   = perf_hud_format_badge
};

static ArcrModuleDescriptor g_perf_hud_desc = {
    .magic              = ARCR_MODULE_MAGIC,
    .struct_size        = sizeof(ArcrModuleDescriptor),
    .abi_version        = ARCR_ABI_VERSION,
    .engine_build_id    = ARCR_ENGINE_BUILD_ID,
    .arch_id            = 0,
#if defined(_WIN32)
    .min_os_major_minor = ARCR_OS_WIN2000, /* 0x0500 Windows 2000 SP4+ */
#else
    .min_os_major_minor = ARCR_OS_POSIX_ANY,
#endif
    .required_cpu_flags = ARCR_CPU_SCALAR, /* Runs on Pentium III / Scalar through AVX2! */
    .module_name        = "ws-plugin-perf-hud",
    .vtable             = &kPerfHudVTable
};

const ArcrModuleDescriptor *ws_plugin_perf_hud_descriptor(void) {
    g_perf_hud_desc.arch_id = (uint16_t)arcr_host_compile_arch();
#if defined(_WIN32)
    g_perf_hud_desc.min_os_major_minor = ARCR_OS_WIN2000;
#else
    g_perf_hud_desc.min_os_major_minor = ARCR_OS_POSIX_ANY;
#endif
    return &g_perf_hud_desc;
}

#if !defined(WS_PLUGIN_STATIC_LINK)
ARCR_API const ArcrModuleDescriptor *arcr_module_query(void) {
    return ws_plugin_perf_hud_descriptor();
}

ARCR_API const ArcrModuleDescriptor *arcr_module_entry(void) {
    return ws_plugin_perf_hud_descriptor();
}
#endif
