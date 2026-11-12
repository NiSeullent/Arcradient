/*
 * Arcradient (MSHTML-Next)
 * platform/posix/arcr_platform_posix.c - Linux/POSIX Backend + FreeType2 Korean Glyph Support
 */
#define _POSIX_C_SOURCE 200809L
#include "../arcr_platform.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(ARCR_HAVE_FREETYPE)
#  include <ft2build.h>
#  include FT_FREETYPE_H
#endif

#if defined(ARCR_HAVE_LIBPNG)
#  include <png.h>
#endif

uint16_t arcr_platform_os_version(void) {
    return ARCR_OS_POSIX_ANY;
}

const char *arcr_platform_os_name(void) {
#if defined(__linux__)
    return "Linux (POSIX Backend)";
#elif defined(__APPLE__)
    return "macOS (POSIX Backend)";
#else
    return "POSIX";
#endif
}

uint64_t arcr_platform_monotonic_usec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000L);
}

int64_t arcr_platform_wall_epoch_sec(void) {
    return (int64_t)time(NULL);
}

bool arcr_platform_csprng_bytes(uint8_t *buf, size_t len) {
    if (!buf || len == 0u) return false;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    size_t done = 0u;
    while (done < len) {
        ssize_t r = read(fd, buf + done, len - done);
        if (r <= 0) {
            close(fd);
            return false;
        }
        done += (size_t)r;
    }
    close(fd);
    return true;
}

struct ArcrDynLib {
    void *handle;
};

void arcr_platform_format_module_path(char *out_buf, size_t cap,
                                      const char *dir, const char *mod_stem) {
    if (!out_buf || cap == 0u) return;
#if defined(__APPLE__)
    const char *ext = ".dylib";
#else
    const char *ext = ".so";
#endif
    const char *d = (dir && dir[0] != '\0') ? dir : ".";
    snprintf(out_buf, cap, "%s/lib%s%s", d, mod_stem, ext);
}

ArcrDynLib *arcr_platform_dlopen(const char *path) {
    if (!path) return NULL;
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) return NULL;
    ArcrDynLib *lib = (ArcrDynLib *)malloc(sizeof(ArcrDynLib));
    if (!lib) {
        dlclose(h);
        return NULL;
    }
    lib->handle = h;
    return lib;
}

void *arcr_platform_dlsym(ArcrDynLib *lib, const char *symbol_name) {
    if (!lib || !lib->handle || !symbol_name) return NULL;
    return dlsym(lib->handle, symbol_name);
}

void arcr_platform_dlclose(ArcrDynLib *lib) {
    if (!lib) return;
    if (lib->handle) dlclose(lib->handle);
    free(lib);
}

void arcr_platform_query_ui_metrics(ArcrSystemUiMetrics *out_metrics) {
    if (!out_metrics) return;
    out_metrics->prefer_classic_opaque = false;
    out_metrics->high_contrast         = false;
    out_metrics->sys_face_argb         = 0xFFD4D0C8u; /* Classic Win2000 3DFace reference */
    out_metrics->sys_window_argb       = 0xFFFFFFFFu;
    out_metrics->sys_text_argb         = 0xFF000000u;
    out_metrics->sys_highlight_argb    = 0xFF0A246Au; /* Classic Win2000 active navy */
    out_metrics->dpi                   = 96;
}

/* -------------------------------------------------------------------------
 * Compact Built-in 5x7 Pixel Fallback Font + FreeType2 CJK KR Support
 * ------------------------------------------------------------------------- */
struct ArcrFontSystem {
    bool has_ft;
#if defined(ARCR_HAVE_FREETYPE)
    FT_Library ft_lib;
    FT_Face    face_regular;
    FT_Face    face_bold;
    int32_t    cur_reg_px;
    int32_t    cur_bold_px;
#endif
};

static const char *const kDefaultCjkFontCandidates[] = {
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    NULL
};

static const char *const kDefaultCjkBoldCandidates[] = {
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    NULL
};

ArcrFontSystem *arcr_font_system_create(const char *preferred_cjk_font_path) {
    ArcrFontSystem *fs = (ArcrFontSystem *)calloc(1u, sizeof(ArcrFontSystem));
    if (!fs) return NULL;

#if defined(ARCR_HAVE_FREETYPE)
    if (FT_Init_FreeType(&fs->ft_lib) == 0) {
        /* Note: In NotoSansCJK-*.ttc, face index 1 is typically Noto Sans CJK KR */
        if (preferred_cjk_font_path && preferred_cjk_font_path[0] != '\0') {
            if (FT_New_Face(fs->ft_lib, preferred_cjk_font_path, 1, &fs->face_regular) != 0) {
                (void)FT_New_Face(fs->ft_lib, preferred_cjk_font_path, 0, &fs->face_regular);
            }
        }
        for (size_t i = 0; !fs->face_regular && kDefaultCjkFontCandidates[i]; ++i) {
            const char *p = kDefaultCjkFontCandidates[i];
            if (FT_New_Face(fs->ft_lib, p, 1, &fs->face_regular) != 0) {
                (void)FT_New_Face(fs->ft_lib, p, 0, &fs->face_regular);
            }
        }
        for (size_t i = 0; !fs->face_bold && kDefaultCjkBoldCandidates[i]; ++i) {
            const char *p = kDefaultCjkBoldCandidates[i];
            if (FT_New_Face(fs->ft_lib, p, 1, &fs->face_bold) != 0) {
                (void)FT_New_Face(fs->ft_lib, p, 0, &fs->face_bold);
            }
        }
        if (fs->face_regular) {
            fs->has_ft = true;
        } else {
            FT_Done_FreeType(fs->ft_lib);
        }
    }
#else
    (void)preferred_cjk_font_path;
#endif
    return fs;
}

void arcr_font_system_destroy(ArcrFontSystem *fs) {
    if (!fs) return;
#if defined(ARCR_HAVE_FREETYPE)
    if (fs->has_ft) {
        if (fs->face_bold) FT_Done_Face(fs->face_bold);
        if (fs->face_regular) FT_Done_Face(fs->face_regular);
        FT_Done_FreeType(fs->ft_lib);
    }
#endif
    free(fs);
}

/* Built-in procedural crisp fallback glyph so engine works with 0 font files */
static void fallback_glyph(uint32_t cp, int32_t font_px, bool bold,
                           ArcrGlyphMetrics *out_m, uint8_t *out_alpha, size_t cap) {
    bool wide = arcr_cp_is_hangul(cp) || arcr_cp_is_cjk_ideograph(cp) || (cp > 0x2000u);
    int32_t adv = wide ? font_px : ARCR_MAX(6, (font_px * 3) / 5);
    int32_t gw  = ARCR_MAX(4, adv - 2);
    int32_t gh  = ARCR_MAX(6, (font_px * 3) / 4);
    if (cp == ' ') {
        out_m->advance_px = adv;
        out_m->bearing_x  = 0;
        out_m->bearing_y  = 0;
        out_m->width      = 0;
        out_m->height     = 0;
        return;
    }
    out_m->advance_px = adv + (bold ? 1 : 0);
    out_m->bearing_x  = 1;
    out_m->bearing_y  = gh - 1;
    out_m->width      = gw;
    out_m->height     = gh;
    if (out_alpha && (size_t)(gw * gh) <= cap) {
        memset(out_alpha, 0, (size_t)(gw * gh));
        for (int32_t y = 0; y < gh; ++y) {
            for (int32_t x = 0; x < gw; ++x) {
                bool edge = (y == 0 || y == gh - 1 || x == 0 || x == gw - 1);
                bool pat  = (((x + y + (int32_t)(cp & 7u)) % 3) == 0);
                out_alpha[y * gw + x] = (edge || pat) ? 220u : 0u;
            }
        }
    }
}

bool arcr_font_measure_glyph(ArcrFontSystem *fs, uint32_t codepoint,
                             int32_t font_px, bool bold,
                             ArcrGlyphMetrics *out_metrics) {
    if (!out_metrics) return false;
    if (font_px < 8) font_px = 8;

#if defined(ARCR_HAVE_FREETYPE)
    if (fs && fs->has_ft) {
        FT_Face face = (bold && fs->face_bold) ? fs->face_bold : fs->face_regular;
        int32_t *cached_px = (face == fs->face_bold) ? &fs->cur_bold_px : &fs->cur_reg_px;
        if (*cached_px != font_px) {
            FT_Set_Pixel_Sizes(face, 0u, (FT_UInt)font_px);
            *cached_px = font_px;
        }
        FT_UInt gidx = FT_Get_Char_Index(face, (FT_ULong)codepoint);
        if (gidx != 0u && FT_Load_Glyph(face, gidx, FT_LOAD_DEFAULT) == 0) {
            out_metrics->advance_px = (int32_t)((face->glyph->advance.x + 32) >> 6);
            out_metrics->bearing_x  = (int32_t)(face->glyph->metrics.horiBearingX >> 6);
            out_metrics->bearing_y  = (int32_t)(face->glyph->metrics.horiBearingY >> 6);
            out_metrics->width      = (int32_t)(face->glyph->metrics.width >> 6);
            out_metrics->height     = (int32_t)(face->glyph->metrics.height >> 6);
            return true;
        }
    }
#else
    (void)fs;
#endif
    fallback_glyph(codepoint, font_px, bold, out_metrics, NULL, 0u);
    return true;
}

bool arcr_font_rasterize_glyph(ArcrFontSystem *fs, uint32_t codepoint,
                               int32_t font_px, bool bold,
                               ArcrGlyphMetrics *out_metrics,
                               uint8_t *out_alpha, size_t alpha_cap) {
    if (!out_metrics || !out_alpha) return false;
    if (font_px < 8) font_px = 8;

#if defined(ARCR_HAVE_FREETYPE)
    if (fs && fs->has_ft) {
        FT_Face face = (bold && fs->face_bold) ? fs->face_bold : fs->face_regular;
        int32_t *cached_px = (face == fs->face_bold) ? &fs->cur_bold_px : &fs->cur_reg_px;
        if (*cached_px != font_px) {
            FT_Set_Pixel_Sizes(face, 0u, (FT_UInt)font_px);
            *cached_px = font_px;
        }
        FT_UInt gidx = FT_Get_Char_Index(face, (FT_ULong)codepoint);
        if (gidx != 0u && FT_Load_Glyph(face, gidx, FT_LOAD_RENDER) == 0) {
            FT_GlyphSlot slot = face->glyph;
            int32_t w = (int32_t)slot->bitmap.width;
            int32_t h = (int32_t)slot->bitmap.rows;
            out_metrics->advance_px = (int32_t)((slot->advance.x + 32) >> 6);
            out_metrics->bearing_x  = slot->bitmap_left;
            out_metrics->bearing_y  = slot->bitmap_top;
            out_metrics->width      = w;
            out_metrics->height     = h;
            if ((size_t)(w * h) > alpha_cap) return false;
            if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
                for (int32_t r = 0; r < h; ++r) {
                    memcpy(out_alpha + r * w,
                           slot->bitmap.buffer + r * slot->bitmap.pitch,
                           (size_t)w);
                }
                return true;
            } else if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                /* Embedded bitmap strike (e.g., 12-16px CJK bitmap tables!) */
                for (int32_t r = 0; r < h; ++r) {
                    const uint8_t *row = slot->bitmap.buffer + r * slot->bitmap.pitch;
                    for (int32_t c = 0; c < w; ++c) {
                        bool bit = (row[c >> 3] & (0x80u >> (c & 7))) != 0u;
                        out_alpha[r * w + c] = bit ? 255u : 0u;
                    }
                }
                return true;
            }
        }
    }
#else
    (void)fs;
#endif
    fallback_glyph(codepoint, font_px, bold, out_metrics, out_alpha, alpha_cap);
    return true;
}

bool arcr_platform_write_png(const char *filepath, const uint32_t *argb_pixels,
                             int32_t width, int32_t height, int32_t stride_pixels) {
    if (!filepath || !argb_pixels || width <= 0 || height <= 0) return false;

#if defined(ARCR_HAVE_LIBPNG)
    FILE *fp = fopen(filepath, "wb");
    if (!fp) return false;

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        fclose(fp);
        return false;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        fclose(fp);
        return false;
    }
    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        return false;
    }

    png_init_io(png_ptr, fp);
    png_set_IHDR(png_ptr, info_ptr, (png_uint_32)width, (png_uint_32)height,
                 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);

    uint8_t *row_buf = (uint8_t *)malloc((size_t)width * 4u);
    if (!row_buf) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        return false;
    }

    for (int32_t y = 0; y < height; ++y) {
        const uint32_t *src = argb_pixels + (size_t)y * (size_t)stride_pixels;
        for (int32_t x = 0; x < width; ++x) {
            uint32_t px = src[x];
            row_buf[x * 4 + 0] = (uint8_t)((px >> 16) & 0xFFu); /* R */
            row_buf[x * 4 + 1] = (uint8_t)((px >> 8) & 0xFFu);  /* G */
            row_buf[x * 4 + 2] = (uint8_t)(px & 0xFFu);         /* B */
            row_buf[x * 4 + 3] = (uint8_t)((px >> 24) & 0xFFu); /* A */
        }
        png_write_row(png_ptr, row_buf);
    }

    free(row_buf);
    png_write_end(png_ptr, NULL);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);
    return true;
#else
    /* PPM P6 binary fallback when built without libpng */
    FILE *fp = fopen(filepath, "wb");
    if (!fp) return false;
    fprintf(fp, "P6\n%d %d\n255\n", (int)width, (int)height);
    for (int32_t y = 0; y < height; ++y) {
        const uint32_t *src = argb_pixels + (size_t)y * (size_t)stride_pixels;
        for (int32_t x = 0; x < width; ++x) {
            uint32_t px = src[x];
            uint8_t rgb[3] = {
                (uint8_t)((px >> 16) & 0xFFu),
                (uint8_t)((px >> 8) & 0xFFu),
                (uint8_t)(px & 0xFFu)
            };
            fwrite(rgb, 1u, 3u, fp);
        }
    }
    fclose(fp);
    return true;
#endif
}
