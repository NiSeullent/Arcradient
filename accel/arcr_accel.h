/*
 * Arcradient (MSHTML-Next)
 * accel/arcr_accel.h - Universal Acceleration Router, Cost Model & CPU/SIMD Kernels
 */
#ifndef ARCR_ACCEL_H
#define ARCR_ACCEL_H

#include "../base/arcr_base.h"
#include "../include/arcradient_embed.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum ArcrAccelBackendId {
    ARCR_BACKEND_CPU_SCALAR    = 0, /* Reference universal path */
    ARCR_BACKEND_CPU_SSE1_SWAR = 1, /* Legacy x86 SSE1 float + 64-bit SWAR integer */
    ARCR_BACKEND_CPU_SSE2_SSE3 = 2, /* Classic/Modern x64 128-bit XMM integer+float */
    ARCR_BACKEND_GPU_D3D9      = 3, /* Legacy Win2000/XP XPDM Direct3D 9.0c */
    ARCR_BACKEND_GPU_OPENGL    = 4, /* Verified ICD OpenGL (rejects "GDI Generic") */
    ARCR_BACKEND_GPU_MODERN    = 5  /* Direct3D 11 / Vulkan / Metal */
} ArcrAccelBackendId;

typedef enum ArcrAccelOpKind {
    ARCR_OP_FILL_RECT       = 0,
    ARCR_OP_BLEND_RECT      = 1,
    ARCR_OP_GLYPH_MASK_BLIT = 2,
    ARCR_OP_LARGE_COMPOSITE = 3,
    ARCR_OP_GPU_RESIDENT_TX = 4
} ArcrAccelOpKind;

typedef struct ArcrAccelCostBreakdown {
    uint32_t setup_cost;
    uint32_t upload_cost;
    uint32_t exec_cost;
    uint32_t sync_cost;
    uint32_t readback_cost;
    uint32_t risk_cost;
    uint32_t total_cost;
} ArcrAccelCostBreakdown;

typedef struct ArcrAccelContext ArcrAccelContext;

ArcrAccelContext *arcr_accel_create(uint32_t allowed_cpu_flags, bool force_cpu,
                                    bool simulate_gpu_driver_present);
void              arcr_accel_destroy(ArcrAccelContext *ctx);

ArcrAccelBackendId arcr_accel_active_cpu_backend(const ArcrAccelContext *ctx);
bool               arcr_accel_is_gpu_healthy(const ArcrAccelContext *ctx);
bool               arcr_accel_did_fallback_occur(const ArcrAccelContext *ctx);
const char        *arcr_accel_backend_name(ArcrAccelBackendId id);

/* Cost estimator: C_total = setup + upload + exec + sync + readback + risk */
ArcrAccelBackendId arcr_accel_choose_route(
    const ArcrAccelContext *ctx,
    ArcrAccelOpKind         op,
    int32_t                 pixel_count,
    bool                    already_on_gpu,
    ArcrAccelCostBreakdown *out_cpu_cost,
    ArcrAccelCostBreakdown *out_gpu_cost
);

/* Fault injection / driver failure reporting (e.g. D3DERR_DEVICELOST, OpenGL "GDI Generic") */
void arcr_accel_reject_software_gl_renderer(ArcrAccelContext *ctx, const char *gl_renderer_str);
void arcr_accel_report_gpu_fault(ArcrAccelContext *ctx, bool fatal_device_lost);

/* Core framebuffer operations (dispatched to verified SIMD or Scalar reference) */
void arcr_accel_fill_rect(ArcrAccelContext *ctx, uint32_t *dst_argb,
                          int32_t fb_w, int32_t fb_h, int32_t stride_px,
                          ArcrRect rect, uint32_t argb_color);

void arcr_accel_blend_rect(ArcrAccelContext *ctx, uint32_t *dst_argb,
                           int32_t fb_w, int32_t fb_h, int32_t stride_px,
                           ArcrRect rect, uint32_t argb_color);

void arcr_accel_blit_glyph_alpha(ArcrAccelContext *ctx, uint32_t *dst_argb,
                                 int32_t fb_w, int32_t fb_h, int32_t stride_px,
                                 int32_t dst_x, int32_t dst_y,
                                 const uint8_t *alpha_mask, int32_t mask_w, int32_t mask_h,
                                 uint32_t text_argb);

/* Direct exposure of reference vs SIMD scanline blenders for bit-exact verification tests */
void arcr_scanline_blend_scalar(uint32_t *dst, size_t count, uint32_t src_argb);
void arcr_scanline_blend_sse1_swar(uint32_t *dst, size_t count, uint32_t src_argb);
void arcr_scanline_blend_sse2(uint32_t *dst, size_t count, uint32_t src_argb);

#endif /* ARCR_ACCEL_H */
