/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * tools/gen_brand_assets.c - Procedural 4x4 Supersampled Brand Icon & Banner Generator
 *
 * Usage:
 *   gen_brand_assets <output_dir>
 *
 * Generates into <output_dir>:
 *   - waterscape_256.png, waterscape_64.png, waterscape_48.png,
 *     waterscape_32.png, waterscape_16.png
 *   - waterscape.ico (16x16, 32x32, 48x48 32-bpp BGRA DIB + AND mask for Win2000+,
 *                     plus 256x256 PNG-compressed frame for Vista/Win10/Win11)
 *   - wizard_banner.bmp (492x64 24-bpp Windows BMP Setup Wizard header banner)
 *   - waterscape.svg (Scalable vector icon for Linux hicolor desktop integration)
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "../base/arcr_codec.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  define WS_MKDIR(path) _mkdir(path)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define WS_MKDIR(path) mkdir((path), 0755)
#endif

#define WS_PI 3.14159265358979323846

typedef struct WsColorF {
    double r;
    double g;
    double b;
    double a;
} WsColorF;

static double ws_clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static double ws_smoothstep(double edge0, double edge1, double x) {
    if (edge1 <= edge0) return (x >= edge1) ? 1.0 : 0.0;
    double t = ws_clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0 - 2.0 * t);
}

static WsColorF ws_rgb(uint8_t r, uint8_t g, uint8_t b, double a) {
    WsColorF c;
    c.r = (double)r / 255.0;
    c.g = (double)g / 255.0;
    c.b = (double)b / 255.0;
    c.a = ws_clamp01(a);
    return c;
}

static WsColorF ws_lerp_color(WsColorF a, WsColorF b, double t) {
    WsColorF out;
    double u = ws_clamp01(t);
    out.r = a.r + (b.r - a.r) * u;
    out.g = a.g + (b.g - a.g) * u;
    out.b = a.b + (b.b - a.b) * u;
    out.a = a.a + (b.a - a.a) * u;
    return out;
}

/* Alpha-over blend in straight-alpha space (within a single subpixel sample) */
static WsColorF ws_blend_over(WsColorF dst, WsColorF src) {
    WsColorF out;
    double sa = ws_clamp01(src.a);
    double da = ws_clamp01(dst.a);
    double oa = sa + da * (1.0 - sa);
    if (oa <= 1e-9) {
        out.r = 0.0;
        out.g = 0.0;
        out.b = 0.0;
        out.a = 0.0;
        return out;
    }
    out.r = (src.r * sa + dst.r * da * (1.0 - sa)) / oa;
    out.g = (src.g * sa + dst.g * da * (1.0 - sa)) / oa;
    out.b = (src.b * sa + dst.b * da * (1.0 - sa)) / oa;
    out.a = oa;
    return out;
}

/* Signed distance from point p to segment a->b */
static double ws_dist_segment(double px, double py,
                              double ax, double ay,
                              double bx, double by) {
    double vx = bx - ax;
    double vy = by - ay;
    double wx = px - ax;
    double wy = py - ay;
    double vv = vx * vx + vy * vy;
    double t  = (vv > 1e-12) ? ((wx * vx + wy * vy) / vv) : 0.0;
    t = ws_clamp01(t);
    double dx = px - (ax + t * vx);
    double dy = py - (ay + t * vy);
    return sqrt(dx * dx + dy * dy);
}

/* Half-plane cross product for 2D triangle test (clockwise/counter-clockwise) */
static double ws_edge_cross(double ax, double ay, double bx, double by, double px, double py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static bool ws_point_in_tri(double px, double py,
                            double ax, double ay,
                            double bx, double by,
                            double cx, double cy) {
    double c1 = ws_edge_cross(ax, ay, bx, by, px, py);
    double c2 = ws_edge_cross(bx, by, cx, cy, px, py);
    double c3 = ws_edge_cross(cx, cy, ax, ay, px, py);
    bool has_neg = (c1 < 0.0) || (c2 < 0.0) || (c3 < 0.0);
    bool has_pos = (c1 > 0.0) || (c2 > 0.0) || (c3 > 0.0);
    return !(has_neg && has_pos);
}

/*
 * Evaluates a single subpixel sample of the Waterscape emblem on normalized [0, 1]^2:
 *   - Deep ocean-indigo rounded squircle with subtle rim glow
 *   - Luminous horizon sun/compass halo
 *   - Crisp dual-faceted white/ice-cyan navigation sail / compass spire
 *   - Three layered azure-to-cyan harmonic wave crests + foam highlight
 */
static WsColorF ws_sample_icon(double u, double v, double aa_px) {
    WsColorF col = {0.0, 0.0, 0.0, 0.0};

    /* Squircle SDF centered at (0.5, 0.5) with radius 0.455 */
    double dx = fabs(u - 0.5) / 0.455;
    double dy = fabs(v - 0.5) / 0.455;
    double p  = 4.35;
    double sq_dist = pow(pow(dx, p) + pow(dy, p), 1.0 / p) - 1.0;
    double mask = 1.0 - ws_smoothstep(-aa_px * 1.1, aa_px * 1.1, sq_dist * 0.455);
    if (mask <= 0.0005) {
        return col;
    }

    /* 1. Deep Ocean-Indigo Squircle Background */
    WsColorF bg_top = ws_rgb(8u, 19u, 48u, 1.0);     /* #081330 midnight indigo */
    WsColorF bg_mid = ws_rgb(14u, 44u, 96u, 1.0);    /* #0E2C60 deep ocean */
    WsColorF bg_bot = ws_rgb(18u, 82u, 148u, 1.0);   /* #125294 rich azure */
    WsColorF bg = (v < 0.55)
        ? ws_lerp_color(bg_top, bg_mid, v / 0.55)
        : ws_lerp_color(bg_mid, bg_bot, (v - 0.55) / 0.45);

    /* Radial horizon glow behind the compass sail */
    double gx = u - 0.50;
    double gy = v - 0.42;
    double gr = sqrt(gx * gx + gy * gy);
    double glow = 1.0 - ws_smoothstep(0.02, 0.36, gr);
    bg = ws_blend_over(bg, ws_rgb(56u, 189u, 248u, 0.28 * glow));

    /* Subtle compass meridian ring in the upper sky */
    double ring_d = fabs(gr - 0.255);
    if (v < 0.66 && ring_d < 0.012 + aa_px) {
        double ring_a = (1.0 - ws_smoothstep(0.003, 0.009 + aa_px, ring_d)) * 0.28;
        bg = ws_blend_over(bg, ws_rgb(186u, 230u, 253u, ring_a));
    }

    /* 2. Back Wave (deep teal-azure) */
    double w1_y = 0.585 + 0.048 * sin((u - 0.08) * 2.0 * WS_PI * 1.05)
                        - 0.018 * cos(u * 2.0 * WS_PI * 2.1);
    if (v > w1_y - aa_px * 2.0) {
        double a1 = ws_smoothstep(w1_y - aa_px, w1_y + aa_px, v);
        WsColorF c1 = ws_lerp_color(ws_rgb(2u, 132u, 199u, a1),
                                    ws_rgb(12u, 74u, 110u, a1),
                                    ws_clamp01((v - w1_y) / 0.36));
        bg = ws_blend_over(bg, c1);
    }

    /* 3. Crisp White / Ice-Cyan Compass Sail Highlight (rises from mid-wave) */
    {
        /* Left sunlit facet: apex (0.50, 0.175), base-left (0.295, 0.625), center-mast (0.492, 0.605) */
        bool in_left = ws_point_in_tri(u, v, 0.500, 0.175, 0.295, 0.625, 0.492, 0.605);
        if (in_left) {
            WsColorF sail_l = ws_lerp_color(ws_rgb(255u, 255u, 255u, 0.99),
                                            ws_rgb(224u, 242u, 254u, 0.97),
                                            ws_clamp01((v - 0.175) / 0.46));
            bg = ws_blend_over(bg, sail_l);
        }

        /* Right shaded cyan-ice facet: apex (0.514, 0.235), mast (0.510, 0.610), base-right (0.685, 0.635) */
        bool in_right = ws_point_in_tri(u, v, 0.514, 0.235, 0.510, 0.610, 0.685, 0.635);
        if (in_right) {
            WsColorF sail_r = ws_lerp_color(ws_rgb(186u, 230u, 253u, 0.95),
                                            ws_rgb(56u, 189u, 248u, 0.92),
                                            ws_clamp01((v - 0.235) / 0.40));
            bg = ws_blend_over(bg, sail_r);
        }

        /* North-star compass diamond sparkle above the mast at (0.50, 0.125) */
        double sx = fabs(u - 0.500);
        double sy = fabs(v - 0.122);
        double star_d = sx * 1.35 + sy * 1.05;
        if (star_d < 0.030 + aa_px) {
            double sa = 1.0 - ws_smoothstep(0.016, 0.028 + aa_px, star_d);
            bg = ws_blend_over(bg, ws_rgb(255u, 255u, 255u, sa * 0.92));
        }
    }

    /* 4. Middle Wave (vibrant cyan -> azure) */
    double w2_y = 0.648 - 0.056 * sin((u + 0.12) * 2.0 * WS_PI * 1.12)
                        + 0.016 * sin(u * 2.0 * WS_PI * 2.4);
    if (v > w2_y - aa_px * 2.0) {
        double a2 = ws_smoothstep(w2_y - aa_px, w2_y + aa_px, v);
        WsColorF c2_top = ws_rgb(34u, 211u, 238u, a2);   /* #22D3EE luminous cyan */
        WsColorF c2_bot = ws_rgb(3u, 105u, 161u, a2);    /* #0369A1 deep cerulean */
        WsColorF c2 = ws_lerp_color(c2_top, c2_bot, ws_clamp01((v - w2_y) / 0.28));
        bg = ws_blend_over(bg, c2);

        /* Crisp crest highlight ribbon along w2_y */
        double crest2 = fabs(v - w2_y);
        if (crest2 < 0.014 + aa_px) {
            double ca = (1.0 - ws_smoothstep(0.002, 0.012 + aa_px, crest2)) * 0.72 * a2;
            bg = ws_blend_over(bg, ws_rgb(224u, 242u, 254u, ca));
        }
    }

    /* 5. Foreground Wave (luminous ice-cyan crest -> electric azure base) */
    double w3_y = 0.742 + 0.052 * sin((u - 0.22) * 2.0 * WS_PI * 1.08)
                        - 0.014 * cos(u * 2.0 * WS_PI * 2.2);
    if (v > w3_y - aa_px * 2.0) {
        double a3 = ws_smoothstep(w3_y - aa_px, w3_y + aa_px, v);
        WsColorF c3_top = ws_rgb(103u, 232u, 249u, a3);  /* #67E8F9 bright aqua */
        WsColorF c3_bot = ws_rgb(14u, 116u, 144u, a3);   /* #0E7490 ocean cyan-indigo */
        WsColorF c3 = ws_lerp_color(c3_top, c3_bot, ws_clamp01((v - w3_y) / 0.24));
        bg = ws_blend_over(bg, c3);

        /* Crisp white sea-foam highlight along foreground wave crest */
        double crest3 = fabs(v - w3_y);
        if (crest3 < 0.016 + aa_px) {
            double ca = (1.0 - ws_smoothstep(0.003, 0.014 + aa_px, crest3)) * 0.88 * a3;
            bg = ws_blend_over(bg, ws_rgb(255u, 255u, 255u, ca));
        }
    }

    /* 6. Inner Glass Rim Bevel around Squircle */
    double rim_band = fabs(sq_dist * 0.455 + 0.010);
    if (sq_dist < 0.0 && rim_band < 0.014 + aa_px) {
        double rim_t = (1.0 - ws_smoothstep(0.002, 0.012 + aa_px, rim_band));
        double light_bias = ws_clamp01(1.15 - (v * 0.85 + u * 0.25));
        bg = ws_blend_over(bg, ws_rgb(186u, 230u, 253u, rim_t * 0.34 * light_bias));
    }

    /* Suppress unused helper warning if compiler inlines segment checks */
    (void)ws_dist_segment;

    bg.a = mask;
    return bg;
}

/*
 * Renders the Waterscape icon at `size x size` into newly malloc'd ARGB8888 buffer
 * using true 4x4 supersampling (16 subpixel samples per output pixel) with
 * premultiplied alpha accumulation.
 */
static uint32_t *ws_render_icon_ssaa4x4(int32_t size) {
    if (size <= 0) return NULL;
    uint32_t *px = (uint32_t *)malloc((size_t)size * (size_t)size * sizeof(uint32_t));
    if (!px) return NULL;

    double inv_sz = 1.0 / (double)size;
    double aa_px  = 0.65 * inv_sz;

    for (int32_t y = 0; y < size; ++y) {
        for (int32_t x = 0; x < size; ++x) {
            double sum_pr = 0.0;
            double sum_pg = 0.0;
            double sum_pb = 0.0;
            double sum_a  = 0.0;

            for (int sy = 0; sy < 4; ++sy) {
                double v = ((double)y + ((double)sy + 0.5) * 0.25) * inv_sz;
                for (int sx = 0; sx < 4; ++sx) {
                    double u = ((double)x + ((double)sx + 0.5) * 0.25) * inv_sz;
                    WsColorF s = ws_sample_icon(u, v, aa_px);
                    sum_pr += s.r * s.a;
                    sum_pg += s.g * s.a;
                    sum_pb += s.b * s.a;
                    sum_a  += s.a;
                }
            }

            double avg_a = ws_clamp01(sum_a * (1.0 / 16.0));
            uint8_t a8 = (uint8_t)floor(avg_a * 255.0 + 0.5);
            uint8_t r8 = 0u, g8 = 0u, b8 = 0u;
            if (sum_a > 1e-6 && a8 > 0u) {
                double r = ws_clamp01(sum_pr / sum_a);
                double g = ws_clamp01(sum_pg / sum_a);
                double b = ws_clamp01(sum_pb / sum_a);
                r8 = (uint8_t)floor(r * 255.0 + 0.5);
                g8 = (uint8_t)floor(g * 255.0 + 0.5);
                b8 = (uint8_t)floor(b * 255.0 + 0.5);
            }
            px[(size_t)y * (size_t)size + (size_t)x] =
                ((uint32_t)a8 << 24) | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | (uint32_t)b8;
        }
    }
    return px;
}

static void ws_put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void ws_put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/*
 * Builds a single Win2000-compatible 32-bpp BGRA bottom-up DIB + 1-bpp AND mask payload
 * for an ICO frame of dimensions `dim x dim` (16, 32, or 48).
 */
static uint8_t *ws_encode_ico_dib_frame(const uint32_t *argb, int32_t dim, size_t *out_len) {
    size_t xor_row_bytes = (size_t)dim * 4u;
    size_t xor_total     = xor_row_bytes * (size_t)dim;
    size_t and_row_bytes = (((size_t)dim + 31u) / 32u) * 4u;
    size_t and_total     = and_row_bytes * (size_t)dim;
    size_t dib_total     = 40u + xor_total + and_total;

    uint8_t *buf = (uint8_t *)calloc(dib_total, 1u);
    if (!buf) return NULL;

    /* BITMAPINFOHEADER (40 bytes); biHeight is doubled in ICO format for XOR + AND masks */
    ws_put_u32le(buf + 0u,  40u);
    ws_put_u32le(buf + 4u,  (uint32_t)dim);
    ws_put_u32le(buf + 8u,  (uint32_t)(dim * 2));
    ws_put_u16le(buf + 12u, 1u);   /* biPlanes */
    ws_put_u16le(buf + 14u, 32u);  /* biBitCount */
    ws_put_u32le(buf + 16u, 0u);   /* BI_RGB */
    ws_put_u32le(buf + 20u, (uint32_t)(xor_total + and_total));
    ws_put_u32le(buf + 24u, 0u);
    ws_put_u32le(buf + 28u, 0u);
    ws_put_u32le(buf + 32u, 0u);
    ws_put_u32le(buf + 36u, 0u);

    uint8_t *xor_dst = buf + 40u;
    uint8_t *and_dst = buf + 40u + xor_total;

    /* Bottom-up scanlines */
    for (int32_t row = 0; row < dim; ++row) {
        int32_t src_y = dim - 1 - row;
        const uint32_t *srow = argb + (size_t)src_y * (size_t)dim;
        uint8_t *xrow = xor_dst + (size_t)row * xor_row_bytes;
        uint8_t *arow = and_dst + (size_t)row * and_row_bytes;

        for (int32_t x = 0; x < dim; ++x) {
            uint32_t px = srow[x];
            uint8_t a = (uint8_t)((px >> 24) & 0xFFu);
            uint8_t r = (uint8_t)((px >> 16) & 0xFFu);
            uint8_t g = (uint8_t)((px >> 8) & 0xFFu);
            uint8_t b = (uint8_t)(px & 0xFFu);
            xrow[x * 4 + 0] = b;
            xrow[x * 4 + 1] = g;
            xrow[x * 4 + 2] = r;
            xrow[x * 4 + 3] = a;

            /* 1-bpp AND mask: bit=1 where transparent (alpha < 128) for Win2000 shell */
            if (a < 128u) {
                arow[x / 8] |= (uint8_t)(0x80u >> (x & 7));
            }
        }
    }

    *out_len = dib_total;
    return buf;
}

/*
 * Writes multi-resolution waterscape.ico:
 *   Entry 0: 16x16 32-bpp BGRA DIB + AND mask
 *   Entry 1: 32x32 32-bpp BGRA DIB + AND mask
 *   Entry 2: 48x48 32-bpp BGRA DIB + AND mask
 *   Entry 3: 256x256 PNG-compressed Vista/Win10/Win11 icon
 */
static bool ws_write_ico_file(const char *filepath,
                              const uint32_t *px16,
                              const uint32_t *px32,
                              const uint32_t *px48,
                              const uint32_t *px256) {
    uint8_t *f16  = NULL, *f32 = NULL, *f48 = NULL, *f256 = NULL;
    size_t   n16  = 0u,   n32  = 0u,   n48  = 0u,   n256  = 0u;

    f16 = ws_encode_ico_dib_frame(px16, 16, &n16);
    f32 = ws_encode_ico_dib_frame(px32, 32, &n32);
    f48 = ws_encode_ico_dib_frame(px48, 48, &n48);
    bool png_ok = arcr_image_encode_png_mem(px256, 256, 256, 256, &f256, &n256);

    if (!f16 || !f32 || !f48 || !png_ok || !f256) {
        free(f16); free(f32); free(f48); free(f256);
        return false;
    }

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        free(f16); free(f32); free(f48); free(f256);
        return false;
    }

    uint8_t hdr[6 + 4 * 16];
    memset(hdr, 0, sizeof(hdr));
    ws_put_u16le(hdr + 0u, 0u); /* idReserved */
    ws_put_u16le(hdr + 2u, 1u); /* idType = 1 (ICON) */
    ws_put_u16le(hdr + 4u, 4u); /* idCount = 4 */

    const uint8_t dims[4]  = {16u, 32u, 48u, 0u}; /* 0 represents 256x256 */
    const size_t  sizes[4] = {n16, n32, n48, n256};
    uint32_t cur_off = (uint32_t)sizeof(hdr);

    for (int i = 0; i < 4; ++i) {
        uint8_t *ent = hdr + 6u + (size_t)i * 16u;
        ent[0] = dims[i];
        ent[1] = dims[i];
        ent[2] = 0u; /* bColorCount */
        ent[3] = 0u; /* bReserved */
        ws_put_u16le(ent + 4u,  1u);  /* wPlanes */
        ws_put_u16le(ent + 6u,  32u); /* wBitCount */
        ws_put_u32le(ent + 8u,  (uint32_t)sizes[i]);
        ws_put_u32le(ent + 12u, cur_off);
        cur_off += (uint32_t)sizes[i];
    }

    bool ok = (fwrite(hdr, 1u, sizeof(hdr), fp) == sizeof(hdr)) &&
              (fwrite(f16, 1u, n16, fp) == n16) &&
              (fwrite(f32, 1u, n32, fp) == n32) &&
              (fwrite(f48, 1u, n48, fp) == n48) &&
              (fwrite(f256, 1u, n256, fp) == n256);

    fclose(fp);
    free(f16);
    free(f32);
    free(f48);
    free(f256);
    return ok;
}

/*
 * Writes wizard_banner.bmp: 492x64 24-bpp uncompressed bottom-up Windows BMP
 * featuring a clean ocean gradient, subtle harmonic wave ribbons, and the
 * 48x48 Waterscape emblem composited on the left.
 */
static bool ws_write_wizard_banner_bmp(const char *filepath, const uint32_t *icon48) {
    const int32_t width  = 492;
    const int32_t height = 64;
    const size_t  row_stride = (((size_t)width * 3u) + 3u) & ~(size_t)3u;
    const size_t  pix_bytes  = row_stride * (size_t)height;
    const size_t  file_bytes = 54u + pix_bytes;

    uint8_t *bmp = (uint8_t *)calloc(file_bytes, 1u);
    if (!bmp) return false;

    /* BITMAPFILEHEADER (14 bytes) */
    bmp[0] = 'B';
    bmp[1] = 'M';
    ws_put_u32le(bmp + 2u,  (uint32_t)file_bytes);
    ws_put_u32le(bmp + 6u,  0u);
    ws_put_u32le(bmp + 10u, 54u);

    /* BITMAPINFOHEADER (40 bytes) */
    ws_put_u32le(bmp + 14u, 40u);
    ws_put_u32le(bmp + 18u, (uint32_t)width);
    ws_put_u32le(bmp + 22u, (uint32_t)height);
    ws_put_u16le(bmp + 26u, 1u);
    ws_put_u16le(bmp + 28u, 24u);
    ws_put_u32le(bmp + 30u, 0u); /* BI_RGB */
    ws_put_u32le(bmp + 34u, (uint32_t)pix_bytes);
    ws_put_u32le(bmp + 38u, 2835u); /* ~72 DPI */
    ws_put_u32le(bmp + 42u, 2835u);
    ws_put_u32le(bmp + 46u, 0u);
    ws_put_u32le(bmp + 50u, 0u);

    for (int32_t y = 0; y < height; ++y) {
        double v = (double)y / (double)(height - 1);
        int32_t bmp_row = height - 1 - y;
        uint8_t *drow = bmp + 54u + (size_t)bmp_row * row_stride;

        for (int32_t x = 0; x < width; ++x) {
            double u = (double)x / (double)(width - 1);

            /* Clean ocean-indigo to slate-azure horizontal + vertical gradient */
            WsColorF left_c  = ws_rgb(9u, 23u, 56u, 1.0);
            WsColorF right_c = ws_rgb(15u, 62u, 118u, 1.0);
            WsColorF bg      = ws_lerp_color(left_c, right_c, u * 0.85 + v * 0.15);

            /* Subtle decorative ocean wave contour across right side of header */
            double wave_a = 0.62 + 0.16 * sin(u * 2.0 * WS_PI * 1.45 - 0.5);
            double wave_b = 0.78 - 0.12 * cos(u * 2.0 * WS_PI * 1.85 + 0.3);
            if (v > wave_a) {
                double t = ws_smoothstep(wave_a, wave_a + 0.04, v);
                bg = ws_blend_over(bg, ws_rgb(14u, 116u, 144u, 0.24 * t));
            }
            if (v > wave_b) {
                double t = ws_smoothstep(wave_b, wave_b + 0.04, v);
                bg = ws_blend_over(bg, ws_rgb(56u, 189u, 248u, 0.20 * t));
            }

            /* Bottom 2px luminous cyan separator line */
            if (y == height - 1) {
                bg = ws_rgb(34u, 211u, 238u, 1.0);
            } else if (y == height - 2) {
                bg = ws_lerp_color(bg, ws_rgb(56u, 189u, 248u, 1.0), 0.45);
            }

            /* Composite 48x48 Waterscape emblem on the left at (12, 8) */
            if (icon48 && x >= 12 && x < 12 + 48 && y >= 8 && y < 8 + 48) {
                uint32_t ipx = icon48[(size_t)(y - 8) * 48u + (size_t)(x - 12)];
                uint8_t ia = (uint8_t)((ipx >> 24) & 0xFFu);
                uint8_t ir = (uint8_t)((ipx >> 16) & 0xFFu);
                uint8_t ig = (uint8_t)((ipx >> 8) & 0xFFu);
                uint8_t ib = (uint8_t)(ipx & 0xFFu);
                bg = ws_blend_over(bg, ws_rgb(ir, ig, ib, (double)ia / 255.0));
            }

            drow[x * 3 + 0] = (uint8_t)floor(ws_clamp01(bg.b) * 255.0 + 0.5);
            drow[x * 3 + 1] = (uint8_t)floor(ws_clamp01(bg.g) * 255.0 + 0.5);
            drow[x * 3 + 2] = (uint8_t)floor(ws_clamp01(bg.r) * 255.0 + 0.5);
        }
    }

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        free(bmp);
        return false;
    }
    bool ok = (fwrite(bmp, 1u, file_bytes, fp) == file_bytes);
    fclose(fp);
    free(bmp);
    return ok;
}

/*
 * Writes waterscape.svg: crisp scalable vector emblem matching the raster brand icon.
 */
static bool ws_write_svg_file(const char *filepath) {
    static const char *const kSvgMarkup =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 256 256\" width=\"256\" height=\"256\">\n"
        "  <defs>\n"
        "    <linearGradient id=\"wsBg\" x1=\"0%\" y1=\"0%\" x2=\"0%\" y2=\"100%\">\n"
        "      <stop offset=\"0%\" stop-color=\"#081330\"/>\n"
        "      <stop offset=\"55%\" stop-color=\"#0E2C60\"/>\n"
        "      <stop offset=\"100%\" stop-color=\"#125294\"/>\n"
        "    </linearGradient>\n"
        "    <radialGradient id=\"wsGlow\" cx=\"50%\" cy=\"42%\" r=\"38%\">\n"
        "      <stop offset=\"0%\" stop-color=\"#38BDF8\" stop-opacity=\"0.35\"/>\n"
        "      <stop offset=\"100%\" stop-color=\"#38BDF8\" stop-opacity=\"0\"/>\n"
        "    </radialGradient>\n"
        "    <linearGradient id=\"wsWaveMid\" x1=\"0%\" y1=\"0%\" x2=\"0%\" y2=\"100%\">\n"
        "      <stop offset=\"0%\" stop-color=\"#22D3EE\"/>\n"
        "      <stop offset=\"100%\" stop-color=\"#0369A1\"/>\n"
        "    </linearGradient>\n"
        "    <linearGradient id=\"wsWaveFront\" x1=\"0%\" y1=\"0%\" x2=\"0%\" y2=\"100%\">\n"
        "      <stop offset=\"0%\" stop-color=\"#67E8F9\"/>\n"
        "      <stop offset=\"100%\" stop-color=\"#0E7490\"/>\n"
        "    </linearGradient>\n"
        "    <clipPath id=\"wsSquircle\">\n"
        "      <rect x=\"12\" y=\"12\" width=\"232\" height=\"232\" rx=\"56\" ry=\"56\"/>\n"
        "    </clipPath>\n"
        "  </defs>\n"
        "  <g clip-path=\"url(#wsSquircle)\">\n"
        "    <rect x=\"12\" y=\"12\" width=\"232\" height=\"232\" fill=\"url(#wsBg)\"/>\n"
        "    <circle cx=\"128\" cy=\"108\" r=\"94\" fill=\"url(#wsGlow)\"/>\n"
        "    <circle cx=\"128\" cy=\"108\" r=\"65\" fill=\"none\" stroke=\"#BAE6FD\" stroke-opacity=\"0.26\" stroke-width=\"2.5\"/>\n"
        "    <path d=\"M0,152 C56,134 98,166 154,148 C196,135 228,150 256,144 L256,256 L0,256 Z\" fill=\"#0284C7\" fill-opacity=\"0.85\"/>\n"
        "    <polygon points=\"128,45 75,160 126,155\" fill=\"#FFFFFF\"/>\n"
        "    <polygon points=\"132,60 130,156 175,162\" fill=\"#7DD3FC\"/>\n"
        "    <polygon points=\"128,25 133,31 128,37 123,31\" fill=\"#FFFFFF\"/>\n"
        "    <path d=\"M0,168 C64,146 116,184 176,162 C212,149 236,160 256,165 L256,256 L0,256 Z\" fill=\"url(#wsWaveMid)\"/>\n"
        "    <path d=\"M0,168 C64,146 116,184 176,162 C212,149 236,160 256,165\" fill=\"none\" stroke=\"#E0F2FE\" stroke-width=\"2.5\" stroke-opacity=\"0.78\"/>\n"
        "    <path d=\"M0,194 C54,210 112,174 174,192 C210,202 236,192 256,186 L256,256 L0,256 Z\" fill=\"url(#wsWaveFront)\"/>\n"
        "    <path d=\"M0,194 C54,210 112,174 174,192 C210,202 236,192 256,186\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"3\" stroke-opacity=\"0.9\"/>\n"
        "  </g>\n"
        "  <rect x=\"13.5\" y=\"13.5\" width=\"229\" height=\"229\" rx=\"54.5\" ry=\"54.5\" fill=\"none\" stroke=\"#BAE6FD\" stroke-opacity=\"0.32\" stroke-width=\"3\"/>\n"
        "</svg>\n";

    FILE *fp = fopen(filepath, "wb");
    if (!fp) return false;
    size_t len = strlen(kSvgMarkup);
    bool ok = (fwrite(kSvgMarkup, 1u, len, fp) == len);
    fclose(fp);
    return ok;
}

static bool ws_ensure_dir_recursive(const char *dir) {
    char tmp[1024];
    size_t len = strlen(dir);
    if (len == 0u || len >= sizeof(tmp)) return false;
    memcpy(tmp, dir, len + 1u);

    for (size_t i = 1u; i < len; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char saved = tmp[i];
            tmp[i] = '\0';
            (void)WS_MKDIR(tmp);
            tmp[i] = saved;
        }
    }
    if (WS_MKDIR(tmp) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

static bool ws_join_path(char *out, size_t cap, const char *dir, const char *leaf) {
    size_t dlen = strlen(dir);
    while (dlen > 0u && (dir[dlen - 1u] == '/' || dir[dlen - 1u] == '\\')) {
        --dlen;
    }
    int n = snprintf(out, cap, "%.*s/%s", (int)dlen, dir, leaf);
    return (n > 0 && (size_t)n < cap);
}

int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        fprintf((argc == 2) ? stdout : stderr,
                "Usage: %s <output_dir>\n"
                "Generates Waterscape 1.0 procedural PNG, ICO, BMP, and SVG brand assets.\n",
                (argc > 0 && argv[0]) ? argv[0] : "gen_brand_assets");
        return (argc == 2) ? 0 : 1;
    }

    const char *out_dir = argv[1];
    if (!ws_ensure_dir_recursive(out_dir)) {
        fprintf(stderr, "gen_brand_assets: failed to create output directory '%s'\n", out_dir);
        return 1;
    }

    static const int32_t kSizes[5] = {256, 64, 48, 32, 16};
    uint32_t *buffers[5] = {NULL, NULL, NULL, NULL, NULL};

    for (size_t i = 0u; i < 5u; ++i) {
        buffers[i] = ws_render_icon_ssaa4x4(kSizes[i]);
        if (!buffers[i]) {
            fprintf(stderr, "gen_brand_assets: out of memory rendering %ldx%ld icon\n",
                    (long)kSizes[i], (long)kSizes[i]);
            for (size_t k = 0u; k < 5u; ++k) free(buffers[k]);
            return 1;
        }

        char png_leaf[64];
        char png_path[1024];
        snprintf(png_leaf, sizeof(png_leaf), "waterscape_%ld.png", (long)kSizes[i]);
        if (!ws_join_path(png_path, sizeof(png_path), out_dir, png_leaf) ||
            !arcr_image_write_png_file(png_path, buffers[i], kSizes[i], kSizes[i], kSizes[i])) {
            fprintf(stderr, "gen_brand_assets: failed writing '%s'\n", png_path);
            for (size_t k = 0u; k < 5u; ++k) free(buffers[k]);
            return 1;
        }
    }

    char ico_path[1024];
    char bmp_path[1024];
    char svg_path[1024];
    bool ok = ws_join_path(ico_path, sizeof(ico_path), out_dir, "waterscape.ico") &&
              ws_write_ico_file(ico_path, buffers[4], buffers[3], buffers[2], buffers[0]) &&
              ws_join_path(bmp_path, sizeof(bmp_path), out_dir, "wizard_banner.bmp") &&
              ws_write_wizard_banner_bmp(bmp_path, buffers[2]) &&
              ws_join_path(svg_path, sizeof(svg_path), out_dir, "waterscape.svg") &&
              ws_write_svg_file(svg_path);

    for (size_t k = 0u; k < 5u; ++k) {
        free(buffers[k]);
    }

    if (!ok) {
        fprintf(stderr, "gen_brand_assets: failed writing ICO/BMP/SVG brand assets\n");
        return 1;
    }

    printf("Waterscape brand assets generated in '%s' (PNG 16..256, ICO, BMP, SVG)\n", out_dir);
    return 0;
}
