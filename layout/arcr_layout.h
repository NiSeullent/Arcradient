/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * layout/arcr_layout.h - Block, Flexbox, CSS Grid, Positioned/Sticky, Table, Form & CJK Inline Layout
 */
#ifndef ARCR_LAYOUT_H
#define ARCR_LAYOUT_H

#include "../css/arcr_css.h"
#include "../include/arcradient_embed.h"
#include "../platform/arcr_platform.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum ArcrControlKind {
    ARCR_CTRL_NONE        = 0,
    ARCR_CTRL_TEXT_INPUT  = 1,
    ARCR_CTRL_CHECKBOX    = 2,
    ARCR_CTRL_BUTTON      = 3,
    ARCR_CTRL_VIDEO_MEDIA = 4
} ArcrControlKind;

typedef struct ArcrPositionedGlyph {
    uint32_t codepoint;
    int32_t  x;          /* Document-space X */
    int32_t  baseline_y; /* Document-space baseline Y */
    int32_t  advance_px;
    int32_t  font_px;
    bool     bold;
    bool     monospace;
    bool     underline;
    bool     line_through;
    uint32_t color_argb;
} ArcrPositionedGlyph;

typedef struct ArcrInlineBgPill {
    ArcrRect rect;
    int32_t  radius;
    uint32_t bg_argb;
    struct ArcrInlineBgPill *next;
} ArcrInlineBgPill;

typedef struct ArcrInlineLinkSpan {
    ArcrRect            rect;
    const ArcrDomNode  *link_node;
    const char         *href;
    struct ArcrInlineLinkSpan *next;
} ArcrInlineLinkSpan;

typedef struct ArcrLayoutBox {
    const ArcrDomNode       *dom_node;
    const ArcrComputedStyle *style;
    ArcrRect                 border_rect;  /* Document-space border box (or viewport-space if is_fixed) */
    ArcrRect                 content_rect; /* Document-space content box */
    ArcrRect                 clip_rect;    /* Active overflow:hidden/auto clip rect when has_clip */
    bool                     has_clip;
    bool                     is_fixed;     /* position: fixed (does not scroll with document!) */
    bool                     is_sticky;    /* position: sticky (clamps within scroll container) */
    int32_t                  sticky_anchor_y;
    int32_t                  sticky_top_px;
    int32_t                  sticky_max_dy;
    int32_t                  z_index;
    uint8_t                  effective_opacity; /* 0..255 */
    bool                     is_list_item;
    int32_t                  list_ordinal; /* 0 = bullet <ul>, >=1 = numbered <ol> */
    bool                     is_hr;
    ArcrControlKind          control_kind;

    ArcrInlineBgPill        *pills_head;
    ArcrInlineLinkSpan      *links_head;
    ArcrPositionedGlyph     *glyphs;
    uint32_t                 glyph_count;

    struct ArcrLayoutBox    *first_child;
    struct ArcrLayoutBox    *last_child;
    struct ArcrLayoutBox    *next_sibling;
} ArcrLayoutBox;

typedef struct ArcrHitTestResult {
    const ArcrDomNode *hit_node;
    const ArcrDomNode *clickable_ancestor; /* Nearest <a>, <button>, <input>, <video> */
    const char        *link_href;          /* Non-NULL if over <a href="..."> */
    ArcrCursorKind     cursor;
} ArcrHitTestResult;

typedef struct ArcrLayoutTree {
    ArcrArena      arena;
    ArcrLayoutBox *root_box;
    int32_t        viewport_width;
    int32_t        viewport_height;
    int32_t        zoom_percent;
    int32_t        total_height;
    uint32_t       box_count;
} ArcrLayoutTree;

ArcrLayoutTree *arcr_layout_build(const ArcrDocument *doc, ArcrFontSystem *fonts,
                                  int32_t viewport_width);
ArcrLayoutTree *arcr_layout_build_zoomed(const ArcrDocument *doc, ArcrFontSystem *fonts,
                                         int32_t viewport_width, int32_t zoom_percent);
ArcrLayoutTree *arcr_layout_build_ex(const ArcrDocument *doc, ArcrFontSystem *fonts,
                                     int32_t viewport_width, int32_t viewport_height,
                                     int32_t zoom_percent);
void            arcr_layout_destroy(ArcrLayoutTree *tree);

/* Spatial hit-test in document coordinates (with scroll_y for fixed-element support) */
void arcr_layout_hit_test(const ArcrLayoutTree *tree, const ArcrDocument *doc,
                          int32_t doc_x, int32_t doc_y, ArcrHitTestResult *out_hit);
void arcr_layout_hit_test_ex(const ArcrLayoutTree *tree, const ArcrDocument *doc,
                             int32_t view_x, int32_t view_y, int32_t scroll_y,
                             ArcrHitTestResult *out_hit);

#endif /* ARCR_LAYOUT_H */
