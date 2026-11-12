/*
 * Arcradient (MSHTML-Next)
 * base/arcr_str.c - Win2000-Safe Pure C String Utilities
 */
#include "arcr_base.h"
#include <string.h>

char arcr_ascii_tolower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

size_t arcr_str_copy(char *dst, size_t dst_cap, const char *src) {
    if (!dst || dst_cap == 0u) return 0u;
    if (!src) {
        dst[0] = '\0';
        return 0u;
    }
    size_t i = 0u;
    while (i + 1u < dst_cap && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
    while (src[i] != '\0') {
        ++i;
    }
    return i;
}

bool arcr_str_eq_nocase_n(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len) return false;
    if (a_len == 0u) return true;
    if (!a || !b) return false;
    for (size_t i = 0u; i < a_len; ++i) {
        if (arcr_ascii_tolower(a[i]) != arcr_ascii_tolower(b[i])) {
            return false;
        }
    }
    return true;
}

bool arcr_str_eq_nocase(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return arcr_str_eq_nocase_n(a, strlen(a), b, strlen(b));
}

bool arcr_str_starts_with_nocase(const char *str, const char *prefix) {
    if (!str || !prefix) return false;
    size_t plen = strlen(prefix);
    size_t slen = strlen(str);
    if (slen < plen) return false;
    return arcr_str_eq_nocase_n(str, plen, prefix, plen);
}

static bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

void arcr_str_trim_view(const char **inout_ptr, size_t *inout_len) {
    if (!inout_ptr || !*inout_ptr || !inout_len) return;
    const char *p = *inout_ptr;
    size_t      n = *inout_len;
    while (n > 0u && is_ascii_space(p[0])) {
        ++p;
        --n;
    }
    while (n > 0u && is_ascii_space(p[n - 1u])) {
        --n;
    }
    *inout_ptr = p;
    *inout_len = n;
}

bool arcr_parse_i32(const char *s, size_t len, int32_t *out_val) {
    if (!s || len == 0u || !out_val) return false;
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;

    size_t  i   = 0u;
    int64_t sign = 1;
    if (s[0] == '-') {
        sign = -1;
        ++i;
    } else if (s[0] == '+') {
        ++i;
    }
    if (i >= len) return false;

    int64_t acc = 0;
    size_t  digits = 0u;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        acc = acc * 10 + (s[i] - '0');
        if (acc > 2147483648LL) break;
        ++i;
        ++digits;
    }
    if (digits == 0u) return false;
    int64_t res = acc * sign;
    if (res < INT32_MIN) res = INT32_MIN;
    if (res > INT32_MAX) res = INT32_MAX;
    *out_val = (int32_t)res;
    return true;
}
