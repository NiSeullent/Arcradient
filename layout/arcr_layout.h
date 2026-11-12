/*
 * Arcradient (MSHTML-Next)
 * layout/arcr_layout.h - Block & Korean/CJK-Aware Inline Layout Engine
 */
#ifndef ARCR_LAYOUT_H
#define ARCR_LAYOUT_H

#include "../css/arcr_css.h"
#include "../include/arcradient_embed.h"
#include "../platform/arcr_platform.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ArcrPositionedGlyph {
    uint32_t codepoint;
    int32_t  x;          /* Document-space X */
    int32_t  baseline_y; /* Document-space baseline Y */
    int32_t  font_px;
    bool     bold;
    uint32_t color_argb;
} ArcrPositionedGlyph;

typedef struct ArcrInlineBgPill {
    ArcrRect rect;
    uint32_t bg_argb;
    struct ArcrInlineBgPill *next;
} ArcrInlineBgPill;

typedef struct ArcrLayoutBox {
    const ArcrDomNode       *dom_node;
    const ArcrComputedStyle *style;
    ArcrRect                 border_rect;  /* Document-space border box */
    ArcrRect                 content_rect; /* Document-space content box */
    bool                     is_list_item;
    bool                     is_hr;

    ArcrInlineBgPill        *pills_head;
    ArcrPositionedGlyph     *glyphs;
    uint32_t                 glyph_count;

    struct ArcrLayoutBox    *first_child;
    struct ArcrLayoutBox    *last_child;
    struct ArcrLayoutBox    *next_sibling;
} ArcrLayoutBox;

typedef struct ArcrLayoutTree {
    ArcrArena      arena;
    ArcrLayoutBox *root_box;
    int32_t        viewport_width;
    int32_t        total_height;
    uint32_t       box_count;
} ArcrLayoutTree;

ArcrLayoutTree *arcr_layout_build(const ArcrDocument *doc, ArcrFontSystem *fonts,
                                  int32_t viewport_width);
void            arcr_layout_destroy(ArcrLayoutTree *tree);

#endif /* ARCR_LAYOUT_H */
