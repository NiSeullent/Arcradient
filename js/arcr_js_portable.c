/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * js/arcr_js_portable.c - QuickJS-NG ES2026 Runtime, Live UID-Backed DOM Bridge,
 *                         2D <canvas> Rasterizer & Full Browser Web APIs
 *
 * Strictly self-contained Tier-4 module: links ONLY QuickJS-NG + Tier-0/1 base/platform/codec
 * (zero undefined symbol dependencies on arcr_engine under dlopen RTLD_LOCAL or Win32 DLL).
 */
#include "arcr_js.h"
#include "../base/arcr_base.h"
#include "../base/arcr_codec.h"
#include "../css/arcr_css.h"
#include "../platform/arcr_platform.h"
#include "quickjs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ARCR_JS_TURN_DEADLINE_USEC 750000ULL /* 750 ms max synchronous turn */
#define ARCR_CANVAS_MAX_PTS        1024
#define ARCR_CANVAS_STACK_MAX      8
#define ARCR_GRAD_MAX_STOPS        8
#define ARCR_FRAG_TAG_ATOM_NAME    "#document-fragment"

/* -------------------------------------------------------------------------
 * 1. Canvas 2D State & VM Context Structures
 * ------------------------------------------------------------------------- */
typedef struct CanvasTransform {
    double a, b, c, d, e, f;
    uint32_t fill_argb;
    uint32_t stroke_argb;
    double   line_width;
    double   global_alpha;
    int32_t  font_px;
    bool     font_bold;
    bool     font_mono;
} CanvasTransform;

typedef struct CanvasGradStop {
    double   offset;
    uint32_t argb;
} CanvasGradStop;

typedef struct CanvasGrad {
    bool           active;
    bool           is_radial;
    double         x0, y0, r0, x1, y1, r1;
    CanvasGradStop stops[ARCR_GRAD_MAX_STOPS];
    int32_t        stop_count;
} CanvasGrad;

typedef struct CanvasPt {
    float x;
    float y;
    bool  move_to;
} CanvasPt;

typedef struct Canvas2DCtx {
    uint32_t        node_uid;
    CanvasTransform cur;
    CanvasTransform stack[ARCR_CANVAS_STACK_MAX];
    int32_t         stack_top;
    int32_t         fill_grad_id;   /* -1 if solid color */
    int32_t         stroke_grad_id; /* -1 if solid color */
    CanvasPt        pts[ARCR_CANVAS_MAX_PTS];
    int32_t         pt_count;
    float           sub_start_x;
    float           sub_start_y;
    struct Canvas2DCtx *next;
} Canvas2DCtx;

typedef struct DetachedSurfTrack {
    ArcrDomSurface           *surf;
    struct DetachedSurfTrack *next;
} DetachedSurfTrack;

struct ArcrJsVm {
    JSRuntime          *rt;
    JSContext          *ctx;
    JSClassID           dom_node_class_id;
    ArcrJsHostCallbacks host;
    size_t              heap_limit;

    uint64_t            boot_usec;
    uint64_t            deadline_usec;
    bool                interrupt_armed;

    /* O(1) UID -> ArcrDomNode* lookup table (covers both attached & detached nodes) */
    ArcrDomNode       **uid_table;
    uint32_t            uid_cap;
    ArcrAtom            frag_atom;

    /* JS-side fast hooks initialized during bootstrap */
    JSValue             fn_dispatch_event;
    JSValue             fn_tick_timers;
    JSValue             fn_has_timers;
    JSValue             fn_wrap_uid;

    /* Canvas 2D contexts & gradients */
    Canvas2DCtx        *canvas_head;
    CanvasGrad         *grads;
    int32_t             grad_count;
    int32_t             grad_cap;
    ArcrFontSystem     *canvas_fonts;

    /* Tracked heap surfaces allocated by JS so detached canvases never leak */
    DetachedSurfTrack  *surf_track_head;
};

/* -------------------------------------------------------------------------
 * 2. Self-Contained C DOM Engine Helpers (Zero arcr_engine Link Deps!)
 * ------------------------------------------------------------------------- */
static uint64_t js_bloom_bits(ArcrAtom atom) {
    if (atom == ARCR_ATOM_NULL) return 0ULL;
    uint32_t x  = (uint32_t)atom * 0x9E3779B1u;
    uint32_t b1 = (x >> 26) & 63u;
    uint32_t b2 = ((x * 0x85EBCA6Bu) >> 26) & 63u;
    return (1ULL << b1) | (1ULL << b2);
}

static uint64_t js_node_self_bloom(const ArcrDomNode *n) {
    if (!n || n->kind != ARCR_NODE_ELEMENT) return 0ULL;
    uint64_t bits = js_bloom_bits(n->tag);
    if (n->id != ARCR_ATOM_NULL) bits |= js_bloom_bits(n->id);
    uint32_t inline_n = ARCR_MIN(n->class_count, ARCR_MAX_INLINE_CLASSES);
    for (uint32_t i = 0u; i < inline_n; ++i) {
        bits |= js_bloom_bits(n->classes[i]);
    }
    if (n->class_count > ARCR_MAX_INLINE_CLASSES && n->extra_classes) {
        uint32_t ext = n->class_count - ARCR_MAX_INLINE_CLASSES;
        for (uint32_t i = 0u; i < ext; ++i) {
            bits |= js_bloom_bits(n->extra_classes[i]);
        }
    }
    return bits;
}

static void js_recompute_bloom_rec(ArcrDomNode *n) {
    if (!n) return;
    n->ancestor_bloom = n->parent
                            ? (n->parent->ancestor_bloom | js_node_self_bloom(n->parent))
                            : 0ULL;
    for (ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        js_recompute_bloom_rec(ch);
    }
}

static bool js_uid_ensure_cap(ArcrJsVm *vm, uint32_t max_uid) {
    if (!vm) return false;
    if (max_uid < vm->uid_cap) return true;
    uint32_t ncap = vm->uid_cap ? vm->uid_cap : 256u;
    while (ncap <= max_uid) ncap *= 2u;
    ArcrDomNode **ntab = (ArcrDomNode **)realloc(vm->uid_table, (size_t)ncap * sizeof(ArcrDomNode *));
    if (!ntab) return false;
    memset(ntab + vm->uid_cap, 0, (size_t)(ncap - vm->uid_cap) * sizeof(ArcrDomNode *));
    vm->uid_table = ntab;
    vm->uid_cap   = ncap;
    return true;
}

static void js_uid_index_tree_rec(ArcrJsVm *vm, ArcrDomNode *n) {
    if (!vm || !n) return;
    if (js_uid_ensure_cap(vm, n->node_uid)) {
        vm->uid_table[n->node_uid] = n;
    }
    for (ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        js_uid_index_tree_rec(vm, ch);
    }
}

static void js_uid_sync_document(ArcrJsVm *vm) {
    if (!vm || !vm->host.doc || !vm->host.doc->root) return;
    if (!js_uid_ensure_cap(vm, vm->host.doc->next_uid + 8u)) return;
    js_uid_index_tree_rec(vm, vm->host.doc->root);
}

static ArcrDomNode *js_dom_find_uid(ArcrJsVm *vm, uint32_t uid) {
    if (!vm || !vm->host.doc) return NULL;
    ArcrDocument *doc = vm->host.doc;
    if (uid == 0u) return doc->root;
    if (uid < vm->uid_cap && vm->uid_table[uid]) {
        ArcrDomNode *n = vm->uid_table[uid];
        if (n->node_uid == uid) return n;
        vm->uid_table[uid] = NULL;
    }
    js_uid_sync_document(vm);
    if (uid < vm->uid_cap && vm->uid_table[uid] && vm->uid_table[uid]->node_uid == uid) {
        return vm->uid_table[uid];
    }
    return NULL;
}

static bool js_node_is_fragment(const ArcrJsVm *vm, const ArcrDomNode *n) {
    return vm && n && n->kind == ARCR_NODE_ELEMENT &&
           vm->frag_atom != ARCR_ATOM_NULL && n->tag == vm->frag_atom;
}

static bool js_node_in_doc_root(const ArcrDocument *doc, const ArcrDomNode *n) {
    if (!doc || !doc->root || !n) return false;
    for (const ArcrDomNode *cur = n; cur != NULL; cur = cur->parent) {
        if (cur == doc->root) return true;
    }
    return false;
}

static void js_dom_unlink(ArcrDocument *doc, ArcrDomNode *node) {
    if (!node || !node->parent) return;
    ArcrDomNode *p = node->parent;
    if (node->prev_sibling) {
        node->prev_sibling->next_sibling = node->next_sibling;
    } else if (p->first_child == node) {
        p->first_child = node->next_sibling;
    } else {
        /* Fallback scan if HTML parser constructed singly-linked list */
        ArcrDomNode *prev = NULL;
        for (ArcrDomNode *c = p->first_child; c != NULL; c = c->next_sibling) {
            if (c == node) {
                if (prev) prev->next_sibling = c->next_sibling;
                else p->first_child = c->next_sibling;
                if (p->last_child == c) p->last_child = prev;
                break;
            }
            prev = c;
        }
    }
    if (node->next_sibling) {
        node->next_sibling->prev_sibling = node->prev_sibling;
    }
    if (p->last_child == node) {
        p->last_child = node->prev_sibling;
    }
    node->parent       = NULL;
    node->prev_sibling = NULL;
    node->next_sibling = NULL;
    if (doc) {
        if (doc->focused_node == node) doc->focused_node = NULL;
        if (doc->hovered_node == node) doc->hovered_node = NULL;
        doc->dom_dirty = true;
    }
}

static void js_dom_append_style_block(ArcrDocument *doc, const char *css, size_t len) {
    if (!doc || !css || len == 0u) return;
    ArcrStyleBlock *sb = (ArcrStyleBlock *)arcr_arena_alloc(
        &doc->arena, sizeof(ArcrStyleBlock), sizeof(void *));
    if (!sb) return;
    sb->css_text = arcr_arena_strdup(&doc->arena, css, len);
    sb->css_len  = len;
    sb->next     = NULL;
    if (!doc->style_blocks_head) {
        doc->style_blocks_head = sb;
        doc->style_blocks_tail = sb;
    } else {
        doc->style_blocks_tail->next = sb;
        doc->style_blocks_tail       = sb;
    }
    doc->dom_dirty = true;
}

static void js_sync_DynamicStyle_if_needed(ArcrDocument *doc, ArcrDomNode *node) {
    if (!doc || !node) return;
    if (node->kind == ARCR_NODE_ELEMENT) {
        const char *tname = arcr_atom_cstr(doc->atoms, node->tag);
        if (arcr_str_eq_nocase(tname, "style")) {
            if (node->text && node->text_len > 0u) {
                js_dom_append_style_block(doc, node->text, node->text_len);
            } else {
                for (ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
                    if (ch->kind == ARCR_NODE_TEXT && ch->text && ch->text_len > 0u) {
                        js_dom_append_style_block(doc, ch->text, ch->text_len);
                    }
                }
            }
        }
    }
}

static bool js_is_ancestor_or_self(const ArcrDomNode *anc, const ArcrDomNode *node) {
    for (const ArcrDomNode *p = node; p != NULL; p = p->parent) {
        if (p == anc) return true;
    }
    return false;
}

static void js_dom_insert_before_raw(ArcrJsVm *vm, ArcrDomNode *parent,
                                     ArcrDomNode *child, ArcrDomNode *ref) {
    if (!vm || !vm->host.doc || !parent || !child || parent == child) return;
    if (js_is_ancestor_or_self(child, parent)) return;
    ArcrDocument *doc = vm->host.doc;

    if (js_node_is_fragment(vm, child)) {
        ArcrDomNode *cur = child->first_child;
        while (cur) {
            ArcrDomNode *nxt = cur->next_sibling;
            js_dom_insert_before_raw(vm, parent, cur, ref);
            cur = nxt;
        }
        return;
    }

    if (ref && ref->parent != parent) ref = NULL;
    if (ref == child) return;

    js_dom_unlink(doc, child);
    child->parent = parent;

    if (!ref) {
        child->prev_sibling = parent->last_child;
        child->next_sibling = NULL;
        if (parent->last_child) {
            parent->last_child->next_sibling = child;
        } else {
            parent->first_child = child;
        }
        parent->last_child = child;
    } else {
        /* Ensure ref->prev_sibling is accurate even if HTML parser only set next_sibling */
        ArcrDomNode *prev = ref->prev_sibling;
        if (!prev && parent->first_child != ref) {
            for (ArcrDomNode *c = parent->first_child; c != NULL; c = c->next_sibling) {
                if (c->next_sibling == ref) { prev = c; break; }
            }
        }
        child->prev_sibling = prev;
        child->next_sibling = ref;
        ref->prev_sibling   = child;
        if (prev) prev->next_sibling = child;
        else parent->first_child = child;
    }

    if (!doc->body && child->kind == ARCR_NODE_ELEMENT &&
        arcr_str_eq_nocase(arcr_atom_cstr(doc->atoms, child->tag), "body")) {
        doc->body = child;
    }
    js_recompute_bloom_rec(child);
    if (js_node_in_doc_root(doc, parent)) {
        js_sync_DynamicStyle_if_needed(doc, child);
    }
    doc->dom_dirty = true;
}

static ArcrDomNode *js_dom_alloc_node(ArcrJsVm *vm, ArcrNodeKind kind) {
    if (!vm || !vm->host.doc) return NULL;
    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *n = (ArcrDomNode *)arcr_slab_alloc(&doc->node_slab);
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->kind     = kind;
    n->node_uid = ++doc->next_uid;
    doc->node_count++;
    doc->dom_dirty = true;
    if (js_uid_ensure_cap(vm, n->node_uid)) {
        vm->uid_table[n->node_uid] = n;
    }
    return n;
}

static ArcrDomNode *js_dom_create_element(ArcrJsVm *vm, const char *tag, size_t len) {
    if (!vm || !vm->host.doc || !tag || len == 0u) return NULL;
    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *n = js_dom_alloc_node(vm, ARCR_NODE_ELEMENT);
    if (!n) return NULL;
    n->tag = arcr_atom_intern_lower(doc->atoms, tag, len);
    if (arcr_str_eq_nocase_n(tag, len, "video", 5u) ||
        arcr_str_eq_nocase_n(tag, len, "audio", 5u)) {
        doc->has_media = true;
    } else if (arcr_str_eq_nocase_n(tag, len, "img", 3u) ||
               arcr_str_eq_nocase_n(tag, len, "canvas", 6u)) {
        doc->has_images = true;
    }
    return n;
}

static ArcrDomNode *js_dom_create_text(ArcrJsVm *vm, const char *s, size_t len) {
    if (!vm || !vm->host.doc) return NULL;
    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *n = js_dom_alloc_node(vm, ARCR_NODE_TEXT);
    if (!n) return NULL;
    n->text     = arcr_arena_strdup(&doc->arena, s ? s : "", len);
    n->text_len = len;
    return n;
}

static ArcrDomNode *js_dom_create_comment(ArcrJsVm *vm, const char *s, size_t len) {
    if (!vm || !vm->host.doc) return NULL;
    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *n = js_dom_alloc_node(vm, ARCR_NODE_COMMENT);
    if (!n) return NULL;
    n->text     = arcr_arena_strdup(&doc->arena, s ? s : "", len);
    n->text_len = len;
    return n;
}

static ArcrDomNode *js_dom_create_fragment(ArcrJsVm *vm) {
    if (!vm || !vm->host.doc) return NULL;
    if (vm->frag_atom == ARCR_ATOM_NULL) {
        vm->frag_atom = arcr_atom_intern_lower(
            vm->host.doc->atoms, ARCR_FRAG_TAG_ATOM_NAME, strlen(ARCR_FRAG_TAG_ATOM_NAME));
    }
    ArcrDomNode *n = js_dom_alloc_node(vm, ARCR_NODE_ELEMENT);
    if (n) n->tag = vm->frag_atom;
    return n;
}

static bool js_elem_has_class(const ArcrDomNode *el, ArcrAtom cls) {
    if (!el || el->kind != ARCR_NODE_ELEMENT || cls == ARCR_ATOM_NULL) return false;
    uint32_t inl = ARCR_MIN(el->class_count, ARCR_MAX_INLINE_CLASSES);
    for (uint32_t i = 0u; i < inl; ++i) {
        if (el->classes[i] == cls) return true;
    }
    if (el->class_count > ARCR_MAX_INLINE_CLASSES && el->extra_classes) {
        uint32_t ext = el->class_count - ARCR_MAX_INLINE_CLASSES;
        for (uint32_t i = 0u; i < ext; ++i) {
            if (el->extra_classes[i] == cls) return true;
        }
    }
    return false;
}

static void js_elem_add_class_atom(ArcrDocument *doc, ArcrDomNode *el, ArcrAtom cls) {
    if (!doc || !el || cls == ARCR_ATOM_NULL || js_elem_has_class(el, cls)) return;
    if (el->class_count < ARCR_MAX_INLINE_CLASSES) {
        el->classes[el->class_count++] = cls;
    } else {
        uint32_t ext_idx = el->class_count - ARCR_MAX_INLINE_CLASSES;
        if (ext_idx >= el->extra_class_cap) {
            uint32_t ncap = el->extra_class_cap ? el->extra_class_cap * 2u : 16u;
            ArcrAtom *narr = (ArcrAtom *)arcr_arena_alloc(
                &doc->arena, (size_t)ncap * sizeof(ArcrAtom), sizeof(ArcrAtom));
            if (!narr) return;
            if (el->extra_classes && ext_idx > 0u) {
                memcpy(narr, el->extra_classes, (size_t)ext_idx * sizeof(ArcrAtom));
            }
            el->extra_classes   = narr;
            el->extra_class_cap = ncap;
        }
        el->extra_classes[ext_idx] = cls;
        el->class_count++;
    }
}

static void js_sync_classes_from_str(ArcrDocument *doc, ArcrDomNode *el,
                                     const char *val, size_t len) {
    if (!doc || !el) return;
    el->class_count = 0u;
    size_t i = 0u;
    while (i < len) {
        while (i < len && (val[i] == ' ' || val[i] == '\t' || val[i] == '\n' || val[i] == '\r')) ++i;
        size_t st = i;
        while (i < len && val[i] != ' ' && val[i] != '\t' && val[i] != '\n' && val[i] != '\r') ++i;
        if (i > st) {
            ArcrAtom a = arcr_atom_intern(doc->atoms, val + st, i - st);
            js_elem_add_class_atom(doc, el, a);
        }
    }
    js_recompute_bloom_rec(el);
}

static const char *js_dom_get_attr(const ArcrDocument *doc, const ArcrDomNode *el,
                                   const char *name) {
    if (!doc || !el || el->kind != ARCR_NODE_ELEMENT || !name) return NULL;
    ArcrAtom na = arcr_atom_intern_lower(doc->atoms, name, strlen(name));
    for (const ArcrAttr *a = el->first_attr; a != NULL; a = a->next) {
        if (a->name == na) return a->value;
    }
    return NULL;
}

static void js_dom_set_input_val(ArcrDocument *doc, ArcrDomNode *el, const char *val) {
    if (!doc || !el) return;
    if (!val) val = "";
    size_t len = strlen(val);
    if (!el->input_value || len + 1u > el->input_cap) {
        size_t ncap = ARCR_MAX(len + 32u, el->input_cap ? el->input_cap * 2u : 64u);
        char *nbuf = (char *)arcr_arena_alloc(&doc->arena, ncap, 1u);
        if (!nbuf) return;
        el->input_value = nbuf;
        el->input_cap   = ncap;
    }
    memcpy(el->input_value, val, len + 1u);
    doc->dom_dirty = true;
}

static void js_dom_set_attr(ArcrDocument *doc, ArcrDomNode *el,
                            const char *name, size_t nlen,
                            const char *val, size_t vlen) {
    if (!doc || !el || el->kind != ARCR_NODE_ELEMENT || !name || nlen == 0u) return;
    ArcrAtom na = arcr_atom_intern_lower(doc->atoms, name, nlen);
    const char *dup_v = arcr_arena_strdup(&doc->arena, val ? val : "", vlen);
    if (!dup_v) return;

    bool found = false;
    ArcrAttr *tail = NULL;
    for (ArcrAttr *a = el->first_attr; a != NULL; a = a->next) {
        if (a->name == na) {
            a->value = dup_v;
            found = true;
            break;
        }
        tail = a;
    }
    if (!found) {
        ArcrAttr *na_node = (ArcrAttr *)arcr_arena_alloc(&doc->arena, sizeof(ArcrAttr), sizeof(void *));
        if (!na_node) return;
        na_node->name  = na;
        na_node->value = dup_v;
        na_node->next  = NULL;
        if (tail) tail->next = na_node;
        else el->first_attr = na_node;
    }

    if (arcr_str_eq_nocase_n(name, nlen, "id", 2u)) {
        el->id = (vlen > 0u) ? arcr_atom_intern(doc->atoms, dup_v, vlen) : ARCR_ATOM_NULL;
        js_recompute_bloom_rec(el);
    } else if (arcr_str_eq_nocase_n(name, nlen, "class", 5u)) {
        js_sync_classes_from_str(doc, el, dup_v, vlen);
    } else if (arcr_str_eq_nocase_n(name, nlen, "checked", 7u)) {
        el->state_flags |= (uint32_t)ARCR_STATE_CHECKED;
    } else if (arcr_str_eq_nocase_n(name, nlen, "value", 5u) && !el->input_value) {
        js_dom_set_input_val(doc, el, dup_v);
    }
    doc->dom_dirty = true;
}

static void js_dom_remove_attr(ArcrDocument *doc, ArcrDomNode *el, const char *name) {
    if (!doc || !el || el->kind != ARCR_NODE_ELEMENT || !name) return;
    size_t nlen = strlen(name);
    ArcrAtom na = arcr_atom_intern_lower(doc->atoms, name, nlen);
    ArcrAttr *prev = NULL;
    for (ArcrAttr *a = el->first_attr; a != NULL; a = a->next) {
        if (a->name == na) {
            if (prev) prev->next = a->next;
            else el->first_attr = a->next;
            break;
        }
        prev = a;
    }
    if (arcr_str_eq_nocase_n(name, nlen, "id", 2u)) {
        el->id = ARCR_ATOM_NULL;
        js_recompute_bloom_rec(el);
    } else if (arcr_str_eq_nocase_n(name, nlen, "class", 5u)) {
        el->class_count = 0u;
        js_recompute_bloom_rec(el);
    } else if (arcr_str_eq_nocase_n(name, nlen, "checked", 7u)) {
        el->state_flags &= ~((uint32_t)ARCR_STATE_CHECKED);
    }
    doc->dom_dirty = true;
}

static ArcrDomSurface *js_dom_ensure_surface(ArcrJsVm *vm, ArcrDomNode *el,
                                             int32_t w, int32_t h) {
    if (!vm || !vm->host.doc || !el || w <= 0 || h <= 0) return NULL;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;
    ArcrDocument *doc = vm->host.doc;

    if (el->surface && el->surface->argb_pixels &&
        el->surface->width == w && el->surface->height == h) {
        return el->surface;
    }
    if (!el->surface) {
        el->surface = (ArcrDomSurface *)arcr_arena_calloc(
            &doc->arena, 1u, sizeof(ArcrDomSurface), sizeof(void *));
        if (!el->surface) return NULL;
        DetachedSurfTrack *tr = (DetachedSurfTrack *)malloc(sizeof(DetachedSurfTrack));
        if (tr) {
            tr->surf = el->surface;
            tr->next = vm->surf_track_head;
            vm->surf_track_head = tr;
        }
    } else if (el->surface->heap_owned && el->surface->argb_pixels) {
        free(el->surface->argb_pixels);
        el->surface->argb_pixels = NULL;
        el->surface->heap_owned  = false;
    }

    uint32_t *px = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!px) return NULL;
    el->surface->width       = w;
    el->surface->height      = h;
    el->surface->argb_pixels = px;
    el->surface->heap_owned  = true;
    doc->has_images = true;
    doc->dom_dirty  = true;
    return el->surface;
}

/* -------------------------------------------------------------------------
 * 3. Growable Buffer, HTML Serializer, Fragment Parser & CSS Selector Engine
 * ------------------------------------------------------------------------- */
typedef struct JsByteBuf {
    char  *data;
    size_t len;
    size_t cap;
} JsByteBuf;

static bool jbuf_push(JsByteBuf *b, const char *s, size_t n) {
    if (!b) return false;
    if (b->len + n + 1u > b->cap) {
        size_t ncap = b->cap ? b->cap * 2u : 256u;
        while (ncap < b->len + n + 1u) ncap *= 2u;
        char *nd = (char *)realloc(b->data, ncap);
        if (!nd) return false;
        b->data = nd;
        b->cap  = ncap;
    }
    if (s && n > 0u) memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return true;
}

static bool jbuf_cstr(JsByteBuf *b, const char *s) {
    return jbuf_push(b, s, s ? strlen(s) : 0u);
}

static void jbuf_html_escaped(JsByteBuf *b, const char *s, size_t len, bool is_attr) {
    if (!s) return;
    for (size_t i = 0u; i < len; ++i) {
        char c = s[i];
        if (c == '&') jbuf_cstr(b, "&amp;");
        else if (c == '<' && !is_attr) jbuf_cstr(b, "&lt;");
        else if (c == '>' && !is_attr) jbuf_cstr(b, "&gt;");
        else if (c == '"' && is_attr)  jbuf_cstr(b, "&quot;");
        else jbuf_push(b, &c, 1u);
    }
}

static bool is_void_html_tag(const char *t) {
    static const char *const kVoid[] = {
        "area", "base", "br", "col", "embed", "hr", "img",
        "input", "link", "meta", "param", "source", "track", "wbr"
    };
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kVoid); ++i) {
        if (arcr_str_eq_nocase(t, kVoid[i])) return true;
    }
    return false;
}

static void js_serialize_node_rec(const ArcrJsVm *vm, const ArcrDocument *doc,
                                  const ArcrDomNode *n, bool include_self, JsByteBuf *out) {
    if (!doc || !n || !out) return;
    if (!include_self || n->kind == ARCR_NODE_DOCUMENT || js_node_is_fragment(vm, n)) {
        for (const ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
            js_serialize_node_rec(vm, doc, ch, true, out);
        }
        return;
    }
    if (n->kind == ARCR_NODE_TEXT) {
        jbuf_html_escaped(out, n->text, n->text_len, false);
        return;
    }
    if (n->kind == ARCR_NODE_COMMENT) {
        jbuf_cstr(out, "<!--");
        jbuf_push(out, n->text, n->text_len);
        jbuf_cstr(out, "-->");
        return;
    }
    if (n->kind == ARCR_NODE_ELEMENT) {
        const char *tag = arcr_atom_cstr(doc->atoms, n->tag);
        jbuf_cstr(out, "<");
        jbuf_cstr(out, tag);
        for (const ArcrAttr *a = n->first_attr; a != NULL; a = a->next) {
            const char *an = arcr_atom_cstr(doc->atoms, a->name);
            jbuf_cstr(out, " ");
            jbuf_cstr(out, an);
            jbuf_cstr(out, "=\"");
            if (a->value) jbuf_html_escaped(out, a->value, strlen(a->value), true);
            jbuf_cstr(out, "\"");
        }
        jbuf_cstr(out, ">");
        if (is_void_html_tag(tag)) return;
        if (!n->first_child && n->text && n->text_len > 0u) {
            jbuf_push(out, n->text, n->text_len);
        } else {
            for (const ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
                js_serialize_node_rec(vm, doc, ch, true, out);
            }
        }
        jbuf_cstr(out, "</");
        jbuf_cstr(out, tag);
        jbuf_cstr(out, ">");
    }
}

static void js_collect_text_rec(const ArcrDomNode *n, JsByteBuf *out) {
    if (!n || !out) return;
    if (n->kind == ARCR_NODE_TEXT && n->text && n->text_len > 0u) {
        jbuf_push(out, n->text, n->text_len);
        return;
    }
    if (n->kind == ARCR_NODE_ELEMENT && !n->first_child && n->text && n->text_len > 0u) {
        jbuf_push(out, n->text, n->text_len);
        return;
    }
    for (const ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        js_collect_text_rec(ch, out);
    }
}

static void decode_html_entities(const char *src, size_t len, JsByteBuf *out) {
    size_t i = 0u;
    while (i < len) {
        if (src[i] == '&') {
            size_t semi = i + 1u;
            while (semi < len && semi - i < 12u && src[semi] != ';' && src[semi] != '&' && src[semi] != ' ') {
                ++semi;
            }
            if (semi < len && src[semi] == ';') {
                const char *ent = src + i + 1u;
                size_t elen = semi - (i + 1u);
                if (arcr_str_eq_nocase_n(ent, elen, "amp", 3u))  { jbuf_cstr(out, "&"); i = semi + 1u; continue; }
                if (arcr_str_eq_nocase_n(ent, elen, "lt", 2u))   { jbuf_cstr(out, "<"); i = semi + 1u; continue; }
                if (arcr_str_eq_nocase_n(ent, elen, "gt", 2u))   { jbuf_cstr(out, ">"); i = semi + 1u; continue; }
                if (arcr_str_eq_nocase_n(ent, elen, "quot", 4u)) { jbuf_cstr(out, "\""); i = semi + 1u; continue; }
                if (arcr_str_eq_nocase_n(ent, elen, "apos", 4u) ||
                    arcr_str_eq_nocase_n(ent, elen, "#39", 3u))  { jbuf_cstr(out, "'"); i = semi + 1u; continue; }
                if (arcr_str_eq_nocase_n(ent, elen, "nbsp", 4u)) { jbuf_cstr(out, "\xC2\xA0"); i = semi + 1u; continue; }
                if (elen >= 2u && ent[0] == '#') {
                    uint32_t cp = 0u;
                    if (ent[1] == 'x' || ent[1] == 'X') {
                        for (size_t k = 2u; k < elen; ++k) {
                            char hc = ent[k];
                            uint32_t d = (hc >= '0' && hc <= '9') ? (uint32_t)(hc - '0') :
                                         (hc >= 'a' && hc <= 'f') ? (uint32_t)(hc - 'a' + 10) :
                                         (hc >= 'A' && hc <= 'F') ? (uint32_t)(hc - 'A' + 10) : 99u;
                            if (d > 15u) { cp = 0u; break; }
                            cp = (cp << 4) | d;
                        }
                    } else {
                        for (size_t k = 1u; k < elen; ++k) {
                            if (ent[k] < '0' || ent[k] > '9') { cp = 0u; break; }
                            cp = cp * 10u + (uint32_t)(ent[k] - '0');
                        }
                    }
                    if (cp > 0u && cp <= 0x10FFFFu) {
                        char u8[4];
                        size_t ulen = arcr_utf8_encode_one(cp, u8);
                        jbuf_push(out, u8, ulen);
                        i = semi + 1u;
                        continue;
                    }
                }
            }
        }
        jbuf_push(out, &src[i], 1u);
        ++i;
    }
}

static bool is_html_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static void js_parse_html_fragment(ArcrJsVm *vm, ArcrDomNode *target_parent,
                                   const char *html, size_t len) {
    if (!vm || !vm->host.doc || !target_parent || !html) return;
    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *stack[64];
    int sp = 0;
    stack[0] = target_parent;

    size_t i = 0u;
    while (i < len) {
        if (html[i] != '<') {
            size_t st = i;
            while (i < len && html[i] != '<') ++i;
            JsByteBuf dec = {0};
            decode_html_entities(html + st, i - st, &dec);
            if (dec.len > 0u) {
                ArcrDomNode *tn = js_dom_create_text(vm, dec.data, dec.len);
                if (tn) js_dom_insert_before_raw(vm, stack[sp], tn, NULL);
            }
            free(dec.data);
            continue;
        }

        /* Comment <!-- ... --> */
        if (i + 4u <= len && memcmp(html + i, "<!--", 4u) == 0) {
            size_t st = i + 4u;
            size_t end = st;
            while (end + 3u <= len && memcmp(html + end, "-->", 3u) != 0) ++end;
            ArcrDomNode *cn = js_dom_create_comment(vm, html + st, end - st);
            if (cn) js_dom_insert_before_raw(vm, stack[sp], cn, NULL);
            i = (end + 3u <= len) ? (end + 3u) : len;
            continue;
        }

        /* <!DOCTYPE ...> */
        if (i + 2u < len && html[i + 1u] == '!') {
            while (i < len && html[i] != '>') ++i;
            if (i < len) ++i;
            continue;
        }

        /* Closing tag </tag> */
        if (i + 2u < len && html[i + 1u] == '/') {
            size_t st = i + 2u;
            while (st < len && is_html_space(html[st])) ++st;
            size_t te = st;
            while (te < len && !is_html_space(html[te]) && html[te] != '>') ++te;
            size_t gt = te;
            while (gt < len && html[gt] != '>') ++gt;
            if (te > st) {
                ArcrAtom close_atom = arcr_atom_intern_lower(doc->atoms, html + st, te - st);
                for (int k = sp; k > 0; --k) {
                    if (stack[k]->tag == close_atom) {
                        sp = k - 1;
                        break;
                    }
                }
            }
            i = (gt < len) ? (gt + 1u) : len;
            continue;
        }

        /* Opening tag <tag attr="val" ...> */
        size_t st = i + 1u;
        size_t te = st;
        while (te < len && !is_html_space(html[te]) && html[te] != '>' && html[te] != '/') ++te;
        if (te == st) {
            ArcrDomNode *tn = js_dom_create_text(vm, "<", 1u);
            if (tn) js_dom_insert_before_raw(vm, stack[sp], tn, NULL);
            ++i;
            continue;
        }

        ArcrDomNode *el = js_dom_create_element(vm, html + st, te - st);
        size_t p = te;
        bool self_close = false;
        while (p < len && html[p] != '>') {
            while (p < len && is_html_space(html[p])) ++p;
            if (p < len && html[p] == '/') {
                self_close = true;
                ++p;
                continue;
            }
            if (p >= len || html[p] == '>') break;
            size_t an_s = p;
            while (p < len && !is_html_space(html[p]) && html[p] != '=' && html[p] != '>' && html[p] != '/') ++p;
            size_t an_e = p;
            while (p < len && is_html_space(html[p])) ++p;
            const char *av_ptr = "";
            size_t av_len = 0u;
            JsByteBuf av_dec = {0};
            if (p < len && html[p] == '=') {
                ++p;
                while (p < len && is_html_space(html[p])) ++p;
                if (p < len && (html[p] == '"' || html[p] == '\'')) {
                    char q = html[p++];
                    size_t vs = p;
                    while (p < len && html[p] != q) ++p;
                    decode_html_entities(html + vs, p - vs, &av_dec);
                    if (p < len && html[p] == q) ++p;
                } else {
                    size_t vs = p;
                    while (p < len && !is_html_space(html[p]) && html[p] != '>') ++p;
                    decode_html_entities(html + vs, p - vs, &av_dec);
                }
                av_ptr = av_dec.data ? av_dec.data : "";
                av_len = av_dec.len;
            }
            if (el && an_e > an_s) {
                js_dom_set_attr(doc, el, html + an_s, an_e - an_s, av_ptr, av_len);
            }
            free(av_dec.data);
        }
        if (p < len && html[p] == '>') ++p;
        i = p;

        if (!el) continue;
        const char *tname = arcr_atom_cstr(doc->atoms, el->tag);

        /* Raw text elements: <script> and <style> */
        if (!self_close && (arcr_str_eq_nocase(tname, "style") || arcr_str_eq_nocase(tname, "script"))) {
            size_t raw_s = i;
            size_t tlen = strlen(tname);
            while (i < len) {
                if (i + 2u + tlen < len && html[i] == '<' && html[i + 1u] == '/' &&
                    arcr_str_eq_nocase_n(html + i + 2u, tlen, tname, tlen)) {
                    break;
                }
                ++i;
            }
            el->text     = arcr_arena_strdup(&doc->arena, html + raw_s, i - raw_s);
            el->text_len = i - raw_s;
            while (i < len && html[i] != '>') ++i;
            if (i < len) ++i;
            js_dom_insert_before_raw(vm, stack[sp], el, NULL);
            continue;
        }

        js_dom_insert_before_raw(vm, stack[sp], el, NULL);
        if (!self_close && !is_void_html_tag(tname) && sp + 1 < 64) {
            stack[++sp] = el;
        }
    }
}

/* Selector matching for querySelector / querySelectorAll / matches / closest */
static bool js_match_selector_list(const ArcrDocument *doc, const ArcrDomNode *el,
                                   const char *sel, size_t sel_len);

static bool js_match_compound(const ArcrDocument *doc, const ArcrDomNode *el,
                              const char *s, size_t len) {
    if (!doc || !el || el->kind != ARCR_NODE_ELEMENT) return false;
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;

    size_t i = 0u;
    while (i < len) {
        char c = s[i];
        if (c == '*') {
            ++i;
        } else if (c == '#' || c == '.') {
            ++i;
            size_t st = i;
            while (i < len && s[i] != '#' && s[i] != '.' && s[i] != '[' && s[i] != ':') ++i;
            if (i == st) return false;
            if (c == '#') {
                const char *id_c = arcr_atom_cstr(doc->atoms, el->id);
                if (!id_c || strlen(id_c) != (i - st) || memcmp(id_c, s + st, i - st) != 0) {
                    return false;
                }
            } else {
                ArcrAtom ca = arcr_atom_intern(doc->atoms, s + st, i - st);
                if (!js_elem_has_class(el, ca)) return false;
            }
        } else if (c == '[') {
            ++i;
            size_t rb = i;
            while (rb < len && s[rb] != ']') ++rb;
            const char *in = s + i;
            size_t in_len = rb - i;
            i = (rb < len) ? (rb + 1u) : len;

            size_t eq = 0u;
            char op = 0;
            while (eq < in_len && in[eq] != '=' && in[eq] != '^' && in[eq] != '$' && in[eq] != '*' && in[eq] != '~') {
                ++eq;
            }
            const char *an = in;
            size_t an_l = eq;
            arcr_str_trim_view(&an, &an_l);
            char an_buf[128];
            if (an_l >= sizeof(an_buf)) an_l = sizeof(an_buf) - 1u;
            memcpy(an_buf, an, an_l);
            an_buf[an_l] = '\0';
            const char *actual = js_dom_get_attr(doc, el, an_buf);
            if (!actual) return false;

            if (eq < in_len) {
                if (in[eq] != '=' && eq + 1u < in_len && in[eq + 1u] == '=') {
                    op = in[eq];
                    eq += 2u;
                } else if (in[eq] == '=') {
                    op = '=';
                    eq += 1u;
                }
                const char *ev = in + eq;
                size_t ev_l = (eq <= in_len) ? (in_len - eq) : 0u;
                arcr_str_trim_view(&ev, &ev_l);
                if (ev_l >= 2u && ((ev[0] == '"' && ev[ev_l - 1u] == '"') ||
                                   (ev[0] == '\'' && ev[ev_l - 1u] == '\''))) {
                    ev += 1u;
                    ev_l -= 2u;
                }
                size_t act_l = strlen(actual);
                if (op == '=') {
                    if (act_l != ev_l || memcmp(actual, ev, ev_l) != 0) return false;
                } else if (op == '^') {
                    if (ev_l == 0u || act_l < ev_l || memcmp(actual, ev, ev_l) != 0) return false;
                } else if (op == '$') {
                    if (ev_l == 0u || act_l < ev_l || memcmp(actual + act_l - ev_l, ev, ev_l) != 0) return false;
                } else if (op == '*') {
                    char needle[128];
                    if (ev_l >= sizeof(needle)) ev_l = sizeof(needle) - 1u;
                    memcpy(needle, ev, ev_l);
                    needle[ev_l] = '\0';
                    if (ev_l == 0u || strstr(actual, needle) == NULL) return false;
                }
            }
        } else if (c == ':') {
            ++i;
            if (i < len && s[i] == ':') return false; /* pseudo-element */
            size_t st = i;
            while (i < len && s[i] != '#' && s[i] != '.' && s[i] != '[' && s[i] != ':' && s[i] != '(') ++i;
            const char *ps = s + st;
            size_t psl = i - st;
            const char *arg = NULL;
            size_t argl = 0u;
            if (i < len && s[i] == '(') {
                ++i;
                arg = s + i;
                int depth = 1;
                while (i < len && depth > 0) {
                    if (s[i] == '(') ++depth;
                    else if (s[i] == ')') --depth;
                    if (depth > 0) ++i;
                }
                argl = (size_t)((s + i) - arg);
                if (i < len && s[i] == ')') ++i;
            }
            if (arcr_str_eq_nocase_n(ps, psl, "checked", 7u)) {
                if ((el->state_flags & (uint32_t)ARCR_STATE_CHECKED) == 0u) return false;
            } else if (arcr_str_eq_nocase_n(ps, psl, "disabled", 8u)) {
                if (!js_dom_get_attr(doc, el, "disabled")) return false;
            } else if (arcr_str_eq_nocase_n(ps, psl, "enabled", 7u)) {
                if (js_dom_get_attr(doc, el, "disabled")) return false;
            } else if (arcr_str_eq_nocase_n(ps, psl, "first-child", 11u)) {
                if (!el->parent) return false;
                for (const ArcrDomNode *ch = el->parent->first_child; ch != NULL; ch = ch->next_sibling) {
                    if (ch->kind == ARCR_NODE_ELEMENT) {
                        if (ch != el) return false;
                        break;
                    }
                }
            } else if (arcr_str_eq_nocase_n(ps, psl, "last-child", 10u)) {
                for (const ArcrDomNode *ch = el->next_sibling; ch != NULL; ch = ch->next_sibling) {
                    if (ch->kind == ARCR_NODE_ELEMENT) return false;
                }
            } else if (arcr_str_eq_nocase_n(ps, psl, "not", 3u) && arg) {
                if (js_match_selector_list(doc, el, arg, argl)) return false;
            } else if (arcr_str_eq_nocase_n(ps, psl, "is", 2u) ||
                       arcr_str_eq_nocase_n(ps, psl, "where", 5u)) {
                if (arg && !js_match_selector_list(doc, el, arg, argl)) return false;
            }
        } else {
            size_t st = i;
            while (i < len && s[i] != '#' && s[i] != '.' && s[i] != '[' && s[i] != ':') ++i;
            const char *tname = arcr_atom_cstr(doc->atoms, el->tag);
            if (!tname || !arcr_str_eq_nocase_n(tname, strlen(tname), s + st, i - st)) {
                return false;
            }
        }
    }
    return true;
}

typedef struct JsSelPart {
    const char *ptr;
    size_t      len;
    char        comb; /* 0, ' ', '>', '+', '~' */
} JsSelPart;

static bool js_match_chain(const ArcrDocument *doc, const ArcrDomNode *el,
                           const JsSelPart *parts, int idx) {
    if (!js_match_compound(doc, el, parts[idx].ptr, parts[idx].len)) return false;
    if (idx == 0) return true;

    char comb = parts[idx].comb;
    if (comb == '>') {
        const ArcrDomNode *p = el->parent;
        return p && p->kind == ARCR_NODE_ELEMENT && js_match_chain(doc, p, parts, idx - 1);
    }
    if (comb == ' ') {
        for (const ArcrDomNode *p = el->parent; p != NULL; p = p->parent) {
            if (p->kind == ARCR_NODE_ELEMENT && js_match_chain(doc, p, parts, idx - 1)) {
                return true;
            }
        }
        return false;
    }
    if (comb == '+' || comb == '~') {
        if (!el->parent) return false;
        const ArcrDomNode *prev_el = NULL;
        for (const ArcrDomNode *c = el->parent->first_child; c != NULL && c != el; c = c->next_sibling) {
            if (c->kind == ARCR_NODE_ELEMENT) {
                if (comb == '~' && js_match_chain(doc, c, parts, idx - 1)) return true;
                prev_el = c;
            }
        }
        if (comb == '+') {
            return prev_el && js_match_chain(doc, prev_el, parts, idx - 1);
        }
    }
    return false;
}

static bool js_match_single_selector(const ArcrDocument *doc, const ArcrDomNode *el,
                                     const char *sel, size_t len) {
    arcr_str_trim_view(&sel, &len);
    if (len == 0u) return false;

    JsSelPart parts[12];
    int count = 0;
    size_t i = 0u;
    char next_comb = 0;

    while (i < len && count < 12) {
        while (i < len && is_html_space(sel[i])) ++i;
        if (i >= len) break;
        if (sel[i] == '>' || sel[i] == '+' || sel[i] == '~') {
            next_comb = sel[i++];
            while (i < len && is_html_space(sel[i])) ++i;
        }
        size_t st = i;
        int brk = 0, par = 0;
        while (i < len) {
            char c = sel[i];
            if (c == '[') ++brk;
            else if (c == ']' && brk > 0) --brk;
            else if (c == '(') ++par;
            else if (c == ')' && par > 0) --par;
            else if (brk == 0 && par == 0 &&
                     (is_html_space(c) || c == '>' || c == '+' || c == '~')) {
                break;
            }
            ++i;
        }
        if (i > st) {
            parts[count].ptr  = sel + st;
            parts[count].len  = i - st;
            parts[count].comb = (count == 0) ? 0 : (next_comb ? next_comb : ' ');
            count++;
            next_comb = 0;
        }
    }
    if (count == 0) return false;
    return js_match_chain(doc, el, parts, count - 1);
}

static bool js_match_selector_list(const ArcrDocument *doc, const ArcrDomNode *el,
                                   const char *sel, size_t sel_len) {
    if (!doc || !el || el->kind != ARCR_NODE_ELEMENT || !sel) return false;
    size_t i = 0u;
    while (i < sel_len) {
        size_t st = i;
        int brk = 0, par = 0;
        while (i < sel_len) {
            if (sel[i] == '[') ++brk;
            else if (sel[i] == ']' && brk > 0) --brk;
            else if (sel[i] == '(') ++par;
            else if (sel[i] == ')' && par > 0) --par;
            else if (brk == 0 && par == 0 && sel[i] == ',') break;
            ++i;
        }
        if (js_match_single_selector(doc, el, sel + st, i - st)) return true;
        if (i < sel_len && sel[i] == ',') ++i;
    }
    return false;
}

/* -------------------------------------------------------------------------
 * 4. CSS Color Parser & Full 2D <canvas> Software Rasterizer
 * ------------------------------------------------------------------------- */
static int js_hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint32_t js_parse_css_color(const char *s) {
    if (!s) return 0xFF000000u;
    size_t len = strlen(s);
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return 0xFF000000u;

    if (s[0] == '#') {
        if (len == 4u) {
            int r = js_hex_val(s[1]), g = js_hex_val(s[2]), b = js_hex_val(s[3]);
            if (r >= 0 && g >= 0 && b >= 0) {
                return 0xFF000000u | ((uint32_t)(r * 17) << 16) |
                       ((uint32_t)(g * 17) << 8) | (uint32_t)(b * 17);
            }
        } else if (len == 5u) {
            int r = js_hex_val(s[1]), g = js_hex_val(s[2]), b = js_hex_val(s[3]), a = js_hex_val(s[4]);
            if (r >= 0 && g >= 0 && b >= 0 && a >= 0) {
                return ((uint32_t)(a * 17) << 24) | ((uint32_t)(r * 17) << 16) |
                       ((uint32_t)(g * 17) << 8) | (uint32_t)(b * 17);
            }
        } else if (len == 7u || len == 9u) {
            int r1 = js_hex_val(s[1]), r2 = js_hex_val(s[2]);
            int g1 = js_hex_val(s[3]), g2 = js_hex_val(s[4]);
            int b1 = js_hex_val(s[5]), b2 = js_hex_val(s[6]);
            int a1 = 15, a2 = 15;
            if (len == 9u) { a1 = js_hex_val(s[7]); a2 = js_hex_val(s[8]); }
            if (r1 >= 0 && r2 >= 0 && g1 >= 0 && g2 >= 0 && b1 >= 0 && b2 >= 0 && a1 >= 0 && a2 >= 0) {
                return ((uint32_t)((a1 << 4) | a2) << 24) |
                       ((uint32_t)((r1 << 4) | r2) << 16) |
                       ((uint32_t)((g1 << 4) | g2) << 8) |
                       (uint32_t)((b1 << 4) | b2);
            }
        }
        return 0xFF000000u;
    }

    if ((len > 4u && arcr_str_eq_nocase_n(s, 4u, "rgb(", 4u)) ||
        (len > 5u && arcr_str_eq_nocase_n(s, 5u, "rgba(", 5u))) {
        const char *lp = (const char *)memchr(s, '(', len);
        if (lp) {
            double r = 0.0, g = 0.0, b = 0.0, a = 1.0;
            char tmp[96];
            size_t il = (size_t)((s + len) - (lp + 1));
            if (il >= sizeof(tmp)) il = sizeof(tmp) - 1u;
            memcpy(tmp, lp + 1, il);
            tmp[il] = '\0';
            for (size_t k = 0u; tmp[k] != '\0'; ++k) {
                if (tmp[k] == ',' || tmp[k] == '/' || tmp[k] == ')') tmp[k] = ' ';
            }
            int n = sscanf(tmp, "%lf %lf %lf %lf", &r, &g, &b, &a);
            if (n >= 3) {
                uint32_t ur = (uint32_t)ARCR_CLAMP((int)(r + 0.5), 0, 255);
                uint32_t ug = (uint32_t)ARCR_CLAMP((int)(g + 0.5), 0, 255);
                uint32_t ub = (uint32_t)ARCR_CLAMP((int)(b + 0.5), 0, 255);
                uint32_t ua = (n >= 4) ? (uint32_t)ARCR_CLAMP((int)(a * 255.0 + 0.5), 0, 255) : 255u;
                return (ua << 24) | (ur << 16) | (ug << 8) | ub;
            }
        }
    }

    static const struct { const char *name; uint32_t argb; } kNamed[] = {
        {"transparent", 0x00000000u}, {"black",       0xFF000000u},
        {"white",       0xFFFFFFFFu}, {"red",         0xFFFF0000u},
        {"green",       0xFF008000u}, {"lime",        0xFF00FF00u},
        {"blue",        0xFF0000FFu}, {"yellow",      0xFFFFFF00u},
        {"cyan",        0xFF00FFFFu}, {"aqua",        0xFF00FFFFu},
        {"magenta",     0xFFFF00FFu}, {"fuchsia",     0xFFFF00FFu},
        {"gray",        0xFF808080u}, {"grey",        0xFF808080u},
        {"silver",      0xFFC0C0C0u}, {"maroon",      0xFF800000u},
        {"olive",       0xFF808000u}, {"purple",      0xFF800080u},
        {"teal",        0xFF008080u}, {"navy",        0xFF000080u},
        {"orange",      0xFFFFA500u}, {"pink",        0xFFFFC0CBu},
        {"gold",        0xFFFFD700u}, {"coral",       0xFFFF7F50u},
        {"tomato",      0xFFFF6347u}, {"crimson",     0xFFDC143Cu},
        {"dodgerblue",  0xFF1E90FFu}, {"royalblue",   0xFF4169E1u},
        {"skyblue",     0xFF87CEEBu}, {"rebeccapurple",0xFF663399u}
    };
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kNamed); ++i) {
        if (arcr_str_eq_nocase_n(s, len, kNamed[i].name, strlen(kNamed[i].name))) {
            return kNamed[i].argb;
        }
    }
    return 0xFF000000u;
}

static Canvas2DCtx *js_get_canvas_ctx(ArcrJsVm *vm, uint32_t uid) {
    if (!vm) return NULL;
    for (Canvas2DCtx *c = vm->canvas_head; c != NULL; c = c->next) {
        if (c->node_uid == uid) return c;
    }
    Canvas2DCtx *c = (Canvas2DCtx *)calloc(1u, sizeof(Canvas2DCtx));
    if (!c) return NULL;
    c->node_uid          = uid;
    c->cur.a             = 1.0;
    c->cur.d             = 1.0;
    c->cur.fill_argb     = 0xFF000000u;
    c->cur.stroke_argb   = 0xFF000000u;
    c->cur.line_width    = 1.0;
    c->cur.global_alpha  = 1.0;
    c->cur.font_px       = 14;
    c->fill_grad_id      = -1;
    c->stroke_grad_id    = -1;
    c->next              = vm->canvas_head;
    vm->canvas_head      = c;
    return c;
}

static ArcrDomSurface *js_canvas_get_surf(ArcrJsVm *vm, uint32_t uid) {
    ArcrDomNode *el = js_dom_find_uid(vm, uid);
    if (!el || el->kind != ARCR_NODE_ELEMENT || !vm->host.doc) return NULL;
    if (el->surface && el->surface->argb_pixels) return el->surface;

    int32_t w = 300, h = 150;
    const char *wa = js_dom_get_attr(vm->host.doc, el, "width");
    const char *ha = js_dom_get_attr(vm->host.doc, el, "height");
    int32_t pw = 0, ph = 0;
    if (wa && arcr_parse_i32(wa, strlen(wa), &pw) && pw > 0) w = pw;
    if (ha && arcr_parse_i32(ha, strlen(ha), &ph) && ph > 0) h = ph;
    return js_dom_ensure_surface(vm, el, w, h);
}

static inline void canvas_xfm_pt(const CanvasTransform *t, double x, double y,
                                 float *ox, float *oy) {
    *ox = (float)(t->a * x + t->c * y + t->e);
    *oy = (float)(t->b * x + t->d * y + t->f);
}

static inline uint32_t canvas_apply_alpha(uint32_t argb, double global_a) {
    uint32_t a = (argb >> 24) & 0xFFu;
    if (global_a < 0.999) {
        a = (uint32_t)ARCR_CLAMP((int)((double)a * global_a + 0.5), 0, 255);
    }
    return (argb & 0x00FFFFFFu) | (a << 24);
}

static inline void canvas_blend_px(uint32_t *dst, uint32_t src) {
    uint32_t sa = (src >> 24) & 0xFFu;
    if (sa == 0u) return;
    if (sa == 255u) { *dst = src; return; }
    uint32_t d = *dst;
    uint32_t da = (d >> 24) & 0xFFu;
    if (da == 0u) { *dst = src; return; }
    uint32_t inv = 255u - sa;
    uint32_t out_a = sa + (da * inv + 127u) / 255u;
    uint32_t r = (((src >> 16) & 0xFFu) * sa + ((d >> 16) & 0xFFu) * inv + 127u) / 255u;
    uint32_t g = (((src >> 8)  & 0xFFu) * sa + ((d >> 8)  & 0xFFu) * inv + 127u) / 255u;
    uint32_t b = ((src         & 0xFFu) * sa + (d         & 0xFFu) * inv + 127u) / 255u;
    *dst = (ARCR_MIN(out_a, 255u) << 24) | (ARCR_MIN(r, 255u) << 16) |
           (ARCR_MIN(g, 255u) << 8) | ARCR_MIN(b, 255u);
}

static uint32_t canvas_sample_grad(const ArcrJsVm *vm, int32_t grad_id, double px, double py) {
    if (!vm || grad_id < 0 || grad_id >= vm->grad_count) return 0xFF000000u;
    const CanvasGrad *g = &vm->grads[grad_id];
    if (!g->active || g->stop_count <= 0) return 0xFF000000u;
    if (g->stop_count == 1) return g->stops[0].argb;

    double t = 0.0;
    if (!g->is_radial) {
        double dx = g->x1 - g->x0;
        double dy = g->y1 - g->y0;
        double len2 = dx * dx + dy * dy;
        if (len2 > 1e-6) {
            t = ((px - g->x0) * dx + (py - g->y0) * dy) / len2;
        }
    } else {
        double dx = px - g->x1;
        double dy = py - g->y1;
        double dist = sqrt(dx * dx + dy * dy);
        double dr = g->r1 - g->r0;
        t = (fabs(dr) > 1e-6) ? ((dist - g->r0) / dr) : 0.0;
    }
    if (t <= g->stops[0].offset) return g->stops[0].argb;
    if (t >= g->stops[g->stop_count - 1].offset) return g->stops[g->stop_count - 1].argb;

    for (int32_t i = 0; i + 1 < g->stop_count; ++i) {
        double o0 = g->stops[i].offset;
        double o1 = g->stops[i + 1].offset;
        if (t >= o0 && t <= o1) {
            double f = (o1 > o0 + 1e-6) ? ((t - o0) / (o1 - o0)) : 0.0;
            uint32_t c0 = g->stops[i].argb;
            uint32_t c1 = g->stops[i + 1].argb;
            uint32_t a = (uint32_t)((1.0 - f) * ((c0 >> 24) & 0xFFu) + f * ((c1 >> 24) & 0xFFu) + 0.5);
            uint32_t r = (uint32_t)((1.0 - f) * ((c0 >> 16) & 0xFFu) + f * ((c1 >> 16) & 0xFFu) + 0.5);
            uint32_t gc = (uint32_t)((1.0 - f) * ((c0 >> 8) & 0xFFu) + f * ((c1 >> 8) & 0xFFu) + 0.5);
            uint32_t b = (uint32_t)((1.0 - f) * (c0 & 0xFFu) + f * (c1 & 0xFFu) + 0.5);
            return (ARCR_MIN(a, 255u) << 24) | (ARCR_MIN(r, 255u) << 16) |
                   (ARCR_MIN(gc, 255u) << 8) | ARCR_MIN(b, 255u);
        }
    }
    return g->stops[g->stop_count - 1].argb;
}

static void canvas_push_pt(Canvas2DCtx *c, double x, double y, bool move_to) {
    if (!c || c->pt_count >= ARCR_CANVAS_MAX_PTS) return;
    float ox = 0.0f, oy = 0.0f;
    canvas_xfm_pt(&c->cur, x, y, &ox, &oy);
    c->pts[c->pt_count].x       = ox;
    c->pts[c->pt_count].y       = oy;
    c->pts[c->pt_count].move_to = move_to;
    if (move_to || c->pt_count == 0) {
        c->sub_start_x = ox;
        c->sub_start_y = oy;
    }
    c->pt_count++;
}

static void canvas_draw_thick_seg(ArcrDomSurface *surf, float x0, float y0, float x1, float y1,
                                  float half_w, uint32_t col) {
    if (!surf || !surf->argb_pixels) return;
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    float r = ARCR_MAX(0.6f, half_w);
    int32_t min_x = ARCR_CLAMP((int32_t)floorf(ARCR_MIN(x0, x1) - r - 1.0f), 0, surf->width - 1);
    int32_t max_x = ARCR_CLAMP((int32_t)ceilf(ARCR_MAX(x0, x1) + r + 1.0f),  0, surf->width - 1);
    int32_t min_y = ARCR_CLAMP((int32_t)floorf(ARCR_MIN(y0, y1) - r - 1.0f), 0, surf->height - 1);
    int32_t max_y = ARCR_CLAMP((int32_t)ceilf(ARCR_MAX(y0, y1) + r + 1.0f),  0, surf->height - 1);

    uint32_t base_a = (col >> 24) & 0xFFu;
    if (base_a == 0u) return;

    for (int32_t py = min_y; py <= max_y; ++py) {
        float cy = (float)py + 0.5f;
        uint32_t *row = surf->argb_pixels + (size_t)py * (size_t)surf->width;
        for (int32_t px = min_x; px <= max_x; ++px) {
            float cx = (float)px + 0.5f;
            float t = (len2 > 1e-5f) ? (((cx - x0) * dx + (cy - y0) * dy) / len2) : 0.0f;
            if (t < 0.0f) t = 0.0f;
            else if (t > 1.0f) t = 1.0f;
            float nx = x0 + t * dx;
            float ny = y0 + t * dy;
            float dist = sqrtf((cx - nx) * (cx - nx) + (cy - ny) * (cy - ny));
            float cov = r + 0.5f - dist;
            if (cov > 0.0f) {
                if (cov > 1.0f) cov = 1.0f;
                uint32_t a = (uint32_t)((float)base_a * cov + 0.5f);
                canvas_blend_px(&row[px], (col & 0x00FFFFFFu) | (a << 24));
            }
        }
    }
}

static void canvas_fill_current_path(ArcrJsVm *vm, Canvas2DCtx *c, ArcrDomSurface *surf) {
    if (!c || !surf || !surf->argb_pixels || c->pt_count < 3) return;

    float min_y_f = c->pts[0].y, max_y_f = c->pts[0].y;
    for (int32_t i = 1; i < c->pt_count; ++i) {
        if (c->pts[i].y < min_y_f) min_y_f = c->pts[i].y;
        if (c->pts[i].y > max_y_f) max_y_f = c->pts[i].y;
    }
    int32_t y0 = ARCR_CLAMP((int32_t)floorf(min_y_f), 0, surf->height - 1);
    int32_t y1 = ARCR_CLAMP((int32_t)ceilf(max_y_f),  0, surf->height - 1);

    float x_inter[256];
    for (int32_t y = y0; y <= y1; ++y) {
        float sy = (float)y + 0.5f;
        int n_int = 0;

        int32_t sub_start = 0;
        for (int32_t i = 1; i <= c->pt_count; ++i) {
            bool end_sub = (i == c->pt_count) || c->pts[i].move_to;
            int32_t a_idx = end_sub ? (i - 1) : (i - 1);
            int32_t b_idx = end_sub ? sub_start : i;
            if (a_idx != b_idx) {
                float ay = c->pts[a_idx].y, by = c->pts[b_idx].y;
                float ax = c->pts[a_idx].x, bx = c->pts[b_idx].x;
                if ((ay <= sy && by > sy) || (by <= sy && ay > sy)) {
                    if (n_int < 256) {
                        x_inter[n_int++] = ax + (sy - ay) * (bx - ax) / (by - ay);
                    }
                }
            }
            if (end_sub) sub_start = i;
        }

        /* Insertion sort intersections */
        for (int i = 1; i < n_int; ++i) {
            float key = x_inter[i];
            int j = i - 1;
            while (j >= 0 && x_inter[j] > key) {
                x_inter[j + 1] = x_inter[j];
                --j;
            }
            x_inter[j + 1] = key;
        }

        uint32_t *row = surf->argb_pixels + (size_t)y * (size_t)surf->width;
        for (int i = 0; i + 1 < n_int; i += 2) {
            int32_t sx0 = ARCR_CLAMP((int32_t)floorf(x_inter[i]),     0, surf->width);
            int32_t sx1 = ARCR_CLAMP((int32_t)ceilf(x_inter[i + 1]),  0, surf->width);
            for (int32_t x = sx0; x < sx1; ++x) {
                uint32_t raw_c = (c->fill_grad_id >= 0)
                                     ? canvas_sample_grad(vm, c->fill_grad_id, (double)x + 0.5, (double)y + 0.5)
                                     : c->cur.fill_argb;
                canvas_blend_px(&row[x], canvas_apply_alpha(raw_c, c->cur.global_alpha));
            }
        }
    }
}

/* Base64 encoder for HTMLCanvasElement.prototype.toDataURL() */
static char *js_base64_encode_png_uri(const uint8_t *src, size_t len) {
    static const char kB64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *prefix = "data:image/png;base64,";
    size_t plen = strlen(prefix);
    size_t b64_len = ((len + 2u) / 3u) * 4u;
    char *out = (char *)malloc(plen + b64_len + 1u);
    if (!out) return NULL;
    memcpy(out, prefix, plen);

    size_t w = plen;
    size_t i = 0u;
    while (i + 3u <= len) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1u] << 8) | src[i + 2u];
        out[w++] = kB64[(v >> 18) & 63u];
        out[w++] = kB64[(v >> 12) & 63u];
        out[w++] = kB64[(v >> 6)  & 63u];
        out[w++] = kB64[v         & 63u];
        i += 3u;
    }
    if (i < len) {
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1u < len) v |= (uint32_t)src[i + 1u] << 8;
        out[w++] = kB64[(v >> 18) & 63u];
        out[w++] = kB64[(v >> 12) & 63u];
        out[w++] = (i + 1u < len) ? kB64[(v >> 6) & 63u] : '=';
        out[w++] = '=';
    }
    out[w] = '\0';
    return out;
}

/* -------------------------------------------------------------------------
 * 5. Native QuickJS Bridge Functions (`B.*`)
 * ------------------------------------------------------------------------- */
static inline ArcrJsVm *vm_from_ctx(JSContext *ctx) {
    return (ArcrJsVm *)JS_GetContextOpaque(ctx);
}

static JSValue b_alloc_node_obj(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || argc < 2) return JS_NULL;
    uint32_t uid = 0u;
    (void)JS_ToUint32(ctx, &uid, argv[0]);
    JSValue obj = JS_NewObjectProtoClass(ctx, argv[1], vm->dom_node_class_id);
    if (!JS_IsException(obj)) {
        (void)JS_SetOpaque(obj, (void *)(uintptr_t)(uid + 1u));
    }
    return obj;
}

static JSValue b_node_uid_of(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || argc < 1 || !JS_IsObject(argv[0])) return JS_NewInt32(ctx, -1);
    void *op = JS_GetOpaque(argv[0], vm->dom_node_class_id);
    if (!op) return JS_NewInt32(ctx, -1);
    uint32_t uid = (uint32_t)((uintptr_t)op - 1u);
    return JS_NewUint32(ctx, uid);
}

static JSValue b_node_info(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || !vm->host.doc || argc < 1) return JS_NULL;
    uint32_t uid = 0u;
    (void)JS_ToUint32(ctx, &uid, argv[0]);
    ArcrDomNode *n = js_dom_find_uid(vm, uid);
    if (!n) return JS_NULL;

    int32_t ntype = 1;
    const char *tname = "";
    if (n->kind == ARCR_NODE_DOCUMENT) {
        ntype = 9;
        tname = "#document";
    } else if (js_node_is_fragment(vm, n)) {
        ntype = 11;
        tname = "#document-fragment";
    } else if (n->kind == ARCR_NODE_TEXT) {
        ntype = 3;
        tname = "#text";
    } else if (n->kind == ARCR_NODE_COMMENT) {
        ntype = 8;
        tname = "#comment";
    } else {
        ntype = 1;
        tname = arcr_atom_cstr(vm->host.doc->atoms, n->tag);
        if (!tname) tname = "div";
    }

    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "uid", JS_NewUint32(ctx, n->node_uid));
    JS_SetPropertyStr(ctx, o, "nodeType", JS_NewInt32(ctx, ntype));
    JS_SetPropertyStr(ctx, o, "tag", JS_NewString(ctx, tname));
    JS_SetPropertyStr(ctx, o, "connected", JS_NewBool(ctx, js_node_in_doc_root(vm->host.doc, n)));
    return o;
}

static ArcrDomNode *find_first_tag_rec(const ArcrDocument *doc, ArcrDomNode *n, const char *tag) {
    if (!doc || !n) return NULL;
    if (n->kind == ARCR_NODE_ELEMENT &&
        arcr_str_eq_nocase(arcr_atom_cstr(doc->atoms, n->tag), tag)) {
        return n;
    }
    for (ArcrDomNode *c = n->first_child; c != NULL; c = c->next_sibling) {
        ArcrDomNode *f = find_first_tag_rec(doc, c, tag);
        if (f) return f;
    }
    return NULL;
}

static JSValue b_doc_special_uids(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || !vm->host.doc || !vm->host.doc->root) return JS_NULL;
    js_uid_sync_document(vm);
    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *html = find_first_tag_rec(doc, doc->root, "html");
    ArcrDomNode *head = find_first_tag_rec(doc, doc->root, "head");
    ArcrDomNode *body = doc->body ? doc->body : find_first_tag_rec(doc, doc->root, "body");
    ArcrDomNode *act  = doc->focused_node ? doc->focused_node : body;

    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "doc",  JS_NewUint32(ctx, doc->root->node_uid));
    JS_SetPropertyStr(ctx, o, "html", JS_NewUint32(ctx, html ? html->node_uid : 0u));
    JS_SetPropertyStr(ctx, o, "head", JS_NewUint32(ctx, head ? head->node_uid : 0u));
    JS_SetPropertyStr(ctx, o, "body", JS_NewUint32(ctx, body ? body->node_uid : 0u));
    JS_SetPropertyStr(ctx, o, "active", JS_NewUint32(ctx, act ? act->node_uid : 0u));
    return o;
}

static JSValue b_get_title(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    const char *t = (vm && vm->host.doc && vm->host.doc->title) ? vm->host.doc->title : "";
    return JS_NewString(ctx, t);
}

static JSValue b_set_title(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    if (vm->host.doc) {
        vm->host.doc->title = arcr_arena_strdup(&vm->host.doc->arena, s, strlen(s));
        ArcrDomNode *tnode = find_first_tag_rec(vm->host.doc, vm->host.doc->root, "title");
        if (tnode) {
            tnode->first_child = NULL;
            tnode->last_child  = NULL;
            ArcrDomNode *tx = js_dom_create_text(vm, s, strlen(s));
            if (tx) js_dom_insert_before_raw(vm, tnode, tx, NULL);
        }
    }
    if (vm->host.set_document_title) {
        vm->host.set_document_title(vm->host.userdata, s);
    }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static ArcrDomNode *find_by_id_rec(const ArcrDocument *doc, ArcrDomNode *n, ArcrAtom id_atom) {
    if (!doc || !n) return NULL;
    if (n->kind == ARCR_NODE_ELEMENT && n->id == id_atom) return n;
    for (ArcrDomNode *c = n->first_child; c != NULL; c = c->next_sibling) {
        ArcrDomNode *f = find_by_id_rec(doc, c, id_atom);
        if (f) return f;
    }
    return NULL;
}

static JSValue b_get_by_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || !vm->host.doc || !vm->host.doc->root || argc < 1) return JS_NewUint32(ctx, 0u);
    size_t len = 0u;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_NewUint32(ctx, 0u);
    uint32_t res_uid = 0u;
    if (len > 0u) {
        ArcrAtom ia = arcr_atom_intern(vm->host.doc->atoms, s, len);
        ArcrDomNode *f = find_by_id_rec(vm->host.doc, vm->host.doc->root, ia);
        if (f) {
            if (js_uid_ensure_cap(vm, f->node_uid)) vm->uid_table[f->node_uid] = f;
            res_uid = f->node_uid;
        }
    }
    JS_FreeCString(ctx, s);
    return JS_NewUint32(ctx, res_uid);
}

static void query_sel_rec(ArcrJsVm *vm, ArcrDomNode *n, bool skip_self,
                          const char *sel, size_t sel_len, bool first_only,
                          JSContext *ctx, JSValue arr, uint32_t *io_cnt) {
    if (!vm || !n || (first_only && *io_cnt > 0u)) return;
    if (!skip_self && n->kind == ARCR_NODE_ELEMENT && !js_node_is_fragment(vm, n)) {
        if (js_match_selector_list(vm->host.doc, n, sel, sel_len)) {
            if (js_uid_ensure_cap(vm, n->node_uid)) vm->uid_table[n->node_uid] = n;
            JS_SetPropertyUint32(ctx, arr, (*io_cnt)++, JS_NewUint32(ctx, n->node_uid));
            if (first_only) return;
        }
    }
    for (ArcrDomNode *c = n->first_child; c != NULL; c = c->next_sibling) {
        query_sel_rec(vm, c, false, sel, sel_len, first_only, ctx, arr, io_cnt);
        if (first_only && *io_cnt > 0u) return;
    }
}

static JSValue b_query_selector(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    JSValue arr = JS_NewArray(ctx);
    if (!vm || !vm->host.doc || argc < 3) return arr;
    uint32_t scope_uid = 0u;
    (void)JS_ToUint32(ctx, &scope_uid, argv[0]);
    size_t slen = 0u;
    const char *sel = JS_ToCStringLen(ctx, &slen, argv[1]);
    bool first_only = JS_ToBool(ctx, argv[2]) > 0;
    if (!sel) return arr;

    ArcrDomNode *scope = js_dom_find_uid(vm, scope_uid);
    uint32_t cnt = 0u;
    if (scope) {
        query_sel_rec(vm, scope, true, sel, slen, first_only, ctx, arr, &cnt);
    }
    JS_FreeCString(ctx, sel);
    return arr;
}

static void query_tag_or_cls_rec(ArcrJsVm *vm, ArcrDomNode *n, bool skip_self,
                                 bool is_tag, const char *q, size_t qlen, bool match_all_tags,
                                 JSContext *ctx, JSValue arr, uint32_t *io_cnt) {
    if (!vm || !n) return;
    if (!skip_self && n->kind == ARCR_NODE_ELEMENT && !js_node_is_fragment(vm, n)) {
        bool ok = false;
        if (is_tag) {
            const char *tn = arcr_atom_cstr(vm->host.doc->atoms, n->tag);
            ok = match_all_tags || (tn && arcr_str_eq_nocase_n(tn, strlen(tn), q, qlen));
        } else {
            ok = true;
            size_t i = 0u;
            bool any_tok = false;
            while (i < qlen) {
                while (i < qlen && is_html_space(q[i])) ++i;
                size_t st = i;
                while (i < qlen && !is_html_space(q[i])) ++i;
                if (i > st) {
                    any_tok = true;
                    ArcrAtom ca = arcr_atom_intern(vm->host.doc->atoms, q + st, i - st);
                    if (!js_elem_has_class(n, ca)) { ok = false; break; }
                }
            }
            if (!any_tok) ok = false;
        }
        if (ok) {
            if (js_uid_ensure_cap(vm, n->node_uid)) vm->uid_table[n->node_uid] = n;
            JS_SetPropertyUint32(ctx, arr, (*io_cnt)++, JS_NewUint32(ctx, n->node_uid));
        }
    }
    for (ArcrDomNode *c = n->first_child; c != NULL; c = c->next_sibling) {
        query_tag_or_cls_rec(vm, c, false, is_tag, q, qlen, match_all_tags, ctx, arr, io_cnt);
    }
}

static JSValue b_get_by_tag_or_cls(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    JSValue arr = JS_NewArray(ctx);
    if (!vm || !vm->host.doc || argc < 3) return arr;
    uint32_t scope_uid = 0u;
    (void)JS_ToUint32(ctx, &scope_uid, argv[0]);
    bool is_tag = JS_ToBool(ctx, argv[1]) > 0;
    size_t qlen = 0u;
    const char *q = JS_ToCStringLen(ctx, &qlen, argv[2]);
    if (!q) return arr;

    ArcrDomNode *scope = js_dom_find_uid(vm, scope_uid);
    uint32_t cnt = 0u;
    bool all_tags = is_tag && qlen == 1u && q[0] == '*';
    if (scope) {
        query_tag_or_cls_rec(vm, scope, true, is_tag, q, qlen, all_tags, ctx, arr, &cnt);
    }
    JS_FreeCString(ctx, q);
    return arr;
}

static JSValue b_matches_or_closest(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || !vm->host.doc || argc < 3) return JS_NewUint32(ctx, 0u);
    uint32_t uid = 0u;
    (void)JS_ToUint32(ctx, &uid, argv[0]);
    size_t slen = 0u;
    const char *sel = JS_ToCStringLen(ctx, &slen, argv[1]);
    bool is_closest = JS_ToBool(ctx, argv[2]) > 0;
    if (!sel) return JS_NewUint32(ctx, 0u);

    ArcrDomNode *n = js_dom_find_uid(vm, uid);
    uint32_t hit_uid = 0u;
    for (ArcrDomNode *cur = n; cur != NULL; cur = is_closest ? cur->parent : NULL) {
        if (cur->kind == ARCR_NODE_ELEMENT && !js_node_is_fragment(vm, cur) &&
            js_match_selector_list(vm->host.doc, cur, sel, slen)) {
            if (js_uid_ensure_cap(vm, cur->node_uid)) vm->uid_table[cur->node_uid] = cur;
            hit_uid = cur->node_uid;
            break;
        }
        if (!is_closest) break;
    }
    JS_FreeCString(ctx, sel);
    return JS_NewUint32(ctx, hit_uid);
}

static JSValue b_contains(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || argc < 2) return JS_FALSE;
    uint32_t a = 0u, b = 0u;
    (void)JS_ToUint32(ctx, &a, argv[0]);
    (void)JS_ToUint32(ctx, &b, argv[1]);
    ArcrDomNode *na = js_dom_find_uid(vm, a);
    ArcrDomNode *nb = js_dom_find_uid(vm, b);
    if (!na || !nb) return JS_FALSE;
    return JS_NewBool(ctx, js_is_ancestor_or_self(na, nb));
}

static ArcrDomNode *find_prev_sibling_safe(const ArcrDomNode *n) {
    if (!n || !n->parent) return NULL;
    if (n->prev_sibling) return n->prev_sibling;
    ArcrDomNode *prev = NULL;
    for (ArcrDomNode *c = n->parent->first_child; c != NULL && c != n; c = c->next_sibling) {
        prev = c;
    }
    return prev;
}

static JSValue b_nav(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || argc < 2) return JS_NewUint32(ctx, 0u);
    uint32_t uid = 0u;
    int32_t op = 0;
    (void)JS_ToUint32(ctx, &uid, argv[0]);
    (void)JS_ToInt32(ctx, &op, argv[1]);
    ArcrDomNode *n = js_dom_find_uid(vm, uid);
    if (!n) return JS_NewUint32(ctx, 0u);

    ArcrDomNode *r = NULL;
    switch (op) {
        case 0: r = n->parent; break; /* parentNode */
        case 1: r = (n->parent && n->parent->kind == ARCR_NODE_ELEMENT &&
                     !js_node_is_fragment(vm, n->parent)) ? n->parent : NULL; break; /* parentElement */
        case 2: r = n->first_child; break;
        case 3: r = n->last_child; break;
        case 4:
            for (ArcrDomNode *c = n->first_child; c; c = c->next_sibling) {
                if (c->kind == ARCR_NODE_ELEMENT) { r = c; break; }
            }
            break;
        case 5:
            for (ArcrDomNode *c = n->first_child; c; c = c->next_sibling) {
                if (c->kind == ARCR_NODE_ELEMENT) r = c;
            }
            break;
        case 6: r = n->next_sibling; break;
        case 7: r = find_prev_sibling_safe(n); break;
        case 8:
            for (ArcrDomNode *c = n->next_sibling; c; c = c->next_sibling) {
                if (c->kind == ARCR_NODE_ELEMENT) { r = c; break; }
            }
            break;
        case 9:
            if (n->parent) {
                for (ArcrDomNode *c = n->parent->first_child; c && c != n; c = c->next_sibling) {
                    if (c->kind == ARCR_NODE_ELEMENT) r = c;
                }
            }
            break;
        default: break;
    }
    if (r && js_uid_ensure_cap(vm, r->node_uid)) vm->uid_table[r->node_uid] = r;
    return JS_NewUint32(ctx, r ? r->node_uid : 0u);
}

static JSValue b_child_uids(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    JSValue arr = JS_NewArray(ctx);
    if (!vm || argc < 2) return arr;
    uint32_t uid = 0u;
    (void)JS_ToUint32(ctx, &uid, argv[0]);
    bool elems_only = JS_ToBool(ctx, argv[1]) > 0;
    ArcrDomNode *n = js_dom_find_uid(vm, uid);
    if (!n) return arr;

    uint32_t idx = 0u;
    for (ArcrDomNode *c = n->first_child; c != NULL; c = c->next_sibling) {
        if (elems_only && c->kind != ARCR_NODE_ELEMENT) continue;
        if (js_uid_ensure_cap(vm, c->node_uid)) vm->uid_table[c->node_uid] = c;
        JS_SetPropertyUint32(ctx, arr, idx++, JS_NewUint32(ctx, c->node_uid));
    }
    return arr;
}

static JSValue b_ancestor_chain(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    JSValue arr = JS_NewArray(ctx);
    if (!vm || argc < 1) return arr;
    uint32_t uid = 0u;
    (void)JS_ToUint32(ctx, &uid, argv[0]);
    ArcrDomNode *n = js_dom_find_uid(vm, uid);
    uint32_t idx = 0u;
    for (ArcrDomNode *cur = n; cur != NULL; cur = cur->parent) {
        if (js_uid_ensure_cap(vm, cur->node_uid)) vm->uid_table[cur->node_uid] = cur;
        JS_SetPropertyUint32(ctx, arr, idx++, JS_NewUint32(ctx, cur->node_uid));
    }
    return arr;
}

static JSValue b_create_node(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || !vm->host.doc || argc < 2) return JS_NewUint32(ctx, 0u);
    int32_t kind = 0;
    (void)JS_ToInt32(ctx, &kind, argv[0]);
    if (kind == 11) {
        ArcrDomNode *f = js_dom_create_fragment(vm);
        return JS_NewUint32(ctx, f ? f->node_uid : 0u);
    }
    size_t len = 0u;
    const char *s = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!s) return JS_NewUint32(ctx, 0u);

    ArcrDomNode *n = NULL;
    if (kind == 1)      n = js_dom_create_element(vm, s, len);
    else if (kind == 3) n = js_dom_create_text(vm, s, len);
    else if (kind == 8) n = js_dom_create_comment(vm, s, len);
    JS_FreeCString(ctx, s);
    return JS_NewUint32(ctx, n ? n->node_uid : 0u);
}

static JSValue b_insert_before(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || argc < 3) return JS_UNDEFINED;
    uint32_t p_uid = 0u, c_uid = 0u, r_uid = 0u;
    (void)JS_ToUint32(ctx, &p_uid, argv[0]);
    (void)JS_ToUint32(ctx, &c_uid, argv[1]);
    (void)JS_ToUint32(ctx, &r_uid, argv[2]);
    ArcrDomNode *p = js_dom_find_uid(vm, p_uid);
    ArcrDomNode *c = js_dom_find_uid(vm, c_uid);
    ArcrDomNode *r = r_uid ? js_dom_find_uid(vm, r_uid) : NULL;
    if (p && c) {
        js_dom_insert_before_raw(vm, p, c, r);
    }
    return JS_UNDEFINED;
}

static JSValue b_remove_node(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || !vm->host.doc || argc < 1) return JS_UNDEFINED;
    uint32_t uid = 0u;
    (void)JS_ToUint32(ctx, &uid, argv[0]);
    ArcrDomNode *n = js_dom_find_uid(vm, uid);
    if (n && n != vm->host.doc->root) {
        js_dom_unlink(vm->host.doc, n);
    }
    return JS_UNDEFINED;
}

static ArcrDomNode *js_clone_node_rec(ArcrJsVm *vm, const ArcrDomNode *src, bool deep) {
    if (!vm || !vm->host.doc || !src) return NULL;
    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *dst = NULL;
    if (js_node_is_fragment(vm, src)) {
        dst = js_dom_create_fragment(vm);
    } else if (src->kind == ARCR_NODE_ELEMENT) {
        const char *tn = arcr_atom_cstr(doc->atoms, src->tag);
        dst = js_dom_create_element(vm, tn ? tn : "div", tn ? strlen(tn) : 3u);
        if (dst) {
            for (const ArcrAttr *a = src->first_attr; a != NULL; a = a->next) {
                const char *an = arcr_atom_cstr(doc->atoms, a->name);
                const char *av = a->value ? a->value : "";
                if (an) js_dom_set_attr(doc, dst, an, strlen(an), av, strlen(av));
            }
            if (src->input_value) js_dom_set_input_val(doc, dst, src->input_value);
            dst->state_flags = src->state_flags & (uint32_t)ARCR_STATE_CHECKED;
        }
    } else if (src->kind == ARCR_NODE_TEXT) {
        dst = js_dom_create_text(vm, src->text, src->text_len);
    } else if (src->kind == ARCR_NODE_COMMENT) {
        dst = js_dom_create_comment(vm, src->text, src->text_len);
    }
    if (dst && deep) {
        for (const ArcrDomNode *c = src->first_child; c != NULL; c = c->next_sibling) {
            ArcrDomNode *cc = js_clone_node_rec(vm, c, true);
            if (cc) js_dom_insert_before_raw(vm, dst, cc, NULL);
        }
    }
    return dst;
}

static JSValue b_clone_node(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || argc < 2) return JS_NewUint32(ctx, 0u);
    uint32_t uid = 0u;
    (void)JS_ToUint32(ctx, &uid, argv[0]);
    bool deep = JS_ToBool(ctx, argv[1]) > 0;
    ArcrDomNode *src = js_dom_find_uid(vm, uid);
    ArcrDomNode *cl  = js_clone_node_rec(vm, src, deep);
    return JS_NewUint32(ctx, cl ? cl->node_uid : 0u);
}

static JSValue b_get_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || argc < 1) return JS_NewString(ctx, "");
    uint32_t uid = 0u;
    (void)JS_ToUint32(ctx, &uid, argv[0]);
    ArcrDomNode *n = js_dom_find_uid(vm, uid);
    if (!n) return JS_NewString(ctx, "");
    if (n->kind == ARCR_NODE_TEXT || n->kind == ARCR_NODE_COMMENT) {
        return JS_NewStringLen(ctx, n->text ? n->text : "", n->text_len);
    }
    JsByteBuf b = {0};
    js_collect_text_rec(n, &b);
    JSValue ret = JS_NewStringLen(ctx, b.data ? b.data : "", b.len);
    free(b.data);
    return ret;
}

static void js_detach_children_only(ArcrDocument *doc, ArcrDomNode *el) {
    if (!el) return;
    ArcrDomNode *c = el->first_child;
    while (c) {
        ArcrDomNode *nxt = c->next_sibling;
        c->parent       = NULL;
        c->prev_sibling = NULL;
        c->next_sibling = NULL;
        if (doc) {
            if (doc->focused_node == c) doc->focused_node = NULL;
            if (doc->hovered_node == c) doc->hovered_node = NULL;
        }
        c = nxt;
    }
    el->first_child = NULL;
    el->last_child  = NULL;
    if (doc) doc->dom_dirty = true;
}

static JSValue b_set_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || !vm->host.doc || argc < 2) return JS_UNDEFINED;
    uint32_t uid = 0u;
    (void)JS_ToUint32(ctx, &uid, argv[0]);
    size_t len = 0u;
    const char *s = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!s) return JS_EXCEPTION;

    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *n = js_dom_find_uid(vm, uid);
    if (n) {
        if (n->kind == ARCR_NODE_TEXT || n->kind == ARCR_NODE_COMMENT) {
            n->text     = arcr_arena_strdup(&doc->arena, s, len);
            n->text_len = len;
            doc->dom_dirty = true;
        } else {
            js_detach_children_only(doc, n);
            n->text     = NULL;
            n->text_len = 0u;
            if (len > 0u) {
                ArcrDomNode *tn = js_dom_create_text(vm, s, len);
                if (tn) js_dom_insert_before_raw(vm, n, tn, NULL);
            }
            if (n->kind == ARCR_NODE_ELEMENT) {
                const char *tname = arcr_atom_cstr(doc->atoms, n->tag);
                if (arcr_str_eq_nocase(tname, "title")) {
                    doc->title = arcr_arena_strdup(&doc->arena, s, len);
                    if (vm->host.set_document_title) {
                        vm->host.set_document_title(vm->host.userdata, s);
                    }
                } else if (arcr_str_eq_nocase(tname, "textarea")) {
                    js_dom_set_input_val(doc, n, s);
                } else if (arcr_str_eq_nocase(tname, "style") && js_node_in_doc_root(doc, n)) {
                    js_dom_append_style_block(doc, s, len);
                }
            }
        }
    }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue b_get_html(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || !vm->host.doc || argc < 2) return JS_NewString(ctx, "");
    uint32_t uid = 0u;
    (void)JS_ToUint32(ctx, &uid, argv[0]);
    bool outer = JS_ToBool(ctx, argv[1]) > 0;
    ArcrDomNode *n = js_dom_find_uid(vm, uid);
    if (!n) return JS_NewString(ctx, "");

    JsByteBuf b = {0};
    js_serialize_node_rec(vm, vm->host.doc, n, outer, &b);
    JSValue ret = JS_NewStringLen(ctx, b.data ? b.data : "", b.len);
    free(b.data);
    return ret;
}

static JSValue b_set_inner_html(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || !vm->host.doc || argc < 2) return JS_UNDEFINED;
    uint32_t uid = 0u;
    (void)JS_ToUint32(ctx, &uid, argv[0]);
    size_t len = 0u;
    const char *html = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!html) return JS_EXCEPTION;

    ArcrDomNode *n = js_dom_find_uid(vm, uid);
    if (n && (n->kind == ARCR_NODE_ELEMENT || n->kind == ARCR_NODE_DOCUMENT)) {
        js_detach_children_only(vm->host.doc, n);
        n->text     = NULL;
        n->text_len = 0u;
        if (len > 0u) {
            if (n->kind == ARCR_NODE_ELEMENT &&
                arcr_str_eq_nocase(arcr_atom_cstr(vm->host.doc->atoms, n->tag), "style")) {
                n->text     = arcr_arena_strdup(&vm->host.doc->arena, html, len);
                n->text_len = len;
                if (js_node_in_doc_root(vm->host.doc, n)) {
                    js_dom_append_style_block(vm->host.doc, html, len);
                }
            } else {
                js_parse_html_fragment(vm, n, html, len);
            }
        }
        vm->host.doc->dom_dirty = true;
    }
    JS_FreeCString(ctx, html);
    return JS_UNDEFINED;
}

static JSValue b_attr_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || !vm->host.doc || argc < 2) return JS_NULL;
    uint32_t uid = 0u;
    int32_t op = 0;
    (void)JS_ToUint32(ctx, &uid, argv[0]);
    (void)JS_ToInt32(ctx, &op, argv[1]);
    ArcrDomNode *el = js_dom_find_uid(vm, uid);
    if (!el || el->kind != ARCR_NODE_ELEMENT || js_node_is_fragment(vm, el)) {
        return (op == 3) ? JS_NewArray(ctx) : ((op == 2) ? JS_FALSE : JS_NULL);
    }

    if (op == 3) { /* getAttributeNames */
        JSValue arr = JS_NewArray(ctx);
        uint32_t idx = 0u;
        for (const ArcrAttr *a = el->first_attr; a != NULL; a = a->next) {
            const char *an = arcr_atom_cstr(vm->host.doc->atoms, a->name);
            if (an) JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, an));
        }
        return arr;
    }

    if (argc < 3) return JS_NULL;
    size_t nlen = 0u;
    const char *name = JS_ToCStringLen(ctx, &nlen, argv[2]);
    if (!name) return JS_EXCEPTION;

    JSValue ret = JS_UNDEFINED;
    if (op == 0) { /* get */
        const char *v = js_dom_get_attr(vm->host.doc, el, name);
        ret = v ? JS_NewString(ctx, v) : JS_NULL;
    } else if (op == 1 && argc >= 4) { /* set */
        size_t vlen = 0u;
        const char *val = JS_ToCStringLen(ctx, &vlen, argv[3]);
        if (val) {
            js_dom_set_attr(vm->host.doc, el, name, nlen, val, vlen);
            JS_FreeCString(ctx, val);
        }
    } else if (op == 2) { /* has */
        ret = JS_NewBool(ctx, js_dom_get_attr(vm->host.doc, el, name) != NULL);
    } else if (op == 4) { /* remove */
        js_dom_remove_attr(vm->host.doc, el, name);
    }
    JS_FreeCString(ctx, name);
    return ret;
}

static JSValue b_form_state(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || !vm->host.doc || argc < 2) return JS_UNDEFINED;
    uint32_t uid = 0u;
    int32_t op = 0;
    (void)JS_ToUint32(ctx, &uid, argv[0]);
    (void)JS_ToInt32(ctx, &op, argv[1]);
    ArcrDomNode *el = js_dom_find_uid(vm, uid);
    if (!el || el->kind != ARCR_NODE_ELEMENT) {
        return (op == 0) ? JS_NewString(ctx, "") : JS_FALSE;
    }

    if (op == 0) { /* get value */
        if (el->input_value) return JS_NewString(ctx, el->input_value);
        const char *av = js_dom_get_attr(vm->host.doc, el, "value");
        return JS_NewString(ctx, av ? av : "");
    }
    if (op == 1 && argc >= 3) { /* set value */
        const char *s = JS_ToCString(ctx, argv[2]);
        if (s) {
            js_dom_set_input_val(vm->host.doc, el, s);
            JS_FreeCString(ctx, s);
        }
        return JS_UNDEFINED;
    }
    if (op == 2) { /* get checked */
        return JS_NewBool(ctx, (el->state_flags & (uint32_t)ARCR_STATE_CHECKED) != 0u);
    }
    if (op == 3 && argc >= 3) { /* set checked */
        if (JS_ToBool(ctx, argv[2]) > 0) el->state_flags |= (uint32_t)ARCR_STATE_CHECKED;
        else el->state_flags &= ~((uint32_t)ARCR_STATE_CHECKED);
        vm->host.doc->dom_dirty = true;
        return JS_UNDEFINED;
    }
    if (op == 4 && argc >= 3) { /* focus / blur */
        bool want = JS_ToBool(ctx, argv[2]) > 0;
        if (want) {
            if (vm->host.doc->focused_node && vm->host.doc->focused_node != el) {
                vm->host.doc->focused_node->state_flags &= ~((uint32_t)ARCR_STATE_FOCUS);
            }
            vm->host.doc->focused_node = el;
            el->state_flags |= (uint32_t)ARCR_STATE_FOCUS;
        } else if (vm->host.doc->focused_node == el) {
            el->state_flags &= ~((uint32_t)ARCR_STATE_FOCUS);
            vm->host.doc->focused_node = NULL;
        }
        vm->host.doc->dom_dirty = true;
    }
    return JS_UNDEFINED;
}

static JSValue b_computed_info(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    JSValue o = JS_NewObject(ctx);
    int32_t vw = (vm && vm->host.viewport_width > 0) ? vm->host.viewport_width : 1280;
    uint32_t uid = 0u;
    if (argc >= 1) (void)JS_ToUint32(ctx, &uid, argv[0]);
    ArcrDomNode *el = vm ? js_dom_find_uid(vm, uid) : NULL;

    int32_t w = vw, h = 24, fpx = 16;
    const char *disp = "block";
    const char *pos  = "static";
    char color_s[32] = "rgb(15, 23, 42)";
    char bg_s[32]    = "rgba(0, 0, 0, 0)";
    double opac      = 1.0;

    if (el && el->surface && el->surface->width > 0) {
        w = el->surface->width;
        h = el->surface->height;
    } else if (el && vm && vm->host.doc) {
        const char *wa = js_dom_get_attr(vm->host.doc, el, "width");
        const char *ha = js_dom_get_attr(vm->host.doc, el, "height");
        int32_t pw = 0, ph = 0;
        if (wa && arcr_parse_i32(wa, strlen(wa), &pw) && pw > 0) w = pw;
        if (ha && arcr_parse_i32(ha, strlen(ha), &ph) && ph > 0) h = ph;
    }
    if (el && el->computed_style) {
        const ArcrComputedStyle *cs = el->computed_style;
        if (cs->width_spec > 0) w = cs->width_spec;
        else if (cs->width_spec < 0) w = (vw * (-cs->width_spec)) / 100;
        if (cs->height_px > 0) h = cs->height_px;
        fpx  = cs->font_px > 0 ? cs->font_px : 16;
        opac = (double)cs->opacity / 255.0;
        if (cs->display == ARCR_DISPLAY_NONE) { disp = "none"; w = 0; h = 0; }
        else if (cs->display == ARCR_DISPLAY_FLEX) disp = "flex";
        else if (cs->display == ARCR_DISPLAY_GRID) disp = "grid";
        else if (cs->display == ARCR_DISPLAY_INLINE) disp = "inline";
        else if (cs->display == ARCR_DISPLAY_INLINE_BLOCK) disp = "inline-block";

        if (cs->position == ARCR_POS_RELATIVE) pos = "relative";
        else if (cs->position == ARCR_POS_ABSOLUTE) pos = "absolute";
        else if (cs->position == ARCR_POS_FIXED) pos = "fixed";
        else if (cs->position == ARCR_POS_STICKY) pos = "sticky";

        snprintf(color_s, sizeof(color_s), "rgb(%u, %u, %u)",
                 (cs->color_argb >> 16) & 0xFFu, (cs->color_argb >> 8) & 0xFFu, cs->color_argb & 0xFFu);
        uint32_t ba = (cs->bg_argb >> 24) & 0xFFu;
        if (ba == 255u) {
            snprintf(bg_s, sizeof(bg_s), "rgb(%u, %u, %u)",
                     (cs->bg_argb >> 16) & 0xFFu, (cs->bg_argb >> 8) & 0xFFu, cs->bg_argb & 0xFFu);
        } else {
            snprintf(bg_s, sizeof(bg_s), "rgba(%u, %u, %u, %.2f)",
                     (cs->bg_argb >> 16) & 0xFFu, (cs->bg_argb >> 8) & 0xFFu, cs->bg_argb & 0xFFu,
                     (double)ba / 255.0);
        }
    }
    JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, h));
    JS_SetPropertyStr(ctx, o, "fontSize", JS_NewInt32(ctx, fpx));
    JS_SetPropertyStr(ctx, o, "display", JS_NewString(ctx, disp));
    JS_SetPropertyStr(ctx, o, "position", JS_NewString(ctx, pos));
    JS_SetPropertyStr(ctx, o, "color", JS_NewString(ctx, color_s));
    JS_SetPropertyStr(ctx, o, "backgroundColor", JS_NewString(ctx, bg_s));
    JS_SetPropertyStr(ctx, o, "opacity", JS_NewFloat64(ctx, opac));
    return o;
}

/* Host & Environment Bridge Calls */
static JSValue b_console_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || !vm->host.console_log || argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (s) {
        vm->host.console_log(vm->host.userdata, s);
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

static JSValue b_env_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    JSValue o = JS_NewObject(ctx);
    const char *url = "about:blank";
    if (vm && vm->host.get_current_url) {
        const char *u = vm->host.get_current_url(vm->host.userdata);
        if (u && u[0] != '\0') url = u;
    }
    JS_SetPropertyStr(ctx, o, "url", JS_NewString(ctx, url));
    JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, (vm && vm->host.viewport_width > 0) ? vm->host.viewport_width : 1280));
    JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, (vm && vm->host.viewport_height > 0) ? vm->host.viewport_height : 720));
    JS_SetPropertyStr(ctx, o, "prefersDark", JS_NewBool(ctx, vm ? vm->host.prefers_dark : false));
    uint64_t now_us = arcr_platform_monotonic_usec();
    double rel_ms = (vm && now_us >= vm->boot_usec) ? ((double)(now_us - vm->boot_usec) * 0.001) : 0.0;
    JS_SetPropertyStr(ctx, o, "nowMs", JS_NewFloat64(ctx, rel_ms));
    return o;
}

static JSValue b_navigate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || !vm->host.navigate_to || argc < 1) return JS_UNDEFINED;
    const char *u = JS_ToCString(ctx, argv[0]);
    if (u) {
        vm->host.navigate_to(vm->host.userdata, u);
        JS_FreeCString(ctx, u);
    }
    return JS_UNDEFINED;
}

static JSValue b_storage_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || argc < 2) return JS_NULL;
    int32_t op = 0;
    (void)JS_ToInt32(ctx, &op, argv[0]);
    const char *key = JS_ToCString(ctx, argv[1]);
    if (!key) return JS_EXCEPTION;

    JSValue ret = JS_UNDEFINED;
    if (op == 0) { /* get */
        char buf[1024];
        buf[0] = '\0';
        if (vm->host.storage_get && vm->host.storage_get(vm->host.userdata, key, buf, sizeof(buf))) {
            ret = JS_NewString(ctx, buf);
        } else {
            ret = JS_NULL;
        }
    } else if (op == 1 && argc >= 3) { /* set / delete */
        if (JS_IsNull(argv[2]) || JS_IsUndefined(argv[2])) {
            if (vm->host.storage_set) vm->host.storage_set(vm->host.userdata, key, NULL);
        } else {
            const char *val = JS_ToCString(ctx, argv[2]);
            if (val) {
                if (vm->host.storage_set) vm->host.storage_set(vm->host.userdata, key, val);
                JS_FreeCString(ctx, val);
            }
        }
    }
    JS_FreeCString(ctx, key);
    return ret;
}

static JSValue b_http_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    JSValue res = JS_NewObject(ctx);
    if (!vm || !vm->host.http_fetch || argc < 4) {
        JS_SetPropertyStr(ctx, res, "ok", JS_FALSE);
        JS_SetPropertyStr(ctx, res, "status", JS_NewInt32(ctx, 0));
        return res;
    }

    const char *url    = JS_ToCString(ctx, argv[0]);
    const char *method = JS_ToCString(ctx, argv[1]);
    size_t body_len    = 0u;
    const char *body   = JS_IsNull(argv[2]) || JS_IsUndefined(argv[2])
                             ? NULL
                             : JS_ToCStringLen(ctx, &body_len, argv[2]);
    const char *ctype  = JS_IsNull(argv[3]) || JS_IsUndefined(argv[3])
                             ? NULL
                             : JS_ToCString(ctx, argv[3]);

    int status = 0;
    char resp_ct[256] = {0};
    uint8_t *resp_bytes = NULL;
    size_t resp_len = 0u;

    /* Pause synchronous script deadline while performing network I/O */
    bool prev_armed = vm->interrupt_armed;
    vm->interrupt_armed = false;

    bool net_ok = false;
    if (url && method) {
        net_ok = vm->host.http_fetch(vm->host.userdata, url, method,
                                     body, body_len, ctype,
                                     &status, resp_ct, sizeof(resp_ct),
                                     &resp_bytes, &resp_len);
    }

    if (prev_armed) {
        vm->deadline_usec   = arcr_platform_monotonic_usec() + ARCR_JS_TURN_DEADLINE_USEC;
        vm->interrupt_armed = true;
    }

    JS_SetPropertyStr(ctx, res, "netOk", JS_NewBool(ctx, net_ok));
    JS_SetPropertyStr(ctx, res, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, res, "contentType", JS_NewString(ctx, resp_ct));
    JSValue ab = JS_NewArrayBufferCopy(ctx, resp_bytes ? resp_bytes : (const uint8_t *)"", resp_len);
    JS_SetPropertyStr(ctx, res, "buffer", ab);

    if (resp_bytes) {
        if (vm->host.http_free) vm->host.http_free(vm->host.userdata, resp_bytes);
        else free(resp_bytes);
    }
    if (url)    JS_FreeCString(ctx, url);
    if (method) JS_FreeCString(ctx, method);
    if (body)   JS_FreeCString(ctx, body);
    if (ctype)  JS_FreeCString(ctx, ctype);
    return res;
}

static JSValue b_random_bytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    size_t byte_off = 0u, byte_len = 0u, bpe = 0u;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &byte_off, &byte_len, &bpe);
    if (!JS_IsException(ab)) {
        size_t total = 0u;
        uint8_t *ptr = JS_GetArrayBuffer(ctx, &total, ab);
        if (ptr && byte_off + byte_len <= total && byte_len <= 65536u) {
            if (!arcr_platform_csprng_bytes(ptr + byte_off, byte_len)) {
                uint64_t seed = arcr_platform_monotonic_usec() ^ 0x9E3779B97F4A7C15ULL;
                for (size_t i = 0u; i < byte_len; ++i) {
                    seed ^= seed << 13;
                    seed ^= seed >> 7;
                    seed ^= seed << 17;
                    ptr[byte_off + i] = (uint8_t)(seed & 0xFFu);
                }
            }
        }
        JS_FreeValue(ctx, ab);
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    return JS_DupValue(ctx, argv[0]);
}

static JSValue b_clipboard_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    if (JS_ToBool(ctx, argv[0]) > 0 && argc >= 2) {
        const char *s = JS_ToCString(ctx, argv[1]);
        if (s) {
            (void)arcr_platform_clipboard_set_utf8(s);
            JS_FreeCString(ctx, s);
        }
        return JS_TRUE;
    }
    char buf[4096];
    buf[0] = '\0';
    (void)arcr_platform_clipboard_get_utf8(buf, sizeof(buf));
    return JS_NewString(ctx, buf);
}

static JSValue b_mark_dirty(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (vm && vm->host.doc) vm->host.doc->dom_dirty = true;
    return JS_UNDEFINED;
}

/* Canvas 2D C Bridge Operations */
static JSValue b_canvas_cmd(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    ArcrJsVm *vm = vm_from_ctx(ctx);
    if (!vm || !vm->host.doc || argc < 2) return JS_UNDEFINED;
    uint32_t uid = 0u;
    int32_t op = 0;
    (void)JS_ToUint32(ctx, &uid, argv[0]);
    (void)JS_ToInt32(ctx, &op, argv[1]);

    if (op == 100) { /* createGradient(isRadial, x0,y0,r0,x1,y1,r1) -> gradId */
        if (vm->grad_count >= vm->grad_cap) {
            int32_t ncap = vm->grad_cap ? vm->grad_cap * 2 : 16;
            CanvasGrad *ng = (CanvasGrad *)realloc(vm->grads, (size_t)ncap * sizeof(CanvasGrad));
            if (!ng) return JS_NewInt32(ctx, -1);
            vm->grads    = ng;
            vm->grad_cap = ncap;
        }
        int32_t id = vm->grad_count++;
        CanvasGrad *g = &vm->grads[id];
        memset(g, 0, sizeof(*g));
        g->active    = true;
        g->is_radial = (argc >= 3) && (JS_ToBool(ctx, argv[2]) > 0);
        if (argc >= 9) {
            (void)JS_ToFloat64(ctx, &g->x0, argv[3]);
            (void)JS_ToFloat64(ctx, &g->y0, argv[4]);
            (void)JS_ToFloat64(ctx, &g->r0, argv[5]);
            (void)JS_ToFloat64(ctx, &g->x1, argv[6]);
            (void)JS_ToFloat64(ctx, &g->y1, argv[7]);
            (void)JS_ToFloat64(ctx, &g->r1, argv[8]);
        }
        return JS_NewInt32(ctx, id);
    }
    if (op == 101 && argc >= 5) { /* addColorStop(gradId, offset, colorStr) */
        int32_t gid = -1;
        double off = 0.0;
        (void)JS_ToInt32(ctx, &gid, argv[2]);
        (void)JS_ToFloat64(ctx, &off, argv[3]);
        const char *cs = JS_ToCString(ctx, argv[4]);
        if (cs && gid >= 0 && gid < vm->grad_count && vm->grads[gid].stop_count < ARCR_GRAD_MAX_STOPS) {
            CanvasGrad *g = &vm->grads[gid];
            g->stops[g->stop_count].offset = ARCR_CLAMP(off, 0.0, 1.0);
            g->stops[g->stop_count].argb   = js_parse_css_color(cs);
            g->stop_count++;
        }
        if (cs) JS_FreeCString(ctx, cs);
        return JS_UNDEFINED;
    }

    ArcrDomSurface *surf = js_canvas_get_surf(vm, uid);
    Canvas2DCtx *c = js_get_canvas_ctx(vm, uid);
    if (!surf || !c) return JS_UNDEFINED;

    double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0, a4 = 0.0, a5 = 0.0;
    if (argc >= 3 && JS_IsNumber(argv[2])) (void)JS_ToFloat64(ctx, &a0, argv[2]);
    if (argc >= 4 && JS_IsNumber(argv[3])) (void)JS_ToFloat64(ctx, &a1, argv[3]);
    if (argc >= 5 && JS_IsNumber(argv[4])) (void)JS_ToFloat64(ctx, &a2, argv[4]);
    if (argc >= 6 && JS_IsNumber(argv[5])) (void)JS_ToFloat64(ctx, &a3, argv[5]);
    if (argc >= 7 && JS_IsNumber(argv[6])) (void)JS_ToFloat64(ctx, &a4, argv[6]);
    if (argc >= 8 && JS_IsNumber(argv[7])) (void)JS_ToFloat64(ctx, &a5, argv[7]);

    switch (op) {
        case 0: { /* resize / ensure (w, h) */
            int32_t nw = (int32_t)a0, nh = (int32_t)a1;
            ArcrDomNode *el = js_dom_find_uid(vm, uid);
            if (el && nw > 0 && nh > 0) (void)js_dom_ensure_surface(vm, el, nw, nh);
            break;
        }
        case 1: /* setFillStyle / setStrokeStyle */
            if (argc >= 4) {
                bool is_stroke = JS_ToBool(ctx, argv[2]) > 0;
                if (JS_IsNumber(argv[3])) {
                    int32_t gid = -1;
                    (void)JS_ToInt32(ctx, &gid, argv[3]);
                    if (is_stroke) c->stroke_grad_id = gid;
                    else c->fill_grad_id = gid;
                } else {
                    const char *cs = JS_ToCString(ctx, argv[3]);
                    if (cs) {
                        uint32_t col = js_parse_css_color(cs);
                        if (is_stroke) { c->cur.stroke_argb = col; c->stroke_grad_id = -1; }
                        else { c->cur.fill_argb = col; c->fill_grad_id = -1; }
                        JS_FreeCString(ctx, cs);
                    }
                }
            }
            break;
        case 2: c->cur.line_width   = ARCR_MAX(0.5, a0); break;
        case 3: c->cur.global_alpha = ARCR_CLAMP(a0, 0.0, 1.0); break;
        case 4: { /* setFont(str) */
            if (argc >= 3) {
                const char *fs = JS_ToCString(ctx, argv[2]);
                if (fs) {
                    c->cur.font_bold = (strstr(fs, "bold") != NULL || strstr(fs, "700") != NULL);
                    c->cur.font_mono = (strstr(fs, "mono") != NULL);
                    for (const char *p = fs; *p; ++p) {
                        if (*p >= '0' && *p <= '9') {
                            int px = atoi(p);
                            if (px >= 6 && px <= 200) { c->cur.font_px = px; break; }
                            while (*p >= '0' && *p <= '9') ++p;
                            if (!*p) break;
                        }
                    }
                    JS_FreeCString(ctx, fs);
                }
            }
            break;
        }
        case 5: /* save */
            if (c->stack_top < ARCR_CANVAS_STACK_MAX) c->stack[c->stack_top++] = c->cur;
            break;
        case 6: /* restore */
            if (c->stack_top > 0) c->cur = c->stack[--c->stack_top];
            break;
        case 7: /* translate */
            c->cur.e += c->cur.a * a0 + c->cur.c * a1;
            c->cur.f += c->cur.b * a0 + c->cur.d * a1;
            break;
        case 8: /* scale */
            c->cur.a *= a0; c->cur.b *= a0;
            c->cur.c *= a1; c->cur.d *= a1;
            break;
        case 9: { /* rotate */
            double co = cos(a0), si = sin(a0);
            double na = c->cur.a * co + c->cur.c * si;
            double nb = c->cur.b * co + c->cur.d * si;
            double nc = c->cur.a * (-si) + c->cur.c * co;
            double nd = c->cur.b * (-si) + c->cur.d * co;
            c->cur.a = na; c->cur.b = nb; c->cur.c = nc; c->cur.d = nd;
            break;
        }
        case 10: /* setTransform */
            c->cur.a = a0; c->cur.b = a1; c->cur.c = a2;
            c->cur.d = a3; c->cur.e = a4; c->cur.f = a5;
            break;
        case 11: /* clearRect */ {
            float fx0 = 0.0f, fy0 = 0.0f, fx1 = 0.0f, fy1 = 0.0f;
            canvas_xfm_pt(&c->cur, a0, a1, &fx0, &fy0);
            canvas_xfm_pt(&c->cur, a0 + a2, a1 + a3, &fx1, &fy1);
            int32_t x0 = ARCR_CLAMP((int32_t)floorf(ARCR_MIN(fx0, fx1)), 0, surf->width);
            int32_t x1 = ARCR_CLAMP((int32_t)ceilf(ARCR_MAX(fx0, fx1)),  0, surf->width);
            int32_t y0 = ARCR_CLAMP((int32_t)floorf(ARCR_MIN(fy0, fy1)), 0, surf->height);
            int32_t y1 = ARCR_CLAMP((int32_t)ceilf(ARCR_MAX(fy0, fy1)),  0, surf->height);
            for (int32_t y = y0; y < y1; ++y) {
                memset(surf->argb_pixels + (size_t)y * (size_t)surf->width + x0,
                       0, (size_t)(x1 - x0) * sizeof(uint32_t));
            }
            vm->host.doc->dom_dirty = true;
            break;
        }
        case 12: /* fillRect */ {
            int32_t saved_cnt = c->pt_count;
            c->pt_count = 0;
            canvas_push_pt(c, a0,      a1,      true);
            canvas_push_pt(c, a0 + a2, a1,      false);
            canvas_push_pt(c, a0 + a2, a1 + a3, false);
            canvas_push_pt(c, a0,      a1 + a3, false);
            canvas_fill_current_path(vm, c, surf);
            c->pt_count = saved_cnt;
            vm->host.doc->dom_dirty = true;
            break;
        }
        case 13: /* strokeRect */ {
            float p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y;
            canvas_xfm_pt(&c->cur, a0,      a1,      &p0x, &p0y);
            canvas_xfm_pt(&c->cur, a0 + a2, a1,      &p1x, &p1y);
            canvas_xfm_pt(&c->cur, a0 + a2, a1 + a3, &p2x, &p2y);
            canvas_xfm_pt(&c->cur, a0,      a1 + a3, &p3x, &p3y);
            uint32_t col = canvas_apply_alpha(c->cur.stroke_argb, c->cur.global_alpha);
            float hw = (float)(c->cur.line_width * 0.5);
            canvas_draw_thick_seg(surf, p0x, p0y, p1x, p1y, hw, col);
            canvas_draw_thick_seg(surf, p1x, p1y, p2x, p2y, hw, col);
            canvas_draw_thick_seg(surf, p2x, p2y, p3x, p3y, hw, col);
            canvas_draw_thick_seg(surf, p3x, p3y, p0x, p0y, hw, col);
            vm->host.doc->dom_dirty = true;
            break;
        }
        case 14: c->pt_count = 0; break; /* beginPath */
        case 15: canvas_push_pt(c, a0, a1, true); break;  /* moveTo */
        case 16: canvas_push_pt(c, a0, a1, false); break; /* lineTo */
        case 17: /* closePath */
            if (c->pt_count > 0 && c->pt_count < ARCR_CANVAS_MAX_PTS) {
                c->pts[c->pt_count].x       = c->sub_start_x;
                c->pts[c->pt_count].y       = c->sub_start_y;
                c->pts[c->pt_count].move_to = false;
                c->pt_count++;
            }
            break;
        case 18: /* rect(x,y,w,h) */
            canvas_push_pt(c, a0,      a1,      true);
            canvas_push_pt(c, a0 + a2, a1,      false);
            canvas_push_pt(c, a0 + a2, a1 + a3, false);
            canvas_push_pt(c, a0,      a1 + a3, false);
            canvas_push_pt(c, a0,      a1,      false);
            break;
        case 19: { /* arc(cx, cy, r, a0, a1, ccw) */
            double r = fabs(a2);
            double st_a = a3, end_a = a4;
            bool ccw = (argc >= 8) && (JS_ToBool(ctx, argv[7]) > 0);
            double diff = end_a - st_a;
            if (!ccw && diff < 0.0) diff += 2.0 * M_PI;
            else if (ccw && diff > 0.0) diff -= 2.0 * M_PI;
            int steps = ARCR_CLAMP((int)(fabs(diff) * ARCR_MAX(8.0, r * 0.5)), 8, 96);
            for (int i = 0; i <= steps; ++i) {
                double ang = st_a + diff * ((double)i / (double)steps);
                canvas_push_pt(c, a0 + cos(ang) * r, a1 + sin(ang) * r,
                               (i == 0 && c->pt_count == 0));
            }
            break;
        }
        case 20: /* fill */
            canvas_fill_current_path(vm, c, surf);
            vm->host.doc->dom_dirty = true;
            break;
        case 21: { /* stroke */
            uint32_t col = canvas_apply_alpha(c->cur.stroke_argb, c->cur.global_alpha);
            float hw = (float)(c->cur.line_width * 0.5);
            for (int32_t i = 1; i < c->pt_count; ++i) {
                if (!c->pts[i].move_to) {
                    canvas_draw_thick_seg(surf, c->pts[i - 1].x, c->pts[i - 1].y,
                                          c->pts[i].x, c->pts[i].y, hw, col);
                }
            }
            vm->host.doc->dom_dirty = true;
            break;
        }
        case 22: /* fillText / measureText */ {
            if (argc < 4) break;
            bool is_measure = JS_ToBool(ctx, argv[2]) > 0;
            size_t tlen = 0u;
            const char *txt = JS_ToCStringLen(ctx, &tlen, argv[3]);
            if (!txt) break;
            if (!vm->canvas_fonts) vm->canvas_fonts = arcr_font_system_create(NULL);

            double tx = (argc >= 5) ? a2 : 0.0;
            double ty = (argc >= 6) ? a3 : 0.0;
            float px = 0.0f, py = 0.0f;
            canvas_xfm_pt(&c->cur, tx, ty, &px, &py);
            int32_t cur_x = (int32_t)(px + 0.5f);
            int32_t base_y = (int32_t)(py + 0.5f);
            int32_t total_adv = 0;
            uint32_t col = canvas_apply_alpha(c->cur.fill_argb, c->cur.global_alpha);
            uint32_t base_a = (col >> 24) & 0xFFu;

            size_t pos = 0u;
            while (pos < tlen) {
                uint32_t cp = 0u;
                size_t step = arcr_utf8_decode_one(txt + pos, tlen - pos, &cp);
                if (step == 0u) break;
                pos += step;
                ArcrGlyphMetrics gm;
                memset(&gm, 0, sizeof(gm));
                if (is_measure) {
                    if (vm->canvas_fonts &&
                        arcr_font_measure_glyph_ex(vm->canvas_fonts, cp, c->cur.font_px,
                                                   c->cur.font_bold, c->cur.font_mono, &gm)) {
                        total_adv += ARCR_MAX(1, gm.advance_px);
                    } else {
                        total_adv += ARCR_MAX(4, c->cur.font_px / 2);
                    }
                } else if (vm->canvas_fonts) {
                    const uint8_t *alpha = arcr_font_get_cached_glyph_ptr(
                        vm->canvas_fonts, cp, c->cur.font_px,
                        c->cur.font_bold, c->cur.font_mono, &gm, true);
                    if (alpha && gm.width > 0 && gm.height > 0) {
                        int32_t gx0 = cur_x + gm.bearing_x;
                        int32_t gy0 = base_y - gm.bearing_y;
                        for (int32_t gy = 0; gy < gm.height; ++gy) {
                            int32_t dy = gy0 + gy;
                            if (dy < 0 || dy >= surf->height) continue;
                            uint32_t *row = surf->argb_pixels + (size_t)dy * (size_t)surf->width;
                            for (int32_t gx = 0; gx < gm.width; ++gx) {
                                int32_t dx = gx0 + gx;
                                if (dx < 0 || dx >= surf->width) continue;
                                uint8_t cov = alpha[(size_t)gy * (size_t)gm.width + (size_t)gx];
                                if (cov > 0u) {
                                    uint32_t ea = (base_a * (uint32_t)cov + 127u) / 255u;
                                    canvas_blend_px(&row[dx], (col & 0x00FFFFFFu) | (ea << 24));
                                }
                            }
                        }
                    }
                    cur_x += ARCR_MAX(1, gm.advance_px);
                }
            }
            JS_FreeCString(ctx, txt);
            if (is_measure) return JS_NewInt32(ctx, total_adv);
            vm->host.doc->dom_dirty = true;
            break;
        }
        case 23: { /* getImageData(x, y, w, h) -> ArrayBuffer of RGBA */
            int32_t rx = (int32_t)a0, ry = (int32_t)a1;
            int32_t rw = ARCR_CLAMP((int32_t)a2, 1, 4096);
            int32_t rh = ARCR_CLAMP((int32_t)a3, 1, 4096);
            size_t blen = (size_t)rw * (size_t)rh * 4u;
            uint8_t *rgba = (uint8_t *)calloc(1u, blen);
            if (!rgba) return JS_NULL;
            for (int32_t y = 0; y < rh; ++y) {
                int32_t sy = ry + y;
                if (sy < 0 || sy >= surf->height) continue;
                const uint32_t *srow = surf->argb_pixels + (size_t)sy * (size_t)surf->width;
                for (int32_t x = 0; x < rw; ++x) {
                    int32_t sx = rx + x;
                    if (sx < 0 || sx >= surf->width) continue;
                    uint32_t p = srow[sx];
                    size_t o = ((size_t)y * (size_t)rw + (size_t)x) * 4u;
                    rgba[o + 0u] = (uint8_t)((p >> 16) & 0xFFu);
                    rgba[o + 1u] = (uint8_t)((p >> 8)  & 0xFFu);
                    rgba[o + 2u] = (uint8_t)(p         & 0xFFu);
                    rgba[o + 3u] = (uint8_t)((p >> 24) & 0xFFu);
                }
            }
            JSValue ab = JS_NewArrayBufferCopy(ctx, rgba, blen);
            free(rgba);
            return ab;
        }
        case 24: { /* putImageData(uid, op, typedArray, dx, dy, w, h) */
            if (argc < 7) break;
            size_t boff = 0u, blen = 0u, bpe = 0u;
            JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[2], &boff, &blen, &bpe);
            if (!JS_IsException(ab)) {
                size_t tot = 0u;
                const uint8_t *ptr = JS_GetArrayBuffer(ctx, &tot, ab);
                int32_t dx = 0, dy = 0, rw = 0, rh = 0;
                (void)JS_ToInt32(ctx, &dx, argv[3]);
                (void)JS_ToInt32(ctx, &dy, argv[4]);
                (void)JS_ToInt32(ctx, &rw, argv[5]);
                (void)JS_ToInt32(ctx, &rh, argv[6]);
                if (ptr && rw > 0 && rh > 0 && boff + (size_t)rw * (size_t)rh * 4u <= tot) {
                    const uint8_t *rgba = ptr + boff;
                    for (int32_t y = 0; y < rh; ++y) {
                        int32_t ty = dy + y;
                        if (ty < 0 || ty >= surf->height) continue;
                        uint32_t *drow = surf->argb_pixels + (size_t)ty * (size_t)surf->width;
                        for (int32_t x = 0; x < rw; ++x) {
                            int32_t tx = dx + x;
                            if (tx < 0 || tx >= surf->width) continue;
                            size_t o = ((size_t)y * (size_t)rw + (size_t)x) * 4u;
                            drow[tx] = ((uint32_t)rgba[o + 3u] << 24) |
                                       ((uint32_t)rgba[o + 0u] << 16) |
                                       ((uint32_t)rgba[o + 1u] << 8)  |
                                       (uint32_t)rgba[o + 2u];
                        }
                    }
                    vm->host.doc->dom_dirty = true;
                }
                JS_FreeValue(ctx, ab);
            } else {
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
            break;
        }
        case 25: { /* drawImage(uid, op, srcUid, dx, dy, dw, dh) */
            if (argc < 7) break;
            uint32_t src_uid = (uint32_t)a0;
            ArcrDomNode *sn = js_dom_find_uid(vm, src_uid);
            if (sn && sn->surface && sn->surface->argb_pixels &&
                sn->surface->width > 0 && sn->surface->height > 0) {
                int32_t dx = (int32_t)a1, dy = (int32_t)a2;
                int32_t dw = (a3 > 0.0) ? (int32_t)a3 : sn->surface->width;
                int32_t dh = (a4 > 0.0) ? (int32_t)a4 : sn->surface->height;
                for (int32_t y = 0; y < dh; ++y) {
                    int32_t ty = dy + y;
                    if (ty < 0 || ty >= surf->height) continue;
                    int32_t sy = (int32_t)(((int64_t)y * sn->surface->height) / ARCR_MAX(1, dh));
                    const uint32_t *srow = sn->surface->argb_pixels + (size_t)sy * (size_t)sn->surface->width;
                    uint32_t *drow = surf->argb_pixels + (size_t)ty * (size_t)surf->width;
                    for (int32_t x = 0; x < dw; ++x) {
                        int32_t tx = dx + x;
                        if (tx < 0 || tx >= surf->width) continue;
                        int32_t sx = (int32_t)(((int64_t)x * sn->surface->width) / ARCR_MAX(1, dw));
                        canvas_blend_px(&drow[tx], canvas_apply_alpha(srow[sx], c->cur.global_alpha));
                    }
                }
                vm->host.doc->dom_dirty = true;
            }
            break;
        }
        case 26: { /* toDataURL() */
            uint8_t *png = NULL;
            size_t png_len = 0u;
            if (arcr_image_encode_png_mem(surf->argb_pixels, surf->width, surf->height,
                                          surf->width, &png, &png_len) && png) {
                char *uri = js_base64_encode_png_uri(png, png_len);
                free(png);
                if (uri) {
                    JSValue v = JS_NewString(ctx, uri);
                    free(uri);
                    return v;
                }
            }
            return JS_NewString(ctx, "data:image/png;base64,");
        }
        default: break;
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry kBridgeFuncs[] = {
    JS_CFUNC_DEF("allocNodeObj",     2, b_alloc_node_obj),
    JS_CFUNC_DEF("nodeUidOf",        1, b_node_uid_of),
    JS_CFUNC_DEF("nodeInfo",         1, b_node_info),
    JS_CFUNC_DEF("docSpecialUids",   0, b_doc_special_uids),
    JS_CFUNC_DEF("getTitle",         0, b_get_title),
    JS_CFUNC_DEF("setTitle",         1, b_set_title),
    JS_CFUNC_DEF("getById",          1, b_get_by_id),
    JS_CFUNC_DEF("querySelector",    3, b_query_selector),
    JS_CFUNC_DEF("getByTagOrCls",    3, b_get_by_tag_or_cls),
    JS_CFUNC_DEF("matchesOrClosest", 3, b_matches_or_closest),
    JS_CFUNC_DEF("contains",         2, b_contains),
    JS_CFUNC_DEF("nav",              2, b_nav),
    JS_CFUNC_DEF("childUids",        2, b_child_uids),
    JS_CFUNC_DEF("ancestorChain",    1, b_ancestor_chain),
    JS_CFUNC_DEF("createNode",       2, b_create_node),
    JS_CFUNC_DEF("insertBefore",     3, b_insert_before),
    JS_CFUNC_DEF("removeNode",       1, b_remove_node),
    JS_CFUNC_DEF("cloneNode",        2, b_clone_node),
    JS_CFUNC_DEF("getText",          1, b_get_text),
    JS_CFUNC_DEF("setText",          2, b_set_text),
    JS_CFUNC_DEF("getHtml",          2, b_get_html),
    JS_CFUNC_DEF("setInnerHtml",     2, b_set_inner_html),
    JS_CFUNC_DEF("attrOp",           4, b_attr_op),
    JS_CFUNC_DEF("formState",        3, b_form_state),
    JS_CFUNC_DEF("computedInfo",     1, b_computed_info),
    JS_CFUNC_DEF("log",              1, b_console_log),
    JS_CFUNC_DEF("envInfo",          0, b_env_info),
    JS_CFUNC_DEF("navigate",         1, b_navigate),
    JS_CFUNC_DEF("storageOp",        3, b_storage_op),
    JS_CFUNC_DEF("httpFetch",        4, b_http_fetch),
    JS_CFUNC_DEF("randomBytes",      1, b_random_bytes),
    JS_CFUNC_DEF("clipboardOp",      2, b_clipboard_op),
    JS_CFUNC_DEF("markDirty",        0, b_mark_dirty),
    JS_CFUNC_DEF("canvasCmd",        9, b_canvas_cmd)
};

/* -------------------------------------------------------------------------
 * 6. ES2026 Browser Web APIs Bootstrap (Chunked < 2400 chars for -Wpedantic)
 * ------------------------------------------------------------------------- */
static const char *const kWebBootstrapChunks[] = {
    /* Chunk 0: Global aliases, Console & Event Hierarchy */
    "(function(B){\n"
    "'use strict';\n"
    "const G = globalThis;\n"
    "G.window = G; G.self = G; G.top = G; G.parent = G; G.frames = G;\n"
    "const fmtVal = (v, depth = 0) => {\n"
    "  if (v === undefined) return 'undefined';\n"
    "  if (v === null) return 'null';\n"
    "  if (typeof v === 'string') return depth > 0 ? JSON.stringify(v) : v;\n"
    "  if (typeof v === 'number' || typeof v === 'boolean' || typeof v === 'bigint') return String(v);\n"
    "  if (v instanceof Error) return (v.stack || (v.name + ': ' + v.message));\n"
    "  if (typeof v === 'function') return '[Function ' + (v.name || 'anonymous') + ']';\n"
    "  if (depth > 2) return '[Object]';\n"
    "  if (Array.isArray(v)) return '[' + v.map(x => fmtVal(x, depth + 1)).join(', ') + ']';\n"
    "  try { return JSON.stringify(v); } catch (_) { return String(v); }\n"
    "};\n"
    "const timersPerf = new Map();\n"
    "const mkLog = (pfx) => (...args) => B.log((pfx ? pfx + ' ' : '') + args.map(a => fmtVal(a, 0)).join(' '));\n"
    "G.console = {\n"
    "  log: mkLog(''), info: mkLog('[INFO]'), warn: mkLog('[WARN]'),\n"
    "  error: mkLog('[ERROR]'), debug: mkLog('[DEBUG]'), trace: mkLog('[TRACE]'),\n"
    "  dir: (o) => B.log(fmtVal(o, 1)), clear: () => {}, group: mkLog('[GROUP]'), groupEnd: () => {},\n"
    "  assert: (cond, ...rest) => { if (!cond) B.log('[ASSERT] ' + rest.map(a => fmtVal(a, 0)).join(' ')); },\n"
    "  time: (l = 'default') => timersPerf.set(String(l), B.envInfo().nowMs),\n"
    "  timeEnd: (l = 'default') => {\n"
    "    const s = timersPerf.get(String(l)); timersPerf.delete(String(l));\n"
    "    if (s !== undefined) B.log(l + ': ' + (B.envInfo().nowMs - s).toFixed(2) + 'ms');\n"
    "  }\n"
    "};\n",

    /* Chunk 1: Event, CustomEvent, MouseEvent, KeyboardEvent, InputEvent, EventTarget */
    "const evStore = new WeakMap();\n"
    "class Event {\n"
    "  constructor(type, init = {}) {\n"
    "    this.type = String(type);\n"
    "    this.bubbles = !!init.bubbles;\n"
    "    this.cancelable = init.cancelable !== undefined ? !!init.cancelable : true;\n"
    "    this.composed = !!init.composed;\n"
    "    this.target = null; this.currentTarget = null; this.srcElement = null;\n"
    "    this.eventPhase = 0; this.defaultPrevented = false; this.returnValue = true;\n"
    "    this.isTrusted = false; this.timeStamp = B.envInfo().nowMs;\n"
    "    this._stop = false; this._stopImm = false; this._path = [];\n"
    "  }\n"
    "  preventDefault() { if (this.cancelable) { this.defaultPrevented = true; this.returnValue = false; } }\n"
    "  stopPropagation() { this._stop = true; }\n"
    "  stopImmediatePropagation() { this._stop = true; this._stopImm = true; }\n"
    "  composedPath() { return this._path.slice(); }\n"
    "  initEvent(t, b, c) { this.type = String(t); this.bubbles = !!b; this.cancelable = !!c; }\n"
    "}\n"
    "Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;\n"
    "class CustomEvent extends Event { constructor(t, i = {}) { super(t, i); this.detail = i.detail !== undefined ? i.detail : null; } }\n"
    "class UIEvent extends Event { constructor(t, i = {}) { super(t, i); this.view = i.view || G; this.detail = i.detail || 0; } }\n"
    "class MouseEvent extends UIEvent {\n"
    "  constructor(t, i = {}) {\n"
    "    super(t, i); this.clientX = i.clientX || 0; this.clientY = i.clientY || 0;\n"
    "    this.pageX = this.clientX + (G.scrollX || 0); this.pageY = this.clientY + (G.scrollY || 0);\n"
    "    this.screenX = this.clientX; this.screenY = this.clientY;\n"
    "    this.button = i.button || 0; this.buttons = i.buttons || 0;\n"
    "    this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey;\n"
    "  }\n"
    "}\n"
    "class PointerEvent extends MouseEvent { constructor(t, i = {}) { super(t, i); this.pointerId = i.pointerId || 1; this.pointerType = i.pointerType || 'mouse'; } }\n"
    "class KeyboardEvent extends UIEvent {\n"
    "  constructor(t, i = {}) {\n"
    "    super(t, i); this.key = i.key || ''; this.code = i.code || '';\n"
    "    this.keyCode = i.keyCode || 0; this.which = i.which || this.keyCode;\n"
    "    this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey;\n"
    "  }\n"
    "}\n"
    "class InputEvent extends UIEvent { constructor(t, i = {}) { super(t, i); this.data = i.data !== undefined ? i.data : null; this.inputType = i.inputType || 'insertText'; } }\n"
    "class FocusEvent extends UIEvent { constructor(t, i = {}) { super(t, i); this.relatedTarget = i.relatedTarget || null; } }\n"
    "Object.assign(G, { Event, CustomEvent, UIEvent, MouseEvent, PointerEvent, KeyboardEvent, InputEvent, FocusEvent });\n",

    /* Chunk 2: EventTarget & W3C Capture/Target/Bubble Dispatcher */
    "const getListeners = (obj) => { let m = evStore.get(obj); if (!m) { m = new Map(); evStore.set(obj, m); } return m; };\n"
    "class EventTarget {\n"
    "  addEventListener(type, cb, opts) {\n"
    "    if (!cb || (typeof cb !== 'function' && typeof cb.handleEvent !== 'function')) return;\n"
    "    const t = String(type);\n"
    "    const cap = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);\n"
    "    const once = !!(opts && typeof opts === 'object' && opts.once);\n"
    "    const sig = (opts && typeof opts === 'object') ? opts.signal : null;\n"
    "    if (sig && sig.aborted) return;\n"
    "    const m = getListeners(this); let arr = m.get(t); if (!arr) { arr = []; m.set(t, arr); }\n"
    "    if (arr.some(e => e.cb === cb && e.cap === cap)) return;\n"
    "    const entry = { cb, cap, once };\n"
    "    arr.push(entry);\n"
    "    if (sig && typeof sig.addEventListener === 'function') {\n"
    "      sig.addEventListener('abort', () => this.removeEventListener(t, cb, cap), { once: true });\n"
    "    }\n"
    "  }\n"
    "  removeEventListener(type, cb, opts) {\n"
    "    const cap = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);\n"
    "    const m = evStore.get(this); if (!m) return;\n"
    "    const arr = m.get(String(type)); if (!arr) return;\n"
    "    const idx = arr.findIndex(e => e.cb === cb && e.cap === cap);\n"
    "    if (idx >= 0) arr.splice(idx, 1);\n"
    "  }\n"
    "  dispatchEvent(ev) { return runEventDispatch(this, ev).notCanceled; }\n"
    "}\n"
    "G.EventTarget = EventTarget;\n"
    "G.addEventListener = EventTarget.prototype.addEventListener.bind(G);\n"
    "G.removeEventListener = EventTarget.prototype.removeEventListener.bind(G);\n"
    "G.dispatchEvent = EventTarget.prototype.dispatchEvent.bind(G);\n",

    /* Chunk 3: Event propagation engine + inline on* attribute support */
    "const uidMap = new Map();\n"
    "let wrapUid = null;\n"
    "const invokeOnTarget = (node, ev, phase) => {\n"
    "  let ran = false;\n"
    "  ev.currentTarget = node; ev.eventPhase = phase;\n"
    "  const m = evStore.get(node);\n"
    "  const list = m && m.get(ev.type) ? m.get(ev.type).slice() : [];\n"
    "  for (const item of list) {\n"
    "    if (phase === 1 && !item.cap) continue;\n"
    "    if (phase === 3 && item.cap) continue;\n"
    "    if (item.once) node.removeEventListener(ev.type, item.cb, item.cap);\n"
    "    ran = true;\n"
    "    try {\n"
    "      if (typeof item.cb === 'function') item.cb.call(node, ev);\n"
    "      else if (item.cb && typeof item.cb.handleEvent === 'function') item.cb.handleEvent(ev);\n"
    "    } catch (err) { B.log('[Uncaught Event Error] ' + fmtVal(err)); }\n"
    "    if (ev._stopImm) return ran;\n"
    "  }\n"
    "  if (phase !== 1) {\n"
    "    const propName = 'on' + ev.type;\n"
    "    const fnProp = node[propName];\n"
    "    if (typeof fnProp === 'function') {\n"
    "      ran = true;\n"
    "      try { if (fnProp.call(node, ev) === false) ev.preventDefault(); }\n"
    "      catch (err) { B.log('[Uncaught Handler Error] ' + fmtVal(err)); }\n"
    "    } else if (node && typeof node.getAttribute === 'function') {\n"
    "      const code = node.getAttribute(propName);\n"
    "      if (code) {\n"
    "        ran = true;\n"
    "        try { const fn = new Function('event', code); if (fn.call(node, ev) === false) ev.preventDefault(); }\n"
    "        catch (err) { B.log('[Uncaught Inline Handler] ' + fmtVal(err)); }\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  return ran;\n"
    "};\n"
    "const runEventDispatch = (target, ev) => {\n"
    "  if (!ev || typeof ev.type !== 'string') return { handled: false, notCanceled: true };\n"
    "  ev.target = target; ev.srcElement = target; ev._stop = false; ev._stopImm = false;\n"
    "  const path = [];\n"
    "  const tuid = B.nodeUidOf(target);\n"
    "  if (tuid >= 0) {\n"
    "    const chain = B.ancestorChain(tuid);\n"
    "    for (let i = 0; i < chain.length; i++) { const w = wrapUid(chain[i]); if (w) path.push(w); }\n"
    "    if (path[path.length - 1] !== G.document && G.document) path.push(G.document);\n"
    "    path.push(G);\n"
    "  } else { path.push(target); }\n"
    "  ev._path = path;\n"
    "  let handled = false;\n"
    "  for (let i = path.length - 1; i >= 1; i--) {\n"
    "    if (invokeOnTarget(path[i], ev, 1)) handled = true;\n"
    "    if (ev._stop) break;\n"
    "  }\n"
    "  if (!ev._stop && path.length > 0) {\n"
    "    if (invokeOnTarget(path[0], ev, 2)) handled = true;\n"
    "  }\n"
    "  if (ev.bubbles && !ev._stop) {\n"
    "    for (let i = 1; i < path.length; i++) {\n"
    "      if (invokeOnTarget(path[i], ev, 3)) handled = true;\n"
    "      if (ev._stop) break;\n"
    "    }\n"
    "  }\n"
    "  ev.currentTarget = null; ev.eventPhase = 0;\n"
    "  return { handled, notCanceled: !ev.defaultPrevented };\n"
    "};\n",

    /* Chunk 4: NodeList, HTMLCollection, DOMTokenList, CSSStyleDeclaration & dataset Proxies */
    "class NodeList {\n"
    "  constructor(items = []) {\n"
    "    this._items = items;\n"
    "    return new Proxy(this, {\n"
    "      get(t, p, r) {\n"
    "        if (typeof p === 'string') {\n"
    "          const n = Number(p);\n"
    "          if (Number.isInteger(n) && n >= 0 && n < t._items.length) return t._items[n];\n"
    "        }\n"
    "        return Reflect.get(t, p, r);\n"
    "      },\n"
    "      has(t, p) {\n"
    "        if (typeof p === 'string') { const n = Number(p); if (Number.isInteger(n) && n >= 0 && n < t._items.length) return true; }\n"
    "        return Reflect.has(t, p);\n"
    "      }\n"
    "    });\n"
    "  }\n"
    "  get length() { return this._items.length; }\n"
    "  item(i) { return this._items[i] || null; }\n"
    "  forEach(cb, thisArg) { this._items.forEach((v, i) => cb.call(thisArg, v, i, this)); }\n"
    "  entries() { return this._items.entries(); }\n"
    "  keys() { return this._items.keys(); }\n"
    "  values() { return this._items.values(); }\n"
    "  [Symbol.iterator]() { return this._items[Symbol.iterator](); }\n"
    "}\n"
    "G.NodeList = NodeList; G.HTMLCollection = NodeList;\n"
    "const camelToKebab = (s) => s.startsWith('--') ? s : s.replace(/[A-Z]/g, m => '-' + m.toLowerCase());\n"
    "const kebabToCamel = (s) => s.replace(/-([a-z])/g, (_, c) => c.toUpperCase());\n"
    "const parseInlineStyle = (raw) => {\n"
    "  const map = new Map(); if (!raw) return map;\n"
    "  for (const decl of String(raw).split(';')) {\n"
    "    const idx = decl.indexOf(':'); if (idx <= 0) continue;\n"
    "    const k = decl.slice(0, idx).trim(); const v = decl.slice(idx + 1).trim();\n"
    "    if (k) map.set(k.startsWith('--') ? k : k.toLowerCase(), v);\n"
    "  }\n"
    "  return map;\n"
    "};\n"
    "const serializeInlineStyle = (map) => { const out = []; for (const [k, v] of map) { if (v !== '') out.push(k + ': ' + v); } return out.join('; '); };\n"
    "const makeStyleProxy = (uid) => {\n"
    "  const api = {\n"
    "    get cssText() { return B.attrOp(uid, 0, 'style') || ''; },\n"
    "    set cssText(v) { B.attrOp(uid, 1, 'style', String(v || '')); },\n"
    "    getPropertyValue(p) { const m = parseInlineStyle(B.attrOp(uid, 0, 'style')); return m.get(camelToKebab(String(p))) || ''; },\n"
    "    setProperty(p, v, prio) {\n"
    "      const m = parseInlineStyle(B.attrOp(uid, 0, 'style')); const k = camelToKebab(String(p));\n"
    "      if (v === null || v === undefined || v === '') m.delete(k);\n"
    "      else m.set(k, String(v) + (prio === 'important' ? ' !important' : ''));\n"
    "      B.attrOp(uid, 1, 'style', serializeInlineStyle(m));\n"
    "    },\n"
    "    removeProperty(p) {\n"
    "      const m = parseInlineStyle(B.attrOp(uid, 0, 'style')); const k = camelToKebab(String(p));\n"
    "      const old = m.get(k) || ''; m.delete(k); B.attrOp(uid, 1, 'style', serializeInlineStyle(m)); return old;\n"
    "    }\n"
    "  };\n"
    "  return new Proxy(api, {\n"
    "    get(t, p) { if (p in t || typeof p === 'symbol') return t[p]; return t.getPropertyValue(String(p)); },\n"
    "    set(t, p, v) { if (p === 'cssText') { t.cssText = v; return true; } t.setProperty(String(p), v); return true; }\n"
    "  });\n"
    "};\n",

    /* Chunk 5: DOMTokenList & dataset Proxy + Node class */
    "class DOMTokenList {\n"
    "  constructor(uid) { this._u = uid; }\n"
    "  _list() { const c = B.attrOp(this._u, 0, 'class') || ''; return c.trim() ? c.trim().split(/\\s+/) : []; }\n"
    "  _save(arr) { B.attrOp(this._u, 1, 'class', arr.join(' ')); }\n"
    "  get length() { return this._list().length; }\n"
    "  get value() { return B.attrOp(this._u, 0, 'class') || ''; }\n"
    "  set value(v) { B.attrOp(this._u, 1, 'class', String(v)); }\n"
    "  item(i) { return this._list()[i] || null; }\n"
    "  contains(tok) { return this._list().includes(String(tok)); }\n"
    "  add(...toks) { const s = this._list(); for (const t of toks) { const k = String(t); if (k && !s.includes(k)) s.push(k); } this._save(s); }\n"
    "  remove(...toks) { const rem = new Set(toks.map(String)); this._save(this._list().filter(x => !rem.has(x))); }\n"
    "  toggle(tok, force) {\n"
    "    const k = String(tok); const has = this.contains(k);\n"
    "    const want = force !== undefined ? !!force : !has;\n"
    "    if (want) this.add(k); else this.remove(k); return want;\n"
    "  }\n"
    "  replace(oldT, newT) { const s = this._list(); const i = s.indexOf(String(oldT)); if (i < 0) return false; s[i] = String(newT); this._save(s); return true; }\n"
    "  forEach(cb, thisArg) { this._list().forEach((v, i) => cb.call(thisArg, v, i, this)); }\n"
    "  toString() { return this.value; }\n"
    "  [Symbol.iterator]() { return this._list()[Symbol.iterator](); }\n"
    "}\n"
    "const makeDatasetProxy = (uid) => new Proxy({}, {\n"
    "  get(_, p) { if (typeof p !== 'string') return undefined; const v = B.attrOp(uid, 0, 'data-' + camelToKebab(p)); return v === null ? undefined : v; },\n"
    "  set(_, p, v) { if (typeof p === 'string') B.attrOp(uid, 1, 'data-' + camelToKebab(p), String(v)); return true; },\n"
    "  has(_, p) { return typeof p === 'string' && B.attrOp(uid, 2, 'data-' + camelToKebab(p)); },\n"
    "  deleteProperty(_, p) { if (typeof p === 'string') B.attrOp(uid, 4, 'data-' + camelToKebab(p)); return true; },\n"
    "  ownKeys() { return B.attrOp(uid, 3).filter(n => n.startsWith('data-')).map(n => kebabToCamel(n.slice(5))); },\n"
    "  getOwnPropertyDescriptor(_, p) {\n"
    "    if (typeof p === 'string' && B.attrOp(uid, 2, 'data-' + camelToKebab(p))) {\n"
    "      return { configurable: true, enumerable: true, writable: true, value: B.attrOp(uid, 0, 'data-' + camelToKebab(p)) };\n"
    "    }\n"
    "    return undefined;\n"
    "  }\n"
    "});\n",

    /* Chunk 6: Node & Element classes */
    "class Node extends EventTarget {\n"
    "  get _uid() { return B.nodeUidOf(this); }\n"
    "  get nodeType() { const i = B.nodeInfo(this._uid); return i ? i.nodeType : 1; }\n"
    "  get nodeName() { const i = B.nodeInfo(this._uid); if (!i) return ''; return i.nodeType === 1 ? i.tag.toUpperCase() : i.tag; }\n"
    "  get isConnected() { const i = B.nodeInfo(this._uid); return !!(i && i.connected); }\n"
    "  get ownerDocument() { return this.nodeType === 9 ? null : G.document; }\n"
    "  get parentNode() { return wrapUid(B.nav(this._uid, 0)); }\n"
    "  get parentElement() { return wrapUid(B.nav(this._uid, 1)); }\n"
    "  get firstChild() { return wrapUid(B.nav(this._uid, 2)); }\n"
    "  get lastChild() { return wrapUid(B.nav(this._uid, 3)); }\n"
    "  get nextSibling() { return wrapUid(B.nav(this._uid, 6)); }\n"
    "  get previousSibling() { return wrapUid(B.nav(this._uid, 7)); }\n"
    "  get childNodes() { return new NodeList(B.childUids(this._uid, false).map(wrapUid).filter(Boolean)); }\n"
    "  hasChildNodes() { return B.nav(this._uid, 2) !== 0; }\n"
    "  get textContent() { return B.getText(this._uid); }\n"
    "  set textContent(v) { B.setText(this._uid, v === null || v === undefined ? '' : String(v)); }\n"
    "  get nodeValue() { const t = this.nodeType; return (t === 3 || t === 8) ? B.getText(this._uid) : null; }\n"
    "  set nodeValue(v) { const t = this.nodeType; if (t === 3 || t === 8) B.setText(this._uid, String(v ?? '')); }\n"
    "  appendChild(ch) { if (ch) B.insertBefore(this._uid, ch._uid, 0); return ch; }\n"
    "  insertBefore(ch, ref) { if (ch) B.insertBefore(this._uid, ch._uid, ref ? ref._uid : 0); return ch; }\n"
    "  removeChild(ch) { if (ch) B.removeNode(ch._uid); return ch; }\n"
    "  replaceChild(nw, old) { if (nw && old) { B.insertBefore(this._uid, nw._uid, old._uid); B.removeNode(old._uid); } return old; }\n"
    "  cloneNode(deep = false) { return wrapUid(B.cloneNode(this._uid, !!deep)); }\n"
    "  contains(other) { return !!(other && B.contains(this._uid, other._uid)); }\n"
    "  getRootNode() { return this.isConnected ? G.document : this; }\n"
    "}\n"
    "Node.ELEMENT_NODE = 1; Node.TEXT_NODE = 3; Node.COMMENT_NODE = 8; Node.DOCUMENT_NODE = 9; Node.DOCUMENT_FRAGMENT_NODE = 11;\n"
    "G.Node = Node;\n",

    /* Chunk 7: Element, HTMLElement, HTMLCanvasElement, HTMLMediaElement */
    "const coerceNode = (x) => (x instanceof Node) ? x : wrapUid(B.createNode(3, String(x)));\n"
    "class Element extends Node {\n"
    "  get tagName() { const i = B.nodeInfo(this._uid); return i ? i.tag.toUpperCase() : 'DIV'; }\n"
    "  get localName() { const i = B.nodeInfo(this._uid); return i ? i.tag : 'div'; }\n"
    "  get id() { return B.attrOp(this._uid, 0, 'id') || ''; }\n"
    "  set id(v) { B.attrOp(this._uid, 1, 'id', String(v)); }\n"
    "  get className() { return B.attrOp(this._uid, 0, 'class') || ''; }\n"
    "  set className(v) { B.attrOp(this._uid, 1, 'class', String(v ?? '')); }\n"
    "  get classList() { if (!this._cl) this._cl = new DOMTokenList(this._uid); return this._cl; }\n"
    "  get innerHTML() { return B.getHtml(this._uid, false); }\n"
    "  set innerHTML(v) { B.setInnerHtml(this._uid, String(v ?? '')); notifyMutations(); }\n"
    "  get outerHTML() { return B.getHtml(this._uid, true); }\n"
    "  set outerHTML(v) {\n"
    "    const p = this.parentNode; if (!p) return;\n"
    "    const f = wrapUid(B.createNode(11, '')); B.setInnerHtml(f._uid, String(v ?? ''));\n"
    "    p.insertBefore(f, this); B.removeNode(this._uid); notifyMutations();\n"
    "  }\n"
    "  insertAdjacentHTML(pos, html) {\n"
    "    const p = String(pos).toLowerCase(); const f = wrapUid(B.createNode(11, '')); B.setInnerHtml(f._uid, String(html ?? ''));\n"
    "    if (p === 'beforeend') this.appendChild(f);\n"
    "    else if (p === 'afterbegin') this.insertBefore(f, this.firstChild);\n"
    "    else if (p === 'beforebegin' && this.parentNode) this.parentNode.insertBefore(f, this);\n"
    "    else if (p === 'afterend' && this.parentNode) this.parentNode.insertBefore(f, this.nextSibling);\n"
    "    notifyMutations();\n"
    "  }\n"
    "  insertAdjacentElement(pos, el) {\n"
    "    const p = String(pos).toLowerCase();\n"
    "    if (p === 'beforeend') this.appendChild(el);\n"
    "    else if (p === 'afterbegin') this.insertBefore(el, this.firstChild);\n"
    "    else if (p === 'beforebegin' && this.parentNode) this.parentNode.insertBefore(el, this);\n"
    "    else if (p === 'afterend' && this.parentNode) this.parentNode.insertBefore(el, this.nextSibling);\n"
    "    return el;\n"
    "  }\n"
    "  getAttribute(n) { return B.attrOp(this._uid, 0, String(n)); }\n",
    "  setAttribute(n, v) { B.attrOp(this._uid, 1, String(n), String(v)); notifyMutations(); }\n"
    "  hasAttribute(n) { return B.attrOp(this._uid, 2, String(n)); }\n"
    "  removeAttribute(n) { B.attrOp(this._uid, 4, String(n)); notifyMutations(); }\n"
    "  toggleAttribute(n, force) {\n"
    "    const has = this.hasAttribute(n); const want = force !== undefined ? !!force : !has;\n"
    "    if (want) this.setAttribute(n, ''); else this.removeAttribute(n); return want;\n"
    "  }\n"
    "  getAttributeNames() { return B.attrOp(this._uid, 3); }\n"
    "  get attributes() { return B.attrOp(this._uid, 3).map(name => ({ name, value: this.getAttribute(name) })); }\n"
    "  querySelector(s) { const u = B.querySelector(this._uid, String(s), true); return u.length ? wrapUid(u[0]) : null; }\n"
    "  querySelectorAll(s) { return new NodeList(B.querySelector(this._uid, String(s), false).map(wrapUid).filter(Boolean)); }\n"
    "  getElementsByTagName(t) { return new NodeList(B.getByTagOrCls(this._uid, true, String(t)).map(wrapUid).filter(Boolean)); }\n"
    "  getElementsByClassName(c) { return new NodeList(B.getByTagOrCls(this._uid, false, String(c)).map(wrapUid).filter(Boolean)); }\n"
    "  matches(s) { return B.matchesOrClosest(this._uid, String(s), false) !== 0; }\n"
    "  closest(s) { return wrapUid(B.matchesOrClosest(this._uid, String(s), true)); }\n"
    "  get children() { return new NodeList(B.childUids(this._uid, true).map(wrapUid).filter(Boolean)); }\n"
    "  get childElementCount() { return B.childUids(this._uid, true).length; }\n"
    "  get firstElementChild() { return wrapUid(B.nav(this._uid, 4)); }\n"
    "  get lastElementChild() { return wrapUid(B.nav(this._uid, 5)); }\n"
    "  get nextElementSibling() { return wrapUid(B.nav(this._uid, 8)); }\n"
    "  get previousElementSibling() { return wrapUid(B.nav(this._uid, 9)); }\n"
    "  append(...ns) { for (const n of ns) this.appendChild(coerceNode(n)); notifyMutations(); }\n"
    "  prepend(...ns) { const f = this.firstChild; for (const n of ns) this.insertBefore(coerceNode(n), f); notifyMutations(); }\n"
    "  before(...ns) { const p = this.parentNode; if (p) for (const n of ns) p.insertBefore(coerceNode(n), this); }\n"
    "  after(...ns) { const p = this.parentNode, nx = this.nextSibling; if (p) for (const n of ns) p.insertBefore(coerceNode(n), nx); }\n"
    "  replaceWith(...ns) { this.before(...ns); this.remove(); }\n"
    "  remove() { B.removeNode(this._uid); notifyMutations(); }\n"
    "  getBoundingClientRect() {\n"
    "    const c = B.computedInfo(this._uid);\n"
    "    return { x: 0, y: 0, top: 0, left: 0, width: c.width, height: c.height, right: c.width, bottom: c.height, toJSON() { return this; } };\n"
    "  }\n"
    "  getClientRects() { return [this.getBoundingClientRect()]; }\n"
    "  scrollIntoView() {}\n"
    "  attachShadow() { if (!this._sr) this._sr = this; return this._sr; }\n"
    "  get shadowRoot() { return this._sr || null; }\n"
    "}\n",

    /* Chunk 8: HTMLElement, HTMLCanvasElement, CanvasRenderingContext2D, HTMLMediaElement */
    "class HTMLElement extends Element {\n"
    "  get style() { if (!this._st) this._st = makeStyleProxy(this._uid); return this._st; }\n"
    "  set style(v) { this.style.cssText = String(v ?? ''); }\n"
    "  get dataset() { if (!this._ds) this._ds = makeDatasetProxy(this._uid); return this._ds; }\n"
    "  get innerText() { return this.textContent; }\n"
    "  set innerText(v) { this.textContent = v; }\n"
    "  get value() { return B.formState(this._uid, 0); }\n"
    "  set value(v) { B.formState(this._uid, 1, String(v ?? '')); }\n"
    "  get checked() { return B.formState(this._uid, 2); }\n"
    "  set checked(v) { B.formState(this._uid, 3, !!v); }\n"
    "  get disabled() { return this.hasAttribute('disabled'); }\n"
    "  set disabled(v) { if (v) this.setAttribute('disabled', ''); else this.removeAttribute('disabled'); }\n"
    "  get hidden() { return this.hasAttribute('hidden'); }\n"
    "  set hidden(v) { if (v) this.setAttribute('hidden', ''); else this.removeAttribute('hidden'); }\n"
    "  get href() { return this.getAttribute('href') || ''; }\n"
    "  set href(v) { this.setAttribute('href', v); }\n"
    "  get src() { return this.getAttribute('src') || ''; }\n"
    "  set src(v) { this.setAttribute('src', v); }\n"
    "  get type() { return this.getAttribute('type') || ''; }\n"
    "  set type(v) { this.setAttribute('type', v); }\n"
    "  get name() { return this.getAttribute('name') || ''; }\n"
    "  set name(v) { this.setAttribute('name', v); }\n"
    "  get offsetWidth() { return B.computedInfo(this._uid).width; }\n"
    "  get offsetHeight() { return B.computedInfo(this._uid).height; }\n"
    "  get clientWidth() { return this.offsetWidth; }\n"
    "  get clientHeight() { return this.offsetHeight; }\n"
    "  get scrollWidth() { return this.offsetWidth; }\n"
    "  get scrollHeight() { return this.offsetHeight; }\n"
    "  focus() { B.formState(this._uid, 4, true); this.dispatchEvent(new FocusEvent('focus')); }\n"
    "  blur() { B.formState(this._uid, 4, false); this.dispatchEvent(new FocusEvent('blur')); }\n"
    "  click() { this.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true })); }\n"
    "}\n"
    "class CanvasGradient {\n"
    "  constructor(id) { this._gid = id; }\n"
    "  addColorStop(off, col) { B.canvasCmd(0, 101, this._gid, Number(off), String(col)); }\n"
    "}\n"
    "class ImageData {\n"
    "  constructor(a, b, c) {\n"
    "    if (a instanceof Uint8ClampedArray) { this.data = a; this.width = b | 0; this.height = (c || (a.length / (4 * this.width))) | 0; }\n"
    "    else { this.width = a | 0; this.height = b | 0; this.data = new Uint8ClampedArray(this.width * this.height * 4); }\n"
    "  }\n"
    "}\n"
    "G.CanvasGradient = CanvasGradient; G.ImageData = ImageData;\n",

    /* Chunk 9: CanvasRenderingContext2D, HTMLCanvasElement & HTMLMediaElement */
    "class CanvasRenderingContext2D {\n"
    "  constructor(canvas) {\n"
    "    this.canvas = canvas; this._u = canvas._uid;\n"
    "    this._fill = '#000000'; this._stroke = '#000000'; this._lw = 1; this._ga = 1;\n"
    "    this._font = '14px sans-serif'; this.textAlign = 'start'; this.textBaseline = 'alphabetic';\n"
    "    this.lineCap = 'butt'; this.lineJoin = 'miter'; this.globalCompositeOperation = 'source-over';\n"
    "  }\n"
    "  get fillStyle() { return this._fill; }\n"
    "  set fillStyle(v) { this._fill = v; B.canvasCmd(this._u, 1, false, v instanceof CanvasGradient ? v._gid : String(v)); }\n"
    "  get strokeStyle() { return this._stroke; }\n"
    "  set strokeStyle(v) { this._stroke = v; B.canvasCmd(this._u, 1, true, v instanceof CanvasGradient ? v._gid : String(v)); }\n"
    "  get lineWidth() { return this._lw; }\n"
    "  set lineWidth(v) { this._lw = Number(v); B.canvasCmd(this._u, 2, this._lw); }\n"
    "  get globalAlpha() { return this._ga; }\n"
    "  set globalAlpha(v) { this._ga = Number(v); B.canvasCmd(this._u, 3, this._ga); }\n"
    "  get font() { return this._font; }\n"
    "  set font(v) { this._font = String(v); B.canvasCmd(this._u, 4, this._font); }\n"
    "  save() { B.canvasCmd(this._u, 5); }\n"
    "  restore() { B.canvasCmd(this._u, 6); }\n"
    "  translate(x, y) { B.canvasCmd(this._u, 7, +x, +y); }\n"
    "  scale(x, y) { B.canvasCmd(this._u, 8, +x, +y); }\n"
    "  rotate(a) { B.canvasCmd(this._u, 9, +a); }\n"
    "  setTransform(a, b, c, d, e, f) { B.canvasCmd(this._u, 10, +a, +b, +c, +d, +e, +f); }\n"
    "  resetTransform() { this.setTransform(1, 0, 0, 1, 0, 0); }\n"
    "  clearRect(x, y, w, h) { B.canvasCmd(this._u, 11, +x, +y, +w, +h); }\n"
    "  fillRect(x, y, w, h) { B.canvasCmd(this._u, 12, +x, +y, +w, +h); }\n"
    "  strokeRect(x, y, w, h) { B.canvasCmd(this._u, 13, +x, +y, +w, +h); }\n"
    "  beginPath() { B.canvasCmd(this._u, 14); }\n"
    "  moveTo(x, y) { B.canvasCmd(this._u, 15, +x, +y); }\n"
    "  lineTo(x, y) { B.canvasCmd(this._u, 16, +x, +y); }\n",
    "  closePath() { B.canvasCmd(this._u, 17); }\n"
    "  rect(x, y, w, h) { B.canvasCmd(this._u, 18, +x, +y, +w, +h); }\n"
    "  roundRect(x, y, w, h) { this.rect(x, y, w, h); }\n"
    "  arc(x, y, r, s, e, ccw = false) { B.canvasCmd(this._u, 19, +x, +y, +r, +s, +e, !!ccw); }\n"
    "  quadraticCurveTo(cx, cy, x, y) { this.lineTo(cx, cy); this.lineTo(x, y); }\n"
    "  bezierCurveTo(c1x, c1y, c2x, c2y, x, y) { this.lineTo(c1x, c1y); this.lineTo(c2x, c2y); this.lineTo(x, y); }\n"
    "  fill() { B.canvasCmd(this._u, 20); }\n"
    "  stroke() { B.canvasCmd(this._u, 21); }\n"
    "  clip() {}\n"
    "  fillText(t, x, y) { B.canvasCmd(this._u, 22, false, String(t), +x, +y); }\n"
    "  strokeText(t, x, y) { this.fillText(t, x, y); }\n"
    "  measureText(t) { const w = B.canvasCmd(this._u, 22, true, String(t), 0, 0) || 0; return { width: w, actualBoundingBoxAscent: 12, actualBoundingBoxDescent: 3 }; }\n"
    "  createLinearGradient(x0, y0, x1, y1) { return new CanvasGradient(B.canvasCmd(0, 100, false, +x0, +y0, 0, +x1, +y1, 0)); }\n"
    "  createRadialGradient(x0, y0, r0, x1, y1, r1) { return new CanvasGradient(B.canvasCmd(0, 100, true, +x0, +y0, +r0, +x1, +y1, +r1)); }\n"
    "  createImageData(w, h) { return new ImageData(w, h); }\n"
    "  getImageData(x, y, w, h) { const ab = B.canvasCmd(this._u, 23, +x, +y, +w, +h); return new ImageData(new Uint8ClampedArray(ab || (w * h * 4)), w, h); }\n"
    "  putImageData(id, dx, dy) { if (id && id.data) B.canvasCmd(this._u, 24, id.data, +dx, +dy, id.width, id.height); }\n"
    "  drawImage(img, dx, dy, dw, dh) { if (img && img._uid) B.canvasCmd(this._u, 25, img._uid, +dx, +dy, dw || 0, dh || 0); }\n"
    "}\n"
    "class HTMLCanvasElement extends HTMLElement {\n"
    "  get width() { return Number(this.getAttribute('width')) || 300; }\n"
    "  set width(v) { const n = Math.max(1, v | 0); this.setAttribute('width', String(n)); B.canvasCmd(this._uid, 0, n, this.height); }\n"
    "  get height() { return Number(this.getAttribute('height')) || 150; }\n"
    "  set height(v) { const n = Math.max(1, v | 0); this.setAttribute('height', String(n)); B.canvasCmd(this._uid, 0, this.width, n); }\n"
    "  getContext(type) {\n"
    "    if (String(type).toLowerCase() !== '2d') return null;\n"
    "    if (!this._ctx2d) { B.canvasCmd(this._uid, 0, this.width, this.height); this._ctx2d = new CanvasRenderingContext2D(this); }\n"
    "    return this._ctx2d;\n"
    "  }\n"
    "  toDataURL() { return B.canvasCmd(this._uid, 26); }\n"
    "}\n"
    "class HTMLMediaElement extends HTMLElement {\n"
    "  constructor() { super(); }\n"
    "  get paused() { return this._paused !== undefined ? this._paused : false; }\n"
    "  get currentTime() { return this._ct || 0; }\n"
    "  set currentTime(v) { this._ct = Math.max(0, Number(v) || 0); this.setAttribute('data-arcr-seek', String(this._ct)); B.markDirty(); }\n"
    "  get duration() { return 180; }\n"
    "  get volume() { return this._vol !== undefined ? this._vol : 1; }\n"
    "  set volume(v) { this._vol = Math.min(1, Math.max(0, Number(v) || 0)); }\n"
    "  get muted() { return !!this._muted; }\n"
    "  set muted(v) { this._muted = !!v; }\n"
    "  get readyState() { return 4; }\n"
    "  play() { this._paused = false; this.setAttribute('data-arcr-paused', '0'); B.markDirty(); this.dispatchEvent(new Event('play')); return Promise.resolve(); }\n"
    "  pause() { this._paused = true; this.setAttribute('data-arcr-paused', '1'); B.markDirty(); this.dispatchEvent(new Event('pause')); }\n"
    "  load() { B.markDirty(); }\n"
    "  canPlayType(t) { const s = String(t || '').toLowerCase(); return (s.includes('mp4') || s.includes('webm') || s.includes('ogg') || s.includes('mpeg')) ? 'probably' : 'maybe'; }\n"
    "}\n"
    "Object.assign(G, { Element, HTMLElement, HTMLCanvasElement, CanvasRenderingContext2D, HTMLMediaElement });\n"
};

static const char *const kWebBootstrapChunks2[] = {
    /* Chunk 10: DocumentFragment, Document, wrapUid & CustomElementRegistry */
    "class DocumentFragment extends Element {}\n"
    "const ceMap = new Map();\n"
    "const ceWait = new Map();\n"
    "G.customElements = {\n"
    "  define(name, ctor) {\n"
    "    const k = String(name).toLowerCase(); ceMap.set(k, ctor);\n"
    "    const w = ceWait.get(k); if (w) { ceWait.delete(k); w.forEach(r => r(ctor)); }\n"
    "  },\n"
    "  get(name) { return ceMap.get(String(name).toLowerCase()); },\n"
    "  whenDefined(name) {\n"
    "    const k = String(name).toLowerCase();\n"
    "    if (ceMap.has(k)) return Promise.resolve(ceMap.get(k));\n"
    "    return new Promise(res => { let a = ceWait.get(k); if (!a) { a = []; ceWait.set(k, a); } a.push(res); });\n"
    "  },\n"
    "  upgrade() {}\n"
    "};\n"
    "let docReadyState = 'interactive';\n"
    "let docCookie = '';\n"
    "class Document extends Element {\n"
    "  get documentElement() { const s = B.docSpecialUids(); return s ? wrapUid(s.html) : null; }\n"
    "  get head() { const s = B.docSpecialUids(); return s ? wrapUid(s.head) : null; }\n"
    "  get body() { const s = B.docSpecialUids(); return s ? wrapUid(s.body) : null; }\n"
    "  get scrollingElement() { return this.documentElement || this.body; }\n"
    "  get activeElement() { const s = B.docSpecialUids(); return s ? wrapUid(s.active) : this.body; }\n"
    "  get title() { return B.getTitle(); }\n"
    "  set title(v) { B.setTitle(String(v ?? '')); }\n"
    "  get readyState() { return docReadyState; }\n"
    "  get visibilityState() { return 'visible'; }\n"
    "  get hidden() { return false; }\n"
    "  get cookie() { return docCookie; }\n"
    "  set cookie(v) { const kv = String(v || '').split(';')[0].trim(); if (kv) docCookie = docCookie ? (docCookie + '; ' + kv) : kv; }\n"
    "  get defaultView() { return G; }\n"
    "  get location() { return G.location; }\n"
    "  get URL() { return G.location.href; }\n"
    "  get documentURI() { return G.location.href; }\n"
    "  get characterSet() { return 'UTF-8'; }\n"
    "  get compatMode() { return 'CSS1Compat'; }\n"
    "  get currentScript() { return null; }\n"
    "  get implementation() { return { createHTMLDocument: (t = '') => { const f = wrapUid(B.createNode(11, '')); f.title = t; return f; }, hasFeature: () => true }; }\n"
    "  getElementById(id) { return wrapUid(B.getById(String(id))); }\n"
    "  getElementsByName(n) { return this.querySelectorAll('[name=\"' + String(n).replace(/\"/g, '') + '\"]'); }\n"
    "  createElement(tag) { return wrapUid(B.createNode(1, String(tag))); }\n"
    "  createElementNS(_ns, tag) { const t = String(tag); const c = t.indexOf(':'); return wrapUid(B.createNode(1, c >= 0 ? t.slice(c + 1) : t)); }\n"
    "  createTextNode(s) { return wrapUid(B.createNode(3, String(s ?? ''))); }\n"
    "  createComment(s) { return wrapUid(B.createNode(8, String(s ?? ''))); }\n"
    "  createDocumentFragment() { return wrapUid(B.createNode(11, '')); }\n"
    "  createEvent() { return new Event(''); }\n"
    "  importNode(n, deep = false) { return n ? n.cloneNode(deep) : null; }\n"
    "  adoptNode(n) { return n; }\n"
    "}\n"
    "wrapUid = (uid) => {\n"
    "  if (!uid) return null;\n"
    "  const ex = uidMap.get(uid); if (ex) return ex;\n"
    "  const info = B.nodeInfo(uid); if (!info) return null;\n"
    "  let proto = Node.prototype;\n"
    "  if (info.nodeType === 9) proto = Document.prototype;\n"
    "  else if (info.nodeType === 11) proto = DocumentFragment.prototype;\n"
    "  else if (info.nodeType === 1) {\n"
    "    if (info.tag === 'canvas') proto = HTMLCanvasElement.prototype;\n"
    "    else if (info.tag === 'video' || info.tag === 'audio') proto = HTMLMediaElement.prototype;\n"
    "    else if (ceMap.has(info.tag)) proto = ceMap.get(info.tag).prototype || HTMLElement.prototype;\n"
    "    else proto = HTMLElement.prototype;\n"
    "  }\n"
    "  const obj = B.allocNodeObj(uid, proto);\n"
    "  uidMap.set(uid, obj);\n"
    "  return obj;\n"
    "};\n"
    "Object.assign(G, { Document, DocumentFragment, HTMLImageElement: HTMLElement, Image: function(w, h) { const i = G.document.createElement('img'); if (w) i.setAttribute('width', w); if (h) i.setAttribute('height', h); return i; } });\n",

    /* Chunk 11: Observers, Timers, rAF & Microtasks */
    "const mutObservers = new Set();\n"
    "let mutPending = false;\n"
    "const notifyMutations = () => {\n"
    "  if (mutObservers.size === 0 || mutPending) return;\n"
    "  mutPending = true;\n"
    "  Promise.resolve().then(() => {\n"
    "    mutPending = false;\n"
    "    for (const mo of mutObservers) {\n"
    "      if (mo._target) { try { mo._cb([{ type: 'childList', target: mo._target, addedNodes: [], removedNodes: [] }], mo); } catch (_) {} }\n"
    "    }\n"
    "  });\n"
    "};\n"
    "class MutationObserver {\n"
    "  constructor(cb) { this._cb = cb; this._target = null; }\n"
    "  observe(target) { this._target = target; mutObservers.add(this); }\n"
    "  disconnect() { this._target = null; mutObservers.delete(this); }\n"
    "  takeRecords() { return []; }\n"
    "}\n"
    "class ResizeObserver {\n"
    "  constructor(cb) { this._cb = cb; }\n"
    "  observe(el) { if (el) Promise.resolve().then(() => { try { const r = el.getBoundingClientRect(); this._cb([{ target: el, contentRect: r }], this); } catch (_) {} }); }\n"
    "  unobserve() {} disconnect() {}\n"
    "}\n"
    "class IntersectionObserver {\n"
    "  constructor(cb, opts = {}) { this._cb = cb; this.root = opts.root || null; this.rootMargin = opts.rootMargin || '0px'; this.thresholds = [0]; }\n"
    "  observe(el) {\n"
    "    if (el) Promise.resolve().then(() => {\n"
    "      try { const r = el.getBoundingClientRect(); this._cb([{ target: el, isIntersecting: true, intersectionRatio: 1, boundingClientRect: r, intersectionRect: r, rootBounds: null, time: B.envInfo().nowMs }], this); } catch (_) {}\n"
    "    });\n"
    "  }\n"
    "  unobserve() {} disconnect() {} takeRecords() { return []; }\n"
    "}\n"
    "Object.assign(G, { MutationObserver, ResizeObserver, IntersectionObserver });\n"
    "let nextTimerId = 1;\n"
    "const timerMap = new Map();\n"
    "const rafMap = new Map();\n"
    "G.queueMicrotask = (cb) => { Promise.resolve().then(() => { if (typeof cb === 'function') cb(); }); };\n"
    "G.setTimeout = (fn, delay = 0, ...args) => {\n"
    "  const id = nextTimerId++;\n"
    "  const due = B.envInfo().nowMs + Math.max(0, Number(delay) || 0);\n"
    "  timerMap.set(id, { id, fn, args, due, interval: -1 });\n"
    "  return id;\n"
    "};\n"
    "G.setInterval = (fn, delay = 0, ...args) => {\n"
    "  const id = nextTimerId++; const ms = Math.max(4, Number(delay) || 4);\n"
    "  timerMap.set(id, { id, fn, args, due: B.envInfo().nowMs + ms, interval: ms });\n"
    "  return id;\n"
    "};\n"
    "G.clearTimeout = (id) => { timerMap.delete(Number(id)); };\n"
    "G.clearInterval = G.clearTimeout;\n"
    "G.requestAnimationFrame = (cb) => { const id = nextTimerId++; rafMap.set(id, cb); return id; };\n"
    "G.cancelAnimationFrame = (id) => { rafMap.delete(Number(id)); };\n"
    "G.requestIdleCallback = (cb) => G.setTimeout(() => cb({ didTimeout: false, timeRemaining: () => 12 }), 1);\n"
    "G.cancelIdleCallback = G.clearTimeout;\n",

    /* Chunk 12: URL, URLSearchParams, TextEncoder, TextDecoder, Blob, FormData */
    "class URLSearchParams {\n"
    "  constructor(init = '') {\n"
    "    this._p = [];\n"
    "    if (typeof init === 'string') {\n"
    "      const s = init.startsWith('?') ? init.slice(1) : init;\n"
    "      if (s) for (const part of s.split('&')) {\n"
    "        const eq = part.indexOf('=');\n"
    "        const k = eq >= 0 ? part.slice(0, eq) : part; const v = eq >= 0 ? part.slice(eq + 1) : '';\n"
    "        this._p.push([decodeURIComponent(k.replace(/\\+/g, ' ')), decodeURIComponent(v.replace(/\\+/g, ' '))]);\n"
    "      }\n"
    "    } else if (init && typeof init === 'object') {\n"
    "      const it = Symbol.iterator in init ? init : Object.entries(init);\n"
    "      for (const [k, v] of it) this._p.push([String(k), String(v)]);\n"
    "    }\n"
    "  }\n"
    "  append(k, v) { this._p.push([String(k), String(v)]); }\n"
    "  set(k, v) { const s = String(k); let done = false; this._p = this._p.filter(e => { if (e[0] === s) { if (!done) { e[1] = String(v); done = true; return true; } return false; } return true; }); if (!done) this._p.push([s, String(v)]); }\n"
    "  get(k) { const f = this._p.find(e => e[0] === String(k)); return f ? f[1] : null; }\n"
    "  getAll(k) { return this._p.filter(e => e[0] === String(k)).map(e => e[1]); }\n"
    "  has(k) { return this._p.some(e => e[0] === String(k)); }\n"
    "  delete(k) { this._p = this._p.filter(e => e[0] !== String(k)); }\n"
    "  forEach(cb, thisArg) { this._p.forEach(([k, v]) => cb.call(thisArg, v, k, this)); }\n"
    "  entries() { return this._p[Symbol.iterator](); }\n"
    "  keys() { return this._p.map(e => e[0])[Symbol.iterator](); }\n"
    "  values() { return this._p.map(e => e[1])[Symbol.iterator](); }\n"
    "  [Symbol.iterator]() { return this.entries(); }\n"
    "  toString() { return this._p.map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v)).join('&'); }\n"
    "}\n"
    "const parseUrlParts = (raw, base) => {\n"
    "  let s = String(raw || '').trim();\n"
    "  if (!/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(s) && base) {\n"
    "    const b = parseUrlParts(base, null);\n"
    "    if (s.startsWith('//')) s = b.protocol + s;\n"
    "    else if (s.startsWith('/')) s = b.origin + s;\n"
    "    else if (s.startsWith('?')) s = b.origin + b.pathname + s;\n"
    "    else if (s.startsWith('#')) s = b.origin + b.pathname + b.search + s;\n"
    "    else { const dir = b.pathname.slice(0, b.pathname.lastIndexOf('/') + 1) || '/'; s = b.origin + dir + s; }\n"
    "  }\n"
    "  const m = s.match(/^([a-zA-Z][a-zA-Z0-9+.-]*:)(?:\\/\\/([^/?#]*))?([^?#]*)(\\?[^#]*)?(#.*)?$/);\n"
    "  if (!m) return { protocol: 'https:', host: 'localhost', hostname: 'localhost', port: '', pathname: '/', search: '', hash: '', origin: 'https://localhost', href: s };\n"
    "  const protocol = m[1].toLowerCase(); const host = m[2] || '';\n"
    "  const ci = host.indexOf(':'); const hostname = ci >= 0 ? host.slice(0, ci) : host; const port = ci >= 0 ? host.slice(ci + 1) : '';\n"
    "  const pathname = m[3] || (host ? '/' : ''); const search = m[4] || ''; const hash = m[5] || '';\n"
    "  const origin = host ? (protocol + '//' + host) : 'null';\n"
    "  const href = (host ? (protocol + '//' + host) : protocol) + pathname + search + hash;\n"
    "  return { protocol, host, hostname, port, pathname, search, hash, origin, href };\n"
    "};\n"
    "class URL {\n"
    "  constructor(url, base) {\n"
    "    Object.assign(this, parseUrlParts(url, base));\n"
    "    this.searchParams = new URLSearchParams(this.search);\n"
    "  }\n"
    "  toString() { const q = this.searchParams.toString(); return (this.origin !== 'null' ? this.origin : this.protocol) + this.pathname + (q ? '?' + q : this.search) + this.hash; }\n"
    "  toJSON() { return this.toString(); }\n"
    "  static createObjectURL() { return 'blob:waterscape/' + Math.random().toString(36).slice(2); }\n"
    "  static revokeObjectURL() {}\n"
    "}\n"
    "G.URL = URL; G.URLSearchParams = URLSearchParams;\n",

    /* Chunk 13: TextEncoder, TextDecoder, Blob, FormData, Crypto, AbortController */
    "class TextEncoder {\n"
    "  get encoding() { return 'utf-8'; }\n"
    "  encode(str = '') {\n"
    "    const s = unescape(encodeURIComponent(String(str)));\n"
    "    const u = new Uint8Array(s.length);\n"
    "    for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i);\n"
    "    return u;\n"
    "  }\n"
    "}\n"
    "class TextDecoder {\n"
    "  constructor(label = 'utf-8') { this.encoding = String(label).toLowerCase(); }\n"
    "  decode(input) {\n"
    "    if (!input) return '';\n"
    "    const u = input instanceof ArrayBuffer ? new Uint8Array(input) : new Uint8Array(input.buffer || input, input.byteOffset || 0, input.byteLength ?? input.length ?? 0);\n"
    "    let bin = ''; for (let i = 0; i < u.length; i++) bin += String.fromCharCode(u[i]);\n"
    "    try { return decodeURIComponent(escape(bin)); } catch (_) { return bin; }\n"
    "  }\n"
    "}\n"
    "class Blob {\n"
    "  constructor(parts = [], opts = {}) {\n"
    "    this.type = String(opts.type || ''); const enc = new TextEncoder(); const chunks = []; let total = 0;\n"
    "    for (const p of parts) {\n"
    "      const b = (p instanceof Uint8Array) ? p : (p instanceof ArrayBuffer) ? new Uint8Array(p) : (p instanceof Blob) ? p._bytes : enc.encode(String(p));\n"
    "      chunks.push(b); total += b.length;\n"
    "    }\n"
    "    this._bytes = new Uint8Array(total); let off = 0;\n"
    "    for (const c of chunks) { this._bytes.set(c, off); off += c.length; }\n"
    "    this.size = total;\n"
    "  }\n"
    "  arrayBuffer() { return Promise.resolve(this._bytes.buffer.slice(0)); }\n"
    "  text() { return Promise.resolve(new TextDecoder().decode(this._bytes)); }\n"
    "  slice(s = 0, e = this.size, type = '') { return new Blob([this._bytes.slice(s, e)], { type }); }\n"
    "}\n"
    "class File extends Blob { constructor(p, name, opts = {}) { super(p, opts); this.name = String(name); this.lastModified = opts.lastModified || Date.now(); } }\n"
    "class FormData extends URLSearchParams {}\n"
    "class AbortSignal extends EventTarget {\n"
    "  constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }\n"
    "  throwIfAborted() { if (this.aborted) throw this.reason || new DOMException('Aborted', 'AbortError'); }\n"
    "}\n"
    "class AbortController {\n"
    "  constructor() { this.signal = new AbortSignal(); }\n"
    "  abort(reason) {\n"
    "    if (this.signal.aborted) return;\n"
    "    this.signal.aborted = true; this.signal.reason = reason || new Error('AbortError');\n"
    "    this.signal.dispatchEvent(new Event('abort'));\n"
    "  }\n"
    "}\n"
    "G.crypto = {\n"
    "  getRandomValues: (arr) => B.randomBytes(arr),\n"
    "  randomUUID: () => {\n"
    "    const b = B.randomBytes(new Uint8Array(16)); b[6] = (b[6] & 0x0f) | 0x40; b[8] = (b[8] & 0x3f) | 0x80;\n"
    "    const h = Array.from(b, x => x.toString(16).padStart(2, '0')).join('');\n"
    "    return h.slice(0, 8) + '-' + h.slice(8, 12) + '-' + h.slice(12, 16) + '-' + h.slice(16, 20) + '-' + h.slice(20);\n"
    "  },\n"
    "  subtle: {}\n"
    "};\n"
    "G.structuredClone = (v) => { if (v === undefined || typeof v !== 'object' || v === null) return v; return JSON.parse(JSON.stringify(v)); };\n"
    "Object.assign(G, { TextEncoder, TextDecoder, Blob, File, FormData, AbortSignal, AbortController });\n",

    /* Chunk 14: Headers, Request, Response, fetch() & XMLHttpRequest */
    "class Headers {\n"
    "  constructor(init) {\n"
    "    this._m = new Map();\n"
    "    if (init instanceof Headers) init.forEach((v, k) => this.set(k, v));\n"
    "    else if (Array.isArray(init)) for (const [k, v] of init) this.append(k, v);\n"
    "    else if (init && typeof init === 'object') for (const [k, v] of Object.entries(init)) this.set(k, v);\n"
    "  }\n"
    "  set(k, v) { this._m.set(String(k).toLowerCase(), String(v)); }\n"
    "  append(k, v) { const kl = String(k).toLowerCase(); const ex = this._m.get(kl); this._m.set(kl, ex ? ex + ', ' + v : String(v)); }\n"
    "  get(k) { const v = this._m.get(String(k).toLowerCase()); return v !== undefined ? v : null; }\n"
    "  has(k) { return this._m.has(String(k).toLowerCase()); }\n"
    "  delete(k) { this._m.delete(String(k).toLowerCase()); }\n"
    "  forEach(cb, thisArg) { this._m.forEach((v, k) => cb.call(thisArg, v, k, this)); }\n"
    "  entries() { return this._m.entries(); } keys() { return this._m.keys(); } values() { return this._m.values(); }\n"
    "  [Symbol.iterator]() { return this._m.entries(); }\n"
    "}\n"
    "class Request {\n"
    "  constructor(input, init = {}) {\n"
    "    const isReq = input instanceof Request;\n"
    "    this.url = new URL(isReq ? input.url : String(input), G.location.href).toString();\n"
    "    this.method = String(init.method || (isReq ? input.method : 'GET')).toUpperCase();\n"
    "    this.headers = new Headers(init.headers || (isReq ? input.headers : undefined));\n"
    "    this.body = init.body !== undefined ? init.body : (isReq ? input.body : null);\n"
    "    this.signal = init.signal || null;\n"
    "  }\n"
    "  clone() { return new Request(this); }\n"
    "}\n",
    "class Response {\n"
    "  constructor(body = null, init = {}) {\n"
    "    this.status = init.status !== undefined ? Number(init.status) : 200;\n"
    "    this.ok = this.status >= 200 && this.status < 300;\n"
    "    this.statusText = init.statusText || (this.ok ? 'OK' : 'Error');\n"
    "    this.url = init.url || '';\n"
    "    this.headers = new Headers(init.headers);\n"
    "    this._ab = body instanceof ArrayBuffer ? body : (typeof body === 'string' ? new TextEncoder().encode(body).buffer : new ArrayBuffer(0));\n"
    "    this.bodyUsed = false;\n"
    "  }\n"
    "  arrayBuffer() { this.bodyUsed = true; return Promise.resolve(this._ab.slice(0)); }\n"
    "  text() { this.bodyUsed = true; return Promise.resolve(new TextDecoder().decode(this._ab)); }\n"
    "  json() { return this.text().then(t => JSON.parse(t)); }\n"
    "  blob() { return Promise.resolve(new Blob([this._ab], { type: this.headers.get('content-type') || '' })); }\n"
    "  clone() { return new Response(this._ab.slice(0), { status: this.status, statusText: this.statusText, headers: this.headers, url: this.url }); }\n"
    "  static json(data, init = {}) { const h = new Headers(init.headers); h.set('content-type', 'application/json'); return new Response(JSON.stringify(data), { ...init, headers: h }); }\n"
    "}\n"
    "G.fetch = (input, init = {}) => new Promise((resolve, reject) => {\n"
    "  try {\n"
    "    const req = new Request(input, init);\n"
    "    if (req.signal && req.signal.aborted) return reject(req.signal.reason || new Error('AbortError'));\n"
    "    const bodyStr = req.body === null || req.body === undefined ? null : (req.body instanceof URLSearchParams ? req.body.toString() : String(req.body));\n"
    "    const r = B.httpFetch(req.url, req.method, bodyStr, req.headers.get('content-type'));\n"
    "    if (!r.netOk && r.status === 0) return reject(new TypeError('NetworkError when attempting to fetch resource.'));\n"
    "    const h = new Headers(); if (r.contentType) h.set('content-type', r.contentType);\n"
    "    resolve(new Response(r.buffer, { status: r.status || 200, headers: h, url: req.url }));\n"
    "  } catch (e) { reject(e); }\n"
    "});\n"
    "class XMLHttpRequest extends EventTarget {\n"
    "  constructor() { super(); this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = ''; this.responseType = ''; this._hdrs = {}; }\n"
    "  open(m, u) { this._m = String(m || 'GET').toUpperCase(); this._u = new URL(String(u), G.location.href).toString(); this.readyState = 1; }\n"
    "  setRequestHeader(k, v) { this._hdrs[String(k).toLowerCase()] = String(v); }\n"
    "  getResponseHeader(k) { return String(k).toLowerCase() === 'content-type' ? (this._ct || null) : null; }\n"
    "  getAllResponseHeaders() { return this._ct ? ('content-type: ' + this._ct + '\\r\\n') : ''; }\n"
    "  abort() { this.readyState = 0; }\n"
    "  send(body = null) {\n"
    "    const r = B.httpFetch(this._u, this._m, body === null ? null : String(body), this._hdrs['content-type'] || null);\n"
    "    this.status = r.status || (r.netOk ? 200 : 0); this._ct = r.contentType || '';\n"
    "    this.responseText = new TextDecoder().decode(r.buffer);\n"
    "    this.response = this.responseType === 'json' ? (() => { try { return JSON.parse(this.responseText); } catch (_) { return null; } })() : (this.responseType === 'arraybuffer' ? r.buffer : this.responseText);\n"
    "    this.readyState = 4;\n"
    "    this.dispatchEvent(new Event('readystatechange'));\n"
    "    this.dispatchEvent(new Event(r.netOk ? 'load' : 'error'));\n"
    "  }\n"
    "}\n"
    "Object.assign(G, { Headers, Request, Response, XMLHttpRequest });\n",

    /* Chunk 15: Storage, Location, History, Navigator, Screen, CSS/Media, DOMParser, Intl & Hooks */
    "const makeStorage = (persist) => {\n"
    "  const mem = new Map();\n"
    "  const api = {\n"
    "    get length() { return mem.size; },\n"
    "    key(i) { return Array.from(mem.keys())[i] ?? null; },\n"
    "    getItem(k) { const s = String(k); if (mem.has(s)) return mem.get(s); if (persist) { const v = B.storageOp(0, s); if (v !== null) { mem.set(s, v); return v; } } return null; },\n"
    "    setItem(k, v) { const s = String(k), sv = String(v); mem.set(s, sv); if (persist) B.storageOp(1, s, sv); },\n"
    "    removeItem(k) { const s = String(k); mem.delete(s); if (persist) B.storageOp(1, s, null); },\n"
    "    clear() { if (persist) for (const k of mem.keys()) B.storageOp(1, k, null); mem.clear(); }\n"
    "  };\n"
    "  return new Proxy(api, {\n"
    "    get(t, p) { if (p in t || typeof p === 'symbol') return t[p]; const v = t.getItem(String(p)); return v === null ? undefined : v; },\n"
    "    set(t, p, v) { t.setItem(String(p), v); return true; },\n"
    "    deleteProperty(t, p) { t.removeItem(String(p)); return true; }\n"
    "  });\n"
    "};\n"
    "G.localStorage = makeStorage(true); G.sessionStorage = makeStorage(false);\n"
    "let curLocUrl = B.envInfo().url || 'about:blank';\n"
    "const getLocParts = () => parseUrlParts(curLocUrl, null);\n"
    "G.location = {\n"
    "  get href() { return getLocParts().href; },\n"
    "  set href(v) { const r = new URL(String(v), curLocUrl).toString(); curLocUrl = r; B.navigate(r); },\n"
    "  get origin() { return getLocParts().origin; },\n"
    "  get protocol() { return getLocParts().protocol; },\n"
    "  get host() { return getLocParts().host; },\n"
    "  get hostname() { return getLocParts().hostname; },\n"
    "  get port() { return getLocParts().port; },\n"
    "  get pathname() { return getLocParts().pathname; },\n"
    "  get search() { return getLocParts().search; },\n"
    "  get hash() { return getLocParts().hash; },\n"
    "  set hash(v) { const h = String(v).startsWith('#') ? String(v) : '#' + v; const p = getLocParts(); curLocUrl = p.origin + p.pathname + p.search + h; G.dispatchEvent(new Event('hashchange')); },\n"
    "  assign(u) { this.href = u; }, replace(u) { this.href = u; }, reload() { B.navigate(curLocUrl); },\n"
    "  toString() { return this.href; }\n"
    "};\n"
    "let histState = null, histLen = 1;\n"
    "G.history = {\n"
    "  get state() { return histState; }, get length() { return histLen; }, scrollRestoration: 'auto',\n"
    "  pushState(st, _t, url) { histState = st; histLen++; if (url) curLocUrl = new URL(String(url), curLocUrl).toString(); },\n"
    "  replaceState(st, _t, url) { histState = st; if (url) curLocUrl = new URL(String(url), curLocUrl).toString(); },\n"
    "  back() {}, forward() {}, go() {}\n"
    "};\n"
    "G.navigator = {\n"
    "  userAgent: 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Waterscape/2.0 Arcradient/2.0 Chrome/134.0.0.0 Safari/537.36',\n"
    "  appName: 'Netscape', platform: 'Linux x86_64', vendor: 'Waterscape',\n"
    "  language: 'ko-KR', languages: ['ko-KR', 'ko', 'en-US', 'en'],\n"
    "  onLine: true, cookieEnabled: true, hardwareConcurrency: 8, maxTouchPoints: 0, webdriver: false,\n"
    "  clipboard: { writeText: (t) => Promise.resolve(B.clipboardOp(true, String(t ?? ''))), readText: () => Promise.resolve(B.clipboardOp(false)) },\n"
    "  mediaCapabilities: { decodingInfo: () => Promise.resolve({ supported: true, smooth: true, powerEfficient: true }) },\n"
    "  serviceWorker: { register: () => Promise.resolve({ scope: '/' }), ready: new Promise(() => {}) },\n"
    "  sendBeacon: () => true\n"
    "};\n",

    /* Chunk 16: Viewport, getComputedStyle, matchMedia, DOMParser, Intl, document init & C hooks */
    "Object.defineProperties(G, {\n"
    "  innerWidth:  { get: () => B.envInfo().width, configurable: true },\n"
    "  innerHeight: { get: () => B.envInfo().height, configurable: true },\n"
    "  outerWidth:  { get: () => B.envInfo().width, configurable: true },\n"
    "  outerHeight: { get: () => B.envInfo().height, configurable: true },\n"
    "  devicePixelRatio: { value: 1, configurable: true }\n"
    "});\n"
    "G.scrollX = 0; G.scrollY = 0; G.pageXOffset = 0; G.pageYOffset = 0;\n"
    "G.scrollTo = (x, y) => { if (typeof x === 'object' && x) { G.scrollX = x.left || 0; G.scrollY = x.top || 0; } else { G.scrollX = x || 0; G.scrollY = y || 0; } };\n"
    "G.scroll = G.scrollTo; G.scrollBy = (dx, dy) => G.scrollTo(G.scrollX + (dx || 0), G.scrollY + (dy || 0));\n"
    "G.screen = { get width() { return G.innerWidth; }, get height() { return G.innerHeight; }, get availWidth() { return G.innerWidth; }, get availHeight() { return G.innerHeight; }, colorDepth: 24, pixelDepth: 24 };\n"
    "G.visualViewport = new Proxy(new EventTarget(), { get(t, p) { if (p === 'width') return G.innerWidth; if (p === 'height') return G.innerHeight; if (p === 'scale') return 1; return t[p]; } });\n"
    "G.getComputedStyle = (el) => {\n"
    "  const uid = el ? el._uid : 0; const c = B.computedInfo(uid); const inl = uid > 0 ? makeStyleProxy(uid) : null;\n"
    "  const base = { display: c.display, position: c.position, color: c.color, backgroundColor: c.backgroundColor, fontSize: c.fontSize + 'px', width: c.width + 'px', height: c.height + 'px', opacity: String(c.opacity),\n"
    "    getPropertyValue(p) { const k = kebabToCamel(String(p)); if (inl) { const iv = inl.getPropertyValue(p); if (iv) return iv; } return this[k] || ''; } };\n"
    "  return new Proxy(base, { get(t, p) { if (p in t || typeof p === 'symbol') return t[p]; return t.getPropertyValue(camelToKebab(String(p))); } });\n"
    "};\n"
    "G.matchMedia = (q) => {\n"
    "  const s = String(q || ''); const env = B.envInfo(); let matches = true;\n"
    "  if (/prefers-color-scheme\\s*:\\s*dark/i.test(s)) matches = !!env.prefersDark;\n"
    "  else if (/prefers-color-scheme\\s*:\\s*light/i.test(s)) matches = !env.prefersDark;\n"
    "  else { const mx = s.match(/max-width\\s*:\\s*(\\d+)/i); if (mx) matches = env.width <= Number(mx[1]); const mn = s.match(/min-width\\s*:\\s*(\\d+)/i); if (mn) matches = matches && env.width >= Number(mn[1]); }\n"
    "  const mql = Object.assign(new EventTarget(), { media: s, matches, onchange: null, addListener(fn) { this.addEventListener('change', fn); }, removeListener(fn) { this.removeEventListener('change', fn); } });\n"
    "  return mql;\n"
    "};\n",
    "G.CSS = { supports: () => true, escape: (s) => String(s).replace(/[^a-zA-Z0-9_-]/g, '\\\\$&') };\n"
    "G.DOMParser = class { parseFromString(str) { const d = wrapUid(B.createNode(11, '')); B.setInnerHtml(d._uid, String(str ?? '')); d.getElementById = (id) => d.querySelector('#' + id); d.body = d.querySelector('body') || d; d.head = d.querySelector('head') || d; d.documentElement = d.querySelector('html') || d; return d; } };\n"
    "G.Range = class { setStart() {} setEnd() {} selectNodeContents() {} collapse() {} getBoundingClientRect() { return { x: 0, y: 0, width: 0, height: 0, top: 0, left: 0, right: 0, bottom: 0 }; } createContextualFragment(h) { const f = G.document.createDocumentFragment(); B.setInnerHtml(f._uid, String(h ?? '')); return f; } };\n"
    "G.document = wrapUid(B.docSpecialUids().doc);\n"
    "G.document.createRange = () => new G.Range();\n"
    "G.getSelection = () => ({ toString: () => '', rangeCount: 0, removeAllRanges() {}, addRange() {} });\n"
    "if (!G.Intl) G.Intl = {};\n"
    "if (!G.Intl.NumberFormat) G.Intl.NumberFormat = class { format(n) { return Number(n).toLocaleString(); } resolvedOptions() { return { locale: 'ko-KR' }; } };\n"
    "if (!G.Intl.DateTimeFormat) G.Intl.DateTimeFormat = class { format(d = new Date()) { return new Date(d).toISOString().slice(0, 10); } resolvedOptions() { return { locale: 'ko-KR', timeZone: 'Asia/Seoul' }; } };\n"
    "if (!G.Intl.RelativeTimeFormat) G.Intl.RelativeTimeFormat = class { format(v, u) { return v + ' ' + u; } };\n"
    "if (!G.Intl.PluralRules) G.Intl.PluralRules = class { select(n) { return n === 1 ? 'one' : 'other'; } };\n"
    "if (!G.Intl.Collator) G.Intl.Collator = class { compare(a, b) { return String(a).localeCompare(String(b)); } };\n"
    "return {\n"
    "  wrapUid,\n"
    "  dispatchNative(uid, type, cx, cy, keyCode) {\n"
    "    if (type === 'load') docReadyState = 'complete';\n"
    "    let target = uid === 0 ? G.document : wrapUid(uid);\n"
    "    if (!target) target = G.document || G;\n"
    "    const bubbles = !(type === 'load' || type === 'focus' || type === 'blur' || type === 'mouseenter' || type === 'mouseleave');\n"
    "    let ev;\n"
    "    if (type === 'click' || type === 'mousedown' || type === 'mouseup' || type === 'mousemove' || type === 'contextmenu') ev = new MouseEvent(type, { bubbles, cancelable: true, clientX: cx, clientY: cy });\n"
    "    else if (type === 'keydown' || type === 'keyup' || type === 'keypress') ev = new KeyboardEvent(type, { bubbles, cancelable: true, keyCode });\n"
    "    else if (type === 'input') ev = new InputEvent(type, { bubbles: true, cancelable: false });\n"
    "    else ev = new Event(type, { bubbles, cancelable: true });\n"
    "    ev.isTrusted = true;\n"
    "    if (type === 'load') { const r = runEventDispatch(G, ev); return { handled: r.handled, defaultPrevented: !r.notCanceled }; }\n"
    "    const res = runEventDispatch(target, ev);\n"
    "    return { handled: res.handled, defaultPrevented: !res.notCanceled };\n"
    "  },\n"
    "  tickTimers(nowMs) {\n"
    "    let ran = false;\n"
    "    if (rafMap.size > 0) {\n"
    "      const batch = Array.from(rafMap.entries()); rafMap.clear();\n"
    "      for (const [, cb] of batch) { ran = true; try { if (typeof cb === 'function') cb(nowMs); } catch (e) { B.log('[rAF Error] ' + fmtVal(e)); } }\n"
    "    }\n"
    "    const due = [];\n"
    "    for (const t of timerMap.values()) { if (t.due <= nowMs) due.push(t); }\n"
    "    due.sort((a, b) => a.due - b.due);\n"
    "    for (const t of due) {\n"
    "      if (!timerMap.has(t.id)) continue;\n"
    "      if (t.interval > 0) t.due = nowMs + t.interval; else timerMap.delete(t.id);\n"
    "      ran = true;\n"
    "      try { if (typeof t.fn === 'function') t.fn(...t.args); else (0, eval)(String(t.fn)); } catch (e) { B.log('[Timer Error] ' + fmtVal(e)); }\n"
    "    }\n"
    "    return ran;\n"
    "  },\n"
    "  hasTimers() { return timerMap.size > 0 || rafMap.size > 0; }\n"
    "};\n"
    "})"
};

/* -------------------------------------------------------------------------
 * 7. QuickJS-NG Lifecycle, Safety Interrupts & ArcrJsVTable Implementation
 * ------------------------------------------------------------------------- */
static int qjs_interrupt_handler(JSContext *ctx, void *opaque) {
    (void)ctx;
    ArcrJsVm *vm = (ArcrJsVm *)opaque;
    if (!vm || !vm->interrupt_armed) return 0;
    return (arcr_platform_monotonic_usec() >= vm->deadline_usec) ? 1 : 0;
}

static void qjs_log_exception(ArcrJsVm *vm, JSContext *ctx) {
    if (!vm || !ctx) return;
    JSValue exc = JS_GetException(ctx);
    if (JS_IsNull(exc) || JS_IsUndefined(exc)) {
        JS_FreeValue(ctx, exc);
        return;
    }
    const char *msg = JS_ToCString(ctx, exc);
    JSValue stack_v = JS_IsObject(exc) ? JS_GetPropertyStr(ctx, exc, "stack") : JS_UNDEFINED;
    const char *stack_s = JS_IsString(stack_v) ? JS_ToCString(ctx, stack_v) : NULL;

    if (vm->host.console_log) {
        char buf[1024];
        if (stack_s && stack_s[0] != '\0') {
            snprintf(buf, sizeof(buf), "[Uncaught JS Exception] %s\n%s",
                     msg ? msg : "Error", stack_s);
        } else {
            snprintf(buf, sizeof(buf), "[Uncaught JS Exception] %s",
                     msg ? msg : "Error");
        }
        vm->host.console_log(vm->host.userdata, buf);
    }

    if (stack_s) JS_FreeCString(ctx, stack_s);
    JS_FreeValue(ctx, stack_v);
    if (msg) JS_FreeCString(ctx, msg);
    JS_FreeValue(ctx, exc);
}

static void qjs_promise_rejection_tracker(JSContext *ctx, JSValueConst promise,
                                          JSValueConst reason, bool is_handled,
                                          void *opaque) {
    (void)promise;
    if (is_handled) return;
    ArcrJsVm *vm = (ArcrJsVm *)opaque;
    if (!vm || !vm->host.console_log) return;
    const char *msg = JS_ToCString(ctx, reason);
    if (msg) {
        char buf[512];
        snprintf(buf, sizeof(buf), "[Unhandled Promise Rejection] %s", msg);
        vm->host.console_log(vm->host.userdata, buf);
        JS_FreeCString(ctx, msg);
    }
}

static void qjs_drain_microtasks(ArcrJsVm *vm) {
    if (!vm || !vm->rt) return;
    JSContext *pctx = NULL;
    int guard = 0;
    vm->deadline_usec   = arcr_platform_monotonic_usec() + ARCR_JS_TURN_DEADLINE_USEC;
    vm->interrupt_armed = true;
    while (guard++ < 4096 && JS_IsJobPending(vm->rt)) {
        int r = JS_ExecutePendingJob(vm->rt, &pctx);
        if (r < 0) {
            if (pctx) qjs_log_exception(vm, pctx);
            break;
        }
    }
    vm->interrupt_armed = false;
}

static const JSClassDef kDomNodeClassDef = {
    .class_name = "DOMNode",
    .finalizer  = NULL,
    .gc_mark    = NULL,
    .call       = NULL,
    .exotic     = NULL
};

static ArcrJsVm *qjs_create_vm(size_t heap_limit_bytes, const ArcrJsHostCallbacks *host_cb) {
    ArcrJsVm *vm = (ArcrJsVm *)calloc(1u, sizeof(ArcrJsVm));
    if (!vm) return NULL;

    if (host_cb) vm->host = *host_cb;
    vm->heap_limit        = (heap_limit_bytes >= (2u * 1024u * 1024u))
                                ? heap_limit_bytes
                                : (16u * 1024u * 1024u);
    vm->boot_usec         = arcr_platform_monotonic_usec();
    vm->fn_dispatch_event = JS_UNDEFINED;
    vm->fn_tick_timers    = JS_UNDEFINED;
    vm->fn_has_timers     = JS_UNDEFINED;
    vm->fn_wrap_uid       = JS_UNDEFINED;

    vm->rt = JS_NewRuntime();
    if (!vm->rt) {
        free(vm);
        return NULL;
    }

    JS_SetMemoryLimit(vm->rt, vm->heap_limit);
    JS_SetMaxStackSize(vm->rt, 512u * 1024u); /* Safe inside 1 MB Win32 stacks! */
    JS_SetRuntimeOpaque(vm->rt, vm);
    JS_SetInterruptHandler(vm->rt, qjs_interrupt_handler, vm);
    JS_SetHostPromiseRejectionTracker(vm->rt, qjs_promise_rejection_tracker, vm);

    JS_NewClassID(vm->rt, &vm->dom_node_class_id);
    if (JS_NewClass(vm->rt, vm->dom_node_class_id, &kDomNodeClassDef) < 0) {
        JS_FreeRuntime(vm->rt);
        free(vm);
        return NULL;
    }

    vm->ctx = JS_NewContext(vm->rt);
    if (!vm->ctx) {
        JS_FreeRuntime(vm->rt);
        free(vm);
        return NULL;
    }
    JS_SetContextOpaque(vm->ctx, vm);
    (void)JS_AddIntrinsicBigInt(vm->ctx);
    (void)JS_AddIntrinsicDOMException(vm->ctx);

    js_uid_sync_document(vm);

    /* Assemble and run modular ES2026 Web API bootstrap */
    JsByteBuf boot = {0};
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kWebBootstrapChunks); ++i) {
        (void)jbuf_cstr(&boot, kWebBootstrapChunks[i]);
    }
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kWebBootstrapChunks2); ++i) {
        (void)jbuf_cstr(&boot, kWebBootstrapChunks2[i]);
    }

    JSValue factory = JS_Eval(vm->ctx, boot.data ? boot.data : "", boot.len,
                              "<waterscape-web-apis>", JS_EVAL_TYPE_GLOBAL);
    free(boot.data);

    if (JS_IsException(factory)) {
        qjs_log_exception(vm, vm->ctx);
        JS_FreeContext(vm->ctx);
        JS_FreeRuntime(vm->rt);
        free(vm->uid_table);
        free(vm);
        return NULL;
    }

    JSValue bridge = JS_NewObject(vm->ctx);
    JS_SetPropertyFunctionList(vm->ctx, bridge, kBridgeFuncs, (int)ARCR_ARRAY_LEN(kBridgeFuncs));

    JSValue hooks = JS_Call(vm->ctx, factory, JS_UNDEFINED, 1, &bridge);
    JS_FreeValue(vm->ctx, bridge);
    JS_FreeValue(vm->ctx, factory);

    if (JS_IsException(hooks)) {
        qjs_log_exception(vm, vm->ctx);
        JS_FreeContext(vm->ctx);
        JS_FreeRuntime(vm->rt);
        free(vm->uid_table);
        free(vm);
        return NULL;
    }

    vm->fn_dispatch_event = JS_GetPropertyStr(vm->ctx, hooks, "dispatchNative");
    vm->fn_tick_timers    = JS_GetPropertyStr(vm->ctx, hooks, "tickTimers");
    vm->fn_has_timers     = JS_GetPropertyStr(vm->ctx, hooks, "hasTimers");
    vm->fn_wrap_uid       = JS_GetPropertyStr(vm->ctx, hooks, "wrapUid");
    JS_FreeValue(vm->ctx, hooks);
    qjs_drain_microtasks(vm);
    return vm;
}

static bool surf_in_doc_tree_rec(const ArcrDomNode *n, const ArcrDomSurface *s) {
    if (!n) return false;
    if (n->surface == s) return true;
    for (const ArcrDomNode *c = n->first_child; c != NULL; c = c->next_sibling) {
        if (surf_in_doc_tree_rec(c, s)) return true;
    }
    return false;
}

static void qjs_destroy_vm(ArcrJsVm *vm) {
    if (!vm) return;

    if (vm->ctx) {
        JS_FreeValue(vm->ctx, vm->fn_dispatch_event);
        JS_FreeValue(vm->ctx, vm->fn_tick_timers);
        JS_FreeValue(vm->ctx, vm->fn_has_timers);
        JS_FreeValue(vm->ctx, vm->fn_wrap_uid);
        JS_FreeContext(vm->ctx);
        vm->ctx = NULL;
    }
    if (vm->rt) {
        JS_RunGC(vm->rt);
        JS_FreeRuntime(vm->rt);
        vm->rt = NULL;
    }

    while (vm->canvas_head) {
        Canvas2DCtx *nx = vm->canvas_head->next;
        free(vm->canvas_head);
        vm->canvas_head = nx;
    }
    free(vm->grads);

    /* Free any JS-allocated canvas surfaces on detached nodes not reachable from doc->root */
    while (vm->surf_track_head) {
        DetachedSurfTrack *nx = vm->surf_track_head->next;
        ArcrDomSurface *s = vm->surf_track_head->surf;
        if (s && s->heap_owned && s->argb_pixels) {
            bool attached = vm->host.doc && vm->host.doc->root &&
                            surf_in_doc_tree_rec(vm->host.doc->root, s);
            if (!attached) {
                free(s->argb_pixels);
                s->argb_pixels = NULL;
                s->heap_owned  = false;
            }
        }
        free(vm->surf_track_head);
        vm->surf_track_head = nx;
    }

    if (vm->canvas_fonts) {
        arcr_font_system_destroy(vm->canvas_fonts);
    }
    free(vm->uid_table);
    free(vm);
}

static bool qjs_eval_script(ArcrJsVm *vm, const char *utf8_code, size_t code_len) {
    if (!vm || !vm->ctx || !utf8_code) return false;
    if (code_len == 0u) return true;

    js_uid_sync_document(vm);
    JS_UpdateStackTop(vm->rt);

    char *buf = (char *)malloc(code_len + 1u);
    if (!buf) return false;
    memcpy(buf, utf8_code, code_len);
    buf[code_len] = '\0';

    vm->deadline_usec   = arcr_platform_monotonic_usec() + ARCR_JS_TURN_DEADLINE_USEC;
    vm->interrupt_armed = true;

    JSValue res = JS_Eval(vm->ctx, buf, code_len, "<script>", JS_EVAL_TYPE_GLOBAL);
    free(buf);
    vm->interrupt_armed = false;

    bool ok = true;
    if (JS_IsException(res)) {
        qjs_log_exception(vm, vm->ctx);
        ok = false;
    } else {
        JS_FreeValue(vm->ctx, res);
    }

    qjs_drain_microtasks(vm);
    return ok;
}

static bool qjs_dispatch_dom_event_ex(ArcrJsVm *vm, uint32_t target_node_uid,
                                      const char *event_type,
                                      int32_t client_x, int32_t client_y, int key_code,
                                      bool *out_default_prevented) {
    if (out_default_prevented) *out_default_prevented = false;
    if (!vm || !vm->ctx || !event_type || !JS_IsFunction(vm->ctx, vm->fn_dispatch_event)) {
        return false;
    }

    js_uid_sync_document(vm);
    JS_UpdateStackTop(vm->rt);

    JSValue args[5];
    args[0] = JS_NewUint32(vm->ctx, target_node_uid);
    args[1] = JS_NewString(vm->ctx, event_type);
    args[2] = JS_NewInt32(vm->ctx, client_x);
    args[3] = JS_NewInt32(vm->ctx, client_y);
    args[4] = JS_NewInt32(vm->ctx, key_code);

    vm->deadline_usec   = arcr_platform_monotonic_usec() + ARCR_JS_TURN_DEADLINE_USEC;
    vm->interrupt_armed = true;
    JSValue ret = JS_Call(vm->ctx, vm->fn_dispatch_event, JS_UNDEFINED, 5, args);
    vm->interrupt_armed = false;

    for (int i = 0; i < 5; ++i) JS_FreeValue(vm->ctx, args[i]);

    bool handled   = false;
    bool prevented = false;
    if (JS_IsException(ret)) {
        qjs_log_exception(vm, vm->ctx);
    } else {
        if (JS_IsObject(ret)) {
            JSValue vh = JS_GetPropertyStr(vm->ctx, ret, "handled");
            JSValue vp = JS_GetPropertyStr(vm->ctx, ret, "defaultPrevented");
            handled   = JS_ToBool(vm->ctx, vh) > 0;
            prevented = JS_ToBool(vm->ctx, vp) > 0;
            JS_FreeValue(vm->ctx, vh);
            JS_FreeValue(vm->ctx, vp);
        }
        JS_FreeValue(vm->ctx, ret);
    }

    qjs_drain_microtasks(vm);
    if (out_default_prevented) *out_default_prevented = prevented;
    return handled || prevented;
}

static bool qjs_dispatch_dom_event(ArcrJsVm *vm, uint32_t target_node_uid,
                                   const char *event_type) {
    return qjs_dispatch_dom_event_ex(vm, target_node_uid, event_type, 0, 0, 0, NULL);
}

static bool qjs_tick_timers_and_raf(ArcrJsVm *vm, uint64_t now_ms, bool *out_dom_mutated) {
    if (out_dom_mutated) *out_dom_mutated = false;
    if (!vm || !vm->ctx || !JS_IsFunction(vm->ctx, vm->fn_tick_timers)) return false;

    bool had_dirty = vm->host.doc ? vm->host.doc->dom_dirty : false;
    if (vm->host.doc) vm->host.doc->dom_dirty = false;

    JS_UpdateStackTop(vm->rt);
    uint64_t now_us = arcr_platform_monotonic_usec();
    double rel_ms = (now_us >= vm->boot_usec) ? ((double)(now_us - vm->boot_usec) * 0.001) : (double)now_ms;
    JSValue arg = JS_NewFloat64(vm->ctx, rel_ms);

    vm->deadline_usec   = now_us + ARCR_JS_TURN_DEADLINE_USEC;
    vm->interrupt_armed = true;
    JSValue ret = JS_Call(vm->ctx, vm->fn_tick_timers, JS_UNDEFINED, 1, &arg);
    vm->interrupt_armed = false;
    JS_FreeValue(vm->ctx, arg);

    bool ran = false;
    if (JS_IsException(ret)) {
        qjs_log_exception(vm, vm->ctx);
    } else {
        ran = JS_ToBool(vm->ctx, ret) > 0;
        JS_FreeValue(vm->ctx, ret);
    }

    if (JS_IsJobPending(vm->rt)) {
        ran = true;
        qjs_drain_microtasks(vm);
    }

    bool mutated = vm->host.doc ? vm->host.doc->dom_dirty : false;
    if (vm->host.doc && had_dirty) vm->host.doc->dom_dirty = true;
    if (out_dom_mutated) *out_dom_mutated = mutated;
    return ran || mutated;
}

static bool qjs_has_pending_timers(const ArcrJsVm *vm) {
    if (!vm || !vm->ctx || !vm->rt) return false;
    if (JS_IsJobPending(vm->rt)) return true;
    if (!JS_IsFunction(vm->ctx, vm->fn_has_timers)) return false;
    JSValue ret = JS_Call(vm->ctx, vm->fn_has_timers, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(ret)) {
        JS_FreeValue(vm->ctx, JS_GetException(vm->ctx));
        return false;
    }
    bool has = JS_ToBool(vm->ctx, ret) > 0;
    JS_FreeValue(vm->ctx, ret);
    return has;
}

static size_t qjs_get_used_heap_bytes(const ArcrJsVm *vm) {
    if (!vm || !vm->rt) return 0u;
    JSMemoryUsage mu;
    memset(&mu, 0, sizeof(mu));
    JS_ComputeMemoryUsage(vm->rt, &mu);
    return (mu.memory_used_size > 0) ? (size_t)mu.memory_used_size : sizeof(ArcrJsVm);
}

static void qjs_release_caches_on_pressure(ArcrJsVm *vm, int pressure_level) {
    if (!vm || !vm->rt) return;
    if (pressure_level >= 1) {
        JS_RunGC(vm->rt);
        if (vm->canvas_fonts) {
            arcr_font_system_purge_cache(vm->canvas_fonts);
        }
    }
}

static const ArcrJsVTable kPortableJsVTable = {
    .struct_size                = sizeof(ArcrJsVTable),
    .engine_backend_name        = "ArcradientJS/QuickJS-NG-ES2026",
    .create_vm                  = qjs_create_vm,
    .destroy_vm                 = qjs_destroy_vm,
    .eval_script                = qjs_eval_script,
    .dispatch_dom_event         = qjs_dispatch_dom_event,
    .dispatch_dom_event_ex      = qjs_dispatch_dom_event_ex,
    .tick_timers_and_raf        = qjs_tick_timers_and_raf,
    .has_pending_timers         = qjs_has_pending_timers,
    .get_used_heap_bytes        = qjs_get_used_heap_bytes,
    .release_caches_on_pressure = qjs_release_caches_on_pressure
};

static ArcrModuleDescriptor g_js_desc = {
    .magic              = ARCR_MODULE_MAGIC,
    .struct_size        = sizeof(ArcrModuleDescriptor),
    .abi_version        = ARCR_ABI_VERSION,
    .engine_build_id    = ARCR_ENGINE_BUILD_ID,
    .arch_id            = 0,
    .min_os_major_minor = ARCR_OS_POSIX_ANY,
    .required_cpu_flags = ARCR_CPU_SCALAR,
    .module_name        = "arcr-js-portable",
    .vtable             = &kPortableJsVTable
};

const ArcrModuleDescriptor *arcr_js_portable_module_query(void);
const ArcrModuleDescriptor *arcr_js_portable_module_query(void) {
    g_js_desc.arch_id = (uint16_t)arcr_host_compile_arch();
#if defined(_WIN32)
    g_js_desc.min_os_major_minor = ARCR_OS_WIN2000;
#endif
    return &g_js_desc;
}

#if !defined(ARCR_JS_STATIC_LINK)
ARCR_API const ArcrModuleDescriptor *arcr_module_query(void) {
    return arcr_js_portable_module_query();
}
#endif
