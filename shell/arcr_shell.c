/*
 * Arcradient Browser Shell
 * shell/arcr_shell.c - CPU Software UI Renderer (Modern Light/Dark & Win2000 Classic)
 */
#include "arcr_shell.h"
#include "../accel/arcr_accel.h"
#include "../base/arcr_base.h"
#include "../platform/arcr_platform.h"
#include <stdlib.h>
#include <string.h>

typedef struct ArcrBookmarkItem {
    char title[64];
    char url[256];
} ArcrBookmarkItem;

struct ArcrBrowserShell {
    ArcrEngine       *engine;
    ArcrShellConfig   cfg;
    ArcrAccelContext *ui_accel;
    ArcrFontSystem   *ui_fonts;

    ArcrWebView      *tabs[ARCR_SHELL_MAX_TABS];
    size_t            tab_count;
    size_t            active_tab;

    ArcrBookmarkItem  bookmarks[ARCR_SHELL_MAX_BOOKMARKS];
    size_t            bookmark_count;

    char              status_overlay[256];
};

typedef struct ShellPalette {
    bool     classic_3d_bevels;
    uint32_t titlebar_left;
    uint32_t titlebar_right;
    uint32_t titlebar_text;
    uint32_t tab_strip_bg;
    uint32_t tab_active_bg;
    uint32_t tab_active_border;
    uint32_t tab_active_accent; /* Firefox-inspired crisp active tab top indicator */
    uint32_t tab_inactive_bg;
    uint32_t tab_inactive_border;
    uint32_t toolbar_bg;
    uint32_t toolbar_divider;
    uint32_t omnibar_bg;
    uint32_t omnibar_border;
    uint32_t btn_hover_bg;
    uint32_t text_primary;
    uint32_t text_muted;
    uint32_t tls_verified_fg;
    uint32_t tls_verified_bg;
    uint32_t star_gold;
    uint32_t status_bg;
    uint32_t status_border;
    uint32_t status_text;
} ShellPalette;

static ShellPalette palette_for_theme(ArcrShellTheme theme) {
    if (theme == ARCR_SHELL_THEME_MODERN_DARK) {
        return (ShellPalette){
            .classic_3d_bevels   = false,
            .titlebar_left       = 0xFF0B1120u,
            .titlebar_right      = 0xFF0B1120u,
            .titlebar_text       = 0xFFF8FAFCu,
            .tab_strip_bg        = 0xFF0B1120u,
            .tab_active_bg       = 0xFF1E293Bu,
            .tab_active_border   = 0xFF334155u,
            .tab_active_accent   = 0xFF38BDF8u,
            .tab_inactive_bg     = 0xFF111827u,
            .tab_inactive_border = 0xFF1E293Bu,
            .toolbar_bg          = 0xFF1E293Bu,
            .toolbar_divider     = 0xFF334155u,
            .omnibar_bg          = 0xFF0F172Au,
            .omnibar_border      = 0xFF475569u,
            .btn_hover_bg        = 0xFF334155u,
            .text_primary        = 0xFFF1F5F9u,
            .text_muted          = 0xFF94A3B8u,
            .tls_verified_fg     = 0xFF4ADE80u,
            .tls_verified_bg     = 0xFF14532Du,
            .star_gold           = 0xFFFACC15u,
            .status_bg           = 0xFF0F172Au,
            .status_border       = 0xFF475569u,
            .status_text         = 0xFFE2E8F0u
        };
    }
    if (theme == ARCR_SHELL_THEME_WIN2000_CLASSIC) {
        /* Authentic Windows 2000 SP4 Classic System Colors (Opaque, Crisp 3D Bevels) */
        return (ShellPalette){
            .classic_3d_bevels   = true,
            .titlebar_left       = 0xFF0A246Au, /* Win2000 ActiveTitle Navy */
            .titlebar_right      = 0xFFA6CAF0u, /* Win2000 GradientActiveTitle */
            .titlebar_text       = 0xFFFFFFFFu,
            .tab_strip_bg        = 0xFFC8C4BCu,
            .tab_active_bg       = 0xFFD4D0C8u, /* COLOR_3DFACE */
            .tab_active_border   = 0xFF404040u,
            .tab_active_accent   = 0xFF0A246Au,
            .tab_inactive_bg     = 0xFFC0BCB4u,
            .tab_inactive_border = 0xFF808080u,
            .toolbar_bg          = 0xFFD4D0C8u,
            .toolbar_divider     = 0xFF808080u,
            .omnibar_bg          = 0xFFFFFFFFu, /* COLOR_WINDOW */
            .omnibar_border      = 0xFF808080u,
            .btn_hover_bg        = 0xFFD4D0C8u,
            .text_primary        = 0xFF000000u, /* COLOR_WINDOWTEXT */
            .text_muted          = 0xFF3A3A3Au,
            .tls_verified_fg     = 0xFF005500u,
            .tls_verified_bg     = 0xFFE4F0E4u,
            .star_gold           = 0xFF996600u,
            .status_bg           = 0xFFD4D0C8u,
            .status_border       = 0xFF404040u,
            .status_text         = 0xFF000000u
        };
    }
    /* Default: Modern Light (Chrome simplicity + Firefox clear tab boundaries) */
    return (ShellPalette){
        .classic_3d_bevels   = false,
        .titlebar_left       = 0xFFDCE3ECu,
        .titlebar_right      = 0xFFDCE3ECu,
        .titlebar_text       = 0xFF0F172Au,
        .tab_strip_bg        = 0xFFDCE3ECu,
        .tab_active_bg       = 0xFFFFFFFFu,
        .tab_active_border   = 0xFF94A3B8u,
        .tab_active_accent   = 0xFF2563EBu,
        .tab_inactive_bg     = 0xFFE9EEF5u,
        .tab_inactive_border = 0xFFCBD5E1u,
        .toolbar_bg          = 0xFFFFFFFFu,
        .toolbar_divider     = 0xFFCBD5E1u,
        .omnibar_bg          = 0xFFF1F5F9u,
        .omnibar_border      = 0xFFCBD5E1u,
        .btn_hover_bg        = 0xFFF1F5F9u,
        .text_primary        = 0xFF0F172Au,
        .text_muted          = 0xFF475569u,
        .tls_verified_fg     = 0xFF15803Du,
        .tls_verified_bg     = 0xFFDCFCE7u,
        .star_gold           = 0xFFD97706u,
        .status_bg           = 0xFF1E293Bu,
        .status_border       = 0xFF0F172Au,
        .status_text         = 0xFFF8FAFCu
    };
}

void arcr_shell_default_config(ArcrShellConfig *out_cfg, ArcrShellTheme theme) {
    if (!out_cfg) return;
    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->theme                = theme;
    out_cfg->window_width         = (theme == ARCR_SHELL_THEME_WIN2000_CLASSIC) ? 800 : 1024;
    out_cfg->window_height        = (theme == ARCR_SHELL_THEME_WIN2000_CLASSIC) ? 600 : 680;
    out_cfg->show_bookmarks_bar   = false; /* Hidden by default per Section 5.2! */
    out_cfg->show_home_button     = false;
    out_cfg->draw_os_window_frame = (theme == ARCR_SHELL_THEME_WIN2000_CLASSIC);
}

void arcr_shell_compute_layout(const ArcrBrowserShell *shell,
                               ArcrShellLayoutMetrics *m) {
    if (!shell || !m) return;
    memset(m, 0, sizeof(*m));

    int32_t w   = shell->cfg.window_width;
    int32_t h   = shell->cfg.window_height;
    int32_t pad = shell->cfg.draw_os_window_frame ? 3 : 0;
    int32_t y   = pad;
    int32_t iw  = w - pad * 2;

    /* Optional Win2000 Classic OS Titlebar when rendering full window frame */
    if (shell->cfg.draw_os_window_frame) {
        m->window_caption_rect = (ArcrRect){pad, y, iw, 20};
        y += 21;
    }

    /* 1. Compact Tab Strip (29px) */
    m->tab_strip_rect = (ArcrRect){pad, y, iw, 29};
    y += 29;

    /* 2. Compact Navigation Toolbar (34px) */
    m->nav_toolbar_rect = (ArcrRect){pad, y, iw, 34};
    y += 34;

    /* 3. Optional Bookmarks Bar (24px only when enabled) */
    if (shell->cfg.show_bookmarks_bar) {
        m->bookmarks_bar_rect = (ArcrRect){pad, y, iw, 24};
        y += 24;
    }

    /* 4. Web Content Viewport (All remaining height!) */
    int32_t content_h = ARCR_MAX(64, (h - pad) - y);
    m->content_viewport = (ArcrRect){pad, y, iw, content_h};

    /* 5. Ephemeral Bottom-Left Status Overlay */
    if (shell->status_overlay[0] != '\0') {
        int32_t sw = ARCR_MIN(iw - 16, 400);
        m->status_overlay_rect = (ArcrRect){pad + 6, (h - pad) - 24, sw, 20};
    }
}

ArcrBrowserShell *arcr_shell_create(ArcrEngine *engine, const ArcrShellConfig *cfg) {
    if (!engine) return NULL;
    ArcrBrowserShell *sh = (ArcrBrowserShell *)calloc(1u, sizeof(ArcrBrowserShell));
    if (!sh) return NULL;
    sh->engine = engine;
    if (cfg) sh->cfg = *cfg;
    else arcr_shell_default_config(&sh->cfg, ARCR_SHELL_THEME_MODERN_LIGHT);

    sh->ui_accel = arcr_accel_create(0xFFFFFFFFu, true, false);
    sh->ui_fonts = arcr_font_system_create(sh->cfg.default_cjk_font_path);
    if (!sh->ui_accel || !sh->ui_fonts) {
        arcr_shell_destroy(sh);
        return NULL;
    }
    return sh;
}

void arcr_shell_destroy(ArcrBrowserShell *sh) {
    if (!sh) return;
    for (size_t i = 0; i < sh->tab_count; ++i) {
        arcr_webview_destroy(sh->tabs[i]);
    }
    arcr_font_system_destroy(sh->ui_fonts);
    arcr_accel_destroy(sh->ui_accel);
    free(sh);
}

static void sync_all_tab_viewports(ArcrBrowserShell *sh) {
    ArcrShellLayoutMetrics m;
    arcr_shell_compute_layout(sh, &m);
    for (size_t i = 0; i < sh->tab_count; ++i) {
        arcr_webview_resize(sh->tabs[i], m.content_viewport.width, m.content_viewport.height);
    }
}

int arcr_shell_open_tab_html(ArcrBrowserShell *sh, const char *url,
                             const char *utf8_html, size_t html_len, bool activate) {
    if (!sh || sh->tab_count >= ARCR_SHELL_MAX_TABS) return -1;
    ArcrShellLayoutMetrics m;
    arcr_shell_compute_layout(sh, &m);

    ArcrWebView *v = arcr_webview_create(
        sh->engine, m.content_viewport.width, m.content_viewport.height, NULL);
    if (!v) return -1;
    arcr_webview_load_html(v, url, utf8_html, html_len);

    size_t idx = sh->tab_count++;
    sh->tabs[idx] = v;
    if (activate || sh->tab_count == 1u) {
        sh->active_tab = idx;
    }
    return (int)idx;
}

bool arcr_shell_activate_tab(ArcrBrowserShell *sh, size_t tab_index) {
    if (!sh || tab_index >= sh->tab_count) return false;
    sh->active_tab = tab_index;
    return true;
}

void arcr_shell_set_theme(ArcrBrowserShell *sh, ArcrShellTheme theme, bool draw_os_frame) {
    if (!sh) return;
    sh->cfg.theme                = theme;
    sh->cfg.draw_os_window_frame = draw_os_frame;
    sync_all_tab_viewports(sh);
}

ArcrShellTheme arcr_shell_get_theme(const ArcrBrowserShell *sh) {
    return sh ? sh->cfg.theme : ARCR_SHELL_THEME_MODERN_LIGHT;
}

void arcr_shell_set_bookmarks_bar_visible(ArcrBrowserShell *sh, bool visible) {
    if (!sh) return;
    sh->cfg.show_bookmarks_bar = visible;
    sync_all_tab_viewports(sh);
}

bool arcr_shell_is_bookmarks_bar_visible(const ArcrBrowserShell *sh) {
    return sh ? sh->cfg.show_bookmarks_bar : false;
}

void arcr_shell_add_bookmark(ArcrBrowserShell *sh, const char *title, const char *url) {
    if (!sh || sh->bookmark_count >= ARCR_SHELL_MAX_BOOKMARKS) return;
    ArcrBookmarkItem *b = &sh->bookmarks[sh->bookmark_count++];
    arcr_str_copy(b->title, sizeof(b->title), title ? title : "");
    arcr_str_copy(b->url, sizeof(b->url), url ? url : "");
}

void arcr_shell_set_status_overlay(ArcrBrowserShell *sh, const char *utf8_status_or_null) {
    if (!sh) return;
    arcr_str_copy(sh->status_overlay, sizeof(sh->status_overlay),
                  utf8_status_or_null ? utf8_status_or_null : "");
}

int arcr_shell_hit_test_tab(const ArcrBrowserShell *sh, int32_t x, int32_t y) {
    if (!sh || sh->tab_count == 0u) return -1;
    ArcrShellLayoutMetrics m;
    arcr_shell_compute_layout(sh, &m);
    ArcrRect ts = m.tab_strip_rect;
    if (y < ts.y || y >= ts.y + ts.height) return -1;
    int32_t tab_x = ts.x + 6;
    int32_t tab_w = ARCR_MIN(196, ARCR_MAX(110, (ts.width - 140) / (int32_t)ARCR_MAX(1u, sh->tab_count)));
    for (size_t i = 0; i < sh->tab_count; ++i) {
        if (x >= tab_x && x < tab_x + tab_w) {
            return (int)i;
        }
        tab_x += tab_w + 4;
    }
    return -1;
}

ArcrWebView *arcr_shell_active_webview(ArcrBrowserShell *sh) {
    if (!sh || sh->tab_count == 0u) return NULL;
    return sh->tabs[sh->active_tab];
}

/* -------------------------------------------------------------------------
 * Pure C CPU UI Drawing Helpers (Crisp 1px Borders, Classic Bevels & UTF-8 Text)
 * ------------------------------------------------------------------------- */
static void ui_fill(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                    ArcrRect r, uint32_t color) {
    arcr_accel_fill_rect(sh->ui_accel, fb, w, h, st, r, color);
}

static void ui_stroke_box(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                          ArcrRect r, uint32_t border_c, bool rounded_corners) {
    if (r.width <= 2 || r.height <= 2) return;
    int32_t inset = rounded_corners ? 1 : 0;
    ui_fill(sh, fb, w, h, st, (ArcrRect){r.x + inset, r.y, r.width - inset * 2, 1}, border_c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){r.x + inset, r.y + r.height - 1, r.width - inset * 2, 1}, border_c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){r.x, r.y + inset, 1, r.height - inset * 2}, border_c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){r.x + r.width - 1, r.y + inset, 1, r.height - inset * 2}, border_c);
}

/* Authentic Win32 DrawEdge(EDGE_RAISED / EDGE_SUNKEN) 2px crisp bevel for Win2000 Classic */
static void ui_win2k_bevel(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                           ArcrRect r, bool sunken) {
    uint32_t tl_outer = sunken ? 0xFF808080u : 0xFFFFFFFFu;
    uint32_t tl_inner = sunken ? 0xFF404040u : 0xFFD4D0C8u;
    uint32_t br_outer = sunken ? 0xFFFFFFFFu : 0xFF404040u;
    uint32_t br_inner = sunken ? 0xFFD4D0C8u : 0xFF808080u;

    ui_fill(sh, fb, w, h, st, (ArcrRect){r.x, r.y, r.width, 1}, tl_outer);
    ui_fill(sh, fb, w, h, st, (ArcrRect){r.x, r.y, 1, r.height}, tl_outer);
    ui_fill(sh, fb, w, h, st, (ArcrRect){r.x + 1, r.y + 1, r.width - 2, 1}, tl_inner);
    ui_fill(sh, fb, w, h, st, (ArcrRect){r.x + 1, r.y + 1, 1, r.height - 2}, tl_inner);

    ui_fill(sh, fb, w, h, st, (ArcrRect){r.x, r.y + r.height - 1, r.width, 1}, br_outer);
    ui_fill(sh, fb, w, h, st, (ArcrRect){r.x + r.width - 1, r.y, 1, r.height}, br_outer);
    ui_fill(sh, fb, w, h, st, (ArcrRect){r.x + 1, r.y + r.height - 2, r.width - 2, 1}, br_inner);
    ui_fill(sh, fb, w, h, st, (ArcrRect){r.x + r.width - 2, r.y + 1, 1, r.height - 2}, br_inner);
}

static int32_t ui_draw_text_clipped(ArcrBrowserShell *sh, uint32_t *fb,
                                    int32_t fb_w, int32_t fb_h, int32_t stride,
                                    const char *utf8, int32_t x, int32_t baseline_y,
                                    int32_t max_right_x, int32_t font_px, bool bold,
                                    uint32_t color_argb) {
    if (!utf8) return x;
    size_t len = strlen(utf8);
    size_t pos = 0u;
    int32_t pen_x = x;
    uint8_t mask[64 * 64];

    while (pos < len) {
        uint32_t cp = 0u;
        size_t step = arcr_utf8_decode_one(utf8 + pos, len - pos, &cp);
        if (step == 0u) break;
        pos += step;

        ArcrGlyphMetrics gm;
        if (!arcr_font_rasterize_glyph(sh->ui_fonts, cp, font_px, bold,
                                       &gm, mask, sizeof(mask))) {
            continue;
        }
        if (pen_x + gm.advance_px > max_right_x) break;
        if (gm.width > 0 && gm.height > 0 && cp != ' ') {
            arcr_accel_blit_glyph_alpha(sh->ui_accel, fb, fb_w, fb_h, stride,
                                        pen_x + gm.bearing_x, baseline_y - gm.bearing_y,
                                        mask, gm.width, gm.height, color_argb);
        }
        pen_x += gm.advance_px;
    }
    return pen_x;
}

/* Crisp pixel-aligned 12x12 padlock icon for HTTPS badge */
static void ui_draw_lock_icon(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                              int32_t x, int32_t y, uint32_t color) {
    /* Shackle */
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 3, y + 1, 6, 1}, color);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 2, y + 2, 1, 3}, color);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 9, y + 2, 1, 3}, color);
    /* Body */
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 1, y + 5, 10, 6}, color);
}

/* Crisp pixel-aligned hamburger menu icon (≡) */
static void ui_draw_menu_icon(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                              int32_t x, int32_t y, uint32_t color) {
    ui_fill(sh, fb, w, h, st, (ArcrRect){x, y,      12, 2}, color);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x, y + 4,  12, 2}, color);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x, y + 8,  12, 2}, color);
}

/* Pixel-aligned 8x8 window caption icons: 0=Minimize, 1=Maximize, 2=Close */
static void ui_draw_caption_glyph(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h,
                                  int32_t st, int kind, int32_t x, int32_t y, uint32_t color) {
    if (kind == 0) {
        ui_fill(sh, fb, w, h, st, (ArcrRect){x, y + 6, 8, 2}, color);
    } else if (kind == 1) {
        ui_fill(sh, fb, w, h, st, (ArcrRect){x, y, 8, 2}, color);
        ui_fill(sh, fb, w, h, st, (ArcrRect){x, y + 7, 8, 1}, color);
        ui_fill(sh, fb, w, h, st, (ArcrRect){x, y, 1, 8}, color);
        ui_fill(sh, fb, w, h, st, (ArcrRect){x + 7, y, 1, 8}, color);
    } else {
        for (int32_t d = 0; d < 8; ++d) {
            ui_fill(sh, fb, w, h, st, (ArcrRect){x + d, y + d, 2, 1}, color);
            ui_fill(sh, fb, w, h, st, (ArcrRect){x + (6 - d >= 0 ? 6 - d : 0), y + d, 2, 1}, color);
        }
    }
}

bool arcr_shell_render_frame(ArcrBrowserShell *sh, uint32_t *fb,
                             int32_t w, int32_t h, int32_t st) {
    if (!sh || !fb || w <= 0 || h <= 0) return false;
    if (w != sh->cfg.window_width || h != sh->cfg.window_height) {
        sh->cfg.window_width  = w;
        sh->cfg.window_height = h;
        sync_all_tab_viewports(sh);
    }

    ShellPalette pal = palette_for_theme(sh->cfg.theme);
    ArcrShellLayoutMetrics m;
    arcr_shell_compute_layout(sh, &m);

    /* Outer window background / Win2000 Classic outer 3D frame */
    ui_fill(sh, fb, w, h, st, (ArcrRect){0, 0, w, h}, pal.toolbar_bg);
    if (sh->cfg.draw_os_window_frame && pal.classic_3d_bevels) {
        ui_win2k_bevel(sh, fb, w, h, st, (ArcrRect){0, 0, w, h}, false);
        /* Win2000 Classic Navy -> Light-Blue horizontal gradient titlebar */
        ArcrRect cap = m.window_caption_rect;
        for (int32_t cx = 0; cx < cap.width; ++cx) {
            uint32_t r0 = (pal.titlebar_left >> 16) & 0xFFu;
            uint32_t g0 = (pal.titlebar_left >> 8) & 0xFFu;
            uint32_t b0 = pal.titlebar_left & 0xFFu;
            uint32_t r1 = (pal.titlebar_right >> 16) & 0xFFu;
            uint32_t g1 = (pal.titlebar_right >> 8) & 0xFFu;
            uint32_t b1 = pal.titlebar_right & 0xFFu;
            uint32_t cr = r0 + ((r1 - r0) * (uint32_t)cx) / (uint32_t)ARCR_MAX(1, cap.width);
            uint32_t cg = g0 + ((g1 - g0) * (uint32_t)cx) / (uint32_t)ARCR_MAX(1, cap.width);
            uint32_t cb = b0 + ((b1 - b0) * (uint32_t)cx) / (uint32_t)ARCR_MAX(1, cap.width);
            ui_fill(sh, fb, w, h, st, (ArcrRect){cap.x + cx, cap.y, 1, cap.height},
                    0xFF000000u | (cr << 16) | (cg << 8) | cb);
        }
        ui_draw_text_clipped(sh, fb, w, h, st, "Arcradient Browser (MSHTML-Next · Windows 2000 Classic)",
                             cap.x + 6, cap.y + 15, cap.x + cap.width - 60, 12, true, pal.titlebar_text);

        /* Classic [_] [□] [X] Win32 caption buttons */
        for (int bi = 0; bi < 3; ++bi) {
            ArcrRect br = {cap.x + cap.width - 54 + bi * 18, cap.y + 2, 16, 16};
            ui_fill(sh, fb, w, h, st, br, pal.toolbar_bg);
            ui_win2k_bevel(sh, fb, w, h, st, br, false);
            ui_draw_caption_glyph(sh, fb, w, h, st, bi, br.x + 4, br.y + 4, 0xFF000000u);
        }
    }

    /* 1. Tab Strip */
    ArcrRect ts = m.tab_strip_rect;
    ui_fill(sh, fb, w, h, st, ts, pal.tab_strip_bg);
    ui_fill(sh, fb, w, h, st, (ArcrRect){ts.x, ts.y + ts.height - 1, ts.width, 1}, pal.toolbar_divider);

    int32_t tab_x = ts.x + 6;
    int32_t tab_w = ARCR_MIN(196, ARCR_MAX(110, (ts.width - 140) / (int32_t)ARCR_MAX(1u, sh->tab_count)));
    for (size_t i = 0; i < sh->tab_count; ++i) {
        bool is_active = (i == sh->active_tab);
        ArcrRect tr = {
            .x      = tab_x,
            .y      = ts.y + (is_active ? 3 : 5),
            .width  = tab_w,
            .height = ts.height - (is_active ? 3 : 5)
        };
        ui_fill(sh, fb, w, h, st, tr, is_active ? pal.tab_active_bg : pal.tab_inactive_bg);
        if (pal.classic_3d_bevels) {
            ui_win2k_bevel(sh, fb, w, h, st, tr, false);
            if (is_active) {
                /* Connect active tab seamlessly into toolbar below */
                ui_fill(sh, fb, w, h, st, (ArcrRect){tr.x + 2, tr.y + tr.height - 2, tr.width - 4, 3},
                        pal.toolbar_bg);
            }
        } else {
            ui_stroke_box(sh, fb, w, h, st, tr,
                          is_active ? pal.tab_active_border : pal.tab_inactive_border, true);
            if (is_active) {
                /* Firefox-like crisp active tab top bar + seamless bottom merge */
                ui_fill(sh, fb, w, h, st, (ArcrRect){tr.x + 2, tr.y, tr.width - 4, 2}, pal.tab_active_accent);
                ui_fill(sh, fb, w, h, st, (ArcrRect){tr.x + 1, tr.y + tr.height - 1, tr.width - 2, 2},
                        pal.toolbar_bg);
            }
        }

        const char *ttitle = arcr_webview_get_title(sh->tabs[i]);
        ui_draw_text_clipped(sh, fb, w, h, st, ttitle,
                             tr.x + 10, tr.y + tr.height - 8, tr.x + tr.width - 22,
                             12, is_active, is_active ? pal.text_primary : pal.text_muted);
        ui_draw_text_clipped(sh, fb, w, h, st, "x",
                             tr.x + tr.width - 15, tr.y + tr.height - 8, tr.x + tr.width - 4,
                             11, false, pal.text_muted);
        tab_x += tab_w + 4;
    }

    /* '+' New Tab Button */
    ArcrRect plus_r = {tab_x + 2, ts.y + 6, 22, 20};
    ui_fill(sh, fb, w, h, st, plus_r, pal.tab_inactive_bg);
    if (pal.classic_3d_bevels) ui_win2k_bevel(sh, fb, w, h, st, plus_r, false);
    else ui_stroke_box(sh, fb, w, h, st, plus_r, pal.tab_inactive_border, true);
    ui_draw_text_clipped(sh, fb, w, h, st, "+", plus_r.x + 7, plus_r.y + 14, plus_r.x + 20,
                         13, true, pal.text_primary);

    /* Modern window controls when OS frame is integrated into tab strip */
    if (!sh->cfg.draw_os_window_frame) {
        int32_t wx = ts.x + ts.width - 78;
        for (int bi = 0; bi < 3; ++bi) {
            ui_draw_caption_glyph(sh, fb, w, h, st, bi, wx + bi * 24, ts.y + 10, pal.text_muted);
        }
    }

    /* 2. Navigation Toolbar */
    ArcrRect nv = m.nav_toolbar_rect;
    ui_fill(sh, fb, w, h, st, nv, pal.toolbar_bg);
    ui_fill(sh, fb, w, h, st, (ArcrRect){nv.x, nv.y + nv.height - 1, nv.width, 1}, pal.toolbar_divider);

    const char *nav_labels[2] = {"<", ">"};
    int32_t nx = nv.x + 6;
    for (int bi = 0; bi < 3; ++bi) {
        ArcrRect br = {nx, nv.y + 5, 24, 24};
        ui_fill(sh, fb, w, h, st, br, pal.btn_hover_bg);
        if (pal.classic_3d_bevels) ui_win2k_bevel(sh, fb, w, h, st, br, false);
        else ui_stroke_box(sh, fb, w, h, st, br, pal.omnibar_border, true);
        if (bi < 2) {
            ui_draw_text_clipped(sh, fb, w, h, st, nav_labels[bi],
                                 br.x + 8, br.y + 17, br.x + 22, 12, true, pal.text_primary);
        } else {
            /* Crisp pixel-aligned 10x10 circular reload arrow */
            int32_t rx = br.x + 7, ry = br.y + 7;
            ui_fill(sh, fb, w, h, st, (ArcrRect){rx + 2, ry,     6, 2}, pal.text_primary);
            ui_fill(sh, fb, w, h, st, (ArcrRect){rx + 2, ry + 8, 6, 2}, pal.text_primary);
            ui_fill(sh, fb, w, h, st, (ArcrRect){rx,     ry + 2, 2, 6}, pal.text_primary);
            ui_fill(sh, fb, w, h, st, (ArcrRect){rx + 8, ry + 5, 2, 3}, pal.text_primary);
            /* Arrowhead top-right */
            ui_fill(sh, fb, w, h, st, (ArcrRect){rx + 6, ry - 1, 4, 4}, pal.text_primary);
        }
        nx += 28;
    }

    /* Unified URL + Search Omnibar */
    int32_t menu_w = 30;
    ArcrRect omni = {nx + 4, nv.y + 4, (nv.x + nv.width - menu_w - 10) - (nx + 4), 25};
    ui_fill(sh, fb, w, h, st, omni, pal.omnibar_bg);
    if (pal.classic_3d_bevels) ui_win2k_bevel(sh, fb, w, h, st, omni, true);
    else ui_stroke_box(sh, fb, w, h, st, omni, pal.omnibar_border, true);

    ArcrWebView *active_v = arcr_shell_active_webview(sh);
    ArcrSecurityState sec = active_v ? arcr_webview_get_security_state(active_v) : ARCR_SEC_LOCAL_OR_NONE;
    const char *cur_url   = active_v ? arcr_webview_get_url(active_v) : "about:blank";

    int32_t url_text_x = omni.x + 8;
    if (sec == ARCR_SEC_HTTPS_VERIFIED) {
        ArcrRect badge = {omni.x + 4, omni.y + 3, 66, omni.height - 6};
        ui_fill(sh, fb, w, h, st, badge, pal.tls_verified_bg);
        ui_draw_lock_icon(sh, fb, w, h, st, badge.x + 4, badge.y + 3, pal.tls_verified_fg);
        ui_draw_text_clipped(sh, fb, w, h, st, "TLS 1.3", badge.x + 19, badge.y + 14,
                             badge.x + 64, 11, true, pal.tls_verified_fg);
        url_text_x = badge.x + badge.width + 8;
    }
    ui_draw_text_clipped(sh, fb, w, h, st, cur_url,
                         url_text_x, omni.y + 17, omni.x + omni.width - 30,
                         13, false, pal.text_primary);
    /* Bookmark Star inside Omnibar right edge */
    ui_draw_text_clipped(sh, fb, w, h, st, "★",
                         omni.x + omni.width - 22, omni.y + 17, omni.x + omni.width - 4,
                         13, true, pal.star_gold);

    /* Single Unified Menu Button (≡) */
    ArcrRect menu_r = {nv.x + nv.width - 32, nv.y + 5, 26, 24};
    ui_fill(sh, fb, w, h, st, menu_r, pal.btn_hover_bg);
    if (pal.classic_3d_bevels) ui_win2k_bevel(sh, fb, w, h, st, menu_r, false);
    else ui_stroke_box(sh, fb, w, h, st, menu_r, pal.omnibar_border, true);
    ui_draw_menu_icon(sh, fb, w, h, st, menu_r.x + 7, menu_r.y + 7, pal.text_primary);

    /* 3. Optional Bookmarks Bar */
    if (sh->cfg.show_bookmarks_bar) {
        ArcrRect bm = m.bookmarks_bar_rect;
        ui_fill(sh, fb, w, h, st, bm, pal.toolbar_bg);
        ui_fill(sh, fb, w, h, st, (ArcrRect){bm.x, bm.y + bm.height - 1, bm.width, 1},
                pal.toolbar_divider);
        int32_t bx = bm.x + 10;
        for (size_t i = 0; i < sh->bookmark_count; ++i) {
            bx = ui_draw_text_clipped(sh, fb, w, h, st, sh->bookmarks[i].title,
                                      bx, bm.y + 16, bm.x + bm.width - 12,
                                      12, false, pal.text_muted) + 18;
        }
    }

    /* 4. Web Content Viewport rendered by Arcradient Engine! */
    if (active_v) {
        uint32_t *vp_ptr = fb + (size_t)m.content_viewport.y * (size_t)st +
                           (size_t)m.content_viewport.x;
        arcr_webview_paint(active_v, vp_ptr,
                           m.content_viewport.width, m.content_viewport.height, st);
    }

    /* 5. Ephemeral Bottom-Left Status Overlay (Shown ONLY when active!) */
    if (sh->status_overlay[0] != '\0') {
        ArcrRect sr = m.status_overlay_rect;
        ui_fill(sh, fb, w, h, st, sr, pal.status_bg);
        if (pal.classic_3d_bevels) ui_win2k_bevel(sh, fb, w, h, st, sr, true);
        else ui_stroke_box(sh, fb, w, h, st, sr, pal.status_border, true);
        ui_draw_text_clipped(sh, fb, w, h, st, sh->status_overlay,
                             sr.x + 8, sr.y + 14, sr.x + sr.width - 6,
                             11, false, pal.status_text);
    }

    return true;
}
