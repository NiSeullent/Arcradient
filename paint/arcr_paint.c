/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * paint/arcr_paint.c - Z-Ordered Stacking Recorder, Linear Gradients, <video> Controls,
 *                      O(N) 64px Y-Band Spatial Binning & Lock-Free Parallel SIMD Rasterizer
 */
#include "arcr_paint.h"
#include <stdlib.h>
#include <string.h>

#define ARCR_MAX_DOC_BANDS 131072u

static void dl_push(ArcrDisplayList *dl, ArcrDisplayCmd cmd) {
    if (cmd.opacity == 0u) return;
    if (dl->count >= dl->capacity) {
        uint32_t ncap = (dl->capacity == 0u) ? 512u : (dl->capacity * 2u);
        ArcrDisplayCmd *grown = (ArcrDisplayCmd *)realloc(
            dl->cmds, (size_t)ncap * sizeof(ArcrDisplayCmd));
        if (!grown) return;
        dl->cmds     = grown;
        dl->capacity = ncap;
    }
    dl->cmds[dl->count++] = cmd;
}

static uint32_t scale_argb_opacity(uint32_t argb, uint8_t opacity) {
    if (opacity >= 255u) return argb;
    if (opacity == 0u)   return argb & 0x00FFFFFFu;
    uint32_t a = (((argb >> 24) & 0xFFu) * (uint32_t)opacity + 127u) / 255u;
    return (a << 24) | (argb & 0x00FFFFFFu);
}

static bool rects_intersect(ArcrRect a, ArcrRect b, ArcrRect *out) {
    int32_t x0 = ARCR_MAX(a.x, b.x);
    int32_t y0 = ARCR_MAX(a.y, b.y);
    int32_t x1 = ARCR_MIN(a.x + a.width, b.x + b.width);
    int32_t y1 = ARCR_MIN(a.y + a.height, b.y + b.height);
    if (x0 >= x1 || y0 >= y1) return false;
    if (out) *out = (ArcrRect){x0, y0, x1 - x0, y1 - y0};
    return true;
}

static ArcrRect compute_glyph_batch_bounds(const ArcrLayoutBox *box) {
    if (!box || box->glyph_count == 0u || !box->glyphs) return box->content_rect;
    int32_t min_x = box->glyphs[0].x - 2;
    int32_t max_x = box->glyphs[0].x + ARCR_MAX(box->glyphs[0].advance_px, box->glyphs[0].font_px) + 2;
    int32_t min_y = box->glyphs[0].baseline_y - box->glyphs[0].font_px - 4;
    int32_t max_y = box->glyphs[0].baseline_y + (box->glyphs[0].font_px / 2) + 4;

    for (uint32_t i = 1u; i < box->glyph_count; ++i) {
        const ArcrPositionedGlyph *g = &box->glyphs[i];
        int32_t gx0 = g->x - 2;
        int32_t gx1 = g->x + ARCR_MAX(g->advance_px, g->font_px) + 2;
        int32_t gy0 = g->baseline_y - g->font_px - 4;
        int32_t gy1 = g->baseline_y + (g->font_px / 2) + 4;
        if (gx0 < min_x) min_x = gx0;
        if (gx1 > max_x) max_x = gx1;
        if (gy0 < min_y) min_y = gy0;
        if (gy1 > max_y) max_y = gy1;
    }
    return (ArcrRect){min_x, min_y, ARCR_MAX(1, max_x - min_x), ARCR_MAX(1, max_y - min_y)};
}

typedef struct PaintClipState {
    ArcrRect clip_rect;
    bool     has_clip;
    bool     is_fixed;
    uint8_t  opacity;
} PaintClipState;

static ArcrDisplayCmd make_cmd(ArcrDisplayCmdKind kind, ArcrRect bounds, PaintClipState cs) {
    ArcrDisplayCmd c;
    memset(&c, 0, sizeof(c));
    c.kind      = kind;
    c.bounds    = bounds;
    c.clip_rect = cs.clip_rect;
    c.has_clip  = cs.has_clip;
    c.is_fixed  = cs.is_fixed;
    c.opacity   = cs.opacity;
    return c;
}

static void record_checkmark_icon(ArcrDisplayList *dl, ArcrRect br, int32_t radius_px,
                                  PaintClipState cs) {
    if (br.width < 8 || br.height < 8) return;
    if (radius_px * 2 >= br.width) {
        int32_t dot = ARCR_MAX(4, br.width / 2 - 2);
        ArcrDisplayCmd c = make_cmd(
            ARCR_DCMD_ROUNDED_RECT,
            (ArcrRect){br.x + (br.width - dot) / 2, br.y + (br.height - dot) / 2, dot, dot},
            cs);
        c.radius_px  = dot / 2;
        c.color_argb = 0xFFFFFFFFu;
        dl_push(dl, c);
        return;
    }

    int32_t cx = br.x + br.width / 2 - 1;
    int32_t cy = br.y + (br.height * 62) / 100;
    int32_t s1 = ARCR_MAX(2, br.width / 6);
    int32_t s2 = ARCR_MAX(4, br.width / 3);
    for (int32_t k = 0; k <= s1; ++k) {
        ArcrDisplayCmd c = make_cmd(ARCR_DCMD_SOLID_RECT, (ArcrRect){cx - k, cy - k, 2, 2}, cs);
        c.color_argb = 0xFFFFFFFFu;
        dl_push(dl, c);
    }
    for (int32_t k = 1; k <= s2; ++k) {
        ArcrDisplayCmd c = make_cmd(ARCR_DCMD_SOLID_RECT, (ArcrRect){cx + k, cy - k, 2, 2}, cs);
        c.color_argb = 0xFFFFFFFFu;
        dl_push(dl, c);
    }
}

static void record_video_chrome(ArcrDisplayList *dl, ArcrRect dst_r, bool has_frame_bitmap,
                                int32_t radius_px, PaintClipState cs) {
    if (dst_r.width < 24 || dst_r.height < 24) return;

    if (!has_frame_bitmap) {
        /* Sleek dark cinema backdrop + centered play badge */
        ArcrDisplayCmd bg = make_cmd(ARCR_DCMD_LINEAR_GRADIENT, dst_r, cs);
        bg.radius_px   = radius_px;
        bg.color_argb  = 0xFF0F172Au;
        bg.color2_argb = 0xFF1E293Bu;
        bg.angle_deg   = 135;
        dl_push(dl, bg);

        int32_t badge = ARCR_CLAMP(ARCR_MIN(dst_r.width, dst_r.height) / 4, 24, 56);
        int32_t bx    = dst_r.x + (dst_r.width - badge) / 2;
        int32_t by    = dst_r.y + (dst_r.height - badge) / 2;
        ArcrDisplayCmd circle = make_cmd(ARCR_DCMD_ROUNDED_RECT,
                                         (ArcrRect){bx, by, badge, badge}, cs);
        circle.radius_px  = badge / 2;
        circle.color_argb = 0xCC2563EBu;
        dl_push(dl, circle);

        /* Crisp play triangle inside badge */
        int32_t tri_h = ARCR_MAX(8, (badge * 44) / 100);
        int32_t tri_w = ARCR_MAX(6, (tri_h * 80) / 100);
        int32_t tx0   = bx + (badge - tri_w) / 2 + 2;
        int32_t tyc   = by + badge / 2;
        for (int32_t col = 0; col < tri_w; ++col) {
            int32_t half_h = ((tri_w - col) * (tri_h / 2)) / ARCR_MAX(1, tri_w);
            if (half_h < 1) half_h = 1;
            ArcrDisplayCmd slice = make_cmd(
                ARCR_DCMD_SOLID_RECT,
                (ArcrRect){tx0 + col, tyc - half_h, 1, half_h * 2},
                cs);
            slice.color_argb = 0xFFFFFFFFu;
            dl_push(dl, slice);
        }
    }

    /* Subtle bottom media scrub bar */
    if (dst_r.width >= 80 && dst_r.height >= 48) {
        int32_t bar_h = 24;
        int32_t bar_y = dst_r.y + dst_r.height - bar_h;
        ArcrDisplayCmd bar = make_cmd(
            ARCR_DCMD_SOLID_RECT,
            (ArcrRect){dst_r.x, bar_y, dst_r.width, bar_h},
            cs);
        bar.color_argb = 0x88000000u;
        dl_push(dl, bar);

        int32_t track_x = dst_r.x + 12;
        int32_t track_w = dst_r.width - 24;
        int32_t track_y = bar_y + (bar_h - 4) / 2;
        ArcrDisplayCmd track = make_cmd(
            ARCR_DCMD_ROUNDED_RECT,
            (ArcrRect){track_x, track_y, track_w, 4},
            cs);
        track.radius_px  = 2;
        track.color_argb = 0x55FFFFFFu;
        dl_push(dl, track);

        int32_t prog_w = ARCR_MAX(8, track_w / 3);
        ArcrDisplayCmd prog = make_cmd(
            ARCR_DCMD_ROUNDED_RECT,
            (ArcrRect){track_x, track_y, prog_w, 4},
            cs);
        prog.radius_px  = 2;
        prog.color_argb = 0xFF3B82F6u;
        dl_push(dl, prog);
    }
}

static void record_text_decorations(ArcrDisplayList *dl, const ArcrLayoutBox *box,
                                    PaintClipState cs) {
    /* Pass 1: underline runs */
    uint32_t i = 0u;
    while (i < box->glyph_count) {
        const ArcrPositionedGlyph *g = &box->glyphs[i];
        if (!g->underline) { ++i; continue; }
        int32_t  x0       = g->x;
        int32_t  x1       = g->x + g->advance_px;
        int32_t  base_y   = g->baseline_y;
        int32_t  fpx      = g->font_px;
        uint32_t line_col = g->color_argb;
        uint32_t j        = i + 1u;
        while (j < box->glyph_count &&
               box->glyphs[j].underline &&
               box->glyphs[j].baseline_y == base_y &&
               box->glyphs[j].color_argb == line_col &&
               box->glyphs[j].x <= x1 + 4) {
            x1 = box->glyphs[j].x + box->glyphs[j].advance_px;
            ++j;
        }
        if (x1 > x0) {
            int32_t thickness = ARCR_MAX(1, fpx / 14);
            ArcrDisplayCmd c = make_cmd(
                ARCR_DCMD_SOLID_RECT,
                (ArcrRect){x0, base_y + 2, x1 - x0, thickness},
                cs);
            c.color_argb = line_col;
            dl_push(dl, c);
        }
        i = j;
    }

    /* Pass 2: line-through runs (<s>, <del>, text-decoration: line-through) */
    i = 0u;
    while (i < box->glyph_count) {
        const ArcrPositionedGlyph *g = &box->glyphs[i];
        if (!g->line_through) { ++i; continue; }
        int32_t  x0       = g->x;
        int32_t  x1       = g->x + g->advance_px;
        int32_t  base_y   = g->baseline_y;
        int32_t  fpx      = g->font_px;
        uint32_t line_col = g->color_argb;
        uint32_t j        = i + 1u;
        while (j < box->glyph_count &&
               box->glyphs[j].line_through &&
               box->glyphs[j].baseline_y == base_y &&
               box->glyphs[j].color_argb == line_col &&
               box->glyphs[j].x <= x1 + 4) {
            x1 = box->glyphs[j].x + box->glyphs[j].advance_px;
            ++j;
        }
        if (x1 > x0) {
            int32_t thickness = ARCR_MAX(1, fpx / 14);
            int32_t strike_y  = base_y - (fpx * 30) / 100;
            ArcrDisplayCmd c = make_cmd(
                ARCR_DCMD_SOLID_RECT,
                (ArcrRect){x0, strike_y, x1 - x0, thickness},
                cs);
            c.color_argb = line_col;
            dl_push(dl, c);
        }
        i = j;
    }
}

static void record_box_self(ArcrDisplayList *dl, const ArcrLayoutBox *box,
                            bool parent_has_clip, ArcrRect parent_clip) {
    if (!box || !box->style) return;
    const ArcrComputedStyle *st = box->style;
    if (st->visibility_hidden || box->effective_opacity == 0u) return;

    ArcrRect br = box->border_rect;
    bool enters_fixed = (st->position == ARCR_POS_FIXED);

    PaintClipState outer_cs = {
        .clip_rect = parent_clip,
        .has_clip  = enters_fixed ? false : parent_has_clip,
        .is_fixed  = box->is_fixed,
        .opacity   = box->effective_opacity
    };
    PaintClipState inner_cs = {
        .clip_rect = box->clip_rect,
        .has_clip  = box->has_clip,
        .is_fixed  = box->is_fixed,
        .opacity   = box->effective_opacity
    };

    if (box->is_hr) {
        int32_t line_y = br.y + br.height / 2;
        ArcrDisplayCmd c = make_cmd(
            ARCR_DCMD_SOLID_RECT,
            (ArcrRect){br.x, line_y, br.width, ARCR_MAX(1, st->border.top ? st->border.top : 1)},
            outer_cs);
        c.color_argb = (((st->border_argb >> 24) & 0xFFu) > 0u) ? st->border_argb : 0xFFCBD5E1u;
        dl_push(dl, c);
        return;
    }

    bool is_focused = (box->dom_node && (box->dom_node->state_flags & ARCR_STATE_FOCUS) != 0u);
    bool is_checked = (box->dom_node && (box->dom_node->state_flags & ARCR_STATE_CHECKED) != 0u);

    /* 1. Elevation Box Shadow (clipped only by ancestor clip, never self overflow clip) */
    if (st->shadow_blur > 0 && ((st->shadow_argb >> 24) & 0xFFu) > 0u &&
        br.width > 0 && br.height > 0) {
        ArcrDisplayCmd c = make_cmd(ARCR_DCMD_BOX_SHADOW, br, outer_cs);
        c.color_argb        = st->shadow_argb;
        c.radius_px         = st->border_radius;
        c.border_or_blur_px = st->shadow_blur;
        c.offset_y          = st->shadow_offset_y;
        dl_push(dl, c);
    }

    /* 2. Focus Ring Halo for Interactive Form Controls */
    if (is_focused && box->control_kind != ARCR_CTRL_NONE &&
        box->control_kind != ARCR_CTRL_VIDEO_MEDIA) {
        ArcrDisplayCmd c = make_cmd(
            ARCR_DCMD_ROUNDED_STROKE,
            (ArcrRect){br.x - 2, br.y - 2, br.width + 4, br.height + 4},
            outer_cs);
        c.radius_px         = st->border_radius + 2;
        c.border_or_blur_px = 2;
        c.color_argb        = 0x553B82F6u;
        dl_push(dl, c);
    }

    /* 3. Box Background (Linear Gradient or Solid / Rounded Rect) */
    uint32_t bg_c = st->bg_argb;
    if (box->control_kind == ARCR_CTRL_CHECKBOX && is_checked) {
        bg_c = 0xFF2563EBu;
    }
    if (st->has_gradient && br.width > 0 && br.height > 0 &&
        !(box->control_kind == ARCR_CTRL_CHECKBOX && is_checked)) {
        ArcrDisplayCmd c = make_cmd(ARCR_DCMD_LINEAR_GRADIENT, br, outer_cs);
        c.radius_px   = st->border_radius;
        c.color_argb  = st->grad_start_argb;
        c.color2_argb = st->grad_end_argb;
        c.angle_deg   = st->grad_angle_deg;
        dl_push(dl, c);
    } else if (((bg_c >> 24) & 0xFFu) > 0u && br.width > 0 && br.height > 0) {
        if (st->border_radius > 0) {
            ArcrDisplayCmd c = make_cmd(ARCR_DCMD_ROUNDED_RECT, br, outer_cs);
            c.radius_px  = st->border_radius;
            c.color_argb = bg_c;
            dl_push(dl, c);
        } else {
            ArcrDisplayCmd c = make_cmd(ARCR_DCMD_SOLID_RECT, br, outer_cs);
            c.color_argb = bg_c;
            dl_push(dl, c);
        }
    }

    /* 4. Borders (Rounded 4-side stroke ONLY when all 4 edges > 0; else per-side rects) */
    uint32_t border_c = st->border_argb;
    if (box->control_kind == ARCR_CTRL_CHECKBOX && is_checked) {
        border_c = 0xFF1D4ED8u;
    } else if (is_focused && box->control_kind == ARCR_CTRL_TEXT_INPUT) {
        border_c = 0xFF2563EBu;
    }

    if (((border_c >> 24) & 0xFFu) > 0u && br.width > 0 && br.height > 0) {
        bool all_four = (st->border.top > 0 && st->border.right > 0 &&
                         st->border.bottom > 0 && st->border.left > 0);
        if (st->border_radius > 0 && all_four) {
            int32_t max_bw = ARCR_MAX(ARCR_MAX(st->border.top, st->border.bottom),
                                      ARCR_MAX(st->border.left, st->border.right));
            ArcrDisplayCmd c = make_cmd(ARCR_DCMD_ROUNDED_STROKE, br, outer_cs);
            c.radius_px         = st->border_radius;
            c.border_or_blur_px = max_bw;
            c.color_argb        = border_c;
            dl_push(dl, c);
        } else {
            if (st->border.top > 0) {
                ArcrDisplayCmd c = make_cmd(
                    ARCR_DCMD_SOLID_RECT,
                    (ArcrRect){br.x, br.y, br.width, st->border.top},
                    outer_cs);
                c.color_argb = border_c;
                dl_push(dl, c);
            }
            if (st->border.bottom > 0) {
                ArcrDisplayCmd c = make_cmd(
                    ARCR_DCMD_SOLID_RECT,
                    (ArcrRect){br.x, br.y + br.height - st->border.bottom,
                               br.width, st->border.bottom},
                    outer_cs);
                c.color_argb = border_c;
                dl_push(dl, c);
            }
            if (st->border.left > 0) {
                ArcrDisplayCmd c = make_cmd(
                    ARCR_DCMD_SOLID_RECT,
                    (ArcrRect){br.x, br.y, st->border.left, br.height},
                    outer_cs);
                c.color_argb = border_c;
                dl_push(dl, c);
            }
            if (st->border.right > 0) {
                ArcrDisplayCmd c = make_cmd(
                    ARCR_DCMD_SOLID_RECT,
                    (ArcrRect){br.x + br.width - st->border.right, br.y,
                               st->border.right, br.height},
                    outer_cs);
                c.color_argb = border_c;
                dl_push(dl, c);
            }
        }
    }

    /* 5. Checked Checkbox / Radio Indicator */
    if (box->control_kind == ARCR_CTRL_CHECKBOX && is_checked) {
        record_checkmark_icon(dl, br, st->border_radius, inner_cs);
    }

    /* 6. Decoded <img> / <svg> / <canvas> / <video> Surface Blit */
    bool has_surface = (box->dom_node && box->dom_node->surface &&
                        box->dom_node->surface->argb_pixels &&
                        box->dom_node->surface->width > 0 &&
                        box->dom_node->surface->height > 0);
    ArcrRect media_dst = (box->content_rect.width > 0 && box->content_rect.height > 0)
                             ? box->content_rect
                             : br;

    if (has_surface) {
        ArcrDisplayCmd c = make_cmd(ARCR_DCMD_IMAGE_BLIT, media_dst, inner_cs);
        c.surface = box->dom_node->surface;
        dl_push(dl, c);
    }

    if (box->control_kind == ARCR_CTRL_VIDEO_MEDIA) {
        record_video_chrome(dl, media_dst, has_surface, st->border_radius, inner_cs);
    } else if (!has_surface && box->dom_node && !box->dom_node->first_child &&
               box->control_kind == ARCR_CTRL_NONE &&
               box->glyph_count > 0u &&
               ((bg_c >> 24) & 0xFFu) == 0u && !st->has_gradient &&
               box->content_rect.width >= 16 && box->content_rect.height >= 16) {
        /* Unloaded <img alt="..."> placeholder frame */
        ArcrDisplayCmd fill = make_cmd(ARCR_DCMD_ROUNDED_RECT, box->content_rect, inner_cs);
        fill.radius_px  = 4;
        fill.color_argb = 0xFFF1F5F9u;
        dl_push(dl, fill);

        ArcrDisplayCmd stroke = make_cmd(ARCR_DCMD_ROUNDED_STROKE, box->content_rect, inner_cs);
        stroke.radius_px         = 4;
        stroke.border_or_blur_px = 1;
        stroke.color_argb        = 0xFFCBD5E1u;
        dl_push(dl, stroke);
    }

    /* 7. Unordered List Bullet Marker (<ul>) */
    if (box->is_list_item && box->list_ordinal == 0) {
        int32_t dot_sz = ARCR_CLAMP(st->font_px / 3, 5, 8);
        int32_t dot_x  = box->content_rect.x - dot_sz * 2 - 4;
        int32_t dot_y  = box->content_rect.y + (st->font_px * 52) / 100;
        /* Bullet sits in parent list gutter, so clip against outer_cs */
        ArcrDisplayCmd c = make_cmd(
            ARCR_DCMD_ROUNDED_RECT,
            (ArcrRect){dot_x, dot_y, dot_sz, dot_sz},
            outer_cs);
        c.radius_px  = dot_sz / 2;
        c.color_argb = st->color_argb;
        dl_push(dl, c);
    }

    /* 8. Inline Background Pills (<code>, <mark>) */
    for (const ArcrInlineBgPill *p = box->pills_head; p != NULL; p = p->next) {
        if (p->radius > 0) {
            ArcrDisplayCmd c = make_cmd(ARCR_DCMD_ROUNDED_RECT, p->rect, inner_cs);
            c.radius_px  = p->radius;
            c.color_argb = p->bg_argb;
            dl_push(dl, c);
        } else {
            ArcrDisplayCmd c = make_cmd(ARCR_DCMD_SOLID_RECT, p->rect, inner_cs);
            c.color_argb = p->bg_argb;
            dl_push(dl, c);
        }
    }

    /* 9. Text Decorations & Positioned Glyph Batch */
    if (box->glyph_count > 0u && box->glyphs) {
        record_text_decorations(dl, box, inner_cs);
        ArcrDisplayCmd c = make_cmd(
            ARCR_DCMD_GLYPH_BATCH,
            compute_glyph_batch_bounds(box),
            inner_cs);
        c.glyphs      = box->glyphs;
        c.glyph_count = box->glyph_count;
        dl_push(dl, c);
    }

    /* 10. Focused Text Input / Textarea Caret */
    if (is_focused && box->control_kind == ARCR_CTRL_TEXT_INPUT) {
        const char *live = box->dom_node ? box->dom_node->input_value : NULL;
        int32_t caret_h  = ARCR_MAX(12, st->font_px + 2);
        int32_t caret_x  = box->content_rect.x + 1;
        int32_t caret_y  = box->content_rect.y +
                           ARCR_MAX(0, (box->content_rect.height - caret_h) / 2);

        if (live && live[0] != '\0' && box->glyph_count > 0u) {
            const ArcrPositionedGlyph *last = &box->glyphs[box->glyph_count - 1u];
            caret_x = last->x + last->advance_px + 1;
            caret_y = last->baseline_y - st->font_px + 2;
        }
        int32_t max_cx = box->content_rect.x + ARCR_MAX(2, box->content_rect.width - 2);
        if (caret_x > max_cx) caret_x = max_cx;
        ArcrDisplayCmd c = make_cmd(
            ARCR_DCMD_SOLID_RECT,
            (ArcrRect){caret_x, caret_y, 2, caret_h},
            inner_cs);
        c.color_argb = 0xFF2563EBu;
        dl_push(dl, c);
    }
}

/* -------------------------------------------------------------------------
 * CSS2.1 / CSS3 Stacking-Context Paint Order Traversal
 * ------------------------------------------------------------------------- */
typedef struct StackedChildEntry {
    const ArcrLayoutBox *box;
    uint32_t             dom_order;
} StackedChildEntry;

static bool is_stacked_child(const ArcrLayoutBox *ch) {
    if (!ch || !ch->style) return false;
    return ch->style->position != ARCR_POS_STATIC || ch->z_index != 0;
}

static int cmp_stacked_children(const void *pa, const void *pb) {
    const StackedChildEntry *a = (const StackedChildEntry *)pa;
    const StackedChildEntry *b = (const StackedChildEntry *)pb;
    if (a->box->z_index != b->box->z_index) {
        return (a->box->z_index < b->box->z_index) ? -1 : 1;
    }
    return (a->dom_order < b->dom_order) ? -1 : 1;
}

static void record_box_stacked(ArcrDisplayList *dl, const ArcrLayoutBox *box,
                               bool parent_has_clip, ArcrRect parent_clip) {
    if (!box) return;

    size_t pos_cnt = 0u;
    for (const ArcrLayoutBox *ch = box->first_child; ch != NULL; ch = ch->next_sibling) {
        if (is_stacked_child(ch)) ++pos_cnt;
    }

    StackedChildEntry *pos_list = NULL;
    if (pos_cnt > 0u) {
        pos_list = (StackedChildEntry *)malloc(pos_cnt * sizeof(StackedChildEntry));
        if (pos_list) {
            uint32_t ord = 0u;
            size_t   k   = 0u;
            for (const ArcrLayoutBox *ch = box->first_child; ch != NULL; ch = ch->next_sibling) {
                if (is_stacked_child(ch)) {
                    pos_list[k++] = (StackedChildEntry){ch, ord++};
                }
            }
            if (pos_cnt > 1u) {
                qsort(pos_list, pos_cnt, sizeof(StackedChildEntry), cmp_stacked_children);
            }
        }
    }

    /* 1. Negative z_index positioned child subtrees */
    if (pos_list) {
        for (size_t k = 0u; k < pos_cnt; ++k) {
            if (pos_list[k].box->z_index < 0) {
                record_box_stacked(dl, pos_list[k].box, box->has_clip, box->clip_rect);
            }
        }
    }

    /* 2. Box background, borders, replaced media & inline content */
    record_box_self(dl, box, parent_has_clip, parent_clip);

    /* 3. Normal-flow non-positioned children */
    for (const ArcrLayoutBox *ch = box->first_child; ch != NULL; ch = ch->next_sibling) {
        if (!is_stacked_child(ch)) {
            record_box_stacked(dl, ch, box->has_clip, box->clip_rect);
        }
    }

    /* 4. z_index == 0 and positive z_index > 0 positioned/fixed children */
    if (pos_list) {
        for (size_t k = 0u; k < pos_cnt; ++k) {
            if (pos_list[k].box->z_index >= 0) {
                record_box_stacked(dl, pos_list[k].box, box->has_clip, box->clip_rect);
            }
        }
        free(pos_list);
    }
}

/* -------------------------------------------------------------------------
 * Case-Insensitive UTF-8 Find-in-Page Highlight Pass
 * ------------------------------------------------------------------------- */
static uint32_t fold_cp_lower(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + ('a' - 'A');
    if (cp >= 0xC0u && cp <= 0xDEu && cp != 0xD7u) return cp + 0x20u;
    return cp;
}

static void scan_find_matches_rec(ArcrDisplayList *dl, const ArcrLayoutBox *box,
                                  const uint32_t *q_cps, uint32_t q_len,
                                  bool emit_rects, uint32_t active_idx,
                                  uint32_t *inout_match_count,
                                  int32_t *out_active_doc_y) {
    if (!box) return;

    if (box->dom_node != NULL && box->style && !box->style->visibility_hidden &&
        box->effective_opacity > 0u && box->glyph_count >= q_len && box->glyphs) {
        uint32_t g = 0u;
        while (g + q_len <= box->glyph_count) {
            bool ok = true;
            for (uint32_t k = 0u; k < q_len; ++k) {
                if (fold_cp_lower(box->glyphs[g + k].codepoint) != q_cps[k]) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                uint32_t cur_idx = (*inout_match_count)++;
                if (emit_rects) {
                    bool     is_active = (cur_idx == active_idx);
                    uint32_t hl_color  = is_active ? 0x99F97316u : 0x66FACC15u;
                    PaintClipState cs  = {box->clip_rect, box->has_clip, box->is_fixed, 255u};

                    uint32_t s = 0u;
                    while (s < q_len) {
                        const ArcrPositionedGlyph *first_g = &box->glyphs[g + s];
                        int32_t base_y = first_g->baseline_y;
                        int32_t x0     = first_g->x;
                        int32_t x1     = first_g->x + first_g->advance_px;
                        int32_t fpx    = first_g->font_px;
                        uint32_t e     = s + 1u;
                        while (e < q_len && box->glyphs[g + e].baseline_y == base_y) {
                            x1 = box->glyphs[g + e].x + box->glyphs[g + e].advance_px;
                            if (box->glyphs[g + e].font_px > fpx) {
                                fpx = box->glyphs[g + e].font_px;
                            }
                            ++e;
                        }
                        int32_t hy = base_y - fpx - 1;
                        int32_t hh = fpx + 5;
                        if (is_active && s == 0u && out_active_doc_y) {
                            *out_active_doc_y = ARCR_MAX(0, hy);
                        }
                        ArcrDisplayCmd c = make_cmd(
                            ARCR_DCMD_ROUNDED_RECT,
                            (ArcrRect){x0 - 1, hy, ARCR_MAX(4, x1 - x0 + 2), hh},
                            cs);
                        c.radius_px  = 3;
                        c.color_argb = hl_color;
                        dl_push(dl, c);
                        s = e;
                    }
                }
                g += q_len;
            } else {
                ++g;
            }
        }
    }

    for (const ArcrLayoutBox *ch = box->first_child; ch != NULL; ch = ch->next_sibling) {
        scan_find_matches_rec(dl, ch, q_cps, q_len, emit_rects, active_idx,
                              inout_match_count, out_active_doc_y);
    }
}

static void append_find_highlights(ArcrDisplayList *dl, const ArcrLayoutTree *layout,
                                   const char *find_query_utf8, uint32_t active_find_index,
                                   uint32_t *out_total_matches, int32_t *out_active_doc_y) {
    if (out_total_matches) *out_total_matches = 0u;
    if (out_active_doc_y)  *out_active_doc_y  = 0;
    if (!dl || !layout || !layout->root_box || !find_query_utf8 || find_query_utf8[0] == '\0') {
        return;
    }

    uint32_t q_cps[128];
    uint32_t q_len = 0u;
    size_t   slen  = strlen(find_query_utf8);
    size_t   pos   = 0u;
    while (pos < slen && q_len < ARCR_ARRAY_LEN(q_cps)) {
        uint32_t cp = 0u;
        size_t step = arcr_utf8_decode_one(find_query_utf8 + pos, slen - pos, &cp);
        if (step == 0u) break;
        pos += step;
        q_cps[q_len++] = fold_cp_lower(cp);
    }
    if (q_len == 0u) return;

    uint32_t total = 0u;
    scan_find_matches_rec(dl, layout->root_box, q_cps, q_len, false, 0u, &total, NULL);
    if (out_total_matches) *out_total_matches = total;
    if (total == 0u) return;

    uint32_t eff_active = (active_find_index < total) ? active_find_index
                                                      : (active_find_index % total);
    uint32_t second_cnt = 0u;
    scan_find_matches_rec(dl, layout->root_box, q_cps, q_len, true, eff_active,
                          &second_cnt, out_active_doc_y);
}

/* -------------------------------------------------------------------------
 * O(N) 64px Spatial Y-Band Binning (Eliminates O(T * C) Tile Scan Bottleneck!)
 * ------------------------------------------------------------------------- */
static bool compute_cmd_cull_rect(const ArcrDisplayCmd *cmd, ArcrRect *out_cull) {
    ArcrRect r = cmd->bounds;
    if (cmd->kind == ARCR_DCMD_BOX_SHADOW) {
        int32_t pad   = ARCR_MAX(4, cmd->border_or_blur_px * 2 + 4);
        int32_t y_min = ARCR_MIN(r.y, r.y + cmd->offset_y) - pad;
        int32_t y_max = ARCR_MAX(r.y + r.height, r.y + cmd->offset_y + r.height) + pad;
        r.x      -= pad;
        r.y       = y_min;
        r.width  += pad * 2;
        r.height  = y_max - y_min;
    } else if (cmd->kind == ARCR_DCMD_ROUNDED_STROKE) {
        r.x -= 2; r.y -= 2; r.width += 4; r.height += 4;
    } else if (cmd->kind == ARCR_DCMD_GLYPH_BATCH) {
        r.x -= 8; r.y -= 8; r.width += 16; r.height += 16;
    }
    if (r.width <= 0 || r.height <= 0) return false;
    if (cmd->has_clip) {
        if (!rects_intersect(r, cmd->clip_rect, &r)) return false;
    }
    *out_cull = r;
    return true;
}

static void build_spatial_bins(ArcrDisplayList *dl, int32_t total_layout_h) {
    if (!dl || dl->count == 0u) return;

    int32_t  max_y     = ARCR_MAX(total_layout_h, ARCR_TILE_SIZE_PX);
    uint32_t fixed_cnt = 0u;

    for (uint32_t i = 0u; i < dl->count; ++i) {
        ArcrRect cr;
        if (!compute_cmd_cull_rect(&dl->cmds[i], &cr)) continue;
        if (dl->cmds[i].is_fixed) {
            ++fixed_cnt;
        } else {
            int32_t y1 = cr.y + cr.height;
            if (y1 > max_y) max_y = y1;
        }
    }

    uint32_t band_count = (uint32_t)((max_y + ARCR_TILE_SIZE_PX - 1) / ARCR_TILE_SIZE_PX);
    if (band_count < 1u) band_count = 1u;
    if (band_count > ARCR_MAX_DOC_BANDS) band_count = ARCR_MAX_DOC_BANDS;

    dl->doc_bands = (ArcrTileBandBin *)arcr_arena_calloc(
        &dl->arena, band_count, sizeof(ArcrTileBandBin), sizeof(void *));
    if (!dl->doc_bands) return;
    dl->doc_band_count = band_count;

    uint32_t *band_caps = (uint32_t *)calloc(band_count, sizeof(uint32_t));
    if (!band_caps) return;

    /* Pass 1: Count commands per 64px document Y-band */
    for (uint32_t i = 0u; i < dl->count; ++i) {
        if (dl->cmds[i].is_fixed) continue;
        ArcrRect cr;
        if (!compute_cmd_cull_rect(&dl->cmds[i], &cr)) continue;
        int32_t y1 = cr.y + cr.height - 1;
        if (y1 < 0) continue;
        int32_t y0 = ARCR_MAX(0, cr.y);

        uint32_t b0 = (uint32_t)(y0 / ARCR_TILE_SIZE_PX);
        uint32_t b1 = (uint32_t)(y1 / ARCR_TILE_SIZE_PX);
        if (b0 >= band_count) continue;
        if (b1 >= band_count) b1 = band_count - 1u;
        for (uint32_t b = b0; b <= b1; ++b) {
            band_caps[b]++;
        }
    }

    /* Allocate arena slices for non-empty bands + fixed layer */
    uint32_t **band_mut = (uint32_t **)calloc(band_count, sizeof(uint32_t *));
    if (!band_mut) {
        free(band_caps);
        return;
    }
    for (uint32_t b = 0u; b < band_count; ++b) {
        if (band_caps[b] > 0u) {
            band_mut[b] = (uint32_t *)arcr_arena_alloc(
                &dl->arena, (size_t)band_caps[b] * sizeof(uint32_t), sizeof(uint32_t));
            dl->doc_bands[b].cmd_indices = band_mut[b];
            dl->doc_bands[b].count       = 0u;
        }
    }

    if (fixed_cnt > 0u) {
        dl->fixed_cmd_indices = (uint32_t *)arcr_arena_alloc(
            &dl->arena, (size_t)fixed_cnt * sizeof(uint32_t), sizeof(uint32_t));
    }
    dl->fixed_cmd_count = 0u;

    /* Pass 2: Populate indices in strictly ascending command Z-order */
    for (uint32_t i = 0u; i < dl->count; ++i) {
        ArcrRect cr;
        if (!compute_cmd_cull_rect(&dl->cmds[i], &cr)) continue;

        if (dl->cmds[i].is_fixed) {
            if (dl->fixed_cmd_indices && dl->fixed_cmd_count < fixed_cnt) {
                dl->fixed_cmd_indices[dl->fixed_cmd_count++] = i;
            }
            continue;
        }

        int32_t y1 = cr.y + cr.height - 1;
        if (y1 < 0) continue;
        int32_t y0 = ARCR_MAX(0, cr.y);

        uint32_t b0 = (uint32_t)(y0 / ARCR_TILE_SIZE_PX);
        uint32_t b1 = (uint32_t)(y1 / ARCR_TILE_SIZE_PX);
        if (b0 >= band_count) continue;
        if (b1 >= band_count) b1 = band_count - 1u;

        for (uint32_t b = b0; b <= b1; ++b) {
            if (band_mut[b] && dl->doc_bands[b].count < band_caps[b]) {
                band_mut[b][dl->doc_bands[b].count++] = i;
            }
        }
    }

    free(band_mut);
    free(band_caps);
}

ArcrDisplayList *arcr_paint_build_display_list_ex(const ArcrLayoutTree *layout,
                                                  const char *find_query_utf8,
                                                  uint32_t active_find_index,
                                                  uint32_t *out_total_find_matches,
                                                  int32_t *out_active_match_doc_y) {
    if (out_total_find_matches) *out_total_find_matches = 0u;
    if (out_active_match_doc_y)  *out_active_match_doc_y  = 0;
    if (!layout) return NULL;

    ArcrDisplayList *dl = (ArcrDisplayList *)calloc(1u, sizeof(ArcrDisplayList));
    if (!dl) return NULL;
    arcr_arena_init(&dl->arena, 16384u);

    dl->default_bg_argb = (layout->root_box && layout->root_box->style &&
                           ((layout->root_box->style->bg_argb >> 24) & 0xFFu) > 0u)
                              ? layout->root_box->style->bg_argb
                              : 0xFFFFFFFFu;
    if (layout->root_box) {
        record_box_stacked(dl, layout->root_box, false, (ArcrRect){0, 0, 0, 0});
        if (find_query_utf8 && find_query_utf8[0] != '\0') {
            append_find_highlights(dl, layout, find_query_utf8, active_find_index,
                                   out_total_find_matches, out_active_match_doc_y);
        }
        build_spatial_bins(dl, layout->total_height);
    }
    return dl;
}

ArcrDisplayList *arcr_paint_build_display_list(const ArcrLayoutTree *layout) {
    return arcr_paint_build_display_list_ex(layout, NULL, 0u, NULL, NULL);
}

void arcr_paint_destroy_display_list(ArcrDisplayList *dl) {
    if (!dl) return;
    free(dl->cmds);
    arcr_arena_destroy(&dl->arena);
    free(dl);
}

/* -------------------------------------------------------------------------
 * Lock-Free Glyph Atlas Pre-Warm + 64x64 Tile-Binned Parallel SIMD Rasterizer
 * ------------------------------------------------------------------------- */
typedef struct TileRasterJob {
    const ArcrDisplayList *dl;
    ArcrAccelContext      *accel;
    ArcrFontSystem        *fonts;
    uint32_t              *dst_argb;
    int32_t                stride_px;
    ArcrRect               viewport_rect;
    ArcrRect               eff_vp;
    int32_t                scroll_y;
    uint32_t               tiles_x;
    uint32_t               tiles_y;
} TileRasterJob;

static bool cmd_intersects_screen_rect(const ArcrDisplayCmd *cmd, ArcrRect screen_r,
                                       ArcrRect target_r) {
    ArcrRect cull_r = screen_r;
    if (cmd->kind == ARCR_DCMD_BOX_SHADOW) {
        int32_t pad   = ARCR_MAX(4, cmd->border_or_blur_px * 2 + 4);
        int32_t y_min = ARCR_MIN(screen_r.y, screen_r.y + cmd->offset_y) - pad;
        int32_t y_max = ARCR_MAX(screen_r.y + screen_r.height,
                                 screen_r.y + cmd->offset_y + screen_r.height) + pad;
        cull_r.x      = screen_r.x - pad;
        cull_r.y      = y_min;
        cull_r.width  = screen_r.width + pad * 2;
        cull_r.height = y_max - y_min;
    } else if (cmd->kind == ARCR_DCMD_ROUNDED_STROKE) {
        cull_r.x -= 2; cull_r.y -= 2; cull_r.width += 4; cull_r.height += 4;
    } else if (cmd->kind == ARCR_DCMD_GLYPH_BATCH) {
        cull_r.x -= 8; cull_r.y -= 8; cull_r.width += 16; cull_r.height += 16;
    }
    return rects_intersect(cull_r, target_r, NULL);
}

/* Translucent image/SVG blit helper when cmd->opacity < 255 */
static void blit_image_with_opacity(uint32_t *win_dst, int32_t win_w, int32_t win_h,
                                    int32_t stride_px, ArcrRect local_r,
                                    const uint32_t *src_argb, int32_t src_w, int32_t src_h,
                                    uint8_t opacity) {
    if (local_r.width <= 0 || local_r.height <= 0 || src_w <= 0 || src_h <= 0) return;
    int32_t x0 = ARCR_MAX(0, local_r.x);
    int32_t y0 = ARCR_MAX(0, local_r.y);
    int32_t x1 = ARCR_MIN(win_w, local_r.x + local_r.width);
    int32_t y1 = ARCR_MIN(win_h, local_r.y + local_r.height);
    if (x0 >= x1 || y0 >= y1) return;

    for (int32_t y = y0; y < y1; ++y) {
        int32_t sy = (int32_t)(((int64_t)(y - local_r.y) * src_h) / local_r.height);
        sy = ARCR_CLAMP(sy, 0, src_h - 1);
        const uint32_t *srow = src_argb + (size_t)sy * (size_t)src_w;
        uint32_t       *drow = win_dst + (size_t)y * (size_t)stride_px;

        for (int32_t x = x0; x < x1; ++x) {
            int32_t sx = (int32_t)(((int64_t)(x - local_r.x) * src_w) / local_r.width);
            sx = ARCR_CLAMP(sx, 0, src_w - 1);
            uint32_t spx = srow[sx];
            uint32_t sa  = (((spx >> 24) & 0xFFu) * (uint32_t)opacity + 127u) / 255u;
            if (sa == 0u) continue;
            if (sa >= 255u) {
                drow[x] = 0xFF000000u | (spx & 0x00FFFFFFu);
            } else {
                uint32_t dpx = drow[x];
                uint32_t inv = 255u - sa;
                uint32_t r = ((((spx >> 16) & 0xFFu) * sa + ((dpx >> 16) & 0xFFu) * inv + 127u) / 255u);
                uint32_t g = ((((spx >> 8)  & 0xFFu) * sa + ((dpx >> 8)  & 0xFFu) * inv + 127u) / 255u);
                uint32_t b = (((spx         & 0xFFu) * sa + (dpx         & 0xFFu) * inv + 127u) / 255u);
                drow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
}

static void execute_cmd_on_tile(const TileRasterJob *job, const ArcrDisplayCmd *cmd,
                                ArcrRect tile_screen_r) {
    if (cmd->opacity == 0u) return;

    int32_t y_scroll = cmd->is_fixed ? 0 : job->scroll_y;
    ArcrRect screen_r = {
        .x      = job->viewport_rect.x + cmd->bounds.x,
        .y      = job->viewport_rect.y + cmd->bounds.y - y_scroll,
        .width  = cmd->bounds.width,
        .height = cmd->bounds.height
    };

    ArcrRect draw_win = tile_screen_r;
    if (cmd->has_clip) {
        ArcrRect screen_clip = {
            .x      = job->viewport_rect.x + cmd->clip_rect.x,
            .y      = job->viewport_rect.y + cmd->clip_rect.y - y_scroll,
            .width  = cmd->clip_rect.width,
            .height = cmd->clip_rect.height
        };
        if (!rects_intersect(draw_win, screen_clip, &draw_win)) return;
    }

    if (!cmd_intersects_screen_rect(cmd, screen_r, draw_win)) return;

    uint32_t *win_dst = job->dst_argb +
                        (size_t)draw_win.y * (size_t)job->stride_px + (size_t)draw_win.x;
    int32_t win_w = draw_win.width;
    int32_t win_h = draw_win.height;

    ArcrRect local_r = {
        .x      = screen_r.x - draw_win.x,
        .y      = screen_r.y - draw_win.y,
        .width  = screen_r.width,
        .height = screen_r.height
    };

    uint32_t col1 = scale_argb_opacity(cmd->color_argb, cmd->opacity);

    switch (cmd->kind) {
        case ARCR_DCMD_SOLID_RECT: {
            uint32_t alpha = (col1 >> 24) & 0xFFu;
            if (alpha == 255u) {
                arcr_accel_fill_rect(job->accel, win_dst, win_w, win_h, job->stride_px,
                                     local_r, col1);
            } else if (alpha > 0u) {
                arcr_accel_blend_rect(job->accel, win_dst, win_w, win_h, job->stride_px,
                                      local_r, col1);
            }
            break;
        }
        case ARCR_DCMD_ROUNDED_RECT: {
            arcr_accel_fill_rounded_rect(job->accel, win_dst, win_w, win_h, job->stride_px,
                                         local_r, cmd->radius_px, col1);
            break;
        }
        case ARCR_DCMD_ROUNDED_STROKE: {
            arcr_accel_stroke_rounded_rect(job->accel, win_dst, win_w, win_h, job->stride_px,
                                           local_r, cmd->radius_px, cmd->border_or_blur_px, col1);
            break;
        }
        case ARCR_DCMD_BOX_SHADOW: {
            arcr_accel_draw_box_shadow(job->accel, win_dst, win_w, win_h, job->stride_px,
                                       local_r, cmd->radius_px, cmd->border_or_blur_px,
                                       cmd->offset_y, col1);
            break;
        }
        case ARCR_DCMD_LINEAR_GRADIENT: {
            uint32_t col2 = scale_argb_opacity(cmd->color2_argb, cmd->opacity);
            arcr_accel_fill_linear_gradient(job->accel, win_dst, win_w, win_h, job->stride_px,
                                            local_r, cmd->radius_px, col1, col2, cmd->angle_deg);
            break;
        }
        case ARCR_DCMD_IMAGE_BLIT: {
            if (cmd->surface && cmd->surface->argb_pixels &&
                cmd->surface->width > 0 && cmd->surface->height > 0) {
                if (cmd->opacity >= 255u) {
                    bool smooth = (local_r.width != cmd->surface->width ||
                                   local_r.height != cmd->surface->height);
                    arcr_accel_blit_image_argb(job->accel, win_dst, win_w, win_h, job->stride_px,
                                               local_r, cmd->surface->argb_pixels,
                                               cmd->surface->width, cmd->surface->height, smooth);
                } else {
                    blit_image_with_opacity(win_dst, win_w, win_h, job->stride_px, local_r,
                                            cmd->surface->argb_pixels,
                                            cmd->surface->width, cmd->surface->height,
                                            cmd->opacity);
                }
            }
            break;
        }
        case ARCR_DCMD_GLYPH_BATCH: {
            if (!job->fonts || !cmd->glyphs) break;
            for (uint32_t g = 0u; g < cmd->glyph_count; ++g) {
                const ArcrPositionedGlyph *pg = &cmd->glyphs[g];
                if (pg->codepoint == ' ' || pg->codepoint == '\t' || pg->codepoint == 0x00A0u) {
                    continue;
                }

                int32_t approx_lx = job->viewport_rect.x + pg->x - draw_win.x;
                int32_t approx_ly = job->viewport_rect.y + pg->baseline_y - y_scroll - draw_win.y;
                int32_t reach     = pg->font_px + 10;
                if (approx_lx + reach * 2 < 0 || approx_lx - reach > win_w ||
                    approx_ly + reach < 0 || approx_ly - reach * 2 > win_h) {
                    continue;
                }

                ArcrGlyphMetrics gm;
                const uint8_t *alpha_ptr = arcr_font_get_cached_glyph_ptr(
                    job->fonts, pg->codepoint, pg->font_px, pg->bold, pg->monospace,
                    &gm, false);
                if (!alpha_ptr || gm.width <= 0 || gm.height <= 0) continue;

                int32_t gx = approx_lx + gm.bearing_x;
                int32_t gy = approx_ly - gm.bearing_y;
                if (gx + gm.width <= 0 || gx >= win_w ||
                    gy + gm.height <= 0 || gy >= win_h) {
                    continue;
                }

                uint32_t gcol = scale_argb_opacity(pg->color_argb, cmd->opacity);
                arcr_accel_blit_glyph_alpha(job->accel, win_dst, win_w, win_h,
                                            job->stride_px, gx, gy, alpha_ptr,
                                            gm.width, gm.height, gcol);
            }
            break;
        }
    }
}

static void render_tile_worker(void *userdata, uint32_t tile_idx, uint32_t worker_id) {
    (void)worker_id;
    const TileRasterJob *job = (const TileRasterJob *)userdata;

    uint32_t tx = tile_idx % job->tiles_x;
    uint32_t ty = tile_idx / job->tiles_x;

    int32_t tile_x = job->eff_vp.x + (int32_t)tx * ARCR_TILE_SIZE_PX;
    int32_t tile_y = job->eff_vp.y + (int32_t)ty * ARCR_TILE_SIZE_PX;
    int32_t tile_w = ARCR_MIN(ARCR_TILE_SIZE_PX, (job->eff_vp.x + job->eff_vp.width) - tile_x);
    int32_t tile_h = ARCR_MIN(ARCR_TILE_SIZE_PX, (job->eff_vp.y + job->eff_vp.height) - tile_y);
    if (tile_w <= 0 || tile_h <= 0) return;

    uint32_t *tile_dst = job->dst_argb + (size_t)tile_y * (size_t)job->stride_px + (size_t)tile_x;
    ArcrRect  tile_screen_r = {tile_x, tile_y, tile_w, tile_h};

    uint32_t clear_c = job->dl ? job->dl->default_bg_argb : 0xFFFFFFFFu;
    arcr_accel_fill_rect(job->accel, tile_dst, tile_w, tile_h, job->stride_px,
                         (ArcrRect){0, 0, tile_w, tile_h}, clear_c);

    const ArcrDisplayList *dl = job->dl;
    if (!dl || dl->count == 0u) return;

    /* Fallback if spatial bands were not constructed */
    if (!dl->doc_bands || dl->doc_band_count == 0u) {
        for (uint32_t i = 0u; i < dl->count; ++i) {
            execute_cmd_on_tile(job, &dl->cmds[i], tile_screen_r);
        }
        return;
    }

    /* Map tile screen Y-span to at most 2 document 64px bands + fixed_cmd_indices */
    int32_t doc_y0 = job->scroll_y + (tile_y - job->viewport_rect.y);
    int32_t doc_y1 = doc_y0 + tile_h - 1;

    const uint32_t *list0 = NULL;
    uint32_t        len0  = 0u;
    const uint32_t *list1 = NULL;
    uint32_t        len1  = 0u;

    if (doc_y1 >= 0) {
        uint32_t b0 = (uint32_t)(ARCR_MAX(0, doc_y0) / ARCR_TILE_SIZE_PX);
        uint32_t b1 = (uint32_t)(doc_y1 / ARCR_TILE_SIZE_PX);
        if (b0 < dl->doc_band_count) {
            list0 = dl->doc_bands[b0].cmd_indices;
            len0  = dl->doc_bands[b0].count;
        }
        if (b1 != b0 && b1 < dl->doc_band_count) {
            list1 = dl->doc_bands[b1].cmd_indices;
            len1  = dl->doc_bands[b1].count;
        }
    }

    const uint32_t *listF = dl->fixed_cmd_indices;
    uint32_t        lenF  = dl->fixed_cmd_count;

    /* 3-way sorted merge preserves exact painter's Z-order across doc & fixed commands! */
    uint32_t p0 = 0u, p1 = 0u, pF = 0u;
    while (p0 < len0 || p1 < len1 || pF < lenF) {
        uint32_t v0 = (p0 < len0) ? list0[p0] : UINT32_MAX;
        uint32_t v1 = (p1 < len1) ? list1[p1] : UINT32_MAX;
        uint32_t vF = (pF < lenF) ? listF[pF] : UINT32_MAX;

        uint32_t next_idx = ARCR_MIN(v0, ARCR_MIN(v1, vF));
        if (v0 == next_idx) ++p0;
        if (v1 == next_idx) ++p1;
        if (vF == next_idx) ++pF;

        if (next_idx < dl->count) {
            execute_cmd_on_tile(job, &dl->cmds[next_idx], tile_screen_r);
        }
    }
}

static void prewarm_cmd_glyphs_for_viewport(const ArcrDisplayCmd *cmd,
                                            ArcrFontSystem *fonts,
                                            ArcrRect viewport_rect,
                                            ArcrRect eff_vp,
                                            int32_t scroll_y) {
    if (cmd->kind != ARCR_DCMD_GLYPH_BATCH || !cmd->glyphs || cmd->opacity == 0u) return;
    int32_t y_scroll = cmd->is_fixed ? 0 : scroll_y;
    ArcrRect screen_r = {
        .x      = viewport_rect.x + cmd->bounds.x,
        .y      = viewport_rect.y + cmd->bounds.y - y_scroll,
        .width  = cmd->bounds.width,
        .height = cmd->bounds.height
    };
    if (!cmd_intersects_screen_rect(cmd, screen_r, eff_vp)) return;

    int32_t vx0 = eff_vp.x - 64;
    int32_t vx1 = eff_vp.x + eff_vp.width + 64;
    int32_t vy0 = eff_vp.y - 64;
    int32_t vy1 = eff_vp.y + eff_vp.height + 64;

    for (uint32_t g = 0u; g < cmd->glyph_count; ++g) {
        const ArcrPositionedGlyph *pg = &cmd->glyphs[g];
        if (pg->codepoint == ' ' || pg->codepoint == '\t' || pg->codepoint == 0x00A0u) continue;
        int32_t sx = viewport_rect.x + pg->x;
        int32_t sy = viewport_rect.y + pg->baseline_y - y_scroll;
        if (sx < vx0 - pg->font_px || sx > vx1 + pg->font_px ||
            sy < vy0 - pg->font_px || sy > vy1 + pg->font_px * 2) {
            continue;
        }
        ArcrGlyphMetrics gm;
        (void)arcr_font_get_cached_glyph_ptr(
            fonts, pg->codepoint, pg->font_px, pg->bold, pg->monospace, &gm, true);
    }
}

static void prewarm_viewport_glyphs(const ArcrDisplayList *dl,
                                    ArcrFontSystem *fonts,
                                    ArcrRect viewport_rect,
                                    ArcrRect eff_vp,
                                    int32_t scroll_y) {
    if (!dl || !fonts || dl->count == 0u) return;

    if (!dl->doc_bands || dl->doc_band_count == 0u) {
        for (uint32_t i = 0u; i < dl->count; ++i) {
            prewarm_cmd_glyphs_for_viewport(&dl->cmds[i], fonts, viewport_rect, eff_vp, scroll_y);
        }
        return;
    }

    /* Pre-warm fixed glyph batches */
    for (uint32_t f = 0u; f < dl->fixed_cmd_count; ++f) {
        uint32_t idx = dl->fixed_cmd_indices[f];
        if (idx < dl->count) {
            prewarm_cmd_glyphs_for_viewport(&dl->cmds[idx], fonts, viewport_rect, eff_vp, scroll_y);
        }
    }

    /* Pre-warm visible document Y-bands */
    int32_t doc_y0 = scroll_y + (eff_vp.y - viewport_rect.y) - ARCR_TILE_SIZE_PX;
    int32_t doc_y1 = scroll_y + (eff_vp.y + eff_vp.height - viewport_rect.y) + ARCR_TILE_SIZE_PX;
    if (doc_y1 < 0) return;

    uint32_t b0 = (uint32_t)(ARCR_MAX(0, doc_y0) / ARCR_TILE_SIZE_PX);
    uint32_t b1 = (uint32_t)(doc_y1 / ARCR_TILE_SIZE_PX);
    if (b0 >= dl->doc_band_count) return;
    if (b1 >= dl->doc_band_count) b1 = dl->doc_band_count - 1u;

    for (uint32_t b = b0; b <= b1; ++b) {
        const ArcrTileBandBin *bin = &dl->doc_bands[b];
        for (uint32_t k = 0u; k < bin->count; ++k) {
            uint32_t idx = bin->cmd_indices[k];
            if (idx < dl->count && dl->cmds[idx].kind == ARCR_DCMD_GLYPH_BATCH) {
                /* Only pre-warm on first overlapping band to avoid redundant work */
                ArcrRect cr;
                if (compute_cmd_cull_rect(&dl->cmds[idx], &cr)) {
                    uint32_t first_b = (uint32_t)(ARCR_MAX(0, cr.y) / ARCR_TILE_SIZE_PX);
                    if (b > b0 && first_b < b) continue;
                }
                prewarm_cmd_glyphs_for_viewport(&dl->cmds[idx], fonts,
                                                viewport_rect, eff_vp, scroll_y);
            }
        }
    }
}

uint32_t arcr_paint_rasterize_parallel(const ArcrDisplayList *dl,
                                       ArcrAccelContext      *accel,
                                       ArcrFontSystem        *fonts,
                                       ArcrThreadPool        *pool,
                                       uint32_t              *dst_argb,
                                       int32_t                fb_w,
                                       int32_t                fb_h,
                                       int32_t                stride_px,
                                       ArcrRect               viewport_rect,
                                       int32_t                scroll_y) {
    if (!dst_argb || fb_w <= 0 || fb_h <= 0 || stride_px < fb_w) return 0u;
    if (viewport_rect.width <= 0 || viewport_rect.height <= 0) return 0u;

    ArcrRect eff_vp;
    if (!rects_intersect(viewport_rect, (ArcrRect){0, 0, fb_w, fb_h}, &eff_vp)) {
        return 0u;
    }

    uint32_t tiles_x    = (uint32_t)((eff_vp.width + ARCR_TILE_SIZE_PX - 1) / ARCR_TILE_SIZE_PX);
    uint32_t tiles_y    = (uint32_t)((eff_vp.height + ARCR_TILE_SIZE_PX - 1) / ARCR_TILE_SIZE_PX);
    uint32_t tile_count = tiles_x * tiles_y;
    if (tile_count == 0u) return 0u;

    /* Pre-warm all visible glyphs on main thread so parallel tile workers run 100% lock-free! */
    prewarm_viewport_glyphs(dl, fonts, viewport_rect, eff_vp, scroll_y);

    TileRasterJob job = {
        .dl            = dl,
        .accel         = accel,
        .fonts         = fonts,
        .dst_argb      = dst_argb,
        .stride_px     = stride_px,
        .viewport_rect = viewport_rect,
        .eff_vp        = eff_vp,
        .scroll_y      = scroll_y,
        .tiles_x       = tiles_x,
        .tiles_y       = tiles_y
    };

    if (pool) {
        arcr_threadpool_parallel_for(pool, tile_count, render_tile_worker, &job);
    } else {
        for (uint32_t t = 0u; t < tile_count; ++t) {
            render_tile_worker(&job, t, 0u);
        }
    }
    return tile_count;
}

void arcr_paint_rasterize(const ArcrDisplayList *dl,
                          ArcrAccelContext      *accel,
                          ArcrFontSystem        *fonts,
                          uint32_t              *dst_argb,
                          int32_t                fb_w,
                          int32_t                fb_h,
                          int32_t                stride_px,
                          ArcrRect               viewport_rect,
                          int32_t                scroll_y) {
    (void)arcr_paint_rasterize_parallel(dl, accel, fonts, NULL, dst_argb,
                                        fb_w, fb_h, stride_px, viewport_rect, scroll_y);
}
