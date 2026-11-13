/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * web/bindings/web_crypto_streams.c - Pure C11 WebCrypto, Async Fetch, IPC & Workers
 *
 * Implements:
 * 1. OS CSPRNG (`/dev/urandom` on POSIX; dynamic `RtlGenRandom` / `SystemFunction036`
 *    + `CryptAcquireContextA` / `CryptGenRandom` fallback on Win32 / Windows 2000 SP4)
 * 2. Pure C11 Cryptographic Primitives:
 *    - SHA-1 (RFC 3174), SHA-256, SHA-384, SHA-512 (FIPS 180-4)
 *    - HMAC (RFC 2104), PBKDF2 (RFC 8018), HKDF (RFC 5869)
 *    - AES-128/192/256 + AES-GCM (NIST SP 800-38D GF(2^128) GHASH + GCTR)
 * 3. UTF-8 Streaming Helpers, ArrayBuffer Detach & Blob URL Registry (`blob:...`)
 * 4. Non-Blocking Threaded `fetch()` / `XMLHttpRequest` (`WebAsyncFetchJob` + `cancel_flag`)
 * 5. Origin- & Private-Mode-Partitioned `BroadcastChannel` Bus & `IndexedDB` Backing Store
 * 6. Multi-Threaded Dedicated `Worker` (`WebWorkerInstance` with Transferable ArrayBuffers)
 *
 * Zero QuickJS dependency. Zero link dependency on arcr_engine.
 */
#include "web_host.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#  include <fcntl.h>
#  include <unistd.h>
#endif

/* =========================================================================
 * 1. OS CSPRNG (POSIX /dev/urandom + Win32 RtlGenRandom / Win2000 CryptoAPI)
 * ========================================================================= */
static bool web_os_csprng_fill(uint8_t *buf, size_t len) {
    if (!buf && len > 0u) return false;
    if (len == 0u) return true;

#if defined(_WIN32)
    HMODULE adv = GetModuleHandleA("advapi32.dll");
    if (!adv) adv = LoadLibraryA("advapi32.dll");
    if (adv) {
        /* 1. Try RtlGenRandom (SystemFunction036, XP+) via ISO C11 pedantic-safe memcpy */
        typedef BOOLEAN (APIENTRY *RtlGenRandomFn)(PVOID, ULONG);
        FARPROC raw_rtl = GetProcAddress(adv, "SystemFunction036");
        if (raw_rtl) {
            RtlGenRandomFn fn_rtl = NULL;
            memcpy(&fn_rtl, &raw_rtl, sizeof(fn_rtl));
            if (fn_rtl && fn_rtl(buf, (ULONG)len)) return true;
        }
        /* 2. Fallback to Windows 2000 SP4 CryptAcquireContextA + CryptGenRandom */
        typedef BOOL (WINAPI *CryptAcquireFn)(ULONG_PTR *, LPCSTR, LPCSTR, DWORD, DWORD);
        typedef BOOL (WINAPI *CryptGenFn)(ULONG_PTR, DWORD, BYTE *);
        typedef BOOL (WINAPI *CryptReleaseFn)(ULONG_PTR, DWORD);
        FARPROC raw_acq = GetProcAddress(adv, "CryptAcquireContextA");
        FARPROC raw_gen = GetProcAddress(adv, "CryptGenRandom");
        FARPROC raw_rel = GetProcAddress(adv, "CryptReleaseContext");
        if (raw_acq && raw_gen && raw_rel) {
            CryptAcquireFn fn_acq = NULL;
            CryptGenFn     fn_gen = NULL;
            CryptReleaseFn fn_rel = NULL;
            memcpy(&fn_acq, &raw_acq, sizeof(fn_acq));
            memcpy(&fn_gen, &raw_gen, sizeof(fn_gen));
            memcpy(&fn_rel, &raw_rel, sizeof(fn_rel));
            ULONG_PTR hprov = 0u;
            if (fn_acq(&hprov, NULL, NULL, 1u /* PROV_RSA_FULL */, 0xF0000000u /* CRYPT_VERIFYCONTEXT */)) {
                BOOL ok = fn_gen(hprov, (DWORD)len, (BYTE *)buf);
                (void)fn_rel(hprov, 0u);
                if (ok) return true;
            }
        }
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t got = 0u;
        while (got < len) {
            ssize_t r = read(fd, buf + got, len - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        (void)close(fd);
        if (got == len) return true;
    }
#endif
    return arcr_platform_csprng_bytes(buf, len);
}

/* =========================================================================
 * 2. Pure C11 SHA-1, SHA-256, SHA-384, SHA-512, HMAC, PBKDF2 & HKDF
 * ========================================================================= */
static inline uint32_t rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }
static inline uint32_t rotl32(uint32_t x, unsigned n) { return (x << n) | (x >> (32u - n)); }
static inline uint64_t rotr64(uint64_t x, unsigned n) { return (x >> n) | (x << (64u - n)); }

static inline uint32_t load_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline void store_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline uint64_t load_be64(const uint8_t *p) {
    return ((uint64_t)load_be32(p) << 32) | (uint64_t)load_be32(p + 4);
}
static inline void store_be64(uint8_t *p, uint64_t v) {
    store_be32(p, (uint32_t)(v >> 32));
    store_be32(p + 4, (uint32_t)(v & 0xFFFFFFFFu));
}

/* --- SHA-1 (RFC 3174) --- */
static void sha1_compress(uint32_t st[5], const uint8_t blk[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = load_be32(blk + i * 4);
    for (int i = 16; i < 80; ++i) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1u);

    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                      k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);    k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                      k = 0xCA62C1D6u; }
        uint32_t tmp = rotl32(a, 5u) + f + e + k + w[i];
        e = d; d = c; c = rotl32(b, 30u); b = a; a = tmp;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}

static void sha1_digest(const uint8_t *msg, size_t len, uint8_t out[20]) {
    uint32_t st[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    size_t off = 0u;
    while (len - off >= 64u) {
        sha1_compress(st, msg + off);
        off += 64u;
    }
    uint8_t tail[128];
    memset(tail, 0, sizeof(tail));
    size_t rem = len - off;
    if (rem > 0u) memcpy(tail, msg + off, rem);
    tail[rem] = 0x80u;
    size_t blocks = (rem >= 56u) ? 2u : 1u;
    store_be64(tail + blocks * 64u - 8u, (uint64_t)len * 8ULL);
    for (size_t b = 0u; b < blocks; ++b) sha1_compress(st, tail + b * 64u);
    for (int i = 0; i < 5; ++i) store_be32(out + i * 4, st[i]);
}

/* --- SHA-256 (FIPS 180-4) --- */
static const uint32_t kSha256K[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u
};

static void sha256_compress(uint32_t st[8], const uint8_t blk[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = load_be32(blk + i * 4);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7u) ^ rotr32(w[i - 15], 18u) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17u) ^ rotr32(w[i - 2], 19u)  ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    uint32_t e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + kSha256K[i] + w[i];
        uint32_t S0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static void sha256_digest(const uint8_t *msg, size_t len, uint8_t out[32]) {
    uint32_t st[8] = {
        0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
        0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
    };
    size_t off = 0u;
    while (len - off >= 64u) {
        sha256_compress(st, msg + off);
        off += 64u;
    }
    uint8_t tail[128];
    memset(tail, 0, sizeof(tail));
    size_t rem = len - off;
    if (rem > 0u) memcpy(tail, msg + off, rem);
    tail[rem] = 0x80u;
    size_t blocks = (rem >= 56u) ? 2u : 1u;
    store_be64(tail + blocks * 64u - 8u, (uint64_t)len * 8ULL);
    for (size_t b = 0u; b < blocks; ++b) sha256_compress(st, tail + b * 64u);
    for (int i = 0; i < 8; ++i) store_be32(out + i * 4, st[i]);
}

/* --- SHA-384 & SHA-512 (FIPS 180-4) --- */
static const uint64_t kSha512K[80] = {
    0x428A2F98D728AE22ULL, 0x7137449123EF65CDULL, 0xB5C0FBCFEC4D3B2FULL, 0xE9B5DBA58189DBBCULL,
    0x3956C25BF348B538ULL, 0x59F111F1B605D019ULL, 0x923F82A4AF194F9BULL, 0xAB1C5ED5DA6D8118ULL,
    0xD807AA98A3030242ULL, 0x12835B0145706FBEULL, 0x243185BE4EE4B28CULL, 0x550C7DC3D5FFB4E2ULL,
    0x72BE5D74F27B896FULL, 0x80DEB1FE3B1696B1ULL, 0x9BDC06A725C71235ULL, 0xC19BF174CF692694ULL,
    0xE49B69C19EF14AD2ULL, 0xEFBE4786384F25E3ULL, 0x0FC19DC68B8CD5B5ULL, 0x240CA1CC77AC9C65ULL,
    0x2DE92C6F592B0275ULL, 0x4A7484AA6EA6E483ULL, 0x5CB0A9DCBD41FBD4ULL, 0x76F988DA831153B5ULL,
    0x983E5152EE66DFABULL, 0xA831C66D2DB43210ULL, 0xB00327C898FB213FULL, 0xBF597FC7BEEF0EE4ULL,
    0xC6E00BF33DA88FC2ULL, 0xD5A79147930AA725ULL, 0x06CA6351E003826FULL, 0x142929670A0E6E70ULL,
    0x27B70A8546D22FFCULL, 0x2E1B21385C26C926ULL, 0x4D2C6DFC5AC42AEDULL, 0x53380D139D95B3DFULL,
    0x650A73548BAF63DEULL, 0x766A0ABB3C77B2A8ULL, 0x81C2C92E47EDAEE6ULL, 0x92722C851482353BULL,
    0xA2BFE8A14CF10364ULL, 0xA81A664BBC423001ULL, 0xC24B8B70D0F89791ULL, 0xC76C51A30654BE30ULL,
    0xD192E819D6EF5218ULL, 0xD69906245565A910ULL, 0xF40E35855771202AULL, 0x106AA07032BBD1B8ULL,
    0x19A4C116B8D2D0C8ULL, 0x1E376C085141AB53ULL, 0x2748774CDF8EEB99ULL, 0x34B0BCB5E19B48A8ULL,
    0x391C0CB3C5C95A63ULL, 0x4ED8AA4AE3418ACBULL, 0x5B9CCA4F7763E373ULL, 0x682E6FF3D6B2B8A3ULL,
    0x748F82EE5DEFB2FCULL, 0x78A5636F43172F60ULL, 0x84C87814A1F0AB72ULL, 0x8CC702081A6439ECULL,
    0x90BEFFFA23631E28ULL, 0xA4506CEBDE82BDE9ULL, 0xBEF9A3F7B2C67915ULL, 0xC67178F2E372532BULL,
    0xCA273ECEEA26619CULL, 0xD186B8C721C0C207ULL, 0xEADA7DD6CDE0EB1EULL, 0xF57D4F7FEE6ED178ULL,
    0x06F067AA72176FBAULL, 0x0A637DC5A2C898A6ULL, 0x113F9804BEF90DAEULL, 0x1B710B35131C471BULL,
    0x28DB77F523047D84ULL, 0x32CAAB7B40C72493ULL, 0x3C9EBE0A15C9BEBCULL, 0x431D67C49C100D4CULL,
    0x4CC5D4BECB3E42B6ULL, 0x597F299CFC657E2AULL, 0x5FCB6FAB3AD6FAECULL, 0x6C44198C4A475817ULL
};

static void sha512_compress(uint64_t st[8], const uint8_t blk[128]) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = load_be64(blk + i * 8);
    for (int i = 16; i < 80; ++i) {
        uint64_t s0 = rotr64(w[i - 15], 1u)  ^ rotr64(w[i - 15], 8u)  ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19u) ^ rotr64(w[i - 2], 61u) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = st[0], b = st[1], c = st[2], d = st[3];
    uint64_t e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 80; ++i) {
        uint64_t S1 = rotr64(e, 14u) ^ rotr64(e, 18u) ^ rotr64(e, 41u);
        uint64_t ch = (e & f) ^ ((~e) & g);
        uint64_t t1 = h + S1 + ch + kSha512K[i] + w[i];
        uint64_t S0 = rotr64(a, 28u) ^ rotr64(a, 34u) ^ rotr64(a, 39u);
        uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static void sha512_family(const uint8_t *msg, size_t len, bool is_384, uint8_t *out) {
    uint64_t st[8];
    if (is_384) {
        st[0] = 0xCBBB9D5DC1059ED8ULL; st[1] = 0x629A292A367CD507ULL;
        st[2] = 0x9159015A3070DD17ULL; st[3] = 0x152FECD8F70E5939ULL;
        st[4] = 0x67332667FFC00B31ULL; st[5] = 0x8EB44A8768581511ULL;
        st[6] = 0xDB0C2E0D64F98FA7ULL; st[7] = 0x47B5481DBEFA4FA4ULL;
    } else {
        st[0] = 0x6A09E667F3BCC908ULL; st[1] = 0xBB67AE8584CAA73BULL;
        st[2] = 0x3C6EF372FE94F82BULL; st[3] = 0xA54FF53A5F1D36F1ULL;
        st[4] = 0x510E527FADE682D1ULL; st[5] = 0x9B05688C2B3E6C1FULL;
        st[6] = 0x1F83D9ABFB41BD6BULL; st[7] = 0x5BE0CD19137E2179ULL;
    }
    size_t off = 0u;
    while (len - off >= 128u) {
        sha512_compress(st, msg + off);
        off += 128u;
    }
    uint8_t tail[256];
    memset(tail, 0, sizeof(tail));
    size_t rem = len - off;
    if (rem > 0u) memcpy(tail, msg + off, rem);
    tail[rem] = 0x80u;
    size_t blocks = (rem >= 112u) ? 2u : 1u;
    store_be64(tail + blocks * 128u - 16u, (uint64_t)len >> 61);
    store_be64(tail + blocks * 128u - 8u,  (uint64_t)len * 8ULL);
    for (size_t b = 0u; b < blocks; ++b) sha512_compress(st, tail + b * 128u);
    int words = is_384 ? 6 : 8;
    for (int i = 0; i < words; ++i) store_be64(out + i * 8, st[i]);
}

static bool web_hash_params(int alg, size_t *out_dlen, size_t *out_blen) {
    switch (alg) {
        case 1:   *out_dlen = 20u; *out_blen = 64u;  return true;
        case 256: *out_dlen = 32u; *out_blen = 64u;  return true;
        case 384: *out_dlen = 48u; *out_blen = 128u; return true;
        case 512: *out_dlen = 64u; *out_blen = 128u; return true;
        default:  return false;
    }
}

static bool web_hash_once(int alg, const uint8_t *data, size_t len, uint8_t out[64], size_t *out_dlen) {
    size_t dlen = 0u, blen = 0u;
    if (!web_hash_params(alg, &dlen, &blen)) return false;
    static const uint8_t kEmpty = 0u;
    const uint8_t *src = data ? data : &kEmpty;
    if (alg == 1)        sha1_digest(src, len, out);
    else if (alg == 256) sha256_digest(src, len, out);
    else if (alg == 384) sha512_family(src, len, true, out);
    else                 sha512_family(src, len, false, out);
    if (out_dlen) *out_dlen = dlen;
    return true;
}

static bool web_hmac_compute(int alg, const uint8_t *key, size_t key_len,
                             const uint8_t *data, size_t data_len,
                             uint8_t out[64], size_t *out_len) {
    size_t dlen = 0u, blen = 0u;
    if (!web_hash_params(alg, &dlen, &blen)) return false;

    uint8_t kpad[128];
    memset(kpad, 0, sizeof(kpad));
    if (key_len > blen) {
        if (!web_hash_once(alg, key, key_len, kpad, NULL)) return false;
    } else if (key && key_len > 0u) {
        memcpy(kpad, key, key_len);
    }

    uint8_t *inner = (uint8_t *)malloc(blen + data_len + 1u);
    if (!inner) return false;
    for (size_t i = 0u; i < blen; ++i) inner[i] = (uint8_t)(kpad[i] ^ 0x36u);
    if (data && data_len > 0u) memcpy(inner + blen, data, data_len);

    uint8_t ih[64];
    (void)web_hash_once(alg, inner, blen + data_len, ih, NULL);
    free(inner);

    uint8_t outer[128 + 64];
    for (size_t i = 0u; i < blen; ++i) outer[i] = (uint8_t)(kpad[i] ^ 0x5Cu);
    memcpy(outer + blen, ih, dlen);
    (void)web_hash_once(alg, outer, blen + dlen, out, out_len);
    return true;
}

static bool web_pbkdf2_derive(int alg, const uint8_t *pass, size_t pass_len,
                              const uint8_t *salt, size_t salt_len,
                              uint32_t iterations, uint8_t *out, size_t dk_len) {
    size_t hlen = 0u, blen = 0u;
    if (!web_hash_params(alg, &hlen, &blen) || iterations == 0u) return false;
    if (dk_len == 0u) return true;

    uint8_t *salt_blk = (uint8_t *)malloc(salt_len + 4u);
    if (!salt_blk) return false;
    if (salt && salt_len > 0u) memcpy(salt_blk, salt, salt_len);

    size_t pos = 0u;
    uint32_t block_idx = 1u;
    while (pos < dk_len) {
        store_be32(salt_blk + salt_len, block_idx++);
        uint8_t u[64], t[64];
        if (!web_hmac_compute(alg, pass, pass_len, salt_blk, salt_len + 4u, u, NULL)) {
            free(salt_blk);
            return false;
        }
        memcpy(t, u, hlen);
        for (uint32_t it = 1u; it < iterations; ++it) {
            uint8_t u_next[64];
            (void)web_hmac_compute(alg, pass, pass_len, u, hlen, u_next, NULL);
            memcpy(u, u_next, hlen);
            for (size_t j = 0u; j < hlen; ++j) t[j] ^= u[j];
        }
        size_t take = (dk_len - pos < hlen) ? (dk_len - pos) : hlen;
        memcpy(out + pos, t, take);
        pos += take;
    }
    free(salt_blk);
    return true;
}

static bool web_hkdf_derive(int alg, const uint8_t *ikm, size_t ikm_len,
                            const uint8_t *salt, size_t salt_len,
                            const uint8_t *info, size_t info_len,
                            uint8_t *out, size_t okm_len) {
    size_t hlen = 0u, blen = 0u;
    if (!web_hash_params(alg, &hlen, &blen)) return false;
    if (okm_len > 255u * hlen) return false;
    if (okm_len == 0u) return true;

    uint8_t zero_salt[64];
    memset(zero_salt, 0, sizeof(zero_salt));
    const uint8_t *eff_salt = (salt && salt_len > 0u) ? salt : zero_salt;
    size_t eff_slen = (salt && salt_len > 0u) ? salt_len : hlen;

    uint8_t prk[64];
    if (!web_hmac_compute(alg, eff_salt, eff_slen, ikm, ikm_len, prk, NULL)) return false;

    uint8_t *buf = (uint8_t *)malloc(hlen + info_len + 1u);
    if (!buf) return false;

    uint8_t prev[64];
    size_t prev_len = 0u;
    size_t pos = 0u;
    uint8_t ctr = 1u;
    while (pos < okm_len) {
        if (prev_len > 0u) memcpy(buf, prev, prev_len);
        if (info && info_len > 0u) memcpy(buf + prev_len, info, info_len);
        buf[prev_len + info_len] = ctr++;
        (void)web_hmac_compute(alg, prk, hlen, buf, prev_len + info_len + 1u, prev, NULL);
        prev_len = hlen;
        size_t take = (okm_len - pos < hlen) ? (okm_len - pos) : hlen;
        memcpy(out + pos, prev, take);
        pos += take;
    }
    free(buf);
    return true;
}

/* =========================================================================
 * 3. Pure C11 AES-128/192/256 & NIST SP 800-38D AES-GCM
 * ========================================================================= */
static const uint8_t kAesSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

typedef struct AesKeySched {
    uint32_t rk[60];
    int      rounds;
} AesKeySched;

static bool aes_expand_key(AesKeySched *ks, const uint8_t *key, size_t key_len) {
    int nk = 0;
    if (key_len == 16u)      { nk = 4; ks->rounds = 10; }
    else if (key_len == 24u) { nk = 6; ks->rounds = 12; }
    else if (key_len == 32u) { nk = 8; ks->rounds = 14; }
    else return false;

    for (int i = 0; i < nk; ++i) ks->rk[i] = load_be32(key + i * 4);
    int total = 4 * (ks->rounds + 1);
    uint32_t rcon = 0x01000000u;
    for (int i = nk; i < total; ++i) {
        uint32_t tmp = ks->rk[i - 1];
        if (i % nk == 0) {
            tmp = (tmp << 8) | (tmp >> 24);
            tmp = ((uint32_t)kAesSbox[(tmp >> 24) & 0xFFu] << 24) |
                  ((uint32_t)kAesSbox[(tmp >> 16) & 0xFFu] << 16) |
                  ((uint32_t)kAesSbox[(tmp >> 8)  & 0xFFu] << 8)  |
                  (uint32_t)kAesSbox[tmp & 0xFFu];
            tmp ^= rcon;
            rcon = ((rcon << 1) ^ ((rcon & 0x80000000u) ? 0x1B000000u : 0u)) & 0xFF000000u;
        } else if (nk == 8 && (i % 8) == 4) {
            tmp = ((uint32_t)kAesSbox[(tmp >> 24) & 0xFFu] << 24) |
                  ((uint32_t)kAesSbox[(tmp >> 16) & 0xFFu] << 16) |
                  ((uint32_t)kAesSbox[(tmp >> 8)  & 0xFFu] << 8)  |
                  (uint32_t)kAesSbox[tmp & 0xFFu];
        }
        ks->rk[i] = ks->rk[i - nk] ^ tmp;
    }
    return true;
}

static inline uint8_t gf256_xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80u) ? 0x1Bu : 0x00u));
}

static void aes_encrypt_block(const AesKeySched *ks, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int c = 0; c < 4; ++c) {
        uint32_t k = ks->rk[c];
        s[c * 4 + 0] = (uint8_t)(in[c * 4 + 0] ^ (k >> 24));
        s[c * 4 + 1] = (uint8_t)(in[c * 4 + 1] ^ (k >> 16));
        s[c * 4 + 2] = (uint8_t)(in[c * 4 + 2] ^ (k >> 8));
        s[c * 4 + 3] = (uint8_t)(in[c * 4 + 3] ^ k);
    }

    for (int r = 1; r <= ks->rounds; ++r) {
        uint8_t t[16];
        for (int i = 0; i < 16; ++i) t[i] = kAesSbox[s[i]];
        /* ShiftRows */
        s[0]  = t[0];  s[4]  = t[4];  s[8]  = t[8];  s[12] = t[12];
        s[1]  = t[5];  s[5]  = t[9];  s[9]  = t[13]; s[13] = t[1];
        s[2]  = t[10]; s[6]  = t[14]; s[10] = t[2];  s[14] = t[6];
        s[3]  = t[15]; s[7]  = t[3];  s[11] = t[7];  s[15] = t[11];

        if (r < ks->rounds) {
            for (int c = 0; c < 4; ++c) {
                uint8_t a0 = s[c * 4 + 0], a1 = s[c * 4 + 1], a2 = s[c * 4 + 2], a3 = s[c * 4 + 3];
                uint8_t xr = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                s[c * 4 + 0] ^= (uint8_t)(xr ^ gf256_xtime((uint8_t)(a0 ^ a1)));
                s[c * 4 + 1] ^= (uint8_t)(xr ^ gf256_xtime((uint8_t)(a1 ^ a2)));
                s[c * 4 + 2] ^= (uint8_t)(xr ^ gf256_xtime((uint8_t)(a2 ^ a3)));
                s[c * 4 + 3] ^= (uint8_t)(xr ^ gf256_xtime((uint8_t)(a3 ^ a0)));
            }
        }
        for (int c = 0; c < 4; ++c) {
            uint32_t k = ks->rk[r * 4 + c];
            s[c * 4 + 0] ^= (uint8_t)(k >> 24);
            s[c * 4 + 1] ^= (uint8_t)(k >> 16);
            s[c * 4 + 2] ^= (uint8_t)(k >> 8);
            s[c * 4 + 3] ^= (uint8_t)k;
        }
    }
    memcpy(out, s, 16u);
}

static void ghash_mul(uint8_t x[16], const uint8_t h[16]) {
    uint64_t z_hi = 0ULL, z_lo = 0ULL;
    uint64_t v_hi = load_be64(h);
    uint64_t v_lo = load_be64(h + 8);

    for (int i = 0; i < 16; ++i) {
        uint8_t xb = x[i];
        for (int bit = 7; bit >= 0; --bit) {
            if ((xb >> bit) & 1u) {
                z_hi ^= v_hi;
                z_lo ^= v_lo;
            }
            uint64_t lsb = v_lo & 1ULL;
            v_lo = (v_lo >> 1) | (v_hi << 63);
            v_hi = (v_hi >> 1) ^ (lsb ? 0xE100000000000000ULL : 0ULL);
        }
    }
    store_be64(x, z_hi);
    store_be64(x + 8, z_lo);
}

static void ghash_update_padded(uint8_t y[16], const uint8_t h[16], const uint8_t *buf, size_t len) {
    size_t off = 0u;
    while (len - off >= 16u) {
        for (size_t i = 0u; i < 16u; ++i) y[i] ^= buf[off + i];
        ghash_mul(y, h);
        off += 16u;
    }
    if (off < len) {
        for (size_t i = 0u; off + i < len; ++i) y[i] ^= buf[off + i];
        ghash_mul(y, h);
    }
}

static inline void gcm_inc32(uint8_t ctr[16]) {
    uint32_t c = load_be32(ctr + 12) + 1u;
    store_be32(ctr + 12, c);
}

static bool aes_gcm_crypt(bool encrypt, const uint8_t *key, size_t key_len,
                          const uint8_t *iv, size_t iv_len,
                          const uint8_t *aad, size_t aad_len,
                          size_t tag_bytes,
                          const uint8_t *in_data, size_t in_len,
                          uint8_t **out_buf, size_t *out_len) {
    if (!key || !iv || iv_len == 0u || !out_buf || !out_len) return false;
    if (tag_bytes != 16u && tag_bytes != 15u && tag_bytes != 14u &&
        tag_bytes != 13u && tag_bytes != 12u && tag_bytes != 8u && tag_bytes != 4u) {
        return false;
    }
    if (!encrypt && in_len < tag_bytes) return false;

    AesKeySched ks;
    if (!aes_expand_key(&ks, key, key_len)) return false;

    uint8_t zero[16] = {0};
    uint8_t h[16];
    aes_encrypt_block(&ks, zero, h);

    uint8_t j0[16] = {0};
    if (iv_len == 12u) {
        memcpy(j0, iv, 12u);
        j0[15] = 1u;
    } else {
        ghash_update_padded(j0, h, iv, iv_len);
        uint8_t len_blk[16] = {0};
        store_be64(len_blk + 8, (uint64_t)iv_len * 8ULL);
        ghash_update_padded(j0, h, len_blk, 16u);
    }

    size_t text_len = encrypt ? in_len : (in_len - tag_bytes);
    size_t alloc_sz = encrypt ? (text_len + tag_bytes) : (text_len > 0u ? text_len : 1u);
    uint8_t *dst = (uint8_t *)malloc(alloc_sz);
    if (!dst) return false;

    uint8_t cb[16], ks_blk[16];
    memcpy(cb, j0, 16u);
    for (size_t off = 0u; off < text_len; off += 16u) {
        gcm_inc32(cb);
        aes_encrypt_block(&ks, cb, ks_blk);
        size_t chunk = (text_len - off < 16u) ? (text_len - off) : 16u;
        for (size_t i = 0u; i < chunk; ++i) {
            dst[off + i] = (uint8_t)(in_data[off + i] ^ ks_blk[i]);
        }
    }

    const uint8_t *ct_ptr = encrypt ? dst : in_data;
    uint8_t s[16] = {0};
    if (aad && aad_len > 0u) ghash_update_padded(s, h, aad, aad_len);
    if (text_len > 0u) ghash_update_padded(s, h, ct_ptr, text_len);
    uint8_t lens[16];
    store_be64(lens,     (uint64_t)aad_len * 8ULL);
    store_be64(lens + 8, (uint64_t)text_len * 8ULL);
    ghash_update_padded(s, h, lens, 16u);

    uint8_t ej0[16], calc_tag[16];
    aes_encrypt_block(&ks, j0, ej0);
    for (int i = 0; i < 16; ++i) calc_tag[i] = (uint8_t)(s[i] ^ ej0[i]);

    if (encrypt) {
        memcpy(dst + text_len, calc_tag, tag_bytes);
        *out_buf = dst;
        *out_len = text_len + tag_bytes;
        return true;
    }

    /* Constant-time tag verification */
    uint8_t diff = 0u;
    const uint8_t *given_tag = in_data + text_len;
    for (size_t i = 0u; i < tag_bytes; ++i) diff |= (uint8_t)(calc_tag[i] ^ given_tag[i]);
    if (diff != 0u) {
        free(dst);
        return false;
    }
    *out_buf = dst;
    *out_len = text_len;
    return true;
}

/* =========================================================================
 * 4. Global Process IPC Bus: Blob URLs, BroadcastChannel & IndexedDB Store
 * ========================================================================= */
#if defined(_WIN32)
static LONG     g_ipc_once_state = 0;
static WebMutex g_ipc_mtx;
static void web_ipc_lock(void) {
    LONG prev = InterlockedCompareExchange(&g_ipc_once_state, 1, 0);
    if (prev == 0) {
        web_mutex_init(&g_ipc_mtx);
        InterlockedExchange(&g_ipc_once_state, 2);
    } else if (prev != 2) {
        while (InterlockedCompareExchange(&g_ipc_once_state, 2, 2) != 2) {
            Sleep(0);
        }
    }
    web_mutex_lock(&g_ipc_mtx);
}
static void web_ipc_unlock(void) { web_mutex_unlock(&g_ipc_mtx); }
#else
static pthread_mutex_t g_ipc_mtx = PTHREAD_MUTEX_INITIALIZER;
static void web_ipc_lock(void)   { (void)pthread_mutex_lock(&g_ipc_mtx); }
static void web_ipc_unlock(void) { (void)pthread_mutex_unlock(&g_ipc_mtx); }
#endif

typedef struct GlobalBlobNode {
    uint64_t               owner_vm_id;
    bool                   private_browsing;
    char                   origin[256];
    char                   url[256];
    char                   mime[128];
    uint8_t               *bytes;
    size_t                 len;
    struct GlobalBlobNode *next;
} GlobalBlobNode;

static GlobalBlobNode *g_blobs_head = NULL;

static bool global_blob_lookup(const char *url, bool *out_priv, char *out_origin, size_t orig_cap,
                               uint8_t **out_copy, size_t *out_len, char *out_mime, size_t mime_cap) {
    if (!url) return false;
    bool found = false;
    web_ipc_lock();
    for (GlobalBlobNode *b = g_blobs_head; b != NULL; b = b->next) {
        if (strcmp(b->url, url) == 0) {
            if (out_priv) *out_priv = b->private_browsing;
            if (out_origin && orig_cap > 0u) arcr_str_copy(out_origin, orig_cap, b->origin);
            if (out_mime && mime_cap > 0u) arcr_str_copy(out_mime, mime_cap, b->mime);
            if (out_len) *out_len = b->len;
            if (out_copy) {
                uint8_t *cp = (uint8_t *)malloc(b->len + 1u);
                if (cp) {
                    if (b->bytes && b->len > 0u) memcpy(cp, b->bytes, b->len);
                    cp[b->len] = 0u;
                    *out_copy = cp;
                    found = true;
                }
            } else {
                found = true;
            }
            break;
        }
    }
    web_ipc_unlock();
    return found;
}

static void global_blob_remove_for_vm(uint64_t vm_id, const char *only_url_or_null) {
    web_ipc_lock();
    GlobalBlobNode **pp = &g_blobs_head;
    while (*pp) {
        GlobalBlobNode *cur = *pp;
        bool match = (only_url_or_null != NULL)
                         ? (strcmp(cur->url, only_url_or_null) == 0)
                         : (cur->owner_vm_id == vm_id);
        if (match) {
            *pp = cur->next;
            free(cur->bytes);
            free(cur);
        } else {
            pp = &cur->next;
        }
    }
    web_ipc_unlock();
}

bool web_lookup_blob_url(ArcrJsVm *vm, const char *url,
                         const uint8_t **out_bytes, size_t *out_len,
                         char *out_mime, size_t mime_cap) {
    if (!vm || !url) return false;
    for (WebBlobUrlEntry *e = vm->blob_urls_head; e != NULL; e = e->next) {
        if (strcmp(e->url, url) == 0) {
            if (out_bytes) *out_bytes = e->bytes;
            if (out_len)   *out_len   = e->len;
            if (out_mime && mime_cap > 0u) arcr_str_copy(out_mime, mime_cap, e->mime);
            return true;
        }
    }
    return false;
}

/* --- BroadcastChannel Bus --- */
typedef struct WebBcMsg {
    uint64_t         seq;
    uint32_t         sender_sub_id;
    bool             private_browsing;
    char             origin[256];
    char             channel[128];
    char            *payload_json;
    struct WebBcMsg *next;
} WebBcMsg;

typedef struct WebBcSub {
    uint32_t         sub_id;
    uint64_t         vm_instance_id;
    bool             private_browsing;
    char             origin[256];
    char             channel[128];
    uint64_t         last_seq;
    struct WebBcSub *next;
} WebBcSub;

static uint64_t  g_bc_seq        = 0u;
static uint32_t  g_bc_next_sub   = 1u;
static size_t    g_bc_msg_count  = 0u;
static WebBcMsg *g_bc_msgs_head  = NULL;
static WebBcMsg *g_bc_msgs_tail  = NULL;
static WebBcSub *g_bc_subs_head  = NULL;

static void web_bc_Current_origin(const ArcrJsVm *vm, char *out_orig, size_t cap) {
    char base[2048];
    web_effective_base_url(vm, base, sizeof(base));
    web_compute_origin(base, out_orig, cap);
}

static bool web_bc_has_unread_locked(uint64_t vm_id) {
    for (const WebBcSub *s = g_bc_subs_head; s != NULL; s = s->next) {
        if (s->vm_instance_id != vm_id) continue;
        for (const WebBcMsg *m = g_bc_msgs_head; m != NULL; m = m->next) {
            if (m->seq <= s->last_seq) continue;
            if (m->sender_sub_id == s->sub_id) continue;
            if (m->private_browsing != s->private_browsing) continue;
            if (strcmp(m->origin, s->origin) != 0) continue;
            if (strcmp(m->channel, s->channel) != 0) continue;
            return true;
        }
    }
    return false;
}

static void web_bc_cleanup_vm(uint64_t vm_id) {
    web_ipc_lock();
    WebBcSub **pp = &g_bc_subs_head;
    while (*pp) {
        WebBcSub *cur = *pp;
        if (cur->vm_instance_id == vm_id) {
            *pp = cur->next;
            free(cur);
        } else {
            pp = &cur->next;
        }
    }
    web_ipc_unlock();
}

/* --- IndexedDB Origin/Profile Backing Store --- */
typedef struct WebIdbEntry {
    bool                private_browsing;
    char                origin[256];
    char                db_name[128];
    uint64_t            version;
    char               *snapshot_json;
    struct WebIdbEntry *next;
} WebIdbEntry;

static WebIdbEntry *g_idb_head = NULL;

/* =========================================================================
 * 5. Non-Blocking Threaded Fetch & data:/blob: Support
 * ========================================================================= */
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode_data_url(const char *url, char *out_mime, size_t mime_cap,
                            uint8_t **out_bytes, size_t *out_len) {
    if (!url || !arcr_str_starts_with_nocase(url, "data:")) return false;
    const char *p = url + 5;
    const char *comma = strchr(p, ',');
    if (!comma) return false;

    size_t meta_len = (size_t)(comma - p);
    bool is_b64 = false;
    if (meta_len >= 7u && arcr_str_eq_nocase_n(comma - 7, 7u, ";base64", 7u)) {
        is_b64 = true;
        meta_len -= 7u;
    }
    if (meta_len == 0u) {
        arcr_str_copy(out_mime, mime_cap, "text/plain;charset=US-ASCII");
    } else {
        size_t cp = meta_len < mime_cap - 1u ? meta_len : mime_cap - 1u;
        memcpy(out_mime, p, cp);
        out_mime[cp] = '\0';
    }

    const char *payload = comma + 1;
    size_t plen = strlen(payload);
    if (is_b64) {
        return arcr_base64_decode(payload, plen, out_bytes, out_len);
    }

    uint8_t *buf = (uint8_t *)malloc(plen + 1u);
    if (!buf) return false;
    size_t o = 0u;
    for (size_t i = 0u; i < plen; ++i) {
        if (payload[i] == '%' && i + 2u < plen) {
            int hi = hex_nibble(payload[i + 1u]);
            int lo = hex_nibble(payload[i + 2u]);
            if (hi >= 0 && lo >= 0) {
                buf[o++] = (uint8_t)((hi << 4) | lo);
                i += 2u;
                continue;
            }
        }
        buf[o++] = (uint8_t)payload[i];
    }
    buf[o] = 0u;
    *out_bytes = buf;
    *out_len   = o;
    return true;
}

static void fetch_job_thread_main(void *arg) {
    WebAsyncFetchJob *job = (WebAsyncFetchJob *)arg;
    if (!job) return;

    if (job->cancel_flag) {
        arcr_str_copy(job->blocked_reason, sizeof(job->blocked_reason), "cancelled");
        job->finished = 1;
        return;
    }

    if (arcr_str_starts_with_nocase(job->url, "data:")) {
        uint8_t *db = NULL;
        size_t dl = 0u;
        if (decode_data_url(job->url, job->content_type, sizeof(job->content_type), &db, &dl)) {
            job->net_ok     = true;
            job->status     = 200;
            job->resp_bytes = db;
            job->resp_len   = dl;
            arcr_str_copy(job->final_url, sizeof(job->final_url), job->url);
        } else {
            arcr_str_copy(job->blocked_reason, sizeof(job->blocked_reason), "net-error");
        }
        job->finished = 1;
        return;
    }

    if (arcr_str_starts_with_nocase(job->url, "blob:")) {
        bool bpriv = false;
        char borig[256] = {0};
        uint8_t *bb = NULL;
        size_t bl = 0u;
        if (global_blob_lookup(job->url, &bpriv, borig, sizeof(borig),
                               &bb, &bl, job->content_type, sizeof(job->content_type)) &&
            bpriv == job->host_snap.private_browsing) {
            job->net_ok     = true;
            job->status     = 200;
            job->resp_bytes = bb;
            job->resp_len   = bl;
            arcr_str_copy(job->final_url, sizeof(job->final_url), job->url);
        } else {
            free(bb);
            arcr_str_copy(job->blocked_reason, sizeof(job->blocked_reason), "net-error");
        }
        job->finished = 1;
        return;
    }

    if (job->host_snap.http_fetch_ex) {
        char *rh = NULL;
        size_t rh_len = 0u;
        uint8_t *rb = NULL;
        size_t rb_len = 0u;
        job->net_ok = job->host_snap.http_fetch_ex(
            job->host_snap.userdata, job->url, job->method,
            job->req_headers_raw, job->req_body, job->req_body_len,
            job->include_credentials, &job->cancel_flag,
            &job->status, job->final_url, sizeof(job->final_url),
            job->content_type, sizeof(job->content_type),
            &rh, &rh_len, &rb, &rb_len,
            job->blocked_reason, sizeof(job->blocked_reason));

        if (rh) {
            job->resp_headers_raw = (char *)malloc(rh_len + 1u);
            if (job->resp_headers_raw) {
                memcpy(job->resp_headers_raw, rh, rh_len);
                job->resp_headers_raw[rh_len] = '\0';
                job->resp_headers_len = rh_len;
            }
            if (job->host_snap.free_host_buffer) job->host_snap.free_host_buffer(job->host_snap.userdata, rh);
            else free(rh);
        }
        if (rb) {
            job->resp_bytes = (uint8_t *)malloc(rb_len + 1u);
            if (job->resp_bytes) {
                if (rb_len > 0u) memcpy(job->resp_bytes, rb, rb_len);
                job->resp_bytes[rb_len] = 0u;
                job->resp_len = rb_len;
            }
            if (job->host_snap.free_host_buffer) job->host_snap.free_host_buffer(job->host_snap.userdata, rb);
            else if (job->host_snap.http_free)   job->host_snap.http_free(job->host_snap.userdata, rb);
            else free(rb);
        }
    } else if (job->host_snap.http_fetch) {
        const char *ct_hdr = NULL;
        char ct_buf[128] = {0};
        if (job->req_headers_raw) {
            const char *p = job->req_headers_raw;
            while (*p) {
                const char *eol = strstr(p, "\r\n");
                size_t ll = eol ? (size_t)(eol - p) : strlen(p);
                if (ll > 13u && arcr_str_eq_nocase_n(p, 13u, "content-type:", 13u)) {
                    const char *v = p + 13;
                    size_t vl = ll - 13u;
                    arcr_str_trim_view(&v, &vl);
                    size_t cp = vl < sizeof(ct_buf) - 1u ? vl : sizeof(ct_buf) - 1u;
                    memcpy(ct_buf, v, cp);
                    ct_buf[cp] = '\0';
                    ct_hdr = ct_buf;
                    break;
                }
                if (!eol) break;
                p = eol + 2;
            }
        }
        uint8_t *rb = NULL;
        size_t rb_len = 0u;
        job->net_ok = job->host_snap.http_fetch(
            job->host_snap.userdata, job->url, job->method,
            (const char *)job->req_body, job->req_body_len, ct_hdr,
            &job->status, job->content_type, sizeof(job->content_type),
            &rb, &rb_len);
        arcr_str_copy(job->final_url, sizeof(job->final_url), job->url);
        if (rb) {
            job->resp_bytes = (uint8_t *)malloc(rb_len + 1u);
            if (job->resp_bytes) {
                if (rb_len > 0u) memcpy(job->resp_bytes, rb, rb_len);
                job->resp_bytes[rb_len] = 0u;
                job->resp_len = rb_len;
            }
            if (job->host_snap.http_free) job->host_snap.http_free(job->host_snap.userdata, rb);
            else free(rb);
        }
    } else {
        arcr_str_copy(job->blocked_reason, sizeof(job->blocked_reason), "net-error");
    }

    if (job->cancel_flag) {
        job->net_ok = false;
        arcr_str_copy(job->blocked_reason, sizeof(job->blocked_reason), "cancelled");
    }
    if (job->final_url[0] == '\0' && job->url) {
        arcr_str_copy(job->final_url, sizeof(job->final_url), job->url);
    }
    job->finished = 1;
}

static void fetch_job_free(WebAsyncFetchJob *job) {
    if (!job) return;
    if (job->thread_started) {
        web_thread_join(job->thread);
        job->thread_started = false;
    }
    free(job->url);
    free(job->method);
    free(job->req_headers_raw);
    free(job->req_body);
    free(job->resp_headers_raw);
    free(job->resp_bytes);
    free(job);
}

/* =========================================================================
 * 6. Dedicated Multi-Threaded Worker (`WebWorkerInstance`)
 * ========================================================================= */
static void worker_msg_free(WebWorkerMsg *m) {
    if (!m) return;
    free(m->payload_json);
    if (m->buffers) {
        for (size_t i = 0u; i < m->buffer_count; ++i) free(m->buffers[i]);
        free(m->buffers);
    }
    free(m->buffer_lens);
    free(m);
}

static void worker_push_to_main(WebWorkerInstance *w, WebWorkerMsg *m) {
    if (!w || !m) return;
    m->next = NULL;
    web_mutex_lock(&w->mtx);
    if (w->to_main_tail) w->to_main_tail->next = m;
    else                 w->to_main_head       = m;
    w->to_main_tail = m;
    web_mutex_unlock(&w->mtx);
}

static AjsValue b_worker_self_post(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    WebWorkerInstance *w = (WebWorkerInstance *)ud;
    if (!w || w->closing || argc < 1) return ajs_undefined();

    size_t jlen = 0u;
    char *json = ajs_to_cstring_len(avm, argv[0], &jlen);
    if (!json) return ajs_undefined();

    WebWorkerMsg *m = (WebWorkerMsg *)calloc(1u, sizeof(WebWorkerMsg));
    if (!m) {
        ajs_free_cstring(avm, json);
        return ajs_undefined();
    }
    m->payload_json = (char *)malloc(jlen + 1u);
    if (!m->payload_json) {
        ajs_free_cstring(avm, json);
        free(m);
        return ajs_undefined();
    }
    memcpy(m->payload_json, json, jlen);
    m->payload_json[jlen] = '\0';
    m->payload_len = jlen;
    ajs_free_cstring(avm, json);

    if (argc >= 2 && ajs_is_array(avm, argv[1])) {
        AjsValue lval = ajs_get_prop_str(avm, argv[1], "length");
        uint32_t cnt = 0u;
        (void)ajs_to_uint32(avm, lval, &cnt);
        if (cnt > 0u && cnt <= 256u) {
            m->buffers     = (uint8_t **)calloc(cnt, sizeof(uint8_t *));
            m->buffer_lens = (size_t *)calloc(cnt, sizeof(size_t));
            if (m->buffers && m->buffer_lens) {
                m->buffer_count = cnt;
                for (uint32_t i = 0u; i < cnt; ++i) {
                    AjsValue ab = ajs_get_prop_index(avm, argv[1], i);
                    size_t blen = 0u;
                    uint8_t *ptr = ajs_get_array_buffer_data(avm, ab, &blen);
                    m->buffers[i] = (uint8_t *)malloc(blen + 1u);
                    if (m->buffers[i]) {
                        if (ptr && blen > 0u) memcpy(m->buffers[i], ptr, blen);
                        m->buffers[i][blen] = 0u;
                        m->buffer_lens[i] = blen;
                    }
                    (void)ajs_detach_array_buffer(avm, ab);
                }
            }
        }
    }

    worker_push_to_main(w, m);
    return ajs_undefined();
}

static AjsValue b_worker_self_close(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)avm; (void)this_val; (void)argc; (void)argv;
    WebWorkerInstance *w = (WebWorkerInstance *)ud;
    if (w) w->closing = 1;
    return ajs_undefined();
}

static const char *worker_host_get_url(void *userdata) {
    const WebWorkerInstance *w = (const WebWorkerInstance *)userdata;
    return w ? w->script_url : "about:blank";
}

static void worker_post_error_msg(WebWorkerInstance *w, const char *msg_text) {
    WebWorkerMsg *em = (WebWorkerMsg *)calloc(1u, sizeof(WebWorkerMsg));
    if (!em) return;
    em->is_error = true;
    const char *s = msg_text ? msg_text : "Worker script error";
    size_t sl = strlen(s);
    em->payload_json = (char *)malloc(sl + 1u);
    if (em->payload_json) {
        memcpy(em->payload_json, s, sl + 1u);
        em->payload_len = sl;
    }
    worker_push_to_main(w, em);
}

/* Forward Proxy callbacks from Worker's ArcrJsHostCallbacks to parent host_snap */
static void worker_cb_console(void *ud, const char *msg) {
    WebWorkerInstance *w = (WebWorkerInstance *)ud;
    if (w && w->host_snap.console_log) w->host_snap.console_log(w->host_snap.userdata, msg);
}
static bool worker_cb_storage_get(void *ud, const char *k, char *out, size_t cap) {
    WebWorkerInstance *w = (WebWorkerInstance *)ud;
    return (w && w->host_snap.storage_get) ? w->host_snap.storage_get(w->host_snap.userdata, k, out, cap) : false;
}
static void worker_cb_storage_set(void *ud, const char *k, const char *v) {
    WebWorkerInstance *w = (WebWorkerInstance *)ud;
    if (w && w->host_snap.storage_set) w->host_snap.storage_set(w->host_snap.userdata, k, v);
}
static bool worker_cb_http_fetch(void *ud, const char *url, const char *method,
                                 const char *body, size_t blen, const char *ct,
                                 int *st, char *rct, size_t ccap, uint8_t **rb, size_t *rlen) {
    WebWorkerInstance *w = (WebWorkerInstance *)ud;
    return (w && w->host_snap.http_fetch)
               ? w->host_snap.http_fetch(w->host_snap.userdata, url, method, body, blen, ct, st, rct, ccap, rb, rlen)
               : false;
}
static void worker_cb_http_free(void *ud, uint8_t *rb) {
    WebWorkerInstance *w = (WebWorkerInstance *)ud;
    if (w && w->host_snap.http_free) w->host_snap.http_free(w->host_snap.userdata, rb);
    else free(rb);
}
static bool worker_cb_load_mod(void *ud, const char *rurl, const char *ref,
                               char **out_code, size_t *out_len, char *furl, size_t fcap,
                               char *mime, size_t mcap, char *err, size_t ecap) {
    WebWorkerInstance *w = (WebWorkerInstance *)ud;
    return (w && w->host_snap.load_module_source)
               ? w->host_snap.load_module_source(w->host_snap.userdata, rurl, ref, out_code, out_len, furl, fcap, mime, mcap, err, ecap)
               : false;
}
static void worker_cb_free_buf(void *ud, void *buf) {
    WebWorkerInstance *w = (WebWorkerInstance *)ud;
    if (w && w->host_snap.free_host_buffer) w->host_snap.free_host_buffer(w->host_snap.userdata, buf);
    else free(buf);
}
static bool worker_cb_fetch_ex(void *ud, const char *url, const char *method,
                               const char *rhdr, const uint8_t *body, size_t blen,
                               bool cred, const volatile int *cflag,
                               int *st, char *furl, size_t fcap, char *ct, size_t ctcap,
                               char **outhdr, size_t *outhdrlen, uint8_t **outb, size_t *outblen,
                               char *reason, size_t rcap) {
    WebWorkerInstance *w = (WebWorkerInstance *)ud;
    return (w && w->host_snap.http_fetch_ex)
               ? w->host_snap.http_fetch_ex(w->host_snap.userdata, url, method, rhdr, body, blen,
                                            cred, cflag, st, furl, fcap, ct, ctcap,
                                            outhdr, outhdrlen, outb, outblen, reason, rcap)
               : false;
}

static void worker_thread_main(void *arg) {
    WebWorkerInstance *w = (WebWorkerInstance *)arg;
    if (!w) return;

    char *code = NULL;
    size_t code_len = 0u;
    bool free_with_host = false;

    if (w->inline_code) {
        code = w->inline_code;
        code_len = w->inline_code_len;
        w->inline_code = NULL;
    } else if (arcr_str_starts_with_nocase(w->script_url, "data:")) {
        char mime[128] = {0};
        uint8_t *db = NULL;
        if (decode_data_url(w->script_url, mime, sizeof(mime), &db, &code_len)) {
            code = (char *)db;
        }
    } else if (arcr_str_starts_with_nocase(w->script_url, "blob:")) {
        uint8_t *bb = NULL;
        if (global_blob_lookup(w->script_url, NULL, NULL, 0u, &bb, &code_len, NULL, 0u)) {
            code = (char *)bb;
        }
    } else if (w->host_snap.load_module_source) {
        char furl[2048] = {0}, mime[128] = {0}, err[512] = {0};
        if (w->host_snap.load_module_source(w->host_snap.userdata, w->script_url, w->script_url,
                                            &code, &code_len, furl, sizeof(furl),
                                            mime, sizeof(mime), err, sizeof(err))) {
            if (furl[0]) arcr_str_copy(w->script_url, sizeof(w->script_url), furl);
            free_with_host = true;
        } else {
            worker_post_error_msg(w, err[0] ? err : "Failed to load Worker script");
            w->busy = 0;
            w->finished = 1;
            return;
        }
    } else if (w->host_snap.http_fetch_ex || w->host_snap.http_fetch) {
        int st = 0;
        char furl[2048] = {0}, ct[256] = {0}, reason[128] = {0};
        uint8_t *rb = NULL;
        size_t rblen = 0u;
        bool ok = false;
        if (w->host_snap.http_fetch_ex) {
            ok = w->host_snap.http_fetch_ex(w->host_snap.userdata, w->script_url, "GET",
                                            NULL, NULL, 0u, true, &w->closing,
                                            &st, furl, sizeof(furl), ct, sizeof(ct),
                                            NULL, NULL, &rb, &rblen, reason, sizeof(reason));
        } else {
            ok = w->host_snap.http_fetch(w->host_snap.userdata, w->script_url, "GET",
                                         NULL, 0u, NULL, &st, ct, sizeof(ct), &rb, &rblen);
        }
        if (ok && st >= 200 && st < 300 && rb) {
            code = (char *)malloc(rblen + 1u);
            if (code) {
                memcpy(code, rb, rblen);
                code[rblen] = '\0';
                code_len = rblen;
            }
            if (w->host_snap.free_host_buffer) w->host_snap.free_host_buffer(w->host_snap.userdata, rb);
            else if (w->host_snap.http_free)   w->host_snap.http_free(w->host_snap.userdata, rb);
            else free(rb);
        } else {
            if (rb) {
                if (w->host_snap.free_host_buffer) w->host_snap.free_host_buffer(w->host_snap.userdata, rb);
                else if (w->host_snap.http_free)   w->host_snap.http_free(w->host_snap.userdata, rb);
                else free(rb);
            }
            worker_post_error_msg(w, "Failed to fetch Worker script");
            w->busy = 0;
            w->finished = 1;
            return;
        }
    }

    if (!code || w->closing) {
        if (code) {
            if (free_with_host && w->host_snap.free_host_buffer) w->host_snap.free_host_buffer(w->host_snap.userdata, code);
            else free(code);
        }
        if (!w->closing) worker_post_error_msg(w, "Empty or missing Worker script");
        w->busy = 0;
        w->finished = 1;
        return;
    }

    ArcrJsHostCallbacks whcb;
    memset(&whcb, 0, sizeof(whcb));
    whcb.userdata           = w;
    whcb.doc                = NULL;
    whcb.viewport_width     = w->host_snap.viewport_width;
    whcb.viewport_height    = w->host_snap.viewport_height;
    whcb.prefers_dark       = w->host_snap.prefers_dark;
    whcb.private_browsing   = w->host_snap.private_browsing;
    whcb.get_current_url    = worker_host_get_url;
    whcb.console_log        = worker_cb_console;
    whcb.storage_get        = worker_cb_storage_get;
    whcb.storage_set        = worker_cb_storage_set;
    whcb.http_fetch         = worker_cb_http_fetch;
    whcb.http_free          = worker_cb_http_free;
    whcb.load_module_source = worker_cb_load_mod;
    whcb.free_host_buffer   = worker_cb_free_buf;
    whcb.http_fetch_ex      = worker_cb_fetch_ex;

    ArcrJsVm *wvm = web_create_worker_vm(w->heap_limit, &whcb, w->script_url);
    if (!wvm) {
        if (free_with_host && w->host_snap.free_host_buffer) w->host_snap.free_host_buffer(w->host_snap.userdata, code);
        else free(code);
        worker_post_error_msg(w, "Failed to create Worker JS VM");
        w->busy = 0;
        w->finished = 1;
        return;
    }

    /* Bind Worker self postMessage / close onto bridge object */
    {
        AjsHandleScope sc;
        ajs_handle_scope_open(wvm->avm, &sc);
        AjsValue bobj = ajs_get_pinned(wvm->avm, wvm->pin_bridge);
        web_bind_fn(wvm->avm, bobj, "workerSelfPost",  b_worker_self_post,  2, w);
        web_bind_fn(wvm->avm, bobj, "workerSelfClose", b_worker_self_close, 0, w);
        ajs_handle_scope_close(wvm->avm, &sc);
    }

    bool eval_ok = web_eval_worker_entry(wvm, code, code_len, w->script_url, w->is_module);
    if (free_with_host && w->host_snap.free_host_buffer) w->host_snap.free_host_buffer(w->host_snap.userdata, code);
    else free(code);

    if (!eval_ok) {
        worker_post_error_msg(w, "Uncaught exception evaluating Worker entry script");
    }

    while (!w->closing) {
        WebWorkerMsg *batch = NULL;
        web_mutex_lock(&w->mtx);
        batch = w->to_worker_head;
        w->to_worker_head = NULL;
        w->to_worker_tail = NULL;
        web_mutex_unlock(&w->mtx);

        while (batch && !w->closing) {
            WebWorkerMsg *nxt = batch->next;
            AjsHandleScope sc;
            ajs_handle_scope_open(wvm->avm, &sc);
            AjsValue glob = ajs_global_object(wvm->avm);
            AjsValue fn = ajs_get_prop_str(wvm->avm, glob, "__dispatchWorkerInbound");
            if (ajs_is_callable(wvm->avm, fn)) {
                AjsValue jstr = ajs_string_utf8(wvm->avm,
                                                batch->payload_json ? batch->payload_json : "null",
                                                batch->payload_len);
                AjsValue barr = ajs_new_array(wvm->avm, (uint32_t)batch->buffer_count);
                for (size_t bi = 0u; bi < batch->buffer_count; ++bi) {
                    AjsValue ab = ajs_new_array_buffer_copy(
                        wvm->avm,
                        batch->buffers[bi] ? batch->buffers[bi] : (const uint8_t *)"",
                        batch->buffer_lens[bi]);
                    (void)ajs_set_prop_index(wvm->avm, barr, (uint32_t)bi, ab);
                }
                AjsValue args[2] = { jstr, barr };
                wvm->deadline_usec   = arcr_platform_monotonic_usec() + ARCR_JS_TURN_DEADLINE_USEC;
                wvm->interrupt_armed = true;
                (void)ajs_call(wvm->avm, fn, glob, 2, args);
                if (ajs_has_exception(wvm->avm)) {
                    AjsErrorInfo ei;
                    (void)ajs_take_exception(wvm->avm, &ei);
                    web_log_ajs_error(wvm, "worker/message", &ei);
                    worker_post_error_msg(w, ei.message[0] ? ei.message : "Worker message error");
                }
                (void)web_drain_microtasks_guarded(wvm);
                wvm->interrupt_armed = false;
            }
            ajs_handle_scope_close(wvm->avm, &sc);
            worker_msg_free(batch);
            batch = nxt;
        }
        while (batch) {
            WebWorkerMsg *nxt = batch->next;
            worker_msg_free(batch);
            batch = nxt;
        }

        if (w->closing) break;
        (void)web_tick_worker_vm(wvm, arcr_platform_monotonic_usec() / 1000ULL);

        bool more_in = false;
        web_mutex_lock(&w->mtx);
        more_in = (w->to_worker_head != NULL);
        web_mutex_unlock(&w->mtx);

        if (more_in || web_worker_vm_has_work(wvm)) {
            w->busy = 1;
            web_sleep_ms(1u);
        } else {
            w->busy = 0;
            web_sleep_ms(2u);
        }
    }

    web_destroy_vm_internal(wvm);
    w->busy     = 0;
    w->finished = 1;
}

static void worker_free_instance(WebWorkerInstance *w) {
    if (!w) return;
    w->closing = 1;
    if (w->thread_started) {
        web_thread_join(w->thread);
        w->thread_started = false;
    }
    WebWorkerMsg *m = w->to_worker_head;
    while (m) { WebWorkerMsg *n = m->next; worker_msg_free(m); m = n; }
    m = w->to_main_head;
    while (m) { WebWorkerMsg *n = m->next; worker_msg_free(m); m = n; }
    free(w->inline_code);
    web_mutex_destroy(&w->mtx);
    free(w);
}

/* =========================================================================
 * 7. Native JS Bridge Functions Registered on `B`
 * ========================================================================= */
static AjsValue b_crypto_random_bytes(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 1) return ajs_int32(-1);
    size_t blen = 0u;
    AjsTypedArrayKind kind = AJS_TA_NONE;
    uint8_t *ptr = ajs_get_buffer_source_bytes(avm, argv[0], &blen, &kind);
    /* WebCrypto spec: getRandomValues accepts ONLY integer TypedArrays, NOT Float32/Float64/DataView */
    if (kind == AJS_TA_NONE || kind == AJS_TA_FLOAT32 || kind == AJS_TA_FLOAT64) {
        return ajs_int32(-1); /* TypeMismatchError */
    }
    if (blen > 65536u) {
        return ajs_int32(-2); /* QuotaExceededError */
    }
    if (ptr && blen > 0u) {
        if (!web_os_csprng_fill(ptr, blen)) return ajs_int32(-3);
    }
    return ajs_int32(0);
}

static AjsValue b_crypto_random_uuid(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)argc; (void)argv; (void)ud;
    uint8_t b[16];
    if (!web_os_csprng_fill(b, sizeof(b))) {
        memset(b, 0x42, sizeof(b));
    }
    b[6] = (uint8_t)((b[6] & 0x0Fu) | 0x40u);
    b[8] = (uint8_t)((b[8] & 0x3Fu) | 0x80u);
    char uuid[37];
    snprintf(uuid, sizeof(uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return ajs_string_cstr(avm, uuid);
}

static AjsValue b_subtle_digest(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 2) return ajs_null();
    int32_t alg = 0;
    (void)ajs_to_int32(avm, argv[0], &alg);
    size_t dlen = 0u;
    uint8_t *data = ajs_get_buffer_source_bytes(avm, argv[1], &dlen, NULL);
    if (!data && dlen > 0u) return ajs_null();
    uint8_t out[64];
    size_t out_len = 0u;
    if (!web_hash_once((int)alg, data, dlen, out, &out_len)) return ajs_null();
    return ajs_new_array_buffer_copy(avm, out, out_len);
}

static AjsValue b_subtle_hmac(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 3) return ajs_null();
    int32_t alg = 0;
    (void)ajs_to_int32(avm, argv[0], &alg);
    size_t klen = 0u, dlen = 0u;
    uint8_t *key  = ajs_get_buffer_source_bytes(avm, argv[1], &klen, NULL);
    uint8_t *data = ajs_get_buffer_source_bytes(avm, argv[2], &dlen, NULL);
    if ((!key && klen > 0u) || (!data && dlen > 0u)) return ajs_null();
    uint8_t out[64];
    size_t out_len = 0u;
    if (!web_hmac_compute((int)alg, key, klen, data, dlen, out, &out_len)) return ajs_null();
    return ajs_new_array_buffer_copy(avm, out, out_len);
}

static AjsValue b_subtle_pbkdf2(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 5) return ajs_null();
    int32_t alg = 0, bit_len = 0;
    uint32_t iters = 0u;
    (void)ajs_to_int32(avm, argv[0], &alg);
    size_t plen = 0u, slen = 0u;
    uint8_t *pass = ajs_get_buffer_source_bytes(avm, argv[1], &plen, NULL);
    uint8_t *salt = ajs_get_buffer_source_bytes(avm, argv[2], &slen, NULL);
    (void)ajs_to_uint32(avm, argv[3], &iters);
    (void)ajs_to_int32(avm, argv[4], &bit_len);
    if (iters == 0u || bit_len < 0 || (bit_len % 8) != 0 || bit_len > 65536) return ajs_null();
    size_t byte_len = (size_t)(bit_len / 8);
    uint8_t *out = (uint8_t *)malloc(byte_len + 1u);
    if (!out) return ajs_null();
    bool ok = web_pbkdf2_derive((int)alg, pass, plen, salt, slen, iters, out, byte_len);
    AjsValue ret = ok ? ajs_new_array_buffer_copy(avm, out, byte_len) : ajs_null();
    free(out);
    return ret;
}

static AjsValue b_subtle_hkdf(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 5) return ajs_null();
    int32_t alg = 0, bit_len = 0;
    (void)ajs_to_int32(avm, argv[0], &alg);
    size_t klen = 0u, slen = 0u, ilen = 0u;
    uint8_t *ikm  = ajs_get_buffer_source_bytes(avm, argv[1], &klen, NULL);
    uint8_t *salt = ajs_get_buffer_source_bytes(avm, argv[2], &slen, NULL);
    uint8_t *info = ajs_get_buffer_source_bytes(avm, argv[3], &ilen, NULL);
    (void)ajs_to_int32(avm, argv[4], &bit_len);
    if (bit_len < 0 || (bit_len % 8) != 0 || bit_len > 65536) return ajs_null();
    size_t byte_len = (size_t)(bit_len / 8);
    uint8_t *out = (uint8_t *)malloc(byte_len + 1u);
    if (!out) return ajs_null();
    bool ok = web_hkdf_derive((int)alg, ikm, klen, salt, slen, info, ilen, out, byte_len);
    AjsValue ret = ok ? ajs_new_array_buffer_copy(avm, out, byte_len) : ajs_null();
    free(out);
    return ret;
}

static AjsValue b_subtle_aes_gcm(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 6) return ajs_null();
    bool encrypt = ajs_to_bool(avm, argv[0]);
    size_t klen = 0u, ivlen = 0u, aadlen = 0u, dlen = 0u;
    uint8_t *key  = ajs_get_buffer_source_bytes(avm, argv[1], &klen, NULL);
    uint8_t *iv   = ajs_get_buffer_source_bytes(avm, argv[2], &ivlen, NULL);
    uint8_t *aad  = ajs_is_nullish(argv[3]) ? NULL : ajs_get_buffer_source_bytes(avm, argv[3], &aadlen, NULL);
    int32_t tag_bits = 128;
    (void)ajs_to_int32(avm, argv[4], &tag_bits);
    uint8_t *data = ajs_get_buffer_source_bytes(avm, argv[5], &dlen, NULL);
    if (!key || !iv || ivlen == 0u || (tag_bits % 8) != 0 || tag_bits < 32 || tag_bits > 128) {
        return ajs_null();
    }
    static const uint8_t kEmpty = 0u;
    uint8_t *out = NULL;
    size_t out_len = 0u;
    if (!aes_gcm_crypt(encrypt, key, klen, iv, ivlen, aad, aadlen,
                       (size_t)(tag_bits / 8), data ? data : &kEmpty, dlen, &out, &out_len)) {
        return ajs_null();
    }
    AjsValue ret = ajs_new_array_buffer_copy(avm, out ? out : &kEmpty, out_len);
    free(out);
    return ret;
}

static AjsValue b_utf8_encode(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 1) return ajs_new_typed_array(avm, AJS_TA_UINT8, NULL, 0u);
    size_t blen = 0u;
    char *s = ajs_to_cstring_len(avm, argv[0], &blen);
    if (!s) return ajs_new_typed_array(avm, AJS_TA_UINT8, NULL, 0u);
    AjsValue u8 = ajs_new_typed_array(avm, AJS_TA_UINT8, (const uint8_t *)s, blen);
    ajs_free_cstring(avm, s);
    return u8;
}

static AjsValue b_copy_buffer_bytes(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 1) return ajs_null();
    size_t blen = 0u;
    AjsTypedArrayKind kind = AJS_TA_NONE;
    uint8_t *ptr = ajs_get_buffer_source_bytes(avm, argv[0], &blen, &kind);
    if (!ptr && blen > 0u) return ajs_null();
    /* Verify it is actually an ArrayBuffer or view */
    size_t raw_ab_len = 0u;
    if (!ptr && kind == AJS_TA_NONE && !ajs_get_array_buffer_data(avm, argv[0], &raw_ab_len)) {
        /* Could be a 0-byte ArrayBuffer or non-buffer object; check byteLength */
        if (!ajs_has_prop_str(avm, argv[0], "byteLength")) return ajs_null();
    }
    static const uint8_t kEmpty = 0u;
    return ajs_new_array_buffer_copy(avm, ptr ? ptr : &kEmpty, blen);
}

static AjsValue b_detach_array_buffer(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)ud;
    if (argc < 1) return ajs_bool(false);
    return ajs_bool(ajs_detach_array_buffer(avm, argv[0]));
}

static AjsValue b_blob_url_op(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 2) return ajs_null();
    int32_t op = 0;
    (void)ajs_to_int32(avm, argv[0], &op);

    if (op == 0) { /* createObjectURL(buffer, mimeStr) -> url */
        size_t blen = 0u;
        uint8_t *ptr = ajs_get_buffer_source_bytes(avm, argv[1], &blen, NULL);
        char *mime = (argc >= 3 && ajs_is_string(avm, argv[2])) ? ajs_to_cstring(avm, argv[2]) : NULL;

        uint8_t rnd[16];
        (void)web_os_csprng_fill(rnd, sizeof(rnd));
        rnd[6] = (uint8_t)((rnd[6] & 0x0Fu) | 0x40u);
        rnd[8] = (uint8_t)((rnd[8] & 0x3Fu) | 0x80u);

        char origin[256];
        web_bc_Current_origin(vm, origin, sizeof(origin));

        WebBlobUrlEntry *ent = (WebBlobUrlEntry *)calloc(1u, sizeof(WebBlobUrlEntry));
        if (!ent) {
            if (mime) ajs_free_cstring(avm, mime);
            return ajs_null();
        }
        snprintf(ent->url, sizeof(ent->url),
                 "blob:%.190s/%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 origin,
                 rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7],
                 rnd[8], rnd[9], rnd[10], rnd[11], rnd[12], rnd[13], rnd[14], rnd[15]);
        arcr_str_copy(ent->mime, sizeof(ent->mime), (mime && mime[0]) ? mime : "application/octet-stream");
        ent->bytes = (uint8_t *)malloc(blen + 1u);
        if (ent->bytes) {
            if (ptr && blen > 0u) memcpy(ent->bytes, ptr, blen);
            ent->bytes[blen] = 0u;
            ent->len = blen;
        }
        ent->next = vm->blob_urls_head;
        vm->blob_urls_head = ent;

        GlobalBlobNode *gn = (GlobalBlobNode *)calloc(1u, sizeof(GlobalBlobNode));
        if (gn) {
            gn->owner_vm_id      = vm->vm_instance_id;
            gn->private_browsing = vm->host.private_browsing;
            arcr_str_copy(gn->origin, sizeof(gn->origin), origin);
            arcr_str_copy(gn->url, sizeof(gn->url), ent->url);
            arcr_str_copy(gn->mime, sizeof(gn->mime), ent->mime);
            gn->bytes = (uint8_t *)malloc(blen + 1u);
            if (gn->bytes) {
                if (ptr && blen > 0u) memcpy(gn->bytes, ptr, blen);
                gn->bytes[blen] = 0u;
                gn->len = blen;
            }
            web_ipc_lock();
            gn->next = g_blobs_head;
            g_blobs_head = gn;
            web_ipc_unlock();
        }

        if (mime) ajs_free_cstring(avm, mime);
        return ajs_string_cstr(avm, ent->url);
    }

    if (op == 1) { /* revokeObjectURL(url) */
        char *u = ajs_to_cstring(avm, argv[1]);
        if (u) {
            WebBlobUrlEntry **pp = &vm->blob_urls_head;
            while (*pp) {
                if (strcmp((*pp)->url, u) == 0) {
                    WebBlobUrlEntry *rm = *pp;
                    *pp = rm->next;
                    free(rm->bytes);
                    free(rm);
                    break;
                }
                pp = &(*pp)->next;
            }
            global_blob_remove_for_vm(vm->vm_instance_id, u);
            ajs_free_cstring(avm, u);
        }
        return ajs_undefined();
    }

    return ajs_null();
}

/* --- Async Fetch Bridge (`fetchStart`, `fetchAbort`, `fetchPoll`) --- */
static AjsValue b_fetch_start(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 5) return ajs_uint32(0u);

    char *raw_url = ajs_to_cstring(avm, argv[0]);
    char *method  = ajs_to_cstring(avm, argv[1]);
    char *req_hdr = ajs_is_nullish(argv[2]) ? NULL : ajs_to_cstring(avm, argv[2]);
    size_t body_len = 0u;
    uint8_t *body_ptr = ajs_is_nullish(argv[3]) ? NULL : ajs_get_buffer_source_bytes(avm, argv[3], &body_len, NULL);
    int32_t cred_mode = 1; /* 0=omit, 1=same-origin, 2=include */
    (void)ajs_to_int32(avm, argv[4], &cred_mode);

    char base_url[2048];
    web_effective_base_url(vm, base_url, sizeof(base_url));
    char resolved[2048];
    if (!raw_url || !web_resolve_url(base_url, raw_url, resolved, sizeof(resolved))) {
        if (raw_url) ajs_free_cstring(avm, raw_url);
        if (method)  ajs_free_cstring(avm, method);
        if (req_hdr) ajs_free_cstring(avm, req_hdr);
        return ajs_uint32(0u);
    }

    char page_orig[256], target_orig[256];
    web_compute_origin(base_url, page_orig, sizeof(page_orig));
    web_compute_origin(resolved, target_orig, sizeof(target_orig));
    bool same_orig = (strcmp(page_orig, target_orig) == 0 && strcmp(page_orig, "null") != 0);

    WebAsyncFetchJob *job = (WebAsyncFetchJob *)calloc(1u, sizeof(WebAsyncFetchJob));
    if (!job) {
        ajs_free_cstring(avm, raw_url);
        if (method)  ajs_free_cstring(avm, method);
        if (req_hdr) ajs_free_cstring(avm, req_hdr);
        return ajs_uint32(0u);
    }

    if (vm->next_fetch_id == 0u) vm->next_fetch_id = 1u;
    job->job_id    = vm->next_fetch_id++;
    job->host_snap = vm->host;
    job->include_credentials = (cred_mode == 2) ? true : ((cred_mode == 0) ? false : same_orig);

    size_t ulen = strlen(resolved);
    job->url = (char *)malloc(ulen + 1u);
    if (job->url) memcpy(job->url, resolved, ulen + 1u);

    const char *mstr = (method && method[0]) ? method : "GET";
    size_t mlen = strlen(mstr);
    job->method = (char *)malloc(mlen + 1u);
    if (job->method) memcpy(job->method, mstr, mlen + 1u);

    if (req_hdr && req_hdr[0]) {
        size_t hlen = strlen(req_hdr);
        job->req_headers_raw = (char *)malloc(hlen + 1u);
        if (job->req_headers_raw) memcpy(job->req_headers_raw, req_hdr, hlen + 1u);
    }
    if (body_ptr && body_len > 0u) {
        job->req_body = (uint8_t *)malloc(body_len);
        if (job->req_body) {
            memcpy(job->req_body, body_ptr, body_len);
            job->req_body_len = body_len;
        }
    }

    ajs_free_cstring(avm, raw_url);
    if (method)  ajs_free_cstring(avm, method);
    if (req_hdr) ajs_free_cstring(avm, req_hdr);

    job->next = vm->fetch_jobs_head;
    vm->fetch_jobs_head = job;

    if (web_thread_spawn(&job->thread, fetch_job_thread_main, job)) {
        job->thread_started = true;
    } else {
        /* Synchronous fallback if OS refuses thread creation */
        fetch_job_thread_main(job);
    }
    return ajs_uint32(job->job_id);
}

static AjsValue b_fetch_abort(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 1) return ajs_undefined();
    uint32_t jid = 0u;
    (void)ajs_to_uint32(avm, argv[0], &jid);
    for (WebAsyncFetchJob *j = vm->fetch_jobs_head; j != NULL; j = j->next) {
        if (j->job_id == jid) {
            j->cancel_flag = 1;
            break;
        }
    }
    return ajs_undefined();
}

static AjsValue b_fetch_poll(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val; (void)argc; (void)argv;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    AjsValue arr = ajs_new_array(avm, 0u);
    if (!vm) return arr;

    uint32_t idx = 0u;
    WebAsyncFetchJob **pp = &vm->fetch_jobs_head;
    while (*pp) {
        WebAsyncFetchJob *j = *pp;
        if (j->finished) {
            *pp = j->next;
            if (j->thread_started) {
                web_thread_join(j->thread);
                j->thread_started = false;
            }
            AjsValue o = ajs_new_object(avm);
            (void)ajs_set_prop_str(avm, o, "jobId", ajs_uint32(j->job_id));
            (void)ajs_set_prop_str(avm, o, "netOk", ajs_bool(j->net_ok && !j->cancel_flag));
            (void)ajs_set_prop_str(avm, o, "status", ajs_int32(j->status));
            (void)ajs_set_prop_str(avm, o, "requestedUrl", ajs_string_cstr(avm, j->url ? j->url : ""));
            (void)ajs_set_prop_str(avm, o, "finalUrl", ajs_string_cstr(avm, j->final_url));
            (void)ajs_set_prop_str(avm, o, "contentType", ajs_string_cstr(avm, j->content_type));
            (void)ajs_set_prop_str(avm, o, "rawHeaders",
                                   ajs_string_utf8(avm, j->resp_headers_raw ? j->resp_headers_raw : "",
                                                   j->resp_headers_len));
            (void)ajs_set_prop_str(avm, o, "blockedReason", ajs_string_cstr(avm, j->blocked_reason));
            static const uint8_t kEmpty = 0u;
            AjsValue ab = ajs_new_array_buffer_copy(avm, j->resp_bytes ? j->resp_bytes : &kEmpty, j->resp_len);
            (void)ajs_set_prop_str(avm, o, "buffer", ab);
            (void)ajs_set_prop_index(avm, arr, idx++, o);

            fetch_job_free(j);
        } else {
            pp = &j->next;
        }
    }
    return arr;
}

/* --- BroadcastChannel Bridge (`bcOp`) --- */
static AjsValue b_bc_op(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 1) return ajs_undefined();
    int32_t op = 0;
    (void)ajs_to_int32(avm, argv[0], &op);

    if (op == 0 && argc >= 2) { /* open(channelName) -> subId */
        char *ch = ajs_to_cstring(avm, argv[1]);
        if (!ch) return ajs_uint32(0u);
        WebBcSub *sub = (WebBcSub *)calloc(1u, sizeof(WebBcSub));
        if (!sub) {
            ajs_free_cstring(avm, ch);
            return ajs_uint32(0u);
        }
        char orig[256];
        web_bc_Current_origin(vm, orig, sizeof(orig));
        web_ipc_lock();
        sub->sub_id           = g_bc_next_sub++;
        sub->vm_instance_id   = vm->vm_instance_id;
        sub->private_browsing = vm->host.private_browsing;
        arcr_str_copy(sub->origin, sizeof(sub->origin), orig);
        arcr_str_copy(sub->channel, sizeof(sub->channel), ch);
        sub->last_seq         = g_bc_seq;
        sub->next             = g_bc_subs_head;
        g_bc_subs_head        = sub;
        uint32_t sid = sub->sub_id;
        web_ipc_unlock();
        ajs_free_cstring(avm, ch);
        return ajs_uint32(sid);
    }

    if (op == 1 && argc >= 3) { /* post(subId, jsonPayload) */
        uint32_t sid = 0u;
        (void)ajs_to_uint32(avm, argv[1], &sid);
        size_t jlen = 0u;
        char *json = ajs_to_cstring_len(avm, argv[2], &jlen);
        if (!json) return ajs_bool(false);

        bool posted = false;
        web_ipc_lock();
        WebBcSub *sender = NULL;
        for (WebBcSub *s = g_bc_subs_head; s != NULL; s = s->next) {
            if (s->sub_id == sid && s->vm_instance_id == vm->vm_instance_id) { sender = s; break; }
        }
        if (sender) {
            WebBcMsg *m = (WebBcMsg *)calloc(1u, sizeof(WebBcMsg));
            if (m) {
                m->seq              = ++g_bc_seq;
                m->sender_sub_id    = sid;
                m->private_browsing = sender->private_browsing;
                arcr_str_copy(m->origin, sizeof(m->origin), sender->origin);
                arcr_str_copy(m->channel, sizeof(m->channel), sender->channel);
                m->payload_json = (char *)malloc(jlen + 1u);
                if (m->payload_json) {
                    memcpy(m->payload_json, json, jlen + 1u);
                    if (g_bc_msgs_tail) g_bc_msgs_tail->next = m;
                    else                g_bc_msgs_head       = m;
                    g_bc_msgs_tail = m;
                    g_bc_msg_count++;
                    while (g_bc_msg_count > 256u && g_bc_msgs_head) {
                        WebBcMsg *old = g_bc_msgs_head;
                        g_bc_msgs_head = old->next;
                        if (!g_bc_msgs_head) g_bc_msgs_tail = NULL;
                        free(old->payload_json);
                        free(old);
                        g_bc_msg_count--;
                    }
                    posted = true;
                } else {
                    free(m);
                }
            }
        }
        web_ipc_unlock();
        ajs_free_cstring(avm, json);
        return ajs_bool(posted);
    }

    if (op == 2 && argc >= 2) { /* close(subId) */
        uint32_t sid = 0u;
        (void)ajs_to_uint32(avm, argv[1], &sid);
        web_ipc_lock();
        WebBcSub **pp = &g_bc_subs_head;
        while (*pp) {
            if ((*pp)->sub_id == sid && (*pp)->vm_instance_id == vm->vm_instance_id) {
                WebBcSub *rm = *pp;
                *pp = rm->next;
                free(rm);
                break;
            }
            pp = &(*pp)->next;
        }
        web_ipc_unlock();
        return ajs_undefined();
    }

    if (op == 3) { /* poll() -> [{subId, payloadJson, origin}] */
        typedef struct BcHit {
            uint32_t sub_id;
            char     origin[256];
            char    *json;
        } BcHit;
        BcHit hits[64];
        size_t hcnt = 0u;

        web_ipc_lock();
        for (WebBcSub *s = g_bc_subs_head; s != NULL && hcnt < 64u; s = s->next) {
            if (s->vm_instance_id != vm->vm_instance_id) continue;
            for (const WebBcMsg *m = g_bc_msgs_head; m != NULL && hcnt < 64u; m = m->next) {
                if (m->seq <= s->last_seq) continue;
                s->last_seq = m->seq;
                if (m->sender_sub_id == s->sub_id) continue;
                if (m->private_browsing != s->private_browsing) continue;
                if (strcmp(m->origin, s->origin) != 0) continue;
                if (strcmp(m->channel, s->channel) != 0) continue;

                size_t jl = strlen(m->payload_json);
                char *cp = (char *)malloc(jl + 1u);
                if (cp) {
                    memcpy(cp, m->payload_json, jl + 1u);
                    hits[hcnt].sub_id = s->sub_id;
                    arcr_str_copy(hits[hcnt].origin, sizeof(hits[hcnt].origin), m->origin);
                    hits[hcnt].json = cp;
                    hcnt++;
                }
            }
        }
        web_ipc_unlock();

        AjsValue arr = ajs_new_array(avm, (uint32_t)hcnt);
        for (size_t i = 0u; i < hcnt; ++i) {
            AjsValue o = ajs_new_object(avm);
            (void)ajs_set_prop_str(avm, o, "subId", ajs_uint32(hits[i].sub_id));
            (void)ajs_set_prop_str(avm, o, "origin", ajs_string_cstr(avm, hits[i].origin));
            (void)ajs_set_prop_str(avm, o, "payloadJson", ajs_string_cstr(avm, hits[i].json));
            (void)ajs_set_prop_index(avm, arr, (uint32_t)i, o);
            free(hits[i].json);
        }
        return arr;
    }

    return ajs_undefined();
}

/* --- IndexedDB Backing Store Bridge (`idbOp`) --- */
static AjsValue b_idb_op(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 1) return ajs_null();
    int32_t op = 0;
    (void)ajs_to_int32(avm, argv[0], &op);

    char orig[256];
    web_bc_Current_origin(vm, orig, sizeof(orig));
    bool priv = vm->host.private_browsing;

    if (op == 0 && argc >= 2) { /* load(dbName) -> {exists, version, snapshotJson} */
        char *dbn = ajs_to_cstring(avm, argv[1]);
        if (!dbn) return ajs_null();
        bool exists = false;
        uint64_t ver = 0u;
        char *snap_copy = NULL;

        web_ipc_lock();
        for (const WebIdbEntry *e = g_idb_head; e != NULL; e = e->next) {
            if (e->private_browsing == priv &&
                strcmp(e->origin, orig) == 0 &&
                strcmp(e->db_name, dbn) == 0) {
                exists = true;
                ver = e->version;
                if (e->snapshot_json) {
                    size_t sl = strlen(e->snapshot_json);
                    snap_copy = (char *)malloc(sl + 1u);
                    if (snap_copy) memcpy(snap_copy, e->snapshot_json, sl + 1u);
                }
                break;
            }
        }
        web_ipc_unlock();

        AjsValue o = ajs_new_object(avm);
        (void)ajs_set_prop_str(avm, o, "exists", ajs_bool(exists));
        (void)ajs_set_prop_str(avm, o, "version", ajs_number((double)ver));
        (void)ajs_set_prop_str(avm, o, "snapshotJson",
                               snap_copy ? ajs_string_cstr(avm, snap_copy) : ajs_null());
        free(snap_copy);
        ajs_free_cstring(avm, dbn);
        return o;
    }

    if (op == 1 && argc >= 4) { /* commit(dbName, version, snapshotJson) */
        char *dbn = ajs_to_cstring(avm, argv[1]);
        double ver_d = 1.0;
        (void)ajs_to_number(avm, argv[2], &ver_d);
        size_t slen = 0u;
        char *snap = ajs_to_cstring_len(avm, argv[3], &slen);
        if (!dbn || !snap) {
            if (dbn)  ajs_free_cstring(avm, dbn);
            if (snap) ajs_free_cstring(avm, snap);
            return ajs_bool(false);
        }

        web_ipc_lock();
        WebIdbEntry *found = NULL;
        for (WebIdbEntry *e = g_idb_head; e != NULL; e = e->next) {
            if (e->private_browsing == priv &&
                strcmp(e->origin, orig) == 0 &&
                strcmp(e->db_name, dbn) == 0) {
                found = e;
                break;
            }
        }
        if (!found) {
            found = (WebIdbEntry *)calloc(1u, sizeof(WebIdbEntry));
            if (found) {
                found->private_browsing = priv;
                arcr_str_copy(found->origin, sizeof(found->origin), orig);
                arcr_str_copy(found->db_name, sizeof(found->db_name), dbn);
                found->next = g_idb_head;
                g_idb_head  = found;
            }
        }
        if (found) {
            char *nc = (char *)malloc(slen + 1u);
            if (nc) {
                memcpy(nc, snap, slen + 1u);
                free(found->snapshot_json);
                found->snapshot_json = nc;
                found->version       = (uint64_t)ver_d;
            }
        }
        web_ipc_unlock();

        ajs_free_cstring(avm, dbn);
        ajs_free_cstring(avm, snap);
        return ajs_bool(found != NULL);
    }

    if (op == 2 && argc >= 2) { /* deleteDatabase(dbName) */
        char *dbn = ajs_to_cstring(avm, argv[1]);
        if (!dbn) return ajs_bool(false);
        web_ipc_lock();
        WebIdbEntry **pp = &g_idb_head;
        while (*pp) {
            WebIdbEntry *cur = *pp;
            if (cur->private_browsing == priv &&
                strcmp(cur->origin, orig) == 0 &&
                strcmp(cur->db_name, dbn) == 0) {
                *pp = cur->next;
                free(cur->snapshot_json);
                free(cur);
                break;
            }
            pp = &cur->next;
        }
        web_ipc_unlock();
        ajs_free_cstring(avm, dbn);
        return ajs_bool(true);
    }

    if (op == 3) { /* databases() -> [{name, version}] */
        AjsValue arr = ajs_new_array(avm, 0u);
        uint32_t idx = 0u;
        web_ipc_lock();
        for (const WebIdbEntry *e = g_idb_head; e != NULL; e = e->next) {
            if (e->private_browsing == priv && strcmp(e->origin, orig) == 0) {
                AjsValue info = ajs_new_object(avm);
                (void)ajs_set_prop_str(avm, info, "name", ajs_string_cstr(avm, e->db_name));
                (void)ajs_set_prop_str(avm, info, "version", ajs_number((double)e->version));
                (void)ajs_set_prop_index(avm, arr, idx++, info);
            }
        }
        web_ipc_unlock();
        return arr;
    }

    return ajs_null();
}

/* --- Dedicated Worker Bridge (`workerOp`) --- */
static AjsValue b_worker_op(AjsVm *avm, AjsValue this_val, int argc, const AjsValue *argv, void *ud) {
    (void)this_val;
    ArcrJsVm *vm = (ArcrJsVm *)ud;
    if (!vm || argc < 1) return ajs_undefined();
    int32_t op = 0;
    (void)ajs_to_int32(avm, argv[0], &op);

    if (op == 0 && argc >= 3) { /* spawn(scriptUrl, isModule) -> workerId (>0) or -1 SecurityError */
        char *raw_url = ajs_to_cstring(avm, argv[1]);
        bool is_mod = ajs_to_bool(avm, argv[2]);
        if (!raw_url) return ajs_int32(-1);

        char base_url[2048];
        web_effective_base_url(vm, base_url, sizeof(base_url));
        char resolved[2048];
        if (!web_resolve_url(base_url, raw_url, resolved, sizeof(resolved))) {
            ajs_free_cstring(avm, raw_url);
            return ajs_int32(-2); /* SyntaxError */
        }
        ajs_free_cstring(avm, raw_url);

        char page_orig[256], script_orig[256];
        web_compute_origin(base_url, page_orig, sizeof(page_orig));
        if (arcr_str_starts_with_nocase(resolved, "blob:")) {
            web_compute_origin(resolved + 5, script_orig, sizeof(script_orig));
        } else {
            web_compute_origin(resolved, script_orig, sizeof(script_orig));
        }
        if (!arcr_str_starts_with_nocase(resolved, "data:") &&
            strcmp(page_orig, "null") != 0 &&
            strcmp(page_orig, script_orig) != 0) {
            return ajs_int32(-1); /* Cross-origin Worker forbidden per HTML spec */
        }

        WebWorkerInstance *w = (WebWorkerInstance *)calloc(1u, sizeof(WebWorkerInstance));
        if (!w) return ajs_int32(0);
        if (vm->next_worker_id == 0u) vm->next_worker_id = 1u;
        w->worker_id = vm->next_worker_id++;
        w->busy      = 1;
        w->is_module = is_mod;
        w->heap_limit = vm->heap_limit > 0u ? vm->heap_limit : (16u * 1024u * 1024u);
        w->host_snap = vm->host;
        arcr_str_copy(w->origin, sizeof(w->origin), page_orig);
        arcr_str_copy(w->script_url, sizeof(w->script_url), resolved);
        web_mutex_init(&w->mtx);

        if (arcr_str_starts_with_nocase(resolved, "blob:")) {
            const uint8_t *bb = NULL;
            size_t bl = 0u;
            if (web_lookup_blob_url(vm, resolved, &bb, &bl, NULL, 0u) && bb) {
                w->inline_code = (char *)malloc(bl + 1u);
                if (w->inline_code) {
                    memcpy(w->inline_code, bb, bl);
                    w->inline_code[bl] = '\0';
                    w->inline_code_len = bl;
                }
            }
        }

        w->next = vm->workers_head;
        vm->workers_head = w;
        if (web_thread_spawn(&w->thread, worker_thread_main, w)) {
            w->thread_started = true;
        } else {
            w->busy = 0;
            w->finished = 1;
        }
        return ajs_int32((int32_t)w->worker_id);
    }

    if (op == 1 && argc >= 3) { /* postMessage(workerId, payloadJson, transferArrayOpt) */
        uint32_t wid = 0u;
        (void)ajs_to_uint32(avm, argv[1], &wid);
        WebWorkerInstance *target = NULL;
        for (WebWorkerInstance *w = vm->workers_head; w != NULL; w = w->next) {
            if (w->worker_id == wid && !w->closing) { target = w; break; }
        }
        if (!target) return ajs_undefined();

        size_t jlen = 0u;
        char *json = ajs_to_cstring_len(avm, argv[2], &jlen);
        if (!json) return ajs_undefined();

        WebWorkerMsg *m = (WebWorkerMsg *)calloc(1u, sizeof(WebWorkerMsg));
        if (!m) {
            ajs_free_cstring(avm, json);
            return ajs_undefined();
        }
        m->payload_json = (char *)malloc(jlen + 1u);
        if (!m->payload_json) {
            ajs_free_cstring(avm, json);
            free(m);
            return ajs_undefined();
        }
        memcpy(m->payload_json, json, jlen + 1u);
        m->payload_len = jlen;
        ajs_free_cstring(avm, json);

        if (argc >= 4 && ajs_is_array(avm, argv[3])) {
            AjsValue lval = ajs_get_prop_str(avm, argv[3], "length");
            uint32_t cnt = 0u;
            (void)ajs_to_uint32(avm, lval, &cnt);
            if (cnt > 0u && cnt <= 256u) {
                m->buffers     = (uint8_t **)calloc(cnt, sizeof(uint8_t *));
                m->buffer_lens = (size_t *)calloc(cnt, sizeof(size_t));
                if (m->buffers && m->buffer_lens) {
                    m->buffer_count = cnt;
                    for (uint32_t i = 0u; i < cnt; ++i) {
                        AjsValue ab = ajs_get_prop_index(avm, argv[3], i);
                        size_t blen = 0u;
                        uint8_t *ptr = ajs_get_array_buffer_data(avm, ab, &blen);
                        m->buffers[i] = (uint8_t *)malloc(blen + 1u);
                        if (m->buffers[i]) {
                            if (ptr && blen > 0u) memcpy(m->buffers[i], ptr, blen);
                            m->buffers[i][blen] = 0u;
                            m->buffer_lens[i] = blen;
                        }
                        (void)ajs_detach_array_buffer(avm, ab);
                    }
                }
            }
        }

        web_mutex_lock(&target->mtx);
        if (target->to_worker_tail) target->to_worker_tail->next = m;
        else                        target->to_worker_head       = m;
        target->to_worker_tail = m;
        target->busy = 1;
        web_mutex_unlock(&target->mtx);
        return ajs_undefined();
    }

    if (op == 2 && argc >= 2) { /* terminate(workerId) */
        uint32_t wid = 0u;
        (void)ajs_to_uint32(avm, argv[1], &wid);
        WebWorkerInstance **pp = &vm->workers_head;
        while (*pp) {
            if ((*pp)->worker_id == wid) {
                WebWorkerInstance *rm = *pp;
                *pp = rm->next;
                worker_free_instance(rm);
                break;
            }
            pp = &(*pp)->next;
        }
        return ajs_undefined();
    }

    if (op == 3) { /* poll() -> [{workerId, isError, payloadJson, buffers}] */
        AjsValue arr = ajs_new_array(avm, 0u);
        uint32_t idx = 0u;
        for (WebWorkerInstance *w = vm->workers_head; w != NULL; w = w->next) {
            WebWorkerMsg *batch = NULL;
            web_mutex_lock(&w->mtx);
            batch = w->to_main_head;
            w->to_main_head = NULL;
            w->to_main_tail = NULL;
            web_mutex_unlock(&w->mtx);

            while (batch) {
                WebWorkerMsg *nxt = batch->next;
                AjsValue o = ajs_new_object(avm);
                (void)ajs_set_prop_str(avm, o, "workerId", ajs_uint32(w->worker_id));
                (void)ajs_set_prop_str(avm, o, "isError", ajs_bool(batch->is_error));
                (void)ajs_set_prop_str(avm, o, "payloadJson",
                                       ajs_string_utf8(avm, batch->payload_json ? batch->payload_json : "",
                                                       batch->payload_len));
                AjsValue barr = ajs_new_array(avm, (uint32_t)batch->buffer_count);
                for (size_t bi = 0u; bi < batch->buffer_count; ++bi) {
                    AjsValue ab = ajs_new_array_buffer_copy(
                        avm,
                        batch->buffers[bi] ? batch->buffers[bi] : (const uint8_t *)"",
                        batch->buffer_lens[bi]);
                    (void)ajs_set_prop_index(avm, barr, (uint32_t)bi, ab);
                }
                (void)ajs_set_prop_str(avm, o, "buffers", barr);
                (void)ajs_set_prop_index(avm, arr, idx++, o);
                worker_msg_free(batch);
                batch = nxt;
            }
        }
        return arr;
    }

    return ajs_undefined();
}

/* =========================================================================
 * 8. Lifecycle & Pending Async Work Queries
 * ========================================================================= */
bool web_has_pending_async_work(const ArcrJsVm *vm) {
    if (!vm) return false;
    if (vm->fetch_jobs_head != NULL) return true;
    for (WebWorkerInstance *w = vm->workers_head; w != NULL; w = w->next) {
        if (w->busy) return true;
        bool has_msg = false;
        web_mutex_lock(&w->mtx);
        has_msg = (w->to_main_head != NULL || w->to_worker_head != NULL);
        web_mutex_unlock(&w->mtx);
        if (has_msg) return true;
    }
    bool has_bc = false;
    web_ipc_lock();
    has_bc = web_bc_has_unread_locked(vm->vm_instance_id);
    web_ipc_unlock();
    return has_bc;
}

void web_poll_async_work(ArcrJsVm *vm, bool *io_mutated) {
    if (!vm || !vm->avm) return;
    AjsHandleScope sc;
    ajs_handle_scope_open(vm->avm, &sc);
    AjsValue glob = ajs_global_object(vm->avm);
    AjsValue fn = ajs_get_prop_str(vm->avm, glob, "__pollHostAsync");
    if (ajs_is_callable(vm->avm, fn)) {
        AjsValue r = ajs_call(vm->avm, fn, glob, 0, NULL);
        if (ajs_has_exception(vm->avm)) {
            AjsErrorInfo ei;
            (void)ajs_take_exception(vm->avm, &ei);
            web_log_ajs_error(vm, "async/poll", &ei);
        } else if (io_mutated && ajs_to_bool(vm->avm, r)) {
            *io_mutated = true;
        }
        (void)web_drain_microtasks_guarded(vm);
    }
    ajs_handle_scope_close(vm->avm, &sc);
}

void web_crypto_streams_register_bridge(ArcrJsVm *vm, AjsValue b) {
    if (!vm || !vm->avm) return;
    AjsVm *avm = vm->avm;
    web_bind_fn(avm, b, "cryptoRandomBytes", b_crypto_random_bytes, 1, vm);
    web_bind_fn(avm, b, "cryptoRandomUUID",  b_crypto_random_uuid,  0, vm);
    web_bind_fn(avm, b, "subtleDigest",      b_subtle_digest,       2, vm);
    web_bind_fn(avm, b, "subtleHmac",        b_subtle_hmac,         3, vm);
    web_bind_fn(avm, b, "subtlePbkdf2",      b_subtle_pbkdf2,       5, vm);
    web_bind_fn(avm, b, "subtleHkdf",        b_subtle_hkdf,         5, vm);
    web_bind_fn(avm, b, "subtleAesGcm",      b_subtle_aes_gcm,      6, vm);
    web_bind_fn(avm, b, "utf8Encode",        b_utf8_encode,         1, vm);
    web_bind_fn(avm, b, "copyBufferBytes",   b_copy_buffer_bytes,   1, vm);
    web_bind_fn(avm, b, "detachArrayBuffer", b_detach_array_buffer, 1, vm);
    web_bind_fn(avm, b, "blobUrlOp",         b_blob_url_op,         3, vm);
    web_bind_fn(avm, b, "fetchStart",        b_fetch_start,         5, vm);
    web_bind_fn(avm, b, "fetchAbort",        b_fetch_abort,         1, vm);
    web_bind_fn(avm, b, "fetchPoll",         b_fetch_poll,          0, vm);
    web_bind_fn(avm, b, "bcOp",              b_bc_op,               3, vm);
    web_bind_fn(avm, b, "idbOp",             b_idb_op,              4, vm);
    web_bind_fn(avm, b, "workerOp",          b_worker_op,           4, vm);
}

void web_crypto_streams_cleanup(ArcrJsVm *vm) {
    if (!vm) return;

    /* 1. Signal cancellation to all in-flight fetch jobs & workers concurrently first */
    for (WebAsyncFetchJob *j = vm->fetch_jobs_head; j != NULL; j = j->next) {
        j->cancel_flag = 1;
    }
    for (WebWorkerInstance *w = vm->workers_head; w != NULL; w = w->next) {
        w->closing = 1;
    }

    /* 2. Join and free fetch jobs */
    WebAsyncFetchJob *fj = vm->fetch_jobs_head;
    vm->fetch_jobs_head = NULL;
    while (fj) {
        WebAsyncFetchJob *nxt = fj->next;
        fetch_job_free(fj);
        fj = nxt;
    }

    /* 3. Join and free worker instances */
    WebWorkerInstance *wk = vm->workers_head;
    vm->workers_head = NULL;
    while (wk) {
        WebWorkerInstance *nxt = wk->next;
        worker_free_instance(wk);
        wk = nxt;
    }

    /* 4. Unsubscribe BroadcastChannel & revoke Blob URLs */
    web_bc_cleanup_vm(vm->vm_instance_id);
    global_blob_remove_for_vm(vm->vm_instance_id, NULL);

    WebBlobUrlEntry *be = vm->blob_urls_head;
    vm->blob_urls_head = NULL;
    while (be) {
        WebBlobUrlEntry *nxt = be->next;
        free(be->bytes);
        free(be);
        be = nxt;
    }
}
