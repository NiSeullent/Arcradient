/*
 * ArcradientJS 1.0 - js/vm/ajs_utf16.c
 * UTF-16 observable strings, WTF-8/UTF-8 codec, interning, Unicode properties & Number formatting
 */
#include "ajs_internal.h"

uint32_t ajs_hash_utf16(const uint16_t *units, uint32_t len) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) {
        h ^= (uint32_t)(units[i] & 0xFFu);
        h *= 16777619u;
        h ^= (uint32_t)(units[i] >> 8);
        h *= 16777619u;
    }
    return h == 0 ? 1u : h;
}

static void ajs_classify_array_idx(AjsString *s) {
    s->is_array_idx = false;
    s->array_idx = 0;
    if (s->len16 == 0 || s->len16 > 10) return;
    if (s->units[0] == '0') {
        if (s->len16 == 1) {
            s->is_array_idx = true;
            s->array_idx = 0;
        }
        return;
    }
    uint64_t val = 0;
    for (uint32_t i = 0; i < s->len16; i++) {
        uint16_t ch = s->units[i];
        if (ch < '0' || ch > '9') return;
        val = val * 10u + (uint64_t)(ch - '0');
    }
    if (val < UINT64_C(0xFFFFFFFF)) {
        s->is_array_idx = true;
        s->array_idx = (uint32_t)val;
    }
}

size_t ajs_code_point_to_utf16(uint32_t cp, uint16_t out_units[2]) {
    if (cp <= 0xFFFFu) {
        out_units[0] = (uint16_t)cp;
        return 1;
    }
    if (cp > 0x10FFFFu) {
        out_units[0] = 0xFFFDu;
        return 1;
    }
    uint32_t u = cp - 0x10000u;
    out_units[0] = (uint16_t)(0xD800u + (u >> 10));
    out_units[1] = (uint16_t)(0xDC00u + (u & 0x3FFu));
    return 2;
}

size_t ajs_code_point_to_utf8(uint32_t cp, char out_utf8[4]) {
    if (cp < 0x80u) {
        out_utf8[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out_utf8[0] = (char)(0xC0u | (cp >> 6));
        out_utf8[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp >= 0xD800u && cp <= 0xDFFFu) {
        cp = 0xFFFDu;
    }
    if (cp < 0x10000u) {
        out_utf8[0] = (char)(0xE0u | (cp >> 12));
        out_utf8[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out_utf8[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        out_utf8[0] = (char)(0xF0u | (cp >> 18));
        out_utf8[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out_utf8[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out_utf8[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    out_utf8[0] = (char)0xEFu;
    out_utf8[1] = (char)0xBFu;
    out_utf8[2] = (char)0xBDu;
    return 3;
}

uint32_t ajs_utf8_decode_one(const char *s, size_t len, size_t *out_adv) {
    if (len == 0) {
        if (out_adv) *out_adv = 0;
        return 0;
    }
    uint8_t b0 = (uint8_t)s[0];
    if (b0 < 0x80u) {
        if (out_adv) *out_adv = 1;
        return b0;
    }
    if ((b0 & 0xE0u) == 0xC0u && len >= 2) {
        uint8_t b1 = (uint8_t)s[1];
        if ((b1 & 0xC0u) == 0x80u) {
            uint32_t cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(b1 & 0x3Fu);
            if (cp >= 0x80u) {
                if (out_adv) *out_adv = 2;
                return cp;
            }
        }
    } else if ((b0 & 0xF0u) == 0xE0u && len >= 3) {
        uint8_t b1 = (uint8_t)s[1];
        uint8_t b2 = (uint8_t)s[2];
        if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
            uint32_t cp = ((uint32_t)(b0 & 0x0Fu) << 12) |
                          ((uint32_t)(b1 & 0x3Fu) << 6) |
                          (uint32_t)(b2 & 0x3Fu);
            if (cp >= 0x800u && !(cp >= 0xD800u && cp <= 0xDFFFu)) {
                if (out_adv) *out_adv = 3;
                return cp;
            }
        }
    } else if ((b0 & 0xF8u) == 0xF0u && len >= 4) {
        uint8_t b1 = (uint8_t)s[1];
        uint8_t b2 = (uint8_t)s[2];
        uint8_t b3 = (uint8_t)s[3];
        if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u) {
            uint32_t cp = ((uint32_t)(b0 & 0x07u) << 18) |
                          ((uint32_t)(b1 & 0x3Fu) << 12) |
                          ((uint32_t)(b2 & 0x3Fu) << 6) |
                          (uint32_t)(b3 & 0x3Fu);
            if (cp >= 0x10000u && cp <= 0x10FFFFu) {
                if (out_adv) *out_adv = 4;
                return cp;
            }
        }
    }
    if (out_adv) *out_adv = 1;
    return 0xFFFDu;
}

uint32_t ajs_utf16_code_point_at(const uint16_t *units, uint32_t len, uint32_t idx, uint32_t *out_adv) {
    if (idx >= len) {
        if (out_adv) *out_adv = 0;
        return 0;
    }
    uint16_t u0 = units[idx];
    if (u0 >= 0xD800u && u0 <= 0xDBFFu && idx + 1 < len) {
        uint16_t u1 = units[idx + 1];
        if (u1 >= 0xDC00u && u1 <= 0xDFFFu) {
            if (out_adv) *out_adv = 2;
            return 0x10000u + (((uint32_t)(u0 - 0xD800u) << 10) | (uint32_t)(u1 - 0xDC00u));
        }
    }
    if (out_adv) *out_adv = 1;
    return u0;
}

static AjsString *ajs_intern_lookup(AjsVm *vm, const uint16_t *units, uint32_t len16, uint32_t hash) {
    if (!vm->intern_tab || vm->intern_cap == 0) return NULL;
    uint32_t mask = vm->intern_cap - 1u;
    uint32_t idx = hash & mask;
    for (uint32_t step = 0; step < vm->intern_cap; step++) {
        AjsString *entry = vm->intern_tab[idx];
        if (!entry) return NULL;
        if (entry != (AjsString *)(uintptr_t)1 &&
            entry->hash == hash && entry->len16 == len16 &&
            (len16 == 0 || memcmp(entry->units, units, (size_t)len16 * sizeof(uint16_t)) == 0)) {
            return entry;
        }
        idx = (idx + 1u) & mask;
    }
    return NULL;
}

static void ajs_intern_insert(AjsVm *vm, AjsString *s) {
    if ((vm->intern_count + 1u) * 10u >= vm->intern_cap * 7u) {
        uint32_t new_cap = vm->intern_cap ? vm->intern_cap * 2u : 256u;
        AjsString **new_tab = (AjsString **)calloc(new_cap, sizeof(AjsString *));
        if (!new_tab) return;
        uint32_t mask = new_cap - 1u;
        uint32_t live = 0;
        for (uint32_t i = 0; i < vm->intern_cap; i++) {
            AjsString *e = vm->intern_tab[i];
            if (e && e != (AjsString *)(uintptr_t)1) {
                uint32_t pos = e->hash & mask;
                while (new_tab[pos]) pos = (pos + 1u) & mask;
                new_tab[pos] = e;
                live++;
            }
        }
        free(vm->intern_tab);
        vm->intern_tab = new_tab;
        vm->intern_cap = new_cap;
        vm->intern_count = live;
    }
    uint32_t mask = vm->intern_cap - 1u;
    uint32_t pos = s->hash & mask;
    int32_t first_tomb = -1;
    while (vm->intern_tab[pos]) {
        if (vm->intern_tab[pos] == s) return;
        if (vm->intern_tab[pos] == (AjsString *)(uintptr_t)1 && first_tomb < 0) {
            first_tomb = (int32_t)pos;
        }
        pos = (pos + 1u) & mask;
    }
    if (first_tomb >= 0) pos = (uint32_t)first_tomb;
    else vm->intern_count++;
    vm->intern_tab[pos] = s;
    s->is_interned = true;
}

AjsString *ajs_str_from_utf16(AjsVm *vm, const uint16_t *units, size_t unit_len, bool intern) {
    if (unit_len > 0x3FFFFFFFu) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid string length");
        return NULL;
    }
    uint32_t len16 = (uint32_t)unit_len;
    uint32_t hash = ajs_hash_utf16(units, len16);
    if (intern) {
        AjsString *found = ajs_intern_lookup(vm, units, len16, hash);
        if (found) return found;
    }

    size_t len8 = 0;
    for (uint32_t i = 0; i < len16; ) {
        uint32_t adv = 1;
        uint32_t cp = ajs_utf16_code_point_at(units, len16, i, &adv);
        char tmp[4];
        len8 += ajs_code_point_to_utf8(cp, tmp);
        i += adv;
    }

    size_t units_bytes = ((size_t)len16 + 1u) * sizeof(uint16_t);
    size_t total_size = sizeof(AjsString) + units_bytes + len8 + 1u;
    AjsString *s = (AjsString *)ajs_gc_alloc(vm, total_size, AJS_GC_STRING);
    if (!s) return NULL;

    s->len16 = len16;
    s->len8 = (uint32_t)len8;
    s->hash = hash;
    s->is_interned = false;
    s->units = (uint16_t *)(void *)(s + 1);
    s->utf8 = (char *)(void *)(s->units + len16 + 1u);

    ajs_memcpy(s->units, units, (size_t)len16 * sizeof(uint16_t));
    s->units[len16] = 0;

    size_t pos8 = 0;
    for (uint32_t i = 0; i < len16; ) {
        uint32_t adv = 1;
        uint32_t cp = ajs_utf16_code_point_at(units, len16, i, &adv);
        pos8 += ajs_code_point_to_utf8(cp, s->utf8 + pos8);
        i += adv;
    }
    s->utf8[len8] = '\0';
    ajs_classify_array_idx(s);

    if (intern) ajs_intern_insert(vm, s);
    return s;
}

AjsString *ajs_str_from_utf8(AjsVm *vm, const char *utf8, size_t byte_len, bool intern) {
    if (byte_len == 0) {
        uint16_t dummy = 0;
        return ajs_str_from_utf16(vm, &dummy, 0, intern);
    }
    /* Fast path for pure ASCII */
    bool all_ascii = true;
    for (size_t i = 0; i < byte_len; i++) {
        if ((uint8_t)utf8[i] >= 0x80u) {
            all_ascii = false;
            break;
        }
    }
    uint16_t stack_buf[256];
    stack_buf[0] = 0;
    uint16_t *buf = stack_buf;
    if (byte_len > 256) {
        buf = (uint16_t *)malloc(byte_len * sizeof(uint16_t));
        if (!buf) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Out of memory");
            return NULL;
        }
    }
    size_t ulen = 0;
    if (all_ascii) {
        for (size_t i = 0; i < byte_len; i++) buf[i] = (uint8_t)utf8[i];
        ulen = byte_len;
    } else {
        size_t pos = 0;
        while (pos < byte_len) {
            size_t adv = 1;
            uint32_t cp = ajs_utf8_decode_one(utf8 + pos, byte_len - pos, &adv);
            uint16_t pair[2];
            size_t n = ajs_code_point_to_utf16(cp, pair);
            buf[ulen++] = pair[0];
            if (n == 2) buf[ulen++] = pair[1];
            pos += adv;
        }
    }
    AjsString *res = ajs_str_from_utf16(vm, buf, ulen, intern);
    if (buf != stack_buf) free(buf);
    return res;
}

AjsString *ajs_str_from_cstr(AjsVm *vm, const char *cstr, bool intern) {
    if (!cstr) cstr = "";
    return ajs_str_from_utf8(vm, cstr, strlen(cstr), intern);
}

AjsString *ajs_str_intern(AjsVm *vm, AjsString *str) {
    if (!str || str->is_interned) return str;
    AjsString *existing = ajs_intern_lookup(vm, str->units, str->len16, str->hash);
    if (existing) return existing;
    ajs_intern_insert(vm, str);
    return str;
}

AjsString *ajs_str_concat(AjsVm *vm, const AjsString *a, const AjsString *b) {
    if (!a || a->len16 == 0) return (AjsString *)b;
    if (!b || b->len16 == 0) return (AjsString *)a;
    uint64_t total = (uint64_t)a->len16 + (uint64_t)b->len16;
    if (total > 0x3FFFFFFFu) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid string length");
        return NULL;
    }
    uint16_t stack_buf[256];
    uint16_t *buf = stack_buf;
    if (total > 256) {
        buf = (uint16_t *)malloc((size_t)total * sizeof(uint16_t));
        if (!buf) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Out of memory");
            return NULL;
        }
    }
    ajs_memcpy(buf, a->units, (size_t)a->len16 * sizeof(uint16_t));
    ajs_memcpy(buf + a->len16, b->units, (size_t)b->len16 * sizeof(uint16_t));
    AjsString *res = ajs_str_from_utf16(vm, buf, (size_t)total, false);
    if (buf != stack_buf) free(buf);
    return res;
}

int ajs_str_cmp_utf16(const AjsString *a, const AjsString *b) {
    if (a == b) return 0;
    uint32_t min_len = a->len16 < b->len16 ? a->len16 : b->len16;
    for (uint32_t i = 0; i < min_len; i++) {
        if (a->units[i] != b->units[i]) {
            return a->units[i] < b->units[i] ? -1 : 1;
        }
    }
    if (a->len16 == b->len16) return 0;
    return a->len16 < b->len16 ? -1 : 1;
}

bool ajs_str_eq(const AjsString *a, const AjsString *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->len16 != b->len16 || a->hash != b->hash) return false;
    if (a->is_interned && b->is_interned) return false;
    return a->len16 == 0 || memcmp(a->units, b->units, (size_t)a->len16 * sizeof(uint16_t)) == 0;
}

bool ajs_str_eq_cstr(const AjsString *a, const char *cstr) {
    if (!a || !cstr) return false;
    size_t clen = strlen(cstr);
    if (a->len8 != clen) return false;
    if (clen == 0) return a->len16 == 0;
    for (size_t i = 0; i < clen; i++) {
        if ((uint8_t)cstr[i] >= 0x80u) {
            return memcmp(a->utf8, cstr, clen) == 0;
        }
    }
    if (a->len16 != clen) return false;
    for (size_t i = 0; i < clen; i++) {
        if (a->units[i] != (uint8_t)cstr[i]) return false;
    }
    return true;
}

/* ============================================================================
 * Unicode Character Properties & Case Folding (ES2026)
 * ============================================================================ */

bool ajs_unicode_is_line_terminator(uint32_t cp) {
    return cp == 0x000Au || cp == 0x000Du || cp == 0x2028u || cp == 0x2029u;
}

bool ajs_unicode_is_whitespace(uint32_t cp) {
    if (cp == 0x20u || (cp >= 0x09u && cp <= 0x0Du)) return true;
    if (cp == 0x00A0u || cp == 0x1680u || cp == 0xFEFFu) return true;
    if (cp >= 0x2000u && cp <= 0x200Au) return true;
    if (cp == 0x2028u || cp == 0x2029u || cp == 0x202Fu || cp == 0x205Fu || cp == 0x3000u) return true;
    return false;
}

static bool ajs_unicode_is_letter(uint32_t cp) {
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return true;
    if (cp == 0x00AAu || cp == 0x00B5u || cp == 0x00BAu) return true;
    if (cp >= 0x00C0u && cp <= 0x02AFu && cp != 0x00D7u && cp != 0x00F7u) return true;
    if (cp >= 0x0370u && cp <= 0x03FFu && cp != 0x037Eu && cp != 0x0387u) return true; /* Greek */
    if (cp >= 0x0400u && cp <= 0x052Fu) return true; /* Cyrillic */
    if (cp >= 0x0531u && cp <= 0x0587u) return true; /* Armenian */
    if (cp >= 0x05D0u && cp <= 0x05EAu) return true; /* Hebrew */
    if (cp >= 0x0620u && cp <= 0x064Au) return true; /* Arabic */
    if (cp >= 0x0904u && cp <= 0x0939u) return true; /* Devanagari */
    if (cp >= 0x1100u && cp <= 0x11FFu) return true; /* Hangul Jamo */
    if (cp >= 0x1E00u && cp <= 0x1FFFu) return true; /* Latin/Greek Ext */
    if (cp >= 0x3041u && cp <= 0x3096u) return true; /* Hiragana */
    if (cp >= 0x30A1u && cp <= 0x30FAu) return true; /* Katakana */
    if (cp >= 0x3131u && cp <= 0x318Eu) return true; /* Hangul Compat Jamo */
    if (cp >= 0x3400u && cp <= 0x4DBFu) return true; /* CJK Ext A */
    if (cp >= 0x4E00u && cp <= 0x9FFFu) return true; /* CJK Unified */
    if (cp >= 0xAC00u && cp <= 0xD7A3u) return true; /* Hangul Syllables */
    if (cp >= 0xF900u && cp <= 0xFAFFu) return true; /* CJK Compat */
    if (cp >= 0x10000u && cp <= 0x104AFu) return true;
    if (cp >= 0x20000u && cp <= 0x2FA1Fu) return true;
    return false;
}

bool ajs_unicode_is_id_start(uint32_t cp) {
    if (cp == '$' || cp == '_') return true;
    if (ajs_unicode_is_letter(cp)) return true;
    if (cp == 0x2118u || cp == 0x212Eu || (cp >= 0x2160u && cp <= 0x2188u)) return true;
    return false;
}

bool ajs_unicode_is_id_continue(uint32_t cp) {
    if (ajs_unicode_is_id_start(cp)) return true;
    if (cp >= '0' && cp <= '9') return true;
    if (cp == 0x200Cu || cp == 0x200Du || cp == 0x00B7u || cp == 0x203Fu || cp == 0x2040u) return true;
    if (cp >= 0x0300u && cp <= 0x036Fu) return true; /* Combining diacritical marks */
    if (cp >= 0x0660u && cp <= 0x0669u) return true;
    if (cp >= 0x0966u && cp <= 0x096Fu) return true;
    if (cp >= 0xFF10u && cp <= 0xFF19u) return true;
    return false;
}

uint32_t ajs_unicode_to_lower(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32u;
    if (cp >= 0x00C0u && cp <= 0x00D6u) return cp + 32u;
    if (cp >= 0x00D8u && cp <= 0x00DEu) return cp + 32u;
    if (cp == 0x0178u) return 0x00FFu;
    if (cp >= 0x0100u && cp <= 0x012Eu && (cp & 1u) == 0) return cp + 1u;
    if (cp >= 0x0391u && cp <= 0x03ABu && cp != 0x03A2u) return cp + 32u;
    if (cp >= 0x0410u && cp <= 0x042Fu) return cp + 32u;
    if (cp == 0x0401u) return 0x0451u;
    return cp;
}

uint32_t ajs_unicode_to_upper(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32u;
    if (cp >= 0x00E0u && cp <= 0x00F6u) return cp - 32u;
    if (cp >= 0x00F8u && cp <= 0x00FEu) return cp - 32u;
    if (cp == 0x00FFu) return 0x0178u;
    if (cp >= 0x0101u && cp <= 0x012Fu && (cp & 1u) == 1) return cp - 1u;
    if (cp >= 0x03B1u && cp <= 0x03CBu && cp != 0x03C2u) return cp - 32u;
    if (cp == 0x03C2u) return 0x03A3u;
    if (cp >= 0x0430u && cp <= 0x044Fu) return cp - 32u;
    if (cp == 0x0451u) return 0x0401u;
    return cp;
}

uint32_t ajs_unicode_fold_case(uint32_t cp, bool unicode_mode) {
    if (!unicode_mode) {
        if (cp < 128u) return ajs_unicode_to_upper(cp);
        uint32_t up = ajs_unicode_to_upper(cp);
        return up < 128u ? cp : up;
    }
    if (cp == 0x017Fu) return 's';
    if (cp == 0x212Au) return 'k';
    return ajs_unicode_to_lower(cp);
}

static bool ajs_unicode_is_emoji(uint32_t cp) {
    if (cp == 0x00A9u || cp == 0x00AEu || cp == 0x203Cu || cp == 0x2049u || cp == 0x2122u) return true;
    if (cp >= 0x231Au && cp <= 0x23FAu) return true;
    if (cp >= 0x2600u && cp <= 0x27BFu) return true;
    if (cp >= 0x2B50u && cp <= 0x2B55u) return true;
    if (cp >= 0x1F000u && cp <= 0x1FAFFu) return true;
    return false;
}

static bool ajs_unicode_is_number(uint32_t cp) {
    if (cp >= '0' && cp <= '9') return true;
    if (cp == 0x00B2u || cp == 0x00B3u || cp == 0x00B9u) return true;
    if (cp >= 0x00BCu && cp <= 0x00BEu) return true;
    if (cp >= 0x0660u && cp <= 0x0669u) return true;
    if (cp >= 0x06F0u && cp <= 0x06F9u) return true;
    if (cp >= 0x0966u && cp <= 0x096Fu) return true;
    if (cp >= 0x2160u && cp <= 0x2188u) return true;
    if (cp >= 0xFF10u && cp <= 0xFF19u) return true;
    return false;
}

bool ajs_unicode_match_prop(const char *prop_name, const char *prop_val_or_null, uint32_t cp) {
    if (!prop_val_or_null || prop_val_or_null[0] == '\0') {
        if (strcmp(prop_name, "ASCII") == 0) return cp <= 0x7Fu;
        if (strcmp(prop_name, "ASCII_Hex_Digit") == 0 || strcmp(prop_name, "AHex") == 0) {
            return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'F') || (cp >= 'a' && cp <= 'f');
        }
        if (strcmp(prop_name, "Any") == 0) return cp <= 0x10FFFFu;
        if (strcmp(prop_name, "Assigned") == 0) return cp <= 0x10FFFFu && !(cp >= 0xFDD0u && cp <= 0xFDEFu);
        if (strcmp(prop_name, "White_Space") == 0 || strcmp(prop_name, "WSpace") == 0 ||
            strcmp(prop_name, "space") == 0) {
            return ajs_unicode_is_whitespace(cp);
        }
        if (strcmp(prop_name, "Emoji") == 0 || strcmp(prop_name, "Emoji_Presentation") == 0 ||
            strcmp(prop_name, "Extended_Pictographic") == 0) {
            if (strcmp(prop_name, "Emoji") == 0 && ((cp >= '0' && cp <= '9') || cp == '#' || cp == '*')) {
                return true;
            }
            return ajs_unicode_is_emoji(cp);
        }
        if (strcmp(prop_name, "ID_Start") == 0) return ajs_unicode_is_id_start(cp) && cp != '$' && cp != '_';
        if (strcmp(prop_name, "ID_Continue") == 0) return ajs_unicode_is_id_continue(cp) && cp != '$';
        if (strcmp(prop_name, "Letter") == 0 || strcmp(prop_name, "L") == 0 ||
            strcmp(prop_name, "Alphabetic") == 0 || strcmp(prop_name, "Alpha") == 0) {
            return ajs_unicode_is_letter(cp);
        }
        if (strcmp(prop_name, "Uppercase_Letter") == 0 || strcmp(prop_name, "Lu") == 0 ||
            strcmp(prop_name, "Uppercase") == 0 || strcmp(prop_name, "Upper") == 0) {
            return ajs_unicode_to_lower(cp) != cp;
        }
        if (strcmp(prop_name, "Lowercase_Letter") == 0 || strcmp(prop_name, "Ll") == 0 ||
            strcmp(prop_name, "Lowercase") == 0 || strcmp(prop_name, "Lower") == 0) {
            return ajs_unicode_to_upper(cp) != cp;
        }
        if (strcmp(prop_name, "Number") == 0 || strcmp(prop_name, "N") == 0) {
            return ajs_unicode_is_number(cp);
        }
        if (strcmp(prop_name, "Decimal_Number") == 0 || strcmp(prop_name, "Nd") == 0 ||
            strcmp(prop_name, "digit") == 0) {
            return (cp >= '0' && cp <= '9') || (cp >= 0x0660u && cp <= 0x0669u) ||
                   (cp >= 0x0966u && cp <= 0x096Fu) || (cp >= 0xFF10u && cp <= 0xFF19u);
        }
        if (strcmp(prop_name, "Punctuation") == 0 || strcmp(prop_name, "P") == 0) {
            return (cp >= 0x21u && cp <= 0x2Fu && cp != '$' && cp != '+' && cp != '<' && cp != '=' && cp != '>') ||
                   (cp >= 0x3Au && cp <= 0x40u && cp != '<' && cp != '=' && cp != '>') ||
                   (cp >= 0x5Bu && cp <= 0x60u && cp != '^' && cp != '`') ||
                   (cp >= 0x7Bu && cp <= 0x7Eu && cp != '|' && cp != '~') ||
                   (cp >= 0x2010u && cp <= 0x2027u) || (cp >= 0x3001u && cp <= 0x301Fu);
        }
        return false;
    }

    if (strcmp(prop_name, "General_Category") == 0 || strcmp(prop_name, "gc") == 0) {
        return ajs_unicode_match_prop(prop_val_or_null, NULL, cp);
    }
    if (strcmp(prop_name, "Script") == 0 || strcmp(prop_name, "sc") == 0 ||
        strcmp(prop_name, "Script_Extensions") == 0 || strcmp(prop_name, "scx") == 0) {
        if (strcmp(prop_val_or_null, "Latin") == 0 || strcmp(prop_val_or_null, "Latn") == 0) {
            return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
                   (cp >= 0x00C0u && cp <= 0x024Fu && cp != 0x00D7u && cp != 0x00F7u) ||
                   (cp >= 0x1E00u && cp <= 0x1EFFu);
        }
        if (strcmp(prop_val_or_null, "Hangul") == 0 || strcmp(prop_val_or_null, "Hang") == 0) {
            return (cp >= 0xAC00u && cp <= 0xD7A3u) ||
                   (cp >= 0x1100u && cp <= 0x11FFu) ||
                   (cp >= 0x3131u && cp <= 0x318Eu);
        }
        if (strcmp(prop_val_or_null, "Han") == 0 || strcmp(prop_val_or_null, "Hani") == 0) {
            return (cp >= 0x3400u && cp <= 0x4DBFu) || (cp >= 0x4E00u && cp <= 0x9FFFu) ||
                   (cp >= 0xF900u && cp <= 0xFAFFu) || (cp >= 0x20000u && cp <= 0x2FA1Fu);
        }
        if (strcmp(prop_val_or_null, "Hiragana") == 0 || strcmp(prop_val_or_null, "Hira") == 0) {
            return cp >= 0x3041u && cp <= 0x309Fu;
        }
        if (strcmp(prop_val_or_null, "Katakana") == 0 || strcmp(prop_val_or_null, "Kana") == 0) {
            return cp >= 0x30A0u && cp <= 0x30FFu;
        }
        if (strcmp(prop_val_or_null, "Greek") == 0 || strcmp(prop_val_or_null, "Grek") == 0) {
            return (cp >= 0x0370u && cp <= 0x03FFu) || (cp >= 0x1F00u && cp <= 0x1FFFu);
        }
        if (strcmp(prop_val_or_null, "Cyrillic") == 0 || strcmp(prop_val_or_null, "Cyrl") == 0) {
            return cp >= 0x0400u && cp <= 0x052Fu;
        }
    }
    return false;
}

/* ============================================================================
 * IEEE-754 Exact Integer / String Conversions (Zero UBSan Float-Cast UB)
 * ============================================================================ */

uint32_t ajs_double_to_uint32(double d) {
    uint64_t bits = ajs_double_to_bits(d);
    int exp = (int)((bits >> 52) & 0x7FFu) - 1023;
    if (exp < 0 || exp >= 84) return 0u;
    uint64_t mant = (bits & UINT64_C(0x000FFFFFFFFFFFFF)) | UINT64_C(0x0010000000000000);
    uint32_t u = (exp >= 52) ? (uint32_t)(mant << (exp - 52)) : (uint32_t)(mant >> (52 - exp));
    if ((bits >> 63) != 0) u = 0u - u;
    return u;
}

int32_t ajs_double_to_int32(double d) {
    uint32_t u = ajs_double_to_uint32(d);
    if (u >= 0x80000000u) {
        return -(int32_t)(~u) - 1;
    }
    return (int32_t)u;
}

double ajs_double_to_integer_or_inf(double d) {
    if (isnan(d) || d == 0.0) return 0.0;
    if (isinf(d)) return d;
    double t = trunc(d);
    return t == 0.0 ? 0.0 : ajs_f64_round(t);
}

AjsString *ajs_number_to_str(AjsVm *vm, double d, int radix) {
    if (isnan(d)) return ajs_str_from_cstr(vm, "NaN", true);
    if (d == 0.0) return ajs_str_from_cstr(vm, "0", true);
    if (isinf(d)) return ajs_str_from_cstr(vm, d < 0 ? "-Infinity" : "Infinity", true);

    if (radix != 10) {
        static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        bool neg = d < 0.0;
        double abs_d = neg ? -d : d;
        double int_part = floor(abs_d);
        double frac_part = abs_d - int_part;

        char rev_int[1100];
        size_t rev_len = 0;
        if (int_part == 0.0) {
            rev_int[rev_len++] = '0';
        } else {
            while (int_part > 0.0 && rev_len < sizeof(rev_int) - 1) {
                double q = floor(int_part / (double)radix);
                int rem = (int)(int_part - q * (double)radix);
                if (rem < 0) rem = 0;
                if (rem >= radix) rem = radix - 1;
                rev_int[rev_len++] = digits[rem];
                int_part = q;
            }
        }
        char out[1200];
        size_t pos = 0;
        if (neg) out[pos++] = '-';
        for (size_t i = 0; i < rev_len; i++) {
            out[pos++] = rev_int[rev_len - 1u - i];
        }
        if (frac_part > 0.0) {
            out[pos++] = '.';
            int max_frac = 52;
            while (frac_part > 0.0 && max_frac-- > 0 && pos < sizeof(out) - 2) {
                frac_part *= (double)radix;
                int dig = (int)floor(frac_part);
                if (dig < 0) dig = 0;
                if (dig >= radix) dig = radix - 1;
                out[pos++] = digits[dig];
                frac_part -= (double)dig;
            }
        }
        out[pos] = '\0';
        return ajs_str_from_utf8(vm, out, pos, false);
    }

    /* Fast path for exact safe integers in [-9007199254740991, 9007199254740991] */
    if (floor(d) == d && fabs(d) <= 9007199254740991.0) {
        char buf[64];
        bool neg = d < 0.0;
        uint64_t u = (uint64_t)(neg ? -d : d);
        char rev[32];
        size_t rl = 0;
        while (u > 0) {
            rev[rl++] = (char)('0' + (u % 10u));
            u /= 10u;
        }
        size_t p = 0;
        if (neg) buf[p++] = '-';
        for (size_t i = 0; i < rl; i++) buf[p++] = rev[rl - 1u - i];
        buf[p] = '\0';
        return ajs_str_from_utf8(vm, buf, p, rl <= 2 && !neg);
    }

    /*Shortest round-trip decimal representation in [1e-6, 1e21) fixed vs exponential */
    double abs_d = fabs(d);
    char cand[64];
    int prec = 15;
    for (int p = 15; p <= 17; p++) {
        snprintf(cand, sizeof(cand), "%.*e", p - 1, abs_d);
        char *endp = NULL;
        double parsed = ajs_f64_round(strtod(cand, &endp));
        if (parsed == abs_d) {
            prec = p;
            break;
        }
    }
    snprintf(cand, sizeof(cand), "%.*e", prec - 1, abs_d);
    char *epos = strchr(cand, 'e');
    if (!epos) epos = strchr(cand, 'E');
    int exp10 = epos ? atoi(epos + 1) : 0;

    char digits[32];
    size_t ndig = 0;
    for (char *c = cand; c && c != epos; c++) {
        if (*c >= '0' && *c <= '9') digits[ndig++] = *c;
    }
    while (ndig > 1 && digits[ndig - 1] == '0') ndig--;

    char out[128];
    size_t pos = 0;
    if (d < 0.0) out[pos++] = '-';
    int k = (int)ndig;
    int n = exp10 + 1; /* ECMA-262 Number::toString n & k */

    if (k <= n && n <= 21) {
        for (int i = 0; i < k; i++) out[pos++] = digits[i];
        for (int i = 0; i < n - k; i++) out[pos++] = '0';
    } else if (0 < n && n <= 21) {
        for (int i = 0; i < n; i++) out[pos++] = digits[i];
        out[pos++] = '.';
        for (int i = n; i < k; i++) out[pos++] = digits[i];
    } else if (-6 < n && n <= 0) {
        out[pos++] = '0';
        out[pos++] = '.';
        for (int i = 0; i < -n; i++) out[pos++] = '0';
        for (int i = 0; i < k; i++) out[pos++] = digits[i];
    } else {
        out[pos++] = digits[0];
        if (k > 1) {
            out[pos++] = '.';
            for (int i = 1; i < k; i++) out[pos++] = digits[i];
        }
        out[pos++] = 'e';
        int e_val = n - 1;
        out[pos++] = e_val >= 0 ? '+' : '-';
        int abs_e = e_val >= 0 ? e_val : -e_val;
        pos += (size_t)snprintf(out + pos, sizeof(out) - pos, "%d", abs_e);
    }
    out[pos] = '\0';
    return ajs_str_from_utf8(vm, out, pos, false);
}

static int ajs_digit_val(uint16_t ch) {
    if (ch >= '0' && ch <= '9') return (int)(ch - '0');
    if (ch >= 'a' && ch <= 'z') return (int)(ch - 'a' + 10);
    if (ch >= 'A' && ch <= 'Z') return (int)(ch - 'A' + 10);
    return -1;
}

double ajs_str_to_number(const AjsString *s) {
    if (!s) return 0.0;
    uint32_t start = 0;
    uint32_t end = s->len16;
    while (start < end && ajs_unicode_is_whitespace(s->units[start])) start++;
    while (end > start && ajs_unicode_is_whitespace(s->units[end - 1])) end--;
    uint32_t len = end - start;
    if (len == 0) return 0.0;

    const uint16_t *u = s->units + start;
    /* Check 0x / 0b / 0o (no sign allowed on radix prefix in StringToNumber!) */
    if (len >= 3 && u[0] == '0' &&
        (u[1] == 'x' || u[1] == 'X' || u[1] == 'b' || u[1] == 'B' || u[1] == 'o' || u[1] == 'O')) {
        int base = (u[1] == 'x' || u[1] == 'X') ? 16 : ((u[1] == 'b' || u[1] == 'B') ? 2 : 8);
        double acc = 0.0;
        for (uint32_t i = 2; i < len; i++) {
            int dv = ajs_digit_val(u[i]);
            if (dv < 0 || dv >= base) return NAN;
            acc = ajs_f64_round(acc * (double)base + (double)dv);
        }
        return acc;
    }

    uint32_t idx = 0;
    bool neg = false;
    if (u[idx] == '+' || u[idx] == '-') {
        neg = (u[idx] == '-');
        idx++;
    }
    if (idx + 8 == len &&
        u[idx] == 'I' && u[idx+1] == 'n' && u[idx+2] == 'f' && u[idx+3] == 'i' &&
        u[idx+4] == 'n' && u[idx+5] == 'i' && u[idx+6] == 't' && u[idx+7] == 'y') {
        return neg ? -INFINITY : INFINITY;
    }

    char ascii[512];
    char *buf = ascii;
    if (len >= sizeof(ascii)) {
        buf = (char *)malloc((size_t)len + 1u);
        if (!buf) return NAN;
    }
    bool saw_digit = false;
    bool saw_dot = false;
    bool saw_exp = false;
    for (uint32_t i = idx; i < len; i++) {
        uint16_t ch = u[i];
        if (ch >= '0' && ch <= '9') {
            saw_digit = true;
        } else if (ch == '.' && !saw_dot && !saw_exp) {
            saw_dot = true;
        } else if ((ch == 'e' || ch == 'E') && !saw_exp && saw_digit) {
            saw_exp = true;
            if (i + 1 < len && (u[i + 1] == '+' || u[i + 1] == '-')) {
                buf[i] = (char)ch;
                i++;
                ch = u[i];
            }
            if (i + 1 >= len && !(ch >= '0' && ch <= '9')) {
                if (buf != ascii) free(buf);
                return NAN;
            }
            if ((ch == '+' || ch == '-') && (i + 1 >= len || u[i + 1] < '0' || u[i + 1] > '9')) {
                if (buf != ascii) free(buf);
                return NAN;
            }
        } else {
            if (buf != ascii) free(buf);
            return NAN;
        }
        buf[i] = (char)ch;
    }
    if (!saw_digit) {
        if (buf != ascii) free(buf);
        return NAN;
    }
    for (uint32_t i = 0; i < idx; i++) buf[i] = (char)u[i];
    buf[len] = '\0';
    char *endp = NULL;
    double res = ajs_f64_round(strtod(buf, &endp));
    bool ok = (endp == buf + len);
    if (buf != ascii) free(buf);
    return ok ? res : NAN;
}

double ajs_parse_int_str(const uint16_t *units, uint32_t len, int32_t radix) {
    uint32_t pos = 0;
    while (pos < len && ajs_unicode_is_whitespace(units[pos])) pos++;
    bool neg = false;
    if (pos < len && (units[pos] == '-' || units[pos] == '+')) {
        neg = (units[pos] == '-');
        pos++;
    }
    bool strip_prefix = true;
    if (radix != 0) {
        if (radix < 2 || radix > 36) return NAN;
        if (radix != 16) strip_prefix = false;
    } else {
        radix = 10;
    }
    if (strip_prefix && pos + 2 <= len && units[pos] == '0' &&
        (units[pos + 1] == 'x' || units[pos + 1] == 'X')) {
        pos += 2;
        radix = 16;
    }
    uint32_t start = pos;
    double acc = 0.0;
    while (pos < len) {
        int dv = ajs_digit_val(units[pos]);
        if (dv < 0 || dv >= radix) break;
        acc = ajs_f64_round(acc * (double)radix + (double)dv);
        pos++;
    }
    if (pos == start) return NAN;
    return neg ? -acc : acc;
}

double ajs_parse_float_str(const uint16_t *units, uint32_t len) {
    uint32_t pos = 0;
    while (pos < len && ajs_unicode_is_whitespace(units[pos])) pos++;
    if (pos >= len) return NAN;

    uint32_t start = pos;
    bool neg = false;
    if (units[pos] == '+' || units[pos] == '-') {
        neg = (units[pos] == '-');
        pos++;
    }
    if (pos + 8 <= len &&
        units[pos] == 'I' && units[pos+1] == 'n' && units[pos+2] == 'f' && units[pos+3] == 'i' &&
        units[pos+4] == 'n' && units[pos+5] == 'i' && units[pos+6] == 't' && units[pos+7] == 'y') {
        return neg ? -INFINITY : INFINITY;
    }
    uint32_t int_start = pos;
    while (pos < len && units[pos] >= '0' && units[pos] <= '9') pos++;
    uint32_t int_digits = pos - int_start;
    uint32_t frac_digits = 0;
    if (pos < len && units[pos] == '.') {
        pos++;
        uint32_t fstart = pos;
        while (pos < len && units[pos] >= '0' && units[pos] <= '9') pos++;
        frac_digits = pos - fstart;
    }
    if (int_digits == 0 && frac_digits == 0) return NAN;
    uint32_t valid_end = pos;
    if (pos < len && (units[pos] == 'e' || units[pos] == 'E')) {
        uint32_t epos = pos + 1;
        if (epos < len && (units[epos] == '+' || units[epos] == '-')) epos++;
        uint32_t edig_start = epos;
        while (epos < len && units[epos] >= '0' && units[epos] <= '9') epos++;
        if (epos > edig_start) valid_end = epos;
    }
    uint32_t slen = valid_end - start;
    char stack_buf[256];
    char *buf = stack_buf;
    if (slen >= sizeof(stack_buf)) {
        buf = (char *)malloc((size_t)slen + 1u);
        if (!buf) return NAN;
    }
    for (uint32_t i = 0; i < slen; i++) buf[i] = (char)units[start + i];
    buf[slen] = '\0';
    double res = ajs_f64_round(strtod(buf, NULL));
    if (buf != stack_buf) free(buf);
    return res;
}
