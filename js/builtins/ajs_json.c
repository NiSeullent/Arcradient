/*
 * ArcradientJS 1.0 (Clean-Room Pure C11 ECMAScript 2026 Virtual Machine)
 * js/builtins/ajs_json.c - RFC 8259 & ES2026 JSON.parse (with reviver source context),
 * JSON.stringify (well-formed UTF-16 surrogates, replacer, space), JSON.rawJSON & isRawJSON
 */
#include "../vm/ajs_internal.h"

#define AJS_METHOD_FLAGS        (AJS_PROP_WRITABLE | AJS_PROP_CONFIGURABLE)
#define AJS_OPAQUE_TAG_RAW_JSON 0x5241574Au
#define AJS_JSON_MAX_DEPTH      512u

/* ============================================================================
 * 1. Dynamic UTF-8 / UTF-16 Output Buffer
 * ============================================================================ */

typedef struct JsonStrBuf {
    char  *data;
    size_t len;
    size_t cap;
    bool   oom;
} JsonStrBuf;

static bool jbuf_reserve(JsonStrBuf *b, size_t extra) {
    if (b->oom) return false;
    if (b->len + extra + 1u <= b->cap) return true;
    size_t ncap = b->cap ? b->cap * 2u : 128u;
    while (ncap < b->len + extra + 1u) ncap *= 2u;
    char *nd = (char *)realloc(b->data, ncap);
    if (!nd) {
        b->oom = true;
        return false;
    }
    b->data = nd;
    b->cap = ncap;
    return true;
}

static void jbuf_putc(JsonStrBuf *b, char c) {
    if (!jbuf_reserve(b, 1u)) return;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

static void jbuf_puts(JsonStrBuf *b, const char *s, size_t n) {
    if (n == 0) return;
    if (!jbuf_reserve(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void jbuf_put_cstr(JsonStrBuf *b, const char *s) {
    jbuf_puts(b, s, strlen(s));
}

/* ============================================================================
 * 2. Strict RFC 8259 UTF-16 Recursive-Descent Parser + Source Tracking
 * ============================================================================ */

typedef struct JsonParseNode {
    AjsValue              val;
    AjsString            *raw_src;      /* Non-NULL for primitive leaf values */
    uint32_t              prop_len;
    struct JsonParseNode *children;     /* Parallel child metadata for reviver source context */
    AjsValue             *child_keys;   /* Keys for object children */
} JsonParseNode;

static void jnode_free_meta(JsonParseNode *n) {
    if (!n) return;
    for (uint32_t i = 0; i < n->prop_len; i++) {
        jnode_free_meta(&n->children[i]);
    }
    free(n->children);
    free(n->child_keys);
    n->children = NULL;
    n->child_keys = NULL;
    n->prop_len = 0;
}

typedef struct JsonParser {
    AjsVm          *vm;
    const uint16_t *u;
    uint32_t        len;
    uint32_t        pos;
    uint32_t        depth;
    bool            track_src;
} JsonParser;

static bool is_json_ws(uint16_t c) {
    return c == 0x20u || c == 0x09u || c == 0x0Au || c == 0x0Du;
}

static void jp_skip_ws(JsonParser *p) {
    while (p->pos < p->len && is_json_ws(p->u[p->pos])) p->pos++;
}

static int hex_digit_val(uint16_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

static AjsString *jp_parse_string_raw(JsonParser *p, bool intern) {
    if (p->pos >= p->len || p->u[p->pos] != '"') {
        ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Unexpected token in JSON at position %u", p->pos);
        return NULL;
    }
    p->pos++; /* skip '"' */

    uint32_t cap = 32u, out_len = 0;
    uint16_t stack_buf[64];
    uint16_t *buf = stack_buf;
    cap = 64u;

    while (p->pos < p->len) {
        uint16_t c = p->u[p->pos++];
        if (c == '"') {
            AjsString *s = ajs_str_from_utf16(p->vm, buf, out_len, intern);
            if (buf != stack_buf) free(buf);
            return s;
        }
        if (c < 0x20u) {
            if (buf != stack_buf) free(buf);
            ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX,
                               "Unescaped control character in JSON string at position %u", p->pos - 1u);
            return NULL;
        }
        uint16_t emit = c;
        if (c == '\\') {
            if (p->pos >= p->len) {
                if (buf != stack_buf) free(buf);
                ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Unterminated escape in JSON string");
                return NULL;
            }
            uint16_t esc = p->u[p->pos++];
            switch (esc) {
                case '"':  emit = '"'; break;
                case '\\': emit = '\\'; break;
                case '/':  emit = '/'; break;
                case 'b':  emit = '\b'; break;
                case 'f':  emit = '\f'; break;
                case 'n':  emit = '\n'; break;
                case 'r':  emit = '\r'; break;
                case 't':  emit = '\t'; break;
                case 'u': {
                    if (p->pos + 4u > p->len) {
                        if (buf != stack_buf) free(buf);
                        ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Incomplete \\uXXXX escape in JSON");
                        return NULL;
                    }
                    int d0 = hex_digit_val(p->u[p->pos]);
                    int d1 = hex_digit_val(p->u[p->pos + 1u]);
                    int d2 = hex_digit_val(p->u[p->pos + 2u]);
                    int d3 = hex_digit_val(p->u[p->pos + 3u]);
                    if (d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0) {
                        if (buf != stack_buf) free(buf);
                        ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Invalid \\uXXXX hex escape in JSON");
                        return NULL;
                    }
                    p->pos += 4u;
                    emit = (uint16_t)((d0 << 12) | (d1 << 8) | (d2 << 4) | d3);
                    break;
                }
                default:
                    if (buf != stack_buf) free(buf);
                    ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Bad escaped character in JSON string");
                    return NULL;
            }
        }
        if (out_len >= cap) {
            uint32_t ncap = cap * 2u;
            uint16_t *nb = (buf == stack_buf) ? (uint16_t *)malloc((size_t)ncap * sizeof(uint16_t))
                                              : (uint16_t *)realloc(buf, (size_t)ncap * sizeof(uint16_t));
            if (!nb) {
                if (buf != stack_buf) free(buf);
                ajs_throw(p->vm, p->vm->oom_error);
                return NULL;
            }
            if (buf == stack_buf) memcpy(nb, stack_buf, (size_t)out_len * sizeof(uint16_t));
            buf = nb;
            cap = ncap;
        }
        buf[out_len++] = emit;
    }

    if (buf != stack_buf) free(buf);
    ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Unterminated string in JSON");
    return NULL;
}

static bool jp_parse_value(JsonParser *p, JsonParseNode *out);

static bool jp_parse_number(JsonParser *p, JsonParseNode *out) {
    uint32_t start = p->pos;
    if (p->pos < p->len && p->u[p->pos] == '-') p->pos++;
    if (p->pos >= p->len) {
        ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Unexpected end of JSON number");
        return false;
    }
    if (p->u[p->pos] == '0') {
        p->pos++;
        if (p->pos < p->len && p->u[p->pos] >= '0' && p->u[p->pos] <= '9') {
            ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Leading zeros are not allowed in JSON numbers");
            return false;
        }
    } else if (p->u[p->pos] >= '1' && p->u[p->pos] <= '9') {
        while (p->pos < p->len && p->u[p->pos] >= '0' && p->u[p->pos] <= '9') p->pos++;
    } else {
        ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Invalid number in JSON at position %u", start);
        return false;
    }

    if (p->pos < p->len && p->u[p->pos] == '.') {
        p->pos++;
        if (p->pos >= p->len || p->u[p->pos] < '0' || p->u[p->pos] > '9') {
            ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Unterminated fractional number in JSON");
            return false;
        }
        while (p->pos < p->len && p->u[p->pos] >= '0' && p->u[p->pos] <= '9') p->pos++;
    }

    if (p->pos < p->len && (p->u[p->pos] == 'e' || p->u[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->u[p->pos] == '+' || p->u[p->pos] == '-')) p->pos++;
        if (p->pos >= p->len || p->u[p->pos] < '0' || p->u[p->pos] > '9') {
            ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Exponent part is missing a number in JSON");
            return false;
        }
        while (p->pos < p->len && p->u[p->pos] >= '0' && p->u[p->pos] <= '9') p->pos++;
    }

    uint32_t nlen = p->pos - start;
    double d = ajs_parse_float_str(p->u + start, nlen);
    out->val = ajs_val_number(d);
    if (p->track_src) {
        out->raw_src = ajs_str_from_utf16(p->vm, p->u + start, nlen, false);
        if (out->raw_src) ajs_temp_root_push(p->vm, ajs_val_from_gc(out->raw_src));
    }
    return true;
}

static bool jp_match_kw(JsonParser *p, const char *kw, AjsValue val, JsonParseNode *out) {
    uint32_t start = p->pos;
    size_t klen = strlen(kw);
    if (p->pos + klen > p->len) {
        ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Unexpected token in JSON at position %u", start);
        return false;
    }
    for (size_t i = 0; i < klen; i++) {
        if (p->u[p->pos + i] != (uint16_t)kw[i]) {
            ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Unexpected token in JSON at position %u", start);
            return false;
        }
    }
    p->pos += (uint32_t)klen;
    out->val = val;
    if (p->track_src) {
        out->raw_src = ajs_str_from_utf8(p->vm, kw, klen, false);
        if (out->raw_src) ajs_temp_root_push(p->vm, ajs_val_from_gc(out->raw_src));
    }
    return true;
}

static void json_create_data_prop(AjsVm *vm, AjsObject *obj, AjsValue key, AjsValue val) {
    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
              AJS_DESC_HAS_ENUMERABLE | AJS_DESC_ENUMERABLE |
              AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    d.value = val;
    ajs_obj_define_own_prop(vm, obj, key, &d, false);
}

static bool jp_parse_array(JsonParser *p, JsonParseNode *out) {
    if (++p->depth > AJS_JSON_MAX_DEPTH) {
        ajs_throw_err_kind(p->vm, AJS_ERR_KIND_RANGE, "Maximum JSON nesting depth exceeded");
        return false;
    }
    p->pos++; /* '[' */
    AjsValue arr_v = ajs_new_array(p->vm, 0);
    if (p->vm->has_exception) return false;
    ajs_temp_root_push(p->vm, arr_v);

    jp_skip_ws(p);
    if (p->pos < p->len && p->u[p->pos] == ']') {
        p->pos++;
        p->depth--;
        out->val = arr_v;
        return true;
    }

    uint32_t cap = 0, idx = 0;
    JsonParseNode *kids = NULL;
    for (;;) {
        jp_skip_ws(p);
        JsonParseNode child;
        memset(&child, 0, sizeof(child));
        if (!jp_parse_value(p, &child)) {
            for (uint32_t i = 0; i < idx; i++) jnode_free_meta(&kids[i]);
            free(kids);
            return false;
        }
        ajs_set_prop_index(p->vm, arr_v, idx, child.val);
        if (p->track_src) {
            if (idx >= cap) {
                uint32_t ncap = cap ? cap * 2u : 8u;
                JsonParseNode *nk = (JsonParseNode *)realloc(kids, (size_t)ncap * sizeof(JsonParseNode));
                if (!nk) {
                    jnode_free_meta(&child);
                    for (uint32_t i = 0; i < idx; i++) jnode_free_meta(&kids[i]);
                    free(kids);
                    ajs_throw(p->vm, p->vm->oom_error);
                    return false;
                }
                kids = nk;
                cap = ncap;
            }
            kids[idx] = child;
        }
        idx++;

        jp_skip_ws(p);
        if (p->pos < p->len && p->u[p->pos] == ',') {
            p->pos++;
            jp_skip_ws(p);
            if (p->pos < p->len && p->u[p->pos] == ']') {
                for (uint32_t i = 0; i < idx && kids; i++) jnode_free_meta(&kids[i]);
                free(kids);
                ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Trailing comma in JSON array");
                return false;
            }
            continue;
        }
        if (p->pos < p->len && p->u[p->pos] == ']') {
            p->pos++;
            break;
        }
        for (uint32_t i = 0; i < idx && kids; i++) jnode_free_meta(&kids[i]);
        free(kids);
        ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Expected ',' or ']' after array element in JSON");
        return false;
    }

    p->depth--;
    out->val = arr_v;
    if (p->track_src) {
        out->children = kids;
        out->prop_len = idx;
    }
    return true;
}

static bool jp_parse_object(JsonParser *p, JsonParseNode *out) {
    if (++p->depth > AJS_JSON_MAX_DEPTH) {
        ajs_throw_err_kind(p->vm, AJS_ERR_KIND_RANGE, "Maximum JSON nesting depth exceeded");
        return false;
    }
    p->pos++; /* '{' */
    AjsValue obj_v = ajs_new_object(p->vm);
    if (p->vm->has_exception) return false;
    ajs_temp_root_push(p->vm, obj_v);
    AjsObject *obj = (AjsObject *)ajs_val_to_gc(obj_v);

    jp_skip_ws(p);
    if (p->pos < p->len && p->u[p->pos] == '}') {
        p->pos++;
        p->depth--;
        out->val = obj_v;
        return true;
    }

    uint32_t cap = 0, cnt = 0;
    JsonParseNode *kids = NULL;
    AjsValue *keys = NULL;

    for (;;) {
        jp_skip_ws(p);
        if (p->pos >= p->len || p->u[p->pos] != '"') {
            for (uint32_t i = 0; i < cnt && kids; i++) jnode_free_meta(&kids[i]);
            free(kids); free(keys);
            ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Expected double-quoted property name in JSON");
            return false;
        }
        AjsString *kstr = jp_parse_string_raw(p, true);
        if (!kstr) {
            for (uint32_t i = 0; i < cnt && kids; i++) jnode_free_meta(&kids[i]);
            free(kids); free(keys);
            return false;
        }
        AjsValue kval = ajs_val_from_gc(kstr);
        ajs_temp_root_push(p->vm, kval);

        jp_skip_ws(p);
        if (p->pos >= p->len || p->u[p->pos] != ':') {
            for (uint32_t i = 0; i < cnt && kids; i++) jnode_free_meta(&kids[i]);
            free(kids); free(keys);
            ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Expected ':' after property name in JSON");
            return false;
        }
        p->pos++;
        jp_skip_ws(p);

        JsonParseNode child;
        memset(&child, 0, sizeof(child));
        if (!jp_parse_value(p, &child)) {
            for (uint32_t i = 0; i < cnt && kids; i++) jnode_free_meta(&kids[i]);
            free(kids); free(keys);
            return false;
        }
        /* CreateDataProperty(obj, key, val) — safe even for "__proto__"! */
        json_create_data_prop(p->vm, obj, kval, child.val);

        if (p->track_src) {
            /* Update existing duplicate key entry if present */
            bool updated = false;
            for (uint32_t i = 0; i < cnt; i++) {
                if (keys[i] == kval) {
                    jnode_free_meta(&kids[i]);
                    kids[i] = child;
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                if (cnt >= cap) {
                    uint32_t ncap = cap ? cap * 2u : 8u;
                    JsonParseNode *nk = (JsonParseNode *)realloc(kids, (size_t)ncap * sizeof(JsonParseNode));
                    AjsValue *nkeys = (AjsValue *)realloc(keys, (size_t)ncap * sizeof(AjsValue));
                    if (!nk || !nkeys) {
                        jnode_free_meta(&child);
                        if (nk) kids = nk;
                        if (nkeys) keys = nkeys;
                        for (uint32_t i = 0; i < cnt; i++) jnode_free_meta(&kids[i]);
                        free(kids); free(keys);
                        ajs_throw(p->vm, p->vm->oom_error);
                        return false;
                    }
                    kids = nk;
                    keys = nkeys;
                    cap = ncap;
                }
                keys[cnt] = kval;
                kids[cnt] = child;
                cnt++;
            }
        }

        jp_skip_ws(p);
        if (p->pos < p->len && p->u[p->pos] == ',') {
            p->pos++;
            jp_skip_ws(p);
            if (p->pos < p->len && p->u[p->pos] == '}') {
                for (uint32_t i = 0; i < cnt && kids; i++) jnode_free_meta(&kids[i]);
                free(kids); free(keys);
                ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Trailing comma in JSON object");
                return false;
            }
            continue;
        }
        if (p->pos < p->len && p->u[p->pos] == '}') {
            p->pos++;
            break;
        }
        for (uint32_t i = 0; i < cnt && kids; i++) jnode_free_meta(&kids[i]);
        free(kids); free(keys);
        ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Expected ',' or '}' after property value in JSON");
        return false;
    }

    p->depth--;
    out->val = obj_v;
    if (p->track_src) {
        out->children = kids;
        out->child_keys = keys;
        out->prop_len = cnt;
    }
    return true;
}

static bool jp_parse_value(JsonParser *p, JsonParseNode *out) {
    jp_skip_ws(p);
    if (p->pos >= p->len) {
        ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Unexpected end of JSON input");
        return false;
    }
    uint16_t c = p->u[p->pos];
    if (c == '"') {
        uint32_t start = p->pos;
        AjsString *s = jp_parse_string_raw(p, false);
        if (!s) return false;
        out->val = ajs_val_from_gc(s);
        ajs_temp_root_push(p->vm, out->val);
        if (p->track_src) {
            out->raw_src = ajs_str_from_utf16(p->vm, p->u + start, p->pos - start, false);
            if (out->raw_src) ajs_temp_root_push(p->vm, ajs_val_from_gc(out->raw_src));
        }
        return true;
    }
    if (c == '{') return jp_parse_object(p, out);
    if (c == '[') return jp_parse_array(p, out);
    if (c == 't') return jp_match_kw(p, "true",  AJS_VAL_TRUE,  out);
    if (c == 'f') return jp_match_kw(p, "false", AJS_VAL_FALSE, out);
    if (c == 'n') return jp_match_kw(p, "null",  AJS_VAL_NULL,  out);
    if (c == '-' || (c >= '0' && c <= '9')) return jp_parse_number(p, out);

    ajs_throw_err_kind(p->vm, AJS_ERR_KIND_SYNTAX, "Unexpected token in JSON at position %u", p->pos);
    return false;
}

/* ES2026 InternalizeJSONProperty(holder, name, reviver, parseNode) */
static AjsValue internalize_json_prop(AjsVm *vm, AjsValue holder, AjsValue name,
                                      AjsValue reviver, const JsonParseNode *meta) {
    size_t rs = ajs_temp_root_save(vm);
    AjsValue val = ajs_obj_get(vm, holder, name, holder);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, val);

    bool modified = true;
    if (ajs_is_object(vm, val)) {
        modified = true;
        AjsObject *vobj = (AjsObject *)ajs_val_to_gc(val);
        if (ajs_is_array(vm, val)) {
            uint64_t alen = 0;
            AjsValue lv = ajs_obj_get(vm, val, ajs_val_from_gc(vm->str_length), val);
            if (vm->has_exception || !ajs_to_length(vm, lv, &alen)) {
                ajs_temp_root_restore(vm, rs);
                return AJS_VAL_EXCEPTION;
            }
            for (uint64_t i = 0; i < alen; i++) {
                size_t ls = ajs_temp_root_save(vm);
                AjsString *istr = ajs_number_to_str(vm, (double)i, 10);
                AjsValue ikey = ajs_val_from_gc(ajs_str_intern(vm, istr));
                ajs_temp_root_push(vm, ikey);
                const JsonParseNode *cmeta = (meta && i < meta->prop_len) ? &meta->children[i] : NULL;
                AjsValue new_el = internalize_json_prop(vm, val, ikey, reviver, cmeta);
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                if (ajs_is_undefined(new_el)) {
                    ajs_obj_delete(vm, vobj, ikey, false);
                } else {
                    json_create_data_prop(vm, vobj, ikey, new_el);
                }
                if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                ajs_temp_root_restore(vm, ls);
            }
        } else {
            AjsValue *keys = NULL;
            uint32_t kcount = 0;
            if (!ajs_obj_own_keys(vm, vobj, true, true, false, &keys, &kcount)) {
                ajs_temp_root_restore(vm, rs);
                return AJS_VAL_EXCEPTION;
            }
            for (uint32_t i = 0; i < kcount; i++) ajs_temp_root_push(vm, keys[i]);
            for (uint32_t i = 0; i < kcount; i++) {
                size_t ls = ajs_temp_root_save(vm);
                AjsValue pkey = keys[i];
                const JsonParseNode *cmeta = NULL;
                if (meta && meta->child_keys) {
                    for (uint32_t j = 0; j < meta->prop_len; j++) {
                        if (meta->child_keys[j] == pkey) { cmeta = &meta->children[j]; break; }
                    }
                }
                AjsValue new_el = internalize_json_prop(vm, val, pkey, reviver, cmeta);
                if (vm->has_exception) { free(keys); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                if (ajs_is_undefined(new_el)) {
                    ajs_obj_delete(vm, vobj, pkey, false);
                } else {
                    json_create_data_prop(vm, vobj, pkey, new_el);
                }
                if (vm->has_exception) { free(keys); ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
                ajs_temp_root_restore(vm, ls);
            }
            free(keys);
        }
    } else if (meta && meta->raw_src && ajs_same_value(vm, val, meta->val)) {
        modified = false;
    }

    AjsValue ctx = ajs_new_object(vm);
    ajs_temp_root_push(vm, ctx);
    if (!modified && meta && meta->raw_src) {
        ajs_define_prop_str(vm, ctx, "source", ajs_val_from_gc(meta->raw_src), AJS_PROP_DEFAULT);
    }
    AjsValue cargs[3] = { name, val, ctx };
    AjsValue out = ajs_call_internal(vm, reviver, holder, 3, cargs);
    ajs_temp_root_restore(vm, rs);
    return out;
}

static AjsValue js_json_parse(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *jstr = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (vm->has_exception || !jstr) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(jstr));

    AjsValue reviver = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    bool has_rev = ajs_is_callable(vm, reviver);

    JsonParser p = { vm, jstr->units, jstr->len16, 0, 0, has_rev };
    JsonParseNode root;
    memset(&root, 0, sizeof(root));
    if (!jp_parse_value(&p, &root)) {
        jnode_free_meta(&root);
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    jp_skip_ws(&p);
    if (p.pos != p.len) {
        jnode_free_meta(&root);
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX,
                                  "Unexpected non-whitespace character after JSON at position %u", p.pos);
    }

    if (!has_rev) {
        jnode_free_meta(&root);
        ajs_temp_root_restore(vm, rs);
        return root.val;
    }

    AjsValue wrapper = ajs_new_object(vm);
    ajs_temp_root_push(vm, wrapper);
    AjsValue empty_k = ajs_val_from_gc(vm->str_empty);
    json_create_data_prop(vm, (AjsObject *)ajs_val_to_gc(wrapper), empty_k, root.val);
    AjsValue res = internalize_json_prop(vm, wrapper, empty_k, reviver, &root);
    jnode_free_meta(&root);
    ajs_temp_root_restore(vm, rs);
    return res;
}

/* ES2026 JSON.rawJSON(text) & JSON.isRawJSON(value) */
static AjsValue js_json_rawJSON(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *jstr = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (vm->has_exception || !jstr) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(jstr));

    if (jstr->len16 == 0 || is_json_ws(jstr->units[0]) || is_json_ws(jstr->units[jstr->len16 - 1u])) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX,
                                  "JSON.rawJSON text cannot be empty or have leading/trailing whitespace");
    }
    uint16_t first = jstr->units[0];
    if (first == '{' || first == '[') {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "JSON.rawJSON only accepts primitive JSON values");
    }

    JsonParser p = { vm, jstr->units, jstr->len16, 0, 0, false };
    JsonParseNode node;
    memset(&node, 0, sizeof(node));
    if (!jp_parse_value(&p, &node) || p.pos != p.len) {
        ajs_temp_root_restore(vm, rs);
        if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_SYNTAX, "Invalid JSON.rawJSON syntax");
        return AJS_VAL_EXCEPTION;
    }

    AjsObject *robj = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ORDINARY, AJS_VAL_NULL);
    if (!robj) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsValue rv = ajs_val_from_gc(robj);
    ajs_temp_root_push(vm, rv);

    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE |
              AJS_DESC_HAS_ENUMERABLE | AJS_DESC_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
    d.value = ajs_val_from_gc(jstr);
    ajs_obj_define_own_prop(vm, robj, ajs_val_from_gc(ajs_str_from_cstr(vm, "rawJSON", true)), &d, false);
    robj->extensible = false;
    robj->opaque_tag = AJS_OPAQUE_TAG_RAW_JSON;

    ajs_temp_root_restore(vm, rs);
    return rv;
}

static AjsValue js_json_isRawJSON(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)vm; (void)this_val; (void)ud;
    AjsValue v = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) return AJS_VAL_FALSE;
    return ajs_bool(((const AjsObject *)ajs_val_to_gc(v))->opaque_tag == AJS_OPAQUE_TAG_RAW_JSON);
}

/* ============================================================================
 * 3. JSON.stringify (Well-Formed UTF-16 Surrogates, Replacer, Space, RawJSON)
 * ============================================================================ */

typedef struct JsonSerState {
    AjsVm      *vm;
    JsonStrBuf  buf;
    AjsValue    replacer_fn;
    AjsValue   *prop_list;
    uint32_t    prop_list_len;
    char        gap[44];        /* Up to 10 UTF-16 units in UTF-8 + NUL */
    size_t      gap_len;
    AjsObject **stack;
    uint32_t    stack_len;
    uint32_t    stack_cap;
} JsonSerState;

static bool ser_push_obj(JsonSerState *st, AjsObject *obj) {
    for (uint32_t i = 0; i < st->stack_len; i++) {
        if (st->stack[i] == obj) {
            ajs_throw_err_kind(st->vm, AJS_ERR_KIND_TYPE, "Converting circular structure to JSON");
            return false;
        }
    }
    if (st->stack_len >= st->stack_cap) {
        uint32_t ncap = st->stack_cap ? st->stack_cap * 2u : 16u;
        AjsObject **ns = (AjsObject **)realloc(st->stack, (size_t)ncap * sizeof(AjsObject *));
        if (!ns) { ajs_throw(st->vm, st->vm->oom_error); return false; }
        st->stack = ns;
        st->stack_cap = ncap;
    }
    st->stack[st->stack_len++] = obj;
    return true;
}

static void quote_json_string(JsonStrBuf *b, const AjsString *s) {
    static const char hex[] = "0123456789abcdef";
    jbuf_putc(b, '"');
    uint32_t i = 0;
    while (i < s->len16) {
        uint16_t u = s->units[i];
        switch (u) {
            case '"':  jbuf_puts(b, "\\\"", 2); i++; continue;
            case '\\': jbuf_puts(b, "\\\\", 2); i++; continue;
            case '\b': jbuf_puts(b, "\\b", 2);  i++; continue;
            case '\f': jbuf_puts(b, "\\f", 2);  i++; continue;
            case '\n': jbuf_puts(b, "\\n", 2);  i++; continue;
            case '\r': jbuf_puts(b, "\\r", 2);  i++; continue;
            case '\t': jbuf_puts(b, "\\t", 2);  i++; continue;
            default: break;
        }
        if (u < 0x20u) {
            char esc[6] = { '\\', 'u', '0', '0', hex[(u >> 4) & 0xF], hex[u & 0xF] };
            jbuf_puts(b, esc, 6);
            i++;
            continue;
        }
        /* ES2019+ Well-Formed JSON.stringify: lone surrogates become \uXXXX! */
        if (u >= 0xD800u && u <= 0xDBFFu) {
            if (i + 1u < s->len16 && s->units[i + 1u] >= 0xDC00u && s->units[i + 1u] <= 0xDFFFu) {
                uint32_t cp = 0x10000u + (((uint32_t)(u - 0xD800u) << 10) | (uint32_t)(s->units[i + 1u] - 0xDC00u));
                char u8[4];
                size_t n = ajs_code_point_to_utf8(cp, u8);
                jbuf_puts(b, u8, n);
                i += 2u;
                continue;
            }
            char esc[6] = { '\\', 'u', hex[(u >> 12) & 0xF], hex[(u >> 8) & 0xF], hex[(u >> 4) & 0xF], hex[u & 0xF] };
            jbuf_puts(b, esc, 6);
            i++;
            continue;
        }
        if (u >= 0xDC00u && u <= 0xDFFFu) {
            char esc[6] = { '\\', 'u', hex[(u >> 12) & 0xF], hex[(u >> 8) & 0xF], hex[(u >> 4) & 0xF], hex[u & 0xF] };
            jbuf_puts(b, esc, 6);
            i++;
            continue;
        }
        char u8[4];
        size_t n = ajs_code_point_to_utf8((uint32_t)u, u8);
        jbuf_puts(b, u8, n);
        i++;
    }
    jbuf_putc(b, '"');
}

static void emit_indent(JsonStrBuf *b, const JsonSerState *st, uint32_t depth) {
    for (uint32_t i = 0; i < depth; i++) {
        jbuf_puts(b, st->gap, st->gap_len);
    }
}

/* Returns: 1 = wrote JSON text, 0 = serialized to `undefined` (omit), -1 = exception */
static int serialize_json_prop(JsonSerState *st, AjsValue holder, AjsValue key_str_val, uint32_t depth);

static int serialize_json_array(JsonSerState *st, AjsObject *arr_obj, uint32_t depth) {
    if (!ser_push_obj(st, arr_obj)) return -1;
    AjsVm *vm = st->vm;
    AjsValue arr_val = ajs_val_from_gc(arr_obj);

    AjsValue lv = ajs_obj_get(vm, arr_val, ajs_val_from_gc(vm->str_length), arr_val);
    uint64_t len = 0;
    if (vm->has_exception || !ajs_to_length(vm, lv, &len)) {
        st->stack_len--;
        return -1;
    }
    if (len == 0) {
        jbuf_puts(&st->buf, "[]", 2);
        st->stack_len--;
        return 1;
    }

    jbuf_putc(&st->buf, '[');
    uint32_t inner_depth = depth + 1u;
    for (uint64_t i = 0; i < len; i++) {
        if (i > 0) jbuf_putc(&st->buf, ',');
        if (st->gap_len > 0) {
            jbuf_putc(&st->buf, '\n');
            emit_indent(&st->buf, st, inner_depth);
        }
        size_t rs = ajs_temp_root_save(vm);
        AjsString *ks = ajs_str_intern(vm, ajs_number_to_str(vm, (double)i, 10));
        AjsValue kv = ajs_val_from_gc(ks);
        ajs_temp_root_push(vm, kv);
        int rc = serialize_json_prop(st, arr_val, kv, inner_depth);
        ajs_temp_root_restore(vm, rs);
        if (rc < 0) { st->stack_len--; return -1; }
        if (rc == 0) jbuf_puts(&st->buf, "null", 4);
    }
    if (st->gap_len > 0) {
        jbuf_putc(&st->buf, '\n');
        emit_indent(&st->buf, st, depth);
    }
    jbuf_putc(&st->buf, ']');
    st->stack_len--;
    return 1;
}

static int serialize_json_object(JsonSerState *st, AjsObject *obj, uint32_t depth) {
    if (!ser_push_obj(st, obj)) return -1;
    AjsVm *vm = st->vm;
    AjsValue obj_val = ajs_val_from_gc(obj);

    AjsValue *own_keys = NULL;
    uint32_t kcount = 0;
    const AjsValue *keys = st->prop_list;
    if (st->prop_list) {
        kcount = st->prop_list_len;
    } else {
        if (!ajs_obj_own_keys(vm, obj, true, true, false, &own_keys, &kcount)) {
            st->stack_len--;
            return -1;
        }
        keys = own_keys;
    }

    size_t rs = ajs_temp_root_save(vm);
    for (uint32_t i = 0; i < kcount && own_keys; i++) ajs_temp_root_push(vm, own_keys[i]);

    jbuf_putc(&st->buf, '{');
    size_t open_pos = st->buf.len;
    uint32_t emitted = 0;
    uint32_t inner_depth = depth + 1u;

    for (uint32_t i = 0; i < kcount; i++) {
        AjsValue kv = keys[i];
        size_t save_len = st->buf.len;
        if (emitted > 0) jbuf_putc(&st->buf, ',');
        if (st->gap_len > 0) {
            jbuf_putc(&st->buf, '\n');
            emit_indent(&st->buf, st, inner_depth);
        }
        quote_json_string(&st->buf, (const AjsString *)ajs_val_to_gc(kv));
        jbuf_putc(&st->buf, ':');
        if (st->gap_len > 0) jbuf_putc(&st->buf, ' ');

        int rc = serialize_json_prop(st, obj_val, kv, inner_depth);
        if (rc < 0) {
            free(own_keys);
            ajs_temp_root_restore(vm, rs);
            st->stack_len--;
            return -1;
        }
        if (rc == 0) {
            /* Property value serialized to undefined — rewind buffer! */
            st->buf.len = save_len;
            if (st->buf.data) st->buf.data[save_len] = '\0';
        } else {
            emitted++;
        }
    }

    free(own_keys);
    ajs_temp_root_restore(vm, rs);

    if (emitted == 0) {
        st->buf.len = open_pos;
        jbuf_putc(&st->buf, '}');
    } else {
        if (st->gap_len > 0) {
            jbuf_putc(&st->buf, '\n');
            emit_indent(&st->buf, st, depth);
        }
        jbuf_putc(&st->buf, '}');
    }
    st->stack_len--;
    return 1;
}

static int serialize_json_prop(JsonSerState *st, AjsValue holder, AjsValue key_str_val, uint32_t depth) {
    AjsVm *vm = st->vm;
    size_t rs = ajs_temp_root_save(vm);

    AjsValue val = ajs_obj_get(vm, holder, key_str_val, holder);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return -1; }
    ajs_temp_root_push(vm, val);

    /* 1. Check `.toJSON(key)` on Objects and BigInts */
    if (ajs_is_object(vm, val) || ajs_val_is_gc_kind(val, AJS_GC_BIGINT)) {
        AjsValue to_json = AJS_VAL_UNDEFINED;
        if (ajs_val_is_gc_kind(val, AJS_GC_BIGINT)) {
            to_json = ajs_obj_get(vm, vm->active_realm->bigint_proto, ajs_val_from_gc(vm->str_toJSON), val);
        } else {
            to_json = ajs_obj_get(vm, val, ajs_val_from_gc(vm->str_toJSON), val);
        }
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return -1; }
        if (ajs_is_callable(vm, to_json)) {
            ajs_temp_root_push(vm, to_json);
            val = ajs_call_internal(vm, to_json, val, 1, &key_str_val);
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return -1; }
            ajs_temp_root_push(vm, val);
        }
    }

    /* 2. Apply callable replacer(key, val) */
    if (!ajs_is_undefined(st->replacer_fn)) {
        AjsValue rargs[2] = { key_str_val, val };
        val = ajs_call_internal(vm, st->replacer_fn, holder, 2, rargs);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return -1; }
        ajs_temp_root_push(vm, val);
    }

    /* 3. Unwrap boxed primitive objects & check RawJSON */
    if (ajs_val_is_gc_kind(val, AJS_GC_OBJECT)) {
        AjsObject *o = (AjsObject *)ajs_val_to_gc(val);
        if (o->opaque_tag == AJS_OPAQUE_TAG_RAW_JSON) {
            AjsValue raw_v = ajs_get_prop_str(vm, val, "rawJSON");
            if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return -1; }
            if (ajs_val_is_gc_kind(raw_v, AJS_GC_STRING)) {
                const AjsString *rs_str = (const AjsString *)ajs_val_to_gc(raw_v);
                jbuf_puts(&st->buf, rs_str->utf8, rs_str->len8);
                ajs_temp_root_restore(vm, rs);
                return 1;
            }
        }
        if (o->kind == AJS_OBJ_NUMBER_OBJ) {
            double d = 0.0;
            if (!ajs_to_number(vm, val, &d)) { ajs_temp_root_restore(vm, rs); return -1; }
            val = ajs_val_number(d);
        } else if (o->kind == AJS_OBJ_STRING_OBJ) {
            AjsString *s = ajs_to_js_string(vm, val);
            if (vm->has_exception || !s) { ajs_temp_root_restore(vm, rs); return -1; }
            val = ajs_val_from_gc(s);
            ajs_temp_root_push(vm, val);
        } else if (o->kind == AJS_OBJ_BOOLEAN_OBJ || o->kind == AJS_OBJ_BIGINT_OBJ) {
            val = o->u.prim_val;
        }
    }

    if (val == AJS_VAL_NULL)  { jbuf_puts(&st->buf, "null", 4);  ajs_temp_root_restore(vm, rs); return 1; }
    if (val == AJS_VAL_TRUE)  { jbuf_puts(&st->buf, "true", 4);  ajs_temp_root_restore(vm, rs); return 1; }
    if (val == AJS_VAL_FALSE) { jbuf_puts(&st->buf, "false", 5); ajs_temp_root_restore(vm, rs); return 1; }

    if (ajs_val_is_gc_kind(val, AJS_GC_STRING)) {
        quote_json_string(&st->buf, (const AjsString *)ajs_val_to_gc(val));
        ajs_temp_root_restore(vm, rs);
        return 1;
    }
    if (ajs_val_is_number(val)) {
        double d = ajs_val_to_number_unchecked(val);
        if (!isfinite(d)) {
            jbuf_puts(&st->buf, "null", 4);
        } else {
            AjsString *ns = ajs_number_to_str(vm, d, 10);
            jbuf_puts(&st->buf, ns->utf8, ns->len8);
        }
        ajs_temp_root_restore(vm, rs);
        return 1;
    }
    if (ajs_val_is_gc_kind(val, AJS_GC_BIGINT)) {
        ajs_temp_root_restore(vm, rs);
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Do not know how to serialize a BigInt");
        return -1;
    }
    if (ajs_is_undefined(val) || ajs_val_is_gc_kind(val, AJS_GC_SYMBOL) || ajs_is_callable(vm, val)) {
        ajs_temp_root_restore(vm, rs);
        return 0;
    }
    if (ajs_val_is_gc_kind(val, AJS_GC_OBJECT)) {
        AjsObject *obj = (AjsObject *)ajs_val_to_gc(val);
        int rc = ajs_is_array(vm, val) ? serialize_json_array(st, obj, depth)
                                       : serialize_json_object(st, obj, depth);
        ajs_temp_root_restore(vm, rs);
        return rc;
    }

    ajs_temp_root_restore(vm, rs);
    return 0;
}

static int run_stringify(AjsVm *vm, AjsValue value, AjsValue replacer, AjsValue space, JsonStrBuf *out_buf) {
    size_t rs = ajs_temp_root_save(vm);
    JsonSerState st;
    memset(&st, 0, sizeof(st));
    st.vm = vm;
    st.replacer_fn = AJS_VAL_UNDEFINED;

    /* 1. Process replacer (callable or Array PropertyList) */
    if (ajs_is_object(vm, replacer)) {
        if (ajs_is_callable(vm, replacer)) {
            st.replacer_fn = replacer;
        } else if (ajs_is_array(vm, replacer)) {
            AjsValue lv = ajs_obj_get(vm, replacer, ajs_val_from_gc(vm->str_length), replacer);
            uint64_t rlen = 0;
            if (vm->has_exception || !ajs_to_length(vm, lv, &rlen)) {
                ajs_temp_root_restore(vm, rs);
                return -1;
            }
            st.prop_list = (AjsValue *)malloc(((size_t)rlen + 1u) * sizeof(AjsValue));
            if (!st.prop_list) {
                ajs_temp_root_restore(vm, rs);
                ajs_throw(vm, vm->oom_error);
                return -1;
            }
            for (uint64_t i = 0; i < rlen; i++) {
                AjsString *iks = ajs_str_intern(vm, ajs_number_to_str(vm, (double)i, 10));
                AjsValue v = ajs_obj_get(vm, replacer, ajs_val_from_gc(iks), replacer);
                if (vm->has_exception) { free(st.prop_list); ajs_temp_root_restore(vm, rs); return -1; }
                AjsString *item = NULL;
                if (ajs_val_is_gc_kind(v, AJS_GC_STRING)) {
                    item = ajs_str_intern(vm, (AjsString *)ajs_val_to_gc(v));
                } else if (ajs_val_is_number(v)) {
                    item = ajs_str_intern(vm, ajs_number_to_str(vm, ajs_val_to_number_unchecked(v), 10));
                } else if (ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) {
                    AjsObjKind ok = ((AjsObject *)ajs_val_to_gc(v))->kind;
                    if (ok == AJS_OBJ_STRING_OBJ || ok == AJS_OBJ_NUMBER_OBJ) {
                        AjsString *ts = ajs_to_js_string(vm, v);
                        if (vm->has_exception || !ts) { free(st.prop_list); ajs_temp_root_restore(vm, rs); return -1; }
                        item = ajs_str_intern(vm, ts);
                    }
                }
                if (item) {
                    AjsValue iv = ajs_val_from_gc(item);
                    bool dup = false;
                    for (uint32_t j = 0; j < st.prop_list_len; j++) {
                        if (st.prop_list[j] == iv) { dup = true; break; }
                    }
                    if (!dup) {
                        ajs_temp_root_push(vm, iv);
                        st.prop_list[st.prop_list_len++] = iv;
                    }
                }
            }
        }
    }

    /* 2. Process `space` */
    if (ajs_val_is_gc_kind(space, AJS_GC_OBJECT)) {
        AjsObject *so = (AjsObject *)ajs_val_to_gc(space);
        if (so->kind == AJS_OBJ_NUMBER_OBJ) {
            double sd = 0.0;
            if (!ajs_to_number(vm, space, &sd)) { free(st.prop_list); ajs_temp_root_restore(vm, rs); return -1; }
            space = ajs_val_number(sd);
        } else if (so->kind == AJS_OBJ_STRING_OBJ) {
            AjsString *ss = ajs_to_js_string(vm, space);
            if (vm->has_exception || !ss) { free(st.prop_list); ajs_temp_root_restore(vm, rs); return -1; }
            space = ajs_val_from_gc(ss);
            ajs_temp_root_push(vm, space);
        }
    }
    if (ajs_val_is_number(space)) {
        double mv = ajs_double_to_integer_or_inf(ajs_val_to_number_unchecked(space));
        int spc = mv < 1.0 ? 0 : (mv > 10.0 ? 10 : (int)mv);
        for (int i = 0; i < spc; i++) st.gap[i] = ' ';
        st.gap[spc] = '\0';
        st.gap_len = (size_t)spc;
    } else if (ajs_val_is_gc_kind(space, AJS_GC_STRING)) {
        const AjsString *ss = (const AjsString *)ajs_val_to_gc(space);
        uint32_t take = ss->len16 < 10u ? ss->len16 : 10u;
        AjsString *sub = ajs_str_from_utf16(vm, ss->units, take, false);
        if (sub && sub->len8 < sizeof(st.gap)) {
            memcpy(st.gap, sub->utf8, sub->len8);
            st.gap[sub->len8] = '\0';
            st.gap_len = sub->len8;
        }
    }

    /* 3. Wrapper `{ "": value }` */
    AjsValue wrapper = ajs_new_object(vm);
    ajs_temp_root_push(vm, wrapper);
    AjsValue empty_k = ajs_val_from_gc(vm->str_empty);
    json_create_data_prop(vm, (AjsObject *)ajs_val_to_gc(wrapper), empty_k, value);

    int rc = serialize_json_prop(&st, wrapper, empty_k, 0);
    free(st.prop_list);
    free(st.stack);
    ajs_temp_root_restore(vm, rs);

    if (st.buf.oom) {
        free(st.buf.data);
        ajs_throw(vm, vm->oom_error);
        return -1;
    }
    if (rc <= 0) {
        free(st.buf.data);
        return rc;
    }
    if (!st.buf.data) jbuf_put_cstr(&st.buf, "");
    *out_buf = st.buf;
    return 1;
}

static AjsValue js_json_stringify(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue val  = argc > 0 ? argv[0] : AJS_VAL_UNDEFINED;
    AjsValue repl = argc > 1 ? argv[1] : AJS_VAL_UNDEFINED;
    AjsValue spc  = argc > 2 ? argv[2] : AJS_VAL_UNDEFINED;

    JsonStrBuf buf;
    memset(&buf, 0, sizeof(buf));
    int rc = run_stringify(vm, val, repl, spc, &buf);
    if (rc < 0) return AJS_VAL_EXCEPTION;
    if (rc == 0) return AJS_VAL_UNDEFINED;

    AjsString *res = ajs_str_from_utf8(vm, buf.data, buf.len, false);
    free(buf.data);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}

/* ============================================================================
 * 4. Public C Convenience APIs & Realm Initialization
 * ============================================================================ */

AjsValue ajs_json_parse(AjsVm *vm, const char *utf8_json, size_t byte_len) {
    if (!vm || !utf8_json) return AJS_VAL_UNDEFINED;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s = ajs_str_from_utf8(vm, utf8_json, byte_len, false);
    if (!s) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsValue sv = ajs_val_from_gc(s);
    ajs_temp_root_push(vm, sv);
    AjsValue res = js_json_parse(vm, AJS_VAL_UNDEFINED, 1, &sv, NULL);
    ajs_temp_root_restore(vm, rs);
    if (!vm->has_exception) ajs_handle_record(vm, res);
    return res;
}

char *ajs_json_stringify(AjsVm *vm, AjsValue val, size_t *out_byte_len) {
    if (out_byte_len) *out_byte_len = 0;
    if (!vm) return NULL;
    JsonStrBuf buf;
    memset(&buf, 0, sizeof(buf));
    int rc = run_stringify(vm, val, AJS_VAL_UNDEFINED, AJS_VAL_UNDEFINED, &buf);
    if (rc <= 0) return NULL;
    if (out_byte_len) *out_byte_len = buf.len;
    return buf.data;
}

void ajs_init_json_builtins(AjsVm *vm, AjsRealm *r) {
    size_t rs = ajs_temp_root_save(vm);
    AjsValue json_obj = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    ajs_temp_root_push(vm, json_obj);

    ajs_def_native_method(vm, r, json_obj, "parse", js_json_parse, 2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, json_obj, "stringify", js_json_stringify, 3, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, json_obj, "rawJSON", js_json_rawJSON, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, json_obj, "isRawJSON", js_json_isRawJSON, 1, AJS_METHOD_FLAGS, 0);

    AjsPropDesc td;
    memset(&td, 0, sizeof(td));
    td.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
               AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    td.value = ajs_val_from_gc(ajs_str_from_cstr(vm, "JSON", true));
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(json_obj),
                            ajs_val_from_gc(vm->well_known_syms[AJS_SYM_TO_STRING_TAG]), &td, false);

    ajs_define_prop_str(vm, r->global_obj, "JSON", json_obj, AJS_METHOD_FLAGS);
    ajs_temp_root_restore(vm, rs);
}
