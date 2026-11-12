/*
 * Arcradient (MSHTML-Next)
 * accel/arcr_accel.c - Cost-Based Routing, Scalar Reference, SSE1/SWAR & SSE2/SSE3 Kernels
 */
#include "arcr_accel.h"
#include <stdlib.h>
#include <string.h>

#if defined(__SSE__) || defined(_M_IX86) || defined(__x86_64__) || defined(_M_X64)
#  include <xmmintrin.h> /* SSE1 (<xmmintrin.h> works on Pentium III / Win2000 x86) */
#endif

#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
#  include <emmintrin.h> /* SSE2 128-bit integer XMM */
#endif

typedef void (*ArcrScanlineBlendFn)(uint32_t *dst, size_t count, uint32_t src_argb);

struct ArcrAccelContext {
    uint32_t           cpu_flags;
    ArcrAccelBackendId cpu_backend;
    ArcrAccelBackendId gpu_backend;
    ArcrScanlineBlendFn blend_scanline_fn;
    bool               force_cpu;
    bool               gpu_present;
    bool               gpu_disabled_by_fault;
    uint32_t           gpu_error_strikes;
};

/* Exact integer division by 255 for [0..65025]: ((x + 128) + ((x + 128) >> 8)) >> 8 */
static inline uint32_t div255_exact(uint32_t x) {
    uint32_t t = x + 128u;
    return (t + (t >> 8)) >> 8;
}

/* -------------------------------------------------------------------------
 * 1. CPU Scalar Reference Kernel (Works on every CPU & OS)
 * ------------------------------------------------------------------------- */
void arcr_scanline_blend_scalar(uint32_t *dst, size_t count, uint32_t src_argb) {
    uint32_t sa = (src_argb >> 24) & 0xFFu;
    if (sa == 0u || count == 0u) return;
    if (sa == 255u) {
        for (size_t i = 0; i < count; ++i) dst[i] = src_argb;
        return;
    }

    uint32_t sr    = (src_argb >> 16) & 0xFFu;
    uint32_t sg    = (src_argb >> 8) & 0xFFu;
    uint32_t sb    = src_argb & 0xFFu;
    uint32_t inv_a = 255u - sa;

    uint32_t s_ra = sr * sa;
    uint32_t s_ga = sg * sa;
    uint32_t s_ba = sb * sa;

    for (size_t i = 0; i < count; ++i) {
        uint32_t d  = dst[i];
        uint32_t dr = (d >> 16) & 0xFFu;
        uint32_t dg = (d >> 8) & 0xFFu;
        uint32_t db = d & 0xFFu;
        uint32_t da = (d >> 24) & 0xFFu;

        uint32_t out_r = div255_exact(s_ra + dr * inv_a);
        uint32_t out_g = div255_exact(s_ga + dg * inv_a);
        uint32_t out_b = div255_exact(s_ba + db * inv_a);
        uint32_t out_a = sa + div255_exact(da * inv_a);

        dst[i] = (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
    }
}

/* -------------------------------------------------------------------------
 * 2. Legacy x86 SSE1 + 64-bit SWAR Kernel (No SSE2 integer ops required!)
 *
 * Processes R+B and A+G channels in parallel inside 64-bit integer lanes with
 * SSE1 prefetch, guaranteeing zero SSE2 illegal-instruction faults on Pentium III.
 * ------------------------------------------------------------------------- */
void arcr_scanline_blend_sse1_swar(uint32_t *dst, size_t count, uint32_t src_argb) {
    uint32_t sa = (src_argb >> 24) & 0xFFu;
    if (sa == 0u || count == 0u) return;
    if (sa == 255u) {
        for (size_t i = 0; i < count; ++i) dst[i] = src_argb;
        return;
    }

    uint32_t sr    = (src_argb >> 16) & 0xFFu;
    uint32_t sg    = (src_argb >> 8) & 0xFFu;
    uint32_t sb    = src_argb & 0xFFu;
    uint32_t inv_a = 255u - sa;

    /* Pre-pack source premultiplied terms + rounding bias 128 in [R|B] and [0|G] lanes */
    uint64_t src_rb = (((uint64_t)(sr * sa + 128u)) << 32) | (uint64_t)(sb * sa + 128u);
    uint64_t src_ag = (((uint64_t)(255u * sa + 128u)) << 32) | (uint64_t)(sg * sa + 128u);

    for (size_t i = 0; i < count; ++i) {
#if defined(__SSE__) || defined(_M_IX86) || defined(__x86_64__)
        if ((i & 15u) == 0u && i + 16u < count) {
            _mm_prefetch((const char *)(dst + i + 16u), _MM_HINT_T0);
        }
#endif
        uint32_t d = dst[i];
        uint64_t d_rb = (((uint64_t)((d >> 16) & 0xFFu)) << 32) | (uint64_t)(d & 0xFFu);
        uint64_t d_ag = (((uint64_t)((d >> 24) & 0xFFu)) << 32) | (uint64_t)((d >> 8) & 0xFFu);

        uint64_t t_rb = src_rb + d_rb * inv_a;
        uint64_t t_ag = src_ag + d_ag * inv_a;

        t_rb = (t_rb + ((t_rb >> 8) & 0x00FF00FF00FF00FFULL)) >> 8;
        t_ag = (t_ag + ((t_ag >> 8) & 0x00FF00FF00FF00FFULL)) >> 8;

        uint32_t out_r = (uint32_t)((t_rb >> 32) & 0xFFu);
        uint32_t out_b = (uint32_t)(t_rb & 0xFFu);
        uint32_t out_a = (uint32_t)((t_ag >> 32) & 0xFFu);
        uint32_t out_g = (uint32_t)(t_ag & 0xFFu);

        dst[i] = (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
    }
}

/* -------------------------------------------------------------------------
 * 3. x64 Baseline SSE2 / SSE3 128-bit XMM Kernel (4 ARGB8888 pixels per iter)
 * ------------------------------------------------------------------------- */
void arcr_scanline_blend_sse2(uint32_t *dst, size_t count, uint32_t src_argb) {
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
    uint32_t sa = (src_argb >> 24) & 0xFFu;
    if (sa == 0u || count == 0u) return;
    if (sa == 255u) {
        __m128i v_solid = _mm_set1_epi32((int)src_argb);
        size_t i = 0u;
        for (; i + 4u <= count; i += 4u) {
            _mm_storeu_si128((__m128i *)(void *)(dst + i), v_solid);
        }
        for (; i < count; ++i) dst[i] = src_argb;
        return;
    }

    uint32_t sr    = (src_argb >> 16) & 0xFFu;
    uint32_t sg    = (src_argb >> 8) & 0xFFu;
    uint32_t sb    = src_argb & 0xFFu;
    uint32_t inv_a = 255u - sa;

    /* Unpacked 16-bit lanes per pixel: [B, G, R, A] */
    __m128i v_zero  = _mm_setzero_si128();
    __m128i v_inva  = _mm_set1_epi16((short)inv_a);
    __m128i v_round = _mm_set1_epi16(128);
    __m128i v_src16 = _mm_setr_epi16(
        (short)(sb * sa), (short)(sg * sa), (short)(sr * sa), (short)(255u * sa),
        (short)(sb * sa), (short)(sg * sa), (short)(sr * sa), (short)(255u * sa)
    );

    size_t i = 0u;
    for (; i + 4u <= count; i += 4u) {
        __m128i d4 = _mm_loadu_si128((const __m128i *)(const void *)(dst + i));

        __m128i d_lo = _mm_unpacklo_epi8(d4, v_zero);
        __m128i d_hi = _mm_unpackhi_epi8(d4, v_zero);

        __m128i t_lo = _mm_add_epi16(_mm_add_epi16(v_src16, _mm_mullo_epi16(d_lo, v_inva)), v_round);
        __m128i t_hi = _mm_add_epi16(_mm_add_epi16(v_src16, _mm_mullo_epi16(d_hi, v_inva)), v_round);

        t_lo = _mm_srli_epi16(_mm_add_epi16(t_lo, _mm_srli_epi16(t_lo, 8)), 8);
        t_hi = _mm_srli_epi16(_mm_add_epi16(t_hi, _mm_srli_epi16(t_hi, 8)), 8);

        __m128i packed = _mm_packus_epi16(t_lo, t_hi);
        _mm_storeu_si128((__m128i *)(void *)(dst + i), packed);
    }
    if (i < count) {
        arcr_scanline_blend_scalar(dst + i, count - i, src_argb);
    }
#else
    arcr_scanline_blend_sse1_swar(dst, count, src_argb);
#endif
}

/* -------------------------------------------------------------------------
 * 4. Context Creation, Cost Estimation & Automatic Fault Fallback
 * ------------------------------------------------------------------------- */
ArcrAccelContext *arcr_accel_create(uint32_t allowed_cpu_flags, bool force_cpu,
                                    bool simulate_gpu_driver_present) {
    ArcrAccelContext *ctx = (ArcrAccelContext *)calloc(1u, sizeof(ArcrAccelContext));
    if (!ctx) return NULL;

    uint32_t hw = arcr_detect_cpu_flags() & allowed_cpu_flags;
    ctx->cpu_flags = hw;
    ctx->force_cpu = force_cpu;

    if (hw & ARCR_CPU_SSE2) {
        ctx->cpu_backend       = ARCR_BACKEND_CPU_SSE2_SSE3;
        ctx->blend_scanline_fn = arcr_scanline_blend_sse2;
    } else if (hw & ARCR_CPU_SSE1) {
        ctx->cpu_backend       = ARCR_BACKEND_CPU_SSE1_SWAR;
        ctx->blend_scanline_fn = arcr_scanline_blend_sse1_swar;
    } else {
        ctx->cpu_backend       = ARCR_BACKEND_CPU_SCALAR;
        ctx->blend_scanline_fn = arcr_scanline_blend_scalar;
    }

    ctx->gpu_present           = simulate_gpu_driver_present && !force_cpu;
    ctx->gpu_disabled_by_fault = false;
    ctx->gpu_error_strikes     = 0u;
    ctx->gpu_backend           = (arcr_host_compile_arch() == ARCR_ARCH_X86_32)
                                     ? ARCR_BACKEND_GPU_D3D9
                                     : ARCR_BACKEND_GPU_MODERN;
    return ctx;
}

void arcr_accel_destroy(ArcrAccelContext *ctx) {
    free(ctx);
}

ArcrAccelBackendId arcr_accel_active_cpu_backend(const ArcrAccelContext *ctx) {
    return ctx ? ctx->cpu_backend : ARCR_BACKEND_CPU_SCALAR;
}

bool arcr_accel_is_gpu_healthy(const ArcrAccelContext *ctx) {
    return ctx && ctx->gpu_present && !ctx->force_cpu && !ctx->gpu_disabled_by_fault;
}

bool arcr_accel_did_fallback_occur(const ArcrAccelContext *ctx) {
    return ctx ? ctx->gpu_disabled_by_fault : false;
}

const char *arcr_accel_backend_name(ArcrAccelBackendId id) {
    switch (id) {
        case ARCR_BACKEND_CPU_SCALAR:    return "CPU-Scalar";
        case ARCR_BACKEND_CPU_SSE1_SWAR: return "CPU-SSE1+SWAR";
        case ARCR_BACKEND_CPU_SSE2_SSE3: return "CPU-SSE2/SSE3";
        case ARCR_BACKEND_GPU_D3D9:      return "GPU-Direct3D9";
        case ARCR_BACKEND_GPU_OPENGL:    return "GPU-OpenGL-ICD";
        case ARCR_BACKEND_GPU_MODERN:    return "GPU-Modern(D3D11/VK)";
        default:                         return "Unknown";
    }
}

void arcr_accel_reject_software_gl_renderer(ArcrAccelContext *ctx, const char *gl_renderer_str) {
    if (!ctx || !gl_renderer_str) return;
    /* Reject Win32 Microsoft OpenGL 1.1 software fallback ("GDI Generic") */
    if (arcr_str_eq_nocase(gl_renderer_str, "GDI Generic") ||
        arcr_str_starts_with_nocase(gl_renderer_str, "llvmpipe")) {
        ctx->gpu_present           = false;
        ctx->gpu_disabled_by_fault = true;
    }
}

void arcr_accel_report_gpu_fault(ArcrAccelContext *ctx, bool fatal_device_lost) {
    if (!ctx) return;
    ctx->gpu_error_strikes++;
    if (fatal_device_lost || ctx->gpu_error_strikes >= 3u) {
        ctx->gpu_disabled_by_fault = true;
        ctx->gpu_present           = false;
    }
}

ArcrAccelBackendId arcr_accel_choose_route(
    const ArcrAccelContext *ctx,
    ArcrAccelOpKind         op,
    int32_t                 pixel_count,
    bool                    already_on_gpu,
    ArcrAccelCostBreakdown *out_cpu_cost,
    ArcrAccelCostBreakdown *out_gpu_cost
) {
    if (!ctx) return ARCR_BACKEND_CPU_SCALAR;
    uint32_t px = (pixel_count > 0) ? (uint32_t)pixel_count : 1u;

    /* CPU cost scales linearly with pixels, accelerated by SIMD factor */
    uint32_t simd_div = (ctx->cpu_backend == ARCR_BACKEND_CPU_SSE2_SSE3) ? 4u
                      : (ctx->cpu_backend == ARCR_BACKEND_CPU_SSE1_SWAR) ? 2u : 1u;
    ArcrAccelCostBreakdown cpu_c = {
        .setup_cost    = 2u,
        .upload_cost   = 0u,
        .exec_cost     = ARCR_MAX(1u, (px / 64u) / simd_div),
        .sync_cost     = 0u,
        .readback_cost = 0u,
        .risk_cost     = 0u,
        .total_cost    = 0u
    };
    cpu_c.total_cost = cpu_c.setup_cost + cpu_c.exec_cost;

    /* GPU cost pays fixed setup/sync + PCI/AGP upload & readback unless resident */
    ArcrAccelCostBreakdown gpu_c = {
        .setup_cost    = 180u,
        .upload_cost   = already_on_gpu ? 0u : (px / 32u),
        .exec_cost     = ARCR_MAX(1u, px / 512u),
        .sync_cost     = 120u,
        .readback_cost = (op == ARCR_OP_LARGE_COMPOSITE || op == ARCR_OP_GPU_RESIDENT_TX)
                             ? 0u : (px / 16u),
        .risk_cost     = ctx->gpu_error_strikes * 400u,
        .total_cost    = 0u
    };
    gpu_c.total_cost = gpu_c.setup_cost + gpu_c.upload_cost + gpu_c.exec_cost +
                       gpu_c.sync_cost + gpu_c.readback_cost + gpu_c.risk_cost;

    if (out_cpu_cost) *out_cpu_cost = cpu_c;
    if (out_gpu_cost) *out_gpu_cost = gpu_c;

    if (!arcr_accel_is_gpu_healthy(ctx)) {
        return ctx->cpu_backend;
    }

    /* Small rects, glyph blits, and static tile updates stay on CPU! */
    if (op == ARCR_OP_FILL_RECT || op == ARCR_OP_GLYPH_MASK_BLIT) {
        return ctx->cpu_backend;
    }

    if (gpu_c.total_cost < cpu_c.total_cost) {
        return ctx->gpu_backend;
    }
    return ctx->cpu_backend;
}

/* -------------------------------------------------------------------------
 * 5. Clipped Framebuffer Drawing Primitives
 * ------------------------------------------------------------------------- */
static bool clip_rect(int32_t fb_w, int32_t fb_h, ArcrRect in,
                      int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1) {
    if (in.width <= 0 || in.height <= 0 || fb_w <= 0 || fb_h <= 0) return false;
    *x0 = ARCR_MAX(0, in.x);
    *y0 = ARCR_MAX(0, in.y);
    *x1 = ARCR_MIN(fb_w, in.x + in.width);
    *y1 = ARCR_MIN(fb_h, in.y + in.height);
    return (*x0 < *x1) && (*y0 < *y1);
}

void arcr_accel_fill_rect(ArcrAccelContext *ctx, uint32_t *dst_argb,
                          int32_t fb_w, int32_t fb_h, int32_t stride_px,
                          ArcrRect rect, uint32_t argb_color) {
    (void)ctx;
    if (!dst_argb) return;
    int32_t x0, y0, x1, y1;
    if (!clip_rect(fb_w, fb_h, rect, &x0, &y0, &x1, &y1)) return;
    size_t span = (size_t)(x1 - x0);
    for (int32_t y = y0; y < y1; ++y) {
        uint32_t *row = dst_argb + (size_t)y * (size_t)stride_px + (size_t)x0;
        for (size_t i = 0; i < span; ++i) {
            row[i] = argb_color;
        }
    }
}

void arcr_accel_blend_rect(ArcrAccelContext *ctx, uint32_t *dst_argb,
                           int32_t fb_w, int32_t fb_h, int32_t stride_px,
                           ArcrRect rect, uint32_t argb_color) {
    if (!dst_argb) return;
    int32_t x0, y0, x1, y1;
    if (!clip_rect(fb_w, fb_h, rect, &x0, &y0, &x1, &y1)) return;
    ArcrScanlineBlendFn fn = (ctx && ctx->blend_scanline_fn)
                                 ? ctx->blend_scanline_fn
                                 : arcr_scanline_blend_scalar;
    size_t span = (size_t)(x1 - x0);
    for (int32_t y = y0; y < y1; ++y) {
        uint32_t *row = dst_argb + (size_t)y * (size_t)stride_px + (size_t)x0;
        fn(row, span, argb_color);
    }
}

void arcr_accel_blit_glyph_alpha(ArcrAccelContext *ctx, uint32_t *dst_argb,
                                 int32_t fb_w, int32_t fb_h, int32_t stride_px,
                                 int32_t dst_x, int32_t dst_y,
                                 const uint8_t *alpha_mask, int32_t mask_w, int32_t mask_h,
                                 uint32_t text_argb) {
    (void)ctx;
    if (!dst_argb || !alpha_mask || mask_w <= 0 || mask_h <= 0) return;
    uint32_t base_a = (text_argb >> 24) & 0xFFu;
    if (base_a == 0u) return;
    uint32_t sr = (text_argb >> 16) & 0xFFu;
    uint32_t sg = (text_argb >> 8) & 0xFFu;
    uint32_t sb = text_argb & 0xFFu;

    for (int32_t my = 0; my < mask_h; ++my) {
        int32_t py = dst_y + my;
        if (py < 0 || py >= fb_h) continue;
        uint32_t *row = dst_argb + (size_t)py * (size_t)stride_px;
        const uint8_t *mrow = alpha_mask + (size_t)my * (size_t)mask_w;

        for (int32_t mx = 0; mx < mask_w; ++mx) {
            int32_t px = dst_x + mx;
            if (px < 0 || px >= fb_w) continue;
            uint32_t cov = mrow[mx];
            if (cov == 0u) continue;
            uint32_t sa = (base_a == 255u) ? cov : div255_exact(cov * base_a);
            if (sa == 255u) {
                row[px] = 0xFF000000u | (sr << 16) | (sg << 8) | sb;
            } else {
                uint32_t d     = row[px];
                uint32_t inv_a = 255u - sa;
                uint32_t out_r = div255_exact(sr * sa + ((d >> 16) & 0xFFu) * inv_a);
                uint32_t out_g = div255_exact(sg * sa + ((d >> 8) & 0xFFu) * inv_a);
                uint32_t out_b = div255_exact(sb * sa + (d & 0xFFu) * inv_a);
                row[px]        = 0xFF000000u | (out_r << 16) | (out_g << 8) | out_b;
            }
        }
    }
}
