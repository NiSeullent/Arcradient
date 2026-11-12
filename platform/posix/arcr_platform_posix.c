/*
 * Waterscape Browser / Arcradient Engine (MSHTML-Next)
 * platform/posix/arcr_platform_posix.c - Linux/POSIX Backend + Thread-Safe Cached FreeType2 Fonts
 */
#define _POSIX_C_SOURCE 200809L
#include "../arcr_platform.h"
#include "../../base/arcr_codec.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(ARCR_HAVE_FREETYPE)
#  include <ft2build.h>
#  include FT_FREETYPE_H
#endif

#if defined(ARCR_HAVE_LIBPNG)
#  include <png.h>
#endif

/* -------------------------------------------------------------------------
 * 1. OS Version, Monotonic/Wall Clocks & CSPRNG
 * ------------------------------------------------------------------------- */
uint16_t arcr_platform_os_version(void) {
    return ARCR_OS_POSIX_ANY;
}

const char *arcr_platform_os_name(void) {
#if defined(__linux__)
    return "Linux (POSIX Backend)";
#elif defined(__APPLE__)
    return "macOS (POSIX Backend)";
#else
    return "POSIX";
#endif
}

uint64_t arcr_platform_monotonic_usec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000L);
}

int64_t arcr_platform_wall_epoch_sec(void) {
    return (int64_t)time(NULL);
}

bool arcr_platform_csprng_bytes(uint8_t *buf, size_t len) {
    if (!buf || len == 0u) return false;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    size_t done = 0u;
    while (done < len) {
        ssize_t r = read(fd, buf + done, len - done);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            close(fd);
            return false;
        }
        done += (size_t)r;
    }
    close(fd);
    return true;
}

/* -------------------------------------------------------------------------
 * 2. Profile / Downloads Directories, Recursive mkdir_p & UTF-8 Clipboard
 * ------------------------------------------------------------------------- */
static bool is_existing_dir(const char *path) {
    struct stat st;
    return (path && stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

bool arcr_platform_mkdir_p(const char *path) {
    if (!path || path[0] == '\0') return false;
    if (is_existing_dir(path)) return true;

    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return false;
    memcpy(tmp, path, len + 1u);

    while (len > 1u && tmp[len - 1u] == '/') {
        tmp[--len] = '\0';
    }

    for (size_t i = 1u; i < len; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (tmp[0] != '\0' && !is_existing_dir(tmp)) {
                if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                    return false;
                }
            }
            tmp[i] = '/';
        }
    }

    if (!is_existing_dir(tmp)) {
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            return false;
        }
    }
    return is_existing_dir(tmp);
}

bool arcr_platform_get_profile_dir(char *out_dir, size_t cap) {
    if (!out_dir || cap < 2u) return false;

    const char *override_dir = getenv("WATERSCAPE_PROFILE_DIR");
    if (override_dir && override_dir[0] != '\0') {
        size_t n = arcr_str_copy(out_dir, cap, override_dir);
        if (n >= cap) return false;
        (void)arcr_platform_mkdir_p(out_dir);
        return true;
    }

    const char *home = getenv("HOME");
    int written;
    if (home && home[0] != '\0') {
        written = snprintf(out_dir, cap, "%s/.waterscape", home);
    } else {
        written = snprintf(out_dir, cap, "/tmp/.waterscape");
    }
    if (written <= 0 || (size_t)written >= cap) return false;
    (void)arcr_platform_mkdir_p(out_dir);
    return true;
}

bool arcr_platform_get_downloads_dir(char *out_dir, size_t cap) {
    if (!out_dir || cap < 2u) return false;

    const char *override_dir = getenv("WATERSCAPE_DOWNLOADS_DIR");
    if (override_dir && override_dir[0] != '\0') {
        size_t n = arcr_str_copy(out_dir, cap, override_dir);
        if (n >= cap) return false;
        (void)arcr_platform_mkdir_p(out_dir);
        return true;
    }

    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        int written = snprintf(out_dir, cap, "%s/Downloads", home);
        if (written > 0 && (size_t)written < cap && arcr_platform_mkdir_p(out_dir)) {
            return true;
        }
    }

    int fallback_n = snprintf(out_dir, cap, "/tmp/waterscape-downloads");
    if (fallback_n <= 0 || (size_t)fallback_n >= cap) return false;
    (void)arcr_platform_mkdir_p(out_dir);
    return true;
}

static pthread_mutex_t g_clipboard_mu       = PTHREAD_MUTEX_INITIALIZER;
static char            g_clipboard_buf[65536];
static bool            g_clipboard_has_text = false;

bool arcr_platform_clipboard_set_utf8(const char *utf8_text) {
    if (!utf8_text) return false;
    pthread_mutex_lock(&g_clipboard_mu);
    (void)arcr_str_copy(g_clipboard_buf, sizeof(g_clipboard_buf), utf8_text);
    g_clipboard_has_text = true;
    pthread_mutex_unlock(&g_clipboard_mu);
    return true;
}

bool arcr_platform_clipboard_get_utf8(char *out_buf, size_t cap) {
    if (!out_buf || cap == 0u) return false;
    pthread_mutex_lock(&g_clipboard_mu);
    if (!g_clipboard_has_text) {
        out_buf[0] = '\0';
        pthread_mutex_unlock(&g_clipboard_mu);
        return false;
    }
    (void)arcr_str_copy(out_buf, cap, g_clipboard_buf);
    pthread_mutex_unlock(&g_clipboard_mu);
    return true;
}

/* -------------------------------------------------------------------------
 * 3. Dynamic Module Loading & System UI Metrics
 * ------------------------------------------------------------------------- */
struct ArcrDynLib {
    void *handle;
};

void arcr_platform_format_module_path(char *out_buf, size_t cap,
                                      const char *dir, const char *mod_stem) {
    if (!out_buf || cap == 0u) return;
#if defined(__APPLE__)
    const char *ext = ".dylib";
#else
    const char *ext = ".so";
#endif
    const char *d = (dir && dir[0] != '\0') ? dir : ".";
    snprintf(out_buf, cap, "%s/lib%s%s", d, mod_stem, ext);
}

ArcrDynLib *arcr_platform_dlopen(const char *path) {
    if (!path) return NULL;
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) return NULL;
    ArcrDynLib *lib = (ArcrDynLib *)malloc(sizeof(ArcrDynLib));
    if (!lib) {
        dlclose(h);
        return NULL;
    }
    lib->handle = h;
    return lib;
}

void *arcr_platform_dlsym(ArcrDynLib *lib, const char *symbol_name) {
    if (!lib || !lib->handle || !symbol_name) return NULL;
    return dlsym(lib->handle, symbol_name);
}

void arcr_platform_dlclose(ArcrDynLib *lib) {
    if (!lib) return;
    if (lib->handle) dlclose(lib->handle);
    free(lib);
}

void arcr_platform_query_ui_metrics(ArcrSystemUiMetrics *out_metrics) {
    if (!out_metrics) return;
    out_metrics->prefer_classic_opaque = false;
    out_metrics->high_contrast         = false;
    out_metrics->sys_face_argb         = 0xFFD4D0C8u; /* Classic Win2000 3DFace reference */
    out_metrics->sys_window_argb       = 0xFFFFFFFFu;
    out_metrics->sys_text_argb         = 0xFF000000u;
    out_metrics->sys_highlight_argb    = 0xFF0A246Au; /* Classic Win2000 active navy */
    out_metrics->dpi                   = 96;
}

/* -------------------------------------------------------------------------
 * 4. Thread-Safe O(1) Cached Font System (FreeType2 Sans/CJK/Mono + Fallback)
 * ------------------------------------------------------------------------- */
enum {
    ARCR_GLYPH_CACHE_SETS     = 512,
    ARCR_GLYPH_CACHE_WAYS     = 4,
    ARCR_GLYPH_POOL_BYTES     = 2097152, /* 2 MB glyph alpha slab buffer */
    ARCR_GLYPH_MAX_CACHE_AREA = 65536    /* Cache bitmaps up to 256x256 px */
};

typedef struct ArcrGlyphCacheSlot {
    uint64_t         key;        /* 0 = empty; bit 63 = 1 when valid */
    uint32_t         bitmap_off; /* Byte offset inside bitmap_pool */
    uint32_t         last_used;  /* Monotonic stamp for 4-way LRU replacement */
    ArcrGlyphMetrics metrics;
    bool             has_bitmap;
} ArcrGlyphCacheSlot;

struct ArcrFontSystem {
    pthread_mutex_t    mutex_storage;
    pthread_mutex_t   *mutex; /* Non-const handle so const stats query locks cleanly */
    bool               mutex_ok;
    bool               has_ft;
#if defined(ARCR_HAVE_FREETYPE)
    FT_Library         ft_lib;
    FT_Face            face_regular;
    FT_Face            face_bold;
    FT_Face            face_mono;
    FT_Face            face_mono_bold;
    int32_t            cur_reg_px;
    int32_t            cur_bold_px;
    int32_t            cur_mono_px;
    int32_t            cur_mono_bold_px;
#endif
    ArcrGlyphCacheSlot slots[ARCR_GLYPH_CACHE_SETS][ARCR_GLYPH_CACHE_WAYS];
    uint8_t           *bitmap_pool;
    size_t             bitmap_pool_used;
    size_t             cache_entries;
    size_t             cache_hits;
    uint32_t           access_clock;
};

#if defined(ARCR_HAVE_FREETYPE)
static const char *const kDefaultCjkFontCandidates[] = {
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    NULL
};

static const char *const kDefaultCjkBoldCandidates[] = {
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/truetype/nanum/NanumGothicBold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    NULL
};

static const char *const kDefaultMonoFontCandidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
    NULL
};

static const char *const kDefaultMonoBoldCandidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansMono-Bold.ttf",
    NULL
};
#endif

static inline uint64_t make_glyph_key(uint32_t cp, int32_t font_px, bool bold, bool monospace) {
    uint64_t px_u  = (uint64_t)(uint32_t)font_px & 0xFFFFULL;
    uint64_t flags = (bold ? 1ULL : 0ULL) | (monospace ? 2ULL : 0ULL);
    return (1ULL << 63) | ((uint64_t)cp << 18) | (px_u << 2) | flags;
}

static inline uint32_t hash_glyph_key_set(uint64_t key) {
    uint64_t h = key * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 32;
    h ^= h >> 15;
    return (uint32_t)h & (uint32_t)(ARCR_GLYPH_CACHE_SETS - 1);
}

static void reset_glyph_cache_locked(ArcrFontSystem *fs, bool reset_hits) {
    memset(fs->slots, 0, sizeof(fs->slots));
    fs->bitmap_pool_used = 0u;
    fs->cache_entries    = 0u;
    fs->access_clock     = 0u;
    if (reset_hits) {
        fs->cache_hits = 0u;
    }
}

ArcrFontSystem *arcr_font_system_create(const char *preferred_cjk_font_path) {
    ArcrFontSystem *fs = (ArcrFontSystem *)calloc(1u, sizeof(ArcrFontSystem));
    if (!fs) return NULL;

    if (pthread_mutex_init(&fs->mutex_storage, NULL) == 0) {
        fs->mutex    = &fs->mutex_storage;
        fs->mutex_ok = true;
    }

    fs->bitmap_pool = (uint8_t *)malloc(ARCR_GLYPH_POOL_BYTES);

#if defined(ARCR_HAVE_FREETYPE)
    if (FT_Init_FreeType(&fs->ft_lib) == 0) {
        /* Note: In NotoSansCJK-*.ttc, face index 1 is typically Noto Sans CJK KR */
        if (preferred_cjk_font_path && preferred_cjk_font_path[0] != '\0') {
            if (FT_New_Face(fs->ft_lib, preferred_cjk_font_path, 1, &fs->face_regular) != 0) {
                (void)FT_New_Face(fs->ft_lib, preferred_cjk_font_path, 0, &fs->face_regular);
            }
        }
        for (size_t i = 0u; !fs->face_regular && kDefaultCjkFontCandidates[i]; ++i) {
            const char *p = kDefaultCjkFontCandidates[i];
            if (FT_New_Face(fs->ft_lib, p, 1, &fs->face_regular) != 0) {
                (void)FT_New_Face(fs->ft_lib, p, 0, &fs->face_regular);
            }
        }
        for (size_t i = 0u; !fs->face_bold && kDefaultCjkBoldCandidates[i]; ++i) {
            const char *p = kDefaultCjkBoldCandidates[i];
            if (FT_New_Face(fs->ft_lib, p, 1, &fs->face_bold) != 0) {
                (void)FT_New_Face(fs->ft_lib, p, 0, &fs->face_bold);
            }
        }
        for (size_t i = 0u; !fs->face_mono && kDefaultMonoFontCandidates[i]; ++i) {
            (void)FT_New_Face(fs->ft_lib, kDefaultMonoFontCandidates[i], 0, &fs->face_mono);
        }
        for (size_t i = 0u; !fs->face_mono_bold && kDefaultMonoBoldCandidates[i]; ++i) {
            (void)FT_New_Face(fs->ft_lib, kDefaultMonoBoldCandidates[i], 0, &fs->face_mono_bold);
        }

        if (fs->face_regular || fs->face_mono) {
            if (!fs->face_regular) fs->face_regular = fs->face_mono;
            fs->has_ft = true;
        } else {
            FT_Done_FreeType(fs->ft_lib);
        }
    }
#else
    (void)preferred_cjk_font_path;
#endif
    return fs;
}

void arcr_font_system_destroy(ArcrFontSystem *fs) {
    if (!fs) return;
#if defined(ARCR_HAVE_FREETYPE)
    if (fs->has_ft) {
        if (fs->face_mono_bold) FT_Done_Face(fs->face_mono_bold);
        if (fs->face_mono && fs->face_mono != fs->face_regular) FT_Done_Face(fs->face_mono);
        if (fs->face_bold) FT_Done_Face(fs->face_bold);
        if (fs->face_regular) FT_Done_Face(fs->face_regular);
        FT_Done_FreeType(fs->ft_lib);
    }
#endif
    free(fs->bitmap_pool);
    if (fs->mutex_ok) {
        pthread_mutex_destroy(&fs->mutex_storage);
    }
    free(fs);
}

void arcr_font_system_purge_cache(ArcrFontSystem *fs) {
    if (!fs) return;
    if (fs->mutex_ok) pthread_mutex_lock(fs->mutex);
    reset_glyph_cache_locked(fs, true);
    if (fs->mutex_ok) pthread_mutex_unlock(fs->mutex);
}

void arcr_font_system_cache_stats(const ArcrFontSystem *fs,
                                  size_t *out_entries, size_t *out_hits) {
    if (!fs) {
        if (out_entries) *out_entries = 0u;
        if (out_hits) *out_hits = 0u;
        return;
    }
    if (fs->mutex_ok) pthread_mutex_lock(fs->mutex);
    if (out_entries) *out_entries = fs->cache_entries;
    if (out_hits) *out_hits = fs->cache_hits;
    if (fs->mutex_ok) pthread_mutex_unlock(fs->mutex);
}

/* Built-in procedural crisp fallback glyph so engine works with 0 font files */
static void fallback_glyph(uint32_t cp, int32_t font_px, bool bold, bool monospace,
                           ArcrGlyphMetrics *out_m, uint8_t *out_alpha, size_t cap) {
    bool wide = arcr_cp_is_hangul(cp) || arcr_cp_is_cjk_ideograph(cp) || (cp > 0x2000u);
    int32_t adv = wide ? font_px : ARCR_MAX(6, (font_px * 3) / 5);
    int32_t gw  = ARCR_MAX(4, adv - 2);
    int32_t gh  = ARCR_MAX(6, (font_px * 3) / 4);
    if (cp == ' ') {
        out_m->advance_px = adv;
        out_m->bearing_x  = 0;
        out_m->bearing_y  = 0;
        out_m->width      = 0;
        out_m->height     = 0;
        return;
    }
    out_m->advance_px = adv + ((bold && !monospace) ? 1 : 0);
    out_m->bearing_x  = 1;
    out_m->bearing_y  = gh - 1;
    out_m->width      = gw;
    out_m->height     = gh;
    if (out_alpha && (size_t)(gw * gh) <= cap) {
        memset(out_alpha, 0, (size_t)(gw * gh));
        for (int32_t y = 0; y < gh; ++y) {
            for (int32_t x = 0; x < gw; ++x) {
                bool edge = (y == 0 || y == gh - 1 || x == 0 || x == gw - 1);
                bool pat  = (((x + y + (int32_t)(cp & 7u)) % 3) == 0);
                out_alpha[y * gw + x] = (edge || pat) ? 220u : 0u;
            }
        }
    }
}

#if defined(ARCR_HAVE_FREETYPE)
static FT_Face select_ft_face_locked(ArcrFontSystem *fs, uint32_t cp, int32_t font_px,
                                     bool bold, bool monospace, FT_UInt *out_gidx) {
    FT_Face candidates[2] = {NULL, NULL};
    int32_t *px_slots[2]  = {NULL, NULL};
    size_t   cand_count   = 0u;

    if (monospace) {
        if (bold && fs->face_mono_bold) {
            candidates[cand_count] = fs->face_mono_bold;
            px_slots[cand_count++] = &fs->cur_mono_bold_px;
        } else if (fs->face_mono) {
            candidates[cand_count] = fs->face_mono;
            px_slots[cand_count++] = &fs->cur_mono_px;
        }
    }

    if (bold && fs->face_bold) {
        candidates[cand_count] = fs->face_bold;
        px_slots[cand_count++] = &fs->cur_bold_px;
    } else if (fs->face_regular) {
        candidates[cand_count] = fs->face_regular;
        px_slots[cand_count++] = &fs->cur_reg_px;
    }

    for (size_t i = 0u; i < cand_count; ++i) {
        FT_Face face = candidates[i];
        if (!face) continue;
        FT_UInt gidx = FT_Get_Char_Index(face, (FT_ULong)cp);
        if (gidx != 0u) {
            if (*px_slots[i] != font_px) {
                FT_Set_Pixel_Sizes(face, 0u, (FT_UInt)font_px);
                *px_slots[i] = font_px;
            }
            *out_gidx = gidx;
            return face;
        }
    }
    *out_gidx = 0u;
    return NULL;
}
#endif

static ArcrGlyphCacheSlot *find_glyph_slot_locked(ArcrFontSystem *fs, uint32_t set_idx, uint64_t key) {
    for (size_t w = 0u; w < ARCR_GLYPH_CACHE_WAYS; ++w) {
        if (fs->slots[set_idx][w].key == key) {
            return &fs->slots[set_idx][w];
        }
    }
    return NULL;
}

static ArcrGlyphCacheSlot *alloc_glyph_slot_locked(ArcrFontSystem *fs, uint32_t set_idx, uint64_t key) {
    size_t   victim = 0u;
    uint32_t oldest = UINT32_MAX;
    for (size_t w = 0u; w < ARCR_GLYPH_CACHE_WAYS; ++w) {
        if (fs->slots[set_idx][w].key == 0ULL) {
            fs->cache_entries++;
            victim = w;
            oldest = 0u;
            break;
        }
        if (fs->slots[set_idx][w].last_used < oldest) {
            oldest = fs->slots[set_idx][w].last_used;
            victim = w;
        }
    }
    ArcrGlyphCacheSlot *slot = &fs->slots[set_idx][victim];
    memset(slot, 0, sizeof(*slot));
    slot->key       = key;
    slot->last_used = ++fs->access_clock;
    return slot;
}

bool arcr_font_measure_glyph_ex(ArcrFontSystem *fs, uint32_t codepoint,
                                int32_t font_px, bool bold, bool monospace,
                                ArcrGlyphMetrics *out_metrics) {
    if (!out_metrics) return false;
    if (font_px < 8) font_px = 8;
    if (font_px > 512) font_px = 512;

    if (!fs) {
        fallback_glyph(codepoint, font_px, bold, monospace, out_metrics, NULL, 0u);
        return true;
    }

    uint64_t key     = make_glyph_key(codepoint, font_px, bold, monospace);
    uint32_t set_idx = hash_glyph_key_set(key);

    if (fs->mutex_ok) pthread_mutex_lock(fs->mutex);

    ArcrGlyphCacheSlot *hit = find_glyph_slot_locked(fs, set_idx, key);
    if (hit) {
        fs->cache_hits++;
        hit->last_used = ++fs->access_clock;
        *out_metrics   = hit->metrics;
        if (fs->mutex_ok) pthread_mutex_unlock(fs->mutex);
        return true;
    }

    ArcrGlyphMetrics m;
    memset(&m, 0, sizeof(m));
    bool measured = false;

#if defined(ARCR_HAVE_FREETYPE)
    if (fs->has_ft) {
        FT_UInt gidx = 0u;
        FT_Face face = select_ft_face_locked(fs, codepoint, font_px, bold, monospace, &gidx);
        if (face && gidx != 0u && FT_Load_Glyph(face, gidx, FT_LOAD_DEFAULT) == 0) {
            m.advance_px = (int32_t)((face->glyph->advance.x + 32) >> 6);
            m.bearing_x  = (int32_t)(face->glyph->metrics.horiBearingX >> 6);
            m.bearing_y  = (int32_t)(face->glyph->metrics.horiBearingY >> 6);
            m.width      = (int32_t)(face->glyph->metrics.width >> 6);
            m.height     = (int32_t)(face->glyph->metrics.height >> 6);
            measured     = true;
        }
    }
#endif
    if (!measured) {
        fallback_glyph(codepoint, font_px, bold, monospace, &m, NULL, 0u);
    }

    ArcrGlyphCacheSlot *slot = alloc_glyph_slot_locked(fs, set_idx, key);
    slot->metrics    = m;
    slot->has_bitmap = false;
    *out_metrics     = m;

    if (fs->mutex_ok) pthread_mutex_unlock(fs->mutex);
    return true;
}

bool arcr_font_measure_glyph(ArcrFontSystem *fs, uint32_t codepoint,
                             int32_t font_px, bool bold,
                             ArcrGlyphMetrics *out_metrics) {
    return arcr_font_measure_glyph_ex(fs, codepoint, font_px, bold, false, out_metrics);
}

bool arcr_font_rasterize_glyph_ex(ArcrFontSystem *fs, uint32_t codepoint,
                                  int32_t font_px, bool bold, bool monospace,
                                  ArcrGlyphMetrics *out_metrics,
                                  uint8_t *out_alpha, size_t alpha_cap) {
    if (!out_metrics || !out_alpha) return false;
    if (font_px < 8) font_px = 8;
    if (font_px > 512) font_px = 512;

    if (!fs) {
        fallback_glyph(codepoint, font_px, bold, monospace, out_metrics, out_alpha, alpha_cap);
        return true;
    }

    uint64_t key     = make_glyph_key(codepoint, font_px, bold, monospace);
    uint32_t set_idx = hash_glyph_key_set(key);

    if (fs->mutex_ok) pthread_mutex_lock(fs->mutex);

    ArcrGlyphCacheSlot *slot = find_glyph_slot_locked(fs, set_idx, key);
    if (slot && slot->has_bitmap && fs->bitmap_pool) {
        size_t area = (size_t)slot->metrics.width * (size_t)slot->metrics.height;
        if (area > alpha_cap) {
            if (fs->mutex_ok) pthread_mutex_unlock(fs->mutex);
            return false;
        }
        fs->cache_hits++;
        slot->last_used = ++fs->access_clock;
        *out_metrics    = slot->metrics;
        if (area > 0u) {
            memcpy(out_alpha, fs->bitmap_pool + slot->bitmap_off, area);
        }
        if (fs->mutex_ok) pthread_mutex_unlock(fs->mutex);
        return true;
    }

    ArcrGlyphMetrics m;
    memset(&m, 0, sizeof(m));
    bool rendered = false;

#if defined(ARCR_HAVE_FREETYPE)
    if (fs->has_ft) {
        FT_UInt gidx = 0u;
        FT_Face face = select_ft_face_locked(fs, codepoint, font_px, bold, monospace, &gidx);
        if (face && gidx != 0u && FT_Load_Glyph(face, gidx, FT_LOAD_RENDER) == 0) {
            FT_GlyphSlot gs = face->glyph;
            int32_t w = (int32_t)gs->bitmap.width;
            int32_t h = (int32_t)gs->bitmap.rows;
            m.advance_px = (int32_t)((gs->advance.x + 32) >> 6);
            m.bearing_x  = gs->bitmap_left;
            m.bearing_y  = gs->bitmap_top;
            m.width      = w;
            m.height     = h;

            if ((size_t)w * (size_t)h > alpha_cap) {
                if (fs->mutex_ok) pthread_mutex_unlock(fs->mutex);
                return false;
            }
            if (w == 0 || h == 0) {
                rendered = true;
            } else if (gs->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
                for (int32_t r = 0; r < h; ++r) {
                    memcpy(out_alpha + (size_t)r * (size_t)w,
                           gs->bitmap.buffer + r * gs->bitmap.pitch,
                           (size_t)w);
                }
                rendered = true;
            } else if (gs->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                for (int32_t r = 0; r < h; ++r) {
                    const uint8_t *row = gs->bitmap.buffer + r * gs->bitmap.pitch;
                    for (int32_t c = 0; c < w; ++c) {
                        bool bit = (row[c >> 3] & (0x80u >> (c & 7))) != 0u;
                        out_alpha[(size_t)r * (size_t)w + (size_t)c] = bit ? 255u : 0u;
                    }
                }
                rendered = true;
            }
        }
    }
#endif

    if (!rendered) {
        fallback_glyph(codepoint, font_px, bold, monospace, &m, out_alpha, alpha_cap);
        if ((size_t)m.width * (size_t)m.height > alpha_cap) {
            if (fs->mutex_ok) pthread_mutex_unlock(fs->mutex);
            return false;
        }
    }

    size_t area = (size_t)m.width * (size_t)m.height;
    if (fs->bitmap_pool && area <= ARCR_GLYPH_MAX_CACHE_AREA) {
        if (fs->bitmap_pool_used + area > ARCR_GLYPH_POOL_BYTES) {
            reset_glyph_cache_locked(fs, false);
            slot = NULL;
        }
        if (!slot) {
            slot = alloc_glyph_slot_locked(fs, set_idx, key);
        } else {
            slot->last_used = ++fs->access_clock;
        }
        slot->metrics    = m;
        slot->bitmap_off = (uint32_t)fs->bitmap_pool_used;
        slot->has_bitmap = true;
        if (area > 0u) {
            memcpy(fs->bitmap_pool + fs->bitmap_pool_used, out_alpha, area);
            fs->bitmap_pool_used += area;
        }
    }

    *out_metrics = m;
    if (fs->mutex_ok) pthread_mutex_unlock(fs->mutex);
    return true;
}

bool arcr_font_rasterize_glyph(ArcrFontSystem *fs, uint32_t codepoint,
                               int32_t font_px, bool bold,
                               ArcrGlyphMetrics *out_metrics,
                               uint8_t *out_alpha, size_t alpha_cap) {
    return arcr_font_rasterize_glyph_ex(fs, codepoint, font_px, bold, false,
                                        out_metrics, out_alpha, alpha_cap);
}

/* -------------------------------------------------------------------------
 * 5. True PNG Framebuffer Export (libpng or Pure C arcr_codec PNG encoder)
 * ------------------------------------------------------------------------- */
bool arcr_platform_write_png(const char *filepath, const uint32_t *argb_pixels,
                             int32_t width, int32_t height, int32_t stride_pixels) {
    if (!filepath || !argb_pixels || width <= 0 || height <= 0) return false;
    if (stride_pixels < width) stride_pixels = width;

#if defined(ARCR_HAVE_LIBPNG)
    FILE *fp = fopen(filepath, "wb");
    if (!fp) return false;

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        fclose(fp);
        return false;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        fclose(fp);
        return false;
    }
    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        return false;
    }

    png_init_io(png_ptr, fp);
    png_set_IHDR(png_ptr, info_ptr, (png_uint_32)width, (png_uint_32)height,
                 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);

    uint8_t *row_buf = (uint8_t *)malloc((size_t)width * 4u);
    if (!row_buf) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        return false;
    }

    for (int32_t y = 0; y < height; ++y) {
        const uint32_t *src = argb_pixels + (size_t)y * (size_t)stride_pixels;
        for (int32_t x = 0; x < width; ++x) {
            uint32_t px = src[x];
            row_buf[x * 4 + 0] = (uint8_t)((px >> 16) & 0xFFu); /* R */
            row_buf[x * 4 + 1] = (uint8_t)((px >> 8) & 0xFFu);  /* G */
            row_buf[x * 4 + 2] = (uint8_t)(px & 0xFFu);         /* B */
            row_buf[x * 4 + 3] = (uint8_t)((px >> 24) & 0xFFu); /* A */
        }
        png_write_row(png_ptr, row_buf);
    }

    free(row_buf);
    png_write_end(png_ptr, NULL);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);
    return true;
#else
    return arcr_image_write_png_file(filepath, argb_pixels, width, height, stride_pixels);
#endif
}
