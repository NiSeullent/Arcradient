/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * core/arcr_html_parser.c - Spec-Grade Error-Recovering HTML5 Parser, Inline <svg> & Resource Scanner
 */
#include "arcr_dom.h"
#include <stdlib.h>
#include <string.h>

#define ARCR_HTML_INIT_STACK_CAP 256u
#define ARCR_HTML_MAX_DEPTH      2048u

static bool is_ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_tag_char(char c) {
    return is_ascii_alpha(c) || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == ':' || c == '.';
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool is_void_element(const char *tag, size_t len) {
    static const char *const kVoidTags[] = {
        "area", "base", "br", "col", "embed", "hr", "img",
        "input", "keygen", "link", "meta", "param", "source", "track", "wbr"
    };
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kVoidTags); ++i) {
        if (arcr_str_eq_nocase_n(tag, len, kVoidTags[i], strlen(kVoidTags[i]))) {
            return true;
        }
    }
    return false;
}

static bool closes_open_paragraph(const char *tag, size_t len) {
    static const char *const kBlockTags[] = {
        "address", "article", "aside", "blockquote", "details", "dialog",
        "div", "dl", "fieldset", "figcaption", "figure", "footer", "form",
        "h1", "h2", "h3", "h4", "h5", "h6", "header", "hgroup", "hr",
        "main", "menu", "nav", "ol", "p", "pre", "search", "section",
        "table", "ul"
    };
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kBlockTags); ++i) {
        if (arcr_str_eq_nocase_n(tag, len, kBlockTags[i], strlen(kBlockTags[i]))) {
            return true;
        }
    }
    return false;
}

/* WHATWG HTML5 Windows-1252 numeric character reference remapping (0x80..0x9F) */
static uint32_t remap_numeric_codepoint(uint32_t cp) {
    static const uint16_t kWin1252[32] = {
        0x20ACu, 0x0081u, 0x201Au, 0x0192u, 0x201Eu, 0x2026u, 0x2020u, 0x2021u,
        0x02C6u, 0x2030u, 0x0160u, 0x2039u, 0x0152u, 0x008Du, 0x017Du, 0x008Fu,
        0x0090u, 0x2018u, 0x2019u, 0x201Cu, 0x201Du, 0x2022u, 0x2013u, 0x2014u,
        0x02DCu, 0x2122u, 0x0161u, 0x203Au, 0x0153u, 0x009Du, 0x017Eu, 0x0178u
    };
    if (cp == 0u || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        return ARCR_UNICODE_REPLACEMENT;
    }
    if (cp >= 0x80u && cp <= 0x9Fu) {
        return (uint32_t)kWin1252[cp - 0x80u];
    }
    return cp;
}

static bool lookup_named_entity(const char *ent, size_t elen, uint32_t *out_cp) {
    static const struct {
        const char *name;
        uint32_t    cp;
        bool        nocase;
    } kEntities[] = {
        /* Core XML / HTML5 entities (case-insensitive fallback allowed) */
        {"amp",      '&',     true},  {"lt",       '<',     true},
        {"gt",       '>',     true},  {"quot",     '"',     true},
        {"apos",     '\'',    true},  {"nbsp",     0x00A0u, true},
        {"copy",     0x00A9u, true},  {"reg",      0x00AEu, true},
        {"trade",    0x2122u, true},  {"mdash",    0x2014u, true},
        {"ndash",    0x2013u, true},  {"bull",     0x2022u, true},
        {"middot",   0x00B7u, true},  {"hellip",   0x2026u, true},
        {"rarr",     0x2192u, false}, {"larr",     0x2190u, false},
        {"uarr",     0x2191u, false}, {"darr",     0x2193u, false},
        {"harr",     0x2194u, false}, {"crarr",    0x21B5u, false},
        {"rArr",     0x21D2u, false}, {"lArr",     0x21D0u, false},
        {"hArr",     0x21D4u, false}, {"times",    0x00D7u, true},
        {"divide",   0x00F7u, true},  {"plusmn",   0x00B1u, true},
        {"minus",    0x2212u, true},  {"lowast",   0x2217u, false},
        {"radic",    0x221Au, false}, {"infin",    0x221Eu, false},
        {"asymp",    0x2248u, false}, {"ne",       0x2260u, false},
        {"equiv",    0x2261u, false}, {"le",       0x2264u, false},
        {"ge",       0x2265u, false}, {"perp",     0x22A5u, false},
        {"sdot",     0x22C5u, false}, {"starf",    0x2605u, false},
        {"star",     0x2606u, false}, {"spades",   0x2660u, false},
        {"clubs",    0x2663u, false}, {"hearts",   0x2665u, false},
        {"diams",    0x2666u, false}, {"check",    0x2713u, false},
        {"cross",    0x2717u, false}, {"laquo",    0x00ABu, true},
        {"raquo",    0x00BBu, true},  {"lsaquo",   0x2039u, false},
        {"rsaquo",   0x203Au, false}, {"lsquo",    0x2018u, false},
        {"rsquo",    0x2019u, false}, {"sbquo",    0x201Au, false},
        {"ldquo",    0x201Cu, false}, {"rdquo",    0x201Du, false},
        {"bdquo",    0x201Eu, false}, {"dagger",   0x2020u, false},
        {"Dagger",   0x2021u, false}, {"permil",   0x2030u, false},
        {"prime",    0x2032u, false}, {"Prime",    0x2033u, false},
        {"euro",     0x20ACu, true},  {"yen",      0x00A5u, true},
        {"pound",    0x00A3u, true},  {"cent",     0x00A2u, true},
        {"curren",   0x00A4u, true},  {"won",      0x20A9u, true},
        {"deg",      0x00B0u, true},  {"sect",     0x00A7u, true},
        {"para",     0x00B6u, true},  {"micro",    0x00B5u, true},
        {"not",      0x00ACu, true},  {"shy",      0x00ADu, true},
        {"macr",     0x00AFu, true},  {"sup1",     0x00B9u, true},
        {"sup2",     0x00B2u, true},  {"sup3",     0x00B3u, true},
        {"frac14",   0x00BCu, true},  {"frac12",   0x00BDu, true},
        {"frac34",   0x00BEu, true},  {"iexcl",    0x00A1u, true},
        {"iquest",   0x00BFu, true},  {"brvbar",   0x00A6u, true},
        {"ordf",     0x00AAu, true},  {"ordm",     0x00BAu, true},
        {"ensp",     0x2002u, false}, {"emsp",     0x2003u, false},
        {"thinsp",   0x2009u, false}, {"zwnj",     0x200Cu, false},
        {"zwj",      0x200Du, false}, {"lrm",      0x200Eu, false},
        {"rlm",      0x200Fu, false}, {"num",      '#',     false},
        {"percnt",   '%',     false}, {"sol",      '/',     false},
        {"colon",    ':',     false}, {"semi",     ';',     false},
        {"equals",   '=',     false}, {"plus",     '+',     false},
        {"ast",      '*',     false}, {"commat",   '@',     false},
        {"lpar",     '(',     false}, {"rpar",     ')',     false},
        {"lsqb",     '[',     false}, {"rsqb",     ']',     false},
        {"lcub",     '{',     false}, {"rcub",     '}',     false},
        {"verbar",   '|',     false},
        /* Accented Latin-1 characters (strictly case-sensitive) */
        {"Agrave",   0x00C0u, false}, {"Aacute",   0x00C1u, false},
        {"Acirc",    0x00C2u, false}, {"Atilde",   0x00C3u, false},
        {"Auml",     0x00C4u, false}, {"Aring",    0x00C5u, false},
        {"AElig",    0x00C6u, false}, {"Ccedil",   0x00C7u, false},
        {"Egrave",   0x00C8u, false}, {"Eacute",   0x00C9u, false},
        {"Ecirc",    0x00CAu, false}, {"Euml",     0x00CBu, false},
        {"Igrave",   0x00CCu, false}, {"Iacute",   0x00CDu, false},
        {"Icirc",    0x00CEu, false}, {"Iuml",     0x00CFu, false},
        {"Ntilde",   0x00D1u, false}, {"Ograve",   0x00D2u, false},
        {"Oacute",   0x00D3u, false}, {"Ocirc",    0x00D4u, false},
        {"Otilde",   0x00D5u, false}, {"Ouml",     0x00D6u, false},
        {"Oslash",   0x00D8u, false}, {"Ugrave",   0x00D9u, false},
        {"Uacute",   0x00DAu, false}, {"Ucirc",    0x00DBu, false},
        {"Uuml",     0x00DCu, false}, {"Yacute",   0x00DDu, false},
        {"szlig",    0x00DFu, false}, {"agrave",   0x00E0u, false},
        {"aacute",   0x00E1u, false}, {"acirc",    0x00E2u, false},
        {"atilde",   0x00E3u, false}, {"auml",     0x00E4u, false},
        {"aring",    0x00E5u, false}, {"aelig",    0x00E6u, false},
        {"ccedil",   0x00E7u, false}, {"egrave",   0x00E8u, false},
        {"eacute",   0x00E9u, false}, {"ecirc",    0x00EAu, false},
        {"euml",     0x00EBu, false}, {"igrave",   0x00ECu, false},
        {"iacute",   0x00EDu, false}, {"icirc",    0x00EEu, false},
        {"iuml",     0x00EFu, false}, {"ntilde",   0x00F1u, false},
        {"ograve",   0x00F2u, false}, {"oacute",   0x00F3u, false},
        {"ocirc",    0x00F4u, false}, {"otilde",   0x00F5u, false},
        {"ouml",     0x00F6u, false}, {"oslash",   0x00F8u, false},
        {"ugrave",   0x00F9u, false}, {"uacute",   0x00FAu, false},
        {"ucirc",    0x00FBu, false}, {"uuml",     0x00FCu, false},
        {"yacute",   0x00FDu, false}, {"yuml",     0x00FFu, false},
        {"OElig",    0x0152u, false}, {"oelig",    0x0153u, false},
        {"Scaron",   0x0160u, false}, {"scaron",   0x0161u, false},
        {"Alpha",    0x0391u, false}, {"Beta",     0x0392u, false},
        {"Gamma",    0x0393u, false}, {"Delta",    0x0394u, false},
        {"Pi",       0x03A0u, false}, {"Sigma",    0x03A3u, false},
        {"Omega",    0x03A9u, false}, {"alpha",    0x03B1u, false},
        {"beta",     0x03B2u, false}, {"gamma",    0x03B3u, false},
        {"delta",    0x03B4u, false}, {"epsilon",  0x03B5u, false},
        {"lambda",   0x03BBu, false}, {"mu",       0x03BCu, false},
        {"pi",       0x03C0u, false}, {"sigma",    0x03C3u, false},
        {"omega",    0x03C9u, false}, {"sum",      0x2211u, false},
        {"prod",     0x220Fu, false}, {"int",      0x222Bu, false},
        {"part",     0x2202u, false}, {"nabla",    0x2207u, false},
        {"isin",     0x2208u, false}, {"notin",    0x2209u, false},
        {"cap",      0x2229u, false}, {"cup",      0x222Au, false},
        {"there4",   0x2234u, false}, {"empty",    0x2205u, false},
        {"circ",     0x02C6u, false}, {"tilde",    0x02DCu, false}
    };

    /* Pass 1: exact case-sensitive match */
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kEntities); ++i) {
        size_t nlen = strlen(kEntities[i].name);
        if (elen == nlen && memcmp(ent, kEntities[i].name, nlen) == 0) {
            *out_cp = kEntities[i].cp;
            return true;
        }
    }
    /* Pass 2: case-insensitive match for non-cased HTML entities (e.g. &AMP;, &NBSP;) */
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kEntities); ++i) {
        if (!kEntities[i].nocase) continue;
        size_t nlen = strlen(kEntities[i].name);
        if (elen == nlen && arcr_str_eq_nocase_n(ent, elen, kEntities[i].name, nlen)) {
            *out_cp = kEntities[i].cp;
            return true;
        }
    }
    return false;
}

/* Decodes HTML5 named & numeric entities (&#1234;, &#x1F600;) and optionally collapses whitespace */
static size_t decode_and_collapse_text(const char *src, size_t src_len,
                                       char *out, bool collapse_ws) {
    size_t i       = 0u;
    size_t pos     = 0u;
    bool   prev_ws = false;

    while (i < src_len) {
        if (collapse_ws && is_space(src[i])) {
            if (!prev_ws) {
                if (out) out[pos] = ' ';
                ++pos;
                prev_ws = true;
            }
            ++i;
            continue;
        }
        prev_ws = false;

        if (src[i] == '&') {
            size_t semi = i + 1u;
            while (semi < src_len && semi - i <= 40u && src[semi] != ';' &&
                   !is_space(src[semi]) && src[semi] != '<' && src[semi] != '&') {
                ++semi;
            }
            if (semi < src_len && src[semi] == ';') {
                const char *ent     = src + i + 1u;
                size_t      elen    = semi - (i + 1u);
                uint32_t    cp      = 0u;
                bool        matched = false;

                if (elen >= 2u && ent[0] == '#') {
                    uint64_t val = 0u;
                    matched      = true;
                    if (ent[1] == 'x' || ent[1] == 'X') {
                        if (elen < 3u) matched = false;
                        for (size_t k = 2u; k < elen; ++k) {
                            char c = ent[k];
                            uint32_t nib = 0u;
                            if (c >= '0' && c <= '9') nib = (uint32_t)(c - '0');
                            else if (c >= 'a' && c <= 'f') nib = (uint32_t)(c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') nib = (uint32_t)(c - 'A' + 10);
                            else { matched = false; break; }
                            if (val <= 0x110000ULL) val = (val << 4) | nib;
                        }
                    } else {
                        for (size_t k = 1u; k < elen; ++k) {
                            if (ent[k] >= '0' && ent[k] <= '9') {
                                if (val <= 0x110000ULL) {
                                    val = val * 10u + (uint64_t)(ent[k] - '0');
                                }
                            } else {
                                matched = false;
                                break;
                            }
                        }
                    }
                    if (matched) {
                        cp = remap_numeric_codepoint((val > 0x110000ULL) ? 0x110000u : (uint32_t)val);
                    }
                } else if (elen > 0u) {
                    matched = lookup_named_entity(ent, elen, &cp);
                }

                if (matched) {
                    char utf8_ch[4];
                    size_t n = arcr_utf8_encode_one(cp, utf8_ch);
                    if (out) memcpy(out + pos, utf8_ch, n);
                    pos += n;
                    i = semi + 1u;
                    continue;
                }
            }
        }

        if (out) out[pos] = src[i];
        ++pos;
        ++i;
    }
    if (out) out[pos] = '\0';
    return pos;
}

static bool is_end_tag_boundary(char c) {
    return c == '>' || c == '/' || is_space(c);
}

static size_t find_raw_end_tag(const char *s, size_t len, size_t start,
                               const char *tag, size_t tag_len) {
    for (size_t i = start; i + 2u + tag_len <= len; ++i) {
        if (s[i] == '<' && s[i + 1u] == '/' &&
            arcr_str_eq_nocase_n(s + i + 2u, tag_len, tag, tag_len)) {
            char next = (i + 2u + tag_len < len) ? s[i + 2u + tag_len] : '>';
            if (is_end_tag_boundary(next)) {
                return i;
            }
        }
    }
    return len;
}

/*
 * WHATWG HTML5 §13.2.5.4 / §13.2.5.19-25 Script Data Scanner:
 * Handles normal script data, `<!--` script-data-escaped, and nested `<script>`
 * script-data-double-escaped states, requiring `</script` to be followed by
 * whitespace, `>`, `/`, or EOF.
 */
static size_t find_script_end_tag(const char *s, size_t len, size_t start) {
    enum {
        SCRIPT_NORMAL         = 0,
        SCRIPT_ESCAPED        = 1,
        SCRIPT_DOUBLE_ESCAPED = 2
    } state = SCRIPT_NORMAL;

    size_t i = start;
    while (i < len) {
        if (state == SCRIPT_NORMAL) {
            if (i + 4u <= len && memcmp(s + i, "<!--", 4u) == 0) {
                state = SCRIPT_ESCAPED;
                i += 4u;
                continue;
            }
            if (i + 8u <= len && s[i] == '<' && s[i + 1u] == '/' &&
                arcr_str_eq_nocase_n(s + i + 2u, 6u, "script", 6u)) {
                char next = (i + 8u < len) ? s[i + 8u] : '>';
                if (is_end_tag_boundary(next)) {
                    return i;
                }
            }
            ++i;
        } else if (state == SCRIPT_ESCAPED) {
            if (i + 3u <= len && memcmp(s + i, "-->", 3u) == 0) {
                state = SCRIPT_NORMAL;
                i += 3u;
                continue;
            }
            if (i + 8u <= len && s[i] == '<' && s[i + 1u] == '/' &&
                arcr_str_eq_nocase_n(s + i + 2u, 6u, "script", 6u)) {
                char next = (i + 8u < len) ? s[i + 8u] : '>';
                if (is_end_tag_boundary(next)) {
                    return i;
                }
            }
            if (i + 7u <= len && s[i] == '<' &&
                arcr_str_eq_nocase_n(s + i + 1u, 6u, "script", 6u)) {
                char next = (i + 7u < len) ? s[i + 7u] : ' ';
                if (is_end_tag_boundary(next)) {
                    state = SCRIPT_DOUBLE_ESCAPED;
                    i += 7u;
                    continue;
                }
            }
            ++i;
        } else {
            /* SCRIPT_DOUBLE_ESCAPED */
            if (i + 3u <= len && memcmp(s + i, "-->", 3u) == 0) {
                state = SCRIPT_NORMAL;
                i += 3u;
                continue;
            }
            if (i + 8u <= len && s[i] == '<' && s[i + 1u] == '/' &&
                arcr_str_eq_nocase_n(s + i + 2u, 6u, "script", 6u)) {
                char next = (i + 8u < len) ? s[i + 8u] : '>';
                if (is_end_tag_boundary(next)) {
                    state = SCRIPT_ESCAPED;
                    i += 8u;
                    continue;
                }
            }
            ++i;
        }
    }
    return len;
}

static void append_external_ref(ArcrDocument *doc, ArcrExternalResourceRef **head_ptr,
                                const char *url_cstr, ArcrDomNode *owner) {
    if (!doc || !head_ptr || !url_cstr || url_cstr[0] == '\0') return;
    ArcrExternalResourceRef *ref = (ArcrExternalResourceRef *)arcr_arena_alloc(
        &doc->arena, sizeof(ArcrExternalResourceRef), sizeof(void *));
    if (!ref) return;
    ref->href_or_src = url_cstr;
    ref->owner_node  = owner;
    ref->owner_uid   = owner ? owner->node_uid : 0u;
    ref->next        = NULL;

    if (!*head_ptr) {
        *head_ptr = ref;
    } else {
        ArcrExternalResourceRef *tail = *head_ptr;
        while (tail->next) tail = tail->next;
        tail->next = ref;
    }
}

static bool rel_contains_stylesheet(const char *rel) {
    if (!rel) return false;
    size_t len = strlen(rel);
    size_t i   = 0u;
    while (i < len) {
        while (i < len && is_space(rel[i])) ++i;
        size_t s = i;
        while (i < len && !is_space(rel[i])) ++i;
        if (i > s && arcr_str_eq_nocase_n(rel + s, i - s, "stylesheet", 10u)) {
            return true;
        }
    }
    return false;
}

static bool is_placeholder_img_src(const char *src) {
    if (!src || src[0] == '\0') return true;
    while (is_space(*src)) ++src;
    if (src[0] == '\0' || src[0] == '#') return true;
    if (arcr_str_starts_with_nocase(src, "about:blank")) return true;
    if (arcr_str_starts_with_nocase(src, "javascript:")) return true;
    /* Lazy-load 1x1 spacer GIFs used on modern portals before data-src/srcset swap */
    if (arcr_str_starts_with_nocase(src, "data:image/gif")) return true;
    if (arcr_str_starts_with_nocase(src, "data:,")) return true;
    return false;
}

/* Extracts the first valid candidate URL from an HTML5 `srcset` attribute */
static const char *extract_srcset_url(ArcrDocument *doc, const char *srcset) {
    if (!doc || !srcset) return NULL;
    size_t len = strlen(srcset);
    size_t i   = 0u;

    while (i < len) {
        while (i < len && (is_space(srcset[i]) || srcset[i] == ',')) ++i;
        if (i >= len) break;

        size_t ustart = i;
        if (arcr_str_starts_with_nocase(srcset + ustart, "data:")) {
            /* Data URI may contain a comma before base64 payload; read until whitespace */
            while (i < len && !is_space(srcset[i])) ++i;
        } else {
            while (i < len && !is_space(srcset[i])) ++i;
        }
        size_t uend = i;
        while (uend > ustart && srcset[uend - 1u] == ',') --uend;

        /* Skip optional width/pixel-density descriptor up to next comma */
        while (i < len && srcset[i] != ',') ++i;
        if (i < len && srcset[i] == ',') ++i;

        if (uend > ustart) {
            char *cand = arcr_arena_strdup(&doc->arena, srcset + ustart, uend - ustart);
            if (cand && !is_placeholder_img_src(cand)) {
                return cand;
            }
        }
    }
    return NULL;
}

static const char *pick_lazy_img_fallback(ArcrDocument *doc, const ArcrDomNode *elem) {
    static const char *const kDataAttrs[] = {
        "data-src", "data-lazy-src", "data-original", "data-url"
    };
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kDataAttrs); ++i) {
        const char *v = arcr_dom_get_attr_cstr(doc, elem, kDataAttrs[i]);
        if (v && !is_placeholder_img_src(v)) {
            const char *tv = v;
            size_t      tl = strlen(v);
            arcr_str_trim_view(&tv, &tl);
            if (tl > 0u) return arcr_arena_strdup(&doc->arena, tv, tl);
        }
    }
    const char *ss = arcr_dom_get_attr_cstr(doc, elem, "srcset");
    if (!ss || ss[0] == '\0') {
        ss = arcr_dom_get_attr_cstr(doc, elem, "data-srcset");
    }
    if (ss && ss[0] != '\0') {
        return extract_srcset_url(doc, ss);
    }
    return NULL;
}

static bool is_supported_picture_mime(const char *mime) {
    if (!mime || mime[0] == '\0') return true;
    size_t len = 0u;
    while (mime[len] != '\0' && mime[len] != ';') ++len;
    const char *m = mime;
    arcr_str_trim_view(&m, &len);
    if (len == 0u) return true;
    static const char *const kSupported[] = {
        "image/png", "image/jpeg", "image/jpg", "image/webp",
        "image/gif", "image/bmp", "image/svg+xml",
        "image/x-icon", "image/vnd.microsoft.icon"
    };
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kSupported); ++i) {
        if (arcr_str_eq_nocase_n(m, len, kSupported[i], strlen(kSupported[i]))) {
            return true;
        }
    }
    return false;
}

static const char *pick_picture_source_url(ArcrDocument *doc, const ArcrDomNode *img_elem) {
    if (!doc || !img_elem || !img_elem->parent || img_elem->parent->kind != ARCR_NODE_ELEMENT) {
        return NULL;
    }
    const char *ptag = arcr_atom_cstr(doc->atoms, img_elem->parent->tag);
    if (!arcr_str_eq_nocase(ptag, "picture")) return NULL;

    for (const ArcrDomNode *sib = img_elem->parent->first_child;
         sib != NULL && sib != img_elem;
         sib = sib->next_sibling) {
        if (sib->kind != ARCR_NODE_ELEMENT) continue;
        const char *stag = arcr_atom_cstr(doc->atoms, sib->tag);
        if (!arcr_str_eq_nocase(stag, "source")) continue;

        const char *type_attr = arcr_dom_get_attr_cstr(doc, sib, "type");
        if (!is_supported_picture_mime(type_attr)) continue;

        const char *ss = arcr_dom_get_attr_cstr(doc, sib, "srcset");
        if (ss && ss[0] != '\0') {
            const char *cand = extract_srcset_url(doc, ss);
            if (cand) return cand;
        }
        const char *s_src = arcr_dom_get_attr_cstr(doc, sib, "src");
        if (s_src && !is_placeholder_img_src(s_src)) {
            const char *tv = s_src;
            size_t      tl = strlen(s_src);
            arcr_str_trim_view(&tv, &tl);
            if (tl > 0u) return arcr_arena_strdup(&doc->arena, tv, tl);
        }
    }
    return NULL;
}

static void attach_decoded_image_to_node(ArcrDocument *doc, ArcrDomNode *elem, ArcrImage *img) {
    if (!doc || !elem || !img || !img->pixels || img->width <= 0 || img->height <= 0) {
        if (img) arcr_image_free(img);
        return;
    }
    ArcrDomSurface *surf = (ArcrDomSurface *)arcr_arena_calloc(
        &doc->arena, 1u, sizeof(ArcrDomSurface), sizeof(void *));
    if (surf) {
        surf->width       = img->width;
        surf->height      = img->height;
        surf->argb_pixels = img->pixels;
        surf->heap_owned  = true;
        elem->surface     = surf;
    } else {
        arcr_image_free(img);
    }
}

static void finalize_opened_element(ArcrDocument *doc, ArcrDomNode *elem,
                                    const char *tag_ptr, size_t tag_len,
                                    size_t svg_depth, bool in_template) {
    if (!doc || !elem || svg_depth > 0u || in_template) return;

    if (arcr_str_eq_nocase_n(tag_ptr, tag_len, "base", 4u)) {
        if (!doc->base_href) {
            const char *href = arcr_dom_get_attr_cstr(doc, elem, "href");
            if (href && href[0] != '\0') {
                const char *tv = href;
                size_t      tl = strlen(href);
                arcr_str_trim_view(&tv, &tl);
                if (tl > 0u) {
                    doc->base_href = arcr_arena_strdup(&doc->arena, tv, tl);
                }
            }
        }
    } else if (arcr_str_eq_nocase_n(tag_ptr, tag_len, "link", 4u)) {
        const char *rel  = arcr_dom_get_attr_cstr(doc, elem, "rel");
        const char *href = arcr_dom_get_attr_cstr(doc, elem, "href");
        if (rel_contains_stylesheet(rel) && href && href[0] != '\0') {
            append_external_ref(doc, &doc->ext_styles_head, href, elem);
        }
    } else if (arcr_str_eq_nocase_n(tag_ptr, tag_len, "script", 6u)) {
        doc->has_scripts = true;
        const char *src  = arcr_dom_get_attr_cstr(doc, elem, "src");
        if (src && src[0] != '\0') {
            append_external_ref(doc, &doc->ext_scripts_head, src, elem);
        }
    } else if (arcr_str_eq_nocase_n(tag_ptr, tag_len, "img", 3u)) {
        doc->has_images          = true;
        const char *orig_src     = arcr_dom_get_attr_cstr(doc, elem, "src");
        const char *picture_cand = pick_picture_source_url(doc, elem);
        const char *src          = orig_src;
        const char *fallback     = NULL;

        if (picture_cand) {
            src = picture_cand;
        } else if (is_placeholder_img_src(src)) {
            fallback = pick_lazy_img_fallback(doc, elem);
            if (fallback) {
                src = fallback;
            }
        }

        if (src && src[0] != '\0') {
            const char *tv = src;
            size_t      tl = strlen(src);
            arcr_str_trim_view(&tv, &tl);
            if (tl > 0u) {
                if (arcr_str_starts_with_nocase(tv, "data:")) {
                    ArcrImage img = {0, 0, NULL};
                    if (arcr_image_decode_data_uri(tv, tl, &img) &&
                        img.pixels && img.width > 0 && img.height > 0) {
                        attach_decoded_image_to_node(doc, elem, &img);
                    } else {
                        arcr_image_free(&img);
                        const char *alt_src = (picture_cand && !is_placeholder_img_src(orig_src))
                                                  ? orig_src
                                                  : (fallback ? NULL : pick_lazy_img_fallback(doc, elem));
                        if (alt_src && !arcr_str_starts_with_nocase(alt_src, "data:")) {
                            append_external_ref(doc, &doc->ext_images_head, alt_src, elem);
                        }
                    }
                } else {
                    const char *url_copy = (tv == src && src[tl] == '\0')
                                               ? src
                                               : arcr_arena_strdup(&doc->arena, tv, tl);
                    append_external_ref(doc, &doc->ext_images_head, url_copy, elem);
                }
            }
        }
    } else if (arcr_str_eq_nocase_n(tag_ptr, tag_len, "video", 5u)) {
        doc->has_media = true;
        const char *poster = arcr_dom_get_attr_cstr(doc, elem, "poster");
        if (poster && poster[0] != '\0' && !arcr_str_starts_with_nocase(poster, "data:")) {
            append_external_ref(doc, &doc->ext_images_head, poster, elem);
        }
    } else if (arcr_str_eq_nocase_n(tag_ptr, tag_len, "input", 5u)) {
        if (!elem->input_value) {
            const char *v = arcr_dom_get_attr_cstr(doc, elem, "value");
            arcr_dom_set_input_value(doc, elem, v ? v : "");
        }
        if (arcr_dom_get_attr_cstr(doc, elem, "checked") != NULL) {
            elem->state_flags |= ARCR_STATE_CHECKED;
        }
    } else if (arcr_str_eq_nocase_n(tag_ptr, tag_len, "canvas", 6u)) {
        int32_t cw = 300;
        int32_t ch = 150;
        const char *ws = arcr_dom_get_attr_cstr(doc, elem, "width");
        const char *hs = arcr_dom_get_attr_cstr(doc, elem, "height");
        if (ws) {
            int32_t pw = 0;
            if (arcr_parse_i32(ws, strlen(ws), &pw) && pw > 0 && pw <= 4096) cw = pw;
        }
        if (hs) {
            int32_t ph = 0;
            if (arcr_parse_i32(hs, strlen(hs), &ph) && ph > 0 && ph <= 4096) ch = ph;
        }
        (void)arcr_dom_ensure_surface(doc, elem, cw, ch);
    }
}

/* -------------------------------------------------------------------------
 * Inline <svg>...</svg> Dimension Extraction & Vector Rasterization
 * ------------------------------------------------------------------------- */
static bool parse_svg_num(const char *s, size_t len, double *out_val, size_t *out_end) {
    size_t i = 0u;
    while (i < len && (is_space(s[i]) || s[i] == ',')) ++i;
    if (i >= len) return false;

    double sign = 1.0;
    if (s[i] == '-') {
        sign = -1.0;
        ++i;
    } else if (s[i] == '+') {
        ++i;
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
    *out_val = sign * val;
    if (out_end) *out_end = i;
    return true;
}

static int32_t parse_svg_length_attr_px(const char *attr) {
    if (!attr || attr[0] == '\0') return 0;
    size_t len   = strlen(attr);
    double val   = 0.0;
    size_t n_end = 0u;
    if (!parse_svg_num(attr, len, &val, &n_end) || val <= 0.0) return 0;

    const char *u = attr + n_end;
    size_t      ul = len - n_end;
    arcr_str_trim_view(&u, &ul);

    /* Ignore percentage sizes (e.g. width="100%") for intrinsic surface raster size */
    if (ul > 0u && u[0] == '%') return 0;
    if (arcr_str_eq_nocase_n(u, ul, "rem", 3u) || arcr_str_eq_nocase_n(u, ul, "em", 2u)) {
        val *= 16.0;
    } else if (arcr_str_eq_nocase_n(u, ul, "pt", 2u)) {
        val *= (4.0 / 3.0);
    }
    int32_t px = (int32_t)(val + 0.5);
    return ARCR_CLAMP(px, 1, 1024);
}

static bool parse_svg_viewbox(const char *vb, double *out_w, double *out_h) {
    if (!vb || vb[0] == '\0') return false;
    size_t len = strlen(vb);
    size_t pos = 0u;
    double nums[4] = {0.0, 0.0, 0.0, 0.0};
    for (int k = 0; k < 4; ++k) {
        size_t adv = 0u;
        if (!parse_svg_num(vb + pos, len - pos, &nums[k], &adv)) return false;
        pos += adv;
    }
    if (nums[2] <= 0.0 || nums[3] <= 0.0) return false;
    *out_w = nums[2];
    *out_h = nums[3];
    return true;
}

static void extract_svg_tailwind_or_style_dims(const ArcrDocument *doc,
                                               const ArcrDomNode *svg_node,
                                               int32_t *inout_w, int32_t *inout_h) {
    /* 1. Check inline style="width:20px;height:20px" if present */
    const char *st = arcr_dom_get_attr_cstr(doc, svg_node, "style");
    if (st && st[0] != '\0') {
        size_t slen = strlen(st);
        size_t i    = 0u;
        while (i < slen) {
            size_t semi = i;
            while (semi < slen && st[semi] != ';') ++semi;
            size_t colon = i;
            while (colon < semi && st[colon] != ':') ++colon;
            if (colon < semi) {
                const char *k  = st + i;
                size_t      kl = colon - i;
                const char *v  = st + colon + 1u;
                size_t      vl = semi - colon - 1u;
                arcr_str_trim_view(&k, &kl);
                arcr_str_trim_view(&v, &vl);
                char vbuf[32];
                if (vl > 0u && vl < sizeof(vbuf)) {
                    memcpy(vbuf, v, vl);
                    vbuf[vl] = '\0';
                    if (*inout_w == 0 && arcr_str_eq_nocase_n(k, kl, "width", 5u)) {
                        *inout_w = parse_svg_length_attr_px(vbuf);
                    } else if (*inout_h == 0 && arcr_str_eq_nocase_n(k, kl, "height", 6u)) {
                        *inout_h = parse_svg_length_attr_px(vbuf);
                    }
                }
            }
            i = (semi < slen) ? (semi + 1u) : slen;
        }
    }

    /* 2. Check common Tailwind icon sizing classes (`w-4`, `h-5`, `size-6`, `w-[20px]`) */
    for (uint32_t c = 0u; c < svg_node->class_count; ++c) {
        ArcrAtom ca = (c < ARCR_MAX_INLINE_CLASSES)
                          ? svg_node->classes[c]
                          : svg_node->extra_classes[c - ARCR_MAX_INLINE_CLASSES];
        const char *cn = arcr_atom_cstr(doc->atoms, ca);
        if (!cn) continue;
        /* Strip responsive/state prefix like `sm:w-5` -> `w-5` */
        const char *colon = strrchr(cn, ':');
        if (colon) cn = colon + 1;

        bool set_w = false, set_h = false;
        const char *val_s = NULL;
        if (strncmp(cn, "w-", 2u) == 0)         { set_w = true; val_s = cn + 2; }
        else if (strncmp(cn, "h-", 2u) == 0)    { set_h = true; val_s = cn + 2; }
        else if (strncmp(cn, "size-", 5u) == 0) { set_w = true; set_h = true; val_s = cn + 5; }
        if (!val_s) continue;

        int32_t px = 0;
        size_t vlen = strlen(val_s);
        if (vlen >= 3u && val_s[0] == '[' && val_s[vlen - 1u] == ']') {
            char inner[32];
            if (vlen - 2u < sizeof(inner)) {
                memcpy(inner, val_s + 1, vlen - 2u);
                inner[vlen - 2u] = '\0';
                px = parse_svg_length_attr_px(inner);
            }
        } else {
            double num = 0.0;
            size_t end = 0u;
            if (parse_svg_num(val_s, vlen, &num, &end) && end == vlen && num > 0.0 && num <= 96.0) {
                px = (int32_t)(num * 4.0 + 0.5); /* Tailwind 1 spacing unit = 0.25rem = 4px */
            }
        }
        if (px > 0) {
            if (set_w && *inout_w == 0) *inout_w = px;
            if (set_h && *inout_h == 0) *inout_h = px;
        }
    }
}

static bool svg_subtree_has_text_element(const ArcrDocument *doc, const ArcrDomNode *node,
                                         ArcrAtom text_atom, ArcrAtom fo_atom) {
    (void)doc;
    for (const ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        if (ch->kind == ARCR_NODE_ELEMENT) {
            if (ch->tag == text_atom || ch->tag == fo_atom) return true;
            if (svg_subtree_has_text_element(doc, ch, text_atom, fo_atom)) return true;
        }
    }
    return false;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_simple_hex_color(const char *s, uint32_t *out_argb) {
    if (!s) return false;
    while (is_space(*s)) ++s;
    if (*s != '#') return false;
    ++s;
    size_t n = 0u;
    while (hex_val(s[n]) >= 0) ++n;
    if (n == 3u) {
        uint32_t r = (uint32_t)(hex_val(s[0]) * 17);
        uint32_t g = (uint32_t)(hex_val(s[1]) * 17);
        uint32_t b = (uint32_t)(hex_val(s[2]) * 17);
        *out_argb  = 0xFF000000u | (r << 16) | (g << 8) | b;
        return true;
    }
    if (n == 6u) {
        uint32_t r = (uint32_t)((hex_val(s[0]) << 4) | hex_val(s[1]));
        uint32_t g = (uint32_t)((hex_val(s[2]) << 4) | hex_val(s[3]));
        uint32_t b = (uint32_t)((hex_val(s[4]) << 4) | hex_val(s[5]));
        *out_argb  = 0xFF000000u | (r << 16) | (g << 8) | b;
        return true;
    }
    return false;
}

static uint32_t infer_svg_current_color(const ArcrDocument *doc, const ArcrDomNode *svg_node) {
    for (const ArcrDomNode *anc = svg_node; anc != NULL; anc = anc->parent) {
        if (anc->kind != ARCR_NODE_ELEMENT) continue;
        const char *col_attr = arcr_dom_get_attr_cstr(doc, anc, "color");
        uint32_t c = 0u;
        if (parse_simple_hex_color(col_attr, &c)) return c;

        const char *st = arcr_dom_get_attr_cstr(doc, anc, "style");
        if (st) {
            const char *p = st;
            while (*p) {
                while (*p && is_space(*p)) ++p;
                if (arcr_str_starts_with_nocase(p, "color:")) {
                    if (parse_simple_hex_color(p + 6, &c)) return c;
                }
                while (*p && *p != ';') ++p;
                if (*p == ';') ++p;
            }
        }
        /* Common Tailwind text color utility classes on button/icon parent */
        for (uint32_t k = 0u; k < anc->class_count; ++k) {
            ArcrAtom ca = (k < ARCR_MAX_INLINE_CLASSES)
                              ? anc->classes[k]
                              : anc->extra_classes[k - ARCR_MAX_INLINE_CLASSES];
            const char *cn = arcr_atom_cstr(doc->atoms, ca);
            if (!cn) continue;
            if (strcmp(cn, "text-white") == 0) return 0xFFFFFFFFu;
            if (strncmp(cn, "text-blue-", 10u) == 0) return 0xFF2563EBu;
            if (strncmp(cn, "text-sky-", 9u) == 0) return 0xFF0284C7u;
            if (strncmp(cn, "text-emerald-", 13u) == 0 ||
                strncmp(cn, "text-green-", 11u) == 0) return 0xFF16A34Au;
            if (strncmp(cn, "text-red-", 9u) == 0 ||
                strncmp(cn, "text-rose-", 10u) == 0) return 0xFFDC2626u;
            if (strncmp(cn, "text-amber-", 11u) == 0 ||
                strncmp(cn, "text-yellow-", 12u) == 0) return 0xFFD97706u;
            if (strncmp(cn, "text-gray-4", 11u) == 0 ||
                strncmp(cn, "text-slate-4", 12u) == 0) return 0xFF94A3B8u;
        }
    }
    return 0xFF1E293Bu; /* Crisp modern slate icon foreground */
}

static void rasterize_inline_svg_element(ArcrDocument *doc, ArcrDomNode *svg_node) {
    if (!doc || !svg_node || svg_node->surface) return;

    int32_t attr_w = parse_svg_length_attr_px(arcr_dom_get_attr_cstr(doc, svg_node, "width"));
    int32_t attr_h = parse_svg_length_attr_px(arcr_dom_get_attr_cstr(doc, svg_node, "height"));
    if (attr_w == 0 || attr_h == 0) {
        extract_svg_tailwind_or_style_dims(doc, svg_node, &attr_w, &attr_h);
    }

    double vb_w = 0.0, vb_h = 0.0;
    bool has_vb = parse_svg_viewbox(arcr_dom_get_attr_cstr(doc, svg_node, "viewbox"), &vb_w, &vb_h);

    int32_t target_w = 24;
    int32_t target_h = 24;

    if (attr_w > 0 && attr_h > 0) {
        target_w = attr_w;
        target_h = attr_h;
    } else if (attr_w > 0) {
        target_w = attr_w;
        target_h = has_vb ? ARCR_MAX(1, (int32_t)((double)attr_w * (vb_h / vb_w) + 0.5)) : attr_w;
    } else if (attr_h > 0) {
        target_h = attr_h;
        target_w = has_vb ? ARCR_MAX(1, (int32_t)((double)attr_h * (vb_w / vb_h) + 0.5)) : attr_h;
    } else if (has_vb) {
        ArcrAtom text_atom = arcr_atom_intern_lower(doc->atoms, "text", 4u);
        ArcrAtom fo_atom   = arcr_atom_intern_lower(doc->atoms, "foreignobject", 13u);
        bool is_diagram    = svg_subtree_has_text_element(doc, svg_node, text_atom, fo_atom);

        if (is_diagram) {
            double scale = (vb_w > 640.0) ? (640.0 / vb_w) : 1.0;
            target_w = ARCR_CLAMP((int32_t)(vb_w * scale + 0.5), 24, 640);
            target_h = ARCR_CLAMP((int32_t)(vb_h * scale + 0.5), 24, 640);
        } else if (vb_w <= 64.0 && vb_h <= 64.0) {
            double max_d = (vb_w > vb_h) ? vb_w : vb_h;
            double scale = (max_d < 16.0) ? (24.0 / max_d) : 1.0;
            target_w = ARCR_CLAMP((int32_t)(vb_w * scale + 0.5), 12, 128);
            target_h = ARCR_CLAMP((int32_t)(vb_h * scale + 0.5), 12, 128);
        } else {
            /* Icon grid viewBox (e.g. FontAwesome 0 0 512 512 / Material 0 0 960 960): default 24px */
            double max_d = (vb_w > vb_h) ? vb_w : vb_h;
            double scale = 24.0 / max_d;
            target_w = ARCR_CLAMP((int32_t)(vb_w * scale + 0.5), 12, 64);
            target_h = ARCR_CLAMP((int32_t)(vb_h * scale + 0.5), 12, 64);
        }
    }

    target_w = ARCR_CLAMP(target_w, 1, 1024);
    target_h = ARCR_CLAMP(target_h, 1, 1024);

    char       *ser_xml = NULL;
    const char *svg_xml = svg_node->raw_svg_markup;
    size_t      svg_len = svg_node->raw_svg_len;

    if (!svg_xml || svg_len == 0u) {
        ser_xml = arcr_dom_serialize_html_alloc(doc, svg_node, true, &svg_len);
        if (!ser_xml || svg_len == 0u) {
            free(ser_xml);
            return;
        }
        for (size_t k = 0u; k + 8u <= svg_len; ++k) {
            if (memcmp(ser_xml + k, "viewbox=", 8u) == 0) {
                ser_xml[k + 4u] = 'B';
            }
        }
        svg_node->raw_svg_markup = arcr_arena_strdup(&doc->arena, ser_xml, svg_len);
        svg_node->raw_svg_len    = svg_len;
        svg_xml                  = svg_node->raw_svg_markup;
        free(ser_xml);
        if (!svg_xml) return;
    }

    uint32_t  cur_color = infer_svg_current_color(doc, svg_node);
    ArcrImage img       = {0, 0, NULL};
    if (arcr_svg_rasterize_mem(svg_xml, svg_len, target_w, target_h, cur_color, &img) &&
        img.pixels && img.width > 0 && img.height > 0) {
        attach_decoded_image_to_node(doc, svg_node, &img);
        svg_node->svg_rendered_color = cur_color;
        svg_node->svg_rendered_w     = img.width;
        svg_node->svg_rendered_h     = img.height;
        doc->has_images              = true;
    } else {
        arcr_image_free(&img);
    }
}

/* -------------------------------------------------------------------------
 * HTML5 Tree Construction, Error Recovery & Resumable Parser State Machine
 * ------------------------------------------------------------------------- */
#define ARCR_HTML_MAX_WRITE_BYTES (64u * 1024u * 1024u)

typedef struct HtmlParseAtoms {
    ArcrAtom html;
    ArcrAtom head;
    ArcrAtom body;
    ArcrAtom p;
    ArcrAtom li;
    ArcrAtom ul;
    ArcrAtom ol;
    ArcrAtom pre;
    ArcrAtom table;
    ArcrAtom caption;
    ArcrAtom colgroup;
    ArcrAtom col;
    ArcrAtom thead;
    ArcrAtom tbody;
    ArcrAtom tfoot;
    ArcrAtom tr;
    ArcrAtom td;
    ArcrAtom th;
    ArcrAtom dl;
    ArcrAtom dt;
    ArcrAtom dd;
    ArcrAtom option;
    ArcrAtom svg;
    ArcrAtom math;
    ArcrAtom template_tag;
    ArcrAtom script;
    ArcrAtom style;
} HtmlParseAtoms;

static void init_parse_atoms(ArcrAtomTable *atoms, HtmlParseAtoms *pa) {
    pa->html         = arcr_atom_intern_lower(atoms, "html", 4u);
    pa->head         = arcr_atom_intern_lower(atoms, "head", 4u);
    pa->body         = arcr_atom_intern_lower(atoms, "body", 4u);
    pa->p            = arcr_atom_intern_lower(atoms, "p", 1u);
    pa->li           = arcr_atom_intern_lower(atoms, "li", 2u);
    pa->ul           = arcr_atom_intern_lower(atoms, "ul", 2u);
    pa->ol           = arcr_atom_intern_lower(atoms, "ol", 2u);
    pa->pre          = arcr_atom_intern_lower(atoms, "pre", 3u);
    pa->table        = arcr_atom_intern_lower(atoms, "table", 5u);
    pa->caption      = arcr_atom_intern_lower(atoms, "caption", 7u);
    pa->colgroup     = arcr_atom_intern_lower(atoms, "colgroup", 8u);
    pa->col          = arcr_atom_intern_lower(atoms, "col", 3u);
    pa->thead        = arcr_atom_intern_lower(atoms, "thead", 5u);
    pa->tbody        = arcr_atom_intern_lower(atoms, "tbody", 5u);
    pa->tfoot        = arcr_atom_intern_lower(atoms, "tfoot", 5u);
    pa->tr           = arcr_atom_intern_lower(atoms, "tr", 2u);
    pa->td           = arcr_atom_intern_lower(atoms, "td", 2u);
    pa->th           = arcr_atom_intern_lower(atoms, "th", 2u);
    pa->dl           = arcr_atom_intern_lower(atoms, "dl", 2u);
    pa->dt           = arcr_atom_intern_lower(atoms, "dt", 2u);
    pa->dd           = arcr_atom_intern_lower(atoms, "dd", 2u);
    pa->option       = arcr_atom_intern_lower(atoms, "option", 6u);
    pa->svg          = arcr_atom_intern_lower(atoms, "svg", 3u);
    pa->math         = arcr_atom_intern_lower(atoms, "math", 4u);
    pa->template_tag = arcr_atom_intern_lower(atoms, "template", 8u);
    pa->script       = arcr_atom_intern_lower(atoms, "script", 6u);
    pa->style        = arcr_atom_intern_lower(atoms, "style", 5u);
}

struct ArcrHtmlParser {
    ArcrDocument  *doc;
    HtmlParseAtoms pa;
    char          *buf;
    size_t         len;
    size_t         cap;
    size_t         pos;
    size_t         write_cursor;
    size_t         written_total;

    ArcrDomNode  **stack;
    size_t         stack_cap;
    size_t         depth;

    ArcrDomNode   *html_el;
    ArcrDomNode   *body_el;
    ArcrDomNode   *svg_open_node;
    size_t         svg_open_offset;

    size_t         svg_depth;
    size_t         math_depth;
    size_t         template_depth;

    bool           scripting_enabled;
    bool           pauses_enabled;
    bool           is_fragment;
    bool           seen_doctype;
    bool           done;
};

static bool push_open_element(ArcrDomNode ***inout_stack, size_t *inout_cap,
                              size_t *inout_depth, ArcrDomNode *node) {
    if (!node) return false;
    if (*inout_depth >= ARCR_HTML_MAX_DEPTH) return false;
    if (*inout_depth >= *inout_cap) {
        size_t ncap = (*inout_cap == 0u) ? ARCR_HTML_INIT_STACK_CAP : (*inout_cap * 2u);
        if (ncap > ARCR_HTML_MAX_DEPTH) ncap = ARCR_HTML_MAX_DEPTH;
        ArcrDomNode **grown = (ArcrDomNode **)realloc(*inout_stack, ncap * sizeof(ArcrDomNode *));
        if (!grown) return false;
        *inout_stack = grown;
        *inout_cap   = ncap;
    }
    (*inout_stack)[(*inout_depth)++] = node;
    return true;
}

static size_t count_open_tag_depth(ArcrDomNode *const *stack, size_t depth, ArcrAtom target_atom) {
    size_t cnt = 0u;
    for (size_t k = 1u; k < depth; ++k) {
        if (stack[k]->kind == ARCR_NODE_ELEMENT && stack[k]->tag == target_atom) ++cnt;
    }
    return cnt;
}

static bool is_formatting_tag_cstr(const char *t) {
    if (!t) return false;
    static const char *const kFmt[] = {
        "a", "b", "big", "code", "em", "font", "i",
        "nobr", "s", "small", "strike", "strong", "tt", "u"
    };
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kFmt); ++i) {
        if (strcmp(t, kFmt[i]) == 0) return true;
    }
    return false;
}

static bool is_formatting_node(const ArcrDocument *doc, const ArcrDomNode *n) {
    if (!n || n->kind != ARCR_NODE_ELEMENT) return false;
    return is_formatting_tag_cstr(arcr_atom_cstr(doc->atoms, n->tag));
}

static bool is_scope_boundary_node(const ArcrDomNode *n, const HtmlParseAtoms *pa) {
    if (!n) return true;
    if (n->kind == ARCR_NODE_DOCUMENT) return true; /* root or <template> fragment boundary */
    ArcrAtom t = n->tag;
    return t == pa->html || t == pa->body || t == pa->table || t == pa->thead ||
           t == pa->tbody || t == pa->tfoot || t == pa->tr || t == pa->td ||
           t == pa->th || t == pa->caption || t == pa->template_tag ||
           t == pa->svg || t == pa->math;
}

static ArcrDomNode *clone_element_shallow(ArcrDocument *doc, const ArcrDomNode *src) {
    if (!doc || !src || src->kind != ARCR_NODE_ELEMENT) return NULL;
    const char *tname = arcr_atom_cstr(doc->atoms, src->tag);
    if (!tname) return NULL;
    ArcrDomNode *clone = arcr_dom_create_element(doc, tname, strlen(tname));
    if (!clone) return NULL;
    for (const ArcrAttr *a = src->first_attr; a != NULL; a = a->next) {
        const char *aname = arcr_atom_cstr(doc->atoms, a->name);
        const char *aval  = a->value ? a->value : "";
        if (aname) {
            arcr_dom_set_attr(doc, clone, aname, strlen(aname), aval, strlen(aval));
        }
    }
    return clone;
}

static bool is_table_foster_parent(const ArcrDomNode *n, const HtmlParseAtoms *pa) {
    if (!n || n->kind != ARCR_NODE_ELEMENT) return false;
    ArcrAtom t = n->tag;
    return t == pa->table || t == pa->thead || t == pa->tbody ||
           t == pa->tfoot || t == pa->tr;
}

static bool is_allowed_in_table_context(const ArcrDomNode *parent, const ArcrDomNode *child,
                                        const HtmlParseAtoms *pa) {
    if (!parent || !child) return true;
    if (child->kind == ARCR_NODE_COMMENT) return true;
    if (child->kind != ARCR_NODE_ELEMENT) return false; /* Text in table/row is foster-parented */

    ArcrAtom ct = child->tag;
    if (ct == pa->script || ct == pa->style || ct == pa->template_tag) return true;

    ArcrAtom pt = parent->tag;
    if (pt == pa->table) {
        return ct == pa->caption || ct == pa->colgroup || ct == pa->col ||
               ct == pa->thead || ct == pa->tbody || ct == pa->tfoot ||
               ct == pa->tr || ct == pa->td || ct == pa->th;
    }
    if (pt == pa->thead || pt == pa->tbody || pt == pa->tfoot) {
        return ct == pa->tr || ct == pa->td || ct == pa->th;
    }
    if (pt == pa->tr) {
        return ct == pa->td || ct == pa->th;
    }
    return true;
}

/* WHATWG HTML5 §13.2.6.1 Foster Parenting insertion into current open element */
static void insert_node_foster_aware(ArcrHtmlParser *p, ArcrDomNode *cur_parent,
                                     ArcrDomNode *child) {
    if (!p || !cur_parent || !child) return;
    if (p->svg_depth == 0u && p->math_depth == 0u &&
        is_table_foster_parent(cur_parent, &p->pa) &&
        !is_allowed_in_table_context(cur_parent, child, &p->pa)) {
        /* Find nearest open <table> in current template/document scope */
        ArcrDomNode *table_el = NULL;
        for (size_t k = p->depth; k > 0u; --k) {
            ArcrDomNode *sn = p->stack[k - 1u];
            if (sn->kind == ARCR_NODE_DOCUMENT) break;
            if (sn->tag == p->pa.table) {
                table_el = sn;
                break;
            }
        }
        if (table_el && table_el->parent) {
            arcr_dom_insert_before(table_el->parent, child, table_el);
            return;
        }
    }
    arcr_dom_append_child(cur_parent, child);
}

static bool contains_ascii_nocase(const char *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0u || hlen < nlen) return false;
    for (size_t i = 0u; i + nlen <= hlen; ++i) {
        if (arcr_str_eq_nocase_n(hay + i, nlen, needle, nlen)) return true;
    }
    return false;
}

/* WHATWG HTML5 §13.2.5.52-67 & §13.2.6.4.1 DOCTYPE Quirks Mode determination */
static ArcrQuirksMode classify_doctype_quirks(const char *s, size_t len) {
    /* `s[0..len)` is inside `<! ... >` after `<!` */
    size_t i = 0u;
    while (i < len && is_space(s[i])) ++i;
    if (i + 7u > len || !arcr_str_eq_nocase_n(s + i, 7u, "doctype", 7u)) {
        return ARCR_QUIRKS_FULL_QUIRKS;
    }
    i += 7u;
    if (i < len && !is_space(s[i])) {
        return ARCR_QUIRKS_FULL_QUIRKS;
    }
    while (i < len && is_space(s[i])) ++i;
    if (i >= len) {
        return ARCR_QUIRKS_FULL_QUIRKS; /* `<!DOCTYPE>` without name -> quirks */
    }

    size_t nstart = i;
    while (i < len && !is_space(s[i])) ++i;
    size_t nlen = i - nstart;
    if (!arcr_str_eq_nocase_n(s + nstart, nlen, "html", 4u)) {
        return ARCR_QUIRKS_FULL_QUIRKS;
    }

    while (i < len && is_space(s[i])) ++i;
    if (i >= len) {
        return ARCR_QUIRKS_NO_QUIRKS; /* Standard `<!DOCTYPE html>` */
    }

    bool        has_pub = false;
    bool        has_sys = false;
    const char *pub_ptr = "";
    size_t      pub_len = 0u;
    const char *sys_ptr = "";
    size_t      sys_len = 0u;

    if (i + 6u <= len && arcr_str_eq_nocase_n(s + i, 6u, "public", 6u)) {
        has_pub = true;
        i += 6u;
        while (i < len && is_space(s[i])) ++i;
        if (i < len && (s[i] == '"' || s[i] == '\'')) {
            char q = s[i++];
            size_t ps = i;
            while (i < len && s[i] != q) ++i;
            pub_ptr = s + ps;
            pub_len = i - ps;
            if (i < len) ++i;
        } else {
            return ARCR_QUIRKS_FULL_QUIRKS;
        }
        while (i < len && is_space(s[i])) ++i;
        if (i < len && (s[i] == '"' || s[i] == '\'')) {
            has_sys = true;
            char q = s[i++];
            size_t ss = i;
            while (i < len && s[i] != q) ++i;
            sys_ptr = s + ss;
            sys_len = i - ss;
            if (i < len) ++i;
        }
    } else if (i + 6u <= len && arcr_str_eq_nocase_n(s + i, 6u, "system", 6u)) {
        has_sys = true;
        i += 6u;
        while (i < len && is_space(s[i])) ++i;
        if (i < len && (s[i] == '"' || s[i] == '\'')) {
            char q = s[i++];
            size_t ss = i;
            while (i < len && s[i] != q) ++i;
            sys_ptr = s + ss;
            sys_len = i - ss;
            if (i < len) ++i;
        } else {
            return ARCR_QUIRKS_FULL_QUIRKS;
        }
    } else {
        /* Bogus token after `<!DOCTYPE html ...` */
        return ARCR_QUIRKS_FULL_QUIRKS;
    }

    if (has_sys && arcr_str_eq_nocase_n(
                       sys_ptr, sys_len,
                       "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd", 58u)) {
        return ARCR_QUIRKS_FULL_QUIRKS;
    }

    if (has_pub) {
        if (contains_ascii_nocase(pub_ptr, pub_len, "xhtml 1.0 transitional") ||
            contains_ascii_nocase(pub_ptr, pub_len, "xhtml 1.0 frameset")) {
            return ARCR_QUIRKS_LIMITED_QUIRKS;
        }
        if (contains_ascii_nocase(pub_ptr, pub_len, "html 4.01 transitional") ||
            contains_ascii_nocase(pub_ptr, pub_len, "html 4.01 frameset")) {
            return has_sys ? ARCR_QUIRKS_LIMITED_QUIRKS : ARCR_QUIRKS_FULL_QUIRKS;
        }
        if (contains_ascii_nocase(pub_ptr, pub_len, "dtd html 2.") ||
            contains_ascii_nocase(pub_ptr, pub_len, "dtd html 3.") ||
            contains_ascii_nocase(pub_ptr, pub_len, "dtd html 4.0 ") ||
            contains_ascii_nocase(pub_ptr, pub_len, "dtd html 4.0/") ||
            contains_ascii_nocase(pub_ptr, pub_len, "dtd html//") ||
            contains_ascii_nocase(pub_ptr, pub_len, "+//silmaril//") ||
            contains_ascii_nocase(pub_ptr, pub_len, "-//microsoft//") ||
            contains_ascii_nocase(pub_ptr, pub_len, "-//netscape") ||
            contains_ascii_nocase(pub_ptr, pub_len, "-//ietf//") ||
            contains_ascii_nocase(pub_ptr, pub_len, "-//softquad") ||
            contains_ascii_nocase(pub_ptr, pub_len, "-//webtechs//")) {
            return ARCR_QUIRKS_FULL_QUIRKS;
        }
    }

    return ARCR_QUIRKS_NO_QUIRKS;
}

static void auto_close_implicit_tags(ArcrDomNode **stack, size_t *inout_depth,
                                     const HtmlParseAtoms *pa,
                                     const char *tag_ptr, size_t tag_len,
                                     ArcrAtom tag_atom) {
    size_t depth = *inout_depth;

    /* 1. Block tags auto-close open <p> in button scope */
    if (closes_open_paragraph(tag_ptr, tag_len)) {
        for (size_t k = depth; k > 1u; --k) {
            if (stack[k - 1u]->kind == ARCR_NODE_DOCUMENT) break;
            ArcrAtom st = stack[k - 1u]->tag;
            if (st == pa->p) {
                depth = k - 1u;
                break;
            }
            if (st == pa->body || st == pa->table || st == pa->td || st == pa->th ||
                st == pa->template_tag || st == pa->svg || st == pa->math) {
                break;
            }
        }
    }

    /* 2. <li> auto-closes open <li> inside same list */
    if (tag_atom == pa->li) {
        for (size_t k = depth; k > 1u; --k) {
            if (stack[k - 1u]->kind == ARCR_NODE_DOCUMENT) break;
            ArcrAtom st = stack[k - 1u]->tag;
            if (st == pa->li) {
                depth = k - 1u;
                break;
            }
            if (st == pa->ul || st == pa->ol || st == pa->body || st == pa->template_tag) break;
        }
    }

    /* 3. <td> / <th> auto-closes open <td> / <th> inside same row */
    if (tag_atom == pa->td || tag_atom == pa->th) {
        for (size_t k = depth; k > 1u; --k) {
            if (stack[k - 1u]->kind == ARCR_NODE_DOCUMENT) break;
            ArcrAtom st = stack[k - 1u]->tag;
            if (st == pa->td || st == pa->th) {
                depth = k - 1u;
                break;
            }
            if (st == pa->tr || st == pa->table || st == pa->body || st == pa->template_tag) break;
        }
    }

    /* 4. <tr> auto-closes open <td>, <th>, and sibling <tr> */
    if (tag_atom == pa->tr) {
        for (size_t k = depth; k > 1u; --k) {
            if (stack[k - 1u]->kind == ARCR_NODE_DOCUMENT) break;
            ArcrAtom st = stack[k - 1u]->tag;
            if (st == pa->tr) {
                depth = k - 1u;
                break;
            }
            if (st == pa->thead || st == pa->tbody || st == pa->tfoot ||
                st == pa->table || st == pa->body || st == pa->template_tag) {
                break;
            }
        }
    }

    /* 5. <thead> / <tbody> / <tfoot> auto-closes open cells/rows/sections */
    if (tag_atom == pa->thead || tag_atom == pa->tbody || tag_atom == pa->tfoot) {
        for (size_t k = depth; k > 1u; --k) {
            if (stack[k - 1u]->kind == ARCR_NODE_DOCUMENT) break;
            ArcrAtom st = stack[k - 1u]->tag;
            if (st == pa->thead || st == pa->tbody || st == pa->tfoot) {
                depth = k - 1u;
                break;
            }
            if (st == pa->table || st == pa->body || st == pa->template_tag) break;
        }
    }

    /* 6. <dt> / <dd> and <option> */
    if (tag_atom == pa->dt || tag_atom == pa->dd) {
        for (size_t k = depth; k > 1u; --k) {
            if (stack[k - 1u]->kind == ARCR_NODE_DOCUMENT) break;
            ArcrAtom st = stack[k - 1u]->tag;
            if (st == pa->dt || st == pa->dd) {
                depth = k - 1u;
                break;
            }
            if (st == pa->dl || st == pa->body || st == pa->template_tag) break;
        }
    }
    if (tag_atom == pa->option && depth > 1u &&
        stack[depth - 1u]->kind == ARCR_NODE_ELEMENT &&
        stack[depth - 1u]->tag == pa->option) {
        --depth;
    }

    *inout_depth = depth;
}

static void handle_close_tag(ArcrHtmlParser *p, ArcrAtom close_atom) {
    ArcrDocument *doc = p->doc;

    /* Closing </template>: pop inert template_content fragment and <template> element */
    if (p->svg_depth == 0u && p->math_depth == 0u && close_atom == p->pa.template_tag) {
        for (size_t k = p->depth; k > 1u; --k) {
            if (p->stack[k - 1u]->kind == ARCR_NODE_ELEMENT &&
                p->stack[k - 1u]->tag == p->pa.template_tag) {
                p->depth = k - 1u;
                if (p->template_depth > 0u) --p->template_depth;
                p->svg_depth  = count_open_tag_depth(p->stack, p->depth, p->pa.svg);
                p->math_depth = count_open_tag_depth(p->stack, p->depth, p->pa.math);
                return;
            }
        }
        return;
    }

    /* Search stack up to current scope boundary */
    size_t match_idx = (size_t)-1;
    for (size_t k = p->depth; k > 1u; --k) {
        ArcrDomNode *sn = p->stack[k - 1u];
        if (sn->kind == ARCR_NODE_DOCUMENT) break; /* Never pop across <template> fragment! */
        if (sn->tag == close_atom) {
            match_idx = k - 1u;
            break;
        }
        if (p->svg_depth == 0u && p->math_depth == 0u &&
            close_atom != p->pa.table && close_atom != p->pa.tr &&
            close_atom != p->pa.td && close_atom != p->pa.th &&
            close_atom != p->pa.tbody && close_atom != p->pa.thead &&
            close_atom != p->pa.tfoot &&
            (sn->tag == p->pa.table || sn->tag == p->pa.td || sn->tag == p->pa.th)) {
            /* Do not pop across table/cell scoping boundaries for unrelated close tags */
            break;
        }
    }
    if (match_idx == (size_t)-1) return;

    ArcrDomNode *closed_el = p->stack[match_idx];

    /*
     * Misnested Formatting Element Recovery (WHATWG Adoption Agency core):
     * e.g. `<b><i>bold-italic</b>italic</i>` -> close `<i>` & `<b>` and reopen a cloned `<i>`
     * so trailing `"italic"` remains italicized without corrupting sibling hierarchy!
     */
    if (p->svg_depth == 0u && p->math_depth == 0u &&
        match_idx + 1u < p->depth && is_formatting_node(doc, closed_el)) {
        bool all_fmt_above    = true;
        size_t furthest_block = (size_t)-1;
        for (size_t m = match_idx + 1u; m < p->depth; ++m) {
            if (is_scope_boundary_node(p->stack[m], &p->pa)) {
                all_fmt_above  = false;
                furthest_block = (size_t)-1;
                break;
            }
            if (!is_formatting_node(doc, p->stack[m])) {
                all_fmt_above = false;
                if (furthest_block == (size_t)-1) furthest_block = m;
            }
        }

        if (all_fmt_above) {
            size_t count_above = p->depth - (match_idx + 1u);
            if (count_above <= 8u) {
                ArcrDomNode *to_reopen[8];
                for (size_t m = 0u; m < count_above; ++m) {
                    to_reopen[m] = p->stack[match_idx + 1u + m];
                }
                p->depth = match_idx;
                for (size_t m = 0u; m < count_above; ++m) {
                    ArcrDomNode *reopened = clone_element_shallow(doc, to_reopen[m]);
                    if (reopened) {
                        insert_node_foster_aware(p, p->stack[p->depth - 1u], reopened);
                        (void)push_open_element(&p->stack, &p->stack_cap, &p->depth, reopened);
                    }
                }
                return;
            }
        } else if (furthest_block != (size_t)-1 && closed_el->parent) {
            /* `<b><div>bold-div</b>after</div>` adoption agency reparenting */
            ArcrDomNode *fb       = p->stack[furthest_block];
            ArcrDomNode *fmt_copy = clone_element_shallow(doc, closed_el);
            if (fmt_copy) {
                while (fb->first_child) {
                    ArcrDomNode *ch = fb->first_child;
                    arcr_dom_append_child(fmt_copy, ch);
                }
                arcr_dom_append_child(fb, fmt_copy);
                arcr_dom_insert_before(closed_el->parent, fb, closed_el->next_sibling);
                for (size_t m = match_idx; m + 1u < p->depth; ++m) {
                    p->stack[m] = p->stack[m + 1u];
                }
                p->depth--;
                return;
            }
        }
    }

    if (p->is_fragment || (close_atom != p->pa.html && close_atom != p->pa.body)) {
        p->depth = match_idx;
    }

    if (p->svg_depth > 0u) {
        size_t new_svg_depth = count_open_tag_depth(p->stack, p->depth, p->pa.svg);
        if (new_svg_depth == 0u && closed_el->tag == p->pa.svg) {
            if (closed_el == p->svg_open_node && p->pos >= p->svg_open_offset) {
                size_t raw_svg_len       = p->pos - p->svg_open_offset;
                closed_el->raw_svg_markup = arcr_arena_strdup(&doc->arena,
                                                              p->buf + p->svg_open_offset,
                                                              raw_svg_len);
                closed_el->raw_svg_len    = raw_svg_len;
            }
            p->svg_open_node = NULL;
            if (p->template_depth == 0u) {
                rasterize_inline_svg_element(doc, closed_el);
            }
        }
        p->svg_depth = new_svg_depth;
    }
    if (p->math_depth > 0u) {
        p->math_depth = count_open_tag_depth(p->stack, p->depth, p->pa.math);
    }
}

static void finalize_parser_eof(ArcrHtmlParser *p) {
    if (!p || p->done) return;
    p->done = true;
    ArcrDocument *doc = p->doc;

    /* Recover any unclosed top-level <svg> at EOF */
    if (p->svg_depth > 0u && p->template_depth == 0u) {
        for (size_t k = 1u; k < p->depth; ++k) {
            if (p->stack[k]->kind == ARCR_NODE_ELEMENT && p->stack[k]->tag == p->pa.svg) {
                rasterize_inline_svg_element(doc, p->stack[k]);
                break;
            }
        }
    }

    if (!p->is_fragment && !p->seen_doctype) {
        doc->quirks_mode = ARCR_QUIRKS_FULL_QUIRKS;
        p->seen_doctype  = true;
    }

    if (!p->is_fragment && !doc->body) {
        if (!p->html_el) {
            p->html_el = arcr_dom_create_element(doc, "html", 4u);
            if (p->html_el) arcr_dom_append_child(doc->root, p->html_el);
        }
        if (p->html_el) {
            p->body_el = arcr_dom_create_element(doc, "body", 4u);
            doc->body  = p->body_el;
            if (p->body_el) arcr_dom_append_child(p->html_el, p->body_el);
        }
    }
}

static ArcrHtmlParser *create_parser_internal(ArcrDocument *doc, ArcrDomNode *root_container,
                                              const char *utf8_html, size_t len,
                                              bool scripting_enabled, bool pauses_enabled,
                                              bool is_fragment) {
    if (!doc || !root_container) return NULL;
    ArcrHtmlParser *p = (ArcrHtmlParser *)calloc(1u, sizeof(ArcrHtmlParser));
    if (!p) return NULL;

    p->doc               = doc;
    p->scripting_enabled = scripting_enabled;
    p->pauses_enabled    = pauses_enabled;
    p->is_fragment       = is_fragment;
    init_parse_atoms(doc->atoms, &p->pa);

    size_t init_cap = len + 1u;
    if (init_cap < 64u) init_cap = 64u;
    p->buf = (char *)malloc(init_cap);
    if (!p->buf) {
        free(p);
        return NULL;
    }
    if (utf8_html && len > 0u) {
        memcpy(p->buf, utf8_html, len);
    }
    p->buf[len]     = '\0';
    p->len          = len;
    p->cap          = init_cap;
    p->pos          = 0u;
    p->write_cursor = 0u;

    p->stack_cap = ARCR_HTML_INIT_STACK_CAP;
    p->stack     = (ArcrDomNode **)malloc(p->stack_cap * sizeof(ArcrDomNode *));
    if (!p->stack) {
        free(p->buf);
        free(p);
        return NULL;
    }
    p->stack[p->depth++] = root_container;

    for (const ArcrDomNode *anc = root_container; anc != NULL; anc = anc->parent) {
        if (anc->kind == ARCR_NODE_ELEMENT) {
            if (anc->tag == p->pa.svg)  ++p->svg_depth;
            if (anc->tag == p->pa.math) ++p->math_depth;
        }
    }
    return p;
}

ArcrHtmlParser *arcr_html_parser_begin(ArcrAtomTable *atoms, const char *utf8_html,
                                       size_t len, bool scripting_enabled) {
    ArcrDocument *doc = arcr_dom_document_create(atoms);
    if (!doc) return NULL;
    ArcrHtmlParser *p = create_parser_internal(doc, doc->root,
                                               utf8_html ? utf8_html : "",
                                               utf8_html ? len : 0u,
                                               scripting_enabled, true, false);
    if (!p) {
        arcr_dom_document_destroy(doc);
        return NULL;
    }
    return p;
}

ArcrDocument *arcr_html_parser_document(ArcrHtmlParser *parser) {
    return parser ? parser->doc : NULL;
}

bool arcr_html_parser_document_write(ArcrHtmlParser *parser,
                                     const char *utf8_chunk, size_t chunk_len) {
    if (!parser || parser->done) return false;
    if (!utf8_chunk || chunk_len == 0u) return true;
    if (chunk_len > ARCR_HTML_MAX_WRITE_BYTES ||
        parser->written_total + chunk_len > ARCR_HTML_MAX_WRITE_BYTES) {
        return false;
    }

    size_t insert_at = parser->write_cursor;
    if (insert_at < parser->pos) insert_at = parser->pos;
    if (insert_at > parser->len) insert_at = parser->len;

    size_t needed = parser->len + chunk_len + 1u;
    if (needed > parser->cap) {
        size_t ncap = parser->cap * 2u;
        while (ncap < needed) ncap *= 2u;
        char *nbuf = (char *)realloc(parser->buf, ncap);
        if (!nbuf) return false;
        parser->buf = nbuf;
        parser->cap = ncap;
    }

    memmove(parser->buf + insert_at + chunk_len,
            parser->buf + insert_at,
            parser->len - insert_at);
    memcpy(parser->buf + insert_at, utf8_chunk, chunk_len);
    parser->len           += chunk_len;
    parser->buf[parser->len] = '\0';
    parser->write_cursor   = insert_at + chunk_len;
    parser->written_total += chunk_len;
    return true;
}

ArcrHtmlPauseReason arcr_html_parser_resume(ArcrHtmlParser *p, ArcrDomNode **out_node) {
    if (out_node) *out_node = NULL;
    if (!p || p->done) return ARCR_HTML_PARSE_DONE;

    ArcrDocument *doc = p->doc;

    while (p->pos < p->len) {
        const char *utf8_html = p->buf;
        size_t      len       = p->len;
        size_t      i         = p->pos;

        if (utf8_html[i] == '<') {
            /* 1a. CDATA Section <![CDATA[ ... ]]> (foreign content SVG/MathML) */
            if (i + 9u <= len && memcmp(utf8_html + i, "<![CDATA[", 9u) == 0) {
                size_t cstart = i + 9u;
                size_t j      = cstart;
                while (j + 3u <= len && memcmp(utf8_html + j, "]]>", 3u) != 0) ++j;
                if (j > cstart && p->depth > 0u) {
                    ArcrDomNode *tn = arcr_dom_create_text(doc, utf8_html + cstart, j - cstart);
                    if (tn) insert_node_foster_aware(p, p->stack[p->depth - 1u], tn);
                }
                p->pos = (j + 3u <= len) ? (j + 3u) : len;
                continue;
            }

            /* 1b. HTML Comment <!-- ... --> including WHATWG abrupt <!--> and <!---> */
            if (i + 4u <= len && memcmp(utf8_html + i, "<!--", 4u) == 0) {
                size_t j = i + 4u;
                if (j < len && utf8_html[j] == '>') {
                    p->pos = j + 1u;
                    continue;
                }
                if (j + 1u < len && utf8_html[j] == '-' && utf8_html[j + 1u] == '>') {
                    p->pos = j + 2u;
                    continue;
                }
                while (j < len) {
                    if (j + 3u <= len && memcmp(utf8_html + j, "-->", 3u) == 0) {
                        j += 3u;
                        break;
                    }
                    if (j + 4u <= len && memcmp(utf8_html + j, "--!>", 4u) == 0) {
                        j += 4u;
                        break;
                    }
                    ++j;
                }
                p->pos = j;
                continue;
            }

            /* 2. DOCTYPE or Processing Instruction <! ... > / <? ... > */
            if (i + 2u <= len && (utf8_html[i + 1u] == '!' || utf8_html[i + 1u] == '?')) {
                size_t j = i + 2u;
                while (j < len && utf8_html[j] != '>') ++j;
                if (!p->is_fragment && !p->seen_doctype && utf8_html[i + 1u] == '!') {
                    doc->quirks_mode = classify_doctype_quirks(utf8_html + i + 2u, j - (i + 2u));
                    p->seen_doctype  = true;
                }
                p->pos = (j < len) ? (j + 1u) : len;
                continue;
            }

            /* 3. Closing tag </tag> (or bogus close tag `</>` / `</123>`) */
            if (i + 2u < len && utf8_html[i + 1u] == '/') {
                if (utf8_html[i + 2u] == '>') {
                    p->pos = i + 3u;
                    continue;
                }
                if (!is_ascii_alpha(utf8_html[i + 2u])) {
                    size_t j = i + 2u;
                    while (j < len && utf8_html[j] != '>') ++j;
                    p->pos = (j < len) ? (j + 1u) : len;
                    continue;
                }

                size_t j      = i + 2u;
                size_t tstart = j;
                while (j < len && is_tag_char(utf8_html[j])) ++j;
                size_t tend = j;
                while (j < len && utf8_html[j] != '>') ++j;
                p->pos = (j < len) ? (j + 1u) : len;

                if (tend > tstart) {
                    ArcrAtom close_atom = arcr_atom_intern_lower(
                        doc->atoms, utf8_html + tstart, tend - tstart);
                    handle_close_tag(p, close_atom);
                }
                continue;
            }

            /* 4. Opening tag <tag attr="val"> (MUST start with ASCII letter per HTML5 spec!) */
            if (i + 1u < len && is_ascii_alpha(utf8_html[i + 1u])) {
                size_t tag_lt_pos = i;
                size_t j          = i + 1u;
                size_t tstart     = j;
                while (j < len && is_tag_char(utf8_html[j])) ++j;
                const char *tag_ptr = utf8_html + tstart;
                size_t      tag_len = j - tstart;

                if (!p->is_fragment && !p->seen_doctype) {
                    doc->quirks_mode = ARCR_QUIRKS_FULL_QUIRKS;
                    p->seen_doctype  = true;
                }

                /*
                 * <noscript>...</noscript>: when scripting_enabled is true, parse inner
                 * markup as rawtext/ignored so fallback DOM/styles never execute or match!
                 */
                if (p->scripting_enabled && p->svg_depth == 0u && p->math_depth == 0u &&
                    arcr_str_eq_nocase_n(tag_ptr, tag_len, "noscript", 8u)) {
                    while (j < len && utf8_html[j] != '>') ++j;
                    bool self_close_ns = (j > i + 1u && j < len && utf8_html[j - 1u] == '/');
                    size_t after_open  = (j < len) ? (j + 1u) : len;
                    if (!self_close_ns) {
                        size_t ns_end = find_raw_end_tag(utf8_html, len, after_open, "noscript", 8u);
                        size_t gt     = ns_end;
                        while (gt < len && utf8_html[gt] != '>') ++gt;
                        p->pos = (gt < len) ? (gt + 1u) : len;
                    } else {
                        p->pos = after_open;
                    }
                    continue;
                }

                ArcrAtom tag_atom = arcr_atom_intern_lower(doc->atoms, tag_ptr, tag_len);
                bool in_foreign   = (p->svg_depth > 0u || p->math_depth > 0u);
                bool in_template  = (p->template_depth > 0u);

                if (!in_foreign) {
                    auto_close_implicit_tags(p->stack, &p->depth, &p->pa,
                                             tag_ptr, tag_len, tag_atom);
                }

                if (!p->is_fragment && !in_foreign && !in_template) {
                    if (!p->html_el && tag_atom != p->pa.html) {
                        p->html_el = arcr_dom_create_element(doc, "html", 4u);
                        arcr_dom_append_child(doc->root, p->html_el);
                        (void)push_open_element(&p->stack, &p->stack_cap, &p->depth, p->html_el);
                    }
                    if (!p->body_el && tag_atom != p->pa.html && tag_atom != p->pa.head &&
                        tag_atom != p->pa.template_tag &&
                        !arcr_str_eq_nocase_n(tag_ptr, tag_len, "meta", 4u) &&
                        !arcr_str_eq_nocase_n(tag_ptr, tag_len, "title", 5u) &&
                        !arcr_str_eq_nocase_n(tag_ptr, tag_len, "style", 5u) &&
                        !arcr_str_eq_nocase_n(tag_ptr, tag_len, "link", 4u) &&
                        !arcr_str_eq_nocase_n(tag_ptr, tag_len, "base", 4u) &&
                        !arcr_str_eq_nocase_n(tag_ptr, tag_len, "noscript", 8u) &&
                        !(p->depth > 1u && p->stack[p->depth - 1u]->tag == p->pa.head &&
                          tag_atom == p->pa.script)) {
                        if (p->depth > 1u && p->stack[p->depth - 1u]->tag == p->pa.head) {
                            --p->depth;
                        }
                        if (tag_atom != p->pa.body) {
                            p->body_el = arcr_dom_create_element(doc, "body", 4u);
                            doc->body  = p->body_el;
                            arcr_dom_append_child(p->html_el, p->body_el);
                            (void)push_open_element(&p->stack, &p->stack_cap, &p->depth, p->body_el);
                        }
                    }
                }

                ArcrDomNode *elem = NULL;
                if (!p->is_fragment && !in_foreign && !in_template &&
                    tag_atom == p->pa.html && p->html_el) {
                    elem = p->html_el;
                } else if (!p->is_fragment && !in_foreign && !in_template &&
                           tag_atom == p->pa.body && p->body_el) {
                    elem = p->body_el;
                } else {
                    elem = arcr_dom_create_element(doc, tag_ptr, tag_len);
                    insert_node_foster_aware(p, p->stack[p->depth - 1u], elem);
                    if (!p->is_fragment && !in_foreign && !in_template && tag_atom == p->pa.html) {
                        p->html_el = elem;
                    }
                    if (!p->is_fragment && !in_foreign && !in_template && tag_atom == p->pa.body) {
                        p->body_el = elem;
                        doc->body  = elem;
                    }
                }

                bool self_closing = false;
                while (j < len) {
                    while (j < len && is_space(utf8_html[j])) ++j;
                    if (j >= len) break;
                    if (utf8_html[j] == '>') {
                        ++j;
                        break;
                    }
                    if (utf8_html[j] == '/' && j + 1u < len && utf8_html[j + 1u] == '>') {
                        self_closing = true;
                        j += 2u;
                        break;
                    }
                    if (utf8_html[j] == '/') {
                        ++j;
                        continue;
                    }

                    size_t astart = j;
                    while (j < len && !is_space(utf8_html[j]) &&
                           utf8_html[j] != '=' && utf8_html[j] != '>' && utf8_html[j] != '/') {
                        ++j;
                    }
                    size_t aend = j;
                    while (j < len && is_space(utf8_html[j])) ++j;

                    const char *vptr = "";
                    size_t      vlen = 0u;
                    if (j < len && utf8_html[j] == '=') {
                        ++j;
                        while (j < len && is_space(utf8_html[j])) ++j;
                        if (j < len && (utf8_html[j] == '"' || utf8_html[j] == '\'')) {
                            char   q      = utf8_html[j++];
                            size_t vstart = j;
                            while (j < len && utf8_html[j] != q) ++j;
                            vptr = utf8_html + vstart;
                            vlen = j - vstart;
                            if (j < len) ++j;
                        } else {
                            size_t vstart = j;
                            while (j < len && !is_space(utf8_html[j]) &&
                                   utf8_html[j] != '>' &&
                                   !(utf8_html[j] == '/' && j + 1u < len && utf8_html[j + 1u] == '>')) {
                                ++j;
                            }
                            vptr = utf8_html + vstart;
                            vlen = j - vstart;
                        }
                    }

                    if (aend > astart && elem) {
                        bool has_amp = false;
                        for (size_t k = 0u; k < vlen; ++k) {
                            if (vptr[k] == '&') { has_amp = true; break; }
                        }
                        if (has_amp) {
                            size_t dlen = decode_and_collapse_text(vptr, vlen, NULL, false);
                            char  *dbuf = (char *)arcr_arena_alloc(&doc->arena, dlen + 1u, 1u);
                            if (dbuf) {
                                decode_and_collapse_text(vptr, vlen, dbuf, false);
                                arcr_dom_set_attr(doc, elem, utf8_html + astart, aend - astart,
                                                  dbuf, dlen);
                            }
                        } else {
                            arcr_dom_set_attr(doc, elem, utf8_html + astart, aend - astart,
                                              vptr, vlen);
                        }
                    } else if (j == astart) {
                        ++j;
                    }
                }
                p->pos = j;

                /* Note: in HTML (outside SVG/MathML), `/>` on non-void tags is NOT self-closing! */
                bool effective_self_close = self_closing &&
                                            (in_foreign || tag_atom == p->pa.svg ||
                                             tag_atom == p->pa.math ||
                                             is_void_element(tag_ptr, tag_len));

                finalize_opened_element(doc, elem, tag_ptr, tag_len, p->svg_depth, in_template);

                /* Optional stylesheet barrier for <link rel="stylesheet" href="..."> */
                if (p->pauses_enabled && !in_foreign && !in_template && elem &&
                    arcr_str_eq_nocase_n(tag_ptr, tag_len, "link", 4u)) {
                    const char *rel  = arcr_dom_get_attr_cstr(doc, elem, "rel");
                    const char *href = arcr_dom_get_attr_cstr(doc, elem, "href");
                    if (rel_contains_stylesheet(rel) && href && href[0] != '\0') {
                        p->write_cursor = p->pos;
                        if (out_node) *out_node = elem;
                        return ARCR_HTML_PARSE_PAUSE_STYLE;
                    }
                }

                /* Raw/RCDATA text elements: <style>, <script>, <title>, <textarea> */
                bool is_style    = (tag_atom == p->pa.style);
                bool is_script   = (tag_atom == p->pa.script);
                bool is_title    = arcr_str_eq_nocase_n(tag_ptr, tag_len, "title", 5u);
                bool is_textarea = !in_foreign &&
                                   arcr_str_eq_nocase_n(tag_ptr, tag_len, "textarea", 8u);

                if (!effective_self_close && (is_style || is_script || is_title || is_textarea)) {
                    size_t raw_start = p->pos;
                    size_t raw_end   = is_script
                                           ? find_script_end_tag(utf8_html, len, raw_start)
                                           : find_raw_end_tag(utf8_html, len, raw_start, tag_ptr, tag_len);
                    const char *raw_ptr = utf8_html + raw_start;
                    size_t      raw_len = raw_end - raw_start;

                    if (is_style && raw_len > 0u) {
                        if (!in_foreign && !in_template) {
                            arcr_dom_append_style_block(doc, raw_ptr, raw_len);
                        }
                        if (elem) {
                            elem->text     = arcr_arena_strdup(&doc->arena, raw_ptr, raw_len);
                            elem->text_len = raw_len;
                        }
                    } else if (is_title && raw_len > 0u) {
                        size_t dlen = decode_and_collapse_text(raw_ptr, raw_len, NULL, true);
                        char  *tbuf = (char *)arcr_arena_alloc(&doc->arena, dlen + 1u, 1u);
                        if (tbuf) {
                            decode_and_collapse_text(raw_ptr, raw_len, tbuf, true);
                            const char *tv = tbuf;
                            size_t      tl = dlen;
                            arcr_str_trim_view(&tv, &tl);
                            if ((!doc->title || doc->title[0] == '\0') &&
                                !in_foreign && !in_template && !p->is_fragment) {
                                doc->title = arcr_arena_strdup(&doc->arena, tv, tl);
                            }
                            if (elem) {
                                elem->text     = arcr_arena_strdup(&doc->arena, tv, tl);
                                elem->text_len = tl;
                            }
                        }
                    } else if (is_script && elem && raw_len > 0u) {
                        elem->text     = arcr_arena_strdup(&doc->arena, raw_ptr, raw_len);
                        elem->text_len = raw_len;
                    } else if (is_textarea && elem) {
                        size_t dlen = decode_and_collapse_text(raw_ptr, raw_len, NULL, false);
                        char  *tbuf = (char *)arcr_arena_alloc(&doc->arena, dlen + 1u, 1u);
                        if (tbuf) {
                            decode_and_collapse_text(raw_ptr, raw_len, tbuf, false);
                            arcr_dom_set_input_value(doc, elem, tbuf);
                            if (dlen > 0u) {
                                ArcrDomNode *tn = arcr_dom_create_text(doc, tbuf, dlen);
                                if (tn) arcr_dom_append_child(elem, tn);
                            }
                        } else {
                            arcr_dom_set_input_value(doc, elem, "");
                        }
                    }

                    size_t close_gt = raw_end;
                    while (close_gt < len && utf8_html[close_gt] != '>') ++close_gt;
                    p->pos = (close_gt < len) ? (close_gt + 1u) : len;

                    if (is_script && p->pauses_enabled && p->scripting_enabled &&
                        !in_foreign && !in_template && elem) {
                        p->write_cursor = p->pos;
                        if (out_node) *out_node = elem;
                        return ARCR_HTML_PARSE_PAUSE_SCRIPT;
                    }
                    continue;
                }

                if (tag_atom == p->pa.svg && effective_self_close && p->svg_depth == 0u && elem) {
                    size_t raw_svg_len   = p->pos - tag_lt_pos;
                    elem->raw_svg_markup = arcr_arena_strdup(&doc->arena,
                                                             utf8_html + tag_lt_pos,
                                                             raw_svg_len);
                    elem->raw_svg_len    = raw_svg_len;
                    if (!in_template) {
                        rasterize_inline_svg_element(doc, elem);
                    }
                    continue;
                }

                /* <template>: push <template> AND its inert template_content fragment */
                if (!in_foreign && tag_atom == p->pa.template_tag && elem && elem->template_content) {
                    if (push_open_element(&p->stack, &p->stack_cap, &p->depth, elem)) {
                        if (push_open_element(&p->stack, &p->stack_cap, &p->depth,
                                              elem->template_content)) {
                            ++p->template_depth;
                        } else {
                            --p->depth;
                        }
                    }
                    continue;
                }

                bool is_void = !in_foreign && is_void_element(tag_ptr, tag_len);
                if (!effective_self_close && !is_void && elem && p->stack[p->depth - 1u] != elem) {
                    if (push_open_element(&p->stack, &p->stack_cap, &p->depth, elem)) {
                        if (tag_atom == p->pa.svg) {
                            if (p->svg_depth == 0u) {
                                p->svg_open_node   = elem;
                                p->svg_open_offset = tag_lt_pos;
                            }
                            ++p->svg_depth;
                        } else if (tag_atom == p->pa.math) {
                            ++p->math_depth;
                        }
                    }
                }
                continue;
            }
        }

        /* 5. Character data / Text node */
        size_t tstart = i;
        if (utf8_html[i] == '<') {
            ++i;
        }
        while (i < len) {
            if (utf8_html[i] == '<' && i + 1u < len) {
                char c1 = utf8_html[i + 1u];
                if (is_ascii_alpha(c1) || c1 == '!' || c1 == '?' ||
                    (c1 == '/' && i + 2u < len)) {
                    break;
                }
            }
            ++i;
        }
        size_t raw_len = i - tstart;
        if (raw_len == 0u) {
            p->pos = i + 1u;
            continue;
        }
        p->pos = i;

        bool all_ws = true;
        for (size_t k = 0u; k < raw_len; ++k) {
            if (!is_space(utf8_html[tstart + k])) {
                all_ws = false;
                break;
            }
        }

        if (!all_ws && !p->is_fragment && !p->seen_doctype) {
            doc->quirks_mode = ARCR_QUIRKS_FULL_QUIRKS;
            p->seen_doctype  = true;
        }

        ArcrDomNode *cur_parent = p->stack[p->depth - 1u];
        bool in_pre = false;
        for (size_t k = p->depth; k > 0u; --k) {
            if (p->stack[k - 1u]->kind == ARCR_NODE_DOCUMENT) break;
            if (p->stack[k - 1u]->tag == p->pa.pre) {
                in_pre = true;
                break;
            }
        }
        for (const ArcrDomNode *anc = cur_parent; anc != NULL && !in_pre; anc = anc->parent) {
            if (anc->tag == p->pa.pre) {
                in_pre = true;
                break;
            }
        }

        if (all_ws && !in_pre) {
            if (p->svg_depth > 0u || p->math_depth > 0u || !cur_parent->first_child ||
                (!p->is_fragment && p->template_depth == 0u &&
                 (cur_parent == doc->root || cur_parent == p->html_el ||
                  cur_parent->tag == p->pa.head)) ||
                cur_parent->tag == p->pa.table || cur_parent->tag == p->pa.thead ||
                cur_parent->tag == p->pa.tbody || cur_parent->tag == p->pa.tfoot ||
                cur_parent->tag == p->pa.tr || cur_parent->tag == p->pa.ul ||
                cur_parent->tag == p->pa.ol) {
                continue;
            }
        }

        if (!p->is_fragment && p->svg_depth == 0u && p->math_depth == 0u &&
            p->template_depth == 0u && !p->body_el) {
            if (!p->html_el) {
                p->html_el = arcr_dom_create_element(doc, "html", 4u);
                arcr_dom_append_child(doc->root, p->html_el);
                (void)push_open_element(&p->stack, &p->stack_cap, &p->depth, p->html_el);
            }
            if (p->depth > 1u && p->stack[p->depth - 1u]->tag == p->pa.head) {
                --p->depth;
            }
            p->body_el = arcr_dom_create_element(doc, "body", 4u);
            doc->body  = p->body_el;
            arcr_dom_append_child(p->html_el, p->body_el);
            (void)push_open_element(&p->stack, &p->stack_cap, &p->depth, p->body_el);
            cur_parent = p->body_el;
        }

        size_t decoded_len = decode_and_collapse_text(utf8_html + tstart, raw_len, NULL, !in_pre);
        if (decoded_len > 0u) {
            char *tbuf = (char *)arcr_arena_alloc(&doc->arena, decoded_len + 1u, 1u);
            if (tbuf) {
                decode_and_collapse_text(utf8_html + tstart, raw_len, tbuf, !in_pre);
                ArcrDomNode *tn = arcr_dom_create_text(doc, tbuf, decoded_len);
                if (tn) {
                    insert_node_foster_aware(p, cur_parent, tn);
                }
            }
        }
    }

    finalize_parser_eof(p);
    return ARCR_HTML_PARSE_DONE;
}

ArcrDocument *arcr_html_parser_finish(ArcrHtmlParser *parser) {
    if (!parser) return NULL;
    if (!parser->done) {
        finalize_parser_eof(parser);
    }
    ArcrDocument *doc = parser->doc;
    if (doc && !parser->is_fragment) {
        doc->dom_dirty = false;
    }
    free(parser->stack);
    free(parser->buf);
    free(parser);
    return doc;
}

ArcrDocument *arcr_html_parse(ArcrAtomTable *atoms, const char *utf8_html, size_t len) {
    ArcrDocument *doc = arcr_dom_document_create(atoms);
    if (!doc) return NULL;
    ArcrHtmlParser *p = create_parser_internal(doc, doc->root,
                                               utf8_html ? utf8_html : "",
                                               utf8_html ? len : 0u,
                                               true, false, false);
    if (!p) {
        arcr_dom_document_destroy(doc);
        return NULL;
    }
    (void)arcr_html_parser_resume(p, NULL);
    return arcr_html_parser_finish(p);
}

bool arcr_html_parse_fragment(ArcrDocument *doc, ArcrDomNode *target_parent,
                              const char *utf8_html, size_t len) {
    if (!doc || !target_parent) return false;
    if (!utf8_html || len == 0u) return true;

    ArcrDomNode *dest = target_parent;
    if (target_parent->kind == ARCR_NODE_ELEMENT && target_parent->template_content &&
        arcr_str_eq_nocase(arcr_atom_cstr(doc->atoms, target_parent->tag), "template")) {
        dest = target_parent->template_content;
    }

    ArcrHtmlParser *p = create_parser_internal(doc, dest, utf8_html, len, true, false, true);
    if (!p) return false;
    if (dest != target_parent) {
        p->template_depth = 1u;
    }
    (void)arcr_html_parser_resume(p, NULL);
    (void)arcr_html_parser_finish(p);
    doc->dom_dirty = true;
    return true;
}
