/*
 * Arcradient (MSHTML-Next)
 * media/arcr_media.c - On-Demand Adaptive Fluid Media Module
 */
#include "arcr_media.h"
#include "../base/arcr_base.h"
#include <string.h>

static void media_compute_adaptive_policy(
    ArcrProfileMode            profile,
    const ArcrStreamVariant   *variants,
    size_t                     variant_count,
    int32_t                    viewport_w,
    int32_t                    viewport_h,
    uint32_t                   cpu_usage_percent,
    ArcrMediaAdaptiveDecision *out
) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->keep_audio_continuous = true;

    int32_t cap_h = 2160;
    if (profile == ARCR_PROFILE_MINIMUM) cap_h = 480;
    else if (profile == ARCR_PROFILE_BALANCED) cap_h = 720;

    if (cpu_usage_percent >= 88u) {
        cap_h = ARCR_MIN(cap_h, 360);
        out->frame_drop_mode = ARCR_DROP_NONREF_BFRAMES;
        out->scale_quality   = ARCR_SCALE_NEAREST_FAST;
    } else if (cpu_usage_percent >= 72u) {
        cap_h = ARCR_MIN(cap_h, 480);
        out->frame_drop_mode = ARCR_DROP_DISPLAY_FRAMES;
        out->scale_quality   = ARCR_SCALE_NEAREST_FAST;
    } else if (cpu_usage_percent >= 50u || profile == ARCR_PROFILE_MINIMUM) {
        out->frame_drop_mode = ARCR_DROP_NONE;
        out->scale_quality   = ARCR_SCALE_BILINEAR;
    } else {
        out->frame_drop_mode = ARCR_DROP_NONE;
        out->scale_quality   = ARCR_SCALE_BICUBIC_HQ;
    }

    int32_t target_h = (viewport_h > 0) ? ARCR_MIN(viewport_h, cap_h) : cap_h;

    if (variants && variant_count > 0u) {
        size_t best_idx = 0u;
        int32_t best_diff = INT32_MAX;
        for (size_t i = 0; i < variant_count; ++i) {
            int32_t vh = variants[i].height;
            int32_t diff = (vh <= target_h) ? (target_h - vh) : ((vh - target_h) * 4);
            if (diff < best_diff) {
                best_diff = diff;
                best_idx  = i;
            }
        }
        out->chosen_stream_index = best_idx;
        out->render_width  = ARCR_MIN(viewport_w > 0 ? viewport_w : variants[best_idx].width,
                                      variants[best_idx].width);
        out->render_height = ARCR_MIN(viewport_h > 0 ? viewport_h : variants[best_idx].height,
                                      variants[best_idx].height);
    } else {
        out->render_width  = (viewport_w > 0) ? viewport_w : 640;
        out->render_height = target_h;
    }
}

static const ArcrMediaVTable kMediaVTable = {
    .struct_size             = sizeof(ArcrMediaVTable),
    .compute_adaptive_policy = media_compute_adaptive_policy
};

static ArcrModuleDescriptor g_media_desc = {
    .magic              = ARCR_MODULE_MAGIC,
    .struct_size        = sizeof(ArcrModuleDescriptor),
    .abi_version        = ARCR_ABI_VERSION,
    .engine_build_id    = ARCR_ENGINE_BUILD_ID,
    .arch_id            = 0,
    .min_os_major_minor = ARCR_OS_POSIX_ANY,
    .required_cpu_flags = ARCR_CPU_SCALAR,
    .module_name        = "arcr-media",
    .vtable             = &kMediaVTable
};

ARCR_API const ArcrModuleDescriptor *arcr_module_query(void) {
    g_media_desc.arch_id = (uint16_t)arcr_host_compile_arch();
#if defined(_WIN32)
    g_media_desc.min_os_major_minor = ARCR_OS_WIN2000;
#endif
    return &g_media_desc;
}
