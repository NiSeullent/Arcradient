/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * accel/arcr_accel.c - Hardware Acceleration Router, Dynamic GPU Probing & 2D SIMD Primitives
 *
 * Pure C11 & Win2000 SP4+ safe:
 * - Zero direct OS headers (<windows.h> / <dlfcn.h>); all dynamic probing uses arcr_platform_dl*.
 * - CPU SIMD tiers: Scalar -> SSE1+SWAR -> SSE2/SSE3 -> AVX2 (256-bit 8-pixel).
 * - GPU tiers: DirectDraw 7 -> Direct3D 9 -> OpenGL ICD -> Direct3D 11 + DXGI -> Vulkan 1.x.
 */
#include "arcr_accel.h"
#include "../platform/arcr_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SSE__) || defined(_M_IX86) || defined(__x86_64__) || defined(_M_X64)
#  include <xmmintrin.h> /* SSE1 (<xmmintrin.h> works on Pentium III / Win2000 x86) */
#endif

#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
#  include <emmintrin.h> /* SSE2 128-bit integer XMM */
#endif

#if defined(_WIN32) && (defined(_M_IX86) || defined(__i386__))
#  define ARCR_ABI_STDCALL __stdcall
#else
#  define ARCR_ABI_STDCALL
#endif

typedef void (*ArcrScanlineFillFn)(uint32_t *dst, size_t count, uint32_t argb);
typedef void (*ArcrScanlineBlendFn)(uint32_t *dst, size_t count, uint32_t src_argb);

struct ArcrAccelContext {
    uint32_t            cpu_flags;
    ArcrAccelBackendId  cpu_backend;
    ArcrAccelBackendId  gpu_backend;
    ArcrScanlineFillFn  fill_scanline_fn;
    ArcrScanlineBlendFn blend_scanline_fn;
    ArcrGpuProbeResult  gpu_probe;
    bool                force_cpu;
    bool                gpu_present;
    bool                gpu_disabled_by_fault;
    uint32_t            gpu_error_strikes;
};

/* Exact integer division by 255 for [0..65025]: ((x + 128) + ((x + 128) >> 8)) >> 8 */
static inline uint32_t div255_exact(uint32_t x) {
    uint32_t t = x + 128u;
    return (t + (t >> 8)) >> 8;
}

/* Fast exact integer square root for 32-bit unsigned values */
static uint32_t isqrt_u32(uint32_t n) {
    if (n == 0u) return 0u;
    uint32_t res = 0u;
    uint32_t bit = 1u << 30;
    while (bit > n) {
        bit >>= 2;
    }
    while (bit != 0u) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

/* Case-insensitive substring search for software renderer rejection */
static bool str_contains_nocase(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    size_t nlen = strlen(needle);
    if (nlen == 0u) return true;
    size_t hlen = strlen(haystack);
    if (hlen < nlen) return false;
    for (size_t i = 0u; i + nlen <= hlen; ++i) {
        if (arcr_str_eq_nocase_n(haystack + i, nlen, needle, nlen)) {
            return true;
        }
    }
    return false;
}

static bool is_software_renderer_string(const char *renderer) {
    if (!renderer || renderer[0] == '\0') return false;
    return str_contains_nocase(renderer, "GDI Generic") ||
           str_contains_nocase(renderer, "llvmpipe") ||
           str_contains_nocase(renderer, "softpipe") ||
           str_contains_nocase(renderer, "Software Rasterizer") ||
           str_contains_nocase(renderer, "Microsoft Basic Render Driver");
}

/* -------------------------------------------------------------------------
 * 1. CPU Scalar & SSE1/SSE2 Scanline Fill + Blend Kernels
 * ------------------------------------------------------------------------- */
void arcr_scanline_fill_u32_scalar(uint32_t *dst, size_t count, uint32_t argb) {
    if (!dst || count == 0u) return;
    for (size_t i = 0u; i < count; ++i) {
        dst[i] = argb;
    }
}

static void arcr_scanline_fill_u32_sse2(uint32_t *dst, size_t count, uint32_t argb) {
    if (!dst || count == 0u) return;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
    __m128i v_fill = _mm_set1_epi32((int)argb);
    size_t  i      = 0u;
    for (; i + 8u <= count; i += 8u) {
        _mm_storeu_si128((__m128i *)(void *)(dst + i), v_fill);
        _mm_storeu_si128((__m128i *)(void *)(dst + i + 4u), v_fill);
    }
    for (; i + 4u <= count; i += 4u) {
        _mm_storeu_si128((__m128i *)(void *)(dst + i), v_fill);
    }
    for (; i < count; ++i) {
        dst[i] = argb;
    }
#else
    arcr_scanline_fill_u32_scalar(dst, count, argb);
#endif
}

void arcr_scanline_blend_scalar(uint32_t *dst, size_t count, uint32_t src_argb) {
    if (!dst || count == 0u) return;
    uint32_t sa = (src_argb >> 24) & 0xFFu;
    if (sa == 0u) return;
    if (sa == 255u) {
        arcr_scanline_fill_u32_scalar(dst, count, src_argb);
        return;
    }

    uint32_t sr    = (src_argb >> 16) & 0xFFu;
    uint32_t sg    = (src_argb >> 8) & 0xFFu;
    uint32_t sb    = src_argb & 0xFFu;
    uint32_t inv_a = 255u - sa;

    uint32_t s_ra = sr * sa;
    uint32_t s_ga = sg * sa;
    uint32_t s_ba = sb * sa;

    for (size_t i = 0u; i < count; ++i) {
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

/*
 * Legacy x86 SSE1 + 64-bit SWAR Kernel (No SSE2 integer ops required!)
 * Processes R+B and A+G channels in parallel inside 64-bit integer lanes with
 * SSE1 prefetch, guaranteeing zero SSE2 illegal-instruction faults on Pentium III.
 */
void arcr_scanline_blend_sse1_swar(uint32_t *dst, size_t count, uint32_t src_argb) {
    if (!dst || count == 0u) return;
    uint32_t sa = (src_argb >> 24) & 0xFFu;
    if (sa == 0u) return;
    if (sa == 255u) {
        arcr_scanline_fill_u32_scalar(dst, count, src_argb);
        return;
    }

    uint32_t sr    = (src_argb >> 16) & 0xFFu;
    uint32_t sg    = (src_argb >> 8) & 0xFFu;
    uint32_t sb    = src_argb & 0xFFu;
    uint32_t inv_a = 255u - sa;

    /* Pre-pack source premultiplied terms + rounding bias 128 in [R|B] and [A|G] lanes */
    uint64_t src_rb = (((uint64_t)(sr * sa + 128u)) << 32) | (uint64_t)(sb * sa + 128u);
    uint64_t src_ag = (((uint64_t)(255u * sa + 128u)) << 32) | (uint64_t)(sg * sa + 128u);

    for (size_t i = 0u; i < count; ++i) {
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

/*
 * x64 Baseline SSE2 / SSE3 128-bit XMM Kernel (4 ARGB8888 pixels per iter)
 */
void arcr_scanline_blend_sse2(uint32_t *dst, size_t count, uint32_t src_argb) {
    if (!dst || count == 0u) return;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
    uint32_t sa = (src_argb >> 24) & 0xFFu;
    if (sa == 0u) return;
    if (sa == 255u) {
        arcr_scanline_fill_u32_sse2(dst, count, src_argb);
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
 * 2. Dynamic GPU Hardware Probing (Zero Static GPU Library Dependencies!)
 * ------------------------------------------------------------------------- */
typedef int32_t (ARCR_ABI_STDCALL *PfnVkEnumInstanceExtProps)(
    const char *layer_name, uint32_t *prop_count, void *props);

typedef const unsigned char *(ARCR_ABI_STDCALL *PfnGlGetString)(uint32_t name);

#if defined(_WIN32)
typedef struct ArcrGuid {
    uint32_t d1;
    uint16_t d2;
    uint16_t d3;
    uint8_t  d4[8];
} ArcrGuid;

typedef struct ArcrDxgiAdapterDesc {
    uint16_t description[128];
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t subsys_id;
    uint32_t revision;
    size_t   dedicated_video_mem;
    size_t   dedicated_sys_mem;
    size_t   shared_sys_mem;
    uint32_t luid_low;
    int32_t  luid_high;
} ArcrDxgiAdapterDesc;

typedef struct ArcrComUnknown {
    void **vtbl;
} ArcrComUnknown;

typedef uint32_t (ARCR_ABI_STDCALL *PfnComRelease)(void *self);
typedef int32_t  (ARCR_ABI_STDCALL *PfnDxgiEnumAdapters)(void *self, uint32_t idx, void **out_adapter);
typedef int32_t  (ARCR_ABI_STDCALL *PfnDxgiGetDesc)(void *self, ArcrDxgiAdapterDesc *out_desc);
typedef int32_t  (ARCR_ABI_STDCALL *PfnCreateDXGIFactory1)(const ArcrGuid *riid, void **out_factory);

static void utf16le_desc_to_utf8(const uint16_t *wstr, size_t max_units, char *out, size_t out_cap) {
    if (!out || out_cap == 0u) return;
    size_t pos = 0u;
    for (size_t i = 0u; i < max_units && wstr[i] != 0u; ++i) {
        uint32_t cp = wstr[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1u < max_units) {
            uint32_t lo = wstr[i + 1u];
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                ++i;
            }
        }
        char u8[4];
        size_t n = arcr_utf8_encode_one(cp, u8);
        if (pos + n >= out_cap) break;
        memcpy(out + pos, u8, n);
        pos += n;
    }
    out[pos] = '\0';
}
#endif

static bool probe_vulkan_loader(const char *const *candidates) {
    for (size_t i = 0u; candidates[i] != NULL; ++i) {
        ArcrDynLib *vk_lib = arcr_platform_dlopen(candidates[i]);
        if (!vk_lib) continue;

        void *sym_create = arcr_platform_dlsym(vk_lib, "vkCreateInstance");
        void *sym_enum   = arcr_platform_dlsym(vk_lib, "vkEnumerateInstanceExtensionProperties");
        bool  ok         = false;
        if (sym_create && sym_enum) {
            PfnVkEnumInstanceExtProps fn_enum = NULL;
            memcpy(&fn_enum, &sym_enum, sizeof(fn_enum));
            uint32_t ext_count = 0u;
            if (fn_enum && fn_enum(NULL, &ext_count, NULL) == 0 && ext_count > 0u) {
                ok = true;
            }
        }
        arcr_platform_dlclose(vk_lib);
        if (ok) return true;
    }
    return false;
}

void arcr_gpu_probe_system(ArcrGpuProbeResult *out_probe) {
    if (!out_probe) return;
    memset(out_probe, 0, sizeof(*out_probe));
    out_probe->best_gpu_backend = ARCR_BACKEND_CPU_SCALAR;

#if defined(_WIN32)
    /* 1. Modern Windows: Direct3D 11 + DXGI Adapter Enumeration */
    {
        ArcrDynLib *d3d11_lib = arcr_platform_dlopen("d3d11.dll");
        ArcrDynLib *dxgi_lib  = arcr_platform_dlopen("dxgi.dll");
        if (d3d11_lib && arcr_platform_dlsym(d3d11_lib, "D3D11CreateDevice")) {
            bool found_hw_adapter = false;
            bool tried_factory    = false;
            if (dxgi_lib) {
                void *sym_fac = arcr_platform_dlsym(dxgi_lib, "CreateDXGIFactory1");
                if (!sym_fac) sym_fac = arcr_platform_dlsym(dxgi_lib, "CreateDXGIFactory");
                if (sym_fac) {
                    /* IID_IDXGIFactory / IID_IDXGIFactory1 */
                    static const ArcrGuid kIidDxgiFactory1 = {
                        0x770AAE78u, 0xF26Fu, 0x4DBAu,
                        {0xA8u, 0x29u, 0x25u, 0x3Cu, 0x83u, 0xD1u, 0xB3u, 0x87u}
                    };
                    static const ArcrGuid kIidDxgiFactory = {
                        0x7B7166ECu, 0x21C7u, 0x44AEu,
                        {0xB2u, 0x1Au, 0xC9u, 0xAEu, 0x32u, 0x1Au, 0xE3u, 0x69u}
                    };
                    PfnCreateDXGIFactory1 fn_create = NULL;
                    memcpy(&fn_create, &sym_fac, sizeof(fn_create));
                    void *factory = NULL;
                    if (fn_create(&kIidDxgiFactory1, &factory) < 0 || !factory) {
                        factory = NULL;
                        (void)fn_create(&kIidDxgiFactory, &factory);
                    }
                    tried_factory = (factory != NULL);
                    if (factory) {
                        ArcrComUnknown *fac_unk = (ArcrComUnknown *)factory;
                        PfnDxgiEnumAdapters fn_enum = NULL;
                        PfnComRelease       fn_rel  = NULL;
                        memcpy(&fn_rel, &fac_unk->vtbl[2], sizeof(fn_rel));
                        memcpy(&fn_enum, &fac_unk->vtbl[7], sizeof(fn_enum));

                        for (uint32_t idx = 0u; idx < 8u && fn_enum; ++idx) {
                            void *adapter = NULL;
                            if (fn_enum(factory, idx, &adapter) < 0 || !adapter) break;

                            ArcrComUnknown     *ad_unk = (ArcrComUnknown *)adapter;
                            PfnDxgiGetDesc      fn_desc = NULL;
                            PfnComRelease       ad_rel  = NULL;
                            ArcrDxgiAdapterDesc desc;
                            memset(&desc, 0, sizeof(desc));
                            memcpy(&ad_rel, &ad_unk->vtbl[2], sizeof(ad_rel));
                            memcpy(&fn_desc, &ad_unk->vtbl[8], sizeof(fn_desc));

                            if (fn_desc && fn_desc(adapter, &desc) >= 0) {
                                char name_u8[128];
                                utf16le_desc_to_utf8(desc.description, 128u, name_u8, sizeof(name_u8));
                                /* Skip Microsoft Basic Render Driver (VendorId 0x1414) */
                                if (desc.vendor_id != 0x1414u && !is_software_renderer_string(name_u8)) {
                                    found_hw_adapter = true;
                                    if (out_probe->adapter_description[0] == '\0') {
                                        arcr_str_copy(out_probe->adapter_description,
                                                      sizeof(out_probe->adapter_description), name_u8);
                                    }
                                    if (ad_rel) ad_rel(adapter);
                                    break;
                                }
                            }
                            if (ad_rel) ad_rel(adapter);
                        }
                        if (fn_rel) fn_rel(factory);
                    }
                }
            }
            out_probe->has_d3d11 = found_hw_adapter || (!tried_factory);
        }
        if (dxgi_lib) arcr_platform_dlclose(dxgi_lib);
        if (d3d11_lib) arcr_platform_dlclose(d3d11_lib);
    }

    /* 2. Modern Cross-Platform: Vulkan 1.x */
    {
        static const char *const kWinVkNames[] = {"vulkan-1.dll", NULL};
        out_probe->has_vulkan = probe_vulkan_loader(kWinVkNames);
    }

    /* 3. Legacy/Classic Windows: Direct3D 9.0c / 9Ex (Win2000 DX9c / XP / Vista+) */
    {
        ArcrDynLib *d3d9_lib = arcr_platform_dlopen("d3d9.dll");
        if (d3d9_lib) {
            if (arcr_platform_dlsym(d3d9_lib, "Direct3DCreate9Ex") ||
                arcr_platform_dlsym(d3d9_lib, "Direct3DCreate9")) {
                out_probe->has_d3d9 = true;
            }
            arcr_platform_dlclose(d3d9_lib);
        }
    }

    /* 4. Legacy Windows 2000 2D Hardware Blitter: DirectDraw 7 */
    {
        ArcrDynLib *ddraw_lib = arcr_platform_dlopen("ddraw.dll");
        if (ddraw_lib) {
            if (arcr_platform_dlsym(ddraw_lib, "DirectDrawCreateEx") ||
                arcr_platform_dlsym(ddraw_lib, "DirectDrawCreate")) {
                out_probe->has_ddraw7 = true;
            }
            arcr_platform_dlclose(ddraw_lib);
        }
    }

    /* 5. OpenGL ICD (Rejects Microsoft OpenGL 1.1 "GDI Generic") */
    {
        typedef struct ArcrPixelFormatDesc {
            uint16_t nSize;
            uint16_t nVersion;
            uint32_t dwFlags;
            uint8_t  iPixelType;
            uint8_t  cColorBits;
            uint8_t  cRedBits;
            uint8_t  cRedShift;
            uint8_t  cGreenBits;
            uint8_t  cGreenShift;
            uint8_t  cBlueBits;
            uint8_t  cBlueShift;
            uint8_t  cAlphaBits;
            uint8_t  cAlphaShift;
            uint8_t  cAccumBits;
            uint8_t  cAccumRedBits;
            uint8_t  cAccumGreenBits;
            uint8_t  cAccumBlueBits;
            uint8_t  cAccumAlphaBits;
            uint8_t  cDepthBits;
            uint8_t  cStencilBits;
            uint8_t  cAuxBuffers;
            uint8_t  iLayerType;
            uint8_t  bReserved;
            uint32_t dwLayerMask;
            uint32_t dwVisibleMask;
            uint32_t dwDamageMask;
        } ArcrPixelFormatDesc;

        typedef void   *(ARCR_ABI_STDCALL *PfnWglGetCurrentContext)(void);
        typedef void   *(ARCR_ABI_STDCALL *PfnWglCreateContext)(void *hdc);
        typedef int32_t (ARCR_ABI_STDCALL *PfnWglMakeCurrent)(void *hdc, void *hglrc);
        typedef int32_t (ARCR_ABI_STDCALL *PfnWglDeleteContext)(void *hglrc);
        typedef void   *(ARCR_ABI_STDCALL *PfnCreateWindowExA)(
            uint32_t ex_style, const char *cls_name, const char *win_name, uint32_t style,
            int x, int y, int w, int h, void *parent, void *menu, void *inst, void *param);
        typedef void   *(ARCR_ABI_STDCALL *PfnGetDC)(void *hwnd);
        typedef int     (ARCR_ABI_STDCALL *PfnReleaseDC)(void *hwnd, void *hdc);
        typedef int32_t (ARCR_ABI_STDCALL *PfnDestroyWindow)(void *hwnd);
        typedef int     (ARCR_ABI_STDCALL *PfnChoosePixelFormat)(void *hdc, const ArcrPixelFormatDesc *pfd);
        typedef int32_t (ARCR_ABI_STDCALL *PfnSetPixelFormat)(void *hdc, int fmt, const ArcrPixelFormatDesc *pfd);

        ArcrDynLib *gl_lib = arcr_platform_dlopen("opengl32.dll");
        if (gl_lib) {
            void *sym_str     = arcr_platform_dlsym(gl_lib, "glGetString");
            void *sym_get_cur = arcr_platform_dlsym(gl_lib, "wglGetCurrentContext");
            void *sym_create  = arcr_platform_dlsym(gl_lib, "wglCreateContext");
            void *sym_make    = arcr_platform_dlsym(gl_lib, "wglMakeCurrent");
            void *sym_del     = arcr_platform_dlsym(gl_lib, "wglDeleteContext");
            if (sym_str) {
                PfnGlGetString          fn_gl_str      = NULL;
                PfnWglGetCurrentContext fn_wgl_get_cur = NULL;
                PfnWglCreateContext     fn_wgl_create  = NULL;
                PfnWglMakeCurrent       fn_wgl_make    = NULL;
                PfnWglDeleteContext     fn_wgl_del     = NULL;
                memcpy(&fn_gl_str, &sym_str, sizeof(fn_gl_str));
                if (sym_get_cur) memcpy(&fn_wgl_get_cur, &sym_get_cur, sizeof(fn_wgl_get_cur));
                if (sym_create)  memcpy(&fn_wgl_create, &sym_create, sizeof(fn_wgl_create));
                if (sym_make)    memcpy(&fn_wgl_make, &sym_make, sizeof(fn_wgl_make));
                if (sym_del)     memcpy(&fn_wgl_del, &sym_del, sizeof(fn_wgl_del));

                ArcrDynLib        *user32_lib     = NULL;
                ArcrDynLib        *gdi32_lib      = NULL;
                PfnReleaseDC       fn_rel_dc      = NULL;
                PfnDestroyWindow   fn_destroy_win = NULL;
                void              *tmp_hwnd       = NULL;
                void              *tmp_hdc        = NULL;
                void              *tmp_glrc       = NULL;
                bool               made_temp_ctx  = false;

                if (fn_wgl_get_cur && fn_wgl_get_cur() == NULL &&
                    fn_wgl_create && fn_wgl_make && fn_wgl_del) {
                    user32_lib = arcr_platform_dlopen("user32.dll");
                    gdi32_lib  = arcr_platform_dlopen("gdi32.dll");
                    if (user32_lib && gdi32_lib) {
                        void *sym_cw  = arcr_platform_dlsym(user32_lib, "CreateWindowExA");
                        void *sym_gdc = arcr_platform_dlsym(user32_lib, "GetDC");
                        void *sym_rdc = arcr_platform_dlsym(user32_lib, "ReleaseDC");
                        void *sym_dw  = arcr_platform_dlsym(user32_lib, "DestroyWindow");
                        void *sym_cpf = arcr_platform_dlsym(gdi32_lib, "ChoosePixelFormat");
                        void *sym_spf = arcr_platform_dlsym(gdi32_lib, "SetPixelFormat");

                        PfnCreateWindowExA   fn_create_win = NULL;
                        PfnGetDC             fn_get_dc     = NULL;
                        PfnChoosePixelFormat fn_choose_pf  = NULL;
                        PfnSetPixelFormat    fn_set_pf     = NULL;
                        if (sym_cw)  memcpy(&fn_create_win, &sym_cw, sizeof(fn_create_win));
                        if (sym_gdc) memcpy(&fn_get_dc, &sym_gdc, sizeof(fn_get_dc));
                        if (sym_rdc) memcpy(&fn_rel_dc, &sym_rdc, sizeof(fn_rel_dc));
                        if (sym_dw)  memcpy(&fn_destroy_win, &sym_dw, sizeof(fn_destroy_win));
                        if (sym_cpf) memcpy(&fn_choose_pf, &sym_cpf, sizeof(fn_choose_pf));
                        if (sym_spf) memcpy(&fn_set_pf, &sym_spf, sizeof(fn_set_pf));

                        if (fn_create_win && fn_get_dc && fn_rel_dc && fn_destroy_win &&
                            fn_choose_pf && fn_set_pf) {
                            /* Hidden 1x1 WS_POPUP (0x80000000u) STATIC dummy window */
                            tmp_hwnd = fn_create_win(0u, "STATIC", "", 0x80000000u,
                                                     0, 0, 1, 1, NULL, NULL, NULL, NULL);
                            if (tmp_hwnd) {
                                tmp_hdc = fn_get_dc(tmp_hwnd);
                                if (tmp_hdc) {
                                    ArcrPixelFormatDesc pfd;
                                    memset(&pfd, 0, sizeof(pfd));
                                    pfd.nSize      = (uint16_t)sizeof(pfd);
                                    pfd.nVersion   = 1u;
                                    /* PFD_DRAW_TO_WINDOW (0x04) | PFD_SUPPORT_OPENGL (0x20) | PFD_DOUBLEBUFFER (0x01) */
                                    pfd.dwFlags    = 0x00000025u;
                                    pfd.iPixelType = 0u; /* PFD_TYPE_RGBA */
                                    pfd.cColorBits = 32u;
                                    pfd.cDepthBits = 16u;
                                    int pf = fn_choose_pf(tmp_hdc, &pfd);
                                    if (pf <= 0) {
                                        pfd.dwFlags = 0x00000024u;
                                        pf = fn_choose_pf(tmp_hdc, &pfd);
                                    }
                                    if (pf > 0 && fn_set_pf(tmp_hdc, pf, &pfd)) {
                                        tmp_glrc = fn_wgl_create(tmp_hdc);
                                        if (tmp_glrc && fn_wgl_make(tmp_hdc, tmp_glrc)) {
                                            made_temp_ctx = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                const char *ren = (const char *)fn_gl_str(0x1F01u /* GL_RENDERER */);
                if (ren && ren[0] != '\0' && !is_software_renderer_string(ren)) {
                    out_probe->has_opengl_icd = true;
                    if (out_probe->adapter_description[0] == '\0') {
                        arcr_str_copy(out_probe->adapter_description,
                                      sizeof(out_probe->adapter_description), ren);
                    }
                }

                if (made_temp_ctx) fn_wgl_make(NULL, NULL);
                if (tmp_glrc && fn_wgl_del) fn_wgl_del(tmp_glrc);
                if (tmp_hdc && tmp_hwnd && fn_rel_dc) fn_rel_dc(tmp_hwnd, tmp_hdc);
                if (tmp_hwnd && fn_destroy_win) fn_destroy_win(tmp_hwnd);
                if (gdi32_lib) arcr_platform_dlclose(gdi32_lib);
                if (user32_lib) arcr_platform_dlclose(user32_lib);
            }
            arcr_platform_dlclose(gl_lib);
        }
    }
#else
    /* POSIX / Linux / macOS Dynamic Probing */
    {
        static const char *const kPosixVkNames[] = {
            "libvulkan.so.1",
            "libvulkan.so",
            "libMoltenVK.dylib",
            NULL
        };
        out_probe->has_vulkan = probe_vulkan_loader(kPosixVkNames);
    }

    {
        static const char *const kPosixGlNames[] = {
            "libGL.so.1",
            "libOpenGL.so.0",
            "libEGL.so.1",
            NULL
        };
        const char *force_sw = getenv("LIBGL_ALWAYS_SOFTWARE");
        bool sw_forced = (force_sw && (strcmp(force_sw, "1") == 0 || arcr_str_eq_nocase(force_sw, "true")));
        bool has_drm_node = out_probe->has_vulkan;
        if (!has_drm_node) {
            FILE *f = fopen("/dev/dri/renderD128", "rb");
            if (!f) f = fopen("/dev/dri/card0", "rb");
            if (f) {
                has_drm_node = true;
                fclose(f);
            }
        }
#if defined(__APPLE__)
        has_drm_node = true;
#endif
        if (!sw_forced) {
            for (size_t i = 0u; kPosixGlNames[i] != NULL; ++i) {
                ArcrDynLib *gl_lib = arcr_platform_dlopen(kPosixGlNames[i]);
                if (!gl_lib) continue;

                void *sym_str = arcr_platform_dlsym(gl_lib, "glGetString");
                bool  gl_ok   = false;
                if (sym_str) {
                    PfnGlGetString fn_gl_str = NULL;
                    memcpy(&fn_gl_str, &sym_str, sizeof(fn_gl_str));
                    const char *ren = (const char *)fn_gl_str(0x1F01u /* GL_RENDERER */);
                    if (ren && ren[0] != '\0') {
                        if (!is_software_renderer_string(ren)) {
                            gl_ok = true;
                            if (out_probe->adapter_description[0] == '\0') {
                                arcr_str_copy(out_probe->adapter_description,
                                              sizeof(out_probe->adapter_description), ren);
                            }
                        }
                    } else if (has_drm_node &&
                               (arcr_platform_dlsym(gl_lib, "glXCreateContext") ||
                                arcr_platform_dlsym(gl_lib, "eglGetDisplay"))) {
                        gl_ok = true;
                    }
                }
                arcr_platform_dlclose(gl_lib);
                if (gl_ok) {
                    out_probe->has_opengl_icd = true;
                    break;
                }
            }
        }
    }
#endif

    if (out_probe->has_d3d11) {
        out_probe->best_gpu_backend = ARCR_BACKEND_GPU_D3D11;
    } else if (out_probe->has_vulkan) {
        out_probe->best_gpu_backend = ARCR_BACKEND_GPU_VULKAN;
    } else if (out_probe->has_d3d9) {
        out_probe->best_gpu_backend = ARCR_BACKEND_GPU_D3D9;
    } else if (out_probe->has_opengl_icd) {
        out_probe->best_gpu_backend = ARCR_BACKEND_GPU_OPENGL;
    } else if (out_probe->has_ddraw7) {
        out_probe->best_gpu_backend = ARCR_BACKEND_GPU_DDRAW7;
    }

    if (out_probe->adapter_description[0] == '\0') {
        if (out_probe->best_gpu_backend != ARCR_BACKEND_CPU_SCALAR) {
            arcr_str_copy(out_probe->adapter_description,
                          sizeof(out_probe->adapter_description),
                          arcr_accel_backend_name(out_probe->best_gpu_backend));
        } else {
            arcr_str_copy(out_probe->adapter_description,
                          sizeof(out_probe->adapter_description),
                          "CPU SIMD Rasterizer (No Hardware ICD)");
        }
    }
}

/* -------------------------------------------------------------------------
 * 3. Context Creation, Cost Estimation & Automatic Fault Fallback
 * ------------------------------------------------------------------------- */
ArcrAccelContext *arcr_accel_create(uint32_t allowed_cpu_flags, bool force_cpu,
                                    bool probe_or_simulate_gpu) {
    ArcrAccelContext *ctx = (ArcrAccelContext *)calloc(1u, sizeof(ArcrAccelContext));
    if (!ctx) return NULL;

    uint32_t hw = arcr_detect_cpu_flags() & allowed_cpu_flags;
    ctx->cpu_flags = hw;
    ctx->force_cpu = force_cpu;

    if ((hw & ARCR_CPU_AVX2) != 0u && arcr_compiled_with_avx2_kernel()) {
        ctx->cpu_backend       = ARCR_BACKEND_CPU_AVX2;
        ctx->fill_scanline_fn  = arcr_scanline_fill_u32_avx2;
        ctx->blend_scanline_fn = arcr_scanline_blend_avx2;
    } else if ((hw & ARCR_CPU_SSE2) != 0u) {
        ctx->cpu_backend       = ARCR_BACKEND_CPU_SSE2_SSE3;
        ctx->fill_scanline_fn  = arcr_scanline_fill_u32_sse2;
        ctx->blend_scanline_fn = arcr_scanline_blend_sse2;
    } else if ((hw & ARCR_CPU_SSE1) != 0u) {
        ctx->cpu_backend       = ARCR_BACKEND_CPU_SSE1_SWAR;
        ctx->fill_scanline_fn  = arcr_scanline_fill_u32_scalar;
        ctx->blend_scanline_fn = arcr_scanline_blend_sse1_swar;
    } else {
        ctx->cpu_backend       = ARCR_BACKEND_CPU_SCALAR;
        ctx->fill_scanline_fn  = arcr_scanline_fill_u32_scalar;
        ctx->blend_scanline_fn = arcr_scanline_blend_scalar;
    }

    ctx->gpu_disabled_by_fault = false;
    ctx->gpu_error_strikes     = 0u;
    ctx->gpu_present           = false;
    ctx->gpu_backend           = ARCR_BACKEND_CPU_SCALAR;

    arcr_gpu_probe_system(&ctx->gpu_probe);

    if (!force_cpu && probe_or_simulate_gpu) {
        if (ctx->gpu_probe.best_gpu_backend >= ARCR_BACKEND_GPU_DDRAW7) {
            ctx->gpu_backend = ctx->gpu_probe.best_gpu_backend;
            ctx->gpu_present = true;
        } else {
            /* Clean headless simulation path when probe_or_simulate_gpu is requested on CI VMs */
            ctx->gpu_backend = (arcr_host_compile_arch() == ARCR_ARCH_X86_32)
                                   ? ARCR_BACKEND_GPU_D3D9
                                   : ARCR_BACKEND_GPU_D3D11;
            ctx->gpu_present = true;
            ctx->gpu_probe.best_gpu_backend = ctx->gpu_backend;
            arcr_str_copy(ctx->gpu_probe.adapter_description,
                          sizeof(ctx->gpu_probe.adapter_description),
                          "Headless Simulated Hardware Compositor");
        }
    }

    return ctx;
}

void arcr_accel_destroy(ArcrAccelContext *ctx) {
    free(ctx);
}

ArcrAccelBackendId arcr_accel_active_cpu_backend(const ArcrAccelContext *ctx) {
    return ctx ? ctx->cpu_backend : ARCR_BACKEND_CPU_SCALAR;
}

ArcrAccelBackendId arcr_accel_active_gpu_backend(const ArcrAccelContext *ctx) {
    if (!ctx || !arcr_accel_is_gpu_healthy(ctx)) {
        return ARCR_BACKEND_CPU_SCALAR;
    }
    return ctx->gpu_backend;
}

void arcr_accel_get_gpu_probe(const ArcrAccelContext *ctx, ArcrGpuProbeResult *out_probe) {
    if (!out_probe) return;
    if (!ctx) {
        memset(out_probe, 0, sizeof(*out_probe));
        return;
    }
    *out_probe = ctx->gpu_probe;
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
        case ARCR_BACKEND_CPU_AVX2:      return "CPU-AVX2 (256-bit)";
        case ARCR_BACKEND_GPU_DDRAW7:    return "GPU-DirectDraw7 (Legacy)";
        case ARCR_BACKEND_GPU_D3D9:      return "GPU-Direct3D9";
        case ARCR_BACKEND_GPU_OPENGL:    return "GPU-OpenGL-ICD";
        case ARCR_BACKEND_GPU_D3D11:     return "GPU-Direct3D11-Flip";
        case ARCR_BACKEND_GPU_VULKAN:    return "GPU-Vulkan-1.x";
        default:                         return "Unknown";
    }
}

void arcr_accel_reject_software_gl_renderer(ArcrAccelContext *ctx, const char *gl_renderer_str) {
    if (!ctx || !gl_renderer_str) return;
    if (is_software_renderer_string(gl_renderer_str)) {
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

    /* CPU cost scales linearly with pixels, accelerated by active SIMD width */
    uint32_t simd_div = (ctx->cpu_backend == ARCR_BACKEND_CPU_AVX2)      ? 8u
                      : (ctx->cpu_backend == ARCR_BACKEND_CPU_SSE2_SSE3) ? 4u
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

    /* GPU cost pays fixed setup/sync + PCI/AGP/PCIe upload & readback unless resident */
    ArcrAccelCostBreakdown gpu_c = {
        .setup_cost    = 180u,
        .upload_cost   = already_on_gpu ? 0u : (px / 32u),
        .exec_cost     = ARCR_MAX(1u, px / 2048u),
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
 * 4. Clipped Framebuffer 2D Hardware-Router Primitives
 * ------------------------------------------------------------------------- */
static bool clip_rect(int32_t fb_w, int32_t fb_h, ArcrRect in,
                      int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1) {
    if (in.width <= 0 || in.height <= 0 || fb_w <= 0 || fb_h <= 0) return false;
    int64_t rx1 = (int64_t)in.x + (int64_t)in.width;
    int64_t ry1 = (int64_t)in.y + (int64_t)in.height;
    if (rx1 <= 0 || ry1 <= 0 || in.x >= fb_w || in.y >= fb_h) return false;
    *x0 = (in.x > 0) ? in.x : 0;
    *y0 = (in.y > 0) ? in.y : 0;
    *x1 = (rx1 < (int64_t)fb_w) ? (int32_t)rx1 : fb_w;
    *y1 = (ry1 < (int64_t)fb_h) ? (int32_t)ry1 : fb_h;
    return (*x0 < *x1) && (*y0 < *y1);
}

static inline void blend_pixel_argb(uint32_t *dst_px, uint32_t sr, uint32_t sg, uint32_t sb, uint32_t sa) {
    if (sa == 0u) return;
    if (sa >= 255u) {
        *dst_px = 0xFF000000u | (sr << 16) | (sg << 8) | sb;
        return;
    }
    uint32_t d     = *dst_px;
    uint32_t inv_a = 255u - sa;
    uint32_t out_r = div255_exact(sr * sa + ((d >> 16) & 0xFFu) * inv_a);
    uint32_t out_g = div255_exact(sg * sa + ((d >> 8) & 0xFFu) * inv_a);
    uint32_t out_b = div255_exact(sb * sa + (d & 0xFFu) * inv_a);
    uint32_t out_a = sa + div255_exact(((d >> 24) & 0xFFu) * inv_a);
    *dst_px        = (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

static inline ArcrScanlineFillFn get_fill_fn(const ArcrAccelContext *ctx) {
    if (ctx && ctx->fill_scanline_fn) return ctx->fill_scanline_fn;
    return arcr_scanline_fill_u32_scalar;
}

static inline ArcrScanlineBlendFn get_blend_fn(const ArcrAccelContext *ctx) {
    if (ctx && ctx->blend_scanline_fn) return ctx->blend_scanline_fn;
    return arcr_scanline_blend_scalar;
}

void arcr_accel_fill_rect(ArcrAccelContext *ctx, uint32_t *dst_argb,
                          int32_t fb_w, int32_t fb_h, int32_t stride_px,
                          ArcrRect rect, uint32_t argb_color) {
    if (!dst_argb) return;
    int32_t x0, y0, x1, y1;
    if (!clip_rect(fb_w, fb_h, rect, &x0, &y0, &x1, &y1)) return;
    ArcrScanlineFillFn fn = get_fill_fn(ctx);
    size_t span = (size_t)(x1 - x0);
    for (int32_t y = y0; y < y1; ++y) {
        uint32_t *row = dst_argb + (size_t)y * (size_t)stride_px + (size_t)x0;
        fn(row, span, argb_color);
    }
}

void arcr_accel_blend_rect(ArcrAccelContext *ctx, uint32_t *dst_argb,
                           int32_t fb_w, int32_t fb_h, int32_t stride_px,
                           ArcrRect rect, uint32_t argb_color) {
    if (!dst_argb) return;
    int32_t x0, y0, x1, y1;
    if (!clip_rect(fb_w, fb_h, rect, &x0, &y0, &x1, &y1)) return;
    ArcrScanlineBlendFn fn = get_blend_fn(ctx);
    size_t span = (size_t)(x1 - x0);
    for (int32_t y = y0; y < y1; ++y) {
        uint32_t *row = dst_argb + (size_t)y * (size_t)stride_px + (size_t)x0;
        fn(row, span, argb_color);
    }
}

/*
 * Point-in-rounded-rect test in 8x subpixel coordinates (subpixel centers at 8*px + {1,3,5,7}).
 */
static inline bool subpixel_in_rrect_8x(int64_t sx8, int64_t sy8, ArcrRect rc, int32_t r) {
    int64_t rx0 = (int64_t)rc.x * 8;
    int64_t ry0 = (int64_t)rc.y * 8;
    int64_t rx1 = ((int64_t)rc.x + (int64_t)rc.width) * 8;
    int64_t ry1 = ((int64_t)rc.y + (int64_t)rc.height) * 8;
    if (sx8 < rx0 || sx8 >= rx1 || sy8 < ry0 || sy8 >= ry1) return false;
    if (r <= 0) return true;

    int64_t r8  = (int64_t)r * 8;
    int64_t ix0 = rx0 + r8;
    int64_t ix1 = rx1 - r8;
    int64_t iy0 = ry0 + r8;
    int64_t iy1 = ry1 - r8;

    int64_t dx = (sx8 < ix0) ? (ix0 - sx8) : ((sx8 > ix1) ? (sx8 - ix1) : 0);
    int64_t dy = (sy8 < iy0) ? (iy0 - sy8) : ((sy8 > iy1) ? (sy8 - iy1) : 0);
    if (dx == 0 || dy == 0) return true;
    return (dx * dx + dy * dy) <= (r8 * r8);
}

void arcr_accel_fill_rounded_rect(ArcrAccelContext *ctx, uint32_t *dst_argb,
                                  int32_t fb_w, int32_t fb_h, int32_t stride_px,
                                  ArcrRect rect, int32_t radius_px, uint32_t argb_color) {
    if (!dst_argb || rect.width <= 0 || rect.height <= 0) return;
    uint32_t base_a = (argb_color >> 24) & 0xFFu;
    if (base_a == 0u) return;

    int32_t max_r = ARCR_MIN(rect.width / 2, rect.height / 2);
    int32_t r     = ARCR_CLAMP(radius_px, 0, max_r);
    if (r <= 0) {
        if (base_a == 255u) {
            arcr_accel_fill_rect(ctx, dst_argb, fb_w, fb_h, stride_px, rect, argb_color);
        } else {
            arcr_accel_blend_rect(ctx, dst_argb, fb_w, fb_h, stride_px, rect, argb_color);
        }
        return;
    }

    int32_t x0, y0, x1, y1;
    if (!clip_rect(fb_w, fb_h, rect, &x0, &y0, &x1, &y1)) return;

    uint32_t sr = (argb_color >> 16) & 0xFFu;
    uint32_t sg = (argb_color >> 8) & 0xFFu;
    uint32_t sb = argb_color & 0xFFu;

    ArcrScanlineFillFn  fill_fn  = get_fill_fn(ctx);
    ArcrScanlineBlendFn blend_fn = get_blend_fn(ctx);

    int32_t core_x0 = rect.x + r;
    int32_t core_x1 = rect.x + rect.width - r;
    int32_t core_y0 = rect.y + r;
    int32_t core_y1 = rect.y + rect.height - r;

    for (int32_t py = y0; py < y1; ++py) {
        uint32_t *row = dst_argb + (size_t)py * (size_t)stride_px;

        /* Interior scanlines have zero corner curvature: 100% SIMD span */
        if (py >= core_y0 && py < core_y1) {
            size_t span = (size_t)(x1 - x0);
            if (base_a == 255u) {
                fill_fn(row + x0, span, argb_color);
            } else {
                blend_fn(row + x0, span, argb_color);
            }
            continue;
        }

        /* Top/bottom corner rows: SIMD fill center span + 4x4 subpixel AA on corner arcs */
        int32_t mid_x0 = ARCR_MAX(x0, core_x0);
        int32_t mid_x1 = ARCR_MIN(x1, core_x1);
        if (mid_x0 < mid_x1) {
            size_t span = (size_t)(mid_x1 - mid_x0);
            if (base_a == 255u) {
                fill_fn(row + mid_x0, span, argb_color);
            } else {
                blend_fn(row + mid_x0, span, argb_color);
            }
        }

        int32_t left_end = ARCR_MIN(x1, core_x0);
        for (int32_t px = x0; px < left_end; ++px) {
            uint32_t hits = 0u;
            for (int32_t sy = 1; sy <= 7; sy += 2) {
                int64_t sy8 = (int64_t)py * 8 + sy;
                for (int32_t sx = 1; sx <= 7; sx += 2) {
                    if (subpixel_in_rrect_8x((int64_t)px * 8 + sx, sy8, rect, r)) {
                        ++hits;
                    }
                }
            }
            if (hits > 0u) {
                uint32_t cov = (hits * 255u + 8u) >> 4;
                uint32_t sa  = (base_a == 255u) ? cov : div255_exact(base_a * cov);
                blend_pixel_argb(&row[px], sr, sg, sb, sa);
            }
        }

        int32_t right_start = ARCR_MAX(x0, ARCR_MAX(left_end, core_x1));
        for (int32_t px = right_start; px < x1; ++px) {
            uint32_t hits = 0u;
            for (int32_t sy = 1; sy <= 7; sy += 2) {
                int64_t sy8 = (int64_t)py * 8 + sy;
                for (int32_t sx = 1; sx <= 7; sx += 2) {
                    if (subpixel_in_rrect_8x((int64_t)px * 8 + sx, sy8, rect, r)) {
                        ++hits;
                    }
                }
            }
            if (hits > 0u) {
                uint32_t cov = (hits * 255u + 8u) >> 4;
                uint32_t sa  = (base_a == 255u) ? cov : div255_exact(base_a * cov);
                blend_pixel_argb(&row[px], sr, sg, sb, sa);
            }
        }
    }
}

void arcr_accel_stroke_rounded_rect(ArcrAccelContext *ctx, uint32_t *dst_argb,
                                    int32_t fb_w, int32_t fb_h, int32_t stride_px,
                                    ArcrRect rect, int32_t radius_px, int32_t border_px,
                                    uint32_t border_argb) {
    if (!dst_argb || rect.width <= 0 || rect.height <= 0 || border_px <= 0) return;
    uint32_t base_a = (border_argb >> 24) & 0xFFu;
    if (base_a == 0u) return;

    if (border_px * 2 >= rect.width || border_px * 2 >= rect.height) {
        arcr_accel_fill_rounded_rect(ctx, dst_argb, fb_w, fb_h, stride_px,
                                     rect, radius_px, border_argb);
        return;
    }

    int32_t max_r = ARCR_MIN(rect.width / 2, rect.height / 2);
    int32_t r_out = ARCR_CLAMP(radius_px, 0, max_r);

    /* Fast-path: sharp 4-edge rectangular stroke via SIMD */
    if (r_out == 0) {
        ArcrRect top_r   = {rect.x, rect.y, rect.width, border_px};
        ArcrRect bot_r   = {rect.x, rect.y + rect.height - border_px, rect.width, border_px};
        ArcrRect left_r  = {rect.x, rect.y + border_px, border_px, rect.height - 2 * border_px};
        ArcrRect right_r = {rect.x + rect.width - border_px, rect.y + border_px,
                            border_px, rect.height - 2 * border_px};
        if (base_a == 255u) {
            arcr_accel_fill_rect(ctx, dst_argb, fb_w, fb_h, stride_px, top_r, border_argb);
            arcr_accel_fill_rect(ctx, dst_argb, fb_w, fb_h, stride_px, bot_r, border_argb);
            arcr_accel_fill_rect(ctx, dst_argb, fb_w, fb_h, stride_px, left_r, border_argb);
            arcr_accel_fill_rect(ctx, dst_argb, fb_w, fb_h, stride_px, right_r, border_argb);
        } else {
            arcr_accel_blend_rect(ctx, dst_argb, fb_w, fb_h, stride_px, top_r, border_argb);
            arcr_accel_blend_rect(ctx, dst_argb, fb_w, fb_h, stride_px, bot_r, border_argb);
            arcr_accel_blend_rect(ctx, dst_argb, fb_w, fb_h, stride_px, left_r, border_argb);
            arcr_accel_blend_rect(ctx, dst_argb, fb_w, fb_h, stride_px, right_r, border_argb);
        }
        return;
    }

    int32_t x0, y0, x1, y1;
    if (!clip_rect(fb_w, fb_h, rect, &x0, &y0, &x1, &y1)) return;

    ArcrRect inner_rect = {
        rect.x + border_px,
        rect.y + border_px,
        rect.width - 2 * border_px,
        rect.height - 2 * border_px
    };
    int32_t r_in = ARCR_MAX(0, r_out - border_px);
    int32_t c_sz = ARCR_MAX(r_out, border_px);

    /* Straight horizontal & vertical segments via SIMD */
    if (rect.width > 2 * c_sz) {
        ArcrRect top_s = {rect.x + c_sz, rect.y, rect.width - 2 * c_sz, border_px};
        ArcrRect bot_s = {rect.x + c_sz, rect.y + rect.height - border_px,
                          rect.width - 2 * c_sz, border_px};
        if (base_a == 255u) {
            arcr_accel_fill_rect(ctx, dst_argb, fb_w, fb_h, stride_px, top_s, border_argb);
            arcr_accel_fill_rect(ctx, dst_argb, fb_w, fb_h, stride_px, bot_s, border_argb);
        } else {
            arcr_accel_blend_rect(ctx, dst_argb, fb_w, fb_h, stride_px, top_s, border_argb);
            arcr_accel_blend_rect(ctx, dst_argb, fb_w, fb_h, stride_px, bot_s, border_argb);
        }
    }
    if (rect.height > 2 * c_sz) {
        ArcrRect left_s  = {rect.x, rect.y + c_sz, border_px, rect.height - 2 * c_sz};
        ArcrRect right_s = {rect.x + rect.width - border_px, rect.y + c_sz,
                            border_px, rect.height - 2 * c_sz};
        if (base_a == 255u) {
            arcr_accel_fill_rect(ctx, dst_argb, fb_w, fb_h, stride_px, left_s, border_argb);
            arcr_accel_fill_rect(ctx, dst_argb, fb_w, fb_h, stride_px, right_s, border_argb);
        } else {
            arcr_accel_blend_rect(ctx, dst_argb, fb_w, fb_h, stride_px, left_s, border_argb);
            arcr_accel_blend_rect(ctx, dst_argb, fb_w, fb_h, stride_px, right_s, border_argb);
        }
    }

    /* 4x4 subpixel AA ring coverage on the 4 corner regions (non-overlapping) */
    uint32_t sr = (border_argb >> 16) & 0xFFu;
    uint32_t sg = (border_argb >> 8) & 0xFFu;
    uint32_t sb = border_argb & 0xFFu;

    int32_t top_end    = ARCR_MIN(y1, rect.y + c_sz);
    int32_t bot_start  = ARCR_MAX(y0, ARCR_MAX(top_end, rect.y + rect.height - c_sz));
    int32_t left_end   = ARCR_MIN(x1, rect.x + c_sz);
    int32_t right_start = ARCR_MAX(x0, ARCR_MAX(left_end, rect.x + rect.width - c_sz));

    for (int32_t pass_y = 0; pass_y < 2; ++pass_y) {
        int32_t py_beg = (pass_y == 0) ? y0 : bot_start;
        int32_t py_end = (pass_y == 0) ? top_end : y1;
        for (int32_t py = py_beg; py < py_end; ++py) {
            uint32_t *row = dst_argb + (size_t)py * (size_t)stride_px;
            for (int32_t pass_x = 0; pass_x < 2; ++pass_x) {
                int32_t px_beg = (pass_x == 0) ? x0 : right_start;
                int32_t px_end = (pass_x == 0) ? left_end : x1;
                for (int32_t px = px_beg; px < px_end; ++px) {
                    uint32_t hits = 0u;
                    for (int32_t sy = 1; sy <= 7; sy += 2) {
                        int64_t sy8 = (int64_t)py * 8 + sy;
                        for (int32_t sx = 1; sx <= 7; sx += 2) {
                            int64_t sx8 = (int64_t)px * 8 + sx;
                            if (subpixel_in_rrect_8x(sx8, sy8, rect, r_out) &&
                                !subpixel_in_rrect_8x(sx8, sy8, inner_rect, r_in)) {
                                ++hits;
                            }
                        }
                    }
                    if (hits > 0u) {
                        uint32_t cov = (hits * 255u + 8u) >> 4;
                        uint32_t sa  = (base_a == 255u) ? cov : div255_exact(base_a * cov);
                        blend_pixel_argb(&row[px], sr, sg, sb, sa);
                    }
                }
            }
        }
    }
}

void arcr_accel_draw_box_shadow(ArcrAccelContext *ctx, uint32_t *dst_argb,
                                int32_t fb_w, int32_t fb_h, int32_t stride_px,
                                ArcrRect rect, int32_t radius_px, int32_t blur_px,
                                int32_t offset_y, uint32_t shadow_argb) {
    if (!dst_argb || rect.width <= 0 || rect.height <= 0) return;
    uint32_t base_a = (shadow_argb >> 24) & 0xFFu;
    if (base_a == 0u) return;

    ArcrRect srect = {rect.x, rect.y + offset_y, rect.width, rect.height};
    if (blur_px <= 0) {
        arcr_accel_fill_rounded_rect(ctx, dst_argb, fb_w, fb_h, stride_px,
                                     srect, radius_px, shadow_argb);
        return;
    }

    int32_t blur  = ARCR_MIN(blur_px, 64);
    int32_t max_r = ARCR_MIN(srect.width / 2, srect.height / 2);
    int32_t r     = ARCR_CLAMP(radius_px, 0, max_r);

    ArcrRect bounds = {
        srect.x - blur,
        srect.y - blur,
        srect.width + 2 * blur,
        srect.height + 2 * blur
    };
    int32_t x0, y0, x1, y1;
    if (!clip_rect(fb_w, fb_h, bounds, &x0, &y0, &x1, &y1)) return;

    uint32_t sr = (shadow_argb >> 16) & 0xFFu;
    uint32_t sg = (shadow_argb >> 8) & 0xFFu;
    uint32_t sb = shadow_argb & 0xFFu;

    /* Half-pixel (2x) coordinates for smooth integer signed-distance evaluation */
    int32_t ix0   = 2 * (srect.x + r);
    int32_t ix1   = 2 * (srect.x + srect.width - r);
    int32_t iy0   = 2 * (srect.y + r);
    int32_t iy1   = 2 * (srect.y + srect.height - r);
    int32_t r2    = 2 * r;
    int32_t blur2 = 2 * blur;

    /* Deep interior span where signed distance <= -blur receives full shadow_argb via SIMD */
    int32_t full_x0 = srect.x + r + blur;
    int32_t full_x1 = srect.x + srect.width - r - blur;
    int32_t full_y0 = srect.y + r + blur;
    int32_t full_y1 = srect.y + srect.height - r - blur;
    ArcrScanlineBlendFn blend_fn = get_blend_fn(ctx);

    for (int32_t py = y0; py < y1; ++py) {
        uint32_t *row = dst_argb + (size_t)py * (size_t)stride_px;
        int32_t   cy  = 2 * py + 1;
        int32_t   qy  = (cy < iy0) ? (iy0 - cy) : ((cy > iy1) ? (cy - iy1) : 0);

        bool has_full_span = (py >= full_y0 && py < full_y1 && full_x0 < full_x1);
        int32_t fx0 = has_full_span ? ARCR_MAX(x0, full_x0) : x1;
        int32_t fx1 = has_full_span ? ARCR_MIN(x1, full_x1) : x0;

        int32_t px = x0;
        while (px < x1) {
            if (has_full_span && px == fx0 && fx0 < fx1) {
                blend_fn(row + fx0, (size_t)(fx1 - fx0), shadow_argb);
                px = fx1;
                continue;
            }

            int32_t cx = 2 * px + 1;
            int32_t qx = (cx < ix0) ? (ix0 - cx) : ((cx > ix1) ? (cx - ix1) : 0);
            int32_t sd2;
            if (qx == 0 && qy == 0) {
                /* Inside inner core: negative signed distance to nearest edge */
                int32_t dx_in = ARCR_MIN(cx - ix0, ix1 - cx);
                int32_t dy_in = ARCR_MIN(cy - iy0, iy1 - cy);
                sd2 = -(ARCR_MIN(dx_in, dy_in) + r2);
            } else if (qx == 0) {
                sd2 = qy - r2;
            } else if (qy == 0) {
                sd2 = qx - r2;
            } else {
                uint32_t d2_sq = (uint32_t)(qx * qx + qy * qy);
                sd2 = (int32_t)isqrt_u32(d2_sq) - r2;
            }

            if (sd2 < blur2) {
                uint32_t sa;
                if (sd2 <= -blur2) {
                    sa = base_a;
                } else {
                    /* Smooth cubic Hermite falloff: t in [0..256] */
                    uint32_t u  = (uint32_t)(((blur2 - sd2) * 256) / (2 * blur2));
                    if (u > 256u) u = 256u;
                    uint32_t sm = (u * u * (768u - 2u * u)) >> 16;
                    sa = (base_a * sm + 128u) >> 8;
                }
                if (sa > 0u) {
                    blend_pixel_argb(&row[px], sr, sg, sb, sa);
                }
            }
            ++px;
        }
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
        uint32_t      *row  = dst_argb + (size_t)py * (size_t)stride_px;
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

void arcr_accel_blit_image_argb(ArcrAccelContext *ctx, uint32_t *dst_argb,
                                int32_t fb_w, int32_t fb_h, int32_t stride_px,
                                ArcrRect dst_rect,
                                const uint32_t *src_argb, int32_t src_w, int32_t src_h,
                                bool bilinear_smooth) {
    (void)ctx;
    if (!dst_argb || !src_argb || src_w <= 0 || src_h <= 0) return;
    int32_t x0, y0, x1, y1;
    if (!clip_rect(fb_w, fb_h, dst_rect, &x0, &y0, &x1, &y1)) return;

    /* Fast-path 1: 1:1 unscaled image blit */
    if (dst_rect.width == src_w && dst_rect.height == src_h) {
        size_t span = (size_t)(x1 - x0);
        for (int32_t py = y0; py < y1; ++py) {
            const uint32_t *srow = src_argb + (size_t)(py - dst_rect.y) * (size_t)src_w +
                                   (size_t)(x0 - dst_rect.x);
            uint32_t       *drow = dst_argb + (size_t)py * (size_t)stride_px + (size_t)x0;
            for (size_t i = 0u; i < span; ++i) {
                uint32_t s  = srow[i];
                uint32_t sa = (s >> 24) & 0xFFu;
                if (sa == 255u) {
                    drow[i] = s;
                } else if (sa > 0u) {
                    blend_pixel_argb(&drow[i], (s >> 16) & 0xFFu, (s >> 8) & 0xFFu, s & 0xFFu, sa);
                }
            }
        }
        return;
    }

    /* Fast-path 2: 16.16 fixed-point Nearest-Neighbor scaling */
    if (!bilinear_smooth || (src_w == 1 && src_h == 1)) {
        uint64_t step_x = ((uint64_t)(uint32_t)src_w << 16) / (uint32_t)dst_rect.width;
        uint64_t step_y = ((uint64_t)(uint32_t)src_h << 16) / (uint32_t)dst_rect.height;
        uint64_t u_init = (uint64_t)(uint32_t)(x0 - dst_rect.x) * step_x + (step_x >> 1);
        uint64_t v_cur  = (uint64_t)(uint32_t)(y0 - dst_rect.y) * step_y + (step_y >> 1);

        for (int32_t py = y0; py < y1; ++py, v_cur += step_y) {
            int32_t sy = (int32_t)(v_cur >> 16);
            if (sy >= src_h) sy = src_h - 1;
            const uint32_t *srow = src_argb + (size_t)sy * (size_t)src_w;
            uint32_t       *drow = dst_argb + (size_t)py * (size_t)stride_px;

            uint64_t u_cur = u_init;
            for (int32_t px = x0; px < x1; ++px, u_cur += step_x) {
                int32_t sx = (int32_t)(u_cur >> 16);
                if (sx >= src_w) sx = src_w - 1;
                uint32_t s  = srow[sx];
                uint32_t sa = (s >> 24) & 0xFFu;
                if (sa == 255u) {
                    drow[px] = s;
                } else if (sa > 0u) {
                    blend_pixel_argb(&drow[px], (s >> 16) & 0xFFu, (s >> 8) & 0xFFu, s & 0xFFu, sa);
                }
            }
        }
        return;
    }

    /* Path 3: 16.16 fixed-point Smooth Bilinear Filtering (exact 256-sum weights) */
    int64_t step_x = ((int64_t)src_w << 16) / (int64_t)dst_rect.width;
    int64_t step_y = ((int64_t)src_h << 16) / (int64_t)dst_rect.height;
    int64_t fx_init = ((int64_t)(x0 - dst_rect.x) * 2 + 1) * (step_x >> 1) - 32768;
    int64_t fy_cur  = ((int64_t)(y0 - dst_rect.y) * 2 + 1) * (step_y >> 1) - 32768;
    int64_t max_fx  = (int64_t)(src_w - 1) << 16;
    int64_t max_fy  = (int64_t)(src_h - 1) << 16;

    for (int32_t py = y0; py < y1; ++py, fy_cur += step_y) {
        int64_t fy_clamped = (fy_cur < 0) ? 0 : ((fy_cur > max_fy) ? max_fy : fy_cur);
        int32_t sy0 = (int32_t)(fy_clamped >> 16);
        int32_t sy1 = (sy0 + 1 < src_h) ? (sy0 + 1) : sy0;
        uint32_t wy1 = ((uint32_t)(fy_clamped & 0xFFFF) + 128u) >> 8; /* [0..256] */
        uint32_t wy0 = 256u - wy1;

        const uint32_t *srow0 = src_argb + (size_t)sy0 * (size_t)src_w;
        const uint32_t *srow1 = src_argb + (size_t)sy1 * (size_t)src_w;
        uint32_t       *drow  = dst_argb + (size_t)py * (size_t)stride_px;

        int64_t fx_cur = fx_init;
        for (int32_t px = x0; px < x1; ++px, fx_cur += step_x) {
            int64_t fx_clamped = (fx_cur < 0) ? 0 : ((fx_cur > max_fx) ? max_fx : fx_cur);
            int32_t sx0 = (int32_t)(fx_clamped >> 16);
            int32_t sx1 = (sx0 + 1 < src_w) ? (sx0 + 1) : sx0;

            uint32_t p00 = srow0[sx0];
            uint32_t p10 = srow0[sx1];
            uint32_t p01 = srow1[sx0];
            uint32_t p11 = srow1[sx1];

            if (p00 == p10 && p00 == p01 && p00 == p11) {
                uint32_t sa = (p00 >> 24) & 0xFFu;
                if (sa == 255u) {
                    drow[px] = p00;
                } else if (sa > 0u) {
                    blend_pixel_argb(&drow[px], (p00 >> 16) & 0xFFu,
                                     (p00 >> 8) & 0xFFu, p00 & 0xFFu, sa);
                }
                continue;
            }

            uint32_t wx1 = ((uint32_t)(fx_clamped & 0xFFFF) + 128u) >> 8; /* [0..256] */
            uint32_t wx0 = 256u - wx1;

            uint32_t w00 = wx0 * wy0;
            uint32_t w10 = wx1 * wy0;
            uint32_t w01 = wx0 * wy1;
            uint32_t w11 = wx1 * wy1;

            uint32_t sa = (((p00 >> 24) & 0xFFu) * w00 + ((p10 >> 24) & 0xFFu) * w10 +
                           ((p01 >> 24) & 0xFFu) * w01 + ((p11 >> 24) & 0xFFu) * w11 + 32768u) >> 16;
            if (sa == 0u) continue;

            uint32_t sr = (((p00 >> 16) & 0xFFu) * w00 + ((p10 >> 16) & 0xFFu) * w10 +
                           ((p01 >> 16) & 0xFFu) * w01 + ((p11 >> 16) & 0xFFu) * w11 + 32768u) >> 16;
            uint32_t sg = (((p00 >> 8) & 0xFFu) * w00 + ((p10 >> 8) & 0xFFu) * w10 +
                           ((p01 >> 8) & 0xFFu) * w01 + ((p11 >> 8) & 0xFFu) * w11 + 32768u) >> 16;
            uint32_t sb = ((p00 & 0xFFu) * w00 + (p10 & 0xFFu) * w10 +
                           (p01 & 0xFFu) * w01 + (p11 & 0xFFu) * w11 + 32768u) >> 16;

            if (sa >= 255u) {
                drow[px] = 0xFF000000u | (sr << 16) | (sg << 8) | sb;
            } else {
                blend_pixel_argb(&drow[px], sr, sg, sb, sa);
            }
        }
    }
}
