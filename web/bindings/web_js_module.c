/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * web/bindings/web_js_module.c - Clean-Room Pure C11 Web Host Entrypoint,
 *                                RFC 3986 / WHATWG URL & Origin Engine,
 *                                HTML Import Maps, ES Module Graph Loader,
 *                                Worker Realm Helpers & ArcrJsVTable (ABI v2)
 *
 * Zero QuickJS dependency. Zero link dependency on `arcr_engine`.
 * Safe on 32-bit x86 SSE1 (Windows 2000 SP4) & 64-bit x86-64.
 */
#include "web_host.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * 1. Global Monotonic VM Instance ID & Inline Module Sequence Counter
 * ========================================================================= */
#if defined(_WIN32)
static LONG     g_vm_id_once = 0;
static WebMutex g_vm_id_mtx;
static uint64_t g_next_vm_id = 1u;

static uint64_t web_alloc_vm_instance_id(void) {
    while (InterlockedCompareExchange(&g_vm_id_once, 1, 0) != 0) {
        if (g_vm_id_once == 2) break;
        Sleep(0);
    }
    if (g_vm_id_once == 1) {
        web_mutex_init(&g_vm_id_mtx);
        InterlockedExchange(&g_vm_id_once, 2);
    }
    web_mutex_lock(&g_vm_id_mtx);
    uint64_t id = g_next_vm_id++;
    web_mutex_unlock(&g_vm_id_mtx);
    return id;
}
#else
static pthread_mutex_t g_vm_id_mtx  = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        g_next_vm_id = 1u;

static uint64_t web_alloc_vm_instance_id(void) {
    (void)pthread_mutex_lock(&g_vm_id_mtx);
    uint64_t id = g_next_vm_id++;
    (void)pthread_mutex_unlock(&g_vm_id_mtx);
    return id;
}
#endif

static char *web_heap_strdup_n(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1u);
    if (!p) return NULL;
    if (s && n > 0u) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static char *web_heap_strdup(const char *s) {
    if (!s) return NULL;
    return web_heap_strdup_n(s, strlen(s));
}

/* =========================================================================
 * 2. RFC 3986 / WHATWG URL Resolution, Origin Computation & <base href>
 * ========================================================================= */
static bool is_ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_scheme_char(char c) {
    return is_ascii_alpha(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

/* Strips leading/trailing C0 controls+space and removes embedded \t, \r, \n per WHATWG URL */
static size_t sanitize_url_input(const char *raw, char *out, size_t cap) {
    if (!out || cap == 0u) return 0u;
    out[0] = '\0';
    if (!raw) return 0u;

    size_t len = strlen(raw);
    size_t b = 0u;
    while (b < len && (unsigned char)raw[b] <= 0x20u) ++b;
    size_t e = len;
    while (e > b && (unsigned char)raw[e - 1u] <= 0x20u) --e;

    size_t o = 0u;
    for (size_t i = b; i < e && o + 1u < cap; ++i) {
        char c = raw[i];
        if (c == '\t' || c == '\r' || c == '\n') continue;
        if ((unsigned char)c < 0x20u || (unsigned char)c == 0x7Fu) {
            out[0] = '\0';
            return (size_t)-1;
        }
        out[o++] = c;
    }
    out[o] = '\0';
    return o;
}

static size_t extract_url_scheme(const char *s, char *out_scheme, size_t scheme_cap) {
    if (!s || !is_ascii_alpha(s[0])) return 0u;
    size_t i = 1u;
    while (s[i] != '\0' && s[i] != ':') {
        if (!is_scheme_char(s[i])) return 0u;
        ++i;
    }
    if (s[i] != ':') return 0u;
    if (out_scheme && scheme_cap > 0u) {
        size_t cp = (i < scheme_cap - 1u) ? i : (scheme_cap - 1u);
        for (size_t k = 0u; k < cp; ++k) out_scheme[k] = arcr_ascii_tolower(s[k]);
        out_scheme[cp] = '\0';
    }
    return i; /* index of ':' */
}

static bool is_special_scheme(const char *scheme_lc) {
    if (!scheme_lc) return false;
    return strcmp(scheme_lc, "http") == 0 ||
           strcmp(scheme_lc, "https") == 0 ||
           strcmp(scheme_lc, "ws") == 0 ||
           strcmp(scheme_lc, "wss") == 0 ||
           strcmp(scheme_lc, "ftp") == 0 ||
           strcmp(scheme_lc, "file") == 0;
}

static int default_port_for_scheme(const char *scheme_lc) {
    if (!scheme_lc) return -1;
    if (strcmp(scheme_lc, "http") == 0 || strcmp(scheme_lc, "ws") == 0) return 80;
    if (strcmp(scheme_lc, "https") == 0 || strcmp(scheme_lc, "wss") == 0) return 443;
    if (strcmp(scheme_lc, "ftp") == 0) return 21;
    return -1;
}

typedef struct ParsedHierUrl {
    char scheme[32];
    char userinfo[256]; /* includes trailing '@' if present */
    char host[512];
    int  port;          /* -1 if omitted or default-elided */
    char path[2048];
    char query[2048];   /* includes leading '?' if present */
    char frag[1024];    /* includes leading '#' if present */
} ParsedHierUrl;

/* RFC 3986 Section 5.2.4 Remove Dot Segments */
static void remove_dot_segments(const char *in_path, char *out_path, size_t out_cap) {
    if (!out_path || out_cap == 0u) return;
    if (!in_path || in_path[0] == '\0') {
        arcr_str_copy(out_path, out_cap, "/");
        return;
    }

    char in_buf[4096];
    size_t ilen = 0u;
    for (size_t i = 0u; in_path[i] != '\0' && ilen + 1u < sizeof(in_buf); ++i) {
        in_buf[ilen++] = (in_path[i] == '\\') ? '/' : in_path[i];
    }
    in_buf[ilen] = '\0';

    char out_buf[4096];
    size_t olen = 0u;
    out_buf[0] = '\0';

    const char *p = in_buf;
    while (*p != '\0') {
        if (strncmp(p, "../", 3u) == 0) {
            p += 3;
        } else if (strncmp(p, "./", 2u) == 0) {
            p += 2;
        } else if (strncmp(p, "/./", 3u) == 0) {
            p += 2;
        } else if (strcmp(p, "/.") == 0) {
            p = "/";
            break; /* append '/' below */
        } else if (strncmp(p, "/../", 4u) == 0) {
            p += 3;
            while (olen > 0u && out_buf[olen - 1u] != '/') --olen;
            if (olen > 0u) --olen;
            out_buf[olen] = '\0';
        } else if (strcmp(p, "/..") == 0) {
            while (olen > 0u && out_buf[olen - 1u] != '/') --olen;
            if (olen > 0u) --olen;
            out_buf[olen] = '\0';
            if (olen + 1u < sizeof(out_buf)) {
                out_buf[olen++] = '/';
                out_buf[olen] = '\0';
            }
            p += 3;
        } else if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0) {
            break;
        } else {
            const char *seg_start = p;
            if (*p == '/') ++p;
            while (*p != '\0' && *p != '/') ++p;
            size_t slen = (size_t)(p - seg_start);
            if (olen + slen + 1u < sizeof(out_buf)) {
                memcpy(out_buf + olen, seg_start, slen);
                olen += slen;
                out_buf[olen] = '\0';
            }
        }
    }

    if (strcmp(p, "/") == 0) {
        if (olen + 1u < sizeof(out_buf)) {
            out_buf[olen++] = '/';
            out_buf[olen] = '\0';
        }
    }
    if (olen == 0u || out_buf[0] != '/') {
        if (out_cap == 1u) {
            out_path[0] = '\0';
        } else {
            out_path[0] = '/';
            arcr_str_copy(out_path + 1u, out_cap - 1u, out_buf);
        }
    } else {
        arcr_str_copy(out_path, out_cap, out_buf);
    }
}

static bool parse_authority(const char *auth, size_t auth_len, const char *scheme_lc,
                            char *out_userinfo, size_t ui_cap,
                            char *out_host, size_t host_cap,
                            int *out_port) {
    out_userinfo[0] = '\0';
    out_host[0]     = '\0';
    *out_port       = -1;

    const char *at = NULL;
    for (size_t i = 0u; i < auth_len; ++i) {
        if (auth[i] == '@') at = auth + i;
    }
    const char *hp = auth;
    size_t hlen = auth_len;
    if (at) {
        size_t ulen = (size_t)(at - auth) + 1u;
        if (ulen < ui_cap) {
            memcpy(out_userinfo, auth, ulen);
            out_userinfo[ulen] = '\0';
        }
        hp   = at + 1;
        hlen = auth_len - ulen;
    }

    if (hlen == 0u) {
        /* Only file:// allows an empty host */
        return strcmp(scheme_lc, "file") == 0;
    }

    const char *port_sep = NULL;
    if (hp[0] == '[') {
        const char *rb = memchr(hp, ']', hlen);
        if (!rb) return false;
        size_t iplen = (size_t)(rb - hp) + 1u;
        if (iplen >= host_cap) return false;
        for (size_t i = 0u; i < iplen; ++i) out_host[i] = arcr_ascii_tolower(hp[i]);
        out_host[iplen] = '\0';
        if (iplen < hlen) {
            if (hp[iplen] != ':') return false;
            port_sep = hp + iplen;
        }
    } else {
        for (size_t i = 0u; i < hlen; ++i) {
            if (hp[i] == ':') {
                port_sep = hp + i;
                break;
            }
        }
        size_t name_len = port_sep ? (size_t)(port_sep - hp) : hlen;
        if (name_len == 0u && strcmp(scheme_lc, "file") != 0) return false;
        if (name_len >= host_cap) return false;
        for (size_t i = 0u; i < name_len; ++i) {
            char ch = hp[i];
            if (ch == ' ' || ch == '/' || ch == '?' || ch == '#' || ch == '\\') return false;
            out_host[i] = arcr_ascii_tolower(ch);
        }
        out_host[name_len] = '\0';
    }

    if (port_sep) {
        size_t plen = hlen - (size_t)(port_sep - hp) - 1u;
        if (plen > 0u) {
            if (plen > 5u) return false;
            int pv = 0;
            for (size_t i = 0u; i < plen; ++i) {
                char d = port_sep[1u + i];
                if (d < '0' || d > '9') return false;
                pv = pv * 10 + (d - '0');
            }
            if (pv > 65535) return false;
            if (pv != default_port_for_scheme(scheme_lc)) {
                *out_port = pv;
            }
        }
    }
    return true;
}

static bool parse_hierarchical_url(const char *url, ParsedHierUrl *out) {
    if (!url || !out) return false;
    memset(out, 0, sizeof(*out));
    out->port = -1;

    size_t colon = extract_url_scheme(url, out->scheme, sizeof(out->scheme));
    if (colon == 0u) return false;

    const char *p = url + colon + 1u;
    bool special = is_special_scheme(out->scheme);

    /* Count leading slashes / backslashes */
    size_t slashes = 0u;
    while (p[slashes] == '/' || (special && p[slashes] == '\\')) ++slashes;
    if (slashes < 2u && !(strcmp(out->scheme, "file") == 0 && slashes == 1u)) {
        return false;
    }
    if (slashes >= 2u) p += 2u;
    /* Note: for file:///path, p now starts at '/path' and authority is empty */
    const char *auth_start = p;
    while (*p != '\0' && *p != '/' && !(special && *p == '\\') && *p != '?' && *p != '#') {
        ++p;
    }
    if (!parse_authority(auth_start, (size_t)(p - auth_start), out->scheme,
                         out->userinfo, sizeof(out->userinfo),
                         out->host, sizeof(out->host), &out->port)) {
        return false;
    }

    const char *path_start = p;
    while (*p != '\0' && *p != '?' && *p != '#') ++p;
    char raw_path[2048];
    size_t rplen = (size_t)(p - path_start);
    if (rplen >= sizeof(raw_path)) rplen = sizeof(raw_path) - 1u;
    memcpy(raw_path, path_start, rplen);
    raw_path[rplen] = '\0';
    remove_dot_segments(raw_path, out->path, sizeof(out->path));

    if (*p == '?') {
        const char *q_start = p;
        while (*p != '\0' && *p != '#') ++p;
        size_t qlen = (size_t)(p - q_start);
        if (qlen >= sizeof(out->query)) qlen = sizeof(out->query) - 1u;
        memcpy(out->query, q_start, qlen);
        out->query[qlen] = '\0';
    }
    if (*p == '#') {
        arcr_str_copy(out->frag, sizeof(out->frag), p);
    }
    return true;
}

static bool format_hierarchical_url(const ParsedHierUrl *u, char *out_url, size_t cap) {
    if (!u || !out_url || cap == 0u) return false;
    int n;
    if (u->port >= 0) {
        n = snprintf(out_url, cap, "%s://%s%s:%d%s%s%s",
                     u->scheme, u->userinfo, u->host, u->port, u->path, u->query, u->frag);
    } else {
        n = snprintf(out_url, cap, "%s://%s%s%s%s%s",
                     u->scheme, u->userinfo, u->host, u->path, u->query, u->frag);
    }
    return n > 0 && (size_t)n < cap;
}

static const ArcrDomNode *find_base_href_node_rec(const ArcrDocument *doc, const ArcrDomNode *n, int *budget) {
    if (!doc || !n || *budget <= 0) return NULL;
    (*budget)--;
    if (n->kind == ARCR_NODE_ELEMENT && doc->atoms) {
        const char *tn = arcr_atom_cstr(doc->atoms, n->tag);
        if (tn && arcr_str_eq_nocase(tn, "base")) {
            for (const ArcrAttr *a = n->first_attr; a != NULL; a = a->next) {
                const char *an = arcr_atom_cstr(doc->atoms, a->name);
                if (an && arcr_str_eq_nocase(an, "href") && a->value) {
                    return n;
                }
            }
        }
    }
    for (const ArcrDomNode *c = n->first_child; c != NULL; c = c->next_sibling) {
        const ArcrDomNode *hit = find_base_href_node_rec(doc, c, budget);
        if (hit) return hit;
    }
    return NULL;
}

void web_effective_base_url(const ArcrJsVm *vm, char *out_url, size_t cap) {
    if (!out_url || cap == 0u) return;
    out_url[0] = '\0';
    if (!vm) {
        arcr_str_copy(out_url, cap, "about:blank");
        return;
    }
    if (vm->is_worker_vm && vm->worker_script_url[0] != '\0') {
        arcr_str_copy(out_url, cap, vm->worker_script_url);
        return;
    }

    const char *doc_url = "about:blank";
    if (vm->host.get_current_url) {
        const char *u = vm->host.get_current_url(vm->host.userdata);
        if (u && u[0] != '\0') doc_url = u;
    }

    const char *base_href = NULL;
    if (vm->host.doc) {
        if (vm->host.doc->base_href && vm->host.doc->base_href[0] != '\0') {
            base_href = vm->host.doc->base_href;
        } else if (vm->host.doc->root && vm->host.doc->atoms) {
            int budget = 512;
            const ArcrDomNode *bn = find_base_href_node_rec(vm->host.doc, vm->host.doc->root, &budget);
            if (bn) {
                for (const ArcrAttr *a = bn->first_attr; a != NULL; a = a->next) {
                    const char *an = arcr_atom_cstr(vm->host.doc->atoms, a->name);
                    if (an && arcr_str_eq_nocase(an, "href") && a->value && a->value[0] != '\0') {
                        base_href = a->value;
                        break;
                    }
                }
            }
        }
    }

    if (base_href && base_href[0] != '\0') {
        char resolved[2048];
        if (web_resolve_url(doc_url, base_href, resolved, sizeof(resolved))) {
            arcr_str_copy(out_url, cap, resolved);
            return;
        }
    }
    arcr_str_copy(out_url, cap, doc_url);
}

void web_compute_origin(const char *url, char *out_origin, size_t cap) {
    if (!out_origin || cap == 0u) return;
    arcr_str_copy(out_origin, cap, "null");
    if (!url || url[0] == '\0') return;

    char clean[2048];
    if (sanitize_url_input(url, clean, sizeof(clean)) == (size_t)-1 || clean[0] == '\0') {
        return;
    }

    if (arcr_str_starts_with_nocase(clean, "blob:")) {
        web_compute_origin(clean + 5, out_origin, cap);
        return;
    }

    ParsedHierUrl pu;
    if (!parse_hierarchical_url(clean, &pu)) return;

    /* Tuple origins apply to http, https, ws, wss, ftp; file: has opaque "null" origin */
    if (strcmp(pu.scheme, "http") != 0 &&
        strcmp(pu.scheme, "https") != 0 &&
        strcmp(pu.scheme, "ws") != 0 &&
        strcmp(pu.scheme, "wss") != 0 &&
        strcmp(pu.scheme, "ftp") != 0) {
        return;
    }
    if (pu.host[0] == '\0') return;

    if (pu.port >= 0) {
        snprintf(out_origin, cap, "%s://%s:%d", pu.scheme, pu.host, pu.port);
    } else {
        snprintf(out_origin, cap, "%s://%s", pu.scheme, pu.host);
    }
}

bool web_resolve_url(const char *base_url, const char *rel_url, char *out_url, size_t out_cap) {
    if (!out_url || out_cap == 0u) return false;
    out_url[0] = '\0';
    if (!rel_url) return false;

    char rel_clean[4096];
    size_t rlen = sanitize_url_input(rel_url, rel_clean, sizeof(rel_clean));
    if (rlen == (size_t)-1) return false;

    char base_clean[2048];
    base_clean[0] = '\0';
    if (base_url && base_url[0] != '\0') {
        if (sanitize_url_input(base_url, base_clean, sizeof(base_clean)) == (size_t)-1) {
            return false;
        }
    }

    /* 1. Check if rel_url carries an explicit scheme */
    char rel_scheme[32];
    rel_scheme[0] = '\0';
    size_t rcolon = extract_url_scheme(rel_clean, rel_scheme, sizeof(rel_scheme));
    if (rcolon > 0u) {
        if (is_special_scheme(rel_scheme)) {
            const char *after = rel_clean + rcolon + 1u;
            /* WHATWG URL: same-scheme without "//" (e.g. "http:sub.js" against "http://ex/a/b") */
            if (after[0] != '/' && after[0] != '\\' && base_clean[0] != '\0') {
                ParsedHierUrl bu;
                if (parse_hierarchical_url(base_clean, &bu) && strcmp(bu.scheme, rel_scheme) == 0) {
                    return web_resolve_url(base_clean, after, out_url, out_cap);
                }
            }
            ParsedHierUrl ru;
            if (!parse_hierarchical_url(rel_clean, &ru)) return false;
            return format_hierarchical_url(&ru, out_url, out_cap);
        }

        /* Non-special scheme (data:, blob:, about:, mailto:, javascript:, urn:, etc.) */
        if (rlen + 1u > out_cap) return false;
        for (size_t i = 0u; i < rcolon; ++i) out_url[i] = rel_scheme[i];
        memcpy(out_url + rcolon, rel_clean + rcolon, rlen - rcolon + 1u);
        return true;
    }

    /* 2. Relative reference requires a valid base URL */
    if (base_clean[0] == '\0') return false;

    ParsedHierUrl base_u;
    if (!parse_hierarchical_url(base_clean, &base_u)) {
        /* Non-hierarchical base (e.g. "about:blank"): only "" or "#hash" can resolve */
        size_t bcolon = extract_url_scheme(base_clean, NULL, 0u);
        if (bcolon == 0u) return false;
        if (rlen == 0u) {
            const char *hash = strchr(base_clean, '#');
            size_t bl = hash ? (size_t)(hash - base_clean) : strlen(base_clean);
            if (bl + 1u > out_cap) return false;
            memcpy(out_url, base_clean, bl);
            out_url[bl] = '\0';
            return true;
        }
        if (rel_clean[0] == '#') {
            const char *hash = strchr(base_clean, '#');
            size_t bl = hash ? (size_t)(hash - base_clean) : strlen(base_clean);
            if (bl + rlen + 1u > out_cap) return false;
            memcpy(out_url, base_clean, bl);
            memcpy(out_url + bl, rel_clean, rlen + 1u);
            return true;
        }
        return false;
    }

    /* 3. Empty relative URL -> base without fragment */
    if (rlen == 0u) {
        base_u.frag[0] = '\0';
        return format_hierarchical_url(&base_u, out_url, out_cap);
    }

    /* 4. Protocol-relative "//authority/..." */
    bool special_base = is_special_scheme(base_u.scheme);
    if ((rel_clean[0] == '/' || (special_base && rel_clean[0] == '\\')) &&
        (rel_clean[1] == '/' || (special_base && rel_clean[1] == '\\'))) {
        char combined[4200];
        int cn = snprintf(combined, sizeof(combined), "%s:%s", base_u.scheme, rel_clean);
        if (cn <= 0 || (size_t)cn >= sizeof(combined)) return false;
        ParsedHierUrl ru;
        if (!parse_hierarchical_url(combined, &ru)) return false;
        return format_hierarchical_url(&ru, out_url, out_cap);
    }

    /* 5. Fragment-only "#..." */
    if (rel_clean[0] == '#') {
        arcr_str_copy(base_u.frag, sizeof(base_u.frag), rel_clean);
        return format_hierarchical_url(&base_u, out_url, out_cap);
    }

    /* 6. Query-only "?..." */
    if (rel_clean[0] == '?') {
        base_u.frag[0] = '\0';
        const char *h = strchr(rel_clean, '#');
        if (h) {
            size_t ql = (size_t)(h - rel_clean);
            if (ql >= sizeof(base_u.query)) ql = sizeof(base_u.query) - 1u;
            memcpy(base_u.query, rel_clean, ql);
            base_u.query[ql] = '\0';
            arcr_str_copy(base_u.frag, sizeof(base_u.frag), h);
        } else {
            arcr_str_copy(base_u.query, sizeof(base_u.query), rel_clean);
        }
        return format_hierarchical_url(&base_u, out_url, out_cap);
    }

    /* 7. Absolute-path "/..." or relative-path "sub/..." */
    base_u.query[0] = '\0';
    base_u.frag[0]  = '\0';

    const char *p = rel_clean;
    while (*p != '\0' && *p != '?' && *p != '#') ++p;
    char rel_path[2048];
    size_t rplen = (size_t)(p - rel_clean);
    if (rplen >= sizeof(rel_path)) rplen = sizeof(rel_path) - 1u;
    memcpy(rel_path, rel_clean, rplen);
    rel_path[rplen] = '\0';

    if (*p == '?') {
        const char *qs = p;
        while (*p != '\0' && *p != '#') ++p;
        size_t ql = (size_t)(p - qs);
        if (ql >= sizeof(base_u.query)) ql = sizeof(base_u.query) - 1u;
        memcpy(base_u.query, qs, ql);
        base_u.query[ql] = '\0';
    }
    if (*p == '#') {
        arcr_str_copy(base_u.frag, sizeof(base_u.frag), p);
    }

    char merged[4096];
    if (rel_path[0] == '/' || (special_base && rel_path[0] == '\\')) {
        arcr_str_copy(merged, sizeof(merged), rel_path);
    } else {
        const char *last_slash = strrchr(base_u.path, '/');
        if (last_slash) {
            size_t dir_len = (size_t)(last_slash - base_u.path) + 1u;
            if (dir_len >= sizeof(merged)) dir_len = sizeof(merged) - 1u;
            memcpy(merged, base_u.path, dir_len);
            merged[dir_len] = '\0';
            arcr_str_copy(merged + dir_len, sizeof(merged) - dir_len, rel_path);
        } else {
            snprintf(merged, sizeof(merged), "/%s", rel_path);
        }
    }

    remove_dot_segments(merged, base_u.path, sizeof(base_u.path));
    return format_hierarchical_url(&base_u, out_url, out_cap);
}

/* =========================================================================
 * 3. Console & Structured Error Diagnostics + Guarded Microtask Drain
 * ========================================================================= */
static void web_emit_raw_console(ArcrJsVm *vm, const char *line) {
    if (!vm || !vm->host.console_log || !line) return;
    uint64_t t0 = arcr_platform_monotonic_usec();
    web_host_call_enter(vm);
    vm->host.console_log(vm->host.userdata, line);
    web_host_call_leave(vm, t0);
}

void web_console_log_line(ArcrJsVm *vm, const char *level_prefix, const char *msg) {
    if (!vm || !vm->host.console_log) return;
    const char *m = msg ? msg : "";
    if (level_prefix && level_prefix[0] != '\0') {
        char buf[4096];
        snprintf(buf, sizeof(buf), "[%s] %s", level_prefix, m);
        web_emit_raw_console(vm, buf);
    } else {
        web_emit_raw_console(vm, m);
    }
}

void web_log_ajs_error(ArcrJsVm *vm, const char *tag_override, const AjsErrorInfo *err) {
    if (!vm || !vm->host.console_log) return;

    const char *tag = "JS RuntimeError";
    if (tag_override && tag_override[0] != '\0') {
        tag = tag_override;
    } else if (err) {
        if (err->phase == AJS_ERR_PARSE)           tag = "JS ParseError";
        else if (err->phase == AJS_ERR_RESOLUTION) tag = "JS ResolutionError";
    }

    if (!err) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[%s] Unknown error", tag);
        web_emit_raw_console(vm, buf);
        return;
    }

    const char *url  = (err->url[0] != '\0') ? err->url : "<anonymous>";
    const char *name = (err->name[0] != '\0') ? err->name : "Error";
    const char *msg  = err->message;
    uint32_t    line = err->line;
    uint32_t    col  = (err->column > 0u) ? err->column : 1u;

    char formatted[4096];
    int n;
    if (line > 0u) {
        n = snprintf(formatted, sizeof(formatted), "[%s] %s:%u:%u %s: %s",
                     tag, url, line, col, name, msg);
    } else {
        n = snprintf(formatted, sizeof(formatted), "[%s] %s %s: %s",
                     tag, url, name, msg);
    }
    if (n > 0 && (size_t)n + 2u < sizeof(formatted) && err->stack[0] != '\0') {
        snprintf(formatted + (size_t)n, sizeof(formatted) - (size_t)n, "\n%s", err->stack);
    }
    web_emit_raw_console(vm, formatted);
}

bool web_drain_microtasks_guarded(ArcrJsVm *vm) {
    if (!vm || !vm->avm) return false;
    bool armed_locally = false;
    if (!vm->interrupt_armed) {
        vm->deadline_usec   = arcr_platform_monotonic_usec() + ARCR_JS_TURN_DEADLINE_USEC;
        vm->interrupt_armed = true;
        armed_locally       = true;
    }

    bool ok = ajs_drain_microtasks(vm->avm);
    if (!ok || ajs_has_exception(vm->avm)) {
        AjsErrorInfo ei;
        memset(&ei, 0, sizeof(ei));
        (void)ajs_take_exception(vm->avm, &ei);
        web_log_ajs_error(vm, NULL, &ei);
        ok = false;
    }

    if (armed_locally) {
        vm->interrupt_armed = false;
    }
    return ok;
}

/* =========================================================================
 * 4. Interrupt Watchdog & Promise Rejection Tracker Callbacks
 * ========================================================================= */
static bool web_ajs_interrupt_cb(AjsVm *avm, void *userdata) {
    (void)avm;
    const ArcrJsVm *vm = (const ArcrJsVm *)userdata;
    if (!vm || !vm->interrupt_armed || vm->host_call_depth > 0) return false;
    return arcr_platform_monotonic_usec() >= vm->deadline_usec;
}

static void web_ajs_rejection_cb(AjsVm *avm, AjsValue promise, AjsValue reason,
                                 bool is_handled, void *userdata) {
    ArcrJsVm *vm = (ArcrJsVm *)userdata;
    if (!vm || !avm) return;

    bool prevented = false;
    if (vm->pin_unhandled_rejection != 0u) {
        AjsHandleScope sc;
        ajs_handle_scope_open(avm, &sc);
        AjsValue fn = ajs_get_pinned(avm, vm->pin_unhandled_rejection);
        if (ajs_is_callable(avm, fn)) {
            AjsValue args[3] = { promise, reason, ajs_bool(is_handled) };
            AjsValue r = ajs_call(avm, fn, ajs_global_object(avm), 3, args);
            if (ajs_has_exception(avm)) {
                ajs_clear_exception(avm);
            } else {
                prevented = ajs_to_bool(avm, r);
            }
        }
        ajs_handle_scope_close(avm, &sc);
    }

    if (!is_handled && !prevented && vm->host.console_log) {
        AjsHandleScope sc;
        ajs_handle_scope_open(avm, &sc);
        char *rstr = ajs_to_cstring(avm, reason);
        if (ajs_has_exception(avm)) ajs_clear_exception(avm);
        web_console_log_line(vm, "Unhandled Promise Rejection", rstr ? rstr : "<unprintable reason>");
        if (rstr) ajs_free_cstring(avm, rstr);
        ajs_handle_scope_close(avm, &sc);
    }
}

/* =========================================================================
 * 5. HTML Import Maps & ES Module Graph Loader (Resolve, Load, Cache, Eval)
 * ========================================================================= */
static bool is_url_like_specifier(const char *s) {
    if (!s || s[0] == '\0') return false;
    if (s[0] == '/') return true;
    if (s[0] == '.' && s[1] == '/') return true;
    if (s[0] == '.' && s[1] == '.' && s[2] == '/') return true;
    return extract_url_scheme(s, NULL, 0u) > 0u;
}

static bool web_is_javascript_mime(const char *mime) {
    if (!mime) return false;
    const char *s = mime;
    size_t len = strlen(mime);
    const char *semi = memchr(s, ';', len);
    if (semi) len = (size_t)(semi - s);
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;

    return arcr_str_eq_nocase_n(s, len, "text/javascript", 15u) ||
           arcr_str_eq_nocase_n(s, len, "application/javascript", 22u) ||
           arcr_str_eq_nocase_n(s, len, "application/x-javascript", 24u) ||
           arcr_str_eq_nocase_n(s, len, "text/ecmascript", 15u) ||
           arcr_str_eq_nocase_n(s, len, "application/ecmascript", 22u) ||
           arcr_str_eq_nocase_n(s, len, "text/jscript", 12u) ||
           arcr_str_eq_nocase_n(s, len, "text/livescript", 15u) ||
           arcr_str_eq_nocase_n(s, len, "text/javascript1.0", 18u) ||
           arcr_str_eq_nocase_n(s, len, "text/javascript1.1", 18u) ||
           arcr_str_eq_nocase_n(s, len, "text/javascript1.2", 18u) ||
           arcr_str_eq_nocase_n(s, len, "text/javascript1.3", 18u) ||
           arcr_str_eq_nocase_n(s, len, "text/javascript1.4", 18u) ||
           arcr_str_eq_nocase_n(s, len, "text/javascript1.5", 18u);
}

static int hex_digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool web_decode_data_url_source(const char *url, char *out_mime, size_t mime_cap,
                                       char **out_code, size_t *out_len) {
    if (out_code) *out_code = NULL;
    if (out_len)  *out_len  = 0u;
    if (out_mime && mime_cap > 0u) out_mime[0] = '\0';
    if (!url || !arcr_str_starts_with_nocase(url, "data:")) return false;

    const char *p = url + 5;
    const char *comma = strchr(p, ',');
    if (!comma) return false;

    size_t meta_len = (size_t)(comma - p);
    bool is_b64 = false;
    if (meta_len >= 7u && arcr_str_eq_nocase_n(comma - 7, 7u, ";base64", 7u)) {
        is_b64 = true;
        meta_len -= 7u;
    }

    if (out_mime && mime_cap > 0u) {
        if (meta_len == 0u) {
            arcr_str_copy(out_mime, mime_cap, "text/plain;charset=US-ASCII");
        } else {
            size_t cp = (meta_len < mime_cap - 1u) ? meta_len : (mime_cap - 1u);
            memcpy(out_mime, p, cp);
            out_mime[cp] = '\0';
        }
    }

    const char *payload = comma + 1;
    size_t plen = strlen(payload);
    if (is_b64) {
        uint8_t *raw = NULL;
        size_t   rlen = 0u;
        if (!arcr_base64_decode(payload, plen, &raw, &rlen)) return false;
        char *str = (char *)malloc(rlen + 1u);
        if (!str) {
            free(raw);
            return false;
        }
        if (raw && rlen > 0u) memcpy(str, raw, rlen);
        str[rlen] = '\0';
        free(raw);
        *out_code = str;
        *out_len  = rlen;
        return true;
    }

    char *buf = (char *)malloc(plen + 1u);
    if (!buf) return false;
    size_t o = 0u;
    for (size_t i = 0u; i < plen; ++i) {
        if (payload[i] == '%' && i + 2u < plen) {
            int hi = hex_digit_val(payload[i + 1u]);
            int lo = hex_digit_val(payload[i + 2u]);
            if (hi >= 0 && lo >= 0) {
                buf[o++] = (char)((hi << 4) | lo);
                i += 2u;
                continue;
            }
        }
        buf[o++] = payload[i];
    }
    buf[o] = '\0';
    *out_code = buf;
    *out_len  = o;
    return true;
}

static int cmp_import_entries_desc(const void *a, const void *b) {
    const WebImportMapEntry *ea = (const WebImportMapEntry *)a;
    const WebImportMapEntry *eb = (const WebImportMapEntry *)b;
    size_t la = ea->key ? strlen(ea->key) : 0u;
    size_t lb = eb->key ? strlen(eb->key) : 0u;
    if (la != lb) return (lb > la) ? 1 : -1;
    if (!ea->key || !eb->key) return 0;
    return strcmp(eb->key, ea->key);
}

static int cmp_import_scopes_desc(const void *a, const void *b) {
    const WebImportScope *sa = (const WebImportScope *)a;
    const WebImportScope *sb = (const WebImportScope *)b;
    size_t la = sa->scope_prefix ? strlen(sa->scope_prefix) : 0u;
    size_t lb = sb->scope_prefix ? strlen(sb->scope_prefix) : 0u;
    if (la != lb) return (lb > la) ? 1 : -1;
    if (!sa->scope_prefix || !sb->scope_prefix) return 0;
    return strcmp(sb->scope_prefix, sa->scope_prefix);
}

static bool upsert_import_entry(WebImportMapEntry **entries, size_t *count, size_t *cap,
                                const char *key, const char *target_or_null) {
    for (size_t i = 0u; i < *count; ++i) {
        if (strcmp((*entries)[i].key, key) == 0) {
            char *nt = target_or_null ? web_heap_strdup(target_or_null) : NULL;
            if (target_or_null && !nt) return false;
            free((*entries)[i].target_url);
            (*entries)[i].target_url = nt;
            return true;
        }
    }
    if (*count == *cap) {
        size_t ncap = (*cap == 0u) ? 8u : (*cap * 2u);
        WebImportMapEntry * narr = (WebImportMapEntry *)realloc(*entries, ncap * sizeof(WebImportMapEntry));
        if (!narr) return false;
        *entries = narr;
        *cap     = ncap;
    }
    char *nk = web_heap_strdup(key);
    char *nt = target_or_null ? web_heap_strdup(target_or_null) : NULL;
    if (!nk || (target_or_null && !nt)) {
        free(nk);
        free(nt);
        return false;
    }
    (*entries)[*count].key        = nk;
    (*entries)[*count].target_url = nt;
    (*count)++;
    return true;
}

static AjsValue web_object_keys(AjsVm *avm, AjsValue obj) {
    AjsValue glob    = ajs_global_object(avm);
    AjsValue obj_cto = ajs_get_prop_str(avm, glob, "Object");
    AjsValue keys_fn = ajs_get_prop_str(avm, obj_cto, "keys");
    if (!ajs_is_callable(avm, keys_fn)) return ajs_new_array(avm, 0u);
    AjsValue res = ajs_call(avm, keys_fn, obj_cto, 1, &obj);
    if (ajs_has_exception(avm)) {
        ajs_clear_exception(avm);
        return ajs_new_array(avm, 0u);
    }
    return res;
}

static bool parse_specifier_map_object(ArcrJsVm *vm, AjsVm *avm, AjsValue map_obj,
                                       const char *eff_base,
                                       WebImportMapEntry **io_entries,
                                       size_t *io_count, size_t *io_cap) {
    if (ajs_is_nullish(map_obj) || !ajs_is_object(avm, map_obj) || ajs_is_array(avm, map_obj)) {
        web_console_log_line(vm, "JS ImportMapError",
                             "TypeError: Import map specifier map must be a non-array JSON object.");
        return false;
    }

    AjsValue keys_arr = web_object_keys(avm, map_obj);
    uint32_t klen = 0u;
    (void)ajs_to_uint32(avm, ajs_get_prop_str(avm, keys_arr, "length"), &klen);

    for (uint32_t i = 0u; i < klen; ++i) {
        AjsValue kval = ajs_get_prop_index(avm, keys_arr, i);
        char *raw_key = ajs_to_cstring(avm, kval);
        if (!raw_key) continue;

        if (raw_key[0] == '\0') {
            web_console_log_line(vm, "JS ImportMapWarning", "Ignoring empty specifier key in import map.");
            ajs_free_cstring(avm, raw_key);
            continue;
        }

        char norm_key_buf[2048];
        const char *norm_key = raw_key;
        if (is_url_like_specifier(raw_key) &&
            web_resolve_url(eff_base, raw_key, norm_key_buf, sizeof(norm_key_buf))) {
            norm_key = norm_key_buf;
        }

        AjsValue vval = ajs_get_prop_str(avm, map_obj, raw_key);
        if (!ajs_is_string(avm, vval)) {
            web_console_log_line(vm, "JS ImportMapWarning",
                                 "Invalid non-string target address in import map; mapping to null.");
            (void)upsert_import_entry(io_entries, io_count, io_cap, norm_key, NULL);
            ajs_free_cstring(avm, raw_key);
            continue;
        }

        char *raw_addr = ajs_to_cstring(avm, vval);
        char resolved_addr[2048];
        bool valid_addr = false;
        if (raw_addr && is_url_like_specifier(raw_addr) &&
            web_resolve_url(eff_base, raw_addr, resolved_addr, sizeof(resolved_addr))) {
            size_t nk_len = strlen(norm_key);
            size_t ra_len = strlen(resolved_addr);
            if (nk_len > 0u && norm_key[nk_len - 1u] == '/' &&
                (ra_len == 0u || resolved_addr[ra_len - 1u] != '/')) {
                web_console_log_line(vm, "JS ImportMapWarning",
                                     "Trailing-slash specifier key mapped to address without trailing slash.");
                valid_addr = false;
            } else {
                valid_addr = true;
            }
        } else {
            web_console_log_line(vm, "JS ImportMapWarning",
                                 "Unresolvable or bare target address in import map; mapping to null.");
        }

        (void)upsert_import_entry(io_entries, io_count, io_cap,
                                  norm_key, valid_addr ? resolved_addr : NULL);
        if (raw_addr) ajs_free_cstring(avm, raw_addr);
        ajs_free_cstring(avm, raw_key);
    }

    if (*io_count > 1u) {
        qsort(*io_entries, *io_count, sizeof(WebImportMapEntry), cmp_import_entries_desc);
    }
    return true;
}

static WebImportScope *find_or_add_import_scope(WebImportMap *im, const char *scope_prefix) {
    for (size_t i = 0u; i < im->scope_count; ++i) {
        if (strcmp(im->scopes[i].scope_prefix, scope_prefix) == 0) {
            return &im->scopes[i];
        }
    }
    if (im->scope_count == im->scope_cap) {
        size_t ncap = (im->scope_cap == 0u) ? 4u : (im->scope_cap * 2u);
        WebImportScope *narr = (WebImportScope *)realloc(im->scopes, ncap * sizeof(WebImportScope));
        if (!narr) return NULL;
        im->scopes    = narr;
        im->scope_cap = ncap;
    }
    char *sp = web_heap_strdup(scope_prefix);
    if (!sp) return NULL;
    WebImportScope *sc = &im->scopes[im->scope_count++];
    memset(sc, 0, sizeof(*sc));
    sc->scope_prefix = sp;
    return sc;
}

static bool web_register_import_map(ArcrJsVm *vm, const char *json_utf8, size_t json_len,
                                    const char *base_url) {
    if (!vm || !vm->avm || !json_utf8) return false;
    if (vm->import_map.locked) {
        web_console_log_line(vm, "JS ImportMapError",
                             "An import map cannot be registered after module loading has started.");
        return false;
    }

    AjsVm *avm = vm->avm;
    AjsHandleScope hsc;
    ajs_handle_scope_open(avm, &hsc);

    AjsValue root = ajs_json_parse(avm, json_utf8, json_len);
    if (ajs_has_exception(avm)) {
        AjsErrorInfo ei;
        memset(&ei, 0, sizeof(ei));
        (void)ajs_take_exception(avm, &ei);
        web_log_ajs_error(vm, "JS ImportMapError", &ei);
        ajs_handle_scope_close(avm, &hsc);
        return false;
    }

    if (ajs_is_nullish(root) || !ajs_is_object(avm, root) || ajs_is_array(avm, root)) {
        web_console_log_line(vm, "JS ImportMapError",
                             "TypeError: Import map top-level value must be a JSON object.");
        ajs_handle_scope_close(avm, &hsc);
        return false;
    }

    char eff_base[2048];
    if (base_url && base_url[0] != '\0') {
        arcr_str_copy(eff_base, sizeof(eff_base), base_url);
    } else {
        web_effective_base_url(vm, eff_base, sizeof(eff_base));
    }

    if (ajs_has_prop_str(avm, root, "imports")) {
        AjsValue imp_obj = ajs_get_prop_str(avm, root, "imports");
        if (!parse_specifier_map_object(vm, avm, imp_obj, eff_base,
                                        &vm->import_map.imports,
                                        &vm->import_map.import_count,
                                        &vm->import_map.import_cap)) {
            ajs_handle_scope_close(avm, &hsc);
            return false;
        }
    }

    if (ajs_has_prop_str(avm, root, "scopes")) {
        AjsValue scopes_obj = ajs_get_prop_str(avm, root, "scopes");
        if (ajs_is_nullish(scopes_obj) || !ajs_is_object(avm, scopes_obj) || ajs_is_array(avm, scopes_obj)) {
            web_console_log_line(vm, "JS ImportMapError",
                                 "TypeError: Import map 'scopes' value must be a JSON object.");
            ajs_handle_scope_close(avm, &hsc);
            return false;
        }

        AjsValue skeys = web_object_keys(avm, scopes_obj);
        uint32_t sklen = 0u;
        (void)ajs_to_uint32(avm, ajs_get_prop_str(avm, skeys, "length"), &sklen);

        for (uint32_t i = 0u; i < sklen; ++i) {
            AjsValue skv = ajs_get_prop_index(avm, skeys, i);
            char *raw_sp = ajs_to_cstring(avm, skv);
            if (!raw_sp) continue;

            char resolved_sp[2048];
            if (!web_resolve_url(eff_base, raw_sp, resolved_sp, sizeof(resolved_sp))) {
                web_console_log_line(vm, "JS ImportMapWarning",
                                     "Ignoring unresolvable scope prefix in import map.");
                ajs_free_cstring(avm, raw_sp);
                continue;
            }

            AjsValue smap = ajs_get_prop_str(avm, scopes_obj, raw_sp);
            ajs_free_cstring(avm, raw_sp);

            WebImportScope *sc_entry = find_or_add_import_scope(&vm->import_map, resolved_sp);
            if (!sc_entry || !parse_specifier_map_object(vm, avm, smap, eff_base,
                                                         &sc_entry->entries,
                                                         &sc_entry->count,
                                                         &sc_entry->cap)) {
                ajs_handle_scope_close(avm, &hsc);
                return false;
            }
        }

        if (vm->import_map.scope_count > 1u) {
            qsort(vm->import_map.scopes, vm->import_map.scope_count,
                  sizeof(WebImportScope), cmp_import_scopes_desc);
        }
    }

    ajs_handle_scope_close(avm, &hsc);
    return true;
}

static bool resolve_specifier_in_entries(const WebImportMapEntry *entries, size_t count,
                                         const char *lookup_key, const char *raw_spec,
                                         const char *eff_ref,
                                         char *out_resolved_url, size_t out_cap,
                                         char *out_err_msg, size_t err_cap,
                                         bool *out_matched) {
    *out_matched = false;
    if (!entries || count == 0u) return false;

    /* 1. Exact match check */
    for (size_t i = 0u; i < count; ++i) {
        const WebImportMapEntry *e = &entries[i];
        if (strcmp(e->key, lookup_key) == 0 || strcmp(e->key, raw_spec) == 0) {
            *out_matched = true;
            if (!e->target_url) {
                if (out_err_msg && err_cap > 0u) {
                    snprintf(out_err_msg, err_cap,
                             "Module specifier \"%s\" from \"%s\" is blocked by a null import map entry.",
                             raw_spec, eff_ref);
                }
                return false;
            }
            arcr_str_copy(out_resolved_url, out_cap, e->target_url);
            return true;
        }
    }

    /* 2. Longest trailing-'/' prefix match (entries are pre-sorted longest key first) */
    for (size_t i = 0u; i < count; ++i) {
        const WebImportMapEntry *e = &entries[i];
        size_t klen = strlen(e->key);
        if (klen == 0u || e->key[klen - 1u] != '/') continue;

        const char *after = NULL;
        if (strncmp(lookup_key, e->key, klen) == 0) {
            after = lookup_key + klen;
        } else if (strncmp(raw_spec, e->key, klen) == 0) {
            after = raw_spec + klen;
        }
        if (!after) continue;

        *out_matched = true;
        if (!e->target_url) {
            if (out_err_msg && err_cap > 0u) {
                snprintf(out_err_msg, err_cap,
                         "Module specifier \"%s\" from \"%s\" matched blocked prefix \"%s\" in import map.",
                         raw_spec, eff_ref, e->key);
            }
            return false;
        }

        char candidate[2048];
        size_t tlen = strlen(e->target_url);
        if (!web_resolve_url(e->target_url, after, candidate, sizeof(candidate)) ||
            strncmp(candidate, e->target_url, tlen) != 0) {
            if (out_err_msg && err_cap > 0u) {
                snprintf(out_err_msg, err_cap,
                         "Module specifier \"%s\" from \"%s\" cannot backtrack above mapped prefix \"%s\".",
                         raw_spec, eff_ref, e->target_url);
            }
            return false;
        }

        arcr_str_copy(out_resolved_url, out_cap, candidate);
        return true;
    }

    return false;
}

static bool scope_prefix_matches_referrer(const char *scope_prefix, const char *referrer_url) {
    if (!scope_prefix || !referrer_url) return false;
    size_t slen = strlen(scope_prefix);
    if (slen == 0u) return false;
    if (strcmp(referrer_url, scope_prefix) == 0) return true;
    if (scope_prefix[slen - 1u] == '/' && strncmp(referrer_url, scope_prefix, slen) == 0) {
        return true;
    }
    return false;
}

static bool web_ajs_module_resolve(AjsVm *avm, const char *specifier, const char *referrer_url,
                                   char *out_resolved_url, size_t out_cap,
                                   char *out_err_msg, size_t err_cap, void *userdata) {
    (void)avm;
    if (out_resolved_url && out_cap > 0u) out_resolved_url[0] = '\0';
    if (out_err_msg && err_cap > 0u) out_err_msg[0] = '\0';

    ArcrJsVm *vm = (ArcrJsVm *)userdata;
    if (!vm || !specifier || specifier[0] == '\0') {
        if (out_err_msg && err_cap > 0u) {
            arcr_str_copy(out_err_msg, err_cap, "Empty module specifier cannot be resolved.");
        }
        return false;
    }

    /* Per HTML spec: once any module resolution begins, no further import maps may be registered */
    vm->import_map.locked = true;

    char eff_ref[2048];
    if (referrer_url && referrer_url[0] != '\0' && referrer_url[0] != '<') {
        arcr_str_copy(eff_ref, sizeof(eff_ref), referrer_url);
    } else {
        web_effective_base_url(vm, eff_ref, sizeof(eff_ref));
    }

    bool url_like = is_url_like_specifier(specifier);
    char as_url[2048];
    as_url[0] = '\0';
    bool has_as_url = url_like && web_resolve_url(eff_ref, specifier, as_url, sizeof(as_url));
    const char *lookup_key = has_as_url ? as_url : specifier;

    /* 1. Check matching import map scopes (pre-sorted longest scope_prefix first) */
    for (size_t i = 0u; i < vm->import_map.scope_count; ++i) {
        const WebImportScope *sc = &vm->import_map.scopes[i];
        if (!scope_prefix_matches_referrer(sc->scope_prefix, eff_ref)) continue;

        bool matched = false;
        bool ok = resolve_specifier_in_entries(sc->entries, sc->count,
                                               lookup_key, specifier, eff_ref,
                                               out_resolved_url, out_cap,
                                               out_err_msg, err_cap, &matched);
        if (matched) return ok;
    }

    /* 2. Check top-level import map imports */
    {
        bool matched = false;
        bool ok = resolve_specifier_in_entries(vm->import_map.imports, vm->import_map.import_count,
                                               lookup_key, specifier, eff_ref,
                                               out_resolved_url, out_cap,
                                               out_err_msg, err_cap, &matched);
        if (matched) return ok;
    }

    /* 3. Unmapped specifier: must be relative ('/', './', '../') or an absolute URL */
    if (has_as_url) {
        arcr_str_copy(out_resolved_url, out_cap, as_url);
        return true;
    }

    if (!url_like) {
        if (out_err_msg && err_cap > 0u) {
            snprintf(out_err_msg, err_cap,
                     "Failed to resolve bare module specifier \"%s\" from \"%s\": "
                     "Relative references must start with \"/\", \"./\", or \"../\".",
                     specifier, eff_ref);
        }
        return false;
    }

    if (out_err_msg && err_cap > 0u) {
        snprintf(out_err_msg, err_cap,
                 "Failed to resolve module URL \"%s\" relative to \"%s\".",
                 specifier, eff_ref);
    }
    return false;
}

static WebModuleCacheEntry *mod_cache_find(ArcrJsVm *vm, const char *url) {
    if (!vm || !url || url[0] == '\0') return NULL;
    for (size_t i = 0u; i < vm->mod_cache_count; ++i) {
        WebModuleCacheEntry *e = &vm->mod_cache[i];
        if ((e->requested_url && strcmp(e->requested_url, url) == 0) ||
            (e->final_url && strcmp(e->final_url, url) == 0)) {
            return e;
        }
    }
    return NULL;
}

static WebModuleCacheEntry *mod_cache_insert(ArcrJsVm *vm, const char *req_url,
                                             const char *final_url, AjsModuleRecord *mod,
                                             bool has_err, const AjsErrorInfo *err_or_null) {
    if (!vm) return NULL;
    if (vm->mod_cache_count == vm->mod_cache_cap) {
        size_t ncap = (vm->mod_cache_cap == 0u) ? 16u : (vm->mod_cache_cap * 2u);
        WebModuleCacheEntry *narr = (WebModuleCacheEntry *)realloc(
            vm->mod_cache, ncap * sizeof(WebModuleCacheEntry));
        if (!narr) return NULL;
        vm->mod_cache     = narr;
        vm->mod_cache_cap = ncap;
    }
    WebModuleCacheEntry *e = &vm->mod_cache[vm->mod_cache_count++];
    memset(e, 0, sizeof(*e));
    e->requested_url = req_url ? web_heap_strdup(req_url) : NULL;
    e->final_url     = final_url ? web_heap_strdup(final_url) : NULL;
    e->mod           = mod;
    e->has_err       = has_err;
    if (err_or_null) {
        e->err_info = *err_or_null;
    }
    return e;
}

static AjsModuleRecord *web_ajs_module_load(AjsVm *avm, const char *resolved_url,
                                            const char *referrer_url,
                                            char *out_err_msg, size_t err_cap, void *userdata) {
    if (out_err_msg && err_cap > 0u) out_err_msg[0] = '\0';
    ArcrJsVm *vm = (ArcrJsVm *)userdata;
    if (!vm || !avm || !resolved_url || resolved_url[0] == '\0') {
        if (out_err_msg && err_cap > 0u) {
            arcr_str_copy(out_err_msg, err_cap, "Invalid empty module URL.");
        }
        return NULL;
    }

    vm->import_map.locked = true;

    /* 1. Consult module map cache first so diamond & circular graphs load/parse at most once */
    WebModuleCacheEntry *cached = mod_cache_find(vm, resolved_url);
    if (cached) {
        if (cached->has_err || !cached->mod) {
            if (out_err_msg && err_cap > 0u) {
                if (cached->err_info.phase == AJS_ERR_PARSE && cached->err_info.line > 0u) {
                    char full_msg[1280];
                    snprintf(full_msg, sizeof(full_msg), "%s:%u:%u %s: %s",
                             cached->err_info.url, cached->err_info.line,
                             cached->err_info.column > 0u ? cached->err_info.column : 1u,
                             cached->err_info.name, cached->err_info.message);
                    arcr_str_copy(out_err_msg, err_cap, full_msg);
                } else {
                    arcr_str_copy(out_err_msg, err_cap,
                                  cached->err_info.message[0]
                                      ? cached->err_info.message
                                      : "Cached module failed to load or parse.");
                }
            }
            return NULL;
        }
        return cached->mod;
    }

    char  *src_code       = NULL;
    size_t src_len        = 0u;
    bool   free_with_host = false;
    char   final_url[2048];
    char   mime[256];
    char   host_err[512];
    arcr_str_copy(final_url, sizeof(final_url), resolved_url);
    mime[0]     = '\0';
    host_err[0] = '\0';

    /* 2. Fetch module source from blob:, data:, or host load_module_source */
    if (arcr_str_starts_with_nocase(resolved_url, "blob:")) {
        const uint8_t *bbytes = NULL;
        size_t         blen   = 0u;
        if (!web_lookup_blob_url(vm, resolved_url, &bbytes, &blen, mime, sizeof(mime))) {
            snprintf(host_err, sizeof(host_err), "Failed to load blob module %.320s (revoked or not found)",
                     resolved_url);
        } else if (mime[0] != '\0' &&
                   !web_is_javascript_mime(mime) &&
                   !arcr_str_eq_nocase(mime, "application/octet-stream")) {
            snprintf(host_err, sizeof(host_err),
                     "Failed to load module script %.320s: Strict MIME type checking rejected \"%.96s\"",
                     resolved_url, mime);
        } else {
            src_code = web_heap_strdup_n((const char *)bbytes, blen);
            src_len  = blen;
            if (!src_code) arcr_str_copy(host_err, sizeof(host_err), "Out of memory loading blob module");
        }
    } else if (arcr_str_starts_with_nocase(resolved_url, "data:")) {
        if (!web_decode_data_url_source(resolved_url, mime, sizeof(mime), &src_code, &src_len)) {
            snprintf(host_err, sizeof(host_err), "Malformed data: module URL %.320s", resolved_url);
        } else if (!web_is_javascript_mime(mime)) {
            free(src_code);
            src_code = NULL;
            snprintf(host_err, sizeof(host_err),
                     "Failed to load data: module %.320s: Strict MIME type checking rejected \"%.96s\"",
                     resolved_url, mime);
        }
    } else if (vm->host.load_module_source) {
        const char *eff_ref = (referrer_url && referrer_url[0] != '\0') ? referrer_url : resolved_url;
        uint64_t t0 = arcr_platform_monotonic_usec();
        web_host_call_enter(vm);
        bool ok = vm->host.load_module_source(
            vm->host.userdata, resolved_url, eff_ref,
            &src_code, &src_len,
            final_url, sizeof(final_url),
            mime, sizeof(mime),
            host_err, sizeof(host_err));
        web_host_call_leave(vm, t0);

        if (!ok || !src_code) {
            if (src_code) {
                if (vm->host.free_host_buffer) vm->host.free_host_buffer(vm->host.userdata, src_code);
                else free(src_code);
                src_code = NULL;
            }
            if (host_err[0] == '\0') {
                snprintf(host_err, sizeof(host_err), "Failed to fetch module script %.320s", resolved_url);
            }
        } else if (mime[0] != '\0' && !web_is_javascript_mime(mime)) {
            if (vm->host.free_host_buffer) vm->host.free_host_buffer(vm->host.userdata, src_code);
            else free(src_code);
            src_code = NULL;
            snprintf(host_err, sizeof(host_err),
                     "Failed to load module script %.320s: Strict MIME type checking rejected \"%.96s\"",
                     final_url[0] ? final_url : resolved_url, mime);
        } else {
            free_with_host = true;
        }
    } else {
        snprintf(host_err, sizeof(host_err),
                 "Host module loader unavailable for external module %.320s", resolved_url);
    }

    if (!src_code) {
        AjsErrorInfo load_err;
        memset(&load_err, 0, sizeof(load_err));
        load_err.phase = AJS_ERR_RESOLUTION;
        arcr_str_copy(load_err.name, sizeof(load_err.name), "TypeError");
        arcr_str_copy(load_err.url, sizeof(load_err.url), resolved_url);
        arcr_str_copy(load_err.message, sizeof(load_err.message),
                      host_err[0] ? host_err : "Module load failed");
        (void)mod_cache_insert(vm, resolved_url, final_url, NULL, true, &load_err);
        if (out_err_msg && err_cap > 0u) {
            arcr_str_copy(out_err_msg, err_cap, load_err.message);
        }
        return NULL;
    }

    const char *actual_url = (final_url[0] != '\0') ? final_url : resolved_url;

    /* If HTTP redirect landed on an already-cached canonical URL, reuse its record */
    if (strcmp(actual_url, resolved_url) != 0) {
        WebModuleCacheEntry *redir_hit = mod_cache_find(vm, actual_url);
        if (redir_hit && redir_hit->mod) {
            if (free_with_host && vm->host.free_host_buffer) {
                vm->host.free_host_buffer(vm->host.userdata, src_code);
            } else {
                free(src_code);
            }
            (void)mod_cache_insert(vm, resolved_url, actual_url, redir_hit->mod, false, NULL);
            return redir_hit->mod;
        }
    }

    /* 3. Parse Source Text Module Record */
    AjsErrorInfo parse_err;
    memset(&parse_err, 0, sizeof(parse_err));
    AjsModuleRecord *mod = ajs_module_parse(avm, src_code, src_len, actual_url, &parse_err);

    if (free_with_host && vm->host.free_host_buffer) {
        vm->host.free_host_buffer(vm->host.userdata, src_code);
    } else {
        free(src_code);
    }

    if (!mod) {
        if (parse_err.phase == AJS_ERR_NONE) parse_err.phase = AJS_ERR_PARSE;
        if (parse_err.url[0] == '\0') arcr_str_copy(parse_err.url, sizeof(parse_err.url), actual_url);
        if (parse_err.name[0] == '\0') arcr_str_copy(parse_err.name, sizeof(parse_err.name), "SyntaxError");

        char full_msg[1280];
        if (parse_err.line > 0u) {
            snprintf(full_msg, sizeof(full_msg), "%s:%u:%u %s: %s",
                     parse_err.url, parse_err.line,
                     parse_err.column > 0u ? parse_err.column : 1u,
                     parse_err.name, parse_err.message);
        } else {
            snprintf(full_msg, sizeof(full_msg), "%s %s: %s",
                     parse_err.url, parse_err.name, parse_err.message);
        }
        (void)mod_cache_insert(vm, resolved_url, actual_url, NULL, true, &parse_err);
        if (out_err_msg && err_cap > 0u) {
            arcr_str_copy(out_err_msg, err_cap, full_msg);
        }
        return NULL;
    }

    (void)mod_cache_insert(vm, resolved_url, actual_url, mod, false, NULL);
    return mod;
}

/* Evaluates a parsed module record (link -> evaluate -> TLA & microtask settlement) */
static bool web_link_and_eval_module_record(ArcrJsVm *vm, AjsModuleRecord *mod, const char *eff_url) {
    if (!vm || !vm->avm || !mod) return false;
    AjsVm *avm = vm->avm;

    AjsErrorInfo err;
    memset(&err, 0, sizeof(err));

    vm->deadline_usec   = arcr_platform_monotonic_usec() + ARCR_JS_TURN_DEADLINE_USEC;
    vm->interrupt_armed = true;
    bool linked = ajs_module_link(avm, mod, &err);
    vm->interrupt_armed = false;

    if (!linked) {
        if (err.url[0] == '\0' && eff_url) arcr_str_copy(err.url, sizeof(err.url), eff_url);
        web_log_ajs_error(vm, "JS LinkError", &err);
        return false;
    }

    AjsHandleScope sc;
    ajs_handle_scope_open(avm, &sc);

    AjsValue prom_or_val = ajs_undefined();
    memset(&err, 0, sizeof(err));

    vm->deadline_usec   = arcr_platform_monotonic_usec() + ARCR_JS_TURN_DEADLINE_USEC;
    vm->interrupt_armed = true;
    bool eval_ok = ajs_module_evaluate(avm, mod, &prom_or_val, &err);
    vm->interrupt_armed = false;

    if (!eval_ok) {
        if (err.url[0] == '\0' && eff_url) arcr_str_copy(err.url, sizeof(err.url), eff_url);
        web_log_ajs_error(vm, "JS ModuleError", &err);
        (void)web_drain_microtasks_guarded(vm);
        ajs_handle_scope_close(avm, &sc);
        return false;
    }

    /* Track Top-Level Await (TLA) / module evaluation Promise rejection if returned */
    AjsValue tla_state = ajs_null();
    if (ajs_is_promise(avm, prom_or_val)) {
        AjsValue glob     = ajs_global_object(avm);
        AjsValue track_fn = ajs_get_prop_str(avm, glob, "__trackTla");
        if (ajs_is_callable(avm, track_fn)) {
            AjsValue targs[2] = {
                prom_or_val,
                ajs_string_cstr(avm, eff_url ? eff_url : "<module>")
            };
            tla_state = ajs_call(avm, track_fn, glob, 2, targs);
            if (ajs_has_exception(avm)) {
                ajs_clear_exception(avm);
                tla_state = ajs_null();
            }
        }
    }

    (void)web_drain_microtasks_guarded(vm);

    if (web_has_pending_async_work(vm)) {
        web_poll_async_work(vm, NULL);
        (void)web_drain_microtasks_guarded(vm);
    }

    if (!ajs_is_nullish(tla_state) && ajs_is_object(avm, tla_state)) {
        bool rejected = ajs_to_bool(avm, ajs_get_prop_str(avm, tla_state, "rejected"));
        if (rejected) {
            (void)ajs_set_prop_str(avm, tla_state, "logged", ajs_bool(true));
            AjsValue reason = ajs_get_prop_str(avm, tla_state, "reason");
            AjsErrorInfo tla_err;
            memset(&tla_err, 0, sizeof(tla_err));
            (void)ajs_throw(avm, reason);
            (void)ajs_take_exception(avm, &tla_err);
            if (tla_err.url[0] == '\0' && eff_url) {
                arcr_str_copy(tla_err.url, sizeof(tla_err.url), eff_url);
            }
            web_log_ajs_error(vm, "JS ModuleError", &tla_err);
            ajs_handle_scope_close(avm, &sc);
            return false;
        }
        (void)ajs_set_prop_str(avm, tla_state, "asyncPhase", ajs_bool(true));
    }

    ajs_handle_scope_close(avm, &sc);
    return true;
}

static bool web_eval_module(ArcrJsVm *vm, const char *utf8_code_or_null, size_t code_len,
                            const char *module_url) {
    if (!vm || !vm->avm) return false;

    vm->import_map.locked = true;
    if (!vm->is_worker_vm) {
        web_uid_sync_document(vm);
    }

    char page_base[2048];
    web_effective_base_url(vm, page_base, sizeof(page_base));

    char eff_url[2048];
    if (module_url && module_url[0] != '\0') {
        if (!web_resolve_url(page_base, module_url, eff_url, sizeof(eff_url))) {
            arcr_str_copy(eff_url, sizeof(eff_url), module_url);
        }
    } else {
        arcr_str_copy(eff_url, sizeof(eff_url), page_base);
    }

    AjsModuleRecord *mod = NULL;
    if (utf8_code_or_null != NULL) {
        /*
         * Check if this is a pre-fetched external async module (`eff_url != page_base`)
         * that was already imported earlier in the module graph; if so, reuse its record!
         * Inline `<script type="module">` tags share `eff_url == page_base` and each
         * execute as a distinct top-level inline module while keeping `import.meta.url == eff_url`.
         */
        bool is_distinct_mod_url = (strcmp(eff_url, page_base) != 0);
        if (is_distinct_mod_url) {
            WebModuleCacheEntry *existing = mod_cache_find(vm, eff_url);
            if (existing && existing->mod) {
                mod = existing->mod;
            }
        }
        if (!mod) {
            AjsErrorInfo parse_err;
            memset(&parse_err, 0, sizeof(parse_err));
            mod = ajs_module_parse(vm->avm, utf8_code_or_null, code_len, eff_url, &parse_err);
            if (!mod) {
                if (parse_err.url[0] == '\0') arcr_str_copy(parse_err.url, sizeof(parse_err.url), eff_url);
                web_log_ajs_error(vm, "JS ParseError", &parse_err);
                return false;
            }
            if (is_distinct_mod_url) {
                (void)mod_cache_insert(vm, eff_url, eff_url, mod, false, NULL);
            }
        }
    } else {
        char load_err_msg[512];
        load_err_msg[0] = '\0';
        mod = web_ajs_module_load(vm->avm, eff_url, page_base, load_err_msg, sizeof(load_err_msg), vm);
        if (!mod) {
            WebModuleCacheEntry *ce = mod_cache_find(vm, eff_url);
            if (ce && ce->has_err) {
                web_log_ajs_error(vm,
                                  ce->err_info.phase == AJS_ERR_PARSE ? "JS ParseError" : "JS ModuleError",
                                  &ce->err_info);
            } else {
                web_console_log_line(vm, "JS ModuleError",
                                     load_err_msg[0] ? load_err_msg : "Failed to load module");
            }
            return false;
        }
    }

    return web_link_and_eval_module_record(vm, mod, eff_url);
}

/* =========================================================================
 * 6. Classic Worker `importScripts()` Native Helper
 * ========================================================================= */
static AjsValue b_worker_import_scripts(AjsVm *avm, AjsValue this_val,
                                        int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 1) return ajs_undefined();

    AjsValue glob = ajs_global_object(avm);
    if (ajs_to_bool(avm, ajs_get_prop_str(avm, glob, "__workerIsModule"))) {
        return ajs_throw_error(avm, "TypeError",
                               "Failed to execute 'importScripts': Module scripts do not support importScripts().");
    }

    char *url = ajs_to_cstring(avm, argv[0]);
    if (!url) return ajs_undefined();

    char  *code           = NULL;
    size_t code_len       = 0u;
    bool   free_with_host = false;
    char   final_url[2048];
    arcr_str_copy(final_url, sizeof(final_url), url);

    if (arcr_str_starts_with_nocase(url, "data:")) {
        char mime[128];
        (void)web_decode_data_url_source(url, mime, sizeof(mime), &code, &code_len);
    } else if (arcr_str_starts_with_nocase(url, "blob:")) {
        const uint8_t *bb = NULL;
        size_t bl = 0u;
        if (web_lookup_blob_url(vm, url, &bb, &bl, NULL, 0u) && bb) {
            code = web_heap_strdup_n((const char *)bb, bl);
            code_len = bl;
        }
    } else if (vm->host.load_module_source) {
        char mime[128] = {0}, herr[256] = {0};
        uint64_t t0 = arcr_platform_monotonic_usec();
        web_host_call_enter(vm);
        if (vm->host.load_module_source(vm->host.userdata, url, vm->worker_script_url,
                                        &code, &code_len, final_url, sizeof(final_url),
                                        mime, sizeof(mime), herr, sizeof(herr))) {
            free_with_host = true;
        }
        web_host_call_leave(vm, t0);
    } else if (vm->host.http_fetch_ex || vm->host.http_fetch) {
        int st = 0;
        char ct[256] = {0}, reason[128] = {0};
        uint8_t *rb = NULL;
        size_t rblen = 0u;
        volatile int cflag = 0;
        uint64_t t0 = arcr_platform_monotonic_usec();
        web_host_call_enter(vm);
        bool ok = vm->host.http_fetch_ex
                      ? vm->host.http_fetch_ex(vm->host.userdata, url, "GET", NULL, NULL, 0u, true,
                                               &cflag, &st, final_url, sizeof(final_url),
                                               ct, sizeof(ct), NULL, NULL, &rb, &rblen,
                                               reason, sizeof(reason))
                      : vm->host.http_fetch(vm->host.userdata, url, "GET", NULL, 0u, NULL,
                                            &st, ct, sizeof(ct), &rb, &rblen);
        web_host_call_leave(vm, t0);
        if (ok && st >= 200 && st < 300 && rb) {
            code = web_heap_strdup_n((const char *)rb, rblen);
            code_len = rblen;
        }
        if (rb) {
            if (vm->host.free_host_buffer) vm->host.free_host_buffer(vm->host.userdata, rb);
            else if (vm->host.http_free)   vm->host.http_free(vm->host.userdata, rb);
            else free(rb);
        }
    }

    if (!code) {
        AjsValue exc = ajs_throw_error(avm, "Error", "NetworkError: Failed to importScripts('%s')", url);
        ajs_free_cstring(avm, url);
        return exc;
    }

    AjsErrorInfo ei;
    memset(&ei, 0, sizeof(ei));
    bool eval_ok = ajs_eval_script(avm, code, code_len,
                                   final_url[0] ? final_url : url, false, NULL, &ei);
    if (free_with_host && vm->host.free_host_buffer) {
        vm->host.free_host_buffer(vm->host.userdata, code);
    } else {
        free(code);
    }
    ajs_free_cstring(avm, url);

    if (!eval_ok) {
        return ajs_throw_error(avm, ei.name[0] ? ei.name : "Error", "%s", ei.message);
    }
    return ajs_undefined();
}

/* =========================================================================
 * 7. VM Creation, Destruction & Dedicated Worker Realm Helpers
 * ========================================================================= */
static void free_import_map_and_mod_cache(ArcrJsVm *vm) {
    if (!vm) return;

    for (size_t i = 0u; i < vm->import_map.import_count; ++i) {
        free(vm->import_map.imports[i].key);
        free(vm->import_map.imports[i].target_url);
    }
    free(vm->import_map.imports);
    vm->import_map.imports      = NULL;
    vm->import_map.import_count = 0u;
    vm->import_map.import_cap   = 0u;

    for (size_t s = 0u; s < vm->import_map.scope_count; ++s) {
        WebImportScope *sc = &vm->import_map.scopes[s];
        free(sc->scope_prefix);
        for (size_t i = 0u; i < sc->count; ++i) {
            free(sc->entries[i].key);
            free(sc->entries[i].target_url);
        }
        free(sc->entries);
    }
    free(vm->import_map.scopes);
    vm->import_map.scopes      = NULL;
    vm->import_map.scope_count = 0u;
    vm->import_map.scope_cap   = 0u;

    for (size_t i = 0u; i < vm->mod_cache_count; ++i) {
        WebModuleCacheEntry *ce = &vm->mod_cache[i];
        if (vm->avm) {
            if (ce->ns_pin)           ajs_unpin_value(vm->avm, ce->ns_pin);
            if (ce->eval_promise_pin) ajs_unpin_value(vm->avm, ce->eval_promise_pin);
        }
        free(ce->requested_url);
        free(ce->final_url);
    }
    free(vm->mod_cache);
    vm->mod_cache       = NULL;
    vm->mod_cache_count = 0u;
    vm->mod_cache_cap   = 0u;
}

static void unpin_vm_hooks(ArcrJsVm *vm) {
    if (!vm || !vm->avm) return;
    if (vm->pin_bridge)              { ajs_unpin_value(vm->avm, vm->pin_bridge);              vm->pin_bridge = 0u; }
    if (vm->pin_dispatch_event)      { ajs_unpin_value(vm->avm, vm->pin_dispatch_event);      vm->pin_dispatch_event = 0u; }
    if (vm->pin_tick_timers)         { ajs_unpin_value(vm->avm, vm->pin_tick_timers);         vm->pin_tick_timers = 0u; }
    if (vm->pin_has_timers)          { ajs_unpin_value(vm->avm, vm->pin_has_timers);          vm->pin_has_timers = 0u; }
    if (vm->pin_wrap_uid)            { ajs_unpin_value(vm->avm, vm->pin_wrap_uid);            vm->pin_wrap_uid = 0u; }
    if (vm->pin_set_ready_state)     { ajs_unpin_value(vm->avm, vm->pin_set_ready_state);     vm->pin_set_ready_state = 0u; }
    if (vm->pin_notify_popstate)     { ajs_unpin_value(vm->avm, vm->pin_notify_popstate);     vm->pin_notify_popstate = 0u; }
    if (vm->pin_unhandled_rejection) { ajs_unpin_value(vm->avm, vm->pin_unhandled_rejection); vm->pin_unhandled_rejection = 0u; }
}

void web_destroy_vm_internal(ArcrJsVm *vm) {
    if (!vm) return;

    /* 1. Cancel & join async fetch / Worker threads before tearing down AjsVm */
    web_crypto_streams_cleanup(vm);

    /* 2. Free Canvas 2D contexts, font cache, DOM tables & private session ref */
    web_canvas_cleanup(vm);
    web_dom_cleanup(vm);

    /* 3. Release import maps, module cache, pinned JS roots & AjsVm */
    free_import_map_and_mod_cache(vm);
    unpin_vm_hooks(vm);

    if (vm->avm) {
        ajs_vm_destroy(vm->avm);
        vm->avm = NULL;
    }
    free(vm);
}

static ArcrJsVm *web_create_vm_common(size_t heap_limit_bytes,
                                      const ArcrJsHostCallbacks *host_cb,
                                      bool is_worker,
                                      const char *worker_script_url) {
    ArcrJsVm *vm = (ArcrJsVm *)calloc(1u, sizeof(ArcrJsVm));
    if (!vm) return NULL;

    if (host_cb) {
        vm->host = *host_cb;
    }
    vm->heap_limit         = heap_limit_bytes;
    vm->is_worker_vm       = is_worker;
    vm->vm_instance_id     = web_alloc_vm_instance_id();
    vm->boot_usec          = arcr_platform_monotonic_usec();
    vm->next_offscreen_uid = 0x40000000u;
    vm->next_fetch_id      = 1u;
    vm->next_worker_id     = 1u;

    if (worker_script_url && worker_script_url[0] != '\0') {
        arcr_str_copy(vm->worker_script_url, sizeof(vm->worker_script_url), worker_script_url);
    }

    if (vm->host.private_browsing) {
        web_private_session_retain(vm);
    }

    AjsVmConfig cfg;
    ajs_default_config(&cfg);
    cfg.heap_limit_bytes   = heap_limit_bytes;
    cfg.interrupt_fn       = web_ajs_interrupt_cb;
    cfg.interrupt_userdata = vm;
    cfg.rejection_fn       = web_ajs_rejection_cb;
    cfg.rejection_userdata = vm;
    cfg.module_resolve_fn  = web_ajs_module_resolve;
    cfg.module_load_fn     = web_ajs_module_load;
    cfg.module_userdata    = vm;

    vm->avm = ajs_vm_create(&cfg);
    if (!vm->avm) {
        web_destroy_vm_internal(vm);
        return NULL;
    }
    ajs_vm_set_userdata(vm->avm, vm);

    AjsHandleScope sc;
    ajs_handle_scope_open(vm->avm, &sc);

    AjsValue bridge_obj = ajs_new_object(vm->avm);
    vm->pin_bridge      = ajs_pin_value(vm->avm, bridge_obj);

    web_dom_register_bridge(vm, bridge_obj);
    web_canvas_register_bridge(vm, bridge_obj);
    web_crypto_streams_register_bridge(vm, bridge_obj);
    if (is_worker) {
        web_bind_fn(vm->avm, bridge_obj, "importScripts", b_worker_import_scripts, 1, vm);
    }

    if (!is_worker && vm->host.doc) {
        web_uid_sync_document(vm);
    }

    bool boot_ok = web_bootstrap_install(vm, bridge_obj);
    ajs_handle_scope_close(vm->avm, &sc);

    if (!boot_ok) {
        web_destroy_vm_internal(vm);
        return NULL;
    }
    return vm;
}

static ArcrJsVm *web_create_vm(size_t heap_limit_bytes, const ArcrJsHostCallbacks *host_cb) {
    return web_create_vm_common(heap_limit_bytes, host_cb, false, NULL);
}

static void web_destroy_vm(ArcrJsVm *vm) {
    web_destroy_vm_internal(vm);
}

ArcrJsVm *web_create_worker_vm(size_t heap_limit_bytes, const ArcrJsHostCallbacks *host_cb,
                               const char *worker_script_url) {
    return web_create_vm_common(heap_limit_bytes, host_cb, true, worker_script_url);
}

static bool web_eval_script_ex(ArcrJsVm *vm, const char *utf8_code, size_t code_len,
                               const char *source_url);

static bool web_tick_timers_and_raf(ArcrJsVm *vm, uint64_t now_ms, bool *out_dom_mutated);
static bool web_has_pending_timers(const ArcrJsVm *vm);

bool web_eval_worker_entry(ArcrJsVm *vm, const char *utf8_code, size_t code_len,
                           const char *script_url, bool is_module) {
    if (!vm || !vm->avm || !utf8_code) return false;
    const char *eff_url = (script_url && script_url[0] != '\0') ? script_url : vm->worker_script_url;

    if (is_module) {
        AjsHandleScope sc;
        ajs_handle_scope_open(vm->avm, &sc);
        AjsValue glob = ajs_global_object(vm->avm);
        (void)ajs_define_prop_str(vm->avm, glob, "__workerIsModule", ajs_bool(true), AJS_PROP_CONFIGURABLE);
        ajs_handle_scope_close(vm->avm, &sc);

        vm->import_map.locked = true;
        AjsErrorInfo parse_err;
        memset(&parse_err, 0, sizeof(parse_err));
        AjsModuleRecord *mod = ajs_module_parse(vm->avm, utf8_code, code_len, eff_url, &parse_err);
        if (!mod) {
            if (parse_err.url[0] == '\0' && eff_url) {
                arcr_str_copy(parse_err.url, sizeof(parse_err.url), eff_url);
            }
            web_log_ajs_error(vm, "JS ParseError", &parse_err);
            return false;
        }
        (void)mod_cache_insert(vm, eff_url, eff_url, mod, false, NULL);
        return web_link_and_eval_module_record(vm, mod, eff_url);
    }

    return web_eval_script_ex(vm, utf8_code, code_len, eff_url);
}

bool web_tick_worker_vm(ArcrJsVm *vm, uint64_t now_ms) {
    return web_tick_timers_and_raf(vm, now_ms, NULL);
}

bool web_worker_vm_has_work(const ArcrJsVm *vm) {
    return web_has_pending_timers(vm);
}

/* =========================================================================
 * 8. ArcrJsVTable v2 Entrypoints (Scripts, Events, Timers, ReadyState, History)
 * ========================================================================= */
static bool web_eval_script_ex(ArcrJsVm *vm, const char *utf8_code, size_t code_len,
                               const char *source_url) {
    if (!vm || !vm->avm || !utf8_code) return false;
    if (code_len == 0u) return true;

    if (!vm->is_worker_vm) {
        web_uid_sync_document(vm);
    }

    char fallback_url[2048];
    const char *eff_url = source_url;
    if (!eff_url || eff_url[0] == '\0') {
        web_effective_base_url(vm, fallback_url, sizeof(fallback_url));
        eff_url = fallback_url;
    }

    AjsHandleScope sc;
    ajs_handle_scope_open(vm->avm, &sc);

    AjsErrorInfo err;
    memset(&err, 0, sizeof(err));

    vm->deadline_usec   = arcr_platform_monotonic_usec() + ARCR_JS_TURN_DEADLINE_USEC;
    vm->interrupt_armed = true;
    bool ok = ajs_eval_script(vm->avm, utf8_code, code_len, eff_url, false, NULL, &err);
    vm->interrupt_armed = false;

    if (!ok) {
        if (err.url[0] == '\0') arcr_str_copy(err.url, sizeof(err.url), eff_url);
        web_log_ajs_error(vm, NULL, &err);
    }

    /* HTML spec: perform a microtask checkpoint at end of script evaluation */
    (void)web_drain_microtasks_guarded(vm);
    ajs_handle_scope_close(vm->avm, &sc);
    return ok;
}

static bool web_eval_script(ArcrJsVm *vm, const char *utf8_code, size_t code_len) {
    return web_eval_script_ex(vm, utf8_code, code_len, NULL);
}

static bool web_dispatch_dom_event_ex(ArcrJsVm *vm, uint32_t target_node_uid,
                                      const char *event_type,
                                      int32_t client_x, int32_t client_y, int key_code,
                                      bool *out_default_prevented) {
    if (out_default_prevented) *out_default_prevented = false;
    if (!vm || !vm->avm || !event_type || vm->pin_dispatch_event == 0u) return false;

    if (!vm->is_worker_vm) {
        web_uid_sync_document(vm);
    }

    AjsVm *avm = vm->avm;
    AjsHandleScope sc;
    ajs_handle_scope_open(avm, &sc);

    AjsValue fn = ajs_get_pinned(avm, vm->pin_dispatch_event);
    if (!ajs_is_callable(avm, fn)) {
        ajs_handle_scope_close(avm, &sc);
        return false;
    }

    AjsValue args[5] = {
        ajs_uint32(target_node_uid),
        ajs_string_cstr(avm, event_type),
        ajs_int32(client_x),
        ajs_int32(client_y),
        ajs_int32((int32_t)key_code)
    };

    vm->deadline_usec   = arcr_platform_monotonic_usec() + ARCR_JS_TURN_DEADLINE_USEC;
    vm->interrupt_armed = true;
    AjsValue ret = ajs_call(avm, fn, ajs_global_object(avm), 5, args);
    vm->interrupt_armed = false;

    bool handled   = false;
    bool prevented = false;
    if (ajs_has_exception(avm)) {
        AjsErrorInfo ei;
        memset(&ei, 0, sizeof(ei));
        (void)ajs_take_exception(avm, &ei);
        web_log_ajs_error(vm, NULL, &ei);
    } else if (ajs_is_object(avm, ret)) {
        handled   = ajs_to_bool(avm, ajs_get_prop_str(avm, ret, "handled"));
        prevented = ajs_to_bool(avm, ajs_get_prop_str(avm, ret, "defaultPrevented"));
    }

    (void)web_drain_microtasks_guarded(vm);
    ajs_handle_scope_close(avm, &sc);

    if (out_default_prevented) *out_default_prevented = prevented;
    return handled || prevented;
}

static bool web_dispatch_dom_event(ArcrJsVm *vm, uint32_t target_node_uid, const char *event_type) {
    return web_dispatch_dom_event_ex(vm, target_node_uid, event_type, 0, 0, 0, NULL);
}

static bool web_tick_timers_and_raf(ArcrJsVm *vm, uint64_t now_ms, bool *out_dom_mutated) {
    if (out_dom_mutated) *out_dom_mutated = false;
    if (!vm || !vm->avm) return false;

    if (!vm->is_worker_vm) {
        web_uid_sync_document(vm);
    }

    bool had_dirty = (vm->host.doc != NULL) ? vm->host.doc->dom_dirty : false;
    if (vm->host.doc) vm->host.doc->dom_dirty = false;

    bool async_ran = false;
    web_poll_async_work(vm, &async_ran);
    (void)web_drain_microtasks_guarded(vm);

    /* Convert now_ms (either monotonic boot+elapsed ms or synthetic test ms) onto performance.now() timeline */
    double   boot_ms     = (double)vm->boot_usec * 0.001;
    uint64_t real_us     = arcr_platform_monotonic_usec();
    double   real_rel_ms = (real_us >= vm->boot_usec) ? ((double)(real_us - vm->boot_usec) * 0.001) : 0.0;
    double   rel_ms;
    if (boot_ms > 1000.0 && (double)now_ms >= (boot_ms - 5.0)) {
        rel_ms = (double)now_ms - boot_ms;
        if (rel_ms < real_rel_ms) rel_ms = real_rel_ms;
    } else {
        rel_ms = ((double)now_ms > real_rel_ms) ? (double)now_ms : real_rel_ms;
    }

    bool timers_ran = false;
    if (vm->pin_tick_timers != 0u) {
        AjsHandleScope sc;
        ajs_handle_scope_open(vm->avm, &sc);
        AjsValue fn = ajs_get_pinned(vm->avm, vm->pin_tick_timers);
        if (ajs_is_callable(vm->avm, fn)) {
            AjsValue arg = ajs_number(rel_ms);
            vm->deadline_usec   = arcr_platform_monotonic_usec() + ARCR_JS_TURN_DEADLINE_USEC;
            vm->interrupt_armed = true;
            AjsValue ret = ajs_call(vm->avm, fn, ajs_global_object(vm->avm), 1, &arg);
            vm->interrupt_armed = false;

            if (ajs_has_exception(vm->avm)) {
                AjsErrorInfo ei;
                memset(&ei, 0, sizeof(ei));
                (void)ajs_take_exception(vm->avm, &ei);
                web_log_ajs_error(vm, NULL, &ei);
            } else {
                timers_ran = ajs_to_bool(vm->avm, ret);
            }
        }
        ajs_handle_scope_close(vm->avm, &sc);
    }

    if (ajs_has_pending_jobs(vm->avm)) {
        timers_ran = true;
        (void)web_drain_microtasks_guarded(vm);
    }

    bool dom_changed = (vm->host.doc != NULL) ? vm->host.doc->dom_dirty : (async_ran || timers_ran);
    if (vm->host.doc && had_dirty) {
        vm->host.doc->dom_dirty = true;
    }
    if (out_dom_mutated) {
        *out_dom_mutated = dom_changed;
    }
    return async_ran || timers_ran || dom_changed;
}

static bool web_has_pending_timers(const ArcrJsVm *vm) {
    if (!vm || !vm->avm) return false;
    if (web_has_pending_async_work(vm)) return true;
    if (ajs_has_pending_jobs(vm->avm)) return true;
    if (vm->pin_has_timers == 0u) return false;

    AjsVm *avm = vm->avm;
    AjsHandleScope sc;
    ajs_handle_scope_open(avm, &sc);
    AjsValue fn = ajs_get_pinned(avm, vm->pin_has_timers);
    bool has_tm = false;
    if (ajs_is_callable(avm, fn)) {
        AjsValue r = ajs_call(avm, fn, ajs_global_object(avm), 0, NULL);
        if (ajs_has_exception(avm)) {
            ajs_clear_exception(avm);
        } else {
            has_tm = ajs_to_bool(avm, r);
        }
    }
    ajs_handle_scope_close(avm, &sc);
    return has_tm;
}

static size_t web_get_used_heap_bytes(const ArcrJsVm *vm) {
    if (!vm || !vm->avm) return 0u;
    size_t used = ajs_gc_used_bytes(vm->avm);
    used += sizeof(ArcrJsVm);
    used += (size_t)vm->uid_cap * sizeof(ArcrDomNode *);
    used += vm->shadow_link_cap * sizeof(WebShadowLink);
    used += vm->mod_cache_cap * sizeof(WebModuleCacheEntry);
    return used;
}

static void web_release_caches_on_pressure(ArcrJsVm *vm, int pressure_level) {
    if (!vm || !vm->avm) return;
    if (pressure_level >= 1) {
        ajs_gc_collect(vm->avm);
        if (vm->canvas_fonts) {
            arcr_font_system_purge_cache(vm->canvas_fonts);
        }
    }
}

static void web_set_ready_state(ArcrJsVm *vm, const char *ready_state) {
    if (!vm || !vm->avm || !ready_state || vm->pin_set_ready_state == 0u) return;

    if (!vm->is_worker_vm) {
        web_uid_sync_document(vm);
    }

    AjsVm *avm = vm->avm;
    AjsHandleScope sc;
    ajs_handle_scope_open(avm, &sc);

    AjsValue fn = ajs_get_pinned(avm, vm->pin_set_ready_state);
    if (ajs_is_callable(avm, fn)) {
        AjsValue arg = ajs_string_cstr(avm, ready_state);
        vm->deadline_usec   = arcr_platform_monotonic_usec() + ARCR_JS_TURN_DEADLINE_USEC;
        vm->interrupt_armed = true;
        (void)ajs_call(avm, fn, ajs_global_object(avm), 1, &arg);
        vm->interrupt_armed = false;

        if (ajs_has_exception(avm)) {
            AjsErrorInfo ei;
            memset(&ei, 0, sizeof(ei));
            (void)ajs_take_exception(avm, &ei);
            web_log_ajs_error(vm, NULL, &ei);
        }
        (void)web_drain_microtasks_guarded(vm);
    }
    ajs_handle_scope_close(avm, &sc);
}

static void web_notify_url_and_popstate(ArcrJsVm *vm, const char *new_url,
                                        const char *state_json_or_null, bool fire_popstate) {
    if (!vm || !vm->avm || vm->pin_notify_popstate == 0u) return;

    if (!vm->is_worker_vm) {
        web_uid_sync_document(vm);
    }

    AjsVm *avm = vm->avm;
    AjsHandleScope sc;
    ajs_handle_scope_open(avm, &sc);

    AjsValue fn = ajs_get_pinned(avm, vm->pin_notify_popstate);
    if (ajs_is_callable(avm, fn)) {
        AjsValue args[3] = {
            new_url ? ajs_string_cstr(avm, new_url) : ajs_null(),
            state_json_or_null ? ajs_string_cstr(avm, state_json_or_null) : ajs_null(),
            ajs_bool(fire_popstate)
        };
        vm->deadline_usec   = arcr_platform_monotonic_usec() + ARCR_JS_TURN_DEADLINE_USEC;
        vm->interrupt_armed = true;
        (void)ajs_call(avm, fn, ajs_global_object(avm), 3, args);
        vm->interrupt_armed = false;

        if (ajs_has_exception(avm)) {
            AjsErrorInfo ei;
            memset(&ei, 0, sizeof(ei));
            (void)ajs_take_exception(avm, &ei);
            web_log_ajs_error(vm, NULL, &ei);
        }
        (void)web_drain_microtasks_guarded(vm);
    }
    ajs_handle_scope_close(avm, &sc);
}

/* =========================================================================
 * 9. Module Descriptor & Dynamic / Static Entrypoint Query
 * ========================================================================= */
static const ArcrJsVTable kArcrJsVTable = {
    .struct_size                = sizeof(ArcrJsVTable),
    .vtable_version             = ARCR_JS_VTABLE_VERSION,
    .engine_backend_name        = "ArcradientJS ES2026",
    .create_vm                  = web_create_vm,
    .destroy_vm                 = web_destroy_vm,
    .eval_script                = web_eval_script,
    .dispatch_dom_event         = web_dispatch_dom_event,
    .dispatch_dom_event_ex      = web_dispatch_dom_event_ex,
    .tick_timers_and_raf        = web_tick_timers_and_raf,
    .has_pending_timers         = web_has_pending_timers,
    .get_used_heap_bytes        = web_get_used_heap_bytes,
    .release_caches_on_pressure = web_release_caches_on_pressure,
    .eval_script_ex             = web_eval_script_ex,
    .register_import_map        = web_register_import_map,
    .eval_module                = web_eval_module,
    .set_ready_state            = web_set_ready_state,
    .notify_url_and_popstate    = web_notify_url_and_popstate
};

static ArcrModuleDescriptor g_arcr_js_module_desc = {
    .magic              = ARCR_MODULE_MAGIC,
    .struct_size        = sizeof(ArcrModuleDescriptor),
    .abi_version        = ARCR_ABI_VERSION,
    .engine_build_id    = ARCR_ENGINE_BUILD_ID,
    .arch_id            = 0,
    .min_os_major_minor = ARCR_OS_POSIX_ANY,
    .required_cpu_flags = ARCR_CPU_SCALAR,
    .module_name        = "arcr-js-portable",
    .vtable             = &kArcrJsVTable
};

const ArcrModuleDescriptor *arcr_js_portable_module_query(void);
const ArcrModuleDescriptor *arcr_js_portable_module_query(void) {
    g_arcr_js_module_desc.arch_id = (uint16_t)arcr_host_compile_arch();
#if defined(_WIN32)
    g_arcr_js_module_desc.min_os_major_minor = ARCR_OS_WIN2000;
#endif
    return &g_arcr_js_module_desc;
}

#if !defined(ARCR_JS_STATIC_LINK)
ARCR_API const ArcrModuleDescriptor *arcr_module_query(void) {
    return arcr_js_portable_module_query();
}
#endif
