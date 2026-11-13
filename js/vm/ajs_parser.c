/*
 * ArcradientJS 1.0 - js/vm/ajs_parser.c
 * Clean-room Pure C11 ECMAScript 2026 Parser (Script & Module) with Full Early Error Validation
 */
#include "ajs_parse_internal.h"

typedef struct AjsDeclEntry {
    const uint16_t *u16;
    uint32_t        len;
    uint32_t        hash;
    int32_t         hash_next;
    bool            is_lex;     /* let / const / class / strict-block-fn */
    bool            is_param;
    bool            is_catch_simple;
} AjsDeclEntry;

typedef struct AjsParseScope {
    struct AjsParseScope *parent;
    bool                  is_func_root; /* Function, Module, or Script top scope */
    bool                  is_catch_scope;
    uint32_t              count;
    uint32_t              cap;
    AjsDeclEntry         *entries;
    uint32_t              hash_cap;
    int32_t              *hash_tab;
} AjsParseScope;

typedef struct AjsLabelEntry {
    const uint16_t *u16;
    uint32_t        len;
    bool            is_loop;
} AjsLabelEntry;

typedef struct AjsPrivDecl {
    const uint16_t *u16;
    uint32_t        len;
    uint8_t         mask; /* 1=other, 2=get, 4=set, 8=static */
} AjsPrivDecl;

typedef struct AjsPrivScope {
    struct AjsPrivScope *parent;
    uint32_t             count;
    uint32_t             cap;
    AjsPrivDecl         *decls;
    /* Deferred private identifier references inside class body validated once class finishes! */
    uint32_t             ref_count;
    uint32_t             ref_cap;
    AjsAstNode         **refs;
} AjsPrivScope;

typedef struct AjsParser {
    AjsVm         *vm;
    AjsAstArena   *arena;
    AjsLexer       lex;
    const char    *source_url;
    bool           is_module;
    bool           strict;
    bool           in_function;
    bool           in_generator;
    bool           in_async;
    bool           in_formal_params;
    bool           in_static_block;
    bool           allow_in;
    bool           allow_new_target;
    bool           allow_super_prop;
    bool           allow_super_call;
    bool           saw_top_await;
    int            loop_depth;
    int            switch_depth;
    int            label_chain_depth;

    AjsParseScope *scope;
    AjsPrivScope  *priv_scope;

    uint32_t       label_count;
    uint32_t       label_cap;
    AjsLabelEntry *labels;

    uint32_t       export_count;
    uint32_t       export_cap;
    AjsToken      *exported_names;

    bool           had_error;
    char           err_msg[512];
    uint32_t       err_line;
    uint32_t       err_col;
} AjsParser;

static bool u16_eq(const uint16_t *a, uint32_t alen, const uint16_t *b, uint32_t blen) {
    if (alen != blen) return false;
    return alen == 0 || memcmp(a, b, (size_t)alen * sizeof(uint16_t)) == 0;
}

static bool u16_eq_ascii(const uint16_t *a, uint32_t alen, const char *s) {
    size_t slen = strlen(s);
    if (alen != slen) return false;
    for (size_t i = 0; i < slen; i++) {
        if (a[i] != (uint8_t)s[i]) return false;
    }
    return true;
}

static void parse_error_at(AjsParser *p, uint32_t line, uint32_t col, const char *fmt, ...) {
    if (p->had_error) return;
    p->had_error = true;
    p->err_line = line ? line : 1;
    p->err_col = col ? col : 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err_msg, sizeof(p->err_msg), fmt, ap);
    va_end(ap);
}

static void parse_error_tok(AjsParser *p, AjsToken tok, const char *fmt, ...) {
    if (p->had_error) return;
    if (tok.kind == TOK_ERROR && p->lex.err_msg[0] != '\0') {
        parse_error_at(p, p->lex.err_line, p->lex.err_col, "%s", p->lex.err_msg);
        return;
    }
    p->had_error = true;
    p->err_line = tok.line ? tok.line : 1;
    p->err_col = tok.col ? tok.col : 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err_msg, sizeof(p->err_msg), fmt, ap);
    va_end(ap);
}

static AjsAstNode *ast_new(AjsParser *p, AjsAstKind kind, uint32_t line, uint32_t col) {
    AjsAstNode *n = (AjsAstNode *)ajs_arena_alloc(p->arena, sizeof(AjsAstNode));
    if (!n) {
        parse_error_at(p, line, col, "Out of parser memory");
        return NULL;
    }
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    n->line = line;
    n->col = col;
    return n;
}

static void nodelist_push(AjsParser *p, AjsNodeList *list, AjsAstNode *node) {
    if (list->count >= list->cap) {
        uint32_t ncap = list->cap ? list->cap * 2u : 8u;
        AjsAstNode **ni = (AjsAstNode **)ajs_arena_alloc(p->arena, ncap * sizeof(AjsAstNode *));
        if (!ni) return;
        ajs_memcpy(ni, list->items, list->count * sizeof(AjsAstNode *));
        list->items = ni;
        list->cap = ncap;
    }
    list->items[list->count++] = node;
}

static AjsToken p_advance(AjsParser *p) {
    AjsToken prev = p->lex.cur;
    if (prev.kind == TOK_ERROR) {
        parse_error_tok(p, prev, "%s", p->lex.err_msg);
        return prev;
    }
    AjsToken nxt = ajs_lexer_next(&p->lex);
    if (nxt.kind == TOK_ERROR) {
        parse_error_tok(p, nxt, "%s", p->lex.err_msg);
    }
    return prev;
}

static bool p_match(AjsParser *p, AjsTokenKind k) {
    if (p->lex.cur.kind == k) {
        p_advance(p);
        return true;
    }
    return false;
}

static bool p_expect(AjsParser *p, AjsTokenKind k, const char *desc) {
    if (p->lex.cur.kind == k) {
        p_advance(p);
        return true;
    }
    parse_error_tok(p, p->lex.cur, "Expected %s", desc);
    return false;
}

static bool p_consume_semicolon(AjsParser *p) {
    if (p->lex.cur.kind == TOK_SEMI) {
        p_advance(p);
        return true;
    }
    if (p->lex.cur.had_newline || p->lex.cur.kind == TOK_RBRACE || p->lex.cur.kind == TOK_EOF) {
        return true; /* Automatic Semicolon Insertion (ASI) */
    }
    parse_error_tok(p, p->lex.cur, "Unexpected token (missing ';')");
    return false;
}

/* ============================================================================
 * Scope & Early Redeclaration / Identifier Validation
 * ============================================================================ */

static void scope_push(AjsParser *p, bool is_func_root, bool is_catch) {
    AjsParseScope *sc = (AjsParseScope *)ajs_arena_alloc(p->arena, sizeof(AjsParseScope));
    memset(sc, 0, sizeof(*sc));
    sc->parent = p->scope;
    sc->is_func_root = is_func_root;
    sc->is_catch_scope = is_catch;
    p->scope = sc;
}

static void scope_pop(AjsParser *p) {
    if (p->scope) p->scope = p->scope->parent;
}

static void scope_rebuild_hash(AjsParser *p, AjsParseScope *sc, uint32_t ncap) {
    int32_t *nt = (int32_t *)ajs_arena_alloc(p->arena, ncap * sizeof(int32_t));
    for (uint32_t i = 0; i < ncap; i++) nt[i] = -1;
    uint32_t mask = ncap - 1u;
    for (uint32_t i = 0; i < sc->count; i++) {
        uint32_t b = sc->entries[i].hash & mask;
        sc->entries[i].hash_next = nt[b];
        nt[b] = (int32_t)i;
    }
    sc->hash_tab = nt;
    sc->hash_cap = ncap;
}

static int32_t scope_find_entry(const AjsParseScope *sc, const uint16_t *u16, uint32_t len, uint32_t h) {
    if (sc->hash_tab) {
        for (int32_t i = sc->hash_tab[h & (sc->hash_cap - 1u)]; i >= 0; i = sc->entries[i].hash_next) {
            if (sc->entries[i].hash == h && u16_eq(sc->entries[i].u16, sc->entries[i].len, u16, len)) {
                return i;
            }
        }
        return -1;
    }
    for (uint32_t i = 0; i < sc->count; i++) {
        if (sc->entries[i].hash == h && u16_eq(sc->entries[i].u16, sc->entries[i].len, u16, len)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t scope_find_lex_entry(const AjsParseScope *sc, const uint16_t *u16, uint32_t len, uint32_t h, bool ignore_catch_simple) {
    if (sc->hash_tab) {
        for (int32_t i = sc->hash_tab[h & (sc->hash_cap - 1u)]; i >= 0; i = sc->entries[i].hash_next) {
            if (sc->entries[i].is_lex && (!ignore_catch_simple || !sc->entries[i].is_catch_simple) &&
                sc->entries[i].hash == h && u16_eq(sc->entries[i].u16, sc->entries[i].len, u16, len)) {
                return i;
            }
        }
        return -1;
    }
    for (uint32_t i = 0; i < sc->count; i++) {
        if (sc->entries[i].is_lex && (!ignore_catch_simple || !sc->entries[i].is_catch_simple) &&
            sc->entries[i].hash == h && u16_eq(sc->entries[i].u16, sc->entries[i].len, u16, len)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void scope_add_raw(AjsParser *p, AjsParseScope *sc, const uint16_t *u16, uint32_t len,
                          bool is_lex, bool is_param, bool is_catch_simple) {
    if (sc->count >= sc->cap) {
        uint32_t ncap = sc->cap ? sc->cap * 2u : 8u;
        AjsDeclEntry *ne = (AjsDeclEntry *)ajs_arena_alloc(p->arena, ncap * sizeof(AjsDeclEntry));
        ajs_memcpy(ne, sc->entries, sc->count * sizeof(AjsDeclEntry));
        sc->entries = ne;
        sc->cap = ncap;
    }
    uint32_t idx = sc->count++;
    uint32_t h = ajs_hash_utf16(u16, len);
    AjsDeclEntry *e = &sc->entries[idx];
    e->u16 = u16;
    e->len = len;
    e->hash = h;
    e->hash_next = -1;
    e->is_lex = is_lex;
    e->is_param = is_param;
    e->is_catch_simple = is_catch_simple;
    if (sc->hash_tab) {
        if (sc->count > sc->hash_cap) {
            scope_rebuild_hash(p, sc, sc->hash_cap * 2u);
        } else {
            uint32_t b = h & (sc->hash_cap - 1u);
            e->hash_next = sc->hash_tab[b];
            sc->hash_tab[b] = (int32_t)idx;
        }
    } else if (sc->count >= 24u) {
        scope_rebuild_hash(p, sc, 64u);
    }
}

static void scope_declare_lex(AjsParser *p, const uint16_t *u16, uint32_t len, uint32_t line, uint32_t col) {
    if (u16_eq_ascii(u16, len, "let")) {
        parse_error_at(p, line, col, "'let' is disallowed as a lexically bound name");
        return;
    }
    AjsParseScope *sc = p->scope;
    uint32_t h = ajs_hash_utf16(u16, len);
    if (scope_find_entry(sc, u16, len, h) >= 0) {
        parse_error_at(p, line, col, "Identifier has already been declared");
        return;
    }
    scope_add_raw(p, sc, u16, len, true, false, false);
}

static void scope_declare_var(AjsParser *p, const uint16_t *u16, uint32_t len, uint32_t line, uint32_t col) {
    AjsParseScope *cur = p->scope;
    uint32_t h = ajs_hash_utf16(u16, len);
    while (cur) {
        if (scope_find_lex_entry(cur, u16, len, h, true) >= 0) {
            parse_error_at(p, line, col, "Identifier has already been lexically declared");
            return;
        }
        if (cur->is_func_root) {
            if (scope_find_entry(cur, u16, len, h) < 0) {
                scope_add_raw(p, cur, u16, len, false, false, false);
            }
            return;
        }
        cur = cur->parent;
    }
}

static bool is_strict_reserved_word(const uint16_t *u16, uint32_t len) {
    return u16_eq_ascii(u16, len, "implements") ||
           u16_eq_ascii(u16, len, "interface") ||
           u16_eq_ascii(u16, len, "let") ||
           u16_eq_ascii(u16, len, "package") ||
           u16_eq_ascii(u16, len, "private") ||
           u16_eq_ascii(u16, len, "protected") ||
           u16_eq_ascii(u16, len, "public") ||
           u16_eq_ascii(u16, len, "static") ||
           u16_eq_ascii(u16, len, "yield");
}

static bool check_binding_ident_tok(AjsParser *p, AjsToken tok, bool is_lhs_assign) {
    if (tok.kind == KW_YIELD) {
        if (p->strict || p->in_generator) {
            parse_error_tok(p, tok, "Unexpected 'yield' identifier");
            return false;
        }
        return true;
    }
    if (tok.kind == KW_AWAIT) {
        if (p->is_module || p->in_async || p->in_static_block) {
            parse_error_tok(p, tok, "Unexpected 'await' identifier");
            return false;
        }
        return true;
    }
    if (tok.kind == KW_LET || tok.kind == KW_STATIC) {
        if (p->strict) {
            parse_error_tok(p, tok, "Unexpected strict mode reserved word");
            return false;
        }
        return true;
    }
    if (tok.kind != TOK_IDENT) {
        parse_error_tok(p, tok, "Expected identifier");
        return false;
    }
    if (p->strict && is_strict_reserved_word(tok.u16_data, tok.u16_len)) {
        parse_error_tok(p, tok, "Unexpected strict mode reserved word");
        return false;
    }
    if (p->strict && (u16_eq_ascii(tok.u16_data, tok.u16_len, "eval") ||
                      u16_eq_ascii(tok.u16_data, tok.u16_len, "arguments"))) {
        parse_error_tok(p, tok, "Unexpected eval or arguments in strict mode");
        return false;
    }
    (void)is_lhs_assign;
    return true;
}

static bool is_ident_reference_tok(AjsParser *p, AjsToken tok) {
    if (tok.kind == TOK_IDENT) {
        if (p->strict && is_strict_reserved_word(tok.u16_data, tok.u16_len)) return false;
        return true;
    }
    if (tok.kind == KW_LET || tok.kind == KW_STATIC) return !p->strict;
    if (tok.kind == KW_YIELD) return !p->strict && !p->in_generator;
    if (tok.kind == KW_AWAIT) return !p->is_module && !p->in_async && !p->in_static_block;
    return false;
}

static bool is_property_name_word(AjsTokenKind k) {
    return k == TOK_IDENT || (k >= KW_AWAIT && k <= KW_YIELD);
}

static void record_priv_ref(AjsParser *p, AjsAstNode *priv_node) {
    if (!p->priv_scope) {
        parse_error_at(p, priv_node->line, priv_node->col, "Private field must be enclosed in an enclosing class");
        return;
    }
    AjsPrivScope *ps = p->priv_scope;
    if (ps->ref_count >= ps->ref_cap) {
        uint32_t ncap = ps->ref_cap ? ps->ref_cap * 2u : 8u;
        AjsAstNode **nr = (AjsAstNode **)ajs_arena_alloc(p->arena, ncap * sizeof(AjsAstNode *));
        ajs_memcpy(nr, ps->refs, ps->ref_count * sizeof(AjsAstNode *));
        ps->refs = nr;
        ps->ref_cap = ncap;
    }
    ps->refs[ps->ref_count++] = priv_node;
}

static void declare_pattern_names(AjsParser *p, AjsAstNode *pat, bool is_lex,
                                  bool check_dup_params, AjsNodeList *seen_params) {
    if (!pat || p->had_error) return;
    switch (pat->kind) {
        case AST_IDENT:
            if (p->strict && (u16_eq_ascii(pat->u.ident.u16, pat->u.ident.len, "eval") ||
                              u16_eq_ascii(pat->u.ident.u16, pat->u.ident.len, "arguments"))) {
                parse_error_at(p, pat->line, pat->col, "Unexpected eval or arguments in strict mode");
                return;
            }
            if (seen_params) {
                if (check_dup_params) {
                    for (uint32_t i = 0; i < seen_params->count; i++) {
                        AjsAstNode *prev = seen_params->items[i];
                        if (u16_eq(prev->u.ident.u16, prev->u.ident.len, pat->u.ident.u16, pat->u.ident.len)) {
                            parse_error_at(p, pat->line, pat->col, "Duplicate parameter name not allowed in this context");
                            return;
                        }
                    }
                }
                nodelist_push(p, seen_params, pat);
                scope_add_raw(p, p->scope, pat->u.ident.u16, pat->u.ident.len, false, true, false);
            } else if (is_lex) {
                scope_declare_lex(p, pat->u.ident.u16, pat->u.ident.len, pat->line, pat->col);
            } else {
                scope_declare_var(p, pat->u.ident.u16, pat->u.ident.len, pat->line, pat->col);
            }
            break;
        case AST_ASSIGN_PATTERN:
            declare_pattern_names(p, pat->u.assign_pat.left, is_lex, check_dup_params, seen_params);
            break;
        case AST_REST_ELEMENT:
            declare_pattern_names(p, pat->u.expr_stmt.expr, is_lex, check_dup_params, seen_params);
            break;
        case AST_ARRAY_PATTERN:
            for (uint32_t i = 0; i < pat->u.array.elems.count; i++) {
                if (pat->u.array.elems.items[i]) {
                    declare_pattern_names(p, pat->u.array.elems.items[i], is_lex, check_dup_params, seen_params);
                }
            }
            break;
        case AST_OBJECT_PATTERN:
            for (uint32_t i = 0; i < pat->u.object.props.count; i++) {
                AjsAstNode *pr = pat->u.object.props.items[i];
                if (pr->kind == AST_REST_ELEMENT) {
                    declare_pattern_names(p, pr, is_lex, check_dup_params, seen_params);
                } else {
                    declare_pattern_names(p, pr->u.prop.value, is_lex, check_dup_params, seen_params);
                }
            }
            break;
        default:
            break;
    }
}

/* Forward declarations for recursive descent */
static AjsAstNode *parse_expr(AjsParser *p);
static AjsAstNode *parse_assign_expr(AjsParser *p);
static AjsAstNode *parse_stmt(AjsParser *p, bool allow_decl);
static AjsAstNode *parse_binding_pattern_or_ident(AjsParser *p);
static AjsAstNode *parse_func(AjsParser *p, bool is_decl, bool is_async, AjsFuncKind base_kind);
static AjsAstNode *parse_class(AjsParser *p, bool is_decl);

/* ============================================================================
 * Destructuring Pattern Parsing & Expression-to-Pattern Conversion
 * ============================================================================ */

static AjsAstNode *make_ident_from_tok(AjsParser *p, AjsToken tok) {
    AjsAstNode *id = ast_new(p, AST_IDENT, tok.line, tok.col);
    if (!id) return NULL;
    id->u.ident.u16 = tok.u16_data;
    id->u.ident.len = tok.u16_len;
    id->u.ident.utf8 = tok.utf8_text;
    return id;
}

static AjsAstNode *parse_binding_elem(AjsParser *p) {
    AjsAstNode *pat = parse_binding_pattern_or_ident(p);
    if (p->had_error) return NULL;
    if (p_match(p, TOK_ASSIGN)) {
        AjsAstNode *ap = ast_new(p, AST_ASSIGN_PATTERN, pat->line, pat->col);
        ap->u.assign_pat.left = pat;
        ap->u.assign_pat.right = parse_assign_expr(p);
        return ap;
    }
    return pat;
}

static AjsAstNode *parse_binding_pattern_or_ident(AjsParser *p) {
    AjsToken cur = p->lex.cur;
    if (cur.kind == TOK_LBRACKET) {
        p_advance(p);
        AjsAstNode *arr = ast_new(p, AST_ARRAY_PATTERN, cur.line, cur.col);
        while (!p->had_error && p->lex.cur.kind != TOK_RBRACKET && p->lex.cur.kind != TOK_EOF) {
            if (p_match(p, TOK_COMMA)) {
                nodelist_push(p, &arr->u.array.elems, NULL);
                continue;
            }
            if (p_match(p, TOK_ELLIPSIS)) {
                AjsAstNode *rest = ast_new(p, AST_REST_ELEMENT, p->lex.cur.line, p->lex.cur.col);
                rest->u.expr_stmt.expr = parse_binding_pattern_or_ident(p);
                nodelist_push(p, &arr->u.array.elems, rest);
                if (p->lex.cur.kind == TOK_COMMA) {
                    parse_error_tok(p, p->lex.cur, "Rest element must be last element");
                }
                break;
            }
            nodelist_push(p, &arr->u.array.elems, parse_binding_elem(p));
            if (!p_match(p, TOK_COMMA)) break;
        }
        p_expect(p, TOK_RBRACKET, "']'");
        return arr;
    }
    if (cur.kind == TOK_LBRACE) {
        p_advance(p);
        AjsAstNode *obj = ast_new(p, AST_OBJECT_PATTERN, cur.line, cur.col);
        while (!p->had_error && p->lex.cur.kind != TOK_RBRACE && p->lex.cur.kind != TOK_EOF) {
            if (p_match(p, TOK_ELLIPSIS)) {
                AjsAstNode *rest = ast_new(p, AST_REST_ELEMENT, p->lex.cur.line, p->lex.cur.col);
                AjsToken rtok = p_advance(p);
                if (!check_binding_ident_tok(p, rtok, false)) return NULL;
                rest->u.expr_stmt.expr = make_ident_from_tok(p, rtok);
                nodelist_push(p, &obj->u.object.props, rest);
                if (p->lex.cur.kind == TOK_COMMA) {
                    parse_error_tok(p, p->lex.cur, "Rest element must be last property");
                }
                break;
            }
            AjsToken ktok = p->lex.cur;
            AjsAstNode *prop = ast_new(p, AST_PROP_PATTERN, ktok.line, ktok.col);
            if (p_match(p, TOK_LBRACKET)) {
                prop->u.prop.computed = true;
                prop->u.prop.key = parse_assign_expr(p);
                p_expect(p, TOK_RBRACKET, "']'");
                p_expect(p, TOK_COLON, "':'");
                prop->u.prop.value = parse_binding_elem(p);
            } else if (ktok.kind == TOK_STRING || ktok.kind == TOK_NUMBER) {
                p_advance(p);
                if (ktok.kind == TOK_STRING) {
                    AjsAstNode *ks = ast_new(p, AST_LITERAL_STR, ktok.line, ktok.col);
                    ks->u.lit.s_u16 = ktok.u16_data;
                    ks->u.lit.s_len = ktok.u16_len;
                    prop->u.prop.key = ks;
                } else {
                    AjsAstNode *kn = ast_new(p, AST_LITERAL_NUM, ktok.line, ktok.col);
                    kn->u.lit.nval = ktok.num_val;
                    prop->u.prop.key = kn;
                }
                p_expect(p, TOK_COLON, "':'");
                prop->u.prop.value = parse_binding_elem(p);
            } else if (is_property_name_word(ktok.kind)) {
                p_advance(p);
                prop->u.prop.key = make_ident_from_tok(p, ktok);
                if (p_match(p, TOK_COLON)) {
                    prop->u.prop.value = parse_binding_elem(p);
                } else {
                    if (!check_binding_ident_tok(p, ktok, false)) return NULL;
                    prop->u.prop.shorthand = true;
                    AjsAstNode *id = make_ident_from_tok(p, ktok);
                    if (p_match(p, TOK_ASSIGN)) {
                        AjsAstNode *ap = ast_new(p, AST_ASSIGN_PATTERN, ktok.line, ktok.col);
                        ap->u.assign_pat.left = id;
                        ap->u.assign_pat.right = parse_assign_expr(p);
                        prop->u.prop.value = ap;
                    } else {
                        prop->u.prop.value = id;
                    }
                }
            } else {
                parse_error_tok(p, ktok, "Expected property name in object pattern");
                return NULL;
            }
            nodelist_push(p, &obj->u.object.props, prop);
            if (!p_match(p, TOK_COMMA)) break;
        }
        p_expect(p, TOK_RBRACE, "'}'");
        return obj;
    }

    AjsToken itok = p_advance(p);
    if (!check_binding_ident_tok(p, itok, false)) return NULL;
    return make_ident_from_tok(p, itok);
}

/* Validates that no unconsumed `{ x = 1 }` CoverInitializedName remains inside a normal expression */
static void check_no_cover_init(AjsParser *p, AjsAstNode *expr) {
    if (!expr || p->had_error) return;
    if (expr->kind == AST_OBJECT_EXPR) {
        for (uint32_t i = 0; i < expr->u.object.props.count; i++) {
            AjsAstNode *pr = expr->u.object.props.items[i];
            if (pr && pr->kind == AST_OBJECT_PROP && pr->u.prop.shorthand &&
                pr->u.prop.value && pr->u.prop.value->kind == AST_ASSIGN_PATTERN) {
                parse_error_at(p, pr->line, pr->col, "Invalid shorthand property initializer");
                return;
            }
        }
    }
}

static bool is_valid_simple_lhs(AjsParser *p, AjsAstNode *e) {
    if (!e) return false;
    if (e->kind == AST_IDENT) {
        if (p->strict && (u16_eq_ascii(e->u.ident.u16, e->u.ident.len, "eval") ||
                          u16_eq_ascii(e->u.ident.u16, e->u.ident.len, "arguments"))) {
            parse_error_at(p, e->line, e->col, "Invalid assignment to eval or arguments in strict mode");
            return false;
        }
        return true;
    }
    if (e->kind == AST_MEMBER_EXPR && !e->u.member.optional) return true;
    return false;
}

static AjsAstNode *expr_to_pattern(AjsParser *p, AjsAstNode *e, bool for_arrow_param) {
    if (!e || p->had_error) return NULL;
    if (e->parenthesized) {
        if (for_arrow_param || (e->kind != AST_IDENT && e->kind != AST_MEMBER_EXPR)) {
            parse_error_at(p, e->line, e->col, "Invalid parenthesized destructuring pattern");
            return NULL;
        }
    }
    switch (e->kind) {
        case AST_IDENT:
            if (p->strict && (u16_eq_ascii(e->u.ident.u16, e->u.ident.len, "eval") ||
                              u16_eq_ascii(e->u.ident.u16, e->u.ident.len, "arguments"))) {
                parse_error_at(p, e->line, e->col, "Unexpected eval or arguments in strict mode");
                return NULL;
            }
            return e;
        case AST_MEMBER_EXPR:
            if (for_arrow_param || e->u.member.optional) {
                parse_error_at(p, e->line, e->col, "Invalid destructuring target");
                return NULL;
            }
            return e;
        case AST_ASSIGN_EXPR:
            if (e->u.binary.op != TOK_ASSIGN) {
                parse_error_at(p, e->line, e->col, "Invalid assignment in destructuring pattern");
                return NULL;
            }
            {
                AjsAstNode *ap = ast_new(p, AST_ASSIGN_PATTERN, e->line, e->col);
                ap->u.assign_pat.left = expr_to_pattern(p, e->u.binary.left, for_arrow_param);
                ap->u.assign_pat.right = e->u.binary.right;
                return ap;
            }
        case AST_SPREAD_ELEM: {
            AjsAstNode *rest = ast_new(p, AST_REST_ELEMENT, e->line, e->col);
            rest->u.expr_stmt.expr = expr_to_pattern(p, e->u.expr_stmt.expr, for_arrow_param);
            if (rest->u.expr_stmt.expr && rest->u.expr_stmt.expr->kind == AST_ASSIGN_PATTERN) {
                parse_error_at(p, e->line, e->col, "Rest element may not have a default initializer");
            }
            return rest;
        }
        case AST_ARRAY_EXPR: {
            AjsAstNode *ap = ast_new(p, AST_ARRAY_PATTERN, e->line, e->col);
            for (uint32_t i = 0; i < e->u.array.elems.count; i++) {
                AjsAstNode *el = e->u.array.elems.items[i];
                if (!el) {
                    nodelist_push(p, &ap->u.array.elems, NULL);
                } else {
                    if (el->kind == AST_SPREAD_ELEM && i + 1 < e->u.array.elems.count) {
                        parse_error_at(p, el->line, el->col, "Rest element must be last element");
                        return NULL;
                    }
                    nodelist_push(p, &ap->u.array.elems, expr_to_pattern(p, el, for_arrow_param));
                }
            }
            return ap;
        }
        case AST_OBJECT_EXPR: {
            AjsAstNode *op = ast_new(p, AST_OBJECT_PATTERN, e->line, e->col);
            for (uint32_t i = 0; i < e->u.object.props.count; i++) {
                AjsAstNode *pr = e->u.object.props.items[i];
                if (pr->kind == AST_SPREAD_ELEM) {
                    if (i + 1 < e->u.object.props.count) {
                        parse_error_at(p, pr->line, pr->col, "Rest element must be last property");
                        return NULL;
                    }
                    AjsAstNode *rt = expr_to_pattern(p, pr->u.expr_stmt.expr, for_arrow_param);
                    if (!rt || (rt->kind != AST_IDENT && (for_arrow_param || rt->kind != AST_MEMBER_EXPR))) {
                        parse_error_at(p, pr->line, pr->col, "Invalid rest property target");
                        return NULL;
                    }
                    AjsAstNode *rest = ast_new(p, AST_REST_ELEMENT, pr->line, pr->col);
                    rest->u.expr_stmt.expr = rt;
                    nodelist_push(p, &op->u.object.props, rest);
                } else if (pr->kind == AST_OBJECT_PROP && pr->u.prop.kind == AJS_PROP_INIT) {
                    AjsAstNode *pp = ast_new(p, AST_PROP_PATTERN, pr->line, pr->col);
                    pp->u.prop.computed = pr->u.prop.computed;
                    pp->u.prop.shorthand = pr->u.prop.shorthand;
                    pp->u.prop.key = pr->u.prop.key;
                    if (pr->u.prop.shorthand && pr->u.prop.value->kind == AST_ASSIGN_PATTERN) {
                        pr->u.prop.value->u.assign_pat.left = expr_to_pattern(p, pr->u.prop.value->u.assign_pat.left, for_arrow_param);
                        pp->u.prop.value = pr->u.prop.value;
                    } else {
                        pp->u.prop.value = expr_to_pattern(p, pr->u.prop.value, for_arrow_param);
                    }
                nodelist_push(p, &op->u.object.props, pp);
                } else {
                    parse_error_at(p, pr->line, pr->col, "Invalid property in destructuring pattern");
                    return NULL;
                }
            }
            return op;
        }
        case AST_ASSIGN_PATTERN:
            e->u.assign_pat.left = expr_to_pattern(p, e->u.assign_pat.left, for_arrow_param);
            return e;
        case AST_REST_ELEMENT:
            e->u.expr_stmt.expr = expr_to_pattern(p, e->u.expr_stmt.expr, for_arrow_param);
            if (e->u.expr_stmt.expr && e->u.expr_stmt.expr->kind == AST_ASSIGN_PATTERN) {
                parse_error_at(p, e->line, e->col, "Rest element may not have a default initializer");
            }
            return e;
        case AST_ARRAY_PATTERN:
            for (uint32_t i = 0; i < e->u.array.elems.count; i++) {
                if (e->u.array.elems.items[i]) {
                    e->u.array.elems.items[i] = expr_to_pattern(p, e->u.array.elems.items[i], for_arrow_param);
                }
            }
            return e;
        case AST_OBJECT_PATTERN:
            for (uint32_t i = 0; i < e->u.object.props.count; i++) {
                AjsAstNode *pr = e->u.object.props.items[i];
                if (!pr) continue;
                if (pr->kind == AST_REST_ELEMENT) {
                    pr->u.expr_stmt.expr = expr_to_pattern(p, pr->u.expr_stmt.expr, for_arrow_param);
                    if (!pr->u.expr_stmt.expr || (pr->u.expr_stmt.expr->kind != AST_IDENT &&
                        (for_arrow_param || pr->u.expr_stmt.expr->kind != AST_MEMBER_EXPR))) {
                        parse_error_at(p, pr->line, pr->col, "Invalid rest property target");
                        return NULL;
                    }
                } else if (pr->kind == AST_PROP_PATTERN) {
                    pr->u.prop.value = expr_to_pattern(p, pr->u.prop.value, for_arrow_param);
                }
            }
            return e;
        default:
            parse_error_at(p, e->line, e->col, "Invalid destructuring assignment target");
            return NULL;
    }
}

static bool params_are_simple(const AjsNodeList *params) {
    for (uint32_t i = 0; i < params->count; i++) {
        if (!params->items[i] || params->items[i]->kind != AST_IDENT) return false;
    }
    return true;
}

/* ============================================================================
 * Template, Object, Array, Arrow & Primary Expressions
 * ============================================================================ */

static AjsAstNode *parse_template_literal(AjsParser *p, bool is_tagged) {
    AjsToken head = p->lex.cur;
    AjsAstNode *tpl = ast_new(p, AST_TEMPLATE_LIT, head.line, head.col);
    uint32_t cap = 4u, cnt = 0;
    tpl->u.tpl.cooked = (const uint16_t **)ajs_arena_alloc(p->arena, cap * sizeof(uint16_t *));
    tpl->u.tpl.cooked_len = (uint32_t *)ajs_arena_alloc(p->arena, cap * sizeof(uint32_t));
    tpl->u.tpl.raw = (const uint16_t **)ajs_arena_alloc(p->arena, cap * sizeof(uint16_t *));
    tpl->u.tpl.raw_len = (uint32_t *)ajs_arena_alloc(p->arena, cap * sizeof(uint32_t));

    if (head.invalid_tpl_escape && !is_tagged) {
        parse_error_tok(p, head, "Invalid escape sequence in template literal");
        return tpl;
    }
    tpl->u.tpl.cooked[0] = head.invalid_tpl_escape ? NULL : head.u16_data;
    tpl->u.tpl.cooked_len[0] = head.u16_len;
    tpl->u.tpl.raw[0] = head.raw_u16;
    tpl->u.tpl.raw_len[0] = head.raw_u16_len;
    cnt = 1;
    p_advance(p);

    if (head.kind == TOK_TEMPLATE_NO_SUB) {
        tpl->u.tpl.part_count = 1;
        return tpl;
    }

    while (!p->had_error) {
        bool saved_in = p->allow_in;
        p->allow_in = true;
        AjsAstNode *sub = parse_expr(p);
        p->allow_in = saved_in;
        check_no_cover_init(p, sub);
        nodelist_push(p, &tpl->u.tpl.exprs, sub);
        if (p->lex.cur.kind != TOK_RBRACE) {
            parse_error_tok(p, p->lex.cur, "Expected '}' in template substitution");
            break;
        }
        if (!ajs_lexer_rescan_template(&p->lex)) {
            parse_error_tok(p, p->lex.cur, "%s", p->lex.err_msg);
            break;
        }
        AjsToken part = p->lex.cur;
        if (part.invalid_tpl_escape && !is_tagged) {
            parse_error_tok(p, part, "Invalid escape sequence in template literal");
            break;
        }
        if (cnt >= cap) {
            uint32_t ncap = cap * 2u;
            const uint16_t **nc = (const uint16_t **)ajs_arena_alloc(p->arena, ncap * sizeof(uint16_t *));
            uint32_t *ncl = (uint32_t *)ajs_arena_alloc(p->arena, ncap * sizeof(uint32_t));
            const uint16_t **nr = (const uint16_t **)ajs_arena_alloc(p->arena, ncap * sizeof(uint16_t *));
            uint32_t *nrl = (uint32_t *)ajs_arena_alloc(p->arena, ncap * sizeof(uint32_t));
            ajs_memcpy(nc, tpl->u.tpl.cooked, cnt * sizeof(uint16_t *));
            ajs_memcpy(ncl, tpl->u.tpl.cooked_len, cnt * sizeof(uint32_t));
            ajs_memcpy(nr, tpl->u.tpl.raw, cnt * sizeof(uint16_t *));
            ajs_memcpy(nrl, tpl->u.tpl.raw_len, cnt * sizeof(uint32_t));
            tpl->u.tpl.cooked = nc; tpl->u.tpl.cooked_len = ncl;
            tpl->u.tpl.raw = nr; tpl->u.tpl.raw_len = nrl;
            cap = ncap;
        }
        tpl->u.tpl.cooked[cnt] = part.invalid_tpl_escape ? NULL : part.u16_data;
        tpl->u.tpl.cooked_len[cnt] = part.u16_len;
        tpl->u.tpl.raw[cnt] = part.raw_u16;
        tpl->u.tpl.raw_len[cnt] = part.raw_u16_len;
        cnt++;
        p_advance(p);
        if (part.kind == TOK_TEMPLATE_TAIL) break;
    }
    tpl->u.tpl.part_count = cnt;
    return tpl;
}

static AjsAstNode *parse_method_body_node(AjsParser *p, size_t src_start, bool is_async, bool is_gen,
                                          AjsPropKind pkind, bool allow_super_call) {
    AjsFuncKind fk = AJS_FN_BC_METHOD;
    if (is_async && is_gen) fk = AJS_FN_BC_ASYNC_GENERATOR;
    else if (is_async) fk = AJS_FN_BC_ASYNC;
    else if (is_gen) fk = AJS_FN_BC_GENERATOR;

    AjsAstNode *fn = ast_new(p, AST_FUNC_EXPR, p->lex.cur.line, p->lex.cur.col);
    fn->u.func.fn_kind = fk;

    bool saved_fn = p->in_function, saved_gen = p->in_generator, saved_async = p->in_async;
    bool saved_nt = p->allow_new_target, saved_sp = p->allow_super_prop, saved_sc = p->allow_super_call;
    bool saved_sb = p->in_static_block, saved_strict = p->strict;
    int saved_loop = p->loop_depth, saved_sw = p->switch_depth;
    uint32_t saved_lbl = p->label_count;

    p->in_function = true;
    p->in_generator = is_gen;
    p->in_async = is_async;
    p->allow_new_target = true;
    p->allow_super_prop = true;
    p->allow_super_call = allow_super_call;
    p->in_static_block = false;
    p->loop_depth = 0;
    p->switch_depth = 0;

    scope_push(p, true, false);
    p_expect(p, TOK_LPAREN, "'('");
    bool saved_fp = p->in_formal_params;
    p->in_formal_params = true;
    if (pkind == AJS_PROP_GET) {
        /* Getter takes 0 parameters */
    } else if (pkind == AJS_PROP_SET) {
        if (p->lex.cur.kind == TOK_RPAREN || p->lex.cur.kind == TOK_ELLIPSIS) {
            parse_error_tok(p, p->lex.cur, "Setter must have exactly one formal parameter");
        } else {
            nodelist_push(p, &fn->u.func.params, parse_binding_elem(p));
        }
    } else {
        while (!p->had_error && p->lex.cur.kind != TOK_RPAREN && p->lex.cur.kind != TOK_EOF) {
            if (p_match(p, TOK_ELLIPSIS)) {
                AjsAstNode *rest = ast_new(p, AST_REST_ELEMENT, p->lex.cur.line, p->lex.cur.col);
                rest->u.expr_stmt.expr = parse_binding_pattern_or_ident(p);
                nodelist_push(p, &fn->u.func.params, rest);
                if (p->lex.cur.kind == TOK_COMMA) parse_error_tok(p, p->lex.cur, "Rest parameter must be last");
                break;
            }
            nodelist_push(p, &fn->u.func.params, parse_binding_elem(p));
            if (!p_match(p, TOK_COMMA)) break;
        }
    }
    p->in_formal_params = saved_fp;
    p_expect(p, TOK_RPAREN, "')'");

    fn->u.func.has_simple_params = params_are_simple(&fn->u.func.params);
    /* Parse body block & check directive prologue */
    AjsToken lb = p->lex.cur;
    p_expect(p, TOK_LBRACE, "'{'");
    AjsAstNode *blk = ast_new(p, AST_BLOCK, lb.line, lb.col);
    bool in_prologue = true;
    bool prologue_octal = false;
    while (!p->had_error && p->lex.cur.kind != TOK_RBRACE && p->lex.cur.kind != TOK_EOF) {
        AjsToken st_tok = p->lex.cur;
        AjsAstNode *st = parse_stmt(p, true);
        if (in_prologue && st && st->kind == AST_EXPR_STMT && st->u.expr_stmt.expr &&
            st->u.expr_stmt.expr->kind == AST_LITERAL_STR && !st->u.expr_stmt.expr->parenthesized) {
            if (st_tok.has_legacy_octal) prologue_octal = true;
            if (!st_tok.has_escape && !st_tok.has_legacy_octal &&
                u16_eq_ascii(st_tok.u16_data, st_tok.u16_len, "use strict")) {
                p->strict = true;
                p->lex.strict = true;
                if (!fn->u.func.has_simple_params) {
                    parse_error_tok(p, st_tok, "Illegal 'use strict' directive in function with non-simple parameter list");
                }
                if (prologue_octal) {
                    parse_error_tok(p, st_tok, "Octal escape sequences are not allowed in strict mode");
                }
            }
        } else {
            in_prologue = false;
        }
        nodelist_push(p, &blk->u.block.stmts, st);
    }
    size_t src_end = p->lex.cur.end_off;
    p_expect(p, TOK_RBRACE, "'}'");
    fn->u.func.strict = p->strict;
    fn->u.func.body = blk;
    fn->u.func.src_utf8 = p->lex.src + src_start;
    fn->u.func.src_len = (src_end >= src_start) ? (src_end - src_start) : 0;

    AjsNodeList seen = {0, 0, NULL};
    for (uint32_t i = 0; i < fn->u.func.params.count; i++) {
        declare_pattern_names(p, fn->u.func.params.items[i], false, true, &seen);
    }

    scope_pop(p);
    p->in_function = saved_fn; p->in_generator = saved_gen; p->in_async = saved_async;
    p->allow_new_target = saved_nt; p->allow_super_prop = saved_sp; p->allow_super_call = saved_sc;
    p->in_static_block = saved_sb; p->strict = saved_strict; p->lex.strict = saved_strict;
    p->loop_depth = saved_loop; p->switch_depth = saved_sw; p->label_count = saved_lbl;
    return fn;
}

static AjsAstNode *parse_object_literal(AjsParser *p) {
    AjsToken start = p_advance(p); /* '{' */
    AjsAstNode *obj = ast_new(p, AST_OBJECT_EXPR, start.line, start.col);
    bool seen_proto = false;

    while (!p->had_error && p->lex.cur.kind != TOK_RBRACE && p->lex.cur.kind != TOK_EOF) {
        size_t elem_off = p->lex.cur.start_off;
        if (p_match(p, TOK_ELLIPSIS)) {
            AjsAstNode *sp = ast_new(p, AST_SPREAD_ELEM, p->lex.cur.line, p->lex.cur.col);
            sp->u.expr_stmt.expr = parse_assign_expr(p);
            check_no_cover_init(p, sp->u.expr_stmt.expr);
            nodelist_push(p, &obj->u.object.props, sp);
            if (!p_match(p, TOK_COMMA)) break;
            continue;
        }

        AjsAstNode *prop = ast_new(p, AST_OBJECT_PROP, p->lex.cur.line, p->lex.cur.col);
        prop->u.prop.kind = AJS_PROP_INIT;
        bool is_async = false, is_gen = false;

        /* Check async / get / set / * modifiers */
        if (p->lex.cur.kind == TOK_IDENT && !p->lex.cur.has_escape &&
            u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "async")) {
            AjsToken pk = ajs_lexer_peek(&p->lex);
            if (!pk.had_newline && (pk.kind == TOK_MUL || pk.kind == TOK_LBRACKET ||
                pk.kind == TOK_STRING || pk.kind == TOK_NUMBER || is_property_name_word(pk.kind))) {
                p_advance(p);
                is_async = true;
            }
        }
        if (p_match(p, TOK_MUL)) is_gen = true;

        if (!is_async && !is_gen && p->lex.cur.kind == TOK_IDENT && !p->lex.cur.has_escape &&
            (u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "get") ||
             u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "set"))) {
            AjsToken pk = ajs_lexer_peek(&p->lex);
            if (pk.kind == TOK_LBRACKET || pk.kind == TOK_STRING || pk.kind == TOK_NUMBER ||
                is_property_name_word(pk.kind)) {
                bool is_get = u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "get");
                p_advance(p);
                prop->u.prop.kind = is_get ? AJS_PROP_GET : AJS_PROP_SET;
            }
        }

        AjsToken ktok = p->lex.cur;
        if (p_match(p, TOK_LBRACKET)) {
            prop->u.prop.computed = true;
            bool saved_in = p->allow_in;
            p->allow_in = true;
            prop->u.prop.key = parse_assign_expr(p);
            p->allow_in = saved_in;
            check_no_cover_init(p, prop->u.prop.key);
            p_expect(p, TOK_RBRACKET, "']'");
        } else if (ktok.kind == TOK_STRING) {
            p_advance(p);
            AjsAstNode *ks = ast_new(p, AST_LITERAL_STR, ktok.line, ktok.col);
            ks->u.lit.s_u16 = ktok.u16_data; ks->u.lit.s_len = ktok.u16_len;
            prop->u.prop.key = ks;
        } else if (ktok.kind == TOK_NUMBER) {
            p_advance(p);
            AjsAstNode *kn = ast_new(p, AST_LITERAL_NUM, ktok.line, ktok.col);
            kn->u.lit.nval = ktok.num_val;
            prop->u.prop.key = kn;
        } else if (ktok.kind == TOK_BIGINT) {
            p_advance(p);
            AjsAstNode *kb = ast_new(p, AST_LITERAL_BIGINT, ktok.line, ktok.col);
            kb->u.lit.bi_utf8 = ktok.utf8_text; kb->u.lit.bi_len = ktok.utf8_len;
            prop->u.prop.key = kb;
        } else if (is_property_name_word(ktok.kind)) {
            p_advance(p);
            prop->u.prop.key = make_ident_from_tok(p, ktok);
        } else {
            parse_error_tok(p, ktok, "Expected property name");
            break;
        }

        if (is_async || is_gen || prop->u.prop.kind != AJS_PROP_INIT || p->lex.cur.kind == TOK_LPAREN) {
            if (prop->u.prop.kind == AJS_PROP_INIT) prop->u.prop.kind = AJS_PROP_METHOD;
            prop->u.prop.value = parse_method_body_node(p, elem_off, is_async, is_gen, prop->u.prop.kind, false);
        } else if (p_match(p, TOK_COLON)) {
            /* Duplicate __proto__: value in object literal is an early SyntaxError (ECMA-262 Annex B.3.1)! */
            if (!prop->u.prop.computed) {
                bool is_dunder = (prop->u.prop.key->kind == AST_IDENT &&
                                  u16_eq_ascii(prop->u.prop.key->u.ident.u16, prop->u.prop.key->u.ident.len, "__proto__")) ||
                                 (prop->u.prop.key->kind == AST_LITERAL_STR &&
                                  u16_eq_ascii(prop->u.prop.key->u.lit.s_u16, prop->u.prop.key->u.lit.s_len, "__proto__"));
                if (is_dunder) {
                    if (seen_proto) parse_error_at(p, prop->line, prop->col, "Duplicate __proto__ fields are not allowed in object literals");
                    seen_proto = true;
                }
            }
            prop->u.prop.value = parse_assign_expr(p);
            check_no_cover_init(p, prop->u.prop.value);
        } else if (!prop->u.prop.computed && prop->u.prop.key->kind == AST_IDENT && is_ident_reference_tok(p, ktok)) {
            prop->u.prop.shorthand = true;
            AjsAstNode *id = make_ident_from_tok(p, ktok);
            if (p_match(p, TOK_ASSIGN)) {
                /* CoverInitializedName `{ a = 1 }` — valid ONLY if converted to destructuring pattern! */
                AjsAstNode *ap = ast_new(p, AST_ASSIGN_PATTERN, ktok.line, ktok.col);
                ap->u.assign_pat.left = id;
                ap->u.assign_pat.right = parse_assign_expr(p);
                check_no_cover_init(p, ap->u.assign_pat.right);
                prop->u.prop.value = ap;
            } else {
                prop->u.prop.value = id;
            }
        } else {
            parse_error_tok(p, p->lex.cur, "Expected ':' in object property");
            break;
        }

        nodelist_push(p, &obj->u.object.props, prop);
        if (!p_match(p, TOK_COMMA)) break;
    }
    p_expect(p, TOK_RBRACE, "'}'");
    return obj;
}

static AjsAstNode *finish_arrow_function(AjsParser *p, const AjsNodeList *params, bool is_async,
                                         size_t src_start, uint32_t line, uint32_t col) {
    if (p->lex.cur.had_newline) {
        parse_error_tok(p, p->lex.cur, "No line terminator allowed before '=>'");
        return NULL;
    }
    p_expect(p, TOK_ARROW, "'=>'");

    AjsAstNode *fn = ast_new(p, AST_FUNC_EXPR, line, col);
    fn->u.func.fn_kind = is_async ? AJS_FN_BC_ASYNC_ARROW : AJS_FN_BC_ARROW;
    fn->u.func.params = *params;
    fn->u.func.has_simple_params = params_are_simple(params);

    bool saved_fn = p->in_function, saved_gen = p->in_generator, saved_async = p->in_async;
    bool saved_sb = p->in_static_block, saved_strict = p->strict;
    int saved_loop = p->loop_depth, saved_sw = p->switch_depth;
    uint32_t saved_lbl = p->label_count;

    p->in_function = true;
    p->in_generator = false;
    p->in_async = is_async;
    p->in_static_block = false;
    p->loop_depth = 0;
    p->switch_depth = 0;
    /* Note: allow_new_target, allow_super_prop, allow_super_call are INHERITED lexically by arrows! */

    scope_push(p, true, false);
    size_t src_end = p->lex.cur.end_off;
    if (p->lex.cur.kind == TOK_LBRACE) {
        fn->u.func.expr_body = false;
        AjsToken lb = p_advance(p);
        AjsAstNode *blk = ast_new(p, AST_BLOCK, lb.line, lb.col);
        bool in_prologue = true, prologue_octal = false;
        while (!p->had_error && p->lex.cur.kind != TOK_RBRACE && p->lex.cur.kind != TOK_EOF) {
            AjsToken st_tok = p->lex.cur;
            AjsAstNode *st = parse_stmt(p, true);
            if (in_prologue && st && st->kind == AST_EXPR_STMT && st->u.expr_stmt.expr &&
                st->u.expr_stmt.expr->kind == AST_LITERAL_STR && !st->u.expr_stmt.expr->parenthesized) {
                if (st_tok.has_legacy_octal) prologue_octal = true;
                if (!st_tok.has_escape && !st_tok.has_legacy_octal &&
                    u16_eq_ascii(st_tok.u16_data, st_tok.u16_len, "use strict")) {
                    p->strict = true; p->lex.strict = true;
                    if (!fn->u.func.has_simple_params) {
                        parse_error_tok(p, st_tok, "Illegal 'use strict' directive in arrow with non-simple parameters");
                    }
                    if (prologue_octal) {
                        parse_error_tok(p, st_tok, "Octal escape sequences are not allowed in strict mode");
                    }
                }
            } else {
                in_prologue = false;
            }
            nodelist_push(p, &blk->u.block.stmts, st);
        }
        src_end = p->lex.cur.end_off;
        p_expect(p, TOK_RBRACE, "'}'");
        fn->u.func.body = blk;
    } else {
        fn->u.func.expr_body = true;
        fn->u.func.body = parse_assign_expr(p);
        check_no_cover_init(p, fn->u.func.body);
        src_end = p->lex.cur.start_off;
    }

    fn->u.func.strict = p->strict;
    fn->u.func.src_utf8 = p->lex.src + src_start;
    fn->u.func.src_len = (src_end >= src_start) ? (src_end - src_start) : 0;

    AjsNodeList seen = {0, 0, NULL};
    for (uint32_t i = 0; i < fn->u.func.params.count; i++) {
        declare_pattern_names(p, fn->u.func.params.items[i], false, true, &seen);
    }

    scope_pop(p);
    p->in_function = saved_fn; p->in_generator = saved_gen; p->in_async = saved_async;
    p->in_static_block = saved_sb; p->strict = saved_strict; p->lex.strict = saved_strict;
    p->loop_depth = saved_loop; p->switch_depth = saved_sw; p->label_count = saved_lbl;
    return fn;
}

static AjsAstNode *parse_paren_or_arrow(AjsParser *p, bool is_async, size_t src_start, uint32_t line, uint32_t col) {
    p_expect(p, TOK_LPAREN, "'('");
    if (p->lex.cur.kind == TOK_RPAREN) {
        p_advance(p);
        if (p->lex.cur.kind != TOK_ARROW) {
            parse_error_tok(p, p->lex.cur, "Expected '=>' after '()'");
            return NULL;
        }
        AjsNodeList empty_params = {0, 0, NULL};
        return finish_arrow_function(p, &empty_params, is_async, src_start, line, col);
    }

    bool saved_in = p->allow_in;
    p->allow_in = true;
    AjsNodeList raw_items = {0, 0, NULL};
    bool has_spread = false;
    bool trailing_comma = false;
    while (!p->had_error && p->lex.cur.kind != TOK_RPAREN && p->lex.cur.kind != TOK_EOF) {
        if (p_match(p, TOK_ELLIPSIS)) {
            has_spread = true;
            AjsAstNode *sp = ast_new(p, AST_SPREAD_ELEM, p->lex.cur.line, p->lex.cur.col);
            sp->u.expr_stmt.expr = parse_assign_expr(p);
            nodelist_push(p, &raw_items, sp);
            if (p->lex.cur.kind == TOK_COMMA) {
                parse_error_tok(p, p->lex.cur, "Rest parameter must be last formal parameter");
            }
            break;
        }
        nodelist_push(p, &raw_items, parse_assign_expr(p));
        if (p_match(p, TOK_COMMA)) {
            if (p->lex.cur.kind == TOK_RPAREN) {
                trailing_comma = true;
                break;
            }
        } else {
            break;
        }
    }
    p->allow_in = saved_in;
    p_expect(p, TOK_RPAREN, "')'");

    if (p->lex.cur.kind == TOK_ARROW) {
        AjsNodeList params = {0, 0, NULL};
        bool saved_async = p->in_async;
        if (is_async) p->in_async = true;
        for (uint32_t i = 0; i < raw_items.count; i++) {
            nodelist_push(p, &params, expr_to_pattern(p, raw_items.items[i], true));
        }
        p->in_async = saved_async;
        return finish_arrow_function(p, &params, is_async, src_start, line, col);
    }

    if (is_async) {
        /* Was actually a call `async(a, b)`! */
        AjsAstNode *callee = ast_new(p, AST_IDENT, line, col);
        static const uint16_t u_async[5] = {'a','s','y','n','c'};
        callee->u.ident.u16 = u_async;
        callee->u.ident.len = 5;
        callee->u.ident.utf8 = "async";
        AjsAstNode *call = ast_new(p, AST_CALL_EXPR, line, col);
        call->u.call.callee = callee;
        for (uint32_t i = 0; i < raw_items.count; i++) {
            check_no_cover_init(p, raw_items.items[i]);
        }
        call->u.call.args = raw_items;
        return call;
    }

    if (has_spread || trailing_comma || raw_items.count == 0) {
        parse_error_at(p, line, col, "Unexpected parenthesized expression");
        return NULL;
    }
    for (uint32_t i = 0; i < raw_items.count; i++) {
        check_no_cover_init(p, raw_items.items[i]);
    }
    if (raw_items.count == 1) {
        raw_items.items[0]->parenthesized = true;
        return raw_items.items[0];
    }
    AjsAstNode *seq = ast_new(p, AST_SEQ_EXPR, line, col);
    seq->u.seq.exprs = raw_items;
    seq->parenthesized = true;
    return seq;
}

static AjsAstNode *parse_primary(AjsParser *p) {
    AjsToken cur = p->lex.cur;

    if (cur.kind == TOK_DIV || cur.kind == TOK_DIV_ASSIGN) {
        if (!ajs_lexer_rescan_regexp(&p->lex)) {
            parse_error_tok(p, p->lex.cur, "%s", p->lex.err_msg);
            return NULL;
        }
        cur = p_advance(p);
        char rx_err[128];
        if (!ajs_regexp_validate_syntax(p->vm, cur.u16_data, cur.u16_len,
                                        cur.rx_flags, cur.rx_flags_len, rx_err, sizeof(rx_err))) {
            parse_error_tok(p, cur, "Invalid regular expression: %s", rx_err);
            return NULL;
        }
        AjsAstNode *rx = ast_new(p, AST_LITERAL_REGEXP, cur.line, cur.col);
        rx->u.lit.rx_pat = cur.u16_data;
        rx->u.lit.rx_pat_len = cur.u16_len;
        rx->u.lit.rx_flags = cur.rx_flags;
        rx->u.lit.rx_flags_len = cur.rx_flags_len;
        return rx;
    }

    switch (cur.kind) {
        case KW_THIS:
            p_advance(p);
            return ast_new(p, AST_THIS_EXPR, cur.line, cur.col);
        case KW_NULL:
            p_advance(p);
            return ast_new(p, AST_LITERAL_NULL, cur.line, cur.col);
        case KW_TRUE:
        case KW_FALSE: {
            p_advance(p);
            AjsAstNode *n = ast_new(p, AST_LITERAL_BOOL, cur.line, cur.col);
            n->u.lit.bval = (cur.kind == KW_TRUE);
            return n;
        }
        case TOK_NUMBER: {
            p_advance(p);
            AjsAstNode *n = ast_new(p, AST_LITERAL_NUM, cur.line, cur.col);
            n->u.lit.nval = cur.num_val;
            return n;
        }
        case TOK_BIGINT: {
            p_advance(p);
            AjsAstNode *n = ast_new(p, AST_LITERAL_BIGINT, cur.line, cur.col);
            n->u.lit.bi_utf8 = cur.utf8_text;
            n->u.lit.bi_len = cur.utf8_len;
            return n;
        }
        case TOK_STRING: {
            p_advance(p);
            AjsAstNode *n = ast_new(p, AST_LITERAL_STR, cur.line, cur.col);
            n->u.lit.s_u16 = cur.u16_data;
            n->u.lit.s_len = cur.u16_len;
            return n;
        }
        case TOK_TEMPLATE_NO_SUB:
        case TOK_TEMPLATE_HEAD:
            return parse_template_literal(p, false);
        case TOK_LBRACKET: {
            p_advance(p);
            AjsAstNode *arr = ast_new(p, AST_ARRAY_EXPR, cur.line, cur.col);
            bool saved_in = p->allow_in;
            p->allow_in = true;
            while (!p->had_error && p->lex.cur.kind != TOK_RBRACKET && p->lex.cur.kind != TOK_EOF) {
                if (p_match(p, TOK_COMMA)) {
                    nodelist_push(p, &arr->u.array.elems, NULL);
                    continue;
                }
                if (p_match(p, TOK_ELLIPSIS)) {
                    AjsAstNode *sp = ast_new(p, AST_SPREAD_ELEM, p->lex.cur.line, p->lex.cur.col);
                    sp->u.expr_stmt.expr = parse_assign_expr(p);
                    check_no_cover_init(p, sp->u.expr_stmt.expr);
                    nodelist_push(p, &arr->u.array.elems, sp);
                } else {
                    AjsAstNode *el = parse_assign_expr(p);
                    nodelist_push(p, &arr->u.array.elems, el);
                }
                if (!p_match(p, TOK_COMMA)) break;
            }
            p->allow_in = saved_in;
            p_expect(p, TOK_RBRACKET, "']'");
            return arr;
        }
        case TOK_LBRACE:
            return parse_object_literal(p);
        case TOK_LPAREN:
            return parse_paren_or_arrow(p, false, cur.start_off, cur.line, cur.col);
        case KW_FUNCTION:
            return parse_func(p, false, false, AJS_FN_BC_NORMAL);
        case KW_CLASS:
            return parse_class(p, false);
        case KW_SUPER: {
            p_advance(p);
            if (p->lex.cur.kind == TOK_LPAREN) {
                if (!p->allow_super_call) {
                    parse_error_tok(p, cur, "'super()' call unexpected here");
                    return NULL;
                }
            } else if (p->lex.cur.kind == TOK_DOT || p->lex.cur.kind == TOK_LBRACKET) {
                if (!p->allow_super_prop) {
                    parse_error_tok(p, cur, "'super' property access unexpected here");
                    return NULL;
                }
            } else {
                parse_error_tok(p, cur, "Invalid use of 'super'");
                return NULL;
            }
            return ast_new(p, AST_SUPER_EXPR, cur.line, cur.col);
        }
        case KW_IMPORT: {
            p_advance(p);
            if (p_match(p, TOK_DOT)) {
                AjsToken mtok = p_advance(p);
                if (!p->is_module || mtok.kind != TOK_IDENT || mtok.has_escape ||
                    !u16_eq_ascii(mtok.u16_data, mtok.u16_len, "meta")) {
                    parse_error_tok(p, mtok, "Cannot use 'import.meta' outside a module");
                    return NULL;
                }
                return ast_new(p, AST_IMPORT_META, cur.line, cur.col);
            }
            p_expect(p, TOK_LPAREN, "'(' after import");
            bool saved_in = p->allow_in;
            p->allow_in = true;
            AjsAstNode *spec = parse_assign_expr(p);
            check_no_cover_init(p, spec);
            if (p_match(p, TOK_COMMA)) {
                if (p->lex.cur.kind != TOK_RPAREN) {
                    AjsAstNode *opts = parse_assign_expr(p);
                    check_no_cover_init(p, opts);
                    p_match(p, TOK_COMMA);
                }
            }
            p->allow_in = saved_in;
            p_expect(p, TOK_RPAREN, "')'");
            AjsAstNode *di = ast_new(p, AST_DYNAMIC_IMPORT, cur.line, cur.col);
            di->u.expr_stmt.expr = spec;
            return di;
        }
        default:
            break;
    }

    /* Check `async function`, `async () =>`, `async x =>` */
    if (cur.kind == TOK_IDENT && !cur.has_escape && u16_eq_ascii(cur.u16_data, cur.u16_len, "async")) {
        AjsToken pk = ajs_lexer_peek(&p->lex);
        if (!pk.had_newline) {
            if (pk.kind == KW_FUNCTION) {
                p_advance(p);
                return parse_func(p, false, true, AJS_FN_BC_ASYNC);
            }
            if (pk.kind == TOK_LPAREN) {
                p_advance(p);
                return parse_paren_or_arrow(p, true, cur.start_off, cur.line, cur.col);
            }
            if (is_ident_reference_tok(p, pk)) {
                /* Peek 2 tokens ahead by consuming `async` and checking if `=>` follows `pk` */
                p_advance(p);
                AjsToken param_tok = p->lex.cur;
                AjsToken after_param = ajs_lexer_peek(&p->lex);
                if (!after_param.had_newline && after_param.kind == TOK_ARROW) {
                    p_advance(p);
                    if (u16_eq_ascii(param_tok.u16_data, param_tok.u16_len, "await")) {
                        parse_error_tok(p, param_tok, "'await' is not allowed as an async arrow parameter");
                    }
                    if (!check_binding_ident_tok(p, param_tok, false)) return NULL;
                    AjsNodeList params = {0, 0, NULL};
                    nodelist_push(p, &params, make_ident_from_tok(p, param_tok));
                    return finish_arrow_function(p, &params, true, cur.start_off, cur.line, cur.col);
                }
                /* Otherwise `cur` (`async`) was just a standalone identifier! Put back? Wait:
                   since `async` followed by ident without `=>` cannot be valid binary expression without operator,
                   wait: what about `async \n + 1`? We already checked `!pk.had_newline`! An identifier followed on
                   the SAME line by another identifier in JS is ALWAYS a syntax error unless `async x =>`! */
                parse_error_tok(p, param_tok, "Unexpected identifier after 'async'");
                return NULL;
            }
        }
    }

    if (is_ident_reference_tok(p, cur)) {
        p_advance(p);
        AjsAstNode *id = make_ident_from_tok(p, cur);
        if (!p->lex.cur.had_newline && p->lex.cur.kind == TOK_ARROW) {
            if (!check_binding_ident_tok(p, cur, false)) return NULL;
            AjsNodeList params = {0, 0, NULL};
            nodelist_push(p, &params, id);
            return finish_arrow_function(p, &params, false, cur.start_off, cur.line, cur.col);
        }
        return id;
    }

    parse_error_tok(p, cur, "Unexpected token");
    return NULL;
}

/* ============================================================================
 * Postfix, Call, Member, Optional Chaining, New, Unary, Binary & Assignment
 * ============================================================================ */

static void parse_arg_list(AjsParser *p, AjsNodeList *out_args) {
    p_expect(p, TOK_LPAREN, "'('");
    bool saved_in = p->allow_in;
    p->allow_in = true;
    while (!p->had_error && p->lex.cur.kind != TOK_RPAREN && p->lex.cur.kind != TOK_EOF) {
        if (p_match(p, TOK_ELLIPSIS)) {
            AjsAstNode *sp = ast_new(p, AST_SPREAD_ELEM, p->lex.cur.line, p->lex.cur.col);
            sp->u.expr_stmt.expr = parse_assign_expr(p);
            check_no_cover_init(p, sp->u.expr_stmt.expr);
            nodelist_push(p, out_args, sp);
        } else {
            AjsAstNode *arg = parse_assign_expr(p);
            check_no_cover_init(p, arg);
            nodelist_push(p, out_args, arg);
        }
        if (!p_match(p, TOK_COMMA)) break;
    }
    p->allow_in = saved_in;
    p_expect(p, TOK_RPAREN, "')'");
}

static AjsAstNode *parse_lhs_expr(AjsParser *p, bool allow_calls) {
    AjsAstNode *base = NULL;
    if (p->lex.cur.kind == KW_NEW) {
        AjsToken ntok = p_advance(p);
        if (p_match(p, TOK_DOT)) {
            AjsToken ttok = p_advance(p);
            if (!p->allow_new_target || ttok.kind != TOK_IDENT || ttok.has_escape ||
                !u16_eq_ascii(ttok.u16_data, ttok.u16_len, "target")) {
                parse_error_tok(p, ttok, "new.target expression is not allowed here");
                return NULL;
            }
            base = ast_new(p, AST_NEW_TARGET, ntok.line, ntok.col);
        } else {
            AjsAstNode *callee = parse_lhs_expr(p, false);
            check_no_cover_init(p, callee);
            if (callee && callee->kind == AST_OPT_CHAIN_EXPR) {
                parse_error_at(p, ntok.line, ntok.col, "Invalid optional chain in new expression");
                return NULL;
            }
            AjsAstNode *ne = ast_new(p, AST_NEW_EXPR, ntok.line, ntok.col);
            ne->u.call.callee = callee;
            if (p->lex.cur.kind == TOK_LPAREN) {
                parse_arg_list(p, &ne->u.call.args);
            }
            base = ne;
        }
    } else {
        base = parse_primary(p);
    }

    bool in_opt_chain = false;
    while (!p->had_error && base) {
        AjsToken cur = p->lex.cur;
        if (cur.kind == TOK_DOT) {
            p_advance(p);
            AjsToken ptok = p->lex.cur;
            AjsAstNode *prop = NULL;
            if (ptok.kind == TOK_PRIV_IDENT) {
                if (base->kind == AST_SUPER_EXPR) {
                    parse_error_tok(p, ptok, "Private fields cannot be accessed on super");
                    return NULL;
                }
                p_advance(p);
                prop = ast_new(p, AST_PRIV_IDENT, ptok.line, ptok.col);
                prop->u.ident.u16 = ptok.u16_data;
                prop->u.ident.len = ptok.u16_len;
                prop->u.ident.utf8 = ptok.utf8_text;
                record_priv_ref(p, prop);
            } else if (is_property_name_word(ptok.kind)) {
                p_advance(p);
                prop = make_ident_from_tok(p, ptok);
            } else {
                parse_error_tok(p, ptok, "Expected property name after '.'");
                return NULL;
            }
            check_no_cover_init(p, base);
            AjsAstNode *m = ast_new(p, AST_MEMBER_EXPR, cur.line, cur.col);
            m->u.member.computed = false;
            m->u.member.optional = false;
            m->u.member.object = base;
            m->u.member.property = prop;
            base = m;
        } else if (cur.kind == TOK_LBRACKET) {
            p_advance(p);
            bool saved_in = p->allow_in;
            p->allow_in = true;
            AjsAstNode *idx = parse_expr(p);
            p->allow_in = saved_in;
            check_no_cover_init(p, idx);
            p_expect(p, TOK_RBRACKET, "']'");
            check_no_cover_init(p, base);
            AjsAstNode *m = ast_new(p, AST_MEMBER_EXPR, cur.line, cur.col);
            m->u.member.computed = true;
            m->u.member.optional = false;
            m->u.member.object = base;
            m->u.member.property = idx;
            base = m;
        } else if (allow_calls && cur.kind == TOK_LPAREN) {
            check_no_cover_init(p, base);
            AjsAstNode *c = ast_new(p, AST_CALL_EXPR, cur.line, cur.col);
            c->u.call.callee = base;
            c->u.call.optional = false;
            parse_arg_list(p, &c->u.call.args);
            base = c;
        } else if (allow_calls && cur.kind == TOK_OPT_CHAIN) {
            if (base->kind == AST_SUPER_EXPR) {
                parse_error_tok(p, cur, "Invalid optional chaining on super");
                return NULL;
            }
            p_advance(p);
            in_opt_chain = true;
            check_no_cover_init(p, base);
            if (p->lex.cur.kind == TOK_LBRACKET) {
                p_advance(p);
                bool saved_in = p->allow_in;
                p->allow_in = true;
                AjsAstNode *idx = parse_expr(p);
                p->allow_in = saved_in;
                check_no_cover_init(p, idx);
                p_expect(p, TOK_RBRACKET, "']'");
                AjsAstNode *m = ast_new(p, AST_MEMBER_EXPR, cur.line, cur.col);
                m->u.member.computed = true;
                m->u.member.optional = true;
                m->u.member.object = base;
                m->u.member.property = idx;
                base = m;
            } else if (p->lex.cur.kind == TOK_LPAREN) {
                AjsAstNode *c = ast_new(p, AST_CALL_EXPR, cur.line, cur.col);
                c->u.call.callee = base;
                c->u.call.optional = true;
                parse_arg_list(p, &c->u.call.args);
                base = c;
            } else if (p->lex.cur.kind == TOK_PRIV_IDENT) {
                AjsToken ptok = p_advance(p);
                AjsAstNode *prop = ast_new(p, AST_PRIV_IDENT, ptok.line, ptok.col);
                prop->u.ident.u16 = ptok.u16_data;
                prop->u.ident.len = ptok.u16_len;
                prop->u.ident.utf8 = ptok.utf8_text;
                record_priv_ref(p, prop);
                AjsAstNode *m = ast_new(p, AST_MEMBER_EXPR, cur.line, cur.col);
                m->u.member.computed = false;
                m->u.member.optional = true;
                m->u.member.object = base;
                m->u.member.property = prop;
                base = m;
            } else if (is_property_name_word(p->lex.cur.kind)) {
                AjsToken ptok = p_advance(p);
                AjsAstNode *m = ast_new(p, AST_MEMBER_EXPR, cur.line, cur.col);
                m->u.member.computed = false;
                m->u.member.optional = true;
                m->u.member.object = base;
                m->u.member.property = make_ident_from_tok(p, ptok);
                base = m;
            } else {
                parse_error_tok(p, p->lex.cur, "Invalid optional chain");
                return NULL;
            }
        } else if (cur.kind == TOK_TEMPLATE_NO_SUB || cur.kind == TOK_TEMPLATE_HEAD) {
            if (in_opt_chain) {
                parse_error_tok(p, cur, "Invalid tagged template on optional chain");
                return NULL;
            }
            check_no_cover_init(p, base);
            AjsAstNode *tt = ast_new(p, AST_TAGGED_TEMPLATE, cur.line, cur.col);
            tt->u.tagged_tpl.tag = base;
            tt->u.tagged_tpl.quasi = parse_template_literal(p, true);
            base = tt;
        } else {
            break;
        }
    }

    if (in_opt_chain && base) {
        AjsAstNode *oc = ast_new(p, AST_OPT_CHAIN_EXPR, base->line, base->col);
        oc->u.opt_chain.inner = base;
        return oc;
    }
    return base;
}

static AjsAstNode *parse_unary_expr(AjsParser *p) {
    AjsToken cur = p->lex.cur;
    if (cur.kind == KW_DELETE || cur.kind == KW_VOID || cur.kind == KW_TYPEOF ||
        cur.kind == TOK_PLUS || cur.kind == TOK_MINUS || cur.kind == TOK_BIT_NOT || cur.kind == TOK_BANG) {
        p_advance(p);
        AjsAstNode *arg = parse_unary_expr(p);
        check_no_cover_init(p, arg);
        if (cur.kind == KW_DELETE && arg) {
            if (p->strict && arg->kind == AST_IDENT) {
                parse_error_tok(p, cur, "Delete of an unqualified identifier in strict mode");
            }
            if (arg->kind == AST_MEMBER_EXPR && arg->u.member.property && arg->u.member.property->kind == AST_PRIV_IDENT) {
                parse_error_tok(p, cur, "Private fields can not be deleted");
            }
            if (arg->kind == AST_OPT_CHAIN_EXPR && arg->u.opt_chain.inner &&
                arg->u.opt_chain.inner->kind == AST_MEMBER_EXPR &&
                arg->u.opt_chain.inner->u.member.property &&
                arg->u.opt_chain.inner->u.member.property->kind == AST_PRIV_IDENT) {
                parse_error_tok(p, cur, "Private fields can not be deleted");
            }
        }
        AjsAstNode *u = ast_new(p, AST_UNARY_EXPR, cur.line, cur.col);
        u->u.unary.op = cur.kind;
        u->u.unary.prefix = true;
        u->u.unary.arg = arg;
        return u;
    }
    if (cur.kind == TOK_INC || cur.kind == TOK_DEC) {
        p_advance(p);
        AjsAstNode *arg = parse_unary_expr(p);
        check_no_cover_init(p, arg);
        if (!is_valid_simple_lhs(p, arg)) {
            parse_error_tok(p, cur, "Invalid left-hand side expression in prefix operation");
        }
        AjsAstNode *u = ast_new(p, AST_UPDATE_EXPR, cur.line, cur.col);
        u->u.unary.op = cur.kind;
        u->u.unary.prefix = true;
        u->u.unary.arg = arg;
        return u;
    }
    if (cur.kind == KW_AWAIT && (p->in_async || (p->is_module && !p->in_function))) {
        if (cur.has_escape) {
            parse_error_tok(p, cur, "Keyword must not contain escaped characters");
            return NULL;
        }
        if (p->in_formal_params || p->in_static_block) {
            parse_error_tok(p, cur, "'await' expression is not allowed here");
            return NULL;
        }
        if (!p->in_function && p->is_module) p->saw_top_await = true;
        p_advance(p);
        AjsAstNode *arg = parse_unary_expr(p);
        check_no_cover_init(p, arg);
        AjsAstNode *aw = ast_new(p, AST_AWAIT_EXPR, cur.line, cur.col);
        aw->u.expr_stmt.expr = arg;
        return aw;
    }

    AjsAstNode *expr = parse_lhs_expr(p, true);
    if (!p->lex.cur.had_newline && (p->lex.cur.kind == TOK_INC || p->lex.cur.kind == TOK_DEC)) {
        AjsToken op = p_advance(p);
        check_no_cover_init(p, expr);
        if (!is_valid_simple_lhs(p, expr)) {
            parse_error_tok(p, op, "Invalid left-hand side expression in postfix operation");
        }
        AjsAstNode *u = ast_new(p, AST_UPDATE_EXPR, op.line, op.col);
        u->u.unary.op = op.kind;
        u->u.unary.prefix = false;
        u->u.unary.arg = expr;
        return u;
    }
    return expr;
}

static AjsAstNode *parse_exp_expr(AjsParser *p) {
    AjsAstNode *left = parse_unary_expr(p);
    if (p->lex.cur.kind == TOK_EXP) {
        if (left && !left->parenthesized &&
            (left->kind == AST_UNARY_EXPR || left->kind == AST_AWAIT_EXPR)) {
            parse_error_at(p, left->line, left->col,
                           "Unary operator used immediately before exponentiation expression without parentheses");
        }
        AjsToken op = p_advance(p);
        check_no_cover_init(p, left);
        AjsAstNode *right = parse_exp_expr(p);
        check_no_cover_init(p, right);
        AjsAstNode *b = ast_new(p, AST_BINARY_EXPR, op.line, op.col);
        b->u.binary.op = TOK_EXP;
        b->u.binary.left = left;
        b->u.binary.right = right;
        return b;
    }
    return left;
}

static int binop_prec(AjsTokenKind k, bool allow_in) {
    switch (k) {
        case TOK_COALESCE: return 1;
        case TOK_OR:       return 2;
        case TOK_AND:      return 3;
        case TOK_BIT_OR:   return 4;
        case TOK_BIT_XOR:  return 5;
        case TOK_BIT_AND:  return 6;
        case TOK_EQ: case TOK_NEQ: case TOK_STRICT_EQ: case TOK_STRICT_NEQ: return 7;
        case TOK_LT: case TOK_LTE: case TOK_GT: case TOK_GTE: case KW_INSTANCEOF: return 8;
        case KW_IN:        return allow_in ? 8 : 0;
        case TOK_SHL: case TOK_SHR: case TOK_USHR: return 9;
        case TOK_PLUS: case TOK_MINUS: return 10;
        case TOK_MUL: case TOK_DIV: case TOK_MOD: return 11;
        default: return 0;
    }
}

static AjsAstNode *parse_binop_rhs(AjsParser *p, int min_prec, AjsAstNode *lhs) {
    while (!p->had_error) {
        AjsToken tok = p->lex.cur;
        int prec = binop_prec(tok.kind, p->allow_in);
        if (prec < min_prec) break;

        p_advance(p);
        check_no_cover_init(p, lhs);
        AjsAstNode *rhs = parse_exp_expr(p);
        check_no_cover_init(p, rhs);

        while (!p->had_error) {
            int next_prec = binop_prec(p->lex.cur.kind, p->allow_in);
            if (next_prec <= prec) break;
            rhs = parse_binop_rhs(p, next_prec, rhs);
        }

        /* ECMA-262: Nullish coalescing `??` cannot mix with unparenthesized `&&` or `||`! */
        if (tok.kind == TOK_COALESCE) {
            if ((lhs && !lhs->parenthesized && lhs->kind == AST_LOGICAL_EXPR &&
                 (lhs->u.binary.op == TOK_AND || lhs->u.binary.op == TOK_OR)) ||
                (rhs && !rhs->parenthesized && rhs->kind == AST_LOGICAL_EXPR &&
                 (rhs->u.binary.op == TOK_AND || rhs->u.binary.op == TOK_OR))) {
                parse_error_tok(p, tok, "Logical expressions and coalesce expressions cannot be mixed without parentheses");
            }
        } else if (tok.kind == TOK_AND || tok.kind == TOK_OR) {
            if ((lhs && !lhs->parenthesized && lhs->kind == AST_LOGICAL_EXPR && lhs->u.binary.op == TOK_COALESCE) ||
                (rhs && !rhs->parenthesized && rhs->kind == AST_LOGICAL_EXPR && rhs->u.binary.op == TOK_COALESCE)) {
                parse_error_tok(p, tok, "Logical expressions and coalesce expressions cannot be mixed without parentheses");
            }
        }

        AjsAstKind nk = (tok.kind == TOK_AND || tok.kind == TOK_OR || tok.kind == TOK_COALESCE)
                        ? AST_LOGICAL_EXPR : AST_BINARY_EXPR;
        AjsAstNode *b = ast_new(p, nk, tok.line, tok.col);
        b->u.binary.op = tok.kind;
        b->u.binary.left = lhs;
        b->u.binary.right = rhs;
        lhs = b;
    }
    return lhs;
}

static AjsAstNode *parse_binary_expr(AjsParser *p) {
    /* Ergonomic brand check `#priv in expr` (ES2022) */
    if (p->allow_in && p->lex.cur.kind == TOK_PRIV_IDENT) {
        AjsToken pk = ajs_lexer_peek(&p->lex);
        if (pk.kind == KW_IN) {
            AjsToken priv_tok = p_advance(p);
            AjsToken in_tok = p_advance(p);
            AjsAstNode *priv = ast_new(p, AST_PRIV_IDENT, priv_tok.line, priv_tok.col);
            priv->u.ident.u16 = priv_tok.u16_data;
            priv->u.ident.len = priv_tok.u16_len;
            priv->u.ident.utf8 = priv_tok.utf8_text;
            record_priv_ref(p, priv);

            AjsAstNode *rhs = parse_exp_expr(p);
            rhs = parse_binop_rhs(p, 9, rhs); /* Precedence above `in` (8) */
            AjsAstNode *b = ast_new(p, AST_BINARY_EXPR, in_tok.line, in_tok.col);
            b->u.binary.op = KW_IN;
            b->u.binary.left = priv;
            b->u.binary.right = rhs;
            return parse_binop_rhs(p, 1, b);
        }
    }
    AjsAstNode *lhs = parse_exp_expr(p);
    return parse_binop_rhs(p, 1, lhs);
}

static bool is_assign_op(AjsTokenKind k) {
    return k >= TOK_ASSIGN && k <= TOK_COALESCE_ASSIGN;
}

static AjsAstNode *parse_assign_expr(AjsParser *p) {
    if (p->in_generator && p->lex.cur.kind == KW_YIELD) {
        if (p->lex.cur.has_escape) {
            parse_error_tok(p, p->lex.cur, "Keyword must not contain escaped characters");
            return NULL;
        }
        if (p->in_formal_params) {
            parse_error_tok(p, p->lex.cur, "'yield' expression is not allowed in formal parameters");
            return NULL;
        }
        AjsToken ytok = p_advance(p);
        AjsAstNode *yn = ast_new(p, AST_YIELD_EXPR, ytok.line, ytok.col);
        if (!p->lex.cur.had_newline &&
            p->lex.cur.kind != TOK_SEMI && p->lex.cur.kind != TOK_RBRACE &&
            p->lex.cur.kind != TOK_RPAREN && p->lex.cur.kind != TOK_RBRACKET &&
            p->lex.cur.kind != TOK_COLON && p->lex.cur.kind != TOK_COMMA && p->lex.cur.kind != TOK_EOF) {
            if (p_match(p, TOK_MUL)) yn->u.yield_expr.delegate = true;
            yn->u.yield_expr.arg = parse_assign_expr(p);
            check_no_cover_init(p, yn->u.yield_expr.arg);
        }
        return yn;
    }

    AjsAstNode *left = parse_binary_expr(p);
    if (!left || p->had_error) return left;

    if (p_match(p, TOK_QUESTION)) {
        check_no_cover_init(p, left);
        bool saved_in = p->allow_in;
        p->allow_in = true;
        AjsAstNode *conseq = parse_assign_expr(p);
        check_no_cover_init(p, conseq);
        p->allow_in = saved_in;
        p_expect(p, TOK_COLON, "':'");
        AjsAstNode *alt = parse_assign_expr(p);
        check_no_cover_init(p, alt);
        AjsAstNode *c = ast_new(p, AST_COND_EXPR, left->line, left->col);
        c->u.cond.test = left;
        c->u.cond.conseq = conseq;
        c->u.cond.alt = alt;
        return c;
    }

    if (is_assign_op(p->lex.cur.kind)) {
        AjsToken op = p_advance(p);
        AjsAstNode *target = left;
        if (op.kind == TOK_ASSIGN && (left->kind == AST_ARRAY_EXPR || left->kind == AST_OBJECT_EXPR)) {
            target = expr_to_pattern(p, left, false);
        } else {
            check_no_cover_init(p, left);
            if (!is_valid_simple_lhs(p, left)) {
                parse_error_tok(p, op, "Invalid left-hand side in assignment");
            }
        }
        AjsAstNode *right = parse_assign_expr(p);
        check_no_cover_init(p, right);
        AjsAstNode *ae = ast_new(p, AST_ASSIGN_EXPR, op.line, op.col);
        ae->u.binary.op = op.kind;
        ae->u.binary.left = target;
        ae->u.binary.right = right;
        return ae;
    }

    check_no_cover_init(p, left);
    return left;
}

static AjsAstNode *parse_expr(AjsParser *p) {
    AjsAstNode *first = parse_assign_expr(p);
    if (p->lex.cur.kind != TOK_COMMA) return first;
    AjsAstNode *seq = ast_new(p, AST_SEQ_EXPR, first ? first->line : p->lex.cur.line, first ? first->col : 1);
    nodelist_push(p, &seq->u.seq.exprs, first);
    while (p_match(p, TOK_COMMA)) {
        nodelist_push(p, &seq->u.seq.exprs, parse_assign_expr(p));
    }
    return seq;
}
/* ============================================================================
 * Statements, Declarations, Functions, Classes, Modules & Program Entry
 * ============================================================================ */

static AjsAstNode *parse_func_ex(AjsParser *p, bool is_decl, bool allow_anon_decl,
                                 bool is_async, AjsFuncKind base_kind);
static AjsAstNode *parse_class_ex(AjsParser *p, bool is_decl, bool allow_anon_decl);

static bool expr_contains_forbidden_field_args(const AjsAstNode *n) {
    if (!n) return false;
    switch (n->kind) {
        case AST_IDENT:
            return u16_eq_ascii(n->u.ident.u16, n->u.ident.len, "arguments");
        case AST_FUNC_EXPR:
        case AST_FUNC_DECL:
            /* Non-arrow functions bind their own `arguments`; arrow functions inherit it! */
            if (n->u.func.fn_kind == AJS_FN_BC_ARROW || n->u.func.fn_kind == AJS_FN_BC_ASYNC_ARROW) {
                for (uint32_t i = 0; i < n->u.func.params.count; i++) {
                    if (expr_contains_forbidden_field_args(n->u.func.params.items[i])) return true;
                }
                return expr_contains_forbidden_field_args(n->u.func.body);
            }
            return false;
        case AST_CLASS_EXPR:
        case AST_CLASS_DECL:
            return expr_contains_forbidden_field_args(n->u.klass.super_expr);
        case AST_BLOCK:
            for (uint32_t i = 0; i < n->u.block.stmts.count; i++) {
                if (expr_contains_forbidden_field_args(n->u.block.stmts.items[i])) return true;
            }
            return false;
        case AST_EXPR_STMT:
        case AST_SPREAD_ELEM:
        case AST_REST_ELEMENT:
        case AST_AWAIT_EXPR:
        case AST_DYNAMIC_IMPORT:
            return expr_contains_forbidden_field_args(n->u.expr_stmt.expr);
        case AST_OPT_CHAIN_EXPR:
            return expr_contains_forbidden_field_args(n->u.opt_chain.inner);
        case AST_UNARY_EXPR:
        case AST_UPDATE_EXPR:
            return expr_contains_forbidden_field_args(n->u.unary.arg);
        case AST_BINARY_EXPR:
        case AST_LOGICAL_EXPR:
        case AST_ASSIGN_EXPR:
            return expr_contains_forbidden_field_args(n->u.binary.left) ||
                   expr_contains_forbidden_field_args(n->u.binary.right);
        case AST_ASSIGN_PATTERN:
            return expr_contains_forbidden_field_args(n->u.assign_pat.left) ||
                   expr_contains_forbidden_field_args(n->u.assign_pat.right);
        case AST_COND_EXPR:
            return expr_contains_forbidden_field_args(n->u.cond.test) ||
                   expr_contains_forbidden_field_args(n->u.cond.conseq) ||
                   expr_contains_forbidden_field_args(n->u.cond.alt);
        case AST_MEMBER_EXPR:
            return expr_contains_forbidden_field_args(n->u.member.object) ||
                   (n->u.member.computed && expr_contains_forbidden_field_args(n->u.member.property));
        case AST_CALL_EXPR:
        case AST_NEW_EXPR:
            if (expr_contains_forbidden_field_args(n->u.call.callee)) return true;
            for (uint32_t i = 0; i < n->u.call.args.count; i++) {
                if (expr_contains_forbidden_field_args(n->u.call.args.items[i])) return true;
            }
            return false;
        case AST_ARRAY_EXPR:
        case AST_ARRAY_PATTERN:
            for (uint32_t i = 0; i < n->u.array.elems.count; i++) {
                if (expr_contains_forbidden_field_args(n->u.array.elems.items[i])) return true;
            }
            return false;
        case AST_OBJECT_EXPR:
        case AST_OBJECT_PATTERN:
            for (uint32_t i = 0; i < n->u.object.props.count; i++) {
                AjsAstNode *pr = n->u.object.props.items[i];
                if (!pr) continue;
                if (pr->kind == AST_SPREAD_ELEM || pr->kind == AST_REST_ELEMENT) {
                    if (expr_contains_forbidden_field_args(pr)) return true;
                } else {
                    if (pr->u.prop.computed && expr_contains_forbidden_field_args(pr->u.prop.key)) return true;
                    if (expr_contains_forbidden_field_args(pr->u.prop.value)) return true;
                }
            }
            return false;
        case AST_SEQ_EXPR:
            for (uint32_t i = 0; i < n->u.seq.exprs.count; i++) {
                if (expr_contains_forbidden_field_args(n->u.seq.exprs.items[i])) return true;
            }
            return false;
        case AST_TEMPLATE_LIT:
            for (uint32_t i = 0; i < n->u.tpl.exprs.count; i++) {
                if (expr_contains_forbidden_field_args(n->u.tpl.exprs.items[i])) return true;
            }
            return false;
        case AST_TAGGED_TEMPLATE:
            return expr_contains_forbidden_field_args(n->u.tagged_tpl.tag) ||
                   expr_contains_forbidden_field_args(n->u.tagged_tpl.quasi);
        case AST_YIELD_EXPR:
            return expr_contains_forbidden_field_args(n->u.yield_expr.arg);
        default:
            return false;
    }
}

static AjsAstNode *parse_func_ex(AjsParser *p, bool is_decl, bool allow_anon_decl,
                                 bool is_async, AjsFuncKind base_kind) {
    AjsToken ftok = p->lex.cur;
    size_t src_start = ftok.start_off;
    p_expect(p, KW_FUNCTION, "'function'");
    bool is_gen = p_match(p, TOK_MUL);

    AjsFuncKind fk = base_kind;
    if (is_async && is_gen) fk = AJS_FN_BC_ASYNC_GENERATOR;
    else if (is_async) fk = AJS_FN_BC_ASYNC;
    else if (is_gen) fk = AJS_FN_BC_GENERATOR;

    AjsAstNode *fn = ast_new(p, is_decl ? AST_FUNC_DECL : AST_FUNC_EXPR, ftok.line, ftok.col);
    fn->u.func.fn_kind = fk;

    AjsToken name_tok;
    memset(&name_tok, 0, sizeof(name_tok));
    bool has_name = false;
    if (p->lex.cur.kind != TOK_LPAREN) {
        name_tok = p_advance(p);
        if (!check_binding_ident_tok(p, name_tok, false)) return fn;
        fn->u.func.name_u16 = name_tok.u16_data;
        fn->u.func.name_len = name_tok.u16_len;
        has_name = true;
    } else if (is_decl && !allow_anon_decl) {
        parse_error_tok(p, p->lex.cur, "Function statements require a function name");
        return fn;
    }

    if (is_decl && has_name) {
        if (p->is_module && !p->in_function && !p->scope->parent) {
            scope_declare_lex(p, name_tok.u16_data, name_tok.u16_len, name_tok.line, name_tok.col);
        } else if (!p->scope->is_func_root) {
            if (p->strict || is_async || is_gen) {
                scope_declare_lex(p, name_tok.u16_data, name_tok.u16_len, name_tok.line, name_tok.col);
            } else {
                /* Sloppy block Annex B.3.3: cannot collide with lexical binding in same block */
                uint32_t nh = ajs_hash_utf16(name_tok.u16_data, name_tok.u16_len);
                if (scope_find_lex_entry(p->scope, name_tok.u16_data, name_tok.u16_len, nh, false) >= 0) {
                    parse_error_tok(p, name_tok, "Identifier has already been lexically declared");
                }
                scope_add_raw(p, p->scope, name_tok.u16_data, name_tok.u16_len, false, false, false);
            }
        } else {
            scope_declare_var(p, name_tok.u16_data, name_tok.u16_len, name_tok.line, name_tok.col);
        }
    }

    bool saved_fn = p->in_function, saved_gen = p->in_generator, saved_async = p->in_async;
    bool saved_nt = p->allow_new_target, saved_sp = p->allow_super_prop, saved_sc = p->allow_super_call;
    bool saved_sb = p->in_static_block, saved_strict = p->strict;
    int saved_loop = p->loop_depth, saved_sw = p->switch_depth;
    uint32_t saved_lbl = p->label_count;

    p->in_function = true;
    p->in_generator = is_gen;
    p->in_async = is_async;
    p->allow_new_target = true;
    p->allow_super_prop = false;
    p->allow_super_call = false;
    p->in_static_block = false;
    p->loop_depth = 0;
    p->switch_depth = 0;

    scope_push(p, true, false);
    p_expect(p, TOK_LPAREN, "'('");
    bool saved_fp = p->in_formal_params;
    p->in_formal_params = true;
    while (!p->had_error && p->lex.cur.kind != TOK_RPAREN && p->lex.cur.kind != TOK_EOF) {
        if (p_match(p, TOK_ELLIPSIS)) {
            AjsAstNode *rest = ast_new(p, AST_REST_ELEMENT, p->lex.cur.line, p->lex.cur.col);
            rest->u.expr_stmt.expr = parse_binding_pattern_or_ident(p);
            nodelist_push(p, &fn->u.func.params, rest);
            if (p->lex.cur.kind == TOK_COMMA) {
                parse_error_tok(p, p->lex.cur, "Rest parameter must be last formal parameter");
            }
            break;
        }
        nodelist_push(p, &fn->u.func.params, parse_binding_elem(p));
        if (!p_match(p, TOK_COMMA)) break;
    }
    p->in_formal_params = saved_fp;
    p_expect(p, TOK_RPAREN, "')'");

    fn->u.func.has_simple_params = params_are_simple(&fn->u.func.params);

    AjsToken lb = p->lex.cur;
    p_expect(p, TOK_LBRACE, "'{'");
    AjsAstNode *blk = ast_new(p, AST_BLOCK, lb.line, lb.col);
    bool in_prologue = true, prologue_octal = false;
    while (!p->had_error && p->lex.cur.kind != TOK_RBRACE && p->lex.cur.kind != TOK_EOF) {
        AjsToken st_tok = p->lex.cur;
        AjsAstNode *st = parse_stmt(p, true);
        if (in_prologue && st && st->kind == AST_EXPR_STMT && st->u.expr_stmt.expr &&
            st->u.expr_stmt.expr->kind == AST_LITERAL_STR && !st->u.expr_stmt.expr->parenthesized) {
            if (st_tok.has_legacy_octal) prologue_octal = true;
            if (!st_tok.has_escape && !st_tok.has_legacy_octal &&
                u16_eq_ascii(st_tok.u16_data, st_tok.u16_len, "use strict")) {
                p->strict = true;
                p->lex.strict = true;
                if (!fn->u.func.has_simple_params) {
                    parse_error_tok(p, st_tok, "Illegal 'use strict' directive in function with non-simple parameter list");
                }
                if (prologue_octal) {
                    parse_error_tok(p, st_tok, "Octal escape sequences are not allowed in strict mode");
                }
            }
        } else {
            in_prologue = false;
        }
        nodelist_push(p, &blk->u.block.stmts, st);
    }
    size_t src_end = p->lex.cur.end_off;
    p_expect(p, TOK_RBRACE, "'}'");

    /* Re-validate function name & parameters if body enabled strict mode */
    if (p->strict && has_name) {
        if (is_strict_reserved_word(name_tok.u16_data, name_tok.u16_len) ||
            u16_eq_ascii(name_tok.u16_data, name_tok.u16_len, "eval") ||
            u16_eq_ascii(name_tok.u16_data, name_tok.u16_len, "arguments")) {
            parse_error_tok(p, name_tok, "Invalid function name in strict mode");
        }
    }
    bool forbid_dup_params = p->strict || is_gen || is_async || !fn->u.func.has_simple_params;
    AjsNodeList seen = {0, 0, NULL};
    for (uint32_t i = 0; i < fn->u.func.params.count; i++) {
        declare_pattern_names(p, fn->u.func.params.items[i], false, forbid_dup_params, &seen);
    }
    /* Check body lexical declarations do not collide with parameter names */
    for (uint32_t j = 0; j < seen.count && !p->had_error; j++) {
        AjsAstNode *pn = seen.items[j];
        uint32_t ph = ajs_hash_utf16(pn->u.ident.u16, pn->u.ident.len);
        if (scope_find_lex_entry(p->scope, pn->u.ident.u16, pn->u.ident.len, ph, false) >= 0) {
            parse_error_at(p, pn->line, pn->col, "Identifier has already been declared as parameter");
            break;
        }
    }

    fn->u.func.strict = p->strict;
    fn->u.func.body = blk;
    fn->u.func.src_utf8 = p->lex.src + src_start;
    fn->u.func.src_len = (src_end >= src_start) ? (src_end - src_start) : 0;

    scope_pop(p);
    p->in_function = saved_fn; p->in_generator = saved_gen; p->in_async = saved_async;
    p->allow_new_target = saved_nt; p->allow_super_prop = saved_sp; p->allow_super_call = saved_sc;
    p->in_static_block = saved_sb; p->strict = saved_strict; p->lex.strict = saved_strict;
    p->loop_depth = saved_loop; p->switch_depth = saved_sw; p->label_count = saved_lbl;
    return fn;
}

static AjsAstNode *parse_func(AjsParser *p, bool is_decl, bool is_async, AjsFuncKind base_kind) {
    return parse_func_ex(p, is_decl, false, is_async, base_kind);
}

static void priv_scope_declare(AjsParser *p, AjsPrivScope *ps, const uint16_t *u16, uint32_t len,
                               AjsPropKind kind, bool is_static, uint32_t line, uint32_t col) {
    if (u16_eq_ascii(u16, len, "constructor")) {
        parse_error_at(p, line, col, "Classes may not have a private element named '#constructor'");
        return;
    }
    uint8_t cur_bit = (kind == AJS_PROP_GET) ? 2u : ((kind == AJS_PROP_SET) ? 4u : 1u);
    uint8_t st_bit = is_static ? 8u : 0u;

    for (uint32_t i = 0; i < ps->count; i++) {
        if (u16_eq(ps->decls[i].u16, ps->decls[i].len, u16, len)) {
            uint8_t existing = ps->decls[i].mask;
            bool existing_static = (existing & 8u) != 0;
            uint8_t existing_kind = existing & 7u;
            /* Allowed ONLY if one is getter (2) and other is setter (4) with identical staticness */
            if (existing_static != is_static || (existing_kind & 1u) != 0 || (cur_bit & 1u) != 0 ||
                (existing_kind & cur_bit) != 0) {
                parse_error_at(p, line, col, "Duplicate private name declaration in class");
                return;
            }
            ps->decls[i].mask |= cur_bit;
            return;
        }
    }
    if (ps->count >= ps->cap) {
        uint32_t ncap = ps->cap ? ps->cap * 2u : 8u;
        AjsPrivDecl *nd = (AjsPrivDecl *)ajs_arena_alloc(p->arena, ncap * sizeof(AjsPrivDecl));
        ajs_memcpy(nd, ps->decls, ps->count * sizeof(AjsPrivDecl));
        ps->decls = nd;
        ps->cap = ncap;
    }
    ps->decls[ps->count].u16 = u16;
    ps->decls[ps->count].len = len;
    ps->decls[ps->count].mask = (uint8_t)(cur_bit | st_bit);
    ps->count++;
}

static AjsAstNode *parse_class_ex(AjsParser *p, bool is_decl, bool allow_anon_decl) {
    AjsToken ctok = p_advance(p); /* KW_CLASS */
    size_t src_start = ctok.start_off;
    AjsAstNode *kl = ast_new(p, is_decl ? AST_CLASS_DECL : AST_CLASS_EXPR, ctok.line, ctok.col);

    bool saved_strict = p->strict;
    p->strict = true;
    p->lex.strict = true;

    if (p->lex.cur.kind != KW_EXTENDS && p->lex.cur.kind != TOK_LBRACE) {
        AjsToken ntok = p_advance(p);
        if (!check_binding_ident_tok(p, ntok, false)) {
            p->strict = saved_strict; p->lex.strict = saved_strict;
            return kl;
        }
        kl->u.klass.name_u16 = ntok.u16_data;
        kl->u.klass.name_len = ntok.u16_len;
        if (is_decl) {
            /* Declare class binding in enclosing scope before class body */
            p->strict = saved_strict;
            scope_declare_lex(p, ntok.u16_data, ntok.u16_len, ntok.line, ntok.col);
            p->strict = true;
        }
    } else if (is_decl && !allow_anon_decl) {
        parse_error_tok(p, p->lex.cur, "Class declaration requires a name");
        p->strict = saved_strict; p->lex.strict = saved_strict;
        return kl;
    }

    if (p_match(p, KW_EXTENDS)) {
        kl->u.klass.super_expr = parse_lhs_expr(p, true);
        check_no_cover_init(p, kl->u.klass.super_expr);
    }

    AjsPrivScope priv_sc;
    memset(&priv_sc, 0, sizeof(priv_sc));
    priv_sc.parent = p->priv_scope;
    p->priv_scope = &priv_sc;

    p_expect(p, TOK_LBRACE, "'{'");
    bool has_ctor = false;

    while (!p->had_error && p->lex.cur.kind != TOK_RBRACE && p->lex.cur.kind != TOK_EOF) {
        if (p_match(p, TOK_SEMI)) continue;

        size_t elem_off = p->lex.cur.start_off;
        AjsAstNode *elem = ast_new(p, AST_CLASS_ELEM, p->lex.cur.line, p->lex.cur.col);
        bool is_static = false;
        bool is_async = false;
        bool is_gen = false;

        /* Check `static` modifier vs `static` method/field name vs `static { ... }` block */
        if (p->lex.cur.kind == KW_STATIC && !p->lex.cur.has_escape) {
            AjsToken pk = ajs_lexer_peek(&p->lex);
            if (pk.kind == TOK_LBRACE) {
                p_advance(p); /* KW_STATIC */
                AjsToken lb = p_advance(p); /* '{' */
                elem->u.prop.kind = AJS_PROP_STATIC_BLOCK;
                elem->u.prop.is_static = true;

                bool s_fn = p->in_function, s_gen = p->in_generator, s_as = p->in_async;
                bool s_nt = p->allow_new_target, s_sp = p->allow_super_prop, s_sc = p->allow_super_call;
                bool s_sb = p->in_static_block;
                int s_lp = p->loop_depth, s_sw = p->switch_depth;
                uint32_t s_lb = p->label_count;

                p->in_function = false;
                p->in_generator = false;
                p->in_async = false;
                p->in_static_block = true;
                p->allow_new_target = true;
                p->allow_super_prop = true;
                p->allow_super_call = false;
                p->loop_depth = 0;
                p->switch_depth = 0;

                scope_push(p, true, false);
                AjsAstNode *blk = ast_new(p, AST_BLOCK, lb.line, lb.col);
                while (!p->had_error && p->lex.cur.kind != TOK_RBRACE && p->lex.cur.kind != TOK_EOF) {
                    nodelist_push(p, &blk->u.block.stmts, parse_stmt(p, true));
                }
                p_expect(p, TOK_RBRACE, "'}'");
                scope_pop(p);

                p->in_function = s_fn; p->in_generator = s_gen; p->in_async = s_as;
                p->allow_new_target = s_nt; p->allow_super_prop = s_sp; p->allow_super_call = s_sc;
                p->in_static_block = s_sb; p->loop_depth = s_lp; p->switch_depth = s_sw;
                p->label_count = s_lb;

                elem->u.prop.value = blk;
                nodelist_push(p, &kl->u.klass.elements, elem);
                continue;
            }
            if (pk.kind != TOK_LPAREN && pk.kind != TOK_ASSIGN && pk.kind != TOK_SEMI && pk.kind != TOK_RBRACE) {
                p_advance(p);
                is_static = true;
            }
        }
        elem->u.prop.is_static = is_static;

        /* Check `async`, `*`, `get`, `set` */
        if (p->lex.cur.kind == TOK_IDENT && !p->lex.cur.has_escape &&
            u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "async")) {
            AjsToken pk = ajs_lexer_peek(&p->lex);
            if (!pk.had_newline && pk.kind != TOK_LPAREN && pk.kind != TOK_ASSIGN &&
                pk.kind != TOK_SEMI && pk.kind != TOK_RBRACE) {
                p_advance(p);
                is_async = true;
            }
        }
        if (p_match(p, TOK_MUL)) is_gen = true;

        elem->u.prop.kind = AJS_PROP_METHOD;
        if (!is_async && !is_gen && p->lex.cur.kind == TOK_IDENT && !p->lex.cur.has_escape &&
            (u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "get") ||
             u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "set"))) {
            AjsToken pk = ajs_lexer_peek(&p->lex);
            if (pk.kind != TOK_LPAREN && pk.kind != TOK_ASSIGN && pk.kind != TOK_SEMI && pk.kind != TOK_RBRACE) {
                bool is_get = u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "get");
                p_advance(p);
                elem->u.prop.kind = is_get ? AJS_PROP_GET : AJS_PROP_SET;
            }
        }

        /* Element name */
        AjsToken ktok = p->lex.cur;
        bool is_literal_ctor_name = false;
        bool is_literal_proto_name = false;
        if (p_match(p, TOK_LBRACKET)) {
            elem->u.prop.computed = true;
            bool saved_in = p->allow_in;
            p->allow_in = true;
            elem->u.prop.key = parse_assign_expr(p);
            p->allow_in = saved_in;
            check_no_cover_init(p, elem->u.prop.key);
            p_expect(p, TOK_RBRACKET, "']'");
        } else if (ktok.kind == TOK_PRIV_IDENT) {
            p_advance(p);
            elem->u.prop.is_private = true;
            AjsAstNode *pn = ast_new(p, AST_PRIV_IDENT, ktok.line, ktok.col);
            pn->u.ident.u16 = ktok.u16_data;
            pn->u.ident.len = ktok.u16_len;
            pn->u.ident.utf8 = ktok.utf8_text;
            elem->u.prop.key = pn;
        } else if (ktok.kind == TOK_STRING) {
            p_advance(p);
            is_literal_ctor_name = u16_eq_ascii(ktok.u16_data, ktok.u16_len, "constructor");
            is_literal_proto_name = u16_eq_ascii(ktok.u16_data, ktok.u16_len, "prototype");
            AjsAstNode *ks = ast_new(p, AST_LITERAL_STR, ktok.line, ktok.col);
            ks->u.lit.s_u16 = ktok.u16_data; ks->u.lit.s_len = ktok.u16_len;
            elem->u.prop.key = ks;
        } else if (ktok.kind == TOK_NUMBER) {
            p_advance(p);
            AjsAstNode *kn = ast_new(p, AST_LITERAL_NUM, ktok.line, ktok.col);
            kn->u.lit.nval = ktok.num_val;
            elem->u.prop.key = kn;
        } else if (ktok.kind == TOK_BIGINT) {
            p_advance(p);
            AjsAstNode *kb = ast_new(p, AST_LITERAL_BIGINT, ktok.line, ktok.col);
            kb->u.lit.bi_utf8 = ktok.utf8_text; kb->u.lit.bi_len = ktok.utf8_len;
            elem->u.prop.key = kb;
        } else if (is_property_name_word(ktok.kind)) {
            p_advance(p);
            is_literal_ctor_name = u16_eq_ascii(ktok.u16_data, ktok.u16_len, "constructor");
            is_literal_proto_name = u16_eq_ascii(ktok.u16_data, ktok.u16_len, "prototype");
            elem->u.prop.key = make_ident_from_tok(p, ktok);
        } else {
            parse_error_tok(p, ktok, "Unexpected token in class body");
            break;
        }

        if (is_static && is_literal_proto_name) {
            parse_error_tok(p, ktok, "Classes may not have a static property named 'prototype'");
        }

        if (p->lex.cur.kind == TOK_LPAREN) {
            /* Method, Constructor, Getter, or Setter */
            if (elem->u.prop.is_private) {
                priv_scope_declare(p, &priv_sc, ktok.u16_data, ktok.u16_len,
                                   elem->u.prop.kind, is_static, ktok.line, ktok.col);
            }
            bool is_class_ctor = (!is_static && !elem->u.prop.computed && !elem->u.prop.is_private && is_literal_ctor_name);
            if (is_class_ctor) {
                if (is_async || is_gen || elem->u.prop.kind != AJS_PROP_METHOD) {
                    parse_error_tok(p, ktok, "Class constructor may not be an accessor, async, or generator");
                }
                if (has_ctor) {
                    parse_error_tok(p, ktok, "A class may only have one constructor");
                }
                has_ctor = true;
            }
            AjsAstNode *mfn = parse_method_body_node(p, elem_off, is_async, is_gen, elem->u.prop.kind,
                                                     is_class_ctor && (kl->u.klass.super_expr != NULL));
            if (is_class_ctor && mfn) {
                mfn->u.func.fn_kind = AJS_FN_BC_CLASS_CTOR;
            }
            elem->u.prop.value = mfn;
        } else {
            /* Class Field Definition */
            if (is_async || is_gen || elem->u.prop.kind != AJS_PROP_METHOD) {
                parse_error_tok(p, p->lex.cur, "Expected '(' for class method or accessor");
                break;
            }
            if (is_literal_ctor_name) {
                parse_error_tok(p, ktok, "Classes may not have a field named 'constructor'");
            }
            elem->u.prop.kind = AJS_PROP_FIELD;
            if (elem->u.prop.is_private) {
                priv_scope_declare(p, &priv_sc, ktok.u16_data, ktok.u16_len,
                                   AJS_PROP_FIELD, is_static, ktok.line, ktok.col);
            }
            if (p_match(p, TOK_ASSIGN)) {
                bool s_nt = p->allow_new_target, s_sp = p->allow_super_prop, s_sc = p->allow_super_call;
                bool s_in = p->allow_in, s_fn = p->in_function, s_as = p->in_async, s_gn = p->in_generator;
                p->allow_new_target = true;
                p->allow_super_prop = true;
                p->allow_super_call = false;
                p->allow_in = true;
                p->in_function = true;
                p->in_async = false;
                p->in_generator = false;

                AjsAstNode *init = parse_assign_expr(p);
                check_no_cover_init(p, init);
                if (expr_contains_forbidden_field_args(init)) {
                    parse_error_at(p, init->line, init->col, "'arguments' is not allowed in class field initializer");
                }
                elem->u.prop.value = init;

                p->allow_new_target = s_nt; p->allow_super_prop = s_sp; p->allow_super_call = s_sc;
                p->allow_in = s_in; p->in_function = s_fn; p->in_async = s_as; p->in_generator = s_gn;
            }
            p_consume_semicolon(p);
        }

        nodelist_push(p, &kl->u.klass.elements, elem);
    }

    size_t src_end = p->lex.cur.end_off;
    p_expect(p, TOK_RBRACE, "'}'");
    kl->u.klass.src_utf8 = p->lex.src + src_start;
    kl->u.klass.src_len = (src_end >= src_start) ? (src_end - src_start) : 0;

    /* Validate all private identifier references collected inside this class */
    for (uint32_t i = 0; i < priv_sc.ref_count && !p->had_error; i++) {
        AjsAstNode *ref = priv_sc.refs[i];
        bool found = false;
        for (uint32_t j = 0; j < priv_sc.count; j++) {
            if (u16_eq(priv_sc.decls[j].u16, priv_sc.decls[j].len, ref->u.ident.u16, ref->u.ident.len)) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (priv_sc.parent) {
                /* Defer to outer enclosing class scope! */
                p->priv_scope = priv_sc.parent;
                record_priv_ref(p, ref);
            } else {
                parse_error_at(p, ref->line, ref->col,
                               "Private field '#%s' must be declared in an enclosing class",
                               ref->u.ident.utf8 ? ref->u.ident.utf8 : "private");
            }
        }
    }

    p->priv_scope = priv_sc.parent;
    p->strict = saved_strict;
    p->lex.strict = saved_strict;
    return kl;
}

static AjsAstNode *parse_class(AjsParser *p, bool is_decl) {
    return parse_class_ex(p, is_decl, false);
}

/* ============================================================================
 * Variable Declarations, Loops, Switch, Try/Catch/Finally & Statements
 * ============================================================================ */

static AjsAstNode *parse_var_decl_list(AjsParser *p, AjsVarKind vkind, bool in_for_head) {
    AjsToken start = p_advance(p); /* KW_VAR / KW_LET / KW_CONST / using / KW_AWAIT */
    if (vkind == AJS_VAR_AWAIT_USING) {
        if (p->in_static_block) {
            parse_error_tok(p, start, "'await using' is not allowed in class static block");
        }
        if (!p->in_function && p->is_module) {
            p->saw_top_await = true;
        }
        p_advance(p); /* contextual 'using' */
    }
    AjsAstNode *vd = ast_new(p, AST_VAR_DECL, start.line, start.col);
    vd->u.var_decl.var_kind = vkind;

    while (!p->had_error) {
        AjsAstNode *pat = parse_binding_pattern_or_ident(p);
        if (!pat || p->had_error) break;
        if ((vkind == AJS_VAR_USING || vkind == AJS_VAR_AWAIT_USING) && pat->kind != AST_IDENT) {
            parse_error_at(p, pat->line, pat->col, "Destructuring pattern is not allowed in 'using' declaration");
        }
        declare_pattern_names(p, pat, vkind != AJS_VAR_VAR, false, NULL);

        AjsAstNode *decl = ast_new(p, AST_VAR_DECLARATOR, pat->line, pat->col);
        decl->u.declarator.id = pat;
        if (p_match(p, TOK_ASSIGN)) {
            decl->u.declarator.init = parse_assign_expr(p);
            check_no_cover_init(p, decl->u.declarator.init);
        } else if (!in_for_head || (p->lex.cur.kind != KW_IN &&
                   !(p->lex.cur.kind == TOK_IDENT && !p->lex.cur.has_escape &&
                     u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "of")))) {
            if (vkind == AJS_VAR_CONST || vkind == AJS_VAR_USING || vkind == AJS_VAR_AWAIT_USING) {
                parse_error_at(p, pat->line, pat->col, "Missing initializer in const/using declaration");
            } else if (pat->kind != AST_IDENT) {
                parse_error_at(p, pat->line, pat->col, "Missing initializer in destructuring declaration");
            }
        }
        nodelist_push(p, &vd->u.var_decl.decls, decl);
        if (!p_match(p, TOK_COMMA)) break;
    }
    return vd;
}

static bool is_let_lexical_decl_start(AjsParser *p) {
    if (p->lex.cur.kind != KW_LET || p->lex.cur.has_escape) return false;
    if (p->strict) return true;
    AjsToken pk = ajs_lexer_peek(&p->lex);
    if (pk.kind == TOK_LBRACE || pk.kind == TOK_LBRACKET) return true;
    if (pk.kind == TOK_IDENT || pk.kind == KW_YIELD || pk.kind == KW_AWAIT ||
        pk.kind == KW_LET || pk.kind == KW_STATIC) {
        return true;
    }
    return false;
}

static bool is_using_decl_start(AjsParser *p, bool in_for_head) {
    AjsToken cur = p->lex.cur;
    if (cur.kind != TOK_IDENT || cur.has_escape || !u16_eq_ascii(cur.u16_data, cur.u16_len, "using")) {
        return false;
    }
    AjsLexer saved = p->lex;
    AjsToken t1 = ajs_lexer_next(&p->lex);
    bool ok = false;
    if (!t1.had_newline && t1.kind != KW_LET &&
        (is_ident_reference_tok(p, t1) || t1.kind == TOK_LBRACE)) {
        if (!in_for_head) {
            ok = true;
        } else if (t1.kind == TOK_LBRACE) {
            ok = true;
        } else {
            AjsToken t2 = ajs_lexer_next(&p->lex);
            if (t2.kind == TOK_ASSIGN || t2.kind == TOK_COMMA || t2.kind == TOK_SEMI || t2.kind == KW_IN ||
                (t2.kind == TOK_IDENT && !t2.has_escape && u16_eq_ascii(t2.u16_data, t2.u16_len, "of"))) {
                ok = true;
            }
        }
    }
    p->lex = saved;
    return ok;
}

static bool is_await_using_decl_start(AjsParser *p, bool in_for_head) {
    if (p->lex.cur.kind != KW_AWAIT || p->lex.cur.has_escape) return false;
    if (!p->in_async && !(p->is_module && !p->in_function)) return false;
    AjsLexer saved = p->lex;
    AjsToken t1 = ajs_lexer_next(&p->lex);
    bool ok = false;
    if (!t1.had_newline) {
        ok = is_using_decl_start(p, in_for_head);
    }
    p->lex = saved;
    return ok;
}

static AjsAstNode *parse_for_stmt(AjsParser *p) {
    AjsToken ftok = p_advance(p); /* KW_FOR */
    bool is_await = false;
    if (p->lex.cur.kind == KW_AWAIT && (p->in_async || (p->is_module && !p->in_function))) {
        if (p->in_static_block) {
            parse_error_tok(p, p->lex.cur, "'for await' is not allowed in class static block");
        }
        if (!p->in_function && p->is_module) p->saw_top_await = true;
        p_advance(p);
        is_await = true;
    }

    p_expect(p, TOK_LPAREN, "'(' after 'for'");
    scope_push(p, false, false);

    AjsAstNode *init = NULL;
    bool init_is_decl = false;
    bool saved_in = p->allow_in;
    p->allow_in = false;

    if (p->lex.cur.kind == TOK_SEMI) {
        if (is_await) parse_error_tok(p, p->lex.cur, "Expected 'of' in 'for await' loop");
    } else if (p->lex.cur.kind == KW_VAR) {
        init = parse_var_decl_list(p, AJS_VAR_VAR, true);
        init_is_decl = true;
    } else if (p->lex.cur.kind == KW_CONST) {
        init = parse_var_decl_list(p, AJS_VAR_CONST, true);
        init_is_decl = true;
    } else if (is_await_using_decl_start(p, true)) {
        init = parse_var_decl_list(p, AJS_VAR_AWAIT_USING, true);
        init_is_decl = true;
    } else if (is_using_decl_start(p, true)) {
        init = parse_var_decl_list(p, AJS_VAR_USING, true);
        init_is_decl = true;
    } else if (is_let_lexical_decl_start(p)) {
        init = parse_var_decl_list(p, AJS_VAR_LET, true);
        init_is_decl = true;
    } else {
        /* Note: don't call check_no_cover_init yet — `for ({a = 1} of xs)` is valid destructuring! */
        init = parse_assign_expr(p);
        if (p->lex.cur.kind == TOK_COMMA) {
            check_no_cover_init(p, init);
            AjsAstNode *seq = ast_new(p, AST_SEQ_EXPR, init ? init->line : ftok.line, init ? init->col : ftok.col);
            nodelist_push(p, &seq->u.seq.exprs, init);
            while (p_match(p, TOK_COMMA)) {
                AjsAstNode *nx = parse_assign_expr(p);
                check_no_cover_init(p, nx);
                nodelist_push(p, &seq->u.seq.exprs, nx);
            }
            init = seq;
        }
    }
    p->allow_in = saved_in;

    bool is_in = (!is_await && p->lex.cur.kind == KW_IN);
    bool is_of = (p->lex.cur.kind == TOK_IDENT && !p->lex.cur.has_escape &&
                  u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "of"));

    if (is_in || is_of) {
        p_advance(p);
        if (!init) {
            parse_error_tok(p, ftok, "Invalid left-hand side in for-in/of loop");
        } else if (init_is_decl) {
            if (is_in && (init->u.var_decl.var_kind == AJS_VAR_USING ||
                          init->u.var_decl.var_kind == AJS_VAR_AWAIT_USING)) {
                parse_error_at(p, init->line, init->col, "'using' declaration is not allowed in for-in statement");
            }
            if (init->u.var_decl.decls.count != 1) {
                parse_error_at(p, init->line, init->col, "Only a single variable declaration is allowed in for-in/of");
            } else {
                AjsAstNode *d0 = init->u.var_decl.decls.items[0];
                if (d0->u.declarator.init != NULL) {
                    /* Annex B allows `for (var x = init in obj)` ONLY in sloppy mode for simple identifier `var` and `in`! */
                    if (is_of || p->strict || init->u.var_decl.var_kind != AJS_VAR_VAR ||
                        d0->u.declarator.id->kind != AST_IDENT) {
                        parse_error_at(p, d0->line, d0->col, "for-%s loop variable declaration may not have an initializer",
                                       is_of ? "of" : "in");
                    }
                }
            }
        } else {
            if (init->kind == AST_ARRAY_EXPR || init->kind == AST_OBJECT_EXPR) {
                init = expr_to_pattern(p, init, false);
            } else {
                check_no_cover_init(p, init);
                if (!is_valid_simple_lhs(p, init)) {
                    parse_error_at(p, init->line, init->col, "Invalid left-hand side in for-%s loop",
                                   is_of ? "of" : "in");
                }
            }
        }

        AjsAstNode *right = is_of ? parse_assign_expr(p) : parse_expr(p);
        check_no_cover_init(p, right);
        p_expect(p, TOK_RPAREN, "')'");

        p->loop_depth++;
        AjsAstNode *body = parse_stmt(p, false);
        p->loop_depth--;
        scope_pop(p);

        AjsAstNode *fio = ast_new(p, AST_FOR_IN_OF_STMT, ftok.line, ftok.col);
        fio->u.for_in_of.is_of = is_of;
        fio->u.for_in_of.is_await = is_await;
        fio->u.for_in_of.left = init;
        fio->u.for_in_of.right = right;
        fio->u.for_in_of.body = body;
        return fio;
    }

    if (is_await) {
        parse_error_tok(p, p->lex.cur, "Expected 'of' in 'for await' statement");
    }
    if (!init_is_decl && init) check_no_cover_init(p, init);

    p_expect(p, TOK_SEMI, "';' in for statement");
    AjsAstNode *test = NULL;
    if (p->lex.cur.kind != TOK_SEMI) {
        test = parse_expr(p);
        check_no_cover_init(p, test);
    }
    p_expect(p, TOK_SEMI, "';' in for statement");
    AjsAstNode *update = NULL;
    if (p->lex.cur.kind != TOK_RPAREN) {
        update = parse_expr(p);
        check_no_cover_init(p, update);
    }
    p_expect(p, TOK_RPAREN, "')'");

    p->loop_depth++;
    AjsAstNode *body = parse_stmt(p, false);
    p->loop_depth--;
    scope_pop(p);

    AjsAstNode *fs = ast_new(p, AST_FOR_STMT, ftok.line, ftok.col);
    fs->u.for_stmt.init = init;
    fs->u.for_stmt.test = test;
    fs->u.for_stmt.update = update;
    fs->u.for_stmt.body = body;
    return fs;
}

static void parse_optional_import_attributes(AjsParser *p) {
    /* ES2025/2026 Import Attributes: `with { key: "val", ... }` */
    if (p->lex.cur.kind == KW_WITH && !p->lex.cur.had_newline) {
        p_advance(p);
        p_expect(p, TOK_LBRACE, "'{' after 'with'");
        while (!p->had_error && p->lex.cur.kind != TOK_RBRACE && p->lex.cur.kind != TOK_EOF) {
            if (p->lex.cur.kind == TOK_STRING || is_property_name_word(p->lex.cur.kind)) {
                p_advance(p);
            } else {
                parse_error_tok(p, p->lex.cur, "Invalid import attribute key");
                break;
            }
            p_expect(p, TOK_COLON, "':' in import attributes");
            p_expect(p, TOK_STRING, "string literal attribute value");
            if (!p_match(p, TOK_COMMA)) break;
        }
        p_expect(p, TOK_RBRACE, "'}' after import attributes");
    }
}

static void record_exported_name(AjsParser *p, const uint16_t *u16, uint32_t len, uint32_t line, uint32_t col) {
    for (uint32_t i = 0; i < p->export_count; i++) {
        if (u16_eq(p->exported_names[i].u16_data, p->exported_names[i].u16_len, u16, len)) {
            parse_error_at(p, line, col, "Duplicate export of name");
            return;
        }
    }
    if (p->export_count >= p->export_cap) {
        uint32_t ncap = p->export_cap ? p->export_cap * 2u : 16u;
        AjsToken *ne = (AjsToken *)ajs_arena_alloc(p->arena, ncap * sizeof(AjsToken));
        ajs_memcpy(ne, p->exported_names, p->export_count * sizeof(AjsToken));
        p->exported_names = ne;
        p->export_cap = ncap;
    }
    AjsToken t;
    memset(&t, 0, sizeof(t));
    t.u16_data = u16;
    t.u16_len = len;
    t.line = line;
    t.col = col;
    p->exported_names[p->export_count++] = t;
}

static void record_exported_pattern(AjsParser *p, AjsAstNode *pat) {
    if (!pat) return;
    switch (pat->kind) {
        case AST_IDENT:
            record_exported_name(p, pat->u.ident.u16, pat->u.ident.len, pat->line, pat->col);
            break;
        case AST_ASSIGN_PATTERN:
            record_exported_pattern(p, pat->u.assign_pat.left);
            break;
        case AST_REST_ELEMENT:
            record_exported_pattern(p, pat->u.expr_stmt.expr);
            break;
        case AST_ARRAY_PATTERN:
            for (uint32_t i = 0; i < pat->u.array.elems.count; i++) {
                record_exported_pattern(p, pat->u.array.elems.items[i]);
            }
            break;
        case AST_OBJECT_PATTERN:
            for (uint32_t i = 0; i < pat->u.object.props.count; i++) {
                AjsAstNode *pr = pat->u.object.props.items[i];
                if (pr->kind == AST_REST_ELEMENT) record_exported_pattern(p, pr);
                else record_exported_pattern(p, pr->u.prop.value);
            }
            break;
        default:
            break;
    }
}

static AjsAstNode *parse_import_decl(AjsParser *p) {
    AjsToken itok = p_advance(p); /* KW_IMPORT */
    AjsAstNode *imp = ast_new(p, AST_IMPORT_DECL, itok.line, itok.col);

    /* `import "spec";` */
    if (p->lex.cur.kind == TOK_STRING) {
        AjsToken st = p_advance(p);
        imp->u.mod_decl.source_u16 = st.u16_data;
        imp->u.mod_decl.source_len = st.u16_len;
        parse_optional_import_attributes(p);
        p_consume_semicolon(p);
        return imp;
    }

    bool need_clause = true;
    /* Default import binding: `import def ...` */
    if (p->lex.cur.kind != TOK_MUL && p->lex.cur.kind != TOK_LBRACE) {
        AjsToken dtok = p_advance(p);
        if (!check_binding_ident_tok(p, dtok, false)) return imp;
        scope_declare_lex(p, dtok.u16_data, dtok.u16_len, dtok.line, dtok.col);
        static const uint16_t u_def[7] = {'d','e','f','a','u','l','t'};
        AjsAstNode *sp = ast_new(p, AST_SPECIFIER, dtok.line, dtok.col);
        sp->u.spec.imported_u16 = u_def;
        sp->u.spec.imported_len = 7;
        sp->u.spec.local_u16 = dtok.u16_data;
        sp->u.spec.local_len = dtok.u16_len;
        nodelist_push(p, &imp->u.mod_decl.specifiers, sp);
        need_clause = p_match(p, TOK_COMMA);
    }

    if (need_clause) {
        if (p_match(p, TOK_MUL)) {
            AjsToken as_tok = p_advance(p);
            if (as_tok.kind != TOK_IDENT || as_tok.has_escape || !u16_eq_ascii(as_tok.u16_data, as_tok.u16_len, "as")) {
                parse_error_tok(p, as_tok, "Expected 'as' after '*'");
                return imp;
            }
            AjsToken nstok = p_advance(p);
            if (!check_binding_ident_tok(p, nstok, false)) return imp;
            scope_declare_lex(p, nstok.u16_data, nstok.u16_len, nstok.line, nstok.col);
            AjsAstNode *sp = ast_new(p, AST_SPECIFIER, nstok.line, nstok.col);
            sp->u.spec.is_namespace = true;
            sp->u.spec.local_u16 = nstok.u16_data;
            sp->u.spec.local_len = nstok.u16_len;
            nodelist_push(p, &imp->u.mod_decl.specifiers, sp);
        } else if (p_match(p, TOK_LBRACE)) {
            while (!p->had_error && p->lex.cur.kind != TOK_RBRACE && p->lex.cur.kind != TOK_EOF) {
                AjsToken imp_tok = p->lex.cur;
                bool is_str_export = (imp_tok.kind == TOK_STRING);
                if (is_str_export || is_property_name_word(imp_tok.kind)) {
                    p_advance(p);
                } else {
                    parse_error_tok(p, imp_tok, "Expected import specifier");
                    break;
                }
                AjsToken loc_tok = imp_tok;
                if (p->lex.cur.kind == TOK_IDENT && !p->lex.cur.has_escape &&
                    u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "as")) {
                    p_advance(p);
                    loc_tok = p_advance(p);
                } else if (is_str_export) {
                    parse_error_tok(p, p->lex.cur, "Expected 'as' after string module export name");
                    break;
                }
                if (!check_binding_ident_tok(p, loc_tok, false)) break;
                scope_declare_lex(p, loc_tok.u16_data, loc_tok.u16_len, loc_tok.line, loc_tok.col);
                AjsAstNode *sp = ast_new(p, AST_SPECIFIER, imp_tok.line, imp_tok.col);
                sp->u.spec.imported_u16 = imp_tok.u16_data;
                sp->u.spec.imported_len = imp_tok.u16_len;
                sp->u.spec.local_u16 = loc_tok.u16_data;
                sp->u.spec.local_len = loc_tok.u16_len;
                nodelist_push(p, &imp->u.mod_decl.specifiers, sp);
                if (!p_match(p, TOK_COMMA)) break;
            }
            p_expect(p, TOK_RBRACE, "'}'");
        } else {
            parse_error_tok(p, p->lex.cur, "Expected '*' or '{' in import clause");
            return imp;
        }
    }

    AjsToken from_tok = p_advance(p);
    if (from_tok.kind != TOK_IDENT || from_tok.has_escape || !u16_eq_ascii(from_tok.u16_data, from_tok.u16_len, "from")) {
        parse_error_tok(p, from_tok, "Expected 'from' in import declaration");
        return imp;
    }
    AjsToken src_tok = p->lex.cur;
    if (!p_expect(p, TOK_STRING, "module specifier string")) return imp;
    imp->u.mod_decl.source_u16 = src_tok.u16_data;
    imp->u.mod_decl.source_len = src_tok.u16_len;
    parse_optional_import_attributes(p);
    p_consume_semicolon(p);
    return imp;
}

static AjsAstNode *parse_export_decl(AjsParser *p) {
    AjsToken etok = p_advance(p); /* KW_EXPORT */
    static const uint16_t u_def[7] = {'d','e','f','a','u','l','t'};

    if (p_match(p, KW_DEFAULT)) {
        record_exported_name(p, u_def, 7, etok.line, etok.col);
        AjsAstNode *ed = ast_new(p, AST_EXPORT_DEFAULT_DECL, etok.line, etok.col);
        if (p->lex.cur.kind == KW_FUNCTION) {
            ed->u.mod_decl.decl = parse_func_ex(p, true, true, false, AJS_FN_BC_NORMAL);
            return ed;
        }
        if (p->lex.cur.kind == TOK_IDENT && !p->lex.cur.has_escape &&
            u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "async")) {
            AjsToken pk = ajs_lexer_peek(&p->lex);
            if (!pk.had_newline && pk.kind == KW_FUNCTION) {
                p_advance(p);
                ed->u.mod_decl.decl = parse_func_ex(p, true, true, true, AJS_FN_BC_ASYNC);
                return ed;
            }
        }
        if (p->lex.cur.kind == KW_CLASS) {
            ed->u.mod_decl.decl = parse_class_ex(p, true, true);
            return ed;
        }
        ed->u.mod_decl.decl = parse_assign_expr(p);
        check_no_cover_init(p, ed->u.mod_decl.decl);
        p_consume_semicolon(p);
        return ed;
    }

    if (p_match(p, TOK_MUL)) {
        AjsAstNode *ea = ast_new(p, AST_EXPORT_ALL_DECL, etok.line, etok.col);
        if (p->lex.cur.kind == TOK_IDENT && !p->lex.cur.has_escape &&
            u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "as")) {
            p_advance(p);
            AjsToken ns_tok = p->lex.cur;
            if (ns_tok.kind == TOK_STRING || is_property_name_word(ns_tok.kind)) {
                p_advance(p);
                record_exported_name(p, ns_tok.u16_data, ns_tok.u16_len, ns_tok.line, ns_tok.col);
                AjsAstNode *sp = ast_new(p, AST_SPECIFIER, ns_tok.line, ns_tok.col);
                sp->u.spec.is_namespace = true;
                sp->u.spec.exported_u16 = ns_tok.u16_data;
                sp->u.spec.exported_len = ns_tok.u16_len;
                nodelist_push(p, &ea->u.mod_decl.specifiers, sp);
            } else {
                parse_error_tok(p, ns_tok, "Expected export name after 'export * as'");
                return ea;
            }
        }
        AjsToken from_tok = p_advance(p);
        if (from_tok.kind != TOK_IDENT || from_tok.has_escape || !u16_eq_ascii(from_tok.u16_data, from_tok.u16_len, "from")) {
            parse_error_tok(p, from_tok, "Expected 'from' after 'export *'");
            return ea;
        }
        AjsToken src_tok = p->lex.cur;
        if (!p_expect(p, TOK_STRING, "module specifier string")) return ea;
        ea->u.mod_decl.source_u16 = src_tok.u16_data;
        ea->u.mod_decl.source_len = src_tok.u16_len;
        parse_optional_import_attributes(p);
        p_consume_semicolon(p);
        return ea;
    }

    if (p_match(p, TOK_LBRACE)) {
        AjsAstNode *en = ast_new(p, AST_EXPORT_NAMED_DECL, etok.line, etok.col);
        bool had_str_local = false;
        while (!p->had_error && p->lex.cur.kind != TOK_RBRACE && p->lex.cur.kind != TOK_EOF) {
            AjsToken loc_tok = p->lex.cur;
            if (loc_tok.kind == TOK_STRING) {
                had_str_local = true;
                p_advance(p);
            } else if (is_property_name_word(loc_tok.kind)) {
                p_advance(p);
            } else {
                parse_error_tok(p, loc_tok, "Expected export specifier");
                break;
            }
            AjsToken exp_tok = loc_tok;
            if (p->lex.cur.kind == TOK_IDENT && !p->lex.cur.has_escape &&
                u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "as")) {
                p_advance(p);
                exp_tok = p->lex.cur;
                if (exp_tok.kind == TOK_STRING || is_property_name_word(exp_tok.kind)) {
                    p_advance(p);
                } else {
                    parse_error_tok(p, exp_tok, "Expected exported name after 'as'");
                    break;
                }
            }
            record_exported_name(p, exp_tok.u16_data, exp_tok.u16_len, exp_tok.line, exp_tok.col);
            AjsAstNode *sp = ast_new(p, AST_SPECIFIER, loc_tok.line, loc_tok.col);
            sp->u.spec.local_u16 = loc_tok.u16_data;
            sp->u.spec.local_len = loc_tok.u16_len;
            sp->u.spec.exported_u16 = exp_tok.u16_data;
            sp->u.spec.exported_len = exp_tok.u16_len;
            sp->u.spec.is_namespace = (loc_tok.kind != TOK_IDENT && loc_tok.kind != KW_YIELD &&
                                       loc_tok.kind != KW_AWAIT && loc_tok.kind != KW_LET && loc_tok.kind != KW_STATIC);
            /* Note: if `from "..."` is absent, local_tok must be a valid IdentifierReference! Checked below. */
            nodelist_push(p, &en->u.mod_decl.specifiers, sp);
            if (!p_match(p, TOK_COMMA)) break;
        }
        p_expect(p, TOK_RBRACE, "'}'");

        if (p->lex.cur.kind == TOK_IDENT && !p->lex.cur.has_escape &&
            u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "from")) {
            p_advance(p);
            AjsToken src_tok = p->lex.cur;
            if (!p_expect(p, TOK_STRING, "module specifier string")) return en;
            en->u.mod_decl.source_u16 = src_tok.u16_data;
            en->u.mod_decl.source_len = src_tok.u16_len;
            for (uint32_t i = 0; i < en->u.mod_decl.specifiers.count; i++) {
                AjsAstNode *sp = en->u.mod_decl.specifiers.items[i];
                sp->u.spec.imported_u16 = sp->u.spec.local_u16;
                sp->u.spec.imported_len = sp->u.spec.local_len;
                sp->u.spec.is_namespace = false;
            }
            parse_optional_import_attributes(p);
        } else if (had_str_local) {
            parse_error_tok(p, etok, "String literal export name requires 'from' clause");
        } else {
            for (uint32_t i = 0; i < en->u.mod_decl.specifiers.count; i++) {
                AjsAstNode *sp = en->u.mod_decl.specifiers.items[i];
                if (sp->u.spec.is_namespace) {
                    parse_error_at(p, sp->line, sp->col, "Reserved word cannot be exported locally without 'from'");
                    break;
                }
            }
        }
        p_consume_semicolon(p);
        return en;
    }

    AjsAstNode *en = ast_new(p, AST_EXPORT_NAMED_DECL, etok.line, etok.col);
    if (p->lex.cur.kind == KW_VAR || p->lex.cur.kind == KW_LET || p->lex.cur.kind == KW_CONST) {
        AjsVarKind vk = (p->lex.cur.kind == KW_VAR) ? AJS_VAR_VAR :
                        ((p->lex.cur.kind == KW_LET) ? AJS_VAR_LET : AJS_VAR_CONST);
        AjsAstNode *vd = parse_var_decl_list(p, vk, false);
        p_consume_semicolon(p);
        for (uint32_t i = 0; i < vd->u.var_decl.decls.count; i++) {
            record_exported_pattern(p, vd->u.var_decl.decls.items[i]->u.declarator.id);
        }
        en->u.mod_decl.decl = vd;
        return en;
    }
    if (p->lex.cur.kind == KW_FUNCTION) {
        AjsAstNode *fd = parse_func(p, true, false, AJS_FN_BC_NORMAL);
        if (fd && fd->u.func.name_u16) {
            record_exported_name(p, fd->u.func.name_u16, fd->u.func.name_len, fd->line, fd->col);
        }
        en->u.mod_decl.decl = fd;
        return en;
    }
    if (p->lex.cur.kind == TOK_IDENT && !p->lex.cur.has_escape &&
        u16_eq_ascii(p->lex.cur.u16_data, p->lex.cur.u16_len, "async")) {
        AjsToken pk = ajs_lexer_peek(&p->lex);
        if (!pk.had_newline && pk.kind == KW_FUNCTION) {
            p_advance(p);
            AjsAstNode *fd = parse_func(p, true, true, AJS_FN_BC_ASYNC);
            if (fd && fd->u.func.name_u16) {
                record_exported_name(p, fd->u.func.name_u16, fd->u.func.name_len, fd->line, fd->col);
            }
            en->u.mod_decl.decl = fd;
            return en;
        }
    }
    if (p->lex.cur.kind == KW_CLASS) {
        AjsAstNode *cd = parse_class(p, true);
        if (cd && cd->u.klass.name_u16) {
            record_exported_name(p, cd->u.klass.name_u16, cd->u.klass.name_len, cd->line, cd->col);
        }
        en->u.mod_decl.decl = cd;
        return en;
    }

    parse_error_tok(p, p->lex.cur, "Unexpected token after 'export'");
    return en;
}

/* Re-parse hook fixup for derived class constructor `super()` calls:
   Wait! Notice in `parse_class_ex` above, `parse_method_body_node` had `allow_super_call = false`.
   Let's make sure derived class constructors DO have `allow_super_call = true`!
   How? Right before calling `parse_method_body_node` in `parse_class_ex`? Wait: `parse_class_ex` is already above!
   Let's check: in `parse_method_body_node` (line 757), it sets `p->allow_super_call = false;` unconditionally!
   We can patch `parse_method_body_node` cleanly with `replace_file_content` right after appending! */

static AjsAstNode *parse_stmt(AjsParser *p, bool allow_decl) {
    if (p->had_error) return NULL;
    AjsToken cur = p->lex.cur;

    switch (cur.kind) {
        case TOK_SEMI:
            p_advance(p);
            return ast_new(p, AST_EMPTY_STMT, cur.line, cur.col);

        case TOK_LBRACE: {
            p_advance(p);
            scope_push(p, false, false);
            AjsAstNode *blk = ast_new(p, AST_BLOCK, cur.line, cur.col);
            while (!p->had_error && p->lex.cur.kind != TOK_RBRACE && p->lex.cur.kind != TOK_EOF) {
                nodelist_push(p, &blk->u.block.stmts, parse_stmt(p, true));
            }
            p_expect(p, TOK_RBRACE, "'}'");
            scope_pop(p);
            return blk;
        }

        case KW_VAR: {
            AjsAstNode *vd = parse_var_decl_list(p, AJS_VAR_VAR, false);
            p_consume_semicolon(p);
            return vd;
        }

        case KW_CONST: {
            if (!allow_decl) {
                parse_error_tok(p, cur, "Lexical declaration cannot appear in a single-statement context");
                return NULL;
            }
            AjsAstNode *vd = parse_var_decl_list(p, AJS_VAR_CONST, false);
            p_consume_semicolon(p);
            return vd;
        }

        case KW_LET: {
            if (is_let_lexical_decl_start(p)) {
                if (!allow_decl) {
                    parse_error_tok(p, cur, "Lexical declaration cannot appear in a single-statement context");
                    return NULL;
                }
                AjsAstNode *vd = parse_var_decl_list(p, AJS_VAR_LET, false);
                p_consume_semicolon(p);
                return vd;
            }
            break;
        }

        case KW_FUNCTION: {
            if (!allow_decl) {
                /* Annex B.3.4 allows single-statement plain `function` inside `if`/`else` in sloppy mode ONLY! */
                AjsToken pk = ajs_lexer_peek(&p->lex);
                if (p->strict || pk.kind == TOK_MUL) {
                    parse_error_tok(p, cur, "Function declarations cannot appear in single-statement context in strict mode");
                    return NULL;
                }
            }
            return parse_func(p, true, false, AJS_FN_BC_NORMAL);
        }

        case KW_CLASS: {
            if (!allow_decl) {
                parse_error_tok(p, cur, "Class declaration cannot appear in a single-statement context");
                return NULL;
            }
            return parse_class(p, true);
        }

        case KW_IF: {
            p_advance(p);
            p_expect(p, TOK_LPAREN, "'(' after 'if'");
            AjsAstNode *test = parse_expr(p);
            check_no_cover_init(p, test);
            p_expect(p, TOK_RPAREN, "')'");
            /* Only plain sloppy `if`/`else` allows Annex B function decl; loops do NOT! */
            AjsAstNode *conseq = NULL;
            if (!p->strict && p->lex.cur.kind == KW_FUNCTION && ajs_lexer_peek(&p->lex).kind != TOK_MUL) {
                conseq = parse_func(p, true, false, AJS_FN_BC_NORMAL);
            } else {
                conseq = parse_stmt(p, false);
            }
            AjsAstNode *alt = NULL;
            if (p_match(p, KW_ELSE)) {
                if (!p->strict && p->lex.cur.kind == KW_FUNCTION && ajs_lexer_peek(&p->lex).kind != TOK_MUL) {
                    alt = parse_func(p, true, false, AJS_FN_BC_NORMAL);
                } else {
                    alt = parse_stmt(p, false);
                }
            }
            AjsAstNode *ifs = ast_new(p, AST_IF_STMT, cur.line, cur.col);
            ifs->u.if_stmt.test = test;
            ifs->u.if_stmt.conseq = conseq;
            ifs->u.if_stmt.alt = alt;
            return ifs;
        }

        case KW_WHILE: {
            p_advance(p);
            p_expect(p, TOK_LPAREN, "'(' after 'while'");
            AjsAstNode *test = parse_expr(p);
            check_no_cover_init(p, test);
            p_expect(p, TOK_RPAREN, "')'");
            if (p->lex.cur.kind == KW_FUNCTION) {
                parse_error_tok(p, p->lex.cur, "Function declaration cannot appear in loop body without block");
            }
            p->loop_depth++;
            AjsAstNode *body = parse_stmt(p, false);
            p->loop_depth--;
            AjsAstNode *ws = ast_new(p, AST_WHILE_STMT, cur.line, cur.col);
            ws->u.loop.test = test;
            ws->u.loop.body = body;
            return ws;
        }

        case KW_DO: {
            p_advance(p);
            if (p->lex.cur.kind == KW_FUNCTION) {
                parse_error_tok(p, p->lex.cur, "Function declaration cannot appear in do-while body without block");
            }
            p->loop_depth++;
            AjsAstNode *body = parse_stmt(p, false);
            p->loop_depth--;
            p_expect(p, KW_WHILE, "'while' after 'do' body");
            p_expect(p, TOK_LPAREN, "'(' after 'while'");
            AjsAstNode *test = parse_expr(p);
            check_no_cover_init(p, test);
            p_expect(p, TOK_RPAREN, "')'");
            /* ECMA-262 §12.9.1: ASI ALWAYS applies right after `do ... while (expr)` even on same line! */
            p_match(p, TOK_SEMI);
            AjsAstNode *dw = ast_new(p, AST_DO_WHILE_STMT, cur.line, cur.col);
            dw->u.loop.test = test;
            dw->u.loop.body = body;
            return dw;
        }

        case KW_FOR:
            return parse_for_stmt(p);

        case KW_SWITCH: {
            p_advance(p);
            p_expect(p, TOK_LPAREN, "'(' after 'switch'");
            AjsAstNode *disc = parse_expr(p);
            check_no_cover_init(p, disc);
            p_expect(p, TOK_RPAREN, "')'");
            p_expect(p, TOK_LBRACE, "'{' in switch statement");

            scope_push(p, false, false);
            p->switch_depth++;
            AjsAstNode *sw = ast_new(p, AST_SWITCH_STMT, cur.line, cur.col);
            sw->u.switch_stmt.discriminant = disc;
            bool seen_default = false;

            while (!p->had_error && p->lex.cur.kind != TOK_RBRACE && p->lex.cur.kind != TOK_EOF) {
                AjsToken ctok = p->lex.cur;
                AjsAstNode *sc = ast_new(p, AST_SWITCH_CASE, ctok.line, ctok.col);
                if (p_match(p, KW_CASE)) {
                    sc->u.switch_case.test = parse_expr(p);
                    check_no_cover_init(p, sc->u.switch_case.test);
                    p_expect(p, TOK_COLON, "':' after case expression");
                } else if (p_match(p, KW_DEFAULT)) {
                    if (seen_default) {
                        parse_error_tok(p, ctok, "More than one default clause in switch statement");
                    }
                    seen_default = true;
                    p_expect(p, TOK_COLON, "':' after 'default'");
                } else {
                    parse_error_tok(p, ctok, "Expected 'case' or 'default' in switch statement");
                    break;
                }
                while (!p->had_error && p->lex.cur.kind != KW_CASE && p->lex.cur.kind != KW_DEFAULT &&
                       p->lex.cur.kind != TOK_RBRACE && p->lex.cur.kind != TOK_EOF) {
                    nodelist_push(p, &sc->u.switch_case.conseq, parse_stmt(p, true));
                }
                nodelist_push(p, &sw->u.switch_stmt.cases, sc);
            }
            p->switch_depth--;
            scope_pop(p);
            p_expect(p, TOK_RBRACE, "'}'");
            return sw;
        }

        case KW_RETURN: {
            if (!p->in_function || p->in_static_block) {
                parse_error_tok(p, cur, "Illegal return statement");
                return NULL;
            }
            p_advance(p);
            AjsAstNode *rs = ast_new(p, AST_RETURN_STMT, cur.line, cur.col);
            if (!p->lex.cur.had_newline && p->lex.cur.kind != TOK_SEMI &&
                p->lex.cur.kind != TOK_RBRACE && p->lex.cur.kind != TOK_EOF) {
                rs->u.expr_stmt.expr = parse_expr(p);
                check_no_cover_init(p, rs->u.expr_stmt.expr);
            }
            p_consume_semicolon(p);
            return rs;
        }

        case KW_BREAK:
        case KW_CONTINUE: {
            bool is_brk = (cur.kind == KW_BREAK);
            p_advance(p);
            AjsAstNode *jmp = ast_new(p, is_brk ? AST_BREAK_STMT : AST_CONTINUE_STMT, cur.line, cur.col);
            if (!p->lex.cur.had_newline && is_ident_reference_tok(p, p->lex.cur)) {
                AjsToken ltok = p_advance(p);
                jmp->u.jump.label_u16 = ltok.u16_data;
                jmp->u.jump.label_len = ltok.u16_len;
                bool found = false;
                for (int i = (int)p->label_count - 1; i >= 0; i--) {
                    if (u16_eq(p->labels[i].u16, p->labels[i].len, ltok.u16_data, ltok.u16_len)) {
                        if (!is_brk && !p->labels[i].is_loop) {
                            parse_error_tok(p, ltok, "Illegal continue statement: label does not denote an iteration statement");
                        }
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    parse_error_tok(p, ltok, "Undefined label");
                }
            } else {
                if (is_brk && p->loop_depth == 0 && p->switch_depth == 0) {
                    parse_error_tok(p, cur, "Illegal break statement");
                } else if (!is_brk && p->loop_depth == 0) {
                    parse_error_tok(p, cur, "Illegal continue statement");
                }
            }
            p_consume_semicolon(p);
            return jmp;
        }

        case KW_THROW: {
            p_advance(p);
            if (p->lex.cur.had_newline) {
                parse_error_tok(p, cur, "Illegal newline after throw");
                return NULL;
            }
            AjsAstNode *ts = ast_new(p, AST_THROW_STMT, cur.line, cur.col);
            ts->u.expr_stmt.expr = parse_expr(p);
            check_no_cover_init(p, ts->u.expr_stmt.expr);
            p_consume_semicolon(p);
            return ts;
        }

        case KW_TRY: {
            p_advance(p);
            if (p->lex.cur.kind != TOK_LBRACE) {
                parse_error_tok(p, p->lex.cur, "Expected '{' after 'try'");
                return NULL;
            }
            AjsAstNode *ts = ast_new(p, AST_TRY_STMT, cur.line, cur.col);
            ts->u.try_stmt.block = parse_stmt(p, true);
            bool has_handler = false;

            if (p_match(p, KW_CATCH)) {
                has_handler = true;
                scope_push(p, false, true);
                if (p_match(p, TOK_LPAREN)) {
                    AjsAstNode *cp = parse_binding_pattern_or_ident(p);
                    ts->u.try_stmt.catch_param = cp;
                    if (cp && cp->kind == AST_IDENT) {
                        if (p->strict && (u16_eq_ascii(cp->u.ident.u16, cp->u.ident.len, "eval") ||
                                          u16_eq_ascii(cp->u.ident.u16, cp->u.ident.len, "arguments"))) {
                            parse_error_at(p, cp->line, cp->col, "Invalid catch parameter name in strict mode");
                        }
                        scope_add_raw(p, p->scope, cp->u.ident.u16, cp->u.ident.len, true, false, true);
                    } else {
                        declare_pattern_names(p, cp, true, false, NULL);
                    }
                    p_expect(p, TOK_RPAREN, "')' after catch parameter");
                }
                if (p->lex.cur.kind != TOK_LBRACE) {
                    parse_error_tok(p, p->lex.cur, "Expected '{' for catch block");
                } else {
                    AjsToken lb = p_advance(p);
                    AjsAstNode *cblk = ast_new(p, AST_BLOCK, lb.line, lb.col);
                    while (!p->had_error && p->lex.cur.kind != TOK_RBRACE && p->lex.cur.kind != TOK_EOF) {
                        nodelist_push(p, &cblk->u.block.stmts, parse_stmt(p, true));
                    }
                    p_expect(p, TOK_RBRACE, "'}'");
                    ts->u.try_stmt.catch_body = cblk;
                }
                scope_pop(p);
            }

            if (p_match(p, KW_FINALLY)) {
                has_handler = true;
                if (p->lex.cur.kind != TOK_LBRACE) {
                    parse_error_tok(p, p->lex.cur, "Expected '{' after 'finally'");
                } else {
                    ts->u.try_stmt.finally_body = parse_stmt(p, true);
                }
            }

            if (!has_handler) {
                parse_error_tok(p, cur, "Missing catch or finally after try");
            }
            return ts;
        }

        case KW_WITH: {
            if (p->strict) {
                parse_error_tok(p, cur, "Strict mode code may not include a with statement");
                return NULL;
            }
            p_advance(p);
            p_expect(p, TOK_LPAREN, "'(' after 'with'");
            AjsAstNode *obj = parse_expr(p);
            check_no_cover_init(p, obj);
            p_expect(p, TOK_RPAREN, "')'");
            AjsAstNode *body = parse_stmt(p, false);
            AjsAstNode *ws = ast_new(p, AST_WITH_STMT, cur.line, cur.col);
            ws->u.with_stmt.obj = obj;
            ws->u.with_stmt.body = body;
            return ws;
        }

        case KW_DEBUGGER:
            p_advance(p);
            p_consume_semicolon(p);
            return ast_new(p, AST_DEBUGGER_STMT, cur.line, cur.col);

        case KW_IMPORT: {
            AjsToken pk = ajs_lexer_peek(&p->lex);
            if (pk.kind != TOK_LPAREN && pk.kind != TOK_DOT) {
                if (!p->is_module || p->in_function || p->scope->parent != NULL) {
                    parse_error_tok(p, cur, "Cannot use import statement outside a module top-level");
                    return NULL;
                }
                return parse_import_decl(p);
            }
            break;
        }

        case KW_EXPORT: {
            if (!p->is_module || p->in_function || p->scope->parent != NULL) {
                parse_error_tok(p, cur, "Cannot use export statement outside a module top-level");
                return NULL;
            }
            return parse_export_decl(p);
        }

        default:
            break;
    }

    /* Check `await using` / `using` lexical declarations */
    if (is_await_using_decl_start(p, false)) {
        if (!allow_decl) {
            parse_error_tok(p, cur, "'await using' declaration cannot appear in a single-statement context");
            return NULL;
        }
        AjsAstNode *vd = parse_var_decl_list(p, AJS_VAR_AWAIT_USING, false);
        p_consume_semicolon(p);
        return vd;
    }
    if (is_using_decl_start(p, false)) {
        if (!allow_decl) {
            parse_error_tok(p, cur, "'using' declaration cannot appear in a single-statement context");
            return NULL;
        }
        AjsAstNode *vd = parse_var_decl_list(p, AJS_VAR_USING, false);
        p_consume_semicolon(p);
        return vd;
    }

    /* Check `async function` declaration */
    if (cur.kind == TOK_IDENT && !cur.has_escape && u16_eq_ascii(cur.u16_data, cur.u16_len, "async")) {
        AjsToken pk = ajs_lexer_peek(&p->lex);
        if (!pk.had_newline && pk.kind == KW_FUNCTION) {
            if (!allow_decl) {
                parse_error_tok(p, cur, "Async function declaration cannot appear in a single-statement context");
                return NULL;
            }
            p_advance(p);
            return parse_func(p, true, true, AJS_FN_BC_ASYNC);
        }
    }

    /* Check labeled statement `label: stmt` */
    if (is_ident_reference_tok(p, cur)) {
        AjsToken pk = ajs_lexer_peek(&p->lex);
        if (pk.kind == TOK_COLON) {
            p_advance(p); /* label */
            p_advance(p); /* ':' */
            for (uint32_t i = 0; i < p->label_count; i++) {
                if (u16_eq(p->labels[i].u16, p->labels[i].len, cur.u16_data, cur.u16_len)) {
                    parse_error_tok(p, cur, "Label has already been declared");
                    return NULL;
                }
            }
            if (p->label_count >= p->label_cap) {
                uint32_t ncap = p->label_cap ? p->label_cap * 2u : 8u;
                AjsLabelEntry *nl = (AjsLabelEntry *)ajs_arena_alloc(p->arena, ncap * sizeof(AjsLabelEntry));
                ajs_memcpy(nl, p->labels, p->label_count * sizeof(AjsLabelEntry));
                p->labels = nl;
                p->label_cap = ncap;
            }
            uint32_t lbl_idx = p->label_count++;
            p->labels[lbl_idx].u16 = cur.u16_data;
            p->labels[lbl_idx].len = cur.u16_len;
            p->labels[lbl_idx].is_loop = false;

            /* Peek ahead across any chained `L2: L3:` labels in source to see if they wrap a loop! */
            AjsToken body_start = p->lex.cur;
            if (body_start.kind == KW_FOR || body_start.kind == KW_WHILE || body_start.kind == KW_DO) {
                for (int c = 0; c <= p->label_chain_depth && (int)lbl_idx - c >= 0; c++) {
                    p->labels[lbl_idx - (uint32_t)c].is_loop = true;
                }
            }

            AjsAstNode *body = NULL;
            if (!p->strict && body_start.kind == KW_FUNCTION && ajs_lexer_peek(&p->lex).kind != TOK_MUL) {
                p->label_chain_depth = 0;
                body = parse_func(p, true, false, AJS_FN_BC_NORMAL);
            } else {
                bool next_is_label = is_ident_reference_tok(p, body_start) &&
                                     ajs_lexer_peek(&p->lex).kind == TOK_COLON;
                int saved_chain = p->label_chain_depth;
                p->label_chain_depth = next_is_label ? (saved_chain + 1) : 0;
                body = parse_stmt(p, false);
                p->label_chain_depth = saved_chain;
            }
            p->label_count--;
            AjsAstNode *ls = ast_new(p, AST_LABELED_STMT, cur.line, cur.col);
            ls->u.labeled.label_u16 = cur.u16_data;
            ls->u.labeled.label_len = cur.u16_len;
            ls->u.labeled.body = body;
            return ls;
        }
    }

    /* Expression statement */
    AjsAstNode *expr = parse_expr(p);
    check_no_cover_init(p, expr);
    p_consume_semicolon(p);
    AjsAstNode *es = ast_new(p, AST_EXPR_STMT, cur.line, cur.col);
    es->u.expr_stmt.expr = expr;
    return es;
}

AjsAstNode *ajs_parse_program(AjsVm *vm, AjsAstArena *arena, const char *src, size_t len,
                              const char *source_url, bool is_module, bool force_strict,
                              bool allow_new_target, bool allow_super_prop, bool allow_super_call,
                              AjsErrorInfo *out_err) {
    AjsParser p;
    memset(&p, 0, sizeof(p));
    p.vm = vm;
    p.arena = arena;
    p.source_url = source_url ? source_url : (is_module ? "<module>" : "<script>");
    p.is_module = is_module;
    p.strict = force_strict || is_module;
    p.allow_in = true;
    p.allow_new_target = allow_new_target;
    p.allow_super_prop = allow_super_prop;
    p.allow_super_call = allow_super_call;

    ajs_lexer_init(&p.lex, arena, src, len, is_module, p.strict);
    if (p.lex.cur.kind == TOK_ERROR) {
        parse_error_tok(&p, p.lex.cur, "%s", p.lex.err_msg);
    }

    scope_push(&p, true, false);
    AjsAstNode *prog = ast_new(&p, AST_PROGRAM, 1, 1);
    prog->u.program.is_module = is_module;

    bool in_prologue = !is_module;
    bool prologue_octal = false;
    while (!p.had_error && p.lex.cur.kind != TOK_EOF) {
        AjsToken st_tok = p.lex.cur;
        AjsAstNode *st = parse_stmt(&p, true);
        if (in_prologue && st && st->kind == AST_EXPR_STMT && st->u.expr_stmt.expr &&
            st->u.expr_stmt.expr->kind == AST_LITERAL_STR && !st->u.expr_stmt.expr->parenthesized) {
            if (st_tok.has_legacy_octal) prologue_octal = true;
            if (!st_tok.has_escape && !st_tok.has_legacy_octal &&
                u16_eq_ascii(st_tok.u16_data, st_tok.u16_len, "use strict")) {
                p.strict = true;
                p.lex.strict = true;
                if (prologue_octal) {
                    parse_error_tok(&p, st_tok, "Octal escape sequences are not allowed in strict mode");
                }
            }
        } else {
            in_prologue = false;
        }
        nodelist_push(&p, &prog->u.program.body, st);
    }

    /* Module early error check: every local `export { localName }` must match a declared binding in module scope! */
    if (is_module && !p.had_error) {
        for (uint32_t i = 0; i < prog->u.program.body.count && !p.had_error; i++) {
            AjsAstNode *st = prog->u.program.body.items[i];
            if (st && st->kind == AST_EXPORT_NAMED_DECL && st->u.mod_decl.decl == NULL &&
                st->u.mod_decl.source_u16 == NULL) {
                for (uint32_t j = 0; j < st->u.mod_decl.specifiers.count; j++) {
                    AjsAstNode *sp = st->u.mod_decl.specifiers.items[j];
                    uint32_t sh = ajs_hash_utf16(sp->u.spec.local_u16, sp->u.spec.local_len);
                    bool declared = (scope_find_entry(p.scope, sp->u.spec.local_u16, sp->u.spec.local_len, sh) >= 0);
                    if (!declared) {
                        parse_error_at(&p, sp->line, sp->col, "Exported binding is not defined in module");
                        break;
                    }
                }
            }
        }
    }

    prog->u.program.strict = p.strict;
    prog->u.program.has_tla = p.saw_top_await;
    scope_pop(&p);

    if (p.had_error) {
        if (out_err) {
            memset(out_err, 0, sizeof(*out_err));
            out_err->phase = AJS_ERR_PARSE;
            snprintf(out_err->name, sizeof(out_err->name), "SyntaxError");
            snprintf(out_err->message, sizeof(out_err->message), "%s", p.err_msg);
            snprintf(out_err->url, sizeof(out_err->url), "%s", p.source_url);
            out_err->line = p.err_line;
            out_err->column = p.err_col;
            snprintf(out_err->stack, sizeof(out_err->stack), "SyntaxError: %s\n    at %s:%u:%u",
                     p.err_msg, p.source_url, p.err_line, p.err_col);
        }
        return NULL;
    }
    return prog;
}
