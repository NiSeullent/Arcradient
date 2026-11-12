/*
 * Arcradient (MSHTML-Next)
 * css/arcr_css.h - CSS Parser, Selector Index, Ancestor Bloom Filter, var() & Style Sharing
 */
#ifndef ARCR_CSS_H
#define ARCR_CSS_H

#include "../core/arcr_dom.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum ArcrDisplayMode {
    ARCR_DISPLAY_BLOCK  = 0,
    ARCR_DISPLAY_INLINE = 1,
    ARCR_DISPLAY_NONE   = 2
} ArcrDisplayMode;

typedef struct ArcrEdgeSizes {
    int32_t top;
    int32_t right;
    int32_t bottom;
    int32_t left;
} ArcrEdgeSizes;

typedef struct ArcrComputedStyle {
    ArcrDisplayMode display;
    uint32_t        color_argb;
    uint32_t        bg_argb;
    uint32_t        border_argb;
    int32_t         font_px;
    bool            font_bold;
    bool            word_break_keep_all;
    int32_t         border_radius;
    ArcrEdgeSizes   margin;
    ArcrEdgeSizes   padding;
    ArcrEdgeSizes   border;
    uint32_t        dependency_flags; /* Tracks relative units / var() dependencies */
} ArcrComputedStyle;

#define ARCR_STYLE_DEP_EM  (1u << 0)
#define ARCR_STYLE_DEP_VAR (1u << 1)

typedef struct ArcrStylesheet ArcrStylesheet;

ArcrStylesheet *arcr_css_parse_document_styles(ArcrDocument *doc);
void            arcr_css_resolve_tree(ArcrDocument *doc, ArcrStylesheet *sheet,
                                      size_t *out_style_share_hits);

bool arcr_css_parse_color(const char *str, size_t len, uint32_t *out_argb);

#endif /* ARCR_CSS_H */
