/*
 * Arcradient Browser Shell
 * shell/arcr_shell.h - Pure C Browser Shell (Chrome Simplicity + Firefox Customizability)
 *
 * Strictly separated from internal engine structures: interacts with Arcradient Engine
 * solely via public `include/arcradient_embed.h` handles.
 */
#ifndef ARCR_SHELL_H
#define ARCR_SHELL_H

#include "../include/arcradient_embed.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARCR_SHELL_MAX_TABS      8u
#define ARCR_SHELL_MAX_BOOKMARKS 8u

typedef enum ArcrShellTheme {
    ARCR_SHELL_THEME_MODERN_LIGHT    = 0,
    ARCR_SHELL_THEME_MODERN_DARK     = 1,
    ARCR_SHELL_THEME_WIN2000_CLASSIC = 2 /* Opaque, 3D Win32 bevels, zero alpha overhead */
} ArcrShellTheme;

typedef struct ArcrShellLayoutMetrics {
    ArcrRect window_caption_rect; /* Optional Classic Win2000 titlebar when simulated */
    ArcrRect tab_strip_rect;      /* 1. 탭 표시줄 (~30px compact) */
    ArcrRect nav_toolbar_rect;    /* 2. 탐색 도구 모음 (~34px compact) */
    ArcrRect bookmarks_bar_rect;  /* 3. 선택형 북마크 바 (0px hidden by default, 24px shown) */
    ArcrRect content_viewport;    /* 4. 웹 콘텐츠 영역 (Maximized remaining space) */
    ArcrRect status_overlay_rect; /* 5. 하단 좌측 임시 상태 표시 (Shown only on hover/load) */
} ArcrShellLayoutMetrics;

typedef struct ArcrShellConfig {
    int32_t        window_width;
    int32_t        window_height;
    ArcrShellTheme theme;
    bool           show_bookmarks_bar; /* Default false per Section 5.2 */
    bool           show_home_button;   /* User customizable */
    bool           draw_os_window_frame;
    const char    *default_cjk_font_path;
} ArcrShellConfig;

typedef struct ArcrBrowserShell ArcrBrowserShell;

void              arcr_shell_default_config(ArcrShellConfig *out_cfg, ArcrShellTheme theme);
ArcrBrowserShell *arcr_shell_create(ArcrEngine *engine, const ArcrShellConfig *cfg);
void              arcr_shell_destroy(ArcrBrowserShell *shell);

/* Tab, Bookmark & Status management */
int  arcr_shell_open_tab_html(ArcrBrowserShell *shell, const char *url,
                              const char *utf8_html, size_t html_len, bool activate);
bool arcr_shell_activate_tab(ArcrBrowserShell *shell, size_t tab_index);
void arcr_shell_set_theme(ArcrBrowserShell *shell, ArcrShellTheme theme, bool draw_os_frame);
ArcrShellTheme arcr_shell_get_theme(const ArcrBrowserShell *shell);
void arcr_shell_set_bookmarks_bar_visible(ArcrBrowserShell *shell, bool visible);
bool arcr_shell_is_bookmarks_bar_visible(const ArcrBrowserShell *shell);
void arcr_shell_add_bookmark(ArcrBrowserShell *shell, const char *title, const char *url);
void arcr_shell_set_status_overlay(ArcrBrowserShell *shell, const char *utf8_status_or_null);
int  arcr_shell_hit_test_tab(const ArcrBrowserShell *shell, int32_t x, int32_t y);

ArcrWebView *arcr_shell_active_webview(ArcrBrowserShell *shell);
void         arcr_shell_compute_layout(const ArcrBrowserShell *shell,
                                       ArcrShellLayoutMetrics *out_metrics);

/* Renders full Browser Chrome + Active WebView into an ARGB8888 framebuffer via CPU */
bool arcr_shell_render_frame(ArcrBrowserShell *shell, uint32_t *argb_pixels,
                             int32_t width, int32_t height, int32_t stride_pixels);

#endif /* ARCR_SHELL_H */
