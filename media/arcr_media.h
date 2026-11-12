/*
 * Arcradient (MSHTML-Next)
 * media/arcr_media.h - Built-in Lightweight Media & Adaptive Fluid Quality Policy
 */
#ifndef ARCR_MEDIA_H
#define ARCR_MEDIA_H

#include "../include/arcr_module.h"
#include "../include/arcradient_embed.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum ArcrMediaScaleQuality {
    ARCR_SCALE_NEAREST_FAST = 0, /* Overloaded CPU: integer table YUV->RGB + nearest */
    ARCR_SCALE_BILINEAR     = 1, /* Balanced */
    ARCR_SCALE_BICUBIC_HQ   = 2  /* Modern / Low CPU load */
} ArcrMediaScaleQuality;

typedef enum ArcrFrameDropMode {
    ARCR_DROP_NONE           = 0,
    ARCR_DROP_DISPLAY_FRAMES = 1, /* Skip presentation blit while keeping decode sync */
    ARCR_DROP_NONREF_BFRAMES = 2  /* Heavy CPU load: skip B-frames, preserve audio 100%! */
} ArcrFrameDropMode;

typedef struct ArcrStreamVariant {
    int32_t  width;
    int32_t  height;
    uint32_t bitrate_kbps;
} ArcrStreamVariant;

typedef struct ArcrMediaAdaptiveDecision {
    size_t                chosen_stream_index;
    int32_t               render_width;
    int32_t               render_height;
    ArcrMediaScaleQuality scale_quality;
    ArcrFrameDropMode     frame_drop_mode;
    bool                  keep_audio_continuous; /* Always true per Section 10.2! */
} ArcrMediaAdaptiveDecision;

typedef struct ArcrMediaVTable {
    uint32_t struct_size;
    void (*compute_adaptive_policy)(
        ArcrProfileMode            profile,
        const ArcrStreamVariant   *variants,
        size_t                     variant_count,
        int32_t                    viewport_w,
        int32_t                    viewport_h,
        uint32_t                   cpu_usage_percent,
        ArcrMediaAdaptiveDecision *out_decision
    );
} ArcrMediaVTable;

#endif /* ARCR_MEDIA_H */
