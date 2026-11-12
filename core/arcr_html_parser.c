/*
 * Arcradient (MSHTML-Next)
 * core/arcr_html_parser.c - Error-Recovering HTML5-Subset Tokenizer & Tree Builder
 */
#include "arcr_dom.h"
#include <stdlib.h>
#include <string.h>

#define ARCR_HTML_MAX_DEPTH 128

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
    for (size_t i = 0; i < ARCR_ARRAY_LEN(kVoidTags); ++i) {
        if (arcr_str_eq_nocase_n(tag, len, kVoidTags[i], strlen(kVoidTags[i]))) {
            return true;
        }
    }
    return false;
}

static bool closes_open_paragraph(const char *tag, size_t len) {
    static const char *const kBlockTags[] = {
        "address", "article", "aside", "blockquote", "div", "dl", "fieldset",
        "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header",
        "hr", "main", "nav", "ol", "p", "pre", "section", "table", "ul"
    };
    for (size_t i = 0; i < ARCR_ARRAY_LEN(kBlockTags); ++i) {
        if (arcr_str_eq_nocase_n(tag, len, kBlockTags[i], strlen(kBlockTags[i]))) {
            return true;
        }
    }
    return false;
}

/* Decodes common HTML entities (&amp;, &lt;, &gt;, &quot;, &nbsp;, &middot;, &rarr;, &#...;)
 * and collapses consecutive ASCII whitespace runs into single spaces. */
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
            while (semi < src_len && semi - i <= 12u && src[semi] != ';' &&
                   !is_space(src[semi]) && src[semi] != '<') {
                ++semi;
            }
            if (semi < src_len && src[semi] == ';') {
                const char *ent = src + i + 1u;
                size_t      elen = semi - (i + 1u);
                uint32_t    cp = 0u;
                bool        matched = true;

                if (arcr_str_eq_nocase_n(ent, elen, "amp", 3u))        cp = '&';
                else if (arcr_str_eq_nocase_n(ent, elen, "lt", 2u))    cp = '<';
                else if (arcr_str_eq_nocase_n(ent, elen, "gt", 2u))    cp = '>';
                else if (arcr_str_eq_nocase_n(ent, elen, "quot", 4u))  cp = '"';
                else if (arcr_str_eq_nocase_n(ent, elen, "apos", 4u))  cp = '\'';
                else if (arcr_str_eq_nocase_n(ent, elen, "nbsp", 4u))  cp = 0x00A0u;
                else if (arcr_str_eq_nocase_n(ent, elen, "middot", 6u)) cp = 0x00B7u;
                else if (arcr_str_eq_nocase_n(ent, elen, "bull", 4u))  cp = 0x2022u;
                else if (arcr_str_eq_nocase_n(ent, elen, "rarr", 4u))  cp = 0x2192u;
                else if (arcr_str_eq_nocase_n(ent, elen, "copy", 4u))  cp = 0x00A9u;
                else if (elen >= 2u && ent[0] == '#') {
                    uint32_t val = 0u;
                    if (ent[1] == 'x' || ent[1] == 'X') {
                        for (size_t k = 2u; k < elen; ++k) {
                            char c = ent[k];
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
                            } else { matched = false; break; }
                        }
                    }
                    if (matched && val > 0u) cp = val;
                    else matched = false;
                } else {
                    matched = false;
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

ArcrDocument *arcr_html_parse(ArcrAtomTable *atoms, const char *utf8_html, size_t len) {
    ArcrDocument *doc = arcr_dom_document_create(atoms);
    if (!doc) return NULL;
    if (!utf8_html || len == 0u) return doc;

    ArcrAtom atom_html  = arcr_atom_intern_lower(atoms, "html", 4u);
    ArcrAtom atom_head  = arcr_atom_intern_lower(atoms, "head", 4u);
    ArcrAtom atom_body  = arcr_atom_intern_lower(atoms, "body", 4u);
    ArcrAtom atom_p     = arcr_atom_intern_lower(atoms, "p", 1u);
    ArcrAtom atom_li    = arcr_atom_intern_lower(atoms, "li", 2u);
    ArcrAtom atom_pre   = arcr_atom_intern_lower(atoms, "pre", 3u);

    ArcrDomNode *stack[ARCR_HTML_MAX_DEPTH];
    size_t       depth = 0u;
    stack[depth++]     = doc->root;

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
            /* 2. DOCTYPE or Declaration <! ... > */
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
                    ArcrAtom close_atom = arcr_atom_intern_lower(atoms, utf8_html + tstart, tend - tstart);
                    /* Search open stack for matching tag (error recovery ignores stray end tags!) */
                    for (size_t k = depth; k > 1u; --k) {
                        if (stack[k - 1u]->tag == close_atom) {
                            /* Never pop <html> or <body> prematurely before EOF */
                            if (close_atom != atom_html && close_atom != atom_body) {
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
                size_t j = i + 1u;
                size_t tstart = j;
                while (j < len && is_tag_char(utf8_html[j])) ++j;
                const char *tag_ptr = utf8_html + tstart;
                size_t      tag_len = j - tstart;

                ArcrAtom tag_atom = arcr_atom_intern_lower(atoms, tag_ptr, tag_len);

                /* Implicit error recovery: auto-close open <p> on block start, <li> on sibling <li> */
                if (closes_open_paragraph(tag_ptr, tag_len)) {
                    for (size_t k = depth; k > 1u; --k) {
                        if (stack[k - 1u]->tag == atom_p) {
                            depth = k - 1u;
                            break;
                        }
                        if (stack[k - 1u]->tag == atom_body) break;
                    }
                }
                if (tag_atom == atom_li && depth > 1u && stack[depth - 1u]->tag == atom_li) {
                    --depth;
                }

                /* Ensure <html> and <body> exist when body-level elements appear */
                if (!html_el && tag_atom != atom_html) {
                    html_el = arcr_dom_create_element(doc, "html", 4u);
                    arcr_dom_append_child(doc->root, html_el);
                    stack[depth++] = html_el;
                }
                if (!body_el && tag_atom != atom_html && tag_atom != atom_head &&
                    !arcr_str_eq_nocase_n(tag_ptr, tag_len, "meta", 4u) &&
                    !arcr_str_eq_nocase_n(tag_ptr, tag_len, "title", 5u) &&
                    !arcr_str_eq_nocase_n(tag_ptr, tag_len, "style", 5u) &&
                    !arcr_str_eq_nocase_n(tag_ptr, tag_len, "link", 4u)) {
                    if (depth > 1u && stack[depth - 1u]->tag == atom_head) {
                        --depth;
                    }
                    if (tag_atom != atom_body) {
                        body_el = arcr_dom_create_element(doc, "body", 4u);
                        doc->body = body_el;
                        arcr_dom_append_child(html_el, body_el);
                        stack[depth++] = body_el;
                    }
                }

                ArcrDomNode *elem = NULL;
                if (tag_atom == atom_html && html_el) {
                    elem = html_el;
                } else if (tag_atom == atom_body && body_el) {
                    elem = body_el;
                } else {
                    elem = arcr_dom_create_element(doc, tag_ptr, tag_len);
                    arcr_dom_append_child(stack[depth - 1u], elem);
                    if (tag_atom == atom_html) html_el = elem;
                    if (tag_atom == atom_body) {
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
                            while (j < len && !is_space(utf8_html[j]) && utf8_html[j] != '>') ++j;
                            vptr = utf8_html + vstart;
                            vlen = j - vstart;
                        }
                    }
                    if (aend > astart && elem) {
                        arcr_dom_set_attr(doc, elem, utf8_html + astart, aend - astart, vptr, vlen);
                    } else if (j == astart) {
                        ++j; /* Guard against malformed character inside tag */
                    }
                }
                i = j;

                /* Raw text elements: <style>, <script>, <title> */
                bool is_style  = arcr_str_eq_nocase_n(tag_ptr, tag_len, "style", 5u);
                bool is_script = arcr_str_eq_nocase_n(tag_ptr, tag_len, "script", 6u);
                bool is_title  = arcr_str_eq_nocase_n(tag_ptr, tag_len, "title", 5u);

                if (!self_closing && (is_style || is_script || is_title)) {
                    size_t raw_end = find_raw_end_tag(utf8_html, len, i, tag_ptr, tag_len);
                    const char *raw_ptr = utf8_html + i;
                    size_t      raw_len = raw_end - i;

                    if (is_style && raw_len > 0u) {
                        ArcrStyleBlock *sb = (ArcrStyleBlock *)arcr_arena_alloc(
                            &doc->arena, sizeof(ArcrStyleBlock), sizeof(void *));
                        if (sb) {
                            sb->css_text = arcr_arena_strdup(&doc->arena, raw_ptr, raw_len);
                            sb->css_len  = raw_len;
                            sb->next     = NULL;
                            if (!doc->style_blocks_head) {
                                doc->style_blocks_head = sb;
                                doc->style_blocks_tail = sb;
                            } else {
                                doc->style_blocks_tail->next = sb;
                                doc->style_blocks_tail       = sb;
                            }
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
                    }

                    /* Advance past closing </style> / </script> / </title> */
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

        /* Skip pure inter-element formatting whitespace when not inside inline/text container */
        bool all_ws = true;
        for (size_t k = 0; k < raw_len; ++k) {
            if (!is_space(utf8_html[tstart + k])) {
                all_ws = false;
                break;
            }
        }
        ArcrDomNode *cur_parent = stack[depth - 1u];
        bool in_pre = false;
        for (size_t k = depth; k > 0u; --k) {
            if (stack[k - 1u]->tag == atom_pre) {
                in_pre = true;
                break;
            }
        }
        if (all_ws && !in_pre) {
            /* Keep single inter-span space only if parent already has inline children */
            if (!cur_parent->first_child || cur_parent == doc->root ||
                cur_parent == html_el || cur_parent->tag == atom_head) {
                continue;
            }
        }

        if (!body_el) {
            if (!html_el) {
                html_el = arcr_dom_create_element(doc, "html", 4u);
                arcr_dom_append_child(doc->root, html_el);
                stack[depth++] = html_el;
            }
            body_el   = arcr_dom_create_element(doc, "body", 4u);
            doc->body = body_el;
            arcr_dom_append_child(html_el, body_el);
            stack[depth++] = body_el;
            cur_parent = body_el;
        }

        size_t decoded_len = decode_and_collapse_text(utf8_html + tstart, raw_len, NULL, !in_pre);
        if (decoded_len > 0u) {
            char *buf = (char *)arcr_arena_alloc(&doc->arena, decoded_len + 1u, 1u);
            if (buf) {
                decode_and_collapse_text(utf8_html + tstart, raw_len, buf, !in_pre);
                ArcrDomNode *tn = (ArcrDomNode *)arcr_slab_alloc(&doc->node_slab);
                if (tn) {
                    tn->kind     = ARCR_NODE_TEXT;
                    tn->text     = buf;
                    tn->text_len = decoded_len;
                    doc->node_count++;
                    arcr_dom_append_child(cur_parent, tn);
                }
            }
        }
    }

    return doc;
}
