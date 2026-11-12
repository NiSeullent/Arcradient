/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * layout/arcr_layout.h - Block, Flexbox, CSS Grid, Table, Form & Korean Inline Layout
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
    ARCR_CTRL_NONE       = 0,
    ARCR_CTRL_TEXT_INPUT = 1,
    ARCR_CTRL_CHECKBOX   = 2,
    ARCR_CTRL_BUTTON     = 3
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
    ArcrRect                 border_rect;  /* Document-space border box */
    ArcrRect                 content_rect; /* Document-space content box */
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
    const ArcrDomNode *clickable_ancestor; /* Nearest <a>, <button>, <input> */
    const char        *link_href;          /* Non-NULL if over <a href="..."> */
    ArcrCursorKind     cursor;
} ArcrHitTestResult;

typedef struct ArcrLayoutTree {
    ArcrArena      arena;
    ArcrLayoutBox *root_box;
    int32_t        viewport_width;
    int32_t        zoom_percent;
    int32_t        total_height;
    uint32_t       box_count;
} ArcrLayoutTree;

ArcrLayoutTree *arcr_layout_build(const ArcrDocument *doc, ArcrFontSystem *fonts,
                                  int32_t viewport_width);
ArcrLayoutTree *arcr_layout_build_zoomed(const ArcrDocument *doc, ArcrFontSystem *fonts,
                                         int32_t viewport_width, int32_t zoom_percent);
void            arcr_layout_destroy(ArcrLayoutTree *tree);

/* Spatial hit-test in document coordinates (doc_x = view_x, doc_y = view_y + scroll_y) */
void arcr_layout_hit_test(const ArcrLayoutTree *tree, const ArcrDocument *doc,
                          int32_t doc_x, int32_t doc_y, ArcrHitTestResult *out_hit);

#endif /* ARCR_LAYOUT_H */
