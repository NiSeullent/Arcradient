/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * paint/arcr_paint.c - Display List Recorder, Find Highlights & Parallel 64x64 Tile Rasterizer
 */
#include "arcr_paint.h"
#include <stdlib.h>
#include <string.h>

static void dl_push(ArcrDisplayList *dl, ArcrDisplayCmd cmd) {
    if (dl->count >= dl->capacity) {
        uint32_t ncap = (dl->capacity == 0u) ? 256u : (dl->capacity * 2u);
        ArcrDisplayCmd *grown = (ArcrDisplayCmd *)realloc(
            dl->cmds, (size_t)ncap * sizeof(ArcrDisplayCmd));
        if (!grown) return;
        dl->cmds     = grown;
        dl->capacity = ncap;
    }
    dl->cmds[dl->count++] = cmd;
}

static ArcrRect compute_glyph_batch_bounds(const ArcrLayoutBox *box) {
    if (!box || box->glyph_count == 0u || !box->glyphs) return box->content_rect;
    int32_t min_x = box->glyphs[0].x;
    int32_t max_x = box->glyphs[0].x + box->glyphs[0].advance_px;
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

static void record_checkmark_icon(ArcrDisplayList *dl, ArcrRect br, int32_t radius_px) {
    if (br.width < 8 || br.height < 8) return;
    /* Radio-style circular dot when border_radius >= half width */
    if (radius_px * 2 >= br.width) {
        int32_t dot = ARCR_MAX(4, br.width / 2 - 2);
        dl_push(dl, (ArcrDisplayCmd){
            .kind       = ARCR_DCMD_ROUNDED_RECT,
            .bounds     = (ArcrRect){br.x + (br.width - dot) / 2,
                                     br.y + (br.height - dot) / 2, dot, dot},
            .radius_px  = dot / 2,
            .color_argb = 0xFFFFFFFFu
        });
        return;
    }

    /* Crisp 2px diagonal checkmark inside checkbox */
    int32_t cx = br.x + br.width / 2 - 1;
    int32_t cy = br.y + (br.height * 62) / 100;
    int32_t s1 = ARCR_MAX(2, br.width / 6);
    int32_t s2 = ARCR_MAX(4, br.width / 3);
    for (int32_t k = 0; k <= s1; ++k) {
        dl_push(dl, (ArcrDisplayCmd){
            .kind       = ARCR_DCMD_SOLID_RECT,
            .bounds     = (ArcrRect){cx - k, cy - k, 2, 2},
            .color_argb = 0xFFFFFFFFu
        });
    }
    for (int32_t k = 1; k <= s2; ++k) {
        dl_push(dl, (ArcrDisplayCmd){
            .kind       = ARCR_DCMD_SOLID_RECT,
            .bounds     = (ArcrRect){cx + k, cy - k, 2, 2},
            .color_argb = 0xFFFFFFFFu
        });
    }
}

static void record_underlines(ArcrDisplayList *dl, const ArcrLayoutBox *box) {
    uint32_t i = 0u;
    while (i < box->glyph_count) {
        const ArcrPositionedGlyph *g = &box->glyphs[i];
        if (!g->underline) {
            ++i;
            continue;
        }
        int32_t  x0        = g->x;
        int32_t  x1        = g->x + g->advance_px;
        int32_t  base_y    = g->baseline_y;
        int32_t  fpx       = g->font_px;
        uint32_t line_col  = g->color_argb;
        uint32_t j         = i + 1u;

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
            dl_push(dl, (ArcrDisplayCmd){
                .kind       = ARCR_DCMD_SOLID_RECT,
                .bounds     = (ArcrRect){x0, base_y + 2, x1 - x0, thickness},
                .color_argb = line_col
            });
        }
        i = j;
    }
}

static void record_box(ArcrDisplayList *dl, const ArcrLayoutBox *box) {
    if (!box || !box->style) return;
    const ArcrComputedStyle *st = box->style;
    ArcrRect br = box->border_rect;

    if (box->is_hr) {
        int32_t line_y = br.y + br.height / 2;
        dl_push(dl, (ArcrDisplayCmd){
            .kind       = ARCR_DCMD_SOLID_RECT,
            .bounds     = (ArcrRect){br.x, line_y, br.width, 1},
            .color_argb = 0xFFCBD5E1u
        });
        return;
    }

    bool is_focused = (box->dom_node && (box->dom_node->state_flags & ARCR_STATE_FOCUS) != 0u);
    bool is_checked = (box->dom_node && (box->dom_node->state_flags & ARCR_STATE_CHECKED) != 0u);

    /* 1. Elevation Box Shadow */
    if (st->shadow_blur > 0 && ((st->shadow_argb >> 24) & 0xFFu) > 0u) {
        dl_push(dl, (ArcrDisplayCmd){
            .kind              = ARCR_DCMD_BOX_SHADOW,
            .bounds            = br,
            .color_argb        = st->shadow_argb,
            .radius_px         = st->border_radius,
            .border_or_blur_px = st->shadow_blur,
            .offset_y          = st->shadow_offset_y
        });
    }

    /* 2. Focus Ring Halo for Interactive Form Controls */
    if (is_focused && box->control_kind != ARCR_CTRL_NONE) {
        dl_push(dl, (ArcrDisplayCmd){
            .kind              = ARCR_DCMD_ROUNDED_STROKE,
            .bounds            = (ArcrRect){br.x - 2, br.y - 2, br.width + 4, br.height + 4},
            .radius_px         = st->border_radius + 2,
            .border_or_blur_px = 2,
            .color_argb        = 0x553B82F6u
        });
    }

    /* 3. Box Background */
    uint32_t bg_c = st->bg_argb;
    if (box->control_kind == ARCR_CTRL_CHECKBOX && is_checked) {
        bg_c = 0xFF2563EBu;
    }
    if (((bg_c >> 24) & 0xFFu) > 0u && br.width > 0 && br.height > 0) {
        if (st->border_radius > 0) {
            dl_push(dl, (ArcrDisplayCmd){
                .kind       = ARCR_DCMD_ROUNDED_RECT,
                .bounds     = br,
                .radius_px  = st->border_radius,
                .color_argb = bg_c
            });
        } else {
            dl_push(dl, (ArcrDisplayCmd){
                .kind       = ARCR_DCMD_SOLID_RECT,
                .bounds     = br,
                .color_argb = bg_c
            });
        }
    }

    /* 4. Borders (Anti-aliased Rounded Stroke vs 4 Sharp Edge Rects) */
    uint32_t border_c = st->border_argb;
    if (box->control_kind == ARCR_CTRL_CHECKBOX && is_checked) {
        border_c = 0xFF1D4ED8u;
    } else if (is_focused && box->control_kind == ARCR_CTRL_TEXT_INPUT) {
        border_c = 0xFF2563EBu;
    }

    if (((border_c >> 24) & 0xFFu) > 0u) {
        int32_t max_bw = ARCR_MAX(ARCR_MAX(st->border.top, st->border.bottom),
                                  ARCR_MAX(st->border.left, st->border.right));
        if (st->border_radius > 0 && max_bw > 0) {
            dl_push(dl, (ArcrDisplayCmd){
                .kind              = ARCR_DCMD_ROUNDED_STROKE,
                .bounds            = br,
                .radius_px         = st->border_radius,
                .border_or_blur_px = max_bw,
                .color_argb        = border_c
            });
        } else {
            if (st->border.top > 0) {
                dl_push(dl, (ArcrDisplayCmd){
                    .kind       = ARCR_DCMD_SOLID_RECT,
                    .bounds     = (ArcrRect){br.x, br.y, br.width, st->border.top},
                    .color_argb = border_c
                });
            }
            if (st->border.bottom > 0) {
                dl_push(dl, (ArcrDisplayCmd){
                    .kind       = ARCR_DCMD_SOLID_RECT,
                    .bounds     = (ArcrRect){br.x, br.y + br.height - st->border.bottom,
                                             br.width, st->border.bottom},
                    .color_argb = border_c
                });
            }
            if (st->border.left > 0) {
                dl_push(dl, (ArcrDisplayCmd){
                    .kind       = ARCR_DCMD_SOLID_RECT,
                    .bounds     = (ArcrRect){br.x, br.y, st->border.left, br.height},
                    .color_argb = border_c
                });
            }
            if (st->border.right > 0) {
                dl_push(dl, (ArcrDisplayCmd){
                    .kind       = ARCR_DCMD_SOLID_RECT,
                    .bounds     = (ArcrRect){br.x + br.width - st->border.right, br.y,
                                             st->border.right, br.height},
                    .color_argb = border_c
                });
            }
        }
    }

    /* 5. Checked Checkbox / Radio Indicator */
    if (box->control_kind == ARCR_CTRL_CHECKBOX && is_checked) {
        record_checkmark_icon(dl, br, st->border_radius);
    }

    /* 6. Decoded <img> / <canvas> Bitmap Blit (or Unloaded Image Placeholder Frame) */
    if (box->dom_node && box->dom_node->surface && box->dom_node->surface->argb_pixels &&
        box->dom_node->surface->width > 0 && box->dom_node->surface->height > 0) {
        ArcrRect dst_r = (box->content_rect.width > 0 && box->content_rect.height > 0)
                             ? box->content_rect
                             : br;
        dl_push(dl, (ArcrDisplayCmd){
            .kind    = ARCR_DCMD_IMAGE_BLIT,
            .bounds  = dst_r,
            .surface = box->dom_node->surface
        });
    } else if (box->dom_node && !box->dom_node->first_child &&
               box->control_kind == ARCR_CTRL_NONE &&
               st->display == ARCR_DISPLAY_INLINE_BLOCK &&
               ((bg_c >> 24) & 0xFFu) == 0u &&
               box->content_rect.width >= 16 && box->content_rect.height >= 16) {
        dl_push(dl, (ArcrDisplayCmd){
            .kind       = ARCR_DCMD_ROUNDED_RECT,
            .bounds     = box->content_rect,
            .radius_px  = 4,
            .color_argb = 0xFFF1F5F9u
        });
        dl_push(dl, (ArcrDisplayCmd){
            .kind              = ARCR_DCMD_ROUNDED_STROKE,
            .bounds            = box->content_rect,
            .radius_px         = 4,
            .border_or_blur_px = 1,
            .color_argb        = 0xFFCBD5E1u
        });
    }

    /* 7. Unordered List Bullet Marker (<ul>; <ol> numbers are child glyph boxes) */
    if (box->is_list_item && box->list_ordinal == 0) {
        int32_t dot_sz = ARCR_CLAMP(st->font_px / 3, 5, 8);
        int32_t dot_x  = box->content_rect.x - dot_sz * 2 - 4;
        int32_t dot_y  = box->content_rect.y + (st->font_px * 52) / 100;
        dl_push(dl, (ArcrDisplayCmd){
            .kind       = ARCR_DCMD_ROUNDED_RECT,
            .bounds     = (ArcrRect){dot_x, dot_y, dot_sz, dot_sz},
            .radius_px  = dot_sz / 2,
            .color_argb = st->color_argb
        });
    }

    /* 8. Inline Background Pills (e.g., <code>, <mark>) */
    for (const ArcrInlineBgPill *p = box->pills_head; p != NULL; p = p->next) {
        if (p->radius > 0) {
            dl_push(dl, (ArcrDisplayCmd){
                .kind       = ARCR_DCMD_ROUNDED_RECT,
                .bounds     = p->rect,
                .radius_px  = p->radius,
                .color_argb = p->bg_argb
            });
        } else {
            dl_push(dl, (ArcrDisplayCmd){
                .kind       = ARCR_DCMD_SOLID_RECT,
                .bounds     = p->rect,
                .color_argb = p->bg_argb
            });
        }
    }

    /* 9. Text Underlines & Positioned Glyph Batch */
    if (box->glyph_count > 0u && box->glyphs) {
        record_underlines(dl, box);
        dl_push(dl, (ArcrDisplayCmd){
            .kind        = ARCR_DCMD_GLYPH_BATCH,
            .bounds      = compute_glyph_batch_bounds(box),
            .glyphs      = box->glyphs,
            .glyph_count = box->glyph_count
        });
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
        dl_push(dl, (ArcrDisplayCmd){
            .kind       = ARCR_DCMD_SOLID_RECT,
            .bounds     = (ArcrRect){caret_x, caret_y, 2, caret_h},
            .color_argb = 0xFF2563EBu
        });
    }

    for (const ArcrLayoutBox *ch = box->first_child; ch != NULL; ch = ch->next_sibling) {
        record_box(dl, ch);
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

    if (box->dom_node != NULL && box->glyph_count >= q_len && box->glyphs) {
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

                    /* Emit highlight span(s) grouped by wrapped baseline_y */
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
                        dl_push(dl, (ArcrDisplayCmd){
                            .kind       = ARCR_DCMD_ROUNDED_RECT,
                            .bounds     = (ArcrRect){x0 - 1, hy, ARCR_MAX(4, x1 - x0 + 2), hh},
                            .radius_px  = 3,
                            .color_argb = hl_color
                        });
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
    arcr_arena_init(&dl->arena, 8192u);

    dl->default_bg_argb = (layout->root_box && layout->root_box->style &&
                           ((layout->root_box->style->bg_argb >> 24) & 0xFFu) > 0u)
                              ? layout->root_box->style->bg_argb
                              : 0xFFFFFFFFu;
    if (layout->root_box) {
        record_box(dl, layout->root_box);
        if (find_query_utf8 && find_query_utf8[0] != '\0') {
            append_find_highlights(dl, layout, find_query_utf8, active_find_index,
                                   out_total_find_matches, out_active_match_doc_y);
        }
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
 * Zero-Copy Multi-Core 64x64 Tile-Binned Parallel SIMD Rasterizer
 * ------------------------------------------------------------------------- */
static bool rects_intersect(ArcrRect a, ArcrRect b, ArcrRect *out) {
    int32_t x0 = ARCR_MAX(a.x, b.x);
    int32_t y0 = ARCR_MAX(a.y, b.y);
    int32_t x1 = ARCR_MIN(a.x + a.width, b.x + b.width);
    int32_t y1 = ARCR_MIN(a.y + a.height, b.y + b.height);
    if (x0 >= x1 || y0 >= y1) return false;
    if (out) *out = (ArcrRect){x0, y0, x1 - x0, y1 - y0};
    return true;
}

typedef struct TileRasterJob {
    const ArcrDisplayList *dl;
    ArcrAccelContext      *accel;
    ArcrFontSystem        *fonts;
    uint32_t              *dst_argb;
    int32_t                stride_px;
    ArcrRect               viewport_rect; /* Unclipped viewport origin for coord mapping */
    ArcrRect               eff_vp;        /* Clipped strictly inside [0, fb_w) x [0, fb_h) */
    int32_t                scroll_y;
    uint32_t               tiles_x;
    uint32_t               tiles_y;
} TileRasterJob;

static bool cmd_intersects_tile(const ArcrDisplayCmd *cmd, ArcrRect screen_r, ArcrRect tile_r) {
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
        cull_r.x      -= 2;
        cull_r.y      -= 2;
        cull_r.width  += 4;
        cull_r.height += 4;
    } else if (cmd->kind == ARCR_DCMD_GLYPH_BATCH) {
        cull_r.x      -= 8;
        cull_r.y      -= 8;
        cull_r.width  += 16;
        cull_r.height += 16;
    }
    return rects_intersect(cull_r, tile_r, NULL);
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

    /*
     * Zero-copy disjoint tile view:
     * `tile_dst` points to `(tile_x, tile_y)` inside `dst_argb` with `(tile_w, tile_h)` bounds
     * and the full framebuffer `stride_px`. Every `arcr_accel_*` kernel clips strictly to
     * `[0, tile_w) x [0, tile_h)`, making cross-thread pixel collisions mathematically impossible!
     */
    uint32_t *tile_dst = job->dst_argb + (size_t)tile_y * (size_t)job->stride_px + (size_t)tile_x;
    ArcrRect  tile_screen_r = {tile_x, tile_y, tile_w, tile_h};

    uint32_t clear_c = job->dl ? job->dl->default_bg_argb : 0xFFFFFFFFu;
    arcr_accel_fill_rect(job->accel, tile_dst, tile_w, tile_h, job->stride_px,
                         (ArcrRect){0, 0, tile_w, tile_h}, clear_c);
    if (!job->dl) return;

    uint8_t glyph_alpha[96 * 96];

    for (uint32_t i = 0u; i < job->dl->count; ++i) {
        const ArcrDisplayCmd *cmd = &job->dl->cmds[i];
        ArcrRect screen_r = {
            .x      = job->viewport_rect.x + cmd->bounds.x,
            .y      = job->viewport_rect.y + cmd->bounds.y - job->scroll_y,
            .width  = cmd->bounds.width,
            .height = cmd->bounds.height
        };
        if (!cmd_intersects_tile(cmd, screen_r, tile_screen_r)) continue;

        ArcrRect local_r = {
            .x      = screen_r.x - tile_x,
            .y      = screen_r.y - tile_y,
            .width  = screen_r.width,
            .height = screen_r.height
        };

        switch (cmd->kind) {
            case ARCR_DCMD_SOLID_RECT: {
                uint32_t alpha = (cmd->color_argb >> 24) & 0xFFu;
                if (alpha == 255u) {
                    arcr_accel_fill_rect(job->accel, tile_dst, tile_w, tile_h, job->stride_px,
                                         local_r, cmd->color_argb);
                } else if (alpha > 0u) {
                    arcr_accel_blend_rect(job->accel, tile_dst, tile_w, tile_h, job->stride_px,
                                          local_r, cmd->color_argb);
                }
                break;
            }
            case ARCR_DCMD_ROUNDED_RECT: {
                arcr_accel_fill_rounded_rect(job->accel, tile_dst, tile_w, tile_h, job->stride_px,
                                             local_r, cmd->radius_px, cmd->color_argb);
                break;
            }
            case ARCR_DCMD_ROUNDED_STROKE: {
                arcr_accel_stroke_rounded_rect(job->accel, tile_dst, tile_w, tile_h, job->stride_px,
                                               local_r, cmd->radius_px, cmd->border_or_blur_px,
                                               cmd->color_argb);
                break;
            }
            case ARCR_DCMD_BOX_SHADOW: {
                arcr_accel_draw_box_shadow(job->accel, tile_dst, tile_w, tile_h, job->stride_px,
                                           local_r, cmd->radius_px, cmd->border_or_blur_px,
                                           cmd->offset_y, cmd->color_argb);
                break;
            }
            case ARCR_DCMD_IMAGE_BLIT: {
                if (cmd->surface && cmd->surface->argb_pixels &&
                    cmd->surface->width > 0 && cmd->surface->height > 0) {
                    bool smooth = (local_r.width != cmd->surface->width ||
                                   local_r.height != cmd->surface->height);
                    arcr_accel_blit_image_argb(job->accel, tile_dst, tile_w, tile_h, job->stride_px,
                                               local_r, cmd->surface->argb_pixels,
                                               cmd->surface->width, cmd->surface->height, smooth);
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

                    /* Coarse per-glyph tile reject before touching font glyph cache */
                    int32_t approx_lx = job->viewport_rect.x + pg->x - tile_x;
                    int32_t approx_ly = job->viewport_rect.y + pg->baseline_y -
                                        job->scroll_y - tile_y;
                    int32_t reach     = pg->font_px + 10;
                    if (approx_lx + reach * 2 < 0 || approx_lx - reach > tile_w ||
                        approx_ly + reach < 0 || approx_ly - reach * 2 > tile_h) {
                        continue;
                    }

                    ArcrGlyphMetrics gm;
                    if (!arcr_font_rasterize_glyph_ex(job->fonts, pg->codepoint, pg->font_px,
                                                      pg->bold, pg->monospace, &gm,
                                                      glyph_alpha, sizeof(glyph_alpha))) {
                        continue;
                    }
                    if (gm.width <= 0 || gm.height <= 0) continue;

                    int32_t gx = approx_lx + gm.bearing_x;
                    int32_t gy = approx_ly - gm.bearing_y;
                    if (gx + gm.width <= 0 || gx >= tile_w ||
                        gy + gm.height <= 0 || gy >= tile_h) {
                        continue;
                    }
                    arcr_accel_blit_glyph_alpha(job->accel, tile_dst, tile_w, tile_h,
                                                job->stride_px, gx, gy, glyph_alpha,
                                                gm.width, gm.height, pg->color_argb);
                }
                break;
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

    uint32_t tiles_x = (uint32_t)((eff_vp.width + ARCR_TILE_SIZE_PX - 1) / ARCR_TILE_SIZE_PX);
    uint32_t tiles_y = (uint32_t)((eff_vp.height + ARCR_TILE_SIZE_PX - 1) / ARCR_TILE_SIZE_PX);
    uint32_t tile_count = tiles_x * tiles_y;
    if (tile_count == 0u) return 0u;

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
