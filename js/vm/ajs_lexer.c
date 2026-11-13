/*
 * ArcradientJS 1.0 - js/vm/ajs_lexer.c
 * Clean-room Pure C11 ECMAScript 2026 Lexer with 1-based line:col, UTF-16 strings,
 * Template TRV/Cooked scanning, RegExp rescanning, BigInt & Strict Octal validation
 */
#include "ajs_parse_internal.h"

void *ajs_arena_alloc(AjsAstArena *arena, size_t size) {
    size = (size + 7u) & ~(size_t)7u;
    if (size == 0) size = 8u;
    AjsAstBlockChunk *cur = arena->head;
    if (!cur || cur->used + size > cur->cap) {
        size_t cap = size > 16384u ? size * 2u : 32768u;
        AjsAstBlockChunk *blk = (AjsAstBlockChunk *)calloc(1, sizeof(AjsAstBlockChunk) + cap);
        if (!blk) return NULL;
        blk->cap = cap;
        blk->used = 0;
        blk->next = arena->head;
        arena->head = blk;
        cur = blk;
    }
    void *ptr = cur->data + cur->used;
    cur->used += size;
    return ptr;
}

char *ajs_arena_strdup(AjsAstArena *arena, const char *s, size_t len) {
    char *dst = (char *)ajs_arena_alloc(arena, len + 1u);
    if (!dst) return NULL;
    ajs_memcpy(dst, s, len);
    dst[len] = '\0';
    return dst;
}

void ajs_arena_free(AjsAstArena *arena) {
    AjsAstBlockChunk *cur = arena->head;
    arena->head = NULL;
    while (cur) {
        AjsAstBlockChunk *nxt = cur->next;
        free(cur);
        cur = nxt;
    }
}

static uint32_t lex_peek_cp(const AjsLexer *lex, size_t *out_adv) {
    if (lex->pos >= lex->len) {
        if (out_adv) *out_adv = 0;
        return 0;
    }
    uint8_t b = (uint8_t)lex->src[lex->pos];
    if (b < 0x80u) {
        if (out_adv) *out_adv = 1;
        return b;
    }
    return ajs_utf8_decode_one(lex->src + lex->pos, lex->len - lex->pos, out_adv);
}

static void lex_advance_cp(AjsLexer *lex, uint32_t cp, size_t adv) {
    if (cp == '\r') {
        lex->pos += adv;
        if (lex->pos < lex->len && lex->src[lex->pos] == '\n') lex->pos++;
        lex->line++;
        lex->col = 1;
        lex->at_line_start = true;
    } else if (ajs_unicode_is_line_terminator(cp)) {
        lex->pos += adv;
        lex->line++;
        lex->col = 1;
        lex->at_line_start = true;
    } else {
        lex->pos += adv;
        lex->col += (cp > 0xFFFFu) ? 2u : 1u;
    }
}

static AjsToken lex_error(AjsLexer *lex, uint32_t line, uint32_t col, const char *msg) {
    if (lex->err_msg[0] == '\0') {
        snprintf(lex->err_msg, sizeof(lex->err_msg), "%s", msg);
        lex->err_line = line;
        lex->err_col = col;
    }
    AjsToken t;
    memset(&t, 0, sizeof(t));
    t.kind = TOK_ERROR;
    t.line = line;
    t.col = col;
    return t;
}

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool lex_read_unicode_escape(AjsLexer *lex, uint32_t *out_cp) {
    /* Assumes `\u` has been matched and `lex->pos` points right after `u` */
    if (lex->pos < lex->len && lex->src[lex->pos] == '{') {
        lex->pos++;
        lex->col++;
        uint32_t cp = 0;
        size_t digits = 0;
        while (lex->pos < lex->len && lex->src[lex->pos] != '}') {
            int hv = hex_val((uint8_t)lex->src[lex->pos]);
            if (hv < 0) return false;
            cp = (cp << 4) | (uint32_t)hv;
            if (cp > 0x10FFFFu) return false;
            lex->pos++;
            lex->col++;
            digits++;
        }
        if (digits == 0 || lex->pos >= lex->len || lex->src[lex->pos] != '}') return false;
        lex->pos++;
        lex->col++;
        *out_cp = cp;
        return true;
    }
    if (lex->pos + 4 > lex->len) return false;
    uint32_t cp = 0;
    for (int i = 0; i < 4; i++) {
        int hv = hex_val((uint8_t)lex->src[lex->pos + (size_t)i]);
        if (hv < 0) return false;
        cp = (cp << 4) | (uint32_t)hv;
    }
    lex->pos += 4;
    lex->col += 4;
    *out_cp = cp;
    return true;
}

static AjsTokenKind match_keyword(const uint16_t *u, uint32_t len) {
    char buf[16];
    if (len < 2 || len > 10) return TOK_IDENT;
    for (uint32_t i = 0; i < len; i++) {
        if (u[i] < 'a' || u[i] > 'z') return TOK_IDENT;
        buf[i] = (char)u[i];
    }
    buf[len] = '\0';
    switch (len) {
        case 2:
            if (memcmp(buf, "do", 2) == 0) return KW_DO;
            if (memcmp(buf, "if", 2) == 0) return KW_IF;
            if (memcmp(buf, "in", 2) == 0) return KW_IN;
            break;
        case 3:
            if (memcmp(buf, "for", 3) == 0) return KW_FOR;
            if (memcmp(buf, "let", 3) == 0) return KW_LET;
            if (memcmp(buf, "new", 3) == 0) return KW_NEW;
            if (memcmp(buf, "try", 3) == 0) return KW_TRY;
            if (memcmp(buf, "var", 3) == 0) return KW_VAR;
            break;
        case 4:
            if (memcmp(buf, "case", 4) == 0) return KW_CASE;
            if (memcmp(buf, "else", 4) == 0) return KW_ELSE;
            if (memcmp(buf, "enum", 4) == 0) return KW_ENUM;
            if (memcmp(buf, "null", 4) == 0) return KW_NULL;
            if (memcmp(buf, "this", 4) == 0) return KW_THIS;
            if (memcmp(buf, "true", 4) == 0) return KW_TRUE;
            if (memcmp(buf, "void", 4) == 0) return KW_VOID;
            if (memcmp(buf, "with", 4) == 0) return KW_WITH;
            break;
        case 5:
            if (memcmp(buf, "await", 5) == 0) return KW_AWAIT;
            if (memcmp(buf, "break", 5) == 0) return KW_BREAK;
            if (memcmp(buf, "catch", 5) == 0) return KW_CATCH;
            if (memcmp(buf, "class", 5) == 0) return KW_CLASS;
            if (memcmp(buf, "const", 5) == 0) return KW_CONST;
            if (memcmp(buf, "false", 5) == 0) return KW_FALSE;
            if (memcmp(buf, "super", 5) == 0) return KW_SUPER;
            if (memcmp(buf, "throw", 5) == 0) return KW_THROW;
            if (memcmp(buf, "while", 5) == 0) return KW_WHILE;
            if (memcmp(buf, "yield", 5) == 0) return KW_YIELD;
            break;
        case 6:
            if (memcmp(buf, "delete", 6) == 0) return KW_DELETE;
            if (memcmp(buf, "export", 6) == 0) return KW_EXPORT;
            if (memcmp(buf, "import", 6) == 0) return KW_IMPORT;
            if (memcmp(buf, "return", 6) == 0) return KW_RETURN;
            if (memcmp(buf, "static", 6) == 0) return KW_STATIC;
            if (memcmp(buf, "switch", 6) == 0) return KW_SWITCH;
            if (memcmp(buf, "typeof", 6) == 0) return KW_TYPEOF;
            break;
        case 7:
            if (memcmp(buf, "default", 7) == 0) return KW_DEFAULT;
            if (memcmp(buf, "extends", 7) == 0) return KW_EXTENDS;
            if (memcmp(buf, "finally", 7) == 0) return KW_FINALLY;
            break;
        case 8:
            if (memcmp(buf, "continue", 8) == 0) return KW_CONTINUE;
            if (memcmp(buf, "debugger", 8) == 0) return KW_DEBUGGER;
            if (memcmp(buf, "function", 8) == 0) return KW_FUNCTION;
            break;
        case 10:
            if (memcmp(buf, "instanceof", 10) == 0) return KW_INSTANCEOF;
            break;
    }
    return TOK_IDENT;
}

static AjsToken lex_scan_ident(AjsLexer *lex, bool is_priv, uint32_t start_line, uint32_t start_col,
                               size_t start_off, bool had_nl) {
    uint16_t stack_buf[128];
    size_t cap = 128, ulen = 0;
    uint16_t *ubuf = stack_buf;
    bool has_escape = false;
    bool first = true;

    while (lex->pos < lex->len) {
        size_t adv = 0;
        uint32_t cp = lex_peek_cp(lex, &adv);
        if (cp == '\\') {
            if (lex->pos + 1 >= lex->len || lex->src[lex->pos + 1] != 'u') {
                if (ubuf != stack_buf) free(ubuf);
                return lex_error(lex, lex->line, lex->col, "Invalid Unicode escape in identifier");
            }
            lex->pos += 2;
            lex->col += 2;
            uint32_t esc_cp = 0;
            if (!lex_read_unicode_escape(lex, &esc_cp)) {
                if (ubuf != stack_buf) free(ubuf);
                return lex_error(lex, lex->line, lex->col, "Invalid Unicode escape sequence");
            }
            bool ok = first ? ajs_unicode_is_id_start(esc_cp) : ajs_unicode_is_id_continue(esc_cp);
            if (!ok) {
                if (ubuf != stack_buf) free(ubuf);
                return lex_error(lex, start_line, start_col, "Invalid escaped character in identifier");
            }
            has_escape = true;
            cp = esc_cp;
        } else {
            bool ok = first ? ajs_unicode_is_id_start(cp) : ajs_unicode_is_id_continue(cp);
            if (!ok) break;
            lex_advance_cp(lex, cp, adv);
        }
        first = false;
        if (ulen + 2 >= cap) {
            size_t ncap = cap * 2u;
            uint16_t *nb = (uint16_t *)malloc(ncap * sizeof(uint16_t));
            ajs_memcpy(nb, ubuf, ulen * sizeof(uint16_t));
            if (ubuf != stack_buf) free(ubuf);
            ubuf = nb;
            cap = ncap;
        }
        uint16_t pair[2];
        size_t n = ajs_code_point_to_utf16(cp, pair);
        ubuf[ulen++] = pair[0];
        if (n == 2) ubuf[ulen++] = pair[1];
    }

    if (first) {
        if (ubuf != stack_buf) free(ubuf);
        return lex_error(lex, start_line, start_col, "Invalid or unexpected token");
    }

    uint16_t *au = (uint16_t *)ajs_arena_alloc(lex->arena, (ulen + 1u) * sizeof(uint16_t));
    if (is_priv) {
        uint16_t *pu = (uint16_t *)ajs_arena_alloc(lex->arena, (ulen + 2u) * sizeof(uint16_t));
        pu[0] = '#';
        ajs_memcpy(pu + 1, ubuf, ulen * sizeof(uint16_t));
        pu[ulen + 1] = 0;
        au = pu;
        ulen += 1u;
    } else {
        ajs_memcpy(au, ubuf, ulen * sizeof(uint16_t));
        au[ulen] = 0;
    }
    if (ubuf != stack_buf) free(ubuf);

    /* Materialize UTF-8 copy in arena for convenience */
    size_t u8_cap = ulen * 3u + 1u;
    char *u8 = (char *)ajs_arena_alloc(lex->arena, u8_cap);
    size_t p8 = 0;
    for (size_t i = 0; i < ulen; ) {
        uint32_t adv16 = 1;
        uint32_t cp = ajs_utf16_code_point_at(au, (uint32_t)ulen, (uint32_t)i, &adv16);
        p8 += ajs_code_point_to_utf8(cp, u8 + p8);
        i += adv16;
    }
    u8[p8] = '\0';

    AjsToken t;
    memset(&t, 0, sizeof(t));
    t.line = start_line;
    t.col = start_col;
    t.start_off = start_off;
    t.end_off = lex->pos;
    t.had_newline = had_nl;
    t.has_escape = has_escape;
    t.u16_data = au;
    t.u16_len = (uint32_t)ulen;
    t.utf8_text = u8;
    t.utf8_len = p8;

    if (is_priv) {
        t.kind = TOK_PRIV_IDENT;
    } else {
        AjsTokenKind kw = match_keyword(au, (uint32_t)ulen);
        if (kw != TOK_IDENT && has_escape) {
            /* Contextual keywords like let/yield/await/static stay TOK_IDENT with has_escape=true so parser checks context;
               unconditional reserved words with escapes are always SyntaxError! */
            if (kw != KW_LET && kw != KW_YIELD && kw != KW_AWAIT && kw != KW_STATIC) {
                return lex_error(lex, start_line, start_col, "Keyword must not contain escaped characters");
            }
            t.kind = kw;
        } else {
            t.kind = kw;
        }
    }
    return t;
}

static bool is_radix_digit(int c, int base) {
    int hv = hex_val(c);
    return hv >= 0 && hv < base;
}

static AjsToken lex_scan_number(AjsLexer *lex, uint32_t start_line, uint32_t start_col,
                                size_t start_off, bool had_nl) {
    char clean[512];
    size_t clen = 0;
    size_t clean_cap = sizeof(clean);
    char *heap_cbuf = NULL;
    char *cbuf = clean;
    bool legacy_octal = false;
    bool has_dot = false, has_exp = false;
    int base = 10;

    #define PUSH_CLEAN(ch) do { \
        if (clen + 2 >= clean_cap) { \
            size_t nc = clean_cap * 2u; \
            char *nb = (char *)malloc(nc); \
            ajs_memcpy(nb, cbuf, clen); \
            free(heap_cbuf); \
            heap_cbuf = nb; \
            cbuf = nb; clean_cap = nc; \
        } \
        cbuf[clen++] = (char)(ch); \
    } while (0)

    if (lex->src[lex->pos] == '0' && lex->pos + 1 < lex->len) {
        char nxt = lex->src[lex->pos + 1];
        if (nxt == 'x' || nxt == 'X' || nxt == 'b' || nxt == 'B' || nxt == 'o' || nxt == 'O') {
            base = (nxt == 'x' || nxt == 'X') ? 16 : ((nxt == 'b' || nxt == 'B') ? 2 : 8);
            PUSH_CLEAN('0');
            PUSH_CLEAN(nxt);
            lex->pos += 2;
            lex->col += 2;
            if (lex->pos >= lex->len || !is_radix_digit((uint8_t)lex->src[lex->pos], base)) {
                free(heap_cbuf);
                return lex_error(lex, start_line, start_col, "Invalid numeric literal");
            }
            bool prev_sep = false;
            while (lex->pos < lex->len) {
                uint8_t c = (uint8_t)lex->src[lex->pos];
                if (c == '_') {
                    if (prev_sep) { free(heap_cbuf); return lex_error(lex, lex->line, lex->col, "Numeric separator cannot be adjacent"); }
                    prev_sep = true;
                    lex->pos++; lex->col++;
                    continue;
                }
                if (!is_radix_digit(c, base)) break;
                prev_sep = false;
                PUSH_CLEAN(c);
                lex->pos++; lex->col++;
            }
            if (prev_sep) { free(heap_cbuf); return lex_error(lex, lex->line, lex->col, "Trailing numeric separator"); }
        } else if (nxt >= '0' && nxt <= '9') {
            legacy_octal = true;
            /* Check if pure legacy octal (all digits 0..7) vs non-octal decimal (contains 8 or 9) */
            size_t scan = lex->pos + 1;
            bool all_oct = true;
            while (scan < lex->len && lex->src[scan] >= '0' && lex->src[scan] <= '9') {
                if (lex->src[scan] >= '8') all_oct = false;
                scan++;
            }
            if (lex->pos < lex->len && lex->src[scan] == '_') {
                free(heap_cbuf);
                return lex_error(lex, start_line, start_col, "Numeric separator not allowed in legacy octal");
            }
            if (all_oct) {
                base = 8;
                PUSH_CLEAN('0');
                PUSH_CLEAN('o');
                lex->pos++; lex->col++;
                while (lex->pos < lex->len && lex->src[lex->pos] >= '0' && lex->src[lex->pos] <= '7') {
                    PUSH_CLEAN(lex->src[lex->pos]);
                    lex->pos++; lex->col++;
                }
            }
        } else if (nxt == '_') {
            free(heap_cbuf);
            return lex_error(lex, start_line, start_col, "Numeric separator not allowed after leading 0");
        }
    }

    if (clen == 0) {
        /* Decimal integer / fraction / exponent */
        bool prev_sep = false;
        while (lex->pos < lex->len) {
            uint8_t c = (uint8_t)lex->src[lex->pos];
            if (c == '_') {
                if (prev_sep || clen == 0) { free(heap_cbuf); return lex_error(lex, lex->line, lex->col, "Invalid numeric separator"); }
                prev_sep = true;
                lex->pos++; lex->col++;
                continue;
            }
            if (c < '0' || c > '9') break;
            prev_sep = false;
            PUSH_CLEAN(c);
            lex->pos++; lex->col++;
        }
        if (prev_sep) { free(heap_cbuf); return lex_error(lex, lex->line, lex->col, "Invalid numeric separator"); }

        if (lex->pos < lex->len && lex->src[lex->pos] == '.') {
            has_dot = true;
            PUSH_CLEAN('.');
            lex->pos++; lex->col++;
            if (lex->pos < lex->len && lex->src[lex->pos] == '_') {
                free(heap_cbuf);
                return lex_error(lex, lex->line, lex->col, "Invalid numeric separator after decimal point");
            }
            prev_sep = false;
            while (lex->pos < lex->len) {
                uint8_t c = (uint8_t)lex->src[lex->pos];
                if (c == '_') {
                    if (prev_sep) { free(heap_cbuf); return lex_error(lex, lex->line, lex->col, "Invalid numeric separator"); }
                    prev_sep = true;
                    lex->pos++; lex->col++;
                    continue;
                }
                if (c < '0' || c > '9') break;
                prev_sep = false;
                PUSH_CLEAN(c);
                lex->pos++; lex->col++;
            }
            if (prev_sep) { free(heap_cbuf); return lex_error(lex, lex->line, lex->col, "Trailing numeric separator"); }
        }

        if (lex->pos < lex->len && (lex->src[lex->pos] == 'e' || lex->src[lex->pos] == 'E')) {
            has_exp = true;
            PUSH_CLEAN('e');
            lex->pos++; lex->col++;
            if (lex->pos < lex->len && (lex->src[lex->pos] == '+' || lex->src[lex->pos] == '-')) {
                PUSH_CLEAN(lex->src[lex->pos]);
                lex->pos++; lex->col++;
            }
            if (lex->pos >= lex->len || lex->src[lex->pos] < '0' || lex->src[lex->pos] > '9') {
                free(heap_cbuf);
                return lex_error(lex, start_line, start_col, "Invalid exponent in numeric literal");
            }
            prev_sep = false;
            while (lex->pos < lex->len) {
                uint8_t c = (uint8_t)lex->src[lex->pos];
                if (c == '_') {
                    if (prev_sep) { free(heap_cbuf); return lex_error(lex, lex->line, lex->col, "Invalid numeric separator"); }
                    prev_sep = true;
                    lex->pos++; lex->col++;
                    continue;
                }
                if (c < '0' || c > '9') break;
                prev_sep = false;
                PUSH_CLEAN(c);
                lex->pos++; lex->col++;
            }
            if (prev_sep) { free(heap_cbuf); return lex_error(lex, lex->line, lex->col, "Trailing numeric separator"); }
        }
    }
    cbuf[clen] = '\0';
    #undef PUSH_CLEAN

    bool is_bigint = false;
    if (lex->pos < lex->len && lex->src[lex->pos] == 'n') {
        if (has_dot || has_exp || legacy_octal) {
            free(heap_cbuf);
            return lex_error(lex, start_line, start_col, "Invalid BigInt literal");
        }
        is_bigint = true;
        lex->pos++; lex->col++;
    }

    /* ECMA-262: NumericLiteral cannot be immediately followed by IdentifierStart or DecimalDigit */
    if (lex->pos < lex->len) {
        size_t adv = 0;
        uint32_t next_cp = lex_peek_cp(lex, &adv);
        if (ajs_unicode_is_id_start(next_cp) || (next_cp >= '0' && next_cp <= '9') || next_cp == '\\') {
            free(heap_cbuf);
            return lex_error(lex, lex->line, lex->col, "Identifier starts immediately after numeric literal");
        }
    }

    if (legacy_octal && lex->strict) {
        free(heap_cbuf);
        return lex_error(lex, start_line, start_col, "Octal literals are not allowed in strict mode");
    }

    AjsToken t;
    memset(&t, 0, sizeof(t));
    t.line = start_line;
    t.col = start_col;
    t.start_off = start_off;
    t.end_off = lex->pos;
    t.had_newline = had_nl;
    t.has_legacy_octal = legacy_octal;

    if (is_bigint) {
        t.kind = TOK_BIGINT;
        t.utf8_text = ajs_arena_strdup(lex->arena, cbuf, clen);
        t.utf8_len = clen;
    } else {
        t.kind = TOK_NUMBER;
        if (base != 10) {
            double acc = 0.0;
            for (size_t i = 2; i < clen; i++) {
                acc = ajs_f64_round(acc * (double)base + (double)hex_val((uint8_t)cbuf[i]));
            }
            t.num_val = acc;
        } else {
            t.num_val = ajs_f64_round(strtod(cbuf, NULL));
        }
    }
    free(heap_cbuf);
    return t;
}

static AjsToken lex_scan_string(AjsLexer *lex, char quote, uint32_t start_line, uint32_t start_col,
                                size_t start_off, bool had_nl) {
    lex->pos++; lex->col++;
    uint16_t stack_buf[256];
    size_t cap = 256, ulen = 0;
    uint16_t *ubuf = stack_buf;
    bool legacy_octal = false;
    bool has_escape = false;

    #define PUSH_U16(u) do { \
        if (ulen + 2 >= cap) { \
            size_t nc = cap * 2u; \
            uint16_t *nb = (uint16_t *)malloc(nc * sizeof(uint16_t)); \
            ajs_memcpy(nb, ubuf, ulen * sizeof(uint16_t)); \
            if (ubuf != stack_buf) free(ubuf); \
            ubuf = nb; cap = nc; \
        } \
        ubuf[ulen++] = (uint16_t)(u); \
    } while (0)

    while (lex->pos < lex->len) {
        size_t adv = 0;
        uint32_t cp = lex_peek_cp(lex, &adv);
        if (cp == (uint32_t)(uint8_t)quote) {
            lex->pos++; lex->col++;
            uint16_t *au = (uint16_t *)ajs_arena_alloc(lex->arena, (ulen + 1u) * sizeof(uint16_t));
            ajs_memcpy(au, ubuf, ulen * sizeof(uint16_t));
            au[ulen] = 0;
            if (ubuf != stack_buf) free(ubuf);
            if (legacy_octal && lex->strict) {
                return lex_error(lex, start_line, start_col, "Octal escape sequences are not allowed in strict mode");
            }
            AjsToken t;
            memset(&t, 0, sizeof(t));
            t.kind = TOK_STRING;
            t.line = start_line;
            t.col = start_col;
            t.start_off = start_off;
            t.end_off = lex->pos;
            t.had_newline = had_nl;
            t.has_escape = has_escape;
            t.has_legacy_octal = legacy_octal;
            t.u16_data = au;
            t.u16_len = (uint32_t)ulen;
            return t;
        }
        if (cp == '\n' || cp == '\r') {
            /* Unescaped LF/CR is illegal in string literal (U+2028/U+2029 are legal since ES2019!) */
            if (ubuf != stack_buf) free(ubuf);
            return lex_error(lex, lex->line, lex->col, "Unterminated string literal");
        }
        if (cp == '\\') {
            has_escape = true;
            lex->pos++; lex->col++;
            if (lex->pos >= lex->len) break;
            size_t eadv = 0;
            uint32_t ec = lex_peek_cp(lex, &eadv);
            if (ajs_unicode_is_line_terminator(ec)) {
                lex_advance_cp(lex, ec, eadv);
                continue;
            }
            lex_advance_cp(lex, ec, eadv);
            switch (ec) {
                case 'n': PUSH_U16('\n'); break;
                case 'r': PUSH_U16('\r'); break;
                case 't': PUSH_U16('\t'); break;
                case 'b': PUSH_U16('\b'); break;
                case 'f': PUSH_U16('\f'); break;
                case 'v': PUSH_U16('\v'); break;
                case '\\': PUSH_U16('\\'); break;
                case '\'': PUSH_U16('\''); break;
                case '"': PUSH_U16('"'); break;
                case 'x': {
                    if (lex->pos + 2 > lex->len) { if (ubuf != stack_buf) free(ubuf); return lex_error(lex, lex->line, lex->col, "Invalid hex escape"); }
                    int h0 = hex_val((uint8_t)lex->src[lex->pos]);
                    int h1 = hex_val((uint8_t)lex->src[lex->pos + 1]);
                    if (h0 < 0 || h1 < 0) { if (ubuf != stack_buf) free(ubuf); return lex_error(lex, lex->line, lex->col, "Invalid hex escape"); }
                    lex->pos += 2; lex->col += 2;
                    PUSH_U16((h0 << 4) | h1);
                    break;
                }
                case 'u': {
                    uint32_t ucp = 0;
                    if (!lex_read_unicode_escape(lex, &ucp)) {
                        if (ubuf != stack_buf) free(ubuf);
                        return lex_error(lex, lex->line, lex->col, "Invalid Unicode escape");
                    }
                    uint16_t pair[2];
                    size_t n = ajs_code_point_to_utf16(ucp, pair);
                    PUSH_U16(pair[0]);
                    if (n == 2) PUSH_U16(pair[1]);
                    break;
                }
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7': {
                    if (ec == '0' && (lex->pos >= lex->len || lex->src[lex->pos] < '0' || lex->src[lex->pos] > '9')) {
                        PUSH_U16(0);
                    } else {
                        legacy_octal = true;
                        uint32_t oct = ec - '0';
                        int max_extra = (ec <= '3') ? 2 : 1;
                        while (max_extra-- > 0 && lex->pos < lex->len &&
                               lex->src[lex->pos] >= '0' && lex->src[lex->pos] <= '7') {
                            oct = (oct << 3) | (uint32_t)(lex->src[lex->pos] - '0');
                            lex->pos++; lex->col++;
                        }
                        PUSH_U16(oct);
                    }
                    break;
                }
                case '8': case '9':
                    legacy_octal = true;
                    PUSH_U16(ec);
                    break;
                default: {
                    uint16_t pair[2];
                    size_t n = ajs_code_point_to_utf16(ec, pair);
                    PUSH_U16(pair[0]);
                    if (n == 2) PUSH_U16(pair[1]);
                    break;
                }
            }
        } else {
            lex_advance_cp(lex, cp, adv);
            uint16_t pair[2];
            size_t n = ajs_code_point_to_utf16(cp, pair);
            PUSH_U16(pair[0]);
            if (n == 2) PUSH_U16(pair[1]);
        }
    }
    #undef PUSH_U16
    if (ubuf != stack_buf) free(ubuf);
    return lex_error(lex, start_line, start_col, "Unterminated string literal");
}

static AjsToken lex_scan_template(AjsLexer *lex, bool is_head, uint32_t start_line, uint32_t start_col,
                                  size_t start_off, bool had_nl) {
    /* Caller already positioned right after opening `` ` `` or `}` */
    uint16_t c_stack[256], r_stack[256];
    size_t c_cap = 256, c_len = 0, r_cap = 256, r_len = 0;
    uint16_t *cbuf = c_stack, *rbuf = r_stack;
    bool bad_escape = false;

    #define PUSH_C(u) do { \
        if (c_len + 2 >= c_cap) { \
            size_t nc = c_cap * 2u; uint16_t *nb = (uint16_t *)malloc(nc * sizeof(uint16_t)); \
            ajs_memcpy(nb, cbuf, c_len * sizeof(uint16_t)); if (cbuf != c_stack) free(cbuf); cbuf = nb; c_cap = nc; \
        } cbuf[c_len++] = (uint16_t)(u); \
    } while (0)
    #define PUSH_R(u) do { \
        if (r_len + 2 >= r_cap) { \
            size_t nr = r_cap * 2u; uint16_t *nb = (uint16_t *)malloc(nr * sizeof(uint16_t)); \
            ajs_memcpy(nb, rbuf, r_len * sizeof(uint16_t)); if (rbuf != r_stack) free(rbuf); rbuf = nb; r_cap = nr; \
        } rbuf[r_len++] = (uint16_t)(u); \
    } while (0)

    while (lex->pos < lex->len) {
        size_t adv = 0;
        uint32_t cp = lex_peek_cp(lex, &adv);
        if (cp == '`') {
            lex->pos++; lex->col++;
            AjsToken t;
            memset(&t, 0, sizeof(t));
            t.kind = is_head ? TOK_TEMPLATE_NO_SUB : TOK_TEMPLATE_TAIL;
            t.line = start_line; t.col = start_col;
            t.start_off = start_off; t.end_off = lex->pos;
            t.had_newline = had_nl;
            t.invalid_tpl_escape = bad_escape;
            uint16_t *ar = (uint16_t *)ajs_arena_alloc(lex->arena, (r_len + 1u) * sizeof(uint16_t));
            ajs_memcpy(ar, rbuf, r_len * sizeof(uint16_t)); ar[r_len] = 0;
            t.raw_u16 = ar; t.raw_u16_len = (uint32_t)r_len;
            if (!bad_escape) {
                uint16_t *ac = (uint16_t *)ajs_arena_alloc(lex->arena, (c_len + 1u) * sizeof(uint16_t));
                ajs_memcpy(ac, cbuf, c_len * sizeof(uint16_t)); ac[c_len] = 0;
                t.u16_data = ac; t.u16_len = (uint32_t)c_len;
            }
            if (cbuf != c_stack) free(cbuf);
            if (rbuf != r_stack) free(rbuf);
            return t;
        }
        if (cp == '$' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '{') {
            lex->pos += 2; lex->col += 2;
            AjsToken t;
            memset(&t, 0, sizeof(t));
            t.kind = is_head ? TOK_TEMPLATE_HEAD : TOK_TEMPLATE_MIDDLE;
            t.line = start_line; t.col = start_col;
            t.start_off = start_off; t.end_off = lex->pos;
            t.had_newline = had_nl;
            t.invalid_tpl_escape = bad_escape;
            uint16_t *ar = (uint16_t *)ajs_arena_alloc(lex->arena, (r_len + 1u) * sizeof(uint16_t));
            ajs_memcpy(ar, rbuf, r_len * sizeof(uint16_t)); ar[r_len] = 0;
            t.raw_u16 = ar; t.raw_u16_len = (uint32_t)r_len;
            if (!bad_escape) {
                uint16_t *ac = (uint16_t *)ajs_arena_alloc(lex->arena, (c_len + 1u) * sizeof(uint16_t));
                ajs_memcpy(ac, cbuf, c_len * sizeof(uint16_t)); ac[c_len] = 0;
                t.u16_data = ac; t.u16_len = (uint32_t)c_len;
            }
            if (cbuf != c_stack) free(cbuf);
            if (rbuf != r_stack) free(rbuf);
            return t;
        }
        if (cp == '\r') {
            /* Normalize CR and CRLF to LF in both TRV and TV per ECMA-262 */
            lex_advance_cp(lex, cp, adv);
            PUSH_R('\n');
            PUSH_C('\n');
            continue;
        }
        if (cp == '\\') {
            size_t esc_start = lex->pos;
            lex->pos++; lex->col++;
            if (lex->pos >= lex->len) break;
            size_t eadv = 0;
            uint32_t ec = lex_peek_cp(lex, &eadv);
            if (ajs_unicode_is_line_terminator(ec)) {
                PUSH_R('\\');
                if (ec == '\r') {
                    lex_advance_cp(lex, ec, eadv);
                    PUSH_R('\n');
                } else {
                    lex_advance_cp(lex, ec, eadv);
                    PUSH_R(ec);
                }
                continue;
            }
            lex_advance_cp(lex, ec, eadv);
            switch (ec) {
                case 'n': PUSH_C('\n'); break;
                case 'r': PUSH_C('\r'); break;
                case 't': PUSH_C('\t'); break;
                case 'b': PUSH_C('\b'); break;
                case 'f': PUSH_C('\f'); break;
                case 'v': PUSH_C('\v'); break;
                case '\\': PUSH_C('\\'); break;
                case '`': PUSH_C('`'); break;
                case '$': PUSH_C('$'); break;
                case 'x': {
                    if (lex->pos + 2 <= lex->len &&
                        hex_val((uint8_t)lex->src[lex->pos]) >= 0 &&
                        hex_val((uint8_t)lex->src[lex->pos + 1]) >= 0) {
                        int v = (hex_val((uint8_t)lex->src[lex->pos]) << 4) | hex_val((uint8_t)lex->src[lex->pos + 1]);
                        lex->pos += 2; lex->col += 2;
                        PUSH_C(v);
                    } else {
                        bad_escape = true;
                    }
                    break;
                }
                case 'u': {
                    uint32_t ucp = 0;
                    if (lex_read_unicode_escape(lex, &ucp)) {
                        uint16_t pair[2];
                        size_t n = ajs_code_point_to_utf16(ucp, pair);
                        PUSH_C(pair[0]);
                        if (n == 2) PUSH_C(pair[1]);
                    } else {
                        bad_escape = true;
                    }
                    break;
                }
                case '0':
                    if (lex->pos < lex->len && lex->src[lex->pos] >= '0' && lex->src[lex->pos] <= '9') {
                        bad_escape = true;
                    } else {
                        PUSH_C(0);
                    }
                    break;
                case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
                    bad_escape = true;
                    break;
                default: {
                    uint16_t pair[2];
                    size_t n = ajs_code_point_to_utf16(ec, pair);
                    PUSH_C(pair[0]);
                    if (n == 2) PUSH_C(pair[1]);
                    break;
                }
            }
            /* Append exact raw characters from esc_start .. lex->pos */
            size_t rpos = esc_start;
            while (rpos < lex->pos) {
                size_t radv = 1;
                uint32_t rcp = ajs_utf8_decode_one(lex->src + rpos, lex->pos - rpos, &radv);
                uint16_t pair[2];
                size_t n = ajs_code_point_to_utf16(rcp, pair);
                PUSH_R(pair[0]);
                if (n == 2) PUSH_R(pair[1]);
                rpos += radv;
            }
        } else {
            lex_advance_cp(lex, cp, adv);
            uint16_t pair[2];
            size_t n = ajs_code_point_to_utf16(cp, pair);
            PUSH_R(pair[0]); PUSH_C(pair[0]);
            if (n == 2) { PUSH_R(pair[1]); PUSH_C(pair[1]); }
        }
    }
    #undef PUSH_C
    #undef PUSH_R
    if (cbuf != c_stack) free(cbuf);
    if (rbuf != r_stack) free(rbuf);
    return lex_error(lex, start_line, start_col, "Unterminated template literal");
}

static AjsToken lex_scan_raw(AjsLexer *lex) {
    bool had_nl = false;

    for (;;) {
        if (lex->pos >= lex->len) {
            AjsToken eof;
            memset(&eof, 0, sizeof(eof));
            eof.kind = TOK_EOF;
            eof.line = lex->line;
            eof.col = lex->col;
            eof.start_off = lex->pos;
            eof.end_off = lex->pos;
            eof.had_newline = had_nl;
            return eof;
        }
        size_t adv = 0;
        uint32_t cp = lex_peek_cp(lex, &adv);
        if (ajs_unicode_is_line_terminator(cp)) {
            had_nl = true;
            lex_advance_cp(lex, cp, adv);
            continue;
        }
        if (ajs_unicode_is_whitespace(cp)) {
            lex_advance_cp(lex, cp, adv);
            continue;
        }
        /* Comments */
        if (cp == '/' && lex->pos + 1 < lex->len) {
            char nxt = lex->src[lex->pos + 1];
            if (nxt == '/') {
                lex->pos += 2; lex->col += 2;
                while (lex->pos < lex->len) {
                    size_t cadv = 0;
                    uint32_t ccp = lex_peek_cp(lex, &cadv);
                    if (ajs_unicode_is_line_terminator(ccp)) break;
                    lex_advance_cp(lex, ccp, cadv);
                }
                continue;
            }
            if (nxt == '*') {
                uint32_t cl = lex->line, cc = lex->col;
                lex->pos += 2; lex->col += 2;
                bool closed = false;
                while (lex->pos < lex->len) {
                    if (lex->src[lex->pos] == '*' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '/') {
                        lex->pos += 2; lex->col += 2;
                        closed = true;
                        break;
                    }
                    size_t cadv = 0;
                    uint32_t ccp = lex_peek_cp(lex, &cadv);
                    if (ajs_unicode_is_line_terminator(ccp)) had_nl = true;
                    lex_advance_cp(lex, ccp, cadv);
                }
                if (!closed) return lex_error(lex, cl, cc, "Unterminated multi-line comment");
                continue;
            }
        }
        /* Annex B HTML comments in classic Script mode */
        if (!lex->is_module) {
            if (cp == '<' && lex->pos + 3 < lex->len &&
                lex->src[lex->pos + 1] == '!' && lex->src[lex->pos + 2] == '-' && lex->src[lex->pos + 3] == '-') {
                lex->pos += 4; lex->col += 4;
                while (lex->pos < lex->len) {
                    size_t cadv = 0;
                    uint32_t ccp = lex_peek_cp(lex, &cadv);
                    if (ajs_unicode_is_line_terminator(ccp)) break;
                    lex_advance_cp(lex, ccp, cadv);
                }
                continue;
            }
            if ((lex->at_line_start || had_nl) && cp == '-' && lex->pos + 2 < lex->len &&
                lex->src[lex->pos + 1] == '-' && lex->src[lex->pos + 2] == '>') {
                lex->pos += 3; lex->col += 3;
                while (lex->pos < lex->len) {
                    size_t cadv = 0;
                    uint32_t ccp = lex_peek_cp(lex, &cadv);
                    if (ajs_unicode_is_line_terminator(ccp)) break;
                    lex_advance_cp(lex, ccp, cadv);
                }
                continue;
            }
        }
        break;
    }

    lex->at_line_start = false;
    uint32_t start_line = lex->line;
    uint32_t start_col = lex->col;
    size_t start_off = lex->pos;

    size_t adv = 0;
    uint32_t cp = lex_peek_cp(lex, &adv);

    if (cp == '#') {
        lex->pos++; lex->col++;
        return lex_scan_ident(lex, true, start_line, start_col, start_off, had_nl);
    }
    if (ajs_unicode_is_id_start(cp) || cp == '\\') {
        return lex_scan_ident(lex, false, start_line, start_col, start_off, had_nl);
    }
    if ((cp >= '0' && cp <= '9') ||
        (cp == '.' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] >= '0' && lex->src[lex->pos + 1] <= '9')) {
        return lex_scan_number(lex, start_line, start_col, start_off, had_nl);
    }
    if (cp == '\'' || cp == '"') {
        return lex_scan_string(lex, (char)cp, start_line, start_col, start_off, had_nl);
    }
    if (cp == '`') {
        lex->pos++; lex->col++;
        return lex_scan_template(lex, true, start_line, start_col, start_off, had_nl);
    }

    /* Punctuators */
    AjsToken t;
    memset(&t, 0, sizeof(t));
    t.line = start_line;
    t.col = start_col;
    t.start_off = start_off;
    t.had_newline = had_nl;

    char c0 = lex->src[lex->pos];
    char c1 = (lex->pos + 1 < lex->len) ? lex->src[lex->pos + 1] : 0;
    char c2 = (lex->pos + 2 < lex->len) ? lex->src[lex->pos + 2] : 0;
    char c3 = (lex->pos + 3 < lex->len) ? lex->src[lex->pos + 3] : 0;

    #define EMIT(k, n) do { t.kind = (k); lex->pos += (n); lex->col += (n); t.end_off = lex->pos; return t; } while (0)

    switch (c0) {
        case '{': EMIT(TOK_LBRACE, 1);
        case '}': EMIT(TOK_RBRACE, 1);
        case '(': EMIT(TOK_LPAREN, 1);
        case ')': EMIT(TOK_RPAREN, 1);
        case '[': EMIT(TOK_LBRACKET, 1);
        case ']': EMIT(TOK_RBRACKET, 1);
        case ';': EMIT(TOK_SEMI, 1);
        case ',': EMIT(TOK_COMMA, 1);
        case '~': EMIT(TOK_BIT_NOT, 1);
        case ':': EMIT(TOK_COLON, 1);
        case '.':
            if (c1 == '.' && c2 == '.') EMIT(TOK_ELLIPSIS, 3);
            EMIT(TOK_DOT, 1);
        case '?':
            if (c1 == '?' && c2 == '=') EMIT(TOK_COALESCE_ASSIGN, 3);
            if (c1 == '?') EMIT(TOK_COALESCE, 2);
            if (c1 == '.' && !(c2 >= '0' && c2 <= '9')) EMIT(TOK_OPT_CHAIN, 2);
            EMIT(TOK_QUESTION, 1);
        case '+':
            if (c1 == '+') EMIT(TOK_INC, 2);
            if (c1 == '=') EMIT(TOK_ADD_ASSIGN, 2);
            EMIT(TOK_PLUS, 1);
        case '-':
            if (c1 == '-') EMIT(TOK_DEC, 2);
            if (c1 == '=') EMIT(TOK_SUB_ASSIGN, 2);
            EMIT(TOK_MINUS, 1);
        case '*':
            if (c1 == '*' && c2 == '=') EMIT(TOK_EXP_ASSIGN, 3);
            if (c1 == '*') EMIT(TOK_EXP, 2);
            if (c1 == '=') EMIT(TOK_MUL_ASSIGN, 2);
            EMIT(TOK_MUL, 1);
        case '/':
            if (c1 == '=') EMIT(TOK_DIV_ASSIGN, 2);
            EMIT(TOK_DIV, 1);
        case '%':
            if (c1 == '=') EMIT(TOK_MOD_ASSIGN, 2);
            EMIT(TOK_MOD, 1);
        case '&':
            if (c1 == '&' && c2 == '=') EMIT(TOK_LOG_AND_ASSIGN, 3);
            if (c1 == '&') EMIT(TOK_AND, 2);
            if (c1 == '=') EMIT(TOK_AND_ASSIGN, 2);
            EMIT(TOK_BIT_AND, 1);
        case '|':
            if (c1 == '|' && c2 == '=') EMIT(TOK_LOG_OR_ASSIGN, 3);
            if (c1 == '|') EMIT(TOK_OR, 2);
            if (c1 == '=') EMIT(TOK_OR_ASSIGN, 2);
            EMIT(TOK_BIT_OR, 1);
        case '^':
            if (c1 == '=') EMIT(TOK_XOR_ASSIGN, 2);
            EMIT(TOK_BIT_XOR, 1);
        case '!':
            if (c1 == '=' && c2 == '=') EMIT(TOK_STRICT_NEQ, 3);
            if (c1 == '=') EMIT(TOK_NEQ, 2);
            EMIT(TOK_BANG, 1);
        case '=':
            if (c1 == '=' && c2 == '=') EMIT(TOK_STRICT_EQ, 3);
            if (c1 == '=') EMIT(TOK_EQ, 2);
            if (c1 == '>') EMIT(TOK_ARROW, 2);
            EMIT(TOK_ASSIGN, 1);
        case '<':
            if (c1 == '<' && c2 == '=') EMIT(TOK_SHL_ASSIGN, 3);
            if (c1 == '<') EMIT(TOK_SHL, 2);
            if (c1 == '=') EMIT(TOK_LTE, 2);
            EMIT(TOK_LT, 1);
        case '>':
            if (c1 == '>' && c2 == '>' && c3 == '=') EMIT(TOK_USHR_ASSIGN, 4);
            if (c1 == '>' && c2 == '>') EMIT(TOK_USHR, 3);
            if (c1 == '>' && c2 == '=') EMIT(TOK_SHR_ASSIGN, 3);
            if (c1 == '>') EMIT(TOK_SHR, 2);
            if (c1 == '=') EMIT(TOK_GTE, 2);
            EMIT(TOK_GT, 1);
        default:
            break;
    }
    #undef EMIT
    return lex_error(lex, start_line, start_col, "Invalid or unexpected token");
}

void ajs_lexer_init(AjsLexer *lex, AjsAstArena *arena, const char *src, size_t len, bool is_module, bool strict) {
    memset(lex, 0, sizeof(*lex));
    lex->arena = arena;
    lex->src = src ? src : "";
    lex->len = len;
    lex->pos = 0;
    lex->line = 1;
    lex->col = 1;
    lex->is_module = is_module;
    lex->strict = strict || is_module;
    lex->at_line_start = true;

    /* Skip UTF-8 BOM at offset 0 if present */
    if (lex->len >= 3 && (uint8_t)lex->src[0] == 0xEFu &&
        (uint8_t)lex->src[1] == 0xBBu && (uint8_t)lex->src[2] == 0xBFu) {
        lex->pos = 3;
    }
    /* Skip ES2023+ Hashbang `#!...` at start of file */
    if (lex->pos + 1 < lex->len && lex->src[lex->pos] == '#' && lex->src[lex->pos + 1] == '!') {
        lex->pos += 2; lex->col += 2;
        while (lex->pos < lex->len) {
            size_t adv = 0;
            uint32_t cp = lex_peek_cp(lex, &adv);
            if (ajs_unicode_is_line_terminator(cp)) break;
            lex_advance_cp(lex, cp, adv);
        }
    }
    lex->cur = lex_scan_raw(lex);
}

AjsToken ajs_lexer_next(AjsLexer *lex) {
    if (lex->has_peek) {
        lex->cur = lex->peek_tok;
        lex->has_peek = false;
    } else {
        lex->cur = lex_scan_raw(lex);
    }
    return lex->cur;
}

AjsToken ajs_lexer_peek(AjsLexer *lex) {
    if (!lex->has_peek) {
        lex->peek_tok = lex_scan_raw(lex);
        lex->has_peek = true;
    }
    return lex->peek_tok;
}

bool ajs_lexer_rescan_regexp(AjsLexer *lex) {
    if (lex->cur.kind != TOK_DIV && lex->cur.kind != TOK_DIV_ASSIGN) return false;
    /* Reset scanner right after opening `/` */
    lex->has_peek = false;
    lex->pos = lex->cur.start_off + 1u;
    lex->line = lex->cur.line;
    lex->col = lex->cur.col + 1u;

    uint16_t p_stack[256];
    size_t p_cap = 256, p_len = 0;
    uint16_t *pbuf = p_stack;
    int class_depth = 0;

    #define PUSH_P(u) do { \
        if (p_len + 2 >= p_cap) { \
            size_t nc = p_cap * 2u; uint16_t *nb = (uint16_t *)malloc(nc * sizeof(uint16_t)); \
            ajs_memcpy(nb, pbuf, p_len * sizeof(uint16_t)); if (pbuf != p_stack) free(pbuf); pbuf = nb; p_cap = nc; \
        } pbuf[p_len++] = (uint16_t)(u); \
    } while (0)

    bool closed = false;
    while (lex->pos < lex->len) {
        size_t adv = 0;
        uint32_t cp = lex_peek_cp(lex, &adv);
        if (ajs_unicode_is_line_terminator(cp)) {
            if (pbuf != p_stack) free(pbuf);
            lex->cur = lex_error(lex, lex->cur.line, lex->cur.col, "Unterminated regular expression literal");
            return false;
        }
        if (cp == '\\') {
            lex_advance_cp(lex, cp, adv);
            PUSH_P('\\');
            if (lex->pos >= lex->len) break;
            size_t eadv = 0;
            uint32_t ec = lex_peek_cp(lex, &eadv);
            if (ajs_unicode_is_line_terminator(ec)) {
                if (pbuf != p_stack) free(pbuf);
                lex->cur = lex_error(lex, lex->cur.line, lex->cur.col, "Unterminated regular expression literal");
                return false;
            }
            lex_advance_cp(lex, ec, eadv);
            uint16_t pair[2];
            size_t n = ajs_code_point_to_utf16(ec, pair);
            PUSH_P(pair[0]);
            if (n == 2) PUSH_P(pair[1]);
            continue;
        }
        if (cp == '[') {
            class_depth = 1;
            lex_advance_cp(lex, cp, adv);
            PUSH_P('[');
            continue;
        }
        if (cp == ']' && class_depth > 0) {
            class_depth = 0;
            lex_advance_cp(lex, cp, adv);
            PUSH_P(']');
            continue;
        }
        if (cp == '/' && class_depth == 0) {
            lex->pos++; lex->col++;
            closed = true;
            break;
        }
        lex_advance_cp(lex, cp, adv);
        uint16_t pair[2];
        size_t n = ajs_code_point_to_utf16(cp, pair);
        PUSH_P(pair[0]);
        if (n == 2) PUSH_P(pair[1]);
    }
    #undef PUSH_P

    if (!closed) {
        if (pbuf != p_stack) free(pbuf);
        lex->cur = lex_error(lex, lex->cur.line, lex->cur.col, "Unterminated regular expression literal");
        return false;
    }

    uint16_t f_buf[32];
    size_t f_len = 0;
    while (lex->pos < lex->len) {
        size_t adv = 0;
        uint32_t cp = lex_peek_cp(lex, &adv);
        if (cp == '\\') {
            if (pbuf != p_stack) free(pbuf);
            lex->cur = lex_error(lex, lex->cur.line, lex->cur.col, "Invalid escaped regular expression flag");
            return false;
        }
        if (!ajs_unicode_is_id_continue(cp)) break;
        lex_advance_cp(lex, cp, adv);
        if (f_len >= 30) {
            if (pbuf != p_stack) free(pbuf);
            lex->cur = lex_error(lex, lex->cur.line, lex->cur.col, "Invalid regular expression flags");
            return false;
        }
        f_buf[f_len++] = (uint16_t)cp;
    }

    uint16_t *ap = (uint16_t *)ajs_arena_alloc(lex->arena, (p_len + 1u) * sizeof(uint16_t));
    ajs_memcpy(ap, pbuf, p_len * sizeof(uint16_t));
    ap[p_len] = 0;
    if (pbuf != p_stack) free(pbuf);

    uint16_t *af = (uint16_t *)ajs_arena_alloc(lex->arena, (f_len + 1u) * sizeof(uint16_t));
    ajs_memcpy(af, f_buf, f_len * sizeof(uint16_t));
    af[f_len] = 0;

    lex->cur.kind = TOK_REGEXP;
    lex->cur.end_off = lex->pos;
    lex->cur.u16_data = ap;
    lex->cur.u16_len = (uint32_t)p_len;
    lex->cur.rx_flags = af;
    lex->cur.rx_flags_len = (uint32_t)f_len;
    return true;
}

bool ajs_lexer_rescan_template(AjsLexer *lex) {
    if (lex->cur.kind != TOK_RBRACE) return false;
    lex->has_peek = false;
    lex->pos = lex->cur.start_off + 1u;
    lex->line = lex->cur.line;
    lex->col = lex->cur.col + 1u;
    lex->cur = lex_scan_template(lex, false, lex->cur.line, lex->cur.col, lex->cur.start_off, lex->cur.had_newline);
    return lex->cur.kind != TOK_ERROR;
}
