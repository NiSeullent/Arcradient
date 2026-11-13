/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * installer/win32/waterscape_setup.c - Pure C11 Win32 GUI & Silent SFX Setup Wizard
 *
 * Compatible with Windows 2000 SP4 (`WINVER=0x0500`) through Windows 11 (x86 & x64).
 * Zero Vista+/XP-only DLL imports; uses only classic kernel32, user32, gdi32,
 * advapi32, shell32, ole32, and comctl32 APIs.
 *
 * Supported CLI switches:
 *   --help / -h / /?       Print usage and exit 0
 *   --verify-stub          Validate stub or embedded WSSFX100 payload and exit 0
 *   --silent / /S          Run unattended headless installation
 *   --dir <install_path>   Override target installation directory
 *   --no-shortcuts         Skip Desktop & Start Menu .lnk creation
 *   --no-registry          Skip HKCU Uninstall / file association registration
 *   --no-addons            Skip extracting bundled addons/<ext>.wsx extensions
 *   --register-assoc       Register HKCU StartMenuInternet & .htm/.html/http/https
 */
#if defined(_WIN32)
#  ifndef WINVER
#    define WINVER 0x0500
#  endif
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0500
#  endif
#  ifndef _WIN32_IE
#    define _WIN32_IE 0x0500
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
#  include <commctrl.h>
#  include <shellapi.h>
#  include <ole2.h>
#  include <shlobj.h>
#  include <objbase.h>
#else
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  include <errno.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#include "../../base/arcr_base.h"
#include "../../base/arcr_codec.h"
#include "../../include/arcr_module.h"
#include "../../include/arcradient_embed.h"
#include "waterscape_sfx_format.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * 1. Shared Cross-Platform Options, Manifest & Path Sanitization
 * ========================================================================= */
typedef struct WsSetupOptions {
    bool show_help;
    bool verify_stub;
    bool silent;
    bool install_addons;
    bool create_desktop_shortcut;
    bool create_startmenu_shortcut;
    bool write_registry;
    bool register_associations;
    char custom_dir_utf8[512];
} WsSetupOptions;

static void ws_setup_default_options(WsSetupOptions *opt) {
    memset(opt, 0, sizeof(*opt));
    opt->install_addons            = true;
    opt->create_desktop_shortcut   = true;
    opt->create_startmenu_shortcut = true;
    opt->write_registry            = true;
    opt->register_associations     = false;
}

static bool ws_is_safe_rel_path(const char *rel) {
    if (!rel || rel[0] == '\0' || rel[0] == '/' || rel[0] == '\\') {
        return false;
    }
    for (size_t i = 0u; rel[i] != '\0'; ++i) {
        if (rel[i] == ':') return false;
        if (rel[i] == '.' && rel[i + 1u] == '.') {
            if ((i == 0u || rel[i - 1u] == '/' || rel[i - 1u] == '\\') &&
                (rel[i + 2u] == '\0' || rel[i + 2u] == '/' || rel[i + 2u] == '\\')) {
                return false;
            }
        }
    }
    return true;
}

static bool ws_is_addon_entry(const char *rel) {
    return arcr_str_starts_with_nocase(rel, "addons/") ||
           arcr_str_starts_with_nocase(rel, "addons\\");
}

static void ws_print_usage(const char *prog) {
    printf("Waterscape Browser %s Setup (Arcradient Pure C11 Engine)\n"
           "Usage: %s [options]\n"
           "  --help, -h, /?         Show this help message and exit 0\n"
           "  --verify-stub          Verify stub or embedded WSSFX100 archive and exit 0\n"
           "  --silent, /S           Headless unattended installation\n"
           "  --dir <install_path>   Set target installation directory\n"
           "  --no-shortcuts         Do not create Desktop or Start Menu shortcuts\n"
           "  --no-registry          Do not write HKCU Uninstall/Client registry keys\n"
           "  --no-addons            Skip bundled official .wsx extensions\n"
           "  --register-assoc       Register HKCU HTML/HTTP/HTTPS associations\n",
           WATERSCAPE_VERSION_STR,
           prog ? prog : "waterscape_setup.exe");
}

static void ws_parse_cli_utf8(int argc, char **argv, WsSetupOptions *opt) {
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!arg) continue;
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0 || strcmp(arg, "/?") == 0) {
            opt->show_help = true;
        } else if (strcmp(arg, "--verify-stub") == 0) {
            opt->verify_stub = true;
        } else if (arcr_str_eq_nocase(arg, "--silent") || strcmp(arg, "/S") == 0 ||
                   strcmp(arg, "/s") == 0) {
            opt->silent = true;
        } else if (strcmp(arg, "--no-shortcuts") == 0) {
            opt->create_desktop_shortcut   = false;
            opt->create_startmenu_shortcut = false;
        } else if (strcmp(arg, "--no-registry") == 0) {
            opt->write_registry        = false;
            opt->register_associations = false;
        } else if (strcmp(arg, "--no-addons") == 0) {
            opt->install_addons = false;
        } else if (strcmp(arg, "--register-assoc") == 0) {
            opt->register_associations = true;
        } else if (strcmp(arg, "--dir") == 0 && i + 1 < argc) {
            arcr_str_copy(opt->custom_dir_utf8, sizeof(opt->custom_dir_utf8), argv[++i]);
        } else if (arcr_str_starts_with_nocase(arg, "--dir=")) {
            arcr_str_copy(opt->custom_dir_utf8, sizeof(opt->custom_dir_utf8), arg + 6);
        } else if (arcr_str_starts_with_nocase(arg, "/D=")) {
            arcr_str_copy(opt->custom_dir_utf8, sizeof(opt->custom_dir_utf8), arg + 3);
        }
    }
}

/* =========================================================================
 * 2. Native Win32 Implementation (Windows 2000 SP4 - Windows 11)
 * ========================================================================= */
#if defined(_WIN32)

#ifndef CSIDL_PROGRAMS
#  define CSIDL_PROGRAMS 0x0002
#endif
#ifndef CSIDL_DESKTOPDIRECTORY
#  define CSIDL_DESKTOPDIRECTORY 0x0010
#endif
#ifndef CSIDL_APPDATA
#  define CSIDL_APPDATA 0x001a
#endif
#ifndef CSIDL_LOCAL_APPDATA
#  define CSIDL_LOCAL_APPDATA 0x001c
#endif
#ifndef BIF_NEWDIALOGSTYLE
#  define BIF_NEWDIALOGSTYLE 0x0040
#endif
#ifndef PBM_SETRANGE32
#  define PBM_SETRANGE32 (WM_USER + 6)
#endif
#ifndef PROCESSOR_ARCHITECTURE_AMD64
#  define PROCESSOR_ARCHITECTURE_AMD64 9
#endif
#ifndef PROCESSOR_ARCHITECTURE_ARM64
#  define PROCESSOR_ARCHITECTURE_ARM64 12
#endif
#ifndef INVALID_SET_FILE_POINTER
#  define INVALID_SET_FILE_POINTER ((DWORD)-1)
#endif

/* Explicit COM GUIDs so linking never requires external uuid.lib */
static const GUID kWsCLSID_ShellLink = {
    0x00021401u, 0x0000u, 0x0000u, {0xC0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x46u}
};
static const GUID kWsIID_IShellLinkW = {
    0x000214F9u, 0x0000u, 0x0000u, {0xC0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x46u}
};
static const GUID kWsIID_IPersistFile = {
    0x0000010Bu, 0x0000u, 0x0000u, {0xC0u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x46u}
};
static const GUID kWsIID_IPropertyStore = {
    0x886D8EEBu, 0x8CF2u, 0x4446u, {0x8Du, 0x02u, 0xCDu, 0xBAu, 0x1Du, 0xBDu, 0xCFu, 0x99u}
};

#define WS_SHORTCUT_APP_USER_MODEL_ID L"Waterscape.Browser.2"
#define WS_VT_LPWSTR                  ((USHORT)31u)

typedef struct WsPropertyKey {
    GUID  fmtid;
    DWORD pid;
} WsPropertyKey;

/* System.AppUserModel.ID = {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}, 5 */
static const WsPropertyKey kWsPKEY_AppUserModel_ID = {
    {0x9F4C2855u, 0x9F79u, 0x4B39u, {0xA8u, 0xD0u, 0xE1u, 0xD4u, 0x2Du, 0xE1u, 0xD5u, 0xF3u}},
    5u
};

typedef struct WsPropVariant {
    USHORT vt;
    WORD   wReserved1;
    WORD   wReserved2;
    WORD   wReserved3;
    union {
        LPCWSTR pwszVal;
        struct {
            ULONG cElems;
            void *pElems;
        } ca;
        ULONGLONG uhVal;
    } u;
} WsPropVariant;

typedef struct WsPropertyStore WsPropertyStore;
typedef struct WsPropertyStoreVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WsPropertyStore *This, REFIID riid, void **ppvObject);
    ULONG   (STDMETHODCALLTYPE *AddRef)(WsPropertyStore *This);
    ULONG   (STDMETHODCALLTYPE *Release)(WsPropertyStore *This);
    HRESULT (STDMETHODCALLTYPE *GetCount)(WsPropertyStore *This, DWORD *cProps);
    HRESULT (STDMETHODCALLTYPE *GetAt)(WsPropertyStore *This, DWORD iProp, WsPropertyKey *pkey);
    HRESULT (STDMETHODCALLTYPE *GetValue)(WsPropertyStore *This, const WsPropertyKey *key, WsPropVariant *pv);
    HRESULT (STDMETHODCALLTYPE *SetValue)(WsPropertyStore *This, const WsPropertyKey *key, const WsPropVariant *propvar);
    HRESULT (STDMETHODCALLTYPE *Commit)(WsPropertyStore *This);
} WsPropertyStoreVtbl;

struct WsPropertyStore {
    const WsPropertyStoreVtbl *lpVtbl;
};

/* Control IDs for the 4-Page Win32 Setup Wizard */
#define IDC_BTN_BACK       1001
#define IDC_BTN_NEXT       1002
#define IDC_BTN_CANCEL     1003
#define IDC_BTN_BROWSE     1004
#define IDC_CHK_ACCEPT_LIC 1005
#define IDC_EDIT_DIR       1006
#define IDC_CHK_CORE       1007
#define IDC_CHK_ADDONS     1008
#define IDC_CHK_DESKTOP    1009
#define IDC_CHK_STARTMENU  1010
#define IDC_CHK_ASSOC      1011
#define IDC_CHK_LAUNCH     1012

static size_t ws_wlen(const WCHAR *s) {
    size_t n = 0u;
    if (!s) return 0u;
    while (s[n] != L'\0') ++n;
    return n;
}

static void ws_wcopy(WCHAR *dst, size_t cap, const WCHAR *src) {
    if (!dst || cap == 0u) return;
    if (!src) {
        dst[0] = L'\0';
        return;
    }
    size_t i = 0u;
    while (i + 1u < cap && src[i] != L'\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = L'\0';
}

static void ws_wcat(WCHAR *dst, size_t cap, const WCHAR *suffix) {
    if (!dst || !suffix || cap == 0u) return;
    size_t cur = ws_wlen(dst);
    if (cur + 1u >= cap) return;
    ws_wcopy(dst + cur, cap - cur, suffix);
}

static void ws_utf8_to_wide(const char *utf8, WCHAR *out_w, size_t cap) {
    if (!out_w || cap == 0u) return;
    out_w[0] = L'\0';
    if (!utf8 || utf8[0] == '\0') return;
    int n = MultiByteToWideChar(CP_UTF8, 0u, utf8, -1, out_w, (int)cap);
    if (n <= 0) {
        MultiByteToWideChar(CP_ACP, 0u, utf8, -1, out_w, (int)cap);
    }
    out_w[cap - 1u] = L'\0';
}

static void ws_wide_to_utf8(const WCHAR *wide, char *out_u8, size_t cap) {
    if (!out_u8 || cap == 0u) return;
    out_u8[0] = '\0';
    if (!wide || wide[0] == L'\0') return;
    WideCharToMultiByte(CP_UTF8, 0u, wide, -1, out_u8, (int)cap, NULL, NULL);
    out_u8[cap - 1u] = '\0';
}

/* Parses Unicode GetCommandLineW() so non-ASCII --dir paths work regardless of ANSI codepage */
static void ws_parse_win32_cmdline_wide(WsSetupOptions *opt) {
    const WCHAR *cmd = GetCommandLineW();
    if (!cmd) return;

    WCHAR token[520];
    int   arg_idx = 0;
    bool  expect_dir = false;

    const WCHAR *p = cmd;
    while (*p != L'\0') {
        while (*p == L' ' || *p == L'\t') ++p;
        if (*p == L'\0') break;

        size_t tlen = 0u;
        bool in_quotes = false;
        while (*p != L'\0' && (in_quotes || (*p != L' ' && *p != L'\t'))) {
            if (*p == L'"') {
                in_quotes = !in_quotes;
                ++p;
                continue;
            }
            if (tlen + 1u < (sizeof(token) / sizeof(token[0]))) {
                token[tlen++] = *p;
            }
            ++p;
        }
        token[tlen] = L'\0';

        if (arg_idx == 0) {
            ++arg_idx;
            continue;
        }
        ++arg_idx;

        char u8[1024];
        ws_wide_to_utf8(token, u8, sizeof(u8));

        if (expect_dir) {
            arcr_str_copy(opt->custom_dir_utf8, sizeof(opt->custom_dir_utf8), u8);
            expect_dir = false;
        } else if (strcmp(u8, "--dir") == 0) {
            expect_dir = true;
        } else if (arcr_str_starts_with_nocase(u8, "--dir=")) {
            arcr_str_copy(opt->custom_dir_utf8, sizeof(opt->custom_dir_utf8), u8 + 6);
        } else if (arcr_str_starts_with_nocase(u8, "/D=")) {
            /* NSIS-style /D=C:\Path swallows rest of line unquoted if needed */
            arcr_str_copy(opt->custom_dir_utf8, sizeof(opt->custom_dir_utf8), u8 + 3);
        } else if (strcmp(u8, "--help") == 0 || strcmp(u8, "-h") == 0 || strcmp(u8, "/?") == 0) {
            opt->show_help = true;
        } else if (strcmp(u8, "--verify-stub") == 0) {
            opt->verify_stub = true;
        } else if (arcr_str_eq_nocase(u8, "--silent") || strcmp(u8, "/S") == 0 ||
                   strcmp(u8, "/s") == 0) {
            opt->silent = true;
        } else if (strcmp(u8, "--no-shortcuts") == 0) {
            opt->create_desktop_shortcut   = false;
            opt->create_startmenu_shortcut = false;
        } else if (strcmp(u8, "--no-registry") == 0) {
            opt->write_registry        = false;
            opt->register_associations = false;
        } else if (strcmp(u8, "--no-addons") == 0) {
            opt->install_addons = false;
        } else if (strcmp(u8, "--register-assoc") == 0) {
            opt->register_associations = true;
        }
    }
}

static void ws_resolve_default_install_dir(WCHAR *out_dir, size_t cap) {
    WCHAR base[MAX_PATH];
    base[0] = L'\0';

    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n > 0u && n < MAX_PATH && base[0] != L'\0') {
        ws_wcopy(out_dir, cap, base);
        ws_wcat(out_dir, cap, L"\\Programs\\Waterscape");
        return;
    }

    if (SHGetSpecialFolderPathW(NULL, base, CSIDL_LOCAL_APPDATA, FALSE) && base[0] != L'\0') {
        ws_wcopy(out_dir, cap, base);
        ws_wcat(out_dir, cap, L"\\Programs\\Waterscape");
        return;
    }

    n = GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH);
    if (n > 0u && n < MAX_PATH && base[0] != L'\0') {
        ws_wcopy(out_dir, cap, base);
        ws_wcat(out_dir, cap, L"\\Waterscape");
        return;
    }

    if (SHGetSpecialFolderPathW(NULL, base, CSIDL_APPDATA, FALSE) && base[0] != L'\0') {
        ws_wcopy(out_dir, cap, base);
        ws_wcat(out_dir, cap, L"\\Waterscape");
        return;
    }

    n = GetEnvironmentVariableW(L"ProgramFiles", base, MAX_PATH);
    if (n > 0u && n < MAX_PATH && base[0] != L'\0') {
        ws_wcopy(out_dir, cap, base);
        ws_wcat(out_dir, cap, L"\\Waterscape");
        return;
    }

    ws_wcopy(out_dir, cap, L"C:\\Waterscape");
}

static void ws_trim_trailing_slashes_w(WCHAR *path) {
    size_t len = ws_wlen(path);
    while (len > 3u && (path[len - 1u] == L'\\' || path[len - 1u] == L'/')) {
        path[--len] = L'\0';
    }
}

static bool ws_ensure_dir_recursive_w(const WCHAR *dir_path) {
    WCHAR tmp[520];
    ws_wcopy(tmp, sizeof(tmp) / sizeof(tmp[0]), dir_path);
    size_t len = ws_wlen(tmp);
    if (len == 0u) return false;

    for (size_t i = 0u; i < len; ++i) {
        if (tmp[i] == L'/') tmp[i] = L'\\';
    }

    for (size_t i = 3u; i < len; ++i) {
        if (tmp[i] == L'\\') {
            tmp[i] = L'\0';
            CreateDirectoryW(tmp, NULL);
            tmp[i] = L'\\';
        }
    }
    if (!CreateDirectoryW(tmp, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return true;
}

typedef enum WsSfxLoadStatus {
    WS_SFX_OK = 0,
    WS_SFX_NO_TRAILER = 1,
    WS_SFX_IO_ERROR = 2,
    WS_SFX_CORRUPT_PAYLOAD = 3
} WsSfxLoadStatus;

static WsSfxLoadStatus ws_load_self_sfx_payload(uint8_t **out_zip,
                                                size_t *out_zip_len,
                                                WsSfxTrailer *out_trailer) {
    *out_zip     = NULL;
    *out_zip_len = 0u;

    WCHAR self_path[520];
    DWORD mlen = GetModuleFileNameW(NULL, self_path, 512u);
    if (mlen == 0u || mlen >= 512u) {
        return WS_SFX_IO_ERROR;
    }

    HANDLE hf = CreateFileW(self_path, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        return WS_SFX_IO_ERROR;
    }

    DWORD size_hi = 0u;
    DWORD size_lo = GetFileSize(hf, &size_hi);
    if (size_lo == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
        CloseHandle(hf);
        return WS_SFX_IO_ERROR;
    }

    uint64_t total_size = ((uint64_t)size_hi << 32) | (uint64_t)size_lo;
    if (total_size < WS_SFX_TRAILER_BYTES + 22u) {
        CloseHandle(hf);
        return WS_SFX_NO_TRAILER;
    }

    uint64_t tr_off = total_size - (uint64_t)WS_SFX_TRAILER_BYTES;
    LONG seek_hi = (LONG)((tr_off >> 32) & 0xFFFFFFFFu);
    DWORD seek_lo = (DWORD)(tr_off & 0xFFFFFFFFu);
    if (SetFilePointer(hf, (LONG)seek_lo, &seek_hi, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
        GetLastError() != NO_ERROR) {
        CloseHandle(hf);
        return WS_SFX_IO_ERROR;
    }

    uint8_t raw_tr[WS_SFX_TRAILER_BYTES];
    DWORD got = 0u;
    if (!ReadFile(hf, raw_tr, WS_SFX_TRAILER_BYTES, &got, NULL) || got != WS_SFX_TRAILER_BYTES) {
        CloseHandle(hf);
        return WS_SFX_IO_ERROR;
    }

    if (memcmp(raw_tr, WS_SFX_MAGIC_STR, WS_SFX_MAGIC_LEN) != 0) {
        CloseHandle(hf);
        return WS_SFX_NO_TRAILER;
    }

    WsSfxTrailer tr;
    if (!ws_sfx_trailer_decode(raw_tr, &tr) ||
        !ws_sfx_trailer_validate_bounds(&tr, total_size) ||
        tr.zip_size > (uint64_t)(256u * 1024u * 1024u)) {
        CloseHandle(hf);
        return WS_SFX_CORRUPT_PAYLOAD;
    }

    LONG zip_hi = (LONG)((tr.zip_offset >> 32) & 0xFFFFFFFFu);
    DWORD zip_lo = (DWORD)(tr.zip_offset & 0xFFFFFFFFu);
    if (SetFilePointer(hf, (LONG)zip_lo, &zip_hi, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
        GetLastError() != NO_ERROR) {
        CloseHandle(hf);
        return WS_SFX_IO_ERROR;
    }

    size_t zlen = (size_t)tr.zip_size;
    uint8_t *zbuf = (uint8_t *)malloc(zlen);
    if (!zbuf) {
        CloseHandle(hf);
        return WS_SFX_IO_ERROR;
    }

    size_t read_total = 0u;
    while (read_total < zlen) {
        DWORD chunk = (zlen - read_total > 65536u) ? 65536u : (DWORD)(zlen - read_total);
        DWORD rd = 0u;
        if (!ReadFile(hf, zbuf + read_total, chunk, &rd, NULL) || rd == 0u) {
            free(zbuf);
            CloseHandle(hf);
            return WS_SFX_IO_ERROR;
        }
        read_total += (size_t)rd;
    }
    CloseHandle(hf);

    if (arcr_crc32(zbuf, zlen) != tr.zip_crc32) {
        free(zbuf);
        return WS_SFX_CORRUPT_PAYLOAD;
    }

    *out_zip     = zbuf;
    *out_zip_len = zlen;
    if (out_trailer) *out_trailer = tr;
    return WS_SFX_OK;
}

/* Formats live detected CPU, core count, RAM, Windows NT version, and CPUID SIMD tier */
static void ws_build_hardware_summary(char *out_buf, size_t cap) {
    SYSTEM_INFO si;
    memset(&si, 0, sizeof(si));
    GetSystemInfo(&si);

    const char *arch_str = "x86 (32-bit)";
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) {
        arch_str = "x86-64 (64-bit)";
    } else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) {
        arch_str = "ARM64 (64-bit)";
    }

    MEMORYSTATUSEX ms;
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    unsigned long ram_mib = 0ul;
    if (GlobalMemoryStatusEx(&ms)) {
        ram_mib = (unsigned long)(ms.ullTotalPhys / (1024ull * 1024ull));
    }

    DWORD ver   = GetVersion();
    DWORD major = ver & 0xFFu;
    DWORD minor = (ver >> 8) & 0xFFu;
    DWORD build = (ver < 0x80000000u) ? ((ver >> 16) & 0xFFFFu) : 0u;

    const char *os_label = "Windows NT";
    if (major == 5u && minor == 0u)      os_label = "Windows 2000";
    else if (major == 5u && minor == 1u) os_label = "Windows XP";
    else if (major == 5u && minor == 2u) os_label = "Windows XP x64 / Server 2003";
    else if (major == 6u && minor == 0u) os_label = "Windows Vista";
    else if (major == 6u && minor == 1u) os_label = "Windows 7";
    else if (major == 6u && minor == 2u) os_label = "Windows 8";
    else if (major == 6u && minor == 3u) os_label = "Windows 8.1";
    else if (major >= 10u)               os_label = (build >= 22000u) ? "Windows 11" : "Windows 10";

    uint32_t cpu_flags = arcr_detect_cpu_flags();
    const char *tier   = arcr_cpu_simd_tier_name(cpu_flags);

    char flag_list[128];
    flag_list[0] = '\0';
    if (cpu_flags & ARCR_CPU_SSE1)   strcat(flag_list, "SSE1 ");
    if (cpu_flags & ARCR_CPU_SSE2)   strcat(flag_list, "SSE2 ");
    if (cpu_flags & ARCR_CPU_SSE3)   strcat(flag_list, "SSE3 ");
    if (cpu_flags & ARCR_CPU_SSE4_1) strcat(flag_list, "SSE4.1 ");
    if (cpu_flags & ARCR_CPU_AVX2)   strcat(flag_list, "AVX2 ");
    if (flag_list[0] == '\0')        strcpy(flag_list, "Scalar-C11");

    const char *profile_str = "Balanced (Multi-Core SIMD)";
    if ((cpu_flags & ARCR_CPU_AVX2) && si.dwNumberOfProcessors >= 4u && ram_mib >= 2048ul) {
        profile_str = "Modern (AVX2 Tile Rasterizer + Hardware Compositor)";
    } else if (major == 5u && minor == 0u) {
        profile_str = "Minimum / Legacy Tuned (Win2000 Low-RAM SSE1 Path)";
    }

    snprintf(out_buf, cap,
             "Live System Hardware Scan (Arcradient Engine Auto-Tuning)\r\n"
             "------------------------------------------------------------\r\n"
             "  CPU Architecture : %s (%lu logical processor%s)\r\n"
             "  Physical Memory  : %lu MiB RAM\r\n"
             "  Operating System : %s (NT %lu.%lu, Build %lu)\r\n"
             "  Active SIMD Tier : %s [%s]\r\n"
             "  Engine Profile   : %s\r\n",
             arch_str,
             (unsigned long)si.dwNumberOfProcessors,
             (si.dwNumberOfProcessors == 1u) ? "" : "s",
             ram_mib,
             os_label, (unsigned long)major, (unsigned long)minor, (unsigned long)build,
             tier, flag_list,
             profile_str);
}

static bool ws_reg_set_sz(HKEY hkey, const WCHAR *name, const WCHAR *val) {
    DWORD cb = (DWORD)((ws_wlen(val) + 1u) * sizeof(WCHAR));
    return (RegSetValueExW(hkey, name, 0u, REG_SZ, (const BYTE *)val, cb) == ERROR_SUCCESS);
}

static bool ws_reg_set_dword(HKEY hkey, const WCHAR *name, DWORD val) {
    return (RegSetValueExW(hkey, name, 0u, REG_DWORD,
                           (const BYTE *)&val, sizeof(DWORD)) == ERROR_SUCCESS);
}

static void ws_register_hkcu_install(const WCHAR *install_dir,
                                     const WCHAR *browser_exe_path,
                                     uint64_t total_bytes,
                                     bool register_associations) {
    WCHAR uninst_exe[560];
    WCHAR uninst_cmd[600];
    WCHAR quiet_cmd[620];
    WCHAR disp_icon[580];

    ws_wcopy(uninst_exe, sizeof(uninst_exe) / sizeof(WCHAR), install_dir);
    ws_wcat(uninst_exe, sizeof(uninst_exe) / sizeof(WCHAR), L"\\uninstall.exe");

    ws_wcopy(uninst_cmd, sizeof(uninst_cmd) / sizeof(WCHAR), L"\"");
    ws_wcat(uninst_cmd, sizeof(uninst_cmd) / sizeof(WCHAR), uninst_exe);
    ws_wcat(uninst_cmd, sizeof(uninst_cmd) / sizeof(WCHAR), L"\"");

    ws_wcopy(quiet_cmd, sizeof(quiet_cmd) / sizeof(WCHAR), uninst_cmd);
    ws_wcat(quiet_cmd, sizeof(quiet_cmd) / sizeof(WCHAR), L" --silent");

    ws_wcopy(disp_icon, sizeof(disp_icon) / sizeof(WCHAR), browser_exe_path);
    ws_wcat(disp_icon, sizeof(disp_icon) / sizeof(WCHAR), L",0");

    HKEY h_uninst = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Waterscape",
                        0u, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL,
                        &h_uninst, NULL) == ERROR_SUCCESS) {
        WCHAR ver_w[32];
        ws_utf8_to_wide(WATERSCAPE_VERSION_STR, ver_w, sizeof(ver_w) / sizeof(WCHAR));
        DWORD est_kb = (DWORD)((total_bytes + 1023ull) / 1024ull);
        if (est_kb == 0u) est_kb = 64u;

        ws_reg_set_sz(h_uninst, L"DisplayName", L"Waterscape Web Browser");
        ws_reg_set_sz(h_uninst, L"DisplayVersion", ver_w);
        ws_reg_set_sz(h_uninst, L"Publisher", L"Arcradient Project");
        ws_reg_set_sz(h_uninst, L"InstallLocation", install_dir);
        ws_reg_set_sz(h_uninst, L"DisplayIcon", disp_icon);
        ws_reg_set_sz(h_uninst, L"UninstallString", uninst_cmd);
        ws_reg_set_sz(h_uninst, L"QuietUninstallString", quiet_cmd);
        ws_reg_set_dword(h_uninst, L"EstimatedSize", est_kb);
        ws_reg_set_dword(h_uninst, L"NoModify", 1u);
        ws_reg_set_dword(h_uninst, L"NoRepair", 1u);
        RegCloseKey(h_uninst);
    }

    if (!register_associations) {
        return;
    }

    WCHAR open_cmd[620];
    ws_wcopy(open_cmd, sizeof(open_cmd) / sizeof(WCHAR), L"\"");
    ws_wcat(open_cmd, sizeof(open_cmd) / sizeof(WCHAR), browser_exe_path);
    ws_wcat(open_cmd, sizeof(open_cmd) / sizeof(WCHAR), L"\" \"%1\"");

    HKEY hk = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Clients\\StartMenuInternet\\Waterscape",
                        0u, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        ws_reg_set_sz(hk, NULL, L"Waterscape Web Browser");
        RegCloseKey(hk);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Clients\\StartMenuInternet\\Waterscape\\DefaultIcon",
                        0u, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        ws_reg_set_sz(hk, NULL, disp_icon);
        RegCloseKey(hk);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Clients\\StartMenuInternet\\Waterscape\\shell\\open\\command",
                        0u, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        ws_reg_set_sz(hk, NULL, open_cmd);
        RegCloseKey(hk);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Clients\\StartMenuInternet\\Waterscape\\Capabilities",
                        0u, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        ws_reg_set_sz(hk, L"ApplicationName", L"Waterscape Web Browser");
        ws_reg_set_sz(hk, L"ApplicationDescription",
                      L"Lightweight hardware-accelerated Pure C11 web browser");
        RegCloseKey(hk);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Clients\\StartMenuInternet\\Waterscape\\Capabilities\\FileAssociations",
                        0u, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        ws_reg_set_sz(hk, L".htm", L"WaterscapeHTML");
        ws_reg_set_sz(hk, L".html", L"WaterscapeHTML");
        RegCloseKey(hk);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Clients\\StartMenuInternet\\Waterscape\\Capabilities\\URLAssociations",
                        0u, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        ws_reg_set_sz(hk, L"http", L"WaterscapeHTML");
        ws_reg_set_sz(hk, L"https", L"WaterscapeHTML");
        RegCloseKey(hk);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\RegisteredApplications",
                        0u, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        ws_reg_set_sz(hk, L"Waterscape",
                      L"Software\\Clients\\StartMenuInternet\\Waterscape\\Capabilities");
        RegCloseKey(hk);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Classes\\WaterscapeHTML",
                        0u, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        ws_reg_set_sz(hk, NULL, L"Waterscape HTML Document");
        RegCloseKey(hk);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Classes\\WaterscapeHTML\\DefaultIcon",
                        0u, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        ws_reg_set_sz(hk, NULL, disp_icon);
        RegCloseKey(hk);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Classes\\WaterscapeHTML\\shell\\open\\command",
                        0u, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        ws_reg_set_sz(hk, NULL, open_cmd);
        RegCloseKey(hk);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Classes\\.htm\\OpenWithProgids",
                        0u, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        ws_reg_set_sz(hk, L"WaterscapeHTML", L"");
        RegCloseKey(hk);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Classes\\.html\\OpenWithProgids",
                        0u, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        ws_reg_set_sz(hk, L"WaterscapeHTML", L"");
        RegCloseKey(hk);
    }
}

static bool ws_create_shell_shortcut(const WCHAR *lnk_path,
                                     const WCHAR *target_exe,
                                     const WCHAR *work_dir,
                                     const WCHAR *description) {
    void *psl_void = NULL;
    HRESULT hr = CoCreateInstance(&kWsCLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                  &kWsIID_IShellLinkW, &psl_void);
    if (FAILED(hr) || !psl_void) {
        return false;
    }

    IShellLinkW *psl = (IShellLinkW *)psl_void;
    psl->lpVtbl->SetPath(psl, target_exe);
    psl->lpVtbl->SetArguments(psl, L"");
    psl->lpVtbl->SetWorkingDirectory(psl, work_dir);
    psl->lpVtbl->SetDescription(psl, description);
    psl->lpVtbl->SetIconLocation(psl, target_exe, 0);

    /* Stamp explicit AppUserModelID ("Waterscape.Browser.2") on Vista/7/10/11+ so taskbar
     * pinning and grouping match SetCurrentProcessExplicitAppUserModelID in waterscape.exe;
     * degrades gracefully on Windows 2000/XP where IPropertyStore is unavailable. */
    void *pps_void = NULL;
    if (SUCCEEDED(psl->lpVtbl->QueryInterface(psl, &kWsIID_IPropertyStore, &pps_void)) && pps_void) {
        WsPropertyStore *pps = (WsPropertyStore *)pps_void;
        WsPropVariant pv;
        memset(&pv, 0, sizeof(pv));
        pv.vt        = WS_VT_LPWSTR;
        pv.u.pwszVal = WS_SHORTCUT_APP_USER_MODEL_ID;
        if (SUCCEEDED(pps->lpVtbl->SetValue(pps, &kWsPKEY_AppUserModel_ID, &pv))) {
            (void)pps->lpVtbl->Commit(pps);
        }
        pps->lpVtbl->Release(pps);
    }

    bool ok = false;
    void *ppf_void = NULL;
    hr = psl->lpVtbl->QueryInterface(psl, &kWsIID_IPersistFile, &ppf_void);
    if (SUCCEEDED(hr) && ppf_void) {
        IPersistFile *ppf = (IPersistFile *)ppf_void;
        if (SUCCEEDED(ppf->lpVtbl->Save(ppf, lnk_path, TRUE))) {
            ok = true;
        }
        ppf->lpVtbl->Release(ppf);
    }
    psl->lpVtbl->Release(psl);
    return ok;
}

static void ws_create_shortcuts(const WCHAR *install_dir,
                                const WCHAR *browser_exe_path,
                                bool want_desktop,
                                bool want_startmenu) {
    HRESULT co_hr = CoInitialize(NULL);

    if (want_desktop) {
        WCHAR desk[MAX_PATH];
        if (SHGetSpecialFolderPathW(NULL, desk, CSIDL_DESKTOPDIRECTORY, TRUE)) {
            WCHAR lnk[560];
            ws_wcopy(lnk, sizeof(lnk) / sizeof(WCHAR), desk);
            ws_wcat(lnk, sizeof(lnk) / sizeof(WCHAR), L"\\Waterscape.lnk");
            ws_create_shell_shortcut(lnk, browser_exe_path, install_dir,
                                     L"Waterscape Web Browser");
        }
    }

    if (want_startmenu) {
        WCHAR progs[MAX_PATH];
        if (SHGetSpecialFolderPathW(NULL, progs, CSIDL_PROGRAMS, TRUE)) {
            WCHAR folder[540];
            WCHAR lnk[580];
            ws_wcopy(folder, sizeof(folder) / sizeof(WCHAR), progs);
            ws_wcat(folder, sizeof(folder) / sizeof(WCHAR), L"\\Waterscape");
            ws_ensure_dir_recursive_w(folder);

            ws_wcopy(lnk, sizeof(lnk) / sizeof(WCHAR), folder);
            ws_wcat(lnk, sizeof(lnk) / sizeof(WCHAR), L"\\Waterscape.lnk");
            ws_create_shell_shortcut(lnk, browser_exe_path, install_dir,
                                     L"Waterscape Web Browser");
        }
    }

    if (SUCCEEDED(co_hr)) {
        CoUninitialize();
    }
}

typedef void (*WsProgressCallback)(void *userdata, int permille, const char *status_utf8);

static bool ws_perform_installation(const uint8_t *zip_bytes,
                                    size_t zip_len,
                                    const WCHAR *install_dir_in,
                                    const WsSetupOptions *opt,
                                    WCHAR *out_browser_exe,
                                    size_t out_exe_cap,
                                    WsProgressCallback progress_cb,
                                    void *progress_ud,
                                    char *err_buf,
                                    size_t err_cap) {
    WCHAR install_dir[520];
    ws_wcopy(install_dir, sizeof(install_dir) / sizeof(WCHAR), install_dir_in);
    ws_trim_trailing_slashes_w(install_dir);

    if (!ws_ensure_dir_recursive_w(install_dir)) {
        snprintf(err_buf, err_cap, "Failed to create installation directory.");
        return false;
    }

    size_t entry_cnt = arcr_zip_list_entries(zip_bytes, zip_len, NULL, 0u);
    if (entry_cnt == 0u) {
        snprintf(err_buf, err_cap, "Embedded installer PKZIP archive contains 0 entries.");
        return false;
    }

    char (*names)[128] = (char (*)[128])calloc(entry_cnt, sizeof(*names));
    if (!names) {
        snprintf(err_buf, err_cap, "Out of memory listing archive entries.");
        return false;
    }
    arcr_zip_list_entries(zip_bytes, zip_len, names, entry_cnt);

    /* Open install_manifest.txt in target directory */
    WCHAR manifest_w[560];
    ws_wcopy(manifest_w, sizeof(manifest_w) / sizeof(WCHAR), install_dir);
    ws_wcat(manifest_w, sizeof(manifest_w) / sizeof(WCHAR), L"\\install_manifest.txt");
    HANDLE h_manifest = CreateFileW(manifest_w, GENERIC_WRITE, 0u, NULL,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    uint64_t total_bytes = 0ull;
    bool found_waterscape_exe = false;
    bool found_arcradient_exe = false;

    for (size_t i = 0u; i < entry_cnt; ++i) {
        const char *rel = names[i];
        if (!ws_is_safe_rel_path(rel)) {
            continue;
        }
        if (!opt->install_addons && ws_is_addon_entry(rel)) {
            continue;
        }

        if (progress_cb) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Extracting %s ...", rel);
            int pct = (int)(((i + 1u) * 850u) / entry_cnt);
            progress_cb(progress_ud, pct, msg);
        }

        uint8_t *fdata = NULL;
        size_t   fsize = 0u;
        if (!arcr_zip_extract_entry(zip_bytes, zip_len, rel, &fdata, &fsize)) {
            if (h_manifest != INVALID_HANDLE_VALUE) CloseHandle(h_manifest);
            snprintf(err_buf, err_cap, "Failed extracting archive entry '%s'.", rel);
            free(names);
            return false;
        }

        WCHAR rel_w[260];
        ws_utf8_to_wide(rel, rel_w, sizeof(rel_w) / sizeof(WCHAR));
        for (size_t k = 0u; rel_w[k] != L'\0'; ++k) {
            if (rel_w[k] == L'/') rel_w[k] = L'\\';
        }

        WCHAR dst_path[640];
        ws_wcopy(dst_path, sizeof(dst_path) / sizeof(WCHAR), install_dir);
        ws_wcat(dst_path, sizeof(dst_path) / sizeof(WCHAR), L"\\");
        ws_wcat(dst_path, sizeof(dst_path) / sizeof(WCHAR), rel_w);

        /* Ensure parent directory exists for nested files such as addons\*.wsx */
        WCHAR parent_dir[640];
        ws_wcopy(parent_dir, sizeof(parent_dir) / sizeof(WCHAR), dst_path);
        size_t plen = ws_wlen(parent_dir);
        while (plen > 0u && parent_dir[plen - 1u] != L'\\') {
            --plen;
        }
        if (plen > 3u) {
            parent_dir[plen - 1u] = L'\0';
            ws_ensure_dir_recursive_w(parent_dir);
        }

        SetFileAttributesW(dst_path, FILE_ATTRIBUTE_NORMAL);
        HANDLE hf = CreateFileW(dst_path, GENERIC_WRITE, 0u, NULL,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE) {
            free(fdata);
            if (h_manifest != INVALID_HANDLE_VALUE) CloseHandle(h_manifest);
            snprintf(err_buf, err_cap, "Cannot write target file '%s' (in use or access denied).", rel);
            free(names);
            return false;
        }

        size_t written_all = 0u;
        while (written_all < fsize) {
            DWORD chunk = (fsize - written_all > 65536u) ? 65536u : (DWORD)(fsize - written_all);
            DWORD wr = 0u;
            if (!WriteFile(hf, fdata + written_all, chunk, &wr, NULL) || wr != chunk) {
                CloseHandle(hf);
                free(fdata);
                if (h_manifest != INVALID_HANDLE_VALUE) CloseHandle(h_manifest);
                snprintf(err_buf, err_cap, "Disk write error while writing '%s'.", rel);
                free(names);
                return false;
            }
            written_all += (size_t)wr;
        }
        CloseHandle(hf);
        free(fdata);
        total_bytes += (uint64_t)fsize;

        if (h_manifest != INVALID_HANDLE_VALUE) {
            char line[160];
            int ln = snprintf(line, sizeof(line), "%s\r\n", rel);
            if (ln > 0) {
                DWORD mwr = 0u;
                WriteFile(h_manifest, line, (DWORD)ln, &mwr, NULL);
            }
        }

        if (arcr_str_eq_nocase(rel, "waterscape.exe")) found_waterscape_exe = true;
        if (arcr_str_eq_nocase(rel, "arcradient.exe")) found_arcradient_exe = true;
    }

    if (h_manifest != INVALID_HANDLE_VALUE) {
        CloseHandle(h_manifest);
    }
    free(names);

    if (!found_waterscape_exe && found_arcradient_exe) {
        snprintf(err_buf, err_cap, "Installer archive is missing GUI executable 'waterscape.exe'.");
        return false;
    }

    /* Strictly target WIN32 GUI subsystem waterscape.exe — never console arcradient.exe */
    ws_wcopy(out_browser_exe, out_exe_cap, install_dir);
    ws_wcat(out_browser_exe, out_exe_cap, L"\\waterscape.exe");

    if (opt->write_registry) {
        if (progress_cb) {
            progress_cb(progress_ud, 910, "Registering Waterscape in HKCU Registry ...");
        }
        ws_register_hkcu_install(install_dir, out_browser_exe, total_bytes,
                                 opt->register_associations);
    }

    if (opt->create_desktop_shortcut || opt->create_startmenu_shortcut) {
        if (progress_cb) {
            progress_cb(progress_ud, 960, "Creating Windows Shell shortcuts (.lnk) ...");
        }
        ws_create_shortcuts(install_dir, out_browser_exe,
                            opt->create_desktop_shortcut,
                            opt->create_startmenu_shortcut);
    }

    if (progress_cb) {
        progress_cb(progress_ud, 1000, "Installation completed successfully.");
    }
    return true;
}

/* =========================================================================
 * 3. Interactive 4-Page Win32 Setup Wizard
 * ========================================================================= */
typedef struct WsWizardState {
    HWND           hwnd;
    HFONT          font_body;
    HFONT          font_bold;
    HFONT          font_mono;
    int            page; /* 0..3 */
    bool           install_running;
    bool           install_done;
    const uint8_t *zip_bytes;
    size_t         zip_len;
    WsSetupOptions opts;
    WCHAR          install_dir[520];
    WCHAR          installed_exe[560];

    /* Page 1 controls */
    HWND lbl_welcome;
    HWND edt_hw_scan;

    /* Page 2 controls */
    HWND edt_license;
    HWND chk_accept;

    /* Page 3 controls */
    HWND lbl_dir;
    HWND edt_dir;
    HWND btn_browse;
    HWND chk_core;
    HWND chk_addons;
    HWND chk_desktop;
    HWND chk_startmenu;
    HWND chk_assoc;

    /* Page 4 controls */
    HWND lbl_status;
    HWND bar_progress;
    HWND edt_log;
    HWND chk_launch;

    /* Navigation buttons */
    HWND btn_back;
    HWND btn_next;
    HWND btn_cancel;
} WsWizardState;

static WsWizardState g_wiz;

static const char *const kLicenseSummaryUtf8 =
    "Waterscape Web Browser 1.0 & Arcradient Engine (MSHTML-Next)\r\n"
    "Copyright (c) 2026 Arcradient Project. Released under the MIT License.\r\n"
    "\r\n"
    "ARCHITECTURAL & PRIVACY PRINCIPLES\r\n"
    "  1. Zero Telemetry & Zero Spyware: Waterscape never transmits browsing\r\n"
    "     history, keystrokes, hardware IDs, or crash dumps without consent.\r\n"
    "  2. Pure C11 Clean-Room Engine: Built from the ground up in ISO C11 with\r\n"
    "     on-demand dynamic modules (arcr_net, arcr_js_portable, arcr_media,\r\n"
    "     arcr_devtools) so static pages use minimal memory.\r\n"
    "  3. Hardware-Scaled Longevity: Runs natively from Windows 2000 SP4 (SSE1)\r\n"
    "     up to modern multi-core x64 AVX2 workstations.\r\n"
    "\r\n"
    "MIT LICENSE PERMISSION NOTICE\r\n"
    "Permission is hereby granted, free of charge, to any person obtaining a\r\n"
    "copy of this software and associated documentation files to deal in the\r\n"
    "Software without restriction, including without limitation the rights to\r\n"
    "use, copy, modify, merge, publish, distribute, sublicense, and/or sell\r\n"
    "copies of the Software, provided the above copyright notice is included.\r\n"
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND.\r\n";

/* Procedurally paints the 500x64 top ocean gradient + Waterscape emblem banner */
static void ws_paint_wizard_banner(HDC hdc, int width, int page, bool done) {
    const int bh = 64;
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -bh; /* Top-down DIB */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP hdib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0u);
    if (hdib && bits) {
        uint32_t *px = (uint32_t *)bits;
        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < width; ++x) {
                uint32_t r = (uint32_t)(9 + (x * 8) / (width > 0 ? width : 1));
                uint32_t g = (uint32_t)(24 + (x * 40) / (width > 0 ? width : 1) + (y / 5));
                uint32_t b = (uint32_t)(58 + (x * 64) / (width > 0 ? width : 1) + (y / 3));

                /* Bottom 2px luminous cyan accent border */
                if (y >= bh - 2) {
                    r = 34u; g = 211u; b = 238u;
                } else {
                    /* Crisp 40x40 Waterscape circular emblem on left at (14, 12) */
                    int dx = x - 34;
                    int dy = y - 32;
                    int d2 = dx * dx + dy * dy;
                    if (d2 <= 20 * 20) {
                        if (dy > 4 + (dx / 4)) {
                            r = 34u; g = 211u; b = 238u; /* Cyan wave */
                        } else if (dx >= -8 && dx <= 7 && dy >= -13 && dy <= 5 &&
                                   (dx <= 0 ? (-dx * 2 <= dy + 14) : (dx * 2 <= dy + 14))) {
                            r = 255u; g = 255u; b = 255u; /* White sail */
                        } else {
                            r = 12u; g = 46u; b = 104u; /* Deep indigo badge */
                        }
                    }
                }
                px[y * width + x] = (r << 16) | (g << 8) | b;
            }
        }
        HDC mem = CreateCompatibleDC(hdc);
        HGDIOBJ old = SelectObject(mem, hdib);
        BitBlt(hdc, 0, 0, width, bh, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(hdib);
    }

    const WCHAR *titles[4] = {
        L"Waterscape Browser 1.0 Setup",
        L"License & Architecture Principles",
        L"Install Location & Components",
        L"Installing Waterscape Browser"
    };
    const WCHAR *subs[4] = {
        L"Welcome & Live CPU / SIMD Hardware Capability Scan",
        L"Clean-room Pure C11 Engine · Zero Telemetry Commitment",
        L"Select destination folder, bundled extensions, and shortcuts",
        L"Extracting verified WSSFX100 package and configuring shell"
    };
    const WCHAR *title = (page == 3 && done)
        ? L"Waterscape Browser 1.0 Ready"
        : titles[(page >= 0 && page < 4) ? page : 0];
    const WCHAR *sub = (page == 3 && done)
        ? L"Setup has finished installing Waterscape on your computer."
        : subs[(page >= 0 && page < 4) ? page : 0];

    SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ oldf = SelectObject(hdc, g_wiz.font_bold);
    SetTextColor(hdc, RGB(255, 255, 255));
    RECT rt = {66, 12, width - 12, 34};
    DrawTextW(hdc, title, -1, &rt, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(hdc, g_wiz.font_body);
    SetTextColor(hdc, RGB(186, 230, 253));
    RECT rs = {66, 35, width - 12, 56};
    DrawTextW(hdc, sub, -1, &rs, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldf);
}

static void ws_wizard_sync_page(void) {
    int p = g_wiz.page;
    ShowWindow(g_wiz.lbl_welcome,   p == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_wiz.edt_hw_scan,   p == 0 ? SW_SHOW : SW_HIDE);

    ShowWindow(g_wiz.edt_license,   p == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_wiz.chk_accept,    p == 1 ? SW_SHOW : SW_HIDE);

    ShowWindow(g_wiz.lbl_dir,       p == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_wiz.edt_dir,       p == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_wiz.btn_browse,    p == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_wiz.chk_core,      p == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_wiz.chk_addons,    p == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_wiz.chk_desktop,   p == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_wiz.chk_startmenu, p == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_wiz.chk_assoc,     p == 2 ? SW_SHOW : SW_HIDE);

    ShowWindow(g_wiz.lbl_status,    p == 3 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_wiz.bar_progress,  p == 3 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_wiz.edt_log,       p == 3 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_wiz.chk_launch,    (p == 3 && g_wiz.install_done) ? SW_SHOW : SW_HIDE);

    EnableWindow(g_wiz.btn_back, (p > 0 && p < 3));
    if (p == 0) {
        SetWindowTextW(g_wiz.btn_next, L"Next >");
        EnableWindow(g_wiz.btn_next, TRUE);
    } else if (p == 1) {
        SetWindowTextW(g_wiz.btn_next, L"Next >");
        BOOL accepted = (SendMessageW(g_wiz.chk_accept, BM_GETCHECK, 0, 0) == BST_CHECKED);
        EnableWindow(g_wiz.btn_next, accepted);
    } else if (p == 2) {
        SetWindowTextW(g_wiz.btn_next, L"Install");
        EnableWindow(g_wiz.btn_next, TRUE);
    } else {
        SetWindowTextW(g_wiz.btn_next, L"Finish");
        EnableWindow(g_wiz.btn_next, g_wiz.install_done ? TRUE : FALSE);
        EnableWindow(g_wiz.btn_cancel, FALSE);
    }

    InvalidateRect(g_wiz.hwnd, NULL, TRUE);
}

static void ws_wizard_append_log(const char *line_utf8) {
    WCHAR wline[320];
    ws_utf8_to_wide(line_utf8, wline, sizeof(wline) / sizeof(WCHAR));
    ws_wcat(wline, sizeof(wline) / sizeof(WCHAR), L"\r\n");

    int len = GetWindowTextLengthW(g_wiz.edt_log);
    SendMessageW(g_wiz.edt_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_wiz.edt_log, EM_REPLACESEL, FALSE, (LPARAM)wline);
}

static void ws_wizard_progress_cb(void *userdata, int permille, const char *status_utf8) {
    (void)userdata;
    WCHAR wstatus[300];
    ws_utf8_to_wide(status_utf8, wstatus, sizeof(wstatus) / sizeof(WCHAR));
    SetWindowTextW(g_wiz.lbl_status, wstatus);
    SendMessageW(g_wiz.bar_progress, PBM_SETPOS, (WPARAM)permille, 0);
    ws_wizard_append_log(status_utf8);

    MSG msg;
    while (PeekMessageW(&msg, NULL, 0u, 0u, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void ws_wizard_start_install(void) {
    g_wiz.install_running = true;
    g_wiz.page = 3;
    ws_wizard_sync_page();

    GetWindowTextW(g_wiz.edt_dir, g_wiz.install_dir,
                   (int)(sizeof(g_wiz.install_dir) / sizeof(WCHAR)));
    g_wiz.opts.install_addons =
        (SendMessageW(g_wiz.chk_addons, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_wiz.opts.create_desktop_shortcut =
        (SendMessageW(g_wiz.chk_desktop, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_wiz.opts.create_startmenu_shortcut =
        (SendMessageW(g_wiz.chk_startmenu, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_wiz.opts.register_associations =
        (SendMessageW(g_wiz.chk_assoc, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_wiz.opts.write_registry = true;

    char err_msg[256];
    err_msg[0] = '\0';
    bool ok = ws_perform_installation(g_wiz.zip_bytes, g_wiz.zip_len,
                                      g_wiz.install_dir, &g_wiz.opts,
                                      g_wiz.installed_exe,
                                      sizeof(g_wiz.installed_exe) / sizeof(WCHAR),
                                      ws_wizard_progress_cb, NULL,
                                      err_msg, sizeof(err_msg));
    g_wiz.install_running = false;
    if (!ok) {
        WCHAR werr[320];
        ws_utf8_to_wide(err_msg, werr, sizeof(werr) / sizeof(WCHAR));
        MessageBoxW(g_wiz.hwnd, werr, L"Waterscape Setup Error", MB_OK | MB_ICONERROR);
        EnableWindow(g_wiz.btn_cancel, TRUE);
        return;
    }

    g_wiz.install_done = true;
    ws_wizard_sync_page();
}

static void ws_browse_install_folder(HWND owner) {
    HRESULT co_hr = OleInitialize(NULL);
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Select Waterscape Browser installation directory:";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        WCHAR chosen[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, chosen) && chosen[0] != L'\0') {
            WCHAR full[520];
            ws_wcopy(full, sizeof(full) / sizeof(WCHAR), chosen);
            ws_trim_trailing_slashes_w(full);
            /* Append \Waterscape unless folder already ends with Waterscape */
            size_t flen = ws_wlen(full);
            if (flen < 10u || lstrcmpiW(full + flen - 10u, L"Waterscape") != 0) {
                ws_wcat(full, sizeof(full) / sizeof(WCHAR), L"\\Waterscape");
            }
            SetWindowTextW(g_wiz.edt_dir, full);
        }
        CoTaskMemFree(pidl);
    }
    if (SUCCEEDED(co_hr)) {
        OleUninitialize();
    }
}

static void ws_launch_installed_browser(void) {
    if (g_wiz.installed_exe[0] == L'\0') return;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    WCHAR cmd[600];
    ws_wcopy(cmd, sizeof(cmd) / sizeof(WCHAR), L"\"");
    ws_wcat(cmd, sizeof(cmd) / sizeof(WCHAR), g_wiz.installed_exe);
    ws_wcat(cmd, sizeof(cmd) / sizeof(WCHAR), L"\"");

    if (CreateProcessW(g_wiz.installed_exe, cmd, NULL, NULL, FALSE,
                       0u, NULL, g_wiz.install_dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static LRESULT CALLBACK ws_wizard_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_wiz.hwnd = hwnd;
            HINSTANCE hinst = ((LPCREATESTRUCTW)lp)->hInstance;

            /* Page 1: Welcome + Live Hardware Scan */
            g_wiz.lbl_welcome = CreateWindowExW(
                0u, L"STATIC",
                L"This wizard will install Waterscape Web Browser 1.0 (powered by the Pure C11 "
                L"Arcradient Engine) onto your computer. Below is your live hardware profile:",
                WS_CHILD | WS_VISIBLE,
                20, 78, 460, 36, hwnd, NULL, hinst, NULL);

            char hw_u8[1024];
            WCHAR hw_w[1024];
            ws_build_hardware_summary(hw_u8, sizeof(hw_u8));
            ws_utf8_to_wide(hw_u8, hw_w, sizeof(hw_w) / sizeof(WCHAR));

            g_wiz.edt_hw_scan = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", hw_w,
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                20, 120, 460, 190, hwnd, NULL, hinst, NULL);

            /* Page 2: License & Architecture Principles */
            WCHAR lic_w[2048];
            ws_utf8_to_wide(kLicenseSummaryUtf8, lic_w, sizeof(lic_w) / sizeof(WCHAR));
            g_wiz.edt_license = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", lic_w,
                WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                20, 78, 460, 204, hwnd, NULL, hinst, NULL);

            g_wiz.chk_accept = CreateWindowExW(
                0u, L"BUTTON",
                L"I accept the MIT License & zero-telemetry architecture principles",
                WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP,
                20, 290, 460, 22, hwnd, (HMENU)(uintptr_t)IDC_CHK_ACCEPT_LIC, hinst, NULL);
            SendMessageW(g_wiz.chk_accept, BM_SETCHECK, BST_CHECKED, 0);

            /* Page 3: Install Location & Components */
            g_wiz.lbl_dir = CreateWindowExW(
                0u, L"STATIC", L"Destination Folder:",
                WS_CHILD, 20, 78, 300, 18, hwnd, NULL, hinst, NULL);

            g_wiz.edt_dir = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", g_wiz.install_dir,
                WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP,
                20, 98, 364, 24, hwnd, (HMENU)(uintptr_t)IDC_EDIT_DIR, hinst, NULL);

            g_wiz.btn_browse = CreateWindowExW(
                0u, L"BUTTON", L"Browse...",
                WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
                392, 97, 88, 26, hwnd, (HMENU)(uintptr_t)IDC_BTN_BROWSE, hinst, NULL);

            g_wiz.chk_core = CreateWindowExW(
                0u, L"BUTTON", L"Waterscape Core & Hardware Acceleration",
                WS_CHILD | BS_AUTOCHECKBOX | WS_DISABLED,
                20, 140, 450, 22, hwnd, (HMENU)(uintptr_t)IDC_CHK_CORE, hinst, NULL);
            SendMessageW(g_wiz.chk_core, BM_SETCHECK, BST_CHECKED, 0);

            g_wiz.chk_addons = CreateWindowExW(
                0u, L"BUTTON", L"Official Extensions (Shield Ad-Blocker, Dark Mode, Reader)",
                WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP,
                20, 168, 450, 22, hwnd, (HMENU)(uintptr_t)IDC_CHK_ADDONS, hinst, NULL);
            SendMessageW(g_wiz.chk_addons, BM_SETCHECK,
                         g_wiz.opts.install_addons ? BST_CHECKED : BST_UNCHECKED, 0);

            g_wiz.chk_desktop = CreateWindowExW(
                0u, L"BUTTON", L"Create Desktop Shortcut",
                WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP,
                20, 196, 450, 22, hwnd, (HMENU)(uintptr_t)IDC_CHK_DESKTOP, hinst, NULL);
            SendMessageW(g_wiz.chk_desktop, BM_SETCHECK,
                         g_wiz.opts.create_desktop_shortcut ? BST_CHECKED : BST_UNCHECKED, 0);

            g_wiz.chk_startmenu = CreateWindowExW(
                0u, L"BUTTON", L"Create Start Menu Shortcut",
                WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP,
                20, 224, 450, 22, hwnd, (HMENU)(uintptr_t)IDC_CHK_STARTMENU, hinst, NULL);
            SendMessageW(g_wiz.chk_startmenu, BM_SETCHECK,
                         g_wiz.opts.create_startmenu_shortcut ? BST_CHECKED : BST_UNCHECKED, 0);

            g_wiz.chk_assoc = CreateWindowExW(
                0u, L"BUTTON", L"Register Waterscape for HTML/HTTP/HTTPS",
                WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP,
                20, 252, 450, 22, hwnd, (HMENU)(uintptr_t)IDC_CHK_ASSOC, hinst, NULL);
            SendMessageW(g_wiz.chk_assoc, BM_SETCHECK,
                         g_wiz.opts.register_associations ? BST_CHECKED : BST_UNCHECKED, 0);

            /* Page 4: Installing & Finish */
            g_wiz.lbl_status = CreateWindowExW(
                0u, L"STATIC", L"Preparing installation...",
                WS_CHILD, 20, 78, 460, 20, hwnd, NULL, hinst, NULL);

            g_wiz.bar_progress = CreateWindowExW(
                0u, PROGRESS_CLASSW, NULL,
                WS_CHILD, 20, 102, 460, 20, hwnd, NULL, hinst, NULL);
            SendMessageW(g_wiz.bar_progress, PBM_SETRANGE32, 0, 1000);

            g_wiz.edt_log = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                20, 130, 460, 150, hwnd, NULL, hinst, NULL);

            g_wiz.chk_launch = CreateWindowExW(
                0u, L"BUTTON", L"Launch Waterscape Browser now",
                WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP,
                20, 290, 320, 22, hwnd, (HMENU)(uintptr_t)IDC_CHK_LAUNCH, hinst, NULL);
            SendMessageW(g_wiz.chk_launch, BM_SETCHECK, BST_CHECKED, 0);

            /* Navigation Buttons */
            g_wiz.btn_back = CreateWindowExW(
                0u, L"BUTTON", L"< Back",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                214, 340, 84, 26, hwnd, (HMENU)(uintptr_t)IDC_BTN_BACK, hinst, NULL);
            g_wiz.btn_next = CreateWindowExW(
                0u, L"BUTTON", L"Next >",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                304, 340, 84, 26, hwnd, (HMENU)(uintptr_t)IDC_BTN_NEXT, hinst, NULL);
            g_wiz.btn_cancel = CreateWindowExW(
                0u, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                400, 340, 80, 26, hwnd, (HMENU)(uintptr_t)IDC_BTN_CANCEL, hinst, NULL);

            HWND ctrls[] = {
                g_wiz.lbl_welcome, g_wiz.chk_accept, g_wiz.lbl_dir, g_wiz.edt_dir,
                g_wiz.btn_browse, g_wiz.chk_core, g_wiz.chk_addons, g_wiz.chk_desktop,
                g_wiz.chk_startmenu, g_wiz.chk_assoc, g_wiz.lbl_status, g_wiz.chk_launch,
                g_wiz.btn_back, g_wiz.btn_next, g_wiz.btn_cancel
            };
            for (size_t i = 0u; i < sizeof(ctrls) / sizeof(ctrls[0]); ++i) {
                SendMessageW(ctrls[i], WM_SETFONT, (WPARAM)g_wiz.font_body, TRUE);
            }
            SendMessageW(g_wiz.edt_hw_scan, WM_SETFONT, (WPARAM)g_wiz.font_mono, TRUE);
            SendMessageW(g_wiz.edt_license, WM_SETFONT, (WPARAM)g_wiz.font_mono, TRUE);
            SendMessageW(g_wiz.edt_log,     WM_SETFONT, (WPARAM)g_wiz.font_mono, TRUE);

            ws_wizard_sync_page();
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDC_CHK_ACCEPT_LIC) {
                ws_wizard_sync_page();
            } else if (id == IDC_BTN_BROWSE) {
                ws_browse_install_folder(hwnd);
            } else if (id == IDC_BTN_BACK) {
                if (!g_wiz.install_running && g_wiz.page > 0 && g_wiz.page < 3) {
                    g_wiz.page--;
                    ws_wizard_sync_page();
                }
            } else if (id == IDC_BTN_NEXT) {
                if (g_wiz.page < 2) {
                    g_wiz.page++;
                    ws_wizard_sync_page();
                } else if (g_wiz.page == 2) {
                    ws_wizard_start_install();
                } else if (g_wiz.page == 3 && g_wiz.install_done) {
                    if (SendMessageW(g_wiz.chk_launch, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                        ws_launch_installed_browser();
                    }
                    DestroyWindow(hwnd);
                }
            } else if (id == IDC_BTN_CANCEL) {
                if (!g_wiz.install_running) {
                    DestroyWindow(hwnd);
                }
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            ws_paint_wizard_banner(hdc, rc.right - rc.left, g_wiz.page, g_wiz.install_done);

            /* Bottom etched separator line */
            RECT sep = {12, 326, rc.right - 12, 328};
            DrawEdge(hdc, &sep, EDGE_ETCHED, BF_TOP);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CLOSE:
            if (!g_wiz.install_running) {
                DestroyWindow(hwnd);
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int ws_run_gui_wizard(const uint8_t *zip_bytes, size_t zip_len, const WsSetupOptions *opt) {
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_PROGRESS_CLASS | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    memset(&g_wiz, 0, sizeof(g_wiz));
    g_wiz.zip_bytes = zip_bytes;
    g_wiz.zip_len   = zip_len;
    g_wiz.opts      = *opt;

    if (opt->custom_dir_utf8[0] != '\0') {
        ws_utf8_to_wide(opt->custom_dir_utf8, g_wiz.install_dir,
                        sizeof(g_wiz.install_dir) / sizeof(WCHAR));
    } else {
        ws_resolve_default_install_dir(g_wiz.install_dir,
                                       sizeof(g_wiz.install_dir) / sizeof(WCHAR));
    }

    g_wiz.font_body = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_wiz.font_bold = CreateFontW(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Tahoma");
    g_wiz.font_mono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Courier New");
    if (!g_wiz.font_bold) g_wiz.font_bold = g_wiz.font_body;
    if (!g_wiz.font_mono) g_wiz.font_mono = g_wiz.font_body;

    HINSTANCE hinst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ws_wizard_wndproc;
    wc.hInstance     = hinst;
    wc.hIcon         = LoadIconW(hinst, MAKEINTRESOURCEW(101));
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(uintptr_t)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"WaterscapeSetupWizardWnd";
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr = {0, 0, 500, 380};
    AdjustWindowRect(&wr, style, FALSE);
    int ww = wr.right - wr.left;
    int wh = wr.bottom - wr.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;

    HWND hwnd = CreateWindowExW(
        0u, wc.lpszClassName, L"Waterscape Browser 1.0 Setup",
        style, sx > 0 ? sx : 40, sy > 0 ? sy : 40, ww, wh,
        NULL, NULL, hinst, NULL);
    if (!hwnd) {
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0u, 0u) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_wiz.font_bold && g_wiz.font_bold != g_wiz.font_body) DeleteObject(g_wiz.font_bold);
    if (g_wiz.font_mono && g_wiz.font_mono != g_wiz.font_body) DeleteObject(g_wiz.font_mono);
    return g_wiz.install_done ? 0 : 1;
}

int main(int argc, char **argv) {
    WsSetupOptions opt;
    ws_setup_default_options(&opt);
    ws_parse_cli_utf8(argc, argv, &opt);
    ws_parse_win32_cmdline_wide(&opt);

    if (opt.show_help) {
        ws_print_usage((argc > 0) ? argv[0] : "waterscape_setup.exe");
        return 0;
    }

    uint8_t *zip_bytes = NULL;
    size_t   zip_len   = 0u;
    WsSfxTrailer tr;
    memset(&tr, 0, sizeof(tr));
    WsSfxLoadStatus st = ws_load_self_sfx_payload(&zip_bytes, &zip_len, &tr);

    if (opt.verify_stub) {
        if (st == WS_SFX_OK) {
            printf("Waterscape Setup SFX OK: %lu entries, %lu ZIP bytes, CRC32=0x%08lX\n",
                   (unsigned long)tr.file_count,
                   (unsigned long)zip_len,
                   (unsigned long)tr.zip_crc32);
            free(zip_bytes);
            return 0;
        }
        if (st == WS_SFX_NO_TRAILER) {
            printf("Waterscape Setup Stub OK (unpacked stub mode; no WSSFX100 trailer appended)\n");
            return 0;
        }
        fprintf(stderr, "Waterscape Setup SFX verification FAILED (status=%d)\n", (int)st);
        return 1;
    }

    if (st != WS_SFX_OK) {
        const char *reason = (st == WS_SFX_NO_TRAILER)
            ? "No WSSFX100 archive payload appended to this setup stub executable."
            : "Embedded WSSFX100 archive payload failed CRC-32 integrity check.";
        if (opt.silent) {
            fprintf(stderr, "waterscape_setup: %s\n", reason);
        } else {
            WCHAR wmsg[256];
            ws_utf8_to_wide(reason, wmsg, sizeof(wmsg) / sizeof(WCHAR));
            MessageBoxW(NULL, wmsg, L"Waterscape Setup", MB_OK | MB_ICONINFORMATION);
        }
        return 1;
    }

    if (opt.silent) {
        WCHAR target_dir[520];
        WCHAR installed_exe[560];
        if (opt.custom_dir_utf8[0] != '\0') {
            ws_utf8_to_wide(opt.custom_dir_utf8, target_dir,
                            sizeof(target_dir) / sizeof(WCHAR));
        } else {
            ws_resolve_default_install_dir(target_dir, sizeof(target_dir) / sizeof(WCHAR));
        }

        char err_msg[256];
        err_msg[0] = '\0';
        bool ok = ws_perform_installation(zip_bytes, zip_len, target_dir, &opt,
                                          installed_exe, sizeof(installed_exe) / sizeof(WCHAR),
                                          NULL, NULL, err_msg, sizeof(err_msg));
        free(zip_bytes);
        if (!ok) {
            fprintf(stderr, "waterscape_setup: %s\n", err_msg);
            return 1;
        }
        printf("Waterscape %s installed silently.\n", WATERSCAPE_VERSION_STR);
        return 0;
    }

    int rc = ws_run_gui_wizard(zip_bytes, zip_len, &opt);
    free(zip_bytes);
    return rc;
}

#else /* POSIX host build fallback for CI / local pack_installer testing */

static bool ws_posix_mkdir_p(const char *dir) {
    char tmp[1024];
    size_t len = strlen(dir);
    if (len == 0u || len >= sizeof(tmp)) return false;
    memcpy(tmp, dir, len + 1u);
    for (size_t i = 1u; i < len; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            (void)mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    return (mkdir(tmp, 0755) == 0 || errno == EEXIST);
}

int main(int argc, char **argv) {
    WsSetupOptions opt;
    ws_setup_default_options(&opt);
    ws_parse_cli_utf8(argc, argv, &opt);

    if (opt.show_help) {
        ws_print_usage((argc > 0) ? argv[0] : "waterscape_setup");
        return 0;
    }

    const char *self_path = (argc > 0 && argv[0]) ? argv[0] : NULL;
    FILE *fp = self_path ? fopen(self_path, "rb") : NULL;
    if (!fp) {
        if (opt.verify_stub) {
            printf("Waterscape Setup Stub OK\n");
            return 0;
        }
        return 1;
    }

    fseek(fp, 0L, SEEK_END);
    long total = ftell(fp);
    if (total < (long)(WS_SFX_TRAILER_BYTES + 22u)) {
        fclose(fp);
        if (opt.verify_stub) {
            printf("Waterscape Setup Stub OK (unpacked stub)\n");
            return 0;
        }
        fprintf(stderr, "waterscape_setup: no WSSFX100 trailer appended\n");
        return 1;
    }

    uint8_t raw_tr[WS_SFX_TRAILER_BYTES];
    fseek(fp, total - (long)WS_SFX_TRAILER_BYTES, SEEK_SET);
    if (fread(raw_tr, 1u, WS_SFX_TRAILER_BYTES, fp) != WS_SFX_TRAILER_BYTES) {
        fclose(fp);
        return 1;
    }

    if (memcmp(raw_tr, WS_SFX_MAGIC_STR, WS_SFX_MAGIC_LEN) != 0) {
        fclose(fp);
        if (opt.verify_stub) {
            printf("Waterscape Setup Stub OK (unpacked stub)\n");
            return 0;
        }
        fprintf(stderr, "waterscape_setup: no WSSFX100 trailer appended\n");
        return 1;
    }

    WsSfxTrailer tr;
    if (!ws_sfx_trailer_decode(raw_tr, &tr) ||
        !ws_sfx_trailer_validate_bounds(&tr, (uint64_t)total)) {
        fclose(fp);
        return 1;
    }

    uint8_t *zbuf = (uint8_t *)malloc((size_t)tr.zip_size);
    if (!zbuf) {
        fclose(fp);
        return 1;
    }
    fseek(fp, (long)tr.zip_offset, SEEK_SET);
    if (fread(zbuf, 1u, (size_t)tr.zip_size, fp) != (size_t)tr.zip_size ||
        arcr_crc32(zbuf, (size_t)tr.zip_size) != tr.zip_crc32) {
        free(zbuf);
        fclose(fp);
        return 1;
    }
    fclose(fp);

    if (opt.verify_stub) {
        printf("Waterscape Setup SFX OK: %lu files, CRC32=0x%08lX\n",
               (unsigned long)tr.file_count, (unsigned long)tr.zip_crc32);
        free(zbuf);
        return 0;
    }

    const char *dst_root = (opt.custom_dir_utf8[0] != '\0') ? opt.custom_dir_utf8 : "./waterscape_inst";
    if (!ws_posix_mkdir_p(dst_root)) {
        free(zbuf);
        return 1;
    }

    size_t cnt = arcr_zip_list_entries(zbuf, (size_t)tr.zip_size, NULL, 0u);
    char (*names)[128] = (char (*)[128])calloc(cnt > 0u ? cnt : 1u, sizeof(*names));
    arcr_zip_list_entries(zbuf, (size_t)tr.zip_size, names, cnt);

    char man_path[1024];
    snprintf(man_path, sizeof(man_path), "%s/install_manifest.txt", dst_root);
    FILE *man_fp = fopen(man_path, "wb");

    for (size_t i = 0u; i < cnt; ++i) {
        if (!ws_is_safe_rel_path(names[i])) continue;
        if (!opt.install_addons && ws_is_addon_entry(names[i])) continue;
        uint8_t *fdata = NULL;
        size_t   fsize = 0u;
        if (!arcr_zip_extract_entry(zbuf, (size_t)tr.zip_size, names[i], &fdata, &fsize)) {
            free(names);
            free(zbuf);
            if (man_fp) fclose(man_fp);
            return 1;
        }
        char out_p[1024];
        snprintf(out_p, sizeof(out_p), "%s/%s", dst_root, names[i]);
        char par_p[1024];
        arcr_str_copy(par_p, sizeof(par_p), out_p);
        char *slash = strrchr(par_p, '/');
        if (slash) {
            *slash = '\0';
            ws_posix_mkdir_p(par_p);
        }
        FILE *ofp = fopen(out_p, "wb");
        if (!ofp || (fsize > 0u && fwrite(fdata, 1u, fsize, ofp) != fsize)) {
            if (ofp) fclose(ofp);
            free(fdata);
            free(names);
            free(zbuf);
            if (man_fp) fclose(man_fp);
            return 1;
        }
        fclose(ofp);
        free(fdata);
        if (man_fp) fprintf(man_fp, "%s\n", names[i]);
    }
    if (man_fp) fclose(man_fp);
    free(names);
    free(zbuf);
    return 0;
}
#endif
