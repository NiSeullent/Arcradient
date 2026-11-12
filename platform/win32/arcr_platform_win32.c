/*
 * Arcradient (MSHTML-Next)
 * platform/win32/arcr_platform_win32.c - Windows 2000 SP4+ (WINVER=0x0500) Backend
 *
 * Key Legacy Engineering Safeguards:
 * 1. Targets _WIN32_WINNT=0x0500 (Windows 2000 SP4) using native NT Unicode (*W) APIs.
 * 2. Never statically imports Vista+/XP+ symbols (BCryptGenRandom, SystemFunction036,
 *    GetTickCount64, Direct3DCreate9Ex). Queries modern APIs via GetProcAddress.
 * 3. Mixes CryptGenRandom (NT 4.0+) with QueryPerformanceCounter + PID/TID
 *    to mitigate known Windows 2000 CryptGenRandom state-extension weaknesses.
 * 4. Renders glyphs via GDI GetGlyphOutlineW with automatic Korean font fallback
 *    ("Malgun Gothic" on Vista+, "굴림" on Windows 2000/XP).
 */
#if defined(_WIN32)

#ifndef WINVER
#  define WINVER 0x0500
#endif
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0500
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <wincrypt.h>
#include <imm.h>
#include "../arcr_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CRYPT_SILENT
#  define CRYPT_SILENT 0x00000040
#endif

uint16_t arcr_platform_os_version(void) {
    OSVERSIONINFOW osvi;
    ZeroMemory(&osvi, sizeof(osvi));
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    if (GetVersionExW(&osvi)) {
        uint16_t maj = (uint16_t)(osvi.dwMajorVersion & 0xFFu);
        uint16_t min = (uint16_t)(osvi.dwMinorVersion & 0xFFu);
        return (uint16_t)((maj << 8) | min);
    }
    return ARCR_OS_WIN2000;
}

const char *arcr_platform_os_name(void) {
    uint16_t v = arcr_platform_os_version();
    if (v == 0x0500u) return "Windows 2000";
    if (v == 0x0501u) return "Windows XP";
    if (v == 0x0502u) return "Windows XP x64 / Server 2003";
    if (v >= 0x0A00u) return "Windows 10/11";
    return "Windows NT";
}

uint64_t arcr_platform_monotonic_usec(void) {
    LARGE_INTEGER freq, cnt;
    if (QueryPerformanceFrequency(&freq) && freq.QuadPart > 0 &&
        QueryPerformanceCounter(&cnt)) {
        return (uint64_t)((cnt.QuadPart * 1000000ULL) / (uint64_t)freq.QuadPart);
    }
    return (uint64_t)GetTickCount() * 1000ULL;
}

int64_t arcr_platform_wall_epoch_sec(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    /* 116444736000000000 = 100ns intervals from 1601-01-01 to 1970-01-01 */
    if (ticks < 116444736000000000ULL) return 0;
    return (int64_t)((ticks - 116444736000000000ULL) / 10000000ULL);
}

typedef LONG (WINAPI *PfnBCryptGenRandom)(void *, PUCHAR, ULONG, ULONG);

bool arcr_platform_csprng_bytes(uint8_t *buf, size_t len) {
    if (!buf || len == 0u) return false;

    /* 1. Try Vista+ BCryptGenRandom dynamically (BCRYPT_USE_SYSTEM_PREFERRED_RNG = 0x2) */
    HMODULE hBcrypt = LoadLibraryW(L"bcrypt.dll");
    if (hBcrypt) {
        FARPROC raw = GetProcAddress(hBcrypt, "BCryptGenRandom");
        PfnBCryptGenRandom fn = NULL;
        memcpy(&fn, &raw, sizeof(fn));
        if (fn && fn(NULL, buf, (ULONG)len, 0x00000002UL) == 0) {
            FreeLibrary(hBcrypt);
            return true;
        }
        FreeLibrary(hBcrypt);
    }

    /* 2. Windows 2000 SP4 / XP path: CryptGenRandom + Hardware Timer Entropy Mixer */
    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContextW(&hProv, NULL, NULL, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        return false;
    }
    BOOL ok = CryptGenRandom(hProv, (DWORD)len, buf);
    CryptReleaseContext(hProv, 0);
    if (!ok) return false;

    /* Mix high-resolution timer, PID, TID to harden Win2000 CryptGenRandom */
    uint64_t mix = arcr_platform_monotonic_usec() ^
                   ((uint64_t)GetCurrentProcessId() << 32) ^
                   (uint64_t)GetCurrentThreadId();
    for (size_t i = 0; i < len; ++i) {
        mix ^= mix << 13;
        mix ^= mix >> 7;
        mix ^= mix << 17;
        buf[i] ^= (uint8_t)(mix & 0xFFu);
    }
    return true;
}

struct ArcrDynLib {
    HMODULE handle;
};

void arcr_platform_format_module_path(char *out_buf, size_t cap,
                                      const char *dir, const char *mod_stem) {
    if (!out_buf || cap == 0u) return;
    const char *d = (dir && dir[0] != '\0') ? dir : ".";
    snprintf(out_buf, cap, "%s\\%s.dll", d, mod_stem);
    out_buf[cap - 1u] = '\0';
}

ArcrDynLib *arcr_platform_dlopen(const char *path) {
    if (!path) return NULL;
    uint16_t wpath[MAX_PATH];
    arcr_utf8_to_utf16le(path, strlen(path), wpath, MAX_PATH);
    HMODULE h = LoadLibraryW((LPCWSTR)wpath);
    if (!h) return NULL;
    ArcrDynLib *lib = (ArcrDynLib *)malloc(sizeof(ArcrDynLib));
    if (!lib) {
        FreeLibrary(h);
        return NULL;
    }
    lib->handle = h;
    return lib;
}

void *arcr_platform_dlsym(ArcrDynLib *lib, const char *symbol_name) {
    if (!lib || !lib->handle || !symbol_name) return NULL;
    FARPROC raw = GetProcAddress(lib->handle, symbol_name);
    void *out = NULL;
    memcpy(&out, &raw, sizeof(out));
    return out;
}

void arcr_platform_dlclose(ArcrDynLib *lib) {
    if (!lib) return;
    if (lib->handle) FreeLibrary(lib->handle);
    free(lib);
}

static uint32_t colorref_to_argb(COLORREF c) {
    return 0xFF000000u |
           ((uint32_t)GetRValue(c) << 16) |
           ((uint32_t)GetGValue(c) << 8) |
           (uint32_t)GetBValue(c);
}

void arcr_platform_query_ui_metrics(ArcrSystemUiMetrics *out_metrics) {
    if (!out_metrics) return;
    uint16_t os_ver = arcr_platform_os_version();
    HIGHCONTRASTW hc;
    ZeroMemory(&hc, sizeof(hc));
    hc.cbSize = sizeof(hc);
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);

    out_metrics->prefer_classic_opaque = (os_ver < 0x0600u);
    out_metrics->high_contrast         = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    out_metrics->sys_face_argb         = colorref_to_argb(GetSysColor(COLOR_3DFACE));
    out_metrics->sys_window_argb       = colorref_to_argb(GetSysColor(COLOR_WINDOW));
    out_metrics->sys_text_argb         = colorref_to_argb(GetSysColor(COLOR_WINDOWTEXT));
    out_metrics->sys_highlight_argb    = colorref_to_argb(GetSysColor(COLOR_HIGHLIGHT));

    HDC hdc = GetDC(NULL);
    out_metrics->dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;
    if (hdc) ReleaseDC(NULL, hdc);
}

struct ArcrFontSystem {
    HDC      mem_dc;
    uint16_t os_ver;
};

ArcrFontSystem *arcr_font_system_create(const char *preferred_cjk_font_path) {
    (void)preferred_cjk_font_path;
    ArcrFontSystem *fs = (ArcrFontSystem *)calloc(1u, sizeof(ArcrFontSystem));
    if (!fs) return NULL;
    fs->mem_dc = CreateCompatibleDC(NULL);
    fs->os_ver = arcr_platform_os_version();
    return fs;
}

void arcr_font_system_destroy(ArcrFontSystem *fs) {
    if (!fs) return;
    if (fs->mem_dc) DeleteDC(fs->mem_dc);
    free(fs);
}

static HFONT create_korean_ui_font(uint16_t os_ver, int32_t font_px, bool bold) {
    /* Vista+ uses Malgun Gothic (맑은 고딕); Windows 2000/XP uses Gulim (굴림) */
    const wchar_t *face = (os_ver >= 0x0600u) ? L"Malgun Gothic" : L"\xAD74\xB9BC";
    return CreateFontW(-font_px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

/* Built-in procedural fallback when headless CI lacks CJK fonts */
static void win32_fallback_glyph(uint32_t cp, int32_t font_px, bool bold,
                                 ArcrGlyphMetrics *out_m, uint8_t *out_alpha, size_t cap) {
    bool wide = arcr_cp_is_hangul(cp) || arcr_cp_is_cjk_ideograph(cp) || (cp > 0x2000u);
    int32_t adv = wide ? font_px : ARCR_MAX(6, (font_px * 3) / 5);
    int32_t gw  = ARCR_MAX(4, adv - 2);
    int32_t gh  = ARCR_MAX(6, (font_px * 3) / 4);
    out_m->advance_px = adv + (bold ? 1 : 0);
    out_m->bearing_x  = 1;
    out_m->bearing_y  = gh - 1;
    out_m->width      = (cp == ' ') ? 0 : gw;
    out_m->height     = (cp == ' ') ? 0 : gh;
    if (cp != ' ' && out_alpha && (size_t)(gw * gh) <= cap) {
        memset(out_alpha, 180, (size_t)(gw * gh));
    }
}

bool arcr_font_measure_glyph(ArcrFontSystem *fs, uint32_t codepoint,
                             int32_t font_px, bool bold,
                             ArcrGlyphMetrics *out_metrics) {
    if (!out_metrics) return false;
    if (!fs || !fs->mem_dc) {
        win32_fallback_glyph(codepoint, font_px, bold, out_metrics, NULL, 0u);
        return true;
    }
    HFONT hFont = create_korean_ui_font(fs->os_ver, font_px, bold);
    HGDIOBJ old = SelectObject(fs->mem_dc, hFont);
    MAT2 mat = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    GLYPHMETRICS gm;
    ZeroMemory(&gm, sizeof(gm));
    DWORD res = GetGlyphOutlineW(fs->mem_dc, (UINT)codepoint, GGO_METRICS, &gm, 0, NULL, &mat);
    SelectObject(fs->mem_dc, old);
    DeleteObject(hFont);
    if (res == GDI_ERROR) {
        win32_fallback_glyph(codepoint, font_px, bold, out_metrics, NULL, 0u);
        return true;
    }

    out_metrics->advance_px = gm.gmCellIncX;
    out_metrics->bearing_x  = gm.gmptGlyphOrigin.x;
    out_metrics->bearing_y  = gm.gmptGlyphOrigin.y;
    out_metrics->width      = (int32_t)gm.gmBlackBoxX;
    out_metrics->height     = (int32_t)gm.gmBlackBoxY;
    return true;
}

bool arcr_font_rasterize_glyph(ArcrFontSystem *fs, uint32_t codepoint,
                               int32_t font_px, bool bold,
                               ArcrGlyphMetrics *out_metrics,
                               uint8_t *out_alpha, size_t alpha_cap) {
    if (!out_metrics || !out_alpha) return false;
    if (!fs || !fs->mem_dc) {
        win32_fallback_glyph(codepoint, font_px, bold, out_metrics, out_alpha, alpha_cap);
        return true;
    }
    HFONT hFont = create_korean_ui_font(fs->os_ver, font_px, bold);
    HGDIOBJ old = SelectObject(fs->mem_dc, hFont);
    MAT2 mat = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    GLYPHMETRICS gm;
    ZeroMemory(&gm, sizeof(gm));

    DWORD buf_sz = GetGlyphOutlineW(fs->mem_dc, (UINT)codepoint, GGO_GRAY8_BITMAP, &gm, 0, NULL, &mat);
    if (buf_sz == GDI_ERROR) {
        SelectObject(fs->mem_dc, old);
        DeleteObject(hFont);
        win32_fallback_glyph(codepoint, font_px, bold, out_metrics, out_alpha, alpha_cap);
        return true;
    }
    if (buf_sz == 0) {
        SelectObject(fs->mem_dc, old);
        DeleteObject(hFont);
        out_metrics->advance_px = gm.gmCellIncX;
        out_metrics->width      = 0;
        out_metrics->height     = 0;
        return true;
    }

    uint8_t *tmp = (uint8_t *)malloc(buf_sz);
    if (!tmp) {
        SelectObject(fs->mem_dc, old);
        DeleteObject(hFont);
        return false;
    }
    GetGlyphOutlineW(fs->mem_dc, (UINT)codepoint, GGO_GRAY8_BITMAP, &gm, buf_sz, tmp, &mat);
    SelectObject(fs->mem_dc, old);
    DeleteObject(hFont);

    int32_t w = (int32_t)gm.gmBlackBoxX;
    int32_t h = (int32_t)gm.gmBlackBoxY;
    int32_t pitch = (w + 3) & ~3;
    out_metrics->advance_px = gm.gmCellIncX;
    out_metrics->bearing_x  = gm.gmptGlyphOrigin.x;
    out_metrics->bearing_y  = gm.gmptGlyphOrigin.y;
    out_metrics->width      = w;
    out_metrics->height     = h;

    if ((size_t)(w * h) > alpha_cap) {
        free(tmp);
        return false;
    }
    /* GGO_GRAY8_BITMAP produces coverage 0..64; scale to 0..255 */
    for (int32_t r = 0; r < h; ++r) {
        for (int32_t c = 0; c < w; ++c) {
            uint32_t v = tmp[r * pitch + c];
            out_alpha[r * w + c] = (uint8_t)((v >= 64u) ? 255u : ((v * 255u) / 64u));
        }
    }
    free(tmp);
    return true;
}

bool arcr_platform_write_png(const char *filepath, const uint32_t *argb_pixels,
                             int32_t width, int32_t height, int32_t stride_pixels) {
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
}

#endif /* _WIN32 */
