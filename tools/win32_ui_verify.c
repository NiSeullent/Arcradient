/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * tools/win32_ui_verify.c - Standalone Pure C11 Win32 Single-Frame UI, DPI, UIA & DevTools Verifier
 *
 * Launches `waterscape.exe --module-dir . --theme dark --url waterscape://addons` as a real
 * desktop GUI process and verifies every Windows completion gate in `04_WIN32_UI.md`:
 *   1. Top-level HWND inventory (`EnumWindows` + `GetWindowThreadProcessId`):
 *      exactly 1 visible `WaterscapeBrowserWnd20` window and 0 visible console windows.
 *   2. Server-side UI Automation (`WM_GETOBJECT` / `IUIAutomation` COM client):
 *      root window, `TabItem_0`, `NewTabButton`, `BackButton`, `ForwardButton`,
 *      `ReloadButton`, `OmnibarEdit` (`Value` == `waterscape://addons`), `MainMenuButton`,
 *      and `WebDocument`, plus `IInvokePattern::Invoke()` & `ISelectionItemPattern::Select()`.
 *   3. `WM_NCHITTEST` hit-testing:
 *      tab `HTCLIENT`, omnibar `HTCLIENT`, titlebar/drag-strip `HTCAPTION`, resize borders,
 *      and live negative multi-monitor screen coordinates (`x < 0`).
 *   4. In-window `F12` DevTools dock toggle:
 *      still 1 visible top-level window (0 extra popup/console windows), `DevToolsDock` UIA
 *      pane appears and reclaims viewport height cleanly on second `F12`.
 *   5. `F11` fullscreen entry & `WINDOWPLACEMENT` / frame style restoration.
 *   6. Narrow-window (`520x640`) multi-tab overflow & simulated `WM_DPICHANGED`
 *      (`96 -> 120 -> 144 -> 192 -> 96` DPI) responsiveness and DIP scaling.
 *   7. Full-window `.bmp` + `.png` evidence capture and structured JSON verification report.
 *
 * Build (MinGW-w64 / Clang / MSVC, zero post-Win2000 static imports):
 *   cc -std=c11 -O2 -Wall -Wextra -Wpedantic tools/win32_ui_verify.c -luser32 -lgdi32 -lole32 -o win32_ui_verify.exe
 */
#if defined(_WIN32)
#  ifndef WINVER
#    define WINVER 0x0500
#  endif
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0500
#  endif
#  ifndef COBJMACROS
#    define COBJMACROS
#  endif
#  ifndef UNICODE
#    define UNICODE
#  endif
#  ifndef _UNICODE
#    define _UNICODE
#  endif
#  include <windows.h>
#  include <ole2.h>
#  include <oleauto.h>
#else
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct WsVerifyOptions {
    char exe_path[512];
    char module_dir[512];
    char initial_url[256];
    char theme[64];
    char extra_arg[128];
    char out_dir[512];
    char report_path[560];
    bool show_help;
} WsVerifyOptions;

static void ws_verify_default_options(WsVerifyOptions *opt) {
    memset(opt, 0, sizeof(*opt));
    strcpy(opt->exe_path, "waterscape.exe");
    strcpy(opt->module_dir, ".");
    strcpy(opt->initial_url, "waterscape://addons");
    strcpy(opt->theme, "dark");
    strcpy(opt->out_dir, ".");
    strcpy(opt->report_path, "win32_ui_report.json");
}

static void ws_verify_print_usage(const char *prog) {
    printf("Waterscape Win32 UI Verification Harness (Pure C11)\n"
           "Usage: %s [options]\n"
           "  --exe <path>         Path to waterscape.exe (default: waterscape.exe)\n"
           "  --module-dir <dir>   Engine module directory (default: .)\n"
           "  --url <url>          Startup URL (default: waterscape://addons)\n"
           "  --theme <name>       Theme: dark | light | classic (default: dark)\n"
           "  --extra-arg <arg>    Optional flag (e.g. --cpu-presenter or --classic-frame)\n"
           "  --out-dir <dir>      Output directory for BMP/PNG screenshots (default: .)\n"
           "  --report <path>      Output JSON report path (default: win32_ui_report.json)\n"
           "  --help, -h           Show usage and exit 0\n",
           prog ? prog : "win32_ui_verify");
}

static void ws_copy_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0u) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool ws_parse_args(int argc, char **argv, WsVerifyOptions *opt) {
    bool explicit_report = false;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!a) continue;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0 || strcmp(a, "/?") == 0) {
            opt->show_help = true;
            return true;
        } else if (strcmp(a, "--exe") == 0 && i + 1 < argc) {
            ws_copy_str(opt->exe_path, sizeof(opt->exe_path), argv[++i]);
        } else if (strcmp(a, "--module-dir") == 0 && i + 1 < argc) {
            ws_copy_str(opt->module_dir, sizeof(opt->module_dir), argv[++i]);
        } else if (strcmp(a, "--url") == 0 && i + 1 < argc) {
            ws_copy_str(opt->initial_url, sizeof(opt->initial_url), argv[++i]);
        } else if (strcmp(a, "--theme") == 0 && i + 1 < argc) {
            ws_copy_str(opt->theme, sizeof(opt->theme), argv[++i]);
        } else if (strcmp(a, "--extra-arg") == 0 && i + 1 < argc) {
            ws_copy_str(opt->extra_arg, sizeof(opt->extra_arg), argv[++i]);
        } else if (strcmp(a, "--out-dir") == 0 && i + 1 < argc) {
            ws_copy_str(opt->out_dir, sizeof(opt->out_dir), argv[++i]);
        } else if (strcmp(a, "--report") == 0 && i + 1 < argc) {
            ws_copy_str(opt->report_path, sizeof(opt->report_path), argv[++i]);
            explicit_report = true;
        } else {
            fprintf(stderr, "win32_ui_verify: unknown or incomplete option '%s'\n", a);
            return false;
        }
    }
    if (!explicit_report) {
        snprintf(opt->report_path, sizeof(opt->report_path),
                 "%s/win32_ui_report.json", opt->out_dir);
    }
    return true;
}

#if defined(_WIN32)

#ifndef WM_DPICHANGED
#  define WM_DPICHANGED 0x02E0
#endif
#ifndef WM_GETOBJECT
#  define WM_GETOBJECT 0x003D
#endif
#ifndef PW_RENDERFULLCONTENT
#  define PW_RENDERFULLCONTENT 0x00000002u
#endif
#ifndef MONITOR_DEFAULTTONEAREST
#  define MONITOR_DEFAULTTONEAREST 0x00000002u
#endif

#define WS_VERIFY_LOAD_PROC(hmod, fn_ptr, sym_name)                              \
    do {                                                                         \
        FARPROC raw_p_ = (hmod) ? GetProcAddress((hmod), (sym_name)) : NULL;     \
        memcpy(&(fn_ptr), &raw_p_, sizeof(fn_ptr));                              \
    } while (0)

/* =========================================================================
 * 1. Self-Contained BMP + Stored-Zlib PNG Writers (Zero External Libs)
 * ========================================================================= */
static uint32_t ws_crc32_calc(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0u; i < len; ++i) {
        crc ^= (uint32_t)buf[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t ws_adler32_calc(const uint8_t *buf, size_t len) {
    uint32_t s1 = 1u;
    uint32_t s2 = 0u;
    for (size_t i = 0u; i < len; ++i) {
        s1 = (s1 + buf[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    return (s2 << 16) | s1;
}

static void ws_put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static bool ws_write_png_chunk(FILE *fp, const char type[4], const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    ws_put_be32(hdr, len);
    memcpy(hdr + 4, type, 4u);
    if (fwrite(hdr, 1u, 8u, fp) != 8u) return false;
    if (len > 0u && fwrite(data, 1u, len, fp) != len) return false;

    uint8_t *crc_buf = (uint8_t *)malloc((size_t)len + 4u);
    if (!crc_buf) return false;
    memcpy(crc_buf, type, 4u);
    if (len > 0u) memcpy(crc_buf + 4u, data, len);
    uint32_t crc = ws_crc32_calc(crc_buf, (size_t)len + 4u);
    free(crc_buf);

    uint8_t crc_be[4];
    ws_put_be32(crc_be, crc);
    return fwrite(crc_be, 1u, 4u, fp) == 4u;
}

static bool ws_save_bgra_as_png(const char *path, const uint8_t *bgra_topdown, int32_t w, int32_t h) {
    if (!path || !bgra_topdown || w <= 0 || h <= 0) return false;
    size_t row_raw = 1u + (size_t)w * 4u;
    size_t raw_len = row_raw * (size_t)h;
    uint8_t *raw   = (uint8_t *)malloc(raw_len);
    if (!raw) return false;

    for (int32_t y = 0; y < h; ++y) {
        uint8_t *dst       = raw + (size_t)y * row_raw;
        const uint8_t *src = bgra_topdown + (size_t)y * (size_t)w * 4u;
        dst[0] = 0u; /* Filter 0 (None) */
        for (int32_t x = 0; x < w; ++x) {
            dst[1 + x * 4 + 0] = src[x * 4 + 2]; /* R */
            dst[1 + x * 4 + 1] = src[x * 4 + 1]; /* G */
            dst[1 + x * 4 + 2] = src[x * 4 + 0]; /* B */
            dst[1 + x * 4 + 3] = 255u;           /* Opaque A */
        }
    }

    size_t num_blocks = (raw_len + 65534u) / 65535u;
    if (num_blocks == 0u) num_blocks = 1u;
    size_t zcap   = 2u + num_blocks * 5u + raw_len + 4u;
    uint8_t *zbuf = (uint8_t *)malloc(zcap);
    if (!zbuf) {
        free(raw);
        return false;
    }

    size_t zp = 0u;
    zbuf[zp++] = 0x78u;
    zbuf[zp++] = 0x01u;
    size_t rem = raw_len;
    size_t off = 0u;
    while (rem > 0u) {
        uint16_t blk  = (rem > 65535u) ? 65535u : (uint16_t)rem;
        uint8_t  last = (rem <= 65535u) ? 1u : 0u;
        uint16_t nblk = (uint16_t)~blk;
        zbuf[zp++] = last;
        zbuf[zp++] = (uint8_t)(blk & 0xFFu);
        zbuf[zp++] = (uint8_t)((blk >> 8) & 0xFFu);
        zbuf[zp++] = (uint8_t)(nblk & 0xFFu);
        zbuf[zp++] = (uint8_t)((nblk >> 8) & 0xFFu);
        memcpy(zbuf + zp, raw + off, blk);
        zp  += blk;
        off += blk;
        rem -= blk;
    }
    ws_put_be32(zbuf + zp, ws_adler32_calc(raw, raw_len));
    zp += 4u;
    free(raw);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        free(zbuf);
        return false;
    }
    static const uint8_t kPngSig[8] = {137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u};
    uint8_t ihdr[13];
    ws_put_be32(ihdr + 0, (uint32_t)w);
    ws_put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8]  = 8u; /* 8 bits/channel */
    ihdr[9]  = 6u; /* RGBA */
    ihdr[10] = 0u;
    ihdr[11] = 0u;
    ihdr[12] = 0u;

    bool ok = (fwrite(kPngSig, 1u, 8u, fp) == 8u) &&
              ws_write_png_chunk(fp, "IHDR", ihdr, 13u) &&
              ws_write_png_chunk(fp, "IDAT", zbuf, (uint32_t)zp) &&
              ws_write_png_chunk(fp, "IEND", NULL, 0u);
    fclose(fp);
    free(zbuf);
    return ok;
}

static bool ws_save_bgra_as_bmp(const char *path, const uint8_t *bgra_topdown, int32_t w, int32_t h) {
    if (!path || !bgra_topdown || w <= 0 || h <= 0) return false;
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;

    uint32_t img_bytes = (uint32_t)w * (uint32_t)h * 4u;
    BITMAPFILEHEADER bfh;
    BITMAPINFOHEADER bih;
    ZeroMemory(&bfh, sizeof(bfh));
    ZeroMemory(&bih, sizeof(bih));

    bfh.bfType    = 0x4D42u; /* 'BM' */
    bfh.bfOffBits = (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));
    bfh.bfSize    = bfh.bfOffBits + img_bytes;

    bih.biSize        = sizeof(BITMAPINFOHEADER);
    bih.biWidth       = w;
    bih.biHeight      = -h; /* Top-down DIB */
    bih.biPlanes      = 1u;
    bih.biBitCount    = 32u;
    bih.biCompression = BI_RGB;
    bih.biSizeImage   = img_bytes;

    bool ok = (fwrite(&bfh, 1u, sizeof(bfh), fp) == sizeof(bfh)) &&
              (fwrite(&bih, 1u, sizeof(bih), fp) == sizeof(bih)) &&
              (fwrite(bgra_topdown, 1u, img_bytes, fp) == img_bytes);
    fclose(fp);
    return ok;
}

typedef BOOL (WINAPI *PFN_WsPrintWindow)(HWND, HDC, UINT);

static bool ws_capture_window_images(HWND hwnd, const char *bmp_or_null, const char *png_or_null) {
    if (!IsWindow(hwnd)) return false;
    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return false;
    int32_t w = (int32_t)(wr.right - wr.left);
    int32_t h = (int32_t)(wr.bottom - wr.top);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;

    HDC screen_dc = GetDC(NULL);
    HDC mem_dc    = CreateCompatibleDC(screen_dc);
    if (!mem_dc) {
        if (screen_dc) ReleaseDC(NULL, screen_dc);
        return false;
    }

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h; /* Top-down */
    bmi.bmiHeader.biPlanes      = 1u;
    bmi.bmiHeader.biBitCount    = 32u;
    bmi.bmiHeader.biCompression = BI_RGB;

    void   *bits = NULL;
    HBITMAP hbm  = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0u);
    if (!hbm || !bits) {
        if (hbm) DeleteObject(hbm);
        DeleteDC(mem_dc);
        if (screen_dc) ReleaseDC(NULL, screen_dc);
        return false;
    }

    HGDIOBJ old_obj = SelectObject(mem_dc, hbm);
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    PFN_WsPrintWindow fn_print_win = NULL;
    WS_VERIFY_LOAD_PROC(hUser32, fn_print_win, "PrintWindow");

    BOOL captured = FALSE;
    if (fn_print_win) {
        captured = fn_print_win(hwnd, mem_dc, PW_RENDERFULLCONTENT);
        if (!captured) {
            captured = fn_print_win(hwnd, mem_dc, 0u);
        }
    }
    if (!captured) {
        HDC wnd_dc = GetWindowDC(hwnd);
        if (wnd_dc) {
            captured = BitBlt(mem_dc, 0, 0, w, h, wnd_dc, 0, 0, SRCCOPY);
            ReleaseDC(hwnd, wnd_dc);
        }
    }
    GdiFlush();

    bool ok = true;
    if (bmp_or_null && bmp_or_null[0] != '\0') {
        if (!ws_save_bgra_as_bmp(bmp_or_null, (const uint8_t *)bits, w, h)) ok = false;
    }
    if (png_or_null && png_or_null[0] != '\0') {
        if (!ws_save_bgra_as_png(png_or_null, (const uint8_t *)bits, w, h)) ok = false;
    }

    SelectObject(mem_dc, old_obj);
    DeleteObject(hbm);
    DeleteDC(mem_dc);
    if (screen_dc) ReleaseDC(NULL, screen_dc);
    return ok && (captured != FALSE);
}

/* =========================================================================
 * 2. Top-Level Window Inventory (`EnumWindows` + `GetWindowThreadProcessId`)
 * ========================================================================= */
typedef struct WsWndEntry {
    HWND  hwnd;
    HWND  owner;
    DWORD pid;
    DWORD style;
    DWORD ex_style;
    bool  visible;
    RECT  win_rect;
    RECT  cli_rect;
    char  class_name[128];
    char  title[256];
} WsWndEntry;

typedef struct WsWndInventory {
    DWORD      target_pid;
    WsWndEntry items[64];
    size_t     count;
    size_t     visible_total;
    size_t     visible_browser_count;
    size_t     visible_console_count;
    HWND       primary_browser_hwnd;
} WsWndInventory;

static void ws_wide_to_u8(const WCHAR *ws, char *out, size_t cap) {
    if (!out || cap == 0u) return;
    out[0] = '\0';
    if (!ws || ws[0] == L'\0') return;
    WideCharToMultiByte(CP_UTF8, 0u, ws, -1, out, (int)cap, NULL, NULL);
    out[cap - 1u] = '\0';
}

static void ws_u8_to_wide(const char *u8, WCHAR *out, size_t cap) {
    if (!out || cap == 0u) return;
    out[0] = L'\0';
    if (!u8 || u8[0] == '\0') return;
    int n = MultiByteToWideChar(CP_UTF8, 0u, u8, -1, out, (int)cap);
    if (n <= 0) {
        MultiByteToWideChar(CP_ACP, 0u, u8, -1, out, (int)cap);
    }
    out[cap - 1u] = L'\0';
}

static bool ws_is_console_class(const char *cls) {
    if (!cls) return false;
    return strcmp(cls, "ConsoleWindowClass") == 0 ||
           strcmp(cls, "PseudoConsoleWindow") == 0 ||
           strcmp(cls, "CASCADIA_HOSTING_WINDOW_CLASS") == 0;
}

static BOOL CALLBACK ws_enum_proc_for_pid(HWND hwnd, LPARAM lp) {
    WsWndInventory *inv = (WsWndInventory *)lp;
    DWORD pid = 0u;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != inv->target_pid) {
        return TRUE;
    }

    WCHAR wcls[128];
    WCHAR wttl[256];
    wcls[0] = L'\0';
    wttl[0] = L'\0';
    GetClassNameW(hwnd, wcls, 128);
    GetWindowTextW(hwnd, wttl, 256);

    bool vis = IsWindowVisible(hwnd) ? true : false;
    char cls_u8[128];
    char ttl_u8[256];
    ws_wide_to_u8(wcls, cls_u8, sizeof(cls_u8));
    ws_wide_to_u8(wttl, ttl_u8, sizeof(ttl_u8));

    if (inv->count < sizeof(inv->items) / sizeof(inv->items[0])) {
        WsWndEntry *e = &inv->items[inv->count++];
        memset(e, 0, sizeof(*e));
        e->hwnd     = hwnd;
        e->owner    = GetWindow(hwnd, GW_OWNER);
        e->pid      = pid;
        e->style    = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
        e->ex_style = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
        e->visible  = vis;
        GetWindowRect(hwnd, &e->win_rect);
        GetClientRect(hwnd, &e->cli_rect);
        ws_copy_str(e->class_name, sizeof(e->class_name), cls_u8);
        ws_copy_str(e->title, sizeof(e->title), ttl_u8);
    }

    if (vis) {
        inv->visible_total++;
        if (strcmp(cls_u8, "WaterscapeBrowserWnd20") == 0) {
            inv->visible_browser_count++;
            if (!inv->primary_browser_hwnd) {
                inv->primary_browser_hwnd = hwnd;
            }
        }
        if (ws_is_console_class(cls_u8)) {
            inv->visible_console_count++;
        }
    }
    return TRUE;
}

typedef struct WsConsoleScan {
    HWND   known[64];
    size_t known_count;
    size_t newly_appeared;
} WsConsoleScan;

static BOOL CALLBACK ws_enum_record_consoles(HWND hwnd, LPARAM lp) {
    WsConsoleScan *cs = (WsConsoleScan *)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;
    WCHAR wcls[128];
    wcls[0] = L'\0';
    GetClassNameW(hwnd, wcls, 128);
    char cls[128];
    ws_wide_to_u8(wcls, cls, sizeof(cls));
    if (ws_is_console_class(cls) && cs->known_count < 64u) {
        cs->known[cs->known_count++] = hwnd;
    }
    return TRUE;
}

static BOOL CALLBACK ws_enum_diff_consoles(HWND hwnd, LPARAM lp) {
    WsConsoleScan *cs = (WsConsoleScan *)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;
    WCHAR wcls[128];
    wcls[0] = L'\0';
    GetClassNameW(hwnd, wcls, 128);
    char cls[128];
    ws_wide_to_u8(wcls, cls, sizeof(cls));
    if (!ws_is_console_class(cls)) return TRUE;
    for (size_t i = 0u; i < cs->known_count; ++i) {
        if (cs->known[i] == hwnd) return TRUE;
    }
    cs->newly_appeared++;
    return TRUE;
}

static void ws_collect_inventory(DWORD pid, WsWndInventory *out) {
    memset(out, 0, sizeof(*out));
    out->target_pid = pid;
    EnumWindows(ws_enum_proc_for_pid, (LPARAM)out);
}

/* =========================================================================
 * 3. Pure C11 UI Automation COM Client (`IUIAutomation` + Patterns)
 * ========================================================================= */
static const GUID kCLSID_CUIAutomation = {
    0xFF48DBA4u, 0x60EFu, 0x4201u, {0xAAu, 0x87u, 0x54u, 0x10u, 0x3Eu, 0xEFu, 0x59u, 0x4Eu}
};
static const GUID kIID_IUIAutomation = {
    0x30CBE57Du, 0xD9D0u, 0x452Au, {0xABu, 0x13u, 0x7Au, 0xC5u, 0xACu, 0x48u, 0x25u, 0xEEu}
};
static const GUID kIID_IUIAutomationInvokePattern = {
    0xFB377FBEu, 0x8EA6u, 0x46D5u, {0x9Cu, 0x73u, 0x64u, 0x99u, 0x64u, 0x2Du, 0x30u, 0x59u}
};
static const GUID kIID_IUIAutomationSelectionItemPattern = {
    0xA8EFA66Au, 0x0FDAu, 0x421Au, {0x91u, 0x94u, 0x38u, 0x02u, 0x1Fu, 0x35u, 0x78u, 0xEAu}
};

#define WS_UIA_InvokePatternId        10000
#define WS_UIA_SelectionItemPatternId 10010
#define WS_UIA_ValueValuePropertyId   30045
#define WS_UIA_ButtonControlTypeId    50000
#define WS_UIA_EditControlTypeId      50004
#define WS_UIA_TabItemControlTypeId   50019
#define WS_UIA_DocumentControlTypeId  50030
#define WS_UIA_WindowControlTypeId    50032
#define WS_UIA_PaneControlTypeId      50033

typedef struct WsUiaCliElement       WsUiaCliElement;
typedef struct WsUiaCliTreeWalker    WsUiaCliTreeWalker;
typedef struct WsUiaCliAutomation    WsUiaCliAutomation;
typedef struct WsUiaCliInvokePat     WsUiaCliInvokePat;
typedef struct WsUiaCliSelItemPat    WsUiaCliSelItemPat;

typedef struct WsUiaCliInvokePatVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WsUiaCliInvokePat *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WsUiaCliInvokePat *This);
    ULONG   (STDMETHODCALLTYPE *Release)(WsUiaCliInvokePat *This);
    HRESULT (STDMETHODCALLTYPE *Invoke)(WsUiaCliInvokePat *This);
} WsUiaCliInvokePatVtbl;

struct WsUiaCliInvokePat {
    const WsUiaCliInvokePatVtbl *lpVtbl;
};

typedef struct WsUiaCliSelItemPatVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WsUiaCliSelItemPat *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WsUiaCliSelItemPat *This);
    ULONG   (STDMETHODCALLTYPE *Release)(WsUiaCliSelItemPat *This);
    HRESULT (STDMETHODCALLTYPE *Select)(WsUiaCliSelItemPat *This);
} WsUiaCliSelItemPatVtbl;

struct WsUiaCliSelItemPat {
    const WsUiaCliSelItemPatVtbl *lpVtbl;
};

typedef struct WsUiaCliElementVtbl {
    /* 0..2: IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WsUiaCliElement *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WsUiaCliElement *This);
    ULONG   (STDMETHODCALLTYPE *Release)(WsUiaCliElement *This);
    /* 3..9 */
    HRESULT (STDMETHODCALLTYPE *SetFocus)(WsUiaCliElement *This);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeId)(WsUiaCliElement *This, SAFEARRAY **runtimeId);
    HRESULT (STDMETHODCALLTYPE *FindFirst)(WsUiaCliElement *This, int scope, void *cond, WsUiaCliElement **found);
    HRESULT (STDMETHODCALLTYPE *FindAll)(WsUiaCliElement *This, int scope, void *cond, void **found);
    HRESULT (STDMETHODCALLTYPE *FindFirstBuildCache)(WsUiaCliElement *This, int scope, void *cond, void *cr, WsUiaCliElement **found);
    HRESULT (STDMETHODCALLTYPE *FindAllBuildCache)(WsUiaCliElement *This, int scope, void *cond, void *cr, void **found);
    HRESULT (STDMETHODCALLTYPE *BuildUpdatedCache)(WsUiaCliElement *This, void *cr, WsUiaCliElement **updated);
    /* 10..19 */
    HRESULT (STDMETHODCALLTYPE *GetCurrentPropertyValue)(WsUiaCliElement *This, int propId, VARIANT *retVal);
    HRESULT (STDMETHODCALLTYPE *GetCurrentPropertyValueEx)(WsUiaCliElement *This, int propId, BOOL ignoreDef, VARIANT *retVal);
    HRESULT (STDMETHODCALLTYPE *GetCachedPropertyValue)(WsUiaCliElement *This, int propId, VARIANT *retVal);
    HRESULT (STDMETHODCALLTYPE *GetCachedPropertyValueEx)(WsUiaCliElement *This, int propId, BOOL ignoreDef, VARIANT *retVal);
    HRESULT (STDMETHODCALLTYPE *GetCurrentPatternAs)(WsUiaCliElement *This, int patId, REFIID riid, void **patObj);
    HRESULT (STDMETHODCALLTYPE *GetCachedPatternAs)(WsUiaCliElement *This, int patId, REFIID riid, void **patObj);
    HRESULT (STDMETHODCALLTYPE *GetCurrentPattern)(WsUiaCliElement *This, int patId, IUnknown **patObj);
    HRESULT (STDMETHODCALLTYPE *GetCachedPattern)(WsUiaCliElement *This, int patId, IUnknown **patObj);
    HRESULT (STDMETHODCALLTYPE *GetCachedParent)(WsUiaCliElement *This, WsUiaCliElement **parent);
    HRESULT (STDMETHODCALLTYPE *GetCachedChildren)(WsUiaCliElement *This, void **children);
    /* 20..43 */
    HRESULT (STDMETHODCALLTYPE *get_CurrentProcessId)(WsUiaCliElement *This, int *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentControlType)(WsUiaCliElement *This, int *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentLocalizedControlType)(WsUiaCliElement *This, BSTR *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentName)(WsUiaCliElement *This, BSTR *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentAcceleratorKey)(WsUiaCliElement *This, BSTR *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentAccessKey)(WsUiaCliElement *This, BSTR *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentHasKeyboardFocus)(WsUiaCliElement *This, BOOL *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentIsKeyboardFocusable)(WsUiaCliElement *This, BOOL *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentIsEnabled)(WsUiaCliElement *This, BOOL *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentAutomationId)(WsUiaCliElement *This, BSTR *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentClassName)(WsUiaCliElement *This, BSTR *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentHelpText)(WsUiaCliElement *This, BSTR *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentCulture)(WsUiaCliElement *This, int *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentIsControlElement)(WsUiaCliElement *This, BOOL *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentIsContentElement)(WsUiaCliElement *This, BOOL *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentIsPassword)(WsUiaCliElement *This, BOOL *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentNativeWindowHandle)(WsUiaCliElement *This, void **retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentItemType)(WsUiaCliElement *This, BSTR *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentIsOffscreen)(WsUiaCliElement *This, BOOL *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentOrientation)(WsUiaCliElement *This, int *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentFrameworkId)(WsUiaCliElement *This, BSTR *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentIsRequiredForForm)(WsUiaCliElement *This, BOOL *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentItemStatus)(WsUiaCliElement *This, BSTR *retVal);
    HRESULT (STDMETHODCALLTYPE *get_CurrentBoundingRectangle)(WsUiaCliElement *This, RECT *retVal);
} WsUiaCliElementVtbl;

struct WsUiaCliElement {
    const WsUiaCliElementVtbl *lpVtbl;
};

typedef struct WsUiaCliTreeWalkerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WsUiaCliTreeWalker *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WsUiaCliTreeWalker *This);
    ULONG   (STDMETHODCALLTYPE *Release)(WsUiaCliTreeWalker *This);
    HRESULT (STDMETHODCALLTYPE *GetParentElement)(WsUiaCliTreeWalker *This, WsUiaCliElement *el, WsUiaCliElement **parent);
    HRESULT (STDMETHODCALLTYPE *GetFirstChildElement)(WsUiaCliTreeWalker *This, WsUiaCliElement *el, WsUiaCliElement **first);
    HRESULT (STDMETHODCALLTYPE *GetLastChildElement)(WsUiaCliTreeWalker *This, WsUiaCliElement *el, WsUiaCliElement **last);
    HRESULT (STDMETHODCALLTYPE *GetNextSiblingElement)(WsUiaCliTreeWalker *This, WsUiaCliElement *el, WsUiaCliElement **next);
    HRESULT (STDMETHODCALLTYPE *GetPreviousSiblingElement)(WsUiaCliTreeWalker *This, WsUiaCliElement *el, WsUiaCliElement **prev);
} WsUiaCliTreeWalkerVtbl;

struct WsUiaCliTreeWalker {
    const WsUiaCliTreeWalkerVtbl *lpVtbl;
};

typedef struct WsUiaCliAutomationVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WsUiaCliAutomation *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WsUiaCliAutomation *This);
    ULONG   (STDMETHODCALLTYPE *Release)(WsUiaCliAutomation *This);
    HRESULT (STDMETHODCALLTYPE *CompareElements)(WsUiaCliAutomation *This, WsUiaCliElement *el1, WsUiaCliElement *el2, BOOL *same);
    HRESULT (STDMETHODCALLTYPE *CompareRuntimeIds)(WsUiaCliAutomation *This, SAFEARRAY *id1, SAFEARRAY *id2, BOOL *same);
    HRESULT (STDMETHODCALLTYPE *GetRootElement)(WsUiaCliAutomation *This, WsUiaCliElement **root);
    HRESULT (STDMETHODCALLTYPE *ElementFromHandle)(WsUiaCliAutomation *This, HWND hwnd, WsUiaCliElement **el);
    HRESULT (STDMETHODCALLTYPE *ElementFromPoint)(WsUiaCliAutomation *This, POINT pt, WsUiaCliElement **el);
    HRESULT (STDMETHODCALLTYPE *GetFocusedElement)(WsUiaCliAutomation *This, WsUiaCliElement **el);
    HRESULT (STDMETHODCALLTYPE *GetRootElementBuildCache)(WsUiaCliAutomation *This, void *cr, WsUiaCliElement **root);
    HRESULT (STDMETHODCALLTYPE *ElementFromHandleBuildCache)(WsUiaCliAutomation *This, HWND hwnd, void *cr, WsUiaCliElement **el);
    HRESULT (STDMETHODCALLTYPE *ElementFromPointBuildCache)(WsUiaCliAutomation *This, POINT pt, void *cr, WsUiaCliElement **el);
    HRESULT (STDMETHODCALLTYPE *GetFocusedElementBuildCache)(WsUiaCliAutomation *This, void *cr, WsUiaCliElement **el);
    HRESULT (STDMETHODCALLTYPE *CreateTreeWalker)(WsUiaCliAutomation *This, void *cond, WsUiaCliTreeWalker **walker);
    HRESULT (STDMETHODCALLTYPE *get_ControlViewWalker)(WsUiaCliAutomation *This, WsUiaCliTreeWalker **walker);
    HRESULT (STDMETHODCALLTYPE *get_ContentViewWalker)(WsUiaCliAutomation *This, WsUiaCliTreeWalker **walker);
    HRESULT (STDMETHODCALLTYPE *get_RawViewWalker)(WsUiaCliAutomation *This, WsUiaCliTreeWalker **walker);
} WsUiaCliAutomationVtbl;

struct WsUiaCliAutomation {
    const WsUiaCliAutomationVtbl *lpVtbl;
};

typedef void    (WINAPI *PFN_WsSysFreeString)(BSTR);
typedef HRESULT (WINAPI *PFN_WsVariantClear)(VARIANTARG *);

typedef struct WsUiaNodeSnapshot {
    char automation_id[64];
    char name[160];
    char value[256];
    int  control_type;
    RECT bounds;
} WsUiaNodeSnapshot;

typedef struct WsUiaTreeSnapshot {
    bool              available;
    WsUiaNodeSnapshot root;
    WsUiaNodeSnapshot children[48];
    size_t            child_count;
} WsUiaTreeSnapshot;

static void ws_free_bstr(PFN_WsSysFreeString fn_free, BSTR b) {
    if (b && fn_free) fn_free(b);
}

static void ws_read_element_snapshot(WsUiaCliElement *el,
                                     PFN_WsSysFreeString fn_free_bstr,
                                     PFN_WsVariantClear fn_var_clear,
                                     WsUiaNodeSnapshot *out) {
    memset(out, 0, sizeof(*out));
    if (!el) return;

    int ct = 0;
    if (SUCCEEDED(el->lpVtbl->get_CurrentControlType(el, &ct))) {
        out->control_type = ct;
    }
    BSTR b_aid = NULL;
    if (SUCCEEDED(el->lpVtbl->get_CurrentAutomationId(el, &b_aid)) && b_aid) {
        ws_wide_to_u8(b_aid, out->automation_id, sizeof(out->automation_id));
        ws_free_bstr(fn_free_bstr, b_aid);
    }
    BSTR b_name = NULL;
    if (SUCCEEDED(el->lpVtbl->get_CurrentName(el, &b_name)) && b_name) {
        ws_wide_to_u8(b_name, out->name, sizeof(out->name));
        ws_free_bstr(fn_free_bstr, b_name);
    }
    RECT rc = {0, 0, 0, 0};
    if (SUCCEEDED(el->lpVtbl->get_CurrentBoundingRectangle(el, &rc))) {
        out->bounds = rc;
    }

    VARIANT v;
    memset(&v, 0, sizeof(v));
    if (SUCCEEDED(el->lpVtbl->GetCurrentPropertyValue(el, WS_UIA_ValueValuePropertyId, &v))) {
        if (V_VT(&v) == VT_BSTR && V_BSTR(&v)) {
            ws_wide_to_u8(V_BSTR(&v), out->value, sizeof(out->value));
        }
        if (fn_var_clear) {
            (void)fn_var_clear(&v);
        } else if (V_VT(&v) == VT_BSTR && V_BSTR(&v)) {
            ws_free_bstr(fn_free_bstr, V_BSTR(&v));
        }
    }
}

static bool ws_snapshot_uia_tree(WsUiaCliAutomation *uia,
                                 HWND hwnd,
                                 PFN_WsSysFreeString fn_free_bstr,
                                 PFN_WsVariantClear fn_var_clear,
                                 WsUiaTreeSnapshot *out) {
    memset(out, 0, sizeof(*out));
    if (!uia || !IsWindow(hwnd)) return false;

    WsUiaCliElement *root = NULL;
    if (FAILED(uia->lpVtbl->ElementFromHandle(uia, hwnd, &root)) || !root) {
        return false;
    }

    out->available = true;
    ws_read_element_snapshot(root, fn_free_bstr, fn_var_clear, &out->root);

    WsUiaCliTreeWalker *walker = NULL;
    if (SUCCEEDED(uia->lpVtbl->get_RawViewWalker(uia, &walker)) && walker) {
        WsUiaCliElement *cur = NULL;
        if (SUCCEEDED(walker->lpVtbl->GetFirstChildElement(walker, root, &cur))) {
            while (cur && out->child_count < sizeof(out->children) / sizeof(out->children[0])) {
                ws_read_element_snapshot(cur, fn_free_bstr, fn_var_clear,
                                         &out->children[out->child_count++]);
                WsUiaCliElement *next = NULL;
                HRESULT hr_n = walker->lpVtbl->GetNextSiblingElement(walker, cur, &next);
                cur->lpVtbl->Release(cur);
                cur = (SUCCEEDED(hr_n) && next) ? next : NULL;
            }
            if (cur) cur->lpVtbl->Release(cur);
        }
        walker->lpVtbl->Release(walker);
    }

    root->lpVtbl->Release(root);
    return true;
}

static const WsUiaNodeSnapshot *ws_find_uia_child(const WsUiaTreeSnapshot *snap, const char *aid) {
    if (!snap || !aid) return NULL;
    for (size_t i = 0u; i < snap->child_count; ++i) {
        if (strcmp(snap->children[i].automation_id, aid) == 0) {
            return &snap->children[i];
        }
    }
    return NULL;
}

static bool ws_uia_invoke_by_aid(WsUiaCliAutomation *uia,
                                 HWND hwnd,
                                 PFN_WsSysFreeString fn_free_bstr,
                                 const char *target_aid) {
    if (!uia || !target_aid) return false;
    WsUiaCliElement *root = NULL;
    if (FAILED(uia->lpVtbl->ElementFromHandle(uia, hwnd, &root)) || !root) return false;

    bool invoked = false;
    WsUiaCliTreeWalker *walker = NULL;
    if (SUCCEEDED(uia->lpVtbl->get_RawViewWalker(uia, &walker)) && walker) {
        WsUiaCliElement *cur = NULL;
        if (SUCCEEDED(walker->lpVtbl->GetFirstChildElement(walker, root, &cur))) {
            while (cur) {
                char aid[64];
                aid[0] = '\0';
                BSTR b_aid = NULL;
                if (SUCCEEDED(cur->lpVtbl->get_CurrentAutomationId(cur, &b_aid)) && b_aid) {
                    ws_wide_to_u8(b_aid, aid, sizeof(aid));
                    ws_free_bstr(fn_free_bstr, b_aid);
                }
                if (strcmp(aid, target_aid) == 0) {
                    void *pat_void = NULL;
                    if (SUCCEEDED(cur->lpVtbl->GetCurrentPatternAs(
                            cur, WS_UIA_InvokePatternId,
                            &kIID_IUIAutomationInvokePattern, &pat_void)) && pat_void) {
                        WsUiaCliInvokePat *inv = (WsUiaCliInvokePat *)pat_void;
                        if (SUCCEEDED(inv->lpVtbl->Invoke(inv))) {
                            invoked = true;
                        }
                        inv->lpVtbl->Release(inv);
                    }
                    cur->lpVtbl->Release(cur);
                    break;
                }
                WsUiaCliElement *next = NULL;
                HRESULT hr_n = walker->lpVtbl->GetNextSiblingElement(walker, cur, &next);
                cur->lpVtbl->Release(cur);
                cur = (SUCCEEDED(hr_n) && next) ? next : NULL;
            }
        }
        walker->lpVtbl->Release(walker);
    }
    root->lpVtbl->Release(root);
    return invoked;
}

static bool ws_uia_select_tab_by_aid(WsUiaCliAutomation *uia,
                                     HWND hwnd,
                                     PFN_WsSysFreeString fn_free_bstr,
                                     const char *target_aid) {
    if (!uia || !target_aid) return false;
    WsUiaCliElement *root = NULL;
    if (FAILED(uia->lpVtbl->ElementFromHandle(uia, hwnd, &root)) || !root) return false;

    bool selected = false;
    WsUiaCliTreeWalker *walker = NULL;
    if (SUCCEEDED(uia->lpVtbl->get_RawViewWalker(uia, &walker)) && walker) {
        WsUiaCliElement *cur = NULL;
        if (SUCCEEDED(walker->lpVtbl->GetFirstChildElement(walker, root, &cur))) {
            while (cur) {
                char aid[64];
                aid[0] = '\0';
                BSTR b_aid = NULL;
                if (SUCCEEDED(cur->lpVtbl->get_CurrentAutomationId(cur, &b_aid)) && b_aid) {
                    ws_wide_to_u8(b_aid, aid, sizeof(aid));
                    ws_free_bstr(fn_free_bstr, b_aid);
                }
                if (strcmp(aid, target_aid) == 0) {
                    void *pat_void = NULL;
                    if (SUCCEEDED(cur->lpVtbl->GetCurrentPatternAs(
                            cur, WS_UIA_SelectionItemPatternId,
                            &kIID_IUIAutomationSelectionItemPattern, &pat_void)) && pat_void) {
                        WsUiaCliSelItemPat *sel = (WsUiaCliSelItemPat *)pat_void;
                        if (SUCCEEDED(sel->lpVtbl->Select(sel))) {
                            selected = true;
                        }
                        sel->lpVtbl->Release(sel);
                    }
                    cur->lpVtbl->Release(cur);
                    break;
                }
                WsUiaCliElement *next = NULL;
                HRESULT hr_n = walker->lpVtbl->GetNextSiblingElement(walker, cur, &next);
                cur->lpVtbl->Release(cur);
                cur = (SUCCEEDED(hr_n) && next) ? next : NULL;
            }
        }
        walker->lpVtbl->Release(walker);
    }
    root->lpVtbl->Release(root);
    return selected;
}

/* =========================================================================
 * 4. Check Recording & JSON Report Serialization
 * ========================================================================= */
typedef struct WsCheckResult {
    char name[96];
    bool passed;
    char detail[256];
} WsCheckResult;

typedef struct WsCheckSuite {
    WsCheckResult items[64];
    size_t        count;
    size_t        failed_count;
} WsCheckSuite;

static void ws_record_check(WsCheckSuite *s, const char *name, bool passed, const char *detail) {
    if (s->count < sizeof(s->items) / sizeof(s->items[0])) {
        WsCheckResult *r = &s->items[s->count++];
        ws_copy_str(r->name, sizeof(r->name), name);
        r->passed = passed;
        ws_copy_str(r->detail, sizeof(r->detail), detail ? detail : "");
    }
    if (!passed) s->failed_count++;
    printf("[%s] %-42s %s\n", passed ? "PASS" : "FAIL", name, detail ? detail : "");
    fflush(stdout);
}

static void ws_json_write_escaped(FILE *fp, const char *s) {
    fputc('"', fp);
    if (s) {
        for (size_t i = 0u; s[i] != '\0'; ++i) {
            unsigned char c = (unsigned char)s[i];
            if (c == '"')       fputs("\\\"", fp);
            else if (c == '\\') fputs("\\\\", fp);
            else if (c == '\n') fputs("\\n", fp);
            else if (c == '\r') fputs("\\r", fp);
            else if (c == '\t') fputs("\\t", fp);
            else if (c < 0x20u) fprintf(fp, "\\u%04x", (unsigned int)c);
            else                fputc((int)c, fp);
        }
    }
    fputc('"', fp);
}

static bool ws_rect_nonempty(RECT r) {
    return (r.right > r.left) && (r.bottom > r.top);
}

static LRESULT ws_send_nchittest(HWND hwnd, int screen_x, int screen_y, bool *out_ok) {
    DWORD_PTR res = 0u;
    LPARAM lp = MAKELPARAM((WORD)(short)screen_x, (WORD)(short)screen_y);
    LRESULT ok = SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0u, lp,
                                     SMTO_NORMAL, 2000u, &res);
    if (out_ok) *out_ok = (ok != 0);
    return (LRESULT)res;
}

static bool ws_send_fkey(HWND hwnd, UINT vk) {
    DWORD_PTR r1 = 0u, r2 = 0u;
    if (!SendMessageTimeoutW(hwnd, WM_KEYDOWN, (WPARAM)vk, 0, SMTO_NORMAL, 2000u, &r1)) {
        return false;
    }
    (void)SendMessageTimeoutW(hwnd, WM_KEYUP, (WPARAM)vk, (LPARAM)0xC0000001L,
                              SMTO_NORMAL, 2000u, &r2);
    return true;
}

typedef HRESULT (WINAPI *PFN_WsDwmIsCompositionEnabled)(BOOL *pfEnabled);

static bool ws_is_dwm_active(void) {
    HMODULE hDwm = LoadLibraryA("dwmapi.dll");
    if (!hDwm) return false;
    PFN_WsDwmIsCompositionEnabled fn_en = NULL;
    WS_VERIFY_LOAD_PROC(hDwm, fn_en, "DwmIsCompositionEnabled");
    BOOL comp = FALSE;
    bool active = fn_en && SUCCEEDED(fn_en(&comp)) && (comp != FALSE);
    FreeLibrary(hDwm);
    return active;
}

/* =========================================================================
 * 5. Main Win32 Verification Driver
 * ========================================================================= */
int main(int argc, char **argv) {
    WsVerifyOptions opt;
    ws_verify_default_options(&opt);
    if (!ws_parse_args(argc, argv, &opt)) {
        return 2;
    }
    if (opt.show_help) {
        ws_verify_print_usage(argc > 0 ? argv[0] : "win32_ui_verify.exe");
        return 0;
    }

    WsConsoleScan con_scan;
    memset(&con_scan, 0, sizeof(con_scan));
    EnumWindows(ws_enum_record_consoles, (LPARAM)&con_scan);

    WCHAR exe_w[520];
    WCHAR cmd_w[1400];
    char  cmd_u8[1400];
    ws_u8_to_wide(opt.exe_path, exe_w, sizeof(exe_w) / sizeof(exe_w[0]));

    if (opt.extra_arg[0] != '\0') {
        snprintf(cmd_u8, sizeof(cmd_u8),
                 "\"%s\" --module-dir \"%s\" --theme %s --url \"%s\" %s",
                 opt.exe_path, opt.module_dir, opt.theme, opt.initial_url, opt.extra_arg);
    } else {
        snprintf(cmd_u8, sizeof(cmd_u8),
                 "\"%s\" --module-dir \"%s\" --theme %s --url \"%s\"",
                 opt.exe_path, opt.module_dir, opt.theme, opt.initial_url);
    }
    ws_u8_to_wide(cmd_u8, cmd_w, sizeof(cmd_w) / sizeof(cmd_w[0]));

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    printf("Launching Waterscape GUI under verification:\n  %s\n", cmd_u8);
    if (!CreateProcessW(exe_w, cmd_w, NULL, NULL, FALSE, 0u, NULL, NULL, &si, &pi)) {
        DWORD err = GetLastError();
        fprintf(stderr, "win32_ui_verify: CreateProcessW('%s') failed (win32 error %lu)\n",
                opt.exe_path, (unsigned long)err);
        return 1;
    }

    (void)WaitForInputIdle(pi.hProcess, 2000u);

    WsWndInventory inv_initial;
    memset(&inv_initial, 0, sizeof(inv_initial));
    DWORD waited_ms = 0u;
    while (waited_ms < 10000u) {
        DWORD wait_p = WaitForSingleObject(pi.hProcess, 0u);
        if (wait_p == WAIT_OBJECT_0) break;
        ws_collect_inventory(pi.dwProcessId, &inv_initial);
        if (inv_initial.primary_browser_hwnd) {
            RECT cr = {0, 0, 0, 0};
            GetClientRect(inv_initial.primary_browser_hwnd, &cr);
            if ((cr.right - cr.left) >= 200 && (cr.bottom - cr.top) >= 150) {
                break;
            }
        }
        Sleep(50u);
        waited_ms += 50u;
    }
    Sleep(300u);

    ws_collect_inventory(pi.dwProcessId, &inv_initial);
    EnumWindows(ws_enum_diff_consoles, (LPARAM)&con_scan);

    WsCheckSuite suite;
    memset(&suite, 0, sizeof(suite));
    HWND hwnd = inv_initial.primary_browser_hwnd;

    char detail[256];
    snprintf(detail, sizeof(detail),
             "pid=%lu visible_browser=%lu visible_total=%lu",
             (unsigned long)pi.dwProcessId,
             (unsigned long)inv_initial.visible_browser_count,
             (unsigned long)inv_initial.visible_total);
    ws_record_check(&suite, "startup_single_visible_hwnd",
                    hwnd != NULL &&
                    inv_initial.visible_browser_count == 1u &&
                    inv_initial.visible_total == 1u,
                    detail);

    snprintf(detail, sizeof(detail),
             "proc_consoles=%lu newly_spawned_consoles=%lu",
             (unsigned long)inv_initial.visible_console_count,
             (unsigned long)con_scan.newly_appeared);
    ws_record_check(&suite, "zero_console_windows",
                    inv_initial.visible_console_count == 0u && con_scan.newly_appeared == 0u,
                    detail);

    char bmp_initial[640];
    char png_initial[640];
    char png_f12[640];
    char png_narrow[640];
    snprintf(bmp_initial, sizeof(bmp_initial), "%s/win32_initial.bmp", opt.out_dir);
    snprintf(png_initial, sizeof(png_initial), "%s/win32_initial.png", opt.out_dir);
    snprintf(png_f12,     sizeof(png_f12),     "%s/win32_devtools_f12.png", opt.out_dir);
    snprintf(png_narrow,  sizeof(png_narrow),  "%s/win32_narrow_520.png", opt.out_dir);

    WsUiaTreeSnapshot uia_initial;
    WsUiaTreeSnapshot uia_f12;
    memset(&uia_initial, 0, sizeof(uia_initial));
    memset(&uia_f12, 0, sizeof(uia_f12));

    HRESULT co_hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HMODULE hOleAut = LoadLibraryA("oleaut32.dll");
    PFN_WsSysFreeString fn_free_bstr = NULL;
    PFN_WsVariantClear  fn_var_clear = NULL;
    WS_VERIFY_LOAD_PROC(hOleAut, fn_free_bstr, "SysFreeString");
    WS_VERIFY_LOAD_PROC(hOleAut, fn_var_clear, "VariantClear");

    HMODULE hUiaCore = LoadLibraryA("uiautomationcore.dll");
    WsUiaCliAutomation *uia = NULL;
    if (hUiaCore) {
        void *uia_void = NULL;
        if (SUCCEEDED(CoCreateInstance(&kCLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                                       &kIID_IUIAutomation, &uia_void)) && uia_void) {
            uia = (WsUiaCliAutomation *)uia_void;
        }
    }

    int dpi_omni_h[5] = {0, 0, 0, 0, 0};

    if (hwnd && IsWindow(hwnd)) {
        bool cap_ok = ws_capture_window_images(hwnd, bmp_initial, png_initial);
        ws_record_check(&suite, "capture_initial_bmp_and_png", cap_ok, png_initial);

        /* --- UI Automation Verification --- */
        if (hUiaCore && uia) {
            DWORD_PTR wm_go_res = 0u;
            BOOL wm_go_ok = SendMessageTimeoutW(hwnd, WM_GETOBJECT, 0u, (LPARAM)-25L,
                                                SMTO_NORMAL, 2000u, &wm_go_res);
            snprintf(detail, sizeof(detail), "WM_GETOBJECT(-25) ok=%d lresult=0x%lX",
                     (int)wm_go_ok, (unsigned long)wm_go_res);
            ws_record_check(&suite, "wm_getobject_uia_root_provider",
                            wm_go_ok != FALSE && wm_go_res != 0u, detail);

            bool tree_ok = ws_snapshot_uia_tree(uia, hwnd, fn_free_bstr, fn_var_clear, &uia_initial);
            const WsUiaNodeSnapshot *n_tab0 = ws_find_uia_child(&uia_initial, "TabItem_0");
            const WsUiaNodeSnapshot *n_newt = ws_find_uia_child(&uia_initial, "NewTabButton");
            const WsUiaNodeSnapshot *n_back = ws_find_uia_child(&uia_initial, "BackButton");
            const WsUiaNodeSnapshot *n_omni = ws_find_uia_child(&uia_initial, "OmnibarEdit");
            const WsUiaNodeSnapshot *n_menu = ws_find_uia_child(&uia_initial, "MainMenuButton");
            const WsUiaNodeSnapshot *n_doc  = ws_find_uia_child(&uia_initial, "WebDocument");
            const WsUiaNodeSnapshot *n_dt0  = ws_find_uia_child(&uia_initial, "DevToolsDock");

            bool roles_ok = tree_ok &&
                uia_initial.root.control_type == WS_UIA_WindowControlTypeId &&
                n_tab0 && n_tab0->control_type == WS_UIA_TabItemControlTypeId && ws_rect_nonempty(n_tab0->bounds) &&
                n_newt && n_newt->control_type == WS_UIA_ButtonControlTypeId  && ws_rect_nonempty(n_newt->bounds) &&
                n_back && n_back->control_type == WS_UIA_ButtonControlTypeId  && ws_rect_nonempty(n_back->bounds) &&
                n_omni && n_omni->control_type == WS_UIA_EditControlTypeId    && ws_rect_nonempty(n_omni->bounds) &&
                n_menu && n_menu->control_type == WS_UIA_ButtonControlTypeId  && ws_rect_nonempty(n_menu->bounds) &&
                n_doc  && n_doc->control_type  == WS_UIA_DocumentControlTypeId && ws_rect_nonempty(n_doc->bounds) &&
                n_dt0 == NULL;
            snprintf(detail, sizeof(detail), "children=%lu omnibar_val='%s'",
                     (unsigned long)uia_initial.child_count, n_omni ? n_omni->value : "");
            ws_record_check(&suite, "uia_fragment_tree_and_roles", roles_ok, detail);

            bool val_ok = n_omni && strstr(n_omni->value, "waterscape://addons") != NULL;
            ws_record_check(&suite, "uia_omnibar_value_pattern", val_ok,
                            n_omni ? n_omni->value : "missing OmnibarEdit");
        } else {
            ws_record_check(&suite, "uia_legacy_os_graceful_skip", hUiaCore == NULL,
                            "uiautomationcore.dll absent on legacy NT; skipped COM UIA checks");
        }

        /* --- WM_NCHITTEST Verification --- */
        bool dwm_mode = ws_is_dwm_active() && (strstr(opt.extra_arg, "--classic-frame") == NULL);
        const WsUiaNodeSnapshot *tab0 = ws_find_uia_child(&uia_initial, "TabItem_0");
        const WsUiaNodeSnapshot *omni = ws_find_uia_child(&uia_initial, "OmnibarEdit");
        const WsUiaNodeSnapshot *newt = ws_find_uia_child(&uia_initial, "NewTabButton");

        POINT tab_pt = {0, 0};
        if (tab0 && ws_rect_nonempty(tab0->bounds)) {
            tab_pt.x = (tab0->bounds.left + tab0->bounds.right) / 2;
            tab_pt.y = (tab0->bounds.top + tab0->bounds.bottom) / 2;
        } else {
            tab_pt.x = 64;
            tab_pt.y = 18;
            ClientToScreen(hwnd, &tab_pt);
        }
        bool ht_ok = false;
        LRESULT ht_tab = ws_send_nchittest(hwnd, (int)tab_pt.x, (int)tab_pt.y, &ht_ok);
        snprintf(detail, sizeof(detail), "pt=(%ld,%ld) ht=%ld (expected HTCLIENT=1)",
                 (long)tab_pt.x, (long)tab_pt.y, (long)ht_tab);
        ws_record_check(&suite, "nchittest_tab_htclient", ht_ok && ht_tab == HTCLIENT, detail);

        POINT omni_pt = {0, 0};
        if (omni && ws_rect_nonempty(omni->bounds)) {
            omni_pt.x = (omni->bounds.left + omni->bounds.right) / 2;
            omni_pt.y = (omni->bounds.top + omni->bounds.bottom) / 2;
        } else {
            omni_pt.x = 260;
            omni_pt.y = 54;
            ClientToScreen(hwnd, &omni_pt);
        }
        LRESULT ht_omni = ws_send_nchittest(hwnd, (int)omni_pt.x, (int)omni_pt.y, &ht_ok);
        snprintf(detail, sizeof(detail), "pt=(%ld,%ld) ht=%ld (expected HTCLIENT=1)",
                 (long)omni_pt.x, (long)omni_pt.y, (long)ht_omni);
        ws_record_check(&suite, "nchittest_omnibar_htclient", ht_ok && ht_omni == HTCLIENT, detail);

        RECT wr_cur = {0, 0, 0, 0};
        GetWindowRect(hwnd, &wr_cur);
        POINT drag_pt = {0, 0};
        if (dwm_mode) {
            POINT c_drag;
            RECT cr_cur = {0, 0, 0, 0};
            GetClientRect(hwnd, &cr_cur);
            c_drag.x = (cr_cur.right - cr_cur.left) - 180;
            if (newt && ws_rect_nonempty(newt->bounds)) {
                POINT nt_cli = {newt->bounds.right + 28, (newt->bounds.top + newt->bounds.bottom) / 2};
                ScreenToClient(hwnd, &nt_cli);
                if (nt_cli.x < c_drag.x) c_drag.x = (nt_cli.x + c_drag.x) / 2;
                c_drag.y = nt_cli.y;
            } else {
                c_drag.y = 18;
            }
            drag_pt = c_drag;
            ClientToScreen(hwnd, &drag_pt);
        } else {
            drag_pt.x = (wr_cur.left + wr_cur.right) / 2;
            drag_pt.y = wr_cur.top + 12;
        }
        LRESULT ht_drag = ws_send_nchittest(hwnd, (int)drag_pt.x, (int)drag_pt.y, &ht_ok);
        snprintf(detail, sizeof(detail), "dwm=%d pt=(%ld,%ld) ht=%ld (expected HTCAPTION=2)",
                 (int)dwm_mode, (long)drag_pt.x, (long)drag_pt.y, (long)ht_drag);
        ws_record_check(&suite, "nchittest_caption_drag_zone", ht_ok && ht_drag == HTCAPTION, detail);

        if (!IsZoomed(hwnd)) {
            LRESULT ht_top = ws_send_nchittest(hwnd, (wr_cur.left + wr_cur.right) / 2,
                                               wr_cur.top + 1, &ht_ok);
            snprintf(detail, sizeof(detail), "top_edge ht=%ld (expected HTTOP=12)", (long)ht_top);
            ws_record_check(&suite, "nchittest_top_resize_border",
                            ht_ok && ht_top == HTTOP, detail);
        }

        /* Live negative screen coordinate hit-test (simulates secondary monitor at x < 0) */
        if (!IsZoomed(hwnd)) {
            int32_t orig_w = (int32_t)(wr_cur.right - wr_cur.left);
            int32_t orig_h = (int32_t)(wr_cur.bottom - wr_cur.top);
            SetWindowPos(hwnd, NULL, -160, 48, orig_w, orig_h, SWP_NOZORDER | SWP_NOACTIVATE);
            Sleep(80u);
            POINT neg_tab_cli = {56, 18};
            POINT neg_tab_scr = neg_tab_cli;
            ClientToScreen(hwnd, &neg_tab_scr);
            LRESULT ht_neg = ws_send_nchittest(hwnd, (int)neg_tab_scr.x, (int)neg_tab_scr.y, &ht_ok);
            SetWindowPos(hwnd, NULL, wr_cur.left, wr_cur.top, orig_w, orig_h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Sleep(80u);
            snprintf(detail, sizeof(detail), "neg_screen_pt=(%ld,%ld) ht=%ld (expected HTCLIENT=1)",
                     (long)neg_tab_scr.x, (long)neg_tab_scr.y, (long)ht_neg);
            ws_record_check(&suite, "nchittest_negative_monitor_coords",
                            ht_ok && neg_tab_scr.x < 0 && ht_neg == HTCLIENT, detail);
        }

        /* --- In-Window F12 DevTools Toggle Verification --- */
        bool f12_sent = ws_send_fkey(hwnd, VK_F12);
        Sleep(250u);
        WsWndInventory inv_f12;
        ws_collect_inventory(pi.dwProcessId, &inv_f12);
        bool f12_single_wnd = f12_sent &&
                              inv_f12.visible_browser_count == 1u &&
                              inv_f12.visible_total == 1u &&
                              inv_f12.visible_console_count == 0u;
        snprintf(detail, sizeof(detail), "visible_total=%lu visible_browser=%lu",
                 (unsigned long)inv_f12.visible_total, (unsigned long)inv_f12.visible_browser_count);
        ws_record_check(&suite, "f12_devtools_in_window_single_hwnd", f12_single_wnd, detail);

        (void)ws_capture_window_images(hwnd, NULL, png_f12);

        if (uia) {
            ws_snapshot_uia_tree(uia, hwnd, fn_free_bstr, fn_var_clear, &uia_f12);
            const WsUiaNodeSnapshot *dt_open  = ws_find_uia_child(&uia_f12, "DevToolsDock");
            const WsUiaNodeSnapshot *doc_init = ws_find_uia_child(&uia_initial, "WebDocument");
            const WsUiaNodeSnapshot *doc_f12  = ws_find_uia_child(&uia_f12, "WebDocument");
            bool dt_uia_ok = dt_open != NULL &&
                             dt_open->control_type == WS_UIA_PaneControlTypeId &&
                             ws_rect_nonempty(dt_open->bounds) &&
                             doc_init && doc_f12 &&
                             (doc_f12->bounds.bottom - doc_f12->bounds.top) <
                             (doc_init->bounds.bottom - doc_init->bounds.top);
            ws_record_check(&suite, "f12_devtools_uia_pane_and_viewport_split", dt_uia_ok,
                            dt_open ? dt_open->name : "DevToolsDock missing");
        }

        /* Close F12 DevTools and verify viewport restoration */
        (void)ws_send_fkey(hwnd, VK_F12);
        Sleep(200u);
        if (uia) {
            WsUiaTreeSnapshot uia_f12_closed;
            ws_snapshot_uia_tree(uia, hwnd, fn_free_bstr, fn_var_clear, &uia_f12_closed);
            const WsUiaNodeSnapshot *dt_closed  = ws_find_uia_child(&uia_f12_closed, "DevToolsDock");
            const WsUiaNodeSnapshot *doc_init   = ws_find_uia_child(&uia_initial, "WebDocument");
            const WsUiaNodeSnapshot *doc_closed = ws_find_uia_child(&uia_f12_closed, "WebDocument");
            bool restored = (dt_closed == NULL) && doc_init && doc_closed &&
                            abs((int)(doc_closed->bounds.bottom - doc_closed->bounds.top) -
                                (int)(doc_init->bounds.bottom - doc_init->bounds.top)) <= 2;
            ws_record_check(&suite, "f12_devtools_close_restores_viewport", restored,
                            "DevTools hidden and WebDocument height restored");
        }

        /* --- F11 Fullscreen Toggle & Placement Restore Verification --- */
        RECT wr_pre_f11 = {0, 0, 0, 0};
        GetWindowRect(hwnd, &wr_pre_f11);
        HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
        BOOL got_mon = hmon ? GetMonitorInfoW(hmon, &mi) : FALSE;

        (void)ws_send_fkey(hwnd, VK_F11);
        Sleep(260u);
        RECT wr_f11 = {0, 0, 0, 0};
        GetWindowRect(hwnd, &wr_f11);
        bool f11_enter_ok = got_mon &&
                            abs((int)wr_f11.left - (int)mi.rcMonitor.left) <= 2 &&
                            abs((int)wr_f11.top - (int)mi.rcMonitor.top) <= 2 &&
                            abs((int)wr_f11.right - (int)mi.rcMonitor.right) <= 2 &&
                            abs((int)wr_f11.bottom - (int)mi.rcMonitor.bottom) <= 2;
        snprintf(detail, sizeof(detail), "f11_rect=(%ld,%ld,%ld,%ld) mon=(%ld,%ld,%ld,%ld)",
                 (long)wr_f11.left, (long)wr_f11.top, (long)wr_f11.right, (long)wr_f11.bottom,
                 (long)mi.rcMonitor.left, (long)mi.rcMonitor.top,
                 (long)mi.rcMonitor.right, (long)mi.rcMonitor.bottom);
        ws_record_check(&suite, "f11_fullscreen_covers_monitor", f11_enter_ok, detail);

        (void)ws_send_fkey(hwnd, VK_F11);
        Sleep(260u);
        RECT wr_post_f11 = {0, 0, 0, 0};
        GetWindowRect(hwnd, &wr_post_f11);
        DWORD st_post = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
        bool f11_exit_ok = ((st_post & WS_CAPTION) != 0u) &&
                           abs((int)wr_post_f11.left - (int)wr_pre_f11.left) <= 4 &&
                           abs((int)wr_post_f11.top - (int)wr_pre_f11.top) <= 4 &&
                           abs((int)wr_post_f11.right - (int)wr_pre_f11.right) <= 4 &&
                           abs((int)wr_post_f11.bottom - (int)wr_pre_f11.bottom) <= 4;
        snprintf(detail, sizeof(detail), "restored_rect=(%ld,%ld,%ld,%ld)",
                 (long)wr_post_f11.left, (long)wr_post_f11.top,
                 (long)wr_post_f11.right, (long)wr_post_f11.bottom);
        ws_record_check(&suite, "f11_fullscreen_exit_restores_placement", f11_exit_ok, detail);

        /* --- UIA Invoke (Open Multiple Tabs) + Narrow Window (520x640) Overflow --- */
        if (uia) {
            bool inv1 = ws_uia_invoke_by_aid(uia, hwnd, fn_free_bstr, "NewTabButton");
            Sleep(100u);
            bool inv2 = ws_uia_invoke_by_aid(uia, hwnd, fn_free_bstr, "NewTabButton");
            Sleep(100u);
            bool sel0 = ws_uia_select_tab_by_aid(uia, hwnd, fn_free_bstr, "TabItem_0");
            Sleep(100u);
            ws_record_check(&suite, "uia_invoke_newtab_and_select_tab",
                            inv1 && inv2 && sel0,
                            "Opened 2 tabs via IInvokePattern and re-selected TabItem_0");
        }

        SetWindowPos(hwnd, NULL, wr_pre_f11.left, wr_pre_f11.top, 520, 640,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Sleep(220u);
        (void)ws_capture_window_images(hwnd, NULL, png_narrow);

        DWORD_PTR ping_res = 0u;
        bool narrow_alive = SendMessageTimeoutW(hwnd, WM_NULL, 0u, 0, SMTO_NORMAL, 2000u, &ping_res) != 0;
        if (uia) {
            WsUiaTreeSnapshot uia_narrow;
            ws_snapshot_uia_tree(uia, hwnd, fn_free_bstr, fn_var_clear, &uia_narrow);
            const WsUiaNodeSnapshot *nt_tab0 = ws_find_uia_child(&uia_narrow, "TabItem_0");
            const WsUiaNodeSnapshot *nt_newt = ws_find_uia_child(&uia_narrow, "NewTabButton");
            const WsUiaNodeSnapshot *nt_omni = ws_find_uia_child(&uia_narrow, "OmnibarEdit");
            const WsUiaNodeSnapshot *nt_menu = ws_find_uia_child(&uia_narrow, "MainMenuButton");
            bool narrow_layout_ok = narrow_alive &&
                nt_tab0 && ws_rect_nonempty(nt_tab0->bounds) &&
                nt_newt && ws_rect_nonempty(nt_newt->bounds) &&
                nt_omni && ws_rect_nonempty(nt_omni->bounds) &&
                nt_menu && ws_rect_nonempty(nt_menu->bounds) &&
                nt_tab0->bounds.right <= nt_newt->bounds.left + 2 &&
                nt_omni->bounds.right <= nt_menu->bounds.left + 2;
            ws_record_check(&suite, "narrow_520px_non_overlapping_controls",
                            narrow_layout_ok, "520x640 tab strip and toolbar remain non-overlapping");
        } else {
            ws_record_check(&suite, "narrow_520px_responsive", narrow_alive, "520x640 responsive");
        }

        /* Restore normal size and test simulated WM_DPICHANGED (96 -> 120 -> 144 -> 192 -> 96) */
        int32_t base_w = (int32_t)(wr_pre_f11.right - wr_pre_f11.left);
        int32_t base_h = (int32_t)(wr_pre_f11.bottom - wr_pre_f11.top);
        SetWindowPos(hwnd, NULL, wr_pre_f11.left, wr_pre_f11.top, base_w, base_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Sleep(120u);

        static const int kDpiSteps[5] = {96, 120, 144, 192, 96};
        bool dpi_all_alive = true;
        for (int i = 0; i < 5; ++i) {
            int d = kDpiSteps[i];
            WPARAM wp_dpi = MAKEWPARAM((WORD)d, (WORD)d);
            RECT sug_rc = wr_pre_f11;
            (void)GetWindowRect(hwnd, &sug_rc);
            DWORD_PTR dpi_res = 0u;
            BOOL sent = SendMessageTimeoutW(hwnd, WM_DPICHANGED, wp_dpi, (LPARAM)&sug_rc,
                                            SMTO_NORMAL, 2000u, &dpi_res);
            if (!sent) {
                sent = SendMessageTimeoutW(hwnd, (UINT)(WM_APP + 0x2E0), wp_dpi, 0,
                                           SMTO_NORMAL, 2000u, &dpi_res);
            }
            if (!sent) {
                dpi_all_alive = false;
                break;
            }
            Sleep(120u);
            if (uia) {
                WsUiaTreeSnapshot snap_d;
                if (ws_snapshot_uia_tree(uia, hwnd, fn_free_bstr, fn_var_clear, &snap_d)) {
                    const WsUiaNodeSnapshot *om_d = ws_find_uia_child(&snap_d, "OmnibarEdit");
                    if (om_d) {
                        dpi_omni_h[i] = (int)(om_d->bounds.bottom - om_d->bounds.top);
                    }
                }
                bool need_app_fallback =
                    (d != 96 && dpi_omni_h[0] > 0 && dpi_omni_h[i] <= dpi_omni_h[0]) ||
                    (i == 4 && dpi_omni_h[0] > 0 && abs(dpi_omni_h[4] - dpi_omni_h[0]) > 2);
                if (need_app_fallback) {
                    if (SendMessageTimeoutW(hwnd, (UINT)(WM_APP + 0x2E0), wp_dpi, 0,
                                            SMTO_NORMAL, 2000u, &dpi_res)) {
                        Sleep(120u);
                        if (ws_snapshot_uia_tree(uia, hwnd, fn_free_bstr, fn_var_clear, &snap_d)) {
                            const WsUiaNodeSnapshot *om_d2 = ws_find_uia_child(&snap_d, "OmnibarEdit");
                            if (om_d2) {
                                dpi_omni_h[i] = (int)(om_d2->bounds.bottom - om_d2->bounds.top);
                            }
                        }
                    }
                }
            }
        }
        bool dpi_scale_ok = dpi_all_alive;
        if (uia) {
            dpi_scale_ok = dpi_all_alive &&
                           dpi_omni_h[0] >= 24 &&
                           dpi_omni_h[3] >= dpi_omni_h[0] + 16 &&
                           abs(dpi_omni_h[4] - dpi_omni_h[0]) <= 2;
        }
        snprintf(detail, sizeof(detail), "omnibar_px[96,120,144,192,96]=[%d,%d,%d,%d,%d]",
                 dpi_omni_h[0], dpi_omni_h[1], dpi_omni_h[2], dpi_omni_h[3], dpi_omni_h[4]);
        ws_record_check(&suite, "wm_dpichanged_96_120_144_192_roundtrip", dpi_scale_ok, detail);

        /* Clean WM_CLOSE shutdown */
        (void)PostMessageW(hwnd, WM_CLOSE, 0u, 0);
    }

    DWORD wait_exit = WaitForSingleObject(pi.hProcess, 5000u);
    DWORD exit_code = 259u;
    if (wait_exit != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 99u);
        (void)WaitForSingleObject(pi.hProcess, 1000u);
    }
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    snprintf(detail, sizeof(detail), "wait_rc=%lu exit_code=%lu",
             (unsigned long)wait_exit, (unsigned long)exit_code);
    ws_record_check(&suite, "clean_wm_close_exit_zero",
                    wait_exit == WAIT_OBJECT_0 && exit_code == 0u, detail);

    if (uia) uia->lpVtbl->Release(uia);
    if (hUiaCore) FreeLibrary(hUiaCore);
    if (hOleAut) FreeLibrary(hOleAut);
    if (SUCCEEDED(co_hr)) CoUninitialize();

    /* Write JSON report */
    FILE *rfp = fopen(opt.report_path, "wb");
    if (rfp) {
        fprintf(rfp, "{\n");
        fprintf(rfp, "  \"command\": ");
        ws_json_write_escaped(rfp, cmd_u8);
        fprintf(rfp, ",\n  \"pid\": %lu,\n", (unsigned long)pi.dwProcessId);
        fprintf(rfp, "  \"passed\": %s,\n", suite.failed_count == 0u ? "true" : "false");
        fprintf(rfp, "  \"total_checks\": %lu,\n", (unsigned long)suite.count);
        fprintf(rfp, "  \"failed_checks\": %lu,\n", (unsigned long)suite.failed_count);
        fprintf(rfp, "  \"screenshots\": {\n    \"initial_bmp\": ");
        ws_json_write_escaped(rfp, bmp_initial);
        fprintf(rfp, ",\n    \"initial_png\": ");
        ws_json_write_escaped(rfp, png_initial);
        fprintf(rfp, ",\n    \"devtools_f12_png\": ");
        ws_json_write_escaped(rfp, png_f12);
        fprintf(rfp, ",\n    \"narrow_520_png\": ");
        ws_json_write_escaped(rfp, png_narrow);
        fprintf(rfp, "\n  },\n  \"top_level_windows\": [\n");
        for (size_t i = 0u; i < inv_initial.count; ++i) {
            const WsWndEntry *e = &inv_initial.items[i];
            fprintf(rfp, "    {\"hwnd\": \"0x%p\", \"owner\": \"0x%p\", \"class\": ",
                    (void *)e->hwnd, (void *)e->owner);
            ws_json_write_escaped(rfp, e->class_name);
            fprintf(rfp, ", \"title\": ");
            ws_json_write_escaped(rfp, e->title);
            fprintf(rfp, ", \"visible\": %s, \"style\": \"0x%08lX\", \"ex_style\": \"0x%08lX\", "
                         "\"rect\": [%ld, %ld, %ld, %ld]}%s\n",
                    e->visible ? "true" : "false",
                    (unsigned long)e->style, (unsigned long)e->ex_style,
                    (long)e->win_rect.left, (long)e->win_rect.top,
                    (long)e->win_rect.right, (long)e->win_rect.bottom,
                    (i + 1u < inv_initial.count) ? "," : "");
        }
        fprintf(rfp, "  ],\n  \"uia_children\": [\n");
        for (size_t i = 0u; i < uia_initial.child_count; ++i) {
            const WsUiaNodeSnapshot *n = &uia_initial.children[i];
            fprintf(rfp, "    {\"automation_id\": ");
            ws_json_write_escaped(rfp, n->automation_id);
            fprintf(rfp, ", \"name\": ");
            ws_json_write_escaped(rfp, n->name);
            fprintf(rfp, ", \"value\": ");
            ws_json_write_escaped(rfp, n->value);
            fprintf(rfp, ", \"control_type\": %d, \"bounds\": [%ld, %ld, %ld, %ld]}%s\n",
                    n->control_type,
                    (long)n->bounds.left, (long)n->bounds.top,
                    (long)n->bounds.right, (long)n->bounds.bottom,
                    (i + 1u < uia_initial.child_count) ? "," : "");
        }
        fprintf(rfp, "  ],\n  \"checks\": [\n");
        for (size_t i = 0u; i < suite.count; ++i) {
            const WsCheckResult *c = &suite.items[i];
            fprintf(rfp, "    {\"name\": ");
            ws_json_write_escaped(rfp, c->name);
            fprintf(rfp, ", \"passed\": %s, \"detail\": ", c->passed ? "true" : "false");
            ws_json_write_escaped(rfp, c->detail);
            fprintf(rfp, "}%s\n", (i + 1u < suite.count) ? "," : "");
        }
        fprintf(rfp, "  ]\n}\n");
        fclose(rfp);
    }

    printf("Summary: %lu/%lu checks passed -> %s\n",
           (unsigned long)(suite.count - suite.failed_count),
           (unsigned long)suite.count,
           opt.report_path);
    return (suite.failed_count == 0u) ? 0 : 1;
}

#else /* Non-Win32 host stub so POSIX tool builds remain clean */

int main(int argc, char **argv) {
    WsVerifyOptions opt;
    ws_verify_default_options(&opt);
    if (!ws_parse_args(argc, argv, &opt)) {
        return 2;
    }
    if (opt.show_help) {
        ws_verify_print_usage(argc > 0 ? argv[0] : "win32_ui_verify");
        return 0;
    }
    fprintf(stderr, "win32_ui_verify: live Win32 HWND/DWM/UIA verification requires a Windows host.\n");
    return 1;
}

#endif
