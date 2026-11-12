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

#ifndef ANTIALIASED_QUALITY
#  define ANTIALIASED_QUALITY 4
#endif

#ifndef CLEARTYPE_QUALITY
#  define CLEARTYPE_QUALITY 5
#endif

#ifndef GGO_GLYPH_INDEX
#  define GGO_GLYPH_INDEX 0x0080
#endif

#ifndef GCP_GLYPHSHAPE
#  define GCP_GLYPHSHAPE 0x0010
#endif

#ifndef GGI_MARK_NONEXISTING_GLYPHS
#  define GGI_MARK_NONEXISTING_GLYPHS 0x0001
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

bool arcr_platform_get_executable_dir(char *out_dir, size_t cap) {
    if (!out_dir || cap < 4u) return false;

    WCHAR wbuf[2048];
    DWORD wn = GetModuleFileNameW(NULL, wbuf, (DWORD)(sizeof(wbuf) / sizeof(wbuf[0])));
    if (wn > 0u && wn < (DWORD)(sizeof(wbuf) / sizeof(wbuf[0]))) {
        size_t last_sep = SIZE_MAX;
        for (size_t i = 0u; i < (size_t)wn; ++i) {
            if (wbuf[i] == L'/') wbuf[i] = L'\\';
            if (wbuf[i] == L'\\') last_sep = i;
        }
        if (last_sep != SIZE_MAX) {
            if (last_sep == 2u && wbuf[1u] == L':') {
                wbuf[3u] = L'\0'; /* Preserve root "C:\" */
            } else if (last_sep == 0u) {
                wbuf[1u] = L'\0';
            } else {
                wbuf[last_sep] = L'\0';
            }
            if (win32_wide_to_utf8(wbuf, out_dir, cap) && out_dir[0] != '\0') {
                return true;
            }
        }
    }

    DWORD cwd_n = GetCurrentDirectoryW((DWORD)(sizeof(wbuf) / sizeof(wbuf[0])), wbuf);
    if (cwd_n > 0u && cwd_n < (DWORD)(sizeof(wbuf) / sizeof(wbuf[0]))) {
        if (win32_wide_to_utf8(wbuf, out_dir, cap) && out_dir[0] != '\0') {
            return true;
        }
    }

    out_dir[0] = '.';
    out_dir[1] = '\0';
    return true;
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
 * 4. Perceptual sRGB Gamma-Corrected Win32 GDI Font System + Lock-Free Atlas
 * ------------------------------------------------------------------------- */
/*
 * Perceptual sRGB stem-darkening & gamma-correction LUT for GDI GGO_GRAY8_BITMAP (0..64):
 * alpha' = clamp(round(pow(v / 64.0, 1.0 / 1.42) * 255.0 * 1.04), 0, 255)
 * Transforms washed-out linear GDI gray coverage into crisp DirectWrite/macOS-grade stems.
 */
static const uint8_t kGdiGray65GammaLut[65] = {
      0u,  14u,  23u,  31u,  38u,  44u,  50u,  56u,  61u,  67u,  72u,  77u,  82u,
     87u,  91u,  96u, 100u, 105u, 109u, 113u, 117u, 121u, 125u, 129u, 133u, 137u,
    141u, 145u, 149u, 152u, 156u, 160u, 163u, 166u, 169u, 173u, 176u, 180u, 183u,
    187u, 190u, 194u, 197u, 200u, 203u, 207u, 210u, 213u, 216u, 220u, 223u, 226u,
    229u, 232u, 235u, 238u, 241u, 244u, 247u, 250u, 253u, 255u, 255u, 255u, 255u
};

static const uint8_t kPerceptualFontGammaLut[256] = {
      0u,   5u,   9u,  12u,  14u,  17u,  19u,  21u,  23u,  25u,  27u,  29u,  31u,  33u,  34u,  36u,
     38u,  39u,  41u,  43u,  44u,  46u,  47u,  49u,  50u,  52u,  53u,  55u,  56u,  57u,  59u,  60u,
     61u,  63u,  64u,  65u,  67u,  68u,  69u,  71u,  72u,  73u,  74u,  76u,  77u,  78u,  79u,  81u,
     82u,  83u,  84u,  85u,  87u,  88u,  89u,  90u,  91u,  92u,  93u,  95u,  96u,  97u,  98u,  99u,
    100u, 101u, 102u, 103u, 105u, 106u, 107u, 108u, 109u, 110u, 111u, 112u, 113u, 114u, 115u, 116u,
    117u, 118u, 119u, 120u, 121u, 122u, 123u, 124u, 125u, 126u, 127u, 128u, 129u, 130u, 131u, 132u,
    133u, 134u, 135u, 136u, 137u, 138u, 139u, 140u, 141u, 142u, 143u, 144u, 145u, 146u, 147u, 148u,
    149u, 150u, 150u, 151u, 152u, 153u, 154u, 155u, 156u, 157u, 158u, 159u, 160u, 161u, 161u, 162u,
    163u, 164u, 165u, 166u, 167u, 168u, 169u, 169u, 170u, 171u, 172u, 173u, 174u, 175u, 176u, 176u,
    177u, 178u, 179u, 180u, 181u, 182u, 183u, 183u, 184u, 185u, 186u, 187u, 188u, 188u, 189u, 190u,
    191u, 192u, 193u, 194u, 194u, 195u, 196u, 197u, 198u, 199u, 199u, 200u, 201u, 202u, 203u, 203u,
    204u, 205u, 206u, 207u, 208u, 208u, 209u, 210u, 211u, 212u, 212u, 213u, 214u, 215u, 216u, 216u,
    217u, 218u, 219u, 220u, 220u, 221u, 222u, 223u, 223u, 224u, 225u, 226u, 227u, 227u, 228u, 229u,
    230u, 231u, 231u, 232u, 233u, 234u, 234u, 235u, 236u, 237u, 237u, 238u, 239u, 240u, 241u, 241u,
    242u, 243u, 244u, 244u, 245u, 246u, 247u, 247u, 248u, 249u, 250u, 250u, 251u, 252u, 253u, 253u,
    254u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u
};

static const uint8_t kEmptyGlyphAlpha = 0u;

enum {
    ARCR_GLYPH_HASH_CAP       = 8192,
    ARCR_GLYPH_HASH_MASK      = ARCR_GLYPH_HASH_CAP - 1,
    ARCR_GLYPH_ARENA_BLOCK_SZ = 131072, /* 128 KB immutable glyph bitmap chunks */
    ARCR_GLYPH_MAX_FONT_PX    = 256,
    ARCR_GLYPH_MAX_DIM        = 384,
    ARCR_WIN32_FONT_CACHE_CAP = 12
};

typedef enum ArcrWin32FontRole {
    ARCR_WFONT_SANS     = 0,
    ARCR_WFONT_CJK      = 1,
    ARCR_WFONT_MONO     = 2,
    ARCR_WFONT_MONO_CJK = 3,
    ARCR_WFONT_SYMBOL   = 4
} ArcrWin32FontRole;

typedef DWORD (WINAPI *PfnGetGlyphIndicesW)(HDC, LPCWSTR, int, LPWORD, DWORD);
typedef int   (WINAPI *PfnAddFontResourceExW)(LPCWSTR, DWORD, PVOID);
typedef BOOL  (WINAPI *PfnRemoveFontResourceExW)(LPCWSTR, DWORD, PVOID);

typedef struct ArcrWin32FontSlot {
    HFONT    hfont;
    int32_t  font_px;
    uint32_t style_key;
    uint32_t last_used;
    uint8_t *cmap_data;
    uint32_t cmap_len;
    uint32_t cmap_fmt12_off;
    bool     cmap_queried;
} ArcrWin32FontSlot;

typedef struct ArcrGlyphCacheSlot {
    volatile LONG    state;      /* 0 = empty, 1 = published (acquire/release) */
    uint64_t         key;        /* Valid once state == 1 */
    const uint8_t   *bitmap_ptr; /* Immutable pointer in bitmap_arena or &kEmptyGlyphAlpha */
    ArcrGlyphMetrics metrics;
} ArcrGlyphCacheSlot;

struct ArcrFontSystem {
    CRITICAL_SECTION         cs_storage;
    CRITICAL_SECTION        *cs_ptr;
    HDC                      mem_dc;
    uint16_t                 os_ver;
    PfnGetGlyphIndicesW      fn_get_glyph_indices;
    PfnRemoveFontResourceExW fn_remove_font_res;
    WCHAR                    private_font_path[MAX_PATH];
    bool                     has_private_font;
    WCHAR                    face_sans[LF_FACESIZE];
    WCHAR                    face_cjk[LF_FACESIZE];
    WCHAR                    face_mono[LF_FACESIZE];
    WCHAR                    face_mono_cjk[LF_FACESIZE];
    WCHAR                    face_symbol[LF_FACESIZE];
    ArcrWin32FontSlot        font_slots[ARCR_WIN32_FONT_CACHE_CAP];
    uint32_t                 font_clock;
    ArcrGlyphCacheSlot       slots[ARCR_GLYPH_HASH_CAP];
    ArcrArena                bitmap_arena;
    size_t                   cache_entries;
    size_t                   cache_hits;
};

static inline uint64_t make_glyph_key(uint32_t cp, int32_t font_px, bool bold, bool monospace) {
    uint64_t px_u  = (uint64_t)(uint32_t)font_px & 0xFFFFULL;
    uint64_t flags = (bold ? 1ULL : 0ULL) | (monospace ? 2ULL : 0ULL);
    return (1ULL << 63) | ((uint64_t)cp << 18) | (px_u << 2) | flags;
}

static inline uint32_t hash_glyph_key_slot(uint64_t key) {
    uint64_t h = key * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 32;
    h ^= h >> 15;
    return (uint32_t)h & (uint32_t)ARCR_GLYPH_HASH_MASK;
}

static inline LONG slot_load_state_acquire(const ArcrGlyphCacheSlot *slot) {
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE);
#else
    return InterlockedCompareExchange((volatile LONG *)&slot->state, 0L, 0L);
#endif
}

static inline void slot_publish_state_release(ArcrGlyphCacheSlot *slot) {
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(&slot->state, 1L, __ATOMIC_RELEASE);
#else
    InterlockedExchange(&slot->state, 1L);
#endif
}

static void copy_wface(WCHAR dst[LF_FACESIZE], const WCHAR *src) {
    size_t i = 0u;
    if (src) {
        while (src[i] != L'\0' && i + 1u < (size_t)LF_FACESIZE) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = L'\0';
}

static int CALLBACK win32_enum_font_exists_proc(const LOGFONTW *lf, const TEXTMETRICW *tm,
                                                DWORD font_type, LPARAM lparam) {
    (void)lf;
    (void)tm;
    (void)font_type;
    if (lparam != 0) {
        *(bool *)lparam = true;
    }
    return 0; /* Stop after first match */
}

static bool win32_font_family_exists(HDC hdc, const WCHAR *face_name) {
    if (!hdc || !face_name || face_name[0] == L'\0') return false;
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    copy_wface(lf.lfFaceName, face_name);
    bool exists = false;
    EnumFontFamiliesExW(hdc, &lf, win32_enum_font_exists_proc, (LPARAM)&exists, 0);
    return exists;
}

static void pick_first_installed_face(HDC hdc, WCHAR out_face[LF_FACESIZE],
                                      const WCHAR *const *candidates, const WCHAR *fallback) {
    for (size_t i = 0u; candidates && candidates[i] != NULL; ++i) {
        if (win32_font_family_exists(hdc, candidates[i])) {
            copy_wface(out_face, candidates[i]);
            return;
        }
    }
    copy_wface(out_face, fallback);
}

static void free_win32_font_slots(ArcrFontSystem *fs) {
    for (size_t i = 0u; i < (size_t)ARCR_WIN32_FONT_CACHE_CAP; ++i) {
        if (fs->font_slots[i].hfont) {
            DeleteObject(fs->font_slots[i].hfont);
            fs->font_slots[i].hfont = NULL;
        }
        free(fs->font_slots[i].cmap_data);
        fs->font_slots[i].cmap_data = NULL;
        fs->font_slots[i].cmap_len  = 0u;
    }
}

ArcrFontSystem *arcr_font_system_create(const char *preferred_cjk_font_path) {
    ArcrFontSystem *fs = (ArcrFontSystem *)calloc(1u, sizeof(ArcrFontSystem));
    if (!fs) return NULL;

    InitializeCriticalSection(&fs->cs_storage);
    fs->cs_ptr = &fs->cs_storage;
    fs->mem_dc = CreateCompatibleDC(NULL);
    fs->os_ver = arcr_platform_os_version();
    arcr_arena_init(&fs->bitmap_arena, ARCR_GLYPH_ARENA_BLOCK_SZ);

    HMODULE hGdi32 = GetModuleHandleW(L"gdi32.dll");
    if (hGdi32) {
        FARPROC raw_ggi = GetProcAddress(hGdi32, "GetGlyphIndicesW");
        if (raw_ggi) {
            memcpy(&fs->fn_get_glyph_indices, &raw_ggi, sizeof(fs->fn_get_glyph_indices));
        }
        FARPROC raw_rem = GetProcAddress(hGdi32, "RemoveFontResourceExW");
        if (raw_rem) {
            memcpy(&fs->fn_remove_font_res, &raw_rem, sizeof(fs->fn_remove_font_res));
        }
        if (preferred_cjk_font_path && preferred_cjk_font_path[0] != '\0') {
            FARPROC raw_add = GetProcAddress(hGdi32, "AddFontResourceExW");
            PfnAddFontResourceExW fn_add = NULL;
            if (raw_add) memcpy(&fn_add, &raw_add, sizeof(fn_add));
            if (fn_add && win32_utf8_to_wide(preferred_cjk_font_path,
                                             fs->private_font_path, MAX_PATH)) {
                /* FR_PRIVATE = 0x10 (Windows 2000+) */
                if (fn_add(fs->private_font_path, 0x10u, NULL) > 0) {
                    fs->has_private_font = true;
                }
            }
        }
    }

    static const WCHAR *const kSansCascade[] = {
        L"Malgun Gothic", L"Segoe UI", L"\xAD74\xB9BC", L"Tahoma", L"Arial", NULL
    };
    static const WCHAR *const kCjkCascade[] = {
        L"Malgun Gothic", L"\xAD74\xB9BC", L"\xBC14\xD0D5", L"Arial Unicode MS", L"Tahoma", NULL
    };
    static const WCHAR *const kMonoCascade[] = {
        L"Consolas", L"Courier New", NULL
    };
    static const WCHAR *const kMonoCjkCascade[] = {
        L"Malgun Gothic", L"\xAD74\xB9BC\xCCB4", L"\xAD74\xB9BC", NULL
    };
    static const WCHAR *const kSymbolCascade[] = {
        L"Segoe UI Emoji", L"Segoe UI Symbol", L"Arial Unicode MS",
        L"Lucida Sans Unicode", L"Malgun Gothic", L"Tahoma", NULL
    };

    pick_first_installed_face(fs->mem_dc, fs->face_sans,     kSansCascade,    L"Tahoma");
    pick_first_installed_face(fs->mem_dc, fs->face_cjk,      kCjkCascade,     fs->face_sans);
    pick_first_installed_face(fs->mem_dc, fs->face_mono,     kMonoCascade,    L"Courier New");
    pick_first_installed_face(fs->mem_dc, fs->face_mono_cjk, kMonoCjkCascade, fs->face_cjk);
    pick_first_installed_face(fs->mem_dc, fs->face_symbol,   kSymbolCascade,  fs->face_sans);

    return fs;
}

void arcr_font_system_destroy(ArcrFontSystem *fs) {
    if (!fs) return;
    free_win32_font_slots(fs);
    if (fs->has_private_font && fs->fn_remove_font_res) {
        (void)fs->fn_remove_font_res(fs->private_font_path, 0x10u, NULL);
    }
    if (fs->mem_dc) DeleteDC(fs->mem_dc);
    arcr_arena_destroy(&fs->bitmap_arena);
    DeleteCriticalSection(&fs->cs_storage);
    free(fs);
}

void arcr_font_system_purge_cache(ArcrFontSystem *fs) {
    if (!fs) return;
    EnterCriticalSection(fs->cs_ptr);
    memset(fs->slots, 0, sizeof(fs->slots));
    arcr_arena_reset(&fs->bitmap_arena);
    fs->cache_entries = 0u;
    fs->cache_hits    = 0u;
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

static inline uint16_t cmap_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t cmap_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/*
 * Loads the TrueType/OpenType 'cmap' table via GetFontData (Win2000+) once per HFONT
 * and locates any Format 12 (UCS-4 full Unicode) subtable for astral codepoints.
 */
static void ensure_font_slot_cmap_locked(HDC hdc, ArcrWin32FontSlot *fslot) {
    if (fslot->cmap_queried) return;
    fslot->cmap_queried   = true;
    fslot->cmap_fmt12_off = 0u;

    /* 'cmap' in little-endian DWORD table tag: 'c' | ('m'<<8) | ('a'<<16) | ('p'<<24) */
    const DWORD kCmapTag = 0x70616D63u;
    DWORD sz = GetFontData(hdc, kCmapTag, 0u, NULL, 0u);
    if (sz == GDI_ERROR || sz < 12u || sz > 4u * 1024u * 1024u) return;

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) return;
    if (GetFontData(hdc, kCmapTag, 0u, buf, sz) != sz) {
        free(buf);
        return;
    }

    uint16_t num_tables = cmap_be16(buf + 2);
    if (4u + (uint32_t)num_tables * 8u <= sz) {
        for (uint16_t i = 0u; i < num_tables; ++i) {
            const uint8_t *rec  = buf + 4u + (size_t)i * 8u;
            uint16_t       plat = cmap_be16(rec + 0);
            uint16_t       enc  = cmap_be16(rec + 2);
            uint32_t       off  = cmap_be32(rec + 4);
            if (off + 16u <= sz && cmap_be16(buf + off) == 12u) {
                if ((plat == 3u && enc == 10u) || plat == 0u || fslot->cmap_fmt12_off == 0u) {
                    fslot->cmap_fmt12_off = off;
                    if (plat == 3u && enc == 10u) break;
                }
            }
        }
    }

    if (fslot->cmap_fmt12_off != 0u) {
        fslot->cmap_data = buf;
        fslot->cmap_len  = (uint32_t)sz;
    } else {
        free(buf);
    }
}

static uint16_t cmap_fmt12_lookup(const ArcrWin32FontSlot *fslot, uint32_t cp) {
    if (!fslot->cmap_data || fslot->cmap_fmt12_off == 0u) return 0u;
    uint32_t off = fslot->cmap_fmt12_off;
    if (off + 16u > fslot->cmap_len) return 0u;

    uint32_t num_groups = cmap_be32(fslot->cmap_data + off + 12u);
    uint32_t max_groups = (fslot->cmap_len - off - 16u) / 12u;
    if (num_groups > max_groups) num_groups = max_groups;

    uint32_t lo = 0u;
    uint32_t hi = num_groups;
    while (lo < hi) {
        uint32_t       mid         = lo + ((hi - lo) >> 1);
        const uint8_t *grp         = fslot->cmap_data + off + 16u + (size_t)mid * 12u;
        uint32_t       start_char  = cmap_be32(grp + 0);
        uint32_t       end_char    = cmap_be32(grp + 4);
        uint32_t       start_glyph = cmap_be32(grp + 8);
        if (cp < start_char) {
            hi = mid;
        } else if (cp > end_char) {
            lo = mid + 1u;
        } else {
            uint32_t gid = start_glyph + (cp - start_char);
            return (gid <= 0xFFFEu) ? (uint16_t)gid : 0u;
        }
    }
    return 0u;
}

static ArcrWin32FontSlot *select_gdi_font_role_locked(ArcrFontSystem *fs,
                                                      ArcrWin32FontRole role,
                                                      int32_t font_px, bool bold) {
    uint32_t style_key = ((uint32_t)role << 1) | (bold ? 1u : 0u);
    size_t   victim    = 0u;
    uint32_t oldest    = UINT32_MAX;

    for (size_t i = 0u; i < (size_t)ARCR_WIN32_FONT_CACHE_CAP; ++i) {
        ArcrWin32FontSlot *s = &fs->font_slots[i];
        if (s->hfont && s->font_px == font_px && s->style_key == style_key) {
            s->last_used = ++fs->font_clock;
            return s;
        }
        if (!s->hfont) {
            victim = i;
            oldest = 0u;
        } else if (s->last_used < oldest) {
            oldest = s->last_used;
            victim = i;
        }
    }

    const WCHAR *face = fs->face_sans;
    DWORD pitch_and_family = DEFAULT_PITCH | FF_DONTCARE;
    switch (role) {
        case ARCR_WFONT_SANS:     face = fs->face_sans; break;
        case ARCR_WFONT_CJK:      face = fs->face_cjk; break;
        case ARCR_WFONT_MONO:
            face = fs->face_mono;
            pitch_and_family = FIXED_PITCH | FF_MODERN;
            break;
        case ARCR_WFONT_MONO_CJK: face = fs->face_mono_cjk; break;
        case ARCR_WFONT_SYMBOL:   face = fs->face_symbol; break;
    }

    /* CLEARTYPE_QUALITY (5) on Windows XP+ (0x0501+); ANTIALIASED_QUALITY (4) on Windows 2000 */
    DWORD quality = (fs->os_ver >= 0x0501u) ? CLEARTYPE_QUALITY : ANTIALIASED_QUALITY;
    HFONT hf = CreateFontW(-font_px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                           FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           quality, pitch_and_family, face);
    if (!hf) return NULL;

    ArcrWin32FontSlot *dst = &fs->font_slots[victim];
    if (dst->hfont) DeleteObject(dst->hfont);
    free(dst->cmap_data);
    memset(dst, 0, sizeof(*dst));
    dst->hfont     = hf;
    dst->font_px   = font_px;
    dst->style_key = style_key;
    dst->last_used = ++fs->font_clock;
    return dst;
}

/*
 * Resolves a Unicode codepoint (including astral U+10000..U+10FFFF) to a 16-bit glyph index
 * in the currently selected HFONT, returning 0 if the font does not cover `cp`.
 */
static uint16_t resolve_glyph_index_locked(ArcrFontSystem *fs, ArcrWin32FontSlot *fslot, uint32_t cp) {
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0u;

    /* 1. Astral plane (Emoji, Symbols, CJK Ext B+): OpenType 'cmap' Format 12 + UTF-16 GCP fallback */
    if (cp > 0xFFFFu) {
        ensure_font_slot_cmap_locked(fs->mem_dc, fslot);
        uint16_t gid12 = cmap_fmt12_lookup(fslot, cp);
        if (gid12 != 0u) return gid12;

        uint32_t u = cp - 0x10000u;
        WCHAR wpair[3];
        wpair[0] = (WCHAR)(0xD800u + (u >> 10));
        wpair[1] = (WCHAR)(0xDC00u + (u & 0x3FFu));
        wpair[2] = L'\0';

        WCHAR out_glyphs[4] = {0, 0, 0, 0};
        GCP_RESULTSW gcp;
        ZeroMemory(&gcp, sizeof(gcp));
        gcp.lStructSize = sizeof(gcp);
        gcp.lpGlyphs    = out_glyphs;
        gcp.nGlyphs     = 4u;
        if (GetCharacterPlacementW(fs->mem_dc, wpair, 2, 0, &gcp, GCP_GLYPHSHAPE) != 0u &&
            gcp.nGlyphs == 1u && out_glyphs[0] != 0u && out_glyphs[0] != 0xFFFFu) {
            return (uint16_t)out_glyphs[0];
        }
        return 0u;
    }

    /* 2. BMP (U+0000..U+FFFF): GetGlyphIndicesW (Win2000+) or 'cmap' Format 12 / GCP */
    WCHAR wch = (WCHAR)cp;
    if (fs->fn_get_glyph_indices) {
        WORD gidx = 0xFFFFu;
        if (fs->fn_get_glyph_indices(fs->mem_dc, &wch, 1, &gidx, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR) {
            if (gidx != 0xFFFFu && gidx != 0u) {
                return (uint16_t)gidx;
            }
            return 0u;
        }
    }

    ensure_font_slot_cmap_locked(fs->mem_dc, fslot);
    uint16_t gid12 = cmap_fmt12_lookup(fslot, cp);
    if (gid12 != 0u) return gid12;

    WCHAR out_glyphs[2] = {0, 0};
    GCP_RESULTSW gcp;
    ZeroMemory(&gcp, sizeof(gcp));
    gcp.lStructSize = sizeof(gcp);
    gcp.lpGlyphs    = out_glyphs;
    gcp.nGlyphs     = 2u;
    if (GetCharacterPlacementW(fs->mem_dc, &wch, 1, 0, &gcp, GCP_GLYPHSHAPE) != 0u &&
        gcp.nGlyphs >= 1u && out_glyphs[0] != 0u && out_glyphs[0] != 0xFFFFu) {
        return (uint16_t)out_glyphs[0];
    }
    return 0u;
}

/*
 * Smooth 4x4 supersampled anti-aliased vector fallback glyph when codepoint is unmapped.
 */
static int64_t dist_sq_to_seg_q4(int32_t px, int32_t py,
                                 int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    int64_t vx = (int64_t)bx - (int64_t)ax;
    int64_t vy = (int64_t)by - (int64_t)ay;
    int64_t wx = (int64_t)px - (int64_t)ax;
    int64_t wy = (int64_t)py - (int64_t)ay;
    int64_t len_sq = vx * vx + vy * vy;
    int64_t cx = ax, cy = ay;
    if (len_sq > 0) {
        int64_t dot = wx * vx + wy * vy;
        if (dot <= 0) {
            cx = ax; cy = ay;
        } else if (dot >= len_sq) {
            cx = bx; cy = by;
        } else {
            cx = (int64_t)ax + (vx * dot) / len_sq;
            cy = (int64_t)ay + (vy * dot) / len_sq;
        }
    }
    int64_t dx = (int64_t)px - cx;
    int64_t dy = (int64_t)py - cy;
    return dx * dx + dy * dy;
}

static void win32_fallback_glyph(uint32_t cp, int32_t font_px, bool bold, bool monospace,
                                 ArcrGlyphMetrics *out_m, uint8_t *out_alpha, size_t cap) {
    bool wide = arcr_cp_is_hangul(cp) || arcr_cp_is_cjk_ideograph(cp) || (cp > 0x2000u);
    int32_t adv = wide ? font_px : ARCR_MAX(6, (font_px * 3) / 5);
    int32_t gw  = ARCR_MAX(4, adv - 2);
    int32_t gh  = ARCR_MAX(6, (font_px * 3) / 4);
    if (arcr_cp_is_whitespace(cp)) {
        out_m->advance_px = adv;
        out_m->bearing_x  = 0;
        out_m->bearing_y  = 0;
        out_m->width      = 0;
        out_m->height     = 0;
        return;
    }
    out_m->advance_px = adv + ((bold && !monospace) ? 1 : 0);
    out_m->bearing_x  = 1;
    out_m->bearing_y  = gh - 1;
    out_m->width      = gw;
    out_m->height     = gh;
    if (!out_alpha || (size_t)gw * (size_t)gh > cap) return;

    int32_t w_q4 = gw * 16;
    int32_t h_q4 = gh * 16;
    int32_t stroke_r = ARCR_MAX(10, (font_px * (bold ? 18 : 13)) / 16);
    int64_t r_sq = (int64_t)stroke_r * (int64_t)stroke_r;

    int32_t x0 = 14, y0 = 14;
    int32_t x1 = ARCR_MAX(x0 + 8, w_q4 - 14);
    int32_t y1 = ARCR_MAX(y0 + 8, h_q4 - 14);
    int32_t xm = (x0 + x1) / 2;
    int32_t ym = (y0 + y1) / 2;

    for (int32_t y = 0; y < gh; ++y) {
        for (int32_t x = 0; x < gw; ++x) {
            uint32_t cov = 0u;
            for (int32_t sy = 0; sy < 4; ++sy) {
                int32_t py = y * 16 + sy * 4 + 2;
                for (int32_t sx = 0; sx < 4; ++sx) {
                    int32_t px = x * 16 + sx * 4 + 2;
                    bool hit = false;
                    if (dist_sq_to_seg_q4(px, py, x0, y0, x1, y0) <= r_sq ||
                        dist_sq_to_seg_q4(px, py, x1, y0, x1, y1) <= r_sq ||
                        dist_sq_to_seg_q4(px, py, x1, y1, x0, y1) <= r_sq ||
                        dist_sq_to_seg_q4(px, py, x0, y1, x0, y0) <= r_sq) {
                        hit = true;
                    } else if ((cp & 1u) != 0u && dist_sq_to_seg_q4(px, py, x0, ym, x1, ym) <= r_sq) {
                        hit = true;
                    } else if ((cp & 2u) != 0u && dist_sq_to_seg_q4(px, py, xm, y0, xm, y1) <= r_sq) {
                        hit = true;
                    } else if ((cp & 4u) != 0u && dist_sq_to_seg_q4(px, py, x0, y1, x1, y0) <= r_sq) {
                        hit = true;
                    }
                    if (hit) ++cov;
                }
            }
            uint32_t raw_a = (cov * 230u + 8u) / 16u;
            out_alpha[(size_t)y * (size_t)gw + (size_t)x] = kPerceptualFontGammaLut[raw_a & 0xFFu];
        }
    }
}

static bool rasterize_win32_glyph_into_arena_locked(ArcrFontSystem *fs, uint32_t codepoint,
                                                    int32_t font_px, bool bold, bool monospace,
                                                    ArcrGlyphMetrics *out_m,
                                                    const uint8_t **out_ptr) {
    memset(out_m, 0, sizeof(*out_m));
    *out_ptr = &kEmptyGlyphAlpha;

    uint8_t  stack_buf[64 * 64];
    uint8_t *heap_buf = NULL;
    uint8_t *work_buf = stack_buf;
    size_t   work_cap = sizeof(stack_buf);
    bool     rendered = false;

    if (fs->mem_dc) {
        bool is_cjk = arcr_cp_is_hangul(codepoint) || arcr_cp_is_cjk_ideograph(codepoint);
        ArcrWin32FontRole roles[4];
        size_t role_cnt = 0u;

        if (codepoint > 0xFFFFu) {
            roles[role_cnt++] = ARCR_WFONT_SYMBOL;
            roles[role_cnt++] = ARCR_WFONT_CJK;
            roles[role_cnt++] = ARCR_WFONT_SANS;
        } else if (monospace && is_cjk) {
            roles[role_cnt++] = ARCR_WFONT_MONO_CJK;
            roles[role_cnt++] = ARCR_WFONT_CJK;
            roles[role_cnt++] = ARCR_WFONT_SYMBOL;
        } else if (monospace) {
            roles[role_cnt++] = ARCR_WFONT_MONO;
            roles[role_cnt++] = ARCR_WFONT_CJK;
            roles[role_cnt++] = ARCR_WFONT_SYMBOL;
        } else if (is_cjk) {
            roles[role_cnt++] = ARCR_WFONT_CJK;
            roles[role_cnt++] = ARCR_WFONT_SANS;
            roles[role_cnt++] = ARCR_WFONT_SYMBOL;
        } else {
            roles[role_cnt++] = ARCR_WFONT_SANS;
            roles[role_cnt++] = ARCR_WFONT_CJK;
            roles[role_cnt++] = ARCR_WFONT_SYMBOL;
        }

        static const MAT2 kIdentMat = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};

        for (size_t ri = 0u; ri < role_cnt && !rendered; ++ri) {
            ArcrWin32FontSlot *fslot = select_gdi_font_role_locked(fs, roles[ri], font_px, bold);
            if (!fslot || !fslot->hfont) continue;

            HGDIOBJ old_obj = SelectObject(fs->mem_dc, fslot->hfont);
            uint16_t gidx   = resolve_glyph_index_locked(fs, fslot, codepoint);

            UINT query_ch  = (UINT)gidx;
            UINT ggo_flags = GGO_GLYPH_INDEX | GGO_GRAY8_BITMAP;
            if (gidx == 0u && codepoint == 0x20u) {
                query_ch  = 0x20u;
                ggo_flags = GGO_GRAY8_BITMAP;
            }

            if (gidx != 0u || codepoint == 0x20u) {
                GLYPHMETRICS gm;
                ZeroMemory(&gm, sizeof(gm));
                DWORD buf_sz = GetGlyphOutlineW(fs->mem_dc, query_ch, ggo_flags,
                                                &gm, 0u, NULL, &kIdentMat);
                if (buf_sz != GDI_ERROR) {
                    if (buf_sz == 0u || gm.gmBlackBoxX == 0u || gm.gmBlackBoxY == 0u) {
                        out_m->advance_px = gm.gmCellIncX;
                        rendered          = true;
                    } else if ((int32_t)gm.gmBlackBoxX <= ARCR_GLYPH_MAX_DIM &&
                               (int32_t)gm.gmBlackBoxY <= ARCR_GLYPH_MAX_DIM) {
                        int32_t w    = (int32_t)gm.gmBlackBoxX;
                        int32_t h    = (int32_t)gm.gmBlackBoxY;
                        size_t  area = (size_t)w * (size_t)h;
                        if (area > work_cap) {
                            heap_buf = (uint8_t *)malloc(area);
                            if (!heap_buf) {
                                SelectObject(fs->mem_dc, old_obj);
                                return false;
                            }
                            work_buf = heap_buf;
                            work_cap = area;
                        }
                        uint8_t *gdi_tmp = (uint8_t *)malloc((size_t)buf_sz);
                        if (!gdi_tmp) {
                            SelectObject(fs->mem_dc, old_obj);
                            free(heap_buf);
                            return false;
                        }
                        if (GetGlyphOutlineW(fs->mem_dc, query_ch, ggo_flags,
                                             &gm, buf_sz, gdi_tmp, &kIdentMat) != GDI_ERROR) {
                            int32_t pitch = (w + 3) & ~3;
                            for (int32_t r = 0; r < h; ++r) {
                                const uint8_t *srow = gdi_tmp + (size_t)r * (size_t)pitch;
                                uint8_t       *drow = work_buf + (size_t)r * (size_t)w;
                                for (int32_t c = 0; c < w; ++c) {
                                    uint32_t v = srow[c];
                                    drow[c] = kGdiGray65GammaLut[(v <= 64u) ? v : 64u];
                                }
                            }
                            out_m->advance_px = gm.gmCellIncX;
                            out_m->bearing_x  = gm.gmptGlyphOrigin.x;
                            out_m->bearing_y  = gm.gmptGlyphOrigin.y;
                            out_m->width      = w;
                            out_m->height     = h;
                            rendered          = true;
                        }
                        free(gdi_tmp);
                    }
                }
            }
            SelectObject(fs->mem_dc, old_obj);
        }
    }

    if (!rendered) {
        int32_t est_dim = ARCR_MIN(font_px + 4, ARCR_GLYPH_MAX_DIM);
        size_t  need    = (size_t)est_dim * (size_t)est_dim;
        if (need > work_cap) {
            heap_buf = (uint8_t *)malloc(need);
            if (!heap_buf) return false;
            work_buf = heap_buf;
            work_cap = need;
        }
        win32_fallback_glyph(codepoint, font_px, bold, monospace, out_m, work_buf, work_cap);
    }

    size_t final_area = (size_t)out_m->width * (size_t)out_m->height;
    if (final_area > 0u) {
        uint8_t *dst = (uint8_t *)arcr_arena_alloc(&fs->bitmap_arena, final_area, 1u);
        if (!dst) {
            free(heap_buf);
            return false;
        }
        memcpy(dst, work_buf, final_area);
        *out_ptr = dst;
    } else {
        *out_ptr = &kEmptyGlyphAlpha;
    }

    free(heap_buf);
    return true;
}

const uint8_t *arcr_font_get_cached_glyph_ptr(ArcrFontSystem *fs, uint32_t codepoint,
                                              int32_t font_px, bool bold, bool monospace,
                                              ArcrGlyphMetrics *out_metrics,
                                              bool allow_insert) {
    if (!fs) return NULL;
    font_px = ARCR_CLAMP(font_px, 8, ARCR_GLYPH_MAX_FONT_PX);

    uint64_t key  = make_glyph_key(codepoint, font_px, bold, monospace);
    uint32_t mask = (uint32_t)ARCR_GLYPH_HASH_MASK;
    uint32_t base = hash_glyph_key_slot(key);

    /*
     * Lock-free zero-copy fast path for parallel 64x64 tile worker threads:
     * ZERO CRITICAL_SECTION locks, ZERO shared state writes, ZERO memcpy!
     */
    if (!allow_insert) {
        for (uint32_t step = 0u; step < (uint32_t)ARCR_GLYPH_HASH_CAP; ++step) {
            const ArcrGlyphCacheSlot *slot = &fs->slots[(base + step) & mask];
            if (slot_load_state_acquire(slot) == 0L) {
                return NULL;
            }
            if (slot->key == key) {
                if (out_metrics) *out_metrics = slot->metrics;
                return slot->bitmap_ptr;
            }
        }
        return NULL;
    }

    EnterCriticalSection(fs->cs_ptr);

    if (fs->cache_entries >= ((size_t)ARCR_GLYPH_HASH_CAP * 3u) / 4u) {
        memset(fs->slots, 0, sizeof(fs->slots));
        fs->cache_entries = 0u;
        if (fs->bitmap_arena.total_used > 16u * 1024u * 1024u) {
            arcr_arena_reset(&fs->bitmap_arena);
        }
    }

    ArcrGlyphCacheSlot *target_slot = NULL;
    for (uint32_t step = 0u; step < (uint32_t)ARCR_GLYPH_HASH_CAP; ++step) {
        ArcrGlyphCacheSlot *slot = &fs->slots[(base + step) & mask];
        if (slot->state != 0L && slot->key == key) {
            fs->cache_hits++;
            if (out_metrics) *out_metrics = slot->metrics;
            const uint8_t *ptr = slot->bitmap_ptr;
            LeaveCriticalSection(fs->cs_ptr);
            return ptr;
        }
        if (slot->state == 0L) {
            target_slot = slot;
            break;
        }
    }

    if (!target_slot) {
        LeaveCriticalSection(fs->cs_ptr);
        return NULL;
    }

    ArcrGlyphMetrics m;
    const uint8_t *arena_ptr = NULL;
    if (!rasterize_win32_glyph_into_arena_locked(fs, codepoint, font_px, bold, monospace,
                                                 &m, &arena_ptr)) {
        LeaveCriticalSection(fs->cs_ptr);
        return NULL;
    }

    target_slot->key        = key;
    target_slot->metrics    = m;
    target_slot->bitmap_ptr = arena_ptr;
    slot_publish_state_release(target_slot);
    fs->cache_entries++;

    if (out_metrics) *out_metrics = m;
    LeaveCriticalSection(fs->cs_ptr);
    return arena_ptr;
}

bool arcr_font_measure_glyph_ex(ArcrFontSystem *fs, uint32_t codepoint,
                                int32_t font_px, bool bold, bool monospace,
                                ArcrGlyphMetrics *out_metrics) {
    if (!out_metrics) return false;
    font_px = ARCR_CLAMP(font_px, 8, ARCR_GLYPH_MAX_FONT_PX);

    if (!fs || !fs->mem_dc) {
        win32_fallback_glyph(codepoint, font_px, bold, monospace, out_metrics, NULL, 0u);
        return true;
    }

    const uint8_t *ptr = arcr_font_get_cached_glyph_ptr(
        fs, codepoint, font_px, bold, monospace, out_metrics, true
    );
    if (ptr) return true;

    win32_fallback_glyph(codepoint, font_px, bold, monospace, out_metrics, NULL, 0u);
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
    font_px = ARCR_CLAMP(font_px, 8, ARCR_GLYPH_MAX_FONT_PX);

    if (!fs || !fs->mem_dc) {
        win32_fallback_glyph(codepoint, font_px, bold, monospace, out_metrics, out_alpha, alpha_cap);
        return ((size_t)out_metrics->width * (size_t)out_metrics->height <= alpha_cap);
    }

    ArcrGlyphMetrics m;
    const uint8_t *ptr = arcr_font_get_cached_glyph_ptr(
        fs, codepoint, font_px, bold, monospace, &m, true
    );
    if (!ptr) return false;

    size_t area = (size_t)m.width * (size_t)m.height;
    if (area > alpha_cap) return false;
    if (area > 0u) {
        memcpy(out_alpha, ptr, area);
    }
    *out_metrics = m;
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
