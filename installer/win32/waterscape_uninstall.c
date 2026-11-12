/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * installer/win32/waterscape_uninstall.c - Pure C11 Win32 & Silent Uninstaller
 *
 * Compatible with Windows 2000 SP4 (`WINVER=0x0500`) through Windows 11.
 * Zero Vista+/XP-only imports; uses classic kernel32, user32, advapi32, shell32 APIs.
 *
 * Supported CLI switches:
 *   --help / -h / /?       Print usage and exit 0
 *   --verify-stub          Verify uninstaller binary and exit 0
 *   --silent / /S          Run unattended silent uninstallation
 *   --dir <install_dir>    Explicit target directory to uninstall
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
#  include <shellapi.h>
#  include <ole2.h>
#  include <shlobj.h>
#else
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  include <dirent.h>
#  include <unistd.h>
#endif

#include "../../base/arcr_base.h"
#include "../../include/arcradient_embed.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct WsUninstallOptions {
    bool show_help;
    bool verify_stub;
    bool silent;
    char custom_dir_utf8[512];
} WsUninstallOptions;

static bool ws_uninst_is_safe_rel_path(const char *rel) {
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

static void ws_uninst_parse_cli_utf8(int argc, char **argv, WsUninstallOptions *opt) {
    memset(opt, 0, sizeof(*opt));
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
        } else if (strcmp(arg, "--dir") == 0 && i + 1 < argc) {
            arcr_str_copy(opt->custom_dir_utf8, sizeof(opt->custom_dir_utf8), argv[++i]);
        } else if (arcr_str_starts_with_nocase(arg, "--dir=")) {
            arcr_str_copy(opt->custom_dir_utf8, sizeof(opt->custom_dir_utf8), arg + 6);
        }
    }
}

static void ws_uninst_print_usage(const char *prog) {
    printf("Waterscape Browser %s Uninstaller\n"
           "Usage: %s [--silent | /S] [--dir <install_dir>] [--help]\n",
           WATERSCAPE_VERSION_STR,
           prog ? prog : "uninstall.exe");
}

#if defined(_WIN32)

#ifndef CSIDL_PROGRAMS
#  define CSIDL_PROGRAMS 0x0002
#endif
#ifndef CSIDL_DESKTOPDIRECTORY
#  define CSIDL_DESKTOPDIRECTORY 0x0010
#endif

static size_t ws_u_wlen(const WCHAR *s) {
    size_t n = 0u;
    if (!s) return 0u;
    while (s[n] != L'\0') ++n;
    return n;
}

static void ws_u_wcopy(WCHAR *dst, size_t cap, const WCHAR *src) {
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

static void ws_u_wcat(WCHAR *dst, size_t cap, const WCHAR *suffix) {
    if (!dst || !suffix || cap == 0u) return;
    size_t cur = ws_u_wlen(dst);
    if (cur + 1u >= cap) return;
    ws_u_wcopy(dst + cur, cap - cur, suffix);
}

static void ws_u_utf8_to_wide(const char *u8, WCHAR *out_w, size_t cap) {
    if (!out_w || cap == 0u) return;
    out_w[0] = L'\0';
    if (!u8 || u8[0] == '\0') return;
    if (MultiByteToWideChar(CP_UTF8, 0u, u8, -1, out_w, (int)cap) <= 0) {
        MultiByteToWideChar(CP_ACP, 0u, u8, -1, out_w, (int)cap);
    }
    out_w[cap - 1u] = L'\0';
}

static void ws_u_wide_to_utf8(const WCHAR *wide, char *out_u8, size_t cap) {
    if (!out_u8 || cap == 0u) return;
    out_u8[0] = '\0';
    if (!wide || wide[0] == L'\0') return;
    WideCharToMultiByte(CP_UTF8, 0u, wide, -1, out_u8, (int)cap, NULL, NULL);
    out_u8[cap - 1u] = '\0';
}

static void ws_u_parse_cmdline_wide(WsUninstallOptions *opt) {
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
            if (tlen + 1u < sizeof(token) / sizeof(WCHAR)) {
                token[tlen++] = *p;
            }
            ++p;
        }
        token[tlen] = L'\0';

        if (arg_idx++ == 0) continue;

        char u8[1024];
        ws_u_wide_to_utf8(token, u8, sizeof(u8));
        if (expect_dir) {
            arcr_str_copy(opt->custom_dir_utf8, sizeof(opt->custom_dir_utf8), u8);
            expect_dir = false;
        } else if (strcmp(u8, "--dir") == 0) {
            expect_dir = true;
        } else if (arcr_str_starts_with_nocase(u8, "--dir=")) {
            arcr_str_copy(opt->custom_dir_utf8, sizeof(opt->custom_dir_utf8), u8 + 6);
        } else if (strcmp(u8, "--help") == 0 || strcmp(u8, "-h") == 0 || strcmp(u8, "/?") == 0) {
            opt->show_help = true;
        } else if (strcmp(u8, "--verify-stub") == 0) {
            opt->verify_stub = true;
        } else if (arcr_str_eq_nocase(u8, "--silent") || strcmp(u8, "/S") == 0 ||
                   strcmp(u8, "/s") == 0) {
            opt->silent = true;
        }
    }
}

static void ws_u_trim_slashes(WCHAR *path) {
    size_t len = ws_u_wlen(path);
    while (len > 3u && (path[len - 1u] == L'\\' || path[len - 1u] == L'/')) {
        path[--len] = L'\0';
    }
}

/* Win2000-compatible recursive Registry key deletion (no Vista RegDeleteTreeW) */
static void ws_reg_delete_tree_win2k(HKEY root, const WCHAR *subkey) {
    HKEY hk = NULL;
    if (RegOpenKeyExW(root, subkey, 0u, KEY_READ | KEY_WRITE, &hk) == ERROR_SUCCESS) {
        WCHAR child[260];
        for (;;) {
            DWORD cch = 256u;
            if (RegEnumKeyExW(hk, 0u, child, &cch, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
                break;
            }
            WCHAR full_sub[540];
            ws_u_wcopy(full_sub, sizeof(full_sub) / sizeof(WCHAR), subkey);
            ws_u_wcat(full_sub, sizeof(full_sub) / sizeof(WCHAR), L"\\");
            ws_u_wcat(full_sub, sizeof(full_sub) / sizeof(WCHAR), child);
            ws_reg_delete_tree_win2k(root, full_sub);
        }
        RegCloseKey(hk);
    }
    RegDeleteKeyW(root, subkey);
}

static void ws_reg_delete_value_win2k(HKEY root, const WCHAR *subkey, const WCHAR *val_name) {
    HKEY hk = NULL;
    if (RegOpenKeyExW(root, subkey, 0u, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
        RegDeleteValueW(hk, val_name);
        RegCloseKey(hk);
    }
}

static void ws_delete_file_quiet(const WCHAR *filepath) {
    SetFileAttributesW(filepath, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(filepath);
}

static void ws_delete_glob_in_dir(const WCHAR *dir_path, const WCHAR *pattern_leaf) {
    WCHAR search[600];
    ws_u_wcopy(search, sizeof(search) / sizeof(WCHAR), dir_path);
    ws_u_wcat(search, sizeof(search) / sizeof(WCHAR), L"\\");
    ws_u_wcat(search, sizeof(search) / sizeof(WCHAR), pattern_leaf);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
            WCHAR target[620];
            ws_u_wcopy(target, sizeof(target) / sizeof(WCHAR), dir_path);
            ws_u_wcat(target, sizeof(target) / sizeof(WCHAR), L"\\");
            ws_u_wcat(target, sizeof(target) / sizeof(WCHAR), fd.cFileName);
            ws_delete_file_quiet(target);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

static bool ws_path_is_inside_dir(const WCHAR *file_path, const WCHAR *dir_path) {
    size_t dlen = ws_u_wlen(dir_path);
    size_t flen = ws_u_wlen(file_path);
    if (dlen < 4u || flen <= dlen) return false;

    WCHAR prefix[520];
    ws_u_wcopy(prefix, sizeof(prefix) / sizeof(WCHAR), file_path);
    prefix[dlen] = L'\0';
    if (lstrcmpiW(prefix, dir_path) != 0) return false;
    return (file_path[dlen] == L'\\' || file_path[dlen] == L'/');
}

static void ws_remove_manifest_files(const WCHAR *install_dir, const WCHAR *self_exe) {
    WCHAR man_path[580];
    ws_u_wcopy(man_path, sizeof(man_path) / sizeof(WCHAR), install_dir);
    ws_u_wcat(man_path, sizeof(man_path) / sizeof(WCHAR), L"\\install_manifest.txt");

    HANDLE hf = CreateFileW(man_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return;

    DWORD sz = GetFileSize(hf, NULL);
    if (sz != INVALID_FILE_SIZE && sz > 0u && sz < (4u * 1024u * 1024u)) {
        char *buf = (char *)malloc((size_t)sz + 1u);
        if (buf) {
            DWORD rd = 0u;
            if (ReadFile(hf, buf, sz, &rd, NULL)) {
                buf[rd] = '\0';
                char *cur = buf;
                while (*cur != '\0') {
                    char *eol = cur;
                    while (*eol != '\0' && *eol != '\r' && *eol != '\n') ++eol;
                    char saved = *eol;
                    *eol = '\0';

                    if (ws_uninst_is_safe_rel_path(cur)) {
                        WCHAR rel_w[260];
                        ws_u_utf8_to_wide(cur, rel_w, sizeof(rel_w) / sizeof(WCHAR));
                        for (size_t k = 0u; rel_w[k] != L'\0'; ++k) {
                            if (rel_w[k] == L'/') rel_w[k] = L'\\';
                        }
                        WCHAR target[640];
                        ws_u_wcopy(target, sizeof(target) / sizeof(WCHAR), install_dir);
                        ws_u_wcat(target, sizeof(target) / sizeof(WCHAR), L"\\");
                        ws_u_wcat(target, sizeof(target) / sizeof(WCHAR), rel_w);

                        if (lstrcmpiW(target, self_exe) != 0) {
                            ws_delete_file_quiet(target);
                        }
                    }

                    if (saved == '\0') break;
                    cur = eol + 1;
                    while (*cur == '\r' || *cur == '\n') ++cur;
                }
            }
            free(buf);
        }
    }
    CloseHandle(hf);
    ws_delete_file_quiet(man_path);
}

static bool ws_resolve_uninstall_target(const WsUninstallOptions *opt,
                                        const WCHAR *self_exe,
                                        WCHAR *out_dir,
                                        size_t cap) {
    if (opt->custom_dir_utf8[0] != '\0') {
        ws_u_utf8_to_wide(opt->custom_dir_utf8, out_dir, cap);
        ws_u_trim_slashes(out_dir);
        return (ws_u_wlen(out_dir) > 3u);
    }

    /* Try HKCU Uninstall InstallLocation if self_exe is not inside an install dir */
    WCHAR self_dir[520];
    ws_u_wcopy(self_dir, sizeof(self_dir) / sizeof(WCHAR), self_exe);
    size_t slen = ws_u_wlen(self_dir);
    while (slen > 0u && self_dir[slen - 1u] != L'\\' && self_dir[slen - 1u] != L'/') {
        --slen;
    }
    if (slen > 3u) {
        self_dir[slen - 1u] = L'\0';
    }

    WCHAR probe_man[580];
    ws_u_wcopy(probe_man, sizeof(probe_man) / sizeof(WCHAR), self_dir);
    ws_u_wcat(probe_man, sizeof(probe_man) / sizeof(WCHAR), L"\\install_manifest.txt");
    if (GetFileAttributesW(probe_man) != INVALID_FILE_ATTRIBUTES) {
        ws_u_wcopy(out_dir, cap, self_dir);
        return (ws_u_wlen(out_dir) > 3u);
    }

    HKEY hk = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Waterscape",
                      0u, KEY_READ, &hk) == ERROR_SUCCESS) {
        WCHAR reg_loc[520];
        DWORD cb = sizeof(reg_loc);
        DWORD type = 0u;
        if (RegQueryValueExW(hk, L"InstallLocation", NULL, &type,
                             (LPBYTE)reg_loc, &cb) == ERROR_SUCCESS &&
            type == REG_SZ && reg_loc[0] != L'\0') {
            RegCloseKey(hk);
            ws_u_wcopy(out_dir, cap, reg_loc);
            ws_u_trim_slashes(out_dir);
            return (ws_u_wlen(out_dir) > 3u);
        }
        RegCloseKey(hk);
    }

    ws_u_wcopy(out_dir, cap, self_dir);
    ws_u_trim_slashes(out_dir);
    return (ws_u_wlen(out_dir) > 3u);
}

static void ws_schedule_self_delete(const WCHAR *self_exe, const WCHAR *install_dir) {
    WCHAR comspec[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    if (n == 0u || n >= MAX_PATH) {
        ws_u_wcopy(comspec, MAX_PATH, L"cmd.exe");
    }

    WCHAR cmdline[1400];
    ws_u_wcopy(cmdline, sizeof(cmdline) / sizeof(WCHAR), L"\"");
    ws_u_wcat(cmdline, sizeof(cmdline) / sizeof(WCHAR), comspec);
    ws_u_wcat(cmdline, sizeof(cmdline) / sizeof(WCHAR),
              L"\" /c ping -n 2 127.0.0.1 >nul & del /f /q \"");
    ws_u_wcat(cmdline, sizeof(cmdline) / sizeof(WCHAR), self_exe);
    ws_u_wcat(cmdline, sizeof(cmdline) / sizeof(WCHAR), L"\" & rmdir \"");
    ws_u_wcat(cmdline, sizeof(cmdline) / sizeof(WCHAR), install_dir);
    ws_u_wcat(cmdline, sizeof(cmdline) / sizeof(WCHAR), L"\"");

    WCHAR temp_dir[MAX_PATH];
    DWORD tn = GetTempPathW(MAX_PATH, temp_dir);
    const WCHAR *work_dir = (tn > 0u && tn < MAX_PATH) ? temp_dir : L"C:\\";

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    if (CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, work_dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

int main(int argc, char **argv) {
    WsUninstallOptions opt;
    ws_uninst_parse_cli_utf8(argc, argv, &opt);
    ws_u_parse_cmdline_wide(&opt);

    if (opt.show_help) {
        ws_uninst_print_usage((argc > 0) ? argv[0] : "uninstall.exe");
        return 0;
    }
    if (opt.verify_stub) {
        printf("Waterscape Uninstaller OK (version %s)\n", WATERSCAPE_VERSION_STR);
        return 0;
    }

    WCHAR self_exe[520];
    self_exe[0] = L'\0';
    GetModuleFileNameW(NULL, self_exe, 512u);

    WCHAR install_dir[520];
    install_dir[0] = L'\0';
    if (!ws_resolve_uninstall_target(&opt, self_exe, install_dir,
                                     sizeof(install_dir) / sizeof(WCHAR))) {
        if (!opt.silent) {
            MessageBoxW(NULL, L"Unable to determine a valid Waterscape installation folder.",
                        L"Waterscape Uninstall", MB_OK | MB_ICONERROR);
        }
        return 1;
    }

    if (!opt.silent) {
        WCHAR prompt[768];
        ws_u_wcopy(prompt, sizeof(prompt) / sizeof(WCHAR),
                   L"Are you sure you want to completely remove Waterscape Web Browser from:\r\n\r\n");
        ws_u_wcat(prompt, sizeof(prompt) / sizeof(WCHAR), install_dir);
        ws_u_wcat(prompt, sizeof(prompt) / sizeof(WCHAR),
                  L"\r\n\r\n(Your personal documents and system files will not be touched.)");

        int ans = MessageBoxW(NULL, prompt, L"Waterscape Browser Uninstall",
                              MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (ans != IDYES) {
            return 0;
        }
    }

    /* 1. Delete files listed in install_manifest.txt */
    ws_remove_manifest_files(install_dir, self_exe);

    /* 2. Delete known Waterscape binaries, dynamic modules, and .wsx extensions */
    static const WCHAR *const kKnownFiles[] = {
        L"waterscape.exe",
        L"arcradient.exe",
        L"README.txt",
        L"LICENSE.txt",
        L"waterscape.ico"
    };
    for (size_t i = 0u; i < sizeof(kKnownFiles) / sizeof(kKnownFiles[0]); ++i) {
        WCHAR fpath[600];
        ws_u_wcopy(fpath, sizeof(fpath) / sizeof(WCHAR), install_dir);
        ws_u_wcat(fpath, sizeof(fpath) / sizeof(WCHAR), L"\\");
        ws_u_wcat(fpath, sizeof(fpath) / sizeof(WCHAR), kKnownFiles[i]);
        if (lstrcmpiW(fpath, self_exe) != 0) {
            ws_delete_file_quiet(fpath);
        }
    }

    ws_delete_glob_in_dir(install_dir, L"arcr_*.dll");
    ws_delete_glob_in_dir(install_dir, L"ws_plugin_*.dll");

    WCHAR addons_dir[580];
    ws_u_wcopy(addons_dir, sizeof(addons_dir) / sizeof(WCHAR), install_dir);
    ws_u_wcat(addons_dir, sizeof(addons_dir) / sizeof(WCHAR), L"\\addons");
    ws_delete_glob_in_dir(addons_dir, L"*.wsx");
    RemoveDirectoryW(addons_dir);

    /* 3. Remove Desktop & Start Menu .lnk shortcuts */
    WCHAR desk[MAX_PATH];
    if (SHGetSpecialFolderPathW(NULL, desk, CSIDL_DESKTOPDIRECTORY, FALSE)) {
        WCHAR lnk[560];
        ws_u_wcopy(lnk, sizeof(lnk) / sizeof(WCHAR), desk);
        ws_u_wcat(lnk, sizeof(lnk) / sizeof(WCHAR), L"\\Waterscape.lnk");
        ws_delete_file_quiet(lnk);
    }

    WCHAR progs[MAX_PATH];
    if (SHGetSpecialFolderPathW(NULL, progs, CSIDL_PROGRAMS, FALSE)) {
        WCHAR sm_dir[540];
        WCHAR sm_lnk[580];
        ws_u_wcopy(sm_dir, sizeof(sm_dir) / sizeof(WCHAR), progs);
        ws_u_wcat(sm_dir, sizeof(sm_dir) / sizeof(WCHAR), L"\\Waterscape");

        ws_u_wcopy(sm_lnk, sizeof(sm_lnk) / sizeof(WCHAR), sm_dir);
        ws_u_wcat(sm_lnk, sizeof(sm_lnk) / sizeof(WCHAR), L"\\Waterscape.lnk");
        ws_delete_file_quiet(sm_lnk);
        RemoveDirectoryW(sm_dir);

        ws_u_wcopy(sm_lnk, sizeof(sm_lnk) / sizeof(WCHAR), progs);
        ws_u_wcat(sm_lnk, sizeof(sm_lnk) / sizeof(WCHAR), L"\\Waterscape.lnk");
        ws_delete_file_quiet(sm_lnk);
    }

    /* 4. Remove HKCU Uninstall & StartMenuInternet Registry entries */
    ws_reg_delete_tree_win2k(HKEY_CURRENT_USER,
                             L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Waterscape");
    ws_reg_delete_tree_win2k(HKEY_CURRENT_USER,
                             L"Software\\Clients\\StartMenuInternet\\Waterscape");
    ws_reg_delete_tree_win2k(HKEY_CURRENT_USER,
                             L"Software\\Classes\\WaterscapeHTML");
    ws_reg_delete_value_win2k(HKEY_CURRENT_USER,
                              L"Software\\RegisteredApplications", L"Waterscape");
    ws_reg_delete_value_win2k(HKEY_CURRENT_USER,
                              L"Software\\Classes\\.htm\\OpenWithProgids", L"WaterscapeHTML");
    ws_reg_delete_value_win2k(HKEY_CURRENT_USER,
                              L"Software\\Classes\\.html\\OpenWithProgids", L"WaterscapeHTML");

    /* 5. Self-delete uninstall.exe if running inside target dir, or delete directly */
    bool self_inside = ws_path_is_inside_dir(self_exe, install_dir);
    if (self_inside) {
        ws_schedule_self_delete(self_exe, install_dir);
    } else {
        WCHAR ext_uninst[580];
        ws_u_wcopy(ext_uninst, sizeof(ext_uninst) / sizeof(WCHAR), install_dir);
        ws_u_wcat(ext_uninst, sizeof(ext_uninst) / sizeof(WCHAR), L"\\uninstall.exe");
        ws_delete_file_quiet(ext_uninst);
        RemoveDirectoryW(install_dir);
    }

    if (!opt.silent) {
        MessageBoxW(NULL, L"Waterscape Web Browser has been uninstalled.",
                    L"Waterscape Uninstall", MB_OK | MB_ICONINFORMATION);
    } else {
        printf("Waterscape uninstalled cleanly.\n");
    }
    return 0;
}

#else /* POSIX host fallback for CI verification */

int main(int argc, char **argv) {
    WsUninstallOptions opt;
    ws_uninst_parse_cli_utf8(argc, argv, &opt);

    if (opt.show_help) {
        ws_uninst_print_usage((argc > 0) ? argv[0] : "waterscape_uninstall");
        return 0;
    }
    if (opt.verify_stub) {
        printf("Waterscape Uninstaller OK (version %s)\n", WATERSCAPE_VERSION_STR);
        return 0;
    }
    if (opt.custom_dir_utf8[0] == '\0' || strlen(opt.custom_dir_utf8) < 4u) {
        fprintf(stderr, "waterscape_uninstall: specify --dir <install_dir>\n");
        return 1;
    }

    char man_path[1024];
    snprintf(man_path, sizeof(man_path), "%s/install_manifest.txt", opt.custom_dir_utf8);
    FILE *fp = fopen(man_path, "rb");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            size_t len = strlen(line);
            while (len > 0u && (line[len - 1u] == '\r' || line[len - 1u] == '\n')) {
                line[--len] = '\0';
            }
            if (ws_uninst_is_safe_rel_path(line)) {
                char target[1024];
                snprintf(target, sizeof(target), "%s/%s", opt.custom_dir_utf8, line);
                (void)unlink(target);
            }
        }
        fclose(fp);
        (void)unlink(man_path);
    }

    char sub_addons[1024];
    snprintf(sub_addons, sizeof(sub_addons), "%s/addons", opt.custom_dir_utf8);
    (void)rmdir(sub_addons);
    (void)rmdir(opt.custom_dir_utf8);
    return 0;
}
#endif
