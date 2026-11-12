/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * layout/arcr_layout.c - Block, Flexbox, CSS Grid, Table, Form, Zoom & Korean/CJK Inline Layout
 */
#include "arcr_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int32_t zpx(int32_t px, int32_t zoom) {
    if (px == 0 || zoom == 100) return px;
    int32_t scaled = (px * zoom + ((px > 0) ? 50 : -50)) / 100;
    if (px > 0 && scaled < 1) scaled = 1;
    if (px < 0 && scaled > -1) scaled = -1;
    return scaled;
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
    zs->border_radius   = (src->border_radius >= 999) ? 999 : zpx(src->border_radius, zoom);
    zs->shadow_blur     = zpx(src->shadow_blur, zoom);
    zs->shadow_offset_y = zpx(src->shadow_offset_y, zoom);
    zs->gap_px          = zpx(src->gap_px, zoom);

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

    if (src->width_spec > 0)   zs->width_spec   = zpx(src->width_spec, zoom);
    if (src->min_width_px > 0) zs->min_width_px = zpx(src->min_width_px, zoom);
    if (src->max_width_px > 0) zs->max_width_px = zpx(src->max_width_px, zoom);
    if (src->height_px > 0)    zs->height_px    = zpx(src->height_px, zoom);
    if (src->min_height_px > 0) zs->min_height_px = zpx(src->min_height_px, zoom);
    return zs;
}

static void offset_box_recursive(ArcrLayoutBox *box, int32_t dx, int32_t dy) {
    if (!box || (dx == 0 && dy == 0)) return;
    box->border_rect.x  += dx;
    box->border_rect.y  += dy;
    box->content_rect.x += dx;
    box->content_rect.y += dy;

    for (uint32_t i = 0u; i < box->glyph_count; ++i) {
        box->glyphs[i].x          += dx;
        box->glyphs[i].baseline_y += dy;
    }
    for (ArcrInlineBgPill *p = box->pills_head; p != NULL; p = p->next) {
        p->rect.x += dx;
        p->rect.y += dy;
    }
    for (ArcrInlineLinkSpan *l = box->links_head; l != NULL; l = l->next) {
        l->rect.x += dx;
        l->rect.y += dy;
    }
    for (ArcrLayoutBox *ch = box->first_child; ch != NULL; ch = ch->next_sibling) {
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

static int32_t measure_codepoint_adv(ArcrFontSystem *fonts, uint32_t cp,
                                     int32_t font_px, bool bold, bool monospace) {
    uint32_t mcp = (cp == 0x00A0u) ? (uint32_t)' ' : cp;
    ArcrGlyphMetrics gm;
    if (arcr_font_measure_glyph_ex(fonts, mcp, font_px, bold, monospace, &gm)) {
        return ARCR_MAX(1, gm.advance_px);
    }
    return ARCR_MAX(4, font_px / 2);
}

static int32_t measure_utf8_string_px(ArcrFontSystem *fonts, const char *s, size_t len,
                                      int32_t font_px, bool bold, bool monospace) {
    if (!s || len == 0u) return 0;
    int32_t w   = 0;
    size_t  pos = 0u;
    while (pos < len) {
        uint32_t cp = 0u;
        size_t step = arcr_utf8_decode_one(s + pos, len - pos, &cp);
        if (step == 0u) break;
        pos += step;
        if (cp == '\r' || cp == '\n') continue;
        w += measure_codepoint_adv(fonts, cp, font_px, bold, monospace);
    }
    return w;
}

static int32_t resolve_spec_px(int32_t spec, int32_t ref_w) {
    if (spec > 0) return spec;
    if (spec < 0) return (ref_w * (-spec)) / 100;
    return 0;
}

static int32_t apply_min_max_w(int32_t w, const ArcrComputedStyle *st, int32_t avail_w) {
    int32_t min_w = resolve_spec_px(st->min_width_px, avail_w);
    int32_t max_w = resolve_spec_px(st->max_width_px, avail_w);
    if (max_w > 0 && w > max_w) w = max_w;
    if (min_w > 0 && w < min_w) w = min_w;
    return w;
}

/* Forward declaration of master element layout function */
static ArcrLayoutBox *layout_element_node(ArcrLayoutTree *tree, const ArcrDocument *doc,
                                          ArcrFontSystem *fonts, const ArcrDomNode *node,
                                          int32_t avail_x, int32_t start_y, int32_t avail_w,
                                          bool shrink_to_fit, int32_t forced_border_w,
                                          int32_t list_ordinal);

/* Measures preferred natural border-box width of an element for inline-block / flex / table */
static int32_t measure_natural_border_w(const ArcrDocument *doc, ArcrFontSystem *fonts,
                                        const ArcrDomNode *node, int32_t zoom, int32_t max_w) {
    if (!node || node->kind != ARCR_NODE_ELEMENT || !node->computed_style) return 0;
    const ArcrComputedStyle *raw_st = node->computed_style;
    if (raw_st->display == ARCR_DISPLAY_NONE) return 0;

    int32_t h_chrome = zpx(raw_st->border.left + raw_st->border.right +
                           raw_st->padding.left + raw_st->padding.right, zoom);
    if (raw_st->width_spec > 0) {
        return ARCR_MIN(max_w, zpx(raw_st->width_spec, zoom));
    }
    if (raw_st->width_spec < 0) {
        return ARCR_CLAMP((max_w * (-raw_st->width_spec)) / 100, 16, max_w);
    }

    const char *tname = arcr_atom_cstr(doc->atoms, node->tag);
    int32_t font_px   = ARCR_MAX(8, zpx(raw_st->font_px, zoom));

    if (arcr_str_eq_nocase(tname, "img") || arcr_str_eq_nocase(tname, "canvas")) {
        int32_t nw = arcr_str_eq_nocase(tname, "canvas") ? zpx(300, zoom) : zpx(120, zoom);
        int32_t nh = arcr_str_eq_nocase(tname, "canvas") ? zpx(150, zoom) : zpx(80, zoom);
        if (node->surface && node->surface->width > 0 && node->surface->height > 0) {
            nw = zpx(node->surface->width, zoom);
            nh = zpx(node->surface->height, zoom);
        } else {
            const char *wa = arcr_dom_get_attr_cstr(doc, node, "width");
            const char *ha = arcr_dom_get_attr_cstr(doc, node, "height");
            int32_t pw = 0, ph = 0;
            if (wa && arcr_parse_i32(wa, strlen(wa), &pw) && pw > 0) nw = zpx(pw, zoom);
            if (ha && arcr_parse_i32(ha, strlen(ha), &ph) && ph > 0) nh = zpx(ph, zoom);
        }
        if (raw_st->height_px > 0 && nh > 0) {
            nw = (zpx(raw_st->height_px, zoom) * nw) / nh;
        }
        return ARCR_CLAMP(nw + h_chrome, 16, max_w);
    }

    if (arcr_str_eq_nocase(tname, "input")) {
        const char *tp = arcr_dom_get_attr_cstr(doc, node, "type");
        if (tp && (arcr_str_eq_nocase(tp, "checkbox") || arcr_str_eq_nocase(tp, "radio"))) {
            return zpx(18, zoom);
        }
        if (tp && (arcr_str_eq_nocase(tp, "submit") || arcr_str_eq_nocase(tp, "button") ||
                   arcr_str_eq_nocase(tp, "reset"))) {
            const char *lbl = arcr_dom_get_attr_cstr(doc, node, "value");
            if (!lbl || lbl[0] == '\0') lbl = "Submit";
            int32_t tw = measure_utf8_string_px(fonts, lbl, strlen(lbl),
                                                font_px, raw_st->font_bold, raw_st->font_monospace);
            return ARCR_CLAMP(tw + h_chrome + zpx(8, zoom), zpx(48, zoom), max_w);
        }
        return ARCR_MIN(max_w, zpx(220, zoom));
    }

    if (arcr_str_eq_nocase(tname, "textarea")) {
        return ARCR_MIN(max_w, zpx(300, zoom));
    }

    int32_t cur_line = 0;
    int32_t best_w   = 0;
    for (const ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        if (ch->kind == ARCR_NODE_TEXT) {
            cur_line += measure_utf8_string_px(fonts, ch->text, ch->text_len,
                                               font_px, raw_st->font_bold, raw_st->font_monospace);
            if (cur_line > best_w) best_w = cur_line;
        } else if (ch->kind == ARCR_NODE_ELEMENT) {
            if (arcr_str_eq_nocase(arcr_atom_cstr(doc->atoms, ch->tag), "br")) {
                cur_line = 0;
                continue;
            }
            if (!ch->computed_style || ch->computed_style->display == ARCR_DISPLAY_NONE) continue;
            int32_t sub = measure_natural_border_w(doc, fonts, ch, zoom, max_w);
            int32_t sub_m = zpx(ch->computed_style->margin.left +
                                ch->computed_style->margin.right, zoom);
            if (ch->computed_style->display == ARCR_DISPLAY_INLINE ||
                ch->computed_style->display == ARCR_DISPLAY_INLINE_BLOCK) {
                cur_line += sub + sub_m;
                if (cur_line > best_w) best_w = cur_line;
            } else {
                cur_line = 0;
                if (sub + sub_m > best_w) best_w = sub + sub_m;
            }
        }
    }
    if (raw_st->display == ARCR_DISPLAY_FLEX && raw_st->flex_direction == ARCR_FLEX_ROW) {
        int32_t sum = 0;
        int32_t cnt = 0;
        for (const ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
            if (ch->kind == ARCR_NODE_ELEMENT && ch->computed_style &&
                ch->computed_style->display != ARCR_DISPLAY_NONE) {
                sum += measure_natural_border_w(doc, fonts, ch, zoom, max_w);
                ++cnt;
            }
        }
        if (cnt > 1) sum += (cnt - 1) * zpx(raw_st->gap_px, zoom);
        if (sum > best_w) best_w = sum;
    }

    int32_t total = best_w + h_chrome;
    if (arcr_str_eq_nocase(tname, "button")) {
        total = ARCR_MAX(total + zpx(4, zoom), zpx(40, zoom));
    }
    return ARCR_CLAMP(total, ARCR_MAX(12, h_chrome), max_w);
}

/* -------------------------------------------------------------------------
 * Inline Formatting Context (Korean/CJK Wrap, Links, Pills & Inline-Blocks)
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
    if (n->kind == ARCR_NODE_ELEMENT) {
        if (arcr_str_eq_nocase(arcr_atom_cstr(atoms, n->tag), "br")) return true;
        if (n->computed_style &&
            (n->computed_style->display == ARCR_DISPLAY_INLINE ||
             n->computed_style->display == ARCR_DISPLAY_INLINE_BLOCK)) {
            return true;
        }
    }
    return false;
}

static void collect_inline_items(ArcrLayoutTree *tree, const ArcrDocument *doc,
                                 ArcrFontSystem *fonts, const ArcrDomNode *node,
                                 const ArcrComputedStyle *cur_style,
                                 const ArcrDomNode *active_link, const char *active_href,
                                 int32_t line_max_w, InlineBuf *buf) {
    if (!node) return;

    if (node->kind == ARCR_NODE_TEXT) {
        size_t pos = 0u;
        while (pos < node->text_len) {
            uint32_t cp = 0u;
            size_t step = arcr_utf8_decode_one(node->text + pos, node->text_len - pos, &cp);
            if (step == 0u) break;
            pos += step;

            if (cp == '\r') continue;
            if (cp == '\n') {
                if (cur_style->white_space_pre) {
                    inline_buf_push(buf, (InlineItem){
                        '\n', 0, cur_style, active_link, active_href, NULL, true
                    });
                } else {
                    int32_t sp_adv = measure_codepoint_adv(
                        fonts, ' ', cur_style->font_px, cur_style->font_bold,
                        cur_style->font_monospace);
                    inline_buf_push(buf, (InlineItem){
                        ' ', sp_adv, cur_style, active_link, active_href, NULL, false
                    });
                }
                continue;
            }
            if (cp == '\t' && cur_style->white_space_pre) {
                int32_t sp_adv = measure_codepoint_adv(
                    fonts, ' ', cur_style->font_px, cur_style->font_bold,
                    cur_style->font_monospace);
                for (int t = 0; t < 4; ++t) {
                    inline_buf_push(buf, (InlineItem){
                        ' ', sp_adv, cur_style, active_link, active_href, NULL, false
                    });
                }
                continue;
            }

            int32_t adv = measure_codepoint_adv(
                fonts, cp, cur_style->font_px, cur_style->font_bold, cur_style->font_monospace);
            inline_buf_push(buf, (InlineItem){
                cp, adv, cur_style, active_link, active_href, NULL, false
            });
        }
        return;
    }

    if (node->kind == ARCR_NODE_ELEMENT) {
        if (node->computed_style && node->computed_style->display == ARCR_DISPLAY_NONE) return;
        const char *tname = arcr_atom_cstr(doc->atoms, node->tag);
        if (arcr_str_eq_nocase(tname, "br")) {
            inline_buf_push(buf, (InlineItem){
                '\n', 0, cur_style, active_link, active_href, NULL, true
            });
            return;
        }

        const ArcrComputedStyle *st = node->computed_style
                                          ? zoom_style_if_needed(&tree->arena, node->computed_style,
                                                                 tree->zoom_percent)
                                          : cur_style;

        if (arcr_str_eq_nocase(tname, "a")) {
            const char *href = arcr_dom_get_attr_cstr(doc, node, "href");
            if (href) {
                active_link = node;
                active_href = href;
            }
        }

        if (st->display == ARCR_DISPLAY_INLINE_BLOCK) {
            ArcrLayoutBox *ib = layout_element_node(tree, doc, fonts, node, 0, 0,
                                                    line_max_w, true, 0, 0);
            if (ib) {
                int32_t ib_total_w = ib->style->margin.left + ib->border_rect.width +
                                     ib->style->margin.right;
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
        for (const ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
            if (is_inline_flow_child(doc->atoms, ch)) {
                collect_inline_items(tree, doc, fonts, ch, st, active_link, active_href,
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
        box->glyphs[g].x += dx;
    }
    for (ArcrInlineBgPill *p = box->pills_head; p != pill_stop && p != NULL; p = p->next) {
        p->rect.x += dx;
    }
    for (ArcrInlineLinkSpan *l = box->links_head; l != link_stop && l != NULL; l = l->next) {
        l->rect.x += dx;
    }
    for (size_t b = 0u; b < line_box_cnt; ++b) {
        offset_box_recursive(line_boxes[b], dx, 0);
    }
}

static void align_finished_line(ArcrLayoutBox *box, int32_t line_w, int32_t max_width,
                                ArcrPositionedGlyph *out_g, uint32_t g_start, uint32_t g_end,
                                ArcrInlineBgPill *pill_stop, ArcrInlineLinkSpan *link_stop,
                                ArcrLayoutBox **line_boxes, size_t line_box_cnt) {
    ArcrTextAlign ta = box->style->text_align;
    if (ta == ARCR_TEXT_ALIGN_LEFT || line_w >= max_width) return;

    int32_t dx = (ta == ARCR_TEXT_ALIGN_CENTER) ? ((max_width - line_w) / 2)
                                                : (max_width - line_w);
    if (dx <= 0) return;
    ArcrPositionedGlyph *saved = box->glyphs;
    box->glyphs = out_g;
    shift_line_artifacts(box, dx, g_start, g_end, pill_stop, link_stop, line_boxes, line_box_cnt);
    box->glyphs = saved;
}

static int32_t layout_inline_run(ArcrLayoutTree *tree, ArcrLayoutBox *box,
                                 const InlineBuf *buf, int32_t start_x, int32_t start_y,
                                 int32_t max_width) {
    if (buf->count == 0u) return 0;
    if (max_width < 24) max_width = 24;

    ArcrPositionedGlyph *out_g = (ArcrPositionedGlyph *)arcr_arena_alloc(
        &tree->arena, buf->count * sizeof(ArcrPositionedGlyph), sizeof(void *));
    if (!out_g) return 0;

    int32_t lh_pct      = (box->style->line_height_pct >= 100) ? box->style->line_height_pct : 145;
    int32_t base_line_h = ARCR_MAX(16, (box->style->font_px * lh_pct) / 100);

    uint32_t gcount        = 0u;
    uint32_t line_g_start  = 0u;
    int32_t  pen_x         = 0;
    int32_t  cur_y         = start_y;
    int32_t  cur_line_h    = base_line_h;

    ArcrInlineBgPill   *line_pill_mark = box->pills_head;
    ArcrInlineLinkSpan *line_link_mark = box->links_head;
    ArcrLayoutBox      *line_boxes[64];
    size_t              line_box_cnt   = 0u;

    size_t i = 0u;
    while (i < buf->count) {
        if (pen_x == 0 && !box->style->white_space_pre &&
            buf->items[i].cp == ' ' && !buf->items[i].forced_break &&
            !buf->items[i].inline_box) {
            ++i;
            continue;
        }
        if (buf->items[i].forced_break) {
            align_finished_line(box, pen_x, max_width, out_g, line_g_start, gcount,
                                line_pill_mark, line_link_mark, line_boxes, line_box_cnt);
            cur_y         += cur_line_h;
            pen_x          = 0;
            cur_line_h     = base_line_h;
            line_g_start   = gcount;
            line_pill_mark = box->pills_head;
            line_link_mark = box->links_head;
            line_box_cnt   = 0u;
            ++i;
            continue;
        }

        /* Group unbreakable cluster [i .. cend) */
        size_t  cend            = i + 1u;
        int32_t cluster_w       = buf->items[i].adv_px;
        int32_t cluster_max_lh  = base_line_h;

        if (buf->items[i].inline_box) {
            ArcrLayoutBox *ib = buf->items[i].inline_box;
            int32_t ib_h = ib->style->margin.top + ib->border_rect.height +
                           ib->style->margin.bottom;
            if (ib_h > cluster_max_lh) cluster_max_lh = ib_h;
        } else {
            int32_t ilh = (buf->items[i].style->font_px *
                           ARCR_MAX(100, buf->items[i].style->line_height_pct)) / 100;
            if (ilh > cluster_max_lh) cluster_max_lh = ilh;

            while (cend < buf->count && !buf->items[cend].forced_break &&
                   !buf->items[cend].inline_box) {
                if (box->style->white_space_pre && buf->items[cend].cp == ' ') break;
                bool keep_all = buf->items[cend - 1u].style->word_break_keep_all ||
                                buf->items[cend].style->word_break_keep_all;
                if (buf->items[cend - 1u].cp != 0x00A0u &&
                    buf->items[cend].cp != 0x00A0u &&
                    arcr_unicode_can_break_line(buf->items[cend - 1u].cp,
                                                buf->items[cend].cp, keep_all)) {
                    break;
                }
                cluster_w += buf->items[cend].adv_px;
                int32_t clh = (buf->items[cend].style->font_px *
                               ARCR_MAX(100, buf->items[cend].style->line_height_pct)) / 100;
                if (clh > cluster_max_lh) cluster_max_lh = clh;
                ++cend;
            }
        }

        if (!box->style->white_space_pre && pen_x > 0 && pen_x + cluster_w > max_width) {
            align_finished_line(box, pen_x, max_width, out_g, line_g_start, gcount,
                                line_pill_mark, line_link_mark, line_boxes, line_box_cnt);
            cur_y         += cur_line_h;
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
            if (!box->style->white_space_pre && !it->inline_box &&
                pen_x > 0 && pen_x + it->adv_px > max_width) {
                align_finished_line(box, pen_x, max_width, out_g, line_g_start, gcount,
                                    line_pill_mark, line_link_mark, line_boxes, line_box_cnt);
                cur_y         += cur_line_h;
                pen_x          = 0;
                baseline_y     = cur_y + (cur_line_h * 78) / 100;
                line_g_start   = gcount;
                line_pill_mark = box->pills_head;
                line_link_mark = box->links_head;
                line_box_cnt   = 0u;
            }

            if (it->inline_box) {
                ArcrLayoutBox *ib = it->inline_box;
                int32_t ib_x = start_x + pen_x + ib->style->margin.left;
                int32_t ib_y = cur_y + ARCR_MAX(0, (cur_line_h - ib->border_rect.height) / 2);
                offset_box_recursive(ib, ib_x - ib->border_rect.x, ib_y - ib->border_rect.y);
                append_child_box(box, ib);
                if (line_box_cnt < ARCR_ARRAY_LEN(line_boxes)) {
                    line_boxes[line_box_cnt++] = ib;
                }
                if (it->link_node && it->link_href) {
                    ArcrInlineLinkSpan *ls = (ArcrInlineLinkSpan *)arcr_arena_alloc(
                        &tree->arena, sizeof(ArcrInlineLinkSpan), sizeof(void *));
                    if (ls) {
                        ls->rect      = ib->border_rect;
                        ls->link_node = it->link_node;
                        ls->href      = it->link_href;
                        ls->next      = box->links_head;
                        box->links_head = ls;
                    }
                }
                pen_x += it->adv_px;
                continue;
            }

            /* Inline background pill (e.g. <code>, <mark>) */
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
                        &tree->arena, sizeof(ArcrInlineBgPill), sizeof(void *));
                    if (pill) {
                        pill->rect    = (ArcrRect){start_x + pen_x, pill_y, it->adv_px, pill_h};
                        pill->radius  = it->style->border_radius;
                        pill->bg_argb = it->style->bg_argb;
                        pill->next    = box->pills_head;
                        box->pills_head = pill;
                    }
                }
            }

            /* Record clickable inline <a href="..."> span */
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
                        &tree->arena, sizeof(ArcrInlineLinkSpan), sizeof(void *));
                    if (ls) {
                        ls->rect      = (ArcrRect){start_x + pen_x, ly, it->adv_px, lh};
                        ls->link_node = it->link_node;
                        ls->href      = it->link_href;
                        ls->next      = box->links_head;
                        box->links_head = ls;
                    }
                }
            }

            if (it->cp != 0x200Bu) {
                out_g[gcount++] = (ArcrPositionedGlyph){
                    .codepoint  = (it->cp == 0x00A0u) ? (uint32_t)' ' : it->cp,
                    .x          = start_x + pen_x,
                    .baseline_y = baseline_y,
                    .advance_px = it->adv_px,
                    .font_px    = it->style->font_px,
                    .bold       = it->style->font_bold,
                    .monospace  = it->style->font_monospace,
                    .underline  = it->style->underline,
                    .color_argb = it->style->color_argb
                };
            }
            pen_x += it->adv_px;
        }
        i = cend;
    }

    if (pen_x > 0 || line_box_cnt > 0u || gcount > line_g_start) {
        align_finished_line(box, pen_x, max_width, out_g, line_g_start, gcount,
                            line_pill_mark, line_link_mark, line_boxes, line_box_cnt);
        cur_y += cur_line_h;
    }

    append_glyphs_to_box(tree, box, out_g, gcount);
    return cur_y - start_y;
}

/* Emits single-line text glyphs (for <input>, <button>, <ol> numbers, <img> alt) */
static void emit_single_line_text(ArcrLayoutTree *tree, ArcrFontSystem *fonts,
                                  ArcrLayoutBox *box, const char *utf8, size_t len,
                                  int32_t x, int32_t baseline_y, int32_t max_w,
                                  uint32_t color_argb, bool mask_password,
                                  bool center_in_max_w, bool keep_tail_on_overflow) {
    if (!utf8 || len == 0u || max_w <= 0) return;

    ArcrPositionedGlyph tmp[256];
    uint32_t cnt   = 0u;
    size_t   pos   = 0u;
    int32_t  total = 0;

    while (pos < len && cnt < ARCR_ARRAY_LEN(tmp)) {
        uint32_t cp = 0u;
        size_t step = arcr_utf8_decode_one(utf8 + pos, len - pos, &cp);
        if (step == 0u) break;
        pos += step;
        if (cp == '\r' || cp == '\n') cp = ' ';
        if (mask_password) cp = 0x2022u; /* '•' */

        int32_t adv = measure_codepoint_adv(fonts, cp, box->style->font_px,
                                            box->style->font_bold, box->style->font_monospace);
        tmp[cnt++] = (ArcrPositionedGlyph){
            .codepoint  = cp,
            .x          = total,
            .baseline_y = baseline_y,
            .advance_px = adv,
            .font_px    = box->style->font_px,
            .bold       = box->style->font_bold,
            .monospace  = box->style->font_monospace,
            .underline  = box->style->underline,
            .color_argb = color_argb
        };
        total += adv;
    }
    if (cnt == 0u) return;

    uint32_t start_idx = 0u;
    int32_t  shift_x   = x;
    if (total > max_w) {
        if (keep_tail_on_overflow) {
            /* Scroll horizontally so newly typed tail characters stay visible */
            while (start_idx + 1u < cnt && (total - tmp[start_idx].x) > max_w) {
                ++start_idx;
            }
            shift_x = x - tmp[start_idx].x;
        } else {
            /* Clip overflowing glyphs on right */
            uint32_t fit = 0u;
            while (fit < cnt && tmp[fit].x + tmp[fit].advance_px <= max_w) ++fit;
            cnt = fit;
        }
    } else if (center_in_max_w) {
        shift_x = x + (max_w - total) / 2;
    }

    if (start_idx >= cnt) return;
    for (uint32_t k = start_idx; k < cnt; ++k) {
        tmp[k].x += shift_x;
    }
    append_glyphs_to_box(tree, box, tmp + start_idx, cnt - start_idx);
}

/* -------------------------------------------------------------------------
 * Flexbox Layout (row + wrap + grow + justify + align, and column)
 * ------------------------------------------------------------------------- */
#define ARCR_MAX_FLEX_ITEMS 128u

static int32_t layout_flex_children(ArcrLayoutTree *tree, const ArcrDocument *doc,
                                    ArcrFontSystem *fonts, const ArcrDomNode *container,
                                    ArcrLayoutBox *box, int32_t cx, int32_t cy, int32_t cw) {
    const ArcrComputedStyle *st = box->style;
    int32_t gap = ARCR_MAX(0, st->gap_px);

    if (st->flex_direction == ARCR_FLEX_COLUMN) {
        int32_t cursor_y = cy;
        bool    first    = true;
        for (const ArcrDomNode *ch = container->first_child; ch != NULL; ch = ch->next_sibling) {
            if (ch->kind != ARCR_NODE_ELEMENT || !ch->computed_style ||
                ch->computed_style->display == ARCR_DISPLAY_NONE) {
                continue;
            }
            if (!first) cursor_y += gap;
            bool shrink = (st->align_items != ARCR_ALIGN_STRETCH) &&
                          (ch->computed_style->width_spec == 0);
            ArcrLayoutBox *ib = layout_element_node(tree, doc, fonts, ch, cx, cursor_y, cw,
                                                    shrink, 0, 0);
            if (!ib) continue;

            int32_t item_outer_w = ib->style->margin.left + ib->border_rect.width +
                                   ib->style->margin.right;
            if (item_outer_w < cw) {
                int32_t dx = 0;
                if (st->align_items == ARCR_ALIGN_CENTER) dx = (cw - item_outer_w) / 2;
                else if (st->align_items == ARCR_ALIGN_END) dx = cw - item_outer_w;
                if (dx > 0) offset_box_recursive(ib, dx, 0);
            }
            append_child_box(box, ib);
            cursor_y = ib->border_rect.y + ib->border_rect.height + ib->style->margin.bottom;
            first    = false;
        }
        return cursor_y - cy;
    }

    /* Row Flexbox */
    const ArcrDomNode *items[ARCR_MAX_FLEX_ITEMS];
    int32_t            base_w[ARCR_MAX_FLEX_ITEMS];
    int32_t            margins_h[ARCR_MAX_FLEX_ITEMS];
    size_t             icount = 0u;

    for (const ArcrDomNode *ch = container->first_child;
         ch != NULL && icount < ARCR_MAX_FLEX_ITEMS; ch = ch->next_sibling) {
        if (ch->kind != ARCR_NODE_ELEMENT || !ch->computed_style ||
            ch->computed_style->display == ARCR_DISPLAY_NONE) {
            continue;
        }
        items[icount]     = ch;
        margins_h[icount] = zpx(ch->computed_style->margin.left +
                                ch->computed_style->margin.right, tree->zoom_percent);
        base_w[icount]    = measure_natural_border_w(doc, fonts, ch, tree->zoom_percent, cw);
        icount++;
    }
    if (icount == 0u) return 0;

    int32_t cur_y      = cy;
    size_t  line_start = 0u;

    while (line_start < icount) {
        size_t  line_end = line_start + 1u;
        int32_t used_w   = base_w[line_start] + margins_h[line_start];

        while (line_end < icount) {
            int32_t next_w = base_w[line_end] + margins_h[line_end];
            if (st->flex_wrap && used_w + gap + next_w > cw) {
                break;
            }
            used_w += gap + next_w;
            line_end++;
        }

        size_t  lcnt       = line_end - line_start;
        int32_t total_gaps = (int32_t)(lcnt - 1u) * gap;
        int32_t total_grow = 0;
        int32_t sum_base   = 0;
        int32_t sum_marg   = 0;

        for (size_t k = line_start; k < line_end; ++k) {
            total_grow += items[k]->computed_style->flex_grow;
            sum_base   += base_w[k];
            sum_marg   += margins_h[k];
        }

        int32_t avail_for_boxes = cw - total_gaps - sum_marg;
        int32_t free_space      = avail_for_boxes - sum_base;

        ArcrLayoutBox *lboxes[ARCR_MAX_FLEX_ITEMS];
        int32_t        line_h       = 0;
        int32_t        actual_used  = total_gaps + sum_marg;

        for (size_t k = line_start; k < line_end; ++k) {
            int32_t target_w = base_w[k];
            int32_t fg       = items[k]->computed_style->flex_grow;
            if (free_space > 0 && total_grow > 0 && fg > 0) {
                target_w += (free_space * fg) / total_grow;
            } else if (free_space < 0 && sum_base > 0) {
                /* Proportional flex-shrink so row items never overflow container */
                target_w = ARCR_MAX(20, (base_w[k] * ARCR_MAX(20 * (int32_t)lcnt, avail_for_boxes)) /
                                            sum_base);
            }
            target_w = apply_min_max_w(
                target_w,
                zoom_style_if_needed(&tree->arena, items[k]->computed_style, tree->zoom_percent),
                cw);

            lboxes[k - line_start] = layout_element_node(
                tree, doc, fonts, items[k], 0, 0, target_w, false, target_w, 0);
            if (lboxes[k - line_start]) {
                ArcrLayoutBox *ib = lboxes[k - line_start];
                actual_used += ib->border_rect.width;
                int32_t oh = ib->style->margin.top + ib->border_rect.height +
                             ib->style->margin.bottom;
                if (oh > line_h) line_h = oh;
            }
        }

        int32_t rem_w   = ARCR_MAX(0, cw - actual_used);
        int32_t pen_x   = cx;
        int32_t eff_gap = gap;

        if (st->justify_content == ARCR_JUSTIFY_CENTER) {
            pen_x += rem_w / 2;
        } else if (st->justify_content == ARCR_JUSTIFY_END) {
            pen_x += rem_w;
        } else if (st->justify_content == ARCR_JUSTIFY_SPACE_BETWEEN && lcnt > 1u) {
            eff_gap = gap + rem_w / (int32_t)(lcnt - 1u);
        } else if (st->justify_content == ARCR_JUSTIFY_SPACE_AROUND && lcnt > 0u) {
            int32_t pad = rem_w / (int32_t)(lcnt * 2u);
            pen_x   += pad;
            eff_gap  = gap + pad * 2;
        }

        for (size_t idx = 0u; idx < lcnt; ++idx) {
            ArcrLayoutBox *ib = lboxes[idx];
            if (!ib) continue;

            int32_t item_x = pen_x + ib->style->margin.left;
            int32_t item_y = cur_y + ib->style->margin.top;
            int32_t oh     = ib->style->margin.top + ib->border_rect.height +
                             ib->style->margin.bottom;

            if (st->align_items == ARCR_ALIGN_STRETCH && ib->style->height_px == 0) {
                int32_t stretch_bh = line_h - ib->style->margin.top - ib->style->margin.bottom;
                if (stretch_bh > ib->border_rect.height) {
                    int32_t dh = stretch_bh - ib->border_rect.height;
                    ib->border_rect.height  += dh;
                    ib->content_rect.height += dh;
                }
            } else if (st->align_items == ARCR_ALIGN_CENTER && oh < line_h) {
                item_y += (line_h - oh) / 2;
            } else if (st->align_items == ARCR_ALIGN_END && oh < line_h) {
                item_y += (line_h - oh);
            }

            offset_box_recursive(ib, item_x - ib->border_rect.x, item_y - ib->border_rect.y);
            append_child_box(box, ib);
            pen_x += ib->style->margin.left + ib->border_rect.width +
                     ib->style->margin.right + eff_gap;
        }

        cur_y += line_h;
        if (line_end < icount) cur_y += gap;
        line_start = line_end;
    }

    return cur_y - cy;
}

/* -------------------------------------------------------------------------
 * CSS Grid Layout (Explicit & Auto-Fit Multi-Column Uniform Row Grid)
 * ------------------------------------------------------------------------- */
static int32_t layout_grid_children(ArcrLayoutTree *tree, const ArcrDocument *doc,
                                    ArcrFontSystem *fonts, const ArcrDomNode *container,
                                    ArcrLayoutBox *box, int32_t cx, int32_t cy, int32_t cw) {
    const ArcrComputedStyle *st = box->style;
    int32_t gap  = ARCR_MAX(0, st->gap_px);
    int32_t cols = (st->grid_columns > 0)
                       ? st->grid_columns
                       : ARCR_CLAMP(cw / zpx(240, tree->zoom_percent), 1, 4);
    int32_t col_w = ARCR_MAX(24, (cw - (cols - 1) * gap) / cols);

    const ArcrDomNode *items[256];
    size_t icount = 0u;
    for (const ArcrDomNode *ch = container->first_child;
         ch != NULL && icount < ARCR_ARRAY_LEN(items); ch = ch->next_sibling) {
        if (ch->kind == ARCR_NODE_ELEMENT && ch->computed_style &&
            ch->computed_style->display != ARCR_DISPLAY_NONE) {
            items[icount++] = ch;
        }
    }
    if (icount == 0u) return 0;

    int32_t cur_y = cy;
    for (size_t i = 0u; i < icount; i += (size_t)cols) {
        if (i > 0u) cur_y += gap;
        ArcrLayoutBox *row_boxes[12];
        int32_t        row_h = 0;
        size_t         rcnt  = ARCR_MIN((size_t)cols, icount - i);

        for (size_t c = 0u; c < rcnt; ++c) {
            int32_t cell_x = cx + (int32_t)c * (col_w + gap);
            int32_t mh     = zpx(items[i + c]->computed_style->margin.left +
                                 items[i + c]->computed_style->margin.right, tree->zoom_percent);
            int32_t item_bw = ARCR_MAX(16, col_w - mh);
            ArcrLayoutBox *ib = layout_element_node(
                tree, doc, fonts, items[i + c], cell_x, cur_y, col_w, false, item_bw, 0);
            row_boxes[c] = ib;
            if (ib) {
                int32_t oh = ib->style->margin.top + ib->border_rect.height +
                             ib->style->margin.bottom;
                if (oh > row_h) row_h = oh;
            }
        }

        for (size_t c = 0u; c < rcnt; ++c) {
            ArcrLayoutBox *ib = row_boxes[c];
            if (!ib) continue;
            if (st->align_items == ARCR_ALIGN_STRETCH && ib->style->height_px == 0) {
                int32_t stretch_bh = row_h - ib->style->margin.top - ib->style->margin.bottom;
                if (stretch_bh > ib->border_rect.height) {
                    int32_t dh = stretch_bh - ib->border_rect.height;
                    ib->border_rect.height  += dh;
                    ib->content_rect.height += dh;
                }
            }
            append_child_box(box, ib);
        }
        cur_y += row_h;
    }
    return cur_y - cy;
}

/* -------------------------------------------------------------------------
 * HTML Table Layout (<thead>/<tbody>/<tfoot>, Auto Column Sizing & Cell Grid)
 * ------------------------------------------------------------------------- */
#define ARCR_MAX_TABLE_ROWS 128u
#define ARCR_MAX_TABLE_COLS 24u

static void collect_table_rows(const ArcrAtomTable *atoms, const ArcrDomNode *parent,
                               const ArcrDomNode **out_rows, size_t *inout_rcnt) {
    for (const ArcrDomNode *ch = parent->first_child; ch != NULL; ch = ch->next_sibling) {
        if (ch->kind != ARCR_NODE_ELEMENT) continue;
        if (ch->computed_style && ch->computed_style->display == ARCR_DISPLAY_NONE) continue;
        const char *tn = arcr_atom_cstr(atoms, ch->tag);
        if (arcr_str_eq_nocase(tn, "tr") ||
            (ch->computed_style && ch->computed_style->display == ARCR_DISPLAY_TABLE_ROW)) {
            if (*inout_rcnt < ARCR_MAX_TABLE_ROWS) {
                out_rows[(*inout_rcnt)++] = ch;
            }
        } else if (arcr_str_eq_nocase(tn, "thead") || arcr_str_eq_nocase(tn, "tbody") ||
                   arcr_str_eq_nocase(tn, "tfoot")) {
            collect_table_rows(atoms, ch, out_rows, inout_rcnt);
        }
    }
}

static bool is_table_cell_el(const ArcrAtomTable *atoms, const ArcrDomNode *n) {
    if (!n || n->kind != ARCR_NODE_ELEMENT) return false;
    if (n->computed_style && n->computed_style->display == ARCR_DISPLAY_NONE) return false;
    const char *tn = arcr_atom_cstr(atoms, n->tag);
    if (arcr_str_eq_nocase(tn, "td") || arcr_str_eq_nocase(tn, "th")) return true;
    return n->computed_style && n->computed_style->display == ARCR_DISPLAY_TABLE_CELL;
}

static int32_t layout_table_children(ArcrLayoutTree *tree, const ArcrDocument *doc,
                                     ArcrFontSystem *fonts, const ArcrDomNode *table_node,
                                     ArcrLayoutBox *table_box, int32_t cx, int32_t cy,
                                     int32_t cw, int32_t *out_used_table_w) {
    const ArcrDomNode *rows[ARCR_MAX_TABLE_ROWS];
    size_t row_cnt = 0u;
    collect_table_rows(doc->atoms, table_node, rows, &row_cnt);
    if (row_cnt == 0u) {
        if (out_used_table_w) *out_used_table_w = cw;
        return 0;
    }

    int32_t col_pref[ARCR_MAX_TABLE_COLS];
    memset(col_pref, 0, sizeof(col_pref));
    size_t max_cols = 0u;

    for (size_t r = 0u; r < row_cnt; ++r) {
        size_t c = 0u;
        for (const ArcrDomNode *cell = rows[r]->first_child;
             cell != NULL && c < ARCR_MAX_TABLE_COLS; cell = cell->next_sibling) {
            if (!is_table_cell_el(doc->atoms, cell)) continue;
            int32_t span = 1;
            const char *cs_s = arcr_dom_get_attr_cstr(doc, cell, "colspan");
            if (cs_s && arcr_parse_i32(cs_s, strlen(cs_s), &span)) {
                span = ARCR_CLAMP(span, 1, (int32_t)(ARCR_MAX_TABLE_COLS - c));
            }
            int32_t nw   = measure_natural_border_w(doc, fonts, cell, tree->zoom_percent, cw);
            int32_t each = ARCR_MAX(zpx(40, tree->zoom_percent), nw / span);
            for (int32_t s = 0; s < span && c + (size_t)s < ARCR_MAX_TABLE_COLS; ++s) {
                if (each > col_pref[c + (size_t)s]) col_pref[c + (size_t)s] = each;
            }
            c += (size_t)span;
        }
        if (c > max_cols) max_cols = c;
    }
    if (max_cols == 0u) {
        if (out_used_table_w) *out_used_table_w = cw;
        return 0;
    }

    int32_t sum_pref = 0;
    for (size_t c = 0u; c < max_cols; ++c) {
        if (col_pref[c] < zpx(36, tree->zoom_percent)) col_pref[c] = zpx(36, tree->zoom_percent);
        sum_pref += col_pref[c];
    }

    bool fill_full = (table_box->style->width_spec != 0) || (sum_pref > cw);
    int32_t target_table_w = fill_full ? cw : ARCR_MIN(cw, ARCR_MAX(sum_pref, cw / 2));
    int32_t col_w[ARCR_MAX_TABLE_COLS];
    int32_t assigned = 0;

    for (size_t c = 0u; c < max_cols; ++c) {
        if (c + 1u == max_cols) {
            col_w[c] = ARCR_MAX(zpx(24, tree->zoom_percent), target_table_w - assigned);
        } else {
            col_w[c] = ARCR_MAX(zpx(24, tree->zoom_percent),
                                (target_table_w * col_pref[c]) / ARCR_MAX(1, sum_pref));
            assigned += col_w[c];
        }
    }
    int32_t final_tw = 0;
    for (size_t c = 0u; c < max_cols; ++c) final_tw += col_w[c];
    if (out_used_table_w) *out_used_table_w = final_tw;

    int32_t cur_y = cy;
    for (size_t r = 0u; r < row_cnt; ++r) {
        const ArcrDomNode *tr_node = rows[r];
        const ArcrComputedStyle *tr_st = zoom_style_if_needed(
            &tree->arena, tr_node->computed_style, tree->zoom_percent);

        ArcrLayoutBox *tr_box = (ArcrLayoutBox *)arcr_arena_calloc(
            &tree->arena, 1u, sizeof(ArcrLayoutBox), sizeof(void *));
        if (!tr_box) continue;
        tree->box_count++;
        tr_box->dom_node = tr_node;
        tr_box->style    = tr_st;

        ArcrLayoutBox *cell_boxes[ARCR_MAX_TABLE_COLS];
        size_t         cell_cnt = 0u;
        size_t         col_idx  = 0u;
        int32_t        pen_x    = cx;
        int32_t        row_h    = zpx(24, tree->zoom_percent);

        for (const ArcrDomNode *cell = tr_node->first_child;
             cell != NULL && col_idx < max_cols; cell = cell->next_sibling) {
            if (!is_table_cell_el(doc->atoms, cell)) continue;
            int32_t span = 1;
            const char *cs_s = arcr_dom_get_attr_cstr(doc, cell, "colspan");
            if (cs_s && arcr_parse_i32(cs_s, strlen(cs_s), &span)) {
                span = ARCR_CLAMP(span, 1, (int32_t)(max_cols - col_idx));
            }
            int32_t cw_span = 0;
            for (int32_t s = 0; s < span && col_idx + (size_t)s < max_cols; ++s) {
                cw_span += col_w[col_idx + (size_t)s];
            }

            ArcrLayoutBox *cb = layout_element_node(
                tree, doc, fonts, cell, pen_x, cur_y, cw_span, false, cw_span, 0);
            if (cb) {
                /* Keep cell flush in row grid regardless of stray margins */
                offset_box_recursive(cb, pen_x - cb->border_rect.x, cur_y - cb->border_rect.y);
                if (cb->border_rect.height > row_h) row_h = cb->border_rect.height;
                cell_boxes[cell_cnt++] = cb;
            }
            pen_x   += cw_span;
            col_idx += (size_t)span;
        }

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
        cur_y += row_h;
    }

    return cur_y - cy;
}

/* -------------------------------------------------------------------------
 * Master Element Box Layout (Block, Controls, Images, Flex, Grid, Table)
 * ------------------------------------------------------------------------- */
static ArcrLayoutBox *layout_element_node(ArcrLayoutTree *tree, const ArcrDocument *doc,
                                          ArcrFontSystem *fonts, const ArcrDomNode *node,
                                          int32_t avail_x, int32_t start_y, int32_t avail_w,
                                          bool shrink_to_fit, int32_t forced_border_w,
                                          int32_t list_ordinal) {
    if (!node || node->kind != ARCR_NODE_ELEMENT || !node->computed_style) return NULL;
    if (node->computed_style->display == ARCR_DISPLAY_NONE) return NULL;

    const ArcrComputedStyle *st = zoom_style_if_needed(
        &tree->arena, node->computed_style, tree->zoom_percent);

    ArcrLayoutBox *box = (ArcrLayoutBox *)arcr_arena_calloc(
        &tree->arena, 1u, sizeof(ArcrLayoutBox), sizeof(void *));
    if (!box) return NULL;
    tree->box_count++;

    box->dom_node     = node;
    box->style        = st;
    const char *tname = arcr_atom_cstr(doc->atoms, node->tag);
    box->is_list_item = arcr_str_eq_nocase(tname, "li");
    box->list_ordinal = list_ordinal;
    box->is_hr        = arcr_str_eq_nocase(tname, "hr");

    bool is_img      = arcr_str_eq_nocase(tname, "img");
    bool is_canvas   = arcr_str_eq_nocase(tname, "canvas");
    bool is_input    = arcr_str_eq_nocase(tname, "input");
    bool is_textarea = arcr_str_eq_nocase(tname, "textarea");
    bool is_button   = arcr_str_eq_nocase(tname, "button");

    int32_t h_chrome = st->border.left + st->border.right + st->padding.left + st->padding.right;
    int32_t v_chrome = st->border.top + st->border.bottom + st->padding.top + st->padding.bottom;

    /* 1. Determine Border-Box Width (bw) & Horizontal Position (bx) */
    int32_t bw = 0;
    int32_t intrinsic_img_h = 0;

    if (is_img || is_canvas) {
        int32_t nw = is_canvas ? zpx(300, tree->zoom_percent) : zpx(120, tree->zoom_percent);
        int32_t nh = is_canvas ? zpx(150, tree->zoom_percent) : zpx(80, tree->zoom_percent);
        if (node->surface && node->surface->width > 0 && node->surface->height > 0) {
            nw = zpx(node->surface->width, tree->zoom_percent);
            nh = zpx(node->surface->height, tree->zoom_percent);
        } else {
            const char *wa = arcr_dom_get_attr_cstr(doc, node, "width");
            const char *ha = arcr_dom_get_attr_cstr(doc, node, "height");
            int32_t pw = 0, ph = 0;
            if (wa && arcr_parse_i32(wa, strlen(wa), &pw) && pw > 0) nw = zpx(pw, tree->zoom_percent);
            if (ha && arcr_parse_i32(ha, strlen(ha), &ph) && ph > 0) nh = zpx(ph, tree->zoom_percent);
        }
        int32_t css_w = resolve_spec_px(st->width_spec, avail_w);
        int32_t css_h = st->height_px;
        int32_t img_cw = nw;
        int32_t img_ch = nh;
        if (css_w > 0 && css_h > 0) {
            img_cw = ARCR_MAX(1, css_w - h_chrome);
            img_ch = ARCR_MAX(1, css_h - v_chrome);
        } else if (css_w > 0) {
            img_cw = ARCR_MAX(1, css_w - h_chrome);
            if (nw > 0) img_ch = ARCR_MAX(1, (img_cw * nh) / nw);
        } else if (css_h > 0) {
            img_ch = ARCR_MAX(1, css_h - v_chrome);
            if (nh > 0) img_cw = ARCR_MAX(1, (img_ch * nw) / nh);
        }
        if (img_cw + h_chrome > avail_w && avail_w > h_chrome) {
            int32_t clamped_cw = avail_w - h_chrome;
            if (css_h == 0 && img_cw > 0) {
                img_ch = ARCR_MAX(1, (clamped_cw * img_ch) / img_cw);
            }
            img_cw = clamped_cw;
        }
        bw              = img_cw + h_chrome;
        intrinsic_img_h = img_ch;
    } else if (forced_border_w > 0) {
        bw = forced_border_w;
    } else if (st->width_spec != 0) {
        bw = resolve_spec_px(st->width_spec, avail_w);
    } else if (shrink_to_fit || is_input || is_textarea || is_button ||
               st->display == ARCR_DISPLAY_INLINE_BLOCK) {
        bw = measure_natural_border_w(doc, fonts, node, tree->zoom_percent,
                                      ARCR_MAX(24, avail_w - st->margin.left - st->margin.right));
    } else {
        bw = ARCR_MAX(24, avail_w - st->margin.left - st->margin.right);
    }

    bw = apply_min_max_w(bw, st, avail_w);
    bw = ARCR_MAX(ARCR_MAX(8, h_chrome), bw);

    int32_t bx = avail_x + st->margin.left;
    if (forced_border_w == 0 && !shrink_to_fit) {
        if (st->margin_left_auto && st->margin_right_auto && bw < avail_w) {
            bx = avail_x + (avail_w - bw) / 2;
        } else if (st->margin_left_auto && !st->margin_right_auto &&
                   bw + st->margin.right < avail_w) {
            bx = avail_x + avail_w - st->margin.right - bw;
        }
    }
    int32_t by = start_y + st->margin.top;
    int32_t cx = bx + st->border.left + st->padding.left;
    int32_t cy = by + st->border.top + st->padding.top;
    int32_t cw = ARCR_MAX(4, bw - h_chrome);

    /* 2. Lay out interior content by element / display mode */
    int32_t content_h = 0;

    if (box->is_hr) {
        content_h = zpx(2, tree->zoom_percent);
    } else if (is_img || is_canvas) {
        content_h = intrinsic_img_h;
        if (is_img && (!node->surface || !node->surface->argb_pixels)) {
            const char *alt = arcr_dom_get_attr_cstr(doc, node, "alt");
            if (alt && alt[0] != '\0' && cw > 16 && content_h >= st->font_px) {
                int32_t base_y = cy + content_h / 2 + st->font_px / 3;
                emit_single_line_text(tree, fonts, box, alt, strlen(alt),
                                      cx + 6, base_y, cw - 12,
                                      0xFF64748Bu, false, true, false);
            }
        }
    } else if (is_input) {
        const char *tp = arcr_dom_get_attr_cstr(doc, node, "type");
        if (tp && (arcr_str_eq_nocase(tp, "checkbox") || arcr_str_eq_nocase(tp, "radio"))) {
            box->control_kind = ARCR_CTRL_CHECKBOX;
            if (st->width_spec == 0) bw = zpx(18, tree->zoom_percent);
            cw        = ARCR_MAX(4, bw - h_chrome);
            content_h = (st->height_px > 0)
                            ? ARCR_MAX(4, st->height_px - v_chrome)
                            : ARCR_MAX(4, zpx(18, tree->zoom_percent) - v_chrome);
        } else if (tp && (arcr_str_eq_nocase(tp, "submit") || arcr_str_eq_nocase(tp, "button") ||
                          arcr_str_eq_nocase(tp, "reset"))) {
            box->control_kind = ARCR_CTRL_BUTTON;
            const char *lbl = arcr_dom_get_attr_cstr(doc, node, "value");
            if (!lbl || lbl[0] == '\0') lbl = "Submit";
            content_h = (st->height_px > 0)
                            ? ARCR_MAX(st->font_px, st->height_px - v_chrome)
                            : ((st->font_px * 135) / 100);
            int32_t base_y = cy + (content_h * 78) / 100;
            emit_single_line_text(tree, fonts, box, lbl, strlen(lbl),
                                  cx, base_y, cw, st->color_argb, false, true, false);
        } else {
            box->control_kind = ARCR_CTRL_TEXT_INPUT;
            content_h = (st->height_px > 0)
                            ? ARCR_MAX(st->font_px, st->height_px - v_chrome)
                            : ((st->font_px * 135) / 100);
            int32_t base_y     = cy + (content_h * 78) / 100;
            const char *live   = arcr_dom_get_input_value(doc, node);
            bool        is_pw  = (tp && arcr_str_eq_nocase(tp, "password"));
            bool        has_focus = ((node->state_flags & ARCR_STATE_FOCUS) != 0u);
            if (live && live[0] != '\0') {
                emit_single_line_text(tree, fonts, box, live, strlen(live),
                                      cx, base_y, cw, st->color_argb, is_pw, false, has_focus);
            } else {
                const char *ph = arcr_dom_get_attr_cstr(doc, node, "placeholder");
                if (ph && ph[0] != '\0') {
                    emit_single_line_text(tree, fonts, box, ph, strlen(ph),
                                          cx, base_y, cw, 0xFF94A3B8u, false, false, false);
                }
            }
        }
    } else if (is_textarea) {
        box->control_kind = ARCR_CTRL_TEXT_INPUT;
        int32_t rows = 4;
        const char *rs = arcr_dom_get_attr_cstr(doc, node, "rows");
        if (rs && arcr_parse_i32(rs, strlen(rs), &rows)) rows = ARCR_CLAMP(rows, 1, 32);
        int32_t def_h = rows * ((st->font_px * 145) / 100);

        const char *live = arcr_dom_get_input_value(doc, node);
        const char *src  = (live && live[0] != '\0')
                               ? live
                               : arcr_dom_get_attr_cstr(doc, node, "placeholder");
        bool is_ph = (!live || live[0] == '\0');
        if (src && src[0] != '\0') {
            ArcrComputedStyle *txt_st = (ArcrComputedStyle *)arcr_arena_alloc(
                &tree->arena, sizeof(ArcrComputedStyle), sizeof(void *));
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
                collect_inline_items(tree, doc, fonts, &tmp_tn, txt_st, NULL, NULL, cw, &ibuf);
                if (ibuf.count > 0u) {
                    content_h = layout_inline_run(tree, box, &ibuf, cx, cy, cw);
                }
                free(ibuf.items);
            }
        }
        if (content_h < def_h) content_h = def_h;
    } else if (st->display == ARCR_DISPLAY_FLEX) {
        content_h = layout_flex_children(tree, doc, fonts, node, box, cx, cy, cw);
    } else if (st->display == ARCR_DISPLAY_GRID) {
        content_h = layout_grid_children(tree, doc, fonts, node, box, cx, cy, cw);
    } else if (st->display == ARCR_DISPLAY_TABLE) {
        int32_t used_tw = cw;
        content_h = layout_table_children(tree, doc, fonts, node, box, cx, cy, cw, &used_tw);
        if (forced_border_w == 0 && st->width_spec == 0) {
            cw = used_tw;
            bw = cw + h_chrome;
        }
    } else {
        if (is_button) box->control_kind = ARCR_CTRL_BUTTON;

        bool is_ol = arcr_str_eq_nocase(tname, "ol");
        int32_t next_li_ord = 1;
        if (is_ol) {
            const char *start_attr = arcr_dom_get_attr_cstr(doc, node, "start");
            if (start_attr) {
                int32_t sv = 1;
                if (arcr_parse_i32(start_attr, strlen(start_attr), &sv) && sv >= 0) {
                    next_li_ord = sv;
                }
            }
        }

        int32_t cursor_y = cy;
        const ArcrDomNode *ch = node->first_child;
        while (ch) {
            if (is_inline_flow_child(doc->atoms, ch)) {
                InlineBuf ibuf = {NULL, 0u, 0u};
                while (ch && is_inline_flow_child(doc->atoms, ch)) {
                    collect_inline_items(tree, doc, fonts, ch, st, NULL, NULL, cw, &ibuf);
                    ch = ch->next_sibling;
                }
                if (ibuf.count > 0u) {
                    cursor_y += layout_inline_run(tree, box, &ibuf, cx, cursor_y, cw);
                }
                free(ibuf.items);
            } else {
                int32_t ord = 0;
                if (ch->kind == ARCR_NODE_ELEMENT &&
                    arcr_str_eq_nocase(arcr_atom_cstr(doc->atoms, ch->tag), "li")) {
                    ord = is_ol ? (next_li_ord++) : 0;
                }
                ArcrLayoutBox *cbox = layout_element_node(
                    tree, doc, fonts, ch, cx, cursor_y, cw, false, 0, ord);
                if (cbox) {
                    append_child_box(box, cbox);
                    cursor_y = cbox->border_rect.y + cbox->border_rect.height +
                               cbox->style->margin.bottom;
                }
                ch = ch->next_sibling;
            }
        }
        content_h = cursor_y - cy;
    }

    /* 3. Ordered list marker ("1.", "2.", ...) positioned in left gutter */
    if (box->is_list_item && box->list_ordinal > 0) {
        char num_buf[24];
        int nlen = snprintf(num_buf, sizeof(num_buf), "%d.", (int)box->list_ordinal);
        if (nlen > 0) {
            ArcrLayoutBox *mk = (ArcrLayoutBox *)arcr_arena_calloc(
                &tree->arena, 1u, sizeof(ArcrLayoutBox), sizeof(void *));
            if (mk) {
                mk->dom_node = NULL;
                mk->style    = st;
                int32_t mw = measure_utf8_string_px(
                    fonts, num_buf, (size_t)nlen, st->font_px, st->font_bold, st->font_monospace);
                int32_t mx = cx - mw - zpx(6, tree->zoom_percent);
                int32_t lh = ARCR_MAX(16, (st->font_px * ARCR_MAX(100, st->line_height_pct)) / 100);
                int32_t base_y = cy + (lh * 78) / 100;
                mk->content_rect = (ArcrRect){mx, cy, mw + 4, lh};
                mk->border_rect  = mk->content_rect;
                emit_single_line_text(tree, fonts, mk, num_buf, (size_t)nlen,
                                      mx, base_y, mw + 4, st->color_argb, false, false, false);
                append_child_box(box, mk);
            }
        }
    }

    /* 4. Apply explicit CSS height / min-height */
    int32_t bh = v_chrome + content_h;
    if (st->height_px > 0 && !is_img && !is_canvas) {
        bh        = ARCR_MAX(v_chrome, st->height_px);
        content_h = bh - v_chrome;
    }
    if (st->min_height_px > 0 && bh < st->min_height_px) {
        bh        = st->min_height_px;
        content_h = ARCR_MAX(0, bh - v_chrome);
    }

    box->content_rect = (ArcrRect){cx, cy, cw, content_h};
    box->border_rect  = (ArcrRect){bx, by, bw, bh};
    return box;
}

ArcrLayoutTree *arcr_layout_build_zoomed(const ArcrDocument *doc, ArcrFontSystem *fonts,
                                         int32_t viewport_width, int32_t zoom_percent) {
    if (!doc || !doc->body) return NULL;
    ArcrLayoutTree *tree = (ArcrLayoutTree *)calloc(1u, sizeof(ArcrLayoutTree));
    if (!tree) return NULL;

    arcr_arena_init(&tree->arena, 32768u);
    tree->viewport_width = (viewport_width > 64) ? viewport_width : 64;
    tree->zoom_percent   = (zoom_percent <= 0) ? 100 : ARCR_CLAMP(zoom_percent, 50, 200);

    tree->root_box = layout_element_node(tree, doc, fonts, doc->body,
                                         0, 0, tree->viewport_width, false, 0, 0);
    if (tree->root_box) {
        tree->total_height = tree->root_box->border_rect.y +
                             tree->root_box->border_rect.height +
                             tree->root_box->style->margin.bottom;
    }
    return tree;
}

ArcrLayoutTree *arcr_layout_build(const ArcrDocument *doc, ArcrFontSystem *fonts,
                                  int32_t viewport_width) {
    return arcr_layout_build_zoomed(doc, fonts, viewport_width, 100);
}

void arcr_layout_destroy(ArcrLayoutTree *tree) {
    if (!tree) return;
    arcr_arena_destroy(&tree->arena);
    free(tree);
}

/* -------------------------------------------------------------------------
 * Spatial Hit-Testing (Document Space -> Deepest Box, Links & Cursor Kind)
 * ------------------------------------------------------------------------- */
static bool point_in_rect(int32_t x, int32_t y, ArcrRect r) {
    return x >= r.x && x < (r.x + r.width) &&
           y >= r.y && y < (r.y + r.height);
}

static void hit_test_walk(const ArcrLayoutBox *box, int32_t doc_x, int32_t doc_y,
                          const ArcrLayoutBox **inout_best_box,
                          const ArcrInlineLinkSpan **inout_best_link) {
    if (!box) return;

    if (box->dom_node && point_in_rect(doc_x, doc_y, box->border_rect)) {
        *inout_best_box  = box;
        *inout_best_link = NULL;
    }

    for (const ArcrInlineLinkSpan *ls = box->links_head; ls != NULL; ls = ls->next) {
        if (point_in_rect(doc_x, doc_y, ls->rect)) {
            *inout_best_box  = box;
            *inout_best_link = ls;
        }
    }

    for (const ArcrLayoutBox *ch = box->first_child; ch != NULL; ch = ch->next_sibling) {
        hit_test_walk(ch, doc_x, doc_y, inout_best_box, inout_best_link);
    }
}

void arcr_layout_hit_test(const ArcrLayoutTree *tree, const ArcrDocument *doc,
                          int32_t doc_x, int32_t doc_y, ArcrHitTestResult *out_hit) {
    if (!out_hit) return;
    out_hit->hit_node           = NULL;
    out_hit->clickable_ancestor = NULL;
    out_hit->link_href          = NULL;
    out_hit->cursor             = ARCR_CURSOR_ARROW;

    if (!tree || !tree->root_box || !doc) return;

    const ArcrLayoutBox      *best_box  = NULL;
    const ArcrInlineLinkSpan *best_link = NULL;
    hit_test_walk(tree->root_box, doc_x, doc_y, &best_box, &best_link);

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
        best_box->control_kind == ARCR_CTRL_BUTTON) {
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
        if (arcr_str_eq_nocase(tn, "button")) {
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
        if (arcr_dom_get_attr_cstr(doc, anc, "onclick") != NULL) {
            if (!out_hit->clickable_ancestor) out_hit->clickable_ancestor = anc;
            out_hit->cursor = ARCR_CURSOR_POINTER;
        }
    }
}
