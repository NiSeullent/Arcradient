/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * core/arcr_dom.c - Arena/Slab DOM Lifecycle, Resettable Style Arena, Unlimited Classes,
 *                   HTML Serialization, Surfaces & Full CSS3 Selector Queries
 */
#include "arcr_dom.h"
#include <stdlib.h>
#include <string.h>

uint64_t arcr_bloom_bits_for_atom(ArcrAtom atom) {
    if (atom == ARCR_ATOM_NULL) return 0ULL;
    /* Double-hash 2 bits in 64-bit Bloom filter */
    uint32_t x  = (uint32_t)atom * 0x9E3779B1u;
    uint32_t b1 = (x >> 26) & 63u;
    uint32_t b2 = ((x * 0x85EBCA6Bu) >> 26) & 63u;
    return (1ULL << b1) | (1ULL << b2);
}

static ArcrAtom dom_class_at(const ArcrDomNode *n, uint32_t idx) {
    if (idx < ARCR_MAX_INLINE_CLASSES) {
        return n->classes[idx];
    }
    return n->extra_classes[idx - ARCR_MAX_INLINE_CLASSES];
}

static void dom_set_class_at(ArcrDomNode *n, uint32_t idx, ArcrAtom atom) {
    if (idx < ARCR_MAX_INLINE_CLASSES) {
        n->classes[idx] = atom;
    } else {
        n->extra_classes[idx - ARCR_MAX_INLINE_CLASSES] = atom;
    }
}

static bool dom_push_class_atom(ArcrDocument *doc, ArcrDomNode *elem, ArcrAtom ca) {
    if (!doc || !elem || ca == ARCR_ATOM_NULL) return false;
    if (elem->class_count < ARCR_MAX_INLINE_CLASSES) {
        elem->classes[elem->class_count++] = ca;
        return true;
    }
    uint32_t extra_idx = elem->class_count - ARCR_MAX_INLINE_CLASSES;
    if (extra_idx >= elem->extra_class_cap) {
        uint32_t ncap = (elem->extra_class_cap == 0u) ? 16u : (elem->extra_class_cap * 2u);
        ArcrAtom *narr = (ArcrAtom *)arcr_arena_alloc(
            &doc->arena, (size_t)ncap * sizeof(ArcrAtom), sizeof(ArcrAtom));
        if (!narr) return false;
        if (elem->extra_classes && extra_idx > 0u) {
            memcpy(narr, elem->extra_classes, (size_t)extra_idx * sizeof(ArcrAtom));
        }
        elem->extra_classes   = narr;
        elem->extra_class_cap = ncap;
    }
    elem->extra_classes[extra_idx] = ca;
    elem->class_count++;
    return true;
}

static uint64_t node_self_bloom(const ArcrDomNode *n) {
    if (!n || n->kind != ARCR_NODE_ELEMENT) return 0ULL;
    uint64_t bits = arcr_bloom_bits_for_atom(n->tag);
    if (n->id != ARCR_ATOM_NULL) {
        bits |= arcr_bloom_bits_for_atom(n->id);
    }
    for (uint32_t i = 0u; i < n->class_count; ++i) {
        bits |= arcr_bloom_bits_for_atom(dom_class_at(n, i));
    }
    return bits;
}

static void recompute_subtree_bloom(ArcrDomNode *root_sub) {
    if (!root_sub) return;
    ArcrDomNode *cur = root_sub;
    while (cur) {
        if (cur->parent) {
            cur->ancestor_bloom = cur->parent->ancestor_bloom | node_self_bloom(cur->parent);
        } else {
            cur->ancestor_bloom = 0ULL;
        }
        if (cur->first_child) {
            cur = cur->first_child;
        } else {
            while (cur && cur != root_sub && !cur->next_sibling) {
                cur = cur->parent;
            }
            if (!cur || cur == root_sub) break;
            cur = cur->next_sibling;
        }
    }
}

static void recompute_children_bloom(ArcrDomNode *elem) {
    if (!elem) return;
    for (ArcrDomNode *ch = elem->first_child; ch != NULL; ch = ch->next_sibling) {
        recompute_subtree_bloom(ch);
    }
}

static void free_surface_heap_buffers(ArcrDomSurface *s) {
    if (!s) return;
    if (s->heap_owned && s->argb_pixels) {
        free(s->argb_pixels);
        s->argb_pixels = NULL;
        s->heap_owned  = false;
    }
    if (s->anim_frames_argb) {
        free(s->anim_frames_argb);
        s->anim_frames_argb = NULL;
    }
    if (s->anim_delays_ms) {
        free(s->anim_delays_ms);
        s->anim_delays_ms = NULL;
    }
    s->anim_frame_count   = 0;
    s->anim_current_frame = 0;
}

static void free_surfaces_in_tree(ArcrDomNode *root_node) {
    if (!root_node) return;
    ArcrDomNode *cur = root_node;
    while (cur) {
        if (cur->surface) {
            free_surface_heap_buffers(cur->surface);
        }
        if (cur->template_content) {
            free_surfaces_in_tree(cur->template_content);
        }
        if (cur->shadow_root) {
            free_surfaces_in_tree(cur->shadow_root);
        }
        if (cur->first_child) {
            cur = cur->first_child;
        } else {
            while (cur && cur != root_node && !cur->next_sibling) {
                cur = cur->parent;
            }
            if (!cur || cur == root_node) break;
            cur = cur->next_sibling;
        }
    }
}

static void destroy_subtree_nodes(ArcrDocument *doc, ArcrDomNode *node) {
    if (!doc || !node) return;
    ArcrDomNode *ch = node->first_child;
    while (ch) {
        ArcrDomNode *next = ch->next_sibling;
        ch->parent       = NULL;
        ch->prev_sibling = NULL;
        ch->next_sibling = NULL;
        destroy_subtree_nodes(doc, ch);
        ch = next;
    }
    if (node->template_content) {
        ArcrDomNode *tc = node->template_content;
        node->template_content = NULL;
        destroy_subtree_nodes(doc, tc);
    }
    if (node->shadow_root) {
        ArcrDomNode *sr = node->shadow_root;
        node->shadow_root = NULL;
        destroy_subtree_nodes(doc, sr);
    }
    if (node->surface) {
        free_surface_heap_buffers(node->surface);
    }
    if (doc->focused_node == node) doc->focused_node = NULL;
    if (doc->hovered_node == node) doc->hovered_node = NULL;
    if (doc->body == node)         doc->body         = NULL;
    if (doc->node_count > 0u)      doc->node_count--;
    arcr_slab_free(&doc->node_slab, node);
}

static void unlink_from_parent(ArcrDomNode *node) {
    if (!node || !node->parent) return;
    ArcrDomNode *p = node->parent;

    if (node->prev_sibling) {
        node->prev_sibling->next_sibling = node->next_sibling;
    } else if (p->first_child == node) {
        p->first_child = node->next_sibling;
    }

    if (node->next_sibling) {
        node->next_sibling->prev_sibling = node->prev_sibling;
    } else if (p->last_child == node) {
        p->last_child = node->prev_sibling;
    }

    node->parent       = NULL;
    node->prev_sibling = NULL;
    node->next_sibling = NULL;
}

ArcrDocument *arcr_dom_document_create(ArcrAtomTable *atoms) {
    if (!atoms) return NULL;
    ArcrDocument *doc = (ArcrDocument *)calloc(1u, sizeof(ArcrDocument));
    if (!doc) return NULL;

    arcr_arena_init(&doc->arena, 32768u);
    arcr_arena_init(&doc->style_arena, 65536u);
    arcr_slab_init(&doc->node_slab, &doc->arena, sizeof(ArcrDomNode), sizeof(void *));
    doc->atoms     = atoms;
    doc->title     = "";
    doc->base_href = NULL;

    ArcrDomNode *root = (ArcrDomNode *)arcr_slab_alloc(&doc->node_slab);
    if (!root) {
        arcr_arena_destroy(&doc->style_arena);
        arcr_arena_destroy(&doc->arena);
        free(doc);
        return NULL;
    }
    memset(root, 0, sizeof(*root));
    root->kind      = ARCR_NODE_DOCUMENT;
    root->node_uid  = ++doc->next_uid;
    doc->root       = root;
    doc->node_count = 1u;
    return doc;
}

void arcr_dom_document_destroy(ArcrDocument *doc) {
    if (!doc) return;
    /* Release all heap-owned image/canvas/animated pixel buffers before tearing down arenas */
    free_surfaces_in_tree(doc->root);
    arcr_arena_destroy(&doc->style_arena);
    arcr_arena_destroy(&doc->arena);
    free(doc);
}

ArcrDomNode *arcr_dom_create_element(ArcrDocument *doc, const char *tag_name, size_t tag_len) {
    if (!doc || !tag_name) return NULL;
    ArcrDomNode *node = (ArcrDomNode *)arcr_slab_alloc(&doc->node_slab);
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));
    node->kind     = ARCR_NODE_ELEMENT;
    node->node_uid = ++doc->next_uid;
    node->tag      = arcr_atom_intern_lower(doc->atoms, tag_name, tag_len);
    doc->node_count++;
    doc->dom_dirty = true;

    if (arcr_str_eq_nocase_n(tag_name, tag_len, "script", 6u)) {
        doc->has_scripts = true;
    } else if (arcr_str_eq_nocase_n(tag_name, tag_len, "video", 5u) ||
               arcr_str_eq_nocase_n(tag_name, tag_len, "audio", 5u)) {
        doc->has_media = true;
    } else if (arcr_str_eq_nocase_n(tag_name, tag_len, "img", 3u) ||
               arcr_str_eq_nocase_n(tag_name, tag_len, "canvas", 6u) ||
               arcr_str_eq_nocase_n(tag_name, tag_len, "svg", 3u)) {
        doc->has_images = true;
    } else if (arcr_str_eq_nocase_n(tag_name, tag_len, "template", 8u)) {
        ArcrDomNode *frag = (ArcrDomNode *)arcr_slab_alloc(&doc->node_slab);
        if (frag) {
            memset(frag, 0, sizeof(*frag));
            frag->kind             = ARCR_NODE_DOCUMENT;
            frag->node_uid         = ++doc->next_uid;
            node->template_content = frag;
            doc->node_count++;
        }
    }
    return node;
}

ArcrDomNode *arcr_dom_create_text(ArcrDocument *doc, const char *utf8, size_t len) {
    if (!doc) return NULL;
    ArcrDomNode *node = (ArcrDomNode *)arcr_slab_alloc(&doc->node_slab);
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));
    node->kind     = ARCR_NODE_TEXT;
    node->node_uid = ++doc->next_uid;
    node->text     = arcr_arena_strdup(&doc->arena, utf8 ? utf8 : "", len);
    node->text_len = len;
    doc->node_count++;
    doc->dom_dirty = true;
    return node;
}

ArcrDomNode *arcr_dom_create_comment(ArcrDocument *doc, const char *utf8, size_t len) {
    if (!doc) return NULL;
    ArcrDomNode *node = (ArcrDomNode *)arcr_slab_alloc(&doc->node_slab);
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));
    node->kind     = ARCR_NODE_COMMENT;
    node->node_uid = ++doc->next_uid;
    node->text     = arcr_arena_strdup(&doc->arena, utf8 ? utf8 : "", len);
    node->text_len = len;
    doc->node_count++;
    doc->dom_dirty = true;
    return node;
}

static bool is_node_ancestor_of(const ArcrDomNode *cand_anc, const ArcrDomNode *node) {
    for (const ArcrDomNode *p = node; p != NULL; p = p->parent) {
        if (p == cand_anc) return true;
    }
    return false;
}

void arcr_dom_append_child(ArcrDomNode *parent, ArcrDomNode *child) {
    if (!parent || !child || is_node_ancestor_of(child, parent)) return;
    if (child->parent) {
        unlink_from_parent(child);
    }
    child->parent       = parent;
    child->next_sibling = NULL;

    if (!parent->first_child) {
        child->prev_sibling = NULL;
        parent->first_child = child;
        parent->last_child  = child;
    } else {
        child->prev_sibling              = parent->last_child;
        parent->last_child->next_sibling = child;
        parent->last_child               = child;
    }
    recompute_subtree_bloom(child);
}

void arcr_dom_insert_before(ArcrDomNode *parent, ArcrDomNode *new_child, ArcrDomNode *ref_child) {
    if (!parent || !new_child || is_node_ancestor_of(new_child, parent)) return;
    if (!ref_child) {
        arcr_dom_append_child(parent, new_child);
        return;
    }
    if (ref_child == new_child && new_child->parent == parent) return;
    if (ref_child->parent != parent) return;

    if (new_child->parent) {
        unlink_from_parent(new_child);
    }

    new_child->parent       = parent;
    new_child->prev_sibling = ref_child->prev_sibling;
    new_child->next_sibling = ref_child;

    if (ref_child->prev_sibling) {
        ref_child->prev_sibling->next_sibling = new_child;
    } else {
        parent->first_child = new_child;
    }
    ref_child->prev_sibling = new_child;

    recompute_subtree_bloom(new_child);
}

void arcr_dom_remove_node(ArcrDocument *doc, ArcrDomNode *node) {
    if (!doc || !node || node == doc->root) return;
    unlink_from_parent(node);
    destroy_subtree_nodes(doc, node);
    doc->dom_dirty = true;
}

void arcr_dom_clear_children(ArcrDocument *doc, ArcrDomNode *elem) {
    if (!doc || !elem) return;
    ArcrDomNode *ch   = elem->first_child;
    elem->first_child = NULL;
    elem->last_child  = NULL;
    while (ch) {
        ArcrDomNode *next = ch->next_sibling;
        ch->parent       = NULL;
        ch->prev_sibling = NULL;
        ch->next_sibling = NULL;
        destroy_subtree_nodes(doc, ch);
        ch = next;
    }
    doc->dom_dirty = true;
}

static bool has_svg_ancestor(const ArcrDocument *doc, const ArcrDomNode *node) {
    for (const ArcrDomNode *p = node ? node->parent : NULL; p != NULL; p = p->parent) {
        if (p->kind == ARCR_NODE_ELEMENT &&
            arcr_str_eq_nocase(arcr_atom_cstr(doc->atoms, p->tag), "svg")) {
            return true;
        }
    }
    return false;
}

static bool is_connected_to_doc_root(const ArcrDocument *doc, const ArcrDomNode *node) {
    if (!doc || !doc->root || !node) return false;
    for (const ArcrDomNode *p = node; p != NULL; p = p->parent) {
        if (p == doc->root) return true;
    }
    return false;
}

void arcr_dom_set_text_content(ArcrDocument *doc, ArcrDomNode *elem,
                               const char *utf8, size_t len) {
    if (!doc || !elem) return;
    if (elem->kind == ARCR_NODE_TEXT || elem->kind == ARCR_NODE_COMMENT) {
        elem->text     = arcr_arena_strdup(&doc->arena, utf8 ? utf8 : "", len);
        elem->text_len = len;
        doc->dom_dirty = true;
        return;
    }
    arcr_dom_clear_children(doc, elem);
    if (utf8 && len > 0u) {
        ArcrDomNode *tn = arcr_dom_create_text(doc, utf8, len);
        if (tn) {
            arcr_dom_append_child(elem, tn);
            if (elem->kind == ARCR_NODE_ELEMENT) {
                const char *tname = arcr_atom_cstr(doc->atoms, elem->tag);
                if (arcr_str_eq_nocase(tname, "title") &&
                    is_connected_to_doc_root(doc, elem) &&
                    !has_svg_ancestor(doc, elem)) {
                    doc->title = tn->text;
                } else if (arcr_str_eq_nocase(tname, "textarea")) {
                    arcr_dom_set_input_value(doc, elem, tn->text);
                }
            }
        }
    }
    doc->dom_dirty = true;
}

static void collect_text_rec(const ArcrDomNode *n, char *out_buf, size_t cap,
                             size_t *inout_written, size_t *inout_total) {
    if (!n) return;
    if ((n->kind == ARCR_NODE_TEXT ||
         (n->kind == ARCR_NODE_ELEMENT && !n->first_child && n->text_len > 0u)) &&
        n->text && n->text_len > 0u) {
        *inout_total += n->text_len;
        if (out_buf && cap > 1u && *inout_written + 1u < cap) {
            size_t avail  = (cap - 1u) - *inout_written;
            size_t copy_n = ARCR_MIN(avail, n->text_len);
            memcpy(out_buf + *inout_written, n->text, copy_n);
            *inout_written += copy_n;
        }
    }
    for (const ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        collect_text_rec(ch, out_buf, cap, inout_written, inout_total);
    }
}

size_t arcr_dom_get_text_content(const ArcrDomNode *node, char *out_buf, size_t cap) {
    if (!node) {
        if (out_buf && cap > 0u) out_buf[0] = '\0';
        return 0u;
    }
    size_t written = 0u;
    size_t total   = 0u;
    collect_text_rec(node, out_buf, cap, &written, &total);
    if (out_buf && cap > 0u) {
        out_buf[written] = '\0';
        return written;
    }
    return total;
}

/* -------------------------------------------------------------------------
 * HTML5 / SVG Subtree Serializer (innerHTML / outerHTML)
 * ------------------------------------------------------------------------- */
typedef struct HtmlSerBuf {
    char  *data;
    size_t len;
    size_t cap;
    bool   oom;
} HtmlSerBuf;

static bool ser_reserve(HtmlSerBuf *sb, size_t extra) {
    if (sb->oom) return false;
    size_t needed = sb->len + extra + 1u;
    if (needed <= sb->cap) return true;
    size_t ncap = (sb->cap == 0u) ? 256u : sb->cap;
    while (ncap < needed) {
        ncap *= 2u;
    }
    char *grown = (char *)realloc(sb->data, ncap);
    if (!grown) {
        sb->oom = true;
        return false;
    }
    sb->data = grown;
    sb->cap  = ncap;
    return true;
}

static void ser_append_raw(HtmlSerBuf *sb, const char *s, size_t n) {
    if (!s || n == 0u) return;
    if (!ser_reserve(sb, n)) return;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void ser_append_cstr(HtmlSerBuf *sb, const char *s) {
    if (!s) return;
    ser_append_raw(sb, s, strlen(s));
}

static void ser_append_escaped_text(HtmlSerBuf *sb, const char *s, size_t n) {
    if (!s || n == 0u) return;
    size_t run = 0u;
    for (size_t i = 0u; i < n; ++i) {
        const char *esc = NULL;
        if (s[i] == '&')      esc = "&amp;";
        else if (s[i] == '<') esc = "&lt;";
        else if (s[i] == '>') esc = "&gt;";
        if (esc) {
            if (i > run) ser_append_raw(sb, s + run, i - run);
            ser_append_cstr(sb, esc);
            run = i + 1u;
        }
    }
    if (n > run) ser_append_raw(sb, s + run, n - run);
}

static void ser_append_escaped_attr(HtmlSerBuf *sb, const char *s, size_t n) {
    if (!s || n == 0u) return;
    size_t run = 0u;
    for (size_t i = 0u; i < n; ++i) {
        const char *esc = NULL;
        if (s[i] == '&')      esc = "&amp;";
        else if (s[i] == '"') esc = "&quot;";
        else if (s[i] == '<') esc = "&lt;";
        else if (s[i] == '>') esc = "&gt;";
        if (esc) {
            if (i > run) ser_append_raw(sb, s + run, i - run);
            ser_append_cstr(sb, esc);
            run = i + 1u;
        }
    }
    if (n > run) ser_append_raw(sb, s + run, n - run);
}

static bool is_html_void_tag(const char *t) {
    static const char *const kVoids[] = {
        "area", "base", "br", "col", "embed", "hr", "img",
        "input", "link", "meta", "param", "source", "track", "wbr"
    };
    if (!t) return false;
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kVoids); ++i) {
        if (arcr_str_eq_nocase(t, kVoids[i])) return true;
    }
    return false;
}

static void serialize_node_rec(const ArcrDocument *doc, const ArcrDomNode *node,
                               HtmlSerBuf *sb, bool raw_parent) {
    if (!node || sb->oom) return;

    if (node->kind == ARCR_NODE_DOCUMENT) {
        for (const ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
            serialize_node_rec(doc, ch, sb, false);
        }
        return;
    }

    if (node->kind == ARCR_NODE_TEXT) {
        if (node->text && node->text_len > 0u) {
            if (raw_parent) {
                ser_append_raw(sb, node->text, node->text_len);
            } else {
                ser_append_escaped_text(sb, node->text, node->text_len);
            }
        }
        return;
    }

    if (node->kind == ARCR_NODE_COMMENT) {
        ser_append_raw(sb, "<!--", 4u);
        if (node->text && node->text_len > 0u) {
            ser_append_raw(sb, node->text, node->text_len);
        }
        ser_append_raw(sb, "-->", 3u);
        return;
    }

    if (node->kind == ARCR_NODE_ELEMENT) {
        const char *tname = arcr_atom_cstr(doc->atoms, node->tag);
        if (!tname || tname[0] == '\0') tname = "div";

        ser_append_raw(sb, "<", 1u);
        ser_append_cstr(sb, tname);

        for (const ArcrAttr *a = node->first_attr; a != NULL; a = a->next) {
            const char *aname = arcr_atom_cstr(doc->atoms, a->name);
            if (!aname || aname[0] == '\0') continue;
            ser_append_raw(sb, " ", 1u);
            ser_append_cstr(sb, aname);
            ser_append_raw(sb, "=\"", 2u);
            if (a->value) {
                ser_append_escaped_attr(sb, a->value, strlen(a->value));
            }
            ser_append_raw(sb, "\"", 1u);
        }

        if (is_html_void_tag(tname)) {
            ser_append_raw(sb, ">", 1u);
            return;
        }

        ser_append_raw(sb, ">", 1u);

        bool is_raw = arcr_str_eq_nocase(tname, "script") ||
                      arcr_str_eq_nocase(tname, "style");
        const ArcrDomNode *child_container =
            (node->template_content && arcr_str_eq_nocase(tname, "template"))
                ? node->template_content
                : node;
        if (!child_container->first_child && node->text && node->text_len > 0u) {
            if (is_raw) {
                ser_append_raw(sb, node->text, node->text_len);
            } else {
                ser_append_escaped_text(sb, node->text, node->text_len);
            }
        } else {
            for (const ArcrDomNode *ch = child_container->first_child; ch != NULL; ch = ch->next_sibling) {
                serialize_node_rec(doc, ch, sb, is_raw);
            }
        }

        ser_append_raw(sb, "</", 2u);
        ser_append_cstr(sb, tname);
        ser_append_raw(sb, ">", 1u);
    }
}

char *arcr_dom_serialize_html_alloc(const ArcrDocument *doc, const ArcrDomNode *node,
                                    bool include_self, size_t *out_len) {
    if (out_len) *out_len = 0u;
    if (!doc || !node) return NULL;

    HtmlSerBuf sb = {NULL, 0u, 0u, false};
    if (!ser_reserve(&sb, 64u)) return NULL;
    sb.data[0] = '\0';

    if (include_self) {
        serialize_node_rec(doc, node, &sb, false);
    } else {
        bool is_raw = false;
        const ArcrDomNode *child_container = node;
        if (node->kind == ARCR_NODE_ELEMENT) {
            const char *tn = arcr_atom_cstr(doc->atoms, node->tag);
            is_raw = arcr_str_eq_nocase(tn, "script") || arcr_str_eq_nocase(tn, "style");
            if (node->template_content && arcr_str_eq_nocase(tn, "template")) {
                child_container = node->template_content;
            }
        }
        if (node->kind == ARCR_NODE_ELEMENT && !child_container->first_child &&
            node->text && node->text_len > 0u) {
            if (is_raw) ser_append_raw(&sb, node->text, node->text_len);
            else        ser_append_escaped_text(&sb, node->text, node->text_len);
        } else {
            for (const ArcrDomNode *ch = child_container->first_child; ch != NULL; ch = ch->next_sibling) {
                serialize_node_rec(doc, ch, &sb, is_raw);
            }
        }
    }

    if (sb.oom) {
        free(sb.data);
        return NULL;
    }
    if (out_len) *out_len = sb.len;
    return sb.data;
}

/* -------------------------------------------------------------------------
 * Attributes & Unlimited Classes
 * ------------------------------------------------------------------------- */
static bool is_ws_char(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static void parse_class_list(ArcrDocument *doc, ArcrDomNode *elem, const char *val, size_t len) {
    elem->class_count = 0u;
    size_t i = 0u;
    while (i < len) {
        while (i < len && is_ws_char(val[i])) ++i;
        size_t start = i;
        while (i < len && !is_ws_char(val[i])) ++i;
        if (i > start) {
            ArcrAtom ca = arcr_atom_intern(doc->atoms, val + start, i - start);
            if (!arcr_dom_has_class(elem, ca)) {
                (void)dom_push_class_atom(doc, elem, ca);
            }
        }
    }
}

static void upsert_raw_attr(ArcrDocument *doc, ArcrDomNode *elem,
                            ArcrAtom name_atom, const char *stored_val) {
    ArcrAttr *tail = NULL;
    for (ArcrAttr *a = elem->first_attr; a != NULL; a = a->next) {
        if (a->name == name_atom) {
            a->value = stored_val;
            return;
        }
        tail = a;
    }
    ArcrAttr *attr = (ArcrAttr *)arcr_arena_alloc(&doc->arena, sizeof(ArcrAttr), sizeof(void *));
    if (!attr) return;
    attr->name  = name_atom;
    attr->value = stored_val;
    attr->next  = NULL;
    if (tail) {
        tail->next = attr;
    } else {
        elem->first_attr = attr;
    }
}

void arcr_dom_set_attr(ArcrDocument *doc, ArcrDomNode *elem,
                       const char *name, size_t name_len,
                       const char *val, size_t val_len) {
    if (!doc || !elem || elem->kind != ARCR_NODE_ELEMENT || !name || name_len == 0u) return;
    ArcrAtom name_atom = arcr_atom_intern_lower(doc->atoms, name, name_len);
    char    *stored    = arcr_arena_strdup(&doc->arena, val ? val : "", val_len);
    if (!stored) return;

    upsert_raw_attr(doc, elem, name_atom, stored);

    if (arcr_str_eq_nocase_n(name, name_len, "id", 2u)) {
        elem->id = (val_len > 0u) ? arcr_atom_intern(doc->atoms, val, val_len) : ARCR_ATOM_NULL;
        recompute_children_bloom(elem);
    } else if (arcr_str_eq_nocase_n(name, name_len, "class", 5u)) {
        parse_class_list(doc, elem, stored, val_len);
        recompute_children_bloom(elem);
    } else if (arcr_str_eq_nocase_n(name, name_len, "checked", 7u)) {
        elem->state_flags |= ARCR_STATE_CHECKED;
    } else if (arcr_str_eq_nocase_n(name, name_len, "value", 5u)) {
        if (!elem->input_value &&
            arcr_str_eq_nocase(arcr_atom_cstr(doc->atoms, elem->tag), "input")) {
            arcr_dom_set_input_value(doc, elem, stored);
        }
    } else if (arcr_str_starts_with_nocase(arcr_atom_cstr(doc->atoms, name_atom), "on")) {
        doc->has_scripts = true;
    }
    doc->dom_dirty = true;
}

void arcr_dom_remove_attr(ArcrDocument *doc, ArcrDomNode *elem, const char *name) {
    if (!doc || !elem || elem->kind != ARCR_NODE_ELEMENT || !name || name[0] == '\0') return;
    size_t nlen = strlen(name);
    ArcrAtom name_atom = arcr_atom_intern_lower(doc->atoms, name, nlen);

    ArcrAttr *prev = NULL;
    bool removed = false;
    for (ArcrAttr *cur = elem->first_attr; cur != NULL; cur = cur->next) {
        if (cur->name == name_atom) {
            if (prev) prev->next = cur->next;
            else      elem->first_attr = cur->next;
            removed = true;
            break;
        }
        prev = cur;
    }
    if (!removed) return;

    if (arcr_str_eq_nocase_n(name, nlen, "id", 2u)) {
        elem->id = ARCR_ATOM_NULL;
        recompute_children_bloom(elem);
    } else if (arcr_str_eq_nocase_n(name, nlen, "class", 5u)) {
        elem->class_count = 0u;
        recompute_children_bloom(elem);
    } else if (arcr_str_eq_nocase_n(name, nlen, "checked", 7u)) {
        elem->state_flags &= ~(uint32_t)ARCR_STATE_CHECKED;
    }
    doc->dom_dirty = true;
}

const char *arcr_dom_get_attr(const ArcrDomNode *elem, ArcrAtom name_atom) {
    if (!elem || name_atom == ARCR_ATOM_NULL) return NULL;
    for (const ArcrAttr *a = elem->first_attr; a != NULL; a = a->next) {
        if (a->name == name_atom) return a->value;
    }
    return NULL;
}

const char *arcr_dom_get_attr_cstr(const ArcrDocument *doc, const ArcrDomNode *elem,
                                    const char *attr_name) {
    if (!doc || !elem || !attr_name || attr_name[0] == '\0') return NULL;
    for (const ArcrAttr *a = elem->first_attr; a != NULL; a = a->next) {
        const char *aname = arcr_atom_cstr(doc->atoms, a->name);
        if (arcr_str_eq_nocase(aname, attr_name)) {
            return a->value;
        }
    }
    return NULL;
}

bool arcr_dom_has_class(const ArcrDomNode *elem, ArcrAtom class_atom) {
    if (!elem || class_atom == ARCR_ATOM_NULL) return false;
    uint32_t inline_n = ARCR_MIN(elem->class_count, ARCR_MAX_INLINE_CLASSES);
    for (uint32_t i = 0u; i < inline_n; ++i) {
        if (elem->classes[i] == class_atom) return true;
    }
    if (elem->class_count > ARCR_MAX_INLINE_CLASSES && elem->extra_classes) {
        uint32_t extra_n = elem->class_count - ARCR_MAX_INLINE_CLASSES;
        for (uint32_t i = 0u; i < extra_n; ++i) {
            if (elem->extra_classes[i] == class_atom) return true;
        }
    }
    return false;
}

static void sync_class_attr_string(ArcrDocument *doc, ArcrDomNode *elem) {
    size_t total_len = 0u;
    for (uint32_t i = 0u; i < elem->class_count; ++i) {
        const char *cs = arcr_atom_cstr(doc->atoms, dom_class_at(elem, i));
        size_t cl = strlen(cs);
        if (cl == 0u) continue;
        if (total_len > 0u) ++total_len;
        total_len += cl;
    }

    char *stored = (char *)arcr_arena_alloc(&doc->arena, total_len + 1u, 1u);
    if (!stored) return;

    size_t pos = 0u;
    for (uint32_t i = 0u; i < elem->class_count; ++i) {
        const char *cs = arcr_atom_cstr(doc->atoms, dom_class_at(elem, i));
        size_t cl = strlen(cs);
        if (cl == 0u) continue;
        if (pos > 0u) stored[pos++] = ' ';
        memcpy(stored + pos, cs, cl);
        pos += cl;
    }
    stored[pos] = '\0';

    ArcrAtom cls_atom = arcr_atom_intern_lower(doc->atoms, "class", 5u);
    upsert_raw_attr(doc, elem, cls_atom, stored);
}

void arcr_dom_add_class(ArcrDocument *doc, ArcrDomNode *elem,
                        const char *cls, size_t cls_len) {
    if (!doc || !elem || elem->kind != ARCR_NODE_ELEMENT || !cls) return;
    arcr_str_trim_view(&cls, &cls_len);
    if (cls_len == 0u) return;

    ArcrAtom ca = arcr_atom_intern(doc->atoms, cls, cls_len);
    if (arcr_dom_has_class(elem, ca)) return;
    if (!dom_push_class_atom(doc, elem, ca)) return;

    sync_class_attr_string(doc, elem);
    recompute_children_bloom(elem);
    doc->dom_dirty = true;
}

void arcr_dom_remove_class(ArcrDomNode *elem, ArcrAtom class_atom) {
    if (!elem || elem->kind != ARCR_NODE_ELEMENT || class_atom == ARCR_ATOM_NULL) return;
    uint32_t out_idx = 0u;
    bool changed = false;
    for (uint32_t i = 0u; i < elem->class_count; ++i) {
        ArcrAtom cur = dom_class_at(elem, i);
        if (cur == class_atom) {
            changed = true;
        } else {
            if (out_idx != i) {
                dom_set_class_at(elem, out_idx, cur);
            }
            out_idx++;
        }
    }
    if (changed) {
        elem->class_count = out_idx;
        recompute_children_bloom(elem);
    }
}

void arcr_dom_set_input_value(ArcrDocument *doc, ArcrDomNode *elem, const char *utf8_val) {
    if (!doc || !elem || elem->kind != ARCR_NODE_ELEMENT) return;
    const char *src = utf8_val ? utf8_val : "";
    size_t len = strlen(src);
    if (!elem->input_value || len + 1u > elem->input_cap) {
        size_t ncap = ARCR_MAX(64u, (len + 1u) * 2u);
        char *nbuf = (char *)arcr_arena_alloc(&doc->arena, ncap, 1u);
        if (!nbuf) return;
        elem->input_value = nbuf;
        elem->input_cap   = ncap;
    }
    memcpy(elem->input_value, src, len + 1u);
    doc->dom_dirty = true;
}

const char *arcr_dom_get_input_value(const ArcrDocument *doc, const ArcrDomNode *elem) {
    if (!elem) return "";
    if (elem->input_value) return elem->input_value;
    const char *v = arcr_dom_get_attr_cstr(doc, elem, "value");
    return v ? v : "";
}

ArcrDomSurface *arcr_dom_ensure_surface(ArcrDocument *doc, ArcrDomNode *elem,
                                        int32_t width, int32_t height) {
    if (!doc || !elem || elem->kind != ARCR_NODE_ELEMENT) return NULL;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return NULL;

    if (!elem->surface) {
        elem->surface = (ArcrDomSurface *)arcr_arena_calloc(
            &doc->arena, 1u, sizeof(ArcrDomSurface), sizeof(void *));
        if (!elem->surface) return NULL;
    }

    if (elem->surface->argb_pixels &&
        elem->surface->width == width &&
        elem->surface->height == height) {
        return elem->surface;
    }

    if (elem->surface->heap_owned && elem->surface->argb_pixels) {
        free(elem->surface->argb_pixels);
        elem->surface->argb_pixels = NULL;
        elem->surface->heap_owned  = false;
    }

    size_t pixel_count = (size_t)width * (size_t)height;
    uint32_t *px = (uint32_t *)calloc(pixel_count, sizeof(uint32_t));
    if (!px) return NULL;

    elem->surface->width       = width;
    elem->surface->height      = height;
    elem->surface->argb_pixels = px;
    elem->surface->heap_owned  = true;
    doc->has_images            = true;
    doc->dom_dirty             = true;
    return elem->surface;
}

bool arcr_dom_rerasterize_svg(ArcrDocument *doc, ArcrDomNode *svg_elem,
                              uint32_t current_color_argb, int32_t target_w, int32_t target_h) {
    if (!doc || !svg_elem || svg_elem->kind != ARCR_NODE_ELEMENT) return false;
    const char *tname = arcr_atom_cstr(doc->atoms, svg_elem->tag);
    if (!arcr_str_eq_nocase(tname, "svg")) return false;

    uint32_t eff_color = (current_color_argb == 0u) ? 0xFF1E293Bu : current_color_argb;
    int32_t  req_w     = (target_w > 0) ? ARCR_CLAMP(target_w, 1, 2048) : 0;
    int32_t  req_h     = (target_h > 0) ? ARCR_CLAMP(target_h, 1, 2048) : 0;

    if (svg_elem->surface && svg_elem->surface->argb_pixels &&
        svg_elem->svg_rendered_color == eff_color &&
        req_w > 0 && req_h > 0 &&
        svg_elem->svg_rendered_w == req_w &&
        svg_elem->svg_rendered_h == req_h) {
        return true;
    }

    if (!svg_elem->raw_svg_markup || svg_elem->raw_svg_len == 0u) {
        size_t ser_len = 0u;
        char  *ser_xml = arcr_dom_serialize_html_alloc(doc, svg_elem, true, &ser_len);
        if (!ser_xml || ser_len == 0u) {
            free(ser_xml);
            return false;
        }
        /* Restore case-sensitive SVG attributes needed by NanoSVG after lowercase atom interning */
        for (size_t i = 0u; i + 8u <= ser_len; ++i) {
            if (memcmp(ser_xml + i, "viewbox=", 8u) == 0) {
                ser_xml[i + 4u] = 'B';
            }
        }
        svg_elem->raw_svg_markup = arcr_arena_strdup(&doc->arena, ser_xml, ser_len);
        svg_elem->raw_svg_len    = ser_len;
        free(ser_xml);
        if (!svg_elem->raw_svg_markup) return false;
    }

    ArcrImage img = {0, 0, NULL};
    if (!arcr_svg_rasterize_mem(svg_elem->raw_svg_markup, svg_elem->raw_svg_len,
                                req_w, req_h, eff_color, &img) ||
        !img.pixels || img.width <= 0 || img.height <= 0) {
        arcr_image_free(&img);
        return false;
    }

    if (!svg_elem->surface) {
        svg_elem->surface = (ArcrDomSurface *)arcr_arena_calloc(
            &doc->arena, 1u, sizeof(ArcrDomSurface), sizeof(void *));
        if (!svg_elem->surface) {
            arcr_image_free(&img);
            return false;
        }
    } else {
        free_surface_heap_buffers(svg_elem->surface);
    }

    svg_elem->surface->width       = img.width;
    svg_elem->surface->height      = img.height;
    svg_elem->surface->argb_pixels = img.pixels;
    svg_elem->surface->heap_owned  = true;

    svg_elem->svg_rendered_color = eff_color;
    svg_elem->svg_rendered_w     = img.width;
    svg_elem->svg_rendered_h     = img.height;
    doc->has_images              = true;
    return true;
}

ArcrDomNode *arcr_dom_find_by_uid(ArcrDomNode *root, uint32_t uid) {
    if (!root || uid == 0u) return NULL;
    ArcrDomNode *cur = root;
    while (cur) {
        if (cur->node_uid == uid) return cur;
        if (cur->template_content) {
            ArcrDomNode *found = arcr_dom_find_by_uid(cur->template_content, uid);
            if (found) return found;
        }
        if (cur->shadow_root) {
            ArcrDomNode *found = arcr_dom_find_by_uid(cur->shadow_root, uid);
            if (found) return found;
        }
        if (cur->first_child) {
            cur = cur->first_child;
        } else {
            while (cur && cur != root && !cur->next_sibling) {
                cur = cur->parent;
            }
            if (!cur || cur == root) break;
            cur = cur->next_sibling;
        }
    }
    return NULL;
}

ArcrDomNode *arcr_dom_get_element_by_id(const ArcrDocument *doc, const char *id_str) {
    if (!doc || !doc->root || !id_str || id_str[0] == '\0') return NULL;
    ArcrDomNode *root = doc->root;
    ArcrDomNode *cur  = root;
    while (cur) {
        if (cur->kind == ARCR_NODE_ELEMENT && cur->id != ARCR_ATOM_NULL) {
            if (strcmp(arcr_atom_cstr(doc->atoms, cur->id), id_str) == 0) {
                return cur;
            }
        }
        if (cur->first_child) {
            cur = cur->first_child;
        } else {
            while (cur && cur != root && !cur->next_sibling) {
                cur = cur->parent;
            }
            if (!cur || cur == root) break;
            cur = cur->next_sibling;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Heap-Backed CSS3/CSS4 Selector Parser & Matcher (querySelectorAll & matches)
 * ------------------------------------------------------------------------- */
typedef enum QSelCombinator {
    QSEL_COMB_NONE       = 0,
    QSEL_COMB_DESCENDANT = 1,
    QSEL_COMB_CHILD      = 2,
    QSEL_COMB_ADJACENT   = 3, /* + */
    QSEL_COMB_SIBLING    = 4  /* ~ */
} QSelCombinator;

typedef enum QSelAttrOp {
    QSEL_ATTR_EXISTS = 0,
    QSEL_ATTR_EQ     = 1, /* =  */
    QSEL_ATTR_PREFIX = 2, /* ^= */
    QSEL_ATTR_SUFFIX = 3, /* $= */
    QSEL_ATTR_SUBSTR = 4, /* *= */
    QSEL_ATTR_WORD   = 5, /* ~= */
    QSEL_ATTR_DASH   = 6  /* |= */
} QSelAttrOp;

typedef struct QSelAttr {
    char      *name;
    size_t     name_len;
    char      *val;
    size_t     val_len;
    QSelAttrOp op;
    bool       ignore_case;
} QSelAttr;

struct QSelList;
struct QSelHasList;

typedef struct QSelStep {
    char                *tag;
    size_t               tag_len;
    char                *id;
    size_t               id_len;
    char               **classes;
    size_t              *class_lens;
    uint32_t             class_count;
    uint32_t             class_cap;
    QSelAttr            *attrs;
    uint32_t             attr_count;
    uint32_t             attr_cap;
    uint32_t             req_state;
    bool                 req_root;
    bool                 req_empty;
    bool                 req_first_child;
    bool                 req_last_child;
    bool                 req_only_child;
    bool                 req_first_of_type;
    bool                 req_last_of_type;
    bool                 req_only_of_type;
    bool                 req_disabled;
    bool                 req_enabled;
    bool                 req_focus_within;
    bool                 has_nth;
    bool                 nth_from_end;
    int32_t              nth_a;
    int32_t              nth_b;
    struct QSelList     *nth_of_list;
    bool                 has_nth_of_type;
    bool                 nth_type_from_end;
    int32_t              nth_type_a;
    int32_t              nth_type_b;
    struct QSelList    **not_lists;
    uint32_t             not_count;
    struct QSelList    **is_lists;
    uint32_t             is_count;
    struct QSelHasList **has_lists;
    uint32_t             has_count;
    bool                 impossible;
    QSelCombinator       comb_to_next;
} QSelStep;

typedef struct QSelChain {
    QSelStep *steps;
    uint32_t  step_count;
    uint32_t  step_cap;
    bool      valid;
} QSelChain;

typedef struct QSelList {
    QSelChain *chains;
    size_t     count;
    size_t     cap;
} QSelList;

typedef struct QSelRelChain {
    QSelCombinator rel_comb;
    QSelChain      chain;
} QSelRelChain;

typedef struct QSelHasList {
    QSelRelChain *items;
    size_t        count;
    size_t        cap;
} QSelHasList;

static void free_qsel_list(QSelList *list);
static void free_qsel_has_list(QSelHasList *hlist);

static void free_qsel_step(QSelStep *st) {
    if (!st) return;
    free(st->tag);
    free(st->id);
    for (uint32_t i = 0u; i < st->class_count; ++i) {
        free(st->classes[i]);
    }
    free(st->classes);
    free(st->class_lens);
    for (uint32_t i = 0u; i < st->attr_count; ++i) {
        free(st->attrs[i].name);
        free(st->attrs[i].val);
    }
    free(st->attrs);
    if (st->nth_of_list) {
        free_qsel_list(st->nth_of_list);
        free(st->nth_of_list);
    }
    for (uint32_t i = 0u; i < st->not_count; ++i) {
        free_qsel_list(st->not_lists[i]);
        free(st->not_lists[i]);
    }
    free(st->not_lists);
    for (uint32_t i = 0u; i < st->is_count; ++i) {
        free_qsel_list(st->is_lists[i]);
        free(st->is_lists[i]);
    }
    free(st->is_lists);
    for (uint32_t i = 0u; i < st->has_count; ++i) {
        free_qsel_has_list(st->has_lists[i]);
        free(st->has_lists[i]);
    }
    free(st->has_lists);
}

static void free_qsel_has_list(QSelHasList *hlist) {
    if (!hlist) return;
    for (size_t c = 0u; c < hlist->count; ++c) {
        for (uint32_t s = 0u; s < hlist->items[c].chain.step_count; ++s) {
            free_qsel_step(&hlist->items[c].chain.steps[s]);
        }
        free(hlist->items[c].chain.steps);
    }
    free(hlist->items);
    hlist->items = NULL;
    hlist->count = 0u;
    hlist->cap   = 0u;
}

static void free_qsel_list(QSelList *list) {
    if (!list) return;
    for (size_t c = 0u; c < list->count; ++c) {
        for (uint32_t s = 0u; s < list->chains[c].step_count; ++s) {
            free_qsel_step(&list->chains[c].steps[s]);
        }
        free(list->chains[c].steps);
    }
    free(list->chains);
    list->chains = NULL;
    list->count  = 0u;
    list->cap    = 0u;
}

static bool is_qsel_plain_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           ((unsigned char)c >= 0x80u);
}

static int qsel_hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Scans and unescapes a CSS identifier (handling \. \: \/ \[ \] \# \32  etc.) */
static char *scan_unescaped_ident(const char *s, size_t len, size_t *inout_i, size_t *out_ulen) {
    size_t i   = *inout_i;
    char  *buf = (char *)malloc(len - i + 5u);
    if (!buf) return NULL;
    size_t pos = 0u;

    while (i < len) {
        if (s[i] == '\\') {
            if (i + 1u >= len) {
                ++i;
                break;
            }
            ++i;
            int hv = qsel_hex_val(s[i]);
            if (hv >= 0) {
                uint32_t cp = 0u;
                size_t digits = 0u;
                while (i < len && digits < 6u && (hv = qsel_hex_val(s[i])) >= 0) {
                    cp = (cp << 4) | (uint32_t)hv;
                    ++i;
                    ++digits;
                }
                if (i < len && is_ws_char(s[i])) {
                    if (s[i] == '\r' && i + 1u < len && s[i + 1u] == '\n') i += 2u;
                    else ++i;
                }
                if (cp == 0u || cp > 0x10FFFFu) cp = ARCR_UNICODE_REPLACEMENT;
                char u8[4];
                size_t n = arcr_utf8_encode_one(cp, u8);
                memcpy(buf + pos, u8, n);
                pos += n;
            } else {
                buf[pos++] = s[i++];
            }
        } else if (is_qsel_plain_ident_char(s[i])) {
            buf[pos++] = s[i++];
        } else {
            break;
        }
    }

    if (pos == 0u) {
        free(buf);
        return NULL;
    }
    buf[pos]  = '\0';
    *inout_i  = i;
    *out_ulen = pos;
    return buf;
}

static bool qsel_parse_nth_expr(const char *s, size_t len, int32_t *out_a, int32_t *out_b) {
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;
    if (arcr_str_eq_nocase_n(s, len, "odd", 3u)) {
        *out_a = 2; *out_b = 1; return true;
    }
    if (arcr_str_eq_nocase_n(s, len, "even", 4u)) {
        *out_a = 2; *out_b = 0; return true;
    }

    /* Compact whitespace for An+B parsing */
    char compact[64];
    size_t cl = 0u;
    for (size_t i = 0u; i < len && cl + 1u < sizeof(compact); ++i) {
        if (!is_ws_char(s[i])) compact[cl++] = arcr_ascii_tolower(s[i]);
    }
    compact[cl] = '\0';
    if (cl == 0u) return false;

    size_t n_pos = 0u;
    while (n_pos < cl && compact[n_pos] != 'n') ++n_pos;
    if (n_pos >= cl) {
        int32_t b = 0;
        if (!arcr_parse_i32(compact, cl, &b)) return false;
        *out_a = 0;
        *out_b = b;
        return true;
    }

    int32_t a = 1;
    if (n_pos == 0u) {
        a = 1;
    } else if (n_pos == 1u && compact[0] == '+') {
        a = 1;
    } else if (n_pos == 1u && compact[0] == '-') {
        a = -1;
    } else if (!arcr_parse_i32(compact, n_pos, &a)) {
        return false;
    }

    int32_t b = 0;
    if (n_pos + 1u < cl) {
        if (!arcr_parse_i32(compact + n_pos + 1u, cl - n_pos - 1u, &b)) return false;
    }
    *out_a = a;
    *out_b = b;
    return true;
}

static bool parse_qsel_list_str(const char *s, size_t len, int depth, QSelList *out_list);
static bool parse_qsel_has_list_str(const char *s, size_t len, int depth, QSelHasList *out_hlist);

static bool parse_qsel_compound(const char *s, size_t len, int depth, QSelStep *out) {
    memset(out, 0, sizeof(*out));
    arcr_str_trim_view(&s, &len);
    if (len == 0u || depth > 4) return false;

    size_t i = 0u;
    if (s[0] == '*') {
        ++i;
    } else if (is_qsel_plain_ident_char(s[0]) || s[0] == '\\') {
        size_t tlen = 0u;
        out->tag = scan_unescaped_ident(s, len, &i, &tlen);
        if (!out->tag) return false;
        out->tag_len = tlen;
    }

    while (i < len) {
        char c = s[i];
        if (c == '#') {
            ++i;
            size_t ilen = 0u;
            char *id_str = scan_unescaped_ident(s, len, &i, &ilen);
            if (!id_str) return false;
            free(out->id);
            out->id     = id_str;
            out->id_len = ilen;
        } else if (c == '.') {
            ++i;
            size_t clen = 0u;
            char *cls_str = scan_unescaped_ident(s, len, &i, &clen);
            if (!cls_str) return false;
            if (out->class_count >= out->class_cap) {
                uint32_t ncap = (out->class_cap == 0u) ? 4u : (out->class_cap * 2u);
                char   **nc   = (char **)realloc(out->classes, ncap * sizeof(char *));
                size_t  *nl   = (size_t *)realloc(out->class_lens, ncap * sizeof(size_t));
                if (!nc || !nl) {
                    free(nc); free(nl); free(cls_str);
                    return false;
                }
                out->classes    = nc;
                out->class_lens = nl;
                out->class_cap  = ncap;
            }
            out->classes[out->class_count]    = cls_str;
            out->class_lens[out->class_count] = clen;
            out->class_count++;
        } else if (c == '[') {
            ++i;
            size_t close_b = i;
            char quote = 0;
            while (close_b < len) {
                if (s[close_b] == '\\' && close_b + 1u < len) {
                    close_b += 2u;
                    continue;
                }
                if (quote) {
                    if (s[close_b] == quote) quote = 0;
                } else {
                    if (s[close_b] == '"' || s[close_b] == '\'') quote = s[close_b];
                    else if (s[close_b] == ']') break;
                }
                ++close_b;
            }
            if (close_b >= len) return false;

            const char *inner = s + i;
            size_t ilen = close_b - i;
            arcr_str_trim_view(&inner, &ilen);
            i = close_b + 1u;
            if (ilen == 0u) return false;

            size_t op_pos = 0u;
            while (op_pos < ilen && inner[op_pos] != '=' && inner[op_pos] != '^' &&
                   inner[op_pos] != '$' && inner[op_pos] != '*' &&
                   inner[op_pos] != '~' && inner[op_pos] != '|') {
                ++op_pos;
            }

            const char *np = inner;
            size_t      nl = op_pos;
            arcr_str_trim_view(&np, &nl);
            if (nl == 0u) return false;

            QSelAttrOp op = QSEL_ATTR_EXISTS;
            size_t val_start = ilen;
            if (op_pos < ilen) {
                if (inner[op_pos] == '=') {
                    op = QSEL_ATTR_EQ;
                    val_start = op_pos + 1u;
                } else if (op_pos + 1u < ilen && inner[op_pos + 1u] == '=') {
                    if (inner[op_pos] == '^')      op = QSEL_ATTR_PREFIX;
                    else if (inner[op_pos] == '$') op = QSEL_ATTR_SUFFIX;
                    else if (inner[op_pos] == '*') op = QSEL_ATTR_SUBSTR;
                    else if (inner[op_pos] == '~') op = QSEL_ATTR_WORD;
                    else if (inner[op_pos] == '|') op = QSEL_ATTR_DASH;
                    val_start = op_pos + 2u;
                } else {
                    return false;
                }
            }

            bool icase = false;
            const char *vp = "";
            size_t vl = 0u;
            if (op != QSEL_ATTR_EXISTS && val_start <= ilen) {
                vp = inner + val_start;
                vl = ilen - val_start;
                arcr_str_trim_view(&vp, &vl);
                /* Check trailing 'i' or 's' modifier after quoted value or whitespace */
                if (vl >= 2u && (vp[vl - 1u] == 'i' || vp[vl - 1u] == 'I' ||
                                 vp[vl - 1u] == 's' || vp[vl - 1u] == 'S') &&
                    (is_ws_char(vp[vl - 2u]) || vp[vl - 2u] == '"' || vp[vl - 2u] == '\'')) {
                    icase = (vp[vl - 1u] == 'i' || vp[vl - 1u] == 'I');
                    --vl;
                    arcr_str_trim_view(&vp, &vl);
                }
                if (vl >= 2u && ((vp[0] == '"' && vp[vl - 1u] == '"') ||
                                 (vp[0] == '\'' && vp[vl - 1u] == '\''))) {
                    vp += 1u;
                    vl -= 2u;
                }
            }

            if (out->attr_count >= out->attr_cap) {
                uint32_t ncap = (out->attr_cap == 0u) ? 4u : (out->attr_cap * 2u);
                QSelAttr *na = (QSelAttr *)realloc(out->attrs, ncap * sizeof(QSelAttr));
                if (!na) return false;
                out->attrs    = na;
                out->attr_cap = ncap;
            }
            QSelAttr *qa = &out->attrs[out->attr_count];
            memset(qa, 0, sizeof(*qa));
            qa->name = (char *)malloc(nl + 1u);
            qa->val  = (char *)malloc(vl + 1u);
            if (!qa->name || !qa->val) {
                free(qa->name); free(qa->val);
                return false;
            }
            memcpy(qa->name, np, nl); qa->name[nl] = '\0'; qa->name_len = nl;
            memcpy(qa->val,  vp, vl); qa->val[vl]  = '\0'; qa->val_len  = vl;
            qa->op          = op;
            qa->ignore_case = icase;
            out->attr_count++;
        } else if (c == ':') {
            ++i;
            if (i < len && s[i] == ':') {
                out->impossible = true;
                return true;
            }
            size_t start = i;
            while (i < len && is_qsel_plain_ident_char(s[i])) ++i;
            const char *ps   = s + start;
            size_t      plen = i - start;
            const char *arg  = NULL;
            size_t      alen = 0u;
            if (i < len && s[i] == '(') {
                int pdepth = 1;
                char q = 0;
                ++i;
                size_t astart = i;
                while (i < len && pdepth > 0) {
                    if (s[i] == '\\' && i + 1u < len) { i += 2u; continue; }
                    if (q) {
                        if (s[i] == q) q = 0;
                    } else {
                        if (s[i] == '"' || s[i] == '\'') q = s[i];
                        else if (s[i] == '(') ++pdepth;
                        else if (s[i] == ')') --pdepth;
                    }
                    if (pdepth > 0) ++i;
                }
                if (pdepth != 0) return false;
                arg  = s + astart;
                alen = i - astart;
                ++i; /* skip ')' */
            }

            if (arcr_str_eq_nocase_n(ps, plen, "hover", 5u)) {
                out->req_state |= ARCR_STATE_HOVER;
            } else if (arcr_str_eq_nocase_n(ps, plen, "focus", 5u) ||
                       arcr_str_eq_nocase_n(ps, plen, "focus-visible", 13u)) {
                out->req_state |= ARCR_STATE_FOCUS;
            } else if (arcr_str_eq_nocase_n(ps, plen, "focus-within", 12u)) {
                out->req_focus_within = true;
            } else if (arcr_str_eq_nocase_n(ps, plen, "active", 6u)) {
                out->req_state |= ARCR_STATE_ACTIVE;
            } else if (arcr_str_eq_nocase_n(ps, plen, "checked", 7u)) {
                out->req_state |= ARCR_STATE_CHECKED;
            } else if (arcr_str_eq_nocase_n(ps, plen, "visited", 7u)) {
                out->req_state |= ARCR_STATE_VISITED;
            } else if (arcr_str_eq_nocase_n(ps, plen, "link", 4u) ||
                       arcr_str_eq_nocase_n(ps, plen, "any-link", 8u)) {
                /* link state */
            } else if (arcr_str_eq_nocase_n(ps, plen, "disabled", 8u)) {
                out->req_disabled = true;
            } else if (arcr_str_eq_nocase_n(ps, plen, "enabled", 7u)) {
                out->req_enabled = true;
            } else if (arcr_str_eq_nocase_n(ps, plen, "root", 4u) ||
                       arcr_str_eq_nocase_n(ps, plen, "scope", 5u)) {
                out->req_root = arcr_str_eq_nocase_n(ps, plen, "root", 4u);
            } else if (arcr_str_eq_nocase_n(ps, plen, "empty", 5u)) {
                out->req_empty = true;
            } else if (arcr_str_eq_nocase_n(ps, plen, "first-child", 11u)) {
                out->req_first_child = true;
            } else if (arcr_str_eq_nocase_n(ps, plen, "last-child", 10u)) {
                out->req_last_child = true;
            } else if (arcr_str_eq_nocase_n(ps, plen, "only-child", 10u)) {
                out->req_only_child = true;
            } else if (arcr_str_eq_nocase_n(ps, plen, "first-of-type", 13u)) {
                out->req_first_of_type = true;
            } else if (arcr_str_eq_nocase_n(ps, plen, "last-of-type", 12u)) {
                out->req_last_of_type = true;
            } else if (arcr_str_eq_nocase_n(ps, plen, "only-of-type", 12u)) {
                out->req_only_of_type = true;
            } else if (arcr_str_eq_nocase_n(ps, plen, "nth-child", 9u) ||
                       arcr_str_eq_nocase_n(ps, plen, "nth-last-child", 14u)) {
                bool from_end = arcr_str_eq_nocase_n(ps, plen, "nth-last-child", 14u);
                if (!arg) {
                    out->impossible = true;
                } else {
                    const char *expr_s = arg;
                    size_t      expr_l = alen;
                    const char *of_s   = NULL;
                    size_t      of_l   = 0u;
                    for (size_t k = 0u; k + 3u < alen; ++k) {
                        if (is_ws_char(arg[k]) &&
                            (arg[k + 1u] == 'o' || arg[k + 1u] == 'O') &&
                            (arg[k + 2u] == 'f' || arg[k + 2u] == 'F') &&
                            is_ws_char(arg[k + 3u])) {
                            expr_l = k;
                            of_s   = arg + k + 4u;
                            of_l   = alen - (k + 4u);
                            break;
                        }
                    }
                    if (!qsel_parse_nth_expr(expr_s, expr_l, &out->nth_a, &out->nth_b)) {
                        out->impossible = true;
                    } else if (of_s) {
                        QSelList *ofl = (QSelList *)calloc(1u, sizeof(QSelList));
                        if (!ofl || !parse_qsel_list_str(of_s, of_l, depth + 1, ofl)) {
                            free_qsel_list(ofl);
                            free(ofl);
                            return false;
                        }
                        if (out->nth_of_list) {
                            free_qsel_list(out->nth_of_list);
                            free(out->nth_of_list);
                        }
                        out->nth_of_list  = ofl;
                        out->has_nth      = true;
                        out->nth_from_end = from_end;
                    } else {
                        out->has_nth      = true;
                        out->nth_from_end = from_end;
                    }
                }
            } else if (arcr_str_eq_nocase_n(ps, plen, "nth-of-type", 11u) ||
                       arcr_str_eq_nocase_n(ps, plen, "nth-last-of-type", 16u)) {
                bool from_end = arcr_str_eq_nocase_n(ps, plen, "nth-last-of-type", 16u);
                if (!arg || !qsel_parse_nth_expr(arg, alen, &out->nth_type_a, &out->nth_type_b)) {
                    out->impossible = true;
                } else {
                    out->has_nth_of_type   = true;
                    out->nth_type_from_end = from_end;
                }
            } else if (arcr_str_eq_nocase_n(ps, plen, "not", 3u)) {
                if (!arg) return false;
                QSelList *sub = (QSelList *)calloc(1u, sizeof(QSelList));
                if (!sub || !parse_qsel_list_str(arg, alen, depth + 1, sub)) {
                    free_qsel_list(sub); free(sub);
                    return false;
                }
                QSelList **nl = (QSelList **)realloc(
                    out->not_lists, (out->not_count + 1u) * sizeof(QSelList *));
                if (!nl) { free_qsel_list(sub); free(sub); return false; }
                out->not_lists = nl;
                out->not_lists[out->not_count++] = sub;
            } else if (arcr_str_eq_nocase_n(ps, plen, "is", 2u) ||
                       arcr_str_eq_nocase_n(ps, plen, "where", 5u) ||
                       arcr_str_eq_nocase_n(ps, plen, "matches", 7u)) {
                if (!arg) return false;
                QSelList *sub = (QSelList *)calloc(1u, sizeof(QSelList));
                if (!sub || !parse_qsel_list_str(arg, alen, depth + 1, sub)) {
                    free_qsel_list(sub); free(sub);
                    return false;
                }
                QSelList **il = (QSelList **)realloc(
                    out->is_lists, (out->is_count + 1u) * sizeof(QSelList *));
                if (!il) { free_qsel_list(sub); free(sub); return false; }
                out->is_lists = il;
                out->is_lists[out->is_count++] = sub;
            } else if (arcr_str_eq_nocase_n(ps, plen, "has", 3u)) {
                if (!arg) return false;
                QSelHasList *hsub = (QSelHasList *)calloc(1u, sizeof(QSelHasList));
                if (!hsub || !parse_qsel_has_list_str(arg, alen, depth + 1, hsub)) {
                    free_qsel_has_list(hsub); free(hsub);
                    return false;
                }
                QSelHasList **hl = (QSelHasList **)realloc(
                    out->has_lists, (out->has_count + 1u) * sizeof(QSelHasList *));
                if (!hl) { free_qsel_has_list(hsub); free(hsub); return false; }
                out->has_lists = hl;
                out->has_lists[out->has_count++] = hsub;
            } else {
                out->impossible = true;
                return true;
            }
        } else {
            return false;
        }
    }
    return true;
}

static bool parse_qsel_chain(const char *s, size_t len, int depth, QSelChain *out) {
    memset(out, 0, sizeof(*out));
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;

    size_t i = 0u;
    while (i < len) {
        while (i < len && is_ws_char(s[i])) ++i;
        if (i >= len) break;

        size_t start = i;
        int bracket = 0;
        int paren   = 0;
        char quote  = 0;
        while (i < len) {
            if (s[i] == '\\' && i + 1u < len) {
                i += 2u;
                continue;
            }
            if (quote) {
                if (s[i] == quote) quote = 0;
            } else {
                if (s[i] == '"' || s[i] == '\'') quote = s[i];
                else if (s[i] == '[') ++bracket;
                else if (s[i] == ']' && bracket > 0) --bracket;
                else if (s[i] == '(') ++paren;
                else if (s[i] == ')' && paren > 0) --paren;
                else if (bracket == 0 && paren == 0 &&
                         (is_ws_char(s[i]) || s[i] == '>' || s[i] == '+' || s[i] == '~')) {
                    break;
                }
            }
            ++i;
        }
        if (i == start) return false;

        if (out->step_count >= out->step_cap) {
            uint32_t ncap = (out->step_cap == 0u) ? 4u : (out->step_cap * 2u);
            QSelStep *ns = (QSelStep *)realloc(out->steps, ncap * sizeof(QSelStep));
            if (!ns) return false;
            out->steps    = ns;
            out->step_cap = ncap;
        }

        QSelStep *step = &out->steps[out->step_count];
        if (!parse_qsel_compound(s + start, i - start, depth, step)) {
            free_qsel_step(step);
            return false;
        }

        QSelCombinator comb = QSEL_COMB_NONE;
        bool saw_ws = false;
        while (i < len && is_ws_char(s[i])) {
            saw_ws = true;
            ++i;
        }
        if (i < len && (s[i] == '>' || s[i] == '+' || s[i] == '~')) {
            char op = s[i++];
            if (op == '>')      comb = QSEL_COMB_CHILD;
            else if (op == '+') comb = QSEL_COMB_ADJACENT;
            else                comb = QSEL_COMB_SIBLING;
            while (i < len && is_ws_char(s[i])) ++i;
            if (i >= len) {
                out->step_count++;
                return false; /* Trailing combinator is invalid */
            }
        } else if (saw_ws && i < len) {
            comb = QSEL_COMB_DESCENDANT;
        }
        step->comb_to_next = comb;
        out->step_count++;
    }
    out->valid = (out->step_count > 0u);
    return out->valid;
}

static bool parse_qsel_has_list_str(const char *s, size_t len, int depth, QSelHasList *out_hlist) {
    memset(out_hlist, 0, sizeof(*out_hlist));
    arcr_str_trim_view(&s, &len);
    if (len == 0u || depth > 4) return false;

    size_t pos = 0u;
    while (pos < len) {
        size_t comma = pos;
        int bracket = 0;
        int paren   = 0;
        char quote  = 0;
        while (comma < len) {
            if (s[comma] == '\\' && comma + 1u < len) {
                comma += 2u;
                continue;
            }
            if (quote) {
                if (s[comma] == quote) quote = 0;
            } else {
                if (s[comma] == '"' || s[comma] == '\'') quote = s[comma];
                else if (s[comma] == '[') ++bracket;
                else if (s[comma] == ']' && bracket > 0) --bracket;
                else if (s[comma] == '(') ++paren;
                else if (s[comma] == ')' && paren > 0) --paren;
                else if (s[comma] == ',' && bracket == 0 && paren == 0) break;
            }
            ++comma;
        }

        const char *seg  = s + pos;
        size_t      slen = comma - pos;
        arcr_str_trim_view(&seg, &slen);
        if (slen > 0u) {
            QSelCombinator rcomb = QSEL_COMB_DESCENDANT;
            if (seg[0] == '>' || seg[0] == '+' || seg[0] == '~') {
                if (seg[0] == '>')      rcomb = QSEL_COMB_CHILD;
                else if (seg[0] == '+') rcomb = QSEL_COMB_ADJACENT;
                else                    rcomb = QSEL_COMB_SIBLING;
                ++seg;
                --slen;
                arcr_str_trim_view(&seg, &slen);
            }
            if (out_hlist->count >= out_hlist->cap) {
                size_t ncap = (out_hlist->cap == 0u) ? 4u : (out_hlist->cap * 2u);
                QSelRelChain *ni = (QSelRelChain *)realloc(
                    out_hlist->items, ncap * sizeof(QSelRelChain));
                if (!ni) return false;
                out_hlist->items = ni;
                out_hlist->cap   = ncap;
            }
            QSelRelChain *item = &out_hlist->items[out_hlist->count];
            item->rel_comb = rcomb;
            if (slen > 0u && parse_qsel_chain(seg, slen, depth, &item->chain)) {
                out_hlist->count++;
            } else {
                for (uint32_t k = 0u; k < item->chain.step_count; ++k) {
                    free_qsel_step(&item->chain.steps[k]);
                }
                free(item->chain.steps);
            }
        }
        pos = (comma < len) ? (comma + 1u) : len;
    }
    return out_hlist->count > 0u;
}

static bool parse_qsel_list_str(const char *s, size_t len, int depth, QSelList *out_list) {
    memset(out_list, 0, sizeof(*out_list));
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;

    size_t pos = 0u;
    while (pos < len) {
        size_t comma = pos;
        int bracket = 0;
        int paren   = 0;
        char quote  = 0;
        while (comma < len) {
            if (s[comma] == '\\' && comma + 1u < len) {
                comma += 2u;
                continue;
            }
            if (quote) {
                if (s[comma] == quote) quote = 0;
            } else {
                if (s[comma] == '"' || s[comma] == '\'') quote = s[comma];
                else if (s[comma] == '[') ++bracket;
                else if (s[comma] == ']' && bracket > 0) --bracket;
                else if (s[comma] == '(') ++paren;
                else if (s[comma] == ')' && paren > 0) --paren;
                else if (s[comma] == ',' && bracket == 0 && paren == 0) break;
            }
            ++comma;
        }

        if (out_list->count >= out_list->cap) {
            size_t ncap = (out_list->cap == 0u) ? 4u : (out_list->cap * 2u);
            QSelChain *nc = (QSelChain *)realloc(out_list->chains, ncap * sizeof(QSelChain));
            if (!nc) return false;
            out_list->chains = nc;
            out_list->cap    = ncap;
        }

        QSelChain *ch = &out_list->chains[out_list->count];
        if (parse_qsel_chain(s + pos, comma - pos, depth, ch)) {
            out_list->count++;
        } else {
            for (uint32_t k = 0u; k < ch->step_count; ++k) free_qsel_step(&ch->steps[k]);
            free(ch->steps);
        }
        pos = (comma < len) ? (comma + 1u) : len;
    }
    return out_list->count > 0u;
}

static bool qsel_mem_eq(const char *a, const char *b, size_t n, bool icase) {
    if (!icase) return memcmp(a, b, n) == 0;
    return arcr_str_eq_nocase_n(a, n, b, n);
}

static bool qsel_attr_value_matches(const QSelAttr *qa, const char *v) {
    if (qa->op == QSEL_ATTR_EXISTS) return true;
    size_t vlen = strlen(v);
    switch (qa->op) {
        case QSEL_ATTR_EQ:
            return vlen == qa->val_len && qsel_mem_eq(v, qa->val, vlen, qa->ignore_case);
        case QSEL_ATTR_PREFIX:
            if (qa->val_len == 0u || vlen < qa->val_len) return false;
            return qsel_mem_eq(v, qa->val, qa->val_len, qa->ignore_case);
        case QSEL_ATTR_SUFFIX:
            if (qa->val_len == 0u || vlen < qa->val_len) return false;
            return qsel_mem_eq(v + (vlen - qa->val_len), qa->val, qa->val_len, qa->ignore_case);
        case QSEL_ATTR_SUBSTR:
            if (qa->val_len == 0u || vlen < qa->val_len) return false;
            for (size_t i = 0u; i + qa->val_len <= vlen; ++i) {
                if (qsel_mem_eq(v + i, qa->val, qa->val_len, qa->ignore_case)) return true;
            }
            return false;
        case QSEL_ATTR_WORD: {
            if (qa->val_len == 0u) return false;
            size_t i = 0u;
            while (i < vlen) {
                while (i < vlen && is_ws_char(v[i])) ++i;
                size_t s = i;
                while (i < vlen && !is_ws_char(v[i])) ++i;
                if (i - s == qa->val_len && qsel_mem_eq(v + s, qa->val, qa->val_len, qa->ignore_case)) {
                    return true;
                }
            }
            return false;
        }
        case QSEL_ATTR_DASH:
            if (vlen < qa->val_len) return false;
            if (!qsel_mem_eq(v, qa->val, qa->val_len, qa->ignore_case)) return false;
            return vlen == qa->val_len || v[qa->val_len] == '-';
        default:
            return false;
    }
}

static bool qsel_has_focused_descendant(const ArcrDomNode *el) {
    const ArcrDomNode *cur = el;
    while (cur) {
        if ((cur->state_flags & ARCR_STATE_FOCUS) != 0u) return true;
        if (cur->first_child) {
            cur = cur->first_child;
        } else {
            while (cur && cur != el && !cur->next_sibling) cur = cur->parent;
            if (!cur || cur == el) break;
            cur = cur->next_sibling;
        }
    }
    return false;
}

static bool qsel_nth_matches(int32_t idx_1based, int32_t a, int32_t b) {
    if (a == 0) return idx_1based == b;
    int32_t diff = idx_1based - b;
    if ((a > 0 && diff < 0) || (a < 0 && diff > 0)) return false;
    return (diff % a) == 0;
}

static bool qsel_list_matches_el(const ArcrDocument *doc, const QSelList *list,
                                 const ArcrDomNode *el);
static bool qsel_has_list_matches_el(const ArcrDocument *doc, const QSelHasList *hlist,
                                     const ArcrDomNode *el);

static bool qsel_step_matches(const ArcrDocument *doc, const QSelStep *step,
                              const ArcrDomNode *el) {
    if (!el || el->kind != ARCR_NODE_ELEMENT || step->impossible) return false;
    if (step->req_root) {
        if (!el->parent || el->parent->kind != ARCR_NODE_DOCUMENT) return false;
    }
    if ((el->state_flags & step->req_state) != step->req_state) return false;
    if (step->req_focus_within && !qsel_has_focused_descendant(el)) return false;

    if (step->req_empty) {
        for (const ArcrDomNode *ch = el->first_child; ch != NULL; ch = ch->next_sibling) {
            if (ch->kind == ARCR_NODE_ELEMENT) return false;
            if (ch->kind == ARCR_NODE_TEXT && ch->text_len > 0u) return false;
        }
    }

    if (step->req_disabled || step->req_enabled) {
        bool has_dis = (arcr_dom_get_attr_cstr(doc, el, "disabled") != NULL);
        if (step->req_disabled && !has_dis) return false;
        if (step->req_enabled && has_dis)   return false;
    }

    if (step->req_first_child || step->req_only_child) {
        if (!el->parent) return false;
        for (const ArcrDomNode *p = el->prev_sibling; p != NULL; p = p->prev_sibling) {
            if (p->kind == ARCR_NODE_ELEMENT) return false;
        }
    }
    if (step->req_last_child || step->req_only_child) {
        if (!el->parent) return false;
        for (const ArcrDomNode *n = el->next_sibling; n != NULL; n = n->next_sibling) {
            if (n->kind == ARCR_NODE_ELEMENT) return false;
        }
    }
    if (step->req_first_of_type || step->req_only_of_type) {
        if (!el->parent) return false;
        for (const ArcrDomNode *p = el->prev_sibling; p != NULL; p = p->prev_sibling) {
            if (p->kind == ARCR_NODE_ELEMENT && p->tag == el->tag) return false;
        }
    }
    if (step->req_last_of_type || step->req_only_of_type) {
        if (!el->parent) return false;
        for (const ArcrDomNode *n = el->next_sibling; n != NULL; n = n->next_sibling) {
            if (n->kind == ARCR_NODE_ELEMENT && n->tag == el->tag) return false;
        }
    }
    if (step->has_nth) {
        if (!el->parent) return false;
        if (step->nth_of_list && !qsel_list_matches_el(doc, step->nth_of_list, el)) {
            return false;
        }
        int32_t idx = 1;
        if (step->nth_from_end) {
            for (const ArcrDomNode *n = el->next_sibling; n != NULL; n = n->next_sibling) {
                if (n->kind == ARCR_NODE_ELEMENT &&
                    (!step->nth_of_list || qsel_list_matches_el(doc, step->nth_of_list, n))) {
                    ++idx;
                }
            }
        } else {
            for (const ArcrDomNode *p = el->prev_sibling; p != NULL; p = p->prev_sibling) {
                if (p->kind == ARCR_NODE_ELEMENT &&
                    (!step->nth_of_list || qsel_list_matches_el(doc, step->nth_of_list, p))) {
                    ++idx;
                }
            }
        }
        if (!qsel_nth_matches(idx, step->nth_a, step->nth_b)) return false;
    }
    if (step->has_nth_of_type) {
        if (!el->parent) return false;
        int32_t idx = 1;
        if (step->nth_type_from_end) {
            for (const ArcrDomNode *n = el->next_sibling; n != NULL; n = n->next_sibling) {
                if (n->kind == ARCR_NODE_ELEMENT && n->tag == el->tag) ++idx;
            }
        } else {
            for (const ArcrDomNode *p = el->prev_sibling; p != NULL; p = p->prev_sibling) {
                if (p->kind == ARCR_NODE_ELEMENT && p->tag == el->tag) ++idx;
            }
        }
        if (!qsel_nth_matches(idx, step->nth_type_a, step->nth_type_b)) return false;
    }

    if (step->tag_len > 0u) {
        const char *tn = arcr_atom_cstr(doc->atoms, el->tag);
        if (!arcr_str_eq_nocase_n(tn, strlen(tn), step->tag, step->tag_len)) return false;
    }
    if (step->id_len > 0u) {
        if (el->id == ARCR_ATOM_NULL) return false;
        const char *id_s = arcr_atom_cstr(doc->atoms, el->id);
        if (strlen(id_s) != step->id_len || memcmp(id_s, step->id, step->id_len) != 0) {
            return false;
        }
    }
    for (uint32_t c = 0u; c < step->class_count; ++c) {
        bool found = false;
        for (uint32_t ec = 0u; ec < el->class_count; ++ec) {
            const char *cs = arcr_atom_cstr(doc->atoms, dom_class_at(el, ec));
            if (strlen(cs) == step->class_lens[c] &&
                memcmp(cs, step->classes[c], step->class_lens[c]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    for (uint32_t a = 0u; a < step->attr_count; ++a) {
        const QSelAttr *qa = &step->attrs[a];
        const char *found_val = NULL;
        for (const ArcrAttr *at = el->first_attr; at != NULL; at = at->next) {
            const char *an = arcr_atom_cstr(doc->atoms, at->name);
            if (arcr_str_eq_nocase_n(an, strlen(an), qa->name, qa->name_len)) {
                found_val = at->value ? at->value : "";
                break;
            }
        }
        if (!found_val || !qsel_attr_value_matches(qa, found_val)) return false;
    }

    for (uint32_t k = 0u; k < step->not_count; ++k) {
        if (qsel_list_matches_el(doc, step->not_lists[k], el)) return false;
    }
    for (uint32_t k = 0u; k < step->is_count; ++k) {
        if (!qsel_list_matches_el(doc, step->is_lists[k], el)) return false;
    }
    for (uint32_t k = 0u; k < step->has_count; ++k) {
        if (!qsel_has_list_matches_el(doc, step->has_lists[k], el)) return false;
    }
    return true;
}

static bool qsel_anchor_relation_ok(const ArcrDomNode *anchor, QSelCombinator rel_comb,
                                    const ArcrDomNode *first_matched) {
    if (!anchor) return true;
    if (rel_comb == QSEL_COMB_CHILD) {
        return first_matched->parent == anchor;
    }
    if (rel_comb == QSEL_COMB_ADJACENT) {
        for (const ArcrDomNode *p = first_matched->prev_sibling; p != NULL; p = p->prev_sibling) {
            if (p->kind == ARCR_NODE_ELEMENT) return p == anchor;
        }
        return false;
    }
    if (rel_comb == QSEL_COMB_SIBLING) {
        for (const ArcrDomNode *p = first_matched->prev_sibling; p != NULL; p = p->prev_sibling) {
            if (p == anchor) return true;
        }
        return false;
    }
    for (const ArcrDomNode *anc = first_matched->parent; anc != NULL; anc = anc->parent) {
        if (anc == anchor) return true;
    }
    return false;
}

static bool qsel_chain_matches_at_rel(const ArcrDocument *doc, const QSelChain *chain,
                                      uint32_t idx, const ArcrDomNode *el,
                                      const ArcrDomNode *anchor, QSelCombinator rel_comb) {
    if (!qsel_step_matches(doc, &chain->steps[idx], el)) return false;
    if (idx == 0u) return qsel_anchor_relation_ok(anchor, rel_comb, el);

    QSelCombinator comb = chain->steps[idx - 1u].comb_to_next;
    if (comb == QSEL_COMB_CHILD) {
        if (!el->parent || el->parent->kind != ARCR_NODE_ELEMENT) return false;
        return qsel_chain_matches_at_rel(doc, chain, idx - 1u, el->parent, anchor, rel_comb);
    }
    if (comb == QSEL_COMB_ADJACENT) {
        for (const ArcrDomNode *p = el->prev_sibling; p != NULL; p = p->prev_sibling) {
            if (p->kind == ARCR_NODE_ELEMENT) {
                return qsel_chain_matches_at_rel(doc, chain, idx - 1u, p, anchor, rel_comb);
            }
        }
        return false;
    }
    if (comb == QSEL_COMB_SIBLING) {
        for (const ArcrDomNode *p = el->prev_sibling; p != NULL; p = p->prev_sibling) {
            if (p->kind == ARCR_NODE_ELEMENT &&
                qsel_chain_matches_at_rel(doc, chain, idx - 1u, p, anchor, rel_comb)) {
                return true;
            }
        }
        return false;
    }
    for (const ArcrDomNode *anc = el->parent; anc != NULL; anc = anc->parent) {
        if (anc->kind == ARCR_NODE_ELEMENT &&
            qsel_chain_matches_at_rel(doc, chain, idx - 1u, anc, anchor, rel_comb)) {
            return true;
        }
    }
    return false;
}

static bool qsel_rel_chain_matches_subtree(const ArcrDocument *doc, const QSelRelChain *rel,
                                           const ArcrDomNode *anchor,
                                           const ArcrDomNode *sub_root) {
    const ArcrDomNode *cur = sub_root;
    while (cur) {
        if (cur->kind == ARCR_NODE_ELEMENT &&
            qsel_chain_matches_at_rel(doc, &rel->chain, rel->chain.step_count - 1u,
                                      cur, anchor, rel->rel_comb)) {
            return true;
        }
        if (cur->first_child) {
            cur = cur->first_child;
        } else {
            while (cur && cur != sub_root && !cur->next_sibling) cur = cur->parent;
            if (!cur || cur == sub_root) break;
            cur = cur->next_sibling;
        }
    }
    return false;
}

static bool qsel_has_list_matches_el(const ArcrDocument *doc, const QSelHasList *hlist,
                                     const ArcrDomNode *el) {
    if (!hlist || !el || el->kind != ARCR_NODE_ELEMENT) return false;
    for (size_t g = 0u; g < hlist->count; ++g) {
        const QSelRelChain *rel = &hlist->items[g];
        if (!rel->chain.valid || rel->chain.step_count == 0u) continue;

        if (rel->rel_comb == QSEL_COMB_CHILD && rel->chain.step_count == 1u) {
            for (const ArcrDomNode *ch = el->first_child; ch != NULL; ch = ch->next_sibling) {
                if (ch->kind == ARCR_NODE_ELEMENT &&
                    qsel_step_matches(doc, &rel->chain.steps[0], ch)) {
                    return true;
                }
            }
        } else if (rel->rel_comb == QSEL_COMB_CHILD || rel->rel_comb == QSEL_COMB_DESCENDANT) {
            for (const ArcrDomNode *ch = el->first_child; ch != NULL; ch = ch->next_sibling) {
                if (qsel_rel_chain_matches_subtree(doc, rel, el, ch)) return true;
            }
        } else {
            for (const ArcrDomNode *sib = el->next_sibling; sib != NULL; sib = sib->next_sibling) {
                if (sib->kind != ARCR_NODE_ELEMENT) continue;
                if (qsel_rel_chain_matches_subtree(doc, rel, el, sib)) return true;
                if (rel->rel_comb == QSEL_COMB_ADJACENT && rel->chain.step_count == 1u) break;
            }
        }
    }
    return false;
}

static bool qsel_list_matches_el(const ArcrDocument *doc, const QSelList *list,
                                 const ArcrDomNode *el) {
    if (!list || !el || el->kind != ARCR_NODE_ELEMENT) return false;
    for (size_t g = 0u; g < list->count; ++g) {
        if (list->chains[g].valid &&
            qsel_chain_matches_at_rel(doc, &list->chains[g],
                                      list->chains[g].step_count - 1u, el,
                                      NULL, QSEL_COMB_NONE)) {
            return true;
        }
    }
    return false;
}

bool arcr_dom_matches_selector(const ArcrDocument *doc, const ArcrDomNode *elem,
                               const char *selector_cstr) {
    if (!doc || !elem || elem->kind != ARCR_NODE_ELEMENT ||
        !selector_cstr || selector_cstr[0] == '\0') {
        return false;
    }
    QSelList list;
    if (!parse_qsel_list_str(selector_cstr, strlen(selector_cstr), 0, &list)) {
        free_qsel_list(&list);
        return false;
    }
    bool matched = qsel_list_matches_el(doc, &list, elem);
    free_qsel_list(&list);
    return matched;
}

size_t arcr_dom_query_selector_all(const ArcrDocument *doc, ArcrDomNode *scope_root,
                                   const char *selector_cstr,
                                   ArcrDomNode **out_nodes, size_t max_out) {
    if (!doc || !selector_cstr || selector_cstr[0] == '\0') return 0u;
    ArcrDomNode *root = scope_root ? scope_root : doc->root;
    if (!root) return 0u;

    QSelList list;
    if (!parse_qsel_list_str(selector_cstr, strlen(selector_cstr), 0, &list)) {
        free_qsel_list(&list);
        return 0u;
    }

    size_t found = 0u;
    ArcrDomNode *cur = root->first_child;
    while (cur) {
        if (cur->kind == ARCR_NODE_ELEMENT && qsel_list_matches_el(doc, &list, cur)) {
            if (out_nodes && max_out > 0u) {
                out_nodes[found++] = cur;
                if (found >= max_out) break;
            } else {
                found++;
            }
        }
        if (cur->first_child) {
            cur = cur->first_child;
        } else {
            while (cur && cur != root && !cur->next_sibling) {
                cur = cur->parent;
            }
            if (!cur || cur == root) break;
            cur = cur->next_sibling;
        }
    }

    free_qsel_list(&list);
    return found;
}

void arcr_dom_append_style_block(ArcrDocument *doc, const char *css, size_t css_len) {
    if (!doc || !css || css_len == 0u) return;
    ArcrStyleBlock *sb = (ArcrStyleBlock *)arcr_arena_alloc(
        &doc->arena, sizeof(ArcrStyleBlock), sizeof(void *));
    if (!sb) return;
    sb->css_text = arcr_arena_strdup(&doc->arena, css, css_len);
    if (!sb->css_text) return;
    sb->css_len = css_len;
    sb->next    = NULL;

    if (!doc->style_blocks_head) {
        doc->style_blocks_head = sb;
        doc->style_blocks_tail = sb;
    } else {
        doc->style_blocks_tail->next = sb;
        doc->style_blocks_tail       = sb;
    }
    doc->dom_dirty = true;
}
