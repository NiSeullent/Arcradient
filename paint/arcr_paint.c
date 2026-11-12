/*
 * Arcradient (MSHTML-Next)
 * paint/arcr_paint.c - Display List Recording, Culling & CPU/SIMD Rasterization
 */
#include "arcr_paint.h"
#include <stdlib.h>
#include <string.h>

static void dl_push(ArcrDisplayList *dl, ArcrDisplayCmd cmd) {
    if (dl->count >= dl->capacity) {
        uint32_t ncap = (dl->capacity == 0u) ? 256u : (dl->capacity * 2u);
        ArcrDisplayCmd *grown = (ArcrDisplayCmd *)realloc(dl->cmds, ncap * sizeof(ArcrDisplayCmd));
        if (!grown) return;
        dl->cmds     = grown;
        dl->capacity = ncap;
    }
    dl->cmds[dl->count++] = cmd;
}

static void record_box(ArcrDisplayList *dl, const ArcrLayoutBox *box) {
    if (!box || !box->style) return;
    const ArcrComputedStyle *st = box->style;
    ArcrRect br = box->border_rect;

    if (box->is_hr) {
        dl_push(dl, (ArcrDisplayCmd){
            .kind       = ARCR_DCMD_SOLID_RECT,
            .bounds     = (ArcrRect){br.x, br.y, br.width, 1},
            .color_argb = 0xFFCBD5E1u
        });
        return;
    }

    /* 1. Box Background */
    if (((st->bg_argb >> 24) & 0xFFu) > 0u) {
        dl_push(dl, (ArcrDisplayCmd){
            .kind       = ARCR_DCMD_SOLID_RECT,
            .bounds     = br,
            .color_argb = st->bg_argb
        });
    }

    /* 2. Borders (Top, Bottom, Left, Right) */
    if (((st->border_argb >> 24) & 0xFFu) > 0u) {
        if (st->border.top > 0) {
            dl_push(dl, (ArcrDisplayCmd){
                .kind       = ARCR_DCMD_SOLID_RECT,
                .bounds     = (ArcrRect){br.x, br.y, br.width, st->border.top},
                .color_argb = st->border_argb
            });
        }
        if (st->border.bottom > 0) {
            dl_push(dl, (ArcrDisplayCmd){
                .kind       = ARCR_DCMD_SOLID_RECT,
                .bounds     = (ArcrRect){br.x, br.y + br.height - st->border.bottom,
                                         br.width, st->border.bottom},
                .color_argb = st->border_argb
            });
        }
        if (st->border.left > 0) {
            dl_push(dl, (ArcrDisplayCmd){
                .kind       = ARCR_DCMD_SOLID_RECT,
                .bounds     = (ArcrRect){br.x, br.y, st->border.left, br.height},
                .color_argb = st->border_argb
            });
        }
        if (st->border.right > 0) {
            dl_push(dl, (ArcrDisplayCmd){
                .kind       = ARCR_DCMD_SOLID_RECT,
                .bounds     = (ArcrRect){br.x + br.width - st->border.right, br.y,
                                         st->border.right, br.height},
                .color_argb = st->border_argb
            });
        }
    }

    /* 3. List item bullet marker */
    if (box->is_list_item) {
        int32_t dot_sz = 5;
        int32_t dot_x  = box->content_rect.x - 11;
        int32_t dot_y  = box->content_rect.y + (st->font_px / 2) - 1;
        dl_push(dl, (ArcrDisplayCmd){
            .kind       = ARCR_DCMD_SOLID_RECT,
            .bounds     = (ArcrRect){dot_x, dot_y, dot_sz, dot_sz},
            .color_argb = st->color_argb
        });
    }

    /* 4. Inline background pills (e.g., <code> badges) */
    for (const ArcrInlineBgPill *p = box->pills_head; p != NULL; p = p->next) {
        dl_push(dl, (ArcrDisplayCmd){
            .kind       = ARCR_DCMD_SOLID_RECT,
            .bounds     = p->rect,
            .color_argb = p->bg_argb
        });
    }

    /* 5. Positioned Korean/English/CJK Glyph Batch */
    if (box->glyph_count > 0u && box->glyphs) {
        dl_push(dl, (ArcrDisplayCmd){
            .kind        = ARCR_DCMD_GLYPH_BATCH,
            .bounds      = box->content_rect,
            .glyphs      = box->glyphs,
            .glyph_count = box->glyph_count
        });
    }

    for (const ArcrLayoutBox *ch = box->first_child; ch != NULL; ch = ch->next_sibling) {
        record_box(dl, ch);
    }
}

ArcrDisplayList *arcr_paint_build_display_list(const ArcrLayoutTree *layout) {
    if (!layout) return NULL;
    ArcrDisplayList *dl = (ArcrDisplayList *)calloc(1u, sizeof(ArcrDisplayList));
    if (!dl) return NULL;
    arcr_arena_init(&dl->arena, 8192u);
    dl->default_bg_argb = (layout->root_box && layout->root_box->style &&
                           ((layout->root_box->style->bg_argb >> 24) & 0xFFu) > 0u)
                              ? layout->root_box->style->bg_argb
                              : 0xFFFFFFFFu;
    if (layout->root_box) {
        record_box(dl, layout->root_box);
    }
    return dl;
}

void arcr_paint_destroy_display_list(ArcrDisplayList *dl) {
    if (!dl) return;
    free(dl->cmds);
    arcr_arena_destroy(&dl->arena);
    free(dl);
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

void arcr_paint_rasterize(const ArcrDisplayList *dl,
                          ArcrAccelContext      *accel,
                          ArcrFontSystem        *fonts,
                          uint32_t              *dst_argb,
                          int32_t                fb_w,
                          int32_t                fb_h,
                          int32_t                stride_px,
                          ArcrRect               viewport_rect,
                          int32_t                scroll_y) {
    if (!dst_argb || viewport_rect.width <= 0 || viewport_rect.height <= 0) return;

    uint32_t clear_c = dl ? dl->default_bg_argb : 0xFFFFFFFFu;
    arcr_accel_fill_rect(accel, dst_argb, fb_w, fb_h, stride_px, viewport_rect, clear_c);
    if (!dl) return;

    uint8_t glyph_alpha[64 * 64];

    for (uint32_t i = 0u; i < dl->count; ++i) {
        const ArcrDisplayCmd *cmd = &dl->cmds[i];
        ArcrRect screen_r = {
            .x      = viewport_rect.x + cmd->bounds.x,
            .y      = viewport_rect.y + cmd->bounds.y - scroll_y,
            .width  = cmd->bounds.width,
            .height = cmd->bounds.height
        };

        if (cmd->kind == ARCR_DCMD_SOLID_RECT) {
            ArcrRect clipped;
            if (!rects_intersect(screen_r, viewport_rect, &clipped)) continue;
            uint32_t alpha = (cmd->color_argb >> 24) & 0xFFu;
            if (alpha == 255u) {
                arcr_accel_fill_rect(accel, dst_argb, fb_w, fb_h, stride_px,
                                     clipped, cmd->color_argb);
            } else if (alpha > 0u) {
                arcr_accel_blend_rect(accel, dst_argb, fb_w, fb_h, stride_px,
                                      clipped, cmd->color_argb);
            }
        } else if (cmd->kind == ARCR_DCMD_GLYPH_BATCH) {
            /* Viewport Y culling for off-screen text paragraphs */
            if (screen_r.y + screen_r.height + 24 < viewport_rect.y ||
                screen_r.y - 24 > viewport_rect.y + viewport_rect.height) {
                continue;
            }
            for (uint32_t g = 0u; g < cmd->glyph_count; ++g) {
                const ArcrPositionedGlyph *pg = &cmd->glyphs[g];
                ArcrGlyphMetrics gm;
                if (!arcr_font_rasterize_glyph(fonts, pg->codepoint, pg->font_px, pg->bold,
                                               &gm, glyph_alpha, sizeof(glyph_alpha))) {
                    continue;
                }
                if (gm.width <= 0 || gm.height <= 0) continue;

                int32_t gx = viewport_rect.x + pg->x + gm.bearing_x;
                int32_t gy = viewport_rect.y + pg->baseline_y - scroll_y - gm.bearing_y;

                /* Clip glyph to viewport_rect */
                if (gx + gm.width <= viewport_rect.x ||
                    gx >= viewport_rect.x + viewport_rect.width ||
                    gy + gm.height <= viewport_rect.y ||
                    gy >= viewport_rect.y + viewport_rect.height) {
                    continue;
                }
                arcr_accel_blit_glyph_alpha(accel, dst_argb, fb_w, fb_h, stride_px,
                                            gx, gy, glyph_alpha, gm.width, gm.height,
                                            pg->color_argb);
            }
        }
    }
}
