/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * base/arcr_codec.h - Pure C11 Compression, Archive, Vector SVG & Modern Image Codecs
 *
 * Provides RFC 1951 DEFLATE, RFC 1950 ZLIB, RFC 1952 Multi-Member GZIP,
 * RFC 7932 Brotli (br), PKZIP (.wsx extension & installer archive), Base64,
 * JPEG (baseline + progressive), PNG (all bit depths + Adam7), GIF (static + animated),
 * WebP (VP8 + VP8L + Alpha), BMP, ICO, and anti-aliased Vector SVG rasterization.
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

/* RFC 1951 Raw DEFLATE, RFC 1950 ZLIB, RFC 1952 GZIP, RFC 7932 Brotli Decompression */
bool arcr_deflate_decompress(const uint8_t *src, size_t src_len,
                             uint8_t **out_buf, size_t *out_len, size_t max_out_len);
bool arcr_zlib_decompress(const uint8_t *src, size_t src_len,
                          uint8_t **out_buf, size_t *out_len, size_t max_out_len);
bool arcr_gzip_decompress(const uint8_t *src, size_t src_len,
                          uint8_t **out_buf, size_t *out_len, size_t max_out_len);
bool arcr_brotli_decompress(const uint8_t *src, size_t src_len,
                            uint8_t **out_buf, size_t *out_len, size_t max_out_len);

/* Auto-detects Brotli (br), GZIP (multi-member), ZLIB, or raw DEFLATE for HTTP Content-Encoding */
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

/* Multi-frame Animated Image (Animated GIF) */
typedef struct ArcrAnimImage {
    int32_t   width;
    int32_t   height;
    int32_t   frame_count;
    uint32_t *frames_argb; /* malloc'd frame_count * width * height ARGB8888 */
    int32_t  *delays_ms;   /* malloc'd frame_count delays in ms */
} ArcrAnimImage;

bool arcr_image_decode_mem(const uint8_t *bytes, size_t len, ArcrImage *out_img);
bool arcr_image_decode_data_uri(const char *uri, size_t uri_len, ArcrImage *out_img);
void arcr_image_free(ArcrImage *img);

bool arcr_image_decode_anim_mem(const uint8_t *bytes, size_t len, ArcrAnimImage *out_anim);
void arcr_image_free_anim(ArcrAnimImage *anim);

/* Anti-aliased Vector SVG Rasterizer (for both <img> SVG and inline <svg>...</svg>) */
bool arcr_svg_rasterize_mem(const char *svg_utf8, size_t svg_len,
                            int32_t target_w, int32_t target_h,
                            uint32_t current_color_argb, ArcrImage *out_img);

/* High-Quality Mitchell/Catmull-Rom ARGB8888 Resizer */
bool arcr_image_resize_argb(const uint32_t *src_argb, int32_t src_w, int32_t src_h,
                            int32_t dst_w, int32_t dst_h, ArcrImage *out_img);

/* Pure C PNG Encoder (zero libpng requirement; works on Windows 2000 & POSIX!) */
bool arcr_image_encode_png_mem(const uint32_t *argb_pixels, int32_t width, int32_t height,
                               int32_t stride_pixels, uint8_t **out_png, size_t *out_len);
bool arcr_image_write_png_file(const char *filepath, const uint32_t *argb_pixels,
                               int32_t width, int32_t height, int32_t stride_pixels);

#endif /* ARCR_CODEC_H */
