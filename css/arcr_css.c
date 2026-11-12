/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * css/arcr_css.c - O(1) Rightmost-Indexed CSS3 Engine, Media Queries, var()/calc() & Style Sharing
 */
#include "arcr_css.h"
#include <stdlib.h>
#include <string.h>

#define ARCR_CSS_ID_BUCKETS    64u
#define ARCR_CSS_CLASS_BUCKETS 128u
#define ARCR_CSS_TAG_BUCKETS   64u
#define ARCR_CSS_MAX_STEPS     6u
#define ARCR_CSS_STEP_CLASSES  4u
#define ARCR_CSS_STEP_ATTRS    2u
#define ARCR_STYLE_SHARE_RING  32u

typedef enum CssCombinator {
    CSS_COMB_NONE       = 0,
    CSS_COMB_DESCENDANT = 1,
    CSS_COMB_CHILD      = 2,
    CSS_COMB_ADJACENT   = 3
} CssCombinator;

typedef struct CssAttrCond {
    ArcrAtom    name;
    const char *val;
    size_t      val_len;
    bool        has_val;
} CssAttrCond;

typedef struct CssCompoundSel {
    ArcrAtom      tag;
    ArcrAtom      id;
    ArcrAtom      classes[ARCR_CSS_STEP_CLASSES];
    uint8_t       class_count;
    CssAttrCond   attrs[ARCR_CSS_STEP_ATTRS];
    uint8_t       attr_count;
    uint32_t      req_state;
    bool          req_root;
    bool          req_first_child;
    bool          req_last_child;
    bool          impossible;
    CssCombinator comb_to_next;
} CssCompoundSel;

typedef struct ArcrCssDecl {
    ArcrAtom            prop;
    const char         *val_str;
    size_t              val_len;
    bool                important;
    struct ArcrCssDecl *next;
} ArcrCssDecl;

typedef struct ArcrCssRule {
    CssCompoundSel      steps[ARCR_CSS_MAX_STEPS];
    uint8_t             step_count;
    bool                uses_state_pseudo;
    bool                from_media;
    uint64_t            ancestor_bloom_req;
    uint32_t            specificity;
    uint32_t            source_order;
    ArcrCssDecl        *decls;
    struct ArcrCssRule *next_in_bucket;
} ArcrCssRule;

typedef struct ArcrCssVar {
    ArcrAtom           name;
    const char        *val;
    size_t             val_len;
    struct ArcrCssVar *next;
} ArcrCssVar;

struct ArcrStylesheet {
    ArcrCssMediaEnv env;
    ArcrCssRule    *by_id[ARCR_CSS_ID_BUCKETS];
    ArcrCssRule    *by_class[ARCR_CSS_CLASS_BUCKETS];
    ArcrCssRule    *by_tag[ARCR_CSS_TAG_BUCKETS];
    ArcrCssRule    *universal_list;
    ArcrCssVar     *vars_head;
    uint32_t        rule_count;
    bool            uses_positional_or_sibling;
};

typedef struct ArcrStyleShareSlot {
    const ArcrComputedStyle *parent_style;
    ArcrAtom                 tag;
    uint32_t                 class_count;
    ArcrAtom                 classes[ARCR_MAX_INLINE_CLASSES];
    bool                     is_first_el;
    bool                     is_last_el;
    const ArcrComputedStyle *shared_style;
} ArcrStyleShareSlot;

static bool is_css_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool is_css_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           ((unsigned char)c >= 0x80u);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parses fixed-point scaled by 1000 (e.g. "1.5" -> 1500, "-0.25" -> -250, "42" -> 42000) */
static bool parse_fixed_1000(const char *s, size_t len, int64_t *out_milli, size_t *out_end) {
    size_t i = 0u;
    while (i < len && is_css_ws(s[i])) ++i;
    if (i >= len) return false;

    int64_t sign = 1;
    if (s[i] == '-') {
        sign = -1;
        ++i;
    } else if (s[i] == '+') {
        ++i;
    }

    int64_t whole  = 0;
    size_t  digits = 0u;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        whole = whole * 10 + (s[i] - '0');
        if (whole > 1000000LL) whole = 1000000LL;
        ++i;
        ++digits;
    }

    int64_t frac  = 0;
    int64_t scale = 100;
    if (i < len && s[i] == '.') {
        ++i;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            if (scale >= 1) {
                frac += (s[i] - '0') * scale;
                scale /= 10;
            }
            ++i;
            ++digits;
        }
    }
    if (digits == 0u) return false;
    *out_milli = sign * (whole * 1000 + frac);
    if (out_end) *out_end = i;
    return true;
}

static uint32_t hsl_to_argb(int32_t deg, int32_t sat_pct, int32_t light_pct, int32_t alpha_255) {
    int32_t h = deg % 360;
    if (h < 0) h += 360;
    int32_t s = ARCR_CLAMP(sat_pct, 0, 100) * 10;   /* 0..1000 */
    int32_t l = ARCR_CLAMP(light_pct, 0, 100) * 10; /* 0..1000 */
    int32_t a = ARCR_CLAMP(alpha_255, 0, 255);

    int32_t two_l_m1 = 2 * l - 1000;
    if (two_l_m1 < 0) two_l_m1 = -two_l_m1;
    int32_t c = ((1000 - two_l_m1) * s) / 1000; /* 0..1000 */

    int32_t h_mod = (h * 1000) / 60; /* 0..6000 */
    int32_t mod2  = (h_mod % 2000) - 1000;
    if (mod2 < 0) mod2 = -mod2;
    int32_t x = (c * (1000 - mod2)) / 1000;
    int32_t m = l - c / 2;

    int32_t r1 = 0, g1 = 0, b1 = 0;
    if (h < 60)       { r1 = c; g1 = x; b1 = 0; }
    else if (h < 120) { r1 = x; g1 = c; b1 = 0; }
    else if (h < 180) { r1 = 0; g1 = c; b1 = x; }
    else if (h < 240) { r1 = 0; g1 = x; b1 = c; }
    else if (h < 300) { r1 = x; g1 = 0; b1 = c; }
    else              { r1 = c; g1 = 0; b1 = x; }

    uint32_t r = (uint32_t)ARCR_CLAMP(((r1 + m) * 255 + 500) / 1000, 0, 255);
    uint32_t g = (uint32_t)ARCR_CLAMP(((g1 + m) * 255 + 500) / 1000, 0, 255);
    uint32_t b = (uint32_t)ARCR_CLAMP(((b1 + m) * 255 + 500) / 1000, 0, 255);
    return ((uint32_t)a << 24) | (r << 16) | (g << 8) | b;
}

static int32_t parse_alpha_component(const char *s, size_t len) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return 255;
    bool is_pct = false;
    if (s[len - 1u] == '%') {
        is_pct = true;
        --len;
    }
    int64_t milli = 1000;
    if (!parse_fixed_1000(s, len, &milli, NULL)) return 255;
    if (is_pct) {
        return (int32_t)ARCR_CLAMP((milli * 255 + 50000) / 100000, 0, 255);
    }
    return (int32_t)ARCR_CLAMP((milli * 255 + 500) / 1000, 0, 255);
}

static size_t split_color_args(const char *inner, size_t ilen,
                               const char *parts[4], size_t plens[4]) {
    size_t count = 0u;
    size_t i = 0u;
    while (i < ilen && count < 4u) {
        while (i < ilen && (is_css_ws(inner[i]) || inner[i] == ',' || inner[i] == '/')) ++i;
        if (i >= ilen) break;
        size_t start = i;
        while (i < ilen && !is_css_ws(inner[i]) && inner[i] != ',' && inner[i] != '/') ++i;
        parts[count] = inner + start;
        plens[count] = i - start;
        count++;
    }
    return count;
}

bool arcr_css_parse_color(const char *s, size_t len, uint32_t *out_argb) {
    if (!s || !out_argb) return false;
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;

    if (s[0] == '#') {
        if (len == 4u || len == 5u) {
            int r = hex_nibble(s[1]), g = hex_nibble(s[2]), b = hex_nibble(s[3]);
            int a = (len == 5u) ? hex_nibble(s[4]) : 15;
            if (r < 0 || g < 0 || b < 0 || a < 0) return false;
            *out_argb = ((uint32_t)(a * 17) << 24) | ((uint32_t)(r * 17) << 16) |
                        ((uint32_t)(g * 17) << 8) | (uint32_t)(b * 17);
            return true;
        }
        if (len == 7u || len == 9u) {
            int r1 = hex_nibble(s[1]), r2 = hex_nibble(s[2]);
            int g1 = hex_nibble(s[3]), g2 = hex_nibble(s[4]);
            int b1 = hex_nibble(s[5]), b2 = hex_nibble(s[6]);
            int a1 = 15, a2 = 15;
            if (len == 9u) {
                a1 = hex_nibble(s[7]);
                a2 = hex_nibble(s[8]);
            }
            if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 ||
                b1 < 0 || b2 < 0 || a1 < 0 || a2 < 0) {
                return false;
            }
            *out_argb = ((uint32_t)((a1 << 4) | a2) << 24) |
                        ((uint32_t)((r1 << 4) | r2) << 16) |
                        ((uint32_t)((g1 << 4) | g2) << 8) |
                        (uint32_t)((b1 << 4) | b2);
            return true;
        }
        return false;
    }

    if (s[len - 1u] == ')') {
        bool is_rgb  = (len > 5u && arcr_str_eq_nocase_n(s, 4u, "rgb(", 4u));
        bool is_rgba = (len > 6u && arcr_str_eq_nocase_n(s, 5u, "rgba(", 5u));
        if (is_rgb || is_rgba) {
            size_t hdr = is_rgba ? 5u : 4u;
            const char *parts[4];
            size_t      plens[4];
            size_t n = split_color_args(s + hdr, len - hdr - 1u, parts, plens);
            if (n < 3u) return false;

            int32_t rgb[3] = {0, 0, 0};
            for (int c = 0; c < 3; ++c) {
                size_t cl = plens[c];
                bool pct = (cl > 0u && parts[c][cl - 1u] == '%');
                if (pct) --cl;
                int64_t m = 0;
                if (!parse_fixed_1000(parts[c], cl, &m, NULL)) return false;
                if (pct) rgb[c] = (int32_t)ARCR_CLAMP((m * 255 + 50000) / 100000, 0, 255);
                else     rgb[c] = (int32_t)ARCR_CLAMP((m + 500) / 1000, 0, 255);
            }
            int32_t a = (n >= 4u) ? parse_alpha_component(parts[3], plens[3]) : 255;
            *out_argb = ((uint32_t)a << 24) | ((uint32_t)rgb[0] << 16) |
                        ((uint32_t)rgb[1] << 8) | (uint32_t)rgb[2];
            return true;
        }

        bool is_hsl  = (len > 5u && arcr_str_eq_nocase_n(s, 4u, "hsl(", 4u));
        bool is_hsla = (len > 6u && arcr_str_eq_nocase_n(s, 5u, "hsla(", 5u));
        if (is_hsl || is_hsla) {
            size_t hdr = is_hsla ? 5u : 4u;
            const char *parts[4];
            size_t      plens[4];
            size_t n = split_color_args(s + hdr, len - hdr - 1u, parts, plens);
            if (n < 3u) return false;

            int64_t hm = 0, sm = 0, lm = 0;
            size_t sl = plens[1], ll = plens[2];
            if (sl > 0u && parts[1][sl - 1u] == '%') --sl;
            if (ll > 0u && parts[2][ll - 1u] == '%') --ll;
            size_t hl = plens[0];
            if (hl > 3u && arcr_str_eq_nocase_n(parts[0] + hl - 3u, 3u, "deg", 3u)) hl -= 3u;
            if (!parse_fixed_1000(parts[0], hl, &hm, NULL)) return false;
            if (!parse_fixed_1000(parts[1], sl, &sm, NULL)) return false;
            if (!parse_fixed_1000(parts[2], ll, &lm, NULL)) return false;
            int32_t a = (n >= 4u) ? parse_alpha_component(parts[3], plens[3]) : 255;
            *out_argb = hsl_to_argb((int32_t)(hm / 1000), (int32_t)(sm / 1000),
                                    (int32_t)(lm / 1000), a);
            return true;
        }
    }

    static const struct {
        const char *name;
        uint32_t    argb;
    } kNamed[] = {
        {"transparent",   0x00000000u}, {"black",         0xFF000000u},
        {"white",         0xFFFFFFFFu}, {"red",           0xFFDC2626u},
        {"blue",          0xFF2563EBu}, {"green",         0xFF16A34Au},
        {"gray",          0xFF6B7280u}, {"grey",          0xFF6B7280u},
        {"darkgray",      0xFF4B5563u}, {"darkgrey",      0xFF4B5563u},
        {"lightgray",     0xFFD1D5DBu}, {"lightgrey",     0xFFD1D5DBu},
        {"dimgray",       0xFF696969u}, {"slategray",     0xFF64748Bu},
        {"slategrey",     0xFF64748Bu}, {"whitesmoke",    0xFFF5F5F5u},
        {"navy",          0xFF1E3A8Au}, {"royalblue",     0xFF2563EBu},
        {"dodgerblue",    0xFF0284C7u}, {"skyblue",       0xFF38BDF8u},
        {"lightblue",     0xFFBAE6FDu}, {"midnightblue",  0xFF172554u},
        {"teal",          0xFF0D9488u}, {"cyan",          0xFF06B6D4u},
        {"aqua",          0xFF06B6D4u}, {"indigo",        0xFF4F46E5u},
        {"purple",        0xFF7E22CEu}, {"rebeccapurple", 0xFF663399u},
        {"violet",        0xFF8B5CF6u}, {"magenta",       0xFFD946EFu},
        {"fuchsia",       0xFFD946EFu}, {"pink",          0xFFF472B6u},
        {"hotpink",       0xFFEC4899u}, {"crimson",       0xFFBE123Cu},
        {"maroon",        0xFF7F1D1Du}, {"brown",         0xFF92400Eu},
        {"coral",         0xFFFB7185u}, {"tomato",        0xFFEF4444u},
        {"orange",        0xFFF97316u}, {"gold",          0xFFEAB308u},
        {"yellow",        0xFFFACC15u}, {"khaki",         0xFFFDE047u},
        {"olive",         0xFF65A30Du}, {"lime",          0xFF84CC16u},
        {"forestgreen",   0xFF15803Du}, {"seagreen",      0xFF059669u},
        {"silver",        0xFFCBD5E1u}, {"ivory",         0xFFFFFFF0u},
        {"beige",         0xFFF5F5DCu}, {"aliceblue",     0xFFF0F9FFu},
        {"lavender",      0xFFEDE9FEu}
    };
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kNamed); ++i) {
        size_t nl = strlen(kNamed[i].name);
        if (len == nl && arcr_str_eq_nocase_n(s, len, kNamed[i].name, nl)) {
            *out_argb = kNamed[i].argb;
            return true;
        }
    }
    return false;
}

/* Extracts a color from `background` shorthands including `linear-gradient(...)` */
static bool parse_background_color_smart(const char *s, size_t len,
                                         uint32_t current_color, uint32_t *out_argb) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;
    if (arcr_str_eq_nocase_n(s, len, "none", 4u)) {
        *out_argb = 0x00000000u;
        return true;
    }
    if (arcr_str_eq_nocase_n(s, len, "currentColor", 12u)) {
        *out_argb = current_color;
        return true;
    }
    if (arcr_css_parse_color(s, len, out_argb)) return true;

    /* Scan tokens or gradient stops for the first valid CSS color */
    size_t i = 0u;
    while (i < len) {
        while (i < len && (is_css_ws(s[i]) || s[i] == ',')) ++i;
        if (i >= len) break;

        if ((i + 16u <= len && arcr_str_eq_nocase_n(s + i, 16u, "linear-gradient(", 16u)) ||
            (i + 16u <= len && arcr_str_eq_nocase_n(s + i, 16u, "radial-gradient(", 16u))) {
            i += 16u;
            continue;
        }

        size_t start = i;
        int paren = 0;
        while (i < len) {
            if (s[i] == '(') ++paren;
            else if (s[i] == ')') {
                if (paren == 0) break;
                --paren;
                if (paren == 0) {
                    ++i;
                    break;
                }
            } else if (paren == 0 && (is_css_ws(s[i]) || s[i] == ',')) {
                break;
            }
            ++i;
        }
        if (i > start) {
            uint32_t c = 0u;
            if (arcr_css_parse_color(s + start, i - start, &c)) {
                *out_argb = c;
                return true;
            }
        }
        if (i < len && s[i] == ')') ++i;
    }
    return false;
}

static size_t strip_css_comments(const char *src, size_t len, char *dst) {
    size_t i = 0u, out = 0u;
    while (i < len) {
        if (i + 1u < len && src[i] == '/' && src[i + 1u] == '*') {
            i += 2u;
            while (i + 1u < len && !(src[i] == '*' && src[i + 1u] == '/')) ++i;
            i = (i + 1u < len) ? (i + 2u) : len;
            dst[out++] = ' ';
        } else {
            dst[out++] = src[i++];
        }
    }
    dst[out] = '\0';
    return out;
}

static void parse_compound_selector(ArcrAtomTable *atoms, const char *s, size_t len,
                                    CssCompoundSel *out, uint32_t *inout_spec) {
    memset(out, 0, sizeof(*out));
    arcr_str_trim_view(&s, &len);
    if (len == 0u) {
        out->impossible = true;
        return;
    }

    size_t i = 0u;
    if (s[0] == '*') {
        ++i;
    } else if (is_css_ident_char(s[0])) {
        size_t start = 0u;
        while (i < len && is_css_ident_char(s[i])) ++i;
        out->tag = arcr_atom_intern_lower(atoms, s + start, i - start);
        *inout_spec += 1u;
    }

    while (i < len) {
        char c = s[i];
        if (c == '#') {
            ++i;
            size_t start = i;
            while (i < len && is_css_ident_char(s[i])) ++i;
            if (i > start) {
                out->id = arcr_atom_intern(atoms, s + start, i - start);
                *inout_spec += 10000u;
            } else {
                out->impossible = true;
                return;
            }
        } else if (c == '.') {
            ++i;
            size_t start = i;
            while (i < len && is_css_ident_char(s[i])) ++i;
            if (i > start) {
                if (out->class_count < ARCR_CSS_STEP_CLASSES) {
                    out->classes[out->class_count++] = arcr_atom_intern(atoms, s + start, i - start);
                }
                *inout_spec += 100u;
            } else {
                out->impossible = true;
                return;
            }
        } else if (c == '[') {
            ++i;
            size_t close_b = i;
            while (close_b < len && s[close_b] != ']') ++close_b;
            if (close_b >= len) {
                out->impossible = true;
                return;
            }
            const char *inner = s + i;
            size_t ilen = close_b - i;
            arcr_str_trim_view(&inner, &ilen);
            size_t eq = 0u;
            while (eq < ilen && inner[eq] != '=' && inner[eq] != '~' &&
                   inner[eq] != '^' && inner[eq] != '$' && inner[eq] != '*') {
                ++eq;
            }
            if (ilen > 0u && out->attr_count < ARCR_CSS_STEP_ATTRS) {
                const char *an = inner;
                size_t al = eq;
                arcr_str_trim_view(&an, &al);
                if (al > 0u) {
                    CssAttrCond *ac = &out->attrs[out->attr_count++];
                    ac->name = arcr_atom_intern_lower(atoms, an, al);
                    size_t val_pos = eq;
                    while (val_pos < ilen && inner[val_pos] != '=') ++val_pos;
                    if (val_pos < ilen && inner[val_pos] == '=') {
                        ac->has_val = true;
                        const char *vp = inner + val_pos + 1u;
                        size_t vl = ilen - val_pos - 1u;
                        arcr_str_trim_view(&vp, &vl);
                        if (vl >= 2u && ((vp[0] == '"' && vp[vl - 1u] == '"') ||
                                         (vp[0] == '\'' && vp[vl - 1u] == '\''))) {
                            vp += 1u;
                            vl -= 2u;
                        }
                        ac->val     = vp; /* Points into arena-backed cleaned CSS buffer */
                        ac->val_len = vl;
                    }
                    *inout_spec += 100u;
                }
            }
            i = close_b + 1u;
        } else if (c == ':') {
            ++i;
            if (i < len && s[i] == ':') {
                /* Pseudo-elements (::before, ::after, ::placeholder) must not match host element */
                out->impossible = true;
                return;
            }
            size_t start = i;
            while (i < len && is_css_ident_char(s[i])) ++i;
            const char *ps = s + start;
            size_t plen = i - start;
            if (i < len && s[i] == '(') {
                int depth = 1;
                ++i;
                while (i < len && depth > 0) {
                    if (s[i] == '(') ++depth;
                    else if (s[i] == ')') --depth;
                    ++i;
                }
            }
            *inout_spec += 100u;
            if (arcr_str_eq_nocase_n(ps, plen, "hover", 5u)) {
                out->req_state |= ARCR_STATE_HOVER;
            } else if (arcr_str_eq_nocase_n(ps, plen, "focus", 5u) ||
                       arcr_str_eq_nocase_n(ps, plen, "focus-visible", 13u)) {
                out->req_state |= ARCR_STATE_FOCUS;
            } else if (arcr_str_eq_nocase_n(ps, plen, "active", 6u)) {
                out->req_state |= ARCR_STATE_ACTIVE;
            } else if (arcr_str_eq_nocase_n(ps, plen, "checked", 7u)) {
                out->req_state |= ARCR_STATE_CHECKED;
            } else if (arcr_str_eq_nocase_n(ps, plen, "visited", 7u)) {
                out->req_state |= ARCR_STATE_VISITED;
            } else if (arcr_str_eq_nocase_n(ps, plen, "link", 4u)) {
                /* Matches unvisited links */
            } else if (arcr_str_eq_nocase_n(ps, plen, "root", 4u)) {
                out->req_root = true;
            } else if (arcr_str_eq_nocase_n(ps, plen, "first-child", 11u)) {
                out->req_first_child = true;
            } else if (arcr_str_eq_nocase_n(ps, plen, "last-child", 10u)) {
                out->req_last_child = true;
            } else {
                out->impossible = true;
                return;
            }
        } else {
            out->impossible = true;
            return;
        }
    }
}

static bool parse_selector_chain(ArcrAtomTable *atoms, const char *one, size_t olen,
                                 ArcrCssRule *rule) {
    arcr_str_trim_view(&one, &olen);
    if (olen == 0u) return false;

    uint32_t spec = 0u;
    size_t   i    = 0u;

    while (i < olen && rule->step_count < ARCR_CSS_MAX_STEPS) {
        while (i < olen && is_css_ws(one[i])) ++i;
        if (i >= olen) break;

        size_t start = i;
        int bracket = 0;
        int paren   = 0;
        while (i < olen) {
            if (one[i] == '[') ++bracket;
            else if (one[i] == ']' && bracket > 0) --bracket;
            else if (one[i] == '(') ++paren;
            else if (one[i] == ')' && paren > 0) --paren;
            else if (bracket == 0 && paren == 0 &&
                     (is_css_ws(one[i]) || one[i] == '>' || one[i] == '+' || one[i] == '~')) {
                break;
            }
            ++i;
        }
        if (i == start) return false;

        CssCompoundSel *step = &rule->steps[rule->step_count];
        parse_compound_selector(atoms, one + start, i - start, step, &spec);
        if (step->impossible) return false;
        if (step->req_state != 0u) rule->uses_state_pseudo = true;

        bool saw_ws = false;
        while (i < olen && is_css_ws(one[i])) {
            saw_ws = true;
            ++i;
        }
        CssCombinator comb = CSS_COMB_NONE;
        if (i < olen && (one[i] == '>' || one[i] == '+' || one[i] == '~')) {
            char op = one[i++];
            if (op == '>')      comb = CSS_COMB_CHILD;
            else if (op == '+') comb = CSS_COMB_ADJACENT;
            else                return false; /* Ignore general sibling ~ rules cleanly */
            while (i < olen && is_css_ws(one[i])) ++i;
        } else if (saw_ws && i < olen) {
            comb = CSS_COMB_DESCENDANT;
        }
        step->comb_to_next = comb;
        rule->step_count++;
    }

    if (rule->step_count == 0u || i < olen) return false;
    rule->specificity = spec;

    /* Compute 64-bit ancestor Bloom mask across ancestor steps */
    uint64_t bloom = 0ULL;
    bool all_ancestor_combs = true;
    for (uint8_t s = 0u; s + 1u < rule->step_count; ++s) {
        if (rule->steps[s].comb_to_next != CSS_COMB_DESCENDANT &&
            rule->steps[s].comb_to_next != CSS_COMB_CHILD) {
            all_ancestor_combs = false;
            break;
        }
    }
    if (all_ancestor_combs) {
        for (uint8_t s = 0u; s + 1u < rule->step_count; ++s) {
            const CssCompoundSel *anc = &rule->steps[s];
            bloom |= arcr_bloom_bits_for_atom(anc->tag);
            bloom |= arcr_bloom_bits_for_atom(anc->id);
            for (uint8_t c = 0u; c < anc->class_count; ++c) {
                bloom |= arcr_bloom_bits_for_atom(anc->classes[c]);
            }
        }
    }
    rule->ancestor_bloom_req = bloom;
    return true;
}

static void insert_rule_indexed(ArcrStylesheet *sheet, ArcrCssRule *rule) {
    for (uint8_t s = 0u; s < rule->step_count; ++s) {
        if (rule->steps[s].req_first_child || rule->steps[s].req_last_child ||
            rule->steps[s].comb_to_next == CSS_COMB_ADJACENT) {
            sheet->uses_positional_or_sibling = true;
            break;
        }
    }

    const CssCompoundSel *key = &rule->steps[rule->step_count - 1u];
    ArcrCssRule **bucket = &sheet->universal_list;

    if (key->id != ARCR_ATOM_NULL) {
        bucket = &sheet->by_id[key->id & (ARCR_CSS_ID_BUCKETS - 1u)];
    } else if (key->class_count > 0u) {
        bucket = &sheet->by_class[key->classes[0] & (ARCR_CSS_CLASS_BUCKETS - 1u)];
    } else if (key->tag != ARCR_ATOM_NULL) {
        bucket = &sheet->by_tag[key->tag & (ARCR_CSS_TAG_BUCKETS - 1u)];
    }

    rule->next_in_bucket = *bucket;
    *bucket              = rule;
}

static ArcrCssDecl *parse_declarations(ArcrDocument *doc, ArcrStylesheet *sheet,
                                       const char *block, size_t len, bool collect_vars) {
    ArcrCssDecl *head = NULL;
    ArcrCssDecl *tail = NULL;
    size_t i = 0u;

    while (i < len) {
        size_t semi = i;
        int paren = 0;
        while (semi < len && !(block[semi] == ';' && paren == 0)) {
            if (block[semi] == '(') ++paren;
            else if (block[semi] == ')' && paren > 0) --paren;
            ++semi;
        }
        const char *stmt = block + i;
        size_t      slen = semi - i;
        i = (semi < len) ? (semi + 1u) : len;

        size_t colon = 0u;
        while (colon < slen && stmt[colon] != ':') ++colon;
        if (colon >= slen) continue;

        const char *pname = stmt;
        size_t      plen  = colon;
        const char *pval  = stmt + colon + 1u;
        size_t      vlen  = slen - colon - 1u;
        arcr_str_trim_view(&pname, &plen);
        arcr_str_trim_view(&pval, &vlen);
        if (plen == 0u || vlen == 0u) continue;

        bool important = false;
        if (vlen > 10u && arcr_str_eq_nocase_n(pval + vlen - 10u, 10u, "!important", 10u)) {
            important = true;
            vlen -= 10u;
            arcr_str_trim_view(&pval, &vlen);
        } else if (vlen > 9u && pval[vlen - 9u] == '!') {
            /* Handle "! important" with whitespace */
            size_t bang = vlen;
            while (bang > 0u && pval[bang - 1u] != '!') --bang;
            if (bang > 0u) {
                const char *imp = pval + bang;
                size_t il = vlen - bang;
                arcr_str_trim_view(&imp, &il);
                if (arcr_str_eq_nocase_n(imp, il, "important", 9u)) {
                    important = true;
                    vlen = bang - 1u;
                    arcr_str_trim_view(&pval, &vlen);
                }
            }
        }
        if (vlen == 0u) continue;

        if (plen > 2u && pname[0] == '-' && pname[1] == '-') {
            if (sheet && collect_vars) {
                ArcrCssVar *v = (ArcrCssVar *)arcr_arena_alloc(
                    &doc->arena, sizeof(ArcrCssVar), sizeof(void *));
                if (v) {
                    v->name          = arcr_atom_intern(doc->atoms, pname, plen);
                    v->val           = arcr_arena_strdup(&doc->arena, pval, vlen);
                    v->val_len       = vlen;
                    v->next          = sheet->vars_head;
                    sheet->vars_head = v;
                }
            }
            continue;
        }

        ArcrCssDecl *d = (ArcrCssDecl *)arcr_arena_alloc(
            &doc->arena, sizeof(ArcrCssDecl), sizeof(void *));
        if (!d) continue;
        d->prop      = arcr_atom_intern_lower(doc->atoms, pname, plen);
        d->val_str   = arcr_arena_strdup(&doc->arena, pval, vlen);
        d->val_len   = vlen;
        d->important = important;
        d->next      = NULL;
        if (!head) {
            head = d;
            tail = d;
        } else {
            tail->next = d;
            tail       = d;
        }
    }
    return head;
}

static int32_t parse_media_px(const char *s, size_t len) {
    arcr_str_trim_view(&s, &len);
    bool is_rem = false;
    if (len > 2u && arcr_str_eq_nocase_n(s + len - 2u, 2u, "px", 2u)) {
        len -= 2u;
    } else if (len > 3u && arcr_str_eq_nocase_n(s + len - 3u, 3u, "rem", 3u)) {
        len -= 3u;
        is_rem = true;
    } else if (len > 2u && arcr_str_eq_nocase_n(s + len - 2u, 2u, "em", 2u)) {
        len -= 2u;
        is_rem = true;
    }
    int64_t m = 0;
    if (!parse_fixed_1000(s, len, &m, NULL)) return 0;
    if (is_rem) return (int32_t)((m * 16 + 500) / 1000);
    return (int32_t)((m + 500) / 1000);
}

static bool eval_single_media_feature(const char *feat, size_t flen, const ArcrCssMediaEnv *env) {
    arcr_str_trim_view(&feat, &flen);
    if (flen >= 2u && feat[0] == '(' && feat[flen - 1u] == ')') {
        feat += 1u;
        flen -= 2u;
        arcr_str_trim_view(&feat, &flen);
    }
    size_t colon = 0u;
    while (colon < flen && feat[colon] != ':') ++colon;
    if (colon >= flen) {
        if (arcr_str_eq_nocase_n(feat, flen, "all", 3u) ||
            arcr_str_eq_nocase_n(feat, flen, "screen", 6u) ||
            arcr_str_eq_nocase_n(feat, flen, "only screen", 11u) ||
            arcr_str_eq_nocase_n(feat, flen, "not print", 9u)) {
            return true;
        }
        if (arcr_str_eq_nocase_n(feat, flen, "print", 5u)) return false;
        return true;
    }

    const char *k = feat;
    size_t      kl = colon;
    const char *v = feat + colon + 1u;
    size_t      vl = flen - colon - 1u;
    arcr_str_trim_view(&k, &kl);
    arcr_str_trim_view(&v, &vl);

    if (arcr_str_eq_nocase_n(k, kl, "max-width", 9u)) {
        return env->viewport_width <= parse_media_px(v, vl);
    }
    if (arcr_str_eq_nocase_n(k, kl, "min-width", 9u)) {
        return env->viewport_width >= parse_media_px(v, vl);
    }
    if (arcr_str_eq_nocase_n(k, kl, "max-height", 10u)) {
        return env->viewport_height <= parse_media_px(v, vl);
    }
    if (arcr_str_eq_nocase_n(k, kl, "min-height", 10u)) {
        return env->viewport_height >= parse_media_px(v, vl);
    }
    if (arcr_str_eq_nocase_n(k, kl, "prefers-color-scheme", 20u)) {
        if (arcr_str_eq_nocase_n(v, vl, "dark", 4u))  return env->prefers_dark;
        if (arcr_str_eq_nocase_n(v, vl, "light", 5u)) return !env->prefers_dark;
        return false;
    }
    return true;
}

static bool eval_media_query(const char *mq, size_t len, const ArcrCssMediaEnv *env) {
    arcr_str_trim_view(&mq, &len);
    if (len == 0u) return true;

    size_t pos = 0u;
    while (pos < len) {
        size_t comma = pos;
        int paren = 0;
        while (comma < len) {
            if (mq[comma] == '(') ++paren;
            else if (mq[comma] == ')' && paren > 0) --paren;
            else if (mq[comma] == ',' && paren == 0) break;
            ++comma;
        }
        const char *branch = mq + pos;
        size_t      blen   = comma - pos;
        pos = (comma < len) ? (comma + 1u) : len;
        arcr_str_trim_view(&branch, &blen);
        if (blen == 0u) continue;

        /* Split branch by top-level "and" */
        bool branch_ok = true;
        size_t bpos = 0u;
        while (bpos < blen) {
            size_t next_and = blen;
            size_t next_end = blen;
            int p2 = 0;
            for (size_t k = bpos; k < blen; ++k) {
                if (branch[k] == '(') ++p2;
                else if (branch[k] == ')' && p2 > 0) --p2;
                else if (p2 == 0 && k + 3u <= blen &&
                         (k == bpos || is_css_ws(branch[k - 1u])) &&
                         arcr_str_eq_nocase_n(branch + k, 3u, "and", 3u) &&
                         (k + 3u == blen || is_css_ws(branch[k + 3u]) || branch[k + 3u] == '(')) {
                    next_and = k;
                    next_end = k + 3u;
                    break;
                }
            }
            if (!eval_single_media_feature(branch + bpos, next_and - bpos, env)) {
                branch_ok = false;
                break;
            }
            bpos = next_end;
        }
        if (branch_ok) return true;
    }
    return false;
}

static void parse_stylesheet_rules(ArcrDocument *doc, ArcrStylesheet *sheet,
                                   const char *clean, size_t clen, bool in_media) {
    size_t i = 0u;
    while (i < clen) {
        while (i < clen && (is_css_ws(clean[i]) || clean[i] == ';')) ++i;
        if (i >= clen) break;

        /* Skip statement-level @import / @charset / @namespace ending in ';' */
        if (clean[i] == '@') {
            size_t scan = i;
            while (scan < clen && clean[scan] != '{' && clean[scan] != ';') ++scan;
            if (scan < clen && clean[scan] == ';') {
                i = scan + 1u;
                continue;
            }
        }

        size_t brace = i;
        while (brace < clen && clean[brace] != '{') ++brace;
        if (brace >= clen) break;

        const char *sel_group = clean + i;
        size_t      sel_glen  = brace - i;

        size_t close_brace = brace + 1u;
        int    depth       = 1;
        while (close_brace < clen && depth > 0) {
            if (clean[close_brace] == '{') ++depth;
            else if (clean[close_brace] == '}') --depth;
            if (depth > 0) ++close_brace;
        }
        const char *body_ptr = clean + brace + 1u;
        size_t      body_len = (close_brace > brace + 1u) ? (close_brace - brace - 1u) : 0u;
        i = (close_brace < clen) ? (close_brace + 1u) : clen;

        arcr_str_trim_view(&sel_group, &sel_glen);
        if (sel_glen == 0u) continue;

        if (sel_group[0] == '@') {
            if (sel_glen >= 6u && arcr_str_eq_nocase_n(sel_group, 6u, "@media", 6u)) {
                const char *mq = sel_group + 6u;
                size_t      mql = sel_glen - 6u;
                if (eval_media_query(mq, mql, &sheet->env)) {
                    parse_stylesheet_rules(doc, sheet, body_ptr, body_len, true);
                }
            }
            continue;
        }

        bool collect_vars = false;
        if (arcr_str_eq_nocase_n(sel_group, sel_glen, ":root", 5u) ||
            arcr_str_eq_nocase_n(sel_group, sel_glen, "html", 4u) ||
            arcr_str_eq_nocase_n(sel_group, sel_glen, "body", 4u) ||
            arcr_str_eq_nocase_n(sel_group, sel_glen, "*", 1u)) {
            collect_vars = true;
        }
        /* Also collect --vars if ":root" appears inside a comma-separated group */
        for (size_t k = 0u; k + 5u <= sel_glen && !collect_vars; ++k) {
            if (arcr_str_eq_nocase_n(sel_group + k, 5u, ":root", 5u)) {
                collect_vars = true;
            }
        }

        ArcrCssDecl *decls = parse_declarations(doc, sheet, body_ptr, body_len, collect_vars);
        if (!decls) continue;

        size_t p = 0u;
        while (p < sel_glen) {
            size_t comma = p;
            int bracket = 0;
            int paren   = 0;
            while (comma < sel_glen) {
                if (sel_group[comma] == '[') ++bracket;
                else if (sel_group[comma] == ']' && bracket > 0) --bracket;
                else if (sel_group[comma] == '(') ++paren;
                else if (sel_group[comma] == ')' && paren > 0) --paren;
                else if (sel_group[comma] == ',' && bracket == 0 && paren == 0) break;
                ++comma;
            }
            const char *one  = sel_group + p;
            size_t      olen = comma - p;
            p = (comma < sel_glen) ? (comma + 1u) : sel_glen;
            arcr_str_trim_view(&one, &olen);
            if (olen == 0u) continue;

            ArcrCssRule *rule = (ArcrCssRule *)arcr_arena_calloc(
                &doc->arena, 1u, sizeof(ArcrCssRule), sizeof(void *));
            if (!rule) continue;

            if (!parse_selector_chain(doc->atoms, one, olen, rule)) {
                continue;
            }
            rule->from_media   = in_media;
            rule->source_order = ++sheet->rule_count;
            rule->decls        = decls;
            insert_rule_indexed(sheet, rule);
        }
    }
}

ArcrStylesheet *arcr_css_parse_document_styles_ex(ArcrDocument *doc, const ArcrCssMediaEnv *env) {
    if (!doc) return NULL;
    ArcrStylesheet *sheet = (ArcrStylesheet *)arcr_arena_calloc(
        &doc->arena, 1u, sizeof(ArcrStylesheet), sizeof(void *));
    if (!sheet) return NULL;

    if (env) {
        sheet->env.viewport_width  = (env->viewport_width > 0) ? env->viewport_width : 1280;
        sheet->env.viewport_height = (env->viewport_height > 0) ? env->viewport_height : 800;
        sheet->env.prefers_dark    = env->prefers_dark;
    } else {
        sheet->env.viewport_width  = 1280;
        sheet->env.viewport_height = 800;
        sheet->env.prefers_dark    = false;
    }

    for (ArcrStyleBlock *sb = doc->style_blocks_head; sb != NULL; sb = sb->next) {
        if (!sb->css_text || sb->css_len == 0u) continue;
        char *clean = (char *)arcr_arena_alloc(&doc->arena, sb->css_len + 1u, 1u);
        if (!clean) continue;
        size_t clen = strip_css_comments(sb->css_text, sb->css_len, clean);
        parse_stylesheet_rules(doc, sheet, clean, clen, false);
    }
    return sheet;
}

ArcrStylesheet *arcr_css_parse_document_styles(ArcrDocument *doc) {
    return arcr_css_parse_document_styles_ex(doc, NULL);
}

static bool is_first_element_child(const ArcrDomNode *el) {
    if (!el || !el->parent) return false;
    for (const ArcrDomNode *ch = el->parent->first_child; ch != NULL; ch = ch->next_sibling) {
        if (ch->kind == ARCR_NODE_ELEMENT) return ch == el;
    }
    return false;
}

static bool is_last_element_child(const ArcrDomNode *el) {
    if (!el || !el->parent) return false;
    for (const ArcrDomNode *ch = el->next_sibling; ch != NULL; ch = ch->next_sibling) {
        if (ch->kind == ARCR_NODE_ELEMENT) return false;
    }
    return true;
}

static const ArcrDomNode *prev_element_sibling(const ArcrDomNode *el) {
    if (!el || !el->parent) return NULL;
    const ArcrDomNode *prev = NULL;
    for (const ArcrDomNode *ch = el->parent->first_child; ch != NULL; ch = ch->next_sibling) {
        if (ch == el) return prev;
        if (ch->kind == ARCR_NODE_ELEMENT) prev = ch;
    }
    return NULL;
}

static bool compound_matches(const CssCompoundSel *sel, const ArcrDomNode *el, bool check_state) {
    if (!el || el->kind != ARCR_NODE_ELEMENT || sel->impossible) return false;
    if (sel->tag != ARCR_ATOM_NULL && el->tag != sel->tag) return false;
    if (sel->id != ARCR_ATOM_NULL && el->id != sel->id) return false;
    for (uint8_t c = 0u; c < sel->class_count; ++c) {
        if (!arcr_dom_has_class(el, sel->classes[c])) return false;
    }
    if (check_state && (el->state_flags & sel->req_state) != sel->req_state) {
        return false;
    }
    if (sel->req_root && (!el->parent || el->parent->kind != ARCR_NODE_DOCUMENT)) {
        return false;
    }
    if (sel->req_first_child && !is_first_element_child(el)) return false;
    if (sel->req_last_child && !is_last_element_child(el)) return false;

    for (uint8_t a = 0u; a < sel->attr_count; ++a) {
        const char *v = arcr_dom_get_attr(el, sel->attrs[a].name);
        if (!v) return false;
        if (sel->attrs[a].has_val) {
            if (strlen(v) != sel->attrs[a].val_len ||
                memcmp(v, sel->attrs[a].val, sel->attrs[a].val_len) != 0) {
                return false;
            }
        }
    }
    return true;
}

static bool chain_matches_at(const ArcrCssRule *rule, uint8_t idx, const ArcrDomNode *el,
                             bool check_state) {
    if (!compound_matches(&rule->steps[idx], el, check_state)) return false;
    if (idx == 0u) return true;

    CssCombinator comb = rule->steps[idx - 1u].comb_to_next;
    if (comb == CSS_COMB_CHILD) {
        if (!el->parent || el->parent->kind != ARCR_NODE_ELEMENT) return false;
        return chain_matches_at(rule, idx - 1u, el->parent, check_state);
    }
    if (comb == CSS_COMB_ADJACENT) {
        const ArcrDomNode *prev = prev_element_sibling(el);
        if (!prev) return false;
        return chain_matches_at(rule, idx - 1u, prev, check_state);
    }
    for (const ArcrDomNode *anc = el->parent; anc != NULL; anc = anc->parent) {
        if (anc->kind == ARCR_NODE_ELEMENT &&
            chain_matches_at(rule, idx - 1u, anc, check_state)) {
            return true;
        }
    }
    return false;
}

static bool rule_matches_element(const ArcrCssRule *rule, const ArcrDomNode *el,
                                 bool *out_has_potential_state) {
    if (rule->step_count > 1u && rule->ancestor_bloom_req != 0ULL) {
        if ((el->ancestor_bloom & rule->ancestor_bloom_req) != rule->ancestor_bloom_req) {
            return false;
        }
    }
    if (rule->uses_state_pseudo && out_has_potential_state) {
        if (chain_matches_at(rule, rule->step_count - 1u, el, false)) {
            *out_has_potential_state = true;
        } else {
            return false;
        }
    }
    return chain_matches_at(rule, rule->step_count - 1u, el, true);
}

/* Expands all `var(--custom, fallback)` occurrences inside a value string */
static size_t expand_css_vars(const ArcrDocument *doc, const ArcrStylesheet *sheet,
                              const char *src, size_t len,
                              char *out_buf, size_t out_cap,
                              bool *used_var, int depth) {
    if (!out_buf || out_cap == 0u) return 0u;
    if (depth > 4) {
        size_t n = ARCR_MIN(len, out_cap - 1u);
        memcpy(out_buf, src, n);
        out_buf[n] = '\0';
        return n;
    }

    size_t i   = 0u;
    size_t pos = 0u;
    while (i < len && pos + 1u < out_cap) {
        if (i + 5u < len && arcr_str_eq_nocase_n(src + i, 4u, "var(", 4u)) {
            size_t j = i + 4u;
            int paren = 1;
            while (j < len && paren > 0) {
                if (src[j] == '(') ++paren;
                else if (src[j] == ')') --paren;
                if (paren > 0) ++j;
            }
            if (j < len && src[j] == ')') {
                if (used_var) *used_var = true;
                const char *inner = src + i + 4u;
                size_t      ilen  = j - (i + 4u);

                size_t comma = 0u;
                int p2 = 0;
                while (comma < ilen) {
                    if (inner[comma] == '(') ++p2;
                    else if (inner[comma] == ')' && p2 > 0) --p2;
                    else if (inner[comma] == ',' && p2 == 0) break;
                    ++comma;
                }

                const char *vname = inner;
                size_t      vnlen = comma;
                arcr_str_trim_view(&vname, &vnlen);

                const char *rep     = NULL;
                size_t      rep_len = 0u;
                if (vnlen > 0u && sheet) {
                    for (const ArcrCssVar *v = sheet->vars_head; v != NULL; v = v->next) {
                        const char *vn = arcr_atom_cstr(doc->atoms, v->name);
                        if (strlen(vn) == vnlen && memcmp(vn, vname, vnlen) == 0) {
                            rep     = v->val;
                            rep_len = v->val_len;
                            break;
                        }
                    }
                }
                if (!rep && comma < ilen) {
                    rep     = inner + comma + 1u;
                    rep_len = ilen - comma - 1u;
                    arcr_str_trim_view(&rep, &rep_len);
                }

                if (rep && rep_len > 0u) {
                    char sub[256];
                    size_t sl = expand_css_vars(doc, sheet, rep, rep_len,
                                                sub, sizeof(sub), used_var, depth + 1);
                    size_t copy_n = ARCR_MIN(sl, out_cap - 1u - pos);
                    memcpy(out_buf + pos, sub, copy_n);
                    pos += copy_n;
                }
                i = j + 1u;
                continue;
            }
        }
        out_buf[pos++] = src[i++];
    }
    out_buf[pos] = '\0';
    return pos;
}

static int32_t parse_single_length_px(const char *s, size_t len,
                                      int32_t base_font_px, int32_t pct_ref_px,
                                      const ArcrCssMediaEnv *env, uint32_t *deps) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return 0;

    int64_t milli = 0;
    size_t  num_end = 0u;
    if (!parse_fixed_1000(s, len, &milli, &num_end)) return 0;

    const char *unit = s + num_end;
    size_t      ulen = len - num_end;
    arcr_str_trim_view(&unit, &ulen);

    if (ulen == 0u || arcr_str_eq_nocase_n(unit, ulen, "px", 2u)) {
        return (int32_t)((milli + (milli >= 0 ? 500 : -500)) / 1000);
    }
    if (arcr_str_eq_nocase_n(unit, ulen, "rem", 3u)) {
        return (int32_t)((milli * 16 + (milli >= 0 ? 500 : -500)) / 1000);
    }
    if (arcr_str_eq_nocase_n(unit, ulen, "em", 2u) ||
        arcr_str_eq_nocase_n(unit, ulen, "ch", 2u)) {
        if (deps) *deps |= ARCR_STYLE_DEP_EM;
        int32_t ref = arcr_str_eq_nocase_n(unit, ulen, "ch", 2u)
                          ? ((base_font_px * 6) / 10)
                          : base_font_px;
        return (int32_t)((milli * ref + (milli >= 0 ? 500 : -500)) / 1000);
    }
    if (arcr_str_eq_nocase_n(unit, ulen, "vw", 2u)) {
        if (deps) *deps |= ARCR_STYLE_DEP_MEDIA;
        int32_t vw = env ? env->viewport_width : 1280;
        return (int32_t)((milli * vw + 50000) / 100000);
    }
    if (arcr_str_eq_nocase_n(unit, ulen, "vh", 2u)) {
        if (deps) *deps |= ARCR_STYLE_DEP_MEDIA;
        int32_t vh = env ? env->viewport_height : 800;
        return (int32_t)((milli * vh + 50000) / 100000);
    }
    if (arcr_str_eq_nocase_n(unit, ulen, "pt", 2u)) {
        return (int32_t)((milli * 4 + 1500) / 3000);
    }
    if (arcr_str_eq_nocase_n(unit, ulen, "%", 1u)) {
        return (int32_t)((milli * pct_ref_px + 50000) / 100000);
    }
    return (int32_t)(milli / 1000);
}

static int32_t parse_length_or_calc_px(const char *s, size_t len,
                                       int32_t base_font_px, int32_t pct_ref_px,
                                       const ArcrCssMediaEnv *env, uint32_t *deps) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return 0;

    if (len > 6u && arcr_str_eq_nocase_n(s, 5u, "calc(", 5u) && s[len - 1u] == ')') {
        const char *expr = s + 5u;
        size_t      elen = len - 6u;
        arcr_str_trim_view(&expr, &elen);

        int32_t total = 0;
        char    op    = '+';
        size_t  i     = 0u;
        while (i < elen) {
            while (i < elen && is_css_ws(expr[i])) ++i;
            if (i >= elen) break;

            size_t tstart = i;
            if (expr[i] == '+' || expr[i] == '-') ++i;
            while (i < elen && !is_css_ws(expr[i])) ++i;
            int32_t term = parse_single_length_px(expr + tstart, i - tstart,
                                                  base_font_px, pct_ref_px, env, deps);
            if (op == '+')      total += term;
            else if (op == '-') total -= term;

            while (i < elen && is_css_ws(expr[i])) ++i;
            if (i < elen && (expr[i] == '+' || expr[i] == '-')) {
                op = expr[i++];
            } else if (i < elen) {
                break;
            }
        }
        return total;
    }

    return parse_single_length_px(s, len, base_font_px, pct_ref_px, env, deps);
}

/* Parses explicit sizing spec: 0 = auto/none, >0 = px, <0 = negated percentage (e.g. -50 = 50%) */
static int32_t parse_width_spec_value(const char *s, size_t len, int32_t font_px,
                                      const ArcrCssMediaEnv *env, uint32_t *deps) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u || arcr_str_eq_nocase_n(s, len, "auto", 4u) ||
        arcr_str_eq_nocase_n(s, len, "none", 4u)) {
        return 0;
    }
    if (s[len - 1u] == '%' && !(len > 5u && arcr_str_eq_nocase_n(s, 5u, "calc(", 5u))) {
        int64_t m = 0;
        if (parse_fixed_1000(s, len - 1u, &m, NULL) && m > 0) {
            int32_t pct = (int32_t)((m + 500) / 1000);
            if (pct > 0 && pct <= 1000) return -pct;
        }
        return 0;
    }
    int32_t vpw = env ? env->viewport_width : 1280;
    int32_t px  = parse_length_or_calc_px(s, len, font_px, vpw, env, deps);
    return (px > 0) ? px : 0;
}

static void parse_box_shorthand(const char *s, size_t len, int32_t font_px,
                                const ArcrCssMediaEnv *env,
                                ArcrEdgeSizes *edges, uint32_t *deps,
                                bool *out_left_auto, bool *out_right_auto) {
    int32_t vals[4]  = {0, 0, 0, 0};
    bool    autos[4] = {false, false, false, false};
    int     cnt      = 0;
    size_t  i        = 0u;

    while (i < len && cnt < 4) {
        while (i < len && is_css_ws(s[i])) ++i;
        if (i >= len) break;
        size_t start = i;
        int paren = 0;
        while (i < len && !(paren == 0 && is_css_ws(s[i]))) {
            if (s[i] == '(') ++paren;
            else if (s[i] == ')' && paren > 0) --paren;
            ++i;
        }
        if (i > start) {
            const char *tk = s + start;
            size_t      tl = i - start;
            if (arcr_str_eq_nocase_n(tk, tl, "auto", 4u)) {
                autos[cnt] = true;
                vals[cnt]  = 0;
            } else {
                vals[cnt] = parse_length_or_calc_px(tk, tl, font_px, 0, env, deps);
            }
            ++cnt;
        }
    }

    bool a_left = false, a_right = false;
    if (cnt == 1) {
        edges->top = edges->right = edges->bottom = edges->left = vals[0];
        a_left = a_right = autos[0];
    } else if (cnt == 2) {
        edges->top = edges->bottom = vals[0];
        edges->right = edges->left = vals[1];
        a_left = a_right = autos[1];
    } else if (cnt == 3) {
        edges->top = vals[0];
        edges->right = edges->left = vals[1];
        edges->bottom = vals[2];
        a_left = a_right = autos[1];
    } else if (cnt == 4) {
        edges->top    = vals[0];
        edges->right  = vals[1];
        edges->bottom = vals[2];
        edges->left   = vals[3];
        a_right       = autos[1];
        a_left        = autos[3];
    }
    if (out_left_auto)  *out_left_auto  = a_left;
    if (out_right_auto) *out_right_auto = a_right;
}

static int32_t parse_grid_columns(const char *s, size_t len, const ArcrCssMediaEnv *env) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u || arcr_str_eq_nocase_n(s, len, "none", 4u)) return 0;

    if (len > 8u && arcr_str_eq_nocase_n(s, 7u, "repeat(", 7u) && s[len - 1u] == ')') {
        const char *inner = s + 7u;
        size_t      ilen  = len - 8u;
        size_t      comma = 0u;
        while (comma < ilen && inner[comma] != ',') ++comma;
        const char *first = inner;
        size_t      flen  = comma;
        arcr_str_trim_view(&first, &flen);

        int32_t n = 0;
        if (arcr_parse_i32(first, flen, &n) && n >= 1) {
            return ARCR_CLAMP(n, 1, 12);
        }
        /* Handle repeat(auto-fit / auto-fill, minmax(220px, 1fr)) */
        if (arcr_str_eq_nocase_n(first, flen, "auto-fit", 8u) ||
            arcr_str_eq_nocase_n(first, flen, "auto-fill", 9u)) {
            int32_t min_track = 220;
            if (comma < ilen) {
                const char *rest = inner + comma + 1u;
                size_t      rlen = ilen - comma - 1u;
                arcr_str_trim_view(&rest, &rlen);
                if (rlen > 8u && arcr_str_eq_nocase_n(rest, 7u, "minmax(", 7u)) {
                    const char *mp = rest + 7u;
                    size_t      ml = rlen - 8u;
                    size_t      c2 = 0u;
                    while (c2 < ml && mp[c2] != ',') ++c2;
                    int32_t parsed = parse_media_px(mp, c2);
                    if (parsed >= 60) min_track = parsed;
                }
            }
            int32_t vpw = env ? env->viewport_width : 1280;
            int32_t cols = ARCR_MAX(1, (vpw - 40) / (min_track + 16));
            return ARCR_CLAMP(cols, 1, 12);
        }
    }

    /* Count top-level space-separated track definitions: e.g. "240px 1fr 1fr" */
    int32_t tracks = 0;
    size_t  i      = 0u;
    while (i < len) {
        while (i < len && is_css_ws(s[i])) ++i;
        if (i >= len) break;
        int paren = 0;
        while (i < len && !(paren == 0 && is_css_ws(s[i]))) {
            if (s[i] == '(') ++paren;
            else if (s[i] == ')' && paren > 0) --paren;
            ++i;
        }
        ++tracks;
    }
    return ARCR_CLAMP(tracks, 0, 12);
}

static void parse_box_shadow(const char *s, size_t len, int32_t font_px,
                             const ArcrCssMediaEnv *env, ArcrComputedStyle *st) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u || arcr_str_eq_nocase_n(s, len, "none", 4u)) {
        st->shadow_argb     = 0u;
        st->shadow_blur     = 0;
        st->shadow_offset_y = 0;
        return;
    }

    int32_t  lengths[4] = {0, 4, 12, 0};
    int      lcount     = 0;
    uint32_t color      = 0x22000000u;
    size_t   i          = 0u;

    while (i < len) {
        while (i < len && (is_css_ws(s[i]) || s[i] == ',')) ++i;
        if (i >= len) break;
        if (s[i] == ',') break; /* Use primary shadow layer */

        size_t start = i;
        int paren = 0;
        while (i < len) {
            if (s[i] == '(') ++paren;
            else if (s[i] == ')' && paren > 0) --paren;
            else if (paren == 0 && (is_css_ws(s[i]) || s[i] == ',')) break;
            ++i;
        }
        const char *tk = s + start;
        size_t      tl = i - start;
        if (arcr_str_eq_nocase_n(tk, tl, "inset", 5u)) continue;

        uint32_t parsed_c = 0u;
        if (arcr_css_parse_color(tk, tl, &parsed_c)) {
            color = parsed_c;
        } else if (lcount < 4) {
            lengths[lcount++] = parse_single_length_px(tk, tl, font_px, 0, env, &st->dependency_flags);
        }
    }

    st->shadow_offset_y = (lcount >= 2) ? lengths[1] : 2;
    st->shadow_blur     = ARCR_CLAMP((lcount >= 3) ? lengths[2] : 8, 1, 48);
    st->shadow_argb     = color;
}

static bool str_contains_nocase(const char *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0u || hlen < nlen) return false;
    for (size_t i = 0u; i + nlen <= hlen; ++i) {
        if (arcr_str_eq_nocase_n(hay + i, nlen, needle, nlen)) return true;
    }
    return false;
}

static uint32_t modulate_alpha(uint32_t argb, int32_t opacity_milli) {
    uint32_t a = (argb >> 24) & 0xFFu;
    if (a == 0u) return argb;
    uint32_t na = (uint32_t)ARCR_CLAMP(((int64_t)a * opacity_milli + 500) / 1000, 0, 255);
    return (na << 24) | (argb & 0x00FFFFFFu);
}

static void apply_ua_defaults(const ArcrDocument *doc, const ArcrDomNode *el,
                              const ArcrComputedStyle *parent, ArcrComputedStyle *st) {
    memset(st, 0, sizeof(*st));
    st->display             = ARCR_DISPLAY_BLOCK;
    st->color_argb          = parent ? parent->color_argb : 0xFF111827u;
    st->bg_argb             = 0x00000000u;
    st->border_argb         = 0xFFD1D5DBu;
    st->font_px             = parent ? parent->font_px : 15;
    st->line_height_pct     = parent ? parent->line_height_pct : 145;
    st->font_bold           = parent ? parent->font_bold : false;
    st->font_monospace      = parent ? parent->font_monospace : false;
    st->underline           = parent ? parent->underline : false;
    st->word_break_keep_all = parent ? parent->word_break_keep_all : false;
    st->white_space_pre     = parent ? parent->white_space_pre : false;
    st->text_align          = parent ? parent->text_align : ARCR_TEXT_ALIGN_LEFT;

    const char *t = arcr_atom_cstr(doc->atoms, el->tag);
    if (arcr_str_eq_nocase(t, "head") || arcr_str_eq_nocase(t, "style") ||
        arcr_str_eq_nocase(t, "script") || arcr_str_eq_nocase(t, "meta") ||
        arcr_str_eq_nocase(t, "title") || arcr_str_eq_nocase(t, "link") ||
        arcr_str_eq_nocase(t, "noscript") || arcr_str_eq_nocase(t, "template")) {
        st->display = ARCR_DISPLAY_NONE;
    } else if (arcr_str_eq_nocase(t, "body")) {
        st->bg_argb = 0xFFFFFFFFu;
        st->padding = (ArcrEdgeSizes){16, 20, 16, 20};
    } else if (arcr_str_eq_nocase(t, "h1")) {
        st->font_px       = 26;
        st->font_bold     = true;
        st->line_height_pct = 125;
        st->margin.bottom = 10;
    } else if (arcr_str_eq_nocase(t, "h2")) {
        st->font_px       = 20;
        st->font_bold     = true;
        st->line_height_pct = 130;
        st->margin.top    = 12;
        st->margin.bottom = 8;
    } else if (arcr_str_eq_nocase(t, "h3")) {
        st->font_px       = 17;
        st->font_bold     = true;
        st->margin.top    = 10;
        st->margin.bottom = 6;
    } else if (arcr_str_eq_nocase(t, "h4") || arcr_str_eq_nocase(t, "h5") ||
               arcr_str_eq_nocase(t, "h6")) {
        st->font_px       = arcr_str_eq_nocase(t, "h4") ? 15 : 13;
        st->font_bold     = true;
        st->margin.top    = 8;
        st->margin.bottom = 4;
    } else if (arcr_str_eq_nocase(t, "p")) {
        st->margin.bottom = 10;
    } else if (arcr_str_eq_nocase(t, "blockquote")) {
        st->margin        = (ArcrEdgeSizes){6, 0, 10, 0};
        st->padding       = (ArcrEdgeSizes){8, 14, 8, 14};
        st->border.left   = 4;
        st->border_argb   = 0xFF94A3B8u;
        st->bg_argb       = 0xFFF8FAFCu;
    } else if (arcr_str_eq_nocase(t, "ul") || arcr_str_eq_nocase(t, "ol")) {
        st->margin.bottom = 10;
        st->padding.left  = 22;
    } else if (arcr_str_eq_nocase(t, "li")) {
        st->margin.bottom = 4;
    } else if (arcr_str_eq_nocase(t, "hr")) {
        st->margin = (ArcrEdgeSizes){10, 0, 10, 0};
    } else if (arcr_str_eq_nocase(t, "span") || arcr_str_eq_nocase(t, "em") ||
               arcr_str_eq_nocase(t, "i") || arcr_str_eq_nocase(t, "label") ||
               arcr_str_eq_nocase(t, "abbr") || arcr_str_eq_nocase(t, "cite") ||
               arcr_str_eq_nocase(t, "time") || arcr_str_eq_nocase(t, "sub") ||
               arcr_str_eq_nocase(t, "sup")) {
        st->display = ARCR_DISPLAY_INLINE;
    } else if (arcr_str_eq_nocase(t, "small")) {
        st->display = ARCR_DISPLAY_INLINE;
        st->font_px = ARCR_MAX(11, (st->font_px * 85) / 100);
    } else if (arcr_str_eq_nocase(t, "u")) {
        st->display   = ARCR_DISPLAY_INLINE;
        st->underline = true;
    } else if (arcr_str_eq_nocase(t, "mark")) {
        st->display    = ARCR_DISPLAY_INLINE;
        st->bg_argb    = 0xFFFEF08Au;
        st->color_argb = 0xFF111827u;
        st->padding    = (ArcrEdgeSizes){1, 3, 1, 3};
    } else if (arcr_str_eq_nocase(t, "strong") || arcr_str_eq_nocase(t, "b")) {
        st->display   = ARCR_DISPLAY_INLINE;
        st->font_bold = true;
    } else if (arcr_str_eq_nocase(t, "a")) {
        st->display    = ARCR_DISPLAY_INLINE;
        st->color_argb = (el->state_flags & ARCR_STATE_VISITED) ? 0xFF7C3AEDu : 0xFF2563EBu;
        st->underline  = true;
    } else if (arcr_str_eq_nocase(t, "code") || arcr_str_eq_nocase(t, "kbd") ||
               arcr_str_eq_nocase(t, "samp")) {
        st->display        = ARCR_DISPLAY_INLINE;
        st->font_monospace = true;
        if (!parent || !parent->white_space_pre) {
            st->bg_argb       = 0xFFF1F5F9u;
            st->color_argb    = 0xFF0F172Au;
            st->padding       = (ArcrEdgeSizes){1, 5, 1, 5};
            st->border_radius = 4;
        }
    } else if (arcr_str_eq_nocase(t, "pre")) {
        st->display         = ARCR_DISPLAY_BLOCK;
        st->font_monospace  = true;
        st->white_space_pre = true;
        st->font_px         = 13;
        st->bg_argb         = 0xFFF8FAFCu;
        st->border          = (ArcrEdgeSizes){1, 1, 1, 1};
        st->border_argb     = 0xFFE2E8F0u;
        st->border_radius   = 6;
        st->padding         = (ArcrEdgeSizes){10, 12, 10, 12};
        st->margin.bottom   = 10;
    } else if (arcr_str_eq_nocase(t, "table")) {
        st->display       = ARCR_DISPLAY_TABLE;
        st->border_argb   = 0xFFE2E8F0u;
        st->margin.bottom = 12;
    } else if (arcr_str_eq_nocase(t, "tr")) {
        st->display = ARCR_DISPLAY_TABLE_ROW;
    } else if (arcr_str_eq_nocase(t, "td") || arcr_str_eq_nocase(t, "th")) {
        st->display     = ARCR_DISPLAY_TABLE_CELL;
        st->padding     = (ArcrEdgeSizes){7, 10, 7, 10};
        st->border      = (ArcrEdgeSizes){1, 1, 1, 1};
        st->border_argb = 0xFFE2E8F0u;
        if (arcr_str_eq_nocase(t, "th")) {
            st->font_bold = true;
            st->bg_argb   = 0xFFF8FAFCu;
        }
    } else if (arcr_str_eq_nocase(t, "img") || arcr_str_eq_nocase(t, "canvas")) {
        st->display = ARCR_DISPLAY_INLINE_BLOCK;
    } else if (arcr_str_eq_nocase(t, "button")) {
        st->display       = ARCR_DISPLAY_INLINE_BLOCK;
        st->bg_argb       = (el->state_flags & ARCR_STATE_HOVER) ? 0xFFDBEAFEu : 0xFFEFF6FFu;
        st->color_argb    = 0xFF1E3A8Au;
        st->border        = (ArcrEdgeSizes){1, 1, 1, 1};
        st->border_argb   = 0xFF93C5FDu;
        st->border_radius = 6;
        st->padding       = (ArcrEdgeSizes){6, 12, 6, 12};
        st->font_bold     = true;
        st->text_align    = ARCR_TEXT_ALIGN_CENTER;
    } else if (arcr_str_eq_nocase(t, "input")) {
        const char *tp = arcr_dom_get_attr_cstr(doc, el, "type");
        if (tp && arcr_str_eq_nocase(tp, "hidden")) {
            st->display = ARCR_DISPLAY_NONE;
        } else if (tp && (arcr_str_eq_nocase(tp, "checkbox") || arcr_str_eq_nocase(tp, "radio"))) {
            st->display       = ARCR_DISPLAY_INLINE_BLOCK;
            st->bg_argb       = 0xFFFFFFFFu;
            st->border        = (ArcrEdgeSizes){1, 1, 1, 1};
            st->border_argb   = 0xFF94A3B8u;
            st->border_radius = arcr_str_eq_nocase(tp, "radio") ? 9 : 4;
        } else if (tp && (arcr_str_eq_nocase(tp, "submit") || arcr_str_eq_nocase(tp, "button") ||
                          arcr_str_eq_nocase(tp, "reset"))) {
            st->display       = ARCR_DISPLAY_INLINE_BLOCK;
            st->bg_argb       = 0xFF2563EBu;
            st->color_argb    = 0xFFFFFFFFu;
            st->border        = (ArcrEdgeSizes){1, 1, 1, 1};
            st->border_argb   = 0xFF1D4ED8u;
            st->border_radius = 6;
            st->padding       = (ArcrEdgeSizes){6, 14, 6, 14};
            st->font_bold     = true;
            st->text_align    = ARCR_TEXT_ALIGN_CENTER;
        } else {
            st->display       = ARCR_DISPLAY_INLINE_BLOCK;
            st->bg_argb       = 0xFFFFFFFFu;
            st->color_argb    = 0xFF0F172Au;
            st->border        = (ArcrEdgeSizes){1, 1, 1, 1};
            st->border_argb   = (el->state_flags & ARCR_STATE_FOCUS) ? 0xFF2563EBu : 0xFFCBD5E1u;
            st->border_radius = 6;
            st->padding       = (ArcrEdgeSizes){6, 10, 6, 10};
        }
    } else if (arcr_str_eq_nocase(t, "textarea")) {
        st->display         = ARCR_DISPLAY_INLINE_BLOCK;
        st->bg_argb         = 0xFFFFFFFFu;
        st->color_argb      = 0xFF0F172Au;
        st->border          = (ArcrEdgeSizes){1, 1, 1, 1};
        st->border_argb     = (el->state_flags & ARCR_STATE_FOCUS) ? 0xFF2563EBu : 0xFFCBD5E1u;
        st->border_radius   = 6;
        st->padding         = (ArcrEdgeSizes){8, 10, 8, 10};
        st->white_space_pre = true;
    }
}

static void parse_border_side_shorthand(const char *val, size_t vlen,
                                        int32_t font_px, const ArcrCssMediaEnv *env,
                                        int32_t *out_bw, uint32_t *inout_color,
                                        uint32_t current_color, uint32_t *deps) {
    arcr_str_trim_view(&val, &vlen);
    if (arcr_str_eq_nocase_n(val, vlen, "none", 4u) ||
        arcr_str_eq_nocase_n(val, vlen, "0", 1u)) {
        *out_bw = 0;
        return;
    }

    int32_t bw    = 1;
    size_t  k     = 0u;
    bool    got_w = false;
    while (k < vlen) {
        while (k < vlen && is_css_ws(val[k])) ++k;
        if (k >= vlen) break;
        size_t st_tok = k;
        int paren = 0;
        while (k < vlen && !(paren == 0 && is_css_ws(val[k]))) {
            if (val[k] == '(') ++paren;
            else if (val[k] == ')' && paren > 0) --paren;
            ++k;
        }
        const char *tk = val + st_tok;
        size_t      tl = k - st_tok;

        if (arcr_str_eq_nocase_n(tk, tl, "none", 4u) ||
            arcr_str_eq_nocase_n(tk, tl, "hidden", 6u)) {
            *out_bw = 0;
            return;
        }
        if (arcr_str_eq_nocase_n(tk, tl, "solid", 5u) ||
            arcr_str_eq_nocase_n(tk, tl, "dashed", 6u) ||
            arcr_str_eq_nocase_n(tk, tl, "dotted", 6u) ||
            arcr_str_eq_nocase_n(tk, tl, "double", 6u)) {
            continue;
        }
        if (arcr_str_eq_nocase_n(tk, tl, "currentColor", 12u)) {
            *inout_color = current_color;
            continue;
        }
        uint32_t pc = 0u;
        if (arcr_css_parse_color(tk, tl, &pc)) {
            *inout_color = pc;
        } else if (!got_w) {
            bw    = ARCR_MAX(0, parse_single_length_px(tk, tl, font_px, 0, env, deps));
            got_w = true;
        }
    }
    *out_bw = bw;
}

static void apply_declarations_pass(const ArcrDocument *doc, const ArcrStylesheet *sheet,
                                    const ArcrCssDecl *decls, ArcrComputedStyle *st,
                                    bool important_pass) {
    const ArcrCssMediaEnv *env = sheet ? &sheet->env : NULL;
    int32_t vpw = env ? env->viewport_width : 1280;

    for (const ArcrCssDecl *d = decls; d != NULL; d = d->next) {
        if (d->important != important_pass) continue;

        char   expanded[384];
        bool   used_var = false;
        size_t vlen     = expand_css_vars(doc, sheet, d->val_str, d->val_len,
                                          expanded, sizeof(expanded), &used_var, 0);
        const char *val = expanded;
        arcr_str_trim_view(&val, &vlen);
        if (used_var) st->dependency_flags |= ARCR_STYLE_DEP_VAR;
        if (vlen == 0u) continue;

        const char *prop = arcr_atom_cstr(doc->atoms, d->prop);

        if (arcr_str_eq_nocase(prop, "color")) {
            if (!arcr_str_eq_nocase_n(val, vlen, "inherit", 7u)) {
                arcr_css_parse_color(val, vlen, &st->color_argb);
            }
        } else if (arcr_str_eq_nocase(prop, "background-color") ||
                   arcr_str_eq_nocase(prop, "background")) {
            parse_background_color_smart(val, vlen, st->color_argb, &st->bg_argb);
        } else if (arcr_str_eq_nocase(prop, "border-color")) {
            if (arcr_str_eq_nocase_n(val, vlen, "currentColor", 12u)) {
                st->border_argb = st->color_argb;
            } else {
                arcr_css_parse_color(val, vlen, &st->border_argb);
            }
        } else if (arcr_str_eq_nocase(prop, "display")) {
            if (arcr_str_eq_nocase_n(val, vlen, "none", 4u)) {
                st->display = ARCR_DISPLAY_NONE;
            } else if (arcr_str_eq_nocase_n(val, vlen, "inline", 6u)) {
                st->display = ARCR_DISPLAY_INLINE;
            } else if (arcr_str_eq_nocase_n(val, vlen, "inline-block", 12u)) {
                st->display = ARCR_DISPLAY_INLINE_BLOCK;
            } else if (arcr_str_eq_nocase_n(val, vlen, "flex", 4u) ||
                       arcr_str_eq_nocase_n(val, vlen, "inline-flex", 11u)) {
                st->display = ARCR_DISPLAY_FLEX;
            } else if (arcr_str_eq_nocase_n(val, vlen, "grid", 4u) ||
                       arcr_str_eq_nocase_n(val, vlen, "inline-grid", 11u)) {
                st->display = ARCR_DISPLAY_GRID;
            } else if (arcr_str_eq_nocase_n(val, vlen, "table", 5u)) {
                st->display = ARCR_DISPLAY_TABLE;
            } else if (arcr_str_eq_nocase_n(val, vlen, "table-row", 9u)) {
                st->display = ARCR_DISPLAY_TABLE_ROW;
            } else if (arcr_str_eq_nocase_n(val, vlen, "table-cell", 10u)) {
                st->display = ARCR_DISPLAY_TABLE_CELL;
            } else {
                st->display = ARCR_DISPLAY_BLOCK;
            }
        } else if (arcr_str_eq_nocase(prop, "flex-direction")) {
            if (arcr_str_starts_with_nocase(val, "column")) st->flex_direction = ARCR_FLEX_COLUMN;
            else                                            st->flex_direction = ARCR_FLEX_ROW;
        } else if (arcr_str_eq_nocase(prop, "flex-wrap")) {
            st->flex_wrap = arcr_str_starts_with_nocase(val, "wrap");
        } else if (arcr_str_eq_nocase(prop, "flex-flow")) {
            if (str_contains_nocase(val, vlen, "column")) st->flex_direction = ARCR_FLEX_COLUMN;
            else if (str_contains_nocase(val, vlen, "row")) st->flex_direction = ARCR_FLEX_ROW;
            if (str_contains_nocase(val, vlen, "wrap") && !str_contains_nocase(val, vlen, "nowrap")) {
                st->flex_wrap = true;
            }
        } else if (arcr_str_eq_nocase(prop, "justify-content")) {
            if (arcr_str_eq_nocase_n(val, vlen, "center", 6u)) {
                st->justify_content = ARCR_JUSTIFY_CENTER;
            } else if (arcr_str_eq_nocase_n(val, vlen, "flex-end", 8u) ||
                       arcr_str_eq_nocase_n(val, vlen, "end", 3u) ||
                       arcr_str_eq_nocase_n(val, vlen, "right", 5u)) {
                st->justify_content = ARCR_JUSTIFY_END;
            } else if (arcr_str_eq_nocase_n(val, vlen, "space-between", 13u)) {
                st->justify_content = ARCR_JUSTIFY_SPACE_BETWEEN;
            } else if (arcr_str_eq_nocase_n(val, vlen, "space-around", 12u) ||
                       arcr_str_eq_nocase_n(val, vlen, "space-evenly", 12u)) {
                st->justify_content = ARCR_JUSTIFY_SPACE_AROUND;
            } else {
                st->justify_content = ARCR_JUSTIFY_START;
            }
        } else if (arcr_str_eq_nocase(prop, "align-items")) {
            if (arcr_str_eq_nocase_n(val, vlen, "center", 6u)) {
                st->align_items = ARCR_ALIGN_CENTER;
            } else if (arcr_str_eq_nocase_n(val, vlen, "flex-start", 10u) ||
                       arcr_str_eq_nocase_n(val, vlen, "start", 5u) ||
                       arcr_str_eq_nocase_n(val, vlen, "baseline", 8u)) {
                st->align_items = ARCR_ALIGN_START;
            } else if (arcr_str_eq_nocase_n(val, vlen, "flex-end", 8u) ||
                       arcr_str_eq_nocase_n(val, vlen, "end", 3u)) {
                st->align_items = ARCR_ALIGN_END;
            } else {
                st->align_items = ARCR_ALIGN_STRETCH;
            }
        } else if (arcr_str_eq_nocase(prop, "gap") || arcr_str_eq_nocase(prop, "grid-gap") ||
                   arcr_str_eq_nocase(prop, "column-gap") || arcr_str_eq_nocase(prop, "row-gap")) {
            size_t sp = 0u;
            while (sp < vlen && !is_css_ws(val[sp])) ++sp;
            st->gap_px = ARCR_MAX(0, parse_length_or_calc_px(val, sp, st->font_px, vpw, env,
                                                             &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "flex-grow")) {
            int32_t fg = 0;
            if (arcr_parse_i32(val, vlen, &fg)) st->flex_grow = ARCR_MAX(0, fg);
        } else if (arcr_str_eq_nocase(prop, "flex")) {
            if (arcr_str_eq_nocase_n(val, vlen, "none", 4u)) {
                st->flex_grow = 0;
            } else if (arcr_str_eq_nocase_n(val, vlen, "auto", 4u)) {
                st->flex_grow = 1;
            } else {
                size_t sp = 0u;
                while (sp < vlen && !is_css_ws(val[sp])) ++sp;
                int32_t fg = 0;
                if (arcr_parse_i32(val, sp, &fg)) st->flex_grow = ARCR_MAX(0, fg);
                /* Check optional 3rd flex-basis token */
                size_t k = sp;
                while (k < vlen && is_css_ws(val[k])) ++k;
                while (k < vlen && !is_css_ws(val[k])) ++k;
                while (k < vlen && is_css_ws(val[k])) ++k;
                if (k < vlen) {
                    int32_t basis = parse_width_spec_value(val + k, vlen - k, st->font_px, env,
                                                           &st->dependency_flags);
                    if (basis != 0) st->width_spec = basis;
                }
            }
        } else if (arcr_str_eq_nocase(prop, "grid-template-columns")) {
            st->grid_columns = parse_grid_columns(val, vlen, env);
        } else if (arcr_str_eq_nocase(prop, "width")) {
            st->width_spec = parse_width_spec_value(val, vlen, st->font_px, env,
                                                    &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "min-width")) {
            st->min_width_px = parse_width_spec_value(val, vlen, st->font_px, env,
                                                      &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "max-width")) {
            st->max_width_px = parse_width_spec_value(val, vlen, st->font_px, env,
                                                      &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "height")) {
            if (arcr_str_eq_nocase_n(val, vlen, "auto", 4u)) {
                st->height_px = 0;
            } else {
                int32_t vph = env ? env->viewport_height : 800;
                st->height_px = ARCR_MAX(0, parse_length_or_calc_px(
                    val, vlen, st->font_px, vph, env, &st->dependency_flags));
            }
        } else if (arcr_str_eq_nocase(prop, "min-height")) {
            int32_t vph = env ? env->viewport_height : 800;
            st->min_height_px = ARCR_MAX(0, parse_length_or_calc_px(
                val, vlen, st->font_px, vph, env, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "font-size")) {
            int32_t px = parse_length_or_calc_px(val, vlen, st->font_px, st->font_px, env,
                                                 &st->dependency_flags);
            if (px >= 8 && px <= 96) st->font_px = px;
        } else if (arcr_str_eq_nocase(prop, "line-height")) {
            if (arcr_str_eq_nocase_n(val, vlen, "normal", 6u)) {
                st->line_height_pct = 145;
            } else if (val[vlen - 1u] == '%') {
                int32_t pct = 145;
                if (arcr_parse_i32(val, vlen - 1u, &pct)) {
                    st->line_height_pct = ARCR_CLAMP(pct, 100, 300);
                }
            } else {
                int64_t m = 0;
                size_t nend = 0u;
                if (parse_fixed_1000(val, vlen, &m, &nend)) {
                    const char *u = val + nend;
                    size_t ul = vlen - nend;
                    arcr_str_trim_view(&u, &ul);
                    if (ul == 0u) {
                        st->line_height_pct = (int32_t)ARCR_CLAMP((m + 5) / 10, 100, 300);
                    } else {
                        int32_t px = parse_single_length_px(val, vlen, st->font_px, st->font_px,
                                                            env, &st->dependency_flags);
                        if (px > 0 && st->font_px > 0) {
                            st->line_height_pct = ARCR_CLAMP((px * 100) / st->font_px, 100, 300);
                        }
                    }
                }
            }
        } else if (arcr_str_eq_nocase(prop, "font-weight")) {
            int32_t num = 400;
            if (arcr_str_eq_nocase_n(val, vlen, "bold", 4u) ||
                arcr_str_eq_nocase_n(val, vlen, "bolder", 6u)) {
                st->font_bold = true;
            } else if (arcr_parse_i32(val, vlen, &num)) {
                st->font_bold = (num >= 600);
            } else {
                st->font_bold = false;
            }
        } else if (arcr_str_eq_nocase(prop, "font-family")) {
            if (str_contains_nocase(val, vlen, "monospace") ||
                str_contains_nocase(val, vlen, "consolas") ||
                str_contains_nocase(val, vlen, "monaco") ||
                str_contains_nocase(val, vlen, "menlo") ||
                str_contains_nocase(val, vlen, "courier") ||
                str_contains_nocase(val, vlen, "code")) {
                st->font_monospace = true;
            } else {
                st->font_monospace = false;
            }
        } else if (arcr_str_eq_nocase(prop, "text-align")) {
            if (arcr_str_eq_nocase_n(val, vlen, "center", 6u)) {
                st->text_align = ARCR_TEXT_ALIGN_CENTER;
            } else if (arcr_str_eq_nocase_n(val, vlen, "right", 5u) ||
                       arcr_str_eq_nocase_n(val, vlen, "end", 3u)) {
                st->text_align = ARCR_TEXT_ALIGN_RIGHT;
            } else {
                st->text_align = ARCR_TEXT_ALIGN_LEFT;
            }
        } else if (arcr_str_eq_nocase(prop, "text-decoration") ||
                   arcr_str_eq_nocase(prop, "text-decoration-line")) {
            if (arcr_str_eq_nocase_n(val, vlen, "none", 4u)) {
                st->underline = false;
            } else if (str_contains_nocase(val, vlen, "underline")) {
                st->underline = true;
            }
        } else if (arcr_str_eq_nocase(prop, "white-space")) {
            st->white_space_pre = str_contains_nocase(val, vlen, "pre");
        } else if (arcr_str_eq_nocase(prop, "word-break")) {
            st->word_break_keep_all = arcr_str_eq_nocase_n(val, vlen, "keep-all", 8u);
        } else if (arcr_str_eq_nocase(prop, "border-radius")) {
            size_t sp = 0u;
            while (sp < vlen && !is_css_ws(val[sp])) ++sp;
            if (sp > 0u && val[sp - 1u] == '%') {
                st->border_radius = 999; /* Pill / circle clamp handled by rasterizer */
            } else {
                st->border_radius = ARCR_MAX(0, parse_length_or_calc_px(
                    val, sp, st->font_px, 0, env, &st->dependency_flags));
            }
        } else if (arcr_str_eq_nocase(prop, "padding")) {
            parse_box_shorthand(val, vlen, st->font_px, env, &st->padding,
                                &st->dependency_flags, NULL, NULL);
            st->padding.top    = ARCR_MAX(0, st->padding.top);
            st->padding.right  = ARCR_MAX(0, st->padding.right);
            st->padding.bottom = ARCR_MAX(0, st->padding.bottom);
            st->padding.left   = ARCR_MAX(0, st->padding.left);
        } else if (arcr_str_eq_nocase(prop, "padding-top")) {
            st->padding.top = ARCR_MAX(0, parse_length_or_calc_px(
                val, vlen, st->font_px, vpw, env, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "padding-right")) {
            st->padding.right = ARCR_MAX(0, parse_length_or_calc_px(
                val, vlen, st->font_px, vpw, env, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "padding-bottom")) {
            st->padding.bottom = ARCR_MAX(0, parse_length_or_calc_px(
                val, vlen, st->font_px, vpw, env, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "padding-left")) {
            st->padding.left = ARCR_MAX(0, parse_length_or_calc_px(
                val, vlen, st->font_px, vpw, env, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "margin")) {
            parse_box_shorthand(val, vlen, st->font_px, env, &st->margin,
                                &st->dependency_flags, &st->margin_left_auto,
                                &st->margin_right_auto);
        } else if (arcr_str_eq_nocase(prop, "margin-top")) {
            st->margin.top = parse_length_or_calc_px(val, vlen, st->font_px, vpw, env,
                                                     &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "margin-bottom")) {
            st->margin.bottom = parse_length_or_calc_px(val, vlen, st->font_px, vpw, env,
                                                        &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "margin-left")) {
            if (arcr_str_eq_nocase_n(val, vlen, "auto", 4u)) {
                st->margin_left_auto = true;
                st->margin.left      = 0;
            } else {
                st->margin_left_auto = false;
                st->margin.left = parse_length_or_calc_px(val, vlen, st->font_px, vpw, env,
                                                          &st->dependency_flags);
            }
        } else if (arcr_str_eq_nocase(prop, "margin-right")) {
            if (arcr_str_eq_nocase_n(val, vlen, "auto", 4u)) {
                st->margin_right_auto = true;
                st->margin.right      = 0;
            } else {
                st->margin_right_auto = false;
                st->margin.right = parse_length_or_calc_px(val, vlen, st->font_px, vpw, env,
                                                           &st->dependency_flags);
            }
        } else if (arcr_str_eq_nocase(prop, "border-width")) {
            parse_box_shorthand(val, vlen, st->font_px, env, &st->border,
                                &st->dependency_flags, NULL, NULL);
        } else if (arcr_str_eq_nocase(prop, "border")) {
            int32_t bw = 0;
            parse_border_side_shorthand(val, vlen, st->font_px, env, &bw,
                                        &st->border_argb, st->color_argb, &st->dependency_flags);
            st->border = (ArcrEdgeSizes){bw, bw, bw, bw};
        } else if (arcr_str_eq_nocase(prop, "border-top")) {
            parse_border_side_shorthand(val, vlen, st->font_px, env, &st->border.top,
                                        &st->border_argb, st->color_argb, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "border-right")) {
            parse_border_side_shorthand(val, vlen, st->font_px, env, &st->border.right,
                                        &st->border_argb, st->color_argb, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "border-bottom")) {
            parse_border_side_shorthand(val, vlen, st->font_px, env, &st->border.bottom,
                                        &st->border_argb, st->color_argb, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "border-left")) {
            parse_border_side_shorthand(val, vlen, st->font_px, env, &st->border.left,
                                        &st->border_argb, st->color_argb, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "box-shadow")) {
            parse_box_shadow(val, vlen, st->font_px, env, st);
        } else if (arcr_str_eq_nocase(prop, "opacity")) {
            int64_t m = 1000;
            if (parse_fixed_1000(val, vlen, &m, NULL)) {
                int32_t op = (int32_t)ARCR_CLAMP(m, 0, 1000);
                st->color_argb  = modulate_alpha(st->color_argb, op);
                st->bg_argb     = modulate_alpha(st->bg_argb, op);
                st->border_argb = modulate_alpha(st->border_argb, op);
            }
        }
    }
}

typedef struct MatchedRule {
    const ArcrCssRule *rule;
} MatchedRule;

static int cmp_matched_rules(const void *a, const void *b) {
    const ArcrCssRule *ra = ((const MatchedRule *)a)->rule;
    const ArcrCssRule *rb = ((const MatchedRule *)b)->rule;
    if (ra->specificity != rb->specificity) {
        return (ra->specificity < rb->specificity) ? -1 : 1;
    }
    return (ra->source_order < rb->source_order) ? -1 : 1;
}

typedef struct StyleResolverCtx {
    ArcrDocument      *doc;
    ArcrStylesheet    *sheet;
    ArcrAtom           style_attr_atom;
    ArcrAtom           class_attr_atom;
    ArcrStyleShareSlot share_ring[ARCR_STYLE_SHARE_RING];
    size_t             share_cursor;
    size_t             share_hits;
} StyleResolverCtx;

static bool is_element_shareable(const StyleResolverCtx *ctx, const ArcrDomNode *el) {
    if (el->state_flags != 0u || el->id != ARCR_ATOM_NULL || el->surface != NULL) {
        return false;
    }
    /* Only share if element has no non-class attributes (guards inline style="", type="", etc.) */
    for (const ArcrAttr *a = el->first_attr; a != NULL; a = a->next) {
        if (a->name != ctx->class_attr_atom) return false;
    }
    return true;
}

static bool try_share_style(StyleResolverCtx *ctx, const ArcrDomNode *el,
                            const ArcrComputedStyle *parent_style,
                            bool is_first_el, bool is_last_el,
                            const ArcrComputedStyle **out_shared) {
    if (!is_element_shareable(ctx, el)) return false;

    for (size_t i = 0u; i < ARCR_STYLE_SHARE_RING; ++i) {
        const ArcrStyleShareSlot *slot = &ctx->share_ring[i];
        if (!slot->shared_style) continue;
        if (slot->parent_style != parent_style) continue;
        if (slot->tag != el->tag || slot->class_count != el->class_count) continue;
        if (ctx->sheet && ctx->sheet->uses_positional_or_sibling) {
            if (slot->is_first_el != is_first_el || slot->is_last_el != is_last_el) continue;
        }

        bool same_classes = true;
        for (uint32_t c = 0u; c < el->class_count; ++c) {
            if (slot->classes[c] != el->classes[c]) {
                same_classes = false;
                break;
            }
        }
        if (same_classes) {
            *out_shared = slot->shared_style;
            ctx->share_hits++;
            return true;
        }
    }
    return false;
}

static void record_shareable_style(StyleResolverCtx *ctx, const ArcrDomNode *el,
                                   const ArcrComputedStyle *parent_style,
                                   bool is_first_el, bool is_last_el,
                                   const ArcrComputedStyle *computed) {
    if (!is_element_shareable(ctx, el)) return;

    ArcrStyleShareSlot *slot = &ctx->share_ring[ctx->share_cursor % ARCR_STYLE_SHARE_RING];
    ctx->share_cursor++;
    slot->parent_style = parent_style;
    slot->tag          = el->tag;
    slot->class_count  = el->class_count;
    slot->is_first_el  = is_first_el;
    slot->is_last_el   = is_last_el;
    for (uint32_t c = 0u; c < el->class_count; ++c) {
        slot->classes[c] = el->classes[c];
    }
    slot->shared_style = computed;
}

static void collect_bucket_matches(const ArcrCssRule *bucket_head, const ArcrDomNode *el,
                                   MatchedRule *matched, size_t max_m, size_t *inout_cnt,
                                   bool *inout_state_dep) {
    for (const ArcrCssRule *r = bucket_head; r != NULL; r = r->next_in_bucket) {
        if (*inout_cnt >= max_m) break;
        if (rule_matches_element(r, el, inout_state_dep)) {
            matched[(*inout_cnt)++].rule = r;
        }
    }
}

static void resolve_node_recursive(StyleResolverCtx *ctx, ArcrDomNode *node,
                                   const ArcrComputedStyle *parent_style,
                                   bool is_first_el, bool is_last_el) {
    if (!node) return;

    if (node->kind == ARCR_NODE_ELEMENT) {
        const ArcrComputedStyle *shared = NULL;
        if (try_share_style(ctx, node, parent_style, is_first_el, is_last_el, &shared)) {
            node->computed_style = shared;
        } else {
            ArcrComputedStyle *st = (ArcrComputedStyle *)arcr_arena_alloc(
                &ctx->doc->arena, sizeof(ArcrComputedStyle), sizeof(void *));
            if (!st) return;
            apply_ua_defaults(ctx->doc, node, parent_style, st);

            MatchedRule matched[256];
            size_t      mcount    = 0u;
            bool        state_dep = false;

            if (ctx->sheet) {
                if (node->id != ARCR_ATOM_NULL) {
                    collect_bucket_matches(
                        ctx->sheet->by_id[node->id & (ARCR_CSS_ID_BUCKETS - 1u)],
                        node, matched, ARCR_ARRAY_LEN(matched), &mcount, &state_dep);
                }
                for (uint32_t c = 0u; c < node->class_count; ++c) {
                    collect_bucket_matches(
                        ctx->sheet->by_class[node->classes[c] & (ARCR_CSS_CLASS_BUCKETS - 1u)],
                        node, matched, ARCR_ARRAY_LEN(matched), &mcount, &state_dep);
                }
                collect_bucket_matches(
                    ctx->sheet->by_tag[node->tag & (ARCR_CSS_TAG_BUCKETS - 1u)],
                    node, matched, ARCR_ARRAY_LEN(matched), &mcount, &state_dep);
                collect_bucket_matches(
                    ctx->sheet->universal_list,
                    node, matched, ARCR_ARRAY_LEN(matched), &mcount, &state_dep);

                if (state_dep) st->dependency_flags |= ARCR_STYLE_DEP_STATE;

                if (mcount > 1u) {
                    qsort(matched, mcount, sizeof(MatchedRule), cmp_matched_rules);
                }
                for (size_t i = 0u; i < mcount; ++i) {
                    if (matched[i].rule->from_media) {
                        st->dependency_flags |= ARCR_STYLE_DEP_MEDIA;
                    }
                    apply_declarations_pass(ctx->doc, ctx->sheet, matched[i].rule->decls,
                                            st, false);
                }
            }

            const char *inline_css = arcr_dom_get_attr(node, ctx->style_attr_atom);
            ArcrCssDecl *idecls    = NULL;
            if (inline_css && inline_css[0] != '\0') {
                idecls = parse_declarations(ctx->doc, ctx->sheet, inline_css,
                                            strlen(inline_css), false);
                apply_declarations_pass(ctx->doc, ctx->sheet, idecls, st, false);
            }

            /* Apply !important cascade pass (stylesheet rules then inline !important) */
            for (size_t i = 0u; i < mcount; ++i) {
                apply_declarations_pass(ctx->doc, ctx->sheet, matched[i].rule->decls, st, true);
            }
            if (idecls) {
                apply_declarations_pass(ctx->doc, ctx->sheet, idecls, st, true);
            }

            node->computed_style = st;
            record_shareable_style(ctx, node, parent_style, is_first_el, is_last_el, st);
        }

        if (node->computed_style->display == ARCR_DISPLAY_NONE) {
            return;
        }
        parent_style = node->computed_style;
    }

    const ArcrDomNode *first_el = NULL;
    const ArcrDomNode *last_el  = NULL;
    for (const ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        if (ch->kind == ARCR_NODE_ELEMENT) {
            if (!first_el) first_el = ch;
            last_el = ch;
        }
    }

    for (ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        resolve_node_recursive(ctx, ch, parent_style, ch == first_el, ch == last_el);
    }
}

void arcr_css_resolve_tree(ArcrDocument *doc, ArcrStylesheet *sheet,
                           size_t *out_style_share_hits) {
    if (!doc || !doc->root) return;
    StyleResolverCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.doc             = doc;
    ctx.sheet           = sheet;
    ctx.style_attr_atom = arcr_atom_intern_lower(doc->atoms, "style", 5u);
    ctx.class_attr_atom = arcr_atom_intern_lower(doc->atoms, "class", 5u);

    resolve_node_recursive(&ctx, doc->root, NULL, false, false);
    if (out_style_share_hits) *out_style_share_hits = ctx.share_hits;
}
