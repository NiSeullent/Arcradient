/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * base/arcr_codec.c - Pure C11 DEFLATE/ZLIB/GZIP, ZIP (.wsx) & PNG/BMP Codecs
 */
#include "arcr_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 * 2. RFC 1951 DEFLATE Inflate (Decompressor)
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
    if (ob->len + extra > ob->max_cap) return false;
    if (ob->len + extra <= ob->cap) return true;
    size_t ncap = (ob->cap == 0u) ? 4096u : ob->cap;
    while (ncap < ob->len + extra) {
        ncap = (ncap < ob->max_cap / 2u) ? (ncap * 2u) : ob->max_cap;
        if (ncap < ob->len + extra) return false;
    }
    uint8_t *grown = (uint8_t *)realloc(ob->data, ncap);
    if (!grown) return false;
    ob->data = grown;
    ob->cap  = ncap;
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
            /* Stored uncompressed block */
            br->bitbuf = 0u;
            br->bitcnt = 0;
            if (br->pos + 4u > br->len) return false;
            uint16_t blen  = (uint16_t)(br->src[br->pos] | (br->src[br->pos + 1u] << 8));
            uint16_t bnlen = (uint16_t)(br->src[br->pos + 2u] | (br->src[br->pos + 3u] << 8));
            br->pos += 4u;
            if (((uint32_t)blen ^ (uint32_t)bnlen) != 0xFFFFu || br->pos + blen > br->len) return false;
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
    BitReader br = {src, src_len, 0u, 0u, 0};
    OutBuf    ob = {NULL, 0u, 0u, (max_out_len > 0u) ? max_out_len : (64u * 1024u * 1024u)};
    if (!ob_reserve(&ob, 64u)) return false;
    if (!inflate_stream(&br, &ob)) {
        free(ob.data);
        return false;
    }
    ob.data[ob.len < ob.cap ? ob.len : (ob.cap - 1u)] = 0u;
    *out_buf = ob.data;
    *out_len = ob.len;
    return true;
}

bool arcr_zlib_decompress(const uint8_t *src, size_t src_len,
                          uint8_t **out_buf, size_t *out_len, size_t max_out_len) {
    if (!src || src_len < 6u) return false;
    uint8_t cmf = src[0];
    uint8_t flg = src[1];
    if ((cmf & 0x0Fu) != 8u || (cmf >> 4) > 7u) return false;
    if ((((uint32_t)cmf << 8) + flg) % 31u != 0u) return false;
    if (flg & 0x20u) return false; /* Preset dictionary not used in HTTP/PNG */

    BitReader br = {src + 2u, src_len - 2u, 0u, 0u, 0};
    OutBuf    ob = {NULL, 0u, 0u, (max_out_len > 0u) ? max_out_len : (64u * 1024u * 1024u)};
    if (!ob_reserve(&ob, 64u)) return false;
    if (!inflate_stream(&br, &ob)) {
        free(ob.data);
        return false;
    }
    /* Verify trailing big-endian Adler-32 if 4 bytes remain */
    size_t consumed = 2u + br.pos - (size_t)(br.bitcnt / 8);
    if (consumed + 4u <= src_len) {
        uint32_t expected = ((uint32_t)src[consumed] << 24) |
                            ((uint32_t)src[consumed + 1u] << 16) |
                            ((uint32_t)src[consumed + 2u] << 8) |
                            (uint32_t)src[consumed + 3u];
        if (arcr_adler32(ob.data, ob.len) != expected) {
            free(ob.data);
            return false;
        }
    }
    if (ob_reserve(&ob, 1u)) ob.data[ob.len] = 0u;
    *out_buf = ob.data;
    *out_len = ob.len;
    return true;
}

bool arcr_gzip_decompress(const uint8_t *src, size_t src_len,
                          uint8_t **out_buf, size_t *out_len, size_t max_out_len) {
    if (!src || src_len < 18u) return false;
    if (src[0] != 0x1Fu || src[1] != 0x8Bu || src[2] != 8u) return false;
    uint8_t flg = src[3];
    size_t pos = 10u;

    if (flg & 0x04u) { /* FEXTRA */
        if (pos + 2u > src_len) return false;
        size_t xlen = (size_t)src[pos] | ((size_t)src[pos + 1u] << 8);
        pos += 2u + xlen;
    }
    if (flg & 0x08u) { /* FNAME */
        while (pos < src_len && src[pos] != 0u) ++pos;
        ++pos;
    }
    if (flg & 0x10u) { /* FCOMMENT */
        while (pos < src_len && src[pos] != 0u) ++pos;
        ++pos;
    }
    if (flg & 0x02u) { /* FHCRC */
        pos += 2u;
    }
    if (pos + 8u > src_len) return false;

    BitReader br = {src + pos, src_len - pos, 0u, 0u, 0};
    OutBuf    ob = {NULL, 0u, 0u, (max_out_len > 0u) ? max_out_len : (64u * 1024u * 1024u)};
    if (!ob_reserve(&ob, 64u)) return false;
    if (!inflate_stream(&br, &ob)) {
        free(ob.data);
        return false;
    }

    size_t trailer = pos + br.pos - (size_t)(br.bitcnt / 8);
    if (trailer + 4u <= src_len) {
        uint32_t expected_crc = (uint32_t)src[trailer] |
                                ((uint32_t)src[trailer + 1u] << 8) |
                                ((uint32_t)src[trailer + 2u] << 16) |
                                ((uint32_t)src[trailer + 3u] << 24);
        if (arcr_crc32(ob.data, ob.len) != expected_crc) {
            free(ob.data);
            return false;
        }
    }
    if (ob_reserve(&ob, 1u)) ob.data[ob.len] = 0u;
    *out_buf = ob.data;
    *out_len = ob.len;
    return true;
}

bool arcr_decompress_http_body(const uint8_t *src, size_t src_len,
                               const char *content_encoding,
                               uint8_t **out_buf, size_t *out_len, size_t max_out_len) {
    if (!src || !out_buf || !out_len) return false;
    if (src_len >= 2u && src[0] == 0x1Fu && src[1] == 0x8Bu) {
        return arcr_gzip_decompress(src, src_len, out_buf, out_len, max_out_len);
    }
    if (content_encoding && arcr_str_starts_with_nocase(content_encoding, "gzip")) {
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
    for (uint16_t i = 0u; i < cnt && pos + 46u <= zip_len; ++i) {
        if (get_u32le(zip_bytes + pos) != 0x02014B50u) break;
        uint16_t fn_len = get_u16le(zip_bytes + pos + 28u);
        uint16_t ex_len = get_u16le(zip_bytes + pos + 30u);
        uint16_t cm_len = get_u16le(zip_bytes + pos + 32u);
        if (pos + 46u + fn_len > zip_len) break;
        if (out_names && stored < max_entries) {
            size_t copy_n = ARCR_MIN((size_t)fn_len, (size_t)127u);
            memcpy(out_names[stored], zip_bytes + pos + 46u, copy_n);
            out_names[stored][copy_n] = '\0';
        }
        ++stored;
        pos += 46u + fn_len + ex_len + cm_len;
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
    for (uint16_t i = 0u; i < cnt && pos + 46u <= zip_len; ++i) {
        if (get_u32le(zip_bytes + pos) != 0x02014B50u) break;
        uint16_t method     = get_u16le(zip_bytes + pos + 10u);
        uint32_t crc_expect = get_u32le(zip_bytes + pos + 16u);
        uint32_t comp_sz    = get_u32le(zip_bytes + pos + 20u);
        uint32_t uncomp_sz  = get_u32le(zip_bytes + pos + 24u);
        uint16_t fn_len     = get_u16le(zip_bytes + pos + 28u);
        uint16_t ex_len     = get_u16le(zip_bytes + pos + 30u);
        uint16_t cm_len     = get_u16le(zip_bytes + pos + 32u);
        uint32_t lhdr_off   = get_u32le(zip_bytes + pos + 42u);
        if (pos + 46u + fn_len > zip_len) break;

        const char *fn_ptr = (const char *)(zip_bytes + pos + 46u);
        if (arcr_str_eq_nocase_n(fn_ptr, fn_len, target_filename, tlen)) {
            if (lhdr_off + 30u > zip_len || get_u32le(zip_bytes + lhdr_off) != 0x04034B50u) {
                return false;
            }
            uint16_t lfn = get_u16le(zip_bytes + lhdr_off + 26u);
            uint16_t lex = get_u16le(zip_bytes + lhdr_off + 28u);
            size_t data_off = (size_t)lhdr_off + 30u + lfn + lex;
            if (data_off + comp_sz > zip_len) return false;

            uint8_t *raw = NULL;
            size_t   rsz = 0u;
            if (method == 0u) {
                raw = (uint8_t *)malloc((size_t)uncomp_sz + 1u);
                if (!raw) return false;
                if (uncomp_sz > 0u) memcpy(raw, zip_bytes + data_off, uncomp_sz);
                raw[uncomp_sz] = 0u;
                rsz = uncomp_sz;
            } else if (method == 8u) {
                if (!arcr_deflate_decompress(zip_bytes + data_off, comp_sz, &raw, &rsz,
                                             (size_t)uncomp_sz + 64u)) {
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
        pos += 46u + fn_len + ex_len + cm_len;
    }
    return false;
}

/* =========================================================================
 * 6. Pure C PNG & BMP Image Decoder + Pure C PNG Encoder
 * ========================================================================= */
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
    while (pos + 12u <= len) {
        uint32_t clen  = get_u32be(bytes + pos);
        uint32_t ctype = get_u32be(bytes + pos + 4u);
        if (pos + 12u + clen > len) break;
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
            if (idat_len + clen > idat_cap) {
                size_t ncap = ARCR_MAX(idat_cap * 2u, idat_len + clen + 1024u);
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
        pos += 12u + clen;
    }

    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || bit_depth != 8u || interlace != 0u || !idat_cat) {
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
    bool top_down    = (raw_h < 0);
    int32_t h        = top_down ? -raw_h : raw_h;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || (bpp != 24u && bpp != 32u) || comp != 0u) {
        return false;
    }
    size_t row_stride = (((size_t)w * (bpp / 8u)) + 3u) & ~(size_t)3u;
    if (pix_off + row_stride * (size_t)h > len) return false;

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

bool arcr_image_decode_mem(const uint8_t *bytes, size_t len, ArcrImage *out_img) {
    if (!bytes || len < 16u || !out_img) return false;
    memset(out_img, 0, sizeof(*out_img));
    static const uint8_t kPngSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (memcmp(bytes, kPngSig, 8u) == 0) {
        return decode_png(bytes, len, out_img);
    }
    if (bytes[0] == 'B' && bytes[1] == 'M') {
        return decode_bmp(bytes, len, out_img);
    }
    return false;
}

bool arcr_image_decode_data_uri(const char *uri, size_t uri_len, ArcrImage *out_img) {
    if (!uri || !out_img || !arcr_str_starts_with_nocase(uri, "data:image/")) return false;
    const char *comma = NULL;
    for (size_t i = 0u; i < uri_len; ++i) {
        if (uri[i] == ',') { comma = uri + i; break; }
    }
    if (!comma) return false;
    size_t hdr_len = (size_t)(comma - uri);
    bool is_b64 = false;
    for (size_t i = 0u; i + 7u <= hdr_len; ++i) {
        if (arcr_str_eq_nocase_n(uri + i, 7u, ";base64", 7u)) { is_b64 = true; break; }
    }
    if (!is_b64) return false;

    uint8_t *raw = NULL;
    size_t   rlen = 0u;
    if (!arcr_base64_decode(comma + 1u, uri_len - hdr_len - 1u, &raw, &rlen)) return false;
    bool ok = arcr_image_decode_mem(raw, rlen, out_img);
    free(raw);
    return ok;
}

void arcr_image_free(ArcrImage *img) {
    if (!img) return;
    free(img->pixels);
    img->pixels = NULL;
    img->width  = 0;
    img->height = 0;
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
