/*
 * Arcradient (MSHTML-Next)
 * layout/arcr_layout.c - Block Formatting & Korean 어절 / 금칙처리 Inline Wrapper
 */
#include "arcr_layout.h"
#include <stdlib.h>
#include <string.h>

typedef struct InlineItem {
    uint32_t                 cp;
    int32_t                  adv_px;
    const ArcrComputedStyle *style;
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

static bool is_inline_node(const ArcrAtomTable *atoms, const ArcrDomNode *n) {
    if (!n) return false;
    if (n->kind == ARCR_NODE_TEXT) return true;
    if (n->kind == ARCR_NODE_ELEMENT) {
        if (arcr_str_eq_nocase(arcr_atom_cstr(atoms, n->tag), "br")) return true;
        if (n->computed_style && n->computed_style->display == ARCR_DISPLAY_INLINE) {
            return true;
        }
    }
    return false;
}

static void collect_inline_items(const ArcrAtomTable *atoms, ArcrFontSystem *fonts,
                                 const ArcrDomNode *node, const ArcrComputedStyle *cur_style,
                                 InlineBuf *buf) {
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
                inline_buf_push(buf, (InlineItem){'\n', 0, cur_style, true});
                continue;
            }
            ArcrGlyphMetrics gm;
            arcr_font_measure_glyph(fonts, cp, cur_style->font_px, cur_style->font_bold, &gm);
            inline_buf_push(buf, (InlineItem){cp, gm.advance_px, cur_style, false});
        }
        return;
    }

    if (node->kind == ARCR_NODE_ELEMENT) {
        if (node->computed_style && node->computed_style->display == ARCR_DISPLAY_NONE) return;
        if (arcr_str_eq_nocase(arcr_atom_cstr(atoms, node->tag), "br")) {
            inline_buf_push(buf, (InlineItem){'\n', 0, cur_style, true});
            return;
        }
        const ArcrComputedStyle *st = node->computed_style ? node->computed_style : cur_style;
        /* Left inline padding (e.g. <code> pill) */
        if (st->padding.left > 0) {
            inline_buf_push(buf, (InlineItem){0x200Bu, st->padding.left, st, false});
        }
        for (const ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
            if (is_inline_node(atoms, ch)) {
                collect_inline_items(atoms, fonts, ch, st, buf);
            }
        }
        if (st->padding.right > 0) {
            inline_buf_push(buf, (InlineItem){0x200Bu, st->padding.right, st, false});
        }
    }
}

/* Wraps collected InlineItems into positioned glyphs inside `box` using Korean/CJK rules */
static int32_t layout_inline_run(ArcrLayoutTree *tree, ArcrLayoutBox *box,
                                 const InlineBuf *buf, int32_t start_x, int32_t start_y,
                                 int32_t max_width) {
    if (buf->count == 0u) return 0;
    if (max_width < 32) max_width = 32;

    ArcrPositionedGlyph *out_g = (ArcrPositionedGlyph *)arcr_arena_alloc(
        &tree->arena, buf->count * sizeof(ArcrPositionedGlyph), sizeof(void *));
    if (!out_g) return 0;

    uint32_t gcount = 0u;
    int32_t  pen_x  = 0;
    int32_t  cur_y  = start_y;
    int32_t  base_line_h = ARCR_MAX(18, (box->style->font_px * 145) / 100);
    int32_t  cur_line_h  = base_line_h;

    size_t i = 0u;
    while (i < buf->count) {
        /* Skip leading ASCII spaces at start of wrapped line */
        if (pen_x == 0 && buf->items[i].cp == ' ' && !buf->items[i].forced_break) {
            ++i;
            continue;
        }
        if (buf->items[i].forced_break) {
            cur_y += cur_line_h;
            pen_x      = 0;
            cur_line_h = base_line_h;
            ++i;
            continue;
        }

        /* Group an unbreakable cluster [i .. cluster_end) using arcr_unicode_can_break_line */
        size_t  cend        = i + 1u;
        int32_t cluster_w   = buf->items[i].adv_px;
        int32_t cluster_max_fpx = buf->items[i].style->font_px;

        while (cend < buf->count && !buf->items[cend].forced_break) {
            bool keep_all = buf->items[cend - 1u].style->word_break_keep_all ||
                            buf->items[cend].style->word_break_keep_all;
            if (arcr_unicode_can_break_line(buf->items[cend - 1u].cp,
                                            buf->items[cend].cp, keep_all)) {
                break;
            }
            cluster_w += buf->items[cend].adv_px;
            if (buf->items[cend].style->font_px > cluster_max_fpx) {
                cluster_max_fpx = buf->items[cend].style->font_px;
            }
            ++cend;
        }

        /* Wrap onto next line if cluster overflows current line */
        if (pen_x > 0 && pen_x + cluster_w > max_width) {
            cur_y += cur_line_h;
            pen_x      = 0;
            cur_line_h = base_line_h;
            if (buf->items[i].cp == ' ') {
                ++i;
                continue;
            }
        }

        int32_t needed_lh = ARCR_MAX(base_line_h, (cluster_max_fpx * 145) / 100);
        if (needed_lh > cur_line_h) cur_line_h = needed_lh;
        int32_t baseline_y = cur_y + (cur_line_h * 78) / 100;

        for (size_t k = i; k < cend; ++k) {
            const InlineItem *it = &buf->items[k];
            /* If a single huge unbroken token still exceeds max_width, emergency-wrap */
            if (pen_x > 0 && pen_x + it->adv_px > max_width) {
                cur_y += cur_line_h;
                pen_x      = 0;
                baseline_y = cur_y + (cur_line_h * 78) / 100;
            }

            /* Record inline background pill (e.g. <code>...</code>) */
            if (((it->style->bg_argb >> 24) & 0xFFu) > 0u &&
                it->style != box->style && it->adv_px > 0) {
                int32_t pill_y = baseline_y - it->style->font_px;
                int32_t pill_h = it->style->font_px + 5;
                if (box->pills_head &&
                    box->pills_head->bg_argb == it->style->bg_argb &&
                    box->pills_head->rect.y == pill_y &&
                    box->pills_head->rect.x + box->pills_head->rect.width == start_x + pen_x) {
                    box->pills_head->rect.width += it->adv_px;
                } else {
                    ArcrInlineBgPill *pill = (ArcrInlineBgPill *)arcr_arena_alloc(
                        &tree->arena, sizeof(ArcrInlineBgPill), sizeof(void *));
                    if (pill) {
                        pill->rect    = (ArcrRect){start_x + pen_x, pill_y, it->adv_px, pill_h};
                        pill->bg_argb = it->style->bg_argb;
                        pill->next    = box->pills_head;
                        box->pills_head = pill;
                    }
                }
            }

            if (it->cp != 0x200Bu && it->cp != ' ') {
                out_g[gcount++] = (ArcrPositionedGlyph){
                    .codepoint  = it->cp,
                    .x          = start_x + pen_x,
                    .baseline_y = baseline_y,
                    .font_px    = it->style->font_px,
                    .bold       = it->style->font_bold,
                    .color_argb = it->style->color_argb
                };
            }
            pen_x += it->adv_px;
        }
        i = cend;
    }

    if (pen_x > 0 || gcount > 0u) {
        cur_y += cur_line_h;
    }
    box->glyphs      = out_g;
    box->glyph_count = gcount;
    return cur_y - start_y;
}

static void append_child_box(ArcrLayoutBox *parent, ArcrLayoutBox *child) {
    if (!parent->first_child) {
        parent->first_child = child;
        parent->last_child  = child;
    } else {
        parent->last_child->next_sibling = child;
        parent->last_child               = child;
    }
}

static ArcrLayoutBox *layout_block_node(ArcrLayoutTree *tree, const ArcrDocument *doc,
                                        ArcrFontSystem *fonts, const ArcrDomNode *node,
                                        int32_t avail_x, int32_t start_y, int32_t avail_w) {
    if (!node || node->kind != ARCR_NODE_ELEMENT || !node->computed_style) return NULL;
    const ArcrComputedStyle *st = node->computed_style;
    if (st->display == ARCR_DISPLAY_NONE) return NULL;

    ArcrLayoutBox *box = (ArcrLayoutBox *)arcr_arena_calloc(
        &tree->arena, 1u, sizeof(ArcrLayoutBox), sizeof(void *));
    if (!box) return NULL;
    tree->box_count++;

    box->dom_node = node;
    box->style    = st;
    const char *tname = arcr_atom_cstr(doc->atoms, node->tag);
    box->is_list_item = arcr_str_eq_nocase(tname, "li");
    box->is_hr        = arcr_str_eq_nocase(tname, "hr");

    int32_t bx = avail_x + st->margin.left;
    int32_t by = start_y + st->margin.top;
    int32_t bw = ARCR_MAX(24, avail_w - st->margin.left - st->margin.right);

    int32_t cx = bx + st->border.left + st->padding.left;
    int32_t cy = by + st->border.top + st->padding.top;
    int32_t cw = ARCR_MAX(16, bw - st->border.left - st->border.right -
                                  st->padding.left - st->padding.right);

    int32_t cursor_y = cy;

    if (box->is_hr) {
        cursor_y += 2;
    } else {
        const ArcrDomNode *ch = node->first_child;
        while (ch) {
            if (is_inline_node(doc->atoms, ch)) {
                InlineBuf ibuf = {NULL, 0u, 0u};
                while (ch && is_inline_node(doc->atoms, ch)) {
                    collect_inline_items(doc->atoms, fonts, ch, st, &ibuf);
                    ch = ch->next_sibling;
                }
                if (ibuf.count > 0u) {
                    cursor_y += layout_inline_run(tree, box, &ibuf, cx, cursor_y, cw);
                }
                free(ibuf.items);
            } else {
                ArcrLayoutBox *cbox = layout_block_node(tree, doc, fonts, ch, cx, cursor_y, cw);
                if (cbox) {
                    append_child_box(box, cbox);
                    cursor_y = cbox->border_rect.y + cbox->border_rect.height +
                               cbox->style->margin.bottom;
                }
                ch = ch->next_sibling;
            }
        }
    }

    int32_t content_h = cursor_y - cy;
    int32_t bh        = st->border.top + st->padding.top + content_h +
                        st->padding.bottom + st->border.bottom;

    box->content_rect = (ArcrRect){cx, cy, cw, content_h};
    box->border_rect  = (ArcrRect){bx, by, bw, bh};
    return box;
}

ArcrLayoutTree *arcr_layout_build(const ArcrDocument *doc, ArcrFontSystem *fonts,
                                  int32_t viewport_width) {
    if (!doc || !doc->body) return NULL;
    ArcrLayoutTree *tree = (ArcrLayoutTree *)calloc(1u, sizeof(ArcrLayoutTree));
    if (!tree) return NULL;
    arcr_arena_init(&tree->arena, 32768u);
    tree->viewport_width = (viewport_width > 64) ? viewport_width : 64;

    tree->root_box = layout_block_node(tree, doc, fonts, doc->body, 0, 0, tree->viewport_width);
    if (tree->root_box) {
        tree->total_height = tree->root_box->border_rect.y +
                             tree->root_box->border_rect.height +
                             tree->root_box->style->margin.bottom;
    }
    return tree;
}

void arcr_layout_destroy(ArcrLayoutTree *tree) {
    if (!tree) return;
    arcr_arena_destroy(&tree->arena);
    free(tree);
}
