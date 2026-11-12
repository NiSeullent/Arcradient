/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * addons/ws_addons.c - WebExtensions (.wsx) & Native C11 Plugin Manager
 *
 * Implements:
 * - Zero-dependency JSON parser for WebExtension `manifest.json`
 * - PKZIP `.wsx` archive loader (memory & file via `arcr_zip_extract_entry`) & unpacked dir loader
 * - Built-in embedded defaults (`waterscape-shield`, `waterscape-dark`, `waterscape-reader`)
 *   with seamless in-place override by external `.wsx` packages
 * - O(1) 512-bucket FNV-1a domain suffix hash table + substring & `@@` exception URL filter
 * - Verified Native C11 dynamic plugin host (`arcr_module_query` / `arcr_module_entry`)
 * - Active UserCSS & Content JS collector and Native Plugin HUD aggregator
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "ws_addons.h"
#include "../base/arcr_base.h"
#include "../base/arcr_codec.h"
#include "../include/waterscape_modules.h"
#include "../platform/arcr_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <wchar.h>
#else
#  include <dirent.h>
#endif

#define WS_DOMAIN_HASH_BUCKETS 512u
#define WS_DOMAIN_HASH_MASK    (WS_DOMAIN_HASH_BUCKETS - 1u)
#define WS_MAX_MANIFEST_PATHS  8u

typedef enum WsRuleKind {
    WS_RULE_DOMAIN    = 0,
    WS_RULE_SUBSTRING = 1
} WsRuleKind;

typedef struct WsFilterRule {
    WsRuleKind kind;
    bool       is_exception;
    bool       third_party_only;
    uint32_t   hash;
    size_t     len;
    char      *pattern;
} WsFilterRule;

typedef struct WsDomainBucketNode {
    const WsFilterRule        *rule;
    uint32_t                   addon_index;
    struct WsDomainBucketNode *next;
} WsDomainBucketNode;

typedef struct WsAddonSlot {
    WsAddonInfo                  info;
    char                        *css_text;
    size_t                       css_len;
    char                        *js_text;
    size_t                       js_len;
    WsFilterRule                *rules;
    size_t                       rule_count;

    /* Native C11 plugin state (`kind == WS_ADDON_NATIVE_PLUGIN`) */
    ArcrDynLib                  *plugin_lib;
    const WaterscapePluginVTable *plugin_vt;
    void                        *plugin_instance;
} WsAddonSlot;

struct WsAddonManager {
    WsAddonSlot         slots[WS_MAX_ADDONS];
    size_t              count;
    uint32_t            total_blocked;
    ArcrArena           index_arena;
    WsDomainBucketNode *allow_domain_buckets[WS_DOMAIN_HASH_BUCKETS];
    WsDomainBucketNode *block_domain_buckets[WS_DOMAIN_HASH_BUCKETS];
};

/* =========================================================================
 * 1. Embedded Built-in Extension Defaults (Standalone Portable Fallback)
 * ========================================================================= */
static const char kBuiltinShieldManifest[] =
    "{\n"
    "  \"manifest_version\": 2,\n"
    "  \"id\": \"waterscape-shield\",\n"
    "  \"name\": \"Waterscape Shield\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"description\": \"\xEC\x8B\xA4\xEC\x8B\x9C\xEA\xB0\x84 \xEA\xB4\x91\xEA\xB3\xA0\xC2\xB7"
    "\xEC\xB6\x94\xEC\xA0\x81\xEA\xB8\xB0\xC2\xB7\xEC\x9B\x90\xEA\xB2\xA9\xEB\xB6\x84\xEC\x84\x9D "
    "\xEC\xB0\xA8\xEB\x8B\xA8 \xEC\x97\x94\xEC\xA7\x84 (O(1) FNV-1a Ad, Tracker & Telemetry Blocker)\",\n"
    "  \"enabled_by_default\": true,\n"
    "  \"filter_rules\": \"filter_rules.txt\",\n"
    "  \"css\": \"inject.css\"\n"
    "}\n";

static const char kBuiltinShieldRules[] =
    "! Waterscape Shield Built-in Rules\n"
    "||doubleclick.net^\n"
    "||googlesyndication.com^\n"
    "||googleadservices.com^\n"
    "||adservice.google.com^\n"
    "||2mdn.net^\n"
    "||adnxs.com^\n"
    "||adsrvr.org^\n"
    "||criteo.com^\n"
    "||criteo.net^\n"
    "||rubiconproject.com^\n"
    "||pubmatic.com^\n"
    "||openx.net^\n"
    "||casalemedia.com^\n"
    "||indexww.com^\n"
    "||smartadserver.com^\n"
    "||adform.net^\n"
    "||advertising.com^\n"
    "||amazon-adsystem.com^\n"
    "||media.net^\n"
    "||yieldmo.com^\n"
    "||triplelift.com^\n"
    "||sharethrough.com^\n"
    "||sovrn.com^\n"
    "||teads.tv^\n"
    "||zedo.com^\n"
    "||popads.net^\n"
    "||popcash.net^\n"
    "||propellerads.com^\n"
    "||taboola.com^\n"
    "||outbrain.com^\n"
    "||revcontent.com^\n"
    "||mgid.com^\n"
    "||google-analytics.com^\n"
    "||googletagmanager.com^\n"
    "||googletagservices.com^\n"
    "||connect.facebook.net^\n"
    "||pixel.facebook.com^\n"
    "||an.facebook.com^\n"
    "||scorecardresearch.com^\n"
    "||quantserve.com^\n"
    "||hotjar.com^\n"
    "||mouseflow.com^\n"
    "||fullstory.com^\n"
    "||clarity.ms^\n"
    "||bat.bing.com^\n"
    "||ads-twitter.com^\n"
    "||analytics.tiktok.com^\n"
    "||mc.yandex.ru^\n"
    "||moatads.com^\n"
    "||doubleverify.com^\n"
    "||chartbeat.com^\n"
    "||mixpanel.com^\n"
    "||amplitude.com^\n"
    "||segment.io^\n"
    "||ads.example.com^\n"
    "||tracker.example.com^\n"
    "||telemetry.example.org^\n"
    "||malvertising.invalid^\n"
    "/ads/\n"
    "/pagead/\n"
    "/adserver/\n"
    "/banner_ad\n"
    "/tracker.js\n"
    "adbanner\n"
    "/adframe.\n"
    "/popunder.\n"
    "/sponsored_pixel\n"
    "@@||safe-allowlist.waterscape.local^\n";

static const char kBuiltinShieldCss[] =
    ".ad-banner, .adsbox, .advertisement, .sponsored-ad, .ad-container,\n"
    ".ad-slot, .dfp-ad, .google-ad, #google_ads, [id^=\"google_ads\"] {\n"
    "    display: none !important;\n"
    "}\n";

static const char kBuiltinDarkManifest[] =
    "{\n"
    "  \"manifest_version\": 2,\n"
    "  \"id\": \"waterscape-dark\",\n"
    "  \"name\": \"NightScape Dark Mode\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"description\": \"\xEC\x9B\xB9 \xED\x8E\x98\xEC\x9D\xB4\xEC\xA7\x80\xEC\x97\x90 \xEB\x88\x88\xEC\x9D\xB4 "
    "\xED\x8E\xB8\xEC\x95\x88\xED\x95\x9C \xEC\x8A\xAC\xEB\xA0\x88\xEC\x9D\xB4\xED\x8A\xB8 \xEB\x8B\xA4\xED\x81\xAC "
    "\xED\x8C\x94\xEB\xA0\x88\xED\x8A\xB8\xEB\xA5\xBC \xEC\xA0\x81\xEC\x9A\xA9\xED\x95\xA9\xEB\x8B\x88\xEB\x8B\xA4 "
    "(Adaptive Slate Dark Mode for Web Documents)\",\n"
    "  \"enabled_by_default\": false,\n"
    "  \"css\": \"inject.css\",\n"
    "  \"js\": \"content.js\"\n"
    "}\n";

static const char kBuiltinDarkCss[] =
    "html, body { background-color: #0F172A !important; color: #E2E8F0 !important; }\n"
    "main, article, section, header, footer { background-color: #0F172A; color: #E2E8F0; }\n"
    "h1, h2, h3, h4, h5, h6 { color: #F8FAFC !important; }\n"
    "a { color: #60A5FA !important; }\n"
    "pre, code, blockquote, table, th, td, input, textarea, select, button {\n"
    "    background-color: #1E293B !important;\n"
    "    color: #E2E8F0 !important;\n"
    "    border-color: #334155 !important;\n"
    "}\n";

static const char kBuiltinDarkJs[] =
    "(function () {\n"
    "    if (typeof document !== \"undefined\" && document.documentElement && document.documentElement.setAttribute) {\n"
    "        document.documentElement.setAttribute(\"data-waterscape-dark\", \"1\");\n"
    "    }\n"
    "    if (typeof console !== \"undefined\" && console.log) {\n"
    "        console.log(\"[NightScape] data-waterscape-dark=\\\"1\\\" active\");\n"
    "    }\n"
    "})();\n";

static const char kBuiltinReaderManifest[] =
    "{\n"
    "  \"manifest_version\": 2,\n"
    "  \"id\": \"waterscape-reader\",\n"
    "  \"name\": \"Waterscape Reader View\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"description\": \"\xEB\xB0\xA9\xED\x95\xB4 \xEC\x9A\x94\xEC\x86\x8C\xEB\xA5\xBC \xEC\xA0\x9C\xEA\xB1\xB0"
    "\xED\x95\x98\xEA\xB3\xA0 \xEB\xB3\xB8\xEB\xAC\xB8 \xED\x83\x80\xEC\x9D\xB4\xED\x8F\xAC\xEA\xB7\xB8\xEB\x9E\x98"
    "\xED\x94\xBC\xEB\xA5\xBC \xEC\xA0\x95\xEB\x8F\x88\xED\x95\x98\xEB\x8A\x94 \xEC\xA7\x91\xEC\xA4\x91 "
    "\xEC\x9D\xBD\xEA\xB8\xB0 \xEB\xAA\xA8\xEB\x93\x9C (Distraction-Free Article Reader)\",\n"
    "  \"enabled_by_default\": false,\n"
    "  \"css\": \"inject.css\",\n"
    "  \"js\": \"content.js\"\n"
    "}\n";

static const char kBuiltinReaderCss[] =
    "nav, aside, footer, .sidebar, .cookie-banner, .cookie-notice, .ad-banner, .advertisement {\n"
    "    display: none !important;\n"
    "}\n"
    "body {\n"
    "    max-width: 720px !important;\n"
    "    margin: 0 auto !important;\n"
    "    font-size: 18px !important;\n"
    "    line-height: 28px !important;\n"
    "    padding: 28px !important;\n"
    "    background-color: #FAF8F5;\n"
    "    color: #1E293B;\n"
    "}\n"
    "article, main, .article-body, .post-content {\n"
    "    max-width: 720px;\n"
    "    margin: 0 auto;\n"
    "    font-size: 18px;\n"
    "    line-height: 28px;\n"
    "}\n";

static const char kBuiltinReaderJs[] =
    "(function () {\n"
    "    if (typeof document !== \"undefined\" && document.documentElement && document.documentElement.setAttribute) {\n"
    "        document.documentElement.setAttribute(\"data-waterscape-reader\", \"1\");\n"
    "    }\n"
    "    if (typeof console !== \"undefined\" && console.log) {\n"
    "        console.log(\"[Waterscape Reader] Reader layout active (max-width: 720px)\");\n"
    "    }\n"
    "})();\n";

/* =========================================================================
 * 2. Low-Level Hashing, File I/O & Zero-Dependency JSON Manifest Parser
 * ========================================================================= */
static uint32_t fnv1a_lower_n(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0u; i < len; ++i) {
        uint8_t c = (uint8_t)arcr_ascii_tolower(s[i]);
        h ^= (uint32_t)c;
        h *= 16777619u;
    }
    return h;
}

static char *dup_bytes_terminated(const char *src, size_t len) {
    char *out = (char *)malloc(len + 1u);
    if (!out) return NULL;
    if (len > 0u && src) {
        memcpy(out, src, len);
    }
    out[len] = '\0';
    return out;
}

static FILE *ws_fopen_rb(const char *path) {
    if (!path || path[0] == '\0') return NULL;
#if defined(_WIN32)
    WCHAR wpath[1024];
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                                 wpath, (int)(sizeof(wpath) / sizeof(wpath[0])));
    if (wn <= 0) {
        wn = MultiByteToWideChar(CP_ACP, 0u, path, -1,
                                 wpath, (int)(sizeof(wpath) / sizeof(wpath[0])));
    }
    if (wn > 0) {
        for (int i = 0; wpath[i] != L'\0'; ++i) {
            if (wpath[i] == L'/') wpath[i] = L'\\';
        }
        return _wfopen(wpath, L"rb");
    }
    return fopen(path, "rb");
#else
    return fopen(path, "rb");
#endif
}

static bool read_file_bytes(const char *path, uint8_t **out_data, size_t *out_len) {
    if (!path || !out_data || !out_len) return false;
    *out_data = NULL;
    *out_len  = 0u;

    FILE *fp = ws_fopen_rb(path);
    if (!fp) return false;
    if (fseek(fp, 0L, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long sz = ftell(fp);
    if (sz < 0L || sz > 32L * 1024L * 1024L) {
        fclose(fp);
        return false;
    }
    if (fseek(fp, 0L, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }

    size_t ulen = (size_t)sz;
    uint8_t *buf = (uint8_t *)malloc(ulen + 1u);
    if (!buf) {
        fclose(fp);
        return false;
    }
    if (ulen > 0u && fread(buf, 1u, ulen, fp) != ulen) {
        free(buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    buf[ulen] = 0u;
    *out_data = buf;
    *out_len  = ulen;
    return true;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static size_t json_skip_ws(const char *js, size_t len, size_t pos) {
    while (pos < len) {
        char c = js[pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++pos;
        } else if (c == '/' && pos + 1u < len && js[pos + 1u] == '/') {
            pos += 2u;
            while (pos < len && js[pos] != '\n') ++pos;
        } else if (c == '/' && pos + 1u < len && js[pos + 1u] == '*') {
            pos += 2u;
            while (pos + 1u < len && !(js[pos] == '*' && js[pos + 1u] == '/')) ++pos;
            pos = (pos + 1u < len) ? (pos + 2u) : len;
        } else {
            break;
        }
    }
    return pos;
}

/* Parses a JSON quoted string starting at `js[*inout_pos] == '"'`. */
static bool json_parse_string(const char *js, size_t len, size_t *inout_pos,
                              char *out_buf, size_t out_cap) {
    size_t pos = *inout_pos;
    if (pos >= len || js[pos] != '"') return false;
    ++pos;

    size_t w = 0u;
    while (pos < len) {
        char c = js[pos++];
        if (c == '"') {
            if (out_buf && out_cap > 0u) out_buf[w] = '\0';
            *inout_pos = pos;
            return true;
        }
        if (c == '\\') {
            if (pos >= len) return false;
            char esc = js[pos++];
            switch (esc) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'u': {
                    if (pos + 4u > len) return false;
                    int h0 = hex_val(js[pos + 0u]);
                    int h1 = hex_val(js[pos + 1u]);
                    int h2 = hex_val(js[pos + 2u]);
                    int h3 = hex_val(js[pos + 3u]);
                    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return false;
                    uint32_t cp = ((uint32_t)h0 << 12) | ((uint32_t)h1 << 8) |
                                  ((uint32_t)h2 << 4)  | (uint32_t)h3;
                    pos += 4u;
                    if (cp >= 0xD800u && cp <= 0xDBFFu && pos + 6u <= len &&
                        js[pos] == '\\' && js[pos + 1u] == 'u') {
                        int l0 = hex_val(js[pos + 2u]);
                        int l1 = hex_val(js[pos + 3u]);
                        int l2 = hex_val(js[pos + 4u]);
                        int l3 = hex_val(js[pos + 5u]);
                        if (l0 >= 0 && l1 >= 0 && l2 >= 0 && l3 >= 0) {
                            uint32_t low = ((uint32_t)l0 << 12) | ((uint32_t)l1 << 8) |
                                           ((uint32_t)l2 << 4)  | (uint32_t)l3;
                            if (low >= 0xDC00u && low <= 0xDFFFu) {
                                cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                                pos += 6u;
                            }
                        }
                    }
                    char u8[4];
                    size_t u8_n = arcr_utf8_encode_one(cp, u8);
                    for (size_t k = 0u; k < u8_n; ++k) {
                        if (out_buf && w + 1u < out_cap) out_buf[w++] = u8[k];
                    }
                    continue;
                }
                default:
                    c = esc;
                    break;
            }
        }
        if (out_buf && w + 1u < out_cap) {
            out_buf[w++] = c;
        }
    }
    return false;
}

static bool json_skip_value(const char *js, size_t len, size_t *inout_pos) {
    size_t pos = json_skip_ws(js, len, *inout_pos);
    if (pos >= len) return false;

    if (js[pos] == '"') {
        *inout_pos = pos;
        return json_parse_string(js, len, inout_pos, NULL, 0u);
    }
    if (js[pos] == '{' || js[pos] == '[') {
        char open_ch  = js[pos];
        char close_ch = (open_ch == '{') ? '}' : ']';
        int  depth    = 1;
        ++pos;
        while (pos < len && depth > 0) {
            pos = json_skip_ws(js, len, pos);
            if (pos >= len) return false;
            if (js[pos] == '"') {
                if (!json_parse_string(js, len, &pos, NULL, 0u)) return false;
                continue;
            }
            if (js[pos] == open_ch) ++depth;
            else if (js[pos] == close_ch) --depth;
            ++pos;
        }
        *inout_pos = pos;
        return (depth == 0);
    }
    while (pos < len) {
        char c = js[pos];
        if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            break;
        }
        ++pos;
    }
    *inout_pos = pos;
    return true;
}

typedef struct WsParsedManifest {
    char   id[64];
    char   name[96];
    char   version[32];
    char   description[192];
    bool   enabled_by_default;
    char   filter_rules_file[128];
    char   css_files[WS_MAX_MANIFEST_PATHS][128];
    size_t css_file_count;
    char   js_files[WS_MAX_MANIFEST_PATHS][128];
    size_t js_file_count;
} WsParsedManifest;

static bool json_parse_string_or_array(const char *js, size_t len, size_t *inout_pos,
                                       char out_list[WS_MAX_MANIFEST_PATHS][128],
                                       size_t *inout_count) {
    size_t pos = json_skip_ws(js, len, *inout_pos);
    if (pos >= len) return false;

    if (js[pos] == '"') {
        char tmp[128];
        if (!json_parse_string(js, len, &pos, tmp, sizeof(tmp))) return false;
        if (tmp[0] != '\0' && *inout_count < WS_MAX_MANIFEST_PATHS) {
            arcr_str_copy(out_list[*inout_count], 128u, tmp);
            (*inout_count)++;
        }
        *inout_pos = pos;
        return true;
    }
    if (js[pos] == '[') {
        ++pos;
        while (pos < len) {
            pos = json_skip_ws(js, len, pos);
            if (pos >= len) return false;
            if (js[pos] == ']') {
                ++pos;
                *inout_pos = pos;
                return true;
            }
            if (js[pos] == '"') {
                char tmp[128];
                if (!json_parse_string(js, len, &pos, tmp, sizeof(tmp))) return false;
                if (tmp[0] != '\0' && *inout_count < WS_MAX_MANIFEST_PATHS) {
                    arcr_str_copy(out_list[*inout_count], 128u, tmp);
                    (*inout_count)++;
                }
            } else {
                if (!json_skip_value(js, len, &pos)) return false;
            }
            pos = json_skip_ws(js, len, pos);
            if (pos < len && js[pos] == ',') ++pos;
        }
        return false;
    }
    return json_skip_value(js, len, inout_pos);
}

static bool parse_manifest_json(const char *json_text, size_t json_len, WsParsedManifest *out_m) {
    if (!json_text || json_len == 0u || !out_m) return false;
    memset(out_m, 0, sizeof(*out_m));
    arcr_str_copy(out_m->version, sizeof(out_m->version), "1.0.0");

    /* Strip UTF-8 BOM if present */
    size_t pos = 0u;
    if (json_len >= 3u &&
        (uint8_t)json_text[0] == 0xEFu &&
        (uint8_t)json_text[1] == 0xBBu &&
        (uint8_t)json_text[2] == 0xBFu) {
        pos = 3u;
    }

    pos = json_skip_ws(json_text, json_len, pos);
    if (pos >= json_len || json_text[pos] != '{') return false;
    ++pos;

    while (pos < json_len) {
        pos = json_skip_ws(json_text, json_len, pos);
        if (pos >= json_len) return false;
        if (json_text[pos] == '}') {
            ++pos;
            break;
        }

        char key[64];
        if (!json_parse_string(json_text, json_len, &pos, key, sizeof(key))) return false;
        pos = json_skip_ws(json_text, json_len, pos);
        if (pos >= json_len || json_text[pos] != ':') return false;
        ++pos;
        pos = json_skip_ws(json_text, json_len, pos);
        if (pos >= json_len) return false;

        if (strcmp(key, "id") == 0) {
            if (!json_parse_string(json_text, json_len, &pos, out_m->id, sizeof(out_m->id))) return false;
        } else if (strcmp(key, "name") == 0) {
            if (!json_parse_string(json_text, json_len, &pos, out_m->name, sizeof(out_m->name))) return false;
        } else if (strcmp(key, "version") == 0) {
            if (!json_parse_string(json_text, json_len, &pos, out_m->version, sizeof(out_m->version))) return false;
        } else if (strcmp(key, "description") == 0) {
            if (!json_parse_string(json_text, json_len, &pos, out_m->description, sizeof(out_m->description))) {
                return false;
            }
        } else if (strcmp(key, "enabled_by_default") == 0 || strcmp(key, "enabled") == 0) {
            if (pos + 4u <= json_len && memcmp(json_text + pos, "true", 4u) == 0) {
                out_m->enabled_by_default = true;
                pos += 4u;
            } else if (pos + 5u <= json_len && memcmp(json_text + pos, "false", 5u) == 0) {
                out_m->enabled_by_default = false;
                pos += 5u;
            } else {
                if (!json_skip_value(json_text, json_len, &pos)) return false;
            }
        } else if (strcmp(key, "filter_rules") == 0) {
            if (json_text[pos] == '"') {
                if (!json_parse_string(json_text, json_len, &pos,
                                       out_m->filter_rules_file, sizeof(out_m->filter_rules_file))) {
                    return false;
                }
            } else {
                if (!json_skip_value(json_text, json_len, &pos)) return false;
            }
        } else if (strcmp(key, "css") == 0) {
            if (!json_parse_string_or_array(json_text, json_len, &pos,
                                            out_m->css_files, &out_m->css_file_count)) {
                return false;
            }
        } else if (strcmp(key, "js") == 0) {
            if (!json_parse_string_or_array(json_text, json_len, &pos,
                                            out_m->js_files, &out_m->js_file_count)) {
                return false;
            }
        } else {
            if (!json_skip_value(json_text, json_len, &pos)) return false;
        }

        pos = json_skip_ws(json_text, json_len, pos);
        if (pos < json_len && json_text[pos] == ',') ++pos;
    }

    if (out_m->id[0] == '\0') return false;
    if (out_m->name[0] == '\0') {
        arcr_str_copy(out_m->name, sizeof(out_m->name), out_m->id);
    }
    return true;
}

/* Removes any leading `./` or `/` and rejects `..` path traversal in manifest asset paths */
static const char *sanitize_rel_asset_path(const char *rel) {
    if (!rel) return NULL;
    while (rel[0] == '.' && (rel[1] == '/' || rel[1] == '\\')) rel += 2;
    while (rel[0] == '/' || rel[0] == '\\') ++rel;
    if (rel[0] == '\0' || strstr(rel, "..") != NULL) return NULL;
    return rel;
}

/* =========================================================================
 * 3. Filter Rule Compiler & O(1) 512-Bucket FNV-1a Domain Hash Table
 * ========================================================================= */
static bool is_valid_domain_token(const char *s, size_t len) {
    if (len == 0u || s[0] == '.' || s[len - 1u] == '.') return false;
    bool has_dot = false;
    for (size_t i = 0u; i < len; ++i) {
        char c = s[i];
        if (c == '.') {
            has_dot = true;
            if (i + 1u < len && s[i + 1u] == '.') return false;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return has_dot;
}

static size_t parse_filter_rules_text(const char *text, size_t text_len, WsFilterRule **out_rules) {
    *out_rules = NULL;
    if (!text || text_len == 0u) return 0u;

    /* Count upper bound of non-comment lines */
    size_t cap = 0u;
    size_t pos = 0u;
    while (pos < text_len) {
        size_t eol = pos;
        while (eol < text_len && text[eol] != '\n' && text[eol] != '\r') ++eol;
        const char *line = text + pos;
        size_t llen = eol - pos;
        arcr_str_trim_view(&line, &llen);
        if (llen > 0u && line[0] != '!' && line[0] != '#' && line[0] != '[') {
            /* Skip cosmetic `##` element-hiding lines if present in EasyList files */
            bool is_elem_hide = false;
            for (size_t k = 0u; k + 1u < llen; ++k) {
                if ((line[k] == '#' && line[k + 1u] == '#') ||
                    (line[k] == '#' && line[k + 1u] == '?')) {
                    is_elem_hide = true;
                    break;
                }
            }
            if (!is_elem_hide) ++cap;
        }
        pos = eol;
        while (pos < text_len && (text[pos] == '\n' || text[pos] == '\r')) ++pos;
    }

    if (cap == 0u) return 0u;
    WsFilterRule *rules = (WsFilterRule *)calloc(cap, sizeof(WsFilterRule));
    if (!rules) return 0u;

    size_t count = 0u;
    pos = 0u;
    while (pos < text_len && count < cap) {
        size_t eol = pos;
        while (eol < text_len && text[eol] != '\n' && text[eol] != '\r') ++eol;
        const char *line = text + pos;
        size_t llen = eol - pos;
        pos = eol;
        while (pos < text_len && (text[pos] == '\n' || text[pos] == '\r')) ++pos;

        arcr_str_trim_view(&line, &llen);
        if (llen == 0u || line[0] == '!' || line[0] == '#' || line[0] == '[') continue;

        bool is_elem_hide = false;
        for (size_t k = 0u; k + 1u < llen; ++k) {
            if ((line[k] == '#' && line[k + 1u] == '#') ||
                (line[k] == '#' && line[k + 1u] == '?')) {
                is_elem_hide = true;
                break;
            }
        }
        if (is_elem_hide) continue;

        bool is_exc = false;
        if (llen >= 2u && line[0] == '@' && line[1] == '@') {
            is_exc = true;
            line += 2u;
            llen -= 2u;
        }

        /* Inspect & strip optional EasyList `$options` suffix */
        bool third_party_only = false;
        for (size_t k = 0u; k < llen; ++k) {
            if (line[k] == '$') {
                const char *opts = line + k + 1u;
                size_t olen = llen - k - 1u;
                if (olen >= 11u) {
                    for (size_t j = 0u; j + 11u <= olen; ++j) {
                        if (arcr_str_eq_nocase_n(opts + j, 11u, "third-party", 11u) &&
                            (j == 0u || opts[j - 1u] != '~')) {
                            third_party_only = true;
                            break;
                        }
                    }
                }
                llen = k;
                break;
            }
        }

        arcr_str_trim_view(&line, &llen);
        if (llen == 0u) continue;

        if (llen > 2u && line[0] == '|' && line[1] == '|') {
            const char *dom = line + 2u;
            size_t dlen = llen - 2u;
            /* Strip trailing separator `^` or `/` if at the end of the host token */
            while (dlen > 0u && (dom[dlen - 1u] == '^' || dom[dlen - 1u] == '/')) {
                --dlen;
            }
            if (is_valid_domain_token(dom, dlen)) {
                char *low = (char *)malloc(dlen + 1u);
                if (!low) continue;
                for (size_t k = 0u; k < dlen; ++k) {
                    low[k] = arcr_ascii_tolower(dom[k]);
                }
                low[dlen] = '\0';

                rules[count].kind             = WS_RULE_DOMAIN;
                rules[count].is_exception     = is_exc;
                rules[count].third_party_only = third_party_only;
                rules[count].hash             = fnv1a_lower_n(low, dlen);
                rules[count].len              = dlen;
                rules[count].pattern          = low;
                ++count;
                continue;
            }
            /* Fallback: `||domain.com^/path` or `||domain.com/path` -> lowered substring */
            line = dom;
            llen = dlen;
        }

        /* Substring rule: strip leading/trailing `*` wildcards & `|` anchors */
        while (llen > 0u && (line[0] == '*' || line[0] == '|')) {
            ++line;
            --llen;
        }
        while (llen > 0u && (line[llen - 1u] == '*' || line[llen - 1u] == '^')) {
            --llen;
        }
        if (llen == 0u) continue;

        char *pat = (char *)malloc(llen + 1u);
        if (!pat) continue;
        for (size_t k = 0u; k < llen; ++k) {
            pat[k] = (line[k] == '^') ? '/' : arcr_ascii_tolower(line[k]);
        }
        pat[llen] = '\0';

        rules[count].kind             = WS_RULE_SUBSTRING;
        rules[count].is_exception     = is_exc;
        rules[count].third_party_only = third_party_only;
        rules[count].hash             = 0u;
        rules[count].len              = llen;
        rules[count].pattern          = pat;
        ++count;
    }

    if (count == 0u) {
        free(rules);
        return 0u;
    }
    *out_rules = rules;
    return count;
}

static void rebuild_domain_hash_index(WsAddonManager *mgr) {
    if (!mgr) return;
    arcr_arena_reset(&mgr->index_arena);
    memset(mgr->allow_domain_buckets, 0, sizeof(mgr->allow_domain_buckets));
    memset(mgr->block_domain_buckets, 0, sizeof(mgr->block_domain_buckets));

    for (size_t s = 0u; s < mgr->count; ++s) {
        const WsAddonSlot *slot = &mgr->slots[s];
        for (size_t r = 0u; r < slot->rule_count; ++r) {
            const WsFilterRule *rule = &slot->rules[r];
            if (rule->kind != WS_RULE_DOMAIN) continue;

            WsDomainBucketNode *node = (WsDomainBucketNode *)arcr_arena_alloc(
                &mgr->index_arena, sizeof(WsDomainBucketNode), sizeof(void *));
            if (!node) return;
            node->rule        = rule;
            node->addon_index = (uint32_t)s;

            size_t bucket = (size_t)(rule->hash & WS_DOMAIN_HASH_MASK);
            if (rule->is_exception) {
                node->next = mgr->allow_domain_buckets[bucket];
                mgr->allow_domain_buckets[bucket] = node;
            } else {
                node->next = mgr->block_domain_buckets[bucket];
                mgr->block_domain_buckets[bucket] = node;
            }
        }
    }
}

static void clear_addon_slot(WsAddonSlot *slot) {
    if (!slot) return;
    free(slot->css_text);
    free(slot->js_text);
    if (slot->rules) {
        for (size_t i = 0u; i < slot->rule_count; ++i) {
            free(slot->rules[i].pattern);
        }
        free(slot->rules);
    }
    if (slot->plugin_vt && slot->plugin_vt->on_shutdown) {
        slot->plugin_vt->on_shutdown(slot->plugin_instance);
    }
    if (slot->plugin_lib) {
        arcr_platform_dlclose(slot->plugin_lib);
    }
    memset(slot, 0, sizeof(*slot));
}

static WsAddonSlot *acquire_slot_for_id(WsAddonManager *mgr, const char *addon_id) {
    if (!mgr || !addon_id || addon_id[0] == '\0') return NULL;
    for (size_t i = 0u; i < mgr->count; ++i) {
        if (strcmp(mgr->slots[i].info.id, addon_id) == 0) {
            clear_addon_slot(&mgr->slots[i]);
            return &mgr->slots[i];
        }
    }
    if (mgr->count >= WS_MAX_ADDONS) return NULL;
    WsAddonSlot *slot = &mgr->slots[mgr->count++];
    memset(slot, 0, sizeof(*slot));
    return slot;
}

static bool install_parsed_extension(WsAddonManager *mgr,
                                     const WsParsedManifest *m,
                                     const char *rules_str, size_t rules_len,
                                     char *css_owned, size_t css_len,
                                     char *js_owned, size_t js_len) {
    WsFilterRule *parsed_rules = NULL;
    size_t        rule_cnt     = 0u;
    if (rules_str && rules_len > 0u) {
        rule_cnt = parse_filter_rules_text(rules_str, rules_len, &parsed_rules);
    }

    WsAddonSlot *slot = acquire_slot_for_id(mgr, m->id);
    if (!slot) {
        free(css_owned);
        free(js_owned);
        if (parsed_rules) {
            for (size_t i = 0u; i < rule_cnt; ++i) free(parsed_rules[i].pattern);
            free(parsed_rules);
        }
        return false;
    }

    arcr_str_copy(slot->info.id, sizeof(slot->info.id), m->id);
    arcr_str_copy(slot->info.name, sizeof(slot->info.name), m->name);
    arcr_str_copy(slot->info.version, sizeof(slot->info.version), m->version);
    arcr_str_copy(slot->info.description, sizeof(slot->info.description), m->description);
    slot->info.kind              = WS_ADDON_EXTENSION_WSX;
    slot->info.enabled           = m->enabled_by_default;
    slot->info.filter_rule_count = (uint32_t)rule_cnt;
    slot->info.blocked_hits      = 0u;
    slot->info.has_css           = (css_owned != NULL && css_len > 0u);
    slot->info.has_js            = (js_owned != NULL && js_len > 0u);

    slot->css_text   = css_owned;
    slot->css_len    = css_len;
    slot->js_text    = js_owned;
    slot->js_len     = js_len;
    slot->rules      = parsed_rules;
    slot->rule_count = rule_cnt;

    rebuild_domain_hash_index(mgr);
    return true;
}

/* =========================================================================
 * 4. Manager Lifecycle, Builtin Defaults, .wsx & Unpacked Directory Loaders
 * ========================================================================= */
WsAddonManager *ws_addons_create(void) {
    WsAddonManager *mgr = (WsAddonManager *)calloc(1u, sizeof(WsAddonManager));
    if (!mgr) return NULL;
    arcr_arena_init(&mgr->index_arena, 8192u);
    return mgr;
}

void ws_addons_destroy(WsAddonManager *mgr) {
    if (!mgr) return;
    for (size_t i = 0u; i < mgr->count; ++i) {
        clear_addon_slot(&mgr->slots[i]);
    }
    arcr_arena_destroy(&mgr->index_arena);
    free(mgr);
}

static bool load_single_builtin(WsAddonManager *mgr,
                                const char *manifest_txt,
                                const char *rules_txt,
                                const char *css_txt,
                                const char *js_txt) {
    WsParsedManifest m;
    if (!parse_manifest_json(manifest_txt, strlen(manifest_txt), &m)) return false;

    char  *css_copy = NULL;
    size_t css_len  = 0u;
    if (css_txt && css_txt[0] != '\0') {
        css_len  = strlen(css_txt);
        css_copy = dup_bytes_terminated(css_txt, css_len);
        if (!css_copy) return false;
    }

    char  *js_copy = NULL;
    size_t js_len  = 0u;
    if (js_txt && js_txt[0] != '\0') {
        js_len  = strlen(js_txt);
        js_copy = dup_bytes_terminated(js_txt, js_len);
        if (!js_copy) {
            free(css_copy);
            return false;
        }
    }

    size_t rlen = rules_txt ? strlen(rules_txt) : 0u;
    return install_parsed_extension(mgr, &m, rules_txt, rlen, css_copy, css_len, js_copy, js_len);
}

size_t ws_addons_load_builtin_defaults(WsAddonManager *mgr) {
    if (!mgr) return 0u;
    size_t loaded = 0u;
    if (load_single_builtin(mgr, kBuiltinShieldManifest, kBuiltinShieldRules, kBuiltinShieldCss, NULL)) {
        ++loaded;
    }
    if (load_single_builtin(mgr, kBuiltinDarkManifest, NULL, kBuiltinDarkCss, kBuiltinDarkJs)) {
        ++loaded;
    }
    if (load_single_builtin(mgr, kBuiltinReaderManifest, NULL, kBuiltinReaderCss, kBuiltinReaderJs)) {
        ++loaded;
    }
    return loaded;
}

static bool append_chunk(char **inout_buf, size_t *inout_len,
                         const uint8_t *chunk, size_t chunk_len, char sep) {
    if (!chunk || chunk_len == 0u) return true;
    size_t extra_sep = (*inout_len > 0u && sep != '\0') ? 1u : 0u;
    char *next = (char *)realloc(*inout_buf, *inout_len + extra_sep + chunk_len + 1u);
    if (!next) return false;
    if (extra_sep > 0u) {
        next[*inout_len] = sep;
        (*inout_len)++;
    }
    memcpy(next + *inout_len, chunk, chunk_len);
    *inout_len += chunk_len;
    next[*inout_len] = '\0';
    *inout_buf = next;
    return true;
}

bool ws_addons_load_wsx_mem(WsAddonManager *mgr, const uint8_t *bytes, size_t len) {
    if (!mgr || !bytes || len < 22u) return false;

    uint8_t *manifest_raw = NULL;
    size_t   manifest_len = 0u;
    if (!arcr_zip_extract_entry(bytes, len, "manifest.json", &manifest_raw, &manifest_len)) {
        return false;
    }

    WsParsedManifest m;
    bool ok_manifest = parse_manifest_json((const char *)manifest_raw, manifest_len, &m);
    free(manifest_raw);
    if (!ok_manifest) return false;

    /* Extract filter rules if configured (or fallback `filter_rules.txt`) */
    uint8_t *rules_raw = NULL;
    size_t   rules_len = 0u;
    const char *rules_entry = sanitize_rel_asset_path(
        m.filter_rules_file[0] != '\0' ? m.filter_rules_file : "filter_rules.txt");
    if (rules_entry) {
        (void)arcr_zip_extract_entry(bytes, len, rules_entry, &rules_raw, &rules_len);
    }

    /* Extract & concatenate CSS files */
    char  *css_buf = NULL;
    size_t css_len = 0u;
    if (m.css_file_count > 0u) {
        for (size_t i = 0u; i < m.css_file_count; ++i) {
            const char *rel = sanitize_rel_asset_path(m.css_files[i]);
            uint8_t *part = NULL;
            size_t   plen = 0u;
            if (rel && arcr_zip_extract_entry(bytes, len, rel, &part, &plen)) {
                (void)append_chunk(&css_buf, &css_len, part, plen, '\n');
                free(part);
            }
        }
    } else {
        uint8_t *part = NULL;
        size_t   plen = 0u;
        if (arcr_zip_extract_entry(bytes, len, "inject.css", &part, &plen)) {
            (void)append_chunk(&css_buf, &css_len, part, plen, '\n');
            free(part);
        }
    }

    /* Extract & concatenate JS files */
    char  *js_buf = NULL;
    size_t js_len = 0u;
    if (m.js_file_count > 0u) {
        for (size_t i = 0u; i < m.js_file_count; ++i) {
            const char *rel = sanitize_rel_asset_path(m.js_files[i]);
            uint8_t *part = NULL;
            size_t   plen = 0u;
            if (rel && arcr_zip_extract_entry(bytes, len, rel, &part, &plen)) {
                (void)append_chunk(&js_buf, &js_len, part, plen, '\n');
                free(part);
            }
        }
    } else {
        uint8_t *part = NULL;
        size_t   plen = 0u;
        if (arcr_zip_extract_entry(bytes, len, "content.js", &part, &plen)) {
            (void)append_chunk(&js_buf, &js_len, part, plen, '\n');
            free(part);
        }
    }

    bool ok = install_parsed_extension(mgr, &m, (const char *)rules_raw, rules_len,
                                       css_buf, css_len, js_buf, js_len);
    free(rules_raw);
    return ok;
}

bool ws_addons_load_wsx_file(WsAddonManager *mgr, const char *wsx_path) {
    if (!mgr || !wsx_path) return false;
    uint8_t *bytes = NULL;
    size_t   len   = 0u;
    if (!read_file_bytes(wsx_path, &bytes, &len)) return false;
    bool ok = ws_addons_load_wsx_mem(mgr, bytes, len);
    free(bytes);
    return ok;
}

static void join_dir_path(char *out, size_t cap, const char *dir, const char *rel) {
    size_t dlen = strlen(dir);
    while (dlen > 0u && (dir[dlen - 1u] == '/' || dir[dlen - 1u] == '\\')) --dlen;
#if defined(_WIN32)
    snprintf(out, cap, "%.*s\\%s", (int)dlen, dir, rel);
#else
    snprintf(out, cap, "%.*s/%s", (int)dlen, dir, rel);
#endif
}

bool ws_addons_load_unpacked_dir(WsAddonManager *mgr, const char *dir_path) {
    if (!mgr || !dir_path || dir_path[0] == '\0') return false;

    char manifest_path[1024];
    join_dir_path(manifest_path, sizeof(manifest_path), dir_path, "manifest.json");

    uint8_t *manifest_raw = NULL;
    size_t   manifest_len = 0u;
    if (!read_file_bytes(manifest_path, &manifest_raw, &manifest_len)) return false;

    WsParsedManifest m;
    bool ok_manifest = parse_manifest_json((const char *)manifest_raw, manifest_len, &m);
    free(manifest_raw);
    if (!ok_manifest) return false;

    uint8_t *rules_raw = NULL;
    size_t   rules_len = 0u;
    const char *rules_rel = sanitize_rel_asset_path(
        m.filter_rules_file[0] != '\0' ? m.filter_rules_file : "filter_rules.txt");
    if (rules_rel) {
        char rules_path[1024];
        join_dir_path(rules_path, sizeof(rules_path), dir_path, rules_rel);
        (void)read_file_bytes(rules_path, &rules_raw, &rules_len);
    }

    char  *css_buf = NULL;
    size_t css_len = 0u;
    if (m.css_file_count > 0u) {
        for (size_t i = 0u; i < m.css_file_count; ++i) {
            const char *rel = sanitize_rel_asset_path(m.css_files[i]);
            if (!rel) continue;
            char p[1024];
            join_dir_path(p, sizeof(p), dir_path, rel);
            uint8_t *part = NULL;
            size_t   plen = 0u;
            if (read_file_bytes(p, &part, &plen)) {
                (void)append_chunk(&css_buf, &css_len, part, plen, '\n');
                free(part);
            }
        }
    } else {
        char p[1024];
        join_dir_path(p, sizeof(p), dir_path, "inject.css");
        uint8_t *part = NULL;
        size_t   plen = 0u;
        if (read_file_bytes(p, &part, &plen)) {
            (void)append_chunk(&css_buf, &css_len, part, plen, '\n');
            free(part);
        }
    }

    char  *js_buf = NULL;
    size_t js_len = 0u;
    if (m.js_file_count > 0u) {
        for (size_t i = 0u; i < m.js_file_count; ++i) {
            const char *rel = sanitize_rel_asset_path(m.js_files[i]);
            if (!rel) continue;
            char p[1024];
            join_dir_path(p, sizeof(p), dir_path, rel);
            uint8_t *part = NULL;
            size_t   plen = 0u;
            if (read_file_bytes(p, &part, &plen)) {
                (void)append_chunk(&js_buf, &js_len, part, plen, '\n');
                free(part);
            }
        }
    } else {
        char p[1024];
        join_dir_path(p, sizeof(p), dir_path, "content.js");
        uint8_t *part = NULL;
        size_t   plen = 0u;
        if (read_file_bytes(p, &part, &plen)) {
            (void)append_chunk(&js_buf, &js_len, part, plen, '\n');
            free(part);
        }
    }

    bool ok = install_parsed_extension(mgr, &m, (const char *)rules_raw, rules_len,
                                       css_buf, css_len, js_buf, js_len);
    free(rules_raw);
    return ok;
}

/* =========================================================================
 * 5. Verified Native C11 Dynamic Plugin Host (`WaterscapePluginVTable`)
 * ========================================================================= */
bool ws_addons_load_native_plugin(WsAddonManager *mgr, const char *module_dir, const char *mod_stem) {
    if (!mgr || !mod_stem || mod_stem[0] == '\0') return false;

    const char *dir = (module_dir && module_dir[0] != '\0') ? module_dir : ".";
    char mod_path[1024];
    arcr_platform_format_module_path(mod_path, sizeof(mod_path), dir, mod_stem);

    ArcrDynLib *lib = arcr_platform_dlopen(mod_path);
    if (!lib) {
        /* Fallback for unprefixed plugin filenames (e.g. `ws_plugin_perf_hud.so` / `libws_plugin_*.dll`) */
        char alt_path[1024];
#if defined(_WIN32)
        snprintf(alt_path, sizeof(alt_path), "%s\\lib%s.dll", dir, mod_stem);
#elif defined(__APPLE__)
        snprintf(alt_path, sizeof(alt_path), "%s/%s.dylib", dir, mod_stem);
#else
        snprintf(alt_path, sizeof(alt_path), "%s/%s.so", dir, mod_stem);
#endif
        lib = arcr_platform_dlopen(alt_path);
        if (!lib) return false;
    }

    /* Resolve primary `arcr_module_query` first, falling back to `arcr_module_entry` */
    void *sym = arcr_platform_dlsym(lib, WS_MOD_ENTRY_PRIMARY);
    if (!sym) {
        sym = arcr_platform_dlsym(lib, WS_MOD_ENTRY_ALIAS);
    }
    ArcrModuleQueryFn query_fn = NULL;
    memcpy(&query_fn, &sym, sizeof(query_fn));
    if (!query_fn) {
        arcr_platform_dlclose(lib);
        return false;
    }

    const ArcrModuleDescriptor *desc = query_fn();
    ArcrModuleStatus st = arcr_module_validate(
        desc,
        arcr_host_compile_arch(),
        arcr_platform_os_version(),
        arcr_detect_cpu_flags()
    );
    if (st != ARCR_MOD_OK || !desc->vtable) {
        arcr_platform_dlclose(lib);
        return false;
    }

    const WaterscapePluginVTable *vt = (const WaterscapePluginVTable *)desc->vtable;
    if (vt->struct_size != sizeof(WaterscapePluginVTable) ||
        !vt->plugin_id || vt->plugin_id[0] == '\0') {
        arcr_platform_dlclose(lib);
        return false;
    }

    void *instance = NULL;
    if (vt->on_init) {
        if (!vt->on_init(&instance)) {
            arcr_platform_dlclose(lib);
            return false;
        }
    }

    WsAddonSlot *slot = acquire_slot_for_id(mgr, vt->plugin_id);
    if (!slot) {
        if (vt->on_shutdown) vt->on_shutdown(instance);
        arcr_platform_dlclose(lib);
        return false;
    }

    arcr_str_copy(slot->info.id, sizeof(slot->info.id), vt->plugin_id);
    arcr_str_copy(slot->info.name, sizeof(slot->info.name),
                  (vt->plugin_name && vt->plugin_name[0] != '\0') ? vt->plugin_name : vt->plugin_id);
    arcr_str_copy(slot->info.version, sizeof(slot->info.version),
                  (vt->plugin_version && vt->plugin_version[0] != '\0') ? vt->plugin_version : "1.0.0");
    arcr_str_copy(slot->info.description, sizeof(slot->info.description),
                  vt->plugin_description ? vt->plugin_description : "");
    slot->info.kind              = WS_ADDON_NATIVE_PLUGIN;
    slot->info.enabled           = true;
    slot->info.filter_rule_count = 0u;
    slot->info.blocked_hits      = 0u;
    slot->info.has_css           = (vt->get_injected_css != NULL);
    slot->info.has_js            = (vt->get_injected_js != NULL);

    slot->plugin_lib      = lib;
    slot->plugin_vt       = vt;
    slot->plugin_instance = instance;

    rebuild_domain_hash_index(mgr);
    return true;
}

/* =========================================================================
 * 6. Cross-Platform Directory Scanner (`_WIN32` FindFirstFileA / POSIX opendir)
 * ========================================================================= */
static bool str_ends_with_nocase(const char *s, const char *suffix) {
    size_t slen = strlen(s);
    size_t xlen = strlen(suffix);
    if (slen < xlen) return false;
    return arcr_str_eq_nocase_n(s + slen - xlen, xlen, suffix, xlen);
}

static bool extract_native_plugin_stem(const char *filename, char *out_stem, size_t stem_cap) {
    const char *base = filename;
    size_t blen = strlen(filename);

    if (str_ends_with_nocase(filename, ".dll")) {
        blen -= 4u;
    } else if (str_ends_with_nocase(filename, ".so")) {
        blen -= 3u;
    } else if (str_ends_with_nocase(filename, ".dylib")) {
        blen -= 6u;
    } else {
        return false;
    }

    if (blen > 3u && arcr_str_starts_with_nocase(base, "lib")) {
        base += 3u;
        blen -= 3u;
    }
    if (blen < 10u || !arcr_str_starts_with_nocase(base, "ws_plugin_")) {
        return false;
    }
    if (blen + 1u > stem_cap) return false;
    memcpy(out_stem, base, blen);
    out_stem[blen] = '\0';
    return true;
}

static size_t scan_single_entry(WsAddonManager *mgr, const char *addons_dir, const char *name) {
    if (!name || name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0u;
    }

    if (str_ends_with_nocase(name, ".wsx")) {
        char full_path[1024];
        join_dir_path(full_path, sizeof(full_path), addons_dir, name);
        return ws_addons_load_wsx_file(mgr, full_path) ? 1u : 0u;
    }

    char stem[128];
    if (extract_native_plugin_stem(name, stem, sizeof(stem))) {
        return ws_addons_load_native_plugin(mgr, addons_dir, stem) ? 1u : 0u;
    }

    /* Check if `addons_dir/name/manifest.json` exists as an unpacked extension folder */
    char sub_dir[1024];
    char sub_manifest[1024];
    join_dir_path(sub_dir, sizeof(sub_dir), addons_dir, name);
    join_dir_path(sub_manifest, sizeof(sub_manifest), sub_dir, "manifest.json");
    FILE *probe = ws_fopen_rb(sub_manifest);
    if (probe) {
        fclose(probe);
        return ws_addons_load_unpacked_dir(mgr, sub_dir) ? 1u : 0u;
    }
    return 0u;
}

size_t ws_addons_scan_directory(WsAddonManager *mgr, const char *addons_dir) {
    if (!mgr || !addons_dir || addons_dir[0] == '\0') return 0u;

    char names[WS_MAX_ADDONS * 2u][256];
    size_t name_count = 0u;

#if defined(_WIN32)
    char pattern[1024];
    join_dir_path(pattern, sizeof(pattern), addons_dir, "*");
    WCHAR wpattern[1024];
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pattern, -1,
                                 wpattern, (int)(sizeof(wpattern) / sizeof(wpattern[0])));
    if (wn <= 0) {
        wn = MultiByteToWideChar(CP_ACP, 0u, pattern, -1,
                                 wpattern, (int)(sizeof(wpattern) / sizeof(wpattern[0])));
    }
    if (wn <= 0) return 0u;
    for (int i = 0; wpattern[i] != L'\0'; ++i) {
        if (wpattern[i] == L'/') wpattern[i] = L'\\';
    }
    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW(wpattern, &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return 0u;
    do {
        if (wcscmp(ffd.cFileName, L".") != 0 && wcscmp(ffd.cFileName, L"..") != 0 &&
            name_count < ARCR_ARRAY_LEN(names)) {
            char u8_name[256];
            int un = WideCharToMultiByte(CP_UTF8, 0u, ffd.cFileName, -1,
                                         u8_name, (int)sizeof(u8_name), NULL, NULL);
            if (un > 0 && u8_name[0] != '\0') {
                arcr_str_copy(names[name_count++], sizeof(names[0]), u8_name);
            }
        }
    } while (FindNextFileW(hFind, &ffd) != 0);
    FindClose(hFind);
#else
    DIR *dp = opendir(addons_dir);
    if (!dp) return 0u;
    struct dirent *de = NULL;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0 &&
            name_count < ARCR_ARRAY_LEN(names)) {
            arcr_str_copy(names[name_count++], sizeof(names[0]), de->d_name);
        }
    }
    closedir(dp);
#endif

    /* Sort entries alphabetically so directory scan order is deterministic across filesystems */
    for (size_t i = 1u; i < name_count; ++i) {
        char tmp[256];
        arcr_str_copy(tmp, sizeof(tmp), names[i]);
        size_t j = i;
        while (j > 0u && strcmp(names[j - 1u], tmp) > 0) {
            arcr_str_copy(names[j], sizeof(names[0]), names[j - 1u]);
            --j;
        }
        arcr_str_copy(names[j], sizeof(names[0]), tmp);
    }

    /* Load unpacked subdirectories first, then `.wsx` archives & native plugins */
    size_t loaded = 0u;
    for (size_t pass = 0u; pass < 2u; ++pass) {
        for (size_t i = 0u; i < name_count; ++i) {
            bool is_wsx_or_lib = str_ends_with_nocase(names[i], ".wsx") ||
                                 str_ends_with_nocase(names[i], ".dll") ||
                                 str_ends_with_nocase(names[i], ".so")  ||
                                 str_ends_with_nocase(names[i], ".dylib");
            if ((pass == 0u && !is_wsx_or_lib) || (pass == 1u && is_wsx_or_lib)) {
                loaded += scan_single_entry(mgr, addons_dir, names[i]);
            }
        }
    }
    return loaded;
}

/* =========================================================================
 * 7. Enumeration & Runtime Toggle State Control
 * ========================================================================= */
size_t ws_addons_count(const WsAddonManager *mgr) {
    return mgr ? mgr->count : 0u;
}

bool ws_addons_get_info(const WsAddonManager *mgr, size_t index, WsAddonInfo *out_info) {
    if (!mgr || index >= mgr->count || !out_info) return false;
    *out_info = mgr->slots[index].info;
    return true;
}

bool ws_addons_find_by_id(const WsAddonManager *mgr, const char *addon_id, WsAddonInfo *out_info) {
    if (!mgr || !addon_id || addon_id[0] == '\0') return false;
    for (size_t i = 0u; i < mgr->count; ++i) {
        if (strcmp(mgr->slots[i].info.id, addon_id) == 0) {
            if (out_info) *out_info = mgr->slots[i].info;
            return true;
        }
    }
    return false;
}

bool ws_addons_set_enabled(WsAddonManager *mgr, const char *addon_id, bool enabled) {
    if (!mgr || !addon_id || addon_id[0] == '\0') return false;
    for (size_t i = 0u; i < mgr->count; ++i) {
        if (strcmp(mgr->slots[i].info.id, addon_id) == 0) {
            mgr->slots[i].info.enabled = enabled;
            return true;
        }
    }
    return false;
}

bool ws_addons_toggle(WsAddonManager *mgr, const char *addon_id) {
    if (!mgr || !addon_id || addon_id[0] == '\0') return false;
    for (size_t i = 0u; i < mgr->count; ++i) {
        if (strcmp(mgr->slots[i].info.id, addon_id) == 0) {
            mgr->slots[i].info.enabled = !mgr->slots[i].info.enabled;
            return true;
        }
    }
    return false;
}

/* =========================================================================
 * 8. High-Speed O(1) FNV-1a URL Blocker (`ws_addons_should_block_url`)
 * ========================================================================= */
static bool is_internal_unblockable_url(const char *url) {
    return arcr_str_starts_with_nocase(url, "waterscape:") ||
           arcr_str_starts_with_nocase(url, "arcr:")       ||
           arcr_str_starts_with_nocase(url, "about:")      ||
           arcr_str_starts_with_nocase(url, "data:")       ||
           arcr_str_starts_with_nocase(url, "javascript:") ||
           arcr_str_starts_with_nocase(url, "blob:");
}

static bool is_internal_browser_page_url(const char *url) {
    if (!url || url[0] == '\0') return false;
    while (*url == ' ' || *url == '\t') ++url;
    return arcr_str_starts_with_nocase(url, "waterscape:") ||
           arcr_str_starts_with_nocase(url, "arcr:");
}

static void extract_lowercase_host(const char *url_or_host, char *out_host, size_t cap) {
    if (!out_host || cap == 0u) return;
    out_host[0] = '\0';
    if (!url_or_host || url_or_host[0] == '\0') return;

    const char *p = url_or_host;
    while (*p == ' ' || *p == '\t') ++p;

    const char *scheme_sep = strstr(p, "://");
    if (scheme_sep) {
        p = scheme_sep + 3u;
    } else if (p[0] == '/' && p[1] == '/') {
        p += 2u;
    }

    /* Find end of authority (`/` `?` `#`) */
    size_t auth_len = 0u;
    while (p[auth_len] != '\0' && p[auth_len] != '/' &&
           p[auth_len] != '?'  && p[auth_len] != '#') {
        ++auth_len;
    }

    /* Strip optional `user:pass@` */
    for (size_t i = 0u; i < auth_len; ++i) {
        if (p[i] == '@') {
            p += i + 1u;
            auth_len -= i + 1u;
            break;
        }
    }

    /* Strip `:port` (preserving IPv6 bracketed literals `[::1]`) */
    size_t hlen = auth_len;
    if (hlen > 0u && p[0] != '[') {
        for (size_t i = 0u; i < hlen; ++i) {
            if (p[i] == ':') {
                hlen = i;
                break;
            }
        }
    }
    while (hlen > 0u && p[hlen - 1u] == '.') --hlen;

    size_t copy_n = ARCR_MIN(hlen, cap - 1u);
    for (size_t i = 0u; i < copy_n; ++i) {
        out_host[i] = arcr_ascii_tolower(p[i]);
    }
    out_host[copy_n] = '\0';
}

static bool domains_share_party(const char *a, const char *b) {
    if (!a || !b || a[0] == '\0' || b[0] == '\0') return false;
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    if (alen == blen) return memcmp(a, b, alen) == 0;
    if (alen > blen) {
        size_t off = alen - blen;
        return (a[off - 1u] == '.') && (memcmp(a + off, b, blen) == 0);
    }
    size_t off = blen - alen;
    return (b[off - 1u] == '.') && (memcmp(b + off, a, alen) == 0);
}

static bool lookup_domain_suffix_buckets(const WsAddonManager *mgr,
                                         WsDomainBucketNode *const buckets[WS_DOMAIN_HASH_BUCKETS],
                                         const char *req_host,
                                         const char *doc_host,
                                         uint32_t *out_addon_idx) {
    if (!req_host || req_host[0] == '\0') return false;
    size_t host_len = strlen(req_host);
    size_t pos = 0u;

    while (pos < host_len) {
        const char *suffix = req_host + pos;
        size_t slen = host_len - pos;

        /* Only test labels that contain at least one dot (e.g. `doubleclick.net`) */
        if (memchr(suffix, '.', slen) != NULL) {
            uint32_t h = fnv1a_lower_n(suffix, slen);
            const WsDomainBucketNode *node = buckets[h & WS_DOMAIN_HASH_MASK];
            while (node) {
                const WsAddonSlot *slot = &mgr->slots[node->addon_index];
                if (slot->info.enabled) {
                    const WsFilterRule *r = node->rule;
                    if (r->hash == h && r->len == slen && memcmp(r->pattern, suffix, slen) == 0) {
                        if (!r->third_party_only || doc_host[0] == '\0' ||
                            !domains_share_party(req_host, doc_host)) {
                            if (out_addon_idx) *out_addon_idx = node->addon_index;
                            return true;
                        }
                    }
                }
                node = node->next;
            }
        }

        const char *dot = (const char *)memchr(suffix, '.', slen);
        if (!dot) break;
        pos = (size_t)(dot - req_host) + 1u;
    }
    return false;
}

bool ws_addons_should_block_url(void *mgr_userdata, const char *url, const char *page_host) {
    WsAddonManager *mgr = (WsAddonManager *)mgr_userdata;
    if (!mgr || !url) return false;

    while (*url == ' ' || *url == '\t') ++url;
    if (*url == '\0' || is_internal_unblockable_url(url)) return false;

    char req_host[256];
    char doc_host[256];
    extract_lowercase_host(url, req_host, sizeof(req_host));
    extract_lowercase_host(page_host, doc_host, sizeof(doc_host));

    char url_lower[2048];
    size_t ulen = strlen(url);
    size_t copy_n = ARCR_MIN(ulen, sizeof(url_lower) - 1u);
    for (size_t i = 0u; i < copy_n; ++i) {
        url_lower[i] = arcr_ascii_tolower(url[i]);
    }
    url_lower[copy_n] = '\0';

    /* 1. Allowlist (`@@`) override check (domain hash + substring) */
    if (lookup_domain_suffix_buckets(mgr, mgr->allow_domain_buckets, req_host, doc_host, NULL)) {
        return false;
    }
    for (size_t s = 0u; s < mgr->count; ++s) {
        const WsAddonSlot *slot = &mgr->slots[s];
        if (!slot->info.enabled || slot->rule_count == 0u) continue;
        for (size_t r = 0u; r < slot->rule_count; ++r) {
            const WsFilterRule *rule = &slot->rules[r];
            if (rule->is_exception && rule->kind == WS_RULE_SUBSTRING) {
                if (strstr(url_lower, rule->pattern) != NULL) {
                    return false;
                }
            }
        }
    }

    /* 2. O(1) 512-bucket FNV-1a domain suffix lookup */
    uint32_t hit_addon = 0u;
    if (lookup_domain_suffix_buckets(mgr, mgr->block_domain_buckets, req_host, doc_host, &hit_addon)) {
        mgr->slots[hit_addon].info.blocked_hits++;
        mgr->total_blocked++;
        return true;
    }

    /* 3. Fast substring rules across enabled extensions */
    for (size_t s = 0u; s < mgr->count; ++s) {
        WsAddonSlot *slot = &mgr->slots[s];
        if (!slot->info.enabled || slot->rule_count == 0u) continue;
        for (size_t r = 0u; r < slot->rule_count; ++r) {
            const WsFilterRule *rule = &slot->rules[r];
            if (!rule->is_exception && rule->kind == WS_RULE_SUBSTRING) {
                if (rule->third_party_only && doc_host[0] != '\0' &&
                    domains_share_party(req_host, doc_host)) {
                    continue;
                }
                if (strstr(url_lower, rule->pattern) != NULL) {
                    slot->info.blocked_hits++;
                    mgr->total_blocked++;
                    return true;
                }
            }
        }
    }

    /* 4. Native C11 plugins `should_block_url` callback */
    for (size_t s = 0u; s < mgr->count; ++s) {
        WsAddonSlot *slot = &mgr->slots[s];
        if (slot->info.enabled &&
            slot->info.kind == WS_ADDON_NATIVE_PLUGIN &&
            slot->plugin_vt &&
            slot->plugin_vt->should_block_url) {
            if (slot->plugin_vt->should_block_url(slot->plugin_instance, url,
                                                  page_host ? page_host : "")) {
                slot->info.blocked_hits++;
                mgr->total_blocked++;
                return true;
            }
        }
    }

    return false;
}

uint32_t ws_addons_total_blocked(const WsAddonManager *mgr) {
    return mgr ? mgr->total_blocked : 0u;
}

void ws_addons_reset_blocked_counts(WsAddonManager *mgr) {
    if (!mgr) return;
    mgr->total_blocked = 0u;
    for (size_t i = 0u; i < mgr->count; ++i) {
        mgr->slots[i].info.blocked_hits = 0u;
    }
}

/* =========================================================================
 * 9. Active UserCSS, Content JS & Native Plugin HUD Aggregation
 * ========================================================================= */
bool ws_addons_collect_injected_css(WsAddonManager *mgr, const char *url,
                                    char **out_css, size_t *out_len) {
    if (out_css) *out_css = NULL;
    if (out_len) *out_len = 0u;
    if (!mgr || !out_css) return false;

    /* Never inject extension CSS into internal `waterscape://` or `arcr://` UI pages */
    if (is_internal_browser_page_url(url)) return false;

    char  *agg = NULL;
    size_t len = 0u;
    const char *safe_url = url ? url : "";

    for (size_t i = 0u; i < mgr->count; ++i) {
        const WsAddonSlot *slot = &mgr->slots[i];
        if (!slot->info.enabled) continue;

        if (slot->info.kind == WS_ADDON_EXTENSION_WSX) {
            if (slot->css_text && slot->css_len > 0u) {
                if (!append_chunk(&agg, &len, (const uint8_t *)slot->css_text, slot->css_len, '\n')) {
                    free(agg);
                    return false;
                }
            }
        } else if (slot->info.kind == WS_ADDON_NATIVE_PLUGIN &&
                   slot->plugin_vt && slot->plugin_vt->get_injected_css) {
            const char *pcss = slot->plugin_vt->get_injected_css(slot->plugin_instance, safe_url);
            if (pcss && pcss[0] != '\0') {
                if (!append_chunk(&agg, &len, (const uint8_t *)pcss, strlen(pcss), '\n')) {
                    free(agg);
                    return false;
                }
            }
        }
    }

    if (!agg || len == 0u) {
        free(agg);
        return false;
    }
    *out_css = agg;
    if (out_len) *out_len = len;
    return true;
}

bool ws_addons_collect_injected_js(WsAddonManager *mgr, const char *url,
                                   char **out_js, size_t *out_len) {
    if (out_js)  *out_js  = NULL;
    if (out_len) *out_len = 0u;
    if (!mgr || !out_js) return false;

    if (is_internal_browser_page_url(url)) return false;

    char  *agg = NULL;
    size_t len = 0u;
    const char *safe_url = url ? url : "";

    for (size_t i = 0u; i < mgr->count; ++i) {
        const WsAddonSlot *slot = &mgr->slots[i];
        if (!slot->info.enabled) continue;

        if (slot->info.kind == WS_ADDON_EXTENSION_WSX) {
            if (slot->js_text && slot->js_len > 0u) {
                if (!append_chunk(&agg, &len, (const uint8_t *)slot->js_text, slot->js_len, '\n')) {
                    free(agg);
                    return false;
                }
            }
        } else if (slot->info.kind == WS_ADDON_NATIVE_PLUGIN &&
                   slot->plugin_vt && slot->plugin_vt->get_injected_js) {
            const char *pjs = slot->plugin_vt->get_injected_js(slot->plugin_instance, safe_url);
            if (pjs && pjs[0] != '\0') {
                if (!append_chunk(&agg, &len, (const uint8_t *)pjs, strlen(pjs), '\n')) {
                    free(agg);
                    return false;
                }
            }
        }
    }

    if (!agg || len == 0u) {
        free(agg);
        return false;
    }
    *out_js = agg;
    if (out_len) *out_len = len;
    return true;
}

void ws_addons_format_plugin_hud(WsAddonManager *mgr, const WaterscapePluginHostContext *host,
                                 char *out_buf, size_t out_cap) {
    if (!out_buf || out_cap == 0u) return;
    out_buf[0] = '\0';
    if (!mgr || !host) return;

    size_t used = 0u;
    for (size_t i = 0u; i < mgr->count; ++i) {
        const WsAddonSlot *slot = &mgr->slots[i];
        if (!slot->info.enabled ||
            slot->info.kind != WS_ADDON_NATIVE_PLUGIN ||
            !slot->plugin_vt ||
            !slot->plugin_vt->format_hud_badge) {
            continue;
        }

        char badge[256];
        badge[0] = '\0';
        slot->plugin_vt->format_hud_badge(slot->plugin_instance, host, badge, sizeof(badge));
        if (badge[0] == '\0') continue;

        if (used > 0u && used + 3u < out_cap) {
            memcpy(out_buf + used, " | ", 3u);
            used += 3u;
            out_buf[used] = '\0';
        }
        size_t blen = strlen(badge);
        size_t avail = (used + 1u < out_cap) ? (out_cap - used - 1u) : 0u;
        size_t copy_n = (blen < avail) ? blen : avail;
        if (copy_n > 0u) {
            memcpy(out_buf + used, badge, copy_n);
            used += copy_n;
            out_buf[used] = '\0';
        }
    }
}
