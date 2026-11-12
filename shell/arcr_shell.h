/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * shell/arcr_shell.h - Firefox-Class Pure C11 Browser Shell, Internal Pages & Profile
 *
 * Strictly separated from internal engine structures: interacts with Arcradient Engine
 * solely via public `include/arcradient_embed.h`, `include/waterscape_modules.h`,
 * and `addons/ws_addons.h`.
 */
#ifndef ARCR_SHELL_H
#define ARCR_SHELL_H

#include "../addons/ws_addons.h"
#include "../include/arcradient_embed.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARCR_SHELL_MAX_TABS      16u
#define ARCR_SHELL_MAX_BOOKMARKS 32u
#define ARCR_SHELL_MAX_HISTORY   64u
#define ARCR_SHELL_TAB_HIST_CAP  32u

typedef enum ArcrShellTheme {
    ARCR_SHELL_THEME_MODERN_LIGHT    = 0,
    ARCR_SHELL_THEME_MODERN_DARK     = 1,
    ARCR_SHELL_THEME_WIN2000_CLASSIC = 2 /* Opaque, 3D Win32 bevels, zero alpha overhead */
} ArcrShellTheme;

typedef struct ArcrShellLayoutMetrics {
    ArcrRect window_caption_rect;   /* Optional Classic Win2000 titlebar when simulated */
    ArcrRect tab_strip_rect;        /* 1. 탭 표시줄 (32px) */
    ArcrRect nav_toolbar_rect;      /* 2. 탐색 도구 모음 (36px) */
    ArcrRect bookmarks_bar_rect;    /* 3. 선택형 북마크 바 (0px hidden by default, 24px shown) */
    ArcrRect content_viewport;      /* 4. 웹 콘텐츠 영역 (Maximized remaining space) */
    ArcrRect status_overlay_rect;   /* 5. 하단 좌측 임시 상태 표시 (Shown on hover/load) */
    ArcrRect find_bar_rect;         /* 6. 문서 내 검색 바 (0px hidden, 28px on Ctrl+F) */
    ArcrRect devtools_dock_rect;    /* 7. 하단 하드웨어·개발자 도구 독 (0px hidden, 150px on F12) */
    ArcrRect vscrollbar_rect;       /* 8. 세로 스크롤바 트랙 */
    ArcrRect vscrollbar_thumb_rect; /* 9. 세로 스크롤바 썸 */
} ArcrShellLayoutMetrics;

typedef struct ArcrShellConfig {
    int32_t        window_width;
    int32_t        window_height;
    ArcrShellTheme theme;
    bool           show_bookmarks_bar; /* Default false per Section 5.2 */
    bool           show_home_button;   /* Default true in Waterscape 1.0 */
    bool           draw_os_window_frame;
    const char    *default_cjk_font_path;
    bool           private_browsing;     /* Ephemeral session: disables disk profile read/write */
    bool           load_saved_prefs;     /* Apply saved theme/bookmarks_bar from prefs.ini */
    bool           show_devtools_dock;   /* Initial F12 Hardware & DevTools dock visibility */
    const char    *module_search_dir;    /* Directory for .wsx add-ons & native plugins */
    const char    *profile_dir_override; /* Optional explicit profile directory path */
} ArcrShellConfig;

typedef struct ArcrBrowserShell ArcrBrowserShell;

/* Lifecycle & Configuration */
void              arcr_shell_default_config(ArcrShellConfig *out_cfg, ArcrShellTheme theme);
ArcrBrowserShell *arcr_shell_create(ArcrEngine *engine, const ArcrShellConfig *cfg);
void              arcr_shell_destroy(ArcrBrowserShell *shell);

/* Add-ons & Profile Access */
WsAddonManager *arcr_shell_get_addons(ArcrBrowserShell *shell);
const char     *arcr_shell_get_profile_dir(const ArcrBrowserShell *shell);
bool            arcr_shell_is_private_browsing(const ArcrBrowserShell *shell);
bool            arcr_shell_load_profile_prefs(ArcrBrowserShell *shell);
bool            arcr_shell_save_profile(const ArcrBrowserShell *shell);
void            arcr_shell_clear_browsing_data(ArcrBrowserShell *shell);

/* Tab, Navigation, Bookmark & UI State Management */
int            arcr_shell_open_tab_html(ArcrBrowserShell *shell, const char *url,
                                        const char *utf8_html, size_t html_len, bool activate);
int            arcr_shell_open_tab_url(ArcrBrowserShell *shell, const char *url, bool activate);
bool           arcr_shell_close_tab(ArcrBrowserShell *shell, size_t tab_index);
bool           arcr_shell_activate_tab(ArcrBrowserShell *shell, size_t tab_index);
size_t         arcr_shell_tab_count(const ArcrBrowserShell *shell);
size_t         arcr_shell_active_tab_index(const ArcrBrowserShell *shell);

bool           arcr_shell_navigate(ArcrBrowserShell *shell, const char *url_or_query);
bool           arcr_shell_go_back(ArcrBrowserShell *shell);
bool           arcr_shell_go_forward(ArcrBrowserShell *shell);
bool           arcr_shell_can_go_back(const ArcrBrowserShell *shell);
bool           arcr_shell_can_go_forward(const ArcrBrowserShell *shell);
bool           arcr_shell_reload(ArcrBrowserShell *shell);
bool           arcr_shell_go_home(ArcrBrowserShell *shell);

void           arcr_shell_set_theme(ArcrBrowserShell *shell, ArcrShellTheme theme, bool draw_os_frame);
ArcrShellTheme arcr_shell_get_theme(const ArcrBrowserShell *shell);
void           arcr_shell_set_bookmarks_bar_visible(ArcrBrowserShell *shell, bool visible);
bool           arcr_shell_is_bookmarks_bar_visible(const ArcrBrowserShell *shell);
void           arcr_shell_set_devtools_visible(ArcrBrowserShell *shell, bool visible);
bool           arcr_shell_is_devtools_visible(const ArcrBrowserShell *shell);
void           arcr_shell_set_find_bar_visible(ArcrBrowserShell *shell, bool visible);
bool           arcr_shell_is_find_bar_visible(const ArcrBrowserShell *shell);

void           arcr_shell_add_bookmark(ArcrBrowserShell *shell, const char *title, const char *url);
bool           arcr_shell_is_url_bookmarked(const ArcrBrowserShell *shell, const char *url);
size_t         arcr_shell_bookmark_count(const ArcrBrowserShell *shell);
void           arcr_shell_set_status_overlay(ArcrBrowserShell *shell, const char *utf8_status_or_null);
int            arcr_shell_hit_test_tab(const ArcrBrowserShell *shell, int32_t x, int32_t y);

ArcrWebView   *arcr_shell_active_webview(ArcrBrowserShell *shell);
void           arcr_shell_compute_layout(const ArcrBrowserShell *shell,
                                         ArcrShellLayoutMetrics *out_metrics);

/*
 * Unified Interactive Input Entrypoints (Win32 + X11 + Headless Automation)
 * - button: 0 = Left, 1 = Middle, 2 = Right
 * - wheel_steps: >0 scroll up / zoom in, <0 scroll down / zoom out
 * - fn_key: 0 = none, 1..12 = F1..F12 (F2 theme, F5 reload, F12 DevTools dock)
 */
ArcrCursorKind arcr_shell_on_mouse_move(ArcrBrowserShell *shell, int32_t x, int32_t y);
bool           arcr_shell_on_mouse_down(ArcrBrowserShell *shell, int32_t x, int32_t y, int button);
bool           arcr_shell_on_mouse_up(ArcrBrowserShell *shell, int32_t x, int32_t y, int button);
bool           arcr_shell_on_mouse_wheel(ArcrBrowserShell *shell, int32_t wheel_steps, bool ctrl_held);
bool           arcr_shell_on_key(ArcrBrowserShell *shell, const char *utf8_text,
                                 ArcrSpecialKey special_key,
                                 bool ctrl_held, bool alt_held, int fn_key);
void           arcr_shell_on_ime_composition(ArcrBrowserShell *shell, const char *utf8_comp_str,
                                             int32_t cursor_offset, bool is_committed);

/* Renders full Waterscape Browser Chrome + Active WebView into an ARGB8888 framebuffer */
bool arcr_shell_render_frame(ArcrBrowserShell *shell, uint32_t *argb_pixels,
                             int32_t width, int32_t height, int32_t stride_pixels);

#ifdef __cplusplus
}
#endif

#endif /* ARCR_SHELL_H */
