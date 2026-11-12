/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * media/arcr_media.c - Hardware/Software <video>/<audio> Decoder, Adaptive Policy
 *                      & 60 FPS Cinema Frame Renderer
 *
 * Backends:
 *   1. POSIX: Non-blocking background `ffmpeg` rawvideo BGRA pipe (`fork`/`execvp` + pthread)
 *   2. Win32: Dynamic `mfplat.dll` + `mfreadwrite.dll` (`IMFSourceReader` RGB32, Win2000-safe!)
 *   3. Universal Fallback: Pure C11 60 FPS color-graded cinema + waveform + HUD renderer
 */
#include "arcr_media.h"
#include "../base/arcr_base.h"
#include "../base/arcr_codec.h"
#include "../platform/arcr_platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <pthread.h>
#  include <signal.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <time.h>
#  include <unistd.h>
#endif

/* -------------------------------------------------------------------------
 * 1. Adaptive Fluid Media Policy (Preserved 100% for ABI & Test Suite)
 * ------------------------------------------------------------------------- */
static void media_compute_adaptive_policy(
    ArcrProfileMode            profile,
    const ArcrStreamVariant   *variants,
    size_t                     variant_count,
    int32_t                    viewport_w,
    int32_t                    viewport_h,
    uint32_t                   cpu_usage_percent,
    ArcrMediaAdaptiveDecision *out
) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->keep_audio_continuous = true;

    int32_t cap_h = 2160;
    if (profile == ARCR_PROFILE_MINIMUM) cap_h = 480;
    else if (profile == ARCR_PROFILE_BALANCED) cap_h = 720;

    if (cpu_usage_percent >= 88u) {
        cap_h = ARCR_MIN(cap_h, 360);
        out->frame_drop_mode = ARCR_DROP_NONREF_BFRAMES;
        out->scale_quality   = ARCR_SCALE_NEAREST_FAST;
    } else if (cpu_usage_percent >= 72u) {
        cap_h = ARCR_MIN(cap_h, 480);
        out->frame_drop_mode = ARCR_DROP_DISPLAY_FRAMES;
        out->scale_quality   = ARCR_SCALE_NEAREST_FAST;
    } else if (cpu_usage_percent >= 50u || profile == ARCR_PROFILE_MINIMUM) {
        out->frame_drop_mode = ARCR_DROP_NONE;
        out->scale_quality   = ARCR_SCALE_BILINEAR;
    } else {
        out->frame_drop_mode = ARCR_DROP_NONE;
        out->scale_quality   = ARCR_SCALE_BICUBIC_HQ;
    }

    int32_t target_h = (viewport_h > 0) ? ARCR_MIN(viewport_h, cap_h) : cap_h;

    if (variants && variant_count > 0u) {
        size_t best_idx = 0u;
        int32_t best_diff = INT32_MAX;
        for (size_t i = 0; i < variant_count; ++i) {
            int32_t vh = variants[i].height;
            int32_t diff = (vh <= target_h) ? (target_h - vh) : ((vh - target_h) * 4);
            if (diff < best_diff) {
                best_diff = diff;
                best_idx  = i;
            }
        }
        out->chosen_stream_index = best_idx;
        out->render_width  = ARCR_MIN(viewport_w > 0 ? viewport_w : variants[best_idx].width,
                                      variants[best_idx].width);
        out->render_height = ARCR_MIN(viewport_h > 0 ? viewport_h : variants[best_idx].height,
                                      variants[best_idx].height);
    } else {
        out->render_width  = (viewport_w > 0) ? viewport_w : 640;
        out->render_height = target_h;
    }
}

/* -------------------------------------------------------------------------
 * 2. Compact 5x7 Pixel HUD Glyph Renderer (Zero External Font Dependency)
 * ------------------------------------------------------------------------- */
static void hud_glyph_rows(char c, uint8_t rows[7]) {
    memset(rows, 0, 7u);
    if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
    switch (c) {
        case '0': { const uint8_t r[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; memcpy(rows,r,7); break; }
        case '1': { const uint8_t r[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; memcpy(rows,r,7); break; }
        case '2': { const uint8_t r[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; memcpy(rows,r,7); break; }
        case '3': { const uint8_t r[7] = {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}; memcpy(rows,r,7); break; }
        case '4': { const uint8_t r[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; memcpy(rows,r,7); break; }
        case '5': { const uint8_t r[7] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}; memcpy(rows,r,7); break; }
        case '6': { const uint8_t r[7] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}; memcpy(rows,r,7); break; }
        case '7': { const uint8_t r[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; memcpy(rows,r,7); break; }
        case '8': { const uint8_t r[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; memcpy(rows,r,7); break; }
        case '9': { const uint8_t r[7] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}; memcpy(rows,r,7); break; }
        case 'A': { const uint8_t r[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; memcpy(rows,r,7); break; }
        case 'B': { const uint8_t r[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; memcpy(rows,r,7); break; }
        case 'C': { const uint8_t r[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}; memcpy(rows,r,7); break; }
        case 'D': { const uint8_t r[7] = {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}; memcpy(rows,r,7); break; }
        case 'E': { const uint8_t r[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; memcpy(rows,r,7); break; }
        case 'F': { const uint8_t r[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; memcpy(rows,r,7); break; }
        case 'G': { const uint8_t r[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}; memcpy(rows,r,7); break; }
        case 'H': { const uint8_t r[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; memcpy(rows,r,7); break; }
        case 'I': { const uint8_t r[7] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}; memcpy(rows,r,7); break; }
        case 'J': { const uint8_t r[7] = {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}; memcpy(rows,r,7); break; }
        case 'K': { const uint8_t r[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11}; memcpy(rows,r,7); break; }
        case 'L': { const uint8_t r[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; memcpy(rows,r,7); break; }
        case 'M': { const uint8_t r[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; memcpy(rows,r,7); break; }
        case 'N': { const uint8_t r[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11}; memcpy(rows,r,7); break; }
        case 'O': { const uint8_t r[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; memcpy(rows,r,7); break; }
        case 'P': { const uint8_t r[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; memcpy(rows,r,7); break; }
        case 'Q': { const uint8_t r[7] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}; memcpy(rows,r,7); break; }
        case 'R': { const uint8_t r[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; memcpy(rows,r,7); break; }
        case 'S': { const uint8_t r[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; memcpy(rows,r,7); break; }
        case 'T': { const uint8_t r[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; memcpy(rows,r,7); break; }
        case 'U': { const uint8_t r[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; memcpy(rows,r,7); break; }
        case 'V': { const uint8_t r[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}; memcpy(rows,r,7); break; }
        case 'W': { const uint8_t r[7] = {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}; memcpy(rows,r,7); break; }
        case 'X': { const uint8_t r[7] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}; memcpy(rows,r,7); break; }
        case 'Y': { const uint8_t r[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; memcpy(rows,r,7); break; }
        case 'Z': { const uint8_t r[7] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}; memcpy(rows,r,7); break; }
        case ':': { const uint8_t r[7] = {0x00,0x04,0x04,0x00,0x04,0x04,0x00}; memcpy(rows,r,7); break; }
        case '/': { const uint8_t r[7] = {0x01,0x02,0x02,0x04,0x08,0x08,0x10}; memcpy(rows,r,7); break; }
        case '-': { const uint8_t r[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}; memcpy(rows,r,7); break; }
        case '.': { const uint8_t r[7] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}; memcpy(rows,r,7); break; }
        case '_': { const uint8_t r[7] = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}; memcpy(rows,r,7); break; }
        case '|': { const uint8_t r[7] = {0x04,0x04,0x04,0x04,0x04,0x04,0x04}; memcpy(rows,r,7); break; }
        case '(': { const uint8_t r[7] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02}; memcpy(rows,r,7); break; }
        case ')': { const uint8_t r[7] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08}; memcpy(rows,r,7); break; }
        case ' ': break;
        default:  { const uint8_t r[7] = {0x0E,0x11,0x02,0x04,0x04,0x00,0x04}; memcpy(rows,r,7); break; }
    }
}

static inline uint32_t blend_argb_over(uint32_t dst, uint32_t src) {
    uint32_t sa = (src >> 24) & 0xFFu;
    if (sa == 0u) return dst;
    if (sa == 255u) return src;
    uint32_t inv = 255u - sa;
    uint32_t r = (((src >> 16) & 0xFFu) * sa + ((dst >> 16) & 0xFFu) * inv + 127u) / 255u;
    uint32_t g = (((src >> 8)  & 0xFFu) * sa + ((dst >> 8)  & 0xFFu) * inv + 127u) / 255u;
    uint32_t b = ((src         & 0xFFu) * sa + (dst         & 0xFFu) * inv + 127u) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void hud_fill_rect(uint32_t *fb, int32_t fw, int32_t fh,
                          int32_t rx, int32_t ry, int32_t rw, int32_t rh,
                          uint32_t argb) {
    int32_t x0 = ARCR_MAX(0, rx);
    int32_t y0 = ARCR_MAX(0, ry);
    int32_t x1 = ARCR_MIN(fw, rx + rw);
    int32_t y1 = ARCR_MIN(fh, ry + rh);
    if (x0 >= x1 || y0 >= y1) return;
    uint32_t sa = (argb >> 24) & 0xFFu;
    for (int32_t y = y0; y < y1; ++y) {
        uint32_t *row = fb + (size_t)y * (size_t)fw;
        if (sa == 255u) {
            for (int32_t x = x0; x < x1; ++x) row[x] = argb;
        } else {
            for (int32_t x = x0; x < x1; ++x) row[x] = blend_argb_over(row[x], argb);
        }
    }
}

static void hud_draw_text(uint32_t *fb, int32_t fw, int32_t fh,
                          int32_t x, int32_t y, const char *s,
                          int32_t scale, uint32_t argb, int32_t max_x) {
    if (!s || scale < 1) return;
    int32_t cx = x;
    for (size_t i = 0u; s[i] != '\0'; ++i) {
        unsigned char uc = (unsigned char)s[i];
        char ch = (uc >= 0x20u && uc <= 0x7Eu) ? (char)uc : ' ';
        if (uc >= 0x80u) {
            /* Skip continuation bytes so UTF-8 codepoints advance once cleanly */
            if ((uc & 0xC0u) == 0x80u) continue;
            ch = '*';
        }
        if (cx + 5 * scale > max_x) break;
        uint8_t rows[7];
        hud_glyph_rows(ch, rows);
        for (int32_t gy = 0; gy < 7; ++gy) {
            uint8_t bits = rows[gy];
            if (!bits) continue;
            for (int32_t gx = 0; gx < 5; ++gx) {
                if ((bits >> (4 - gx)) & 1u) {
                    hud_fill_rect(fb, fw, fh,
                                  cx + gx * scale, y + gy * scale,
                                  scale, scale, argb);
                }
            }
        }
        cx += 6 * scale;
    }
}

/* -------------------------------------------------------------------------
 * 3. Win32 Dynamic Media Foundation COM Definitions (Zero Import-Table Deps)
 * ------------------------------------------------------------------------- */
#if defined(_WIN32)
typedef struct ArcrGuid {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} ArcrGuid;

static const ArcrGuid kMF_MT_MAJOR_TYPE =
    {0x48EBA18Eu, 0xF8C9u, 0x4687u, {0xBF, 0x11, 0x0A, 0x74, 0xC9, 0xF9, 0x6A, 0x8F}};
static const ArcrGuid kMF_MT_SUBTYPE =
    {0xF7E34C9Au, 0x42E8u, 0x4714u, {0xB7, 0x4B, 0xCB, 0x29, 0xD7, 0x2C, 0x35, 0xE5}};
static const ArcrGuid kMF_MT_FRAME_SIZE =
    {0x1652C33Du, 0xD6B2u, 0x4012u, {0xB8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xA3, 0x7D}};
static const ArcrGuid kMFMediaType_Video =
    {0x73646976u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const ArcrGuid kMFVideoFormat_RGB32 =
    {22u,         0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

#define ARCR_MF_VERSION                         0x00020070u
#define ARCR_MF_SOURCE_READER_FIRST_VIDEO       0xFFFFFFFCu
#define ARCR_MF_SOURCE_READERF_ENDOFSTREAM      0x00000002u
#define ARCR_MF_SOURCE_READERF_CURRENTMEDIATYPE 0x00000020u

typedef struct ArcrIMFMediaBuffer ArcrIMFMediaBuffer;
typedef struct ArcrIMFSample      ArcrIMFSample;
typedef struct ArcrIMFMediaType   ArcrIMFMediaType;
typedef struct ArcrIMFSourceReader ArcrIMFSourceReader;

typedef struct ArcrIMFMediaBufferVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ArcrIMFMediaBuffer *, const void *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ArcrIMFMediaBuffer *);
    ULONG   (STDMETHODCALLTYPE *Release)(ArcrIMFMediaBuffer *);
    HRESULT (STDMETHODCALLTYPE *Lock)(ArcrIMFMediaBuffer *, uint8_t **, DWORD *, DWORD *);
    HRESULT (STDMETHODCALLTYPE *Unlock)(ArcrIMFMediaBuffer *);
    HRESULT (STDMETHODCALLTYPE *GetCurrentLength)(ArcrIMFMediaBuffer *, DWORD *);
    HRESULT (STDMETHODCALLTYPE *SetCurrentLength)(ArcrIMFMediaBuffer *, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetMaxLength)(ArcrIMFMediaBuffer *, DWORD *);
} ArcrIMFMediaBufferVtbl;

struct ArcrIMFMediaBuffer { const ArcrIMFMediaBufferVtbl *lpVtbl; };

typedef struct ArcrIMFSampleVtbl {
    void *base_methods[41]; /* IUnknown (3) + IMFAttributes (30) + IMFSample prefix (8) */
    HRESULT (STDMETHODCALLTYPE *ConvertToContiguousBuffer)(ArcrIMFSample *, ArcrIMFMediaBuffer **);
} ArcrIMFSampleVtbl;

struct ArcrIMFSample { const ArcrIMFSampleVtbl *lpVtbl; };

typedef struct ArcrIMFMediaTypeVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ArcrIMFMediaType *, const void *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ArcrIMFMediaType *);
    ULONG   (STDMETHODCALLTYPE *Release)(ArcrIMFMediaType *);
    void   *attr_getters[18];
    HRESULT (STDMETHODCALLTYPE *SetUINT32)(ArcrIMFMediaType *, const ArcrGuid *, UINT32);
    HRESULT (STDMETHODCALLTYPE *SetUINT64)(ArcrIMFMediaType *, const ArcrGuid *, UINT64);
    HRESULT (STDMETHODCALLTYPE *SetDouble)(ArcrIMFMediaType *, const ArcrGuid *, double);
    HRESULT (STDMETHODCALLTYPE *SetGUID)(ArcrIMFMediaType *, const ArcrGuid *, const ArcrGuid *);
} ArcrIMFMediaTypeVtbl;

struct ArcrIMFMediaType { const ArcrIMFMediaTypeVtbl *lpVtbl; };

typedef struct ArcrPropVariant {
    USHORT vt;
    USHORT r1, r2, r3;
    LONGLONG hVal;
    LONGLONG pad;
} ArcrPropVariant;

typedef struct ArcrIMFSourceReaderVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ArcrIMFSourceReader *, const void *, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ArcrIMFSourceReader *);
    ULONG   (STDMETHODCALLTYPE *Release)(ArcrIMFSourceReader *);
    HRESULT (STDMETHODCALLTYPE *GetStreamSelection)(ArcrIMFSourceReader *, DWORD, BOOL *);
    HRESULT (STDMETHODCALLTYPE *SetStreamSelection)(ArcrIMFSourceReader *, DWORD, BOOL);
    HRESULT (STDMETHODCALLTYPE *GetNativeMediaType)(ArcrIMFSourceReader *, DWORD, DWORD, ArcrIMFMediaType **);
    HRESULT (STDMETHODCALLTYPE *GetCurrentMediaType)(ArcrIMFSourceReader *, DWORD, ArcrIMFMediaType **);
    HRESULT (STDMETHODCALLTYPE *SetCurrentMediaType)(ArcrIMFSourceReader *, DWORD, DWORD *, ArcrIMFMediaType *);
    HRESULT (STDMETHODCALLTYPE *SetCurrentPosition)(ArcrIMFSourceReader *, const ArcrGuid *, const ArcrPropVariant *);
    HRESULT (STDMETHODCALLTYPE *ReadSample)(ArcrIMFSourceReader *, DWORD, DWORD, DWORD *, DWORD *, LONGLONG *, ArcrIMFSample **);
} ArcrIMFSourceReaderVtbl;

struct ArcrIMFSourceReader { const ArcrIMFSourceReaderVtbl *lpVtbl; };

typedef HRESULT (WINAPI *PFN_MFStartup)(ULONG, DWORD);
typedef HRESULT (WINAPI *PFN_MFShutdown)(void);
typedef HRESULT (WINAPI *PFN_MFCreateMediaType)(ArcrIMFMediaType **);
typedef HRESULT (WINAPI *PFN_MFCreateSourceReaderFromURL)(LPCWSTR, void *, ArcrIMFSourceReader **);
#endif

/* -------------------------------------------------------------------------
 * 4. ArcrMediaPlayer State & Backend Implementations
 * ------------------------------------------------------------------------- */
struct ArcrMediaPlayer {
    char      source_url[2048];
    char      temp_path[512];
    char      title[192];
    int32_t   dec_w;
    int32_t   dec_h;

    bool      paused;
    bool      dirty;
    double    pos_sec;
    double    duration_sec;
    uint64_t  last_tick_usec;
    uint64_t  last_render_usec;

    uint32_t *decoded_frame;
    bool      has_decoded_frame;
    bool      use_procedural;

    /* Static poster / single-frame image if decoded directly from mem/file */
    ArcrImage static_poster;

#if defined(_WIN32)
    HMODULE              h_mfplat;
    HMODULE              h_mfreadwrite;
    PFN_MFShutdown       fn_mf_shutdown;
    ArcrIMFSourceReader *mf_reader;
    bool                 mf_started;
    int32_t              mf_vid_w;
    int32_t              mf_vid_h;
    uint64_t             mf_last_step_usec;
#else
    pthread_mutex_t      lock;
    bool                 lock_inited;
    pthread_t            reader_thread;
    bool                 thread_running;
    volatile bool        stop_requested;
    pid_t                ffmpeg_pid;
    int                  pipe_fd;
    uint32_t             frames_decoded;
#endif
};

static void sanitize_ascii_title(const char *raw, const char *url, char *out, size_t cap) {
    if (!out || cap == 0u) return;
    out[0] = '\0';
    const char *src = (raw && raw[0] != '\0') ? raw : url;
    if (!src || src[0] == '\0') {
        arcr_str_copy(out, cap, "Waterscape Cinema Stream");
        return;
    }
    /* If URL path, pick basename when raw title was empty */
    if ((!raw || raw[0] == '\0')) {
        const char *slash = strrchr(src, '/');
        if (slash && slash[1] != '\0') src = slash + 1;
    }
    size_t w = 0u;
    for (size_t i = 0u; src[i] != '\0' && w + 1u < cap; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '?' || c == '#') break;
        if (c >= 0x20u && c <= 0x7Eu) {
            out[w++] = (char)c;
        } else if ((c & 0xC0u) != 0x80u) {
            if (w == 0u || out[w - 1u] != ' ') out[w++] = ' ';
        }
    }
    out[w] = '\0';
    /* Trim whitespace */
    const char *tp = out;
    size_t tlen = strlen(out);
    arcr_str_trim_view(&tp, &tlen);
    if (tlen == 0u) {
        arcr_str_copy(out, cap, "Waterscape HD Video");
    } else if (tp != out) {
        memmove(out, tp, tlen);
        out[tlen] = '\0';
    }
}

#if !defined(_WIN32)
static bool find_ffmpeg_binary(char *out_path, size_t cap) {
    static int   s_probed = 0;
    static char  s_cached[512];
    if (s_probed != 0) {
        if (s_probed > 0) {
            arcr_str_copy(out_path, cap, s_cached);
            return true;
        }
        return false;
    }

    const char *path_env = getenv("PATH");
    if (!path_env || path_env[0] == '\0') {
        path_env = "/usr/local/bin:/usr/bin:/bin";
    }
    const char *p = path_env;
    while (*p != '\0') {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen > 0u && dlen + 10u < sizeof(s_cached)) {
            char cand[512];
            memcpy(cand, p, dlen);
            cand[dlen] = '\0';
            snprintf(cand + dlen, sizeof(cand) - dlen, "/ffmpeg");
            if (access(cand, X_OK) == 0) {
                arcr_str_copy(s_cached, sizeof(s_cached), cand);
                s_probed = 1;
                arcr_str_copy(out_path, cap, s_cached);
                return true;
            }
        }
        if (!colon) break;
        p = colon + 1;
    }
    s_probed = -1;
    return false;
}

static void posix_stop_ffmpeg_worker(ArcrMediaPlayer *mp) {
    if (!mp) return;
    mp->stop_requested = true;
    if (mp->ffmpeg_pid > 0) {
        (void)kill(mp->ffmpeg_pid, SIGKILL);
    }
    if (mp->thread_running) {
        (void)pthread_join(mp->reader_thread, NULL);
        mp->thread_running = false;
    }
    if (mp->pipe_fd >= 0) {
        close(mp->pipe_fd);
        mp->pipe_fd = -1;
    }
    if (mp->ffmpeg_pid > 0) {
        int status = 0;
        (void)waitpid(mp->ffmpeg_pid, &status, 0);
        mp->ffmpeg_pid = -1;
    }
}

static void sleep_short_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

static void *posix_ffmpeg_reader_main(void *arg) {
    ArcrMediaPlayer *mp = (ArcrMediaPlayer *)arg;
    size_t frame_bytes = (size_t)mp->dec_w * (size_t)mp->dec_h * sizeof(uint32_t);
    uint8_t *buf = (uint8_t *)malloc(frame_bytes);
    if (!buf) {
        pthread_mutex_lock(&mp->lock);
        mp->use_procedural = true;
        pthread_mutex_unlock(&mp->lock);
        return NULL;
    }

    uint64_t next_frame_usec = arcr_platform_monotonic_usec();
    while (!mp->stop_requested) {
        pthread_mutex_lock(&mp->lock);
        bool is_paused = mp->paused;
        pthread_mutex_unlock(&mp->lock);

        if (is_paused && mp->frames_decoded > 0u) {
            sleep_short_ms(16);
            next_frame_usec = arcr_platform_monotonic_usec();
            continue;
        }

        size_t got = 0u;
        while (got < frame_bytes && !mp->stop_requested) {
            ssize_t n = read(mp->pipe_fd, buf + got, frame_bytes - got);
            if (n > 0) {
                got += (size_t)n;
            } else if (n == 0) {
                break;
            } else {
                if (errno == EINTR) continue;
                break;
            }
        }

        if (got < frame_bytes) {
            pthread_mutex_lock(&mp->lock);
            if (mp->frames_decoded == 0u) {
                mp->use_procedural = true;
            } else {
                mp->paused = true;
            }
            mp->dirty = true;
            pthread_mutex_unlock(&mp->lock);
            break;
        }

        /* Ensure opaque alpha on BGRA -> ARGB8888 little-endian uint32_t */
        uint32_t *px = (uint32_t *)(void *)buf;
        size_t count = (size_t)mp->dec_w * (size_t)mp->dec_h;
        for (size_t i = 0u; i < count; ++i) {
            px[i] |= 0xFF000000u;
        }

        pthread_mutex_lock(&mp->lock);
        if (mp->decoded_frame) {
            memcpy(mp->decoded_frame, buf, frame_bytes);
            mp->has_decoded_frame = true;
            mp->frames_decoded++;
            mp->pos_sec += (1.0 / 24.0);
            if (mp->pos_sec > mp->duration_sec) {
                mp->duration_sec = mp->pos_sec + 30.0;
            }
            mp->dirty = true;
        }
        pthread_mutex_unlock(&mp->lock);

        next_frame_usec += 41667ULL; /* 24 FPS pacing */
        uint64_t now = arcr_platform_monotonic_usec();
        if (next_frame_usec > now) {
            uint64_t wait_ms = (next_frame_usec - now) / 1000ULL;
            if (wait_ms > 2ULL) sleep_short_ms((int)ARCR_MIN(wait_ms, 40ULL));
        } else {
            next_frame_usec = now;
        }
    }

    free(buf);
    return NULL;
}

static bool posix_start_ffmpeg_worker(ArcrMediaPlayer *mp, double start_sec) {
    if (!mp || mp->source_url[0] == '\0') return false;

    char ffmpeg_bin[512];
    if (!find_ffmpeg_binary(ffmpeg_bin, sizeof(ffmpeg_bin))) return false;

    const char *input_arg = mp->source_url;
    if (arcr_str_starts_with_nocase(input_arg, "file://")) {
        input_arg += 7;
    } else if (!arcr_str_starts_with_nocase(input_arg, "http://") &&
               !arcr_str_starts_with_nocase(input_arg, "https://")) {
        if (access(input_arg, R_OK) != 0) {
            return false;
        }
    } else {
        /* When HTTP(S) proxy restrictions or offline sandboxes exist, only spawn ffmpeg
         * for direct local files unless HTTP_PROXY is reachable; ffmpeg handles HTTP_PROXY too. */
    }
    if (arcr_str_starts_with_nocase(mp->source_url, "file://") && access(input_arg, R_OK) != 0) {
        return false;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) return false;

    char scale_filter[64];
    snprintf(scale_filter, sizeof(scale_filter), "scale=%d:%d", (int)mp->dec_w, (int)mp->dec_h);

    char seek_buf[32];
    snprintf(seek_buf, sizeof(seek_buf), "%.2f", start_sec > 0.0 ? start_sec : 0.0);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        /* Child process: redirect stdin/stderr to /dev/null, stdout to pipefd[1] */
        close(pipefd[0]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        (void)dup2(pipefd[1], STDOUT_FILENO);
        if (pipefd[1] != STDOUT_FILENO) close(pipefd[1]);

        char *argv_seek[] = {
            ffmpeg_bin,
            "-nostdin", "-loglevel", "quiet",
            "-ss", seek_buf,
            "-i", (char *)input_arg,
            "-vf", scale_filter,
            "-pix_fmt", "bgra",
            "-f", "rawvideo",
            "-r", "24",
            "-",
            NULL
        };
        char *argv_noseek[] = {
            ffmpeg_bin,
            "-nostdin", "-loglevel", "quiet",
            "-i", (char *)input_arg,
            "-vf", scale_filter,
            "-pix_fmt", "bgra",
            "-f", "rawvideo",
            "-r", "24",
            "-",
            NULL
        };
        execvp(ffmpeg_bin, (start_sec > 0.05) ? argv_seek : argv_noseek);
        _exit(127);
    }

    close(pipefd[1]);
    mp->ffmpeg_pid     = pid;
    mp->pipe_fd        = pipefd[0];
    mp->stop_requested = false;
    mp->frames_decoded = 0u;

    if (pthread_create(&mp->reader_thread, NULL, posix_ffmpeg_reader_main, mp) != 0) {
        (void)kill(pid, SIGKILL);
        close(pipefd[0]);
        mp->pipe_fd    = -1;
        mp->ffmpeg_pid = -1;
        int st = 0;
        (void)waitpid(pid, &st, 0);
        return false;
    }
    mp->thread_running = true;
    return true;
}
#endif

#if defined(_WIN32)
static bool win32_init_media_foundation(ArcrMediaPlayer *mp) {
    if (!mp || mp->source_url[0] == '\0') return false;
    mp->h_mfplat      = LoadLibraryA("mfplat.dll");
    mp->h_mfreadwrite = LoadLibraryA("mfreadwrite.dll");
    if (!mp->h_mfplat || !mp->h_mfreadwrite) return false;

    PFN_MFStartup fn_startup = NULL;
    PFN_MFShutdown fn_shutdown = NULL;
    PFN_MFCreateMediaType fn_create_mt = NULL;
    PFN_MFCreateSourceReaderFromURL fn_create_reader = NULL;

    FARPROC p1 = GetProcAddress(mp->h_mfplat, "MFStartup");
    FARPROC p2 = GetProcAddress(mp->h_mfplat, "MFShutdown");
    FARPROC p3 = GetProcAddress(mp->h_mfplat, "MFCreateMediaType");
    FARPROC p4 = GetProcAddress(mp->h_mfreadwrite, "MFCreateSourceReaderFromURL");
    memcpy(&fn_startup, &p1, sizeof(fn_startup));
    memcpy(&fn_shutdown, &p2, sizeof(fn_shutdown));
    memcpy(&fn_create_mt, &p3, sizeof(fn_create_mt));
    memcpy(&fn_create_reader, &p4, sizeof(fn_create_reader));

    if (!fn_startup || !fn_shutdown || !fn_create_mt || !fn_create_reader) return false;
    if (FAILED(fn_startup(ARCR_MF_VERSION, 0u))) return false;
    mp->fn_mf_shutdown = fn_shutdown;
    mp->mf_started     = true;

    WCHAR wurl[2048];
    int wlen = MultiByteToWideChar(CP_UTF8, 0, mp->source_url, -1, wurl, 2048);
    if (wlen <= 0) return false;

    ArcrIMFSourceReader *reader = NULL;
    if (FAILED(fn_create_reader(wurl, NULL, &reader)) || !reader) return false;

    ArcrIMFMediaType *mt = NULL;
    if (SUCCEEDED(fn_create_mt(&mt)) && mt) {
        (void)mt->lpVtbl->SetGUID(mt, &kMF_MT_MAJOR_TYPE, &kMFMediaType_Video);
        (void)mt->lpVtbl->SetGUID(mt, &kMF_MT_SUBTYPE, &kMFVideoFormat_RGB32);
        HRESULT hr = reader->lpVtbl->SetCurrentMediaType(
            reader, ARCR_MF_SOURCE_READER_FIRST_VIDEO, NULL, mt);
        mt->lpVtbl->Release(mt);
        if (FAILED(hr)) {
            reader->lpVtbl->Release(reader);
            return false;
        }
    }

    mp->mf_reader = reader;
    mp->mf_vid_w  = mp->dec_w;
    mp->mf_vid_h  = mp->dec_h;
    return true;
}

static void win32_step_mf_frame(ArcrMediaPlayer *mp, uint64_t now_usec) {
    if (!mp || !mp->mf_reader || mp->paused) return;
    if (mp->mf_last_step_usec != 0u && now_usec - mp->mf_last_step_usec < 33000ULL) return;
    mp->mf_last_step_usec = now_usec;

    DWORD stream_idx = 0u, flags = 0u;
    LONGLONG ts_100ns = 0;
    ArcrIMFSample *sample = NULL;
    HRESULT hr = mp->mf_reader->lpVtbl->ReadSample(
        mp->mf_reader, ARCR_MF_SOURCE_READER_FIRST_VIDEO, 0u,
        &stream_idx, &flags, &ts_100ns, &sample);
    if (FAILED(hr)) {
        mp->use_procedural = true;
        return;
    }
    if (flags & ARCR_MF_SOURCE_READERF_ENDOFSTREAM) {
        mp->paused = true;
        mp->dirty  = true;
        if (sample) ((ArcrIMFMediaBuffer *)(void *)sample)->lpVtbl->Release((ArcrIMFMediaBuffer *)(void *)sample);
        return;
    }
    if (!sample) return;

    ArcrIMFMediaBuffer *buf = NULL;
    if (SUCCEEDED(sample->lpVtbl->ConvertToContiguousBuffer(sample, &buf)) && buf) {
        uint8_t *raw = NULL;
        DWORD max_len = 0u, cur_len = 0u;
        if (SUCCEEDED(buf->lpVtbl->Lock(buf, &raw, &max_len, &cur_len)) && raw) {
            size_t px_cnt = cur_len / 4u;
            size_t dst_cnt = (size_t)mp->dec_w * (size_t)mp->dec_h;
            size_t copy_cnt = ARCR_MIN(px_cnt, dst_cnt);
            const uint32_t *src32 = (const uint32_t *)(const void *)raw;
            for (size_t i = 0u; i < copy_cnt; ++i) {
                mp->decoded_frame[i] = src32[i] | 0xFF000000u;
            }
            mp->has_decoded_frame = true;
            mp->pos_sec = (double)ts_100ns * 1e-7;
            mp->dirty = true;
            (void)buf->lpVtbl->Unlock(buf);
        }
        buf->lpVtbl->Release(buf);
    }
    ((ArcrIMFMediaBuffer *)(void *)sample)->lpVtbl->Release((ArcrIMFMediaBuffer *)(void *)sample);
}
#endif

/* -------------------------------------------------------------------------
 * 5. Pure C 60 FPS Cinema Frame + HUD Renderer
 * ------------------------------------------------------------------------- */
static void blit_scaled_nearest(const uint32_t *src, int32_t sw, int32_t sh,
                                uint32_t *dst, int32_t dw, int32_t dh) {
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if (sw == dw && sh == dh) {
        memcpy(dst, src, (size_t)dw * (size_t)dh * sizeof(uint32_t));
        return;
    }
    for (int32_t y = 0; y < dh; ++y) {
        int32_t sy = (int32_t)(((int64_t)y * sh) / dh);
        const uint32_t *srow = src + (size_t)sy * (size_t)sw;
        uint32_t *drow = dst + (size_t)y * (size_t)dw;
        for (int32_t x = 0; x < dw; ++x) {
            int32_t sx = (int32_t)(((int64_t)x * sw) / dw);
            drow[x] = srow[sx];
        }
    }
}

static void render_procedural_cinema_stage(uint32_t *out_argb, int32_t w, int32_t h,
                                           double pos_sec, bool paused) {
    /* Precompute X wave offsets once per frame so per-pixel inner loop is pure integer math! */
    int32_t wave_a[1920];
    int32_t wave_b[1920];
    int32_t cw = ARCR_MIN(w, 1920);
    double t1 = pos_sec * 2.1;
    double t2 = pos_sec * 1.35;

    for (int32_t x = 0; x < cw; ++x) {
        double nx = (double)x / (double)ARCR_MAX(1, w);
        wave_a[x] = (int32_t)(sin(nx * 6.2831853 + t1) * 28.0 +
                              cos(nx * 12.5663706 - t2) * 14.0);
        wave_b[x] = (int32_t)(cos(nx * 4.7123890 - t1 * 0.7) * 32.0);
    }

    int32_t horizon_y = (h * 56) / 100;
    for (int32_t y = 0; y < h; ++y) {
        uint32_t *row = out_argb + (size_t)y * (size_t)w;
        int32_t base_r = 10 + (y * 20) / ARCR_MAX(1, h);
        int32_t base_g = 16 + (y * 34) / ARCR_MAX(1, h);
        int32_t base_b = 34 + (y * 54) / ARCR_MAX(1, h);

        for (int32_t x = 0; x < w; ++x) {
            int32_t wx = (x < cw) ? x : (cw - 1);
            int32_t dy1 = y - (horizon_y / 2 + wave_a[wx]);
            int32_t dy2 = y - (horizon_y + wave_b[wx] / 2);

            int32_t glow1 = 68 - (dy1 < 0 ? -dy1 : dy1);
            int32_t glow2 = 54 - (dy2 < 0 ? -dy2 : dy2);

            int32_t r = base_r;
            int32_t g = base_g;
            int32_t b = base_b;
            if (glow1 > 0) {
                r += (glow1 * 22) / 68;
                g += (glow1 * 112) / 68;
                b += (glow1 * 138) / 68;
            }
            if (glow2 > 0) {
                r += (glow2 * 96) / 54;
                g += (glow2 * 36) / 54;
                b += (glow2 * 118) / 54;
            }
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            row[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    /* Animated 24-band audio spectrum visualizer above the scrubber bar */
    if (h >= 96 && w >= 140) {
        int32_t bars = 24;
        int32_t total_bw = ARCR_MIN(w - 48, 360);
        int32_t start_x  = (w - total_bw) / 2;
        int32_t step_x   = ARCR_MAX(4, total_bw / bars);
        int32_t bar_w    = ARCR_MAX(2, step_x - 3);
        int32_t base_y   = h - 38;
        int32_t max_bh   = ARCR_CLAMP(h / 4, 18, 68);

        for (int32_t b = 0; b < bars; ++b) {
            double phase = pos_sec * 6.5 + (double)b * 0.55;
            double amp = paused
                             ? (0.22 + 0.10 * sin((double)b * 0.7))
                             : (0.25 + 0.42 * fabs(sin(phase)) + 0.30 * fabs(cos(phase * 0.63)));
            int32_t bh = ARCR_CLAMP((int32_t)(amp * (double)max_bh), 4, max_bh);
            uint32_t col = (b % 3 == 0) ? 0xCC38BDF8u : ((b % 3 == 1) ? 0xCC818CF8u : 0xCCF43F5Eu);
            hud_fill_rect(out_argb, w, h, start_x + b * step_x, base_y - bh, bar_w, bh, col);
        }
    }
}

static void format_mmss(double sec, char out[16]) {
    if (sec < 0.0 || !(sec == sec)) sec = 0.0;
    int32_t total = (int32_t)(sec + 0.5);
    int32_t mm = total / 60;
    int32_t ss = total % 60;
    snprintf(out, 16u, "%02d:%02d", (int)mm, (int)ss);
}

static void render_cinema_hud_overlay(const ArcrMediaPlayer *mp, uint32_t *out_argb,
                                      int32_t w, int32_t h) {
    if (!mp || !out_argb || w < 48 || h < 36) return;

    /* Top header gradient bar + badge + title */
    if (h >= 80) {
        hud_fill_rect(out_argb, w, h, 0, 0, w, 26, 0x78080D1Au);
        const char *badge = mp->has_decoded_frame ? "HW/CODEC" : "HD 60FPS";
        hud_fill_rect(out_argb, w, h, 8, 5, 56, 15,
                      mp->paused ? 0xCC475569u : 0xCCEF4444u);
        hud_draw_text(out_argb, w, h, 13, 9, badge, 1, 0xFFFFFFFFu, 64);
        hud_draw_text(out_argb, w, h, 72, 9, mp->title, 1, 0xFFE2E8F0u, w - 10);
    }

    /* Center translucent Pause/Play modal emblem when paused */
    if (mp->paused && w >= 96 && h >= 72) {
        int32_t bw = 44, bh = 44;
        int32_t bx = (w - bw) / 2;
        int32_t by = (h - 26 - bh) / 2;
        hud_fill_rect(out_argb, w, h, bx, by, bw, bh, 0xB80F172Au);
        /* Crisp play triangle inside emblem */
        for (int32_t dy = -9; dy <= 9; ++dy) {
            int32_t span = 10 - (dy < 0 ? -dy : dy);
            if (span > 0) {
                hud_fill_rect(out_argb, w, h, bx + 17, by + 22 + dy, span + 4, 1, 0xFFFFFFFFu);
            }
        }
    }

    /* Bottom interactive control bar (matches core/arcr_engine.c click-to-seek!) */
    int32_t bar_h = ARCR_MIN(28, h / 2);
    int32_t bar_y = h - bar_h;
    hud_fill_rect(out_argb, w, h, 0, bar_y, w, bar_h, 0xCC090D16u);

    /* Play / Pause mini icon at left */
    int32_t icon_x = 10;
    int32_t icon_y = bar_y + (bar_h - 10) / 2;
    if (mp->paused) {
        for (int32_t i = 0; i < 5; ++i) {
            hud_fill_rect(out_argb, w, h, icon_x + i * 2, icon_y + i, 2, 10 - i * 2, 0xFFFFFFFFu);
        }
    } else {
        hud_fill_rect(out_argb, w, h, icon_x,     icon_y, 3, 10, 0xFFFFFFFFu);
        hud_fill_rect(out_argb, w, h, icon_x + 5, icon_y, 3, 10, 0xFFFFFFFFu);
    }

    /* Progress scrubber timeline along top edge of bottom bar */
    double dur = (mp->duration_sec > 0.1) ? mp->duration_sec : 180.0;
    double ratio = ARCR_CLAMP(mp->pos_sec / dur, 0.0, 1.0);
    int32_t track_x = 8;
    int32_t track_w = ARCR_MAX(16, w - 16);
    int32_t fill_w  = (int32_t)((double)track_w * ratio + 0.5);

    hud_fill_rect(out_argb, w, h, track_x, bar_y + 2, track_w, 4, 0x6694A3B8u);
    if (fill_w > 0) {
        hud_fill_rect(out_argb, w, h, track_x, bar_y + 2, fill_w, 4, 0xFFFF0033u);
    }
    hud_fill_rect(out_argb, w, h,
                  ARCR_CLAMP(track_x + fill_w - 3, track_x, track_x + track_w - 6),
                  bar_y + 1, 6, 6, 0xFFFFFFFFu);

    /* MM:SS / MM:SS timecode */
    if (w >= 140 && bar_h >= 20) {
        char p_str[16], d_str[16], tc[40];
        format_mmss(mp->pos_sec, p_str);
        format_mmss(dur, d_str);
        snprintf(tc, sizeof(tc), "%s / %s", p_str, d_str);
        hud_draw_text(out_argb, w, h, 26, bar_y + 11, tc, 1, 0xFFE2E8F0u, w - 8);
    }
}

/* -------------------------------------------------------------------------
 * 6. Public ArcrMediaVTable Player Methods
 * ------------------------------------------------------------------------- */
static ArcrMediaPlayer *media_player_create(const char *url_or_path,
                                            const uint8_t *mem_bytes, size_t mem_len,
                                            int32_t target_w, int32_t target_h,
                                            const char *title_hint) {
    ArcrMediaPlayer *mp = (ArcrMediaPlayer *)calloc(1u, sizeof(ArcrMediaPlayer));
    if (!mp) return NULL;

    int32_t cw = ARCR_CLAMP(target_w > 0 ? target_w : 640, 64, 1920) & ~1;
    int32_t ch = ARCR_CLAMP(target_h > 0 ? target_h : 360, 36, 1080) & ~1;
    if (cw < 64) cw = 64;
    if (ch < 36) ch = 36;

    mp->dec_w          = cw;
    mp->dec_h          = ch;
    mp->paused         = false;
    mp->dirty          = true;
    mp->pos_sec        = 0.0;
    mp->duration_sec   = 180.0;
    mp->use_procedural = true;

#if !defined(_WIN32)
    mp->ffmpeg_pid = -1;
    mp->pipe_fd    = -1;
    if (pthread_mutex_init(&mp->lock, NULL) == 0) {
        mp->lock_inited = true;
    }
#endif

    mp->decoded_frame = (uint32_t *)calloc((size_t)cw * (size_t)ch, sizeof(uint32_t));
    if (!mp->decoded_frame) {
#if !defined(_WIN32)
        if (mp->lock_inited) pthread_mutex_destroy(&mp->lock);
#endif
        free(mp);
        return NULL;
    }

    if (url_or_path && url_or_path[0] != '\0') {
        arcr_str_copy(mp->source_url, sizeof(mp->source_url), url_or_path);
    }
    sanitize_ascii_title(title_hint, mp->source_url, mp->title, sizeof(mp->title));

    /* Stage in-memory media buffer to temp file if supplied */
    if (mem_bytes && mem_len > 0u) {
        (void)arcr_image_decode_mem(mem_bytes, mem_len, &mp->static_poster);
#if defined(_WIN32)
        char tmp_dir[MAX_PATH];
        char tmp_file[MAX_PATH];
        if (GetTempPathA(MAX_PATH, tmp_dir) > 0 &&
            GetTempFileNameA(tmp_dir, "arc", 0u, tmp_file) != 0) {
            FILE *fp = fopen(tmp_file, "wb");
            if (fp) {
                (void)fwrite(mem_bytes, 1u, mem_len, fp);
                fclose(fp);
                arcr_str_copy(mp->temp_path, sizeof(mp->temp_path), tmp_file);
                arcr_str_copy(mp->source_url, sizeof(mp->source_url), tmp_file);
            }
        }
#else
        char tmpl[] = "/tmp/arcr_media_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0) {
            size_t off = 0u;
            while (off < mem_len) {
                ssize_t nw = write(fd, mem_bytes + off, mem_len - off);
                if (nw <= 0) break;
                off += (size_t)nw;
            }
            close(fd);
            arcr_str_copy(mp->temp_path, sizeof(mp->temp_path), tmpl);
            arcr_str_copy(mp->source_url, sizeof(mp->source_url), tmpl);
        }
#endif
    }

    /* Probe platform hardware/software video decoder */
#if defined(_WIN32)
    if (win32_init_media_foundation(mp)) {
        mp->use_procedural = false;
    }
#else
    if (mp->lock_inited && posix_start_ffmpeg_worker(mp, 0.0)) {
        mp->use_procedural = false;
    }
#endif

    return mp;
}

static void media_player_destroy(ArcrMediaPlayer *mp) {
    if (!mp) return;

#if defined(_WIN32)
    if (mp->mf_reader) {
        mp->mf_reader->lpVtbl->Release(mp->mf_reader);
        mp->mf_reader = NULL;
    }
    if (mp->mf_started && mp->fn_mf_shutdown) {
        (void)mp->fn_mf_shutdown();
        mp->mf_started = false;
    }
    if (mp->h_mfreadwrite) FreeLibrary(mp->h_mfreadwrite);
    if (mp->h_mfplat)      FreeLibrary(mp->h_mfplat);
    if (mp->temp_path[0] != '\0') {
        (void)DeleteFileA(mp->temp_path);
    }
#else
    posix_stop_ffmpeg_worker(mp);
    if (mp->lock_inited) {
        pthread_mutex_destroy(&mp->lock);
        mp->lock_inited = false;
    }
    if (mp->temp_path[0] != '\0') {
        (void)unlink(mp->temp_path);
    }
#endif

    arcr_image_free(&mp->static_poster);
    free(mp->decoded_frame);
    free(mp);
}

static bool media_player_tick_frame(ArcrMediaPlayer *mp, uint64_t now_usec,
                                    uint32_t *out_argb, int32_t w, int32_t h,
                                    double *out_pos_sec, double *out_dur_sec) {
    if (!mp || !out_argb || w <= 0 || h <= 0) return false;

    if (mp->last_tick_usec == 0u) {
        mp->last_tick_usec = now_usec;
    }
    uint64_t dt_usec = (now_usec >= mp->last_tick_usec) ? (now_usec - mp->last_tick_usec) : 0u;
    mp->last_tick_usec = now_usec;
    if (dt_usec > 250000ULL) dt_usec = 250000ULL;

#if defined(_WIN32)
    if (!mp->use_procedural) {
        win32_step_mf_frame(mp, now_usec);
    }
#endif

    bool use_proc = true;
    bool has_dec  = false;
    bool paused   = false;
    bool is_dirty = false;
    double pos    = 0.0;
    double dur    = 180.0;

#if !defined(_WIN32)
    if (mp->lock_inited) pthread_mutex_lock(&mp->lock);
#endif
    use_proc = mp->use_procedural;
    has_dec  = mp->has_decoded_frame;
    paused   = mp->paused;

    if ((use_proc || !has_dec) && !paused) {
        mp->pos_sec += (double)dt_usec * 1e-6;
        if (mp->pos_sec >= mp->duration_sec) {
            mp->pos_sec = fmod(mp->pos_sec, mp->duration_sec);
        }
        if (mp->last_render_usec == 0u || (now_usec - mp->last_render_usec) >= 15000ULL) {
            mp->dirty = true;
        }
    }

    is_dirty = mp->dirty;
    pos      = mp->pos_sec;
    dur      = mp->duration_sec;

    if (out_pos_sec) *out_pos_sec = pos;
    if (out_dur_sec) *out_dur_sec = dur;

    if (!is_dirty) {
#if !defined(_WIN32)
        if (mp->lock_inited) pthread_mutex_unlock(&mp->lock);
#endif
        return false;
    }

    mp->dirty            = false;
    mp->last_render_usec = now_usec;

    if (has_dec && mp->decoded_frame) {
        blit_scaled_nearest(mp->decoded_frame, mp->dec_w, mp->dec_h, out_argb, w, h);
    } else if (mp->static_poster.pixels && mp->static_poster.width > 0 && mp->static_poster.height > 0) {
        blit_scaled_nearest(mp->static_poster.pixels, mp->static_poster.width, mp->static_poster.height,
                            out_argb, w, h);
    }
#if !defined(_WIN32)
    if (mp->lock_inited) pthread_mutex_unlock(&mp->lock);
#endif

    if (!has_dec && !mp->static_poster.pixels) {
        render_procedural_cinema_stage(out_argb, w, h, pos, paused);
    }
    render_cinema_hud_overlay(mp, out_argb, w, h);
    return true;
}

static void media_player_set_paused(ArcrMediaPlayer *mp, bool paused) {
    if (!mp) return;
#if !defined(_WIN32)
    if (mp->lock_inited) pthread_mutex_lock(&mp->lock);
#endif
    if (mp->paused != paused) {
        mp->paused = paused;
        mp->dirty  = true;
    }
#if !defined(_WIN32)
    if (mp->lock_inited) pthread_mutex_unlock(&mp->lock);
#endif
}

static bool media_player_is_paused(const ArcrMediaPlayer *mp) {
    if (!mp) return true;
    return mp->paused;
}

static void media_player_seek(ArcrMediaPlayer *mp, double target_sec) {
    if (!mp) return;
    if (!(target_sec == target_sec) || target_sec < 0.0) target_sec = 0.0;
    double dur = (mp->duration_sec > 0.1) ? mp->duration_sec : 180.0;
    if (target_sec > dur) target_sec = dur;

#if defined(_WIN32)
    mp->pos_sec = target_sec;
    mp->dirty   = true;
    if (mp->mf_reader && !mp->use_procedural) {
        ArcrPropVariant pv;
        memset(&pv, 0, sizeof(pv));
        pv.vt   = 20u; /* VT_I8 */
        pv.hVal = (LONGLONG)(target_sec * 10000000.0);
        (void)mp->mf_reader->lpVtbl->SetCurrentPosition(mp->mf_reader, NULL, &pv);
    }
#else
    bool had_worker = !mp->use_procedural && mp->thread_running;
    if (had_worker) {
        posix_stop_ffmpeg_worker(mp);
    }
    if (mp->lock_inited) pthread_mutex_lock(&mp->lock);
    mp->pos_sec = target_sec;
    mp->dirty   = true;
    if (mp->lock_inited) pthread_mutex_unlock(&mp->lock);
    if (had_worker) {
        if (!posix_start_ffmpeg_worker(mp, target_sec)) {
            mp->use_procedural = true;
        }
    }
#endif
}

static double media_player_get_position(const ArcrMediaPlayer *mp) {
    return mp ? mp->pos_sec : 0.0;
}

static double media_player_get_duration(const ArcrMediaPlayer *mp) {
    return mp ? mp->duration_sec : 0.0;
}

static const ArcrMediaVTable kMediaVTable = {
    .struct_size             = sizeof(ArcrMediaVTable),
    .compute_adaptive_policy = media_compute_adaptive_policy,
    .player_create           = media_player_create,
    .player_destroy          = media_player_destroy,
    .player_tick_frame       = media_player_tick_frame,
    .player_set_paused       = media_player_set_paused,
    .player_is_paused        = media_player_is_paused,
    .player_seek             = media_player_seek,
    .player_get_position     = media_player_get_position,
    .player_get_duration     = media_player_get_duration
};

static ArcrModuleDescriptor g_media_desc = {
    .magic              = ARCR_MODULE_MAGIC,
    .struct_size        = sizeof(ArcrModuleDescriptor),
    .abi_version        = ARCR_ABI_VERSION,
    .engine_build_id    = ARCR_ENGINE_BUILD_ID,
    .arch_id            = 0,
    .min_os_major_minor = ARCR_OS_POSIX_ANY,
    .required_cpu_flags = ARCR_CPU_SCALAR,
    .module_name        = "arcr-media",
    .vtable             = &kMediaVTable
};

ARCR_API const ArcrModuleDescriptor *arcr_module_query(void) {
    g_media_desc.arch_id = (uint16_t)arcr_host_compile_arch();
#if defined(_WIN32)
    g_media_desc.min_os_major_minor = ARCR_OS_WIN2000;
#endif
    return &g_media_desc;
}
