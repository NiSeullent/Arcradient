/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * layout/arcr_layout.c - O(N) Memoized Intrinsic Sizing, Block/Flex/Grid/Table,
 *                        Positioned/Fixed/Sticky, 2D Transforms, Overflow Clip & Hit-Test
 */
#include "arcr_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Must match css/arcr_css.c positioned percentage offset encoding */
#define ARCR_POS_PCT_BIAS      (-100000000)
#define ARCR_POS_PCT_THRESHOLD (-99000000)
#define ARCR_LAYOUT_COORD_MAX  (10000000)

typedef struct DeferredPosItem {
    const ArcrDomNode *node;
    ArcrLayoutBox     *attach_parent;
    int32_t            static_x;
    int32_t            static_y;
    int32_t            static_avail_w;
    bool               is_fixed;
} DeferredPosItem;

typedef struct LayoutCtx {
    ArcrLayoutTree     *tree;
    const ArcrDocument *doc;
    ArcrFontSystem     *fonts;
    int32_t            *intrinsic_w_cache;
    uint32_t            cache_cap;
    DeferredPosItem    *deferred;
    size_t              deferred_cnt;
    size_t              deferred_cap;
} LayoutCtx;

static int32_t clamp_coord(int64_t v) {
    if (v > (int64_t)ARCR_LAYOUT_COORD_MAX)  return ARCR_LAYOUT_COORD_MAX;
    if (v < (int64_t)-ARCR_LAYOUT_COORD_MAX) return -ARCR_LAYOUT_COORD_MAX;
    return (int32_t)v;
}

static int32_t zpx(int32_t px, int32_t zoom) {
    if (px == 0 || zoom == 100) return px;
    int64_t num    = (int64_t)px * (int64_t)zoom + ((px > 0) ? 50LL : -50LL);
    int32_t scaled = clamp_coord(num / 100LL);
    if (px > 0 && scaled < 1)  scaled = 1;
    if (px < 0 && scaled > -1) scaled = -1;
    return scaled;
}

static int32_t zoom_pos_offset(int32_t encoded, int32_t zoom) {
    if (encoded <= ARCR_POS_PCT_THRESHOLD) return encoded;
    return zpx(encoded, zoom);
}

static int32_t resolve_pos_offset(int32_t encoded, int32_t ref_px) {
    if (encoded <= ARCR_POS_PCT_THRESHOLD) {
        int32_t pct = encoded - ARCR_POS_PCT_BIAS;
        return clamp_coord(((int64_t)ref_px * (int64_t)pct) / 100LL);
    }
    return clamp_coord(encoded);
}

static const ArcrComputedStyle *zoom_style_if_needed(ArcrArena *arena,
                                                     const ArcrComputedStyle *src,
                                                     int32_t zoom) {
    if (!src || zoom == 100) return src;
    ArcrComputedStyle *zs = (ArcrComputedStyle *)arcr_arena_alloc(
        arena, sizeof(ArcrComputedStyle), sizeof(void *));
    if (!zs) return src;
    *zs = *src;

    zs->font_px         = ARCR_MAX(8, zpx(src->font_px, zoom));
    if (src->line_height_px > 0) zs->line_height_px = zpx(src->line_height_px, zoom);
    zs->border_radius   = (src->border_radius >= 999) ? src->border_radius : zpx(src->border_radius, zoom);
    zs->shadow_blur     = zpx(src->shadow_blur, zoom);
    zs->shadow_offset_y = zpx(src->shadow_offset_y, zoom);
    zs->gap_px          = zpx(src->gap_px, zoom);
    zs->row_gap_px      = zpx(src->row_gap_px, zoom);
    if (src->grid_min_col_px > 0) zs->grid_min_col_px = zpx(src->grid_min_col_px, zoom);
    for (uint8_t t = 0u; t < src->grid_track_count && t < ARCR_MAX_GRID_TRACKS; ++t) {
        if (src->grid_track_specs[t] > 0) {
            zs->grid_track_specs[t] = zpx(src->grid_track_specs[t], zoom);
        }
    }

    zs->margin.top    = zpx(src->margin.top, zoom);
    zs->margin.right  = zpx(src->margin.right, zoom);
    zs->margin.bottom = zpx(src->margin.bottom, zoom);
    zs->margin.left   = zpx(src->margin.left, zoom);

    zs->padding.top    = zpx(src->padding.top, zoom);
    zs->padding.right  = zpx(src->padding.right, zoom);
    zs->padding.bottom = zpx(src->padding.bottom, zoom);
    zs->padding.left   = zpx(src->padding.left, zoom);

    zs->border.top    = zpx(src->border.top, zoom);
    zs->border.right  = zpx(src->border.right, zoom);
    zs->border.bottom = zpx(src->border.bottom, zoom);
    zs->border.left   = zpx(src->border.left, zoom);

    if (src->width_spec > 0)      zs->width_spec      = zpx(src->width_spec, zoom);
    if (src->min_width_px > 0)    zs->min_width_px    = zpx(src->min_width_px, zoom);
    if (src->max_width_px > 0)    zs->max_width_px    = zpx(src->max_width_px, zoom);
    if (src->height_px > 0)       zs->height_px       = zpx(src->height_px, zoom);
    if (src->min_height_px > 0)   zs->min_height_px   = zpx(src->min_height_px, zoom);
    if (src->max_height_px > 0)   zs->max_height_px   = zpx(src->max_height_px, zoom);
    if (src->flex_basis_spec > 0) zs->flex_basis_spec = zpx(src->flex_basis_spec, zoom);

    if (src->has_top)    zs->top_px    = zoom_pos_offset(src->top_px, zoom);
    if (src->has_right)  zs->right_px  = zoom_pos_offset(src->right_px, zoom);
    if (src->has_bottom) zs->bottom_px = zoom_pos_offset(src->bottom_px, zoom);
    if (src->has_left)   zs->left_px   = zoom_pos_offset(src->left_px, zoom);

    zs->translate_x_px = zpx(src->translate_x_px, zoom);
    zs->translate_y_px = zpx(src->translate_y_px, zoom);
    return zs;
}

static void push_deferred_pos(LayoutCtx *ctx, const ArcrDomNode *node,
                              ArcrLayoutBox *attach_parent,
                              int32_t sx, int32_t sy, int32_t savail_w, bool is_fixed) {
    if (!ctx || !node || !attach_parent) return;
    if (ctx->deferred_cnt >= ctx->deferred_cap) {
        size_t ncap = (ctx->deferred_cap == 0u) ? 32u : (ctx->deferred_cap * 2u);
        DeferredPosItem *grown = (DeferredPosItem *)realloc(
            ctx->deferred, ncap * sizeof(DeferredPosItem));
        if (!grown) return;
        ctx->deferred     = grown;
        ctx->deferred_cap = ncap;
    }
    ctx->deferred[ctx->deferred_cnt++] = (DeferredPosItem){
        node, attach_parent, sx, sy, savail_w, is_fixed
    };
}

static bool is_out_of_flow_el(const ArcrDomNode *n) {
    if (!n || n->kind != ARCR_NODE_ELEMENT || !n->computed_style) return false;
    if (n->computed_style->display == ARCR_DISPLAY_NONE ||
        n->computed_style->display == ARCR_DISPLAY_CONTENTS) {
        return false;
    }
    return n->computed_style->position == ARCR_POS_ABSOLUTE ||
           n->computed_style->position == ARCR_POS_FIXED;
}

static void offset_box_recursive(ArcrLayoutBox *box, int32_t dx, int32_t dy) {
    if (!box || (dx == 0 && dy == 0)) return;
    box->border_rect.x  = clamp_coord((int64_t)box->border_rect.x + dx);
    box->border_rect.y  = clamp_coord((int64_t)box->border_rect.y + dy);
    box->content_rect.x = clamp_coord((int64_t)box->content_rect.x + dx);
    box->content_rect.y = clamp_coord((int64_t)box->content_rect.y + dy);

    for (uint32_t i = 0u; i < box->glyph_count; ++i) {
        box->glyphs[i].x          = clamp_coord((int64_t)box->glyphs[i].x + dx);
        box->glyphs[i].baseline_y = clamp_coord((int64_t)box->glyphs[i].baseline_y + dy);
    }
    for (ArcrInlineBgPill *p = box->pills_head; p != NULL; p = p->next) {
        p->rect.x = clamp_coord((int64_t)p->rect.x + dx);
        p->rect.y = clamp_coord((int64_t)p->rect.y + dy);
    }
    for (ArcrInlineLinkSpan *l = box->links_head; l != NULL; l = l->next) {
        l->rect.x = clamp_coord((int64_t)l->rect.x + dx);
        l->rect.y = clamp_coord((int64_t)l->rect.y + dy);
    }
    for (ArcrLayoutBox *ch = box->first_child; ch != NULL; ch = ch->next_sibling) {
        /* Fixed descendants stay anchored to viewport even if normal ancestor shifts */
        if (ch->style && ch->style->position == ARCR_POS_FIXED) continue;
        offset_box_recursive(ch, dx, dy);
    }
}

static void append_child_box(ArcrLayoutBox *parent, ArcrLayoutBox *child) {
    if (!parent || !child) return;
    child->next_sibling = NULL;
    if (!parent->first_child) {
        parent->first_child = child;
        parent->last_child  = child;
    } else {
        parent->last_child->next_sibling = child;
        parent->last_child               = child;
    }
}

static void append_glyphs_to_box(ArcrLayoutTree *tree, ArcrLayoutBox *box,
                                 const ArcrPositionedGlyph *new_g, uint32_t new_cnt) {
    if (!box || !new_g || new_cnt == 0u) return;
    if (box->glyph_count == 0u) {
        ArcrPositionedGlyph *arr = (ArcrPositionedGlyph *)arcr_arena_alloc(
            &tree->arena, (size_t)new_cnt * sizeof(ArcrPositionedGlyph), sizeof(void *));
        if (!arr) return;
        memcpy(arr, new_g, (size_t)new_cnt * sizeof(ArcrPositionedGlyph));
        box->glyphs      = arr;
        box->glyph_count = new_cnt;
        return;
    }
    uint32_t total = box->glyph_count + new_cnt;
    ArcrPositionedGlyph *merged = (ArcrPositionedGlyph *)arcr_arena_alloc(
        &tree->arena, (size_t)total * sizeof(ArcrPositionedGlyph), sizeof(void *));
    if (!merged) return;
    memcpy(merged, box->glyphs, (size_t)box->glyph_count * sizeof(ArcrPositionedGlyph));
    memcpy(merged + box->glyph_count, new_g, (size_t)new_cnt * sizeof(ArcrPositionedGlyph));
    box->glyphs      = merged;
    box->glyph_count = total;
}

static uint32_t apply_text_transform_cp(uint32_t cp, uint8_t tt) {
    if (tt == 1u) { /* uppercase */
        if (cp >= 'a' && cp <= 'z') return cp - 32u;
        if (cp >= 0xE0u && cp <= 0xFEu && cp != 0xF7u) return cp - 32u;
    } else if (tt == 2u) { /* lowercase */
        if (cp >= 'A' && cp <= 'Z') return cp + 32u;
        if (cp >= 0xC0u && cp <= 0xDEu && cp != 0xD7u) return cp + 32u;
    }
    return cp;
}

static int32_t compute_style_line_h(const ArcrComputedStyle *s) {
    if (!s) return 18;
    if (s->line_height_px > 0) return ARCR_MAX(1, s->line_height_px);
    int32_t lh_pct = (s->line_height_pct >= 90) ? s->line_height_pct : 145;
    return ARCR_MAX(14, (s->font_px * lh_pct) / 100);
}

static int32_t natural_glyph_floor_px(uint32_t mcp, int32_t font_px, bool monospace) {
    if (monospace) {
        return ARCR_MAX(4, (font_px * 52 + 50) / 100);
    }
    if (mcp == ' ' || mcp == 'i' || mcp == 'l' || mcp == 'j' || mcp == 'I' ||
        mcp == '!' || mcp == '|' || mcp == '.' || mcp == ',' || mcp == ':' ||
        mcp == ';' || mcp == '\'' || mcp == '`') {
        return ARCR_MAX(3, (font_px * 26 + 50) / 100);
    }
    if (mcp == 't' || mcp == 'f' || mcp == 'r' || mcp == '(' || mcp == ')' ||
        mcp == '[' || mcp == ']' || mcp == '-') {
        return ARCR_MAX(4, (font_px * 35 + 50) / 100);
    }
    if (mcp == 'W' || mcp == 'M' || mcp == 'w' || mcp == 'm' || mcp == '@' || mcp == '%') {
        return ARCR_MAX(6, (font_px * 68 + 50) / 100);
    }
    if (mcp >= 'A' && mcp <= 'Z') {
        return ARCR_MAX(5, (font_px * 58 + 50) / 100);
    }
    if (mcp > 0x20u && mcp < 0x7Fu) {
        return ARCR_MAX(5, (font_px * 52 + 50) / 100);
    }
    if ((mcp >= 0x1100u && mcp <= 0x11FFu) || (mcp >= 0x2E80u && mcp <= 0x9FFFu) ||
        (mcp >= 0xAC00u && mcp <= 0xD7AFu) || (mcp >= 0xF900u && mcp <= 0xFAFFu) ||
        (mcp >= 0xFF01u && mcp <= 0xFF60u)) {
        return ARCR_MAX(8, (font_px * 92 + 50) / 100);
    }
    return 1;
}

static int32_t measure_codepoint_adv(ArcrFontSystem *fonts, uint32_t cp,
                                     int32_t font_px, bool bold, bool monospace) {
    uint32_t mcp     = (cp == 0x00A0u) ? (uint32_t)' ' : cp;
    int32_t  min_adv = natural_glyph_floor_px(mcp, font_px, monospace);
    ArcrGlyphMetrics gm;
    if (arcr_font_measure_glyph_ex(fonts, mcp, font_px, bold, monospace, &gm)) {
        return ARCR_MAX(min_adv, gm.advance_px);
    }
    return ARCR_MAX(min_adv, ARCR_MAX(4, font_px / 2));
}

static int32_t measure_utf8_string_px_ex(ArcrFontSystem *fonts, const char *s, size_t len,
                                         int32_t font_px, bool bold, bool monospace,
                                         bool collapse_ws, uint8_t text_transform) {
    if (!s || len == 0u) return 0;
    int32_t w       = 0;
    size_t  pos     = 0u;
    bool    prev_ws = false;
    while (pos < len) {
        uint32_t cp = 0u;
        size_t step = arcr_utf8_decode_one(s + pos, len - pos, &cp);
        if (step == 0u) break;
        pos += step;
        if (cp == '\r') continue;
        if (collapse_ws && (cp == ' ' || cp == '\t' || cp == '\n')) {
            if (prev_ws) continue;
            prev_ws = true;
            cp      = ' ';
        } else {
            if (cp == '\n') continue;
            prev_ws = false;
        }
        cp = apply_text_transform_cp(cp, text_transform);
        w  = clamp_coord((int64_t)w + measure_codepoint_adv(fonts, cp, font_px, bold, monospace));
    }
    return w;
}

static int32_t resolve_spec_px(int32_t spec, int32_t ref_w) {
    if (spec > 0) return clamp_coord(spec);
    if (spec < 0) return clamp_coord(((int64_t)ref_w * (int64_t)(-spec)) / 100LL);
    return 0;
}

static int32_t css_w_to_border_w(int32_t raw_w, const ArcrComputedStyle *st, int32_t h_chrome) {
    if (raw_w <= 0) return 0;
    if (st->box_sizing == ARCR_BOX_BORDER) {
        return ARCR_MAX(h_chrome, raw_w);
    }
    return clamp_coord((int64_t)raw_w + h_chrome);
}

static int32_t css_h_to_border_h(int32_t raw_h, const ArcrComputedStyle *st, int32_t v_chrome) {
    if (raw_h <= 0) return 0;
    if (st->box_sizing == ARCR_BOX_BORDER) {
        return ARCR_MAX(v_chrome, raw_h);
    }
    return clamp_coord((int64_t)raw_h + v_chrome);
}

static int32_t apply_min_max_w(int32_t bw, const ArcrComputedStyle *st,
                               int32_t avail_w, int32_t h_chrome) {
    int32_t min_raw = resolve_spec_px(st->min_width_px, avail_w);
    int32_t max_raw = resolve_spec_px(st->max_width_px, avail_w);
    int32_t min_bw  = (min_raw > 0) ? css_w_to_border_w(min_raw, st, h_chrome) : 0;
    int32_t max_bw  = (max_raw > 0) ? css_w_to_border_w(max_raw, st, h_chrome) : 0;
    if (max_bw > 0 && bw > max_bw) bw = max_bw;
    if (min_bw > 0 && bw < min_bw) bw = min_bw;
    return bw;
}

static void query_svg_or_media_intrinsic(const ArcrDocument *doc, const ArcrDomNode *node,
                                         bool is_svg, bool is_canvas, bool is_video,
                                         int32_t zoom, int32_t *out_nw, int32_t *out_nh) {
    int32_t nw = is_video ? 480 : (is_canvas ? 300 : (is_svg ? 24 : 120));
    int32_t nh = is_video ? 270 : (is_canvas ? 150 : (is_svg ? 24 : 80));

    if (!is_svg && node->surface && node->surface->width > 0 && node->surface->height > 0) {
        nw = node->surface->width;
        nh = node->surface->height;
    } else {
        const char *wa = arcr_dom_get_attr_cstr(doc, node, "width");
        const char *ha = arcr_dom_get_attr_cstr(doc, node, "height");
        int32_t pw = 0, ph = 0;
        bool got_w = false, got_h = false;
        if (wa && arcr_parse_i32(wa, strlen(wa), &pw) && pw > 0) { nw = pw; got_w = true; }
        if (ha && arcr_parse_i32(ha, strlen(ha), &ph) && ph > 0) { nh = ph; got_h = true; }
        if (is_svg && (!got_w || !got_h)) {
            const char *vb = arcr_dom_get_attr_cstr(doc, node, "viewBox");
            if (!vb) vb = arcr_dom_get_attr_cstr(doc, node, "viewbox");
            if (vb) {
                /* Parse 4 numbers: min-x min-y vb_w vb_h */
                double nums[4] = {0, 0, 0, 0};
                int ncnt = 0;
                const char *p = vb;
                while (*p && ncnt < 4) {
                    while (*p == ' ' || *p == '\t' || *p == ',') ++p;
                    if (!*p) break;
                    char *endp = NULL;
                    double v = strtod(p, &endp);
                    if (endp == p) break;
                    nums[ncnt++] = v;
                    p = endp;
                }
                if (ncnt == 4 && nums[2] > 0.5 && nums[3] > 0.5) {
                    int32_t vbw = (int32_t)(nums[2] + 0.5);
                    int32_t vbh = (int32_t)(nums[3] + 0.5);
                    if (!got_w && !got_h) {
                        /* Icon-sized SVGs keep crisp size; huge viewBoxes cap to sensible default */
                        if (vbw <= 64 && vbh <= 64) {
                            nw = vbw;
                            nh = vbh;
                        } else {
                            nw = 24;
                            nh = ARCR_MAX(1, (24 * vbh) / vbw);
                        }
                    } else if (got_w && !got_h) {
                        nh = ARCR_MAX(1, (int32_t)(((int64_t)nw * vbh) / vbw));
                    } else if (!got_w && got_h) {
                        nw = ARCR_MAX(1, (int32_t)(((int64_t)nh * vbw) / vbh));
                    }
                    got_w = true;
                    got_h = true;
                }
            }
            if (!got_w && !got_h && node->svg_rendered_w == 0 &&
                node->surface && node->surface->width > 0 && node->surface->height > 0) {
                nw = node->surface->width;
                nh = node->surface->height;
            }
        }
    }
    *out_nw = zpx(nw, zoom);
    *out_nh = zpx(nh, zoom);
}

/* Forward declaration of master element layout function */
static ArcrLayoutBox *layout_element_node(LayoutCtx *ctx, const ArcrDomNode *node,
                                          ArcrLayoutBox *pos_owner,
                                          int32_t avail_x, int32_t start_y, int32_t avail_w,
                                          int32_t parent_def_h,
                                          bool shrink_to_fit, int32_t forced_border_w,
                                          int32_t list_ordinal);

static int32_t measure_unconstrained_natural_w(LayoutCtx *ctx, const ArcrDomNode *node);

static void measure_children_into(LayoutCtx *ctx, const ArcrDomNode *parent,
                                  const ArcrComputedStyle *container_st, int32_t zoom,
                                  bool is_flex_row, int32_t flex_gap,
                                  int32_t *inout_cur_line, int32_t *inout_best_w,
                                  int32_t *inout_flex_cnt) {
    const ArcrDomNode *tree_parent =
        (parent && parent->kind == ARCR_NODE_ELEMENT && parent->shadow_root)
            ? parent->shadow_root
            : parent;
    for (const ArcrDomNode *ch = tree_parent ? tree_parent->first_child : NULL;
         ch != NULL; ch = ch->next_sibling) {
        if (ch->kind == ARCR_NODE_TEXT) {
            if (is_flex_row) continue;
            int32_t font_px = ARCR_MAX(8, zpx(container_st->font_px, zoom));
            int32_t tw = measure_utf8_string_px_ex(
                ctx->fonts, ch->text, ch->text_len, font_px,
                container_st->font_bold, container_st->font_monospace,
                !container_st->white_space_pre, container_st->text_transform);
            *inout_cur_line = clamp_coord((int64_t)(*inout_cur_line) + tw);
            if (*inout_cur_line > *inout_best_w) *inout_best_w = *inout_cur_line;
        } else if (ch->kind == ARCR_NODE_ELEMENT) {
            if (arcr_str_eq_nocase(arcr_atom_cstr(ctx->doc->atoms, ch->tag), "br")) {
                *inout_cur_line = 0;
                continue;
            }
            if (!ch->computed_style || ch->computed_style->display == ARCR_DISPLAY_NONE) continue;
            if (is_out_of_flow_el(ch)) continue;

            if (ch->computed_style->display == ARCR_DISPLAY_CONTENTS) {
                measure_children_into(ctx, ch, ch->computed_style, zoom, is_flex_row, flex_gap,
                                      inout_cur_line, inout_best_w, inout_flex_cnt);
                continue;
            }

            int32_t sub   = measure_unconstrained_natural_w(ctx, ch);
            int32_t sub_m = zpx(ch->computed_style->margin.left +
                                ch->computed_style->margin.right, zoom);
            int32_t outer = clamp_coord((int64_t)sub + sub_m);

            if (is_flex_row) {
                if (*inout_flex_cnt > 0) {
                    *inout_best_w = clamp_coord((int64_t)(*inout_best_w) + flex_gap);
                }
                *inout_best_w = clamp_coord((int64_t)(*inout_best_w) + outer);
                (*inout_flex_cnt)++;
            } else if (ch->computed_style->display == ARCR_DISPLAY_INLINE ||
                       ch->computed_style->display == ARCR_DISPLAY_INLINE_BLOCK ||
                       ch->computed_style->display == ARCR_DISPLAY_INLINE_FLEX) {
                *inout_cur_line = clamp_coord((int64_t)(*inout_cur_line) + outer);
                if (*inout_cur_line > *inout_best_w) *inout_best_w = *inout_cur_line;
            } else {
                *inout_cur_line = 0;
                if (outer > *inout_best_w) *inout_best_w = outer;
            }
        }
    }
}

/*
 * Strictly O(N) memoized intrinsic border-box width measurement!
 * Eliminates the O(2^D) nested-flex double-recursion freeze on modern web apps.
 */
static int32_t measure_unconstrained_natural_w(LayoutCtx *ctx, const ArcrDomNode *node) {
    if (!node || node->kind != ARCR_NODE_ELEMENT || !node->computed_style) return 0;
    const ArcrComputedStyle *raw_st = node->computed_style;
    if (raw_st->display == ARCR_DISPLAY_NONE) return 0;

    if (ctx->intrinsic_w_cache && node->node_uid < ctx->cache_cap &&
        ctx->intrinsic_w_cache[node->node_uid] >= 0) {
        return ctx->intrinsic_w_cache[node->node_uid];
    }

    int32_t zoom     = ctx->tree->zoom_percent;
    int32_t h_chrome = zpx(raw_st->border.left + raw_st->border.right +
                           raw_st->padding.left + raw_st->padding.right, zoom);

    if (raw_st->width_spec > 0) {
        int32_t res = css_w_to_border_w(zpx(raw_st->width_spec, zoom), raw_st, h_chrome);
        if (ctx->intrinsic_w_cache && node->node_uid < ctx->cache_cap) {
            ctx->intrinsic_w_cache[node->node_uid] = res;
        }
        return res;
    }

    const char *tname = arcr_atom_cstr(ctx->doc->atoms, node->tag);
    int32_t font_px   = ARCR_MAX(8, zpx(raw_st->font_px, zoom));

    bool is_img    = arcr_str_eq_nocase(tname, "img");
    bool is_svg    = arcr_str_eq_nocase(tname, "svg");
    bool is_canvas = arcr_str_eq_nocase(tname, "canvas");
    bool is_video  = arcr_str_eq_nocase(tname, "video");

    int32_t result = 0;

    if (is_img || is_svg || is_canvas || is_video) {
        int32_t nw = 0, nh = 0;
        query_svg_or_media_intrinsic(ctx->doc, node, is_svg, is_canvas, is_video, zoom, &nw, &nh);
        if (raw_st->height_px > 0 && nh > 0) {
            int32_t v_chrome = zpx(raw_st->border.top + raw_st->border.bottom +
                                   raw_st->padding.top + raw_st->padding.bottom, zoom);
            int32_t bh = css_h_to_border_h(zpx(raw_st->height_px, zoom), raw_st, v_chrome);
            int32_t ch = ARCR_MAX(1, bh - v_chrome);
            nw = clamp_coord(((int64_t)ch * nw) / nh);
        }
        result = clamp_coord((int64_t)nw + h_chrome);
    } else if (arcr_str_eq_nocase(tname, "input")) {
        const char *tp = arcr_dom_get_attr_cstr(ctx->doc, node, "type");
        if (tp && (arcr_str_eq_nocase(tp, "checkbox") || arcr_str_eq_nocase(tp, "radio"))) {
            result = zpx(18, zoom);
        } else if (tp && (arcr_str_eq_nocase(tp, "submit") || arcr_str_eq_nocase(tp, "button") ||
                          arcr_str_eq_nocase(tp, "reset"))) {
            const char *lbl = arcr_dom_get_attr_cstr(ctx->doc, node, "value");
            if (!lbl || lbl[0] == '\0') lbl = "Submit";
            int32_t tw = measure_utf8_string_px_ex(
                ctx->fonts, lbl, strlen(lbl), font_px,
                raw_st->font_bold, raw_st->font_monospace, true, raw_st->text_transform);
            result = ARCR_MAX(zpx(48, zoom), tw + h_chrome + zpx(8, zoom));
        } else {
            result = zpx(220, zoom);
        }
    } else if (arcr_str_eq_nocase(tname, "textarea")) {
        result = zpx(300, zoom);
    } else {
        bool is_flex_row = ((raw_st->display == ARCR_DISPLAY_FLEX ||
                             raw_st->display == ARCR_DISPLAY_INLINE_FLEX) &&
                            (raw_st->flex_direction == ARCR_FLEX_ROW ||
                             raw_st->flex_direction == ARCR_FLEX_ROW_REVERSE));
        int32_t cur_line = 0;
        int32_t best_w   = 0;
        int32_t flex_cnt = 0;
        int32_t flex_gap = zpx(ARCR_MAX(0, raw_st->gap_px), zoom);
        measure_children_into(ctx, node, raw_st, zoom, is_flex_row, flex_gap,
                              &cur_line, &best_w, &flex_cnt);

        result = clamp_coord((int64_t)best_w + h_chrome);
        if (arcr_str_eq_nocase(tname, "button")) {
            result = ARCR_MAX(result + zpx(4, zoom), zpx(36, zoom));
        }
    }

    if (raw_st->min_width_px > 0) {
        int32_t min_bw = css_w_to_border_w(zpx(raw_st->min_width_px, zoom), raw_st, h_chrome);
        if (result < min_bw) result = min_bw;
    }
    if (raw_st->max_width_px > 0) {
        int32_t max_bw = css_w_to_border_w(zpx(raw_st->max_width_px, zoom), raw_st, h_chrome);
        if (result > max_bw) result = max_bw;
    }

    if (ctx->intrinsic_w_cache && node->node_uid < ctx->cache_cap) {
        ctx->intrinsic_w_cache[node->node_uid] = result;
    }
    return result;
}

static int32_t measure_natural_border_w(LayoutCtx *ctx, const ArcrDomNode *node, int32_t max_w) {
    if (!node || node->kind != ARCR_NODE_ELEMENT || !node->computed_style) return 0;
    const ArcrComputedStyle *raw_st = node->computed_style;
    if (raw_st->display == ARCR_DISPLAY_NONE) return 0;
    int32_t zoom     = ctx->tree->zoom_percent;
    int32_t h_chrome = zpx(raw_st->border.left + raw_st->border.right +
                           raw_st->padding.left + raw_st->padding.right, zoom);
    if (raw_st->width_spec < 0) {
        int32_t pw = resolve_spec_px(raw_st->width_spec, max_w);
        int32_t bw = css_w_to_border_w(pw, raw_st, h_chrome);
        return ARCR_CLAMP(bw, ARCR_MAX(4, h_chrome), ARCR_MAX(4, max_w));
    }
    int32_t nat = measure_unconstrained_natural_w(ctx, node);
    return ARCR_CLAMP(nat, ARCR_MAX(4, h_chrome), ARCR_MAX(4, max_w));
}

/* -------------------------------------------------------------------------
 * Inline Formatting Context (Korean/CJK Wrap, Ellipsis, Links, Pills, Inline-Flex)
 * ------------------------------------------------------------------------- */
typedef struct InlineItem {
    uint32_t                 cp;
    int32_t                  adv_px;
    const ArcrComputedStyle *style;
    const ArcrDomNode       *link_node;
    const char              *link_href;
    ArcrLayoutBox           *inline_box;
    bool                     forced_break;
} InlineItem;

typedef struct InlineBuf {
    InlineItem *items;
    size_t      count;
    size_t      cap;
} InlineBuf;

static bool inline_buf_push(InlineBuf *buf, InlineItem item) {
    if (buf->count >= buf->cap) {
        size_t ncap = (buf->cap == 0u) ? 256u : (buf->cap * 2u);
        InlineItem *grown = (InlineItem *)realloc(buf->items, ncap * sizeof(InlineItem));
        if (!grown) return false;
        buf->items = grown;
        buf->cap   = ncap;
    }
    buf->items[buf->count++] = item;
    return true;
}

static bool is_inline_flow_child(const ArcrAtomTable *atoms, const ArcrDomNode *n) {
    if (!n) return false;
    if (n->kind == ARCR_NODE_TEXT) return true;
    if (n->kind == ARCR_NODE_COMMENT) return true;
    if (n->kind == ARCR_NODE_ELEMENT) {
        if (is_out_of_flow_el(n)) return false;
        if (arcr_str_eq_nocase(arcr_atom_cstr(atoms, n->tag), "br")) return true;
        if (!n->computed_style) return false;
        if (n->computed_style->display == ARCR_DISPLAY_NONE) return true;
        if (n->computed_style->display == ARCR_DISPLAY_CONTENTS) {
            for (const ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
                if (!is_inline_flow_child(atoms, ch)) return false;
            }
            return true;
        }
        return n->computed_style->display == ARCR_DISPLAY_INLINE ||
               n->computed_style->display == ARCR_DISPLAY_INLINE_BLOCK ||
               n->computed_style->display == ARCR_DISPLAY_INLINE_FLEX;
    }
    return false;
}

static void collect_inline_items(LayoutCtx *ctx, const ArcrDomNode *node,
                                 ArcrLayoutBox *pos_owner,
                                 const ArcrComputedStyle *cur_style,
                                 const ArcrDomNode *active_link, const char *active_href,
                                 int32_t line_max_w, InlineBuf *buf) {
    if (!node || node->kind == ARCR_NODE_COMMENT) return;

    if (node->kind == ARCR_NODE_TEXT) {
        size_t pos = 0u;
        bool prev_was_space = (buf->count > 0u && !buf->items[buf->count - 1u].inline_box &&
                               (buf->items[buf->count - 1u].cp == ' ' ||
                                buf->items[buf->count - 1u].forced_break));
        while (pos < node->text_len) {
            uint32_t cp = 0u;
            size_t step = arcr_utf8_decode_one(node->text + pos, node->text_len - pos, &cp);
            if (step == 0u) break;
            pos += step;

            if (cp == '\r') continue;
            if (!cur_style->white_space_pre && (cp == ' ' || cp == '\t' || cp == '\n')) {
                if (prev_was_space) continue;
                prev_was_space = true;
                int32_t sp_adv = measure_codepoint_adv(
                    ctx->fonts, ' ', cur_style->font_px, cur_style->font_bold,
                    cur_style->font_monospace);
                inline_buf_push(buf, (InlineItem){
                    ' ', sp_adv, cur_style, active_link, active_href, NULL, false
                });
                continue;
            }
            prev_was_space = false;

            if (cp == '\n') {
                inline_buf_push(buf, (InlineItem){
                    '\n', 0, cur_style, active_link, active_href, NULL, true
                });
                prev_was_space = true;
                continue;
            }
            if (cp == '\t' && cur_style->white_space_pre) {
                int32_t sp_adv = measure_codepoint_adv(
                    ctx->fonts, ' ', cur_style->font_px, cur_style->font_bold,
                    cur_style->font_monospace);
                for (int t = 0; t < 4; ++t) {
                    inline_buf_push(buf, (InlineItem){
                        ' ', sp_adv, cur_style, active_link, active_href, NULL, false
                    });
                }
                continue;
            }

            cp = apply_text_transform_cp(cp, cur_style->text_transform);
            int32_t adv = measure_codepoint_adv(
                ctx->fonts, cp, cur_style->font_px, cur_style->font_bold,
                cur_style->font_monospace);
            inline_buf_push(buf, (InlineItem){
                cp, adv, cur_style, active_link, active_href, NULL, false
            });
        }
        return;
    }

    if (node->kind == ARCR_NODE_ELEMENT) {
        if (node->computed_style && node->computed_style->display == ARCR_DISPLAY_NONE) return;
        if (is_out_of_flow_el(node)) {
            ArcrLayoutBox *owner = (node->computed_style->position == ARCR_POS_FIXED)
                                       ? ctx->tree->root_box
                                       : pos_owner;
            push_deferred_pos(ctx, node, owner, 0, 0, line_max_w,
                              node->computed_style->position == ARCR_POS_FIXED);
            return;
        }

        const char *tname = arcr_atom_cstr(ctx->doc->atoms, node->tag);
        if (arcr_str_eq_nocase(tname, "br")) {
            inline_buf_push(buf, (InlineItem){
                '\n', 0, cur_style, active_link, active_href, NULL, true
            });
            return;
        }

        const ArcrComputedStyle *st = node->computed_style
                                          ? zoom_style_if_needed(&ctx->tree->arena,
                                                                 node->computed_style,
                                                                 ctx->tree->zoom_percent)
                                          : cur_style;
        if (st->sr_only_clip) return;

        if (arcr_str_eq_nocase(tname, "a")) {
            const char *href = arcr_dom_get_attr_cstr(ctx->doc, node, "href");
            if (href) {
                active_link = node;
                active_href = href;
            }
        }

        const ArcrDomNode *inl_parent = node->shadow_root ? node->shadow_root : node;
        if (st->display == ARCR_DISPLAY_CONTENTS) {
            for (const ArcrDomNode *ch = inl_parent->first_child; ch != NULL; ch = ch->next_sibling) {
                collect_inline_items(ctx, ch, pos_owner, st, active_link, active_href,
                                     line_max_w, buf);
            }
            return;
        }

        if (st->display == ARCR_DISPLAY_INLINE_BLOCK ||
            st->display == ARCR_DISPLAY_INLINE_FLEX) {
            ArcrLayoutBox *ib = layout_element_node(ctx, node, pos_owner, 0, 0,
                                                    line_max_w, 0, true, 0, 0);
            if (ib) {
                int32_t ib_total_w = clamp_coord((int64_t)ib->style->margin.left +
                                                 ib->border_rect.width +
                                                 ib->style->margin.right);
                inline_buf_push(buf, (InlineItem){
                    0xFFFCu, ib_total_w, st, active_link, active_href, ib, false
                });
            }
            return;
        }

        if (st->padding.left > 0) {
            inline_buf_push(buf, (InlineItem){
                0x200Bu, st->padding.left, st, active_link, active_href, NULL, false
            });
        }
        for (const ArcrDomNode *ch = inl_parent->first_child; ch != NULL; ch = ch->next_sibling) {
            if (is_inline_flow_child(ctx->doc->atoms, ch) || is_out_of_flow_el(ch)) {
                collect_inline_items(ctx, ch, pos_owner, st, active_link, active_href,
                                     line_max_w, buf);
            }
        }
        if (st->padding.right > 0) {
            inline_buf_push(buf, (InlineItem){
                0x200Bu, st->padding.right, st, active_link, active_href, NULL, false
            });
        }
    }
}

static void shift_line_artifacts(ArcrLayoutBox *box, int32_t dx,
                                 uint32_t g_start, uint32_t g_end,
                                 ArcrInlineBgPill *pill_stop,
                                 ArcrInlineLinkSpan *link_stop,
                                 ArcrLayoutBox **line_boxes, size_t line_box_cnt) {
    if (dx <= 0) return;
    for (uint32_t g = g_start; g < g_end; ++g) {
        box->glyphs[g].x = clamp_coord((int64_t)box->glyphs[g].x + dx);
    }
    for (ArcrInlineBgPill *p = box->pills_head; p != pill_stop && p != NULL; p = p->next) {
        p->rect.x = clamp_coord((int64_t)p->rect.x + dx);
    }
    for (ArcrInlineLinkSpan *l = box->links_head; l != link_stop && l != NULL; l = l->next) {
        l->rect.x = clamp_coord((int64_t)l->rect.x + dx);
    }
    for (size_t b = 0u; b < line_box_cnt; ++b) {
        offset_box_recursive(line_boxes[b], dx, 0);
    }
}

static void finish_and_align_line(LayoutCtx *ctx, ArcrLayoutBox *box, int32_t start_x,
                                  int32_t *inout_pen_x, int32_t max_width,
                                  ArcrPositionedGlyph *out_g, uint32_t g_start, uint32_t *inout_g_end,
                                  ArcrInlineBgPill *pill_stop, ArcrInlineLinkSpan *link_stop,
                                  ArcrLayoutBox **line_boxes, size_t line_box_cnt) {
    /* Apply text-overflow: ellipsis when overflowing nowrap/clipped container */
    if (box->style->text_ellipsis &&
        box->style->overflow_x != ARCR_OVERFLOW_VISIBLE &&
        *inout_pen_x > max_width && *inout_g_end > g_start) {
        int32_t ell_adv = measure_codepoint_adv(
            ctx->fonts, 0x2026u, box->style->font_px, box->style->font_bold,
            box->style->font_monospace);
        int32_t limit_x = start_x + ARCR_MAX(0, max_width - ell_adv);
        uint32_t keep   = *inout_g_end;
        while (keep > g_start && out_g[keep - 1u].x + out_g[keep - 1u].advance_px > limit_x) {
            --keep;
        }
        int32_t ell_x = (keep > g_start)
                            ? (out_g[keep - 1u].x + out_g[keep - 1u].advance_px)
                            : start_x;
        ArcrPositionedGlyph sample = out_g[(keep > g_start) ? (keep - 1u) : g_start];
        sample.codepoint  = 0x2026u; /* '…' */
        sample.x          = ell_x;
        sample.advance_px = ell_adv;
        out_g[keep]       = sample;
        *inout_g_end      = keep + 1u;
        *inout_pen_x      = (ell_x + ell_adv) - start_x;
    }

    ArcrTextAlign ta = box->style->text_align;
    if (ta == ARCR_TEXT_ALIGN_LEFT || *inout_pen_x >= max_width) return;

    int32_t dx = (ta == ARCR_TEXT_ALIGN_CENTER) ? ((max_width - *inout_pen_x) / 2)
                                                : (max_width - *inout_pen_x);
    if (dx <= 0) return;
    ArcrPositionedGlyph *saved = box->glyphs;
    box->glyphs = out_g;
    shift_line_artifacts(box, dx, g_start, *inout_g_end, pill_stop, link_stop,
                         line_boxes, line_box_cnt);
    box->glyphs = saved;
}

static int32_t layout_inline_run(LayoutCtx *ctx, ArcrLayoutBox *box,
                                 const InlineBuf *buf, int32_t start_x, int32_t start_y,
                                 int32_t max_width) {
    if (buf->count == 0u) return 0;
    if (max_width < 16) max_width = 16;

    ArcrPositionedGlyph *out_g = (ArcrPositionedGlyph *)arcr_arena_alloc(
        &ctx->tree->arena, (buf->count + 4u) * sizeof(ArcrPositionedGlyph), sizeof(void *));
    if (!out_g) return 0;

    int32_t base_line_h = compute_style_line_h(box->style);
    bool    allow_wrap  = !box->style->white_space_pre && !box->style->white_space_nowrap;

    uint32_t gcount       = 0u;
    uint32_t line_g_start = 0u;
    int32_t  pen_x        = 0;
    int32_t  cur_y        = start_y;
    int32_t  cur_line_h   = base_line_h;

    ArcrInlineBgPill   *line_pill_mark = box->pills_head;
    ArcrInlineLinkSpan *line_link_mark = box->links_head;
    size_t              line_box_cap   = 64u;
    size_t              line_box_cnt   = 0u;
    ArcrLayoutBox     **line_boxes     = (ArcrLayoutBox **)malloc(
        line_box_cap * sizeof(ArcrLayoutBox *));
    if (!line_boxes) return 0;

    size_t i = 0u;
    while (i < buf->count) {
        if (pen_x == 0 && !box->style->white_space_pre &&
            buf->items[i].cp == ' ' && !buf->items[i].forced_break &&
            !buf->items[i].inline_box) {
            ++i;
            continue;
        }
        if (buf->items[i].forced_break) {
            finish_and_align_line(ctx, box, start_x, &pen_x, max_width, out_g,
                                  line_g_start, &gcount, line_pill_mark, line_link_mark,
                                  line_boxes, line_box_cnt);
            cur_y          = clamp_coord((int64_t)cur_y + cur_line_h);
            pen_x          = 0;
            cur_line_h     = base_line_h;
            line_g_start   = gcount;
            line_pill_mark = box->pills_head;
            line_link_mark = box->links_head;
            line_box_cnt   = 0u;
            ++i;
            continue;
        }

        size_t  cend           = i + 1u;
        int32_t cluster_w      = buf->items[i].adv_px;
        int32_t cluster_max_lh = base_line_h;

        if (buf->items[i].inline_box) {
            ArcrLayoutBox *ib = buf->items[i].inline_box;
            int32_t ib_h = clamp_coord((int64_t)ib->style->margin.top +
                                       ib->border_rect.height + ib->style->margin.bottom);
            if (ib_h > cluster_max_lh) cluster_max_lh = ib_h;
        } else {
            int32_t ilh = compute_style_line_h(buf->items[i].style);
            if (ilh > cluster_max_lh) cluster_max_lh = ilh;

            while (cend < buf->count && !buf->items[cend].forced_break &&
                   !buf->items[cend].inline_box) {
                if (!allow_wrap && buf->items[cend].cp == ' ') break;
                bool keep_all = buf->items[cend - 1u].style->word_break_keep_all ||
                                buf->items[cend].style->word_break_keep_all;
                if (allow_wrap &&
                    buf->items[cend - 1u].cp != 0x00A0u &&
                    buf->items[cend].cp != 0x00A0u &&
                    arcr_unicode_can_break_line(buf->items[cend - 1u].cp,
                                                buf->items[cend].cp, keep_all)) {
                    break;
                }
                cluster_w = clamp_coord((int64_t)cluster_w + buf->items[cend].adv_px);
                int32_t clh = compute_style_line_h(buf->items[cend].style);
                if (clh > cluster_max_lh) cluster_max_lh = clh;
                ++cend;
            }
        }

        if (allow_wrap && pen_x > 0 && pen_x + cluster_w > max_width) {
            finish_and_align_line(ctx, box, start_x, &pen_x, max_width, out_g,
                                  line_g_start, &gcount, line_pill_mark, line_link_mark,
                                  line_boxes, line_box_cnt);
            cur_y          = clamp_coord((int64_t)cur_y + cur_line_h);
            pen_x          = 0;
            cur_line_h     = base_line_h;
            line_g_start   = gcount;
            line_pill_mark = box->pills_head;
            line_link_mark = box->links_head;
            line_box_cnt   = 0u;
            if (buf->items[i].cp == ' ' && !buf->items[i].inline_box) {
                ++i;
                continue;
            }
        }

        if (cluster_max_lh > cur_line_h) cur_line_h = cluster_max_lh;
        int32_t baseline_y = cur_y + (cur_line_h * 78) / 100;

        for (size_t k = i; k < cend; ++k) {
            const InlineItem *it = &buf->items[k];
            if (allow_wrap && !it->inline_box &&
                pen_x > 0 && pen_x + it->adv_px > max_width) {
                finish_and_align_line(ctx, box, start_x, &pen_x, max_width, out_g,
                                      line_g_start, &gcount, line_pill_mark, line_link_mark,
                                      line_boxes, line_box_cnt);
                cur_y          = clamp_coord((int64_t)cur_y + cur_line_h);
                pen_x          = 0;
                baseline_y     = cur_y + (cur_line_h * 78) / 100;
                line_g_start   = gcount;
                line_pill_mark = box->pills_head;
                line_link_mark = box->links_head;
                line_box_cnt   = 0u;
            }

            if (it->inline_box) {
                ArcrLayoutBox *ib = it->inline_box;
                int32_t ib_x = clamp_coord((int64_t)start_x + pen_x + ib->style->margin.left);
                int32_t ib_y = clamp_coord((int64_t)cur_y +
                                           ARCR_MAX(0, (cur_line_h - ib->border_rect.height) / 2));
                offset_box_recursive(ib, ib_x - ib->border_rect.x, ib_y - ib->border_rect.y);
                append_child_box(box, ib);
                if (line_box_cnt >= line_box_cap) {
                    size_t ncap = line_box_cap * 2u;
                    ArcrLayoutBox **grown = (ArcrLayoutBox **)realloc(
                        line_boxes, ncap * sizeof(ArcrLayoutBox *));
                    if (grown) { line_boxes = grown; line_box_cap = ncap; }
                }
                if (line_box_cnt < line_box_cap) {
                    line_boxes[line_box_cnt++] = ib;
                }
                if (it->link_node && it->link_href) {
                    ArcrInlineLinkSpan *ls = (ArcrInlineLinkSpan *)arcr_arena_alloc(
                        &ctx->tree->arena, sizeof(ArcrInlineLinkSpan), sizeof(void *));
                    if (ls) {
                        ls->rect        = ib->border_rect;
                        ls->link_node   = it->link_node;
                        ls->href        = it->link_href;
                        ls->next        = box->links_head;
                        box->links_head = ls;
                    }
                }
                pen_x = clamp_coord((int64_t)pen_x + it->adv_px);
                continue;
            }

            if (((it->style->bg_argb >> 24) & 0xFFu) > 0u &&
                it->style != box->style && it->adv_px > 0) {
                int32_t pill_y = baseline_y - it->style->font_px;
                int32_t pill_h = it->style->font_px + 5;
                if (box->pills_head && box->pills_head != line_pill_mark &&
                    box->pills_head->bg_argb == it->style->bg_argb &&
                    box->pills_head->rect.y == pill_y &&
                    box->pills_head->rect.x + box->pills_head->rect.width == start_x + pen_x) {
                    box->pills_head->rect.width += it->adv_px;
                } else {
                    ArcrInlineBgPill *pill = (ArcrInlineBgPill *)arcr_arena_alloc(
                        &ctx->tree->arena, sizeof(ArcrInlineBgPill), sizeof(void *));
                    if (pill) {
                        pill->rect       = (ArcrRect){start_x + pen_x, pill_y, it->adv_px, pill_h};
                        pill->radius     = it->style->border_radius;
                        pill->bg_argb    = it->style->bg_argb;
                        pill->next       = box->pills_head;
                        box->pills_head  = pill;
                    }
                }
            }

            if (it->link_node && it->link_href && it->adv_px > 0) {
                int32_t ly = baseline_y - it->style->font_px;
                int32_t lh = it->style->font_px + 6;
                if (box->links_head && box->links_head != line_link_mark &&
                    box->links_head->link_node == it->link_node &&
                    box->links_head->rect.y == ly &&
                    box->links_head->rect.x + box->links_head->rect.width == start_x + pen_x) {
                    box->links_head->rect.width += it->adv_px;
                } else {
                    ArcrInlineLinkSpan *ls = (ArcrInlineLinkSpan *)arcr_arena_alloc(
                        &ctx->tree->arena, sizeof(ArcrInlineLinkSpan), sizeof(void *));
                    if (ls) {
                        ls->rect        = (ArcrRect){start_x + pen_x, ly, it->adv_px, lh};
                        ls->link_node   = it->link_node;
                        ls->href        = it->link_href;
                        ls->next        = box->links_head;
                        box->links_head = ls;
                    }
                }
            }

            if (it->cp != 0x200Bu) {
                out_g[gcount++] = (ArcrPositionedGlyph){
                    .codepoint    = (it->cp == 0x00A0u) ? (uint32_t)' ' : it->cp,
                    .x            = start_x + pen_x,
                    .baseline_y   = baseline_y,
                    .advance_px   = it->adv_px,
                    .font_px      = it->style->font_px,
                    .bold         = it->style->font_bold,
                    .monospace    = it->style->font_monospace,
                    .underline    = it->style->underline,
                    .line_through = it->style->line_through,
                    .color_argb   = it->style->color_argb
                };
            }
            pen_x = clamp_coord((int64_t)pen_x + it->adv_px);
        }
        i = cend;
    }

    if (pen_x > 0 || line_box_cnt > 0u || gcount > line_g_start) {
        finish_and_align_line(ctx, box, start_x, &pen_x, max_width, out_g,
                              line_g_start, &gcount, line_pill_mark, line_link_mark,
                              line_boxes, line_box_cnt);
        cur_y = clamp_coord((int64_t)cur_y + cur_line_h);
    }

    free(line_boxes);
    append_glyphs_to_box(ctx->tree, box, out_g, gcount);
    return cur_y - start_y;
}

static void emit_single_line_text(ArcrLayoutTree *tree, ArcrFontSystem *fonts,
                                  ArcrLayoutBox *box, const char *utf8, size_t len,
                                  int32_t x, int32_t baseline_y, int32_t max_w,
                                  uint32_t color_argb, bool mask_password,
                                  bool center_in_max_w, bool keep_tail_on_overflow) {
    if (!utf8 || len == 0u || max_w <= 0) return;

    size_t cap = 256u;
    ArcrPositionedGlyph *tmp = (ArcrPositionedGlyph *)malloc(cap * sizeof(ArcrPositionedGlyph));
    if (!tmp) return;

    uint32_t cnt   = 0u;
    size_t   pos   = 0u;
    int32_t  total = 0;

    while (pos < len) {
        uint32_t cp = 0u;
        size_t step = arcr_utf8_decode_one(utf8 + pos, len - pos, &cp);
        if (step == 0u) break;
        pos += step;
        if (cp == '\r' || cp == '\n') cp = ' ';
        if (mask_password) cp = 0x2022u;
        else cp = apply_text_transform_cp(cp, box->style->text_transform);

        if (cnt >= cap) {
            size_t ncap = cap * 2u;
            ArcrPositionedGlyph *grown = (ArcrPositionedGlyph *)realloc(
                tmp, ncap * sizeof(ArcrPositionedGlyph));
            if (!grown) break;
            tmp = grown;
            cap = ncap;
        }

        int32_t adv = measure_codepoint_adv(fonts, cp, box->style->font_px,
                                            box->style->font_bold, box->style->font_monospace);
        tmp[cnt++] = (ArcrPositionedGlyph){
            .codepoint    = cp,
            .x            = total,
            .baseline_y   = baseline_y,
            .advance_px   = adv,
            .font_px      = box->style->font_px,
            .bold         = box->style->font_bold,
            .monospace    = box->style->font_monospace,
            .underline    = box->style->underline,
            .line_through = box->style->line_through,
            .color_argb   = color_argb
        };
        total = clamp_coord((int64_t)total + adv);
    }
    if (cnt == 0u) {
        free(tmp);
        return;
    }

    uint32_t start_idx = 0u;
    int32_t  shift_x   = x;
    if (total > max_w) {
        if (keep_tail_on_overflow) {
            while (start_idx + 1u < cnt && (total - tmp[start_idx].x) > max_w) {
                ++start_idx;
            }
            shift_x = x - tmp[start_idx].x;
        } else {
            uint32_t fit = 0u;
            while (fit < cnt && tmp[fit].x + tmp[fit].advance_px <= max_w) ++fit;
            cnt = fit;
        }
    } else if (center_in_max_w) {
        shift_x = x + (max_w - total) / 2;
    }

    if (start_idx < cnt) {
        for (uint32_t k = start_idx; k < cnt; ++k) {
            tmp[k].x = clamp_coord((int64_t)tmp[k].x + shift_x);
        }
        append_glyphs_to_box(tree, box, tmp + start_idx, cnt - start_idx);
    }
    free(tmp);
}
/* -------------------------------------------------------------------------
 * Dynamic Element Child Collector (Flattens display:contents, Defers Out-of-Flow)
 * ------------------------------------------------------------------------- */
typedef struct DomNodeVec {
    const ArcrDomNode **items;
    size_t              count;
    size_t              cap;
} DomNodeVec;

static void dom_vec_push(DomNodeVec *v, const ArcrDomNode *n) {
    if (v->count >= v->cap) {
        size_t ncap = (v->cap == 0u) ? 32u : (v->cap * 2u);
        const ArcrDomNode **grown = (const ArcrDomNode **)realloc(
            v->items, ncap * sizeof(const ArcrDomNode *));
        if (!grown) return;
        v->items = grown;
        v->cap   = ncap;
    }
    v->items[v->count++] = n;
}

static void collect_flow_element_children(LayoutCtx *ctx, const ArcrDomNode *parent,
                                          ArcrLayoutBox *pos_owner,
                                          int32_t static_x, int32_t static_y, int32_t avail_w,
                                          DomNodeVec *out_vec) {
    const ArcrDomNode *src_parent =
        (parent && parent->kind == ARCR_NODE_ELEMENT && parent->shadow_root)
            ? parent->shadow_root
            : parent;
    for (const ArcrDomNode *ch = src_parent ? src_parent->first_child : NULL;
         ch != NULL; ch = ch->next_sibling) {
        if (ch->kind != ARCR_NODE_ELEMENT || !ch->computed_style) continue;
        if (ch->computed_style->display == ARCR_DISPLAY_NONE) continue;
        if (ch->computed_style->display == ARCR_DISPLAY_CONTENTS) {
            collect_flow_element_children(ctx, ch, pos_owner, static_x, static_y, avail_w, out_vec);
            continue;
        }
        if (is_out_of_flow_el(ch)) {
            ArcrLayoutBox *owner = (ch->computed_style->position == ARCR_POS_FIXED)
                                       ? ctx->tree->root_box
                                       : pos_owner;
            push_deferred_pos(ctx, ch, owner, static_x, static_y, avail_w,
                              ch->computed_style->position == ARCR_POS_FIXED);
            continue;
        }
        dom_vec_push(out_vec, ch);
    }
}

/* -------------------------------------------------------------------------
 * Flexbox Layout (Row/Row-Reverse/Column/Column-Reverse, Basis, Shrink, Space-Evenly)
 * ------------------------------------------------------------------------- */
static int32_t layout_flex_children(LayoutCtx *ctx, const ArcrDomNode *container,
                                    ArcrLayoutBox *box, ArcrLayoutBox *pos_owner,
                                    int32_t cx, int32_t cy, int32_t cw, int32_t container_def_h) {
    const ArcrComputedStyle *st = box->style;
    int32_t col_gap = ARCR_MAX(0, st->gap_px);
    int32_t row_gap = (st->row_gap_px > 0) ? st->row_gap_px : col_gap;

    DomNodeVec vec = {NULL, 0u, 0u};
    collect_flow_element_children(ctx, container, pos_owner, cx, cy, cw, &vec);
    if (vec.count == 0u) {
        free(vec.items);
        return 0;
    }

    /* Column / Column-Reverse Flexbox */
    if (st->flex_direction == ARCR_FLEX_COLUMN ||
        st->flex_direction == ARCR_FLEX_COLUMN_REVERSE) {
        bool rev = (st->flex_direction == ARCR_FLEX_COLUMN_REVERSE);
        int32_t cursor_y = cy;
        bool    first    = true;

        for (size_t idx = 0u; idx < vec.count; ++idx) {
            size_t k = rev ? (vec.count - 1u - idx) : idx;
            const ArcrDomNode *ch = vec.items[k];
            const ArcrComputedStyle *cst = ch->computed_style;

            if (!first) cursor_y = clamp_coord((int64_t)cursor_y + row_gap);
            ArcrAlignItems eff_align = cst->has_align_self ? cst->align_self : st->align_items;
            bool shrink = (eff_align != ARCR_ALIGN_STRETCH) && (cst->width_spec == 0);

            ArcrLayoutBox *ib = layout_element_node(ctx, ch, pos_owner, cx, cursor_y, cw,
                                                    container_def_h, shrink, 0, 0);
            if (!ib) continue;

            int32_t item_outer_w = clamp_coord((int64_t)ib->style->margin.left +
                                               ib->border_rect.width + ib->style->margin.right);
            if (item_outer_w < cw) {
                int32_t dx = 0;
                if (ib->style->margin_left_auto && ib->style->margin_right_auto) {
                    dx = (cw - ib->border_rect.width) / 2 - ib->style->margin.left;
                } else if (ib->style->margin_left_auto) {
                    dx = cw - item_outer_w;
                } else if (eff_align == ARCR_ALIGN_CENTER) {
                    dx = (cw - item_outer_w) / 2;
                } else if (eff_align == ARCR_ALIGN_END) {
                    dx = cw - item_outer_w;
                }
                if (dx != 0) offset_box_recursive(ib, dx, 0);
            }
            append_child_box(box, ib);
            cursor_y = clamp_coord((int64_t)ib->border_rect.y + ib->border_rect.height +
                                   ib->style->margin.bottom);
            first = false;
        }
        free(vec.items);
        return cursor_y - cy;
    }

    /* Row / Row-Reverse Flexbox */
    int32_t *base_w    = (int32_t *)malloc(vec.count * sizeof(int32_t));
    int32_t *margins_h = (int32_t *)malloc(vec.count * sizeof(int32_t));
    if (!base_w || !margins_h) {
        free(base_w);
        free(margins_h);
        free(vec.items);
        return 0;
    }

    int32_t zoom = ctx->tree->zoom_percent;
    for (size_t i = 0u; i < vec.count; ++i) {
        const ArcrDomNode *ch = vec.items[i];
        const ArcrComputedStyle *cst = ch->computed_style;
        margins_h[i] = zpx(cst->margin.left + cst->margin.right, zoom);
        int32_t h_chrome = zpx(cst->border.left + cst->border.right +
                               cst->padding.left + cst->padding.right, zoom);
        if (cst->flex_basis_spec != 0) {
            int32_t raw_b = (cst->flex_basis_spec > 0)
                                ? zpx(cst->flex_basis_spec, zoom)
                                : resolve_spec_px(cst->flex_basis_spec, cw);
            base_w[i] = css_w_to_border_w(raw_b, cst, h_chrome);
        } else {
            base_w[i] = measure_natural_border_w(ctx, ch, cw);
        }
    }

    int32_t cur_y      = cy;
    size_t  line_start = 0u;
    bool    row_rev    = (st->flex_direction == ARCR_FLEX_ROW_REVERSE);

    while (line_start < vec.count) {
        size_t  line_end = line_start + 1u;
        int32_t used_w   = clamp_coord((int64_t)base_w[line_start] + margins_h[line_start]);

        while (line_end < vec.count) {
            int32_t next_w = clamp_coord((int64_t)base_w[line_end] + margins_h[line_end]);
            if (st->flex_wrap && used_w + col_gap + next_w > cw) {
                break;
            }
            used_w = clamp_coord((int64_t)used_w + col_gap + next_w);
            line_end++;
        }

        size_t  lcnt       = line_end - line_start;
        int32_t total_gaps = (int32_t)(lcnt - 1u) * col_gap;
        int32_t total_grow = 0;
        int64_t weighted_shrink_sum = 0;
        int32_t sum_base   = 0;
        int32_t sum_marg   = 0;
        int32_t auto_margin_cnt = 0;

        for (size_t k = line_start; k < line_end; ++k) {
            const ArcrComputedStyle *cst = vec.items[k]->computed_style;
            total_grow += cst->flex_grow;
            weighted_shrink_sum += (int64_t)ARCR_MAX(0, cst->flex_shrink) * (int64_t)base_w[k];
            sum_base = clamp_coord((int64_t)sum_base + base_w[k]);
            sum_marg = clamp_coord((int64_t)sum_marg + margins_h[k]);
            if (cst->margin_left_auto)  ++auto_margin_cnt;
            if (cst->margin_right_auto) ++auto_margin_cnt;
        }

        int32_t avail_for_boxes = cw - total_gaps - sum_marg;
        int32_t free_space      = avail_for_boxes - sum_base;

        ArcrLayoutBox **lboxes = (ArcrLayoutBox **)calloc(lcnt, sizeof(ArcrLayoutBox *));
        if (!lboxes) break;

        int32_t line_h      = 0;
        int32_t actual_used = total_gaps + sum_marg;

        for (size_t k = line_start; k < line_end; ++k) {
            const ArcrDomNode *item_node = vec.items[k];
            const ArcrComputedStyle *zst = zoom_style_if_needed(
                &ctx->tree->arena, item_node->computed_style, zoom);
            int32_t h_chrome = zst->border.left + zst->border.right +
                               zst->padding.left + zst->padding.right;

            int32_t target_w = base_w[k];
            if (free_space > 0 && total_grow > 0 && zst->flex_grow > 0) {
                target_w = clamp_coord((int64_t)target_w +
                                       ((int64_t)free_space * zst->flex_grow) / total_grow);
            } else if (free_space < 0 && weighted_shrink_sum > 0 && zst->flex_shrink > 0) {
                int64_t w_sh = (int64_t)zst->flex_shrink * (int64_t)base_w[k];
                int32_t delta = (int32_t)(((int64_t)(-free_space) * w_sh) / weighted_shrink_sum);
                target_w = ARCR_MAX(ARCR_MAX(8, h_chrome), base_w[k] - delta);
            }
            target_w = apply_min_max_w(target_w, zst, cw, h_chrome);

            ArcrLayoutBox *ib = layout_element_node(
                ctx, item_node, pos_owner, 0, 0, target_w, container_def_h, false, target_w, 0);
            lboxes[k - line_start] = ib;
            if (ib) {
                actual_used = clamp_coord((int64_t)actual_used + ib->border_rect.width);
                int32_t oh  = clamp_coord((int64_t)ib->style->margin.top +
                                          ib->border_rect.height + ib->style->margin.bottom);
                if (oh > line_h) line_h = oh;
            }
        }
        if (lcnt == vec.count && container_def_h > line_h) {
            line_h = container_def_h;
        }

        int32_t rem_w   = ARCR_MAX(0, cw - actual_used);
        int32_t pen_x   = cx;
        int32_t eff_gap = col_gap;

        if (auto_margin_cnt == 0) {
            if (st->justify_content == ARCR_JUSTIFY_CENTER) {
                pen_x += rem_w / 2;
            } else if (st->justify_content == ARCR_JUSTIFY_END) {
                pen_x += rem_w;
            } else if (st->justify_content == ARCR_JUSTIFY_SPACE_BETWEEN && lcnt > 1u) {
                eff_gap = col_gap + rem_w / (int32_t)(lcnt - 1u);
            } else if (st->justify_content == ARCR_JUSTIFY_SPACE_AROUND && lcnt > 0u) {
                int32_t pad = rem_w / (int32_t)(lcnt * 2u);
                pen_x   += pad;
                eff_gap  = col_gap + pad * 2;
            } else if (st->justify_content == ARCR_JUSTIFY_SPACE_EVENLY && lcnt > 0u) {
                int32_t pad = rem_w / (int32_t)(lcnt + 1u);
                pen_x   += pad;
                eff_gap  = col_gap + pad;
            } else if (row_rev) {
                pen_x += rem_w;
            }
        }

        int32_t each_auto = (auto_margin_cnt > 0) ? (rem_w / auto_margin_cnt) : 0;

        for (size_t step = 0u; step < lcnt; ++step) {
            size_t idx = row_rev ? (lcnt - 1u - step) : step;
            ArcrLayoutBox *ib = lboxes[idx];
            if (!ib) continue;

            int32_t ml = ib->style->margin.left + (ib->style->margin_left_auto ? each_auto : 0);
            int32_t mr = ib->style->margin.right + (ib->style->margin_right_auto ? each_auto : 0);

            int32_t item_x = clamp_coord((int64_t)pen_x + ml);
            int32_t item_y = clamp_coord((int64_t)cur_y + ib->style->margin.top);
            int32_t oh     = clamp_coord((int64_t)ib->style->margin.top +
                                         ib->border_rect.height + ib->style->margin.bottom);

            ArcrAlignItems eff_align = ib->style->has_align_self
                                           ? ib->style->align_self
                                           : st->align_items;
            if (eff_align == ARCR_ALIGN_STRETCH &&
                ib->style->height_px == 0 && ib->style->height_pct == 0) {
                int32_t stretch_bh = line_h - ib->style->margin.top - ib->style->margin.bottom;
                if (stretch_bh > ib->border_rect.height) {
                    int32_t dh = stretch_bh - ib->border_rect.height;
                    ib->border_rect.height  += dh;
                    ib->content_rect.height += dh;
                }
            } else if (eff_align == ARCR_ALIGN_CENTER && oh < line_h) {
                item_y += (line_h - oh) / 2;
            } else if (eff_align == ARCR_ALIGN_END && oh < line_h) {
                item_y += (line_h - oh);
            }

            offset_box_recursive(ib, item_x - ib->border_rect.x, item_y - ib->border_rect.y);
            append_child_box(box, ib);
            pen_x = clamp_coord((int64_t)pen_x + ml + ib->border_rect.width + mr + eff_gap);
        }

        free(lboxes);
        cur_y = clamp_coord((int64_t)cur_y + line_h);
        if (line_end < vec.count) cur_y = clamp_coord((int64_t)cur_y + row_gap);
        line_start = line_end;
    }

    free(base_w);
    free(margins_h);
    free(vec.items);
    return cur_y - cy;
}

/* -------------------------------------------------------------------------
 * CSS Grid Layout (Explicit Tracks, auto-fit/auto-fill minmax, grid-column: span N)
 * ------------------------------------------------------------------------- */
static int32_t layout_grid_children(LayoutCtx *ctx, const ArcrDomNode *container,
                                    ArcrLayoutBox *box, ArcrLayoutBox *pos_owner,
                                    int32_t cx, int32_t cy, int32_t cw, int32_t container_def_h) {
    const ArcrComputedStyle *st = box->style;
    int32_t col_gap = ARCR_MAX(0, st->gap_px);
    int32_t row_gap = (st->row_gap_px > 0) ? st->row_gap_px : col_gap;

    int32_t cols = 1;
    if (st->grid_min_col_px > 0) {
        cols = ARCR_CLAMP((cw + col_gap) / ARCR_MAX(1, st->grid_min_col_px + col_gap), 1, 16);
    } else if (st->grid_track_count > 0u) {
        cols = ARCR_CLAMP((int32_t)st->grid_track_count, 1, (int32_t)ARCR_MAX_GRID_TRACKS);
    } else if (st->grid_columns > 0) {
        cols = ARCR_CLAMP(st->grid_columns, 1, 16);
    }

    int32_t track_widths[16];
    int32_t track_xs[16];
    int32_t total_gaps   = (cols - 1) * col_gap;
    int32_t avail_tracks = ARCR_MAX(0, cw - total_gaps);

    if (st->grid_min_col_px == 0 && st->grid_track_count > 0u &&
        cols <= (int32_t)ARCR_MAX_GRID_TRACKS &&
        (int32_t)st->grid_track_count == cols) {
        int32_t fixed_sum = 0;
        int64_t fr_sum    = 0;
        int32_t auto_cnt  = 0;
        int32_t last_fr   = -1;
        for (int32_t c = 0; c < cols; ++c) {
            int32_t sp = st->grid_track_specs[c];
            if (sp > 0) {
                fixed_sum += sp;
            } else if (sp < 0) {
                fr_sum += (int64_t)(-sp);
                last_fr = c;
            } else {
                ++auto_cnt;
            }
        }
        int32_t rem = ARCR_MAX(0, avail_tracks - fixed_sum);
        if (fr_sum == 0 && auto_cnt > 0) {
            int32_t used_auto = 0;
            int32_t seen_auto = 0;
            for (int32_t c = 0; c < cols; ++c) {
                int32_t sp = st->grid_track_specs[c];
                if (sp > 0) {
                    track_widths[c] = sp;
                } else {
                    ++seen_auto;
                    int32_t aw = (seen_auto == auto_cnt) ? (rem - used_auto) : (rem / auto_cnt);
                    track_widths[c] = ARCR_MAX(1, aw);
                    used_auto += track_widths[c];
                }
            }
        } else {
            int32_t auto_share = (auto_cnt > 0 && rem > 0)
                                     ? ARCR_MIN(zpx(120, ctx->tree->zoom_percent),
                                                rem / (auto_cnt + 2))
                                     : 0;
            int32_t fr_pool = ARCR_MAX(0, rem - auto_cnt * auto_share);
            int32_t fr_used = 0;
            for (int32_t c = 0; c < cols; ++c) {
                int32_t sp = st->grid_track_specs[c];
                if (sp > 0) {
                    track_widths[c] = sp;
                } else if (sp < 0 && fr_sum > 0) {
                    int32_t fw = (c == last_fr)
                                     ? (fr_pool - fr_used)
                                     : (int32_t)(((int64_t)fr_pool * (int64_t)(-sp)) / fr_sum);
                    track_widths[c] = ARCR_MAX(1, fw);
                    fr_used += track_widths[c];
                } else {
                    track_widths[c] = ARCR_MAX(1, auto_share);
                }
            }
        }
    } else {
        int32_t uniform_w = ARCR_MAX(16, avail_tracks / cols);
        for (int32_t c = 0; c < cols; ++c) {
            track_widths[c] = uniform_w;
        }
    }

    int32_t run_x = cx;
    for (int32_t c = 0; c < cols; ++c) {
        track_xs[c] = run_x;
        run_x = clamp_coord((int64_t)run_x + track_widths[c] + col_gap);
    }

    DomNodeVec vec = {NULL, 0u, 0u};
    collect_flow_element_children(ctx, container, pos_owner, cx, cy, cw, &vec);
    if (vec.count == 0u) {
        free(vec.items);
        return 0;
    }

    int32_t cur_y = cy;
    size_t  idx   = 0u;
    bool    first_row = true;

    ArcrLayoutBox **row_boxes = (ArcrLayoutBox **)malloc((size_t)cols * sizeof(ArcrLayoutBox *));
    if (!row_boxes) {
        free(vec.items);
        return 0;
    }

    while (idx < vec.count) {
        if (!first_row) cur_y = clamp_coord((int64_t)cur_y + row_gap);
        first_row = false;

        int32_t cur_col = 0;
        size_t  rcnt    = 0u;
        int32_t row_h   = 0;

        while (idx < vec.count && cur_col < cols) {
            const ArcrDomNode *ch = vec.items[idx];
            int32_t span = ch->computed_style
                               ? ARCR_CLAMP(ch->computed_style->grid_col_span, 1, cols)
                               : 1;
            if (cur_col > 0 && cur_col + span > cols) {
                break;
            }
            if (cur_col + span > cols) span = cols - cur_col;

            int32_t cell_x  = track_xs[cur_col];
            int32_t track_w = (span - 1) * col_gap;
            for (int32_t s = 0; s < span; ++s) {
                track_w += track_widths[cur_col + s];
            }
            int32_t mh      = zpx(ch->computed_style->margin.left +
                                  ch->computed_style->margin.right, ctx->tree->zoom_percent);
            int32_t item_bw = (ch->computed_style && ch->computed_style->width_spec != 0)
                                  ? 0
                                  : ARCR_MAX(1, track_w - mh);

            ArcrLayoutBox *ib = layout_element_node(
                ctx, ch, pos_owner, cell_x, cur_y, track_w, container_def_h, false, item_bw, 0);
            row_boxes[rcnt++] = ib;
            if (ib) {
                int32_t oh = clamp_coord((int64_t)ib->style->margin.top +
                                         ib->border_rect.height + ib->style->margin.bottom);
                if (oh > row_h) row_h = oh;
            }
            cur_col += span;
            ++idx;
        }

        for (size_t c = 0u; c < rcnt; ++c) {
            ArcrLayoutBox *ib = row_boxes[c];
            if (!ib) continue;
            ArcrAlignItems eff_align = ib->style->has_align_self
                                           ? ib->style->align_self
                                           : st->align_items;
            int32_t oh = clamp_coord((int64_t)ib->style->margin.top +
                                     ib->border_rect.height + ib->style->margin.bottom);
            if (eff_align == ARCR_ALIGN_STRETCH &&
                ib->style->height_px == 0 && ib->style->height_pct == 0) {
                int32_t stretch_bh = row_h - ib->style->margin.top - ib->style->margin.bottom;
                if (stretch_bh > ib->border_rect.height) {
                    int32_t dh = stretch_bh - ib->border_rect.height;
                    ib->border_rect.height  += dh;
                    ib->content_rect.height += dh;
                }
            } else if (eff_align == ARCR_ALIGN_CENTER && oh < row_h) {
                offset_box_recursive(ib, 0, (row_h - oh) / 2);
            } else if (eff_align == ARCR_ALIGN_END && oh < row_h) {
                offset_box_recursive(ib, 0, row_h - oh);
            }
            append_child_box(box, ib);
        }
        cur_y = clamp_coord((int64_t)cur_y + row_h);
    }

    free(row_boxes);
    free(vec.items);
    return cur_y - cy;
}

/* -------------------------------------------------------------------------
 * HTML Table Layout (Dynamic Rows/Columns, thead/tbody/tfoot & display:contents)
 * ------------------------------------------------------------------------- */
static void collect_table_rows(const ArcrAtomTable *atoms, const ArcrDomNode *parent,
                               DomNodeVec *out_rows) {
    for (const ArcrDomNode *ch = parent->first_child; ch != NULL; ch = ch->next_sibling) {
        if (ch->kind != ARCR_NODE_ELEMENT) continue;
        if (ch->computed_style && ch->computed_style->display == ARCR_DISPLAY_NONE) continue;
        const char *tn = arcr_atom_cstr(atoms, ch->tag);
        if (arcr_str_eq_nocase(tn, "tr") ||
            (ch->computed_style && ch->computed_style->display == ARCR_DISPLAY_TABLE_ROW)) {
            dom_vec_push(out_rows, ch);
        } else if (arcr_str_eq_nocase(tn, "thead") || arcr_str_eq_nocase(tn, "tbody") ||
                   arcr_str_eq_nocase(tn, "tfoot") ||
                   (ch->computed_style && ch->computed_style->display == ARCR_DISPLAY_CONTENTS)) {
            collect_table_rows(atoms, ch, out_rows);
        }
    }
}

static void collect_row_cells(const ArcrAtomTable *atoms, const ArcrDomNode *row_parent,
                              DomNodeVec *out_cells) {
    for (const ArcrDomNode *ch = row_parent->first_child; ch != NULL; ch = ch->next_sibling) {
        if (ch->kind != ARCR_NODE_ELEMENT) continue;
        if (ch->computed_style && ch->computed_style->display == ARCR_DISPLAY_NONE) continue;
        if (ch->computed_style && ch->computed_style->display == ARCR_DISPLAY_CONTENTS) {
            collect_row_cells(atoms, ch, out_cells);
            continue;
        }
        const char *tn = arcr_atom_cstr(atoms, ch->tag);
        if (arcr_str_eq_nocase(tn, "td") || arcr_str_eq_nocase(tn, "th") ||
            (ch->computed_style && ch->computed_style->display == ARCR_DISPLAY_TABLE_CELL)) {
            dom_vec_push(out_cells, ch);
        }
    }
}

static int32_t layout_table_children(LayoutCtx *ctx, const ArcrDomNode *table_node,
                                     ArcrLayoutBox *table_box, ArcrLayoutBox *pos_owner,
                                     int32_t cx, int32_t cy, int32_t cw,
                                     int32_t *out_used_table_w) {
    DomNodeVec rows = {NULL, 0u, 0u};
    collect_table_rows(ctx->doc->atoms, table_node, &rows);
    if (rows.count == 0u) {
        free(rows.items);
        if (out_used_table_w) *out_used_table_w = cw;
        return 0;
    }

    size_t col_cap  = 16u;
    size_t max_cols = 0u;
    int32_t *col_pref = (int32_t *)calloc(col_cap, sizeof(int32_t));
    if (!col_pref) {
        free(rows.items);
        if (out_used_table_w) *out_used_table_w = cw;
        return 0;
    }

    int32_t zoom = ctx->tree->zoom_percent;

    for (size_t r = 0u; r < rows.count; ++r) {
        DomNodeVec cells = {NULL, 0u, 0u};
        collect_row_cells(ctx->doc->atoms, rows.items[r], &cells);
        size_t c = 0u;
        for (size_t ci = 0u; ci < cells.count; ++ci) {
            const ArcrDomNode *cell = cells.items[ci];
            int32_t span = 1;
            const char *cs_s = arcr_dom_get_attr_cstr(ctx->doc, cell, "colspan");
            if (cs_s && arcr_parse_i32(cs_s, strlen(cs_s), &span)) {
                span = ARCR_CLAMP(span, 1, 64);
            }
            while (c + (size_t)span > col_cap) {
                size_t ncap = col_cap * 2u;
                int32_t *grown = (int32_t *)realloc(col_pref, ncap * sizeof(int32_t));
                if (!grown) break;
                memset(grown + col_cap, 0, (ncap - col_cap) * sizeof(int32_t));
                col_pref = grown;
                col_cap  = ncap;
            }
            if (c + (size_t)span > col_cap) break;

            int32_t nw   = measure_natural_border_w(ctx, cell, cw);
            int32_t each = ARCR_MAX(zpx(36, zoom), nw / span);
            for (int32_t s = 0; s < span; ++s) {
                if (each > col_pref[c + (size_t)s]) col_pref[c + (size_t)s] = each;
            }
            c += (size_t)span;
        }
        if (c > max_cols) max_cols = c;
        free(cells.items);
    }

    if (max_cols == 0u) {
        free(col_pref);
        free(rows.items);
        if (out_used_table_w) *out_used_table_w = cw;
        return 0;
    }

    int32_t sum_pref = 0;
    for (size_t c = 0u; c < max_cols; ++c) {
        if (col_pref[c] < zpx(32, zoom)) col_pref[c] = zpx(32, zoom);
        sum_pref = clamp_coord((int64_t)sum_pref + col_pref[c]);
    }

    bool fill_full = (table_box->style->width_spec != 0) || (sum_pref > cw);
    int32_t target_table_w = fill_full ? cw : ARCR_MIN(cw, sum_pref);
    int32_t *col_w = (int32_t *)malloc(max_cols * sizeof(int32_t));
    if (!col_w) {
        free(col_pref);
        free(rows.items);
        return 0;
    }

    int32_t assigned = 0;
    for (size_t c = 0u; c < max_cols; ++c) {
        if (c + 1u == max_cols) {
            col_w[c] = ARCR_MAX(zpx(20, zoom), target_table_w - assigned);
        } else {
            col_w[c] = ARCR_MAX(zpx(20, zoom),
                                (int32_t)(((int64_t)target_table_w * col_pref[c]) /
                                          ARCR_MAX(1, sum_pref)));
            assigned += col_w[c];
        }
    }
    int32_t final_tw = 0;
    for (size_t c = 0u; c < max_cols; ++c) final_tw += col_w[c];
    if (out_used_table_w) *out_used_table_w = final_tw;

    int32_t cur_y = cy;
    ArcrLayoutBox **cell_boxes = (ArcrLayoutBox **)malloc(max_cols * sizeof(ArcrLayoutBox *));

    for (size_t r = 0u; r < rows.count && cell_boxes != NULL; ++r) {
        const ArcrDomNode *tr_node = rows.items[r];
        const ArcrComputedStyle *tr_st = zoom_style_if_needed(
            &ctx->tree->arena, tr_node->computed_style, zoom);

        ArcrLayoutBox *tr_box = (ArcrLayoutBox *)arcr_arena_calloc(
            &ctx->tree->arena, 1u, sizeof(ArcrLayoutBox), sizeof(void *));
        if (!tr_box) continue;
        ctx->tree->box_count++;
        tr_box->dom_node = tr_node;
        tr_box->style    = tr_st;

        DomNodeVec cells = {NULL, 0u, 0u};
        collect_row_cells(ctx->doc->atoms, tr_node, &cells);

        size_t  cell_cnt = 0u;
        size_t  col_idx  = 0u;
        int32_t pen_x    = cx;
        int32_t row_h    = zpx(24, zoom);

        for (size_t ci = 0u; ci < cells.count && col_idx < max_cols; ++ci) {
            const ArcrDomNode *cell = cells.items[ci];
            int32_t span = 1;
            const char *cs_s = arcr_dom_get_attr_cstr(ctx->doc, cell, "colspan");
            if (cs_s && arcr_parse_i32(cs_s, strlen(cs_s), &span)) {
                span = ARCR_CLAMP(span, 1, (int32_t)(max_cols - col_idx));
            }
            int32_t cw_span = 0;
            for (int32_t s = 0; s < span && col_idx + (size_t)s < max_cols; ++s) {
                cw_span += col_w[col_idx + (size_t)s];
            }

            ArcrLayoutBox *cb = layout_element_node(
                ctx, cell, pos_owner, pen_x, cur_y, cw_span, 0, false, cw_span, 0);
            if (cb && cell_cnt < max_cols) {
                offset_box_recursive(cb, pen_x - cb->border_rect.x, cur_y - cb->border_rect.y);
                if (cb->border_rect.height > row_h) row_h = cb->border_rect.height;
                cell_boxes[cell_cnt++] = cb;
            }
            pen_x   += cw_span;
            col_idx += (size_t)span;
        }
        free(cells.items);

        for (size_t k = 0u; k < cell_cnt; ++k) {
            ArcrLayoutBox *cb = cell_boxes[k];
            if (cb->border_rect.height < row_h) {
                int32_t dh = row_h - cb->border_rect.height;
                cb->border_rect.height  += dh;
                cb->content_rect.height += dh;
            }
            append_child_box(tr_box, cb);
        }

        tr_box->border_rect  = (ArcrRect){cx, cur_y, final_tw, row_h};
        tr_box->content_rect = tr_box->border_rect;
        append_child_box(table_box, tr_box);
        cur_y = clamp_coord((int64_t)cur_y + row_h);
    }

    free(cell_boxes);
    free(col_w);
    free(col_pref);
    free(rows.items);
    return cur_y - cy;
}

/* -------------------------------------------------------------------------
 * Out-of-Flow (`position: absolute` & `position: fixed`) Resolution
 * ------------------------------------------------------------------------- */
static void resolve_deferred_for_owner(LayoutCtx *ctx, ArcrLayoutBox *owner_box,
                                       bool resolve_fixed_only) {
    /* Snapshot count before starting; nested positioned children resolve themselves */
    for (size_t i = 0u; i < ctx->deferred_cnt; ++i) {
        DeferredPosItem item = ctx->deferred[i];
        if (!item.node || item.attach_parent != owner_box) continue;
        if (resolve_fixed_only && !item.is_fixed) continue;
        if (!resolve_fixed_only && item.is_fixed) continue;

        /* Mark consumed so re-entrant scans never double-place */
        ctx->deferred[i].node = NULL;

        ArcrRect cb;
        if (item.is_fixed) {
            cb = (ArcrRect){0, 0, ctx->tree->viewport_width, ctx->tree->viewport_height};
        } else {
            const ArcrComputedStyle *ost = owner_box->style;
            int32_t bl = ost ? ost->border.left : 0;
            int32_t bt = ost ? ost->border.top : 0;
            int32_t br = ost ? ost->border.right : 0;
            int32_t bb = ost ? ost->border.bottom : 0;
            cb = (ArcrRect){
                owner_box->border_rect.x + bl,
                owner_box->border_rect.y + bt,
                ARCR_MAX(1, owner_box->border_rect.width - bl - br),
                ARCR_MAX(1, owner_box->border_rect.height - bt - bb)
            };
        }

        const ArcrComputedStyle *zst = zoom_style_if_needed(
            &ctx->tree->arena, item.node->computed_style, ctx->tree->zoom_percent);
        if (!zst) continue;

        int32_t left_off   = zst->has_left   ? resolve_pos_offset(zst->left_px, cb.width)   : 0;
        int32_t right_off  = zst->has_right  ? resolve_pos_offset(zst->right_px, cb.width)  : 0;
        int32_t top_off    = zst->has_top    ? resolve_pos_offset(zst->top_px, cb.height)   : 0;
        int32_t bottom_off = zst->has_bottom ? resolve_pos_offset(zst->bottom_px, cb.height): 0;

        int32_t forced_bw = 0;
        bool    shrink    = (zst->width_spec == 0);
        if (zst->width_spec == 0 && zst->has_left && zst->has_right) {
            forced_bw = ARCR_MAX(0, cb.width - left_off - right_off -
                                    zst->margin.left - zst->margin.right);
            shrink    = false;
        }

        ArcrLayoutBox *ab = layout_element_node(
            ctx, item.node, owner_box, 0, 0, cb.width, cb.height, shrink, forced_bw, 0);
        if (!ab) continue;

        /* Stretch height when top & bottom are both specified and height is auto */
        if (zst->height_px == 0 && zst->height_pct == 0 && zst->has_top && zst->has_bottom) {
            int32_t stretch_bh = cb.height - top_off - bottom_off -
                                 zst->margin.top - zst->margin.bottom;
            if (zst->max_height_px > 0) {
                int32_t v_chr = zst->border.top + zst->border.bottom +
                                zst->padding.top + zst->padding.bottom;
                int32_t mxh = css_h_to_border_h(zst->max_height_px, zst, v_chr);
                if (stretch_bh > mxh) stretch_bh = mxh;
            }
            if (stretch_bh > ab->border_rect.height) {
                int32_t dh = stretch_bh - ab->border_rect.height;
                ab->border_rect.height  += dh;
                ab->content_rect.height += dh;
            }
        }

        /* Note: layout_element_node already applied 2D transform around (0, 0);
         * compute target anchor delta from un-shifted origin (zst->margin.left, zst->margin.top). */
        int32_t target_bx = item.static_x + zst->margin.left;
        if (zst->has_left) {
            target_bx = cb.x + left_off + zst->margin.left;
            if (zst->has_right && zst->margin_left_auto && zst->margin_right_auto) {
                int32_t rem = cb.width - left_off - right_off - ab->border_rect.width;
                if (rem > 0) target_bx = cb.x + left_off + rem / 2;
            }
        } else if (zst->has_right) {
            target_bx = cb.x + cb.width - right_off - zst->margin.right - ab->border_rect.width;
        }

        int32_t target_by = item.static_y + zst->margin.top;
        if (zst->has_top) {
            target_by = cb.y + top_off + zst->margin.top;
            if (zst->has_bottom && ab->border_rect.height < (cb.height - top_off - bottom_off)) {
                /* Top wins unless vertical margin auto centering is used */
            }
        } else if (zst->has_bottom) {
            target_by = cb.y + cb.height - bottom_off - zst->margin.bottom -
                        ab->border_rect.height;
        }

        int32_t base_bx = zst->margin.left;
        int32_t base_by = zst->margin.top;
        offset_box_recursive(ab, target_bx - base_bx, target_by - base_by);
        append_child_box(owner_box, ab);
    }
}

/* -------------------------------------------------------------------------
 * Master Element Box Layout (Block, Replaced SVG/Video/Img, Flex, Grid, Table)
 * ------------------------------------------------------------------------- */
static ArcrLayoutBox *layout_element_node(LayoutCtx *ctx, const ArcrDomNode *node,
                                          ArcrLayoutBox *pos_owner,
                                          int32_t avail_x, int32_t start_y, int32_t avail_w,
                                          int32_t parent_def_h,
                                          bool shrink_to_fit, int32_t forced_border_w,
                                          int32_t list_ordinal) {
    if (!node || node->kind != ARCR_NODE_ELEMENT || !node->computed_style) return NULL;
    if (node->computed_style->display == ARCR_DISPLAY_NONE) return NULL;

    int32_t zoom = ctx->tree->zoom_percent;
    const ArcrComputedStyle *st = zoom_style_if_needed(
        &ctx->tree->arena, node->computed_style, zoom);

    ArcrLayoutBox *box = (ArcrLayoutBox *)arcr_arena_calloc(
        &ctx->tree->arena, 1u, sizeof(ArcrLayoutBox), sizeof(void *));
    if (!box) return NULL;
    ctx->tree->box_count++;
    if (!ctx->tree->root_box) ctx->tree->root_box = box;

    box->dom_node     = node;
    box->style        = st;
    const char *tname = arcr_atom_cstr(ctx->doc->atoms, node->tag);
    box->is_list_item = arcr_str_eq_nocase(tname, "li") && !st->list_style_none;
    box->list_ordinal = list_ordinal;
    box->is_hr        = arcr_str_eq_nocase(tname, "hr");

    ArcrLayoutBox *child_pos_owner = (st->position != ARCR_POS_STATIC || !pos_owner)
                                         ? box
                                         : pos_owner;

    bool is_img      = arcr_str_eq_nocase(tname, "img");
    bool is_svg      = arcr_str_eq_nocase(tname, "svg");
    bool is_canvas   = arcr_str_eq_nocase(tname, "canvas");
    bool is_video    = arcr_str_eq_nocase(tname, "video");
    bool is_replaced = is_img || is_svg || is_canvas || is_video;
    bool is_input    = arcr_str_eq_nocase(tname, "input");
    bool is_textarea = arcr_str_eq_nocase(tname, "textarea");
    bool is_button   = arcr_str_eq_nocase(tname, "button");
    bool is_select   = arcr_str_eq_nocase(tname, "select");

    if (is_video) box->control_kind = ARCR_CTRL_VIDEO_MEDIA;

    int32_t h_chrome = st->border.left + st->border.right + st->padding.left + st->padding.right;
    int32_t v_chrome = st->border.top + st->border.bottom + st->padding.top + st->padding.bottom;

    /* Resolve explicit CSS height if definite (px/vh or % of definite-height parent) */
    int32_t definite_bh = 0;
    if (st->height_px > 0) {
        definite_bh = css_h_to_border_h(st->height_px, st, v_chrome);
    } else if (st->height_pct > 0 && parent_def_h > 0) {
        int32_t raw_hp = clamp_coord(((int64_t)parent_def_h * st->height_pct) / 100LL);
        definite_bh    = css_h_to_border_h(raw_hp, st, v_chrome);
    }

    /* 1. Determine Border-Box Width (bw) & Horizontal Position (bx) */
    int32_t bw = 0;
    int32_t intrinsic_rep_h = 0;

    if (is_replaced) {
        int32_t nw = 0, nh = 0;
        query_svg_or_media_intrinsic(ctx->doc, node, is_svg, is_canvas, is_video, zoom, &nw, &nh);
        if (st->aspect_ratio_w > 0 && st->aspect_ratio_h > 0 && nw > 0) {
            nh = ARCR_MAX(1, (int32_t)(((int64_t)nw * st->aspect_ratio_h) / st->aspect_ratio_w));
        }

        int32_t css_bw = (forced_border_w > 0)
                             ? forced_border_w
                             : ((st->width_spec != 0)
                                    ? css_w_to_border_w(resolve_spec_px(st->width_spec, avail_w),
                                                        st, h_chrome)
                                    : 0);
        int32_t css_bh = definite_bh;

        int32_t rep_cw = nw;
        int32_t rep_ch = nh;
        if (css_bw > 0 && css_bh > 0) {
            rep_cw = ARCR_MAX(1, css_bw - h_chrome);
            rep_ch = ARCR_MAX(1, css_bh - v_chrome);
        } else if (css_bw > 0) {
            rep_cw = ARCR_MAX(1, css_bw - h_chrome);
            if (nw > 0) rep_ch = ARCR_MAX(1, (int32_t)(((int64_t)rep_cw * nh) / nw));
        } else if (css_bh > 0) {
            rep_ch = ARCR_MAX(1, css_bh - v_chrome);
            if (nh > 0) rep_cw = ARCR_MAX(1, (int32_t)(((int64_t)rep_ch * nw) / nh));
        }
        int32_t max_cw_allowed = avail_w - h_chrome;
        if (st->max_width_px > 0) {
            int32_t mxb = css_w_to_border_w(st->max_width_px, st, h_chrome) - h_chrome;
            if (mxb < max_cw_allowed) max_cw_allowed = mxb;
        }
        if (rep_cw > max_cw_allowed && max_cw_allowed > 0) {
            if (css_bh == 0 && rep_cw > 0) {
                rep_ch = ARCR_MAX(1, (int32_t)(((int64_t)max_cw_allowed * rep_ch) / rep_cw));
            }
            rep_cw = max_cw_allowed;
        }
        bw              = rep_cw + h_chrome;
        intrinsic_rep_h = rep_ch;
    } else if (forced_border_w > 0) {
        bw = forced_border_w;
    } else if (st->width_spec != 0) {
        int32_t raw_w = resolve_spec_px(st->width_spec, avail_w);
        bw            = css_w_to_border_w(raw_w, st, h_chrome);
    } else if (shrink_to_fit || is_input || is_textarea || is_button || is_select ||
               st->display == ARCR_DISPLAY_INLINE_BLOCK ||
               st->display == ARCR_DISPLAY_INLINE_FLEX) {
        bw = measure_natural_border_w(
            ctx, node, ARCR_MAX(16, avail_w - st->margin.left - st->margin.right));
    } else {
        bw = ARCR_MAX(16, avail_w - st->margin.left - st->margin.right);
    }

    bw = apply_min_max_w(bw, st, avail_w, h_chrome);
    bw = ARCR_MAX(ARCR_MAX(0, h_chrome), bw);

    int32_t bx = clamp_coord((int64_t)avail_x + st->margin.left);
    if (forced_border_w == 0 && !shrink_to_fit) {
        if (st->margin_left_auto && st->margin_right_auto && bw < avail_w) {
            bx = avail_x + (avail_w - bw) / 2;
        } else if (st->margin_left_auto && !st->margin_right_auto &&
                   bw + st->margin.right < avail_w) {
            bx = avail_x + avail_w - st->margin.right - bw;
        }
    }
    int32_t by = clamp_coord((int64_t)start_y + st->margin.top);
    int32_t cx = bx + st->border.left + st->padding.left;
    int32_t cy = by + st->border.top + st->padding.top;
    int32_t cw = ARCR_MAX(1, bw - h_chrome);

    if (definite_bh == 0 && st->aspect_ratio_w > 0 && st->aspect_ratio_h > 0 && !is_replaced) {
        int32_t ar_ch = (int32_t)(((int64_t)cw * st->aspect_ratio_h) / st->aspect_ratio_w);
        definite_bh   = v_chrome + ARCR_MAX(0, ar_ch);
    }
    int32_t child_def_h = (definite_bh > v_chrome) ? (definite_bh - v_chrome) : 0;

    /* 2. Lay out interior content */
    int32_t content_h = 0;

    if (box->is_hr) {
        content_h = zpx(2, zoom);
    } else if (is_replaced) {
        content_h = intrinsic_rep_h;
        if (is_svg && cw > 0 && content_h > 0) {
            (void)arcr_dom_rerasterize_svg((ArcrDocument *)ctx->doc, (ArcrDomNode *)node,
                                           st->color_argb, cw, content_h);
        } else if (is_img && (!node->surface || !node->surface->argb_pixels)) {
            const char *alt = arcr_dom_get_attr_cstr(ctx->doc, node, "alt");
            if (alt && alt[0] != '\0' && cw > 16 && content_h >= st->font_px) {
                int32_t base_y = cy + content_h / 2 + st->font_px / 3;
                emit_single_line_text(ctx->tree, ctx->fonts, box, alt, strlen(alt),
                                      cx + 6, base_y, cw - 12,
                                      0xFF64748Bu, false, true, false);
            }
        }
    } else if (is_input) {
        const char *tp = arcr_dom_get_attr_cstr(ctx->doc, node, "type");
        if (tp && (arcr_str_eq_nocase(tp, "checkbox") || arcr_str_eq_nocase(tp, "radio"))) {
            box->control_kind = ARCR_CTRL_CHECKBOX;
            if (st->width_spec == 0 && forced_border_w == 0) bw = zpx(18, zoom);
            cw        = ARCR_MAX(4, bw - h_chrome);
            content_h = (definite_bh > 0)
                            ? ARCR_MAX(4, definite_bh - v_chrome)
                            : ARCR_MAX(4, zpx(18, zoom) - v_chrome);
        } else if (tp && (arcr_str_eq_nocase(tp, "submit") || arcr_str_eq_nocase(tp, "button") ||
                          arcr_str_eq_nocase(tp, "reset"))) {
            box->control_kind = ARCR_CTRL_BUTTON;
            const char *lbl = arcr_dom_get_attr_cstr(ctx->doc, node, "value");
            if (!lbl || lbl[0] == '\0') lbl = "Submit";
            content_h = (definite_bh > 0)
                            ? ARCR_MAX(st->font_px, definite_bh - v_chrome)
                            : ((st->font_px * 135) / 100);
            int32_t base_y = cy + (content_h * 78) / 100;
            emit_single_line_text(ctx->tree, ctx->fonts, box, lbl, strlen(lbl),
                                   cx, base_y, cw, st->color_argb, false, true, false);
        } else {
            box->control_kind = ARCR_CTRL_TEXT_INPUT;
            content_h = (definite_bh > 0)
                            ? ARCR_MAX(st->font_px, definite_bh - v_chrome)
                            : ((st->font_px * 135) / 100);
            int32_t     base_y    = cy + (content_h * 78) / 100;
            const char *live      = arcr_dom_get_input_value(ctx->doc, node);
            bool        is_pw     = (tp && arcr_str_eq_nocase(tp, "password"));
            bool        has_focus = ((node->state_flags & ARCR_STATE_FOCUS) != 0u);
            if (live && live[0] != '\0') {
                emit_single_line_text(ctx->tree, ctx->fonts, box, live, strlen(live),
                                      cx, base_y, cw, st->color_argb, is_pw, false, has_focus);
            } else {
                const char *ph = arcr_dom_get_attr_cstr(ctx->doc, node, "placeholder");
                if (ph && ph[0] != '\0') {
                    emit_single_line_text(ctx->tree, ctx->fonts, box, ph, strlen(ph),
                                          cx, base_y, cw, 0xFF94A3B8u, false, false, false);
                }
            }
        }
    } else if (is_textarea) {
        box->control_kind = ARCR_CTRL_TEXT_INPUT;
        int32_t rows = 4;
        const char *rs = arcr_dom_get_attr_cstr(ctx->doc, node, "rows");
        if (rs && arcr_parse_i32(rs, strlen(rs), &rows)) rows = ARCR_CLAMP(rows, 1, 32);
        int32_t def_h = rows * ((st->font_px * 145) / 100);

        const char *live = arcr_dom_get_input_value(ctx->doc, node);
        const char *src  = (live && live[0] != '\0')
                               ? live
                               : arcr_dom_get_attr_cstr(ctx->doc, node, "placeholder");
        bool is_ph = (!live || live[0] == '\0');
        if (src && src[0] != '\0') {
            ArcrComputedStyle *txt_st = (ArcrComputedStyle *)arcr_arena_alloc(
                &ctx->tree->arena, sizeof(ArcrComputedStyle), sizeof(void *));
            if (txt_st) {
                *txt_st = *st;
                if (is_ph) txt_st->color_argb = 0xFF94A3B8u;
                txt_st->white_space_pre = true;

                InlineBuf ibuf = {NULL, 0u, 0u};
                ArcrDomNode tmp_tn;
                memset(&tmp_tn, 0, sizeof(tmp_tn));
                tmp_tn.kind     = ARCR_NODE_TEXT;
                tmp_tn.text     = src;
                tmp_tn.text_len = strlen(src);
                collect_inline_items(ctx, &tmp_tn, child_pos_owner, txt_st, NULL, NULL, cw, &ibuf);
                if (ibuf.count > 0u) {
                    content_h = layout_inline_run(ctx, box, &ibuf, cx, cy, cw);
                }
                free(ibuf.items);
            }
        }
        if (content_h < def_h) content_h = def_h;
    } else if (is_select) {
        box->control_kind = ARCR_CTRL_BUTTON;
        char opt_label[128];
        opt_label[0] = '\0';
        for (const ArcrDomNode *opt = node->first_child; opt != NULL; opt = opt->next_sibling) {
            if (opt->kind == ARCR_NODE_ELEMENT &&
                arcr_str_eq_nocase(arcr_atom_cstr(ctx->doc->atoms, opt->tag), "option")) {
                if (opt_label[0] == '\0' ||
                    arcr_dom_get_attr_cstr(ctx->doc, opt, "selected") != NULL) {
                    arcr_dom_get_text_content(opt, opt_label, sizeof(opt_label));
                    if (arcr_dom_get_attr_cstr(ctx->doc, opt, "selected") != NULL) break;
                }
            }
        }
        content_h = (definite_bh > 0)
                        ? ARCR_MAX(st->font_px, definite_bh - v_chrome)
                        : ((st->font_px * 135) / 100);
        if (opt_label[0] != '\0') {
            int32_t base_y = cy + (content_h * 78) / 100;
            emit_single_line_text(ctx->tree, ctx->fonts, box, opt_label, strlen(opt_label),
                                  cx, base_y, cw, st->color_argb, false, false, false);
        }
    } else if (st->display == ARCR_DISPLAY_FLEX ||
               st->display == ARCR_DISPLAY_INLINE_FLEX) {
        content_h = layout_flex_children(ctx, node, box, child_pos_owner, cx, cy, cw, child_def_h);
    } else if (st->display == ARCR_DISPLAY_GRID) {
        content_h = layout_grid_children(ctx, node, box, child_pos_owner, cx, cy, cw, child_def_h);
    } else if (st->display == ARCR_DISPLAY_TABLE) {
        int32_t used_tw = cw;
        content_h = layout_table_children(ctx, node, box, child_pos_owner,
                                          cx, cy, cw, &used_tw);
        if (forced_border_w == 0 && st->width_spec == 0) {
            cw = used_tw;
            bw = cw + h_chrome;
        }
    } else {
        if (is_button) box->control_kind = ARCR_CTRL_BUTTON;

        bool is_ol = arcr_str_eq_nocase(tname, "ol");
        int32_t next_li_ord = 1;
        if (is_ol) {
            const char *start_attr = arcr_dom_get_attr_cstr(ctx->doc, node, "start");
            if (start_attr) {
                int32_t sv = 1;
                if (arcr_parse_i32(start_attr, strlen(start_attr), &sv) && sv >= 0) {
                    next_li_ord = sv;
                }
            }
        }

        int32_t cursor_y = cy;
        const ArcrDomNode *flow_parent = node->shadow_root ? node->shadow_root : node;
        const ArcrDomNode *ch = flow_parent->first_child;
        while (ch) {
            if (is_inline_flow_child(ctx->doc->atoms, ch)) {
                InlineBuf ibuf = {NULL, 0u, 0u};
                while (ch && is_inline_flow_child(ctx->doc->atoms, ch)) {
                    collect_inline_items(ctx, ch, child_pos_owner, st, NULL, NULL, cw, &ibuf);
                    ch = ch->next_sibling;
                }
                if (ibuf.count > 0u) {
                    cursor_y = clamp_coord(
                        (int64_t)cursor_y + layout_inline_run(ctx, box, &ibuf, cx, cursor_y, cw));
                }
                free(ibuf.items);
            } else if (ch->kind == ARCR_NODE_ELEMENT && ch->computed_style &&
                       ch->computed_style->display == ARCR_DISPLAY_CONTENTS) {
                /* Flatten block-containing display:contents children in normal flow */
                DomNodeVec flat = {NULL, 0u, 0u};
                collect_flow_element_children(ctx, ch, child_pos_owner, cx, cursor_y, cw, &flat);
                for (size_t fi = 0u; fi < flat.count; ++fi) {
                    ArcrLayoutBox *cbox = layout_element_node(
                        ctx, flat.items[fi], child_pos_owner, cx, cursor_y, cw,
                        child_def_h, false, 0, 0);
                    if (cbox) {
                        append_child_box(box, cbox);
                        cursor_y = clamp_coord((int64_t)cbox->border_rect.y +
                                               cbox->border_rect.height +
                                               cbox->style->margin.bottom);
                    }
                }
                free(flat.items);
                ch = ch->next_sibling;
            } else if (is_out_of_flow_el(ch)) {
                ArcrLayoutBox *owner = (ch->computed_style->position == ARCR_POS_FIXED)
                                           ? ctx->tree->root_box
                                           : child_pos_owner;
                push_deferred_pos(ctx, ch, owner, cx, cursor_y, cw,
                                  ch->computed_style->position == ARCR_POS_FIXED);
                ch = ch->next_sibling;
            } else {
                int32_t ord = 0;
                if (ch->kind == ARCR_NODE_ELEMENT &&
                    arcr_str_eq_nocase(arcr_atom_cstr(ctx->doc->atoms, ch->tag), "li")) {
                    ord = is_ol ? (next_li_ord++) : 0;
                }
                ArcrLayoutBox *cbox = layout_element_node(
                    ctx, ch, child_pos_owner, cx, cursor_y, cw, child_def_h, false, 0, ord);
                if (cbox) {
                    append_child_box(box, cbox);
                    cursor_y = clamp_coord((int64_t)cbox->border_rect.y +
                                           cbox->border_rect.height +
                                           cbox->style->margin.bottom);
                }
                ch = ch->next_sibling;
            }
        }
        content_h = cursor_y - cy;
    }

    /* 3. Ordered list marker ("1.", "2.", ...) */
    if (box->is_list_item && box->list_ordinal > 0) {
        char num_buf[24];
        int nlen = snprintf(num_buf, sizeof(num_buf), "%d.", (int)box->list_ordinal);
        if (nlen > 0) {
            ArcrLayoutBox *mk = (ArcrLayoutBox *)arcr_arena_calloc(
                &ctx->tree->arena, 1u, sizeof(ArcrLayoutBox), sizeof(void *));
            if (mk) {
                mk->dom_node = NULL;
                mk->style    = st;
                int32_t mw = measure_utf8_string_px_ex(
                    ctx->fonts, num_buf, (size_t)nlen, st->font_px,
                    st->font_bold, st->font_monospace, false, 0u);
                int32_t mx = cx - mw - zpx(6, zoom);
                int32_t lh = compute_style_line_h(st);
                int32_t base_y = cy + (lh * 78) / 100;
                mk->content_rect = (ArcrRect){mx, cy, mw + 4, lh};
                mk->border_rect  = mk->content_rect;
                emit_single_line_text(ctx->tree, ctx->fonts, mk, num_buf, (size_t)nlen,
                                      mx, base_y, mw + 4, st->color_argb, false, false, false);
                append_child_box(box, mk);
            }
        }
    }

    /* 4. Finalize Box Height (CSS height, height_pct, aspect-ratio, min-height, max-height) */
    int32_t bh = clamp_coord((int64_t)v_chrome + content_h);
    if (definite_bh > 0 && !is_replaced) {
        bh        = ARCR_MAX(v_chrome, definite_bh);
        content_h = bh - v_chrome;
    }
    if (st->min_height_px > 0) {
        int32_t min_bh = css_h_to_border_h(st->min_height_px, st, v_chrome);
        if (bh < min_bh) {
            bh        = min_bh;
            content_h = ARCR_MAX(0, bh - v_chrome);
        }
    }
    if (st->max_height_px > 0) {
        int32_t max_bh = css_h_to_border_h(st->max_height_px, st, v_chrome);
        if (bh > max_bh) {
            bh        = ARCR_MAX(v_chrome, max_bh);
            content_h = ARCR_MAX(0, bh - v_chrome);
        }
    }

    if (st->sr_only_clip) {
        bw        = ARCR_MIN(bw, 1);
        bh        = ARCR_MIN(bh, 1);
        cw        = 0;
        content_h = 0;
    } else if (!is_replaced && st->mask_image_url &&
               arcr_str_starts_with_nocase(st->mask_image_url, "data:image/svg+xml") &&
               cw > 0 && content_h > 0 && cw <= 512 && content_h <= 512) {
        uint32_t mask_tint = (((st->bg_argb >> 24) & 0xFFu) > 0u) ? st->bg_argb : st->color_argb;
        if (!(node->surface && node->surface->argb_pixels &&
              node->svg_rendered_color == mask_tint &&
              node->svg_rendered_w == cw && node->svg_rendered_h == content_h)) {
            ArcrImage mimg = {0, 0, NULL};
            if (arcr_image_decode_data_uri(st->mask_image_url, strlen(st->mask_image_url), &mimg) &&
                mimg.pixels && mimg.width > 0 && mimg.height > 0) {
                ArcrImage resized = {0, 0, NULL};
                const ArcrImage *src_img = &mimg;
                if ((mimg.width != cw || mimg.height != content_h) &&
                    arcr_image_resize_argb(mimg.pixels, mimg.width, mimg.height,
                                           cw, content_h, &resized) &&
                    resized.pixels) {
                    src_img = &resized;
                }
                ArcrDomNode    *mut_node = (ArcrDomNode *)node;
                ArcrDomSurface *surf = arcr_dom_ensure_surface((ArcrDocument *)ctx->doc, mut_node,
                                                               src_img->width, src_img->height);
                if (surf && surf->argb_pixels) {
                    uint32_t ta  = (mask_tint >> 24) & 0xFFu;
                    uint32_t rgb = mask_tint & 0x00FFFFFFu;
                    size_t   npx = (size_t)src_img->width * (size_t)src_img->height;
                    for (size_t pi = 0u; pi < npx; ++pi) {
                        uint32_t ma = (src_img->pixels[pi] >> 24) & 0xFFu;
                        uint32_t fa = (ma * ta + 127u) / 255u;
                        surf->argb_pixels[pi] = (fa == 0u) ? 0u : ((fa << 24) | rgb);
                    }
                    mut_node->svg_rendered_color = mask_tint;
                    mut_node->svg_rendered_w     = src_img->width;
                    mut_node->svg_rendered_h     = src_img->height;
                }
                arcr_image_free(&resized);
            }
            arcr_image_free(&mimg);
        }
    }

    box->content_rect = (ArcrRect){cx, cy, cw, content_h};
    box->border_rect  = (ArcrRect){bx, by, bw, bh};

    /* 5. Resolve any `position: absolute` descendants scoped to this positioned box */
    if (child_pos_owner == box) {
        resolve_deferred_for_owner(ctx, box, false);
    }

    /* 6. Apply `position: relative` offsets and 2D CSS `transform` */
    int32_t rel_dx = 0;
    int32_t rel_dy = 0;
    if (st->position == ARCR_POS_RELATIVE) {
        if (st->has_left)       rel_dx = resolve_pos_offset(st->left_px, avail_w);
        else if (st->has_right) rel_dx = -resolve_pos_offset(st->right_px, avail_w);

        int32_t vref = (parent_def_h > 0) ? parent_def_h : 0;
        if (st->has_top) {
            if (st->top_px > ARCR_POS_PCT_THRESHOLD || vref > 0) {
                rel_dy = resolve_pos_offset(st->top_px, vref);
            }
        } else if (st->has_bottom) {
            if (st->bottom_px > ARCR_POS_PCT_THRESHOLD || vref > 0) {
                rel_dy = -resolve_pos_offset(st->bottom_px, vref);
            }
        }
    }

    int32_t tx = clamp_coord((int64_t)st->translate_x_px +
                             ((int64_t)box->border_rect.width * st->translate_x_pct) / 100LL);
    int32_t ty = clamp_coord((int64_t)st->translate_y_px +
                             ((int64_t)box->border_rect.height * st->translate_y_pct) / 100LL);

    if (st->scale_x_milli != 1000 || st->scale_y_milli != 1000) {
        int32_t old_w = box->border_rect.width;
        int32_t old_h = box->border_rect.height;
        int32_t new_w = clamp_coord(((int64_t)old_w * st->scale_x_milli + 500LL) / 1000LL);
        int32_t new_h = clamp_coord(((int64_t)old_h * st->scale_y_milli + 500LL) / 1000LL);
        if (new_w < 0) new_w = 0;
        if (new_h < 0) new_h = 0;
        box->border_rect.x     += (old_w - new_w) / 2;
        box->border_rect.y     += (old_h - new_h) / 2;
        box->border_rect.width  = new_w;
        box->border_rect.height = new_h;
    }

    int32_t total_dx = clamp_coord((int64_t)rel_dx + tx);
    int32_t total_dy = clamp_coord((int64_t)rel_dy + ty);
    if (total_dx != 0 || total_dy != 0) {
        offset_box_recursive(box, total_dx, total_dy);
    }

    return box;
}

/* -------------------------------------------------------------------------
 * Post-Layout Top-Down Clip, Fixed, Z-Index & Effective Opacity Finalizer
 * ------------------------------------------------------------------------- */
static ArcrRect intersect_clip_rects(ArcrRect a, ArcrRect b) {
    int32_t x0 = ARCR_MAX(a.x, b.x);
    int32_t y0 = ARCR_MAX(a.y, b.y);
    int32_t x1 = ARCR_MIN(a.x + a.width, b.x + b.width);
    int32_t y1 = ARCR_MIN(a.y + a.height, b.y + b.height);
    if (x1 <= x0 || y1 <= y0) return (ArcrRect){x0, y0, 0, 0};
    return (ArcrRect){x0, y0, x1 - x0, y1 - y0};
}

static void clear_dom_layout_boxes_recursive(ArcrDomNode *n) {
    for (; n != NULL; n = n->next_sibling) {
        n->layout_x       = 0;
        n->layout_y       = 0;
        n->layout_w       = 0;
        n->layout_h       = 0;
        n->has_layout_box = false;
        if (n->first_child) {
            clear_dom_layout_boxes_recursive(n->first_child);
        }
        if (n->shadow_root) {
            clear_dom_layout_boxes_recursive(n->shadow_root);
        }
        if (n->template_content) {
            clear_dom_layout_boxes_recursive(n->template_content);
        }
    }
}

static void sync_dom_node_rect(ArcrDomNode *dn, ArcrRect r) {
    if (!dn) return;
    if (!dn->has_layout_box) {
        dn->layout_x       = r.x;
        dn->layout_y       = r.y;
        dn->layout_w       = r.width;
        dn->layout_h       = r.height;
        dn->has_layout_box = true;
    } else {
        int32_t x0 = ARCR_MIN(dn->layout_x, r.x);
        int32_t y0 = ARCR_MIN(dn->layout_y, r.y);
        int32_t x1 = ARCR_MAX(dn->layout_x + dn->layout_w, r.x + r.width);
        int32_t y1 = ARCR_MAX(dn->layout_y + dn->layout_h, r.y + r.height);
        dn->layout_x = x0;
        dn->layout_y = y0;
        dn->layout_w = ARCR_MAX(0, x1 - x0);
        dn->layout_h = ARCR_MAX(0, y1 - y0);
    }
}

static void finalize_box_state_recursive(ArcrLayoutBox *box,
                                         bool is_root_box,
                                         int32_t viewport_h,
                                         int32_t parent_sticky_limit_y,
                                         bool anc_fixed,
                                         bool anc_sticky,
                                         int32_t anc_sticky_anchor_y,
                                         int32_t anc_sticky_top_px,
                                         int32_t anc_sticky_max_dy,
                                         bool anc_has_clip, ArcrRect anc_clip,
                                         uint8_t anc_opacity,
                                         int32_t *inout_max_doc_bottom) {
    if (!box) return;
    const ArcrComputedStyle *st = box->style;

    bool enters_fixed = (!anc_fixed && st && st->position == ARCR_POS_FIXED);
    bool cur_fixed    = anc_fixed || enters_fixed;
    if (enters_fixed) {
        /* Viewport-fixed elements escape normal document overflow clips & sticky ancestors */
        anc_has_clip = false;
        anc_sticky   = false;
    }

    bool    cur_sticky          = anc_sticky;
    int32_t cur_sticky_anchor_y = anc_sticky_anchor_y;
    int32_t cur_sticky_top_px   = anc_sticky_top_px;
    int32_t cur_sticky_max_dy   = anc_sticky_max_dy;

    if (!cur_fixed && !cur_sticky && st && st->position == ARCR_POS_STICKY && st->has_top) {
        cur_sticky          = true;
        cur_sticky_anchor_y = box->border_rect.y;
        cur_sticky_top_px   = resolve_pos_offset(st->top_px, viewport_h);
        cur_sticky_max_dy   = ARCR_MAX(
            0, clamp_coord((int64_t)parent_sticky_limit_y -
                           ((int64_t)box->border_rect.y + box->border_rect.height)));
    }

    uint8_t own_op = st ? st->opacity : 255u;
    uint8_t eff_op = (uint8_t)(((uint32_t)anc_opacity * (uint32_t)own_op + 127u) / 255u);

    box->is_fixed          = cur_fixed;
    box->is_sticky         = cur_sticky;
    box->sticky_anchor_y   = cur_sticky_anchor_y;
    box->sticky_top_px     = cur_sticky_top_px;
    box->sticky_max_dy     = cur_sticky_max_dy;
    box->z_index           = st ? st->z_index : 0;
    box->effective_opacity = eff_op;

    bool     child_has_clip = anc_has_clip;
    ArcrRect child_clip     = anc_clip;

    if (st && st->sr_only_clip) {
        child_clip     = (ArcrRect){box->border_rect.x, box->border_rect.y, 0, 0};
        child_has_clip = true;
    } else if (st && (st->overflow_x != ARCR_OVERFLOW_VISIBLE ||
                      st->overflow_y != ARCR_OVERFLOW_VISIBLE)) {
        ArcrRect self_clip = box->border_rect;
        if (st->overflow_x == ARCR_OVERFLOW_VISIBLE) {
            self_clip.x     = -ARCR_LAYOUT_COORD_MAX;
            self_clip.width = ARCR_LAYOUT_COORD_MAX * 2;
        }
        if (st->overflow_y == ARCR_OVERFLOW_VISIBLE) {
            self_clip.y      = -ARCR_LAYOUT_COORD_MAX;
            self_clip.height = ARCR_LAYOUT_COORD_MAX * 2;
        }
        child_clip     = child_has_clip ? intersect_clip_rects(child_clip, self_clip) : self_clip;
        child_has_clip = true;
    }

    box->has_clip  = child_has_clip;
    box->clip_rect = child_clip;

    if (box->dom_node) {
        sync_dom_node_rect((ArcrDomNode *)box->dom_node, box->border_rect);
    }
    for (const ArcrInlineLinkSpan *ls = box->links_head; ls != NULL; ls = ls->next) {
        if (ls->link_node) {
            sync_dom_node_rect((ArcrDomNode *)ls->link_node, ls->rect);
        }
    }

    if (!cur_fixed && !child_has_clip) {
        int32_t bottom = clamp_coord((int64_t)box->border_rect.y + box->border_rect.height);
        if (bottom > *inout_max_doc_bottom) *inout_max_doc_bottom = bottom;
    }

    int32_t child_sticky_limit_y =
        is_root_box ? ARCR_LAYOUT_COORD_MAX
                    : clamp_coord((int64_t)box->content_rect.y + box->content_rect.height);

    for (ArcrLayoutBox *ch = box->first_child; ch != NULL; ch = ch->next_sibling) {
        finalize_box_state_recursive(ch, false, viewport_h, child_sticky_limit_y,
                                     cur_fixed, cur_sticky, cur_sticky_anchor_y,
                                     cur_sticky_top_px, cur_sticky_max_dy,
                                     child_has_clip, child_clip,
                                     eff_op, inout_max_doc_bottom);
    }
}

ArcrLayoutTree *arcr_layout_build_ex(const ArcrDocument *doc, ArcrFontSystem *fonts,
                                     int32_t viewport_width, int32_t viewport_height,
                                     int32_t zoom_percent) {
    if (!doc || !doc->body) return NULL;
    ArcrLayoutTree *tree = (ArcrLayoutTree *)calloc(1u, sizeof(ArcrLayoutTree));
    if (!tree) return NULL;

    if (doc->root) {
        clear_dom_layout_boxes_recursive((ArcrDomNode *)doc->root);
    }

    arcr_arena_init(&tree->arena, 32768u);
    tree->viewport_width  = (viewport_width > 64) ? viewport_width : 64;
    tree->viewport_height = (viewport_height > 64) ? viewport_height : 800;
    tree->zoom_percent    = (zoom_percent <= 0) ? 100 : ARCR_CLAMP(zoom_percent, 50, 200);

    LayoutCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.tree      = tree;
    ctx.doc       = doc;
    ctx.fonts     = fonts;
    ctx.cache_cap = doc->next_uid + 4u;
    ctx.intrinsic_w_cache = (int32_t *)malloc((size_t)ctx.cache_cap * sizeof(int32_t));
    if (ctx.intrinsic_w_cache) {
        for (uint32_t i = 0u; i < ctx.cache_cap; ++i) {
            ctx.intrinsic_w_cache[i] = -1;
        }
    }

    tree->root_box = layout_element_node(&ctx, doc->body, NULL,
                                         0, 0, tree->viewport_width, 0,
                                         false, 0, 0);

    if (tree->root_box) {
        /* Resolve any remaining unowned absolute or viewport-fixed boxes at root_box */
        resolve_deferred_for_owner(&ctx, tree->root_box, false);
        resolve_deferred_for_owner(&ctx, tree->root_box, true);

        int32_t max_bottom = tree->root_box->border_rect.y +
                             tree->root_box->border_rect.height +
                             tree->root_box->style->margin.bottom;
        finalize_box_state_recursive(tree->root_box, true, tree->viewport_height,
                                     ARCR_LAYOUT_COORD_MAX,
                                     false, false, 0, 0, 0,
                                     false, (ArcrRect){0, 0, 0, 0},
                                     255u, &max_bottom);
        tree->total_height = ARCR_MAX(tree->viewport_height, max_bottom);
    }

    free(ctx.deferred);
    free(ctx.intrinsic_w_cache);
    return tree;
}

ArcrLayoutTree *arcr_layout_build_zoomed(const ArcrDocument *doc, ArcrFontSystem *fonts,
                                         int32_t viewport_width, int32_t zoom_percent) {
    return arcr_layout_build_ex(doc, fonts, viewport_width, 800, zoom_percent);
}

ArcrLayoutTree *arcr_layout_build(const ArcrDocument *doc, ArcrFontSystem *fonts,
                                  int32_t viewport_width) {
    return arcr_layout_build_ex(doc, fonts, viewport_width, 800, 100);
}

void arcr_layout_destroy(ArcrLayoutTree *tree) {
    if (!tree) return;
    arcr_arena_destroy(&tree->arena);
    free(tree);
}

/* -------------------------------------------------------------------------
 * Z-Ordered Spatial Hit-Testing (Supports Fixed, Sticky, Scroll, Overflow Clip & Pointer-Events)
 * ------------------------------------------------------------------------- */
static bool point_in_rect(int32_t x, int32_t y, ArcrRect r) {
    return x >= r.x && x < (r.x + r.width) &&
           y >= r.y && y < (r.y + r.height);
}

static bool is_positioned_stack_child(const ArcrLayoutBox *ch) {
    if (!ch || !ch->style) return false;
    return ch->style->position != ARCR_POS_STATIC || ch->z_index != 0;
}

typedef struct HitZEntry {
    const ArcrLayoutBox *box;
    uint32_t             order;
} HitZEntry;

static int cmp_hit_z_entries(const void *pa, const void *pb) {
    const HitZEntry *a = (const HitZEntry *)pa;
    const HitZEntry *b = (const HitZEntry *)pb;
    if (a->box->z_index != b->box->z_index) {
        return (a->box->z_index < b->box->z_index) ? -1 : 1;
    }
    return (a->order < b->order) ? -1 : 1;
}

static void hit_test_walk_stacked(const ArcrLayoutBox *box,
                                  int32_t view_x, int32_t view_y, int32_t scroll_y,
                                  const ArcrLayoutBox **inout_best_box,
                                  const ArcrInlineLinkSpan **inout_best_link) {
    if (!box) return;
    if (box->style && box->style->sr_only_clip) return;

    /* Collect positioned children with z_index != 0 for exact paint-order walk */
    size_t pos_cnt = 0u;
    for (const ArcrLayoutBox *ch = box->first_child; ch != NULL; ch = ch->next_sibling) {
        if (is_positioned_stack_child(ch)) ++pos_cnt;
    }

    HitZEntry *pos_list = NULL;
    if (pos_cnt > 0u) {
        pos_list = (HitZEntry *)malloc(pos_cnt * sizeof(HitZEntry));
        if (pos_list) {
            uint32_t ord = 0u;
            size_t   k   = 0u;
            for (const ArcrLayoutBox *ch = box->first_child; ch != NULL; ch = ch->next_sibling) {
                if (is_positioned_stack_child(ch)) {
                    pos_list[k++] = (HitZEntry){ch, ord++};
                }
            }
            if (pos_cnt > 1u) {
                qsort(pos_list, pos_cnt, sizeof(HitZEntry), cmp_hit_z_entries);
            }
        }
    }

    /* 1. Negative z_index positioned children */
    if (pos_list) {
        for (size_t k = 0u; k < pos_cnt; ++k) {
            if (pos_list[k].box->z_index < 0) {
                hit_test_walk_stacked(pos_list[k].box, view_x, view_y, scroll_y,
                                      inout_best_box, inout_best_link);
            }
        }
    }

    /* 2. Self box & inline link spans */
    int32_t eff_scroll_y = scroll_y;
    if (box->is_fixed) {
        eff_scroll_y = 0;
    } else if (box->is_sticky) {
        int32_t sticky_dy = ARCR_CLAMP(
            clamp_coord((int64_t)scroll_y + box->sticky_top_px - box->sticky_anchor_y),
            0, box->sticky_max_dy);
        eff_scroll_y = clamp_coord((int64_t)scroll_y - sticky_dy);
    }

    int32_t tx = view_x;
    int32_t ty = clamp_coord((int64_t)view_y + eff_scroll_y);
    bool self_interactable = box->style &&
                             !box->style->visibility_hidden &&
                             !box->style->pointer_events_none &&
                             box->effective_opacity > 0u &&
                             (!box->has_clip || point_in_rect(tx, ty, box->clip_rect));

    if (self_interactable) {
        if (box->dom_node && point_in_rect(tx, ty, box->border_rect)) {
            *inout_best_box  = box;
            *inout_best_link = NULL;
        }
        for (const ArcrInlineLinkSpan *ls = box->links_head; ls != NULL; ls = ls->next) {
            if (point_in_rect(tx, ty, ls->rect)) {
                *inout_best_box  = box;
                *inout_best_link = ls;
            }
        }
    }

    /* 3. Normal-flow non-positioned children */
    for (const ArcrLayoutBox *ch = box->first_child; ch != NULL; ch = ch->next_sibling) {
        if (!is_positioned_stack_child(ch)) {
            hit_test_walk_stacked(ch, view_x, view_y, scroll_y,
                                  inout_best_box, inout_best_link);
        }
    }

    /* 4. z_index >= 0 positioned/fixed children in ascending z_index order */
    if (pos_list) {
        for (size_t k = 0u; k < pos_cnt; ++k) {
            if (pos_list[k].box->z_index >= 0) {
                hit_test_walk_stacked(pos_list[k].box, view_x, view_y, scroll_y,
                                      inout_best_box, inout_best_link);
            }
        }
        free(pos_list);
    }
}

void arcr_layout_hit_test_ex(const ArcrLayoutTree *tree, const ArcrDocument *doc,
                             int32_t view_x, int32_t view_y, int32_t scroll_y,
                             ArcrHitTestResult *out_hit) {
    if (!out_hit) return;
    out_hit->hit_node           = NULL;
    out_hit->clickable_ancestor = NULL;
    out_hit->link_href          = NULL;
    out_hit->cursor             = ARCR_CURSOR_ARROW;

    if (!tree || !tree->root_box || !doc) return;

    const ArcrLayoutBox      *best_box  = NULL;
    const ArcrInlineLinkSpan *best_link = NULL;
    hit_test_walk_stacked(tree->root_box, view_x, view_y, scroll_y, &best_box, &best_link);

    if (best_link && best_link->link_node) {
        out_hit->hit_node           = best_link->link_node;
        out_hit->clickable_ancestor = best_link->link_node;
        out_hit->link_href          = best_link->href;
        out_hit->cursor             = ARCR_CURSOR_POINTER;
        return;
    }

    if (!best_box || !best_box->dom_node) return;
    out_hit->hit_node = best_box->dom_node;

    if (best_box->control_kind == ARCR_CTRL_TEXT_INPUT) {
        out_hit->clickable_ancestor = best_box->dom_node;
        out_hit->cursor             = ARCR_CURSOR_TEXT;
        return;
    }
    if (best_box->control_kind == ARCR_CTRL_CHECKBOX ||
        best_box->control_kind == ARCR_CTRL_BUTTON ||
        best_box->control_kind == ARCR_CTRL_VIDEO_MEDIA) {
        out_hit->clickable_ancestor = best_box->dom_node;
        out_hit->cursor             = ARCR_CURSOR_POINTER;
        return;
    }

    for (const ArcrDomNode *anc = best_box->dom_node; anc != NULL; anc = anc->parent) {
        if (anc->kind != ARCR_NODE_ELEMENT) continue;
        const char *tn = arcr_atom_cstr(doc->atoms, anc->tag);
        if (arcr_str_eq_nocase(tn, "a")) {
            const char *href = arcr_dom_get_attr_cstr(doc, anc, "href");
            if (!out_hit->clickable_ancestor) out_hit->clickable_ancestor = anc;
            if (href) {
                out_hit->link_href = href;
                out_hit->cursor    = ARCR_CURSOR_POINTER;
            }
            return;
        }
        if (arcr_str_eq_nocase(tn, "button") || arcr_str_eq_nocase(tn, "select") ||
            arcr_str_eq_nocase(tn, "summary") || arcr_str_eq_nocase(tn, "video")) {
            out_hit->clickable_ancestor = anc;
            out_hit->cursor             = ARCR_CURSOR_POINTER;
            return;
        }
        if (arcr_str_eq_nocase(tn, "input")) {
            out_hit->clickable_ancestor = anc;
            const char *tp = arcr_dom_get_attr_cstr(doc, anc, "type");
            if (tp && (arcr_str_eq_nocase(tp, "checkbox") || arcr_str_eq_nocase(tp, "radio") ||
                       arcr_str_eq_nocase(tp, "submit") || arcr_str_eq_nocase(tp, "button") ||
                       arcr_str_eq_nocase(tp, "reset"))) {
                out_hit->cursor = ARCR_CURSOR_POINTER;
            } else {
                out_hit->cursor = ARCR_CURSOR_TEXT;
            }
            return;
        }
        if (arcr_str_eq_nocase(tn, "textarea")) {
            out_hit->clickable_ancestor = anc;
            out_hit->cursor             = ARCR_CURSOR_TEXT;
            return;
        }
        const char *role = arcr_dom_get_attr_cstr(doc, anc, "role");
        if (arcr_dom_get_attr_cstr(doc, anc, "onclick") != NULL ||
            (role && arcr_str_eq_nocase(role, "button"))) {
            if (!out_hit->clickable_ancestor) out_hit->clickable_ancestor = anc;
            out_hit->cursor = ARCR_CURSOR_POINTER;
        }
    }
}

void arcr_layout_hit_test(const ArcrLayoutTree *tree, const ArcrDocument *doc,
                          int32_t doc_x, int32_t doc_y, ArcrHitTestResult *out_hit) {
    arcr_layout_hit_test_ex(tree, doc, doc_x, doc_y, 0, out_hit);
}
