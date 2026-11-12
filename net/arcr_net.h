/*
 * Arcradient (MSHTML-Next)
 * net/arcr_net.h - Arcradient-Net C VTable (URL, Root CA Trust & Clock-Skew Guard)
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
    char     host[128];
    uint16_t port;
    char     path[256];
} ArcrParsedUrl;

typedef struct ArcrCertCheckInput {
    const char *expected_host;
    const char *cert_san_dns;
    bool        trusted_root_signature_ok;
    int64_t     not_before_epoch;
    int64_t     not_after_epoch;
    int64_t     system_wall_epoch;
} ArcrCertCheckInput;

typedef struct ArcrNetVTable {
    uint32_t struct_size;
    bool (*parse_url)(const char *raw_url, ArcrParsedUrl *out_parsed);
    ArcrSecurityState (*verify_tls_trust)(const ArcrCertCheckInput *in,
                                          char *out_reason, size_t reason_cap);
    bool (*load_url_resource)(const char *url, char **out_utf8_body,
                              size_t *out_len, ArcrSecurityState *out_sec,
                              char *out_detail, size_t detail_cap);
    void (*free_resource)(char *body);
} ArcrNetVTable;

#endif /* ARCR_NET_H */
