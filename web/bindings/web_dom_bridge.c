/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * web/bindings/web_dom_bridge.c - Self-Contained Live DOM, HTML5 Fragment/Template,
 *                                 Shadow DOM, Selector, Layout Geometry, CSSOM & Host Bridge
 *
 * Zero QuickJS dependency. Zero link dependency on `arcr_engine`.
 */
#include "web_host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * 1. Ephemeral Private-Mode Storage (Never touches host.storage_*!)
 * ------------------------------------------------------------------------- */
typedef struct WebPrivKv {
    char             *key;
    char             *val;
    struct WebPrivKv *next;
} WebPrivKv;

static WebMutex   g_priv_mtx;
static bool       g_priv_mtx_ready = false;
static size_t     g_priv_vm_refs   = 0u;
static WebPrivKv *g_priv_head      = NULL;

static void web_priv_ensure_init(void) {
    if (!g_priv_mtx_ready) {
        web_mutex_init(&g_priv_mtx);
        g_priv_mtx_ready = true;
    }
}

void web_private_session_retain(ArcrJsVm *vm) {
    if (!vm || !vm->host.private_browsing || vm->counted_private_vm) return;
    web_priv_ensure_init();
    web_mutex_lock(&g_priv_mtx);
    g_priv_vm_refs++;
    vm->counted_private_vm = true;
    web_mutex_unlock(&g_priv_mtx);
}

void web_private_session_release(ArcrJsVm *vm) {
    if (!vm || !vm->counted_private_vm) return;
    web_priv_ensure_init();
    web_mutex_lock(&g_priv_mtx);
    vm->counted_private_vm = false;
    if (g_priv_vm_refs > 0u) g_priv_vm_refs--;
    if (g_priv_vm_refs == 0u) {
        WebPrivKv *cur = g_priv_head;
        g_priv_head = NULL;
        while (cur) {
            WebPrivKv *nxt = cur->next;
            free(cur->key);
            free(cur->val);
            free(cur);
            cur = nxt;
        }
    }
    web_mutex_unlock(&g_priv_mtx);
}

static char *web_strdup_heap(const char *s, size_t len) {
    char *p = (char *)malloc(len + 1u);
    if (!p) return NULL;
    if (s && len > 0u) memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

static char *web_priv_get_alloc(const char *key) {
    if (!key) return NULL;
    web_priv_ensure_init();
    web_mutex_lock(&g_priv_mtx);
    char *res = NULL;
    for (WebPrivKv *e = g_priv_head; e != NULL; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            res = web_strdup_heap(e->val, strlen(e->val));
            break;
        }
    }
    web_mutex_unlock(&g_priv_mtx);
    return res;
}

static void web_priv_set(const char *key, const char *val_or_null) {
    if (!key) return;
    web_priv_ensure_init();
    web_mutex_lock(&g_priv_mtx);
    WebPrivKv *prev = NULL;
    for (WebPrivKv *e = g_priv_head; e != NULL; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            if (!val_or_null) {
                if (prev) prev->next = e->next;
                else g_priv_head = e->next;
                free(e->key);
                free(e->val);
                free(e);
            } else {
                char *nv = web_strdup_heap(val_or_null, strlen(val_or_null));
                if (nv) {
                    free(e->val);
                    e->val = nv;
                }
            }
            web_mutex_unlock(&g_priv_mtx);
            return;
        }
        prev = e;
    }
    if (val_or_null) {
        WebPrivKv *ne = (WebPrivKv *)calloc(1u, sizeof(WebPrivKv));
        if (ne) {
            ne->key = web_strdup_heap(key, strlen(key));
            ne->val = web_strdup_heap(val_or_null, strlen(val_or_null));
            if (ne->key && ne->val) {
                ne->next = g_priv_head;
                g_priv_head = ne;
            } else {
                free(ne->key);
                free(ne->val);
                free(ne);
            }
        }
    }
    web_mutex_unlock(&g_priv_mtx);
}

/* Matches `struct ArcrCssCustomPropMap` layout in `css/arcr_css.c` */
typedef struct WebCssVarMirror {
    ArcrAtom                name;
    const char             *val;
    size_t                  val_len;
    struct WebCssVarMirror *next;
} WebCssVarMirror;

typedef struct WebCssCustomPropMapMirror {
    const struct WebCssCustomPropMapMirror *parent;
    const WebCssVarMirror                  *vars_head;
} WebCssCustomPropMapMirror;

/* -------------------------------------------------------------------------
 * 2. Self-Contained C11 DOM Helpers, UID Table & Shadow/Template Tracking
 * ------------------------------------------------------------------------- */
static uint64_t dom_bloom_bits(ArcrAtom atom) {
    if (atom == ARCR_ATOM_NULL) return 0ULL;
    uint32_t x  = (uint32_t)atom * 0x9E3779B1u;
    uint32_t b1 = (x >> 26) & 63u;
    uint32_t b2 = ((x * 0x85EBCA6Bu) >> 26) & 63u;
    return (1ULL << b1) | (1ULL << b2);
}

static uint64_t dom_node_self_bloom(const ArcrDomNode *n) {
    if (!n || n->kind != ARCR_NODE_ELEMENT) return 0ULL;
    uint64_t bits = dom_bloom_bits(n->tag);
    if (n->id != ARCR_ATOM_NULL) bits |= dom_bloom_bits(n->id);
    uint32_t inline_n = ARCR_MIN(n->class_count, ARCR_MAX_INLINE_CLASSES);
    for (uint32_t i = 0u; i < inline_n; ++i) bits |= dom_bloom_bits(n->classes[i]);
    if (n->class_count > ARCR_MAX_INLINE_CLASSES && n->extra_classes) {
        uint32_t ext = n->class_count - ARCR_MAX_INLINE_CLASSES;
        for (uint32_t i = 0u; i < ext; ++i) bits |= dom_bloom_bits(n->extra_classes[i]);
    }
    return bits;
}

static void dom_recompute_bloom_rec(ArcrDomNode *n) {
    if (!n) return;
    n->ancestor_bloom = n->parent
                            ? (n->parent->ancestor_bloom | dom_node_self_bloom(n->parent))
                            : 0ULL;
    for (ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        dom_recompute_bloom_rec(ch);
    }
}

static void dom_ensure_special_atoms(ArcrJsVm *vm) {
    if (!vm || !vm->host.doc || !vm->host.doc->atoms) return;
    if (vm->frag_atom == ARCR_ATOM_NULL) {
        vm->frag_atom = arcr_atom_intern_lower(
            vm->host.doc->atoms, ARCR_FRAG_TAG_ATOM_NAME, strlen(ARCR_FRAG_TAG_ATOM_NAME));
    }
    if (vm->shadow_atom == ARCR_ATOM_NULL) {
        vm->shadow_atom = arcr_atom_intern_lower(
            vm->host.doc->atoms, ARCR_SHADOW_TAG_ATOM_NAME, strlen(ARCR_SHADOW_TAG_ATOM_NAME));
    }
}

static bool dom_node_is_fragment(ArcrJsVm *vm, const ArcrDomNode *n) {
    if (!vm || !n || n->kind != ARCR_NODE_ELEMENT) return false;
    dom_ensure_special_atoms(vm);
    return (vm->frag_atom != ARCR_ATOM_NULL && n->tag == vm->frag_atom);
}

static bool dom_node_is_shadow_root(ArcrJsVm *vm, const ArcrDomNode *n) {
    if (!vm || !n || n->kind != ARCR_NODE_ELEMENT) return false;
    dom_ensure_special_atoms(vm);
    return (vm->shadow_atom != ARCR_ATOM_NULL && n->tag == vm->shadow_atom);
}

static bool dom_node_is_frag_or_shadow(ArcrJsVm *vm, const ArcrDomNode *n) {
    return dom_node_is_fragment(vm, n) || dom_node_is_shadow_root(vm, n);
}

static void dom_record_shadow_link(ArcrJsVm *vm, uint32_t shadow_uid, uint32_t host_uid, bool is_open) {
    if (!vm || shadow_uid == 0u || host_uid == 0u) return;
    for (size_t i = 0u; i < vm->shadow_link_count; ++i) {
        if (vm->shadow_links[i].shadow_uid == shadow_uid) {
            vm->shadow_links[i].host_uid = host_uid;
            vm->shadow_links[i].is_open  = is_open;
            return;
        }
    }
    if (vm->shadow_link_count >= vm->shadow_link_cap) {
        size_t ncap = vm->shadow_link_cap ? vm->shadow_link_cap * 2u : 16u;
        WebShadowLink * narr = (WebShadowLink *)realloc(vm->shadow_links, ncap * sizeof(WebShadowLink));
        if (!narr) return;
        vm->shadow_links    = narr;
        vm->shadow_link_cap = ncap;
    }
    vm->shadow_links[vm->shadow_link_count].shadow_uid = shadow_uid;
    vm->shadow_links[vm->shadow_link_count].host_uid   = host_uid;
    vm->shadow_links[vm->shadow_link_count].is_open    = is_open;
    vm->shadow_link_count++;
}

static uint32_t dom_find_shadow_host_uid(const ArcrJsVm *vm, uint32_t shadow_uid, bool *out_open) {
    if (!vm || shadow_uid == 0u) return 0u;
    for (size_t i = 0u; i < vm->shadow_link_count; ++i) {
        if (vm->shadow_links[i].shadow_uid == shadow_uid) {
            if (out_open) *out_open = vm->shadow_links[i].is_open;
            return vm->shadow_links[i].host_uid;
        }
    }
    return 0u;
}

static bool dom_uid_ensure_cap(ArcrJsVm *vm, uint32_t max_uid) {
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

void web_uid_register(ArcrJsVm *vm, ArcrDomNode *n) {
    if (!vm || !n) return;
    if (dom_uid_ensure_cap(vm, n->node_uid)) {
        vm->uid_table[n->node_uid] = n;
    }
}

static void dom_uid_index_tree_rec(ArcrJsVm *vm, ArcrDomNode *n) {
    if (!vm || !n) return;
    web_uid_register(vm, n);
    if (n->template_content) {
        dom_ensure_special_atoms(vm);
        if (n->template_content->kind == ARCR_NODE_ELEMENT && n->template_content->tag == ARCR_ATOM_NULL) {
            n->template_content->tag = vm->frag_atom;
        }
        dom_uid_index_tree_rec(vm, n->template_content);
    }
    if (n->shadow_root) {
        dom_ensure_special_atoms(vm);
        n->shadow_root->tag = vm->shadow_atom;
        dom_record_shadow_link(vm, n->shadow_root->node_uid, n->node_uid, n->shadow_open);
        dom_uid_index_tree_rec(vm, n->shadow_root);
    }
    for (ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        dom_uid_index_tree_rec(vm, ch);
    }
}

void web_uid_sync_document(ArcrJsVm *vm) {
    if (!vm || !vm->host.doc || !vm->host.doc->root) return;
    dom_ensure_special_atoms(vm);
    if (!dom_uid_ensure_cap(vm, vm->host.doc->next_uid + 8u)) return;
    dom_uid_index_tree_rec(vm, vm->host.doc->root);
}

ArcrDomNode *web_dom_by_uid(ArcrJsVm *vm, uint32_t uid) {
    if (!vm || !vm->host.doc) return NULL;
    ArcrDocument *doc = vm->host.doc;
    if (uid == 0u) return doc->root;
    if (uid < vm->uid_cap && vm->uid_table[uid]) {
        ArcrDomNode *n = vm->uid_table[uid];
        if (n->node_uid == uid) return n;
        vm->uid_table[uid] = NULL;
    }
    web_uid_sync_document(vm);
    if (uid < vm->uid_cap && vm->uid_table[uid] && vm->uid_table[uid]->node_uid == uid) {
        return vm->uid_table[uid];
    }
    return NULL;
}

static bool dom_node_is_connected(ArcrJsVm *vm, const ArcrDomNode *n) {
    if (!vm || !vm->host.doc || !vm->host.doc->root || !n) return false;
    const ArcrDomNode *cur = n;
    int guard = 0;
    while (cur && guard++ < 512) {
        if (cur == vm->host.doc->root) return true;
        if (cur->parent) {
            cur = cur->parent;
            continue;
        }
        if (dom_node_is_shadow_root(vm, cur)) {
            uint32_t host_uid = dom_find_shadow_host_uid(vm, cur->node_uid, NULL);
            cur = host_uid ? web_dom_by_uid(vm, host_uid) : NULL;
            continue;
        }
        break;
    }
    return false;
}

static void dom_unlink(ArcrDocument *doc, ArcrDomNode *node) {
    if (!node || !node->parent) return;
    ArcrDomNode *p = node->parent;
    if (node->prev_sibling) {
        node->prev_sibling->next_sibling = node->next_sibling;
    } else if (p->first_child == node) {
        p->first_child = node->next_sibling;
    } else {
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
    if (node->next_sibling) node->next_sibling->prev_sibling = node->prev_sibling;
    if (p->last_child == node) p->last_child = node->prev_sibling;
    node->parent       = NULL;
    node->prev_sibling = NULL;
    node->next_sibling = NULL;
    if (doc) {
        if (doc->focused_node == node) doc->focused_node = NULL;
        if (doc->hovered_node == node) doc->hovered_node = NULL;
        doc->dom_dirty = true;
    }
}

static void dom_append_style_block(ArcrDocument *doc, const char *css, size_t len) {
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

static void dom_sync_dynamic_style(ArcrDocument *doc, ArcrDomNode *node) {
    if (!doc || !node || node->kind != ARCR_NODE_ELEMENT) return;
    const char *tname = arcr_atom_cstr(doc->atoms, node->tag);
    if (arcr_str_eq_nocase(tname, "style")) {
        if (node->text && node->text_len > 0u) {
            dom_append_style_block(doc, node->text, node->text_len);
        } else {
            for (ArcrDomNode *ch = node->first_child; ch != NULL; ch = ch->next_sibling) {
                if (ch->kind == ARCR_NODE_TEXT && ch->text && ch->text_len > 0u) {
                    dom_append_style_block(doc, ch->text, ch->text_len);
                }
            }
        }
    }
}

static bool dom_is_ancestor_or_self(const ArcrDomNode *anc, const ArcrDomNode *node) {
    for (const ArcrDomNode *p = node; p != NULL; p = p->parent) {
        if (p == anc) return true;
    }
    return false;
}

static void dom_insert_before_raw(ArcrJsVm *vm, ArcrDomNode *parent,
                                  ArcrDomNode *child, ArcrDomNode *ref) {
    if (!vm || !vm->host.doc || !parent || !child || parent == child) return;
    if (dom_is_ancestor_or_self(child, parent)) return;
    ArcrDocument *doc = vm->host.doc;

    if (dom_node_is_fragment(vm, child)) {
        ArcrDomNode *cur = child->first_child;
        while (cur) {
            ArcrDomNode *nxt = cur->next_sibling;
            dom_insert_before_raw(vm, parent, cur, ref);
            cur = nxt;
        }
        return;
    }

    if (ref && ref->parent != parent) ref = NULL;
    if (ref == child) return;

    dom_unlink(doc, child);
    child->parent = parent;

    if (!ref) {
        child->prev_sibling = parent->last_child;
        child->next_sibling = NULL;
        if (parent->last_child) parent->last_child->next_sibling = child;
        else parent->first_child = child;
        parent->last_child = child;
    } else {
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
    dom_recompute_bloom_rec(child);
    if (dom_node_is_connected(vm, parent)) {
        dom_sync_dynamic_style(doc, child);
    }
    doc->dom_dirty = true;
}

static ArcrDomNode *dom_alloc_node(ArcrJsVm *vm, ArcrNodeKind kind) {
    if (!vm || !vm->host.doc) return NULL;
    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *n = (ArcrDomNode *)arcr_slab_alloc(&doc->node_slab);
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->kind     = kind;
    n->node_uid = ++doc->next_uid;
    doc->node_count++;
    doc->dom_dirty = true;
    web_uid_register(vm, n);
    return n;
}

static ArcrDomNode *dom_create_fragment(ArcrJsVm *vm) {
    if (!vm || !vm->host.doc) return NULL;
    dom_ensure_special_atoms(vm);
    ArcrDomNode *n = dom_alloc_node(vm, ARCR_NODE_ELEMENT);
    if (n) n->tag = vm->frag_atom;
    return n;
}

static ArcrDomNode *dom_create_shadow_root(ArcrJsVm *vm) {
    if (!vm || !vm->host.doc) return NULL;
    dom_ensure_special_atoms(vm);
    ArcrDomNode *n = dom_alloc_node(vm, ARCR_NODE_ELEMENT);
    if (n) n->tag = vm->shadow_atom;
    return n;
}

static ArcrDomNode *dom_create_element(ArcrJsVm *vm, const char *tag, size_t len) {
    if (!vm || !vm->host.doc || !tag || len == 0u) return NULL;
    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *n = dom_alloc_node(vm, ARCR_NODE_ELEMENT);
    if (!n) return NULL;
    n->tag = arcr_atom_intern_lower(doc->atoms, tag, len);
    if (arcr_str_eq_nocase_n(tag, len, "video", 5u) ||
        arcr_str_eq_nocase_n(tag, len, "audio", 5u)) {
        doc->has_media = true;
    } else if (arcr_str_eq_nocase_n(tag, len, "img", 3u) ||
               arcr_str_eq_nocase_n(tag, len, "canvas", 6u)) {
        doc->has_images = true;
    } else if (arcr_str_eq_nocase_n(tag, len, "template", 8u)) {
        n->template_content = dom_create_fragment(vm);
    }
    return n;
}

static ArcrDomNode *dom_create_text(ArcrJsVm *vm, const char *s, size_t len) {
    if (!vm || !vm->host.doc) return NULL;
    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *n = dom_alloc_node(vm, ARCR_NODE_TEXT);
    if (!n) return NULL;
    n->text     = arcr_arena_strdup(&doc->arena, s ? s : "", len);
    n->text_len = len;
    return n;
}

static ArcrDomNode *dom_create_comment(ArcrJsVm *vm, const char *s, size_t len) {
    if (!vm || !vm->host.doc) return NULL;
    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *n = dom_alloc_node(vm, ARCR_NODE_COMMENT);
    if (!n) return NULL;
    n->text     = arcr_arena_strdup(&doc->arena, s ? s : "", len);
    n->text_len = len;
    return n;
}

static bool dom_elem_has_class(const ArcrDomNode *el, ArcrAtom cls) {
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

static void dom_elem_add_class_atom(ArcrDocument *doc, ArcrDomNode *el, ArcrAtom cls) {
    if (!doc || !el || cls == ARCR_ATOM_NULL || dom_elem_has_class(el, cls)) return;
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

static void dom_sync_classes_from_str(ArcrDocument *doc, ArcrDomNode *el,
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
            dom_elem_add_class_atom(doc, el, a);
        }
    }
    dom_recompute_bloom_rec(el);
}

static const char *dom_get_attr(const ArcrDocument *doc, const ArcrDomNode *el, const char *name) {
    if (!doc || !el || el->kind != ARCR_NODE_ELEMENT || !name) return NULL;
    ArcrAtom na = arcr_atom_intern_lower(doc->atoms, name, strlen(name));
    for (const ArcrAttr *a = el->first_attr; a != NULL; a = a->next) {
        if (a->name == na) return a->value;
    }
    return NULL;
}

static void dom_set_input_val(ArcrDocument *doc, ArcrDomNode *el, const char *val) {
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

static void dom_set_attr(ArcrDocument *doc, ArcrDomNode *el,
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
        dom_recompute_bloom_rec(el);
    } else if (arcr_str_eq_nocase_n(name, nlen, "class", 5u)) {
        dom_sync_classes_from_str(doc, el, dup_v, vlen);
    } else if (arcr_str_eq_nocase_n(name, nlen, "checked", 7u)) {
        el->state_flags |= (uint32_t)ARCR_STATE_CHECKED;
    } else if (arcr_str_eq_nocase_n(name, nlen, "value", 5u) && !el->input_value) {
        dom_set_input_val(doc, el, dup_v);
    }
    doc->dom_dirty = true;
}

static void dom_remove_attr(ArcrDocument *doc, ArcrDomNode *el, const char *name) {
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
        dom_recompute_bloom_rec(el);
    } else if (arcr_str_eq_nocase_n(name, nlen, "class", 5u)) {
        el->class_count = 0u;
        dom_recompute_bloom_rec(el);
    } else if (arcr_str_eq_nocase_n(name, nlen, "checked", 7u)) {
        el->state_flags &= ~((uint32_t)ARCR_STATE_CHECKED);
    }
    doc->dom_dirty = true;
}

ArcrDomSurface *web_ensure_node_surface(ArcrJsVm *vm, ArcrDomNode *el, int32_t w, int32_t h) {
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
 * 3. HTML5 Serializer, Fragment Parser (<template> aware) & CSS Selectors
 * ------------------------------------------------------------------------- */
typedef struct DomByteBuf {
    char  *data;
    size_t len;
    size_t cap;
} DomByteBuf;

static bool dbuf_push(DomByteBuf *b, const char *s, size_t n) {
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

static bool dbuf_cstr(DomByteBuf *b, const char *s) {
    return dbuf_push(b, s, s ? strlen(s) : 0u);
}

static void dbuf_html_escaped(DomByteBuf *b, const char *s, size_t len, bool is_attr) {
    if (!s) return;
    for (size_t i = 0u; i < len; ++i) {
        char c = s[i];
        if (c == '&') dbuf_cstr(b, "&amp;");
        else if (c == '<' && !is_attr) dbuf_cstr(b, "&lt;");
        else if (c == '>' && !is_attr) dbuf_cstr(b, "&gt;");
        else if (c == '"' && is_attr)  dbuf_cstr(b, "&quot;");
        else dbuf_push(b, &c, 1u);
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

static void dom_serialize_node_rec(ArcrJsVm *vm, const ArcrDocument *doc,
                                   const ArcrDomNode *n, bool include_self, DomByteBuf *out) {
    if (!doc || !n || !out) return;
    if (!include_self || n->kind == ARCR_NODE_DOCUMENT || dom_node_is_frag_or_shadow(vm, n)) {
        const ArcrDomNode *container = n;
        if (!include_self && n->kind == ARCR_NODE_ELEMENT &&
            arcr_str_eq_nocase(arcr_atom_cstr(doc->atoms, n->tag), "template") &&
            n->template_content) {
            container = n->template_content;
        }
        for (const ArcrDomNode *ch = container->first_child; ch != NULL; ch = ch->next_sibling) {
            dom_serialize_node_rec(vm, doc, ch, true, out);
        }
        return;
    }
    if (n->kind == ARCR_NODE_TEXT) {
        dbuf_html_escaped(out, n->text, n->text_len, false);
        return;
    }
    if (n->kind == ARCR_NODE_COMMENT) {
        dbuf_cstr(out, "<!--");
        dbuf_push(out, n->text, n->text_len);
        dbuf_cstr(out, "-->");
        return;
    }
    if (n->kind == ARCR_NODE_ELEMENT) {
        const char *tag = arcr_atom_cstr(doc->atoms, n->tag);
        dbuf_cstr(out, "<");
        dbuf_cstr(out, tag);
        for (const ArcrAttr *a = n->first_attr; a != NULL; a = a->next) {
            const char *an = arcr_atom_cstr(doc->atoms, a->name);
            dbuf_cstr(out, " ");
            dbuf_cstr(out, an);
            dbuf_cstr(out, "=\"");
            if (a->value) dbuf_html_escaped(out, a->value, strlen(a->value), true);
            dbuf_cstr(out, "\"");
        }
        dbuf_cstr(out, ">");
        if (is_void_html_tag(tag)) return;
        if (arcr_str_eq_nocase(tag, "template") && n->template_content) {
            for (const ArcrDomNode *ch = n->template_content->first_child; ch != NULL; ch = ch->next_sibling) {
                dom_serialize_node_rec(vm, doc, ch, true, out);
            }
        } else if (!n->first_child && n->text && n->text_len > 0u) {
            dbuf_push(out, n->text, n->text_len);
        } else {
            for (const ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
                dom_serialize_node_rec(vm, doc, ch, true, out);
            }
        }
        dbuf_cstr(out, "</");
        dbuf_cstr(out, tag);
        dbuf_cstr(out, ">");
    }
}

static void dom_collect_text_rec(const ArcrDomNode *n, DomByteBuf *out) {
    if (!n || !out) return;
    if (n->kind == ARCR_NODE_TEXT && n->text && n->text_len > 0u) {
        dbuf_push(out, n->text, n->text_len);
        return;
    }
    if (n->kind == ARCR_NODE_ELEMENT && !n->first_child && n->text && n->text_len > 0u) {
        dbuf_push(out, n->text, n->text_len);
        return;
    }
    for (const ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        dom_collect_text_rec(ch, out);
    }
}

static void decode_html_entities(const char *src, size_t len, DomByteBuf *out) {
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
                if (arcr_str_eq_nocase_n(ent, elen, "amp", 3u))  { dbuf_cstr(out, "&"); i = semi + 1u; continue; }
                if (arcr_str_eq_nocase_n(ent, elen, "lt", 2u))   { dbuf_cstr(out, "<"); i = semi + 1u; continue; }
                if (arcr_str_eq_nocase_n(ent, elen, "gt", 2u))   { dbuf_cstr(out, ">"); i = semi + 1u; continue; }
                if (arcr_str_eq_nocase_n(ent, elen, "quot", 4u)) { dbuf_cstr(out, "\""); i = semi + 1u; continue; }
                if (arcr_str_eq_nocase_n(ent, elen, "apos", 4u) ||
                    arcr_str_eq_nocase_n(ent, elen, "#39", 3u))  { dbuf_cstr(out, "'"); i = semi + 1u; continue; }
                if (arcr_str_eq_nocase_n(ent, elen, "nbsp", 4u)) { dbuf_cstr(out, "\xC2\xA0"); i = semi + 1u; continue; }
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
                        dbuf_push(out, u8, ulen);
                        i = semi + 1u;
                        continue;
                    }
                }
            }
        }
        dbuf_push(out, &src[i], 1u);
        ++i;
    }
}

static bool is_html_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static void dom_parse_html_fragment(ArcrJsVm *vm, ArcrDomNode *target_parent,
                                    const char *html, size_t len) {
    if (!vm || !vm->host.doc || !target_parent || !html) return;
    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *stack[64];
    ArcrAtom     open_tags[64];
    int sp = 0;
    stack[0]     = target_parent;
    open_tags[0] = ARCR_ATOM_NULL;

    size_t i = 0u;
    while (i < len) {
        if (html[i] != '<') {
            size_t st = i;
            while (i < len && html[i] != '<') ++i;
            DomByteBuf dec = {0};
            decode_html_entities(html + st, i - st, &dec);
            if (dec.len > 0u) {
                ArcrDomNode *tn = dom_create_text(vm, dec.data, dec.len);
                if (tn) dom_insert_before_raw(vm, stack[sp], tn, NULL);
            }
            free(dec.data);
            continue;
        }

        if (i + 4u <= len && memcmp(html + i, "<!--", 4u) == 0) {
            size_t st = i + 4u;
            size_t end = st;
            while (end + 3u <= len && memcmp(html + end, "-->", 3u) != 0) ++end;
            ArcrDomNode *cn = dom_create_comment(vm, html + st, end - st);
            if (cn) dom_insert_before_raw(vm, stack[sp], cn, NULL);
            i = (end + 3u <= len) ? (end + 3u) : len;
            continue;
        }

        if (i + 2u < len && html[i + 1u] == '!') {
            while (i < len && html[i] != '>') ++i;
            if (i < len) ++i;
            continue;
        }

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
                    if (open_tags[k] == close_atom) {
                        sp = k - 1;
                        break;
                    }
                }
            }
            i = (gt < len) ? (gt + 1u) : len;
            continue;
        }

        size_t st = i + 1u;
        size_t te = st;
        while (te < len && !is_html_space(html[te]) && html[te] != '>' && html[te] != '/') ++te;
        if (te == st) {
            ArcrDomNode *tn = dom_create_text(vm, "<", 1u);
            if (tn) dom_insert_before_raw(vm, stack[sp], tn, NULL);
            ++i;
            continue;
        }

        ArcrDomNode *el = dom_create_element(vm, html + st, te - st);
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
            DomByteBuf av_dec = {0};
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
                dom_set_attr(doc, el, html + an_s, an_e - an_s, av_ptr, av_len);
            }
            free(av_dec.data);
        }
        if (p < len && html[p] == '>') ++p;
        i = p;

        if (!el) continue;
        const char *tname = arcr_atom_cstr(doc->atoms, el->tag);

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
            dom_insert_before_raw(vm, stack[sp], el, NULL);
            continue;
        }

        dom_insert_before_raw(vm, stack[sp], el, NULL);
        if (!self_close && !is_void_html_tag(tname) && sp + 1 < 64) {
            ++sp;
            stack[sp] = (arcr_str_eq_nocase(tname, "template") && el->template_content)
                            ? el->template_content
                            : el;
            open_tags[sp] = el->tag;
        }
    }
}

/* CSS Selector matching */
static bool dom_match_selector_list(ArcrJsVm *vm, const ArcrDocument *doc, const ArcrDomNode *el,
                                    const char *sel, size_t sel_len);

static bool dom_attr_word_includes(const char *actual, size_t act_l, const char *ev, size_t ev_l) {
    if (ev_l == 0u) return false;
    size_t i = 0u;
    while (i < act_l) {
        while (i < act_l && is_html_space(actual[i])) ++i;
        size_t st = i;
        while (i < act_l && !is_html_space(actual[i])) ++i;
        if (i - st == ev_l && memcmp(actual + st, ev, ev_l) == 0) return true;
    }
    return false;
}

static int32_t dom_elem_child_index_1based(ArcrJsVm *vm, const ArcrDomNode *el) {
    if (!el || !el->parent) return 0;
    int32_t idx = 0;
    for (const ArcrDomNode *c = el->parent->first_child; c != NULL; c = c->next_sibling) {
        if (c->kind == ARCR_NODE_ELEMENT && !dom_node_is_frag_or_shadow(vm, c)) {
            idx++;
            if (c == el) return idx;
        }
    }
    return 0;
}

static bool dom_match_nth(int32_t idx, const char *arg, size_t argl) {
    if (idx <= 0 || !arg) return false;
    arcr_str_trim_view(&arg, &argl);
    if (arcr_str_eq_nocase_n(arg, argl, "odd", 3u)) return (idx % 2) == 1;
    if (arcr_str_eq_nocase_n(arg, argl, "even", 4u)) return (idx % 2) == 0;
    int32_t exact = 0;
    if (arcr_parse_i32(arg, argl, &exact)) return idx == exact;
    const char *n_pos = NULL;
    for (size_t i = 0u; i < argl; ++i) {
        if (arg[i] == 'n' || arg[i] == 'N') { n_pos = arg + i; break; }
    }
    if (!n_pos) return false;
    size_t a_len = (size_t)(n_pos - arg);
    const char *a_str = arg;
    arcr_str_trim_view(&a_str, &a_len);
    int32_t a = 1;
    if (a_len == 1u && a_str[0] == '-') a = -1;
    else if (a_len == 1u && a_str[0] == '+') a = 1;
    else if (a_len > 0u && !arcr_parse_i32(a_str, a_len, &a)) return false;

    const char *b_str = n_pos + 1;
    size_t b_len = (size_t)((arg + argl) - b_str);
    arcr_str_trim_view(&b_str, &b_len);
    int32_t b = 0;
    if (b_len > 0u) {
        char clean[32];
        size_t cl = 0u;
        for (size_t i = 0u; i < b_len && cl + 1u < sizeof(clean); ++i) {
            if (!is_html_space(b_str[i])) clean[cl++] = b_str[i];
        }
        clean[cl] = '\0';
        if (!arcr_parse_i32(clean, cl, &b)) return false;
    }
    if (a == 0) return idx == b;
    int32_t diff = idx - b;
    if ((a > 0 && diff < 0) || (a < 0 && diff > 0)) return false;
    return (diff % a) == 0;
}

static bool dom_match_compound(ArcrJsVm *vm, const ArcrDocument *doc, const ArcrDomNode *el,
                               const char *s, size_t len) {
    if (!doc || !el || el->kind != ARCR_NODE_ELEMENT || dom_node_is_frag_or_shadow(vm, el)) return false;
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
                if (!id_c || strlen(id_c) != (i - st) || memcmp(id_c, s + st, i - st) != 0) return false;
            } else {
                ArcrAtom ca = arcr_atom_intern(doc->atoms, s + st, i - st);
                if (!dom_elem_has_class(el, ca)) return false;
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
            while (eq < in_len && in[eq] != '=' && in[eq] != '^' && in[eq] != '$' &&
                   in[eq] != '*' && in[eq] != '~' && in[eq] != '|') {
                ++eq;
            }
            const char *an = in;
            size_t an_l = eq;
            arcr_str_trim_view(&an, &an_l);
            char an_buf[128];
            if (an_l >= sizeof(an_buf)) an_l = sizeof(an_buf) - 1u;
            memcpy(an_buf, an, an_l);
            an_buf[an_l] = '\0';
            const char *actual = dom_get_attr(doc, el, an_buf);
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
                } else if (op == '~') {
                    if (!dom_attr_word_includes(actual, act_l, ev, ev_l)) return false;
                } else if (op == '|') {
                    if (act_l < ev_l || memcmp(actual, ev, ev_l) != 0) return false;
                    if (act_l > ev_l && actual[ev_l] != '-') return false;
                }
            }
        } else if (c == ':') {
            ++i;
            if (i < len && s[i] == ':') return false;
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
            } else if (arcr_str_eq_nocase_n(ps, psl, "focus", 5u)) {
                if ((el->state_flags & (uint32_t)ARCR_STATE_FOCUS) == 0u && doc->focused_node != el) return false;
            } else if (arcr_str_eq_nocase_n(ps, psl, "hover", 5u)) {
                if ((el->state_flags & (uint32_t)ARCR_STATE_HOVER) == 0u && doc->hovered_node != el) return false;
            } else if (arcr_str_eq_nocase_n(ps, psl, "disabled", 8u)) {
                if (!dom_get_attr(doc, el, "disabled")) return false;
            } else if (arcr_str_eq_nocase_n(ps, psl, "enabled", 7u)) {
                if (dom_get_attr(doc, el, "disabled")) return false;
            } else if (arcr_str_eq_nocase_n(ps, psl, "root", 4u)) {
                if (!el->parent || el->parent != doc->root) return false;
            } else if (arcr_str_eq_nocase_n(ps, psl, "empty", 5u)) {
                for (const ArcrDomNode *ch = el->first_child; ch != NULL; ch = ch->next_sibling) {
                    if (ch->kind == ARCR_NODE_ELEMENT) return false;
                    if (ch->kind == ARCR_NODE_TEXT && ch->text_len > 0u) return false;
                }
            } else if (arcr_str_eq_nocase_n(ps, psl, "first-child", 11u)) {
                if (dom_elem_child_index_1based(vm, el) != 1) return false;
            } else if (arcr_str_eq_nocase_n(ps, psl, "last-child", 10u)) {
                for (const ArcrDomNode *ch = el->next_sibling; ch != NULL; ch = ch->next_sibling) {
                    if (ch->kind == ARCR_NODE_ELEMENT && !dom_node_is_frag_or_shadow(vm, ch)) return false;
                }
            } else if (arcr_str_eq_nocase_n(ps, psl, "only-child", 10u)) {
                if (dom_elem_child_index_1based(vm, el) != 1) return false;
                for (const ArcrDomNode *ch = el->next_sibling; ch != NULL; ch = ch->next_sibling) {
                    if (ch->kind == ARCR_NODE_ELEMENT && !dom_node_is_frag_or_shadow(vm, ch)) return false;
                }
            } else if (arcr_str_eq_nocase_n(ps, psl, "nth-child", 9u)) {
                if (!dom_match_nth(dom_elem_child_index_1based(vm, el), arg, argl)) return false;
            } else if (arcr_str_eq_nocase_n(ps, psl, "not", 3u) && arg) {
                if (dom_match_selector_list(vm, doc, el, arg, argl)) return false;
            } else if (arcr_str_eq_nocase_n(ps, psl, "is", 2u) ||
                       arcr_str_eq_nocase_n(ps, psl, "where", 5u)) {
                if (arg && !dom_match_selector_list(vm, doc, el, arg, argl)) return false;
            } else {
                return false;
            }
        } else {
            size_t st = i;
            while (i < len && s[i] != '#' && s[i] != '.' && s[i] != '[' && s[i] != ':') ++i;
            const char *tname = arcr_atom_cstr(doc->atoms, el->tag);
            if (!tname || !arcr_str_eq_nocase_n(tname, strlen(tname), s + st, i - st)) return false;
        }
    }
    return true;
}

typedef struct DomSelPart {
    const char *ptr;
    size_t      len;
    char        comb;
} DomSelPart;

static bool dom_match_chain(ArcrJsVm *vm, const ArcrDocument *doc, const ArcrDomNode *el,
                            const DomSelPart *parts, int idx) {
    if (!dom_match_compound(vm, doc, el, parts[idx].ptr, parts[idx].len)) return false;
    if (idx == 0) return true;

    char comb = parts[idx].comb;
    if (comb == '>') {
        const ArcrDomNode *p = el->parent;
        return p && p->kind == ARCR_NODE_ELEMENT && !dom_node_is_frag_or_shadow(vm, p) &&
               dom_match_chain(vm, doc, p, parts, idx - 1);
    }
    if (comb == ' ') {
        for (const ArcrDomNode *p = el->parent; p != NULL; p = p->parent) {
            if (dom_node_is_frag_or_shadow(vm, p)) break; /* Never cross shadow boundary */
            if (p->kind == ARCR_NODE_ELEMENT && dom_match_chain(vm, doc, p, parts, idx - 1)) return true;
        }
        return false;
    }
    if (comb == '+' || comb == '~') {
        if (!el->parent) return false;
        const ArcrDomNode *prev_el = NULL;
        for (const ArcrDomNode *c = el->parent->first_child; c != NULL && c != el; c = c->next_sibling) {
            if (c->kind == ARCR_NODE_ELEMENT && !dom_node_is_frag_or_shadow(vm, c)) {
                if (comb == '~' && dom_match_chain(vm, doc, c, parts, idx - 1)) return true;
                prev_el = c;
            }
        }
        if (comb == '+') return prev_el && dom_match_chain(vm, doc, prev_el, parts, idx - 1);
    }
    return false;
}

static bool dom_match_single_selector(ArcrJsVm *vm, const ArcrDocument *doc, const ArcrDomNode *el,
                                      const char *sel, size_t len) {
    arcr_str_trim_view(&sel, &len);
    if (len == 0u) return false;

    DomSelPart parts[12];
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
    return dom_match_chain(vm, doc, el, parts, count - 1);
}

static bool dom_match_selector_list(ArcrJsVm *vm, const ArcrDocument *doc, const ArcrDomNode *el,
                                    const char *sel, size_t sel_len) {
    if (!doc || !el || el->kind != ARCR_NODE_ELEMENT || dom_node_is_frag_or_shadow(vm, el) || !sel) return false;
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
        if (dom_match_single_selector(vm, doc, el, sel + st, i - st)) return true;
        if (i < sel_len && sel[i] == ',') ++i;
    }
    return false;
}

/* -------------------------------------------------------------------------
 * 4. CSS Color Parser (Hex, rgb/rgba, hsl/hsla, oklch, Named Colors)
 * ------------------------------------------------------------------------- */
static int dom_hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static double hsl_hue_to_rgb(double p, double q, double t) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0 / 2.0) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

bool web_parse_css_color(const char *s, size_t len, uint32_t *out_argb) {
    if (!out_argb) return false;
    *out_argb = 0xFF000000u;
    if (!s) return false;
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;

    if (s[0] == '#') {
        if (len == 4u) {
            int r = dom_hex_val(s[1]), g = dom_hex_val(s[2]), b = dom_hex_val(s[3]);
            if (r >= 0 && g >= 0 && b >= 0) {
                *out_argb = 0xFF000000u | ((uint32_t)(r * 17) << 16) |
                            ((uint32_t)(g * 17) << 8) | (uint32_t)(b * 17);
                return true;
            }
        } else if (len == 5u) {
            int r = dom_hex_val(s[1]), g = dom_hex_val(s[2]), b = dom_hex_val(s[3]), a = dom_hex_val(s[4]);
            if (r >= 0 && g >= 0 && b >= 0 && a >= 0) {
                *out_argb = ((uint32_t)(a * 17) << 24) | ((uint32_t)(r * 17) << 16) |
                            ((uint32_t)(g * 17) << 8) | (uint32_t)(b * 17);
                return true;
            }
        } else if (len == 7u || len == 9u) {
            int r1 = dom_hex_val(s[1]), r2 = dom_hex_val(s[2]);
            int g1 = dom_hex_val(s[3]), g2 = dom_hex_val(s[4]);
            int b1 = dom_hex_val(s[5]), b2 = dom_hex_val(s[6]);
            int a1 = 15, a2 = 15;
            if (len == 9u) { a1 = dom_hex_val(s[7]); a2 = dom_hex_val(s[8]); }
            if (r1 >= 0 && r2 >= 0 && g1 >= 0 && g2 >= 0 && b1 >= 0 && b2 >= 0 && a1 >= 0 && a2 >= 0) {
                *out_argb = ((uint32_t)((a1 << 4) | a2) << 24) |
                            ((uint32_t)((r1 << 4) | r2) << 16) |
                            ((uint32_t)((g1 << 4) | g2) << 8) |
                            (uint32_t)((b1 << 4) | b2);
                return true;
            }
        }
        return false;
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
                *out_argb = (ua << 24) | (ur << 16) | (ug << 8) | ub;
                return true;
            }
        }
        return false;
    }

    if ((len > 4u && arcr_str_eq_nocase_n(s, 4u, "hsl(", 4u)) ||
        (len > 5u && arcr_str_eq_nocase_n(s, 5u, "hsla(", 5u))) {
        const char *lp = (const char *)memchr(s, '(', len);
        if (lp) {
            double h = 0.0, sat = 0.0, lig = 0.0, a = 1.0;
            char tmp[96];
            size_t il = (size_t)((s + len) - (lp + 1));
            if (il >= sizeof(tmp)) il = sizeof(tmp) - 1u;
            memcpy(tmp, lp + 1, il);
            tmp[il] = '\0';
            for (size_t k = 0u; tmp[k] != '\0'; ++k) {
                if (tmp[k] == ',' || tmp[k] == '/' || tmp[k] == ')' || tmp[k] == '%') tmp[k] = ' ';
            }
            int n = sscanf(tmp, "%lf %lf %lf %lf", &h, &sat, &lig, &a);
            if (n >= 3) {
                h = fmod(h, 360.0);
                if (h < 0.0) h += 360.0;
                h /= 360.0;
                sat = ARCR_CLAMP(sat / 100.0, 0.0, 1.0);
                lig = ARCR_CLAMP(lig / 100.0, 0.0, 1.0);
                double rf = lig, gf = lig, bf = lig;
                if (sat > 0.0) {
                    double q = (lig < 0.5) ? (lig * (1.0 + sat)) : (lig + sat - lig * sat);
                    double p = 2.0 * lig - q;
                    rf = hsl_hue_to_rgb(p, q, h + 1.0 / 3.0);
                    gf = hsl_hue_to_rgb(p, q, h);
                    bf = hsl_hue_to_rgb(p, q, h - 1.0 / 3.0);
                }
                uint32_t ur = (uint32_t)ARCR_CLAMP((int)(rf * 255.0 + 0.5), 0, 255);
                uint32_t ug = (uint32_t)ARCR_CLAMP((int)(gf * 255.0 + 0.5), 0, 255);
                uint32_t ub = (uint32_t)ARCR_CLAMP((int)(bf * 255.0 + 0.5), 0, 255);
                uint32_t ua = (n >= 4) ? (uint32_t)ARCR_CLAMP((int)((a > 1.0 ? a / 100.0 : a) * 255.0 + 0.5), 0, 255) : 255u;
                *out_argb = (ua << 24) | (ur << 16) | (ug << 8) | ub;
                return true;
            }
        }
        return false;
    }

    if (len > 6u && arcr_str_eq_nocase_n(s, 6u, "oklch(", 6u)) {
        const char *lp = s + 6;
        double l = 0.0, c = 0.0, h = 0.0, a = 1.0;
        bool l_pct = false;
        char tmp[96];
        size_t il = len - 6u;
        if (il >= sizeof(tmp)) il = sizeof(tmp) - 1u;
        memcpy(tmp, lp, il);
        tmp[il] = '\0';
        const char *pct = strchr(tmp, '%');
        if (pct && pct < tmp + 16) l_pct = true;
        for (size_t k = 0u; tmp[k] != '\0'; ++k) {
            if (tmp[k] == ',' || tmp[k] == '/' || tmp[k] == ')' || tmp[k] == '%') tmp[k] = ' ';
        }
        int n = sscanf(tmp, "%lf %lf %lf %lf", &l, &c, &h, &a);
        if (n >= 3) {
            if (l_pct || l > 1.0) l /= 100.0;
            double hr = h * (3.14159265358979323846 / 180.0);
            double ok_a = c * cos(hr);
            double ok_b = c * sin(hr);
            double l_ = l + 0.3963377774 * ok_a + 0.2158037573 * ok_b;
            double m_ = l - 0.1055613458 * ok_a - 0.0638541728 * ok_b;
            double s_ = l - 0.0894841775 * ok_a - 1.2914855480 * ok_b;
            double lc = l_ * l_ * l_;
            double mc = m_ * m_ * m_;
            double sc = s_ * s_ * s_;
            double rl = +4.0767416621 * lc - 3.3077115913 * mc + 0.2309699292 * sc;
            double gl = -1.2684380046 * lc + 2.6097574011 * mc - 0.3413193965 * sc;
            double bl = -0.0041960863 * lc - 0.7034186147 * mc + 1.7076147010 * sc;
            double comps[3] = { rl, gl, bl };
            uint32_t out_c[3];
            for (int ci = 0; ci < 3; ++ci) {
                double v = ARCR_CLAMP(comps[ci], 0.0, 1.0);
                double sr = (v <= 0.0031308) ? (12.92 * v) : (1.055 * pow(v, 1.0 / 2.4) - 0.055);
                out_c[ci] = (uint32_t)ARCR_CLAMP((int)(sr * 255.0 + 0.5), 0, 255);
            }
            uint32_t ua = (n >= 4) ? (uint32_t)ARCR_CLAMP((int)((a > 1.0 ? a / 100.0 : a) * 255.0 + 0.5), 0, 255) : 255u;
            *out_argb = (ua << 24) | (out_c[0] << 16) | (out_c[1] << 8) | out_c[2];
            return true;
        }
        return false;
    }

    static const struct { const char *name; uint32_t argb; } kNamed[] = {
        {"transparent", 0x00000000u}, {"black",         0xFF000000u},
        {"white",       0xFFFFFFFFu}, {"red",           0xFFFF0000u},
        {"green",       0xFF008000u}, {"lime",          0xFF00FF00u},
        {"blue",        0xFF0000FFu}, {"yellow",        0xFFFFFF00u},
        {"cyan",        0xFF00FFFFu}, {"aqua",          0xFF00FFFFu},
        {"magenta",     0xFFFF00FFu}, {"fuchsia",       0xFFFF00FFu},
        {"gray",        0xFF808080u}, {"grey",          0xFF808080u},
        {"silver",      0xFFC0C0C0u}, {"maroon",        0xFF800000u},
        {"olive",       0xFF808000u}, {"purple",        0xFF800080u},
        {"teal",        0xFF008080u}, {"navy",          0xFF000080u},
        {"orange",      0xFFFFA500u}, {"pink",          0xFFFFC0CBu},
        {"gold",        0xFFFFD700u}, {"coral",         0xFFFF7F50u},
        {"tomato",      0xFFFF6347u}, {"crimson",       0xFFDC143Cu},
        {"dodgerblue",  0xFF1E90FFu}, {"royalblue",     0xFF4169E1u},
        {"skyblue",     0xFF87CEEBu}, {"rebeccapurple", 0xFF663399u},
        {"indigo",      0xFF4B0082u}, {"violet",        0xFFEE82EEu},
        {"brown",       0xFFA52A2Au}, {" currentcolor",  0xFF000000u}
    };
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kNamed); ++i) {
        if (arcr_str_eq_nocase_n(s, len, kNamed[i].name, strlen(kNamed[i].name))) {
            *out_argb = kNamed[i].argb;
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------------------
 * 5. Native DOM Bridge Functions (`AjsNativeFn`)
 * ------------------------------------------------------------------------- */
static AjsValue b_bind_node(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 2 || !ajs_is_object(avm, argv[0])) return ajs_undefined();
    uint32_t uid = 0u;
    (void)ajs_to_uint32(avm, argv[1], &uid);
    ajs_set_opaque(avm, argv[0], WEB_CLASS_DOM_NODE, (void *)(uintptr_t)uid, NULL, NULL);
    return argv[0];
}

static AjsValue b_node_uid_of(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 1 || !ajs_is_object(avm, argv[0])) return ajs_int32(-1);
    void *op = ajs_get_opaque(avm, argv[0], WEB_CLASS_DOM_NODE);
    if (!op) {
        AjsValue u = ajs_get_prop_str(avm, argv[0], "__uid");
        uint32_t fuid = 0u;
        if (ajs_is_number(u) && ajs_to_uint32(avm, u, &fuid)) return ajs_uint32(fuid);
        return ajs_int32(-1);
    }
    return ajs_uint32((uint32_t)(uintptr_t)op);
}

static AjsValue b_node_info(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || argc < 1) return ajs_null();
    uint32_t uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    ArcrDomNode *n = web_dom_by_uid(vm, uid);
    if (!n) return ajs_null();

    int32_t ntype = 1;
    const char *tname = "";
    bool is_shadow = false;
    if (n->kind == ARCR_NODE_DOCUMENT) {
        ntype = 9;
        tname = "#document";
    } else if (n->kind == ARCR_NODE_TEXT) {
        ntype = 3;
        tname = "#text";
    } else if (n->kind == ARCR_NODE_COMMENT) {
        ntype = 8;
        tname = "#comment";
    } else if (dom_node_is_shadow_root(vm, n)) {
        ntype = 11;
        tname = "#document-fragment";
        is_shadow = true;
    } else if (dom_node_is_fragment(vm, n)) {
        ntype = 11;
        tname = "#document-fragment";
    } else {
        ntype = 1;
        tname = arcr_atom_cstr(vm->host.doc->atoms, n->tag);
        if (!tname) tname = "div";
    }

    AjsValue o = ajs_new_object(avm);
    (void)ajs_set_prop_str(avm, o, "uid", ajs_uint32(n->node_uid));
    (void)ajs_set_prop_str(avm, o, "nodeType", ajs_int32(ntype));
    (void)ajs_set_prop_str(avm, o, "tag", ajs_string_cstr(avm, tname));
    (void)ajs_set_prop_str(avm, o, "isShadow", ajs_bool(is_shadow));
    (void)ajs_set_prop_str(avm, o, "connected", ajs_bool(dom_node_is_connected(vm, n)));
    return o;
}

static ArcrDomNode *dom_find_first_tag_rec(const ArcrDocument *doc, ArcrDomNode *n, const char *tag) {
    if (!doc || !n) return NULL;
    if (n->kind == ARCR_NODE_ELEMENT &&
        arcr_str_eq_nocase(arcr_atom_cstr(doc->atoms, n->tag), tag)) {
        return n;
    }
    for (ArcrDomNode *c = n->first_child; c != NULL; c = c->next_sibling) {
        ArcrDomNode *f = dom_find_first_tag_rec(doc, c, tag);
        if (f) return f;
    }
    return NULL;
}

static AjsValue b_doc_special_uids(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)argc; (void)argv;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || !vm->host.doc->root) return ajs_null();
    web_uid_sync_document(vm);
    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *html = dom_find_first_tag_rec(doc, doc->root, "html");
    ArcrDomNode *head = dom_find_first_tag_rec(doc, doc->root, "head");
    ArcrDomNode *body = doc->body ? doc->body : dom_find_first_tag_rec(doc, doc->root, "body");
    ArcrDomNode *act  = doc->focused_node ? doc->focused_node : body;

    AjsValue o = ajs_new_object(avm);
    (void)ajs_set_prop_str(avm, o, "doc",    ajs_uint32(doc->root->node_uid));
    (void)ajs_set_prop_str(avm, o, "html",   ajs_uint32(html ? html->node_uid : 0u));
    (void)ajs_set_prop_str(avm, o, "head",   ajs_uint32(head ? head->node_uid : 0u));
    (void)ajs_set_prop_str(avm, o, "body",   ajs_uint32(body ? body->node_uid : 0u));
    (void)ajs_set_prop_str(avm, o, "active", ajs_uint32(act ? act->node_uid : 0u));
    (void)ajs_set_prop_str(avm, o, "compatMode",
                           ajs_string_cstr(avm, doc->quirks_mode == ARCR_QUIRKS_FULL_QUIRKS ? "BackCompat" : "CSS1Compat"));
    return o;
}

static AjsValue b_get_title(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)argc; (void)argv;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    const char *t = (vm && vm->host.doc && vm->host.doc->title) ? vm->host.doc->title : "";
    return ajs_string_cstr(avm, t);
}

static AjsValue b_set_title(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 1) return ajs_undefined();
    size_t len = 0u;
    char *s = ajs_to_cstring_len(avm, argv[0], &len);
    if (!s) return ajs_undefined();
    if (vm->host.doc) {
        vm->host.doc->title = arcr_arena_strdup(&vm->host.doc->arena, s, len);
        ArcrDomNode *tnode = dom_find_first_tag_rec(vm->host.doc, vm->host.doc->root, "title");
        if (tnode) {
            tnode->first_child = NULL;
            tnode->last_child  = NULL;
            ArcrDomNode *tx = dom_create_text(vm, s, len);
            if (tx) dom_insert_before_raw(vm, tnode, tx, NULL);
        }
    }
    if (vm->host.set_document_title) {
        uint64_t t0 = arcr_platform_monotonic_usec();
        web_host_call_enter(vm);
        vm->host.set_document_title(vm->host.userdata, s);
        web_host_call_leave(vm, t0);
    }
    ajs_free_cstring(avm, s);
    return ajs_undefined();
}

static ArcrDomNode *dom_find_by_id_rec(ArcrJsVm *vm, ArcrDomNode *n, ArcrAtom id_atom) {
    if (!vm || !n) return NULL;
    if (n->kind == ARCR_NODE_ELEMENT && !dom_node_is_frag_or_shadow(vm, n) && n->id == id_atom) {
        return n;
    }
    for (ArcrDomNode *c = n->first_child; c != NULL; c = c->next_sibling) {
        ArcrDomNode *f = dom_find_by_id_rec(vm, c, id_atom);
        if (f) return f;
    }
    return NULL;
}

static AjsValue b_get_by_id(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || !vm->host.doc->root || argc < 1) return ajs_uint32(0u);
    uint32_t scope_uid = 0u;
    int id_arg = 0;
    if (argc >= 2) {
        (void)ajs_to_uint32(avm, argv[0], &scope_uid);
        id_arg = 1;
    }
    size_t len = 0u;
    char *s = ajs_to_cstring_len(avm, argv[id_arg], &len);
    if (!s) return ajs_uint32(0u);
    uint32_t res_uid = 0u;
    if (len > 0u) {
        ArcrDomNode *scope = scope_uid ? web_dom_by_uid(vm, scope_uid) : vm->host.doc->root;
        if (scope) {
            ArcrAtom ia = arcr_atom_intern(vm->host.doc->atoms, s, len);
            ArcrDomNode *f = dom_find_by_id_rec(vm, scope, ia);
            if (f) {
                web_uid_register(vm, f);
                res_uid = f->node_uid;
            }
        }
    }
    ajs_free_cstring(avm, s);
    return ajs_uint32(res_uid);
}

static bool dom_selector_is_valid(const char *sel, size_t len) {
    arcr_str_trim_view(&sel, &len);
    if (len == 0u) return false;
    int brk = 0, par = 0;
    for (size_t i = 0u; i < len; ++i) {
        char c = sel[i];
        if (c == '[') ++brk;
        else if (c == ']') { if (--brk < 0) return false; }
        else if (c == '(') ++par;
        else if (c == ')') { if (--par < 0) return false; }
    }
    if (brk != 0 || par != 0) return false;
    char last = sel[len - 1u];
    if (last == ',' || last == '>' || last == '+' || last == '~') return false;
    return true;
}

static void dom_query_sel_rec(ArcrJsVm *vm, ArcrDomNode *n, bool skip_self,
                              const char *sel, size_t sel_len, bool first_only,
                              AjsVm *avm, AjsValue arr, uint32_t *io_cnt) {
    if (!vm || !n || (first_only && *io_cnt > 0u)) return;
    if (!skip_self && n->kind == ARCR_NODE_ELEMENT && !dom_node_is_frag_or_shadow(vm, n)) {
        if (dom_match_selector_list(vm, vm->host.doc, n, sel, sel_len)) {
            web_uid_register(vm, n);
            (void)ajs_set_prop_index(avm, arr, (*io_cnt)++, ajs_uint32(n->node_uid));
            if (first_only) return;
        }
    }
    /* Scoped to current tree: never traverse into child->shadow_root or child->template_content */
    for (ArcrDomNode *c = n->first_child; c != NULL; c = c->next_sibling) {
        dom_query_sel_rec(vm, c, false, sel, sel_len, first_only, avm, arr, io_cnt);
        if (first_only && *io_cnt > 0u) return;
    }
}

static AjsValue b_query_selector(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || argc < 3) return ajs_new_array(avm, 0u);
    uint32_t scope_uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &scope_uid);
    size_t slen = 0u;
    char *sel = ajs_to_cstring_len(avm, argv[1], &slen);
    bool first_only = ajs_to_bool(avm, argv[2]);
    if (!sel) return ajs_new_array(avm, 0u);
    if (!dom_selector_is_valid(sel, slen)) {
        ajs_free_cstring(avm, sel);
        return ajs_int32(-1); /* Signals SyntaxError DOMException in JS wrapper */
    }

    AjsValue arr = ajs_new_array(avm, 0u);
    ArcrDomNode *scope = web_dom_by_uid(vm, scope_uid);
    uint32_t cnt = 0u;
    if (scope) {
        dom_query_sel_rec(vm, scope, true, sel, slen, first_only, avm, arr, &cnt);
    }
    ajs_free_cstring(avm, sel);
    return arr;
}

static void dom_query_tag_or_cls_rec(ArcrJsVm *vm, ArcrDomNode *n, bool skip_self,
                                     bool is_tag, const char *q, size_t qlen, bool match_all_tags,
                                     AjsVm *avm, AjsValue arr, uint32_t *io_cnt) {
    if (!vm || !n) return;
    if (!skip_self && n->kind == ARCR_NODE_ELEMENT && !dom_node_is_frag_or_shadow(vm, n)) {
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
                    if (!dom_elem_has_class(n, ca)) { ok = false; break; }
                }
            }
            if (!any_tok) ok = false;
        }
        if (ok) {
            web_uid_register(vm, n);
            (void)ajs_set_prop_index(avm, arr, (*io_cnt)++, ajs_uint32(n->node_uid));
        }
    }
    for (ArcrDomNode *c = n->first_child; c != NULL; c = c->next_sibling) {
        dom_query_tag_or_cls_rec(vm, c, false, is_tag, q, qlen, match_all_tags, avm, arr, io_cnt);
    }
}

static AjsValue b_get_by_tag_or_cls(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    AjsValue arr = ajs_new_array(avm, 0u);
    if (!vm || !vm->host.doc || argc < 3) return arr;
    uint32_t scope_uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &scope_uid);
    bool is_tag = ajs_to_bool(avm, argv[1]);
    size_t qlen = 0u;
    char *q = ajs_to_cstring_len(avm, argv[2], &qlen);
    if (!q) return arr;

    ArcrDomNode *scope = web_dom_by_uid(vm, scope_uid);
    uint32_t cnt = 0u;
    bool all_tags = is_tag && qlen == 1u && q[0] == '*';
    if (scope) {
        dom_query_tag_or_cls_rec(vm, scope, true, is_tag, q, qlen, all_tags, avm, arr, &cnt);
    }
    ajs_free_cstring(avm, q);
    return arr;
}

static AjsValue b_matches_or_closest(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || argc < 3) return ajs_uint32(0u);
    uint32_t uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    size_t slen = 0u;
    char *sel = ajs_to_cstring_len(avm, argv[1], &slen);
    bool is_closest = ajs_to_bool(avm, argv[2]);
    if (!sel) return ajs_uint32(0u);
    if (!dom_selector_is_valid(sel, slen)) {
        ajs_free_cstring(avm, sel);
        return ajs_int32(-1);
    }

    ArcrDomNode *n = web_dom_by_uid(vm, uid);
    uint32_t hit_uid = 0u;
    for (ArcrDomNode *cur = n; cur != NULL; cur = is_closest ? cur->parent : NULL) {
        if (dom_node_is_frag_or_shadow(vm, cur)) break;
        if (cur->kind == ARCR_NODE_ELEMENT &&
            dom_match_selector_list(vm, vm->host.doc, cur, sel, slen)) {
            web_uid_register(vm, cur);
            hit_uid = cur->node_uid;
            break;
        }
        if (!is_closest) break;
    }
    ajs_free_cstring(avm, sel);
    return ajs_uint32(hit_uid);
}

static AjsValue b_contains(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 2) return ajs_bool(false);
    uint32_t a = 0u, b = 0u;
    (void)ajs_to_uint32(avm, argv[0], &a);
    (void)ajs_to_uint32(avm, argv[1], &b);
    ArcrDomNode *na = web_dom_by_uid(vm, a);
    ArcrDomNode *nb = web_dom_by_uid(vm, b);
    if (!na || !nb) return ajs_bool(false);
    return ajs_bool(dom_is_ancestor_or_self(na, nb));
}

static ArcrDomNode *dom_prev_sibling_safe(const ArcrDomNode *n) {
    if (!n || !n->parent) return NULL;
    if (n->prev_sibling) return n->prev_sibling;
    ArcrDomNode *prev = NULL;
    for (ArcrDomNode *c = n->parent->first_child; c != NULL && c != n; c = c->next_sibling) {
        prev = c;
    }
    return prev;
}

static AjsValue b_nav(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 2) return ajs_uint32(0u);
    uint32_t uid = 0u;
    int32_t op = 0;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    (void)ajs_to_int32(avm, argv[1], &op);
    ArcrDomNode *n = web_dom_by_uid(vm, uid);
    if (!n) return ajs_uint32(0u);

    ArcrDomNode *r = NULL;
    switch (op) {
        case 0: r = n->parent; break;
        case 1: r = (n->parent && n->parent->kind == ARCR_NODE_ELEMENT &&
                     !dom_node_is_frag_or_shadow(vm, n->parent)) ? n->parent : NULL; break;
        case 2: r = n->first_child; break;
        case 3: r = n->last_child; break;
        case 4:
            for (ArcrDomNode *c = n->first_child; c; c = c->next_sibling) {
                if (c->kind == ARCR_NODE_ELEMENT && !dom_node_is_frag_or_shadow(vm, c)) { r = c; break; }
            }
            break;
        case 5:
            for (ArcrDomNode *c = n->first_child; c; c = c->next_sibling) {
                if (c->kind == ARCR_NODE_ELEMENT && !dom_node_is_frag_or_shadow(vm, c)) r = c;
            }
            break;
        case 6: r = n->next_sibling; break;
        case 7: r = dom_prev_sibling_safe(n); break;
        case 8:
            for (ArcrDomNode *c = n->next_sibling; c; c = c->next_sibling) {
                if (c->kind == ARCR_NODE_ELEMENT && !dom_node_is_frag_or_shadow(vm, c)) { r = c; break; }
            }
            break;
        case 9:
            if (n->parent) {
                for (ArcrDomNode *c = n->parent->first_child; c && c != n; c = c->next_sibling) {
                    if (c->kind == ARCR_NODE_ELEMENT && !dom_node_is_frag_or_shadow(vm, c)) r = c;
                }
            }
            break;
        case 10: { /* getRootNode({composed}) */
            bool composed = (argc >= 3) ? ajs_to_bool(avm, argv[2]) : false;
            ArcrDomNode *cur = n;
            int guard = 0;
            while (cur && guard++ < 512) {
                if (cur->parent) {
                    cur = cur->parent;
                    continue;
                }
                if (composed && dom_node_is_shadow_root(vm, cur)) {
                    uint32_t huid = dom_find_shadow_host_uid(vm, cur->node_uid, NULL);
                    ArcrDomNode *host = huid ? web_dom_by_uid(vm, huid) : NULL;
                    if (host) { cur = host; continue; }
                }
                break;
            }
            r = cur;
            break;
        }
        default: break;
    }
    if (r) web_uid_register(vm, r);
    return ajs_uint32(r ? r->node_uid : 0u);
}

static AjsValue b_child_uids(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    AjsValue arr = ajs_new_array(avm, 0u);
    if (!vm || argc < 2) return arr;
    uint32_t uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    bool elems_only = ajs_to_bool(avm, argv[1]);
    ArcrDomNode *n = web_dom_by_uid(vm, uid);
    if (!n) return arr;

    uint32_t idx = 0u;
    for (ArcrDomNode *c = n->first_child; c != NULL; c = c->next_sibling) {
        if (elems_only && (c->kind != ARCR_NODE_ELEMENT || dom_node_is_frag_or_shadow(vm, c))) continue;
        web_uid_register(vm, c);
        (void)ajs_set_prop_index(avm, arr, idx++, ajs_uint32(c->node_uid));
    }
    return arr;
}

static AjsValue b_ancestor_chain(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    AjsValue arr = ajs_new_array(avm, 0u);
    if (!vm || argc < 1) return arr;
    uint32_t uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    ArcrDomNode *cur = web_dom_by_uid(vm, uid);
    uint32_t idx = 0u;
    int guard = 0;
    while (cur && guard++ < 512) {
        web_uid_register(vm, cur);
        (void)ajs_set_prop_index(avm, arr, idx++, ajs_uint32(cur->node_uid));
        if (cur->parent) {
            cur = cur->parent;
        } else if (dom_node_is_shadow_root(vm, cur)) {
            uint32_t huid = dom_find_shadow_host_uid(vm, cur->node_uid, NULL);
            cur = huid ? web_dom_by_uid(vm, huid) : NULL;
        } else {
            break;
        }
    }
    return arr;
}

static AjsValue b_create_node(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || argc < 2) return ajs_uint32(0u);
    int32_t kind = 0;
    (void)ajs_to_int32(avm, argv[0], &kind);
    if (kind == 11) {
        ArcrDomNode *f = dom_create_fragment(vm);
        return ajs_uint32(f ? f->node_uid : 0u);
    }
    size_t len = 0u;
    char *s = ajs_to_cstring_len(avm, argv[1], &len);
    if (!s) return ajs_uint32(0u);

    ArcrDomNode *n = NULL;
    if (kind == 1)      n = dom_create_element(vm, s, len);
    else if (kind == 3) n = dom_create_text(vm, s, len);
    else if (kind == 8) n = dom_create_comment(vm, s, len);
    ajs_free_cstring(avm, s);
    return ajs_uint32(n ? n->node_uid : 0u);
}

static AjsValue b_insert_before(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 3) return ajs_bool(false);
    uint32_t p_uid = 0u, c_uid = 0u, r_uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &p_uid);
    (void)ajs_to_uint32(avm, argv[1], &c_uid);
    (void)ajs_to_uint32(avm, argv[2], &r_uid);
    ArcrDomNode *p = web_dom_by_uid(vm, p_uid);
    ArcrDomNode *c = web_dom_by_uid(vm, c_uid);
    ArcrDomNode *r = r_uid ? web_dom_by_uid(vm, r_uid) : NULL;
    if (!p || !c || p == c || dom_is_ancestor_or_self(c, p)) return ajs_bool(false);
    if (r && r->parent != p) return ajs_bool(false);
    dom_insert_before_raw(vm, p, c, r);
    return ajs_bool(true);
}

static AjsValue b_remove_node(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || argc < 1) return ajs_undefined();
    uint32_t uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    ArcrDomNode *n = web_dom_by_uid(vm, uid);
    if (n && n != vm->host.doc->root && !dom_node_is_shadow_root(vm, n)) {
        dom_unlink(vm->host.doc, n);
    }
    return ajs_undefined();
}

static ArcrDomNode *dom_clone_node_rec(ArcrJsVm *vm, const ArcrDomNode *src, bool deep) {
    if (!vm || !vm->host.doc || !src) return NULL;
    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *dst = NULL;
    if (dom_node_is_frag_or_shadow(vm, src)) {
        dst = dom_create_fragment(vm);
    } else if (src->kind == ARCR_NODE_ELEMENT) {
        const char *tn = arcr_atom_cstr(doc->atoms, src->tag);
        dst = dom_create_element(vm, tn ? tn : "div", tn ? strlen(tn) : 3u);
        if (dst) {
            for (const ArcrAttr *a = src->first_attr; a != NULL; a = a->next) {
                const char *an = arcr_atom_cstr(doc->atoms, a->name);
                const char *av = a->value ? a->value : "";
                if (an) dom_set_attr(doc, dst, an, strlen(an), av, strlen(av));
            }
            if (src->input_value) dom_set_input_val(doc, dst, src->input_value);
            dst->state_flags = src->state_flags & (uint32_t)ARCR_STATE_CHECKED;
            if (deep && src->template_content && dst->template_content) {
                for (const ArcrDomNode *tc = src->template_content->first_child; tc != NULL; tc = tc->next_sibling) {
                    ArcrDomNode *tcc = dom_clone_node_rec(vm, tc, true);
                    if (tcc) dom_insert_before_raw(vm, dst->template_content, tcc, NULL);
                }
            }
        }
    } else if (src->kind == ARCR_NODE_TEXT) {
        dst = dom_create_text(vm, src->text, src->text_len);
    } else if (src->kind == ARCR_NODE_COMMENT) {
        dst = dom_create_comment(vm, src->text, src->text_len);
    }
    if (dst && deep) {
        for (const ArcrDomNode *c = src->first_child; c != NULL; c = c->next_sibling) {
            ArcrDomNode *cc = dom_clone_node_rec(vm, c, true);
            if (cc) dom_insert_before_raw(vm, dst, cc, NULL);
        }
    }
    return dst;
}

static AjsValue b_clone_node(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 2) return ajs_uint32(0u);
    uint32_t uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    bool deep = ajs_to_bool(avm, argv[1]);
    ArcrDomNode *src = web_dom_by_uid(vm, uid);
    ArcrDomNode *cl  = dom_clone_node_rec(vm, src, deep);
    return ajs_uint32(cl ? cl->node_uid : 0u);
}

static AjsValue b_template_content(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || argc < 1) return ajs_uint32(0u);
    uint32_t uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    ArcrDomNode *el = web_dom_by_uid(vm, uid);
    if (!el || el->kind != ARCR_NODE_ELEMENT) return ajs_uint32(0u);
    const char *tn = arcr_atom_cstr(vm->host.doc->atoms, el->tag);
    if (!tn || !arcr_str_eq_nocase(tn, "template")) return ajs_uint32(0u);
    if (!el->template_content) {
        el->template_content = dom_create_fragment(vm);
        /* Move any parser-placed direct children under template_content if present */
        ArcrDomNode *ch = el->first_child;
        while (ch) {
            ArcrDomNode *nxt = ch->next_sibling;
            dom_insert_before_raw(vm, el->template_content, ch, NULL);
            ch = nxt;
        }
    } else {
        dom_ensure_special_atoms(vm);
        el->template_content->tag = vm->frag_atom;
        dom_uid_index_tree_rec(vm, el->template_content);
    }
    return ajs_uint32(el->template_content ? el->template_content->node_uid : 0u);
}

static AjsValue b_attach_shadow(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || argc < 2) return ajs_int32(0);
    uint32_t host_uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &host_uid);
    bool is_open = ajs_to_bool(avm, argv[1]);
    ArcrDomNode *el = web_dom_by_uid(vm, host_uid);
    if (!el || el->kind != ARCR_NODE_ELEMENT || dom_node_is_frag_or_shadow(vm, el)) {
        return ajs_int32(-1); /* NotSupportedError */
    }
    if (el->shadow_root) {
        return ajs_int32(-2); /* NotSupportedError: shadow root already attached */
    }
    ArcrDomNode *sr = dom_create_shadow_root(vm);
    if (!sr) return ajs_int32(0);
    el->shadow_root = sr;
    el->shadow_open = is_open;
    dom_record_shadow_link(vm, sr->node_uid, el->node_uid, is_open);
    vm->host.doc->dom_dirty = true;
    return ajs_uint32(sr->node_uid);
}

static AjsValue b_shadow_info(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || argc < 1) return ajs_null();
    uint32_t uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    ArcrDomNode *n = web_dom_by_uid(vm, uid);
    if (!n) return ajs_null();

    AjsValue o = ajs_new_object(avm);
    if (dom_node_is_shadow_root(vm, n)) {
        bool is_open = false;
        uint32_t huid = dom_find_shadow_host_uid(vm, n->node_uid, &is_open);
        (void)ajs_set_prop_str(avm, o, "isShadowRoot", ajs_bool(true));
        (void)ajs_set_prop_str(avm, o, "hostUid", ajs_uint32(huid));
        (void)ajs_set_prop_str(avm, o, "isOpen", ajs_bool(is_open));
        (void)ajs_set_prop_str(avm, o, "shadowUid", ajs_uint32(n->node_uid));
    } else {
        uint32_t suid = 0u;
        if (n->shadow_root) {
            dom_ensure_special_atoms(vm);
            n->shadow_root->tag = vm->shadow_atom;
            dom_record_shadow_link(vm, n->shadow_root->node_uid, n->node_uid, n->shadow_open);
            dom_uid_index_tree_rec(vm, n->shadow_root);
            suid = n->shadow_root->node_uid;
        }
        (void)ajs_set_prop_str(avm, o, "isShadowRoot", ajs_bool(false));
        (void)ajs_set_prop_str(avm, o, "hostUid", ajs_uint32(0u));
        (void)ajs_set_prop_str(avm, o, "isOpen", ajs_bool(n->shadow_open));
        (void)ajs_set_prop_str(avm, o, "shadowUid", ajs_uint32(suid));
    }
    return o;
}

static AjsValue b_get_text(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 1) return ajs_string_cstr(avm, "");
    uint32_t uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    ArcrDomNode *n = web_dom_by_uid(vm, uid);
    if (!n) return ajs_string_cstr(avm, "");
    if (n->kind == ARCR_NODE_TEXT || n->kind == ARCR_NODE_COMMENT) {
        return ajs_string_utf8(avm, n->text ? n->text : "", n->text_len);
    }
    DomByteBuf b = {0};
    dom_collect_text_rec(n, &b);
    AjsValue ret = ajs_string_utf8(avm, b.data ? b.data : "", b.len);
    free(b.data);
    return ret;
}

static void dom_detach_children_only(ArcrDocument *doc, ArcrDomNode *el) {
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

static AjsValue b_set_text(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || argc < 2) return ajs_undefined();
    uint32_t uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    size_t len = 0u;
    char *s = ajs_to_cstring_len(avm, argv[1], &len);
    if (!s) return ajs_undefined();

    ArcrDocument *doc = vm->host.doc;
    ArcrDomNode *n = web_dom_by_uid(vm, uid);
    if (n) {
        if (n->kind == ARCR_NODE_TEXT || n->kind == ARCR_NODE_COMMENT) {
            n->text     = arcr_arena_strdup(&doc->arena, s, len);
            n->text_len = len;
            doc->dom_dirty = true;
        } else {
            dom_detach_children_only(doc, n);
            n->text     = NULL;
            n->text_len = 0u;
            if (len > 0u) {
                ArcrDomNode *tn = dom_create_text(vm, s, len);
                if (tn) dom_insert_before_raw(vm, n, tn, NULL);
            }
            if (n->kind == ARCR_NODE_ELEMENT && !dom_node_is_frag_or_shadow(vm, n)) {
                const char *tname = arcr_atom_cstr(doc->atoms, n->tag);
                if (arcr_str_eq_nocase(tname, "title") && dom_node_is_connected(vm, n)) {
                    doc->title = arcr_arena_strdup(&doc->arena, s, len);
                    if (vm->host.set_document_title) {
                        uint64_t t0 = arcr_platform_monotonic_usec();
                        web_host_call_enter(vm);
                        vm->host.set_document_title(vm->host.userdata, s);
                        web_host_call_leave(vm, t0);
                    }
                } else if (arcr_str_eq_nocase(tname, "textarea")) {
                    dom_set_input_val(doc, n, s);
                } else if (arcr_str_eq_nocase(tname, "style") && dom_node_is_connected(vm, n)) {
                    dom_append_style_block(doc, s, len);
                }
            }
        }
    }
    ajs_free_cstring(avm, s);
    return ajs_undefined();
}

static AjsValue b_get_html(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || argc < 2) return ajs_string_cstr(avm, "");
    uint32_t uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    bool outer = ajs_to_bool(avm, argv[1]);
    ArcrDomNode *n = web_dom_by_uid(vm, uid);
    if (!n) return ajs_string_cstr(avm, "");

    DomByteBuf b = {0};
    dom_serialize_node_rec(vm, vm->host.doc, n, outer, &b);
    AjsValue ret = ajs_string_utf8(avm, b.data ? b.data : "", b.len);
    free(b.data);
    return ret;
}

static AjsValue b_set_inner_html(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || argc < 2) return ajs_undefined();
    uint32_t uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    size_t len = 0u;
    char *html = ajs_to_cstring_len(avm, argv[1], &len);
    if (!html) return ajs_undefined();

    ArcrDomNode *n = web_dom_by_uid(vm, uid);
    if (n && (n->kind == ARCR_NODE_ELEMENT || n->kind == ARCR_NODE_DOCUMENT)) {
        ArcrDomNode *target = n;
        if (n->kind == ARCR_NODE_ELEMENT && !dom_node_is_frag_or_shadow(vm, n) &&
            arcr_str_eq_nocase(arcr_atom_cstr(vm->host.doc->atoms, n->tag), "template")) {
            if (!n->template_content) n->template_content = dom_create_fragment(vm);
            if (n->template_content) target = n->template_content;
        }
        dom_detach_children_only(vm->host.doc, target);
        target->text     = NULL;
        target->text_len = 0u;
        if (len > 0u) {
            if (target->kind == ARCR_NODE_ELEMENT && !dom_node_is_frag_or_shadow(vm, target) &&
                arcr_str_eq_nocase(arcr_atom_cstr(vm->host.doc->atoms, target->tag), "style")) {
                target->text     = arcr_arena_strdup(&vm->host.doc->arena, html, len);
                target->text_len = len;
                if (dom_node_is_connected(vm, target)) {
                    dom_append_style_block(vm->host.doc, html, len);
                }
            } else {
                dom_parse_html_fragment(vm, target, html, len);
            }
        }
        vm->host.doc->dom_dirty = true;
    }
    ajs_free_cstring(avm, html);
    return ajs_undefined();
}

static AjsValue b_attr_op(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || argc < 2) return ajs_null();
    uint32_t uid = 0u;
    int32_t op = 0;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    (void)ajs_to_int32(avm, argv[1], &op);
    ArcrDomNode *el = web_dom_by_uid(vm, uid);
    if (!el || el->kind != ARCR_NODE_ELEMENT || dom_node_is_frag_or_shadow(vm, el)) {
        return (op == 3) ? ajs_new_array(avm, 0u) : ((op == 2) ? ajs_bool(false) : ajs_null());
    }

    if (op == 3) {
        AjsValue arr = ajs_new_array(avm, 0u);
        uint32_t idx = 0u;
        for (const ArcrAttr *a = el->first_attr; a != NULL; a = a->next) {
            const char *an = arcr_atom_cstr(vm->host.doc->atoms, a->name);
            if (an) (void)ajs_set_prop_index(avm, arr, idx++, ajs_string_cstr(avm, an));
        }
        return arr;
    }

    if (argc < 3) return ajs_null();
    size_t nlen = 0u;
    char *name = ajs_to_cstring_len(avm, argv[2], &nlen);
    if (!name) return ajs_null();

    AjsValue ret = ajs_undefined();
    if (op == 0) {
        const char *v = dom_get_attr(vm->host.doc, el, name);
        ret = v ? ajs_string_cstr(avm, v) : ajs_null();
    } else if (op == 1 && argc >= 4) {
        size_t vlen = 0u;
        char *val = ajs_to_cstring_len(avm, argv[3], &vlen);
        if (val) {
            dom_set_attr(vm->host.doc, el, name, nlen, val, vlen);
            ajs_free_cstring(avm, val);
        }
    } else if (op == 2) {
        ret = ajs_bool(dom_get_attr(vm->host.doc, el, name) != NULL);
    } else if (op == 4) {
        dom_remove_attr(vm->host.doc, el, name);
    }
    ajs_free_cstring(avm, name);
    return ret;
}

static AjsValue b_form_state(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || argc < 2) return ajs_undefined();
    uint32_t uid = 0u;
    int32_t op = 0;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    (void)ajs_to_int32(avm, argv[1], &op);
    ArcrDomNode *el = web_dom_by_uid(vm, uid);
    if (!el || el->kind != ARCR_NODE_ELEMENT || dom_node_is_frag_or_shadow(vm, el)) {
        return (op == 0) ? ajs_string_cstr(avm, "") : ajs_bool(false);
    }

    if (op == 0) {
        if (el->input_value) return ajs_string_cstr(avm, el->input_value);
        const char *av = dom_get_attr(vm->host.doc, el, "value");
        return ajs_string_cstr(avm, av ? av : "");
    }
    if (op == 1 && argc >= 3) {
        char *s = ajs_to_cstring(avm, argv[2]);
        if (s) {
            dom_set_input_val(vm->host.doc, el, s);
            ajs_free_cstring(avm, s);
        }
        return ajs_undefined();
    }
    if (op == 2) {
        return ajs_bool((el->state_flags & (uint32_t)ARCR_STATE_CHECKED) != 0u);
    }
    if (op == 3 && argc >= 3) {
        if (ajs_to_bool(avm, argv[2])) el->state_flags |= (uint32_t)ARCR_STATE_CHECKED;
        else el->state_flags &= ~((uint32_t)ARCR_STATE_CHECKED);
        vm->host.doc->dom_dirty = true;
        return ajs_undefined();
    }
    if (op == 4 && argc >= 3) {
        bool want = ajs_to_bool(avm, argv[2]);
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
    return ajs_undefined();
}

/* -------------------------------------------------------------------------
 * 6. Live Layout Geometry, ComputedStyle, CSS.supports, History & Storage
 * ------------------------------------------------------------------------- */
static void dom_ensure_layout_fresh(ArcrJsVm *vm) {
    if (!vm || !vm->host.ensure_layout) return;
    uint64_t t0 = arcr_platform_monotonic_usec();
    web_host_call_enter(vm);
    vm->host.ensure_layout(vm->host.userdata);
    web_host_call_leave(vm, t0);
}

static void fmt_css_argb(uint32_t argb, char *out, size_t cap) {
    uint32_t a = (argb >> 24) & 0xFFu;
    uint32_t r = (argb >> 16) & 0xFFu;
    uint32_t g = (argb >> 8)  & 0xFFu;
    uint32_t b = argb         & 0xFFu;
    if (a == 255u) {
        snprintf(out, cap, "rgb(%u, %u, %u)", r, g, b);
    } else if (a == 0u) {
        snprintf(out, cap, "rgba(0, 0, 0, 0)");
    } else {
        snprintf(out, cap, "rgba(%u, %u, %u, %.3g)", r, g, b, (double)a / 255.0);
    }
}

static const char *fmt_display_mode(ArcrDisplayMode m) {
    switch (m) {
        case ARCR_DISPLAY_BLOCK:        return "block";
        case ARCR_DISPLAY_INLINE:       return "inline";
        case ARCR_DISPLAY_NONE:         return "none";
        case ARCR_DISPLAY_INLINE_BLOCK: return "inline-block";
        case ARCR_DISPLAY_FLEX:         return "flex";
        case ARCR_DISPLAY_GRID:         return "grid";
        case ARCR_DISPLAY_TABLE:        return "table";
        case ARCR_DISPLAY_TABLE_ROW:    return "table-row";
        case ARCR_DISPLAY_TABLE_CELL:   return "table-cell";
        case ARCR_DISPLAY_INLINE_FLEX:  return "inline-flex";
        case ARCR_DISPLAY_CONTENTS:     return "contents";
        default:                        return "block";
    }
}

static const char *fmt_pos_mode(ArcrPositionMode p) {
    switch (p) {
        case ARCR_POS_RELATIVE: return "relative";
        case ARCR_POS_ABSOLUTE: return "absolute";
        case ARCR_POS_FIXED:    return "fixed";
        case ARCR_POS_STICKY:   return "sticky";
        default:                return "static";
    }
}

static const char *fmt_overflow_mode(ArcrOverflowMode o) {
    switch (o) {
        case ARCR_OVERFLOW_HIDDEN: return "hidden";
        case ARCR_OVERFLOW_AUTO:   return "auto";
        case ARCR_OVERFLOW_SCROLL: return "scroll";
        default:                   return "visible";
    }
}

static AjsValue b_layout_box(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    AjsValue o = ajs_new_object(avm);
    if (!vm || !vm->host.doc || argc < 1) return o;

    dom_ensure_layout_fresh(vm);

    uint32_t uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    ArcrDomNode *el = web_dom_by_uid(vm, uid);
    int32_t vw = (vm->host.viewport_width > 0) ? vm->host.viewport_width : 1280;
    int32_t vh = (vm->host.viewport_height > 0) ? vm->host.viewport_height : 720;

    int32_t x = 0, y = 0, w = 0, h = 0;
    int32_t bt = 0, br = 0, bb = 0, bl = 0;
    int32_t pt = 0, pr = 0, pb = 0, pl = 0;
    bool has_box = false;
    uint32_t offset_parent_uid = 0u;
    const char *pos_s = "static";

    if (el) {
        if (el == vm->host.doc->root ||
            (el->kind == ARCR_NODE_ELEMENT &&
             arcr_str_eq_nocase(arcr_atom_cstr(vm->host.doc->atoms, el->tag), "html"))) {
            w = el->has_layout_box ? el->layout_w : vw;
            h = el->has_layout_box ? el->layout_h : vh;
            x = el->has_layout_box ? el->layout_x : 0;
            y = el->has_layout_box ? el->layout_y : 0;
            has_box = true;
        } else if (el->has_layout_box) {
            x = el->layout_x;
            y = el->layout_y;
            w = el->layout_w;
            h = el->layout_h;
            has_box = true;
        } else if (el->surface && el->surface->width > 0) {
            w = el->surface->width;
            h = el->surface->height;
            has_box = true;
        } else if (el->computed_style && el->computed_style->display != ARCR_DISPLAY_NONE) {
            const ArcrComputedStyle *cs = el->computed_style;
            if (cs->width_spec > 0) w = cs->width_spec;
            else if (cs->width_spec < 0) w = (vw * (-cs->width_spec)) / 100;
            if (cs->height_px > 0) h = cs->height_px;
            has_box = (w > 0 || h > 0);
        }

        if (el->computed_style) {
            const ArcrComputedStyle *cs = el->computed_style;
            bt = cs->border.top;    br = cs->border.right;
            bb = cs->border.bottom; bl = cs->border.left;
            pt = cs->padding.top;   pr = cs->padding.right;
            pb = cs->padding.bottom;pl = cs->padding.left;
            pos_s = fmt_pos_mode(cs->position);
            if (cs->display == ARCR_DISPLAY_NONE) {
                x = 0; y = 0; w = 0; h = 0;
                has_box = false;
            }
        }

        if (has_box && (!el->computed_style || el->computed_style->position != ARCR_POS_FIXED)) {
            for (ArcrDomNode *p = el->parent; p != NULL; p = p->parent) {
                if (dom_node_is_frag_or_shadow(vm, p) || p->kind != ARCR_NODE_ELEMENT) continue;
                const char *ptag = arcr_atom_cstr(vm->host.doc->atoms, p->tag);
                if ((p->computed_style && p->computed_style->position != ARCR_POS_STATIC) ||
                    arcr_str_eq_nocase(ptag, "body") ||
                    arcr_str_eq_nocase(ptag, "td") ||
                    arcr_str_eq_nocase(ptag, "th") ||
                    arcr_str_eq_nocase(ptag, "table")) {
                    web_uid_register(vm, p);
                    offset_parent_uid = p->node_uid;
                    break;
                }
            }
        }
    }

    int32_t off_left = x, off_top = y;
    if (offset_parent_uid != 0u) {
        ArcrDomNode *op = web_dom_by_uid(vm, offset_parent_uid);
        if (op && op->has_layout_box) {
            int32_t op_bl = op->computed_style ? op->computed_style->border.left : 0;
            int32_t op_bt = op->computed_style ? op->computed_style->border.top  : 0;
            off_left = x - (op->layout_x + op_bl);
            off_top  = y - (op->layout_y + op_bt);
        }
    }

    (void)ajs_set_prop_str(avm, o, "x", ajs_int32(x));
    (void)ajs_set_prop_str(avm, o, "y", ajs_int32(y));
    (void)ajs_set_prop_str(avm, o, "width", ajs_int32(w));
    (void)ajs_set_prop_str(avm, o, "height", ajs_int32(h));
    (void)ajs_set_prop_str(avm, o, "hasBox", ajs_bool(has_box));
    (void)ajs_set_prop_str(avm, o, "borderTop", ajs_int32(bt));
    (void)ajs_set_prop_str(avm, o, "borderRight", ajs_int32(br));
    (void)ajs_set_prop_str(avm, o, "borderBottom", ajs_int32(bb));
    (void)ajs_set_prop_str(avm, o, "borderLeft", ajs_int32(bl));
    (void)ajs_set_prop_str(avm, o, "padTop", ajs_int32(pt));
    (void)ajs_set_prop_str(avm, o, "padRight", ajs_int32(pr));
    (void)ajs_set_prop_str(avm, o, "padBottom", ajs_int32(pb));
    (void)ajs_set_prop_str(avm, o, "padLeft", ajs_int32(pl));
    (void)ajs_set_prop_str(avm, o, "offsetLeft", ajs_int32(off_left));
    (void)ajs_set_prop_str(avm, o, "offsetTop", ajs_int32(off_top));
    (void)ajs_set_prop_str(avm, o, "offsetParentUid", ajs_uint32(offset_parent_uid));
    (void)ajs_set_prop_str(avm, o, "position", ajs_string_cstr(avm, pos_s));
    return o;
}

static AjsValue b_computed_info(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    AjsValue o = ajs_new_object(avm);
    if (!vm || !vm->host.doc) return o;

    dom_ensure_layout_fresh(vm);

    int32_t vw = (vm->host.viewport_width > 0) ? vm->host.viewport_width : 1280;
    uint32_t uid = 0u;
    if (argc >= 1) (void)ajs_to_uint32(avm, argv[0], &uid);
    ArcrDomNode *el = web_dom_by_uid(vm, uid);

    int32_t w = vw, h = 24, fpx = 16;
    const char *disp = "block";
    const char *pos  = "static";
    const char *boxs = "content-box";
    const char *ovx  = "visible";
    const char *ovy  = "visible";
    const char *vis  = "visible";
    const char *pe   = "auto";
    const char *ta   = "left";
    const char *fdir = "row";
    const char *jc   = "flex-start";
    const char *ai   = "stretch";
    char color_s[40] = "rgb(0, 0, 0)";
    char bg_s[40]    = "rgba(0, 0, 0, 0)";
    char bcol_s[40]  = "rgb(0, 0, 0)";
    char xfm_s[96]   = "none";
    double opac      = 1.0;
    int32_t zidx     = 0;
    int32_t fw       = 400;
    int32_t lh_px    = 20;
    int32_t rad      = 0, gap = 0, rgap = 0, fgrow = 0, fshrink = 1;
    ArcrEdgeSizes mg = {0, 0, 0, 0}, pd = {0, 0, 0, 0}, bd = {0, 0, 0, 0};

    if (el && el->has_layout_box) {
        w = el->layout_w;
        h = el->layout_h;
    } else if (el && el->surface && el->surface->width > 0) {
        w = el->surface->width;
        h = el->surface->height;
    }

    if (el && el->computed_style) {
        const ArcrComputedStyle *cs = el->computed_style;
        if (!el->has_layout_box) {
            if (cs->width_spec > 0) w = cs->width_spec;
            else if (cs->width_spec < 0) w = (vw * (-cs->width_spec)) / 100;
            if (cs->height_px > 0) h = cs->height_px;
        }
        fpx  = cs->font_px > 0 ? cs->font_px : 16;
        fw   = cs->font_bold ? 700 : 400;
        lh_px = cs->line_height_px > 0
                    ? cs->line_height_px
                    : (cs->line_height_pct > 0 ? (fpx * cs->line_height_pct + 50) / 100 : (fpx * 120) / 100);
        opac = (double)cs->opacity / 255.0;
        zidx = cs->z_index;
        disp = fmt_display_mode(cs->display);
        if (cs->display == ARCR_DISPLAY_NONE) { w = 0; h = 0; }
        pos  = fmt_pos_mode(cs->position);
        boxs = (cs->box_sizing == ARCR_BOX_BORDER) ? "border-box" : "content-box";
        ovx  = fmt_overflow_mode(cs->overflow_x);
        ovy  = fmt_overflow_mode(cs->overflow_y);
        vis  = cs->visibility_hidden ? "hidden" : "visible";
        pe   = cs->pointer_events_none ? "none" : "auto";
        ta   = (cs->text_align == ARCR_TEXT_ALIGN_CENTER) ? "center"
             : (cs->text_align == ARCR_TEXT_ALIGN_RIGHT ? "right" : "left");
        fdir = (cs->flex_direction == ARCR_FLEX_COLUMN) ? "column"
             : (cs->flex_direction == ARCR_FLEX_ROW_REVERSE ? "row-reverse"
             : (cs->flex_direction == ARCR_FLEX_COLUMN_REVERSE ? "column-reverse" : "row"));
        switch (cs->justify_content) {
            case ARCR_JUSTIFY_CENTER:        jc = "center"; break;
            case ARCR_JUSTIFY_END:           jc = "flex-end"; break;
            case ARCR_JUSTIFY_SPACE_BETWEEN: jc = "space-between"; break;
            case ARCR_JUSTIFY_SPACE_AROUND:  jc = "space-around"; break;
            case ARCR_JUSTIFY_SPACE_EVENLY:  jc = "space-evenly"; break;
            default:                         jc = "flex-start"; break;
        }
        switch (cs->align_items) {
            case ARCR_ALIGN_START:  ai = "flex-start"; break;
            case ARCR_ALIGN_CENTER: ai = "center"; break;
            case ARCR_ALIGN_END:    ai = "flex-end"; break;
            default:                ai = "stretch"; break;
        }
        fmt_css_argb(cs->color_argb, color_s, sizeof(color_s));
        fmt_css_argb(cs->bg_argb, bg_s, sizeof(bg_s));
        fmt_css_argb(cs->border_argb, bcol_s, sizeof(bcol_s));
        rad     = cs->border_radius;
        gap     = cs->gap_px;
        rgap    = cs->row_gap_px;
        fgrow   = cs->flex_grow;
        fshrink = cs->flex_shrink;
        mg      = cs->margin;
        pd      = cs->padding;
        bd      = cs->border;

        if (cs->translate_x_px != 0 || cs->translate_y_px != 0 ||
            cs->translate_x_pct != 0 || cs->translate_y_pct != 0 ||
            (cs->scale_x_milli != 0 && cs->scale_x_milli != 1000) ||
            (cs->scale_y_milli != 0 && cs->scale_y_milli != 1000) ||
            cs->rotate_deg_milli != 0) {
            double sx = (cs->scale_x_milli == 0) ? 1.0 : ((double)cs->scale_x_milli * 0.001);
            double sy = (cs->scale_y_milli == 0) ? 1.0 : ((double)cs->scale_y_milli * 0.001);
            double rad_ang = ((double)cs->rotate_deg_milli * 0.001) * (3.14159265358979323846 / 180.0);
            double ca = cos(rad_ang), sa = sin(rad_ang);
            double tx = (double)cs->translate_x_px + ((double)w * (double)cs->translate_x_pct * 0.01);
            double ty = (double)cs->translate_y_px + ((double)h * (double)cs->translate_y_pct * 0.01);
            snprintf(xfm_s, sizeof(xfm_s), "matrix(%.4g, %.4g, %.4g, %.4g, %.4g, %.4g)",
                     ca * sx, sa * sx, -sa * sy, ca * sy, tx, ty);
        }
    }

    (void)ajs_set_prop_str(avm, o, "width", ajs_int32(w));
    (void)ajs_set_prop_str(avm, o, "height", ajs_int32(h));
    (void)ajs_set_prop_str(avm, o, "fontSize", ajs_int32(fpx));
    (void)ajs_set_prop_str(avm, o, "fontWeight", ajs_int32(fw));
    (void)ajs_set_prop_str(avm, o, "lineHeight", ajs_int32(lh_px));
    (void)ajs_set_prop_str(avm, o, "display", ajs_string_cstr(avm, disp));
    (void)ajs_set_prop_str(avm, o, "position", ajs_string_cstr(avm, pos));
    (void)ajs_set_prop_str(avm, o, "boxSizing", ajs_string_cstr(avm, boxs));
    (void)ajs_set_prop_str(avm, o, "overflowX", ajs_string_cstr(avm, ovx));
    (void)ajs_set_prop_str(avm, o, "overflowY", ajs_string_cstr(avm, ovy));
    (void)ajs_set_prop_str(avm, o, "visibility", ajs_string_cstr(avm, vis));
    (void)ajs_set_prop_str(avm, o, "pointerEvents", ajs_string_cstr(avm, pe));
    (void)ajs_set_prop_str(avm, o, "textAlign", ajs_string_cstr(avm, ta));
    (void)ajs_set_prop_str(avm, o, "flexDirection", ajs_string_cstr(avm, fdir));
    (void)ajs_set_prop_str(avm, o, "justifyContent", ajs_string_cstr(avm, jc));
    (void)ajs_set_prop_str(avm, o, "alignItems", ajs_string_cstr(avm, ai));
    (void)ajs_set_prop_str(avm, o, "flexWrap", ajs_string_cstr(avm, (el && el->computed_style && el->computed_style->flex_wrap) ? "wrap" : "nowrap"));
    (void)ajs_set_prop_str(avm, o, "flexGrow", ajs_int32(fgrow));
    (void)ajs_set_prop_str(avm, o, "flexShrink", ajs_int32(fshrink));
    (void)ajs_set_prop_str(avm, o, "gap", ajs_int32(gap));
    (void)ajs_set_prop_str(avm, o, "rowGap", ajs_int32(rgap));
    (void)ajs_set_prop_str(avm, o, "borderRadius", ajs_int32(rad));
    (void)ajs_set_prop_str(avm, o, "zIndex", ajs_int32(zidx));
    (void)ajs_set_prop_str(avm, o, "color", ajs_string_cstr(avm, color_s));
    (void)ajs_set_prop_str(avm, o, "backgroundColor", ajs_string_cstr(avm, bg_s));
    (void)ajs_set_prop_str(avm, o, "borderColor", ajs_string_cstr(avm, bcol_s));
    (void)ajs_set_prop_str(avm, o, "opacity", ajs_number(opac));
    (void)ajs_set_prop_str(avm, o, "transform", ajs_string_cstr(avm, xfm_s));
    (void)ajs_set_prop_str(avm, o, "marginTop", ajs_int32(mg.top));
    (void)ajs_set_prop_str(avm, o, "marginRight", ajs_int32(mg.right));
    (void)ajs_set_prop_str(avm, o, "marginBottom", ajs_int32(mg.bottom));
    (void)ajs_set_prop_str(avm, o, "marginLeft", ajs_int32(mg.left));
    (void)ajs_set_prop_str(avm, o, "paddingTop", ajs_int32(pd.top));
    (void)ajs_set_prop_str(avm, o, "paddingRight", ajs_int32(pd.right));
    (void)ajs_set_prop_str(avm, o, "paddingBottom", ajs_int32(pd.bottom));
    (void)ajs_set_prop_str(avm, o, "paddingLeft", ajs_int32(pd.left));
    (void)ajs_set_prop_str(avm, o, "borderTopWidth", ajs_int32(bd.top));
    (void)ajs_set_prop_str(avm, o, "borderRightWidth", ajs_int32(bd.right));
    (void)ajs_set_prop_str(avm, o, "borderBottomWidth", ajs_int32(bd.bottom));
    (void)ajs_set_prop_str(avm, o, "borderLeftWidth", ajs_int32(bd.left));
    return o;
}

static AjsValue b_custom_var(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.doc || argc < 2) return ajs_string_cstr(avm, "");
    dom_ensure_layout_fresh(vm);
    uint32_t uid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &uid);
    size_t nlen = 0u;
    char *vname = ajs_to_cstring_len(avm, argv[1], &nlen);
    if (!vname) return ajs_string_cstr(avm, "");
    ArcrDomNode *el = web_dom_by_uid(vm, uid);
    AjsValue ret = ajs_string_cstr(avm, "");
    if (el && nlen >= 2u && vname[0] == '-' && vname[1] == '-') {
        ArcrAtom va = arcr_atom_intern(vm->host.doc->atoms, vname, nlen);
        const WebCssCustomPropMapMirror *mp = (const WebCssCustomPropMapMirror *)el->custom_props;
        int guard = 0;
        while (mp && guard++ < 128) {
            for (const WebCssVarMirror *v = mp->vars_head; v != NULL; v = v->next) {
                if (v->name == va && v->val) {
                    const char *vp = v->val;
                    size_t vl = v->val_len;
                    arcr_str_trim_view(&vp, &vl);
                    ret = ajs_string_utf8(avm, vp, vl);
                    mp = NULL;
                    break;
                }
            }
            if (mp) mp = mp->parent;
        }
    }
    ajs_free_cstring(avm, vname);
    return ret;
}

static bool fallback_css_supports_decl(const char *prop, size_t plen, const char *val, size_t vlen) {
    arcr_str_trim_view(&prop, &plen);
    arcr_str_trim_view(&val, &vlen);
    if (plen == 0u || vlen == 0u) return false;
    if (plen >= 2u && prop[0] == '-' && prop[1] == '-') return true;
    static const char *const kProps[] = {
        "display", "position", "top", "right", "bottom", "left", "inset",
        "width", "min-width", "max-width", "height", "min-height", "max-height",
        "aspect-ratio", "box-sizing", "margin", "margin-top", "margin-right",
        "margin-bottom", "margin-left", "padding", "padding-top", "padding-right",
        "padding-bottom", "padding-left", "border", "border-width", "border-color",
        "border-style", "border-radius", "color", "background", "background-color",
        "background-image", "opacity", "z-index", "overflow", "overflow-x", "overflow-y",
        "visibility", "pointer-events", "font", "font-size", "font-weight", "font-family",
        "line-height", "text-align", "text-decoration", "text-transform", "white-space",
        "word-break", "flex", "flex-direction", "flex-wrap", "flex-grow", "flex-shrink",
        "flex-basis", "justify-content", "align-items", "align-self", "gap", "row-gap",
        "column-gap", "grid", "grid-template-columns", "grid-column", "transform",
        "transition", "animation", "box-shadow", "container-type", "container"
    };
    for (size_t i = 0u; i < ARCR_ARRAY_LEN(kProps); ++i) {
        if (arcr_str_eq_nocase_n(prop, plen, kProps[i], strlen(kProps[i]))) return true;
    }
    return false;
}

static AjsValue b_css_supports(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 1) return ajs_bool(false);

    if (argc >= 2 && !ajs_is_undefined(argv[1])) {
        size_t plen = 0u, vlen = 0u;
        char *prop = ajs_to_cstring_len(avm, argv[0], &plen);
        char *val  = ajs_to_cstring_len(avm, argv[1], &vlen);
        bool ok = false;
        if (prop && val) {
            if (vm->host.css_supports) {
                uint64_t t0 = arcr_platform_monotonic_usec();
                web_host_call_enter(vm);
                ok = vm->host.css_supports(vm->host.userdata, prop, val);
                web_host_call_leave(vm, t0);
            } else {
                ok = fallback_css_supports_decl(prop, plen, val, vlen);
            }
        }
        if (prop) ajs_free_cstring(avm, prop);
        if (val)  ajs_free_cstring(avm, val);
        return ajs_bool(ok);
    }

    size_t clen = 0u;
    char *cond = ajs_to_cstring_len(avm, argv[0], &clen);
    bool ok = false;
    if (cond) {
        if (vm->host.css_supports) {
            uint64_t t0 = arcr_platform_monotonic_usec();
            web_host_call_enter(vm);
            ok = vm->host.css_supports(vm->host.userdata, cond, NULL);
            web_host_call_leave(vm, t0);
        } else {
            const char *s = cond;
            size_t l = clen;
            arcr_str_trim_view(&s, &l);
            while (l >= 2u && s[0] == '(' && s[l - 1u] == ')') {
                s += 1u; l -= 2u;
                arcr_str_trim_view(&s, &l);
            }
            const char *colon = (const char *)memchr(s, ':', l);
            if (colon) {
                ok = fallback_css_supports_decl(s, (size_t)(colon - s),
                                                colon + 1, l - (size_t)(colon - s) - 1u);
            }
        }
        ajs_free_cstring(avm, cond);
    }
    return ajs_bool(ok);
}

static AjsValue b_history_op(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 1) return ajs_undefined();
    int32_t op = 0;
    (void)ajs_to_int32(avm, argv[0], &op);

    if (op == 0 || op == 1) { /* 0=pushState, 1=replaceState */
        char *state_json = (argc >= 2 && !ajs_is_nullish(argv[1])) ? ajs_to_cstring(avm, argv[1]) : NULL;
        char *new_url    = (argc >= 3 && !ajs_is_nullish(argv[2])) ? ajs_to_cstring(avm, argv[2]) : NULL;
        if (vm->host.history_push_or_replace) {
            uint64_t t0 = arcr_platform_monotonic_usec();
            web_host_call_enter(vm);
            vm->host.history_push_or_replace(vm->host.userdata,
                                             new_url ? new_url : "",
                                             state_json ? state_json : "null",
                                             op == 1);
            web_host_call_leave(vm, t0);
        }
        if (state_json) ajs_free_cstring(avm, state_json);
        if (new_url)    ajs_free_cstring(avm, new_url);
        return ajs_undefined();
    }
    if (op == 2) { /* go(delta) */
        int32_t delta = 0;
        if (argc >= 2) (void)ajs_to_int32(avm, argv[1], &delta);
        bool ok = false;
        if (vm->host.history_go) {
            uint64_t t0 = arcr_platform_monotonic_usec();
            web_host_call_enter(vm);
            ok = vm->host.history_go(vm->host.userdata, delta);
            web_host_call_leave(vm, t0);
        }
        return ajs_bool(ok);
    }
    if (op == 3) { /* length */
        size_t len = 1u;
        if (vm->host.history_length) {
            len = vm->host.history_length(vm->host.userdata);
            if (len == 0u) len = 1u;
        }
        return ajs_uint32((uint32_t)len);
    }
    return ajs_undefined();
}

static AjsValue b_doc_write(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 1) return ajs_bool(false);
    size_t len = 0u;
    char *html = ajs_to_cstring_len(avm, argv[0], &len);
    if (!html) return ajs_bool(false);
    bool ok = false;
    if (vm->host.document_write) {
        uint64_t t0 = arcr_platform_monotonic_usec();
        web_host_call_enter(vm);
        ok = vm->host.document_write(vm->host.userdata, html, len);
        web_host_call_leave(vm, t0);
        if (ok) web_uid_sync_document(vm);
    }
    if (!ok && vm->host.doc) {
        ArcrDomNode *body = vm->host.doc->body
                                ? vm->host.doc->body
                                : dom_find_first_tag_rec(vm->host.doc, vm->host.doc->root, "body");
        if (body && len > 0u) {
            dom_parse_html_fragment(vm, body, html, len);
            ok = true;
        }
    }
    ajs_free_cstring(avm, html);
    return ajs_bool(ok);
}

/* Host storage with private-mode isolation & transparent >450B chunking */
#define WEB_STORAGE_CHUNK_SIZE 440u

static AjsValue b_storage_op(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 2) return ajs_null();
    int32_t op = 0;
    (void)ajs_to_int32(avm, argv[0], &op);
    char *key = ajs_to_cstring(avm, argv[1]);
    if (!key) return ajs_null();

    if (vm->host.private_browsing) {
        AjsValue ret = ajs_undefined();
        if (op == 0) {
            char *v = web_priv_get_alloc(key);
            ret = v ? ajs_string_cstr(avm, v) : ajs_null();
            free(v);
        } else if (op == 1 && argc >= 3) {
            if (ajs_is_nullish(argv[2])) {
                web_priv_set(key, NULL);
            } else {
                char *val = ajs_to_cstring(avm, argv[2]);
                if (val) {
                    web_priv_set(key, val);
                    ajs_free_cstring(avm, val);
                }
            }
        }
        ajs_free_cstring(avm, key);
        return ret;
    }

    AjsValue ret = ajs_undefined();
    if (op == 0) {
        size_t cap = 262144u;
        char *buf = (char *)malloc(cap);
        if (!buf) {
            ajs_free_cstring(avm, key);
            return ajs_null();
        }
        buf[0] = '\0';
        bool found = vm->host.storage_get && vm->host.storage_get(vm->host.userdata, key, buf, cap);
        if (found) {
            size_t blen = strlen(buf);
            /* Check if legacy 512-byte slot truncated a chunked entry */
            if (blen == 511u) {
                char mkey[160];
                char mval[64];
                snprintf(mkey, sizeof(mkey), "__chk_n__:%s", key);
                if (vm->host.storage_get(vm->host.userdata, mkey, mval, sizeof(mval))) {
                    int32_t nchunks = 0;
                    if (arcr_parse_i32(mval, strlen(mval), &nchunks) && nchunks > 0 && nchunks < 512) {
                        DomByteBuf full = {0};
                        bool all_ok = true;
                        for (int32_t ci = 0; ci < nchunks; ++ci) {
                            char ckey[160];
                            char cbuf[512];
                            snprintf(ckey, sizeof(ckey), "__chk_%d__:%s", (int)ci, key);
                            if (!vm->host.storage_get(vm->host.userdata, ckey, cbuf, sizeof(cbuf))) {
                                all_ok = false;
                                break;
                            }
                            dbuf_cstr(&full, cbuf);
                        }
                        if (all_ok && full.data) {
                            ret = ajs_string_utf8(avm, full.data, full.len);
                            free(full.data);
                            free(buf);
                            ajs_free_cstring(avm, key);
                            return ret;
                        }
                        free(full.data);
                    }
                }
            }
            ret = ajs_string_utf8(avm, buf, blen);
        } else {
            ret = ajs_null();
        }
        free(buf);
    } else if (op == 1 && argc >= 3) {
        if (ajs_is_nullish(argv[2])) {
            if (vm->host.storage_set) {
                vm->host.storage_set(vm->host.userdata, key, NULL);
                char mkey[160];
                snprintf(mkey, sizeof(mkey), "__chk_n__:%s", key);
                vm->host.storage_set(vm->host.userdata, mkey, NULL);
            }
        } else {
            size_t vlen = 0u;
            char *val = ajs_to_cstring_len(avm, argv[2], &vlen);
            if (val && vm->host.storage_set) {
                vm->host.storage_set(vm->host.userdata, key, val);
                if (vlen >= 500u && strlen(key) < 110u) {
                    size_t nchunks = (vlen + WEB_STORAGE_CHUNK_SIZE - 1u) / WEB_STORAGE_CHUNK_SIZE;
                    if (nchunks < 60u) {
                        char mkey[160], mval[32];
                        snprintf(mkey, sizeof(mkey), "__chk_n__:%s", key);
                        snprintf(mval, sizeof(mval), "%u", (unsigned)nchunks);
                        vm->host.storage_set(vm->host.userdata, mkey, mval);
                        for (size_t ci = 0u; ci < nchunks; ++ci) {
                            char ckey[160], cslice[WEB_STORAGE_CHUNK_SIZE + 1u];
                            size_t off = ci * WEB_STORAGE_CHUNK_SIZE;
                            size_t sl  = ARCR_MIN(WEB_STORAGE_CHUNK_SIZE, vlen - off);
                            memcpy(cslice, val + off, sl);
                            cslice[sl] = '\0';
                            snprintf(ckey, sizeof(ckey), "__chk_%u__:%s", (unsigned)ci, key);
                            vm->host.storage_set(vm->host.userdata, ckey, cslice);
                        }
                    }
                } else if (strlen(key) < 110u) {
                    char mkey[160];
                    snprintf(mkey, sizeof(mkey), "__chk_n__:%s", key);
                    vm->host.storage_set(vm->host.userdata, mkey, NULL);
                }
            }
            if (val) ajs_free_cstring(avm, val);
        }
    }
    ajs_free_cstring(avm, key);
    return ret;
}

static AjsValue b_console_log(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.console_log || argc < 1) return ajs_undefined();
    char *s = ajs_to_cstring(avm, argv[0]);
    if (s) {
        vm->host.console_log(vm->host.userdata, s);
        ajs_free_cstring(avm, s);
    }
    return ajs_undefined();
}

static AjsValue b_env_info(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)argc; (void)argv;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    AjsValue o = ajs_new_object(avm);
    const char *url = "about:blank";
    if (vm && vm->host.get_current_url) {
        const char *u = vm->host.get_current_url(vm->host.userdata);
        if (u && u[0] != '\0') url = u;
    }
    char base_url[2048];
    char origin[512];
    if (vm) web_effective_base_url(vm, base_url, sizeof(base_url));
    else arcr_str_copy(base_url, sizeof(base_url), url);
    web_compute_origin(url, origin, sizeof(origin));

    (void)ajs_set_prop_str(avm, o, "url", ajs_string_cstr(avm, url));
    (void)ajs_set_prop_str(avm, o, "baseUrl", ajs_string_cstr(avm, base_url));
    (void)ajs_set_prop_str(avm, o, "origin", ajs_string_cstr(avm, origin));
    (void)ajs_set_prop_str(avm, o, "width", ajs_int32((vm && vm->host.viewport_width > 0) ? vm->host.viewport_width : 1280));
    (void)ajs_set_prop_str(avm, o, "height", ajs_int32((vm && vm->host.viewport_height > 0) ? vm->host.viewport_height : 720));
    (void)ajs_set_prop_str(avm, o, "prefersDark", ajs_bool(vm ? vm->host.prefers_dark : false));
    (void)ajs_set_prop_str(avm, o, "privateBrowsing", ajs_bool(vm ? vm->host.private_browsing : false));
    uint64_t now_us = arcr_platform_monotonic_usec();
    double rel_ms = (vm && now_us >= vm->boot_usec) ? ((double)(now_us - vm->boot_usec) * 0.001) : 0.0;
    (void)ajs_set_prop_str(avm, o, "nowMs", ajs_number(rel_ms));
    (void)ajs_set_prop_str(avm, o, "timeOrigin", ajs_number((double)arcr_platform_wall_epoch_sec() * 1000.0 - rel_ms));
    return o;
}

static AjsValue b_navigate(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || !vm->host.navigate_to || argc < 1) return ajs_undefined();
    char *u = ajs_to_cstring(avm, argv[0]);
    if (u) {
        vm->host.navigate_to(vm->host.userdata, u);
        ajs_free_cstring(avm, u);
    }
    return ajs_undefined();
}

static AjsValue b_clipboard_op(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 1) return ajs_string_cstr(avm, "");
    bool is_write = ajs_to_bool(avm, argv[0]);
    if (is_write) {
        if (argc < 2) return ajs_bool(false);
        char *s = ajs_to_cstring(avm, argv[1]);
        bool ok = s ? arcr_platform_clipboard_set_utf8(s) : false;
        if (s) ajs_free_cstring(avm, s);
        return ajs_bool(ok);
    }
    char buf[16384];
    buf[0] = '\0';
    (void)arcr_platform_clipboard_get_utf8(buf, sizeof(buf));
    return ajs_string_cstr(avm, buf);
}

static AjsValue b_resolve_url(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (argc < 1) return ajs_null();
    char *rel = ajs_to_cstring(avm, argv[0]);
    char *base = (argc >= 2 && !ajs_is_nullish(argv[1])) ? ajs_to_cstring(avm, argv[1]) : NULL;
    char eff_base[2048];
    if (base && base[0] != '\0') {
        arcr_str_copy(eff_base, sizeof(eff_base), base);
    } else if (vm) {
        web_effective_base_url(vm, eff_base, sizeof(eff_base));
    } else {
        arcr_str_copy(eff_base, sizeof(eff_base), "about:blank");
    }
    char out[2048];
    bool ok = rel && web_resolve_url(eff_base, rel, out, sizeof(out));
    if (rel)  ajs_free_cstring(avm, rel);
    if (base) ajs_free_cstring(avm, base);
    return ok ? ajs_string_cstr(avm, out) : ajs_null();
}

static AjsValue b_compute_origin(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 1) return ajs_string_cstr(avm, "null");
    char *u = ajs_to_cstring(avm, argv[0]);
    char orig[512];
    web_compute_origin(u ? u : "", orig, sizeof(orig));
    if (u) ajs_free_cstring(avm, u);
    return ajs_string_cstr(avm, orig);
}

void web_dom_register_bridge(ArcrJsVm *vm, AjsValue b) {
    if (!vm || !vm->avm) return;
    AjsVm *avm = vm->avm;
    web_bind_fn(avm, b, "bindNode",         b_bind_node,          2, vm);
    web_bind_fn(avm, b, "nodeUidOf",        b_node_uid_of,        1, vm);
    web_bind_fn(avm, b, "nodeInfo",         b_node_info,          1, vm);
    web_bind_fn(avm, b, "docSpecialUids",   b_doc_special_uids,   0, vm);
    web_bind_fn(avm, b, "getTitle",         b_get_title,          0, vm);
    web_bind_fn(avm, b, "setTitle",         b_set_title,          1, vm);
    web_bind_fn(avm, b, "getById",          b_get_by_id,          2, vm);
    web_bind_fn(avm, b, "querySelector",    b_query_selector,     3, vm);
    web_bind_fn(avm, b, "getByTagOrCls",    b_get_by_tag_or_cls,  3, vm);
    web_bind_fn(avm, b, "matchesOrClosest", b_matches_or_closest, 3, vm);
    web_bind_fn(avm, b, "contains",         b_contains,           2, vm);
    web_bind_fn(avm, b, "nav",              b_nav,                3, vm);
    web_bind_fn(avm, b, "childUids",        b_child_uids,         2, vm);
    web_bind_fn(avm, b, "ancestorChain",    b_ancestor_chain,     1, vm);
    web_bind_fn(avm, b, "createNode",       b_create_node,        2, vm);
    web_bind_fn(avm, b, "insertBefore",     b_insert_before,      3, vm);
    web_bind_fn(avm, b, "removeNode",       b_remove_node,        1, vm);
    web_bind_fn(avm, b, "cloneNode",        b_clone_node,         2, vm);
    web_bind_fn(avm, b, "templateContent",  b_template_content,   1, vm);
    web_bind_fn(avm, b, "attachShadow",     b_attach_shadow,      2, vm);
    web_bind_fn(avm, b, "shadowInfo",       b_shadow_info,        1, vm);
    web_bind_fn(avm, b, "getText",          b_get_text,           1, vm);
    web_bind_fn(avm, b, "setText",          b_set_text,           2, vm);
    web_bind_fn(avm, b, "getHtml",          b_get_html,           2, vm);
    web_bind_fn(avm, b, "setInnerHtml",     b_set_inner_html,     2, vm);
    web_bind_fn(avm, b, "attrOp",           b_attr_op,            4, vm);
    web_bind_fn(avm, b, "formState",        b_form_state,         3, vm);
    web_bind_fn(avm, b, "layoutBox",        b_layout_box,         1, vm);
    web_bind_fn(avm, b, "computedInfo",     b_computed_info,      1, vm);
    web_bind_fn(avm, b, "customVar",        b_custom_var,         2, vm);
    web_bind_fn(avm, b, "cssSupports",      b_css_supports,       2, vm);
    web_bind_fn(avm, b, "historyOp",        b_history_op,         3, vm);
    web_bind_fn(avm, b, "docWrite",         b_doc_write,          1, vm);
    web_bind_fn(avm, b, "storageOp",        b_storage_op,         3, vm);
    web_bind_fn(avm, b, "consoleLog",       b_console_log,        1, vm);
    web_bind_fn(avm, b, "envInfo",          b_env_info,           0, vm);
    web_bind_fn(avm, b, "navigate",         b_navigate,           1, vm);
    web_bind_fn(avm, b, "clipboardOp",      b_clipboard_op,       2, vm);
    web_bind_fn(avm, b, "resolveUrl",       b_resolve_url,        2, vm);
    web_bind_fn(avm, b, "computeOrigin",    b_compute_origin,     1, vm);
}

static bool dom_surf_in_tree_rec(const ArcrDomNode *n, const ArcrDomSurface *s) {
    if (!n || !s) return false;
    if (n->surface == s) return true;
    if (n->template_content && dom_surf_in_tree_rec(n->template_content, s)) return true;
    if (n->shadow_root && dom_surf_in_tree_rec(n->shadow_root, s)) return true;
    for (const ArcrDomNode *c = n->first_child; c != NULL; c = c->next_sibling) {
        if (dom_surf_in_tree_rec(c, s)) return true;
    }
    return false;
}

void web_dom_cleanup(ArcrJsVm *vm) {
    if (!vm) return;
    web_private_session_release(vm);

    DetachedSurfTrack *st = vm->surf_track_head;
    vm->surf_track_head = NULL;
    while (st) {
        DetachedSurfTrack *nxt = st->next;
        ArcrDomSurface *s = st->surf;
        if (s && s->heap_owned && s->argb_pixels) {
            bool attached = vm->host.doc && vm->host.doc->root &&
                            dom_surf_in_tree_rec(vm->host.doc->root, s);
            if (!attached) {
                free(s->argb_pixels);
                s->argb_pixels = NULL;
                s->heap_owned  = false;
            }
        }
        free(st);
        st = nxt;
    }

    free(vm->uid_table);
    vm->uid_table = NULL;
    vm->uid_cap   = 0u;
    free(vm->shadow_links);
    vm->shadow_links      = NULL;
    vm->shadow_link_count = 0u;
    vm->shadow_link_cap   = 0u;
}
