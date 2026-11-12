/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * css/arcr_css.c - Modern 2026 CSS3 Engine (Tailwind v4, oklch/color-mix, @layer/@supports,
 *                  Per-Element --vars, Full calc/min/max/clamp, 60 FPS @keyframes & Transforms)
 */
#include "arcr_css.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ARCR_CSS_ID_BUCKETS    64u
#define ARCR_CSS_CLASS_BUCKETS 128u
#define ARCR_CSS_TAG_BUCKETS   64u
#define ARCR_CSS_MAX_STEPS     10u
#define ARCR_CSS_STEP_CLASSES  8u
#define ARCR_CSS_STEP_ATTRS    4u
#define ARCR_STYLE_SHARE_RING  32u

/* Encoding for positioned percentage offsets in top_px/right_px/bottom_px/left_px */
#define ARCR_POS_PCT_BIAS      (-100000000)
#define ARCR_POS_PCT_THRESHOLD (-99000000)
#define ARCR_CSS_PX_MAX        (10000000)

typedef enum CssCombinator {
    CSS_COMB_NONE       = 0,
    CSS_COMB_DESCENDANT = 1,
    CSS_COMB_CHILD      = 2,
    CSS_COMB_ADJACENT   = 3,
    CSS_COMB_SIBLING    = 4
} CssCombinator;

typedef enum CssAttrOp {
    CSS_ATTR_EXISTS   = 0,
    CSS_ATTR_EQ       = 1,
    CSS_ATTR_INCLUDES = 2, /* ~= */
    CSS_ATTR_DASH     = 3, /* |= */
    CSS_ATTR_PREFIX   = 4, /* ^= */
    CSS_ATTR_SUFFIX   = 5, /* $= */
    CSS_ATTR_SUBSTR   = 6  /* *= */
} CssAttrOp;

typedef struct CssAttrCond {
    ArcrAtom    name;
    CssAttrOp   op;
    const char *val;
    size_t      val_len;
    bool        ignore_case;
} CssAttrCond;

struct CssCompoundSel;

typedef struct CssSelectorGroup {
    struct CssCompoundSel *alts;
    uint8_t                alt_count;
} CssSelectorGroup;

typedef struct CssCompoundSel {
    ArcrAtom          tag;
    ArcrAtom          id;
    ArcrAtom          classes[ARCR_CSS_STEP_CLASSES];
    uint8_t           class_count;
    CssAttrCond       attrs[ARCR_CSS_STEP_ATTRS];
    uint8_t           attr_count;
    uint32_t          req_state;
    bool              req_focus_within;
    bool              req_disabled;
    bool              req_enabled;
    bool              req_root;
    bool              req_empty;
    bool              req_first_child;
    bool              req_last_child;
    bool              req_only_child;
    bool              has_nth;
    int32_t           nth_a;
    int32_t           nth_b;
    CssSelectorGroup *not_groups;
    uint8_t           not_count;
    CssSelectorGroup *is_groups;
    uint8_t           is_count;
    bool              impossible;
    CssCombinator     comb_to_next;
} CssCompoundSel;

typedef struct ArcrCssDecl {
    ArcrAtom            prop;
    const char         *val_str;
    size_t              val_len;
    bool                important;
    bool                is_custom;
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

struct ArcrCssCustomPropMap {
    const struct ArcrCssCustomPropMap *parent;
    ArcrCssVar                        *vars_head;
};

typedef struct ArcrKeyframeStop {
    int32_t                  pct_milli; /* 0..100000 (0%..100%) */
    bool                     has_opacity;
    uint8_t                  opacity;
    bool                     has_bg;
    uint32_t                 bg_argb;
    bool                     has_color;
    uint32_t                 color_argb;
    bool                     has_transform;
    int32_t                  tx_px;
    int32_t                  ty_px;
    int32_t                  tx_pct;
    int32_t                  ty_pct;
    int32_t                  sx_milli;
    int32_t                  sy_milli;
    int32_t                  rot_milli;
    bool                     has_top;
    int32_t                  top_px;
    bool                     has_left;
    int32_t                  left_px;
    struct ArcrKeyframeStop *next;
} ArcrKeyframeStop;

typedef struct ArcrKeyframeRule {
    ArcrAtom                 name;
    ArcrKeyframeStop        *stops_head;
    struct ArcrKeyframeRule *next;
} ArcrKeyframeRule;

struct ArcrStylesheet {
    ArcrArena         arena;
    ArcrCssMediaEnv   env;
    ArcrCssRule      *by_id[ARCR_CSS_ID_BUCKETS];
    ArcrCssRule      *by_class[ARCR_CSS_CLASS_BUCKETS];
    ArcrCssRule      *by_tag[ARCR_CSS_TAG_BUCKETS];
    ArcrCssRule      *universal_list;
    ArcrCssVar       *vars_head;
    ArcrKeyframeRule *keyframes_head;
    uint32_t          rule_count;
    bool              uses_first_or_last_child;
    bool              uses_positional_or_sibling;
};

typedef struct ArcrStyleShareSlot {
    const ArcrComputedStyle           *parent_style;
    const struct ArcrCssCustomPropMap *parent_vars;
    ArcrAtom                           tag;
    uint32_t                           class_count;
    ArcrAtom                           classes[ARCR_MAX_INLINE_CLASSES];
    bool                               is_first_el;
    bool                               is_last_el;
    const ArcrComputedStyle           *shared_style;
} ArcrStyleShareSlot;

/* -------------------------------------------------------------------------
 * 1. Low-Level Lexical, Escape & Scientific Number Utilities
 * ------------------------------------------------------------------------- */
static bool is_css_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool is_css_raw_ident_char(char c) {
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

/*
 * Consumes a CSS identifier with CSS Syntax Level 3 backslash unescaping
 * (e.g. `.sm\:flex` -> `sm:flex`, `.bg-\[\#0f172a\]` -> `bg-[#0f172a]`, `.w-1\/2` -> `w-1/2`,
 *  `.\32 xl\:p-4` -> `2xl:p-4`).
 */
static size_t read_unescaped_css_ident(const char *s, size_t len, size_t *inout_pos,
                                       char *out_buf, size_t out_cap) {
    size_t i   = *inout_pos;
    size_t out = 0u;

    while (i < len) {
        char c = s[i];
        if (c == '\\') {
            ++i;
            if (i >= len) break;
            int h0 = hex_nibble(s[i]);
            if (h0 >= 0) {
                uint32_t cp = (uint32_t)h0;
                ++i;
                for (int k = 0; k < 5 && i < len; ++k) {
                    int hk = hex_nibble(s[i]);
                    if (hk < 0) break;
                    cp = (cp << 4) | (uint32_t)hk;
                    ++i;
                }
                /* Optional single whitespace after hex escape per CSS spec */
                if (i < len && s[i] == '\r' && i + 1u < len && s[i + 1u] == '\n') i += 2u;
                else if (i < len && is_css_ws(s[i])) ++i;

                if (cp == 0u || cp > 0x10FFFFu) cp = 0xFFFDu;
                char u8[4];
                size_t n = arcr_utf8_encode_one(cp, u8);
                if (out + n < out_cap) {
                    memcpy(out_buf + out, u8, n);
                    out += n;
                }
            } else if (s[i] != '\n' && s[i] != '\r' && s[i] != '\f') {
                if (out + 1u < out_cap) out_buf[out++] = s[i];
                ++i;
            }
        } else if (is_css_raw_ident_char(c)) {
            if (out + 1u < out_cap) out_buf[out++] = c;
            ++i;
        } else {
            break;
        }
    }
    if (out_cap > 0u) out_buf[out] = '\0';
    *inout_pos = i;
    return out;
}

/* Parses floating-point numbers including scientific notation (`3.40282e38`, `1e-3`) */
static bool parse_css_double(const char *s, size_t len, double *out_val, size_t *out_end) {
    size_t i = 0u;
    while (i < len && is_css_ws(s[i])) ++i;
    if (i >= len) return false;

    double sign = 1.0;
    if (s[i] == '-') {
        sign = -1.0;
        ++i;
    } else if (s[i] == '+') {
        ++i;
    }

    if (i + 8u <= len && arcr_str_eq_nocase_n(s + i, 8u, "infinity", 8u)) {
        *out_val = sign * 1.0e15;
        if (out_end) *out_end = i + 8u;
        return true;
    }

    double val    = 0.0;
    size_t digits = 0u;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        val = val * 10.0 + (double)(s[i] - '0');
        ++i;
        ++digits;
    }
    if (i < len && s[i] == '.') {
        ++i;
        double scale = 0.1;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            val   += (double)(s[i] - '0') * scale;
            scale *= 0.1;
            ++i;
            ++digits;
        }
    }
    if (digits == 0u) return false;

    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        size_t exp_i = i + 1u;
        int    esign = 1;
        if (exp_i < len && (s[exp_i] == '+' || s[exp_i] == '-')) {
            if (s[exp_i] == '-') esign = -1;
            ++exp_i;
        }
        size_t edigits = 0u;
        int    exp_val = 0;
        while (exp_i < len && s[exp_i] >= '0' && s[exp_i] <= '9') {
            if (exp_val < 1000) exp_val = exp_val * 10 + (s[exp_i] - '0');
            ++exp_i;
            ++edigits;
        }
        if (edigits > 0u) {
            i = exp_i;
            int signed_exp = esign * exp_val;
            if (signed_exp > 30)       val = 1.0e30;
            else if (signed_exp < -30) val = 0.0;
            else                       val *= pow(10.0, (double)signed_exp);
        }
    }

    *out_val = sign * val;
    if (out_end) *out_end = i;
    return true;
}

static bool parse_fixed_1000(const char *s, size_t len, int64_t *out_milli, size_t *out_end) {
    double v = 0.0;
    if (!parse_css_double(s, len, &v, out_end)) return false;
    if (v > 5.0e11)  v = 5.0e11;
    if (v < -5.0e11) v = -5.0e11;
    *out_milli = (int64_t)(v >= 0.0 ? (v * 1000.0 + 0.5) : (v * 1000.0 - 0.5));
    return true;
}

static int32_t clamp_css_px_f(double px) {
    if (px >= (double)(INT32_MAX / 4))  return INT32_MAX / 4;
    if (px <= (double)(-INT32_MAX / 4)) return -INT32_MAX / 4;
    if (px > (double)ARCR_CSS_PX_MAX)   return ARCR_CSS_PX_MAX;
    if (px < (double)-ARCR_CSS_PX_MAX)  return -ARCR_CSS_PX_MAX;
    return (int32_t)(px >= 0.0 ? (px + 0.5) : (px - 0.5));
}

/* Quote-aware CSS comment stripper (block comments preserved inside `"..."` and `'...'`) */
static size_t strip_css_comments(const char *src, size_t len, char *dst) {
    size_t i     = 0u;
    size_t out   = 0u;
    char   quote = '\0';

    while (i < len) {
        char c = src[i];
        if (quote != '\0') {
            dst[out++] = c;
            if (c == '\\' && i + 1u < len) {
                dst[out++] = src[i + 1u];
                i += 2u;
                continue;
            }
            if (c == quote) quote = '\0';
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote      = c;
            dst[out++] = c;
            ++i;
            continue;
        }
        if (c == '\\' && i + 1u < len) {
            dst[out++] = c;
            dst[out++] = src[i + 1u];
            i += 2u;
            continue;
        }
        if (i + 1u < len && c == '/' && src[i + 1u] == '*') {
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

/* -------------------------------------------------------------------------
 * 2. CSS Color 4 (`rgb`, `hsl`, `oklab`, `oklch`, `color-mix`, 148 Named)
 * ------------------------------------------------------------------------- */
static uint8_t linear_to_srgb_u8(double c) {
    if (c <= 0.0) return 0u;
    if (c >= 1.0) return 255u;
    double s = (c <= 0.0031308) ? (12.92 * c) : (1.055 * pow(c, 1.0 / 2.4) - 0.055);
    int v    = (int)(s * 255.0 + 0.5);
    return (uint8_t)ARCR_CLAMP(v, 0, 255);
}

static double srgb_u8_to_linear(uint8_t u) {
    double s = (double)u / 255.0;
    return (s <= 0.04045) ? (s / 12.92) : pow((s + 0.055) / 1.055, 2.4);
}

static uint32_t oklab_to_argb(double L, double a, double b, int32_t alpha_255) {
    L = ARCR_CLAMP(L, 0.0, 1.0);
    double l_ = L + 0.3963377774 * a + 0.2158037573 * b;
    double m_ = L - 0.1055613458 * a - 0.0638541728 * b;
    double s_ = L - 0.0894841775 * a - 1.2914855480 * b;

    double l = l_ * l_ * l_;
    double m = m_ * m_ * m_;
    double s = s_ * s_ * s_;

    double r_lin = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    double g_lin = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    double b_lin = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;

    uint32_t ru = linear_to_srgb_u8(r_lin);
    uint32_t gu = linear_to_srgb_u8(g_lin);
    uint32_t bu = linear_to_srgb_u8(b_lin);
    uint32_t au = (uint32_t)ARCR_CLAMP(alpha_255, 0, 255);
    return (au << 24) | (ru << 16) | (gu << 8) | bu;
}

static void argb_to_oklab(uint32_t argb, double *out_L, double *out_a, double *out_b) {
    double r = srgb_u8_to_linear((uint8_t)((argb >> 16) & 0xFFu));
    double g = srgb_u8_to_linear((uint8_t)((argb >> 8) & 0xFFu));
    double b = srgb_u8_to_linear((uint8_t)(argb & 0xFFu));

    double l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
    double m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
    double s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;

    double l_ = (l > 0.0) ? pow(l, 1.0 / 3.0) : 0.0;
    double m_ = (m > 0.0) ? pow(m, 1.0 / 3.0) : 0.0;
    double s_ = (s > 0.0) ? pow(s, 1.0 / 3.0) : 0.0;

    *out_L = 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_;
    *out_a = 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_;
    *out_b = 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_;
}

static double parse_angle_deg(const char *s, size_t len) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u || arcr_str_eq_nocase_n(s, len, "none", 4u)) return 0.0;
    double v = 0.0;
    size_t nend = 0u;
    if (!parse_css_double(s, len, &v, &nend)) return 0.0;
    const char *u = s + nend;
    size_t ul = len - nend;
    arcr_str_trim_view(&u, &ul);
    if (arcr_str_eq_nocase_n(u, ul, "turn", 4u)) return v * 360.0;
    if (arcr_str_eq_nocase_n(u, ul, "rad", 3u))  return v * (180.0 / 3.14159265358979323846);
    if (arcr_str_eq_nocase_n(u, ul, "grad", 4u)) return v * 0.9;
    return v;
}

static uint32_t hsl_to_argb(double deg, double sat_pct, double light_pct, int32_t alpha_255) {
    double h = fmod(deg, 360.0);
    if (h < 0.0) h += 360.0;
    double s = ARCR_CLAMP(sat_pct, 0.0, 100.0) / 100.0;
    double l = ARCR_CLAMP(light_pct, 0.0, 100.0) / 100.0;

    double c  = (1.0 - fabs(2.0 * l - 1.0)) * s;
    double hp = h / 60.0;
    double x  = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
    double m  = l - c * 0.5;

    double r1 = 0.0, g1 = 0.0, b1 = 0.0;
    if (hp < 1.0)      { r1 = c; g1 = x; b1 = 0.0; }
    else if (hp < 2.0) { r1 = x; g1 = c; b1 = 0.0; }
    else if (hp < 3.0) { r1 = 0.0; g1 = c; b1 = x; }
    else if (hp < 4.0) { r1 = 0.0; g1 = x; b1 = c; }
    else if (hp < 5.0) { r1 = x; g1 = 0.0; b1 = c; }
    else               { r1 = c; g1 = 0.0; b1 = x; }

    uint32_t r = (uint32_t)ARCR_CLAMP((int)((r1 + m) * 255.0 + 0.5), 0, 255);
    uint32_t g = (uint32_t)ARCR_CLAMP((int)((g1 + m) * 255.0 + 0.5), 0, 255);
    uint32_t b = (uint32_t)ARCR_CLAMP((int)((b1 + m) * 255.0 + 0.5), 0, 255);
    uint32_t a = (uint32_t)ARCR_CLAMP(alpha_255, 0, 255);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static int32_t parse_alpha_component(const char *s, size_t len) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return 255;
    if (arcr_str_eq_nocase_n(s, len, "none", 4u)) return 0;
    bool is_pct = false;
    if (s[len - 1u] == '%') {
        is_pct = true;
        --len;
    }
    double v = 1.0;
    if (!parse_css_double(s, len, &v, NULL)) return 255;
    if (is_pct) v /= 100.0;
    return (int32_t)ARCR_CLAMP((int)(v * 255.0 + 0.5), 0, 255);
}

static size_t split_color_args(const char *inner, size_t ilen,
                               const char *parts[4], size_t plens[4]) {
    size_t count = 0u;
    size_t i     = 0u;
    while (i < ilen && count < 4u) {
        while (i < ilen && (is_css_ws(inner[i]) || inner[i] == ',' || inner[i] == '/')) ++i;
        if (i >= ilen) break;
        size_t start = i;
        int paren = 0;
        while (i < ilen) {
            if (inner[i] == '(') ++paren;
            else if (inner[i] == ')' && paren > 0) --paren;
            else if (paren == 0 && (is_css_ws(inner[i]) || inner[i] == ',' || inner[i] == '/')) {
                break;
            }
            ++i;
        }
        parts[count] = inner + start;
        plens[count] = i - start;
        count++;
    }
    return count;
}

static bool parse_color_mix(const char *inner, size_t ilen, uint32_t *out_argb) {
    /* Syntax: color-mix(in srgb|oklab|oklch [hue], color1 [p1%], color2 [p2%]) */
    size_t c1 = 0u;
    int paren = 0;
    while (c1 < ilen) {
        if (inner[c1] == '(') ++paren;
        else if (inner[c1] == ')' && paren > 0) --paren;
        else if (inner[c1] == ',' && paren == 0) break;
        ++c1;
    }
    if (c1 >= ilen) return false;

    const char *space_s = inner;
    size_t      space_l = c1;
    arcr_str_trim_view(&space_s, &space_l);
    bool use_oklab = false;
    for (size_t k = 0u; k + 2u <= space_l; ++k) {
        if (arcr_str_eq_nocase_n(space_s + k, 2u, "ok", 2u)) {
            use_oklab = true;
            break;
        }
    }

    size_t c2 = c1 + 1u;
    paren = 0;
    while (c2 < ilen) {
        if (inner[c2] == '(') ++paren;
        else if (inner[c2] == ')' && paren > 0) --paren;
        else if (inner[c2] == ',' && paren == 0) break;
        ++c2;
    }
    if (c2 >= ilen) return false;

    const char *arg1 = inner + c1 + 1u;
    size_t      len1 = c2 - (c1 + 1u);
    const char *arg2 = inner + c2 + 1u;
    size_t      len2 = ilen - (c2 + 1u);
    arcr_str_trim_view(&arg1, &len1);
    arcr_str_trim_view(&arg2, &len2);

    const char *args[2] = {arg1, arg2};
    size_t      alens[2] = {len1, len2};
    uint32_t    cols[2]  = {0u, 0u};
    double      pcts[2]  = {-1.0, -1.0};

    for (int idx = 0; idx < 2; ++idx) {
        const char *a = args[idx];
        size_t      l = alens[idx];
        if (l == 0u) return false;

        /* Optional trailing percentage `color 40%` or leading `40% color` */
        if (a[l - 1u] == '%') {
            size_t sp = l;
            while (sp > 0u && !is_css_ws(a[sp - 1u])) --sp;
            if (sp > 0u) {
                double p = 0.0;
                if (parse_css_double(a + sp, (l - 1u) - sp, &p, NULL)) {
                    pcts[idx] = ARCR_CLAMP(p, 0.0, 100.0);
                    l = sp;
                    arcr_str_trim_view(&a, &l);
                }
            }
        } else {
            size_t sp = 0u;
            while (sp < l && !is_css_ws(a[sp])) ++sp;
            if (sp > 1u && sp < l && a[sp - 1u] == '%') {
                double p = 0.0;
                if (parse_css_double(a, sp - 1u, &p, NULL)) {
                    pcts[idx] = ARCR_CLAMP(p, 0.0, 100.0);
                    a += sp;
                    l -= sp;
                    arcr_str_trim_view(&a, &l);
                }
            }
        }
        if (!arcr_css_parse_color(a, l, &cols[idx])) return false;
    }

    double p1 = pcts[0];
    double p2 = pcts[1];
    if (p1 < 0.0 && p2 < 0.0) { p1 = 50.0; p2 = 50.0; }
    else if (p1 < 0.0)        { p1 = 100.0 - p2; }
    else if (p2 < 0.0)        { p2 = 100.0 - p1; }

    double sum_p = p1 + p2;
    if (sum_p <= 0.0001) {
        *out_argb = 0x00000000u;
        return true;
    }
    double alpha_mult = (sum_p < 100.0) ? (sum_p / 100.0) : 1.0;
    double w1 = p1 / sum_p;
    double w2 = p2 / sum_p;

    double a1 = (double)((cols[0] >> 24) & 0xFFu) / 255.0;
    double a2 = (double)((cols[1] >> 24) & 0xFFu) / 255.0;
    double out_a = (a1 * w1 + a2 * w2);
    if (out_a <= 0.0001) {
        *out_argb = 0x00000000u;
        return true;
    }

    int32_t final_a = (int32_t)ARCR_CLAMP((int)(out_a * alpha_mult * 255.0 + 0.5), 0, 255);

    if (use_oklab) {
        double L1, oa1, ob1, L2, oa2, ob2;
        argb_to_oklab(cols[0], &L1, &oa1, &ob1);
        argb_to_oklab(cols[1], &L2, &oa2, &ob2);
        double mL = (L1 * a1 * w1 + L2 * a2 * w2) / out_a;
        double ma = (oa1 * a1 * w1 + oa2 * a2 * w2) / out_a;
        double mb = (ob1 * a1 * w1 + ob2 * a2 * w2) / out_a;
        *out_argb = oklab_to_argb(mL, ma, mb, final_a);
        return true;
    }

    double r1 = (double)((cols[0] >> 16) & 0xFFu);
    double g1 = (double)((cols[0] >> 8) & 0xFFu);
    double b1 = (double)(cols[0] & 0xFFu);
    double r2 = (double)((cols[1] >> 16) & 0xFFu);
    double g2 = (double)((cols[1] >> 8) & 0xFFu);
    double b2 = (double)(cols[1] & 0xFFu);

    uint32_t mr = (uint32_t)ARCR_CLAMP((int)((r1 * a1 * w1 + r2 * a2 * w2) / out_a + 0.5), 0, 255);
    uint32_t mg = (uint32_t)ARCR_CLAMP((int)((g1 * a1 * w1 + g2 * a2 * w2) / out_a + 0.5), 0, 255);
    uint32_t mb = (uint32_t)ARCR_CLAMP((int)((b1 * a1 * w1 + b2 * a2 * w2) / out_a + 0.5), 0, 255);
    *out_argb = ((uint32_t)final_a << 24) | (mr << 16) | (mg << 8) | mb;
    return true;
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
                if (arcr_str_eq_nocase_n(parts[c], cl, "none", 4u)) {
                    rgb[c] = 0;
                    continue;
                }
                bool pct = (cl > 0u && parts[c][cl - 1u] == '%');
                if (pct) --cl;
                double v = 0.0;
                if (!parse_css_double(parts[c], cl, &v, NULL)) return false;
                if (pct) rgb[c] = (int32_t)ARCR_CLAMP((int)(v * 2.55 + 0.5), 0, 255);
                else     rgb[c] = (int32_t)ARCR_CLAMP((int)(v + 0.5), 0, 255);
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

            double hd = parse_angle_deg(parts[0], plens[0]);
            double sv = 0.0, lv = 0.0;
            size_t sl = plens[1], ll = plens[2];
            if (sl > 0u && parts[1][sl - 1u] == '%') --sl;
            if (ll > 0u && parts[2][ll - 1u] == '%') --ll;
            if (!arcr_str_eq_nocase_n(parts[1], sl, "none", 4u) &&
                !parse_css_double(parts[1], sl, &sv, NULL)) return false;
            if (!arcr_str_eq_nocase_n(parts[2], ll, "none", 4u) &&
                !parse_css_double(parts[2], ll, &lv, NULL)) return false;
            int32_t a = (n >= 4u) ? parse_alpha_component(parts[3], plens[3]) : 255;
            *out_argb = hsl_to_argb(hd, sv, lv, a);
            return true;
        }

        if (len > 7u && arcr_str_eq_nocase_n(s, 6u, "oklch(", 6u)) {
            const char *parts[4];
            size_t      plens[4];
            size_t n = split_color_args(s + 6u, len - 7u, parts, plens);
            if (n < 3u) return false;

            double L = 0.0, C = 0.0;
            size_t l_len = plens[0];
            bool   l_pct = (l_len > 0u && parts[0][l_len - 1u] == '%');
            if (l_pct) --l_len;
            if (!arcr_str_eq_nocase_n(parts[0], l_len, "none", 4u)) {
                if (!parse_css_double(parts[0], l_len, &L, NULL)) return false;
                if (l_pct) L /= 100.0;
            }
            size_t c_len = plens[1];
            bool   c_pct = (c_len > 0u && parts[1][c_len - 1u] == '%');
            if (c_pct) --c_len;
            if (!arcr_str_eq_nocase_n(parts[1], c_len, "none", 4u)) {
                if (!parse_css_double(parts[1], c_len, &C, NULL)) return false;
                if (c_pct) C = (C / 100.0) * 0.4; /* CSS Color 4: 100% chroma = 0.4 */
            }
            double H_deg = parse_angle_deg(parts[2], plens[2]);
            double H_rad = H_deg * (3.14159265358979323846 / 180.0);
            int32_t a    = (n >= 4u) ? parse_alpha_component(parts[3], plens[3]) : 255;
            *out_argb    = oklab_to_argb(L, C * cos(H_rad), C * sin(H_rad), a);
            return true;
        }

        if (len > 7u && arcr_str_eq_nocase_n(s, 6u, "oklab(", 6u)) {
            const char *parts[4];
            size_t      plens[4];
            size_t n = split_color_args(s + 6u, len - 7u, parts, plens);
            if (n < 3u) return false;

            double L = 0.0, oa = 0.0, ob = 0.0;
            size_t l_len = plens[0];
            bool   l_pct = (l_len > 0u && parts[0][l_len - 1u] == '%');
            if (l_pct) --l_len;
            if (!arcr_str_eq_nocase_n(parts[0], l_len, "none", 4u)) {
                if (!parse_css_double(parts[0], l_len, &L, NULL)) return false;
                if (l_pct) L /= 100.0;
            }
            size_t a_len = plens[1];
            bool   a_pct = (a_len > 0u && parts[1][a_len - 1u] == '%');
            if (a_pct) --a_len;
            if (!arcr_str_eq_nocase_n(parts[1], a_len, "none", 4u)) {
                if (!parse_css_double(parts[1], a_len, &oa, NULL)) return false;
                if (a_pct) oa = (oa / 100.0) * 0.4;
            }
            size_t b_len = plens[2];
            bool   b_pct = (b_len > 0u && parts[2][b_len - 1u] == '%');
            if (b_pct) --b_len;
            if (!arcr_str_eq_nocase_n(parts[2], b_len, "none", 4u)) {
                if (!parse_css_double(parts[2], b_len, &ob, NULL)) return false;
                if (b_pct) ob = (ob / 100.0) * 0.4;
            }
            int32_t alpha = (n >= 4u) ? parse_alpha_component(parts[3], plens[3]) : 255;
            *out_argb     = oklab_to_argb(L, oa, ob, alpha);
            return true;
        }

        if (len > 11u && arcr_str_eq_nocase_n(s, 10u, "color-mix(", 10u)) {
            return parse_color_mix(s + 10u, len - 11u, out_argb);
        }

        if (len > 7u && arcr_str_eq_nocase_n(s, 6u, "color(", 6u)) {
            /* CSS Color 4: color(srgb r g b / a) or color(display-p3 r g b / a) */
            const char *inner = s + 6u;
            size_t      ilen  = len - 7u;
            arcr_str_trim_view(&inner, &ilen);
            size_t sp = 0u;
            while (sp < ilen && !is_css_ws(inner[sp])) ++sp;
            if (sp < ilen) {
                const char *parts[4];
                size_t      plens[4];
                size_t n = split_color_args(inner + sp, ilen - sp, parts, plens);
                if (n >= 3u) {
                    int32_t rgb[3] = {0, 0, 0};
                    for (int c = 0; c < 3; ++c) {
                        size_t cl = plens[c];
                        bool pct  = (cl > 0u && parts[c][cl - 1u] == '%');
                        if (pct) --cl;
                        double v = 0.0;
                        if (!parse_css_double(parts[c], cl, &v, NULL)) return false;
                        if (pct) v /= 100.0;
                        rgb[c] = (int32_t)ARCR_CLAMP((int)(v * 255.0 + 0.5), 0, 255);
                    }
                    int32_t a = (n >= 4u) ? parse_alpha_component(parts[3], plens[3]) : 255;
                    *out_argb = ((uint32_t)a << 24) | ((uint32_t)rgb[0] << 16) |
                                ((uint32_t)rgb[1] << 8) | (uint32_t)rgb[2];
                    return true;
                }
            }
        }
    }

    /* Full Standard CSS Color Module Level 4 Named Color Table (148 keywords + transparent) */
    static const struct {
        const char *name;
        uint32_t    argb;
    } kNamed[] = {
        {"transparent",          0x00000000u},
        {"aliceblue",            0xFFF0F8FFu}, {"antiquewhite",         0xFFFAEBD7u},
        {"aqua",                 0xFF00FFFFu}, {"aquamarine",           0xFF7FFFD4u},
        {"azure",                0xFFF0FFFFu}, {"beige",                0xFFF5F5DCu},
        {"bisque",               0xFFFFE4C4u}, {"black",                0xFF000000u},
        {"blanchedalmond",       0xFFFFEBCDu}, {"blue",                 0xFF0000FFu},
        {"blueviolet",           0xFF8A2BE2u}, {"brown",                0xFFA52A2Au},
        {"burlywood",            0xFFDEB887u}, {"cadetblue",            0xFF5F9EA0u},
        {"chartreuse",           0xFF7FFF00u}, {"chocolate",            0xFFD2691Eu},
        {"coral",                0xFFFF7F50u}, {"cornflowerblue",       0xFF6495EDu},
        {"cornsilk",             0xFFFFF8DCu}, {"crimson",              0xFFDC143Cu},
        {"cyan",                 0xFF00FFFFu}, {"darkblue",             0xFF00008Bu},
        {"darkcyan",             0xFF008B8Bu}, {"darkgoldenrod",        0xFFB8860Bu},
        {"darkgray",             0xFFA9A9A9u}, {"darkgreen",            0xFF006400u},
        {"darkgrey",             0xFFA9A9A9u}, {"darkkhaki",            0xFFBDB76Bu},
        {"darkmagenta",          0xFF8B008Bu}, {"darkolivegreen",       0xFF556B2Fu},
        {"darkorange",           0xFFFF8C00u}, {"darkorchid",           0xFF9932CCu},
        {"darkred",              0xFF8B0000u}, {"darksalmon",           0xFFE9967Au},
        {"darkseagreen",         0xFF8FBC8Fu}, {"darkslateblue",        0xFF483D8Bu},
        {"darkslategray",        0xFF2F4F4Fu}, {"darkslategrey",        0xFF2F4F4Fu},
        {"darkturquoise",        0xFF00CED1u}, {"darkviolet",           0xFF9400D3u},
        {"deeppink",             0xFFFF1493u}, {"deepskyblue",          0xFF00BFFFu},
        {"dimgray",              0xFF696969u}, {"dimgrey",              0xFF696969u},
        {"dodgerblue",           0xFF1E90FFu}, {"firebrick",            0xFFB22222u},
        {"floralwhite",          0xFFFFFAF0u}, {"forestgreen",          0xFF228B22u},
        {"fuchsia",              0xFFFF00FFu}, {"gainsboro",            0xFFDCDCDCu},
        {"ghostwhite",           0xFFF8F8FFu}, {"gold",                 0xFFFFD700u},
        {"goldenrod",            0xFFDAA520u}, {"gray",                 0xFF808080u},
        {"green",                0xFF008000u}, {"greenyellow",          0xFFADFF2Fu},
        {"grey",                 0xFF808080u}, {"honeydew",             0xFFF0FFF0u},
        {"hotpink",              0xFFFF69B4u}, {"indianred",            0xFFCD5C5Cu},
        {"indigo",               0xFF4B0082u}, {"ivory",                0xFFFFFFF0u},
        {"khaki",                0xFFF0E68Cu}, {"lavender",             0xFFE6E6FAu},
        {"lavenderblush",        0xFFFFF0F5u}, {"lawngreen",            0xFF7CFC00u},
        {"lemonchiffon",         0xFFFFFACDu}, {"lightblue",            0xFFADD8E6u},
        {"lightcoral",           0xFFF08080u}, {"lightcyan",            0xFFE0FFFFu},
        {"lightgoldenrodyellow", 0xFFFAFAD2u}, {"lightgray",            0xFFD3D3D3u},
        {"lightgreen",           0xFF90EE90u}, {"lightgrey",            0xFFD3D3D3u},
        {"lightpink",            0xFFFFB6C1u}, {"lightsalmon",          0xFFFFA07Au},
        {"lightseagreen",        0xFF20B2AAu}, {"lightskyblue",         0xFF87CEFAu},
        {"lightslategray",       0xFF778899u}, {"lightslategrey",       0xFF778899u},
        {"lightsteelblue",       0xFFB0C4DEu}, {"lightyellow",          0xFFFFFFE0u},
        {"lime",                 0xFF00FF00u}, {"limegreen",            0xFF32CD32u},
        {"linen",                0xFFFAF0E6u}, {"magenta",              0xFFFF00FFu},
        {"maroon",               0xFF800000u}, {"mediumaquamarine",     0xFF66CDAAu},
        {"mediumblue",           0xFF0000CDu}, {"mediumorchid",         0xFFBA55D3u},
        {"mediumpurple",         0xFF9370DBu}, {"mediumseagreen",       0xFF3CB371u},
        {"mediumslateblue",      0xFF7B68EEu}, {"mediumspringgreen",    0xFF00FA9Au},
        {"mediumturquoise",      0xFF48D1CCu}, {"mediumvioletred",      0xFFC71585u},
        {"midnightblue",         0xFF191970u}, {"mintcream",            0xFFF5FFFAu},
        {"mistyrose",            0xFFFFE4E1u}, {"moccasin",             0xFFFFE4B5u},
        {"navajowhite",          0xFFFFDEADu}, {"navy",                 0xFF000080u},
        {"oldlace",              0xFFFDF5E6u}, {"olive",                0xFF808000u},
        {"olivedrab",            0xFF6B8E23u}, {"orange",               0xFFFFA500u},
        {"orangered",            0xFFFF4500u}, {"orchid",               0xFFDA70D6u},
        {"palegoldenrod",        0xFFEEE8AAu}, {"palegreen",            0xFF98FB98u},
        {"paleturquoise",        0xFFAFEEEEu}, {"palevioletred",        0xFFDB7093u},
        {"papayawhip",           0xFFFFEFD5u}, {"peachpuff",            0xFFFFDAB9u},
        {"peru",                 0xFFCD853Fu}, {"pink",                 0xFFFFC0CBu},
        {"plum",                 0xFFDDA0DDu}, {"powderblue",           0xFFB0E0E6u},
        {"purple",               0xFF800080u}, {"rebeccapurple",        0xFF663399u},
        {"red",                  0xFFFF0000u}, {"rosybrown",            0xFFBC8F8Fu},
        {"royalblue",            0xFF4169E1u}, {"saddlebrown",          0xFF8B4513u},
        {"salmon",               0xFFFA8072u}, {"sandybrown",           0xFFF4A460u},
        {"seagreen",             0xFF2E8B57u}, {"seashell",             0xFFFFF5EEu},
        {"sienna",               0xFFA0522Du}, {"silver",               0xFFC0C0C0u},
        {"skyblue",              0xFF87CEEBu}, {"slateblue",            0xFF6A5ACDu},
        {"slategray",            0xFF708090u}, {"slategrey",            0xFF708090u},
        {"snow",                 0xFFFFFAFAu}, {"springgreen",          0xFF00FF7Fu},
        {"steelblue",            0xFF4682B4u}, {"tan",                  0xFFD2B48Cu},
        {"teal",                 0xFF008080u}, {"thistle",              0xFFD8BFD8u},
        {"tomato",               0xFFFF6347u}, {"turquoise",            0xFF40E0D0u},
        {"violet",               0xFFEE82EEu}, {"wheat",                0xFFF5DEB3u},
        {"white",                0xFFFFFFFFu}, {"whitesmoke",           0xFFF5F5F5u},
        {"yellow",               0xFFFFFF00u}, {"yellowgreen",          0xFF9ACD32u}
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

static bool parse_color_with_current(const char *s, size_t len,
                                     uint32_t current_color, uint32_t *out_argb) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;
    if (arcr_str_eq_nocase_n(s, len, "currentColor", 12u)) {
        *out_argb = current_color;
        return true;
    }
    /* Support color-mix(..., currentColor, ...) by substituting currentColor inline if present */
    if (len > 11u && arcr_str_eq_nocase_n(s, 10u, "color-mix(", 10u)) {
        char buf[256];
        size_t pos = 0u;
        size_t i   = 0u;
        bool replaced = false;
        while (i < len && pos + 12u < sizeof(buf)) {
            if (i + 12u <= len && arcr_str_eq_nocase_n(s + i, 12u, "currentColor", 12u)) {
                static const char kHex[] = "0123456789abcdef";
                buf[pos++] = '#';
                buf[pos++] = kHex[(current_color >> 20) & 0xFu];
                buf[pos++] = kHex[(current_color >> 16) & 0xFu];
                buf[pos++] = kHex[(current_color >> 12) & 0xFu];
                buf[pos++] = kHex[(current_color >> 8) & 0xFu];
                buf[pos++] = kHex[(current_color >> 4) & 0xFu];
                buf[pos++] = kHex[current_color & 0xFu];
                buf[pos++] = kHex[(current_color >> 28) & 0xFu];
                buf[pos++] = kHex[(current_color >> 24) & 0xFu];
                i += 12u;
                replaced = true;
                continue;
            }
            buf[pos++] = s[i++];
        }
        if (replaced && i == len) {
            buf[pos] = '\0';
            return arcr_css_parse_color(buf, pos, out_argb);
        }
    }
    return arcr_css_parse_color(s, len, out_argb);
}

/* Parses `linear-gradient(...)` into has_gradient, grad_start_argb, grad_end_argb, grad_angle_deg */
static bool parse_linear_gradient_into_style(const char *s, size_t len,
                                             uint32_t current_color,
                                             ArcrComputedStyle *st) {
    size_t lg_pos = len;
    for (size_t i = 0u; i + 16u <= len; ++i) {
        if (arcr_str_eq_nocase_n(s + i, 16u, "linear-gradient(", 16u)) {
            lg_pos = i;
            break;
        }
    }
    if (lg_pos >= len) return false;

    size_t inner_start = lg_pos + 16u;
    size_t j           = inner_start;
    int    paren       = 1;
    while (j < len && paren > 0) {
        if (s[j] == '(') ++paren;
        else if (s[j] == ')') --paren;
        if (paren > 0) ++j;
    }
    if (j >= len) return false;

    const char *inner = s + inner_start;
    size_t      ilen  = j - inner_start;

    int32_t  angle_deg  = 180; /* CSS default: top to bottom (180deg) */
    uint32_t first_col  = 0u;
    uint32_t last_col   = 0u;
    size_t   stop_count = 0u;

    size_t pos = 0u;
    size_t arg_idx = 0u;
    while (pos < ilen) {
        size_t comma = pos;
        int p2 = 0;
        while (comma < ilen) {
            if (inner[comma] == '(') ++p2;
            else if (inner[comma] == ')' && p2 > 0) --p2;
            else if (inner[comma] == ',' && p2 == 0) break;
            ++comma;
        }
        const char *part = inner + pos;
        size_t      plen = comma - pos;
        pos = (comma < ilen) ? (comma + 1u) : ilen;
        arcr_str_trim_view(&part, &plen);
        if (plen == 0u) continue;

        if (arg_idx == 0u) {
            if (arcr_str_starts_with_nocase(part, "to ")) {
                bool left   = false, right  = false, top    = false, bottom = false;
                for (size_t k = 3u; k < plen; ++k) {
                    if (k + 4u <= plen && arcr_str_eq_nocase_n(part + k, 4u, "left", 4u)) left = true;
                    if (k + 5u <= plen && arcr_str_eq_nocase_n(part + k, 5u, "right", 5u)) right = true;
                    if (k + 3u <= plen && arcr_str_eq_nocase_n(part + k, 3u, "top", 3u)) top = true;
                    if (k + 6u <= plen && arcr_str_eq_nocase_n(part + k, 6u, "bottom", 6u)) bottom = true;
                }
                if (top && right)        angle_deg = 45;
                else if (bottom && right) angle_deg = 135;
                else if (bottom && left) angle_deg = 225;
                else if (top && left)    angle_deg = 315;
                else if (right)          angle_deg = 90;
                else if (left)           angle_deg = 270;
                else if (top)            angle_deg = 0;
                else                     angle_deg = 180;
                ++arg_idx;
                continue;
            }
            /* Check if first argument is an angle (`135deg`, `0.25turn`, etc.) or `in oklch` */
            if (arcr_str_starts_with_nocase(part, "in ")) {
                ++arg_idx;
                continue;
            }
            if ((plen > 3u && arcr_str_eq_nocase_n(part + plen - 3u, 3u, "deg", 3u)) ||
                (plen > 4u && arcr_str_eq_nocase_n(part + plen - 4u, 4u, "turn", 4u)) ||
                (plen > 3u && arcr_str_eq_nocase_n(part + plen - 3u, 3u, "rad", 3u)) ||
                (plen > 4u && arcr_str_eq_nocase_n(part + plen - 4u, 4u, "grad", 4u))) {
                angle_deg = (int32_t)(parse_angle_deg(part, plen) + 0.5);
                ++arg_idx;
                continue;
            }
        }
        ++arg_idx;

        /* Extract leading color token from stop (e.g. `#3b82f6 0%` or `oklch(0.6 0.2 250) 50%`) */
        size_t cend = 0u;
        int cparen = 0;
        while (cend < plen) {
            if (part[cend] == '(') ++cparen;
            else if (part[cend] == ')' && cparen > 0) {
                --cparen;
                if (cparen == 0) { ++cend; break; }
            } else if (cparen == 0 && is_css_ws(part[cend])) {
                break;
            }
            ++cend;
        }
        uint32_t stop_c = 0u;
        if (parse_color_with_current(part, cend, current_color, &stop_c)) {
            if (stop_count == 0u) first_col = stop_c;
            last_col = stop_c;
            ++stop_count;
        }
    }

    if (stop_count >= 2u) {
        st->has_gradient    = true;
        st->grad_start_argb = first_col;
        st->grad_end_argb   = last_col;
        st->grad_angle_deg  = angle_deg;
        if (((st->bg_argb >> 24) & 0xFFu) == 0u) {
            st->bg_argb = first_col;
        }
        return true;
    }
    return false;
}

static bool parse_background_color_smart(const char *s, size_t len,
                                         uint32_t current_color,
                                         ArcrComputedStyle *st) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;
    if (arcr_str_eq_nocase_n(s, len, "none", 4u) ||
        arcr_str_eq_nocase_n(s, len, "transparent", 11u)) {
        st->bg_argb      = 0x00000000u;
        st->has_gradient = false;
        return true;
    }

    if (parse_linear_gradient_into_style(s, len, current_color, st)) {
        return true;
    }

    uint32_t c = 0u;
    if (parse_color_with_current(s, len, current_color, &c)) {
        st->bg_argb      = c;
        st->has_gradient = false;
        return true;
    }

    /* Scan shorthand tokens or radial-gradient stops for a valid color */
    size_t i = 0u;
    while (i < len) {
        while (i < len && (is_css_ws(s[i]) || s[i] == ',')) ++i;
        if (i >= len) break;
        if (i + 16u <= len && arcr_str_eq_nocase_n(s + i, 16u, "radial-gradient(", 16u)) {
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
                if (paren == 0) { ++i; break; }
            } else if (paren == 0 && (is_css_ws(s[i]) || s[i] == ',')) {
                break;
            }
            ++i;
        }
        if (i > start && parse_color_with_current(s + start, i - start, current_color, &c)) {
            st->bg_argb = c;
            return true;
        }
        if (i < len && s[i] == ')') ++i;
    }
    return false;
}

/* -------------------------------------------------------------------------
 * 3. Selectors (Tailwind Backslash Escapes, :not/:is/:where, :nth-child, ~, +)
 * ------------------------------------------------------------------------- */
static bool parse_nth_expr(const char *s, size_t len, int32_t *out_a, int32_t *out_b) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;
    for (size_t k = 0u; k + 4u <= len; ++k) {
        if (is_css_ws(s[k]) && arcr_str_eq_nocase_n(s + k + 1u, 2u, "of", 2u) &&
            is_css_ws(s[k + 3u])) {
            len = k;
            arcr_str_trim_view(&s, &len);
            break;
        }
    }
    if (arcr_str_eq_nocase_n(s, len, "odd", 3u)) {
        *out_a = 2; *out_b = 1;
        return true;
    }
    if (arcr_str_eq_nocase_n(s, len, "even", 4u)) {
        *out_a = 2; *out_b = 0;
        return true;
    }

    size_t n_pos = len;
    for (size_t i = 0u; i < len; ++i) {
        if (s[i] == 'n' || s[i] == 'N') { n_pos = i; break; }
    }
    if (n_pos == len) {
        int32_t b = 0;
        if (!arcr_parse_i32(s, len, &b)) return false;
        *out_a = 0; *out_b = b;
        return true;
    }

    const char *ap = s;
    size_t      al = n_pos;
    arcr_str_trim_view(&ap, &al);
    int32_t a = 1;
    if (al == 0u || (al == 1u && ap[0] == '+'))      a = 1;
    else if (al == 1u && ap[0] == '-')               a = -1;
    else if (!arcr_parse_i32(ap, al, &a))            return false;

    const char *bp = s + n_pos + 1u;
    size_t      bl = len - n_pos - 1u;
    arcr_str_trim_view(&bp, &bl);
    int32_t b = 0;
    if (bl > 0u) {
        int32_t sign = 1;
        if (bp[0] == '+' || bp[0] == '-') {
            if (bp[0] == '-') sign = -1;
            ++bp; --bl;
            arcr_str_trim_view(&bp, &bl);
        }
        int32_t mag = 0;
        if (!arcr_parse_i32(bp, bl, &mag)) return false;
        b = sign * mag;
    }
    *out_a = a;
    *out_b = b;
    return true;
}

static void parse_compound_selector(ArcrArena *arena, ArcrAtomTable *atoms,
                                    const char *s, size_t len,
                                    CssCompoundSel *out, uint32_t *inout_spec,
                                    bool allow_functional);

static bool parse_selector_group_list(ArcrArena *arena, ArcrAtomTable *atoms,
                                      const char *inner, size_t ilen,
                                      CssSelectorGroup *out_grp, uint32_t *out_max_spec) {
    memset(out_grp, 0, sizeof(*out_grp));
    *out_max_spec = 0u;
    arcr_str_trim_view(&inner, &ilen);
    if (ilen == 0u) return false;

    CssCompoundSel tmp[16];
    uint8_t        cnt = 0u;
    size_t         pos = 0u;

    while (pos < ilen && cnt < 16u) {
        size_t comma = pos;
        int brk = 0, par = 0;
        char q = '\0';
        while (comma < ilen) {
            char c = inner[comma];
            if (q != '\0') {
                if (c == '\\' && comma + 1u < ilen) { comma += 2u; continue; }
                if (c == q) q = '\0';
            } else {
                if (c == '\\' && comma + 1u < ilen) { comma += 2u; continue; }
                if (c == '"' || c == '\'') q = c;
                else if (c == '[') ++brk;
                else if (c == ']' && brk > 0) --brk;
                else if (c == '(') ++par;
                else if (c == ')' && par > 0) --par;
                else if (c == ',' && brk == 0 && par == 0) break;
            }
            ++comma;
        }
        const char *one = inner + pos;
        size_t      ol  = comma - pos;
        pos = (comma < ilen) ? (comma + 1u) : ilen;
        arcr_str_trim_view(&one, &ol);
        if (ol == 0u) continue;

        bool has_comb = false;
        int b2 = 0, p2 = 0;
        char q2 = '\0';
        for (size_t k = 0u; k < ol; ++k) {
            char c = one[k];
            if (q2 != '\0') {
                if (c == '\\' && k + 1u < ol) { ++k; continue; }
                if (c == q2) q2 = '\0';
            } else {
                if (c == '\\' && k + 1u < ol) { ++k; continue; }
                if (c == '"' || c == '\'') q2 = c;
                else if (c == '[') ++b2;
                else if (c == ']' && b2 > 0) --b2;
                else if (c == '(') ++p2;
                else if (c == ')' && p2 > 0) --p2;
                else if (b2 == 0 && p2 == 0 &&
                         (is_css_ws(c) || c == '>' || c == '+' || c == '~')) {
                    has_comb = true;
                    break;
                }
            }
        }
        if (has_comb) continue;

        uint32_t sp = 0u;
        parse_compound_selector(arena, atoms, one, ol, &tmp[cnt], &sp, false);
        if (!tmp[cnt].impossible) {
            if (sp > *out_max_spec) *out_max_spec = sp;
            ++cnt;
        }
    }
    if (cnt == 0u) return false;

    out_grp->alts = (CssCompoundSel *)arcr_arena_alloc(
        arena, (size_t)cnt * sizeof(CssCompoundSel), sizeof(void *));
    if (!out_grp->alts) return false;
    memcpy(out_grp->alts, tmp, (size_t)cnt * sizeof(CssCompoundSel));
    out_grp->alt_count = cnt;
    return true;
}

static void parse_attr_condition(ArcrArena *arena, ArcrAtomTable *atoms,
                                 const char *inner, size_t ilen,
                                 CssAttrCond *ac) {
    memset(ac, 0, sizeof(*ac));
    arcr_str_trim_view(&inner, &ilen);
    if (ilen == 0u) return;

    size_t i = 0u;
    char name_buf[128];
    size_t nlen = read_unescaped_css_ident(inner, ilen, &i, name_buf, sizeof(name_buf));
    if (nlen == 0u) return;
    ac->name = arcr_atom_intern_lower(atoms, name_buf, nlen);

    while (i < ilen && is_css_ws(inner[i])) ++i;
    if (i >= ilen) {
        ac->op = CSS_ATTR_EXISTS;
        return;
    }

    CssAttrOp op = CSS_ATTR_EQ;
    if (inner[i] == '=') {
        op = CSS_ATTR_EQ;
        ++i;
    } else if (i + 1u < ilen && inner[i + 1u] == '=') {
        char p = inner[i];
        if (p == '~')      op = CSS_ATTR_INCLUDES;
        else if (p == '|') op = CSS_ATTR_DASH;
        else if (p == '^') op = CSS_ATTR_PREFIX;
        else if (p == '$') op = CSS_ATTR_SUFFIX;
        else if (p == '*') op = CSS_ATTR_SUBSTR;
        else return;
        i += 2u;
    } else {
        return;
    }
    ac->op = op;

    while (i < ilen && is_css_ws(inner[i])) ++i;
    const char *vp = inner + i;
    size_t      vl = ilen - i;
    arcr_str_trim_view(&vp, &vl);

    if (vl >= 2u && is_css_ws(vp[vl - 2u])) {
        char mod = vp[vl - 1u];
        if (mod == 'i' || mod == 'I') {
            ac->ignore_case = true;
            vl -= 2u;
            arcr_str_trim_view(&vp, &vl);
        } else if (mod == 's' || mod == 'S') {
            vl -= 2u;
            arcr_str_trim_view(&vp, &vl);
        }
    }

    if (vl >= 2u && ((vp[0] == '"' && vp[vl - 1u] == '"') ||
                     (vp[0] == '\'' && vp[vl - 1u] == '\''))) {
        vp += 1u;
        vl -= 2u;
    }
    ac->val     = arcr_arena_strdup(arena, vp, vl);
    ac->val_len = vl;
}

static void parse_compound_selector(ArcrArena *arena, ArcrAtomTable *atoms,
                                    const char *s, size_t len,
                                    CssCompoundSel *out, uint32_t *inout_spec,
                                    bool allow_functional) {
    memset(out, 0, sizeof(*out));
    arcr_str_trim_view(&s, &len);
    if (len == 0u) {
        out->impossible = true;
        return;
    }

    CssSelectorGroup not_tmp[4];
    CssSelectorGroup is_tmp[4];
    uint8_t not_cnt = 0u;
    uint8_t is_cnt  = 0u;

    size_t i = 0u;
    if (s[0] == '*') {
        ++i;
    } else if (is_css_raw_ident_char(s[0]) || s[0] == '\\') {
        char tag_buf[128];
        size_t tlen = read_unescaped_css_ident(s, len, &i, tag_buf, sizeof(tag_buf));
        if (tlen > 0u) {
            out->tag = arcr_atom_intern_lower(atoms, tag_buf, tlen);
            *inout_spec += 1u;
        }
    }

    while (i < len) {
        char c = s[i];
        if (c == '#') {
            ++i;
            char id_buf[160];
            size_t ilen = read_unescaped_css_ident(s, len, &i, id_buf, sizeof(id_buf));
            if (ilen > 0u) {
                out->id = arcr_atom_intern(atoms, id_buf, ilen);
                *inout_spec += 10000u;
            } else {
                out->impossible = true;
                return;
            }
        } else if (c == '.') {
            ++i;
            char cls_buf[256];
            size_t clen = read_unescaped_css_ident(s, len, &i, cls_buf, sizeof(cls_buf));
            if (clen > 0u) {
                if (out->class_count < ARCR_CSS_STEP_CLASSES) {
                    out->classes[out->class_count++] = arcr_atom_intern(atoms, cls_buf, clen);
                }
                *inout_spec += 100u;
            } else {
                out->impossible = true;
                return;
            }
        } else if (c == '[') {
            ++i;
            size_t close_b = i;
            char q = '\0';
            while (close_b < len) {
                char bc = s[close_b];
                if (q != '\0') {
                    if (bc == '\\' && close_b + 1u < len) { close_b += 2u; continue; }
                    if (bc == q) q = '\0';
                } else {
                    if (bc == '\\' && close_b + 1u < len) { close_b += 2u; continue; }
                    if (bc == '"' || bc == '\'') q = bc;
                    else if (bc == ']') break;
                }
                ++close_b;
            }
            if (close_b >= len) {
                out->impossible = true;
                return;
            }
            if (out->attr_count < ARCR_CSS_STEP_ATTRS) {
                CssAttrCond ac;
                parse_attr_condition(arena, atoms, s + i, close_b - i, &ac);
                if (ac.name != ARCR_ATOM_NULL) {
                    out->attrs[out->attr_count++] = ac;
                    *inout_spec += 100u;
                }
            }
            i = close_b + 1u;
        } else if (c == ':') {
            ++i;
            if (i < len && s[i] == ':') {
                out->impossible = true;
                return;
            }
            char ps_buf[64];
            size_t plen = read_unescaped_css_ident(s, len, &i, ps_buf, sizeof(ps_buf));
            const char *arg_ptr = NULL;
            size_t      arg_len = 0u;
            if (i < len && s[i] == '(') {
                ++i;
                size_t astart = i;
                int depth = 1;
                char q = '\0';
                while (i < len && depth > 0) {
                    char pc = s[i];
                    if (q != '\0') {
                        if (pc == '\\' && i + 1u < len) { i += 2u; continue; }
                        if (pc == q) q = '\0';
                    } else {
                        if (pc == '\\' && i + 1u < len) { i += 2u; continue; }
                        if (pc == '"' || pc == '\'') q = pc;
                        else if (pc == '(') ++depth;
                        else if (pc == ')') --depth;
                    }
                    if (depth > 0) ++i;
                }
                if (i >= len) {
                    out->impossible = true;
                    return;
                }
                arg_ptr = s + astart;
                arg_len = i - astart;
                ++i;
            }

            if (arcr_str_eq_nocase_n(ps_buf, plen, "before", 6u) ||
                arcr_str_eq_nocase_n(ps_buf, plen, "after", 5u) ||
                arcr_str_eq_nocase_n(ps_buf, plen, "first-line", 10u) ||
                arcr_str_eq_nocase_n(ps_buf, plen, "first-letter", 12u)) {
                out->impossible = true;
                return;
            }

            if (arcr_str_eq_nocase_n(ps_buf, plen, "where", 5u)) {
                if (allow_functional && arg_ptr && is_cnt < 4u) {
                    uint32_t dummy_sp = 0u;
                    if (parse_selector_group_list(arena, atoms, arg_ptr, arg_len,
                                                  &is_tmp[is_cnt], &dummy_sp)) {
                        ++is_cnt;
                    } else {
                        out->impossible = true;
                        return;
                    }
                }
                continue;
            }
            if (arcr_str_eq_nocase_n(ps_buf, plen, "is", 2u) ||
                arcr_str_eq_nocase_n(ps_buf, plen, "matches", 7u)) {
                if (allow_functional && arg_ptr && is_cnt < 4u) {
                    uint32_t max_sp = 0u;
                    if (parse_selector_group_list(arena, atoms, arg_ptr, arg_len,
                                                  &is_tmp[is_cnt], &max_sp)) {
                        *inout_spec += max_sp;
                        ++is_cnt;
                    } else {
                        out->impossible = true;
                        return;
                    }
                }
                continue;
            }
            if (arcr_str_eq_nocase_n(ps_buf, plen, "not", 3u)) {
                if (allow_functional && arg_ptr && not_cnt < 4u) {
                    uint32_t max_sp = 0u;
                    if (parse_selector_group_list(arena, atoms, arg_ptr, arg_len,
                                                  &not_tmp[not_cnt], &max_sp)) {
                        *inout_spec += max_sp;
                        ++not_cnt;
                    }
                }
                continue;
            }

            *inout_spec += 100u;
            if (arcr_str_eq_nocase_n(ps_buf, plen, "hover", 5u)) {
                out->req_state |= ARCR_STATE_HOVER;
            } else if (arcr_str_eq_nocase_n(ps_buf, plen, "focus", 5u) ||
                       arcr_str_eq_nocase_n(ps_buf, plen, "focus-visible", 13u)) {
                out->req_state |= ARCR_STATE_FOCUS;
            } else if (arcr_str_eq_nocase_n(ps_buf, plen, "focus-within", 12u)) {
                out->req_focus_within = true;
            } else if (arcr_str_eq_nocase_n(ps_buf, plen, "active", 6u)) {
                out->req_state |= ARCR_STATE_ACTIVE;
            } else if (arcr_str_eq_nocase_n(ps_buf, plen, "checked", 7u)) {
                out->req_state |= ARCR_STATE_CHECKED;
            } else if (arcr_str_eq_nocase_n(ps_buf, plen, "visited", 7u)) {
                out->req_state |= ARCR_STATE_VISITED;
            } else if (arcr_str_eq_nocase_n(ps_buf, plen, "disabled", 8u)) {
                out->req_disabled = true;
            } else if (arcr_str_eq_nocase_n(ps_buf, plen, "enabled", 7u)) {
                out->req_enabled = true;
            } else if (arcr_str_eq_nocase_n(ps_buf, plen, "link", 4u) ||
                       arcr_str_eq_nocase_n(ps_buf, plen, "any-link", 8u)) {
                /* Matches links */
            } else if (arcr_str_eq_nocase_n(ps_buf, plen, "root", 4u)) {
                out->req_root = true;
            } else if (arcr_str_eq_nocase_n(ps_buf, plen, "empty", 5u)) {
                out->req_empty = true;
            } else if (arcr_str_eq_nocase_n(ps_buf, plen, "first-child", 11u)) {
                out->req_first_child = true;
            } else if (arcr_str_eq_nocase_n(ps_buf, plen, "last-child", 10u)) {
                out->req_last_child = true;
            } else if (arcr_str_eq_nocase_n(ps_buf, plen, "only-child", 10u)) {
                out->req_only_child = true;
            } else if (arcr_str_eq_nocase_n(ps_buf, plen, "nth-child", 9u)) {
                if (!arg_ptr || !parse_nth_expr(arg_ptr, arg_len, &out->nth_a, &out->nth_b)) {
                    out->impossible = true;
                    return;
                }
                out->has_nth = true;
            } else {
                out->impossible = true;
                return;
            }
        } else {
            out->impossible = true;
            return;
        }
    }

    if (not_cnt > 0u) {
        out->not_groups = (CssSelectorGroup *)arcr_arena_alloc(
            arena, (size_t)not_cnt * sizeof(CssSelectorGroup), sizeof(void *));
        if (out->not_groups) {
            memcpy(out->not_groups, not_tmp, (size_t)not_cnt * sizeof(CssSelectorGroup));
            out->not_count = not_cnt;
        }
    }
    if (is_cnt > 0u) {
        out->is_groups = (CssSelectorGroup *)arcr_arena_alloc(
            arena, (size_t)is_cnt * sizeof(CssSelectorGroup), sizeof(void *));
        if (out->is_groups) {
            memcpy(out->is_groups, is_tmp, (size_t)is_cnt * sizeof(CssSelectorGroup));
            out->is_count = is_cnt;
        }
    }
}

static bool step_uses_state(const CssCompoundSel *step) {
    if (step->req_state != 0u || step->req_focus_within) return true;
    for (uint8_t g = 0u; g < step->not_count; ++g) {
        for (uint8_t a = 0u; a < step->not_groups[g].alt_count; ++a) {
            if (step->not_groups[g].alts[a].req_state != 0u ||
                step->not_groups[g].alts[a].req_focus_within) return true;
        }
    }
    for (uint8_t g = 0u; g < step->is_count; ++g) {
        for (uint8_t a = 0u; a < step->is_groups[g].alt_count; ++a) {
            if (step->is_groups[g].alts[a].req_state != 0u ||
                step->is_groups[g].alts[a].req_focus_within) return true;
        }
    }
    return false;
}

static bool parse_selector_chain(ArcrArena *arena, ArcrAtomTable *atoms,
                                 const char *one, size_t olen,
                                 ArcrCssRule *rule) {
    arcr_str_trim_view(&one, &olen);
    if (olen == 0u) return false;

    uint32_t spec = 0u;
    size_t   i    = 0u;

    if (one[0] == '&') {
        ++i;
        while (i < olen && is_css_ws(one[i])) ++i;
    }

    while (i < olen && rule->step_count < ARCR_CSS_MAX_STEPS) {
        while (i < olen && is_css_ws(one[i])) ++i;
        if (i >= olen) break;

        size_t start = i;
        int bracket  = 0;
        int paren    = 0;
        char quote   = '\0';
        while (i < olen) {
            char c = one[i];
            if (quote != '\0') {
                if (c == '\\' && i + 1u < olen) { i += 2u; continue; }
                if (c == quote) quote = '\0';
            } else {
                if (c == '\\' && i + 1u < olen) { i += 2u; continue; }
                if (c == '"' || c == '\'') quote = c;
                else if (c == '[') ++bracket;
                else if (c == ']' && bracket > 0) --bracket;
                else if (c == '(') ++paren;
                else if (c == ')' && paren > 0) --paren;
                else if (bracket == 0 && paren == 0 &&
                         (is_css_ws(c) || c == '>' || c == '+' || c == '~')) {
                    break;
                }
            }
            ++i;
        }
        if (i == start) return false;

        CssCompoundSel *step = &rule->steps[rule->step_count];
        parse_compound_selector(arena, atoms, one + start, i - start, step, &spec, true);
        if (step->impossible) return false;
        if (step_uses_state(step)) rule->uses_state_pseudo = true;

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
            else                comb = CSS_COMB_SIBLING;
            while (i < olen && is_css_ws(one[i])) ++i;
        } else if (saw_ws && i < olen) {
            comb = CSS_COMB_DESCENDANT;
        }
        step->comb_to_next = comb;
        rule->step_count++;
    }

    if (rule->step_count == 0u || i < olen) return false;
    rule->specificity = spec;

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
            rule->steps[s].req_only_child) {
            sheet->uses_first_or_last_child = true;
        }
        if (rule->steps[s].has_nth || rule->steps[s].req_empty ||
            rule->steps[s].comb_to_next == CSS_COMB_ADJACENT ||
            rule->steps[s].comb_to_next == CSS_COMB_SIBLING) {
            sheet->uses_positional_or_sibling = true;
        }
    }

    const CssCompoundSel *key = &rule->steps[rule->step_count - 1u];
    ArcrCssRule **bucket      = &sheet->universal_list;

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

/* -------------------------------------------------------------------------
 * 4. Declarations, @media (Level 4 Ranges), @supports, @layer, @keyframes
 * ------------------------------------------------------------------------- */
static ArcrCssDecl *parse_declarations(ArcrArena *target_arena, ArcrAtomTable *atoms,
                                       ArcrStylesheet *sheet,
                                       const char *block, size_t len, bool collect_global_vars) {
    ArcrCssDecl *head = NULL;
    ArcrCssDecl *tail = NULL;
    size_t i = 0u;

    while (i < len) {
        size_t semi  = i;
        int    paren = 0;
        int    brace = 0;
        char   quote = '\0';
        while (semi < len) {
            char c = block[semi];
            if (quote != '\0') {
                if (c == '\\' && semi + 1u < len) { semi += 2u; continue; }
                if (c == quote) quote = '\0';
            } else {
                if (c == '\\' && semi + 1u < len) { semi += 2u; continue; }
                if (c == '"' || c == '\'') quote = c;
                else if (c == '(') ++paren;
                else if (c == ')' && paren > 0) --paren;
                else if (c == '{') ++brace;
                else if (c == '}' && brace > 0) --brace;
                else if (c == ';' && paren == 0 && brace == 0) break;
            }
            ++semi;
        }
        const char *stmt = block + i;
        size_t      slen = semi - i;
        i = (semi < len) ? (semi + 1u) : len;

        bool has_brace = false;
        char q2 = '\0';
        for (size_t k = 0u; k < slen; ++k) {
            char c = stmt[k];
            if (q2 != '\0') {
                if (c == '\\' && k + 1u < slen) { ++k; continue; }
                if (c == q2) q2 = '\0';
            } else {
                if (c == '"' || c == '\'') q2 = c;
                else if (c == '{') { has_brace = true; break; }
            }
        }
        if (has_brace) continue;

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
        if (vlen >= 10u) {
            size_t bang = vlen;
            char q3 = '\0';
            for (size_t k = 0u; k < vlen; ++k) {
                if (q3 != '\0') {
                    if (pval[k] == '\\' && k + 1u < vlen) { ++k; continue; }
                    if (pval[k] == q3) q3 = '\0';
                } else {
                    if (pval[k] == '"' || pval[k] == '\'') q3 = pval[k];
                    else if (pval[k] == '!') bang = k;
                }
            }
            if (bang < vlen) {
                const char *imp = pval + bang + 1u;
                size_t      il  = vlen - bang - 1u;
                arcr_str_trim_view(&imp, &il);
                if (arcr_str_eq_nocase_n(imp, il, "important", 9u)) {
                    important = true;
                    vlen      = bang;
                    arcr_str_trim_view(&pval, &vlen);
                }
            }
        }
        if (vlen == 0u) continue;

        bool is_custom = (plen > 2u && pname[0] == '-' && pname[1] == '-');
        if (is_custom && sheet && collect_global_vars) {
            ArcrCssVar *v = (ArcrCssVar *)arcr_arena_alloc(
                &sheet->arena, sizeof(ArcrCssVar), sizeof(void *));
            if (v) {
                v->name          = arcr_atom_intern(atoms, pname, plen);
                v->val           = arcr_arena_strdup(&sheet->arena, pval, vlen);
                v->val_len       = vlen;
                v->next          = sheet->vars_head;
                sheet->vars_head = v;
            }
        }

        ArcrCssDecl *d = (ArcrCssDecl *)arcr_arena_alloc(
            target_arena, sizeof(ArcrCssDecl), sizeof(void *));
        if (!d) continue;
        d->prop      = is_custom
                           ? arcr_atom_intern(atoms, pname, plen)
                           : arcr_atom_intern_lower(atoms, pname, plen);
        d->val_str   = arcr_arena_strdup(target_arena, pval, vlen);
        d->val_len   = vlen;
        d->important = important;
        d->is_custom = is_custom;
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
    double v = 0.0;
    if (!parse_css_double(s, len, &v, NULL)) return 0;
    if (is_rem) v *= 16.0;
    return clamp_css_px_f(v);
}

static bool eval_cmp_op(int32_t lhs, int32_t rhs, const char *op, size_t oplen) {
    if (oplen == 2u && memcmp(op, ">=", 2u) == 0) return lhs >= rhs;
    if (oplen == 2u && memcmp(op, "<=", 2u) == 0) return lhs <= rhs;
    if (oplen == 1u && op[0] == '>')              return lhs > rhs;
    if (oplen == 1u && op[0] == '<')              return lhs < rhs;
    if (oplen == 1u && op[0] == '=')              return lhs == rhs;
    return true;
}

static int32_t eval_media_term(const char *t, size_t tl, const ArcrCssMediaEnv *env) {
    arcr_str_trim_view(&t, &tl);
    if (arcr_str_eq_nocase_n(t, tl, "width", 5u) ||
        arcr_str_eq_nocase_n(t, tl, "inline-size", 11u)) {
        return env->viewport_width;
    }
    if (arcr_str_eq_nocase_n(t, tl, "height", 6u) ||
        arcr_str_eq_nocase_n(t, tl, "block-size", 10u)) {
        return env->viewport_height;
    }
    return parse_media_px(t, tl);
}

static bool eval_single_media_feature(const char *feat, size_t flen, const ArcrCssMediaEnv *env) {
    arcr_str_trim_view(&feat, &flen);
    if (flen == 0u) return true;

    bool negate = false;
    if (flen > 4u && arcr_str_eq_nocase_n(feat, 3u, "not", 3u) &&
        (is_css_ws(feat[3]) || feat[3] == '(')) {
        negate = true;
        feat  += 3u;
        flen  -= 3u;
        arcr_str_trim_view(&feat, &flen);
    } else if (flen > 5u && arcr_str_eq_nocase_n(feat, 4u, "only", 4u) &&
               is_css_ws(feat[4])) {
        feat += 4u;
        flen -= 4u;
        arcr_str_trim_view(&feat, &flen);
    }

    if (flen >= 2u && feat[0] == '(' && feat[flen - 1u] == ')') {
        int d = 0;
        bool outer_match = true;
        for (size_t k = 0u; k + 1u < flen; ++k) {
            if (feat[k] == '(') ++d;
            else if (feat[k] == ')') {
                --d;
                if (d == 0) { outer_match = false; break; }
            }
        }
        if (outer_match) {
            feat += 1u;
            flen -= 2u;
            arcr_str_trim_view(&feat, &flen);
            if (flen > 4u && arcr_str_eq_nocase_n(feat, 3u, "not", 3u) &&
                (is_css_ws(feat[3]) || feat[3] == '(')) {
                negate = !negate;
                feat  += 3u;
                flen  -= 3u;
                arcr_str_trim_view(&feat, &flen);
                if (flen >= 2u && feat[0] == '(' && feat[flen - 1u] == ')') {
                    feat += 1u;
                    flen -= 2u;
                    arcr_str_trim_view(&feat, &flen);
                }
            }
        }
    }

    size_t op1_pos = flen, op1_len = 0u;
    for (size_t k = 0u; k < flen; ++k) {
        if (feat[k] == '<' || feat[k] == '>' || feat[k] == '=') {
            op1_pos = k;
            op1_len = (k + 1u < flen && feat[k + 1u] == '=') ? 2u : 1u;
            break;
        }
    }
    if (op1_pos < flen) {
        size_t after1  = op1_pos + op1_len;
        size_t op2_pos = flen, op2_len = 0u;
        for (size_t k = after1; k < flen; ++k) {
            if (feat[k] == '<' || feat[k] == '>' || feat[k] == '=') {
                op2_pos = k;
                op2_len = (k + 1u < flen && feat[k + 1u] == '=') ? 2u : 1u;
                break;
            }
        }
        bool res = true;
        if (op2_pos < flen) {
            int32_t t0 = eval_media_term(feat, op1_pos, env);
            int32_t t1 = eval_media_term(feat + after1, op2_pos - after1, env);
            int32_t t2 = eval_media_term(feat + op2_pos + op2_len,
                                         flen - (op2_pos + op2_len), env);
            res = eval_cmp_op(t0, t1, feat + op1_pos, op1_len) &&
                  eval_cmp_op(t1, t2, feat + op2_pos, op2_len);
        } else {
            int32_t t0 = eval_media_term(feat, op1_pos, env);
            int32_t t1 = eval_media_term(feat + after1, flen - after1, env);
            res = eval_cmp_op(t0, t1, feat + op1_pos, op1_len);
        }
        return negate ? !res : res;
    }

    size_t colon = 0u;
    while (colon < flen && feat[colon] != ':') ++colon;
    bool res = true;
    if (colon >= flen) {
        if (arcr_str_eq_nocase_n(feat, flen, "print", 5u) ||
            arcr_str_eq_nocase_n(feat, flen, "speech", 6u)) {
            res = false;
        }
        return negate ? !res : res;
    }

    const char *k  = feat;
    size_t      kl = colon;
    const char *v  = feat + colon + 1u;
    size_t      vl = flen - colon - 1u;
    arcr_str_trim_view(&k, &kl);
    arcr_str_trim_view(&v, &vl);

    if (arcr_str_eq_nocase_n(k, kl, "max-width", 9u)) {
        res = (env->viewport_width <= parse_media_px(v, vl));
    } else if (arcr_str_eq_nocase_n(k, kl, "min-width", 9u)) {
        res = (env->viewport_width >= parse_media_px(v, vl));
    } else if (arcr_str_eq_nocase_n(k, kl, "width", 5u)) {
        res = (env->viewport_width == parse_media_px(v, vl));
    } else if (arcr_str_eq_nocase_n(k, kl, "max-height", 10u)) {
        res = (env->viewport_height <= parse_media_px(v, vl));
    } else if (arcr_str_eq_nocase_n(k, kl, "min-height", 10u)) {
        res = (env->viewport_height >= parse_media_px(v, vl));
    } else if (arcr_str_eq_nocase_n(k, kl, "orientation", 11u)) {
        if (arcr_str_eq_nocase_n(v, vl, "landscape", 9u)) res = (env->viewport_width >= env->viewport_height);
        else if (arcr_str_eq_nocase_n(v, vl, "portrait", 8u)) res = (env->viewport_height > env->viewport_width);
    } else if (arcr_str_eq_nocase_n(k, kl, "prefers-color-scheme", 20u)) {
        if (arcr_str_eq_nocase_n(v, vl, "dark", 4u))       res = env->prefers_dark;
        else if (arcr_str_eq_nocase_n(v, vl, "light", 5u)) res = !env->prefers_dark;
        else                                               res = false;
    } else if (arcr_str_eq_nocase_n(k, kl, "hover", 5u)) {
        res = !arcr_str_eq_nocase_n(v, vl, "none", 4u);
    } else if (arcr_str_eq_nocase_n(k, kl, "pointer", 7u)) {
        res = arcr_str_eq_nocase_n(v, vl, "fine", 4u);
    } else if (arcr_str_eq_nocase_n(k, kl, "prefers-reduced-motion", 22u)) {
        res = arcr_str_eq_nocase_n(v, vl, "no-preference", 13u);
    }
    return negate ? !res : res;
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

        bool branch_negate = false;
        if (blen > 4u && arcr_str_eq_nocase_n(branch, 3u, "not", 3u) &&
            (is_css_ws(branch[3]) || branch[3] == '(')) {
            branch_negate = true;
            branch       += 3u;
            blen         -= 3u;
            arcr_str_trim_view(&branch, &blen);
        }

        bool   branch_ok = true;
        size_t bpos      = 0u;
        while (bpos < blen) {
            size_t next_op  = blen;
            size_t next_end = blen;
            bool   is_or    = false;
            int    p2       = 0;
            for (size_t k = bpos; k < blen; ++k) {
                if (branch[k] == '(') ++p2;
                else if (branch[k] == ')' && p2 > 0) --p2;
                else if (p2 == 0 && k + 3u <= blen &&
                         (k == bpos || is_css_ws(branch[k - 1u]) || branch[k - 1u] == ')') &&
                         arcr_str_eq_nocase_n(branch + k, 3u, "and", 3u) &&
                         (k + 3u == blen || is_css_ws(branch[k + 3u]) || branch[k + 3u] == '(')) {
                    next_op  = k;
                    next_end = k + 3u;
                    break;
                } else if (p2 == 0 && k + 2u <= blen &&
                           (k == bpos || is_css_ws(branch[k - 1u]) || branch[k - 1u] == ')') &&
                           arcr_str_eq_nocase_n(branch + k, 2u, "or", 2u) &&
                           (k + 2u == blen || is_css_ws(branch[k + 2u]) || branch[k + 2u] == '(')) {
                    next_op  = k;
                    next_end = k + 2u;
                    is_or    = true;
                    break;
                }
            }
            bool term_ok = eval_single_media_feature(branch + bpos, next_op - bpos, env);
            if (is_or) {
                if (term_ok) { branch_ok = true; break; }
                branch_ok = false;
            } else {
                if (!term_ok) { branch_ok = false; break; }
                branch_ok = true;
            }
            bpos = next_end;
        }
        if (branch_negate) branch_ok = !branch_ok;
        if (branch_ok) return true;
    }
    return false;
}

static bool eval_supports_condition(const char *cond, size_t len) {
    arcr_str_trim_view(&cond, &len);
    if (len == 0u) return true;

    if (len > 4u && arcr_str_eq_nocase_n(cond, 3u, "not", 3u) &&
        (is_css_ws(cond[3]) || cond[3] == '(')) {
        return !eval_supports_condition(cond + 3u, len - 3u);
    }

    int paren = 0;
    for (size_t k = 0u; k < len; ++k) {
        if (cond[k] == '(') ++paren;
        else if (cond[k] == ')' && paren > 0) --paren;
        else if (paren == 0) {
            if (k + 2u <= len && (k == 0u || is_css_ws(cond[k - 1u]) || cond[k - 1u] == ')') &&
                arcr_str_eq_nocase_n(cond + k, 2u, "or", 2u) &&
                (k + 2u == len || is_css_ws(cond[k + 2u]) || cond[k + 2u] == '(')) {
                return eval_supports_condition(cond, k) ||
                       eval_supports_condition(cond + k + 2u, len - k - 2u);
            }
            if (k + 3u <= len && (k == 0u || is_css_ws(cond[k - 1u]) || cond[k - 1u] == ')') &&
                arcr_str_eq_nocase_n(cond + k, 3u, "and", 3u) &&
                (k + 3u == len || is_css_ws(cond[k + 3u]) || cond[k + 3u] == '(')) {
                return eval_supports_condition(cond, k) &&
                       eval_supports_condition(cond + k + 3u, len - k - 3u);
            }
        }
    }

    if (len >= 2u && cond[0] == '(' && cond[len - 1u] == ')') {
        return eval_supports_condition(cond + 1u, len - 2u);
    }

    if (arcr_str_starts_with_nocase(cond, "-webkit-touch-callout") ||
        arcr_str_starts_with_nocase(cond, "-moz-appearance")) {
        return false;
    }
    return true;
}

static void parse_transform_list(const char *s, size_t len, int32_t font_px,
                                 const ArcrCssMediaEnv *env,
                                 int32_t *out_tx_px, int32_t *out_ty_px,
                                 int32_t *out_tx_pct, int32_t *out_ty_pct,
                                 int32_t *out_sx_milli, int32_t *out_sy_milli,
                                 int32_t *out_rot_milli);

static int32_t parse_pos_offset_encoded(const char *s, size_t len, int32_t font_px,
                                        const ArcrCssMediaEnv *env, bool *out_has);

static void parse_keyframes_block(ArcrDocument *doc, ArcrStylesheet *sheet,
                                  const char *kf_name, size_t kf_nlen,
                                  const char *body, size_t blen) {
    arcr_str_trim_view(&kf_name, &kf_nlen);
    if (kf_nlen >= 2u && ((kf_name[0] == '"' && kf_name[kf_nlen - 1u] == '"') ||
                          (kf_name[0] == '\'' && kf_name[kf_nlen - 1u] == '\''))) {
        kf_name += 1u;
        kf_nlen -= 2u;
    }
    if (kf_nlen == 0u) return;

    ArcrKeyframeRule *kf = (ArcrKeyframeRule *)arcr_arena_calloc(
        &sheet->arena, 1u, sizeof(ArcrKeyframeRule), sizeof(void *));
    if (!kf) return;
    kf->name = arcr_atom_intern(doc->atoms, kf_name, kf_nlen);

    size_t i = 0u;
    while (i < blen) {
        while (i < blen && is_css_ws(body[i])) ++i;
        if (i >= blen) break;

        size_t brace = i;
        while (brace < blen && body[brace] != '{') ++brace;
        if (brace >= blen) break;

        const char *stop_sels = body + i;
        size_t      stop_slen = brace - i;

        size_t close_b = brace + 1u;
        int    depth   = 1;
        while (close_b < blen && depth > 0) {
            if (body[close_b] == '{') ++depth;
            else if (body[close_b] == '}') --depth;
            if (depth > 0) ++close_b;
        }
        const char *decls_s = body + brace + 1u;
        size_t      decls_l = (close_b > brace + 1u) ? (close_b - brace - 1u) : 0u;
        i = (close_b < blen) ? (close_b + 1u) : blen;

        ArcrCssDecl *decls = parse_declarations(&sheet->arena, doc->atoms, sheet,
                                                decls_s, decls_l, false);
        if (!decls) continue;

        ArcrKeyframeStop proto;
        memset(&proto, 0, sizeof(proto));
        proto.sx_milli = 1000;
        proto.sy_milli = 1000;

        for (const ArcrCssDecl *d = decls; d != NULL; d = d->next) {
            if (d->is_custom) continue;
            const char *p = arcr_atom_cstr(doc->atoms, d->prop);
            if (arcr_str_eq_nocase(p, "opacity")) {
                proto.has_opacity = true;
                proto.opacity     = (uint8_t)parse_alpha_component(d->val_str, d->val_len);
            } else if (arcr_str_eq_nocase(p, "background-color") ||
                       arcr_str_eq_nocase(p, "background")) {
                uint32_t c = 0u;
                if (arcr_css_parse_color(d->val_str, d->val_len, &c)) {
                    proto.has_bg  = true;
                    proto.bg_argb = c;
                }
            } else if (arcr_str_eq_nocase(p, "color")) {
                uint32_t c = 0u;
                if (arcr_css_parse_color(d->val_str, d->val_len, &c)) {
                    proto.has_color  = true;
                    proto.color_argb = c;
                }
            } else if (arcr_str_eq_nocase(p, "transform")) {
                proto.has_transform = true;
                parse_transform_list(d->val_str, d->val_len, 16, &sheet->env,
                                     &proto.tx_px, &proto.ty_px,
                                     &proto.tx_pct, &proto.ty_pct,
                                     &proto.sx_milli, &proto.sy_milli,
                                     &proto.rot_milli);
            } else if (arcr_str_eq_nocase(p, "top")) {
                bool has = false;
                int32_t v = parse_pos_offset_encoded(d->val_str, d->val_len, 16, &sheet->env, &has);
                if (has) { proto.has_top = true; proto.top_px = v; }
            } else if (arcr_str_eq_nocase(p, "left")) {
                bool has = false;
                int32_t v = parse_pos_offset_encoded(d->val_str, d->val_len, 16, &sheet->env, &has);
                if (has) { proto.has_left = true; proto.left_px = v; }
            }
        }

        size_t p = 0u;
        while (p < stop_slen) {
            size_t cpos = p;
            while (cpos < stop_slen && stop_sels[cpos] != ',') ++cpos;
            const char *tok = stop_sels + p;
            size_t      tl  = cpos - p;
            p = (cpos < stop_slen) ? (cpos + 1u) : stop_slen;
            arcr_str_trim_view(&tok, &tl);
            if (tl == 0u) continue;

            int32_t pct_milli = -1;
            if (arcr_str_eq_nocase_n(tok, tl, "from", 4u)) {
                pct_milli = 0;
            } else if (arcr_str_eq_nocase_n(tok, tl, "to", 2u)) {
                pct_milli = 100000;
            } else if (tok[tl - 1u] == '%') {
                double pv = 0.0;
                if (parse_css_double(tok, tl - 1u, &pv, NULL)) {
                    pct_milli = (int32_t)ARCR_CLAMP((int)(pv * 1000.0 + 0.5), 0, 100000);
                }
            }
            if (pct_milli < 0) continue;

            ArcrKeyframeStop *st = (ArcrKeyframeStop *)arcr_arena_alloc(
                &sheet->arena, sizeof(ArcrKeyframeStop), sizeof(void *));
            if (!st) continue;
            *st           = proto;
            st->pct_milli = pct_milli;
            st->next      = NULL;

            ArcrKeyframeStop **pp = &kf->stops_head;
            while (*pp && (*pp)->pct_milli <= pct_milli) {
                pp = &(*pp)->next;
            }
            st->next = *pp;
            *pp      = st;
        }
    }

    kf->next              = sheet->keyframes_head;
    sheet->keyframes_head = kf;
}

static void parse_property_at_rule(ArcrDocument *doc, ArcrStylesheet *sheet,
                                   const char *prop_name, size_t prop_len,
                                   const char *body, size_t blen) {
    arcr_str_trim_view(&prop_name, &prop_len);
    if (prop_len <= 2u || prop_name[0] != '-' || prop_name[1] != '-') return;

    ArcrCssDecl *decls = parse_declarations(&sheet->arena, doc->atoms, NULL, body, blen, false);
    for (const ArcrCssDecl *d = decls; d != NULL; d = d->next) {
        const char *dn = arcr_atom_cstr(doc->atoms, d->prop);
        if (arcr_str_eq_nocase(dn, "initial-value")) {
            ArcrCssVar *v = (ArcrCssVar *)arcr_arena_alloc(
                &sheet->arena, sizeof(ArcrCssVar), sizeof(void *));
            if (v) {
                v->name    = arcr_atom_intern(doc->atoms, prop_name, prop_len);
                v->val     = d->val_str;
                v->val_len = d->val_len;
                v->next    = NULL;
                if (!sheet->vars_head) {
                    sheet->vars_head = v;
                } else {
                    ArcrCssVar *t = sheet->vars_head;
                    while (t->next) t = t->next;
                    t->next = v;
                }
            }
            break;
        }
    }
}

static void parse_stylesheet_rules(ArcrDocument *doc, ArcrStylesheet *sheet,
                                   const char *clean, size_t clen, bool in_media) {
    size_t i = 0u;
    while (i < clen) {
        while (i < clen && (is_css_ws(clean[i]) || clean[i] == ';')) ++i;
        if (i >= clen) break;

        size_t brace = i;
        char   quote = '\0';
        int    paren = 0;
        bool   at_stmt_ended = false;
        while (brace < clen) {
            char c = clean[brace];
            if (quote != '\0') {
                if (c == '\\' && brace + 1u < clen) { brace += 2u; continue; }
                if (c == quote) quote = '\0';
            } else {
                if (c == '\\' && brace + 1u < clen) { brace += 2u; continue; }
                if (c == '"' || c == '\'') quote = c;
                else if (c == '(') ++paren;
                else if (c == ')' && paren > 0) --paren;
                else if (c == ';' && paren == 0 && clean[i] == '@') {
                    at_stmt_ended = true;
                    break;
                } else if (c == '{' && paren == 0) {
                    break;
                }
            }
            ++brace;
        }
        if (at_stmt_ended) {
            i = brace + 1u;
            continue;
        }
        if (brace >= clen) break;

        const char *sel_group = clean + i;
        size_t      sel_glen  = brace - i;

        size_t close_brace = brace + 1u;
        int    depth       = 1;
        quote              = '\0';
        while (close_brace < clen && depth > 0) {
            char c = clean[close_brace];
            if (quote != '\0') {
                if (c == '\\' && close_brace + 1u < clen) { close_brace += 2u; continue; }
                if (c == quote) quote = '\0';
            } else {
                if (c == '\\' && close_brace + 1u < clen) { close_brace += 2u; continue; }
                if (c == '"' || c == '\'') quote = c;
                else if (c == '{') ++depth;
                else if (c == '}') --depth;
            }
            if (depth > 0) ++close_brace;
        }
        const char *body_ptr = clean + brace + 1u;
        size_t      body_len = (close_brace > brace + 1u) ? (close_brace - brace - 1u) : 0u;
        i = (close_brace < clen) ? (close_brace + 1u) : clen;

        arcr_str_trim_view(&sel_group, &sel_glen);
        if (sel_glen == 0u) continue;

        if (sel_group[0] == '@') {
            if (sel_glen >= 6u && arcr_str_eq_nocase_n(sel_group, 6u, "@media", 6u)) {
                if (eval_media_query(sel_group + 6u, sel_glen - 6u, &sheet->env)) {
                    parse_stylesheet_rules(doc, sheet, body_ptr, body_len, true);
                }
            } else if (sel_glen >= 9u && arcr_str_eq_nocase_n(sel_group, 9u, "@supports", 9u)) {
                if (eval_supports_condition(sel_group + 9u, sel_glen - 9u)) {
                    parse_stylesheet_rules(doc, sheet, body_ptr, body_len, in_media);
                }
            } else if ((sel_glen >= 6u && arcr_str_eq_nocase_n(sel_group, 6u, "@layer", 6u)) ||
                       (sel_glen >= 10u && arcr_str_eq_nocase_n(sel_group, 10u, "@container", 10u)) ||
                       (sel_glen >= 6u && arcr_str_eq_nocase_n(sel_group, 6u, "@scope", 6u))) {
                parse_stylesheet_rules(doc, sheet, body_ptr, body_len, in_media);
            } else if (sel_glen > 10u && arcr_str_eq_nocase_n(sel_group, 10u, "@keyframes", 10u)) {
                parse_keyframes_block(doc, sheet, sel_group + 10u, sel_glen - 10u,
                                      body_ptr, body_len);
            } else if (sel_glen > 18u &&
                       arcr_str_eq_nocase_n(sel_group, 18u, "@-webkit-keyframes", 18u)) {
                parse_keyframes_block(doc, sheet, sel_group + 18u, sel_glen - 18u,
                                      body_ptr, body_len);
            } else if (sel_glen > 9u && arcr_str_eq_nocase_n(sel_group, 9u, "@property", 9u)) {
                parse_property_at_rule(doc, sheet, sel_group + 9u, sel_glen - 9u,
                                       body_ptr, body_len);
            }
            continue;
        }

        bool collect_global_vars = false;
        if (arcr_str_eq_nocase_n(sel_group, sel_glen, ":root", 5u) ||
            arcr_str_eq_nocase_n(sel_group, sel_glen, "html", 4u) ||
            arcr_str_eq_nocase_n(sel_group, sel_glen, ":host", 5u) ||
            arcr_str_eq_nocase_n(sel_group, sel_glen, "*", 1u)) {
            collect_global_vars = true;
        }
        for (size_t k = 0u; k + 5u <= sel_glen && !collect_global_vars; ++k) {
            if (arcr_str_eq_nocase_n(sel_group + k, 5u, ":root", 5u) ||
                arcr_str_eq_nocase_n(sel_group + k, 5u, ":host", 5u)) {
                collect_global_vars = true;
            }
        }

        ArcrCssDecl *decls = parse_declarations(&sheet->arena, doc->atoms, sheet,
                                                body_ptr, body_len, collect_global_vars);
        if (!decls) continue;

        size_t p = 0u;
        while (p < sel_glen) {
            size_t comma   = p;
            int    bracket = 0;
            int    par     = 0;
            char   q       = '\0';
            while (comma < sel_glen) {
                char c = sel_group[comma];
                if (q != '\0') {
                    if (c == '\\' && comma + 1u < sel_glen) { comma += 2u; continue; }
                    if (c == q) q = '\0';
                } else {
                    if (c == '\\' && comma + 1u < sel_glen) { comma += 2u; continue; }
                    if (c == '"' || c == '\'') q = c;
                    else if (c == '[') ++bracket;
                    else if (c == ']' && bracket > 0) --bracket;
                    else if (c == '(') ++par;
                    else if (c == ')' && par > 0) --par;
                    else if (c == ',' && bracket == 0 && par == 0) break;
                }
                ++comma;
            }
            const char *one  = sel_group + p;
            size_t      olen = comma - p;
            p = (comma < sel_glen) ? (comma + 1u) : sel_glen;
            arcr_str_trim_view(&one, &olen);
            if (olen == 0u) continue;

            ArcrCssRule *rule = (ArcrCssRule *)arcr_arena_calloc(
                &sheet->arena, 1u, sizeof(ArcrCssRule), sizeof(void *));
            if (!rule) continue;

            if (!parse_selector_chain(&sheet->arena, doc->atoms, one, olen, rule)) {
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
    ArcrStylesheet *sheet = (ArcrStylesheet *)calloc(1u, sizeof(ArcrStylesheet));
    if (!sheet) return NULL;
    arcr_arena_init(&sheet->arena, 65536u);

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
        char *clean = (char *)malloc(sb->css_len + 1u);
        if (!clean) continue;
        size_t clen = strip_css_comments(sb->css_text, sb->css_len, clean);
        parse_stylesheet_rules(doc, sheet, clean, clen, false);
        free(clean);
    }
    return sheet;
}

ArcrStylesheet *arcr_css_parse_document_styles(ArcrDocument *doc) {
    return arcr_css_parse_document_styles_ex(doc, NULL);
}

void arcr_css_destroy_stylesheet(ArcrStylesheet *sheet) {
    if (!sheet) return;
    arcr_arena_destroy(&sheet->arena);
    free(sheet);
}

/* -------------------------------------------------------------------------
 * 5. Selector Matching Engine (:nth-child, :not/:is/:where, [attr^=/$=/ *=], ~)
 * ------------------------------------------------------------------------- */
static bool is_first_element_child(const ArcrDomNode *el) {
    if (!el || !el->parent) return false;
    for (const ArcrDomNode *ch = el->prev_sibling; ch != NULL; ch = ch->prev_sibling) {
        if (ch->kind == ARCR_NODE_ELEMENT) return false;
    }
    return true;
}

static bool is_last_element_child(const ArcrDomNode *el) {
    if (!el || !el->parent) return false;
    for (const ArcrDomNode *ch = el->next_sibling; ch != NULL; ch = ch->next_sibling) {
        if (ch->kind == ARCR_NODE_ELEMENT) return false;
    }
    return true;
}

static const ArcrDomNode *prev_element_sibling(const ArcrDomNode *el) {
    if (!el) return NULL;
    for (const ArcrDomNode *ch = el->prev_sibling; ch != NULL; ch = ch->prev_sibling) {
        if (ch->kind == ARCR_NODE_ELEMENT) return ch;
    }
    return NULL;
}

static int32_t element_child_index_1based(const ArcrDomNode *el) {
    int32_t idx = 1;
    for (const ArcrDomNode *ch = el->prev_sibling; ch != NULL; ch = ch->prev_sibling) {
        if (ch->kind == ARCR_NODE_ELEMENT) ++idx;
    }
    return idx;
}

static bool nth_matches_index(int32_t idx, int32_t a, int32_t b) {
    if (a == 0) return idx == b;
    int32_t diff = idx - b;
    if (a > 0) return (diff >= 0) && ((diff % a) == 0);
    return (diff <= 0) && (((-diff) % (-a)) == 0);
}

static bool subtree_has_focus(const ArcrDomNode *node) {
    if ((node->state_flags & ARCR_STATE_FOCUS) != 0u) return true;
    for (const ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        if (ch->kind == ARCR_NODE_ELEMENT && subtree_has_focus(ch)) return true;
    }
    return false;
}

static bool css_str_match(const char *a, size_t alen, const char *b, size_t blen, bool icase) {
    if (alen != blen) return false;
    if (alen == 0u) return true;
    return icase ? arcr_str_eq_nocase_n(a, alen, b, blen) : (memcmp(a, b, alen) == 0);
}

static bool attr_cond_matches(const CssAttrCond *ac, const ArcrDomNode *el) {
    const char *v = arcr_dom_get_attr(el, ac->name);
    if (!v) return false;
    if (ac->op == CSS_ATTR_EXISTS) return true;

    size_t vlen = strlen(v);
    switch (ac->op) {
        case CSS_ATTR_EQ:
            return css_str_match(v, vlen, ac->val, ac->val_len, ac->ignore_case);
        case CSS_ATTR_PREFIX:
            if (ac->val_len == 0u || vlen < ac->val_len) return false;
            return css_str_match(v, ac->val_len, ac->val, ac->val_len, ac->ignore_case);
        case CSS_ATTR_SUFFIX:
            if (ac->val_len == 0u || vlen < ac->val_len) return false;
            return css_str_match(v + (vlen - ac->val_len), ac->val_len,
                                 ac->val, ac->val_len, ac->ignore_case);
        case CSS_ATTR_SUBSTR:
            if (ac->val_len == 0u || vlen < ac->val_len) return false;
            for (size_t k = 0u; k + ac->val_len <= vlen; ++k) {
                if (css_str_match(v + k, ac->val_len, ac->val, ac->val_len, ac->ignore_case)) {
                    return true;
                }
            }
            return false;
        case CSS_ATTR_DASH:
            if (css_str_match(v, vlen, ac->val, ac->val_len, ac->ignore_case)) return true;
            if (vlen > ac->val_len && v[ac->val_len] == '-' &&
                css_str_match(v, ac->val_len, ac->val, ac->val_len, ac->ignore_case)) {
                return true;
            }
            return false;
        case CSS_ATTR_INCLUDES: {
            if (ac->val_len == 0u) return false;
            size_t i = 0u;
            while (i < vlen) {
                while (i < vlen && is_css_ws(v[i])) ++i;
                size_t s = i;
                while (i < vlen && !is_css_ws(v[i])) ++i;
                if (i > s && css_str_match(v + s, i - s, ac->val, ac->val_len, ac->ignore_case)) {
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

static bool compound_matches(const ArcrDocument *doc, const CssCompoundSel *sel,
                             const ArcrDomNode *el, bool check_state) {
    if (!el || el->kind != ARCR_NODE_ELEMENT || sel->impossible) return false;
    if (sel->tag != ARCR_ATOM_NULL && el->tag != sel->tag) return false;
    if (sel->id != ARCR_ATOM_NULL && el->id != sel->id) return false;
    for (uint8_t c = 0u; c < sel->class_count; ++c) {
        if (!arcr_dom_has_class(el, sel->classes[c])) return false;
    }
    if (check_state) {
        if ((el->state_flags & sel->req_state) != sel->req_state) return false;
        if (sel->req_focus_within && !subtree_has_focus(el)) return false;
    }
    if (sel->req_disabled) {
        if (arcr_dom_get_attr_cstr(doc, el, "disabled") == NULL) return false;
    }
    if (sel->req_enabled) {
        if (arcr_dom_get_attr_cstr(doc, el, "disabled") != NULL) return false;
    }
    if (sel->req_root && (!el->parent || el->parent->kind != ARCR_NODE_DOCUMENT)) {
        return false;
    }
    if (sel->req_empty) {
        for (const ArcrDomNode *ch = el->first_child; ch != NULL; ch = ch->next_sibling) {
            if (ch->kind == ARCR_NODE_ELEMENT) return false;
            if (ch->kind == ARCR_NODE_TEXT && ch->text_len > 0u) return false;
        }
    }
    if (sel->req_first_child && !is_first_element_child(el)) return false;
    if (sel->req_last_child && !is_last_element_child(el)) return false;
    if (sel->req_only_child && (!is_first_element_child(el) || !is_last_element_child(el))) {
        return false;
    }
    if (sel->has_nth) {
        if (!el->parent) return false;
        if (!nth_matches_index(element_child_index_1based(el), sel->nth_a, sel->nth_b)) {
            return false;
        }
    }

    for (uint8_t a = 0u; a < sel->attr_count; ++a) {
        if (!attr_cond_matches(&sel->attrs[a], el)) return false;
    }

    for (uint8_t g = 0u; g < sel->not_count; ++g) {
        const CssSelectorGroup *grp = &sel->not_groups[g];
        for (uint8_t k = 0u; k < grp->alt_count; ++k) {
            if (compound_matches(doc, &grp->alts[k], el, check_state)) {
                return false;
            }
        }
    }

    for (uint8_t g = 0u; g < sel->is_count; ++g) {
        const CssSelectorGroup *grp = &sel->is_groups[g];
        bool any_ok = false;
        for (uint8_t k = 0u; k < grp->alt_count; ++k) {
            if (compound_matches(doc, &grp->alts[k], el, check_state)) {
                any_ok = true;
                break;
            }
        }
        if (!any_ok) return false;
    }

    return true;
}

static bool chain_matches_at(const ArcrDocument *doc, const ArcrCssRule *rule,
                             uint8_t idx, const ArcrDomNode *el, bool check_state) {
    if (!compound_matches(doc, &rule->steps[idx], el, check_state)) return false;
    if (idx == 0u) return true;

    CssCombinator comb = rule->steps[idx - 1u].comb_to_next;
    if (comb == CSS_COMB_CHILD) {
        if (!el->parent || el->parent->kind != ARCR_NODE_ELEMENT) return false;
        return chain_matches_at(doc, rule, idx - 1u, el->parent, check_state);
    }
    if (comb == CSS_COMB_ADJACENT) {
        const ArcrDomNode *prev = prev_element_sibling(el);
        if (!prev) return false;
        return chain_matches_at(doc, rule, idx - 1u, prev, check_state);
    }
    if (comb == CSS_COMB_SIBLING) {
        for (const ArcrDomNode *prev = prev_element_sibling(el);
             prev != NULL; prev = prev_element_sibling(prev)) {
            if (chain_matches_at(doc, rule, idx - 1u, prev, check_state)) {
                return true;
            }
        }
        return false;
    }
    for (const ArcrDomNode *anc = el->parent; anc != NULL; anc = anc->parent) {
        if (anc->kind == ARCR_NODE_ELEMENT &&
            chain_matches_at(doc, rule, idx - 1u, anc, check_state)) {
            return true;
        }
    }
    return false;
}

static bool rule_matches_element(const ArcrDocument *doc, const ArcrCssRule *rule,
                                 const ArcrDomNode *el, bool *out_has_potential_state) {
    if (rule->step_count > 1u && rule->ancestor_bloom_req != 0ULL) {
        if ((el->ancestor_bloom & rule->ancestor_bloom_req) != rule->ancestor_bloom_req) {
            return false;
        }
    }
    if (rule->uses_state_pseudo && out_has_potential_state) {
        if (chain_matches_at(doc, rule, rule->step_count - 1u, el, false)) {
            *out_has_potential_state = true;
        } else {
            return false;
        }
    }
    return chain_matches_at(doc, rule, rule->step_count - 1u, el, true);
}

/* -------------------------------------------------------------------------
 * 6. Per-Element Inherited `--*` Custom Properties & Nested `var()` Expansion
 * ------------------------------------------------------------------------- */
static const ArcrCssVar *lookup_custom_prop(const struct ArcrCssCustomPropMap *elem_vars,
                                            const ArcrStylesheet *sheet,
                                            ArcrAtom name_atom) {
    if (name_atom == ARCR_ATOM_NULL) return NULL;
    for (const struct ArcrCssCustomPropMap *m = elem_vars; m != NULL; m = m->parent) {
        for (const ArcrCssVar *v = m->vars_head; v != NULL; v = v->next) {
            if (v->name == name_atom) return v;
        }
    }
    if (sheet) {
        for (const ArcrCssVar *v = sheet->vars_head; v != NULL; v = v->next) {
            if (v->name == name_atom) return v;
        }
    }
    return NULL;
}

static size_t expand_css_vars(const ArcrDocument *doc,
                              const ArcrStylesheet *sheet,
                              const struct ArcrCssCustomPropMap *elem_vars,
                              const char *src, size_t len,
                              char *out_buf, size_t out_cap,
                              bool *used_var, int depth) {
    if (!out_buf || out_cap == 0u) return 0u;
    if (depth > 8) {
        size_t n = ARCR_MIN(len, out_cap - 1u);
        memcpy(out_buf, src, n);
        out_buf[n] = '\0';
        return n;
    }

    size_t i   = 0u;
    size_t pos = 0u;
    while (i < len && pos + 1u < out_cap) {
        if (i + 5u <= len && arcr_str_eq_nocase_n(src + i, 4u, "var(", 4u) &&
            (i == 0u || !is_css_raw_ident_char(src[i - 1u]))) {
            size_t j     = i + 4u;
            int    paren = 1;
            char   quote = '\0';
            while (j < len && paren > 0) {
                char c = src[j];
                if (quote != '\0') {
                    if (c == '\\' && j + 1u < len) { j += 2u; continue; }
                    if (c == quote) quote = '\0';
                } else {
                    if (c == '"' || c == '\'') quote = c;
                    else if (c == '(') ++paren;
                    else if (c == ')') --paren;
                }
                if (paren > 0) ++j;
            }
            if (j < len && src[j] == ')') {
                if (used_var) *used_var = true;
                const char *inner = src + i + 4u;
                size_t      ilen  = j - (i + 4u);

                size_t comma = 0u;
                int    p2    = 0;
                char   q2    = '\0';
                while (comma < ilen) {
                    char c = inner[comma];
                    if (q2 != '\0') {
                        if (c == '\\' && comma + 1u < ilen) { comma += 2u; continue; }
                        if (c == q2) q2 = '\0';
                    } else {
                        if (c == '"' || c == '\'') q2 = c;
                        else if (c == '(') ++p2;
                        else if (c == ')' && p2 > 0) --p2;
                        else if (c == ',' && p2 == 0) break;
                    }
                    ++comma;
                }

                const char *vname = inner;
                size_t      vnlen = comma;
                arcr_str_trim_view(&vname, &vnlen);

                const char *rep     = NULL;
                size_t      rep_len = 0u;
                if (vnlen > 2u && vname[0] == '-' && vname[1] == '-') {
                    ArcrAtom vatom = arcr_atom_intern(doc->atoms, vname, vnlen);
                    const ArcrCssVar *found = lookup_custom_prop(elem_vars, sheet, vatom);
                    if (found && found->val) {
                        rep     = found->val;
                        rep_len = found->val_len;
                    }
                }
                if (!rep && comma < ilen) {
                    rep     = inner + comma + 1u;
                    rep_len = ilen - comma - 1u;
                    arcr_str_trim_view(&rep, &rep_len);
                }

                if (rep && rep_len > 0u) {
                    char sub[512];
                    size_t sl = expand_css_vars(doc, sheet, elem_vars, rep, rep_len,
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

/* -------------------------------------------------------------------------
 * 7. CSS Values 4 Math Parser (`calc()`, `min()`, `max()`, `clamp()`, Units)
 * ------------------------------------------------------------------------- */
typedef struct CssMathCtx {
    const char            *s;
    size_t                 len;
    size_t                 pos;
    int32_t                base_font_px;
    int32_t                pct_ref_px;
    const ArcrCssMediaEnv *env;
    uint32_t              *deps;
} CssMathCtx;

static double eval_math_expr(CssMathCtx *mc);

static double unit_scale_to_px(const char *u, size_t ulen,
                               int32_t base_font_px, int32_t pct_ref_px,
                               const ArcrCssMediaEnv *env, uint32_t *deps) {
    if (ulen == 0u || arcr_str_eq_nocase_n(u, ulen, "px", 2u)) return 1.0;
    if (arcr_str_eq_nocase_n(u, ulen, "rem", 3u)) return 16.0;
    if (arcr_str_eq_nocase_n(u, ulen, "em", 2u)) {
        if (deps) *deps |= ARCR_STYLE_DEP_EM;
        return (double)base_font_px;
    }
    if (arcr_str_eq_nocase_n(u, ulen, "ch", 2u)) {
        if (deps) *deps |= ARCR_STYLE_DEP_EM;
        return (double)base_font_px * 0.55;
    }
    if (arcr_str_eq_nocase_n(u, ulen, "ex", 2u)) {
        if (deps) *deps |= ARCR_STYLE_DEP_EM;
        return (double)base_font_px * 0.5;
    }
    if (arcr_str_eq_nocase_n(u, ulen, "vw", 2u) ||
        arcr_str_eq_nocase_n(u, ulen, "svw", 3u) ||
        arcr_str_eq_nocase_n(u, ulen, "dvw", 3u) ||
        arcr_str_eq_nocase_n(u, ulen, "lvw", 3u)) {
        if (deps) *deps |= ARCR_STYLE_DEP_MEDIA;
        return (double)(env ? env->viewport_width : 1280) / 100.0;
    }
    if (arcr_str_eq_nocase_n(u, ulen, "vh", 2u) ||
        arcr_str_eq_nocase_n(u, ulen, "svh", 3u) ||
        arcr_str_eq_nocase_n(u, ulen, "dvh", 3u) ||
        arcr_str_eq_nocase_n(u, ulen, "lvh", 3u)) {
        if (deps) *deps |= ARCR_STYLE_DEP_MEDIA;
        return (double)(env ? env->viewport_height : 800) / 100.0;
    }
    if (arcr_str_eq_nocase_n(u, ulen, "vmin", 4u)) {
        if (deps) *deps |= ARCR_STYLE_DEP_MEDIA;
        int32_t vw = env ? env->viewport_width : 1280;
        int32_t vh = env ? env->viewport_height : 800;
        return (double)ARCR_MIN(vw, vh) / 100.0;
    }
    if (arcr_str_eq_nocase_n(u, ulen, "vmax", 4u)) {
        if (deps) *deps |= ARCR_STYLE_DEP_MEDIA;
        int32_t vw = env ? env->viewport_width : 1280;
        int32_t vh = env ? env->viewport_height : 800;
        return (double)ARCR_MAX(vw, vh) / 100.0;
    }
    if (arcr_str_eq_nocase_n(u, ulen, "pt", 2u)) return 4.0 / 3.0;
    if (arcr_str_eq_nocase_n(u, ulen, "pc", 2u)) return 16.0;
    if (arcr_str_eq_nocase_n(u, ulen, "in", 2u)) return 96.0;
    if (arcr_str_eq_nocase_n(u, ulen, "cm", 2u)) return 96.0 / 2.54;
    if (arcr_str_eq_nocase_n(u, ulen, "mm", 2u)) return 96.0 / 25.4;
    if (arcr_str_eq_nocase_n(u, ulen, "%", 1u))  return (double)pct_ref_px / 100.0;
    return 1.0;
}

static void skip_math_ws(CssMathCtx *mc) {
    while (mc->pos < mc->len && is_css_ws(mc->s[mc->pos])) mc->pos++;
}

static double eval_math_primary(CssMathCtx *mc) {
    skip_math_ws(mc);
    if (mc->pos >= mc->len) return 0.0;

    if (mc->s[mc->pos] == '+') {
        mc->pos++;
        return eval_math_primary(mc);
    }
    if (mc->s[mc->pos] == '-') {
        mc->pos++;
        return -eval_math_primary(mc);
    }
    if (mc->s[mc->pos] == '(') {
        mc->pos++;
        double v = eval_math_expr(mc);
        skip_math_ws(mc);
        if (mc->pos < mc->len && mc->s[mc->pos] == ')') mc->pos++;
        return v;
    }

    /* Function calls: calc(...), min(...), max(...), clamp(...) */
    size_t ident_start = mc->pos;
    if ((mc->s[mc->pos] >= 'a' && mc->s[mc->pos] <= 'z') ||
        (mc->s[mc->pos] >= 'A' && mc->s[mc->pos] <= 'Z')) {
        size_t p = mc->pos;
        while (p < mc->len && is_css_raw_ident_char(mc->s[p])) ++p;
        const char *fn = mc->s + ident_start;
        size_t      fl = p - ident_start;

        if (arcr_str_eq_nocase_n(fn, fl, "infinity", 8u)) {
            mc->pos = p;
            return 1.0e15;
        }
        if (arcr_str_eq_nocase_n(fn, fl, "pi", 2u)) {
            mc->pos = p;
            return 3.14159265358979323846;
        }
        if (p < mc->len && mc->s[p] == '(') {
            mc->pos = p + 1u;
            if (arcr_str_eq_nocase_n(fn, fl, "calc", 4u)) {
                double v = eval_math_expr(mc);
                skip_math_ws(mc);
                if (mc->pos < mc->len && mc->s[mc->pos] == ')') mc->pos++;
                return v;
            }
            if (arcr_str_eq_nocase_n(fn, fl, "min", 3u) ||
                arcr_str_eq_nocase_n(fn, fl, "max", 3u) ||
                arcr_str_eq_nocase_n(fn, fl, "clamp", 5u)) {
                double args[8];
                size_t acnt = 0u;
                while (mc->pos < mc->len && acnt < 8u) {
                    args[acnt++] = eval_math_expr(mc);
                    skip_math_ws(mc);
                    if (mc->pos < mc->len && mc->s[mc->pos] == ',') {
                        mc->pos++;
                    } else {
                        break;
                    }
                }
                if (mc->pos < mc->len && mc->s[mc->pos] == ')') mc->pos++;
                if (acnt == 0u) return 0.0;
                if (arcr_str_eq_nocase_n(fn, fl, "min", 3u)) {
                    double best = args[0];
                    for (size_t k = 1u; k < acnt; ++k) if (args[k] < best) best = args[k];
                    return best;
                }
                if (arcr_str_eq_nocase_n(fn, fl, "max", 3u)) {
                    double best = args[0];
                    for (size_t k = 1u; k < acnt; ++k) if (args[k] > best) best = args[k];
                    return best;
                }
                if (acnt >= 3u) {
                    /* clamp(min, val, max) = max(min, min(val, max)) */
                    double v = (args[1] < args[2]) ? args[1] : args[2];
                    return (v > args[0]) ? v : args[0];
                }
                return args[0];
            }
        }
    }

    double num  = 0.0;
    size_t nend = 0u;
    if (!parse_css_double(mc->s + mc->pos, mc->len - mc->pos, &num, &nend)) {
        return 0.0;
    }
    mc->pos += nend;

    size_t ustart = mc->pos;
    if (mc->pos < mc->len && mc->s[mc->pos] == '%') {
        mc->pos++;
    } else {
        while (mc->pos < mc->len && is_css_raw_ident_char(mc->s[mc->pos])) mc->pos++;
    }
    size_t ulen = mc->pos - ustart;
    if (ulen > 0u) {
        num *= unit_scale_to_px(mc->s + ustart, ulen, mc->base_font_px, mc->pct_ref_px,
                                mc->env, mc->deps);
    }
    return num;
}

static double eval_math_term(CssMathCtx *mc) {
    double lhs = eval_math_primary(mc);
    for (;;) {
        skip_math_ws(mc);
        if (mc->pos >= mc->len) break;
        char op = mc->s[mc->pos];
        if (op != '*' && op != '/') break;
        mc->pos++;
        double rhs = eval_math_primary(mc);
        if (op == '*') {
            lhs *= rhs;
        } else {
            lhs = (fabs(rhs) > 1.0e-9) ? (lhs / rhs) : 0.0;
        }
    }
    return lhs;
}

static double eval_math_expr(CssMathCtx *mc) {
    double lhs = eval_math_term(mc);
    for (;;) {
        skip_math_ws(mc);
        if (mc->pos >= mc->len) break;
        char op = mc->s[mc->pos];
        if (op != '+' && op != '-') break;
        mc->pos++;
        double rhs = eval_math_term(mc);
        if (op == '+') lhs += rhs;
        else           lhs -= rhs;
    }
    return lhs;
}

static int32_t parse_single_length_px(const char *s, size_t len,
                                      int32_t base_font_px, int32_t pct_ref_px,
                                      const ArcrCssMediaEnv *env, uint32_t *deps) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return 0;

    CssMathCtx mc = {s, len, 0u, base_font_px, pct_ref_px, env, deps};
    double res = eval_math_expr(&mc);
    return clamp_css_px_f(res);
}

static int32_t parse_length_or_calc_px(const char *s, size_t len,
                                       int32_t base_font_px, int32_t pct_ref_px,
                                       const ArcrCssMediaEnv *env, uint32_t *deps) {
    return parse_single_length_px(s, len, base_font_px, pct_ref_px, env, deps);
}

static bool is_pure_percent_token(const char *s, size_t len, int32_t *out_pct) {
    arcr_str_trim_view(&s, &len);
    if (len < 2u || s[len - 1u] != '%') return false;
    for (size_t i = 0u; i < len - 1u; ++i) {
        if (s[i] == '(' || s[i] == ')') return false;
    }
    double v = 0.0;
    size_t end = 0u;
    if (!parse_css_double(s, len - 1u, &v, &end)) return false;
    const char *rem = s + end;
    size_t rlen = (len - 1u) - end;
    arcr_str_trim_view(&rem, &rlen);
    if (rlen != 0u) return false;
    *out_pct = (int32_t)ARCR_CLAMP((int)(v >= 0.0 ? (v + 0.5) : (v - 0.5)), -10000, 10000);
    return true;
}

static int32_t parse_width_spec_value(const char *s, size_t len, int32_t font_px,
                                      const ArcrCssMediaEnv *env, uint32_t *deps) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u || arcr_str_eq_nocase_n(s, len, "auto", 4u) ||
        arcr_str_eq_nocase_n(s, len, "none", 4u) ||
        arcr_str_eq_nocase_n(s, len, "fit-content", 11u) ||
        arcr_str_eq_nocase_n(s, len, "max-content", 11u) ||
        arcr_str_eq_nocase_n(s, len, "min-content", 11u)) {
        return 0;
    }
    int32_t pct = 0;
    if (is_pure_percent_token(s, len, &pct)) {
        if (pct > 0 && pct <= 1000) return -pct;
        return 0;
    }
    int32_t vpw = env ? env->viewport_width : 1280;
    int32_t px  = parse_length_or_calc_px(s, len, font_px, vpw, env, deps);
    return (px > 0) ? px : 0;
}

static int32_t parse_pos_offset_encoded(const char *s, size_t len, int32_t font_px,
                                        const ArcrCssMediaEnv *env, bool *out_has) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u || arcr_str_eq_nocase_n(s, len, "auto", 4u)) {
        *out_has = false;
        return 0;
    }
    *out_has = true;
    int32_t pct = 0;
    if (is_pure_percent_token(s, len, &pct)) {
        return ARCR_POS_PCT_BIAS + pct;
    }
    int32_t vpw = env ? env->viewport_width : 1280;
    int32_t px  = parse_length_or_calc_px(s, len, font_px, vpw, env, NULL);
    return ARCR_CLAMP(px, -ARCR_CSS_PX_MAX, ARCR_CSS_PX_MAX);
}

static void parse_inset_shorthand(const char *s, size_t len, int32_t font_px,
                                  const ArcrCssMediaEnv *env, ArcrComputedStyle *st) {
    int32_t vals[4] = {0, 0, 0, 0};
    bool    has[4]  = {false, false, false, false};
    int     cnt     = 0;
    size_t  i       = 0u;

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
            vals[cnt] = parse_pos_offset_encoded(s + start, i - start, font_px, env, &has[cnt]);
            ++cnt;
        }
    }
    if (cnt == 1) {
        st->top_px = st->right_px = st->bottom_px = st->left_px = vals[0];
        st->has_top = st->has_right = st->has_bottom = st->has_left = has[0];
    } else if (cnt == 2) {
        st->top_px = st->bottom_px = vals[0];
        st->has_top = st->has_bottom = has[0];
        st->right_px = st->left_px = vals[1];
        st->has_right = st->has_left = has[1];
    } else if (cnt == 3) {
        st->top_px = vals[0]; st->has_top = has[0];
        st->right_px = st->left_px = vals[1]; st->has_right = st->has_left = has[1];
        st->bottom_px = vals[2]; st->has_bottom = has[2];
    } else if (cnt == 4) {
        st->top_px = vals[0];    st->has_top = has[0];
        st->right_px = vals[1];  st->has_right = has[1];
        st->bottom_px = vals[2]; st->has_bottom = has[2];
        st->left_px = vals[3];   st->has_left = has[3];
    }
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

static void parse_grid_columns_ex(const char *s, size_t len, int32_t font_px,
                                  const ArcrCssMediaEnv *env,
                                  int32_t *out_cols, int32_t *out_min_col_px) {
    *out_cols       = 0;
    *out_min_col_px = 0;
    arcr_str_trim_view(&s, &len);
    if (len == 0u || arcr_str_eq_nocase_n(s, len, "none", 4u)) return;

    if (len > 8u && arcr_str_eq_nocase_n(s, 7u, "repeat(", 7u) && s[len - 1u] == ')') {
        const char *inner = s + 7u;
        size_t      ilen  = len - 8u;
        size_t      comma = 0u;
        int paren = 0;
        while (comma < ilen) {
            if (inner[comma] == '(') ++paren;
            else if (inner[comma] == ')' && paren > 0) --paren;
            else if (inner[comma] == ',' && paren == 0) break;
            ++comma;
        }
        const char *first = inner;
        size_t      flen  = comma;
        arcr_str_trim_view(&first, &flen);

        int32_t n = 0;
        if (arcr_parse_i32(first, flen, &n) && n >= 1) {
            *out_cols = ARCR_CLAMP(n, 1, 16);
            return;
        }
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
                    int p2 = 0;
                    while (c2 < ml) {
                        if (mp[c2] == '(') ++p2;
                        else if (mp[c2] == ')' && p2 > 0) --p2;
                        else if (mp[c2] == ',' && p2 == 0) break;
                        ++c2;
                    }
                    int32_t parsed = parse_length_or_calc_px(mp, c2, font_px, 0, env, NULL);
                    if (parsed >= 32) min_track = parsed;
                } else {
                    int32_t parsed = parse_length_or_calc_px(rest, rlen, font_px, 0, env, NULL);
                    if (parsed >= 32) min_track = parsed;
                }
            }
            *out_min_col_px = min_track;
            int32_t vpw = env ? env->viewport_width : 1280;
            *out_cols   = ARCR_CLAMP(ARCR_MAX(1, (vpw - 32) / (min_track + 16)), 1, 16);
            return;
        }
    }

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
    *out_cols = ARCR_CLAMP(tracks, 0, 16);
}

static void parse_transform_arg_len_or_pct(const char *s, size_t len, int32_t font_px,
                                           const ArcrCssMediaEnv *env,
                                           int32_t *inout_px, int32_t *inout_pct) {
    arcr_str_trim_view(&s, &len);
    int32_t pct = 0;
    if (is_pure_percent_token(s, len, &pct)) {
        *inout_pct += pct;
    } else {
        *inout_px += parse_length_or_calc_px(s, len, font_px, 0, env, NULL);
    }
}

static void parse_transform_list(const char *s, size_t len, int32_t font_px,
                                 const ArcrCssMediaEnv *env,
                                 int32_t *out_tx_px, int32_t *out_ty_px,
                                 int32_t *out_tx_pct, int32_t *out_ty_pct,
                                 int32_t *out_sx_milli, int32_t *out_sy_milli,
                                 int32_t *out_rot_milli) {
    *out_tx_px     = 0;
    *out_ty_px     = 0;
    *out_tx_pct    = 0;
    *out_ty_pct    = 0;
    *out_sx_milli  = 1000;
    *out_sy_milli  = 1000;
    *out_rot_milli = 0;

    arcr_str_trim_view(&s, &len);
    if (len == 0u || arcr_str_eq_nocase_n(s, len, "none", 4u)) return;

    size_t i = 0u;
    while (i < len) {
        while (i < len && is_css_ws(s[i])) ++i;
        if (i >= len) break;

        size_t fn_start = i;
        while (i < len && s[i] != '(' && !is_css_ws(s[i])) ++i;
        const char *fn = s + fn_start;
        size_t      fl = i - fn_start;
        while (i < len && is_css_ws(s[i])) ++i;
        if (i >= len || s[i] != '(') break;
        ++i;

        size_t arg_start = i;
        int paren = 1;
        while (i < len && paren > 0) {
            if (s[i] == '(') ++paren;
            else if (s[i] == ')') --paren;
            if (paren > 0) ++i;
        }
        const char *args_s = s + arg_start;
        size_t      args_l = i - arg_start;
        if (i < len && s[i] == ')') ++i;

        const char *parts[4];
        size_t      plens[4];
        size_t      acnt = split_color_args(args_s, args_l, parts, plens);

        if (arcr_str_eq_nocase_n(fn, fl, "translateX", 10u) && acnt >= 1u) {
            parse_transform_arg_len_or_pct(parts[0], plens[0], font_px, env,
                                           out_tx_px, out_tx_pct);
        } else if (arcr_str_eq_nocase_n(fn, fl, "translateY", 10u) && acnt >= 1u) {
            parse_transform_arg_len_or_pct(parts[0], plens[0], font_px, env,
                                           out_ty_px, out_ty_pct);
        } else if ((arcr_str_eq_nocase_n(fn, fl, "translate", 9u) ||
                    arcr_str_eq_nocase_n(fn, fl, "translate3d", 11u)) && acnt >= 1u) {
            parse_transform_arg_len_or_pct(parts[0], plens[0], font_px, env,
                                           out_tx_px, out_tx_pct);
            if (acnt >= 2u) {
                parse_transform_arg_len_or_pct(parts[1], plens[1], font_px, env,
                                               out_ty_px, out_ty_pct);
            }
        } else if (arcr_str_eq_nocase_n(fn, fl, "scaleX", 6u) && acnt >= 1u) {
            double sx = 1.0;
            bool pct = (plens[0] > 0u && parts[0][plens[0] - 1u] == '%');
            if (parse_css_double(parts[0], pct ? (plens[0] - 1u) : plens[0], &sx, NULL)) {
                if (pct) sx /= 100.0;
                *out_sx_milli = (int32_t)(sx * 1000.0 + 0.5);
            }
        } else if (arcr_str_eq_nocase_n(fn, fl, "scaleY", 6u) && acnt >= 1u) {
            double sy = 1.0;
            bool pct = (plens[0] > 0u && parts[0][plens[0] - 1u] == '%');
            if (parse_css_double(parts[0], pct ? (plens[0] - 1u) : plens[0], &sy, NULL)) {
                if (pct) sy /= 100.0;
                *out_sy_milli = (int32_t)(sy * 1000.0 + 0.5);
            }
        } else if ((arcr_str_eq_nocase_n(fn, fl, "scale", 5u) ||
                    arcr_str_eq_nocase_n(fn, fl, "scale3d", 7u)) && acnt >= 1u) {
            double sx = 1.0;
            bool pct = (plens[0] > 0u && parts[0][plens[0] - 1u] == '%');
            if (parse_css_double(parts[0], pct ? (plens[0] - 1u) : plens[0], &sx, NULL)) {
                if (pct) sx /= 100.0;
                *out_sx_milli = (int32_t)(sx * 1000.0 + 0.5);
                *out_sy_milli = *out_sx_milli;
            }
            if (acnt >= 2u) {
                double sy = 1.0;
                bool pct2 = (plens[1] > 0u && parts[1][plens[1] - 1u] == '%');
                if (parse_css_double(parts[1], pct2 ? (plens[1] - 1u) : plens[1], &sy, NULL)) {
                    if (pct2) sy /= 100.0;
                    *out_sy_milli = (int32_t)(sy * 1000.0 + 0.5);
                }
            }
        } else if ((arcr_str_eq_nocase_n(fn, fl, "rotate", 6u) ||
                    arcr_str_eq_nocase_n(fn, fl, "rotateZ", 7u)) && acnt >= 1u) {
            double deg = parse_angle_deg(parts[0], plens[0]);
            *out_rot_milli = (int32_t)(deg * 1000.0 + (deg >= 0.0 ? 0.5 : -0.5));
        }
    }
}

static bool parse_time_ms(const char *s, size_t len, int32_t *out_ms) {
    arcr_str_trim_view(&s, &len);
    if (len < 2u) return false;
    bool is_ms = false;
    size_t nlen = 0u;
    if (len > 2u && arcr_str_eq_nocase_n(s + len - 2u, 2u, "ms", 2u)) {
        is_ms = true;
        nlen  = len - 2u;
    } else if (len > 1u && (s[len - 1u] == 's' || s[len - 1u] == 'S')) {
        is_ms = false;
        nlen  = len - 1u;
    } else {
        return false;
    }
    double v = 0.0;
    size_t end = 0u;
    if (!parse_css_double(s, nlen, &v, &end) || end != nlen) return false;
    if (!is_ms) v *= 1000.0;
    *out_ms = (int32_t)ARCR_CLAMP((int)(v >= 0.0 ? (v + 0.5) : (v - 0.5)), -600000, 600000);
    return true;
}

static ArcrOverflowMode parse_overflow_keyword(const char *s, size_t len) {
    arcr_str_trim_view(&s, &len);
    if (arcr_str_eq_nocase_n(s, len, "hidden", 6u) ||
        arcr_str_eq_nocase_n(s, len, "clip", 4u)) {
        return ARCR_OVERFLOW_HIDDEN;
    }
    if (arcr_str_eq_nocase_n(s, len, "auto", 4u) ||
        arcr_str_eq_nocase_n(s, len, "overlay", 7u)) {
        return ARCR_OVERFLOW_AUTO;
    }
    if (arcr_str_eq_nocase_n(s, len, "scroll", 6u)) {
        return ARCR_OVERFLOW_SCROLL;
    }
    return ARCR_OVERFLOW_VISIBLE;
}

static ArcrAlignItems parse_align_keyword(const char *val, size_t vlen) {
    if (arcr_str_eq_nocase_n(val, vlen, "center", 6u)) {
        return ARCR_ALIGN_CENTER;
    }
    if (arcr_str_eq_nocase_n(val, vlen, "flex-start", 10u) ||
        arcr_str_eq_nocase_n(val, vlen, "start", 5u) ||
        arcr_str_eq_nocase_n(val, vlen, "self-start", 10u) ||
        arcr_str_eq_nocase_n(val, vlen, "baseline", 8u)) {
        return ARCR_ALIGN_START;
    }
    if (arcr_str_eq_nocase_n(val, vlen, "flex-end", 8u) ||
        arcr_str_eq_nocase_n(val, vlen, "end", 3u) ||
        arcr_str_eq_nocase_n(val, vlen, "self-end", 8u)) {
        return ARCR_ALIGN_END;
    }
    return ARCR_ALIGN_STRETCH;
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
        while (i < len && is_css_ws(s[i])) ++i;
        if (i >= len || s[i] == ',') break;

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
        if (parse_color_with_current(tk, tl, st->color_argb, &parsed_c)) {
            color = parsed_c;
        } else if (lcount < 4) {
            lengths[lcount++] = parse_single_length_px(tk, tl, font_px, 0, env, &st->dependency_flags);
        }
    }

    if (((color >> 24) & 0xFFu) == 0u) {
        st->shadow_argb = 0u;
        st->shadow_blur = 0;
        return;
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

static bool has_svg_ancestor(const ArcrDomNode *el, ArcrAtom svg_atom) {
    for (const ArcrDomNode *anc = el ? el->parent : NULL; anc != NULL; anc = anc->parent) {
        if (anc->kind == ARCR_NODE_ELEMENT && anc->tag == svg_atom) return true;
    }
    return false;
}

static void apply_ua_defaults(const ArcrDocument *doc, const ArcrDomNode *el,
                              const ArcrComputedStyle *parent, ArcrAtom svg_atom,
                              ArcrComputedStyle *st) {
    memset(st, 0, sizeof(*st));
    st->display             = ARCR_DISPLAY_BLOCK;
    st->opacity             = 255u;
    st->flex_shrink         = 1;
    st->grid_col_span       = 1;
    st->scale_x_milli       = 1000;
    st->scale_y_milli       = 1000;
    st->anim_iterations     = 1;

    st->visibility_hidden   = parent ? parent->visibility_hidden : false;
    st->pointer_events_none = parent ? parent->pointer_events_none : false;
    st->color_argb          = parent ? parent->color_argb : 0xFF111827u;
    st->bg_argb             = 0x00000000u;
    st->border_argb         = 0xFFD1D5DBu;
    st->font_px             = parent ? parent->font_px : 15;
    st->line_height_pct     = parent ? parent->line_height_pct : 145;
    st->font_bold           = parent ? parent->font_bold : false;
    st->font_monospace      = parent ? parent->font_monospace : false;
    st->underline           = parent ? parent->underline : false;
    st->line_through        = parent ? parent->line_through : false;
    st->word_break_keep_all = parent ? parent->word_break_keep_all : false;
    st->white_space_pre     = parent ? parent->white_space_pre : false;
    st->white_space_nowrap  = parent ? parent->white_space_nowrap : false;
    st->text_transform      = parent ? parent->text_transform : 0u;
    st->text_align          = parent ? parent->text_align : ARCR_TEXT_ALIGN_LEFT;

    /* Internal SVG child primitives are rasterized into <svg>->surface, never laid out as HTML */
    if (has_svg_ancestor(el, svg_atom)) {
        st->display = ARCR_DISPLAY_NONE;
        return;
    }

    const char *t = arcr_atom_cstr(doc->atoms, el->tag);
    if (arcr_dom_get_attr_cstr(doc, el, "hidden") != NULL) {
        st->display = ARCR_DISPLAY_NONE;
        return;
    }

    if (arcr_str_eq_nocase(t, "head") || arcr_str_eq_nocase(t, "style") ||
        arcr_str_eq_nocase(t, "script") || arcr_str_eq_nocase(t, "meta") ||
        arcr_str_eq_nocase(t, "title") || arcr_str_eq_nocase(t, "link") ||
        arcr_str_eq_nocase(t, "base") || arcr_str_eq_nocase(t, "noscript") ||
        arcr_str_eq_nocase(t, "template") || arcr_str_eq_nocase(t, "datalist") ||
        arcr_str_eq_nocase(t, "audio") || arcr_str_eq_nocase(t, "source") ||
        arcr_str_eq_nocase(t, "track")) {
        st->display = ARCR_DISPLAY_NONE;
    } else if (arcr_str_eq_nocase(t, "body")) {
        st->bg_argb = 0xFFFFFFFFu;
        st->padding = (ArcrEdgeSizes){16, 20, 16, 20};
    } else if (arcr_str_eq_nocase(t, "h1")) {
        st->font_px         = 26;
        st->font_bold       = true;
        st->line_height_pct = 125;
        st->margin.bottom   = 10;
    } else if (arcr_str_eq_nocase(t, "h2")) {
        st->font_px         = 20;
        st->font_bold       = true;
        st->line_height_pct = 130;
        st->margin.top      = 12;
        st->margin.bottom   = 8;
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
        st->margin      = (ArcrEdgeSizes){6, 0, 10, 0};
        st->padding     = (ArcrEdgeSizes){8, 14, 8, 14};
        st->border.left = 4;
        st->border_argb = 0xFF94A3B8u;
        st->bg_argb     = 0xFFF8FAFCu;
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
               arcr_str_eq_nocase(t, "sup") || arcr_str_eq_nocase(t, "q") ||
               arcr_str_eq_nocase(t, "bdi") || arcr_str_eq_nocase(t, "bdo")) {
        st->display = ARCR_DISPLAY_INLINE;
    } else if (arcr_str_eq_nocase(t, "s") || arcr_str_eq_nocase(t, "del") ||
               arcr_str_eq_nocase(t, "strike")) {
        st->display      = ARCR_DISPLAY_INLINE;
        st->line_through = true;
    } else if (arcr_str_eq_nocase(t, "small")) {
        st->display = ARCR_DISPLAY_INLINE;
        st->font_px = ARCR_MAX(11, (st->font_px * 85) / 100);
    } else if (arcr_str_eq_nocase(t, "u") || arcr_str_eq_nocase(t, "ins")) {
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
    } else if (arcr_str_eq_nocase(t, "img") || arcr_str_eq_nocase(t, "canvas") ||
               arcr_str_eq_nocase(t, "svg")) {
        st->display = ARCR_DISPLAY_INLINE_BLOCK;
    } else if (arcr_str_eq_nocase(t, "video")) {
        st->display       = ARCR_DISPLAY_INLINE_BLOCK;
        st->bg_argb       = 0xFF0F172Au;
        st->border_radius = 8;
    } else if (arcr_str_eq_nocase(t, "button") || arcr_str_eq_nocase(t, "select")) {
        st->display       = ARCR_DISPLAY_INLINE_BLOCK;
        st->box_sizing    = ARCR_BOX_BORDER;
        st->bg_argb       = (el->state_flags & ARCR_STATE_HOVER) ? 0xFFDBEAFEu : 0xFFEFF6FFu;
        st->color_argb    = 0xFF1E3A8Au;
        st->border        = (ArcrEdgeSizes){1, 1, 1, 1};
        st->border_argb   = 0xFF93C5FDu;
        st->border_radius = 6;
        st->padding       = (ArcrEdgeSizes){6, 12, 6, 12};
        st->font_bold     = arcr_str_eq_nocase(t, "button");
        st->text_align    = ARCR_TEXT_ALIGN_CENTER;
    } else if (arcr_str_eq_nocase(t, "input")) {
        st->box_sizing = ARCR_BOX_BORDER;
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
        st->box_sizing      = ARCR_BOX_BORDER;
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
        arcr_str_eq_nocase_n(val, vlen, "hidden", 6u) ||
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
            arcr_str_eq_nocase_n(tk, tl, "double", 6u) ||
            arcr_str_eq_nocase_n(tk, tl, "groove", 6u) ||
            arcr_str_eq_nocase_n(tk, tl, "ridge", 5u) ||
            arcr_str_eq_nocase_n(tk, tl, "inset", 5u) ||
            arcr_str_eq_nocase_n(tk, tl, "outset", 6u)) {
            continue;
        }
        if (arcr_str_eq_nocase_n(tk, tl, "thin", 4u))   { bw = 1; got_w = true; continue; }
        if (arcr_str_eq_nocase_n(tk, tl, "medium", 6u)) { bw = 2; got_w = true; continue; }
        if (arcr_str_eq_nocase_n(tk, tl, "thick", 5u))  { bw = 4; got_w = true; continue; }

        uint32_t pc = 0u;
        if (parse_color_with_current(tk, tl, current_color, &pc)) {
            *inout_color = pc;
        } else if (!got_w) {
            bw    = ARCR_MAX(0, parse_single_length_px(tk, tl, font_px, 0, env, deps));
            got_w = true;
        }
    }
    *out_bw = bw;
}
static ArcrAtom get_elem_class_at(const ArcrDomNode *el, uint32_t idx) {
    if (idx < ARCR_MAX_INLINE_CLASSES) {
        return el->classes[idx];
    }
    uint32_t ext = idx - ARCR_MAX_INLINE_CLASSES;
    if (el->extra_classes && ext < el->extra_class_cap) {
        return el->extra_classes[ext];
    }
    return ARCR_ATOM_NULL;
}

static void upsert_elem_custom_prop(ArcrDocument *doc, const ArcrStylesheet *sheet,
                                    struct ArcrCssCustomPropMap *map,
                                    const struct ArcrCssCustomPropMap *lookup_scope,
                                    ArcrAtom prop_atom, const char *raw_val, size_t raw_len) {
    char expanded[512];
    bool used_v = false;
    size_t elen = expand_css_vars(doc, sheet, lookup_scope, raw_val, raw_len,
                                  expanded, sizeof(expanded), &used_v, 0);
    const char *stored = arcr_arena_strdup(&doc->style_arena, expanded, elen);
    if (!stored) return;

    for (ArcrCssVar *v = map->vars_head; v != NULL; v = v->next) {
        if (v->name == prop_atom) {
            v->val     = stored;
            v->val_len = elen;
            return;
        }
    }
    ArcrCssVar *nv = (ArcrCssVar *)arcr_arena_alloc(
        &doc->style_arena, sizeof(ArcrCssVar), sizeof(void *));
    if (!nv) return;
    nv->name       = prop_atom;
    nv->val        = stored;
    nv->val_len    = elen;
    nv->next       = map->vars_head;
    map->vars_head = nv;
}

static void parse_animation_shorthand(ArcrDocument *doc, const char *val, size_t vlen,
                                      ArcrComputedStyle *st) {
    arcr_str_trim_view(&val, &vlen);
    if (vlen == 0u || arcr_str_eq_nocase_n(val, vlen, "none", 4u)) {
        st->anim_name        = ARCR_ATOM_NULL;
        st->anim_duration_ms = 0;
        return;
    }

    bool   got_dur  = false;
    size_t i        = 0u;
    while (i < vlen) {
        while (i < vlen && (is_css_ws(val[i]) || val[i] == ',')) {
            if (val[i] == ',') return; /* First animation in list */
            ++i;
        }
        if (i >= vlen) break;
        size_t start = i;
        int paren = 0;
        while (i < vlen) {
            if (val[i] == '(') ++paren;
            else if (val[i] == ')' && paren > 0) --paren;
            else if (paren == 0 && (is_css_ws(val[i]) || val[i] == ',')) break;
            ++i;
        }
        const char *tk = val + start;
        size_t      tl = i - start;
        if (tl == 0u) continue;

        int32_t ms = 0;
        if (parse_time_ms(tk, tl, &ms)) {
            if (!got_dur) {
                st->anim_duration_ms = ARCR_MAX(0, ms);
                got_dur              = true;
            } else {
                st->anim_delay_ms = ms;
            }
            continue;
        }
        if (arcr_str_eq_nocase_n(tk, tl, "infinite", 8u)) {
            st->anim_iterations = -1;
            continue;
        }
        int32_t iter_n = 0;
        if (arcr_parse_i32(tk, tl, &iter_n) && iter_n >= 0) {
            st->anim_iterations = iter_n;
            continue;
        }
        if (arcr_str_eq_nocase_n(tk, tl, "alternate", 9u) ||
            arcr_str_eq_nocase_n(tk, tl, "alternate-reverse", 17u)) {
            st->anim_alternate = true;
            continue;
        }
        if (arcr_str_eq_nocase_n(tk, tl, "normal", 6u) ||
            arcr_str_eq_nocase_n(tk, tl, "reverse", 7u)) {
            st->anim_alternate = false;
            continue;
        }
        if (arcr_str_eq_nocase_n(tk, tl, "linear", 6u) ||
            arcr_str_eq_nocase_n(tk, tl, "ease", 4u) ||
            arcr_str_eq_nocase_n(tk, tl, "ease-in", 7u) ||
            arcr_str_eq_nocase_n(tk, tl, "ease-out", 8u) ||
            arcr_str_eq_nocase_n(tk, tl, "ease-in-out", 11u) ||
            arcr_str_eq_nocase_n(tk, tl, "forwards", 8u) ||
            arcr_str_eq_nocase_n(tk, tl, "backwards", 9u) ||
            arcr_str_eq_nocase_n(tk, tl, "both", 4u) ||
            arcr_str_eq_nocase_n(tk, tl, "running", 7u) ||
            arcr_str_eq_nocase_n(tk, tl, "paused", 6u) ||
            arcr_str_starts_with_nocase(tk, "cubic-bezier(") ||
            arcr_str_starts_with_nocase(tk, "steps(")) {
            continue;
        }
        if (tl >= 2u && ((tk[0] == '"' && tk[tl - 1u] == '"') ||
                         (tk[0] == '\'' && tk[tl - 1u] == '\''))) {
            tk += 1u;
            tl -= 2u;
        }
        if (tl > 0u) {
            st->anim_name = arcr_atom_intern(doc->atoms, tk, tl);
        }
    }
}

static void apply_declarations_pass(ArcrDocument *doc, const ArcrStylesheet *sheet,
                                    const struct ArcrCssCustomPropMap *elem_vars,
                                    const ArcrCssDecl *decls, bool want_important,
                                    int32_t parent_font_px, ArcrComputedStyle *st) {
    const ArcrCssMediaEnv *env = sheet ? &sheet->env : NULL;
    char val_buf[512];

    for (const ArcrCssDecl *d = decls; d != NULL; d = d->next) {
        if (d->is_custom) continue;
        if (d->important != want_important) continue;

        bool used_var = false;
        size_t vlen = expand_css_vars(doc, sheet, elem_vars, d->val_str, d->val_len,
                                      val_buf, sizeof(val_buf), &used_var, 0);
        if (used_var) st->dependency_flags |= ARCR_STYLE_DEP_VAR;

        const char *val = val_buf;
        arcr_str_trim_view(&val, &vlen);
        if (vlen == 0u) continue;

        const char *prop = arcr_atom_cstr(doc->atoms, d->prop);

        if (arcr_str_eq_nocase(prop, "color")) {
            uint32_t c = 0u;
            if (arcr_str_eq_nocase_n(val, vlen, "inherit", 7u)) {
                /* Keep inherited color_argb */
            } else if (parse_color_with_current(val, vlen, st->color_argb, &c)) {
                st->color_argb = c;
            }
        } else if (arcr_str_eq_nocase(prop, "background-color")) {
            uint32_t c = 0u;
            if (parse_color_with_current(val, vlen, st->color_argb, &c)) {
                st->bg_argb = c;
            }
        } else if (arcr_str_eq_nocase(prop, "background")) {
            parse_background_color_smart(val, vlen, st->color_argb, st);
        } else if (arcr_str_eq_nocase(prop, "background-image")) {
            if (arcr_str_eq_nocase_n(val, vlen, "none", 4u)) {
                st->has_gradient = false;
            } else {
                parse_linear_gradient_into_style(val, vlen, st->color_argb, st);
            }
        } else if (arcr_str_eq_nocase(prop, "border-color") ||
                   arcr_str_eq_nocase(prop, "border-top-color") ||
                   arcr_str_eq_nocase(prop, "border-right-color") ||
                   arcr_str_eq_nocase(prop, "border-bottom-color") ||
                   arcr_str_eq_nocase(prop, "border-left-color") ||
                   arcr_str_eq_nocase(prop, "border-inline-color") ||
                   arcr_str_eq_nocase(prop, "border-block-color")) {
            uint32_t c = 0u;
            if (parse_color_with_current(val, vlen, st->color_argb, &c)) {
                st->border_argb = c;
            }
        } else if (arcr_str_eq_nocase(prop, "display")) {
            if (arcr_str_eq_nocase_n(val, vlen, "none", 4u)) {
                st->display = ARCR_DISPLAY_NONE;
            } else if (arcr_str_eq_nocase_n(val, vlen, "contents", 8u)) {
                st->display = ARCR_DISPLAY_CONTENTS;
            } else if (arcr_str_eq_nocase_n(val, vlen, "inline", 6u)) {
                st->display = ARCR_DISPLAY_INLINE;
            } else if (arcr_str_eq_nocase_n(val, vlen, "inline-block", 12u)) {
                st->display = ARCR_DISPLAY_INLINE_BLOCK;
            } else if (arcr_str_eq_nocase_n(val, vlen, "flex", 4u) ||
                       arcr_str_eq_nocase_n(val, vlen, "-webkit-flex", 12u) ||
                       arcr_str_eq_nocase_n(val, vlen, "-webkit-box", 11u)) {
                st->display = ARCR_DISPLAY_FLEX;
            } else if (arcr_str_eq_nocase_n(val, vlen, "inline-flex", 11u) ||
                       arcr_str_eq_nocase_n(val, vlen, "-webkit-inline-flex", 19u)) {
                st->display = ARCR_DISPLAY_INLINE_FLEX;
            } else if (arcr_str_eq_nocase_n(val, vlen, "grid", 4u) ||
                       arcr_str_eq_nocase_n(val, vlen, "inline-grid", 11u)) {
                st->display = ARCR_DISPLAY_GRID;
            } else if (arcr_str_eq_nocase_n(val, vlen, "table", 5u)) {
                st->display = ARCR_DISPLAY_TABLE;
            } else if (arcr_str_eq_nocase_n(val, vlen, "table-row", 9u)) {
                st->display = ARCR_DISPLAY_TABLE_ROW;
            } else if (arcr_str_eq_nocase_n(val, vlen, "table-cell", 10u)) {
                st->display = ARCR_DISPLAY_TABLE_CELL;
            } else if (arcr_str_eq_nocase_n(val, vlen, "block", 5u) ||
                       arcr_str_eq_nocase_n(val, vlen, "flow-root", 9u) ||
                       arcr_str_eq_nocase_n(val, vlen, "list-item", 9u)) {
                st->display = ARCR_DISPLAY_BLOCK;
            }
        } else if (arcr_str_eq_nocase(prop, "position")) {
            if (arcr_str_eq_nocase_n(val, vlen, "relative", 8u)) {
                st->position = ARCR_POS_RELATIVE;
            } else if (arcr_str_eq_nocase_n(val, vlen, "absolute", 8u)) {
                st->position = ARCR_POS_ABSOLUTE;
            } else if (arcr_str_eq_nocase_n(val, vlen, "fixed", 5u)) {
                st->position = ARCR_POS_FIXED;
            } else if (arcr_str_eq_nocase_n(val, vlen, "sticky", 6u) ||
                       arcr_str_eq_nocase_n(val, vlen, "-webkit-sticky", 14u)) {
                st->position = ARCR_POS_STICKY;
            } else if (arcr_str_eq_nocase_n(val, vlen, "static", 6u)) {
                st->position = ARCR_POS_STATIC;
            }
        } else if (arcr_str_eq_nocase(prop, "inset")) {
            parse_inset_shorthand(val, vlen, st->font_px, env, st);
        } else if (arcr_str_eq_nocase(prop, "inset-inline")) {
            int32_t v0 = 0, v1 = 0;
            bool h0 = false, h1 = false;
            size_t sp = 0u;
            int par = 0;
            while (sp < vlen && !(par == 0 && is_css_ws(val[sp]))) {
                if (val[sp] == '(') ++par;
                else if (val[sp] == ')' && par > 0) --par;
                ++sp;
            }
            v0 = parse_pos_offset_encoded(val, sp, st->font_px, env, &h0);
            if (sp < vlen) {
                v1 = parse_pos_offset_encoded(val + sp, vlen - sp, st->font_px, env, &h1);
            } else {
                v1 = v0; h1 = h0;
            }
            st->left_px = v0;  st->has_left = h0;
            st->right_px = v1; st->has_right = h1;
        } else if (arcr_str_eq_nocase(prop, "inset-block")) {
            int32_t v0 = 0, v1 = 0;
            bool h0 = false, h1 = false;
            size_t sp = 0u;
            int par = 0;
            while (sp < vlen && !(par == 0 && is_css_ws(val[sp]))) {
                if (val[sp] == '(') ++par;
                else if (val[sp] == ')' && par > 0) --par;
                ++sp;
            }
            v0 = parse_pos_offset_encoded(val, sp, st->font_px, env, &h0);
            if (sp < vlen) {
                v1 = parse_pos_offset_encoded(val + sp, vlen - sp, st->font_px, env, &h1);
            } else {
                v1 = v0; h1 = h0;
            }
            st->top_px = v0;    st->has_top = h0;
            st->bottom_px = v1; st->has_bottom = h1;
        } else if (arcr_str_eq_nocase(prop, "top") ||
                   arcr_str_eq_nocase(prop, "inset-block-start")) {
            st->top_px = parse_pos_offset_encoded(val, vlen, st->font_px, env, &st->has_top);
        } else if (arcr_str_eq_nocase(prop, "right") ||
                   arcr_str_eq_nocase(prop, "inset-inline-end")) {
            st->right_px = parse_pos_offset_encoded(val, vlen, st->font_px, env, &st->has_right);
        } else if (arcr_str_eq_nocase(prop, "bottom") ||
                   arcr_str_eq_nocase(prop, "inset-block-end")) {
            st->bottom_px = parse_pos_offset_encoded(val, vlen, st->font_px, env, &st->has_bottom);
        } else if (arcr_str_eq_nocase(prop, "left") ||
                   arcr_str_eq_nocase(prop, "inset-inline-start")) {
            st->left_px = parse_pos_offset_encoded(val, vlen, st->font_px, env, &st->has_left);
        } else if (arcr_str_eq_nocase(prop, "z-index")) {
            if (arcr_str_eq_nocase_n(val, vlen, "auto", 4u)) {
                st->z_index = 0;
            } else {
                int32_t zi = 0;
                if (arcr_parse_i32(val, vlen, &zi)) {
                    st->z_index = ARCR_CLAMP(zi, -1000000, 1000000);
                } else {
                    st->z_index = parse_single_length_px(val, vlen, st->font_px, 0, env, NULL);
                }
            }
        } else if (arcr_str_eq_nocase(prop, "opacity")) {
            st->opacity = (uint8_t)parse_alpha_component(val, vlen);
        } else if (arcr_str_eq_nocase(prop, "visibility")) {
            st->visibility_hidden = arcr_str_eq_nocase_n(val, vlen, "hidden", 6u) ||
                                    arcr_str_eq_nocase_n(val, vlen, "collapse", 8u);
        } else if (arcr_str_eq_nocase(prop, "pointer-events")) {
            st->pointer_events_none = arcr_str_eq_nocase_n(val, vlen, "none", 4u);
        } else if (arcr_str_eq_nocase(prop, "overflow")) {
            size_t sp = 0u;
            while (sp < vlen && !is_css_ws(val[sp])) ++sp;
            ArcrOverflowMode ox = parse_overflow_keyword(val, sp);
            ArcrOverflowMode oy = (sp < vlen) ? parse_overflow_keyword(val + sp, vlen - sp) : ox;
            st->overflow_x = ox;
            st->overflow_y = oy;
        } else if (arcr_str_eq_nocase(prop, "overflow-x")) {
            st->overflow_x = parse_overflow_keyword(val, vlen);
        } else if (arcr_str_eq_nocase(prop, "overflow-y")) {
            st->overflow_y = parse_overflow_keyword(val, vlen);
        } else if (arcr_str_eq_nocase(prop, "box-sizing")) {
            if (arcr_str_eq_nocase_n(val, vlen, "border-box", 10u)) {
                st->box_sizing = ARCR_BOX_BORDER;
            } else if (arcr_str_eq_nocase_n(val, vlen, "content-box", 11u)) {
                st->box_sizing = ARCR_BOX_CONTENT;
            }
        } else if (arcr_str_eq_nocase(prop, "flex-direction")) {
            if (arcr_str_eq_nocase_n(val, vlen, "column", 6u)) {
                st->flex_direction = ARCR_FLEX_COLUMN;
            } else if (arcr_str_eq_nocase_n(val, vlen, "row-reverse", 11u)) {
                st->flex_direction = ARCR_FLEX_ROW_REVERSE;
            } else if (arcr_str_eq_nocase_n(val, vlen, "column-reverse", 14u)) {
                st->flex_direction = ARCR_FLEX_COLUMN_REVERSE;
            } else {
                st->flex_direction = ARCR_FLEX_ROW;
            }
        } else if (arcr_str_eq_nocase(prop, "flex-wrap")) {
            st->flex_wrap = arcr_str_eq_nocase_n(val, vlen, "wrap", 4u) ||
                            arcr_str_eq_nocase_n(val, vlen, "wrap-reverse", 12u);
        } else if (arcr_str_eq_nocase(prop, "flex-flow")) {
            if (str_contains_nocase(val, vlen, "column-reverse")) {
                st->flex_direction = ARCR_FLEX_COLUMN_REVERSE;
            } else if (str_contains_nocase(val, vlen, "row-reverse")) {
                st->flex_direction = ARCR_FLEX_ROW_REVERSE;
            } else if (str_contains_nocase(val, vlen, "column")) {
                st->flex_direction = ARCR_FLEX_COLUMN;
            } else if (str_contains_nocase(val, vlen, "row")) {
                st->flex_direction = ARCR_FLEX_ROW;
            }
            if (str_contains_nocase(val, vlen, "nowrap")) {
                st->flex_wrap = false;
            } else if (str_contains_nocase(val, vlen, "wrap")) {
                st->flex_wrap = true;
            }
        } else if (arcr_str_eq_nocase(prop, "justify-content")) {
            if (str_contains_nocase(val, vlen, "space-between")) {
                st->justify_content = ARCR_JUSTIFY_SPACE_BETWEEN;
            } else if (str_contains_nocase(val, vlen, "space-around")) {
                st->justify_content = ARCR_JUSTIFY_SPACE_AROUND;
            } else if (str_contains_nocase(val, vlen, "space-evenly")) {
                st->justify_content = ARCR_JUSTIFY_SPACE_EVENLY;
            } else if (str_contains_nocase(val, vlen, "center")) {
                st->justify_content = ARCR_JUSTIFY_CENTER;
            } else if (str_contains_nocase(val, vlen, "end") ||
                       arcr_str_eq_nocase_n(val, vlen, "right", 5u)) {
                st->justify_content = ARCR_JUSTIFY_END;
            } else {
                st->justify_content = ARCR_JUSTIFY_START;
            }
        } else if (arcr_str_eq_nocase(prop, "align-items")) {
            st->align_items = parse_align_keyword(val, vlen);
        } else if (arcr_str_eq_nocase(prop, "place-items")) {
            size_t sp = 0u;
            while (sp < vlen && !is_css_ws(val[sp])) ++sp;
            st->align_items = parse_align_keyword(val, sp);
            const char *jv = (sp < vlen) ? (val + sp) : val;
            size_t      jl = (sp < vlen) ? (vlen - sp) : sp;
            arcr_str_trim_view(&jv, &jl);
            if (arcr_str_eq_nocase_n(jv, jl, "center", 6u)) {
                st->justify_content = ARCR_JUSTIFY_CENTER;
            } else if (str_contains_nocase(jv, jl, "end")) {
                st->justify_content = ARCR_JUSTIFY_END;
            }
        } else if (arcr_str_eq_nocase(prop, "align-self")) {
            if (arcr_str_eq_nocase_n(val, vlen, "auto", 4u)) {
                st->has_align_self = false;
            } else {
                st->align_self     = parse_align_keyword(val, vlen);
                st->has_align_self = true;
            }
        } else if (arcr_str_eq_nocase(prop, "flex-grow")) {
            double fg = 0.0;
            if (parse_css_double(val, vlen, &fg, NULL) && fg >= 0.0) {
                st->flex_grow = (int32_t)ARCR_CLAMP((int)(fg + 0.5), 0, 1000);
            }
        } else if (arcr_str_eq_nocase(prop, "flex-shrink")) {
            double fs = 1.0;
            if (parse_css_double(val, vlen, &fs, NULL) && fs >= 0.0) {
                st->flex_shrink = (int32_t)ARCR_CLAMP((int)(fs + 0.5), 0, 1000);
            }
        } else if (arcr_str_eq_nocase(prop, "flex-basis")) {
            st->flex_basis_spec = parse_width_spec_value(val, vlen, st->font_px, env,
                                                         &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "flex")) {
            if (arcr_str_eq_nocase_n(val, vlen, "none", 4u)) {
                st->flex_grow       = 0;
                st->flex_shrink     = 0;
                st->flex_basis_spec = 0;
            } else if (arcr_str_eq_nocase_n(val, vlen, "auto", 4u)) {
                st->flex_grow       = 1;
                st->flex_shrink     = 1;
                st->flex_basis_spec = 0;
            } else if (arcr_str_eq_nocase_n(val, vlen, "initial", 7u)) {
                st->flex_grow       = 0;
                st->flex_shrink     = 1;
                st->flex_basis_spec = 0;
            } else {
                /* Parse 1..3 tokens: [grow [shrink]] [basis] */
                size_t k = 0u;
                int num_cnt = 0;
                double nums[2] = {0.0, 1.0};
                const char *basis_tk = NULL;
                size_t      basis_tl = 0u;
                while (k < vlen) {
                    while (k < vlen && is_css_ws(val[k])) ++k;
                    if (k >= vlen) break;
                    size_t st_t = k;
                    int par = 0;
                    while (k < vlen && !(par == 0 && is_css_ws(val[k]))) {
                        if (val[k] == '(') ++par;
                        else if (val[k] == ')' && par > 0) --par;
                        ++k;
                    }
                    const char *tk = val + st_t;
                    size_t      tl = k - st_t;
                    double nv = 0.0;
                    size_t nend = 0u;
                    if (num_cnt < 2 && parse_css_double(tk, tl, &nv, &nend) && nend == tl) {
                        nums[num_cnt++] = nv;
                    } else {
                        basis_tk = tk;
                        basis_tl = tl;
                    }
                }
                if (num_cnt >= 1) {
                    st->flex_grow = (int32_t)ARCR_CLAMP((int)(nums[0] + 0.5), 0, 1000);
                }
                if (num_cnt >= 2) {
                    st->flex_shrink = (int32_t)ARCR_CLAMP((int)(nums[1] + 0.5), 0, 1000);
                } else if (num_cnt == 1) {
                    st->flex_shrink = 1;
                }
                if (basis_tk) {
                    st->flex_basis_spec = parse_width_spec_value(basis_tk, basis_tl, st->font_px,
                                                                 env, &st->dependency_flags);
                } else if (num_cnt == 1) {
                    st->flex_basis_spec = 0;
                }
            }
        } else if (arcr_str_eq_nocase(prop, "gap") || arcr_str_eq_nocase(prop, "grid-gap")) {
            size_t sp = 0u;
            int par = 0;
            while (sp < vlen && !(par == 0 && is_css_ws(val[sp]))) {
                if (val[sp] == '(') ++par;
                else if (val[sp] == ')' && par > 0) --par;
                ++sp;
            }
            int32_t rg = ARCR_MAX(0, parse_length_or_calc_px(val, sp, st->font_px, 0,
                                                             env, &st->dependency_flags));
            int32_t cg = rg;
            if (sp < vlen) {
                cg = ARCR_MAX(0, parse_length_or_calc_px(val + sp, vlen - sp, st->font_px, 0,
                                                         env, &st->dependency_flags));
            }
            st->row_gap_px = rg;
            st->gap_px     = cg;
        } else if (arcr_str_eq_nocase(prop, "column-gap") ||
                   arcr_str_eq_nocase(prop, "grid-column-gap")) {
            st->gap_px = ARCR_MAX(0, parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                             env, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "row-gap") ||
                   arcr_str_eq_nocase(prop, "grid-row-gap")) {
            st->row_gap_px = ARCR_MAX(0, parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                                 env, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "grid-template-columns")) {
            parse_grid_columns_ex(val, vlen, st->font_px, env,
                                  &st->grid_columns, &st->grid_min_col_px);
        } else if (arcr_str_eq_nocase(prop, "grid-column") ||
                   arcr_str_eq_nocase(prop, "grid-column-start")) {
            if (str_contains_nocase(val, vlen, "-1")) {
                st->grid_col_span = 16;
            } else {
                for (size_t k = 0u; k + 4u <= vlen; ++k) {
                    if (arcr_str_eq_nocase_n(val + k, 4u, "span", 4u)) {
                        const char *np = val + k + 4u;
                        size_t      nl = vlen - (k + 4u);
                        arcr_str_trim_view(&np, &nl);
                        size_t dlen = 0u;
                        while (dlen < nl && np[dlen] >= '0' && np[dlen] <= '9') ++dlen;
                        int32_t spn = 1;
                        if (dlen > 0u && arcr_parse_i32(np, dlen, &spn)) {
                            st->grid_col_span = ARCR_CLAMP(spn, 1, 16);
                        }
                        break;
                    }
                }
            }
        } else if (arcr_str_eq_nocase(prop, "width") ||
                   arcr_str_eq_nocase(prop, "inline-size")) {
            st->width_spec = parse_width_spec_value(val, vlen, st->font_px, env,
                                                    &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "min-width") ||
                   arcr_str_eq_nocase(prop, "min-inline-size")) {
            int32_t mw = parse_width_spec_value(val, vlen, st->font_px, env, &st->dependency_flags);
            st->min_width_px = (mw > 0) ? mw : 0;
        } else if (arcr_str_eq_nocase(prop, "max-width") ||
                   arcr_str_eq_nocase(prop, "max-inline-size")) {
            if (arcr_str_eq_nocase_n(val, vlen, "none", 4u)) {
                st->max_width_px = 0;
            } else {
                int32_t mw = parse_width_spec_value(val, vlen, st->font_px, env,
                                                    &st->dependency_flags);
                st->max_width_px = (mw > 0) ? mw : 0;
            }
        } else if (arcr_str_eq_nocase(prop, "height") ||
                   arcr_str_eq_nocase(prop, "block-size")) {
            if (arcr_str_eq_nocase_n(val, vlen, "auto", 4u) ||
                arcr_str_eq_nocase_n(val, vlen, "none", 4u) ||
                arcr_str_eq_nocase_n(val, vlen, "fit-content", 11u) ||
                arcr_str_eq_nocase_n(val, vlen, "max-content", 11u) ||
                arcr_str_eq_nocase_n(val, vlen, "min-content", 11u)) {
                st->height_px  = 0;
                st->height_pct = 0;
            } else {
                int32_t hpct = 0;
                if (is_pure_percent_token(val, vlen, &hpct)) {
                    /* CSS2.1: store in height_pct, NOT resolved against viewport! */
                    st->height_pct = ARCR_MAX(0, hpct);
                    st->height_px  = 0;
                } else {
                    int32_t h = parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                        env, &st->dependency_flags);
                    st->height_px  = ARCR_MAX(0, h);
                    st->height_pct = 0;
                }
            }
        } else if (arcr_str_eq_nocase(prop, "min-height") ||
                   arcr_str_eq_nocase(prop, "min-block-size")) {
            int32_t hpct = 0;
            if (arcr_str_eq_nocase_n(val, vlen, "auto", 4u) ||
                is_pure_percent_token(val, vlen, &hpct)) {
                st->min_height_px = 0;
            } else {
                int32_t h = parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                    env, &st->dependency_flags);
                st->min_height_px = ARCR_MAX(0, h);
            }
        } else if (arcr_str_eq_nocase(prop, "max-height") ||
                   arcr_str_eq_nocase(prop, "max-block-size")) {
            int32_t hpct = 0;
            if (arcr_str_eq_nocase_n(val, vlen, "none", 4u) ||
                is_pure_percent_token(val, vlen, &hpct)) {
                st->max_height_px = 0;
            } else {
                int32_t h = parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                    env, &st->dependency_flags);
                st->max_height_px = ARCR_MAX(0, h);
            }
        } else if (arcr_str_eq_nocase(prop, "aspect-ratio")) {
            if (arcr_str_eq_nocase_n(val, vlen, "auto", 4u) ||
                arcr_str_eq_nocase_n(val, vlen, "none", 4u)) {
                st->aspect_ratio_w = 0;
                st->aspect_ratio_h = 0;
            } else {
                size_t slash = 0u;
                while (slash < vlen && val[slash] != '/') ++slash;
                if (slash < vlen) {
                    double aw = 0.0, ah = 0.0;
                    if (parse_css_double(val, slash, &aw, NULL) &&
                        parse_css_double(val + slash + 1u, vlen - slash - 1u, &ah, NULL) &&
                        aw > 0.001 && ah > 0.001) {
                        st->aspect_ratio_w = (int32_t)ARCR_CLAMP((int)(aw * 100.0 + 0.5), 1, 1000000);
                        st->aspect_ratio_h = (int32_t)ARCR_CLAMP((int)(ah * 100.0 + 0.5), 1, 1000000);
                    }
                } else {
                    int64_t milli = 0;
                    if (parse_fixed_1000(val, vlen, &milli, NULL) && milli > 0) {
                        st->aspect_ratio_w = (int32_t)ARCR_CLAMP((int)milli, 1, 1000000);
                        st->aspect_ratio_h = 1000;
                    }
                }
            }
        } else if (arcr_str_eq_nocase(prop, "font-size")) {
            int32_t px = 0;
            if (arcr_str_eq_nocase_n(val, vlen, "xx-small", 8u))     px = 10;
            else if (arcr_str_eq_nocase_n(val, vlen, "x-small", 7u)) px = 11;
            else if (arcr_str_eq_nocase_n(val, vlen, "small", 5u))   px = 13;
            else if (arcr_str_eq_nocase_n(val, vlen, "medium", 6u))  px = 16;
            else if (arcr_str_eq_nocase_n(val, vlen, "large", 5u))   px = 18;
            else if (arcr_str_eq_nocase_n(val, vlen, "x-large", 7u)) px = 24;
            else if (arcr_str_eq_nocase_n(val, vlen, "xx-large", 8u))px = 32;
            else if (arcr_str_eq_nocase_n(val, vlen, "smaller", 7u)) px = (parent_font_px * 85) / 100;
            else if (arcr_str_eq_nocase_n(val, vlen, "larger", 6u))  px = (parent_font_px * 120) / 100;
            else {
                px = parse_length_or_calc_px(val, vlen, parent_font_px, parent_font_px,
                                             env, &st->dependency_flags);
            }
            if (px >= 8 && px <= 160) st->font_px = px;
        } else if (arcr_str_eq_nocase(prop, "line-height")) {
            if (arcr_str_eq_nocase_n(val, vlen, "normal", 6u)) {
                st->line_height_pct = 145;
            } else {
                int32_t pct = 0;
                size_t end_m = 0u;
                int64_t milli = 0;
                if (is_pure_percent_token(val, vlen, &pct)) {
                    st->line_height_pct = ARCR_CLAMP(pct, 90, 400);
                } else if (parse_fixed_1000(val, vlen, &milli, &end_m) && end_m == vlen) {
                    /* Unitless multiplier e.g. `1.5` -> `150%` */
                    st->line_height_pct = (int32_t)ARCR_CLAMP((int)((milli + 5) / 10), 90, 400);
                } else {
                    int32_t lpx = parse_length_or_calc_px(val, vlen, st->font_px, st->font_px,
                                                          env, &st->dependency_flags);
                    if (lpx > 0 && st->font_px > 0) {
                        st->line_height_pct = ARCR_CLAMP((lpx * 100) / st->font_px, 90, 400);
                    }
                }
            }
        } else if (arcr_str_eq_nocase(prop, "font-weight")) {
            if (arcr_str_eq_nocase_n(val, vlen, "bold", 4u) ||
                arcr_str_eq_nocase_n(val, vlen, "bolder", 6u)) {
                st->font_bold = true;
            } else if (arcr_str_eq_nocase_n(val, vlen, "normal", 6u) ||
                       arcr_str_eq_nocase_n(val, vlen, "lighter", 7u)) {
                st->font_bold = false;
            } else {
                int32_t fw = 400;
                if (arcr_parse_i32(val, vlen, &fw)) {
                    st->font_bold = (fw >= 600);
                }
            }
        } else if (arcr_str_eq_nocase(prop, "font-family")) {
            st->font_monospace = str_contains_nocase(val, vlen, "monospace") ||
                                 str_contains_nocase(val, vlen, "consolas") ||
                                 str_contains_nocase(val, vlen, "courier") ||
                                 str_contains_nocase(val, vlen, "menlo") ||
                                 str_contains_nocase(val, vlen, "fira code") ||
                                 str_contains_nocase(val, vlen, "jetbrains");
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
            if (str_contains_nocase(val, vlen, "none")) {
                st->underline    = false;
                st->line_through = false;
            } else {
                if (str_contains_nocase(val, vlen, "underline"))    st->underline    = true;
                if (str_contains_nocase(val, vlen, "line-through")) st->line_through = true;
            }
        } else if (arcr_str_eq_nocase(prop, "text-transform")) {
            if (arcr_str_eq_nocase_n(val, vlen, "uppercase", 9u))      st->text_transform = 1u;
            else if (arcr_str_eq_nocase_n(val, vlen, "lowercase", 9u)) st->text_transform = 2u;
            else if (arcr_str_eq_nocase_n(val, vlen, "none", 4u))      st->text_transform = 0u;
        } else if (arcr_str_eq_nocase(prop, "text-overflow")) {
            st->text_ellipsis = str_contains_nocase(val, vlen, "ellipsis");
        } else if (arcr_str_eq_nocase(prop, "white-space")) {
            if (arcr_str_eq_nocase_n(val, vlen, "nowrap", 6u)) {
                st->white_space_nowrap = true;
                st->white_space_pre    = false;
            } else if (arcr_str_starts_with_nocase(val, "pre")) {
                st->white_space_pre    = true;
                st->white_space_nowrap = false;
            } else if (arcr_str_eq_nocase_n(val, vlen, "normal", 6u)) {
                st->white_space_pre    = false;
                st->white_space_nowrap = false;
            }
        } else if (arcr_str_eq_nocase(prop, "word-break")) {
            st->word_break_keep_all = arcr_str_eq_nocase_n(val, vlen, "keep-all", 8u);
        } else if (arcr_str_eq_nocase(prop, "border-radius")) {
            size_t sp = 0u;
            int par = 0;
            while (sp < vlen && !(par == 0 && (is_css_ws(val[sp]) || val[sp] == '/'))) {
                if (val[sp] == '(') ++par;
                else if (val[sp] == ')' && par > 0) --par;
                ++sp;
            }
            int32_t pct = 0;
            int32_t rad = 0;
            if (is_pure_percent_token(val, sp, &pct) && pct >= 50) {
                rad = 9999;
            } else {
                rad = parse_length_or_calc_px(val, sp, st->font_px, 64, env, &st->dependency_flags);
            }
            st->border_radius = ARCR_CLAMP(rad, 0, 9999);
        } else if (arcr_str_eq_nocase(prop, "padding")) {
            parse_box_shorthand(val, vlen, st->font_px, env, &st->padding,
                                &st->dependency_flags, NULL, NULL);
        } else if (arcr_str_eq_nocase(prop, "padding-inline")) {
            ArcrEdgeSizes tmp = {0, 0, 0, 0};
            parse_box_shorthand(val, vlen, st->font_px, env, &tmp,
                                &st->dependency_flags, NULL, NULL);
            st->padding.left  = tmp.top;
            st->padding.right = tmp.right;
        } else if (arcr_str_eq_nocase(prop, "padding-block")) {
            ArcrEdgeSizes tmp = {0, 0, 0, 0};
            parse_box_shorthand(val, vlen, st->font_px, env, &tmp,
                                &st->dependency_flags, NULL, NULL);
            st->padding.top    = tmp.top;
            st->padding.bottom = tmp.right;
        } else if (arcr_str_eq_nocase(prop, "padding-top") ||
                   arcr_str_eq_nocase(prop, "padding-block-start")) {
            st->padding.top = ARCR_MAX(0, parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                                  env, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "padding-right") ||
                   arcr_str_eq_nocase(prop, "padding-inline-end")) {
            st->padding.right = ARCR_MAX(0, parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                                    env, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "padding-bottom") ||
                   arcr_str_eq_nocase(prop, "padding-block-end")) {
            st->padding.bottom = ARCR_MAX(0, parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                                     env, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "padding-left") ||
                   arcr_str_eq_nocase(prop, "padding-inline-start")) {
            st->padding.left = ARCR_MAX(0, parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                                   env, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "margin")) {
            parse_box_shorthand(val, vlen, st->font_px, env, &st->margin,
                                &st->dependency_flags, &st->margin_left_auto, &st->margin_right_auto);
        } else if (arcr_str_eq_nocase(prop, "margin-inline")) {
            if (arcr_str_eq_nocase_n(val, vlen, "auto", 4u)) {
                st->margin.left = st->margin.right = 0;
                st->margin_left_auto = st->margin_right_auto = true;
            } else {
                ArcrEdgeSizes tmp = {0, 0, 0, 0};
                parse_box_shorthand(val, vlen, st->font_px, env, &tmp,
                                    &st->dependency_flags,
                                    &st->margin_left_auto, &st->margin_right_auto);
                st->margin.left  = tmp.top;
                st->margin.right = tmp.right;
            }
        } else if (arcr_str_eq_nocase(prop, "margin-block")) {
            ArcrEdgeSizes tmp = {0, 0, 0, 0};
            parse_box_shorthand(val, vlen, st->font_px, env, &tmp,
                                &st->dependency_flags, NULL, NULL);
            st->margin.top    = tmp.top;
            st->margin.bottom = tmp.right;
        } else if (arcr_str_eq_nocase(prop, "margin-top") ||
                   arcr_str_eq_nocase(prop, "margin-block-start")) {
            st->margin.top = arcr_str_eq_nocase_n(val, vlen, "auto", 4u)
                                 ? 0
                                 : parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                           env, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "margin-right") ||
                   arcr_str_eq_nocase(prop, "margin-inline-end")) {
            if (arcr_str_eq_nocase_n(val, vlen, "auto", 4u)) {
                st->margin.right      = 0;
                st->margin_right_auto = true;
            } else {
                st->margin.right      = parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                                env, &st->dependency_flags);
                st->margin_right_auto = false;
            }
        } else if (arcr_str_eq_nocase(prop, "margin-bottom") ||
                   arcr_str_eq_nocase(prop, "margin-block-end")) {
            st->margin.bottom = arcr_str_eq_nocase_n(val, vlen, "auto", 4u)
                                    ? 0
                                    : parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                              env, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "margin-left") ||
                   arcr_str_eq_nocase(prop, "margin-inline-start")) {
            if (arcr_str_eq_nocase_n(val, vlen, "auto", 4u)) {
                st->margin.left      = 0;
                st->margin_left_auto = true;
            } else {
                st->margin.left      = parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                               env, &st->dependency_flags);
                st->margin_left_auto = false;
            }
        } else if (arcr_str_eq_nocase(prop, "border")) {
            int32_t bw = 0;
            parse_border_side_shorthand(val, vlen, st->font_px, env,
                                        &bw, &st->border_argb, st->color_argb,
                                        &st->dependency_flags);
            st->border = (ArcrEdgeSizes){bw, bw, bw, bw};
        } else if (arcr_str_eq_nocase(prop, "border-top") ||
                   arcr_str_eq_nocase(prop, "border-block-start")) {
            parse_border_side_shorthand(val, vlen, st->font_px, env,
                                        &st->border.top, &st->border_argb, st->color_argb,
                                        &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "border-right") ||
                   arcr_str_eq_nocase(prop, "border-inline-end")) {
            parse_border_side_shorthand(val, vlen, st->font_px, env,
                                        &st->border.right, &st->border_argb, st->color_argb,
                                        &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "border-bottom") ||
                   arcr_str_eq_nocase(prop, "border-block-end")) {
            parse_border_side_shorthand(val, vlen, st->font_px, env,
                                        &st->border.bottom, &st->border_argb, st->color_argb,
                                        &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "border-left") ||
                   arcr_str_eq_nocase(prop, "border-inline-start")) {
            parse_border_side_shorthand(val, vlen, st->font_px, env,
                                        &st->border.left, &st->border_argb, st->color_argb,
                                        &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "border-width")) {
            parse_box_shorthand(val, vlen, st->font_px, env, &st->border,
                                &st->dependency_flags, NULL, NULL);
        } else if (arcr_str_eq_nocase(prop, "border-top-width")) {
            st->border.top = ARCR_MAX(0, parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                                 env, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "border-right-width")) {
            st->border.right = ARCR_MAX(0, parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                                   env, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "border-bottom-width")) {
            st->border.bottom = ARCR_MAX(0, parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                                    env, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "border-left-width")) {
            st->border.left = ARCR_MAX(0, parse_length_or_calc_px(val, vlen, st->font_px, 0,
                                                                  env, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "border-style")) {
            if (arcr_str_eq_nocase_n(val, vlen, "none", 4u) ||
                arcr_str_eq_nocase_n(val, vlen, "hidden", 6u)) {
                st->border = (ArcrEdgeSizes){0, 0, 0, 0};
            }
        } else if (arcr_str_eq_nocase(prop, "box-shadow")) {
            parse_box_shadow(val, vlen, st->font_px, env, st);
        } else if (arcr_str_eq_nocase(prop, "transform")) {
            parse_transform_list(val, vlen, st->font_px, env,
                                 &st->translate_x_px, &st->translate_y_px,
                                 &st->translate_x_pct, &st->translate_y_pct,
                                 &st->scale_x_milli, &st->scale_y_milli,
                                 &st->rotate_deg_milli);
        } else if (arcr_str_eq_nocase(prop, "translate")) {
            st->translate_x_px = st->translate_y_px = 0;
            st->translate_x_pct = st->translate_y_pct = 0;
            if (!arcr_str_eq_nocase_n(val, vlen, "none", 4u)) {
                size_t sp = 0u;
                int par = 0;
                while (sp < vlen && !(par == 0 && is_css_ws(val[sp]))) {
                    if (val[sp] == '(') ++par;
                    else if (val[sp] == ')' && par > 0) --par;
                    ++sp;
                }
                parse_transform_arg_len_or_pct(val, sp, st->font_px, env,
                                               &st->translate_x_px, &st->translate_x_pct);
                if (sp < vlen) {
                    parse_transform_arg_len_or_pct(val + sp, vlen - sp, st->font_px, env,
                                                   &st->translate_y_px, &st->translate_y_pct);
                }
            }
        } else if (arcr_str_eq_nocase(prop, "scale")) {
            if (arcr_str_eq_nocase_n(val, vlen, "none", 4u)) {
                st->scale_x_milli = 1000;
                st->scale_y_milli = 1000;
            } else {
                size_t sp = 0u;
                while (sp < vlen && !is_css_ws(val[sp])) ++sp;
                int64_t sxm = 1000;
                int32_t spct = 0;
                if (is_pure_percent_token(val, sp, &spct)) {
                    st->scale_x_milli = spct * 10;
                    st->scale_y_milli = spct * 10;
                } else if (parse_fixed_1000(val, sp, &sxm, NULL)) {
                    st->scale_x_milli = (int32_t)ARCR_CLAMP((int)sxm, 0, 100000);
                    st->scale_y_milli = st->scale_x_milli;
                }
                if (sp < vlen) {
                    const char *s2 = val + sp;
                    size_t      l2 = vlen - sp;
                    arcr_str_trim_view(&s2, &l2);
                    int64_t sym = 1000;
                    if (is_pure_percent_token(s2, l2, &spct)) {
                        st->scale_y_milli = spct * 10;
                    } else if (parse_fixed_1000(s2, l2, &sym, NULL)) {
                        st->scale_y_milli = (int32_t)ARCR_CLAMP((int)sym, 0, 100000);
                    }
                }
            }
        } else if (arcr_str_eq_nocase(prop, "rotate")) {
            if (arcr_str_eq_nocase_n(val, vlen, "none", 4u)) {
                st->rotate_deg_milli = 0;
            } else {
                double deg = parse_angle_deg(val, vlen);
                st->rotate_deg_milli = (int32_t)(deg * 1000.0 + (deg >= 0.0 ? 0.5 : -0.5));
            }
        } else if (arcr_str_eq_nocase(prop, "animation")) {
            parse_animation_shorthand(doc, val, vlen, st);
            if (st->anim_name != ARCR_ATOM_NULL && st->anim_duration_ms > 0) {
                st->dependency_flags |= ARCR_STYLE_DEP_ANIM;
            }
        } else if (arcr_str_eq_nocase(prop, "animation-name")) {
            if (arcr_str_eq_nocase_n(val, vlen, "none", 4u)) {
                st->anim_name = ARCR_ATOM_NULL;
            } else {
                const char *nm = val;
                size_t      nl = vlen;
                size_t      cm = 0u;
                while (cm < nl && nm[cm] != ',') ++cm;
                nl = cm;
                arcr_str_trim_view(&nm, &nl);
                if (nl >= 2u && ((nm[0] == '"' && nm[nl - 1u] == '"') ||
                                 (nm[0] == '\'' && nm[nl - 1u] == '\''))) {
                    nm += 1u; nl -= 2u;
                }
                if (nl > 0u) st->anim_name = arcr_atom_intern(doc->atoms, nm, nl);
            }
            if (st->anim_name != ARCR_ATOM_NULL && st->anim_duration_ms > 0) {
                st->dependency_flags |= ARCR_STYLE_DEP_ANIM;
            }
        } else if (arcr_str_eq_nocase(prop, "animation-duration")) {
            int32_t ms = 0;
            if (parse_time_ms(val, vlen, &ms)) st->anim_duration_ms = ARCR_MAX(0, ms);
            if (st->anim_name != ARCR_ATOM_NULL && st->anim_duration_ms > 0) {
                st->dependency_flags |= ARCR_STYLE_DEP_ANIM;
            }
        } else if (arcr_str_eq_nocase(prop, "animation-delay")) {
            int32_t ms = 0;
            if (parse_time_ms(val, vlen, &ms)) st->anim_delay_ms = ms;
        } else if (arcr_str_eq_nocase(prop, "animation-iteration-count")) {
            if (arcr_str_eq_nocase_n(val, vlen, "infinite", 8u)) {
                st->anim_iterations = -1;
            } else {
                int32_t it = 1;
                if (arcr_parse_i32(val, vlen, &it)) st->anim_iterations = it;
            }
        } else if (arcr_str_eq_nocase(prop, "animation-direction")) {
            st->anim_alternate = arcr_str_eq_nocase_n(val, vlen, "alternate", 9u) ||
                                 arcr_str_eq_nocase_n(val, vlen, "alternate-reverse", 17u);
        } else if (arcr_str_eq_nocase(prop, "transition-duration")) {
            int32_t ms = 0;
            if (parse_time_ms(val, vlen, &ms)) st->transition_duration_ms = ARCR_MAX(0, ms);
        } else if (arcr_str_eq_nocase(prop, "transition")) {
            size_t k = 0u;
            while (k < vlen && val[k] != ',') {
                while (k < vlen && is_css_ws(val[k])) ++k;
                size_t st_t = k;
                while (k < vlen && !is_css_ws(val[k]) && val[k] != ',') ++k;
                int32_t ms = 0;
                if (k > st_t && parse_time_ms(val + st_t, k - st_t, &ms)) {
                    st->transition_duration_ms = ARCR_MAX(0, ms);
                    break;
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * 8. 60 FPS `@keyframes` Live Property Interpolator
 * ------------------------------------------------------------------------- */
static int32_t lerp_i32_milli(int32_t a, int32_t b, int32_t t_milli) {
    int64_t diff = (int64_t)b - (int64_t)a;
    return (int32_t)((int64_t)a + (diff * (int64_t)t_milli) / 1000);
}

static uint32_t lerp_argb_milli(uint32_t c0, uint32_t c1, int32_t t_milli) {
    int32_t a0 = (int32_t)((c0 >> 24) & 0xFFu), a1 = (int32_t)((c1 >> 24) & 0xFFu);
    int32_t r0 = (int32_t)((c0 >> 16) & 0xFFu), r1 = (int32_t)((c1 >> 16) & 0xFFu);
    int32_t g0 = (int32_t)((c0 >> 8)  & 0xFFu), g1 = (int32_t)((c1 >> 8)  & 0xFFu);
    int32_t b0 = (int32_t)(c0         & 0xFFu), b1 = (int32_t)(c1         & 0xFFu);

    uint32_t a = (uint32_t)ARCR_CLAMP(lerp_i32_milli(a0, a1, t_milli), 0, 255);
    uint32_t r = (uint32_t)ARCR_CLAMP(lerp_i32_milli(r0, r1, t_milli), 0, 255);
    uint32_t g = (uint32_t)ARCR_CLAMP(lerp_i32_milli(g0, g1, t_milli), 0, 255);
    uint32_t b = (uint32_t)ARCR_CLAMP(lerp_i32_milli(b0, b1, t_milli), 0, 255);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static void apply_keyframe_animation(const ArcrStylesheet *sheet,
                                     ArcrComputedStyle *st,
                                     uint64_t anim_elapsed_ms,
                                     bool *out_is_active) {
    if (!sheet || st->anim_name == ARCR_ATOM_NULL || st->anim_duration_ms <= 0 ||
        st->anim_iterations == 0) {
        return;
    }

    const ArcrKeyframeRule *kf = NULL;
    for (const ArcrKeyframeRule *r = sheet->keyframes_head; r != NULL; r = r->next) {
        if (r->name == st->anim_name) {
            kf = r;
            break;
        }
    }
    if (!kf || !kf->stops_head) return;

    int64_t local_ms = (int64_t)anim_elapsed_ms - (int64_t)st->anim_delay_ms;
    if (local_ms < 0) {
        if (out_is_active) *out_is_active = true;
        return;
    }

    int64_t dur = (int64_t)st->anim_duration_ms;
    int64_t cycle = local_ms / dur;
    int64_t rem   = local_ms % dur;
    bool    running = true;

    if (st->anim_iterations > 0 && cycle >= (int64_t)st->anim_iterations) {
        cycle   = (int64_t)st->anim_iterations - 1;
        rem     = dur;
        running = false;
    }
    if (running && out_is_active) *out_is_active = true;

    int32_t prog_100k = (int32_t)((rem * 100000LL) / dur);
    prog_100k = ARCR_CLAMP(prog_100k, 0, 100000);
    if (st->anim_alternate && ((cycle & 1LL) != 0LL)) {
        prog_100k = 100000 - prog_100k;
    }

    /* Smooth cubic ease-in-out approximation in [0..100000] -> [0..100000] */
    const ArcrKeyframeStop *s0 = kf->stops_head;
    const ArcrKeyframeStop *s1 = kf->stops_head;
    for (const ArcrKeyframeStop *s = kf->stops_head; s != NULL; s = s->next) {
        if (s->pct_milli <= prog_100k) {
            s0 = s;
            s1 = s->next ? s->next : s;
        } else {
            s1 = s;
            break;
        }
    }

    int32_t span = s1->pct_milli - s0->pct_milli;
    int32_t t_milli = (span > 0) ? (((prog_100k - s0->pct_milli) * 1000) / span) : 0;
    t_milli = ARCR_CLAMP(t_milli, 0, 1000);

    if (s0->has_opacity || s1->has_opacity) {
        int32_t o0 = s0->has_opacity ? (int32_t)s0->opacity : (int32_t)st->opacity;
        int32_t o1 = s1->has_opacity ? (int32_t)s1->opacity : (int32_t)st->opacity;
        st->opacity = (uint8_t)ARCR_CLAMP(lerp_i32_milli(o0, o1, t_milli), 0, 255);
    }
    if (s0->has_bg || s1->has_bg) {
        uint32_t c0 = s0->has_bg ? s0->bg_argb : st->bg_argb;
        uint32_t c1 = s1->has_bg ? s1->bg_argb : st->bg_argb;
        st->bg_argb = lerp_argb_milli(c0, c1, t_milli);
    }
    if (s0->has_color || s1->has_color) {
        uint32_t c0 = s0->has_color ? s0->color_argb : st->color_argb;
        uint32_t c1 = s1->has_color ? s1->color_argb : st->color_argb;
        st->color_argb = lerp_argb_milli(c0, c1, t_milli);
    }
    if (s0->has_transform || s1->has_transform) {
        int32_t tx0 = s0->has_transform ? s0->tx_px : st->translate_x_px;
        int32_t tx1 = s1->has_transform ? s1->tx_px : st->translate_x_px;
        int32_t ty0 = s0->has_transform ? s0->ty_px : st->translate_y_px;
        int32_t ty1 = s1->has_transform ? s1->ty_px : st->translate_y_px;
        int32_t px0 = s0->has_transform ? s0->tx_pct : st->translate_x_pct;
        int32_t px1 = s1->has_transform ? s1->tx_pct : st->translate_x_pct;
        int32_t py0 = s0->has_transform ? s0->ty_pct : st->translate_y_pct;
        int32_t py1 = s1->has_transform ? s1->ty_pct : st->translate_y_pct;
        int32_t sx0 = s0->has_transform ? s0->sx_milli : st->scale_x_milli;
        int32_t sx1 = s1->has_transform ? s1->sx_milli : st->scale_x_milli;
        int32_t sy0 = s0->has_transform ? s0->sy_milli : st->scale_y_milli;
        int32_t sy1 = s1->has_transform ? s1->sy_milli : st->scale_y_milli;
        int32_t rt0 = s0->has_transform ? s0->rot_milli : st->rotate_deg_milli;
        int32_t rt1 = s1->has_transform ? s1->rot_milli : st->rotate_deg_milli;

        st->translate_x_px   = lerp_i32_milli(tx0, tx1, t_milli);
        st->translate_y_px   = lerp_i32_milli(ty0, ty1, t_milli);
        st->translate_x_pct  = lerp_i32_milli(px0, px1, t_milli);
        st->translate_y_pct  = lerp_i32_milli(py0, py1, t_milli);
        st->scale_x_milli    = lerp_i32_milli(sx0, sx1, t_milli);
        st->scale_y_milli    = lerp_i32_milli(sy0, sy1, t_milli);
        st->rotate_deg_milli = lerp_i32_milli(rt0, rt1, t_milli);
    }
    if ((s0->has_top || s1->has_top) &&
        !(s0->has_top && s0->top_px <= ARCR_POS_PCT_THRESHOLD) &&
        !(s1->has_top && s1->top_px <= ARCR_POS_PCT_THRESHOLD)) {
        int32_t v0 = s0->has_top ? s0->top_px : st->top_px;
        int32_t v1 = s1->has_top ? s1->top_px : st->top_px;
        st->has_top = true;
        st->top_px  = lerp_i32_milli(v0, v1, t_milli);
    }
    if ((s0->has_left || s1->has_left) &&
        !(s0->has_left && s0->left_px <= ARCR_POS_PCT_THRESHOLD) &&
        !(s1->has_left && s1->left_px <= ARCR_POS_PCT_THRESHOLD)) {
        int32_t v0 = s0->has_left ? s0->left_px : st->left_px;
        int32_t v1 = s1->has_left ? s1->left_px : st->left_px;
        st->has_left = true;
        st->left_px  = lerp_i32_milli(v0, v1, t_milli);
    }
}

/* -------------------------------------------------------------------------
 * 9. Resettable Style Arena Cascade & Iterative Tree Resolution (0 Hover Leak!)
 * ------------------------------------------------------------------------- */
typedef struct MatchedRule {
    const ArcrCssRule *rule;
} MatchedRule;

typedef struct MatchedRuleBuf {
    MatchedRule *items;
    size_t       count;
    size_t       cap;
} MatchedRuleBuf;

static void matched_buf_push(MatchedRuleBuf *mb, const ArcrCssRule *rule) {
    for (size_t i = 0u; i < mb->count; ++i) {
        if (mb->items[i].rule == rule) return;
    }
    if (mb->count >= mb->cap) {
        size_t new_cap = (mb->cap == 0u) ? 256u : (mb->cap * 2u);
        MatchedRule *ni = (MatchedRule *)realloc(mb->items, new_cap * sizeof(MatchedRule));
        if (!ni) return;
        mb->items = ni;
        mb->cap   = new_cap;
    }
    mb->items[mb->count++].rule = rule;
}

static void collect_bucket_matches(const ArcrDocument *doc, const ArcrCssRule *head,
                                   const ArcrDomNode *el, MatchedRuleBuf *mb,
                                   bool *out_has_potential_state) {
    for (const ArcrCssRule *r = head; r != NULL; r = r->next_in_bucket) {
        if (rule_matches_element(doc, r, el, out_has_potential_state)) {
            matched_buf_push(mb, r);
        }
    }
}

static int cmp_matched_rules(const void *pa, const void *pb) {
    const ArcrCssRule *a = ((const MatchedRule *)pa)->rule;
    const ArcrCssRule *b = ((const MatchedRule *)pb)->rule;
    if (a->specificity != b->specificity) {
        return (a->specificity < b->specificity) ? -1 : 1;
    }
    return (a->source_order < b->source_order) ? -1 : 1;
}

static bool can_attempt_style_sharing(const ArcrDocument *doc, const ArcrDomNode *el,
                                      ArcrAtom style_atom) {
    if (el->id != ARCR_ATOM_NULL || el->state_flags != 0u) return false;
    if (el->class_count > ARCR_MAX_INLINE_CLASSES) return false;
    if (arcr_dom_get_attr(el, style_atom) != NULL) return false;
    const char *t = arcr_atom_cstr(doc->atoms, el->tag);
    if (arcr_str_eq_nocase(t, "input") || arcr_str_eq_nocase(t, "a") ||
        arcr_str_eq_nocase(t, "button") || arcr_str_eq_nocase(t, "select") ||
        arcr_str_eq_nocase(t, "textarea") || arcr_str_eq_nocase(t, "svg") ||
        arcr_str_eq_nocase(t, "img")) {
        return false;
    }
    return true;
}

static const ArcrComputedStyle *lookup_shared_style(const ArcrStyleShareSlot *ring,
                                                    const ArcrComputedStyle *parent_style,
                                                    const struct ArcrCssCustomPropMap *parent_vars,
                                                    const ArcrDomNode *el,
                                                    bool check_ends, bool is_first, bool is_last) {
    for (size_t i = 0u; i < ARCR_STYLE_SHARE_RING; ++i) {
        const ArcrStyleShareSlot *s = &ring[i];
        if (!s->shared_style) continue;
        if (s->parent_style != parent_style || s->parent_vars != parent_vars) continue;
        if (s->tag != el->tag || s->class_count != el->class_count) continue;
        if (check_ends && (s->is_first_el != is_first || s->is_last_el != is_last)) continue;
        bool cls_eq = true;
        for (uint32_t c = 0u; c < el->class_count; ++c) {
            if (s->classes[c] != el->classes[c]) {
                cls_eq = false;
                break;
            }
        }
        if (cls_eq) return s->shared_style;
    }
    return NULL;
}

typedef struct ResolveWorkItem {
    ArcrDomNode                       *node;
    const ArcrComputedStyle           *parent_style;
    const struct ArcrCssCustomPropMap *parent_vars;
    uint64_t                           inherited_bloom;
} ResolveWorkItem;

void arcr_css_resolve_tree_animated(ArcrDocument *doc, ArcrStylesheet *sheet,
                                    uint64_t anim_elapsed_ms,
                                    size_t *out_style_share_hits,
                                    bool *out_has_active_animations) {
    if (out_style_share_hits)      *out_style_share_hits      = 0u;
    if (out_has_active_animations) *out_has_active_animations = false;
    if (!doc || !doc->root) return;

    /* Reset style_arena: O(1) bulk reclamation with ZERO hover/animation memory leak! */
    arcr_arena_reset(&doc->style_arena);

    ArcrAtom style_atom = arcr_atom_intern_lower(doc->atoms, "style", 5u);
    ArcrAtom svg_atom   = arcr_atom_intern_lower(doc->atoms, "svg", 3u);

    ArcrStyleShareSlot share_ring[ARCR_STYLE_SHARE_RING];
    memset(share_ring, 0, sizeof(share_ring));
    size_t share_cursor = 0u;
    size_t share_hits   = 0u;
    bool   any_anim     = false;

    MatchedRuleBuf mb = {NULL, 0u, 0u};

    size_t stack_cap = 256u;
    size_t stack_len = 0u;
    ResolveWorkItem *stack = (ResolveWorkItem *)malloc(stack_cap * sizeof(ResolveWorkItem));
    if (!stack) return;

    stack[stack_len++] = (ResolveWorkItem){doc->root, NULL, NULL, 0ULL};

    while (stack_len > 0u) {
        ResolveWorkItem wi = stack[--stack_len];
        ArcrDomNode *node  = wi.node;
        const ArcrComputedStyle * next_parent_style = wi.parent_style;
        const struct ArcrCssCustomPropMap *next_parent_vars = wi.parent_vars;
        uint64_t next_bloom = wi.inherited_bloom;

        if (node->kind == ARCR_NODE_ELEMENT) {
            node->ancestor_bloom = wi.inherited_bloom;

            bool check_ends = sheet && sheet->uses_first_or_last_child;
            bool allow_share = (!sheet || !sheet->uses_positional_or_sibling) &&
                               can_attempt_style_sharing(doc, node, style_atom);
            bool is_first = (allow_share && check_ends) ? is_first_element_child(node) : false;
            bool is_last  = (allow_share && check_ends) ? is_last_element_child(node)  : false;

            const ArcrComputedStyle *resolved = NULL;
            if (allow_share) {
                resolved = lookup_shared_style(share_ring, wi.parent_style, wi.parent_vars,
                                               node, check_ends, is_first, is_last);
                if (resolved) {
                    node->computed_style = resolved;
                    node->custom_props   = wi.parent_vars;
                    ++share_hits;
                }
            }

            if (!resolved) {
                ArcrComputedStyle *st = (ArcrComputedStyle *)arcr_arena_alloc(
                    &doc->style_arena, sizeof(ArcrComputedStyle), sizeof(void *));
                if (!st) continue;

                int32_t parent_fpx = wi.parent_style ? wi.parent_style->font_px : 15;
                apply_ua_defaults(doc, node, wi.parent_style, svg_atom, st);

                mb.count = 0u;
                bool has_potential_state = false;

                if (sheet) {
                    if (node->id != ARCR_ATOM_NULL) {
                        collect_bucket_matches(doc,
                            sheet->by_id[node->id & (ARCR_CSS_ID_BUCKETS - 1u)],
                            node, &mb, &has_potential_state);
                    }
                    for (uint32_t c = 0u; c < node->class_count; ++c) {
                        ArcrAtom cls_atom = get_elem_class_at(node, c);
                        if (cls_atom != ARCR_ATOM_NULL) {
                            collect_bucket_matches(doc,
                                sheet->by_class[cls_atom & (ARCR_CSS_CLASS_BUCKETS - 1u)],
                                node, &mb, &has_potential_state);
                        }
                    }
                    collect_bucket_matches(doc,
                        sheet->by_tag[node->tag & (ARCR_CSS_TAG_BUCKETS - 1u)],
                        node, &mb, &has_potential_state);
                    collect_bucket_matches(doc, sheet->universal_list, node, &mb,
                                           &has_potential_state);

                    if (mb.count > 1u) {
                        qsort(mb.items, mb.count, sizeof(MatchedRule), cmp_matched_rules);
                    }
                }
                if (has_potential_state) st->dependency_flags |= ARCR_STYLE_DEP_STATE;

                const char *inline_css = arcr_dom_get_attr(node, style_atom);
                ArcrCssDecl *inline_decls = NULL;
                if (inline_css && inline_css[0] != '\0') {
                    inline_decls = parse_declarations(&doc->style_arena, doc->atoms, NULL,
                                                      inline_css, strlen(inline_css), false);
                }

                /* Collect any element-scoped `--*` custom properties in cascade order */
                bool has_own_custom = false;
                for (size_t i = 0u; i < mb.count && !has_own_custom; ++i) {
                    for (const ArcrCssDecl *d = mb.items[i].rule->decls; d != NULL; d = d->next) {
                        if (d->is_custom) { has_own_custom = true; break; }
                    }
                }
                if (!has_own_custom) {
                    for (const ArcrCssDecl *d = inline_decls; d != NULL; d = d->next) {
                        if (d->is_custom) { has_own_custom = true; break; }
                    }
                }

                const struct ArcrCssCustomPropMap *elem_vars = wi.parent_vars;
                if (has_own_custom) {
                    struct ArcrCssCustomPropMap *own_map =
                        (struct ArcrCssCustomPropMap *)arcr_arena_calloc(
                            &doc->style_arena, 1u, sizeof(struct ArcrCssCustomPropMap),
                            sizeof(void *));
                    if (own_map) {
                        own_map->parent = wi.parent_vars;
                        for (size_t i = 0u; i < mb.count; ++i) {
                            for (const ArcrCssDecl *d = mb.items[i].rule->decls; d; d = d->next) {
                                if (d->is_custom && !d->important) {
                                    upsert_elem_custom_prop(doc, sheet, own_map, own_map,
                                                            d->prop, d->val_str, d->val_len);
                                }
                            }
                        }
                        for (const ArcrCssDecl *d = inline_decls; d; d = d->next) {
                            if (d->is_custom && !d->important) {
                                upsert_elem_custom_prop(doc, sheet, own_map, own_map,
                                                        d->prop, d->val_str, d->val_len);
                            }
                        }
                        for (size_t i = 0u; i < mb.count; ++i) {
                            for (const ArcrCssDecl *d = mb.items[i].rule->decls; d; d = d->next) {
                                if (d->is_custom && d->important) {
                                    upsert_elem_custom_prop(doc, sheet, own_map, own_map,
                                                            d->prop, d->val_str, d->val_len);
                                }
                            }
                        }
                        for (const ArcrCssDecl *d = inline_decls; d; d = d->next) {
                            if (d->is_custom && d->important) {
                                upsert_elem_custom_prop(doc, sheet, own_map, own_map,
                                                        d->prop, d->val_str, d->val_len);
                            }
                        }
                        elem_vars = own_map;
                    }
                }
                node->custom_props = elem_vars;

                /* Cascade Pass 1: Normal author rules */
                for (size_t i = 0u; i < mb.count; ++i) {
                    if (mb.items[i].rule->from_media) {
                        st->dependency_flags |= ARCR_STYLE_DEP_MEDIA;
                    }
                    apply_declarations_pass(doc, sheet, elem_vars, mb.items[i].rule->decls,
                                            false, parent_fpx, st);
                }
                /* Cascade Pass 2: Normal inline style */
                if (inline_decls) {
                    apply_declarations_pass(doc, sheet, elem_vars, inline_decls,
                                            false, parent_fpx, st);
                }
                /* Cascade Pass 3: !important author rules */
                for (size_t i = 0u; i < mb.count; ++i) {
                    apply_declarations_pass(doc, sheet, elem_vars, mb.items[i].rule->decls,
                                            true, parent_fpx, st);
                }
                /* Cascade Pass 4: !important inline style */
                if (inline_decls) {
                    apply_declarations_pass(doc, sheet, elem_vars, inline_decls,
                                            true, parent_fpx, st);
                }

                /* Live 60 FPS @keyframes interpolation */
                if (st->anim_name != ARCR_ATOM_NULL && st->anim_duration_ms > 0) {
                    bool is_act = false;
                    apply_keyframe_animation(sheet, st, anim_elapsed_ms, &is_act);
                    if (is_act) any_anim = true;
                }

                node->computed_style = st;
                resolved             = st;

                if (allow_share && !has_own_custom && st->anim_name == ARCR_ATOM_NULL) {
                    ArcrStyleShareSlot *slot =
                        &share_ring[share_cursor % ARCR_STYLE_SHARE_RING];
                    ++share_cursor;
                    slot->parent_style = wi.parent_style;
                    slot->parent_vars  = wi.parent_vars;
                    slot->tag          = node->tag;
                    slot->class_count  = node->class_count;
                    slot->is_first_el  = is_first;
                    slot->is_last_el   = is_last;
                    for (uint32_t c = 0u; c < node->class_count; ++c) {
                        slot->classes[c] = node->classes[c];
                    }
                    slot->shared_style = st;
                }
            }

            next_parent_style = resolved;
            next_parent_vars  = node->custom_props;
            next_bloom |= arcr_bloom_bits_for_atom(node->tag);
            if (node->id != ARCR_ATOM_NULL) {
                next_bloom |= arcr_bloom_bits_for_atom(node->id);
            }
            for (uint32_t c = 0u; c < node->class_count; ++c) {
                ArcrAtom cls_atom = get_elem_class_at(node, c);
                if (cls_atom != ARCR_ATOM_NULL) {
                    next_bloom |= arcr_bloom_bits_for_atom(cls_atom);
                }
            }
        }

        /* Push children in reverse sibling order so preorder traversal matches DOM order */
        for (ArcrDomNode *ch = node->last_child; ch != NULL; ch = ch->prev_sibling) {
            if (ch->kind != ARCR_NODE_ELEMENT && ch->kind != ARCR_NODE_DOCUMENT) continue;
            if (stack_len >= stack_cap) {
                size_t new_cap = stack_cap * 2u;
                ResolveWorkItem *ns = (ResolveWorkItem *)realloc(
                    stack, new_cap * sizeof(ResolveWorkItem));
                if (!ns) break;
                stack     = ns;
                stack_cap = new_cap;
            }
            stack[stack_len++] = (ResolveWorkItem){
                ch, next_parent_style, next_parent_vars, next_bloom
            };
        }
    }

    free(stack);
    free(mb.items);

    if (out_style_share_hits)      *out_style_share_hits      = share_hits;
    if (out_has_active_animations) *out_has_active_animations = any_anim;
}

void arcr_css_resolve_tree(ArcrDocument *doc, ArcrStylesheet *sheet,
                           size_t *out_style_share_hits) {
    arcr_css_resolve_tree_animated(doc, sheet, 0u, out_style_share_hits, NULL);
}
