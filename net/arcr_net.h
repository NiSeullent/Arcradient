/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * net/arcr_net.h - HTTP/1.1 + HTTPS (OpenSSL / Win32 SChannel), Gzip, Cookie & Cache VTable
 */
#ifndef ARCR_NET_H
#define ARCR_NET_H

#include "../include/arcr_module.h"
#include "../include/arcradient_embed.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 2025-01-01T00:00:00Z epoch guard for CMOS battery reset detection on retro PCs */
#define ARCR_BUILD_MIN_EPOCH_SEC 1735689600LL

typedef struct ArcrParsedUrl {
    char     scheme[16];
    char     host[192];
    uint16_t port;
    char     path[512]; /* Includes path + ?query */
} ArcrParsedUrl;

typedef struct ArcrCertCheckInput {
    const char *expected_host;
    const char *cert_san_dns;
    bool        trusted_root_signature_ok;
    int64_t     not_before_epoch;
    int64_t     not_after_epoch;
    int64_t     system_wall_epoch;
} ArcrCertCheckInput;

typedef struct ArcrHttpRequest {
    const char *url;
    const char *method;           /* "GET" or "POST" */
    const char *post_body;        /* Optional form/JSON payload */
    size_t      post_body_len;
    const char *content_type;     /* Default: application/x-www-form-urlencoded */
    bool        private_browsing; /* Do not read/write persistent cookies when true */
    bool        bypass_cache;
} ArcrHttpRequest;

typedef struct ArcrHttpResponse {
    int               status_code;
    char              final_url[512];
    char              content_type[128];
    uint8_t          *body;       /* Null-terminated byte buffer (caller frees via free_resource) */
    size_t            body_len;
    ArcrSecurityState security;
    char              detail[256];
    bool              from_cache;
} ArcrHttpResponse;

typedef struct ArcrNetVTable {
    uint32_t struct_size;
    bool (*parse_url)(const char *raw_url, ArcrParsedUrl *out_parsed);
    bool (*resolve_url)(const char *base_url, const char *rel_url,
                        char *out_resolved, size_t out_cap);
    ArcrSecurityState (*verify_tls_trust)(const ArcrCertCheckInput *in,
                                          char *out_reason, size_t reason_cap);
    bool (*load_url_resource)(const char *url, char **out_utf8_body,
                              size_t *out_len, ArcrSecurityState *out_sec,
                              char *out_detail, size_t detail_cap);
    bool (*fetch_http)(const ArcrHttpRequest *req, ArcrHttpResponse *out_resp);
    void (*clear_cookies_and_cache)(void);
    void (*free_resource)(char *body);
} ArcrNetVTable;

#endif /* ARCR_NET_H */
