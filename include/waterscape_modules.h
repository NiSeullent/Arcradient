/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * include/waterscape_modules.h - Systematized Multi-Tier Module Catalog & Registry
 *
 * Defines the strict layered module taxonomy of Waterscape so every subsystem
 * has an explicit boundary, lifecycle, ABI contract, and dependency tier:
 *
 *   [Tier 0: Base]      arcr_base       (Arena, Atom, UTF-8/CJK, CPUID, Codec, ThreadPool)
 *   [Tier 1: Platform]  arcr_platform   (Win2000..Win11 Win32 / POSIX, Thread-Safe Glyph Cache)
 *   [Tier 2: Accel]     arcr_accel      (Scalar / SSE1+SWAR / SSE2 / AVX2 + D3D11/VK/GL/D3D9/DDraw7)
 *   [Tier 3: Web Core]  arcr_engine     (DOM, HTML5, O(1) CSS3, Flex/Grid/Table Layout, 64x64 Tile Paint)
 *   [Tier 4: Dynamic]   arcr_net | arcr_js_portable | arcr_media | arcr_devtools  (On-Demand .dll/.so)
 *   [Tier 5: Add-ons]   ws_addons + .wsx packages + ws_plugin_* native C plugins
 *   [Tier 6: Shell/UI]  arcr_shell + waterscape_win32 (D3D11/D3D9/GDI) + waterscape_x11 (XShm/X11)
 *   [Tier 7: Installer] waterscape_setup + waterscape_uninstall + Linux .deb / .run
 */
#ifndef WATERSCAPE_MODULES_H
#define WATERSCAPE_MODULES_H

#include "arcr_module.h"
#include "arcradient_embed.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum WaterscapeModuleTier {
    WS_TIER_0_BASE      = 0,
    WS_TIER_1_PLATFORM  = 1,
    WS_TIER_2_ACCEL     = 2,
    WS_TIER_3_WEBCORE   = 3,
    WS_TIER_4_ONDEMAND  = 4,
    WS_TIER_5_ADDON     = 5,
    WS_TIER_6_SHELL     = 6,
    WS_TIER_7_INSTALLER = 7
} WaterscapeModuleTier;

typedef enum WaterscapeDynamicSlot {
    WS_DYN_SLOT_NET      = 0, /* arcr_net (.dll / .so) */
    WS_DYN_SLOT_JS       = 1, /* arcr_js_portable (.dll / .so) */
    WS_DYN_SLOT_MEDIA    = 2, /* arcr_media (.dll / .so) */
    WS_DYN_SLOT_DEVTOOLS = 3, /* arcr_devtools (.dll / .so) */
    WS_DYN_SLOT_COUNT    = 4
} WaterscapeDynamicSlot;

typedef struct WaterscapeModuleStatusEntry {
    const char          *stem;
    const char          *role_summary;
    WaterscapeModuleTier tier;
    bool                 is_loaded;
    bool                 load_attempted;
    ArcrModuleStatus     last_status;
} WaterscapeModuleStatusEntry;

/* Canonical stems for Tier-4 On-Demand Engine Modules */
#define WS_MOD_STEM_NET      "arcr_net"
#define WS_MOD_STEM_JS       "arcr_js_portable"
#define WS_MOD_STEM_MEDIA    "arcr_media"
#define WS_MOD_STEM_DEVTOOLS "arcr_devtools"

/* Canonical entry symbol names (primary `arcr_module_query`, alias `arcr_module_entry`) */
#define WS_MOD_ENTRY_PRIMARY "arcr_module_query"
#define WS_MOD_ENTRY_ALIAS   "arcr_module_entry"

/* Queries the live status of Tier-4 On-Demand Engine Modules on an `ArcrEngine` instance */
size_t arcr_engine_query_modules(const ArcrEngine *engine,
                                 WaterscapeModuleStatusEntry *out_entries,
                                 size_t max_entries);

#ifdef __cplusplus
}
#endif

#endif /* WATERSCAPE_MODULES_H */
