/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * accel/arcr_accel.h - Full Modern + Legacy Hardware Acceleration Router & SIMD Kernels
 *
 * Supports:
 * - CPU SIMD Tiers: Scalar -> SSE1+SWAR (Win2000 P3) -> SSE2/SSE3 -> AVX2 (256-bit 8-pixel)
 * - Legacy GPU Tiers: DirectDraw 7 (Win2000 2D) -> Direct3D 9.0c/9Ex -> OpenGL Hardware ICD
 * - Modern GPU Tiers: Direct3D 11 + DXGI Flip -> Vulkan 1.x Dynamic Loader -> GLX/EGL
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
    ARCR_BACKEND_CPU_SSE2_SSE3 = 2, /* Classic/Modern x64 128-bit XMM (4 pixels/op) */
    ARCR_BACKEND_CPU_AVX2      = 3, /* Modern x64 256-bit YMM (8 pixels/op) */
    ARCR_BACKEND_GPU_DDRAW7    = 4, /* Legacy Win2000 DirectDraw 7 2D hardware surface */
    ARCR_BACKEND_GPU_D3D9      = 5, /* Legacy Win2000/XP/Vista Direct3D 9.0c / 9Ex */
    ARCR_BACKEND_GPU_OPENGL    = 6, /* Verified ICD OpenGL / GLX / EGL (rejects "GDI Generic") */
    ARCR_BACKEND_GPU_D3D11     = 7, /* Modern Windows Direct3D 11 + DXGI Flip SwapChain */
    ARCR_BACKEND_GPU_VULKAN    = 8, /* Modern Cross-Platform Vulkan 1.x Explicit Queue */
    ARCR_BACKEND_GPU_MODERN    = ARCR_BACKEND_GPU_D3D11
} ArcrAccelBackendId;

typedef enum ArcrAccelOpKind {
    ARCR_OP_FILL_RECT       = 0,
    ARCR_OP_BLEND_RECT      = 1,
    ARCR_OP_GLYPH_MASK_BLIT = 2,
    ARCR_OP_IMAGE_SCALE     = 3,
    ARCR_OP_LARGE_COMPOSITE = 4,
    ARCR_OP_GPU_RESIDENT_TX = 5
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

typedef struct ArcrGpuProbeResult {
    ArcrAccelBackendId best_gpu_backend;
    bool               has_d3d11;
    bool               has_vulkan;
    bool               has_d3d9;
    bool               has_ddraw7;
    bool               has_opengl_icd;
    char               adapter_description[128];
} ArcrGpuProbeResult;

typedef struct ArcrAccelContext ArcrAccelContext;

ArcrAccelContext *arcr_accel_create(uint32_t allowed_cpu_flags, bool force_cpu,
                                    bool probe_or_simulate_gpu);
void              arcr_accel_destroy(ArcrAccelContext *ctx);

ArcrAccelBackendId arcr_accel_active_cpu_backend(const ArcrAccelContext *ctx);
ArcrAccelBackendId arcr_accel_active_gpu_backend(const ArcrAccelContext *ctx);
void               arcr_accel_get_gpu_probe(const ArcrAccelContext *ctx, ArcrGpuProbeResult *out_probe);
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

/* Core SIMD-accelerated framebuffer operations */
void arcr_accel_fill_rect(ArcrAccelContext *ctx, uint32_t *dst_argb,
                          int32_t fb_w, int32_t fb_h, int32_t stride_px,
                          ArcrRect rect, uint32_t argb_color);

void arcr_accel_blend_rect(ArcrAccelContext *ctx, uint32_t *dst_argb,
                           int32_t fb_w, int32_t fb_h, int32_t stride_px,
                           ArcrRect rect, uint32_t argb_color);

/* Anti-aliased rounded rectangle fill & border + subtle elevation shadow */
void arcr_accel_fill_rounded_rect(ArcrAccelContext *ctx, uint32_t *dst_argb,
                                  int32_t fb_w, int32_t fb_h, int32_t stride_px,
                                  ArcrRect rect, int32_t radius_px, uint32_t argb_color);

void arcr_accel_stroke_rounded_rect(ArcrAccelContext *ctx, uint32_t *dst_argb,
                                    int32_t fb_w, int32_t fb_h, int32_t stride_px,
                                    ArcrRect rect, int32_t radius_px, int32_t border_px,
                                    uint32_t border_argb);

void arcr_accel_draw_box_shadow(ArcrAccelContext *ctx, uint32_t *dst_argb,
                                int32_t fb_w, int32_t fb_h, int32_t stride_px,
                                ArcrRect rect, int32_t radius_px, int32_t blur_px,
                                int32_t offset_y, uint32_t shadow_argb);

void arcr_accel_blit_glyph_alpha(ArcrAccelContext *ctx, uint32_t *dst_argb,
                                 int32_t fb_w, int32_t fb_h, int32_t stride_px,
                                 int32_t dst_x, int32_t dst_y,
                                 const uint8_t *alpha_mask, int32_t mask_w, int32_t mask_h,
                                 uint32_t text_argb);

void arcr_accel_blit_image_argb(ArcrAccelContext *ctx, uint32_t *dst_argb,
                                int32_t fb_w, int32_t fb_h, int32_t stride_px,
                                ArcrRect dst_rect,
                                const uint32_t *src_argb, int32_t src_w, int32_t src_h,
                                bool bilinear_smooth);

/* Direct exposure of reference vs SIMD kernels for bit-exact verification & benchmarks */
void arcr_scanline_fill_u32_scalar(uint32_t *dst, size_t count, uint32_t argb);
void arcr_scanline_fill_u32_avx2(uint32_t *dst, size_t count, uint32_t argb);

void arcr_scanline_blend_scalar(uint32_t *dst, size_t count, uint32_t src_argb);
void arcr_scanline_blend_sse1_swar(uint32_t *dst, size_t count, uint32_t src_argb);
void arcr_scanline_blend_sse2(uint32_t *dst, size_t count, uint32_t src_argb);
void arcr_scanline_blend_avx2(uint32_t *dst, size_t count, uint32_t src_argb);
bool arcr_compiled_with_avx2_kernel(void);

/* Dynamic GPU Hardware Probing (D3D11 / Vulkan / D3D9 / DDraw7 / OpenGL ICD) */
void arcr_gpu_probe_system(ArcrGpuProbeResult *out_probe);

#endif /* ARCR_ACCEL_H */
