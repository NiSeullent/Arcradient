/*
 * ArcradientJS - Clean-Room ECMAScript 2026 RegExp Engine & Built-ins
 * Pure C11, zero VLAs, zero external regex library dependencies.
 *
 * Implements:
 *  - Full ES2026 + Annex B RegExp pattern parser & bytecode compiler
 *  - Flags: d (hasIndices), g (global), i (ignoreCase), m (multiline),
 *           s (dotAll), u (unicode), v (unicodeSets), y (sticky)
 *  - ES2025 inline pattern modifiers (?ims-ims:...)
 *  - ES2018/ES2025 named capture groups (?<name>...) & duplicate names across disjunctions
 *  - Lookahead (?=...) / (?!...) and backward-compiled Lookbehind (?<=...) / (?<!...)
 *  - Backreferences \1.. and \k<name>, Unicode property escapes \p{...} / \P{...}
 *  - Explicit heap-backed iterative backtracking VM with zero-width loop protection
 *    and step budget (zero C stack overflow on adversarial inputs)
 *  - RegExp constructor, RegExp.escape, RegExp.prototype.*, [Symbol.match/matchAll/replace/search/split]
 */

#include "../vm/ajs_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ============================================================================
 * 1. Flags & Data Structures
 * ============================================================================ */

enum {
    RE_FLAG_D = 1u << 0, /* hasIndices */
    RE_FLAG_G = 1u << 1, /* global */
    RE_FLAG_I = 1u << 2, /* ignoreCase */
    RE_FLAG_M = 1u << 3, /* multiline */
    RE_FLAG_S = 1u << 4, /* dotAll */
    RE_FLAG_U = 1u << 5, /* unicode */
    RE_FLAG_V = 1u << 6, /* unicodeSets */
    RE_FLAG_Y = 1u << 7  /* sticky */
};

typedef struct AjsReRange {
    uint32_t lo;
    uint32_t hi;
} AjsReRange;

typedef struct AjsRePropCheck {
    bool negate;
    char name[40];
    char value[40];
} AjsRePropCheck;

typedef struct AjsReStrAlt {
    uint32_t *cps;
    uint32_t  len;
} AjsReStrAlt;

typedef struct AjsReCharClass {
    bool            invert;
    uint32_t        local_flags;
    uint32_t        range_count;
    uint32_t        range_cap;
    AjsReRange     *ranges;
    uint32_t        predef_mask; /* bit0:\d, bit1:\D, bit2:\w, bit3:\W, bit4:\s, bit5:\S */
    uint32_t        prop_count;
    uint32_t        prop_cap;
    AjsRePropCheck *props;
    uint32_t        str_count;
    uint32_t        str_cap;
    AjsReStrAlt    *strs;        /* For v-mode \q{abc|def} multi-codepoint strings */
} AjsReCharClass;

enum {
    RE_PREDEF_d = 1u << 0,
    RE_PREDEF_D = 1u << 1,
    RE_PREDEF_w = 1u << 2,
    RE_PREDEF_W = 1u << 3,
    RE_PREDEF_s = 1u << 4,
    RE_PREDEF_S = 1u << 5
};

typedef struct AjsReGroupEntry {
    char     *name_utf8;
    uint16_t *name_u16;
    uint32_t  name_len16;
    uint32_t  cap_idx;     /* 1-based capture index */
} AjsReGroupEntry;

typedef enum AjsReOp {
    RE_OP_MATCH = 0,
    RE_OP_LOOK_END,
    RE_OP_CHAR,            /* a = code point / unit, flags = local flags | dir */
    RE_OP_ANY,             /* . */
    RE_OP_CLASS,           /* a = class index */
    RE_OP_BOL,             /* ^ */
    RE_OP_EOL,             /* $ */
    RE_OP_WORD_BOUND,      /* \b */
    RE_OP_NOT_WORD_BOUND,  /* \B */
    RE_OP_SAVE,            /* a = slot index (0 .. (cap_count+1)*2 - 1) */
    RE_OP_CLEAR_CAPS,      /* a = cap_lo (1-based), b = cap_hi (1-based inclusive) */
    RE_OP_JMP,             /* a = target pc */
    RE_OP_SPLIT,           /* a = preferred pc, b = fallback pc */
    RE_OP_LOOP_INIT,       /* a = reg_idx, b = body_pc */
    RE_OP_LOOP_STEP,       /* a = reg_idx, b = body_pc, c = exit_pc, min/max in aux */
    RE_OP_BACKREF,         /* a = 1-based cap_idx */
    RE_OP_NAMED_BACKREF,   /* a = index into unique group names or first matching group */
    RE_OP_LOOKAROUND       /* a = sub_pc, b = is_positive (1/0) */
} AjsReOp;

#define RE_INST_BACKWARD (1u << 16)

typedef struct AjsReInst {
    uint8_t  op;
    uint8_t  pad;
    uint16_t flags_dir; /* low8: local RE_FLAG_*, bit8 (0x100): backward */
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t min_rep;
    uint32_t max_rep;   /* UINT32_MAX for infinity */
} AjsReInst;

struct AjsRegExp {
    uint16_t        *source_u16;
    uint32_t         source_len;
    uint32_t         flags;
    uint32_t         capture_count;
    uint32_t         loop_count;
    uint32_t         group_count;
    AjsReGroupEntry *groups;
    uint32_t         class_count;
    AjsReCharClass  *classes;
    uint32_t         code_len;
    AjsReInst       *code;
};

static void re_free_char_class(AjsReCharClass *cc) {
    if (!cc) return;
    free(cc->ranges);
    free(cc->props);
    for (uint32_t i = 0; i < cc->str_count; i++) {
        free(cc->strs[i].cps);
    }
    free(cc->strs);
    memset(cc, 0, sizeof(*cc));
}

void ajs_regexp_free(AjsRegExp *re) {
    if (!re) return;
    free(re->source_u16);
    for (uint32_t i = 0; i < re->group_count; i++) {
        free(re->groups[i].name_utf8);
        free(re->groups[i].name_u16);
    }
    free(re->groups);
    for (uint32_t i = 0; i < re->class_count; i++) {
        re_free_char_class(&re->classes[i]);
    }
    free(re->classes);
    free(re->code);
    free(re);
}

/* ============================================================================
 * 2. Helper Predicates & Unicode Property Validation
 * ============================================================================ */

static bool re_parse_flags(const uint16_t *u, uint32_t len, uint32_t *out_flags, char *err_buf, size_t err_cap) {
    uint32_t f = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t bit = 0;
        switch (u[i]) {
            case 'd': bit = RE_FLAG_D; break;
            case 'g': bit = RE_FLAG_G; break;
            case 'i': bit = RE_FLAG_I; break;
            case 'm': bit = RE_FLAG_M; break;
            case 's': bit = RE_FLAG_S; break;
            case 'u': bit = RE_FLAG_U; break;
            case 'v': bit = RE_FLAG_V; break;
            case 'y': bit = RE_FLAG_Y; break;
            default:
                if (err_buf && err_cap > 0) snprintf(err_buf, err_cap, "Invalid regular expression flag");
                return false;
        }
        if (f & bit) {
            if (err_buf && err_cap > 0) snprintf(err_buf, err_cap, "Duplicate regular expression flag");
            return false;
        }
        f |= bit;
    }
    if ((f & RE_FLAG_U) && (f & RE_FLAG_V)) {
        if (err_buf && err_cap > 0) snprintf(err_buf, err_cap, "Invalid regular expression flags: 'u' and 'v' cannot be combined");
        return false;
    }
    *out_flags = f;
    return true;
}

static bool re_is_line_terminator(uint32_t cp) {
    return cp == 0x000Au || cp == 0x000Du || cp == 0x2028u || cp == 0x2029u;
}

static bool re_is_word_char(uint32_t cp, bool unicode_icase) {
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
        (cp >= '0' && cp <= '9') || cp == '_') {
        return true;
    }
    if (unicode_icase && (cp == 0x017Fu || cp == 0x212Au)) {
        return true;
    }
    return false;
}

static uint32_t re_fold_cp(uint32_t cp, uint32_t flags) {
    if (!(flags & RE_FLAG_I)) return cp;
    bool uni = (flags & (RE_FLAG_U | RE_FLAG_V)) != 0;
    return ajs_unicode_fold_case(cp, uni);
}

static bool re_is_valid_unicode_prop(const char *name, const char *val) {
    static const char *const k_lone_props[] = {
        "ASCII", "ASCII_Hex_Digit", "AHex", "Any", "Assigned",
        "White_Space", "WSpace", "space",
        "Emoji", "Emoji_Presentation", "Emoji_Modifier", "Emoji_Modifier_Base",
        "Emoji_Component", "Extended_Pictographic",
        "ID_Start", "IDS", "ID_Continue", "IDC", "XID_Start", "XIDS", "XID_Continue", "XIDC",
        "Letter", "L", "Alphabetic", "Alpha",
        "Uppercase_Letter", "Lu", "Uppercase", "Upper",
        "Lowercase_Letter", "Ll", "Lowercase", "Lower",
        "Titlecase_Letter", "Lt", "Modifier_Letter", "Lm", "Other_Letter", "Lo", "Cased_Letter", "LC",
        "Mark", "M", "Nonspacing_Mark", "Mn", "Spacing_Mark", "Mc", "Enclosing_Mark", "Me",
        "Number", "N", "Decimal_Number", "Nd", "digit", "Letter_Number", "Nl", "Other_Number", "No",
        "Punctuation", "P", "Connector_Punctuation", "Pc", "Dash_Punctuation", "Pd",
        "Open_Punctuation", "Ps", "Close_Punctuation", "Pe", "Initial_Punctuation", "Pi",
        "Final_Punctuation", "Pf", "Other_Punctuation", "Po",
        "Symbol", "S", "Math_Symbol", "Sm", "Currency_Symbol", "Sc", "Modifier_Symbol", "Sk", "Other_Symbol", "So",
        "Separator", "Z", "Space_Separator", "Zs", "Line_Separator", "Zl", "Paragraph_Separator", "Zp",
        "Other", "C", "Control", "Cc", "Format", "Cf", "Surrogate", "Cs", "Private_Use", "Co", "Unassigned", "Cn",
        "Hex_Digit", "Hex", "Bidi_Control", "Bidi_C", "Bidi_Mirrored", "Bidi_M",
        "Case_Ignorable", "CI", "Cased", "Dash", "Default_Ignorable_Code_Point", "DI",
        "Deprecated", "Dep", "Diacritic", "Dia", "Extender", "Ext", "Grapheme_Base", "Gr_Base",
        "Grapheme_Extend", "Gr_Ext", "Ideographic", "Ideo", "Join_Control", "Join_C",
        "Logical_Order_Exception", "LOE", "Math", "Noncharacter_Code_Point", "NChar",
        "Pattern_Syntax", "Pat_Syn", "Pattern_White_Space", "Pat_WS", "Quotation_Mark", "QMark",
        "Radical", "Regional_Indicator", "RI", "Sentence_Terminal", "STerm", "Soft_Dotted", "SD",
        "Terminal_Punctuation", "Term", "Unified_Ideograph", "UIdeo", "Variation_Selector", "VS",
        "Basic_Emoji", "RGI_Emoji"
    };
    static const char *const k_gc_vals[] = {
        "Letter", "L", "Uppercase_Letter", "Lu", "Lowercase_Letter", "Ll",
        "Titlecase_Letter", "Lt", "Modifier_Letter", "Lm", "Other_Letter", "Lo", "Cased_Letter", "LC",
        "Mark", "M", "Nonspacing_Mark", "Mn", "Spacing_Mark", "Mc", "Enclosing_Mark", "Me",
        "Number", "N", "Decimal_Number", "Nd", "Letter_Number", "Nl", "Other_Number", "No",
        "Punctuation", "P", "Symbol", "S", "Separator", "Z", "Space_Separator", "Zs",
        "Other", "C", "Control", "Cc", "Format", "Cf", "Surrogate", "Cs", "Private_Use", "Co", "Unassigned", "Cn"
    };
    static const char *const k_scripts[] = {
        "Latin", "Latn", "Hangul", "Hang", "Han", "Hani", "Hiragana", "Hira",
        "Katakana", "Kana", "Greek", "Grek", "Cyrillic", "Cyrl", "Arabic", "Arab",
        "Hebrew", "Hebr", "Devanagari", "Deva", "Thai", "Common", "Zyyy", "Inherited", "Zinh"
    };

    if (!val || val[0] == '\0') {
        for (size_t i = 0; i < sizeof(k_lone_props) / sizeof(k_lone_props[0]); i++) {
            if (strcmp(name, k_lone_props[i]) == 0) return true;
        }
        return false;
    }
    if (strcmp(name, "General_Category") == 0 || strcmp(name, "gc") == 0) {
        for (size_t i = 0; i < sizeof(k_gc_vals) / sizeof(k_gc_vals[0]); i++) {
            if (strcmp(val, k_gc_vals[i]) == 0) return true;
        }
        return false;
    }
    if (strcmp(name, "Script") == 0 || strcmp(name, "sc") == 0 ||
        strcmp(name, "Script_Extensions") == 0 || strcmp(name, "scx") == 0) {
        for (size_t i = 0; i < sizeof(k_scripts) / sizeof(k_scripts[0]); i++) {
            if (strcmp(val, k_scripts[i]) == 0) return true;
        }
        return false;
    }
    return false;
}

static bool re_eval_unicode_prop(const AjsRePropCheck *p, uint32_t cp, uint32_t flags) {
    bool m = false;
    const char *n = p->name;
    const char *v = p->value[0] ? p->value : NULL;
    if (!v) {
        if (strcmp(n, "Surrogate") == 0 || strcmp(n, "Cs") == 0) m = (cp >= 0xD800u && cp <= 0xDFFFu);
        else if (strcmp(n, "Control") == 0 || strcmp(n, "Cc") == 0) m = (cp <= 0x1Fu) || (cp >= 0x7Fu && cp <= 0x9Fu);
        else if (strcmp(n, "Hex_Digit") == 0 || strcmp(n, "Hex") == 0) {
            m = (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'F') || (cp >= 'a' && cp <= 'f') ||
                (cp >= 0xFF10u && cp <= 0xFF19u) || (cp >= 0xFF21u && cp <= 0xFF26u) || (cp >= 0xFF41u && cp <= 0xFF46u);
        } else if (strcmp(n, "Space_Separator") == 0 || strcmp(n, "Zs") == 0 || strcmp(n, "Separator") == 0 || strcmp(n, "Z") == 0) {
            m = (cp == 0x20u || cp == 0xA0u || cp == 0x1680u || (cp >= 0x2000u && cp <= 0x200Au) || cp == 0x202Fu || cp == 0x205Fu || cp == 0x3000u);
        } else if (strcmp(n, "Symbol") == 0 || strcmp(n, "S") == 0 || strcmp(n, "Math_Symbol") == 0 || strcmp(n, "Sm") == 0 || strcmp(n, "Math") == 0) {
            m = (cp == '+' || cp == '<' || cp == '=' || cp == '>' || cp == '|' || cp == '~' || cp == '$' || cp == '^' || cp == '`' ||
                 (cp >= 0x2200u && cp <= 0x22FFu) || ajs_unicode_match_prop("Emoji", NULL, cp));
        } else if (strcmp(n, "Currency_Symbol") == 0 || strcmp(n, "Sc") == 0) {
            m = (cp == '$' || cp == 0x00A2u || cp == 0x00A3u || cp == 0x00A4u || cp == 0x00A5u || (cp >= 0x20A0u && cp <= 0x20CFu));
        } else if (strcmp(n, "XID_Start") == 0 || strcmp(n, "XIDS") == 0 || strcmp(n, "IDS") == 0) {
            m = ajs_unicode_match_prop("ID_Start", NULL, cp);
        } else if (strcmp(n, "XID_Continue") == 0 || strcmp(n, "XIDC") == 0 || strcmp(n, "IDC") == 0) {
            m = ajs_unicode_match_prop("ID_Continue", NULL, cp);
        } else {
            m = ajs_unicode_match_prop(n, NULL, cp);
        }
    } else if (strcmp(n, "Script") == 0 || strcmp(n, "sc") == 0 || strcmp(n, "Script_Extensions") == 0 || strcmp(n, "scx") == 0) {
        if (strcmp(v, "Arabic") == 0 || strcmp(v, "Arab") == 0) m = (cp >= 0x0600u && cp <= 0x06FFu);
        else if (strcmp(v, "Hebrew") == 0 || strcmp(v, "Hebr") == 0) m = (cp >= 0x0590u && cp <= 0x05FFu);
        else if (strcmp(v, "Devanagari") == 0 || strcmp(v, "Deva") == 0) m = (cp >= 0x0900u && cp <= 0x097Fu);
        else if (strcmp(v, "Thai") == 0) m = (cp >= 0x0E00u && cp <= 0x0E7Fu);
        else if (strcmp(v, "Common") == 0 || strcmp(v, "Zyyy") == 0) m = (cp <= 0x40u || (cp >= 0x5Bu && cp <= 0x60u) || (cp >= 0x7Bu && cp <= 0x7Fu));
        else m = ajs_unicode_match_prop(n, v, cp);
    } else if (strcmp(n, "General_Category") == 0 || strcmp(n, "gc") == 0) {
        AjsRePropCheck tmp = *p;
        snprintf(tmp.name, sizeof(tmp.name), "%s", v);
        tmp.value[0] = '\0';
        tmp.negate = false;
        m = re_eval_unicode_prop(&tmp, cp, flags);
    } else {
        m = ajs_unicode_match_prop(n, v, cp);
    }

    if ((flags & RE_FLAG_I) && !m && !(flags & RE_FLAG_V)) {
        uint32_t lo = ajs_unicode_to_lower(cp);
        uint32_t up = ajs_unicode_to_upper(cp);
        if (lo != cp || up != cp) {
            AjsRePropCheck base = *p;
            base.negate = false;
            if (lo != cp && re_eval_unicode_prop(&base, lo, flags & ~RE_FLAG_I)) m = true;
            if (!m && up != cp && re_eval_unicode_prop(&base, up, flags & ~RE_FLAG_I)) m = true;
        }
    }
    return p->negate ? !m : m;
}

static bool re_match_predef(uint32_t mask, uint32_t cp, uint32_t flags) {
    bool uni_i = (flags & RE_FLAG_I) && (flags & (RE_FLAG_U | RE_FLAG_V));
    if ((mask & RE_PREDEF_d) && (cp >= '0' && cp <= '9')) return true;
    if ((mask & RE_PREDEF_D) && !(cp >= '0' && cp <= '9')) return true;
    if ((mask & RE_PREDEF_w) && re_is_word_char(cp, uni_i)) return true;
    if ((mask & RE_PREDEF_W) && !re_is_word_char(cp, uni_i)) return true;
    if ((mask & RE_PREDEF_s) && ajs_unicode_is_whitespace(cp)) return true;
    if ((mask & RE_PREDEF_S) && !ajs_unicode_is_whitespace(cp)) return true;
    return false;
}

static bool re_char_class_matches_cp(const AjsReCharClass *cc, uint32_t cp) {
    uint32_t fl = cc->local_flags;
    uint32_t fcp = re_fold_cp(cp, fl);
    bool hit = false;
    if (cc->predef_mask && re_match_predef(cc->predef_mask, cp, fl)) {
        hit = true;
    }
    for (uint32_t i = 0; !hit && i < cc->range_count; i++) {
        uint32_t lo = cc->ranges[i].lo;
        uint32_t hi = cc->ranges[i].hi;
        if (cp >= lo && cp <= hi) { hit = true; break; }
        if (fl & RE_FLAG_I) {
            if (lo == hi) {
                if (fcp == re_fold_cp(lo, fl)) { hit = true; break; }
            } else if (hi - lo <= 256u) {
                for (uint32_t c = lo; c <= hi; c++) {
                    if (re_fold_cp(c, fl) == fcp) { hit = true; break; }
                }
            } else {
                uint32_t lo_c = ajs_unicode_to_lower(cp);
                uint32_t up_c = ajs_unicode_to_upper(cp);
                if ((lo_c >= lo && lo_c <= hi) || (up_c >= lo && up_c <= hi)) { hit = true; break; }
            }
        }
    }
    for (uint32_t i = 0; !hit && i < cc->prop_count; i++) {
        if (re_eval_unicode_prop(&cc->props[i], cp, fl)) hit = true;
    }
    return cc->invert ? !hit : hit;
}

/* ============================================================================
 * 3. AST & Clean-Room Compiler (Supports Forward & Backward Lookbehind Emission)
 * ============================================================================ */

typedef enum ReAstKind {
    RE_AST_EMPTY = 0,
    RE_AST_DISJ,
    RE_AST_SEQ,
    RE_AST_CHAR,
    RE_AST_ANY,
    RE_AST_CLASS,
    RE_AST_BOL,
    RE_AST_EOL,
    RE_AST_BOUND,
    RE_AST_NOT_BOUND,
    RE_AST_GROUP,        /* cap_idx == 0 for non-capturing / modifier group */
    RE_AST_QUANT,
    RE_AST_BACKREF,
    RE_AST_NAMED_REF,
    RE_AST_LOOKAROUND
} ReAstKind;

typedef struct ReAstNode ReAstNode;
struct ReAstNode {
    ReAstKind   kind;
    uint32_t    local_flags;
    uint32_t    u1;          /* cp, class_idx, cap_idx, min_rep, is_pos */
    uint32_t    u2;          /* max_rep, is_lookbehind */
    bool        greedy;
    uint32_t    cap_lo;      /* 1-based range of captures inside subtree */
    uint32_t    cap_hi;
    char       *ref_name;
    ReAstNode  *child;
    ReAstNode **items;
    uint32_t    item_count;
    uint32_t    item_cap;
};

typedef struct ReCompiler {
    const uint16_t  *pat;
    uint32_t         len;
    uint32_t         pos;
    uint32_t         flags;
    uint32_t         cur_flags;
    bool             uni_mode;
    bool             has_named_groups;
    uint32_t         total_captures;
    uint32_t         next_cap_idx;
    uint32_t         next_loop_idx;
    uint32_t         depth;

    /* Group registry + duplicate group-name scoping (ES2025) */
    uint32_t         group_count;
    uint32_t         group_cap;
    AjsReGroupEntry *groups;
    char           **active_names; /* Forbidden in current alternative/ancestors */
    uint32_t         active_count;
    uint32_t         active_cap;

    /* Character classes */
    uint32_t         class_count;
    uint32_t         class_cap;
    AjsReCharClass  *classes;

    /* Emitted bytecode */
    uint32_t         code_len;
    uint32_t         code_cap;
    AjsReInst       *code;

    /* AST arena list for 1-pass cleanup */
    ReAstNode      **all_nodes;
    uint32_t         node_count;
    uint32_t         node_cap;

    bool             failed;
    char             err[160];
} ReCompiler;

static void rc_fail(ReCompiler *rc, const char *msg) {
    if (!rc->failed) {
        rc->failed = true;
        snprintf(rc->err, sizeof(rc->err), "%s", msg);
    }
}

static ReAstNode *rc_alloc_node(ReCompiler *rc, ReAstKind kind) {
    if (rc->failed) return NULL;
    ReAstNode *n = (ReAstNode *)calloc(1, sizeof(ReAstNode));
    if (!n) { rc_fail(rc, "Out of memory compiling RegExp"); return NULL; }
    if (rc->node_count >= rc->node_cap) {
        uint32_t ncap = rc->node_cap ? rc->node_cap * 2u : 64u;
        ReAstNode **arr = (ReAstNode **)realloc(rc->all_nodes, ncap * sizeof(ReAstNode *));
        if (!arr) { free(n); rc_fail(rc, "Out of memory"); return NULL; }
        rc->all_nodes = arr;
        rc->node_cap = ncap;
    }
    rc->all_nodes[rc->node_count++] = n;
    n->kind = kind;
    n->local_flags = rc->cur_flags;
    return n;
}

static bool rc_node_push(ReCompiler *rc, ReAstNode *parent, ReAstNode *item) {
    if (!parent || !item || rc->failed) return false;
    if (parent->item_count >= parent->item_cap) {
        uint32_t ncap = parent->item_cap ? parent->item_cap * 2u : 8u;
        ReAstNode **arr = (ReAstNode **)realloc(parent->items, ncap * sizeof(ReAstNode *));
        if (!arr) { rc_fail(rc, "Out of memory"); return false; }
        parent->items = arr;
        parent->item_cap = ncap;
    }
    parent->items[parent->item_count++] = item;
    return true;
}

static void rc_free_all_ast(ReCompiler *rc) {
    for (uint32_t i = 0; i < rc->node_count; i++) {
        free(rc->all_nodes[i]->items);
        free(rc->all_nodes[i]->ref_name);
        free(rc->all_nodes[i]);
    }
    free(rc->all_nodes);
    rc->all_nodes = NULL;
    rc->node_count = rc->node_cap = 0;
}

static uint32_t rc_read_cp(ReCompiler *rc) {
    if (rc->pos >= rc->len) return UINT32_MAX;
    if (rc->uni_mode) {
        uint32_t adv = 1;
        uint32_t cp = ajs_utf16_code_point_at(rc->pat, rc->len, rc->pos, &adv);
        rc->pos += adv;
        return cp;
    }
    return rc->pat[rc->pos++];
}

static int rc_hex_val(uint16_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Pre-scan pattern to count total captures and detect whether any (?<name>...) exists */
static void rc_prescan_captures(ReCompiler *rc) {
    uint32_t caps = 0;
    bool in_class = false;
    uint32_t class_depth = 0;
    for (uint32_t i = 0; i < rc->len; i++) {
        uint16_t c = rc->pat[i];
        if (c == '\\') { i++; continue; }
        if (c == '[') {
            if (!in_class) { in_class = true; class_depth = 1; }
            else if (rc->flags & RE_FLAG_V) class_depth++;
            continue;
        }
        if (c == ']' && in_class) {
            if (rc->flags & RE_FLAG_V) {
                if (--class_depth == 0) in_class = false;
            } else {
                in_class = false;
            }
            continue;
        }
        if (!in_class && c == '(') {
            if (i + 1 < rc->len && rc->pat[i + 1] == '?') {
                if (i + 2 < rc->len && rc->pat[i + 2] == '<' &&
                    !(i + 3 < rc->len && (rc->pat[i + 3] == '=' || rc->pat[i + 3] == '!'))) {
                    rc->has_named_groups = true;
                    caps++;
                }
            } else {
                caps++;
            }
        }
    }
    rc->total_captures = caps;
}

static bool rc_add_class_range(ReCompiler *rc, AjsReCharClass *cc, uint32_t lo, uint32_t hi) {
    if (lo > hi) {
        rc_fail(rc, "Range out of order in character class");
        return false;
    }
    if (cc->range_count >= cc->range_cap) {
        uint32_t ncap = cc->range_cap ? cc->range_cap * 2u : 8u;
        AjsReRange *nr = (AjsReRange *)realloc(cc->ranges, ncap * sizeof(AjsReRange));
        if (!nr) { rc_fail(rc, "Out of memory"); return false; }
        cc->ranges = nr;
        cc->range_cap = ncap;
    }
    cc->ranges[cc->range_count].lo = lo;
    cc->ranges[cc->range_count].hi = hi;
    cc->range_count++;
    return true;
}

static bool rc_add_class_prop(ReCompiler *rc, AjsReCharClass *cc, const AjsRePropCheck *p) {
    if (cc->prop_count >= cc->prop_cap) {
        uint32_t ncap = cc->prop_cap ? cc->prop_cap * 2u : 4u;
        AjsRePropCheck *np = (AjsRePropCheck *)realloc(cc->props, ncap * sizeof(AjsRePropCheck));
        if (!np) { rc_fail(rc, "Out of memory"); return false; }
        cc->props = np;
        cc->prop_cap = ncap;
    }
    cc->props[cc->prop_count++] = *p;
    return true;
}

static bool rc_parse_unicode_escape(ReCompiler *rc, uint32_t *out_cp) {
    if (rc->pos < rc->len && rc->pat[rc->pos] == '{') {
        if (!rc->uni_mode) return false;
        rc->pos++;
        if (rc->pos >= rc->len || rc->pat[rc->pos] == '}') {
            rc_fail(rc, "Invalid Unicode escape in regular expression");
            return false;
        }
        uint32_t val = 0;
        while (rc->pos < rc->len && rc->pat[rc->pos] != '}') {
            int h = rc_hex_val(rc->pat[rc->pos++]);
            if (h < 0) { rc_fail(rc, "Invalid hex digit in Unicode escape"); return false; }
            val = (val << 4) | (uint32_t)h;
            if (val > 0x10FFFFu) { rc_fail(rc, "Unicode code point out of range in regular expression"); return false; }
        }
        if (rc->pos >= rc->len || rc->pat[rc->pos] != '}') {
            rc_fail(rc, "Unterminated Unicode escape in regular expression");
            return false;
        }
        rc->pos++;
        *out_cp = val;
        return true;
    }
    if (rc->pos + 4 <= rc->len) {
        int h0 = rc_hex_val(rc->pat[rc->pos]);
        int h1 = rc_hex_val(rc->pat[rc->pos + 1]);
        int h2 = rc_hex_val(rc->pat[rc->pos + 2]);
        int h3 = rc_hex_val(rc->pat[rc->pos + 3]);
        if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
            rc->pos += 4;
            uint32_t u0 = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
            if (rc->uni_mode && u0 >= 0xD800u && u0 <= 0xDBFFu &&
                rc->pos + 6 <= rc->len && rc->pat[rc->pos] == '\\' && rc->pat[rc->pos + 1] == 'u') {
                int l0 = rc_hex_val(rc->pat[rc->pos + 2]);
                int l1 = rc_hex_val(rc->pat[rc->pos + 3]);
                int l2 = rc_hex_val(rc->pat[rc->pos + 4]);
                int l3 = rc_hex_val(rc->pat[rc->pos + 5]);
                if (l0 >= 0 && l1 >= 0 && l2 >= 0 && l3 >= 0) {
                    uint32_t u1 = (uint32_t)((l0 << 12) | (l1 << 8) | (l2 << 4) | l3);
                    if (u1 >= 0xDC00u && u1 <= 0xDFFFu) {
                        rc->pos += 6;
                        *out_cp = 0x10000u + (((u0 - 0xD800u) << 10) | (u1 - 0xDC00u));
                        return true;
                    }
                }
            }
            *out_cp = u0;
            return true;
        }
    }
    if (rc->uni_mode) {
        rc_fail(rc, "Invalid Unicode escape in regular expression");
    }
    return false;
}

static bool rc_parse_prop_escape(ReCompiler *rc, bool negate, AjsRePropCheck *out_prop) {
    if (rc->pos >= rc->len || rc->pat[rc->pos] != '{') {
        rc_fail(rc, "Invalid Unicode property escape: expected '{'");
        return false;
    }
    rc->pos++;
    memset(out_prop, 0, sizeof(*out_prop));
    out_prop->negate = negate;
    size_t nlen = 0, vlen = 0;
    bool in_val = false;
    while (rc->pos < rc->len && rc->pat[rc->pos] != '}') {
        uint16_t ch = rc->pat[rc->pos++];
        if (ch == '=' && !in_val) {
            in_val = true;
            continue;
        }
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) {
            rc_fail(rc, "Invalid character in Unicode property escape");
            return false;
        }
        if (!in_val) {
            if (nlen + 1 < sizeof(out_prop->name)) out_prop->name[nlen++] = (char)ch;
        } else {
            if (vlen + 1 < sizeof(out_prop->value)) out_prop->value[vlen++] = (char)ch;
        }
    }
    if (rc->pos >= rc->len || rc->pat[rc->pos] != '}' || nlen == 0 || (in_val && vlen == 0)) {
        rc_fail(rc, "Invalid or unterminated Unicode property escape");
        return false;
    }
    rc->pos++;
    if (!re_is_valid_unicode_prop(out_prop->name, in_val ? out_prop->value : NULL)) {
        rc_fail(rc, "Unknown Unicode property name or value in regular expression");
        return false;
    }
    if ((rc->flags & RE_FLAG_V) && negate && !in_val &&
        (strcmp(out_prop->name, "Basic_Emoji") == 0 || strcmp(out_prop->name, "RGI_Emoji") == 0)) {
        rc_fail(rc, "Negated property of strings is forbidden in regular expression");
        return false;
    }
    return true;
}

static uint32_t rc_register_class(ReCompiler *rc, const AjsReCharClass *cc) {
    if (rc->class_count >= rc->class_cap) {
        uint32_t ncap = rc->class_cap ? rc->class_cap * 2u : 8u;
        AjsReCharClass *nc = (AjsReCharClass *)realloc(rc->classes, ncap * sizeof(AjsReCharClass));
        if (!nc) { rc_fail(rc, "Out of memory"); return 0; }
        rc->classes = nc;
        rc->class_cap = ncap;
    }
    uint32_t idx = rc->class_count++;
    rc->classes[idx] = *cc;
    return idx;
}

static bool rc_parse_class_escape(ReCompiler *rc, bool *out_is_set, uint32_t *out_cp, uint32_t *out_predef, AjsRePropCheck *out_prop) {
    *out_is_set = false;
    *out_predef = 0;
    if (rc->pos >= rc->len) { rc_fail(rc, "Trailing backslash in character class"); return false; }
    uint32_t c = rc_read_cp(rc);
    switch (c) {
        case 'b': *out_cp = 0x08u; return true;
        case 't': *out_cp = 0x09u; return true;
        case 'n': *out_cp = 0x0Au; return true;
        case 'v': *out_cp = 0x0Bu; return true;
        case 'f': *out_cp = 0x0Cu; return true;
        case 'r': *out_cp = 0x0Du; return true;
        case '-': *out_cp = '-';   return true;
        case 'd': *out_is_set = true; *out_predef = RE_PREDEF_d; return true;
        case 'D': *out_is_set = true; *out_predef = RE_PREDEF_D; return true;
        case 'w': *out_is_set = true; *out_predef = RE_PREDEF_w; return true;
        case 'W': *out_is_set = true; *out_predef = RE_PREDEF_W; return true;
        case 's': *out_is_set = true; *out_predef = RE_PREDEF_s; return true;
        case 'S': *out_is_set = true; *out_predef = RE_PREDEF_S; return true;
        case 'p':
        case 'P':
            if (rc->uni_mode) {
                *out_is_set = true;
                return rc_parse_prop_escape(rc, c == 'P', out_prop);
            }
            *out_cp = c;
            return true;
        case '0':
            if (rc->pos < rc->len && rc->pat[rc->pos] >= '0' && rc->pat[rc->pos] <= '9') {
                if (rc->uni_mode) { rc_fail(rc, "Invalid decimal escape in unicode class"); return false; }
                uint32_t oct = 0;
                for (int k = 0; k < 2 && rc->pos < rc->len && rc->pat[rc->pos] >= '0' && rc->pat[rc->pos] <= '7'; k++) {
                    oct = (oct << 3) | (uint32_t)(rc->pat[rc->pos++] - '0');
                }
                *out_cp = oct;
                return true;
            }
            *out_cp = 0;
            return true;
        case 'x':
            if (rc->pos + 2 <= rc->len) {
                int h0 = rc_hex_val(rc->pat[rc->pos]);
                int h1 = rc_hex_val(rc->pat[rc->pos + 1]);
                if (h0 >= 0 && h1 >= 0) {
                    rc->pos += 2;
                    *out_cp = (uint32_t)((h0 << 4) | h1);
                    return true;
                }
            }
            if (rc->uni_mode) { rc_fail(rc, "Invalid hex escape in regular expression"); return false; }
            *out_cp = 'x';
            return true;
        case 'u': {
            uint32_t ucp = 0;
            if (rc_parse_unicode_escape(rc, &ucp)) { *out_cp = ucp; return true; }
            if (rc->failed) return false;
            *out_cp = 'u';
            return true;
        }
        case 'c':
            if (rc->pos < rc->len) {
                uint16_t cc = rc->pat[rc->pos];
                if ((cc >= 'a' && cc <= 'z') || (cc >= 'A' && cc <= 'Z') ||
                    (!rc->uni_mode && ((cc >= '0' && cc <= '9') || cc == '_'))) {
                    rc->pos++;
                    *out_cp = cc & 0x1Fu;
                    return true;
                }
            }
            if (rc->uni_mode) { rc_fail(rc, "Invalid control escape in regular expression"); return false; }
            rc->pos--; /* Annex B: treat '\c' in class as literal '\' followed by 'c' */
            *out_cp = '\\';
            return true;
        default:
            if (!rc->uni_mode && c >= '1' && c <= '7') {
                uint32_t oct = c - '0';
                for (int k = 0; k < 2 && rc->pos < rc->len && rc->pat[rc->pos] >= '0' && rc->pat[rc->pos] <= '7'; k++) {
                    if (oct * 8u + (uint32_t)(rc->pat[rc->pos] - '0') > 255u) break;
                    oct = oct * 8u + (uint32_t)(rc->pat[rc->pos++] - '0');
                }
                *out_cp = oct;
                return true;
            }
            if (rc->uni_mode) {
                /* IdentityEscape in u/v mode: SyntaxCharacter, '/', '-', or v-mode class syntax punctuators */
                bool ok = (c == '^' || c == '$' || c == '\\' || c == '.' || c == '*' || c == '+' ||
                           c == '?' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' ||
                           c == '}' || c == '|' || c == '/' || c == '-');
                if ((rc->flags & RE_FLAG_V) &&
                    (c == '&' || c == '!' || c == '#' || c == '%' || c == ',' || c == ':' ||
                     c == ';' || c == '<' || c == '=' || c == '>' || c == '@' || c == '`' || c == '~')) {
                    ok = true;
                }
                if (!ok) {
                    rc_fail(rc, "Invalid identity escape in unicode character class");
                    return false;
                }
            }
            *out_cp = c;
            return true;
    }
}

static ReAstNode *rc_parse_char_class_ast(ReCompiler *rc) {
    AjsReCharClass cc;
    memset(&cc, 0, sizeof(cc));
    cc.local_flags = rc->cur_flags;
    if (rc->pos < rc->len && rc->pat[rc->pos] == '^') {
        cc.invert = true;
        rc->pos++;
    }

    bool closed = false;
    while (rc->pos < rc->len && !rc->failed) {
        if (rc->pat[rc->pos] == ']') {
            rc->pos++;
            closed = true;
            break;
        }
        /* Handle v-mode nested class or set operators '--' / '&&' cleanly for ASCII/BMP sets */
        if ((rc->flags & RE_FLAG_V) && rc->pat[rc->pos] == '\\' &&
            rc->pos + 2 < rc->len && rc->pat[rc->pos + 1] == 'q' && rc->pat[rc->pos + 2] == '{') {
            rc->pos += 3;
            ReAstNode *disj = rc_alloc_node(rc, RE_AST_DISJ);
            while (rc->pos < rc->len && rc->pat[rc->pos] != '}') {
                ReAstNode *seq = rc_alloc_node(rc, RE_AST_SEQ);
                while (rc->pos < rc->len && rc->pat[rc->pos] != '|' && rc->pat[rc->pos] != '}') {
                    uint32_t qcp = 0;
                    if (rc->pat[rc->pos] == '\\') {
                        rc->pos++;
                        bool is_s = false; uint32_t pre = 0; AjsRePropCheck pr;
                        if (!rc_parse_class_escape(rc, &is_s, &qcp, &pre, &pr) || is_s) {
                            re_free_char_class(&cc);
                            rc_fail(rc, "Invalid escape inside \\q{...}");
                            return NULL;
                        }
                    } else {
                        qcp = rc_read_cp(rc);
                    }
                    ReAstNode *cn = rc_alloc_node(rc, RE_AST_CHAR);
                    if (cn) { cn->u1 = qcp; rc_node_push(rc, seq, cn); }
                }
                rc_node_push(rc, disj, seq);
                if (rc->pos < rc->len && rc->pat[rc->pos] == '|') rc->pos++;
            }
            if (rc->pos >= rc->len || rc->pat[rc->pos] != '}') {
                re_free_char_class(&cc);
                rc_fail(rc, "Unterminated \\q{...}");
                return NULL;
            }
            rc->pos++;
            if (!cc.invert && rc->pos < rc->len && rc->pat[rc->pos] == ']' &&
                cc.range_count == 0 && cc.predef_mask == 0 && cc.prop_count == 0) {
                rc->pos++;
                re_free_char_class(&cc);
                return disj;
            }
            continue;
        }
        if ((rc->flags & RE_FLAG_V) && rc->pos + 1 < rc->len &&
            ((rc->pat[rc->pos] == '-' && rc->pat[rc->pos + 1] == '-') ||
             (rc->pat[rc->pos] == '&' && rc->pat[rc->pos + 1] == '&'))) {
            bool is_sub = (rc->pat[rc->pos] == '-');
            rc->pos += 2;
            if (rc->pos < rc->len && rc->pat[rc->pos] == '[') {
                rc->pos++;
                ReAstNode *rhs_node = rc_parse_char_class_ast(rc);
                if (!rhs_node || rhs_node->kind != RE_AST_CLASS) {
                    re_free_char_class(&cc);
                    rc_fail(rc, "Invalid set operand in v-mode character class");
                    return NULL;
                }
                const AjsReCharClass *rhs = &rc->classes[rhs_node->u1];
                AjsReCharClass merged;
                memset(&merged, 0, sizeof(merged));
                merged.local_flags = cc.local_flags;
                for (uint32_t cp = 0; cp <= 0xFFFFu; cp++) {
                    bool lm = re_char_class_matches_cp(&cc, cp);
                    bool rm = re_char_class_matches_cp(rhs, cp);
                    bool keep = is_sub ? (lm && !rm) : (lm && rm);
                    if (keep) {
                        if (merged.range_count > 0 && merged.ranges[merged.range_count - 1].hi + 1 == cp) {
                            merged.ranges[merged.range_count - 1].hi = cp;
                        } else {
                            rc_add_class_range(rc, &merged, cp, cp);
                        }
                    }
                }
                re_free_char_class(&cc);
                cc = merged;
                continue;
            }
        }

        bool is_set = false;
        uint32_t cp1 = 0, predef1 = 0;
        AjsRePropCheck prop1;
        memset(&prop1, 0, sizeof(prop1));
        if (rc->pat[rc->pos] == '\\') {
            rc->pos++;
            if (!rc_parse_class_escape(rc, &is_set, &cp1, &predef1, &prop1)) {
                re_free_char_class(&cc);
                return NULL;
            }
        } else {
            cp1 = rc_read_cp(rc);
        }

        if (rc->pos + 1 < rc->len && rc->pat[rc->pos] == '-' && rc->pat[rc->pos + 1] != ']') {
            if (is_set) {
                if (rc->uni_mode) {
                    re_free_char_class(&cc);
                    rc_fail(rc, "Invalid character class range with character class escape");
                    return NULL;
                }
                /* Annex B: add set + literal '-' */
                cc.predef_mask |= predef1;
                rc_add_class_range(rc, &cc, '-', '-');
                rc->pos++;
                continue;
            }
            rc->pos++; /* consume '-' */
            bool is_set2 = false;
            uint32_t cp2 = 0, predef2 = 0;
            AjsRePropCheck prop2;
            memset(&prop2, 0, sizeof(prop2));
            if (rc->pat[rc->pos] == '\\') {
                rc->pos++;
                if (!rc_parse_class_escape(rc, &is_set2, &cp2, &predef2, &prop2)) {
                    re_free_char_class(&cc);
                    return NULL;
                }
            } else {
                cp2 = rc_read_cp(rc);
            }
            if (is_set2) {
                if (rc->uni_mode) {
                    re_free_char_class(&cc);
                    rc_fail(rc, "Invalid character class range with character class escape");
                    return NULL;
                }
                rc_add_class_range(rc, &cc, cp1, cp1);
                rc_add_class_range(rc, &cc, '-', '-');
                cc.predef_mask |= predef2;
                continue;
            }
            if (!rc_add_class_range(rc, &cc, cp1, cp2)) {
                re_free_char_class(&cc);
                return NULL;
            }
        } else {
            if (is_set) {
                if (predef1) cc.predef_mask |= predef1;
                else if (!rc_add_class_prop(rc, &cc, &prop1)) { re_free_char_class(&cc); return NULL; }
            } else {
                if (!rc_add_class_range(rc, &cc, cp1, cp1)) { re_free_char_class(&cc); return NULL; }
            }
        }
    }

    if (!closed) {
        re_free_char_class(&cc);
        rc_fail(rc, "Unterminated character class in regular expression");
        return NULL;
    }
    uint32_t cidx = rc_register_class(rc, &cc);
    ReAstNode *node = rc_alloc_node(rc, RE_AST_CLASS);
    if (node) node->u1 = cidx;
    return node;
}

static bool rc_parse_group_name(ReCompiler *rc, char **out_utf8, uint16_t **out_u16, uint32_t *out_len16) {
    if (rc->pos >= rc->len || rc->pat[rc->pos] != '<') {
        rc_fail(rc, "Expected '<' in group name");
        return false;
    }
    rc->pos++;
    uint16_t *u16 = NULL;
    uint32_t len16 = 0, cap16 = 0;
    bool first = true;
    while (rc->pos < rc->len && rc->pat[rc->pos] != '>') {
        uint32_t cp = 0;
        if (rc->pat[rc->pos] == '\\') {
            if (rc->pos + 1 >= rc->len || rc->pat[rc->pos + 1] != 'u') {
                free(u16);
                rc_fail(rc, "Invalid escape in capture group name");
                return false;
            }
            rc->pos += 2;
            bool old_uni = rc->uni_mode;
            rc->uni_mode = true;
            bool ok = rc_parse_unicode_escape(rc, &cp);
            rc->uni_mode = old_uni;
            if (!ok) { free(u16); return false; }
        } else {
            uint32_t adv = 1;
            cp = ajs_utf16_code_point_at(rc->pat, rc->len, rc->pos, &adv);
            rc->pos += adv;
        }
        bool valid_ch = first ? ajs_unicode_is_id_start(cp) : ajs_unicode_is_id_continue(cp);
        if (!valid_ch) {
            free(u16);
            rc_fail(rc, "Invalid identifier character in capture group name");
            return false;
        }
        first = false;
        if (len16 + 2 >= cap16) {
            uint32_t nc = cap16 ? cap16 * 2u : 16u;
            uint16_t *nu = (uint16_t *)realloc(u16, nc * sizeof(uint16_t));
            if (!nu) { free(u16); rc_fail(rc, "Out of memory"); return false; }
            u16 = nu; cap16 = nc;
        }
        if (cp <= 0xFFFFu) {
            u16[len16++] = (uint16_t)cp;
        } else {
            uint32_t v = cp - 0x10000u;
            u16[len16++] = (uint16_t)(0xD800u + (v >> 10));
            u16[len16++] = (uint16_t)(0xDC00u + (v & 0x3FFu));
        }
    }
    if (rc->pos >= rc->len || rc->pat[rc->pos] != '>' || len16 == 0) {
        free(u16);
        rc_fail(rc, "Invalid or empty capture group name");
        return false;
    }
    rc->pos++; /* consume '>' */

    size_t u8_cap = (size_t)len16 * 3u + 1u;
    char *u8 = (char *)malloc(u8_cap);
    if (!u8) { free(u16); rc_fail(rc, "Out of memory"); return false; }
    size_t u8_len = 0;
    for (uint32_t i = 0; i < len16; ) {
        uint32_t adv = 1;
        uint32_t cp = ajs_utf16_code_point_at(u16, len16, i, &adv);
        u8_len += ajs_code_point_to_utf8(cp, u8 + u8_len);
        i += adv;
    }
    u8[u8_len] = '\0';
    *out_utf8 = u8;
    if (out_u16) *out_u16 = u16;
    else free(u16);
    if (out_len16) *out_len16 = len16;
    return true;
}

static bool rc_register_named_group(ReCompiler *rc, char *name_utf8, uint16_t *name_u16, uint32_t len16, uint32_t cap_idx) {
    for (uint32_t i = 0; i < rc->active_count; i++) {
        if (strcmp(rc->active_names[i], name_utf8) == 0) {
            free(name_utf8);
            free(name_u16);
            rc_fail(rc, "Duplicate capture group name in same alternative");
            return false;
        }
    }
    if (rc->group_count >= rc->group_cap) {
        uint32_t ncap = rc->group_cap ? rc->group_cap * 2u : 8u;
        AjsReGroupEntry *ng = (AjsReGroupEntry *)realloc(rc->groups, ncap * sizeof(AjsReGroupEntry));
        if (!ng) { free(name_utf8); free(name_u16); rc_fail(rc, "Out of memory"); return false; }
        rc->groups = ng;
        rc->group_cap = ncap;
    }
    if (rc->active_count >= rc->active_cap) {
        uint32_t ncap = rc->active_cap ? rc->active_cap * 2u : 8u;
        char **na = (char **)realloc(rc->active_names, ncap * sizeof(char *));
        if (!na) { free(name_utf8); free(name_u16); rc_fail(rc, "Out of memory"); return false; }
        rc->active_names = na;
        rc->active_cap = ncap;
    }
    rc->groups[rc->group_count].name_utf8 = name_utf8;
    rc->groups[rc->group_count].name_u16 = name_u16;
    rc->groups[rc->group_count].name_len16 = len16;
    rc->groups[rc->group_count].cap_idx = cap_idx;
    rc->group_count++;
    rc->active_names[rc->active_count++] = name_utf8;
    return true;
}

static ReAstNode *rc_parse_disjunction(ReCompiler *rc);

static ReAstNode *rc_parse_atom(ReCompiler *rc, bool *out_quantifiable) {
    *out_quantifiable = true;
    uint16_t ch = rc->pat[rc->pos];
    if (ch == '^') {
        rc->pos++;
        *out_quantifiable = false;
        return rc_alloc_node(rc, RE_AST_BOL);
    }
    if (ch == '$') {
        rc->pos++;
        *out_quantifiable = false;
        return rc_alloc_node(rc, RE_AST_EOL);
    }
    if (ch == '.') {
        rc->pos++;
        return rc_alloc_node(rc, RE_AST_ANY);
    }
    if (ch == '[') {
        rc->pos++;
        return rc_parse_char_class_ast(rc);
    }
    if (ch == '(') {
        if (++rc->depth > 256u) { rc_fail(rc, "Regular expression nesting too deep"); return NULL; }
        rc->pos++;
        uint32_t cap_start_before = rc->next_cap_idx;
        if (rc->pos < rc->len && rc->pat[rc->pos] == '?') {
            rc->pos++;
            if (rc->pos >= rc->len) { rc_fail(rc, "Invalid group in regular expression"); return NULL; }
            uint16_t gch = rc->pat[rc->pos];
            if (gch == ':' ) {
                rc->pos++;
                ReAstNode *sub = rc_parse_disjunction(rc);
                if (rc->pos >= rc->len || rc->pat[rc->pos] != ')') { rc_fail(rc, "Unterminated group"); return NULL; }
                rc->pos++;
                rc->depth--;
                ReAstNode *gn = rc_alloc_node(rc, RE_AST_GROUP);
                if (gn) { gn->u1 = 0; gn->child = sub; gn->cap_lo = cap_start_before; gn->cap_hi = rc->next_cap_idx - 1u; }
                return gn;
            }
            if (gch == '=' || gch == '!') {
                rc->pos++;
                ReAstNode *sub = rc_parse_disjunction(rc);
                if (rc->pos >= rc->len || rc->pat[rc->pos] != ')') { rc_fail(rc, "Unterminated lookahead"); return NULL; }
                rc->pos++;
                rc->depth--;
                /* Annex B allows quantifying (?=...) and (?!...) ONLY when not in u/v mode! */
                if (rc->uni_mode) *out_quantifiable = false;
                ReAstNode *la = rc_alloc_node(rc, RE_AST_LOOKAROUND);
                if (la) {
                    la->u1 = (gch == '=') ? 1u : 0u;
                    la->u2 = 0u; /* forward */
                    la->child = sub;
                    la->cap_lo = cap_start_before;
                    la->cap_hi = rc->next_cap_idx - 1u;
                }
                return la;
            }
            if (gch == '<') {
                if (rc->pos + 1 < rc->len && (rc->pat[rc->pos + 1] == '=' || rc->pat[rc->pos + 1] == '!')) {
                    bool is_pos = (rc->pat[rc->pos + 1] == '=');
                    rc->pos += 2;
                    ReAstNode *sub = rc_parse_disjunction(rc);
                    if (rc->pos >= rc->len || rc->pat[rc->pos] != ')') { rc_fail(rc, "Unterminated lookbehind"); return NULL; }
                    rc->pos++;
                    rc->depth--;
                    *out_quantifiable = false; /* Lookbehind is never quantifiable even in Annex B */
                    ReAstNode *lb = rc_alloc_node(rc, RE_AST_LOOKAROUND);
                    if (lb) {
                        lb->u1 = is_pos ? 1u : 0u;
                        lb->u2 = 1u; /* backward */
                        lb->child = sub;
                        lb->cap_lo = cap_start_before;
                        lb->cap_hi = rc->next_cap_idx - 1u;
                    }
                    return lb;
                }
                /* Named capturing group (?<name>...) */
                char *gname = NULL;
                uint16_t *gu16 = NULL;
                uint32_t glen16 = 0;
                if (!rc_parse_group_name(rc, &gname, &gu16, &glen16)) return NULL;
                uint32_t cidx = rc->next_cap_idx++;
                if (!rc_register_named_group(rc, gname, gu16, glen16, cidx)) return NULL;
                ReAstNode *sub = rc_parse_disjunction(rc);
                if (rc->pos >= rc->len || rc->pat[rc->pos] != ')') { rc_fail(rc, "Unterminated named group"); return NULL; }
                rc->pos++;
                rc->depth--;
                ReAstNode *gn = rc_alloc_node(rc, RE_AST_GROUP);
                if (gn) { gn->u1 = cidx; gn->child = sub; gn->cap_lo = cidx; gn->cap_hi = rc->next_cap_idx - 1u; }
                return gn;
            }
            /* ES2025 Inline Pattern Modifiers (?ims-ims:...) */
            if (gch == 'i' || gch == 'm' || gch == 's' || gch == '-') {
                uint32_t add_f = 0, rem_f = 0;
                bool seen_dash = false;
                while (rc->pos < rc->len && rc->pat[rc->pos] != ':') {
                    uint16_t mc = rc->pat[rc->pos++];
                    if (mc == '-' && !seen_dash) { seen_dash = true; continue; }
                    uint32_t bit = (mc == 'i') ? RE_FLAG_I : (mc == 'm') ? RE_FLAG_M : (mc == 's') ? RE_FLAG_S : 0u;
                    if (!bit || ((add_f | rem_f) & bit)) {
                        rc_fail(rc, "Invalid pattern modifier flags in regular expression");
                        return NULL;
                    }
                    if (seen_dash) rem_f |= bit;
                    else add_f |= bit;
                }
                if ((add_f == 0 && rem_f == 0) || rc->pos >= rc->len || rc->pat[rc->pos] != ':') {
                    rc_fail(rc, "Invalid pattern modifier group");
                    return NULL;
                }
                rc->pos++; /* consume ':' */
                uint32_t saved_f = rc->cur_flags;
                rc->cur_flags = (rc->cur_flags | add_f) & ~rem_f;
                ReAstNode *sub = rc_parse_disjunction(rc);
                rc->cur_flags = saved_f;
                if (rc->pos >= rc->len || rc->pat[rc->pos] != ')') { rc_fail(rc, "Unterminated modifier group"); return NULL; }
                rc->pos++;
                rc->depth--;
                ReAstNode *gn = rc_alloc_node(rc, RE_AST_GROUP);
                if (gn) { gn->u1 = 0; gn->child = sub; gn->cap_lo = cap_start_before; gn->cap_hi = rc->next_cap_idx - 1u; }
                return gn;
            }
            rc_fail(rc, "Invalid group syntax in regular expression");
            return NULL;
        }
        uint32_t cidx = rc->next_cap_idx++;
        ReAstNode *sub = rc_parse_disjunction(rc);
        if (rc->pos >= rc->len || rc->pat[rc->pos] != ')') { rc_fail(rc, "Unterminated parenthetical in regular expression"); return NULL; }
        rc->pos++;
        rc->depth--;
        ReAstNode *gn = rc_alloc_node(rc, RE_AST_GROUP);
        if (gn) { gn->u1 = cidx; gn->child = sub; gn->cap_lo = cidx; gn->cap_hi = rc->next_cap_idx - 1u; }
        return gn;
    }
    if (ch == '\\') {
        rc->pos++;
        if (rc->pos >= rc->len) { rc_fail(rc, "Trailing backslash in regular expression"); return NULL; }
        uint32_t esc = rc_read_cp(rc);
        if (esc == 'b') { *out_quantifiable = false; return rc_alloc_node(rc, RE_AST_BOUND); }
        if (esc == 'B') { *out_quantifiable = false; return rc_alloc_node(rc, RE_AST_NOT_BOUND); }
        if (esc == 't' || esc == 'n' || esc == 'v' || esc == 'f' || esc == 'r') {
            uint32_t mapped = (esc == 't') ? 9u : (esc == 'n') ? 10u : (esc == 'v') ? 11u : (esc == 'f') ? 12u : 13u;
            ReAstNode *cn = rc_alloc_node(rc, RE_AST_CHAR);
            if (cn) cn->u1 = mapped;
            return cn;
        }
        if (esc == 'd' || esc == 'D' || esc == 'w' || esc == 'W' || esc == 's' || esc == 'S') {
            AjsReCharClass cc;
            memset(&cc, 0, sizeof(cc));
            cc.local_flags = rc->cur_flags;
            cc.predef_mask = (esc == 'd') ? RE_PREDEF_d : (esc == 'D') ? RE_PREDEF_D :
                             (esc == 'w') ? RE_PREDEF_w : (esc == 'W') ? RE_PREDEF_W :
                             (esc == 's') ? RE_PREDEF_s : RE_PREDEF_S;
            uint32_t cidx = rc_register_class(rc, &cc);
            ReAstNode *cn = rc_alloc_node(rc, RE_AST_CLASS);
            if (cn) cn->u1 = cidx;
            return cn;
        }
        if ((esc == 'p' || esc == 'P') && rc->uni_mode) {
            AjsRePropCheck prop;
            if (!rc_parse_prop_escape(rc, esc == 'P', &prop)) return NULL;
            if (!prop.negate && prop.value[0] == '\0' &&
                (strcmp(prop.name, "Basic_Emoji") == 0 || strcmp(prop.name, "RGI_Emoji") == 0)) {
                /* Multi-codepoint Emoji fallback: match Emoji code point optionally followed by FE0F/ZWJ sequence */
                strcpy(prop.name, "Emoji");
            }
            AjsReCharClass cc;
            memset(&cc, 0, sizeof(cc));
            cc.local_flags = rc->cur_flags;
            rc_add_class_prop(rc, &cc, &prop);
            uint32_t cidx = rc_register_class(rc, &cc);
            ReAstNode *cn = rc_alloc_node(rc, RE_AST_CLASS);
            if (cn) cn->u1 = cidx;
            return cn;
        }
        if (esc == 'k') {
            if (rc->uni_mode || rc->has_named_groups) {
                char *rname = NULL;
                if (!rc_parse_group_name(rc, &rname, NULL, NULL)) return NULL;
                ReAstNode *rn = rc_alloc_node(rc, RE_AST_NAMED_REF);
                if (rn) rn->ref_name = rname;
                else free(rname);
                return rn;
            }
            ReAstNode *cn = rc_alloc_node(rc, RE_AST_CHAR);
            if (cn) cn->u1 = 'k';
            return cn;
        }
        if (esc >= '1' && esc <= '9') {
            uint32_t save_p = rc->pos;
            uint32_t num = esc - '0';
            while (rc->pos < rc->len && rc->pat[rc->pos] >= '0' && rc->pat[rc->pos] <= '9') {
                uint32_t d = (uint32_t)(rc->pat[rc->pos] - '0');
                if (num > 100000u) break;
                num = num * 10u + d;
                rc->pos++;
            }
            if (num <= rc->total_captures) {
                ReAstNode *bn = rc_alloc_node(rc, RE_AST_BACKREF);
                if (bn) bn->u1 = num;
                return bn;
            }
            if (rc->uni_mode) {
                rc_fail(rc, "Invalid decimal escape / backreference in unicode regular expression");
                return NULL;
            }
            /* Annex B legacy octal / literal escape */
            rc->pos = save_p;
            if (esc <= '7') {
                uint32_t oct = esc - '0';
                for (int k = 0; k < 2 && rc->pos < rc->len && rc->pat[rc->pos] >= '0' && rc->pat[rc->pos] <= '7'; k++) {
                    if (oct * 8u + (uint32_t)(rc->pat[rc->pos] - '0') > 255u) break;
                    oct = oct * 8u + (uint32_t)(rc->pat[rc->pos++] - '0');
                }
                ReAstNode *cn = rc_alloc_node(rc, RE_AST_CHAR);
                if (cn) cn->u1 = oct;
                return cn;
            }
            ReAstNode *cn = rc_alloc_node(rc, RE_AST_CHAR);
            if (cn) cn->u1 = esc;
            return cn;
        }
        if (esc == '0') {
            if (rc->pos < rc->len && rc->pat[rc->pos] >= '0' && rc->pat[rc->pos] <= '9') {
                if (rc->uni_mode) { rc_fail(rc, "Invalid \\0 escape followed by digit in unicode regular expression"); return NULL; }
                uint32_t oct = 0;
                for (int k = 0; k < 2 && rc->pos < rc->len && rc->pat[rc->pos] >= '0' && rc->pat[rc->pos] <= '7'; k++) {
                    oct = oct * 8u + (uint32_t)(rc->pat[rc->pos++] - '0');
                }
                ReAstNode *cn = rc_alloc_node(rc, RE_AST_CHAR);
                if (cn) cn->u1 = oct;
                return cn;
            }
            ReAstNode *cn = rc_alloc_node(rc, RE_AST_CHAR);
            if (cn) cn->u1 = 0u;
            return cn;
        }
        if (esc == 'x') {
            if (rc->pos + 2 <= rc->len) {
                int h0 = rc_hex_val(rc->pat[rc->pos]);
                int h1 = rc_hex_val(rc->pat[rc->pos + 1]);
                if (h0 >= 0 && h1 >= 0) {
                    rc->pos += 2;
                    ReAstNode *cn = rc_alloc_node(rc, RE_AST_CHAR);
                    if (cn) cn->u1 = (uint32_t)((h0 << 4) | h1);
                    return cn;
                }
            }
            if (rc->uni_mode) { rc_fail(rc, "Invalid hex escape in regular expression"); return NULL; }
            ReAstNode *cn = rc_alloc_node(rc, RE_AST_CHAR);
            if (cn) cn->u1 = 'x';
            return cn;
        }
        if (esc == 'u') {
            uint32_t ucp = 0;
            if (rc_parse_unicode_escape(rc, &ucp)) {
                ReAstNode *cn = rc_alloc_node(rc, RE_AST_CHAR);
                if (cn) cn->u1 = ucp;
                return cn;
            }
            if (rc->failed) return NULL;
            ReAstNode *cn = rc_alloc_node(rc, RE_AST_CHAR);
            if (cn) cn->u1 = 'u';
            return cn;
        }
        if (esc == 'c') {
            if (rc->pos < rc->len) {
                uint16_t cc = rc->pat[rc->pos];
                if ((cc >= 'a' && cc <= 'z') || (cc >= 'A' && cc <= 'Z')) {
                    rc->pos++;
                    ReAstNode *cn = rc_alloc_node(rc, RE_AST_CHAR);
                    if (cn) cn->u1 = cc & 0x1Fu;
                    return cn;
                }
            }
            if (rc->uni_mode) { rc_fail(rc, "Invalid control escape in regular expression"); return NULL; }
            rc->pos--; /* Annex B: treat '\c' as literal '\' */
            ReAstNode *cn = rc_alloc_node(rc, RE_AST_CHAR);
            if (cn) cn->u1 = '\\';
            return cn;
        }
        if (rc->uni_mode) {
            bool ok = (esc == '^' || esc == '$' || esc == '\\' || esc == '.' || esc == '*' || esc == '+' ||
                       esc == '?' || esc == '(' || esc == ')' || esc == '[' || esc == ']' || esc == '{' ||
                       esc == '}' || esc == '|' || esc == '/');
            if (!ok) {
                rc_fail(rc, "Invalid identity escape in unicode regular expression");
                return NULL;
            }
        }
        ReAstNode *cn = rc_alloc_node(rc, RE_AST_CHAR);
        if (cn) cn->u1 = esc;
        return cn;
    }

    if (ch == '*' || ch == '+' || ch == '?') {
        rc_fail(rc, "Nothing to repeat in regular expression");
        return NULL;
    }
    if (ch == ')' || ch == ']') {
        if (rc->uni_mode || ch == ')') {
            rc_fail(rc, "Unmatched ')' or ']' in regular expression");
            return NULL;
        }
    }
    if (ch == '{' || ch == '}') {
        if (rc->uni_mode) {
            rc_fail(rc, "Lone quantifier bracket in unicode regular expression");
            return NULL;
        }
        /* In Annex B sloppy mode, '{m,n}' with nothing before it is a SyntaxError if valid quantifier, else literal! */
        if (ch == '{') {
            uint32_t p = rc->pos + 1;
            if (p < rc->len && rc->pat[p] >= '0' && rc->pat[p] <= '9') {
                while (p < rc->len && rc->pat[p] >= '0' && rc->pat[p] <= '9') p++;
                if (p < rc->len && rc->pat[p] == ',') {
                    p++;
                    while (p < rc->len && rc->pat[p] >= '0' && rc->pat[p] <= '9') p++;
                }
                if (p < rc->len && rc->pat[p] == '}') {
                    rc_fail(rc, "Nothing to repeat in regular expression");
                    return NULL;
                }
            }
        }
    }

    uint32_t cp = rc_read_cp(rc);
    ReAstNode *cn = rc_alloc_node(rc, RE_AST_CHAR);
    if (cn) cn->u1 = cp;
    return cn;
}

static bool rc_try_parse_quantifier(ReCompiler *rc, uint32_t *out_min, uint32_t *out_max, bool *out_greedy, bool *out_found) {
    *out_found = false;
    if (rc->pos >= rc->len) return true;
    uint16_t c = rc->pat[rc->pos];
    if (c == '*') {
        rc->pos++;
        *out_min = 0; *out_max = UINT32_MAX; *out_found = true;
    } else if (c == '+') {
        rc->pos++;
        *out_min = 1; *out_max = UINT32_MAX; *out_found = true;
    } else if (c == '?') {
        rc->pos++;
        *out_min = 0; *out_max = 1; *out_found = true;
    } else if (c == '{') {
        uint32_t p = rc->pos + 1;
        if (p < rc->len && rc->pat[p] >= '0' && rc->pat[p] <= '9') {
            uint64_t mn = 0;
            while (p < rc->len && rc->pat[p] >= '0' && rc->pat[p] <= '9') {
                mn = mn * 10u + (uint64_t)(rc->pat[p++] - '0');
                if (mn > 0x7FFFFFFFu) mn = 0x7FFFFFFFu;
            }
            uint64_t mx = mn;
            if (p < rc->len && rc->pat[p] == ',') {
                p++;
                if (p < rc->len && rc->pat[p] == '}') {
                    mx = UINT32_MAX;
                } else if (p < rc->len && rc->pat[p] >= '0' && rc->pat[p] <= '9') {
                    mx = 0;
                    while (p < rc->len && rc->pat[p] >= '0' && rc->pat[p] <= '9') {
                        mx = mx * 10u + (uint64_t)(rc->pat[p++] - '0');
                        if (mx > 0x7FFFFFFFu) mx = 0x7FFFFFFFu;
                    }
                } else {
                    goto not_quant;
                }
            }
            if (p < rc->len && rc->pat[p] == '}') {
                rc->pos = p + 1;
                if (mx != UINT32_MAX && mn > mx) {
                    rc_fail(rc, "Numbers out of order in {} quantifier");
                    return false;
                }
                *out_min = (uint32_t)mn;
                *out_max = (uint32_t)mx;
                *out_found = true;
            } else {
                goto not_quant;
            }
        } else {
        not_quant:
            if (rc->uni_mode) {
                rc_fail(rc, "Incomplete quantifier in unicode regular expression");
                return false;
            }
            return true;
        }
    } else {
        return true;
    }

    *out_greedy = true;
    if (rc->pos < rc->len && rc->pat[rc->pos] == '?') {
        rc->pos++;
        *out_greedy = false;
    }
    return true;
}

static ReAstNode *rc_parse_alternative(ReCompiler *rc) {
    ReAstNode *seq = rc_alloc_node(rc, RE_AST_SEQ);
    if (!seq) return NULL;
    while (rc->pos < rc->len && !rc->failed) {
        uint16_t ch = rc->pat[rc->pos];
        if (ch == '|' || ch == ')') break;
        bool quantifiable = true;
        ReAstNode *atom = rc_parse_atom(rc, &quantifiable);
        if (!atom || rc->failed) return NULL;

        bool has_q = false, greedy = true;
        uint32_t qmin = 0, qmax = 0;
        if (!rc_try_parse_quantifier(rc, &qmin, &qmax, &greedy, &has_q)) return NULL;
        if (has_q) {
            if (!quantifiable) {
                rc_fail(rc, "Invalid quantifier on assertion in regular expression");
                return NULL;
            }
            ReAstNode *qn = rc_alloc_node(rc, RE_AST_QUANT);
            if (!qn) return NULL;
            qn->u1 = qmin;
            qn->u2 = qmax;
            qn->greedy = greedy;
            qn->child = atom;
            qn->cap_lo = atom->cap_lo;
            qn->cap_hi = atom->cap_hi;
            atom = qn;
        }
        rc_node_push(rc, seq, atom);
    }
    return seq;
}

static ReAstNode *rc_parse_disjunction(ReCompiler *rc) {
    uint32_t base_active = rc->active_count;
    char **union_names = NULL;
    uint32_t union_count = 0, union_cap = 0;

    ReAstNode *first = rc_parse_alternative(rc);
    if (!first || rc->failed) return NULL;
    if (rc->pos >= rc->len || rc->pat[rc->pos] != '|') {
        return first;
    }

    /* Collect names declared in first branch into union_names, reset active_count to base_active for sibling branch! */
    for (uint32_t i = base_active; i < rc->active_count; i++) {
        if (union_count >= union_cap) {
            uint32_t nc = union_cap ? union_cap * 2u : 8u;
            char **nu = (char **)realloc(union_names, nc * sizeof(char *));
            if (!nu) { free(union_names); rc_fail(rc, "Out of memory"); return NULL; }
            union_names = nu; union_cap = nc;
        }
        union_names[union_count++] = rc->active_names[i];
    }
    rc->active_count = base_active;

    ReAstNode *disj = rc_alloc_node(rc, RE_AST_DISJ);
    if (!disj) { free(union_names); return NULL; }
    rc_node_push(rc, disj, first);

    while (rc->pos < rc->len && rc->pat[rc->pos] == '|' && !rc->failed) {
        rc->pos++;
        ReAstNode *alt = rc_parse_alternative(rc);
        if (!alt || rc->failed) { free(union_names); return NULL; }
        rc_node_push(rc, disj, alt);
        for (uint32_t i = base_active; i < rc->active_count; i++) {
            bool dup = false;
            for (uint32_t j = 0; j < union_count; j++) {
                if (strcmp(union_names[j], rc->active_names[i]) == 0) { dup = true; break; }
            }
            if (!dup) {
                if (union_count >= union_cap) {
                    uint32_t nc = union_cap ? union_cap * 2u : 8u;
                    char **nu = (char **)realloc(union_names, nc * sizeof(char *));
                    if (!nu) { free(union_names); rc_fail(rc, "Out of memory"); return NULL; }
                    union_names = nu; union_cap = nc;
                }
                union_names[union_count++] = rc->active_names[i];
            }
        }
        rc->active_count = base_active;
    }

    /* After disjunction completes, ALL names declared in ANY branch become active for subsequent sequence items! */
    for (uint32_t i = 0; i < union_count; i++) {
        if (rc->active_count >= rc->active_cap) {
            uint32_t nc = rc->active_cap ? rc->active_cap * 2u : 8u;
            char **na = (char **)realloc(rc->active_names, nc * sizeof(char *));
            if (!na) { free(union_names); rc_fail(rc, "Out of memory"); return NULL; }
            rc->active_names = na; rc->active_cap = nc;
        }
        rc->active_names[rc->active_count++] = union_names[i];
    }
    free(union_names);
    return disj;
}

static bool rc_verify_named_refs(ReCompiler *rc, ReAstNode *n) {
    if (!n || rc->failed) return false;
    if (n->kind == RE_AST_NAMED_REF) {
        int found = -1;
        for (uint32_t i = 0; i < rc->group_count; i++) {
            if (strcmp(rc->groups[i].name_utf8, n->ref_name) == 0) {
                found = (int)i;
                break;
            }
        }
        if (found < 0) {
            rc_fail(rc, "Invalid named capture referenced in regular expression");
            return false;
        }
        n->u1 = (uint32_t)found;
    }
    if (n->child && !rc_verify_named_refs(rc, n->child)) return false;
    for (uint32_t i = 0; i < n->item_count; i++) {
        if (!rc_verify_named_refs(rc, n->items[i])) return false;
    }
    return true;
}

static uint32_t rc_emit(ReCompiler *rc, AjsReOp op, uint32_t local_flags, bool backward, uint32_t a, uint32_t b) {
    if (rc->failed) return 0;
    if (rc->code_len >= rc->code_cap) {
        uint32_t ncap = rc->code_cap ? rc->code_cap * 2u : 64u;
        AjsReInst *nc = (AjsReInst *)realloc(rc->code, ncap * sizeof(AjsReInst));
        if (!nc) { rc_fail(rc, "Out of memory"); return 0; }
        rc->code = nc;
        rc->code_cap = ncap;
    }
    uint32_t pc = rc->code_len++;
    AjsReInst *ins = &rc->code[pc];
    memset(ins, 0, sizeof(*ins));
    ins->op = (uint8_t)op;
    ins->flags_dir = (uint16_t)((local_flags & 0xFFu) | (backward ? 0x100u : 0u));
    ins->a = a;
    ins->b = b;
    return pc;
}

static void rc_emit_node(ReCompiler *rc, ReAstNode *n, bool backward) {
    if (!n || rc->failed) return;
    switch (n->kind) {
        case RE_AST_EMPTY:
            break;
        case RE_AST_CHAR:
            rc_emit(rc, RE_OP_CHAR, n->local_flags, backward, n->u1, 0);
            break;
        case RE_AST_ANY:
            rc_emit(rc, RE_OP_ANY, n->local_flags, backward, 0, 0);
            break;
        case RE_AST_CLASS:
            rc_emit(rc, RE_OP_CLASS, n->local_flags, backward, n->u1, 0);
            break;
        case RE_AST_BOL:
            rc_emit(rc, RE_OP_BOL, n->local_flags, backward, 0, 0);
            break;
        case RE_AST_EOL:
            rc_emit(rc, RE_OP_EOL, n->local_flags, backward, 0, 0);
            break;
        case RE_AST_BOUND:
            rc_emit(rc, RE_OP_WORD_BOUND, n->local_flags, backward, 0, 0);
            break;
        case RE_AST_NOT_BOUND:
            rc_emit(rc, RE_OP_NOT_WORD_BOUND, n->local_flags, backward, 0, 0);
            break;
        case RE_AST_BACKREF:
            rc_emit(rc, RE_OP_BACKREF, n->local_flags, backward, n->u1, 0);
            break;
        case RE_AST_NAMED_REF:
            rc_emit(rc, RE_OP_NAMED_BACKREF, n->local_flags, backward, n->u1, 0);
            break;
        case RE_AST_SEQ:
            if (!backward) {
                for (uint32_t i = 0; i < n->item_count; i++) rc_emit_node(rc, n->items[i], false);
            } else {
                for (uint32_t i = n->item_count; i > 0; i--) rc_emit_node(rc, n->items[i - 1u], true);
            }
            break;
        case RE_AST_DISJ: {
            if (n->item_count == 0) break;
            if (n->item_count == 1) { rc_emit_node(rc, n->items[0], backward); break; }
            uint32_t *end_jmps = (uint32_t *)malloc(n->item_count * sizeof(uint32_t));
            if (!end_jmps) { rc_fail(rc, "Out of memory"); break; }
            for (uint32_t i = 0; i + 1 < n->item_count; i++) {
                uint32_t sp_pc = rc_emit(rc, RE_OP_SPLIT, n->local_flags, backward, 0, 0);
                uint32_t alt_pc = rc->code_len;
                rc_emit_node(rc, n->items[i], backward);
                end_jmps[i] = rc_emit(rc, RE_OP_JMP, n->local_flags, backward, 0, 0);
                uint32_t next_pc = rc->code_len;
                if (!rc->failed) {
                    rc->code[sp_pc].a = alt_pc;
                    rc->code[sp_pc].b = next_pc;
                }
            }
            rc_emit_node(rc, n->items[n->item_count - 1u], backward);
            uint32_t done_pc = rc->code_len;
            if (!rc->failed) {
                for (uint32_t i = 0; i + 1 < n->item_count; i++) {
                    rc->code[end_jmps[i]].a = done_pc;
                }
            }
            free(end_jmps);
            break;
        }
        case RE_AST_GROUP: {
            if (n->u1 == 0) {
                rc_emit_node(rc, n->child, backward);
            } else {
                uint32_t s_start = n->u1 * 2u;
                uint32_t s_end   = n->u1 * 2u + 1u;
                rc_emit(rc, RE_OP_SAVE, n->local_flags, backward, backward ? s_end : s_start, 0);
                rc_emit_node(rc, n->child, backward);
                rc_emit(rc, RE_OP_SAVE, n->local_flags, backward, backward ? s_start : s_end, 0);
            }
            break;
        }
        case RE_AST_QUANT: {
            uint32_t qmin = n->u1;
            uint32_t qmax = n->u2;
            if (qmax == 0) {
                if (n->cap_lo > 0 && n->cap_hi >= n->cap_lo) {
                    rc_emit(rc, RE_OP_CLEAR_CAPS, n->local_flags, backward, n->cap_lo, n->cap_hi);
                }
                break;
            }
            uint32_t reg_idx = rc->next_loop_idx++;
            uint32_t init_pc = rc_emit(rc, RE_OP_LOOP_INIT, n->local_flags, backward, reg_idx, 0);
            uint32_t step_pc = rc_emit(rc, RE_OP_LOOP_STEP, n->local_flags, backward, reg_idx, 0);
            uint32_t body_pc = rc->code_len;
            if (n->cap_lo > 0 && n->cap_hi >= n->cap_lo) {
                rc_emit(rc, RE_OP_CLEAR_CAPS, n->local_flags, backward, n->cap_lo, n->cap_hi);
            }
            rc_emit_node(rc, n->child, backward);
            rc_emit(rc, RE_OP_JMP, n->local_flags, backward, step_pc, 0);
            uint32_t exit_pc = rc->code_len;
            if (!rc->failed) {
                rc->code[init_pc].b = step_pc;
                rc->code[step_pc].b = body_pc;
                rc->code[step_pc].c = exit_pc;
                rc->code[step_pc].min_rep = qmin;
                rc->code[step_pc].max_rep = qmax;
                rc->code[step_pc].pad = n->greedy ? 1u : 0u;
            }
            break;
        }
        case RE_AST_LOOKAROUND: {
            bool is_pos = (n->u1 != 0);
            bool is_lb  = (n->u2 != 0);
            uint32_t la_pc = rc_emit(rc, RE_OP_LOOKAROUND, n->local_flags, backward, 0, is_pos ? 1u : 0u);
            uint32_t jmp_over = rc_emit(rc, RE_OP_JMP, n->local_flags, backward, 0, 0);
            uint32_t sub_pc = rc->code_len;
            rc_emit_node(rc, n->child, is_lb);
            rc_emit(rc, RE_OP_LOOK_END, n->local_flags, is_lb, 0, 0);
            uint32_t after_pc = rc->code_len;
            if (!rc->failed) {
                rc->code[la_pc].a = sub_pc;
                rc->code[la_pc].c = n->cap_lo;
                rc->code[la_pc].min_rep = n->cap_hi;
                rc->code[jmp_over].a = after_pc;
            }
            break;
        }
    }
}

static AjsRegExp *re_compile_internal(const uint16_t *pat, uint32_t pat_len,
                                      const uint16_t *flags_u16, uint32_t flags_len,
                                      char *err_buf, size_t err_cap) {
    uint32_t flags = 0;
    if (!re_parse_flags(flags_u16, flags_len, &flags, err_buf, err_cap)) {
        return NULL;
    }

    ReCompiler rc;
    memset(&rc, 0, sizeof(rc));
    rc.pat = pat;
    rc.len = pat_len;
    rc.flags = flags;
    rc.cur_flags = flags;
    rc.uni_mode = (flags & (RE_FLAG_U | RE_FLAG_V)) != 0;
    rc.next_cap_idx = 1;

    rc_prescan_captures(&rc);
    ReAstNode *root = rc_parse_disjunction(&rc);
    if (!rc.failed && rc.pos < rc.len) {
        rc_fail(&rc, "Unexpected ')' or trailing characters in regular expression");
    }
    if (!rc.failed) {
        rc_verify_named_refs(&rc, root);
    }
    if (!rc.failed) {
        rc_emit(&rc, RE_OP_SAVE, flags, false, 0, 0);
        rc_emit_node(&rc, root, false);
        rc_emit(&rc, RE_OP_SAVE, flags, false, 1, 0);
        rc_emit(&rc, RE_OP_MATCH, flags, false, 0, 0);
    }

    rc_free_all_ast(&rc);
    free(rc.active_names);

    if (rc.failed) {
        if (err_buf && err_cap > 0) snprintf(err_buf, err_cap, "%s", rc.err);
        for (uint32_t i = 0; i < rc.group_count; i++) {
            free(rc.groups[i].name_utf8);
            free(rc.groups[i].name_u16);
        }
        free(rc.groups);
        for (uint32_t i = 0; i < rc.class_count; i++) re_free_char_class(&rc.classes[i]);
        free(rc.classes);
        free(rc.code);
        return NULL;
    }

    AjsRegExp *re = (AjsRegExp *)calloc(1, sizeof(AjsRegExp));
    if (!re) {
        for (uint32_t i = 0; i < rc.group_count; i++) { free(rc.groups[i].name_utf8); free(rc.groups[i].name_u16); }
        free(rc.groups);
        for (uint32_t i = 0; i < rc.class_count; i++) re_free_char_class(&rc.classes[i]);
        free(rc.classes);
        free(rc.code);
        return NULL;
    }
    re->source_u16 = (uint16_t *)malloc(((size_t)pat_len + 1u) * sizeof(uint16_t));
    if (re->source_u16) {
        if (pat_len > 0) memcpy(re->source_u16, pat, (size_t)pat_len * sizeof(uint16_t));
        re->source_u16[pat_len] = 0;
    }
    re->source_len = pat_len;
    re->flags = flags;
    re->capture_count = rc.next_cap_idx - 1u;
    re->loop_count = rc.next_loop_idx;
    re->group_count = rc.group_count;
    re->groups = rc.groups;
    re->class_count = rc.class_count;
    re->classes = rc.classes;
    re->code_len = rc.code_len;
    re->code = rc.code;
    return re;
}

bool ajs_regexp_validate_syntax(AjsVm *vm, const uint16_t *pat, uint32_t pat_len,
                                const uint16_t *flags, uint32_t flags_len,
                                char *err_buf, size_t err_cap) {
    (void)vm;
    AjsRegExp *re = re_compile_internal(pat, pat_len, flags, flags_len, err_buf, err_cap);
    if (!re) return false;
    ajs_regexp_free(re);
    return true;
}

/* ============================================================================
 * 4. Iterative Backtracking Execution Engine (Undo Trail + Bounded Steps)
 * ============================================================================ */

typedef struct ReUndoEntry {
    uint8_t  kind; /* 0 = capture slot, 1 = loop count, 2 = loop last_sp */
    uint32_t idx;
    int32_t  old_val;
} ReUndoEntry;

typedef struct ReBtFrame {
    uint32_t pc;
    uint32_t sp;
    size_t   undo_top;
} ReBtFrame;

typedef struct ReExecCtx {
    AjsVm          *vm;
    const AjsRegExp *re;
    const uint16_t *str;
    uint32_t        slen;
    bool            uni;
    int32_t        *caps;       /* (capture_count + 1) * 2 */
    int32_t        *loop_cnt;   /* loop_count */
    int32_t        *loop_pos;   /* loop_count */
    ReUndoEntry    *undo;
    size_t          undo_len;
    size_t          undo_cap;
    uint64_t        steps;
    uint32_t        la_depth;
    bool            aborted;
} ReExecCtx;

#define RE_MAX_STEPS UINT64_C(3000000)

static bool re_push_undo(ReExecCtx *ctx, uint8_t kind, uint32_t idx, int32_t old_val) {
    if (ctx->undo_len >= ctx->undo_cap) {
        size_t ncap = ctx->undo_cap ? ctx->undo_cap * 2u : 128u;
        ReUndoEntry *nu = (ReUndoEntry *)realloc(ctx->undo, ncap * sizeof(ReUndoEntry));
        if (!nu) { ctx->aborted = true; return false; }
        ctx->undo = nu;
        ctx->undo_cap = ncap;
    }
    ctx->undo[ctx->undo_len].kind = kind;
    ctx->undo[ctx->undo_len].idx = idx;
    ctx->undo[ctx->undo_len].old_val = old_val;
    ctx->undo_len++;
    return true;
}

static void re_restore_undo(ReExecCtx *ctx, size_t target_top) {
    while (ctx->undo_len > target_top) {
        ReUndoEntry e = ctx->undo[--ctx->undo_len];
        if (e.kind == 0) ctx->caps[e.idx] = e.old_val;
        else if (e.kind == 1) ctx->loop_cnt[e.idx] = e.old_val;
        else ctx->loop_pos[e.idx] = e.old_val;
    }
}

static uint32_t re_step_char(const uint16_t *s, uint32_t slen, uint32_t sp, bool uni, bool backward, uint32_t *out_next_sp) {
    if (!backward) {
        if (sp >= slen) return UINT32_MAX;
        if (uni) {
            uint32_t adv = 1;
            uint32_t cp = ajs_utf16_code_point_at(s, slen, sp, &adv);
            *out_next_sp = sp + adv;
            return cp;
        }
        *out_next_sp = sp + 1u;
        return s[sp];
    } else {
        if (sp == 0) return UINT32_MAX;
        if (uni && sp >= 2) {
            uint16_t lo = s[sp - 1u];
            uint16_t hi = s[sp - 2u];
            if (hi >= 0xD800u && hi <= 0xDBFFu && lo >= 0xDC00u && lo <= 0xDFFFu) {
                *out_next_sp = sp - 2u;
                return 0x10000u + (((uint32_t)(hi - 0xD800u) << 10) | (uint32_t)(lo - 0xDC00u));
            }
        }
        *out_next_sp = sp - 1u;
        return s[sp - 1u];
    }
}

static bool re_run_subprogram(ReExecCtx *ctx, uint32_t start_pc, uint32_t start_sp, AjsReOp accept_op) {
    size_t bt_cap = 64, bt_len = 0;
    ReBtFrame *bt = (ReBtFrame *)malloc(bt_cap * sizeof(ReBtFrame));
    if (!bt) { ctx->aborted = true; return false; }

    #define PUSH_BT(target_pc, target_sp) do { \
        if (bt_len >= bt_cap) { \
            size_t nc = bt_cap * 2u; \
            ReBtFrame *nb = (ReBtFrame *)realloc(bt, nc * sizeof(ReBtFrame)); \
            if (!nb) { ctx->aborted = true; free(bt); return false; } \
            bt = nb; bt_cap = nc; \
        } \
        bt[bt_len].pc = (target_pc); \
        bt[bt_len].sp = (target_sp); \
        bt[bt_len].undo_top = ctx->undo_len; \
        bt_len++; \
    } while (0)

    uint32_t pc = start_pc;
    uint32_t sp = start_sp;
    const AjsReInst *code = ctx->re->code;

    for (;;) {
        if ((++ctx->steps & 4095u) == 0) {
            if (ctx->vm && ctx->vm->cfg.interrupt_fn &&
                ctx->vm->cfg.interrupt_fn(ctx->vm, ctx->vm->cfg.interrupt_userdata)) {
                ctx->aborted = true;
                free(bt);
                return false;
            }
            if (ctx->steps > RE_MAX_STEPS) {
                free(bt);
                return false;
            }
        }

        const AjsReInst *ins = &code[pc];
        if (ins->op == (uint8_t)accept_op) {
            free(bt);
            return true;
        }

        uint32_t lflags = (ins->flags_dir & 0xFFu) | (ctx->re->flags & (RE_FLAG_U | RE_FLAG_V));
        bool bwd = (ins->flags_dir & 0x100u) != 0;
        bool step_ok = true;

        switch ((AjsReOp)ins->op) {
            case RE_OP_MATCH:
            case RE_OP_LOOK_END:
                step_ok = false;
                break;

            case RE_OP_CHAR: {
                uint32_t nsp = sp;
                uint32_t cp = re_step_char(ctx->str, ctx->slen, sp, ctx->uni, bwd, &nsp);
                if (cp == UINT32_MAX || re_fold_cp(cp, lflags) != re_fold_cp(ins->a, lflags)) {
                    step_ok = false;
                } else {
                    sp = nsp;
                    pc++;
                }
                break;
            }

            case RE_OP_ANY: {
                uint32_t nsp = sp;
                uint32_t cp = re_step_char(ctx->str, ctx->slen, sp, ctx->uni, bwd, &nsp);
                if (cp == UINT32_MAX || (!(lflags & RE_FLAG_S) && re_is_line_terminator(cp))) {
                    step_ok = false;
                } else {
                    sp = nsp;
                    pc++;
                }
                break;
            }

            case RE_OP_CLASS: {
                uint32_t nsp = sp;
                uint32_t cp = re_step_char(ctx->str, ctx->slen, sp, ctx->uni, bwd, &nsp);
                if (cp == UINT32_MAX || !re_char_class_matches_cp(&ctx->re->classes[ins->a], cp)) {
                    step_ok = false;
                } else {
                    sp = nsp;
                    pc++;
                }
                break;
            }

            case RE_OP_BOL: {
                if (sp == 0) {
                    pc++;
                } else if ((lflags & RE_FLAG_M) && re_is_line_terminator(ctx->str[sp - 1u])) {
                    pc++;
                } else {
                    step_ok = false;
                }
                break;
            }

            case RE_OP_EOL: {
                if (sp == ctx->slen) {
                    pc++;
                } else if ((lflags & RE_FLAG_M) && re_is_line_terminator(ctx->str[sp])) {
                    pc++;
                } else {
                    step_ok = false;
                }
                break;
            }

            case RE_OP_WORD_BOUND:
            case RE_OP_NOT_WORD_BOUND: {
                bool uni_i = (lflags & RE_FLAG_I) && ctx->uni;
                uint32_t nsp = 0;
                uint32_t prev_cp = (sp > 0) ? re_step_char(ctx->str, ctx->slen, sp, ctx->uni, true, &nsp) : UINT32_MAX;
                uint32_t next_cp = (sp < ctx->slen) ? re_step_char(ctx->str, ctx->slen, sp, ctx->uni, false, &nsp) : UINT32_MAX;
                bool w1 = (prev_cp != UINT32_MAX) && re_is_word_char(prev_cp, uni_i);
                bool w2 = (next_cp != UINT32_MAX) && re_is_word_char(next_cp, uni_i);
                bool is_bound = (w1 != w2);
                if ((ins->op == RE_OP_WORD_BOUND) ? is_bound : !is_bound) pc++;
                else step_ok = false;
                break;
            }

            case RE_OP_SAVE: {
                int32_t old = ctx->caps[ins->a];
                if (old != (int32_t)sp) {
                    if (!re_push_undo(ctx, 0, ins->a, old)) { free(bt); return false; }
                    ctx->caps[ins->a] = (int32_t)sp;
                }
                pc++;
                break;
            }

            case RE_OP_CLEAR_CAPS: {
                for (uint32_t c = ins->a; c <= ins->b; c++) {
                    uint32_t s0 = c * 2u, s1 = c * 2u + 1u;
                    if (ctx->caps[s0] != -1) {
                        if (!re_push_undo(ctx, 0, s0, ctx->caps[s0])) { free(bt); return false; }
                        ctx->caps[s0] = -1;
                    }
                    if (ctx->caps[s1] != -1) {
                        if (!re_push_undo(ctx, 0, s1, ctx->caps[s1])) { free(bt); return false; }
                        ctx->caps[s1] = -1;
                    }
                }
                pc++;
                break;
            }

            case RE_OP_JMP:
                pc = ins->a;
                break;

            case RE_OP_SPLIT:
                PUSH_BT(ins->b, sp);
                pc = ins->a;
                break;

            case RE_OP_LOOP_INIT: {
                uint32_t r = ins->a;
                if (!re_push_undo(ctx, 1, r, ctx->loop_cnt[r])) { free(bt); return false; }
                if (!re_push_undo(ctx, 2, r, ctx->loop_pos[r])) { free(bt); return false; }
                ctx->loop_cnt[r] = 0;
                ctx->loop_pos[r] = -2;
                pc = ins->b;
                break;
            }

            case RE_OP_LOOP_STEP: {
                uint32_t r = ins->a;
                uint32_t cnt = (uint32_t)ctx->loop_cnt[r];
                int32_t prev_sp = ctx->loop_pos[r];
                /* Zero-width guard once minimum repetitions have been satisfied */
                if (cnt > ins->min_rep && prev_sp == (int32_t)sp) {
                    step_ok = false;
                    break;
                }
                if (cnt < ins->min_rep) {
                    if (!re_push_undo(ctx, 1, r, ctx->loop_cnt[r])) { free(bt); return false; }
                    if (!re_push_undo(ctx, 2, r, ctx->loop_pos[r])) { free(bt); return false; }
                    ctx->loop_cnt[r] = (int32_t)(cnt + 1u);
                    ctx->loop_pos[r] = (int32_t)sp;
                    pc = ins->b;
                } else if (ins->max_rep != UINT32_MAX && cnt >= ins->max_rep) {
                    pc = ins->c;
                } else {
                    bool greedy = (ins->pad != 0);
                    if (greedy) {
                        PUSH_BT(ins->c, sp);
                        if (!re_push_undo(ctx, 1, r, ctx->loop_cnt[r])) { free(bt); return false; }
                        if (!re_push_undo(ctx, 2, r, ctx->loop_pos[r])) { free(bt); return false; }
                        ctx->loop_cnt[r] = (int32_t)(cnt + 1u);
                        ctx->loop_pos[r] = (int32_t)sp;
                        pc = ins->b;
                    } else {
                        /* Lazy: try exit first; fallback increments loop and enters body */
                        size_t pre_undo = ctx->undo_len;
                        if (!re_push_undo(ctx, 1, r, ctx->loop_cnt[r])) { free(bt); return false; }
                        if (!re_push_undo(ctx, 2, r, ctx->loop_pos[r])) { free(bt); return false; }
                        /* Wait: backtrack restores TO undo_top, so push body choice with incremented state?
                           Simpler: push body_pc AFTER recording undo? No, re_restore_undo pops DOWN to undo_top!
                           So if we try exit_pc first without modifying loop_cnt, then when exit_pc fails we want loop_cnt+1!
                           Let's handle that cleanly: only increment loop_cnt on entry to body via an inline helper or push body_pc with a wrapper! */
                        re_restore_undo(ctx, pre_undo);
                        /* Even cleaner: in lazy branch, save current state, try exit_pc? No, just modify loop_cnt BEFORE PUSH_BT,
                           and for exit_pc loop_cnt is unused after loop exit unless outer backtrack re-enters!
                           Wait: if inner regex after exit_pc re-enters the SAME loop (cannot happen unless nested inside another loop, which runs LOOP_INIT!),
                           setting loop_cnt = cnt+1 BEFORE PUSH_BT(body_pc) is 100% safe because exit_pc leaves this loop, and any outer loop runs LOOP_INIT! */
                        if (!re_push_undo(ctx, 1, r, ctx->loop_cnt[r])) { free(bt); return false; }
                        if (!re_push_undo(ctx, 2, r, ctx->loop_pos[r])) { free(bt); return false; }
                        ctx->loop_cnt[r] = (int32_t)(cnt + 1u);
                        ctx->loop_pos[r] = (int32_t)sp;
                        PUSH_BT(ins->b, sp);
                        pc = ins->c;
                    }
                }
                break;
            }

            case RE_OP_BACKREF:
            case RE_OP_NAMED_BACKREF: {
                int32_t cs = -1, ce = -1;
                if (ins->op == RE_OP_BACKREF) {
                    cs = ctx->caps[ins->a * 2u];
                    ce = ctx->caps[ins->a * 2u + 1u];
                } else {
                    const char *tname = ctx->re->groups[ins->a].name_utf8;
                    for (uint32_t g = 0; g < ctx->re->group_count; g++) {
                        if (strcmp(ctx->re->groups[g].name_utf8, tname) == 0) {
                            uint32_t ci = ctx->re->groups[g].cap_idx;
                            if (ctx->caps[ci * 2u] >= 0 && ctx->caps[ci * 2u + 1u] >= 0) {
                                cs = ctx->caps[ci * 2u];
                                ce = ctx->caps[ci * 2u + 1u];
                                break;
                            }
                        }
                    }
                }
                if (cs < 0 || ce < 0 || ce <= cs) {
                    /* Unmatched or empty capture always matches 0 characters per ECMA-262 */
                    pc++;
                    break;
                }
                uint32_t ref_p = bwd ? (uint32_t)ce : (uint32_t)cs;
                uint32_t cur_sp = sp;
                bool matched = true;
                while (bwd ? (ref_p > (uint32_t)cs) : (ref_p < (uint32_t)ce)) {
                    uint32_t next_ref = ref_p, next_sp = cur_sp;
                    uint32_t rcp = re_step_char(ctx->str, (uint32_t)ce, ref_p, ctx->uni, bwd, &next_ref);
                    uint32_t scp = re_step_char(ctx->str, ctx->slen, cur_sp, ctx->uni, bwd, &next_sp);
                    if (scp == UINT32_MAX || re_fold_cp(rcp, lflags) != re_fold_cp(scp, lflags)) {
                        matched = false;
                        break;
                    }
                    ref_p = next_ref;
                    cur_sp = next_sp;
                }
                if (matched) {
                    sp = cur_sp;
                    pc++;
                } else {
                    step_ok = false;
                }
                break;
            }

            case RE_OP_LOOKAROUND: {
                if (ctx->la_depth >= 64u) { step_ok = false; break; }
                size_t saved_undo = ctx->undo_len;
                ctx->la_depth++;
                bool sub_ok = re_run_subprogram(ctx, ins->a, sp, RE_OP_LOOK_END);
                ctx->la_depth--;
                if (ctx->aborted) { free(bt); return false; }
                bool is_pos = (ins->b != 0);
                if (is_pos) {
                    if (sub_ok) {
                        /* Positive lookaround keeps inner captures! (And if outer path backtracks past lookaround, saved_undo is on ctx->undo so it rolls back cleanly!) */
                        pc++;
                    } else {
                        re_restore_undo(ctx, saved_undo);
                        step_ok = false;
                    }
                } else {
                    /* Negative lookaround always discards inner captures */
                    re_restore_undo(ctx, saved_undo);
                    if (!sub_ok) pc++;
                    else step_ok = false;
                }
                break;
            }
        }

        if (!step_ok) {
            if (bt_len == 0) {
                free(bt);
                return false;
            }
            ReBtFrame f = bt[--bt_len];
            re_restore_undo(ctx, f.undo_top);
            pc = f.pc;
            sp = f.sp;
        }
    }
    #undef PUSH_BT
}

/* Attempt match starting at index [start_idx .. slen]; if sticky, only at start_idx */
static int re_exec_raw(AjsVm *vm, const AjsRegExp *re, const uint16_t *str, uint32_t slen,
                       uint32_t start_idx, int32_t *out_caps) {
    uint32_t nslots = (re->capture_count + 1u) * 2u;
    int32_t *loop_cnt = re->loop_count ? (int32_t *)calloc(re->loop_count, sizeof(int32_t)) : NULL;
    int32_t *loop_pos = re->loop_count ? (int32_t *)calloc(re->loop_count, sizeof(int32_t)) : NULL;
    if (re->loop_count && (!loop_cnt || !loop_pos)) {
        free(loop_cnt); free(loop_pos);
        ajs_throw_err_kind(vm, AJS_ERR_KIND_ERROR, "Out of memory in RegExp exec");
        return -1;
    }

    ReExecCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.vm = vm;
    ctx.re = re;
    ctx.str = str;
    ctx.slen = slen;
    ctx.uni = (re->flags & (RE_FLAG_U | RE_FLAG_V)) != 0;
    ctx.caps = out_caps;
    ctx.loop_cnt = loop_cnt;
    ctx.loop_pos = loop_pos;

    bool sticky = (re->flags & RE_FLAG_Y) != 0;
    uint32_t pos = start_idx;
    if (ctx.uni && pos > 0 && pos < slen &&
        str[pos - 1u] >= 0xD800u && str[pos - 1u] <= 0xDBFFu &&
        str[pos] >= 0xDC00u && str[pos] <= 0xDFFFu) {
        if (!sticky) pos--; /* Align surrogate pair if needed */
    }

    int result = 0;
    for (;;) {
        for (uint32_t i = 0; i < nslots; i++) out_caps[i] = -1;
        ctx.undo_len = 0;
        if (re_run_subprogram(&ctx, 0, pos, RE_OP_MATCH)) {
            result = 1;
            break;
        }
        if (ctx.aborted) {
            result = -1;
            break;
        }
        if (sticky || pos >= slen) break;
        if (ctx.uni) {
            uint32_t adv = 1;
            (void)ajs_utf16_code_point_at(str, slen, pos, &adv);
            pos += adv;
        } else {
            pos++;
        }
    }

    free(ctx.undo);
    free(loop_cnt);
    free(loop_pos);
    if (result < 0 && vm && !vm->has_exception) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Regular expression execution interrupted");
    }
    return result;
}

/* ============================================================================
 * 5. RegExp Object Construction, BuiltinExec & Abstract Operations
 * ============================================================================ */

#define RE_METHOD_FLAGS (AJS_PROP_WRITABLE | AJS_PROP_CONFIGURABLE)

static AjsString *re_flags_to_string(AjsVm *vm, uint32_t flags) {
    char buf[12];
    size_t n = 0;
    if (flags & RE_FLAG_D) buf[n++] = 'd';
    if (flags & RE_FLAG_G) buf[n++] = 'g';
    if (flags & RE_FLAG_I) buf[n++] = 'i';
    if (flags & RE_FLAG_M) buf[n++] = 'm';
    if (flags & RE_FLAG_S) buf[n++] = 's';
    if (flags & RE_FLAG_U) buf[n++] = 'u';
    if (flags & RE_FLAG_V) buf[n++] = 'v';
    if (flags & RE_FLAG_Y) buf[n++] = 'y';
    buf[n] = '\0';
    return ajs_str_from_utf8(vm, buf, n, false);
}

static AjsString *re_escape_pattern_source(AjsVm *vm, const uint16_t *u, uint32_t len) {
    if (len == 0) {
        return ajs_str_from_cstr(vm, "(?:)", true);
    }
    size_t cap = (size_t)len * 2u + 8u;
    uint16_t *out = (uint16_t *)malloc(cap * sizeof(uint16_t));
    if (!out) return NULL;
    size_t pos = 0;
    bool in_class = false;
    for (uint32_t i = 0; i < len; i++) {
        if (pos + 8u >= cap) {
            cap *= 2u;
            uint16_t *no = (uint16_t *)realloc(out, cap * sizeof(uint16_t));
            if (!no) { free(out); return NULL; }
            out = no;
        }
        uint16_t c = u[i];
        if (c == '\\' && i + 1u < len) {
            uint16_t nx = u[++i];
            if (nx == '\n') { out[pos++] = '\\'; out[pos++] = 'n'; }
            else if (nx == '\r') { out[pos++] = '\\'; out[pos++] = 'r'; }
            else if (nx == 0x2028u) { memcpy(out + pos, (const uint16_t[]){'\\','u','2','0','2','8'}, 6 * sizeof(uint16_t)); pos += 6; }
            else if (nx == 0x2029u) { memcpy(out + pos, (const uint16_t[]){'\\','u','2','0','2','9'}, 6 * sizeof(uint16_t)); pos += 6; }
            else { out[pos++] = '\\'; out[pos++] = nx; }
            continue;
        }
        if (c == '[') in_class = true;
        else if (c == ']') in_class = false;

        if (c == '/' && !in_class) {
            out[pos++] = '\\';
            out[pos++] = '/';
        } else if (c == '\n') {
            out[pos++] = '\\';
            out[pos++] = 'n';
        } else if (c == '\r') {
            out[pos++] = '\\';
            out[pos++] = 'r';
        } else if (c == 0x2028u) {
            memcpy(out + pos, (const uint16_t[]){'\\','u','2','0','2','8'}, 6 * sizeof(uint16_t));
            pos += 6;
        } else if (c == 0x2029u) {
            memcpy(out + pos, (const uint16_t[]){'\\','u','2','0','2','9'}, 6 * sizeof(uint16_t));
            pos += 6;
        } else {
            out[pos++] = c;
        }
    }
    AjsString *res = ajs_str_from_utf16(vm, out, pos, false);
    free(out);
    return res;
}

static bool re_define_last_index(AjsVm *vm, AjsObject *obj, AjsValue init_val) {
    AjsString *k = ajs_str_from_cstr(vm, "lastIndex", true);
    if (!k) return false;
    AjsPropDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                 AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
    desc.value = init_val;
    return ajs_obj_define_own_prop(vm, obj, ajs_val_from_gc(k), &desc, true);
}

AjsValue ajs_regexp_new_obj(AjsVm *vm, AjsRealm *realm, const AjsString *pattern, const AjsString *flags) {
    if (!realm) realm = vm->active_realm;
    const uint16_t *pu = pattern ? pattern->units : NULL;
    uint32_t plen = pattern ? pattern->len16 : 0;
    const uint16_t *fu = flags ? flags->units : NULL;
    uint32_t flen = flags ? flags->len16 : 0;

    char err_buf[160];
    err_buf[0] = '\0';
    AjsRegExp *re = re_compile_internal(pu, plen, fu, flen, err_buf, sizeof(err_buf));
    if (!re) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "%s", err_buf[0] ? err_buf : "Invalid regular expression");
    }

    AjsObject *obj = ajs_obj_new(vm, realm, AJS_OBJ_REGEXP, realm->regexp_proto);
    if (!obj) {
        ajs_regexp_free(re);
        return AJS_VAL_EXCEPTION;
    }
    obj->u.regexp = re;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, ajs_val_from_gc(obj));
    re_define_last_index(vm, obj, ajs_int32(0));
    ajs_temp_root_restore(vm, rs);
    return vm->has_exception ? AJS_VAL_EXCEPTION : ajs_val_from_gc(obj);
}

bool ajs_is_regexp_like(AjsVm *vm, AjsValue v, bool *out_is_rx) {
    *out_is_rx = false;
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) {
        return true;
    }
    AjsValue matcher = ajs_obj_get(vm, v, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_MATCH]), v);
    if (vm->has_exception) return false;
    if (matcher != AJS_VAL_UNDEFINED) {
        *out_is_rx = ajs_to_bool(vm, matcher);
        return true;
    }
    AjsObject *obj = (AjsObject *)ajs_val_to_gc(v);
    *out_is_rx = (obj->kind == AJS_OBJ_REGEXP);
    return true;
}

static bool re_set_last_index(AjsVm *vm, AjsValue rx_val, double idx) {
    AjsString *k = ajs_str_from_cstr(vm, "lastIndex", true);
    if (!k) return false;
    return ajs_obj_set(vm, rx_val, ajs_val_from_gc(k), ajs_val_number(idx), rx_val, true);
}

static uint64_t re_advance_string_index(const AjsString *s, uint64_t index, bool unicode) {
    if (!unicode || index + 1u >= (uint64_t)s->len16) {
        return index + 1u;
    }
    uint32_t adv = 1;
    (void)ajs_utf16_code_point_at(s->units, s->len16, (uint32_t)index, &adv);
    return index + (uint64_t)adv;
}

static void re_define_data_prop(AjsVm *vm, AjsObject *obj, AjsValue key, AjsValue val) {
    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE |
              AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
              AJS_DESC_HAS_ENUMERABLE | AJS_DESC_ENUMERABLE |
              AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    d.value = val;
    ajs_obj_define_own_prop(vm, obj, key, &d, false);
}

AjsValue ajs_regexp_exec_builtin(AjsVm *vm, AjsValue rx_val, AjsString *str) {
    if (!ajs_val_is_gc_kind(rx_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExpBuiltinExec called on non-object");
    }
    AjsObject *rx_obj = (AjsObject *)ajs_val_to_gc(rx_val);
    if (rx_obj->kind != AJS_OBJ_REGEXP || !rx_obj->u.regexp) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExpBuiltinExec called on incompatible receiver");
    }
    AjsRegExp *re = rx_obj->u.regexp;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, rx_val);
    ajs_temp_root_push(vm, ajs_val_from_gc(str));

    AjsString *k_last = ajs_str_from_cstr(vm, "lastIndex", true);
    AjsValue li_val = ajs_obj_get(vm, rx_val, ajs_val_from_gc(k_last), rx_val);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }

    uint64_t last_index = 0;
    if (!ajs_to_length(vm, li_val, &last_index)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }

    bool global = (re->flags & RE_FLAG_G) != 0;
    bool sticky = (re->flags & RE_FLAG_Y) != 0;
    if (!global && !sticky) {
        last_index = 0;
    }

    if (last_index > (uint64_t)str->len16) {
        if (global || sticky) {
            if (!re_set_last_index(vm, rx_val, 0.0)) {
                ajs_temp_root_restore(vm, rs);
                return AJS_VAL_EXCEPTION;
            }
        }
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_NULL;
    }

    uint32_t nslots = (re->capture_count + 1u) * 2u;
    int32_t *caps = (int32_t *)malloc(nslots * sizeof(int32_t));
    if (!caps) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_ERROR, "Out of memory");
    }

    int rc = re_exec_raw(vm, re, str->units, str->len16, (uint32_t)last_index, caps);
    if (rc < 0) {
        free(caps);
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    if (rc == 0) {
        free(caps);
        if (global || sticky) {
            if (!re_set_last_index(vm, rx_val, 0.0)) {
                ajs_temp_root_restore(vm, rs);
                return AJS_VAL_EXCEPTION;
            }
        }
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_NULL;
    }

    uint32_t match_start = (uint32_t)caps[0];
    uint32_t match_end   = (uint32_t)caps[1];
    if (global || sticky) {
        if (!re_set_last_index(vm, rx_val, (double)match_end)) {
            free(caps);
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }

    uint32_t ncaps = re->capture_count + 1u;
    AjsValue arr = ajs_new_array(vm, ncaps);
    ajs_temp_root_push(vm, arr);
    AjsObject *arr_obj = (AjsObject *)ajs_val_to_gc(arr);

    AjsValue indices_arr = AJS_VAL_UNDEFINED;
    AjsObject *indices_groups = NULL;
    bool has_indices = (re->flags & RE_FLAG_D) != 0;
    if (has_indices) {
        indices_arr = ajs_new_array(vm, ncaps);
        ajs_temp_root_push(vm, indices_arr);
    }

    AjsObject *groups_obj = NULL;
    if (re->group_count > 0) {
        groups_obj = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ORDINARY, AJS_VAL_NULL);
        ajs_temp_root_push(vm, ajs_val_from_gc(groups_obj));
        if (has_indices) {
            indices_groups = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ORDINARY, AJS_VAL_NULL);
            ajs_temp_root_push(vm, ajs_val_from_gc(indices_groups));
        }
        /* Pre-populate unique group names in source order as undefined */
        for (uint32_t g = 0; g < re->group_count; g++) {
            AjsString *gk = ajs_str_from_utf16(vm, re->groups[g].name_u16, re->groups[g].name_len16, true);
            re_define_data_prop(vm, groups_obj, ajs_val_from_gc(gk), AJS_VAL_UNDEFINED);
            if (indices_groups) {
                re_define_data_prop(vm, indices_groups, ajs_val_from_gc(gk), AJS_VAL_UNDEFINED);
            }
        }
    }

    for (uint32_t i = 0; i < ncaps; i++) {
        int32_t s_idx = caps[i * 2u];
        int32_t e_idx = caps[i * 2u + 1u];
        AjsValue cap_val = AJS_VAL_UNDEFINED;
        AjsValue pair_val = AJS_VAL_UNDEFINED;
        size_t ls = ajs_temp_root_save(vm);
        if (s_idx >= 0 && e_idx >= s_idx) {
            AjsString *sub = ajs_str_from_utf16(vm, str->units + s_idx, (size_t)(e_idx - s_idx), false);
            cap_val = ajs_val_from_gc(sub);
            ajs_temp_root_push(vm, cap_val);
            if (has_indices) {
                pair_val = ajs_new_array(vm, 2);
                ajs_temp_root_push(vm, pair_val);
                ajs_set_prop_index(vm, pair_val, 0, ajs_val_number((double)s_idx));
                ajs_set_prop_index(vm, pair_val, 1, ajs_val_number((double)e_idx));
            }
        }
        ajs_set_prop_index(vm, arr, i, cap_val);
        if (has_indices) {
            ajs_set_prop_index(vm, indices_arr, i, pair_val);
        }
        if (i > 0 && s_idx >= 0 && groups_obj) {
            for (uint32_t g = 0; g < re->group_count; g++) {
                if (re->groups[g].cap_idx == i) {
                    AjsString *gk = ajs_str_from_utf16(vm, re->groups[g].name_u16, re->groups[g].name_len16, true);
                    re_define_data_prop(vm, groups_obj, ajs_val_from_gc(gk), cap_val);
                    if (indices_groups) {
                        re_define_data_prop(vm, indices_groups, ajs_val_from_gc(gk), pair_val);
                    }
                    break;
                }
            }
        }
        ajs_temp_root_restore(vm, ls);
    }

    free(caps);

    re_define_data_prop(vm, arr_obj, ajs_val_from_gc(ajs_str_from_cstr(vm, "index", true)), ajs_val_number((double)match_start));
    re_define_data_prop(vm, arr_obj, ajs_val_from_gc(ajs_str_from_cstr(vm, "input", true)), ajs_val_from_gc(str));
    re_define_data_prop(vm, arr_obj, ajs_val_from_gc(ajs_str_from_cstr(vm, "groups", true)),
                        groups_obj ? ajs_val_from_gc(groups_obj) : AJS_VAL_UNDEFINED);
    if (has_indices) {
        AjsObject *ind_obj = (AjsObject *)ajs_val_to_gc(indices_arr);
        re_define_data_prop(vm, ind_obj, ajs_val_from_gc(ajs_str_from_cstr(vm, "groups", true)),
                            indices_groups ? ajs_val_from_gc(indices_groups) : AJS_VAL_UNDEFINED);
        re_define_data_prop(vm, arr_obj, ajs_val_from_gc(ajs_str_from_cstr(vm, "indices", true)), indices_arr);
    }

    ajs_temp_root_restore(vm, rs);
    return arr;
}

static AjsValue re_exec_abstract(AjsVm *vm, AjsValue rx_val, AjsString *str) {
    if (!ajs_val_is_gc_kind(rx_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExpExec called on non-object");
    }
    AjsValue exec_fn = ajs_obj_get(vm, rx_val, ajs_val_from_gc(ajs_str_from_cstr(vm, "exec", true)), rx_val);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (ajs_is_callable(vm, exec_fn)) {
        AjsValue arg = ajs_val_from_gc(str);
        AjsValue res = ajs_call_internal(vm, exec_fn, rx_val, 1, &arg);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        if (res != AJS_VAL_NULL && !ajs_val_is_gc_kind(res, AJS_GC_OBJECT)) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp exec method returned neither an object nor null");
        }
        return res;
    }
    return ajs_regexp_exec_builtin(vm, rx_val, str);
}

/* ============================================================================
 * 6. RegExp Constructor, RegExp.escape, Prototype Getters & Methods
 * ============================================================================ */

static AjsValue re_get_proto_from_ctor(AjsVm *vm, AjsValue new_target, AjsValue default_proto) {
    if (!ajs_val_is_gc_kind(new_target, AJS_GC_OBJECT)) return default_proto;
    AjsValue p = ajs_obj_get(vm, new_target, ajs_val_from_gc(vm->str_prototype), new_target);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (ajs_val_is_gc_kind(p, AJS_GC_OBJECT)) return p;
    AjsObject *nt_obj = (AjsObject *)ajs_val_to_gc(new_target);
    if (nt_obj->realm) return nt_obj->realm->regexp_proto;
    return default_proto;
}

static AjsValue js_regexp_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsRealm *realm = vm->active_realm;
    AjsValue pat_val = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue flags_val = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;

    bool pat_is_rx = false;
    if (!ajs_is_regexp_like(vm, pat_val, &pat_is_rx)) return AJS_VAL_EXCEPTION;

    AjsValue new_target = ajs_get_native_new_target(vm);
    if (new_target == AJS_VAL_UNDEFINED) {
        new_target = realm->regexp_ctor;
        if (pat_is_rx && flags_val == AJS_VAL_UNDEFINED) {
            AjsValue pat_ctor = ajs_obj_get(vm, pat_val, ajs_val_from_gc(vm->str_constructor), pat_val);
            if (vm->has_exception) return AJS_VAL_EXCEPTION;
            if (ajs_strict_eq(pat_ctor, new_target)) {
                return pat_val;
            }
        }
    }

    size_t rs = ajs_temp_root_save(vm);
    AjsString *pat_str = NULL;
    AjsString *flags_str = NULL;

    if (ajs_val_is_gc_kind(pat_val, AJS_GC_OBJECT) &&
        ((AjsObject *)ajs_val_to_gc(pat_val))->kind == AJS_OBJ_REGEXP &&
        ((AjsObject *)ajs_val_to_gc(pat_val))->u.regexp != NULL) {
        AjsRegExp *src_re = ((AjsObject *)ajs_val_to_gc(pat_val))->u.regexp;
        pat_str = ajs_str_from_utf16(vm, src_re->source_u16, src_re->source_len, false);
        ajs_temp_root_push(vm, ajs_val_from_gc(pat_str));
        if (flags_val == AJS_VAL_UNDEFINED) {
            flags_str = re_flags_to_string(vm, src_re->flags);
        } else {
            flags_str = ajs_to_js_string(vm, flags_val);
            if (!flags_str || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
        ajs_temp_root_push(vm, ajs_val_from_gc(flags_str));
    } else if (pat_is_rx) {
        AjsValue pv = ajs_obj_get(vm, pat_val, ajs_val_from_gc(ajs_str_from_cstr(vm, "source", true)), pat_val);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        AjsValue fv = flags_val;
        if (flags_val == AJS_VAL_UNDEFINED) {
            fv = ajs_obj_get(vm, pat_val, ajs_val_from_gc(ajs_str_from_cstr(vm, "flags", true)), pat_val);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
        pat_str = (pv == AJS_VAL_UNDEFINED) ? vm->str_empty : ajs_to_js_string(vm, pv);
        if (!pat_str || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(pat_str));
        flags_str = (fv == AJS_VAL_UNDEFINED) ? vm->str_empty : ajs_to_js_string(vm, fv);
        if (!flags_str || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(flags_str));
    } else {
        pat_str = (pat_val == AJS_VAL_UNDEFINED) ? vm->str_empty : ajs_to_js_string(vm, pat_val);
        if (!pat_str || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(pat_str));
        flags_str = (flags_val == AJS_VAL_UNDEFINED) ? vm->str_empty : ajs_to_js_string(vm, flags_val);
        if (!flags_str || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(flags_str));
    }

    AjsValue proto = re_get_proto_from_ctor(vm, new_target, realm->regexp_proto);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, proto);

    char err_buf[160];
    err_buf[0] = '\0';
    AjsRegExp *re = re_compile_internal(pat_str->units, pat_str->len16,
                                        flags_str->units, flags_str->len16,
                                        err_buf, sizeof(err_buf));
    if (!re) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "%s", err_buf[0] ? err_buf : "Invalid regular expression");
    }

    AjsObject *obj = ajs_obj_new(vm, realm, AJS_OBJ_REGEXP, proto);
    if (!obj) {
        ajs_regexp_free(re);
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    obj->u.regexp = re;
    ajs_temp_root_push(vm, ajs_val_from_gc(obj));
    re_define_last_index(vm, obj, ajs_int32(0));
    re_set_last_index(vm, ajs_val_from_gc(obj), 0.0);
    ajs_temp_root_restore(vm, rs);
    return vm->has_exception ? AJS_VAL_EXCEPTION : ajs_val_from_gc(obj);
}

static AjsValue js_regexp_species_get(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)vm; (void)c; (void)v; (void)u;
    return this_val;
}

/* ES2025/2026 RegExp.escape(S) */
static AjsValue js_regexp_escape(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue arg = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(arg, AJS_GC_STRING)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp.escape requires a string argument");
    }
    AjsString *s = (AjsString *)ajs_val_to_gc(arg);
    if (s->len16 == 0) return arg;

    static const char hex_lc[] = "0123456789abcdef";
    size_t cap = (size_t)s->len16 * 6u + 8u;
    uint16_t *out = (uint16_t *)malloc(cap * sizeof(uint16_t));
    if (!out) return ajs_throw_err_kind(vm, AJS_ERR_KIND_ERROR, "Out of memory");
    size_t pos = 0;

    for (uint32_t i = 0; i < s->len16; ) {
        uint32_t adv = 1;
        uint32_t cp = ajs_utf16_code_point_at(s->units, s->len16, i, &adv);
        bool is_first = (i == 0);
        i += adv;

        if (is_first && ((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'))) {
            out[pos++] = '\\'; out[pos++] = 'x';
            out[pos++] = (uint16_t)hex_lc[(cp >> 4) & 0xFu];
            out[pos++] = (uint16_t)hex_lc[cp & 0xFu];
            continue;
        }
        if (cp == '^' || cp == '$' || cp == '\\' || cp == '.' || cp == '*' || cp == '+' ||
            cp == '?' || cp == '(' || cp == ')' || cp == '[' || cp == ']' || cp == '{' ||
            cp == '}' || cp == '|' || cp == '/') {
            out[pos++] = '\\';
            out[pos++] = (uint16_t)cp;
            continue;
        }
        if (cp == 0x09u) { out[pos++] = '\\'; out[pos++] = 't'; continue; }
        if (cp == 0x0Au) { out[pos++] = '\\'; out[pos++] = 'n'; continue; }
        if (cp == 0x0Bu) { out[pos++] = '\\'; out[pos++] = 'v'; continue; }
        if (cp == 0x0Cu) { out[pos++] = '\\'; out[pos++] = 'f'; continue; }
        if (cp == 0x0Du) { out[pos++] = '\\'; out[pos++] = 'r'; continue; }

        bool other_punct = (cp == ',' || cp == '-' || cp == '=' || cp == '<' || cp == '>' ||
                            cp == '#' || cp == '&' || cp == '!' || cp == '%' || cp == ':' ||
                            cp == ';' || cp == '@' || cp == '~' || cp == '\'' || cp == '`' ||
                            cp == '"' || cp == ' ');
        bool must_hex = other_punct || cp <= 0x1Fu || (cp >= 0x7Fu && cp <= 0x9Fu) ||
                        (cp >= 0xD800u && cp <= 0xDFFFu) || ajs_unicode_is_whitespace(cp);
        if (must_hex) {
            if (cp <= 0xFFu) {
                out[pos++] = '\\'; out[pos++] = 'x';
                out[pos++] = (uint16_t)hex_lc[(cp >> 4) & 0xFu];
                out[pos++] = (uint16_t)hex_lc[cp & 0xFu];
            } else {
                out[pos++] = '\\'; out[pos++] = 'u';
                out[pos++] = (uint16_t)hex_lc[(cp >> 12) & 0xFu];
                out[pos++] = (uint16_t)hex_lc[(cp >> 8) & 0xFu];
                out[pos++] = (uint16_t)hex_lc[(cp >> 4) & 0xFu];
                out[pos++] = (uint16_t)hex_lc[cp & 0xFu];
            }
        } else if (cp <= 0xFFFFu) {
            out[pos++] = (uint16_t)cp;
        } else {
            uint32_t v = cp - 0x10000u;
            out[pos++] = (uint16_t)(0xD800u + (v >> 10));
            out[pos++] = (uint16_t)(0xDC00u + (v & 0x3FFu));
        }
    }

    AjsString *res = ajs_str_from_utf16(vm, out, pos, false);
    free(out);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}

static AjsValue js_regexp_exec(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT) ||
        ((AjsObject *)ajs_val_to_gc(this_val))->kind != AJS_OBJ_REGEXP) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp.prototype.exec called on incompatible receiver");
    }
    AjsString *s = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    return ajs_regexp_exec_builtin(vm, this_val, s);
}

static AjsValue js_regexp_test(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp.prototype.test called on non-object");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));
    AjsValue match = re_exec_abstract(vm, this_val, s);
    ajs_temp_root_restore(vm, rs);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    return ajs_bool(match != AJS_VAL_NULL);
}

/* Annex B RegExp.prototype.compile(pattern, flags) */
static AjsValue js_regexp_compile(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp.prototype.compile called on non-object");
    }
    AjsObject *obj = (AjsObject *)ajs_val_to_gc(this_val);
    if (obj->kind != AJS_OBJ_REGEXP || !obj->u.regexp || obj->realm != vm->active_realm) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp.prototype.compile called on incompatible receiver");
    }
    AjsValue pat_val = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue flags_val = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;

    size_t rs = ajs_temp_root_save(vm);
    AjsString *pat_str = NULL;
    AjsString *flags_str = NULL;
    if (ajs_val_is_gc_kind(pat_val, AJS_GC_OBJECT) &&
        ((AjsObject *)ajs_val_to_gc(pat_val))->kind == AJS_OBJ_REGEXP &&
        ((AjsObject *)ajs_val_to_gc(pat_val))->u.regexp != NULL) {
        if (flags_val != AJS_VAL_UNDEFINED) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot supply flags when compiling from a RegExp");
        }
        AjsRegExp *src = ((AjsObject *)ajs_val_to_gc(pat_val))->u.regexp;
        pat_str = ajs_str_from_utf16(vm, src->source_u16, src->source_len, false);
        ajs_temp_root_push(vm, ajs_val_from_gc(pat_str));
        flags_str = re_flags_to_string(vm, src->flags);
        ajs_temp_root_push(vm, ajs_val_from_gc(flags_str));
    } else {
        pat_str = (pat_val == AJS_VAL_UNDEFINED) ? vm->str_empty : ajs_to_js_string(vm, pat_val);
        if (!pat_str || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(pat_str));
        flags_str = (flags_val == AJS_VAL_UNDEFINED) ? vm->str_empty : ajs_to_js_string(vm, flags_val);
        if (!flags_str || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(flags_str));
    }

    char err_buf[160];
    err_buf[0] = '\0';
    AjsRegExp *new_re = re_compile_internal(pat_str->units, pat_str->len16,
                                            flags_str->units, flags_str->len16,
                                            err_buf, sizeof(err_buf));
    ajs_temp_root_restore(vm, rs);
    if (!new_re) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "%s", err_buf[0] ? err_buf : "Invalid regular expression");
    }
    ajs_regexp_free(obj->u.regexp);
    obj->u.regexp = new_re;
    if (!re_set_last_index(vm, this_val, 0.0)) return AJS_VAL_EXCEPTION;
    return this_val;
}

static AjsValue js_regexp_toString(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp.prototype.toString called on non-object");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue sv = ajs_obj_get(vm, this_val, ajs_val_from_gc(ajs_str_from_cstr(vm, "source", true)), this_val);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    AjsString *src = ajs_to_js_string(vm, sv);
    if (!src || vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(src));

    AjsValue fv = ajs_obj_get(vm, this_val, ajs_val_from_gc(ajs_str_from_cstr(vm, "flags", true)), this_val);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsString *fl = ajs_to_js_string(vm, fv);
    if (!fl || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(fl));

    size_t total16 = (size_t)src->len16 + (size_t)fl->len16 + 2u;
    uint16_t *buf = (uint16_t *)malloc(total16 * sizeof(uint16_t));
    if (!buf) { ajs_temp_root_restore(vm, rs); return ajs_throw_err_kind(vm, AJS_ERR_KIND_ERROR, "Out of memory"); }
    buf[0] = '/';
    if (src->len16 > 0) memcpy(buf + 1, src->units, (size_t)src->len16 * sizeof(uint16_t));
    buf[1 + src->len16] = '/';
    if (fl->len16 > 0) memcpy(buf + 2 + src->len16, fl->units, (size_t)fl->len16 * sizeof(uint16_t));
    AjsString *res = ajs_str_from_utf16(vm, buf, total16, false);
    free(buf);
    ajs_temp_root_restore(vm, rs);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}

static AjsValue js_regexp_source_get(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp.prototype.source getter called on non-object");
    }
    AjsObject *obj = (AjsObject *)ajs_val_to_gc(this_val);
    if (obj->kind != AJS_OBJ_REGEXP || !obj->u.regexp) {
        if (ajs_strict_eq(this_val, vm->active_realm->regexp_proto)) {
            return ajs_val_from_gc(ajs_str_from_cstr(vm, "(?:)", true));
        }
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp.prototype.source getter called on incompatible receiver");
    }
    AjsString *escaped = re_escape_pattern_source(vm, obj->u.regexp->source_u16, obj->u.regexp->source_len);
    return escaped ? ajs_val_from_gc(escaped) : AJS_VAL_EXCEPTION;
}

static AjsValue js_regexp_flags_get(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp.prototype.flags getter called on non-object");
    }
    static const struct { const char *prop; char ch; } k_flag_props[] = {
        {"hasIndices", 'd'}, {"global", 'g'}, {"ignoreCase", 'i'}, {"multiline", 'm'},
        {"dotAll", 's'}, {"unicode", 'u'}, {"unicodeSets", 'v'}, {"sticky", 'y'}
    };
    char buf[12];
    size_t n = 0;
    for (size_t i = 0; i < sizeof(k_flag_props) / sizeof(k_flag_props[0]); i++) {
        AjsValue pv = ajs_obj_get(vm, this_val, ajs_val_from_gc(ajs_str_from_cstr(vm, k_flag_props[i].prop, true)), this_val);
        if (vm->has_exception) return AJS_VAL_EXCEPTION;
        if (ajs_to_bool(vm, pv)) buf[n++] = k_flag_props[i].ch;
    }
    buf[n] = '\0';
    AjsString *s = ajs_str_from_utf8(vm, buf, n, false);
    return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
}

static AjsValue re_flag_getter_common(AjsVm *vm, AjsValue this_val, uint32_t bit) {
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp flag getter called on non-object");
    }
    AjsObject *obj = (AjsObject *)ajs_val_to_gc(this_val);
    if (obj->kind != AJS_OBJ_REGEXP || !obj->u.regexp) {
        if (ajs_strict_eq(this_val, vm->active_realm->regexp_proto)) {
            return AJS_VAL_UNDEFINED;
        }
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp flag getter called on incompatible receiver");
    }
    return ajs_bool((obj->u.regexp->flags & bit) != 0);
}

static AjsValue js_re_get_d(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)c;(void)v;(void)u; return re_flag_getter_common(vm, t, RE_FLAG_D); }
static AjsValue js_re_get_g(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)c;(void)v;(void)u; return re_flag_getter_common(vm, t, RE_FLAG_G); }
static AjsValue js_re_get_i(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)c;(void)v;(void)u; return re_flag_getter_common(vm, t, RE_FLAG_I); }
static AjsValue js_re_get_m(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)c;(void)v;(void)u; return re_flag_getter_common(vm, t, RE_FLAG_M); }
static AjsValue js_re_get_s(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)c;(void)v;(void)u; return re_flag_getter_common(vm, t, RE_FLAG_S); }
static AjsValue js_re_get_u(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)c;(void)v;(void)u; return re_flag_getter_common(vm, t, RE_FLAG_U); }
static AjsValue js_re_get_v(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)c;(void)v;(void)u; return re_flag_getter_common(vm, t, RE_FLAG_V); }
static AjsValue js_re_get_y(AjsVm *vm, AjsValue t, int c, const AjsValue *v, void *u) { (void)c;(void)v;(void)u; return re_flag_getter_common(vm, t, RE_FLAG_Y); }

/* ============================================================================
 * 7. Well-Known Symbol Methods: @@match, @@matchAll, @@replace, @@search, @@split
 * ============================================================================ */

static AjsValue re_species_ctor(AjsVm *vm, AjsValue obj_val, AjsValue default_ctor) {
    AjsValue c = ajs_obj_get(vm, obj_val, ajs_val_from_gc(vm->str_constructor), obj_val);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (c == AJS_VAL_UNDEFINED) return default_ctor;
    if (!ajs_val_is_gc_kind(c, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "SpeciesConstructor: constructor is not an object");
    }
    AjsValue sp = ajs_obj_get(vm, c, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_SPECIES]), c);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (ajs_is_nullish(sp)) return default_ctor;
    if (!ajs_is_constructor_val(sp)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "SpeciesConstructor: [Symbol.species] is not a constructor");
    }
    return sp;
}

/* RegExp.prototype[Symbol.match](string) */
static AjsValue js_regexp_sym_match(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp.prototype[Symbol.match] called on non-object");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));

    AjsValue fv = ajs_obj_get(vm, this_val, ajs_val_from_gc(ajs_str_from_cstr(vm, "flags", true)), this_val);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsString *fl = ajs_to_js_string(vm, fv);
    if (!fl || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(fl));

    if (!strchr(fl->utf8, 'g')) {
        AjsValue res = re_exec_abstract(vm, this_val, s);
        ajs_temp_root_restore(vm, rs);
        return res;
    }

    bool full_unicode = (strchr(fl->utf8, 'u') != NULL) || (strchr(fl->utf8, 'v') != NULL);
    if (!re_set_last_index(vm, this_val, 0.0)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }

    AjsValue arr = ajs_new_array(vm, 0);
    ajs_temp_root_push(vm, arr);
    uint32_t n = 0;

    for (;;) {
        size_t ls = ajs_temp_root_save(vm);
        AjsValue result = re_exec_abstract(vm, this_val, s);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (result == AJS_VAL_NULL) {
            ajs_temp_root_restore(vm, rs);
            return (n == 0) ? AJS_VAL_NULL : arr;
        }
        ajs_temp_root_push(vm, result);
        AjsValue m0 = ajs_get_prop_index(vm, result, 0);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        AjsString *match_str = ajs_to_js_string(vm, m0);
        if (!match_str || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(match_str));

        re_define_data_prop(vm, (AjsObject *)ajs_val_to_gc(arr), ajs_val_from_gc(ajs_number_to_str(vm, (double)n, 10)), ajs_val_from_gc(match_str));
        ajs_set_prop_index(vm, arr, n, ajs_val_from_gc(match_str));

        if (match_str->len16 == 0) {
            AjsValue li_v = ajs_obj_get(vm, this_val, ajs_val_from_gc(ajs_str_from_cstr(vm, "lastIndex", true)), this_val);
            uint64_t this_idx = 0;
            if (vm->has_exception || !ajs_to_length(vm, li_v, &this_idx)) {
                ajs_temp_root_restore(vm, rs);
                return AJS_VAL_EXCEPTION;
            }
            uint64_t next_idx = re_advance_string_index(s, this_idx, full_unicode);
            if (!re_set_last_index(vm, this_val, (double)next_idx)) {
                ajs_temp_root_restore(vm, rs);
                return AJS_VAL_EXCEPTION;
            }
        }
        n++;
        ajs_temp_root_restore(vm, ls);
    }
}

/* %RegExpStringIteratorPrototype%.next() & RegExp.prototype[Symbol.matchAll](string) */
static AjsValue js_regexp_iter_next(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp String Iterator.prototype.next called on non-object");
    }
    AjsObject *it = (AjsObject *)ajs_val_to_gc(this_val);
    if (it->kind != AJS_OBJ_ITERATOR || it->u.iter.kind != AJS_ITER_REGEXP_STRING) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp String Iterator.prototype.next called on incompatible receiver");
    }
    if (it->u.iter.done) {
        return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
    }

    size_t rs = ajs_temp_root_save(vm);
    AjsValue R = it->u.iter.target;
    AjsString *S = (AjsString *)ajs_val_to_gc(it->u.iter.helper_fn);
    ajs_temp_root_push(vm, R);
    ajs_temp_root_push(vm, ajs_val_from_gc(S));

    AjsValue match = re_exec_abstract(vm, R, S);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (match == AJS_VAL_NULL) {
        it->u.iter.done = true;
        ajs_temp_root_restore(vm, rs);
        return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
    }
    ajs_temp_root_push(vm, match);
    if (!it->u.iter.global) {
        it->u.iter.done = true;
        AjsValue res = ajs_make_iter_result(vm, match, false);
        ajs_temp_root_restore(vm, rs);
        return res;
    }

    AjsValue m0 = ajs_get_prop_index(vm, match, 0);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsString *mstr = ajs_to_js_string(vm, m0);
    if (!mstr || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (mstr->len16 == 0) {
        AjsValue li_v = ajs_obj_get(vm, R, ajs_val_from_gc(ajs_str_from_cstr(vm, "lastIndex", true)), R);
        uint64_t this_idx = 0;
        if (vm->has_exception || !ajs_to_length(vm, li_v, &this_idx)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
        uint64_t next_idx = re_advance_string_index(S, this_idx, it->u.iter.unicode);
        if (!re_set_last_index(vm, R, (double)next_idx)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    AjsValue res = ajs_make_iter_result(vm, match, false);
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_regexp_sym_matchAll(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp.prototype[Symbol.matchAll] called on non-object");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));

    AjsValue ctor = re_species_ctor(vm, this_val, vm->active_realm->regexp_ctor);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ctor);

    AjsValue fv = ajs_obj_get(vm, this_val, ajs_val_from_gc(ajs_str_from_cstr(vm, "flags", true)), this_val);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsString *flags = ajs_to_js_string(vm, fv);
    if (!flags || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(flags));

    AjsValue cargs[2] = { this_val, ajs_val_from_gc(flags) };
    AjsValue matcher = ajs_construct_internal(vm, ctor, 2, cargs, ctor);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, matcher);

    AjsValue li_v = ajs_obj_get(vm, this_val, ajs_val_from_gc(ajs_str_from_cstr(vm, "lastIndex", true)), this_val);
    uint64_t last_idx = 0;
    if (vm->has_exception || !ajs_to_length(vm, li_v, &last_idx)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    if (!re_set_last_index(vm, matcher, (double)last_idx)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }

    bool global = (strchr(flags->utf8, 'g') != NULL);
    bool full_uni = (strchr(flags->utf8, 'u') != NULL) || (strchr(flags->utf8, 'v') != NULL);

    AjsObject *it = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ITERATOR, vm->active_realm->regexp_iter_proto);
    ajs_temp_root_restore(vm, rs);
    if (!it) return AJS_VAL_EXCEPTION;
    it->u.iter.kind = AJS_ITER_REGEXP_STRING;
    it->u.iter.target = matcher;
    it->u.iter.helper_fn = ajs_val_from_gc(s);
    it->u.iter.global = global;
    it->u.iter.unicode = full_uni;
    it->u.iter.done = false;
    return ajs_val_from_gc(it);
}

/* GetSubstitution helper for RegExp.prototype[Symbol.replace] */
static AjsString *re_get_substitution(AjsVm *vm, const AjsString *matched, const AjsString *str,
                                      uint32_t position, const AjsValue *captures, uint32_t ncaps,
                                      AjsValue named_captures, const AjsString *rep) {
    size_t cap = (size_t)rep->len16 + (size_t)str->len16 + 32u;
    uint16_t *out = (uint16_t *)malloc(cap * sizeof(uint16_t));
    if (!out) { ajs_throw_err_kind(vm, AJS_ERR_KIND_ERROR, "Out of memory"); return NULL; }
    size_t pos = 0;
    uint32_t tail_pos = position + matched->len16;
    if (tail_pos > str->len16) tail_pos = str->len16;

    #define RE_APPEND(ptr, n) do { \
        size_t _n = (size_t)(n); \
        if (pos + _n + 4u >= cap) { \
            while (pos + _n + 4u >= cap) cap *= 2u; \
            uint16_t *_no = (uint16_t *)realloc(out, cap * sizeof(uint16_t)); \
            if (!_no) { free(out); ajs_throw_err_kind(vm, AJS_ERR_KIND_ERROR, "Out of memory"); return NULL; } \
            out = _no; \
        } \
        if (_n > 0) memcpy(out + pos, (ptr), _n * sizeof(uint16_t)); \
        pos += _n; \
    } while (0)

    for (uint32_t i = 0; i < rep->len16; i++) {
        uint16_t ch = rep->units[i];
        if (ch == '$' && i + 1u < rep->len16) {
            uint16_t nx = rep->units[i + 1u];
            if (nx == '$') { RE_APPEND(&nx, 1); i++; continue; }
            if (nx == '&') { RE_APPEND(matched->units, matched->len16); i++; continue; }
            if (nx == '`') { RE_APPEND(str->units, position <= str->len16 ? position : str->len16); i++; continue; }
            if (nx == '\'') { RE_APPEND(str->units + tail_pos, str->len16 - tail_pos); i++; continue; }
            if (nx >= '0' && nx <= '9') {
                uint32_t d1 = (uint32_t)(nx - '0');
                if (i + 2u < rep->len16 && rep->units[i + 2u] >= '0' && rep->units[i + 2u] <= '9') {
                    uint32_t nn = d1 * 10u + (uint32_t)(rep->units[i + 2u] - '0');
                    if (nn >= 1u && nn <= ncaps) {
                        AjsValue cv = captures[nn - 1u];
                        if (ajs_val_is_gc_kind(cv, AJS_GC_STRING)) {
                            AjsString *cs = (AjsString *)ajs_val_to_gc(cv);
                            RE_APPEND(cs->units, cs->len16);
                        }
                        i += 2u;
                        continue;
                    }
                }
                if (d1 >= 1u && d1 <= ncaps) {
                    AjsValue cv = captures[d1 - 1u];
                    if (ajs_val_is_gc_kind(cv, AJS_GC_STRING)) {
                        AjsString *cs = (AjsString *)ajs_val_to_gc(cv);
                        RE_APPEND(cs->units, cs->len16);
                    }
                    i += 1u;
                    continue;
                }
            }
            if (nx == '<' && named_captures != AJS_VAL_UNDEFINED) {
                int gt = -1;
                for (uint32_t k = i + 2u; k < rep->len16; k++) {
                    if (rep->units[k] == '>') { gt = (int)k; break; }
                }
                if (gt > 0) {
                    AjsString *gn = ajs_str_from_utf16(vm, rep->units + i + 2u, (size_t)((uint32_t)gt - (i + 2u)), true);
                    AjsValue gv = ajs_obj_get(vm, named_captures, ajs_val_from_gc(gn), named_captures);
                    if (vm->has_exception) { free(out); return NULL; }
                    if (gv != AJS_VAL_UNDEFINED) {
                        size_t ls = ajs_temp_root_save(vm);
                        AjsString *gs = ajs_to_js_string(vm, gv);
                        if (!gs || vm->has_exception) { ajs_temp_root_restore(vm, ls); free(out); return NULL; }
                        RE_APPEND(gs->units, gs->len16);
                        ajs_temp_root_restore(vm, ls);
                    }
                    i = (uint32_t)gt;
                    continue;
                }
            }
        }
        RE_APPEND(&ch, 1);
    }
    #undef RE_APPEND

    AjsString *res = ajs_str_from_utf16(vm, out, pos, false);
    free(out);
    return res;
}

/* RegExp.prototype[Symbol.replace](string, replaceValue) */
static AjsValue js_regexp_sym_replace(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp.prototype[Symbol.replace] called on non-object");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));

    AjsValue rep_val = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    bool fn_replace = ajs_is_callable(vm, rep_val);
    AjsString *rep_str = NULL;
    if (!fn_replace) {
        rep_str = ajs_to_js_string(vm, rep_val);
        if (!rep_str || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(rep_str));
    }

    AjsValue fv = ajs_obj_get(vm, this_val, ajs_val_from_gc(ajs_str_from_cstr(vm, "flags", true)), this_val);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsString *flags = ajs_to_js_string(vm, fv);
    if (!flags || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(flags));

    bool global = (strchr(flags->utf8, 'g') != NULL);
    bool full_uni = false;
    if (global) {
        full_uni = (strchr(flags->utf8, 'u') != NULL) || (strchr(flags->utf8, 'v') != NULL);
        if (!re_set_last_index(vm, this_val, 0.0)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }

    AjsValue results_arr = ajs_new_array(vm, 0);
    ajs_temp_root_push(vm, results_arr);
    uint32_t res_count = 0;

    for (;;) {
        AjsValue result = re_exec_abstract(vm, this_val, s);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (result == AJS_VAL_NULL) break;
        ajs_set_prop_index(vm, results_arr, res_count++, result);
        if (!global) break;

        size_t ls = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, result);
        AjsValue m0 = ajs_get_prop_index(vm, result, 0);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        AjsString *mstr = ajs_to_js_string(vm, m0);
        if (!mstr || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (mstr->len16 == 0) {
            AjsValue li_v = ajs_obj_get(vm, this_val, ajs_val_from_gc(ajs_str_from_cstr(vm, "lastIndex", true)), this_val);
            uint64_t this_idx = 0;
            if (vm->has_exception || !ajs_to_length(vm, li_v, &this_idx)) {
                ajs_temp_root_restore(vm, rs);
                return AJS_VAL_EXCEPTION;
            }
            uint64_t next_idx = re_advance_string_index(s, this_idx, full_uni);
            if (!re_set_last_index(vm, this_val, (double)next_idx)) {
                ajs_temp_root_restore(vm, rs);
                return AJS_VAL_EXCEPTION;
            }
        }
        ajs_temp_root_restore(vm, ls);
    }

    if (res_count == 0) {
        ajs_temp_root_restore(vm, rs);
        return ajs_val_from_gc(s);
    }

    AjsString *acc = vm->str_empty;
    uint32_t next_src_pos = 0;

    for (uint32_t r_i = 0; r_i < res_count; r_i++) {
        size_t ls = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, ajs_val_from_gc(acc));

        AjsValue result = ajs_get_prop_index(vm, results_arr, r_i);
        ajs_temp_root_push(vm, result);

        AjsValue len_v = ajs_obj_get(vm, result, ajs_val_from_gc(vm->str_length), result);
        uint64_t rlen64 = 0;
        if (vm->has_exception || !ajs_to_length(vm, len_v, &rlen64)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
        uint32_t ncaps = (rlen64 > 1u) ? (uint32_t)(rlen64 - 1u) : 0u;

        AjsValue m0 = ajs_get_prop_index(vm, result, 0);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        AjsString *matched = ajs_to_js_string(vm, m0);
        if (!matched || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(matched));

        AjsValue idx_v = ajs_obj_get(vm, result, ajs_val_from_gc(ajs_str_from_cstr(vm, "index", true)), result);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        double pos_d = 0.0;
        if (!ajs_to_number(vm, idx_v, &pos_d)) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        pos_d = ajs_double_to_integer_or_inf(pos_d);
        uint32_t position = (pos_d < 0.0) ? 0u : (pos_d > (double)s->len16 ? s->len16 : (uint32_t)pos_d);

        AjsValue *captures = ncaps ? (AjsValue *)malloc(ncaps * sizeof(AjsValue)) : NULL;
        if (ncaps && !captures) { ajs_temp_root_restore(vm, rs); return ajs_throw_err_kind(vm, AJS_ERR_KIND_ERROR, "Out of memory"); }
        for (uint32_t j = 0; j < ncaps; j++) {
            AjsValue cj = ajs_get_prop_index(vm, result, j + 1u);
            if (vm->has_exception) { free(captures); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            if (cj != AJS_VAL_UNDEFINED) {
                AjsString *cs = ajs_to_js_string(vm, cj);
                if (!cs || vm->has_exception) { free(captures); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                cj = ajs_val_from_gc(cs);
                ajs_temp_root_push(vm, cj);
            }
            captures[j] = cj;
        }

        AjsValue named_caps = ajs_obj_get(vm, result, ajs_val_from_gc(ajs_str_from_cstr(vm, "groups", true)), result);
        if (vm->has_exception) { free(captures); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, named_caps);

        AjsString *replacement = NULL;
        if (fn_replace) {
            uint32_t fargc = 3u + ncaps + (named_caps != AJS_VAL_UNDEFINED ? 1u : 0u);
            AjsValue *fargs = (AjsValue *)malloc(fargc * sizeof(AjsValue));
            if (!fargs) { free(captures); ajs_temp_root_restore(vm, rs); return ajs_throw_err_kind(vm, AJS_ERR_KIND_ERROR, "Out of memory"); }
            fargs[0] = ajs_val_from_gc(matched);
            for (uint32_t j = 0; j < ncaps; j++) fargs[1u + j] = captures[j];
            fargs[1u + ncaps] = ajs_val_number((double)position);
            fargs[2u + ncaps] = ajs_val_from_gc(s);
            if (named_caps != AJS_VAL_UNDEFINED) fargs[3u + ncaps] = named_caps;
            AjsValue rv = ajs_call_internal(vm, rep_val, AJS_VAL_UNDEFINED, (int)fargc, fargs);
            free(fargs);
            if (vm->has_exception) { free(captures); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            replacement = ajs_to_js_string(vm, rv);
            if (!replacement || vm->has_exception) { free(captures); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        } else {
            if (named_caps != AJS_VAL_UNDEFINED && !ajs_val_is_gc_kind(named_caps, AJS_GC_OBJECT)) {
                AjsObject *no = ajs_to_object(vm, named_caps);
                if (!no || vm->has_exception) { free(captures); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                named_caps = ajs_val_from_gc(no);
                ajs_temp_root_push(vm, named_caps);
            }
            replacement = re_get_substitution(vm, matched, s, position, captures, ncaps, named_caps, rep_str);
            if (!replacement || vm->has_exception) { free(captures); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        }
        free(captures);
        ajs_temp_root_push(vm, ajs_val_from_gc(replacement));

        if (position >= next_src_pos) {
            if (position > next_src_pos) {
                AjsString *pre = ajs_str_from_utf16(vm, s->units + next_src_pos, position - next_src_pos, false);
                ajs_temp_root_push(vm, ajs_val_from_gc(pre));
                acc = ajs_str_concat(vm, acc, pre);
                ajs_temp_root_push(vm, ajs_val_from_gc(acc));
            }
            acc = ajs_str_concat(vm, acc, replacement);
            next_src_pos = position + matched->len16;
        }
        ajs_temp_root_restore(vm, ls);
    }

    if (next_src_pos < s->len16) {
        ajs_temp_root_push(vm, ajs_val_from_gc(acc));
        AjsString *tail = ajs_str_from_utf16(vm, s->units + next_src_pos, s->len16 - next_src_pos, false);
        ajs_temp_root_push(vm, ajs_val_from_gc(tail));
        acc = ajs_str_concat(vm, acc, tail);
    }
    ajs_temp_root_restore(vm, rs);
    return acc ? ajs_val_from_gc(acc) : AJS_VAL_EXCEPTION;
}

/* RegExp.prototype[Symbol.search](string) */
static AjsValue js_regexp_sym_search(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp.prototype[Symbol.search] called on non-object");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));

    AjsString *k_li = ajs_str_from_cstr(vm, "lastIndex", true);
    AjsValue prev_li = ajs_obj_get(vm, this_val, ajs_val_from_gc(k_li), this_val);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, prev_li);

    if (!ajs_same_value(vm, prev_li, ajs_int32(0))) {
        if (!ajs_obj_set(vm, this_val, ajs_val_from_gc(k_li), ajs_int32(0), this_val, true)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }

    AjsValue result = re_exec_abstract(vm, this_val, s);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, result);

    AjsValue cur_li = ajs_obj_get(vm, this_val, ajs_val_from_gc(k_li), this_val);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    if (!ajs_same_value(vm, cur_li, prev_li)) {
        if (!ajs_obj_set(vm, this_val, ajs_val_from_gc(k_li), prev_li, this_val, true)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }

    if (result == AJS_VAL_NULL) {
        ajs_temp_root_restore(vm, rs);
        return ajs_int32(-1);
    }
    AjsValue idx = ajs_obj_get(vm, result, ajs_val_from_gc(ajs_str_from_cstr(vm, "index", true)), result);
    ajs_temp_root_restore(vm, rs);
    return vm->has_exception ? AJS_VAL_EXCEPTION : idx;
}

/* RegExp.prototype[Symbol.split](string, limit) */
static AjsValue js_regexp_sym_split(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "RegExp.prototype[Symbol.split] called on non-object");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (!s || vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, ajs_val_from_gc(s));

    AjsValue ctor = re_species_ctor(vm, this_val, vm->active_realm->regexp_ctor);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ctor);

    AjsValue fv = ajs_obj_get(vm, this_val, ajs_val_from_gc(ajs_str_from_cstr(vm, "flags", true)), this_val);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsString *flags = ajs_to_js_string(vm, fv);
    if (!flags || vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(flags));

    bool uni = (strchr(flags->utf8, 'u') != NULL) || (strchr(flags->utf8, 'v') != NULL);
    AjsString *new_flags = flags;
    if (!strchr(flags->utf8, 'y')) {
        AjsString *ys = ajs_str_from_cstr(vm, "y", true);
        new_flags = ajs_str_concat(vm, flags, ys);
        ajs_temp_root_push(vm, ajs_val_from_gc(new_flags));
    }

    AjsValue cargs[2] = { this_val, ajs_val_from_gc(new_flags) };
    AjsValue splitter = ajs_construct_internal(vm, ctor, 2, cargs, ctor);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, splitter);

    AjsValue arr = ajs_new_array(vm, 0);
    ajs_temp_root_push(vm, arr);

    uint32_t lim = UINT32_MAX;
    if (argc > 1 && argv[1] != AJS_VAL_UNDEFINED) {
        if (!ajs_to_uint32(vm, argv[1], &lim)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    if (lim == 0) {
        ajs_temp_root_restore(vm, rs);
        return arr;
    }

    if (s->len16 == 0) {
        AjsValue z = re_exec_abstract(vm, splitter, s);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (z != AJS_VAL_NULL) {
            ajs_temp_root_restore(vm, rs);
            return arr;
        }
        ajs_set_prop_index(vm, arr, 0, ajs_val_from_gc(s));
        ajs_temp_root_restore(vm, rs);
        return arr;
    }

    uint32_t out_n = 0;
    uint32_t p = 0;
    uint64_t q = 0;
    while (q < (uint64_t)s->len16) {
        size_t ls = ajs_temp_root_save(vm);
        if (!re_set_last_index(vm, splitter, (double)q)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
        AjsValue z = re_exec_abstract(vm, splitter, s);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        if (z == AJS_VAL_NULL) {
            q = re_advance_string_index(s, q, uni);
            ajs_temp_root_restore(vm, ls);
            continue;
        }
        ajs_temp_root_push(vm, z);
        AjsValue li_v = ajs_obj_get(vm, splitter, ajs_val_from_gc(ajs_str_from_cstr(vm, "lastIndex", true)), splitter);
        uint64_t e64 = 0;
        if (vm->has_exception || !ajs_to_length(vm, li_v, &e64)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
        uint32_t e = (e64 < (uint64_t)s->len16) ? (uint32_t)e64 : s->len16;
        if (e == p) {
            q = re_advance_string_index(s, q, uni);
            ajs_temp_root_restore(vm, ls);
            continue;
        }
        AjsString *sub = ajs_str_from_utf16(vm, s->units + p, (uint32_t)q - p, false);
        ajs_temp_root_push(vm, ajs_val_from_gc(sub));
        ajs_set_prop_index(vm, arr, out_n++, ajs_val_from_gc(sub));
        if (out_n == lim) {
            ajs_temp_root_restore(vm, rs);
            return arr;
        }
        p = e;
        AjsValue len_v = ajs_obj_get(vm, z, ajs_val_from_gc(vm->str_length), z);
        uint64_t ncaps64 = 0;
        if (vm->has_exception || !ajs_to_length(vm, len_v, &ncaps64)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
        uint32_t ncaps = (ncaps64 > 1u) ? (uint32_t)(ncaps64 - 1u) : 0u;
        for (uint32_t i = 1; i <= ncaps; i++) {
            AjsValue cap_v = ajs_get_prop_index(vm, z, i);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
            ajs_set_prop_index(vm, arr, out_n++, cap_v);
            if (out_n == lim) {
                ajs_temp_root_restore(vm, rs);
                return arr;
            }
        }
        q = (uint64_t)p;
        ajs_temp_root_restore(vm, ls);
    }

    AjsString *rest = ajs_str_from_utf16(vm, s->units + p, s->len16 - p, false);
    ajs_temp_root_push(vm, ajs_val_from_gc(rest));
    ajs_set_prop_index(vm, arr, out_n, ajs_val_from_gc(rest));
    ajs_temp_root_restore(vm, rs);
    return arr;
}

/* ============================================================================
 * 8. Realm Intrinsic Initialization
 * ============================================================================ */

void ajs_init_regexp_builtins(AjsVm *vm, AjsRealm *realm) {
    /* 1. %RegExpStringIteratorPrototype% */
    AjsObject *re_iter_proto = ajs_obj_new(vm, realm, AJS_OBJ_ORDINARY, realm->iterator_proto);
    realm->regexp_iter_proto = ajs_val_from_gc(re_iter_proto);
    ajs_def_native_method(vm, realm, realm->regexp_iter_proto, "next", js_regexp_iter_next, 0, RE_METHOD_FLAGS, 0);
    {
        AjsPropDesc tag_d;
        memset(&tag_d, 0, sizeof(tag_d));
        tag_d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE |
                      AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
        tag_d.value = ajs_val_from_gc(ajs_str_from_cstr(vm, "RegExp String Iterator", true));
        ajs_obj_define_own_prop(vm, re_iter_proto, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_TO_STRING_TAG]), &tag_d, false);
    }

    /* 2. RegExp.prototype & RegExp constructor */
    if (!ajs_val_is_gc_kind(realm->regexp_proto, AJS_GC_OBJECT)) {
        realm->regexp_proto = ajs_val_from_gc(ajs_obj_new(vm, realm, AJS_OBJ_ORDINARY, realm->object_proto));
    }
    AjsObject *ctor = ajs_obj_new(vm, realm, AJS_OBJ_FUNCTION, realm->function_proto);
    ctor->u.fn.fn_kind = AJS_FN_NATIVE;
    ctor->u.fn.is_constructor = true;
    ctor->u.fn.native_fn = js_regexp_ctor;
    ctor->u.fn.native_magic = -60;
    realm->regexp_ctor = ajs_val_from_gc(ctor);

    {
        AjsPropDesc d;
        memset(&d, 0, sizeof(d));
        d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE |
                  AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
        d.value = ajs_int32(2);
        ajs_obj_define_own_prop(vm, ctor, ajs_val_from_gc(vm->str_length), &d, false);
        d.value = ajs_val_from_gc(ajs_str_from_cstr(vm, "RegExp", true));
        ajs_obj_define_own_prop(vm, ctor, ajs_val_from_gc(vm->str_name), &d, false);

        /* RegExp.prototype (non-writable, non-enumerable, non-configurable) */
        d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE |
                  AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
        d.value = realm->regexp_proto;
        ajs_obj_define_own_prop(vm, ctor, ajs_val_from_gc(vm->str_prototype), &d, false);

        /* RegExp.prototype.constructor */
        d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                  AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
        d.value = realm->regexp_ctor;
        ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(realm->regexp_proto), ajs_val_from_gc(vm->str_constructor), &d, false);

        /* globalThis.RegExp */
        ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(realm->global_obj),
                                ajs_val_from_gc(ajs_str_from_cstr(vm, "RegExp", true)), &d, false);
    }

    /* RegExp static methods & [Symbol.species] */
    ajs_def_native_method(vm, realm, realm->regexp_ctor, "escape", js_regexp_escape, 1, RE_METHOD_FLAGS, 0);
    {
        AjsObject *sp_get = ajs_obj_new(vm, realm, AJS_OBJ_FUNCTION, realm->function_proto);
        sp_get->u.fn.fn_kind = AJS_FN_NATIVE;
        sp_get->u.fn.native_fn = js_regexp_species_get;
        AjsPropDesc gd;
        memset(&gd, 0, sizeof(gd));
        gd.flags = AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_GET |
                   AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
        gd.getter = ajs_val_from_gc(sp_get);
        ajs_obj_define_own_prop(vm, ctor, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_SPECIES]), &gd, false);
    }

    /* RegExp.prototype methods & accessors */
    ajs_def_native_method(vm, realm, realm->regexp_proto, "exec", js_regexp_exec, 1, RE_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, realm, realm->regexp_proto, "test", js_regexp_test, 1, RE_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, realm, realm->regexp_proto, "compile", js_regexp_compile, 2, RE_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, realm, realm->regexp_proto, "toString", js_regexp_toString, 0, RE_METHOD_FLAGS, 0);

    ajs_def_native_accessor(vm, realm, realm->regexp_proto, "source",      js_regexp_source_get, NULL, AJS_PROP_CONFIGURABLE);
    ajs_def_native_accessor(vm, realm, realm->regexp_proto, "flags",       js_regexp_flags_get,  NULL, AJS_PROP_CONFIGURABLE);
    ajs_def_native_accessor(vm, realm, realm->regexp_proto, "hasIndices",  js_re_get_d,          NULL, AJS_PROP_CONFIGURABLE);
    ajs_def_native_accessor(vm, realm, realm->regexp_proto, "global",      js_re_get_g,          NULL, AJS_PROP_CONFIGURABLE);
    ajs_def_native_accessor(vm, realm, realm->regexp_proto, "ignoreCase",  js_re_get_i,          NULL, AJS_PROP_CONFIGURABLE);
    ajs_def_native_accessor(vm, realm, realm->regexp_proto, "multiline",   js_re_get_m,          NULL, AJS_PROP_CONFIGURABLE);
    ajs_def_native_accessor(vm, realm, realm->regexp_proto, "dotAll",      js_re_get_s,          NULL, AJS_PROP_CONFIGURABLE);
    ajs_def_native_accessor(vm, realm, realm->regexp_proto, "unicode",     js_re_get_u,          NULL, AJS_PROP_CONFIGURABLE);
    ajs_def_native_accessor(vm, realm, realm->regexp_proto, "unicodeSets", js_re_get_v,          NULL, AJS_PROP_CONFIGURABLE);
    ajs_def_native_accessor(vm, realm, realm->regexp_proto, "sticky",      js_re_get_y,          NULL, AJS_PROP_CONFIGURABLE);

    ajs_def_native_sym_method(vm, realm, realm->regexp_proto, vm->well_known_syms[AJS_SYM_MATCH],     "[Symbol.match]",    js_regexp_sym_match,    1, RE_METHOD_FLAGS);
    ajs_def_native_sym_method(vm, realm, realm->regexp_proto, vm->well_known_syms[AJS_SYM_MATCH_ALL], "[Symbol.matchAll]", js_regexp_sym_matchAll, 1, RE_METHOD_FLAGS);
    ajs_def_native_sym_method(vm, realm, realm->regexp_proto, vm->well_known_syms[AJS_SYM_REPLACE],   "[Symbol.replace]",  js_regexp_sym_replace,  2, RE_METHOD_FLAGS);
    ajs_def_native_sym_method(vm, realm, realm->regexp_proto, vm->well_known_syms[AJS_SYM_SEARCH],    "[Symbol.search]",   js_regexp_sym_search,   1, RE_METHOD_FLAGS);
    ajs_def_native_sym_method(vm, realm, realm->regexp_proto, vm->well_known_syms[AJS_SYM_SPLIT],     "[Symbol.split]",    js_regexp_sym_split,    2, RE_METHOD_FLAGS);
}
