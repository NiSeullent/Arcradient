/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * paint/arcr_paint.h - Z-Ordered Display List Recorder, O(N) Tile-Row Binning & Multi-Core SIMD Rasterizer
 */
#ifndef ARCR_PAINT_H
#define ARCR_PAINT_H

#include "../accel/arcr_accel.h"
#include "../base/arcr_threadpool.h"
#include "../layout/arcr_layout.h"
#include <stdbool.h>
#include <stdint.h>

#define ARCR_TILE_SIZE_PX 64

typedef enum ArcrDisplayCmdKind {
    ARCR_DCMD_SOLID_RECT      = 0,
    ARCR_DCMD_ROUNDED_RECT    = 1,
    ARCR_DCMD_ROUNDED_STROKE  = 2,
    ARCR_DCMD_BOX_SHADOW      = 3,
    ARCR_DCMD_IMAGE_BLIT      = 4,
    ARCR_DCMD_GLYPH_BATCH     = 5,
    ARCR_DCMD_LINEAR_GRADIENT = 6
} ArcrDisplayCmdKind;

typedef struct ArcrDisplayCmd {
    ArcrDisplayCmdKind         kind;
    ArcrRect                   bounds;
    ArcrRect                   clip_rect;
    bool                       has_clip;
    bool                       is_fixed; /* Viewport-fixed: does NOT shift by scroll_y! */
    uint8_t                    opacity;  /* 0..255 */
    uint32_t                   color_argb;
    uint32_t                   color2_argb; /* End color for linear gradient */
    int32_t                    angle_deg;
    int32_t                    radius_px;
    int32_t                    border_or_blur_px;
    int32_t                    offset_y;
    const ArcrDomSurface      *surface;
    const ArcrPositionedGlyph *glyphs;
    uint32_t                   glyph_count;
} ArcrDisplayCmd;

/* Per-64px-band spatial index for O(1) tile command lookup */
typedef struct ArcrTileBandBin {
    const uint32_t *cmd_indices;
    uint32_t        count;
} ArcrTileBandBin;

typedef struct ArcrDisplayList {
    ArcrArena        arena;
    ArcrDisplayCmd  *cmds;
    uint32_t         count;
    uint32_t         capacity;
    uint32_t         default_bg_argb;

    /* Spatial Y-band bins (64px document-space bands) + viewport-fixed list */
    ArcrTileBandBin *doc_bands;
    uint32_t         doc_band_count;
    uint32_t        *fixed_cmd_indices;
    uint32_t         fixed_cmd_count;
} ArcrDisplayList;

ArcrDisplayList *arcr_paint_build_display_list(const ArcrLayoutTree *layout);
ArcrDisplayList *arcr_paint_build_display_list_ex(const ArcrLayoutTree *layout,
                                                  const char *find_query_utf8,
                                                  uint32_t active_find_index,
                                                  uint32_t *out_total_find_matches,
                                                  int32_t *out_active_match_doc_y);
void             arcr_paint_destroy_display_list(ArcrDisplayList *dl);

/* Single-thread / compatibility rasterize wrapper */
void arcr_paint_rasterize(const ArcrDisplayList *dl,
                          ArcrAccelContext      *accel,
                          ArcrFontSystem        *fonts,
                          uint32_t              *dst_argb,
                          int32_t                fb_w,
                          int32_t                fb_h,
                          int32_t                stride_px,
                          ArcrRect               viewport_rect,
                          int32_t                scroll_y);

/*
 * Multi-core 64x64 Tile-Binned Parallel Rasterizer!
 * Pre-warms all viewport glyphs on the main thread (lock-free tile reads),
 * uses O(1) spatial Y-band bins per tile, and dispatches across `pool` worker threads.
 */
uint32_t arcr_paint_rasterize_parallel(const ArcrDisplayList *dl,
                                       ArcrAccelContext      *accel,
                                       ArcrFontSystem        *fonts,
                                       ArcrThreadPool        *pool,
                                       uint32_t              *dst_argb,
                                       int32_t                fb_w,
                                       int32_t                fb_h,
                                       int32_t                stride_px,
                                       ArcrRect               viewport_rect,
                                       int32_t                scroll_y);

#endif /* ARCR_PAINT_H */
