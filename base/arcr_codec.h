/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * base/arcr_codec.h - Zero-Dependency Pure C11 Compression, Archive & Image Codecs
 *
 * Provides RFC 1951 DEFLATE (inflate + fast LZ77 fixed-Huffman compress),
 * RFC 1950 ZLIB, RFC 1952 GZIP, ZIP (.wsx extension & installer archive),
 * Base64, and Pure C PNG / BMP image decoding & encoding.
 * Works identically on Windows 2000 SP4 (SSE1) through modern 64-bit Linux/Windows.
 */
#ifndef ARCR_CODEC_H
#define ARCR_CODEC_H

#include "arcr_base.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Checksums */
uint32_t arcr_crc32_update(uint32_t crc, const uint8_t *data, size_t len);
uint32_t arcr_crc32(const uint8_t *data, size_t len);
uint32_t arcr_adler32(const uint8_t *data, size_t len);

/* RFC 1951 Raw DEFLATE, RFC 1950 ZLIB, RFC 1952 GZIP Decompression (caller frees *out_buf) */
bool arcr_deflate_decompress(const uint8_t *src, size_t src_len,
                             uint8_t **out_buf, size_t *out_len, size_t max_out_len);
bool arcr_zlib_decompress(const uint8_t *src, size_t src_len,
                          uint8_t **out_buf, size_t *out_len, size_t max_out_len);
bool arcr_gzip_decompress(const uint8_t *src, size_t src_len,
                          uint8_t **out_buf, size_t *out_len, size_t max_out_len);

/* Auto-detects GZIP (1F 8B), ZLIB, or raw DEFLATE for HTTP Content-Encoding */
bool arcr_decompress_http_body(const uint8_t *src, size_t src_len,
                               const char *content_encoding,
                               uint8_t **out_buf, size_t *out_len, size_t max_out_len);

/* Fast Pure C LZ77 + Fixed-Huffman DEFLATE & ZLIB Compression (caller frees *out_buf) */
bool arcr_deflate_compress(const uint8_t *src, size_t src_len,
                           uint8_t **out_buf, size_t *out_len);
bool arcr_zlib_compress(const uint8_t *src, size_t src_len,
                        uint8_t **out_buf, size_t *out_len);

/* Base64 Decode (for data: URIs) */
bool arcr_base64_decode(const char *b64, size_t b64_len,
                        uint8_t **out_buf, size_t *out_len);

/* Standard PKZIP (.zip / .wsx Add-on Package) Reader & Writer */
typedef struct ArcrZipEntryView {
    const char    *filename;
    const uint8_t *data;
    size_t         size;
} ArcrZipEntryView;

bool arcr_zip_create_mem(const ArcrZipEntryView *entries, size_t entry_count,
                         uint8_t **out_zip, size_t *out_zip_len);
bool arcr_zip_extract_entry(const uint8_t *zip_bytes, size_t zip_len,
                            const char *target_filename,
                            uint8_t **out_data, size_t *out_size);
size_t arcr_zip_list_entries(const uint8_t *zip_bytes, size_t zip_len,
                             char (*out_names)[128], size_t max_entries);

/* Decoded ARGB8888 Image (0xAARRGGBB) */
typedef struct ArcrImage {
    int32_t   width;
    int32_t   height;
    uint32_t *pixels; /* malloc'd width * height ARGB8888 pixels */
} ArcrImage;

bool arcr_image_decode_mem(const uint8_t *bytes, size_t len, ArcrImage *out_img);
bool arcr_image_decode_data_uri(const char *uri, size_t uri_len, ArcrImage *out_img);
void arcr_image_free(ArcrImage *img);

/* Pure C PNG Encoder (zero libpng requirement; works on Windows 2000 & POSIX!) */
bool arcr_image_encode_png_mem(const uint32_t *argb_pixels, int32_t width, int32_t height,
                               int32_t stride_pixels, uint8_t **out_png, size_t *out_len);
bool arcr_image_write_png_file(const char *filepath, const uint32_t *argb_pixels,
                               int32_t width, int32_t height, int32_t stride_pixels);

#endif /* ARCR_CODEC_H */
