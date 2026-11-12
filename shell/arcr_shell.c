/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * shell/arcr_shell.c - Firefox-Class Pure C11 Browser Shell, Internal Pages,
 *                      Persistent Profile, Add-ons UI & Hardware Telemetry Dock
 */
#include "arcr_shell.h"
#include "../accel/arcr_accel.h"
#include "../base/arcr_base.h"
#include "../include/waterscape_modules.h"
#include "../platform/arcr_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum WsFocusTarget {
    WS_FOCUS_WEBVIEW = 0,
    WS_FOCUS_OMNIBAR = 1,
    WS_FOCUS_FINDBAR = 2
} WsFocusTarget;

typedef struct ArcrBookmarkItem {
    char title[96];
    char url[384];
} ArcrBookmarkItem;

typedef struct ArcrHistoryItem {
    int64_t timestamp;
    char    title[96];
    char    url[384];
} ArcrHistoryItem;

typedef struct WsTabSlot {
    ArcrWebView      *view;
    ArcrBrowserShell *owner;
    char              current_url[512];
    char              hist_urls[ARCR_SHELL_TAB_HIST_CAP][512];
    size_t            hist_count;
    size_t            hist_pos;
    bool              has_inline_html;
} WsTabSlot;

struct ArcrBrowserShell {
    ArcrEngine       *engine;
    ArcrShellConfig   cfg;
    ArcrAccelContext *ui_accel;
    ArcrFontSystem   *ui_fonts;
    WsAddonManager   *addons;

    WsTabSlot         tabs[ARCR_SHELL_MAX_TABS];
    size_t            tab_count;
    size_t            active_tab;

    ArcrBookmarkItem  bookmarks[ARCR_SHELL_MAX_BOOKMARKS];
    size_t            bookmark_count;

    ArcrHistoryItem   history[ARCR_SHELL_MAX_HISTORY];
    size_t            history_count;

    char              profile_dir[512];
    int32_t           default_zoom_pct;

    /* Focus, Editable Omnibar & Find-in-Page state */
    WsFocusTarget     focus;
    char              omni_buf[512];
    size_t            omni_len;
    size_t            omni_cursor;
    bool              omni_select_all;

    bool              show_find_bar;
    char              find_buf[128];
    size_t            find_len;
    size_t            find_cursor;
    uint32_t          find_total_matches;
    uint32_t          find_active_index;

    /* Mouse hover & Scrollbar drag state */
    int32_t           mouse_x;
    int32_t           mouse_y;
    bool              dragging_scrollbar;
    int32_t           drag_grab_offset_y;

    char              status_overlay[256];
    bool              status_from_hover;
};

typedef struct ShellPalette {
    bool     classic_3d_bevels;
    uint32_t titlebar_left;
    uint32_t titlebar_right;
    uint32_t titlebar_text;
    uint32_t tab_strip_bg;
    uint32_t tab_active_bg;
    uint32_t tab_active_border;
    uint32_t tab_active_accent;
    uint32_t tab_inactive_bg;
    uint32_t tab_inactive_border;
    uint32_t toolbar_bg;
    uint32_t toolbar_divider;
    uint32_t omnibar_bg;
    uint32_t omnibar_border;
    uint32_t omnibar_focus_border;
    uint32_t selection_bg;
    uint32_t btn_bg;
    uint32_t btn_active_bg;
    uint32_t btn_active_fg;
    uint32_t text_primary;
    uint32_t text_muted;
    uint32_t tls_verified_fg;
    uint32_t tls_verified_bg;
    uint32_t sec_internal_fg;
    uint32_t sec_internal_bg;
    uint32_t sec_warn_fg;
    uint32_t sec_warn_bg;
    uint32_t private_bg;
    uint32_t private_fg;
    uint32_t star_gold;
    uint32_t status_bg;
    uint32_t status_border;
    uint32_t status_text;
    uint32_t dock_bg;
    uint32_t dock_panel_bg;
    uint32_t dock_accent;
    uint32_t scrollbar_track;
    uint32_t scrollbar_thumb;
} ShellPalette;

typedef struct ToolbarControls {
    ArcrRect plus_btn;
    ArcrRect back_btn;
    ArcrRect fwd_btn;
    ArcrRect reload_btn;
    bool     has_home_btn;
    ArcrRect home_btn;
    ArcrRect omnibar;
    ArcrRect star_btn;
    bool     has_ext_btns;
    ArcrRect shield_btn;
    ArcrRect reader_btn;
    ArcrRect dark_btn;
    ArcrRect addons_btn;
    ArcrRect menu_btn;
    /* Find bar controls */
    ArcrRect find_input;
    ArcrRect find_prev_btn;
    ArcrRect find_next_btn;
    ArcrRect find_close_btn;
} ToolbarControls;

static ShellPalette palette_for_theme(ArcrShellTheme theme) {
    if (theme == ARCR_SHELL_THEME_MODERN_DARK) {
        return (ShellPalette){
            .classic_3d_bevels    = false,
            .titlebar_left        = 0xFF0B1120u,
            .titlebar_right       = 0xFF0B1120u,
            .titlebar_text        = 0xFFF8FAFCu,
            .tab_strip_bg         = 0xFF090E1Au,
            .tab_active_bg        = 0xFF1E293Bu,
            .tab_active_border    = 0xFF334155u,
            .tab_active_accent    = 0xFF38BDF8u,
            .tab_inactive_bg      = 0xFF0F172Au,
            .tab_inactive_border  = 0xFF1E293Bu,
            .toolbar_bg           = 0xFF1E293Bu,
            .toolbar_divider      = 0xFF334155u,
            .omnibar_bg           = 0xFF0B1120u,
            .omnibar_border       = 0xFF475569u,
            .omnibar_focus_border = 0xFF38BDF8u,
            .selection_bg         = 0xFF1D4ED8u,
            .btn_bg               = 0xFF273549u,
            .btn_active_bg        = 0xFF0369A1u,
            .btn_active_fg        = 0xFFE0F2FEu,
            .text_primary         = 0xFFF1F5F9u,
            .text_muted           = 0xFF94A3B8u,
            .tls_verified_fg      = 0xFF4ADE80u,
            .tls_verified_bg      = 0xFF14532Du,
            .sec_internal_fg      = 0xFF38BDF8u,
            .sec_internal_bg      = 0xFF0C4A6Eu,
            .sec_warn_fg          = 0xFFFBBF24u,
            .sec_warn_bg          = 0xFF78350Fu,
            .private_bg           = 0xFF581C87u,
            .private_fg           = 0xFFF3E8FFu,
            .star_gold            = 0xFFFACC15u,
            .status_bg            = 0xFF0F172Au,
            .status_border        = 0xFF475569u,
            .status_text          = 0xFFE2E8F0u,
            .dock_bg              = 0xFF090E1Au,
            .dock_panel_bg        = 0xFF111827u,
            .dock_accent          = 0xFF38BDF8u,
            .scrollbar_track      = 0xFF0F172Au,
            .scrollbar_thumb      = 0xFF475569u
        };
    }
    if (theme == ARCR_SHELL_THEME_WIN2000_CLASSIC) {
        return (ShellPalette){
            .classic_3d_bevels    = true,
            .titlebar_left        = 0xFF0A246Au,
            .titlebar_right       = 0xFFA6CAF0u,
            .titlebar_text        = 0xFFFFFFFFu,
            .tab_strip_bg         = 0xFFC8C4BCu,
            .tab_active_bg        = 0xFFD4D0C8u,
            .tab_active_border    = 0xFF404040u,
            .tab_active_accent    = 0xFF0A246Au,
            .tab_inactive_bg      = 0xFFC0BCB4u,
            .tab_inactive_border  = 0xFF808080u,
            .toolbar_bg           = 0xFFD4D0C8u,
            .toolbar_divider      = 0xFF808080u,
            .omnibar_bg           = 0xFFFFFFFFu,
            .omnibar_border       = 0xFF808080u,
            .omnibar_focus_border = 0xFF0A246Au,
            .selection_bg         = 0xFF0A246Au,
            .btn_bg               = 0xFFD4D0C8u,
            .btn_active_bg        = 0xFFB8C4D8u,
            .btn_active_fg        = 0xFF0A246Au,
            .text_primary         = 0xFF000000u,
            .text_muted           = 0xFF3A3A3Au,
            .tls_verified_fg      = 0xFF005500u,
            .tls_verified_bg      = 0xFFE4F0E4u,
            .sec_internal_fg      = 0xFF0A246Au,
            .sec_internal_bg      = 0xFFDCE6F5u,
            .sec_warn_fg          = 0xFF803000u,
            .sec_warn_bg          = 0xFFFFF0D8u,
            .private_bg           = 0xFF4B0082u,
            .private_fg           = 0xFFFFFFFFu,
            .star_gold            = 0xFF996600u,
            .status_bg            = 0xFFD4D0C8u,
            .status_border        = 0xFF404040u,
            .status_text          = 0xFF000000u,
            .dock_bg              = 0xFFD4D0C8u,
            .dock_panel_bg        = 0xFFFFFFFFu,
            .dock_accent          = 0xFF0A246Au,
            .scrollbar_track      = 0xFFE4E0D8u,
            .scrollbar_thumb      = 0xFFD4D0C8u
        };
    }
    return (ShellPalette){
        .classic_3d_bevels    = false,
        .titlebar_left        = 0xFFDCE4EEu,
        .titlebar_right       = 0xFFDCE4EEu,
        .titlebar_text        = 0xFF0F172Au,
        .tab_strip_bg         = 0xFFD8E1ECu,
        .tab_active_bg        = 0xFFFFFFFFu,
        .tab_active_border    = 0xFF94A3B8u,
        .tab_active_accent    = 0xFF0284C7u,
        .tab_inactive_bg      = 0xFFE6EDF5u,
        .tab_inactive_border  = 0xFFCBD5E1u,
        .toolbar_bg           = 0xFFFFFFFFu,
        .toolbar_divider      = 0xFFCBD5E1u,
        .omnibar_bg           = 0xFFF1F5F9u,
        .omnibar_border       = 0xFFCBD5E1u,
        .omnibar_focus_border = 0xFF0284C7u,
        .selection_bg         = 0xFFBFDBFEu,
        .btn_bg               = 0xFFF1F5F9u,
        .btn_active_bg        = 0xFFE0F2FEu,
        .btn_active_fg        = 0xFF0369A1u,
        .text_primary         = 0xFF0F172Au,
        .text_muted           = 0xFF475569u,
        .tls_verified_fg      = 0xFF15803Du,
        .tls_verified_bg      = 0xFFDCFCE7u,
        .sec_internal_fg      = 0xFF0369A1u,
        .sec_internal_bg      = 0xFFE0F2FEu,
        .sec_warn_fg          = 0xFFB45309u,
        .sec_warn_bg          = 0xFFFEF3C7u,
        .private_bg           = 0xFF6B21A8u,
        .private_fg           = 0xFFFAF5FFu,
        .star_gold            = 0xFFD97706u,
        .status_bg            = 0xFF1E293Bu,
        .status_border        = 0xFF0F172Au,
        .status_text          = 0xFFF8FAFCu,
        .dock_bg              = 0xFFF1F5F9u,
        .dock_panel_bg        = 0xFFFFFFFFu,
        .dock_accent          = 0xFF0284C7u,
        .scrollbar_track      = 0xFFF1F5F9u,
        .scrollbar_thumb      = 0xFF94A3B8u
    };
}

static bool rect_contains(ArcrRect r, int32_t x, int32_t y) {
    return r.width > 0 && r.height > 0 &&
           x >= r.x && x < r.x + r.width &&
           y >= r.y && y < r.y + r.height;
}

static bool is_internal_url(const char *url) {
    if (!url || url[0] == '\0') return true;
    return arcr_str_starts_with_nocase(url, "waterscape://") ||
           arcr_str_starts_with_nocase(url, "arcr://") ||
           arcr_str_starts_with_nocase(url, "about:");
}

/* -------------------------------------------------------------------------
 * Dynamic String Builder & HTML / URL Escaping Helpers
 * ------------------------------------------------------------------------- */
typedef struct StrBuf {
    char  *data;
    size_t len;
    size_t cap;
} StrBuf;

static bool sb_init(StrBuf *sb, size_t initial_cap) {
    sb->cap  = initial_cap < 256u ? 256u : initial_cap;
    sb->len  = 0u;
    sb->data = (char *)malloc(sb->cap);
    if (!sb->data) return false;
    sb->data[0] = '\0';
    return true;
}

static bool sb_reserve(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1u <= sb->cap) return true;
    size_t next_cap = sb->cap * 2u;
    while (next_cap < sb->len + extra + 1u) next_cap *= 2u;
    char * grown = (char *)realloc(sb->data, next_cap);
    if (!grown) return false;
    sb->data = grown;
    sb->cap  = next_cap;
    return true;
}

static void sb_append_n(StrBuf *sb, const char *s, size_t n) {
    if (!s || n == 0u || !sb_reserve(sb, n)) return;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_append(StrBuf *sb, const char *s) {
    if (s) sb_append_n(sb, s, strlen(s));
}

static void sb_append_html_escaped(StrBuf *sb, const char *s) {
    if (!s) return;
    for (size_t i = 0u; s[i] != '\0'; ++i) {
        char c = s[i];
        if (c == '&')      sb_append(sb, "&amp;");
        else if (c == '<') sb_append(sb, "&lt;");
        else if (c == '>') sb_append(sb, "&gt;");
        else if (c == '"') sb_append(sb, "&quot;");
        else               sb_append_n(sb, &c, 1u);
    }
}

static void sb_append_u64(StrBuf *sb, uint64_t val) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)val);
    sb_append(sb, tmp);
}

static void url_encode_query(const char *raw, char *out, size_t out_cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0u;
    if (!out || out_cap == 0u) return;
    for (size_t i = 0u; raw && raw[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)raw[i];
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (c == ' ') {
            if (pos + 1u >= out_cap) break;
            out[pos++] = '+';
        } else if (safe) {
            if (pos + 1u >= out_cap) break;
            out[pos++] = (char)c;
        } else {
            if (pos + 3u >= out_cap) break;
            out[pos++] = '%';
            out[pos++] = hex[(c >> 4) & 0x0Fu];
            out[pos++] = hex[c & 0x0Fu];
        }
    }
    out[pos] = '\0';
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void url_decode_query(const char *enc, char *out, size_t out_cap) {
    size_t pos = 0u;
    if (!out || out_cap == 0u) return;
    for (size_t i = 0u; enc && enc[i] != '\0' && pos + 1u < out_cap; ++i) {
        if (enc[i] == '+') {
            out[pos++] = ' ';
        } else if (enc[i] == '%' && enc[i + 1] != '\0' && enc[i + 2] != '\0') {
            int hi = hex_nibble(enc[i + 1]);
            int lo = hex_nibble(enc[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[pos++] = (char)((hi << 4) | lo);
                i += 2u;
                continue;
            }
            out[pos++] = enc[i];
        } else {
            out[pos++] = enc[i];
        }
    }
    out[pos] = '\0';
}

static void build_prefixed_url(char *out_url, size_t out_cap,
                               const char *prefix, const char *rest) {
    size_t plen = arcr_str_copy(out_url, out_cap, prefix);
    if (plen + 1u < out_cap) {
        arcr_str_copy(out_url + plen, out_cap - plen, rest);
    }
}

/* Smart Omnibar Routing: internal schemes, localhost, domain names, or DuckDuckGo search */
static void resolve_omnibar_target(const char *input, char *out_url, size_t out_cap) {
    if (!out_url || out_cap == 0u) return;
    if (!input) {
        arcr_str_copy(out_url, out_cap, "waterscape://newtab");
        return;
    }
    const char *p   = input;
    size_t      len = strlen(input);
    arcr_str_trim_view(&p, &len);
    if (len == 0u) {
        arcr_str_copy(out_url, out_cap, "waterscape://newtab");
        return;
    }

    char trimmed[512];
    size_t copy_n = len < sizeof(trimmed) - 1u ? len : sizeof(trimmed) - 1u;
    memcpy(trimmed, p, copy_n);
    trimmed[copy_n] = '\0';

    if (arcr_str_starts_with_nocase(trimmed, "waterscape://") ||
        arcr_str_starts_with_nocase(trimmed, "arcr://") ||
        arcr_str_starts_with_nocase(trimmed, "about:") ||
        arcr_str_starts_with_nocase(trimmed, "http://") ||
        arcr_str_starts_with_nocase(trimmed, "https://") ||
        arcr_str_starts_with_nocase(trimmed, "file://") ||
        arcr_str_starts_with_nocase(trimmed, "data:")) {
        arcr_str_copy(out_url, out_cap, trimmed);
        return;
    }

    bool has_space = false;
    for (size_t i = 0u; trimmed[i] != '\0'; ++i) {
        if (trimmed[i] == ' ' || trimmed[i] == '\t') {
            has_space = true;
            break;
        }
    }

    if (!has_space) {
        if (arcr_str_starts_with_nocase(trimmed, "localhost") &&
            (trimmed[9] == '\0' || trimmed[9] == ':' || trimmed[9] == '/')) {
            build_prefixed_url(out_url, out_cap, "http://", trimmed);
            return;
        }
        if (strncmp(trimmed, "127.0.0.1", 9u) == 0 &&
            (trimmed[9] == '\0' || trimmed[9] == ':' || trimmed[9] == '/')) {
            build_prefixed_url(out_url, out_cap, "http://", trimmed);
            return;
        }
        /* Check if host part contains a dot and valid domain chars */
        const char *dot      = NULL;
        bool        host_ok  = true;
        size_t      host_end = 0u;
        for (; trimmed[host_end] != '\0' && trimmed[host_end] != '/' &&
               trimmed[host_end] != '?' && trimmed[host_end] != '#'; ++host_end) {
            unsigned char c = (unsigned char)trimmed[host_end];
            if (c == '.') {
                dot = &trimmed[host_end];
            } else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == ':')) {
                host_ok = false;
                break;
            }
        }
        if (host_ok && dot && dot > trimmed && (size_t)((trimmed + host_end) - (dot + 1)) >= 2u) {
            build_prefixed_url(out_url, out_cap, "https://", trimmed);
            return;
        }
    }

    char encoded[768];
    url_encode_query(trimmed, encoded, sizeof(encoded));
    build_prefixed_url(out_url, out_cap, "https://duckduckgo.com/html/?q=", encoded);
}

/* -------------------------------------------------------------------------
 * Persistent User Profile (~/.waterscape/prefs.ini, bookmarks.tsv, history.tsv)
 * ------------------------------------------------------------------------- */
static void strip_line_endings(char *line) {
    size_t n = strlen(line);
    while (n > 0u && (line[n - 1u] == '\n' || line[n - 1u] == '\r')) {
        line[--n] = '\0';
    }
}

static void sanitize_tsv_field(const char *src, char *dst, size_t cap) {
    arcr_str_copy(dst, cap, src ? src : "");
    for (size_t i = 0u; dst[i] != '\0'; ++i) {
        if (dst[i] == '\t' || dst[i] == '\r' || dst[i] == '\n') dst[i] = ' ';
    }
}

static bool save_prefs_file(const ArcrBrowserShell *sh) {
    if (!sh || sh->cfg.private_browsing || sh->profile_dir[0] == '\0') return false;
    arcr_platform_mkdir_p(sh->profile_dir);
    char path[640];
    snprintf(path, sizeof(path), "%s/prefs.ini", sh->profile_dir);
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    fprintf(fp, "# Waterscape Browser 1.0 User Preferences\n");
    fprintf(fp, "theme=%d\n", (int)sh->cfg.theme);
    fprintf(fp, "bookmarks_bar=%d\n", sh->cfg.show_bookmarks_bar ? 1 : 0);
    fprintf(fp, "zoom_percent=%d\n", (int)sh->default_zoom_pct);
    fclose(fp);
    return true;
}

static bool save_bookmarks_file(const ArcrBrowserShell *sh) {
    if (!sh || sh->cfg.private_browsing || sh->profile_dir[0] == '\0') return false;
    arcr_platform_mkdir_p(sh->profile_dir);
    char path[640];
    snprintf(path, sizeof(path), "%s/bookmarks.tsv", sh->profile_dir);
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    for (size_t i = 0u; i < sh->bookmark_count; ++i) {
        char t[96], u[384];
        sanitize_tsv_field(sh->bookmarks[i].title, t, sizeof(t));
        sanitize_tsv_field(sh->bookmarks[i].url, u, sizeof(u));
        fprintf(fp, "%s\t%s\n", t, u);
    }
    fclose(fp);
    return true;
}

static bool save_history_file(const ArcrBrowserShell *sh) {
    if (!sh || sh->cfg.private_browsing || sh->profile_dir[0] == '\0') return false;
    arcr_platform_mkdir_p(sh->profile_dir);
    char path[640];
    snprintf(path, sizeof(path), "%s/history.tsv", sh->profile_dir);
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    for (size_t i = 0u; i < sh->history_count; ++i) {
        char t[96], u[384];
        sanitize_tsv_field(sh->history[i].title, t, sizeof(t));
        sanitize_tsv_field(sh->history[i].url, u, sizeof(u));
        fprintf(fp, "%ld\t%s\t%s\n", (long)sh->history[i].timestamp, t, u);
    }
    fclose(fp);
    return true;
}

static void load_bookmarks_and_history(ArcrBrowserShell *sh) {
    if (!sh || sh->cfg.private_browsing || sh->profile_dir[0] == '\0') return;

    char path[640];
    char line[640];

    snprintf(path, sizeof(path), "%s/bookmarks.tsv", sh->profile_dir);
    FILE *fp = fopen(path, "rb");
    if (fp) {
        sh->bookmark_count = 0u;
        while (sh->bookmark_count < ARCR_SHELL_MAX_BOOKMARKS &&
               fgets(line, (int)sizeof(line), fp)) {
            strip_line_endings(line);
            char *tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = '\0';
            ArcrBookmarkItem *b = &sh->bookmarks[sh->bookmark_count++];
            arcr_str_copy(b->title, sizeof(b->title), line);
            arcr_str_copy(b->url, sizeof(b->url), tab + 1);
        }
        fclose(fp);
    }

    snprintf(path, sizeof(path), "%s/history.tsv", sh->profile_dir);
    fp = fopen(path, "rb");
    if (fp) {
        sh->history_count = 0u;
        while (sh->history_count < ARCR_SHELL_MAX_HISTORY &&
               fgets(line, (int)sizeof(line), fp)) {
            strip_line_endings(line);
            char *t1 = strchr(line, '\t');
            if (!t1) continue;
            *t1 = '\0';
            char *t2 = strchr(t1 + 1, '\t');
            if (!t2) continue;
            *t2 = '\0';
            ArcrHistoryItem *h = &sh->history[sh->history_count++];
            h->timestamp = (int64_t)strtol(line, NULL, 10);
            arcr_str_copy(h->title, sizeof(h->title), t1 + 1);
            arcr_str_copy(h->url, sizeof(h->url), t2 + 1);
        }
        fclose(fp);
    }
}

bool arcr_shell_load_profile_prefs(ArcrBrowserShell *sh) {
    if (!sh || sh->cfg.private_browsing || sh->profile_dir[0] == '\0') return false;
    char path[640];
    snprintf(path, sizeof(path), "%s/prefs.ini", sh->profile_dir);
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    char line[256];
    while (fgets(line, (int)sizeof(line), fp)) {
        strip_line_endings(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        int32_t val = 0;
        if (!arcr_parse_i32(eq + 1, strlen(eq + 1), &val)) continue;
        if (strcmp(line, "theme") == 0 && val >= 0 && val <= 2) {
            sh->cfg.theme = (ArcrShellTheme)val;
        } else if (strcmp(line, "bookmarks_bar") == 0) {
            sh->cfg.show_bookmarks_bar = (val != 0);
        } else if (strcmp(line, "zoom_percent") == 0) {
            sh->default_zoom_pct = ARCR_CLAMP(val, 50, 300);
        }
    }
    fclose(fp);
    return true;
}

bool arcr_shell_save_profile(const ArcrBrowserShell *sh) {
    if (!sh || sh->cfg.private_browsing) return false;
    bool a = save_prefs_file(sh);
    bool b = save_bookmarks_file(sh);
    bool c = save_history_file(sh);
    return a && b && c;
}

static void record_global_history(ArcrBrowserShell *sh, const char *title, const char *url) {
    if (!sh || !url || url[0] == '\0' || sh->cfg.private_browsing) return;
    if (arcr_str_starts_with_nocase(url, "waterscape://addon-toggle/") ||
        arcr_str_starts_with_nocase(url, "waterscape://set-theme/") ||
        arcr_str_starts_with_nocase(url, "waterscape://clear-data") ||
        arcr_str_starts_with_nocase(url, "waterscape://zoom/")) {
        return;
    }

    /* Deduplicate consecutive identical visits by hoisting to index 0 */
    if (sh->history_count > 0u && strcmp(sh->history[0].url, url) == 0) {
        sh->history[0].timestamp = arcr_platform_wall_epoch_sec();
        if (title && title[0] != '\0') {
            arcr_str_copy(sh->history[0].title, sizeof(sh->history[0].title), title);
        }
        save_history_file(sh);
        return;
    }

    size_t shift_cnt = sh->history_count < ARCR_SHELL_MAX_HISTORY
                           ? sh->history_count
                           : ARCR_SHELL_MAX_HISTORY - 1u;
    if (shift_cnt > 0u) {
        memmove(&sh->history[1], &sh->history[0], shift_cnt * sizeof(ArcrHistoryItem));
    }
    sh->history_count = shift_cnt + 1u;
    sh->history[0].timestamp = arcr_platform_wall_epoch_sec();
    arcr_str_copy(sh->history[0].title, sizeof(sh->history[0].title),
                  (title && title[0] != '\0') ? title : url);
    arcr_str_copy(sh->history[0].url, sizeof(sh->history[0].url), url);
    save_history_file(sh);
}

void arcr_shell_clear_browsing_data(ArcrBrowserShell *sh) {
    if (!sh) return;
    sh->history_count = 0u;
    if (sh->addons) {
        ws_addons_reset_blocked_counts(sh->addons);
    }
    if (!sh->cfg.private_browsing) {
        save_history_file(sh);
    }
    arcr_shell_set_status_overlay(sh, "방문 기록 및 차단 통계가 초기화되었습니다.");
}

/* -------------------------------------------------------------------------
 * Built-in Interactive `waterscape://` (and `arcr://` alias) Pages
 * ------------------------------------------------------------------------- */
static void append_internal_page_head(const ArcrBrowserShell *sh, StrBuf *sb, const char *title) {
    bool dark    = (sh->cfg.theme == ARCR_SHELL_THEME_MODERN_DARK);
    bool classic = (sh->cfg.theme == ARCR_SHELL_THEME_WIN2000_CLASSIC);

    const char *bg      = dark ? "#0b1120" : (classic ? "#d4d0c8" : "#f8fafc");
    const char *card_bg = dark ? "#1e293b" : (classic ? "#ffffff" : "#ffffff");
    const char *fg      = dark ? "#f1f5f9" : (classic ? "#000000" : "#0f172a");
    const char *muted   = dark ? "#94a3b8" : (classic ? "#3a3a3a" : "#475569");
    const char *border  = dark ? "#334155" : (classic ? "#808080" : "#cbd5e1");
    const char *accent  = dark ? "#38bdf8" : (classic ? "#0a246a" : "#0284c7");
    const char *pill_bg = dark ? "#0f172a" : (classic ? "#e4e0d8" : "#e0f2fe");

    sb_append(sb, "<!DOCTYPE html><html lang=\"ko\"><head><meta charset=\"utf-8\"><title>");
    sb_append_html_escaped(sb, title);
    sb_append(sb, "</title><style>");
    sb_append(sb, "body{background:"); sb_append(sb, bg);
    sb_append(sb, ";color:"); sb_append(sb, fg);
    sb_append(sb, ";padding:20px 26px;font-size:14px;word-break:keep-all;}");
    sb_append(sb, "h1{font-size:22px;margin-top:0px;margin-bottom:4px;color:"); sb_append(sb, fg); sb_append(sb, ";}");
    sb_append(sb, "h2{font-size:16px;margin-top:0px;margin-bottom:8px;color:"); sb_append(sb, accent); sb_append(sb, ";}");
    sb_append(sb, ".sub{color:"); sb_append(sb, muted); sb_append(sb, ";font-size:13px;margin-bottom:16px;}");
    sb_append(sb, ".nav{margin-bottom:16px;padding:8px 12px;background:"); sb_append(sb, card_bg);
    sb_append(sb, ";border:1px solid "); sb_append(sb, border); sb_append(sb, ";}");
    sb_append(sb, ".nav a{margin-right:14px;font-weight:bold;color:"); sb_append(sb, accent); sb_append(sb, ";text-decoration:none;}");
    sb_append(sb, ".card{background:"); sb_append(sb, card_bg);
    sb_append(sb, ";border:1px solid "); sb_append(sb, border);
    sb_append(sb, ";border-left:4px solid "); sb_append(sb, accent);
    sb_append(sb, ";padding:12px 16px;margin-bottom:12px;}");
    sb_append(sb, ".badge{background:"); sb_append(sb, pill_bg);
    sb_append(sb, ";color:"); sb_append(sb, accent); sb_append(sb, ";padding:2px 8px;font-size:12px;font-weight:bold;}");
    sb_append(sb, ".on{background:#14532d;color:#4ade80;padding:2px 8px;font-size:12px;font-weight:bold;}");
    sb_append(sb, ".off{background:#334155;color:#cbd5e1;padding:2px 8px;font-size:12px;}");
    sb_append(sb, "a{color:"); sb_append(sb, accent); sb_append(sb, ";}");
    sb_append(sb, "code{background:"); sb_append(sb, pill_bg);
    sb_append(sb, ";color:"); sb_append(sb, accent); sb_append(sb, ";padding:1px 6px;}");
    sb_append(sb, "input{padding:6px 10px;width:340px;border:1px solid "); sb_append(sb, border);
    sb_append(sb, ";background:"); sb_append(sb, card_bg); sb_append(sb, ";color:"); sb_append(sb, fg); sb_append(sb, ";}");
    sb_append(sb, "button{padding:6px 14px;background:"); sb_append(sb, accent);
    sb_append(sb, ";color:#ffffff;border:none;font-weight:bold;}");
    sb_append(sb, "table{width:100%;border-collapse:collapse;margin-top:6px;}");
    sb_append(sb, "th,td{border:1px solid "); sb_append(sb, border); sb_append(sb, ";padding:6px 10px;text-align:left;font-size:13px;}");
    sb_append(sb, "</style></head><body>");

    sb_append(sb, "<div class=\"nav\">"
                  "<a href=\"waterscape://newtab\">새 탭 홈</a> &nbsp;&middot;&nbsp; "
                  "<a href=\"waterscape://hardware\">하드웨어 가속</a> &nbsp;&middot;&nbsp; "
                  "<a href=\"waterscape://addons\">확장 기능·플러그인</a> &nbsp;&middot;&nbsp; "
                  "<a href=\"waterscape://bookmarks\">북마크</a> &nbsp;&middot;&nbsp; "
                  "<a href=\"waterscape://history\">방문 기록</a> &nbsp;&middot;&nbsp; "
                  "<a href=\"waterscape://downloads\">다운로드</a> &nbsp;&middot;&nbsp; "
                  "<a href=\"waterscape://settings\">환경설정</a>"
                  "</div>");
}

static char *generate_internal_page_html(ArcrBrowserShell *sh, const char *page_key, size_t *out_len) {
    StrBuf sb;
    if (!sb_init(&sb, 4096u)) return NULL;

    ArcrEngineStats st;
    memset(&st, 0, sizeof(st));
    arcr_engine_get_stats(sh->engine, &st);

    uint32_t total_blocked = sh->addons ? ws_addons_total_blocked(sh->addons) : 0u;
    size_t   addon_cnt     = sh->addons ? ws_addons_count(sh->addons) : 0u;

    if (arcr_str_eq_nocase(page_key, "addons")) {
        append_internal_page_head(sh, &sb, "Waterscape 확장 기능 및 플러그인");
        sb_append(&sb, "<h1>확장 기능 및 네이티브 플러그인 관리자</h1>");
        sb_append(&sb, "<p class=\"sub\">패키지형 <code>.wsx</code> WebExtensions 및 순수 C11 네이티브 플러그인 실시간 제어 · 총 차단 요청: <code>");
        sb_append_u64(&sb, total_blocked);
        sb_append(&sb, "회</code></p>");

        if (sh->addons) {
            WaterscapePluginHostContext hctx = {
                .detected_cpu_flags     = st.detected_cpu_flags,
                .active_simd_backend    = st.active_simd_level,
                .active_gpu_backend     = st.active_gpu_backend,
                .worker_threads         = st.worker_thread_count,
                .last_frame_render_usec = st.last_layout_usec + st.last_paint_usec,
                .last_frame_tiles       = st.tiles_rendered_last_frame,
                .engine_memory_bytes    = st.arena_used_bytes
            };
            char hud[256];
            hud[0] = '\0';
            ws_addons_format_plugin_hud(sh->addons, &hctx, hud, sizeof(hud));
            if (hud[0] != '\0') {
                sb_append(&sb, "<div class=\"card\"><h2>실시간 플러그인 텔레메트리 HUD</h2><p><code>");
                sb_append_html_escaped(&sb, hud);
                sb_append(&sb, "</code></p></div>");
            }
        }

        for (size_t i = 0u; i < addon_cnt; ++i) {
            WsAddonInfo info;
            if (!ws_addons_get_info(sh->addons, i, &info)) continue;
            sb_append(&sb, "<div class=\"card\"><h2>");
            sb_append_html_escaped(&sb, info.name);
            sb_append(&sb, " <span class=\"");
            sb_append(&sb, info.enabled ? "on\">활성" : "off\">비활성");
            sb_append(&sb, "</span></h2><p>");
            sb_append_html_escaped(&sb, info.description);
            sb_append(&sb, "</p><p>ID: <code>");
            sb_append_html_escaped(&sb, info.id);
            sb_append(&sb, "</code> · 버전: <code>");
            sb_append_html_escaped(&sb, info.version);
            sb_append(&sb, "</code> · 유형: <code>");
            sb_append(&sb, info.kind == WS_ADDON_NATIVE_PLUGIN ? "Native C11 Plugin" : "WebExtension (.wsx)");
            sb_append(&sb, "</code> · 필터 규칙: <code>");
            sb_append_u64(&sb, info.filter_rule_count);
            sb_append(&sb, "</code> · 차단 적중: <code>");
            sb_append_u64(&sb, info.blocked_hits);
            sb_append(&sb, "</code> · <a href=\"waterscape://addon-toggle/");
            sb_append_html_escaped(&sb, info.id);
            sb_append(&sb, "\">");
            sb_append(&sb, info.enabled ? "[비활성화로 전환]" : "[활성화로 전환]");
            sb_append(&sb, "</a></p></div>");
        }
    } else if (arcr_str_eq_nocase(page_key, "hardware") || arcr_str_eq_nocase(page_key, "gpu")) {
        ArcrGpuProbeResult probe;
        memset(&probe, 0, sizeof(probe));
        arcr_gpu_probe_system(&probe);

        append_internal_page_head(sh, &sb, "Waterscape 하드웨어 가속 및 모듈 진단");
        sb_append(&sb, "<h1>하드웨어 가속 파이프라인 및 계층형 모듈 진단</h1>");
        sb_append(&sb, "<p class=\"sub\">Windows 2000 SP4 (SSE1+SWAR) ~ 현대 64비트 (AVX2 + 멀티코어 64x64 타일 + D3D11/Vulkan/XShm)</p>");

        sb_append(&sb, "<div class=\"card\"><h2>1. CPU SIMD 커널 및 멀티코어 타일 래스터라이저</h2>"
                       "<p>호스트 OS: <code>");
        sb_append_html_escaped(&sb, arcr_platform_os_name());
        sb_append(&sb, "</code> · 감지된 SIMD 계층: <code>");
        sb_append_html_escaped(&sb, arcr_cpu_simd_tier_name(st.detected_cpu_flags));
        sb_append(&sb, "</code> · 활성 SIMD 백엔드: <code>");
        sb_append_html_escaped(&sb, arcr_accel_backend_name((ArcrAccelBackendId)st.active_simd_level));
        sb_append(&sb, "</code></p><p>작업자 스레드 풀: <code>");
        sb_append_u64(&sb, st.worker_thread_count);
        sb_append(&sb, " 스레드</code> · 최근 프레임 64x64 타일: <code>");
        sb_append_u64(&sb, st.tiles_rendered_last_frame);
        sb_append(&sb, "개</code> · 레이아웃 지연: <code>");
        sb_append_u64(&sb, st.last_layout_usec);
        sb_append(&sb, " us</code> · 페인트 지연: <code>");
        sb_append_u64(&sb, st.last_paint_usec);
        sb_append(&sb, " us</code></p></div>");

        sb_append(&sb, "<div class=\"card\"><h2>2. GPU 하드웨어 프로브 및 무중단 폴백 상태</h2>"
                       "<p>어댑터 요약: <code>");
        sb_append_html_escaped(&sb, probe.adapter_description[0] ? probe.adapter_description
                                                                 : arcr_engine_gpu_backend_summary(sh->engine));
        sb_append(&sb, "</code> · 엔진 GPU 백엔드: <code>");
        sb_append_html_escaped(&sb, arcr_engine_gpu_backend_summary(sh->engine));
        sb_append(&sb, "</code></p><p>"
                       "Direct3D 11 Flip: <code>");
        sb_append(&sb, probe.has_d3d11 ? "지원" : "미감지");
        sb_append(&sb, "</code> · Vulkan 1.x: <code>");
        sb_append(&sb, probe.has_vulkan ? "지원" : "미감지");
        sb_append(&sb, "</code> · Direct3D 9: <code>");
        sb_append(&sb, probe.has_d3d9 ? "지원" : "미감지");
        sb_append(&sb, "</code> · DirectDraw 7: <code>");
        sb_append(&sb, probe.has_ddraw7 ? "지원" : "미감지");
        sb_append(&sb, "</code> · OpenGL ICD: <code>");
        sb_append(&sb, probe.has_opengl_icd ? "검증됨" : "미감지/소프트웨어 제외");
        sb_append(&sb, "</code> · 폴백 발생: <code>");
        sb_append(&sb, st.gpu_fallback_triggered ? "CPU 복귀 완료" : "정상 (오류 없음)");
        sb_append(&sb, "</code></p></div>");

        WaterscapeModuleStatusEntry mods[8];
        size_t mod_n = arcr_engine_query_modules(sh->engine, mods, ARCR_ARRAY_LEN(mods));
        sb_append(&sb, "<div class=\"card\"><h2>3. Tier 0~7 모듈 레지스트리 및 지연 로드(On-Demand) 상태</h2>"
                       "<table><tr><th>모듈 스템</th><th>계층</th><th>역할</th><th>적재 상태</th><th>ABI 검증</th></tr>");
        for (size_t i = 0u; i < mod_n; ++i) {
            sb_append(&sb, "<tr><td><code>");
            sb_append_html_escaped(&sb, mods[i].stem ? mods[i].stem : "-");
            sb_append(&sb, "</code></td><td>Tier ");
            sb_append_u64(&sb, (uint64_t)mods[i].tier);
            sb_append(&sb, "</td><td>");
            sb_append_html_escaped(&sb, mods[i].role_summary ? mods[i].role_summary : "");
            sb_append(&sb, "</td><td>");
            sb_append(&sb, mods[i].is_loaded ? "<span class=\"on\">적재됨</span>"
                                             : "<span class=\"off\">대기 (0 Byte)</span>");
            sb_append(&sb, "</td><td><code>");
            sb_append_html_escaped(&sb, arcr_module_status_str(mods[i].last_status));
            sb_append(&sb, "</code></td></tr>");
        }
        sb_append(&sb, "</table></div>");

        sb_append(&sb, "<div class=\"card\"><h2>4. 메모리 아레나 및 캐시 통계</h2><p>"
                       "아레나 사용/할당: <code>");
        sb_append_u64(&sb, st.arena_used_bytes);
        sb_append(&sb, " / ");
        sb_append_u64(&sb, st.arena_allocated_bytes);
        sb_append(&sb, " B</code> · 인터닝 원자(Atom): <code>");
        sb_append_u64(&sb, st.interned_atom_count);
        sb_append(&sb, "</code> · O(1) 스타일 공유 적중: <code>");
        sb_append_u64(&sb, st.style_shared_hits);
        sb_append(&sb, "</code> · 글리프 캐시 적중: <code>");
        sb_append_u64(&sb, st.glyph_cache_hits);
        sb_append(&sb, " (항목 ");
        sb_append_u64(&sb, st.glyph_cache_entries);
        sb_append(&sb, "개)</code> · JS 힙: <code>");
        sb_append_u64(&sb, st.js_heap_bytes);
        sb_append(&sb, " B</code></p></div>");
    } else if (arcr_str_eq_nocase(page_key, "settings") || arcr_str_eq_nocase(page_key, "config")) {
        ArcrWebView *av = arcr_shell_active_webview(sh);
        int32_t cur_zoom = av ? arcr_webview_get_zoom(av) : sh->default_zoom_pct;
        append_internal_page_head(sh, &sb, "Waterscape 환경설정");
        sb_append(&sb, "<h1>브라우저 환경설정 및 사용자 지정</h1>");
        sb_append(&sb, "<p class=\"sub\">테마, 도구 모음, 확대 배율, 영구 프로필 및 개인정보 보호 설정을 즉시 변경합니다.</p>");

        sb_append(&sb, "<div class=\"card\"><h2>1. 외형 테마 선택 (단축키 F2)</h2><p>"
                       "<a href=\"waterscape://set-theme/light\">[Modern Light 밝은 테마]</a> · "
                       "<a href=\"waterscape://set-theme/dark\">[Modern Dark 어두운 테마]</a> · "
                       "<a href=\"waterscape://set-theme/classic\">[Windows 2000 Classic 고전 테마]</a>"
                       "</p></div>");

        sb_append(&sb, "<div class=\"card\"><h2>2. 도구 모음 및 화면 배율</h2><p>북마크 바 상태: <code>");
        sb_append(&sb, sh->cfg.show_bookmarks_bar ? "표시됨 (24px)" : "숨김 (0px)");
        sb_append(&sb, "</code> · <a href=\"waterscape://toggle-bookmarks-bar\">[북마크 바 표시/숨김 전환 (Ctrl+B)]</a>"
                       " · <a href=\"waterscape://toggle-devtools\">[하단 하드웨어·개발자 독 전환 (F12)]</a></p>"
                       "<p>현재 확대 배율: <code>");
        sb_append_u64(&sb, (uint64_t)(cur_zoom > 0 ? cur_zoom : 100));
        sb_append(&sb, "%</code> · <a href=\"waterscape://zoom/out\">[축소 -10%]</a> · "
                       "<a href=\"waterscape://zoom/reset\">[100% 기본값]</a> · "
                       "<a href=\"waterscape://zoom/in\">[확대 +10%]</a></p></div>");

        sb_append(&sb, "<div class=\"card\"><h2>3. 프로필 저장소 및 사생활 보호</h2><p>프로필 경로: <code>");
        if (sh->cfg.private_browsing) {
            sb_append(&sb, "[사생활 보호 모드 — 디스크 기록 비활성화]");
        } else {
            sb_append_html_escaped(&sb, sh->profile_dir[0] ? sh->profile_dir : "(기본 메모리)");
        }
        sb_append(&sb, "</code></p><p>저장된 북마크: <code>");
        sb_append_u64(&sb, sh->bookmark_count);
        sb_append(&sb, "개</code> · 저장된 방문 기록: <code>");
        sb_append_u64(&sb, sh->history_count);
        sb_append(&sb, "건</code> · <a href=\"waterscape://clear-data\">[방문 기록 및 차단 통계 즉시 삭제]</a></p></div>");
    } else if (arcr_str_eq_nocase(page_key, "bookmarks")) {
        append_internal_page_head(sh, &sb, "Waterscape 북마크 관리자");
        sb_append(&sb, "<h1>북마크 관리자</h1>");
        sb_append(&sb, "<p class=\"sub\">주소창 우측 ★ 버튼 또는 <code>Ctrl+D</code>로 현재 페이지를 북마크에 추가/해제할 수 있습니다.</p>");
        sb_append(&sb, "<div class=\"card\"><table><tr><th>제목</th><th>대상 URL</th></tr>");
        if (sh->bookmark_count == 0u) {
            sb_append(&sb, "<tr><td colspan=\"2\">저장된 북마크가 없습니다.</td></tr>");
        } else {
            for (size_t i = 0u; i < sh->bookmark_count; ++i) {
                sb_append(&sb, "<tr><td>");
                sb_append_html_escaped(&sb, sh->bookmarks[i].title);
                sb_append(&sb, "</td><td><a href=\"");
                sb_append_html_escaped(&sb, sh->bookmarks[i].url);
                sb_append(&sb, "\">");
                sb_append_html_escaped(&sb, sh->bookmarks[i].url);
                sb_append(&sb, "</a></td></tr>");
            }
        }
        sb_append(&sb, "</table></div>");
    } else if (arcr_str_eq_nocase(page_key, "history")) {
        append_internal_page_head(sh, &sb, "Waterscape 방문 기록");
        sb_append(&sb, "<h1>방문 기록</h1>");
        sb_append(&sb, "<p class=\"sub\">영구 프로필(<code>history.tsv</code>)에 기록된 최근 탐색 목록 · <a href=\"waterscape://clear-data\">[전체 기록 삭제]</a></p>");
        sb_append(&sb, "<div class=\"card\"><table><tr><th>시각 (Epoch)</th><th>문서 제목</th><th>주소</th></tr>");
        if (sh->history_count == 0u) {
            sb_append(&sb, "<tr><td colspan=\"3\">방문 기록이 비어 있습니다.</td></tr>");
        } else {
            for (size_t i = 0u; i < sh->history_count; ++i) {
                sb_append(&sb, "<tr><td><code>");
                sb_append_u64(&sb, (uint64_t)sh->history[i].timestamp);
                sb_append(&sb, "</code></td><td>");
                sb_append_html_escaped(&sb, sh->history[i].title);
                sb_append(&sb, "</td><td><a href=\"");
                sb_append_html_escaped(&sb, sh->history[i].url);
                sb_append(&sb, "\">");
                sb_append_html_escaped(&sb, sh->history[i].url);
                sb_append(&sb, "</a></td></tr>");
            }
        }
        sb_append(&sb, "</table></div>");
    } else if (arcr_str_eq_nocase(page_key, "downloads")) {
        char dl_dir[512];
        dl_dir[0] = '\0';
        arcr_platform_get_downloads_dir(dl_dir, sizeof(dl_dir));
        append_internal_page_head(sh, &sb, "Waterscape 다운로드 목록");
        sb_append(&sb, "<h1>다운로드 관리자 및 실행 파일 보호</h1>");
        sb_append(&sb, "<p class=\"sub\">기본 다운로드 폴더: <code>");
        sb_append_html_escaped(&sb, dl_dir[0] ? dl_dir : "~/Downloads");
        sb_append(&sb, "</code> · 실행 확장자(<code>.exe/.bat/.scr/.msi</code>) 자동 격리 검사 활성</p>");
        sb_append(&sb, "<div class=\"card\"><table><tr><th>상태</th><th>보안 정책</th><th>설명</th></tr>"
                       "<tr><td><span class=\"on\">보호 활성</span></td><td>TLS 1.3 + 루트 CA 검증</td>"
                       "<td>다운로드 스트림 무결성 및 시계 오차(CMOS) 사전 점검</td></tr>"
                       "<tr><td><span class=\"badge\">대기 중</span></td><td>진행 중인 다운로드 없음</td>"
                       "<td>웹 페이지의 파일 링크 클릭 시 이곳에 기록됩니다.</td></tr></table></div>");
    } else {
        /* Default: `waterscape://newtab` Speed Dial */
        append_internal_page_head(sh, &sb, "Waterscape 새 탭");
        sb_append(&sb, "<h1>Waterscape Browser 1.0</h1>");
        sb_append(&sb, "<p class=\"sub\">Arcradient Pure C11 Engine · 초경량 네이티브 속도 · 멀티코어 SIMD + 하드웨어 GPU 가속</p>");
        if (sh->cfg.private_browsing) {
            sb_append(&sb, "<div class=\"card\"><h2>사생활 보호 모드 활성</h2>"
                           "<p>이 창의 탐색 기록, 쿠키 및 검색어는 종료 즉시 메모리에서 완전 소거되며 디스크에 저장되지 않습니다.</p></div>");
        }
        sb_append(&sb, "<div class=\"card\"><h2>통합 웹 검색 및 빠른 이동</h2>"
                       "<form action=\"waterscape://search\" method=\"GET\">"
                       "<input type=\"text\" name=\"q\" placeholder=\"DuckDuckGo 검색어 또는 도메인/URL 입력...\" /> "
                       "<button type=\"submit\">이동</button></form></div>");

        sb_append(&sb, "<div class=\"card\"><h2>빠른 실행 (Speed Dial)</h2><p>"
                       "<a href=\"waterscape://hardware\">[하드웨어 가속 진단]</a> · "
                       "<a href=\"waterscape://addons\">[확장 기능 및 차단기]</a> · "
                       "<a href=\"waterscape://settings\">[테마·환경설정]</a> · "
                       "<a href=\"waterscape://bookmarks\">[북마크]</a> · "
                       "<a href=\"waterscape://history\">[방문 기록]</a> · "
                       "<a href=\"waterscape://downloads\">[다운로드]</a></p></div>");

        sb_append(&sb, "<div class=\"card\"><h2>실시간 엔진 가속 및 Waterscape Shield 요약</h2><p>"
                       "활성 SIMD 커널: <code>");
        sb_append_html_escaped(&sb, arcr_cpu_simd_tier_name(st.detected_cpu_flags));
        sb_append(&sb, "</code> · 스레드 풀: <code>");
        sb_append_u64(&sb, st.worker_thread_count);
        sb_append(&sb, " 코어</code> · GPU 상태: <code>");
        sb_append_html_escaped(&sb, arcr_engine_gpu_backend_summary(sh->engine));
        sb_append(&sb, "</code></p><p>차단된 광고·추적기: <code>");
        sb_append_u64(&sb, total_blocked);
        sb_append(&sb, "건</code> · 적재된 확장/플러그인: <code>");
        sb_append_u64(&sb, addon_cnt);
        sb_append(&sb, "개</code> · 아레나 메모리: <code>");
        sb_append_u64(&sb, st.arena_used_bytes / 1024u);
        sb_append(&sb, " KiB</code></p></div>");
    }

    sb_append(&sb, "</body></html>");
    if (out_len) *out_len = sb.len;
    return sb.data;
}

/* -------------------------------------------------------------------------
 * WebView Callbacks & Extension CSS/JS Injection
 * ------------------------------------------------------------------------- */
static void on_webview_hover_link_cb(void *userdata, const char *utf8_href) {
    WsTabSlot *slot = (WsTabSlot *)userdata;
    if (!slot || !slot->owner) return;
    ArcrBrowserShell *sh = slot->owner;
    if (&sh->tabs[sh->active_tab] != slot) return;

    if (utf8_href && utf8_href[0] != '\0') {
        snprintf(sh->status_overlay, sizeof(sh->status_overlay), "링크 이동: %s", utf8_href);
        sh->status_from_hover = true;
    } else if (sh->status_from_hover) {
        sh->status_overlay[0] = '\0';
        sh->status_from_hover = false;
    }
}

static void sync_omnibar_from_active_tab(ArcrBrowserShell *sh) {
    if (!sh) return;
    const char *u = "waterscape://newtab";
    if (sh->tab_count > 0u) {
        WsTabSlot *slot = &sh->tabs[sh->active_tab];
        if (slot->current_url[0] != '\0') {
            u = slot->current_url;
        } else if (slot->view) {
            const char *vu = arcr_webview_get_url(slot->view);
            if (vu && vu[0] != '\0') u = vu;
        }
    }
    sh->omni_len        = arcr_str_copy(sh->omni_buf, sizeof(sh->omni_buf), u);
    sh->omni_cursor     = sh->omni_len;
    sh->omni_select_all = false;
}

static void apply_addons_to_tab(ArcrBrowserShell *sh, WsTabSlot *slot, bool clear_empty_css) {
    if (!sh || !sh->addons || !slot || !slot->view) return;
    const char *url = slot->current_url[0] ? slot->current_url : arcr_webview_get_url(slot->view);
    if (is_internal_url(url)) return;

    char  *css     = NULL;
    size_t css_len = 0u;
    if (ws_addons_collect_injected_css(sh->addons, url, &css, &css_len) && css && css_len > 0u) {
        arcr_webview_inject_user_css(slot->view, css, css_len);
    } else if (clear_empty_css) {
        arcr_webview_inject_user_css(slot->view, "", 0u);
    }
    free(css);

    char  *js     = NULL;
    size_t js_len = 0u;
    if (ws_addons_collect_injected_js(sh->addons, url, &js, &js_len) && js && js_len > 0u) {
        arcr_webview_eval_js(slot->view, js, js_len);
    }
    free(js);
}

static void push_tab_history(WsTabSlot *slot, const char *url) {
    if (!slot || !url || url[0] == '\0') return;
    if (slot->hist_count > 0u &&
        strcmp(slot->hist_urls[slot->hist_pos], url) == 0) {
        return;
    }
    if (slot->hist_count > 0u && slot->hist_pos + 1u < slot->hist_count) {
        slot->hist_count = slot->hist_pos + 1u;
    }
    if (slot->hist_count >= ARCR_SHELL_TAB_HIST_CAP) {
        memmove(&slot->hist_urls[0], &slot->hist_urls[1],
                (ARCR_SHELL_TAB_HIST_CAP - 1u) * sizeof(slot->hist_urls[0]));
        slot->hist_count = ARCR_SHELL_TAB_HIST_CAP - 1u;
    }
    arcr_str_copy(slot->hist_urls[slot->hist_count], sizeof(slot->hist_urls[0]), url);
    slot->hist_pos   = slot->hist_count;
    slot->hist_count += 1u;
}

static bool load_url_into_slot(ArcrBrowserShell *sh, WsTabSlot *slot,
                               const char *resolved_url, bool record_hist) {
    if (!sh || !slot || !slot->view || !resolved_url) return false;

    /* Extract internal page key if `waterscape://` / `arcr://` / `about:` */
    const char *internal_rest = NULL;
    if (arcr_str_starts_with_nocase(resolved_url, "waterscape://")) {
        internal_rest = resolved_url + 13;
    } else if (arcr_str_starts_with_nocase(resolved_url, "arcr://")) {
        internal_rest = resolved_url + 7;
    } else if (arcr_str_starts_with_nocase(resolved_url, "about:")) {
        const char *ab = resolved_url + 6;
        internal_rest = (ab[0] == '\0' || arcr_str_eq_nocase(ab, "blank") ||
                         arcr_str_eq_nocase(ab, "home")) ? "newtab" : ab;
    }

    if (internal_rest) {
        /* Handle interactive internal action URLs! */
        if (arcr_str_starts_with_nocase(internal_rest, "search?") ||
            arcr_str_starts_with_nocase(internal_rest, "search/?")) {
            const char *qpos = strstr(internal_rest, "q=");
            char decoded[384];
            decoded[0] = '\0';
            if (qpos) {
                char raw_q[384];
                size_t k = 0u;
                qpos += 2;
                while (qpos[k] != '\0' && qpos[k] != '&' && k + 1u < sizeof(raw_q)) {
                    raw_q[k] = qpos[k];
                    ++k;
                }
                raw_q[k] = '\0';
                url_decode_query(raw_q, decoded, sizeof(decoded));
            }
            char target[512];
            resolve_omnibar_target(decoded, target, sizeof(target));
            return load_url_into_slot(sh, slot, target, true);
        }
        if (arcr_str_starts_with_nocase(internal_rest, "addon-toggle/")) {
            const char *aid = internal_rest + 13;
            if (sh->addons && aid[0] != '\0') {
                bool now_on = ws_addons_toggle(sh->addons, aid);
                char msg[192];
                snprintf(msg, sizeof(msg), "확장 기능 '%s' -> %s", aid, now_on ? "활성" : "비활성");
                arcr_shell_set_status_overlay(sh, msg);
            }
            const char *back_page = is_internal_url(slot->current_url) ? slot->current_url
                                                                       : "waterscape://addons";
            return load_url_into_slot(sh, slot, back_page, false);
        }
        if (arcr_str_starts_with_nocase(internal_rest, "set-theme/")) {
            const char *tname = internal_rest + 10;
            if (arcr_str_eq_nocase(tname, "dark")) {
                arcr_shell_set_theme(sh, ARCR_SHELL_THEME_MODERN_DARK, false);
            } else if (arcr_str_eq_nocase(tname, "classic")) {
                arcr_shell_set_theme(sh, ARCR_SHELL_THEME_WIN2000_CLASSIC, sh->cfg.draw_os_window_frame);
            } else {
                arcr_shell_set_theme(sh, ARCR_SHELL_THEME_MODERN_LIGHT, false);
            }
            return load_url_into_slot(sh, slot, "waterscape://settings", false);
        }
        if (arcr_str_eq_nocase(internal_rest, "toggle-bookmarks-bar")) {
            arcr_shell_set_bookmarks_bar_visible(sh, !sh->cfg.show_bookmarks_bar);
            return load_url_into_slot(sh, slot, "waterscape://settings", false);
        }
        if (arcr_str_eq_nocase(internal_rest, "toggle-devtools")) {
            arcr_shell_set_devtools_visible(sh, !sh->cfg.show_devtools_dock);
            return load_url_into_slot(sh, slot,
                                      slot->current_url[0] ? slot->current_url : "waterscape://settings",
                                      false);
        }
        if (arcr_str_starts_with_nocase(internal_rest, "zoom/")) {
            const char *zcmd = internal_rest + 5;
            int32_t cur = arcr_webview_get_zoom(slot->view);
            if (cur <= 0) cur = sh->default_zoom_pct;
            if (arcr_str_eq_nocase(zcmd, "in")) cur = ARCR_CLAMP(cur + 10, 50, 300);
            else if (arcr_str_eq_nocase(zcmd, "out")) cur = ARCR_CLAMP(cur - 10, 50, 300);
            else cur = 100;
            sh->default_zoom_pct = cur;
            arcr_webview_set_zoom(slot->view, cur);
            save_prefs_file(sh);
            return load_url_into_slot(sh, slot, "waterscape://settings", false);
        }
        if (arcr_str_eq_nocase(internal_rest, "clear-data")) {
            arcr_shell_clear_browsing_data(sh);
            return load_url_into_slot(sh, slot,
                                      is_internal_url(slot->current_url) ? slot->current_url
                                                                         : "waterscape://settings",
                                      false);
        }

        char clean_key[64];
        size_t k = 0u;
        while (internal_rest[k] != '\0' && internal_rest[k] != '/' &&
               internal_rest[k] != '?' && internal_rest[k] != '#' && k + 1u < sizeof(clean_key)) {
            clean_key[k] = internal_rest[k];
            ++k;
        }
        clean_key[k] = '\0';
        if (clean_key[0] == '\0' || arcr_str_eq_nocase(clean_key, "home")) {
            arcr_str_copy(clean_key, sizeof(clean_key), "newtab");
        }

        char canonical_url[128];
        snprintf(canonical_url, sizeof(canonical_url), "waterscape://%s", clean_key);

        size_t html_len = 0u;
        char  *html     = generate_internal_page_html(sh, clean_key, &html_len);
        if (!html) return false;

        arcr_webview_set_prefers_dark(slot->view, sh->cfg.theme == ARCR_SHELL_THEME_MODERN_DARK);
        arcr_webview_inject_user_css(slot->view, "", 0u);
        bool ok = arcr_webview_load_html(slot->view, canonical_url, html, html_len);
        free(html);
        arcr_webview_set_scroll(slot->view, 0);

        arcr_str_copy(slot->current_url, sizeof(slot->current_url), canonical_url);
        slot->has_inline_html = false;
        if (record_hist) {
            push_tab_history(slot, canonical_url);
            record_global_history(sh, arcr_webview_get_title(slot->view), canonical_url);
        }
        if (&sh->tabs[sh->active_tab] == slot) {
            sync_omnibar_from_active_tab(sh);
        }
        return ok;
    }

    /* External Document Navigation */
    arcr_webview_set_prefers_dark(slot->view, sh->cfg.theme == ARCR_SHELL_THEME_MODERN_DARK);
    bool ok = arcr_webview_navigate_url(slot->view, resolved_url);
    arcr_webview_set_scroll(slot->view, 0);
    arcr_str_copy(slot->current_url, sizeof(slot->current_url), resolved_url);
    slot->has_inline_html = false;
    apply_addons_to_tab(sh, slot, false);

    if (record_hist) {
        push_tab_history(slot, resolved_url);
        record_global_history(sh, arcr_webview_get_title(slot->view), resolved_url);
    }
    if (&sh->tabs[sh->active_tab] == slot) {
        sync_omnibar_from_active_tab(sh);
    }
    return ok;
}

/* -------------------------------------------------------------------------
 * Layout & Geometry Computation
 * ------------------------------------------------------------------------- */
void arcr_shell_default_config(ArcrShellConfig *out_cfg, ArcrShellTheme theme) {
    if (!out_cfg) return;
    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->theme                = theme;
    out_cfg->window_width         = (theme == ARCR_SHELL_THEME_WIN2000_CLASSIC) ? 800 : 1080;
    out_cfg->window_height        = (theme == ARCR_SHELL_THEME_WIN2000_CLASSIC) ? 600 : 720;
    out_cfg->show_bookmarks_bar   = false; /* Hidden by default per Section 5.2 & test_suite.c! */
    out_cfg->show_home_button     = true;
    out_cfg->draw_os_window_frame = (theme == ARCR_SHELL_THEME_WIN2000_CLASSIC);
    out_cfg->private_browsing     = false;
    out_cfg->load_saved_prefs     = false;
    out_cfg->show_devtools_dock   = false;
    out_cfg->module_search_dir    = ".";
}

void arcr_shell_compute_layout(const ArcrBrowserShell *shell,
                               ArcrShellLayoutMetrics *m) {
    if (!shell || !m) return;
    memset(m, 0, sizeof(*m));

    int32_t w   = shell->cfg.window_width;
    int32_t h   = shell->cfg.window_height;
    int32_t pad = shell->cfg.draw_os_window_frame ? 3 : 0;
    int32_t y   = pad;
    int32_t iw  = ARCR_MAX(64, w - pad * 2);

    if (shell->cfg.draw_os_window_frame) {
        m->window_caption_rect = (ArcrRect){pad, y, iw, 20};
        y += 21;
    }

    /* 1. Tab Strip (32px) */
    m->tab_strip_rect = (ArcrRect){pad, y, iw, 32};
    y += 32;

    /* 2. Navigation Toolbar (36px) */
    m->nav_toolbar_rect = (ArcrRect){pad, y, iw, 36};
    y += 36;

    /* 3. Optional Bookmarks Bar (24px when enabled — preserves exact 24px delta in test_suite.c) */
    if (shell->cfg.show_bookmarks_bar) {
        m->bookmarks_bar_rect = (ArcrRect){pad, y, iw, 24};
        y += 24;
    }

    /* 4. Optional Find-in-Page Bar (28px on Ctrl+F) */
    if (shell->show_find_bar) {
        m->find_bar_rect = (ArcrRect){pad, y, iw, 28};
        y += 28;
    }

    /* 5. Optional Bottom Hardware & DevTools Dock (150px on F12) */
    int32_t dock_h = (shell->cfg.show_devtools_dock && (h - pad - y) > 220) ? 150 : 0;

    /* 6. Web Content Viewport */
    int32_t content_h = ARCR_MAX(64, (h - pad - dock_h) - y);
    m->content_viewport = (ArcrRect){pad, y, iw, content_h};

    if (dock_h > 0) {
        m->devtools_dock_rect = (ArcrRect){pad, y + content_h, iw, dock_h};
    }

    /* 7. Vertical Scrollbar inside right edge of content viewport when scrollable */
    if (shell->tab_count > 0u && shell->tabs[shell->active_tab].view && iw > 80) {
        ArcrWebView *av = shell->tabs[shell->active_tab].view;
        int32_t doc_h   = arcr_webview_get_content_height(av);
        if (doc_h > content_h) {
            int32_t sb_w = (shell->cfg.theme == ARCR_SHELL_THEME_WIN2000_CLASSIC) ? 14 : 10;
            m->vscrollbar_rect = (ArcrRect){
                m->content_viewport.x + iw - sb_w,
                m->content_viewport.y,
                sb_w,
                content_h
            };
            int32_t thumb_h = ARCR_MAX(24, (content_h * content_h) / ARCR_MAX(1, doc_h));
            thumb_h         = ARCR_MIN(content_h, thumb_h);
            int32_t max_sc  = ARCR_MAX(1, doc_h - content_h);
            int32_t cur_sc  = ARCR_CLAMP(arcr_webview_get_scroll(av), 0, max_sc);
            int32_t track_r = ARCR_MAX(0, content_h - thumb_h);
            int32_t thumb_y = m->vscrollbar_rect.y + (cur_sc * track_r) / max_sc;
            m->vscrollbar_thumb_rect = (ArcrRect){
                m->vscrollbar_rect.x + 1,
                thumb_y,
                ARCR_MAX(2, sb_w - 2),
                thumb_h
            };
        }
    }

    /* 8. Ephemeral Bottom-Left Status Overlay */
    if (shell->status_overlay[0] != '\0') {
        int32_t sw     = ARCR_MIN(iw - 24, 460);
        int32_t base_y = m->content_viewport.y + m->content_viewport.height;
        m->status_overlay_rect = (ArcrRect){pad + 6, base_y - 24, sw, 20};
    }
}

static int32_t compute_tab_pill_width(const ArcrBrowserShell *sh, ArcrRect ts) {
    int32_t right_reserve = sh->cfg.private_browsing ? 210 : 124;
    int32_t avail         = ARCR_MAX(90, ts.width - right_reserve);
    size_t  cnt           = ARCR_MAX(1u, sh->tab_count);
    return ARCR_MIN(200, ARCR_MAX(76, avail / (int32_t)cnt - 4));
}

static void compute_toolbar_controls(const ArcrBrowserShell *sh,
                                     const ArcrShellLayoutMetrics *m,
                                     ToolbarControls *tc) {
    memset(tc, 0, sizeof(*tc));
    ArcrRect ts    = m->tab_strip_rect;
    int32_t  tab_w = compute_tab_pill_width(sh, ts);
    int32_t  tx    = ts.x + 6 + (int32_t)sh->tab_count * (tab_w + 4);
    tc->plus_btn   = (ArcrRect){tx + 2, ts.y + 6, 24, 21};

    ArcrRect nv = m->nav_toolbar_rect;
    int32_t  nx = nv.x + 6;
    int32_t  by = nv.y + 5;
    tc->back_btn   = (ArcrRect){nx, by, 26, 26}; nx += 29;
    tc->fwd_btn    = (ArcrRect){nx, by, 26, 26}; nx += 29;
    tc->reload_btn = (ArcrRect){nx, by, 26, 26}; nx += 29;
    if (sh->cfg.show_home_button && nv.width >= 420) {
        tc->has_home_btn = true;
        tc->home_btn     = (ArcrRect){nx, by, 26, 26};
        nx += 29;
    }

    int32_t rx = nv.x + nv.width - 6;
    rx -= 28;
    tc->menu_btn = (ArcrRect){rx, by, 28, 26};

    if (nv.width >= 560) {
        tc->has_ext_btns = true;
        rx -= 30; tc->addons_btn = (ArcrRect){rx, by, 26, 26};
        rx -= 30; tc->dark_btn   = (ArcrRect){rx, by, 26, 26};
        rx -= 30; tc->reader_btn = (ArcrRect){rx, by, 26, 26};
        rx -= 58; tc->shield_btn = (ArcrRect){rx, by, 54, 26};
    }

    int32_t omni_x = nx + 4;
    int32_t omni_w = ARCR_MAX(64, (rx - 6) - omni_x);
    tc->omnibar    = (ArcrRect){omni_x, nv.y + 5, omni_w, 26};
    tc->star_btn   = (ArcrRect){tc->omnibar.x + tc->omnibar.width - 24, tc->omnibar.y + 2, 22, 22};

    if (sh->show_find_bar && m->find_bar_rect.height > 0) {
        ArcrRect fb = m->find_bar_rect;
        int32_t  iw = ARCR_MIN(260, ARCR_MAX(80, fb.width - 210));
        tc->find_input     = (ArcrRect){fb.x + 62, fb.y + 3, iw, 22};
        int32_t fx         = tc->find_input.x + tc->find_input.width + 86;
        tc->find_prev_btn  = (ArcrRect){fx, fb.y + 3, 24, 22};
        tc->find_next_btn  = (ArcrRect){fx + 27, fb.y + 3, 24, 22};
        tc->find_close_btn = (ArcrRect){fb.x + fb.width - 28, fb.y + 3, 22, 22};
    }
}

/* -------------------------------------------------------------------------
 * Shell Lifecycle & Public State Controls
 * ------------------------------------------------------------------------- */
static void sync_all_tab_viewports(ArcrBrowserShell *sh) {
    ArcrShellLayoutMetrics m;
    arcr_shell_compute_layout(sh, &m);
    for (size_t i = 0u; i < sh->tab_count; ++i) {
        if (sh->tabs[i].view) {
            arcr_webview_resize(sh->tabs[i].view,
                                m.content_viewport.width,
                                m.content_viewport.height);
        }
    }
}

ArcrBrowserShell *arcr_shell_create(ArcrEngine *engine, const ArcrShellConfig *cfg) {
    if (!engine) return NULL;
    ArcrBrowserShell *sh = (ArcrBrowserShell *)calloc(1u, sizeof(ArcrBrowserShell));
    if (!sh) return NULL;

    sh->engine           = engine;
    sh->default_zoom_pct = 100;
    sh->focus            = WS_FOCUS_WEBVIEW;
    if (cfg) {
        sh->cfg = *cfg;
    } else {
        arcr_shell_default_config(&sh->cfg, ARCR_SHELL_THEME_MODERN_LIGHT);
        sh->cfg.load_saved_prefs = true;
    }
    if (!sh->cfg.module_search_dir || sh->cfg.module_search_dir[0] == '\0') {
        sh->cfg.module_search_dir = ".";
    }

    sh->ui_accel = arcr_accel_create(0xFFFFFFFFu, true, false);
    sh->ui_fonts = arcr_font_system_create(sh->cfg.default_cjk_font_path);
    if (!sh->ui_accel || !sh->ui_fonts) {
        arcr_shell_destroy(sh);
        return NULL;
    }

    /* Initialize Add-on & Native Plugin Manager */
    sh->addons = ws_addons_create();
    if (sh->addons) {
        ws_addons_load_builtin_defaults(sh->addons);
        char addons_subdir[512];
        snprintf(addons_subdir, sizeof(addons_subdir), "%s/addons", sh->cfg.module_search_dir);
        ws_addons_scan_directory(sh->addons, addons_subdir);
        ws_addons_scan_directory(sh->addons, sh->cfg.module_search_dir);
        ws_addons_load_native_plugin(sh->addons, sh->cfg.module_search_dir, "ws_plugin_perf_hud");

        /* Keep Shield active by default; Reader & Dark Reader start opt-in */
        ws_addons_set_enabled(sh->addons, "waterscape-shield", true);
        ws_addons_set_enabled(sh->addons, "waterscape-reader", false);
        ws_addons_set_enabled(sh->addons, "waterscape-dark", false);

        arcr_engine_set_url_filter(engine, ws_addons_should_block_url, sh->addons);
    }

    /* Initialize Persistent Profile (unless Private Browsing is active) */
    if (!sh->cfg.private_browsing) {
        if (sh->cfg.profile_dir_override && sh->cfg.profile_dir_override[0] != '\0') {
            arcr_str_copy(sh->profile_dir, sizeof(sh->profile_dir), sh->cfg.profile_dir_override);
        } else {
            arcr_platform_get_profile_dir(sh->profile_dir, sizeof(sh->profile_dir));
        }
        if (sh->profile_dir[0] != '\0') {
            arcr_platform_mkdir_p(sh->profile_dir);
            load_bookmarks_and_history(sh);
            if (sh->cfg.load_saved_prefs) {
                arcr_shell_load_profile_prefs(sh);
            }
        }
    }

    sync_omnibar_from_active_tab(sh);
    return sh;
}

void arcr_shell_destroy(ArcrBrowserShell *sh) {
    if (!sh) return;
    if (sh->engine) {
        arcr_engine_set_url_filter(sh->engine, NULL, NULL);
    }
    for (size_t i = 0u; i < sh->tab_count; ++i) {
        arcr_webview_destroy(sh->tabs[i].view);
    }
    ws_addons_destroy(sh->addons);
    arcr_font_system_destroy(sh->ui_fonts);
    arcr_accel_destroy(sh->ui_accel);
    free(sh);
}

WsAddonManager *arcr_shell_get_addons(ArcrBrowserShell *sh) {
    return sh ? sh->addons : NULL;
}

const char *arcr_shell_get_profile_dir(const ArcrBrowserShell *sh) {
    return sh ? sh->profile_dir : "";
}

bool arcr_shell_is_private_browsing(const ArcrBrowserShell *sh) {
    return sh ? sh->cfg.private_browsing : false;
}

static WsTabSlot *alloc_tab_slot(ArcrBrowserShell *sh, bool activate) {
    if (!sh || sh->tab_count >= ARCR_SHELL_MAX_TABS) return NULL;
    ArcrShellLayoutMetrics m;
    arcr_shell_compute_layout(sh, &m);

    size_t     idx  = sh->tab_count;
    WsTabSlot *slot = &sh->tabs[idx];
    memset(slot, 0, sizeof(*slot));
    slot->owner = sh;

    ArcrWebViewCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.userdata      = slot;
    cbs.on_hover_link = on_webview_hover_link_cb;

    slot->view = arcr_webview_create(sh->engine,
                                     m.content_viewport.width,
                                     m.content_viewport.height,
                                     &cbs);
    if (!slot->view) return NULL;

    if (sh->default_zoom_pct != 100) {
        arcr_webview_set_zoom(slot->view, sh->default_zoom_pct);
    }
    arcr_webview_set_prefers_dark(slot->view, sh->cfg.theme == ARCR_SHELL_THEME_MODERN_DARK);

    sh->tab_count += 1u;
    if (activate || sh->tab_count == 1u) {
        sh->active_tab = idx;
    }
    return slot;
}

int arcr_shell_open_tab_html(ArcrBrowserShell *sh, const char *url,
                             const char *utf8_html, size_t html_len, bool activate) {
    WsTabSlot *slot = alloc_tab_slot(sh, activate);
    if (!slot) return -1;
    const char *eff_url = (url && url[0] != '\0') ? url : "about:blank";
    arcr_webview_load_html(slot->view, eff_url, utf8_html ? utf8_html : "", html_len);
    arcr_str_copy(slot->current_url, sizeof(slot->current_url), eff_url);
    slot->has_inline_html = true;
    push_tab_history(slot, eff_url);
    apply_addons_to_tab(sh, slot, false);
    if (&sh->tabs[sh->active_tab] == slot) {
        sync_omnibar_from_active_tab(sh);
    }
    return (int)(slot - sh->tabs);
}

int arcr_shell_open_tab_url(ArcrBrowserShell *sh, const char *url, bool activate) {
    WsTabSlot *slot = alloc_tab_slot(sh, activate);
    if (!slot) return -1;
    char resolved[512];
    resolve_omnibar_target(url ? url : "waterscape://newtab", resolved, sizeof(resolved));
    load_url_into_slot(sh, slot, resolved, true);
    return (int)(slot - sh->tabs);
}

bool arcr_shell_close_tab(ArcrBrowserShell *sh, size_t tab_index) {
    if (!sh || tab_index >= sh->tab_count) return false;
    arcr_webview_destroy(sh->tabs[tab_index].view);

    for (size_t i = tab_index; i + 1u < sh->tab_count; ++i) {
        sh->tabs[i] = sh->tabs[i + 1u];
        /* Rebind callback userdata pointer after slot shift */
        sh->tabs[i].owner = sh;
    }
    sh->tab_count -= 1u;
    memset(&sh->tabs[sh->tab_count], 0, sizeof(sh->tabs[0]));

    if (sh->tab_count == 0u) {
        arcr_shell_open_tab_url(sh, "waterscape://newtab", true);
        return true;
    }
    if (sh->active_tab >= sh->tab_count) {
        sh->active_tab = sh->tab_count - 1u;
    } else if (tab_index < sh->active_tab && sh->active_tab > 0u) {
        sh->active_tab -= 1u;
    }
    sync_omnibar_from_active_tab(sh);
    return true;
}

bool arcr_shell_activate_tab(ArcrBrowserShell *sh, size_t tab_index) {
    if (!sh || tab_index >= sh->tab_count) return false;
    sh->active_tab = tab_index;
    sh->focus      = WS_FOCUS_WEBVIEW;
    sync_omnibar_from_active_tab(sh);
    return true;
}

size_t arcr_shell_tab_count(const ArcrBrowserShell *sh) {
    return sh ? sh->tab_count : 0u;
}

size_t arcr_shell_active_tab_index(const ArcrBrowserShell *sh) {
    return sh ? sh->active_tab : 0u;
}

bool arcr_shell_navigate(ArcrBrowserShell *sh, const char *url_or_query) {
    if (!sh) return false;
    if (sh->tab_count == 0u) {
        return arcr_shell_open_tab_url(sh, url_or_query, true) >= 0;
    }
    char resolved[512];
    resolve_omnibar_target(url_or_query, resolved, sizeof(resolved));
    sh->focus = WS_FOCUS_WEBVIEW;
    return load_url_into_slot(sh, &sh->tabs[sh->active_tab], resolved, true);
}

bool arcr_shell_can_go_back(const ArcrBrowserShell *sh) {
    if (!sh || sh->tab_count == 0u) return false;
    const WsTabSlot *slot = &sh->tabs[sh->active_tab];
    return slot->hist_count > 1u && slot->hist_pos > 0u;
}

bool arcr_shell_can_go_forward(const ArcrBrowserShell *sh) {
    if (!sh || sh->tab_count == 0u) return false;
    const WsTabSlot *slot = &sh->tabs[sh->active_tab];
    return slot->hist_count > 1u && slot->hist_pos + 1u < slot->hist_count;
}

bool arcr_shell_go_back(ArcrBrowserShell *sh) {
    if (!arcr_shell_can_go_back(sh)) return false;
    WsTabSlot *slot = &sh->tabs[sh->active_tab];
    slot->hist_pos -= 1u;
    return load_url_into_slot(sh, slot, slot->hist_urls[slot->hist_pos], false);
}

bool arcr_shell_go_forward(ArcrBrowserShell *sh) {
    if (!arcr_shell_can_go_forward(sh)) return false;
    WsTabSlot *slot = &sh->tabs[sh->active_tab];
    slot->hist_pos += 1u;
    return load_url_into_slot(sh, slot, slot->hist_urls[slot->hist_pos], false);
}

bool arcr_shell_reload(ArcrBrowserShell *sh) {
    if (!sh || sh->tab_count == 0u) return false;
    WsTabSlot *slot = &sh->tabs[sh->active_tab];
    if (slot->has_inline_html) {
        apply_addons_to_tab(sh, slot, true);
        return true;
    }
    const char *u = slot->current_url[0] ? slot->current_url : "waterscape://newtab";
    char copy_u[512];
    arcr_str_copy(copy_u, sizeof(copy_u), u);
    return load_url_into_slot(sh, slot, copy_u, false);
}

bool arcr_shell_go_home(ArcrBrowserShell *sh) {
    return arcr_shell_navigate(sh, "waterscape://newtab");
}

void arcr_shell_set_theme(ArcrBrowserShell *sh, ArcrShellTheme theme, bool draw_os_frame) {
    if (!sh) return;
    sh->cfg.theme                = theme;
    sh->cfg.draw_os_window_frame = draw_os_frame;
    sync_all_tab_viewports(sh);
    for (size_t i = 0u; i < sh->tab_count; ++i) {
        if (sh->tabs[i].view) {
            arcr_webview_set_prefers_dark(sh->tabs[i].view, theme == ARCR_SHELL_THEME_MODERN_DARK);
            if (!sh->tabs[i].has_inline_html && is_internal_url(sh->tabs[i].current_url)) {
                char copy_u[512];
                arcr_str_copy(copy_u, sizeof(copy_u), sh->tabs[i].current_url);
                load_url_into_slot(sh, &sh->tabs[i], copy_u, false);
            }
        }
    }
    save_prefs_file(sh);
}

ArcrShellTheme arcr_shell_get_theme(const ArcrBrowserShell *sh) {
    return sh ? sh->cfg.theme : ARCR_SHELL_THEME_MODERN_LIGHT;
}

void arcr_shell_set_bookmarks_bar_visible(ArcrBrowserShell *sh, bool visible) {
    if (!sh) return;
    sh->cfg.show_bookmarks_bar = visible;
    sync_all_tab_viewports(sh);
    save_prefs_file(sh);
}

bool arcr_shell_is_bookmarks_bar_visible(const ArcrBrowserShell *sh) {
    return sh ? sh->cfg.show_bookmarks_bar : false;
}

void arcr_shell_set_devtools_visible(ArcrBrowserShell *sh, bool visible) {
    if (!sh) return;
    sh->cfg.show_devtools_dock = visible;
    sync_all_tab_viewports(sh);
}

bool arcr_shell_is_devtools_visible(const ArcrBrowserShell *sh) {
    return sh ? sh->cfg.show_devtools_dock : false;
}

void arcr_shell_set_find_bar_visible(ArcrBrowserShell *sh, bool visible) {
    if (!sh) return;
    sh->show_find_bar = visible;
    if (visible) {
        sh->focus = WS_FOCUS_FINDBAR;
    } else if (sh->focus == WS_FOCUS_FINDBAR) {
        sh->focus = WS_FOCUS_WEBVIEW;
    }
    sync_all_tab_viewports(sh);
}

bool arcr_shell_is_find_bar_visible(const ArcrBrowserShell *sh) {
    return sh ? sh->show_find_bar : false;
}

bool arcr_shell_is_url_bookmarked(const ArcrBrowserShell *sh, const char *url) {
    if (!sh || !url || url[0] == '\0') return false;
    for (size_t i = 0u; i < sh->bookmark_count; ++i) {
        if (strcmp(sh->bookmarks[i].url, url) == 0) return true;
    }
    return false;
}

size_t arcr_shell_bookmark_count(const ArcrBrowserShell *sh) {
    return sh ? sh->bookmark_count : 0u;
}

void arcr_shell_add_bookmark(ArcrBrowserShell *sh, const char *title, const char *url) {
    if (!sh || !url || url[0] == '\0') return;
    if (arcr_shell_is_url_bookmarked(sh, url)) return;
    if (sh->bookmark_count >= ARCR_SHELL_MAX_BOOKMARKS) return;

    ArcrBookmarkItem *b = &sh->bookmarks[sh->bookmark_count++];
    arcr_str_copy(b->title, sizeof(b->title), (title && title[0] != '\0') ? title : url);
    arcr_str_copy(b->url, sizeof(b->url), url);
    save_bookmarks_file(sh);
}

static void toggle_active_page_bookmark(ArcrBrowserShell *sh) {
    if (!sh || sh->tab_count == 0u) return;
    WsTabSlot  *slot  = &sh->tabs[sh->active_tab];
    const char *url   = slot->current_url[0] ? slot->current_url : arcr_webview_get_url(slot->view);
    const char *title = slot->view ? arcr_webview_get_title(slot->view) : url;
    if (!url || url[0] == '\0') return;

    for (size_t i = 0u; i < sh->bookmark_count; ++i) {
        if (strcmp(sh->bookmarks[i].url, url) == 0) {
            for (size_t j = i; j + 1u < sh->bookmark_count; ++j) {
                sh->bookmarks[j] = sh->bookmarks[j + 1u];
            }
            sh->bookmark_count -= 1u;
            save_bookmarks_file(sh);
            arcr_shell_set_status_overlay(sh, "북마크에서 제거되었습니다.");
            return;
        }
    }
    arcr_shell_add_bookmark(sh, title, url);
    arcr_shell_set_status_overlay(sh, "북마크에 추가되었습니다 (Ctrl+B로 북마크 바 표시).");
}

void arcr_shell_set_status_overlay(ArcrBrowserShell *sh, const char *utf8_status_or_null) {
    if (!sh) return;
    arcr_str_copy(sh->status_overlay, sizeof(sh->status_overlay),
                  utf8_status_or_null ? utf8_status_or_null : "");
    sh->status_from_hover = false;
}

int arcr_shell_hit_test_tab(const ArcrBrowserShell *sh, int32_t x, int32_t y) {
    if (!sh || sh->tab_count == 0u) return -1;
    ArcrShellLayoutMetrics m;
    arcr_shell_compute_layout(sh, &m);
    ArcrRect ts = m.tab_strip_rect;
    if (y < ts.y || y >= ts.y + ts.height) return -1;

    int32_t tab_x = ts.x + 6;
    int32_t tab_w = compute_tab_pill_width(sh, ts);
    for (size_t i = 0u; i < sh->tab_count; ++i) {
        if (x >= tab_x && x < tab_x + tab_w) {
            return (int)i;
        }
        tab_x += tab_w + 4;
    }
    return -1;
}

ArcrWebView *arcr_shell_active_webview(ArcrBrowserShell *sh) {
    if (!sh || sh->tab_count == 0u) return NULL;
    return sh->tabs[sh->active_tab].view;
}

/* -------------------------------------------------------------------------
 * Interactive Mouse, Wheel, Keyboard & Korean IME Handling
 * ------------------------------------------------------------------------- */
static size_t utf8_prev_boundary(const char *buf, size_t pos) {
    if (pos == 0u) return 0u;
    size_t p = pos - 1u;
    while (p > 0u && ((unsigned char)buf[p] & 0xC0u) == 0x80u) {
        --p;
    }
    return p;
}

static size_t utf8_next_boundary(const char *buf, size_t len, size_t pos) {
    if (pos >= len) return len;
    uint32_t cp   = 0u;
    size_t   step = arcr_utf8_decode_one(buf + pos, len - pos, &cp);
    return step > 0u ? (pos + step) : (pos + 1u);
}

static void edit_buf_insert(char *buf, size_t cap, size_t *inout_len,
                            size_t *inout_cursor, const char *ins) {
    if (!ins || ins[0] == '\0') return;
    /* Filter control chars like \r or \n */
    char clean[256];
    size_t clen = 0u;
    for (size_t i = 0u; ins[i] != '\0' && clen + 1u < sizeof(clean); ++i) {
        if ((unsigned char)ins[i] >= 0x20u) {
            clean[clen++] = ins[i];
        }
    }
    clean[clen] = '\0';
    if (clen == 0u || *inout_len + clen + 1u > cap) return;

    memmove(buf + *inout_cursor + clen,
            buf + *inout_cursor,
            (*inout_len - *inout_cursor) + 1u);
    memcpy(buf + *inout_cursor, clean, clen);
    *inout_len    += clen;
    *inout_cursor += clen;
}

static void update_find_matches(ArcrBrowserShell *sh, bool advance_next) {
    ArcrWebView *av = arcr_shell_active_webview(sh);
    if (!av || sh->find_len == 0u) {
        sh->find_total_matches = 0u;
        sh->find_active_index  = 0u;
        if (av) arcr_webview_find_in_page(av, "", false, NULL);
        return;
    }
    uint32_t active_idx = 0u;
    sh->find_total_matches = arcr_webview_find_in_page(av, sh->find_buf, advance_next, &active_idx);
    sh->find_active_index  = sh->find_total_matches > 0u ? (active_idx + 1u) : 0u;
}

static void toggle_toolbar_addon(ArcrBrowserShell *sh, const char *addon_id, const char *label) {
    if (!sh || !sh->addons) return;
    bool on = ws_addons_toggle(sh->addons, addon_id);
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: %s", label, on ? "활성화됨" : "비활성화됨");
    arcr_shell_set_status_overlay(sh, msg);

    if (sh->tab_count > 0u) {
        WsTabSlot *slot = &sh->tabs[sh->active_tab];
        if (!slot->has_inline_html && is_internal_url(slot->current_url)) {
            char copy_u[512];
            arcr_str_copy(copy_u, sizeof(copy_u), slot->current_url);
            load_url_into_slot(sh, slot, copy_u, false);
        } else {
            apply_addons_to_tab(sh, slot, true);
        }
    }
}

ArcrCursorKind arcr_shell_on_mouse_move(ArcrBrowserShell *sh, int32_t x, int32_t y) {
    if (!sh) return ARCR_CURSOR_ARROW;
    sh->mouse_x = x;
    sh->mouse_y = y;

    ArcrShellLayoutMetrics m;
    arcr_shell_compute_layout(sh, &m);

    if (sh->dragging_scrollbar && m.vscrollbar_rect.height > 0) {
        ArcrWebView *av = arcr_shell_active_webview(sh);
        if (av) {
            int32_t doc_h   = arcr_webview_get_content_height(av);
            int32_t max_sc  = ARCR_MAX(1, doc_h - m.content_viewport.height);
            int32_t track_r = ARCR_MAX(1, m.vscrollbar_rect.height - m.vscrollbar_thumb_rect.height);
            int32_t rel_y   = ARCR_CLAMP((y - sh->drag_grab_offset_y) - m.vscrollbar_rect.y, 0, track_r);
            arcr_webview_set_scroll(av, (rel_y * max_sc) / track_r);
        }
        return ARCR_CURSOR_ARROW;
    }

    ToolbarControls tc;
    compute_toolbar_controls(sh, &m, &tc);

    if (rect_contains(tc.omnibar, x, y) && !rect_contains(tc.star_btn, x, y)) {
        return ARCR_CURSOR_TEXT;
    }
    if (sh->show_find_bar && rect_contains(tc.find_input, x, y)) {
        return ARCR_CURSOR_TEXT;
    }
    if (arcr_shell_hit_test_tab(sh, x, y) >= 0 ||
        rect_contains(tc.plus_btn, x, y) ||
        rect_contains(tc.back_btn, x, y) ||
        rect_contains(tc.fwd_btn, x, y) ||
        rect_contains(tc.reload_btn, x, y) ||
        (tc.has_home_btn && rect_contains(tc.home_btn, x, y)) ||
        rect_contains(tc.star_btn, x, y) ||
        (tc.has_ext_btns && (rect_contains(tc.shield_btn, x, y) ||
                             rect_contains(tc.reader_btn, x, y) ||
                             rect_contains(tc.dark_btn, x, y) ||
                             rect_contains(tc.addons_btn, x, y))) ||
        rect_contains(tc.menu_btn, x, y) ||
        rect_contains(m.bookmarks_bar_rect, x, y)) {
        return ARCR_CURSOR_POINTER;
    }

    if (rect_contains(m.content_viewport, x, y) && !rect_contains(m.vscrollbar_rect, x, y)) {
        ArcrWebView *av = arcr_shell_active_webview(sh);
        if (av) {
            return arcr_webview_mouse_move(av,
                                           x - m.content_viewport.x,
                                           y - m.content_viewport.y);
        }
    }
    return ARCR_CURSOR_ARROW;
}

bool arcr_shell_on_mouse_up(ArcrBrowserShell *sh, int32_t x, int32_t y, int button) {
    (void)x;
    (void)y;
    (void)button;
    if (!sh) return false;
    bool was_dragging = sh->dragging_scrollbar;
    sh->dragging_scrollbar = false;
    return was_dragging;
}

bool arcr_shell_on_mouse_down(ArcrBrowserShell *sh, int32_t x, int32_t y, int button) {
    if (!sh) return false;
    sh->mouse_x = x;
    sh->mouse_y = y;

    ArcrShellLayoutMetrics m;
    arcr_shell_compute_layout(sh, &m);
    ToolbarControls tc;
    compute_toolbar_controls(sh, &m, &tc);

    /* 1. Tab Strip Hit-Testing (Tab select, middle-click close, 'x' close, '+' new tab) */
    if (rect_contains(m.tab_strip_rect, x, y)) {
        if (button == 0 && rect_contains(tc.plus_btn, x, y)) {
            return arcr_shell_open_tab_url(sh, "waterscape://newtab", true) >= 0;
        }
        int tidx = arcr_shell_hit_test_tab(sh, x, y);
        if (tidx >= 0) {
            int32_t tab_w   = compute_tab_pill_width(sh, m.tab_strip_rect);
            int32_t tab_x   = m.tab_strip_rect.x + 6 + tidx * (tab_w + 4);
            ArcrRect close_r = {tab_x + tab_w - 20, m.tab_strip_rect.y + 7, 16, 16};
            if (button == 1 || (button == 0 && rect_contains(close_r, x, y))) {
                return arcr_shell_close_tab(sh, (size_t)tidx);
            }
            return arcr_shell_activate_tab(sh, (size_t)tidx);
        }
        return false;
    }

    /* 2. Navigation Toolbar Controls */
    if (rect_contains(m.nav_toolbar_rect, x, y)) {
        if (button == 0 && rect_contains(tc.back_btn, x, y))   return arcr_shell_go_back(sh);
        if (button == 0 && rect_contains(tc.fwd_btn, x, y))    return arcr_shell_go_forward(sh);
        if (button == 0 && rect_contains(tc.reload_btn, x, y)) return arcr_shell_reload(sh);
        if (button == 0 && tc.has_home_btn && rect_contains(tc.home_btn, x, y)) {
            return arcr_shell_go_home(sh);
        }
        if (button == 0 && rect_contains(tc.star_btn, x, y)) {
            toggle_active_page_bookmark(sh);
            return true;
        }
        if (button == 0 && rect_contains(tc.omnibar, x, y)) {
            if (sh->focus != WS_FOCUS_OMNIBAR) {
                sh->focus           = WS_FOCUS_OMNIBAR;
                sh->omni_select_all = true;
                sh->omni_cursor     = sh->omni_len;
            } else {
                sh->omni_select_all = false;
                sh->omni_cursor     = sh->omni_len;
            }
            return true;
        }
        if (button == 0 && tc.has_ext_btns) {
            if (rect_contains(tc.shield_btn, x, y)) {
                toggle_toolbar_addon(sh, "waterscape-shield", "Waterscape Shield 차단기");
                return true;
            }
            if (rect_contains(tc.reader_btn, x, y)) {
                toggle_toolbar_addon(sh, "waterscape-reader", "리더 모드 (Reader View)");
                return true;
            }
            if (rect_contains(tc.dark_btn, x, y)) {
                toggle_toolbar_addon(sh, "waterscape-dark", "다크 모드 변환기");
                return true;
            }
            if (rect_contains(tc.addons_btn, x, y)) {
                return arcr_shell_navigate(sh, "waterscape://addons");
            }
        }
        if (button == 0 && rect_contains(tc.menu_btn, x, y)) {
            return arcr_shell_navigate(sh, "waterscape://settings");
        }
        return false;
    }

    /* 3. Bookmarks Bar Items */
    if (sh->cfg.show_bookmarks_bar && rect_contains(m.bookmarks_bar_rect, x, y)) {
        int32_t bx = m.bookmarks_bar_rect.x + 8;
        for (size_t i = 0u; i < sh->bookmark_count; ++i) {
            /* Approximate pill width from glyph measurement */
            int32_t tw = 0;
            size_t  bl = strlen(sh->bookmarks[i].title);
            size_t  bp = 0u;
            while (bp < bl) {
                uint32_t cp = 0u;
                size_t   st = arcr_utf8_decode_one(sh->bookmarks[i].title + bp, bl - bp, &cp);
                if (st == 0u) break;
                bp += st;
                ArcrGlyphMetrics gm;
                if (arcr_font_measure_glyph(sh->ui_fonts, cp, 12, false, &gm)) {
                    tw += gm.advance_px;
                }
            }
            int32_t pill_w = tw + 16;
            if (x >= bx && x < bx + pill_w) {
                if (button == 1) {
                    return arcr_shell_open_tab_url(sh, sh->bookmarks[i].url, true) >= 0;
                }
                return arcr_shell_navigate(sh, sh->bookmarks[i].url);
            }
            bx += pill_w + 6;
        }
        return false;
    }

    /* 4. Find-in-Page Bar */
    if (sh->show_find_bar && rect_contains(m.find_bar_rect, x, y)) {
        if (rect_contains(tc.find_input, x, y)) {
            sh->focus = WS_FOCUS_FINDBAR;
            return true;
        }
        if (rect_contains(tc.find_prev_btn, x, y) || rect_contains(tc.find_next_btn, x, y)) {
            update_find_matches(sh, true);
            return true;
        }
        if (rect_contains(tc.find_close_btn, x, y)) {
            arcr_shell_set_find_bar_visible(sh, false);
            return true;
        }
        return false;
    }

    /* 5. Vertical Scrollbar */
    if (button == 0 && rect_contains(m.vscrollbar_rect, x, y)) {
        ArcrWebView *av = arcr_shell_active_webview(sh);
        if (av) {
            if (rect_contains(m.vscrollbar_thumb_rect, x, y)) {
                sh->dragging_scrollbar = true;
                sh->drag_grab_offset_y = y - m.vscrollbar_thumb_rect.y;
            } else {
                int32_t doc_h   = arcr_webview_get_content_height(av);
                int32_t max_sc  = ARCR_MAX(1, doc_h - m.content_viewport.height);
                int32_t rel_y   = ARCR_CLAMP(y - m.vscrollbar_rect.y -
                                             m.vscrollbar_thumb_rect.height / 2,
                                             0,
                                             ARCR_MAX(1, m.vscrollbar_rect.height -
                                                         m.vscrollbar_thumb_rect.height));
                int32_t track_r = ARCR_MAX(1, m.vscrollbar_rect.height -
                                              m.vscrollbar_thumb_rect.height);
                arcr_webview_set_scroll(av, (rel_y * max_sc) / track_r);
                sh->dragging_scrollbar = true;
                sh->drag_grab_offset_y = m.vscrollbar_thumb_rect.height / 2;
            }
            return true;
        }
    }

    /* 6. Web Content Viewport Click */
    if (rect_contains(m.content_viewport, x, y)) {
        sh->focus           = WS_FOCUS_WEBVIEW;
        sh->omni_select_all = false;
        ArcrWebView *av     = arcr_shell_active_webview(sh);
        if (!av) return false;

        char nav_url[512];
        nav_url[0] = '\0';
        bool open_new_tab = false;
        bool handled = arcr_webview_mouse_click(av,
                                                x - m.content_viewport.x,
                                                y - m.content_viewport.y,
                                                button,
                                                nav_url, sizeof(nav_url),
                                                &open_new_tab);
        if (nav_url[0] != '\0') {
            if (open_new_tab || button == 1) {
                return arcr_shell_open_tab_url(sh, nav_url, true) >= 0;
            }
            return arcr_shell_navigate(sh, nav_url);
        }
        return handled;
    }

    return false;
}

bool arcr_shell_on_mouse_wheel(ArcrBrowserShell *sh, int32_t wheel_steps, bool ctrl_held) {
    if (!sh || wheel_steps == 0) return false;
    ArcrWebView *av = arcr_shell_active_webview(sh);
    if (!av) return false;

    if (ctrl_held) {
        int32_t cur = arcr_webview_get_zoom(av);
        if (cur <= 0) cur = sh->default_zoom_pct;
        cur = ARCR_CLAMP(cur + wheel_steps * 10, 50, 300);
        sh->default_zoom_pct = cur;
        arcr_webview_set_zoom(av, cur);
        save_prefs_file(sh);
        char msg[64];
        snprintf(msg, sizeof(msg), "화면 확대/축소: %d%%", (int)cur);
        arcr_shell_set_status_overlay(sh, msg);
        return true;
    }

    ArcrShellLayoutMetrics m;
    arcr_shell_compute_layout(sh, &m);
    int32_t doc_h  = arcr_webview_get_content_height(av);
    int32_t max_sc = ARCR_MAX(0, doc_h - m.content_viewport.height);
    int32_t next_y = ARCR_CLAMP(arcr_webview_get_scroll(av) - wheel_steps * 56, 0, max_sc);
    arcr_webview_set_scroll(av, next_y);
    return true;
}

bool arcr_shell_on_key(ArcrBrowserShell *sh, const char *utf8_text,
                       ArcrSpecialKey special_key,
                       bool ctrl_held, bool alt_held, int fn_key) {
    if (!sh) return false;

    /* 1. Function Keys: F2 (Theme), F5 (Reload), F12 (Hardware & DevTools Dock) */
    if (fn_key == 2) {
        ArcrShellTheme next = (ArcrShellTheme)(((int)sh->cfg.theme + 1) % 3);
        arcr_shell_set_theme(sh, next, false);
        return true;
    }
    if (fn_key == 5) {
        return arcr_shell_reload(sh);
    }
    if (fn_key == 12) {
        arcr_shell_set_devtools_visible(sh, !sh->cfg.show_devtools_dock);
        return true;
    }

    /* 2. Alt+Left / Alt+Right History Navigation */
    if (alt_held && !ctrl_held) {
        if (special_key == ARCR_KEY_LEFT)  return arcr_shell_go_back(sh);
        if (special_key == ARCR_KEY_RIGHT) return arcr_shell_go_forward(sh);
    }

    /* 3. Ctrl+<Key> Browser Accelerators */
    if (ctrl_held && utf8_text && utf8_text[0] != '\0') {
        char k = arcr_ascii_tolower(utf8_text[0]);
        if (k == 't') return arcr_shell_open_tab_url(sh, "waterscape://newtab", true) >= 0;
        if (k == 'w') return arcr_shell_close_tab(sh, sh->active_tab);
        if (k == 'l') {
            sh->focus           = WS_FOCUS_OMNIBAR;
            sh->omni_select_all = true;
            sh->omni_cursor     = sh->omni_len;
            return true;
        }
        if (k == 'b') {
            arcr_shell_set_bookmarks_bar_visible(sh, !sh->cfg.show_bookmarks_bar);
            return true;
        }
        if (k == 'f') {
            arcr_shell_set_find_bar_visible(sh, true);
            return true;
        }
        if (k == 'd') {
            toggle_active_page_bookmark(sh);
            return true;
        }
        if (k == 'r') return arcr_shell_reload(sh);
        if (k == '=' || k == '+') return arcr_shell_on_mouse_wheel(sh, 1, true);
        if (k == '-')             return arcr_shell_on_mouse_wheel(sh, -1, true);
        if (k == '0') {
            ArcrWebView *av = arcr_shell_active_webview(sh);
            sh->default_zoom_pct = 100;
            if (av) arcr_webview_set_zoom(av, 100);
            save_prefs_file(sh);
            arcr_shell_set_status_overlay(sh, "화면 배율: 100%");
            return true;
        }
        if (k == 'a' && sh->focus == WS_FOCUS_OMNIBAR) {
            sh->omni_select_all = true;
            sh->omni_cursor     = sh->omni_len;
            return true;
        }
        if (k == 'c' && sh->focus == WS_FOCUS_OMNIBAR) {
            arcr_platform_clipboard_set_utf8(sh->omni_buf);
            return true;
        }
        if (k == 'v' && sh->focus == WS_FOCUS_OMNIBAR) {
            char clip[512];
            if (arcr_platform_clipboard_get_utf8(clip, sizeof(clip))) {
                if (sh->omni_select_all) {
                    sh->omni_buf[0]     = '\0';
                    sh->omni_len        = 0u;
                    sh->omni_cursor     = 0u;
                    sh->omni_select_all = false;
                }
                edit_buf_insert(sh->omni_buf, sizeof(sh->omni_buf),
                                &sh->omni_len, &sh->omni_cursor, clip);
                return true;
            }
        }
    }

    /* 4. Editable Omnibar Input Routing */
    if (sh->focus == WS_FOCUS_OMNIBAR) {
        if (special_key == ARCR_KEY_ESCAPE) {
            sync_omnibar_from_active_tab(sh);
            sh->focus = WS_FOCUS_WEBVIEW;
            return true;
        }
        if (special_key == ARCR_KEY_ENTER) {
            char target[512];
            arcr_str_copy(target, sizeof(target), sh->omni_buf);
            sh->omni_select_all = false;
            return arcr_shell_navigate(sh, target);
        }
        if (special_key == ARCR_KEY_LEFT) {
            if (sh->omni_select_all) {
                sh->omni_select_all = false;
                sh->omni_cursor     = 0u;
            } else {
                sh->omni_cursor = utf8_prev_boundary(sh->omni_buf, sh->omni_cursor);
            }
            return true;
        }
        if (special_key == ARCR_KEY_RIGHT) {
            if (sh->omni_select_all) {
                sh->omni_select_all = false;
                sh->omni_cursor     = sh->omni_len;
            } else {
                sh->omni_cursor = utf8_next_boundary(sh->omni_buf, sh->omni_len, sh->omni_cursor);
            }
            return true;
        }
        if (special_key == ARCR_KEY_HOME) {
            sh->omni_select_all = false;
            sh->omni_cursor     = 0u;
            return true;
        }
        if (special_key == ARCR_KEY_END) {
            sh->omni_select_all = false;
            sh->omni_cursor     = sh->omni_len;
            return true;
        }
        if (special_key == ARCR_KEY_BACKSPACE) {
            if (sh->omni_select_all) {
                sh->omni_buf[0]     = '\0';
                sh->omni_len        = 0u;
                sh->omni_cursor     = 0u;
                sh->omni_select_all = false;
            } else if (sh->omni_cursor > 0u) {
                size_t prev = utf8_prev_boundary(sh->omni_buf, sh->omni_cursor);
                size_t del  = sh->omni_cursor - prev;
                memmove(sh->omni_buf + prev,
                        sh->omni_buf + sh->omni_cursor,
                        (sh->omni_len - sh->omni_cursor) + 1u);
                sh->omni_len   -= del;
                sh->omni_cursor = prev;
            }
            return true;
        }
        if (special_key == ARCR_KEY_DELETE) {
            if (sh->omni_select_all) {
                sh->omni_buf[0]     = '\0';
                sh->omni_len        = 0u;
                sh->omni_cursor     = 0u;
                sh->omni_select_all = false;
            } else if (sh->omni_cursor < sh->omni_len) {
                size_t next = utf8_next_boundary(sh->omni_buf, sh->omni_len, sh->omni_cursor);
                size_t del  = next - sh->omni_cursor;
                memmove(sh->omni_buf + sh->omni_cursor,
                        sh->omni_buf + next,
                        (sh->omni_len - next) + 1u);
                sh->omni_len -= del;
            }
            return true;
        }
        if (!ctrl_held && !alt_held && utf8_text && utf8_text[0] != '\0') {
            if (sh->omni_select_all) {
                sh->omni_buf[0]     = '\0';
                sh->omni_len        = 0u;
                sh->omni_cursor     = 0u;
                sh->omni_select_all = false;
            }
            edit_buf_insert(sh->omni_buf, sizeof(sh->omni_buf),
                            &sh->omni_len, &sh->omni_cursor, utf8_text);
            return true;
        }
        return false;
    }

    /* 5. Find-in-Page Bar Input Routing */
    if (sh->focus == WS_FOCUS_FINDBAR) {
        if (special_key == ARCR_KEY_ESCAPE) {
            arcr_shell_set_find_bar_visible(sh, false);
            return true;
        }
        if (special_key == ARCR_KEY_ENTER) {
            update_find_matches(sh, true);
            return true;
        }
        if (special_key == ARCR_KEY_BACKSPACE && sh->find_cursor > 0u) {
            size_t prev = utf8_prev_boundary(sh->find_buf, sh->find_cursor);
            size_t del  = sh->find_cursor - prev;
            memmove(sh->find_buf + prev,
                    sh->find_buf + sh->find_cursor,
                    (sh->find_len - sh->find_cursor) + 1u);
            sh->find_len   -= del;
            sh->find_cursor = prev;
            update_find_matches(sh, false);
            return true;
        }
        if (!ctrl_held && !alt_held && utf8_text && utf8_text[0] != '\0') {
            edit_buf_insert(sh->find_buf, sizeof(sh->find_buf),
                            &sh->find_len, &sh->find_cursor, utf8_text);
            update_find_matches(sh, false);
            return true;
        }
        return false;
    }

    /* 6. WebView Form Input & Escape to dismiss Find bar */
    if (special_key == ARCR_KEY_ESCAPE && sh->show_find_bar) {
        arcr_shell_set_find_bar_visible(sh, false);
        return true;
    }

    ArcrWebView *av = arcr_shell_active_webview(sh);
    if (av && !ctrl_held && !alt_held) {
        char submitted_url[512];
        submitted_url[0] = '\0';
        bool handled = arcr_webview_key_input(av, utf8_text, special_key,
                                              submitted_url, sizeof(submitted_url));
        if (submitted_url[0] != '\0') {
            return arcr_shell_navigate(sh, submitted_url);
        }
        return handled;
    }
    return false;
}

void arcr_shell_on_ime_composition(ArcrBrowserShell *sh, const char *utf8_comp_str,
                                   int32_t cursor_offset, bool is_committed) {
    if (!sh || !utf8_comp_str) return;
    if (sh->focus == WS_FOCUS_OMNIBAR || sh->focus == WS_FOCUS_FINDBAR) {
        if (is_committed && utf8_comp_str[0] != '\0') {
            arcr_shell_on_key(sh, utf8_comp_str, ARCR_KEY_NONE, false, false, 0);
        }
        return;
    }
    ArcrWebView *av = arcr_shell_active_webview(sh);
    if (av) {
        arcr_webview_ime_composition(av, utf8_comp_str, cursor_offset, is_committed);
    }
}

/* -------------------------------------------------------------------------
 * Pure C11 UI Drawing Primitives & Pixel-Crisp Vector Toolbar Icons
 * ------------------------------------------------------------------------- */
static void ui_fill(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                    ArcrRect r, uint32_t color) {
    arcr_accel_fill_rect(sh->ui_accel, fb, w, h, st, r, color);
}

static void ui_box(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                   ArcrRect r, uint32_t bg_c, uint32_t border_c, int32_t radius, bool classic) {
    if (classic) {
        ui_fill(sh, fb, w, h, st, r, bg_c);
    } else {
        arcr_accel_fill_rounded_rect(sh->ui_accel, fb, w, h, st, r, radius, bg_c);
        arcr_accel_stroke_rounded_rect(sh->ui_accel, fb, w, h, st, r, radius, 1, border_c);
    }
}

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

static int32_t ui_draw_text_ex(ArcrBrowserShell *sh, uint32_t *fb,
                               int32_t fb_w, int32_t fb_h, int32_t stride,
                               const char *utf8, size_t max_bytes,
                               int32_t x, int32_t baseline_y,
                               int32_t max_right_x, int32_t font_px,
                               bool bold, bool mono, uint32_t color_argb) {
    if (!utf8) return x;
    size_t  len   = strlen(utf8);
    if (max_bytes < len) len = max_bytes;
    size_t  pos   = 0u;
    int32_t pen_x = x;
    uint8_t mask[64 * 64];

    while (pos < len) {
        uint32_t cp   = 0u;
        size_t   step = arcr_utf8_decode_one(utf8 + pos, len - pos, &cp);
        if (step == 0u) break;
        pos += step;
        if (cp == '\r' || cp == '\n') break;

        ArcrGlyphMetrics gm;
        if (!arcr_font_rasterize_glyph_ex(sh->ui_fonts, cp, font_px, bold, mono,
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

static int32_t ui_draw_text_clipped(ArcrBrowserShell *sh, uint32_t *fb,
                                    int32_t fb_w, int32_t fb_h, int32_t stride,
                                    const char *utf8, int32_t x, int32_t baseline_y,
                                    int32_t max_right_x, int32_t font_px, bool bold,
                                    uint32_t color_argb) {
    return ui_draw_text_ex(sh, fb, fb_w, fb_h, stride, utf8, (size_t)-1,
                           x, baseline_y, max_right_x, font_px, bold, false, color_argb);
}

/* Crisp pixel-aligned icons (guaranteed sharp even without emoji fonts!) */
static void ui_icon_arrow_h(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                            int32_t x, int32_t y, bool right, uint32_t c) {
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 1, y + 5, 10, 2}, c);
    for (int32_t i = 0; i < 4; ++i) {
        int32_t ax = right ? (x + 9 - i) : (x + 2 + i);
        ui_fill(sh, fb, w, h, st, (ArcrRect){ax, y + 5 - i - 1, 2, 1}, c);
        ui_fill(sh, fb, w, h, st, (ArcrRect){ax, y + 6 + i + 1, 2, 1}, c);
    }
}

static void ui_icon_reload(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                           int32_t x, int32_t y, uint32_t c) {
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 2, y + 1, 7, 2}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 2, y + 9, 7, 2}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 1, y + 2, 2, 8}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 9, y + 6, 2, 4}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 7, y,     4, 4}, c);
}

static void ui_icon_home(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                         int32_t x, int32_t y, uint32_t c) {
    for (int32_t r = 0; r < 5; ++r) {
        ui_fill(sh, fb, w, h, st, (ArcrRect){x + 5 - r, y + 1 + r, 2 + r * 2, 1}, c);
    }
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 2, y + 6, 8, 5}, c);
}

static void ui_icon_lock(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                         int32_t x, int32_t y, uint32_t c) {
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 3, y + 1, 5, 1}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 2, y + 2, 1, 3}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 8, y + 2, 1, 3}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 1, y + 5, 9, 6}, c);
}

static void ui_icon_shield(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                           int32_t x, int32_t y, uint32_t c) {
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 1, y + 1, 8, 5}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 2, y + 6, 6, 3}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 4, y + 9, 2, 2}, c);
}

static void ui_icon_book(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                         int32_t x, int32_t y, uint32_t c) {
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 1, y + 2, 4, 8}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 7, y + 2, 4, 8}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 5, y + 3, 2, 8}, c);
}

static void ui_icon_moon(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                         int32_t x, int32_t y, uint32_t c) {
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 3, y + 1, 5, 2}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 2, y + 3, 3, 6}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 3, y + 9, 6, 2}, c);
}

static void ui_icon_puzzle(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                           int32_t x, int32_t y, uint32_t c) {
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 4, y + 1, 3, 2}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 2, y + 3, 7, 7}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x + 9, y + 5, 2, 3}, c);
}

static void ui_icon_menu(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                         int32_t x, int32_t y, uint32_t c) {
    ui_fill(sh, fb, w, h, st, (ArcrRect){x, y + 1, 12, 2}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x, y + 5, 12, 2}, c);
    ui_fill(sh, fb, w, h, st, (ArcrRect){x, y + 9, 12, 2}, c);
}

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

static void ui_draw_toolbar_btn(ArcrBrowserShell *sh, uint32_t *fb, int32_t w, int32_t h, int32_t st,
                                const ShellPalette *pal, ArcrRect r, bool active_state) {
    uint32_t bg = active_state ? pal->btn_active_bg : pal->btn_bg;
    uint32_t bd = active_state ? pal->omnibar_focus_border : pal->omnibar_border;
    ui_box(sh, fb, w, h, st, r, bg, bd, 5, pal->classic_3d_bevels);
    if (pal->classic_3d_bevels) {
        ui_win2k_bevel(sh, fb, w, h, st, r, active_state);
    }
}

static bool is_addon_enabled(const WsAddonManager *mgr, const char *addon_id) {
    if (!mgr) return false;
    WsAddonInfo info;
    return ws_addons_find_by_id(mgr, addon_id, &info) && info.enabled;
}

/* -------------------------------------------------------------------------
 * Full Waterscape Browser Frame Renderer
 * ------------------------------------------------------------------------- */
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
    ToolbarControls tc;
    compute_toolbar_controls(sh, &m, &tc);

    ui_fill(sh, fb, w, h, st, (ArcrRect){0, 0, w, h}, pal.toolbar_bg);

    /* Optional Win2000 Classic outer 3D frame & gradient titlebar */
    if (sh->cfg.draw_os_window_frame && pal.classic_3d_bevels) {
        ui_win2k_bevel(sh, fb, w, h, st, (ArcrRect){0, 0, w, h}, false);
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
        ui_draw_text_clipped(sh, fb, w, h, st,
                             "Waterscape Browser 1.0 (Arcradient Pure C11 · Windows 2000 Classic)",
                             cap.x + 6, cap.y + 15, cap.x + cap.width - 60, 12, true, pal.titlebar_text);
        for (int bi = 0; bi < 3; ++bi) {
            ArcrRect br = {cap.x + cap.width - 54 + bi * 18, cap.y + 2, 16, 16};
            ui_fill(sh, fb, w, h, st, br, pal.toolbar_bg);
            ui_win2k_bevel(sh, fb, w, h, st, br, false);
            ui_draw_caption_glyph(sh, fb, w, h, st, bi, br.x + 4, br.y + 4, 0xFF000000u);
        }
    }

    /* 1. Tab Strip (32px) */
    ArcrRect ts = m.tab_strip_rect;
    ui_fill(sh, fb, w, h, st, ts, pal.tab_strip_bg);
    ui_fill(sh, fb, w, h, st, (ArcrRect){ts.x, ts.y + ts.height - 1, ts.width, 1}, pal.toolbar_divider);

    int32_t tab_x = ts.x + 6;
    int32_t tab_w = compute_tab_pill_width(sh, ts);
    for (size_t i = 0u; i < sh->tab_count; ++i) {
        bool is_active = (i == sh->active_tab);
        ArcrRect tr = {
            .x      = tab_x,
            .y      = ts.y + (is_active ? 3 : 6),
            .width  = tab_w,
            .height = ts.height - (is_active ? 3 : 6)
        };
        if (pal.classic_3d_bevels) {
            ui_fill(sh, fb, w, h, st, tr, is_active ? pal.tab_active_bg : pal.tab_inactive_bg);
            ui_win2k_bevel(sh, fb, w, h, st, tr, false);
            if (is_active) {
                ui_fill(sh, fb, w, h, st,
                        (ArcrRect){tr.x + 2, tr.y + tr.height - 2, tr.width - 4, 3},
                        pal.toolbar_bg);
            }
        } else {
            ui_box(sh, fb, w, h, st, tr,
                   is_active ? pal.tab_active_bg : pal.tab_inactive_bg,
                   is_active ? pal.tab_active_border : pal.tab_inactive_border,
                   6, false);
            if (is_active) {
                ui_fill(sh, fb, w, h, st,
                        (ArcrRect){tr.x + 4, tr.y, tr.width - 8, 3}, pal.tab_active_accent);
                ui_fill(sh, fb, w, h, st,
                        (ArcrRect){tr.x + 1, tr.y + tr.height - 2, tr.width - 2, 3},
                        pal.toolbar_bg);
            }
        }

        const char *ttitle = sh->tabs[i].view ? arcr_webview_get_title(sh->tabs[i].view) : "새 탭";
        if (!ttitle || ttitle[0] == '\0') ttitle = "Waterscape 새 탭";
        ui_draw_text_clipped(sh, fb, w, h, st, ttitle,
                             tr.x + 10, tr.y + tr.height - 8, tr.x + tr.width - 22,
                             12, is_active, is_active ? pal.text_primary : pal.text_muted);
        ui_draw_text_clipped(sh, fb, w, h, st, "×",
                             tr.x + tr.width - 16, tr.y + tr.height - 8, tr.x + tr.width - 3,
                             12, true, pal.text_muted);
        tab_x += tab_w + 4;
    }

    /* '+' New Tab Button */
    ui_draw_toolbar_btn(sh, fb, w, h, st, &pal, tc.plus_btn, false);
    ui_draw_text_clipped(sh, fb, w, h, st, "+",
                         tc.plus_btn.x + 7, tc.plus_btn.y + 15, tc.plus_btn.x + 22,
                         14, true, pal.text_primary);

    /* Private Mode Pill */
    if (sh->cfg.private_browsing && ts.width >= 420) {
        int32_t px = ts.x + ts.width - (sh->cfg.draw_os_window_frame ? 104 : 176);
        ArcrRect pr = {px, ts.y + 6, 94, 20};
        ui_box(sh, fb, w, h, st, pr, pal.private_bg, pal.private_fg, 4, pal.classic_3d_bevels);
        ui_draw_text_clipped(sh, fb, w, h, st, "사생활 보호",
                             pr.x + 10, pr.y + 14, pr.x + pr.width - 6, 11, true, pal.private_fg);
    }

    if (!sh->cfg.draw_os_window_frame && ts.width >= 360) {
        int32_t wx = ts.x + ts.width - 74;
        for (int bi = 0; bi < 3; ++bi) {
            ui_draw_caption_glyph(sh, fb, w, h, st, bi, wx + bi * 22, ts.y + 11, pal.text_muted);
        }
    }

    /* 2. Navigation Toolbar (36px) */
    ArcrRect nv = m.nav_toolbar_rect;
    ui_fill(sh, fb, w, h, st, nv, pal.toolbar_bg);
    ui_fill(sh, fb, w, h, st, (ArcrRect){nv.x, nv.y + nv.height - 1, nv.width, 1}, pal.toolbar_divider);

    ui_draw_toolbar_btn(sh, fb, w, h, st, &pal, tc.back_btn, false);
    ui_icon_arrow_h(sh, fb, w, h, st, tc.back_btn.x + 7, tc.back_btn.y + 7, false,
                    arcr_shell_can_go_back(sh) ? pal.text_primary : pal.text_muted);

    ui_draw_toolbar_btn(sh, fb, w, h, st, &pal, tc.fwd_btn, false);
    ui_icon_arrow_h(sh, fb, w, h, st, tc.fwd_btn.x + 7, tc.fwd_btn.y + 7, true,
                    arcr_shell_can_go_forward(sh) ? pal.text_primary : pal.text_muted);

    ui_draw_toolbar_btn(sh, fb, w, h, st, &pal, tc.reload_btn, false);
    ui_icon_reload(sh, fb, w, h, st, tc.reload_btn.x + 7, tc.reload_btn.y + 7, pal.text_primary);

    if (tc.has_home_btn) {
        ui_draw_toolbar_btn(sh, fb, w, h, st, &pal, tc.home_btn, false);
        ui_icon_home(sh, fb, w, h, st, tc.home_btn.x + 7, tc.home_btn.y + 7, pal.text_primary);
    }

    /* Editable Omnibar with Security State Badge + Bookmark Star */
    bool omni_focused = (sh->focus == WS_FOCUS_OMNIBAR);
    ui_box(sh, fb, w, h, st, tc.omnibar, pal.omnibar_bg,
           omni_focused ? pal.omnibar_focus_border : pal.omnibar_border,
           6, pal.classic_3d_bevels);
    if (pal.classic_3d_bevels) {
        ui_win2k_bevel(sh, fb, w, h, st, tc.omnibar, true);
    }

    ArcrWebView      *active_v = arcr_shell_active_webview(sh);
    const char       *live_url = (sh->tab_count > 0u && sh->tabs[sh->active_tab].current_url[0])
                                     ? sh->tabs[sh->active_tab].current_url
                                     : (active_v ? arcr_webview_get_url(active_v) : "waterscape://newtab");
    ArcrSecurityState sec      = active_v ? arcr_webview_get_security_state(active_v)
                                          : ARCR_SEC_LOCAL_OR_NONE;

    int32_t url_text_x = tc.omnibar.x + 8;
    if (tc.omnibar.width >= 180) {
        ArcrRect badge = {tc.omnibar.x + 4, tc.omnibar.y + 3, 68, tc.omnibar.height - 6};
        if (is_internal_url(live_url)) {
            ui_box(sh, fb, w, h, st, badge, pal.sec_internal_bg, pal.sec_internal_bg, 4, pal.classic_3d_bevels);
            ui_draw_text_clipped(sh, fb, w, h, st, "ⓘ 내부",
                                 badge.x + 8, badge.y + 14, badge.x + badge.width - 4,
                                 11, true, pal.sec_internal_fg);
        } else if (sec == ARCR_SEC_HTTPS_VERIFIED) {
            ui_box(sh, fb, w, h, st, badge, pal.tls_verified_bg, pal.tls_verified_bg, 4, pal.classic_3d_bevels);
            ui_icon_lock(sh, fb, w, h, st, badge.x + 5, badge.y + 4, pal.tls_verified_fg);
            ui_draw_text_clipped(sh, fb, w, h, st, "HTTPS",
                                 badge.x + 19, badge.y + 14, badge.x + badge.width - 4,
                                 11, true, pal.tls_verified_fg);
        } else {
            ui_box(sh, fb, w, h, st, badge, pal.sec_warn_bg, pal.sec_warn_bg, 4, pal.classic_3d_bevels);
            ui_draw_text_clipped(sh, fb, w, h, st, "! 주의",
                                 badge.x + 10, badge.y + 14, badge.x + badge.width - 4,
                                 11, true, pal.sec_warn_fg);
        }
        url_text_x = badge.x + badge.width + 8;
    }

    const char *disp_url = omni_focused ? sh->omni_buf : live_url;
    int32_t     url_max  = tc.star_btn.x - 4;
    if (omni_focused && sh->omni_select_all && sh->omni_len > 0u && url_max > url_text_x) {
        ui_fill(sh, fb, w, h, st,
                (ArcrRect){url_text_x - 2, tc.omnibar.y + 4,
                           ARCR_MIN(url_max - url_text_x, 320), tc.omnibar.height - 8},
                pal.selection_bg);
    }
    ui_draw_text_clipped(sh, fb, w, h, st, disp_url,
                         url_text_x, tc.omnibar.y + 18, url_max,
                         13, false,
                         (omni_focused && sh->omni_select_all && sh->cfg.theme != ARCR_SHELL_THEME_MODERN_LIGHT)
                             ? 0xFFFFFFFFu
                             : pal.text_primary);
    if (omni_focused) {
        int32_t caret_x = ui_draw_text_ex(sh, fb, w, h, st,
                                          sh->omni_buf, sh->omni_cursor,
                                          url_text_x, tc.omnibar.y + 18, url_max,
                                          13, false, false, 0x00000000u);
        if (caret_x < url_max) {
            ui_fill(sh, fb, w, h, st,
                    (ArcrRect){caret_x, tc.omnibar.y + 5, 2, tc.omnibar.height - 10},
                    pal.omnibar_focus_border);
        }
    }

    bool bookmarked = arcr_shell_is_url_bookmarked(sh, live_url);
    ui_draw_text_clipped(sh, fb, w, h, st, bookmarked ? "★" : "☆",
                         tc.star_btn.x + 4, tc.omnibar.y + 18, tc.omnibar.x + tc.omnibar.width - 2,
                         13, true, bookmarked ? pal.star_gold : pal.text_muted);

    /* Right-side Waterscape Shield, Reader, Dark Mode, Add-ons & Settings Menu Buttons */
    if (tc.has_ext_btns) {
        bool shield_on = is_addon_enabled(sh->addons, "waterscape-shield");
        bool reader_on = is_addon_enabled(sh->addons, "waterscape-reader");
        bool dark_on   = is_addon_enabled(sh->addons, "waterscape-dark");
        uint32_t blk   = sh->addons ? ws_addons_total_blocked(sh->addons) : 0u;

        ui_draw_toolbar_btn(sh, fb, w, h, st, &pal, tc.shield_btn, shield_on);
        ui_icon_shield(sh, fb, w, h, st, tc.shield_btn.x + 6, tc.shield_btn.y + 7,
                       shield_on ? pal.btn_active_fg : pal.text_muted);
        char blk_txt[24];
        snprintf(blk_txt, sizeof(blk_txt), "%lu", (unsigned long)blk);
        ui_draw_text_clipped(sh, fb, w, h, st, blk_txt,
                             tc.shield_btn.x + 20, tc.shield_btn.y + 18,
                             tc.shield_btn.x + tc.shield_btn.width - 4,
                             11, true, shield_on ? pal.btn_active_fg : pal.text_muted);

        ui_draw_toolbar_btn(sh, fb, w, h, st, &pal, tc.reader_btn, reader_on);
        ui_icon_book(sh, fb, w, h, st, tc.reader_btn.x + 7, tc.reader_btn.y + 7,
                     reader_on ? pal.btn_active_fg : pal.text_primary);

        ui_draw_toolbar_btn(sh, fb, w, h, st, &pal, tc.dark_btn, dark_on);
        ui_icon_moon(sh, fb, w, h, st, tc.dark_btn.x + 7, tc.dark_btn.y + 7,
                     dark_on ? pal.btn_active_fg : pal.text_primary);

        ui_draw_toolbar_btn(sh, fb, w, h, st, &pal, tc.addons_btn, false);
        ui_icon_puzzle(sh, fb, w, h, st, tc.addons_btn.x + 7, tc.addons_btn.y + 7, pal.text_primary);
    }

    ui_draw_toolbar_btn(sh, fb, w, h, st, &pal, tc.menu_btn, false);
    ui_icon_menu(sh, fb, w, h, st, tc.menu_btn.x + 8, tc.menu_btn.y + 7, pal.text_primary);

    /* 3. Optional Bookmarks Bar (24px) */
    if (sh->cfg.show_bookmarks_bar) {
        ArcrRect bm = m.bookmarks_bar_rect;
        ui_fill(sh, fb, w, h, st, bm, pal.toolbar_bg);
        ui_fill(sh, fb, w, h, st, (ArcrRect){bm.x, bm.y + bm.height - 1, bm.width, 1},
                pal.toolbar_divider);
        int32_t bx = bm.x + 10;
        for (size_t i = 0u; i < sh->bookmark_count; ++i) {
            if (bx + 40 >= bm.x + bm.width) break;
            bx = ui_draw_text_clipped(sh, fb, w, h, st, sh->bookmarks[i].title,
                                      bx, bm.y + 16, bm.x + bm.width - 10,
                                      12, false, pal.text_primary) + 18;
        }
    }

    /* 4. Optional Find-in-Page Bar (28px) */
    if (sh->show_find_bar && m.find_bar_rect.height > 0) {
        ArcrRect fbr = m.find_bar_rect;
        ui_fill(sh, fb, w, h, st, fbr, pal.toolbar_bg);
        ui_fill(sh, fb, w, h, st, (ArcrRect){fbr.x, fbr.y + fbr.height - 1, fbr.width, 1},
                pal.toolbar_divider);
        ui_draw_text_clipped(sh, fb, w, h, st, "검색:",
                             fbr.x + 12, fbr.y + 19, fbr.x + 60, 12, true, pal.text_primary);
        ui_box(sh, fb, w, h, st, tc.find_input, pal.omnibar_bg,
               (sh->focus == WS_FOCUS_FINDBAR) ? pal.omnibar_focus_border : pal.omnibar_border,
               4, pal.classic_3d_bevels);
        int32_t f_end_x = ui_draw_text_clipped(sh, fb, w, h, st, sh->find_buf,
                                               tc.find_input.x + 6, tc.find_input.y + 15,
                                               tc.find_input.x + tc.find_input.width - 6,
                                               12, false, pal.text_primary);
        if (sh->focus == WS_FOCUS_FINDBAR) {
            ui_fill(sh, fb, w, h, st,
                    (ArcrRect){f_end_x + 1, tc.find_input.y + 4, 2, 14}, pal.omnibar_focus_border);
        }

        char match_lbl[48];
        snprintf(match_lbl, sizeof(match_lbl), "%u/%u 일치",
                 (unsigned)sh->find_active_index, (unsigned)sh->find_total_matches);
        ui_draw_text_clipped(sh, fb, w, h, st, match_lbl,
                             tc.find_input.x + tc.find_input.width + 8, fbr.y + 19,
                             tc.find_prev_btn.x - 4, 11, false, pal.text_muted);

        ui_draw_toolbar_btn(sh, fb, w, h, st, &pal, tc.find_prev_btn, false);
        ui_draw_text_clipped(sh, fb, w, h, st, "▲",
                             tc.find_prev_btn.x + 6, tc.find_prev_btn.y + 16,
                             tc.find_prev_btn.x + 22, 11, true, pal.text_primary);

        ui_draw_toolbar_btn(sh, fb, w, h, st, &pal, tc.find_next_btn, false);
        ui_draw_text_clipped(sh, fb, w, h, st, "▼",
                             tc.find_next_btn.x + 6, tc.find_next_btn.y + 16,
                             tc.find_next_btn.x + 22, 11, true, pal.text_primary);

        ui_draw_toolbar_btn(sh, fb, w, h, st, &pal, tc.find_close_btn, false);
        ui_draw_text_clipped(sh, fb, w, h, st, "×",
                             tc.find_close_btn.x + 6, tc.find_close_btn.y + 16,
                             tc.find_close_btn.x + 20, 12, true, pal.text_primary);
    }

    /* 5. Web Content Viewport + Vertical Scrollbar */
    if (active_v) {
        uint32_t *vp_ptr = fb + (size_t)m.content_viewport.y * (size_t)st +
                           (size_t)m.content_viewport.x;
        arcr_webview_paint(active_v, vp_ptr,
                           m.content_viewport.width, m.content_viewport.height, st);
    }

    if (m.vscrollbar_rect.height > 0) {
        ui_fill(sh, fb, w, h, st, m.vscrollbar_rect, pal.scrollbar_track);
        if (pal.classic_3d_bevels) {
            ui_fill(sh, fb, w, h, st, m.vscrollbar_thumb_rect, pal.scrollbar_thumb);
            ui_win2k_bevel(sh, fb, w, h, st, m.vscrollbar_thumb_rect, false);
        } else {
            arcr_accel_fill_rounded_rect(sh->ui_accel, fb, w, h, st,
                                         m.vscrollbar_thumb_rect, 4, pal.scrollbar_thumb);
        }
    }

    /* 6. Optional Bottom Hardware Acceleration & DevTools Dock (150px, F12) */
    if (m.devtools_dock_rect.height > 0) {
        ArcrRect dk = m.devtools_dock_rect;
        ui_fill(sh, fb, w, h, st, dk, pal.dock_bg);
        ui_fill(sh, fb, w, h, st, (ArcrRect){dk.x, dk.y, dk.width, 2}, pal.dock_accent);

        ArcrEngineStats est;
        memset(&est, 0, sizeof(est));
        arcr_engine_get_stats(sh->engine, &est);

        int32_t half_w = (dk.width - 18) / 2;
        ArcrRect left_p  = {dk.x + 6, dk.y + 6, half_w, dk.height - 12};
        ArcrRect right_p = {dk.x + 12 + half_w, dk.y + 6, dk.width - half_w - 18, dk.height - 12};
        ui_box(sh, fb, w, h, st, left_p, pal.dock_panel_bg, pal.toolbar_divider, 4, pal.classic_3d_bevels);
        ui_box(sh, fb, w, h, st, right_p, pal.dock_panel_bg, pal.toolbar_divider, 4, pal.classic_3d_bevels);

        ui_draw_text_clipped(sh, fb, w, h, st,
                             "Waterscape Hardware Acceleration & Engine Telemetry (F12)",
                             left_p.x + 8, left_p.y + 16, left_p.x + left_p.width - 6,
                             12, true, pal.dock_accent);

        char line1[256], line2[256], line3[256], line4[256];
        snprintf(line1, sizeof(line1), "SIMD: %s | Threads: %u | GPU: %s",
                 arcr_cpu_simd_tier_name(est.detected_cpu_flags),
                 (unsigned)est.worker_thread_count,
                 arcr_engine_gpu_backend_summary(sh->engine));
        snprintf(line2, sizeof(line2), "64x64 Tiles: %u | Layout: %lu us | Paint: %lu us",
                 (unsigned)est.tiles_rendered_last_frame,
                 (unsigned long)est.last_layout_usec,
                 (unsigned long)est.last_paint_usec);
        snprintf(line3, sizeof(line3),
                 "Glyph Cache: %lu hits (%lu entries) | Style Share: %lu | Arena: %lu KiB",
                 (unsigned long)est.glyph_cache_hits,
                 (unsigned long)est.glyph_cache_entries,
                 (unsigned long)est.style_shared_hits,
                 (unsigned long)(est.arena_used_bytes / 1024u));

        WaterscapePluginHostContext hctx = {
            .detected_cpu_flags     = est.detected_cpu_flags,
            .active_simd_backend    = est.active_simd_level,
            .active_gpu_backend     = est.active_gpu_backend,
            .worker_threads         = est.worker_thread_count,
            .last_frame_render_usec = est.last_layout_usec + est.last_paint_usec,
            .last_frame_tiles       = est.tiles_rendered_last_frame,
            .engine_memory_bytes    = est.arena_used_bytes
        };
        char hud[192];
        hud[0] = '\0';
        if (sh->addons) ws_addons_format_plugin_hud(sh->addons, &hctx, hud, sizeof(hud));
        snprintf(line4, sizeof(line4), "Shield Blocked: %u | %s",
                 (unsigned)(sh->addons ? ws_addons_total_blocked(sh->addons) : 0u),
                 hud[0] ? hud : "Add-on HUD Ready");

        ui_draw_text_ex(sh, fb, w, h, st, line1, (size_t)-1,
                        left_p.x + 8, left_p.y + 38, left_p.x + left_p.width - 6,
                        11, false, true, pal.text_primary);
        ui_draw_text_ex(sh, fb, w, h, st, line2, (size_t)-1,
                        left_p.x + 8, left_p.y + 58, left_p.x + left_p.width - 6,
                        11, false, true, pal.text_primary);
        ui_draw_text_ex(sh, fb, w, h, st, line3, (size_t)-1,
                        left_p.x + 8, left_p.y + 78, left_p.x + left_p.width - 6,
                        11, false, true, pal.text_muted);
        ui_draw_text_ex(sh, fb, w, h, st, line4, (size_t)-1,
                        left_p.x + 8, left_p.y + 98, left_p.x + left_p.width - 6,
                        11, false, true, pal.dock_accent);

        ui_draw_text_clipped(sh, fb, w, h, st,
                             "ArcradientJS Console & DOM Inspector",
                             right_p.x + 8, right_p.y + 16, right_p.x + right_p.width - 6,
                             12, true, pal.dock_accent);

        char dom_line[256];
        snprintf(dom_line, sizeof(dom_line),
                 "DOM: %u nodes | Boxes: %u | Cmds: %u | JS Heap: %lu B",
                 (unsigned)est.dom_node_count,
                 (unsigned)est.layout_box_count,
                 (unsigned)est.display_cmd_count,
                 (unsigned long)est.js_heap_bytes);
        ui_draw_text_ex(sh, fb, w, h, st, dom_line, (size_t)-1,
                        right_p.x + 8, right_p.y + 38, right_p.x + right_p.width - 6,
                        11, false, true, pal.text_muted);

        const char *clog = active_v ? arcr_webview_get_console_log(active_v) : NULL;
        if (!clog || clog[0] == '\0') {
            ui_draw_text_ex(sh, fb, w, h, st, "> Console ready (0 errors).", (size_t)-1,
                            right_p.x + 8, right_p.y + 62, right_p.x + right_p.width - 6,
                            11, false, true, pal.text_primary);
        } else {
            const char *lp = clog;
            int32_t     cy = right_p.y + 60;
            while (*lp && cy < right_p.y + right_p.height - 6) {
                const char *nl = strchr(lp, '\n');
                size_t llen = nl ? (size_t)(nl - lp) : strlen(lp);
                ui_draw_text_ex(sh, fb, w, h, st, lp, llen,
                                right_p.x + 8, cy, right_p.x + right_p.width - 6,
                                11, false, true, pal.text_primary);
                cy += 18;
                if (!nl) break;
                lp = nl + 1;
            }
        }
    }

    /* 7. Ephemeral Bottom-Left Status Overlay */
    if (sh->status_overlay[0] != '\0') {
        ArcrRect sr = m.status_overlay_rect;
        ui_box(sh, fb, w, h, st, sr, pal.status_bg, pal.status_border, 4, pal.classic_3d_bevels);
        if (pal.classic_3d_bevels) ui_win2k_bevel(sh, fb, w, h, st, sr, true);
        ui_draw_text_clipped(sh, fb, w, h, st, sh->status_overlay,
                             sr.x + 8, sr.y + 14, sr.x + sr.width - 6,
                             11, false, pal.status_text);
    }

    return true;
}
