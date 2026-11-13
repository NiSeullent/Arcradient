/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * net/arcr_net.h - Incremental HTTP/1.1 Keep-Alive Pool, TLS 1.3 (OpenSSL / Win32 SChannel),
 *                  Proxy CONNECT, Brotli/Gzip, Cookie & Parallel Multi-Core Fetch VTable
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
#define ARCR_NET_MAX_URL_LEN     4096u

typedef struct ArcrParsedUrl {
    char     scheme[16];
    char     host[256];
    uint16_t port;
    char     path[ARCR_NET_MAX_URL_LEN]; /* Includes path + ?query */
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
    const char         *url;
    const char         *method;           /* "GET", "POST", "HEAD", "OPTIONS", etc. */
    const char         *post_body;        /* Optional form/JSON payload */
    size_t              post_body_len;
    const char         *content_type;     /* Default: application/x-www-form-urlencoded */
    const char         *accept;           /* Optional Accept header override */
    const char         *referer;          /* Optional Referer header */
    const char         *origin;           /* Optional CORS Origin header */
    const char         *extra_headers;    /* Optional raw "Header: Val\r\n" lines */
    const volatile int *cancel_flag;      /* Optional cancellation signal (*cancel_flag != 0 aborts I/O) */
    bool                private_browsing; /* Do not read/write persistent cookies when true */
    bool                bypass_cache;
    bool                include_credentials; /* Send/store cookies on cross-origin when true */
} ArcrHttpRequest;

typedef struct ArcrHttpResponse {
    int               status_code;
    char              final_url[ARCR_NET_MAX_URL_LEN];
    char              content_type[256];
    uint8_t          *body;            /* Null-terminated byte buffer (freed via free_response) */
    size_t            body_len;
    size_t            encoded_bytes;   /* Wire payload bytes before decompression */
    size_t            decoded_bytes;   /* Decompressed payload bytes */
    char             *raw_headers;     /* "\r\n"-delimited response headers (freed via free_response) */
    size_t            raw_headers_len;
    ArcrSecurityState security;
    char              detail[256];
    bool              from_cache;
    bool              redirected;
    bool              cancelled;
    bool              length_mismatch;
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
    void (*fetch_batch_parallel)(const ArcrHttpRequest *reqs, ArcrHttpResponse *out_resps,
                                 size_t count);
    void (*free_response)(ArcrHttpResponse *resp);
    void (*clear_cookies_and_cache)(void);
    void (*free_resource)(char *body);
} ArcrNetVTable;

#endif /* ARCR_NET_H */
