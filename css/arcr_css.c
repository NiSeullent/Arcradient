/*
 * Arcradient (MSHTML-Next)
 * css/arcr_css.c - CSS Rule Indexing, Ancestor Bloom Rejection, var() & Style Sharing
 */
#include "arcr_css.h"
#include <stdlib.h>
#include <string.h>

typedef struct ArcrSimpleSelector {
    ArcrAtom tag;        /* 0 = any */
    ArcrAtom id;         /* 0 = any */
    ArcrAtom class_name; /* 0 = any */
} ArcrSimpleSelector;

typedef struct ArcrCssDecl {
    ArcrAtom            prop;
    const char         *val_str;
    size_t              val_len;
    struct ArcrCssDecl *next;
} ArcrCssDecl;

typedef struct ArcrCssRule {
    bool               has_ancestor;
    ArcrSimpleSelector ancestor;
    uint64_t           ancestor_bloom_req;
    ArcrSimpleSelector key;
    uint32_t           specificity;
    uint32_t           source_order;
    ArcrCssDecl       *decls;
    struct ArcrCssRule *next;
} ArcrCssRule;

typedef struct ArcrCssVar {
    ArcrAtom           name; /* e.g., "--accent" */
    const char        *val;
    size_t             val_len;
    struct ArcrCssVar *next;
} ArcrCssVar;

struct ArcrStylesheet {
    ArcrCssRule *rules_head;
    ArcrCssRule *rules_tail;
    ArcrCssVar  *vars_head;
    uint32_t     rule_count;
};

/* Style Sharing Cache Entry */
typedef struct ArcrStyleShareSlot {
    const ArcrComputedStyle *parent_style;
    ArcrAtom                 tag;
    uint32_t                 class_count;
    ArcrAtom                 classes[ARCR_MAX_INLINE_CLASSES];
    const ArcrComputedStyle *shared_style;
} ArcrStyleShareSlot;

#define ARCR_STYLE_SHARE_RING 32u

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool arcr_css_parse_color(const char *s, size_t len, uint32_t *out_argb) {
    if (!s || !out_argb) return false;
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;

    if (s[0] == '#') {
        if (len == 4u) {
            int r = hex_nibble(s[1]), g = hex_nibble(s[2]), b = hex_nibble(s[3]);
            if (r < 0 || g < 0 || b < 0) return false;
            *out_argb = 0xFF000000u | ((uint32_t)(r * 17) << 16) |
                        ((uint32_t)(g * 17) << 8) | (uint32_t)(b * 17);
            return true;
        }
        if (len == 7u || len == 9u) {
            int r = (hex_nibble(s[1]) << 4) | hex_nibble(s[2]);
            int g = (hex_nibble(s[3]) << 4) | hex_nibble(s[4]);
            int b = (hex_nibble(s[5]) << 4) | hex_nibble(s[6]);
            int a = 255;
            if (len == 9u) a = (hex_nibble(s[7]) << 4) | hex_nibble(s[8]);
            if (r < 0 || g < 0 || b < 0 || a < 0) return false;
            *out_argb = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                        ((uint32_t)g << 8) | (uint32_t)b;
            return true;
        }
        return false;
    }

    if (len > 5u && arcr_str_eq_nocase_n(s, 4u, "rgb(", 4u) && s[len - 1u] == ')') {
        const char *p = s + 4u;
        size_t rem = len - 5u;
        int32_t vals[3] = {0, 0, 0};
        for (int idx = 0; idx < 3; ++idx) {
            size_t comma = 0u;
            while (comma < rem && p[comma] != ',') ++comma;
            if (!arcr_parse_i32(p, comma, &vals[idx])) return false;
            vals[idx] = ARCR_CLAMP(vals[idx], 0, 255);
            if (idx < 2) {
                if (comma >= rem) return false;
                p   += comma + 1u;
                rem -= comma + 1u;
            }
        }
        *out_argb = 0xFF000000u | ((uint32_t)vals[0] << 16) |
                    ((uint32_t)vals[1] << 8) | (uint32_t)vals[2];
        return true;
    }

    if (len > 6u && arcr_str_eq_nocase_n(s, 5u, "rgba(", 5u) && s[len - 1u] == ')') {
        const char *p = s + 5u;
        size_t rem = len - 6u;
        int32_t vals[4] = {0, 0, 0, 255};
        for (int idx = 0; idx < 4; ++idx) {
            size_t comma = 0u;
            while (comma < rem && p[comma] != ',') ++comma;
            if (idx < 3) {
                if (!arcr_parse_i32(p, comma, &vals[idx])) return false;
                vals[idx] = ARCR_CLAMP(vals[idx], 0, 255);
                if (comma >= rem) return false;
                p   += comma + 1u;
                rem -= comma + 1u;
            } else {
                const char *ap = p;
                size_t al = rem;
                arcr_str_trim_view(&ap, &al);
                if (al >= 3u && ap[0] == '0' && ap[1] == '.') {
                    int d1 = (ap[2] >= '0' && ap[2] <= '9') ? (ap[2] - '0') : 0;
                    int d2 = (al >= 4u && ap[3] >= '0' && ap[3] <= '9') ? (ap[3] - '0') : 0;
                    vals[3] = ((d1 * 10 + d2) * 255) / 100;
                } else if (al >= 1u && ap[0] == '1') {
                    vals[3] = 255;
                } else {
                    vals[3] = 0;
                }
            }
        }
        *out_argb = ((uint32_t)vals[3] << 24) | ((uint32_t)vals[0] << 16) |
                    ((uint32_t)vals[1] << 8) | (uint32_t)vals[2];
        return true;
    }

    struct { const char *name; uint32_t argb; } kNamed[] = {
        {"transparent", 0x00000000u}, {"black",   0xFF000000u},
        {"white",       0xFFFFFFFFu}, {"red",     0xFFDC2626u},
        {"blue",        0xFF2563EBu}, {"green",   0xFF16A34Au},
        {"gray",        0xFF6B7280u}, {"grey",    0xFF6B7280u},
        {"navy",        0xFF0A246Au}, {"silver",  0xFFD4D0C8u},
        {"yellow",      0xFFEAB308u}
    };
    for (size_t i = 0; i < ARCR_ARRAY_LEN(kNamed); ++i) {
        if (arcr_str_eq_nocase_n(s, len, kNamed[i].name, strlen(kNamed[i].name))) {
            *out_argb = kNamed[i].argb;
            return true;
        }
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

static void parse_simple_selector(ArcrAtomTable *atoms, const char *s, size_t len,
                                  ArcrSimpleSelector *out_sel, uint32_t *inout_spec) {
    memset(out_sel, 0, sizeof(*out_sel));
    arcr_str_trim_view(&s, &len);
    if (len == 0u || (len == 1u && s[0] == '*')) return;

    size_t i = 0u;
    if (s[0] != '.' && s[0] != '#' && s[0] != ':') {
        size_t start = 0u;
        while (i < len && s[i] != '.' && s[i] != '#' && s[i] != ':') ++i;
        if (i > start) {
            out_sel->tag = arcr_atom_intern_lower(atoms, s + start, i - start);
            *inout_spec += 1u;
        }
    }
    while (i < len) {
        char kind = s[i++];
        size_t start = i;
        while (i < len && s[i] != '.' && s[i] != '#' && s[i] != ':') ++i;
        if (i > start) {
            if (kind == '#') {
                out_sel->id = arcr_atom_intern(atoms, s + start, i - start);
                *inout_spec += 100u;
            } else if (kind == '.') {
                out_sel->class_name = arcr_atom_intern(atoms, s + start, i - start);
                *inout_spec += 10u;
            }
        }
    }
}

static ArcrCssDecl *parse_declarations(ArcrDocument *doc, ArcrStylesheet *sheet,
                                       const char *block, size_t len, bool is_root_scope) {
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

        /* Strip trailing !important for phase-1 cascade simplicity */
        if (vlen > 10u && arcr_str_eq_nocase_n(pval + vlen - 10u, 10u, "!important", 10u)) {
            vlen -= 10u;
            arcr_str_trim_view(&pval, &vlen);
        }

        /* CSS Custom Property (--var-name: value) */
        if (plen > 2u && pname[0] == '-' && pname[1] == '-') {
            if (sheet && is_root_scope) {
                ArcrCssVar *v = (ArcrCssVar *)arcr_arena_alloc(
                    &doc->arena, sizeof(ArcrCssVar), sizeof(void *));
                if (v) {
                    v->name    = arcr_atom_intern(doc->atoms, pname, plen);
                    v->val     = arcr_arena_strdup(&doc->arena, pval, vlen);
                    v->val_len = vlen;
                    v->next    = sheet->vars_head;
                    sheet->vars_head = v;
                }
            }
            continue;
        }

        ArcrCssDecl *d = (ArcrCssDecl *)arcr_arena_alloc(
            &doc->arena, sizeof(ArcrCssDecl), sizeof(void *));
        if (!d) continue;
        d->prop    = arcr_atom_intern_lower(doc->atoms, pname, plen);
        d->val_str = arcr_arena_strdup(&doc->arena, pval, vlen);
        d->val_len = vlen;
        d->next    = NULL;
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

ArcrStylesheet *arcr_css_parse_document_styles(ArcrDocument *doc) {
    if (!doc) return NULL;
    ArcrStylesheet *sheet = (ArcrStylesheet *)arcr_arena_calloc(
        &doc->arena, 1u, sizeof(ArcrStylesheet), sizeof(void *));
    if (!sheet) return NULL;

    for (ArcrStyleBlock *sb = doc->style_blocks_head; sb != NULL; sb = sb->next) {
        char *clean = (char *)arcr_arena_alloc(&doc->arena, sb->css_len + 1u, 1u);
        if (!clean) continue;
        size_t clen = strip_css_comments(sb->css_text, sb->css_len, clean);

        size_t i = 0u;
        while (i < clen) {
            size_t brace = i;
            while (brace < clen && clean[brace] != '{') ++brace;
            if (brace >= clen) break;

            const char *sel_group = clean + i;
            size_t      sel_glen  = brace - i;

            size_t close_brace = brace + 1u;
            int    depth = 1;
            while (close_brace < clen && depth > 0) {
                if (clean[close_brace] == '{') ++depth;
                else if (clean[close_brace] == '}') --depth;
                if (depth > 0) ++close_brace;
            }
            const char *body_ptr = clean + brace + 1u;
            size_t      body_len = (close_brace > brace + 1u) ? (close_brace - brace - 1u) : 0u;
            i = (close_brace < clen) ? (close_brace + 1u) : clen;

            arcr_str_trim_view(&sel_group, &sel_glen);
            /* Skip @media / @keyframes blocks in phase-1 base parser without crashing */
            if (sel_glen > 0u && sel_group[0] == '@') continue;

            bool is_root = arcr_str_eq_nocase_n(sel_group, sel_glen, ":root", 5u) ||
                           arcr_str_eq_nocase_n(sel_group, sel_glen, "html", 4u) ||
                           arcr_str_eq_nocase_n(sel_group, sel_glen, "body", 4u);
            ArcrCssDecl *decls = parse_declarations(doc, sheet, body_ptr, body_len, is_root);
            if (!decls) continue;

            /* Split comma-separated selector list: e.g. "h1, h2, .title" */
            size_t p = 0u;
            while (p < sel_glen) {
                size_t comma = p;
                while (comma < sel_glen && sel_group[comma] != ',') ++comma;
                const char *one = sel_group + p;
                size_t      olen = comma - p;
                p = (comma < sel_glen) ? (comma + 1u) : sel_glen;
                arcr_str_trim_view(&one, &olen);
                if (olen == 0u || arcr_str_eq_nocase_n(one, olen, ":root", 5u)) continue;

                ArcrCssRule *rule = (ArcrCssRule *)arcr_arena_calloc(
                    &doc->arena, 1u, sizeof(ArcrCssRule), sizeof(void *));
                if (!rule) continue;

                /* Check for single space descendant combinator: "ancestor key" */
                size_t last_space = olen;
                for (size_t k = olen; k > 0u; --k) {
                    if (one[k - 1u] == ' ' || one[k - 1u] == '\t') {
                        last_space = k - 1u;
                        break;
                    }
                }
                uint32_t spec = 0u;
                if (last_space < olen) {
                    const char *anc_s = one;
                    size_t      anc_l = last_space;
                    /* Take immediate first token of ancestor for phase-1 fast matching */
                    arcr_str_trim_view(&anc_s, &anc_l);
                    size_t sp2 = 0u;
                    while (sp2 < anc_l && anc_s[sp2] != ' ' && anc_s[sp2] != '>') ++sp2;
                    parse_simple_selector(doc->atoms, anc_s, sp2, &rule->ancestor, &spec);
                    rule->has_ancestor = true;
                    rule->ancestor_bloom_req =
                        arcr_bloom_bits_for_atom(rule->ancestor.tag) |
                        arcr_bloom_bits_for_atom(rule->ancestor.id) |
                        arcr_bloom_bits_for_atom(rule->ancestor.class_name);

                    parse_simple_selector(doc->atoms, one + last_space + 1u,
                                          olen - last_space - 1u, &rule->key, &spec);
                } else {
                    parse_simple_selector(doc->atoms, one, olen, &rule->key, &spec);
                }

                rule->specificity  = spec;
                rule->source_order = ++sheet->rule_count;
                rule->decls        = decls;
                if (!sheet->rules_head) {
                    sheet->rules_head = rule;
                    sheet->rules_tail = rule;
                } else {
                    sheet->rules_tail->next = rule;
                    sheet->rules_tail       = rule;
                }
            }
        }
    }
    return sheet;
}

static bool simple_selector_matches(const ArcrSimpleSelector *sel, const ArcrDomNode *el) {
    if (!el || el->kind != ARCR_NODE_ELEMENT) return false;
    if (sel->tag != ARCR_ATOM_NULL && el->tag != sel->tag) return false;
    if (sel->id != ARCR_ATOM_NULL && el->id != sel->id) return false;
    if (sel->class_name != ARCR_ATOM_NULL && !arcr_dom_has_class(el, sel->class_name)) {
        return false;
    }
    return true;
}

static bool rule_matches_element(const ArcrCssRule *rule, const ArcrDomNode *el) {
    if (!simple_selector_matches(&rule->key, el)) return false;
    if (!rule->has_ancestor) return true;

    /* Fast O(1) ancestor Bloom filter rejection! */
    if ((el->ancestor_bloom & rule->ancestor_bloom_req) != rule->ancestor_bloom_req) {
        return false;
    }
    for (const ArcrDomNode *anc = el->parent; anc != NULL; anc = anc->parent) {
        if (simple_selector_matches(&rule->ancestor, anc)) return true;
    }
    return false;
}

static const char *resolve_css_var(const ArcrDocument *doc, const ArcrStylesheet *sheet,
                                   const char *val, size_t *inout_len, bool *used_var) {
    size_t len = *inout_len;
    if (len > 6u && arcr_str_eq_nocase_n(val, 4u, "var(", 4u) && val[len - 1u] == ')') {
        *used_var = true;
        const char *inner = val + 4u;
        size_t      ilen  = len - 5u;
        size_t      comma = 0u;
        while (comma < ilen && inner[comma] != ',') ++comma;

        const char *vname = inner;
        size_t      vnlen = comma;
        arcr_str_trim_view(&vname, &vnlen);
        ArcrAtom name_atom = arcr_atom_intern(doc->atoms, vname, vnlen);
        if (sheet) {
            for (const ArcrCssVar *v = sheet->vars_head; v != NULL; v = v->next) {
                if (v->name == name_atom) {
                    *inout_len = v->val_len;
                    return v->val;
                }
            }
        }
        /* Fallback value in var(--missing, fallback) */
        if (comma < ilen) {
            const char *fb = inner + comma + 1u;
            size_t      fbl = ilen - comma - 1u;
            arcr_str_trim_view(&fb, &fbl);
            *inout_len = fbl;
            return fb;
        }
    }
    return val;
}

static int32_t parse_length_px(const char *s, size_t len, int32_t base_font_px, uint32_t *deps) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return 0;
    if (len > 2u && arcr_str_eq_nocase_n(s + len - 2u, 2u, "px", 2u)) {
        int32_t v = 0;
        arcr_parse_i32(s, len - 2u, &v);
        return v;
    }
    if (len > 3u && arcr_str_eq_nocase_n(s + len - 3u, 3u, "rem", 3u)) {
        int32_t v = 1;
        arcr_parse_i32(s, len - 3u, &v);
        return v * 16;
    }
    if (len > 2u && arcr_str_eq_nocase_n(s + len - 2u, 2u, "em", 2u)) {
        if (deps) *deps |= ARCR_STYLE_DEP_EM;
        /* Support simple decimal like 1.5em or integer 2em */
        int32_t whole = 0;
        size_t dot = 0u;
        while (dot < len - 2u && s[dot] != '.') ++dot;
        arcr_parse_i32(s, dot, &whole);
        int32_t tenth = 0;
        if (dot + 1u < len - 2u && s[dot + 1u] >= '0' && s[dot + 1u] <= '9') {
            tenth = s[dot + 1u] - '0';
        }
        return (whole * base_font_px) + ((tenth * base_font_px) / 10);
    }
    int32_t raw = 0;
    arcr_parse_i32(s, len, &raw);
    return raw;
}

static void parse_box_shorthand(const char *s, size_t len, int32_t font_px,
                                ArcrEdgeSizes *edges, uint32_t *deps) {
    int32_t vals[4] = {0, 0, 0, 0};
    int     cnt     = 0;
    size_t  i       = 0u;
    while (i < len && cnt < 4) {
        while (i < len && (s[i] == ' ' || s[i] == '\t')) ++i;
        size_t start = i;
        while (i < len && s[i] != ' ' && s[i] != '\t') ++i;
        if (i > start) {
            vals[cnt++] = parse_length_px(s + start, i - start, font_px, deps);
        }
    }
    if (cnt == 1) {
        edges->top = edges->right = edges->bottom = edges->left = vals[0];
    } else if (cnt == 2) {
        edges->top = edges->bottom = vals[0];
        edges->right = edges->left = vals[1];
    } else if (cnt == 3) {
        edges->top = vals[0];
        edges->right = edges->left = vals[1];
        edges->bottom = vals[2];
    } else if (cnt == 4) {
        edges->top = vals[0];
        edges->right = vals[1];
        edges->bottom = vals[2];
        edges->left = vals[3];
    }
}

static void apply_ua_defaults(const ArcrAtomTable *atoms, const ArcrDomNode *el,
                              const ArcrComputedStyle *parent, ArcrComputedStyle *st) {
    memset(st, 0, sizeof(*st));
    st->display             = ARCR_DISPLAY_BLOCK;
    st->color_argb          = parent ? parent->color_argb : 0xFF111827u;
    st->bg_argb             = 0x00000000u;
    st->border_argb         = 0xFFD1D5DBu;
    st->font_px             = parent ? parent->font_px : 15;
    st->font_bold           = parent ? parent->font_bold : false;
    st->word_break_keep_all = parent ? parent->word_break_keep_all : false;

    const char *t = arcr_atom_cstr(atoms, el->tag);
    if (arcr_str_eq_nocase(t, "head") || arcr_str_eq_nocase(t, "style") ||
        arcr_str_eq_nocase(t, "script") || arcr_str_eq_nocase(t, "meta") ||
        arcr_str_eq_nocase(t, "title") || arcr_str_eq_nocase(t, "link")) {
        st->display = ARCR_DISPLAY_NONE;
    } else if (arcr_str_eq_nocase(t, "body")) {
        st->bg_argb = 0xFFFFFFFFu;
        st->padding = (ArcrEdgeSizes){16, 20, 16, 20};
    } else if (arcr_str_eq_nocase(t, "h1")) {
        st->font_px   = 24;
        st->font_bold = true;
        st->margin.bottom = 10;
    } else if (arcr_str_eq_nocase(t, "h2")) {
        st->font_px   = 19;
        st->font_bold = true;
        st->margin.top    = 12;
        st->margin.bottom = 8;
    } else if (arcr_str_eq_nocase(t, "h3")) {
        st->font_px   = 16;
        st->font_bold = true;
        st->margin.top    = 8;
        st->margin.bottom = 6;
    } else if (arcr_str_eq_nocase(t, "p") || arcr_str_eq_nocase(t, "ul")) {
        st->margin.bottom = 10;
    } else if (arcr_str_eq_nocase(t, "li")) {
        st->margin.bottom = 5;
        st->padding.left  = 14;
    } else if (arcr_str_eq_nocase(t, "span") || arcr_str_eq_nocase(t, "em") ||
               arcr_str_eq_nocase(t, "i") || arcr_str_eq_nocase(t, "small")) {
        st->display = ARCR_DISPLAY_INLINE;
    } else if (arcr_str_eq_nocase(t, "strong") || arcr_str_eq_nocase(t, "b")) {
        st->display   = ARCR_DISPLAY_INLINE;
        st->font_bold = true;
    } else if (arcr_str_eq_nocase(t, "a")) {
        st->display    = ARCR_DISPLAY_INLINE;
        st->color_argb = 0xFF1D4ED8u;
    } else if (arcr_str_eq_nocase(t, "code")) {
        st->display       = ARCR_DISPLAY_INLINE;
        st->bg_argb       = 0xFFF1F5F9u;
        st->color_argb    = 0xFF0F172Au;
        st->padding       = (ArcrEdgeSizes){1, 5, 1, 5};
        st->border_radius = 4;
    }
}

static void apply_declarations(const ArcrDocument *doc, const ArcrStylesheet *sheet,
                               const ArcrCssDecl *decls, ArcrComputedStyle *st) {
    for (const ArcrCssDecl *d = decls; d != NULL; d = d->next) {
        const char *prop = arcr_atom_cstr(doc->atoms, d->prop);
        size_t      vlen = d->val_len;
        bool        used_var = false;
        const char *val  = resolve_css_var(doc, sheet, d->val_str, &vlen, &used_var);
        if (used_var) st->dependency_flags |= ARCR_STYLE_DEP_VAR;

        if (arcr_str_eq_nocase(prop, "color")) {
            arcr_css_parse_color(val, vlen, &st->color_argb);
        } else if (arcr_str_eq_nocase(prop, "background-color") ||
                   arcr_str_eq_nocase(prop, "background")) {
            arcr_css_parse_color(val, vlen, &st->bg_argb);
        } else if (arcr_str_eq_nocase(prop, "border-color")) {
            arcr_css_parse_color(val, vlen, &st->border_argb);
        } else if (arcr_str_eq_nocase(prop, "display")) {
            if (arcr_str_eq_nocase_n(val, vlen, "none", 4u)) st->display = ARCR_DISPLAY_NONE;
            else if (arcr_str_eq_nocase_n(val, vlen, "inline", 6u)) st->display = ARCR_DISPLAY_INLINE;
            else st->display = ARCR_DISPLAY_BLOCK;
        } else if (arcr_str_eq_nocase(prop, "font-size")) {
            int32_t px = parse_length_px(val, vlen, st->font_px, &st->dependency_flags);
            if (px >= 8 && px <= 96) st->font_px = px;
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
        } else if (arcr_str_eq_nocase(prop, "word-break")) {
            st->word_break_keep_all = arcr_str_eq_nocase_n(val, vlen, "keep-all", 8u);
        } else if (arcr_str_eq_nocase(prop, "border-radius")) {
            st->border_radius = ARCR_MAX(0, parse_length_px(val, vlen, st->font_px, &st->dependency_flags));
        } else if (arcr_str_eq_nocase(prop, "padding")) {
            parse_box_shorthand(val, vlen, st->font_px, &st->padding, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "padding-top")) {
            st->padding.top = parse_length_px(val, vlen, st->font_px, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "padding-right")) {
            st->padding.right = parse_length_px(val, vlen, st->font_px, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "padding-bottom")) {
            st->padding.bottom = parse_length_px(val, vlen, st->font_px, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "padding-left")) {
            st->padding.left = parse_length_px(val, vlen, st->font_px, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "margin")) {
            parse_box_shorthand(val, vlen, st->font_px, &st->margin, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "margin-top")) {
            st->margin.top = parse_length_px(val, vlen, st->font_px, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "margin-bottom")) {
            st->margin.bottom = parse_length_px(val, vlen, st->font_px, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "margin-left")) {
            st->margin.left = parse_length_px(val, vlen, st->font_px, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "margin-right")) {
            st->margin.right = parse_length_px(val, vlen, st->font_px, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "border-width")) {
            parse_box_shorthand(val, vlen, st->font_px, &st->border, &st->dependency_flags);
        } else if (arcr_str_eq_nocase(prop, "border") || arcr_str_eq_nocase(prop, "border-left")) {
            bool left_only = arcr_str_eq_nocase(prop, "border-left");
            /* Parse e.g. "1px solid #cbd5e1" or "4px solid var(--accent)" */
            size_t k = 0u;
            int tok_idx = 0;
            while (k < vlen) {
                while (k < vlen && val[k] == ' ') ++k;
                size_t st_tok = k;
                int paren = 0;
                while (k < vlen && !(val[k] == ' ' && paren == 0)) {
                    if (val[k] == '(') ++paren;
                    else if (val[k] == ')' && paren > 0) --paren;
                    ++k;
                }
                if (k > st_tok) {
                    const char *tk = val + st_tok;
                    size_t tkl = k - st_tok;
                    if (tok_idx == 0) {
                        int32_t bw = parse_length_px(tk, tkl, st->font_px, &st->dependency_flags);
                        if (left_only) st->border.left = bw;
                        else st->border = (ArcrEdgeSizes){bw, bw, bw, bw};
                    } else {
                        bool u2 = false;
                        const char *resolved_c = resolve_css_var(doc, sheet, tk, &tkl, &u2);
                        arcr_css_parse_color(resolved_c, tkl, &st->border_argb);
                    }
                    ++tok_idx;
                }
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
    ArcrStyleShareSlot share_ring[ARCR_STYLE_SHARE_RING];
    size_t             share_cursor;
    size_t             share_hits;
} StyleResolverCtx;

static bool try_share_style(StyleResolverCtx *ctx, const ArcrDomNode *el,
                            const ArcrComputedStyle *parent_style,
                            const ArcrComputedStyle **out_shared) {
    /* Strict invalidation guard: never share if element has #id or inline style="..." */
    if (el->id != ARCR_ATOM_NULL) return false;
    if (arcr_dom_get_attr(el, ctx->style_attr_atom) != NULL) return false;

    for (size_t i = 0; i < ARCR_STYLE_SHARE_RING; ++i) {
        const ArcrStyleShareSlot *slot = &ctx->share_ring[i];
        if (!slot->shared_style) continue;
        if (slot->parent_style != parent_style) continue;
        if (slot->tag != el->tag || slot->class_count != el->class_count) continue;
        bool same_classes = true;
        for (uint32_t c = 0; c < el->class_count; ++c) {
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
                                   const ArcrComputedStyle *computed) {
    if (el->id != ARCR_ATOM_NULL) return;
    if (arcr_dom_get_attr(el, ctx->style_attr_atom) != NULL) return;

    ArcrStyleShareSlot *slot = &ctx->share_ring[ctx->share_cursor % ARCR_STYLE_SHARE_RING];
    ctx->share_cursor++;
    slot->parent_style = parent_style;
    slot->tag          = el->tag;
    slot->class_count  = el->class_count;
    for (uint32_t c = 0; c < el->class_count; ++c) {
        slot->classes[c] = el->classes[c];
    }
    slot->shared_style = computed;
}

static void resolve_node_recursive(StyleResolverCtx *ctx, ArcrDomNode *node,
                                   const ArcrComputedStyle *parent_style) {
    if (!node) return;
    if (node->kind == ARCR_NODE_ELEMENT) {
        const ArcrComputedStyle *shared = NULL;
        if (try_share_style(ctx, node, parent_style, &shared)) {
            node->computed_style = shared;
        } else {
            ArcrComputedStyle *st = (ArcrComputedStyle *)arcr_arena_alloc(
                &ctx->doc->arena, sizeof(ArcrComputedStyle), sizeof(void *));
            if (!st) return;
            apply_ua_defaults(ctx->doc->atoms, node, parent_style, st);

            if (ctx->sheet) {
                MatchedRule matched[64];
                size_t      mcount = 0u;
                for (const ArcrCssRule *r = ctx->sheet->rules_head;
                     r != NULL && mcount < ARCR_ARRAY_LEN(matched); r = r->next) {
                    if (rule_matches_element(r, node)) {
                        matched[mcount++].rule = r;
                    }
                }
                if (mcount > 1u) {
                    qsort(matched, mcount, sizeof(MatchedRule), cmp_matched_rules);
                }
                for (size_t i = 0; i < mcount; ++i) {
                    apply_declarations(ctx->doc, ctx->sheet, matched[i].rule->decls, st);
                }
            }

            /* Inline style="..." attribute overrides rule declarations */
            const char *inline_css = arcr_dom_get_attr(node, ctx->style_attr_atom);
            if (inline_css && inline_css[0] != '\0') {
                ArcrCssDecl *idecls = parse_declarations(
                    ctx->doc, ctx->sheet, inline_css, strlen(inline_css), false);
                apply_declarations(ctx->doc, ctx->sheet, idecls, st);
            }

            node->computed_style = st;
            record_shareable_style(ctx, node, parent_style, st);
        }

        if (node->computed_style->display == ARCR_DISPLAY_NONE) {
            return;
        }
        parent_style = node->computed_style;
    }

    for (ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        resolve_node_recursive(ctx, ch, parent_style);
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

    resolve_node_recursive(&ctx, doc->root, NULL);
    if (out_style_share_hits) *out_style_share_hits = ctx.share_hits;
}
