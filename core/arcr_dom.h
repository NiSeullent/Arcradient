/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * core/arcr_dom.h - Compact Arena-Backed DOM, Interactive State & HTML5 Parser
 */
#ifndef ARCR_DOM_H
#define ARCR_DOM_H

#include "../base/arcr_base.h"
#include "../base/arcr_codec.h"
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

typedef enum ArcrNodeStateFlags {
    ARCR_STATE_HOVER   = 1u << 0,
    ARCR_STATE_FOCUS   = 1u << 1,
    ARCR_STATE_ACTIVE  = 1u << 2,
    ARCR_STATE_CHECKED = 1u << 3,
    ARCR_STATE_VISITED = 1u << 4
} ArcrNodeStateFlags;

typedef struct ArcrAttr {
    ArcrAtom         name;
    const char      *value;
    struct ArcrAttr *next;
} ArcrAttr;

struct ArcrComputedStyle;

/* Optional attached bitmap for <img> or <canvas> elements */
typedef struct ArcrDomSurface {
    int32_t   width;
    int32_t   height;
    uint32_t *argb_pixels; /* Heap-owned or arena-owned ARGB8888 buffer */
    bool      heap_owned;
} ArcrDomSurface;

typedef struct ArcrDomNode {
    ArcrNodeKind               kind;
    uint32_t                   node_uid; /* Unique 1-based ID within document for JS handles */
    ArcrAtom                   tag;      /* Lowercase tag atom for elements */
    ArcrAtom                   id;       /* id="..." atom if present */
    uint32_t                   class_count;
    ArcrAtom                   classes[ARCR_MAX_INLINE_CLASSES];
    uint32_t                   state_flags; /* ArcrNodeStateFlags (:hover, :focus, :checked) */
    uint64_t                   ancestor_bloom; /* 64-bit Bloom hash of ancestor tags/classes/ids */
    ArcrAttr                  *first_attr;
    const char                *text;     /* UTF-8 text for TEXT / COMMENT / raw <script>/<style> */
    size_t                     text_len;

    /* Live mutable form input value (for <input>, <textarea>) */
    char                      *input_value;
    size_t                     input_cap;

    /* Optional decoded <img> or 2D <canvas> surface */
    ArcrDomSurface            *surface;

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

typedef struct ArcrExternalResourceRef {
    const char                     *href_or_src;
    ArcrDomNode                    *owner_node;
    struct ArcrExternalResourceRef *next;
} ArcrExternalResourceRef;

typedef struct ArcrDocument {
    ArcrArena                arena;
    ArcrSlabPool             node_slab;
    ArcrAtomTable           *atoms;
    ArcrDomNode             *root;
    ArcrDomNode             *body;
    ArcrDomNode             *focused_node;
    ArcrDomNode             *hovered_node;
    const char              *title;
    ArcrStyleBlock          *style_blocks_head;
    ArcrStyleBlock          *style_blocks_tail;
    ArcrExternalResourceRef *ext_styles_head;
    ArcrExternalResourceRef *ext_scripts_head;
    ArcrExternalResourceRef *ext_images_head;
    uint32_t                 node_count;
    uint32_t                 next_uid;
    bool                     has_scripts;
    bool                     has_media;
    bool                     has_images;
    bool                     dom_dirty;
} ArcrDocument;

ArcrDocument *arcr_dom_document_create(ArcrAtomTable *atoms);
void          arcr_dom_document_destroy(ArcrDocument *doc);

ArcrDomNode  *arcr_dom_create_element(ArcrDocument *doc, const char *tag_name, size_t tag_len);
ArcrDomNode  *arcr_dom_create_text(ArcrDocument *doc, const char *utf8, size_t len);
void          arcr_dom_append_child(ArcrDomNode *parent, ArcrDomNode *child);
void          arcr_dom_remove_node(ArcrDocument *doc, ArcrDomNode *node);
void          arcr_dom_clear_children(ArcrDocument *doc, ArcrDomNode *elem);
void          arcr_dom_set_text_content(ArcrDocument *doc, ArcrDomNode *elem,
                                        const char *utf8, size_t len);
size_t        arcr_dom_get_text_content(const ArcrDomNode *node, char *out_buf, size_t cap);

void          arcr_dom_set_attr(ArcrDocument *doc, ArcrDomNode *elem,
                                const char *name, size_t name_len,
                                const char *val, size_t val_len);
const char   *arcr_dom_get_attr(const ArcrDomNode *elem, ArcrAtom name_atom);
const char   *arcr_dom_get_attr_cstr(const ArcrDocument *doc, const ArcrDomNode *elem,
                                     const char *attr_name);
bool          arcr_dom_has_class(const ArcrDomNode *elem, ArcrAtom class_atom);
void          arcr_dom_add_class(ArcrDocument *doc, ArcrDomNode *elem,
                                 const char *cls, size_t cls_len);
void          arcr_dom_remove_class(ArcrDomNode *elem, ArcrAtom class_atom);

void          arcr_dom_set_input_value(ArcrDocument *doc, ArcrDomNode *elem, const char *utf8_val);
const char   *arcr_dom_get_input_value(const ArcrDocument *doc, const ArcrDomNode *elem);
ArcrDomSurface *arcr_dom_ensure_surface(ArcrDocument *doc, ArcrDomNode *elem,
                                        int32_t width, int32_t height);

/* Fast DOM queries for JS & CSS */
ArcrDomNode  *arcr_dom_find_by_uid(ArcrDomNode *root, uint32_t uid);
ArcrDomNode  *arcr_dom_get_element_by_id(const ArcrDocument *doc, const char *id_str);
size_t        arcr_dom_query_selector_all(const ArcrDocument *doc, ArcrDomNode *scope_root,
                                          const char *selector_cstr,
                                          ArcrDomNode **out_nodes, size_t max_out);

void          arcr_dom_append_style_block(ArcrDocument *doc, const char *css, size_t css_len);

/* Computes 64-bit Bloom filter bitmask for an atom */
uint64_t arcr_bloom_bits_for_atom(ArcrAtom atom);

/* Parses UTF-8 HTML with tag error recovery into a newly created ArcrDocument */
ArcrDocument *arcr_html_parse(ArcrAtomTable *atoms, const char *utf8_html, size_t len);

/* Parses an HTML fragment and appends its nodes under `target_parent` (for innerHTML) */
bool arcr_html_parse_fragment(ArcrDocument *doc, ArcrDomNode *target_parent,
                              const char *utf8_html, size_t len);

#endif /* ARCR_DOM_H */
