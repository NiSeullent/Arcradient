/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * installer/win32/waterscape_sfx_format.h - Self-Extracting Installer Trailer Specification
 *
 * Appended to the end of `waterscape_setup_stub.exe` after the PKZIP payload:
 *   Offset  Size  Field
 *   0       8     magic      ("WSSFX100")
 *   8       8     zip_offset (uint64_t little-endian byte offset of PKZIP archive)
 *   16      8     zip_size   (uint64_t little-endian byte length of PKZIP archive)
 *   24      4     zip_crc32  (uint32_t little-endian IEEE 802.3 CRC-32 of PKZIP archive)
 *   28      4     file_count (uint32_t little-endian total file entries inside archive)
 *   Total: 32 bytes exact (serialized byte-by-byte for 32-bit x86 / 64-bit x64 parity).
 */
#ifndef WATERSCAPE_SFX_FORMAT_H
#define WATERSCAPE_SFX_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WS_SFX_MAGIC_STR     "WSSFX100"
#define WS_SFX_MAGIC_LEN     8u
#define WS_SFX_TRAILER_BYTES 32u

typedef struct WsSfxTrailer {
    uint8_t  magic[8];
    uint64_t zip_offset;
    uint64_t zip_size;
    uint32_t zip_crc32;
    uint32_t file_count;
} WsSfxTrailer;

static inline void ws_sfx_put_u32le(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline void ws_sfx_put_u64le(uint8_t *dst, uint64_t v) {
    ws_sfx_put_u32le(dst + 0u, (uint32_t)(v & 0xFFFFFFFFu));
    ws_sfx_put_u32le(dst + 4u, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static inline uint32_t ws_sfx_get_u32le(const uint8_t *src) {
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static inline uint64_t ws_sfx_get_u64le(const uint8_t *src) {
    uint64_t lo = (uint64_t)ws_sfx_get_u32le(src + 0u);
    uint64_t hi = (uint64_t)ws_sfx_get_u32le(src + 4u);
    return lo | (hi << 32);
}

static inline void ws_sfx_trailer_encode(const WsSfxTrailer *in,
                                         uint8_t out_raw[WS_SFX_TRAILER_BYTES]) {
    memcpy(out_raw + 0u, WS_SFX_MAGIC_STR, WS_SFX_MAGIC_LEN);
    ws_sfx_put_u64le(out_raw + 8u,  in ? in->zip_offset : 0u);
    ws_sfx_put_u64le(out_raw + 16u, in ? in->zip_size   : 0u);
    ws_sfx_put_u32le(out_raw + 24u, in ? in->zip_crc32  : 0u);
    ws_sfx_put_u32le(out_raw + 28u, in ? in->file_count : 0u);
}

static inline bool ws_sfx_trailer_decode(const uint8_t raw[WS_SFX_TRAILER_BYTES],
                                         WsSfxTrailer *out) {
    if (!raw || !out) return false;
    if (memcmp(raw, WS_SFX_MAGIC_STR, WS_SFX_MAGIC_LEN) != 0) {
        return false;
    }
    memcpy(out->magic, raw, WS_SFX_MAGIC_LEN);
    out->zip_offset = ws_sfx_get_u64le(raw + 8u);
    out->zip_size   = ws_sfx_get_u64le(raw + 16u);
    out->zip_crc32  = ws_sfx_get_u32le(raw + 24u);
    out->file_count = ws_sfx_get_u32le(raw + 28u);
    return (out->zip_size >= 22u);
}

static inline bool ws_sfx_trailer_validate_bounds(const WsSfxTrailer *t,
                                                  uint64_t total_exe_bytes) {
    if (!t || total_exe_bytes < WS_SFX_TRAILER_BYTES + 22u) {
        return false;
    }
    uint64_t payload_end = total_exe_bytes - (uint64_t)WS_SFX_TRAILER_BYTES;
    if (t->zip_offset > payload_end || t->zip_size > payload_end - t->zip_offset) {
        return false;
    }
    return (t->zip_offset + t->zip_size == payload_end);
}

#endif /* WATERSCAPE_SFX_FORMAT_H */
