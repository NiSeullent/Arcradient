/*
 * Arcradient (MSHTML-Next)
 * base/arcr_utf8.c - UTF-8/UTF-16LE & First-Class Korean/CJK Line-Break Rules
 */
#include "arcr_base.h"

size_t arcr_utf8_decode_one(const char *s, size_t len, uint32_t *out_codepoint) {
    if (!s || len == 0u) {
        if (out_codepoint) *out_codepoint = 0u;
        return 0u;
    }
    uint8_t b0 = (uint8_t)s[0];
    if (b0 < 0x80u) {
        if (out_codepoint) *out_codepoint = b0;
        return 1u;
    }
    if ((b0 & 0xE0u) == 0xC0u) {
        if (len < 2u || ((uint8_t)s[1] & 0xC0u) != 0x80u) goto bad;
        uint32_t cp = ((uint32_t)(b0 & 0x1Fu) << 6) | ((uint32_t)s[1] & 0x3Fu);
        if (cp < 0x80u) goto bad;
        if (out_codepoint) *out_codepoint = cp;
        return 2u;
    }
    if ((b0 & 0xF0u) == 0xE0u) {
        if (len < 3u ||
            ((uint8_t)s[1] & 0xC0u) != 0x80u ||
            ((uint8_t)s[2] & 0xC0u) != 0x80u) goto bad;
        uint32_t cp = ((uint32_t)(b0 & 0x0Fu) << 12) |
                      (((uint32_t)s[1] & 0x3Fu) << 6) |
                      ((uint32_t)s[2] & 0x3Fu);
        if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) goto bad;
        if (out_codepoint) *out_codepoint = cp;
        return 3u;
    }
    if ((b0 & 0xF8u) == 0xF0u) {
        if (len < 4u ||
            ((uint8_t)s[1] & 0xC0u) != 0x80u ||
            ((uint8_t)s[2] & 0xC0u) != 0x80u ||
            ((uint8_t)s[3] & 0xC0u) != 0x80u) goto bad;
        uint32_t cp = ((uint32_t)(b0 & 0x07u) << 18) |
                      (((uint32_t)s[1] & 0x3Fu) << 12) |
                      (((uint32_t)s[2] & 0x3Fu) << 6) |
                      ((uint32_t)s[3] & 0x3Fu);
        if (cp < 0x10000u || cp > 0x10FFFFu) goto bad;
        if (out_codepoint) *out_codepoint = cp;
        return 4u;
    }

bad:
    if (out_codepoint) *out_codepoint = ARCR_UNICODE_REPLACEMENT;
    return 1u;
}

size_t arcr_utf8_encode_one(uint32_t cp, char out_buf[4]) {
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        cp = ARCR_UNICODE_REPLACEMENT;
    }
    if (cp < 0x80u) {
        out_buf[0] = (char)cp;
        return 1u;
    }
    if (cp < 0x800u) {
        out_buf[0] = (char)(0xC0u | (cp >> 6));
        out_buf[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2u;
    }
    if (cp < 0x10000u) {
        out_buf[0] = (char)(0xE0u | (cp >> 12));
        out_buf[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out_buf[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3u;
    }
    out_buf[0] = (char)(0xF0u | (cp >> 18));
    out_buf[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out_buf[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out_buf[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4u;
}

size_t arcr_utf8_to_utf16le(const char *utf8, size_t utf8_len,
                            uint16_t *out_utf16, size_t out_cap_units) {
    size_t in_pos  = 0u;
    size_t out_pos = 0u;
    while (in_pos < utf8_len) {
        uint32_t cp   = 0u;
        size_t   step = arcr_utf8_decode_one(utf8 + in_pos, utf8_len - in_pos, &cp);
        if (step == 0u) break;
        in_pos += step;

        if (cp <= 0xFFFFu) {
            if (out_utf16 && out_pos + 1u < out_cap_units) {
                out_utf16[out_pos] = (uint16_t)cp;
            }
            out_pos += 1u;
        } else {
            uint32_t u = cp - 0x10000u;
            if (out_utf16 && out_pos + 2u < out_cap_units) {
                out_utf16[out_pos]      = (uint16_t)(0xD800u + (u >> 10));
                out_utf16[out_pos + 1u] = (uint16_t)(0xDC00u + (u & 0x3FFu));
            }
            out_pos += 2u;
        }
    }
    if (out_utf16 && out_cap_units > 0u) {
        size_t term = (out_pos < out_cap_units) ? out_pos : (out_cap_units - 1u);
        out_utf16[term] = 0u;
    }
    return out_pos;
}

bool arcr_cp_is_hangul(uint32_t cp) {
    return (cp >= 0xAC00u && cp <= 0xD7A3u) || /* Hangul Syllables (가..힣) */
           (cp >= 0x1100u && cp <= 0x11FFu) || /* Hangul Jamo */
           (cp >= 0x3130u && cp <= 0x318Fu) || /* Hangul Compatibility Jamo */
           (cp >= 0xA960u && cp <= 0xA97Fu) || /* Hangul Jamo Extended-A */
           (cp >= 0xD7B0u && cp <= 0xD7FFu);   /* Hangul Jamo Extended-B */
}

bool arcr_cp_is_cjk_ideograph(uint32_t cp) {
    return (cp >= 0x4E00u && cp <= 0x9FFFu) || /* CJK Unified Ideographs */
           (cp >= 0x3400u && cp <= 0x4DBFu) || /* CJK Extension A */
           (cp >= 0x3040u && cp <= 0x30FFu) || /* Hiragana / Katakana */
           (cp >= 0xF900u && cp <= 0xFAFFu);   /* CJK Compatibility Ideographs */
}

/* 행두 금칙 문자 (Characters prohibited from starting a new line) */
bool arcr_cp_is_cjk_no_line_start(uint32_t cp) {
    switch (cp) {
        case '.': case ',': case ':': case ';': case '!': case '?':
        case ')': case ']': case '}': case '%':
        case 0x00B7u: /* · Middle dot */
        case 0x2019u: /* ’ Right single quotation mark */
        case 0x201Du: /* ” Right double quotation mark */
        case 0x2026u: /* … Horizontal ellipsis */
        case 0x3001u: /* 、 Ideographic comma */
        case 0x3002u: /* 。 Ideographic full stop */
        case 0x3009u: /* 〉 */
        case 0x300Bu: /* 》 */
        case 0x300Du: /* 」 */
        case 0x300Fu: /* 』 */
        case 0x3011u: /* 】 */
        case 0xFF01u: /* ！ Fullwidth exclamation */
        case 0xFF09u: /* ） Fullwidth right paren */
        case 0xFF0Cu: /* ， Fullwidth comma */
        case 0xFF0Eu: /* ． Fullwidth period */
        case 0xFF1Au: /* ： Fullwidth colon */
        case 0xFF1Bu: /* ； Fullwidth semicolon */
        case 0xFF1Fu: /* ？ Fullwidth question */
            return true;
        default:
            return false;
    }
}

/* 행말 금칙 문자 (Characters prohibited from ending a line alone before content) */
bool arcr_cp_is_cjk_no_line_end(uint32_t cp) {
    switch (cp) {
        case '(': case '[': case '{':
        case 0x2018u: /* ‘ Left single quote */
        case 0x201Cu: /* “ Left double quote */
        case 0x3008u: /* 〈 */
        case 0x300Au: /* 《 */
        case 0x300Cu: /* 「 */
        case 0x300Eu: /* 『 */
        case 0x3010u: /* 【 */
        case 0xFF08u: /* （ Fullwidth left paren */
            return true;
        default:
            return false;
    }
}

bool arcr_cp_is_whitespace(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x3000u;
}

bool arcr_unicode_can_break_line(uint32_t prev_cp, uint32_t next_cp, bool keep_all) {
    if (prev_cp == 0u || next_cp == 0u) return false;
    if (prev_cp == '\n') return true;

    /* Never orphan closing/sentence punctuation at the start of a line (행두 금칙) */
    if (arcr_cp_is_cjk_no_line_start(next_cp)) return false;

    /* Never leave opening bracket/quote dangling at the end of a line (행말 금칙) */
    if (arcr_cp_is_cjk_no_line_end(prev_cp)) return false;

    /* Standard break after whitespace or hyphen/slash */
    if (arcr_cp_is_whitespace(prev_cp) && !arcr_cp_is_whitespace(next_cp)) {
        return true;
    }
    if (prev_cp == '-') {
        return true;
    }

    /* Korean Hangul & CJK handling */
    bool prev_hangul = arcr_cp_is_hangul(prev_cp);
    bool next_hangul = arcr_cp_is_hangul(next_cp);
    bool prev_cjk    = arcr_cp_is_cjk_ideograph(prev_cp);
    bool next_cjk    = arcr_cp_is_cjk_ideograph(next_cp);

    if (keep_all) {
        /* word-break: keep-all keeps Korean 어절 (Hangul runs + attached particles) intact */
        if ((prev_hangul || prev_cjk) && (next_hangul || next_cjk)) {
            return false;
        }
        return false;
    }

    /* word-break: normal allows wrapping at CJK/Hangul syllable boundaries (unless 금칙) */
    if (prev_hangul || next_hangul || prev_cjk || next_cjk) {
        /* Keep Korean postpositions/particles attached if preceded by ASCII?
         * Actually UAX #14 allows ID <-> ID and H2/H3 syllable breaks, while keeping
         * ASCII words intact. Allow break if either side is CJK/Hangul and neither violates 금칙. */
        return true;
    }

    return false;
}
