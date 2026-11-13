/*
 * ArcradientJS 1.0 - js/vm/ajs_parse_internal.h
 * Internal Lexer, Token, Arena & AST Definitions shared across Lexer, Parser, and Compiler
 */
#ifndef AJS_PARSE_INTERNAL_H
#define AJS_PARSE_INTERNAL_H

#include "ajs_internal.h"

typedef enum AjsTokenKind {
    TOK_EOF = 0,
    TOK_ERROR,

    TOK_NUMBER,
    TOK_BIGINT,
    TOK_STRING,
    TOK_TEMPLATE_NO_SUB,
    TOK_TEMPLATE_HEAD,
    TOK_TEMPLATE_MIDDLE,
    TOK_TEMPLATE_TAIL,
    TOK_REGEXP,

    TOK_IDENT,
    TOK_PRIV_IDENT,

    /* Keywords */
    KW_AWAIT,
    KW_BREAK,
    KW_CASE,
    KW_CATCH,
    KW_CLASS,
    KW_CONST,
    KW_CONTINUE,
    KW_DEBUGGER,
    KW_DEFAULT,
    KW_DELETE,
    KW_DO,
    KW_ELSE,
    KW_ENUM,
    KW_EXPORT,
    KW_EXTENDS,
    KW_FALSE,
    KW_FINALLY,
    KW_FOR,
    KW_FUNCTION,
    KW_IF,
    KW_IMPORT,
    KW_IN,
    KW_INSTANCEOF,
    KW_LET,
    KW_NEW,
    KW_NULL,
    KW_RETURN,
    KW_STATIC,
    KW_SUPER,
    KW_SWITCH,
    KW_THIS,
    KW_THROW,
    KW_TRUE,
    KW_TRY,
    KW_TYPEOF,
    KW_VAR,
    KW_VOID,
    KW_WHILE,
    KW_WITH,
    KW_YIELD,

    /* Punctuators */
    TOK_LBRACE,         /* { */
    TOK_RBRACE,         /* } */
    TOK_LPAREN,         /* ( */
    TOK_RPAREN,         /* ) */
    TOK_LBRACKET,       /* [ */
    TOK_RBRACKET,       /* ] */
    TOK_DOT,            /* . */
    TOK_ELLIPSIS,       /* ... */
    TOK_SEMI,           /* ; */
    TOK_COMMA,          /* , */
    TOK_QUESTION,       /* ? */
    TOK_OPT_CHAIN,      /* ?. */
    TOK_COLON,          /* : */
    TOK_ARROW,          /* => */

    TOK_PLUS,           /* + */
    TOK_MINUS,          /* - */
    TOK_MUL,            /* * */
    TOK_DIV,            /* / */
    TOK_MOD,            /* % */
    TOK_EXP,            /* ** */
    TOK_INC,            /* ++ */
    TOK_DEC,            /* -- */

    TOK_BIT_AND,        /* & */
    TOK_BIT_OR,         /* | */
    TOK_BIT_XOR,        /* ^ */
    TOK_BIT_NOT,        /* ~ */
    TOK_BANG,           /* ! */
    TOK_SHL,            /* << */
    TOK_SHR,            /* >> */
    TOK_USHR,           /* >>> */

    TOK_AND,            /* && */
    TOK_OR,             /* || */
    TOK_COALESCE,       /* ?? */

    TOK_EQ,             /* == */
    TOK_NEQ,            /* != */
    TOK_STRICT_EQ,      /* === */
    TOK_STRICT_NEQ,     /* !== */
    TOK_LT,             /* < */
    TOK_LTE,            /* <= */
    TOK_GT,             /* > */
    TOK_GTE,            /* >= */

    TOK_ASSIGN,         /* = */
    TOK_ADD_ASSIGN,     /* += */
    TOK_SUB_ASSIGN,     /* -= */
    TOK_MUL_ASSIGN,     /* *= */
    TOK_DIV_ASSIGN,     /* /= */
    TOK_MOD_ASSIGN,     /* %= */
    TOK_EXP_ASSIGN,     /* **= */
    TOK_SHL_ASSIGN,     /* <<= */
    TOK_SHR_ASSIGN,     /* >>= */
    TOK_USHR_ASSIGN,    /* >>>= */
    TOK_AND_ASSIGN,     /* &= */
    TOK_OR_ASSIGN,      /* |= */
    TOK_XOR_ASSIGN,     /* ^= */
    TOK_LOG_AND_ASSIGN, /* &&= */
    TOK_LOG_OR_ASSIGN,  /* ||= */
    TOK_COALESCE_ASSIGN /* ??= */
} AjsTokenKind;

typedef struct AjsAstBlockChunk {
    struct AjsAstBlockChunk *next;
    size_t                   used;
    size_t                   cap;
    uint8_t                  data[];
} AjsAstBlockChunk;

typedef struct AjsAstArena {
    AjsAstBlockChunk *head;
} AjsAstArena;

void *ajs_arena_alloc(AjsAstArena *arena, size_t size);
char *ajs_arena_strdup(AjsAstArena *arena, const char *s, size_t len);
void  ajs_arena_free(AjsAstArena *arena);

typedef struct AjsToken {
    AjsTokenKind    kind;
    uint32_t        line;
    uint32_t        col;
    size_t          start_off;
    size_t          end_off;
    bool            had_newline;
    bool            has_escape;
    bool            has_legacy_octal;
    bool            invalid_tpl_escape;
    double          num_val;
    const uint16_t *u16_data;
    uint32_t        u16_len;
    const uint16_t *raw_u16;
    uint32_t        raw_u16_len;
    const char     *utf8_text;
    size_t          utf8_len;
    const uint16_t *rx_flags;
    uint32_t        rx_flags_len;
} AjsToken;

typedef struct AjsLexer {
    AjsAstArena *arena;
    const char  *src;
    size_t       len;
    size_t       pos;
    uint32_t     line;
    uint32_t     col;
    bool         is_module;
    bool         strict;
    bool         at_line_start;
    char         err_msg[256];
    uint32_t     err_line;
    uint32_t     err_col;
    AjsToken     cur;
    bool         has_peek;
    AjsToken     peek_tok;
} AjsLexer;

void     ajs_lexer_init(AjsLexer *lex, AjsAstArena *arena, const char *src, size_t len, bool is_module, bool strict);
AjsToken ajs_lexer_next(AjsLexer *lex);
AjsToken ajs_lexer_peek(AjsLexer *lex);
bool     ajs_lexer_rescan_regexp(AjsLexer *lex);
bool     ajs_lexer_rescan_template(AjsLexer *lex);

/* ============================================================================
 * AST Node Definitions
 * ============================================================================ */

typedef struct AjsAstNode AjsAstNode;

typedef struct AjsNodeList {
    uint32_t     count;
    uint32_t     cap;
    AjsAstNode **items;
} AjsNodeList;

typedef enum AjsAstKind {
    /* Program / Declarations / Statements */
    AST_PROGRAM = 1,
    AST_BLOCK,
    AST_EMPTY_STMT,
    AST_EXPR_STMT,
    AST_IF_STMT,
    AST_WHILE_STMT,
    AST_DO_WHILE_STMT,
    AST_FOR_STMT,
    AST_FOR_IN_OF_STMT,
    AST_SWITCH_STMT,
    AST_SWITCH_CASE,
    AST_RETURN_STMT,
    AST_BREAK_STMT,
    AST_CONTINUE_STMT,
    AST_THROW_STMT,
    AST_TRY_STMT,
    AST_WITH_STMT,
    AST_LABELED_STMT,
    AST_DEBUGGER_STMT,
    AST_VAR_DECL,       /* var / let / const */
    AST_VAR_DECLARATOR,
    AST_FUNC_DECL,
    AST_CLASS_DECL,

    /* Patterns (Destructuring & Params) */
    AST_IDENT,
    AST_PRIV_IDENT,
    AST_ARRAY_PATTERN,
    AST_OBJECT_PATTERN,
    AST_PROP_PATTERN,
    AST_ASSIGN_PATTERN,
    AST_REST_ELEMENT,

    /* Expressions */
    AST_LITERAL_UNDEF,
    AST_LITERAL_NULL,
    AST_LITERAL_BOOL,
    AST_LITERAL_NUM,
    AST_LITERAL_BIGINT,
    AST_LITERAL_STR,
    AST_LITERAL_REGEXP,
    AST_THIS_EXPR,
    AST_SUPER_EXPR,
    AST_NEW_TARGET,
    AST_IMPORT_META,
    AST_ARRAY_EXPR,
    AST_SPREAD_ELEM,
    AST_OBJECT_EXPR,
    AST_OBJECT_PROP,
    AST_FUNC_EXPR,
    AST_CLASS_EXPR,
    AST_CLASS_ELEM,     /* method, getter, setter, field, static_block */
    AST_TEMPLATE_LIT,
    AST_TAGGED_TEMPLATE,
    AST_MEMBER_EXPR,
    AST_CALL_EXPR,
    AST_NEW_EXPR,
    AST_DYNAMIC_IMPORT,
    AST_OPT_CHAIN_EXPR, /* Wraps an optional chain sequence for short-circuit jump */
    AST_UNARY_EXPR,
    AST_UPDATE_EXPR,
    AST_BINARY_EXPR,
    AST_LOGICAL_EXPR,
    AST_ASSIGN_EXPR,
    AST_COND_EXPR,
    AST_SEQ_EXPR,
    AST_YIELD_EXPR,
    AST_AWAIT_EXPR,

    /* ES Module Declarations */
    AST_IMPORT_DECL,
    AST_EXPORT_NAMED_DECL,
    AST_EXPORT_DEFAULT_DECL,
    AST_EXPORT_ALL_DECL,
    AST_SPECIFIER
} AjsAstKind;

typedef enum AjsVarKind {
    AJS_VAR_VAR         = 0,
    AJS_VAR_LET         = 1,
    AJS_VAR_CONST       = 2,
    AJS_VAR_USING       = 3,
    AJS_VAR_AWAIT_USING = 4
} AjsVarKind;

typedef enum AjsPropKind {
    AJS_PROP_INIT   = 0,
    AJS_PROP_GET    = 1,
    AJS_PROP_SET    = 2,
    AJS_PROP_METHOD = 3,
    AJS_PROP_FIELD  = 4,
    AJS_PROP_STATIC_BLOCK = 5
} AjsPropKind;

struct AjsAstNode {
    AjsAstKind kind;
    uint32_t   line;
    uint32_t   col;
    bool       parenthesized;

    union {
        struct {
            bool        is_module;
            bool        strict;
            bool        has_tla;
            AjsNodeList body;
        } program;

        struct {
            AjsNodeList stmts;
        } block;

        struct {
            AjsAstNode *expr;
        } expr_stmt;

        struct {
            AjsAstNode *test;
            AjsAstNode *conseq;
            AjsAstNode *alt;
        } if_stmt;

        struct {
            AjsAstNode *test;
            AjsAstNode *body;
        } loop;

        struct {
            AjsAstNode *init;   /* VarDecl or Expr or NULL */
            AjsAstNode *test;
            AjsAstNode *update;
            AjsAstNode *body;
        } for_stmt;

        struct {
            bool        is_of;
            bool        is_await;
            AjsAstNode *left;   /* VarDecl or Pattern/LHS */
            AjsAstNode *right;
            AjsAstNode *body;
        } for_in_of;

        struct {
            AjsAstNode *discriminant;
            AjsNodeList cases;
        } switch_stmt;

        struct {
            AjsAstNode *test;   /* NULL for default: */
            AjsNodeList conseq;
        } switch_case;

        struct {
            const uint16_t *label_u16;
            uint32_t        label_len;
        } jump;

        struct {
            const uint16_t *label_u16;
            uint32_t        label_len;
            AjsAstNode     *body;
        } labeled;

        struct {
            AjsAstNode *block;
            AjsAstNode *catch_param; /* NULL for optional catch binding */
            AjsAstNode *catch_body;  /* NULL if no catch */
            AjsAstNode *finally_body;/* NULL if no finally */
        } try_stmt;

        struct {
            AjsAstNode *obj;
            AjsAstNode *body;
        } with_stmt;

        struct {
            AjsVarKind  var_kind;
            AjsNodeList decls;
        } var_decl;

        struct {
            AjsAstNode *id;     /* Ident or Pattern */
            AjsAstNode *init;
        } declarator;

        struct {
            const uint16_t *u16;
            uint32_t        len;
            const char     *utf8;
        } ident;

        struct {
            bool            bval;
            double          nval;
            const char     *bi_utf8;
            size_t          bi_len;
            const uint16_t *s_u16;
            uint32_t        s_len;
            const uint16_t *rx_pat;
            uint32_t        rx_pat_len;
            const uint16_t *rx_flags;
            uint32_t        rx_flags_len;
        } lit;

        struct {
            AjsNodeList elems;  /* NULL entries represent elisions `,` */
        } array;

        struct {
            AjsNodeList props;
        } object;

        struct {
            AjsPropKind kind;
            bool        computed;
            bool        shorthand;
            bool        is_static;
            bool        is_private;
            AjsAstNode *key;
            AjsAstNode *value;
        } prop;

        struct {
            AjsAstNode *left;
            AjsAstNode *right;
        } assign_pat;

        struct {
            AjsFuncKind     fn_kind;
            bool            strict;
            bool            has_simple_params;
            bool            expr_body;
            const uint16_t *name_u16;
            uint32_t        name_len;
            const char     *src_utf8;
            size_t          src_len;
            AjsNodeList     params;
            AjsAstNode     *body;   /* AST_BLOCK or expression */
        } func;

        struct {
            const uint16_t *name_u16;
            uint32_t        name_len;
            AjsAstNode     *super_expr;
            AjsNodeList     elements;
            const char     *src_utf8;
            size_t          src_len;
        } klass;

        struct {
            uint32_t         part_count;
            const uint16_t **cooked;    /* NULL if invalid escape in tagged template */
            uint32_t        *cooked_len;
            const uint16_t **raw;
            uint32_t        *raw_len;
            AjsNodeList      exprs;
        } tpl;

        struct {
            AjsAstNode *tag;
            AjsAstNode *quasi;  /* AST_TEMPLATE_LIT */
        } tagged_tpl;

        struct {
            bool        computed;
            bool        optional;   /* true if `?.` right at this step */
            AjsAstNode *object;
            AjsAstNode *property;
        } member;

        struct {
            bool        optional;   /* true if `?.(` right at this step */
            AjsAstNode *callee;
            AjsNodeList args;
        } call;

        struct {
            AjsAstNode *inner;
        } opt_chain;

        struct {
            AjsTokenKind op;
            bool         prefix;
            AjsAstNode  *arg;
        } unary;

        struct {
            AjsTokenKind op;
            AjsAstNode  *left;
            AjsAstNode  *right;
        } binary;

        struct {
            AjsAstNode *test;
            AjsAstNode *conseq;
            AjsAstNode *alt;
        } cond;

        struct {
            AjsNodeList exprs;
        } seq;

        struct {
            bool        delegate;   /* yield* */
            AjsAstNode *arg;
        } yield_expr;

        struct {
            AjsNodeList     specifiers;
            const uint16_t *source_u16;
            uint32_t        source_len;
            AjsAstNode     *decl;       /* For export var/let/const/function/class or default */
        } mod_decl;

        struct {
            const uint16_t *imported_u16; /* NULL for import * as ns */
            uint32_t        imported_len;
            const uint16_t *local_u16;
            uint32_t        local_len;
            const uint16_t *exported_u16;
            uint32_t        exported_len;
            bool            is_namespace;
        } spec;
    } u;
};

/* Parser entry point */
AjsAstNode *ajs_parse_program(AjsVm *vm, AjsAstArena *arena, const char *src, size_t len,
                              const char *source_url, bool is_module, bool force_strict,
                              bool allow_new_target, bool allow_super_prop, bool allow_super_call,
                              AjsErrorInfo *out_err);

#endif /* AJS_PARSE_INTERNAL_H */
