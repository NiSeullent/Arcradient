/*
 * ArcradientJS 1.0 (Clean-Room Pure C11 ECMAScript 2026 Virtual Machine)
 * js/builtins/ajs_intl.c - Self-Contained ECMA-402 Internationalization Suite (Zero ICU)
 *
 * Implements:
 * - Intl.getCanonicalLocales, Intl.supportedValuesOf (BCP 47 validation & canonicalization)
 * - Intl.Locale (subtag extraction, -u- extensions, maximize, minimize)
 * - Intl.Collator + String.prototype.localeCompare
 * - Intl.NumberFormat + Number/BigInt.prototype.toLocaleString (incl. BigInt > 64-bit, compact, range)
 * - Intl.DateTimeFormat + Date.prototype.toLocale{,Date,Time}String (IANA TZ + DST + ISO offsets)
 * - Intl.PluralRules (cardinal/ordinal, select, selectRange)
 * - Intl.RelativeTimeFormat (always/auto, format, formatToParts)
 * - Intl.Segmenter + %SegmentsPrototype% + %SegmentIteratorPrototype% (UAX #29 grapheme/word/sentence)
 * - Intl.DisplayNames & Intl.ListFormat
 */
#include "../vm/ajs_internal.h"
#include <time.h>

#define AJS_METHOD_FLAGS (AJS_PROP_WRITABLE | AJS_PROP_CONFIGURABLE)

/* Built-in constructor magic values (< 0 so `ajs_construct_internal` passes `new.target`) */
#define INTL_MAGIC_LOCALE       (-201)
#define INTL_MAGIC_COLLATOR     (-202)
#define INTL_MAGIC_NUMFMT       (-203)
#define INTL_MAGIC_DTFMT        (-204)
#define INTL_MAGIC_PLURAL       (-205)
#define INTL_MAGIC_RELTIME      (-206)
#define INTL_MAGIC_SEGMENTER    (-207)
#define INTL_MAGIC_DISPNAMES    (-208)
#define INTL_MAGIC_LISTFMT      (-209)

/* ============================================================================
 * 1. Core Allocation, Prototype & Option Parsing Helpers
 * ============================================================================ */

static int intl_ascii_tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
}

static int intl_ascii_toupper(int c) {
    return (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
}

static bool intl_is_alpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool intl_is_digit(int c) {
    return (c >= '0' && c <= '9');
}

static bool intl_is_alnum(int c) {
    return intl_is_alpha(c) || intl_is_digit(c);
}

static int intl_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = intl_ascii_tolower((unsigned char)*a++);
        int cb = intl_ascii_tolower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return intl_ascii_tolower((unsigned char)*a) - intl_ascii_tolower((unsigned char)*b);
}

static bool intl_lang_is(const char *locale, const char *lang2) {
    size_t n = strlen(lang2);
    if (strncmp(locale, lang2, n) != 0) return false;
    return locale[n] == '\0' || locale[n] == '-';
}

static AjsValue intl_make_ctor(AjsVm *vm, AjsRealm *r, AjsNativeFn fn, const char *name,
                               int length, int32_t magic, AjsValue proto_obj) {
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, proto_obj);
    AjsObject *f = ajs_obj_new(vm, r, AJS_OBJ_FUNCTION, r->function_proto);
    if (!f) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    AjsValue fv = ajs_val_from_gc(f);
    ajs_temp_root_push(vm, fv);

    f->u.fn.fn_kind = AJS_FN_NATIVE;
    f->u.fn.is_constructor = true;
    f->u.fn.strict = true;
    f->u.fn.native_fn = fn;
    f->u.fn.native_magic = magic;

    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
              AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    d.value = ajs_val_number((double)length);
    ajs_obj_define_own_prop(vm, f, ajs_val_from_gc(vm->str_length), &d, false);
    d.value = ajs_val_from_gc(ajs_str_from_cstr(vm, name, true));
    ajs_obj_define_own_prop(vm, f, ajs_val_from_gc(vm->str_name), &d, false);

    if (ajs_val_is_gc_kind(proto_obj, AJS_GC_OBJECT)) {
        AjsPropDesc pd;
        memset(&pd, 0, sizeof(pd));
        pd.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE;
        pd.value = proto_obj;
        ajs_obj_define_own_prop(vm, f, ajs_val_from_gc(vm->str_prototype), &pd, false);

        AjsPropDesc cd;
        memset(&cd, 0, sizeof(cd));
        cd.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_WRITABLE |
                   AJS_DESC_HAS_ENUMERABLE | AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
        cd.value = fv;
        ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(proto_obj),
                                ajs_val_from_gc(vm->str_constructor), &cd, false);
    }

    ajs_temp_root_restore(vm, rs);
    return fv;
}

static void intl_def_to_string_tag(AjsVm *vm, AjsValue target, const char *tag) {
    if (!ajs_val_is_gc_kind(target, AJS_GC_OBJECT)) return;
    AjsString *s = ajs_str_from_cstr(vm, tag, true);
    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
              AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    d.value = ajs_val_from_gc(s);
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(target),
                            ajs_val_from_gc(vm->well_known_syms[AJS_SYM_TO_STRING_TAG]), &d, false);
}

static AjsValue intl_proto_from_new_target(AjsVm *vm, AjsValue nt, AjsValue fallback_proto) {
    if (ajs_is_undefined(nt) || !ajs_val_is_gc_kind(nt, AJS_GC_OBJECT)) return fallback_proto;
    AjsValue p = ajs_obj_get(vm, nt, ajs_val_from_gc(vm->str_prototype), nt);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    if (ajs_val_is_gc_kind(p, AJS_GC_OBJECT)) return p;
    return fallback_proto;
}

static AjsObject *intl_alloc_instance(AjsVm *vm, AjsRealm *r, AjsIntlKind kind, AjsValue proto) {
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, proto);
    AjsObject *obj = ajs_obj_new(vm, r, AJS_OBJ_INTL, proto);
    if (!obj) { ajs_temp_root_restore(vm, rs); return NULL; }
    AjsIntlData *d = (AjsIntlData *)calloc(1, sizeof(AjsIntlData));
    if (!d) {
        ajs_temp_root_restore(vm, rs);
        ajs_throw(vm, vm->oom_error);
        return NULL;
    }
    d->kind = kind;
    d->bound_fn = AJS_VAL_UNDEFINED;
    d->seg_input = AJS_VAL_UNDEFINED;
    d->segmenter_obj = AJS_VAL_UNDEFINED;
    obj->u.intl = d;
    ajs_temp_root_restore(vm, rs);
    return obj;
}

static AjsIntlData *intl_require_kind(AjsVm *vm, AjsValue v, AjsIntlKind kind, const char *type_name) {
    if (!ajs_val_is_gc_kind(v, AJS_GC_OBJECT)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Method called on incompatible receiver (expected %s)", type_name);
        return NULL;
    }
    AjsObject *o = (AjsObject *)ajs_val_to_gc(v);
    if (o->kind != AJS_OBJ_INTL || !o->u.intl || o->u.intl->kind != kind) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Method called on incompatible receiver (expected %s)", type_name);
        return NULL;
    }
    return o->u.intl;
}

static AjsValue intl_make_bound_fn(AjsVm *vm, AjsRealm *r, AjsNativeFn fn, int length, AjsValue bound_this) {
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, bound_this);
    AjsObject *target = ajs_obj_new(vm, r, AJS_OBJ_FUNCTION, r->function_proto);
    if (!target) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(target));
    target->u.fn.fn_kind = AJS_FN_NATIVE;
    target->u.fn.strict = true;
    target->u.fn.native_fn = fn;

    AjsObject *bfn = ajs_obj_new(vm, r, AJS_OBJ_FUNCTION, r->function_proto);
    if (!bfn) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(bfn));
    bfn->u.fn.fn_kind = AJS_FN_BOUND;
    bfn->u.fn.strict = true;
    bfn->u.fn.bound_target = ajs_val_from_gc(target);
    bfn->u.fn.bound_this = bound_this;
    bfn->u.fn.bound_argc = 0;

    AjsPropDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
              AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    d.value = ajs_val_number((double)length);
    ajs_obj_define_own_prop(vm, bfn, ajs_val_from_gc(vm->str_length), &d, false);
    d.value = ajs_val_from_gc(vm->str_empty);
    ajs_obj_define_own_prop(vm, bfn, ajs_val_from_gc(vm->str_name), &d, false);

    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(bfn);
}

/* Coerce options per ECMA-402 GetOptionsObject: undefined -> undefined, Object -> Object, else TypeError! */
static bool intl_coerce_options_obj(AjsVm *vm, AjsValue opts, AjsValue *out_obj) {
    if (ajs_is_undefined(opts)) {
        *out_obj = AJS_VAL_UNDEFINED;
        return true;
    }
    if (ajs_is_null(opts)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Cannot convert null to options object");
        return false;
    }
    if (ajs_val_is_gc_kind(opts, AJS_GC_OBJECT)) {
        *out_obj = opts;
        return true;
    }
    AjsObject *boxed = ajs_to_object(vm, opts);
    if (!boxed || vm->has_exception) return false;
    *out_obj = ajs_val_from_gc(boxed);
    return true;
}

static bool intl_get_opt_str(AjsVm *vm, AjsValue opts, const char *prop,
                             const char *const *allowed, int allowed_cnt,
                             const char *def_val, char *out_buf, size_t out_cap, bool *out_present) {
    if (out_present) *out_present = false;
    if (ajs_is_undefined(opts)) {
        snprintf(out_buf, out_cap, "%s", def_val ? def_val : "");
        return true;
    }
    AjsValue v = ajs_get_prop_str(vm, opts, prop);
    if (vm->has_exception) return false;
    if (ajs_is_undefined(v)) {
        snprintf(out_buf, out_cap, "%s", def_val ? def_val : "");
        return true;
    }
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, v);
    AjsString *s = ajs_to_js_string(vm, v);
    ajs_temp_root_restore(vm, rs);
    if (vm->has_exception || !s) return false;
    if (out_present) *out_present = true;

    if (allowed && allowed_cnt > 0) {
        bool ok = false;
        for (int i = 0; i < allowed_cnt; i++) {
            if (strcmp(s->utf8, allowed[i]) == 0) { ok = true; break; }
        }
        if (!ok) {
            ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Value '%s' out of range for option '%s'", s->utf8, prop);
            return false;
        }
    }
    snprintf(out_buf, out_cap, "%s", s->utf8);
    return true;
}

static bool intl_get_opt_bool(AjsVm *vm, AjsValue opts, const char *prop,
                              bool def_val, bool *out_val, bool *out_present) {
    if (out_present) *out_present = false;
    if (ajs_is_undefined(opts)) { *out_val = def_val; return true; }
    AjsValue v = ajs_get_prop_str(vm, opts, prop);
    if (vm->has_exception) return false;
    if (ajs_is_undefined(v)) { *out_val = def_val; return true; }
    if (out_present) *out_present = true;
    *out_val = ajs_to_bool(vm, v);
    return true;
}

static bool intl_get_opt_int(AjsVm *vm, AjsValue opts, const char *prop,
                             int min_v, int max_v, int def_v, int *out_val, bool *out_present) {
    if (out_present) *out_present = false;
    if (ajs_is_undefined(opts)) { *out_val = def_v; return true; }
    AjsValue v = ajs_get_prop_str(vm, opts, prop);
    if (vm->has_exception) return false;
    if (ajs_is_undefined(v)) { *out_val = def_v; return true; }
    double d = 0.0;
    if (!ajs_to_number(vm, v, &d)) return false;
    if (isnan(d) || d < (double)min_v || d > (double)max_v) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Property '%s' value is out of range [%d..%d]", prop, min_v, max_v);
        return false;
    }
    if (out_present) *out_present = true;
    *out_val = (int)floor(d);
    return true;
}

static bool intl_push_part_ex(AjsVm *vm, AjsValue arr, uint32_t *io_idx,
                              const char *type_str, const char *val_str, const char *unit_or_null) {
    size_t rs = ajs_temp_root_save(vm);
    AjsValue obj = ajs_new_object(vm);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
    ajs_temp_root_push(vm, obj);

    AjsValue tv = ajs_val_from_gc(ajs_str_from_cstr(vm, type_str, true));
    ajs_define_prop_str(vm, obj, "type", tv, AJS_PROP_DEFAULT);

    AjsString *vs = ajs_str_from_cstr(vm, val_str, false);
    if (vs) {
        ajs_temp_root_push(vm, ajs_val_from_gc(vs));
        ajs_define_prop_str(vm, obj, "value", ajs_val_from_gc(vs), AJS_PROP_DEFAULT);
    }
    if (unit_or_null) {
        AjsString *us = ajs_str_from_cstr(vm, unit_or_null, true);
        if (us) ajs_define_prop_str(vm, obj, "unit", ajs_val_from_gc(us), AJS_PROP_DEFAULT);
    }

    ajs_set_prop_index(vm, arr, (*io_idx)++, obj);
    ajs_temp_root_restore(vm, rs);
    return !vm->has_exception;
}

/* ============================================================================
 * 2. BCP 47 Validation, Canonicalization & Likely Subtags (UTS #35 / ECMA-402)
 * ============================================================================ */

typedef struct IntlParsedTag {
    char language[12];
    char script[8];
    char region[8];
    char variants[4][12];
    int  variant_count;
    /* Unicode -u- keywords */
    char u_ca[32];
    char u_co[32];
    char u_hc[32];
    char u_kf[32];
    char u_kn[32];
    bool has_u_kn;
    char u_nu[32];
    /* Other -u- attributes/keys & other singleton extensions & -x- privateuse */
    char other_u[64];
    char other_ext[64];
    char priv_x[64];
} IntlParsedTag;

typedef struct IntlUKeyVal {
    char key[4];
    char val[32];
} IntlUKeyVal;

static int cmp_u_kv(const void *pa, const void *pb) {
    const IntlUKeyVal *a = (const IntlUKeyVal *)pa;
    const IntlUKeyVal *b = (const IntlUKeyVal *)pb;
    return strcmp(a->key, b->key);
}

static int cmp_cstr8(const void *pa, const void *pb) {
    return strcmp((const char *)pa, (const char *)pb);
}

static void canonicalize_lang_region_aliases(IntlParsedTag *pt) {
    static const char *const k_lang_map[][2] = {
        {"iw", "he"}, {"in", "id"}, {"ji", "yi"}, {"jw", "jv"},
        {"mo", "ro"}, {"tl", "fil"}, {"aam", "aas"}, {"eng", "en"},
        {"kor", "ko"}, {"jpn", "ja"}, {"zho", "zh"}, {"fra", "fr"},
        {"fre", "fr"}, {"deu", "de"}, {"ger", "de"}, {"spa", "es"},
        {"rus", "ru"}, {"ita", "it"}, {"por", "pt"}, {"ara", "ar"}
    };
    for (size_t i = 0; i < sizeof(k_lang_map) / sizeof(k_lang_map[0]); i++) {
        if (strcmp(pt->language, k_lang_map[i][0]) == 0) {
            snprintf(pt->language, sizeof(pt->language), "%s", k_lang_map[i][1]);
            break;
        }
    }
    static const char *const k_reg_map[][2] = {
        {"UK", "GB"}, {"SU", "RU"}, {"BU", "MM"}, {"DD", "DE"}, {"ZR", "CD"}, {"TP", "TL"}
    };
    for (size_t i = 0; i < sizeof(k_reg_map) / sizeof(k_reg_map[0]); i++) {
        if (strcmp(pt->region, k_reg_map[i][0]) == 0) {
            snprintf(pt->region, sizeof(pt->region), "%s", k_reg_map[i][1]);
            break;
        }
    }
}

static void format_parsed_tag(const IntlParsedTag *pt, char *out_tag, size_t out_cap, char *out_base, size_t base_cap) {
    char base[64];
    size_t pos = 0;
    pos += (size_t)snprintf(base + pos, sizeof(base) - pos, "%s", pt->language[0] ? pt->language : "und");
    if (pt->script[0] && pos + 8 < sizeof(base)) {
        pos += (size_t)snprintf(base + pos, sizeof(base) - pos, "-%s", pt->script);
    }
    if (pt->region[0] && pos + 8 < sizeof(base)) {
        pos += (size_t)snprintf(base + pos, sizeof(base) - pos, "-%s", pt->region);
    }
    /* Sort variants alphabetically per UTS #35 canonicalization */
    if (pt->variant_count > 0) {
        char sorted_v[4][12];
        for (int i = 0; i < pt->variant_count; i++) {
            snprintf(sorted_v[i], sizeof(sorted_v[i]), "%s", pt->variants[i]);
        }
        qsort(sorted_v, (size_t)pt->variant_count, sizeof(sorted_v[0]), cmp_cstr8);
        for (int i = 0; i < pt->variant_count; i++) {
            if (pos + 12 < sizeof(base)) {
                pos += (size_t)snprintf(base + pos, sizeof(base) - pos, "-%s", sorted_v[i]);
            }
        }
    }
    if (out_base && base_cap > 0) {
        size_t bl = strlen(base);
        if (bl >= base_cap) bl = base_cap - 1u;
        ajs_memcpy(out_base, base, bl);
        out_base[bl] = '\0';
    }

    IntlUKeyVal u_kvs[8];
    int u_cnt = 0;
    if (pt->u_ca[0]) { snprintf(u_kvs[u_cnt].key, 4, "ca"); snprintf(u_kvs[u_cnt].val, 32, "%s", pt->u_ca); u_cnt++; }
    if (pt->u_co[0]) { snprintf(u_kvs[u_cnt].key, 4, "co"); snprintf(u_kvs[u_cnt].val, 32, "%s", pt->u_co); u_cnt++; }
    if (pt->u_hc[0]) { snprintf(u_kvs[u_cnt].key, 4, "hc"); snprintf(u_kvs[u_cnt].val, 32, "%s", pt->u_hc); u_cnt++; }
    if (pt->u_kf[0]) { snprintf(u_kvs[u_cnt].key, 4, "kf"); snprintf(u_kvs[u_cnt].val, 32, "%s", pt->u_kf); u_cnt++; }
    if (pt->has_u_kn) {
        snprintf(u_kvs[u_cnt].key, 4, "kn");
        snprintf(u_kvs[u_cnt].val, 32, "%s", (strcmp(pt->u_kn, "true") == 0) ? "" : pt->u_kn);
        u_cnt++;
    }
    if (pt->u_nu[0]) { snprintf(u_kvs[u_cnt].key, 4, "nu"); snprintf(u_kvs[u_cnt].val, 32, "%s", pt->u_nu); u_cnt++; }
    if (u_cnt > 1) qsort(u_kvs, (size_t)u_cnt, sizeof(u_kvs[0]), cmp_u_kv);

    char full[128];
    size_t fp = (size_t)snprintf(full, sizeof(full), "%s", base);
    if (pt->other_u[0] || u_cnt > 0) {
        if (fp + 4 < sizeof(full)) fp += (size_t)snprintf(full + fp, sizeof(full) - fp, "-u");
        if (pt->other_u[0] && fp + strlen(pt->other_u) + 2 < sizeof(full)) {
            fp += (size_t)snprintf(full + fp, sizeof(full) - fp, "-%s", pt->other_u);
        }
        for (int i = 0; i < u_cnt; i++) {
            if (u_kvs[i].val[0]) {
                if (fp + strlen(u_kvs[i].val) + 5 < sizeof(full))
                    fp += (size_t)snprintf(full + fp, sizeof(full) - fp, "-%s-%s", u_kvs[i].key, u_kvs[i].val);
            } else {
                if (fp + 4 < sizeof(full))
                    fp += (size_t)snprintf(full + fp, sizeof(full) - fp, "-%s", u_kvs[i].key);
            }
        }
    }
    if (pt->other_ext[0] && fp + strlen(pt->other_ext) + 2 < sizeof(full)) {
        fp += (size_t)snprintf(full + fp, sizeof(full) - fp, "-%s", pt->other_ext);
    }
    if (pt->priv_x[0] && fp + strlen(pt->priv_x) + 4 < sizeof(full)) {
        fp += (size_t)snprintf(full + fp, sizeof(full) - fp, "-x-%s", pt->priv_x);
    }
    if (out_tag && out_cap > 0) {
        size_t fl = strlen(full);
        if (fl >= out_cap) fl = out_cap - 1u;
        ajs_memcpy(out_tag, full, fl);
        out_tag[fl] = '\0';
    }
}

static bool parse_and_canonicalize_bcp47(const char *raw, IntlParsedTag *out_pt,
                                         char *out_canon, size_t canon_cap) {
    memset(out_pt, 0, sizeof(*out_pt));
    if (!raw || !raw[0]) return false;

    size_t rlen = strlen(raw);
    if (rlen > 256 || raw[0] == '-' || raw[rlen - 1] == '-') return false;

    /* Split on '-' into subtags; reject empty subtags or >8 chars or non-ASCII-alnum */
    char sub[32][12];
    int scnt = 0;
    size_t i = 0;
    while (i < rlen) {
        if (scnt >= 32) return false;
        size_t start = i;
        while (i < rlen && raw[i] != '-') {
            if (!intl_is_alnum((unsigned char)raw[i])) return false;
            i++;
        }
        size_t slen = i - start;
        if (slen == 0 || slen > 8) return false;
        for (size_t k = 0; k < slen; k++) {
            sub[scnt][k] = (char)intl_ascii_tolower((unsigned char)raw[start + k]);
        }
        sub[scnt][slen] = '\0';
        scnt++;
        if (i < rlen && raw[i] == '-') i++;
    }
    if (scnt == 0) return false;

    int idx = 0;
    /* Private-use root tag `x-...` is NOT a valid Unicode locale identifier in ECMA-402 */
    if (strlen(sub[0]) == 1) return false;

    /* 1. Language: 2..3 alpha or 5..8 alpha (4 alpha is reserved!) */
    size_t l0 = strlen(sub[0]);
    if (!((l0 >= 2 && l0 <= 3) || (l0 >= 5 && l0 <= 8))) return false;
    for (size_t k = 0; k < l0; k++) {
        if (!intl_is_alpha((unsigned char)sub[0][k])) return false;
    }
    snprintf(out_pt->language, sizeof(out_pt->language), "%s", sub[0]);
    idx++;

    /* Optional extlang: single 3-alpha subtag after 2..3-alpha language (e.g., zh-cmn -> cmn) */
    if (idx < scnt && (l0 == 2 || l0 == 3) && strlen(sub[idx]) == 3 &&
        intl_is_alpha((unsigned char)sub[idx][0]) &&
        intl_is_alpha((unsigned char)sub[idx][1]) &&
        intl_is_alpha((unsigned char)sub[idx][2])) {
        if (strcmp(out_pt->language, "zh") == 0 && strcmp(sub[idx], "cmn") == 0) {
            snprintf(out_pt->language, sizeof(out_pt->language), "zh");
        } else {
            snprintf(out_pt->language, sizeof(out_pt->language), "%s", sub[idx]);
        }
        idx++;
        /* Reject >1 extlang subtags per UTS #35 unicode_language_subtag */
        if (idx < scnt && strlen(sub[idx]) == 3 &&
            intl_is_alpha((unsigned char)sub[idx][0]) &&
            intl_is_alpha((unsigned char)sub[idx][1]) &&
            intl_is_alpha((unsigned char)sub[idx][2])) {
            return false;
        }
    }

    /* 2. Optional Script: 4 alpha -> TitleCase */
    if (idx < scnt && strlen(sub[idx]) == 4 &&
        intl_is_alpha((unsigned char)sub[idx][0]) &&
        intl_is_alpha((unsigned char)sub[idx][1]) &&
        intl_is_alpha((unsigned char)sub[idx][2]) &&
        intl_is_alpha((unsigned char)sub[idx][3])) {
        out_pt->script[0] = (char)intl_ascii_toupper((unsigned char)sub[idx][0]);
        out_pt->script[1] = sub[idx][1];
        out_pt->script[2] = sub[idx][2];
        out_pt->script[3] = sub[idx][3];
        out_pt->script[4] = '\0';
        idx++;
    }

    /* 3. Optional Region: 2 alpha -> UPPERCASE, or 3 digits */
    if (idx < scnt) {
        size_t rl = strlen(sub[idx]);
        if (rl == 2 && intl_is_alpha((unsigned char)sub[idx][0]) && intl_is_alpha((unsigned char)sub[idx][1])) {
            out_pt->region[0] = (char)intl_ascii_toupper((unsigned char)sub[idx][0]);
            out_pt->region[1] = (char)intl_ascii_toupper((unsigned char)sub[idx][1]);
            out_pt->region[2] = '\0';
            idx++;
        } else if (rl == 3 && intl_is_digit((unsigned char)sub[idx][0]) &&
                   intl_is_digit((unsigned char)sub[idx][1]) && intl_is_digit((unsigned char)sub[idx][2])) {
            out_pt->region[0] = sub[idx][0];
            out_pt->region[1] = sub[idx][1];
            out_pt->region[2] = sub[idx][2];
            out_pt->region[3] = '\0';
            idx++;
        }
    }

    /* 4. Optional Variants: (5..8 alnum) or (digit + 3 alnum), no duplicates */
    while (idx < scnt) {
        size_t vl = strlen(sub[idx]);
        bool is_var = (vl >= 5 && vl <= 8) || (vl == 4 && intl_is_digit((unsigned char)sub[idx][0]));
        if (!is_var) break;
        for (int v = 0; v < out_pt->variant_count; v++) {
            if (strcmp(out_pt->variants[v], sub[idx]) == 0) return false; /* duplicate variant */
        }
        if (out_pt->variant_count >= 4) return false;
        snprintf(out_pt->variants[out_pt->variant_count++], 12, "%s", sub[idx]);
        idx++;
    }

    /* 5. Extensions & Privateuse */
    bool seen_singleton[128];
    memset(seen_singleton, 0, sizeof(seen_singleton));

    while (idx < scnt) {
        if (strlen(sub[idx]) != 1) return false;
        char sing = sub[idx][0];
        if (sing == 'x') {
            idx++;
            if (idx >= scnt) return false; /* trailing `-x` without subtags */
            size_t xp = 0;
            while (idx < scnt) {
                if (xp > 0 && xp + 1 < sizeof(out_pt->priv_x)) out_pt->priv_x[xp++] = '-';
                xp += (size_t)snprintf(out_pt->priv_x + xp, sizeof(out_pt->priv_x) - xp, "%s", sub[idx]);
                idx++;
            }
            break;
        }
        if (seen_singleton[(unsigned char)sing]) return false; /* duplicate singleton */
        seen_singleton[(unsigned char)sing] = true;
        idx++;
        if (idx >= scnt || strlen(sub[idx]) == 1) return false; /* empty extension */

        if (sing == 'u') {
            /* Attributes (3..8 alnum) before first 2-char key */
            char attrs[4][12];
            int attr_cnt = 0;
            while (idx < scnt && strlen(sub[idx]) >= 3) {
                bool dup = false;
                for (int a = 0; a < attr_cnt; a++) {
                    if (strcmp(attrs[a], sub[idx]) == 0) { dup = true; break; }
                }
                if (!dup && attr_cnt < 4) snprintf(attrs[attr_cnt++], 12, "%s", sub[idx]);
                idx++;
            }
            if (attr_cnt > 1) qsort(attrs, (size_t)attr_cnt, sizeof(attrs[0]), cmp_cstr8);
            size_t op = 0;
            for (int a = 0; a < attr_cnt; a++) {
                if (op > 0 && op + 1 < sizeof(out_pt->other_u)) out_pt->other_u[op++] = '-';
                op += (size_t)snprintf(out_pt->other_u + op, sizeof(out_pt->other_u) - op, "%s", attrs[a]);
            }
            /* Keywords: 2-char key (`alnum + alpha`) followed by 0+ type subtags (3..8 alnum) */
            while (idx < scnt && strlen(sub[idx]) == 2) {
                if (!intl_is_alnum((unsigned char)sub[idx][0]) || !intl_is_alpha((unsigned char)sub[idx][1])) {
                    return false;
                }
                char ukey[4];
                ukey[0] = sub[idx][0];
                ukey[1] = sub[idx][1];
                ukey[2] = '\0';
                idx++;
                char uval[32];
                uval[0] = '\0';
                size_t vp = 0;
                while (idx < scnt && strlen(sub[idx]) >= 3) {
                    if (vp > 0 && vp + 1 < sizeof(uval)) uval[vp++] = '-';
                    vp += (size_t)snprintf(uval + vp, sizeof(uval) - vp, "%s", sub[idx]);
                    idx++;
                }
                if (strcmp(ukey, "ca") == 0) {
                    if (!out_pt->u_ca[0]) {
                        snprintf(out_pt->u_ca, sizeof(out_pt->u_ca), "%s",
                                 strcmp(uval, "gregorian") == 0 ? "gregory" : uval);
                    }
                } else if (strcmp(ukey, "co") == 0) {
                    if (!out_pt->u_co[0]) snprintf(out_pt->u_co, sizeof(out_pt->u_co), "%s", uval);
                } else if (strcmp(ukey, "hc") == 0) {
                    if (!out_pt->u_hc[0]) snprintf(out_pt->u_hc, sizeof(out_pt->u_hc), "%s", uval);
                } else if (strcmp(ukey, "kf") == 0) {
                    if (!out_pt->u_kf[0]) snprintf(out_pt->u_kf, sizeof(out_pt->u_kf), "%s", uval);
                } else if (strcmp(ukey, "kn") == 0) {
                    if (!out_pt->has_u_kn) {
                        out_pt->has_u_kn = true;
                        snprintf(out_pt->u_kn, sizeof(out_pt->u_kn), "%s", uval);
                    }
                } else if (strcmp(ukey, "nu") == 0) {
                    if (!out_pt->u_nu[0]) snprintf(out_pt->u_nu, sizeof(out_pt->u_nu), "%s", uval);
                } else {
                    if (op > 0 && op + 1 < sizeof(out_pt->other_u)) out_pt->other_u[op++] = '-';
                    if (uval[0]) op += (size_t)snprintf(out_pt->other_u + op, sizeof(out_pt->other_u) - op, "%s-%s", ukey, uval);
                    else op += (size_t)snprintf(out_pt->other_u + op, sizeof(out_pt->other_u) - op, "%s", ukey);
                }
            }
        } else {
            size_t ep = strlen(out_pt->other_ext);
            if (ep > 0 && ep + 1 < sizeof(out_pt->other_ext)) out_pt->other_ext[ep++] = '-';
            ep += (size_t)snprintf(out_pt->other_ext + ep, sizeof(out_pt->other_ext) - ep, "%c", sing);
            while (idx < scnt && strlen(sub[idx]) >= 2) {
                if (ep + strlen(sub[idx]) + 2 < sizeof(out_pt->other_ext)) {
                    ep += (size_t)snprintf(out_pt->other_ext + ep, sizeof(out_pt->other_ext) - ep, "-%s", sub[idx]);
                }
                idx++;
            }
        }
    }

    canonicalize_lang_region_aliases(out_pt);
    format_parsed_tag(out_pt, out_canon, canon_cap, NULL, 0);
    return true;
}

typedef struct IntlLocaleList {
    char items[16][64];
    int  count;
} IntlLocaleList;

static bool intl_canonicalize_locale_list(AjsVm *vm, AjsValue locales, IntlLocaleList *out) {
    memset(out, 0, sizeof(*out));
    if (ajs_is_undefined(locales)) return true;

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, locales);

    if (ajs_val_is_gc_kind(locales, AJS_GC_STRING)) {
        const AjsString *s = (const AjsString *)ajs_val_to_gc(locales);
        IntlParsedTag pt;
        if (!parse_and_canonicalize_bcp47(s->utf8, &pt, out->items[0], sizeof(out->items[0]))) {
            ajs_temp_root_restore(vm, rs);
            ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Incorrect locale information provided: '%s'", s->utf8);
            return false;
        }
        out->count = 1;
        ajs_temp_root_restore(vm, rs);
        return true;
    }

    if (ajs_val_is_gc_kind(locales, AJS_GC_OBJECT)) {
        AjsObject *lo = (AjsObject *)ajs_val_to_gc(locales);
        if (lo->kind == AJS_OBJ_INTL && lo->u.intl && lo->u.intl->kind == AJS_INTL_LOCALE) {
            snprintf(out->items[0], sizeof(out->items[0]), "%s", lo->u.intl->locale);
            out->count = 1;
            ajs_temp_root_restore(vm, rs);
            return true;
        }
    }

    AjsObject *obj = ajs_to_object(vm, locales);
    if (!obj || vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
    AjsValue ov = ajs_val_from_gc(obj);
    ajs_temp_root_push(vm, ov);

    AjsValue len_v = ajs_get_prop_str(vm, ov, "length");
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
    uint64_t len = 0;
    if (!ajs_to_length(vm, len_v, &len)) { ajs_temp_root_restore(vm, rs); return false; }
    if (len > 1024) len = 1024;

    for (uint64_t k = 0; k < len; k++) {
        char kbuf[24];
        snprintf(kbuf, sizeof(kbuf), "%llu", (unsigned long long)k);
        AjsString *ks = ajs_str_from_cstr(vm, kbuf, true);
        bool has = false;
        if (!ajs_obj_has_prop(vm, obj, ajs_val_from_gc(ks), &has)) {
            ajs_temp_root_restore(vm, rs);
            return false;
        }
        if (!has) continue;
        AjsValue kv = ajs_obj_get(vm, ov, ajs_val_from_gc(ks), ov);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }

        if (!ajs_val_is_gc_kind(kv, AJS_GC_STRING) && !ajs_val_is_gc_kind(kv, AJS_GC_OBJECT)) {
            ajs_temp_root_restore(vm, rs);
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Locale list element must be String or Object");
            return false;
        }

        const char *tag_cstr = NULL;
        AjsString *tmp_s = NULL;
        if (ajs_val_is_gc_kind(kv, AJS_GC_OBJECT)) {
            AjsObject *ko = (AjsObject *)ajs_val_to_gc(kv);
            if (ko->kind == AJS_OBJ_INTL && ko->u.intl && ko->u.intl->kind == AJS_INTL_LOCALE) {
                tag_cstr = ko->u.intl->locale;
            }
        }
        if (!tag_cstr) {
            ajs_temp_root_push(vm, kv);
            tmp_s = ajs_to_js_string(vm, kv);
            if (vm->has_exception || !tmp_s) { ajs_temp_root_restore(vm, rs); return false; }
            tag_cstr = tmp_s->utf8;
        }

        IntlParsedTag pt;
        char canon[64];
        if (!parse_and_canonicalize_bcp47(tag_cstr, &pt, canon, sizeof(canon))) {
            ajs_temp_root_restore(vm, rs);
            ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid language tag: '%s'", tag_cstr);
            return false;
        }

        bool dup = false;
        for (int m = 0; m < out->count; m++) {
            if (strcmp(out->items[m], canon) == 0) { dup = true; break; }
        }
        if (!dup && out->count < 16) {
            snprintf(out->items[out->count++], sizeof(out->items[0]), "%s", canon);
        }
    }

    ajs_temp_root_restore(vm, rs);
    return true;
}

static bool intl_resolve_locale(AjsVm *vm, AjsValue locales, AjsValue opts,
                                IntlParsedTag *out_pt, char *out_base_or_full, size_t cap) {
    static const char *const k_matchers[2] = { "lookup", "best fit" };
    char matcher[16];
    if (!intl_get_opt_str(vm, opts, "localeMatcher", k_matchers, 2, "best fit", matcher, sizeof(matcher), NULL)) {
        return false;
    }
    IntlLocaleList list;
    if (!intl_canonicalize_locale_list(vm, locales, &list)) return false;

    const char *chosen = (list.count > 0) ? list.items[0] : "en-US";
    if (!parse_and_canonicalize_bcp47(chosen, out_pt, out_base_or_full, cap)) {
        parse_and_canonicalize_bcp47("en-US", out_pt, out_base_or_full, cap);
    }
    if (strcmp(out_pt->language, "und") == 0) {
        snprintf(out_pt->language, sizeof(out_pt->language), "en");
        if (!out_pt->region[0]) snprintf(out_pt->region, sizeof(out_pt->region), "US");
        format_parsed_tag(out_pt, out_base_or_full, cap, NULL, 0);
    }
    return true;
}

static AjsValue js_intl_supported_locales_of(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    IntlLocaleList list;
    if (!intl_canonicalize_locale_list(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &list)) {
        return AJS_VAL_EXCEPTION;
    }
    if (argc > 1 && !ajs_is_undefined(argv[1])) {
        AjsValue opts = AJS_VAL_UNDEFINED;
        if (!intl_coerce_options_obj(vm, argv[1], &opts)) return AJS_VAL_EXCEPTION;
        static const char *const k_matchers[2] = { "lookup", "best fit" };
        char m[16];
        if (!intl_get_opt_str(vm, opts, "localeMatcher", k_matchers, 2, "best fit", m, sizeof(m), NULL)) {
            return AJS_VAL_EXCEPTION;
        }
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue arr = ajs_new_array(vm, (uint32_t)list.count);
    ajs_temp_root_push(vm, arr);
    for (int i = 0; i < list.count; i++) {
        AjsString *s = ajs_str_from_cstr(vm, list.items[i], false);
        ajs_set_prop_index(vm, arr, (uint32_t)i, ajs_val_from_gc(s));
    }
    ajs_temp_root_restore(vm, rs);
    return arr;
}

static AjsValue js_intl_getCanonicalLocales(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    IntlLocaleList list;
    if (!intl_canonicalize_locale_list(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &list)) {
        return AJS_VAL_EXCEPTION;
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue arr = ajs_new_array(vm, (uint32_t)list.count);
    ajs_temp_root_push(vm, arr);
    for (int i = 0; i < list.count; i++) {
        AjsString *s = ajs_str_from_cstr(vm, list.items[i], false);
        ajs_set_prop_index(vm, arr, (uint32_t)i, ajs_val_from_gc(s));
    }
    ajs_temp_root_restore(vm, rs);
    return arr;
}

static const char *const k_sv_calendars[] = {
    "buddhist", "chinese", "dangi", "gregory", "hebrew", "indian",
    "islamic", "islamic-civil", "iso8601", "japanese", "persian", "roc"
};
static const char *const k_sv_collations[] = {
    "big5han", "compat", "dict", "direct", "ducet", "emoji", "eor",
    "gb2312", "phonebk", "phonetic", "pinyin", "searchjl", "stroke", "trad", "unihan", "zhuyin"
};
static const char *const k_sv_currencies[] = {
    "AUD", "BRL", "CAD", "CHF", "CNY", "EUR", "GBP", "HKD", "INR", "JPY",
    "KRW", "MXN", "NOK", "NZD", "PLN", "RUB", "SEK", "SGD", "THB", "TRY",
    "TWD", "USD", "VND", "ZAR"
};
static const char *const k_sv_numsys[] = {
    "arab", "arabext", "bali", "beng", "deva", "fullwide", "gujr", "guru",
    "hanidec", "khmr", "knda", "laoo", "latn", "limb", "mlym", "mong",
    "mymr", "orya", "tamldec", "telu", "thai", "tibt"
};
static const char *const k_sv_timezones[] = {
    "America/Chicago", "America/Denver", "America/Los_Angeles", "America/New_York",
    "America/Sao_Paulo", "America/Toronto", "Asia/Bangkok", "Asia/Dubai",
    "Asia/Hong_Kong", "Asia/Kolkata", "Asia/Seoul", "Asia/Shanghai",
    "Asia/Singapore", "Asia/Tokyo", "Australia/Sydney", "Europe/Berlin",
    "Europe/London", "Europe/Moscow", "Europe/Paris", "Pacific/Auckland", "UTC"
};
static const char *const k_sv_units[] = {
    "acre", "bit", "byte", "celsius", "centimeter", "day", "degree",
    "fahrenheit", "fluid-ounce", "foot", "gallon", "gigabit", "gigabyte",
    "gram", "hectare", "hour", "inch", "kilobit", "kilobyte", "kilogram",
    "kilometer", "liter", "megabit", "megabyte", "meter", "microsecond",
    "mile", "mile-scandinavian", "milliliter", "millimeter", "millisecond",
    "minute", "month", "nanosecond", "ounce", "percent", "petabyte",
    "pound", "second", "stone", "terabit", "terabyte", "week", "yard", "year"
};

static bool intl_is_sanctioned_single_unit(const char *u) {
    for (size_t i = 0; i < sizeof(k_sv_units) / sizeof(k_sv_units[0]); i++) {
        if (strcmp(u, k_sv_units[i]) == 0) return true;
    }
    return false;
}

static bool intl_is_valid_unit_id(const char *u) {
    if (intl_is_sanctioned_single_unit(u)) return true;
    const char *per = strstr(u, "-per-");
    if (!per) return false;
    char num[32], den[32];
    size_t nlen = (size_t)(per - u);
    if (nlen == 0 || nlen >= sizeof(num)) return false;
    ajs_memcpy(num, u, nlen);
    num[nlen] = '\0';
    snprintf(den, sizeof(den), "%s", per + 5);
    return intl_is_sanctioned_single_unit(num) && intl_is_sanctioned_single_unit(den);
}

static AjsValue js_intl_supportedValuesOf(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsString *ks = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (vm->has_exception || !ks) return AJS_VAL_EXCEPTION;

    const char *const *tbl = NULL;
    size_t cnt = 0;
    if (strcmp(ks->utf8, "calendar") == 0) {
        tbl = k_sv_calendars; cnt = sizeof(k_sv_calendars) / sizeof(k_sv_calendars[0]);
    } else if (strcmp(ks->utf8, "collation") == 0) {
        tbl = k_sv_collations; cnt = sizeof(k_sv_collations) / sizeof(k_sv_collations[0]);
    } else if (strcmp(ks->utf8, "currency") == 0) {
        tbl = k_sv_currencies; cnt = sizeof(k_sv_currencies) / sizeof(k_sv_currencies[0]);
    } else if (strcmp(ks->utf8, "numberingSystem") == 0) {
        tbl = k_sv_numsys; cnt = sizeof(k_sv_numsys) / sizeof(k_sv_numsys[0]);
    } else if (strcmp(ks->utf8, "timeZone") == 0) {
        tbl = k_sv_timezones; cnt = sizeof(k_sv_timezones) / sizeof(k_sv_timezones[0]);
    } else if (strcmp(ks->utf8, "unit") == 0) {
        tbl = k_sv_units; cnt = sizeof(k_sv_units) / sizeof(k_sv_units[0]);
    } else {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid key '%s' for Intl.supportedValuesOf", ks->utf8);
    }

    size_t rs = ajs_temp_root_save(vm);
    AjsValue arr = ajs_new_array(vm, (uint32_t)cnt);
    ajs_temp_root_push(vm, arr);
    for (size_t i = 0; i < cnt; i++) {
        AjsString *s = ajs_str_from_cstr(vm, tbl[i], true);
        ajs_set_prop_index(vm, arr, (uint32_t)i, ajs_val_from_gc(s));
    }
    ajs_temp_root_restore(vm, rs);
    return arr;
}

/* ============================================================================
 * 3. Intl.Locale (ECMA-402 §14)
 * ============================================================================ */

static AjsObject *intl_build_locale_from_pt(AjsVm *vm, AjsRealm *r, const IntlParsedTag *pt, AjsValue proto) {
    AjsObject *obj = intl_alloc_instance(vm, r, AJS_INTL_LOCALE, proto);
    if (!obj) return NULL;
    AjsIntlData *d = obj->u.intl;
    /* For AJS_INTL_LOCALE, store baseName in d->time_zone (64-byte unused slot) */
    format_parsed_tag(pt, d->locale, sizeof(d->locale), d->time_zone, sizeof(d->time_zone));
    snprintf(d->language, sizeof(d->language), "%s", pt->language);
    snprintf(d->script, sizeof(d->script), "%s", pt->script);
    snprintf(d->region, sizeof(d->region), "%s", pt->region);
    snprintf(d->calendar, sizeof(d->calendar), "%s", pt->u_ca);
    snprintf(d->collation, sizeof(d->collation), "%s", pt->u_co);
    snprintf(d->numbering_system, sizeof(d->numbering_system), "%s", pt->u_nu);
    snprintf(d->hour_cycle, sizeof(d->hour_cycle), "%.15s", pt->u_hc);
    snprintf(d->case_first, sizeof(d->case_first), "%.15s", pt->u_kf);
    d->numeric = pt->has_u_kn && (pt->u_kn[0] == '\0' || strcmp(pt->u_kn, "true") == 0);
    return obj;
}

static bool intl_is_valid_unicode_type_seq(const char *s) {
    if (!s || !s[0]) return false;
    size_t i = 0, len = strlen(s);
    while (i < len) {
        size_t st = i;
        while (i < len && s[i] != '-') {
            if (!intl_is_alnum((unsigned char)s[i])) return false;
            i++;
        }
        size_t sl = i - st;
        if (sl < 3 || sl > 8) return false;
        if (i < len && s[i] == '-') {
            i++;
            if (i == len) return false;
        }
    }
    return true;
}

static AjsValue js_intl_locale_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (ajs_is_undefined(nt)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Constructor Intl.Locale requires 'new'");
    }
    AjsValue tag_arg = (argc > 0) ? argv[0] : AJS_VAL_UNDEFINED;
    if (!ajs_val_is_gc_kind(tag_arg, AJS_GC_STRING) && !ajs_val_is_gc_kind(tag_arg, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "First argument to Intl.Locale must be a string or Object");
    }

    size_t rs = ajs_temp_root_save(vm);
    AjsValue proto = intl_proto_from_new_target(vm, nt, vm->active_realm->intl_locale_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, proto);

    const char *raw_tag = NULL;
    AjsString *tag_s = NULL;
    if (ajs_val_is_gc_kind(tag_arg, AJS_GC_OBJECT)) {
        AjsObject *to = (AjsObject *)ajs_val_to_gc(tag_arg);
        if (to->kind == AJS_OBJ_INTL && to->u.intl && to->u.intl->kind == AJS_INTL_LOCALE) {
            raw_tag = to->u.intl->locale;
        }
    }
    if (!raw_tag) {
        tag_s = ajs_to_js_string(vm, tag_arg);
        if (vm->has_exception || !tag_s) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
        ajs_temp_root_push(vm, ajs_val_from_gc(tag_s));
        raw_tag = tag_s->utf8;
    }

    IntlParsedTag pt;
    char canon[64];
    if (!parse_and_canonicalize_bcp47(raw_tag, &pt, canon, sizeof(canon))) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid language tag '%s'", raw_tag);
    }

    if (argc > 1 && !ajs_is_undefined(argv[1])) {
        AjsValue opts = AJS_VAL_UNDEFINED;
        if (!intl_coerce_options_obj(vm, argv[1], &opts)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
        ajs_temp_root_push(vm, opts);

        char opt_buf[32];
        bool pres = false;
        if (!intl_get_opt_str(vm, opts, "language", NULL, 0, "", opt_buf, sizeof(opt_buf), &pres)) {
            ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
        }
        if (pres) {
            IntlParsedTag lpt; char lc[64];
            if (!parse_and_canonicalize_bcp47(opt_buf, &lpt, lc, sizeof(lc)) || lpt.script[0] || lpt.region[0] || lpt.variant_count > 0) {
                ajs_temp_root_restore(vm, rs);
                return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid language subtag '%s'", opt_buf);
            }
            snprintf(pt.language, sizeof(pt.language), "%s", lpt.language);
        }

        if (!intl_get_opt_str(vm, opts, "script", NULL, 0, "", opt_buf, sizeof(opt_buf), &pres)) {
            ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
        }
        if (pres) {
            if (strlen(opt_buf) != 4 || !intl_is_alpha((unsigned char)opt_buf[0]) ||
                !intl_is_alpha((unsigned char)opt_buf[1]) || !intl_is_alpha((unsigned char)opt_buf[2]) ||
                !intl_is_alpha((unsigned char)opt_buf[3])) {
                ajs_temp_root_restore(vm, rs);
                return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid script subtag '%s'", opt_buf);
            }
            pt.script[0] = (char)intl_ascii_toupper((unsigned char)opt_buf[0]);
            pt.script[1] = (char)intl_ascii_tolower((unsigned char)opt_buf[1]);
            pt.script[2] = (char)intl_ascii_tolower((unsigned char)opt_buf[2]);
            pt.script[3] = (char)intl_ascii_tolower((unsigned char)opt_buf[3]);
            pt.script[4] = '\0';
        }

        if (!intl_get_opt_str(vm, opts, "region", NULL, 0, "", opt_buf, sizeof(opt_buf), &pres)) {
            ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
        }
        if (pres) {
            size_t rl = strlen(opt_buf);
            bool ok_r = (rl == 2 && intl_is_alpha((unsigned char)opt_buf[0]) && intl_is_alpha((unsigned char)opt_buf[1])) ||
                        (rl == 3 && intl_is_digit((unsigned char)opt_buf[0]) && intl_is_digit((unsigned char)opt_buf[1]) && intl_is_digit((unsigned char)opt_buf[2]));
            if (!ok_r) {
                ajs_temp_root_restore(vm, rs);
                return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid region subtag '%s'", opt_buf);
            }
            for (size_t k = 0; k < rl; k++) pt.region[k] = (char)intl_ascii_toupper((unsigned char)opt_buf[k]);
            pt.region[rl] = '\0';
        }

        if (!intl_get_opt_str(vm, opts, "calendar", NULL, 0, "", opt_buf, sizeof(opt_buf), &pres)) {
            ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
        }
        if (pres) {
            if (!intl_is_valid_unicode_type_seq(opt_buf)) {
                ajs_temp_root_restore(vm, rs);
                return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid calendar '%s'", opt_buf);
            }
            for (size_t k = 0; opt_buf[k]; k++) opt_buf[k] = (char)intl_ascii_tolower((unsigned char)opt_buf[k]);
            snprintf(pt.u_ca, sizeof(pt.u_ca), "%s", strcmp(opt_buf, "gregorian") == 0 ? "gregory" : opt_buf);
        }

        if (!intl_get_opt_str(vm, opts, "collation", NULL, 0, "", opt_buf, sizeof(opt_buf), &pres)) {
            ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
        }
        if (pres) {
            if (!intl_is_valid_unicode_type_seq(opt_buf)) {
                ajs_temp_root_restore(vm, rs);
                return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid collation '%s'", opt_buf);
            }
            for (size_t k = 0; opt_buf[k]; k++) opt_buf[k] = (char)intl_ascii_tolower((unsigned char)opt_buf[k]);
            snprintf(pt.u_co, sizeof(pt.u_co), "%s", opt_buf);
        }

        static const char *const k_hc[4] = { "h11", "h12", "h23", "h24" };
        if (!intl_get_opt_str(vm, opts, "hourCycle", k_hc, 4, "", opt_buf, sizeof(opt_buf), &pres)) {
            ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
        }
        if (pres) snprintf(pt.u_hc, sizeof(pt.u_hc), "%s", opt_buf);

        static const char *const k_kf[3] = { "upper", "lower", "false" };
        if (!intl_get_opt_str(vm, opts, "caseFirst", k_kf, 3, "", opt_buf, sizeof(opt_buf), &pres)) {
            ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
        }
        if (pres) snprintf(pt.u_kf, sizeof(pt.u_kf), "%s", opt_buf);

        bool num_b = false;
        if (!intl_get_opt_bool(vm, opts, "numeric", false, &num_b, &pres)) {
            ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
        }
        if (pres) {
            pt.has_u_kn = true;
            snprintf(pt.u_kn, sizeof(pt.u_kn), "%s", num_b ? "true" : "false");
        }

        if (!intl_get_opt_str(vm, opts, "numberingSystem", NULL, 0, "", opt_buf, sizeof(opt_buf), &pres)) {
            ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
        }
        if (pres) {
            if (!intl_is_valid_unicode_type_seq(opt_buf)) {
                ajs_temp_root_restore(vm, rs);
                return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid numberingSystem '%s'", opt_buf);
            }
            for (size_t k = 0; opt_buf[k]; k++) opt_buf[k] = (char)intl_ascii_tolower((unsigned char)opt_buf[k]);
            snprintf(pt.u_nu, sizeof(pt.u_nu), "%s", opt_buf);
        }

        canonicalize_lang_region_aliases(&pt);
    }

    AjsObject *loc = intl_build_locale_from_pt(vm, vm->active_realm, &pt, proto);
    ajs_temp_root_restore(vm, rs);
    return loc ? ajs_val_from_gc(loc) : AJS_VAL_EXCEPTION;
}

static const struct { const char *lang; const char *script; const char *region; } k_likely_subtags[] = {
    {"en", "Latn", "US"}, {"ko", "Kore", "KR"}, {"ja", "Jpan", "JP"},
    {"zh", "Hans", "CN"}, {"fr", "Latn", "FR"}, {"de", "Latn", "DE"},
    {"es", "Latn", "ES"}, {"pt", "Latn", "BR"}, {"it", "Latn", "IT"},
    {"ru", "Cyrl", "RU"}, {"ar", "Arab", "EG"}, {"hi", "Deva", "IN"},
    {"th", "Thai", "TH"}, {"vi", "Latn", "VN"}, {"id", "Latn", "ID"},
    {"nl", "Latn", "NL"}, {"pl", "Latn", "PL"}, {"tr", "Latn", "TR"},
    {"sv", "Latn", "SE"}, {"und", "Latn", "US"}
};

static void intl_maximize_pt(IntlParsedTag *pt) {
    if (strcmp(pt->language, "und") == 0) snprintf(pt->language, sizeof(pt->language), "en");
    if (strcmp(pt->language, "zh") == 0) {
        if (!pt->script[0]) {
            if (strcmp(pt->region, "TW") == 0 || strcmp(pt->region, "HK") == 0 || strcmp(pt->region, "MO") == 0)
                snprintf(pt->script, sizeof(pt->script), "Hant");
            else
                snprintf(pt->script, sizeof(pt->script), "Hans");
        }
        if (!pt->region[0]) {
            snprintf(pt->region, sizeof(pt->region), strcmp(pt->script, "Hant") == 0 ? "TW" : "CN");
        }
        return;
    }
    for (size_t i = 0; i < sizeof(k_likely_subtags) / sizeof(k_likely_subtags[0]); i++) {
        if (strcmp(pt->language, k_likely_subtags[i].lang) == 0) {
            if (!pt->script[0]) snprintf(pt->script, sizeof(pt->script), "%s", k_likely_subtags[i].script);
            if (!pt->region[0]) snprintf(pt->region, sizeof(pt->region), "%s", k_likely_subtags[i].region);
            return;
        }
    }
    if (!pt->script[0]) snprintf(pt->script, sizeof(pt->script), "Latn");
    if (!pt->region[0]) snprintf(pt->region, sizeof(pt->region), "US");
}

static void intl_minimize_pt(IntlParsedTag *pt) {
    IntlParsedTag max_full = *pt;
    intl_maximize_pt(&max_full);

    /* Try language only */
    IntlParsedTag cand = *pt;
    cand.script[0] = '\0';
    cand.region[0] = '\0';
    IntlParsedTag cand_max = cand;
    intl_maximize_pt(&cand_max);
    if (strcmp(cand_max.language, max_full.language) == 0 &&
        strcmp(cand_max.script, max_full.script) == 0 &&
        strcmp(cand_max.region, max_full.region) == 0) {
        *pt = cand;
        return;
    }

    /* Try language + region */
    if (max_full.region[0]) {
        cand = *pt;
        cand.script[0] = '\0';
        snprintf(cand.region, sizeof(cand.region), "%s", max_full.region);
        cand_max = cand;
        intl_maximize_pt(&cand_max);
        if (strcmp(cand_max.language, max_full.language) == 0 &&
            strcmp(cand_max.script, max_full.script) == 0 &&
            strcmp(cand_max.region, max_full.region) == 0) {
            *pt = cand;
            return;
        }
    }

    /* Try language + script */
    cand = *pt;
    snprintf(cand.script, sizeof(cand.script), "%s", max_full.script);
    cand.region[0] = '\0';
    cand_max = cand;
    intl_maximize_pt(&cand_max);
    if (strcmp(cand_max.language, max_full.language) == 0 &&
        strcmp(cand_max.script, max_full.script) == 0 &&
        strcmp(cand_max.region, max_full.region) == 0) {
        *pt = cand;
    }
}

static AjsValue js_intl_locale_toString(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_LOCALE, "Intl.Locale");
    if (!d) return AJS_VAL_EXCEPTION;
    return ajs_val_from_gc(ajs_str_from_cstr(vm, d->locale, false));
}

static AjsValue js_intl_locale_maximize(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_LOCALE, "Intl.Locale");
    if (!d) return AJS_VAL_EXCEPTION;
    IntlParsedTag pt; char canon[64];
    parse_and_canonicalize_bcp47(d->locale, &pt, canon, sizeof(canon));
    intl_maximize_pt(&pt);
    AjsObject *nl = intl_build_locale_from_pt(vm, vm->active_realm, &pt, vm->active_realm->intl_locale_proto);
    return nl ? ajs_val_from_gc(nl) : AJS_VAL_EXCEPTION;
}

static AjsValue js_intl_locale_minimize(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_LOCALE, "Intl.Locale");
    if (!d) return AJS_VAL_EXCEPTION;
    IntlParsedTag pt; char canon[64];
    parse_and_canonicalize_bcp47(d->locale, &pt, canon, sizeof(canon));
    intl_minimize_pt(&pt);
    AjsObject *nl = intl_build_locale_from_pt(vm, vm->active_realm, &pt, vm->active_realm->intl_locale_proto);
    return nl ? ajs_val_from_gc(nl) : AJS_VAL_EXCEPTION;
}

static AjsValue js_intl_locale_getter(AjsVm *vm, AjsValue this_val, int c, const AjsValue *v, void *u) {
    (void)c; (void)v; (void)u;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_LOCALE, "Intl.Locale");
    if (!d) return AJS_VAL_EXCEPTION;
    int32_t m = 0;
    if (vm->call_depth > 0 && vm->call_stack[vm->call_depth - 1u].native_fn_obj) {
        m = vm->call_stack[vm->call_depth - 1u].native_fn_obj->u.fn.native_magic;
    }
    const char *s = NULL;
    switch (m) {
        case 1: s = d->time_zone; break;
        case 2: s = d->language; break;
        case 3: s = d->script[0] ? d->script : NULL; break;
        case 4: s = d->region[0] ? d->region : NULL; break;
        case 5: s = d->calendar[0] ? d->calendar : NULL; break;
        case 6: s = d->collation[0] ? d->collation : NULL; break;
        case 7: s = d->numbering_system[0] ? d->numbering_system : NULL; break;
        case 8: s = d->hour_cycle[0] ? d->hour_cycle : NULL; break;
        case 9: s = d->case_first[0] ? d->case_first : NULL; break;
        case 10: return d->numeric ? AJS_VAL_TRUE : AJS_VAL_FALSE;
        default: break;
    }
    if (!s) return AJS_VAL_UNDEFINED;
    return ajs_val_from_gc(ajs_str_from_cstr(vm, s, false));
}
/* ============================================================================
 * 4. Intl.Collator & String.prototype.localeCompare (ECMA-402 §10)
 * ============================================================================ */

static bool intl_is_ignorable_punct(uint32_t cp) {
    if (cp <= 0x20u) return true;
    if ((cp >= 0x21u && cp <= 0x2Fu) || (cp >= 0x3Au && cp <= 0x40u) ||
        (cp >= 0x5Bu && cp <= 0x60u) || (cp >= 0x7Bu && cp <= 0x7Eu)) {
        return true;
    }
    if (cp == 0x00A0u || cp == 0x00A1u || cp == 0x00BFu || cp == 0x00B7u ||
        (cp >= 0x2000u && cp <= 0x206Fu) || (cp >= 0x3000u && cp <= 0x303Fu)) {
        return true;
    }
    return false;
}

/* Decompose Latin-1 / Latin Extended-A precomposed characters into (base ASCII lower, accent rank, is_upper) */
static void intl_collate_weights(uint32_t cp, uint32_t *out_primary, uint32_t *out_secondary, uint32_t *out_tertiary) {
    bool is_up = false;
    uint32_t accent = 0;
    uint32_t base = cp;

    if (cp >= 'A' && cp <= 'Z') {
        is_up = true;
        base = cp - 'A' + 'a';
    } else if (cp >= 0x00C0u && cp <= 0x00FFu && cp != 0x00D7u && cp != 0x00F7u) {
        uint32_t lc = cp;
        if (cp >= 0x00C0u && cp <= 0x00DEu) {
            is_up = true;
            lc = cp + 0x20u;
        }
        if (lc >= 0x00E0u && lc <= 0x00E6u)      { base = 'a'; accent = lc - 0x00DFu; }
        else if (lc == 0x00E7u)                  { base = 'c'; accent = 1; }
        else if (lc >= 0x00E8u && lc <= 0x00EBu) { base = 'e'; accent = lc - 0x00E7u; }
        else if (lc >= 0x00ECu && lc <= 0x00EFu) { base = 'i'; accent = lc - 0x00EBu; }
        else if (lc == 0x00F0u)                  { base = 'd'; accent = 1; }
        else if (lc == 0x00F1u)                  { base = 'n'; accent = 1; }
        else if ((lc >= 0x00F2u && lc <= 0x00F6u) || lc == 0x00F8u) { base = 'o'; accent = lc - 0x00F1u; }
        else if (lc >= 0x00F9u && lc <= 0x00FCu) { base = 'u'; accent = lc - 0x00F8u; }
        else if (lc == 0x00FDu || lc == 0x00FFu) { base = 'y'; accent = (lc == 0x00FDu) ? 1u : 2u; }
        else if (lc == 0x00DFu)                  { base = 's'; accent = 2; }
    } else {
        uint32_t lo = ajs_unicode_to_lower(cp);
        if (lo != cp) {
            is_up = true;
            base = lo;
        }
    }

    *out_primary = base;
    *out_secondary = accent;
    *out_tertiary = is_up ? 2u : 1u;
}

static int intl_collator_compare_strings(const AjsIntlData *d, const AjsString *sa, const AjsString *sb) {
    uint32_t ia = 0, ib = 0;
    uint32_t la = sa->len16, lb = sb->len16;
    int sec_diff = 0;
    int ter_diff = 0;

    bool sens_accent = (strcmp(d->sensitivity, "accent") == 0 || strcmp(d->sensitivity, "variant") == 0);
    bool sens_case   = (strcmp(d->sensitivity, "case") == 0 || strcmp(d->sensitivity, "variant") == 0);
    bool upper_first = (strcmp(d->case_first, "upper") == 0);

    while (true) {
        if (d->ignore_punct) {
            while (ia < la) {
                uint32_t adv = 1;
                uint32_t cp = ajs_utf16_code_point_at(sa->units, la, ia, &adv);
                if (!intl_is_ignorable_punct(cp)) break;
                ia += adv;
            }
            while (ib < lb) {
                uint32_t adv = 1;
                uint32_t cp = ajs_utf16_code_point_at(sb->units, lb, ib, &adv);
                if (!intl_is_ignorable_punct(cp)) break;
                ib += adv;
            }
        }

        if (ia >= la && ib >= lb) break;
        if (ia >= la) return -1;
        if (ib >= lb) return 1;

        uint32_t adv_a = 1, adv_b = 1;
        uint32_t cpa = ajs_utf16_code_point_at(sa->units, la, ia, &adv_a);
        uint32_t cpb = ajs_utf16_code_point_at(sb->units, lb, ib, &adv_b);

        /* Natural numeric collation (`numeric: true`) on contiguous ASCII digit runs */
        if (d->numeric && cpa >= '0' && cpa <= '9' && cpb >= '0' && cpb <= '9') {
            uint32_t sa_start = ia, sb_start = ib;
            while (ia < la && sa->units[ia] == '0') ia++;
            while (ib < lb && sb->units[ib] == '0') ib++;
            uint32_t nz_a = ia, nz_b = ib;
            while (ia < la && sa->units[ia] >= '0' && sa->units[ia] <= '9') ia++;
            while (ib < lb && sb->units[ib] >= '0' && sb->units[ib] <= '9') ib++;
            uint32_t len_a = ia - nz_a;
            uint32_t len_b = ib - nz_b;
            if (len_a != len_b) return (len_a < len_b) ? -1 : 1;
            for (uint32_t k = 0; k < len_a; k++) {
                if (sa->units[nz_a + k] != sb->units[nz_b + k]) {
                    return (sa->units[nz_a + k] < sb->units[nz_b + k]) ? -1 : 1;
                }
            }
            if (ter_diff == 0 && (ia - sa_start) != (ib - sb_start)) {
                ter_diff = ((ia - sa_start) < (ib - sb_start)) ? -1 : 1;
            }
            continue;
        }

        ia += adv_a;
        ib += adv_b;

        uint32_t pa, sa_w, ta;
        uint32_t pb, sb_w, tb;
        intl_collate_weights(cpa, &pa, &sa_w, &ta);
        intl_collate_weights(cpb, &pb, &sb_w, &tb);

        /* German phonebook collation (`de-u-co-phonebk`): ä->ae, ö->oe, ü->ue approximation */
        if (pa != pb) return (pa < pb) ? -1 : 1;
        if (sec_diff == 0 && sa_w != sb_w) {
            sec_diff = (sa_w < sb_w) ? -1 : 1;
        }
        if (ter_diff == 0 && ta != tb) {
            if (upper_first) ter_diff = (ta > tb) ? -1 : 1;
            else             ter_diff = (ta < tb) ? -1 : 1;
        }
        if (ter_diff == 0 && strcmp(d->sensitivity, "variant") == 0 && cpa != cpb) {
            ter_diff = (cpa < cpb) ? -1 : 1;
        }
    }

    if (sens_accent && sec_diff != 0) return sec_diff;
    if (sens_case && ter_diff != 0) return ter_diff;
    return 0;
}

static bool intl_init_collator_data(AjsVm *vm, AjsValue locales, AjsValue opts_arg, AjsIntlData *d) {
    size_t rs = ajs_temp_root_save(vm);
    AjsValue opts = AJS_VAL_UNDEFINED;
    if (!intl_coerce_options_obj(vm, opts_arg, &opts)) return false;
    ajs_temp_root_push(vm, opts);

    static const char *const k_usage[2] = { "sort", "search" };
    /* Store `usage` in d->style for Collator */
    if (!intl_get_opt_str(vm, opts, "usage", k_usage, 2, "sort", d->style, sizeof(d->style), NULL)) {
        ajs_temp_root_restore(vm, rs); return false;
    }

    IntlParsedTag pt;
    char full_loc[64];
    if (!intl_resolve_locale(vm, locales, opts, &pt, full_loc, sizeof(full_loc))) {
        ajs_temp_root_restore(vm, rs); return false;
    }

    bool num_pres = false, num_val = false;
    if (!intl_get_opt_bool(vm, opts, "numeric", false, &num_val, &num_pres)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    if (num_pres) {
        d->numeric = num_val;
    } else if (pt.has_u_kn) {
        d->numeric = (pt.u_kn[0] == '\0' || strcmp(pt.u_kn, "true") == 0);
    } else {
        d->numeric = false;
    }

    static const char *const k_kf[3] = { "upper", "lower", "false" };
    bool kf_pres = false;
    char kf_buf[16];
    if (!intl_get_opt_str(vm, opts, "caseFirst", k_kf, 3, "false", kf_buf, sizeof(kf_buf), &kf_pres)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    if (kf_pres) {
        snprintf(d->case_first, sizeof(d->case_first), "%s", kf_buf);
    } else if (pt.u_kf[0] && (strcmp(pt.u_kf, "upper") == 0 || strcmp(pt.u_kf, "lower") == 0 || strcmp(pt.u_kf, "false") == 0)) {
        snprintf(d->case_first, sizeof(d->case_first), "%.15s", pt.u_kf);
    } else {
        snprintf(d->case_first, sizeof(d->case_first), "false");
    }

    static const char *const k_sens[4] = { "base", "accent", "case", "variant" };
    if (!intl_get_opt_str(vm, opts, "sensitivity", k_sens, 4, "variant", d->sensitivity, sizeof(d->sensitivity), NULL)) {
        ajs_temp_root_restore(vm, rs); return false;
    }

    if (!intl_get_opt_bool(vm, opts, "ignorePunctuation", false, &d->ignore_punct, NULL)) {
        ajs_temp_root_restore(vm, rs); return false;
    }

    char col_opt[32];
    bool col_pres = false;
    if (!intl_get_opt_str(vm, opts, "collation", NULL, 0, "", col_opt, sizeof(col_opt), &col_pres)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    const char *chosen_col = "default";
    const char *cand_col = col_pres ? col_opt : pt.u_co;
    if (cand_col[0]) {
        for (size_t i = 0; i < sizeof(k_sv_collations) / sizeof(k_sv_collations[0]); i++) {
            if (strcmp(cand_col, k_sv_collations[i]) == 0) { chosen_col = k_sv_collations[i]; break; }
        }
    }
    snprintf(d->collation, sizeof(d->collation), "%s", chosen_col);

    /* Build resolved locale base tag */
    snprintf(d->language, sizeof(d->language), "%s", pt.language);
    snprintf(d->script, sizeof(d->script), "%s", pt.script);
    snprintf(d->region, sizeof(d->region), "%s", pt.region);
    format_parsed_tag(&pt, NULL, 0, d->locale, sizeof(d->locale));

    ajs_temp_root_restore(vm, rs);
    return true;
}

static AjsValue js_intl_collator_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    size_t rs = ajs_temp_root_save(vm);
    AjsValue proto = intl_proto_from_new_target(vm, nt, vm->active_realm->intl_collator_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, proto);

    AjsObject *obj = intl_alloc_instance(vm, vm->active_realm, AJS_INTL_COLLATOR, proto);
    if (!obj) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(obj));

    if (!intl_init_collator_data(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED,
                                 argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, obj->u.intl)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(obj);
}

static AjsValue js_intl_collator_compare_bound(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_COLLATOR, "Intl.Collator");
    if (!d) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *sx = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (vm->has_exception || !sx) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(sx));
    AjsString *sy = ajs_to_js_string(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED);
    if (vm->has_exception || !sy) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    int cmp = intl_collator_compare_strings(d, sx, sy);
    ajs_temp_root_restore(vm, rs);
    return ajs_val_number((double)cmp);
}

static AjsValue js_intl_collator_compare_get(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_COLLATOR, "Intl.Collator");
    if (!d) return AJS_VAL_EXCEPTION;
    if (ajs_is_undefined(d->bound_fn)) {
        d->bound_fn = intl_make_bound_fn(vm, vm->active_realm, js_intl_collator_compare_bound, 2, this_val);
    }
    return d->bound_fn;
}

static AjsValue js_intl_collator_resolvedOptions(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_COLLATOR, "Intl.Collator");
    if (!d) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue res = ajs_new_object(vm);
    ajs_temp_root_push(vm, res);
    ajs_define_prop_str(vm, res, "locale", ajs_val_from_gc(ajs_str_from_cstr(vm, d->locale, false)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "usage", ajs_val_from_gc(ajs_str_from_cstr(vm, d->style, true)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "sensitivity", ajs_val_from_gc(ajs_str_from_cstr(vm, d->sensitivity, true)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "ignorePunctuation", d->ignore_punct ? AJS_VAL_TRUE : AJS_VAL_FALSE, AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "collation", ajs_val_from_gc(ajs_str_from_cstr(vm, d->collation, true)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "numeric", d->numeric ? AJS_VAL_TRUE : AJS_VAL_FALSE, AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "caseFirst", ajs_val_from_gc(ajs_str_from_cstr(vm, d->case_first, true)), AJS_PROP_DEFAULT);
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_str_proto_localeCompare_intl(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (ajs_is_nullish(this_val)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "String.prototype.localeCompare called on null or undefined");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s1 = ajs_to_js_string(vm, this_val);
    if (vm->has_exception || !s1) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(s1));

    AjsString *s2 = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (vm->has_exception || !s2) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(s2));

    AjsIntlData cd;
    memset(&cd, 0, sizeof(cd));
    cd.kind = AJS_INTL_COLLATOR;
    if (!intl_init_collator_data(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED,
                                 argc > 2 ? argv[2] : AJS_VAL_UNDEFINED, &cd)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    int cmp = intl_collator_compare_strings(&cd, s1, s2);
    ajs_temp_root_restore(vm, rs);
    return ajs_val_number((double)cmp);
}

/* ============================================================================
 * 5. Intl.NumberFormat & Number/BigInt.prototype.toLocaleString (ECMA-402 §15)
 * ============================================================================ */

static int intl_currency_default_digits(const char *cur) {
    if (strcmp(cur, "JPY") == 0 || strcmp(cur, "KRW") == 0 || strcmp(cur, "VND") == 0 ||
        strcmp(cur, "CLP") == 0 || strcmp(cur, "ISK") == 0 || strcmp(cur, "TWD") == 0) {
        return 0;
    }
    if (strcmp(cur, "BHD") == 0 || strcmp(cur, "IQD") == 0 || strcmp(cur, "JOD") == 0 ||
        strcmp(cur, "KWD") == 0 || strcmp(cur, "LYD") == 0 || strcmp(cur, "OMR") == 0 ||
        strcmp(cur, "TND") == 0) {
        return 3;
    }
    return 2;
}

static const char *intl_currency_symbol(const char *cur, const char *display, const char *locale) {
    if (strcmp(display, "code") == 0) return cur;
    bool narrow = (strcmp(display, "narrowSymbol") == 0);
    if (strcmp(cur, "USD") == 0) return (narrow || intl_lang_is(locale, "en")) ? "$" : "US$";
    if (strcmp(cur, "KRW") == 0) return "\xE2\x82\xA9"; /* ₩ */
    if (strcmp(cur, "EUR") == 0) return "\xE2\x82\xAC"; /* € */
    if (strcmp(cur, "JPY") == 0) return "\xC2\xA5";     /* ¥ */
    if (strcmp(cur, "CNY") == 0) return narrow ? "\xC2\xA5" : "CN\xC2\xA5";
    if (strcmp(cur, "GBP") == 0) return "\xC2\xA3";     /* £ */
    if (strcmp(cur, "INR") == 0) return "\xE2\x82\xB9"; /* ₹ */
    if (strcmp(cur, "RUB") == 0) return "\xE2\x82\xBD"; /* ₽ */
    if (strcmp(cur, "TRY") == 0) return "\xE2\x82\xBA"; /* ₺ */
    if (strcmp(cur, "VND") == 0) return "\xE2\x82\xAB"; /* ₫ */
    if (strcmp(cur, "THB") == 0) return "\xE0\xB8\xBF"; /* ฿ */
    return cur;
}

static const char *intl_currency_name(const char *cur, const char *locale) {
    bool ko = intl_lang_is(locale, "ko");
    if (strcmp(cur, "KRW") == 0) return ko ? "\xEB\x8C\x80\xED\x95\x9C\xEB\xAF\xBC\xEA\xB5\xAD \xEC\x9B\x90" : "South Korean won";
    if (strcmp(cur, "USD") == 0) return ko ? "\xEB\xAF\xB8\xEA\xB5\xAD \xEB\x8B\xAC\xEB\x9F\xAC" : "US dollars";
    if (strcmp(cur, "EUR") == 0) return ko ? "\xEC\x9C\xA0\xEB\xA1\x9C" : "euros";
    if (strcmp(cur, "JPY") == 0) return ko ? "\xEC\x9D\xBC\xEB\xB3\xB8 \xEC\x97\x94\xED\x99\x94" : "Japanese yen";
    if (strcmp(cur, "GBP") == 0) return ko ? "\xEC\x98\x81\xEA\xB5\xAD \xED\x8C\x8C\xEC\x9A\xB4\xEB\x93\x9C" : "British pounds";
    if (strcmp(cur, "CNY") == 0) return ko ? "\xEC\xA4\x91\xEA\xB5\xAD \xEC\x9C\x84\xEC\x95\x88\xED\x99\x94" : "Chinese yuan";
    return cur;
}

static void intl_unit_label(const char *unit, const char *display, const char *locale, bool plural, char *out, size_t cap) {
    bool ko = intl_lang_is(locale, "ko");
    bool lg = (strcmp(display, "long") == 0);
    if (strcmp(unit, "kilometer-per-hour") == 0) { snprintf(out, cap, "%s", lg ? (ko ? "\xEC\x8B\x9C\xEC\x86\x8D \xED\x82\xAC\xEB\xA1\x9C\xEB\xAF\xB8\xED\x84\xB0" : (plural ? "kilometers per hour" : "kilometer per hour")) : "km/h"); return; }
    if (strcmp(unit, "meter-per-second") == 0)   { snprintf(out, cap, "%s", lg ? (plural ? "meters per second" : "meter per second") : "m/s"); return; }
    if (strcmp(unit, "kilometer") == 0)          { snprintf(out, cap, "%s", lg ? (ko ? "\xED\x82\xAC\xEB\xA1\x9C\xEB\xAF\xB8\xED\x84\xB0" : (plural ? "kilometers" : "kilometer")) : "km"); return; }
    if (strcmp(unit, "meter") == 0)              { snprintf(out, cap, "%s", lg ? (ko ? "\xEB\xAF\xB8\xED\x84\xB0" : (plural ? "meters" : "meter")) : "m"); return; }
    if (strcmp(unit, "centimeter") == 0)         { snprintf(out, cap, "%s", lg ? (plural ? "centimeters" : "centimeter") : "cm"); return; }
    if (strcmp(unit, "millimeter") == 0)         { snprintf(out, cap, "%s", lg ? (plural ? "millimeters" : "millimeter") : "mm"); return; }
    if (strcmp(unit, "kilogram") == 0)           { snprintf(out, cap, "%s", lg ? (ko ? "\xED\x82\xAC\xEB\xA1\x9C\xEA\xB7\xB8\xEB\x9E\x8C" : (plural ? "kilograms" : "kilogram")) : "kg"); return; }
    if (strcmp(unit, "gram") == 0)               { snprintf(out, cap, "%s", lg ? (plural ? "grams" : "gram") : "g"); return; }
    if (strcmp(unit, "celsius") == 0)            { snprintf(out, cap, "%s", lg ? (ko ? "\xEC\x84\xAD\xEC\x94\xA8" : "degrees Celsius") : "\xC2\xB0" "C"); return; }
    if (strcmp(unit, "fahrenheit") == 0)         { snprintf(out, cap, "%s", lg ? "degrees Fahrenheit" : "\xC2\xB0" "F"); return; }
    if (strcmp(unit, "percent") == 0)            { snprintf(out, cap, "%s", lg ? (ko ? "\xED\x8D\xBC\xEC\x84\xBC\xED\x8A\xB8" : "percent") : "%"); return; }
    if (strcmp(unit, "byte") == 0)               { snprintf(out, cap, "%s", lg ? (plural ? "bytes" : "byte") : "B"); return; }
    if (strcmp(unit, "kilobyte") == 0)           { snprintf(out, cap, "%s", lg ? (plural ? "kilobytes" : "kilobyte") : "kB"); return; }
    if (strcmp(unit, "megabyte") == 0)           { snprintf(out, cap, "%s", lg ? (plural ? "megabytes" : "megabyte") : "MB"); return; }
    if (strcmp(unit, "gigabyte") == 0)           { snprintf(out, cap, "%s", lg ? (plural ? "gigabytes" : "gigabyte") : "GB"); return; }
    if (strcmp(unit, "liter") == 0)              { snprintf(out, cap, "%s", lg ? (plural ? "liters" : "liter") : "L"); return; }
    if (strcmp(unit, "millisecond") == 0)        { snprintf(out, cap, "%s", lg ? (plural ? "milliseconds" : "millisecond") : "ms"); return; }
    if (strcmp(unit, "second") == 0)             { snprintf(out, cap, "%s", lg ? (plural ? "seconds" : "second") : "s"); return; }
    if (strcmp(unit, "minute") == 0)             { snprintf(out, cap, "%s", lg ? (plural ? "minutes" : "minute") : "min"); return; }
    if (strcmp(unit, "hour") == 0)               { snprintf(out, cap, "%s", lg ? (plural ? "hours" : "hour") : "hr"); return; }
    if (strcmp(unit, "day") == 0)                { snprintf(out, cap, "%s", lg ? (plural ? "days" : "day") : "d"); return; }
    snprintf(out, cap, "%s", unit);
}

static bool intl_init_numfmt_data(AjsVm *vm, AjsValue locales, AjsValue opts_arg, AjsIntlData *d) {
    size_t rs = ajs_temp_root_save(vm);
    AjsValue opts = AJS_VAL_UNDEFINED;
    if (!intl_coerce_options_obj(vm, opts_arg, &opts)) return false;
    ajs_temp_root_push(vm, opts);

    IntlParsedTag pt;
    char full_loc[64];
    if (!intl_resolve_locale(vm, locales, opts, &pt, full_loc, sizeof(full_loc))) {
        ajs_temp_root_restore(vm, rs); return false;
    }

    char nu_opt[32];
    bool nu_pres = false;
    if (!intl_get_opt_str(vm, opts, "numberingSystem", NULL, 0, "", nu_opt, sizeof(nu_opt), &nu_pres)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    if (nu_pres) {
        if (!intl_is_valid_unicode_type_seq(nu_opt)) {
            ajs_temp_root_restore(vm, rs);
            ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid numberingSystem '%s'", nu_opt);
            return false;
        }
        for (size_t k = 0; nu_opt[k]; k++) nu_opt[k] = (char)intl_ascii_tolower((unsigned char)nu_opt[k]);
    }
    const char *chosen_nu = "latn";
    const char *cand_nu = nu_pres ? nu_opt : pt.u_nu;
    if (cand_nu[0]) {
        for (size_t i = 0; i < sizeof(k_sv_numsys) / sizeof(k_sv_numsys[0]); i++) {
            if (strcmp(cand_nu, k_sv_numsys[i]) == 0) { chosen_nu = k_sv_numsys[i]; break; }
        }
    }
    snprintf(d->numbering_system, sizeof(d->numbering_system), "%s", chosen_nu);

    static const char *const k_styles[4] = { "decimal", "currency", "percent", "unit" };
    if (!intl_get_opt_str(vm, opts, "style", k_styles, 4, "decimal", d->style, sizeof(d->style), NULL)) {
        ajs_temp_root_restore(vm, rs); return false;
    }

    char cur_buf[16];
    bool cur_pres = false;
    if (!intl_get_opt_str(vm, opts, "currency", NULL, 0, "", cur_buf, sizeof(cur_buf), &cur_pres)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    if (cur_pres) {
        if (strlen(cur_buf) != 3 || !intl_is_alpha((unsigned char)cur_buf[0]) ||
            !intl_is_alpha((unsigned char)cur_buf[1]) || !intl_is_alpha((unsigned char)cur_buf[2])) {
            ajs_temp_root_restore(vm, rs);
            ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid ISO 4217 currency code '%s'", cur_buf);
            return false;
        }
        d->currency[0] = (char)intl_ascii_toupper((unsigned char)cur_buf[0]);
        d->currency[1] = (char)intl_ascii_toupper((unsigned char)cur_buf[1]);
        d->currency[2] = (char)intl_ascii_toupper((unsigned char)cur_buf[2]);
        d->currency[3] = '\0';
    } else if (strcmp(d->style, "currency") == 0) {
        ajs_temp_root_restore(vm, rs);
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Currency code is required with currency style");
        return false;
    }

    static const char *const k_cur_disp[4] = { "code", "symbol", "narrowSymbol", "name" };
    if (!intl_get_opt_str(vm, opts, "currencyDisplay", k_cur_disp, 4, "symbol",
                          d->currency_display, sizeof(d->currency_display), NULL)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    static const char *const k_cur_sign[2] = { "standard", "accounting" };
    /* Store `currencySign` in d->fallback for NumberFormat */
    if (!intl_get_opt_str(vm, opts, "currencySign", k_cur_sign, 2, "standard",
                          d->fallback, sizeof(d->fallback), NULL)) {
        ajs_temp_root_restore(vm, rs); return false;
    }

    char unit_buf[32];
    bool unit_pres = false;
    if (!intl_get_opt_str(vm, opts, "unit", NULL, 0, "", unit_buf, sizeof(unit_buf), &unit_pres)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    if (unit_pres) {
        if (!intl_is_valid_unit_id(unit_buf)) {
            ajs_temp_root_restore(vm, rs);
            ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid unit identifier '%s'", unit_buf);
            return false;
        }
        snprintf(d->unit, sizeof(d->unit), "%s", unit_buf);
    } else if (strcmp(d->style, "unit") == 0) {
        ajs_temp_root_restore(vm, rs);
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Unit is required with unit style");
        return false;
    }

    static const char *const k_unit_disp[3] = { "short", "narrow", "long" };
    if (!intl_get_opt_str(vm, opts, "unitDisplay", k_unit_disp, 3, "short",
                          d->unit_display, sizeof(d->unit_display), NULL)) {
        ajs_temp_root_restore(vm, rs); return false;
    }

    static const char *const k_notations[4] = { "standard", "scientific", "engineering", "compact" };
    if (!intl_get_opt_str(vm, opts, "notation", k_notations, 4, "standard",
                          d->notation, sizeof(d->notation), NULL)) {
        ajs_temp_root_restore(vm, rs); return false;
    }

    static const char *const k_comp_disp[2] = { "short", "long" };
    /* Store `compactDisplay` in d->disp_type for NumberFormat */
    if (!intl_get_opt_str(vm, opts, "compactDisplay", k_comp_disp, 2, "short",
                          d->disp_type, sizeof(d->disp_type), NULL)) {
        ajs_temp_root_restore(vm, rs); return false;
    }

    /* useGrouping: supports boolean or ES2023+ "auto" | "always" | "min2" | "false" */
    d->use_grouping = (strcmp(d->notation, "compact") != 0);
    if (!ajs_is_undefined(opts)) {
        AjsValue ug = ajs_get_prop_str(vm, opts, "useGrouping");
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
        if (!ajs_is_undefined(ug)) {
            if (ajs_is_bool(ug)) {
                d->use_grouping = (ug == AJS_VAL_TRUE);
            } else if (ajs_val_is_gc_kind(ug, AJS_GC_STRING)) {
                const AjsString *ugs = (const AjsString *)ajs_val_to_gc(ug);
                if (strcmp(ugs->utf8, "false") == 0 || strcmp(ugs->utf8, "") == 0) {
                    d->use_grouping = false;
                } else if (strcmp(ugs->utf8, "auto") == 0 || strcmp(ugs->utf8, "always") == 0 ||
                           strcmp(ugs->utf8, "min2") == 0 || strcmp(ugs->utf8, "true") == 0) {
                    d->use_grouping = true;
                } else {
                    ajs_temp_root_restore(vm, rs);
                    ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid useGrouping value '%s'", ugs->utf8);
                    return false;
                }
            } else {
                d->use_grouping = ajs_to_bool(vm, ug);
            }
        }
    }

    int min_int = 1;
    if (!intl_get_opt_int(vm, opts, "minimumIntegerDigits", 1, 21, 1, &min_int, NULL)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    d->min_int_digits = min_int;

    int def_min_fd = 0, def_max_fd = 3;
    if (strcmp(d->style, "currency") == 0 && strcmp(d->notation, "standard") == 0) {
        def_min_fd = intl_currency_default_digits(d->currency);
        def_max_fd = def_min_fd;
    } else if (strcmp(d->style, "percent") == 0) {
        def_min_fd = 0;
        def_max_fd = 0;
    } else if (strcmp(d->notation, "compact") == 0) {
        def_min_fd = 0;
        def_max_fd = 0;
    }

    int min_fd = def_min_fd, max_fd = def_max_fd;
    bool has_min_fd = false, has_max_fd = false;
    if (!intl_get_opt_int(vm, opts, "minimumFractionDigits", 0, 100, def_min_fd, &min_fd, &has_min_fd)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    int fallback_max_fd = has_min_fd && min_fd > def_max_fd ? min_fd : def_max_fd;
    if (!intl_get_opt_int(vm, opts, "maximumFractionDigits", 0, 100, fallback_max_fd, &max_fd, &has_max_fd)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    if (has_min_fd && has_max_fd && min_fd > max_fd) {
        ajs_temp_root_restore(vm, rs);
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "minimumFractionDigits (%d) > maximumFractionDigits (%d)", min_fd, max_fd);
        return false;
    }
    if (!has_min_fd && has_max_fd && min_fd > max_fd) min_fd = max_fd;
    d->min_frac_digits = min_fd;
    d->max_frac_digits = max_fd;

    int min_sd = 0, max_sd = 0;
    bool has_min_sd = false, has_max_sd = false;
    if (!intl_get_opt_int(vm, opts, "minimumSignificantDigits", 1, 21, 1, &min_sd, &has_min_sd)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    if (!intl_get_opt_int(vm, opts, "maximumSignificantDigits", 1, 21, 21, &max_sd, &has_max_sd)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    if (has_min_sd || has_max_sd) {
        if (!has_min_sd) min_sd = 1;
        if (!has_max_sd) max_sd = 21;
        if (min_sd > max_sd) {
            ajs_temp_root_restore(vm, rs);
            ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "minimumSignificantDigits > maximumSignificantDigits");
            return false;
        }
        d->min_sig_digits = min_sd;
        d->max_sig_digits = max_sd;
    }

    static const char *const k_sign_disp[5] = { "auto", "never", "always", "exceptZero", "negative" };
    if (!intl_get_opt_str(vm, opts, "signDisplay", k_sign_disp, 5, "auto",
                          d->sign_display, sizeof(d->sign_display), NULL)) {
        ajs_temp_root_restore(vm, rs); return false;
    }

    snprintf(d->language, sizeof(d->language), "%s", pt.language);
    snprintf(d->script, sizeof(d->script), "%s", pt.script);
    snprintf(d->region, sizeof(d->region), "%s", pt.region);
    format_parsed_tag(&pt, NULL, 0, d->locale, sizeof(d->locale));
    if (strcmp(d->numbering_system, "latn") != 0 && strcmp(d->numbering_system, pt.u_nu) == 0) {
        size_t bl = strlen(d->locale);
        snprintf(d->locale + bl, sizeof(d->locale) - bl, "-u-nu-%s", d->numbering_system);
    }

    ajs_temp_root_restore(vm, rs);
    return true;
}

typedef struct IntlNumParts {
    int  count;
    char types[24][20];
    char vals[24][160];
} IntlNumParts;

static void np_push(IntlNumParts *np, const char *t, const char *v) {
    if (!v || !v[0] || np->count >= 24) return;
    snprintf(np->types[np->count], sizeof(np->types[0]), "%s", t);
    snprintf(np->vals[np->count], sizeof(np->vals[0]), "%s", v);
    np->count++;
}

static void intl_locale_separators(const char *locale, const char **out_grp, const char **out_dec) {
    if (intl_lang_is(locale, "de") || intl_lang_is(locale, "es") || intl_lang_is(locale, "it") ||
        intl_lang_is(locale, "pt") || intl_lang_is(locale, "nl") || intl_lang_is(locale, "tr")) {
        *out_grp = ".";
        *out_dec = ",";
    } else if (intl_lang_is(locale, "fr") || intl_lang_is(locale, "ru") || intl_lang_is(locale, "pl") ||
               intl_lang_is(locale, "sv")) {
        *out_grp = "\xC2\xA0"; /* NBSP */
        *out_dec = ",";
    } else {
        *out_grp = ",";
        *out_dec = ".";
    }
}

static void emit_grouped_int_digits(IntlNumParts *np, const char *digits, size_t len,
                                    bool use_grouping, const char *grp_sep) {
    if (!use_grouping || len <= 3) {
        char buf[160];
        snprintf(buf, sizeof(buf), "%.*s", (int)(len < 155 ? len : 155), digits);
        np_push(np, "integer", buf);
        return;
    }
    size_t pos = 0;
    size_t first = len % 3;
    if (first == 0) first = 3;
    while (pos < len) {
        size_t chunk = (pos == 0) ? first : 3;
        char cbuf[16];
        ajs_memcpy(cbuf, digits + pos, chunk);
        cbuf[chunk] = '\0';
        np_push(np, "integer", cbuf);
        pos += chunk;
        if (pos < len) np_push(np, "group", grp_sep);
    }
}

static bool intl_coerce_intl_mathematical_val(AjsVm *vm, AjsValue val, bool *out_is_bigint,
                                              AjsBigInt **out_bi, double *out_num) {
    *out_is_bigint = false;
    *out_bi = NULL;
    *out_num = 0.0;

    if (ajs_val_is_gc_kind(val, AJS_GC_BIGINT)) {
        *out_is_bigint = true;
        *out_bi = (AjsBigInt *)ajs_val_to_gc(val);
        return true;
    }
    if (ajs_val_is_gc_kind(val, AJS_GC_STRING)) {
        const AjsString *s = (const AjsString *)ajs_val_to_gc(val);
        *out_num = ajs_str_to_number(s);
        return true;
    }
    AjsValue prim = ajs_to_primitive(vm, val, 2 /* number */);
    if (vm->has_exception) return false;
    if (ajs_val_is_gc_kind(prim, AJS_GC_BIGINT)) {
        *out_is_bigint = true;
        *out_bi = (AjsBigInt *)ajs_val_to_gc(prim);
        return true;
    }
    return ajs_to_number(vm, prim, out_num);
}

static bool intl_format_number_parts(AjsVm *vm, const AjsIntlData *d, AjsValue val, IntlNumParts *np) {
    memset(np, 0, sizeof(*np));
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, val);

    bool is_bi = false;
    AjsBigInt *bi = NULL;
    double num = 0.0;
    if (!intl_coerce_intl_mathematical_val(vm, val, &is_bi, &bi, &num)) {
        ajs_temp_root_restore(vm, rs);
        return false;
    }
    if (is_bi && bi) ajs_temp_root_push(vm, ajs_val_from_gc(bi));

    const char *grp_sep = ",", *dec_sep = ".";
    intl_locale_separators(d->locale, &grp_sep, &dec_sep);

    if (!is_bi && isnan(num)) {
        np_push(np, "nan", "NaN");
        ajs_temp_root_restore(vm, rs);
        return true;
    }

    bool is_neg = is_bi ? (bi->sign < 0) : (signbit(num) != 0);
    bool is_zero_val = is_bi ? (bi->sign == 0) : (num == 0.0);

    if (!is_bi && isinf(num)) {
        if (is_neg && strcmp(d->sign_display, "never") != 0) np_push(np, "minusSign", "-");
        else if (!is_neg && (strcmp(d->sign_display, "always") == 0 || strcmp(d->sign_display, "exceptZero") == 0))
            np_push(np, "plusSign", "+");
        np_push(np, "infinity", "\xE2\x88\x9E");
        ajs_temp_root_restore(vm, rs);
        return true;
    }

    /* Fast arbitrary-precision path for standard-notation BigInt (preserves >53-bit precision!) */
    char int_buf[256];
    char frac_buf[128];
    int_buf[0] = '\0';
    frac_buf[0] = '\0';
    const char *compact_sym = NULL;
    bool has_exp = false;
    int exp_val = 0;

    if (is_bi && strcmp(d->notation, "standard") == 0 && d->min_sig_digits == 0) {
        AjsString *bs = ajs_bigint_to_str(vm, bi, 10);
        if (!bs) { ajs_temp_root_restore(vm, rs); return false; }
        const char *digits = (bs->utf8[0] == '-') ? (bs->utf8 + 1) : bs->utf8;
        char raw_int[256];
        if (strcmp(d->style, "percent") == 0 && bi->sign != 0) {
            snprintf(raw_int, sizeof(raw_int), "%s00", digits);
        } else {
            snprintf(raw_int, sizeof(raw_int), "%s", digits);
        }
        size_t rlen = strlen(raw_int);
        size_t pad = ((int)rlen < d->min_int_digits) ? (size_t)(d->min_int_digits - (int)rlen) : 0;
        size_t ip = 0;
        for (size_t k = 0; k < pad && ip + 1 < sizeof(int_buf); k++) int_buf[ip++] = '0';
        snprintf(int_buf + ip, sizeof(int_buf) - ip, "%s", raw_int);
        for (int f = 0; f < d->min_frac_digits && f < 100; f++) frac_buf[f] = '0';
        frac_buf[d->min_frac_digits < 100 ? d->min_frac_digits : 100] = '\0';
    } else {
        double abs_v = 0.0;
        if (is_bi) {
            AjsString *bs = ajs_bigint_to_str(vm, bi, 10);
            abs_v = bs ? fabs(strtod(bs->utf8, NULL)) : 0.0;
        } else {
            abs_v = fabs(num);
        }
        if (strcmp(d->style, "percent") == 0) abs_v *= 100.0;

        int min_fd = d->min_frac_digits;
        int max_fd = d->max_frac_digits;

        if (strcmp(d->notation, "scientific") == 0 || strcmp(d->notation, "engineering") == 0) {
            has_exp = true;
            if (abs_v > 0.0) {
                exp_val = (int)floor(log10(abs_v));
                if (strcmp(d->notation, "engineering") == 0) {
                    int rem = ((exp_val % 3) + 3) % 3;
                    exp_val -= rem;
                }
                abs_v /= pow(10.0, (double)exp_val);
            }
        } else if (strcmp(d->notation, "compact") == 0) {
            bool ko = intl_lang_is(d->locale, "ko");
            bool ja = intl_lang_is(d->locale, "ja");
            bool zh = intl_lang_is(d->locale, "zh");
            bool lg = (strcmp(d->disp_type, "long") == 0);
            if (ko) {
                if (abs_v >= 1e12)      { abs_v /= 1e12; compact_sym = "\xEC\xA1\xB0"; }
                else if (abs_v >= 1e8)  { abs_v /= 1e8;  compact_sym = "\xEC\x96\xB5"; }
                else if (abs_v >= 1e4)  { abs_v /= 1e4;  compact_sym = "\xEB\xA7\x8C"; }
                else if (abs_v >= 1e3)  { abs_v /= 1e3;  compact_sym = "\xEC\xB2\x9C"; }
            } else if (ja || zh) {
                if (abs_v >= 1e12)      { abs_v /= 1e12; compact_sym = "\xE5\x85\x86"; }
                else if (abs_v >= 1e8)  { abs_v /= 1e8;  compact_sym = ja ? "\xE5\x84\x84" : "\xE4\xBA\xBF"; }
                else if (abs_v >= 1e4)  { abs_v /= 1e4;  compact_sym = "\xE4\xB8\x87"; }
            } else {
                if (abs_v >= 1e12)      { abs_v /= 1e12; compact_sym = lg ? " trillion" : "T"; }
                else if (abs_v >= 1e9)  { abs_v /= 1e9;  compact_sym = lg ? " billion"  : "B"; }
                else if (abs_v >= 1e6)  { abs_v /= 1e6;  compact_sym = lg ? " million"  : "M"; }
                else if (abs_v >= 1e3)  { abs_v /= 1e3;  compact_sym = lg ? " thousand" : "K"; }
            }
            if (compact_sym && max_fd == 0 && abs_v < 10.0 && fabs(abs_v - floor(abs_v + 0.5)) >= 0.05) {
                max_fd = 1;
            }
        }

        if (d->max_sig_digits > 0) {
            int sig_e = (abs_v > 0.0) ? (int)floor(log10(abs_v)) : 0;
            max_fd = d->max_sig_digits - 1 - sig_e;
            min_fd = d->min_sig_digits - 1 - sig_e;
            if (max_fd < 0) {
                double scale = pow(10.0, (double)(-max_fd));
                abs_v = floor(abs_v / scale + 0.5) * scale;
                max_fd = 0;
            }
            if (min_fd < 0) min_fd = 0;
        }

        int prec = max_fd > 20 ? 20 : max_fd;
        /* Half-away-from-zero rounding per ECMA-402 DefaultNumberOption ("halfExpand") */
        double p10 = pow(10.0, (double)prec);
        double scaled = floor(abs_v * p10 + 0.5 + 1e-12);
        double rounded = scaled / p10;

        char tmp[128];
        snprintf(tmp, sizeof(tmp), "%.*f", prec, rounded);
        char *dot = strchr(tmp, '.');
        size_t ilen = dot ? (size_t)(dot - tmp) : strlen(tmp);
        size_t pad = ((int)ilen < d->min_int_digits) ? (size_t)(d->min_int_digits - (int)ilen) : 0;
        size_t ip = 0;
        for (size_t k = 0; k < pad && ip + 1 < sizeof(int_buf); k++) int_buf[ip++] = '0';
        ajs_memcpy(int_buf + ip, tmp, ilen);
        int_buf[ip + ilen] = '\0';

        if (dot) {
            snprintf(frac_buf, sizeof(frac_buf), "%s", dot + 1);
            size_t flen = strlen(frac_buf);
            while ((int)flen > min_fd && flen > 0 && frac_buf[flen - 1] == '0') {
                frac_buf[--flen] = '\0';
            }
            while ((int)flen < min_fd && flen + 1 < sizeof(frac_buf)) {
                frac_buf[flen++] = '0';
                frac_buf[flen] = '\0';
            }
        }
        /* Check if rounded value became zero */
        bool all_z = true;
        for (size_t k = 0; int_buf[k]; k++) if (int_buf[k] != '0') all_z = false;
        for (size_t k = 0; frac_buf[k]; k++) if (frac_buf[k] != '0') all_z = false;
        if (all_z) is_zero_val = true;
    }

    /* Sign display rules (ECMA-402 §15.5.7) */
    bool show_minus = false, show_plus = false;
    bool acct_parens = (strcmp(d->style, "currency") == 0 && strcmp(d->fallback, "accounting") == 0);
    if (strcmp(d->sign_display, "never") == 0) {
        show_minus = false; show_plus = false;
    } else if (strcmp(d->sign_display, "always") == 0) {
        if (is_neg) show_minus = true; else show_plus = true;
    } else if (strcmp(d->sign_display, "exceptZero") == 0) {
        if (!is_zero_val) { if (is_neg) show_minus = true; else show_plus = true; }
    } else if (strcmp(d->sign_display, "negative") == 0) {
        if (is_neg && !is_zero_val) show_minus = true;
    } else { /* "auto" */
        if (is_neg) show_minus = true; /* Preserves -0 -> "-0"! */
    }

    if (show_minus) {
        if (acct_parens) np_push(np, "literal", "(");
        else             np_push(np, "minusSign", "-");
    } else if (show_plus) {
        np_push(np, "plusSign", "+");
    }

    bool cur_prefix = (strcmp(d->style, "currency") == 0 && strcmp(d->currency_display, "name") != 0 &&
                       !intl_lang_is(d->locale, "de") && !intl_lang_is(d->locale, "fr") && !intl_lang_is(d->locale, "ru"));
    if (cur_prefix) {
        const char *cs = intl_currency_symbol(d->currency, d->currency_display, d->locale);
        np_push(np, "currency", cs);
        if (strcmp(d->currency_display, "code") == 0) np_push(np, "literal", "\xC2\xA0");
    }

    emit_grouped_int_digits(np, int_buf, strlen(int_buf), d->use_grouping, grp_sep);
    if (frac_buf[0]) {
        np_push(np, "decimal", dec_sep);
        np_push(np, "fraction", frac_buf);
    }

    if (has_exp) {
        np_push(np, "exponentSeparator", "E");
        if (exp_val < 0) np_push(np, "exponentMinusSign", "-");
        char ebuf[16];
        snprintf(ebuf, sizeof(ebuf), "%d", abs(exp_val));
        np_push(np, "exponentInteger", ebuf);
    } else if (compact_sym) {
        if (compact_sym[0] == ' ') {
            np_push(np, "literal", " ");
            np_push(np, "compact", compact_sym + 1);
        } else {
            np_push(np, "compact", compact_sym);
        }
    }

    if (strcmp(d->style, "percent") == 0) {
        np_push(np, "percentSign", "%");
    } else if (strcmp(d->style, "currency") == 0 && !cur_prefix) {
        np_push(np, "literal", strcmp(d->currency_display, "name") == 0 ? " " : "\xC2\xA0");
        const char *cs = strcmp(d->currency_display, "name") == 0
                             ? intl_currency_name(d->currency, d->locale)
                             : intl_currency_symbol(d->currency, d->currency_display, d->locale);
        np_push(np, "currency", cs);
    } else if (strcmp(d->style, "unit") == 0) {
        bool is_one = (strcmp(int_buf, "1") == 0 && frac_buf[0] == '\0' && !compact_sym);
        char ubuf[64];
        intl_unit_label(d->unit, d->unit_display, d->locale, !is_one, ubuf, sizeof(ubuf));
        if (strcmp(d->unit_display, "narrow") != 0 && strcmp(d->unit, "percent") != 0 && strcmp(d->unit, "degree") != 0) {
            np_push(np, "literal", " ");
        }
        np_push(np, "unit", ubuf);
    }

    if (show_minus && acct_parens) np_push(np, "literal", ")");

    ajs_temp_root_restore(vm, rs);
    return true;
}

static AjsString *intl_join_num_parts(AjsVm *vm, const IntlNumParts *np) {
    char buf[512];
    size_t pos = 0;
    buf[0] = '\0';
    for (int i = 0; i < np->count; i++) {
        size_t vl = strlen(np->vals[i]);
        if (pos + vl + 1 < sizeof(buf)) {
            ajs_memcpy(buf + pos, np->vals[i], vl);
            pos += vl;
            buf[pos] = '\0';
        }
    }
    return ajs_str_from_utf8(vm, buf, pos, false);
}

static AjsValue js_intl_numfmt_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    size_t rs = ajs_temp_root_save(vm);
    AjsValue proto = intl_proto_from_new_target(vm, nt, vm->active_realm->intl_numfmt_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, proto);

    AjsObject *obj = intl_alloc_instance(vm, vm->active_realm, AJS_INTL_NUMBER_FORMAT, proto);
    if (!obj) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(obj));

    if (!intl_init_numfmt_data(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED,
                               argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, obj->u.intl)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(obj);
}

static AjsValue js_intl_numfmt_format_bound(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_NUMBER_FORMAT, "Intl.NumberFormat");
    if (!d) return AJS_VAL_EXCEPTION;
    IntlNumParts np;
    if (!intl_format_number_parts(vm, d, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &np)) {
        return AJS_VAL_EXCEPTION;
    }
    AjsString *s = intl_join_num_parts(vm, &np);
    return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
}

static AjsValue js_intl_numfmt_format_get(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_NUMBER_FORMAT, "Intl.NumberFormat");
    if (!d) return AJS_VAL_EXCEPTION;
    if (ajs_is_undefined(d->bound_fn)) {
        d->bound_fn = intl_make_bound_fn(vm, vm->active_realm, js_intl_numfmt_format_bound, 1, this_val);
    }
    return d->bound_fn;
}

static AjsValue js_intl_numfmt_formatToParts(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_NUMBER_FORMAT, "Intl.NumberFormat");
    if (!d) return AJS_VAL_EXCEPTION;
    IntlNumParts np;
    if (!intl_format_number_parts(vm, d, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &np)) {
        return AJS_VAL_EXCEPTION;
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue arr = ajs_new_array(vm, (uint32_t)np.count);
    ajs_temp_root_push(vm, arr);
    uint32_t idx = 0;
    for (int i = 0; i < np.count; i++) {
        if (!intl_push_part_ex(vm, arr, &idx, np.types[i], np.vals[i], NULL)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    ajs_temp_root_restore(vm, rs);
    return arr;
}

static AjsValue js_intl_numfmt_formatRange(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_NUMBER_FORMAT, "Intl.NumberFormat");
    if (!d) return AJS_VAL_EXCEPTION;
    if (argc < 2 || ajs_is_undefined(argv[0]) || ajs_is_undefined(argv[1])) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "formatRange requires start and end arguments");
    }
    IntlNumParts p1, p2;
    if (!intl_format_number_parts(vm, d, argv[0], &p1) || !intl_format_number_parts(vm, d, argv[1], &p2)) {
        return AJS_VAL_EXCEPTION;
    }
    if ((p1.count == 1 && strcmp(p1.types[0], "nan") == 0) ||
        (p2.count == 1 && strcmp(p2.types[0], "nan") == 0)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid NaN argument to formatRange");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsString *s1 = intl_join_num_parts(vm, &p1);
    ajs_temp_root_push(vm, ajs_val_from_gc(s1));
    AjsString *s2 = intl_join_num_parts(vm, &p2);
    ajs_temp_root_push(vm, ajs_val_from_gc(s2));

    char buf[512];
    if (strcmp(s1->utf8, s2->utf8) == 0) {
        snprintf(buf, sizeof(buf), "~%s", s1->utf8);
    } else {
        snprintf(buf, sizeof(buf), "%s\xE2\x80\x93%s", s1->utf8, s2->utf8); /* en-dash */
    }
    AjsString *res = ajs_str_from_cstr(vm, buf, false);
    ajs_temp_root_restore(vm, rs);
    return res ? ajs_val_from_gc(res) : AJS_VAL_EXCEPTION;
}

static AjsValue js_intl_numfmt_resolvedOptions(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_NUMBER_FORMAT, "Intl.NumberFormat");
    if (!d) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue res = ajs_new_object(vm);
    ajs_temp_root_push(vm, res);
    ajs_define_prop_str(vm, res, "locale", ajs_val_from_gc(ajs_str_from_cstr(vm, d->locale, false)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "numberingSystem", ajs_val_from_gc(ajs_str_from_cstr(vm, d->numbering_system, true)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "style", ajs_val_from_gc(ajs_str_from_cstr(vm, d->style, true)), AJS_PROP_DEFAULT);
    if (strcmp(d->style, "currency") == 0) {
        ajs_define_prop_str(vm, res, "currency", ajs_val_from_gc(ajs_str_from_cstr(vm, d->currency, true)), AJS_PROP_DEFAULT);
        ajs_define_prop_str(vm, res, "currencyDisplay", ajs_val_from_gc(ajs_str_from_cstr(vm, d->currency_display, true)), AJS_PROP_DEFAULT);
        ajs_define_prop_str(vm, res, "currencySign", ajs_val_from_gc(ajs_str_from_cstr(vm, d->fallback, true)), AJS_PROP_DEFAULT);
    }
    if (strcmp(d->style, "unit") == 0) {
        ajs_define_prop_str(vm, res, "unit", ajs_val_from_gc(ajs_str_from_cstr(vm, d->unit, true)), AJS_PROP_DEFAULT);
        ajs_define_prop_str(vm, res, "unitDisplay", ajs_val_from_gc(ajs_str_from_cstr(vm, d->unit_display, true)), AJS_PROP_DEFAULT);
    }
    ajs_define_prop_str(vm, res, "minimumIntegerDigits", ajs_val_number((double)d->min_int_digits), AJS_PROP_DEFAULT);
    if (d->max_sig_digits > 0) {
        ajs_define_prop_str(vm, res, "minimumSignificantDigits", ajs_val_number((double)d->min_sig_digits), AJS_PROP_DEFAULT);
        ajs_define_prop_str(vm, res, "maximumSignificantDigits", ajs_val_number((double)d->max_sig_digits), AJS_PROP_DEFAULT);
    } else {
        ajs_define_prop_str(vm, res, "minimumFractionDigits", ajs_val_number((double)d->min_frac_digits), AJS_PROP_DEFAULT);
        ajs_define_prop_str(vm, res, "maximumFractionDigits", ajs_val_number((double)d->max_frac_digits), AJS_PROP_DEFAULT);
    }
    ajs_define_prop_str(vm, res, "useGrouping", d->use_grouping ? AJS_VAL_TRUE : AJS_VAL_FALSE, AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "notation", ajs_val_from_gc(ajs_str_from_cstr(vm, d->notation, true)), AJS_PROP_DEFAULT);
    if (strcmp(d->notation, "compact") == 0) {
        ajs_define_prop_str(vm, res, "compactDisplay", ajs_val_from_gc(ajs_str_from_cstr(vm, d->disp_type, true)), AJS_PROP_DEFAULT);
    }
    ajs_define_prop_str(vm, res, "signDisplay", ajs_val_from_gc(ajs_str_from_cstr(vm, d->sign_display, true)), AJS_PROP_DEFAULT);
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_num_proto_toLocaleString_intl(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsValue nv = this_val;
    if (!ajs_val_is_number(nv)) {
        if (ajs_val_is_gc_kind(nv, AJS_GC_OBJECT)) {
            AjsObject *o = (AjsObject *)ajs_val_to_gc(nv);
            if (o->kind == AJS_OBJ_NUMBER_OBJ) nv = o->u.prim_val;
            else return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Number.prototype.toLocaleString requires a Number");
        } else {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Number.prototype.toLocaleString requires a Number");
        }
    }
    AjsIntlData nd;
    memset(&nd, 0, sizeof(nd));
    nd.kind = AJS_INTL_NUMBER_FORMAT;
    if (!intl_init_numfmt_data(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED,
                               argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, &nd)) {
        return AJS_VAL_EXCEPTION;
    }
    IntlNumParts np;
    if (!intl_format_number_parts(vm, &nd, nv, &np)) return AJS_VAL_EXCEPTION;
    AjsString *s = intl_join_num_parts(vm, &np);
    return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
}

static AjsValue js_bigint_proto_toLocaleString_intl(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsValue bv = this_val;
    if (!ajs_val_is_gc_kind(bv, AJS_GC_BIGINT)) {
        if (ajs_val_is_gc_kind(bv, AJS_GC_OBJECT)) {
            AjsObject *o = (AjsObject *)ajs_val_to_gc(bv);
            if (o->kind == AJS_OBJ_BIGINT_OBJ) bv = o->u.prim_val;
            else return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "BigInt.prototype.toLocaleString requires a BigInt");
        } else {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "BigInt.prototype.toLocaleString requires a BigInt");
        }
    }
    AjsIntlData nd;
    memset(&nd, 0, sizeof(nd));
    nd.kind = AJS_INTL_NUMBER_FORMAT;
    if (!intl_init_numfmt_data(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED,
                               argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, &nd)) {
        return AJS_VAL_EXCEPTION;
    }
    IntlNumParts np;
    if (!intl_format_number_parts(vm, &nd, bv, &np)) return AJS_VAL_EXCEPTION;
    AjsString *s = intl_join_num_parts(vm, &np);
    return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
}

/* ============================================================================
 * 6. Intl.DateTimeFormat & Date.prototype.toLocale*String (ECMA-402 §11)
 * ============================================================================ */

typedef struct IntlTzEntry {
    const char *alias;
    const char *canonical;
    int32_t     std_offset_min;
    int         dst_rule; /* 0 = none, 1 = US, 2 = EU */
} IntlTzEntry;

static const IntlTzEntry k_tz_table[] = {
    {"UTC",                  "UTC",                  0,    0},
    {"Etc/UTC",              "UTC",                  0,    0},
    {"Etc/GMT",              "UTC",                  0,    0},
    {"GMT",                  "UTC",                  0,    0},
    {"Z",                    "UTC",                  0,    0},
    {"Asia/Seoul",           "Asia/Seoul",           540,  0},
    {"ROK",                  "Asia/Seoul",           540,  0},
    {"Asia/Tokyo",           "Asia/Tokyo",           540,  0},
    {"Japan",                "Asia/Tokyo",           540,  0},
    {"Asia/Shanghai",        "Asia/Shanghai",        480,  0},
    {"PRC",                  "Asia/Shanghai",        480,  0},
    {"Asia/Hong_Kong",       "Asia/Hong_Kong",       480,  0},
    {"Asia/Taipei",          "Asia/Taipei",          480,  0},
    {"Asia/Singapore",       "Asia/Singapore",       480,  0},
    {"Asia/Bangkok",         "Asia/Bangkok",         420,  0},
    {"Asia/Kolkata",         "Asia/Kolkata",         330,  0},
    {"Asia/Calcutta",        "Asia/Kolkata",         330,  0},
    {"Asia/Dubai",           "Asia/Dubai",           240,  0},
    {"Europe/London",        "Europe/London",        0,    2},
    {"GB",                   "Europe/London",        0,    2},
    {"Europe/Paris",         "Europe/Paris",         60,   2},
    {"Europe/Berlin",        "Europe/Berlin",        60,   2},
    {"Europe/Madrid",        "Europe/Madrid",        60,   2},
    {"Europe/Rome",          "Europe/Rome",          60,   2},
    {"Europe/Amsterdam",     "Europe/Amsterdam",     60,   2},
    {"Europe/Moscow",        "Europe/Moscow",        180,  0},
    {"America/New_York",     "America/New_York",    -300,  1},
    {"US/Eastern",           "America/New_York",    -300,  1},
    {"America/Toronto",      "America/Toronto",     -300,  1},
    {"America/Chicago",      "America/Chicago",     -360,  1},
    {"US/Central",           "America/Chicago",     -360,  1},
    {"America/Denver",       "America/Denver",      -420,  1},
    {"US/Mountain",          "America/Denver",      -420,  1},
    {"America/Los_Angeles",  "America/Los_Angeles", -480,  1},
    {"US/Pacific",           "America/Los_Angeles", -480,  1},
    {"America/Sao_Paulo",    "America/Sao_Paulo",   -180,  0},
    {"Australia/Sydney",     "Australia/Sydney",     600,  0},
    {"Pacific/Auckland",     "Pacific/Auckland",     720,  0}
};

static bool intl_canonicalize_timezone(const char *raw, char *out_tz, size_t cap, int32_t *out_std_off) {
    if (!raw || !raw[0]) return false;
    for (size_t i = 0; i < sizeof(k_tz_table) / sizeof(k_tz_table[0]); i++) {
        if (intl_strcasecmp(raw, k_tz_table[i].alias) == 0) {
            snprintf(out_tz, cap, "%s", k_tz_table[i].canonical);
            *out_std_off = k_tz_table[i].std_offset_min;
            return true;
        }
    }
    /* ISO 8601 UTC offset: ±HH:MM, ±HHMM, ±HH */
    if (raw[0] == '+' || raw[0] == '-') {
        int sign = (raw[0] == '-') ? -1 : 1;
        size_t len = strlen(raw);
        int hh = -1, mm = -1;
        if (len == 3 && intl_is_digit((unsigned char)raw[1]) && intl_is_digit((unsigned char)raw[2])) {
            hh = (raw[1] - '0') * 10 + (raw[2] - '0');
            mm = 0;
        } else if (len == 5 && intl_is_digit((unsigned char)raw[1]) && intl_is_digit((unsigned char)raw[2]) &&
                   intl_is_digit((unsigned char)raw[3]) && intl_is_digit((unsigned char)raw[4])) {
            hh = (raw[1] - '0') * 10 + (raw[2] - '0');
            mm = (raw[3] - '0') * 10 + (raw[4] - '0');
        } else if (len == 6 && raw[3] == ':' &&
                   intl_is_digit((unsigned char)raw[1]) && intl_is_digit((unsigned char)raw[2]) &&
                   intl_is_digit((unsigned char)raw[4]) && intl_is_digit((unsigned char)raw[5])) {
            hh = (raw[1] - '0') * 10 + (raw[2] - '0');
            mm = (raw[4] - '0') * 10 + (raw[5] - '0');
        }
        if (hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59) {
            int32_t total = sign * (hh * 60 + mm);
            if (total == 0) sign = 1; /* -00:00 canonicalizes to +00:00 */
            snprintf(out_tz, cap, "%c%02d:%02d", sign < 0 ? '-' : '+', hh, mm);
            *out_std_off = total;
            return true;
        }
    }
    return false;
}

typedef struct IntlCivilDate {
    int64_t year;
    int     month;   /* 1..12 */
    int     day;     /* 1..31 */
    int     weekday; /* 0=Sun..6=Sat */
    int     hour;    /* 0..23 */
    int     minute;  /* 0..59 */
    int     second;  /* 0..59 */
    int     ms;      /* 0..999 */
} IntlCivilDate;

static int64_t intl_days_from_civil(int64_t y, int m, int d) {
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(m > 2 ? m - 3 : m + 9) + 2u) / 5u + (unsigned)d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void intl_decompose_epoch_ms(int64_t epoch_ms, int32_t offset_min, IntlCivilDate *out) {
    int64_t local_ms = epoch_ms + (int64_t)offset_min * 60000LL;
    int64_t day_ms = 86400000LL;
    int64_t z = local_ms / day_ms;
    int64_t rem = local_ms % day_ms;
    if (rem < 0) { rem += day_ms; z -= 1; }

    out->weekday = (int)(((z + 4) % 7 + 7) % 7);
    out->hour    = (int)(rem / 3600000LL);
    rem %= 3600000LL;
    out->minute  = (int)(rem / 60000LL);
    rem %= 60000LL;
    out->second  = (int)(rem / 1000LL);
    out->ms      = (int)(rem % 1000LL);

    int64_t zp = z + 719468;
    int64_t era = (zp >= 0 ? zp : zp - 146096) / 146097;
    unsigned doe = (unsigned)(zp - era * 146097);
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    unsigned m = (mp < 10u) ? (mp + 3u) : (mp - 9u);
    out->year  = y + (m <= 2u ? 1 : 0);
    out->month = (int)m;
    out->day   = (int)d;
}

static int32_t intl_effective_tz_offset(const char *tz, int32_t std_off, int64_t utc_ms, bool *out_is_dst) {
    *out_is_dst = false;
    int dst_rule = 0;
    for (size_t i = 0; i < sizeof(k_tz_table) / sizeof(k_tz_table[0]); i++) {
        if (strcmp(tz, k_tz_table[i].canonical) == 0) { dst_rule = k_tz_table[i].dst_rule; break; }
    }
    if (dst_rule == 0) return std_off;

    IntlCivilDate cd;
    intl_decompose_epoch_ms(utc_ms, 0, &cd);
    if (cd.year < 1970 || cd.year > 2100) return std_off;

    if (dst_rule == 1) {
        /* US DST (2007+): 2nd Sunday in March 02:00 std -> 1st Sunday in Nov 02:00 dst (01:00 std) */
        int64_t mar1 = intl_days_from_civil(cd.year, 3, 1);
        int mar1_wd = (int)(((mar1 + 4) % 7 + 7) % 7);
        int mar_sun2 = 1 + ((7 - mar1_wd) % 7) + 7;
        int64_t start_utc = (intl_days_from_civil(cd.year, 3, mar_sun2) * 1440LL + 120LL - std_off) * 60000LL;

        int64_t nov1 = intl_days_from_civil(cd.year, 11, 1);
        int nov1_wd = (int)(((nov1 + 4) % 7 + 7) % 7);
        int nov_sun1 = 1 + ((7 - nov1_wd) % 7);
        int64_t end_utc = (intl_days_from_civil(cd.year, 11, nov_sun1) * 1440LL + 120LL - (std_off + 60)) * 60000LL;

        if (utc_ms >= start_utc && utc_ms < end_utc) {
            *out_is_dst = true;
            return std_off + 60;
        }
    } else if (dst_rule == 2) {
        /* EU DST: Last Sunday in March 01:00 UTC -> Last Sunday in October 01:00 UTC */
        int64_t mar31 = intl_days_from_civil(cd.year, 3, 31);
        int mar31_wd = (int)(((mar31 + 4) % 7 + 7) % 7);
        int mar_last_sun = 31 - mar31_wd;
        int64_t start_utc = (intl_days_from_civil(cd.year, 3, mar_last_sun) * 1440LL + 60LL) * 60000LL;

        int64_t oct31 = intl_days_from_civil(cd.year, 10, 31);
        int oct31_wd = (int)(((oct31 + 4) % 7 + 7) % 7);
        int oct_last_sun = 31 - oct31_wd;
        int64_t end_utc = (intl_days_from_civil(cd.year, 10, oct_last_sun) * 1440LL + 60LL) * 60000LL;

        if (utc_ms >= start_utc && utc_ms < end_utc) {
            *out_is_dst = true;
            return std_off + 60;
        }
    }
    return std_off;
}

/* Modes for DateTimeFormat defaults: 0=DATE_DEFAULT, 1=ALL_DEFAULT, 2=TIME_DEFAULT */
static bool intl_init_dtfmt_data(AjsVm *vm, AjsValue locales, AjsValue opts_arg, int def_mode, AjsIntlData *d) {
    size_t rs = ajs_temp_root_save(vm);
    AjsValue opts = AJS_VAL_UNDEFINED;
    if (!intl_coerce_options_obj(vm, opts_arg, &opts)) return false;
    ajs_temp_root_push(vm, opts);

    IntlParsedTag pt;
    char full_loc[64];
    if (!intl_resolve_locale(vm, locales, opts, &pt, full_loc, sizeof(full_loc))) {
        ajs_temp_root_restore(vm, rs); return false;
    }

    char ca_opt[32], nu_opt[32];
    bool ca_pres = false, nu_pres = false;
    if (!intl_get_opt_str(vm, opts, "calendar", NULL, 0, "", ca_opt, sizeof(ca_opt), &ca_pres) ||
        !intl_get_opt_str(vm, opts, "numberingSystem", NULL, 0, "", nu_opt, sizeof(nu_opt), &nu_pres)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    if (ca_pres && !intl_is_valid_unicode_type_seq(ca_opt)) {
        ajs_temp_root_restore(vm, rs);
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid calendar '%s'", ca_opt);
        return false;
    }
    if (nu_pres && !intl_is_valid_unicode_type_seq(nu_opt)) {
        ajs_temp_root_restore(vm, rs);
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid numberingSystem '%s'", nu_opt);
        return false;
    }
    const char *chosen_ca = "gregory";
    const char *cand_ca = ca_pres ? ca_opt : pt.u_ca;
    if (cand_ca[0]) {
        char lca[32];
        for (size_t k = 0; k < sizeof(lca) - 1 && cand_ca[k]; k++) {
            lca[k] = (char)intl_ascii_tolower((unsigned char)cand_ca[k]);
            lca[k + 1] = '\0';
        }
        if (strcmp(lca, "gregorian") == 0) snprintf(lca, sizeof(lca), "gregory");
        for (size_t i = 0; i < sizeof(k_sv_calendars) / sizeof(k_sv_calendars[0]); i++) {
            if (strcmp(lca, k_sv_calendars[i]) == 0) { chosen_ca = k_sv_calendars[i]; break; }
        }
    }
    snprintf(d->calendar, sizeof(d->calendar), "%s", chosen_ca);

    const char *chosen_nu = "latn";
    const char *cand_nu = nu_pres ? nu_opt : pt.u_nu;
    if (cand_nu[0]) {
        for (size_t i = 0; i < sizeof(k_sv_numsys) / sizeof(k_sv_numsys[0]); i++) {
            if (intl_strcasecmp(cand_nu, k_sv_numsys[i]) == 0) { chosen_nu = k_sv_numsys[i]; break; }
        }
    }
    snprintf(d->numbering_system, sizeof(d->numbering_system), "%s", chosen_nu);

    bool h12_pres = false, h12_val = false;
    if (!intl_get_opt_bool(vm, opts, "hour12", false, &h12_val, &h12_pres)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    static const char *const k_hc[4] = { "h11", "h12", "h23", "h24" };
    char hc_opt[16];
    bool hc_pres = false;
    if (!intl_get_opt_str(vm, opts, "hourCycle", k_hc, 4, "", hc_opt, sizeof(hc_opt), &hc_pres)) {
        ajs_temp_root_restore(vm, rs); return false;
    }

    const char *def_hc = (strcmp(pt.language, "en") == 0 && (!pt.region[0] || strcmp(pt.region, "US") == 0)) ||
                         strcmp(pt.language, "ko") == 0 ? "h12" : "h23";
    if (h12_pres) {
        snprintf(d->hour_cycle, sizeof(d->hour_cycle), "%s", h12_val ? "h12" : "h23");
    } else if (hc_pres) {
        snprintf(d->hour_cycle, sizeof(d->hour_cycle), "%s", hc_opt);
    } else if (pt.u_hc[0] && (strcmp(pt.u_hc, "h11") == 0 || strcmp(pt.u_hc, "h12") == 0 ||
                              strcmp(pt.u_hc, "h23") == 0 || strcmp(pt.u_hc, "h24") == 0)) {
        snprintf(d->hour_cycle, sizeof(d->hour_cycle), "%.15s", pt.u_hc);
    } else {
        snprintf(d->hour_cycle, sizeof(d->hour_cycle), "%s", def_hc);
    }
    d->hour12 = (strcmp(d->hour_cycle, "h11") == 0 || strcmp(d->hour_cycle, "h12") == 0);

    char tz_raw[64];
    bool tz_pres = false;
    if (!intl_get_opt_str(vm, opts, "timeZone", NULL, 0, "UTC", tz_raw, sizeof(tz_raw), &tz_pres)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    if (tz_pres) {
        if (!intl_canonicalize_timezone(tz_raw, d->time_zone, sizeof(d->time_zone), &d->tz_offset_min)) {
            ajs_temp_root_restore(vm, rs);
            ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid timeZone '%s'", tz_raw);
            return false;
        }
    } else {
        snprintf(d->time_zone, sizeof(d->time_zone), "UTC");
        d->tz_offset_min = 0;
    }

    /* Component options */
    static const char *const k_wd[3]  = { "narrow", "short", "long" };
    static const char *const k_yr[2]  = { "2-digit", "numeric" };
    static const char *const k_mo[5]  = { "2-digit", "numeric", "narrow", "short", "long" };
    static const char *const k_tzn[6] = { "short", "long", "shortOffset", "longOffset", "shortGeneric", "longGeneric" };
    static const char *const k_fm[2]  = { "basic", "best fit" };
    static const char *const k_ds[4]  = { "full", "long", "medium", "short" };

    bool p_wd = false, p_yr = false, p_mo = false, p_dy = false, p_hr = false, p_mn = false, p_sc = false, p_tzn = false;
    /* Store timeZoneName option in d->disp_type */
    if (!intl_get_opt_str(vm, opts, "weekday", k_wd, 3, "", d->weekday_opt, sizeof(d->weekday_opt), &p_wd) ||
        !intl_get_opt_str(vm, opts, "year",    k_yr, 2, "", d->year_opt,    sizeof(d->year_opt),    &p_yr) ||
        !intl_get_opt_str(vm, opts, "month",   k_mo, 5, "", d->month_opt,   sizeof(d->month_opt),   &p_mo) ||
        !intl_get_opt_str(vm, opts, "day",     k_yr, 2, "", d->day_opt,     sizeof(d->day_opt),     &p_dy) ||
        !intl_get_opt_str(vm, opts, "hour",    k_yr, 2, "", d->hour_opt,    sizeof(d->hour_opt),    &p_hr) ||
        !intl_get_opt_str(vm, opts, "minute",  k_yr, 2, "", d->minute_opt,  sizeof(d->minute_opt),  &p_mn) ||
        !intl_get_opt_str(vm, opts, "second",  k_yr, 2, "", d->second_opt,  sizeof(d->second_opt),  &p_sc) ||
        !intl_get_opt_str(vm, opts, "timeZoneName", k_tzn, 6, "", d->disp_type, sizeof(d->disp_type), &p_tzn)) {
        ajs_temp_root_restore(vm, rs); return false;
    }
    char fm_dummy[16];
    if (!intl_get_opt_str(vm, opts, "formatMatcher", k_fm, 2, "best fit", fm_dummy, sizeof(fm_dummy), NULL)) {
        ajs_temp_root_restore(vm, rs); return false;
    }

    bool p_ds = false, p_ts = false;
    if (!intl_get_opt_str(vm, opts, "dateStyle", k_ds, 4, "", d->date_style, sizeof(d->date_style), &p_ds) ||
        !intl_get_opt_str(vm, opts, "timeStyle", k_ds, 4, "", d->time_style, sizeof(d->time_style), &p_ts)) {
        ajs_temp_root_restore(vm, rs); return false;
    }

    bool any_comp = p_wd || p_yr || p_mo || p_dy || p_hr || p_mn || p_sc || p_tzn;
    if ((p_ds || p_ts) && any_comp) {
        ajs_temp_root_restore(vm, rs);
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Can't set option with dateStyle or timeStyle");
        return false;
    }

    if (!p_ds && !p_ts && !any_comp) {
        if (def_mode == 0 || def_mode == 1) {
            snprintf(d->year_opt, sizeof(d->year_opt), "numeric");
            snprintf(d->month_opt, sizeof(d->month_opt), "numeric");
            snprintf(d->day_opt, sizeof(d->day_opt), "numeric");
        }
        if (def_mode == 1 || def_mode == 2) {
            snprintf(d->hour_opt, sizeof(d->hour_opt), "numeric");
            snprintf(d->minute_opt, sizeof(d->minute_opt), "2-digit");
            snprintf(d->second_opt, sizeof(d->second_opt), "2-digit");
        }
    }

    d->has_hour12 = (d->hour_opt[0] != '\0' || d->time_style[0] != '\0');

    snprintf(d->language, sizeof(d->language), "%s", pt.language);
    snprintf(d->script, sizeof(d->script), "%s", pt.script);
    snprintf(d->region, sizeof(d->region), "%s", pt.region);
    format_parsed_tag(&pt, NULL, 0, d->locale, sizeof(d->locale));

    ajs_temp_root_restore(vm, rs);
    return true;
}

static const char *const k_en_months_long[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
static const char *const k_en_months_short[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char *const k_en_wd_long[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char *const k_en_wd_short[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const k_ko_wd_long[7] = {
    "\xEC\x9D\xBC\xEC\x9A\x94\xEC\x9D\xBC", "\xEC\x9B\x94\xEC\x9A\x94\xEC\x9D\xBC",
    "\xED\x99\x94\xEC\x9A\x94\xEC\x9D\xBC", "\xEC\x88\x98\xEC\x9A\x94\xEC\x9D\xBC",
    "\xEB\xAA\xA9\xEC\x9A\x94\xEC\x9D\xBC", "\xEA\xB8\x88\xEC\x9A\x94\xEC\x9D\xBC",
    "\xED\x86\xA0\xEC\x9A\x94\xEC\x9D\xBC"
};
static const char *const k_ko_wd_short[7] = {
    "\xEC\x9D\xBC", "\xEC\x9B\x94", "\xED\x99\x94", "\xEC\x88\x98", "\xEB\xAA\xA9", "\xEA\xB8\x88", "\xED\x86\xA0"
};

static bool intl_format_dt_parts(AjsVm *vm, const AjsIntlData *d, AjsValue date_val, IntlNumParts *np) {
    memset(np, 0, sizeof(*np));
    double ms = 0.0;
    if (ajs_is_undefined(date_val)) {
        ms = (double)time(NULL) * 1000.0;
    } else {
        if (!ajs_to_number(vm, date_val, &ms)) return false;
    }
    /* ECMA-262 TimeClip: |ms| <= 8.64e15 */
    if (!isfinite(ms) || fabs(ms) > 8.64e15) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid time value");
        return false;
    }
    int64_t epoch_ms = (int64_t)trunc(ms);
    bool is_dst = false;
    int32_t eff_off = intl_effective_tz_offset(d->time_zone, d->tz_offset_min, epoch_ms, &is_dst);
    IntlCivilDate cd;
    intl_decompose_epoch_ms(epoch_ms, eff_off, &cd);

    /* Expand dateStyle / timeStyle into effective component specifiers */
    const char *w_opt  = d->weekday_opt;
    const char *y_opt  = d->year_opt;
    const char *m_opt  = d->month_opt;
    const char *d_opt  = d->day_opt;
    const char *hr_opt = d->hour_opt;
    const char *mn_opt = d->minute_opt;
    const char *sc_opt = d->second_opt;
    const char *tz_opt = d->disp_type;

    if (d->date_style[0]) {
        y_opt = "numeric";
        d_opt = "numeric";
        if (strcmp(d->date_style, "full") == 0)        { w_opt = "long"; m_opt = "long"; }
        else if (strcmp(d->date_style, "long") == 0)   { m_opt = "long"; }
        else if (strcmp(d->date_style, "medium") == 0) { m_opt = "short"; }
        else                                           { m_opt = "numeric"; }
    }
    if (d->time_style[0]) {
        hr_opt = "numeric";
        mn_opt = "2-digit";
        if (strcmp(d->time_style, "short") != 0) sc_opt = "2-digit";
        if (strcmp(d->time_style, "full") == 0 || strcmp(d->time_style, "long") == 0) tz_opt = "short";
    }

    bool ko  = intl_lang_is(d->locale, "ko");
    bool ja  = intl_lang_is(d->locale, "ja");
    bool zh  = intl_lang_is(d->locale, "zh");
    bool iso = (strcmp(d->calendar, "iso8601") == 0);
    bool dmy = (!ko && !ja && !zh && !iso &&
                !(intl_lang_is(d->locale, "en") && (!d->region[0] || strcmp(d->region, "US") == 0)));

    bool has_date = (w_opt[0] || y_opt[0] || m_opt[0] || d_opt[0]);
    bool has_time = (hr_opt[0] || mn_opt[0] || sc_opt[0]);

    char ybuf[24], mbuf[24], dbuf[16];
    int64_t disp_y = cd.year <= 0 ? (1 - cd.year) : cd.year;
    if (strcmp(y_opt, "2-digit") == 0) snprintf(ybuf, sizeof(ybuf), "%02d", (int)(disp_y % 100));
    else                               snprintf(ybuf, sizeof(ybuf), "%lld", (long long)cd.year);

    if (strcmp(m_opt, "2-digit") == 0)      snprintf(mbuf, sizeof(mbuf), "%02d", cd.month);
    else if (strcmp(m_opt, "numeric") == 0) snprintf(mbuf, sizeof(mbuf), "%d", cd.month);
    else if (ko)                            snprintf(mbuf, sizeof(mbuf), "%d\xEC\x9B\x94", cd.month);
    else if (ja || zh)                      snprintf(mbuf, sizeof(mbuf), "%d\xE6\x9C\x88", cd.month);
    else if (strcmp(m_opt, "long") == 0)    snprintf(mbuf, sizeof(mbuf), "%s", k_en_months_long[cd.month - 1]);
    else if (strcmp(m_opt, "short") == 0)   snprintf(mbuf, sizeof(mbuf), "%s", k_en_months_short[cd.month - 1]);
    else                                    snprintf(mbuf, sizeof(mbuf), "%c", k_en_months_long[cd.month - 1][0]);

    if (strcmp(d_opt, "2-digit") == 0) snprintf(dbuf, sizeof(dbuf), "%02d", cd.day);
    else                               snprintf(dbuf, sizeof(dbuf), "%d", cd.day);

    if (has_date) {
        if (iso) {
            if (y_opt[0]) {
                char iy[24]; snprintf(iy, sizeof(iy), "%04lld", (long long)cd.year);
                np_push(np, "year", iy);
            }
            if (m_opt[0]) {
                if (y_opt[0]) np_push(np, "literal", "-");
                char im[8]; snprintf(im, sizeof(im), "%02d", cd.month);
                np_push(np, "month", im);
            }
            if (d_opt[0]) {
                if (y_opt[0] || m_opt[0]) np_push(np, "literal", "-");
                char id[8]; snprintf(id, sizeof(id), "%02d", cd.day);
                np_push(np, "day", id);
            }
        } else if (ko) {
            bool word_style = (strcmp(m_opt, "long") == 0 || strcmp(m_opt, "short") == 0);
            if (y_opt[0]) {
                np_push(np, "year", ybuf);
                np_push(np, "literal", word_style ? "\xEB\x85\x84 " : ". ");
            }
            if (m_opt[0]) {
                if (word_style) {
                    char km[16]; snprintf(km, sizeof(km), "%d", cd.month);
                    np_push(np, "month", km);
                    np_push(np, "literal", "\xEC\x9B\x94 ");
                } else {
                    np_push(np, "month", mbuf);
                    np_push(np, "literal", d_opt[0] ? ". " : ".");
                }
            }
            if (d_opt[0]) {
                np_push(np, "day", dbuf);
                np_push(np, "literal", word_style ? "\xEC\x9D\xBC" : ".");
            }
            if (w_opt[0]) {
                if (y_opt[0] || m_opt[0] || d_opt[0]) np_push(np, "literal", " ");
                np_push(np, "weekday", strcmp(w_opt, "long") == 0 ? k_ko_wd_long[cd.weekday] : k_ko_wd_short[cd.weekday]);
            }
        } else if (ja || zh) {
            bool word_style = (strcmp(m_opt, "long") == 0 || strcmp(m_opt, "short") == 0);
            if (y_opt[0]) {
                np_push(np, "year", ybuf);
                np_push(np, "literal", word_style ? "\xE5\xB9\xB4" : "/");
            }
            if (m_opt[0]) {
                char jm[16]; snprintf(jm, sizeof(jm), "%d", cd.month);
                np_push(np, "month", jm);
                if (word_style) np_push(np, "literal", "\xE6\x9C\x88");
                else if (d_opt[0]) np_push(np, "literal", "/");
            }
            if (d_opt[0]) {
                np_push(np, "day", dbuf);
                if (word_style) np_push(np, "literal", "\xE6\x97\xA5");
            }
        } else {
            if (w_opt[0]) {
                np_push(np, "weekday", strcmp(w_opt, "long") == 0 ? k_en_wd_long[cd.weekday] : k_en_wd_short[cd.weekday]);
                if (y_opt[0] || m_opt[0] || d_opt[0]) np_push(np, "literal", ", ");
            }
            bool text_m = (strcmp(m_opt, "long") == 0 || strcmp(m_opt, "short") == 0 || strcmp(m_opt, "narrow") == 0);
            if (dmy) {
                const char *sep = intl_lang_is(d->locale, "de") ? "." : "/";
                if (d_opt[0]) np_push(np, "day", dbuf);
                if (m_opt[0]) {
                    if (d_opt[0]) np_push(np, "literal", text_m ? " " : sep);
                    np_push(np, "month", mbuf);
                }
                if (y_opt[0]) {
                    if (d_opt[0] || m_opt[0]) np_push(np, "literal", text_m ? " " : sep);
                    np_push(np, "year", ybuf);
                }
            } else {
                if (m_opt[0]) np_push(np, "month", mbuf);
                if (d_opt[0]) {
                    if (m_opt[0]) np_push(np, "literal", text_m ? " " : "/");
                    np_push(np, "day", dbuf);
                }
                if (y_opt[0]) {
                    if (m_opt[0] || d_opt[0]) np_push(np, "literal", text_m ? ", " : "/");
                    np_push(np, "year", ybuf);
                }
            }
        }
    }

    if (has_date && has_time) {
        np_push(np, "literal", (ko || ja || zh || iso) ? " " : ", ");
    }

    if (has_time) {
        bool is_pm = (cd.hour >= 12);
        int disp_hr = cd.hour;
        if (strcmp(d->hour_cycle, "h12") == 0) {
            disp_hr = cd.hour % 12;
            if (disp_hr == 0) disp_hr = 12;
        } else if (strcmp(d->hour_cycle, "h11") == 0) {
            disp_hr = cd.hour % 12;
        } else if (strcmp(d->hour_cycle, "h24") == 0) {
            disp_hr = (cd.hour == 0) ? 24 : cd.hour;
        }

        if (d->hour12 && (ko || ja || zh)) {
            const char *dp = ko ? (is_pm ? "\xEC\x98\xA4\xED\x9B\x84" : "\xEC\x98\xA4\xEC\xA0\x84")
                                : (is_pm ? "\xE5\x8D\x88\xE5\xBE\x8C" : "\xE5\x8D\x88\xE5\x89\x8D");
            np_push(np, "dayPeriod", dp);
            if (ko) np_push(np, "literal", " ");
        }

        if (hr_opt[0]) {
            char hb[8];
            if (strcmp(hr_opt, "2-digit") == 0 || (!d->hour12 && mn_opt[0])) snprintf(hb, sizeof(hb), "%02d", disp_hr);
            else snprintf(hb, sizeof(hb), "%d", disp_hr);
            np_push(np, "hour", hb);
        }
        if (mn_opt[0]) {
            if (hr_opt[0]) np_push(np, "literal", ":");
            char mb[8];
            if (strcmp(mn_opt, "2-digit") == 0 || hr_opt[0]) snprintf(mb, sizeof(mb), "%02d", cd.minute);
            else snprintf(mb, sizeof(mb), "%d", cd.minute);
            np_push(np, "minute", mb);
        }
        if (sc_opt[0]) {
            if (hr_opt[0] || mn_opt[0]) np_push(np, "literal", ":");
            char sb[8];
            if (strcmp(sc_opt, "2-digit") == 0 || mn_opt[0]) snprintf(sb, sizeof(sb), "%02d", cd.second);
            else snprintf(sb, sizeof(sb), "%d", cd.second);
            np_push(np, "second", sb);
        }

        if (d->hour12 && !(ko || ja || zh)) {
            np_push(np, "literal", " ");
            np_push(np, "dayPeriod", is_pm ? "PM" : "AM");
        }
    }

    if (tz_opt[0]) {
        if (has_date || has_time) np_push(np, "literal", " ");
        char tzb[32];
        if (strcmp(d->time_zone, "UTC") == 0) {
            snprintf(tzb, sizeof(tzb), "UTC");
        } else if (strcmp(d->time_zone, "Asia/Seoul") == 0 && strcmp(tz_opt, "shortOffset") != 0) {
            snprintf(tzb, sizeof(tzb), "KST");
        } else if (strcmp(d->time_zone, "Asia/Tokyo") == 0 && strcmp(tz_opt, "shortOffset") != 0) {
            snprintf(tzb, sizeof(tzb), "JST");
        } else if (strcmp(d->time_zone, "America/New_York") == 0 && strcmp(tz_opt, "shortOffset") != 0) {
            snprintf(tzb, sizeof(tzb), "%s", is_dst ? "EDT" : "EST");
        } else if (strcmp(d->time_zone, "America/Los_Angeles") == 0 && strcmp(tz_opt, "shortOffset") != 0) {
            snprintf(tzb, sizeof(tzb), "%s", is_dst ? "PDT" : "PST");
        } else {
            int aoff = abs(eff_off);
            if (aoff == 0) snprintf(tzb, sizeof(tzb), "GMT");
            else if (aoff % 60 == 0 && strcmp(tz_opt, "longOffset") != 0)
                snprintf(tzb, sizeof(tzb), "GMT%c%d", eff_off < 0 ? '-' : '+', aoff / 60);
            else
                snprintf(tzb, sizeof(tzb), "GMT%c%02d:%02d", eff_off < 0 ? '-' : '+', aoff / 60, aoff % 60);
        }
        np_push(np, "timeZoneName", tzb);
    }

    return true;
}

static AjsValue js_intl_dtfmt_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    size_t rs = ajs_temp_root_save(vm);
    AjsValue proto = intl_proto_from_new_target(vm, nt, vm->active_realm->intl_dtfmt_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, proto);

    AjsObject *obj = intl_alloc_instance(vm, vm->active_realm, AJS_INTL_DATETIME_FORMAT, proto);
    if (!obj) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(obj));

    if (!intl_init_dtfmt_data(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED,
                              argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, 0, obj->u.intl)) {
        ajs_temp_root_restore(vm, rs);
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(obj);
}

static AjsValue js_intl_dtfmt_format_bound(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_DATETIME_FORMAT, "Intl.DateTimeFormat");
    if (!d) return AJS_VAL_EXCEPTION;
    IntlNumParts np;
    if (!intl_format_dt_parts(vm, d, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &np)) {
        return AJS_VAL_EXCEPTION;
    }
    AjsString *s = intl_join_num_parts(vm, &np);
    return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
}

static AjsValue js_intl_dtfmt_format_get(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_DATETIME_FORMAT, "Intl.DateTimeFormat");
    if (!d) return AJS_VAL_EXCEPTION;
    if (ajs_is_undefined(d->bound_fn)) {
        /* ECMA-402 §11.3.3: DateTimeFormat bound format function has length = 1 */
        d->bound_fn = intl_make_bound_fn(vm, vm->active_realm, js_intl_dtfmt_format_bound, 1, this_val);
    }
    return d->bound_fn;
}

static AjsValue js_intl_dtfmt_formatToParts(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_DATETIME_FORMAT, "Intl.DateTimeFormat");
    if (!d) return AJS_VAL_EXCEPTION;
    IntlNumParts np;
    if (!intl_format_dt_parts(vm, d, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &np)) {
        return AJS_VAL_EXCEPTION;
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue arr = ajs_new_array(vm, (uint32_t)np.count);
    ajs_temp_root_push(vm, arr);
    uint32_t idx = 0;
    for (int i = 0; i < np.count; i++) {
        if (!intl_push_part_ex(vm, arr, &idx, np.types[i], np.vals[i], NULL)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    ajs_temp_root_restore(vm, rs);
    return arr;
}

static AjsValue js_intl_dtfmt_resolvedOptions(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_DATETIME_FORMAT, "Intl.DateTimeFormat");
    if (!d) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue res = ajs_new_object(vm);
    ajs_temp_root_push(vm, res);
    ajs_define_prop_str(vm, res, "locale", ajs_val_from_gc(ajs_str_from_cstr(vm, d->locale, false)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "calendar", ajs_val_from_gc(ajs_str_from_cstr(vm, d->calendar, true)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "numberingSystem", ajs_val_from_gc(ajs_str_from_cstr(vm, d->numbering_system, true)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "timeZone", ajs_val_from_gc(ajs_str_from_cstr(vm, d->time_zone, false)), AJS_PROP_DEFAULT);
    if (d->has_hour12) {
        ajs_define_prop_str(vm, res, "hourCycle", ajs_val_from_gc(ajs_str_from_cstr(vm, d->hour_cycle, true)), AJS_PROP_DEFAULT);
        ajs_define_prop_str(vm, res, "hour12", d->hour12 ? AJS_VAL_TRUE : AJS_VAL_FALSE, AJS_PROP_DEFAULT);
    }
    if (d->weekday_opt[0]) ajs_define_prop_str(vm, res, "weekday", ajs_val_from_gc(ajs_str_from_cstr(vm, d->weekday_opt, true)), AJS_PROP_DEFAULT);
    if (d->year_opt[0])    ajs_define_prop_str(vm, res, "year",    ajs_val_from_gc(ajs_str_from_cstr(vm, d->year_opt, true)), AJS_PROP_DEFAULT);
    if (d->month_opt[0])   ajs_define_prop_str(vm, res, "month",   ajs_val_from_gc(ajs_str_from_cstr(vm, d->month_opt, true)), AJS_PROP_DEFAULT);
    if (d->day_opt[0])     ajs_define_prop_str(vm, res, "day",     ajs_val_from_gc(ajs_str_from_cstr(vm, d->day_opt, true)), AJS_PROP_DEFAULT);
    if (d->hour_opt[0])    ajs_define_prop_str(vm, res, "hour",    ajs_val_from_gc(ajs_str_from_cstr(vm, d->hour_opt, true)), AJS_PROP_DEFAULT);
    if (d->minute_opt[0])  ajs_define_prop_str(vm, res, "minute",  ajs_val_from_gc(ajs_str_from_cstr(vm, d->minute_opt, true)), AJS_PROP_DEFAULT);
    if (d->second_opt[0])  ajs_define_prop_str(vm, res, "second",  ajs_val_from_gc(ajs_str_from_cstr(vm, d->second_opt, true)), AJS_PROP_DEFAULT);
    if (d->disp_type[0])   ajs_define_prop_str(vm, res, "timeZoneName", ajs_val_from_gc(ajs_str_from_cstr(vm, d->disp_type, true)), AJS_PROP_DEFAULT);
    if (d->date_style[0])  ajs_define_prop_str(vm, res, "dateStyle", ajs_val_from_gc(ajs_str_from_cstr(vm, d->date_style, true)), AJS_PROP_DEFAULT);
    if (d->time_style[0])  ajs_define_prop_str(vm, res, "timeStyle", ajs_val_from_gc(ajs_str_from_cstr(vm, d->time_style, true)), AJS_PROP_DEFAULT);
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_date_proto_toLocale_intl(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Date.prototype.toLocaleString called on non-Date");
    }
    AjsObject *o = (AjsObject *)ajs_val_to_gc(this_val);
    if (o->kind != AJS_OBJ_DATE) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Date.prototype.toLocaleString called on non-Date");
    }
    double ms = o->u.date_ms;
    if (!isfinite(ms)) {
        return ajs_val_from_gc(ajs_str_from_cstr(vm, "Invalid Date", true));
    }
    int32_t def_mode = 1; /* 1=ALL, 0=DATE, 2=TIME */
    if (vm->call_depth > 0 && vm->call_stack[vm->call_depth - 1u].native_fn_obj) {
        def_mode = vm->call_stack[vm->call_depth - 1u].native_fn_obj->u.fn.native_magic;
    }
    AjsIntlData dd;
    memset(&dd, 0, sizeof(dd));
    dd.kind = AJS_INTL_DATETIME_FORMAT;
    if (!intl_init_dtfmt_data(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED,
                              argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, (int)def_mode, &dd)) {
        return AJS_VAL_EXCEPTION;
    }
    IntlNumParts np;
    if (!intl_format_dt_parts(vm, &dd, ajs_val_number(ms), &np)) return AJS_VAL_EXCEPTION;
    AjsString *s = intl_join_num_parts(vm, &np);
    return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
}

/* ============================================================================
 * 7. Intl.PluralRules (ECMA-402 §16)
 * ============================================================================ */

static const char *intl_plural_select_impl(const AjsIntlData *d, double num) {
    if (!isfinite(num)) return "other";
    double abs_n = fabs(num);
    bool is_ord = (strcmp(d->plural_type, "ordinal") == 0);

    if (intl_lang_is(d->locale, "ko") || intl_lang_is(d->locale, "ja") || intl_lang_is(d->locale, "zh") ||
        intl_lang_is(d->locale, "vi") || intl_lang_is(d->locale, "th") || intl_lang_is(d->locale, "id")) {
        return "other";
    }

    /* Compute CLDR plural operands: i (integer digits), v (visible fraction digit count) */
    int prec = d->max_frac_digits > 20 ? 20 : d->max_frac_digits;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", prec, abs_n);
    char *dot = strchr(buf, '.');
    int v = 0;
    if (dot) {
        size_t fl = strlen(dot + 1);
        while ((int)fl > d->min_frac_digits && fl > 0 && dot[fl] == '0') {
            dot[fl--] = '\0';
        }
        v = (int)fl;
        *dot = '\0';
    }
    uint64_t i_val = (uint64_t)strtoull(buf, NULL, 10);

    if (is_ord) {
        if (intl_lang_is(d->locale, "en")) {
            if (v == 0) {
                uint64_t m10 = i_val % 10u, m100 = i_val % 100u;
                if (m10 == 1u && m100 != 11u) return "one";
                if (m10 == 2u && m100 != 12u) return "two";
                if (m10 == 3u && m100 != 13u) return "few";
            }
            return "other";
        }
        if (intl_lang_is(d->locale, "fr") && v == 0 && i_val == 1u) return "one";
        return "other";
    }

    /* Cardinal rules */
    if (intl_lang_is(d->locale, "fr") || intl_lang_is(d->locale, "pt")) {
        return (i_val == 0u || i_val == 1u) ? "one" : "other";
    }
    if (intl_lang_is(d->locale, "ru")) {
        if (v == 0) {
            uint64_t m10 = i_val % 10u, m100 = i_val % 100u;
            if (m10 == 1u && m100 != 11u) return "one";
            if (m10 >= 2u && m10 <= 4u && (m100 < 12u || m100 > 14u)) return "few";
            if (m10 == 0u || (m10 >= 5u && m10 <= 9u) || (m100 >= 11u && m100 <= 14u)) return "many";
        }
        return "other";
    }
    if (intl_lang_is(d->locale, "ar")) {
        if (abs_n == 0.0) return "zero";
        if (v == 0) {
            if (i_val == 1u) return "one";
            if (i_val == 2u) return "two";
            uint64_t m100 = i_val % 100u;
            if (m100 >= 3u && m100 <= 10u) return "few";
            if (m100 >= 11u && m100 <= 99u) return "many";
        }
        return "other";
    }
    /* Default (en, de, es, it, nl, sv, etc.): i = 1 and v = 0 -> "one", else "other" */
    return (i_val == 1u && v == 0) ? "one" : "other";
}

static AjsValue js_intl_plural_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (ajs_is_undefined(nt)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Constructor Intl.PluralRules requires 'new'");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue proto = intl_proto_from_new_target(vm, nt, vm->active_realm->intl_plural_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, proto);

    AjsValue opts = AJS_VAL_UNDEFINED;
    if (!intl_coerce_options_obj(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, &opts)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_push(vm, opts);

    AjsObject *obj = intl_alloc_instance(vm, vm->active_realm, AJS_INTL_PLURAL_RULES, proto);
    if (!obj) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(obj));
    AjsIntlData *d = obj->u.intl;

    IntlParsedTag pt;
    char full_loc[64];
    if (!intl_resolve_locale(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, opts, &pt, full_loc, sizeof(full_loc))) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    format_parsed_tag(&pt, NULL, 0, d->locale, sizeof(d->locale));
    snprintf(d->language, sizeof(d->language), "%s", pt.language);

    static const char *const k_ptypes[2] = { "cardinal", "ordinal" };
    if (!intl_get_opt_str(vm, opts, "type", k_ptypes, 2, "cardinal", d->plural_type, sizeof(d->plural_type), NULL)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }

    int min_id = 1, min_fd = 0, max_fd = 3;
    bool has_min_fd = false, has_max_fd = false;
    if (!intl_get_opt_int(vm, opts, "minimumIntegerDigits", 1, 21, 1, &min_id, NULL) ||
        !intl_get_opt_int(vm, opts, "minimumFractionDigits", 0, 100, 0, &min_fd, &has_min_fd)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    int def_max = (has_min_fd && min_fd > 3) ? min_fd : 3;
    if (!intl_get_opt_int(vm, opts, "maximumFractionDigits", 0, 100, def_max, &max_fd, &has_max_fd)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    if (min_fd > max_fd) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "minimumFractionDigits > maximumFractionDigits");
    }
    d->min_int_digits = min_id;
    d->min_frac_digits = min_fd;
    d->max_frac_digits = max_fd;

    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(obj);
}

static AjsValue js_intl_plural_select(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_PLURAL_RULES, "Intl.PluralRules");
    if (!d) return AJS_VAL_EXCEPTION;
    double n = 0.0;
    if (!ajs_to_number(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &n)) return AJS_VAL_EXCEPTION;
    return ajs_val_from_gc(ajs_str_from_cstr(vm, intl_plural_select_impl(d, n), true));
}

static AjsValue js_intl_plural_selectRange(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_PLURAL_RULES, "Intl.PluralRules");
    if (!d) return AJS_VAL_EXCEPTION;
    if (argc < 2 || ajs_is_undefined(argv[0]) || ajs_is_undefined(argv[1])) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "selectRange requires start and end");
    }
    double a = 0.0, b = 0.0;
    if (!ajs_to_number(vm, argv[0], &a) || !ajs_to_number(vm, argv[1], &b)) return AJS_VAL_EXCEPTION;
    if (isnan(a) || isnan(b)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid NaN argument to selectRange");
    }
    return ajs_val_from_gc(ajs_str_from_cstr(vm, intl_plural_select_impl(d, b), true));
}

static AjsValue js_intl_plural_resolvedOptions(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_PLURAL_RULES, "Intl.PluralRules");
    if (!d) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue res = ajs_new_object(vm);
    ajs_temp_root_push(vm, res);
    ajs_define_prop_str(vm, res, "locale", ajs_val_from_gc(ajs_str_from_cstr(vm, d->locale, false)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "type", ajs_val_from_gc(ajs_str_from_cstr(vm, d->plural_type, true)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "minimumIntegerDigits", ajs_val_number((double)d->min_int_digits), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "minimumFractionDigits", ajs_val_number((double)d->min_frac_digits), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "maximumFractionDigits", ajs_val_number((double)d->max_frac_digits), AJS_PROP_DEFAULT);

    const char *cats[6];
    int ccnt = 0;
    bool is_ord = (strcmp(d->plural_type, "ordinal") == 0);
    if (intl_lang_is(d->locale, "ko") || intl_lang_is(d->locale, "ja") || intl_lang_is(d->locale, "zh")) {
        cats[ccnt++] = "other";
    } else if (is_ord && intl_lang_is(d->locale, "en")) {
        cats[ccnt++] = "one"; cats[ccnt++] = "two"; cats[ccnt++] = "few"; cats[ccnt++] = "other";
    } else if (!is_ord && intl_lang_is(d->locale, "ru")) {
        cats[ccnt++] = "one"; cats[ccnt++] = "few"; cats[ccnt++] = "many"; cats[ccnt++] = "other";
    } else if (!is_ord && intl_lang_is(d->locale, "ar")) {
        cats[ccnt++] = "zero"; cats[ccnt++] = "one"; cats[ccnt++] = "two";
        cats[ccnt++] = "few"; cats[ccnt++] = "many"; cats[ccnt++] = "other";
    } else {
        cats[ccnt++] = "one"; cats[ccnt++] = "other";
    }
    AjsValue carr = ajs_new_array(vm, (uint32_t)ccnt);
    ajs_temp_root_push(vm, carr);
    for (int i = 0; i < ccnt; i++) {
        ajs_set_prop_index(vm, carr, (uint32_t)i, ajs_val_from_gc(ajs_str_from_cstr(vm, cats[i], true)));
    }
    ajs_define_prop_str(vm, res, "pluralCategories", carr, AJS_PROP_DEFAULT);
    ajs_temp_root_restore(vm, rs);
    return res;
}

/* ============================================================================
 * 8. Intl.RelativeTimeFormat (ECMA-402 §17)
 * ============================================================================ */

static const char *intl_singular_rel_unit(const char *u) {
    if (strcmp(u, "second") == 0  || strcmp(u, "seconds") == 0)  return "second";
    if (strcmp(u, "minute") == 0  || strcmp(u, "minutes") == 0)  return "minute";
    if (strcmp(u, "hour") == 0    || strcmp(u, "hours") == 0)    return "hour";
    if (strcmp(u, "day") == 0     || strcmp(u, "days") == 0)     return "day";
    if (strcmp(u, "week") == 0    || strcmp(u, "weeks") == 0)    return "week";
    if (strcmp(u, "month") == 0   || strcmp(u, "months") == 0)   return "month";
    if (strcmp(u, "quarter") == 0 || strcmp(u, "quarters") == 0) return "quarter";
    if (strcmp(u, "year") == 0    || strcmp(u, "years") == 0)    return "year";
    return NULL;
}

static bool intl_reltime_build(AjsVm *vm, const AjsIntlData *d, AjsValue val_arg, AjsValue unit_arg,
                               IntlNumParts *np, const char **out_unit_for_num) {
    memset(np, 0, sizeof(*np));
    *out_unit_for_num = NULL;
    double val = 0.0;
    if (!ajs_to_number(vm, val_arg, &val)) return false;
    AjsString *us = ajs_to_js_string(vm, unit_arg);
    if (vm->has_exception || !us) return false;
    const char *su = intl_singular_rel_unit(us->utf8);
    if (!su) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid unit '%s' for Intl.RelativeTimeFormat", us->utf8);
        return false;
    }
    if (!isfinite(val)) {
        ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Value need to be finite number for Intl.RelativeTimeFormat");
        return false;
    }

    bool ko = intl_lang_is(d->locale, "ko");
    /* d->numeric: true -> "always", false -> "auto" */
    if (!d->numeric && (val == -1.0 || val == 0.0 || val == 1.0)) {
        const char *auto_word = NULL;
        if (strcmp(su, "day") == 0) {
            if (val == -1.0)      auto_word = ko ? "\xEC\x96\xB4\xEC\xA0\x9C" : "yesterday";
            else if (val == 0.0)  auto_word = ko ? "\xEC\x98\xA4\xEB\x8A\x98" : "today";
            else if (val == 1.0)  auto_word = ko ? "\xEB\x82\xB4\xEC\x9D\xBC" : "tomorrow";
        } else if (strcmp(su, "second") == 0 && val == 0.0) {
            auto_word = ko ? "\xEC\xA7\x80\xEA\xB8\x88" : "now";
        } else if (strcmp(su, "year") == 0) {
            if (val == -1.0)      auto_word = ko ? "\xEC\x9E\x91\xEB\x85\x84" : "last year";
            else if (val == 0.0)  auto_word = ko ? "\xEC\x98\xAC\xED\x95\xB4" : "this year";
            else if (val == 1.0)  auto_word = ko ? "\xEB\x82\xB4\xEB\x85\x84" : "next year";
        } else if (strcmp(su, "month") == 0) {
            if (val == -1.0)      auto_word = ko ? "\xEC\xA7\x80\xEB\x82\x9C\xEB\x8B\xAC" : "last month";
            else if (val == 0.0)  auto_word = ko ? "\xEC\x9D\xB4\xEB\xB2\x88 \xEB\x8B\xAC" : "this month";
            else if (val == 1.0)  auto_word = ko ? "\xEB\x8B\xA4\xEC\x9D\x8C \xEB\x8B\xAC" : "next month";
        } else if (strcmp(su, "week") == 0) {
            if (val == -1.0)      auto_word = ko ? "\xEC\xA7\x80\xEB\x82\x9C\xEC\xA3\xBC" : "last week";
            else if (val == 0.0)  auto_word = ko ? "\xEC\x9D\xB4\xEB\xB2\x88 \xEC\xA3\xBC" : "this week";
            else if (val == 1.0)  auto_word = ko ? "\xEB\x8B\xA4\xEC\x9D\x8C \xEC\xA3\xBC" : "next week";
        }
        if (auto_word) {
            np_push(np, "literal", auto_word);
            return true;
        }
    }

    *out_unit_for_num = su;
    bool is_past = (signbit(val) != 0); /* -0 is past per ECMA-402! */
    double abs_v = fabs(val);

    AjsIntlData nd;
    memset(&nd, 0, sizeof(nd));
    nd.kind = AJS_INTL_NUMBER_FORMAT;
    snprintf(nd.locale, sizeof(nd.locale), "%s", d->locale);
    snprintf(nd.numbering_system, sizeof(nd.numbering_system), "%s", d->numbering_system);
    snprintf(nd.style, sizeof(nd.style), "decimal");
    snprintf(nd.notation, sizeof(nd.notation), "standard");
    snprintf(nd.sign_display, sizeof(nd.sign_display), "never");
    nd.use_grouping = true;
    nd.min_int_digits = 1;
    nd.min_frac_digits = 0;
    nd.max_frac_digits = 3;

    IntlNumParts num_p;
    if (!intl_format_number_parts(vm, &nd, ajs_val_number(abs_v), &num_p)) return false;

    bool is_one = (abs_v == 1.0);
    bool sh = (strcmp(d->style, "short") == 0);
    bool nr = (strcmp(d->style, "narrow") == 0);

    if (ko) {
        const char *ku = "\xEC\xB4\x88";
        if (strcmp(su, "minute") == 0)       ku = "\xEB\xB6\x84";
        else if (strcmp(su, "hour") == 0)    ku = "\xEC\x8B\x9C\xEA\xB0\x84";
        else if (strcmp(su, "day") == 0)     ku = "\xEC\x9D\xBC";
        else if (strcmp(su, "week") == 0)    ku = "\xEC\xA3\xBC";
        else if (strcmp(su, "month") == 0)   ku = "\xEA\xB0\x9C\xEC\x9B\x94";
        else if (strcmp(su, "quarter") == 0) ku = "\xEB\xB6\x84\xEA\xB8\xB0";
        else if (strcmp(su, "year") == 0)    ku = "\xEB\x85\x84";
        for (int i = 0; i < num_p.count; i++) np_push(np, num_p.types[i], num_p.vals[i]);
        char tail[32];
        snprintf(tail, sizeof(tail), "%s %s", ku, is_past ? "\xEC\xA0\x84" : "\xED\x9B\x84");
        np_push(np, "literal", tail);
    } else {
        const char *eu = su;
        if (nr) {
            if (strcmp(su, "second") == 0)       eu = "s";
            else if (strcmp(su, "minute") == 0)  eu = "m";
            else if (strcmp(su, "hour") == 0)    eu = "h";
            else if (strcmp(su, "day") == 0)     eu = "d";
            else if (strcmp(su, "week") == 0)    eu = "w";
            else if (strcmp(su, "month") == 0)   eu = "mo";
            else if (strcmp(su, "quarter") == 0) eu = "q";
            else if (strcmp(su, "year") == 0)    eu = "yr";
        } else if (sh) {
            if (strcmp(su, "second") == 0)       eu = "sec.";
            else if (strcmp(su, "minute") == 0)  eu = "min.";
            else if (strcmp(su, "hour") == 0)    eu = "hr.";
            else if (strcmp(su, "day") == 0)     eu = is_one ? "day" : "days";
            else if (strcmp(su, "week") == 0)    eu = "wk.";
            else if (strcmp(su, "month") == 0)   eu = "mo.";
            else if (strcmp(su, "quarter") == 0) eu = "qtr.";
            else if (strcmp(su, "year") == 0)    eu = "yr.";
        } else {
            static char plural_u[24];
            if (!is_one) { snprintf(plural_u, sizeof(plural_u), "%ss", su); eu = plural_u; }
        }

        if (!is_past) np_push(np, "literal", "in ");
        for (int i = 0; i < num_p.count; i++) np_push(np, num_p.types[i], num_p.vals[i]);
        char tail[32];
        if (is_past) snprintf(tail, sizeof(tail), "%s%s ago", nr ? "" : " ", eu);
        else         snprintf(tail, sizeof(tail), "%s%s", nr ? "" : " ", eu);
        np_push(np, "literal", tail);
    }
    return true;
}

static AjsValue js_intl_reltime_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (ajs_is_undefined(nt)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Constructor Intl.RelativeTimeFormat requires 'new'");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue proto = intl_proto_from_new_target(vm, nt, vm->active_realm->intl_reltime_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, proto);

    AjsValue opts = AJS_VAL_UNDEFINED;
    if (!intl_coerce_options_obj(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, &opts)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_push(vm, opts);

    AjsObject *obj = intl_alloc_instance(vm, vm->active_realm, AJS_INTL_RELATIVE_TIME_FORMAT, proto);
    if (!obj) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(obj));
    AjsIntlData *d = obj->u.intl;

    IntlParsedTag pt;
    char full_loc[64];
    if (!intl_resolve_locale(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, opts, &pt, full_loc, sizeof(full_loc))) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    format_parsed_tag(&pt, NULL, 0, d->locale, sizeof(d->locale));

    static const char *const k_styles[3] = { "long", "short", "narrow" };
    if (!intl_get_opt_str(vm, opts, "style", k_styles, 3, "long", d->style, sizeof(d->style), NULL)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    static const char *const k_num_modes[2] = { "always", "auto" };
    char num_mode[16];
    if (!intl_get_opt_str(vm, opts, "numeric", k_num_modes, 2, "always", num_mode, sizeof(num_mode), NULL)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    d->numeric = (strcmp(num_mode, "always") == 0);
    snprintf(d->numbering_system, sizeof(d->numbering_system), "latn");

    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(obj);
}

static AjsValue js_intl_reltime_format(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_RELATIVE_TIME_FORMAT, "Intl.RelativeTimeFormat");
    if (!d) return AJS_VAL_EXCEPTION;
    IntlNumParts np;
    const char *u_num = NULL;
    if (!intl_reltime_build(vm, d, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED,
                            argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, &np, &u_num)) {
        return AJS_VAL_EXCEPTION;
    }
    AjsString *s = intl_join_num_parts(vm, &np);
    return s ? ajs_val_from_gc(s) : AJS_VAL_EXCEPTION;
}

static AjsValue js_intl_reltime_formatToParts(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_RELATIVE_TIME_FORMAT, "Intl.RelativeTimeFormat");
    if (!d) return AJS_VAL_EXCEPTION;
    IntlNumParts np;
    const char *u_num = NULL;
    if (!intl_reltime_build(vm, d, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED,
                            argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, &np, &u_num)) {
        return AJS_VAL_EXCEPTION;
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue arr = ajs_new_array(vm, (uint32_t)np.count);
    ajs_temp_root_push(vm, arr);
    uint32_t idx = 0;
    for (int i = 0; i < np.count; i++) {
        const char *unit_tag = (u_num && strcmp(np.types[i], "literal") != 0) ? u_num : NULL;
        if (!intl_push_part_ex(vm, arr, &idx, np.types[i], np.vals[i], unit_tag)) {
            ajs_temp_root_restore(vm, rs);
            return AJS_VAL_EXCEPTION;
        }
    }
    ajs_temp_root_restore(vm, rs);
    return arr;
}

static AjsValue js_intl_reltime_resolvedOptions(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_RELATIVE_TIME_FORMAT, "Intl.RelativeTimeFormat");
    if (!d) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue res = ajs_new_object(vm);
    ajs_temp_root_push(vm, res);
    ajs_define_prop_str(vm, res, "locale", ajs_val_from_gc(ajs_str_from_cstr(vm, d->locale, false)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "style", ajs_val_from_gc(ajs_str_from_cstr(vm, d->style, true)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "numeric", ajs_val_from_gc(ajs_str_from_cstr(vm, d->numeric ? "always" : "auto", true)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "numberingSystem", ajs_val_from_gc(ajs_str_from_cstr(vm, d->numbering_system, true)), AJS_PROP_DEFAULT);
    ajs_temp_root_restore(vm, rs);
    return res;
}

/* ============================================================================
 * 9. Intl.Segmenter, %SegmentsPrototype% & %SegmentIteratorPrototype% (§18)
 * ============================================================================ */

#define INTL_SEG_ITER_TAG UINT64_C(0x5345474954455201)

static bool intl_is_grapheme_extend(uint32_t cp) {
    if ((cp >= 0x0300u && cp <= 0x036Fu) || (cp >= 0x0483u && cp <= 0x0489u) ||
        (cp >= 0x0591u && cp <= 0x05BDu) || (cp >= 0x064Bu && cp <= 0x065Fu) ||
        (cp >= 0x0E31u && cp <= 0x0E3Au) || (cp >= 0x0E47u && cp <= 0x0E4Eu) ||
        (cp >= 0x1AB0u && cp <= 0x1AFFu) || (cp >= 0x1DC0u && cp <= 0x1DFFu) ||
        (cp >= 0x20D0u && cp <= 0x20FFu) || (cp >= 0xFE00u && cp <= 0xFE0Fu) ||
        (cp >= 0xFE20u && cp <= 0xFE2Fu) || (cp >= 0xE0100u && cp <= 0xE01EFu) ||
        (cp >= 0x1F3FBu && cp <= 0x1F3FFu) || cp == 0x200Cu) {
        return true;
    }
    return false;
}

static bool intl_is_regional_indicator(uint32_t cp) {
    return (cp >= 0x1F1E6u && cp <= 0x1F1FFu);
}

static bool intl_is_hangul_jamo_l(uint32_t cp) {
    return (cp >= 0x1100u && cp <= 0x115Fu) || (cp >= 0xA960u && cp <= 0xA97Cu);
}
static bool intl_is_hangul_jamo_v(uint32_t cp) {
    return (cp >= 0x1160u && cp <= 0x11A7u) || (cp >= 0xD7B0u && cp <= 0xD7C6u);
}
static bool intl_is_hangul_jamo_t(uint32_t cp) {
    return (cp >= 0x11A8u && cp <= 0x11FFu) || (cp >= 0xD7CBu && cp <= 0xD7FBu);
}

static uint32_t intl_next_grapheme_end(const uint16_t *u, uint32_t len, uint32_t pos) {
    if (pos >= len) return len;
    if (u[pos] == '\r') {
        return (pos + 1u < len && u[pos + 1u] == '\n') ? (pos + 2u) : (pos + 1u);
    }
    if (u[pos] == '\n' || u[pos] < 0x20u) return pos + 1u;

    uint32_t adv = 1u;
    uint32_t cp = ajs_utf16_code_point_at(u, len, pos, &adv);
    uint32_t cur = pos + adv;

    /* Regional Indicator pair (flag emoji: two RI code points per cluster) */
    if (intl_is_regional_indicator(cp) && cur < len) {
        uint32_t adv2 = 1u;
        uint32_t cp2 = ajs_utf16_code_point_at(u, len, cur, &adv2);
        if (intl_is_regional_indicator(cp2)) cur += adv2;
    }

    /* Conjoining Hangul Jamo L* V+ T* */
    if (intl_is_hangul_jamo_l(cp)) {
        while (cur < len) {
            uint32_t a2 = 1u;
            uint32_t c2 = ajs_utf16_code_point_at(u, len, cur, &a2);
            if (!intl_is_hangul_jamo_l(c2)) break;
            cur += a2;
        }
        if (cur < len) {
            uint32_t a2 = 1u;
            uint32_t c2 = ajs_utf16_code_point_at(u, len, cur, &a2);
            if (intl_is_hangul_jamo_v(c2) || (c2 >= 0xAC00u && c2 <= 0xD7A3u)) {
                cur += a2;
                while (cur < len) {
                    uint32_t a3 = 1u;
                    uint32_t c3 = ajs_utf16_code_point_at(u, len, cur, &a3);
                    if (!intl_is_hangul_jamo_v(c3)) break;
                    cur += a3;
                }
                while (cur < len) {
                    uint32_t a3 = 1u;
                    uint32_t c3 = ajs_utf16_code_point_at(u, len, cur, &a3);
                    if (!intl_is_hangul_jamo_t(c3)) break;
                    cur += a3;
                }
            }
        }
    }

    /* Trailing Extend / SpacingMark / Skin-Tone Modifiers & ZWJ emoji sequences */
    while (cur < len) {
        uint32_t a2 = 1u;
        uint32_t c2 = ajs_utf16_code_point_at(u, len, cur, &a2);
        if (intl_is_grapheme_extend(c2)) {
            cur += a2;
            continue;
        }
        if (c2 == 0x200Du && cur + a2 < len) { /* ZWJ + following pictographic/base */
            uint32_t a3 = 1u;
            (void)ajs_utf16_code_point_at(u, len, cur + a2, &a3);
            cur += a2 + a3;
            continue;
        }
        break;
    }
    return cur;
}

static bool intl_is_cjk_ideograph(uint32_t cp) {
    return (cp >= 0x4E00u && cp <= 0x9FFFu) || (cp >= 0x3400u && cp <= 0x4DBFu) ||
           (cp >= 0x3040u && cp <= 0x30FFu) || (cp >= 0x20000u && cp <= 0x2CEAFu);
}

static bool intl_is_word_alnum(uint32_t cp) {
    if (intl_is_alnum((int)cp) || cp == '_') return true;
    if (cp >= 0x00C0u && cp <= 0x02AFu && cp != 0x00D7u && cp != 0x00F7u) return true;
    if ((cp >= 0x0370u && cp <= 0x052Fu) || (cp >= 0x0590u && cp <= 0x06FFu) ||
        (cp >= 0x0900u && cp <= 0x0DFFu)) return true;
    if ((cp >= 0xAC00u && cp <= 0xD7A3u) || (cp >= 0x1100u && cp <= 0x11FFu) ||
        (cp >= 0x3130u && cp <= 0x318Fu)) return true; /* Hangul */
    return ajs_unicode_is_id_continue(cp);
}

static uint32_t intl_next_word_end(const uint16_t *u, uint32_t len, uint32_t pos, bool *out_word_like) {
    *out_word_like = false;
    if (pos >= len) return len;
    uint32_t g_end = intl_next_grapheme_end(u, len, pos);
    uint32_t adv = 1u;
    uint32_t cp = ajs_utf16_code_point_at(u, len, pos, &adv);

    /* CJK Ideographs / Kana segment individually (or per character) as word-like */
    if (intl_is_cjk_ideograph(cp)) {
        *out_word_like = true;
        return g_end;
    }

    if (intl_is_word_alnum(cp)) {
        *out_word_like = true;
        uint32_t cur = g_end;
        bool is_num_run = (cp >= '0' && cp <= '9');
        while (cur < len) {
            uint32_t ng = intl_next_grapheme_end(u, len, cur);
            uint32_t a2 = 1u;
            uint32_t c2 = ajs_utf16_code_point_at(u, len, cur, &a2);
            if (intl_is_cjk_ideograph(c2)) break;
            if (intl_is_word_alnum(c2)) {
                cur = ng;
                continue;
            }
            /* MidLetter (' or ’) inside word, or MidNum (. or ,) inside number */
            if ((c2 == '\'' || c2 == 0x2019u || (is_num_run && (c2 == '.' || c2 == ','))) && ng < len) {
                uint32_t a3 = 1u;
                uint32_t c3 = ajs_utf16_code_point_at(u, len, ng, &a3);
                if (is_num_run ? (c3 >= '0' && c3 <= '9') : intl_is_word_alnum(c3)) {
                    cur = intl_next_grapheme_end(u, len, ng);
                    continue;
                }
            }
            break;
        }
        return cur;
    }

    /* Contiguous horizontal whitespace run */
    if (cp == ' ' || cp == '\t') {
        uint32_t cur = g_end;
        while (cur < len && (u[cur] == ' ' || u[cur] == '\t')) cur++;
        return cur;
    }

    return g_end;
}

static uint32_t intl_next_sentence_end(const uint16_t *u, uint32_t len, uint32_t pos) {
    if (pos >= len) return len;
    uint32_t cur = pos;
    while (cur < len) {
        uint32_t ng = intl_next_grapheme_end(u, len, cur);
        uint32_t a = 1u;
        uint32_t cp = ajs_utf16_code_point_at(u, len, cur, &a);
        cur = ng;
        if (cp == '\r' || cp == '\n') return cur;
        if (cp == '.' || cp == '!' || cp == '?' || cp == 0x3002u || cp == 0xFF01u || cp == 0xFF1Fu) {
            /* Consume closing quotes/brackets and trailing spaces on same line */
            while (cur < len && (u[cur] == '"' || u[cur] == '\'' || u[cur] == ')' || u[cur] == ']' ||
                                 u[cur] == 0x201Du || u[cur] == 0x300Du)) {
                cur = intl_next_grapheme_end(u, len, cur);
            }
            while (cur < len && (u[cur] == ' ' || u[cur] == '\t')) cur++;
            return cur;
        }
    }
    return len;
}

static uint32_t intl_step_segment(const char *granularity, const uint16_t *u, uint32_t len,
                                  uint32_t pos, bool *out_word_like) {
    *out_word_like = false;
    if (strcmp(granularity, "word") == 0) return intl_next_word_end(u, len, pos, out_word_like);
    if (strcmp(granularity, "sentence") == 0) return intl_next_sentence_end(u, len, pos);
    return intl_next_grapheme_end(u, len, pos);
}

static AjsValue intl_create_segment_data_obj(AjsVm *vm, const AjsString *input_str,
                                             uint32_t start, uint32_t end,
                                             const char *granularity, bool word_like) {
    size_t rs = ajs_temp_root_save(vm);
    AjsValue obj = ajs_new_object(vm);
    ajs_temp_root_push(vm, obj);

    AjsString *seg = ajs_str_from_utf16(vm, input_str->units + start, end - start, false);
    ajs_temp_root_push(vm, ajs_val_from_gc(seg));

    ajs_define_prop_str(vm, obj, "segment", ajs_val_from_gc(seg), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, obj, "index", ajs_val_number((double)start), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, obj, "input", ajs_val_from_gc(input_str), AJS_PROP_DEFAULT);
    if (strcmp(granularity, "word") == 0) {
        ajs_define_prop_str(vm, obj, "isWordLike", word_like ? AJS_VAL_TRUE : AJS_VAL_FALSE, AJS_PROP_DEFAULT);
    }
    ajs_temp_root_restore(vm, rs);
    return obj;
}

static AjsValue js_intl_segmenter_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (ajs_is_undefined(nt)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Constructor Intl.Segmenter requires 'new'");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue proto = intl_proto_from_new_target(vm, nt, vm->active_realm->intl_segmenter_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, proto);

    AjsValue opts = AJS_VAL_UNDEFINED;
    if (!intl_coerce_options_obj(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, &opts)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_push(vm, opts);

    AjsObject *obj = intl_alloc_instance(vm, vm->active_realm, AJS_INTL_SEGMENTER, proto);
    if (!obj) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(obj));
    AjsIntlData *d = obj->u.intl;

    IntlParsedTag pt;
    char full_loc[64];
    if (!intl_resolve_locale(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, opts, &pt, full_loc, sizeof(full_loc))) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    format_parsed_tag(&pt, NULL, 0, d->locale, sizeof(d->locale));

    static const char *const k_gran[3] = { "grapheme", "word", "sentence" };
    if (!intl_get_opt_str(vm, opts, "granularity", k_gran, 3, "grapheme", d->granularity, sizeof(d->granularity), NULL)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }

    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(obj);
}

static AjsValue js_intl_segmenter_segment(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_SEGMENTER, "Intl.Segmenter");
    if (!d) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsString *str = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (vm->has_exception || !str) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(str));

    AjsObject *segs = intl_alloc_instance(vm, vm->active_realm, AJS_INTL_SEGMENTS, vm->active_realm->intl_segments_proto);
    if (!segs) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    segs->u.intl->seg_input = ajs_val_from_gc(str);
    segs->u.intl->segmenter_obj = this_val;
    snprintf(segs->u.intl->granularity, sizeof(segs->u.intl->granularity), "%s", d->granularity);
    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(segs);
}

static AjsValue js_intl_segmenter_resolvedOptions(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_SEGMENTER, "Intl.Segmenter");
    if (!d) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue res = ajs_new_object(vm);
    ajs_temp_root_push(vm, res);
    ajs_define_prop_str(vm, res, "locale", ajs_val_from_gc(ajs_str_from_cstr(vm, d->locale, false)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "granularity", ajs_val_from_gc(ajs_str_from_cstr(vm, d->granularity, true)), AJS_PROP_DEFAULT);
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_intl_segments_containing(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_SEGMENTS, "Segments");
    if (!d) return AJS_VAL_EXCEPTION;
    double idx_num = 0.0;
    if (!ajs_to_number(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &idx_num)) return AJS_VAL_EXCEPTION;
    double n = ajs_double_to_integer_or_inf(idx_num);
    const AjsString *str = (const AjsString *)ajs_val_to_gc(d->seg_input);
    if (n < 0.0 || n >= (double)str->len16) return AJS_VAL_UNDEFINED;

    uint32_t target = (uint32_t)n;
    uint32_t pos = 0;
    while (pos < str->len16) {
        bool wl = false;
        uint32_t next = intl_step_segment(d->granularity, str->units, str->len16, pos, &wl);
        if (target >= pos && target < next) {
            return intl_create_segment_data_obj(vm, str, pos, next, d->granularity, wl);
        }
        pos = next;
    }
    return AJS_VAL_UNDEFINED;
}

static AjsValue js_intl_seg_iter_next(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    if (!ajs_val_is_gc_kind(this_val, AJS_GC_OBJECT)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Method Segment Iterator.prototype.next called on incompatible receiver");
    }
    AjsObject *it = (AjsObject *)ajs_val_to_gc(this_val);
    if (it->kind != AJS_OBJ_ITERATOR || it->u.iter.limit != INTL_SEG_ITER_TAG) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Method Segment Iterator.prototype.next called on incompatible receiver");
    }
    if (it->u.iter.done) {
        return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
    }
    AjsObject *segs_obj = (AjsObject *)ajs_val_to_gc(it->u.iter.target);
    AjsIntlData *sd = segs_obj->u.intl;
    const AjsString *str = (const AjsString *)ajs_val_to_gc(sd->seg_input);
    if (it->u.iter.index >= str->len16) {
        it->u.iter.done = true;
        return ajs_make_iter_result(vm, AJS_VAL_UNDEFINED, true);
    }
    uint32_t start = it->u.iter.index;
    bool wl = false;
    uint32_t end = intl_step_segment(sd->granularity, str->units, str->len16, start, &wl);
    it->u.iter.index = end;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue data = intl_create_segment_data_obj(vm, str, start, end, sd->granularity, wl);
    ajs_temp_root_push(vm, data);
    AjsValue res = ajs_make_iter_result(vm, data, false);
    ajs_temp_root_restore(vm, rs);
    return res;
}

static AjsValue js_intl_segments_iterator(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    if (!intl_require_kind(vm, this_val, AJS_INTL_SEGMENTS, "Segments")) return AJS_VAL_EXCEPTION;
    AjsValue iter_proto = vm->active_realm->iterator_proto;
    if (vm->call_depth > 0 && vm->call_stack[vm->call_depth - 1u].native_fn_obj) {
        AjsValue ho = vm->call_stack[vm->call_depth - 1u].native_fn_obj->u.fn.home_object;
        if (ajs_val_is_gc_kind(ho, AJS_GC_OBJECT)) iter_proto = ho;
    }
    AjsObject *it = ajs_obj_new(vm, vm->active_realm, AJS_OBJ_ITERATOR, iter_proto);
    if (!it) return AJS_VAL_EXCEPTION;
    it->u.iter.target = this_val;
    it->u.iter.index = 0;
    it->u.iter.done = false;
    it->u.iter.limit = INTL_SEG_ITER_TAG;
    it->u.iter.next_method = AJS_VAL_UNDEFINED;
    it->u.iter.helper_fn = AJS_VAL_UNDEFINED;
    it->u.iter.inner_iter = AJS_VAL_UNDEFINED;
    it->u.iter.inner_next = AJS_VAL_UNDEFINED;
    return ajs_val_from_gc(it);
}

/* ============================================================================
 * 10. Intl.DisplayNames (ECMA-402 §12)
 * ============================================================================ */

static const struct { const char *code; const char *en; const char *ko; } k_dn_langs[] = {
    {"en",    "English",             "\xEC\x98\x81\xEC\x96\xB4"},
    {"en-US", "American English",    "\xEB\xAF\xB8\xEA\xB5\xAD \xEC\x98\x81\xEC\x96\xB4"},
    {"en-GB", "British English",     "\xEC\x98\x81\xEA\xB5\xAD \xEC\x98\x81\xEC\x96\xB4"},
    {"ko",    "Korean",              "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4"},
    {"ko-KR", "Korean (South Korea)","\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4(\xEB\x8C\x80\xED\x95\x9C\xEB\xAF\xBC\xEA\xB5\xAD)"},
    {"ja",    "Japanese",            "\xEC\x9D\xBC\xEB\xB3\xB8\xEC\x96\xB4"},
    {"zh",    "Chinese",             "\xEC\xA4\x91\xEA\xB5\xAD\xEC\x96\xB4"},
    {"zh-Hans","Simplified Chinese", "\xEC\xA4\x91\xEA\xB5\xAD\xEC\x96\xB4(\xEA\xB0\x84\xEC\xB2\xB4)"},
    {"zh-Hant","Traditional Chinese","\xEC\xA4\x91\xEA\xB5\xAD\xEC\x96\xB4(\xEB\xB2\x88\xEC\xB2\xB4)"},
    {"fr",    "French",              "\xED\x94\x84\xEB\x9E\x91\xEC\x8A\xA4\xEC\x96\xB4"},
    {"de",    "German",              "\xEB\x8F\x85\xEC\x9D\xBC\xEC\x96\xB4"},
    {"es",    "Spanish",             "\xEC\x8A\xA4\xED\x8E\x98\xEC\x9D\xB8\xEC\x96\xB4"},
    {"ru",    "Russian",             "\xEB\x9F\xAC\xEC\x8B\x9C\xEC\x95\x84\xEC\x96\xB4"},
    {"ar",    "Arabic",              "\xEC\x95\x84\xEB\x9E\x8D\xEC\x96\xB4"}
};

static const struct { const char *code; const char *en; const char *ko; } k_dn_regions[] = {
    {"KR", "South Korea",    "\xEB\x8C\x80\xED\x95\x9C\xEB\xAF\xBC\xEA\xB5\xAD"},
    {"US", "United States",  "\xEB\xAF\xB8\xEA\xB5\xAD"},
    {"GB", "United Kingdom", "\xEC\x98\x81\xEA\xB5\xAD"},
    {"JP", "Japan",          "\xEC\x9D\xBC\xEB\xB3\xB8"},
    {"CN", "China",          "\xEC\xA4\x91\xEA\xB5\xAD"},
    {"TW", "Taiwan",         "\xEB\x8C\x80\xEB\xA7\x8C"},
    {"DE", "Germany",        "\xEB\x8F\x85\xEC\x9D\xBC"},
    {"FR", "France",         "\xED\x94\x84\xEB\x9E\x91\xEC\x8A\xA4"},
    {"ES", "Spain",          "\xEC\x8A\xA4\xED\x8E\x98\xEC\x9D\xB8"},
    {"CA", "Canada",         "\xEC\xBA\x90\xEB\x82\x98\xEB\x8B\xA4"},
    {"AU", "Australia",      "\xEC\x98\xA4\xEC\x8A\xA4\xED\x8A\xB8\xEB\xA0\x88\xEC\x9D\xBC\xEB\xA6\xAC\xEC\x95\x84"},
    {"UN", "United Nations", "\xEC\x9C\xA0\xEC\x97\x94"}
};

static const struct { const char *code; const char *en; const char *ko; } k_dn_scripts[] = {
    {"Latn", "Latin",               "\xEB\x9D\xBC\xED\x8B\xB4 \xEB\xAC\xB8\xEC\x9E\x90"},
    {"Hang", "Hangul",              "\xED\x95\x9C\xEA\xB8\x80"},
    {"Kore", "Korean",              "\xED\x95\x9C\xEA\xB5\xAD \xEB\xAC\xB8\xEC\x9E\x90"},
    {"Hans", "Simplified",          "\xEA\xB0\x84\xEC\xB2\xB4"},
    {"Hant", "Traditional",         "\xEB\xB2\x88\xEC\xB2\xB4"},
    {"Jpan", "Japanese",            "\xEC\x9D\xBC\xEB\xB3\xB8 \xEB\xAC\xB8\xEC\x9E\x90"},
    {"Cyrl", "Cyrillic",            "\xED\x82\xA4\xEB\xA6\xB4 \xEB\xAC\xB8\xEC\x9E\x90"},
    {"Arab", "Arabic",              "\xEC\x95\x84\xEB\x9E\x8D \xEB\xAC\xB8\xEC\x9E\x90"}
};

static AjsValue js_intl_dispnames_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (ajs_is_undefined(nt)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Constructor Intl.DisplayNames requires 'new'");
    }
    if (argc < 2 || ajs_is_undefined(argv[1])) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Options argument is required for Intl.DisplayNames");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue proto = intl_proto_from_new_target(vm, nt, vm->active_realm->intl_dispnames_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, proto);

    AjsValue opts = AJS_VAL_UNDEFINED;
    if (!intl_coerce_options_obj(vm, argv[1], &opts)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_push(vm, opts);

    AjsObject *obj = intl_alloc_instance(vm, vm->active_realm, AJS_INTL_DISPLAY_NAMES, proto);
    if (!obj) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(obj));
    AjsIntlData *d = obj->u.intl;

    IntlParsedTag pt;
    char full_loc[64];
    if (!intl_resolve_locale(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, opts, &pt, full_loc, sizeof(full_loc))) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    format_parsed_tag(&pt, NULL, 0, d->locale, sizeof(d->locale));

    static const char *const k_styles[3] = { "narrow", "short", "long" };
    if (!intl_get_opt_str(vm, opts, "style", k_styles, 3, "long", d->style, sizeof(d->style), NULL)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    static const char *const k_dtypes[6] = { "language", "region", "script", "currency", "calendar", "dateTimeField" };
    bool has_type = false;
    if (!intl_get_opt_str(vm, opts, "type", k_dtypes, 6, "", d->disp_type, sizeof(d->disp_type), &has_type)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    if (!has_type) {
        ajs_temp_root_restore(vm, rs);
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Option 'type' is required for Intl.DisplayNames");
    }
    static const char *const k_fb[2] = { "code", "none" };
    if (!intl_get_opt_str(vm, opts, "fallback", k_fb, 2, "code", d->fallback, sizeof(d->fallback), NULL)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    static const char *const k_ld[2] = { "dialect", "standard" };
    /* Store languageDisplay in d->list_type */
    if (!intl_get_opt_str(vm, opts, "languageDisplay", k_ld, 2, "dialect", d->list_type, sizeof(d->list_type), NULL)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }

    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(obj);
}

static AjsValue js_intl_dispnames_of(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_DISPLAY_NAMES, "Intl.DisplayNames");
    if (!d) return AJS_VAL_EXCEPTION;
    AjsString *cs = ajs_to_js_string(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED);
    if (vm->has_exception || !cs) return AJS_VAL_EXCEPTION;

    const char *raw = cs->utf8;
    bool ko = intl_lang_is(d->locale, "ko");
    char canon[64];
    canon[0] = '\0';

    if (strcmp(d->disp_type, "language") == 0) {
        IntlParsedTag pt;
        if (!parse_and_canonicalize_bcp47(raw, &pt, canon, sizeof(canon)) || pt.other_u[0] || pt.priv_x[0]) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid language code '%s'", raw);
        }
        format_parsed_tag(&pt, NULL, 0, canon, sizeof(canon));
        for (size_t i = 0; i < sizeof(k_dn_langs) / sizeof(k_dn_langs[0]); i++) {
            if (strcmp(canon, k_dn_langs[i].code) == 0) {
                return ajs_val_from_gc(ajs_str_from_cstr(vm, ko ? k_dn_langs[i].ko : k_dn_langs[i].en, false));
            }
        }
    } else if (strcmp(d->disp_type, "region") == 0) {
        size_t rl = strlen(raw);
        bool ok = (rl == 2 && intl_is_alpha((unsigned char)raw[0]) && intl_is_alpha((unsigned char)raw[1])) ||
                  (rl == 3 && intl_is_digit((unsigned char)raw[0]) && intl_is_digit((unsigned char)raw[1]) && intl_is_digit((unsigned char)raw[2]));
        if (!ok) return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid region code '%s'", raw);
        for (size_t k = 0; k < rl; k++) canon[k] = (char)intl_ascii_toupper((unsigned char)raw[k]);
        canon[rl] = '\0';
        if (strcmp(canon, "UK") == 0) snprintf(canon, sizeof(canon), "GB");
        for (size_t i = 0; i < sizeof(k_dn_regions) / sizeof(k_dn_regions[0]); i++) {
            if (strcmp(canon, k_dn_regions[i].code) == 0) {
                return ajs_val_from_gc(ajs_str_from_cstr(vm, ko ? k_dn_regions[i].ko : k_dn_regions[i].en, false));
            }
        }
    } else if (strcmp(d->disp_type, "script") == 0) {
        if (strlen(raw) != 4 || !intl_is_alpha((unsigned char)raw[0]) || !intl_is_alpha((unsigned char)raw[1]) ||
            !intl_is_alpha((unsigned char)raw[2]) || !intl_is_alpha((unsigned char)raw[3])) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid script code '%s'", raw);
        }
        canon[0] = (char)intl_ascii_toupper((unsigned char)raw[0]);
        for (int k = 1; k < 4; k++) canon[k] = (char)intl_ascii_tolower((unsigned char)raw[k]);
        canon[4] = '\0';
        for (size_t i = 0; i < sizeof(k_dn_scripts) / sizeof(k_dn_scripts[0]); i++) {
            if (strcmp(canon, k_dn_scripts[i].code) == 0) {
                return ajs_val_from_gc(ajs_str_from_cstr(vm, ko ? k_dn_scripts[i].ko : k_dn_scripts[i].en, false));
            }
        }
    } else if (strcmp(d->disp_type, "currency") == 0) {
        if (strlen(raw) != 3 || !intl_is_alpha((unsigned char)raw[0]) || !intl_is_alpha((unsigned char)raw[1]) ||
            !intl_is_alpha((unsigned char)raw[2])) {
            return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Invalid currency code '%s'", raw);
        }
        for (int k = 0; k < 3; k++) canon[k] = (char)intl_ascii_toupper((unsigned char)raw[k]);
        canon[3] = '\0';
        const char *cn = intl_currency_name(canon, d->locale);
        if (strcmp(cn, canon) != 0) {
            return ajs_val_from_gc(ajs_str_from_cstr(vm, cn, false));
        }
    } else {
        if (!raw[0]) return ajs_throw_err_kind(vm, AJS_ERR_KIND_RANGE, "Empty code");
        snprintf(canon, sizeof(canon), "%s", raw);
    }

    if (strcmp(d->fallback, "none") == 0) return AJS_VAL_UNDEFINED;
    return ajs_val_from_gc(ajs_str_from_cstr(vm, canon, false));
}

static AjsValue js_intl_dispnames_resolvedOptions(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_DISPLAY_NAMES, "Intl.DisplayNames");
    if (!d) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue res = ajs_new_object(vm);
    ajs_temp_root_push(vm, res);
    ajs_define_prop_str(vm, res, "locale", ajs_val_from_gc(ajs_str_from_cstr(vm, d->locale, false)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "style", ajs_val_from_gc(ajs_str_from_cstr(vm, d->style, true)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "type", ajs_val_from_gc(ajs_str_from_cstr(vm, d->disp_type, true)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "fallback", ajs_val_from_gc(ajs_str_from_cstr(vm, d->fallback, true)), AJS_PROP_DEFAULT);
    if (strcmp(d->disp_type, "language") == 0) {
        ajs_define_prop_str(vm, res, "languageDisplay", ajs_val_from_gc(ajs_str_from_cstr(vm, d->list_type, true)), AJS_PROP_DEFAULT);
    }
    ajs_temp_root_restore(vm, rs);
    return res;
}

/* ============================================================================
 * 11. Intl.ListFormat (ECMA-402 §13)
 * ============================================================================ */

static bool intl_collect_string_list(AjsVm *vm, AjsValue iterable, AjsValue *out_arr, uint32_t *out_len) {
    *out_arr = ajs_new_array(vm, 0);
    *out_len = 0;
    if (ajs_is_undefined(iterable)) return true;

    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, *out_arr);
    ajs_temp_root_push(vm, iterable);

    AjsValue iter_fn = ajs_obj_get(vm, iterable, ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), iterable);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
    if (!ajs_is_callable(vm, iter_fn)) {
        ajs_temp_root_restore(vm, rs);
        ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Argument to Intl.ListFormat is not iterable");
        return false;
    }
    ajs_temp_root_push(vm, iter_fn);

    AjsValue iter = ajs_call_internal(vm, iter_fn, iterable, 0, NULL);
    if (vm->has_exception || !ajs_val_is_gc_kind(iter, AJS_GC_OBJECT)) {
        ajs_temp_root_restore(vm, rs);
        if (!vm->has_exception) ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator is not an object");
        return false;
    }
    ajs_temp_root_push(vm, iter);

    AjsValue next_fn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_next), iter);
    if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
    ajs_temp_root_push(vm, next_fn);

    while (true) {
        AjsValue step = ajs_call_internal(vm, next_fn, iter, 0, NULL);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
        if (!ajs_val_is_gc_kind(step, AJS_GC_OBJECT)) {
            ajs_temp_root_restore(vm, rs);
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterator result is not an object");
            return false;
        }
        size_t srs = ajs_temp_root_save(vm);
        ajs_temp_root_push(vm, step);
        AjsValue done_v = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_done), step);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
        if (ajs_to_bool(vm, done_v)) {
            ajs_temp_root_restore(vm, srs);
            break;
        }
        AjsValue val = ajs_obj_get(vm, step, ajs_val_from_gc(vm->str_value), step);
        if (vm->has_exception) { ajs_temp_root_restore(vm, rs); return false; }
        if (!ajs_val_is_gc_kind(val, AJS_GC_STRING)) {
            /* IteratorClose on abrupt TypeError per StringListFromIterable */
            AjsValue ret_fn = ajs_obj_get(vm, iter, ajs_val_from_gc(vm->str_return), iter);
            if (!vm->has_exception && ajs_is_callable(vm, ret_fn)) {
                (void)ajs_call_internal(vm, ret_fn, iter, 0, NULL);
                vm->has_exception = false;
            }
            vm->has_exception = false;
            ajs_temp_root_restore(vm, rs);
            ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Iterable yielded non-string value in Intl.ListFormat");
            return false;
        }
        ajs_set_prop_index(vm, *out_arr, (*out_len)++, val);
        ajs_temp_root_restore(vm, srs);
    }

    ajs_temp_root_restore(vm, rs);
    return true;
}

static void intl_list_delims(const AjsIntlData *d, uint32_t count,
                             const char **out_mid, const char **out_pair, const char **out_end) {
    bool ko = intl_lang_is(d->locale, "ko");
    bool ja = intl_lang_is(d->locale, "ja");
    bool zh = intl_lang_is(d->locale, "zh");
    bool sh = (strcmp(d->style, "short") == 0);
    bool nr = (strcmp(d->style, "narrow") == 0);
    bool disj = (strcmp(d->list_type, "disjunction") == 0);
    bool unit = (strcmp(d->list_type, "unit") == 0);
    (void)count;

    if (ko) {
        *out_mid = ", ";
        if (unit)      { *out_pair = nr ? " " : ", "; *out_end = nr ? " " : ", "; }
        else if (disj) { *out_pair = " \xEB\x98\x90\xEB\x8A\x94 "; *out_end = ", \xEB\x98\x90\xEB\x8A\x94 "; }
        else           { *out_pair = nr ? ", " : " \xEB\xB0\x8F "; *out_end = nr ? ", " : " \xEB\xB0\x8F "; }
        return;
    }
    if (ja || zh) {
        *out_mid = "\xE3\x80\x81";
        *out_pair = disj ? (ja ? "\xE3\x81\xBE\xE3\x81\x9F\xE3\x81\xAF" : "\xE6\x88\x96") : "\xE3\x80\x81";
        *out_end = *out_pair;
        return;
    }
    *out_mid = ", ";
    if (unit) {
        *out_pair = nr ? " " : ", ";
        *out_end  = nr ? " " : ", ";
    } else if (disj) {
        *out_pair = " or ";
        *out_end  = ", or ";
    } else {
        if (nr)      { *out_pair = ", ";    *out_end = ", "; }
        else if (sh) { *out_pair = " & ";   *out_end = ", & "; }
        else         { *out_pair = " and "; *out_end = ", and "; }
    }
}

static AjsValue js_intl_listfmt_ctor(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    AjsValue nt = ajs_get_native_new_target(vm);
    if (ajs_is_undefined(nt)) {
        return ajs_throw_err_kind(vm, AJS_ERR_KIND_TYPE, "Constructor Intl.ListFormat requires 'new'");
    }
    size_t rs = ajs_temp_root_save(vm);
    AjsValue proto = intl_proto_from_new_target(vm, nt, vm->active_realm->intl_listfmt_proto);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    ajs_temp_root_push(vm, proto);

    AjsValue opts = AJS_VAL_UNDEFINED;
    if (!intl_coerce_options_obj(vm, argc > 1 ? argv[1] : AJS_VAL_UNDEFINED, &opts)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_push(vm, opts);

    AjsObject *obj = intl_alloc_instance(vm, vm->active_realm, AJS_INTL_LIST_FORMAT, proto);
    if (!obj) { ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION; }
    ajs_temp_root_push(vm, ajs_val_from_gc(obj));
    AjsIntlData *d = obj->u.intl;

    IntlParsedTag pt;
    char full_loc[64];
    if (!intl_resolve_locale(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, opts, &pt, full_loc, sizeof(full_loc))) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    format_parsed_tag(&pt, NULL, 0, d->locale, sizeof(d->locale));

    static const char *const k_ltypes[3] = { "conjunction", "disjunction", "unit" };
    if (!intl_get_opt_str(vm, opts, "type", k_ltypes, 3, "conjunction", d->list_type, sizeof(d->list_type), NULL)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }
    static const char *const k_styles[3] = { "long", "short", "narrow" };
    if (!intl_get_opt_str(vm, opts, "style", k_styles, 3, "long", d->style, sizeof(d->style), NULL)) {
        ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
    }

    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(obj);
}

static AjsValue js_intl_listfmt_formatToParts(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_LIST_FORMAT, "Intl.ListFormat");
    if (!d) return AJS_VAL_EXCEPTION;

    size_t rs = ajs_temp_root_save(vm);
    AjsValue items = AJS_VAL_UNDEFINED;
    uint32_t len = 0;
    if (!intl_collect_string_list(vm, argc > 0 ? argv[0] : AJS_VAL_UNDEFINED, &items, &len)) {
        return AJS_VAL_EXCEPTION;
    }
    ajs_temp_root_push(vm, items);

    AjsValue out = ajs_new_array(vm, 0);
    ajs_temp_root_push(vm, out);
    uint32_t idx = 0;

    const char *mid = ", ", *pair = " and ", *end = ", and ";
    intl_list_delims(d, len, &mid, &pair, &end);

    for (uint32_t i = 0; i < len; i++) {
        AjsValue iv = ajs_get_prop_index(vm, items, i);
        const AjsString *is = (const AjsString *)ajs_val_to_gc(iv);
        if (!intl_push_part_ex(vm, out, &idx, "element", is->utf8, NULL)) {
            ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
        }
        if (i + 1u < len) {
            const char *sep = (len == 2u) ? pair : ((i + 2u == len) ? end : mid);
            if (!intl_push_part_ex(vm, out, &idx, "literal", sep, NULL)) {
                ajs_temp_root_restore(vm, rs); return AJS_VAL_EXCEPTION;
            }
        }
    }

    ajs_temp_root_restore(vm, rs);
    return out;
}

static AjsValue js_intl_listfmt_format(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    AjsValue parts = js_intl_listfmt_formatToParts(vm, this_val, argc, argv, ud);
    if (vm->has_exception) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    ajs_temp_root_push(vm, parts);

    AjsString *acc = vm->str_empty;
    AjsValue lv = ajs_get_prop_str(vm, parts, "length");
    uint64_t plen = 0;
    ajs_to_length(vm, lv, &plen);
    for (uint32_t i = 0; i < (uint32_t)plen; i++) {
        AjsValue p = ajs_get_prop_index(vm, parts, i);
        AjsValue vv = ajs_get_prop_str(vm, p, "value");
        if (ajs_val_is_gc_kind(vv, AJS_GC_STRING)) {
            size_t irs = ajs_temp_root_save(vm);
            ajs_temp_root_push(vm, ajs_val_from_gc(acc));
            ajs_temp_root_push(vm, vv);
            acc = ajs_str_concat(vm, acc, (const AjsString *)ajs_val_to_gc(vv));
            ajs_temp_root_restore(vm, irs);
        }
    }
    ajs_temp_root_restore(vm, rs);
    return ajs_val_from_gc(acc);
}

static AjsValue js_intl_listfmt_resolvedOptions(AjsVm *vm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)argc; (void)argv; (void)ud;
    AjsIntlData *d = intl_require_kind(vm, this_val, AJS_INTL_LIST_FORMAT, "Intl.ListFormat");
    if (!d) return AJS_VAL_EXCEPTION;
    size_t rs = ajs_temp_root_save(vm);
    AjsValue res = ajs_new_object(vm);
    ajs_temp_root_push(vm, res);
    ajs_define_prop_str(vm, res, "locale", ajs_val_from_gc(ajs_str_from_cstr(vm, d->locale, false)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "type", ajs_val_from_gc(ajs_str_from_cstr(vm, d->list_type, true)), AJS_PROP_DEFAULT);
    ajs_define_prop_str(vm, res, "style", ajs_val_from_gc(ajs_str_from_cstr(vm, d->style, true)), AJS_PROP_DEFAULT);
    ajs_temp_root_restore(vm, rs);
    return res;
}

/* ============================================================================
 * 12. Realm Initialization (`ajs_init_intl_builtins`)
 * ============================================================================ */

static void intl_def_magic_getter(AjsVm *vm, AjsRealm *r, AjsValue proto, const char *name,
                                  AjsNativeFn getter, int32_t magic) {
    size_t rs = ajs_temp_root_save(vm);
    AjsObject *gfn = ajs_obj_new(vm, r, AJS_OBJ_FUNCTION, r->function_proto);
    if (!gfn) { ajs_temp_root_restore(vm, rs); return; }
    ajs_temp_root_push(vm, ajs_val_from_gc(gfn));
    gfn->u.fn.fn_kind = AJS_FN_NATIVE;
    gfn->u.fn.strict = true;
    gfn->u.fn.native_fn = getter;
    gfn->u.fn.native_magic = magic;

    char gname[48];
    snprintf(gname, sizeof(gname), "get %s", name);
    AjsPropDesc fd;
    memset(&fd, 0, sizeof(fd));
    fd.flags = AJS_DESC_HAS_VALUE | AJS_DESC_HAS_WRITABLE | AJS_DESC_HAS_ENUMERABLE |
               AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    fd.value = ajs_val_number(0.0);
    ajs_obj_define_own_prop(vm, gfn, ajs_val_from_gc(vm->str_length), &fd, false);
    fd.value = ajs_val_from_gc(ajs_str_from_cstr(vm, gname, true));
    ajs_obj_define_own_prop(vm, gfn, ajs_val_from_gc(vm->str_name), &fd, false);

    AjsPropDesc ad;
    memset(&ad, 0, sizeof(ad));
    ad.flags = AJS_DESC_IS_ACCESSOR | AJS_DESC_HAS_GET | AJS_DESC_HAS_ENUMERABLE |
               AJS_DESC_HAS_CONFIGURABLE | AJS_DESC_CONFIGURABLE;
    ad.getter = ajs_val_from_gc(gfn);
    ajs_obj_define_own_prop(vm, (AjsObject *)ajs_val_to_gc(proto),
                            ajs_val_from_gc(ajs_str_from_cstr(vm, name, true)), &ad, false);
    ajs_temp_root_restore(vm, rs);
}

void ajs_init_intl_builtins(AjsVm *vm, AjsRealm *r) {
    size_t rs = ajs_temp_root_save(vm);

    AjsObject *intl_obj_ptr = ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto);
    if (!intl_obj_ptr) return;
    AjsValue intl_obj = ajs_val_from_gc(intl_obj_ptr);
    ajs_temp_root_push(vm, intl_obj);
    intl_def_to_string_tag(vm, intl_obj, "Intl");

    ajs_def_native_method(vm, r, intl_obj, "getCanonicalLocales", js_intl_getCanonicalLocales, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, intl_obj, "supportedValuesOf", js_intl_supportedValuesOf, 1, AJS_METHOD_FLAGS, 0);

    /* 1. Intl.Locale */
    r->intl_locale_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    intl_def_to_string_tag(vm, r->intl_locale_proto, "Intl.Locale");
    ajs_def_native_method(vm, r, r->intl_locale_proto, "maximize", js_intl_locale_maximize, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->intl_locale_proto, "minimize", js_intl_locale_minimize, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->intl_locale_proto, "toString", js_intl_locale_toString, 0, AJS_METHOD_FLAGS, 0);
    intl_def_magic_getter(vm, r, r->intl_locale_proto, "baseName",        js_intl_locale_getter, 1);
    intl_def_magic_getter(vm, r, r->intl_locale_proto, "language",        js_intl_locale_getter, 2);
    intl_def_magic_getter(vm, r, r->intl_locale_proto, "script",          js_intl_locale_getter, 3);
    intl_def_magic_getter(vm, r, r->intl_locale_proto, "region",          js_intl_locale_getter, 4);
    intl_def_magic_getter(vm, r, r->intl_locale_proto, "calendar",        js_intl_locale_getter, 5);
    intl_def_magic_getter(vm, r, r->intl_locale_proto, "collation",       js_intl_locale_getter, 6);
    intl_def_magic_getter(vm, r, r->intl_locale_proto, "numberingSystem", js_intl_locale_getter, 7);
    intl_def_magic_getter(vm, r, r->intl_locale_proto, "hourCycle",       js_intl_locale_getter, 8);
    intl_def_magic_getter(vm, r, r->intl_locale_proto, "caseFirst",       js_intl_locale_getter, 9);
    intl_def_magic_getter(vm, r, r->intl_locale_proto, "numeric",         js_intl_locale_getter, 10);
    AjsValue locale_ctor = intl_make_ctor(vm, r, js_intl_locale_ctor, "Locale", 1, INTL_MAGIC_LOCALE, r->intl_locale_proto);
    ajs_define_prop_str(vm, intl_obj, "Locale", locale_ctor, AJS_METHOD_FLAGS);

    /* 2. Intl.Collator */
    r->intl_collator_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    intl_def_to_string_tag(vm, r->intl_collator_proto, "Intl.Collator");
    intl_def_magic_getter(vm, r, r->intl_collator_proto, "compare", js_intl_collator_compare_get, 0);
    ajs_def_native_method(vm, r, r->intl_collator_proto, "resolvedOptions", js_intl_collator_resolvedOptions, 0, AJS_METHOD_FLAGS, 0);
    AjsValue coll_ctor = intl_make_ctor(vm, r, js_intl_collator_ctor, "Collator", 0, INTL_MAGIC_COLLATOR, r->intl_collator_proto);
    ajs_temp_root_push(vm, coll_ctor);
    ajs_def_native_method(vm, r, coll_ctor, "supportedLocalesOf", js_intl_supported_locales_of, 1, AJS_METHOD_FLAGS, 0);
    ajs_define_prop_str(vm, intl_obj, "Collator", coll_ctor, AJS_METHOD_FLAGS);

    /* 3. Intl.NumberFormat */
    r->intl_numfmt_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    intl_def_to_string_tag(vm, r->intl_numfmt_proto, "Intl.NumberFormat");
    intl_def_magic_getter(vm, r, r->intl_numfmt_proto, "format", js_intl_numfmt_format_get, 0);
    ajs_def_native_method(vm, r, r->intl_numfmt_proto, "formatToParts",   js_intl_numfmt_formatToParts,   1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->intl_numfmt_proto, "formatRange",     js_intl_numfmt_formatRange,     2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->intl_numfmt_proto, "resolvedOptions", js_intl_numfmt_resolvedOptions, 0, AJS_METHOD_FLAGS, 0);
    AjsValue nf_ctor = intl_make_ctor(vm, r, js_intl_numfmt_ctor, "NumberFormat", 0, INTL_MAGIC_NUMFMT, r->intl_numfmt_proto);
    ajs_temp_root_push(vm, nf_ctor);
    ajs_def_native_method(vm, r, nf_ctor, "supportedLocalesOf", js_intl_supported_locales_of, 1, AJS_METHOD_FLAGS, 0);
    ajs_define_prop_str(vm, intl_obj, "NumberFormat", nf_ctor, AJS_METHOD_FLAGS);

    /* 4. Intl.DateTimeFormat */
    r->intl_dtfmt_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    intl_def_to_string_tag(vm, r->intl_dtfmt_proto, "Intl.DateTimeFormat");
    intl_def_magic_getter(vm, r, r->intl_dtfmt_proto, "format", js_intl_dtfmt_format_get, 0);
    ajs_def_native_method(vm, r, r->intl_dtfmt_proto, "formatToParts",   js_intl_dtfmt_formatToParts,   1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->intl_dtfmt_proto, "resolvedOptions", js_intl_dtfmt_resolvedOptions, 0, AJS_METHOD_FLAGS, 0);
    AjsValue dtf_ctor = intl_make_ctor(vm, r, js_intl_dtfmt_ctor, "DateTimeFormat", 0, INTL_MAGIC_DTFMT, r->intl_dtfmt_proto);
    ajs_temp_root_push(vm, dtf_ctor);
    ajs_def_native_method(vm, r, dtf_ctor, "supportedLocalesOf", js_intl_supported_locales_of, 1, AJS_METHOD_FLAGS, 0);
    ajs_define_prop_str(vm, intl_obj, "DateTimeFormat", dtf_ctor, AJS_METHOD_FLAGS);

    /* 5. Intl.PluralRules */
    r->intl_plural_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    intl_def_to_string_tag(vm, r->intl_plural_proto, "Intl.PluralRules");
    ajs_def_native_method(vm, r, r->intl_plural_proto, "select",          js_intl_plural_select,          1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->intl_plural_proto, "selectRange",     js_intl_plural_selectRange,     2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->intl_plural_proto, "resolvedOptions", js_intl_plural_resolvedOptions, 0, AJS_METHOD_FLAGS, 0);
    AjsValue pl_ctor = intl_make_ctor(vm, r, js_intl_plural_ctor, "PluralRules", 0, INTL_MAGIC_PLURAL, r->intl_plural_proto);
    ajs_temp_root_push(vm, pl_ctor);
    ajs_def_native_method(vm, r, pl_ctor, "supportedLocalesOf", js_intl_supported_locales_of, 1, AJS_METHOD_FLAGS, 0);
    ajs_define_prop_str(vm, intl_obj, "PluralRules", pl_ctor, AJS_METHOD_FLAGS);

    /* 6. Intl.RelativeTimeFormat */
    r->intl_reltime_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    intl_def_to_string_tag(vm, r->intl_reltime_proto, "Intl.RelativeTimeFormat");
    ajs_def_native_method(vm, r, r->intl_reltime_proto, "format",          js_intl_reltime_format,          2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->intl_reltime_proto, "formatToParts",   js_intl_reltime_formatToParts,   2, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->intl_reltime_proto, "resolvedOptions", js_intl_reltime_resolvedOptions, 0, AJS_METHOD_FLAGS, 0);
    AjsValue rtf_ctor = intl_make_ctor(vm, r, js_intl_reltime_ctor, "RelativeTimeFormat", 0, INTL_MAGIC_RELTIME, r->intl_reltime_proto);
    ajs_temp_root_push(vm, rtf_ctor);
    ajs_def_native_method(vm, r, rtf_ctor, "supportedLocalesOf", js_intl_supported_locales_of, 1, AJS_METHOD_FLAGS, 0);
    ajs_define_prop_str(vm, intl_obj, "RelativeTimeFormat", rtf_ctor, AJS_METHOD_FLAGS);

    /* 7. Intl.Segmenter + %SegmentsPrototype% + %SegmentIteratorPrototype% */
    r->intl_segmenter_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    intl_def_to_string_tag(vm, r->intl_segmenter_proto, "Intl.Segmenter");
    ajs_def_native_method(vm, r, r->intl_segmenter_proto, "segment",         js_intl_segmenter_segment,         1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->intl_segmenter_proto, "resolvedOptions", js_intl_segmenter_resolvedOptions, 0, AJS_METHOD_FLAGS, 0);

    AjsValue seg_iter_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->iterator_proto));
    ajs_temp_root_push(vm, seg_iter_proto);
    intl_def_to_string_tag(vm, seg_iter_proto, "Segmenter String Iterator");
    ajs_def_native_method(vm, r, seg_iter_proto, "next", js_intl_seg_iter_next, 0, AJS_METHOD_FLAGS, 0);

    r->intl_segments_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    ajs_def_native_method(vm, r, r->intl_segments_proto, "containing", js_intl_segments_containing, 1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_sym_method(vm, r, r->intl_segments_proto, vm->well_known_syms[AJS_SYM_ITERATOR],
                              "[Symbol.iterator]", js_intl_segments_iterator, 0, AJS_METHOD_FLAGS);
    AjsValue sit_fn = ajs_obj_get(vm, r->intl_segments_proto,
                                  ajs_val_from_gc(vm->well_known_syms[AJS_SYM_ITERATOR]), r->intl_segments_proto);
    if (ajs_val_is_gc_kind(sit_fn, AJS_GC_OBJECT)) {
        ((AjsObject *)ajs_val_to_gc(sit_fn))->u.fn.home_object = seg_iter_proto;
    }

    AjsValue seg_ctor = intl_make_ctor(vm, r, js_intl_segmenter_ctor, "Segmenter", 0, INTL_MAGIC_SEGMENTER, r->intl_segmenter_proto);
    ajs_temp_root_push(vm, seg_ctor);
    ajs_def_native_method(vm, r, seg_ctor, "supportedLocalesOf", js_intl_supported_locales_of, 1, AJS_METHOD_FLAGS, 0);
    ajs_define_prop_str(vm, intl_obj, "Segmenter", seg_ctor, AJS_METHOD_FLAGS);

    /* 8. Intl.DisplayNames */
    r->intl_dispnames_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    intl_def_to_string_tag(vm, r->intl_dispnames_proto, "Intl.DisplayNames");
    ajs_def_native_method(vm, r, r->intl_dispnames_proto, "of",              js_intl_dispnames_of,              1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->intl_dispnames_proto, "resolvedOptions", js_intl_dispnames_resolvedOptions, 0, AJS_METHOD_FLAGS, 0);
    AjsValue dn_ctor = intl_make_ctor(vm, r, js_intl_dispnames_ctor, "DisplayNames", 2, INTL_MAGIC_DISPNAMES, r->intl_dispnames_proto);
    ajs_temp_root_push(vm, dn_ctor);
    ajs_def_native_method(vm, r, dn_ctor, "supportedLocalesOf", js_intl_supported_locales_of, 1, AJS_METHOD_FLAGS, 0);
    ajs_define_prop_str(vm, intl_obj, "DisplayNames", dn_ctor, AJS_METHOD_FLAGS);

    /* 9. Intl.ListFormat */
    r->intl_listfmt_proto = ajs_val_from_gc(ajs_obj_new(vm, r, AJS_OBJ_ORDINARY, r->object_proto));
    intl_def_to_string_tag(vm, r->intl_listfmt_proto, "Intl.ListFormat");
    ajs_def_native_method(vm, r, r->intl_listfmt_proto, "format",          js_intl_listfmt_format,          1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->intl_listfmt_proto, "formatToParts",   js_intl_listfmt_formatToParts,   1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->intl_listfmt_proto, "resolvedOptions", js_intl_listfmt_resolvedOptions, 0, AJS_METHOD_FLAGS, 0);
    AjsValue lf_ctor = intl_make_ctor(vm, r, js_intl_listfmt_ctor, "ListFormat", 0, INTL_MAGIC_LISTFMT, r->intl_listfmt_proto);
    ajs_temp_root_push(vm, lf_ctor);
    ajs_def_native_method(vm, r, lf_ctor, "supportedLocalesOf", js_intl_supported_locales_of, 1, AJS_METHOD_FLAGS, 0);
    ajs_define_prop_str(vm, intl_obj, "ListFormat", lf_ctor, AJS_METHOD_FLAGS);

    /* Expose global `Intl` (writable, configurable, non-enumerable per ECMA-402 §8) */
    ajs_define_prop_str(vm, r->global_obj, "Intl", intl_obj, AJS_METHOD_FLAGS);

    /* Wire ECMA-402 §19 locale-sensitive prototype overrides cleanly without touching other files */
    ajs_def_native_method(vm, r, r->string_proto, "localeCompare",      js_str_proto_localeCompare_intl,     1, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->number_proto, "toLocaleString",     js_num_proto_toLocaleString_intl,    0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->bigint_proto, "toLocaleString",     js_bigint_proto_toLocaleString_intl, 0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->date_proto,   "toLocaleString",     js_date_proto_toLocale_intl,         0, AJS_METHOD_FLAGS, 1);
    ajs_def_native_method(vm, r, r->date_proto,   "toLocaleDateString", js_date_proto_toLocale_intl,         0, AJS_METHOD_FLAGS, 0);
    ajs_def_native_method(vm, r, r->date_proto,   "toLocaleTimeString", js_date_proto_toLocale_intl,         0, AJS_METHOD_FLAGS, 2);

    ajs_temp_root_restore(vm, rs);
}
