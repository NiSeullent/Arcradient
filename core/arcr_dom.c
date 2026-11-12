/*
 * Arcradient (MSHTML-Next)
 * core/arcr_dom.c - Arena/Slab DOM Node Lifecycle & Bloom Filter Indexing
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
    for (uint32_t i = 0; i < n->class_count; ++i) {
        bits |= arcr_bloom_bits_for_atom(n->classes[i]);
    }
    return bits;
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
    root->kind = ARCR_NODE_DOCUMENT;
    doc->root  = root;
    doc->node_count = 1u;
    return doc;
}

void arcr_dom_document_destroy(ArcrDocument *doc) {
    if (!doc) return;
    /* Instant O(blocks) release of entire DOM tree, attributes, text & style blocks! */
    arcr_arena_destroy(&doc->arena);
    free(doc);
}

ArcrDomNode *arcr_dom_create_element(ArcrDocument *doc, const char *tag_name, size_t tag_len) {
    if (!doc || !tag_name) return NULL;
    ArcrDomNode *node = (ArcrDomNode *)arcr_slab_alloc(&doc->node_slab);
    if (!node) return NULL;
    node->kind = ARCR_NODE_ELEMENT;
    node->tag  = arcr_atom_intern_lower(doc->atoms, tag_name, tag_len);
    doc->node_count++;

    if (arcr_str_eq_nocase_n(tag_name, tag_len, "script", 6u)) {
        doc->has_scripts = true;
    } else if (arcr_str_eq_nocase_n(tag_name, tag_len, "video", 5u) ||
               arcr_str_eq_nocase_n(tag_name, tag_len, "audio", 5u)) {
        doc->has_media = true;
    } else if (arcr_str_eq_nocase_n(tag_name, tag_len, "img", 3u)) {
        doc->has_images = true;
    }
    return node;
}

ArcrDomNode *arcr_dom_create_text(ArcrDocument *doc, const char *utf8, size_t len) {
    if (!doc) return NULL;
    ArcrDomNode *node = (ArcrDomNode *)arcr_slab_alloc(&doc->node_slab);
    if (!node) return NULL;
    node->kind     = ARCR_NODE_TEXT;
    node->text     = arcr_arena_strdup(&doc->arena, utf8, len);
    node->text_len = len;
    doc->node_count++;
    return node;
}

void arcr_dom_append_child(ArcrDomNode *parent, ArcrDomNode *child) {
    if (!parent || !child) return;
    child->parent       = parent;
    child->next_sibling = NULL;
    child->ancestor_bloom = parent->ancestor_bloom | node_self_bloom(parent);

    if (!parent->first_child) {
        parent->first_child = child;
        parent->last_child  = child;
    } else {
        parent->last_child->next_sibling = child;
        parent->last_child               = child;
    }
}

static void parse_class_list(ArcrDocument *doc, ArcrDomNode *elem, const char *val, size_t len) {
    size_t i = 0u;
    while (i < len && elem->class_count < ARCR_MAX_INLINE_CLASSES) {
        while (i < len && (val[i] == ' ' || val[i] == '\t' || val[i] == '\n' || val[i] == '\r')) {
            ++i;
        }
        size_t start = i;
        while (i < len && val[i] != ' ' && val[i] != '\t' && val[i] != '\n' && val[i] != '\r') {
            ++i;
        }
        if (i > start) {
            elem->classes[elem->class_count++] =
                arcr_atom_intern(doc->atoms, val + start, i - start);
        }
    }
}

void arcr_dom_set_attr(ArcrDocument *doc, ArcrDomNode *elem,
                       const char *name, size_t name_len,
                       const char *val, size_t val_len) {
    if (!doc || !elem || elem->kind != ARCR_NODE_ELEMENT || !name || name_len == 0u) return;
    ArcrAtom name_atom = arcr_atom_intern_lower(doc->atoms, name, name_len);
    char    *stored    = arcr_arena_strdup(&doc->arena, val ? val : "", val_len);
    if (!stored) return;

    ArcrAttr *attr = (ArcrAttr *)arcr_arena_alloc(&doc->arena, sizeof(ArcrAttr), sizeof(void *));
    if (!attr) return;
    attr->name       = name_atom;
    attr->value      = stored;
    attr->next       = elem->first_attr;
    elem->first_attr = attr;

    if (arcr_str_eq_nocase_n(name, name_len, "id", 2u) && val_len > 0u) {
        elem->id = arcr_atom_intern(doc->atoms, val, val_len);
    } else if (arcr_str_eq_nocase_n(name, name_len, "class", 5u) && val_len > 0u) {
        parse_class_list(doc, elem, val, val_len);
    } else if (arcr_str_starts_with_nocase(arcr_atom_cstr(doc->atoms, name_atom), "on")) {
        /* Inline event handler like onclick="..." requires JS module */
        doc->has_scripts = true;
    }
}

const char *arcr_dom_get_attr(const ArcrDomNode *elem, ArcrAtom name_atom) {
    if (!elem || name_atom == ARCR_ATOM_NULL) return NULL;
    for (const ArcrAttr *a = elem->first_attr; a != NULL; a = a->next) {
        if (a->name == name_atom) return a->value;
    }
    return NULL;
}

bool arcr_dom_has_class(const ArcrDomNode *elem, ArcrAtom class_atom) {
    if (!elem || class_atom == ARCR_ATOM_NULL) return false;
    for (uint32_t i = 0; i < elem->class_count; ++i) {
        if (elem->classes[i] == class_atom) return true;
    }
    return false;
}
