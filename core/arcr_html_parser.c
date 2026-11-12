/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * core/arcr_html_parser.c - Error-Recovering HTML5 Tokenizer, Fragment Parser & Resource Scanner
 */
#include "arcr_dom.h"
#include <stdlib.h>
#include <string.h>

#define ARCR_HTML_MAX_DEPTH 128u

static bool is_tag_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool is_void_element(const char *tag, size_t len) {
    static const char *const kVoidTags[] = {
        "area", "base", "br", "col", "embed", "hr", "img",
        "input", "link", "meta", "param", "source", "track", "wbr"
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
        "address", "article", "aside", "blockquote", "details", "div", "dl",
        "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3",
        "h4", "h5", "h6", "header", "hgroup", "hr", "main", "nav", "ol", "p",
        "pre", "section", "table", "ul"
    };
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kBlockTags); ++i) {
        if (arcr_str_eq_nocase_n(tag, len, kBlockTags[i], strlen(kBlockTags[i]))) {
            return true;
        }
    }
    return false;
}

static bool lookup_named_entity(const char *ent, size_t elen, uint32_t *out_cp) {
    static const struct {
        const char *name;
        uint32_t    cp;
    } kEntities[] = {
        {"amp",    '&'},     {"lt",     '<'},     {"gt",     '>'},
        {"quot",   '"'},     {"apos",   '\''},    {"nbsp",   0x00A0u},
        {"copy",   0x00A9u}, {"reg",    0x00AEu}, {"trade",  0x2122u},
        {"mdash",  0x2014u}, {"ndash",  0x2013u}, {"bull",   0x2022u},
        {"middot", 0x00B7u}, {"hellip", 0x2026u}, {"rarr",   0x2192u},
        {"larr",   0x2190u}, {"uarr",   0x2191u}, {"darr",   0x2193u},
        {"times",  0x00D7u}, {"divide", 0x00F7u}, {"plusmn", 0x00B1u},
        {"starf",  0x2605u}, {"star",   0x2606u}, {"hearts", 0x2665u},
        {"check",  0x2713u}, {"laquo",  0x00ABu}, {"raquo",  0x00BBu},
        {"lsquo",  0x2018u}, {"rsquo",  0x2019u}, {"ldquo",  0x201Cu},
        {"rdquo",  0x201Du}, {"euro",   0x20ACu}, {"yen",    0x00A5u},
        {"won",    0x20A9u}, {"deg",    0x00B0u}, {"sect",   0x00A7u},
        {"para",   0x00B6u}, {"le",     0x2264u}, {"ge",     0x2265u},
        {"ne",     0x2260u}, {"infin",  0x221Eu}
    };
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kEntities); ++i) {
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
    size_t i   = 0u;
    size_t pos = 0u;
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
            while (semi < src_len && semi - i <= 14u && src[semi] != ';' &&
                   !is_space(src[semi]) && src[semi] != '<' && src[semi] != '&') {
                ++semi;
            }
            if (semi < src_len && src[semi] == ';') {
                const char *ent  = src + i + 1u;
                size_t      elen = semi - (i + 1u);
                uint32_t    cp   = 0u;
                bool        matched = false;

                if (elen >= 2u && ent[0] == '#') {
                    uint32_t val = 0u;
                    matched = true;
                    if (ent[1] == 'x' || ent[1] == 'X') {
                        if (elen < 3u) matched = false;
                        for (size_t k = 2u; k < elen; ++k) {
                            char c = ent[k];
                            if (val > 0x10FFFFu) { matched = false; break; }
                            val <<= 4;
                            if (c >= '0' && c <= '9') val |= (uint32_t)(c - '0');
                            else if (c >= 'a' && c <= 'f') val |= (uint32_t)(c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') val |= (uint32_t)(c - 'A' + 10);
                            else { matched = false; break; }
                        }
                    } else {
                        for (size_t k = 1u; k < elen; ++k) {
                            if (ent[k] >= '0' && ent[k] <= '9') {
                                val = val * 10u + (uint32_t)(ent[k] - '0');
                                if (val > 0x10FFFFu) { matched = false; break; }
                            } else {
                                matched = false;
                                break;
                            }
                        }
                    }
                    if (matched && val > 0u && val <= 0x10FFFFu) {
                        cp = val;
                    } else {
                        matched = false;
                    }
                } else {
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

static size_t find_raw_end_tag(const char *s, size_t len, size_t start,
                               const char *tag, size_t tag_len) {
    for (size_t i = start; i + 2u + tag_len <= len; ++i) {
        if (s[i] == '<' && s[i + 1u] == '/' &&
            arcr_str_eq_nocase_n(s + i + 2u, tag_len, tag, tag_len)) {
            char next = (i + 2u + tag_len < len) ? s[i + 2u + tag_len] : '>';
            if (next == '>' || is_space(next) || next == '/') {
                return i;
            }
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
    size_t i = 0u;
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

static void finalize_opened_element(ArcrDocument *doc, ArcrDomNode *elem,
                                    const char *tag_ptr, size_t tag_len) {
    if (!doc || !elem) return;

    if (arcr_str_eq_nocase_n(tag_ptr, tag_len, "link", 4u)) {
        const char *rel  = arcr_dom_get_attr_cstr(doc, elem, "rel");
        const char *href = arcr_dom_get_attr_cstr(doc, elem, "href");
        if (rel_contains_stylesheet(rel) && href && href[0] != '\0') {
            append_external_ref(doc, &doc->ext_styles_head, href, elem);
        }
    } else if (arcr_str_eq_nocase_n(tag_ptr, tag_len, "script", 6u)) {
        const char *src = arcr_dom_get_attr_cstr(doc, elem, "src");
        if (src && src[0] != '\0') {
            append_external_ref(doc, &doc->ext_scripts_head, src, elem);
        }
    } else if (arcr_str_eq_nocase_n(tag_ptr, tag_len, "img", 3u)) {
        const char *src = arcr_dom_get_attr_cstr(doc, elem, "src");
        if (src && src[0] != '\0') {
            if (arcr_str_starts_with_nocase(src, "data:")) {
                ArcrImage img = {0, 0, NULL};
                if (arcr_image_decode_data_uri(src, strlen(src), &img) &&
                    img.pixels && img.width > 0 && img.height > 0) {
                    ArcrDomSurface *surf = (ArcrDomSurface *)arcr_arena_calloc(
                        &doc->arena, 1u, sizeof(ArcrDomSurface), sizeof(void *));
                    if (surf) {
                        surf->width       = img.width;
                        surf->height      = img.height;
                        surf->argb_pixels = img.pixels;
                        surf->heap_owned  = true;
                        elem->surface     = surf;
                    } else {
                        arcr_image_free(&img);
                    }
                }
            } else {
                append_external_ref(doc, &doc->ext_images_head, src, elem);
            }
        }
    } else if (arcr_str_eq_nocase_n(tag_ptr, tag_len, "input", 5u)) {
        if (!elem->input_value) {
            const char *v = arcr_dom_get_attr_cstr(doc, elem, "value");
            arcr_dom_set_input_value(doc, elem, v ? v : "");
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
} HtmlParseAtoms;

static void init_parse_atoms(ArcrAtomTable *atoms, HtmlParseAtoms *pa) {
    pa->html   = arcr_atom_intern_lower(atoms, "html", 4u);
    pa->head   = arcr_atom_intern_lower(atoms, "head", 4u);
    pa->body   = arcr_atom_intern_lower(atoms, "body", 4u);
    pa->p      = arcr_atom_intern_lower(atoms, "p", 1u);
    pa->li     = arcr_atom_intern_lower(atoms, "li", 2u);
    pa->ul     = arcr_atom_intern_lower(atoms, "ul", 2u);
    pa->ol     = arcr_atom_intern_lower(atoms, "ol", 2u);
    pa->pre    = arcr_atom_intern_lower(atoms, "pre", 3u);
    pa->table  = arcr_atom_intern_lower(atoms, "table", 5u);
    pa->thead  = arcr_atom_intern_lower(atoms, "thead", 5u);
    pa->tbody  = arcr_atom_intern_lower(atoms, "tbody", 5u);
    pa->tfoot  = arcr_atom_intern_lower(atoms, "tfoot", 5u);
    pa->tr     = arcr_atom_intern_lower(atoms, "tr", 2u);
    pa->td     = arcr_atom_intern_lower(atoms, "td", 2u);
    pa->th     = arcr_atom_intern_lower(atoms, "th", 2u);
    pa->dl     = arcr_atom_intern_lower(atoms, "dl", 2u);
    pa->dt     = arcr_atom_intern_lower(atoms, "dt", 2u);
    pa->dd     = arcr_atom_intern_lower(atoms, "dd", 2u);
    pa->option = arcr_atom_intern_lower(atoms, "option", 6u);
}

static void auto_close_implicit_tags(ArcrDomNode **stack, size_t *inout_depth,
                                     const HtmlParseAtoms *pa,
                                     const char *tag_ptr, size_t tag_len,
                                     ArcrAtom tag_atom) {
    size_t depth = *inout_depth;

    /* 1. Block tags auto-close open <p> */
    if (closes_open_paragraph(tag_ptr, tag_len)) {
        for (size_t k = depth; k > 1u; --k) {
            ArcrAtom st = stack[k - 1u]->tag;
            if (st == pa->p) {
                depth = k - 1u;
                break;
            }
            if (st == pa->body || st == pa->td || st == pa->th) break;
        }
    }

    /* 2. <li> auto-closes open <li> inside same list */
    if (tag_atom == pa->li) {
        for (size_t k = depth; k > 1u; --k) {
            ArcrAtom st = stack[k - 1u]->tag;
            if (st == pa->li) {
                depth = k - 1u;
                break;
            }
            if (st == pa->ul || st == pa->ol || st == pa->body) break;
        }
    }

    /* 3. <td> / <th> auto-closes open <td> / <th> inside same row */
    if (tag_atom == pa->td || tag_atom == pa->th) {
        for (size_t k = depth; k > 1u; --k) {
            ArcrAtom st = stack[k - 1u]->tag;
            if (st == pa->td || st == pa->th) {
                depth = k - 1u;
                break;
            }
            if (st == pa->tr || st == pa->table || st == pa->body) break;
        }
    }

    /* 4. <tr> auto-closes open <td>, <th>, and sibling <tr> */
    if (tag_atom == pa->tr) {
        for (size_t k = depth; k > 1u; --k) {
            ArcrAtom st = stack[k - 1u]->tag;
            if (st == pa->tr) {
                depth = k - 1u;
                break;
            }
            if (st == pa->thead || st == pa->tbody || st == pa->tfoot ||
                st == pa->table || st == pa->body) {
                break;
            }
        }
    }

    /* 5. <thead> / <tbody> / <tfoot> auto-closes open cells/rows/sections */
    if (tag_atom == pa->thead || tag_atom == pa->tbody || tag_atom == pa->tfoot) {
        for (size_t k = depth; k > 1u; --k) {
            ArcrAtom st = stack[k - 1u]->tag;
            if (st == pa->thead || st == pa->tbody || st == pa->tfoot) {
                depth = k - 1u;
                break;
            }
            if (st == pa->table || st == pa->body) break;
        }
    }

    /* 6. <dt> / <dd> and <option> */
    if (tag_atom == pa->dt || tag_atom == pa->dd) {
        for (size_t k = depth; k > 1u; --k) {
            ArcrAtom st = stack[k - 1u]->tag;
            if (st == pa->dt || st == pa->dd) {
                depth = k - 1u;
                break;
            }
            if (st == pa->dl || st == pa->body) break;
        }
    }
    if (tag_atom == pa->option && depth > 1u && stack[depth - 1u]->tag == pa->option) {
        --depth;
    }

    *inout_depth = depth;
}

static void parse_html_core(ArcrDocument *doc, ArcrDomNode *root_container,
                            const char *utf8_html, size_t len, bool is_fragment) {
    HtmlParseAtoms pa;
    init_parse_atoms(doc->atoms, &pa);

    ArcrDomNode *stack[ARCR_HTML_MAX_DEPTH];
    size_t       depth = 0u;
    stack[depth++]     = root_container;

    ArcrDomNode *html_el = NULL;
    ArcrDomNode *body_el = NULL;

    size_t i = 0u;
    while (i < len) {
        if (utf8_html[i] == '<') {
            /* 1. HTML Comment <!-- ... --> */
            if (i + 4u <= len && memcmp(utf8_html + i, "<!--", 4u) == 0) {
                size_t j = i + 4u;
                while (j + 3u <= len && memcmp(utf8_html + j, "-->", 3u) != 0) ++j;
                i = (j + 3u <= len) ? (j + 3u) : len;
                continue;
            }
            /* 2. DOCTYPE or Declaration <! ... > / <? ... > */
            if (i + 2u <= len && (utf8_html[i + 1u] == '!' || utf8_html[i + 1u] == '?')) {
                size_t j = i + 2u;
                while (j < len && utf8_html[j] != '>') ++j;
                i = (j < len) ? (j + 1u) : len;
                continue;
            }
            /* 3. Closing tag </tag> */
            if (i + 2u < len && utf8_html[i + 1u] == '/') {
                size_t j = i + 2u;
                while (j < len && is_space(utf8_html[j])) ++j;
                size_t tstart = j;
                while (j < len && is_tag_char(utf8_html[j])) ++j;
                size_t tend = j;
                while (j < len && utf8_html[j] != '>') ++j;
                i = (j < len) ? (j + 1u) : len;

                if (tend > tstart) {
                    ArcrAtom close_atom = arcr_atom_intern_lower(
                        doc->atoms, utf8_html + tstart, tend - tstart);
                    for (size_t k = depth; k > 1u; --k) {
                        if (stack[k - 1u]->tag == close_atom) {
                            if (is_fragment || (close_atom != pa.html && close_atom != pa.body)) {
                                depth = k - 1u;
                            }
                            break;
                        }
                    }
                }
                continue;
            }
            /* 4. Opening tag <tag attr="val"> */
            if (i + 1u < len && is_tag_char(utf8_html[i + 1u])) {
                size_t j      = i + 1u;
                size_t tstart = j;
                while (j < len && is_tag_char(utf8_html[j])) ++j;
                const char *tag_ptr = utf8_html + tstart;
                size_t      tag_len = j - tstart;

                ArcrAtom tag_atom = arcr_atom_intern_lower(doc->atoms, tag_ptr, tag_len);
                auto_close_implicit_tags(stack, &depth, &pa, tag_ptr, tag_len, tag_atom);

                if (!is_fragment) {
                    if (!html_el && tag_atom != pa.html) {
                        html_el = arcr_dom_create_element(doc, "html", 4u);
                        arcr_dom_append_child(doc->root, html_el);
                        if (depth < ARCR_HTML_MAX_DEPTH) stack[depth++] = html_el;
                    }
                    if (!body_el && tag_atom != pa.html && tag_atom != pa.head &&
                        !arcr_str_eq_nocase_n(tag_ptr, tag_len, "meta", 4u) &&
                        !arcr_str_eq_nocase_n(tag_ptr, tag_len, "title", 5u) &&
                        !arcr_str_eq_nocase_n(tag_ptr, tag_len, "style", 5u) &&
                        !arcr_str_eq_nocase_n(tag_ptr, tag_len, "link", 4u) &&
                        !arcr_str_eq_nocase_n(tag_ptr, tag_len, "base", 4u) &&
                        !(depth > 1u && stack[depth - 1u]->tag == pa.head &&
                          arcr_str_eq_nocase_n(tag_ptr, tag_len, "script", 6u))) {
                        if (depth > 1u && stack[depth - 1u]->tag == pa.head) {
                            --depth;
                        }
                        if (tag_atom != pa.body) {
                            body_el   = arcr_dom_create_element(doc, "body", 4u);
                            doc->body = body_el;
                            arcr_dom_append_child(html_el, body_el);
                            if (depth < ARCR_HTML_MAX_DEPTH) stack[depth++] = body_el;
                        }
                    }
                }

                ArcrDomNode *elem = NULL;
                if (!is_fragment && tag_atom == pa.html && html_el) {
                    elem = html_el;
                } else if (!is_fragment && tag_atom == pa.body && body_el) {
                    elem = body_el;
                } else {
                    elem = arcr_dom_create_element(doc, tag_ptr, tag_len);
                    arcr_dom_append_child(stack[depth - 1u], elem);
                    if (!is_fragment && tag_atom == pa.html) html_el = elem;
                    if (!is_fragment && tag_atom == pa.body) {
                        body_el   = elem;
                        doc->body = elem;
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
                            char q = utf8_html[j++];
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
                        /* Decode HTML entities inside attribute values when '&' is present */
                        bool has_amp = false;
                        for (size_t k = 0u; k < vlen; ++k) {
                            if (vptr[k] == '&') { has_amp = true; break; }
                        }
                        if (has_amp) {
                            size_t dlen = decode_and_collapse_text(vptr, vlen, NULL, false);
                            char *dbuf = (char *)arcr_arena_alloc(&doc->arena, dlen + 1u, 1u);
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
                i = j;

                finalize_opened_element(doc, elem, tag_ptr, tag_len);

                /* Raw/RCDATA text elements: <style>, <script>, <title>, <textarea> */
                bool is_style    = arcr_str_eq_nocase_n(tag_ptr, tag_len, "style", 5u);
                bool is_script   = arcr_str_eq_nocase_n(tag_ptr, tag_len, "script", 6u);
                bool is_title    = arcr_str_eq_nocase_n(tag_ptr, tag_len, "title", 5u);
                bool is_textarea = arcr_str_eq_nocase_n(tag_ptr, tag_len, "textarea", 8u);

                if (!self_closing && (is_style || is_script || is_title || is_textarea)) {
                    size_t raw_end      = find_raw_end_tag(utf8_html, len, i, tag_ptr, tag_len);
                    const char *raw_ptr = utf8_html + i;
                    size_t      raw_len = raw_end - i;

                    if (is_style && raw_len > 0u) {
                        arcr_dom_append_style_block(doc, raw_ptr, raw_len);
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
                            doc->title = arcr_arena_strdup(&doc->arena, tv, tl);
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
                    i = (close_gt < len) ? (close_gt + 1u) : len;
                    continue;
                }

                if (!self_closing && !is_void_element(tag_ptr, tag_len) &&
                    elem && stack[depth - 1u] != elem && depth + 1u < ARCR_HTML_MAX_DEPTH) {
                    stack[depth++] = elem;
                }
                continue;
            }
        }

        /* 5. Character data / Text node */
        size_t tstart = i;
        while (i < len) {
            if (utf8_html[i] == '<' && i + 1u < len &&
                (is_tag_char(utf8_html[i + 1u]) || utf8_html[i + 1u] == '/' ||
                 utf8_html[i + 1u] == '!' || utf8_html[i + 1u] == '?')) {
                break;
            }
            ++i;
        }
        size_t raw_len = i - tstart;
        if (raw_len == 0u) continue;

        bool all_ws = true;
        for (size_t k = 0u; k < raw_len; ++k) {
            if (!is_space(utf8_html[tstart + k])) {
                all_ws = false;
                break;
            }
        }
        ArcrDomNode *cur_parent = stack[depth - 1u];
        bool in_pre = false;
        for (size_t k = depth; k > 0u; --k) {
            if (stack[k - 1u]->tag == pa.pre) {
                in_pre = true;
                break;
            }
        }
        for (const ArcrDomNode *anc = cur_parent; anc != NULL && !in_pre; anc = anc->parent) {
            if (anc->tag == pa.pre) {
                in_pre = true;
                break;
            }
        }

        if (all_ws && !in_pre) {
            if (!cur_parent->first_child ||
                (!is_fragment && (cur_parent == doc->root || cur_parent == html_el ||
                                  cur_parent->tag == pa.head)) ||
                cur_parent->tag == pa.table || cur_parent->tag == pa.thead ||
                cur_parent->tag == pa.tbody || cur_parent->tag == pa.tfoot ||
                cur_parent->tag == pa.tr || cur_parent->tag == pa.ul ||
                cur_parent->tag == pa.ol) {
                continue;
            }
        }

        if (!is_fragment && !body_el) {
            if (!html_el) {
                html_el = arcr_dom_create_element(doc, "html", 4u);
                arcr_dom_append_child(doc->root, html_el);
                if (depth < ARCR_HTML_MAX_DEPTH) stack[depth++] = html_el;
            }
            body_el   = arcr_dom_create_element(doc, "body", 4u);
            doc->body = body_el;
            arcr_dom_append_child(html_el, body_el);
            if (depth < ARCR_HTML_MAX_DEPTH) stack[depth++] = body_el;
            cur_parent = body_el;
        }

        size_t decoded_len = decode_and_collapse_text(utf8_html + tstart, raw_len, NULL, !in_pre);
        if (decoded_len > 0u) {
            char *buf = (char *)arcr_arena_alloc(&doc->arena, decoded_len + 1u, 1u);
            if (buf) {
                decode_and_collapse_text(utf8_html + tstart, raw_len, buf, !in_pre);
                ArcrDomNode *tn = arcr_dom_create_text(doc, buf, decoded_len);
                if (tn) {
                    arcr_dom_append_child(cur_parent, tn);
                }
            }
        }
    }

    if (!is_fragment && !doc->body) {
        if (!html_el) {
            html_el = arcr_dom_create_element(doc, "html", 4u);
            if (html_el) arcr_dom_append_child(doc->root, html_el);
        }
        if (html_el) {
            body_el   = arcr_dom_create_element(doc, "body", 4u);
            doc->body = body_el;
            if (body_el) arcr_dom_append_child(html_el, body_el);
        }
    }
}

ArcrDocument *arcr_html_parse(ArcrAtomTable *atoms, const char *utf8_html, size_t len) {
    ArcrDocument *doc = arcr_dom_document_create(atoms);
    if (!doc) return NULL;
    parse_html_core(doc, doc->root, utf8_html ? utf8_html : "", utf8_html ? len : 0u, false);
    doc->dom_dirty = false;
    return doc;
}

bool arcr_html_parse_fragment(ArcrDocument *doc, ArcrDomNode *target_parent,
                              const char *utf8_html, size_t len) {
    if (!doc || !target_parent) return false;
    if (!utf8_html || len == 0u) return true;
    parse_html_core(doc, target_parent, utf8_html, len, true);
    doc->dom_dirty = true;
    return true;
}
