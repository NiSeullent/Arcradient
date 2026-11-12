/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * css/arcr_css.h - O(1) Indexed CSS3 Engine (Flexbox, Grid, Table, Media Queries, var/calc)
 */
#ifndef ARCR_CSS_H
#define ARCR_CSS_H

#include "../core/arcr_dom.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum ArcrDisplayMode {
    ARCR_DISPLAY_BLOCK        = 0,
    ARCR_DISPLAY_INLINE       = 1,
    ARCR_DISPLAY_NONE         = 2,
    ARCR_DISPLAY_INLINE_BLOCK = 3,
    ARCR_DISPLAY_FLEX         = 4,
    ARCR_DISPLAY_GRID         = 5,
    ARCR_DISPLAY_TABLE        = 6,
    ARCR_DISPLAY_TABLE_ROW    = 7,
    ARCR_DISPLAY_TABLE_CELL   = 8
} ArcrDisplayMode;

typedef enum ArcrTextAlign {
    ARCR_TEXT_ALIGN_LEFT   = 0,
    ARCR_TEXT_ALIGN_CENTER = 1,
    ARCR_TEXT_ALIGN_RIGHT  = 2
} ArcrTextAlign;

typedef enum ArcrFlexDirection {
    ARCR_FLEX_ROW    = 0,
    ARCR_FLEX_COLUMN = 1
} ArcrFlexDirection;

typedef enum ArcrJustifyContent {
    ARCR_JUSTIFY_START         = 0,
    ARCR_JUSTIFY_CENTER        = 1,
    ARCR_JUSTIFY_END           = 2,
    ARCR_JUSTIFY_SPACE_BETWEEN = 3,
    ARCR_JUSTIFY_SPACE_AROUND  = 4
} ArcrJustifyContent;

typedef enum ArcrAlignItems {
    ARCR_ALIGN_STRETCH = 0,
    ARCR_ALIGN_START   = 1,
    ARCR_ALIGN_CENTER  = 2,
    ARCR_ALIGN_END     = 3
} ArcrAlignItems;

typedef struct ArcrEdgeSizes {
    int32_t top;
    int32_t right;
    int32_t bottom;
    int32_t left;
} ArcrEdgeSizes;

typedef struct ArcrComputedStyle {
    ArcrDisplayMode    display;
    uint32_t           color_argb;
    uint32_t           bg_argb;
    uint32_t           border_argb;
    uint32_t           shadow_argb;
    int32_t            shadow_blur;
    int32_t            shadow_offset_y;

    int32_t            font_px;
    int32_t            line_height_pct; /* e.g. 145 = 1.45x */
    bool               font_bold;
    bool               font_monospace;
    bool               underline;
    bool               word_break_keep_all;
    bool               white_space_pre;
    ArcrTextAlign      text_align;

    int32_t            border_radius;
    ArcrEdgeSizes      margin;
    bool               margin_left_auto;
    bool               margin_right_auto;
    ArcrEdgeSizes      padding;
    ArcrEdgeSizes      border;

    /* Explicit sizing (0 = auto, >0 = px, <0 = negated percentage of parent width e.g. -50 = 50%) */
    int32_t            width_spec;
    int32_t            min_width_px;
    int32_t            max_width_px;
    int32_t            height_px;
    int32_t            min_height_px;

    /* Flexbox & CSS Grid layout fields */
    ArcrFlexDirection  flex_direction;
    ArcrJustifyContent justify_content;
    ArcrAlignItems     align_items;
    bool               flex_wrap;
    int32_t            flex_grow;
    int32_t            gap_px;
    int32_t            grid_columns; /* >0 when grid-template-columns: repeat(N, 1fr) */

    uint32_t           dependency_flags;
} ArcrComputedStyle;

#define ARCR_STYLE_DEP_EM    (1u << 0)
#define ARCR_STYLE_DEP_VAR   (1u << 1)
#define ARCR_STYLE_DEP_MEDIA (1u << 2)
#define ARCR_STYLE_DEP_STATE (1u << 3)

typedef struct ArcrCssMediaEnv {
    int32_t viewport_width;
    int32_t viewport_height;
    bool    prefers_dark;
} ArcrCssMediaEnv;

typedef struct ArcrStylesheet ArcrStylesheet;

ArcrStylesheet *arcr_css_parse_document_styles(ArcrDocument *doc);
ArcrStylesheet *arcr_css_parse_document_styles_ex(ArcrDocument *doc, const ArcrCssMediaEnv *env);

void arcr_css_resolve_tree(ArcrDocument *doc, ArcrStylesheet *sheet,
                           size_t *out_style_share_hits);

bool arcr_css_parse_color(const char *str, size_t len, uint32_t *out_argb);

#endif /* ARCR_CSS_H */
