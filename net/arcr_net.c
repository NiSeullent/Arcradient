/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * net/arcr_net.c - Incremental HTTP/1.1 Keep-Alive Pool, TLS 1.3 (OpenSSL / Win32 SChannel),
 *                  HTTP/HTTPS Proxy CONNECT, Brotli/Gzip, Cookie & Parallel Multi-Core Fetch
 */
#include "arcr_net.h"
#include "../base/arcr_base.h"
#include "../base/arcr_codec.h"
#include "../base/arcr_threadpool.h"
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
#  ifndef SP_PROT_TLS1_3_CLIENT
#    define SP_PROT_TLS1_3_CLIENT 0x00002000u
#  endif
#  ifndef SCH_CRED_NO_DEFAULT_CREDS
#    define SCH_CRED_NO_DEFAULT_CREDS 0x00000010u
#  endif
#  ifndef SCH_CRED_AUTO_CRED_VALIDATION
#    define SCH_CRED_AUTO_CRED_VALIDATION 0x00000020u
#  endif
#  ifndef SCH_USE_STRONG_CRYPTO
#    define SCH_USE_STRONG_CRYPTO 0x00400000u
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
#  ifndef SEC_I_RENEGOTIATE
#    define SEC_I_RENEGOTIATE ((SECURITY_STATUS)0x00090326L)
#  endif
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <pthread.h>
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

#define ARCR_NET_CONNECT_TIMEOUT_MS   5000
#define ARCR_NET_IO_TIMEOUT_MS        8000
#define ARCR_NET_MAX_REDIRECTS        6
#define ARCR_NET_MAX_HEADER_BYTES     (64u * 1024u)
#define ARCR_NET_MAX_RESPONSE_BYTES   (64u * 1024u * 1024u)
#define ARCR_NET_CACHE_SLOTS          32u
#define ARCR_NET_MAX_CACHE_TOTAL      (8u * 1024u * 1024u)
#define ARCR_NET_MAX_CACHE_ITEM       (1u * 1024u * 1024u)
#define ARCR_NET_MAX_COOKIES          128u
#define ARCR_NET_MAX_COOKIE_VAL_LEN   4096u
#define ARCR_NET_KA_POOL_SLOTS        8u
#define ARCR_NET_KA_MAX_IDLE_USEC     (30ULL * 1000000ULL)
#define ARCR_NET_INIT_ENC_CAP         65536u
#define ARCR_NET_MAX_ENC_CAP          (256u * 1024u)
#define ARCR_NET_DEC_STASH_CAP        65536u
#define ARCR_NET_PRE_BUF_CAP          16384u

/* =========================================================================
 * 0. Cross-Platform Thread-Safe Synchronization (Win2000 SP4+ & POSIX)
 * ========================================================================= */
#if defined(_WIN32)
typedef struct ArcrNetMutex {
    volatile LONG    state; /* 0 = uninit, 1 = initing, 2 = ready */
    CRITICAL_SECTION cs;
} ArcrNetMutex;

static void net_mutex_lock(ArcrNetMutex *m) {
    for (;;) {
        LONG st = InterlockedCompareExchange(&m->state, 1L, 0L);
        if (st == 2L) break;
        if (st == 0L) {
            InitializeCriticalSection(&m->cs);
            InterlockedExchange(&m->state, 2L);
            break;
        }
        Sleep(0);
    }
    EnterCriticalSection(&m->cs);
}

static void net_mutex_unlock(ArcrNetMutex *m) {
    LeaveCriticalSection(&m->cs);
}

static ArcrNetMutex g_state_mtx; /* Protects cookies + LRU cache */
static ArcrNetMutex g_pool_mtx;  /* Protects 8-slot Keep-Alive pool */
static ArcrNetMutex g_tpool_mtx; /* Protects parallel batch threadpool */
static ArcrNetMutex g_dns_mtx;   /* Protects Win2000 gethostbyname fallback */
#else
typedef pthread_mutex_t ArcrNetMutex;

static void net_mutex_lock(ArcrNetMutex *m) {
    pthread_mutex_lock(m);
}

static void net_mutex_unlock(ArcrNetMutex *m) {
    pthread_mutex_unlock(m);
}

static ArcrNetMutex g_state_mtx = PTHREAD_MUTEX_INITIALIZER;
static ArcrNetMutex g_pool_mtx  = PTHREAD_MUTEX_INITIALIZER;
static ArcrNetMutex g_tpool_mtx = PTHREAD_MUTEX_INITIALIZER;
#  if defined(ARCR_HAVE_OPENSSL)
static ArcrNetMutex g_ssl_mtx   = PTHREAD_MUTEX_INITIALIZER;
#  endif
#endif

/* Forward declaration for Keep-Alive pool purge called by net_clear_cookies_and_cache */
static void ka_pool_purge_all(void);

/* =========================================================================
 * 1. Thread-Safe In-Memory Cookie Jar (4096B values) & 32-Entry LRU Cache
 * ========================================================================= */
typedef struct ArcrCookieEntry {
    bool in_use;
    bool secure_only;
    char domain[256];
    char path[512];
    char name[256];
    char value[ARCR_NET_MAX_COOKIE_VAL_LEN];
} ArcrCookieEntry;

typedef struct ArcrCacheEntry {
    bool              valid;
    bool              must_revalidate;
    uint64_t          last_used_tick;
    char              url[ARCR_NET_MAX_URL_LEN];
    char              content_type[256];
    char              etag[128];
    char              last_modified[128];
    int               status_code;
    ArcrSecurityState security;
    char              detail[256];
    uint8_t          *body;
    size_t            body_len;
    size_t            encoded_bytes;
    char             *raw_headers;
    size_t            raw_headers_len;
} ArcrCacheEntry;

static ArcrCookieEntry g_cookies[ARCR_NET_MAX_COOKIES];
static ArcrCacheEntry  g_cache[ARCR_NET_CACHE_SLOTS];
static size_t          g_cache_bytes_used = 0u;
static uint64_t        g_cache_clock      = 1u;

static void net_clear_cookies_and_cache(void) {
    ka_pool_purge_all();
    net_mutex_lock(&g_state_mtx);
    memset(g_cookies, 0, sizeof(g_cookies));
    for (size_t i = 0u; i < ARCR_NET_CACHE_SLOTS; ++i) {
        free(g_cache[i].body);
        free(g_cache[i].raw_headers);
        memset(&g_cache[i], 0, sizeof(g_cache[i]));
    }
    g_cache_bytes_used = 0u;
    net_mutex_unlock(&g_state_mtx);
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
    if (cname_len == 0u || cname_len >= sizeof(g_cookies[0].name) ||
        cval_len >= sizeof(g_cookies[0].value)) {
        return;
    }

    char domain_buf[256];
    char path_buf[512];
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
                        char tmp[256];
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

    char name_cstr[256];
    memcpy(name_cstr, cname, cname_len);
    name_cstr[cname_len] = '\0';

    net_mutex_lock(&g_state_mtx);
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
                net_mutex_unlock(&g_state_mtx);
                return;
            }
            memcpy(g_cookies[i].value, cval, cval_len);
            g_cookies[i].value[cval_len] = '\0';
            g_cookies[i].secure_only = secure_only;
            net_mutex_unlock(&g_state_mtx);
            return;
        }
    }
    if (delete_now) {
        net_mutex_unlock(&g_state_mtx);
        return;
    }

    if (free_idx == ARCR_NET_MAX_COOKIES) {
        free_idx = 0u;
    }
    ArcrCookieEntry *ce = &g_cookies[free_idx];
    ce->in_use      = true;
    ce->secure_only = secure_only;
    arcr_str_copy(ce->domain, sizeof(ce->domain), domain_buf);
    arcr_str_copy(ce->path, sizeof(ce->path), path_buf);
    arcr_str_copy(ce->name, sizeof(ce->name), name_cstr);
    memcpy(ce->value, cval, cval_len);
    ce->value[cval_len] = '\0';
    net_mutex_unlock(&g_state_mtx);
}

static size_t cookie_jar_build_header(const char *req_host, const char *req_path,
                                      bool is_https, char *out_buf, size_t cap) {
    if (!out_buf || cap == 0u) return 0u;
    out_buf[0] = '\0';
    size_t pos = 0u;

    net_mutex_lock(&g_state_mtx);
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
    net_mutex_unlock(&g_state_mtx);
    return pos;
}

static bool populate_resp_from_cache_entry_unlocked(ArcrCacheEntry *e, ArcrHttpResponse *out_resp,
                                                    bool revalidated_304) {
    uint8_t *copy = (uint8_t *)malloc(e->body_len + 1u);
    if (!copy) return false;
    char *hdr_copy = NULL;
    if (e->raw_headers && e->raw_headers_len > 0u) {
        hdr_copy = (char *)malloc(e->raw_headers_len + 1u);
        if (!hdr_copy) {
            free(copy);
            return false;
        }
        memcpy(hdr_copy, e->raw_headers, e->raw_headers_len);
        hdr_copy[e->raw_headers_len] = '\0';
    }
    if (e->body_len > 0u) {
        memcpy(copy, e->body, e->body_len);
    }
    copy[e->body_len] = '\0';

    e->last_used_tick         = ++g_cache_clock;
    out_resp->status_code     = e->status_code;
    arcr_str_copy(out_resp->final_url, sizeof(out_resp->final_url), e->url);
    arcr_str_copy(out_resp->content_type, sizeof(out_resp->content_type), e->content_type);
    out_resp->body            = copy;
    out_resp->body_len        = e->body_len;
    out_resp->encoded_bytes   = revalidated_304 ? 0u : e->encoded_bytes;
    out_resp->decoded_bytes   = e->body_len;
    out_resp->raw_headers     = hdr_copy;
    out_resp->raw_headers_len = hdr_copy ? e->raw_headers_len : 0u;
    out_resp->security        = e->security;
    arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                  revalidated_304 ? "304 Not Modified (캐시 재검증 완료)" : e->detail);
    out_resp->from_cache      = true;
    return true;
}

static bool cache_lookup_or_validators(const char *url, ArcrHttpResponse *out_resp,
                                       char *out_etag, size_t etag_cap,
                                       char *out_lastmod, size_t lm_cap,
                                       bool *out_has_validator) {
    if (out_etag && etag_cap > 0u) out_etag[0] = '\0';
    if (out_lastmod && lm_cap > 0u) out_lastmod[0] = '\0';
    if (out_has_validator) *out_has_validator = false;

    net_mutex_lock(&g_state_mtx);
    for (size_t i = 0u; i < ARCR_NET_CACHE_SLOTS; ++i) {
        ArcrCacheEntry *e = &g_cache[i];
        if (!e->valid || strcmp(e->url, url) != 0) continue;

        if (e->must_revalidate) {
            if (out_etag && etag_cap > 0u) arcr_str_copy(out_etag, etag_cap, e->etag);
            if (out_lastmod && lm_cap > 0u) arcr_str_copy(out_lastmod, lm_cap, e->last_modified);
            if (out_has_validator) {
                *out_has_validator = (e->etag[0] != '\0' || e->last_modified[0] != '\0');
            }
            net_mutex_unlock(&g_state_mtx);
            return false;
        }

        bool ok = populate_resp_from_cache_entry_unlocked(e, out_resp, false);
        net_mutex_unlock(&g_state_mtx);
        return ok;
    }
    net_mutex_unlock(&g_state_mtx);
    return false;
}

static bool cache_fulfill_304(const char *url, ArcrHttpResponse *out_resp) {
    net_mutex_lock(&g_state_mtx);
    for (size_t i = 0u; i < ARCR_NET_CACHE_SLOTS; ++i) {
        ArcrCacheEntry *e = &g_cache[i];
        if (!e->valid || strcmp(e->url, url) != 0) continue;
        bool ok = populate_resp_from_cache_entry_unlocked(e, out_resp, true);
        net_mutex_unlock(&g_state_mtx);
        return ok;
    }
    net_mutex_unlock(&g_state_mtx);
    return false;
}

static void cache_evict_slot_unlocked(size_t idx) {
    if (g_cache[idx].valid) {
        if (g_cache_bytes_used >= g_cache[idx].body_len) {
            g_cache_bytes_used -= g_cache[idx].body_len;
        } else {
            g_cache_bytes_used = 0u;
        }
        free(g_cache[idx].body);
        free(g_cache[idx].raw_headers);
        memset(&g_cache[idx], 0, sizeof(g_cache[idx]));
    }
}

static void cache_insert(const char *url, const ArcrHttpResponse *resp,
                         const char *etag, const char *last_modified, bool must_revalidate) {
    if (!url || !resp || !resp->body) return;
    if (resp->body_len > ARCR_NET_MAX_CACHE_ITEM) return;
    if (strlen(url) >= sizeof(g_cache[0].url)) return;

    uint8_t *buf = (uint8_t *)malloc(resp->body_len + 1u);
    if (!buf) return;
    char *hdr_copy = NULL;
    if (resp->raw_headers && resp->raw_headers_len > 0u) {
        hdr_copy = (char *)malloc(resp->raw_headers_len + 1u);
        if (hdr_copy) {
            memcpy(hdr_copy, resp->raw_headers, resp->raw_headers_len);
            hdr_copy[resp->raw_headers_len] = '\0';
        }
    }
    if (resp->body_len > 0u) memcpy(buf, resp->body, resp->body_len);
    buf[resp->body_len] = '\0';

    net_mutex_lock(&g_state_mtx);
    for (size_t i = 0u; i < ARCR_NET_CACHE_SLOTS; ++i) {
        if (g_cache[i].valid && strcmp(g_cache[i].url, url) == 0) {
            cache_evict_slot_unlocked(i);
            break;
        }
    }

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
        cache_evict_slot_unlocked(victim);
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
    if (target == ARCR_NET_CACHE_SLOTS) {
        net_mutex_unlock(&g_state_mtx);
        free(buf);
        free(hdr_copy);
        return;
    }
    if (g_cache[target].valid) cache_evict_slot_unlocked(target);

    ArcrCacheEntry *e  = &g_cache[target];
    e->valid           = true;
    e->must_revalidate = must_revalidate;
    e->last_used_tick  = ++g_cache_clock;
    arcr_str_copy(e->url, sizeof(e->url), url);
    arcr_str_copy(e->content_type, sizeof(e->content_type), resp->content_type);
    arcr_str_copy(e->etag, sizeof(e->etag), etag ? etag : "");
    arcr_str_copy(e->last_modified, sizeof(e->last_modified), last_modified ? last_modified : "");
    e->status_code     = resp->status_code;
    e->security        = resp->security;
    arcr_str_copy(e->detail, sizeof(e->detail), resp->detail);
    e->body            = buf;
    e->body_len        = resp->body_len;
    e->encoded_bytes   = resp->encoded_bytes;
    e->raw_headers     = hdr_copy;
    e->raw_headers_len = hdr_copy ? resp->raw_headers_len : 0u;
    g_cache_bytes_used += resp->body_len;
    net_mutex_unlock(&g_state_mtx);
}

/* =========================================================================
 * 2. URL Parsing & RFC 3986 Relative URL Resolution (4096B + ?query Fix)
 * ========================================================================= */
static bool is_alpha_ascii(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_scheme_char(char c) {
    return is_alpha_ascii(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

/* Normalizes RFC 3986 '.' and '..' path segments into `out_path` */
static void normalize_dot_segments(const char *in_path, size_t in_len,
                                   char *out_path, size_t out_cap) {
    if (!out_path || out_cap == 0u) return;
    if (!in_path || in_len == 0u) {
        arcr_str_copy(out_path, out_cap, "/");
        return;
    }

    const char *segs[128];
    size_t      seg_lens[128];
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

static bool is_opaque_browser_scheme(const char *s, size_t len) {
    return (len >= 6u && arcr_str_eq_nocase_n(s, 6u, "about:", 6u)) ||
           (len >= 5u && arcr_str_eq_nocase_n(s, 5u, "data:", 5u)) ||
           (len >= 11u && arcr_str_eq_nocase_n(s, 11u, "javascript:", 11u)) ||
           (len >= 7u && arcr_str_eq_nocase_n(s, 7u, "mailto:", 7u));
}

static bool net_parse_url(const char *raw_url, ArcrParsedUrl *out) {
    if (!raw_url || !out) return false;
    memset(out, 0, sizeof(*out));

    size_t rlen = strlen(raw_url);
    arcr_str_trim_view(&raw_url, &rlen);
    if (rlen == 0u) return false;

    /* Handle opaque single-colon browser schemes directly before fixed buffer copy */
    if (is_opaque_browser_scheme(raw_url, rlen)) {
        const char *colon = memchr(raw_url, ':', rlen);
        if (!colon) return false;
        size_t slen = (size_t)(colon - raw_url);
        if (slen >= sizeof(out->scheme)) return false;
        for (size_t i = 0u; i < slen; ++i) {
            out->scheme[i] = arcr_ascii_tolower(raw_url[i]);
        }
        out->scheme[slen] = '\0';
        size_t plen = rlen - slen - 1u;
        if (plen >= sizeof(out->path)) plen = sizeof(out->path) - 1u;
        memcpy(out->path, colon + 1, plen);
        out->path[plen] = '\0';
        return true;
    }

    /* Strip #fragment before copying into 4096B work buffer so HTTP wire requests never leak fragments */
    const char *hash_pos = memchr(raw_url, '#', rlen);
    if (hash_pos) {
        rlen = (size_t)(hash_pos - raw_url);
        if (rlen == 0u) return false;
    }

    char *work = (char *)malloc(ARCR_NET_MAX_URL_LEN);
    if (!work) return false;
    if (rlen >= ARCR_NET_MAX_URL_LEN) rlen = ARCR_NET_MAX_URL_LEN - 1u;
    memcpy(work, raw_url, rlen);
    work[rlen] = '\0';

    const char *sep = strstr(work, "://");
    const char *rest = work;
    if (sep) {
        size_t slen = (size_t)(sep - work);
        if (slen == 0u || slen >= sizeof(out->scheme) || !is_alpha_ascii(work[0])) {
            free(work);
            return false;
        }
        for (size_t i = 0u; i < slen; ++i) {
            if (!is_scheme_char(work[i])) {
                free(work);
                return false;
            }
            out->scheme[i] = arcr_ascii_tolower(work[i]);
        }
        out->scheme[slen] = '\0';
        rest = sep + 3;
    } else {
        arcr_str_copy(out->scheme, sizeof(out->scheme), "https");
    }

    if (arcr_str_eq_nocase(out->scheme, "file")) {
        /* Support file:///path and file://localhost/path */
        if (arcr_str_starts_with_nocase(rest, "localhost/")) {
            rest += 9; /* keep leading '/' */
        }
        arcr_str_copy(out->path, sizeof(out->path), rest);
        free(work);
        return true;
    }

    const char *slash = strchr(rest, '/');
    const char *qmark = strchr(rest, '?');
    const char *delim = slash;
    if (qmark && (!delim || qmark < delim)) {
        delim = qmark;
    }

    size_t host_port_len = delim ? (size_t)(delim - rest) : strlen(rest);
    /* Strip optional userinfo@ if present in authority */
    const char *at_sign = memchr(rest, '@', host_port_len);
    if (at_sign) {
        size_t skip = (size_t)(at_sign - rest + 1);
        rest += skip;
        host_port_len -= skip;
    }

    if (host_port_len == 0u &&
        (arcr_str_eq_nocase(out->scheme, "http") || arcr_str_eq_nocase(out->scheme, "https"))) {
        free(work);
        return false;
    }

    /* Support IPv6 literal [::1]:8080 or IPv4/hostname:port */
    if (host_port_len > 0u && rest[0] == '[') {
        const char *rb = memchr(rest, ']', host_port_len);
        if (!rb) {
            free(work);
            return false;
        }
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
        const char *path_q = strchr(delim, '?');
        size_t raw_plen = path_q ? (size_t)(path_q - delim) : strlen(delim);
        char *norm_path = (char *)malloc(ARCR_NET_MAX_URL_LEN);
        if (!norm_path) {
            free(work);
            return false;
        }
        normalize_dot_segments(delim, raw_plen, norm_path, ARCR_NET_MAX_URL_LEN);
        if (path_q) {
            snprintf(out->path, sizeof(out->path), "%s%s", norm_path, path_q);
        } else {
            arcr_str_copy(out->path, sizeof(out->path), norm_path);
        }
        free(norm_path);
    }
    free(work);
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

    /* Fast-path for opaque URIs (including 500 KB+ inline base64 data: URIs!) without fixed buffer cap */
    if (is_opaque_browser_scheme(rel_url, rlen)) {
        size_t clen = (rlen < out_cap) ? rlen : (out_cap - 1u);
        memcpy(out_resolved, rel_url, clen);
        out_resolved[clen] = '\0';
        return true;
    }

    if (rlen >= ARCR_NET_MAX_URL_LEN) return false;

    /* Heap-allocate URL resolution scratch struct to keep thread stacks small and avoid -Wrestrict */
    typedef struct UrlResolveScratch {
        char rel_buf[ARCR_NET_MAX_URL_LEN];
        char frag_part[ARCR_NET_MAX_URL_LEN];
        char query_part[ARCR_NET_MAX_URL_LEN];
        char base_path_only[ARCR_NET_MAX_URL_LEN];
        char norm_path[ARCR_NET_MAX_URL_LEN];
        char merged[ARCR_NET_MAX_URL_LEN * 2u];
        ArcrParsedUrl parsed_tmp;
    } UrlResolveScratch;

    UrlResolveScratch *sc = (UrlResolveScratch *)malloc(sizeof(UrlResolveScratch));
    if (!sc) return false;

    memcpy(sc->rel_buf, rel_url, rlen);
    sc->rel_buf[rlen] = '\0';
    sc->frag_part[0]  = '\0';
    sc->query_part[0] = '\0';

    /* 1. Full URL with explicit scheme (https://, waterscape://, file://, etc.) */
    if (has_explicit_url_scheme(sc->rel_buf, rlen)) {
        const char *frag = strchr(sc->rel_buf, '#');
        if (arcr_str_starts_with_nocase(sc->rel_buf, "http://") ||
            arcr_str_starts_with_nocase(sc->rel_buf, "https://")) {
            if (frag) arcr_str_copy(sc->frag_part, sizeof(sc->frag_part), frag);
            if (!net_parse_url(sc->rel_buf, &sc->parsed_tmp)) {
                free(sc);
                return false;
            }
            char prefix[300];
            format_authority_prefix(&sc->parsed_tmp, prefix, sizeof(prefix));
            snprintf(out_resolved, out_cap, "%s%s%s", prefix, sc->parsed_tmp.path, sc->frag_part);
            free(sc);
            return true;
        }
        arcr_str_copy(out_resolved, out_cap, sc->rel_buf);
        free(sc);
        return true;
    }

    if (!base_url || base_url[0] == '\0') {
        free(sc);
        return false;
    }
    if (!net_parse_url(base_url, &sc->parsed_tmp)) {
        free(sc);
        return false;
    }

    /* 2. Protocol-relative URL: //cdn.example.com/a.css */
    if (rlen >= 2u && sc->rel_buf[0] == '/' && sc->rel_buf[1] == '/') {
        const char *sch = (sc->parsed_tmp.scheme[0] != '\0') ? sc->parsed_tmp.scheme : "https";
        snprintf(sc->merged, sizeof(sc->merged), "%s:%s", sch, sc->rel_buf);
        bool ok = net_resolve_url(base_url, sc->merged, out_resolved, out_cap);
        free(sc);
        return ok;
    }

    /* 3. Fragment-only reference: #section */
    if (sc->rel_buf[0] == '#') {
        const char *bfrag = strchr(base_url, '#');
        size_t blen = bfrag ? (size_t)(bfrag - base_url) : strlen(base_url);
        if (blen + rlen >= out_cap) {
            free(sc);
            return false;
        }
        memcpy(out_resolved, base_url, blen);
        memcpy(out_resolved + blen, sc->rel_buf, rlen + 1u);
        free(sc);
        return true;
    }

    /* Split rel_buf into rel_path, ?query, and #fragment */
    char *rel_frag = strchr(sc->rel_buf, '#');
    if (rel_frag) {
        arcr_str_copy(sc->frag_part, sizeof(sc->frag_part), rel_frag);
        *rel_frag = '\0';
    }

    /* Extract base path without ?query */
    const char *bq = strchr(sc->parsed_tmp.path, '?');
    if (bq) {
        size_t bplen = (size_t)(bq - sc->parsed_tmp.path);
        if (bplen >= sizeof(sc->base_path_only)) bplen = sizeof(sc->base_path_only) - 1u;
        memcpy(sc->base_path_only, sc->parsed_tmp.path, bplen);
        sc->base_path_only[bplen] = '\0';
    } else {
        arcr_str_copy(sc->base_path_only, sizeof(sc->base_path_only),
                      sc->parsed_tmp.path[0] ? sc->parsed_tmp.path : "/");
    }

    /* 4. RFC 3986 Section 5.2.2 Query-only reference (?query):
     *    Keep full base_path_only (do NOT strip last path segment!) and replace query! */
    if (sc->rel_buf[0] == '?') {
        arcr_str_copy(sc->query_part, sizeof(sc->query_part), sc->rel_buf);
        arcr_str_copy(sc->norm_path, sizeof(sc->norm_path), sc->base_path_only);
    } else {
        char *rel_query = strchr(sc->rel_buf, '?');
        if (rel_query) {
            arcr_str_copy(sc->query_part, sizeof(sc->query_part), rel_query);
            *rel_query = '\0';
        }
        if (sc->rel_buf[0] == '\0') {
            arcr_str_copy(sc->norm_path, sizeof(sc->norm_path), sc->base_path_only);
        } else if (sc->rel_buf[0] == '/') {
            /* 5. Root-relative reference: /assets/app.css */
            normalize_dot_segments(sc->rel_buf, strlen(sc->rel_buf),
                                   sc->norm_path, sizeof(sc->norm_path));
        } else {
            /* 6. Relative path: ../img/logo.png or style.css */
            const char *last_slash = strrchr(sc->base_path_only, '/');
            if (last_slash) {
                size_t dir_len = (size_t)(last_slash - sc->base_path_only + 1);
                if (dir_len >= sizeof(sc->merged)) dir_len = sizeof(sc->merged) - 1u;
                memcpy(sc->merged, sc->base_path_only, dir_len);
                sc->merged[dir_len] = '\0';
                arcr_str_copy(sc->merged + dir_len, sizeof(sc->merged) - dir_len, sc->rel_buf);
            } else {
                snprintf(sc->merged, sizeof(sc->merged), "/%s", sc->rel_buf);
            }
            normalize_dot_segments(sc->merged, strlen(sc->merged),
                                   sc->norm_path, sizeof(sc->norm_path));
        }
    }

    if (arcr_str_eq_nocase(sc->parsed_tmp.scheme, "file")) {
        snprintf(out_resolved, out_cap, "file://%s%s%s",
                 sc->norm_path, sc->query_part, sc->frag_part);
        free(sc);
        return true;
    }

    char prefix[300];
    format_authority_prefix(&sc->parsed_tmp, prefix, sizeof(prefix));
    snprintf(out_resolved, out_cap, "%s%s%s%s",
             prefix, sc->norm_path, sc->query_part, sc->frag_part);
    free(sc);
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
 * 4. Built-in Waterscape/Arcradient Pages, Large data: URIs & Guarded file://
 * ========================================================================= */
static const char *const kBuiltInSettingsHtml =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Waterscape 설정</title>"
    "<style>"
    ":root { --accent: #1d4ed8; --card-bg: #f8fafc; }"
    "body { background: #ffffff; color: #0f172a; padding: 20px; }"
    "h1 { color: var(--accent); font-size: 22px; }"
    ".card { background: var(--card-bg); border: 1px solid #cbd5e1; padding: 12px; margin-bottom: 10px; border-radius: 6px; }"
    "</style></head><body>"
    "<h1>Waterscape Browser 2.0 · Arcradient 엔진 설정</h1>"
    "<div class='card'><strong>동작 프로필:</strong> Minimum (Win2000 SP4/x86) · Balanced · Modern (AVX2)</div>"
    "<div class='card'><strong>렌더링 경로:</strong> CPU SIMD 소프트웨어 기본 + 검증된 GPU 선택 가속</div>"
    "<div class='card'><strong>네트워크 스택:</strong> HTTP/1.1 Keep-Alive · TLS 1.3 (OpenSSL / Win32 SChannel) · Brotli/Gzip</div>"
    "<div class='card'><strong>고급 설정:</strong> 세부 플래그는 <code>waterscape://config</code>에서 확인합니다.</div>"
    "</body></html>";

static const char *const kBuiltInWelcomeHtml =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Waterscape Browser 2.0</title>"
    "<style>"
    ":root { --brand: #1d4ed8; --surface: #f1f5f9; }"
    "body { background: #ffffff; color: #0f172a; padding: 24px; }"
    "h1 { color: var(--brand); font-size: 24px; }"
    ".hero { background: var(--surface); border: 1px solid #cbd5e1; padding: 16px; border-radius: 8px; margin-bottom: 12px; }"
    "</style></head><body>"
    "<h1>Waterscape Browser 2.0 (Arcradient Pure C11 Engine)</h1>"
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
    if (arcr_str_eq_nocase(dot, ".css"))   return "text/css; charset=utf-8";
    if (arcr_str_eq_nocase(dot, ".js") || arcr_str_eq_nocase(dot, ".mjs"))
        return "application/javascript; charset=utf-8";
    if (arcr_str_eq_nocase(dot, ".json"))  return "application/json; charset=utf-8";
    if (arcr_str_eq_nocase(dot, ".svg"))   return "image/svg+xml";
    if (arcr_str_eq_nocase(dot, ".png"))   return "image/png";
    if (arcr_str_eq_nocase(dot, ".jpg") || arcr_str_eq_nocase(dot, ".jpeg"))
        return "image/jpeg";
    if (arcr_str_eq_nocase(dot, ".gif"))   return "image/gif";
    if (arcr_str_eq_nocase(dot, ".webp"))  return "image/webp";
    if (arcr_str_eq_nocase(dot, ".ico"))   return "image/x-icon";
    if (arcr_str_eq_nocase(dot, ".bmp"))   return "image/bmp";
    if (arcr_str_eq_nocase(dot, ".txt"))   return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static int hex_digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool percent_decode_path(const char *src, size_t src_len, char *dst, size_t dst_cap) {
    if (!src || !dst || dst_cap == 0u) return false;
    size_t j = 0u;
    for (size_t i = 0u; i < src_len; ++i) {
        if (j + 1u >= dst_cap) return false;
        if (src[i] == '%' && i + 2u < src_len) {
            int hi = hex_digit_val(src[i + 1u]);
            int lo = hex_digit_val(src[i + 2u]);
            if (hi >= 0 && lo >= 0) {
                unsigned char byte_val = (unsigned char)((hi << 4) | lo);
                if (byte_val < 0x20u) return false; /* Reject %00 and control chars */
                dst[j++] = (char)byte_val;
                i += 2u;
                continue;
            }
        }
        if ((unsigned char)src[i] < 0x20u) return false;
        dst[j++] = src[i];
    }
    dst[j] = '\0';
    return true;
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
    char *lower_path = (char *)malloc(len + 1u);
    if (!lower_path) return false;
    for (size_t k = 0u; k < len; ++k) {
        lower_path[k] = arcr_ascii_tolower(raw_path[k]);
    }
    lower_path[len] = '\0';

    for (size_t b = 0u; b < ARCR_ARRAY_LEN(kBlockedSubstrings); ++b) {
        if (strstr(lower_path, kBlockedSubstrings[b]) != NULL) {
            free(lower_path);
            return false;
        }
    }
    free(lower_path);
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
    out->body          = buf;
    out->body_len      = len;
    out->encoded_bytes = len;
    out->decoded_bytes = len;
    out->security      = ARCR_SEC_LOCAL_OR_NONE;
    arcr_str_copy(out->detail, sizeof(out->detail), detail_str);
    out->from_cache = false;
    return true;
}

static bool handle_data_uri_view(const char *full_url, size_t full_url_len,
                                 const char *payload_spec, size_t payload_len,
                                 ArcrHttpResponse *out) {
    /* Strip optional #fragment at end of data: URI */
    const char *hash_p = memchr(payload_spec, '#', payload_len);
    if (hash_p) {
        payload_len = (size_t)(hash_p - payload_spec);
    }

    const char *comma = memchr(payload_spec, ',', payload_len);
    if (!comma) return false;

    size_t meta_len = (size_t)(comma - payload_spec);
    bool is_base64  = false;
    char mime[256]  = "text/plain; charset=us-ascii";

    if (meta_len > 0u) {
        /* Detect ;base64 parameter (even if whitespace surrounds ';') */
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
    size_t data_len       = payload_len - meta_len - 1u;
    if (data_len > ARCR_NET_MAX_RESPONSE_BYTES * 2u) return false;

    uint8_t *decoded   = NULL;
    size_t decoded_len = 0u;

    if (is_base64) {
        /* Support percent-encoded base64 (e.g. %2B, %2F, %3D, %20) if '%' appears */
        char *unpct = NULL;
        const char *b64_src = data_part;
        size_t b64_len = data_len;
        if (memchr(data_part, '%', data_len) != NULL) {
            unpct = (char *)malloc(data_len + 1u);
            if (!unpct) return false;
            size_t j = 0u;
            for (size_t i = 0u; i < data_len; ++i) {
                if (data_part[i] == '%' && i + 2u < data_len) {
                    int hi = hex_digit_val(data_part[i + 1u]);
                    int lo = hex_digit_val(data_part[i + 2u]);
                    if (hi >= 0 && lo >= 0) {
                        unpct[j++] = (char)((hi << 4) | lo);
                        i += 2u;
                        continue;
                    }
                }
                unpct[j++] = data_part[i];
            }
            unpct[j] = '\0';
            b64_src = unpct;
            b64_len = j;
        }
        bool ok = arcr_base64_decode(b64_src, b64_len, &decoded, &decoded_len);
        free(unpct);
        if (!ok) return false;

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
    size_t flen = (full_url_len < sizeof(out->final_url) - 1u)
                      ? full_url_len
                      : (sizeof(out->final_url) - 1u);
    memcpy(out->final_url, full_url, flen);
    out->final_url[flen] = '\0';
    arcr_str_copy(out->content_type, sizeof(out->content_type), mime);
    out->body          = decoded;
    out->body_len      = decoded_len;
    out->encoded_bytes = data_len;
    out->decoded_bytes = decoded_len;
    out->security      = ARCR_SEC_LOCAL_OR_NONE;
    arcr_str_copy(out->detail, sizeof(out->detail), "인라인 데이터 URI (data:)");
    out->from_cache = false;
    return true;
}

/* =========================================================================
 * 5. HTTP_PROXY / HTTPS_PROXY / NO_PROXY & Base64 Proxy-Authorization Helper
 * ========================================================================= */
typedef struct ArcrProxyConfig {
    bool     enabled;
    char     host[256];
    uint16_t port;
    char     auth_header[512]; /* Empty or "Proxy-Authorization: Basic ...\r\n" */
} ArcrProxyConfig;

static void base64_encode_small(const uint8_t *src, size_t len, char *out, size_t out_cap) {
    static const char kB64[65] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!out || out_cap == 0u) return;
    size_t j = 0u;
    for (size_t i = 0u; i < len; i += 3u) {
        if (j + 5u >= out_cap) break;
        uint32_t v = ((uint32_t)src[i]) << 16;
        if (i + 1u < len) v |= ((uint32_t)src[i + 1u]) << 8;
        if (i + 2u < len) v |= (uint32_t)src[i + 2u];
        out[j++] = kB64[(v >> 18) & 0x3Fu];
        out[j++] = kB64[(v >> 12) & 0x3Fu];
        out[j++] = (i + 1u < len) ? kB64[(v >> 6) & 0x3Fu] : '=';
        out[j++] = (i + 2u < len) ? kB64[v & 0x3Fu] : '=';
    }
    out[j] = '\0';
}

static size_t percent_decode_inline(const char *src, size_t len, uint8_t *dst, size_t cap) {
    size_t j = 0u;
    for (size_t i = 0u; i < len && j + 1u < cap; ++i) {
        if (src[i] == '%' && i + 2u < len) {
            int hi = hex_digit_val(src[i + 1u]);
            int lo = hex_digit_val(src[i + 2u]);
            if (hi >= 0 && lo >= 0) {
                dst[j++] = (uint8_t)((hi << 4) | lo);
                i += 2u;
                continue;
            }
        }
        dst[j++] = (uint8_t)src[i];
    }
    if (j < cap) dst[j] = 0u;
    return j;
}

static bool host_bypasses_proxy(const char *host, uint16_t port) {
    if (!host || host[0] == '\0') return true;
    if (arcr_str_eq_nocase(host, "localhost") ||
        strcmp(host, "127.0.0.1") == 0 ||
        strncmp(host, "127.", 4) == 0 ||
        strcmp(host, "::1") == 0 ||
        strcmp(host, "[::1]") == 0) {
        return true;
    }

    const char *no_proxy = getenv("NO_PROXY");
    if (!no_proxy || no_proxy[0] == '\0') {
        no_proxy = getenv("no_proxy");
    }
    if (!no_proxy || no_proxy[0] == '\0') return false;

    size_t hlen = strlen(host);
    const char *p = no_proxy;
    while (*p != '\0') {
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
        if (*p == '\0') break;
        const char *start = p;
        while (*p != '\0' && *p != ',') ++p;
        const char *tok = start;
        size_t tlen = (size_t)(p - start);
        arcr_str_trim_view(&tok, &tlen);
        if (tlen == 0u) continue;
        if (tlen == 1u && tok[0] == '*') return true;

        /* Optional :port suffix on NO_PROXY entry */
        const char *tcolon = memchr(tok, ':', tlen);
        if (tcolon && tok[0] != '[') {
            int32_t np_port = 0;
            size_t plen = tlen - (size_t)(tcolon - tok + 1);
            if (arcr_parse_i32(tcolon + 1, plen, &np_port) && np_port > 0) {
                if ((uint16_t)np_port != port) continue;
                tlen = (size_t)(tcolon - tok);
            }
        }
        if (tok[0] == '.') {
            ++tok;
            --tlen;
        }
        if (tlen == 0u) continue;
        if (hlen == tlen && arcr_str_eq_nocase_n(host, hlen, tok, tlen)) {
            return true;
        }
        if (hlen > tlen + 1u && host[hlen - tlen - 1u] == '.' &&
            arcr_str_eq_nocase_n(host + (hlen - tlen), tlen, tok, tlen)) {
            return true;
        }
    }
    return false;
}

static bool resolve_proxy_for_target(const char *target_host, uint16_t target_port,
                                     bool use_tls, ArcrProxyConfig *out_cfg) {
    memset(out_cfg, 0, sizeof(*out_cfg));
    if (host_bypasses_proxy(target_host, target_port)) return false;

    const char *env_val = NULL;
    if (use_tls) {
        env_val = getenv("HTTPS_PROXY");
        if (!env_val || env_val[0] == '\0') env_val = getenv("https_proxy");
    } else {
        env_val = getenv("HTTP_PROXY");
        if (!env_val || env_val[0] == '\0') env_val = getenv("http_proxy");
    }
    if (!env_val || env_val[0] == '\0') return false;

    const char *s = env_val;
    size_t slen = strlen(s);
    arcr_str_trim_view(&s, &slen);
    if (slen == 0u || slen >= 1024u) return false;

    char buf[1024];
    memcpy(buf, s, slen);
    buf[slen] = '\0';

    const char *rest = buf;
    const char *sep = strstr(buf, "://");
    uint16_t default_port = 8080u;
    if (sep) {
        size_t sch_len = (size_t)(sep - buf);
        if (!arcr_str_eq_nocase_n(buf, sch_len, "http", 4u)) {
            return false; /* Only HTTP/CONNECT proxies supported cleanly */
        }
        default_port = 80u;
        rest = sep + 3;
    }

    /* Strip trailing path if present */
    char *slash = strchr((char *)rest, '/');
    if (slash) *slash = '\0';

    /* Optional user:pass@ Basic credentials */
    const char *at = strrchr(rest, '@');
    if (at) {
        uint8_t raw_cred[256];
        size_t cred_len = percent_decode_inline(rest, (size_t)(at - rest),
                                                raw_cred, sizeof(raw_cred));
        char b64_cred[384];
        base64_encode_small(raw_cred, cred_len, b64_cred, sizeof(b64_cred));
        if (b64_cred[0] != '\0') {
            snprintf(out_cfg->auth_header, sizeof(out_cfg->auth_header),
                     "Proxy-Authorization: Basic %s\r\n", b64_cred);
        }
        rest = at + 1;
    }

    size_t hp_len = strlen(rest);
    if (hp_len == 0u) return false;

    if (rest[0] == '[') {
        const char *rb = strchr(rest, ']');
        if (!rb) return false;
        size_t hlen = (size_t)(rb - rest + 1);
        if (hlen >= sizeof(out_cfg->host)) return false;
        memcpy(out_cfg->host, rest, hlen);
        out_cfg->host[hlen] = '\0';
        out_cfg->port = default_port;
        if (rb[1] == ':') {
            int32_t p = 0;
            if (arcr_parse_i32(rb + 2, strlen(rb + 2), &p) && p > 0 && p <= 65535) {
                out_cfg->port = (uint16_t)p;
            }
        }
    } else {
        const char *colon = strchr(rest, ':');
        if (colon) {
            size_t hlen = (size_t)(colon - rest);
            if (hlen == 0u || hlen >= sizeof(out_cfg->host)) return false;
            memcpy(out_cfg->host, rest, hlen);
            out_cfg->host[hlen] = '\0';
            int32_t p = 0;
            if (arcr_parse_i32(colon + 1, strlen(colon + 1), &p) && p > 0 && p <= 65535) {
                out_cfg->port = (uint16_t)p;
            } else {
                return false;
            }
        } else {
            if (hp_len >= sizeof(out_cfg->host)) return false;
            arcr_str_copy(out_cfg->host, sizeof(out_cfg->host), rest);
            out_cfg->port = default_port;
        }
    }

    out_cfg->enabled = true;
    return true;
}

/* =========================================================================
 * 6. Heap-Allocated TCP + TLS 1.3 Stream & 8-Slot Keep-Alive Connection Pool
 * ========================================================================= */
typedef struct ArcrNetStream {
    bool                is_tls;
    bool                via_http_proxy; /* Plain HTTP via HTTP_PROXY uses absolute-form URI */
    bool                was_cancelled;
    const volatile int *cancel_flag;
    char                proxy_auth_hdr[512];
    ArcrSecurityState   security;
    char                detail[256];
    /* Unread pushback buffer for incremental HTTP/1.1 framing across 1xx & Keep-Alive */
    uint8_t             pre_buf[ARCR_NET_PRE_BUF_CAP];
    size_t              pre_pos;
    size_t              pre_len;
#if defined(_WIN32)
    SOCKET            sock;
    HMODULE           secur32_mod;
    CredHandle        cred_handle;
    CtxtHandle        ctxt_handle;
    bool              has_cred;
    bool              has_ctxt;
    char              target_host[256];
    SecPkgContext_StreamSizes stream_sizes;
    uint8_t          *enc_buf;   /* Dynamically growable: 64 KB -> up to 256 KB */
    size_t            enc_cap;
    size_t            enc_len;
    uint8_t          *dec_stash; /* Heap-allocated 64 KB plaintext stash */
    size_t            dec_cap;
    size_t            dec_len;
    size_t            dec_pos;
#else
    int               fd;
#  if defined(ARCR_HAVE_OPENSSL)
    SSL              *ssl;       /* Backed by shared global SSL_CTX* */
#  endif
#endif
} ArcrNetStream;

static void stream_pushback(ArcrNetStream *st, const uint8_t *bytes, size_t len) {
    if (!st || !bytes || len == 0u) return;
    size_t rem = (st->pre_len > st->pre_pos) ? (st->pre_len - st->pre_pos) : 0u;
    size_t cap = sizeof(st->pre_buf);
    if (len >= cap) {
        memcpy(st->pre_buf, bytes, cap);
        st->pre_pos = 0u;
        st->pre_len = cap;
        return;
    }
    if (len + rem > cap) {
        rem = cap - len;
    }
    if (rem > 0u) {
        memmove(st->pre_buf + len, st->pre_buf + st->pre_pos, rem);
    }
    memcpy(st->pre_buf, bytes, len);
    st->pre_pos = 0u;
    st->pre_len = len + rem;
}

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

static SOCKET win32_connect_with_timeout(const struct sockaddr *sa, int sa_len,
                                         const volatile int *cancel_flag, bool *out_cancelled) {
    if (cancel_flag && *cancel_flag != 0) {
        if (out_cancelled) *out_cancelled = true;
        return INVALID_SOCKET;
    }
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
        int elapsed_ms = 0;
        bool ready = false;
        while (elapsed_ms < ARCR_NET_CONNECT_TIMEOUT_MS) {
            if (cancel_flag && *cancel_flag != 0) {
                if (out_cancelled) *out_cancelled = true;
                closesocket(s);
                return INVALID_SOCKET;
            }
            fd_set wfds, efds;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            FD_SET(s, &wfds);
            FD_SET(s, &efds);
            struct timeval tv;
            tv.tv_sec  = 0;
            tv.tv_usec = 50000; /* 50ms cancellable slice */
            int sel = select(0, NULL, &wfds, &efds, &tv);
            if (sel < 0 || FD_ISSET(s, &efds)) {
                closesocket(s);
                return INVALID_SOCKET;
            }
            if (sel > 0 && FD_ISSET(s, &wfds)) {
                ready = true;
                break;
            }
            elapsed_ms += 50;
        }
        if (!ready) {
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
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, (int)sizeof(nodelay));
    DWORD io_ms = (DWORD)ARCR_NET_IO_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&io_ms, (int)sizeof(io_ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&io_ms, (int)sizeof(io_ms));
    return s;
}

static SOCKET win32_tcp_connect(const char *host, uint16_t port,
                                const volatile int *cancel_flag, bool *out_cancelled) {
    if (!host || !ensure_winsock_initialized()) return INVALID_SOCKET;

    char clean_host[256];
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
                connected = win32_connect_with_timeout(ai->ai_addr, (int)ai->ai_addrlen,
                                                       cancel_flag, out_cancelled);
                if (connected != INVALID_SOCKET || (out_cancelled && *out_cancelled)) break;
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
        return win32_connect_with_timeout((const struct sockaddr *)&sin4, (int)sizeof(sin4),
                                          cancel_flag, out_cancelled);
    }

    struct in_addr addrs[8];
    int addr_count = 0;
    net_mutex_lock(&g_dns_mtx);
    struct hostent *he = gethostbyname(host);
    if (he && he->h_addr_list) {
        for (int i = 0; he->h_addr_list[i] != NULL && addr_count < 8; ++i) {
            memcpy(&addrs[addr_count++], he->h_addr_list[i], sizeof(struct in_addr));
        }
    }
    net_mutex_unlock(&g_dns_mtx);

    for (int i = 0; i < addr_count; ++i) {
        sin4.sin_addr = addrs[i];
        SOCKET s = win32_connect_with_timeout((const struct sockaddr *)&sin4, (int)sizeof(sin4),
                                              cancel_flag, out_cancelled);
        if (s != INVALID_SOCKET || (out_cancelled && *out_cancelled)) return s;
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

static bool win32_proxy_connect_tunnel(SOCKET sock, const char *target_host,
                                       uint16_t target_port, const char *proxy_auth_hdr) {
    char req[1024];
    int n = snprintf(req, sizeof(req),
                     "CONNECT %s:%u HTTP/1.1\r\n"
                     "Host: %s:%u\r\n"
                     "%s"
                     "Proxy-Connection: keep-alive\r\n\r\n",
                     target_host, (unsigned)target_port,
                     target_host, (unsigned)target_port,
                     proxy_auth_hdr ? proxy_auth_hdr : "");
    if (n <= 0 || (size_t)n >= sizeof(req)) return false;
    if (!win32_send_all(sock, (const uint8_t *)req, (size_t)n)) return false;

    char resp[4096];
    size_t rlen = 0u;
    while (rlen + 1u < sizeof(resp)) {
        int r = recv(sock, resp + rlen, 1, 0);
        if (r <= 0) return false;
        rlen += (size_t)r;
        if (rlen >= 4u &&
            resp[rlen - 4u] == '\r' && resp[rlen - 3u] == '\n' &&
            resp[rlen - 2u] == '\r' && resp[rlen - 1u] == '\n') {
            break;
        }
        if (rlen >= 2u && resp[rlen - 2u] == '\n' && resp[rlen - 1u] == '\n') {
            break;
        }
    }
    resp[rlen] = '\0';
    if (!arcr_str_starts_with_nocase(resp, "HTTP/")) return false;
    const char *sp = strchr(resp, ' ');
    if (!sp) return false;
    while (*sp == ' ') ++sp;
    int32_t code = 0;
    if (!arcr_parse_i32(sp, 3u, &code)) return false;
    return (code >= 200 && code < 300);
}

static bool win32_grow_enc_buf(ArcrNetStream *st) {
    if (st->enc_cap >= ARCR_NET_MAX_ENC_CAP) return false;
    size_t next_cap = (st->enc_cap == 0u) ? ARCR_NET_INIT_ENC_CAP : (st->enc_cap * 2u);
    if (next_cap > ARCR_NET_MAX_ENC_CAP) next_cap = ARCR_NET_MAX_ENC_CAP;
    uint8_t *grown = (uint8_t *)realloc(st->enc_buf, next_cap);
    if (!grown) return false;
    st->enc_buf = grown;
    st->enc_cap = next_cap;
    return true;
}

static bool win32_schannel_handshake_loop(ArcrNetStream *st, const ArcrSspiTable *sspi,
                                          SECURITY_STATUS initial_ss, bool call_isc_first) {
    ULONG req_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                      ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR |
                      ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
    ULONG ret_flags = 0UL;
    TimeStamp expiry;
    SECURITY_STATUS ss = initial_ss;
    bool skip_recv_once = call_isc_first;

    while (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_INCOMPLETE_MESSAGE) {
        if (st->cancel_flag && *st->cancel_flag != 0) {
            st->was_cancelled = true;
            return false;
        }
        if (!skip_recv_once && (st->enc_len == 0u || ss == SEC_E_INCOMPLETE_MESSAGE)) {
            if (st->enc_len >= st->enc_cap) {
                if (!win32_grow_enc_buf(st)) return false;
            }
            int r = recv(st->sock, (char *)(st->enc_buf + st->enc_len),
                         (int)(st->enc_cap - st->enc_len), 0);
            if (r <= 0) return false;
            st->enc_len += (size_t)r;
        }
        skip_recv_once = false;

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

        SecBuffer out_sbuf;
        out_sbuf.pvBuffer   = NULL;
        out_sbuf.cbBuffer   = 0UL;
        out_sbuf.BufferType = SECBUFFER_TOKEN;
        SecBufferDesc out_desc;
        out_desc.ulVersion = SECBUFFER_VERSION;
        out_desc.cBuffers  = 1UL;
        out_desc.pBuffers  = &out_sbuf;

        ss = sspi->InitializeSecurityContextA_fn(
            &st->cred_handle, &st->ctxt_handle, (SEC_CHAR *)st->target_host,
            req_flags, 0UL, 0UL, &in_desc, 0UL, NULL, &out_desc, &ret_flags, &expiry);

        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            continue;
        }
        if (out_sbuf.cbBuffer > 0UL && out_sbuf.pvBuffer) {
            bool sent = win32_send_all(st->sock, (const uint8_t *)out_sbuf.pvBuffer,
                                       out_sbuf.cbBuffer);
            sspi->FreeContextBuffer_fn(out_sbuf.pvBuffer);
            if (!sent) return false;
        }
        if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer > 0UL) {
            size_t extra = (size_t)in_bufs[1].cbBuffer;
            if (extra <= st->enc_len) {
                memmove(st->enc_buf, st->enc_buf + (st->enc_len - extra), extra);
                st->enc_len = extra;
            } else {
                st->enc_len = 0u;
            }
        } else {
            st->enc_len = 0u;
        }

        if (ss == SEC_E_OK) {
            (void)sspi->QueryContextAttributesA_fn(&st->ctxt_handle, SECPKG_ATTR_STREAM_SIZES,
                                                   &st->stream_sizes);
            return true;
        }
        if (ss != SEC_I_CONTINUE_NEEDED) {
            return false;
        }
    }
    return (ss == SEC_E_OK);
}

static bool stream_is_socket_alive(ArcrNetStream *st) {
    if (!st || st->sock == INVALID_SOCKET) return false;
    u_long nonblock = 1UL;
    ioctlsocket(st->sock, FIONBIO, &nonblock);
    char probe = 0;
    int r = recv(st->sock, &probe, 1, MSG_PEEK);
    int err = (r < 0) ? WSAGetLastError() : 0;
    nonblock = 0UL;
    ioctlsocket(st->sock, FIONBIO, &nonblock);
    if (r == 0) return false; /* Peer closed TCP connection */
    if (r < 0 && err != WSAEWOULDBLOCK) return false;
    return true;
}

static void stream_destroy(ArcrNetStream *st) {
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
    free(st->enc_buf);
    free(st->dec_stash);
    free(st);
}

static ArcrNetStream *stream_connect_new(const char *host, uint16_t port, bool use_tls,
                                         const volatile int *cancel_flag, bool *out_cancelled,
                                         ArcrSecurityState *out_sec,
                                         char *out_detail, size_t detail_cap) {
    if (out_cancelled) *out_cancelled = false;
    if (cancel_flag && *cancel_flag != 0) {
        if (out_cancelled) *out_cancelled = true;
        arcr_str_copy(out_detail, detail_cap, "요청 취소됨");
        return NULL;
    }
    if (use_tls && arcr_platform_wall_epoch_sec() < ARCR_BUILD_MIN_EPOCH_SEC) {
        *out_sec = ARCR_SEC_CLOCK_SKEW_ERR;
        arcr_str_copy(out_detail, detail_cap,
                      "시스템 시계가 빌드 기준 시각보다 과거입니다 (CMOS 배터리·날짜 설정 확인 필요)");
        return NULL;
    }

    ArcrNetStream *st = (ArcrNetStream *)calloc(1u, sizeof(ArcrNetStream));
    if (!st) return NULL;
    st->sock        = INVALID_SOCKET;
    st->is_tls      = use_tls;
    st->cancel_flag = cancel_flag;

    ArcrProxyConfig proxy;
    bool has_proxy = resolve_proxy_for_target(host, port, use_tls, &proxy);
    const char *dial_host = has_proxy ? proxy.host : host;
    uint16_t    dial_port = has_proxy ? proxy.port : port;

    bool conn_cancelled = false;
    st->sock = win32_tcp_connect(dial_host, dial_port, cancel_flag, &conn_cancelled);
    if (st->sock == INVALID_SOCKET) {
        if (conn_cancelled && out_cancelled) *out_cancelled = true;
        *out_sec = use_tls ? ARCR_SEC_CERT_ERROR : ARCR_SEC_HTTP_INSECURE;
        arcr_str_copy(out_detail, detail_cap,
                      conn_cancelled ? "요청 취소됨" : "서버 TCP 연결 또는 DNS 조회 실패");
        stream_destroy(st);
        return NULL;
    }

    if (has_proxy && use_tls) {
        if (!win32_proxy_connect_tunnel(st->sock, host, port, proxy.auth_header)) {
            *out_sec = ARCR_SEC_CERT_ERROR;
            arcr_str_copy(out_detail, detail_cap, "HTTPS 프록시 CONNECT 터널 수립 실패");
            stream_destroy(st);
            return NULL;
        }
    } else if (has_proxy && !use_tls) {
        st->via_http_proxy = true;
        arcr_str_copy(st->proxy_auth_hdr, sizeof(st->proxy_auth_hdr), proxy.auth_header);
    }

    if (!use_tls) {
        st->security = ARCR_SEC_HTTP_INSECURE;
        arcr_str_copy(st->detail, sizeof(st->detail), "암호화되지 않은 HTTP/1.1 연결");
        *out_sec = st->security;
        arcr_str_copy(out_detail, detail_cap, st->detail);
        return st;
    }

    st->enc_buf   = (uint8_t *)malloc(ARCR_NET_INIT_ENC_CAP);
    st->enc_cap   = ARCR_NET_INIT_ENC_CAP;
    st->dec_stash = (uint8_t *)malloc(ARCR_NET_DEC_STASH_CAP);
    st->dec_cap   = ARCR_NET_DEC_STASH_CAP;
    if (!st->enc_buf || !st->dec_stash) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap, "TLS 버퍼 메모리 할당 실패");
        stream_destroy(st);
        return NULL;
    }

    st->secur32_mod = LoadLibraryA("secur32.dll");
    ArcrSspiTable sspi;
    if (!st->secur32_mod || !load_sspi_functions(st->secur32_mod, &sspi)) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap, "Windows SChannel (secur32.dll) 초기화 실패");
        stream_destroy(st);
        return NULL;
    }

    SCHANNEL_CRED sc_cred;
    memset(&sc_cred, 0, sizeof(sc_cred));
    sc_cred.dwVersion = SCHANNEL_CRED_VERSION;

    TimeStamp expiry;
    SECURITY_STATUS ss = (SECURITY_STATUS)-1;
    if (arcr_platform_os_version() >= 0x0600u) {
        /* Vista / Win7 / Win10 / Win11: grbitEnabledProtocols = 0 enables TLS 1.3 + TLS 1.2! */
        sc_cred.grbitEnabledProtocols = 0u;
        sc_cred.dwFlags = SCH_CRED_NO_DEFAULT_CREDS |
                          SCH_CRED_AUTO_CRED_VALIDATION |
                          SCH_USE_STRONG_CRYPTO;
        ss = sspi.AcquireCredentialsHandleA_fn(
            NULL, (SEC_CHAR *)UNISP_NAME_A, SECPKG_CRED_OUTBOUND,
            NULL, &sc_cred, NULL, NULL, &st->cred_handle, &expiry);
    }
    if (ss != SEC_E_OK) {
        /* Fallback 1: explicit TLS 1.2/1.1/1.0 client mask */
        sc_cred.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_AUTO_CRED_VALIDATION;
        sc_cred.grbitEnabledProtocols =
            SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_1_CLIENT | SP_PROT_TLS1_0_CLIENT;
        ss = sspi.AcquireCredentialsHandleA_fn(
            NULL, (SEC_CHAR *)UNISP_NAME_A, SECPKG_CRED_OUTBOUND,
            NULL, &sc_cred, NULL, NULL, &st->cred_handle, &expiry);
    }
    if (ss != SEC_E_OK) {
        /* Fallback 2: vanilla Windows 2000 SP4 SChannel */
        sc_cred.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_AUTO_CRED_VALIDATION;
        sc_cred.grbitEnabledProtocols = 0u;
        ss = sspi.AcquireCredentialsHandleA_fn(
            NULL, (SEC_CHAR *)UNISP_NAME_A, SECPKG_CRED_OUTBOUND,
            NULL, &sc_cred, NULL, NULL, &st->cred_handle, &expiry);
        if (ss != SEC_E_OK) {
            *out_sec = ARCR_SEC_CERT_ERROR;
            arcr_str_copy(out_detail, detail_cap, "SChannel 자격 증명 획득 실패");
            stream_destroy(st);
            return NULL;
        }
    }
    st->has_cred = true;

    /* Strip IPv6 brackets for SNI & X.509 target hostname verification */
    arcr_str_copy(st->target_host, sizeof(st->target_host), host ? host : "");
    if (st->target_host[0] == '[') {
        size_t clen = strlen(st->target_host);
        if (clen >= 2u && st->target_host[clen - 1u] == ']') {
            memmove(st->target_host, st->target_host + 1, clen - 2u);
            st->target_host[clen - 2u] = '\0';
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
        &st->cred_handle, NULL, (SEC_CHAR *)st->target_host, req_flags, 0UL, 0UL,
        NULL, 0UL, &st->ctxt_handle, &out_desc, &ret_flags, &expiry);
    if (ss != SEC_I_CONTINUE_NEEDED && ss != SEC_E_OK) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap, "TLS ClientHello 생성 실패");
        stream_destroy(st);
        return NULL;
    }
    st->has_ctxt = true;

    if (out_sbuf.cbBuffer > 0UL && out_sbuf.pvBuffer) {
        bool sent = win32_send_all(st->sock, (const uint8_t *)out_sbuf.pvBuffer, out_sbuf.cbBuffer);
        sspi.FreeContextBuffer_fn(out_sbuf.pvBuffer);
        if (!sent) {
            *out_sec = ARCR_SEC_CERT_ERROR;
            arcr_str_copy(out_detail, detail_cap, "TLS 핸드셰이크 전송 실패");
            stream_destroy(st);
            return NULL;
        }
    }

    if (ss != SEC_E_OK && !win32_schannel_handshake_loop(st, &sspi, ss, false)) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap,
                      "HTTPS 서버 인증서 검증 또는 TLS 1.2/1.3 프로토콜 협상 실패 (SChannel)");
        stream_destroy(st);
        return NULL;
    }

    if (sspi.QueryContextAttributesA_fn(&st->ctxt_handle, SECPKG_ATTR_STREAM_SIZES,
                                        &st->stream_sizes) != SEC_E_OK) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap, "SChannel 스트림 버퍼 크기 조회 실패");
        stream_destroy(st);
        return NULL;
    }

    st->security = ARCR_SEC_HTTPS_VERIFIED;
    arcr_str_copy(st->detail, sizeof(st->detail), "HTTPS 검증 완료 (Windows SChannel TLS)");
    *out_sec = st->security;
    arcr_str_copy(out_detail, detail_cap, st->detail);
    return st;
}

static bool stream_write_all(ArcrNetStream *st, const uint8_t *data, size_t len) {
    if (!st->is_tls) {
        return win32_send_all(st->sock, data, len);
    }
    ArcrSspiTable sspi;
    if (!load_sspi_functions(st->secur32_mod, &sspi)) return false;

    size_t max_hdr   = st->stream_sizes.cbHeader;
    size_t max_trl   = st->stream_sizes.cbTrailer;
    size_t max_plain = st->stream_sizes.cbMaximumMessage;
    if (max_plain == 0u) return false;
    if (max_plain > 16384u) max_plain = 16384u;

    size_t pkt_cap = max_hdr + max_plain + max_trl;
    uint8_t *packet = (uint8_t *)malloc(pkt_cap);
    if (!packet) return false;

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
            free(packet);
            return false;
        }
        size_t total_enc = (size_t)bufs[0].cbBuffer + (size_t)bufs[1].cbBuffer + (size_t)bufs[2].cbBuffer;
        if (!win32_send_all(st->sock, packet, total_enc)) {
            free(packet);
            return false;
        }
        off += chunk;
    }
    free(packet);
    return true;
}

static bool win32_wait_readable(ArcrNetStream *st) {
    if (!st) return false;
    if (st->cancel_flag && *st->cancel_flag != 0) {
        st->was_cancelled = true;
        return false;
    }
    if (!st->cancel_flag) return true;

    int elapsed_ms = 0;
    while (elapsed_ms < ARCR_NET_IO_TIMEOUT_MS) {
        if (*st->cancel_flag != 0) {
            st->was_cancelled = true;
            return false;
        }
        fd_set rfds, efds;
        FD_ZERO(&rfds);
        FD_ZERO(&efds);
        FD_SET(st->sock, &rfds);
        FD_SET(st->sock, &efds);
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 50000;
        int sel = select(0, &rfds, NULL, &efds, &tv);
        if (sel < 0) return false;
        if (sel > 0 && (FD_ISSET(st->sock, &rfds) || FD_ISSET(st->sock, &efds))) {
            return true;
        }
        elapsed_ms += 50;
    }
    return false;
}

static int stream_read_some(ArcrNetStream *st, uint8_t *out, size_t cap) {
    if (!st || !out || cap == 0u) return -1;
    if (st->cancel_flag && *st->cancel_flag != 0) {
        st->was_cancelled = true;
        return -1;
    }

    /* 1. Drain unread pushback bytes first */
    if (st->pre_pos < st->pre_len) {
        size_t avail = ARCR_MIN(cap, st->pre_len - st->pre_pos);
        memcpy(out, st->pre_buf + st->pre_pos, avail);
        st->pre_pos += avail;
        if (st->pre_pos == st->pre_len) {
            st->pre_pos = 0u;
            st->pre_len = 0u;
        }
        return (int)avail;
    }

    if (!st->is_tls) {
        if (!win32_wait_readable(st)) return -1;
        int r = recv(st->sock, (char *)out, (int)ARCR_MIN(cap, 65536u), 0);
        if (r < 0) return -1;
        return r; /* 0 on clean TCP EOF, >0 on data */
    }

    /* 2. Drain decrypted TLS plaintext stash */
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
        if (st->cancel_flag && *st->cancel_flag != 0) {
            st->was_cancelled = true;
            return -1;
        }
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
            if (ss == SEC_E_OK || ss == SEC_I_RENEGOTIATE) {
                SecBuffer *data_buf  = NULL;
                SecBuffer *extra_buf = NULL;
                for (int i = 0; i < 4; ++i) {
                    if (!data_buf && bufs[i].BufferType == SECBUFFER_DATA) data_buf = &bufs[i];
                    if (!extra_buf && bufs[i].BufferType == SECBUFFER_EXTRA) extra_buf = &bufs[i];
                }

                /* Copy decrypted SECBUFFER_DATA BEFORE memmoving enc_buf! */
                size_t plain_len = (data_buf && data_buf->pvBuffer) ? (size_t)data_buf->cbBuffer : 0u;
                size_t copy_now  = 0u;
                if (plain_len > 0u) {
                    copy_now = ARCR_MIN(cap, plain_len);
                    memcpy(out, data_buf->pvBuffer, copy_now);
                    if (plain_len > copy_now) {
                        size_t rem = ARCR_MIN(plain_len - copy_now, st->dec_cap);
                        memcpy(st->dec_stash, (const uint8_t *)data_buf->pvBuffer + copy_now, rem);
                        st->dec_pos = 0u;
                        st->dec_len = rem;
                    }
                }

                if (extra_buf && extra_buf->cbBuffer > 0UL) {
                    size_t ex = (size_t)extra_buf->cbBuffer;
                    if (ex <= st->enc_len) {
                        memmove(st->enc_buf, st->enc_buf + (st->enc_len - ex), ex);
                        st->enc_len = ex;
                    } else {
                        st->enc_len = 0u;
                    }
                } else {
                    st->enc_len = 0u;
                }

                /* TLS 1.3 Post-Handshake NewSessionTicket / KeyUpdate triggers SEC_I_RENEGOTIATE! */
                if (ss == SEC_I_RENEGOTIATE) {
                    if (!win32_schannel_handshake_loop(st, &sspi, SEC_I_CONTINUE_NEEDED, true)) {
                        return (copy_now > 0u) ? (int)copy_now : -1;
                    }
                }

                if (copy_now > 0u) {
                    return (int)copy_now;
                }
                continue;
            }
            if (ss != SEC_E_INCOMPLETE_MESSAGE) {
                return -1;
            }
        }

        if (st->enc_len >= st->enc_cap) {
            if (!win32_grow_enc_buf(st)) return -1;
        }
        if (!win32_wait_readable(st)) return -1;
        int r = recv(st->sock, (char *)(st->enc_buf + st->enc_len),
                     (int)(st->enc_cap - st->enc_len), 0);
        if (r < 0) return -1; /* Socket timeout or connection reset */
        if (r == 0) return 0; /* Clean TCP EOF */
        st->enc_len += (size_t)r;
    }
}

#else /* POSIX Sockets + Shared Cached OpenSSL SSL_CTX */

#  if defined(ARCR_HAVE_OPENSSL)
static SSL_CTX *g_shared_ssl_ctx      = NULL;
static char     g_loaded_ca_file[1024] = "";
static bool     g_ssl_ctx_inited      = false;

static SSL_CTX *get_or_init_shared_ssl_ctx(void) {
    const char *ca_env = getenv("ARCR_SSL_CA_FILE");
    if (!ca_env || ca_env[0] == '\0') {
        ca_env = getenv("SSL_CERT_FILE");
    }
    if (!ca_env) ca_env = "";

    net_mutex_lock(&g_ssl_mtx);
    if (g_shared_ssl_ctx && (!g_ssl_ctx_inited || strcmp(g_loaded_ca_file, ca_env) != 0)) {
        SSL_CTX_free(g_shared_ssl_ctx);
        g_shared_ssl_ctx = NULL;
    }
    if (!g_shared_ssl_ctx) {
        g_shared_ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (g_shared_ssl_ctx) {
            (void)SSL_CTX_set_default_verify_paths(g_shared_ssl_ctx);
            if (ca_env[0] != '\0') {
                (void)SSL_CTX_load_verify_locations(g_shared_ssl_ctx, ca_env, NULL);
            }
            SSL_CTX_set_verify(g_shared_ssl_ctx, SSL_VERIFY_NONE, NULL);
#    ifdef TLS1_2_VERSION
            (void)SSL_CTX_set_min_proto_version(g_shared_ssl_ctx, TLS1_2_VERSION);
#    endif
            /* Advertise ALPN "http/1.1" so TLS 1.3 servers never negotiate h2 over HTTP/1.1 parser */
            static const unsigned char kAlpnHttp11[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
            (void)SSL_CTX_set_alpn_protos(g_shared_ssl_ctx, kAlpnHttp11, sizeof(kAlpnHttp11));
            arcr_str_copy(g_loaded_ca_file, sizeof(g_loaded_ca_file), ca_env);
            g_ssl_ctx_inited = true;
        }
    }
    SSL_CTX *ctx = g_shared_ssl_ctx;
    net_mutex_unlock(&g_ssl_mtx);
    return ctx;
}
#  endif

static int posix_connect_with_timeout(const struct sockaddr *sa, socklen_t sa_len,
                                      const volatile int *cancel_flag, bool *out_cancelled) {
    if (cancel_flag && *cancel_flag != 0) {
        if (out_cancelled) *out_cancelled = true;
        return -1;
    }
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
        int elapsed_ms = 0;
        bool ready = false;
        while (elapsed_ms < ARCR_NET_CONNECT_TIMEOUT_MS) {
            if (cancel_flag && *cancel_flag != 0) {
                if (out_cancelled) *out_cancelled = true;
                close(fd);
                return -1;
            }
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv;
            tv.tv_sec  = 0;
            tv.tv_usec = 50000; /* 50ms cancellable slice */
            int sel = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (sel < 0) {
                if (errno == EINTR) continue;
                close(fd);
                return -1;
            }
            if (sel > 0 && FD_ISSET(fd, &wfds)) {
                ready = true;
                break;
            }
            elapsed_ms += 50;
        }
        if (!ready) {
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

static int posix_tcp_connect(const char *host, uint16_t port,
                             const volatile int *cancel_flag, bool *out_cancelled) {
    if (!host || host[0] == '\0') return -1;
    char clean_host[256];
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
        fd = posix_connect_with_timeout(ai->ai_addr, ai->ai_addrlen, cancel_flag, out_cancelled);
        if (fd >= 0 || (out_cancelled && *out_cancelled)) break;
    }
    freeaddrinfo(res);
    return fd;
}

static bool posix_send_all_raw(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0u;
    while (off < len) {
        size_t chunk = ARCR_MIN(len - off, (size_t)65536u);
#  ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, buf + off, chunk, MSG_NOSIGNAL);
#  else
        ssize_t n = send(fd, buf + off, chunk, 0);
#  endif
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        off += (size_t)n;
    }
    return true;
}

static bool posix_proxy_connect_tunnel(int fd, const char *target_host,
                                       uint16_t target_port, const char *proxy_auth_hdr) {
    char req[1024];
    int n = snprintf(req, sizeof(req),
                     "CONNECT %s:%u HTTP/1.1\r\n"
                     "Host: %s:%u\r\n"
                     "%s"
                     "Proxy-Connection: keep-alive\r\n\r\n",
                     target_host, (unsigned)target_port,
                     target_host, (unsigned)target_port,
                     proxy_auth_hdr ? proxy_auth_hdr : "");
    if (n <= 0 || (size_t)n >= sizeof(req)) return false;
    if (!posix_send_all_raw(fd, (const uint8_t *)req, (size_t)n)) return false;

    char resp[4096];
    size_t rlen = 0u;
    while (rlen + 1u < sizeof(resp)) {
        ssize_t r = recv(fd, resp + rlen, 1u, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        rlen += (size_t)r;
        if (rlen >= 4u &&
            resp[rlen - 4u] == '\r' && resp[rlen - 3u] == '\n' &&
            resp[rlen - 2u] == '\r' && resp[rlen - 1u] == '\n') {
            break;
        }
        if (rlen >= 2u && resp[rlen - 2u] == '\n' && resp[rlen - 1u] == '\n') {
            break;
        }
    }
    resp[rlen] = '\0';
    if (!arcr_str_starts_with_nocase(resp, "HTTP/")) return false;
    const char *sp = strchr(resp, ' ');
    if (!sp) return false;
    while (*sp == ' ') ++sp;
    int32_t code = 0;
    if (!arcr_parse_i32(sp, 3u, &code)) return false;
    return (code >= 200 && code < 300);
}

static bool stream_is_socket_alive(ArcrNetStream *st) {
    if (!st || st->fd < 0) return false;
    char probe = 0;
#  ifdef MSG_DONTWAIT
    ssize_t r = recv(st->fd, &probe, 1u, MSG_PEEK | MSG_DONTWAIT);
    if (r == 0) return false;
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
    return true;
#  else
    int flags = fcntl(st->fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(st->fd, F_SETFL, flags | O_NONBLOCK);
    ssize_t r = recv(st->fd, &probe, 1u, MSG_PEEK);
    int saved_err = errno;
    if (flags >= 0) (void)fcntl(st->fd, F_SETFL, flags);
    if (r == 0) return false;
    if (r < 0 && saved_err != EAGAIN && saved_err != EWOULDBLOCK && saved_err != EINTR) return false;
    return true;
#  endif
}

static void stream_destroy(ArcrNetStream *st) {
    if (!st) return;
#  if defined(ARCR_HAVE_OPENSSL)
    if (st->ssl) {
        SSL_shutdown(st->ssl);
        SSL_free(st->ssl);
        st->ssl = NULL;
    }
#  endif
    if (st->fd >= 0) {
        close(st->fd);
        st->fd = -1;
    }
    free(st);
}

static ArcrNetStream *stream_connect_new(const char *host, uint16_t port, bool use_tls,
                                         const volatile int *cancel_flag, bool *out_cancelled,
                                         ArcrSecurityState *out_sec,
                                         char *out_detail, size_t detail_cap) {
    if (out_cancelled) *out_cancelled = false;
    if (cancel_flag && *cancel_flag != 0) {
        if (out_cancelled) *out_cancelled = true;
        arcr_str_copy(out_detail, detail_cap, "요청 취소됨");
        return NULL;
    }
    if (use_tls && arcr_platform_wall_epoch_sec() < ARCR_BUILD_MIN_EPOCH_SEC) {
        *out_sec = ARCR_SEC_CLOCK_SKEW_ERR;
        arcr_str_copy(out_detail, detail_cap,
                      "시스템 시계가 빌드 기준 시각보다 과거입니다 (CMOS 배터리·날짜 설정 확인 필요)");
        return NULL;
    }

#  if !defined(ARCR_HAVE_OPENSSL)
    if (use_tls) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap,
                      "POSIX 빌드에서 OpenSSL TLS 백엔드가 비활성화되어 있습니다");
        return NULL;
    }
#  endif

    ArcrNetStream *st = (ArcrNetStream *)calloc(1u, sizeof(ArcrNetStream));
    if (!st) return NULL;
    st->fd          = -1;
    st->is_tls      = use_tls;
    st->cancel_flag = cancel_flag;

    ArcrProxyConfig proxy;
    bool has_proxy = resolve_proxy_for_target(host, port, use_tls, &proxy);
    const char *dial_host = has_proxy ? proxy.host : host;
    uint16_t    dial_port = has_proxy ? proxy.port : port;

    bool conn_cancelled = false;
    st->fd = posix_tcp_connect(dial_host, dial_port, cancel_flag, &conn_cancelled);
    if (st->fd < 0) {
        if (conn_cancelled && out_cancelled) *out_cancelled = true;
        *out_sec = use_tls ? ARCR_SEC_CERT_ERROR : ARCR_SEC_HTTP_INSECURE;
        arcr_str_copy(out_detail, detail_cap,
                      conn_cancelled ? "요청 취소됨" : "서버 TCP 연결 또는 DNS 조회 실패");
        stream_destroy(st);
        return NULL;
    }

    if (has_proxy && use_tls) {
        if (!posix_proxy_connect_tunnel(st->fd, host, port, proxy.auth_header)) {
            *out_sec = ARCR_SEC_CERT_ERROR;
            arcr_str_copy(out_detail, detail_cap, "HTTPS 프록시 CONNECT 터널 수립 실패");
            stream_destroy(st);
            return NULL;
        }
    } else if (has_proxy && !use_tls) {
        st->via_http_proxy = true;
        arcr_str_copy(st->proxy_auth_hdr, sizeof(st->proxy_auth_hdr), proxy.auth_header);
    }

    if (!use_tls) {
        st->security = ARCR_SEC_HTTP_INSECURE;
        arcr_str_copy(st->detail, sizeof(st->detail), "암호화되지 않은 HTTP/1.1 연결");
        *out_sec = st->security;
        arcr_str_copy(out_detail, detail_cap, st->detail);
        return st;
    }

#  if defined(ARCR_HAVE_OPENSSL)
    SSL_CTX *shared_ctx = get_or_init_shared_ssl_ctx();
    if (!shared_ctx) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap, "OpenSSL 공유 SSL_CTX 초기화 실패");
        stream_destroy(st);
        return NULL;
    }

    st->ssl = SSL_new(shared_ctx);
    if (!st->ssl) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap, "OpenSSL 세션 할당 실패");
        stream_destroy(st);
        return NULL;
    }

    /* Strip IPv6 brackets for SNI & X.509 hostname/IP verification */
    char clean_host[256];
    arcr_str_copy(clean_host, sizeof(clean_host), host);
    if (clean_host[0] == '[') {
        size_t clen = strlen(clean_host);
        if (clen >= 2u && clean_host[clen - 1u] == ']') {
            memmove(clean_host, clean_host + 1, clen - 2u);
            clean_host[clen - 2u] = '\0';
        }
    }

    X509_VERIFY_PARAM *vparam = SSL_get0_param(st->ssl);
    bool is_ip_literal = false;
    if (vparam && X509_VERIFY_PARAM_set1_ip_asc(vparam, clean_host) == 1) {
        is_ip_literal = true;
    } else {
        (void)SSL_set_tlsext_host_name(st->ssl, clean_host);
        if (vparam) {
            (void)X509_VERIFY_PARAM_set1_host(vparam, clean_host, 0u);
        }
    }
    (void)is_ip_literal;
    (void)SSL_set_fd(st->ssl, st->fd);

    if (SSL_connect(st->ssl) != 1) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap, "TLS 핸드셰이크 협상 실패");
        stream_destroy(st);
        return NULL;
    }

    X509 *peer = SSL_get_peer_certificate(st->ssl);
    if (!peer) {
        *out_sec = ARCR_SEC_CERT_ERROR;
        arcr_str_copy(out_detail, detail_cap, "서버 TLS 인증서가 제공되지 않았습니다");
        stream_destroy(st);
        return NULL;
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
        stream_destroy(st);
        return NULL;
    }

    st->security = ARCR_SEC_HTTPS_VERIFIED;
    snprintf(st->detail, sizeof(st->detail), "HTTPS 검증 완료 (%s)", SSL_get_version(st->ssl));
    *out_sec = st->security;
    arcr_str_copy(out_detail, detail_cap, st->detail);
    return st;
#  else
    stream_destroy(st);
    return NULL;
#  endif
}

static bool stream_write_all(ArcrNetStream *st, const uint8_t *data, size_t len) {
    if (!st->is_tls) {
        return posix_send_all_raw(st->fd, data, len);
    }
#  if defined(ARCR_HAVE_OPENSSL)
    size_t off = 0u;
    while (off < len) {
        int chunk = (len - off > 65536u) ? 65536 : (int)(len - off);
        int n = SSL_write(st->ssl, data + off, chunk);
        if (n <= 0) {
            int err = SSL_get_error(st->ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
            return false;
        }
        off += (size_t)n;
    }
    return true;
#  else
    (void)data;
    (void)len;
    return false;
#  endif
}

static bool posix_wait_readable(ArcrNetStream *st) {
    if (!st) return false;
    if (st->cancel_flag && *st->cancel_flag != 0) {
        st->was_cancelled = true;
        return false;
    }
    if (!st->cancel_flag) return true;
    if (st->fd < 0 || st->fd >= FD_SETSIZE) return true;

    int elapsed_ms = 0;
    while (elapsed_ms < ARCR_NET_IO_TIMEOUT_MS) {
        if (*st->cancel_flag != 0) {
            st->was_cancelled = true;
            return false;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(st->fd, &rfds);
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 50000; /* 50ms cancellable slice */
        int sel = select(st->fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (sel > 0 && FD_ISSET(st->fd, &rfds)) {
            return true;
        }
        elapsed_ms += 50;
    }
    return false;
}

static int stream_read_some(ArcrNetStream *st, uint8_t *out, size_t cap) {
    if (!st || !out || cap == 0u) return -1;
    if (st->cancel_flag && *st->cancel_flag != 0) {
        st->was_cancelled = true;
        return -1;
    }

    if (st->pre_pos < st->pre_len) {
        size_t avail = ARCR_MIN(cap, st->pre_len - st->pre_pos);
        memcpy(out, st->pre_buf + st->pre_pos, avail);
        st->pre_pos += avail;
        if (st->pre_pos == st->pre_len) {
            st->pre_pos = 0u;
            st->pre_len = 0u;
        }
        return (int)avail;
    }

    int chunk = (cap > 65536u) ? 65536 : (int)cap;
    for (;;) {
        if (st->cancel_flag && *st->cancel_flag != 0) {
            st->was_cancelled = true;
            return -1;
        }
#  if defined(ARCR_HAVE_OPENSSL)
        if (st->is_tls) {
            if (SSL_pending(st->ssl) == 0 && !posix_wait_readable(st)) {
                return -1;
            }
            int n = SSL_read(st->ssl, out, chunk);
            if (n > 0) return n;
            int err = SSL_get_error(st->ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
            if (err == SSL_ERROR_ZERO_RETURN) return 0;
            if (err == SSL_ERROR_SYSCALL && n == 0 && errno == 0) return 0;
            return -1;
        }
#  endif
        if (!posix_wait_readable(st)) {
            return -1;
        }
        ssize_t n = recv(st->fd, out, (size_t)chunk, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        return (int)n; /* 0 on clean EOF, >0 on bytes */
    }
}
#endif

/* -------------------------------------------------------------------------
 * 8-Slot Thread-Safe Keep-Alive Pool (Max Idle 30s)
 * ------------------------------------------------------------------------- */
typedef struct ArcrKeepAliveSlot {
    bool           in_use;
    bool           use_tls;
    uint16_t       port;
    char           host[256];
    uint64_t       idle_since_usec;
    ArcrNetStream *stream;
} ArcrKeepAliveSlot;

static ArcrKeepAliveSlot g_ka_pool[ARCR_NET_KA_POOL_SLOTS];

static void ka_pool_purge_all(void) {
    ArcrNetStream *to_close[ARCR_NET_KA_POOL_SLOTS];
    size_t close_cnt = 0u;

    net_mutex_lock(&g_pool_mtx);
    for (size_t i = 0u; i < ARCR_NET_KA_POOL_SLOTS; ++i) {
        if (g_ka_pool[i].in_use && g_ka_pool[i].stream) {
            to_close[close_cnt++] = g_ka_pool[i].stream;
        }
        memset(&g_ka_pool[i], 0, sizeof(g_ka_pool[i]));
    }
    net_mutex_unlock(&g_pool_mtx);

    for (size_t i = 0u; i < close_cnt; ++i) {
        stream_destroy(to_close[i]);
    }
}

static ArcrNetStream *ka_pool_acquire(const char *host, uint16_t port, bool use_tls) {
    if (!host || host[0] == '\0') return NULL;
    uint64_t now = arcr_platform_monotonic_usec();
    ArcrNetStream *found = NULL;
    ArcrNetStream *expired[ARCR_NET_KA_POOL_SLOTS];
    size_t exp_cnt = 0u;

    net_mutex_lock(&g_pool_mtx);
    for (size_t i = 0u; i < ARCR_NET_KA_POOL_SLOTS; ++i) {
        ArcrKeepAliveSlot *sl = &g_ka_pool[i];
        if (!sl->in_use) continue;
        if (now >= sl->idle_since_usec &&
            (now - sl->idle_since_usec) >= ARCR_NET_KA_MAX_IDLE_USEC) {
            expired[exp_cnt++] = sl->stream;
            memset(sl, 0, sizeof(*sl));
            continue;
        }
        if (!found && sl->use_tls == use_tls && sl->port == port &&
            arcr_str_eq_nocase(sl->host, host)) {
            found = sl->stream;
            memset(sl, 0, sizeof(*sl));
        }
    }
    net_mutex_unlock(&g_pool_mtx);

    for (size_t i = 0u; i < exp_cnt; ++i) {
        stream_destroy(expired[i]);
    }

    if (found && !stream_is_socket_alive(found)) {
        stream_destroy(found);
        return NULL;
    }
    if (found) {
        found->cancel_flag   = NULL;
        found->was_cancelled = false;
    }
    return found;
}

static void ka_pool_release(ArcrNetStream *st, const char *host, uint16_t port, bool use_tls) {
    if (!st || st->was_cancelled || !host || host[0] == '\0' || strlen(host) >= sizeof(g_ka_pool[0].host)) {
        stream_destroy(st);
        return;
    }
    st->cancel_flag = NULL;
    /* Do not pool streams with leftover unconsumed pushback bytes */
    if (st->pre_pos < st->pre_len) {
        stream_destroy(st);
        return;
    }

    uint64_t now = arcr_platform_monotonic_usec();
    ArcrNetStream *evicted = NULL;

    net_mutex_lock(&g_pool_mtx);
    size_t target = ARCR_NET_KA_POOL_SLOTS;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0u; i < ARCR_NET_KA_POOL_SLOTS; ++i) {
        if (!g_ka_pool[i].in_use) {
            target = i;
            break;
        }
        if (g_ka_pool[i].idle_since_usec < oldest) {
            oldest = g_ka_pool[i].idle_since_usec;
            target = i;
        }
    }
    if (g_ka_pool[target].in_use) {
        evicted = g_ka_pool[target].stream;
    }
    g_ka_pool[target].in_use          = true;
    g_ka_pool[target].use_tls         = use_tls;
    g_ka_pool[target].port            = port;
    arcr_str_copy(g_ka_pool[target].host, sizeof(g_ka_pool[target].host), host);
    g_ka_pool[target].idle_since_usec = now;
    g_ka_pool[target].stream          = st;
    net_mutex_unlock(&g_pool_mtx);

    if (evicted) {
        stream_destroy(evicted);
    }
}

/* =========================================================================
 * 7. True Incremental RFC 9112 HTTP/1.1 Stream Framing & Chunked Decoder
 * ========================================================================= */
static bool parse_u64_strict(const char *s, size_t len, uint64_t *out_val) {
    if (!s || len == 0u || !out_val) return false;
    arcr_str_trim_view(&s, &len);
    if (len == 0u) return false;
    uint64_t v = 0u;
    for (size_t i = 0u; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        uint64_t digit = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - digit) / 10u) return false;
        v = v * 10u + digit;
    }
    *out_val = v;
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
        if (arcr_str_eq_nocase_n(hay + i, nlen, needle, nlen)) {
            bool left_ok  = (i == 0u) || (!is_alpha_ascii(hay[i - 1u]) && hay[i - 1u] != '-');
            bool right_ok = (i + nlen == hlen) ||
                            (!is_alpha_ascii(hay[i + nlen]) && hay[i + nlen] != '-');
            if (left_ok && right_ok) return true;
        }
    }
    return false;
}

/* Reads exactly `exact_len` bytes, stopping immediately once all bytes arrive */
static bool stream_read_exact(ArcrNetStream *st, uint8_t *dst, size_t exact_len) {
    size_t got = 0u;
    while (got < exact_len) {
        int n = stream_read_some(st, dst + got, exact_len - got);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

/* Reads a single CRLF/LF-terminated line using buffered reads + pushback */
static bool stream_read_crlf_line(ArcrNetStream *st, char *line_out, size_t line_cap,
                                  size_t *out_line_len) {
    if (!st || !line_out || line_cap < 2u) return false;
    size_t pos = 0u;
    uint8_t chunk[4096];

    for (;;) {
        int n = stream_read_some(st, chunk, sizeof(chunk));
        if (n <= 0) return false;

        for (int i = 0; i < n; ++i) {
            if (chunk[i] == '\n') {
                size_t unread = (size_t)(n - (i + 1));
                if (unread > 0u) {
                    stream_pushback(st, chunk + i + 1, unread);
                }
                if (pos > 0u && line_out[pos - 1u] == '\r') {
                    --pos;
                }
                line_out[pos] = '\0';
                if (out_line_len) *out_line_len = pos;
                return true;
            }
            if (pos + 1u >= line_cap) return false;
            line_out[pos++] = (char)chunk[i];
        }
    }
}

/* Incrementally reads HTTP response headers up to \r\n\r\n (max 64 KB) and pushes back body prefix */
static bool stream_read_header_block(ArcrNetStream *st, char **out_hdr, size_t *out_hdr_len,
                                     bool *out_got_any_byte) {
    *out_hdr = NULL;
    *out_hdr_len = 0u;
    if (out_got_any_byte) *out_got_any_byte = false;

    size_t cap = 4096u;
    size_t len = 0u;
    uint8_t *buf = (uint8_t *)malloc(cap + 1u);
    if (!buf) return false;

    for (;;) {
        size_t sep_len = 0u;
        size_t hdr_end = find_header_end(buf, len, &sep_len);
        if (sep_len > 0u) {
            size_t total_hdr_wire = hdr_end + sep_len;
            if (len > total_hdr_wire) {
                stream_pushback(st, buf + total_hdr_wire, len - total_hdr_wire);
            }
            buf[hdr_end] = '\0';
            *out_hdr     = (char *)buf;
            *out_hdr_len = hdr_end;
            return true;
        }

        if (len >= ARCR_NET_MAX_HEADER_BYTES) {
            free(buf);
            return false;
        }
        if (len + 4096u > cap) {
            size_t next_cap = ARCR_MIN(cap * 2u, ARCR_NET_MAX_HEADER_BYTES + 8192u);
            if (next_cap <= len) {
                free(buf);
                return false;
            }
            uint8_t *grown = (uint8_t *)realloc(buf, next_cap + 1u);
            if (!grown) {
                free(buf);
                return false;
            }
            buf = grown;
            cap = next_cap;
        }

        /* Read bounded chunk (<= 8192) so any body bytes after \r\n\r\n always fit in pre_buf */
        size_t want = ARCR_MIN(cap - len, (size_t)8192u);
        int n = stream_read_some(st, buf + len, want);
        if (n <= 0) {
            free(buf);
            return false;
        }
        if (out_got_any_byte) *out_got_any_byte = true;
        len += (size_t)n;
    }
}

/* Incrementally decodes RFC 9112 Transfer-Encoding: chunked and stops immediately at 0\r\n...\r\n */
static bool stream_read_chunked_body(ArcrNetStream *st, uint8_t **out_body, size_t *out_len,
                                     bool *out_clean_eof) {
    *out_body = NULL;
    *out_len  = 0u;
    if (out_clean_eof) *out_clean_eof = false;

    size_t cap = 8192u;
    size_t len = 0u;
    uint8_t *dst = (uint8_t *)malloc(cap + 1u);
    if (!dst) return false;

    char line[2048];
    for (;;) {
        size_t line_len = 0u;
        if (!stream_read_crlf_line(st, line, sizeof(line), &line_len)) {
            free(dst);
            return false;
        }
        /* Skip optional blank whitespace line if server emitted an extra CRLF */
        const char *p = line;
        size_t plen = line_len;
        arcr_str_trim_view(&p, &plen);
        if (plen == 0u) continue;

        size_t chunk_sz = 0u;
        bool got_hex = false;
        for (size_t i = 0u; i < plen; ++i) {
            if (p[i] == ';' || p[i] == ' ' || p[i] == '\t') break;
            int hv = hex_digit_val(p[i]);
            if (hv < 0) {
                free(dst);
                return false;
            }
            got_hex = true;
            if (chunk_sz > (ARCR_NET_MAX_RESPONSE_BYTES >> 4)) {
                free(dst);
                return false;
            }
            chunk_sz = (chunk_sz << 4) | (size_t)hv;
        }
        if (!got_hex) {
            free(dst);
            return false;
        }

        if (chunk_sz == 0u) {
            /* Consume optional trailer headers until terminating empty CRLF line! */
            for (int t = 0; t < 128; ++t) {
                size_t tlen = 0u;
                if (!stream_read_crlf_line(st, line, sizeof(line), &tlen)) {
                    break;
                }
                if (tlen == 0u) {
                    if (out_clean_eof) *out_clean_eof = true;
                    break;
                }
            }
            break;
        }

        if (len + chunk_sz > ARCR_NET_MAX_RESPONSE_BYTES) {
            free(dst);
            return false;
        }
        if (len + chunk_sz + 1u > cap) {
            size_t next_cap = cap * 2u;
            while (next_cap < len + chunk_sz + 1u) {
                next_cap *= 2u;
            }
            if (next_cap > ARCR_NET_MAX_RESPONSE_BYTES + 65536u) {
                next_cap = len + chunk_sz + 1u;
            }
            uint8_t *grown = (uint8_t *)realloc(dst, next_cap);
            if (!grown) {
                free(dst);
                return false;
            }
            dst = grown;
            cap = next_cap;
        }

        if (!stream_read_exact(st, dst + len, chunk_sz)) {
            free(dst);
            return false;
        }
        len += chunk_sz;

        /* Consume trailing CRLF (or LF) after chunk data */
        uint8_t crlf[2];
        if (!stream_read_exact(st, &crlf[0], 1u)) {
            free(dst);
            return false;
        }
        if (crlf[0] == '\r') {
            if (!stream_read_exact(st, &crlf[1], 1u) || crlf[1] != '\n') {
                free(dst);
                return false;
            }
        } else if (crlf[0] != '\n') {
            free(dst);
            return false;
        }
    }

    dst[len]  = '\0';
    *out_body = dst;
    *out_len  = len;
    return true;
}

/* Reads until clean EOF when neither Content-Length nor chunked framing is present */
static bool stream_read_until_eof(ArcrNetStream *st, uint8_t **out_body, size_t *out_len) {
    *out_body = NULL;
    *out_len  = 0u;

    size_t cap = 16384u;
    size_t len = 0u;
    uint8_t *dst = (uint8_t *)malloc(cap + 1u);
    if (!dst) return false;

    for (;;) {
        if (len >= ARCR_NET_MAX_RESPONSE_BYTES) {
            free(dst);
            return false;
        }
        if (len + 8192u > cap) {
            size_t next_cap = ARCR_MIN(cap * 2u, ARCR_NET_MAX_RESPONSE_BYTES + 16384u);
            if (next_cap <= len) {
                free(dst);
                return false;
            }
            uint8_t *grown = (uint8_t *)realloc(dst, next_cap + 1u);
            if (!grown) {
                free(dst);
                return false;
            }
            dst = grown;
            cap = next_cap;
        }
        size_t want = ARCR_MIN(cap - len, ARCR_NET_MAX_RESPONSE_BYTES - len);
        int n = stream_read_some(st, dst + len, want);
        if (n < 0) {
            /* If server reset after sending full body without Content-Length, keep bytes if >0 */
            if (len > 0u) break;
            free(dst);
            return false;
        }
        if (n == 0) break;
        len += (size_t)n;
    }

    dst[len]  = '\0';
    *out_body = dst;
    *out_len  = len;
    return true;
}

typedef struct ParsedHttpHeaders {
    int      status_code;
    bool     is_http10;
    bool     has_content_length;
    uint64_t content_length;
    bool     is_chunked;
    bool     conn_close;
    bool     conn_keep_alive;
    char     content_type[256];
    char     content_encoding[64];
    char     cache_control[128];
    char     etag[128];
    char     last_modified[128];
    char     location[ARCR_NET_MAX_URL_LEN];
} ParsedHttpHeaders;

static bool extra_headers_has_name(const char *extra_hdr, const char *name_nocase) {
    if (!extra_hdr || !name_nocase || name_nocase[0] == '\0') return false;
    size_t nlen = strlen(name_nocase);
    const char *p = extra_hdr;
    while (*p != '\0') {
        while (*p == '\r' || *p == '\n') ++p;
        if (*p == '\0') break;
        const char *lstart = p;
        while (*p != '\0' && *p != '\r' && *p != '\n') ++p;
        size_t llen = (size_t)(p - lstart);
        const char *colon = memchr(lstart, ':', llen);
        if (colon) {
            const char *hk = lstart;
            size_t hk_len  = (size_t)(colon - lstart);
            arcr_str_trim_view(&hk, &hk_len);
            if (hk_len == nlen && arcr_str_eq_nocase_n(hk, hk_len, name_nocase, nlen)) {
                return true;
            }
        }
    }
    return false;
}

static bool is_same_origin_url(const char *origin_url, const ArcrParsedUrl *target) {
    if (!origin_url || origin_url[0] == '\0' || !target) return true;
    if (arcr_str_eq_nocase(origin_url, "null")) return false;
    ArcrParsedUrl op;
    if (!net_parse_url(origin_url, &op)) return false;
    uint16_t p1 = op.port;
    if (p1 == 0u) p1 = arcr_str_eq_nocase(op.scheme, "https") ? 443u : 80u;
    uint16_t p2 = target->port;
    if (p2 == 0u) p2 = arcr_str_eq_nocase(target->scheme, "https") ? 443u : 80u;
    return arcr_str_eq_nocase(op.scheme, target->scheme) &&
           arcr_str_eq_nocase(op.host, target->host) &&
           p1 == p2;
}

static bool parse_http_header_block(const char *hdr_text, size_t hdr_len,
                                    const char *req_host, const char *req_path,
                                    bool store_cookies, ParsedHttpHeaders *out,
                                    char **out_raw_hdrs, size_t *out_raw_hdrs_len) {
    memset(out, 0, sizeof(*out));
    if (out_raw_hdrs) *out_raw_hdrs = NULL;
    if (out_raw_hdrs_len) *out_raw_hdrs_len = 0u;
    arcr_str_copy(out->content_type, sizeof(out->content_type), "text/html; charset=utf-8");

    size_t line_end = 0u;
    while (line_end < hdr_len && hdr_text[line_end] != '\r' && hdr_text[line_end] != '\n') {
        ++line_end;
    }
    if (line_end < 9u || !arcr_str_starts_with_nocase(hdr_text, "HTTP/")) {
        return false;
    }
    if (line_end >= 8u && strncmp(hdr_text + 5u, "1.0", 3u) == 0) {
        out->is_http10 = true;
    }
    size_t sp = 5u;
    while (sp < line_end && hdr_text[sp] != ' ') ++sp;
    while (sp < line_end && hdr_text[sp] == ' ') ++sp;
    int32_t sc = 0;
    if (sp + 3u > line_end || !arcr_parse_i32(hdr_text + sp, 3u, &sc) || sc <= 0) {
        return false;
    }
    out->status_code = (int)sc;

    size_t hpos = line_end;
    while (hpos < hdr_len && (hdr_text[hpos] == '\r' || hdr_text[hpos] == '\n')) ++hpos;
    if (out_raw_hdrs && hpos <= hdr_len) {
        size_t rem = hdr_len - hpos;
        char *rh = (char *)malloc(rem + 3u);
        if (rh) {
            if (rem > 0u) memcpy(rh, hdr_text + hpos, rem);
            size_t rlen = rem;
            if (rlen > 0u && rh[rlen - 1u] != '\n') {
                rh[rlen++] = '\r';
                rh[rlen++] = '\n';
            }
            rh[rlen] = '\0';
            *out_raw_hdrs = rh;
            if (out_raw_hdrs_len) *out_raw_hdrs_len = rlen;
        }
    }

    while (hpos < hdr_len) {
        while (hpos < hdr_len && (hdr_text[hpos] == '\r' || hdr_text[hpos] == '\n')) ++hpos;
        if (hpos >= hdr_len) break;
        size_t lstart = hpos;
        while (hpos < hdr_len && hdr_text[hpos] != '\r' && hdr_text[hpos] != '\n') ++hpos;
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

        if (arcr_str_eq_nocase_n(hk, hk_len, "content-length", 14u)) {
            uint64_t cl_val = 0u;
            if (parse_u64_strict(hv, hv_len, &cl_val)) {
                out->has_content_length = true;
                out->content_length     = cl_val;
            }
        } else if (arcr_str_eq_nocase_n(hk, hk_len, "transfer-encoding", 17u)) {
            char te_tmp[128];
            size_t cl = ARCR_MIN(hv_len, sizeof(te_tmp) - 1u);
            memcpy(te_tmp, hv, cl);
            te_tmp[cl] = '\0';
            if (header_contains_token_nocase(te_tmp, "chunked")) {
                out->is_chunked = true;
            }
        } else if (arcr_str_eq_nocase_n(hk, hk_len, "content-encoding", 16u)) {
            size_t cl = ARCR_MIN(hv_len, sizeof(out->content_encoding) - 1u);
            memcpy(out->content_encoding, hv, cl);
            out->content_encoding[cl] = '\0';
        } else if (arcr_str_eq_nocase_n(hk, hk_len, "content-type", 12u)) {
            size_t cl = ARCR_MIN(hv_len, sizeof(out->content_type) - 1u);
            memcpy(out->content_type, hv, cl);
            out->content_type[cl] = '\0';
        } else if (arcr_str_eq_nocase_n(hk, hk_len, "location", 8u)) {
            size_t cl = ARCR_MIN(hv_len, sizeof(out->location) - 1u);
            memcpy(out->location, hv, cl);
            out->location[cl] = '\0';
        } else if (arcr_str_eq_nocase_n(hk, hk_len, "connection", 10u)) {
            char conn_tmp[128];
            size_t cl = ARCR_MIN(hv_len, sizeof(conn_tmp) - 1u);
            memcpy(conn_tmp, hv, cl);
            conn_tmp[cl] = '\0';
            if (header_contains_token_nocase(conn_tmp, "close")) {
                out->conn_close = true;
            }
            if (header_contains_token_nocase(conn_tmp, "keep-alive")) {
                out->conn_keep_alive = true;
            }
        } else if (arcr_str_eq_nocase_n(hk, hk_len, "cache-control", 13u)) {
            size_t cl = ARCR_MIN(hv_len, sizeof(out->cache_control) - 1u);
            memcpy(out->cache_control, hv, cl);
            out->cache_control[cl] = '\0';
        } else if (arcr_str_eq_nocase_n(hk, hk_len, "etag", 4u)) {
            size_t cl = ARCR_MIN(hv_len, sizeof(out->etag) - 1u);
            memcpy(out->etag, hv, cl);
            out->etag[cl] = '\0';
        } else if (arcr_str_eq_nocase_n(hk, hk_len, "last-modified", 13u)) {
            size_t cl = ARCR_MIN(hv_len, sizeof(out->last_modified) - 1u);
            memcpy(out->last_modified, hv, cl);
            out->last_modified[cl] = '\0';
        } else if (arcr_str_eq_nocase_n(hk, hk_len, "set-cookie", 10u)) {
            if (store_cookies) {
                cookie_jar_store_header(req_host, req_path, hv, hv_len);
            }
        }
    }
    return true;
}

static FILE *open_local_file_utf8(const char *path) {
#if defined(_WIN32)
    WCHAR wpath[4096];
    int wn = MultiByteToWideChar(CP_UTF8, 0, path, -1,
                                 wpath, (int)(sizeof(wpath) / sizeof(wpath[0])));
    if (wn > 0) {
        FILE *wfp = _wfopen(wpath, L"rb");
        if (wfp) return wfp;
    }
#endif
    return fopen(path, "rb");
}

static bool build_http_request_wire(const ArcrParsedUrl *parsed, bool use_tls,
                                    const ArcrNetStream *stream,
                                    const char *method,
                                    const char *post_body, size_t post_len,
                                    const ArcrHttpRequest *req,
                                    bool allow_cookies,
                                    const char *cond_etag, const char *cond_lastmod,
                                    char **out_wire, size_t *out_wire_len) {
    *out_wire     = NULL;
    *out_wire_len = 0u;

    char host_hdr[300];
    if ((use_tls && parsed->port == 443u) || (!use_tls && parsed->port == 80u)) {
        arcr_str_copy(host_hdr, sizeof(host_hdr), parsed->host);
    } else {
        snprintf(host_hdr, sizeof(host_hdr), "%s:%u", parsed->host, (unsigned)parsed->port);
    }

    /* RFC 9112 Section 3.2.2: use absolute-form target when speaking plain HTTP through HTTP_PROXY */
    char *req_target = (char *)malloc(ARCR_NET_MAX_URL_LEN + 320u);
    char *cookie_vals = (char *)malloc(16384u);
    if (!req_target || !cookie_vals) {
        free(req_target);
        free(cookie_vals);
        return false;
    }

    if (stream->via_http_proxy && !use_tls) {
        if (parsed->port == 80u) {
            snprintf(req_target, ARCR_NET_MAX_URL_LEN + 320u,
                     "http://%s%s", parsed->host, parsed->path);
        } else {
            snprintf(req_target, ARCR_NET_MAX_URL_LEN + 320u,
                     "http://%s:%u%s", parsed->host, (unsigned)parsed->port, parsed->path);
        }
    } else {
        arcr_str_copy(req_target, ARCR_NET_MAX_URL_LEN + 320u, parsed->path);
    }

    const char *extra_hdr = (req->extra_headers && req->extra_headers[0] != '\0')
                                ? req->extra_headers
                                : NULL;

    size_t cookie_len = 0u;
    if (allow_cookies && !extra_headers_has_name(extra_hdr, "cookie")) {
        cookie_len = cookie_jar_build_header(parsed->host, parsed->path, use_tls,
                                             cookie_vals, 16384u);
    }

    const char *accept_hdr = (req->accept && req->accept[0] != '\0')
        ? req->accept
        : "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8";
    const char *referer_hdr = (req->referer && req->referer[0] != '\0') ? req->referer : NULL;
    const char *origin_hdr  = (req->origin && req->origin[0] != '\0' &&
                               !extra_headers_has_name(extra_hdr, "origin"))
                                  ? req->origin
                                  : NULL;
    const char *inm_hdr     = (cond_etag && cond_etag[0] != '\0' &&
                               !extra_headers_has_name(extra_hdr, "if-none-match"))
                                  ? cond_etag
                                  : NULL;
    const char *ims_hdr     = (cond_lastmod && cond_lastmod[0] != '\0' &&
                               !extra_headers_has_name(extra_hdr, "if-modified-since"))
                                  ? cond_lastmod
                                  : NULL;

    size_t extra_len = extra_hdr ? strlen(extra_hdr) : 0u;
    bool extra_needs_crlf = (extra_len > 0u && extra_hdr[extra_len - 1u] != '\n');

    size_t cap = strlen(req_target) + cookie_len +
                 (referer_hdr ? strlen(referer_hdr) : 0u) +
                 (origin_hdr ? strlen(origin_hdr) : 0u) +
                 extra_len + 2560u;
    char *wire = (char *)malloc(cap);
    if (!wire) {
        free(req_target);
        free(cookie_vals);
        return false;
    }

    bool has_body_hdrs = arcr_str_eq_nocase(method, "POST") ||
                         arcr_str_eq_nocase(method, "PUT") ||
                         (post_body != NULL && post_len > 0u);
    bool emit_ctype = has_body_hdrs && !extra_headers_has_name(extra_hdr, "content-type");
    const char *ctype = (req->content_type && req->content_type[0] != '\0')
                            ? req->content_type
                            : "application/x-www-form-urlencoded";

    int n = snprintf(
        wire, cap,
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Waterscape/2.0 Chrome/134.0.0.0 Safari/537.36\r\n"
        "Accept: %s\r\n"
        "Accept-Encoding: br, gzip, deflate\r\n"
        "Accept-Language: ko-KR,ko;q=0.9,en-US;q=0.8,en;q=0.7\r\n"
        "%s"
        "%s%s%s"
        "%s%s%s"
        "%s%s%s"
        "%s%s%s"
        "%s%s%s"
        "%s%s"
        "Connection: keep-alive\r\n",
        method, req_target,
        host_hdr,
        accept_hdr,
        stream->via_http_proxy ? stream->proxy_auth_hdr : "",
        referer_hdr ? "Referer: " : "", referer_hdr ? referer_hdr : "", referer_hdr ? "\r\n" : "",
        origin_hdr ? "Origin: " : "", origin_hdr ? origin_hdr : "", origin_hdr ? "\r\n" : "",
        inm_hdr ? "If-None-Match: " : "", inm_hdr ? inm_hdr : "", inm_hdr ? "\r\n" : "",
        ims_hdr ? "If-Modified-Since: " : "", ims_hdr ? ims_hdr : "", ims_hdr ? "\r\n" : "",
        (cookie_len > 0u) ? "Cookie: " : "", (cookie_len > 0u) ? cookie_vals : "", (cookie_len > 0u) ? "\r\n" : "",
        extra_hdr ? extra_hdr : "", extra_needs_crlf ? "\r\n" : ""
    );
    free(req_target);
    free(cookie_vals);

    if (n <= 0 || (size_t)n + 256u >= cap) {
        free(wire);
        return false;
    }
    size_t pos = (size_t)n;
    if (has_body_hdrs) {
        int nb = emit_ctype
                     ? snprintf(wire + pos, cap - pos,
                                "Content-Type: %s\r\n"
                                "Content-Length: %lu\r\n\r\n",
                                ctype, (unsigned long)post_len)
                     : snprintf(wire + pos, cap - pos,
                                "Content-Length: %lu\r\n\r\n",
                                (unsigned long)post_len);
        if (nb <= 0 || pos + (size_t)nb >= cap) {
            free(wire);
            return false;
        }
        pos += (size_t)nb;
    } else {
        wire[pos++] = '\r';
        wire[pos++] = '\n';
        wire[pos]   = '\0';
    }

    *out_wire     = wire;
    *out_wire_len = pos;
    return true;
}

static bool net_fetch_http(const ArcrHttpRequest *req, ArcrHttpResponse *out_resp) {
    if (!req || !req->url || !out_resp) return false;
    memset(out_resp, 0, sizeof(*out_resp));

    if (req->cancel_flag && *req->cancel_flag != 0) {
        out_resp->cancelled = true;
        arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), "요청 취소됨");
        return false;
    }

    /* 0. Handle arbitrary-size inline data: URIs BEFORE copying into fixed 4096B URL buffers! */
    const char *init_url = req->url;
    size_t init_ulen     = strlen(init_url);
    arcr_str_trim_view(&init_url, &init_ulen);
    if (init_ulen == 0u) return false;
    if (init_ulen >= 5u && arcr_str_eq_nocase_n(init_url, 5u, "data:", 5u)) {
        return handle_data_uri_view(init_url, init_ulen,
                                    init_url + 5u, init_ulen - 5u, out_resp);
    }

    char *current_url = (char *)malloc(ARCR_NET_MAX_URL_LEN);
    char *next_url    = (char *)malloc(ARCR_NET_MAX_URL_LEN);
    if (!current_url || !next_url) {
        free(current_url);
        free(next_url);
        return false;
    }

    size_t copy_u = (init_ulen < ARCR_NET_MAX_URL_LEN - 1u) ? init_ulen : (ARCR_NET_MAX_URL_LEN - 1u);
    memcpy(current_url, init_url, copy_u);
    current_url[copy_u] = '\0';
    arcr_str_copy(out_resp->final_url, sizeof(out_resp->final_url), current_url);

    char method[16] = "GET";
    if (req->method && req->method[0] != '\0') {
        size_t mlen = strlen(req->method);
        if (mlen < sizeof(method)) {
            for (size_t i = 0u; i < mlen; ++i) {
                char c = req->method[i];
                method[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
            }
            method[mlen] = '\0';
        }
    }
    bool is_get  = (strcmp(method, "GET") == 0);
    bool is_head = (strcmp(method, "HEAD") == 0);
    const char *post_body = (!is_get && !is_head) ? req->post_body : NULL;
    size_t post_len       = (post_body != NULL) ? req->post_body_len : 0u;

    for (int hop = 0; hop <= ARCR_NET_MAX_REDIRECTS; ++hop) {
        if (req->cancel_flag && *req->cancel_flag != 0) {
            out_resp->cancelled = true;
            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), "요청 취소됨");
            free(current_url);
            free(next_url);
            return false;
        }
        arcr_str_copy(out_resp->final_url, sizeof(out_resp->final_url), current_url);
        out_resp->redirected = (hop > 0);

        size_t cur_len = strlen(current_url);
        if (cur_len >= 5u && arcr_str_eq_nocase_n(current_url, 5u, "data:", 5u)) {
            bool ok = handle_data_uri_view(current_url, cur_len,
                                           current_url + 5u, cur_len - 5u, out_resp);
            out_resp->redirected = (hop > 0);
            free(current_url);
            free(next_url);
            return ok;
        }

        ArcrParsedUrl *parsed = (ArcrParsedUrl *)malloc(sizeof(ArcrParsedUrl));
        if (!parsed) {
            free(current_url);
            free(next_url);
            return false;
        }
        if (!net_parse_url(current_url, parsed)) {
            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), "잘못된 URL 형식");
            free(parsed);
            free(current_url);
            free(next_url);
            return false;
        }

        /* 1. Built-in schemes: waterscape://, arcr://, about: */
        if (arcr_str_eq_nocase(parsed->scheme, "waterscape") ||
            arcr_str_eq_nocase(parsed->scheme, "arcr")) {
            bool ok = (arcr_str_eq_nocase(parsed->host, "welcome") ||
                       arcr_str_eq_nocase(parsed->host, "newtab"))
                          ? fill_static_html_response(current_url, kBuiltInWelcomeHtml,
                                                      "Waterscape 시작 페이지", out_resp)
                          : fill_static_html_response(current_url, kBuiltInSettingsHtml,
                                                      "Arcradient 내부 페이지", out_resp);
            free(parsed);
            free(current_url);
            free(next_url);
            return ok;
        }
        if (arcr_str_eq_nocase(parsed->scheme, "about")) {
            bool ok = arcr_str_eq_nocase(parsed->path, "waterscape")
                          ? fill_static_html_response(current_url, kBuiltInWelcomeHtml,
                                                      "Waterscape 소개 페이지", out_resp)
                          : fill_static_html_response(current_url, kAboutBlankHtml,
                                                      "빈 페이지 (about:blank)", out_resp);
            free(parsed);
            free(current_url);
            free(next_url);
            return ok;
        }

        /* 2. Guarded local file:// URI with %XX URL decoding (%20 -> space) */
        if (arcr_str_eq_nocase(parsed->scheme, "file")) {
            const char *fpath = parsed->path;
#if defined(_WIN32)
            if (fpath[0] == '/' && is_alpha_ascii(fpath[1]) && fpath[2] == ':') {
                ++fpath;
            }
#endif
            const char *fq = strchr(fpath, '?');
            size_t raw_flen = fq ? (size_t)(fq - fpath) : strlen(fpath);

            char *raw_fpath = (char *)malloc(raw_flen + 1u);
            char *dec_fpath = (char *)malloc(raw_flen + 1u);
            if (!raw_fpath || !dec_fpath) {
                free(raw_fpath);
                free(dec_fpath);
                free(parsed);
                free(current_url);
                free(next_url);
                return false;
            }
            memcpy(raw_fpath, fpath, raw_flen);
            raw_fpath[raw_flen] = '\0';

            if (!is_safe_local_file_path(raw_fpath) ||
                !percent_decode_path(raw_fpath, raw_flen, dec_fpath, raw_flen + 1u) ||
                !is_safe_local_file_path(dec_fpath)) {
                out_resp->status_code = 403;
                out_resp->security    = ARCR_SEC_LOCAL_OR_NONE;
                arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                              "보안 정책에 의해 차단된 로컬 파일 경로 (file://)");
                free(raw_fpath);
                free(dec_fpath);
                free(parsed);
                free(current_url);
                free(next_url);
                return false;
            }

            FILE *fp = open_local_file_utf8(dec_fpath);
            if (!fp) {
                out_resp->status_code = 404;
                arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                              "로컬 파일을 찾을 수 없습니다 (file://)");
                free(raw_fpath);
                free(dec_fpath);
                free(parsed);
                free(current_url);
                free(next_url);
                return false;
            }
            if (fseek(fp, 0, SEEK_END) != 0) {
                fclose(fp);
                free(raw_fpath);
                free(dec_fpath);
                free(parsed);
                free(current_url);
                free(next_url);
                return false;
            }
            long sz = ftell(fp);
            if (sz < 0 || (unsigned long)sz > ARCR_NET_MAX_RESPONSE_BYTES) {
                fclose(fp);
                free(raw_fpath);
                free(dec_fpath);
                free(parsed);
                free(current_url);
                free(next_url);
                return false;
            }
            (void)fseek(fp, 0, SEEK_SET);
            uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1u);
            if (!buf) {
                fclose(fp);
                free(raw_fpath);
                free(dec_fpath);
                free(parsed);
                free(current_url);
                free(next_url);
                return false;
            }
            size_t rd = fread(buf, 1u, (size_t)sz, fp);
            buf[rd] = '\0';
            fclose(fp);

            out_resp->status_code   = 200;
            arcr_str_copy(out_resp->content_type, sizeof(out_resp->content_type),
                          guess_mime_from_path(dec_fpath));
            out_resp->body          = buf;
            out_resp->body_len      = rd;
            out_resp->encoded_bytes = rd;
            out_resp->decoded_bytes = rd;
            out_resp->security      = ARCR_SEC_LOCAL_OR_NONE;
            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), "로컬 문서 (file://)");

            free(raw_fpath);
            free(dec_fpath);
            free(parsed);
            free(current_url);
            free(next_url);
            return true;
        }

        /* 3. Check HTTP/HTTPS scheme */
        bool use_tls = arcr_str_eq_nocase(parsed->scheme, "https");
        if (!use_tls && !arcr_str_eq_nocase(parsed->scheme, "http")) {
            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), "지원하지 않는 URL 프로토콜");
            free(parsed);
            free(current_url);
            free(next_url);
            return false;
        }

        bool is_cross_origin = (req->origin && req->origin[0] != '\0') &&
                               !is_same_origin_url(req->origin, parsed);
        bool allow_cookies   = !req->private_browsing &&
                               (!is_cross_origin || req->include_credentials);

        /* 4. Check 32-entry LRU GET cache (keyed without #fragment via canonicalized URL) */
        const char *frag_in_cur = strchr(current_url, '#');
        size_t cache_key_len = frag_in_cur ? (size_t)(frag_in_cur - current_url) : strlen(current_url);
        char *cache_key = (char *)malloc(cache_key_len + 1u);
        if (!cache_key) {
            free(parsed);
            free(current_url);
            free(next_url);
            return false;
        }
        memcpy(cache_key, current_url, cache_key_len);
        cache_key[cache_key_len] = '\0';

        char cond_etag[128]    = "";
        char cond_lastmod[128] = "";
        bool has_cond_validator = false;
        bool caller_sent_conditional =
            extra_headers_has_name(req->extra_headers, "if-none-match") ||
            extra_headers_has_name(req->extra_headers, "if-modified-since");

        if (is_get && !req->bypass_cache && !req->private_browsing && !caller_sent_conditional) {
            if (cache_lookup_or_validators(cache_key, out_resp,
                                           cond_etag, sizeof(cond_etag),
                                           cond_lastmod, sizeof(cond_lastmod),
                                           &has_cond_validator)) {
                arcr_str_copy(out_resp->final_url, sizeof(out_resp->final_url), current_url);
                out_resp->redirected = (hop > 0);
                free(cache_key);
                free(parsed);
                free(current_url);
                free(next_url);
                return true;
            }
        }

        /* Verify CSPRNG entropy before external network I/O */
        uint8_t rng_probe[16];
        if (!arcr_platform_csprng_bytes(rng_probe, sizeof(rng_probe))) {
            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), "플랫폼 CSPRNG 난수원 오류");
            free(cache_key);
            free(parsed);
            free(current_url);
            free(next_url);
            return false;
        }

        /* 5. Acquire from 8-Slot Keep-Alive Pool (or dial fresh connection with 1x transparent retry) */
        ArcrNetStream *stream = ka_pool_acquire(parsed->host, parsed->port, use_tls);
        bool reused_from_pool = (stream != NULL);
        char *hdr_block       = NULL;
        size_t hdr_block_len  = 0u;

        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!stream) {
                reused_from_pool = false;
                bool conn_cancelled = false;
                stream = stream_connect_new(parsed->host, parsed->port, use_tls,
                                            req->cancel_flag, &conn_cancelled,
                                            &out_resp->security, out_resp->detail,
                                            sizeof(out_resp->detail));
                if (!stream) {
                    if (conn_cancelled) out_resp->cancelled = true;
                    free(cache_key);
                    free(parsed);
                    free(current_url);
                    free(next_url);
                    return false;
                }
            } else {
                stream->cancel_flag   = req->cancel_flag;
                stream->was_cancelled = false;
                out_resp->security    = stream->security;
                arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), stream->detail);
            }

            char *req_wire = NULL;
            size_t req_wire_len = 0u;
            if (!build_http_request_wire(parsed, use_tls, stream, method,
                                         post_body, post_len, req,
                                         allow_cookies, cond_etag, cond_lastmod,
                                         &req_wire, &req_wire_len)) {
                stream_destroy(stream);
                free(cache_key);
                free(parsed);
                free(current_url);
                free(next_url);
                return false;
            }

            bool write_ok = stream_write_all(stream, (const uint8_t *)req_wire, req_wire_len);
            free(req_wire);
            if (write_ok && post_body && post_len > 0u) {
                write_ok = stream_write_all(stream, (const uint8_t *)post_body, post_len);
            }

            if (!write_ok) {
                bool was_canc = stream->was_cancelled;
                stream_destroy(stream);
                stream = NULL;
                if (!was_canc && reused_from_pool && attempt == 0) {
                    continue; /* Transparently retry once on a fresh connection! */
                }
                if (was_canc) out_resp->cancelled = true;
                arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                              was_canc ? "요청 취소됨" : "HTTP 요청 전송 실패");
                free(cache_key);
                free(parsed);
                free(current_url);
                free(next_url);
                return false;
            }

            bool got_any_byte = false;
            if (stream_read_header_block(stream, &hdr_block, &hdr_block_len, &got_any_byte)) {
                break;
            }

            bool was_canc = stream->was_cancelled;
            stream_destroy(stream);
            stream = NULL;
            if (!was_canc && reused_from_pool && !got_any_byte && attempt == 0) {
                continue; /* Server closed idle Keep-Alive socket right as we wrote; retry fresh! */
            }

            if (was_canc) out_resp->cancelled = true;
            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                          was_canc ? "요청 취소됨" : "HTTP 응답 헤더 수신 실패");
            free(cache_key);
            free(parsed);
            free(current_url);
            free(next_url);
            return false;
        }

        /* 6. Parse status + headers, skipping 1xx informational blocks (100 Continue, 103 Early Hints) */
        ParsedHttpHeaders *ph = (ParsedHttpHeaders *)malloc(sizeof(ParsedHttpHeaders));
        if (!ph) {
            free(hdr_block);
            stream_destroy(stream);
            free(cache_key);
            free(parsed);
            free(current_url);
            free(next_url);
            return false;
        }

        char  *resp_raw_headers     = NULL;
        size_t resp_raw_headers_len = 0u;
        bool   hdr_ok               = false;
        for (int info_hop = 0; info_hop < 8; ++info_hop) {
            free(resp_raw_headers);
            resp_raw_headers     = NULL;
            resp_raw_headers_len = 0u;

            if (!parse_http_header_block(hdr_block, hdr_block_len, parsed->host, parsed->path,
                                         allow_cookies, ph,
                                         &resp_raw_headers, &resp_raw_headers_len)) {
                free(hdr_block);
                hdr_block = NULL;
                break;
            }
            free(hdr_block);
            hdr_block = NULL;

            if (ph->status_code >= 100 && ph->status_code <= 199 && ph->status_code != 101) {
                /* Discard 100 Continue / 103 Early Hints and immediately read final response headers! */
                if (!stream_read_header_block(stream, &hdr_block, &hdr_block_len, NULL)) {
                    break;
                }
                continue;
            }
            hdr_ok = true;
            break;
        }

        if (!hdr_ok) {
            if (stream->was_cancelled) out_resp->cancelled = true;
            free(resp_raw_headers);
            free(ph);
            stream_destroy(stream);
            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                          out_resp->cancelled ? "요청 취소됨" : "잘못된 HTTP 응답 상태줄·헤더");
            free(cache_key);
            free(parsed);
            free(current_url);
            free(next_url);
            return false;
        }

        /* 7. Read exact response body according to RFC 9112 Section 6 framing (ZERO hang!) */
        uint8_t *final_body = NULL;
        size_t   final_len  = 0u;
        bool     framing_clean = false;
        bool     body_ok       = false;

        if (is_head || ph->status_code == 204 || ph->status_code == 304 || ph->status_code == 101) {
            final_body = (uint8_t *)malloc(1u);
            if (final_body) {
                final_body[0] = '\0';
                final_len     = 0u;
                framing_clean = (ph->status_code != 101);
                body_ok       = true;
            }
        } else if (ph->is_chunked) {
            body_ok = stream_read_chunked_body(stream, &final_body, &final_len, &framing_clean);
            if (!body_ok) {
                if (stream->was_cancelled) {
                    out_resp->cancelled = true;
                    arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), "요청 취소됨");
                } else {
                    out_resp->length_mismatch = true;
                    out_resp->status_code     = ph->status_code;
                    arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                                  "HTTP chunked 본문 스트림 디코딩 실패");
                }
            }
        } else if (ph->has_content_length) {
            if (ph->content_length > ARCR_NET_MAX_RESPONSE_BYTES) {
                arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                              "HTTP 응답 Content-Length 제한(64MB) 초과");
                body_ok = false;
            } else {
                size_t exact = (size_t)ph->content_length;
                final_body = (uint8_t *)malloc(exact + 1u);
                if (final_body) {
                    if (exact == 0u || stream_read_exact(stream, final_body, exact)) {
                        final_body[exact] = '\0';
                        final_len         = exact;
                        framing_clean     = true;
                        body_ok           = true;
                    } else {
                        free(final_body);
                        final_body = NULL;
                        if (stream->was_cancelled) {
                            out_resp->cancelled = true;
                            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail), "요청 취소됨");
                        } else {
                            out_resp->length_mismatch = true;
                            out_resp->status_code     = ph->status_code;
                            arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                                          "HTTP Content-Length 본문 길이 불일치(수신 중 연결 끊김)");
                        }
                    }
                }
            }
        } else {
            body_ok = stream_read_until_eof(stream, &final_body, &final_len);
            framing_clean = false; /* EOF-delimited stream cannot be pooled */
            if (!body_ok) {
                if (stream->was_cancelled) out_resp->cancelled = true;
                arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                              out_resp->cancelled ? "요청 취소됨" : "HTTP 응답 본문 수신 실패");
            }
        }

        bool can_keep_alive = framing_clean && !ph->conn_close && !stream->was_cancelled &&
                              (!ph->is_http10 || ph->conn_keep_alive);
        if (body_ok && can_keep_alive) {
            ka_pool_release(stream, parsed->host, parsed->port, use_tls);
        } else {
            stream_destroy(stream);
        }
        stream = NULL;

        if (!body_ok || !final_body) {
            free(resp_raw_headers);
            free(final_body);
            free(ph);
            free(cache_key);
            free(parsed);
            free(current_url);
            free(next_url);
            return false;
        }

        /* 8. Handle HTTP Redirects (301, 302, 303, 307, 308 up to 6 hops) */
        int status_code = ph->status_code;
        if ((status_code == 301 || status_code == 302 || status_code == 303 ||
             status_code == 307 || status_code == 308) && ph->location[0] != '\0') {
            free(resp_raw_headers);
            free(final_body);
            if (hop == ARCR_NET_MAX_REDIRECTS) {
                arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                              "HTTP 리다이렉트 횟수 초과 (최대 6회)");
                free(ph);
                free(cache_key);
                free(parsed);
                free(current_url);
                free(next_url);
                return false;
            }
            if (!net_resolve_url(current_url, ph->location, next_url, ARCR_NET_MAX_URL_LEN)) {
                arcr_str_copy(out_resp->detail, sizeof(out_resp->detail),
                              "잘못된 리다이렉트 Location URL");
                free(ph);
                free(cache_key);
                free(parsed);
                free(current_url);
                free(next_url);
                return false;
            }
            arcr_str_copy(current_url, ARCR_NET_MAX_URL_LEN, next_url);
            if (status_code == 303 ||
                ((status_code == 301 || status_code == 302) && !is_get && !is_head)) {
                arcr_str_copy(method, sizeof(method), "GET");
                is_get    = true;
                post_body = NULL;
                post_len  = 0u;
            }
            free(ph);
            free(cache_key);
            free(parsed);
            continue;
        }

        /* 8b. Fulfill 304 Not Modified from conditional cache entry when automatically revalidated */
        if (status_code == 304 && has_cond_validator && !caller_sent_conditional) {
            if (cache_fulfill_304(cache_key, out_resp)) {
                arcr_str_copy(out_resp->final_url, sizeof(out_resp->final_url), current_url);
                out_resp->redirected = (hop > 0);
                free(resp_raw_headers);
                free(final_body);
                free(ph);
                free(cache_key);
                free(parsed);
                free(current_url);
                free(next_url);
                return true;
            }
        }

        /* 9. Decompress Content-Encoding (br, gzip, deflate) strictly — never return binary garbage! */
        size_t wire_body_len = final_len;
        if (ph->content_encoding[0] != '\0' &&
            !arcr_str_eq_nocase(ph->content_encoding, "identity") &&
            final_len > 0u) {
            uint8_t *decomp     = NULL;
            size_t   decomp_len = 0u;
            if (!arcr_decompress_http_body(final_body, final_len, ph->content_encoding,
                                           &decomp, &decomp_len, ARCR_NET_MAX_RESPONSE_BYTES)) {
                snprintf(out_resp->detail, sizeof(out_resp->detail),
                         "HTTP 압축 해제 실패 (Content-Encoding: %s)", ph->content_encoding);
                out_resp->status_code = status_code;
                free(resp_raw_headers);
                free(final_body);
                free(ph);
                free(cache_key);
                free(parsed);
                free(current_url);
                free(next_url);
                return false;
            }
            uint8_t *nt = (uint8_t *)realloc(decomp, decomp_len + 1u);
            if (!nt) {
                free(decomp);
                free(resp_raw_headers);
                free(final_body);
                free(ph);
                free(cache_key);
                free(parsed);
                free(current_url);
                free(next_url);
                return false;
            }
            nt[decomp_len] = '\0';
            free(final_body);
            final_body = nt;
            final_len  = decomp_len;
        }

        out_resp->status_code     = status_code;
        arcr_str_copy(out_resp->content_type, sizeof(out_resp->content_type), ph->content_type);
        out_resp->body            = final_body;
        out_resp->body_len        = final_len;
        out_resp->encoded_bytes   = wire_body_len;
        out_resp->decoded_bytes   = final_len;
        out_resp->raw_headers     = resp_raw_headers;
        out_resp->raw_headers_len = resp_raw_headers ? resp_raw_headers_len : 0u;
        out_resp->from_cache      = false;
        out_resp->redirected      = (hop > 0);

        if (is_get && status_code == 200 && !req->private_browsing &&
            !header_contains_token_nocase(ph->cache_control, "no-store")) {
            bool must_reval = header_contains_token_nocase(ph->cache_control, "no-cache") ||
                              header_contains_token_nocase(ph->cache_control, "must-revalidate") ||
                              strstr(ph->cache_control, "max-age=0") != NULL;
            cache_insert(cache_key, out_resp, ph->etag, ph->last_modified, must_reval);
        }

        free(ph);
        free(cache_key);
        free(parsed);
        free(current_url);
        free(next_url);
        return true;
    }

    free(current_url);
    free(next_url);
    return false;
}

/* =========================================================================
 * 8. Parallel Multi-Core Batch Fetch & VTable Export
 * ========================================================================= */
static ArcrThreadPool *g_net_threadpool = NULL;

typedef struct BatchFetchCtx {
    const ArcrHttpRequest *reqs;
    ArcrHttpResponse      *resps;
    size_t                 count;
} BatchFetchCtx;

static void batch_fetch_worker(void *userdata, uint32_t idx, uint32_t worker_id) {
    (void)worker_id;
    BatchFetchCtx *ctx = (BatchFetchCtx *)userdata;
    if ((size_t)idx < ctx->count) {
        (void)net_fetch_http(&ctx->reqs[idx], &ctx->resps[idx]);
    }
}

static void net_fetch_batch_parallel(const ArcrHttpRequest *reqs, ArcrHttpResponse *out_resps,
                                     size_t count) {
    if (!reqs || !out_resps || count == 0u) return;
    memset(out_resps, 0, count * sizeof(ArcrHttpResponse));

    if (count == 1u) {
        (void)net_fetch_http(&reqs[0], &out_resps[0]);
        return;
    }

    BatchFetchCtx ctx;
    ctx.reqs  = reqs;
    ctx.resps = out_resps;
    ctx.count = count;

    net_mutex_lock(&g_tpool_mtx);
    if (!g_net_threadpool) {
        uint32_t hw = arcr_hardware_cpu_threads();
        uint32_t workers = ARCR_CLAMP(hw < 4u ? 4u : hw, 2u, 8u);
        g_net_threadpool = arcr_threadpool_create(workers);
    }
    if (g_net_threadpool) {
        arcr_threadpool_parallel_for(g_net_threadpool, (uint32_t)count,
                                     batch_fetch_worker, &ctx);
    } else {
        for (size_t i = 0u; i < count; ++i) {
            (void)net_fetch_http(&reqs[i], &out_resps[i]);
        }
    }
    net_mutex_unlock(&g_tpool_mtx);
}

static void net_free_response(ArcrHttpResponse *resp) {
    if (!resp) return;
    free(resp->body);
    free(resp->raw_headers);
    memset(resp, 0, sizeof(*resp));
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
    free(resp.raw_headers);
    resp.raw_headers = NULL;
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
    .fetch_batch_parallel    = net_fetch_batch_parallel,
    .free_response           = net_free_response,
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

static int g_net_module_refs = 0;

const ArcrModuleDescriptor *arcr_net_module_query(void);
const ArcrModuleDescriptor *arcr_net_module_query(void) {
    net_mutex_lock(&g_state_mtx);
    g_net_module_refs++;
    net_mutex_unlock(&g_state_mtx);

    g_net_desc.arch_id = (uint16_t)arcr_host_compile_arch();
#if defined(_WIN32)
    g_net_desc.min_os_major_minor = ARCR_OS_WIN2000;
#endif
    return &g_net_desc;
}

static void arcr_net_module_shutdown_impl(void) {
    net_mutex_lock(&g_state_mtx);
    if (g_net_module_refs > 0) --g_net_module_refs;
    bool last_ref = (g_net_module_refs == 0);
    net_mutex_unlock(&g_state_mtx);

    if (!last_ref) return;

    net_clear_cookies_and_cache();

    net_mutex_lock(&g_tpool_mtx);
    if (g_net_threadpool) {
        arcr_threadpool_destroy(g_net_threadpool);
        g_net_threadpool = NULL;
    }
    net_mutex_unlock(&g_tpool_mtx);

#if !defined(_WIN32) && defined(ARCR_HAVE_OPENSSL)
    net_mutex_lock(&g_ssl_mtx);
    if (g_shared_ssl_ctx) {
        SSL_CTX_free(g_shared_ssl_ctx);
        g_shared_ssl_ctx = NULL;
    }
    net_mutex_unlock(&g_ssl_mtx);
#endif
}

#if !defined(ARCR_NET_STATIC_LINK)
ARCR_API const ArcrModuleDescriptor *arcr_module_query(void) {
    return arcr_net_module_query();
}

ARCR_API void arcr_module_shutdown(void);
ARCR_API void arcr_module_shutdown(void) {
    arcr_net_module_shutdown_impl();
}
#endif
