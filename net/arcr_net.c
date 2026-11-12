/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * net/arcr_net.c - HTTP/1.1 + HTTPS (OpenSSL / Win32 SChannel), Gzip, Cookie & Cache Module
 */
#include "arcr_net.h"
#include "../base/arcr_base.h"
#include "../base/arcr_codec.h"
#include "../platform/arcr_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <wincrypt.h>
#  ifndef SECURITY_WIN32
#    define SECURITY_WIN32
#  endif
#  include <security.h>
#  include <schannel.h>

#  ifndef UNISP_NAME_A
#    define UNISP_NAME_A "Microsoft Unified Security Protocol Provider"
#  endif
#  ifndef SP_PROT_TLS1_0_CLIENT
#    define SP_PROT_TLS1_0_CLIENT 0x00000080u
#  endif
#  ifndef SP_PROT_TLS1_1_CLIENT
#    define SP_PROT_TLS1_1_CLIENT 0x00000200u
#  endif
#  ifndef SP_PROT_TLS1_2_CLIENT
#    define SP_PROT_TLS1_2_CLIENT 0x00000800u
#  endif
#  ifndef SCH_CRED_NO_DEFAULT_CREDS
#    define SCH_CRED_NO_DEFAULT_CREDS 0x00000010u
#  endif
#  ifndef SCH_CRED_AUTO_CRED_VALIDATION
#    define SCH_CRED_AUTO_CRED_VALIDATION 0x00000020u
#  endif
#  ifndef SECBUFFER_EXTRA
#    define SECBUFFER_EXTRA 5
#  endif
#  ifndef SECBUFFER_STREAM_TRAILER
#    define SECBUFFER_STREAM_TRAILER 6
#  endif
#  ifndef SECBUFFER_STREAM_HEADER
#    define SECBUFFER_STREAM_HEADER 7
#  endif
#  ifndef SEC_E_OK
#    define SEC_E_OK ((SECURITY_STATUS)0x00000000L)
#  endif
#  ifndef SEC_I_CONTINUE_NEEDED
#    define SEC_I_CONTINUE_NEEDED ((SECURITY_STATUS)0x00090312L)
#  endif
#  ifndef SEC_I_CONTEXT_EXPIRED
#    define SEC_I_CONTEXT_EXPIRED ((SECURITY_STATUS)0x00090317L)
#  endif
#  ifndef SEC_E_INCOMPLETE_MESSAGE
#    define SEC_E_INCOMPLETE_MESSAGE ((SECURITY_STATUS)0x80090318L)
#  endif
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <unistd.h>
#  if defined(ARCR_HAVE_OPENSSL)
#    include <openssl/err.h>
#    include <openssl/ssl.h>
#    include <openssl/x509.h>
#    include <openssl/x509v3.h>
#  endif
#endif

#define ARCR_NET_CONNECT_TIMEOUT_MS  5000
#define ARCR_NET_IO_TIMEOUT_MS       8000
#define ARCR_NET_MAX_REDIRECTS       6
#define ARCR_NET_MAX_RESPONSE_BYTES  (32u * 1024u * 1024u)
#define ARCR_NET_CACHE_SLOTS         32u
#define ARCR_NET_MAX_CACHE_TOTAL     (8u * 1024u * 1024u)
#define ARCR_NET_MAX_CACHE_ITEM      (1u * 1024u * 1024u)
#define ARCR_NET_MAX_COOKIES         128u

/* =========================================================================
 * 1. In-Memory Cookie Jar & 32-Entry LRU GET Cache
 * ========================================================================= */
typedef struct ArcrCookieEntry {
    bool in_use;
    bool secure_only;
    char domain[128];
    char path[128];
    char name[64];
    char value[256];
} ArcrCookieEntry;

typedef struct ArcrCacheEntry {
    bool              valid;
    uint64_t          last_used_tick;
    char              url[512];
    char              content_type[128];
    int               status_code;
    ArcrSecurityState security;
    char              detail[256];
    uint8_t          *body;
    size_t            body_len;
} ArcrCacheEntry;

static ArcrCookieEntry g_cookies[ARCR_NET_MAX_COOKIES];
static ArcrCacheEntry  g_cache[ARCR_NET_CACHE_SLOTS];
static size_t          g_cache_bytes_used = 0u;
static uint64_t        g_cache_clock      = 1u;

static void net_clear_cookies_and_cache(void) {
    memset(g_cookies, 0, sizeof(g_cookies));
    for (size_t i = 0u; i < ARCR_NET_CACHE_SLOTS; ++i) {
        if (g_cache[i].body) {
            free(g_cache[i].body);
            g_cache[i].body = NULL;
        }
        memset(&g_cache[i], 0, sizeof(g_cache[i]));
    }
    g_cache_bytes_used = 0u;
}

static bool host_matches_cookie_domain(const char *req_host, const char *cookie_domain) {
    if (!req_host || !cookie_domain || cookie_domain[0] == '\0') return false;
    if (cookie_domain[0] == '.') ++cookie_domain;
    if (arcr_str_eq_nocase(req_host, cookie_domain)) return true;

    /* Must contain at least one dot in cookie_domain to prevent TLD-wide supercookies */
    if (!strchr(cookie_domain, '.')) return false;
    size_t hlen = strlen(req_host);
    size_t dlen = strlen(cookie_domain);
    if (hlen > dlen + 1u && req_host[hlen - dlen - 1u] == '.' &&
        arcr_str_eq_nocase(req_host + (hlen - dlen), cookie_domain)) {
        return true;
    }
    return false;
}

static bool path_matches_cookie_path(const char *req_path, const char *cookie_path) {
    if (!cookie_path || cookie_path[0] == '\0') return true;
    if (!req_path || req_path[0] == '\0') req_path = "/";
    size_t clen = strlen(cookie_path);
    if (strncmp(req_path, cookie_path, clen) != 0) return false;
    if (req_path[clen] == '\0' || req_path[clen] == '?' ||
        cookie_path[clen - 1u] == '/' || req_path[clen] == '/') {
        return true;
    }
    return false;
}

static void cookie_jar_store_header(const char *req_host, const char *req_path,
                                    const char *hdr_val, size_t hdr_len) {
    if (!req_host || !hdr_val || hdr_len == 0u) return;

    /* Extract first name=value pair before ';' */
    size_t semi = 0u;
    while (semi < hdr_len && hdr_val[semi] != ';') ++semi;
    const char *nv = hdr_val;
    size_t nv_len  = semi;
    arcr_str_trim_view(&nv, &nv_len);

    size_t eq = 0u;
    while (eq < nv_len && nv[eq] != '=') ++eq;
    if (eq == 0u || eq >= nv_len) return;

    const char *cname = nv;
    size_t cname_len  = eq;
    const char *cval  = nv + eq + 1u;
    size_t cval_len   = nv_len - eq - 1u;
    arcr_str_trim_view(&cname, &cname_len);
    arcr_str_trim_view(&cval, &cval_len);
    if (cname_len == 0u || cname_len >= 64u || cval_len >= 256u) return;

    char domain_buf[128];
    char path_buf[128];
    arcr_str_copy(domain_buf, sizeof(domain_buf), req_host);
    if (req_path && req_path[0] == '/') {
        const char *q = strchr(req_path, '?');
        size_t plen = q ? (size_t)(q - req_path) : strlen(req_path);
        const char *last_slash = NULL;
        for (size_t i = 0u; i < plen; ++i) {
            if (req_path[i] == '/') last_slash = req_path + i;
        }
        if (last_slash && last_slash > req_path) {
            size_t dlen = (size_t)(last_slash - req_path);
            if (dlen >= sizeof(path_buf)) dlen = sizeof(path_buf) - 1u;
            memcpy(path_buf, req_path, dlen);
            path_buf[dlen] = '\0';
        } else {
            arcr_str_copy(path_buf, sizeof(path_buf), "/");
        }
    } else {
        arcr_str_copy(path_buf, sizeof(path_buf), "/");
    }

    bool secure_only = false;
    bool delete_now  = false;

    /* Parse cookie attributes */
    size_t pos = (semi < hdr_len) ? (semi + 1u) : hdr_len;
    while (pos < hdr_len) {
        size_t next_semi = pos;
        while (next_semi < hdr_len && hdr_val[next_semi] != ';') ++next_semi;
        const char *attr = hdr_val + pos;
        size_t alen      = next_semi - pos;
        pos = (next_semi < hdr_len) ? (next_semi + 1u) : hdr_len;
        arcr_str_trim_view(&attr, &alen);
        if (alen == 0u) continue;

        if (arcr_str_eq_nocase_n(attr, alen, "secure", 6u)) {
            secure_only = true;
        } else {
            size_t aeq = 0u;
            while (aeq < alen && attr[aeq] != '=') ++aeq;
            if (aeq < alen) {
                const char *ak = attr;
                size_t ak_len  = aeq;
                const char *av = attr + aeq + 1u;
                size_t av_len  = alen - aeq - 1u;
                arcr_str_trim_view(&ak, &ak_len);
                arcr_str_trim_view(&av, &av_len);
                if (arcr_str_eq_nocase_n(ak, ak_len, "domain", 6u) && av_len > 0u) {
                    if (av[0] == '.') { ++av; --av_len; }
                    if (av_len > 0u && av_len < sizeof(domain_buf)) {
                        char tmp[128];
                        memcpy(tmp, av, av_len);
                        tmp[av_len] = '\0';
                        if (host_matches_cookie_domain(req_host, tmp)) {
                            arcr_str_copy(domain_buf, sizeof(domain_buf), tmp);
                        }
                    }
                } else if (arcr_str_eq_nocase_n(ak, ak_len, "path", 4u) &&
                           av_len > 0u && av_len < sizeof(path_buf)) {
                    memcpy(path_buf, av, av_len);
                    path_buf[av_len] = '\0';
                } else if (arcr_str_eq_nocase_n(ak, ak_len, "max-age", 7u)) {
                    int32_t max_age = 1;
                    if (arcr_parse_i32(av, av_len, &max_age) && max_age <= 0) {
                        delete_now = true;
                    }
                }
            }
        }
    }

    char name_cstr[64];
    memcpy(name_cstr, cname, cname_len);
    name_cstr[cname_len] = '\0';

    /* Update or delete existing matching cookie */
    size_t free_idx = ARCR_NET_MAX_COOKIES;
    for (size_t i = 0u; i < ARCR_NET_MAX_COOKIES; ++i) {
        if (!g_cookies[i].in_use) {
            if (free_idx == ARCR_NET_MAX_COOKIES) free_idx = i;
            continue;
        }
        if (strcmp(g_cookies[i].name, name_cstr) == 0 &&
            arcr_str_eq_nocase(g_cookies[i].domain, domain_buf) &&
            strcmp(g_cookies[i].path, path_buf) == 0) {
            if (delete_now) {
                memset(&g_cookies[i], 0, sizeof(g_cookies[i]));
                return;
            }
            memcpy(g_cookies[i].value, cval, cval_len);
            g_cookies[i].value[cval_len] = '\0';
            g_cookies[i].secure_only = secure_only;
            return;
        }
    }
    if (delete_now) return;

    if (free_idx == ARCR_NET_MAX_COOKIES) {
        free_idx = 0u; /* Evict slot 0 when jar is full */
    }
    ArcrCookieEntry *ce = &g_cookies[free_idx];
    ce->in_use      = true;
    ce->secure_only = secure_only;
    arcr_str_copy(ce->domain, sizeof(ce->domain), domain_buf);
    arcr_str_copy(ce->path, sizeof(ce->path), path_buf);
    arcr_str_copy(ce->name, sizeof(ce->name), name_cstr);
    memcpy(ce->value, cval, cval_len);
    ce->value[cval_len] = '\0';
}

static size_t cookie_jar_build_header(const char *req_host, const char *req_path,
                                      bool is_https, char *out_buf, size_t cap) {
    if (!out_buf || cap == 0u) return 0u;
    out_buf[0] = '\0';
    size_t pos = 0u;

    for (size_t i = 0u; i < ARCR_NET_MAX_COOKIES; ++i) {
        const ArcrCookieEntry *ce = &g_cookies[i];
        if (!ce->in_use) continue;
        if (ce->secure_only && !is_https) continue;
        if (!host_matches_cookie_domain(req_host, ce->domain)) continue;
        if (!path_matches_cookie_path(req_path, ce->path)) continue;

        size_t nlen = strlen(ce->name);
        size_t vlen = strlen(ce->value);
        size_t sep  = (pos > 0u) ? 2u : 0u;
        if (pos + sep + nlen + 1u + vlen + 1u >= cap) break;

        if (pos > 0u) {
            out_buf[pos++] = ';';
            out_buf[pos++] = ' ';
        }
        memcpy(out_buf + pos, ce->name, nlen);
        pos += nlen;
        out_buf[pos++] = '=';
        memcpy(out_buf + pos, ce->value, vlen);
        pos += vlen;
        out_buf[pos] = '\0';
    }
    return pos;
}

static bool cache_lookup(const char *url, ArcrHttpResponse *out_resp) {
    for (size_t i = 0u; i < ARCR_NET_CACHE_SLOTS; ++i) {
        ArcrCacheEntry *e = &g_cache[i];
        if (!e->valid || strcmp(e->url, url) != 0) continue;

        uint8_t *copy = (uint8_t *)malloc(e->body_len + 1u);
        if (!copy) return false;
        if (e->body_len > 0u) {
            memcpy(copy, e->body, e->body_len);
        }
        copy[e->body_len] = '\0';

        e->last_used_tick     = ++g_cache_clock;
        out_resp->status_code = e->status_code;
        arcr_str_copy(out_resp->final_url, sizeof(out_resp->final_url), e->url);
        arcr_str_copy(out_resp->content_type, sizeof(out_resp->content_type), e->content_type);
        out_resp->body        = copy;
        out_resp->body_len    = e->body_len;
        out_resp->security    = e->security;
        arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), e->detail);
        out_resp->from_cache  = true;
        return true;
    }
    return false;
}

static void cache_evict_slot(size_t idx) {
    if (g_cache[idx].valid) {
        if (g_cache_bytes_used >= g_cache[idx].body_len) {
            g_cache_bytes_used -= g_cache[idx].body_len;
        } else {
            g_cache_bytes_used = 0u;
        }
        free(g_cache[idx].body);
        memset(&g_cache[idx], 0, sizeof(g_cache[idx]));
    }
}

static void cache_insert(const char *url, const ArcrHttpResponse *resp) {
    if (!url || !resp || !resp->body) return;
    if (resp->body_len > ARCR_NET_MAX_CACHE_ITEM) return;
    if (strlen(url) >= sizeof(g_cache[0].url)) return;

    /* Check if URL already exists */
    for (size_t i = 0u; i < ARCR_NET_CACHE_SLOTS; ++i) {
        if (g_cache[i].valid && strcmp(g_cache[i].url, url) == 0) {
            cache_evict_slot(i);
            break;
        }
    }

    /* Evict LRU entries until within total memory budget */
    while (g_cache_bytes_used + resp->body_len > ARCR_NET_MAX_CACHE_TOTAL) {
        size_t victim = ARCR_NET_CACHE_SLOTS;
        uint64_t oldest = UINT64_MAX;
        for (size_t i = 0u; i < ARCR_NET_CACHE_SLOTS; ++i) {
            if (g_cache[i].valid && g_cache[i].last_used_tick < oldest) {
                oldest = g_cache[i].last_used_tick;
                victim = i;
            }
        }
        if (victim == ARCR_NET_CACHE_SLOTS) break;
        cache_evict_slot(victim);
    }

    size_t target = ARCR_NET_CACHE_SLOTS;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0u; i < ARCR_NET_CACHE_SLOTS; ++i) {
        if (!g_cache[i].valid) {
            target = i;
            break;
        }
        if (g_cache[i].last_used_tick < oldest) {
            oldest = g_cache[i].last_used_tick;
            target = i;
        }
    }
    if (target == ARCR_NET_CACHE_SLOTS) return;
    if (g_cache[target].valid) cache_evict_slot(target);

    uint8_t *buf = (uint8_t *)malloc(resp->body_len + 1u);
    if (!buf) return;
    if (resp->body_len > 0u) memcpy(buf, resp->body, resp->body_len);
    buf[resp->body_len] = '\0';

    ArcrCacheEntry *e = &g_cache[target];
    e->valid          = true;
    e->last_used_tick = ++g_cache_clock;
    arcr_str_copy(e->url, sizeof(e->url), url);
    arcr_str_copy(e->content_type, sizeof(e->content_type), resp->content_type);
    e->status_code    = resp->status_code;
    e->security       = resp->security;
    arcr_str_copy(e->detail, sizeof(e->detail), resp->detail);
    e->body           = buf;
    e->body_len       = resp->body_len;
    g_cache_bytes_used += resp->body_len;
}

/* =========================================================================
 * 2. URL Parsing & RFC 3986 Relative URL Resolution
 * ========================================================================= */
static bool is_alpha_ascii(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_scheme_char(char c) {
    return is_alpha_ascii(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

/* Normalizes RFC 3986 '.' and '..' path segments in-place into `out_path` */
static void normalize_dot_segments(const char *in_path, size_t in_len,
                                   char *out_path, size_t out_cap) {
    if (!out_path || out_cap == 0u) return;
    if (!in_path || in_len == 0u) {
        arcr_str_copy(out_path, out_cap, "/");
        return;
    }

    const char *segs[64];
    size_t      seg_lens[64];
    size_t      seg_count = 0u;
    bool        trailing_slash = (in_path[in_len - 1u] == '/');

    size_t i = 0u;
    while (i < in_len) {
        while (i < in_len && in_path[i] == '/') ++i;
        if (i >= in_len) break;
        size_t start = i;
        while (i < in_len && in_path[i] != '/') ++i;
        size_t slen = i - start;
        const char *s = in_path + start;

        if (slen == 1u && s[0] == '.') {
            if (i == in_len) trailing_slash = true;
            continue;
        }
        if (slen == 2u && s[0] == '.' && s[1] == '.') {
            if (seg_count > 0u) --seg_count;
            if (i == in_len) trailing_slash = true;
            continue;
        }
        if (seg_count < ARCR_ARRAY_LEN(segs)) {
            segs[seg_count]     = s;
            seg_lens[seg_count] = slen;
            ++seg_count;
        }
    }

    size_t pos = 0u;
    if (pos + 1u < out_cap) out_path[pos++] = '/';
    for (size_t k = 0u; k < seg_count; ++k) {
        if (k > 0u && pos + 1u < out_cap) {
            out_path[pos++] = '/';
        }
        size_t copy_n = seg_lens[k];
        if (pos + copy_n >= out_cap) {
            copy_n = (out_cap - 1u > pos) ? (out_cap - 1u - pos) : 0u;
        }
        if (copy_n > 0u) {
            memcpy(out_path + pos, segs[k], copy_n);
            pos += copy_n;
        }
    }
    if (trailing_slash && seg_count > 0u && pos + 1u < out_cap) {
        out_path[pos++] = '/';
    }
    out_path[pos] = '\0';
}

static bool net_parse_url(const char *raw_url, ArcrParsedUrl *out) {
    if (!raw_url || !out) return false;
    memset(out, 0, sizeof(*out));

    size_t rlen = strlen(raw_url);
    arcr_str_trim_view(&raw_url, &rlen);
    if (rlen == 0u) return false;

    char work[1024];
    if (rlen >= sizeof(work)) rlen = sizeof(work) - 1u;
    memcpy(work, raw_url, rlen);
    work[rlen] = '\0';

    /* Handle opaque single-colon browser schemes: about:..., data:..., javascript:... */
    if (arcr_str_starts_with_nocase(work, "about:") ||
        arcr_str_starts_with_nocase(work, "data:") ||
        arcr_str_starts_with_nocase(work, "javascript:") ||
        arcr_str_starts_with_nocase(work, "mailto:")) {
        const char *colon = strchr(work, ':');
        size_t slen = (size_t)(colon - work);
        if (slen >= sizeof(out->scheme)) return false;
        for (size_t i = 0u; i < slen; ++i) {
            out->scheme[i] = arcr_ascii_tolower(work[i]);
        }
        out->scheme[slen] = '\0';
        arcr_str_copy(out->path, sizeof(out->path), colon + 1);
        return true;
    }

    const char *sep = strstr(work, "://");
    const char *rest = work;
    if (sep) {
        size_t slen = (size_t)(sep - work);
        if (slen == 0u || slen >= sizeof(out->scheme) || !is_alpha_ascii(work[0])) {
            return false;
        }
        for (size_t i = 0u; i < slen; ++i) {
            if (!is_scheme_char(work[i])) return false;
            out->scheme[i] = arcr_ascii_tolower(work[i]);
        }
        out->scheme[slen] = '\0';
        rest = sep + 3;
    } else {
        arcr_str_copy(out->scheme, sizeof(out->scheme), "https");
    }

    if (arcr_str_eq_nocase(out->scheme, "file")) {
        /* Strip #fragment if present */
        char *frag = strchr((char *)rest, '#');
        if (frag) *frag = '\0';
        arcr_str_copy(out->path, sizeof(out->path), rest);
        return true;
    }

    /* Strip #fragment before splitting authority & request-target */
    char *frag = strchr((char *)rest, '#');
    if (frag) *frag = '\0';

    const char *slash = strchr(rest, '/');
    const char *qmark = strchr(rest, '?');
    const char *delim = slash;
    if (qmark && (!delim || qmark < delim)) {
        delim = qmark;
    }

    size_t host_port_len = delim ? (size_t)(delim - rest) : strlen(rest);
    if (host_port_len == 0u &&
        (arcr_str_eq_nocase(out->scheme, "http") || arcr_str_eq_nocase(out->scheme, "https"))) {
        return false;
    }

    /* Support IPv6 literal [::1]:8080 or IPv4/hostname:port */
    if (host_port_len > 0u && rest[0] == '[') {
        const char *rb = memchr(rest, ']', host_port_len);
        if (!rb) return false;
        size_t hlen = (size_t)(rb - rest + 1);
        if (hlen >= sizeof(out->host)) hlen = sizeof(out->host) - 1u;
        memcpy(out->host, rest, hlen);
        out->host[hlen] = '\0';
        if ((size_t)(rb - rest + 1) < host_port_len && rb[1] == ':') {
            int32_t p = 0;
            size_t plen = host_port_len - (size_t)(rb - rest + 2);
            if (arcr_parse_i32(rb + 2, plen, &p) && p > 0 && p <= 65535) {
                out->port = (uint16_t)p;
            }
        }
    } else {
        const char *colon = NULL;
        for (size_t i = 0u; i < host_port_len; ++i) {
            if (rest[i] == ':') {
                colon = rest + i;
                break;
            }
        }
        if (colon) {
            size_t hlen = (size_t)(colon - rest);
            if (hlen >= sizeof(out->host)) hlen = sizeof(out->host) - 1u;
            memcpy(out->host, rest, hlen);
            out->host[hlen] = '\0';
            int32_t p = 0;
            if (arcr_parse_i32(colon + 1, host_port_len - hlen - 1u, &p) && p > 0 && p <= 65535) {
                out->port = (uint16_t)p;
            }
        } else {
            size_t hlen = host_port_len;
            if (hlen >= sizeof(out->host)) hlen = sizeof(out->host) - 1u;
            memcpy(out->host, rest, hlen);
            out->host[hlen] = '\0';
        }
    }

    if (out->port == 0u) {
        if (arcr_str_eq_nocase(out->scheme, "https")) out->port = 443u;
        else if (arcr_str_eq_nocase(out->scheme, "http")) out->port = 80u;
    }

    if (!delim) {
        arcr_str_copy(out->path, sizeof(out->path), "/");
    } else if (delim[0] == '?') {
        snprintf(out->path, sizeof(out->path), "/%s", delim);
    } else {
        const char * path_q = strchr(delim, '?');
        size_t raw_plen = path_q ? (size_t)(path_q - delim) : strlen(delim);
        char norm_path[512];
        normalize_dot_segments(delim, raw_plen, norm_path, sizeof(norm_path));
        if (path_q) {
            snprintf(out->path, sizeof(out->path), "%s%s", norm_path, path_q);
        } else {
            arcr_str_copy(out->path, sizeof(out->path), norm_path);
        }
    }
    return true;
}

static bool has_explicit_url_scheme(const char *s, size_t len) {
    if (len < 2u || !is_alpha_ascii(s[0])) return false;
    for (size_t i = 1u; i < len; ++i) {
        if (s[i] == ':') return true;
        if (!is_scheme_char(s[i])) return false;
    }
    return false;
}

static void format_authority_prefix(const ArcrParsedUrl *base, char *out, size_t cap) {
    bool default_port =
        (arcr_str_eq_nocase(base->scheme, "https") && (base->port == 0u || base->port == 443u)) ||
        (arcr_str_eq_nocase(base->scheme, "http") && (base->port == 0u || base->port == 80u)) ||
        base->port == 0u;
    if (default_port) {
        snprintf(out, cap, "%s://%s", base->scheme, base->host);
    } else {
        snprintf(out, cap, "%s://%s:%u", base->scheme, base->host, (unsigned)base->port);
    }
}

static bool net_resolve_url(const char *base_url, const char *rel_url,
                            char *out_resolved, size_t out_cap) {
    if (!out_resolved || out_cap == 0u) return false;
    out_resolved[0] = '\0';
    if (!rel_url) {
        if (!base_url) return false;
        arcr_str_copy(out_resolved, out_cap, base_url);
        return true;
    }

    size_t rlen = strlen(rel_url);
    arcr_str_trim_view(&rel_url, &rlen);
    if (rlen == 0u) {
        if (!base_url) return false;
        const char *frag = strchr(base_url, '#');
        size_t blen = frag ? (size_t)(frag - base_url) : strlen(base_url);
        if (blen >= out_cap) blen = out_cap - 1u;
        memcpy(out_resolved, base_url, blen);
        out_resolved[blen] = '\0';
        return true;
    }

    char rel_buf[1024];
    if (rlen >= sizeof(rel_buf)) return false;
    memcpy(rel_buf, rel_url, rlen);
    rel_buf[rlen] = '\0';

    /* 1. Full URL with explicit scheme (https://, waterscape://, data:, about:, etc.) */
    if (has_explicit_url_scheme(rel_buf, rlen)) {
        const char *frag = strchr(rel_buf, '#');
        if (arcr_str_starts_with_nocase(rel_buf, "http://") ||
            arcr_str_starts_with_nocase(rel_buf, "https://")) {
            ArcrParsedUrl p;
            if (!net_parse_url(rel_buf, &p)) return false;
            char prefix[256];
            format_authority_prefix(&p, prefix, sizeof(prefix));
            if (frag) {
                snprintf(out_resolved, out_cap, "%s%s%s", prefix, p.path, frag);
            } else {
                snprintf(out_resolved, out_cap, "%s%s", prefix, p.path);
            }
            return true;
        }
        arcr_str_copy(out_resolved, out_cap, rel_buf);
        return true;
    }

    if (!base_url || base_url[0] == '\0') return false;
    ArcrParsedUrl base;
    if (!net_parse_url(base_url, &base)) return false;

    /* 2. Protocol-relative URL: //cdn.example.com/a.css */
    if (rlen >= 2u && rel_buf[0] == '/' && rel_buf[1] == '/') {
        char combined[1088];
        const char *sch = (base.scheme[0] != '\0') ? base.scheme : "https";
        snprintf(combined, sizeof(combined), "%s:%s", sch, rel_buf);
        return net_resolve_url(base_url, combined, out_resolved, out_cap);
    }

    /* 3. Fragment-only reference: #section */
    if (rel_buf[0] == '#') {
        const char *bfrag = strchr(base_url, '#');
        size_t blen = bfrag ? (size_t)(bfrag - base_url) : strlen(base_url);
        if (blen + rlen >= out_cap) return false;
        memcpy(out_resolved, base_url, blen);
        memcpy(out_resolved + blen, rel_buf, rlen + 1u);
        return true;
    }

    /* Split rel_buf into rel_path, ?query, and #fragment */
    const char *rel_frag = strchr(rel_buf, '#');
    char frag_part[256] = {0};
    if (rel_frag) {
        arcr_str_copy(frag_part, sizeof(frag_part), rel_frag);
        *(char *)rel_frag = '\0';
    }
    const char *rel_query = strchr(rel_buf, '?');
    char query_part[512] = {0};
    if (rel_query) {
        arcr_str_copy(query_part, sizeof(query_part), rel_query);
        *(char *)rel_query = '\0';
    }

    /* Extract base path without ?query */
    char base_path_only[512];
    const char *bq = strchr(base.path, '?');
    if (bq) {
        size_t bplen = (size_t)(bq - base.path);
        if (bplen >= sizeof(base_path_only)) bplen = sizeof(base_path_only) - 1u;
        memcpy(base_path_only, base.path, bplen);
        base_path_only[bplen] = '\0';
    } else {
        arcr_str_copy(base_path_only, sizeof(base_path_only),
                      base.path[0] ? base.path : "/");
    }

    char norm_path[512];
    if (rel_buf[0] == '\0') {
        /* 4. Query-only reference: ?page=2 */
        normalize_dot_segments(base_path_only, strlen(base_path_only), norm_path, sizeof(norm_path));
    } else if (rel_buf[0] == '/') {
        /* 5. Root-relative reference: /assets/app.css */
        normalize_dot_segments(rel_buf, strlen(rel_buf), norm_path, sizeof(norm_path));
    } else {
        /* 6. Relative path: ../img/logo.png or style.css */
        char merged[1536];
        const char *last_slash = strrchr(base_path_only, '/');
        if (last_slash) {
            size_t dir_len = (size_t)(last_slash - base_path_only + 1);
            if (dir_len >= sizeof(merged)) dir_len = sizeof(merged) - 1u;
            memcpy(merged, base_path_only, dir_len);
            merged[dir_len] = '\0';
            snprintf(merged + dir_len, sizeof(merged) - dir_len, "%s", rel_buf);
        } else {
            snprintf(merged, sizeof(merged), "/%s", rel_buf);
        }
        normalize_dot_segments(merged, strlen(merged), norm_path, sizeof(norm_path));
    }

    if (arcr_str_eq_nocase(base.scheme, "file")) {
        snprintf(out_resolved, out_cap, "file://%s%s%s", norm_path, query_part, frag_part);
        return true;
    }

    char prefix[256];
    format_authority_prefix(&base, prefix, sizeof(prefix));
    snprintf(out_resolved, out_cap, "%s%s%s%s", prefix, norm_path, query_part, frag_part);
    return true;
}

/* =========================================================================
 * 3. TLS Root Trust & CMOS Clock-Skew Verification
 * ========================================================================= */
static bool san_hostname_matches(const char *expected_host, const char *san_pattern) {
    if (!expected_host || !san_pattern) return false;
    if (san_pattern[0] == '*' && san_pattern[1] == '.') {
        const char *dot = strchr(expected_host, '.');
        if (!dot || dot == expected_host) return false;
        return arcr_str_eq_nocase(dot + 1, san_pattern + 2);
    }
    return arcr_str_eq_nocase(expected_host, san_pattern);
}

static ArcrSecurityState net_verify_tls_trust(const ArcrCertCheckInput *in,
                                              char *out_reason, size_t reason_cap) {
    if (!in) return ARCR_SEC_CERT_ERROR;

    /* 1. Detect dead CMOS CR2032 battery / reset system clock on legacy Win2000 PCs! */
    if (in->system_wall_epoch < ARCR_BUILD_MIN_EPOCH_SEC) {
        arcr_str_copy(out_reason, reason_cap,
                      "시스템 시계가 빌드 기준 시각보다 과거입니다 (CMOS 배터리·날짜 설정 확인 필요)");
        return ARCR_SEC_CLOCK_SKEW_ERR;
    }

    /* 2. Built-in Root CA chain signature check */
    if (!in->trusted_root_signature_ok) {
        arcr_str_copy(out_reason, reason_cap,
                      "내장 신뢰 루트 인증기관(Root CA)에서 서명 체인을 확인할 수 없습니다");
        return ARCR_SEC_CERT_ERROR;
    }

    /* 3. Subject Alternative Name (SAN) host match */
    if (!san_hostname_matches(in->expected_host, in->cert_san_dns)) {
        arcr_str_copy(out_reason, reason_cap,
                      "접속 도메인과 인증서 호스트 이름(SAN)이 일치하지 않습니다");
        return ARCR_SEC_CERT_ERROR;
    }

    /* 4. Certificate validity window */
    if (in->system_wall_epoch < in->not_before_epoch ||
        in->system_wall_epoch > in->not_after_epoch) {
        arcr_str_copy(out_reason, reason_cap,
                      "인증서 유효기간이 만료되었거나 아직 시작되지 않았습니다");
        return ARCR_SEC_CERT_ERROR;
    }

    arcr_str_copy(out_reason, reason_cap, "TLS 검증 완료 · 신뢰 루트 CA 확인");
    return ARCR_SEC_HTTPS_VERIFIED;
}

/* =========================================================================
 * 4. Built-in Waterscape/Arcradient Pages, data: URIs & Guarded file://
 * ========================================================================= */
static const char *const kBuiltInSettingsHtml =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Waterscape 설정</title>"
    "<style>"
    ":root { --accent: #1d4ed8; --card-bg: #f8fafc; }"
    "body { background: #ffffff; color: #0f172a; padding: 20px; }"
    "h1 { color: var(--accent); font-size: 22px; }"
    ".card { background: var(--card-bg); border: 1px solid #cbd5e1; padding: 12px; margin-bottom: 10px; border-radius: 6px; }"
    "</style></head><body>"
    "<h1>Waterscape Browser 1.0 · Arcradient 엔진 설정</h1>"
    "<div class='card'><strong>동작 프로필:</strong> Minimum (Win2000 SP4/x86) · Balanced · Modern (AVX2)</div>"
    "<div class='card'><strong>렌더링 경로:</strong> CPU SIMD 소프트웨어 기본 + 검증된 GPU 선택 가속</div>"
    "<div class='card'><strong>네트워크 스택:</strong> HTTP/1.1 · TLS (OpenSSL / Win32 SChannel) · Gzip/Deflate</div>"
    "<div class='card'><strong>고급 설정:</strong> 세부 플래그는 <code>waterscape://config</code>에서 확인합니다.</div>"
    "</body></html>";

static const char *const kBuiltInWelcomeHtml =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Waterscape Browser 1.0</title>"
    "<style>"
    ":root { --brand: #1d4ed8; --surface: #f1f5f9; }"
    "body { background: #ffffff; color: #0f172a; padding: 24px; }"
    "h1 { color: var(--brand); font-size: 24px; }"
    ".hero { background: var(--surface); border: 1px solid #cbd5e1; padding: 16px; border-radius: 8px; margin-bottom: 12px; }"
    "</style></head><body>"
    "<h1>Waterscape Browser 1.0 (Arcradient Pure C11 Engine)</h1>"
    "<div class='hero'>Windows 2000 SP4부터 최신 64비트 시스템까지 가볍고 빠르게 동작하는 차세대 순수 C11 웹 브라우저입니다.</div>"
    "<p>주소창에 URL 또는 검색어를 입력하거나 <a href='waterscape://settings'>브라우저 설정</a>을 여세요.</p>"
    "</body></html>";

static const char *const kAboutBlankHtml =
    "<!DOCTYPE html><html><head><title>about:blank</title></head><body></body></html>";

static const char *guess_mime_from_path(const char *path) {
    if (!path) return "application/octet-stream";
    const char *dot = strrchr(path, '.');
    if (!dot) return "text/html; charset=utf-8";
    if (arcr_str_eq_nocase(dot, ".html") || arcr_str_eq_nocase(dot, ".htm"))
        return "text/html; charset=utf-8";
    if (arcr_str_eq_nocase(dot, ".css"))  return "text/css; charset=utf-8";
    if (arcr_str_eq_nocase(dot, ".js"))   return "application/javascript; charset=utf-8";
    if (arcr_str_eq_nocase(dot, ".json")) return "application/json; charset=utf-8";
    if (arcr_str_eq_nocase(dot, ".svg"))  return "image/svg+xml";
    if (arcr_str_eq_nocase(dot, ".png"))  return "image/png";
    if (arcr_str_eq_nocase(dot, ".bmp"))  return "image/bmp";
    if (arcr_str_eq_nocase(dot, ".txt"))  return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static int hex_digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool is_safe_local_file_path(const char *raw_path) {
    if (!raw_path || raw_path[0] == '\0') return false;
    size_t len = strlen(raw_path);
    for (size_t i = 0u; i < len; ++i) {
        unsigned char ch = (unsigned char)raw_path[i];
        if (ch < 0x20u) return false;
    }

    /* Block any '..' path traversal segment (both '/' and '\\', raw or %2e encoded) */
    if (strstr(raw_path, "%2e") || strstr(raw_path, "%2E")) return false;
    size_t i = 0u;
    while (i < len) {
        while (i < len && (raw_path[i] == '/' || raw_path[i] == '\\')) ++i;
        size_t start = i;
        while (i < len && raw_path[i] != '/' && raw_path[i] != '\\') ++i;
        if (i - start == 2u && raw_path[start] == '.' && raw_path[start + 1u] == '.') {
            return false;
        }
    }

    /* Block sensitive system/credential files on POSIX and Windows */
    static const char *const kBlockedSubstrings[] = {
        "/etc/shadow", "/etc/gshadow", "/etc/passwd", "/etc/sudoers",
        "/proc/", "/sys/", "/dev/",
        "/.ssh/", "\\.ssh\\", "/.gnupg/", "\\.gnupg\\",
        "/.env", "\\.env", "/.netrc", "\\.netrc", "/.gitcookies", "\\.gitcookies",
        "system32/config/sam", "system32\\config\\sam",
        "winnt/repair/sam", "winnt\\repair\\sam"
    };
    char lower_path[512];
    size_t clen = (len < sizeof(lower_path) - 1u) ? len : (sizeof(lower_path) - 1u);
    for (size_t k = 0u; k < clen; ++k) {
        lower_path[k] = arcr_ascii_tolower(raw_path[k]);
    }
    lower_path[clen] = '\0';

    for (size_t b = 0u; b < ARCR_ARRAY_LEN(kBlockedSubstrings); ++b) {
        if (strstr(lower_path, kBlockedSubstrings[b]) != NULL) {
            return false;
        }
    }
    return true;
}

static bool fill_static_html_response(const char *url, const char *html,
                                      const char *detail_str, ArcrHttpResponse *out) {
    size_t len = strlen(html);
    uint8_t *buf = (uint8_t *)malloc(len + 1u);
    if (!buf) return false;
    memcpy(buf, html, len + 1u);
    out->status_code = 200;
    arcr_str_copy(out->final_url, sizeof(out->final_url), url);
    arcr_str_copy(out->content_type, sizeof(out->content_type), "text/html; charset=utf-8");
    out->body      = buf;
    out->body_len  = len;
    out->security  = ARCR_SEC_LOCAL_OR_NONE;
    arcr_str_copy(out->detail, sizeof(out->detail), detail_str);
    out->from_cache = false;
    return true;
}

static bool handle_data_uri(const char *full_url, const char *payload_spec,
                            ArcrHttpResponse *out) {
    const char *comma = strchr(payload_spec, ',');
    if (!comma) return false;

    size_t meta_len = (size_t)(comma - payload_spec);
    bool is_base64  = false;
    char mime[128]  = "text/plain; charset=us-ascii";

    if (meta_len > 0u) {
        size_t semi_b64 = meta_len;
        if (meta_len >= 7u &&
            arcr_str_eq_nocase_n(payload_spec + meta_len - 7u, 7u, ";base64", 7u)) {
            is_base64 = true;
            semi_b64  = meta_len - 7u;
        }
        if (semi_b64 > 0u) {
            size_t clen = (semi_b64 < sizeof(mime) - 1u) ? semi_b64 : (sizeof(mime) - 1u);
            memcpy(mime, payload_spec, clen);
            mime[clen] = '\0';
        }
    }

    const char *data_part = comma + 1;
    size_t data_len       = strlen(data_part);
    uint8_t *decoded      = NULL;
    size_t decoded_len    = 0u;

    if (is_base64) {
        if (!arcr_base64_decode(data_part, data_len, &decoded, &decoded_len)) {
            return false;
        }
        /* Ensure null termination for text callers */
        uint8_t *nt = (uint8_t *)realloc(decoded, decoded_len + 1u);
        if (!nt) {
            free(decoded);
            return false;
        }
        decoded = nt;
        decoded[decoded_len] = '\0';
    } else {
        decoded = (uint8_t *)malloc(data_len + 1u);
        if (!decoded) return false;
        size_t j = 0u;
        for (size_t i = 0u; i < data_len; ++i) {
            if (data_part[i] == '%' && i + 2u < data_len) {
                int hi = hex_digit_val(data_part[i + 1u]);
                int lo = hex_digit_val(data_part[i + 2u]);
                if (hi >= 0 && lo >= 0) {
                    decoded[j++] = (uint8_t)((hi << 4) | lo);
                    i += 2u;
                    continue;
                }
            }
            decoded[j++] = (uint8_t)data_part[i];
        }
        decoded[j]  = '\0';
        decoded_len = j;
    }

    out->status_code = 200;
    arcr_str_copy(out->final_url, sizeof(out->final_url), full_url);
    arcr_str_copy(out->content_type, sizeof(out->content_type), mime);
    out->body       = decoded;
    out->body_len   = decoded_len;
    out->security   = ARCR_SEC_LOCAL_OR_NONE;
    arcr_str_copy(out->detail, sizeof(out->detail), "인라인 데이터 URI (data:)");
    out->from_cache = false;
    return true;
}

/* =========================================================================
 * 5. Cross-Platform TCP + TLS Transport (POSIX OpenSSL / Win32 SChannel)
 * ========================================================================= */
typedef struct ArcrNetStream {
    bool is_tls;
#if defined(_WIN32)
    SOCKET       sock;
    HMODULE      secur32_mod;
    CredHandle   cred_handle;
    CtxtHandle   ctxt_handle;
    bool         has_cred;
    bool         has_ctxt;
    SecPkgContext_StreamSizes stream_sizes;
    uint8_t      enc_buf[32768];
    size_t       enc_len;
    uint8_t      dec_stash[32768];
    size_t       dec_len;
    size_t       dec_pos;
#else
    int          fd;
#  if defined(ARCR_HAVE_OPENSSL)
    SSL_CTX     *ssl_ctx;
    SSL         *ssl;
#  endif
#endif
} ArcrNetStream;

#if defined(_WIN32)
typedef struct ArcrWinAddrInfo {
    int                     ai_flags;
    int                     ai_family;
    int                     ai_socktype;
    int                     ai_protocol;
    size_t                  ai_addrlen;
    char                   *ai_canonname;
    struct sockaddr        *ai_addr;
    struct ArcrWinAddrInfo *ai_next;
} ArcrWinAddrInfo;

typedef int (WSAAPI *ArcrGetAddrInfoFn)(const char *, const char *,
                                        const ArcrWinAddrInfo *, ArcrWinAddrInfo **);
typedef void (WSAAPI *ArcrFreeAddrInfoFn)(ArcrWinAddrInfo *);

typedef SECURITY_STATUS (SEC_ENTRY *ArcrAcquireCredFn)(
    SEC_CHAR *, SEC_CHAR *, unsigned long, void *, void *, SEC_GET_KEY_FN, void *,
    PCredHandle, PTimeStamp);
typedef SECURITY_STATUS (SEC_ENTRY *ArcrInitSecCtxFn)(
    PCredHandle, PCtxtHandle, SEC_CHAR *, unsigned long, unsigned long, unsigned long,
    PSecBufferDesc, unsigned long, PCtxtHandle, PSecBufferDesc, unsigned long *, PTimeStamp);
typedef SECURITY_STATUS (SEC_ENTRY *ArcrQueryCtxAttrFn)(
    PCtxtHandle, unsigned long, void *);
typedef SECURITY_STATUS (SEC_ENTRY *ArcrEncryptMsgFn)(
    PCtxtHandle, unsigned long, PSecBufferDesc, unsigned long);
typedef SECURITY_STATUS (SEC_ENTRY *ArcrDecryptMsgFn)(
    PCtxtHandle, PSecBufferDesc, unsigned long, unsigned long *);
typedef SECURITY_STATUS (SEC_ENTRY *ArcrFreeCtxBufFn)(void *);
typedef SECURITY_STATUS (SEC_ENTRY *ArcrDeleteSecCtxFn)(PCtxtHandle);
typedef SECURITY_STATUS (SEC_ENTRY *ArcrFreeCredFn)(PCredHandle);

typedef struct ArcrSspiTable {
    ArcrAcquireCredFn  AcquireCredentialsHandleA_fn;
    ArcrInitSecCtxFn   InitializeSecurityContextA_fn;
    ArcrQueryCtxAttrFn QueryContextAttributesA_fn;
    ArcrEncryptMsgFn   EncryptMessage_fn;
    ArcrDecryptMsgFn   DecryptMessage_fn;
    ArcrFreeCtxBufFn   FreeContextBuffer_fn;
    ArcrDeleteSecCtxFn DeleteSecurityContext_fn;
    ArcrFreeCredFn     FreeCredentialsHandle_fn;
} ArcrSspiTable;

/* 0 = uninitialized, 1 = initializing, 2 = ready */
static volatile LONG g_wsa_state = 0L;

static bool ensure_winsock_initialized(void) {
    for (;;) {
        LONG st = InterlockedCompareExchange(&g_wsa_state, 1L, 0L);
        if (st == 2L) return true;
        if (st == 0L) {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
                InterlockedExchange(&g_wsa_state, 0L);
                return false;
            }
            InterlockedExchange(&g_wsa_state, 2L);
            return true;
        }
        Sleep(0);
    }
}

static bool load_sspi_functions(HMODULE mod, ArcrSspiTable *out) {
    if (!mod || !out) return false;
    memset(out, 0, sizeof(*out));
    FARPROC p1 = GetProcAddress(mod, "AcquireCredentialsHandleA");
    FARPROC p2 = GetProcAddress(mod, "InitializeSecurityContextA");
    FARPROC p3 = GetProcAddress(mod, "QueryContextAttributesA");
    FARPROC p4 = GetProcAddress(mod, "EncryptMessage");
    FARPROC p5 = GetProcAddress(mod, "DecryptMessage");
    FARPROC p6 = GetProcAddress(mod, "FreeContextBuffer");
    FARPROC p7 = GetProcAddress(mod, "DeleteSecurityContext");
    FARPROC p8 = GetProcAddress(mod, "FreeCredentialsHandle");
    if (!p1 || !p2 || !p3 || !p4 || !p5 || !p6 || !p7 || !p8) return false;

    memcpy(&out->AcquireCredentialsHandleA_fn, &p1, sizeof(p1));
    memcpy(&out->InitializeSecurityContextA_fn, &p2, sizeof(p2));
    memcpy(&out->QueryContextAttributesA_fn, &p3, sizeof(p3));
    memcpy(&out->EncryptMessage_fn, &p4, sizeof(p4));
    memcpy(&out->DecryptMessage_fn, &p5, sizeof(p5));
    memcpy(&out->FreeContextBuffer_fn, &p6, sizeof(p6));
    memcpy(&out->DeleteSecurityContext_fn, &p7, sizeof(p7));
    memcpy(&out->FreeCredentialsHandle_fn, &p8, sizeof(p8));
    return true;
}

static SOCKET win32_connect_with_timeout(const struct sockaddr *sa, int sa_len) {
    SOCKET s = socket(sa->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    u_long nonblock = 1UL;
    ioctlsocket(s, FIONBIO, &nonblock);

    int rc = connect(s, sa, sa_len);
    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        struct timeval tv;
        tv.tv_sec  = ARCR_NET_CONNECT_TIMEOUT_MS / 1000;
        tv.tv_usec = (ARCR_NET_CONNECT_TIMEOUT_MS % 1000) * 1000;
        int sel = select(0, NULL, &wfds, &efds, &tv);
        if (sel <= 0 || FD_ISSET(s, &efds) || !FD_ISSET(s, &wfds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        int so_err = 0;
        int optlen = (int)sizeof(so_err);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so_err, &optlen) != 0 || so_err != 0) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    nonblock = 0UL;
    ioctlsocket(s, FIONBIO, &nonblock);
    DWORD io_ms = (DWORD)ARCR_NET_IO_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&io_ms, (int)sizeof(io_ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&io_ms, (int)sizeof(io_ms));
    return s;
}

/* Windows 2000 SP4 DNS Guard: dynamically probes ws2_32!getaddrinfo, falls back to gethostbyname */
static SOCKET win32_tcp_connect(const char *host, uint16_t port) {
    if (!host || !ensure_winsock_initialized()) return INVALID_SOCKET;

    char clean_host[192];
    if (host[0] == '[') {
        size_t hlen = strlen(host);
        if (hlen >= 2u && host[hlen - 1u] == ']') {
            size_t inner = hlen - 2u;
            if (inner >= sizeof(clean_host)) inner = sizeof(clean_host) - 1u;
            memcpy(clean_host, host + 1, inner);
            clean_host[inner] = '\0';
            host = clean_host;
        }
    }

    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    ArcrGetAddrInfoFn  fn_getaddrinfo  = NULL;
    ArcrFreeAddrInfoFn fn_freeaddrinfo = NULL;
    if (ws2) {
        FARPROC p_gai = GetProcAddress(ws2, "getaddrinfo");
        FARPROC p_fai = GetProcAddress(ws2, "freeaddrinfo");
        if (p_gai && p_fai) {
            memcpy(&fn_getaddrinfo, &p_gai, sizeof(p_gai));
            memcpy(&fn_freeaddrinfo, &p_fai, sizeof(p_fai));
        }
    }

    if (fn_getaddrinfo && fn_freeaddrinfo) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
        ArcrWinAddrInfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        ArcrWinAddrInfo *res = NULL;
        if (fn_getaddrinfo(host, port_str, &hints, &res) == 0 && res) {
            SOCKET connected = INVALID_SOCKET;
            for (ArcrWinAddrInfo *ai = res; ai != NULL; ai = ai->ai_next) {
                connected = win32_connect_with_timeout(ai->ai_addr, (int)ai->ai_addrlen);
                if (connected != INVALID_SOCKET) break;
            }
            fn_freeaddrinfo(res);
            return connected;
        }
    }

    /* Windows 2000 SP4 IPv4 fallback without ws2_32!getaddrinfo */
    struct sockaddr_in sin4;
    memset(&sin4, 0, sizeof(sin4));
    sin4.sin_family = AF_INET;
    sin4.sin_port   = htons(port);

    unsigned long ip_num = inet_addr(host);
    if (ip_num != INADDR_NONE) {
        sin4.sin_addr.s_addr = ip_num;
        return win32_connect_with_timeout((const struct sockaddr *)&sin4, (int)sizeof(sin4));
    }

    struct hostent *he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) return INVALID_SOCKET;
    for (int i = 0; he->h_addr_list[i] != NULL; ++i) {
        memcpy(&sin4.sin_addr, he->h_addr_list[i], sizeof(struct in_addr));
        SOCKET s = win32_connect_with_timeout((const struct sockaddr *)&sin4, (int)sizeof(sin4));
        if (s != INVALID_SOCKET) return s;
    }
    return INVALID_SOCKET;
}

static bool win32_send_all(SOCKET sock, const uint8_t *buf, size_t len) {
    size_t sent_total = 0u;
    while (sent_total < len) {
        int chunk = (len - sent_total > 65536u) ? 65536 : (int)(len - sent_total);
        int n = send(sock, (const char *)(buf + sent_total), chunk, 0);
        if (n <= 0) return false;
        sent_total += (size_t)n;
    }
    return true;
}

static void stream_close(ArcrNetStream *st) {
    if (!st) return;
    if (st->secur32_mod) {
        ArcrSspiTable sspi;
        if (load_sspi_functions(st->secur32_mod, &sspi)) {
            if (st->has_ctxt) sspi.DeleteSecurityContext_fn(&st->ctxt_handle);
            if (st->has_cred) sspi.FreeCredentialsHandle_fn(&st->cred_handle);
        }
        FreeLibrary(st->secur32_mod);
        st->secur32_mod = NULL;
    }
    if (st->sock != INVALID_SOCKET) {
        closesocket(st->sock);
        st->sock = INVALID_SOCKET;
    }
}

static bool stream_connect(ArcrNetStream *st, const char *host, uint16_t port, bool use_tls,
                           ArcrSecurityState *out_sec, char *out_detail, size_t detail_cap) {
    memset(st, 0, sizeof(*st));
    st->sock   = INVALID_SOCKET;
    st->is_tls = use_tls;

    if (use_tls && arcr_platform_wall_epoch_sec() < ARCR_BUILD_MIN_EPOCH_SEC) {
        *out_sec = ARCR_SEC_CLOCK_SKEW_ERR;
        arcr_str_copy(out_detail, detail_cap,
                      "시스템 시계가 빌드 기준 시각보다 과거입니다 (CMOS 배터리·날짜 설정 확인 필요)");
        return false;
    }

    st->sock = win32_tcp_connect(host, port);
    if (st->sock == INVALID_SOCKET) {
        *out_sec = use_tls ? ARCR_SEC_CERT_ERROR : ARCR_SEC_HTTP_INSECURE;
        arcr_str_copy(out_detail, detail_cap, "서버 TCP 연결 또는 DNS 조회 실패");
        return false;
    }

    if (!use_tls) {
        *out_sec = ARCR_SEC_HTTP_INSECURE;
        arcr_str_copy(out_detail, detail_cap, "암호화되지 않은 HTTP/1.1 연결");
        return true;
    }

    st->secur32_mod = LoadLibraryA("secur32.dll");
    ArcrSspiTable sspi;
    if (!st->secur32_mod || !load_sspi_functions(st->secur32_mod, &sspi)) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap, "Windows SChannel (secur32.dll) 초기화 실패");
        stream_close(st);
        return false;
    }

    SCHANNEL_CRED sc_cred;
    memset(&sc_cred, 0, sizeof(sc_cred));
    sc_cred.dwVersion = SCHANNEL_CRED_VERSION;
    sc_cred.dwFlags   = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_AUTO_CRED_VALIDATION;
    sc_cred.grbitEnabledProtocols =
        SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_1_CLIENT | SP_PROT_TLS1_0_CLIENT;

    TimeStamp expiry;
    SECURITY_STATUS ss = sspi.AcquireCredentialsHandleA_fn(
        NULL, (SEC_CHAR *)UNISP_NAME_A, SECPKG_CRED_OUTBOUND,
        NULL, &sc_cred, NULL, NULL, &st->cred_handle, &expiry);
    if (ss != SEC_E_OK) {
        /* Fallback for vanilla Windows 2000 SP4 SChannel rejecting newer TLS flags */
        sc_cred.grbitEnabledProtocols = 0u;
        ss = sspi.AcquireCredentialsHandleA_fn(
            NULL, (SEC_CHAR *)UNISP_NAME_A, SECPKG_CRED_OUTBOUND,
            NULL, &sc_cred, NULL, NULL, &st->cred_handle, &expiry);
        if (ss != SEC_E_OK) {
            *out_sec = ARCR_SEC_CERT_ERROR;
            arcr_str_copy(out_detail, detail_cap, "SChannel 자격 증명 획득 실패");
            stream_close(st);
            return false;
        }
    }
    st->has_cred = true;

    /* Strip IPv6 brackets for SNI & X.509 target hostname verification */
    char target_host[192];
    arcr_str_copy(target_host, sizeof(target_host), host ? host : "");
    if (target_host[0] == '[') {
        size_t clen = strlen(target_host);
        if (clen >= 2u && target_host[clen - 1u] == ']') {
            memmove(target_host, target_host + 1, clen - 2u);
            target_host[clen - 2u] = '\0';
        }
    }

    ULONG req_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                      ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR |
                      ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
    ULONG ret_flags = 0UL;

    SecBuffer out_sbuf;
    out_sbuf.pvBuffer   = NULL;
    out_sbuf.BufferType = SECBUFFER_TOKEN;
    out_sbuf.cbBuffer   = 0UL;
    SecBufferDesc out_desc;
    out_desc.ulVersion = SECBUFFER_VERSION;
    out_desc.cBuffers  = 1UL;
    out_desc.pBuffers  = &out_sbuf;

    ss = sspi.InitializeSecurityContextA_fn(
        &st->cred_handle, NULL, (SEC_CHAR *)target_host, req_flags, 0UL, 0UL,
        NULL, 0UL, &st->ctxt_handle, &out_desc, &ret_flags, &expiry);
    if (ss != SEC_I_CONTINUE_NEEDED && ss != SEC_E_OK) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap, "TLS ClientHello 생성 실패");
        stream_close(st);
        return false;
    }
    st->has_ctxt = true;

    if (out_sbuf.cbBuffer > 0UL && out_sbuf.pvBuffer) {
        bool sent = win32_send_all(st->sock, (const uint8_t *)out_sbuf.pvBuffer, out_sbuf.cbBuffer);
        sspi.FreeContextBuffer_fn(out_sbuf.pvBuffer);
        if (!sent) {
            *out_sec = ARCR_SEC_CERT_ERROR;
            arcr_str_copy(out_detail, detail_cap, "TLS 핸드셰이크 전송 실패");
            stream_close(st);
            return false;
        }
    }

    while (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_INCOMPLETE_MESSAGE) {
        if (st->enc_len == 0u || ss == SEC_E_INCOMPLETE_MESSAGE) {
            if (st->enc_len >= sizeof(st->enc_buf)) {
                *out_sec = ARCR_SEC_CERT_ERROR;
                arcr_str_copy(out_detail, detail_cap, "TLS 핸드셰이크 레코드 크기 초과");
                stream_close(st);
                return false;
            }
            int r = recv(st->sock, (char *)(st->enc_buf + st->enc_len),
                         (int)(sizeof(st->enc_buf) - st->enc_len), 0);
            if (r <= 0) {
                *out_sec = ARCR_SEC_CERT_ERROR;
                arcr_str_copy(out_detail, detail_cap, "TLS 핸드셰이크 중 서버 연결 종료");
                stream_close(st);
                return false;
            }
            st->enc_len += (size_t)r;
        }

        SecBuffer in_bufs[2];
        in_bufs[0].pvBuffer   = st->enc_buf;
        in_bufs[0].cbBuffer   = (ULONG)st->enc_len;
        in_bufs[0].BufferType = SECBUFFER_TOKEN;
        in_bufs[1].pvBuffer   = NULL;
        in_bufs[1].cbBuffer   = 0UL;
        in_bufs[1].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc in_desc;
        in_desc.ulVersion = SECBUFFER_VERSION;
        in_desc.cBuffers  = 2UL;
        in_desc.pBuffers  = in_bufs;

        out_sbuf.pvBuffer   = NULL;
        out_sbuf.cbBuffer   = 0UL;
        out_sbuf.BufferType = SECBUFFER_TOKEN;

        ss = sspi.InitializeSecurityContextA_fn(
            &st->cred_handle, &st->ctxt_handle, (SEC_CHAR *)target_host, req_flags, 0UL, 0UL,
            &in_desc, 0UL, NULL, &out_desc, &ret_flags, &expiry);

        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            continue;
        }
        if (out_sbuf.cbBuffer > 0UL && out_sbuf.pvBuffer) {
            bool sent = win32_send_all(st->sock, (const uint8_t *)out_sbuf.pvBuffer, out_sbuf.cbBuffer);
            sspi.FreeContextBuffer_fn(out_sbuf.pvBuffer);
            if (!sent) {
                *out_sec = ARCR_SEC_CERT_ERROR;
                arcr_str_copy(out_detail, detail_cap, "TLS 핸드셰이크 응답 전송 실패");
                stream_close(st);
                return false;
            }
        }
        if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer > 0UL) {
            size_t extra = (size_t)in_bufs[1].cbBuffer;
            memmove(st->enc_buf, st->enc_buf + (st->enc_len - extra), extra);
            st->enc_len = extra;
        } else {
            st->enc_len = 0u;
        }

        if (ss == SEC_E_OK) break;
        if (ss != SEC_I_CONTINUE_NEEDED) {
            *out_sec = ARCR_SEC_CERT_ERROR;
            arcr_str_copy(out_detail, detail_cap,
                          "HTTPS 서버 인증서 검증 또는 TLS 프로토콜 협상 실패 (SChannel)");
            stream_close(st);
            return false;
        }
    }

    if (sspi.QueryContextAttributesA_fn(&st->ctxt_handle, SECPKG_ATTR_STREAM_SIZES,
                                        &st->stream_sizes) != SEC_E_OK) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap, "SChannel 스트림 버퍼 크기 조회 실패");
        stream_close(st);
        return false;
    }

    *out_sec = ARCR_SEC_HTTPS_VERIFIED;
    arcr_str_copy(out_detail, detail_cap, "HTTPS 검증 완료 (Windows SChannel)");
    return true;
}

static bool stream_write_all(ArcrNetStream *st, const uint8_t *data, size_t len) {
    if (!st->is_tls) {
        return win32_send_all(st->sock, data, len);
    }
    ArcrSspiTable sspi;
    if (!load_sspi_functions(st->secur32_mod, &sspi)) return false;

    uint8_t packet[16896];
    size_t max_hdr   = st->stream_sizes.cbHeader;
    size_t max_trl   = st->stream_sizes.cbTrailer;
    size_t max_plain = st->stream_sizes.cbMaximumMessage;
    if (max_hdr + max_trl + 256u > sizeof(packet)) return false;
    if (max_plain > sizeof(packet) - max_hdr - max_trl) {
        max_plain = sizeof(packet) - max_hdr - max_trl;
    }

    size_t off = 0u;
    while (off < len) {
        size_t chunk = ARCR_MIN(len - off, max_plain);
        memcpy(packet + max_hdr, data + off, chunk);

        SecBuffer bufs[4];
        bufs[0].pvBuffer   = packet;
        bufs[0].cbBuffer   = (ULONG)max_hdr;
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[1].pvBuffer   = packet + max_hdr;
        bufs[1].cbBuffer   = (ULONG)chunk;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[2].pvBuffer   = packet + max_hdr + chunk;
        bufs[2].cbBuffer   = (ULONG)max_trl;
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[3].pvBuffer   = NULL;
        bufs[3].cbBuffer   = 0UL;
        bufs[3].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc desc;
        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers  = 4UL;
        desc.pBuffers  = bufs;

        if (sspi.EncryptMessage_fn(&st->ctxt_handle, 0UL, &desc, 0UL) != SEC_E_OK) {
            return false;
        }
        size_t total_enc = (size_t)bufs[0].cbBuffer + (size_t)bufs[1].cbBuffer + (size_t)bufs[2].cbBuffer;
        if (!win32_send_all(st->sock, packet, total_enc)) return false;
        off += chunk;
    }
    return true;
}

static int stream_read_some(ArcrNetStream *st, uint8_t *out, size_t cap) {
    if (!st->is_tls) {
        int r = recv(st->sock, (char *)out, (int)ARCR_MIN(cap, 65536u), 0);
        return (r > 0) ? r : 0;
    }
    if (st->dec_pos < st->dec_len) {
        size_t avail = ARCR_MIN(cap, st->dec_len - st->dec_pos);
        memcpy(out, st->dec_stash + st->dec_pos, avail);
        st->dec_pos += avail;
        if (st->dec_pos == st->dec_len) {
            st->dec_pos = 0u;
            st->dec_len = 0u;
        }
        return (int)avail;
    }

    ArcrSspiTable sspi;
    if (!load_sspi_functions(st->secur32_mod, &sspi)) return -1;

    for (;;) {
        if (st->enc_len > 0u) {
            SecBuffer bufs[4];
            bufs[0].pvBuffer   = st->enc_buf;
            bufs[0].cbBuffer   = (ULONG)st->enc_len;
            bufs[0].BufferType = SECBUFFER_DATA;
            for (int i = 1; i < 4; ++i) {
                bufs[i].pvBuffer   = NULL;
                bufs[i].cbBuffer   = 0UL;
                bufs[i].BufferType = SECBUFFER_EMPTY;
            }
            SecBufferDesc desc;
            desc.ulVersion = SECBUFFER_VERSION;
            desc.cBuffers  = 4UL;
            desc.pBuffers  = bufs;

            ULONG qop = 0UL;
            SECURITY_STATUS ss = sspi.DecryptMessage_fn(&st->ctxt_handle, &desc, 0UL, &qop);
            if (ss == SEC_I_CONTEXT_EXPIRED) {
                return 0; /* Clean TLS close_notify */
            }
            if (ss == SEC_E_OK) {
                SecBuffer *data_buf  = NULL;
                SecBuffer *extra_buf = NULL;
                for (int i = 0; i < 4; ++i) {
                    if (!data_buf && bufs[i].BufferType == SECBUFFER_DATA) data_buf = &bufs[i];
                    if (!extra_buf && bufs[i].BufferType == SECBUFFER_EXTRA) extra_buf = &bufs[i];
                }
                size_t plain_len = (data_buf && data_buf->pvBuffer) ? (size_t)data_buf->cbBuffer : 0u;
                if (plain_len > 0u) {
                    size_t copy_now = ARCR_MIN(cap, plain_len);
                    memcpy(out, data_buf->pvBuffer, copy_now);
                    if (plain_len > copy_now) {
                        size_t rem = ARCR_MIN(plain_len - copy_now, sizeof(st->dec_stash));
                        memcpy(st->dec_stash, (const uint8_t *)data_buf->pvBuffer + copy_now, rem);
                        st->dec_pos = 0u;
                        st->dec_len = rem;
                    }
                    if (extra_buf && extra_buf->cbBuffer > 0UL) {
                        size_t ex = (size_t)extra_buf->cbBuffer;
                        memmove(st->enc_buf, st->enc_buf + (st->enc_len - ex), ex);
                        st->enc_len = ex;
                    } else {
                        st->enc_len = 0u;
                    }
                    return (int)copy_now;
                }
                if (extra_buf && extra_buf->cbBuffer > 0UL) {
                    size_t ex = (size_t)extra_buf->cbBuffer;
                    memmove(st->enc_buf, st->enc_buf + (st->enc_len - ex), ex);
                    st->enc_len = ex;
                } else {
                    st->enc_len = 0u;
                }
                continue;
            }
            if (ss != SEC_E_INCOMPLETE_MESSAGE) {
                return -1;
            }
        }

        if (st->enc_len >= sizeof(st->enc_buf)) return -1;
        int r = recv(st->sock, (char *)(st->enc_buf + st->enc_len),
                     (int)(sizeof(st->enc_buf) - st->enc_len), 0);
        if (r <= 0) return 0;
        st->enc_len += (size_t)r;
    }
}

#else /* POSIX Sockets + Optional OpenSSL */

static int posix_connect_with_timeout(const struct sockaddr *sa, socklen_t sa_len) {
    int fd = socket(sa->sa_family, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    int rc = connect(fd, sa, sa_len);
    if (rc < 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv;
        tv.tv_sec  = ARCR_NET_CONNECT_TIMEOUT_MS / 1000;
        tv.tv_usec = (ARCR_NET_CONNECT_TIMEOUT_MS % 1000) * 1000;
        int sel = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (sel <= 0 || !FD_ISSET(fd, &wfds)) {
            close(fd);
            return -1;
        }
        int so_err = 0;
        socklen_t optlen = (socklen_t)sizeof(so_err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &optlen) < 0 || so_err != 0) {
            close(fd);
            return -1;
        }
    }

    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags);
    }
    struct timeval io_tv;
    io_tv.tv_sec  = ARCR_NET_IO_TIMEOUT_MS / 1000;
    io_tv.tv_usec = (ARCR_NET_IO_TIMEOUT_MS % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_tv, (socklen_t)sizeof(io_tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_tv, (socklen_t)sizeof(io_tv));
    return fd;
}

static int posix_tcp_connect(const char *host, uint16_t port) {
    char clean_host[192];
    if (host[0] == '[') {
        size_t hlen = strlen(host);
        if (hlen >= 2u && host[hlen - 1u] == ']') {
            size_t inner = hlen - 2u;
            if (inner >= sizeof(clean_host)) inner = sizeof(clean_host) - 1u;
            memcpy(clean_host, host + 1, inner);
            clean_host[inner] = '\0';
            host = clean_host;
        }
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = posix_connect_with_timeout(ai->ai_addr, ai->ai_addrlen);
        if (fd >= 0) break;
    }
    freeaddrinfo(res);
    return fd;
}

static void stream_close(ArcrNetStream *st) {
    if (!st) return;
#  if defined(ARCR_HAVE_OPENSSL)
    if (st->ssl) {
        SSL_shutdown(st->ssl);
        SSL_free(st->ssl);
        st->ssl = NULL;
    }
    if (st->ssl_ctx) {
        SSL_CTX_free(st->ssl_ctx);
        st->ssl_ctx = NULL;
    }
#  endif
    if (st->fd >= 0) {
        close(st->fd);
        st->fd = -1;
    }
}

static bool stream_connect(ArcrNetStream *st, const char *host, uint16_t port, bool use_tls,
                           ArcrSecurityState *out_sec, char *out_detail, size_t detail_cap) {
    memset(st, 0, sizeof(*st));
    st->fd     = -1;
    st->is_tls = use_tls;

    if (use_tls && arcr_platform_wall_epoch_sec() < ARCR_BUILD_MIN_EPOCH_SEC) {
        *out_sec = ARCR_SEC_CLOCK_SKEW_ERR;
        arcr_str_copy(out_detail, detail_cap,
                      "시스템 시계가 빌드 기준 시각보다 과거입니다 (CMOS 배터리·날짜 설정 확인 필요)");
        return false;
    }

#  if !defined(ARCR_HAVE_OPENSSL)
    if (use_tls) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap,
                      "POSIX 빌드에서 OpenSSL TLS 백엔드가 비활성화되어 있습니다");
        return false;
    }
#  endif

    st->fd = posix_tcp_connect(host, port);
    if (st->fd < 0) {
        *out_sec = use_tls ? ARCR_SEC_CERT_ERROR : ARCR_SEC_HTTP_INSECURE;
        arcr_str_copy(out_detail, detail_cap, "서버 TCP 연결 또는 DNS 조회 실패");
        return false;
    }

    if (!use_tls) {
        *out_sec = ARCR_SEC_HTTP_INSECURE;
        arcr_str_copy(out_detail, detail_cap, "암호화되지 않은 HTTP/1.1 연결");
        return true;
    }

#  if defined(ARCR_HAVE_OPENSSL)
    st->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!st->ssl_ctx) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap, "OpenSSL SSL_CTX 초기화 실패");
        stream_close(st);
        return false;
    }
    (void)SSL_CTX_set_default_verify_paths(st->ssl_ctx);
    SSL_CTX_set_verify(st->ssl_ctx, SSL_VERIFY_NONE, NULL);

    st->ssl = SSL_new(st->ssl_ctx);
    if (!st->ssl) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap, "OpenSSL 세션 할당 실패");
        stream_close(st);
        return false;
    }

    /* Strip IPv6 brackets for SNI & X.509 hostname verification */
    char clean_host[192];
    arcr_str_copy(clean_host, sizeof(clean_host), host);
    if (clean_host[0] == '[') {
        size_t clen = strlen(clean_host);
        if (clen >= 2u && clean_host[clen - 1u] == ']') {
            memmove(clean_host, clean_host + 1, clen - 2u);
            clean_host[clen - 2u] = '\0';
        }
    }

    (void)SSL_set_tlsext_host_name(st->ssl, clean_host);
    X509_VERIFY_PARAM *vparam = SSL_get0_param(st->ssl);
    if (vparam) {
        (void)X509_VERIFY_PARAM_set1_host(vparam, clean_host, 0u);
    }
    (void)SSL_set_fd(st->ssl, st->fd);

    if (SSL_connect(st->ssl) != 1) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap, "TLS 핸드셰이크 협상 실패");
        stream_close(st);
        return false;
    }

    X509 *peer = SSL_get_peer_certificate(st->ssl);
    if (!peer) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap, "서버 TLS 인증서가 제공되지 않았습니다");
        stream_close(st);
        return false;
    }
    X509_free(peer);

    long vres = SSL_get_verify_result(st->ssl);
    if (vres != X509_V_OK) {
        if (vres == X509_V_ERR_CERT_NOT_YET_VALID &&
            arcr_platform_wall_epoch_sec() < ARCR_BUILD_MIN_EPOCH_SEC) {
            *out_sec = ARCR_SEC_CLOCK_SKEW_ERR;
            arcr_str_copy(out_detail, detail_cap,
                          "시스템 시계 오차로 인해 인증서 시작일 이전으로 판정됨");
        } else {
            *out_sec = ARCR_SEC_CERT_ERROR;
            snprintf(out_detail, detail_cap, "TLS 인증서 검증 실패 (%s)",
                     X509_verify_cert_error_string(vres));
        }
        stream_close(st);
        return false;
    }

    *out_sec = ARCR_SEC_HTTPS_VERIFIED;
    snprintf(out_detail, detail_cap, "HTTPS 검증 완료 (%s)", SSL_get_version(st->ssl));
    return true;
#  else
    return false;
#  endif
}

static bool stream_write_all(ArcrNetStream *st, const uint8_t *data, size_t len) {
    size_t off = 0u;
    while (off < len) {
        int chunk = (len - off > 65536u) ? 65536 : (int)(len - off);
        ssize_t n = -1;
#  if defined(ARCR_HAVE_OPENSSL)
        if (st->is_tls) {
            n = SSL_write(st->ssl, data + off, chunk);
        } else
#  endif
        {
#  ifdef MSG_NOSIGNAL
            n = send(st->fd, data + off, (size_t)chunk, MSG_NOSIGNAL);
#  else
            n = send(st->fd, data + off, (size_t)chunk, 0);
#  endif
        }
        if (n <= 0) {
            if (!st->is_tls && errno == EINTR) continue;
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

static int stream_read_some(ArcrNetStream *st, uint8_t *out, size_t cap) {
    int chunk = (cap > 65536u) ? 65536 : (int)cap;
    for (;;) {
        ssize_t n = -1;
#  if defined(ARCR_HAVE_OPENSSL)
        if (st->is_tls) {
            n = SSL_read(st->ssl, out, chunk);
            return (n > 0) ? (int)n : 0;
        }
#  endif
        n = recv(st->fd, out, (size_t)chunk, 0);
        if (n < 0 && errno == EINTR) continue;
        return (n > 0) ? (int)n : 0;
    }
}
#endif

/* =========================================================================
 * 6. HTTP/1.1 Wire Parser (Headers, Chunked Transfer-Encoding, Gzip/Deflate)
 * ========================================================================= */
static bool decode_chunked_body(const uint8_t *src, size_t src_len,
                                uint8_t **out_buf, size_t *out_len) {
    size_t cap  = (src_len > 64u) ? src_len : 64u;
    uint8_t *dst = (uint8_t *)malloc(cap + 1u);
    if (!dst) return false;

    size_t pos = 0u;
    size_t out = 0u;

    while (pos < src_len) {
        /* Skip optional leading CR/LF */
        while (pos < src_len && (src[pos] == '\r' || src[pos] == '\n' || src[pos] == ' ')) {
            ++pos;
        }
        if (pos >= src_len) break;

        /* Parse hex chunk size up to ';', '\r', or '\n' */
        size_t chunk_sz = 0u;
        bool   got_hex  = false;
        while (pos < src_len) {
            int hv = hex_digit_val((char)src[pos]);
            if (hv < 0) break;
            got_hex = true;
            if (chunk_sz > (ARCR_NET_MAX_RESPONSE_BYTES >> 4)) {
                free(dst);
                return false;
            }
            chunk_sz = (chunk_sz << 4) | (size_t)hv;
            ++pos;
        }
        if (!got_hex) break;

        /* Skip optional chunk extension (;name=val) to CRLF */
        while (pos < src_len && src[pos] != '\n') ++pos;
        if (pos < src_len && src[pos] == '\n') ++pos;

        if (chunk_sz == 0u) break; /* Terminal 0-length chunk */
        if (chunk_sz > src_len - pos) {
            chunk_sz = src_len - pos; /* Tolerate truncated final stream */
        }
        if (out + chunk_sz > ARCR_NET_MAX_RESPONSE_BYTES) {
            free(dst);
            return false;
        }
        if (out + chunk_sz + 1u > cap) {
            size_t new_cap = (cap * 2u > out + chunk_sz + 1u) ? (cap * 2u) : (out + chunk_sz + 64u);
            uint8_t *grown = (uint8_t *)realloc(dst, new_cap);
            if (!grown) {
                free(dst);
                return false;
            }
            dst = grown;
            cap = new_cap;
        }
        memcpy(dst + out, src + pos, chunk_sz);
        out += chunk_sz;
        pos += chunk_sz;

        if (pos < src_len && src[pos] == '\r') ++pos;
        if (pos < src_len && src[pos] == '\n') ++pos;
    }

    dst[out] = '\0';
    *out_buf = dst;
    *out_len = out;
    return true;
}

static size_t find_header_end(const uint8_t *buf, size_t len, size_t *out_sep_len) {
    for (size_t i = 0u; i + 1u < len; ++i) {
        if (i + 3u < len && buf[i] == '\r' && buf[i + 1u] == '\n' &&
            buf[i + 2u] == '\r' && buf[i + 3u] == '\n') {
            *out_sep_len = 4u;
            return i;
        }
        if (buf[i] == '\n' && buf[i + 1u] == '\n') {
            *out_sep_len = 2u;
            return i;
        }
    }
    *out_sep_len = 0u;
    return len;
}

static bool header_contains_token_nocase(const char *hay, const char *needle) {
    if (!hay || !needle) return false;
    size_t hlen = strlen(hay);
    size_t nlen = strlen(needle);
    if (nlen == 0u || nlen > hlen) return false;
    for (size_t i = 0u; i + nlen <= hlen; ++i) {
        if (arcr_str_eq_nocase_n(hay + i, nlen, needle, nlen)) return true;
    }
    return false;
}

static bool net_fetch_http(const ArcrHttpRequest *req, ArcrHttpResponse *out_resp) {
    if (!req || !req->url || !out_resp) return false;
    memset(out_resp, 0, sizeof(*out_resp));

    char current_url[512];
    arcr_str_copy(current_url, sizeof(current_url), req->url);
    arcr_str_copy(out_resp->final_url, sizeof(out_resp->final_url), current_url);

    bool is_post = (req->method && arcr_str_eq_nocase(req->method, "POST"));
    const char *post_body = is_post ? req->post_body : NULL;
    size_t post_len       = (is_post && post_body) ? req->post_body_len : 0u;

    for (int hop = 0; hop <= ARCR_NET_MAX_REDIRECTS; ++hop) {
        arcr_str_copy(out_resp->final_url, sizeof(out_resp->final_url), current_url);

        ArcrParsedUrl parsed;
        if (!net_parse_url(current_url, &parsed)) {
            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), "잘못된 URL 형식");
            return false;
        }

        /* 1. Built-in schemes: waterscape://, arcr://, about: */
        if (arcr_str_eq_nocase(parsed.scheme, "waterscape") ||
            arcr_str_eq_nocase(parsed.scheme, "arcr")) {
            if (arcr_str_eq_nocase(parsed.host, "welcome") ||
                arcr_str_eq_nocase(parsed.host, "newtab")) {
                return fill_static_html_response(current_url, kBuiltInWelcomeHtml,
                                                 "Waterscape 시작 페이지", out_resp);
            }
            return fill_static_html_response(current_url, kBuiltInSettingsHtml,
                                             "Arcradient 내부 페이지", out_resp);
        }
        if (arcr_str_eq_nocase(parsed.scheme, "about")) {
            if (arcr_str_eq_nocase(parsed.path, "waterscape")) {
                return fill_static_html_response(current_url, kBuiltInWelcomeHtml,
                                                 "Waterscape 소개 페이지", out_resp);
            }
            return fill_static_html_response(current_url, kAboutBlankHtml,
                                             "빈 페이지 (about:blank)", out_resp);
        }

        /* 2. Inline data: URI */
        if (arcr_str_eq_nocase(parsed.scheme, "data")) {
            return handle_data_uri(current_url, parsed.path, out_resp);
        }

        /* 3. Guarded local file:// URI */
        if (arcr_str_eq_nocase(parsed.scheme, "file")) {
            const char *fpath = parsed.path;
#if defined(_WIN32)
            /* Normalize file:///C:/dir/page.html -> C:/dir/page.html on Win32 */
            if (fpath[0] == '/' && is_alpha_ascii(fpath[1]) && fpath[2] == ':') {
                ++fpath;
            }
#endif
            if (!is_safe_local_file_path(fpath)) {
                out_resp->status_code = 403;
                out_resp->security    = ARCR_SEC_LOCAL_OR_NONE;
                arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                              "보안 정책에 의해 차단된 로컬 파일 경로 (file://)");
                return false;
            }
            FILE *fp = fopen(fpath, "rb");
            if (!fp) {
                out_resp->status_code = 404;
                arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                              "로컬 파일을 찾을 수 없습니다 (file://)");
                return false;
            }
            if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
            long sz = ftell(fp);
            if (sz < 0 || (unsigned long)sz > ARCR_NET_MAX_RESPONSE_BYTES) {
                fclose(fp);
                return false;
            }
            (void)fseek(fp, 0, SEEK_SET);
            uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1u);
            if (!buf) { fclose(fp); return false; }
            size_t rd = fread(buf, 1u, (size_t)sz, fp);
            buf[rd] = '\0';
            fclose(fp);

            out_resp->status_code = 200;
            arcr_str_copy(out_resp->content_type, sizeof(out_resp->content_type),
                          guess_mime_from_path(fpath));
            out_resp->body      = buf;
            out_resp->body_len  = rd;
            out_resp->security  = ARCR_SEC_LOCAL_OR_NONE;
            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), "로컬 문서 (file://)");
            return true;
        }

        /* 4. Check HTTP/HTTPS scheme */
        bool use_tls = arcr_str_eq_nocase(parsed.scheme, "https");
        if (!use_tls && !arcr_str_eq_nocase(parsed.scheme, "http")) {
            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), "지원하지 않는 URL 프로토콜");
            return false;
        }

        /* 5. Check 32-entry LRU GET cache */
        if (!is_post && !req->bypass_cache && !req->private_browsing) {
            if (cache_lookup(current_url, out_resp)) {
                return true;
            }
        }

        /* Verify CSPRNG entropy before external network I/O */
        uint8_t rng_probe[16];
        if (!arcr_platform_csprng_bytes(rng_probe, sizeof(rng_probe))) {
            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), "플랫폼 CSPRNG 난수원 오류");
            return false;
        }

        ArcrNetStream stream;
        if (!stream_connect(&stream, parsed.host, parsed.port, use_tls,
                            &out_resp->security, out_resp->detail, sizeof(out_resp->detail))) {
            return false;
        }

        /* Build HTTP/1.1 request header */
        char host_hdr[224];
        if ((use_tls && parsed.port == 443u) || (!use_tls && parsed.port == 80u)) {
            arcr_str_copy(host_hdr, sizeof(host_hdr), parsed.host);
        } else {
            snprintf(host_hdr, sizeof(host_hdr), "%s:%u", parsed.host, (unsigned)parsed.port);
        }

        char cookie_line[1024];
        cookie_line[0] = '\0';
        if (!req->private_browsing) {
            char cookie_vals[960];
            if (cookie_jar_build_header(parsed.host, parsed.path, use_tls,
                                        cookie_vals, sizeof(cookie_vals)) > 0u) {
                snprintf(cookie_line, sizeof(cookie_line), "Cookie: %s\r\n", cookie_vals);
            }
        }

        char req_head[2560];
        int req_head_len = 0;
        if (is_post) {
            const char *ctype = (req->content_type && req->content_type[0] != '\0')
                                    ? req->content_type
                                    : "application/x-www-form-urlencoded";
            req_head_len = snprintf(
                req_head, sizeof(req_head),
                "POST %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: Mozilla/5.0 (compatible; Waterscape/1.0; Arcradient/1.0)\r\n"
                "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
                "Accept-Encoding: gzip, deflate\r\n"
                "%s"
                "Content-Type: %s\r\n"
                "Content-Length: %lu\r\n"
                "Connection: close\r\n\r\n",
                parsed.path, host_hdr, cookie_line, ctype, (unsigned long)post_len
            );
        } else {
            req_head_len = snprintf(
                req_head, sizeof(req_head),
                "GET %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: Mozilla/5.0 (compatible; Waterscape/1.0; Arcradient/1.0)\r\n"
                "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
                "Accept-Encoding: gzip, deflate\r\n"
                "%s"
                "Connection: close\r\n\r\n",
                parsed.path, host_hdr, cookie_line
            );
        }

        bool write_ok = (req_head_len > 0 && (size_t)req_head_len < sizeof(req_head)) &&
                        stream_write_all(&stream, (const uint8_t *)req_head, (size_t)req_head_len);
        if (write_ok && is_post && post_len > 0u && post_body) {
            write_ok = stream_write_all(&stream, (const uint8_t *)post_body, post_len);
        }
        if (!write_ok) {
            stream_close(&stream);
            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), "HTTP 요청 전송 실패");
            return false;
        }

        /* Read full HTTP/1.1 response stream */
        size_t raw_cap = 16384u;
        size_t raw_len = 0u;
        uint8_t *raw_buf = (uint8_t *)malloc(raw_cap);
        if (!raw_buf) {
            stream_close(&stream);
            return false;
        }

        bool read_err = false;
        for (;;) {
            if (raw_len + 8192u > raw_cap) {
                size_t next_cap = raw_cap * 2u;
                if (next_cap > ARCR_NET_MAX_RESPONSE_BYTES + 65536u) {
                    read_err = true;
                    break;
                }
                uint8_t *grown = (uint8_t *)realloc(raw_buf, next_cap);
                if (!grown) {
                    read_err = true;
                    break;
                }
                raw_buf = grown;
                raw_cap = next_cap;
            }
            int n = stream_read_some(&stream, raw_buf + raw_len, raw_cap - raw_len - 1u);
            if (n < 0) {
                read_err = true;
                break;
            }
            if (n == 0) break;
            raw_len += (size_t)n;
        }
        stream_close(&stream);

        if (read_err || raw_len == 0u) {
            free(raw_buf);
            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), "HTTP 응답 수신 실패");
            return false;
        }
        raw_buf[raw_len] = '\0';

        size_t sep_len = 0u;
        size_t hdr_end = find_header_end(raw_buf, raw_len, &sep_len);
        if (sep_len == 0u) {
            free(raw_buf);
            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), "잘못된 HTTP 응답 헤더");
            return false;
        }

        /* Parse status line: HTTP/1.1 200 OK */
        int status_code = 0;
        const char *hdr_text = (const char *)raw_buf;
        size_t line_end = 0u;
        while (line_end < hdr_end && hdr_text[line_end] != '\r' && hdr_text[line_end] != '\n') {
            ++line_end;
        }
        if (line_end >= 9u && arcr_str_starts_with_nocase(hdr_text, "HTTP/")) {
            size_t sp = 5u;
            while (sp < line_end && hdr_text[sp] != ' ') ++sp;
            while (sp < line_end && hdr_text[sp] == ' ') ++sp;
            int32_t sc = 0;
            if (sp + 3u <= line_end && arcr_parse_i32(hdr_text + sp, 3u, &sc)) {
                status_code = (int)sc;
            }
        }
        if (status_code <= 0) {
            free(raw_buf);
            return false;
        }

        char content_type[128]     = "text/html; charset=utf-8";
        char content_encoding[64]  = {0};
        char transfer_encoding[64] = {0};
        char cache_control[128]    = {0};
        char location_hdr[512]     = {0};

        size_t hpos = line_end;
        while (hpos < hdr_end) {
            while (hpos < hdr_end && (hdr_text[hpos] == '\r' || hdr_text[hpos] == '\n')) ++hpos;
            if (hpos >= hdr_end) break;
            size_t lstart = hpos;
            while (hpos < hdr_end && hdr_text[hpos] != '\r' && hdr_text[hpos] != '\n') ++hpos;
            size_t llen = hpos - lstart;

            size_t colon = 0u;
            while (colon < llen && hdr_text[lstart + colon] != ':') ++colon;
            if (colon == 0u || colon >= llen) continue;

            const char *hk = hdr_text + lstart;
            size_t hk_len  = colon;
            const char *hv = hdr_text + lstart + colon + 1u;
            size_t hv_len  = llen - colon - 1u;
            arcr_str_trim_view(&hk, &hk_len);
            arcr_str_trim_view(&hv, &hv_len);

            if (arcr_str_eq_nocase_n(hk, hk_len, "content-type", 12u)) {
                size_t cl = ARCR_MIN(hv_len, sizeof(content_type) - 1u);
                memcpy(content_type, hv, cl);
                content_type[cl] = '\0';
            } else if (arcr_str_eq_nocase_n(hk, hk_len, "content-encoding", 16u)) {
                size_t cl = ARCR_MIN(hv_len, sizeof(content_encoding) - 1u);
                memcpy(content_encoding, hv, cl);
                content_encoding[cl] = '\0';
            } else if (arcr_str_eq_nocase_n(hk, hk_len, "transfer-encoding", 17u)) {
                size_t cl = ARCR_MIN(hv_len, sizeof(transfer_encoding) - 1u);
                memcpy(transfer_encoding, hv, cl);
                transfer_encoding[cl] = '\0';
            } else if (arcr_str_eq_nocase_n(hk, hk_len, "cache-control", 13u)) {
                size_t cl = ARCR_MIN(hv_len, sizeof(cache_control) - 1u);
                memcpy(cache_control, hv, cl);
                cache_control[cl] = '\0';
            } else if (arcr_str_eq_nocase_n(hk, hk_len, "location", 8u)) {
                size_t cl = ARCR_MIN(hv_len, sizeof(location_hdr) - 1u);
                memcpy(location_hdr, hv, cl);
                location_hdr[cl] = '\0';
            } else if (arcr_str_eq_nocase_n(hk, hk_len, "set-cookie", 10u)) {
                if (!req->private_browsing) {
                    cookie_jar_store_header(parsed.host, parsed.path, hv, hv_len);
                }
            }
        }

        /* Handle HTTP Redirects (301, 302, 303, 307, 308 up to 6 hops) */
        if ((status_code == 301 || status_code == 302 || status_code == 303 ||
             status_code == 307 || status_code == 308) && location_hdr[0] != '\0') {
            free(raw_buf);
            if (hop == ARCR_NET_MAX_REDIRECTS) {
                arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                              "HTTP 리다이렉트 횟수 초과 (최대 6회)");
                return false;
            }
            char next_url[512];
            if (!net_resolve_url(current_url, location_hdr, next_url, sizeof(next_url))) {
                return false;
            }
            arcr_str_copy(current_url, sizeof(current_url), next_url);
            if (status_code == 303 || status_code == 301 || status_code == 302) {
                is_post   = false;
                post_body = NULL;
                post_len  = 0u;
            }
            continue;
        }

        /* Extract payload after CRLFCRLF */
        const uint8_t *payload_ptr = raw_buf + hdr_end + sep_len;
        size_t         payload_len = raw_len - (hdr_end + sep_len);
        uint8_t       *final_body  = NULL;
        size_t         final_len   = 0u;

        if (header_contains_token_nocase(transfer_encoding, "chunked")) {
            if (!decode_chunked_body(payload_ptr, payload_len, &final_body, &final_len)) {
                free(raw_buf);
                arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                              "HTTP chunked 본문 디코딩 실패");
                return false;
            }
            free(raw_buf);
        } else {
            final_body = (uint8_t *)malloc(payload_len + 1u);
            if (!final_body) {
                free(raw_buf);
                return false;
            }
            if (payload_len > 0u) memcpy(final_body, payload_ptr, payload_len);
            final_body[payload_len] = '\0';
            final_len = payload_len;
            free(raw_buf);
        }

        /* Transparently decompress Content-Encoding: gzip / deflate */
        if (content_encoding[0] != '\0' &&
            !arcr_str_eq_nocase(content_encoding, "identity") &&
            final_len > 0u) {
            uint8_t *decomp     = NULL;
            size_t   decomp_len = 0u;
            if (arcr_decompress_http_body(final_body, final_len, content_encoding,
                                          &decomp, &decomp_len, ARCR_NET_MAX_RESPONSE_BYTES)) {
                uint8_t *nt = (uint8_t *)realloc(decomp, decomp_len + 1u);
                if (nt) {
                    nt[decomp_len] = '\0';
                    free(final_body);
                    final_body = nt;
                    final_len  = decomp_len;
                } else {
                    free(decomp);
                }
            }
        }

        out_resp->status_code = status_code;
        arcr_str_copy(out_resp->content_type, sizeof(out_resp->content_type), content_type);
        out_resp->body       = final_body;
        out_resp->body_len   = final_len;
        out_resp->from_cache = false;

        if (!is_post && status_code == 200 && !req->private_browsing &&
            !header_contains_token_nocase(cache_control, "no-store")) {
            cache_insert(current_url, out_resp);
        }
        return true;
    }

    return false;
}

static bool net_load_url_resource(const char *url, char **out_utf8_body,
                                  size_t *out_len, ArcrSecurityState *out_sec,
                                  char *out_detail, size_t detail_cap) {
    if (!url || !out_utf8_body || !out_len || !out_sec) return false;
    *out_utf8_body = NULL;
    *out_len       = 0u;

    ArcrHttpRequest req;
    memset(&req, 0, sizeof(req));
    req.url    = url;
    req.method = "GET";

    ArcrHttpResponse resp;
    bool ok = net_fetch_http(&req, &resp);
    *out_sec = resp.security;
    if (out_detail && detail_cap > 0u) {
        arcr_str_copy(out_detail, detail_cap, resp.detail);
    }
    if (!ok || !resp.body) {
        free(resp.body);
        return false;
    }

    *out_utf8_body = (char *)resp.body;
    *out_len       = resp.body_len;
    return true;
}

static void net_free_resource(char *body) {
    free(body);
}

static const ArcrNetVTable kNetVTable = {
    .struct_size             = sizeof(ArcrNetVTable),
    .parse_url               = net_parse_url,
    .resolve_url             = net_resolve_url,
    .verify_tls_trust        = net_verify_tls_trust,
    .load_url_resource       = net_load_url_resource,
    .fetch_http              = net_fetch_http,
    .clear_cookies_and_cache = net_clear_cookies_and_cache,
    .free_resource           = net_free_resource
};

static ArcrModuleDescriptor g_net_desc = {
    .magic              = ARCR_MODULE_MAGIC,
    .struct_size        = sizeof(ArcrModuleDescriptor),
    .abi_version        = ARCR_ABI_VERSION,
    .engine_build_id    = ARCR_ENGINE_BUILD_ID,
    .arch_id            = 0,
    .min_os_major_minor = ARCR_OS_POSIX_ANY,
    .required_cpu_flags = ARCR_CPU_SCALAR,
    .module_name        = "arcr-net",
    .vtable             = &kNetVTable
};

const ArcrModuleDescriptor *arcr_net_module_query(void);
const ArcrModuleDescriptor *arcr_net_module_query(void) {
    g_net_desc.arch_id = (uint16_t)arcr_host_compile_arch();
#if defined(_WIN32)
    g_net_desc.min_os_major_minor = ARCR_OS_WIN2000;
#endif
    return &g_net_desc;
}

#if !defined(ARCR_NET_STATIC_LINK)
ARCR_API const ArcrModuleDescriptor *arcr_module_query(void) {
    return arcr_net_module_query();
}
#endif
