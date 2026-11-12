/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * accel/arcr_accel_avx2.c - 256-bit AVX2 8-Pixel Scanline Fill & Bit-Exact Blend Kernels
 *
 * Compiled as a dedicated translation unit so 32-bit Legacy x86 (Win2000 SSE1)
 * and baseline x64 (SSE2/SSE3) builds cleanly fall back when __AVX2__ is absent,
 * while AVX2-enabled builds process 8-16 ARGB8888 pixels per iteration with
 * 100% bit-for-bit identical output to arcr_scanline_blend_scalar.
 */
#include "arcr_accel.h"
#include <string.h>

#if defined(__AVX2__)
#  include <immintrin.h>
#endif

bool arcr_compiled_with_avx2_kernel(void) {
#if defined(__AVX2__)
    return true;
#else
    return false;
#endif
}

void arcr_scanline_fill_u32_avx2(uint32_t *dst, size_t count, uint32_t argb) {
    if (!dst || count == 0u) return;

#if defined(__AVX2__)
    __m256i v_fill = _mm256_set1_epi32((int)argb);
    size_t  i      = 0u;

    for (; i + 16u <= count; i += 16u) {
        _mm256_storeu_si256((__m256i *)(void *)(dst + i), v_fill);
        _mm256_storeu_si256((__m256i *)(void *)(dst + i + 8u), v_fill);
    }
    for (; i + 8u <= count; i += 8u) {
        _mm256_storeu_si256((__m256i *)(void *)(dst + i), v_fill);
    }
    for (; i < count; ++i) {
        dst[i] = argb;
    }
#else
    arcr_scanline_fill_u32_scalar(dst, count, argb);
#endif
}

void arcr_scanline_blend_avx2(uint32_t *dst, size_t count, uint32_t src_argb) {
    if (!dst || count == 0u) return;

#if defined(__AVX2__)
    uint32_t sa = (src_argb >> 24) & 0xFFu;
    if (sa == 0u) return;
    if (sa == 255u) {
        arcr_scanline_fill_u32_avx2(dst, count, src_argb);
        return;
    }

    uint32_t sr    = (src_argb >> 16) & 0xFFu;
    uint32_t sg    = (src_argb >> 8) & 0xFFu;
    uint32_t sb    = src_argb & 0xFFu;
    uint32_t inv_a = 255u - sa;

    /*
     * Pre-pack 16-bit premultiplied source + rounding bias 128 in little-endian
     * ARGB8888 channel order [B, G, R, A] so every channel computes:
     *   t = s_ca + d_c * (255 - sa) + 128
     *   out_c = (t + (t >> 8)) >> 8
     * Note: for alpha, s_aa = 255 * sa, and div255_exact(255*sa + da*inv_a)
     * is algebraically identical to sa + div255_exact(da*inv_a) for all [0..255].
     */
    uint64_t u64_src_bias =
        ((uint64_t)(255u * sa + 128u) << 48) |
        ((uint64_t)(sr * sa + 128u) << 32) |
        ((uint64_t)(sg * sa + 128u) << 16) |
        (uint64_t)(sb * sa + 128u);
    long long lane64 = 0;
    memcpy(&lane64, &u64_src_bias, sizeof(lane64));

    __m256i v_src_bias = _mm256_set1_epi64x(lane64);
    __m256i v_inva     = _mm256_set1_epi16((short)inv_a);
    __m256i v_zero     = _mm256_setzero_si256();

    size_t i = 0u;
    for (; i + 8u <= count; i += 8u) {
        __m256i d8 = _mm256_loadu_si256((const __m256i *)(const void *)(dst + i));

        /* In-lane 128-bit unpacks pair naturally with in-lane _mm256_packus_epi16 */
        __m256i d_lo = _mm256_unpacklo_epi8(d8, v_zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d8, v_zero);

        __m256i t_lo = _mm256_add_epi16(v_src_bias, _mm256_mullo_epi16(d_lo, v_inva));
        __m256i t_hi = _mm256_add_epi16(v_src_bias, _mm256_mullo_epi16(d_hi, v_inva));

        t_lo = _mm256_srli_epi16(_mm256_add_epi16(t_lo, _mm256_srli_epi16(t_lo, 8)), 8);
        t_hi = _mm256_srli_epi16(_mm256_add_epi16(t_hi, _mm256_srli_epi16(t_hi, 8)), 8);

        __m256i packed = _mm256_packus_epi16(t_lo, t_hi);
        _mm256_storeu_si256((__m256i *)(void *)(dst + i), packed);
    }

    if (i < count) {
        arcr_scanline_blend_scalar(dst + i, count - i, src_argb);
    }
#else
    arcr_scanline_blend_scalar(dst, count, src_argb);
#endif
}
