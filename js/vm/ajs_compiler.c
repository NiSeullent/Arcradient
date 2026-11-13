/*
 * ArcradientJS 1.0 - js/vm/ajs_compiler.c
 * Clean-Room Pure C11 ECMAScript 2026 AST-to-Bytecode Compiler
 *
 * Compiles Scripts, Direct/Indirect Eval, Dynamic Functions, Functions/Arrows/Generators/Async,
 * Classes (derived super(), super.prop, static blocks, #private fields/methods/accessors),
 * Destructuring, Optional Chaining, Try/Catch/Finally + IteratorClose Unwinding, and ES Modules.
 */
#include "ajs_parse_internal.h"

/* ============================================================================
 * 1. Compiler Data Structures
 * ============================================================================ */

typedef enum AjsBindKind {
    BIND_VAR = 0,
    BIND_LET,
    BIND_CONST,
    BIND_PARAM,
    BIND_FUNC_NAME,
    BIND_IMPORT,
    BIND_PRIVATE
} AjsBindKind;

typedef struct AjsCompLocal {
    const uint16_t *u16;
    uint32_t        len;
    uint32_t        hash;
    int32_t         hash_next;
    AjsString      *str;
    uint32_t        slot;
    AjsBindKind     kind;
    bool            is_priv;
    uint8_t         priv_kind; /* AjsPropKind for private members */
    bool            priv_has_get;
    bool            priv_has_set;
} AjsCompLocal;

static inline bool is_const_var_kind(AjsVarKind vk) {
    return vk == AJS_VAR_CONST || vk == AJS_VAR_USING || vk == AJS_VAR_AWAIT_USING;
}

typedef struct AjsCompScope {
    struct AjsCompScope *parent;
    bool                 is_base;       /* true = function/module/eval top frame env */
    bool                 has_runtime_env;
    bool                 has_disp_stack;
    uint16_t             env_depth_idx; /* Runtime AjsEnv depth from frame base (0 = base_env) */
    uint32_t             disp_slot;
    uint32_t             slot_count;
    uint32_t             slot_cap;
    AjsCompLocal        *locals;
    uint32_t             hash_cap;
    int32_t             *hash_tab;
} AjsCompScope;

typedef struct AjsJumpPatch {
    uint32_t patch_off;
    uint16_t extra_pops;
} AjsJumpPatch;

typedef struct AjsLoopCtx {
    struct AjsLoopCtx *prev;
    bool               is_switch;
    bool               is_label_only;
    const uint16_t    *label_u16;
    uint32_t           label_len;
    uint16_t           try_depth;
    uint16_t           env_depth;
    uint16_t           cont_try_depth;
    uint16_t           cont_env_depth;
    uint32_t           cont_pc;       /* UINT32_MAX if forward-patched */
    uint32_t          *break_patches;
    uint32_t           break_count;
    uint32_t           break_cap;
    uint32_t          *cont_patches;
    uint32_t           cont_count;
    uint32_t           cont_cap;
} AjsLoopCtx;

typedef struct AjsCompiler {
    AjsVm           *vm;
    AjsRealm        *realm;
    struct AjsCompiler *enclosing;
    AjsBytecode     *bc;
    AjsModuleRecord *mod;
    const char      *src_base;
    AjsSourceBuf    *src_buf;

    bool             is_script_global; /* true when top-level var/let go to global_obj / global_lex_env */
    bool             strict;
    bool             has_eval_or_with;
    uint16_t         with_depth;
    uint16_t         cur_env_depth;    /* Number of active PUSH_ENV / PUSH_WITH above base_env */
    uint16_t         cur_try_depth;

    uint32_t         eval_ret_slot;    /* AJS_NO_SLOT unless script/eval completion value tracking */

    uint32_t         const_hash_cap;
    int32_t         *const_hash;

    AjsCompScope    *scope;
    AjsCompScope     base_scope;
    AjsLoopCtx      *loop_top;

    uint32_t         last_line;
    uint32_t         last_col;
    bool             had_error;
    char             err_msg[256];
    uint32_t         err_line;
    uint32_t         err_col;
} AjsCompiler;

static bool compile_stmt(AjsCompiler *c, AjsAstNode *n);
static bool compile_expr(AjsCompiler *c, AjsAstNode *n);
static AjsBytecode *compile_func_node(AjsCompiler *parent, AjsAstNode *fn_node,
                                      const char *default_name_cstr, bool is_derived_ctor);
static bool compile_class_node(AjsCompiler *c, AjsAstNode *kl, AjsString *inferred_name, bool push_ctor);
static bool compile_pattern_assign(AjsCompiler *c, AjsAstNode *pat, bool is_init, AjsVarKind vk);

/* ============================================================================
 * 2. UTF-16 & Bytecode Emission Utilities
 * ============================================================================ */

static bool c_u16_eq(const uint16_t *a, uint32_t alen, const uint16_t *b, uint32_t blen) {
    if (alen != blen) return false;
    for (uint32_t i = 0; i < alen; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static bool c_u16_eq_ascii(const uint16_t *a, uint32_t alen, const char *s) {
    uint32_t i = 0;
    while (s[i]) {
        if (i >= alen || a[i] != (uint8_t)s[i]) return false;
        i++;
    }
    return i == alen;
}

static void comp_error(AjsCompiler *c, uint32_t line, uint32_t col, const char *fmt, ...) {
    if (c->had_error) return;
    c->had_error = true;
    c->err_line = line ? line : 1;
    c->err_col = col ? col : 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err_msg, sizeof(c->err_msg), fmt, ap);
    va_end(ap);
}

static void emit_line_pos(AjsCompiler *c, uint32_t line, uint32_t col) {
    if (line == 0) return;
    if (line == c->last_line && col == c->last_col) return;
    c->last_line = line;
    c->last_col = col;
    AjsBytecode *bc = c->bc;
    if (bc->line_count > 0 && bc->lines[bc->line_count - 1].bc_offset == bc->code_len) {
        bc->lines[bc->line_count - 1].line = line;
        bc->lines[bc->line_count - 1].col = col;
        return;
    }
    if (bc->line_count >= bc->line_cap) {
        uint32_t ncap = bc->line_cap ? bc->line_cap * 2u : 32u;
        AjsLineSpan *nl = (AjsLineSpan *)realloc(bc->lines, ncap * sizeof(AjsLineSpan));
        if (!nl) return;
        bc->lines = nl;
        bc->line_cap = ncap;
    }
    bc->lines[bc->line_count].bc_offset = bc->code_len;
    bc->lines[bc->line_count].line = line;
    bc->lines[bc->line_count].col = col;
    bc->line_count++;
}

static void emit_u8(AjsCompiler *c, uint8_t b) {
    AjsBytecode *bc = c->bc;
    if (bc->code_len >= bc->code_cap) {
        uint32_t ncap = bc->code_cap ? bc->code_cap * 2u : 128u;
        uint8_t *nc = (uint8_t *)realloc(bc->code, ncap);
        if (!nc) { comp_error(c, 1, 1, "Out of memory"); return; }
        bc->code = nc;
        bc->code_cap = ncap;
    }
    bc->code[bc->code_len++] = b;
}

static void emit_op(AjsCompiler *c, AjsOpcode op) {
    emit_u8(c, (uint8_t)op);
}

static void emit_u16(AjsCompiler *c, uint16_t v) {
    emit_u8(c, (uint8_t)(v & 0xFFu));
    emit_u8(c, (uint8_t)((v >> 8) & 0xFFu));
}

static void emit_u32(AjsCompiler *c, uint32_t v) {
    emit_u8(c, (uint8_t)(v & 0xFFu));
    emit_u8(c, (uint8_t)((v >> 8) & 0xFFu));
    emit_u8(c, (uint8_t)((v >> 16) & 0xFFu));
    emit_u8(c, (uint8_t)((v >> 24) & 0xFFu));
}

static void emit_i32(AjsCompiler *c, int32_t v) {
    emit_u32(c, (uint32_t)v);
}

static uint32_t emit_jump(AjsCompiler *c, AjsOpcode op) {
    emit_op(c, op);
    uint32_t off = c->bc->code_len;
    emit_u32(c, 0);
    return off;
}

static void patch_u32(AjsCompiler *c, uint32_t off, uint32_t val) {
    if (off + 4u > c->bc->code_len) return;
    c->bc->code[off + 0] = (uint8_t)(val & 0xFFu);
    c->bc->code[off + 1] = (uint8_t)((val >> 8) & 0xFFu);
    c->bc->code[off + 2] = (uint8_t)((val >> 16) & 0xFFu);
    c->bc->code[off + 3] = (uint8_t)((val >> 24) & 0xFFu);
}

static void patch_jump(AjsCompiler *c, uint32_t off) {
    patch_u32(c, off, c->bc->code_len);
}

static uint32_t hash_const_val(AjsValue v) {
    uint64_t x = (uint64_t)v;
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    return (uint32_t)x;
}

static void rebuild_const_hash(AjsCompiler *c, uint32_t ncap) {
    int32_t *nt = (int32_t *)malloc(ncap * sizeof(int32_t));
    if (!nt) return;
    for (uint32_t i = 0; i < ncap; i++) nt[i] = -1;
    uint32_t mask = ncap - 1u;
    for (uint32_t i = 0; i < c->bc->const_count; i++) {
        uint32_t pos = hash_const_val(c->bc->consts[i]) & mask;
        while (nt[pos] >= 0) pos = (pos + 1u) & mask;
        nt[pos] = (int32_t)i;
    }
    free(c->const_hash);
    c->const_hash = nt;
    c->const_hash_cap = ncap;
}

static uint32_t add_const(AjsCompiler *c, AjsValue val) {
    AjsBytecode *bc = c->bc;
    if (c->const_hash) {
        uint32_t mask = c->const_hash_cap - 1u;
        uint32_t pos = hash_const_val(val) & mask;
        while (c->const_hash[pos] >= 0) {
            uint32_t idx = (uint32_t)c->const_hash[pos];
            if (bc->consts[idx] == val) return idx;
            pos = (pos + 1u) & mask;
        }
    } else {
        for (uint32_t i = 0; i < bc->const_count; i++) {
            if (bc->consts[i] == val) return i;
        }
    }
    if (bc->const_count >= bc->const_cap) {
        uint32_t ncap = bc->const_cap ? bc->const_cap * 2u : 16u;
        AjsValue *nc = (AjsValue *)realloc(bc->consts, ncap * sizeof(AjsValue));
        if (!nc) { comp_error(c, 1, 1, "Out of memory"); return 0; }
        bc->consts = nc;
        bc->const_cap = ncap;
    }
    uint32_t idx = bc->const_count++;
    bc->consts[idx] = val;
    if (c->const_hash) {
        if ((bc->const_count + 1u) * 10u >= c->const_hash_cap * 7u) {
            rebuild_const_hash(c, c->const_hash_cap * 2u);
        } else {
            uint32_t mask = c->const_hash_cap - 1u;
            uint32_t pos = hash_const_val(val) & mask;
            while (c->const_hash[pos] >= 0) pos = (pos + 1u) & mask;
            c->const_hash[pos] = (int32_t)idx;
        }
    } else if (bc->const_count >= 24u) {
        rebuild_const_hash(c, 64u);
    }
    return idx;
}

static void emit_get_lex(AjsCompiler *c, uint16_t depth, uint32_t slot) {
    if (depth == 0 && slot <= 0xFFu) {
        emit_op(c, OP_GET_LOCAL8);
        emit_u8(c, (uint8_t)slot);
    } else if (slot <= 0xFFFEu) {
        emit_op(c, OP_GET_LEX);
        emit_u16(c, depth);
        emit_u16(c, (uint16_t)slot);
    } else {
        emit_op(c, OP_GET_LEX32);
        emit_u16(c, depth);
        emit_u32(c, slot);
    }
}

static void emit_set_lex(AjsCompiler *c, uint16_t depth, uint32_t slot, uint8_t strict_u8) {
    if (depth == 0 && slot <= 0xFFu) {
        emit_op(c, OP_SET_LOCAL8);
        emit_u8(c, (uint8_t)slot);
        emit_u8(c, strict_u8);
    } else if (slot <= 0xFFFEu) {
        emit_op(c, OP_SET_LEX);
        emit_u16(c, depth);
        emit_u16(c, (uint16_t)slot);
        emit_u8(c, strict_u8);
    } else {
        emit_op(c, OP_SET_LEX32);
        emit_u16(c, depth);
        emit_u32(c, slot);
        emit_u8(c, strict_u8);
    }
}

static void emit_init_lex(AjsCompiler *c, uint16_t depth, uint32_t slot) {
    if (depth == 0 && slot <= 0xFFu) {
        emit_op(c, OP_INIT_LOCAL8);
        emit_u8(c, (uint8_t)slot);
    } else if (slot <= 0xFFFEu) {
        emit_op(c, OP_INIT_LEX);
        emit_u16(c, depth);
        emit_u16(c, (uint16_t)slot);
    } else {
        emit_op(c, OP_INIT_LEX32);
        emit_u16(c, depth);
        emit_u32(c, slot);
    }
}

static void emit_push_env(AjsCompiler *c, uint32_t count, uint32_t ed_idx) {
    if (count <= 0xFFFEu && (ed_idx == AJS_NO_SLOT || ed_idx < 0xFFFFu)) {
        emit_op(c, OP_PUSH_ENV);
        emit_u16(c, (uint16_t)count);
        emit_u16(c, ed_idx == AJS_NO_SLOT ? 0xFFFFu : (uint16_t)ed_idx);
    } else {
        emit_op(c, OP_PUSH_ENV32);
        emit_u32(c, count);
        emit_u32(c, ed_idx);
    }
}

static void emit_clone_env(AjsCompiler *c, uint32_t count) {
    if (count <= 0xFFFEu) {
        emit_op(c, OP_CLONE_ENV);
        emit_u16(c, (uint16_t)count);
    } else {
        emit_op(c, OP_CLONE_ENV32);
        emit_u32(c, count);
    }
}

static uint32_t add_str_u16(AjsCompiler *c, const uint16_t *u16, uint32_t len) {
    AjsString *s = ajs_str_from_utf16(c->vm, u16, len, true);
    if (!s) { comp_error(c, 1, 1, "Out of memory"); return 0; }
    return add_const(c, ajs_val_from_gc(s));
}

static uint32_t add_str_cstr(AjsCompiler *c, const char *cstr) {
    AjsString *s = ajs_str_from_cstr(c->vm, cstr, true);
    if (!s) { comp_error(c, 1, 1, "Out of memory"); return 0; }
    return add_const(c, ajs_val_from_gc(s));
}

/* ============================================================================
 * 3. Pre-pass AST Analysis (Nested Arrows, Eval/With, Arguments, Var Hoisting)
 * ============================================================================ */

typedef struct AjsFnScanInfo {
    bool has_arrow_or_super;
    bool has_eval_or_with;
    bool uses_arguments;
} AjsFnScanInfo;

static void scan_fn_ast(AjsAstNode *n, bool inside_arrow, AjsFnScanInfo *info) {
    if (!n) return;
    switch (n->kind) {
        case AST_IDENT:
            if (c_u16_eq_ascii(n->u.ident.u16, n->u.ident.len, "arguments")) {
                info->uses_arguments = true;
            }
            break;
        case AST_SUPER_EXPR:
            info->has_arrow_or_super = true;
            break;
        case AST_WITH_STMT:
            info->has_eval_or_with = true;
            scan_fn_ast(n->u.with_stmt.obj, inside_arrow, info);
            scan_fn_ast(n->u.with_stmt.body, inside_arrow, info);
            break;
        case AST_CALL_EXPR:
            if (!n->u.call.optional && n->u.call.callee && n->u.call.callee->kind == AST_IDENT &&
                c_u16_eq_ascii(n->u.call.callee->u.ident.u16, n->u.call.callee->u.ident.len, "eval")) {
                info->has_eval_or_with = true;
                info->uses_arguments = true;
                info->has_arrow_or_super = true;
            }
            scan_fn_ast(n->u.call.callee, inside_arrow, info);
            for (uint32_t i = 0; i < n->u.call.args.count; i++) {
                scan_fn_ast(n->u.call.args.items[i], inside_arrow, info);
            }
            break;
        case AST_FUNC_DECL:
        case AST_FUNC_EXPR:
            if (n->u.func.fn_kind == AJS_FN_BC_ARROW || n->u.func.fn_kind == AJS_FN_BC_ASYNC_ARROW) {
                info->has_arrow_or_super = true;
                for (uint32_t i = 0; i < n->u.func.params.count; i++) {
                    scan_fn_ast(n->u.func.params.items[i], true, info);
                }
                scan_fn_ast(n->u.func.body, true, info);
            }
            /* Non-arrow inner functions have their own `this`/`arguments`/`new.target`! */
            break;
        case AST_CLASS_DECL:
        case AST_CLASS_EXPR:
            scan_fn_ast(n->u.klass.super_expr, inside_arrow, info);
            for (uint32_t i = 0; i < n->u.klass.elements.count; i++) {
                AjsAstNode *el = n->u.klass.elements.items[i];
                if (el && el->u.prop.computed) {
                    scan_fn_ast(el->u.prop.key, inside_arrow, info);
                }
            }
            break;
        case AST_PROGRAM:
            for (uint32_t i = 0; i < n->u.program.body.count; i++)
                scan_fn_ast(n->u.program.body.items[i], inside_arrow, info);
            break;
        case AST_BLOCK:
            for (uint32_t i = 0; i < n->u.block.stmts.count; i++)
                scan_fn_ast(n->u.block.stmts.items[i], inside_arrow, info);
            break;
        case AST_EXPR_STMT:
        case AST_RETURN_STMT:
        case AST_THROW_STMT:
        case AST_AWAIT_EXPR:
        case AST_SPREAD_ELEM:
        case AST_REST_ELEMENT:
            scan_fn_ast(n->u.expr_stmt.expr, inside_arrow, info);
            break;
        case AST_IF_STMT:
            scan_fn_ast(n->u.if_stmt.test, inside_arrow, info);
            scan_fn_ast(n->u.if_stmt.conseq, inside_arrow, info);
            scan_fn_ast(n->u.if_stmt.alt, inside_arrow, info);
            break;
        case AST_WHILE_STMT:
        case AST_DO_WHILE_STMT:
            scan_fn_ast(n->u.loop.test, inside_arrow, info);
            scan_fn_ast(n->u.loop.body, inside_arrow, info);
            break;
        case AST_FOR_STMT:
            scan_fn_ast(n->u.for_stmt.init, inside_arrow, info);
            scan_fn_ast(n->u.for_stmt.test, inside_arrow, info);
            scan_fn_ast(n->u.for_stmt.update, inside_arrow, info);
            scan_fn_ast(n->u.for_stmt.body, inside_arrow, info);
            break;
        case AST_FOR_IN_OF_STMT:
            scan_fn_ast(n->u.for_in_of.left, inside_arrow, info);
            scan_fn_ast(n->u.for_in_of.right, inside_arrow, info);
            scan_fn_ast(n->u.for_in_of.body, inside_arrow, info);
            break;
        case AST_SWITCH_STMT:
            scan_fn_ast(n->u.switch_stmt.discriminant, inside_arrow, info);
            for (uint32_t i = 0; i < n->u.switch_stmt.cases.count; i++) {
                AjsAstNode *sc = n->u.switch_stmt.cases.items[i];
                scan_fn_ast(sc->u.switch_case.test, inside_arrow, info);
                for (uint32_t j = 0; j < sc->u.switch_case.conseq.count; j++)
                    scan_fn_ast(sc->u.switch_case.conseq.items[j], inside_arrow, info);
            }
            break;
        case AST_TRY_STMT:
            scan_fn_ast(n->u.try_stmt.block, inside_arrow, info);
            scan_fn_ast(n->u.try_stmt.catch_param, inside_arrow, info);
            scan_fn_ast(n->u.try_stmt.catch_body, inside_arrow, info);
            scan_fn_ast(n->u.try_stmt.finally_body, inside_arrow, info);
            break;
        case AST_LABELED_STMT:
            scan_fn_ast(n->u.labeled.body, inside_arrow, info);
            break;
        case AST_VAR_DECL:
            for (uint32_t i = 0; i < n->u.var_decl.decls.count; i++)
                scan_fn_ast(n->u.var_decl.decls.items[i], inside_arrow, info);
            break;
        case AST_VAR_DECLARATOR:
            scan_fn_ast(n->u.declarator.id, inside_arrow, info);
            scan_fn_ast(n->u.declarator.init, inside_arrow, info);
            break;
        case AST_ARRAY_EXPR:
        case AST_ARRAY_PATTERN:
            for (uint32_t i = 0; i < n->u.array.elems.count; i++)
                scan_fn_ast(n->u.array.elems.items[i], inside_arrow, info);
            break;
        case AST_OBJECT_EXPR:
        case AST_OBJECT_PATTERN:
            for (uint32_t i = 0; i < n->u.object.props.count; i++) {
                AjsAstNode *p = n->u.object.props.items[i];
                if (!p) continue;
                if (p->kind == AST_SPREAD_ELEM || p->kind == AST_REST_ELEMENT) {
                    scan_fn_ast(p->u.expr_stmt.expr, inside_arrow, info);
                } else {
                    if (p->u.prop.computed) scan_fn_ast(p->u.prop.key, inside_arrow, info);
                    scan_fn_ast(p->u.prop.value, inside_arrow, info);
                }
            }
            break;
        case AST_ASSIGN_PATTERN:
            scan_fn_ast(n->u.assign_pat.left, inside_arrow, info);
            scan_fn_ast(n->u.assign_pat.right, inside_arrow, info);
            break;
        case AST_TEMPLATE_LIT:
            for (uint32_t i = 0; i < n->u.tpl.exprs.count; i++)
                scan_fn_ast(n->u.tpl.exprs.items[i], inside_arrow, info);
            break;
        case AST_TAGGED_TEMPLATE:
            scan_fn_ast(n->u.tagged_tpl.tag, inside_arrow, info);
            scan_fn_ast(n->u.tagged_tpl.quasi, inside_arrow, info);
            break;
        case AST_MEMBER_EXPR:
            scan_fn_ast(n->u.member.object, inside_arrow, info);
            if (n->u.member.computed) scan_fn_ast(n->u.member.property, inside_arrow, info);
            break;
        case AST_NEW_EXPR:
            scan_fn_ast(n->u.call.callee, inside_arrow, info);
            for (uint32_t i = 0; i < n->u.call.args.count; i++)
                scan_fn_ast(n->u.call.args.items[i], inside_arrow, info);
            break;
        case AST_DYNAMIC_IMPORT:
            scan_fn_ast(n->u.expr_stmt.expr, inside_arrow, info);
            break;
        case AST_OPT_CHAIN_EXPR:
            scan_fn_ast(n->u.opt_chain.inner, inside_arrow, info);
            break;
        case AST_UNARY_EXPR:
        case AST_UPDATE_EXPR:
            scan_fn_ast(n->u.unary.arg, inside_arrow, info);
            break;
        case AST_BINARY_EXPR:
        case AST_LOGICAL_EXPR:
        case AST_ASSIGN_EXPR:
            scan_fn_ast(n->u.binary.left, inside_arrow, info);
            scan_fn_ast(n->u.binary.right, inside_arrow, info);
            break;
        case AST_COND_EXPR:
            scan_fn_ast(n->u.cond.test, inside_arrow, info);
            scan_fn_ast(n->u.cond.conseq, inside_arrow, info);
            scan_fn_ast(n->u.cond.alt, inside_arrow, info);
            break;
        case AST_SEQ_EXPR:
            for (uint32_t i = 0; i < n->u.seq.exprs.count; i++)
                scan_fn_ast(n->u.seq.exprs.items[i], inside_arrow, info);
            break;
        case AST_YIELD_EXPR:
            scan_fn_ast(n->u.yield_expr.arg, inside_arrow, info);
            break;
        case AST_EXPORT_NAMED_DECL:
        case AST_EXPORT_DEFAULT_DECL:
            scan_fn_ast(n->u.mod_decl.decl, inside_arrow, info);
            break;
        default:
            break;
    }
}

/* ============================================================================
 * 4. Lexical Scope & Binding Resolution
 * ============================================================================ */

static void comp_scope_cleanup(AjsCompScope *sc) {
    if (!sc) return;
    free(sc->locals);
    free(sc->hash_tab);
    sc->locals = NULL;
    sc->hash_tab = NULL;
    sc->slot_count = 0;
    sc->slot_cap = 0;
    sc->hash_cap = 0;
}

static void comp_scope_rebuild_hash(AjsCompScope *sc, uint32_t ncap) {
    int32_t *nt = (int32_t *)malloc(ncap * sizeof(int32_t));
    if (!nt) return;
    for (uint32_t i = 0; i < ncap; i++) nt[i] = -1;
    uint32_t mask = ncap - 1u;
    for (uint32_t i = 0; i < sc->slot_count; i++) {
        if (!sc->locals[i].u16) continue;
        uint32_t b = sc->locals[i].hash & mask;
        sc->locals[i].hash_next = nt[b];
        nt[b] = (int32_t)i;
    }
    free(sc->hash_tab);
    sc->hash_tab = nt;
    sc->hash_cap = ncap;
}

static int32_t comp_scope_find_own(AjsCompScope *sc, const uint16_t *u16, uint32_t len, bool is_priv) {
    uint32_t h = ajs_hash_utf16(u16, len);
    if (sc->hash_tab) {
        for (int32_t i = sc->hash_tab[h & (sc->hash_cap - 1u)]; i >= 0; i = sc->locals[i].hash_next) {
            if (sc->locals[i].is_priv == is_priv && sc->locals[i].hash == h && sc->locals[i].u16 &&
                c_u16_eq(sc->locals[i].u16, sc->locals[i].len, u16, len)) {
                return i;
            }
        }
        return -1;
    }
    for (uint32_t i = 0; i < sc->slot_count; i++) {
        if (sc->locals[i].is_priv == is_priv && sc->locals[i].hash == h && sc->locals[i].u16 &&
            c_u16_eq(sc->locals[i].u16, sc->locals[i].len, u16, len)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static uint32_t comp_scope_add(AjsCompiler *c, AjsCompScope *sc, const uint16_t *u16, uint32_t len,
                               AjsBindKind kind, bool is_priv) {
    if (!is_priv && u16) {
        int32_t ex = comp_scope_find_own(sc, u16, len, false);
        if (ex >= 0) {
            return sc->locals[ex].slot;
        }
    }
    if (sc->slot_count >= sc->slot_cap) {
        uint32_t ncap = sc->slot_cap ? (sc->slot_cap * 2u) : 8u;
        AjsCompLocal *nl = (AjsCompLocal *)realloc(sc->locals, ncap * sizeof(AjsCompLocal));
        if (!nl) { comp_error(c, 1, 1, "Out of memory"); return 0; }
        sc->locals = nl;
        sc->slot_cap = ncap;
    }
    uint32_t slot = sc->slot_count++;
    uint32_t h = u16 ? ajs_hash_utf16(u16, len) : 0u;
    AjsCompLocal *loc = &sc->locals[slot];
    memset(loc, 0, sizeof(*loc));
    loc->u16 = u16;
    loc->len = len;
    loc->hash = h;
    loc->hash_next = -1;
    loc->str = u16 ? ajs_str_from_utf16(c->vm, u16, len, true) : NULL;
    loc->slot = slot;
    loc->kind = kind;
    loc->is_priv = is_priv;
    if (u16) {
        if (sc->hash_tab) {
            if (sc->slot_count > sc->hash_cap) {
                comp_scope_rebuild_hash(sc, sc->hash_cap * 2u);
            } else {
                uint32_t b = h & (sc->hash_cap - 1u);
                loc->hash_next = sc->hash_tab[b];
                sc->hash_tab[b] = (int32_t)slot;
            }
        } else if (sc->slot_count >= 24u) {
            comp_scope_rebuild_hash(sc, 64u);
        }
    }
    return slot;
}

static bool resolve_binding(AjsCompiler *c, const uint16_t *u16, uint32_t len, bool is_priv,
                            uint16_t *out_depth, uint32_t *out_slot, AjsBindKind *out_kind) {
    uint16_t depth = 0;
    AjsCompiler *cur_c = c;
    while (cur_c) {
        /* Count runtime envs inside cur_c from innermost scope down to base_scope */
        for (AjsCompScope *sc = cur_c->scope; sc; sc = sc->parent) {
            int32_t idx = comp_scope_find_own(sc, u16, len, is_priv);
            if (idx >= 0) {
                uint16_t local_depth = (uint16_t)(cur_c->cur_env_depth - sc->env_depth_idx);
                *out_depth = (uint16_t)(depth + local_depth);
                *out_slot = sc->locals[idx].slot;
                if (out_kind) *out_kind = sc->locals[idx].kind;
                return true;
            }
        }
        /* Stepping across a closure boundary adds cur_c's active env_depth + 1 (for cur_c's base_env) */
        depth = (uint16_t)(depth + cur_c->cur_env_depth + 1u);
        cur_c = cur_c->enclosing;
    }
    return false;
}

/* Resolve non-arrow enclosing function's special lexical slots (<this>, <new.target>, <arguments>, <active_fn>) */
static bool resolve_special_fn_slots(AjsCompiler *c, uint16_t *out_depth, uint32_t *out_this_slot,
                                     uint32_t *out_nt_slot, uint32_t *out_args_slot, uint32_t *out_fn_slot) {
    uint16_t depth = c->cur_env_depth;
    AjsCompiler *cur_c = c;
    while (cur_c) {
        AjsFuncKind fk = cur_c->bc->fn_kind;
        if (fk != AJS_FN_BC_ARROW && fk != AJS_FN_BC_ASYNC_ARROW) {
            if (out_depth) *out_depth = depth;
            if (out_this_slot) *out_this_slot = cur_c->bc->this_slot;
            if (out_nt_slot) *out_nt_slot = cur_c->bc->new_target_slot;
            if (out_args_slot) *out_args_slot = cur_c->bc->arguments_slot;
            if (out_fn_slot) *out_fn_slot = cur_c->bc->active_fn_slot;
            return true;
        }
        if (!cur_c->enclosing) break;
        depth = (uint16_t)(depth + 1u + cur_c->enclosing->cur_env_depth);
        cur_c = cur_c->enclosing;
    }
    return false;
}

static uint32_t build_env_desc(AjsCompiler *c, AjsCompScope *sc) {
    bool need_names = c->has_eval_or_with;
    AjsBytecode *bc = c->bc;
    uint32_t idx = bc->env_desc_count++;
    bc->env_descs = (AjsEnvNamesDesc *)realloc(bc->env_descs, bc->env_desc_count * sizeof(AjsEnvNamesDesc));
    AjsEnvNamesDesc *ed = &bc->env_descs[idx];
    ed->count = sc->slot_count;
    ed->slot_flags = (uint8_t *)calloc(sc->slot_count ? sc->slot_count : 1u, 1);
    ed->names = need_names ? (AjsString **)calloc(sc->slot_count ? sc->slot_count : 1u, sizeof(AjsString *)) : NULL;
    for (uint32_t i = 0; i < sc->slot_count; i++) {
        uint8_t fl = 0;
        if (sc->locals[i].kind == BIND_CONST) fl |= AJS_SLOT_CONST | AJS_SLOT_INIT_TDZ;
        else if (sc->locals[i].kind == BIND_LET) fl |= AJS_SLOT_INIT_TDZ;
        else if (sc->locals[i].kind == BIND_FUNC_NAME) fl |= AJS_SLOT_FUNC_NAME;
        ed->slot_flags[i] = fl;
        if (need_names) ed->names[i] = sc->locals[i].str;
    }
    return idx;
}

static void collect_pattern_vars(AjsCompiler *c, AjsCompScope *sc, AjsAstNode *pat, AjsBindKind kind, bool to_global) {
    if (!pat) return;
    switch (pat->kind) {
        case AST_IDENT:
            if (to_global) {
                uint32_t nidx = add_str_u16(c, pat->u.ident.u16, pat->u.ident.len);
                if (kind == BIND_VAR) {
                    emit_op(c, OP_DEF_GLOBAL_VAR);
                    emit_u32(c, nidx);
                    emit_u8(c, 0);
                } else {
                    emit_op(c, OP_DEF_GLOBAL_LEX);
                    emit_u32(c, nidx);
                    emit_u8(c, kind == BIND_CONST ? 1 : 0);
                }
            } else {
                comp_scope_add(c, sc, pat->u.ident.u16, pat->u.ident.len, kind, false);
            }
            break;
        case AST_ASSIGN_PATTERN:
            collect_pattern_vars(c, sc, pat->u.assign_pat.left, kind, to_global);
            break;
        case AST_REST_ELEMENT:
            collect_pattern_vars(c, sc, pat->u.expr_stmt.expr, kind, to_global);
            break;
        case AST_ARRAY_PATTERN:
            for (uint32_t i = 0; i < pat->u.array.elems.count; i++)
                collect_pattern_vars(c, sc, pat->u.array.elems.items[i], kind, to_global);
            break;
        case AST_OBJECT_PATTERN:
            for (uint32_t i = 0; i < pat->u.object.props.count; i++) {
                AjsAstNode *p = pat->u.object.props.items[i];
                if (!p) continue;
                if (p->kind == AST_REST_ELEMENT)
                    collect_pattern_vars(c, sc, p->u.expr_stmt.expr, kind, to_global);
                else
                    collect_pattern_vars(c, sc, p->u.prop.value, kind, to_global);
            }
            break;
        default:
            break;
    }
}

/* Hoist `var` declarations across statements within the same function/script/module */
static void hoist_vars_in_stmt(AjsCompiler *c, AjsAstNode *n, bool to_global) {
    if (!n) return;
    switch (n->kind) {
        case AST_VAR_DECL:
            if (n->u.var_decl.var_kind == AJS_VAR_VAR) {
                for (uint32_t i = 0; i < n->u.var_decl.decls.count; i++) {
                    AjsAstNode *d = n->u.var_decl.decls.items[i];
                    collect_pattern_vars(c, &c->base_scope, d->u.declarator.id, BIND_VAR, to_global);
                }
            }
            break;
        case AST_BLOCK: {
            /* Annex B.3.3: sloppy block-level plain function also creates var binding if no lex conflict */
            AjsCompScope blk_lex = {0};
            bool has_sloppy_fn = false;
            if (!c->strict) {
                for (uint32_t i = 0; i < n->u.block.stmts.count; i++) {
                    AjsAstNode *st = n->u.block.stmts.items[i];
                    if (st && st->kind == AST_FUNC_DECL &&
                        st->u.func.fn_kind == AJS_FN_BC_NORMAL && st->u.func.name_u16) {
                        has_sloppy_fn = true;
                        break;
                    }
                }
                if (has_sloppy_fn) {
                    for (uint32_t j = 0; j < n->u.block.stmts.count; j++) {
                        AjsAstNode *s2 = n->u.block.stmts.items[j];
                        if (s2 && s2->kind == AST_VAR_DECL && s2->u.var_decl.var_kind != AJS_VAR_VAR) {
                            for (uint32_t k = 0; k < s2->u.var_decl.decls.count; k++) {
                                collect_pattern_vars(c, &blk_lex, s2->u.var_decl.decls.items[k]->u.declarator.id, BIND_LET, false);
                            }
                        } else if (s2 && s2->kind == AST_CLASS_DECL && s2->u.klass.name_u16) {
                            comp_scope_add(c, &blk_lex, s2->u.klass.name_u16, s2->u.klass.name_len, BIND_LET, false);
                        }
                    }
                }
            }
            for (uint32_t i = 0; i < n->u.block.stmts.count; i++) {
                AjsAstNode *st = n->u.block.stmts.items[i];
                if (!c->strict && st && st->kind == AST_FUNC_DECL &&
                    st->u.func.fn_kind == AJS_FN_BC_NORMAL && st->u.func.name_u16) {
                    bool lex_conflict = (comp_scope_find_own(&blk_lex, st->u.func.name_u16, st->u.func.name_len, false) >= 0);
                    if (!lex_conflict) {
                        if (to_global) {
                            uint32_t nidx = add_str_u16(c, st->u.func.name_u16, st->u.func.name_len);
                            emit_op(c, OP_DEF_GLOBAL_VAR);
                            emit_u32(c, nidx);
                            emit_u8(c, 0);
                        } else {
                            comp_scope_add(c, &c->base_scope, st->u.func.name_u16, st->u.func.name_len, BIND_VAR, false);
                        }
                    }
                }
                hoist_vars_in_stmt(c, st, to_global);
            }
            comp_scope_cleanup(&blk_lex);
            break;
        }
        case AST_IF_STMT:
            if (!c->strict) {
                if (n->u.if_stmt.conseq && n->u.if_stmt.conseq->kind == AST_FUNC_DECL && n->u.if_stmt.conseq->u.func.name_u16) {
                    if (to_global) {
                        uint32_t nidx = add_str_u16(c, n->u.if_stmt.conseq->u.func.name_u16, n->u.if_stmt.conseq->u.func.name_len);
                        emit_op(c, OP_DEF_GLOBAL_VAR); emit_u32(c, nidx); emit_u8(c, 0);
                    } else {
                        comp_scope_add(c, &c->base_scope, n->u.if_stmt.conseq->u.func.name_u16, n->u.if_stmt.conseq->u.func.name_len, BIND_VAR, false);
                    }
                }
                if (n->u.if_stmt.alt && n->u.if_stmt.alt->kind == AST_FUNC_DECL && n->u.if_stmt.alt->u.func.name_u16) {
                    if (to_global) {
                        uint32_t nidx = add_str_u16(c, n->u.if_stmt.alt->u.func.name_u16, n->u.if_stmt.alt->u.func.name_len);
                        emit_op(c, OP_DEF_GLOBAL_VAR); emit_u32(c, nidx); emit_u8(c, 0);
                    } else {
                        comp_scope_add(c, &c->base_scope, n->u.if_stmt.alt->u.func.name_u16, n->u.if_stmt.alt->u.func.name_len, BIND_VAR, false);
                    }
                }
            }
            hoist_vars_in_stmt(c, n->u.if_stmt.conseq, to_global);
            hoist_vars_in_stmt(c, n->u.if_stmt.alt, to_global);
            break;
        case AST_WHILE_STMT:
        case AST_DO_WHILE_STMT:
            hoist_vars_in_stmt(c, n->u.loop.body, to_global);
            break;
        case AST_FOR_STMT:
            hoist_vars_in_stmt(c, n->u.for_stmt.init, to_global);
            hoist_vars_in_stmt(c, n->u.for_stmt.body, to_global);
            break;
        case AST_FOR_IN_OF_STMT:
            hoist_vars_in_stmt(c, n->u.for_in_of.left, to_global);
            hoist_vars_in_stmt(c, n->u.for_in_of.body, to_global);
            break;
        case AST_SWITCH_STMT:
            for (uint32_t i = 0; i < n->u.switch_stmt.cases.count; i++) {
                AjsAstNode *sc = n->u.switch_stmt.cases.items[i];
                for (uint32_t j = 0; j < sc->u.switch_case.conseq.count; j++)
                    hoist_vars_in_stmt(c, sc->u.switch_case.conseq.items[j], to_global);
            }
            break;
        case AST_TRY_STMT:
            hoist_vars_in_stmt(c, n->u.try_stmt.block, to_global);
            hoist_vars_in_stmt(c, n->u.try_stmt.catch_body, to_global);
            hoist_vars_in_stmt(c, n->u.try_stmt.finally_body, to_global);
            break;
        case AST_WITH_STMT:
            hoist_vars_in_stmt(c, n->u.with_stmt.body, to_global);
            break;
        case AST_LABELED_STMT:
            hoist_vars_in_stmt(c, n->u.labeled.body, to_global);
            break;
        case AST_EXPORT_NAMED_DECL:
            hoist_vars_in_stmt(c, n->u.mod_decl.decl, to_global);
            break;
        default:
            break;
    }
}

/* Scan immediate statement list for block-scoped `let`/`const`/`using`/`class`/`function` declarations */
static void collect_block_lex_in_list(AjsCompiler *c, AjsCompScope *sc, const AjsNodeList *list, bool to_global) {
    for (uint32_t i = 0; i < list->count; i++) {
        AjsAstNode *st = list->items[i];
        if (!st) continue;
        if (st->kind == AST_EXPORT_NAMED_DECL && st->u.mod_decl.decl) st = st->u.mod_decl.decl;
        if (st->kind == AST_VAR_DECL && st->u.var_decl.var_kind != AJS_VAR_VAR) {
            AjsVarKind vk = st->u.var_decl.var_kind;
            AjsBindKind bk = is_const_var_kind(vk) ? BIND_CONST : BIND_LET;
            for (uint32_t j = 0; j < st->u.var_decl.decls.count; j++) {
                collect_pattern_vars(c, sc, st->u.var_decl.decls.items[j]->u.declarator.id, bk, to_global);
            }
            if ((vk == AJS_VAR_USING || vk == AJS_VAR_AWAIT_USING) && !sc->has_disp_stack) {
                sc->disp_slot = comp_scope_add(c, sc, NULL, 0, BIND_VAR, false);
                sc->has_disp_stack = true;
            }
        } else if (st->kind == AST_CLASS_DECL && st->u.klass.name_u16) {
            if (to_global) {
                uint32_t nidx = add_str_u16(c, st->u.klass.name_u16, st->u.klass.name_len);
                emit_op(c, OP_DEF_GLOBAL_LEX);
                emit_u32(c, nidx);
                emit_u8(c, 0);
            } else {
                comp_scope_add(c, sc, st->u.klass.name_u16, st->u.klass.name_len, BIND_LET, false);
            }
        } else if (st->kind == AST_FUNC_DECL && st->u.func.name_u16) {
            if (sc->is_base) {
                if (to_global) {
                    uint32_t nidx = add_str_u16(c, st->u.func.name_u16, st->u.func.name_len);
                    emit_op(c, OP_DEF_GLOBAL_VAR);
                    emit_u32(c, nidx);
                    emit_u8(c, 1);
                } else {
                    comp_scope_add(c, sc, st->u.func.name_u16, st->u.func.name_len, BIND_VAR, false);
                }
            } else {
                comp_scope_add(c, sc, st->u.func.name_u16, st->u.func.name_len, BIND_VAR, false);
            }
        }
    }
}

static inline bool compiler_allows_await(const AjsCompiler *c) {
    return c->bc->fn_kind == AJS_FN_BC_ASYNC ||
           c->bc->fn_kind == AJS_FN_BC_ASYNC_ARROW ||
           c->bc->fn_kind == AJS_FN_BC_ASYNC_GENERATOR;
}

static uint32_t emit_dispose_enter(AjsCompiler *c, uint16_t lex_depth, uint32_t disp_slot) {
    emit_op(c, OP_DISPOSE_NEW);
    emit_init_lex(c, lex_depth, disp_slot);
    emit_op(c, OP_POP);
    emit_op(c, OP_TRY_PUSH);
    emit_u32(c, UINT32_MAX);
    uint32_t fin_patch = c->bc->code_len;
    emit_u32(c, 0);
    c->cur_try_depth++;
    return fin_patch;
}

static void emit_dispose_finally(AjsCompiler *c, uint16_t lex_depth, uint32_t disp_slot, uint32_t fin_patch) {
    bool allow_await = compiler_allows_await(c);
    c->cur_try_depth--;
    patch_jump(c, fin_patch);
    emit_op(c, OP_ENTER_FINALLY);

    uint32_t loop_top_pc = c->bc->code_len;
    emit_op(c, OP_TRY_PUSH);
    uint32_t step_catch_patch = c->bc->code_len;
    emit_u32(c, 0);
    emit_u32(c, UINT32_MAX);

    emit_get_lex(c, lex_depth, disp_slot);
    emit_op(c, OP_DISPOSE_STEP);

    emit_op(c, OP_DUP);
    emit_op(c, OP_CONST);
    emit_u32(c, add_const(c, ajs_val_number(0.0)));
    emit_op(c, OP_STRICT_EQ);
    uint32_t j_done = emit_jump(c, OP_JMP_TRUE);

    if (allow_await) {
        emit_op(c, OP_CONST);
        emit_u32(c, add_const(c, ajs_val_number(1.0)));
        emit_op(c, OP_STRICT_EQ);
        uint32_t j_sync = emit_jump(c, OP_JMP_FALSE);
        emit_op(c, OP_AWAIT);
        patch_jump(c, j_sync);
        emit_op(c, OP_POP);
    } else {
        emit_op(c, OP_POP);
        emit_op(c, OP_POP);
    }
    emit_op(c, OP_TRY_POP);
    emit_op(c, OP_JMP);
    emit_u32(c, loop_top_pc);

    patch_jump(c, step_catch_patch);
    emit_get_lex(c, lex_depth, disp_slot);
    emit_op(c, OP_SWAP);
    emit_op(c, OP_DISPOSE_CATCH);
    emit_op(c, OP_JMP);
    emit_u32(c, loop_top_pc);

    patch_jump(c, j_done);
    emit_op(c, OP_POP);
    emit_op(c, OP_POP);
    emit_op(c, OP_TRY_POP);

    emit_get_lex(c, lex_depth, disp_slot);
    emit_op(c, OP_DISPOSE_FINISH);
    emit_op(c, OP_FINALLY_END);
}

/* Instantiate hoisted function declarations in the current scope */
static bool instantiate_hoisted_funcs(AjsCompiler *c, AjsCompScope *sc, const AjsNodeList *list, bool to_global) {
    for (uint32_t i = 0; i < list->count; i++) {
        AjsAstNode *st = list->items[i];
        if (!st) continue;
        bool is_export_def = false;
        if (st->kind == AST_EXPORT_NAMED_DECL && st->u.mod_decl.decl) {
            st = st->u.mod_decl.decl;
        } else if (st->kind == AST_EXPORT_DEFAULT_DECL && st->u.mod_decl.decl &&
                   st->u.mod_decl.decl->kind == AST_FUNC_DECL) {
            st = st->u.mod_decl.decl;
            is_export_def = true;
        }
        if (st->kind != AST_FUNC_DECL) continue;

        AjsBytecode *fn_bc = compile_func_node(c, st, is_export_def ? "default" : NULL, false);
        if (!fn_bc || c->had_error) return false;

        /* For ES Module top-level function declarations, record in mod->hoisted_fns so
         * `ModuleDeclarationInstantiation` (link phase) initializes live bindings BEFORE evaluation! */
        if (c->mod && sc->is_base) {
            const uint16_t *bname = st->u.func.name_u16;
            uint32_t blen = st->u.func.name_len;
            static const uint16_t u_def_star[9] = {'*','d','e','f','a','u','l','t','*'};
            if (is_export_def && !bname) {
                bname = u_def_star;
                blen = 9;
            }
            int32_t lidx = comp_scope_find_own(sc, bname, blen, false);
            if (lidx >= 0) {
                uint32_t hc = c->mod->hoisted_fn_count++;
                c->mod->hoisted_fns = (AjsFuncInitEntry *)realloc(
                    c->mod->hoisted_fns, c->mod->hoisted_fn_count * sizeof(AjsFuncInitEntry));
                c->mod->hoisted_fns[hc].slot = sc->locals[lidx].slot;
                c->mod->hoisted_fns[hc].fn_bc = fn_bc;
            }
            continue;
        }

        uint32_t bc_idx = add_const(c, ajs_val_from_gc(fn_bc));
        emit_op(c, OP_CLOSURE);
        emit_u32(c, bc_idx);

        if (to_global && sc->is_base) {
            uint32_t nidx = add_str_u16(c, st->u.func.name_u16, st->u.func.name_len);
            emit_op(c, OP_SET_GLOBAL);
            emit_u32(c, nidx);
            emit_u8(c, 0);
            emit_op(c, OP_POP);
        } else if (st->u.func.name_u16) {
            int32_t lidx = comp_scope_find_own(sc, st->u.func.name_u16, st->u.func.name_len, false);
            if (lidx >= 0) {
                emit_init_lex(c, 0, sc->locals[lidx].slot);
            }
            emit_op(c, OP_POP);
        } else {
            emit_op(c, OP_POP);
        }
    }
    return true;
}

/* ============================================================================
 * 5. Identifier Load / Store & Destructuring Assignment Compilation
 * ============================================================================ */

static bool is_anon_fn_or_class_def(AjsAstNode *n) {
    if (!n || n->parenthesized) return false;
    if (n->kind == AST_FUNC_EXPR) return n->u.func.name_u16 == NULL;
    if (n->kind == AST_CLASS_EXPR) return n->u.klass.name_u16 == NULL;
    return false;
}

static void emit_load_ident(AjsCompiler *c, const uint16_t *u16, uint32_t len, bool typeof_mode) {
    /* Special `arguments` identifier inside functions */
    if (c_u16_eq_ascii(u16, len, "arguments")) {
        uint16_t depth = 0;
        uint32_t slot = 0;
        AjsBindKind bk;
        if (resolve_binding(c, u16, len, false, &depth, &slot, &bk)) {
            emit_get_lex(c, depth, slot);
            return;
        }
        uint16_t sp_depth = 0;
        uint32_t args_slot = AJS_NO_SLOT;
        if (resolve_special_fn_slots(c, &sp_depth, NULL, NULL, &args_slot, NULL)) {
            if (args_slot != AJS_NO_SLOT) {
                emit_get_lex(c, sp_depth, args_slot);
                return;
            }
            if (sp_depth == c->cur_env_depth && c->bc->fn_kind != AJS_FN_BC_ARROW &&
                c->bc->fn_kind != AJS_FN_BC_ASYNC_ARROW) {
                emit_op(c, OP_ARGUMENTS);
                return;
            }
        }
    }

    if (c->with_depth > 0 || (c->has_eval_or_with && !c->strict)) {
        uint32_t nidx = add_str_u16(c, u16, len);
        emit_op(c, OP_GET_DYN_NAME);
        emit_u32(c, nidx);
        emit_u8(c, typeof_mode ? 1 : 0);
        return;
    }

    uint16_t depth = 0;
    uint32_t slot = 0;
    AjsBindKind bk;
    if (resolve_binding(c, u16, len, false, &depth, &slot, &bk)) {
        emit_get_lex(c, depth, slot);
        return;
    }

    uint32_t nidx = add_str_u16(c, u16, len);
    emit_op(c, OP_GET_GLOBAL);
    emit_u32(c, nidx);
    emit_u8(c, typeof_mode ? 1 : 0);
}

/* Expects value on top of stack; leaves value on top of stack! */
static void emit_store_ident(AjsCompiler *c, const uint16_t *u16, uint32_t len, bool is_init, AjsVarKind vk) {
    if (c->with_depth > 0 || (c->has_eval_or_with && !c->strict && !is_init)) {
        uint32_t nidx = add_str_u16(c, u16, len);
        emit_op(c, OP_SET_DYN_NAME);
        emit_u32(c, nidx);
        emit_u8(c, c->strict ? 1 : 0);
        return;
    }

    if (is_init && vk != AJS_VAR_VAR) {
        /* Lexical initialization in current scope (or global_lex_env if top-level script) */
        int32_t own_idx = comp_scope_find_own(c->scope, u16, len, false);
        if (own_idx >= 0) {
            uint16_t init_depth = (uint16_t)(c->cur_env_depth - c->scope->env_depth_idx);
            emit_init_lex(c, init_depth, c->scope->locals[own_idx].slot);
            return;
        }
        if (c->is_script_global && c->scope->is_base) {
            uint32_t nidx = add_str_u16(c, u16, len);
            emit_op(c, OP_INIT_GLOBAL_LEX);
            emit_u32(c, nidx);
            return;
        }
    }

    uint16_t depth = 0;
    uint32_t slot = 0;
    AjsBindKind bk;
    if (resolve_binding(c, u16, len, false, &depth, &slot, &bk)) {
        if (is_init && vk == AJS_VAR_VAR && (bk == BIND_VAR || bk == BIND_PARAM)) {
            emit_init_lex(c, depth, slot);
        } else {
            emit_set_lex(c, depth, slot, c->strict ? 1 : 0);
        }
        return;
    }

    uint32_t nidx = add_str_u16(c, u16, len);
    emit_op(c, OP_SET_GLOBAL);
    emit_u32(c, nidx);
    emit_u8(c, c->strict ? 1 : 0);
}

/* Consumes value on top of stack and assigns into pattern/LHS `pat`! */
static bool compile_pattern_assign(AjsCompiler *c, AjsAstNode *pat, bool is_init, AjsVarKind vk) {
    if (!pat) {
        emit_op(c, OP_POP);
        return true;
    }
    emit_line_pos(c, pat->line, pat->col);

    /* Default value wrapper `lhs = defaultExpr` */
    if (pat->kind == AST_ASSIGN_PATTERN) {
        emit_op(c, OP_DUP);
        uint32_t j_not_undef = emit_jump(c, OP_JMP_UNDEF);
        uint32_t j_done = emit_jump(c, OP_JMP);
        patch_jump(c, j_not_undef);
        emit_op(c, OP_POP);
        if (!compile_expr(c, pat->u.assign_pat.right)) return false;
        if (pat->u.assign_pat.left && pat->u.assign_pat.left->kind == AST_IDENT &&
            is_anon_fn_or_class_def(pat->u.assign_pat.right)) {
            uint32_t nidx = add_str_u16(c, pat->u.assign_pat.left->u.ident.u16, pat->u.assign_pat.left->u.ident.len);
            emit_op(c, OP_CONST);
            emit_u32(c, nidx);
            emit_op(c, OP_SET_FN_NAME);
            emit_u8(c, 0);
        }
        patch_jump(c, j_done);
        return compile_pattern_assign(c, pat->u.assign_pat.left, is_init, vk);
    }

    if (pat->kind == AST_IDENT) {
        emit_store_ident(c, pat->u.ident.u16, pat->u.ident.len, is_init, vk);
        emit_op(c, OP_POP);
        return true;
    }

    if (pat->kind == AST_MEMBER_EXPR) {
        /* Stack initially: `[rhs]` */
        AjsAstNode *obj = pat->u.member.object;
        AjsAstNode *prop = pat->u.member.property;
        if (obj && obj->kind == AST_SUPER_EXPR) {
            uint16_t sp_d = 0;
            uint32_t this_s = AJS_NO_SLOT, fn_s = AJS_NO_SLOT;
            resolve_special_fn_slots(c, &sp_d, &this_s, NULL, NULL, &fn_s);
            uint32_t fn_arg = (fn_s != AJS_NO_SLOT) ? fn_s : (sp_d == c->cur_env_depth ? AJS_NO_SLOT : fn_s);
            if (this_s != AJS_NO_SLOT) { emit_get_lex(c, sp_d, this_s); }
            else { emit_op(c, OP_THIS); }
            /* Stack: [rhs, this] */
            if (pat->u.member.computed) {
                if (!compile_expr(c, prop)) return false;
                /* Stack: [rhs, this, key] -> ROT3L -> [this, key, rhs] */
                emit_op(c, OP_ROT3L);
                emit_op(c, OP_SET_SUPER_ELEM);
                emit_u16(c, sp_d);
                emit_u32(c, fn_arg);
                emit_u8(c, c->strict ? 1 : 0);
            } else {
                /* Stack: [rhs, this] -> SWAP -> [this, rhs] */
                emit_op(c, OP_SWAP);
                uint32_t nidx = add_str_u16(c, prop->u.ident.u16, prop->u.ident.len);
                emit_op(c, OP_SET_SUPER_PROP);
                emit_u16(c, sp_d);
                emit_u32(c, fn_arg);
                emit_u32(c, nidx);
                emit_u8(c, c->strict ? 1 : 0);
            }
            emit_op(c, OP_POP);
            return true;
        }

        if (!compile_expr(c, obj)) return false;
        /* Stack: [rhs, obj] */
        if (pat->u.member.computed) {
            if (!compile_expr(c, prop)) return false;
            /* Stack: [rhs, obj, key] -> ROT3L -> [obj, key, rhs] */
            emit_op(c, OP_ROT3L);
            emit_op(c, OP_SET_ELEM);
            emit_u8(c, c->strict ? 1 : 0);
        } else if (prop->kind == AST_PRIV_IDENT) {
            emit_op(c, OP_SWAP); /* [obj, rhs] */
            uint16_t d = 0;
            uint32_t s = 0;
            if (!resolve_binding(c, prop->u.ident.u16, prop->u.ident.len, true, &d, &s, NULL)) {
                comp_error(c, prop->line, prop->col, "Unresolved private identifier");
                return false;
            }
            emit_op(c, OP_SET_PRIVATE);
            emit_u16(c, d);
            emit_u32(c, s);
        } else {
            emit_op(c, OP_SWAP); /* [obj, rhs] */
            uint32_t nidx = add_str_u16(c, prop->u.ident.u16, prop->u.ident.len);
            emit_op(c, OP_SET_PROP);
            emit_u32(c, nidx);
            emit_u8(c, c->strict ? 1 : 0);
        }
        emit_op(c, OP_POP);
        return true;
    }

    if (pat->kind == AST_ARRAY_PATTERN || pat->kind == AST_ARRAY_EXPR) {
        /* Allocate a 3-slot mini environment `[raw_iter, next_fn, active_iter]` so IteratorClose
         * runs automatically via try/finally if any inner assignment throws before exhaustion! */
        emit_op(c, OP_GET_ITER);
        emit_u8(c, 0); /* sync iterator: [iter, next_fn] */

        emit_push_env(c, 3, AJS_NO_SLOT);
        c->cur_env_depth++;

        /* Slot 1 = next_fn, Slot 0 = raw_iter, Slot 2 = active_iter (initially raw_iter) */
        emit_init_lex(c, 0, 1); emit_op(c, OP_POP);
        emit_op(c, OP_DUP);
        emit_init_lex(c, 0, 0); emit_op(c, OP_POP);
        emit_init_lex(c, 0, 2); emit_op(c, OP_POP);

        emit_op(c, OP_TRY_PUSH);
        emit_u32(c, UINT32_MAX);
        uint32_t fin_patch = c->bc->code_len;
        emit_u32(c, 0);
        c->cur_try_depth++;

        for (uint32_t i = 0; i < pat->u.array.elems.count; i++) {
            AjsAstNode *el = pat->u.array.elems.items[i];
            if (el && (el->kind == AST_REST_ELEMENT || el->kind == AST_SPREAD_ELEM)) {
                /* Exhaust remaining iterator into rest array; mark active_iter = undefined first! */
                emit_get_lex(c, 0, 2);
                uint32_t j_already_done = emit_jump(c, OP_JMP_UNDEF);
                emit_op(c, OP_UNDEF);
                emit_set_lex(c, 0, 2, 0); emit_op(c, OP_POP);
                emit_get_lex(c, 0, 0);
                emit_get_lex(c, 0, 1);
                emit_op(c, OP_REST_FROM_ITER);
                uint32_t j_after_rest = emit_jump(c, OP_JMP);
                patch_jump(c, j_already_done);
                emit_op(c, OP_NEW_ARRAY); emit_u32(c, 0);
                patch_jump(c, j_after_rest);
                if (!compile_pattern_assign(c, el->u.expr_stmt.expr, is_init, vk)) return false;
                break;
            }

            /* Step iterator if not already exhausted */
            emit_get_lex(c, 0, 2);
            uint32_t j_exhausted = emit_jump(c, OP_JMP_UNDEF);
            /* Temporarily clear active_iter while calling next() so a throw inside next() does NOT IteratorClose! */
            emit_op(c, OP_UNDEF);
            emit_set_lex(c, 0, 2, 0); emit_op(c, OP_POP);

            emit_get_lex(c, 0, 0);
            emit_get_lex(c, 0, 1);
            emit_op(c, OP_ITER_NEXT);
            /* [iter, next_fn, res_obj] -> NIP twice -> [res_obj] */
            emit_op(c, OP_NIP);
            emit_op(c, OP_NIP);
            emit_op(c, OP_ITER_UNPACK); /* [value, done_bool] */
            uint32_t j_step_done = emit_jump(c, OP_JMP_TRUE);
            /* Not done: restore active_iter = raw_iter */
            emit_get_lex(c, 0, 0);
            emit_set_lex(c, 0, 2, 0); emit_op(c, OP_POP);
            uint32_t j_have_val = emit_jump(c, OP_JMP);

            patch_jump(c, j_step_done);
            emit_op(c, OP_POP); /* discard value when done:true, push undefined */
            patch_jump(c, j_exhausted);
            emit_op(c, OP_UNDEF);

            patch_jump(c, j_have_val);
            if (!el) {
                emit_op(c, OP_POP);
            } else {
                if (!compile_pattern_assign(c, el, is_init, vk)) return false;
            }
        }

        c->cur_try_depth--;
        patch_jump(c, fin_patch);
        emit_op(c, OP_ENTER_FINALLY);
        emit_get_lex(c, 0, 2);
        uint32_t j_no_close = emit_jump(c, OP_JMP_UNDEF);
        emit_get_lex(c, 0, 2);
        emit_op(c, OP_ITER_CLOSE);
        patch_jump(c, j_no_close);
        emit_op(c, OP_FINALLY_END);

        emit_op(c, OP_POP_ENV);
        c->cur_env_depth--;
        return true;
    }

    if (pat->kind == AST_OBJECT_PATTERN || pat->kind == AST_OBJECT_EXPR) {
        emit_op(c, OP_REQ_OBJ_COERCIBLE);
        bool has_rest = false;
        for (uint32_t i = 0; i < pat->u.object.props.count; i++) {
            AjsAstNode *p = pat->u.object.props.items[i];
            if (p && (p->kind == AST_REST_ELEMENT || p->kind == AST_SPREAD_ELEM)) has_rest = true;
        }
        if (has_rest) {
            emit_op(c, OP_NEW_ARRAY); emit_u32(c, pat->u.object.props.count);
            /* Stack: [src, excl_arr] */
        }
        for (uint32_t i = 0; i < pat->u.object.props.count; i++) {
            AjsAstNode *p = pat->u.object.props.items[i];
            if (!p) continue;
            if (p->kind == AST_REST_ELEMENT || p->kind == AST_SPREAD_ELEM) {
                /* Stack: [src, excl_arr] -> OP_OBJECT_REST -> [rest_obj] */
                emit_op(c, OP_OBJECT_REST);
                if (!compile_pattern_assign(c, p->u.expr_stmt.expr, is_init, vk)) return false;
                return true;
            }
            /* Compute property key */
            if (has_rest) {
                /* Stack: [src, excl_arr] */
                if (p->u.prop.computed) {
                    if (!compile_expr(c, p->u.prop.key)) return false;
                    emit_op(c, OP_TO_PROP_KEY);
                } else if (p->u.prop.key->kind == AST_IDENT) {
                    emit_op(c, OP_CONST);
                    emit_u32(c, add_str_u16(c, p->u.prop.key->u.ident.u16, p->u.prop.key->u.ident.len));
                } else if (p->u.prop.key->kind == AST_LITERAL_STR) {
                    emit_op(c, OP_CONST);
                    emit_u32(c, add_str_u16(c, p->u.prop.key->u.lit.s_u16, p->u.prop.key->u.lit.s_len));
                } else if (p->u.prop.key->kind == AST_LITERAL_NUM) {
                    emit_op(c, OP_CONST);
                    emit_u32(c, add_const(c, ajs_val_number(p->u.prop.key->u.lit.nval)));
                    emit_op(c, OP_TO_PROP_KEY);
                } else {
                    if (!compile_expr(c, p->u.prop.key)) return false;
                    emit_op(c, OP_TO_PROP_KEY);
                }
                /* Stack: [src, excl_arr, key]. Push key into excl_arr AND read src[key]!
                 * DUP2 -> [src, excl_arr, key, excl_arr, key] -> ARRAY_PUSH -> [src, excl_arr, key, excl_arr] -> POP -> [src, excl_arr, key]
                 * Wait: we also need `src`! Let's use ROT3:
                 * [src, excl_arr, key] -> DUP2 -> [src, excl_arr, key, excl_arr, key] -> ARRAY_PUSH -> [src, excl_arr, key, excl_arr] -> POP -> [src, excl_arr, key]
                 * ROT3 -> [key, src, excl_arr] -> ROT3 -> [excl_arr, key, src] -> SWAP -> [excl_arr, src, key]
                 * DUP2 -> [excl_arr, src, key, src, key] -> NIP of key after GET_ELEM:
                 * Wait: [excl_arr, src, key] -> SWAP -> [excl_arr, key, src] -> DUP -> [excl_arr, key, src, src] -> ROT3 -> [excl_arr, src, key, src] -> SWAP -> [excl_arr, src, src, key] -> GET_ELEM -> [excl_arr, src, val] -> ROT3L -> [src, val, excl_arr] -> SWAP -> [src, excl_arr, val]!! */
                emit_op(c, OP_DUP2);
                emit_op(c, OP_ARRAY_PUSH);
                emit_op(c, OP_POP);        /* [src, excl_arr, key] */
                emit_op(c, OP_ROT3);       /* [key, src, excl_arr] */
                emit_op(c, OP_ROT3);       /* [excl_arr, key, src] */
                emit_op(c, OP_DUP);        /* [excl_arr, key, src, src] */
                emit_op(c, OP_ROT3);       /* [excl_arr, src, key, src] */
                emit_op(c, OP_SWAP);       /* [excl_arr, src, src, key] */
                emit_op(c, OP_GET_ELEM);   /* [excl_arr, src, val] */
                emit_op(c, OP_ROT3L);      /* [src, val, excl_arr] */
                emit_op(c, OP_SWAP);       /* [src, excl_arr, val] */
            } else {
                /* Stack: [src] */
                emit_op(c, OP_DUP);
                if (!p->u.prop.computed && p->u.prop.key->kind == AST_IDENT) {
                    emit_op(c, OP_GET_PROP);
                    emit_u32(c, add_str_u16(c, p->u.prop.key->u.ident.u16, p->u.prop.key->u.ident.len));
                } else {
                    if (p->u.prop.computed) {
                        if (!compile_expr(c, p->u.prop.key)) return false;
                    } else if (p->u.prop.key->kind == AST_LITERAL_STR) {
                        emit_op(c, OP_CONST);
                        emit_u32(c, add_str_u16(c, p->u.prop.key->u.lit.s_u16, p->u.prop.key->u.lit.s_len));
                    } else if (p->u.prop.key->kind == AST_LITERAL_NUM) {
                        emit_op(c, OP_CONST);
                        emit_u32(c, add_const(c, ajs_val_number(p->u.prop.key->u.lit.nval)));
                    } else {
                        if (!compile_expr(c, p->u.prop.key)) return false;
                    }
                    emit_op(c, OP_GET_ELEM);
                }
            }
            if (!compile_pattern_assign(c, p->u.prop.value, is_init, vk)) return false;
        }
        if (has_rest) emit_op(c, OP_POP);
        emit_op(c, OP_POP);
        return true;
    }

    comp_error(c, pat->line, pat->col, "Invalid assignment target");
    return false;
}

/* ============================================================================
 * 6. Expressions, Optional Chaining, Functions & Classes
 * ============================================================================ */

typedef struct AjsOptChainCtx {
    AjsJumpPatch *patches;
    uint32_t      count;
    uint32_t      cap;
} AjsOptChainCtx;

static void opt_chain_add_patch(AjsOptChainCtx *oc, uint32_t off, uint16_t extra_pops) {
    if (oc->count >= oc->cap) {
        uint32_t ncap = oc->cap ? oc->cap * 2u : 8u;
        oc->patches = (AjsJumpPatch *)realloc(oc->patches, ncap * sizeof(AjsJumpPatch));
        oc->cap = ncap;
    }
    oc->patches[oc->count].patch_off = off;
    oc->patches[oc->count].extra_pops = extra_pops;
    oc->count++;
}

static bool emit_call_args_list(AjsCompiler *c, const AjsNodeList *args, bool *out_has_spread) {
    bool has_spread = false;
    for (uint32_t i = 0; i < args->count; i++) {
        if (args->items[i] && args->items[i]->kind == AST_SPREAD_ELEM) {
            has_spread = true;
            break;
        }
    }
    *out_has_spread = has_spread;
    if (has_spread) {
        emit_op(c, OP_NEW_ARRAY);
        emit_u32(c, args->count);
        for (uint32_t i = 0; i < args->count; i++) {
            AjsAstNode *a = args->items[i];
            if (a->kind == AST_SPREAD_ELEM) {
                if (!compile_expr(c, a->u.expr_stmt.expr)) return false;
                emit_op(c, OP_ARRAY_SPREAD);
            } else {
                if (!compile_expr(c, a)) return false;
                emit_op(c, OP_ARRAY_PUSH);
            }
        }
    } else {
        for (uint32_t i = 0; i < args->count; i++) {
            if (!compile_expr(c, args->items[i])) return false;
        }
    }
    return true;
}

static bool compile_opt_chain_inner(AjsCompiler *c, AjsAstNode *n, AjsOptChainCtx *oc,
                                    bool want_recv_and_fn, bool is_delete) {
    if (!n) return false;
    emit_line_pos(c, n->line, n->col);

    if (!n->parenthesized && n->kind == AST_MEMBER_EXPR) {
        if (!compile_opt_chain_inner(c, n->u.member.object, oc, false, false)) return false;
        /* Stack: [obj] */
        if (n->u.member.optional) {
            emit_op(c, OP_DUP);
            uint32_t j = emit_jump(c, OP_JMP_NULLISH);
            opt_chain_add_patch(oc, j, 1);
        }
        if (is_delete) {
            if (n->u.member.computed) {
                if (!compile_expr(c, n->u.member.property)) return false;
                emit_op(c, OP_DELETE_ELEM);
                emit_u8(c, c->strict ? 1 : 0);
            } else {
                uint32_t nidx = add_str_u16(c, n->u.member.property->u.ident.u16, n->u.member.property->u.ident.len);
                emit_op(c, OP_DELETE_PROP);
                emit_u32(c, nidx);
                emit_u8(c, c->strict ? 1 : 0);
            }
            return true;
        }
        if (want_recv_and_fn) {
            emit_op(c, OP_DUP); /* [recv, recv] */
        }
        if (n->u.member.computed) {
            if (!compile_expr(c, n->u.member.property)) return false;
            emit_op(c, OP_GET_ELEM);
        } else if (n->u.member.property->kind == AST_PRIV_IDENT) {
            uint16_t d = 0;
            uint32_t s = 0;
            if (!resolve_binding(c, n->u.member.property->u.ident.u16, n->u.member.property->u.ident.len, true, &d, &s, NULL)) {
                comp_error(c, n->line, n->col, "Unresolved private identifier");
                return false;
            }
            emit_op(c, OP_GET_PRIVATE);
            emit_u16(c, d);
            emit_u32(c, s);
        } else {
            uint32_t nidx = add_str_u16(c, n->u.member.property->u.ident.u16, n->u.member.property->u.ident.len);
            emit_op(c, OP_GET_PROP);
            emit_u32(c, nidx);
        }
        return true;
    }

    if (!n->parenthesized && n->kind == AST_CALL_EXPR) {
        AjsAstNode *cal = n->u.call.callee;
        bool is_method = (!cal->parenthesized && cal->kind == AST_MEMBER_EXPR);
        if (is_method) {
            if (cal->u.member.object && cal->u.member.object->kind == AST_SUPER_EXPR) {
                /* super.m?.(...) */
                uint16_t sp_d = 0;
                uint32_t this_s = AJS_NO_SLOT, fn_s = AJS_NO_SLOT;
                resolve_special_fn_slots(c, &sp_d, &this_s, NULL, NULL, &fn_s);
                uint32_t fn_arg = (fn_s != AJS_NO_SLOT) ? fn_s : (sp_d == c->cur_env_depth ? AJS_NO_SLOT : fn_s);
                if (this_s != AJS_NO_SLOT) { emit_get_lex(c, sp_d, this_s); }
                else { emit_op(c, OP_THIS); }
                emit_op(c, OP_DUP);
                if (cal->u.member.computed) {
                    if (!compile_expr(c, cal->u.member.property)) return false;
                    emit_op(c, OP_GET_SUPER_ELEM); emit_u16(c, sp_d); emit_u32(c, fn_arg);
                } else {
                    uint32_t nidx = add_str_u16(c, cal->u.member.property->u.ident.u16, cal->u.member.property->u.ident.len);
                    emit_op(c, OP_GET_SUPER_PROP); emit_u16(c, sp_d); emit_u32(c, fn_arg); emit_u32(c, nidx);
                }
            } else {
                if (!compile_opt_chain_inner(c, cal, oc, true, false)) return false;
            }
            /* Stack: [recv, fn] */
            if (n->u.call.optional) {
                emit_op(c, OP_DUP);
                uint32_t j = emit_jump(c, OP_JMP_NULLISH);
                opt_chain_add_patch(oc, j, 2);
            }
            bool has_spread = false;
            if (!emit_call_args_list(c, &n->u.call.args, &has_spread)) return false;
            if (has_spread) {
                emit_op(c, OP_CALL_SPREAD);
            } else {
                emit_op(c, OP_CALL_METHOD);
                emit_u16(c, (uint16_t)n->u.call.args.count);
            }
        } else {
            if (!compile_opt_chain_inner(c, cal, oc, false, false)) return false;
            /* Stack: [fn] */
            if (n->u.call.optional) {
                emit_op(c, OP_DUP);
                uint32_t j = emit_jump(c, OP_JMP_NULLISH);
                opt_chain_add_patch(oc, j, 1);
            }
            bool has_spread = false;
            if (has_spread) { /* determined below */ }
            /* Check if spread is present before emitting */
            bool sp_check = false;
            for (uint32_t i = 0; i < n->u.call.args.count; i++) {
                if (n->u.call.args.items[i] && n->u.call.args.items[i]->kind == AST_SPREAD_ELEM) sp_check = true;
            }
            if (sp_check) {
                emit_op(c, OP_UNDEF);
                emit_op(c, OP_SWAP); /* [undef, fn] */
                if (!emit_call_args_list(c, &n->u.call.args, &has_spread)) return false;
                emit_op(c, OP_CALL_SPREAD);
            } else {
                if (!emit_call_args_list(c, &n->u.call.args, &has_spread)) return false;
                emit_op(c, OP_CALL);
                emit_u16(c, (uint16_t)n->u.call.args.count);
            }
        }
        if (is_delete) {
            emit_op(c, OP_POP);
            emit_op(c, OP_TRUE);
            return true;
        }
        if (want_recv_and_fn) {
            emit_op(c, OP_UNDEF);
            emit_op(c, OP_SWAP);
        }
        return true;
    }

    if (!compile_expr(c, n)) return false;
    if (is_delete) {
        emit_op(c, OP_POP);
        emit_op(c, OP_TRUE);
        return true;
    }
    if (want_recv_and_fn) {
        emit_op(c, OP_UNDEF);
        emit_op(c, OP_SWAP);
    }
    return true;
}

static bool compile_opt_chain(AjsCompiler *c, AjsAstNode *oc_node, bool is_delete) {
    AjsOptChainCtx oc;
    memset(&oc, 0, sizeof(oc));
    if (!compile_opt_chain_inner(c, oc_node->u.opt_chain.inner, &oc, false, is_delete)) {
        free(oc.patches);
        return false;
    }
    if (oc.count > 0) {
        uint32_t j_done = emit_jump(c, OP_JMP);
        uint32_t *end_jumps = (uint32_t *)malloc(oc.count * sizeof(uint32_t));
        for (uint32_t i = 0; i < oc.count; i++) {
            patch_jump(c, oc.patches[i].patch_off);
            for (uint16_t p = 0; p < oc.patches[i].extra_pops; p++) {
                emit_op(c, OP_POP);
            }
            emit_op(c, is_delete ? OP_TRUE : OP_UNDEF);
            end_jumps[i] = emit_jump(c, OP_JMP);
        }
        patch_jump(c, j_done);
        for (uint32_t i = 0; i < oc.count; i++) {
            patch_jump(c, end_jumps[i]);
        }
        free(end_jumps);
    }
    free(oc.patches);
    return true;
}

static AjsOpcode token_to_bin_op(AjsTokenKind op) {
    switch (op) {
        case TOK_PLUS: case TOK_ADD_ASSIGN:       return OP_ADD;
        case TOK_MINUS: case TOK_SUB_ASSIGN:      return OP_SUB;
        case TOK_MUL: case TOK_MUL_ASSIGN:        return OP_MUL;
        case TOK_DIV: case TOK_DIV_ASSIGN:        return OP_DIV;
        case TOK_MOD: case TOK_MOD_ASSIGN:        return OP_MOD;
        case TOK_EXP: case TOK_EXP_ASSIGN:        return OP_EXP;
        case TOK_BIT_AND: case TOK_AND_ASSIGN:    return OP_BIT_AND;
        case TOK_BIT_OR: case TOK_OR_ASSIGN:      return OP_BIT_OR;
        case TOK_BIT_XOR: case TOK_XOR_ASSIGN:    return OP_BIT_XOR;
        case TOK_SHL: case TOK_SHL_ASSIGN:        return OP_SHL;
        case TOK_SHR: case TOK_SHR_ASSIGN:        return OP_SHR;
        case TOK_USHR: case TOK_USHR_ASSIGN:      return OP_USHR;
        case TOK_EQ:                              return OP_EQ;
        case TOK_NEQ:                             return OP_NEQ;
        case TOK_STRICT_EQ:                       return OP_STRICT_EQ;
        case TOK_STRICT_NEQ:                      return OP_STRICT_NEQ;
        case TOK_LT:                              return OP_LT;
        case TOK_LTE:                             return OP_LTE;
        case TOK_GT:                              return OP_GT;
        case TOK_GTE:                             return OP_GTE;
        case KW_IN:                               return OP_IN;
        case KW_INSTANCEOF:                       return OP_INSTANCEOF;
        default:                                  return OP_NOP;
    }
}

static uint32_t build_tpl_site(AjsCompiler *c, AjsAstNode *quasi) {
    AjsBytecode *bc = c->bc;
    uint32_t tid = bc->tpl_count++;
    bc->tpls = (AjsTplSiteDesc *)realloc(bc->tpls, bc->tpl_count * sizeof(AjsTplSiteDesc));
    AjsTplSiteDesc *td = &bc->tpls[tid];
    td->id = tid;
    td->count = quasi->u.tpl.part_count;
    td->cooked = (AjsString **)calloc(td->count, sizeof(AjsString *));
    td->raw = (AjsString **)calloc(td->count, sizeof(AjsString *));
    for (uint32_t i = 0; i < td->count; i++) {
        if (quasi->u.tpl.cooked[i] != NULL) {
            td->cooked[i] = ajs_str_from_utf16(c->vm, quasi->u.tpl.cooked[i], quasi->u.tpl.cooked_len[i], false);
        } else {
            td->cooked[i] = NULL;
        }
        td->raw[i] = ajs_str_from_utf16(c->vm, quasi->u.tpl.raw[i], quasi->u.tpl.raw_len[i], false);
    }
    return tid;
}

static bool compile_update_expr(AjsCompiler *c, AjsAstNode *n) {
    AjsAstNode *arg = n->u.unary.arg;
    bool is_inc = (n->u.unary.op == TOK_INC);
    bool prefix = n->u.unary.prefix;
    AjsOpcode step_op = is_inc ? OP_INC_NUM : OP_DEC_NUM;

    if (arg->kind == AST_IDENT) {
        /* Read old value -> ToNumeric -> DUP (if postfix) -> INC/DEC -> store -> (if postfix, POP new so old remains!) */
        bool use_dyn = (c->with_depth > 0 || (c->has_eval_or_with && !c->strict));
        uint16_t depth = 0;
        uint32_t slot = 0;
        AjsBindKind bk;
        bool is_lex = !use_dyn && resolve_binding(c, arg->u.ident.u16, arg->u.ident.len, false, &depth, &slot, &bk);
        uint32_t nidx = is_lex ? 0 : add_str_u16(c, arg->u.ident.u16, arg->u.ident.len);

        if (use_dyn) {
            emit_op(c, OP_GET_DYN_NAME); emit_u32(c, nidx); emit_u8(c, 0);
        } else if (is_lex) {
            emit_get_lex(c, depth, slot);
        } else {
            if (c->strict) { emit_op(c, OP_CHECK_GLOBAL_REF); emit_u32(c, nidx); }
            emit_op(c, OP_GET_GLOBAL); emit_u32(c, nidx); emit_u8(c, 0);
        }
        emit_op(c, OP_TO_NUMERIC);
        if (!prefix) emit_op(c, OP_DUP);
        emit_op(c, step_op);
        if (use_dyn) {
            emit_op(c, OP_SET_DYN_NAME); emit_u32(c, nidx); emit_u8(c, c->strict ? 1 : 0);
        } else if (is_lex) {
            emit_set_lex(c, depth, slot, c->strict ? 1 : 0);
        } else {
            emit_op(c, OP_SET_GLOBAL); emit_u32(c, nidx); emit_u8(c, c->strict ? 1 : 0);
        }
        if (!prefix) emit_op(c, OP_POP);
        return true;
    }

    if (arg->kind == AST_MEMBER_EXPR) {
        AjsAstNode *obj = arg->u.member.object;
        AjsAstNode *prop = arg->u.member.property;
        if (obj && obj->kind == AST_SUPER_EXPR) {
            uint16_t sp_d = 0;
            uint32_t this_s = AJS_NO_SLOT, fn_s = AJS_NO_SLOT;
            resolve_special_fn_slots(c, &sp_d, &this_s, NULL, NULL, &fn_s);
            uint32_t fn_arg = (fn_s != AJS_NO_SLOT) ? fn_s : (sp_d == c->cur_env_depth ? AJS_NO_SLOT : fn_s);
            if (this_s != AJS_NO_SLOT) { emit_get_lex(c, sp_d, this_s); }
            else { emit_op(c, OP_THIS); }
            if (arg->u.member.computed) {
                if (!compile_expr(c, prop)) return false;
                emit_op(c, OP_TO_PROP_KEY);
                emit_op(c, OP_DUP2);
                emit_op(c, OP_GET_SUPER_ELEM); emit_u16(c, sp_d); emit_u32(c, fn_arg);
                emit_op(c, OP_TO_NUMERIC);
                if (!prefix) {
                    emit_push_env(c, 1, AJS_NO_SLOT);
                    emit_init_lex(c, 0, 0);
                    emit_op(c, step_op);
                    emit_op(c, OP_SET_SUPER_ELEM); emit_u16(c, (uint16_t)(sp_d + 1u));
                    emit_u32(c, fn_arg); emit_u8(c, c->strict ? 1 : 0);
                    emit_op(c, OP_POP);
                    emit_get_lex(c, 0, 0);
                    emit_op(c, OP_POP_ENV);
                    return true;
                }
                emit_op(c, step_op);
                emit_op(c, OP_SET_SUPER_ELEM); emit_u16(c, sp_d); emit_u32(c, fn_arg); emit_u8(c, c->strict ? 1 : 0);
            } else {
                uint32_t nidx = add_str_u16(c, prop->u.ident.u16, prop->u.ident.len);
                emit_op(c, OP_DUP);
                emit_op(c, OP_GET_SUPER_PROP); emit_u16(c, sp_d); emit_u32(c, fn_arg); emit_u32(c, nidx);
                emit_op(c, OP_TO_NUMERIC);
                if (!prefix) {
                    emit_op(c, OP_DUP);
                    emit_op(c, OP_ROT3L);
                    emit_op(c, OP_SWAP);
                }
                emit_op(c, step_op);
                emit_op(c, OP_SET_SUPER_PROP); emit_u16(c, sp_d); emit_u32(c, fn_arg); emit_u32(c, nidx); emit_u8(c, c->strict ? 1 : 0);
                if (!prefix) emit_op(c, OP_POP);
            }
            return true;
        }

        if (!compile_expr(c, obj)) return false;
        if (arg->u.member.computed) {
            if (!compile_expr(c, prop)) return false;
            emit_op(c, OP_TO_PROP_KEY);
            emit_op(c, OP_DUP2);
            emit_op(c, OP_GET_ELEM);
            emit_op(c, OP_TO_NUMERIC);
            if (!prefix) {
                emit_push_env(c, 1, AJS_NO_SLOT);
                emit_init_lex(c, 0, 0);
                emit_op(c, step_op);
                emit_op(c, OP_SET_ELEM); emit_u8(c, c->strict ? 1 : 0);
                emit_op(c, OP_POP);
                emit_get_lex(c, 0, 0);
                emit_op(c, OP_POP_ENV);
                return true;
            }
            emit_op(c, step_op);
            emit_op(c, OP_SET_ELEM); emit_u8(c, c->strict ? 1 : 0);
            return true;
        }

        if (prop->kind == AST_PRIV_IDENT) {
            uint16_t d = 0;
            uint32_t s = 0;
            if (!resolve_binding(c, prop->u.ident.u16, prop->u.ident.len, true, &d, &s, NULL)) {
                comp_error(c, prop->line, prop->col, "Unresolved private identifier");
                return false;
            }
            emit_op(c, OP_DUP);
            emit_op(c, OP_GET_PRIVATE); emit_u16(c, d); emit_u32(c, s);
            emit_op(c, OP_TO_NUMERIC);
            if (!prefix) {
                emit_op(c, OP_DUP);
                emit_op(c, OP_ROT3L);
                emit_op(c, OP_SWAP);
            }
            emit_op(c, step_op);
            emit_op(c, OP_SET_PRIVATE); emit_u16(c, d); emit_u32(c, s);
            if (!prefix) emit_op(c, OP_POP);
            return true;
        }

        uint32_t nidx = add_str_u16(c, prop->u.ident.u16, prop->u.ident.len);
        emit_op(c, OP_DUP);
        emit_op(c, OP_GET_PROP); emit_u32(c, nidx);
        emit_op(c, OP_TO_NUMERIC);
        if (!prefix) {
            emit_op(c, OP_DUP);
            emit_op(c, OP_ROT3L);
            emit_op(c, OP_SWAP);
        }
        emit_op(c, step_op);
        emit_op(c, OP_SET_PROP); emit_u32(c, nidx); emit_u8(c, c->strict ? 1 : 0);
        if (!prefix) emit_op(c, OP_POP);
        return true;
    }

    comp_error(c, n->line, n->col, "Invalid update target");
    return false;
}

static bool compile_assign_expr(AjsCompiler *c, AjsAstNode *n) {
    AjsTokenKind op = n->u.binary.op;
    AjsAstNode *lhs = n->u.binary.left;
    AjsAstNode *rhs = n->u.binary.right;

    /* Plain `=` assignment */
    if (op == TOK_ASSIGN) {
        if (lhs->kind == AST_ARRAY_PATTERN || lhs->kind == AST_OBJECT_PATTERN ||
            lhs->kind == AST_ARRAY_EXPR || lhs->kind == AST_OBJECT_EXPR) {
            if (!compile_expr(c, rhs)) return false;
            emit_op(c, OP_DUP);
            return compile_pattern_assign(c, lhs, false, AJS_VAR_LET);
        }

        if (lhs->kind == AST_IDENT) {
            bool use_dyn = (c->with_depth > 0 || (c->has_eval_or_with && !c->strict));
            uint16_t depth = 0;
            uint32_t slot = 0;
            AjsBindKind bk;
            bool is_lex = !use_dyn && resolve_binding(c, lhs->u.ident.u16, lhs->u.ident.len, false, &depth, &slot, &bk);
            uint32_t nidx = is_lex ? 0 : add_str_u16(c, lhs->u.ident.u16, lhs->u.ident.len);

            if (!compile_expr(c, rhs)) return false;
            if (is_anon_fn_or_class_def(rhs)) {
                uint32_t fn_nidx = add_str_u16(c, lhs->u.ident.u16, lhs->u.ident.len);
                emit_op(c, OP_CONST); emit_u32(c, fn_nidx);
                emit_op(c, OP_SET_FN_NAME); emit_u8(c, 0);
            }
            if (use_dyn) {
                emit_op(c, OP_SET_DYN_NAME); emit_u32(c, nidx); emit_u8(c, c->strict ? 1 : 0);
            } else if (is_lex) {
                emit_set_lex(c, depth, slot, c->strict ? 1 : 0);
            } else {
                emit_op(c, OP_SET_GLOBAL); emit_u32(c, nidx); emit_u8(c, c->strict ? 1 : 0);
            }
            return true;
        }

        if (lhs->kind == AST_MEMBER_EXPR) {
            AjsAstNode *obj = lhs->u.member.object;
            AjsAstNode *prop = lhs->u.member.property;
            if (obj && obj->kind == AST_SUPER_EXPR) {
                uint16_t sp_d = 0;
                uint32_t this_s = AJS_NO_SLOT, fn_s = AJS_NO_SLOT;
                resolve_special_fn_slots(c, &sp_d, &this_s, NULL, NULL, &fn_s);
                uint32_t fn_arg = (fn_s != AJS_NO_SLOT) ? fn_s : (sp_d == c->cur_env_depth ? AJS_NO_SLOT : fn_s);
                if (this_s != AJS_NO_SLOT) { emit_get_lex(c, sp_d, this_s); }
                else { emit_op(c, OP_THIS); }
                if (lhs->u.member.computed) {
                    if (!compile_expr(c, prop)) return false;
                    emit_op(c, OP_TO_PROP_KEY);
                    if (!compile_expr(c, rhs)) return false;
                    emit_op(c, OP_SET_SUPER_ELEM); emit_u16(c, sp_d); emit_u32(c, fn_arg); emit_u8(c, c->strict ? 1 : 0);
                } else {
                    if (!compile_expr(c, rhs)) return false;
                    uint32_t nidx = add_str_u16(c, prop->u.ident.u16, prop->u.ident.len);
                    emit_op(c, OP_SET_SUPER_PROP); emit_u16(c, sp_d); emit_u32(c, fn_arg); emit_u32(c, nidx); emit_u8(c, c->strict ? 1 : 0);
                }
                return true;
            }

            if (!compile_expr(c, obj)) return false;
            if (lhs->u.member.computed) {
                emit_op(c, OP_REQ_OBJ_COERCIBLE);
                if (!compile_expr(c, prop)) return false;
                emit_op(c, OP_TO_PROP_KEY);
                if (!compile_expr(c, rhs)) return false;
                emit_op(c, OP_SET_ELEM); emit_u8(c, c->strict ? 1 : 0);
            } else if (prop->kind == AST_PRIV_IDENT) {
                uint16_t d = 0;
                uint32_t s = 0;
                if (!resolve_binding(c, prop->u.ident.u16, prop->u.ident.len, true, &d, &s, NULL)) {
                    comp_error(c, prop->line, prop->col, "Unresolved private identifier");
                    return false;
                }
                if (!compile_expr(c, rhs)) return false;
                emit_op(c, OP_SET_PRIVATE); emit_u16(c, d); emit_u32(c, s);
            } else {
                emit_op(c, OP_REQ_OBJ_COERCIBLE);
                if (!compile_expr(c, rhs)) return false;
                uint32_t nidx = add_str_u16(c, prop->u.ident.u16, prop->u.ident.len);
                emit_op(c, OP_SET_PROP); emit_u32(c, nidx); emit_u8(c, c->strict ? 1 : 0);
            }
            return true;
        }
        comp_error(c, lhs->line, lhs->col, "Invalid assignment target");
        return false;
    }

    /* Logical assignment (`&&=`, `||=`, `??=`) & Compound assignment (`+=`, `-=`, etc.) */
    bool is_logical = (op == TOK_LOG_AND_ASSIGN || op == TOK_LOG_OR_ASSIGN || op == TOK_COALESCE_ASSIGN);
    AjsOpcode log_jmp = (op == TOK_LOG_AND_ASSIGN) ? OP_JMP_FALSE :
                        ((op == TOK_LOG_OR_ASSIGN) ? OP_JMP_TRUE : OP_JMP_NOT_NULLISH);

    if (lhs->kind == AST_IDENT) {
        emit_load_ident(c, lhs->u.ident.u16, lhs->u.ident.len, false);
        if (is_logical) {
            emit_op(c, OP_DUP);
            uint32_t j_skip = emit_jump(c, log_jmp);
            emit_op(c, OP_POP);
            if (!compile_expr(c, rhs)) return false;
            if (is_anon_fn_or_class_def(rhs)) {
                uint32_t fn_nidx = add_str_u16(c, lhs->u.ident.u16, lhs->u.ident.len);
                emit_op(c, OP_CONST); emit_u32(c, fn_nidx);
                emit_op(c, OP_SET_FN_NAME); emit_u8(c, 0);
            }
            emit_store_ident(c, lhs->u.ident.u16, lhs->u.ident.len, false, AJS_VAR_LET);
            patch_jump(c, j_skip);
            return true;
        }
        if (!compile_expr(c, rhs)) return false;
        emit_op(c, token_to_bin_op(op));
        emit_store_ident(c, lhs->u.ident.u16, lhs->u.ident.len, false, AJS_VAR_LET);
        return true;
    }

    if (lhs->kind == AST_MEMBER_EXPR) {
        AjsAstNode *obj = lhs->u.member.object;
        AjsAstNode *prop = lhs->u.member.property;

        if (obj && obj->kind == AST_SUPER_EXPR) {
            uint16_t sp_d = 0;
            uint32_t this_s = AJS_NO_SLOT, fn_s = AJS_NO_SLOT;
            resolve_special_fn_slots(c, &sp_d, &this_s, NULL, NULL, &fn_s);
            uint32_t fn_arg = (fn_s != AJS_NO_SLOT) ? fn_s : (sp_d == c->cur_env_depth ? AJS_NO_SLOT : fn_s);
            if (this_s != AJS_NO_SLOT) { emit_get_lex(c, sp_d, this_s); }
            else { emit_op(c, OP_THIS); }
            if (lhs->u.member.computed) {
                if (!compile_expr(c, prop)) return false;
                emit_op(c, OP_TO_PROP_KEY);
                emit_op(c, OP_DUP2);
                emit_op(c, OP_GET_SUPER_ELEM); emit_u16(c, sp_d); emit_u32(c, fn_arg);
                if (is_logical) {
                    emit_op(c, OP_DUP);
                    uint32_t j_skip = emit_jump(c, log_jmp);
                    emit_op(c, OP_POP);
                    if (!compile_expr(c, rhs)) return false;
                    emit_op(c, OP_SET_SUPER_ELEM); emit_u16(c, sp_d); emit_u32(c, fn_arg); emit_u8(c, c->strict ? 1 : 0);
                    uint32_t j_end = emit_jump(c, OP_JMP);
                    patch_jump(c, j_skip);
                    emit_op(c, OP_NIP); emit_op(c, OP_NIP);
                    patch_jump(c, j_end);
                    return true;
                }
                if (!compile_expr(c, rhs)) return false;
                emit_op(c, token_to_bin_op(op));
                emit_op(c, OP_SET_SUPER_ELEM); emit_u16(c, sp_d); emit_u32(c, fn_arg); emit_u8(c, c->strict ? 1 : 0);
                return true;
            } else {
                uint32_t nidx = add_str_u16(c, prop->u.ident.u16, prop->u.ident.len);
                emit_op(c, OP_DUP);
                emit_op(c, OP_GET_SUPER_PROP); emit_u16(c, sp_d); emit_u32(c, fn_arg); emit_u32(c, nidx);
                if (is_logical) {
                    emit_op(c, OP_DUP);
                    uint32_t j_skip = emit_jump(c, log_jmp);
                    emit_op(c, OP_POP);
                    if (!compile_expr(c, rhs)) return false;
                    emit_op(c, OP_SET_SUPER_PROP); emit_u16(c, sp_d); emit_u32(c, fn_arg); emit_u32(c, nidx); emit_u8(c, c->strict ? 1 : 0);
                    uint32_t j_end = emit_jump(c, OP_JMP);
                    patch_jump(c, j_skip);
                    emit_op(c, OP_NIP);
                    patch_jump(c, j_end);
                    return true;
                }
                if (!compile_expr(c, rhs)) return false;
                emit_op(c, token_to_bin_op(op));
                emit_op(c, OP_SET_SUPER_PROP); emit_u16(c, sp_d); emit_u32(c, fn_arg); emit_u32(c, nidx); emit_u8(c, c->strict ? 1 : 0);
                return true;
            }
        }

        if (!compile_expr(c, obj)) return false;
        if (lhs->u.member.computed) {
            emit_op(c, OP_REQ_OBJ_COERCIBLE);
            if (!compile_expr(c, prop)) return false;
            emit_op(c, OP_TO_PROP_KEY);
            emit_op(c, OP_DUP2);
            emit_op(c, OP_GET_ELEM);
            if (is_logical) {
                emit_op(c, OP_DUP);
                uint32_t j_skip = emit_jump(c, log_jmp);
                emit_op(c, OP_POP);
                if (!compile_expr(c, rhs)) return false;
                emit_op(c, OP_SET_ELEM); emit_u8(c, c->strict ? 1 : 0);
                uint32_t j_end = emit_jump(c, OP_JMP);
                patch_jump(c, j_skip);
                emit_op(c, OP_NIP); emit_op(c, OP_NIP);
                patch_jump(c, j_end);
                return true;
            }
            if (!compile_expr(c, rhs)) return false;
            emit_op(c, token_to_bin_op(op));
            emit_op(c, OP_SET_ELEM); emit_u8(c, c->strict ? 1 : 0);
            return true;
        }

        if (prop->kind == AST_PRIV_IDENT) {
            uint16_t d = 0;
            uint32_t s = 0;
            if (!resolve_binding(c, prop->u.ident.u16, prop->u.ident.len, true, &d, &s, NULL)) {
                comp_error(c, prop->line, prop->col, "Unresolved private identifier");
                return false;
            }
            emit_op(c, OP_DUP);
            emit_op(c, OP_GET_PRIVATE); emit_u16(c, d); emit_u32(c, s);
            if (is_logical) {
                emit_op(c, OP_DUP);
                uint32_t j_skip = emit_jump(c, log_jmp);
                emit_op(c, OP_POP);
                if (!compile_expr(c, rhs)) return false;
                emit_op(c, OP_SET_PRIVATE); emit_u16(c, d); emit_u32(c, s);
                uint32_t j_end = emit_jump(c, OP_JMP);
                patch_jump(c, j_skip);
                emit_op(c, OP_NIP);
                patch_jump(c, j_end);
                return true;
            }
            if (!compile_expr(c, rhs)) return false;
            emit_op(c, token_to_bin_op(op));
            emit_op(c, OP_SET_PRIVATE); emit_u16(c, d); emit_u32(c, s);
            return true;
        }

        emit_op(c, OP_REQ_OBJ_COERCIBLE);
        uint32_t nidx = add_str_u16(c, prop->u.ident.u16, prop->u.ident.len);
        emit_op(c, OP_DUP);
        emit_op(c, OP_GET_PROP); emit_u32(c, nidx);
        if (is_logical) {
            emit_op(c, OP_DUP);
            uint32_t j_skip = emit_jump(c, log_jmp);
            emit_op(c, OP_POP);
            if (!compile_expr(c, rhs)) return false;
            emit_op(c, OP_SET_PROP); emit_u32(c, nidx); emit_u8(c, c->strict ? 1 : 0);
            uint32_t j_end = emit_jump(c, OP_JMP);
            patch_jump(c, j_skip);
            emit_op(c, OP_NIP);
            patch_jump(c, j_end);
            return true;
        }
        if (!compile_expr(c, rhs)) return false;
        emit_op(c, token_to_bin_op(op));
        emit_op(c, OP_SET_PROP); emit_u32(c, nidx); emit_u8(c, c->strict ? 1 : 0);
        return true;
    }

    comp_error(c, lhs->line, lhs->col, "Invalid assignment target");
    return false;
}

static bool compile_expr(AjsCompiler *c, AjsAstNode *n) {
    if (!n || c->had_error) return false;
    emit_line_pos(c, n->line, n->col);

    switch (n->kind) {
        case AST_LITERAL_UNDEF:
            emit_op(c, OP_UNDEF);
            return true;
        case AST_LITERAL_NULL:
            emit_op(c, OP_NULL);
            return true;
        case AST_LITERAL_BOOL:
            emit_op(c, n->u.lit.bval ? OP_TRUE : OP_FALSE);
            return true;
        case AST_LITERAL_NUM: {
            double d = n->u.lit.nval;
            /* Check for exact +0 (not -0!), 1, or small int32 */
            if (ajs_double_to_bits(d) == 0) {
                emit_op(c, OP_ZERO);
            } else if (d == 1.0) {
                emit_op(c, OP_ONE);
            } else if (d >= -2147483648.0 && d <= 2147483647.0 && (double)(int32_t)d == d) {
                emit_op(c, OP_INT32);
                emit_i32(c, (int32_t)d);
            } else {
                emit_op(c, OP_CONST);
                emit_u32(c, add_const(c, ajs_val_number(d)));
            }
            return true;
        }
        case AST_LITERAL_BIGINT: {
            bool ok = false;
            AjsBigInt *bi = ajs_bigint_from_cstr(c->vm, n->u.lit.bi_utf8, n->u.lit.bi_len, true, &ok);
            if (!ok || !bi) {
                comp_error(c, n->line, n->col, "Invalid BigInt literal");
                return false;
            }
            emit_op(c, OP_CONST);
            emit_u32(c, add_const(c, ajs_val_from_gc(bi)));
            return true;
        }
        case AST_LITERAL_STR:
            emit_op(c, OP_CONST);
            emit_u32(c, add_str_u16(c, n->u.lit.s_u16, n->u.lit.s_len));
            return true;
        case AST_LITERAL_REGEXP: {
            uint32_t pidx = add_str_u16(c, n->u.lit.rx_pat, n->u.lit.rx_pat_len);
            uint32_t fidx = add_str_u16(c, n->u.lit.rx_flags, n->u.lit.rx_flags_len);
            emit_op(c, OP_REGEXP);
            emit_u32(c, pidx);
            emit_u32(c, fidx);
            return true;
        }
        case AST_IDENT:
            emit_load_ident(c, n->u.ident.u16, n->u.ident.len, false);
            return true;
        case AST_THIS_EXPR: {
            uint16_t sp_d = 0;
            uint32_t this_s = AJS_NO_SLOT;
            if (resolve_special_fn_slots(c, &sp_d, &this_s, NULL, NULL, NULL) && this_s != AJS_NO_SLOT) {
                emit_get_lex(c, sp_d, this_s);
            } else {
                emit_op(c, OP_THIS);
            }
            return true;
        }
        case AST_NEW_TARGET: {
            uint16_t sp_d = 0;
            uint32_t nt_s = AJS_NO_SLOT;
            if (resolve_special_fn_slots(c, &sp_d, NULL, &nt_s, NULL, NULL) && nt_s != AJS_NO_SLOT) {
                emit_get_lex(c, sp_d, nt_s);
            } else {
                emit_op(c, OP_NEW_TARGET);
            }
            return true;
        }
        case AST_IMPORT_META:
            emit_op(c, OP_IMPORT_META);
            return true;

        case AST_ARRAY_EXPR: {
            emit_op(c, OP_NEW_ARRAY);
            emit_u32(c, n->u.array.elems.count);
            for (uint32_t i = 0; i < n->u.array.elems.count; i++) {
                AjsAstNode *el = n->u.array.elems.items[i];
                if (!el) {
                    emit_op(c, OP_EMPTY);
                    emit_op(c, OP_ARRAY_PUSH);
                } else if (el->kind == AST_SPREAD_ELEM) {
                    if (!compile_expr(c, el->u.expr_stmt.expr)) return false;
                    emit_op(c, OP_ARRAY_SPREAD);
                } else {
                    if (!compile_expr(c, el)) return false;
                    emit_op(c, OP_ARRAY_PUSH);
                }
            }
            return true;
        }

        case AST_OBJECT_EXPR: {
            emit_op(c, OP_NEW_OBJECT);
            for (uint32_t i = 0; i < n->u.object.props.count; i++) {
                AjsAstNode *p = n->u.object.props.items[i];
                if (!p) continue;
                if (p->kind == AST_SPREAD_ELEM) {
                    if (!compile_expr(c, p->u.expr_stmt.expr)) return false;
                    emit_op(c, OP_OBJECT_SPREAD);
                    continue;
                }
                /* Check Annex B `__proto__: val` (non-computed, non-shorthand, kind == INIT) */
                bool is_proto_setter = false;
                if (!p->u.prop.computed && !p->u.prop.shorthand && p->u.prop.kind == AJS_PROP_INIT) {
                    if (p->u.prop.key->kind == AST_IDENT &&
                        c_u16_eq_ascii(p->u.prop.key->u.ident.u16, p->u.prop.key->u.ident.len, "__proto__")) {
                        is_proto_setter = true;
                    } else if (p->u.prop.key->kind == AST_LITERAL_STR &&
                               c_u16_eq_ascii(p->u.prop.key->u.lit.s_u16, p->u.prop.key->u.lit.s_len, "__proto__")) {
                        is_proto_setter = true;
                    }
                }
                if (is_proto_setter) {
                    if (!compile_expr(c, p->u.prop.value)) return false;
                    emit_op(c, OP_SET_PROTO_LIT);
                    continue;
                }

                uint8_t def_kind = 0; /* 0=data(enum), 1=get(enum), 2=set(enum) */
                uint8_t fn_prefix = 0;
                if (p->u.prop.kind == AJS_PROP_GET) { def_kind = 1; fn_prefix = 1; }
                else if (p->u.prop.kind == AJS_PROP_SET) { def_kind = 2; fn_prefix = 2; }

                bool need_home_obj = (p->u.prop.kind == AJS_PROP_METHOD ||
                                      p->u.prop.kind == AJS_PROP_GET ||
                                      p->u.prop.kind == AJS_PROP_SET);
                bool need_fn_name = need_home_obj || is_anon_fn_or_class_def(p->u.prop.value);

                if (!p->u.prop.computed && p->u.prop.key->kind == AST_IDENT) {
                    uint32_t kidx = add_str_u16(c, p->u.prop.key->u.ident.u16, p->u.prop.key->u.ident.len);
                    if (!compile_expr(c, p->u.prop.value)) return false;
                    if (need_fn_name) {
                        emit_op(c, OP_CONST); emit_u32(c, kidx);
                        emit_op(c, OP_SET_FN_NAME); emit_u8(c, fn_prefix);
                    }
                    if (need_home_obj) {
                        emit_op(c, OP_SET_HOME_OBJ);
                    }
                    emit_op(c, OP_DEFINE_PROP);
                    emit_u32(c, kidx);
                    emit_u8(c, def_kind);
                } else {
                    /* Stack: [obj] -> eval key -> TO_PROP_KEY -> [obj, key] */
                    if (p->u.prop.computed) {
                        if (!compile_expr(c, p->u.prop.key)) return false;
                    } else if (p->u.prop.key->kind == AST_LITERAL_STR) {
                        emit_op(c, OP_CONST);
                        emit_u32(c, add_str_u16(c, p->u.prop.key->u.lit.s_u16, p->u.prop.key->u.lit.s_len));
                    } else if (p->u.prop.key->kind == AST_LITERAL_NUM) {
                        emit_op(c, OP_CONST);
                        emit_u32(c, add_const(c, ajs_val_number(p->u.prop.key->u.lit.nval)));
                    } else {
                        if (!compile_expr(c, p->u.prop.key)) return false;
                    }
                    emit_op(c, OP_TO_PROP_KEY);
                    if (!compile_expr(c, p->u.prop.value)) return false;
                    /* Stack: [obj, key, val] */
                    if (need_fn_name) {
                        /* Copy key onto top: [obj, key, val] -> SWAP -> [obj, val, key] -> DUP2 -> [obj, val, key, val, key]
                         * Wait: simpler! SWAP -> [obj, val, key] -> DUP -> [obj, val, key, key] -> ROT3 -> [obj, key, val, key] -> SET_FN_NAME -> [obj, key, val]! */
                        emit_op(c, OP_SWAP);
                        emit_op(c, OP_DUP);
                        emit_op(c, OP_ROT3);
                        emit_op(c, OP_SET_FN_NAME);
                        emit_u8(c, fn_prefix);
                    }
                    if (need_home_obj) {
                        /* Stack: [obj, key, fn]. Need SET_HOME_OBJ on [obj, fn]:
                         * ROT3L -> [key, fn, obj] -> SWAP -> [key, obj, fn] -> SET_HOME_OBJ -> [key, obj, fn] -> ROT3 -> [fn, key, obj] -> SWAP -> [fn, obj, key] -> ROT3 -> [key, fn, obj]...
                         * Let's trace cleanly:
                         * [obj, key, fn] -> ROT3 -> [fn, obj, key] -> ROT3 -> [key, fn, obj] -> SWAP -> [key, obj, fn]
                         * -> OP_SET_HOME_OBJ -> [key, obj, fn] -> ROT3L -> [obj, fn, key] -> SWAP -> [obj, key, fn]!!
                         * Verify!
                         * Start: [obj, key, fn]
                         * ROT3 ([a,b,c]->[c,a,b]): [fn, obj, key]
                         * ROT3: [key, fn, obj]
                         * SWAP: [key, obj, fn]
                         * SET_HOME_OBJ ([home, fn]->[home, fn]): [key, obj, fn]
                         * ROT3L ([a,b,c]->[b,c,a]): [obj, fn, key]
                         * SWAP: [obj, key, fn]!! Exact! */
                        emit_op(c, OP_ROT3);
                        emit_op(c, OP_ROT3);
                        emit_op(c, OP_SWAP);
                        emit_op(c, OP_SET_HOME_OBJ);
                        emit_op(c, OP_ROT3L);
                        emit_op(c, OP_SWAP);
                    }
                    emit_op(c, OP_DEFINE_ELEM);
                    emit_u8(c, def_kind);
                }
            }
            return true;
        }

        case AST_FUNC_EXPR: {
            AjsBytecode *fn_bc = compile_func_node(c, n, NULL, false);
            if (!fn_bc || c->had_error) return false;
            uint32_t bc_idx = add_const(c, ajs_val_from_gc(fn_bc));
            emit_op(c, OP_CLOSURE);
            emit_u32(c, bc_idx);
            return true;
        }

        case AST_CLASS_EXPR:
            return compile_class_node(c, n, NULL, true);

        case AST_TEMPLATE_LIT: {
            emit_op(c, OP_CONST);
            emit_u32(c, add_str_u16(c, n->u.tpl.cooked[0], n->u.tpl.cooked_len[0]));
            for (uint32_t i = 0; i < n->u.tpl.exprs.count; i++) {
                if (!compile_expr(c, n->u.tpl.exprs.items[i])) return false;
                /* ECMA-262 §13.2.8.4: ToString(sub) uses string hint and throws TypeError on Symbol.
                 * OP_TO_PROP_KEY invokes ToPrimitive(sub, STRING) and OP_ADD with left AjsString
                 * concatenates strings while throwing TypeError if key is a Symbol. */
                emit_op(c, OP_TO_PROP_KEY);
                emit_op(c, OP_ADD);
                if (n->u.tpl.cooked_len[i + 1] > 0) {
                    emit_op(c, OP_CONST);
                    emit_u32(c, add_str_u16(c, n->u.tpl.cooked[i + 1], n->u.tpl.cooked_len[i + 1]));
                    emit_op(c, OP_ADD);
                }
            }
            return true;
        }

        case AST_TAGGED_TEMPLATE: {
            AjsAstNode *tag = n->u.tagged_tpl.tag;
            AjsAstNode *quasi = n->u.tagged_tpl.quasi;
            uint32_t tid = build_tpl_site(c, quasi);
            bool is_method = (!tag->parenthesized && tag->kind == AST_MEMBER_EXPR);
            if (is_method) {
                if (tag->u.member.object && tag->u.member.object->kind == AST_SUPER_EXPR) {
                    uint16_t sp_d = 0;
                    uint32_t this_s = AJS_NO_SLOT, fn_s = AJS_NO_SLOT;
                    resolve_special_fn_slots(c, &sp_d, &this_s, NULL, NULL, &fn_s);
                    uint32_t fn_arg = (fn_s != AJS_NO_SLOT) ? fn_s : (sp_d == c->cur_env_depth ? AJS_NO_SLOT : fn_s);
                    if (this_s != AJS_NO_SLOT) { emit_get_lex(c, sp_d, this_s); }
                    else { emit_op(c, OP_THIS); }
                    emit_op(c, OP_DUP);
                    if (tag->u.member.computed) {
                        if (!compile_expr(c, tag->u.member.property)) return false;
                        emit_op(c, OP_GET_SUPER_ELEM); emit_u16(c, sp_d); emit_u32(c, fn_arg);
                    } else {
                        uint32_t nidx = add_str_u16(c, tag->u.member.property->u.ident.u16, tag->u.member.property->u.ident.len);
                        emit_op(c, OP_GET_SUPER_PROP); emit_u16(c, sp_d); emit_u32(c, fn_arg); emit_u32(c, nidx);
                    }
                } else {
                    if (!compile_expr(c, tag->u.member.object)) return false;
                    emit_op(c, OP_DUP);
                    if (tag->u.member.computed) {
                        if (!compile_expr(c, tag->u.member.property)) return false;
                        emit_op(c, OP_GET_ELEM);
                    } else if (tag->u.member.property->kind == AST_PRIV_IDENT) {
                        uint16_t d = 0;
                        uint32_t s = 0;
                        resolve_binding(c, tag->u.member.property->u.ident.u16, tag->u.member.property->u.ident.len, true, &d, &s, NULL);
                        emit_op(c, OP_GET_PRIVATE); emit_u16(c, d); emit_u32(c, s);
                    } else {
                        uint32_t nidx = add_str_u16(c, tag->u.member.property->u.ident.u16, tag->u.member.property->u.ident.len);
                        emit_op(c, OP_GET_PROP); emit_u32(c, nidx);
                    }
                }
                emit_op(c, OP_TEMPLATE_OBJ);
                emit_u32(c, tid);
                for (uint32_t i = 0; i < quasi->u.tpl.exprs.count; i++) {
                    if (!compile_expr(c, quasi->u.tpl.exprs.items[i])) return false;
                }
                emit_op(c, OP_CALL_METHOD);
                emit_u16(c, (uint16_t)(1u + quasi->u.tpl.exprs.count));
            } else {
                if (!compile_expr(c, tag)) return false;
                emit_op(c, OP_TEMPLATE_OBJ);
                emit_u32(c, tid);
                for (uint32_t i = 0; i < quasi->u.tpl.exprs.count; i++) {
                    if (!compile_expr(c, quasi->u.tpl.exprs.items[i])) return false;
                }
                emit_op(c, OP_CALL);
                emit_u16(c, (uint16_t)(1u + quasi->u.tpl.exprs.count));
            }
            return true;
        }

        case AST_OPT_CHAIN_EXPR:
            return compile_opt_chain(c, n, false);

        case AST_MEMBER_EXPR: {
            AjsAstNode *obj = n->u.member.object;
            AjsAstNode *prop = n->u.member.property;
            if (obj && obj->kind == AST_SUPER_EXPR) {
                uint16_t sp_d = 0;
                uint32_t this_s = AJS_NO_SLOT, fn_s = AJS_NO_SLOT;
                resolve_special_fn_slots(c, &sp_d, &this_s, NULL, NULL, &fn_s);
                uint32_t fn_arg = (fn_s != AJS_NO_SLOT) ? fn_s : (sp_d == c->cur_env_depth ? AJS_NO_SLOT : fn_s);
                if (this_s != AJS_NO_SLOT) { emit_get_lex(c, sp_d, this_s); }
                else { emit_op(c, OP_THIS); }
                if (n->u.member.computed) {
                    if (!compile_expr(c, prop)) return false;
                    emit_op(c, OP_GET_SUPER_ELEM); emit_u16(c, sp_d); emit_u32(c, fn_arg);
                } else {
                    uint32_t nidx = add_str_u16(c, prop->u.ident.u16, prop->u.ident.len);
                    emit_op(c, OP_GET_SUPER_PROP); emit_u16(c, sp_d); emit_u32(c, fn_arg); emit_u32(c, nidx);
                }
                return true;
            }
            if (!compile_expr(c, obj)) return false;
            if (n->u.member.computed) {
                if (!compile_expr(c, prop)) return false;
                emit_op(c, OP_GET_ELEM);
            } else if (prop->kind == AST_PRIV_IDENT) {
                uint16_t d = 0;
                uint32_t s = 0;
                if (!resolve_binding(c, prop->u.ident.u16, prop->u.ident.len, true, &d, &s, NULL)) {
                    comp_error(c, prop->line, prop->col, "Unresolved private identifier");
                    return false;
                }
                emit_op(c, OP_GET_PRIVATE); emit_u16(c, d); emit_u32(c, s);
            } else {
                uint32_t nidx = add_str_u16(c, prop->u.ident.u16, prop->u.ident.len);
                emit_op(c, OP_GET_PROP); emit_u32(c, nidx);
            }
            return true;
        }

        case AST_CALL_EXPR: {
            AjsAstNode *cal = n->u.call.callee;
            /* Super constructor call: `super(...args)` */
            if (cal && cal->kind == AST_SUPER_EXPR) {
                uint16_t sp_d = 0;
                uint32_t this_s = AJS_NO_SLOT, nt_s = AJS_NO_SLOT, fn_s = AJS_NO_SLOT;
                resolve_special_fn_slots(c, &sp_d, &this_s, &nt_s, NULL, &fn_s);
                emit_op(c, OP_NEW_ARRAY);
                emit_u32(c, n->u.call.args.count);
                for (uint32_t i = 0; i < n->u.call.args.count; i++) {
                    AjsAstNode *a = n->u.call.args.items[i];
                    if (a->kind == AST_SPREAD_ELEM) {
                        if (!compile_expr(c, a->u.expr_stmt.expr)) return false;
                        emit_op(c, OP_ARRAY_SPREAD);
                    } else {
                        if (!compile_expr(c, a)) return false;
                        emit_op(c, OP_ARRAY_PUSH);
                    }
                }
                emit_op(c, OP_SUPER_CALL);
                emit_u16(c, sp_d);
                emit_u32(c, this_s);
                emit_u32(c, nt_s);
                emit_u32(c, fn_s);
                return true;
            }

            /* Direct `eval(...)` check: unparenthesized `eval(...)` */
            if (!cal->parenthesized && cal->kind == AST_IDENT &&
                c_u16_eq_ascii(cal->u.ident.u16, cal->u.ident.len, "eval")) {
                bool has_spread = false;
                for (uint32_t i = 0; i < n->u.call.args.count; i++) {
                    if (n->u.call.args.items[i]->kind == AST_SPREAD_ELEM) has_spread = true;
                }
                if (!has_spread) {
                    emit_load_ident(c, cal->u.ident.u16, cal->u.ident.len, false);
                    for (uint32_t i = 0; i < n->u.call.args.count; i++) {
                        if (!compile_expr(c, n->u.call.args.items[i])) return false;
                    }
                    emit_op(c, OP_CALL_EVAL);
                    emit_u16(c, (uint16_t)n->u.call.args.count);
                    emit_u8(c, c->strict ? 1 : 0);
                    return true;
                }
            }

            /* Method call `obj.m(...)` or `super.m(...)` */
            if (!cal->parenthesized && cal->kind == AST_MEMBER_EXPR) {
                AjsAstNode *obj = cal->u.member.object;
                AjsAstNode *prop = cal->u.member.property;
                if (obj && obj->kind == AST_SUPER_EXPR) {
                    uint16_t sp_d = 0;
                    uint32_t this_s = AJS_NO_SLOT, fn_s = AJS_NO_SLOT;
                    resolve_special_fn_slots(c, &sp_d, &this_s, NULL, NULL, &fn_s);
                    uint32_t fn_arg = (fn_s != AJS_NO_SLOT) ? fn_s : (sp_d == c->cur_env_depth ? AJS_NO_SLOT : fn_s);
                    if (this_s != AJS_NO_SLOT) { emit_get_lex(c, sp_d, this_s); }
                    else { emit_op(c, OP_THIS); }
                    emit_op(c, OP_DUP);
                    if (cal->u.member.computed) {
                        if (!compile_expr(c, prop)) return false;
                        emit_op(c, OP_GET_SUPER_ELEM); emit_u16(c, sp_d); emit_u32(c, fn_arg);
                    } else {
                        uint32_t nidx = add_str_u16(c, prop->u.ident.u16, prop->u.ident.len);
                        emit_op(c, OP_GET_SUPER_PROP); emit_u16(c, sp_d); emit_u32(c, fn_arg); emit_u32(c, nidx);
                    }
                } else {
                    if (!compile_expr(c, obj)) return false;
                    emit_op(c, OP_DUP);
                    if (cal->u.member.computed) {
                        if (!compile_expr(c, prop)) return false;
                        emit_op(c, OP_GET_ELEM);
                    } else if (prop->kind == AST_PRIV_IDENT) {
                        uint16_t d = 0;
                        uint32_t s = 0;
                        if (!resolve_binding(c, prop->u.ident.u16, prop->u.ident.len, true, &d, &s, NULL)) {
                            comp_error(c, prop->line, prop->col, "Unresolved private identifier");
                            return false;
                        }
                        emit_op(c, OP_GET_PRIVATE); emit_u16(c, d); emit_u32(c, s);
                    } else {
                        uint32_t nidx = add_str_u16(c, prop->u.ident.u16, prop->u.ident.len);
                        emit_op(c, OP_GET_PROP); emit_u32(c, nidx);
                    }
                }
                bool has_spread = false;
                if (!emit_call_args_list(c, &n->u.call.args, &has_spread)) return false;
                if (has_spread) {
                    emit_op(c, OP_CALL_SPREAD);
                } else {
                    emit_op(c, OP_CALL_METHOD);
                    emit_u16(c, (uint16_t)n->u.call.args.count);
                }
                return true;
            }

            /* Sloppy `with` identifier call passes `with_obj` as `this` if resolved from `with_obj`!
             * For ordinary calls: */
            bool has_spread = false;
            for (uint32_t i = 0; i < n->u.call.args.count; i++) {
                if (n->u.call.args.items[i]->kind == AST_SPREAD_ELEM) has_spread = true;
            }
            if (has_spread) {
                emit_op(c, OP_UNDEF);
                if (!compile_expr(c, cal)) return false;
                if (!emit_call_args_list(c, &n->u.call.args, &has_spread)) return false;
                emit_op(c, OP_CALL_SPREAD);
            } else {
                if (!compile_expr(c, cal)) return false;
                if (!emit_call_args_list(c, &n->u.call.args, &has_spread)) return false;
                emit_op(c, OP_CALL);
                emit_u16(c, (uint16_t)n->u.call.args.count);
            }
            return true;
        }

        case AST_NEW_EXPR: {
            if (!compile_expr(c, n->u.call.callee)) return false;
            bool has_spread = false;
            if (!emit_call_args_list(c, &n->u.call.args, &has_spread)) return false;
            if (has_spread) {
                emit_op(c, OP_NEW_SPREAD);
            } else {
                emit_op(c, OP_NEW);
                emit_u16(c, (uint16_t)n->u.call.args.count);
            }
            return true;
        }

        case AST_DYNAMIC_IMPORT:
            if (n->u.expr_stmt.expr) {
                if (!compile_expr(c, n->u.expr_stmt.expr)) return false;
            } else {
                emit_op(c, OP_UNDEF);
            }
            emit_op(c, OP_DYNAMIC_IMPORT);
            return true;

        case AST_UNARY_EXPR: {
            AjsAstNode *arg = n->u.unary.arg;
            switch (n->u.unary.op) {
                case KW_TYPEOF:
                    if (!arg->parenthesized && arg->kind == AST_IDENT) {
                        emit_load_ident(c, arg->u.ident.u16, arg->u.ident.len, true);
                    } else {
                        if (!compile_expr(c, arg)) return false;
                    }
                    emit_op(c, OP_TYPEOF);
                    return true;
                case KW_DELETE:
                    if (!arg->parenthesized && arg->kind == AST_OPT_CHAIN_EXPR) {
                        return compile_opt_chain(c, arg, true);
                    }
                    if (!arg->parenthesized && arg->kind == AST_MEMBER_EXPR) {
                        if (arg->u.member.object && arg->u.member.object->kind == AST_SUPER_EXPR) {
                            /* ECMA-262 §13.5.1.2: `delete super.prop` ALWAYS throws ReferenceError (after evaluating key if computed)! */
                            uint16_t sp_d = 0;
                            uint32_t this_s = AJS_NO_SLOT;
                            resolve_special_fn_slots(c, &sp_d, &this_s, NULL, NULL, NULL);
                            if (this_s != AJS_NO_SLOT) { emit_get_lex(c, sp_d, this_s); emit_op(c, OP_POP); }
                            if (arg->u.member.computed) {
                                if (!compile_expr(c, arg->u.member.property)) return false;
                                emit_op(c, OP_TO_PROP_KEY);
                                emit_op(c, OP_POP);
                            }
                            /* Emit ReferenceError throw by reading TDZ! */
                            emit_op(c, OP_TDZ);
                            emit_op(c, OP_CHECK_THIS_TDZ);
                            return true;
                        }
                        if (!compile_expr(c, arg->u.member.object)) return false;
                        if (arg->u.member.computed) {
                            if (!compile_expr(c, arg->u.member.property)) return false;
                            emit_op(c, OP_DELETE_ELEM);
                            emit_u8(c, c->strict ? 1 : 0);
                        } else {
                            uint32_t nidx = add_str_u16(c, arg->u.member.property->u.ident.u16, arg->u.member.property->u.ident.len);
                            emit_op(c, OP_DELETE_PROP);
                            emit_u32(c, nidx);
                            emit_u8(c, c->strict ? 1 : 0);
                        }
                        return true;
                    }
                    if (!arg->parenthesized && arg->kind == AST_IDENT) {
                        uint16_t d = 0;
                        uint32_t s = 0;
                        if (c->with_depth == 0 && !c->has_eval_or_with &&
                            resolve_binding(c, arg->u.ident.u16, arg->u.ident.len, false, &d, &s, NULL)) {
                            emit_op(c, OP_FALSE);
                            return true;
                        }
                        uint32_t nidx = add_str_u16(c, arg->u.ident.u16, arg->u.ident.len);
                        emit_op(c, OP_DELETE_NAME);
                        emit_u32(c, nidx);
                        emit_u8(c, c->strict ? 1 : 0);
                        return true;
                    }
                    if (!compile_expr(c, arg)) return false;
                    emit_op(c, OP_POP);
                    emit_op(c, OP_TRUE);
                    return true;
                case KW_VOID:
                    if (!compile_expr(c, arg)) return false;
                    emit_op(c, OP_VOID);
                    return true;
                case TOK_PLUS:
                    if (!compile_expr(c, arg)) return false;
                    emit_op(c, OP_POS);
                    return true;
                case TOK_MINUS:
                    if (!compile_expr(c, arg)) return false;
                    emit_op(c, OP_NEG);
                    return true;
                case TOK_BIT_NOT:
                    if (!compile_expr(c, arg)) return false;
                    emit_op(c, OP_BIT_NOT);
                    return true;
                case TOK_BANG:
                    if (!compile_expr(c, arg)) return false;
                    emit_op(c, OP_NOT);
                    return true;
                default:
                    break;
            }
            break;
        }

        case AST_UPDATE_EXPR:
            return compile_update_expr(c, n);

        case AST_BINARY_EXPR: {
            if (n->u.binary.op == KW_IN && n->u.binary.left && n->u.binary.left->kind == AST_PRIV_IDENT) {
                /* Ergonomic brand check: `#priv in expr` */
                if (!compile_expr(c, n->u.binary.right)) return false;
                uint16_t d = 0;
                uint32_t s = 0;
                if (!resolve_binding(c, n->u.binary.left->u.ident.u16, n->u.binary.left->u.ident.len, true, &d, &s, NULL)) {
                    comp_error(c, n->line, n->col, "Unresolved private identifier");
                    return false;
                }
                emit_op(c, OP_IN_PRIVATE);
                emit_u16(c, d);
                emit_u32(c, s);
                return true;
            }
            if (!compile_expr(c, n->u.binary.left)) return false;
            if (!compile_expr(c, n->u.binary.right)) return false;
            emit_op(c, token_to_bin_op(n->u.binary.op));
            return true;
        }

        case AST_LOGICAL_EXPR: {
            if (!compile_expr(c, n->u.binary.left)) return false;
            emit_op(c, OP_DUP);
            AjsOpcode jop = (n->u.binary.op == TOK_AND) ? OP_JMP_FALSE :
                            ((n->u.binary.op == TOK_OR) ? OP_JMP_TRUE : OP_JMP_NOT_NULLISH);
            uint32_t j_short = emit_jump(c, jop);
            emit_op(c, OP_POP);
            if (!compile_expr(c, n->u.binary.right)) return false;
            patch_jump(c, j_short);
            return true;
        }

        case AST_ASSIGN_EXPR:
            return compile_assign_expr(c, n);

        case AST_COND_EXPR: {
            if (!compile_expr(c, n->u.cond.test)) return false;
            uint32_t j_false = emit_jump(c, OP_JMP_FALSE);
            if (!compile_expr(c, n->u.cond.conseq)) return false;
            uint32_t j_end = emit_jump(c, OP_JMP);
            patch_jump(c, j_false);
            if (!compile_expr(c, n->u.cond.alt)) return false;
            patch_jump(c, j_end);
            return true;
        }

        case AST_SEQ_EXPR:
            for (uint32_t i = 0; i < n->u.seq.exprs.count; i++) {
                if (!compile_expr(c, n->u.seq.exprs.items[i])) return false;
                if (i + 1 < n->u.seq.exprs.count) emit_op(c, OP_POP);
            }
            return true;

        case AST_YIELD_EXPR:
            if (n->u.yield_expr.arg) {
                if (!compile_expr(c, n->u.yield_expr.arg)) return false;
            } else {
                emit_op(c, OP_UNDEF);
            }
            if (n->u.yield_expr.delegate) {
                emit_op(c, OP_YIELD_STAR);
                emit_u8(c, c->bc->fn_kind == AJS_FN_BC_ASYNC_GENERATOR ? 1 : 0);
            } else {
                emit_op(c, OP_YIELD);
            }
            return true;

        case AST_AWAIT_EXPR:
            if (!compile_expr(c, n->u.expr_stmt.expr)) return false;
            emit_op(c, OP_AWAIT);
            return true;

        default:
            break;
    }

    comp_error(c, n->line, n->col, "Unsupported expression AST kind %d", (int)n->kind);
    return false;
}

/* ============================================================================
 * 7. Function & Class Compilation
 * ============================================================================ */

static void attach_lazy_source(AjsCompiler *c, AjsBytecode *bc, const char *slice_utf8, size_t slice_len) {
    if (!slice_utf8 || slice_len == 0) return;
    if (c->src_buf && c->src_base && slice_utf8 >= c->src_base) {
        size_t off = (size_t)(slice_utf8 - c->src_base);
        if (off + slice_len <= c->src_buf->len && off <= UINT32_MAX && slice_len <= UINT32_MAX) {
            bc->source_buf = ajs_source_buf_retain(c->src_buf);
            bc->src_offset = (uint32_t)off;
            bc->src_len = (uint32_t)slice_len;
            return;
        }
    }
    bc->source_text = ajs_str_from_utf8(c->vm, slice_utf8, slice_len, false);
}

static void init_compiler(AjsCompiler *c, AjsVm *vm, AjsRealm *realm, AjsCompiler *enclosing,
                          AjsFuncKind fn_kind, bool strict, const char *url_cstr) {
    memset(c, 0, sizeof(*c));
    c->vm = vm;
    c->realm = realm;
    c->enclosing = enclosing;
    c->src_base = enclosing ? enclosing->src_base : NULL;
    c->src_buf = enclosing ? enclosing->src_buf : NULL;
    c->strict = strict;
    c->eval_ret_slot = AJS_NO_SLOT;
    c->scope = &c->base_scope;
    c->base_scope.is_base = true;
    c->base_scope.has_runtime_env = true;
    c->base_scope.env_depth_idx = 0;

    AjsBytecode *bc = (AjsBytecode *)ajs_gc_alloc(vm, sizeof(AjsBytecode), AJS_GC_BYTECODE);
    c->bc = bc;
    ajs_temp_root_push(vm, ajs_val_from_gc(bc));

    bc->fn_kind = fn_kind;
    bc->strict = strict;
    bc->this_slot = AJS_NO_SLOT;
    bc->new_target_slot = AJS_NO_SLOT;
    bc->arguments_slot = AJS_NO_SLOT;
    bc->active_fn_slot = AJS_NO_SLOT;
    bc->source_url = (enclosing && enclosing->bc && enclosing->bc->source_url)
                         ? enclosing->bc->source_url
                         : ajs_str_from_cstr(vm, url_cstr ? url_cstr : "<script>", true);
}

static void cleanup_compiler(AjsCompiler *c) {
    comp_scope_cleanup(&c->base_scope);
    free(c->const_hash);
    c->const_hash = NULL;
    c->const_hash_cap = 0;
}

static void finalize_base_scope_metadata(AjsCompiler *c) {
    AjsBytecode *bc = c->bc;
    uint32_t cnt = c->base_scope.slot_count;
    bc->env_slot_count = cnt;
    if (cnt > 0) {
        bool need_names = c->has_eval_or_with || bc->is_module || bc->is_eval;
        bc->env_slot_flags = (uint8_t *)calloc(cnt, sizeof(uint8_t));
        bc->env_slot_names = need_names ? (AjsString **)calloc(cnt, sizeof(AjsString *)) : NULL;
        for (uint32_t i = 0; i < cnt; i++) {
            AjsCompLocal *loc = &c->base_scope.locals[i];
            uint8_t fl = 0;
            if (loc->kind == BIND_CONST || loc->kind == BIND_IMPORT) {
                fl |= AJS_SLOT_CONST | AJS_SLOT_INIT_TDZ;
            } else if (loc->kind == BIND_LET || loc->kind == BIND_PARAM || loc->kind == BIND_PRIVATE) {
                fl |= AJS_SLOT_INIT_TDZ;
            } else if (loc->kind == BIND_FUNC_NAME) {
                fl |= AJS_SLOT_FUNC_NAME;
            }
            bc->env_slot_flags[i] = fl;
            if (need_names) bc->env_slot_names[i] = loc->str;
        }
    }
    if (bc->code_len > 0 && bc->code_cap > bc->code_len + 32u) {
        uint8_t *nc = (uint8_t *)realloc(bc->code, bc->code_len);
        if (nc) { bc->code = nc; bc->code_cap = bc->code_len; }
    }
    if (bc->const_count > 0 && bc->const_cap > bc->const_count + 8u) {
        AjsValue *nk = (AjsValue *)realloc(bc->consts, (size_t)bc->const_count * sizeof(AjsValue));
        if (nk) { bc->consts = nk; bc->const_cap = bc->const_count; }
    }
    if (bc->line_count > 0 && bc->line_cap > bc->line_count + 8u) {
        AjsLineSpan *nl = (AjsLineSpan *)realloc(bc->lines, (size_t)bc->line_count * sizeof(AjsLineSpan));
        if (nl) { bc->lines = nl; bc->line_cap = bc->line_count; }
    }
    cleanup_compiler(c);
}

static AjsBytecode *compile_func_node(AjsCompiler *parent, AjsAstNode *fn_node,
                                      const char *default_name_cstr, bool is_derived_ctor) {
    AjsVm *vm = parent->vm;
    size_t root_save = ajs_temp_root_save(vm);

    AjsFuncKind fk = fn_node->u.func.fn_kind;
    bool strict = parent->strict || fn_node->u.func.strict;

    AjsCompiler fc;
    const char *url_cstr = parent->bc->source_url ? parent->bc->source_url->utf8 : "<script>";
    init_compiler(&fc, vm, parent->realm, parent, fk, strict, url_cstr);
    fc.has_eval_or_with = (parent->has_eval_or_with && !strict) || (parent->with_depth > 0);

    AjsBytecode *bc = fc.bc;
    bc->is_derived_ctor = is_derived_ctor;
    bc->has_simple_params = fn_node->u.func.has_simple_params;
    if (fn_node->u.func.name_u16) {
        bc->name = ajs_str_from_utf16(vm, fn_node->u.func.name_u16, fn_node->u.func.name_len, true);
    } else if (default_name_cstr) {
        bc->name = ajs_str_from_cstr(vm, default_name_cstr, true);
    } else {
        bc->name = vm->str_empty;
    }
    if (fn_node->u.func.src_utf8 && fn_node->u.func.src_len > 0) {
        attach_lazy_source(&fc, bc, fn_node->u.func.src_utf8, fn_node->u.func.src_len);
    }

    /* Scan function AST for nested arrows, `super`, `eval`/`with`, and `arguments` */
    AjsFnScanInfo scan;
    memset(&scan, 0, sizeof(scan));
    for (uint32_t i = 0; i < fn_node->u.func.params.count; i++) {
        scan_fn_ast(fn_node->u.func.params.items[i], false, &scan);
    }
    scan_fn_ast(fn_node->u.func.body, false, &scan);
    if (scan.has_eval_or_with) fc.has_eval_or_with = true;

    bool is_arrow = (fk == AJS_FN_BC_ARROW || fk == AJS_FN_BC_ASYNC_ARROW);
    bc->needs_arguments = !is_arrow && scan.uses_arguments;

    /* Compute Function.length (arity up to first default or rest parameter) */
    uint32_t arity = 0;
    bool seen_opt_or_rest = false;
    uint32_t raw_param_count = fn_node->u.func.params.count;
    for (uint32_t i = 0; i < raw_param_count; i++) {
        AjsAstNode *p = fn_node->u.func.params.items[i];
        if (!p) continue;
        if (p->kind == AST_REST_ELEMENT) {
            bc->has_rest_param = true;
            seen_opt_or_rest = true;
        } else if (p->kind == AST_ASSIGN_PATTERN) {
            seen_opt_or_rest = true;
        } else if (!seen_opt_or_rest) {
            arity++;
        }
    }
    bc->arity = arity;
    bc->param_count = raw_param_count;

    if (!is_arrow && (is_derived_ctor || scan.has_arrow_or_super)) {
        bc->this_slot = comp_scope_add(&fc, &fc.base_scope, NULL, 0, is_derived_ctor ? BIND_LET : BIND_VAR, false);
        bc->new_target_slot = comp_scope_add(&fc, &fc.base_scope, NULL, 0, BIND_VAR, false);
        bc->active_fn_slot = comp_scope_add(&fc, &fc.base_scope, NULL, 0, BIND_VAR, false);
        if (bc->needs_arguments) {
            bc->arguments_slot = comp_scope_add(&fc, &fc.base_scope, NULL, 0, BIND_VAR, false);
        }
    }

    if (fn_node->kind == AST_FUNC_EXPR && fn_node->u.func.name_u16) {
        comp_scope_add(&fc, &fc.base_scope, fn_node->u.func.name_u16, fn_node->u.func.name_len, BIND_FUNC_NAME, false);
    }

    for (uint32_t i = 0; i < raw_param_count; i++) {
        AjsAstNode *p = fn_node->u.func.params.items[i];
        if (p && p->kind == AST_IDENT) {
            int32_t ex = comp_scope_find_own(&fc.base_scope, p->u.ident.u16, p->u.ident.len, false);
            if (ex >= 0 && fc.base_scope.locals[ex].kind != BIND_FUNC_NAME) {
                continue;
            }
            if (ex >= 0 && fc.base_scope.locals[ex].kind == BIND_FUNC_NAME) {
                fc.base_scope.locals[ex].kind = BIND_PARAM;
                continue;
            }
            comp_scope_add(&fc, &fc.base_scope, p->u.ident.u16, p->u.ident.len, BIND_PARAM, false);
        } else {
            collect_pattern_vars(&fc, &fc.base_scope, p, BIND_PARAM, false);
        }
    }

    if (!strict && bc->has_simple_params && bc->needs_arguments && raw_param_count > 0) {
        bc->mapped_param_slots = (uint32_t *)malloc((size_t)raw_param_count * sizeof(uint32_t));
        for (uint32_t i = 0; i < raw_param_count; i++) {
            bc->mapped_param_slots[i] = AJS_NO_SLOT;
            AjsAstNode *p = fn_node->u.func.params.items[i];
            if (p && p->kind == AST_IDENT) {
                bool dup_later = false;
                for (uint32_t j = i + 1u; j < raw_param_count; j++) {
                    AjsAstNode *p2 = fn_node->u.func.params.items[j];
                    if (p2 && p2->kind == AST_IDENT &&
                        c_u16_eq(p->u.ident.u16, p->u.ident.len, p2->u.ident.u16, p2->u.ident.len)) {
                        dup_later = true;
                        break;
                    }
                }
                if (!dup_later) {
                    int32_t idx = comp_scope_find_own(&fc.base_scope, p->u.ident.u16, p->u.ident.len, false);
                    if (idx >= 0) bc->mapped_param_slots[i] = fc.base_scope.locals[idx].slot;
                }
            }
        }
    }

    if (!fn_node->u.func.expr_body && fn_node->u.func.body && fn_node->u.func.body->kind == AST_BLOCK) {
        AjsNodeList *bstmts = &fn_node->u.func.body->u.block.stmts;
        for (uint32_t i = 0; i < bstmts->count; i++) {
            hoist_vars_in_stmt(&fc, bstmts->items[i], false);
        }
        collect_block_lex_in_list(&fc, &fc.base_scope, bstmts, false);
    }

    for (uint32_t i = 0; i < raw_param_count; i++) {
        AjsAstNode *p = fn_node->u.func.params.items[i];
        if (p && p->kind == AST_REST_ELEMENT) {
            if (!compile_pattern_assign(&fc, p->u.expr_stmt.expr, true, AJS_VAR_LET)) goto fail;
        } else {
            if (!compile_pattern_assign(&fc, p, true, AJS_VAR_LET)) goto fail;
        }
    }

    if (fk == AJS_FN_BC_CLASS_CTOR && !is_derived_ctor) {
        emit_op(&fc, OP_RUN_INST_INIT);
    }

    if (fk == AJS_FN_BC_GENERATOR || fk == AJS_FN_BC_ASYNC_GENERATOR) {
        emit_op(&fc, OP_UNDEF);
        emit_op(&fc, OP_YIELD);
        emit_op(&fc, OP_POP);
    }

    if (fn_node->u.func.expr_body) {
        if (!compile_expr(&fc, fn_node->u.func.body)) goto fail;
        emit_op(&fc, OP_RETURN);
    } else if (fn_node->u.func.body && fn_node->u.func.body->kind == AST_BLOCK) {
        AjsNodeList *bstmts = &fn_node->u.func.body->u.block.stmts;
        if (!instantiate_hoisted_funcs(&fc, &fc.base_scope, bstmts, false)) goto fail;
        uint32_t fn_disp_fin = UINT32_MAX;
        if (fc.base_scope.has_disp_stack) {
            fn_disp_fin = emit_dispose_enter(&fc, 0, fc.base_scope.disp_slot);
        }
        for (uint32_t i = 0; i < bstmts->count; i++) {
            if (!compile_stmt(&fc, bstmts->items[i])) goto fail;
        }
        if (fc.base_scope.has_disp_stack) {
            emit_dispose_finally(&fc, 0, fc.base_scope.disp_slot, fn_disp_fin);
        }
        emit_op(&fc, OP_UNDEF);
        if (is_derived_ctor) {
            emit_op(&fc, OP_RETURN_DERIVED);
            emit_u32(&fc, bc->this_slot);
        } else {
            emit_op(&fc, OP_RETURN);
        }
    }

    finalize_base_scope_metadata(&fc);
    ajs_temp_root_restore(vm, root_save);
    return bc;

fail:
    if (fc.had_error && !parent->had_error) {
        parent->had_error = true;
        snprintf(parent->err_msg, sizeof(parent->err_msg), "%s", fc.err_msg);
        parent->err_line = fc.err_line;
        parent->err_col = fc.err_col;
    }
    cleanup_compiler(&fc);
    ajs_temp_root_restore(vm, root_save);
    return NULL;
}

/* Synthesize `<instance_init>` bytecode for class instance private methods/accessors + instance fields */
static AjsBytecode *compile_class_instance_init(AjsCompiler *c, AjsAstNode *kl, const uint32_t *comp_key_slots) {
    bool needed = false;
    for (uint32_t i = 0; i < kl->u.klass.elements.count; i++) {
        AjsAstNode *el = kl->u.klass.elements.items[i];
        if (!el || el->u.prop.is_static) continue;
        if (el->u.prop.is_private || el->u.prop.kind == AJS_PROP_FIELD) {
            needed = true;
            break;
        }
    }
    if (!needed) return NULL;

    AjsVm *vm = c->vm;
    size_t rs = ajs_temp_root_save(vm);
    AjsCompiler ic;
    const char *url_cstr = c->bc->source_url ? c->bc->source_url->utf8 : "<script>";
    init_compiler(&ic, vm, c->realm, c, AJS_FN_BC_METHOD, true, url_cstr);
    ic.bc->name = ajs_str_from_cstr(vm, "<instance_init>", true);

    /* Check if any field initializer uses nested arrow or `super` */
    AjsFnScanInfo scan;
    memset(&scan, 0, sizeof(scan));
    for (uint32_t i = 0; i < kl->u.klass.elements.count; i++) {
        AjsAstNode *el = kl->u.klass.elements.items[i];
        if (el && !el->u.prop.is_static && el->u.prop.kind == AJS_PROP_FIELD) {
            scan_fn_ast(el->u.prop.value, false, &scan);
        }
    }
    if (scan.has_arrow_or_super) {
        ic.bc->this_slot = comp_scope_add(&ic, &ic.base_scope, NULL, 0, BIND_VAR, false);
        ic.bc->new_target_slot = comp_scope_add(&ic, &ic.base_scope, NULL, 0, BIND_VAR, false);
        ic.bc->active_fn_slot = comp_scope_add(&ic, &ic.base_scope, NULL, 0, BIND_VAR, false);
    }

    /* Step 1 (ECMA-262): Install all instance private methods & accessors onto `this` BEFORE running field initializers! */
    uint32_t comp_idx = 0;
    for (uint32_t i = 0; i < kl->u.klass.elements.count; i++) {
        AjsAstNode *el = kl->u.klass.elements.items[i];
        if (!el) continue;
        if (el->u.prop.computed) comp_idx++;
        if (el->u.prop.is_static || !el->u.prop.is_private || el->u.prop.kind == AJS_PROP_FIELD) continue;

        uint16_t d = 0;
        uint32_t s = 0;
        resolve_binding(&ic, el->u.prop.key->u.ident.u16, el->u.prop.key->u.ident.len, true, &d, &s, NULL);

        AjsBytecode *mbc = compile_func_node(&ic, el->u.prop.value, el->u.prop.key->u.ident.utf8, false);
        if (!mbc || ic.had_error) goto fail;
        uint32_t bc_idx = add_const(&ic, ajs_val_from_gc(mbc));

        emit_op(&ic, OP_THIS);
        emit_op(&ic, OP_CLOSURE);
        emit_u32(&ic, bc_idx);
        uint8_t pk = (el->u.prop.kind == AJS_PROP_METHOD) ? 2 :
                     ((el->u.prop.kind == AJS_PROP_GET) ? 3 : 4); /* 2=method, 3=getter, 4=setter */
        emit_op(&ic, OP_DEF_PRIVATE);
        emit_u16(&ic, d);
        emit_u32(&ic, s);
        emit_u8(&ic, pk);
        emit_op(&ic, OP_POP);
    }

    /* Step 2: Initialize all instance fields (public & private) in exact source order */
    comp_idx = 0;
    for (uint32_t i = 0; i < kl->u.klass.elements.count; i++) {
        AjsAstNode *el = kl->u.klass.elements.items[i];
        if (!el) continue;
        uint32_t ck_slot = el->u.prop.computed ? comp_key_slots[comp_idx++] : AJS_NO_SLOT;
        if (el->u.prop.is_static || el->u.prop.kind != AJS_PROP_FIELD) continue;

        emit_op(&ic, OP_THIS);
        if (el->u.prop.is_private) {
            uint16_t d = 0;
            uint32_t s = 0;
            resolve_binding(&ic, el->u.prop.key->u.ident.u16, el->u.prop.key->u.ident.len, true, &d, &s, NULL);
            if (el->u.prop.value) {
                if (!compile_expr(&ic, el->u.prop.value)) goto fail;
                if (is_anon_fn_or_class_def(el->u.prop.value)) {
                    uint32_t kidx = add_str_u16(&ic, el->u.prop.key->u.ident.u16, el->u.prop.key->u.ident.len);
                    emit_op(&ic, OP_CONST); emit_u32(&ic, kidx);
                    emit_op(&ic, OP_SET_FN_NAME); emit_u8(&ic, 0);
                }
            } else {
                emit_op(&ic, OP_UNDEF);
            }
            emit_op(&ic, OP_DEF_PRIVATE);
            emit_u16(&ic, d);
            emit_u32(&ic, s);
            emit_u8(&ic, 1); /* 1 = private field */
            emit_op(&ic, OP_POP);
        } else if (el->u.prop.computed) {
            /* Read pre-evaluated computed key from enclosing class scope (depth = 1)! */
            emit_get_lex(&ic, 1, ck_slot);
            if (el->u.prop.value) {
                if (!compile_expr(&ic, el->u.prop.value)) goto fail;
                if (is_anon_fn_or_class_def(el->u.prop.value)) {
                    emit_op(&ic, OP_SWAP);
                    emit_op(&ic, OP_DUP);
                    emit_op(&ic, OP_ROT3);
                    emit_op(&ic, OP_SET_FN_NAME);
                    emit_u8(&ic, 0);
                }
            } else {
                emit_op(&ic, OP_UNDEF);
            }
            emit_op(&ic, OP_DEFINE_ELEM);
            emit_u8(&ic, 0);
            emit_op(&ic, OP_POP);
        } else {
            uint32_t kidx = 0;
            if (el->u.prop.key->kind == AST_IDENT) {
                kidx = add_str_u16(&ic, el->u.prop.key->u.ident.u16, el->u.prop.key->u.ident.len);
            } else if (el->u.prop.key->kind == AST_LITERAL_STR) {
                kidx = add_str_u16(&ic, el->u.prop.key->u.lit.s_u16, el->u.prop.key->u.lit.s_len);
            } else {
                AjsString *ns = ajs_number_to_str(vm, el->u.prop.key->u.lit.nval, 10);
                kidx = add_const(&ic, ajs_val_from_gc(ns));
            }
            if (el->u.prop.value) {
                if (!compile_expr(&ic, el->u.prop.value)) goto fail;
                if (is_anon_fn_or_class_def(el->u.prop.value)) {
                    emit_op(&ic, OP_CONST); emit_u32(&ic, kidx);
                    emit_op(&ic, OP_SET_FN_NAME); emit_u8(&ic, 0);
                }
            } else {
                emit_op(&ic, OP_UNDEF);
            }
            emit_op(&ic, OP_DEFINE_PROP);
            emit_u32(&ic, kidx);
            emit_u8(&ic, 0);
            emit_op(&ic, OP_POP);
        }
    }

    emit_op(&ic, OP_UNDEF);
    emit_op(&ic, OP_RETURN);
    finalize_base_scope_metadata(&ic);
    ajs_temp_root_restore(vm, rs);
    return ic.bc;

fail:
    if (ic.had_error && !c->had_error) {
        c->had_error = true;
        snprintf(c->err_msg, sizeof(c->err_msg), "%s", ic.err_msg);
        c->err_line = ic.err_line;
        c->err_col = ic.err_col;
    }
    cleanup_compiler(&ic);
    ajs_temp_root_restore(vm, rs);
    return NULL;
}

/* Compile a default constructor when a class omits an explicit `constructor` */
static AjsBytecode *compile_default_class_ctor(AjsCompiler *c, AjsAstNode *kl, const char *cls_name, bool has_super) {
    AjsVm *vm = c->vm;
    size_t rs = ajs_temp_root_save(vm);
    AjsCompiler cc;
    const char *url_cstr = c->bc->source_url ? c->bc->source_url->utf8 : "<script>";
    init_compiler(&cc, vm, c->realm, c, AJS_FN_BC_CLASS_CTOR, true, url_cstr);
    cc.bc->is_derived_ctor = has_super;
    cc.bc->has_simple_params = true;
    cc.bc->name = ajs_str_from_cstr(vm, cls_name ? cls_name : "", true);
    if (kl->u.klass.src_utf8 && kl->u.klass.src_len > 0) {
        attach_lazy_source(&cc, cc.bc, kl->u.klass.src_utf8, kl->u.klass.src_len);
    }

    if (has_super) {
        /* Default derived constructor: `constructor(...args) { super(...args); }` */
        cc.bc->needs_arguments = true;
        cc.bc->this_slot = comp_scope_add(&cc, &cc.base_scope, NULL, 0, BIND_LET, false);
        cc.bc->new_target_slot = comp_scope_add(&cc, &cc.base_scope, NULL, 0, BIND_VAR, false);
        cc.bc->active_fn_slot = comp_scope_add(&cc, &cc.base_scope, NULL, 0, BIND_VAR, false);
        cc.bc->arguments_slot = comp_scope_add(&cc, &cc.base_scope, NULL, 0, BIND_VAR, false);

        emit_op(&cc, OP_NEW_ARRAY);
        emit_u32(&cc, 0);
        emit_get_lex(&cc, 0, cc.bc->arguments_slot);
        emit_op(&cc, OP_ARRAY_SPREAD);
        emit_op(&cc, OP_SUPER_CALL);
        emit_u16(&cc, 0);
        emit_u32(&cc, cc.bc->this_slot);
        emit_u32(&cc, cc.bc->new_target_slot);
        emit_u32(&cc, cc.bc->active_fn_slot);
        emit_op(&cc, OP_RETURN_DERIVED);
        emit_u32(&cc, cc.bc->this_slot);
    } else {
        emit_op(&cc, OP_RUN_INST_INIT);
        emit_op(&cc, OP_UNDEF);
        emit_op(&cc, OP_RETURN);
    }

    finalize_base_scope_metadata(&cc);
    ajs_temp_root_restore(vm, rs);
    return cc.bc;
}

static bool compile_class_node(AjsCompiler *c, AjsAstNode *kl, AjsString *inferred_name, bool push_ctor) {
    bool saved_strict = c->strict;
    c->strict = true; /* ECMA-262: All parts of a ClassDefinition are strict mode code! */

    /* Push inner class lexical scope holding:
     * - Class self-name binding (immutable `BIND_CONST`) if named
     * - Private member symbols (`BIND_PRIVATE`)
     * - Computed property key slots (`comp_key_slots`) so computed keys evaluate ONCE in source order! */
    AjsCompScope cls_sc;
    memset(&cls_sc, 0, sizeof(cls_sc));
    cls_sc.parent = c->scope;
    cls_sc.has_runtime_env = true;

    uint32_t self_name_slot = AJS_NO_SLOT;
    AjsString *cls_name_str = NULL;
    if (kl->u.klass.name_u16) {
        cls_name_str = ajs_str_from_utf16(c->vm, kl->u.klass.name_u16, kl->u.klass.name_len, true);
        self_name_slot = comp_scope_add(c, &cls_sc, kl->u.klass.name_u16, kl->u.klass.name_len, BIND_CONST, false);
    } else if (inferred_name) {
        cls_name_str = inferred_name;
    } else {
        cls_name_str = c->vm->str_empty;
    }

    uint32_t comp_count = 0;
    for (uint32_t i = 0; i < kl->u.klass.elements.count; i++) {
        AjsAstNode *el = kl->u.klass.elements.items[i];
        if (!el) continue;
        if (el->u.prop.computed) comp_count++;
        if (el->u.prop.is_private) {
            int32_t ex = comp_scope_find_own(&cls_sc, el->u.prop.key->u.ident.u16, el->u.prop.key->u.ident.len, true);
            if (ex < 0) {
                comp_scope_add(c, &cls_sc, el->u.prop.key->u.ident.u16, el->u.prop.key->u.ident.len, BIND_PRIVATE, true);
            }
        }
    }

    uint32_t *comp_key_slots = comp_count ? (uint32_t *)malloc(comp_count * sizeof(uint32_t)) : NULL;
    for (uint32_t i = 0; i < comp_count; i++) {
        comp_key_slots[i] = comp_scope_add(c, &cls_sc, NULL, 0, BIND_VAR, false);
    }
    uint32_t ctor_tmp_slot = comp_scope_add(c, &cls_sc, NULL, 0, BIND_VAR, false);
    uint32_t proto_tmp_slot = comp_scope_add(c, &cls_sc, NULL, 0, BIND_VAR, false);

    c->cur_env_depth++;
    cls_sc.env_depth_idx = c->cur_env_depth;
    c->scope = &cls_sc;

    uint32_t ed_idx = build_env_desc(c, &cls_sc);
    emit_push_env(c, cls_sc.slot_count, ed_idx);

    for (uint32_t i = 0; i < cls_sc.slot_count; i++) {
        if (cls_sc.locals[i].is_priv) {
            uint32_t d_idx = add_const(c, ajs_val_from_gc(cls_sc.locals[i].str));
            emit_op(c, OP_NEW_PRIV_SYM);
            emit_u32(c, d_idx);
            emit_init_lex(c, 0, cls_sc.locals[i].slot);
            emit_op(c, OP_POP);
        }
    }

    bool has_super = (kl->u.klass.super_expr != NULL);
    if (has_super) {
        if (!compile_expr(c, kl->u.klass.super_expr)) goto fail;
    } else {
        emit_op(c, OP_UNDEF);
    }

    uint32_t c_idx = 0;
    for (uint32_t i = 0; i < kl->u.klass.elements.count; i++) {
        AjsAstNode *el = kl->u.klass.elements.items[i];
        if (el && el->u.prop.computed) {
            if (!compile_expr(c, el->u.prop.key)) goto fail;
            emit_op(c, OP_TO_PROP_KEY);
            emit_init_lex(c, 0, comp_key_slots[c_idx++]);
            emit_op(c, OP_POP);
        }
    }

    AjsAstNode *ctor_elem = NULL;
    for (uint32_t i = 0; i < kl->u.klass.elements.count; i++) {
        AjsAstNode *el = kl->u.klass.elements.items[i];
        if (el && el->u.prop.value && el->u.prop.value->kind == AST_FUNC_EXPR &&
            el->u.prop.value->u.func.fn_kind == AJS_FN_BC_CLASS_CTOR) {
            ctor_elem = el;
            break;
        }
    }

    AjsBytecode *ctor_bc = NULL;
    if (ctor_elem) {
        ctor_bc = compile_func_node(c, ctor_elem->u.prop.value, cls_name_str->utf8, has_super);
        if (ctor_bc && kl->u.klass.src_utf8 && kl->u.klass.src_len > 0) {
            ctor_bc->source_text = NULL;
            ajs_source_buf_release(ctor_bc->source_buf);
            ctor_bc->source_buf = NULL;
            attach_lazy_source(c, ctor_bc, kl->u.klass.src_utf8, kl->u.klass.src_len);
        }
    } else {
        ctor_bc = compile_default_class_ctor(c, kl, cls_name_str->utf8, has_super);
    }
    if (!ctor_bc || c->had_error) goto fail;

    emit_op(c, OP_CLOSURE);
    emit_u32(c, add_const(c, ajs_val_from_gc(ctor_bc)));

    AjsBytecode *inst_init_bc = compile_class_instance_init(c, kl, comp_key_slots);
    if (c->had_error) goto fail;
    if (inst_init_bc) {
        emit_op(c, OP_CLOSURE);
        emit_u32(c, add_const(c, ajs_val_from_gc(inst_init_bc)));
    } else {
        emit_op(c, OP_UNDEF);
    }

    emit_op(c, OP_BUILD_CLASS);
    emit_u32(c, add_const(c, ajs_val_from_gc(cls_name_str)));
    emit_u8(c, has_super ? 1 : 0);

    emit_init_lex(c, 0, proto_tmp_slot); emit_op(c, OP_POP);
    if (self_name_slot != AJS_NO_SLOT) {
        emit_init_lex(c, 0, self_name_slot);
    }
    emit_init_lex(c, 0, ctor_tmp_slot); emit_op(c, OP_POP);

    c_idx = 0;
    for (uint32_t i = 0; i < kl->u.klass.elements.count; i++) {
        AjsAstNode *el = kl->u.klass.elements.items[i];
        if (!el) continue;
        uint32_t ck_slot = el->u.prop.computed ? comp_key_slots[c_idx++] : AJS_NO_SLOT;
        if (el == ctor_elem || el->u.prop.kind == AJS_PROP_FIELD || el->u.prop.kind == AJS_PROP_STATIC_BLOCK) continue;
        if (!el->u.prop.is_static && el->u.prop.is_private) continue;

        uint32_t target_slot = el->u.prop.is_static ? ctor_tmp_slot : proto_tmp_slot;
        emit_get_lex(c, 0, target_slot);

        AjsBytecode *mbc = compile_func_node(c, el->u.prop.value, NULL, false);
        if (!mbc || c->had_error) goto fail;

        if (el->u.prop.is_private) {
            uint16_t d = 0;
            uint32_t s = 0;
            resolve_binding(c, el->u.prop.key->u.ident.u16, el->u.prop.key->u.ident.len, true, &d, &s, NULL);
            mbc->name = cls_sc.locals[s].str;
            emit_op(c, OP_CLOSURE); emit_u32(c, add_const(c, ajs_val_from_gc(mbc)));
            emit_op(c, OP_SET_HOME_OBJ);
            uint8_t pk = (el->u.prop.kind == AJS_PROP_METHOD) ? 2 :
                         ((el->u.prop.kind == AJS_PROP_GET) ? 3 : 4);
            emit_op(c, OP_DEF_PRIVATE); emit_u16(c, d); emit_u32(c, s); emit_u8(c, pk);
            emit_op(c, OP_POP);
            continue;
        }

        uint8_t def_kind = 0x10u;
        uint8_t fn_prefix = 0;
        if (el->u.prop.kind == AJS_PROP_GET) { def_kind |= 1u; fn_prefix = 1; }
        else if (el->u.prop.kind == AJS_PROP_SET) { def_kind |= 2u; fn_prefix = 2; }
        else { def_kind |= 3u; }

        if (el->u.prop.computed) {
            emit_get_lex(c, 0, ck_slot);
            emit_op(c, OP_CLOSURE); emit_u32(c, add_const(c, ajs_val_from_gc(mbc)));
            emit_op(c, OP_SWAP); emit_op(c, OP_DUP); emit_op(c, OP_ROT3);
            emit_op(c, OP_SET_FN_NAME); emit_u8(c, fn_prefix);
            emit_op(c, OP_ROT3); emit_op(c, OP_ROT3); emit_op(c, OP_SWAP);
            emit_op(c, OP_SET_HOME_OBJ);
            emit_op(c, OP_ROT3L); emit_op(c, OP_SWAP);
            emit_op(c, OP_DEFINE_ELEM); emit_u8(c, def_kind);
            emit_op(c, OP_POP);
        } else {
            uint32_t kidx = 0;
            if (el->u.prop.key->kind == AST_IDENT) {
                kidx = add_str_u16(c, el->u.prop.key->u.ident.u16, el->u.prop.key->u.ident.len);
            } else if (el->u.prop.key->kind == AST_LITERAL_STR) {
                kidx = add_str_u16(c, el->u.prop.key->u.lit.s_u16, el->u.prop.key->u.lit.s_len);
            } else {
                AjsString *ns = ajs_number_to_str(c->vm, el->u.prop.key->u.lit.nval, 10);
                kidx = add_const(c, ajs_val_from_gc(ns));
            }
            emit_op(c, OP_CLOSURE); emit_u32(c, add_const(c, ajs_val_from_gc(mbc)));
            emit_op(c, OP_CONST); emit_u32(c, kidx);
            emit_op(c, OP_SET_FN_NAME); emit_u8(c, fn_prefix);
            emit_op(c, OP_SET_HOME_OBJ);
            emit_op(c, OP_DEFINE_PROP); emit_u32(c, kidx); emit_u8(c, def_kind);
            emit_op(c, OP_POP);
        }
    }

    c_idx = 0;
    for (uint32_t i = 0; i < kl->u.klass.elements.count; i++) {
        AjsAstNode *el = kl->u.klass.elements.items[i];
        if (!el) continue;
        uint32_t ck_slot = el->u.prop.computed ? comp_key_slots[c_idx++] : AJS_NO_SLOT;
        if (!el->u.prop.is_static) continue;

        if (el->u.prop.kind == AJS_PROP_STATIC_BLOCK) {
            AjsAstNode synth_fn;
            memset(&synth_fn, 0, sizeof(synth_fn));
            synth_fn.kind = AST_FUNC_EXPR;
            synth_fn.line = el->line;
            synth_fn.col = el->col;
            synth_fn.u.func.fn_kind = AJS_FN_BC_METHOD;
            synth_fn.u.func.strict = true;
            synth_fn.u.func.has_simple_params = true;
            synth_fn.u.func.body = el->u.prop.value;

            AjsBytecode *sbbc = compile_func_node(c, &synth_fn, "<static_initializer>", false);
            if (!sbbc || c->had_error) goto fail;
            emit_get_lex(c, 0, ctor_tmp_slot);
            emit_op(c, OP_CLOSURE); emit_u32(c, add_const(c, ajs_val_from_gc(sbbc)));
            emit_op(c, OP_SET_HOME_OBJ);
            emit_op(c, OP_CALL_METHOD); emit_u16(c, 0);
            emit_op(c, OP_POP);
        } else if (el->u.prop.kind == AJS_PROP_FIELD) {
            AjsBytecode *sfbc = NULL;
            if (el->u.prop.value) {
                AjsAstNode synth_fn;
                memset(&synth_fn, 0, sizeof(synth_fn));
                synth_fn.kind = AST_FUNC_EXPR;
                synth_fn.line = el->line;
                synth_fn.col = el->col;
                synth_fn.u.func.fn_kind = AJS_FN_BC_METHOD;
                synth_fn.u.func.strict = true;
                synth_fn.u.func.has_simple_params = true;
                synth_fn.u.func.expr_body = true;
                synth_fn.u.func.body = el->u.prop.value;
                sfbc = compile_func_node(c, &synth_fn, "<static_field>", false);
                if (!sfbc || c->had_error) goto fail;
            }

            emit_get_lex(c, 0, ctor_tmp_slot);
            if (el->u.prop.is_private) {
                uint16_t d = 0;
                uint32_t s = 0;
                resolve_binding(c, el->u.prop.key->u.ident.u16, el->u.prop.key->u.ident.len, true, &d, &s, NULL);
                if (sfbc) {
                    emit_get_lex(c, 0, ctor_tmp_slot);
                    emit_op(c, OP_CLOSURE); emit_u32(c, add_const(c, ajs_val_from_gc(sfbc)));
                    emit_op(c, OP_SET_HOME_OBJ);
                    emit_op(c, OP_CALL_METHOD); emit_u16(c, 0);
                    if (is_anon_fn_or_class_def(el->u.prop.value)) {
                        uint32_t kidx = add_str_u16(c, el->u.prop.key->u.ident.u16, el->u.prop.key->u.ident.len);
                        emit_op(c, OP_CONST); emit_u32(c, kidx);
                        emit_op(c, OP_SET_FN_NAME); emit_u8(c, 0);
                    }
                } else {
                    emit_op(c, OP_UNDEF);
                }
                emit_op(c, OP_DEF_PRIVATE); emit_u16(c, d); emit_u32(c, s); emit_u8(c, 1);
                emit_op(c, OP_POP);
            } else if (el->u.prop.computed) {
                emit_get_lex(c, 0, ck_slot);
                if (sfbc) {
                    emit_get_lex(c, 0, ctor_tmp_slot);
                    emit_op(c, OP_CLOSURE); emit_u32(c, add_const(c, ajs_val_from_gc(sfbc)));
                    emit_op(c, OP_SET_HOME_OBJ);
                    emit_op(c, OP_CALL_METHOD); emit_u16(c, 0);
                    if (is_anon_fn_or_class_def(el->u.prop.value)) {
                        emit_op(c, OP_SWAP); emit_op(c, OP_DUP); emit_op(c, OP_ROT3);
                        emit_op(c, OP_SET_FN_NAME); emit_u8(c, 0);
                    }
                } else {
                    emit_op(c, OP_UNDEF);
                }
                emit_op(c, OP_DEFINE_ELEM); emit_u8(c, 0);
                emit_op(c, OP_POP);
            } else {
                uint32_t kidx = 0;
                if (el->u.prop.key->kind == AST_IDENT) {
                    kidx = add_str_u16(c, el->u.prop.key->u.ident.u16, el->u.prop.key->u.ident.len);
                } else if (el->u.prop.key->kind == AST_LITERAL_STR) {
                    kidx = add_str_u16(c, el->u.prop.key->u.lit.s_u16, el->u.prop.key->u.lit.s_len);
                } else {
                    AjsString *ns = ajs_number_to_str(c->vm, el->u.prop.key->u.lit.nval, 10);
                    kidx = add_const(c, ajs_val_from_gc(ns));
                }
                if (sfbc) {
                    emit_get_lex(c, 0, ctor_tmp_slot);
                    emit_op(c, OP_CLOSURE); emit_u32(c, add_const(c, ajs_val_from_gc(sfbc)));
                    emit_op(c, OP_SET_HOME_OBJ);
                    emit_op(c, OP_CALL_METHOD); emit_u16(c, 0);
                    if (is_anon_fn_or_class_def(el->u.prop.value)) {
                        emit_op(c, OP_CONST); emit_u32(c, kidx);
                        emit_op(c, OP_SET_FN_NAME); emit_u8(c, 0);
                    }
                } else {
                    emit_op(c, OP_UNDEF);
                }
                emit_op(c, OP_DEFINE_PROP); emit_u32(c, kidx); emit_u8(c, 0);
                emit_op(c, OP_POP);
            }
        }
    }

    if (push_ctor) {
        emit_get_lex(c, 0, ctor_tmp_slot);
    }

    emit_op(c, OP_POP_ENV);
    c->scope = cls_sc.parent;
    c->cur_env_depth--;
    c->strict = saved_strict;
    free(comp_key_slots);
    comp_scope_cleanup(&cls_sc);
    return true;

fail:
    c->scope = cls_sc.parent;
    c->cur_env_depth--;
    c->strict = saved_strict;
    free(comp_key_slots);
    comp_scope_cleanup(&cls_sc);
    return false;
}

/* ============================================================================
 * 8. Statement Compilation (Control Flow, Try/Catch/Finally, Loops, ESM)
 * ============================================================================ */

static void loop_push_patch(uint32_t **arr, uint32_t *cnt, uint32_t *cap, uint32_t off) {
    if (*cnt >= *cap) {
        uint32_t ncap = *cap ? (*cap * 2u) : 8u;
        *arr = (uint32_t *)realloc(*arr, ncap * sizeof(uint32_t));
        *cap = ncap;
    }
    (*arr)[(*cnt)++] = off;
}

static void emit_reset_eval_ret(AjsCompiler *c) {
    if (c->eval_ret_slot != AJS_NO_SLOT) {
        emit_op(c, OP_UNDEF);
        emit_set_lex(c, c->cur_env_depth, c->eval_ret_slot, 0);
        emit_op(c, OP_POP);
    }
}

static bool compile_var_decl_stmt(AjsCompiler *c, AjsAstNode *vd) {
    AjsVarKind vk = vd->u.var_decl.var_kind;
    for (uint32_t i = 0; i < vd->u.var_decl.decls.count; i++) {
        AjsAstNode *d = vd->u.var_decl.decls.items[i];
        AjsAstNode *id = d->u.declarator.id;
        AjsAstNode *init = d->u.declarator.init;
        if (!init) {
            if (vk != AJS_VAR_VAR && id->kind == AST_IDENT) {
                emit_op(c, OP_UNDEF);
                emit_store_ident(c, id->u.ident.u16, id->u.ident.len, true, vk);
                emit_op(c, OP_POP);
            }
            continue;
        }
        if (!compile_expr(c, init)) return false;
        if (id->kind == AST_IDENT && is_anon_fn_or_class_def(init)) {
            uint32_t fn_nidx = add_str_u16(c, id->u.ident.u16, id->u.ident.len);
            emit_op(c, OP_CONST); emit_u32(c, fn_nidx);
            emit_op(c, OP_SET_FN_NAME); emit_u8(c, 0);
        }
        if (vk == AJS_VAR_USING || vk == AJS_VAR_AWAIT_USING) {
            AjsCompScope *dsc = c->scope;
            while (dsc && !dsc->has_disp_stack) dsc = dsc->parent;
            if (!dsc) {
                comp_error(c, d->line, d->col, "Internal error: missing disposal scope for 'using'");
                return false;
            }
            uint16_t disp_depth = (uint16_t)(c->cur_env_depth - dsc->env_depth_idx);
            emit_get_lex(c, disp_depth, dsc->disp_slot);
            emit_op(c, OP_SWAP);
            emit_op(c, OP_DISPOSE_ADD);
            emit_u8(c, vk == AJS_VAR_AWAIT_USING ? 1 : 0);
        }
        if (!compile_pattern_assign(c, id, (vk != AJS_VAR_VAR), vk)) return false;
    }
    return true;
}

static bool compile_for_stmt(AjsCompiler *c, AjsAstNode *n, const uint16_t *lbl_u16, uint32_t lbl_len) {
    emit_reset_eval_ret(c);
    AjsAstNode *init = n->u.for_stmt.init;
    bool has_lex_init = (init && init->kind == AST_VAR_DECL && init->u.var_decl.var_kind != AJS_VAR_VAR);
    bool is_using_init = (has_lex_init && (init->u.var_decl.var_kind == AJS_VAR_USING ||
                                           init->u.var_decl.var_kind == AJS_VAR_AWAIT_USING));
    uint32_t for_disp_fin = UINT32_MAX;

    AjsCompScope loop_sc;
    memset(&loop_sc, 0, sizeof(loop_sc));
    if (has_lex_init) {
        loop_sc.parent = c->scope;
        loop_sc.has_runtime_env = true;
        AjsBindKind bk = is_const_var_kind(init->u.var_decl.var_kind) ? BIND_CONST : BIND_LET;
        for (uint32_t i = 0; i < init->u.var_decl.decls.count; i++) {
            collect_pattern_vars(c, &loop_sc, init->u.var_decl.decls.items[i]->u.declarator.id, bk, false);
        }
        if (is_using_init) {
            loop_sc.disp_slot = comp_scope_add(c, &loop_sc, NULL, 0, BIND_VAR, false);
            loop_sc.has_disp_stack = true;
        }
        c->cur_env_depth++;
        loop_sc.env_depth_idx = c->cur_env_depth;
        c->scope = &loop_sc;
        uint32_t ed_idx = build_env_desc(c, &loop_sc);
        emit_push_env(c, loop_sc.slot_count, ed_idx);
        if (is_using_init) {
            for_disp_fin = emit_dispose_enter(c, 0, loop_sc.disp_slot);
        }
    }

    if (init) {
        if (init->kind == AST_VAR_DECL) {
            if (!compile_var_decl_stmt(c, init)) goto fail;
        } else {
            if (!compile_expr(c, init)) goto fail;
            emit_op(c, OP_POP);
        }
    }

    if (has_lex_init && loop_sc.slot_count > 0) {
        emit_clone_env(c, loop_sc.slot_count);
    }

    AjsLoopCtx lctx;
    memset(&lctx, 0, sizeof(lctx));
    lctx.prev = c->loop_top;
    lctx.label_u16 = lbl_u16;
    lctx.label_len = lbl_len;
    lctx.try_depth = c->cur_try_depth;
    lctx.env_depth = c->cur_env_depth;
    lctx.cont_try_depth = c->cur_try_depth;
    lctx.cont_env_depth = c->cur_env_depth;
    lctx.cont_pc = UINT32_MAX;
    c->loop_top = &lctx;

    uint32_t test_pc = c->bc->code_len;
    uint32_t j_exit = UINT32_MAX;
    if (n->u.for_stmt.test) {
        if (!compile_expr(c, n->u.for_stmt.test)) goto fail_loop;
        j_exit = emit_jump(c, OP_JMP_FALSE);
    }

    if (!compile_stmt(c, n->u.for_stmt.body)) goto fail_loop;

    uint32_t cont_pc = c->bc->code_len;
    for (uint32_t i = 0; i < lctx.cont_count; i++) {
        patch_u32(c, lctx.cont_patches[i], cont_pc);
    }

    if (has_lex_init && loop_sc.slot_count > 0) {
        emit_clone_env(c, loop_sc.slot_count);
    }

    if (n->u.for_stmt.update) {
        if (!compile_expr(c, n->u.for_stmt.update)) goto fail_loop;
        emit_op(c, OP_POP);
    }

    emit_op(c, OP_JMP);
    emit_u32(c, test_pc);

    uint32_t break_pc = c->bc->code_len;
    if (j_exit != UINT32_MAX) patch_u32(c, j_exit, break_pc);
    for (uint32_t i = 0; i < lctx.break_count; i++) {
        patch_u32(c, lctx.break_patches[i], break_pc);
    }

    c->loop_top = lctx.prev;
    free(lctx.break_patches);
    free(lctx.cont_patches);

    if (has_lex_init) {
        if (is_using_init) {
            emit_dispose_finally(c, 0, loop_sc.disp_slot, for_disp_fin);
        }
        emit_op(c, OP_POP_ENV);
        c->scope = loop_sc.parent;
        c->cur_env_depth--;
        comp_scope_cleanup(&loop_sc);
    }
    return true;

fail_loop:
    c->loop_top = lctx.prev;
    free(lctx.break_patches);
    free(lctx.cont_patches);
fail:
    if (has_lex_init) {
        if (is_using_init) c->cur_try_depth--;
        c->scope = loop_sc.parent;
        c->cur_env_depth--;
        comp_scope_cleanup(&loop_sc);
    }
    return false;
}

static bool compile_for_in_of_stmt(AjsCompiler *c, AjsAstNode *n, const uint16_t *lbl_u16, uint32_t lbl_len) {
    emit_reset_eval_ret(c);
    bool is_of = n->u.for_in_of.is_of;
    bool is_await = n->u.for_in_of.is_await;
    AjsAstNode *left = n->u.for_in_of.left;
    bool has_lex_left = (left && left->kind == AST_VAR_DECL && left->u.var_decl.var_kind != AJS_VAR_VAR);
    bool is_using_left = (has_lex_left && (left->u.var_decl.var_kind == AJS_VAR_USING ||
                                           left->u.var_decl.var_kind == AJS_VAR_AWAIT_USING));

    AjsCompScope tdz_sc;
    memset(&tdz_sc, 0, sizeof(tdz_sc));
    if (has_lex_left) {
        tdz_sc.parent = c->scope;
        tdz_sc.has_runtime_env = true;
        AjsBindKind bk = is_const_var_kind(left->u.var_decl.var_kind) ? BIND_CONST : BIND_LET;
        collect_pattern_vars(c, &tdz_sc, left->u.var_decl.decls.items[0]->u.declarator.id, bk, false);
        c->cur_env_depth++;
        tdz_sc.env_depth_idx = c->cur_env_depth;
        c->scope = &tdz_sc;
        uint32_t ed_idx = build_env_desc(c, &tdz_sc);
        emit_push_env(c, tdz_sc.slot_count, ed_idx);
    }

    if (!compile_expr(c, n->u.for_in_of.right)) goto fail_tdz;

    if (has_lex_left) {
        emit_op(c, OP_POP_ENV);
        c->scope = tdz_sc.parent;
        c->cur_env_depth--;
        comp_scope_cleanup(&tdz_sc);
        memset(&tdz_sc, 0, sizeof(tdz_sc));
    }

    uint32_t j_nullish_for_in = UINT32_MAX;
    if (!is_of) {
        emit_op(c, OP_DUP);
        j_nullish_for_in = emit_jump(c, OP_JMP_NULLISH);
        emit_op(c, OP_FOR_IN_INIT);
    } else {
        emit_op(c, OP_GET_ITER);
        emit_u8(c, is_await ? 1 : 0);
    }

    AjsCompScope iter_sc;
    memset(&iter_sc, 0, sizeof(iter_sc));
    iter_sc.parent = c->scope;
    iter_sc.has_runtime_env = true;
    uint32_t raw_iter_slot = comp_scope_add(c, &iter_sc, NULL, 0, BIND_VAR, false);
    uint32_t next_fn_slot = comp_scope_add(c, &iter_sc, NULL, 0, BIND_VAR, false);
    uint32_t act_iter_slot = comp_scope_add(c, &iter_sc, NULL, 0, BIND_VAR, false);

    c->cur_env_depth++;
    iter_sc.env_depth_idx = c->cur_env_depth;
    c->scope = &iter_sc;
    emit_push_env(c, 3, AJS_NO_SLOT);

    emit_init_lex(c, 0, next_fn_slot); emit_op(c, OP_POP);
    emit_init_lex(c, 0, raw_iter_slot); emit_op(c, OP_POP);

    uint32_t fin_patch = UINT32_MAX;
    if (is_of) {
        emit_op(c, OP_TRY_PUSH);
        emit_u32(c, UINT32_MAX);
        fin_patch = c->bc->code_len;
        emit_u32(c, 0);
        c->cur_try_depth++;
    }

    AjsLoopCtx lctx;
    memset(&lctx, 0, sizeof(lctx));
    lctx.prev = c->loop_top;
    lctx.label_u16 = lbl_u16;
    lctx.label_len = lbl_len;
    lctx.try_depth = is_of ? (uint16_t)(c->cur_try_depth - 1u) : c->cur_try_depth;
    lctx.env_depth = c->cur_env_depth;
    lctx.cont_try_depth = c->cur_try_depth;
    lctx.cont_env_depth = c->cur_env_depth;
    uint32_t step_pc = c->bc->code_len;
    lctx.cont_pc = step_pc;
    c->loop_top = &lctx;

    if (is_of) {
        emit_op(c, OP_UNDEF);
        emit_set_lex(c, 0, act_iter_slot, 0);
        emit_op(c, OP_POP);
    }

    emit_get_lex(c, 0, raw_iter_slot);
    emit_get_lex(c, 0, next_fn_slot);
    emit_op(c, OP_ITER_NEXT);
    emit_op(c, OP_NIP);
    emit_op(c, OP_NIP);
    if (is_await) {
        emit_op(c, OP_AWAIT);
    }
    emit_op(c, OP_ITER_UNPACK);
    uint32_t j_done = emit_jump(c, OP_JMP_TRUE);

    if (is_of) {
        emit_get_lex(c, 0, raw_iter_slot);
        emit_set_lex(c, 0, act_iter_slot, 0);
        emit_op(c, OP_POP);
    }

    AjsCompScope body_lex_sc;
    memset(&body_lex_sc, 0, sizeof(body_lex_sc));
    if (has_lex_left) {
        body_lex_sc.parent = c->scope;
        body_lex_sc.has_runtime_env = true;
        AjsBindKind bk = is_const_var_kind(left->u.var_decl.var_kind) ? BIND_CONST : BIND_LET;
        collect_pattern_vars(c, &body_lex_sc, left->u.var_decl.decls.items[0]->u.declarator.id, bk, false);
        if (is_using_left) {
            body_lex_sc.disp_slot = comp_scope_add(c, &body_lex_sc, NULL, 0, BIND_VAR, false);
            body_lex_sc.has_disp_stack = true;
        }
        c->cur_env_depth++;
        body_lex_sc.env_depth_idx = c->cur_env_depth;
        c->scope = &body_lex_sc;
        uint32_t ed_idx = build_env_desc(c, &body_lex_sc);
        emit_push_env(c, body_lex_sc.slot_count, ed_idx);
    }

    if (left->kind == AST_VAR_DECL) {
        AjsVarKind vk = left->u.var_decl.var_kind;
        AjsAstNode *pat = left->u.var_decl.decls.items[0]->u.declarator.id;
        if (is_using_left) {
            emit_op(c, OP_DISPOSE_NEW);
            emit_init_lex(c, 0, body_lex_sc.disp_slot);
            emit_op(c, OP_POP);
            emit_get_lex(c, 0, body_lex_sc.disp_slot);
            emit_op(c, OP_SWAP);
            emit_op(c, OP_DISPOSE_ADD);
            emit_u8(c, vk == AJS_VAR_AWAIT_USING ? 1 : 0);
        }
        if (!compile_pattern_assign(c, pat, (vk != AJS_VAR_VAR), vk)) goto fail_body;
    } else {
        if (!compile_pattern_assign(c, left, false, AJS_VAR_LET)) goto fail_body;
    }

    uint32_t body_disp_fin = UINT32_MAX;
    if (is_using_left) {
        emit_op(c, OP_TRY_PUSH);
        emit_u32(c, UINT32_MAX);
        body_disp_fin = c->bc->code_len;
        emit_u32(c, 0);
        c->cur_try_depth++;
    }

    if (!compile_stmt(c, n->u.for_in_of.body)) {
        if (is_using_left) c->cur_try_depth--;
        goto fail_body;
    }

    if (is_using_left) {
        emit_dispose_finally(c, 0, body_lex_sc.disp_slot, body_disp_fin);
    }

    if (has_lex_left) {
        emit_op(c, OP_POP_ENV);
        c->scope = body_lex_sc.parent;
        c->cur_env_depth--;
        comp_scope_cleanup(&body_lex_sc);
        memset(&body_lex_sc, 0, sizeof(body_lex_sc));
    }

    emit_op(c, OP_JMP);
    emit_u32(c, step_pc);

    patch_jump(c, j_done);
    emit_op(c, OP_POP);

    if (is_of) {
        c->cur_try_depth--;
        patch_jump(c, fin_patch);
        emit_op(c, OP_ENTER_FINALLY);
        emit_get_lex(c, 0, act_iter_slot);
        uint32_t j_skip_close = emit_jump(c, OP_JMP_UNDEF);
        emit_get_lex(c, 0, act_iter_slot);
        if (is_await) {
            uint32_t ret_nidx = add_str_cstr(c, "return");
            emit_op(c, OP_DUP);
            emit_op(c, OP_GET_PROP); emit_u32(c, ret_nidx);
            emit_op(c, OP_DUP);
            uint32_t j_no_ret_fn = emit_jump(c, OP_JMP_NULLISH);
            emit_op(c, OP_CALL_METHOD); emit_u16(c, 0);
            emit_op(c, OP_AWAIT);
            emit_op(c, OP_POP);
            uint32_t j_closed_async = emit_jump(c, OP_JMP);
            patch_jump(c, j_no_ret_fn);
            emit_op(c, OP_POP); emit_op(c, OP_POP);
            patch_jump(c, j_closed_async);
        } else {
            emit_op(c, OP_ITER_CLOSE);
        }
        patch_jump(c, j_skip_close);
        emit_op(c, OP_FINALLY_END);
    }

    uint32_t break_pc = c->bc->code_len;
    for (uint32_t i = 0; i < lctx.break_count; i++) {
        patch_u32(c, lctx.break_patches[i], break_pc);
    }
    for (uint32_t i = 0; i < lctx.cont_count; i++) {
        patch_u32(c, lctx.cont_patches[i], step_pc);
    }

    c->loop_top = lctx.prev;
    free(lctx.break_patches);
    free(lctx.cont_patches);

    emit_op(c, OP_POP_ENV);
    c->scope = iter_sc.parent;
    c->cur_env_depth--;
    comp_scope_cleanup(&iter_sc);

    if (j_nullish_for_in != UINT32_MAX) {
        uint32_t j_past_null = emit_jump(c, OP_JMP);
        patch_jump(c, j_nullish_for_in);
        emit_op(c, OP_POP);
        patch_jump(c, j_past_null);
    }
    return true;

fail_body:
    if (has_lex_left) {
        c->scope = body_lex_sc.parent;
        c->cur_env_depth--;
        comp_scope_cleanup(&body_lex_sc);
    }
    c->loop_top = lctx.prev;
    free(lctx.break_patches);
    free(lctx.cont_patches);
    c->scope = iter_sc.parent;
    c->cur_env_depth--;
    comp_scope_cleanup(&iter_sc);
    return false;

fail_tdz:
    if (has_lex_left) {
        c->scope = tdz_sc.parent;
        c->cur_env_depth--;
        comp_scope_cleanup(&tdz_sc);
    }
    return false;
}

static bool compile_stmt_with_label(AjsCompiler *c, AjsAstNode *n, const uint16_t *lbl_u16, uint32_t lbl_len) {
    if (!n || c->had_error) return !c->had_error;
    emit_line_pos(c, n->line, n->col);

    switch (n->kind) {
        case AST_EMPTY_STMT:
        case AST_DEBUGGER_STMT:
        case AST_IMPORT_DECL:
        case AST_EXPORT_ALL_DECL:
            return true;

        case AST_EXPR_STMT:
            if (!compile_expr(c, n->u.expr_stmt.expr)) return false;
            if (c->eval_ret_slot != AJS_NO_SLOT) {
                emit_set_lex(c, c->cur_env_depth, c->eval_ret_slot, 0);
            }
            emit_op(c, OP_POP);
            return true;

        case AST_BLOCK: {
            AjsCompScope bsc;
            memset(&bsc, 0, sizeof(bsc));
            bsc.parent = c->scope;
            collect_block_lex_in_list(c, &bsc, &n->u.block.stmts, false);
            bool pushed = (bsc.slot_count > 0);
            uint32_t blk_disp_fin = UINT32_MAX;
            if (pushed) {
                bsc.has_runtime_env = true;
                c->cur_env_depth++;
                bsc.env_depth_idx = c->cur_env_depth;
                c->scope = &bsc;
                uint32_t ed_idx = build_env_desc(c, &bsc);
                emit_push_env(c, bsc.slot_count, ed_idx);
                if (!instantiate_hoisted_funcs(c, &bsc, &n->u.block.stmts, false)) {
                    c->scope = bsc.parent;
                    c->cur_env_depth--;
                    comp_scope_cleanup(&bsc);
                    return false;
                }
                if (bsc.has_disp_stack) {
                    blk_disp_fin = emit_dispose_enter(c, 0, bsc.disp_slot);
                }
            }
            for (uint32_t i = 0; i < n->u.block.stmts.count; i++) {
                if (!compile_stmt(c, n->u.block.stmts.items[i])) {
                    if (pushed) {
                        if (bsc.has_disp_stack) c->cur_try_depth--;
                        c->scope = bsc.parent;
                        c->cur_env_depth--;
                        comp_scope_cleanup(&bsc);
                    }
                    return false;
                }
            }
            if (pushed) {
                if (bsc.has_disp_stack) {
                    emit_dispose_finally(c, 0, bsc.disp_slot, blk_disp_fin);
                }
                emit_op(c, OP_POP_ENV);
                c->scope = bsc.parent;
                c->cur_env_depth--;
                comp_scope_cleanup(&bsc);
            }
            return true;
        }

        case AST_VAR_DECL:
            return compile_var_decl_stmt(c, n);

        case AST_FUNC_DECL:
            if (!c->strict && !c->scope->is_base && n->u.func.fn_kind == AJS_FN_BC_NORMAL && n->u.func.name_u16) {
                int32_t blk_idx = comp_scope_find_own(c->scope, n->u.func.name_u16, n->u.func.name_len, false);
                if (blk_idx >= 0) {
                    int32_t base_idx = comp_scope_find_own(&c->base_scope, n->u.func.name_u16, n->u.func.name_len, false);
                    if (base_idx >= 0) {
                        emit_get_lex(c, 0, c->scope->locals[blk_idx].slot);
                        emit_set_lex(c, c->cur_env_depth, c->base_scope.locals[base_idx].slot, 0);
                        emit_op(c, OP_POP);
                    } else if (c->is_script_global) {
                        emit_get_lex(c, 0, c->scope->locals[blk_idx].slot);
                        uint32_t nidx = add_str_u16(c, n->u.func.name_u16, n->u.func.name_len);
                        emit_op(c, OP_SET_GLOBAL); emit_u32(c, nidx); emit_u8(c, 0);
                        emit_op(c, OP_POP);
                    }
                }
            }
            if (!c->strict && n->u.func.name_u16 && comp_scope_find_own(c->scope, n->u.func.name_u16, n->u.func.name_len, false) < 0) {
                AjsBytecode *fn_bc = compile_func_node(c, n, NULL, false);
                if (!fn_bc || c->had_error) return false;
                emit_op(c, OP_CLOSURE); emit_u32(c, add_const(c, ajs_val_from_gc(fn_bc)));
                emit_store_ident(c, n->u.func.name_u16, n->u.func.name_len, false, AJS_VAR_VAR);
                emit_op(c, OP_POP);
            }
            return true;

        case AST_CLASS_DECL:
            if (!compile_class_node(c, n, NULL, true)) return false;
            emit_store_ident(c, n->u.klass.name_u16, n->u.klass.name_len, true, AJS_VAR_LET);
            emit_op(c, OP_POP);
            return true;

        case AST_IF_STMT: {
            emit_reset_eval_ret(c);
            if (!compile_expr(c, n->u.if_stmt.test)) return false;
            uint32_t j_false = emit_jump(c, OP_JMP_FALSE);
            if (!compile_stmt(c, n->u.if_stmt.conseq)) return false;
            if (n->u.if_stmt.alt) {
                uint32_t j_end = emit_jump(c, OP_JMP);
                patch_jump(c, j_false);
                if (!compile_stmt(c, n->u.if_stmt.alt)) return false;
                patch_jump(c, j_end);
            } else {
                patch_jump(c, j_false);
            }
            return true;
        }

        case AST_WHILE_STMT: {
            emit_reset_eval_ret(c);
            AjsLoopCtx lctx;
            memset(&lctx, 0, sizeof(lctx));
            lctx.prev = c->loop_top;
            lctx.label_u16 = lbl_u16;
            lctx.label_len = lbl_len;
            lctx.try_depth = c->cur_try_depth;
            lctx.env_depth = c->cur_env_depth;
            lctx.cont_try_depth = c->cur_try_depth;
            lctx.cont_env_depth = c->cur_env_depth;
            uint32_t top_pc = c->bc->code_len;
            lctx.cont_pc = top_pc;
            c->loop_top = &lctx;

            if (!compile_expr(c, n->u.loop.test)) goto fail_while;
            uint32_t j_exit = emit_jump(c, OP_JMP_FALSE);
            if (!compile_stmt(c, n->u.loop.body)) goto fail_while;
            emit_op(c, OP_JMP); emit_u32(c, top_pc);

            uint32_t brk_pc = c->bc->code_len;
            patch_u32(c, j_exit, brk_pc);
            for (uint32_t i = 0; i < lctx.break_count; i++) patch_u32(c, lctx.break_patches[i], brk_pc);
            for (uint32_t i = 0; i < lctx.cont_count; i++) patch_u32(c, lctx.cont_patches[i], top_pc);

            c->loop_top = lctx.prev;
            free(lctx.break_patches); free(lctx.cont_patches);
            return true;
        fail_while:
            c->loop_top = lctx.prev;
            free(lctx.break_patches); free(lctx.cont_patches);
            return false;
        }

        case AST_DO_WHILE_STMT: {
            emit_reset_eval_ret(c);
            AjsLoopCtx lctx;
            memset(&lctx, 0, sizeof(lctx));
            lctx.prev = c->loop_top;
            lctx.label_u16 = lbl_u16;
            lctx.label_len = lbl_len;
            lctx.try_depth = c->cur_try_depth;
            lctx.env_depth = c->cur_env_depth;
            lctx.cont_try_depth = c->cur_try_depth;
            lctx.cont_env_depth = c->cur_env_depth;
            lctx.cont_pc = UINT32_MAX;
            c->loop_top = &lctx;

            uint32_t top_pc = c->bc->code_len;
            if (!compile_stmt(c, n->u.loop.body)) goto fail_do;

            uint32_t cont_pc = c->bc->code_len;
            for (uint32_t i = 0; i < lctx.cont_count; i++) patch_u32(c, lctx.cont_patches[i], cont_pc);

            if (!compile_expr(c, n->u.loop.test)) goto fail_do;
            emit_op(c, OP_JMP_TRUE); emit_u32(c, top_pc);

            uint32_t brk_pc = c->bc->code_len;
            for (uint32_t i = 0; i < lctx.break_count; i++) patch_u32(c, lctx.break_patches[i], brk_pc);

            c->loop_top = lctx.prev;
            free(lctx.break_patches); free(lctx.cont_patches);
            return true;
        fail_do:
            c->loop_top = lctx.prev;
            free(lctx.break_patches); free(lctx.cont_patches);
            return false;
        }

        case AST_FOR_STMT:
            return compile_for_stmt(c, n, lbl_u16, lbl_len);

        case AST_FOR_IN_OF_STMT:
            return compile_for_in_of_stmt(c, n, lbl_u16, lbl_len);

        case AST_SWITCH_STMT: {
            emit_reset_eval_ret(c);
            if (!compile_expr(c, n->u.switch_stmt.discriminant)) return false;

            AjsCompScope sw_sc;
            memset(&sw_sc, 0, sizeof(sw_sc));
            sw_sc.parent = c->scope;
            sw_sc.has_runtime_env = true;
            uint32_t disc_slot = comp_scope_add(c, &sw_sc, NULL, 0, BIND_VAR, false);
            for (uint32_t i = 0; i < n->u.switch_stmt.cases.count; i++) {
                AjsAstNode *sc = n->u.switch_stmt.cases.items[i];
                collect_block_lex_in_list(c, &sw_sc, &sc->u.switch_case.conseq, false);
            }

            c->cur_env_depth++;
            sw_sc.env_depth_idx = c->cur_env_depth;
            c->scope = &sw_sc;
            uint32_t ed_idx = build_env_desc(c, &sw_sc);
            emit_push_env(c, sw_sc.slot_count, ed_idx);

            emit_init_lex(c, 0, disc_slot); emit_op(c, OP_POP);

            for (uint32_t i = 0; i < n->u.switch_stmt.cases.count; i++) {
                AjsAstNode *sc = n->u.switch_stmt.cases.items[i];
                if (!instantiate_hoisted_funcs(c, &sw_sc, &sc->u.switch_case.conseq, false)) {
                    c->scope = sw_sc.parent; c->cur_env_depth--; comp_scope_cleanup(&sw_sc);
                    return false;
                }
            }

            uint32_t sw_disp_fin = UINT32_MAX;
            if (sw_sc.has_disp_stack) {
                sw_disp_fin = emit_dispose_enter(c, 0, sw_sc.disp_slot);
            }

            AjsLoopCtx lctx;
            memset(&lctx, 0, sizeof(lctx));
            lctx.prev = c->loop_top;
            lctx.is_switch = true;
            lctx.try_depth = c->cur_try_depth;
            lctx.env_depth = c->cur_env_depth;
            c->loop_top = &lctx;

            uint32_t case_cnt = n->u.switch_stmt.cases.count;
            uint32_t *case_jumps = case_cnt ? (uint32_t *)malloc(case_cnt * sizeof(uint32_t)) : NULL;
            int32_t default_idx = -1;

            for (uint32_t i = 0; i < case_cnt; i++) {
                AjsAstNode *sc = n->u.switch_stmt.cases.items[i];
                if (!sc->u.switch_case.test) {
                    default_idx = (int32_t)i;
                    case_jumps[i] = UINT32_MAX;
                    continue;
                }
                emit_get_lex(c, 0, disc_slot);
                if (!compile_expr(c, sc->u.switch_case.test)) goto fail_sw;
                emit_op(c, OP_STRICT_EQ);
                case_jumps[i] = emit_jump(c, OP_JMP_TRUE);
            }
            uint32_t j_to_def_or_end = emit_jump(c, OP_JMP);

            for (uint32_t i = 0; i < case_cnt; i++) {
                AjsAstNode *sc = n->u.switch_stmt.cases.items[i];
                if (case_jumps[i] != UINT32_MAX) patch_jump(c, case_jumps[i]);
                if ((int32_t)i == default_idx) patch_jump(c, j_to_def_or_end);
                for (uint32_t j = 0; j < sc->u.switch_case.conseq.count; j++) {
                    if (!compile_stmt(c, sc->u.switch_case.conseq.items[j])) goto fail_sw;
                }
            }

            uint32_t end_pc = c->bc->code_len;
            if (default_idx < 0) patch_u32(c, j_to_def_or_end, end_pc);
            for (uint32_t i = 0; i < lctx.break_count; i++) patch_u32(c, lctx.break_patches[i], end_pc);

            c->loop_top = lctx.prev;
            free(case_jumps);
            free(lctx.break_patches);

            if (sw_sc.has_disp_stack) {
                emit_dispose_finally(c, 0, sw_sc.disp_slot, sw_disp_fin);
            }
            emit_op(c, OP_POP_ENV);
            c->scope = sw_sc.parent;
            c->cur_env_depth--;
            comp_scope_cleanup(&sw_sc);
            return true;

        fail_sw:
            c->loop_top = lctx.prev;
            free(case_jumps);
            free(lctx.break_patches);
            c->scope = sw_sc.parent;
            c->cur_env_depth--;
            comp_scope_cleanup(&sw_sc);
            return false;
        }

        case AST_LABELED_STMT: {
            AjsAstNode *body = n->u.labeled.body;
            if (body && (body->kind == AST_FOR_STMT || body->kind == AST_FOR_IN_OF_STMT ||
                         body->kind == AST_WHILE_STMT || body->kind == AST_DO_WHILE_STMT)) {
                return compile_stmt_with_label(c, body, n->u.labeled.label_u16, n->u.labeled.label_len);
            }
            /* Chained loop labels `L1: L2: while(...)` or non-loop labeled statement `L1: { break L1; }` */
            AjsLoopCtx lctx;
            memset(&lctx, 0, sizeof(lctx));
            lctx.prev = c->loop_top;
            lctx.is_label_only = true;
            lctx.label_u16 = n->u.labeled.label_u16;
            lctx.label_len = n->u.labeled.label_len;
            lctx.try_depth = c->cur_try_depth;
            lctx.env_depth = c->cur_env_depth;
            lctx.cont_try_depth = c->cur_try_depth;
            lctx.cont_env_depth = c->cur_env_depth;
            lctx.cont_pc = UINT32_MAX;
            c->loop_top = &lctx;

            bool ok = compile_stmt(c, body);

            uint32_t brk_pc = c->bc->code_len;
            for (uint32_t i = 0; i < lctx.break_count; i++) patch_u32(c, lctx.break_patches[i], brk_pc);
            c->loop_top = lctx.prev;
            free(lctx.break_patches);
            free(lctx.cont_patches);
            return ok;
        }

        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT: {
            bool is_brk = (n->kind == AST_BREAK_STMT);
            const uint16_t *lu = n->u.jump.label_u16;
            uint32_t ll = n->u.jump.label_len;
            AjsLoopCtx *target = NULL;
            for (AjsLoopCtx *lc = c->loop_top; lc; lc = lc->prev) {
                if (lu) {
                    if (lc->label_u16 && c_u16_eq(lc->label_u16, lc->label_len, lu, ll)) {
                        /* For chained loop labels `L1: L2: for(...)`, `continue L1` targets the inner loop! */
                        if (!is_brk && lc->is_label_only) {
                            /* Find the enclosed loop directly below in source? Wait: `lc->prev` is enclosing,
                             * so for `L1: L2: for(...)`, when inside `for`, `loop_top` is `for` (`L2`), and `for->prev` is `L1`!
                             * Let's walk from `c->loop_top` down to `lc` to find the loop just inside `lc`! */
                            AjsLoopCtx *inner = c->loop_top;
                            while (inner && inner->prev != lc) inner = inner->prev;
                            while (inner && inner->is_label_only) {
                                AjsLoopCtx *nxt = c->loop_top;
                                while (nxt && nxt->prev != inner) nxt = nxt->prev;
                                inner = nxt;
                            }
                            if (inner) target = inner;
                        } else {
                            target = lc;
                        }
                        break;
                    }
                } else {
                    if (is_brk && !lc->is_label_only) { target = lc; break; }
                    if (!is_brk && !lc->is_switch && !lc->is_label_only) { target = lc; break; }
                }
            }
            if (!target) {
                comp_error(c, n->line, n->col, "Unresolved jump target");
                return false;
            }
            uint16_t tgt_try = is_brk ? target->try_depth : target->cont_try_depth;
            uint16_t tgt_env = is_brk ? target->env_depth : target->cont_env_depth;
            emit_op(c, OP_UNWIND_JUMP);
            emit_u16(c, tgt_try);
            emit_u16(c, tgt_env);
            uint32_t poff = c->bc->code_len;
            if (!is_brk && target->cont_pc != UINT32_MAX) {
                emit_u32(c, target->cont_pc);
            } else {
                emit_u32(c, 0);
                if (is_brk) loop_push_patch(&target->break_patches, &target->break_count, &target->break_cap, poff);
                else loop_push_patch(&target->cont_patches, &target->cont_count, &target->cont_cap, poff);
            }
            return true;
        }

        case AST_RETURN_STMT: {
            if (n->u.expr_stmt.expr) {
                if (!compile_expr(c, n->u.expr_stmt.expr)) return false;
            } else {
                emit_op(c, OP_UNDEF);
            }
            if (c->bc->fn_kind == AJS_FN_BC_ASYNC_GENERATOR && n->u.expr_stmt.expr) {
                emit_op(c, OP_AWAIT);
            }
            if (c->cur_try_depth > 0) {
                emit_op(c, OP_UNWIND_RETURN);
            } else if (c->bc->is_derived_ctor) {
                emit_op(c, OP_RETURN_DERIVED);
                emit_u32(c, c->bc->this_slot);
            } else {
                emit_op(c, OP_RETURN);
            }
            return true;
        }

        case AST_THROW_STMT:
            if (!compile_expr(c, n->u.expr_stmt.expr)) return false;
            emit_op(c, OP_THROW);
            return true;

        case AST_TRY_STMT: {
            emit_reset_eval_ret(c);
            bool has_catch = (n->u.try_stmt.catch_body != NULL);
            bool has_finally = (n->u.try_stmt.finally_body != NULL);

            emit_op(c, OP_TRY_PUSH);
            uint32_t catch_patch = c->bc->code_len;
            emit_u32(c, UINT32_MAX);
            uint32_t fin_patch = c->bc->code_len;
            emit_u32(c, UINT32_MAX);
            c->cur_try_depth++;

            if (!compile_stmt(c, n->u.try_stmt.block)) {
                c->cur_try_depth--;
                return false;
            }

            uint32_t j_after_try = UINT32_MAX;
            if (has_catch && !has_finally) {
                emit_op(c, OP_TRY_POP);
                c->cur_try_depth--;
                j_after_try = emit_jump(c, OP_JMP);
            } else if (has_catch && has_finally) {
                j_after_try = emit_jump(c, OP_JMP);
            }

            if (has_catch) {
                patch_jump(c, catch_patch);
                AjsCompScope csc;
                memset(&csc, 0, sizeof(csc));
                csc.parent = c->scope;
                if (n->u.try_stmt.catch_param) {
                    collect_pattern_vars(c, &csc, n->u.try_stmt.catch_param, BIND_LET, false);
                }
                collect_block_lex_in_list(c, &csc, &n->u.try_stmt.catch_body->u.block.stmts, false);

                c->cur_env_depth++;
                csc.has_runtime_env = true;
                csc.env_depth_idx = c->cur_env_depth;
                c->scope = &csc;
                uint32_t ed_idx = build_env_desc(c, &csc);
                emit_push_env(c, csc.slot_count, ed_idx);

                if (n->u.try_stmt.catch_param) {
                    if (!compile_pattern_assign(c, n->u.try_stmt.catch_param, true, AJS_VAR_LET)) {
                        c->scope = csc.parent; c->cur_env_depth--; comp_scope_cleanup(&csc);
                        if (has_finally) c->cur_try_depth--;
                        return false;
                    }
                } else {
                    emit_op(c, OP_POP);
                }

                if (!instantiate_hoisted_funcs(c, &csc, &n->u.try_stmt.catch_body->u.block.stmts, false)) {
                    c->scope = csc.parent; c->cur_env_depth--; comp_scope_cleanup(&csc);
                    if (has_finally) c->cur_try_depth--;
                    return false;
                }
                uint32_t catch_disp_fin = UINT32_MAX;
                if (csc.has_disp_stack) {
                    catch_disp_fin = emit_dispose_enter(c, 0, csc.disp_slot);
                }
                for (uint32_t i = 0; i < n->u.try_stmt.catch_body->u.block.stmts.count; i++) {
                    if (!compile_stmt(c, n->u.try_stmt.catch_body->u.block.stmts.items[i])) {
                        c->scope = csc.parent; c->cur_env_depth--; comp_scope_cleanup(&csc);
                        if (csc.has_disp_stack) c->cur_try_depth--;
                        if (has_finally) c->cur_try_depth--;
                        return false;
                    }
                }
                if (csc.has_disp_stack) {
                    emit_dispose_finally(c, 0, csc.disp_slot, catch_disp_fin);
                }
                emit_op(c, OP_POP_ENV);
                c->scope = csc.parent;
                c->cur_env_depth--;
                comp_scope_cleanup(&csc);
            }

            if (has_finally) {
                uint32_t saved_eval_slot = AJS_NO_SLOT;
                if (j_after_try != UINT32_MAX) patch_jump(c, j_after_try);
                c->cur_try_depth--;
                patch_jump(c, fin_patch);
                emit_op(c, OP_ENTER_FINALLY);
                if (c->eval_ret_slot != AJS_NO_SLOT) {
                    saved_eval_slot = c->eval_ret_slot;
                    emit_push_env(c, 1, AJS_NO_SLOT);
                    c->cur_env_depth++;
                    emit_get_lex(c, c->cur_env_depth, saved_eval_slot);
                    emit_init_lex(c, 0, 0); emit_op(c, OP_POP);
                }
                if (!compile_stmt(c, n->u.try_stmt.finally_body)) {
                    if (saved_eval_slot != AJS_NO_SLOT) c->cur_env_depth--;
                    return false;
                }
                if (saved_eval_slot != AJS_NO_SLOT) {
                    emit_get_lex(c, 0, 0);
                    emit_set_lex(c, c->cur_env_depth, saved_eval_slot, 0);
                    emit_op(c, OP_POP);
                    emit_op(c, OP_POP_ENV);
                    c->cur_env_depth--;
                }
                emit_op(c, OP_FINALLY_END);
            } else {
                patch_jump(c, j_after_try);
            }
            return true;
        }

        case AST_WITH_STMT: {
            emit_reset_eval_ret(c);
            if (!compile_expr(c, n->u.with_stmt.obj)) return false;
            emit_op(c, OP_PUSH_WITH);
            c->cur_env_depth++;
            c->with_depth++;
            c->has_eval_or_with = true;
            bool ok = compile_stmt(c, n->u.with_stmt.body);
            c->with_depth--;
            c->cur_env_depth--;
            emit_op(c, OP_POP_ENV);
            return ok;
        }

        case AST_EXPORT_NAMED_DECL:
            if (n->u.mod_decl.decl) {
                return compile_stmt(c, n->u.mod_decl.decl);
            }
            return true;

        case AST_EXPORT_DEFAULT_DECL: {
            AjsAstNode *decl = n->u.mod_decl.decl;
            if (!decl) return true;
            if (decl->kind == AST_FUNC_DECL) {
                return true;
            }
            static const uint16_t u_def_star[9] = {'*','d','e','f','a','u','l','t','*'};
            if (decl->kind == AST_CLASS_DECL) {
                AjsString *def_str = c->vm->str_default;
                if (!compile_class_node(c, decl, def_str, true)) return false;
                if (decl->u.klass.name_u16) {
                    emit_store_ident(c, decl->u.klass.name_u16, decl->u.klass.name_len, true, AJS_VAR_LET);
                } else {
                    emit_store_ident(c, u_def_star, 9, true, AJS_VAR_LET);
                }
                emit_op(c, OP_POP);
                return true;
            }
            if (!compile_expr(c, decl)) return false;
            if (is_anon_fn_or_class_def(decl)) {
                emit_op(c, OP_CONST);
                emit_u32(c, add_const(c, ajs_val_from_gc(c->vm->str_default)));
                emit_op(c, OP_SET_FN_NAME);
                emit_u8(c, 0);
            }
            emit_store_ident(c, u_def_star, 9, true, AJS_VAR_LET);
            emit_op(c, OP_POP);
            return true;
        }

        default:
            break;
    }

    comp_error(c, n->line, n->col, "Unsupported statement AST kind %d", (int)n->kind);
    return false;
}

static bool compile_stmt(AjsCompiler *c, AjsAstNode *n) {
    return compile_stmt_with_label(c, n, NULL, 0);
}

/* ============================================================================
 * 9. Public Compiler Entry Points (Script, Eval, Dynamic Function, ES Module)
 * ============================================================================ */

static void fill_comp_err(const AjsCompiler *c, const char *url, AjsErrorInfo *out_err) {
    if (!out_err) return;
    memset(out_err, 0, sizeof(*out_err));
    out_err->phase = AJS_ERR_PARSE;
    snprintf(out_err->name, sizeof(out_err->name), "SyntaxError");
    snprintf(out_err->message, sizeof(out_err->message), "%s", c->err_msg);
    snprintf(out_err->url, sizeof(out_err->url), "%s", url ? url : "<script>");
    out_err->line = c->err_line;
    out_err->column = c->err_col;
    snprintf(out_err->stack, sizeof(out_err->stack), "SyntaxError: %s\n    at %s:%u:%u",
             c->err_msg, out_err->url, c->err_line, c->err_col);
}

AjsBytecode *ajs_compile_script(AjsVm *vm, AjsRealm *realm, const char *utf8_code, size_t code_len,
                                const char *source_url, bool force_strict, bool is_eval,
                                AjsEnv *eval_lex_env, AjsErrorInfo *out_err) {
    AjsAstArena arena = {0};
    bool allow_nt = false, allow_sp = false, allow_sc = false;
    if (is_eval && eval_lex_env && vm->call_depth > 0) {
        for (ssize_t i = (ssize_t)vm->call_depth - 1; i >= 0; i--) {
            AjsExecFrame *f = vm->call_stack[i].frame;
            if (!f || !f->bc) continue;
            AjsFuncKind fk = f->bc->fn_kind;
            if (fk == AJS_FN_BC_ARROW || fk == AJS_FN_BC_ASYNC_ARROW) continue;
            if (!f->bc->is_eval && !f->bc->is_module && f->func_obj) {
                allow_nt = true;
                if (fk == AJS_FN_BC_METHOD || fk == AJS_FN_BC_CLASS_CTOR ||
                    f->func_obj->u.fn.home_object != AJS_VAL_UNDEFINED) {
                    allow_sp = true;
                }
                if (fk == AJS_FN_BC_CLASS_CTOR && f->bc->is_derived_ctor) {
                    allow_sc = true;
                }
            }
            break;
        }
    }

    vm->gc_suppress++;
    AjsAstNode *prog = ajs_parse_program(vm, &arena, utf8_code, code_len, source_url,
                                         false, force_strict, allow_nt, allow_sp, allow_sc, out_err);
    if (!prog) {
        ajs_arena_free(&arena);
        vm->gc_suppress--;
        return NULL;
    }

    size_t rs = ajs_temp_root_save(vm);
    bool strict = prog->u.program.strict;
    bool to_global = (!is_eval) || (!strict && eval_lex_env == NULL);
    if (is_eval && !strict && eval_lex_env != NULL) {
        to_global = (eval_lex_env == realm->global_lex_env);
    }

    AjsCompiler c;
    init_compiler(&c, vm, realm, NULL, AJS_FN_BC_NORMAL, strict, source_url);
    c.src_base = utf8_code;
    c.src_buf = ajs_source_buf_new(utf8_code, code_len);
    c.bc->is_eval = is_eval;
    c.bc->name = ajs_str_from_cstr(vm, is_eval ? "<eval>" : "<script>", true);
    c.is_script_global = to_global;
    if (is_eval && eval_lex_env != NULL) {
        c.has_eval_or_with = true;
    }

    AjsFnScanInfo scan;
    memset(&scan, 0, sizeof(scan));
    scan_fn_ast(prog, false, &scan);
    if (scan.has_eval_or_with) c.has_eval_or_with = true;

    c.eval_ret_slot = comp_scope_add(&c, &c.base_scope, NULL, 0, BIND_VAR, false);

    for (uint32_t i = 0; i < prog->u.program.body.count; i++) {
        hoist_vars_in_stmt(&c, prog->u.program.body.items[i], to_global);
    }
    collect_block_lex_in_list(&c, &c.base_scope, &prog->u.program.body, !is_eval);

    if (!instantiate_hoisted_funcs(&c, &c.base_scope, &prog->u.program.body, to_global)) goto fail;

    {
        uint32_t scr_disp_fin = UINT32_MAX;
        if (c.base_scope.has_disp_stack) {
            scr_disp_fin = emit_dispose_enter(&c, 0, c.base_scope.disp_slot);
        }
        for (uint32_t i = 0; i < prog->u.program.body.count; i++) {
            if (!compile_stmt(&c, prog->u.program.body.items[i])) goto fail;
        }
        if (c.base_scope.has_disp_stack) {
            emit_dispose_finally(&c, 0, c.base_scope.disp_slot, scr_disp_fin);
        }
    }

    emit_get_lex(&c, 0, c.eval_ret_slot);
    emit_op(&c, OP_RETURN);

    ajs_source_buf_release(c.src_buf);
    finalize_base_scope_metadata(&c);
    ajs_arena_free(&arena);
    vm->gc_suppress--;
    if (vm->gc_suppress == 0 && vm->gc_bytes > vm->gc_threshold) {
        ajs_gc_collect(vm);
    }
    ajs_temp_root_restore(vm, rs);
    return c.bc;

fail:
    fill_comp_err(&c, source_url, out_err);
    ajs_source_buf_release(c.src_buf);
    cleanup_compiler(&c);
    ajs_arena_free(&arena);
    ajs_temp_root_restore(vm, rs);
    vm->gc_suppress--;
    return NULL;
}

AjsBytecode *ajs_compile_function_dynamic(AjsVm *vm, AjsRealm *realm, const char *params_utf8,
                                          size_t params_len, const char *body_utf8, size_t body_len,
                                          AjsFuncKind fn_kind) {
    {
        size_t pchk_len = params_len + 32u;
        char *pchk = (char *)malloc(pchk_len);
        if (!pchk) return NULL;
        const char *hdr = "(function(";
        if (fn_kind == AJS_FN_BC_GENERATOR) hdr = "(function*(";
        else if (fn_kind == AJS_FN_BC_ASYNC) hdr = "(async function(";
        else if (fn_kind == AJS_FN_BC_ASYNC_GENERATOR) hdr = "(async function*(";
        size_t hlen = strlen(hdr);
        ajs_memcpy(pchk, hdr, hlen);
        ajs_memcpy(pchk + hlen, params_utf8, params_len);
        ajs_memcpy(pchk + hlen + params_len, "\n){})", 6);
        AjsAstArena pa = {0};
        AjsErrorInfo perr;
        AjsAstNode *pprog = ajs_parse_program(vm, &pa, pchk, hlen + params_len + 5u,
                                              "<anonymous>", false, false, false, false, false, &perr);
        free(pchk);
        ajs_arena_free(&pa);
        if (!pprog) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "%s", perr.message);
            return NULL;
        }
    }

    const char *prefix = "(function anonymous(";
    if (fn_kind == AJS_FN_BC_GENERATOR) prefix = "(function* anonymous(";
    else if (fn_kind == AJS_FN_BC_ASYNC) prefix = "(async function anonymous(";
    else if (fn_kind == AJS_FN_BC_ASYNC_GENERATOR) prefix = "(async function* anonymous(";

    size_t plen = strlen(prefix);
    const char *mid = "\n) {\n";
    size_t mlen = strlen(mid);
    const char *suffix = "\n})";
    size_t slen = strlen(suffix);

    size_t total = plen + params_len + mlen + body_len + slen;
    char *full = (char *)malloc(total + 1u);
    if (!full) return NULL;
    size_t pos = 0;
    ajs_memcpy(full + pos, prefix, plen); pos += plen;
    ajs_memcpy(full + pos, params_utf8, params_len); pos += params_len;
    ajs_memcpy(full + pos, mid, mlen); pos += mlen;
    ajs_memcpy(full + pos, body_utf8, body_len); pos += body_len;
    ajs_memcpy(full + pos, suffix, slen); pos += slen;
    full[pos] = '\0';

    AjsAstArena arena = {0};
    AjsErrorInfo err;
    AjsAstNode *prog = ajs_parse_program(vm, &arena, full, pos, "<anonymous>",
                                         false, false, false, false, false, &err);
    if (!prog || prog->u.program.body.count != 1) {
        free(full);
        ajs_arena_free(&arena);
        ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "%s", prog ? "Unexpected token after function body" : err.message);
        return NULL;
    }

    AjsAstNode *es = prog->u.program.body.items[0];
    if (!es || es->kind != AST_EXPR_STMT || !es->u.expr_stmt.expr || es->u.expr_stmt.expr->kind != AST_FUNC_EXPR) {
        free(full);
        ajs_arena_free(&arena);
        ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Invalid dynamic function");
        return NULL;
    }

    AjsAstNode *fn_node = es->u.expr_stmt.expr;
    fn_node->u.func.name_u16 = NULL;
    fn_node->u.func.name_len = 0;

    size_t rs = ajs_temp_root_save(vm);
    vm->gc_suppress++;
    AjsCompiler dummy_parent;
    init_compiler(&dummy_parent, vm, realm, NULL, AJS_FN_BC_NORMAL, false, "<anonymous>");
    dummy_parent.src_base = full;
    dummy_parent.src_buf = ajs_source_buf_new(full, pos);
    AjsBytecode *fn_bc = compile_func_node(&dummy_parent, fn_node, "anonymous", false);
    ajs_source_buf_release(dummy_parent.src_buf);
    cleanup_compiler(&dummy_parent);
    free(full);
    ajs_arena_free(&arena);
    ajs_temp_root_restore(vm, rs);
    vm->gc_suppress--;

    if (!fn_bc) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "%s", dummy_parent.err_msg);
        return NULL;
    }
    return fn_bc;
}

static void mod_add_requested_spec(AjsModuleRecord *mod, AjsString *spec) {
    for (uint32_t i = 0; i < mod->req_count; i++) {
        if (ajs_str_eq(mod->requested_specifiers[i], spec)) return;
    }
    mod->requested_specifiers = (AjsString **)realloc(
        mod->requested_specifiers, (mod->req_count + 1u) * sizeof(AjsString *));
    mod->requested_specifiers[mod->req_count++] = spec;
}

static void mod_add_import_entry(AjsModuleRecord *mod, AjsString *spec, AjsString *imp_name,
                                 AjsString *loc_name, uint32_t slot, uint32_t line, uint32_t col) {
    mod->imports = (AjsImportEntry *)realloc(mod->imports, (mod->import_count + 1u) * sizeof(AjsImportEntry));
    AjsImportEntry *ie = &mod->imports[mod->import_count++];
    ie->module_specifier = spec;
    ie->import_name = imp_name;
    ie->local_name = loc_name;
    ie->local_slot = slot;
    ie->line = line;
    ie->col = col;
}

static void mod_add_export_entry(AjsModuleRecord *mod, AjsString *exp_name, AjsString *spec,
                                 AjsString *imp_name, AjsString *loc_name, uint32_t slot,
                                 uint32_t line, uint32_t col) {
    mod->exports = (AjsExportEntry *)realloc(mod->exports, (mod->export_count + 1u) * sizeof(AjsExportEntry));
    AjsExportEntry *ee = &mod->exports[mod->export_count++];
    ee->export_name = exp_name;
    ee->module_specifier = spec;
    ee->import_name = imp_name;
    ee->local_name = loc_name;
    ee->local_slot = slot;
    ee->line = line;
    ee->col = col;
}

static void mod_export_pattern_bindings(AjsVm *vm, AjsCompiler *c, AjsModuleRecord *mod, AjsAstNode *pat) {
    if (!pat) return;
    switch (pat->kind) {
        case AST_IDENT: {
            AjsString *nm = ajs_str_from_utf16(vm, pat->u.ident.u16, pat->u.ident.len, true);
            int32_t idx = comp_scope_find_own(&c->base_scope, pat->u.ident.u16, pat->u.ident.len, false);
            uint32_t slot = idx >= 0 ? c->base_scope.locals[idx].slot : AJS_NO_SLOT;
            mod_add_export_entry(mod, nm, NULL, NULL, nm, slot, pat->line, pat->col);
            break;
        }
        case AST_ASSIGN_PATTERN:
            mod_export_pattern_bindings(vm, c, mod, pat->u.assign_pat.left);
            break;
        case AST_REST_ELEMENT:
            mod_export_pattern_bindings(vm, c, mod, pat->u.expr_stmt.expr);
            break;
        case AST_ARRAY_PATTERN:
            for (uint32_t i = 0; i < pat->u.array.elems.count; i++)
                mod_export_pattern_bindings(vm, c, mod, pat->u.array.elems.items[i]);
            break;
        case AST_OBJECT_PATTERN:
            for (uint32_t i = 0; i < pat->u.object.props.count; i++) {
                AjsAstNode *p = pat->u.object.props.items[i];
                if (!p) continue;
                if (p->kind == AST_REST_ELEMENT)
                    mod_export_pattern_bindings(vm, c, mod, p->u.expr_stmt.expr);
                else
                    mod_export_pattern_bindings(vm, c, mod, p->u.prop.value);
            }
            break;
        default:
            break;
    }
}

bool ajs_parse_and_compile_module(AjsVm *vm, AjsRealm *realm, AjsModuleRecord *mod,
                                  const char *utf8_code, size_t code_len, AjsErrorInfo *out_err) {
    AjsAstArena arena = {0};
    vm->gc_suppress++;
    AjsAstNode *prog = ajs_parse_program(vm, &arena, utf8_code, code_len, mod->url,
                                         true, true, false, false, false, out_err);
    if (!prog) {
        ajs_arena_free(&arena);
        vm->gc_suppress--;
        return false;
    }

    size_t rs = ajs_temp_root_save(vm);
    AjsCompiler c;
    init_compiler(&c, vm, realm, NULL, prog->u.program.has_tla ? AJS_FN_BC_ASYNC : AJS_FN_BC_NORMAL, true, mod->url);
    c.src_base = utf8_code;
    c.src_buf = ajs_source_buf_new(utf8_code, code_len);
    c.mod = mod;
    c.bc->is_module = true;
    c.bc->name = ajs_str_from_cstr(vm, mod->url ? mod->url : "<module>", true);
    mod->bc = c.bc;
    mod->has_tla = prog->u.program.has_tla;

    AjsFnScanInfo scan;
    memset(&scan, 0, sizeof(scan));
    scan_fn_ast(prog, false, &scan);
    if (scan.has_arrow_or_super) {
        c.bc->this_slot = comp_scope_add(&c, &c.base_scope, NULL, 0, BIND_VAR, false);
    }

    AjsString *str_star = ajs_str_from_cstr(vm, "*", true);
    static const uint16_t u_def_star[9] = {'*','d','e','f','a','u','l','t','*'};

    for (uint32_t i = 0; i < prog->u.program.body.count; i++) {
        AjsAstNode *st = prog->u.program.body.items[i];
        if (!st || st->kind != AST_IMPORT_DECL) continue;
        AjsString *spec = ajs_str_from_utf16(vm, st->u.mod_decl.source_u16, st->u.mod_decl.source_len, true);
        mod_add_requested_spec(mod, spec);
        for (uint32_t j = 0; j < st->u.mod_decl.specifiers.count; j++) {
            AjsAstNode *sp = st->u.mod_decl.specifiers.items[j];
            uint32_t slot = comp_scope_add(&c, &c.base_scope, sp->u.spec.local_u16, sp->u.spec.local_len, BIND_IMPORT, false);
            AjsString *loc_str = ajs_str_from_utf16(vm, sp->u.spec.local_u16, sp->u.spec.local_len, true);
            AjsString *imp_str = sp->u.spec.is_namespace ? str_star :
                                 ajs_str_from_utf16(vm, sp->u.spec.imported_u16, sp->u.spec.imported_len, true);
            mod_add_import_entry(mod, spec, imp_str, loc_str, slot, sp->line, sp->col);
        }
    }

    for (uint32_t i = 0; i < prog->u.program.body.count; i++) {
        AjsAstNode *st = prog->u.program.body.items[i];
        if (!st) continue;
        hoist_vars_in_stmt(&c, st, false);
        if (st->kind == AST_EXPORT_DEFAULT_DECL && st->u.mod_decl.decl) {
            AjsAstNode *d = st->u.mod_decl.decl;
            if (d->kind == AST_FUNC_DECL) {
                if (d->u.func.name_u16) {
                    comp_scope_add(&c, &c.base_scope, d->u.func.name_u16, d->u.func.name_len, BIND_VAR, false);
                } else {
                    comp_scope_add(&c, &c.base_scope, u_def_star, 9, BIND_VAR, false);
                }
            } else if (d->kind == AST_CLASS_DECL && d->u.klass.name_u16) {
                comp_scope_add(&c, &c.base_scope, d->u.klass.name_u16, d->u.klass.name_len, BIND_LET, false);
            } else {
                comp_scope_add(&c, &c.base_scope, u_def_star, 9, BIND_LET, false);
            }
        }
    }
    collect_block_lex_in_list(&c, &c.base_scope, &prog->u.program.body, false);

    for (uint32_t i = 0; i < prog->u.program.body.count; i++) {
        AjsAstNode *st = prog->u.program.body.items[i];
        if (!st) continue;
        if (st->kind == AST_EXPORT_ALL_DECL) {
            AjsString *spec = ajs_str_from_utf16(vm, st->u.mod_decl.source_u16, st->u.mod_decl.source_len, true);
            mod_add_requested_spec(mod, spec);
            if (st->u.mod_decl.specifiers.count > 0) {
                AjsAstNode *sp = st->u.mod_decl.specifiers.items[0];
                AjsString *exp_str = ajs_str_from_utf16(vm, sp->u.spec.exported_u16, sp->u.spec.exported_len, true);
                mod_add_export_entry(mod, exp_str, spec, str_star, NULL, AJS_NO_SLOT, sp->line, sp->col);
            } else {
                mod_add_export_entry(mod, NULL, spec, str_star, NULL, AJS_NO_SLOT, st->line, st->col);
            }
        } else if (st->kind == AST_EXPORT_DEFAULT_DECL) {
            AjsAstNode *d = st->u.mod_decl.decl;
            const uint16_t *lu = u_def_star;
            uint32_t ll = 9;
            if (d && d->kind == AST_FUNC_DECL && d->u.func.name_u16) {
                lu = d->u.func.name_u16; ll = d->u.func.name_len;
            } else if (d && d->kind == AST_CLASS_DECL && d->u.klass.name_u16) {
                lu = d->u.klass.name_u16; ll = d->u.klass.name_len;
            }
            int32_t idx = comp_scope_find_own(&c.base_scope, lu, ll, false);
            uint32_t slot = idx >= 0 ? c.base_scope.locals[idx].slot : AJS_NO_SLOT;
            AjsString *loc_str = ajs_str_from_utf16(vm, lu, ll, true);
            mod_add_export_entry(mod, vm->str_default, NULL, NULL, loc_str, slot, st->line, st->col);
        } else if (st->kind == AST_EXPORT_NAMED_DECL) {
            if (st->u.mod_decl.source_u16) {
                AjsString *spec = ajs_str_from_utf16(vm, st->u.mod_decl.source_u16, st->u.mod_decl.source_len, true);
                mod_add_requested_spec(mod, spec);
                for (uint32_t j = 0; j < st->u.mod_decl.specifiers.count; j++) {
                    AjsAstNode *sp = st->u.mod_decl.specifiers.items[j];
                    AjsString *imp_str = ajs_str_from_utf16(vm, sp->u.spec.imported_u16, sp->u.spec.imported_len, true);
                    AjsString *exp_str = ajs_str_from_utf16(vm, sp->u.spec.exported_u16, sp->u.spec.exported_len, true);
                    mod_add_export_entry(mod, exp_str, spec, imp_str, NULL, AJS_NO_SLOT, sp->line, sp->col);
                }
            } else if (st->u.mod_decl.decl) {
                AjsAstNode *d = st->u.mod_decl.decl;
                if (d->kind == AST_VAR_DECL) {
                    for (uint32_t j = 0; j < d->u.var_decl.decls.count; j++) {
                        mod_export_pattern_bindings(vm, &c, mod, d->u.var_decl.decls.items[j]->u.declarator.id);
                    }
                } else if (d->kind == AST_FUNC_DECL && d->u.func.name_u16) {
                    AjsString *nm = ajs_str_from_utf16(vm, d->u.func.name_u16, d->u.func.name_len, true);
                    int32_t idx = comp_scope_find_own(&c.base_scope, d->u.func.name_u16, d->u.func.name_len, false);
                    mod_add_export_entry(mod, nm, NULL, NULL, nm, idx >= 0 ? c.base_scope.locals[idx].slot : AJS_NO_SLOT, d->line, d->col);
                } else if (d->kind == AST_CLASS_DECL && d->u.klass.name_u16) {
                    AjsString *nm = ajs_str_from_utf16(vm, d->u.klass.name_u16, d->u.klass.name_len, true);
                    int32_t idx = comp_scope_find_own(&c.base_scope, d->u.klass.name_u16, d->u.klass.name_len, false);
                    mod_add_export_entry(mod, nm, NULL, NULL, nm, idx >= 0 ? c.base_scope.locals[idx].slot : AJS_NO_SLOT, d->line, d->col);
                }
            } else {
                for (uint32_t j = 0; j < st->u.mod_decl.specifiers.count; j++) {
                    AjsAstNode *sp = st->u.mod_decl.specifiers.items[j];
                    AjsString *loc_str = ajs_str_from_utf16(vm, sp->u.spec.local_u16, sp->u.spec.local_len, true);
                    AjsString *exp_str = ajs_str_from_utf16(vm, sp->u.spec.exported_u16, sp->u.spec.exported_len, true);
                    int32_t idx = comp_scope_find_own(&c.base_scope, sp->u.spec.local_u16, sp->u.spec.local_len, false);
                    uint32_t slot = idx >= 0 ? c.base_scope.locals[idx].slot : AJS_NO_SLOT;
                    bool was_import = false;
                    for (uint32_t k = 0; k < mod->import_count; k++) {
                        if (ajs_str_eq(mod->imports[k].local_name, loc_str) &&
                            !ajs_str_eq_cstr(mod->imports[k].import_name, "*")) {
                            mod_add_export_entry(mod, exp_str, mod->imports[k].module_specifier,
                                                 mod->imports[k].import_name, NULL, AJS_NO_SLOT, sp->line, sp->col);
                            was_import = true;
                            break;
                        }
                    }
                    if (!was_import) {
                        mod_add_export_entry(mod, exp_str, NULL, NULL, loc_str, slot, sp->line, sp->col);
                    }
                }
            }
        }
    }

    if (!instantiate_hoisted_funcs(&c, &c.base_scope, &prog->u.program.body, false)) goto fail;

    {
        uint32_t mod_disp_fin = UINT32_MAX;
        if (c.base_scope.has_disp_stack) {
            mod_disp_fin = emit_dispose_enter(&c, 0, c.base_scope.disp_slot);
        }
        for (uint32_t i = 0; i < prog->u.program.body.count; i++) {
            if (!compile_stmt(&c, prog->u.program.body.items[i])) goto fail;
        }
        if (c.base_scope.has_disp_stack) {
            emit_dispose_finally(&c, 0, c.base_scope.disp_slot, mod_disp_fin);
        }
    }

    emit_op(&c, OP_UNDEF);
    emit_op(&c, OP_RETURN);

    ajs_source_buf_release(c.src_buf);
    finalize_base_scope_metadata(&c);
    ajs_arena_free(&arena);
    vm->gc_suppress--;
    if (vm->gc_suppress == 0 && vm->gc_bytes > vm->gc_threshold) {
        ajs_gc_collect(vm);
    }
    ajs_temp_root_restore(vm, rs);
    return true;

fail:
    fill_comp_err(&c, mod->url, out_err);
    ajs_source_buf_release(c.src_buf);
    cleanup_compiler(&c);
    ajs_arena_free(&arena);
    ajs_temp_root_restore(vm, rs);
    vm->gc_suppress--;
    return false;
}
