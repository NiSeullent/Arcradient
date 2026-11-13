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
#else
#  include <math.h>
#  if defined(__GNUC__) || defined(__clang__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wpragmas"
#    pragma GCC diagnostic ignored "-Wunused-function"
#    pragma GCC diagnostic ignored "-Wunused-parameter"
#    pragma GCC diagnostic ignored "-Wunused-variable"
#    pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#    pragma GCC diagnostic ignored "-Wsign-compare"
#    pragma GCC diagnostic ignored "-Wpedantic"
#  endif
#  define STBTT_STATIC
#  define STB_TRUETYPE_IMPLEMENTATION
#  include "stb_truetype.h"
#  if defined(__GNUC__) || defined(__clang__)
#    pragma GCC diagnostic pop
#  endif
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
 * 2. Executable / Profile / Downloads Directories, mkdir_p & UTF-8 Clipboard
 * ------------------------------------------------------------------------- */
bool arcr_platform_get_executable_dir(char *out_dir, size_t cap) {
    if (!out_dir || cap < 2u) return false;
    out_dir[0] = '\0';

    char exe_buf[4096];
    ssize_t n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1u);
    if (n <= 0) {
        n = readlink("/proc/curproc/file", exe_buf, sizeof(exe_buf) - 1u);
    }
    if (n > 0) {
        exe_buf[(size_t)n] = '\0';
        char *slash = strrchr(exe_buf, '/');
        if (slash) {
            if (slash == exe_buf) {
                exe_buf[1] = '\0';
            } else {
                *slash = '\0';
            }
            size_t copied = arcr_str_copy(out_dir, cap, exe_buf);
            return (copied > 0u && copied < cap);
        }
    }

    if (getcwd(exe_buf, sizeof(exe_buf)) != NULL) {
        size_t copied = arcr_str_copy(out_dir, cap, exe_buf);
        return (copied > 0u && copied < cap);
    }
    return false;
}

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
 * 4. Perceptual sRGB Gamma-Corrected Anti-Aliased Font System + Lock-Free Atlas
 * ------------------------------------------------------------------------- */
/*
 * Perceptual sRGB stem-darkening & gamma-correction LUT:
 * alpha' = clamp(round(pow(alpha / 255.0, 1.0 / 1.42) * 255.0 * 1.04), 0, 255)
 * Transforms washed-out linear coverage into crisp DirectWrite/CoreText-grade stems.
 */
static const uint8_t kPerceptualFontGammaLut[256] = {
      0u,   5u,   9u,  12u,  14u,  17u,  19u,  21u,  23u,  25u,  27u,  29u,  31u,  33u,  34u,  36u,
     38u,  39u,  41u,  43u,  44u,  46u,  47u,  49u,  50u,  52u,  53u,  55u,  56u,  57u,  59u,  60u,
     61u,  63u,  64u,  65u,  67u,  68u,  69u,  71u,  72u,  73u,  74u,  76u,  77u,  78u,  79u,  81u,
     82u,  83u,  84u,  85u,  87u,  88u,  89u,  90u,  91u,  92u,  93u,  95u,  96u,  97u,  98u,  99u,
    100u, 101u, 102u, 103u, 105u, 106u, 107u, 108u, 109u, 110u, 111u, 112u, 113u, 114u, 115u, 116u,
    117u, 118u, 119u, 120u, 121u, 122u, 123u, 124u, 125u, 126u, 127u, 128u, 129u, 130u, 131u, 132u,
    133u, 134u, 135u, 136u, 137u, 138u, 139u, 140u, 141u, 142u, 143u, 144u, 145u, 146u, 147u, 148u,
    149u, 150u, 150u, 151u, 152u, 153u, 154u, 155u, 156u, 157u, 158u, 159u, 160u, 161u, 161u, 162u,
    163u, 164u, 165u, 166u, 167u, 168u, 169u, 169u, 170u, 171u, 172u, 173u, 174u, 175u, 176u, 176u,
    177u, 178u, 179u, 180u, 181u, 182u, 183u, 183u, 184u, 185u, 186u, 187u, 188u, 188u, 189u, 190u,
    191u, 192u, 193u, 194u, 194u, 195u, 196u, 197u, 198u, 199u, 199u, 200u, 201u, 202u, 203u, 203u,
    204u, 205u, 206u, 207u, 208u, 208u, 209u, 210u, 211u, 212u, 212u, 213u, 214u, 215u, 216u, 216u,
    217u, 218u, 219u, 220u, 220u, 221u, 222u, 223u, 223u, 224u, 225u, 226u, 227u, 227u, 228u, 229u,
    230u, 231u, 231u, 232u, 233u, 234u, 234u, 235u, 236u, 237u, 237u, 238u, 239u, 240u, 241u, 241u,
    242u, 243u, 244u, 244u, 245u, 246u, 247u, 247u, 248u, 249u, 250u, 250u, 251u, 252u, 253u, 253u,
    254u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u, 255u
};

static const uint8_t kEmptyGlyphAlpha = 0u;

enum {
    ARCR_GLYPH_HASH_CAP       = 8192,
    ARCR_GLYPH_HASH_MASK      = ARCR_GLYPH_HASH_CAP - 1,
    ARCR_GLYPH_ARENA_BLOCK_SZ = 131072,   /* 128 KB immutable glyph bitmap chunks */
    ARCR_GLYPH_MAX_FONT_PX    = 256,
    ARCR_GLYPH_MAX_DIM        = 384
};

typedef struct ArcrGlyphCacheSlot {
    uint32_t         state;      /* 0 = empty, 1 = published (acquire/release) */
    uint64_t         key;        /* Valid once state == 1 */
    const uint8_t   *bitmap_ptr; /* Immutable pointer in bitmap_arena or &kEmptyGlyphAlpha */
    ArcrGlyphMetrics metrics;
} ArcrGlyphCacheSlot;

#if !defined(ARCR_HAVE_FREETYPE)
typedef struct ArcrStbFontFace {
    uint8_t       *ttf_bytes;
    stbtt_fontinfo info;
    bool           valid;
} ArcrStbFontFace;
#endif

struct ArcrFontSystem {
    pthread_mutex_t    mutex_storage;
    pthread_mutex_t   *mutex;
    bool               mutex_ok;
    bool               has_ft;
#if defined(ARCR_HAVE_FREETYPE)
    FT_Library         ft_lib;
    FT_Face            face_regular;
    FT_Face            face_bold;
    FT_Face            face_mono;
    FT_Face            face_mono_bold;
    FT_Face            face_symbols;
    FT_Face            face_emoji;
    int32_t            cur_reg_px;
    int32_t            cur_bold_px;
    int32_t            cur_mono_px;
    int32_t            cur_mono_bold_px;
    int32_t            cur_symbols_px;
    int32_t            cur_emoji_px;
#else
    ArcrStbFontFace    stb_reg;
    ArcrStbFontFace    stb_bold;
    ArcrStbFontFace    stb_mono;
#endif
    ArcrGlyphCacheSlot slots[ARCR_GLYPH_HASH_CAP];
    ArcrArena          bitmap_arena;
    size_t             cache_entries;
    size_t             cache_hits;
};

static const char *const kDefaultCjkFontCandidates[] = {
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
    NULL
};

static const char *const kDefaultCjkBoldCandidates[] = {
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/truetype/nanum/NanumGothicBold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf",
    NULL
};

static const char *const kDefaultMonoFontCandidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    NULL
};

#if defined(ARCR_HAVE_FREETYPE)
static const char *const kDefaultMonoBoldCandidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-B.ttf",
    NULL
};

static const char *const kDefaultSymbolFontCandidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
    NULL
};

static const char *const kDefaultEmojiFontCandidates[] = {
    "/usr/share/fonts/truetype/noto/NotoEmoji-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
    NULL
};
#endif

static inline uint64_t make_glyph_key(uint32_t cp, int32_t font_px, bool bold, bool monospace) {
    uint64_t px_u  = (uint64_t)(uint32_t)font_px & 0xFFFFULL;
    uint64_t flags = (bold ? 1ULL : 0ULL) | (monospace ? 2ULL : 0ULL);
    return (1ULL << 63) | ((uint64_t)cp << 18) | (px_u << 2) | flags;
}

static inline uint32_t hash_glyph_key_slot(uint64_t key) {
    uint64_t h = key * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 32;
    h ^= h >> 15;
    return (uint32_t)h & (uint32_t)ARCR_GLYPH_HASH_MASK;
}

static inline uint32_t slot_load_state_acquire(const ArcrGlyphCacheSlot *slot) {
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE);
#else
    return slot->state;
#endif
}

static inline void slot_publish_state_release(ArcrGlyphCacheSlot *slot) {
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(&slot->state, 1u, __ATOMIC_RELEASE);
#else
    slot->state = 1u;
#endif
}

#if !defined(ARCR_HAVE_FREETYPE)
static bool load_stb_font_candidates(ArcrStbFontFace *out_face, const char *preferred,
                                     const char *const *candidates) {
    memset(out_face, 0, sizeof(*out_face));
    const char *paths[12];
    size_t npaths = 0u;
    if (preferred && preferred[0] != '\0') paths[npaths++] = preferred;
    for (size_t i = 0u; candidates && candidates[i] && npaths < 12u; ++i) {
        paths[npaths++] = candidates[i];
    }
    for (size_t i = 0u; i < npaths; ++i) {
        FILE *fp = fopen(paths[i], "rb");
        if (!fp) continue;
        if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); continue; }
        long sz = ftell(fp);
        if (sz <= 64 || sz > 64L * 1024L * 1024L || fseek(fp, 0, SEEK_SET) != 0) {
            fclose(fp);
            continue;
        }
        uint8_t *buf = (uint8_t *)malloc((size_t)sz);
        if (!buf) { fclose(fp); continue; }
        if (fread(buf, 1u, (size_t)sz, fp) != (size_t)sz) {
            free(buf);
            fclose(fp);
            continue;
        }
        fclose(fp);

        int off = stbtt_GetFontOffsetForIndex(buf, 1);
        if (off < 0) off = stbtt_GetFontOffsetForIndex(buf, 0);
        if (off >= 0 && stbtt_InitFont(&out_face->info, buf, off) != 0) {
            out_face->ttf_bytes = buf;
            out_face->valid     = true;
            return true;
        }
        free(buf);
    }
    return false;
}
#endif

ArcrFontSystem *arcr_font_system_create(const char *preferred_cjk_font_path) {
    ArcrFontSystem *fs = (ArcrFontSystem *)calloc(1u, sizeof(ArcrFontSystem));
    if (!fs) return NULL;

    if (pthread_mutex_init(&fs->mutex_storage, NULL) == 0) {
        fs->mutex    = &fs->mutex_storage;
        fs->mutex_ok = true;
    }

    arcr_arena_init(&fs->bitmap_arena, ARCR_GLYPH_ARENA_BLOCK_SZ);

#if defined(ARCR_HAVE_FREETYPE)
    if (FT_Init_FreeType(&fs->ft_lib) == 0) {
        /* In NotoSansCJK-*.ttc, face index 1 is Noto Sans CJK KR */
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
        for (size_t i = 0u; !fs->face_symbols && kDefaultSymbolFontCandidates[i]; ++i) {
            (void)FT_New_Face(fs->ft_lib, kDefaultSymbolFontCandidates[i], 0, &fs->face_symbols);
        }
        for (size_t i = 0u; !fs->face_emoji && kDefaultEmojiFontCandidates[i]; ++i) {
            (void)FT_New_Face(fs->ft_lib, kDefaultEmojiFontCandidates[i], 0, &fs->face_emoji);
        }

        if (fs->face_regular || fs->face_mono || fs->face_symbols) {
            if (!fs->face_regular) {
                fs->face_regular = fs->face_symbols ? fs->face_symbols : fs->face_mono;
            }
            fs->has_ft = true;
        } else {
            FT_Done_FreeType(fs->ft_lib);
        }
    }
#else
    (void)load_stb_font_candidates(&fs->stb_reg, preferred_cjk_font_path, kDefaultCjkFontCandidates);
    (void)load_stb_font_candidates(&fs->stb_bold, NULL, kDefaultCjkBoldCandidates);
    (void)load_stb_font_candidates(&fs->stb_mono, NULL, kDefaultMonoFontCandidates);
#endif
    return fs;
}

void arcr_font_system_destroy(ArcrFontSystem *fs) {
    if (!fs) return;
#if defined(ARCR_HAVE_FREETYPE)
    if (fs->has_ft) {
        if (fs->face_emoji) FT_Done_Face(fs->face_emoji);
        if (fs->face_symbols && fs->face_symbols != fs->face_regular) FT_Done_Face(fs->face_symbols);
        if (fs->face_mono_bold) FT_Done_Face(fs->face_mono_bold);
        if (fs->face_mono && fs->face_mono != fs->face_regular) FT_Done_Face(fs->face_mono);
        if (fs->face_bold) FT_Done_Face(fs->face_bold);
        if (fs->face_regular) FT_Done_Face(fs->face_regular);
        FT_Done_FreeType(fs->ft_lib);
    }
#else
    free(fs->stb_reg.ttf_bytes);
    free(fs->stb_bold.ttf_bytes);
    free(fs->stb_mono.ttf_bytes);
#endif
    arcr_arena_destroy(&fs->bitmap_arena);
    if (fs->mutex_ok) {
        pthread_mutex_destroy(&fs->mutex_storage);
    }
    free(fs);
}

void arcr_font_system_purge_cache(ArcrFontSystem *fs) {
    if (!fs) return;
    if (fs->mutex_ok) pthread_mutex_lock(fs->mutex);
    memset(fs->slots, 0, sizeof(fs->slots));
    arcr_arena_reset(&fs->bitmap_arena);
    fs->cache_entries = 0u;
    fs->cache_hits    = 0u;
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

/*
 * Smooth 4x4 supersampled anti-aliased vector fallback glyph (when 0 fonts exist
 * or codepoint is unmapped) — eliminates jagged 1-bit pixelation completely.
 */
static int64_t dist_sq_to_seg_q4(int32_t px, int32_t py,
                                 int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    int64_t vx = (int64_t)bx - (int64_t)ax;
    int64_t vy = (int64_t)by - (int64_t)ay;
    int64_t wx = (int64_t)px - (int64_t)ax;
    int64_t wy = (int64_t)py - (int64_t)ay;
    int64_t len_sq = vx * vx + vy * vy;
    int64_t cx = ax, cy = ay;
    if (len_sq > 0) {
        int64_t dot = wx * vx + wy * vy;
        if (dot <= 0) {
            cx = ax; cy = ay;
        } else if (dot >= len_sq) {
            cx = bx; cy = by;
        } else {
            cx = (int64_t)ax + (vx * dot) / len_sq;
            cy = (int64_t)ay + (vy * dot) / len_sq;
        }
    }
    int64_t dx = (int64_t)px - cx;
    int64_t dy = (int64_t)py - cy;
    return dx * dx + dy * dy;
}

static void fallback_glyph(uint32_t cp, int32_t font_px, bool bold, bool monospace,
                           ArcrGlyphMetrics *out_m, uint8_t *out_alpha, size_t cap) {
    bool wide = arcr_cp_is_hangul(cp) || arcr_cp_is_cjk_ideograph(cp) || (cp > 0x2000u);
    int32_t adv = wide ? font_px : ARCR_MAX(6, (font_px * 3) / 5);
    int32_t gw  = ARCR_MAX(4, adv - 2);
    int32_t gh  = ARCR_MAX(6, (font_px * 3) / 4);
    if (arcr_cp_is_whitespace(cp)) {
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
    if (!out_alpha || (size_t)gw * (size_t)gh > cap) return;

    /* Coordinates in 1/16th pixel units (Q4) for 4x4 subpixel anti-aliasing */
    int32_t w_q4 = gw * 16;
    int32_t h_q4 = gh * 16;
    int32_t stroke_r = ARCR_MAX(10, (font_px * (bold ? 18 : 13)) / 16);
    int64_t r_sq = (int64_t)stroke_r * (int64_t)stroke_r;

    int32_t x0 = 14, y0 = 14;
    int32_t x1 = ARCR_MAX(x0 + 8, w_q4 - 14);
    int32_t y1 = ARCR_MAX(y0 + 8, h_q4 - 14);
    int32_t xm = (x0 + x1) / 2;
    int32_t ym = (y0 + y1) / 2;

    for (int32_t y = 0; y < gh; ++y) {
        for (int32_t x = 0; x < gw; ++x) {
            uint32_t cov = 0u;
            for (int32_t sy = 0; sy < 4; ++sy) {
                int32_t py = y * 16 + sy * 4 + 2;
                for (int32_t sx = 0; sx < 4; ++sx) {
                    int32_t px = x * 16 + sx * 4 + 2;
                    bool hit = false;
                    if (dist_sq_to_seg_q4(px, py, x0, y0, x1, y0) <= r_sq ||
                        dist_sq_to_seg_q4(px, py, x1, y0, x1, y1) <= r_sq ||
                        dist_sq_to_seg_q4(px, py, x1, y1, x0, y1) <= r_sq ||
                        dist_sq_to_seg_q4(px, py, x0, y1, x0, y0) <= r_sq) {
                        hit = true;
                    } else if ((cp & 1u) != 0u && dist_sq_to_seg_q4(px, py, x0, ym, x1, ym) <= r_sq) {
                        hit = true;
                    } else if ((cp & 2u) != 0u && dist_sq_to_seg_q4(px, py, xm, y0, xm, y1) <= r_sq) {
                        hit = true;
                    } else if ((cp & 4u) != 0u && dist_sq_to_seg_q4(px, py, x0, y1, x1, y0) <= r_sq) {
                        hit = true;
                    }
                    if (hit) ++cov;
                }
            }
            uint32_t raw_a = (cov * 230u + 8u) / 16u;
            out_alpha[(size_t)y * (size_t)gw + (size_t)x] = kPerceptualFontGammaLut[raw_a & 0xFFu];
        }
    }
}

#if defined(ARCR_HAVE_FREETYPE)
static void configure_ft_face_size(FT_Face face, int32_t font_px, int32_t *cached_px) {
    if (*cached_px == font_px) return;
    if (FT_IS_SCALABLE(face)) {
        (void)FT_Set_Pixel_Sizes(face, 0u, (FT_UInt)font_px);
    } else if (face->num_fixed_sizes > 0) {
        int best_idx = 0;
        int32_t best_diff = 0x7FFFFFFF;
        for (int i = 0; i < face->num_fixed_sizes; ++i) {
            int32_t sh = (int32_t)face->available_sizes[i].height;
            int32_t diff = (sh >= font_px) ? (sh - font_px) : (font_px - sh + 512);
            if (diff < best_diff) {
                best_diff = diff;
                best_idx  = i;
            }
        }
        (void)FT_Select_Size(face, best_idx);
    }
    *cached_px = font_px;
}

static FT_Face select_ft_face_locked(ArcrFontSystem *fs, uint32_t cp, int32_t font_px,
                                     bool bold, bool monospace, FT_UInt *out_gidx) {
    FT_Face  candidates[6];
    int32_t *px_slots[6];
    size_t   cand_count = 0u;

    if (monospace) {
        if (bold && fs->face_mono_bold) {
            candidates[cand_count] = fs->face_mono_bold;
            px_slots[cand_count++] = &fs->cur_mono_bold_px;
        }
        if (fs->face_mono) {
            candidates[cand_count] = fs->face_mono;
            px_slots[cand_count++] = &fs->cur_mono_px;
        }
    }

    if (bold && fs->face_bold) {
        candidates[cand_count] = fs->face_bold;
        px_slots[cand_count++] = &fs->cur_bold_px;
    }
    if (fs->face_regular) {
        candidates[cand_count] = fs->face_regular;
        px_slots[cand_count++] = &fs->cur_reg_px;
    }
    if (fs->face_symbols && fs->face_symbols != fs->face_regular) {
        candidates[cand_count] = fs->face_symbols;
        px_slots[cand_count++] = &fs->cur_symbols_px;
    }
    if (fs->face_emoji) {
        candidates[cand_count] = fs->face_emoji;
        px_slots[cand_count++] = &fs->cur_emoji_px;
    }

    for (size_t i = 0u; i < cand_count; ++i) {
        FT_Face face = candidates[i];
        if (!face) continue;
        FT_UInt gidx = FT_Get_Char_Index(face, (FT_ULong)cp);
        if (gidx != 0u) {
            configure_ft_face_size(face, font_px, px_slots[i]);
            *out_gidx = gidx;
            return face;
        }
    }
    *out_gidx = 0u;
    return NULL;
}
#endif

static bool rasterize_glyph_into_arena_locked(ArcrFontSystem *fs, uint32_t codepoint,
                                              int32_t font_px, bool bold, bool monospace,
                                              ArcrGlyphMetrics *out_m,
                                              const uint8_t **out_ptr) {
    memset(out_m, 0, sizeof(*out_m));
    *out_ptr = &kEmptyGlyphAlpha;

    uint8_t  stack_buf[64 * 64];
    uint8_t *heap_buf = NULL;
    uint8_t *work_buf = stack_buf;
    size_t   work_cap = sizeof(stack_buf);
    bool     rendered = false;

#if defined(ARCR_HAVE_FREETYPE)
    if (fs->has_ft) {
        FT_UInt gidx = 0u;
        FT_Face face = select_ft_face_locked(fs, codepoint, font_px, bold, monospace, &gidx);
        if (face && gidx != 0u) {
            FT_Int32 load_flags = FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT;
            if (FT_HAS_COLOR(face)) {
                load_flags |= FT_LOAD_COLOR;
            }
            if (FT_Load_Glyph(face, gidx, load_flags) == 0) {
                FT_GlyphSlot gs = face->glyph;
                int32_t src_w = (int32_t)gs->bitmap.width;
                int32_t src_h = (int32_t)gs->bitmap.rows;
                int     pitch = gs->bitmap.pitch;

                if (src_w <= 0 || src_h <= 0) {
                    int32_t adv = (int32_t)((gs->advance.x + 32) >> 6);
                    if (!FT_IS_SCALABLE(face) && face->size && face->size->metrics.y_ppem > 0) {
                        adv = (adv * font_px + (int32_t)face->size->metrics.y_ppem / 2) /
                              (int32_t)face->size->metrics.y_ppem;
                    }
                    out_m->advance_px = ARCR_MAX(1, adv);
                    rendered = true;
                } else if (gs->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
                    /* Color bitmap strike (e.g. NotoColorEmoji): scale down to font_px */
                    int32_t strike_ppem = (face->size && face->size->metrics.y_ppem > 0)
                                              ? (int32_t)face->size->metrics.y_ppem
                                              : src_h;
                    int32_t dst_w = ARCR_CLAMP((src_w * font_px + strike_ppem / 2) / strike_ppem,
                                               1, ARCR_GLYPH_MAX_DIM);
                    int32_t dst_h = ARCR_CLAMP((src_h * font_px + strike_ppem / 2) / strike_ppem,
                                               1, ARCR_GLYPH_MAX_DIM);
                    size_t area = (size_t)dst_w * (size_t)dst_h;
                    if (area > work_cap) {
                        heap_buf = (uint8_t *)malloc(area);
                        if (!heap_buf) return false;
                        work_buf = heap_buf;
                        work_cap = area;
                    }
                    for (int32_t dy = 0; dy < dst_h; ++dy) {
                        int32_t sy0 = (dy * src_h) / dst_h;
                        int32_t sy1 = ARCR_MAX(sy0 + 1, ((dy + 1) * src_h) / dst_h);
                        if (sy1 > src_h) sy1 = src_h;
                        for (int32_t dx = 0; dx < dst_w; ++dx) {
                            int32_t sx0 = (dx * src_w) / dst_w;
                            int32_t sx1 = ARCR_MAX(sx0 + 1, ((dx + 1) * src_w) / dst_w);
                            if (sx1 > src_w) sx1 = src_w;
                            uint32_t sum_a = 0u, cnt = 0u;
                            for (int32_t sy = sy0; sy < sy1; ++sy) {
                                const uint8_t *row = (pitch >= 0)
                                    ? (gs->bitmap.buffer + (size_t)sy * (size_t)pitch)
                                    : (gs->bitmap.buffer + (size_t)(src_h - 1 - sy) * (size_t)(-pitch));
                                for (int32_t sx = sx0; sx < sx1; ++sx) {
                                    sum_a += row[sx * 4 + 3];
                                    ++cnt;
                                }
                            }
                            uint8_t a = (uint8_t)(cnt > 0u ? (sum_a / cnt) : 0u);
                            work_buf[(size_t)dy * (size_t)dst_w + (size_t)dx] = kPerceptualFontGammaLut[a];
                        }
                    }
                    out_m->advance_px = ARCR_MAX(dst_w + 1, font_px);
                    out_m->bearing_x  = 0;
                    out_m->bearing_y  = (dst_h * 4) / 5;
                    out_m->width      = dst_w;
                    out_m->height     = dst_h;
                    rendered          = true;
                } else if (src_w <= ARCR_GLYPH_MAX_DIM && src_h <= ARCR_GLYPH_MAX_DIM) {
                    size_t area = (size_t)src_w * (size_t)src_h;
                    if (area > work_cap) {
                        heap_buf = (uint8_t *)malloc(area);
                        if (!heap_buf) return false;
                        work_buf = heap_buf;
                        work_cap = area;
                    }
                    if (gs->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
                        uint32_t ngrays = (gs->bitmap.num_grays > 1u) ? (uint32_t)(gs->bitmap.num_grays - 1u) : 255u;
                        for (int32_t r = 0; r < src_h; ++r) {
                            const uint8_t *srow = (pitch >= 0)
                                ? (gs->bitmap.buffer + (size_t)r * (size_t)pitch)
                                : (gs->bitmap.buffer + (size_t)(src_h - 1 - r) * (size_t)(-pitch));
                            uint8_t *drow = work_buf + (size_t)r * (size_t)src_w;
                            for (int32_t c = 0; c < src_w; ++c) {
                                uint32_t v = srow[c];
                                if (ngrays != 255u) v = (v * 255u + ngrays / 2u) / ngrays;
                                drow[c] = kPerceptualFontGammaLut[v & 0xFFu];
                            }
                        }
                        rendered = true;
                    } else if (gs->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                        for (int32_t r = 0; r < src_h; ++r) {
                            const uint8_t *srow = (pitch >= 0)
                                ? (gs->bitmap.buffer + (size_t)r * (size_t)pitch)
                                : (gs->bitmap.buffer + (size_t)(src_h - 1 - r) * (size_t)(-pitch));
                            uint8_t *drow = work_buf + (size_t)r * (size_t)src_w;
                            for (int32_t c = 0; c < src_w; ++c) {
                                bool bit = (srow[c >> 3] & (0x80u >> (c & 7))) != 0u;
                                drow[c] = bit ? 255u : 0u;
                            }
                        }
                        rendered = true;
                    }
                    if (rendered) {
                        out_m->advance_px = (int32_t)((gs->advance.x + 32) >> 6);
                        out_m->bearing_x  = gs->bitmap_left;
                        out_m->bearing_y  = gs->bitmap_top;
                        out_m->width      = src_w;
                        out_m->height     = src_h;
                        /* Synthetic slight bold advance when using regular face for bold */
                        if (bold && !monospace && face == fs->face_regular && !fs->face_bold) {
                            out_m->advance_px += 1;
                        }
                    }
                }
            }
        }
    }
#else
    /* Secondary TrueType rasterizer via embedded stb_truetype when built without FreeType */
    const ArcrStbFontFace *sfaces[3] = {NULL, NULL, NULL};
    size_t sf_cnt = 0u;
    if (monospace && fs->stb_mono.valid) sfaces[sf_cnt++] = &fs->stb_mono;
    if (bold && fs->stb_bold.valid)      sfaces[sf_cnt++] = &fs->stb_bold;
    if (fs->stb_reg.valid)               sfaces[sf_cnt++] = &fs->stb_reg;

    for (size_t i = 0u; i < sf_cnt; ++i) {
        const stbtt_fontinfo *fi = &sfaces[i]->info;
        int gidx = stbtt_FindGlyphIndex(fi, (int)codepoint);
        if (gidx != 0) {
            float scale = stbtt_ScaleForPixelHeight(fi, (float)font_px);
            int adv_u = 0, lsb_u = 0;
            stbtt_GetGlyphHMetrics(fi, gidx, &adv_u, &lsb_u);
            int ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
            stbtt_GetGlyphBitmapBox(fi, gidx, scale, scale, &ix0, &iy0, &ix1, &iy1);
            int32_t gw = (int32_t)(ix1 - ix0);
            int32_t gh = (int32_t)(iy1 - iy0);
            out_m->advance_px = (int32_t)(adv_u * scale + 0.5f);
            if (gw <= 0 || gh <= 0) {
                rendered = true;
            } else if (gw <= ARCR_GLYPH_MAX_DIM && gh <= ARCR_GLYPH_MAX_DIM) {
                size_t area = (size_t)gw * (size_t)gh;
                if (area > work_cap) {
                    heap_buf = (uint8_t *)malloc(area);
                    if (!heap_buf) return false;
                    work_buf = heap_buf;
                    work_cap = area;
                }
                stbtt_MakeGlyphBitmap(fi, work_buf, gw, gh, gw, scale, scale, gidx);
                for (size_t p = 0u; p < area; ++p) {
                    work_buf[p] = kPerceptualFontGammaLut[work_buf[p]];
                }
                out_m->bearing_x = (int32_t)ix0;
                out_m->bearing_y = (int32_t)(-iy0);
                out_m->width     = gw;
                out_m->height    = gh;
                rendered         = true;
            }
            break;
        }
    }
#endif

    if (!rendered) {
        int32_t est_dim = ARCR_MIN(font_px + 4, ARCR_GLYPH_MAX_DIM);
        size_t  need    = (size_t)est_dim * (size_t)est_dim;
        if (need > work_cap) {
            heap_buf = (uint8_t *)malloc(need);
            if (!heap_buf) return false;
            work_buf = heap_buf;
            work_cap = need;
        }
        fallback_glyph(codepoint, font_px, bold, monospace, out_m, work_buf, work_cap);
    }

    size_t final_area = (size_t)out_m->width * (size_t)out_m->height;
    if (final_area > 0u) {
        uint8_t *dst = (uint8_t *)arcr_arena_alloc(&fs->bitmap_arena, final_area, 1u);
        if (!dst) {
            free(heap_buf);
            return false;
        }
        memcpy(dst, work_buf, final_area);
        *out_ptr = dst;
    } else {
        *out_ptr = &kEmptyGlyphAlpha;
    }

    free(heap_buf);
    return true;
}

const uint8_t *arcr_font_get_cached_glyph_ptr(ArcrFontSystem *fs, uint32_t codepoint,
                                              int32_t font_px, bool bold, bool monospace,
                                              ArcrGlyphMetrics *out_metrics,
                                              bool allow_insert) {
    if (!fs) return NULL;
    font_px = ARCR_CLAMP(font_px, 8, ARCR_GLYPH_MAX_FONT_PX);

    uint64_t key  = make_glyph_key(codepoint, font_px, bold, monospace);
    uint32_t mask = (uint32_t)ARCR_GLYPH_HASH_MASK;
    uint32_t base = hash_glyph_key_slot(key);

    /*
     * Lock-free zero-copy fast path for parallel 64x64 tile worker threads:
     * ZERO mutex locks, ZERO shared state writes, ZERO memcpy!
     */
    if (!allow_insert) {
        for (uint32_t step = 0u; step < (uint32_t)ARCR_GLYPH_HASH_CAP; ++step) {
            const ArcrGlyphCacheSlot *slot = &fs->slots[(base + step) & mask];
            if (slot_load_state_acquire(slot) == 0u) {
                return NULL;
            }
            if (slot->key == key) {
                if (out_metrics) *out_metrics = slot->metrics;
                return slot->bitmap_ptr;
            }
        }
        return NULL;
    }

    if (fs->mutex_ok) pthread_mutex_lock(fs->mutex);

    if (fs->cache_entries >= ((size_t)ARCR_GLYPH_HASH_CAP * 3u) / 4u) {
        memset(fs->slots, 0, sizeof(fs->slots));
        fs->cache_entries = 0u;
        if (fs->bitmap_arena.total_used > 16u * 1024u * 1024u) {
            arcr_arena_reset(&fs->bitmap_arena);
        }
    }

    ArcrGlyphCacheSlot *target_slot = NULL;
    for (uint32_t step = 0u; step < (uint32_t)ARCR_GLYPH_HASH_CAP; ++step) {
        ArcrGlyphCacheSlot *slot = &fs->slots[(base + step) & mask];
        if (slot->state != 0u && slot->key == key) {
            fs->cache_hits++;
            if (out_metrics) *out_metrics = slot->metrics;
            const uint8_t *ptr = slot->bitmap_ptr;
            if (fs->mutex_ok) pthread_mutex_unlock(fs->mutex);
            return ptr;
        }
        if (slot->state == 0u) {
            target_slot = slot;
            break;
        }
    }

    if (!target_slot) {
        if (fs->mutex_ok) pthread_mutex_unlock(fs->mutex);
        return NULL;
    }

    ArcrGlyphMetrics m;
    const uint8_t *arena_ptr = NULL;
    if (!rasterize_glyph_into_arena_locked(fs, codepoint, font_px, bold, monospace, &m, &arena_ptr)) {
        if (fs->mutex_ok) pthread_mutex_unlock(fs->mutex);
        return NULL;
    }

    target_slot->key        = key;
    target_slot->metrics    = m;
    target_slot->bitmap_ptr = arena_ptr;
    slot_publish_state_release(target_slot);
    fs->cache_entries++;

    if (out_metrics) *out_metrics = m;
    if (fs->mutex_ok) pthread_mutex_unlock(fs->mutex);
    return arena_ptr;
}

bool arcr_font_measure_glyph_ex(ArcrFontSystem *fs, uint32_t codepoint,
                                int32_t font_px, bool bold, bool monospace,
                                ArcrGlyphMetrics *out_metrics) {
    if (!out_metrics) return false;
    font_px = ARCR_CLAMP(font_px, 8, ARCR_GLYPH_MAX_FONT_PX);

    if (!fs) {
        fallback_glyph(codepoint, font_px, bold, monospace, out_metrics, NULL, 0u);
        return true;
    }

    const uint8_t *ptr = arcr_font_get_cached_glyph_ptr(
        fs, codepoint, font_px, bold, monospace, out_metrics, true
    );
    if (ptr) return true;

    fallback_glyph(codepoint, font_px, bold, monospace, out_metrics, NULL, 0u);
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
    font_px = ARCR_CLAMP(font_px, 8, ARCR_GLYPH_MAX_FONT_PX);

    if (!fs) {
        fallback_glyph(codepoint, font_px, bold, monospace, out_metrics, out_alpha, alpha_cap);
        return ((size_t)out_metrics->width * (size_t)out_metrics->height <= alpha_cap);
    }

    ArcrGlyphMetrics m;
    const uint8_t *ptr = arcr_font_get_cached_glyph_ptr(
        fs, codepoint, font_px, bold, monospace, &m, true
    );
    if (!ptr) return false;

    size_t area = (size_t)m.width * (size_t)m.height;
    if (area > alpha_cap) return false;
    if (area > 0u) {
        memcpy(out_alpha, ptr, area);
    }
    *out_metrics = m;
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
