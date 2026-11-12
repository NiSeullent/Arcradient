/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * addons/ws_addons.h - WebExtensions (.wsx) & Native C Plugin Manager
 *
 * Supports:
 * - Packaged `.wsx` (PKZIP) and unpacked extension directories (`manifest.json`,
 *   `filter_rules.txt`, `inject.css`, `content.js`)
 * - Native Pure C11 dynamic plugins (`WaterscapePluginVTable` via `arcr_module_validate`)
 * - O(1) FNV-1a domain hash table + substring matcher for sub-microsecond ad/tracker blocking
 * - Per-extension runtime toggle, blocked-request telemetry, UserCSS & Content JS aggregation
 */
#ifndef WS_ADDONS_H
#define WS_ADDONS_H

#include "../include/waterscape_plugin.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WS_MAX_ADDONS 32

typedef enum WsAddonKind {
    WS_ADDON_EXTENSION_WSX = 0, /* .wsx archive or unpacked WebExtension directory */
    WS_ADDON_NATIVE_PLUGIN = 1  /* .dll / .so native C plugin */
} WsAddonKind;

typedef struct WsAddonInfo {
    char        id[64];
    char        name[96];
    char        version[32];
    char        description[192];
    WsAddonKind kind;
    bool        enabled;
    uint32_t    filter_rule_count;
    uint32_t    blocked_hits;
    bool        has_css;
    bool        has_js;
} WsAddonInfo;

typedef struct WsAddonManager WsAddonManager;

WsAddonManager *ws_addons_create(void);
void            ws_addons_destroy(WsAddonManager *mgr);

/* Loads bundled built-in extensions (Shield, Dark, Reader) even if external .wsx files are absent */
size_t ws_addons_load_builtin_defaults(WsAddonManager *mgr);

/* Loads a `.wsx` package from disk or memory, or an unpacked extension folder */
bool ws_addons_load_wsx_file(WsAddonManager *mgr, const char *wsx_path);
bool ws_addons_load_wsx_mem(WsAddonManager *mgr, const uint8_t *bytes, size_t len);
bool ws_addons_load_unpacked_dir(WsAddonManager *mgr, const char *dir_path);

/* Loads a native C plugin (`.dll` / `.so`) exporting `arcr_module_entry()` with `ARCR_MOD_PLUGIN` or `WaterscapePluginVTable` */
bool ws_addons_load_native_plugin(WsAddonManager *mgr, const char *module_dir, const char *mod_stem);

/* Scans a directory for `*.wsx` packages and known native plugins */
size_t ws_addons_scan_directory(WsAddonManager *mgr, const char *addons_dir);

/* Enumeration & State Control */
size_t ws_addons_count(const WsAddonManager *mgr);
bool   ws_addons_get_info(const WsAddonManager *mgr, size_t index, WsAddonInfo *out_info);
bool   ws_addons_find_by_id(const WsAddonManager *mgr, const char *addon_id, WsAddonInfo *out_info);
bool   ws_addons_set_enabled(WsAddonManager *mgr, const char *addon_id, bool enabled);
bool   ws_addons_toggle(WsAddonManager *mgr, const char *addon_id);

/* High-speed URL Filter Callback (compatible with `ArcrUrlFilterFn`: `bool fn(void *ud, const char *url, const char *page_host)`) */
bool     ws_addons_should_block_url(void *mgr_userdata, const char *url, const char *page_host);
uint32_t ws_addons_total_blocked(const WsAddonManager *mgr);
void     ws_addons_reset_blocked_counts(WsAddonManager *mgr);

/* Aggregates active UserCSS / Content JS across all enabled extensions & native plugins for `url` (caller frees `*out_buf` if non-NULL) */
bool ws_addons_collect_injected_css(WsAddonManager *mgr, const char *url, char **out_css, size_t *out_len);
bool ws_addons_collect_injected_js(WsAddonManager *mgr, const char *url, char **out_js, size_t *out_len);

/* Formats combined native plugin HUD telemetry string (e.g. Perf HUD badge) */
void ws_addons_format_plugin_hud(WsAddonManager *mgr, const WaterscapePluginHostContext *host,
                                 char *out_buf, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* WS_ADDONS_H */
