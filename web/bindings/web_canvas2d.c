/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * web/bindings/web_canvas2d.c - Pure C11 2D <canvas> & OffscreenCanvas Rasterizer
 *
 * Implements HTML5 CanvasRenderingContext2D & OffscreenCanvasRenderingContext2D:
 * - Backing surface synchronization with ArcrDomNode->surface (ARGB8888) & dom_dirty
 * - OffscreenCanvas standalone pixel buffers (node_uid >= 0x80000000u)
 * - Affine transforms (save, restore, translate, scale, rotate, transform, setTransform)
 * - Solid CSS colors (hex, rgb/rgba, hsl/hsla, oklch, named) + Linear & Radial Gradients
 * - Paths (beginPath, moveTo, lineTo, closePath, rect, roundRect, arc, ellipse,
 *   quadraticCurveTo, bezierCurveTo) with nonzero & evenodd scanline fill + thick stroke
 * - Anti-aliased Unicode/CJK glyph measurement & text rendering (fillText, strokeText, measureText)
 * - Pixel manipulation (getImageData, putImageData, drawImage) & PNG export (toDataURL, convertToBlob)
 *
 * Zero QuickJS dependency. Zero link dependency on arcr_engine.
 */
#include "web_host.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEB_OFFSCREEN_UID_BASE 0x80000000u
#define WEB_PI                 3.14159265358979323846

/* -------------------------------------------------------------------------
 * 1. Context State & Backing Surface Management
 * ------------------------------------------------------------------------- */
static void c2d_init_transform_defaults(CanvasTransform *t) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->a             = 1.0;
    t->b             = 0.0;
    t->c             = 0.0;
    t->d             = 1.0;
    t->e             = 0.0;
    t->f             = 0.0;
    t->fill_argb     = 0xFF000000u; /* HTML5 default #000000 */
    t->stroke_argb   = 0xFF000000u; /* HTML5 default #000000 */
    t->line_width    = 1.0;
    t->global_alpha  = 1.0;
    t->font_px       = 10;          /* HTML5 default "10px sans-serif" */
    t->font_bold     = false;
    t->font_mono     = false;
    t->text_align    = 0u;          /* start */
    t->text_baseline = 0u;          /* alphabetic */
}

static void c2d_reset_state(Canvas2DCtx *c) {
    if (!c) return;
    c2d_init_transform_defaults(&c->cur);
    c->stack_top      = 0;
    c->fill_grad_id   = -1;
    c->stroke_grad_id = -1;
    c->pt_count       = 0;
    c->sub_start_x    = 0.0f;
    c->sub_start_y    = 0.0f;
}

static Canvas2DCtx *c2d_find_ctx(const ArcrJsVm *vm, uint32_t uid) {
    if (!vm || uid == 0u) return NULL;
    for (Canvas2DCtx *c = vm->canvas_head; c != NULL; c = c->next) {
        if (c->node_uid == uid) return c;
    }
    return NULL;
}

static Canvas2DCtx *c2d_get_or_create_ctx(ArcrJsVm *vm, uint32_t uid, bool is_offscreen) {
    if (!vm || uid == 0u) return NULL;
    Canvas2DCtx *c = c2d_find_ctx(vm, uid);
    if (c) return c;

    c = (Canvas2DCtx *)calloc(1u, sizeof(Canvas2DCtx));
    if (!c) return NULL;
    c->node_uid     = uid;
    c->is_offscreen = is_offscreen || (uid >= WEB_OFFSCREEN_UID_BASE);
    c2d_reset_state(c);
    c->next         = vm->canvas_head;
    vm->canvas_head = c;
    return c;
}

static void c2d_read_dom_canvas_dims(const ArcrJsVm *vm, const ArcrDomNode *el,
                                     int32_t *out_w, int32_t *out_h) {
    int32_t w = 300;
    int32_t h = 150;
    if (vm && vm->host.doc && el && el->kind == ARCR_NODE_ELEMENT) {
        for (const ArcrAttr *a = el->first_attr; a != NULL; a = a->next) {
            const char *an = arcr_atom_cstr(vm->host.doc->atoms, a->name);
            if (!an || !a->value) continue;
            if (arcr_str_eq_nocase(an, "width")) {
                int32_t pv = 0;
                if (arcr_parse_i32(a->value, strlen(a->value), &pv) && pv > 0) w = pv;
            } else if (arcr_str_eq_nocase(an, "height")) {
                int32_t pv = 0;
                if (arcr_parse_i32(a->value, strlen(a->value), &pv) && pv > 0) h = pv;
            }
        }
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;
    *out_w = w;
    *out_h = h;
}

static ArcrDomSurface *c2d_ensure_offscreen_surf(Canvas2DCtx *c, int32_t w, int32_t h, bool force_clear) {
    if (!c) return NULL;
    if (w <= 0) w = (c->off_surf.width > 0) ? c->off_surf.width : 300;
    if (h <= 0) h = (c->off_surf.height > 0) ? c->off_surf.height : 150;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;

    if (c->off_surf.argb_pixels && c->off_surf.width == w && c->off_surf.height == h) {
        if (force_clear) {
            memset(c->off_surf.argb_pixels, 0, (size_t)w * (size_t)h * sizeof(uint32_t));
        }
        return &c->off_surf;
    }

    size_t count = (size_t)w * (size_t)h;
    uint32_t *px = (uint32_t *)calloc(count, sizeof(uint32_t));
    if (!px) return NULL;
    free(c->off_surf.argb_pixels);
    c->off_surf.argb_pixels = px;
    c->off_surf.width       = w;
    c->off_surf.height      = h;
    c->off_surf.heap_owned  = true;
    return &c->off_surf;
}

static ArcrDomSurface *c2d_get_surface(ArcrJsVm *vm, Canvas2DCtx *c, bool mark_dirty) {
    if (!vm || !c) return NULL;
    if (c->is_offscreen || c->node_uid >= WEB_OFFSCREEN_UID_BASE) {
        return c2d_ensure_offscreen_surf(c, 0, 0, false);
    }
    if (!vm->host.doc) return NULL;
    ArcrDomNode *el = web_dom_by_uid(vm, c->node_uid);
    if (!el || el->kind != ARCR_NODE_ELEMENT) return NULL;

    ArcrDomSurface *surf = el->surface;
    if (!surf || !surf->argb_pixels || surf->width <= 0 || surf->height <= 0) {
        int32_t w = 300, h = 150;
        c2d_read_dom_canvas_dims(vm, el, &w, &h);
        surf = web_ensure_node_surface(vm, el, w, h);
    }
    if (surf && mark_dirty && vm->host.doc) {
        vm->host.doc->dom_dirty = true;
    }
    return surf;
}

/* -------------------------------------------------------------------------
 * 2. Pixel Compositing & Gradient Sampling
 * ------------------------------------------------------------------------- */
static inline uint32_t c2d_apply_alpha(uint32_t argb, double global_alpha) {
    if (global_alpha >= 0.999) return argb;
    if (global_alpha <= 0.001) return argb & 0x00FFFFFFu;
    uint32_t a = (argb >> 24) & 0xFFu;
    uint32_t na = (uint32_t)((double)a * global_alpha + 0.5);
    if (na > 255u) na = 255u;
    return (na << 24) | (argb & 0x00FFFFFFu);
}

static inline void c2d_blend_pixel(uint32_t *dst_ptr, uint32_t src_argb) {
    uint32_t sa = (src_argb >> 24) & 0xFFu;
    if (sa == 0u) return;
    if (sa == 255u) {
        *dst_ptr = src_argb;
        return;
    }
    uint32_t dst = *dst_ptr;
    uint32_t da  = (dst >> 24) & 0xFFu;
    if (da == 0u) {
        *dst_ptr = src_argb;
        return;
    }
    uint32_t sr = (src_argb >> 16) & 0xFFu;
    uint32_t sg = (src_argb >> 8)  & 0xFFu;
    uint32_t sb = src_argb         & 0xFFu;

    uint32_t dr = (dst >> 16) & 0xFFu;
    uint32_t dg = (dst >> 8)  & 0xFFu;
    uint32_t db = dst         & 0xFFu;

    uint32_t inv_sa = 255u - sa;
    /* Unpremultiplied Porter-Duff Source-Over with accurate alpha denominator */
    uint32_t out_a_255 = sa * 255u + da * inv_sa;
    uint32_t out_a     = (out_a_255 + 127u) / 255u;
    if (out_a == 0u) {
        *dst_ptr = 0u;
        return;
    }
    uint32_t nr = (sr * sa * 255u + dr * da * inv_sa + (out_a_255 >> 1)) / out_a_255;
    uint32_t ng = (sg * sa * 255u + dg * da * inv_sa + (out_a_255 >> 1)) / out_a_255;
    uint32_t nb = (sb * sa * 255u + db * da * inv_sa + (out_a_255 >> 1)) / out_a_255;
    if (nr > 255u) nr = 255u;
    if (ng > 255u) ng = 255u;
    if (nb > 255u) nb = 255u;
    if (out_a > 255u) out_a = 255u;
    *dst_ptr = (out_a << 24) | (nr << 16) | (ng << 8) | nb;
}

static uint32_t c2d_lerp_argb(uint32_t c0, uint32_t c1, double t) {
    if (t <= 0.0) return c0;
    if (t >= 1.0) return c1;
    double inv = 1.0 - t;
    uint32_t a = (uint32_t)(((double)((c0 >> 24) & 0xFFu) * inv) + ((double)((c1 >> 24) & 0xFFu) * t) + 0.5);
    uint32_t r = (uint32_t)(((double)((c0 >> 16) & 0xFFu) * inv) + ((double)((c1 >> 16) & 0xFFu) * t) + 0.5);
    uint32_t g = (uint32_t)(((double)((c0 >> 8)  & 0xFFu) * inv) + ((double)((c1 >> 8)  & 0xFFu) * t) + 0.5);
    uint32_t b = (uint32_t)(((double)(c0         & 0xFFu) * inv) + ((double)(c1         & 0xFFu) * t) + 0.5);
    if (a > 255u) a = 255u;
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static bool c2d_invert_ctm(const CanvasTransform *t,
                           double *ia, double *ib, double *ic, double *id, double *ie, double *ig) {
    double det = t->a * t->d - t->b * t->c;
    if (fabs(det) < 1e-12) return false;
    double inv = 1.0 / det;
    *ia =  t->d * inv;
    *ib = -t->b * inv;
    *ic = -t->c * inv;
    *id =  t->a * inv;
    *ie = (t->c * t->f - t->d * t->e) * inv;
    *ig = (t->b * t->e - t->a * t->f) * inv;
    return true;
}

static uint32_t c2d_sample_grad(const CanvasGrad *g, double ux, double uy) {
    if (!g || g->stop_count <= 0) return 0u;
    if (g->stop_count == 1) return g->stops[0].argb;

    double t = 0.0;
    if (!g->is_radial) {
        double dx = g->x1 - g->x0;
        double dy = g->y1 - g->y0;
        double len2 = dx * dx + dy * dy;
        if (len2 > 1e-9) {
            t = ((ux - g->x0) * dx + (uy - g->y0) * dy) / len2;
        }
    } else {
        double dx = ux - g->x1;
        double dy = uy - g->y1;
        double d  = sqrt(dx * dx + dy * dy);
        double dr = g->r1 - g->r0;
        t = (fabs(dr) > 1e-9) ? ((d - g->r0) / dr) : (d <= g->r1 ? 0.0 : 1.0);
    }

    if (t <= g->stops[0].offset) return g->stops[0].argb;
    if (t >= g->stops[g->stop_count - 1].offset) return g->stops[g->stop_count - 1].argb;

    for (int32_t i = 0; i + 1 < g->stop_count; ++i) {
        double o0 = g->stops[i].offset;
        double o1 = g->stops[i + 1].offset;
        if (t >= o0 && t <= o1) {
            double span = o1 - o0;
            double f = (span > 1e-9) ? ((t - o0) / span) : 0.0;
            return c2d_lerp_argb(g->stops[i].argb, g->stops[i + 1].argb, f);
        }
    }
    return g->stops[g->stop_count - 1].argb;
}

static inline void c2d_transform_pt(const CanvasTransform *t, double x, double y, float *ox, float *oy) {
    *ox = (float)(t->a * x + t->c * y + t->e);
    *oy = (float)(t->b * x + t->d * y + t->f);
}

/* -------------------------------------------------------------------------
 * 3. Polygon Fill (Nonzero & Even-Odd) & Thick Anti-Aliased Stroke
 * ------------------------------------------------------------------------- */
static void c2d_path_move_raw(Canvas2DCtx *c, float sx, float sy) {
    if (!c || c->pt_count >= ARCR_CANVAS_MAX_PTS) return;
    c->pts[c->pt_count].x       = sx;
    c->pts[c->pt_count].y       = sy;
    c->pts[c->pt_count].move_to = true;
    c->pt_count++;
    c->sub_start_x = sx;
    c->sub_start_y = sy;
}

static void c2d_path_line_raw(Canvas2DCtx *c, float sx, float sy) {
    if (!c || c->pt_count >= ARCR_CANVAS_MAX_PTS) return;
    if (c->pt_count == 0) {
        c2d_path_move_raw(c, sx, sy);
        return;
    }
    c->pts[c->pt_count].x       = sx;
    c->pts[c->pt_count].y       = sy;
    c->pts[c->pt_count].move_to = false;
    c->pt_count++;
}

typedef struct C2DEdge {
    float x0, y0, x1, y1;
} C2DEdge;

static int c2d_collect_closed_edges(const CanvasPt *pts, int32_t pt_count,
                                    C2DEdge *edges, int max_edges) {
    int ec = 0;
    int32_t sub_start = -1;
    for (int32_t i = 0; i < pt_count && ec < max_edges; ++i) {
        if (pts[i].move_to) {
            if (sub_start >= 0 && (i - 1) > sub_start && ec < max_edges) {
                edges[ec].x0 = pts[i - 1].x;
                edges[ec].y0 = pts[i - 1].y;
                edges[ec].x1 = pts[sub_start].x;
                edges[ec].y1 = pts[sub_start].y;
                ec++;
            }
            sub_start = i;
        } else if (i > 0) {
            edges[ec].x0 = pts[i - 1].x;
            edges[ec].y0 = pts[i - 1].y;
            edges[ec].x1 = pts[i].x;
            edges[ec].y1 = pts[i].y;
            ec++;
        }
    }
    if (sub_start >= 0 && (pt_count - 1) > sub_start && ec < max_edges) {
        edges[ec].x0 = pts[pt_count - 1].x;
        edges[ec].y0 = pts[pt_count - 1].y;
        edges[ec].x1 = pts[sub_start].x;
        edges[ec].y1 = pts[sub_start].y;
        ec++;
    }
    return ec;
}

typedef struct C2DIsect {
    float x;
    int   dir;
} C2DIsect;

static void c2d_sort_isects(C2DIsect *arr, int n) {
    for (int i = 1; i < n; ++i) {
        C2DIsect key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j].x > key.x) {
            arr[j + 1] = arr[j];
            --j;
        }
        arr[j + 1] = key;
    }
}

static void c2d_fill_pts(ArcrJsVm *vm, Canvas2DCtx *c, ArcrDomSurface *surf,
                         const CanvasPt *pts, int32_t pt_count, bool even_odd) {
    if (!c || !surf || !surf->argb_pixels || pt_count < 3) return;

    C2DEdge edges[ARCR_CANVAS_MAX_PTS + 16];
    int ec = c2d_collect_closed_edges(pts, pt_count, edges, (int)ARCR_ARRAY_LEN(edges));
    if (ec < 2) return;

    float min_y = edges[0].y0, max_y = edges[0].y0;
    for (int i = 0; i < ec; ++i) {
        if (edges[i].y0 < min_y) min_y = edges[i].y0;
        if (edges[i].y1 < min_y) min_y = edges[i].y1;
        if (edges[i].y0 > max_y) max_y = edges[i].y0;
        if (edges[i].y1 > max_y) max_y = edges[i].y1;
    }

    int32_t y_start = (int32_t)floor((double)min_y);
    int32_t y_end   = (int32_t)ceil((double)max_y);
    if (y_start < 0) y_start = 0;
    if (y_end > surf->height) y_end = surf->height;
    if (y_start >= y_end) return;

    const CanvasGrad *grad = NULL;
    if (vm && c->fill_grad_id >= 0 && c->fill_grad_id < vm->grad_count &&
        vm->grads[c->fill_grad_id].active) {
        grad = &vm->grads[c->fill_grad_id];
    }
    double ia = 1.0, ib = 0.0, ic = 0.0, id = 1.0, ie = 0.0, ig = 0.0;
    if (grad) {
        (void)c2d_invert_ctm(&c->cur, &ia, &ib, &ic, &id, &ie, &ig);
    }
    uint32_t solid = c2d_apply_alpha(c->cur.fill_argb, c->cur.global_alpha);
    if (!grad && ((solid >> 24) & 0xFFu) == 0u) return;

    C2DIsect isects[512];
    for (int32_t y = y_start; y < y_end; ++y) {
        float sy = (float)y + 0.5f;
        int icnt = 0;
        for (int i = 0; i < ec && icnt < (int)ARCR_ARRAY_LEN(isects); ++i) {
            float y0 = edges[i].y0;
            float y1 = edges[i].y1;
            if ((y0 <= sy && y1 > sy) || (y1 <= sy && y0 > sy)) {
                float t = (sy - y0) / (y1 - y0);
                isects[icnt].x   = edges[i].x0 + t * (edges[i].x1 - edges[i].x0);
                isects[icnt].dir = (y1 > y0) ? 1 : -1;
                icnt++;
            }
        }
        if (icnt < 2) continue;
        c2d_sort_isects(isects, icnt);

        uint32_t *row = surf->argb_pixels + (size_t)y * (size_t)surf->width;
        if (even_odd) {
            for (int i = 0; i + 1 < icnt; i += 2) {
                int32_t x0 = (int32_t)floor((double)isects[i].x + 0.5);
                int32_t x1 = (int32_t)floor((double)isects[i + 1].x + 0.5);
                if (x0 < 0) x0 = 0;
                if (x1 > surf->width) x1 = surf->width;
                for (int32_t x = x0; x < x1; ++x) {
                    uint32_t col = solid;
                    if (grad) {
                        double px = (double)x + 0.5, py = (double)y + 0.5;
                        double ux = ia * px + ic * py + ie;
                        double uy = ib * px + id * py + ig;
                        col = c2d_apply_alpha(c2d_sample_grad(grad, ux, uy), c->cur.global_alpha);
                    }
                    c2d_blend_pixel(&row[x], col);
                }
            }
        } else {
            int winding = 0;
            float span_start = 0.0f;
            for (int i = 0; i < icnt; ++i) {
                if (winding == 0) span_start = isects[i].x;
                winding += isects[i].dir;
                if (winding == 0) {
                    int32_t x0 = (int32_t)floor((double)span_start + 0.5);
                    int32_t x1 = (int32_t)floor((double)isects[i].x + 0.5);
                    if (x0 < 0) x0 = 0;
                    if (x1 > surf->width) x1 = surf->width;
                    for (int32_t x = x0; x < x1; ++x) {
                        uint32_t col = solid;
                        if (grad) {
                            double px = (double)x + 0.5, py = (double)y + 0.5;
                            double ux = ia * px + ic * py + ie;
                            double uy = ib * px + id * py + ig;
                            col = c2d_apply_alpha(c2d_sample_grad(grad, ux, uy), c->cur.global_alpha);
                        }
                        c2d_blend_pixel(&row[x], col);
                    }
                }
            }
        }
    }
}

static bool c2d_point_in_path(const Canvas2DCtx *c, float px, float py, bool even_odd) {
    if (!c || c->pt_count < 3) return false;
    C2DEdge edges[ARCR_CANVAS_MAX_PTS + 16];
    int ec = c2d_collect_closed_edges(c->pts, c->pt_count, edges, (int)ARCR_ARRAY_LEN(edges));
    int winding = 0;
    int crossings = 0;
    for (int i = 0; i < ec; ++i) {
        float y0 = edges[i].y0, y1 = edges[i].y1;
        if ((y0 <= py && y1 > py) || (y1 <= py && y0 > py)) {
            float t = (py - y0) / (y1 - y0);
            float ix = edges[i].x0 + t * (edges[i].x1 - edges[i].x0);
            if (px < ix) {
                crossings++;
                winding += (y1 > y0) ? 1 : -1;
            }
        }
    }
    return even_odd ? ((crossings & 1) != 0) : (winding != 0);
}

static void c2d_stroke_segment(ArcrJsVm *vm, Canvas2DCtx *c, ArcrDomSurface *surf,
                               float x0, float y0, float x1, float y1,
                               double half_w, const CanvasGrad *grad,
                               double ia, double ib, double ic, double id, double ie, double ig,
                               uint32_t solid) {
    float min_fx = (x0 < x1 ? x0 : x1) - (float)half_w - 1.5f;
    float max_fx = (x0 > x1 ? x0 : x1) + (float)half_w + 1.5f;
    float min_fy = (y0 < y1 ? y0 : y1) - (float)half_w - 1.5f;
    float max_fy = (y0 > y1 ? y0 : y1) + (float)half_w + 1.5f;

    int32_t ix0 = (int32_t)floor((double)min_fx);
    int32_t ix1 = (int32_t)ceil((double)max_fx);
    int32_t iy0 = (int32_t)floor((double)min_fy);
    int32_t iy1 = (int32_t)ceil((double)max_fy);

    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > surf->width) ix1 = surf->width;
    if (iy1 > surf->height) iy1 = surf->height;
    if (ix0 >= ix1 || iy0 >= iy1) return;

    double dx = (double)x1 - (double)x0;
    double dy = (double)y1 - (double)y0;
    double len2 = dx * dx + dy * dy;

    for (int32_t y = iy0; y < iy1; ++y) {
        uint32_t *row = surf->argb_pixels + (size_t)y * (size_t)surf->width;
        double py = (double)y + 0.5;
        for (int32_t x = ix0; x < ix1; ++x) {
            double px = (double)x + 0.5;
            double t = 0.0;
            if (len2 > 1e-8) {
                t = ((px - (double)x0) * dx + (py - (double)y0) * dy) / len2;
                if (t < 0.0) t = 0.0;
                else if (t > 1.0) t = 1.0;
            }
            double cx = (double)x0 + t * dx;
            double cy = (double)y0 + t * dy;
            double dist = sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
            double cov = (half_w + 0.5) - dist;
            if (cov <= 0.0) continue;
            if (cov > 1.0) cov = 1.0;

            uint32_t base_col = solid;
            if (grad && vm) {
                double ux = ia * px + ic * py + ie;
                double uy = ib * px + id * py + ig;
                base_col = c2d_apply_alpha(c2d_sample_grad(grad, ux, uy), c->cur.global_alpha);
            }
            uint32_t cov_col = (cov >= 0.999) ? base_col : c2d_apply_alpha(base_col, cov);
            c2d_blend_pixel(&row[x], cov_col);
        }
    }
}

static void c2d_stroke_pts(ArcrJsVm *vm, Canvas2DCtx *c, ArcrDomSurface *surf,
                           const CanvasPt *pts, int32_t pt_count) {
    if (!c || !surf || !surf->argb_pixels || pt_count < 2) return;

    double sx_scale = sqrt(c->cur.a * c->cur.a + c->cur.b * c->cur.b);
    double sy_scale = sqrt(c->cur.c * c->cur.c + c->cur.d * c->cur.d);
    double scale = 0.5 * (sx_scale + sy_scale);
    if (scale < 0.01) scale = 1.0;
    double lw = c->cur.line_width * scale;
    if (lw < 0.5) lw = 0.5;
    double half_w = lw * 0.5;

    const CanvasGrad *grad = NULL;
    if (vm && c->stroke_grad_id >= 0 && c->stroke_grad_id < vm->grad_count &&
        vm->grads[c->stroke_grad_id].active) {
        grad = &vm->grads[c->stroke_grad_id];
    }
    double ia = 1.0, ib = 0.0, ic = 0.0, id = 1.0, ie = 0.0, ig = 0.0;
    if (grad) {
        (void)c2d_invert_ctm(&c->cur, &ia, &ib, &ic, &id, &ie, &ig);
    }
    uint32_t solid = c2d_apply_alpha(c->cur.stroke_argb, c->cur.global_alpha);
    if (!grad && ((solid >> 24) & 0xFFu) == 0u) return;

    for (int32_t i = 1; i < pt_count; ++i) {
        if (pts[i].move_to) continue;
        c2d_stroke_segment(vm, c, surf,
                           pts[i - 1].x, pts[i - 1].y, pts[i].x, pts[i].y,
                           half_w, grad, ia, ib, ic, id, ie, ig, solid);
    }
}

/* -------------------------------------------------------------------------
 * 4. Font Parsing, Glyph Measurement & Text Rendering
 * ------------------------------------------------------------------------- */
static void c2d_parse_font_css(CanvasTransform *t, const char *font_str) {
    if (!t || !font_str) return;
    bool bold = false;
    bool mono = false;
    int32_t px = t->font_px > 0 ? t->font_px : 10;

    /* Case-insensitive scan for bold / weight >= 600, monospace, and <N>px / <N>pt */
    const char *p = font_str;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        const char *st = p;
        while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != '/') ++p;
        size_t len = (size_t)(p - st);
        if (len > 0u) {
            if (arcr_str_eq_nocase_n(st, len, "bold", 4u) ||
                arcr_str_eq_nocase_n(st, len, "bolder", 6u) ||
                arcr_str_eq_nocase_n(st, len, "600", 3u) ||
                arcr_str_eq_nocase_n(st, len, "700", 3u) ||
                arcr_str_eq_nocase_n(st, len, "800", 3u) ||
                arcr_str_eq_nocase_n(st, len, "900", 3u)) {
                bold = true;
            } else if (arcr_str_eq_nocase_n(st, len, "monospace", 9u) ||
                       arcr_str_eq_nocase_n(st, len, "consolas", 8u) ||
                       arcr_str_eq_nocase_n(st, len, "courier", 7u) ||
                       arcr_str_eq_nocase_n(st, len, "menlo", 5u)) {
                mono = true;
            } else if (isdigit((unsigned char)st[0]) || (st[0] == '.' && len > 1u)) {
                char *endp = NULL;
                double val = strtod(st, &endp);
                if (endp && endp > st && val > 0.0) {
                    size_t ulen = (size_t)((st + len) - endp);
                    if (ulen >= 2u && arcr_str_eq_nocase_n(endp, 2u, "px", 2u)) {
                        px = (int32_t)(val + 0.5);
                    } else if (ulen >= 2u && arcr_str_eq_nocase_n(endp, 2u, "pt", 2u)) {
                        px = (int32_t)(val * (96.0 / 72.0) + 0.5);
                    } else if (ulen >= 2u && arcr_str_eq_nocase_n(endp, 2u, "rem", 3u)) {
                        px = (int32_t)(val * 16.0 + 0.5);
                    } else if (ulen >= 2u && arcr_str_eq_nocase_n(endp, 2u, "em", 2u)) {
                        px = (int32_t)(val * 16.0 + 0.5);
                    }
                }
            }
        }
        while (*p && (*p == ',' || *p == '/')) {
            if (*p == '/') {
                ++p;
                while (*p && !isspace((unsigned char)*p)) ++p;
            } else {
                ++p;
            }
        }
    }
    if (px < 4) px = 4;
    if (px > 256) px = 256;
    t->font_px   = px;
    t->font_bold = bold;
    t->font_mono = mono;
}

static ArcrFontSystem *c2d_ensure_fonts(ArcrJsVm *vm) {
    if (!vm) return NULL;
    if (!vm->canvas_fonts) {
        vm->canvas_fonts = arcr_font_system_create(NULL);
    }
    return vm->canvas_fonts;
}

static double c2d_measure_utf8_width(ArcrJsVm *vm, const CanvasTransform *ct,
                                     const char *utf8, size_t len) {
    if (!ct || !utf8 || len == 0u) return 0.0;
    ArcrFontSystem *fs = c2d_ensure_fonts(vm);
    int32_t fpx = ct->font_px > 0 ? ct->font_px : 10;
    double total = 0.0;
    size_t off = 0u;
    while (off < len) {
        uint32_t cp = 0u;
        size_t adv = arcr_utf8_decode_one(utf8 + off, len - off, &cp);
        if (adv == 0u) break;
        off += adv;
        ArcrGlyphMetrics gm;
        if (fs && arcr_font_measure_glyph_ex(fs, cp, fpx, ct->font_bold, ct->font_mono, &gm)) {
            total += (double)gm.advance_px;
        } else {
            total += (cp > 0x7Fu) ? (double)fpx : (double)fpx * (ct->font_mono ? 0.6 : 0.56);
        }
    }
    return total;
}

static void c2d_draw_text(ArcrJsVm *vm, Canvas2DCtx *c, ArcrDomSurface *surf,
                          const char *utf8, size_t len, double ux, double uy, bool is_stroke) {
    if (!vm || !c || !surf || !surf->argb_pixels || !utf8 || len == 0u) return;
    ArcrFontSystem *fs = c2d_ensure_fonts(vm);
    int32_t fpx = c->cur.font_px > 0 ? c->cur.font_px : 10;

    double run_w = c2d_measure_utf8_width(vm, &c->cur, utf8, len);
    if (c->cur.text_align == 1u) ux -= run_w * 0.5;
    else if (c->cur.text_align == 2u) ux -= run_w;

    double baseline_y = uy;
    if (c->cur.text_baseline == 1u)      baseline_y += (double)fpx * 0.82; /* top */
    else if (c->cur.text_baseline == 2u) baseline_y += (double)fpx * 0.35; /* middle */
    else if (c->cur.text_baseline == 3u) baseline_y -= (double)fpx * 0.18; /* bottom */

    uint32_t base_argb = is_stroke ? c->cur.stroke_argb : c->cur.fill_argb;
    uint32_t col = c2d_apply_alpha(base_argb, c->cur.global_alpha);
    if (((col >> 24) & 0xFFu) == 0u) return;

    double pen_x = ux;
    size_t off = 0u;
    while (off < len) {
        uint32_t cp = 0u;
        size_t adv = arcr_utf8_decode_one(utf8 + off, len - off, &cp);
        if (adv == 0u) break;
        off += adv;

        ArcrGlyphMetrics gm;
        memset(&gm, 0, sizeof(gm));
        const uint8_t *alpha = fs ? arcr_font_get_cached_glyph_ptr(fs, cp, fpx,
                                                                   c->cur.font_bold, c->cur.font_mono,
                                                                   &gm, true)
                                  : NULL;
        if (alpha && gm.width > 0 && gm.height > 0) {
            double gx0 = pen_x + (double)gm.bearing_x;
            double gy0 = baseline_y - (double)gm.bearing_y;
            for (int32_t gy = 0; gy < gm.height; ++gy) {
                for (int32_t gx = 0; gx < gm.width; ++gx) {
                    uint8_t cov = alpha[(size_t)gy * (size_t)gm.width + (size_t)gx];
                    if (cov == 0u) continue;
                    float sx = 0.0f, sy = 0.0f;
                    c2d_transform_pt(&c->cur, gx0 + (double)gx + 0.5, gy0 + (double)gy + 0.5, &sx, &sy);
                    int32_t px = (int32_t)floor((double)sx);
                    int32_t py = (int32_t)floor((double)sy);
                    if (px >= 0 && px < surf->width && py >= 0 && py < surf->height) {
                        uint32_t gcol = c2d_apply_alpha(col, (double)cov / 255.0);
                        c2d_blend_pixel(&surf->argb_pixels[(size_t)py * (size_t)surf->width + (size_t)px], gcol);
                    }
                }
            }
            pen_x += (double)gm.advance_px;
        } else {
            /* Minimal fallback block glyph if headless build has no font backend */
            int32_t gw = (int32_t)((double)fpx * 0.5);
            int32_t gh = (int32_t)((double)fpx * 0.7);
            if (!arcr_cp_is_whitespace(cp) && gw > 1 && gh > 1) {
                for (int32_t gy = 0; gy < gh; ++gy) {
                    for (int32_t gx = 0; gx < gw - 1; ++gx) {
                        if (is_stroke && gx > 0 && gx < gw - 2 && gy > 0 && gy < gh - 1) continue;
                        float sx = 0.0f, sy = 0.0f;
                        c2d_transform_pt(&c->cur, pen_x + (double)gx, baseline_y - (double)gh + (double)gy, &sx, &sy);
                        int32_t px = (int32_t)floor((double)sx);
                        int32_t py = (int32_t)floor((double)sy);
                        if (px >= 0 && px < surf->width && py >= 0 && py < surf->height) {
                            c2d_blend_pixel(&surf->argb_pixels[(size_t)py * (size_t)surf->width + (size_t)px], col);
                        }
                    }
                }
            }
            pen_x += (double)fpx * 0.58;
        }
    }
}

/* -------------------------------------------------------------------------
 * 5. Base64 Encoder for toDataURL("image/png")
 * ------------------------------------------------------------------------- */
static char *c2d_encode_png_data_url(const ArcrDomSurface *surf, size_t *out_str_len) {
    if (!surf || !surf->argb_pixels || surf->width <= 0 || surf->height <= 0) return NULL;
    uint8_t *png = NULL;
    size_t png_len = 0u;
    if (!arcr_image_encode_png_mem(surf->argb_pixels, surf->width, surf->height,
                                   surf->width, &png, &png_len) || !png) {
        return NULL;
    }

    static const char kB64[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *prefix = "data:image/png;base64,";
    size_t plen = strlen(prefix);
    size_t b64_len = ((png_len + 2u) / 3u) * 4u;
    char *out = (char *)malloc(plen + b64_len + 1u);
    if (!out) {
        free(png);
        return NULL;
    }
    memcpy(out, prefix, plen);
    size_t o = plen;
    for (size_t i = 0u; i < png_len; i += 3u) {
        uint32_t v = ((uint32_t)png[i]) << 16;
        if (i + 1u < png_len) v |= ((uint32_t)png[i + 1u]) << 8;
        if (i + 2u < png_len) v |= (uint32_t)png[i + 2u];
        out[o++] = kB64[(v >> 18) & 0x3Fu];
        out[o++] = kB64[(v >> 12) & 0x3Fu];
        out[o++] = (i + 1u < png_len) ? kB64[(v >> 6) & 0x3Fu] : '=';
        out[o++] = (i + 2u < png_len) ? kB64[v & 0x3Fu] : '=';
    }
    out[o] = '\0';
    free(png);
    if (out_str_len) *out_str_len = o;
    return out;
}

/* -------------------------------------------------------------------------
 * 6. Native JS Bridge Entrypoints (`B.canvasCmd`, `B.offscreenCreate`)
 * ------------------------------------------------------------------------- */
static AjsValue b_offscreen_create(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm) return ajs_uint32(0u);
    int32_t w = 300, h = 150;
    if (argc >= 1) (void)ajs_to_int32(avm, argv[0], &w);
    if (argc >= 2) (void)ajs_to_int32(avm, argv[1], &h);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;

    if (vm->next_offscreen_uid < WEB_OFFSCREEN_UID_BASE + 1u) {
        vm->next_offscreen_uid = WEB_OFFSCREEN_UID_BASE + 1u;
    }
    uint32_t uid = vm->next_offscreen_uid++;
    Canvas2DCtx *c = c2d_get_or_create_ctx(vm, uid, true);
    if (!c || !c2d_ensure_offscreen_surf(c, w, h, true)) {
        return ajs_uint32(0u);
    }
    return ajs_uint32(uid);
}

static bool c2d_arg_num(AjsVm *avm, int argc, const AjsValue *argv, int idx, double *out) {
    if (idx >= argc) return false;
    double v = 0.0;
    if (!ajs_to_number(avm, argv[idx], &v) || !isfinite(v)) return false;
    *out = v;
    return true;
}

static AjsValue b_canvas_cmd(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 2) return ajs_undefined();

    uint32_t uid = 0u;
    int32_t op = 0;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    (void)ajs_to_int32(avm, argv[1], &op);

    /* Gradient creation & stop registration (ctx-independent table on vm) */
    if (op == 100) { /* createGradient(isRadial, x0, y0, r0, x1, y1, r1) */
        if (vm->grad_count >= vm->grad_cap) {
            int32_t ncap = vm->grad_cap > 0 ? vm->grad_cap * 2 : 16;
            CanvasGrad *ng = (CanvasGrad *)realloc(vm->grads, (size_t)ncap * sizeof(CanvasGrad));
            if (!ng) return ajs_int32(-1);
            vm->grads    = ng;
            vm->grad_cap = ncap;
        }
        int32_t gid = vm->grad_count++;
        CanvasGrad *g = &vm->grads[gid];
        memset(g, 0, sizeof(*g));
        g->active    = true;
        g->is_radial = (argc >= 3) && ajs_to_bool(avm, argv[2]);
        (void)c2d_arg_num(avm, argc, argv, 3, &g->x0);
        (void)c2d_arg_num(avm, argc, argv, 4, &g->y0);
        (void)c2d_arg_num(avm, argc, argv, 5, &g->r0);
        (void)c2d_arg_num(avm, argc, argv, 6, &g->x1);
        (void)c2d_arg_num(avm, argc, argv, 7, &g->y1);
        (void)c2d_arg_num(avm, argc, argv, 8, &g->r1);
        if (g->is_radial && (g->r0 < 0.0 || g->r1 < 0.0)) {
            g->active = false;
            return ajs_int32(-2); /* IndexSizeError */
        }
        return ajs_int32(gid);
    }
    if (op == 101) { /* addColorStop(gradId, offset, colorStr) */
        int32_t gid = -1;
        double off = 0.0;
        if (argc < 5 || !ajs_to_int32(avm, argv[2], &gid)) return ajs_int32(-1);
        if (!c2d_arg_num(avm, argc, argv, 3, &off) || off < 0.0 || off > 1.0) {
            return ajs_int32(-2); /* IndexSizeError */
        }
        if (gid < 0 || gid >= vm->grad_count || !vm->grads[gid].active) return ajs_int32(-1);
        size_t clen = 0u;
        char *cstr = ajs_to_cstring_len(avm, argv[4], &clen);
        uint32_t argb = 0u;
        bool ok = cstr && web_parse_css_color(cstr, clen, &argb);
        if (cstr) ajs_free_cstring(avm, cstr);
        if (!ok) return ajs_int32(-3); /* SyntaxError */

        CanvasGrad *g = &vm->grads[gid];
        if (g->stop_count < ARCR_GRAD_MAX_STOPS) {
            int32_t pos = g->stop_count;
            while (pos > 0 && g->stops[pos - 1].offset > off) {
                g->stops[pos] = g->stops[pos - 1];
                --pos;
            }
            g->stops[pos].offset = off;
            g->stops[pos].argb   = argb;
            g->stop_count++;
        }
        return ajs_int32(0);
    }

    if (uid == 0u) return ajs_undefined();
    bool is_off = (uid >= WEB_OFFSCREEN_UID_BASE);
    Canvas2DCtx *c = c2d_get_or_create_ctx(vm, uid, is_off);
    if (!c) return ajs_undefined();

    switch (op) {
        case 0: { /* ensure/resize surface: (w, h, reset_and_clear) -> {width, height} */
            int32_t w = 0, h = 0;
            if (argc >= 3) (void)ajs_to_int32(avm, argv[2], &w);
            if (argc >= 4) (void)ajs_to_int32(avm, argv[3], &h);
            bool reset = (argc >= 5) ? ajs_to_bool(avm, argv[4]) : false;

            ArcrDomSurface *surf = NULL;
            if (c->is_offscreen) {
                surf = c2d_ensure_offscreen_surf(c, w, h, reset);
            } else if (vm->host.doc) {
                ArcrDomNode *el = web_dom_by_uid(vm, uid);
                if (el && el->kind == ARCR_NODE_ELEMENT) {
                    if (w <= 0 || h <= 0) {
                        int32_t dw = 300, dh = 150;
                        c2d_read_dom_canvas_dims(vm, el, &dw, &dh);
                        if (w <= 0) w = (el->surface && el->surface->width > 0) ? el->surface->width : dw;
                        if (h <= 0) h = (el->surface && el->surface->height > 0) ? el->surface->height : dh;
                    }
                    surf = web_ensure_node_surface(vm, el, w, h);
                    if (surf && reset && surf->argb_pixels) {
                        memset(surf->argb_pixels, 0, (size_t)surf->width * (size_t)surf->height * sizeof(uint32_t));
                    }
                    if (reset) vm->host.doc->dom_dirty = true;
                }
            }
            if (reset) c2d_reset_state(c);
            AjsValue o = ajs_new_object(avm);
            (void)ajs_set_prop_str(avm, o, "width", ajs_int32(surf ? surf->width : 300));
            (void)ajs_set_prop_str(avm, o, "height", ajs_int32(surf ? surf->height : 150));
            return o;
        }

        case 1: /* fillStyle */
        case 2: { /* strokeStyle */
            if (argc < 4) return ajs_undefined();
            bool is_grad = ajs_to_bool(avm, argv[2]);
            if (is_grad) {
                int32_t gid = -1;
                (void)ajs_to_int32(avm, argv[3], &gid);
                if (op == 1) c->fill_grad_id = gid;
                else         c->stroke_grad_id = gid;
            } else {
                size_t clen = 0u;
                char *cs = ajs_to_cstring_len(avm, argv[3], &clen);
                uint32_t argb = 0u;
                if (cs && web_parse_css_color(cs, clen, &argb)) {
                    if (op == 1) { c->cur.fill_argb = argb;   c->fill_grad_id = -1; }
                    else         { c->cur.stroke_argb = argb; c->stroke_grad_id = -1; }
                }
                if (cs) ajs_free_cstring(avm, cs);
            }
            return ajs_undefined();
        }

        case 3: { /* lineWidth */
            double lw = 1.0;
            if (c2d_arg_num(avm, argc, argv, 2, &lw) && lw > 0.0) c->cur.line_width = lw;
            return ajs_undefined();
        }

        case 4: { /* globalAlpha */
            double ga = 1.0;
            if (c2d_arg_num(avm, argc, argv, 2, &ga) && ga >= 0.0 && ga <= 1.0) c->cur.global_alpha = ga;
            return ajs_undefined();
        }

        case 5: { /* font */
            if (argc >= 3) {
                char *fs = ajs_to_cstring(avm, argv[2]);
                if (fs) {
                    c2d_parse_font_css(&c->cur, fs);
                    ajs_free_cstring(avm, fs);
                }
            }
            return ajs_undefined();
        }

        case 6: { /* textAlign */
            if (argc >= 3) {
                char *ta = ajs_to_cstring(avm, argv[2]);
                if (ta) {
                    if (arcr_str_eq_nocase(ta, "center")) c->cur.text_align = 1u;
                    else if (arcr_str_eq_nocase(ta, "right") || arcr_str_eq_nocase(ta, "end")) c->cur.text_align = 2u;
                    else if (arcr_str_eq_nocase(ta, "left") || arcr_str_eq_nocase(ta, "start")) c->cur.text_align = 0u;
                    ajs_free_cstring(avm, ta);
                }
            }
            return ajs_undefined();
        }

        case 7: { /* textBaseline */
            if (argc >= 3) {
                char *tb = ajs_to_cstring(avm, argv[2]);
                if (tb) {
                    if (arcr_str_eq_nocase(tb, "top") || arcr_str_eq_nocase(tb, "hanging")) c->cur.text_baseline = 1u;
                    else if (arcr_str_eq_nocase(tb, "middle")) c->cur.text_baseline = 2u;
                    else if (arcr_str_eq_nocase(tb, "bottom") || arcr_str_eq_nocase(tb, "ideographic")) c->cur.text_baseline = 3u;
                    else if (arcr_str_eq_nocase(tb, "alphabetic")) c->cur.text_baseline = 0u;
                    ajs_free_cstring(avm, tb);
                }
            }
            return ajs_undefined();
        }

        case 10: { /* save */
            if (c->stack_top < ARCR_CANVAS_STACK_MAX) {
                c->stack[c->stack_top++] = c->cur;
            }
            return ajs_undefined();
        }

        case 11: { /* restore */
            if (c->stack_top > 0) {
                c->cur = c->stack[--c->stack_top];
            }
            return ajs_undefined();
        }

        case 12: { /* translate(tx, ty) */
            double tx = 0.0, ty = 0.0;
            if (c2d_arg_num(avm, argc, argv, 2, &tx) && c2d_arg_num(avm, argc, argv, 3, &ty)) {
                c->cur.e += c->cur.a * tx + c->cur.c * ty;
                c->cur.f += c->cur.b * tx + c->cur.d * ty;
            }
            return ajs_undefined();
        }

        case 13: { /* scale(sx, sy) */
            double sx = 1.0, sy = 1.0;
            if (c2d_arg_num(avm, argc, argv, 2, &sx) && c2d_arg_num(avm, argc, argv, 3, &sy)) {
                c->cur.a *= sx; c->cur.b *= sx;
                c->cur.c *= sy; c->cur.d *= sy;
            }
            return ajs_undefined();
        }

        case 14: { /* rotate(rad) */
            double rad = 0.0;
            if (c2d_arg_num(avm, argc, argv, 2, &rad)) {
                double cs = cos(rad), sn = sin(rad);
                double a = c->cur.a, b = c->cur.b, cc = c->cur.c, d = c->cur.d;
                c->cur.a = a * cs + cc * sn;
                c->cur.b = b * cs + d  * sn;
                c->cur.c = cc * cs - a * sn;
                c->cur.d = d  * cs - b * sn;
            }
            return ajs_undefined();
        }

        case 15: { /* setTransform(a, b, c, d, e, f) */
            double a = 1.0, b = 0.0, cc = 0.0, d = 1.0, e = 0.0, f = 0.0;
            if (argc < 8) {
                c->cur.a = 1.0; c->cur.b = 0.0; c->cur.c = 0.0;
                c->cur.d = 1.0; c->cur.e = 0.0; c->cur.f = 0.0;
            } else if (c2d_arg_num(avm, argc, argv, 2, &a) && c2d_arg_num(avm, argc, argv, 3, &b) &&
                       c2d_arg_num(avm, argc, argv, 4, &cc) && c2d_arg_num(avm, argc, argv, 5, &d) &&
                       c2d_arg_num(avm, argc, argv, 6, &e) && c2d_arg_num(avm, argc, argv, 7, &f)) {
                c->cur.a = a; c->cur.b = b; c->cur.c = cc;
                c->cur.d = d; c->cur.e = e; c->cur.f = f;
            }
            return ajs_undefined();
        }

        case 16: { /* transform(a, b, c, d, e, f) */
            double a = 1.0, b = 0.0, cc = 0.0, d = 1.0, e = 0.0, f = 0.0;
            if (c2d_arg_num(avm, argc, argv, 2, &a) && c2d_arg_num(avm, argc, argv, 3, &b) &&
                c2d_arg_num(avm, argc, argv, 4, &cc) && c2d_arg_num(avm, argc, argv, 5, &d) &&
                c2d_arg_num(avm, argc, argv, 6, &e) && c2d_arg_num(avm, argc, argv, 7, &f)) {
                double oa = c->cur.a, ob = c->cur.b, oc = c->cur.c, od = c->cur.d, oe = c->cur.e, of = c->cur.f;
                c->cur.a = oa * a  + oc * b;
                c->cur.b = ob * a  + od * b;
                c->cur.c = oa * cc + oc * d;
                c->cur.d = ob * cc + od * d;
                c->cur.e = oa * e  + oc * f + oe;
                c->cur.f = ob * e  + od * f + of;
            }
            return ajs_undefined();
        }

        case 17: { /* getTransform() -> [a, b, c, d, e, f] */
            AjsValue arr = ajs_new_array(avm, 6u);
            (void)ajs_set_prop_index(avm, arr, 0u, ajs_number(c->cur.a));
            (void)ajs_set_prop_index(avm, arr, 1u, ajs_number(c->cur.b));
            (void)ajs_set_prop_index(avm, arr, 2u, ajs_number(c->cur.c));
            (void)ajs_set_prop_index(avm, arr, 3u, ajs_number(c->cur.d));
            (void)ajs_set_prop_index(avm, arr, 4u, ajs_number(c->cur.e));
            (void)ajs_set_prop_index(avm, arr, 5u, ajs_number(c->cur.f));
            return arr;
        }

        case 20: /* clearRect(x, y, w, h) */
        case 21: /* fillRect(x, y, w, h) */
        case 22: { /* strokeRect(x, y, w, h) */
            double rx = 0.0, ry = 0.0, rw = 0.0, rh = 0.0;
            if (!c2d_arg_num(avm, argc, argv, 2, &rx) || !c2d_arg_num(avm, argc, argv, 3, &ry) ||
                !c2d_arg_num(avm, argc, argv, 4, &rw) || !c2d_arg_num(avm, argc, argv, 5, &rh)) {
                return ajs_undefined();
            }
            if (rw == 0.0 || rh == 0.0) return ajs_undefined();
            ArcrDomSurface *surf = c2d_get_surface(vm, c, true);
            if (!surf || !surf->argb_pixels) return ajs_undefined();

            CanvasPt q[5];
            c2d_transform_pt(&c->cur, rx,      ry,      &q[0].x, &q[0].y); q[0].move_to = true;
            c2d_transform_pt(&c->cur, rx + rw, ry,      &q[1].x, &q[1].y); q[1].move_to = false;
            c2d_transform_pt(&c->cur, rx + rw, ry + rh, &q[2].x, &q[2].y); q[2].move_to = false;
            c2d_transform_pt(&c->cur, rx,      ry + rh, &q[3].x, &q[3].y); q[3].move_to = false;
            q[4] = q[0]; q[4].move_to = false;

            if (op == 20) {
                /* Clear transformed quad to transparent black */
                if (fabs(c->cur.b) < 1e-7 && fabs(c->cur.c) < 1e-7) {
                    int32_t x0 = (int32_t)floor((double)(q[0].x < q[2].x ? q[0].x : q[2].x) + 0.5);
                    int32_t x1 = (int32_t)floor((double)(q[0].x > q[2].x ? q[0].x : q[2].x) + 0.5);
                    int32_t y0 = (int32_t)floor((double)(q[0].y < q[2].y ? q[0].y : q[2].y) + 0.5);
                    int32_t y1 = (int32_t)floor((double)(q[0].y > q[2].y ? q[0].y : q[2].y) + 0.5);
                    if (x0 < 0) x0 = 0;
                    if (y0 < 0) y0 = 0;
                    if (x1 > surf->width) x1 = surf->width;
                    if (y1 > surf->height) y1 = surf->height;
                    for (int32_t y = y0; y < y1; ++y) {
                        uint32_t *row = surf->argb_pixels + (size_t)y * (size_t)surf->width;
                        if (x1 > x0) memset(&row[x0], 0, (size_t)(x1 - x0) * sizeof(uint32_t));
                    }
                } else {
                    C2DEdge edges[4];
                    int ec = c2d_collect_closed_edges(q, 4, edges, 4);
                    for (int32_t y = 0; y < surf->height; ++y) {
                        float sy = (float)y + 0.5f;
                        C2DIsect is[8];
                        int icnt = 0;
                        for (int i = 0; i < ec; ++i) {
                            if ((edges[i].y0 <= sy && edges[i].y1 > sy) ||
                                (edges[i].y1 <= sy && edges[i].y0 > sy)) {
                                float t = (sy - edges[i].y0) / (edges[i].y1 - edges[i].y0);
                                is[icnt].x = edges[i].x0 + t * (edges[i].x1 - edges[i].x0);
                                is[icnt].dir = 1;
                                icnt++;
                            }
                        }
                        c2d_sort_isects(is, icnt);
                        uint32_t *row = surf->argb_pixels + (size_t)y * (size_t)surf->width;
                        for (int i = 0; i + 1 < icnt; i += 2) {
                            int32_t x0 = ARCR_MAX(0, (int32_t)floor((double)is[i].x + 0.5));
                            int32_t x1 = ARCR_MIN(surf->width, (int32_t)floor((double)is[i + 1].x + 0.5));
                            if (x1 > x0) memset(&row[x0], 0, (size_t)(x1 - x0) * sizeof(uint32_t));
                        }
                    }
                }
            } else if (op == 21) {
                c2d_fill_pts(vm, c, surf, q, 4, false);
            } else {
                c2d_stroke_pts(vm, c, surf, q, 5);
            }
            return ajs_undefined();
        }

        case 30: /* beginPath */
            c->pt_count = 0;
            return ajs_undefined();

        case 31: { /* moveTo(x, y) */
            double x = 0.0, y = 0.0;
            if (c2d_arg_num(avm, argc, argv, 2, &x) && c2d_arg_num(avm, argc, argv, 3, &y)) {
                float sx = 0.0f, sy = 0.0f;
                c2d_transform_pt(&c->cur, x, y, &sx, &sy);
                c2d_path_move_raw(c, sx, sy);
            }
            return ajs_undefined();
        }

        case 32: { /* lineTo(x, y) */
            double x = 0.0, y = 0.0;
            if (c2d_arg_num(avm, argc, argv, 2, &x) && c2d_arg_num(avm, argc, argv, 3, &y)) {
                float sx = 0.0f, sy = 0.0f;
                c2d_transform_pt(&c->cur, x, y, &sx, &sy);
                c2d_path_line_raw(c, sx, sy);
            }
            return ajs_undefined();
        }

        case 33: /* closePath */
            if (c->pt_count > 0) {
                c2d_path_line_raw(c, c->sub_start_x, c->sub_start_y);
            }
            return ajs_undefined();

        case 34: { /* rect(x, y, w, h) */
            double rx = 0.0, ry = 0.0, rw = 0.0, rh = 0.0;
            if (c2d_arg_num(avm, argc, argv, 2, &rx) && c2d_arg_num(avm, argc, argv, 3, &ry) &&
                c2d_arg_num(avm, argc, argv, 4, &rw) && c2d_arg_num(avm, argc, argv, 5, &rh)) {
                float p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y;
                c2d_transform_pt(&c->cur, rx,      ry,      &p0x, &p0y);
                c2d_transform_pt(&c->cur, rx + rw, ry,      &p1x, &p1y);
                c2d_transform_pt(&c->cur, rx + rw, ry + rh, &p2x, &p2y);
                c2d_transform_pt(&c->cur, rx,      ry + rh, &p3x, &p3y);
                c2d_path_move_raw(c, p0x, p0y);
                c2d_path_line_raw(c, p1x, p1y);
                c2d_path_line_raw(c, p2x, p2y);
                c2d_path_line_raw(c, p3x, p3y);
                c2d_path_line_raw(c, p0x, p0y);
            }
            return ajs_undefined();
        }

        case 35: /* arc(cx, cy, r, a0, a1, anticlockwise) */
        case 36: { /* ellipse(cx, cy, rx, ry, rot, a0, a1, anticlockwise) */
            double cx = 0.0, cy = 0.0, rx = 0.0, ry = 0.0, rot = 0.0, a0 = 0.0, a1 = 0.0;
            bool ccw = false;
            if (op == 35) {
                if (!c2d_arg_num(avm, argc, argv, 2, &cx) || !c2d_arg_num(avm, argc, argv, 3, &cy) ||
                    !c2d_arg_num(avm, argc, argv, 4, &rx) || !c2d_arg_num(avm, argc, argv, 5, &a0) ||
                    !c2d_arg_num(avm, argc, argv, 6, &a1)) {
                    return ajs_int32(0);
                }
                if (rx < 0.0) return ajs_int32(-1); /* IndexSizeError */
                ry = rx;
                if (argc >= 8) ccw = ajs_to_bool(avm, argv[7]);
            } else {
                if (!c2d_arg_num(avm, argc, argv, 2, &cx) || !c2d_arg_num(avm, argc, argv, 3, &cy) ||
                    !c2d_arg_num(avm, argc, argv, 4, &rx) || !c2d_arg_num(avm, argc, argv, 5, &ry) ||
                    !c2d_arg_num(avm, argc, argv, 6, &rot) || !c2d_arg_num(avm, argc, argv, 7, &a0) ||
                    !c2d_arg_num(avm, argc, argv, 8, &a1)) {
                    return ajs_int32(0);
                }
                if (rx < 0.0 || ry < 0.0) return ajs_int32(-1); /* IndexSizeError */
                if (argc >= 10) ccw = ajs_to_bool(avm, argv[9]);
            }

            double tau = 2.0 * WEB_PI;
            double sweep = 0.0;
            if (!ccw) {
                if (a1 - a0 >= tau) sweep = tau;
                else {
                    sweep = fmod(a1 - a0, tau);
                    if (sweep < 0.0) sweep += tau;
                }
            } else {
                if (a0 - a1 >= tau) sweep = -tau;
                else {
                    sweep = -fmod(a0 - a1, tau);
                    if (sweep > 0.0) sweep -= tau;
                }
            }

            int steps = (int)(fabs(sweep) * 12.0 + 4.0);
            if (steps < 8) steps = 8;
            if (steps > 96) steps = 96;
            double crot = cos(rot), srot = sin(rot);
            for (int i = 0; i <= steps; ++i) {
                double ang = a0 + sweep * ((double)i / (double)steps);
                double ex = rx * cos(ang);
                double ey = ry * sin(ang);
                double ux = cx + ex * crot - ey * srot;
                double uy = cy + ex * srot + ey * crot;
                float sx = 0.0f, sy = 0.0f;
                c2d_transform_pt(&c->cur, ux, uy, &sx, &sy);
                if (i == 0 && c->pt_count == 0) c2d_path_move_raw(c, sx, sy);
                else c2d_path_line_raw(c, sx, sy);
            }
            return ajs_int32(0);
        }

        case 37: { /* quadraticCurveTo(cpx, cpy, x, y) */
            double cpx = 0.0, cpy = 0.0, x = 0.0, y = 0.0;
            if (!c2d_arg_num(avm, argc, argv, 2, &cpx) || !c2d_arg_num(avm, argc, argv, 3, &cpy) ||
                !c2d_arg_num(avm, argc, argv, 4, &x)   || !c2d_arg_num(avm, argc, argv, 5, &y)) {
                return ajs_undefined();
            }
            float scx, scy, sex, sey;
            c2d_transform_pt(&c->cur, cpx, cpy, &scx, &scy);
            c2d_transform_pt(&c->cur, x,   y,   &sex, &sey);
            if (c->pt_count == 0) c2d_path_move_raw(c, scx, scy);
            float s0x = c->pts[c->pt_count - 1].x;
            float s0y = c->pts[c->pt_count - 1].y;
            for (int i = 1; i <= 16; ++i) {
                float t = (float)i / 16.0f;
                float mt = 1.0f - t;
                float px = mt * mt * s0x + 2.0f * mt * t * scx + t * t * sex;
                float py = mt * mt * s0y + 2.0f * mt * t * scy + t * t * sey;
                c2d_path_line_raw(c, px, py);
            }
            return ajs_undefined();
        }

        case 38: { /* bezierCurveTo(cp1x, cp1y, cp2x, cp2y, x, y) */
            double c1x = 0.0, c1y = 0.0, c2x = 0.0, c2y = 0.0, x = 0.0, y = 0.0;
            if (!c2d_arg_num(avm, argc, argv, 2, &c1x) || !c2d_arg_num(avm, argc, argv, 3, &c1y) ||
                !c2d_arg_num(avm, argc, argv, 4, &c2x) || !c2d_arg_num(avm, argc, argv, 5, &c2y) ||
                !c2d_arg_num(avm, argc, argv, 6, &x)   || !c2d_arg_num(avm, argc, argv, 7, &y)) {
                return ajs_undefined();
            }
            float s1x, s1y, s2x, s2y, sex, sey;
            c2d_transform_pt(&c->cur, c1x, c1y, &s1x, &s1y);
            c2d_transform_pt(&c->cur, c2x, c2y, &s2x, &s2y);
            c2d_transform_pt(&c->cur, x,   y,   &sex, &sey);
            if (c->pt_count == 0) c2d_path_move_raw(c, s1x, s1y);
            float s0x = c->pts[c->pt_count - 1].x;
            float s0y = c->pts[c->pt_count - 1].y;
            for (int i = 1; i <= 20; ++i) {
                float t = (float)i / 20.0f;
                float mt = 1.0f - t;
                float b0 = mt * mt * mt;
                float b1 = 3.0f * mt * mt * t;
                float b2 = 3.0f * mt * t * t;
                float b3 = t * t * t;
                c2d_path_line_raw(c, b0 * s0x + b1 * s1x + b2 * s2x + b3 * sex,
                                     b0 * s0y + b1 * s1y + b2 * s2y + b3 * sey);
            }
            return ajs_undefined();
        }

        case 40: { /* fill(windingRuleOpt) */
            ArcrDomSurface *surf = c2d_get_surface(vm, c, true);
            if (!surf) return ajs_undefined();
            bool even_odd = false;
            if (argc >= 3 && ajs_is_string(avm, argv[2])) {
                char *wr = ajs_to_cstring(avm, argv[2]);
                if (wr) {
                    even_odd = (strcmp(wr, "evenodd") == 0);
                    ajs_free_cstring(avm, wr);
                }
            }
            c2d_fill_pts(vm, c, surf, c->pts, c->pt_count, even_odd);
            return ajs_undefined();
        }

        case 41: { /* stroke() */
            ArcrDomSurface *surf = c2d_get_surface(vm, c, true);
            if (!surf) return ajs_undefined();
            c2d_stroke_pts(vm, c, surf, c->pts, c->pt_count);
            return ajs_undefined();
        }

        case 42: { /* isPointInPath(x, y, windingRuleOpt) */
            double x = 0.0, y = 0.0;
            if (!c2d_arg_num(avm, argc, argv, 2, &x) || !c2d_arg_num(avm, argc, argv, 3, &y)) {
                return ajs_bool(false);
            }
            bool even_odd = false;
            if (argc >= 5 && ajs_is_string(avm, argv[4])) {
                char *wr = ajs_to_cstring(avm, argv[4]);
                if (wr) {
                    even_odd = (strcmp(wr, "evenodd") == 0);
                    ajs_free_cstring(avm, wr);
                }
            }
            return ajs_bool(c2d_point_in_path(c, (float)x, (float)y, even_odd));
        }

        case 50: /* fillText(text, x, y) */
        case 51: { /* strokeText(text, x, y) */
            double x = 0.0, y = 0.0;
            if (argc < 5 || !c2d_arg_num(avm, argc, argv, 3, &x) || !c2d_arg_num(avm, argc, argv, 4, &y)) {
                return ajs_undefined();
            }
            ArcrDomSurface *surf = c2d_get_surface(vm, c, true);
            if (!surf) return ajs_undefined();
            size_t tlen = 0u;
            char *txt = ajs_to_cstring_len(avm, argv[2], &tlen);
            if (txt) {
                c2d_draw_text(vm, c, surf, txt, tlen, x, y, op == 51);
                ajs_free_cstring(avm, txt);
            }
            return ajs_undefined();
        }

        case 52: { /* measureText(text) */
            double w = 0.0;
            if (argc >= 3) {
                size_t tlen = 0u;
                char *txt = ajs_to_cstring_len(avm, argv[2], &tlen);
                if (txt) {
                    w = c2d_measure_utf8_width(vm, &c->cur, txt, tlen);
                    ajs_free_cstring(avm, txt);
                }
            }
            double fpx = (double)(c->cur.font_px > 0 ? c->cur.font_px : 10);
            AjsValue m = ajs_new_object(avm);
            (void)ajs_set_prop_str(avm, m, "width", ajs_number(w));
            (void)ajs_set_prop_str(avm, m, "actualBoundingBoxAscent", ajs_number(fpx * 0.8));
            (void)ajs_set_prop_str(avm, m, "actualBoundingBoxDescent", ajs_number(fpx * 0.2));
            (void)ajs_set_prop_str(avm, m, "fontBoundingBoxAscent", ajs_number(fpx * 0.85));
            (void)ajs_set_prop_str(avm, m, "fontBoundingBoxDescent", ajs_number(fpx * 0.25));
            return m;
        }

        case 60: { /* getImageData(sx, sy, sw, sh) -> ArrayBuffer (RGBA8888) */
            int32_t sx = 0, sy = 0, sw = 0, sh = 0;
            if (argc < 6 || !ajs_to_int32(avm, argv[2], &sx) || !ajs_to_int32(avm, argv[3], &sy) ||
                !ajs_to_int32(avm, argv[4], &sw) || !ajs_to_int32(avm, argv[5], &sh)) {
                return ajs_null();
            }
            if (sw == 0 || sh == 0) return ajs_null();
            if (sw < 0) { sx += sw; sw = -sw; }
            if (sh < 0) { sy += sh; sh = -sh; }
            if (sw > 4096 || sh > 4096) return ajs_null();

            ArcrDomSurface *surf = c2d_get_surface(vm, c, false);
            size_t total = (size_t)sw * (size_t)sh * 4u;
            uint8_t *rgba = (uint8_t *)calloc(1u, total);
            if (!rgba) return ajs_null();

            if (surf && surf->argb_pixels) {
                for (int32_t y = 0; y < sh; ++y) {
                    int32_t py = sy + y;
                    if (py < 0 || py >= surf->height) continue;
                    const uint32_t *srow = surf->argb_pixels + (size_t)py * (size_t)surf->width;
                    uint8_t *drow = rgba + (size_t)y * (size_t)sw * 4u;
                    for (int32_t x = 0; x < sw; ++x) {
                        int32_t px = sx + x;
                        if (px < 0 || px >= surf->width) continue;
                        uint32_t argb = srow[px];
                        drow[(size_t)x * 4u + 0u] = (uint8_t)((argb >> 16) & 0xFFu);
                        drow[(size_t)x * 4u + 1u] = (uint8_t)((argb >> 8)  & 0xFFu);
                        drow[(size_t)x * 4u + 2u] = (uint8_t)(argb         & 0xFFu);
                        drow[(size_t)x * 4u + 3u] = (uint8_t)((argb >> 24) & 0xFFu);
                    }
                }
            }
            AjsValue ab = ajs_new_array_buffer_copy(avm, rgba, total);
            free(rgba);
            return ab;
        }

        case 61: { /* putImageData(rgbaBuf, imgW, imgH, dx, dy, dirtyX, dirtyY, dirtyW, dirtyH) */
            if (argc < 11) return ajs_undefined();
            size_t blen = 0u;
            uint8_t *rgba = ajs_get_buffer_source_bytes(avm, argv[2], &blen, NULL);
            int32_t iw = 0, ih = 0, dx = 0, dy = 0, drx = 0, dry = 0, drw = 0, drh = 0;
            (void)ajs_to_int32(avm, argv[3], &iw);
            (void)ajs_to_int32(avm, argv[4], &ih);
            (void)ajs_to_int32(avm, argv[5], &dx);
            (void)ajs_to_int32(avm, argv[6], &dy);
            (void)ajs_to_int32(avm, argv[7], &drx);
            (void)ajs_to_int32(avm, argv[8], &dry);
            (void)ajs_to_int32(avm, argv[9], &drw);
            (void)ajs_to_int32(avm, argv[10], &drh);
            if (!rgba || iw <= 0 || ih <= 0 || blen < (size_t)iw * (size_t)ih * 4u) return ajs_undefined();

            ArcrDomSurface *surf = c2d_get_surface(vm, c, true);
            if (!surf || !surf->argb_pixels) return ajs_undefined();

            if (drw < 0) { drx += drw; drw = -drw; }
            if (drh < 0) { dry += drh; drh = -drh; }
            if (drx < 0) { drw += drx; drx = 0; }
            if (dry < 0) { drh += dry; dry = 0; }
            if (drx + drw > iw) drw = iw - drx;
            if (dry + drh > ih) drh = ih - dry;
            if (drw <= 0 || drh <= 0) return ajs_undefined();

            for (int32_t y = 0; y < drh; ++y) {
                int32_t sy = dry + y;
                int32_t ty = dy + sy;
                if (ty < 0 || ty >= surf->height) continue;
                uint32_t *drow = surf->argb_pixels + (size_t)ty * (size_t)surf->width;
                const uint8_t *srow = rgba + (size_t)sy * (size_t)iw * 4u;
                for (int32_t x = 0; x < drw; ++x) {
                    int32_t sx = drx + x;
                    int32_t tx = dx + sx;
                    if (tx < 0 || tx >= surf->width) continue;
                    uint32_t r = srow[(size_t)sx * 4u + 0u];
                    uint32_t g = srow[(size_t)sx * 4u + 1u];
                    uint32_t b = srow[(size_t)sx * 4u + 2u];
                    uint32_t a = srow[(size_t)sx * 4u + 3u];
                    drow[tx] = (a << 24) | (r << 16) | (g << 8) | b;
                }
            }
            return ajs_undefined();
        }

        case 70: { /* drawImage(srcUid, sx, sy, sw, sh, dx, dy, dw, dh, hasSrcRect) */
            if (argc < 12) return ajs_undefined();
            uint32_t src_uid = 0u;
            (void)ajs_to_uint32(avm, argv[2], &src_uid);
            const ArcrDomSurface *src_surf = NULL;
            if (src_uid >= WEB_OFFSCREEN_UID_BASE) {
                Canvas2DCtx *sc = c2d_find_ctx(vm, src_uid);
                if (sc && sc->off_surf.argb_pixels) src_surf = &sc->off_surf;
            } else {
                ArcrDomNode *sn = web_dom_by_uid(vm, src_uid);
                if (sn && sn->surface && sn->surface->argb_pixels) src_surf = sn->surface;
            }
            if (!src_surf || src_surf->width <= 0 || src_surf->height <= 0) return ajs_undefined();

            double sx = 0.0, sy = 0.0, sw = (double)src_surf->width, sh = (double)src_surf->height;
            double dx = 0.0, dy = 0.0, dw = sw, dh = sh;
            (void)c2d_arg_num(avm, argc, argv, 3, &sx);
            (void)c2d_arg_num(avm, argc, argv, 4, &sy);
            (void)c2d_arg_num(avm, argc, argv, 5, &sw);
            (void)c2d_arg_num(avm, argc, argv, 6, &sh);
            (void)c2d_arg_num(avm, argc, argv, 7, &dx);
            (void)c2d_arg_num(avm, argc, argv, 8, &dy);
            (void)c2d_arg_num(avm, argc, argv, 9, &dw);
            (void)c2d_arg_num(avm, argc, argv, 10, &dh);
            bool has_src_rect = ajs_to_bool(avm, argv[11]);
            if (!has_src_rect) {
                sx = 0.0; sy = 0.0;
                sw = (double)src_surf->width;
                sh = (double)src_surf->height;
                if (dw <= 0.0) dw = sw;
                if (dh <= 0.0) dh = sh;
            }
            if (sw == 0.0 || sh == 0.0 || dw == 0.0 || dh == 0.0) return ajs_undefined();

            ArcrDomSurface *dst_surf = c2d_get_surface(vm, c, true);
            if (!dst_surf || !dst_surf->argb_pixels) return ajs_undefined();

            /* Copy self-blit safely if src_surf == dst_surf */
            uint32_t *temp_copy = NULL;
            const uint32_t *spx = src_surf->argb_pixels;
            int32_t src_w = src_surf->width, src_h = src_surf->height;
            if (src_surf == dst_surf) {
                size_t nb = (size_t)src_w * (size_t)src_h * sizeof(uint32_t);
                temp_copy = (uint32_t *)malloc(nb);
                if (!temp_copy) return ajs_undefined();
                memcpy(temp_copy, src_surf->argb_pixels, nb);
                spx = temp_copy;
            }

            double ia = 1.0, ib = 0.0, ic = 0.0, id = 1.0, ie = 0.0, ig = 0.0;
            if (c2d_invert_ctm(&c->cur, &ia, &ib, &ic, &id, &ie, &ig)) {
                float qx[4], qy[4];
                c2d_transform_pt(&c->cur, dx,      dy,      &qx[0], &qy[0]);
                c2d_transform_pt(&c->cur, dx + dw, dy,      &qx[1], &qy[1]);
                c2d_transform_pt(&c->cur, dx + dw, dy + dh, &qx[2], &qy[2]);
                c2d_transform_pt(&c->cur, dx,      dy + dh, &qx[3], &qy[3]);
                float minx = qx[0], maxx = qx[0], miny = qy[0], maxy = qy[0];
                for (int i = 1; i < 4; ++i) {
                    if (qx[i] < minx) minx = qx[i];
                    if (qx[i] > maxx) maxx = qx[i];
                    if (qy[i] < miny) miny = qy[i];
                    if (qy[i] > maxy) maxy = qy[i];
                }
                int32_t bx0 = ARCR_MAX(0, (int32_t)floor((double)minx));
                int32_t bx1 = ARCR_MIN(dst_surf->width, (int32_t)ceil((double)maxx));
                int32_t by0 = ARCR_MAX(0, (int32_t)floor((double)miny));
                int32_t by1 = ARCR_MIN(dst_surf->height, (int32_t)ceil((double)maxy));

                for (int32_t py = by0; py < by1; ++py) {
                    uint32_t *drow = dst_surf->argb_pixels + (size_t)py * (size_t)dst_surf->width;
                    for (int32_t px = bx0; px < bx1; ++px) {
                        double scx = (double)px + 0.5, scy = (double)py + 0.5;
                        double ux = ia * scx + ic * scy + ie;
                        double uy = ib * scx + id * scy + ig;
                        double u = (ux - dx) / dw;
                        double v = (uy - dy) / dh;
                        if (u < 0.0 || u >= 1.0 || v < 0.0 || v >= 1.0) continue;
                        int32_t sp_x = (int32_t)floor(sx + u * sw);
                        int32_t sp_y = (int32_t)floor(sy + v * sh);
                        if (sp_x < 0 || sp_x >= src_w || sp_y < 0 || sp_y >= src_h) continue;
                        uint32_t col = c2d_apply_alpha(spx[(size_t)sp_y * (size_t)src_w + (size_t)sp_x],
                                                       c->cur.global_alpha);
                        c2d_blend_pixel(&drow[px], col);
                    }
                }
            }
            free(temp_copy);
            return ajs_undefined();
        }

        case 80: { /* toDataURL() -> "data:image/png;base64,..." */
            ArcrDomSurface *surf = c2d_get_surface(vm, c, false);
            if (!surf) return ajs_string_cstr(avm, "data:,");
            size_t slen = 0u;
            char *uri = c2d_encode_png_data_url(surf, &slen);
            if (!uri) return ajs_string_cstr(avm, "data:,");
            AjsValue ret = ajs_string_utf8(avm, uri, slen);
            free(uri);
            return ret;
        }

        case 81: { /* toPngBytes() -> ArrayBuffer */
            ArcrDomSurface *surf = c2d_get_surface(vm, c, false);
            if (!surf || !surf->argb_pixels) return ajs_null();
            uint8_t *png = NULL;
            size_t plen = 0u;
            if (!arcr_image_encode_png_mem(surf->argb_pixels, surf->width, surf->height,
                                           surf->width, &png, &plen) || !png) {
                return ajs_null();
            }
            AjsValue ab = ajs_new_array_buffer_copy(avm, png, plen);
            free(png);
            return ab;
        }

        default:
            break;
    }
    return ajs_undefined();
}

void web_canvas_register_bridge(ArcrJsVm *vm, AjsValue bridge_obj) {
    if (!vm || !vm->avm) return;
    web_bind_fn(vm->avm, bridge_obj, "canvasCmd",       b_canvas_cmd,       12, vm);
    web_bind_fn(vm->avm, bridge_obj, "offscreenCreate", b_offscreen_create,  2, vm);
}

void web_canvas_cleanup(ArcrJsVm *vm) {
    if (!vm) return;
    Canvas2DCtx *c = vm->canvas_head;
    vm->canvas_head = NULL;
    while (c) {
        Canvas2DCtx *nxt = c->next;
        free(c->off_surf.argb_pixels);
        c->off_surf.argb_pixels = NULL;
        free(c);
        c = nxt;
    }
    free(vm->grads);
    vm->grads      = NULL;
    vm->grad_count = 0;
    vm->grad_cap   = 0;
    if (vm->canvas_fonts) {
        arcr_font_system_destroy(vm->canvas_fonts);
        vm->canvas_fonts = NULL;
    }
}
