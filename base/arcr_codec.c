/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * base/arcr_codec.c - Pure C11 DEFLATE/ZLIB/GZIP/Brotli, ZIP (.wsx) & Modern Image/SVG Codecs
 */
#include "arcr_codec.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARCR_HAVE_MODERN_CODECS)
#include <math.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wvla"
#pragma GCC diagnostic ignored "-Woverflow"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include <brotli/decode.h>
#include <webp/decode.h>

#if !defined(__SSE2__)
#ifndef STBI_NO_SIMD
#define STBI_NO_SIMD
#endif
#ifndef STBIR_NO_SIMD
#define STBIR_NO_SIMD
#endif
#endif

#define STBI_NO_THREAD_LOCALS
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 8192
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#define NANOSVG_ALL_COLOR_KEYWORDS
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif /* ARCR_HAVE_MODERN_CODECS */

/* =========================================================================
 * 1. Checksums: CRC-32 (IEEE 802.3) & Adler-32
 * ========================================================================= */
static uint32_t g_crc32_table[256];
static bool     g_crc32_ready = false;

static void init_crc32_table(void) {
    if (g_crc32_ready) return;
    for (uint32_t i = 0u; i < 256u; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_crc32_table[i] = c;
    }
    g_crc32_ready = true;
}

uint32_t arcr_crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    init_crc32_table();
    if (!data || len == 0u) return crc;
    uint32_t c = ~crc;
    for (size_t i = 0u; i < len; ++i) {
        c = g_crc32_table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return ~c;
}

uint32_t arcr_crc32(const uint8_t *data, size_t len) {
    if (!data || len == 0u) return 0u;
    return arcr_crc32_update(0u, data, len);
}

uint32_t arcr_adler32(const uint8_t *data, size_t len) {
    uint32_t s1 = 1u;
    uint32_t s2 = 0u;
    if (!data) return 1u;
    size_t i = 0u;
    while (i < len) {
        size_t chunk = ARCR_MIN(len - i, 5552u);
        for (size_t k = 0u; k < chunk; ++k) {
            s1 += data[i + k];
            s2 += s1;
        }
        s1 %= 65521u;
        s2 %= 65521u;
        i += chunk;
    }
    return (s2 << 16) | s1;
}

/* =========================================================================
 * 2. RFC 1951 DEFLATE, RFC 1950 ZLIB, RFC 1952 GZIP & RFC 7932 Brotli
 * ========================================================================= */
typedef struct BitReader {
    const uint8_t *src;
    size_t         len;
    size_t         pos;
    uint32_t       bitbuf;
    int            bitcnt;
} BitReader;

static bool br_ensure(BitReader *br, int n) {
    while (br->bitcnt < n) {
        if (br->pos >= br->len) return false;
        br->bitbuf |= ((uint32_t)br->src[br->pos++]) << br->bitcnt;
        br->bitcnt += 8;
    }
    return true;
}

static uint32_t br_read(BitReader *br, int n, bool *ok) {
    if (n == 0) return 0u;
    if (!br_ensure(br, n)) {
        *ok = false;
        return 0u;
    }
    uint32_t val = br->bitbuf & ((1u << n) - 1u);
    br->bitbuf >>= n;
    br->bitcnt -= n;
    return val;
}

typedef struct HuffTable {
    uint16_t counts[16];
    uint16_t symbols[288];
} HuffTable;

static bool build_huff(HuffTable *h, const uint8_t *lengths, size_t num_syms) {
    memset(h->counts, 0, sizeof(h->counts));
    for (size_t i = 0u; i < num_syms; ++i) {
        if (lengths[i] > 15u) return false;
        h->counts[lengths[i]]++;
    }
    if (h->counts[0] == num_syms) {
        return true; /* Empty table */
    }
    int32_t left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= (int32_t)h->counts[len];
        if (left < 0) return false; /* Over-subscribed */
    }
    uint16_t offsets[16];
    offsets[1] = 0u;
    for (int len = 1; len < 15; ++len) {
        offsets[len + 1] = (uint16_t)(offsets[len] + h->counts[len]);
    }
    for (size_t sym = 0u; sym < num_syms; ++sym) {
        uint8_t l = lengths[sym];
        if (l != 0u) {
            h->symbols[offsets[l]++] = (uint16_t)sym;
        }
    }
    return true;
}

static int decode_huff(BitReader *br, const HuffTable *h) {
    int32_t code  = 0;
    int32_t first = 0;
    int32_t index = 0;
    for (int len = 1; len <= 15; ++len) {
        if (!br_ensure(br, 1)) return -1;
        code |= (int32_t)(br->bitbuf & 1u);
        br->bitbuf >>= 1;
        br->bitcnt -= 1;
        int32_t cnt = (int32_t)h->counts[len];
        if (code - cnt < first) {
            return (int)h->symbols[index + (code - first)];
        }
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

typedef struct OutBuf {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    size_t   max_cap;
} OutBuf;

static bool ob_reserve(OutBuf *ob, size_t extra) {
    if (extra > ob->max_cap || ob->len > ob->max_cap - extra) return false;
    size_t needed = ob->len + extra;
    if (needed <= ob->cap) return true;
    if (needed == SIZE_MAX) return false;
    size_t ncap = (ob->cap == 0u) ? 4096u : ob->cap;
    if (ncap > ob->max_cap) ncap = ob->max_cap;
    while (ncap < needed) {
        ncap = (ncap <= ob->max_cap / 2u) ? (ncap * 2u) : ob->max_cap;
        if (ncap < needed) return false;
    }
    /* Allocate +1 sentinel byte so exact-capacity buffers can be NUL-terminated without OOB or overwrite */
    size_t alloc_sz = (ncap < SIZE_MAX) ? (ncap + 1u) : ncap;
    uint8_t *grown = (uint8_t *)realloc(ob->data, alloc_sz);
    if (!grown) return false;
    ob->data = grown;
    ob->cap  = ncap;
    return true;
}

static bool ob_terminate_nul(OutBuf *ob) {
    if (ob->len == SIZE_MAX) return false;
    if (!ob->data) {
        ob->data = (uint8_t *)malloc(1u);
        if (!ob->data) return false;
        ob->cap = 0u;
        ob->data[0] = 0u;
        return true;
    }
    if (ob->len >= ob->cap) {
        uint8_t *grown = (uint8_t *)realloc(ob->data, ob->len + 1u);
        if (!grown) return false;
        ob->data = grown;
    }
    ob->data[ob->len] = 0u;
    return true;
}

static bool inflate_stream(BitReader *br, OutBuf *ob) {
    static const uint16_t kLenBase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t kLenExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t kDistBase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577
    };
    static const uint8_t kDistExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    static const uint8_t kCodeLenOrder[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    bool ok = true;
    uint32_t bfinal = 0u;

    do {
        bfinal = br_read(br, 1, &ok);
        uint32_t btype = br_read(br, 2, &ok);
        if (!ok || btype == 3u) return false;

        if (btype == 0u) {
            /* Rewind any whole prefetched bytes before byte-aligning stored block */
            size_t whole_unused = (size_t)(br->bitcnt / 8);
            if (whole_unused <= br->pos) br->pos -= whole_unused;
            br->bitbuf = 0u;
            br->bitcnt = 0;
            if (br->len - br->pos < 4u) return false;
            uint16_t blen  = (uint16_t)(br->src[br->pos] | (br->src[br->pos + 1u] << 8));
            uint16_t bnlen = (uint16_t)(br->src[br->pos + 2u] | (br->src[br->pos + 3u] << 8));
            br->pos += 4u;
            if (((uint32_t)blen ^ (uint32_t)bnlen) != 0xFFFFu || (size_t)blen > br->len - br->pos) {
                return false;
            }
            if (!ob_reserve(ob, blen)) return false;
            if (blen > 0u) {
                memcpy(ob->data + ob->len, br->src + br->pos, blen);
                ob->len += blen;
                br->pos += blen;
            }
            continue;
        }

        HuffTable lit_huff, dist_huff;
        uint8_t lengths[320];

        if (btype == 1u) {
            for (int i = 0; i <= 143; ++i)   lengths[i] = 8u;
            for (int i = 144; i <= 255; ++i) lengths[i] = 9u;
            for (int i = 256; i <= 279; ++i) lengths[i] = 7u;
            for (int i = 280; i <= 287; ++i) lengths[i] = 8u;
            build_huff(&lit_huff, lengths, 288u);
            for (int i = 0; i < 30; ++i) lengths[i] = 5u;
            build_huff(&dist_huff, lengths, 30u);
        } else {
            uint32_t hlit  = br_read(br, 5, &ok) + 257u;
            uint32_t hdist = br_read(br, 5, &ok) + 1u;
            uint32_t hclen = br_read(br, 4, &ok) + 4u;
            if (!ok || hlit > 286u || hdist > 30u) return false;

            uint8_t clen_lens[19];
            memset(clen_lens, 0, sizeof(clen_lens));
            for (uint32_t i = 0u; i < hclen; ++i) {
                clen_lens[kCodeLenOrder[i]] = (uint8_t)br_read(br, 3, &ok);
            }
            if (!ok) return false;

            HuffTable clen_huff;
            if (!build_huff(&clen_huff, clen_lens, 19u)) return false;

            uint32_t total = hlit + hdist;
            uint32_t idx   = 0u;
            memset(lengths, 0, sizeof(lengths));
            while (idx < total) {
                int sym = decode_huff(br, &clen_huff);
                if (sym < 0) return false;
                if (sym < 16) {
                    lengths[idx++] = (uint8_t)sym;
                } else {
                    uint8_t rep_val = 0u;
                    uint32_t rep_cnt = 0u;
                    if (sym == 16) {
                        if (idx == 0u) return false;
                        rep_val = lengths[idx - 1u];
                        rep_cnt = 3u + br_read(br, 2, &ok);
                    } else if (sym == 17) {
                        rep_cnt = 3u + br_read(br, 3, &ok);
                    } else {
                        rep_cnt = 11u + br_read(br, 7, &ok);
                    }
                    if (!ok || idx + rep_cnt > total) return false;
                    for (uint32_t r = 0u; r < rep_cnt; ++r) {
                        lengths[idx++] = rep_val;
                    }
                }
            }
            if (!build_huff(&lit_huff, lengths, hlit)) return false;
            if (!build_huff(&dist_huff, lengths + hlit, hdist)) return false;
        }

        for (;;) {
            int sym = decode_huff(br, &lit_huff);
            if (sym < 0) return false;
            if (sym < 256) {
                if (!ob_reserve(ob, 1u)) return false;
                ob->data[ob->len++] = (uint8_t)sym;
            } else if (sym == 256) {
                break;
            } else {
                int lidx = sym - 257;
                if (lidx < 0 || lidx >= 29) return false;
                uint32_t match_len = kLenBase[lidx] + br_read(br, kLenExtra[lidx], &ok);
                int dsym = decode_huff(br, &dist_huff);
                if (!ok || dsym < 0 || dsym >= 30) return false;
                uint32_t dist = kDistBase[dsym] + br_read(br, kDistExtra[dsym], &ok);
                if (!ok || dist == 0u || dist > ob->len) return false;
                if (!ob_reserve(ob, match_len)) return false;
                size_t src_off = ob->len - dist;
                for (uint32_t k = 0u; k < match_len; ++k) {
                    ob->data[ob->len++] = ob->data[src_off + k];
                }
            }
        }
    } while (bfinal == 0u);

    return true;
}

bool arcr_deflate_decompress(const uint8_t *src, size_t src_len,
                             uint8_t **out_buf, size_t *out_len, size_t max_out_len) {
    if (!src || !out_buf || !out_len) return false;
    *out_buf = NULL;
    *out_len = 0u;
    size_t cap_limit = (max_out_len > 0u) ? max_out_len : (64u * 1024u * 1024u);
    BitReader br = {src, src_len, 0u, 0u, 0};
    OutBuf    ob = {NULL, 0u, 0u, cap_limit};
    if (!ob_reserve(&ob, ARCR_MIN(cap_limit, 64u))) return false;
    if (!inflate_stream(&br, &ob) || !ob_terminate_nul(&ob)) {
        free(ob.data);
        return false;
    }
    *out_buf = ob.data;
    *out_len = ob.len;
    return true;
}

bool arcr_zlib_decompress(const uint8_t *src, size_t src_len,
                          uint8_t **out_buf, size_t *out_len, size_t max_out_len) {
    if (!src || !out_buf || !out_len || src_len < 6u) return false;
    *out_buf = NULL;
    *out_len = 0u;
    uint8_t cmf = src[0];
    uint8_t flg = src[1];
    if ((cmf & 0x0Fu) != 8u || (cmf >> 4) > 7u) return false;
    if ((((uint32_t)cmf << 8) + flg) % 31u != 0u) return false;
    if (flg & 0x20u) return false; /* Preset dictionary not used in HTTP/PNG */

    size_t cap_limit = (max_out_len > 0u) ? max_out_len : (64u * 1024u * 1024u);
    BitReader br = {src + 2u, src_len - 2u, 0u, 0u, 0};
    OutBuf    ob = {NULL, 0u, 0u, cap_limit};
    if (!ob_reserve(&ob, ARCR_MIN(cap_limit, 64u))) return false;
    if (!inflate_stream(&br, &ob)) {
        free(ob.data);
        return false;
    }

    size_t consumed = 2u + br.pos - (size_t)(br.bitcnt / 8);
    if (src_len < consumed || (src_len - consumed) < 4u) {
        free(ob.data);
        return false;
    }
    uint32_t expected = ((uint32_t)src[consumed] << 24) |
                        ((uint32_t)src[consumed + 1u] << 16) |
                        ((uint32_t)src[consumed + 2u] << 8) |
                        (uint32_t)src[consumed + 3u];
    if (arcr_adler32(ob.data, ob.len) != expected || !ob_terminate_nul(&ob)) {
        free(ob.data);
        return false;
    }
    *out_buf = ob.data;
    *out_len = ob.len;
    return true;
}

bool arcr_gzip_decompress(const uint8_t *src, size_t src_len,
                          uint8_t **out_buf, size_t *out_len, size_t max_out_len) {
    if (!src || !out_buf || !out_len || src_len < 18u) return false;
    *out_buf = NULL;
    *out_len = 0u;
    if (src[0] != 0x1Fu || src[1] != 0x8Bu || src[2] != 8u) return false;

    size_t cap_limit = (max_out_len > 0u) ? max_out_len : (64u * 1024u * 1024u);
    OutBuf ob = {NULL, 0u, 0u, cap_limit};
    if (!ob_reserve(&ob, ARCR_MIN(cap_limit, 64u))) return false;

    size_t pos = 0u;
    while (pos < src_len) {
        if (src_len - pos < 18u || src[pos] != 0x1Fu || src[pos + 1u] != 0x8Bu || src[pos + 2u] != 8u) {
            free(ob.data);
            return false;
        }
        uint8_t flg = src[pos + 3u];
        if ((flg & 0xE0u) != 0u) {
            free(ob.data);
            return false;
        }
        pos += 10u;

        if (flg & 0x04u) { /* FEXTRA */
            if (src_len - pos < 2u) { free(ob.data); return false; }
            size_t xlen = (size_t)src[pos] | ((size_t)src[pos + 1u] << 8);
            pos += 2u;
            if (xlen > src_len - pos) { free(ob.data); return false; }
            pos += xlen;
        }
        if (flg & 0x08u) { /* FNAME */
            while (pos < src_len && src[pos] != 0u) ++pos;
            if (pos >= src_len) { free(ob.data); return false; }
            ++pos;
        }
        if (flg & 0x10u) { /* FCOMMENT */
            while (pos < src_len && src[pos] != 0u) ++pos;
            if (pos >= src_len) { free(ob.data); return false; }
            ++pos;
        }
        if (flg & 0x02u) { /* FHCRC */
            if (src_len - pos < 2u) { free(ob.data); return false; }
            pos += 2u;
        }
        if (src_len - pos < 8u) {
            free(ob.data);
            return false;
        }

        size_t member_out_start = ob.len;
        BitReader br = {src + pos, src_len - pos, 0u, 0u, 0};
        if (!inflate_stream(&br, &ob)) {
            free(ob.data);
            return false;
        }

        size_t consumed_deflate = br.pos - (size_t)(br.bitcnt / 8);
        if (consumed_deflate > src_len - pos || (src_len - pos - consumed_deflate) < 8u) {
            free(ob.data);
            return false;
        }
        size_t trailer = pos + consumed_deflate;
        uint32_t expected_crc = (uint32_t)src[trailer] |
                                ((uint32_t)src[trailer + 1u] << 8) |
                                ((uint32_t)src[trailer + 2u] << 16) |
                                ((uint32_t)src[trailer + 3u] << 24);
        uint32_t expected_isize = (uint32_t)src[trailer + 4u] |
                                  ((uint32_t)src[trailer + 5u] << 8) |
                                  ((uint32_t)src[trailer + 6u] << 16) |
                                  ((uint32_t)src[trailer + 7u] << 24);
        size_t member_out_len = ob.len - member_out_start;
        if (arcr_crc32(ob.data + member_out_start, member_out_len) != expected_crc ||
            (uint32_t)(member_out_len & 0xFFFFFFFFu) != expected_isize) {
            free(ob.data);
            return false;
        }

        pos = trailer + 8u;
        /* RFC 1952 multi-member GZIP: continue if trailing bytes begin another 0x1F 0x8B member */
        while (pos < src_len && (src[pos] == 0u || src[pos] == '\r' || src[pos] == '\n')) {
            ++pos;
        }
        if (pos < src_len && src[pos] == 0x1Fu) {
            continue;
        }
        break;
    }

    if (!ob_terminate_nul(&ob)) {
        free(ob.data);
        return false;
    }
    *out_buf = ob.data;
    *out_len = ob.len;
    return true;
}

bool arcr_brotli_decompress(const uint8_t *src, size_t src_len,
                            uint8_t **out_buf, size_t *out_len, size_t max_out_len) {
    if (!src || !out_buf || !out_len) return false;
    *out_buf = NULL;
    *out_len = 0u;
#if defined(ARCR_HAVE_MODERN_CODECS)
    size_t cap_limit = (max_out_len > 0u) ? max_out_len : (64u * 1024u * 1024u);
    BrotliDecoderState *st = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (!st) return false;
    (void)BrotliDecoderSetParameter(st, BROTLI_DECODER_PARAM_LARGE_WINDOW, 1u);

    OutBuf ob = {NULL, 0u, 0u, cap_limit};
    if (!ob_reserve(&ob, ARCR_MIN(cap_limit, 4096u))) {
        BrotliDecoderDestroyInstance(st);
        return false;
    }

    size_t avail_in = src_len;
    const uint8_t *next_in = src;
    BrotliDecoderResult res = BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT;

    while (res == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
        if (ob.len == ob.cap) {
            size_t rem = ob.max_cap - ob.len;
            if (rem == 0u) {
                BrotliDecoderDestroyInstance(st);
                free(ob.data);
                return false;
            }
            size_t grow = (ob.cap == 0u) ? 4096u : ob.cap;
            if (grow > rem) grow = rem;
            if (!ob_reserve(&ob, grow)) {
                BrotliDecoderDestroyInstance(st);
                free(ob.data);
                return false;
            }
        }
        size_t avail_out = ob.cap - ob.len;
        uint8_t *next_out = ob.data + ob.len;
        res = BrotliDecoderDecompressStream(st, &avail_in, &next_in, &avail_out, &next_out, NULL);
        ob.len = (size_t)(next_out - ob.data);
    }

    BrotliDecoderDestroyInstance(st);
    if (res != BROTLI_DECODER_RESULT_SUCCESS || !ob_terminate_nul(&ob)) {
        free(ob.data);
        return false;
    }
    *out_buf = ob.data;
    *out_len = ob.len;
    return true;
#else
    (void)src_len;
    (void)max_out_len;
    return false;
#endif
}

static bool has_encoding_token(const char *hdr, const char *tok) {
    if (!hdr || !tok) return false;
    size_t tok_len = strlen(tok);
    if (tok_len == 0u) return false;
    const char *p = hdr;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') ++p;
        if (*p == '\0') break;
        const char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != ',' && *p != ';') {
            ++p;
        }
        size_t len = (size_t)(p - start);
        if (len == tok_len && arcr_str_eq_nocase_n(start, len, tok, tok_len)) {
            return true;
        }
        while (*p != '\0' && *p != ',') ++p;
    }
    return false;
}

bool arcr_decompress_http_body(const uint8_t *src, size_t src_len,
                               const char *content_encoding,
                               uint8_t **out_buf, size_t *out_len, size_t max_out_len) {
    if (!src || !out_buf || !out_len) return false;
    *out_buf = NULL;
    *out_len = 0u;

    const char *ce_ptr = content_encoding;
    size_t ce_len = content_encoding ? strlen(content_encoding) : 0u;
    if (ce_ptr) arcr_str_trim_view(&ce_ptr, &ce_len);

    if (ce_len > 0u) {
        if (has_encoding_token(content_encoding, "br")) {
            return arcr_brotli_decompress(src, src_len, out_buf, out_len, max_out_len);
        }
        if (has_encoding_token(content_encoding, "gzip") ||
            has_encoding_token(content_encoding, "x-gzip")) {
            return arcr_gzip_decompress(src, src_len, out_buf, out_len, max_out_len);
        }
        if (has_encoding_token(content_encoding, "deflate")) {
            if (arcr_zlib_decompress(src, src_len, out_buf, out_len, max_out_len)) {
                return true;
            }
            return arcr_deflate_decompress(src, src_len, out_buf, out_len, max_out_len);
        }
        if (has_encoding_token(content_encoding, "identity")) {
            size_t cap_limit = (max_out_len > 0u) ? max_out_len : (64u * 1024u * 1024u);
            if (src_len > cap_limit || src_len == SIZE_MAX) return false;
            uint8_t *copy = (uint8_t *)malloc(src_len + 1u);
            if (!copy) return false;
            if (src_len > 0u) memcpy(copy, src, src_len);
            copy[src_len] = 0u;
            *out_buf = copy;
            *out_len = src_len;
            return true;
        }
        return false;
    }

    /* Unspecified encoding: auto-detect magic / RFC 1952 / RFC 1950 / RFC 1951 */
    if (src_len >= 2u && src[0] == 0x1Fu && src[1] == 0x8Bu) {
        return arcr_gzip_decompress(src, src_len, out_buf, out_len, max_out_len);
    }
    if (arcr_zlib_decompress(src, src_len, out_buf, out_len, max_out_len)) {
        return true;
    }
    return arcr_deflate_decompress(src, src_len, out_buf, out_len, max_out_len);
}

/* =========================================================================
 * 3. Fast Pure C LZ77 + Fixed-Huffman DEFLATE & ZLIB Compressor
 * ========================================================================= */
typedef struct BitWriter {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    uint32_t bitbuf;
    int      bitcnt;
} BitWriter;

static bool bw_grow(BitWriter *bw, size_t extra) {
    if (bw->len + extra <= bw->cap) return true;
    size_t ncap = (bw->cap == 0u) ? 4096u : (bw->cap * 2u);
    while (ncap < bw->len + extra) ncap *= 2u;
    uint8_t *grown = (uint8_t *)realloc(bw->buf, ncap);
    if (!grown) return false;
    bw->buf = grown;
    bw->cap = ncap;
    return true;
}

static bool bw_put_bits(BitWriter *bw, uint32_t bits, int count) {
    bw->bitbuf |= (bits << bw->bitcnt);
    bw->bitcnt += count;
    while (bw->bitcnt >= 8) {
        if (!bw_grow(bw, 1u)) return false;
        bw->buf[bw->len++] = (uint8_t)(bw->bitbuf & 0xFFu);
        bw->bitbuf >>= 8;
        bw->bitcnt -= 8;
    }
    return true;
}

static uint32_t reverse_bits(uint32_t code, int len) {
    uint32_t r = 0u;
    for (int i = 0; i < len; ++i) {
        r = (r << 1) | (code & 1u);
        code >>= 1;
    }
    return r;
}

static bool emit_fixed_sym(BitWriter *bw, uint32_t sym) {
    if (sym <= 143u) return bw_put_bits(bw, reverse_bits(0x30u + sym, 8), 8);
    if (sym <= 255u) return bw_put_bits(bw, reverse_bits(0x190u + (sym - 144u), 9), 9);
    if (sym <= 279u) return bw_put_bits(bw, reverse_bits(sym - 256u, 7), 7);
    return bw_put_bits(bw, reverse_bits(0xC0u + (sym - 280u), 8), 8);
}

static bool emit_match(BitWriter *bw, uint32_t len, uint32_t dist) {
    static const uint16_t kLenBase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t kLenExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t kDistBase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577
    };
    static const uint8_t kDistExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    int lcode = 28;
    for (int i = 0; i < 28; ++i) {
        if (len < kLenBase[i + 1]) { lcode = i; break; }
    }
    if (!emit_fixed_sym(bw, (uint32_t)(257 + lcode))) return false;
    if (kLenExtra[lcode] > 0 && !bw_put_bits(bw, len - kLenBase[lcode], kLenExtra[lcode])) {
        return false;
    }

    int dcode = 29;
    for (int i = 0; i < 29; ++i) {
        if (dist < kDistBase[i + 1]) { dcode = i; break; }
    }
    if (!bw_put_bits(bw, reverse_bits((uint32_t)dcode, 5), 5)) return false;
    if (kDistExtra[dcode] > 0 && !bw_put_bits(bw, dist - kDistBase[dcode], kDistExtra[dcode])) {
        return false;
    }
    return true;
}

bool arcr_deflate_compress(const uint8_t *src, size_t src_len,
                           uint8_t **out_buf, size_t *out_len) {
    if (!out_buf || !out_len) return false;
    BitWriter bw = {NULL, 0u, 0u, 0u, 0};
    if (!bw_grow(&bw, (src_len / 2u) + 64u)) return false;

    /* Single final fixed-Huffman block header: BFINAL=1, BTYPE=01 */
    if (!bw_put_bits(&bw, 1u, 1) || !bw_put_bits(&bw, 1u, 2)) {
        free(bw.buf);
        return false;
    }

    int32_t *htab = (int32_t *)malloc(4096u * sizeof(int32_t));
    if (!htab) {
        free(bw.buf);
        return false;
    }
    for (size_t i = 0u; i < 4096u; ++i) htab[i] = -1;

    size_t i = 0u;
    while (i < src_len) {
        uint32_t best_len  = 0u;
        uint32_t best_dist = 0u;

        if (i + 3u <= src_len) {
            uint32_t h = (((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1u] << 8) | src[i + 2u]);
            h = ((h * 2654435761u) >> 20) & 4095u;
            int32_t cand = htab[h];
            htab[h] = (int32_t)i;

            if (cand >= 0 && (i - (size_t)cand) <= 32768u) {
                size_t max_l = ARCR_MIN((size_t)258u, src_len - i);
                size_t l = 0u;
                while (l < max_l && src[(size_t)cand + l] == src[i + l]) ++l;
                if (l >= 4u) {
                    best_len  = (uint32_t)l;
                    best_dist = (uint32_t)(i - (size_t)cand);
                }
            }
        }

        if (best_len >= 4u) {
            if (!emit_match(&bw, best_len, best_dist)) {
                free(htab);
                free(bw.buf);
                return false;
            }
            for (size_t step = 1u; step < best_len && (i + step + 3u) <= src_len; ++step) {
                uint32_t h2 = (((uint32_t)src[i + step] << 16) |
                               ((uint32_t)src[i + step + 1u] << 8) |
                               src[i + step + 2u]);
                h2 = ((h2 * 2654435761u) >> 20) & 4095u;
                htab[h2] = (int32_t)(i + step);
            }
            i += best_len;
        } else {
            if (!emit_fixed_sym(&bw, src ? src[i] : 0u)) {
                free(htab);
                free(bw.buf);
                return false;
            }
            ++i;
        }
    }

    free(htab);
    if (!emit_fixed_sym(&bw, 256u)) {
        free(bw.buf);
        return false;
    }
    if (bw.bitcnt > 0 && !bw_put_bits(&bw, 0u, 8 - bw.bitcnt)) {
        free(bw.buf);
        return false;
    }

    *out_buf = bw.buf;
    *out_len = bw.len;
    return true;
}

bool arcr_zlib_compress(const uint8_t *src, size_t src_len,
                        uint8_t **out_buf, size_t *out_len) {
    uint8_t *raw = NULL;
    size_t   rlen = 0u;
    if (!arcr_deflate_compress(src, src_len, &raw, &rlen)) return false;

    uint8_t *z = (uint8_t *)malloc(rlen + 6u);
    if (!z) {
        free(raw);
        return false;
    }
    /* ZLIB header 0x78 0x9C (32K window, default compression, 0x789C % 31 == 0) */
    z[0] = 0x78u;
    z[1] = 0x9Cu;
    memcpy(z + 2u, raw, rlen);
    free(raw);

    uint32_t adl = arcr_adler32(src, src_len);
    z[2u + rlen + 0u] = (uint8_t)((adl >> 24) & 0xFFu);
    z[2u + rlen + 1u] = (uint8_t)((adl >> 16) & 0xFFu);
    z[2u + rlen + 2u] = (uint8_t)((adl >> 8) & 0xFFu);
    z[2u + rlen + 3u] = (uint8_t)(adl & 0xFFu);

    *out_buf = z;
    *out_len = rlen + 6u;
    return true;
}

/* =========================================================================
 * 4. Base64 Decoder
 * ========================================================================= */
static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

bool arcr_base64_decode(const char *b64, size_t b64_len,
                        uint8_t **out_buf, size_t *out_len) {
    if (!b64 || !out_buf || !out_len) return false;
    uint8_t *out = (uint8_t *)malloc((b64_len * 3u) / 4u + 4u);
    if (!out) return false;

    size_t   pos  = 0u;
    uint32_t acc  = 0u;
    int      bits = 0;
    for (size_t i = 0u; i < b64_len; ++i) {
        char c = b64[i];
        if (c == '=') break;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        int v = b64_val(c);
        if (v < 0) {
            free(out);
            return false;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[pos++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }
    out[pos] = 0u;
    *out_buf = out;
    *out_len = pos;
    return true;
}

/* =========================================================================
 * 5. Standard PKZIP (.zip / .wsx) Reader & Writer
 * ========================================================================= */
static void put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool arcr_zip_create_mem(const ArcrZipEntryView *entries, size_t entry_count,
                         uint8_t **out_zip, size_t *out_zip_len) {
    if (!out_zip || !out_zip_len || (entry_count > 0u && !entries)) return false;

    typedef struct PackedRec {
        uint8_t *comp;
        size_t   comp_len;
        uint32_t crc;
        uint32_t local_off;
        uint16_t method;
        uint16_t fn_len;
    } PackedRec;

    PackedRec *recs = (PackedRec *)calloc(entry_count > 0u ? entry_count : 1u, sizeof(PackedRec));
    if (!recs) return false;

    size_t total_est = 64u;
    for (size_t i = 0u; i < entry_count; ++i) {
        recs[i].fn_len = (uint16_t)strlen(entries[i].filename);
        recs[i].crc    = arcr_crc32(entries[i].data, entries[i].size);
        if (entries[i].size > 32u &&
            arcr_deflate_compress(entries[i].data, entries[i].size, &recs[i].comp, &recs[i].comp_len) &&
            recs[i].comp_len < entries[i].size) {
            recs[i].method = 8u; /* DEFLATE */
        } else {
            free(recs[i].comp);
            recs[i].comp     = NULL;
            recs[i].comp_len = entries[i].size;
            recs[i].method   = 0u; /* STORE */
        }
        total_est += 30u + recs[i].fn_len + recs[i].comp_len + 46u + recs[i].fn_len;
    }

    uint8_t *buf = (uint8_t *)malloc(total_est);
    if (!buf) {
        for (size_t i = 0u; i < entry_count; ++i) free(recs[i].comp);
        free(recs);
        return false;
    }

    size_t pos = 0u;
    for (size_t i = 0u; i < entry_count; ++i) {
        recs[i].local_off = (uint32_t)pos;
        put_u32le(buf + pos + 0u,  0x04034B50u); /* Local file header signature */
        put_u16le(buf + pos + 4u,  20u);
        put_u16le(buf + pos + 6u,  0x0800u);     /* UTF-8 filename flag */
        put_u16le(buf + pos + 8u,  recs[i].method);
        put_u16le(buf + pos + 10u, 0u);
        put_u16le(buf + pos + 12u, 0u);
        put_u32le(buf + pos + 14u, recs[i].crc);
        put_u32le(buf + pos + 18u, (uint32_t)recs[i].comp_len);
        put_u32le(buf + pos + 22u, (uint32_t)entries[i].size);
        put_u16le(buf + pos + 26u, recs[i].fn_len);
        put_u16le(buf + pos + 28u, 0u);
        pos += 30u;
        memcpy(buf + pos, entries[i].filename, recs[i].fn_len);
        pos += recs[i].fn_len;
        if (recs[i].comp_len > 0u) {
            memcpy(buf + pos, recs[i].method == 8u ? recs[i].comp : entries[i].data, recs[i].comp_len);
            pos += recs[i].comp_len;
        }
        free(recs[i].comp);
    }

    uint32_t cd_off = (uint32_t)pos;
    for (size_t i = 0u; i < entry_count; ++i) {
        put_u32le(buf + pos + 0u,  0x02014B50u); /* Central directory header */
        put_u16le(buf + pos + 4u,  20u);
        put_u16le(buf + pos + 6u,  20u);
        put_u16le(buf + pos + 8u,  0x0800u);
        put_u16le(buf + pos + 10u, recs[i].method);
        put_u16le(buf + pos + 12u, 0u);
        put_u16le(buf + pos + 14u, 0u);
        put_u32le(buf + pos + 16u, recs[i].crc);
        put_u32le(buf + pos + 20u, (uint32_t)recs[i].comp_len);
        put_u32le(buf + pos + 24u, (uint32_t)entries[i].size);
        put_u16le(buf + pos + 28u, recs[i].fn_len);
        put_u16le(buf + pos + 30u, 0u);
        put_u16le(buf + pos + 32u, 0u);
        put_u16le(buf + pos + 34u, 0u);
        put_u16le(buf + pos + 36u, 0u);
        put_u32le(buf + pos + 38u, 0u);
        put_u32le(buf + pos + 42u, recs[i].local_off);
        pos += 46u;
        memcpy(buf + pos, entries[i].filename, recs[i].fn_len);
        pos += recs[i].fn_len;
    }
    uint32_t cd_size = (uint32_t)(pos - cd_off);

    /* End of Central Directory Record (EOCD) */
    put_u32le(buf + pos + 0u,  0x06054B50u);
    put_u16le(buf + pos + 4u,  0u);
    put_u16le(buf + pos + 6u,  0u);
    put_u16le(buf + pos + 8u,  (uint16_t)entry_count);
    put_u16le(buf + pos + 10u, (uint16_t)entry_count);
    put_u32le(buf + pos + 12u, cd_size);
    put_u32le(buf + pos + 16u, cd_off);
    put_u16le(buf + pos + 20u, 0u);
    pos += 22u;

    free(recs);
    *out_zip     = buf;
    *out_zip_len = pos;
    return true;
}

static bool find_zip_eocd(const uint8_t *z, size_t zlen, uint32_t *out_cd_off, uint16_t *out_cnt) {
    if (!z || zlen < 22u) return false;
    size_t min_off = (zlen > 65557u) ? (zlen - 65557u) : 0u;
    for (size_t pos = zlen - 22u + 1u; pos > min_off; --pos) {
        size_t idx = pos - 1u;
        if (get_u32le(z + idx) == 0x06054B50u) {
            *out_cnt    = get_u16le(z + idx + 10u);
            *out_cd_off = get_u32le(z + idx + 16u);
            return (*out_cd_off < zlen);
        }
    }
    return false;
}

size_t arcr_zip_list_entries(const uint8_t *zip_bytes, size_t zip_len,
                             char (*out_names)[128], size_t max_entries) {
    uint32_t cd_off = 0u;
    uint16_t cnt    = 0u;
    if (!find_zip_eocd(zip_bytes, zip_len, &cd_off, &cnt)) return 0u;

    size_t pos = cd_off;
    size_t stored = 0u;
    for (uint16_t i = 0u; i < cnt && zip_len - pos >= 46u; ++i) {
        if (get_u32le(zip_bytes + pos) != 0x02014B50u) break;
        uint16_t fn_len = get_u16le(zip_bytes + pos + 28u);
        uint16_t ex_len = get_u16le(zip_bytes + pos + 30u);
        uint16_t cm_len = get_u16le(zip_bytes + pos + 32u);
        size_t   tail   = (size_t)fn_len + (size_t)ex_len + (size_t)cm_len;
        if ((size_t)fn_len > zip_len - pos - 46u) break;
        if (out_names && stored < max_entries) {
            size_t copy_n = ARCR_MIN((size_t)fn_len, (size_t)127u);
            memcpy(out_names[stored], zip_bytes + pos + 46u, copy_n);
            out_names[stored][copy_n] = '\0';
        }
        ++stored;
        if (tail > zip_len - pos - 46u) break;
        pos += 46u + tail;
    }
    return stored;
}

bool arcr_zip_extract_entry(const uint8_t *zip_bytes, size_t zip_len,
                            const char *target_filename,
                            uint8_t **out_data, size_t *out_size) {
    if (!zip_bytes || !target_filename || !out_data || !out_size) return false;
    uint32_t cd_off = 0u;
    uint16_t cnt    = 0u;
    if (!find_zip_eocd(zip_bytes, zip_len, &cd_off, &cnt)) return false;

    size_t tlen = strlen(target_filename);
    size_t pos  = cd_off;
    for (uint16_t i = 0u; i < cnt && zip_len - pos >= 46u; ++i) {
        if (get_u32le(zip_bytes + pos) != 0x02014B50u) break;
        uint16_t method     = get_u16le(zip_bytes + pos + 10u);
        uint32_t crc_expect = get_u32le(zip_bytes + pos + 16u);
        uint32_t comp_sz    = get_u32le(zip_bytes + pos + 20u);
        uint32_t uncomp_sz  = get_u32le(zip_bytes + pos + 24u);
        uint16_t fn_len     = get_u16le(zip_bytes + pos + 28u);
        uint16_t ex_len     = get_u16le(zip_bytes + pos + 30u);
        uint16_t cm_len     = get_u16le(zip_bytes + pos + 32u);
        uint32_t lhdr_off   = get_u32le(zip_bytes + pos + 42u);
        size_t   tail       = (size_t)fn_len + (size_t)ex_len + (size_t)cm_len;
        if ((size_t)fn_len > zip_len - pos - 46u) break;

        const char *fn_ptr = (const char *)(zip_bytes + pos + 46u);
        if (arcr_str_eq_nocase_n(fn_ptr, fn_len, target_filename, tlen)) {
            if (lhdr_off > zip_len || zip_len - lhdr_off < 30u ||
                get_u32le(zip_bytes + lhdr_off) != 0x04034B50u) {
                return false;
            }
            uint16_t lfn = get_u16le(zip_bytes + lhdr_off + 26u);
            uint16_t lex = get_u16le(zip_bytes + lhdr_off + 28u);
            size_t lskip = 30u + (size_t)lfn + (size_t)lex;
            if (lskip > zip_len - (size_t)lhdr_off) return false;
            size_t data_off = (size_t)lhdr_off + lskip;
            if ((size_t)comp_sz > zip_len - data_off) return false;

            uint8_t *raw = NULL;
            size_t   rsz = 0u;
            size_t   usz = (size_t)uncomp_sz;
            if (method == 0u) {
                if (comp_sz < uncomp_sz || usz == SIZE_MAX) return false;
                raw = (uint8_t *)malloc(usz + 1u);
                if (!raw) return false;
                if (usz > 0u) memcpy(raw, zip_bytes + data_off, usz);
                raw[usz] = 0u;
                rsz = usz;
            } else if (method == 8u) {
                size_t cap_lim = (usz <= SIZE_MAX - 64u) ? (usz + 64u) : usz;
                if (!arcr_deflate_decompress(zip_bytes + data_off, comp_sz, &raw, &rsz, cap_lim)) {
                    return false;
                }
            } else {
                return false;
            }

            if (rsz != uncomp_sz || arcr_crc32(raw, rsz) != crc_expect) {
                free(raw);
                return false;
            }
            *out_data = raw;
            *out_size = rsz;
            return true;
        }
        if (tail > zip_len - pos - 46u) break;
        pos += 46u + tail;
    }
    return false;
}

/* =========================================================================
 * 6. Modern Image, ICO, Animated GIF, Vector SVG & Resize Codecs
 * ========================================================================= */
#define ARCR_IMAGE_MAX_DIM    8192
#define ARCR_IMAGE_MAX_PIXELS (64u * 1024u * 1024u)

static uint32_t get_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_u32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static uint8_t paeth_predictor(int a, int b, int c) {
    int p  = a + b - c;
    int pa = (p > a) ? (p - a) : (a - p);
    int pb = (p > b) ? (p - b) : (b - p);
    int pc = (p > c) ? (p - c) : (c - p);
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    if (pb <= pc) return (uint8_t)b;
    return (uint8_t)c;
}

static bool decode_png(const uint8_t *bytes, size_t len, ArcrImage *out_img) {
    if (len < 33u) return false;
    int32_t  w = 0, h = 0;
    uint8_t  bit_depth = 0, color_type = 0, interlace = 0;
    uint32_t palette[256];
    memset(palette, 0, sizeof(palette));
    for (int i = 0; i < 256; ++i) palette[i] = 0xFF000000u;

    uint8_t *idat_cat = NULL;
    size_t   idat_len = 0u, idat_cap = 0u;

    size_t pos = 8u;
    while (len - pos >= 12u) {
        uint32_t clen  = get_u32be(bytes + pos);
        uint32_t ctype = get_u32be(bytes + pos + 4u);
        if ((size_t)clen > len - pos - 12u) break;
        const uint8_t *cdata = bytes + pos + 8u;

        if (ctype == 0x49484452u && clen >= 13u) { /* IHDR */
            w          = (int32_t)get_u32be(cdata);
            h          = (int32_t)get_u32be(cdata + 4u);
            bit_depth  = cdata[8];
            color_type = cdata[9];
            interlace  = cdata[12];
        } else if (ctype == 0x504C5445u) { /* PLTE */
            size_t entries = ARCR_MIN((size_t)(clen / 3u), (size_t)256u);
            for (size_t i = 0u; i < entries; ++i) {
                palette[i] = 0xFF000000u |
                             ((uint32_t)cdata[i * 3u + 0u] << 16) |
                             ((uint32_t)cdata[i * 3u + 1u] << 8) |
                             (uint32_t)cdata[i * 3u + 2u];
            }
        } else if (ctype == 0x74524E53u) { /* tRNS */
            if (color_type == 3u) {
                size_t entries = ARCR_MIN((size_t)clen, (size_t)256u);
                for (size_t i = 0u; i < entries; ++i) {
                    palette[i] = (palette[i] & 0x00FFFFFFu) | ((uint32_t)cdata[i] << 24);
                }
            }
        } else if (ctype == 0x49444154u && clen > 0u) { /* IDAT */
            if ((size_t)clen > SIZE_MAX - idat_len - 1024u) {
                free(idat_cat);
                return false;
            }
            if (idat_len + clen > idat_cap) {
                size_t dbl  = (idat_cap <= SIZE_MAX / 2u) ? (idat_cap * 2u) : SIZE_MAX;
                size_t ncap = ARCR_MAX(dbl, idat_len + (size_t)clen + 1024u);
                uint8_t *grown = (uint8_t *)realloc(idat_cat, ncap);
                if (!grown) { free(idat_cat); return false; }
                idat_cat = grown;
                idat_cap = ncap;
            }
            memcpy(idat_cat + idat_len, cdata, clen);
            idat_len += clen;
        } else if (ctype == 0x49454E44u) { /* IEND */
            break;
        }
        pos += 12u + (size_t)clen;
    }

    if (w <= 0 || h <= 0 || w > ARCR_IMAGE_MAX_DIM || h > ARCR_IMAGE_MAX_DIM ||
        (uint64_t)w * (uint64_t)h > (uint64_t)ARCR_IMAGE_MAX_PIXELS ||
        bit_depth != 8u || interlace != 0u || !idat_cat) {
        free(idat_cat);
        return false;
    }

    int bpp = 0;
    if      (color_type == 0u) bpp = 1;
    else if (color_type == 2u) bpp = 3;
    else if (color_type == 3u) bpp = 1;
    else if (color_type == 4u) bpp = 2;
    else if (color_type == 6u) bpp = 4;
    else { free(idat_cat); return false; }

    size_t row_bytes  = (size_t)w * (size_t)bpp;
    size_t expect_raw = ((size_t)1u + row_bytes) * (size_t)h;
    uint8_t *raw = NULL;
    size_t   raw_len = 0u;
    bool ok = arcr_zlib_decompress(idat_cat, idat_len, &raw, &raw_len, expect_raw + 256u);
    free(idat_cat);
    if (!ok || raw_len < expect_raw) {
        free(raw);
        return false;
    }

    uint32_t *px = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    uint8_t  *prev_row = (uint8_t *)calloc(row_bytes, 1u);
    uint8_t  *cur_row  = (uint8_t *)malloc(row_bytes);
    if (!px || !prev_row || !cur_row) {
        free(px); free(prev_row); free(cur_row); free(raw);
        return false;
    }

    for (int32_t y = 0; y < h; ++y) {
        const uint8_t *src_line = raw + (size_t)y * (1u + row_bytes);
        uint8_t        filt     = src_line[0];
        const uint8_t *fbytes   = src_line + 1u;

        for (size_t x = 0u; x < row_bytes; ++x) {
            int a = (x >= (size_t)bpp) ? cur_row[x - (size_t)bpp] : 0;
            int b = prev_row[x];
            int c = (x >= (size_t)bpp) ? prev_row[x - (size_t)bpp] : 0;
            uint8_t v = fbytes[x];
            if      (filt == 1u) v = (uint8_t)(v + a);
            else if (filt == 2u) v = (uint8_t)(v + b);
            else if (filt == 3u) v = (uint8_t)(v + ((a + b) >> 1));
            else if (filt == 4u) v = (uint8_t)(v + paeth_predictor(a, b, c));
            cur_row[x] = v;
        }

        uint32_t *dst_row = px + (size_t)y * (size_t)w;
        for (int32_t x = 0; x < w; ++x) {
            if (color_type == 6u) {
                uint32_t r = cur_row[x * 4 + 0], g = cur_row[x * 4 + 1];
                uint32_t b = cur_row[x * 4 + 2], a = cur_row[x * 4 + 3];
                dst_row[x] = (a << 24) | (r << 16) | (g << 8) | b;
            } else if (color_type == 2u) {
                uint32_t r = cur_row[x * 3 + 0], g = cur_row[x * 3 + 1], b = cur_row[x * 3 + 2];
                dst_row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            } else if (color_type == 3u) {
                dst_row[x] = palette[cur_row[x]];
            } else if (color_type == 0u) {
                uint32_t v = cur_row[x];
                dst_row[x] = 0xFF000000u | (v << 16) | (v << 8) | v;
            } else {
                uint32_t v = cur_row[x * 2 + 0], a = cur_row[x * 2 + 1];
                dst_row[x] = (a << 24) | (v << 16) | (v << 8) | v;
            }
        }
        memcpy(prev_row, cur_row, row_bytes);
    }

    free(cur_row);
    free(prev_row);
    free(raw);
    out_img->width  = w;
    out_img->height = h;
    out_img->pixels = px;
    return true;
}

static bool decode_bmp(const uint8_t *bytes, size_t len, ArcrImage *out_img) {
    if (len < 54u || bytes[0] != 'B' || bytes[1] != 'M') return false;
    uint32_t pix_off = get_u32le(bytes + 10u);
    int32_t  w       = (int32_t)get_u32le(bytes + 18u);
    int32_t  raw_h   = (int32_t)get_u32le(bytes + 22u);
    uint16_t bpp     = get_u16le(bytes + 28u);
    uint32_t comp    = get_u32le(bytes + 30u);
    if (raw_h == INT32_MIN || pix_off > len) return false;
    bool top_down    = (raw_h < 0);
    int32_t h        = top_down ? -raw_h : raw_h;
    if (w <= 0 || h <= 0 || w > ARCR_IMAGE_MAX_DIM || h > ARCR_IMAGE_MAX_DIM ||
        (uint64_t)w * (uint64_t)h > (uint64_t)ARCR_IMAGE_MAX_PIXELS ||
        (bpp != 24u && bpp != 32u) || comp != 0u) {
        return false;
    }
    size_t row_stride = (((size_t)w * (bpp / 8u)) + 3u) & ~(size_t)3u;
    if (row_stride > (len - (size_t)pix_off) / (size_t)h) return false;

    uint32_t *px = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!px) return false;
    for (int32_t y = 0; y < h; ++y) {
        int32_t src_y = top_down ? y : (h - 1 - y);
        const uint8_t *srow = bytes + pix_off + (size_t)src_y * row_stride;
        uint32_t      *drow = px + (size_t)y * (size_t)w;
        for (int32_t x = 0; x < w; ++x) {
            if (bpp == 32u) {
                uint32_t b = srow[x * 4 + 0], g = srow[x * 4 + 1];
                uint32_t r = srow[x * 4 + 2], a = srow[x * 4 + 3];
                if (a == 0u) a = 255u;
                drow[x] = (a << 24) | (r << 16) | (g << 8) | b;
            } else {
                uint32_t b = srow[x * 3 + 0], g = srow[x * 3 + 1], r = srow[x * 3 + 2];
                drow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
    out_img->width  = w;
    out_img->height = h;
    out_img->pixels = px;
    return true;
}

#if defined(ARCR_HAVE_MODERN_CODECS)
static bool decode_via_stbi(const uint8_t *bytes, size_t len, ArcrImage *out_img) {
    if (!bytes || len == 0u || len > (size_t)INT_MAX) return false;
    int w = 0, h = 0, comp = 0;
    stbi_uc *rgba = stbi_load_from_memory(bytes, (int)len, &w, &h, &comp, 4);
    if (!rgba) return false;
    if (w <= 0 || h <= 0 || w > ARCR_IMAGE_MAX_DIM || h > ARCR_IMAGE_MAX_DIM ||
        (uint64_t)w * (uint64_t)h > (uint64_t)ARCR_IMAGE_MAX_PIXELS) {
        stbi_image_free(rgba);
        return false;
    }
    size_t count = (size_t)w * (size_t)h;
    uint32_t *px = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!px) {
        stbi_image_free(rgba);
        return false;
    }
    for (size_t i = 0u; i < count; ++i) {
        uint32_t r = rgba[i * 4u + 0u];
        uint32_t g = rgba[i * 4u + 1u];
        uint32_t b = rgba[i * 4u + 2u];
        uint32_t a = rgba[i * 4u + 3u];
        px[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    stbi_image_free(rgba);
    out_img->width  = (int32_t)w;
    out_img->height = (int32_t)h;
    out_img->pixels = px;
    return true;
}
#endif

static bool decode_ico(const uint8_t *bytes, size_t len, ArcrImage *out_img) {
    if (!bytes || len < 22u || !out_img) return false;
    if (bytes[0] != 0u || bytes[1] != 0u || (bytes[2] != 1u && bytes[2] != 2u) || bytes[3] != 0u) {
        return false;
    }
    uint16_t count = get_u16le(bytes + 4u);
    if (count == 0u) return false;

    bool     found     = false;
    uint64_t best_rank = 0u;
    uint32_t best_off  = 0u;
    uint32_t best_size = 0u;

    for (uint16_t i = 0u; i < count; ++i) {
        size_t eoff = 6u + (size_t)i * 16u;
        if (len < eoff || len - eoff < 16u) break;
        uint32_t ew   = (bytes[eoff + 0u] == 0u) ? 256u : (uint32_t)bytes[eoff + 0u];
        uint32_t eh   = (bytes[eoff + 1u] == 0u) ? 256u : (uint32_t)bytes[eoff + 1u];
        uint16_t ebpp = get_u16le(bytes + eoff + 6u);
        uint32_t esz  = get_u32le(bytes + eoff + 8u);
        uint32_t edat = get_u32le(bytes + eoff + 12u);
        if (esz < 16u || edat > len || esz > len - (size_t)edat) continue;
        uint64_t rank = ((uint64_t)ew * (uint64_t)eh) * 64u + (uint64_t)ARCR_MIN(ebpp, 64u);
        if (!found || rank > best_rank) {
            found     = true;
            best_rank = rank;
            best_off  = edat;
            best_size = esz;
        }
    }
    if (!found) return false;

    const uint8_t *edata = bytes + best_off;
    size_t         esize = best_size;
    static const uint8_t kPngSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (esize >= 8u && memcmp(edata, kPngSig, 8u) == 0) {
#if defined(ARCR_HAVE_MODERN_CODECS)
        if (decode_via_stbi(edata, esize, out_img)) return true;
#endif
        return decode_png(edata, esize, out_img);
    }

    /* DIB BITMAPINFOHEADER inside ICO/CUR (height is 2x for XOR + AND masks) */
    if (esize < 40u) return false;
    uint32_t hdr_sz = get_u32le(edata + 0u);
    if (hdr_sz < 40u || hdr_sz > esize) return false;
    int32_t  w        = (int32_t)get_u32le(edata + 4u);
    int32_t  raw_h    = (int32_t)get_u32le(edata + 8u);
    uint16_t bpp      = get_u16le(edata + 14u);
    uint32_t comp     = get_u32le(edata + 16u);
    uint32_t clr_used = get_u32le(edata + 32u);
    if (raw_h == INT32_MIN || comp != 0u) return false;

    bool    top_down = (raw_h < 0);
    int32_t abs_h    = top_down ? -raw_h : raw_h;
    int32_t h        = abs_h / 2;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;
    if (bpp != 1u && bpp != 4u && bpp != 8u && bpp != 24u && bpp != 32u) return false;

    uint32_t palette[256];
    for (int i = 0; i < 256; ++i) palette[i] = 0xFF000000u;

    size_t pal_cnt = 0u;
    if (bpp <= 8u) {
        size_t max_pal = (size_t)1u << bpp;
        pal_cnt = (clr_used > 0u && clr_used <= max_pal) ? (size_t)clr_used : max_pal;
        if (pal_cnt * 4u > esize - (size_t)hdr_sz) return false;
        const uint8_t *pptr = edata + hdr_sz;
        for (size_t i = 0u; i < pal_cnt; ++i) {
            uint32_t b = pptr[i * 4u + 0u];
            uint32_t g = pptr[i * 4u + 1u];
            uint32_t r = pptr[i * 4u + 2u];
            palette[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }

    size_t xor_off    = (size_t)hdr_sz + pal_cnt * 4u;
    size_t xor_stride = ((((size_t)w * (size_t)bpp) + 31u) / 32u) * 4u;
    if (xor_stride > (esize - xor_off) / (size_t)h) return false;
    size_t xor_bytes  = xor_stride * (size_t)h;
    size_t and_off    = xor_off + xor_bytes;
    size_t and_stride = (((size_t)w + 31u) / 32u) * 4u;
    bool   has_and    = (esize - and_off >= and_stride * (size_t)h);

    uint32_t *px = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!px) return false;

    bool any_alpha = false;
    for (int32_t y = 0; y < h; ++y) {
        int32_t src_y = top_down ? y : (h - 1 - y);
        const uint8_t *xrow = edata + xor_off + (size_t)src_y * xor_stride;
        const uint8_t *arow = has_and ? (edata + and_off + (size_t)src_y * and_stride) : NULL;
        uint32_t      *drow = px + (size_t)y * (size_t)w;

        for (int32_t x = 0; x < w; ++x) {
            bool and_transparent = arow ? (((arow[x >> 3] >> (7 - (x & 7))) & 1u) != 0u) : false;
            uint32_t argb = 0xFF000000u;
            if (bpp == 32u) {
                uint32_t b = xrow[x * 4 + 0];
                uint32_t g = xrow[x * 4 + 1];
                uint32_t r = xrow[x * 4 + 2];
                uint32_t a = xrow[x * 4 + 3];
                if (a != 0u) any_alpha = true;
                argb = (a << 24) | (r << 16) | (g << 8) | b;
            } else if (bpp == 24u) {
                uint32_t b = xrow[x * 3 + 0];
                uint32_t g = xrow[x * 3 + 1];
                uint32_t r = xrow[x * 3 + 2];
                argb = (and_transparent ? 0x00000000u : 0xFF000000u) | (r << 16) | (g << 8) | b;
            } else if (bpp == 8u) {
                uint32_t c = palette[xrow[x]];
                argb = and_transparent ? (c & 0x00FFFFFFu) : c;
            } else if (bpp == 4u) {
                uint8_t idx = (x & 1) ? (xrow[x >> 1] & 0x0Fu) : (uint8_t)(xrow[x >> 1] >> 4);
                uint32_t c  = palette[idx];
                argb = and_transparent ? (c & 0x00FFFFFFu) : c;
            } else {
                uint8_t idx = (uint8_t)((xrow[x >> 3] >> (7 - (x & 7))) & 1u);
                uint32_t c  = palette[idx];
                argb = and_transparent ? (c & 0x00FFFFFFu) : c;
            }
            drow[x] = argb;
        }
    }

    /* Legacy 32bpp icons with all-zero alpha channel rely on the 1-bit AND mask */
    if (bpp == 32u && !any_alpha) {
        for (int32_t y = 0; y < h; ++y) {
            int32_t src_y = top_down ? y : (h - 1 - y);
            const uint8_t *arow = has_and ? (edata + and_off + (size_t)src_y * and_stride) : NULL;
            uint32_t      *drow = px + (size_t)y * (size_t)w;
            for (int32_t x = 0; x < w; ++x) {
                bool and_transparent = arow ? (((arow[x >> 3] >> (7 - (x & 7))) & 1u) != 0u) : false;
                drow[x] = (drow[x] & 0x00FFFFFFu) | (and_transparent ? 0x00000000u : 0xFF000000u);
            }
        }
    }

    out_img->width  = w;
    out_img->height = h;
    out_img->pixels = px;
    return true;
}

#if defined(ARCR_HAVE_MODERN_CODECS)
static bool looks_like_svg_mem(const uint8_t *bytes, size_t len) {
    if (!bytes || len < 4u) return false;
    size_t i = 0u;
    if (len >= 3u && bytes[0] == 0xEFu && bytes[1] == 0xBBu && bytes[2] == 0xBFu) {
        i = 3u;
    }
    while (i < len && (bytes[i] == ' ' || bytes[i] == '\t' || bytes[i] == '\r' || bytes[i] == '\n')) {
        ++i;
    }
    if (i >= len || bytes[i] != '<') return false;
    size_t rem = len - i;
    const char *s = (const char *)(bytes + i);
    if (rem >= 4u && arcr_str_eq_nocase_n(s, 4u, "<svg", 4u)) return true;
    if ((rem >= 5u && arcr_str_eq_nocase_n(s, 5u, "<?xml", 5u)) ||
        (rem >= 9u && arcr_str_eq_nocase_n(s, 9u, "<!doctype", 9u)) ||
        (rem >= 4u && memcmp(s, "<!--", 4u) == 0)) {
        size_t scan_lim = ARCR_MIN(rem, (size_t)4096u);
        for (size_t k = 0u; k + 4u <= scan_lim; ++k) {
            if (s[k] == '<' && arcr_str_eq_nocase_n(s + k, 4u, "<svg", 4u)) {
                return true;
            }
        }
    }
    return false;
}
#endif

bool arcr_svg_rasterize_mem(const char *svg_utf8, size_t svg_len,
                            int32_t target_w, int32_t target_h,
                            uint32_t current_color_argb, ArcrImage *out_img) {
    if (!out_img) return false;
    memset(out_img, 0, sizeof(*out_img));
    if (!svg_utf8 || svg_len == 0u) return false;
#if defined(ARCR_HAVE_MODERN_CODECS)
    if (svg_len > 16u * 1024u * 1024u) return false;
    size_t start = 0u;
    if (svg_len >= 3u &&
        (uint8_t)svg_utf8[0] == 0xEFu &&
        (uint8_t)svg_utf8[1] == 0xBBu &&
        (uint8_t)svg_utf8[2] == 0xBFu) {
        start = 3u;
    }
    size_t clean_len = svg_len - start;
    char *mutable_svg = (char *)malloc(clean_len + 1u);
    if (!mutable_svg) return false;

    uint32_t eff_color = (current_color_argb == 0u) ? 0xFF1E293Bu : current_color_argb;
    char hex_color[8];
    snprintf(hex_color, sizeof(hex_color), "#%02X%02X%02X",
             (unsigned)((eff_color >> 16) & 0xFFu),
             (unsigned)((eff_color >> 8) & 0xFFu),
             (unsigned)(eff_color & 0xFFu));

    /* Copy into mutable buffer while substituting CSS `currentColor` -> `#RRGGBB` */
    size_t wpos = 0u;
    size_t rpos = start;
    while (rpos < svg_len) {
        if (svg_len - rpos >= 12u &&
            (svg_utf8[rpos] == 'c' || svg_utf8[rpos] == 'C') &&
            arcr_str_eq_nocase_n(svg_utf8 + rpos, 12u, "currentColor", 12u)) {
            memcpy(mutable_svg + wpos, hex_color, 7u);
            wpos += 7u;
            rpos += 12u;
        } else {
            mutable_svg[wpos++] = svg_utf8[rpos++];
        }
    }
    mutable_svg[wpos] = '\0';

    NSVGimage *svg = nsvgParse(mutable_svg, "px", 96.0f);
    free(mutable_svg);
    if (!svg) return false;

    float svg_w = (svg->width > 0.0f) ? svg->width : 24.0f;
    float svg_h = (svg->height > 0.0f) ? svg->height : 24.0f;

    int32_t dst_w = target_w;
    int32_t dst_h = target_h;
    if (dst_w <= 0 && dst_h <= 0) {
        dst_w = (int32_t)(svg_w + 0.5f);
        dst_h = (int32_t)(svg_h + 0.5f);
        if (dst_w <= 0) dst_w = 24;
        if (dst_h <= 0) dst_h = 24;
    } else if (dst_w > 0 && dst_h <= 0) {
        dst_h = (int32_t)((float)dst_w * (svg_h / svg_w) + 0.5f);
    } else if (dst_h > 0 && dst_w <= 0) {
        dst_w = (int32_t)((float)dst_h * (svg_w / svg_h) + 0.5f);
    }
    dst_w = ARCR_CLAMP(dst_w, 1, 2048);
    dst_h = ARCR_CLAMP(dst_h, 1, 2048);

    float scale = fminf((float)dst_w / svg_w, (float)dst_h / svg_h);
    if (!(scale > 0.0f)) scale = 1.0f;
    float tx = ((float)dst_w - svg_w * scale) * 0.5f;
    float ty = ((float)dst_h - svg_h * scale) * 0.5f;

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(svg);
        return false;
    }

    size_t px_count = (size_t)dst_w * (size_t)dst_h;
    uint8_t  *rgba = (uint8_t *)calloc(px_count, 4u);
    uint32_t *argb = (uint32_t *)malloc(px_count * sizeof(uint32_t));
    if (!rgba || !argb) {
        free(rgba);
        free(argb);
        nsvgDeleteRasterizer(rast);
        nsvgDelete(svg);
        return false;
    }

    nsvgRasterize(rast, svg, tx, ty, scale, rgba, (int)dst_w, (int)dst_h, (int)dst_w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);

    for (size_t i = 0u; i < px_count; ++i) {
        uint32_t r = rgba[i * 4u + 0u];
        uint32_t g = rgba[i * 4u + 1u];
        uint32_t b = rgba[i * 4u + 2u];
        uint32_t a = rgba[i * 4u + 3u];
        argb[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    free(rgba);

    out_img->width  = dst_w;
    out_img->height = dst_h;
    out_img->pixels = argb;
    return true;
#else
    (void)target_w;
    (void)target_h;
    (void)current_color_argb;
    return false;
#endif
}

bool arcr_image_decode_mem(const uint8_t *bytes, size_t len, ArcrImage *out_img) {
    if (!bytes || len < 4u || !out_img) return false;
    memset(out_img, 0, sizeof(*out_img));

#if defined(ARCR_HAVE_MODERN_CODECS)
    /* 1. WebP (VP8 / VP8L / VP8X + Alpha) */
    if (len >= 12u && memcmp(bytes, "RIFF", 4u) == 0 && memcmp(bytes + 8u, "WEBP", 4u) == 0) {
        int w = 0, h = 0;
        if (!WebPGetInfo(bytes, len, &w, &h)) return false;
        if (w <= 0 || h <= 0 || w > ARCR_IMAGE_MAX_DIM || h > ARCR_IMAGE_MAX_DIM ||
            (uint64_t)w * (uint64_t)h > (uint64_t)ARCR_IMAGE_MAX_PIXELS) {
            return false;
        }
        uint8_t *bgra = WebPDecodeBGRA(bytes, len, &w, &h);
        if (!bgra || w <= 0 || h <= 0) {
            if (bgra) WebPFree(bgra);
            return false;
        }
        size_t count = (size_t)w * (size_t)h;
        uint32_t *px = (uint32_t *)malloc(count * sizeof(uint32_t));
        if (!px) {
            WebPFree(bgra);
            return false;
        }
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        for (size_t i = 0u; i < count; ++i) {
            uint32_t b = bgra[i * 4u + 0u];
            uint32_t g = bgra[i * 4u + 1u];
            uint32_t r = bgra[i * 4u + 2u];
            uint32_t a = bgra[i * 4u + 3u];
            px[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
#else
        /* Little-endian x86/x64: BGRA bytes [B, G, R, A] ARE exact uint32_t 0xAARRGGBB */
        memcpy(px, bgra, count * sizeof(uint32_t));
#endif
        WebPFree(bgra);
        out_img->width  = (int32_t)w;
        out_img->height = (int32_t)h;
        out_img->pixels = px;
        return true;
    }

    /* 2. Vector SVG */
    if (looks_like_svg_mem(bytes, len)) {
        return arcr_svg_rasterize_mem((const char *)bytes, len, 0, 0, 0xFF1E293Bu, out_img);
    }
#endif

    /* 3. ICO / CUR multi-entry icon directory */
    if (len >= 6u && bytes[0] == 0u && bytes[1] == 0u &&
        (bytes[2] == 1u || bytes[2] == 2u) && bytes[3] == 0u) {
        if (decode_ico(bytes, len, out_img)) return true;
    }

#if defined(ARCR_HAVE_MODERN_CODECS)
    /* 4. JPEG (baseline + progressive), PNG (1/2/4/8/16-bit + Adam7 + tRNS), GIF, BMP, TGA */
    if (decode_via_stbi(bytes, len, out_img)) {
        return true;
    }
#endif

    /* Built-in pure C PNG & BMP decoders (host tools + fallback) */
    static const uint8_t kPngSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (len >= 8u && memcmp(bytes, kPngSig, 8u) == 0) {
        return decode_png(bytes, len, out_img);
    }
    if (len >= 2u && bytes[0] == 'B' && bytes[1] == 'M') {
        return decode_bmp(bytes, len, out_img);
    }
    return false;
}

static int hex_nibble_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool arcr_image_decode_data_uri(const char *uri, size_t uri_len, ArcrImage *out_img) {
    if (!uri || !out_img || uri_len < 12u) return false;
    memset(out_img, 0, sizeof(*out_img));
    if (!arcr_str_eq_nocase_n(uri, 11u, "data:image/", 11u)) return false;

    const char *comma = NULL;
    for (size_t i = 11u; i < uri_len; ++i) {
        if (uri[i] == ',') { comma = uri + i; break; }
    }
    if (!comma) return false;

    size_t hdr_len     = (size_t)(comma - uri);
    size_t payload_len = uri_len - hdr_len - 1u;
    const char *payload = comma + 1u;

    bool is_b64 = false;
    for (size_t i = 11u; i + 7u <= hdr_len; ++i) {
        if (arcr_str_eq_nocase_n(uri + i, 7u, ";base64", 7u)) {
            is_b64 = true;
            break;
        }
    }
    bool is_svg_mime = (hdr_len >= 18u && arcr_str_eq_nocase_n(uri, 18u, "data:image/svg+xml", 18u));

    if (is_b64) {
        uint8_t *raw = NULL;
        size_t   rlen = 0u;
        if (!arcr_base64_decode(payload, payload_len, &raw, &rlen)) return false;
        bool ok = is_svg_mime
                      ? arcr_svg_rasterize_mem((const char *)raw, rlen, 0, 0, 0xFF1E293Bu, out_img)
                      : arcr_image_decode_mem(raw, rlen, out_img);
        free(raw);
        return ok;
    }

    /* Percent-encoded / plain UTF-8 data URI (e.g. data:image/svg+xml,<svg...>) */
    if (payload_len > 16u * 1024u * 1024u) return false;
    uint8_t *decoded = (uint8_t *)malloc(payload_len + 1u);
    if (!decoded) return false;

    size_t dlen = 0u;
    for (size_t i = 0u; i < payload_len; ++i) {
        if (payload[i] == '%' && i + 2u < payload_len) {
            int hi = hex_nibble_val(payload[i + 1u]);
            int lo = hex_nibble_val(payload[i + 2u]);
            if (hi >= 0 && lo >= 0) {
                decoded[dlen++] = (uint8_t)((hi << 4) | lo);
                i += 2u;
                continue;
            }
        }
        decoded[dlen++] = (uint8_t)payload[i];
    }
    decoded[dlen] = 0u;

    bool ok = false;
    if (is_svg_mime) {
        ok = arcr_svg_rasterize_mem((const char *)decoded, dlen, 0, 0, 0xFF1E293Bu, out_img);
    }
    if (!ok) {
        ok = arcr_image_decode_mem(decoded, dlen, out_img);
    }
    free(decoded);
    return ok;
}

void arcr_image_free(ArcrImage *img) {
    if (!img) return;
    free(img->pixels);
    img->pixels = NULL;
    img->width  = 0;
    img->height = 0;
}

bool arcr_image_decode_anim_mem(const uint8_t *bytes, size_t len, ArcrAnimImage *out_anim) {
    if (!out_anim) return false;
    memset(out_anim, 0, sizeof(*out_anim));
    if (!bytes || len < 6u) return false;
#if defined(ARCR_HAVE_MODERN_CODECS)
    if (len > (size_t)INT_MAX) return false;
    if (memcmp(bytes, "GIF87a", 6u) != 0 && memcmp(bytes, "GIF89a", 6u) != 0) {
        return false;
    }

    int *raw_delays = NULL;
    int  w = 0, h = 0, z = 0, comp = 0;
    stbi_uc *rgba = stbi_load_gif_from_memory(bytes, (int)len, &raw_delays, &w, &h, &z, &comp, 4);
    if (!rgba || w <= 0 || h <= 0 || z <= 0 ||
        w > ARCR_IMAGE_MAX_DIM || h > ARCR_IMAGE_MAX_DIM) {
        if (rgba) stbi_image_free(rgba);
        if (raw_delays) stbi_image_free(raw_delays);
        return false;
    }

    int32_t frame_cnt = (z > 120) ? 120 : (int32_t)z;
    uint64_t frame_px = (uint64_t)w * (uint64_t)h;
    if (frame_px == 0u || frame_px * (uint64_t)frame_cnt > (uint64_t)ARCR_IMAGE_MAX_PIXELS) {
        if (frame_px > (uint64_t)ARCR_IMAGE_MAX_PIXELS) {
            stbi_image_free(rgba);
            if (raw_delays) stbi_image_free(raw_delays);
            return false;
        }
        frame_cnt = (int32_t)((uint64_t)ARCR_IMAGE_MAX_PIXELS / frame_px);
        if (frame_cnt <= 0) frame_cnt = 1;
    }

    size_t total_px = (size_t)frame_px * (size_t)frame_cnt;
    uint32_t *frames_argb = (uint32_t *)malloc(total_px * sizeof(uint32_t));
    int32_t  *delays_ms   = (int32_t *)malloc((size_t)frame_cnt * sizeof(int32_t));
    if (!frames_argb || !delays_ms) {
        free(frames_argb);
        free(delays_ms);
        stbi_image_free(rgba);
        if (raw_delays) stbi_image_free(raw_delays);
        return false;
    }

    for (size_t i = 0u; i < total_px; ++i) {
        uint32_t r = rgba[i * 4u + 0u];
        uint32_t g = rgba[i * 4u + 1u];
        uint32_t b = rgba[i * 4u + 2u];
        uint32_t a = rgba[i * 4u + 3u];
        frames_argb[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    for (int32_t f = 0; f < frame_cnt; ++f) {
        int32_t d = raw_delays ? (int32_t)raw_delays[f] : 100;
        delays_ms[f] = (d <= 10) ? 100 : d;
    }

    stbi_image_free(rgba);
    if (raw_delays) stbi_image_free(raw_delays);

    out_anim->width       = (int32_t)w;
    out_anim->height      = (int32_t)h;
    out_anim->frame_count = frame_cnt;
    out_anim->frames_argb = frames_argb;
    out_anim->delays_ms   = delays_ms;
    return true;
#else
    return false;
#endif
}

void arcr_image_free_anim(ArcrAnimImage *anim) {
    if (!anim) return;
    free(anim->frames_argb);
    free(anim->delays_ms);
    anim->frames_argb = NULL;
    anim->delays_ms   = NULL;
    anim->width       = 0;
    anim->height      = 0;
    anim->frame_count = 0;
}

bool arcr_image_resize_argb(const uint32_t *src_argb, int32_t src_w, int32_t src_h,
                            int32_t dst_w, int32_t dst_h, ArcrImage *out_img) {
    if (!out_img) return false;
    memset(out_img, 0, sizeof(*out_img));
    if (!src_argb || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return false;
    if (src_w > ARCR_IMAGE_MAX_DIM || src_h > ARCR_IMAGE_MAX_DIM ||
        dst_w > ARCR_IMAGE_MAX_DIM || dst_h > ARCR_IMAGE_MAX_DIM ||
        (uint64_t)dst_w * (uint64_t)dst_h > (uint64_t)ARCR_IMAGE_MAX_PIXELS) {
        return false;
    }

    size_t dst_count = (size_t)dst_w * (size_t)dst_h;
    uint32_t *dst_px = (uint32_t *)malloc(dst_count * sizeof(uint32_t));
    if (!dst_px) return false;

    if (src_w == dst_w && src_h == dst_h) {
        memcpy(dst_px, src_argb, dst_count * sizeof(uint32_t));
        out_img->width  = dst_w;
        out_img->height = dst_h;
        out_img->pixels = dst_px;
        return true;
    }

#if defined(ARCR_HAVE_MODERN_CODECS)
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    const stbir_pixel_layout layout = STBIR_ARGB;
#else
    const stbir_pixel_layout layout = STBIR_BGRA;
#endif
    unsigned char *res = stbir_resize_uint8_srgb(
        (const unsigned char *)src_argb, (int)src_w, (int)src_h, (int)src_w * 4,
        (unsigned char *)dst_px, (int)dst_w, (int)dst_h, (int)dst_w * 4,
        layout
    );
    if (!res) {
        free(dst_px);
        return false;
    }
#else
    /* Pure integer bilinear fallback for host CLI tools built without libm */
    for (int32_t y = 0; y < dst_h; ++y) {
        int64_t sy_fp = (((int64_t)y * 2 + 1) * (int64_t)src_h * 128) / (int64_t)dst_h - 128;
        if (sy_fp < 0) sy_fp = 0;
        int32_t y0 = (int32_t)(sy_fp >> 8);
        uint32_t fy = (uint32_t)(sy_fp & 255);
        if (y0 >= src_h - 1) { y0 = src_h - 1; fy = 0u; }
        int32_t y1 = (y0 + 1 < src_h) ? (y0 + 1) : y0;
        const uint32_t *r0 = src_argb + (size_t)y0 * (size_t)src_w;
        const uint32_t *r1 = src_argb + (size_t)y1 * (size_t)src_w;

        for (int32_t x = 0; x < dst_w; ++x) {
            int64_t sx_fp = (((int64_t)x * 2 + 1) * (int64_t)src_w * 128) / (int64_t)dst_w - 128;
            if (sx_fp < 0) sx_fp = 0;
            int32_t x0 = (int32_t)(sx_fp >> 8);
            uint32_t fx = (uint32_t)(sx_fp & 255);
            if (x0 >= src_w - 1) { x0 = src_w - 1; fx = 0u; }
            int32_t x1 = (x0 + 1 < src_w) ? (x0 + 1) : x0;

            uint32_t p00 = r0[x0], p10 = r0[x1], p01 = r1[x0], p11 = r1[x1];
            uint32_t w00 = (256u - fx) * (256u - fy);
            uint32_t w10 = fx * (256u - fy);
            uint32_t w01 = (256u - fx) * fy;
            uint32_t w11 = fx * fy;

            uint32_t a = (((p00 >> 24) & 0xFFu) * w00 + ((p10 >> 24) & 0xFFu) * w10 +
                          ((p01 >> 24) & 0xFFu) * w01 + ((p11 >> 24) & 0xFFu) * w11 + 32768u) >> 16;
            uint32_t r = (((p00 >> 16) & 0xFFu) * w00 + ((p10 >> 16) & 0xFFu) * w10 +
                          ((p01 >> 16) & 0xFFu) * w01 + ((p11 >> 16) & 0xFFu) * w11 + 32768u) >> 16;
            uint32_t g = (((p00 >> 8) & 0xFFu) * w00 + ((p10 >> 8) & 0xFFu) * w10 +
                          ((p01 >> 8) & 0xFFu) * w01 + ((p11 >> 8) & 0xFFu) * w11 + 32768u) >> 16;
            uint32_t b = ((p00 & 0xFFu) * w00 + (p10 & 0xFFu) * w10 +
                          (p01 & 0xFFu) * w01 + (p11 & 0xFFu) * w11 + 32768u) >> 16;
            dst_px[(size_t)y * (size_t)dst_w + (size_t)x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
#endif

    out_img->width  = dst_w;
    out_img->height = dst_h;
    out_img->pixels = dst_px;
    return true;
}

static void write_png_chunk(uint8_t **p, const char type[4], const uint8_t *data, uint32_t len) {
    uint8_t *cur = *p;
    put_u32be(cur, len);
    memcpy(cur + 4u, type, 4u);
    if (len > 0u && data) memcpy(cur + 8u, data, len);
    uint32_t crc = arcr_crc32_update(0u, cur + 4u, 4u + (size_t)len);
    put_u32be(cur + 8u + len, crc);
    *p = cur + 12u + len;
}

bool arcr_image_encode_png_mem(const uint32_t *argb_pixels, int32_t width, int32_t height,
                               int32_t stride_pixels, uint8_t **out_png, size_t *out_len) {
    if (!argb_pixels || width <= 0 || height <= 0 || !out_png || !out_len) return false;
    size_t row_bytes = 1u + (size_t)width * 4u;
    size_t raw_size  = row_bytes * (size_t)height;
    uint8_t *raw = (uint8_t *)malloc(raw_size);
    if (!raw) return false;

    /* Filter 1 (Sub) yields dramatic compression ratios on solid UI chrome & glyphs! */
    for (int32_t y = 0; y < height; ++y) {
        const uint32_t *src = argb_pixels + (size_t)y * (size_t)stride_pixels;
        uint8_t *dst = raw + (size_t)y * row_bytes;
        dst[0] = 1u; /* Sub filter */
        uint8_t pr = 0u, pg = 0u, pb = 0u, pa = 0u;
        for (int32_t x = 0; x < width; ++x) {
            uint32_t px = src[x];
            uint8_t r = (uint8_t)((px >> 16) & 0xFFu);
            uint8_t g = (uint8_t)((px >> 8) & 0xFFu);
            uint8_t b = (uint8_t)(px & 0xFFu);
            uint8_t a = (uint8_t)((px >> 24) & 0xFFu);
            dst[1 + x * 4 + 0] = (uint8_t)(r - pr);
            dst[1 + x * 4 + 1] = (uint8_t)(g - pg);
            dst[1 + x * 4 + 2] = (uint8_t)(b - pb);
            dst[1 + x * 4 + 3] = (uint8_t)(a - pa);
            pr = r; pg = g; pb = b; pa = a;
        }
    }

    uint8_t *zdata = NULL;
    size_t   zlen  = 0u;
    bool ok = arcr_zlib_compress(raw, raw_size, &zdata, &zlen);
    free(raw);
    if (!ok) return false;

    size_t total = 8u + (12u + 13u) + (12u + zlen) + 12u;
    uint8_t *png = (uint8_t *)malloc(total);
    if (!png) {
        free(zdata);
        return false;
    }

    static const uint8_t kSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    memcpy(png, kSig, 8u);
    uint8_t *ptr = png + 8u;

    uint8_t ihdr[13];
    put_u32be(ihdr + 0u, (uint32_t)width);
    put_u32be(ihdr + 4u, (uint32_t)height);
    ihdr[8]  = 8u; /* 8-bit */
    ihdr[9]  = 6u; /* RGBA */
    ihdr[10] = 0u;
    ihdr[11] = 0u;
    ihdr[12] = 0u;

    write_png_chunk(&ptr, "IHDR", ihdr, 13u);
    write_png_chunk(&ptr, "IDAT", zdata, (uint32_t)zlen);
    write_png_chunk(&ptr, "IEND", NULL, 0u);
    free(zdata);

    *out_png = png;
    *out_len = (size_t)(ptr - png);
    return true;
}

bool arcr_image_write_png_file(const char *filepath, const uint32_t *argb_pixels,
                               int32_t width, int32_t height, int32_t stride_pixels) {
    uint8_t *png = NULL;
    size_t   len = 0u;
    if (!arcr_image_encode_png_mem(argb_pixels, width, height, stride_pixels, &png, &len)) {
        return false;
    }
    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        free(png);
        return false;
    }
    size_t wr = fwrite(png, 1u, len, fp);
    fclose(fp);
    free(png);
    return (wr == len);
}
