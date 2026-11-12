/*
 * Arcradient (MSHTML-Next)
 * core/arcr_dom.h - Compact Arena-Backed DOM & Error-Recovering HTML5 Parser
 */
#ifndef ARCR_DOM_H
#define ARCR_DOM_H

#include "../base/arcr_base.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARCR_MAX_INLINE_CLASSES 8u

typedef enum ArcrNodeKind {
    ARCR_NODE_DOCUMENT = 0,
    ARCR_NODE_ELEMENT  = 1,
    ARCR_NODE_TEXT     = 2,
    ARCR_NODE_COMMENT  = 3
} ArcrNodeKind;

typedef struct ArcrAttr {
    ArcrAtom         name;
    const char      *value;
    struct ArcrAttr *next;
} ArcrAttr;

struct ArcrComputedStyle;

typedef struct ArcrDomNode {
    ArcrNodeKind               kind;
    ArcrAtom                   tag;      /* Lowercase tag atom for elements */
    ArcrAtom                   id;       /* id="..." atom if present */
    uint32_t                   class_count;
    ArcrAtom                   classes[ARCR_MAX_INLINE_CLASSES];
    uint64_t                   ancestor_bloom; /* 64-bit Bloom hash of ancestor tags/classes/ids */
    ArcrAttr                  *first_attr;
    const char                *text;     /* UTF-8 text for TEXT / COMMENT / raw <script>/<style> */
    size_t                     text_len;

    struct ArcrDomNode        *parent;
    struct ArcrDomNode        *first_child;
    struct ArcrDomNode        *last_child;
    struct ArcrDomNode        *next_sibling;

    const struct ArcrComputedStyle *computed_style;
} ArcrDomNode;

typedef struct ArcrStyleBlock {
    const char            *css_text;
    size_t                 css_len;
    struct ArcrStyleBlock *next;
} ArcrStyleBlock;

typedef struct ArcrDocument {
    ArcrArena       arena;
    ArcrSlabPool    node_slab;
    ArcrAtomTable  *atoms;
    ArcrDomNode    *root;
    ArcrDomNode    *body;
    const char     *title;
    ArcrStyleBlock *style_blocks_head;
    ArcrStyleBlock *style_blocks_tail;
    uint32_t        node_count;
    bool            has_scripts;
    bool            has_media;
    bool            has_images;
} ArcrDocument;

ArcrDocument *arcr_dom_document_create(ArcrAtomTable *atoms);
void          arcr_dom_document_destroy(ArcrDocument *doc);

ArcrDomNode  *arcr_dom_create_element(ArcrDocument *doc, const char *tag_name, size_t tag_len);
ArcrDomNode  *arcr_dom_create_text(ArcrDocument *doc, const char *utf8, size_t len);
void          arcr_dom_append_child(ArcrDomNode *parent, ArcrDomNode *child);
void          arcr_dom_set_attr(ArcrDocument *doc, ArcrDomNode *elem,
                                const char *name, size_t name_len,
                                const char *val, size_t val_len);
const char   *arcr_dom_get_attr(const ArcrDomNode *elem, ArcrAtom name_atom);
bool          arcr_dom_has_class(const ArcrDomNode *elem, ArcrAtom class_atom);

/* Computes 64-bit Bloom filter bitmask for an atom */
uint64_t arcr_bloom_bits_for_atom(ArcrAtom atom);

/* Parses UTF-8 HTML with tag error recovery into a newly created ArcrDocument */
ArcrDocument *arcr_html_parse(ArcrAtomTable *atoms, const char *utf8_html, size_t len);

#endif /* ARCR_DOM_H */
