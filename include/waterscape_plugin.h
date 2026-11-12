/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * include/waterscape_plugin.h - Native Pure C11 Browser Plugin & Extension Host ABI
 */
#ifndef WATERSCAPE_PLUGIN_H
#define WATERSCAPE_PLUGIN_H

#include "arcr_module.h"
#include "arcradient_embed.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WATERSCAPE_PLUGIN_ABI_VERSION 0x00010000u

typedef struct WaterscapePluginHostContext {
    uint32_t detected_cpu_flags;
    uint32_t active_simd_backend;
    uint32_t active_gpu_backend;
    uint32_t worker_threads;
    uint64_t last_frame_render_usec;
    uint32_t last_frame_tiles;
    size_t   engine_memory_bytes;
} WaterscapePluginHostContext;

typedef struct WaterscapePluginVTable {
    uint32_t    struct_size;
    const char *plugin_id;
    const char *plugin_name;
    const char *plugin_version;
    const char *plugin_description;

    bool (*on_init)(void **out_instance);
    void (*on_shutdown)(void *instance);

    /* Return true to BLOCK a network URL request (e.g., ad/tracker/malware filter) */
    bool (*should_block_url)(void *instance, const char *url, const char *page_host);

    /* Optional CSS or JS snippet injected into loaded documents (may return NULL) */
    const char *(*get_injected_css)(void *instance, const char *url);
    const char *(*get_injected_js)(void *instance, const char *url);

    /* Formats a live status/HUD string for toolbar badge or status overlay */
    void (*format_hud_badge)(void *instance, const WaterscapePluginHostContext *host,
                             char *out_text, size_t out_cap);
} WaterscapePluginVTable;

#ifdef __cplusplus
}
#endif

#endif /* WATERSCAPE_PLUGIN_H */
