/*
 * Arcradient (MSHTML-Next)
 * paint/arcr_paint.h - Display List Recorder & CPU/SIMD Tile Rasterizer
 */
#ifndef ARCR_PAINT_H
#define ARCR_PAINT_H

#include "../accel/arcr_accel.h"
#include "../layout/arcr_layout.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum ArcrDisplayCmdKind {
    ARCR_DCMD_SOLID_RECT  = 0,
    ARCR_DCMD_GLYPH_BATCH = 1
} ArcrDisplayCmdKind;

typedef struct ArcrDisplayCmd {
    ArcrDisplayCmdKind         kind;
    ArcrRect                   bounds;
    uint32_t                   color_argb;
    const ArcrPositionedGlyph *glyphs;
    uint32_t                   glyph_count;
} ArcrDisplayCmd;

typedef struct ArcrDisplayList {
    ArcrArena       arena;
    ArcrDisplayCmd *cmds;
    uint32_t        count;
    uint32_t        capacity;
    uint32_t        default_bg_argb;
} ArcrDisplayList;

ArcrDisplayList *arcr_paint_build_display_list(const ArcrLayoutTree *layout);
void             arcr_paint_destroy_display_list(ArcrDisplayList *dl);

/* Rasterizes the display list within `dirty_viewport_rect` at `scroll_y` into `dst_argb` */
void arcr_paint_rasterize(const ArcrDisplayList *dl,
                          ArcrAccelContext      *accel,
                          ArcrFontSystem        *fonts,
                          uint32_t              *dst_argb,
                          int32_t                fb_w,
                          int32_t                fb_h,
                          int32_t                stride_px,
                          ArcrRect               viewport_rect,
                          int32_t                scroll_y);

#endif /* ARCR_PAINT_H */
