/*
 * Arcradient Browser (MSHTML-Next)
 * shell/main.c - Vertical-Slice Executable, Win32 GDI Window & Snapshot Generator
 */
#if defined(_WIN32)
#  ifndef WINVER
#    define WINVER 0x0500
#  endif
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0500
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <imm.h>
#endif

#include "arcr_shell.h"
#include "../base/arcr_base.h"
#include "../platform/arcr_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const kKoreanShowcaseLightHtml =
    "<!DOCTYPE html>"
    "<html lang=\"ko\">"
    "<head>"
    "  <title>Arcradient 한국어 설계 개요</title>"
    "  <style>"
    "    :root {"
    "      --accent: #2563eb;"
    "      --card-bg: #f8fafc;"
    "      --card-border: #cbd5e1;"
    "      --badge-bg: #dbeafe;"
    "    }"
    "    body { background: #ffffff; color: #0f172a; padding: 22px; }"
    "    h1 { color: #0f172a; font-size: 23px; margin-bottom: 6px; }"
    "    .subtitle { color: #475569; font-size: 14px; margin-bottom: 16px; }"
    "    .card {"
    "      background: var(--card-bg);"
    "      border: 1px solid var(--card-border);"
    "      border-left: 4px solid var(--accent);"
    "      padding: 14px;"
    "      margin-bottom: 12px;"
    "    }"
    "    h2 { color: var(--accent); font-size: 17px; margin-top: 0px; margin-bottom: 6px; }"
    "    p { font-size: 14px; margin-bottom: 6px; word-break: keep-all; }"
    "    li { font-size: 14px; margin-bottom: 4px; }"
    "    code { background: var(--badge-bg); color: #1e3a8a; padding: 2px 6px; }"
    "  </style>"
    "</head>"
    "<body>"
    "  <h1>Arcradient (MSHTML-Next) 수직 시제품</h1>"
    "  <p class=\"subtitle\">순수 C11 무의존성 코어 · CPU 소프트웨어 기본 렌더링 · 선택적 GPU 가속</p>"
    "  <div class=\"card\">"
    "    <h2>1. 구형 PC부터 현대 64비트까지 단일 엔진 계열</h2>"
    "    <p>Windows 2000 SP4(32비트 <code>SSE1</code>)부터 최신 x64 환경까지 동일한 순수 C 아키텍처로 구동됩니다. 정적 문서에서는 <code>ArcradientJS</code> 모듈을 메모리에 올리지 않아 자원 낭비를 원천 차단합니다.</p>"
    "  </div>"
    "  <div class=\"card\">"
    "    <h2>2. 한국어 조판 및 행두·행말 금칙처리 내장</h2>"
    "    <p>초기 단계부터 한글 어절 줄바꿈(<code>word-break: keep-all</code>), 영문·숫자 혼용 배치, 문장 부호 금칙처리(마침표·쉼표·괄호 고립 방지), 그리고 <code>Imm32</code> 한글 조합기를 기본 지원합니다.</p>"
    "  </div>"
    "  <div class=\"card\">"
    "    <h2>3. 비용 추정 기반 가속과 무중단 CPU 복귀</h2>"
    "    <ul>"
    "      <li>작은 도형과 정적 문서 스크롤은 <code>CPU Scalar / SIMD</code> 타일 경로로 즉시 처리</li>"
    "      <li>GPU 드라이버 오류나 장치 손실 발생 시 프로세스 종료 없이 CPU 경로로 자동 복귀</li>"
    "      <li>내장 루트 인증서 저장소와 CMOS 시계 오차 감지로 안전한 <code>TLS 1.3</code> 연결 수행</li>"
    "    </ul>"
    "  </div>"
    "</body>"
    "</html>";

static const char *const kKoreanShowcaseDarkHtml =
    "<!DOCTYPE html>"
    "<html lang=\"ko\">"
    "<head>"
    "  <title>Arcradient 어두운 테마 검증</title>"
    "  <style>"
    "    :root { --accent: #38bdf8; --card-bg: #1e293b; --border: #334155; }"
    "    body { background: #0f172a; color: #f1f5f9; padding: 22px; }"
    "    h1 { color: #f8fafc; font-size: 23px; margin-bottom: 8px; }"
    "    .card {"
    "      background: var(--card-bg);"
    "      border: 1px solid var(--border);"
    "      border-left: 4px solid var(--accent);"
    "      padding: 14px;"
    "      margin-bottom: 12px;"
    "    }"
    "    h2 { color: var(--accent); font-size: 17px; margin-top: 0px; margin-bottom: 6px; }"
    "    p { color: #cbd5e1; font-size: 14px; word-break: keep-all; }"
    "    code { background: #0b1120; color: #7dd3fc; padding: 2px 6px; }"
    "  </style>"
    "</head>"
    "<body>"
    "  <h1>Chrome의 간결함과 Firefox의 사용자 지정 결합</h1>"
    "  <div class=\"card\">"
    "    <h2>선택형 북마크 바와 압축형 도구 모음 활성화 예시</h2>"
    "    <p>상단 도구 모음은 세로 공간을 아끼는 압축형 높이를 기본으로 하며, 북마크 바는 필요할 때만 <code>Ctrl+B</code> 또는 설정에서 꺼내 쓸 수 있습니다.</p>"
    "  </div>"
    "  <div class=\"card\">"
    "    <h2>지연 로드 상태 실시간 확인</h2>"
    "    <p>현재 문서는 스크립트와 영상 태그를 포함하지 않으므로 <code>arcr-js</code> 및 <code>arcr-media</code> 동적 라이브러리가 메모리에 적재되지 않은 상태(<code>0 Byte</code>)로 동작합니다.</p>"
    "  </div>"
    "</body>"
    "</html>";

static void populate_demo_shell(ArcrBrowserShell *sh, bool add_bookmarks, const char *primary_html) {
    if (add_bookmarks) {
        arcr_shell_add_bookmark(sh, "★ Arcradient 문서", "https://arcradient.org/docs");
        arcr_shell_add_bookmark(sh, "★ 호환성 표 (WPT)", "https://arcradient.org/wpt");
        arcr_shell_add_bookmark(sh, "★ 고급 설정 (arcr://config)", "arcr://config");
    }

    arcr_shell_open_tab_html(sh, "https://arcradient.org/ko/mshtml-next",
                             primary_html, strlen(primary_html), true);
    arcr_shell_open_tab_html(sh, "arcr://settings",
                             kKoreanShowcaseDarkHtml, strlen(kKoreanShowcaseDarkHtml), false);
    const char *compat_html =
        "<!DOCTYPE html><html lang=\"ko\"><head><title>지원 범위 표</title>"
        "<style>body{background:#ffffff;color:#0f172a;padding:22px;}h1{font-size:21px;margin-bottom:10px;}"
        "p{font-size:14px;word-break:keep-all;margin-bottom:8px;}code{background:#e2e8f0;padding:2px 6px;}</style>"
        "</head><body><h1>플랫폼 지원 범위 (Legacy x86 ~ Modern x64)</h1>"
        "<p><code>Legacy x86</code>: Windows 2000 SP4 이상 · 32비트 <code>SSE1</code> + 스칼라 기본 렌더링</p>"
        "<p><code>Classic / Modern x64</code>: <code>SSE3</code> 최소 기준선 유지 (<code>AVX</code> 비필수, 런타임 감지 시 가속)</p>"
        "<p>단축키 안내: <code>F2</code> 테마 전환 · <code>Ctrl+B</code> 북마크 바 표시/숨김 · 탭 클릭 전환</p>"
        "</body></html>";
    arcr_shell_open_tab_html(sh, "https://arcradient.org/ko/compat",
                             compat_html, strlen(compat_html), false);
}

static bool render_theme_snapshot(const char *module_dir, const char *out_png,
                                  ArcrProfileMode profile, ArcrShellTheme theme,
                                  bool show_bookmarks, const char *html) {
    ArcrEngineConfig ecfg;
    arcr_engine_default_config(&ecfg, profile);
    ecfg.module_search_dir = module_dir;

    ArcrEngine *eng = arcr_engine_create(&ecfg);
    if (!eng) return false;

    ArcrShellConfig scfg;
    arcr_shell_default_config(&scfg, theme);
    scfg.show_bookmarks_bar = show_bookmarks;

    ArcrBrowserShell *sh = arcr_shell_create(eng, &scfg);
    if (!sh) {
        arcr_engine_destroy(eng);
        return false;
    }

    populate_demo_shell(sh, show_bookmarks, html);
    arcr_shell_activate_tab(sh, 0u);
    arcr_shell_set_status_overlay(sh, "링크 미리보기: https://arcradient.org/ko/accel-pipeline");

    int32_t w = scfg.window_width;
    int32_t h = scfg.window_height;
    uint32_t *fb = (uint32_t *)calloc((size_t)(w * h), sizeof(uint32_t));
    if (!fb) {
        arcr_shell_destroy(sh);
        arcr_engine_destroy(eng);
        return false;
    }

    bool ok = arcr_shell_render_frame(sh, fb, w, h, w) &&
              arcr_platform_write_png(out_png, fb, w, h, w);

    ArcrEngineStats st;
    arcr_engine_get_stats(eng, &st);
    printf("  -> Saved %-32s (%dx%d) | DOM=%u boxes=%u cmds=%u shared=%lu arena=%luB js_loaded=%s\n",
           out_png, (int)w, (int)h,
           st.dom_node_count, st.layout_box_count, st.display_cmd_count,
           (unsigned long)st.style_shared_hits, (unsigned long)st.arena_used_bytes,
           st.js_module_loaded ? "YES" : "NO");

    free(fb);
    arcr_shell_destroy(sh);
    arcr_engine_destroy(eng);
    return ok;
}

#if defined(_WIN32)
#ifndef WM_MOUSEWHEEL
#  define WM_MOUSEWHEEL 0x020A
#endif
#ifndef GWLP_USERDATA
#  define GWLP_USERDATA (-21)
#endif

typedef struct Win32HostWindow {
    ArcrBrowserShell *shell;
    HDC               mem_dc;
    HBITMAP           dib;
    HGDIOBJ           old_bmp;
    uint32_t         *dib_pixels;
    int32_t           dib_w;
    int32_t           dib_h;
    int32_t           scroll_y;
} Win32HostWindow;

static void win32_resize_dib(Win32HostWindow *host, int32_t w, int32_t h) {
    if (!host || w <= 0 || h <= 0) return;
    if (w == host->dib_w && h == host->dib_h && host->dib) return;

    if (!host->mem_dc) {
        host->mem_dc = CreateCompatibleDC(NULL);
    }
    if (host->dib) {
        SelectObject(host->mem_dc, host->old_bmp);
        DeleteObject(host->dib);
        host->dib        = NULL;
        host->dib_pixels = NULL;
    }

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h; /* Top-down ARGB8888 framebuffer */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(host->mem_dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (hbm && bits) {
        host->old_bmp    = SelectObject(host->mem_dc, hbm);
        host->dib        = hbm;
        host->dib_pixels = (uint32_t *)bits;
        host->dib_w      = w;
        host->dib_h      = h;
    }
}

static LRESULT CALLBACK arcr_win32_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Win32HostWindow *host = (Win32HostWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; /* Prevent flicker on non-composited Windows 2000/XP desktops */
    case WM_SIZE:
        if (host) {
            int32_t w = (int32_t)(LOWORD(lParam));
            int32_t h = (int32_t)(HIWORD(lParam));
            if (w > 0 && h > 0) {
                win32_resize_dib(host, w, h);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (host && host->shell) {
            int32_t x = (int32_t)(int16_t)LOWORD(lParam);
            int32_t y = (int32_t)(int16_t)HIWORD(lParam);
            int tab_idx = arcr_shell_hit_test_tab(host->shell, x, y);
            if (tab_idx >= 0) {
                arcr_shell_activate_tab(host->shell, (size_t)tab_idx);
                host->scroll_y = 0;
                ArcrWebView *v = arcr_shell_active_webview(host->shell);
                if (v) arcr_webview_set_scroll(v, 0);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (host && host->shell) {
            int16_t delta = (int16_t)HIWORD(wParam);
            host->scroll_y -= (delta / 120) * 48;
            if (host->scroll_y < 0) host->scroll_y = 0;
            ArcrWebView *v = arcr_shell_active_webview(host->shell);
            if (v) arcr_webview_set_scroll(v, host->scroll_y);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (host && host->shell) {
            if (wParam == VK_F2) {
                ArcrShellTheme next = (ArcrShellTheme)(((int)arcr_shell_get_theme(host->shell) + 1) % 3);
                arcr_shell_set_theme(host->shell, next, false);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wParam == 'B' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                bool vis = !arcr_shell_is_bookmarks_bar_visible(host->shell);
                arcr_shell_set_bookmarks_bar_visible(host->shell, vis);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wParam == VK_DOWN || wParam == VK_UP || wParam == VK_NEXT || wParam == VK_PRIOR) {
                int32_t step = (wParam == VK_NEXT || wParam == VK_PRIOR) ? 180 : 36;
                if (wParam == VK_UP || wParam == VK_PRIOR) step = -step;
                host->scroll_y += step;
                if (host->scroll_y < 0) host->scroll_y = 0;
                ArcrWebView *v = arcr_shell_active_webview(host->shell);
                if (v) arcr_webview_set_scroll(v, host->scroll_y);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }
        break;
    case WM_IME_COMPOSITION:
        if (host && host->shell) {
            HIMC himc = ImmGetContext(hwnd);
            if (himc) {
                DWORD flag = (lParam & GCS_RESULTSTR) ? GCS_RESULTSTR : GCS_COMPSTR;
                LONG bytes = ImmGetCompositionStringW(himc, flag, NULL, 0);
                if (bytes > 0 && bytes < 512) {
                    WCHAR wbuf[256];
                    ZeroMemory(wbuf, sizeof(wbuf));
                    ImmGetCompositionStringW(himc, flag, wbuf, (DWORD)bytes);
                    char utf8[768];
                    int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, bytes / (LONG)sizeof(WCHAR),
                                                utf8, (int)sizeof(utf8) - 1, NULL, NULL);
                    if (n > 0) {
                        utf8[n] = '\0';
                        ArcrWebView *v = arcr_shell_active_webview(host->shell);
                        if (v) {
                            arcr_webview_ime_composition(v, utf8, n, (lParam & GCS_RESULTSTR) != 0);
                        }
                        char status[832];
                        snprintf(status, sizeof(status), "IME 조합 입력: %s", utf8);
                        arcr_shell_set_status_overlay(host->shell, status);
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                }
                ImmReleaseContext(hwnd, himc);
            }
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (host && host->shell && host->dib_pixels && host->dib_w > 0 && host->dib_h > 0) {
            arcr_shell_render_frame(host->shell, host->dib_pixels,
                                    host->dib_w, host->dib_h, host->dib_w);
            BitBlt(hdc, 0, 0, host->dib_w, host->dib_h, host->mem_dc, 0, 0, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int run_win32_interactive_window(const char *mod_dir) {
    ArcrSystemUiMetrics ui_m;
    arcr_platform_query_ui_metrics(&ui_m);

    ArcrProfileMode prof = ui_m.prefer_classic_opaque ? ARCR_PROFILE_MINIMUM : ARCR_PROFILE_MODERN;
    ArcrShellTheme theme = ui_m.prefer_classic_opaque ? ARCR_SHELL_THEME_WIN2000_CLASSIC
                                                      : ARCR_SHELL_THEME_MODERN_LIGHT;

    ArcrEngineConfig ecfg;
    arcr_engine_default_config(&ecfg, prof);
    ecfg.module_search_dir = mod_dir;

    ArcrEngine *eng = arcr_engine_create(&ecfg);
    if (!eng) return 1;

    ArcrShellConfig scfg;
    arcr_shell_default_config(&scfg, theme);
    scfg.draw_os_window_frame = false; /* Native Win32 provides outer window frame */

    ArcrBrowserShell *sh = arcr_shell_create(eng, &scfg);
    if (!sh) {
        arcr_engine_destroy(eng);
        return 1;
    }

    populate_demo_shell(sh, true, kKoreanShowcaseLightHtml);
    arcr_shell_set_status_overlay(sh, "F2: 테마 전환 | Ctrl+B: 북마크 바 | 탭 클릭 지원");

    Win32HostWindow host;
    ZeroMemory(&host, sizeof(host));
    host.shell = sh;

    HINSTANCE hInst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = arcr_win32_wndproc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"ArcradientBrowserWnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName,
        L"Arcradient Browser (MSHTML-Next \x00B7 Pure C11)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, scfg.window_width, scfg.window_height,
        NULL, NULL, hInst, &host);
    if (!hwnd) {
        arcr_shell_destroy(sh);
        arcr_engine_destroy(eng);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (host.dib) {
        SelectObject(host.mem_dc, host.old_bmp);
        DeleteObject(host.dib);
    }
    if (host.mem_dc) DeleteDC(host.mem_dc);
    arcr_shell_destroy(sh);
    arcr_engine_destroy(eng);
    return (int)msg.wParam;
}
#endif /* _WIN32 */

int main(int argc, char **argv) {
    uint32_t cpu_flags = arcr_detect_cpu_flags();
    printf("Arcradient Browser / Engine (MSHTML-Next · Pure C11)\n");
    printf("  OS Platform  : %s (ver 0x%04X)\n",
           arcr_platform_os_name(), (unsigned)arcr_platform_os_version());
    printf("  Host SIMD    : %s (flags=0x%X)\n",
           arcr_cpu_simd_tier_name(cpu_flags), cpu_flags);

    const char *mod_dir = ".";
    const char *snap_dir = NULL;
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--module-dir") == 0 && i + 1 < argc) {
            mod_dir = argv[++i];
        } else if (strcmp(argv[i], "--snapshot-dir") == 0 && i + 1 < argc) {
            snap_dir = argv[++i];
        } else if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        }
    }

    if (snap_dir) {
        char p1[512], p2[512], p3[512];
        snprintf(p1, sizeof(p1), "%s/shell_modern_light.png", snap_dir);
        snprintf(p2, sizeof(p2), "%s/shell_modern_dark.png", snap_dir);
        snprintf(p3, sizeof(p3), "%s/shell_win2000_classic.png", snap_dir);

        bool ok1 = render_theme_snapshot(mod_dir, p1, ARCR_PROFILE_MODERN,
                                         ARCR_SHELL_THEME_MODERN_LIGHT, false,
                                         kKoreanShowcaseLightHtml);
        bool ok2 = render_theme_snapshot(mod_dir, p2, ARCR_PROFILE_MODERN,
                                         ARCR_SHELL_THEME_MODERN_DARK, true,
                                         kKoreanShowcaseDarkHtml);
        bool ok3 = render_theme_snapshot(mod_dir, p3, ARCR_PROFILE_MINIMUM,
                                         ARCR_SHELL_THEME_WIN2000_CLASSIC, false,
                                         kKoreanShowcaseLightHtml);
        return (ok1 && ok2 && ok3) ? 0 : 1;
    }

#if defined(_WIN32)
    if (!headless) {
        return run_win32_interactive_window(mod_dir);
    }
#else
    (void)headless;
#endif

    return 0;
}
