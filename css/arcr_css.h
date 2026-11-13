/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * css/arcr_css.h - Modern 2026 CSS3 Engine (Tailwind v4, oklch, Flexbox/Grid,
 *                  Positioning, z-index, Overflow, @keyframes Animations & Transforms)
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
    ARCR_DISPLAY_TABLE_CELL   = 8,
    ARCR_DISPLAY_INLINE_FLEX  = 9,
    ARCR_DISPLAY_CONTENTS     = 10
} ArcrDisplayMode;

typedef enum ArcrPositionMode {
    ARCR_POS_STATIC   = 0,
    ARCR_POS_RELATIVE = 1,
    ARCR_POS_ABSOLUTE = 2,
    ARCR_POS_FIXED    = 3,
    ARCR_POS_STICKY   = 4
} ArcrPositionMode;

typedef enum ArcrOverflowMode {
    ARCR_OVERFLOW_VISIBLE = 0,
    ARCR_OVERFLOW_HIDDEN  = 1,
    ARCR_OVERFLOW_AUTO    = 2,
    ARCR_OVERFLOW_SCROLL  = 3
} ArcrOverflowMode;

typedef enum ArcrBoxSizing {
    ARCR_BOX_CONTENT = 0,
    ARCR_BOX_BORDER  = 1
} ArcrBoxSizing;

typedef enum ArcrTextAlign {
    ARCR_TEXT_ALIGN_LEFT   = 0,
    ARCR_TEXT_ALIGN_CENTER = 1,
    ARCR_TEXT_ALIGN_RIGHT  = 2
} ArcrTextAlign;

typedef enum ArcrFlexDirection {
    ARCR_FLEX_ROW            = 0,
    ARCR_FLEX_COLUMN         = 1,
    ARCR_FLEX_ROW_REVERSE    = 2,
    ARCR_FLEX_COLUMN_REVERSE = 3
} ArcrFlexDirection;

typedef enum ArcrJustifyContent {
    ARCR_JUSTIFY_START         = 0,
    ARCR_JUSTIFY_CENTER        = 1,
    ARCR_JUSTIFY_END           = 2,
    ARCR_JUSTIFY_SPACE_BETWEEN = 3,
    ARCR_JUSTIFY_SPACE_AROUND  = 4,
    ARCR_JUSTIFY_SPACE_EVENLY  = 5
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
    ArcrPositionMode   position;
    ArcrOverflowMode   overflow_x;
    ArcrOverflowMode   overflow_y;
    ArcrBoxSizing      box_sizing;
    bool               visibility_hidden;
    bool               pointer_events_none;
    uint8_t            opacity; /* 0..255 (255 = 1.0) */
    int32_t            z_index;

#define ARCR_MAX_GRID_TRACKS 12u

    uint32_t           color_argb;
    uint32_t           bg_argb;
    uint32_t           border_argb;
    uint32_t           border_top_argb;
    uint32_t           border_right_argb;
    uint32_t           border_bottom_argb;
    uint32_t           border_left_argb;
    uint32_t           shadow_argb;
    int32_t            shadow_blur;
    int32_t            shadow_offset_y;

    /* Linear Gradient Background */
    bool               has_gradient;
    uint32_t           grad_start_argb;
    uint32_t           grad_end_argb;
    int32_t            grad_angle_deg;

    /* CSS Mask Image (e.g. data:image/svg+xml icon tinted with bg_argb) */
    const char        *mask_image_url;

    int32_t            font_px;
    int32_t            line_height_pct; /* e.g. 145 = 1.45x */
    int32_t            line_height_px;  /* >0 when explicit px/rem/em line-height is set */
    bool               font_bold;
    bool               font_monospace;
    bool               underline;
    bool               line_through;
    bool               text_ellipsis;
    bool               word_break_keep_all;
    bool               white_space_pre;
    bool               white_space_nowrap;
    bool               list_style_none;
    bool               sr_only_clip;    /* clip: rect(0,0,0,0) / clip-path: inset(50%) */
    uint8_t            text_transform;  /* 0=none, 1=uppercase, 2=lowercase */
    uint8_t            container_type;  /* 0=normal, 1=inline-size, 2=size */
    ArcrTextAlign      text_align;

    int32_t            border_radius;
    ArcrEdgeSizes      margin;
    bool               margin_left_auto;
    bool               margin_right_auto;
    ArcrEdgeSizes      padding;
    ArcrEdgeSizes      border;

    /* Explicit sizing (width_spec: 0 = auto, >0 = px, <0 = negated % of parent width) */
    int32_t            width_spec;
    int32_t            min_width_px;
    int32_t            max_width_px;
    int32_t            height_px;
    int32_t            height_pct;     /* >0 when height: X% (CSS2.1 auto-parent safe!) */
    int32_t            min_height_px;
    int32_t            max_height_px;
    int32_t            aspect_ratio_w; /* e.g. 16 for aspect-ratio: 16 / 9 */
    int32_t            aspect_ratio_h; /* e.g. 9 */

    /* Positioned offsets (top / right / bottom / left; pct when negated or has_pos_*_pct) */
    bool               has_top;
    bool               has_right;
    bool               has_bottom;
    bool               has_left;
    int32_t            top_px;
    int32_t            right_px;
    int32_t            bottom_px;
    int32_t            left_px;

    /* Flexbox & CSS Grid layout fields */
    ArcrFlexDirection  flex_direction;
    ArcrJustifyContent justify_content;
    ArcrAlignItems     align_items;
    ArcrAlignItems     align_self;
    bool               has_align_self;
    bool               flex_wrap;
    int32_t            flex_grow;
    int32_t            flex_shrink;     /* Default 1 */
    int32_t            flex_basis_spec; /* 0 = auto, >0 = px, <0 = negated % */
    int32_t            gap_px;
    int32_t            row_gap_px;
    int32_t            grid_columns;    /* >0 when grid-template-columns: repeat(N, ...) */
    int32_t            grid_min_col_px; /* >0 for repeat(auto-fill/auto-fit, minmax(Npx, 1fr)) */
    int32_t            grid_col_span;   /* >=1 for grid-column: span N */
    uint8_t            grid_track_count;/* >0 for explicit track list e.g. "240px 1fr" */
    int32_t            grid_track_specs[ARCR_MAX_GRID_TRACKS]; /* >0 = px, <0 = negated fr, 0 = auto */

    /* 2D CSS Transforms (applied during layout/paint & @keyframes interpolation) */
    int32_t            translate_x_px;
    int32_t            translate_y_px;
    int32_t            translate_x_pct; /* e.g. -50 for translateX(-50%) */
    int32_t            translate_y_pct;
    int32_t            scale_x_milli;   /* 1000 = 1.0 */
    int32_t            scale_y_milli;   /* 1000 = 1.0 */
    int32_t            rotate_deg_milli;/* degrees * 1000 */

    /* CSS @keyframes Animation & Transition */
    ArcrAtom           anim_name;
    int32_t            anim_duration_ms;
    int32_t            anim_delay_ms;
    int32_t            anim_iterations; /* <0 = infinite, >=1 = finite count */
    bool               anim_alternate;
    int32_t            transition_duration_ms;

    uint32_t           dependency_flags;
} ArcrComputedStyle;

#define ARCR_STYLE_DEP_EM        (1u << 0)
#define ARCR_STYLE_DEP_VAR       (1u << 1)
#define ARCR_STYLE_DEP_MEDIA     (1u << 2)
#define ARCR_STYLE_DEP_STATE     (1u << 3)
#define ARCR_STYLE_DEP_ANIM      (1u << 4)
#define ARCR_STYLE_DEP_CONTAINER (1u << 5)

typedef struct ArcrCssMediaEnv {
    int32_t viewport_width;
    int32_t viewport_height;
    bool    prefers_dark;
} ArcrCssMediaEnv;

typedef struct ArcrStylesheet ArcrStylesheet;

ArcrStylesheet *arcr_css_parse_document_styles(ArcrDocument *doc);
ArcrStylesheet *arcr_css_parse_document_styles_ex(ArcrDocument *doc, const ArcrCssMediaEnv *env);
void            arcr_css_destroy_stylesheet(ArcrStylesheet *sheet);

void arcr_css_resolve_tree(ArcrDocument *doc, ArcrStylesheet *sheet,
                           size_t *out_style_share_hits);
void arcr_css_resolve_tree_animated(ArcrDocument *doc, ArcrStylesheet *sheet,
                                    uint64_t anim_elapsed_ms,
                                    size_t *out_style_share_hits,
                                    bool *out_has_active_animations);

bool arcr_css_parse_color(const char *str, size_t len, uint32_t *out_argb);

/* Truthful CSS @supports / CSS.supports() evaluation against supported engine properties & values */
bool arcr_css_supports_declaration(const char *prop, size_t prop_len,
                                   const char *val, size_t val_len);
bool arcr_css_supports_condition(const char *cond, size_t cond_len);

#endif /* ARCR_CSS_H */
