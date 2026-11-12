/*
 * Arcradient (MSHTML-Next)
 * platform/arcr_platform.h - Strict OS Abstraction Boundary
 *
 * Core modules (base, core, css, layout, paint, accel) NEVER include <windows.h>
 * or OS-specific headers directly; all platform interactions pass through here.
 */
#ifndef ARCR_PLATFORM_H
#define ARCR_PLATFORM_H

#include "../base/arcr_base.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* OS version & timing */
uint16_t    arcr_platform_os_version(void);
const char *arcr_platform_os_name(void);
uint64_t    arcr_platform_monotonic_usec(void);
int64_t     arcr_platform_wall_epoch_sec(void);

/* Cryptographically seeded random bytes (Win2000 CryptGenRandom+timer mix / Vista+ BCrypt / POSIX urandom) */
bool arcr_platform_csprng_bytes(uint8_t *buf, size_t len);

/* Dynamic module loading (.dll / .so / .dylib) */
typedef struct ArcrDynLib ArcrDynLib;

void        arcr_platform_format_module_path(char *out_buf, size_t cap,
                                             const char *dir, const char *mod_stem);
ArcrDynLib *arcr_platform_dlopen(const char *path);
void       *arcr_platform_dlsym(ArcrDynLib *lib, const char *symbol_name);
void        arcr_platform_dlclose(ArcrDynLib *lib);

/* Platform System Theme & Accessibility Metrics (Win2000 GetSysColor / High-Contrast ready) */
typedef struct ArcrSystemUiMetrics {
    bool     prefer_classic_opaque; /* True on Win2000 or Classic theme */
    bool     high_contrast;
    uint32_t sys_face_argb;         /* COLOR_3DFACE equivalent */
    uint32_t sys_window_argb;       /* COLOR_WINDOW equivalent */
    uint32_t sys_text_argb;         /* COLOR_WINDOWTEXT equivalent */
    uint32_t sys_highlight_argb;    /* COLOR_HIGHLIGHT equivalent */
    int32_t  dpi;                   /* 96 default */
} ArcrSystemUiMetrics;

void arcr_platform_query_ui_metrics(ArcrSystemUiMetrics *out_metrics);

/* Font & Korean/Unicode Glyph Rasterization */
typedef struct ArcrFontSystem ArcrFontSystem;

typedef struct ArcrGlyphMetrics {
    int32_t advance_px;
    int32_t bearing_x;
    int32_t bearing_y; /* Offset from baseline upward */
    int32_t width;
    int32_t height;
} ArcrGlyphMetrics;

ArcrFontSystem *arcr_font_system_create(const char *preferred_cjk_font_path);
void            arcr_font_system_destroy(ArcrFontSystem *fs);

bool arcr_font_measure_glyph(ArcrFontSystem *fs, uint32_t codepoint,
                             int32_t font_px, bool bold,
                             ArcrGlyphMetrics *out_metrics);

/*
 * Rasterizes 8-bit coverage alpha (0..255) into `out_alpha` (size >= max_cap).
 * Works with FreeType2 (POSIX), GDI GetGlyphOutlineW (Win2000+), or built-in
 * pixel-aligned bitmap fallback when no external font file is present.
 */
bool arcr_font_rasterize_glyph(ArcrFontSystem *fs, uint32_t codepoint,
                               int32_t font_px, bool bold,
                               ArcrGlyphMetrics *out_metrics,
                               uint8_t *out_alpha, size_t alpha_cap);

/* Saves an ARGB8888 framebuffer to PNG (or PPM fallback) for headless snapshots & tests */
bool arcr_platform_write_png(const char *filepath, const uint32_t *argb_pixels,
                             int32_t width, int32_t height, int32_t stride_pixels);

#endif /* ARCR_PLATFORM_H */
