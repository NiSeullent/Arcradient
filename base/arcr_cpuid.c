/*
 * Arcradient (MSHTML-Next)
 * base/arcr_cpuid.c - Runtime x86/x64 SIMD Feature Detection (Win2000-safe)
 */
#include "arcr_base.h"

#if (defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64))
#  if defined(_MSC_VER)
#    include <intrin.h>
static void arcr_cpuid_ex(uint32_t leaf, uint32_t subleaf, uint32_t regs[4]) {
    int tmp[4] = {0, 0, 0, 0};
    __cpuidex(tmp, (int)leaf, (int)subleaf);
    for (int i = 0; i < 4; ++i) regs[i] = (uint32_t)tmp[i];
}
static uint64_t arcr_xgetbv0(void) {
    return _xgetbv(0);
}
#  else
#    include <cpuid.h>
static void arcr_cpuid_ex(uint32_t leaf, uint32_t subleaf, uint32_t regs[4]) {
    __cpuid_count(leaf, subleaf, regs[0], regs[1], regs[2], regs[3]);
}
static uint64_t arcr_xgetbv0(void) {
    uint32_t eax = 0u, edx = 0u;
    __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((uint64_t)edx << 32) | eax;
}
#  endif
#endif

uint32_t arcr_detect_cpu_flags(void) {
    uint32_t flags = ARCR_CPU_SCALAR;

#if (defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64))
    uint32_t regs[4] = {0, 0, 0, 0};
    arcr_cpuid_ex(0u, 0u, regs);
    uint32_t max_leaf = regs[0];
    if (max_leaf >= 1u) {
        arcr_cpuid_ex(1u, 0u, regs);
        uint32_t ecx = regs[2];
        uint32_t edx = regs[3];

        if (edx & (1u << 23)) flags |= ARCR_CPU_MMX;
        if (edx & (1u << 25)) flags |= ARCR_CPU_SSE1;
        if (edx & (1u << 26)) flags |= ARCR_CPU_SSE2;
        if (ecx & (1u << 0))  flags |= ARCR_CPU_SSE3;
        if (ecx & (1u << 9))  flags |= ARCR_CPU_SSSE3;
        if (ecx & (1u << 19)) flags |= ARCR_CPU_SSE4_1;
        if (ecx & (1u << 20)) flags |= ARCR_CPU_SSE4_2;

        /* OSXSAVE (bit 27) + AVX (bit 28): verify OS saves XMM & YMM state!
         * Note: Windows 2000 / XP do NOT save YMM registers even if run in a modern VM! */
        bool osxsave = (ecx & (1u << 27)) != 0u;
        bool avx_hw  = (ecx & (1u << 28)) != 0u;
        if (osxsave && avx_hw) {
            uint64_t xcr0 = arcr_xgetbv0();
            if ((xcr0 & 0x6u) == 0x6u) {
                flags |= ARCR_CPU_AVX;
                if (max_leaf >= 7u) {
                    uint32_t regs7[4] = {0, 0, 0, 0};
                    arcr_cpuid_ex(7u, 0u, regs7);
                    if (regs7[1] & (1u << 5)) {
                        flags |= ARCR_CPU_AVX2;
                    }
                }
            }
        }
    }
#endif

    return flags;
}

const char *arcr_cpu_simd_tier_name(uint32_t flags) {
    if (flags & ARCR_CPU_AVX2)   return "AVX2";
    if (flags & ARCR_CPU_SSE4_2) return "SSE4.2";
    if (flags & ARCR_CPU_SSE3)   return "SSE3+SSE2";
    if (flags & ARCR_CPU_SSE2)   return "SSE2";
    if (flags & ARCR_CPU_SSE1)   return "SSE1+SWAR";
    return "CPU-Scalar";
}
