/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * platform/win32/arcr_platform_win32.c - Windows 2000 SP4+ (WINVER=0x0500) Backend
 *
 * Key Legacy & Modern Engineering Safeguards:
 * 1. Targets _WIN32_WINNT=0x0500 (Windows 2000 SP4) using native NT Unicode (*W) APIs.
 * 2. Never statically imports Vista+/XP+ symbols (BCryptGenRandom, SystemFunction036,
 *    GetTickCount64, Direct3DCreate9Ex). Queries modern APIs via GetProcAddress + memcpy.
 * 3. Mixes CryptGenRandom (NT 4.0+) with QueryPerformanceCounter + PID/TID
 *    to mitigate known Windows 2000 CryptGenRandom state-extension weaknesses.
 * 4. Thread-safe CRITICAL_SECTION O(1) 4-way set-associative glyph cache + GDI GetGlyphOutlineW
 *    supporting Korean UI ("Malgun Gothic" / "굴림") and Monospace ("Consolas" / "Courier New").
 * 5. Writes genuine RFC 2083 PNG snapshots via arcr_image_write_png_file (zero external DLLs).
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
#include "../../base/arcr_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CRYPT_SILENT
#  define CRYPT_SILENT 0x00000040
#endif

#ifndef INVALID_FILE_ATTRIBUTES
#  define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

/* -------------------------------------------------------------------------
 * 1. OS Version, Monotonic/Wall Clocks & CSPRNG
 * ------------------------------------------------------------------------- */
typedef LONG (WINAPI *PfnRtlGetVersion)(OSVERSIONINFOW *);

uint16_t arcr_platform_os_version(void) {
    OSVERSIONINFOW osvi;
    ZeroMemory(&osvi, sizeof(osvi));
    osvi.dwOSVersionInfoSize = sizeof(osvi);

    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        FARPROC raw = GetProcAddress(hNtdll, "RtlGetVersion");
        if (raw) {
            PfnRtlGetVersion fn_rtl = NULL;
            memcpy(&fn_rtl, &raw, sizeof(fn_rtl));
            if (fn_rtl && fn_rtl(&osvi) == 0 && osvi.dwMajorVersion > 0u) {
                uint16_t maj = (uint16_t)(osvi.dwMajorVersion & 0xFFu);
                uint16_t min = (uint16_t)(osvi.dwMinorVersion & 0xFFu);
                return (uint16_t)((maj << 8) | min);
            }
        }
    }

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
    if (v >= 0x0600u && v < 0x0602u) return "Windows Vista / 7";
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
    for (size_t i = 0u; i < len; ++i) {
        mix ^= mix << 13;
        mix ^= mix >> 7;
        mix ^= mix << 17;
        buf[i] ^= (uint8_t)(mix & 0xFFu);
    }
    return true;
}

/* -------------------------------------------------------------------------
 * 2. Profile / Downloads Directories, Recursive mkdir_p & UTF-8 Clipboard
 * ------------------------------------------------------------------------- */
static bool win32_utf8_to_wide(const char *u8, WCHAR *out_w, int out_cap) {
    if (!u8 || !out_w || out_cap <= 0) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, out_w, out_cap);
    if (n <= 0) {
        out_w[0] = L'\0';
        return false;
    }
    return true;
}

static bool win32_wide_to_utf8(const WCHAR *wstr, char *out_u8, size_t out_cap) {
    if (!wstr || !out_u8 || out_cap == 0u) return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out_u8, (int)out_cap, NULL, NULL);
    if (n <= 0) {
        out_u8[0] = '\0';
        return false;
    }
    return true;
}

static bool win32_is_existing_dir_w(const WCHAR *wpath) {
    if (!wpath || wpath[0] == L'\0') return false;
    DWORD attr = GetFileAttributesW(wpath);
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0u);
}

bool arcr_platform_mkdir_p(const char *path) {
    if (!path || path[0] == '\0') return false;

    WCHAR wtmp[1024];
    if (!win32_utf8_to_wide(path, wtmp, 1024)) return false;

    size_t len = 0u;
    while (wtmp[len] != L'\0') {
        if (wtmp[len] == L'/') wtmp[len] = L'\\';
        ++len;
    }
    if (len == 0u) return false;

    /* Strip trailing backslashes except root ("C:\" or "\") */
    while (len > 1u && wtmp[len - 1u] == L'\\') {
        if (len == 3u && wtmp[1u] == L':') break;
        wtmp[--len] = L'\0';
    }

    if (win32_is_existing_dir_w(wtmp)) return true;

    size_t start = 1u;
    if (len >= 2u && wtmp[1u] == L':') {
        start = (len >= 3u && wtmp[2u] == L'\\') ? 3u : 2u;
    } else if (len >= 2u && wtmp[0u] == L'\\' && wtmp[1u] == L'\\') {
        /* Skip UNC share prefix \\server\share\ */
        size_t slashes = 0u;
        start = 2u;
        while (start < len && slashes < 2u) {
            if (wtmp[start] == L'\\') ++slashes;
            ++start;
        }
    }

    for (size_t i = start; i < len; ++i) {
        if (wtmp[i] == L'\\') {
            wtmp[i] = L'\0';
            if (wtmp[0] != L'\0' && !win32_is_existing_dir_w(wtmp)) {
                if (!CreateDirectoryW(wtmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                    return false;
                }
            }
            wtmp[i] = L'\\';
        }
    }

    if (!win32_is_existing_dir_w(wtmp)) {
        if (!CreateDirectoryW(wtmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return win32_is_existing_dir_w(wtmp);
}

static bool query_env_utf8(const char *name_a, const WCHAR *name_w, char *out_buf, size_t cap) {
    WCHAR wval[MAX_PATH * 2];
    DWORD wn = GetEnvironmentVariableW(name_w, wval, (DWORD)(sizeof(wval) / sizeof(wval[0])));
    if (wn > 0u && wn < (DWORD)(sizeof(wval) / sizeof(wval[0]))) {
        if (win32_wide_to_utf8(wval, out_buf, cap) && out_buf[0] != '\0') {
            return true;
        }
    }
    DWORD an = GetEnvironmentVariableA(name_a, out_buf, (DWORD)cap);
    return (an > 0u && (size_t)an < cap && out_buf[0] != '\0');
}

bool arcr_platform_get_profile_dir(char *out_dir, size_t cap) {
    if (!out_dir || cap < 4u) return false;

    if (query_env_utf8("WATERSCAPE_PROFILE_DIR", L"WATERSCAPE_PROFILE_DIR", out_dir, cap)) {
        (void)arcr_platform_mkdir_p(out_dir);
        return true;
    }

    char appdata[512];
    if (query_env_utf8("APPDATA", L"APPDATA", appdata, sizeof(appdata))) {
        int n = snprintf(out_dir, cap, "%s\\Waterscape", appdata);
        if (n > 0 && (size_t)n < cap) {
            (void)arcr_platform_mkdir_p(out_dir);
            return true;
        }
    }

    size_t fb = arcr_str_copy(out_dir, cap, "C:\\WaterscapeProfile");
    if (fb >= cap) return false;
    (void)arcr_platform_mkdir_p(out_dir);
    return true;
}

bool arcr_platform_get_downloads_dir(char *out_dir, size_t cap) {
    if (!out_dir || cap < 4u) return false;

    if (query_env_utf8("WATERSCAPE_DOWNLOADS_DIR", L"WATERSCAPE_DOWNLOADS_DIR", out_dir, cap)) {
        (void)arcr_platform_mkdir_p(out_dir);
        return true;
    }

    char userprof[512];
    if (query_env_utf8("USERPROFILE", L"USERPROFILE", userprof, sizeof(userprof))) {
        uint16_t os_v = arcr_platform_os_version();
        if (os_v < 0x0600u) {
            /* Windows 2000 / XP convention: %USERPROFILE%\My Documents\Downloads */
            char mydocs[640];
            int mn = snprintf(mydocs, sizeof(mydocs), "%s\\My Documents", userprof);
            WCHAR wmydocs[640];
            if (mn > 0 && (size_t)mn < sizeof(mydocs) &&
                win32_utf8_to_wide(mydocs, wmydocs, 640) &&
                win32_is_existing_dir_w(wmydocs)) {
                int dn = snprintf(out_dir, cap, "%s\\Downloads", mydocs);
                if (dn > 0 && (size_t)dn < cap && arcr_platform_mkdir_p(out_dir)) {
                    return true;
                }
            }
        }
        int n = snprintf(out_dir, cap, "%s\\Downloads", userprof);
        if (n > 0 && (size_t)n < cap && arcr_platform_mkdir_p(out_dir)) {
            return true;
        }
    }

    size_t fb = arcr_str_copy(out_dir, cap, "C:\\WaterscapeDownloads");
    if (fb >= cap) return false;
    (void)arcr_platform_mkdir_p(out_dir);
    return true;
}

static volatile LONG g_clip_spin     = 0;
static char          g_clip_fallback[65536];
static bool          g_clip_has_text = false;

static void clip_spin_lock(void) {
    while (InterlockedCompareExchange(&g_clip_spin, 1L, 0L) != 0L) {
        Sleep(0);
    }
}

static void clip_spin_unlock(void) {
    InterlockedExchange(&g_clip_spin, 0L);
}

bool arcr_platform_clipboard_set_utf8(const char *utf8_text) {
    if (!utf8_text) return false;

    clip_spin_lock();
    (void)arcr_str_copy(g_clip_fallback, sizeof(g_clip_fallback), utf8_text);
    g_clip_has_text = true;
    clip_spin_unlock();

    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_text, -1, NULL, 0);
    if (wlen > 0 && OpenClipboard(NULL)) {
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wlen * sizeof(WCHAR));
        if (hMem) {
            WCHAR *dst = (WCHAR *)GlobalLock(hMem);
            if (dst) {
                MultiByteToWideChar(CP_UTF8, 0, utf8_text, -1, dst, wlen);
                GlobalUnlock(hMem);
                if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
                    GlobalFree(hMem);
                }
            } else {
                GlobalFree(hMem);
            }
        }
        CloseClipboard();
    }
    return true;
}

bool arcr_platform_clipboard_get_utf8(char *out_buf, size_t cap) {
    if (!out_buf || cap == 0u) return false;

    if (IsClipboardFormatAvailable(CF_UNICODETEXT) && OpenClipboard(NULL)) {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            const WCHAR *wstr = (const WCHAR *)GlobalLock(hData);
            if (wstr) {
                bool ok = win32_wide_to_utf8(wstr, out_buf, cap);
                GlobalUnlock(hData);
                CloseClipboard();
                if (ok) return true;
            }
        }
        CloseClipboard();
    }

    clip_spin_lock();
    bool has = g_clip_has_text;
    if (has) {
        (void)arcr_str_copy(out_buf, cap, g_clip_fallback);
    } else {
        out_buf[0] = '\0';
    }
    clip_spin_unlock();
    return has;
}

/* -------------------------------------------------------------------------
 * 3. Dynamic Module Loading & System UI Metrics
 * ------------------------------------------------------------------------- */
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
    if (!path || path[0] == '\0') return NULL;
    WCHAR wpath[MAX_PATH];
    arcr_utf8_to_utf16le(path, strlen(path), (uint16_t *)wpath, MAX_PATH);

    bool has_sep = false;
    for (size_t i = 0u; wpath[i] != L'\0'; ++i) {
        if (wpath[i] == L'/') {
            wpath[i] = L'\\';
        }
        if (wpath[i] == L'\\' || wpath[i] == L':') {
            has_sep = true;
        }
    }

    WCHAR full_w[MAX_PATH];
    const WCHAR *load_target = wpath;
    if (has_sep) {
        DWORD n = GetFullPathNameW(wpath, MAX_PATH, full_w, NULL);
        if (n > 0u && n < MAX_PATH) {
            load_target = full_w;
        }
    }

    HMODULE h = LoadLibraryW(load_target);
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

/* -------------------------------------------------------------------------
 * 4. Thread-Safe O(1) Cached Win32 GDI Font System (Korean UI + Monospace)
 * ------------------------------------------------------------------------- */
enum {
    ARCR_GLYPH_CACHE_SETS     = 512,
    ARCR_GLYPH_CACHE_WAYS     = 4,
    ARCR_GLYPH_POOL_BYTES     = 2097152, /* 2 MB glyph alpha slab buffer */
    ARCR_GLYPH_MAX_CACHE_AREA = 65536
};

typedef struct ArcrGlyphCacheSlot {
    uint64_t         key;
    uint32_t         bitmap_off;
    uint32_t         last_used;
    ArcrGlyphMetrics metrics;
    bool             has_bitmap;
} ArcrGlyphCacheSlot;

struct ArcrFontSystem {
    CRITICAL_SECTION   cs_storage;
    CRITICAL_SECTION  *cs_ptr;
    HDC                mem_dc;
    uint16_t           os_ver;
    HFONT              cached_hfont;
    int32_t            cached_px;
    uint32_t           cached_style_mask;
    ArcrGlyphCacheSlot slots[ARCR_GLYPH_CACHE_SETS][ARCR_GLYPH_CACHE_WAYS];
    uint8_t           *bitmap_pool;
    size_t             bitmap_pool_used;
    size_t             cache_entries;
    size_t             cache_hits;
    uint32_t           access_clock;
};

static inline uint64_t make_glyph_key(uint32_t cp, int32_t font_px, bool bold, bool monospace) {
    uint64_t px_u  = (uint64_t)(uint32_t)font_px & 0xFFFFULL;
    uint64_t flags = (bold ? 1ULL : 0ULL) | (monospace ? 2ULL : 0ULL);
    return (1ULL << 63) | ((uint64_t)cp << 18) | (px_u << 2) | flags;
}

static inline uint32_t hash_glyph_key_set(uint64_t key) {
    uint64_t h = key * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 32;
    h ^= h >> 15;
    return (uint32_t)h & (uint32_t)(ARCR_GLYPH_CACHE_SETS - 1);
}

static void win32_reset_cache_locked(ArcrFontSystem *fs, bool reset_hits) {
    memset(fs->slots, 0, sizeof(fs->slots));
    fs->bitmap_pool_used = 0u;
    fs->cache_entries    = 0u;
    fs->access_clock     = 0u;
    if (reset_hits) {
        fs->cache_hits = 0u;
    }
}

ArcrFontSystem *arcr_font_system_create(const char *preferred_cjk_font_path) {
    (void)preferred_cjk_font_path;
    ArcrFontSystem *fs = (ArcrFontSystem *)calloc(1u, sizeof(ArcrFontSystem));
    if (!fs) return NULL;
    InitializeCriticalSection(&fs->cs_storage);
    fs->cs_ptr            = &fs->cs_storage;
    fs->mem_dc            = CreateCompatibleDC(NULL);
    fs->os_ver            = arcr_platform_os_version();
    fs->cached_style_mask = 0xFFFFFFFFu;
    fs->bitmap_pool       = (uint8_t *)malloc(ARCR_GLYPH_POOL_BYTES);
    return fs;
}

void arcr_font_system_destroy(ArcrFontSystem *fs) {
    if (!fs) return;
    if (fs->cached_hfont) DeleteObject(fs->cached_hfont);
    if (fs->mem_dc) DeleteDC(fs->mem_dc);
    free(fs->bitmap_pool);
    DeleteCriticalSection(&fs->cs_storage);
    free(fs);
}

void arcr_font_system_purge_cache(ArcrFontSystem *fs) {
    if (!fs) return;
    EnterCriticalSection(fs->cs_ptr);
    win32_reset_cache_locked(fs, true);
    LeaveCriticalSection(fs->cs_ptr);
}

void arcr_font_system_cache_stats(const ArcrFontSystem *fs,
                                  size_t *out_entries, size_t *out_hits) {
    if (!fs) {
        if (out_entries) *out_entries = 0u;
        if (out_hits) *out_hits = 0u;
        return;
    }
    EnterCriticalSection(fs->cs_ptr);
    if (out_entries) *out_entries = fs->cache_entries;
    if (out_hits) *out_hits = fs->cache_hits;
    LeaveCriticalSection(fs->cs_ptr);
}

static HFONT select_gdi_font_locked(ArcrFontSystem *fs, uint32_t cp,
                                    int32_t font_px, bool bold, bool monospace) {
    bool is_cjk = arcr_cp_is_hangul(cp) || arcr_cp_is_cjk_ideograph(cp);
    uint32_t mask = (bold ? 1u : 0u) | (monospace ? 2u : 0u) | (is_cjk ? 4u : 0u);
    if (fs->cached_hfont && fs->cached_px == font_px && fs->cached_style_mask == mask) {
        return fs->cached_hfont;
    }

    const wchar_t *face;
    DWORD pitch_and_family = DEFAULT_PITCH | FF_DONTCARE;
    if (monospace && !is_cjk) {
        face = (fs->os_ver >= 0x0600u) ? L"Consolas" : L"Courier New";
        pitch_and_family = FIXED_PITCH | FF_MODERN;
    } else if (monospace && is_cjk) {
        /* GulimChe (굴림체) on Win2000/XP; Malgun Gothic on Vista+ */
        face = (fs->os_ver >= 0x0600u) ? L"Malgun Gothic" : L"\xAD74\xB9BC\xCCB4";
    } else if (is_cjk) {
        /* Malgun Gothic on Vista+; Gulim (굴림) on Win2000/XP */
        face = (fs->os_ver >= 0x0600u) ? L"Malgun Gothic" : L"\xAD74\xB9BC";
    } else {
        face = (fs->os_ver >= 0x0600u) ? L"Malgun Gothic" : L"Tahoma";
    }

    HFONT hf = CreateFontW(-font_px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                           FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           DEFAULT_QUALITY, pitch_and_family, face);
    if (hf) {
        if (fs->cached_hfont) DeleteObject(fs->cached_hfont);
        fs->cached_hfont      = hf;
        fs->cached_px         = font_px;
        fs->cached_style_mask = mask;
    }
    return fs->cached_hfont;
}

static void win32_fallback_glyph(uint32_t cp, int32_t font_px, bool bold, bool monospace,
                                 ArcrGlyphMetrics *out_m, uint8_t *out_alpha, size_t cap) {
    bool wide = arcr_cp_is_hangul(cp) || arcr_cp_is_cjk_ideograph(cp) || (cp > 0x2000u);
    int32_t adv = wide ? font_px : ARCR_MAX(6, (font_px * 3) / 5);
    int32_t gw  = ARCR_MAX(4, adv - 2);
    int32_t gh  = ARCR_MAX(6, (font_px * 3) / 4);
    out_m->advance_px = adv + ((bold && !monospace) ? 1 : 0);
    out_m->bearing_x  = (cp == ' ') ? 0 : 1;
    out_m->bearing_y  = (cp == ' ') ? 0 : (gh - 1);
    out_m->width      = (cp == ' ') ? 0 : gw;
    out_m->height     = (cp == ' ') ? 0 : gh;
    if (cp != ' ' && out_alpha && (size_t)(gw * gh) <= cap) {
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

static ArcrGlyphCacheSlot *win32_find_slot_locked(ArcrFontSystem *fs, uint32_t set_idx, uint64_t key) {
    for (size_t w = 0u; w < ARCR_GLYPH_CACHE_WAYS; ++w) {
        if (fs->slots[set_idx][w].key == key) {
            return &fs->slots[set_idx][w];
        }
    }
    return NULL;
}

static ArcrGlyphCacheSlot *win32_alloc_slot_locked(ArcrFontSystem *fs, uint32_t set_idx, uint64_t key) {
    size_t   victim = 0u;
    uint32_t oldest = UINT32_MAX;
    for (size_t w = 0u; w < ARCR_GLYPH_CACHE_WAYS; ++w) {
        if (fs->slots[set_idx][w].key == 0ULL) {
            fs->cache_entries++;
            victim = w;
            break;
        }
        if (fs->slots[set_idx][w].last_used < oldest) {
            oldest = fs->slots[set_idx][w].last_used;
            victim = w;
        }
    }
    ArcrGlyphCacheSlot *slot = &fs->slots[set_idx][victim];
    memset(slot, 0, sizeof(*slot));
    slot->key       = key;
    slot->last_used = ++fs->access_clock;
    return slot;
}

bool arcr_font_measure_glyph_ex(ArcrFontSystem *fs, uint32_t codepoint,
                                int32_t font_px, bool bold, bool monospace,
                                ArcrGlyphMetrics *out_metrics) {
    if (!out_metrics) return false;
    if (font_px < 8) font_px = 8;
    if (font_px > 512) font_px = 512;

    if (!fs || !fs->mem_dc) {
        win32_fallback_glyph(codepoint, font_px, bold, monospace, out_metrics, NULL, 0u);
        return true;
    }

    uint64_t key     = make_glyph_key(codepoint, font_px, bold, monospace);
    uint32_t set_idx = hash_glyph_key_set(key);

    EnterCriticalSection(fs->cs_ptr);

    ArcrGlyphCacheSlot *hit = win32_find_slot_locked(fs, set_idx, key);
    if (hit) {
        fs->cache_hits++;
        hit->last_used = ++fs->access_clock;
        *out_metrics   = hit->metrics;
        LeaveCriticalSection(fs->cs_ptr);
        return true;
    }

    ArcrGlyphMetrics m;
    memset(&m, 0, sizeof(m));
    bool measured = false;

    if (codepoint <= 0xFFFFu) {
        HFONT hFont = select_gdi_font_locked(fs, codepoint, font_px, bold, monospace);
        if (hFont) {
            HGDIOBJ old = SelectObject(fs->mem_dc, hFont);
            MAT2 mat = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
            GLYPHMETRICS gm;
            ZeroMemory(&gm, sizeof(gm));
            DWORD res = GetGlyphOutlineW(fs->mem_dc, (UINT)codepoint, GGO_METRICS, &gm, 0, NULL, &mat);
            SelectObject(fs->mem_dc, old);
            if (res != GDI_ERROR) {
                m.advance_px = gm.gmCellIncX;
                m.bearing_x  = gm.gmptGlyphOrigin.x;
                m.bearing_y  = gm.gmptGlyphOrigin.y;
                m.width      = (int32_t)gm.gmBlackBoxX;
                m.height     = (int32_t)gm.gmBlackBoxY;
                measured     = true;
            }
        }
    }

    if (!measured) {
        win32_fallback_glyph(codepoint, font_px, bold, monospace, &m, NULL, 0u);
    }

    ArcrGlyphCacheSlot *slot = win32_alloc_slot_locked(fs, set_idx, key);
    slot->metrics    = m;
    slot->has_bitmap = false;
    *out_metrics     = m;

    LeaveCriticalSection(fs->cs_ptr);
    return true;
}

bool arcr_font_measure_glyph(ArcrFontSystem *fs, uint32_t codepoint,
                             int32_t font_px, bool bold,
                             ArcrGlyphMetrics *out_metrics) {
    return arcr_font_measure_glyph_ex(fs, codepoint, font_px, bold, false, out_metrics);
}

bool arcr_font_rasterize_glyph_ex(ArcrFontSystem *fs, uint32_t codepoint,
                                  int32_t font_px, bool bold, bool monospace,
                                  ArcrGlyphMetrics *out_metrics,
                                  uint8_t *out_alpha, size_t alpha_cap) {
    if (!out_metrics || !out_alpha) return false;
    if (font_px < 8) font_px = 8;
    if (font_px > 512) font_px = 512;

    if (!fs || !fs->mem_dc) {
        win32_fallback_glyph(codepoint, font_px, bold, monospace, out_metrics, out_alpha, alpha_cap);
        return true;
    }

    uint64_t key     = make_glyph_key(codepoint, font_px, bold, monospace);
    uint32_t set_idx = hash_glyph_key_set(key);

    EnterCriticalSection(fs->cs_ptr);

    ArcrGlyphCacheSlot *slot = win32_find_slot_locked(fs, set_idx, key);
    if (slot && slot->has_bitmap && fs->bitmap_pool) {
        size_t area = (size_t)slot->metrics.width * (size_t)slot->metrics.height;
        if (area > alpha_cap) {
            LeaveCriticalSection(fs->cs_ptr);
            return false;
        }
        fs->cache_hits++;
        slot->last_used = ++fs->access_clock;
        *out_metrics    = slot->metrics;
        if (area > 0u) {
            memcpy(out_alpha, fs->bitmap_pool + slot->bitmap_off, area);
        }
        LeaveCriticalSection(fs->cs_ptr);
        return true;
    }

    ArcrGlyphMetrics m;
    memset(&m, 0, sizeof(m));
    bool rendered = false;

    if (codepoint <= 0xFFFFu) {
        HFONT hFont = select_gdi_font_locked(fs, codepoint, font_px, bold, monospace);
        if (hFont) {
            HGDIOBJ old = SelectObject(fs->mem_dc, hFont);
            MAT2 mat = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
            GLYPHMETRICS gm;
            ZeroMemory(&gm, sizeof(gm));

            DWORD buf_sz = GetGlyphOutlineW(fs->mem_dc, (UINT)codepoint,
                                            GGO_GRAY8_BITMAP, &gm, 0, NULL, &mat);
            if (buf_sz != GDI_ERROR) {
                if (buf_sz == 0u) {
                    SelectObject(fs->mem_dc, old);
                    m.advance_px = gm.gmCellIncX;
                    m.bearing_x  = 0;
                    m.bearing_y  = 0;
                    m.width      = 0;
                    m.height     = 0;
                    rendered     = true;
                } else {
                    int32_t w = (int32_t)gm.gmBlackBoxX;
                    int32_t h = (int32_t)gm.gmBlackBoxY;
                    if ((size_t)w * (size_t)h > alpha_cap) {
                        SelectObject(fs->mem_dc, old);
                        LeaveCriticalSection(fs->cs_ptr);
                        return false;
                    }
                    uint8_t *tmp = (uint8_t *)malloc(buf_sz);
                    if (!tmp) {
                        SelectObject(fs->mem_dc, old);
                        LeaveCriticalSection(fs->cs_ptr);
                        return false;
                    }
                    if (GetGlyphOutlineW(fs->mem_dc, (UINT)codepoint,
                                         GGO_GRAY8_BITMAP, &gm, buf_sz, tmp, &mat) != GDI_ERROR) {
                        int32_t pitch = (w + 3) & ~3;
                        m.advance_px  = gm.gmCellIncX;
                        m.bearing_x   = gm.gmptGlyphOrigin.x;
                        m.bearing_y   = gm.gmptGlyphOrigin.y;
                        m.width       = w;
                        m.height      = h;
                        /* GGO_GRAY8_BITMAP produces coverage 0..64; scale cleanly to 0..255 */
                        for (int32_t r = 0; r < h; ++r) {
                            for (int32_t c = 0; c < w; ++c) {
                                uint32_t v = tmp[r * pitch + c];
                                out_alpha[(size_t)r * (size_t)w + (size_t)c] =
                                    (uint8_t)((v >= 64u) ? 255u : ((v * 255u + 32u) >> 6));
                            }
                        }
                        rendered = true;
                    }
                    free(tmp);
                }
            }
            SelectObject(fs->mem_dc, old);
        }
    }

    if (!rendered) {
        win32_fallback_glyph(codepoint, font_px, bold, monospace, &m, out_alpha, alpha_cap);
        if ((size_t)m.width * (size_t)m.height > alpha_cap) {
            LeaveCriticalSection(fs->cs_ptr);
            return false;
        }
    }

    size_t area = (size_t)m.width * (size_t)m.height;
    if (fs->bitmap_pool && area <= ARCR_GLYPH_MAX_CACHE_AREA) {
        if (fs->bitmap_pool_used + area > ARCR_GLYPH_POOL_BYTES) {
            win32_reset_cache_locked(fs, false);
            slot = NULL;
        }
        if (!slot) {
            slot = win32_alloc_slot_locked(fs, set_idx, key);
        } else {
            slot->last_used = ++fs->access_clock;
        }
        slot->metrics    = m;
        slot->bitmap_off = (uint32_t)fs->bitmap_pool_used;
        slot->has_bitmap = true;
        if (area > 0u) {
            memcpy(fs->bitmap_pool + fs->bitmap_pool_used, out_alpha, area);
            fs->bitmap_pool_used += area;
        }
    }

    *out_metrics = m;
    LeaveCriticalSection(fs->cs_ptr);
    return true;
}

bool arcr_font_rasterize_glyph(ArcrFontSystem *fs, uint32_t codepoint,
                               int32_t font_px, bool bold,
                               ArcrGlyphMetrics *out_metrics,
                               uint8_t *out_alpha, size_t alpha_cap) {
    return arcr_font_rasterize_glyph_ex(fs, codepoint, font_px, bold, false,
                                        out_metrics, out_alpha, alpha_cap);
}

/* -------------------------------------------------------------------------
 * 5. True PNG Framebuffer Export (Pure C11 arcr_codec RFC 2083 PNG Writer)
 * ------------------------------------------------------------------------- */
bool arcr_platform_write_png(const char *filepath, const uint32_t *argb_pixels,
                             int32_t width, int32_t height, int32_t stride_pixels) {
    if (!filepath || !argb_pixels || width <= 0 || height <= 0) return false;
    if (stride_pixels < width) stride_pixels = width;
    return arcr_image_write_png_file(filepath, argb_pixels, width, height, stride_pixels);
}

#else
/* Satisfy ISO C11 non-empty translation unit rule under -Wpedantic when compiled on POSIX */
typedef int arcr_win32_platform_unused_tu;
#endif /* _WIN32 */
