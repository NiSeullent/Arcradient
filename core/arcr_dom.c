/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * core/arcr_dom.c - Arena/Slab DOM Lifecycle, Bloom Indexing, Surfaces & Selector Queries
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

static uint64_t node_self_bloom(const ArcrDomNode *n) {
    if (!n || n->kind != ARCR_NODE_ELEMENT) return 0ULL;
    uint64_t bits = arcr_bloom_bits_for_atom(n->tag);
    if (n->id != ARCR_ATOM_NULL) {
        bits |= arcr_bloom_bits_for_atom(n->id);
    }
    for (uint32_t i = 0u; i < n->class_count; ++i) {
        bits |= arcr_bloom_bits_for_atom(n->classes[i]);
    }
    return bits;
}

static void recompute_subtree_bloom(ArcrDomNode *node) {
    if (!node) return;
    if (node->parent) {
        node->ancestor_bloom = node->parent->ancestor_bloom | node_self_bloom(node->parent);
    } else {
        node->ancestor_bloom = 0ULL;
    }
    for (ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        recompute_subtree_bloom(ch);
    }
}

static void recompute_children_bloom(ArcrDomNode *elem) {
    if (!elem) return;
    for (ArcrDomNode *ch = elem->first_child; ch != NULL; ch = ch->next_sibling) {
        recompute_subtree_bloom(ch);
    }
}

static void free_surfaces_in_tree(ArcrDomNode *node) {
    if (!node) return;
    if (node->surface && node->surface->heap_owned && node->surface->argb_pixels) {
        free(node->surface->argb_pixels);
        node->surface->argb_pixels = NULL;
        node->surface->heap_owned  = false;
    }
    for (ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        free_surfaces_in_tree(ch);
    }
}

static void destroy_subtree_nodes(ArcrDocument *doc, ArcrDomNode *node) {
    if (!doc || !node) return;
    ArcrDomNode *ch = node->first_child;
    while (ch) {
        ArcrDomNode *next = ch->next_sibling;
        destroy_subtree_nodes(doc, ch);
        ch = next;
    }
    if (node->surface && node->surface->heap_owned && node->surface->argb_pixels) {
        free(node->surface->argb_pixels);
        node->surface->argb_pixels = NULL;
        node->surface->heap_owned  = false;
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
    ArcrDomNode *prev = NULL;
    for (ArcrDomNode *cur = p->first_child; cur != NULL; cur = cur->next_sibling) {
        if (cur == node) {
            if (prev) {
                prev->next_sibling = cur->next_sibling;
            } else {
                p->first_child = cur->next_sibling;
            }
            if (p->last_child == cur) {
                p->last_child = prev;
            }
            break;
        }
        prev = cur;
    }
    node->parent       = NULL;
    node->next_sibling = NULL;
}

ArcrDocument *arcr_dom_document_create(ArcrAtomTable *atoms) {
    if (!atoms) return NULL;
    ArcrDocument *doc = (ArcrDocument *)calloc(1u, sizeof(ArcrDocument));
    if (!doc) return NULL;

    arcr_arena_init(&doc->arena, 32768u);
    arcr_slab_init(&doc->node_slab, &doc->arena, sizeof(ArcrDomNode), sizeof(void *));
    doc->atoms = atoms;
    doc->title = "";

    ArcrDomNode *root = (ArcrDomNode *)arcr_slab_alloc(&doc->node_slab);
    if (!root) {
        arcr_arena_destroy(&doc->arena);
        free(doc);
        return NULL;
    }
    memset(root, 0, sizeof(*root));
    root->kind     = ARCR_NODE_DOCUMENT;
    root->node_uid = ++doc->next_uid;
    doc->root      = root;
    doc->node_count = 1u;
    return doc;
}

void arcr_dom_document_destroy(ArcrDocument *doc) {
    if (!doc) return;
    /* Release all heap-owned image/canvas pixel buffers before tearing down arena */
    free_surfaces_in_tree(doc->root);
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
               arcr_str_eq_nocase_n(tag_name, tag_len, "canvas", 6u)) {
        doc->has_images = true;
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

void arcr_dom_append_child(ArcrDomNode *parent, ArcrDomNode *child) {
    if (!parent || !child || parent == child) return;
    if (child->parent) {
        unlink_from_parent(child);
    }
    child->parent       = parent;
    child->next_sibling = NULL;

    if (!parent->first_child) {
        parent->first_child = child;
        parent->last_child  = child;
    } else {
        parent->last_child->next_sibling = child;
        parent->last_child               = child;
    }
    recompute_subtree_bloom(child);
}

void arcr_dom_remove_node(ArcrDocument *doc, ArcrDomNode *node) {
    if (!doc || !node || node == doc->root) return;
    unlink_from_parent(node);
    destroy_subtree_nodes(doc, node);
    doc->dom_dirty = true;
}

void arcr_dom_clear_children(ArcrDocument *doc, ArcrDomNode *elem) {
    if (!doc || !elem) return;
    ArcrDomNode *ch = elem->first_child;
    elem->first_child = NULL;
    elem->last_child  = NULL;
    while (ch) {
        ArcrDomNode *next = ch->next_sibling;
        ch->parent       = NULL;
        ch->next_sibling = NULL;
        destroy_subtree_nodes(doc, ch);
        ch = next;
    }
    doc->dom_dirty = true;
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
                if (arcr_str_eq_nocase(tname, "title")) {
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
            size_t avail = (cap - 1u) - *inout_written;
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

static bool is_ws_char(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static void parse_class_list(ArcrDocument *doc, ArcrDomNode *elem, const char *val, size_t len) {
    elem->class_count = 0u;
    size_t i = 0u;
    while (i < len && elem->class_count < ARCR_MAX_INLINE_CLASSES) {
        while (i < len && is_ws_char(val[i])) ++i;
        size_t start = i;
        while (i < len && !is_ws_char(val[i])) ++i;
        if (i > start) {
            ArcrAtom ca = arcr_atom_intern(doc->atoms, val + start, i - start);
            if (!arcr_dom_has_class(elem, ca)) {
                elem->classes[elem->class_count++] = ca;
            }
        }
    }
}

static void upsert_raw_attr(ArcrDocument *doc, ArcrDomNode *elem,
                            ArcrAtom name_atom, const char *stored_val) {
    for (ArcrAttr *a = elem->first_attr; a != NULL; a = a->next) {
        if (a->name == name_atom) {
            a->value = stored_val;
            return;
        }
    }
    ArcrAttr *attr = (ArcrAttr *)arcr_arena_alloc(&doc->arena, sizeof(ArcrAttr), sizeof(void *));
    if (!attr) return;
    attr->name       = name_atom;
    attr->value      = stored_val;
    attr->next       = elem->first_attr;
    elem->first_attr = attr;
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
        /* Inline event handler like onclick="..." requires JS module */
        doc->has_scripts = true;
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
    for (uint32_t i = 0u; i < elem->class_count; ++i) {
        if (elem->classes[i] == class_atom) return true;
    }
    return false;
}

static void sync_class_attr_string(ArcrDocument *doc, ArcrDomNode *elem) {
    char buf[512];
    size_t pos = 0u;
    for (uint32_t i = 0u; i < elem->class_count; ++i) {
        const char *cs = arcr_atom_cstr(doc->atoms, elem->classes[i]);
        size_t cl = strlen(cs);
        if (cl == 0u) continue;
        if (pos > 0u && pos + 1u < sizeof(buf)) {
            buf[pos++] = ' ';
        }
        if (pos + cl < sizeof(buf)) {
            memcpy(buf + pos, cs, cl);
            pos += cl;
        }
    }
    buf[pos] = '\0';
    ArcrAtom cls_atom = arcr_atom_intern_lower(doc->atoms, "class", 5u);
    char *stored = arcr_arena_strdup(&doc->arena, buf, pos);
    if (stored) {
        upsert_raw_attr(doc, elem, cls_atom, stored);
    }
}

void arcr_dom_add_class(ArcrDocument *doc, ArcrDomNode *elem,
                        const char *cls, size_t cls_len) {
    if (!doc || !elem || elem->kind != ARCR_NODE_ELEMENT || !cls) return;
    arcr_str_trim_view(&cls, &cls_len);
    if (cls_len == 0u) return;

    ArcrAtom ca = arcr_atom_intern(doc->atoms, cls, cls_len);
    if (arcr_dom_has_class(elem, ca)) return;
    if (elem->class_count >= ARCR_MAX_INLINE_CLASSES) return;

    elem->classes[elem->class_count++] = ca;
    sync_class_attr_string(doc, elem);
    recompute_children_bloom(elem);
    doc->dom_dirty = true;
}

void arcr_dom_remove_class(ArcrDomNode *elem, ArcrAtom class_atom) {
    if (!elem || elem->kind != ARCR_NODE_ELEMENT || class_atom == ARCR_ATOM_NULL) return;
    uint32_t out_idx = 0u;
    bool changed = false;
    for (uint32_t i = 0u; i < elem->class_count; ++i) {
        if (elem->classes[i] == class_atom) {
            changed = true;
        } else {
            elem->classes[out_idx++] = elem->classes[i];
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

ArcrDomNode *arcr_dom_find_by_uid(ArcrDomNode *root, uint32_t uid) {
    if (!root || uid == 0u) return NULL;
    if (root->node_uid == uid) return root;
    for (ArcrDomNode *ch = root->first_child; ch != NULL; ch = ch->next_sibling) {
        ArcrDomNode *hit = arcr_dom_find_by_uid(ch, uid);
        if (hit) return hit;
    }
    return NULL;
}

static ArcrDomNode *find_id_rec(const ArcrDocument *doc, ArcrDomNode *node, const char *id_str) {
    if (!node) return NULL;
    if (node->kind == ARCR_NODE_ELEMENT && node->id != ARCR_ATOM_NULL) {
        if (strcmp(arcr_atom_cstr(doc->atoms, node->id), id_str) == 0) {
            return node;
        }
    }
    for (ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        ArcrDomNode *hit = find_id_rec(doc, ch, id_str);
        if (hit) return hit;
    }
    return NULL;
}

ArcrDomNode *arcr_dom_get_element_by_id(const ArcrDocument *doc, const char *id_str) {
    if (!doc || !doc->root || !id_str || id_str[0] == '\0') return NULL;
    return find_id_rec(doc, doc->root, id_str);
}

/* -------------------------------------------------------------------------
 * Zero-Allocation CSS Selector Parser & Matcher for querySelectorAll
 * ------------------------------------------------------------------------- */
#define ARCR_QSEL_MAX_STEPS   8u
#define ARCR_QSEL_MAX_CLASSES 4u
#define ARCR_QSEL_MAX_ATTRS   3u
#define ARCR_QSEL_MAX_GROUPS  16u

typedef enum QSelCombinator {
    QSEL_COMB_NONE       = 0,
    QSEL_COMB_DESCENDANT = 1,
    QSEL_COMB_CHILD      = 2
} QSelCombinator;

typedef struct QSelAttr {
    const char *name;
    size_t      name_len;
    const char *val;
    size_t      val_len;
    bool        has_val;
} QSelAttr;

typedef struct QSelStep {
    const char    *tag;
    size_t         tag_len;
    const char    *id;
    size_t         id_len;
    const char    *classes[ARCR_QSEL_MAX_CLASSES];
    size_t         class_lens[ARCR_QSEL_MAX_CLASSES];
    uint32_t       class_count;
    QSelAttr       attrs[ARCR_QSEL_MAX_ATTRS];
    uint32_t       attr_count;
    uint32_t       req_state;
    bool           req_root;
    bool           impossible;
    QSelCombinator comb_to_next; /* Combinator linking this step to step + 1 */
} QSelStep;

typedef struct QSelChain {
    QSelStep steps[ARCR_QSEL_MAX_STEPS];
    uint32_t step_count;
    bool     valid;
} QSelChain;

static bool is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           ((unsigned char)c >= 0x80u);
}

static bool parse_qsel_compound(const char *s, size_t len, QSelStep *out) {
    memset(out, 0, sizeof(*out));
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;

    size_t i = 0u;
    if (s[0] == '*') {
        ++i;
    } else if (is_ident_char(s[0])) {
        size_t start = 0u;
        while (i < len && is_ident_char(s[i])) ++i;
        out->tag     = s + start;
        out->tag_len = i - start;
    }

    while (i < len) {
        char c = s[i];
        if (c == '#') {
            ++i;
            size_t start = i;
            while (i < len && is_ident_char(s[i])) ++i;
            if (i == start) return false;
            out->id     = s + start;
            out->id_len = i - start;
        } else if (c == '.') {
            ++i;
            size_t start = i;
            while (i < len && is_ident_char(s[i])) ++i;
            if (i == start) return false;
            if (out->class_count < ARCR_QSEL_MAX_CLASSES) {
                out->classes[out->class_count]    = s + start;
                out->class_lens[out->class_count] = i - start;
                out->class_count++;
            }
        } else if (c == '[') {
            ++i;
            size_t close_b = i;
            while (close_b < len && s[close_b] != ']') ++close_b;
            if (close_b >= len) return false;

            const char *inner = s + i;
            size_t ilen = close_b - i;
            arcr_str_trim_view(&inner, &ilen);

            size_t eq = 0u;
            while (eq < ilen && inner[eq] != '=') ++eq;
            if (out->attr_count < ARCR_QSEL_MAX_ATTRS && ilen > 0u) {
                QSelAttr *qa = &out->attrs[out->attr_count++];
                qa->name     = inner;
                qa->name_len = eq;
                arcr_str_trim_view(&qa->name, &qa->name_len);
                if (eq < ilen) {
                    qa->has_val = true;
                    const char *vp = inner + eq + 1u;
                    size_t vl = ilen - eq - 1u;
                    arcr_str_trim_view(&vp, &vl);
                    if (vl >= 2u && ((vp[0] == '"' && vp[vl - 1u] == '"') ||
                                     (vp[0] == '\'' && vp[vl - 1u] == '\''))) {
                        vp += 1u;
                        vl -= 2u;
                    }
                    qa->val     = vp;
                    qa->val_len = vl;
                }
            }
            i = close_b + 1u;
        } else if (c == ':') {
            ++i;
            if (i < len && s[i] == ':') {
                out->impossible = true;
                return true;
            }
            size_t start = i;
            while (i < len && is_ident_char(s[i])) ++i;
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
            if (arcr_str_eq_nocase_n(ps, plen, "hover", 5u))        out->req_state |= ARCR_STATE_HOVER;
            else if (arcr_str_eq_nocase_n(ps, plen, "focus", 5u))   out->req_state |= ARCR_STATE_FOCUS;
            else if (arcr_str_eq_nocase_n(ps, plen, "active", 6u))  out->req_state |= ARCR_STATE_ACTIVE;
            else if (arcr_str_eq_nocase_n(ps, plen, "checked", 7u)) out->req_state |= ARCR_STATE_CHECKED;
            else if (arcr_str_eq_nocase_n(ps, plen, "visited", 7u)) out->req_state |= ARCR_STATE_VISITED;
            else if (arcr_str_eq_nocase_n(ps, plen, "root", 4u))    out->req_root = true;
            else out->impossible = true;
        } else {
            return false;
        }
    }
    return true;
}

static bool parse_qsel_chain(const char *s, size_t len, QSelChain *out) {
    memset(out, 0, sizeof(*out));
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;

    size_t i = 0u;
    while (i < len && out->step_count < ARCR_QSEL_MAX_STEPS) {
        while (i < len && is_ws_char(s[i])) ++i;
        if (i >= len) break;

        size_t start = i;
        int bracket = 0;
        while (i < len) {
            if (s[i] == '[') ++bracket;
            else if (s[i] == ']' && bracket > 0) --bracket;
            else if (bracket == 0 && (is_ws_char(s[i]) || s[i] == '>')) break;
            ++i;
        }
        if (i == start) return false;
        if (!parse_qsel_compound(s + start, i - start, &out->steps[out->step_count])) {
            return false;
        }

        QSelCombinator comb = QSEL_COMB_NONE;
        bool saw_ws = false;
        while (i < len && is_ws_char(s[i])) {
            saw_ws = true;
            ++i;
        }
        if (i < len && s[i] == '>') {
            comb = QSEL_COMB_CHILD;
            ++i;
            while (i < len && is_ws_char(s[i])) ++i;
        } else if (saw_ws && i < len) {
            comb = QSEL_COMB_DESCENDANT;
        }
        out->steps[out->step_count].comb_to_next = comb;
        out->step_count++;
    }
    out->valid = (out->step_count > 0u);
    return out->valid;
}

static bool qsel_step_matches(const ArcrDocument *doc, const QSelStep *step,
                              const ArcrDomNode *el) {
    if (!el || el->kind != ARCR_NODE_ELEMENT || step->impossible) return false;
    if (step->req_root) {
        if (!el->parent || el->parent->kind != ARCR_NODE_DOCUMENT) return false;
    }
    if ((el->state_flags & step->req_state) != step->req_state) return false;

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
            const char *cs = arcr_atom_cstr(doc->atoms, el->classes[ec]);
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
                found_val = at->value;
                break;
            }
        }
        if (!found_val) return false;
        if (qa->has_val) {
            if (strlen(found_val) != qa->val_len ||
                memcmp(found_val, qa->val, qa->val_len) != 0) {
                return false;
            }
        }
    }
    return true;
}

static bool qsel_chain_matches_at(const ArcrDocument *doc, const QSelChain *chain,
                                  uint32_t idx, const ArcrDomNode *el) {
    if (!qsel_step_matches(doc, &chain->steps[idx], el)) return false;
    if (idx == 0u) return true;

    QSelCombinator comb = chain->steps[idx - 1u].comb_to_next;
    if (comb == QSEL_COMB_CHILD) {
        if (!el->parent || el->parent->kind != ARCR_NODE_ELEMENT) return false;
        return qsel_chain_matches_at(doc, chain, idx - 1u, el->parent);
    }
    for (const ArcrDomNode *anc = el->parent; anc != NULL; anc = anc->parent) {
        if (anc->kind == ARCR_NODE_ELEMENT &&
            qsel_chain_matches_at(doc, chain, idx - 1u, anc)) {
            return true;
        }
    }
    return false;
}

static void query_walk_rec(const ArcrDocument *doc, ArcrDomNode *node, bool test_self,
                           const QSelChain *chains, size_t chain_count,
                           ArcrDomNode **out_nodes, size_t max_out, size_t *inout_count) {
    if (!node) return;
    if (out_nodes && max_out > 0u && *inout_count >= max_out) return;

    if (test_self && node->kind == ARCR_NODE_ELEMENT) {
        for (size_t g = 0u; g < chain_count; ++g) {
            if (chains[g].valid &&
                qsel_chain_matches_at(doc, &chains[g], chains[g].step_count - 1u, node)) {
                if (out_nodes && max_out > 0u) {
                    if (*inout_count < max_out) {
                        out_nodes[*inout_count] = node;
                        (*inout_count)++;
                    }
                } else {
                    (*inout_count)++;
                }
                break;
            }
        }
    }

    for (ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
        query_walk_rec(doc, ch, true, chains, chain_count, out_nodes, max_out, inout_count);
        if (out_nodes && max_out > 0u && *inout_count >= max_out) return;
    }
}

size_t arcr_dom_query_selector_all(const ArcrDocument *doc, ArcrDomNode *scope_root,
                                   const char *selector_cstr,
                                   ArcrDomNode **out_nodes, size_t max_out) {
    if (!doc || !selector_cstr || selector_cstr[0] == '\0') return 0u;
    ArcrDomNode *root = scope_root ? scope_root : doc->root;
    if (!root) return 0u;

    QSelChain chains[ARCR_QSEL_MAX_GROUPS];
    size_t    chain_count = 0u;
    size_t    slen        = strlen(selector_cstr);
    size_t    pos         = 0u;

    while (pos < slen && chain_count < ARCR_QSEL_MAX_GROUPS) {
        size_t comma = pos;
        int bracket = 0;
        int paren   = 0;
        while (comma < slen) {
            if (selector_cstr[comma] == '[') ++bracket;
            else if (selector_cstr[comma] == ']' && bracket > 0) --bracket;
            else if (selector_cstr[comma] == '(') ++paren;
            else if (selector_cstr[comma] == ')' && paren > 0) --paren;
            else if (selector_cstr[comma] == ',' && bracket == 0 && paren == 0) break;
            ++comma;
        }
        if (parse_qsel_chain(selector_cstr + pos, comma - pos, &chains[chain_count])) {
            chain_count++;
        }
        pos = (comma < slen) ? (comma + 1u) : slen;
    }
    if (chain_count == 0u) return 0u;

    size_t found = 0u;
    query_walk_rec(doc, root, false, chains, chain_count, out_nodes, max_out, &found);
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
