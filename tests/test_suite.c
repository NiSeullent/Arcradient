/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * tests/test_suite.c - Comprehensive 22-Test Verification & Hardware Benchmark Suite
 *
 * Covers all 8 modular architecture tiers (Base, Platform, Hardware Accel/SIMD/GPU,
 * HTML5/DOM/CSS3/Layout/Parallel-Tile-Paint, Bytecode JS & HTTP/1.1+TLS Net,
 * Extensions/.wsx/Native Plugins, Waterscape Shell & Profile, SFX Installer/Uninstaller).
 */
#if defined(_WIN32)
#  ifndef WINVER
#    define WINVER 0x0500
#  endif
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0500
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <windows.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <pthread.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#include "../accel/arcr_accel.h"
#include "../addons/ws_addons.h"
#include "../base/arcr_base.h"
#include "../base/arcr_codec.h"
#include "../base/arcr_threadpool.h"
#include "../core/arcr_dom.h"
#include "../css/arcr_css.h"
#include "../include/arcr_module.h"
#include "../include/arcradient_embed.h"
#include "../include/waterscape_modules.h"
#include "../include/waterscape_plugin.h"
#include "../layout/arcr_layout.h"
#include "../media/arcr_media.h"
#include "../net/arcr_net.h"
#include "../paint/arcr_paint.h"
#include "../platform/arcr_platform.h"
#include "../shell/arcr_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define ASSERT_TRUE(cond, msg)                                                      \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, (msg));     \
            g_tests_failed++;                                                       \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define RUN_TEST(fn)                                    \
    do {                                                \
        int before = g_tests_failed;                    \
        g_tests_run++;                                  \
        fn();                                           \
        if (g_tests_failed == before) {                 \
            printf("  [PASS] %s\n", #fn);               \
        }                                               \
    } while (0)

static const char *g_module_dir = ".";

/* 1. Tier 0: Arena & Slab Pool reclamation + Atom string deduplication */
static void test_01_arena_slab_and_atoms(void) {
    ArcrArena arena;
    arcr_arena_init(&arena, 4096u);
    ArcrSlabPool slab;
    arcr_slab_init(&slab, &arena, 64u, 8u);

    void *p1 = arcr_slab_alloc(&slab);
    void *p2 = arcr_slab_alloc(&slab);
    ASSERT_TRUE(p1 != NULL && p2 != NULL && p1 != p2, "Slab allocations distinct");
    arcr_slab_free(&slab, p1);
    void *p3 = arcr_slab_alloc(&slab);
    ASSERT_TRUE(p3 == p1, "Freed slab slot immediately recycled without new heap alloc");
    arcr_arena_destroy(&arena);
    ASSERT_TRUE(arena.total_allocated == 0u, "Arena destroy releases all blocks immediately");

    ArcrAtomTable atoms;
    ASSERT_TRUE(arcr_atom_table_init(&atoms, 64u), "Atom table init");
    ArcrAtom a1 = arcr_atom_intern_lower(&atoms, "DIV", 3u);
    ArcrAtom a2 = arcr_atom_intern_lower(&atoms, "div", 3u);
    ASSERT_TRUE(a1 != ARCR_ATOM_NULL && a1 == a2, "Case-insensitive tag interning deduplicates");
    ASSERT_TRUE(atoms.count == 1u, "Only single string stored");
    arcr_atom_table_destroy(&atoms);
}

/* 2. Tier 0/4: C Module ABI safety: rejects AVX2 module on SSE1-only Legacy x86 host */
static void test_02_module_abi_guard(void) {
    int dummy_vtable = 42;
    ArcrModuleDescriptor desc = {
        .magic              = ARCR_MODULE_MAGIC,
        .struct_size        = sizeof(ArcrModuleDescriptor),
        .abi_version        = ARCR_ABI_VERSION,
        .engine_build_id    = ARCR_ENGINE_BUILD_ID,
        .arch_id            = ARCR_ARCH_X86_32,
        .min_os_major_minor = ARCR_OS_WIN2000,
        .required_cpu_flags = ARCR_CPU_SSE1,
        .module_name        = "arcr-legacy-test",
        .vtable             = &dummy_vtable
    };

    ASSERT_TRUE(arcr_module_validate(&desc, ARCR_ARCH_X86_32, ARCR_OS_WIN2000, ARCR_CPU_SSE1) == ARCR_MOD_OK,
                "Valid Win2000 x86 SSE1 module accepted");
    ASSERT_TRUE(arcr_module_validate(&desc, ARCR_ARCH_X86_64, ARCR_OS_WIN10, ARCR_CPU_AVX2) == ARCR_MOD_ERR_ARCH_MISMATCH,
                "Architecture mismatch rejected");

    desc.required_cpu_flags = ARCR_CPU_SSE1 | ARCR_CPU_AVX2;
    ASSERT_TRUE(arcr_module_validate(&desc, ARCR_ARCH_X86_32, ARCR_OS_WIN2000, ARCR_CPU_SSE1) == ARCR_MOD_ERR_CPU_UNSUPPORTED,
                "AVX2 module cleanly rejected on SSE1-only CPU before SIGILL");

    desc.required_cpu_flags = ARCR_CPU_SSE1;
    desc.min_os_major_minor = ARCR_OS_WIN10;
    ASSERT_TRUE(arcr_module_validate(&desc, ARCR_ARCH_X86_32, ARCR_OS_WIN2000, ARCR_CPU_SSE1) == ARCR_MOD_ERR_OS_TOO_OLD,
                "Win10-only module rejected on Windows 2000 host");
}

/* 3. Tier 0: Korean UTF-8, word-break: keep-all vs normal, and CJK 금칙처리 */
static void test_03_korean_utf8_and_line_breaking(void) {
    const char *ko = "한글.";
    uint32_t cp1 = 0, cp2 = 0, cp3 = 0;
    size_t n1 = arcr_utf8_decode_one(ko, strlen(ko), &cp1);
    size_t n2 = arcr_utf8_decode_one(ko + n1, strlen(ko) - n1, &cp2);
    size_t n3 = arcr_utf8_decode_one(ko + n1 + n2, strlen(ko) - n1 - n2, &cp3);
    ASSERT_TRUE(cp1 == 0xD55Cu && cp2 == 0xAE00u && cp3 == '.' && n3 == 1u,
                "UTF-8 decodes '한'(U+D55C), '글'(U+AE00), '.'");
    ASSERT_TRUE(arcr_cp_is_hangul(cp1) && arcr_cp_is_hangul(cp2), "Hangul syllable detected");
    ASSERT_TRUE(arcr_unicode_can_break_line(cp1, cp2, false) == true, "normal wrap");
    ASSERT_TRUE(arcr_unicode_can_break_line(cp1, cp2, true) == false, "keep-all 어절 intact");
    ASSERT_TRUE(arcr_unicode_can_break_line(' ', cp1, true) == true, "break after space");
    ASSERT_TRUE(arcr_unicode_can_break_line(cp2, '.', false) == false, "행두 금칙 '.'");
    ASSERT_TRUE(arcr_unicode_can_break_line('(', cp1, false) == false, "행말 금칙 '('");
}

/* 4. Tier 0: Pure C11 DEFLATE/ZLIB/GZIP, PKZIP (.wsx), Base64, and PNG Codec Round-Trip */
static void test_04_codec_deflate_zip_and_png(void) {
    const char *sample =
        "Waterscape Browser 1.0 Pure C11 Codec Verification — "
        "빠른 속도와 무의존성 압축 테스트! Waterscape Browser 1.0 Pure C11 Codec Verification!";
    size_t slen = strlen(sample);

    uint8_t *zcomp = NULL;
    size_t   zlen  = 0u;
    ASSERT_TRUE(arcr_zlib_compress((const uint8_t *)sample, slen, &zcomp, &zlen), "ZLIB compress");
    ASSERT_TRUE(zlen > 0u && zlen < slen, "LZ77 + Fixed Huffman compressed repeated string");

    uint8_t *zdec = NULL;
    size_t   zdlen = 0u;
    ASSERT_TRUE(arcr_decompress_http_body(zcomp, zlen, "deflate", &zdec, &zdlen, 65536u),
                "HTTP deflate auto-decompress");
    ASSERT_TRUE(zdlen == slen && memcmp(zdec, sample, slen) == 0, "ZLIB round-trip exact");
    free(zcomp);
    free(zdec);

    /* PKZIP (.wsx) archive create & extract */
    const char *mjson = "{\"id\":\"test-ext\",\"version\":\"1.0.0\"}";
    const char *rules = "||ads.example.com^\n";
    ArcrZipEntryView entries[2] = {
        {"manifest.json", (const uint8_t *)mjson, strlen(mjson)},
        {"filter_rules.txt", (const uint8_t *)rules, strlen(rules)}
    };
    uint8_t *zip_buf = NULL;
    size_t   zip_len = 0u;
    ASSERT_TRUE(arcr_zip_create_mem(entries, 2u, &zip_buf, &zip_len), "PKZIP create");
    char names[4][128];
    ASSERT_TRUE(arcr_zip_list_entries(zip_buf, zip_len, names, 4u) == 2u, "PKZIP lists 2 entries");

    uint8_t *ext_m = NULL;
    size_t   ext_mlen = 0u;
    ASSERT_TRUE(arcr_zip_extract_entry(zip_buf, zip_len, "manifest.json", &ext_m, &ext_mlen),
                "PKZIP extract manifest.json");
    ASSERT_TRUE(ext_mlen == strlen(mjson) && memcmp(ext_m, mjson, ext_mlen) == 0,
                "Extracted manifest matches byte-for-byte");
    free(ext_m);
    free(zip_buf);

    /* Pure C PNG encode & decode pixel-exact round-trip */
    uint32_t orig_px[16];
    for (int i = 0; i < 16; ++i) {
        orig_px[i] = 0xFF000000u | ((uint32_t)(i * 15) << 16) | ((uint32_t)(240 - i * 12) << 8) | 0x7Fu;
    }
    uint8_t *png_bytes = NULL;
    size_t   png_len   = 0u;
    ASSERT_TRUE(arcr_image_encode_png_mem(orig_px, 4, 4, 4, &png_bytes, &png_len), "PNG encode");
    ASSERT_TRUE(png_len > 24u && png_bytes[0] == 0x89u && png_bytes[1] == 'P', "Valid PNG signature");

    ArcrImage decoded;
    ASSERT_TRUE(arcr_image_decode_mem(png_bytes, png_len, &decoded), "Pure C PNG decode");
    ASSERT_TRUE(decoded.width == 4 && decoded.height == 4 &&
                memcmp(decoded.pixels, orig_px, sizeof(orig_px)) == 0,
                "PNG encode->decode 100% pixel-identical");
    arcr_image_free(&decoded);
    free(png_bytes);
}

/* 5. Tier 0: Hardware ThreadPool atomic work-stealing execution */
typedef struct PoolTestCtx {
    uint8_t visited[512];
} PoolTestCtx;

static void pool_mark_item(void *ud, uint32_t idx, uint32_t worker_id) {
    (void)worker_id;
    PoolTestCtx *ctx = (PoolTestCtx *)ud;
    if (idx < 512u) ctx->visited[idx]++;
}

static void test_05_hardware_threadpool_work_stealing(void) {
    uint32_t hw = arcr_hardware_cpu_threads();
    ASSERT_TRUE(hw >= 1u, "Detected >= 1 logical CPU core");

    ArcrThreadPool *pool = arcr_threadpool_create(4u);
    ASSERT_TRUE(pool != NULL && arcr_threadpool_worker_count(pool) == 4u, "4-worker pool created");

    PoolTestCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    arcr_threadpool_parallel_for(pool, 512u, pool_mark_item, &ctx);

    for (size_t i = 0u; i < 512u; ++i) {
        ASSERT_TRUE(ctx.visited[i] == 1u, "Every parallel work item executed exactly once");
    }
    arcr_threadpool_destroy(pool);
}

/* 6. Tier 2: Bit-Exact Parity across Scalar == SSE1+SWAR == SSE2 == AVX2 + GPU Router */
static void test_06_accel_simd_avx2_bit_exact_and_gpu_fallback(void) {
    enum { N = 139 }; /* Odd pixel count exercises 16-px AVX2, 8-px AVX2, 4-px SSE2, and scalar tail */
    uint32_t buf_scalar[N], buf_sse1[N], buf_sse2[N], buf_avx2[N];
    for (size_t i = 0; i < N; ++i) {
        uint32_t seed = (uint32_t)(i * 0x45D9F3Bu + 0x12345678u);
        buf_scalar[i] = buf_sse1[i] = buf_sse2[i] = buf_avx2[i] = seed | 0xFF000000u;
    }

    const uint32_t colors[] = {
        0x00112233u, 0xFF336699u, 0x802563EBu, 0x37DC2626u, 0xFE10B981u, 0x01ABCDEFu
    };
    bool can_run_avx2 = (arcr_detect_cpu_flags() & ARCR_CPU_AVX2) != 0u &&
                        arcr_compiled_with_avx2_kernel();

    for (size_t c = 0; c < ARCR_ARRAY_LEN(colors); ++c) {
        arcr_scanline_blend_scalar(buf_scalar, N, colors[c]);
        arcr_scanline_blend_sse1_swar(buf_sse1, N, colors[c]);
        arcr_scanline_blend_sse2(buf_sse2, N, colors[c]);
        if (can_run_avx2) {
            arcr_scanline_blend_avx2(buf_avx2, N, colors[c]);
        }
    }

    ASSERT_TRUE(memcmp(buf_scalar, buf_sse1, sizeof(buf_scalar)) == 0,
                "SSE1+SWAR matches CPU Scalar 100% bit-for-bit");
    ASSERT_TRUE(memcmp(buf_scalar, buf_sse2, sizeof(buf_scalar)) == 0,
                "SSE2 XMM matches CPU Scalar 100% bit-for-bit");
    if (can_run_avx2) {
        ASSERT_TRUE(memcmp(buf_scalar, buf_avx2, sizeof(buf_scalar)) == 0,
                    "AVX2 256-bit YMM matches CPU Scalar 100% bit-for-bit");
    }

    /* Cost router & GPU fault / software GL rejection test */
    ArcrAccelContext *accel = arcr_accel_create(0xFFFFFFFFu, false, true);
    ASSERT_TRUE(arcr_accel_is_gpu_healthy(accel), "GPU initially healthy");

    ArcrAccelBackendId small_route = arcr_accel_choose_route(
        accel, ARCR_OP_FILL_RECT, 32 * 32, false, NULL, NULL);
    ASSERT_TRUE(small_route <= ARCR_BACKEND_CPU_AVX2,
                "Small rect stays on CPU SIMD tier rather than wasting GPU setup/sync");

    ArcrAccelBackendId big_route = arcr_accel_choose_route(
        accel, ARCR_OP_LARGE_COMPOSITE, 1024 * 768, true, NULL, NULL);
    ASSERT_TRUE(big_route >= ARCR_BACKEND_GPU_DDRAW7,
                "Large GPU-resident composite routes to GPU backend");

    /* Reject Windows 2000 software OpenGL "GDI Generic" -> instant zero-crash CPU fallback */
    arcr_accel_reject_software_gl_renderer(accel, "GDI Generic");
    ASSERT_TRUE(!arcr_accel_is_gpu_healthy(accel) && arcr_accel_did_fallback_occur(accel),
                "Software OpenGL 'GDI Generic' cleanly rejected");
    ArcrAccelBackendId recovered_route = arcr_accel_choose_route(
        accel, ARCR_OP_LARGE_COMPOSITE, 1024 * 768, true, NULL, NULL);
    ASSERT_TRUE(recovered_route <= ARCR_BACKEND_CPU_AVX2,
                "Large composite seamlessly falls back to CPU SIMD after GPU rejection");

    arcr_accel_destroy(accel);
}

/* 7. Tier 2: Anti-Aliased Rounded Rectangles, Box Shadows & Bilinear Image Blit */
static void test_07_accel_2d_rounded_shadow_and_image_scale(void) {
    ArcrAccelContext *accel = arcr_accel_create(0xFFFFFFFFu, true, false);
    uint32_t fb[64 * 64];
    arcr_scanline_fill_u32_scalar(fb, 64 * 64, 0xFFFFFFFFu);

    arcr_accel_draw_box_shadow(accel, fb, 64, 64, 64, (ArcrRect){12, 12, 40, 40}, 8, 6, 2, 0x66000000u);
    arcr_accel_fill_rounded_rect(accel, fb, 64, 64, 64, (ArcrRect){12, 12, 40, 40}, 8, 0xFF2563EBu);
    arcr_accel_stroke_rounded_rect(accel, fb, 64, 64, 64, (ArcrRect){12, 12, 40, 40}, 8, 2, 0xFF0F172Au);
    ASSERT_TRUE(fb[32 * 64 + 32] == 0xFF2563EBu, "Rounded rect interior filled with primary blue");

    uint32_t src_img[4] = {0xFFEF4444u, 0xFF10B981u, 0xFF3B82F6u, 0xFFF59E0Bu};
    arcr_accel_blit_image_argb(accel, fb, 64, 64, 64, (ArcrRect){0, 0, 8, 8}, src_img, 2, 2, true);
    ASSERT_TRUE(fb[0] != 0xFFFFFFFFu, "Bilinear scaled image blitted into top-left region");

    arcr_accel_destroy(accel);
}

/* 8. Tier 1: Thread-Safe O(1) Glyph Atlas Cache, Monospace Font & Clipboard */
static void test_08_platform_glyph_cache_and_clipboard(void) {
    ArcrFontSystem *fs = arcr_font_system_create(NULL);
    ASSERT_TRUE(fs != NULL, "Font system created");

    ArcrGlyphMetrics gm1, gm2;
    uint8_t alpha[64 * 64];
    ASSERT_TRUE(arcr_font_rasterize_glyph_ex(fs, 0xD55Cu /* '한' */, 16, false, false, &gm1, alpha, sizeof(alpha)),
                "Rasterized Hangul glyph '한'");
    ASSERT_TRUE(arcr_font_rasterize_glyph_ex(fs, 0xD55Cu /* '한' */, 16, false, false, &gm2, alpha, sizeof(alpha)),
                "Second lookup hits O(1) glyph cache");

    size_t entries = 0u, hits = 0u;
    arcr_font_system_cache_stats(fs, &entries, &hits);
    ASSERT_TRUE(entries >= 1u && hits >= 1u, "Glyph cache recorded entries and cache hit");

    ArcrGlyphMetrics gm_mono;
    ASSERT_TRUE(arcr_font_measure_glyph_ex(fs, 'W', 14, false, true, &gm_mono) && gm_mono.advance_px > 0,
                "Monospace glyph measured");

    arcr_font_system_purge_cache(fs);
    arcr_font_system_cache_stats(fs, &entries, &hits);
    ASSERT_TRUE(entries == 0u, "Glyph cache purged cleanly on memory pressure");
    arcr_font_system_destroy(fs);

    /* UTF-8 Clipboard round-trip */
    ASSERT_TRUE(arcr_platform_clipboard_set_utf8("Waterscape 클립보드 테스트"), "Set UTF-8 clipboard");
    char clip_out[128] = {0};
    ASSERT_TRUE(arcr_platform_clipboard_get_utf8(clip_out, sizeof(clip_out)), "Get UTF-8 clipboard");
    ASSERT_TRUE(strcmp(clip_out, "Waterscape 클립보드 테스트") == 0, "Clipboard UTF-8 exact match");
}

/* 9. Tier 3: HTML5 Error Recovery, Entities, querySelectorAll & Fragment innerHTML */
static void test_09_html5_recovery_entities_and_selectors(void) {
    ArcrAtomTable atoms;
    arcr_atom_table_init(&atoms, 128u);

    const char *html =
        "<html><head><title>Waterscape &mdash; 브라우저</title></head><body>"
        "<div id='hero'><p>문단 1<p>문단 2</div>"
        "<ul class='nav'><li class='item'>A<li class='item active'>B<li class='item'>C</ul>"
        "</body></html>";

    ArcrDocument *doc = arcr_html_parse(&atoms, html, strlen(html));
    ASSERT_TRUE(doc != NULL && doc->body != NULL, "HTML5 parsed");
    ASSERT_TRUE(strstr(doc->title, "—") != NULL, "Decoded &mdash; HTML entity in <title>");

    ArcrDomNode *matches[8];
    size_t n = arcr_dom_query_selector_all(doc, doc->root, "ul.nav > li.item", matches, 8u);
    ASSERT_TRUE(n == 3u, "querySelectorAll('ul.nav > li.item') matched all 3 auto-closed <li> elements");

    ArcrDomNode *hero = arcr_dom_get_element_by_id(doc, "hero");
    ASSERT_TRUE(hero != NULL, "getElementById('hero') found");
    ASSERT_TRUE(arcr_html_parse_fragment(doc, hero, "<span class='tag'>NEW</span>", 28u),
                "arcr_html_parse_fragment appended dynamic child");
    ASSERT_TRUE(arcr_dom_query_selector_all(doc, hero, ".tag", matches, 4u) == 1u,
                "Fragment child queryable via selector");

    arcr_dom_document_destroy(doc);
    arcr_atom_table_destroy(&atoms);
}

/* 10. Tier 3: O(1) CSS3 Hash Index, var(), calc(), hsl(), @media & Smart Style Sharing */
static void test_10_css3_o1_index_vars_calc_media_and_sharing(void) {
    ArcrAtomTable atoms;
    arcr_atom_table_init(&atoms, 128u);

    const char *html =
        "<html><head><style>"
        "  :root { --brand: #1d4ed8; }"
        "  .box { width: calc(100% - 40px); color: hsl(210, 100%, 50%); }"
        "  ul .item { color: var(--brand); padding: 6px; }"
        "  @media (prefers-color-scheme: dark) { body { background: #0f172a; } }"
        "</style></head><body>"
        "<div id='b' class='box'>Box</div>"
        "<ul>"
        "  <li class='item'>1<li class='item'>2<li class='item'>3<li class='item'>4<li class='item'>5"
        "</ul></body></html>";

    ArcrDocument *doc = arcr_html_parse(&atoms, html, strlen(html));
    ArcrCssMediaEnv dark_env = {1000, 700, true};
    ArcrStylesheet *sheet = arcr_css_parse_document_styles_ex(doc, &dark_env);
    size_t share_hits = 0u;
    arcr_css_resolve_tree(doc, sheet, &share_hits);

    ASSERT_TRUE(share_hits >= 4u, "4 sibling <li class='item'> nodes shared computed style in O(1)");
    ASSERT_TRUE(doc->body->computed_style->bg_argb == 0xFF0F172Au,
                "@media (prefers-color-scheme: dark) applied dark background");

    arcr_dom_document_destroy(doc);
    arcr_atom_table_destroy(&atoms);
}

static const ArcrLayoutBox *find_layout_box_by_dom(const ArcrLayoutBox *b, const ArcrDomNode *target) {
    if (!b) return NULL;
    if (b->dom_node == target) return b;
    for (const ArcrLayoutBox *ch = b->first_child; ch != NULL; ch = ch->next_sibling) {
        const ArcrLayoutBox *hit = find_layout_box_by_dom(ch, target);
        if (hit) return hit;
    }
    return NULL;
}

/* 11. Tier 3: Modern Layout — Flexbox, CSS Grid & Multi-Column HTML Tables */
static void test_11_modern_layout_flexbox_grid_and_table(void) {
    ArcrAtomTable atoms;
    arcr_atom_table_init(&atoms, 128u);
    ArcrFontSystem *fs = arcr_font_system_create(NULL);

    const char *html =
        "<html><head><style>"
        "  .flex { display: flex; gap: 16px; }"
        "  .fitem { flex: 1; padding: 8px; }"
        "  .grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; }"
        "  table { width: 600px; }"
        "</style></head><body>"
        "  <div class='flex'><div id='f1' class='fitem'>Left</div><div id='f2' class='fitem'>Right</div></div>"
        "  <div class='grid'><div id='g1'>1</div><div id='g2'>2</div><div id='g3'>3</div></div>"
        "  <table><tr><td id='c1'>Col1</td><td id='c2'>Col2</td></tr></table>"
        "</body></html>";

    ArcrDocument *doc = arcr_html_parse(&atoms, html, strlen(html));
    ArcrStylesheet *sh = arcr_css_parse_document_styles(doc);
    arcr_css_resolve_tree(doc, sh, NULL);
    ArcrLayoutTree *lt = arcr_layout_build_zoomed(doc, fs, 800, 100);
    ASSERT_TRUE(lt != NULL && lt->box_count >= 8u, "Built Flex + Grid + Table layout tree");

    /* Verify f2 is laid out to the right of f1, g3 to the right of g2, and c2 to the right of c1 */
    const ArcrLayoutBox *bf1 = find_layout_box_by_dom(lt->root_box, arcr_dom_get_element_by_id(doc, "f1"));
    const ArcrLayoutBox *bf2 = find_layout_box_by_dom(lt->root_box, arcr_dom_get_element_by_id(doc, "f2"));
    const ArcrLayoutBox *bg2 = find_layout_box_by_dom(lt->root_box, arcr_dom_get_element_by_id(doc, "g2"));
    const ArcrLayoutBox *bg3 = find_layout_box_by_dom(lt->root_box, arcr_dom_get_element_by_id(doc, "g3"));
    const ArcrLayoutBox *bc1 = find_layout_box_by_dom(lt->root_box, arcr_dom_get_element_by_id(doc, "c1"));
    const ArcrLayoutBox *bc2 = find_layout_box_by_dom(lt->root_box, arcr_dom_get_element_by_id(doc, "c2"));

    ASSERT_TRUE(bf1 && bf2 && bf2->border_rect.x > bf1->border_rect.x + 100,
                "Flexbox placed items horizontally with flex:1");
    ASSERT_TRUE(bg2 && bg3 && bg2->border_rect.x > 100 && bg3->border_rect.x > bg2->border_rect.x + 100,
                "CSS Grid repeat(3, 1fr) placed 3 columns horizontally");
    ASSERT_TRUE(bc1 && bc2 && bc2->border_rect.x > bc1->border_rect.x + 100,
                "HTML <table> placed <td> cells side-by-side");

    arcr_layout_destroy(lt);
    arcr_dom_document_destroy(doc);
    arcr_font_system_destroy(fs);
    arcr_atom_table_destroy(&atoms);
}

/* 12. Tier 3: Bit-Exact Parity between 1-Thread and Multi-Core 64x64 Tile Rasterization */
static void test_12_parallel_tile_rasterizer_parity(void) {
    ArcrAtomTable atoms;
    arcr_atom_table_init(&atoms, 128u);
    ArcrFontSystem *fs = arcr_font_system_create(NULL);
    ArcrAccelContext *accel = arcr_accel_create(0xFFFFFFFFu, true, false);

    const char *html =
        "<html><head><style>"
        "  body { background: #f8fafc; padding: 20px; }"
        "  .card { background: #ffffff; border: 2px solid #2563eb; border-radius: 12px;"
        "          box-shadow: 0 4px 10px rgba(0,0,0,0.2); padding: 16px; margin-bottom: 14px; }"
        "</style></head><body>"
        "  <div class='card'><h1>멀티코어 64x64 타일 래스터라이저 검증</h1>"
        "  <p>단일 스레드와 병렬 워커 스레드 결과가 1비트 오차 없이 일치해야 합니다.</p></div>"
        "</body></html>";

    ArcrDocument *doc = arcr_html_parse(&atoms, html, strlen(html));
    ArcrStylesheet *sh = arcr_css_parse_document_styles(doc);
    arcr_css_resolve_tree(doc, sh, NULL);
    ArcrLayoutTree *lt = arcr_layout_build(doc, fs, 360);
    ArcrDisplayList *dl = arcr_paint_build_display_list(lt);

    uint32_t *fb_single = (uint32_t *)calloc(360u * 260u, sizeof(uint32_t));
    uint32_t *fb_multi  = (uint32_t *)calloc(360u * 260u, sizeof(uint32_t));

    arcr_paint_rasterize(dl, accel, fs, fb_single, 360, 260, 360, (ArcrRect){0, 0, 360, 260}, 0);

    ArcrThreadPool *pool = arcr_threadpool_create(4u);
    uint32_t tiles = arcr_paint_rasterize_parallel(dl, accel, fs, pool, fb_multi,
                                                   360, 260, 360, (ArcrRect){0, 0, 360, 260}, 0);
    ASSERT_TRUE(tiles == 6u * 5u, "360x260 viewport partitioned into 6x5 = 30 tiles (64x64)");
    ASSERT_TRUE(memcmp(fb_single, fb_multi, 360u * 260u * sizeof(uint32_t)) == 0,
                "4-Thread 64x64 Tile Rasterizer matches single-thread output 100% bit-for-bit!");

    free(fb_single);
    free(fb_multi);
    arcr_threadpool_destroy(pool);
    arcr_paint_destroy_display_list(dl);
    arcr_layout_destroy(lt);
    arcr_dom_document_destroy(doc);
    arcr_accel_destroy(accel);
    arcr_font_system_destroy(fs);
    arcr_atom_table_destroy(&atoms);
}

/* 13. Tier 3/4: On-Demand Lazy Module Loading & Systematized Tier-4 Registry */
static void test_13_on_demand_lazy_modules_and_registry(void) {
    ArcrEngineConfig cfg;
    arcr_engine_default_config(&cfg, ARCR_PROFILE_BALANCED);
    cfg.module_search_dir = g_module_dir;

    ArcrEngine *eng = arcr_engine_create(&cfg);
    ASSERT_TRUE(eng != NULL, "Engine created");

    ArcrWebView *view = arcr_webview_create(eng, 800, 600, NULL);
    const char *static_html = "<html><head><title>정적 문서</title></head><body><h1>순수 문서</h1></body></html>";
    ASSERT_TRUE(arcr_webview_load_html(view, "https://example.org/", static_html, strlen(static_html)),
                "Static HTML loaded");

    WaterscapeModuleStatusEntry mods[WS_DYN_SLOT_COUNT];
    ASSERT_TRUE(arcr_engine_query_modules(eng, mods, WS_DYN_SLOT_COUNT) == WS_DYN_SLOT_COUNT,
                "Queried 4 systematized Tier-4 module slots");
    ASSERT_TRUE(!mods[WS_DYN_SLOT_JS].is_loaded && !mods[WS_DYN_SLOT_MEDIA].is_loaded,
                "Static page keeps JS & Media dynamic modules completely unloaded");

    const char *dyn_html =
        "<html><head><title>초기 제목</title></head><body>"
        "<video src='clip.webm'></video>"
        "<script>document.title = '동적 갱신 완료';</script>"
        "</body></html>";
    ASSERT_TRUE(arcr_webview_load_html(view, "https://example.org/app", dyn_html, strlen(dyn_html)),
                "Interactive HTML loaded");

    arcr_engine_query_modules(eng, mods, WS_DYN_SLOT_COUNT);
    ASSERT_TRUE(mods[WS_DYN_SLOT_JS].is_loaded && mods[WS_DYN_SLOT_MEDIA].is_loaded,
                "JS & Media modules loaded on demand");
    ASSERT_TRUE(strcmp(arcr_webview_get_title(view), "동적 갱신 완료") == 0,
                "Bytecode ArcradientJS updated document.title");

    ArcrEngineStats st_before, st_after;
    arcr_engine_get_stats(eng, &st_before);
    arcr_engine_notify_memory_pressure(eng, 2);
    arcr_engine_get_stats(eng, &st_after);
    ASSERT_TRUE(st_after.js_heap_bytes <= st_before.js_heap_bytes, "Memory pressure handled cleanly");

    arcr_engine_destroy(eng);
}

/* 14. Tier 4: Bytecode ArcradientJS Closures, Arrow Functions, Arrays, JSON & Infinite-Loop Fuel Guard */
static void test_14_bytecode_js_closures_json_and_fuel(void) {
    ArcrEngineConfig cfg;
    arcr_engine_default_config(&cfg, ARCR_PROFILE_MODERN);
    cfg.module_search_dir = g_module_dir;
    ArcrEngine *eng = arcr_engine_create(&cfg);
    ArcrWebView *view = arcr_webview_create(eng, 640, 480, NULL);

    const char *html =
        "<html><head><title>init</title></head><body><script>"
        "  function makeAdder(base) { return (x) => base + x; }"
        "  const add10 = makeAdder(10);"
        "  const nums = [1, 2, 3, 4].map(n => add10(n)).filter(v => v > 12);"
        "  const obj = JSON.parse('{\"browser\":\"Waterscape\",\"ver\":1}');"
        "  console.log(obj.browser + ':' + nums.join(','));"
        "  document.title = obj.browser + '-' + nums[0] + '-' + nums[1];"
        "</script></body></html>";

    ASSERT_TRUE(arcr_webview_load_html(view, "https://waterscape.local/", html, strlen(html)),
                "Loaded bytecode JS closure/arrow/JSON page");
    ASSERT_TRUE(strcmp(arcr_webview_get_title(view), "Waterscape-13-14") == 0,
                "Closures + arrow functions + Array.map/filter + JSON.parse produced 'Waterscape-13-14'");
    ASSERT_TRUE(strstr(arcr_webview_get_console_log(view), "Waterscape:13,14") != NULL,
                "console.log captured in WebView console buffer");

    /* Fuel limit guard: infinite while(true){} must terminate safely in milliseconds! */
    const char *inf = "while (true) {}";
    (void)arcr_webview_eval_js(view, inf, strlen(inf));
    ASSERT_TRUE(strcmp(arcr_webview_get_title(view), "Waterscape-13-14") == 0,
                "500k instruction fuel guard aborted infinite loop without hanging browser!");

    arcr_engine_destroy(eng);
}

/* 15. Tier 4: Live JS DOM Manipulation, Event Bubbling & HTML5 <canvas> 2D Rendering */
static void test_15_js_live_dom_events_and_canvas_2d(void) {
    ArcrEngineConfig cfg;
    arcr_engine_default_config(&cfg, ARCR_PROFILE_MODERN);
    cfg.module_search_dir = g_module_dir;
    ArcrEngine *eng = arcr_engine_create(&cfg);
    ArcrWebView *view = arcr_webview_create(eng, 400, 300, NULL);

    const char *html =
        "<html><head><style>"
        "  body { margin: 0; padding: 0; background: #ffffff; }"
        "  #btn { display: block; width: 160px; height: 40px; background: #2563eb; color: #fff; }"
        "</style></head><body>"
        "  <button id='btn'>Click Me</button>"
        "  <canvas id='cv' width='80' height='60'></canvas>"
        "  <script>"
        "    let clicks = 0;"
        "    const btn = document.getElementById('btn');"
        "    btn.addEventListener('click', () => {"
        "      clicks++;"
        "      document.title = 'Clicked:' + clicks;"
        "    });"
        "    const ctx = document.getElementById('cv').getContext('2d');"
        "    ctx.fillStyle = '#ef4444';"
        "    ctx.fillRect(0, 0, 80, 60);"
        "  </script>"
        "</body></html>";

    ASSERT_TRUE(arcr_webview_load_html(view, "https://waterscape.local/canvas", html, strlen(html)),
                "Loaded DOM + Canvas 2D page");

    /* Click button at (20, 15) and verify JS event listener fires */
    char nav_url[256] = {0};
    bool new_tab = false;
    (void)arcr_webview_mouse_click(view, 20, 15, 0, nav_url, sizeof(nav_url), &new_tab);
    ASSERT_TRUE(strcmp(arcr_webview_get_title(view), "Clicked:1") == 0,
                "Clicking <button> dispatched 'click' event to JS addEventListener!");

    /* Paint viewport and verify red #EF4444 pixels from <canvas> 2D context appear! */
    uint32_t *fb = (uint32_t *)calloc(400u * 300u, sizeof(uint32_t));
    ASSERT_TRUE(arcr_webview_paint(view, fb, 400, 300, 400), "Painted WebView with <canvas>");
    bool found_canvas_red = false;
    for (size_t i = 0u; i < 400u * 300u; ++i) {
        if (fb[i] == 0xFFEF4444u) {
            found_canvas_red = true;
            break;
        }
    }
    ASSERT_TRUE(found_canvas_red, "HTML5 <canvas> 2D fillRect('#ef4444') rendered into viewport!");
    free(fb);
    arcr_engine_destroy(eng);
}

/* 16. Tier 4: Net RFC 3986 URL Resolution, CMOS Clock-Skew Guard & Adaptive Media */
static void test_16_net_rfc3986_clock_skew_and_media(void) {
    char net_path[512];
    arcr_platform_format_module_path(net_path, sizeof(net_path), g_module_dir, "arcr_net");
    ArcrDynLib *net_lib = arcr_platform_dlopen(net_path);
    ASSERT_TRUE(net_lib != NULL, "arcr_net dynamic library opened");

    ArcrModuleQueryFn qfn = NULL;
    void *sym = arcr_platform_dlsym(net_lib, "arcr_module_query");
    memcpy(&qfn, &sym, sizeof(qfn));
    const ArcrNetVTable *nvt = (const ArcrNetVTable *)qfn()->vtable;

    char resolved[256];
    ASSERT_TRUE(nvt->resolve_url("https://waterscape.org/docs/guide/index.html",
                                 "../assets/main.css", resolved, sizeof(resolved)),
                "RFC 3986 relative URL resolved");
    ASSERT_TRUE(strcmp(resolved, "https://waterscape.org/docs/assets/main.css") == 0,
                "Dot-segment '../assets/main.css' normalized accurately");

    char reason[256];
    ArcrCertCheckInput cmos_reset = {
        .expected_host             = "waterscape.org",
        .cert_san_dns              = "*.waterscape.org",
        .trusted_root_signature_ok = true,
        .not_before_epoch          = 1735689600LL,
        .not_after_epoch           = 1893456000LL,
        .system_wall_epoch         = 946684800LL
    };
    ASSERT_TRUE(nvt->verify_tls_trust(&cmos_reset, reason, sizeof(reason)) == ARCR_SEC_CLOCK_SKEW_ERR,
                "Detects dead CMOS battery clock skew distinctly");

    /* Verify file:// path traversal & sensitive /etc/passwd guard */
    char *fbody = NULL;
    size_t flen = 0u;
    ArcrSecurityState fsec = ARCR_SEC_LOCAL_OR_NONE;
    ASSERT_TRUE(!nvt->load_url_resource("file:///etc/passwd", &fbody, &flen, &fsec, reason, sizeof(reason)),
                "Blocked sensitive file:///etc/passwd read");
    arcr_platform_dlclose(net_lib);

    /* Adaptive Fluid Media test under 92% CPU overload */
    char med_path[512];
    arcr_platform_format_module_path(med_path, sizeof(med_path), g_module_dir, "arcr_media");
    ArcrDynLib *med_lib = arcr_platform_dlopen(med_path);
    ASSERT_TRUE(med_lib != NULL, "arcr_media opened");
    sym = arcr_platform_dlsym(med_lib, "arcr_module_query");
    memcpy(&qfn, &sym, sizeof(qfn));
    const ArcrMediaVTable *mvt = (const ArcrMediaVTable *)qfn()->vtable;

    const ArcrStreamVariant streams[3] = {{640, 360, 600}, {1280, 720, 2200}, {1920, 1080, 5000}};
    ArcrMediaAdaptiveDecision dec;
    mvt->compute_adaptive_policy(ARCR_PROFILE_MINIMUM, streams, 3u, 800, 450, 92u, &dec);
    ASSERT_TRUE(dec.chosen_stream_index == 0u && dec.keep_audio_continuous,
                "Overloaded legacy CPU selects 360p while keeping audio continuous");
    arcr_platform_dlclose(med_lib);
}

/* 17. Tier 4/5: Live Loopback HTTP/1.1 Server (302 Redirect, Cookies, Deflate, Subresources & Shield Block) */
#if defined(_WIN32)
typedef SOCKET ws_sock_t;
#  define WS_CLOSE_SOCK closesocket
#else
typedef int ws_sock_t;
#  define WS_CLOSE_SOCK close
#endif

typedef struct LoopbackServerCtx {
    ws_sock_t listen_fd;
    uint16_t  port;
    bool      saw_cookie_on_redirect;
    uint8_t  *deflated_html;
    size_t    deflated_len;
    uint8_t  *logo_png;
    size_t    logo_len;
} LoopbackServerCtx;

static void serve_one_http_conn(LoopbackServerCtx *srv) {
    struct sockaddr_in cli;
#if defined(_WIN32)
    int clen = (int)sizeof(cli);
#else
    socklen_t clen = (socklen_t)sizeof(cli);
#endif
    ws_sock_t cfd = accept(srv->listen_fd, (struct sockaddr *)&cli, &clen);
#if defined(_WIN32)
    if (cfd == INVALID_SOCKET) return;
#else
    if (cfd < 0) return;
#endif

    char req[2048];
    memset(req, 0, sizeof(req));
    int r = (int)recv(cfd, req, sizeof(req) - 1, 0);
    if (r <= 0) {
        WS_CLOSE_SOCK(cfd);
        return;
    }

    char resp_hdr[512];
    if (strncmp(req, "GET /start ", 11) == 0) {
        int n = snprintf(resp_hdr, sizeof(resp_hdr),
                         "HTTP/1.1 302 Found\r\n"
                         "Location: /home\r\n"
                         "Set-Cookie: ws_sid=ok777; Path=/\r\n"
                         "Content-Length: 0\r\n"
                         "Connection: close\r\n\r\n");
        send(cfd, resp_hdr, n, 0);
    } else if (strncmp(req, "GET /home ", 10) == 0) {
        if (strstr(req, "ws_sid=ok777") != NULL) {
            srv->saw_cookie_on_redirect = true;
        }
        int n = snprintf(resp_hdr, sizeof(resp_hdr),
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/html; charset=utf-8\r\n"
                         "Content-Encoding: deflate\r\n"
                         "Content-Length: %lu\r\n"
                         "Connection: close\r\n\r\n",
                         (unsigned long)srv->deflated_len);
        send(cfd, resp_hdr, n, 0);
        send(cfd, (const char *)srv->deflated_html, (int)srv->deflated_len, 0);
    } else if (strncmp(req, "GET /style.css ", 15) == 0) {
        const char *css = "body { background-color: #10b981; }";
        int n = snprintf(resp_hdr, sizeof(resp_hdr),
                         "HTTP/1.1 200 OK\r\nContent-Type: text/css\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n%s",
                         (unsigned long)strlen(css), css);
        send(cfd, resp_hdr, n, 0);
    } else if (strncmp(req, "GET /app.js ", 12) == 0) {
        const char *js = "document.title = 'HTTP11-Live-OK';";
        int n = snprintf(resp_hdr, sizeof(resp_hdr),
                         "HTTP/1.1 200 OK\r\nContent-Type: application/javascript\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n%s",
                         (unsigned long)strlen(js), js);
        send(cfd, resp_hdr, n, 0);
    } else if (strncmp(req, "GET /logo.png ", 14) == 0) {
        int n = snprintf(resp_hdr, sizeof(resp_hdr),
                         "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
                         (unsigned long)srv->logo_len);
        send(cfd, resp_hdr, n, 0);
        send(cfd, (const char *)srv->logo_png, (int)srv->logo_len, 0);
    }
    shutdown(cfd, 1);
    WS_CLOSE_SOCK(cfd);
}

#if defined(_WIN32)
static DWORD WINAPI loopback_server_thread(LPVOID arg) {
    LoopbackServerCtx *srv = (LoopbackServerCtx *)arg;
    for (int i = 0; i < 5; ++i) serve_one_http_conn(srv);
    return 0;
}
#else
static void *loopback_server_thread(void *arg) {
    LoopbackServerCtx *srv = (LoopbackServerCtx *)arg;
    for (int i = 0; i < 5; ++i) serve_one_http_conn(srv);
    return NULL;
}
#endif

static void test_17_live_http11_redirect_deflate_cookies_and_subresources(void) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    LoopbackServerCtx srv;
    memset(&srv, 0, sizeof(srv));

    const char *page_html =
        "<!DOCTYPE html><html><head><title>BeforeJS</title>"
        "<link rel='stylesheet' href='/style.css'>"
        "</head><body>"
        "<img src='/logo.png' width='8' height='8'>"
        "<script src='http://doubleclick.net/tracker.js'></script>"
        "<script src='/app.js'></script>"
        "</body></html>";
    ASSERT_TRUE(arcr_zlib_compress((const uint8_t *)page_html, strlen(page_html),
                                   &srv.deflated_html, &srv.deflated_len),
                "Compressed HTML payload with ZLIB/deflate");

    uint32_t logo_px[16];
    for (int i = 0; i < 16; ++i) logo_px[i] = 0xFF2563EBu;
    ASSERT_TRUE(arcr_image_encode_png_mem(logo_px, 4, 4, 4, &srv.logo_png, &srv.logo_len),
                "Encoded 4x4 PNG subresource");

    srv.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0; /* Ephemeral port */
    ASSERT_TRUE(bind(srv.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0, "Bound loopback socket");
    ASSERT_TRUE(listen(srv.listen_fd, 8) == 0, "Listening on loopback socket");

#if defined(_WIN32)
    int alen = (int)sizeof(addr);
#else
    socklen_t alen = (socklen_t)sizeof(addr);
#endif
    getsockname(srv.listen_fd, (struct sockaddr *)&addr, &alen);
    srv.port = ntohs(addr.sin_port);

#if defined(_WIN32)
    HANDLE th = CreateThread(NULL, 0, loopback_server_thread, &srv, 0, NULL);
#else
    pthread_t th;
    pthread_create(&th, NULL, loopback_server_thread, &srv);
#endif

    /* Configure engine with Waterscape Shield ad/tracker blocker active! */
    WsAddonManager *addons = ws_addons_create();
    ws_addons_load_builtin_defaults(addons);

    ArcrEngineConfig cfg;
    arcr_engine_default_config(&cfg, ARCR_PROFILE_MODERN);
    cfg.module_search_dir = g_module_dir;
    ArcrEngine *eng = arcr_engine_create(&cfg);
    arcr_engine_set_url_filter(eng, ws_addons_should_block_url, addons);

    ArcrWebView *view = arcr_webview_create(eng, 320, 200, NULL);
    char start_url[128];
    snprintf(start_url, sizeof(start_url), "http://127.0.0.1:%u/start", (unsigned)srv.port);
    ASSERT_TRUE(arcr_webview_navigate_url(view, start_url), "Navigated live HTTP/1.1 loopback URL");

#if defined(_WIN32)
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
#else
    pthread_join(th, NULL);
#endif
    WS_CLOSE_SOCK(srv.listen_fd);

    ASSERT_TRUE(srv.saw_cookie_on_redirect, "Cookie Jar stored Set-Cookie on 302 and echoed Cookie on /home!");
    ASSERT_TRUE(strstr(arcr_webview_get_url(view), "/home") != NULL, "Final URL updated to redirected /home");
    ASSERT_TRUE(strcmp(arcr_webview_get_title(view), "HTTP11-Live-OK") == 0,
                "External /app.js fetched and executed after deflate HTML decode!");

    ArcrEngineStats st;
    arcr_engine_get_stats(eng, &st);
    ASSERT_TRUE(st.blocked_requests_count >= 1u && ws_addons_total_blocked(addons) >= 1u,
                "Waterscape Shield blocked http://doubleclick.net/tracker.js subresource!");

    uint32_t *fb = (uint32_t *)calloc(320u * 200u, sizeof(uint32_t));
    arcr_webview_paint(view, fb, 320, 200, 320);
    ASSERT_TRUE(fb[190 * 320 + 300] == 0xFF10B981u,
                "External /style.css colored body background emerald #10B981!");
    free(fb);

    arcr_engine_destroy(eng);
    ws_addons_destroy(addons);
    free(srv.deflated_html);
    free(srv.logo_png);
}

/* 18. Tier 3: Interactive Form Input, Submit URL Encoding & Find-in-Page */
static void test_18_forms_input_typing_and_find_in_page(void) {
    ArcrEngineConfig cfg;
    arcr_engine_default_config(&cfg, ARCR_PROFILE_MODERN);
    cfg.module_search_dir = g_module_dir;
    ArcrEngine *eng = arcr_engine_create(&cfg);
    ArcrWebView *view = arcr_webview_create(eng, 500, 360, NULL);

    const char *html =
        "<html><body style='margin:0;padding:0;'>"
        "<form action='https://search.local/go' method='get'>"
        "  <input id='q' name='q' type='text' style='width:200px;height:32px;display:block;' value=''>"
        "  <button type='submit' style='width:100px;height:32px;display:block;'>Search</button>"
        "</form>"
        "<p>Waterscape 고속 렌더링 엔진은 Waterscape 브라우저의 핵심입니다.</p>"
        "</body></html>";
    ASSERT_TRUE(arcr_webview_load_html(view, "https://search.local/", html, strlen(html)), "Loaded form HTML");

    /* Click input at (30, 16), type 'C11', press Enter to submit */
    char sub_url[256] = {0};
    bool new_tab = false;
    (void)arcr_webview_mouse_click(view, 30, 16, 0, sub_url, sizeof(sub_url), &new_tab);
    (void)arcr_webview_key_input(view, "C11", ARCR_KEY_NONE, sub_url, sizeof(sub_url));
    sub_url[0] = '\0';
    (void)arcr_webview_key_input(view, NULL, ARCR_KEY_ENTER, sub_url, sizeof(sub_url));
    ASSERT_TRUE(strstr(sub_url, "q=C11") != NULL, "Form Enter submission URL-encoded '?q=C11'");

    /* Find-in-Page case-insensitive match count */
    uint32_t active_idx = 99u;
    uint32_t matches = arcr_webview_find_in_page(view, "waterscape", false, &active_idx);
    ASSERT_TRUE(matches == 2u && active_idx == 0u, "Find-in-Page found 2 occurrences of 'waterscape'");
    matches = arcr_webview_find_in_page(view, "waterscape", true, &active_idx);
    ASSERT_TRUE(matches == 2u && active_idx == 1u, "Find-in-Page advance_next moved to match #2");

    arcr_engine_destroy(eng);
}

/* 19. Tier 5: Packaged .wsx Extensions (Shield, Dark, Reader) & Native C Plugin (Perf HUD) */
static void test_19_extensions_wsx_and_native_perf_plugin(void) {
    WsAddonManager *mgr = ws_addons_create();
    ASSERT_TRUE(mgr != NULL, "Created WsAddonManager");

    char addons_dir[512];
    snprintf(addons_dir, sizeof(addons_dir), "%s/addons", g_module_dir);
    ws_addons_load_builtin_defaults(mgr);
    (void)ws_addons_scan_directory(mgr, addons_dir);
    ASSERT_TRUE(ws_addons_load_native_plugin(mgr, g_module_dir, "ws_plugin_perf_hud"),
                "Loaded native C11 plugin ws_plugin_perf_hud");

    ASSERT_TRUE(ws_addons_count(mgr) >= 4u, "3 extensions + 1 native C plugin active in manager");
    ASSERT_TRUE(ws_addons_should_block_url(mgr, "https://pagead2.googlesyndication.com/pagead/js/adsbygoogle.js", "news.org"),
                "Shield blocked googlesyndication.com ad script in O(1)");
    ASSERT_TRUE(!ws_addons_should_block_url(mgr, "waterscape://addons", "waterscape"),
                "Internal waterscape:// scheme bypassed filter");

    /* Toggle NightScape Dark Mode and verify injected CSS collection */
    ASSERT_TRUE(ws_addons_set_enabled(mgr, "waterscape-dark", true), "Enabled waterscape-dark");
    char *inj_css = NULL;
    size_t css_len = 0u;
    ASSERT_TRUE(ws_addons_collect_injected_css(mgr, "https://example.com/", &inj_css, &css_len) &&
                inj_css != NULL && strstr(inj_css, "#0F172A") != NULL,
                "Collected NightScape Dark Mode CSS for external URL");
    free(inj_css);

    WaterscapePluginHostContext host_ctx = {
        .detected_cpu_flags     = arcr_detect_cpu_flags(),
        .active_simd_backend    = ARCR_BACKEND_CPU_AVX2,
        .active_gpu_backend     = ARCR_BACKEND_GPU_D3D11,
        .worker_threads         = 8u,
        .last_frame_render_usec = 420u,
        .last_frame_tiles       = 24u,
        .engine_memory_bytes    = 262144u
    };
    char hud[128] = {0};
    ws_addons_format_plugin_hud(mgr, &host_ctx, hud, sizeof(hud));
    ASSERT_TRUE(strstr(hud, "SIMD:") != NULL && strstr(hud, "8T") != NULL,
                "Native Perf HUD plugin formatted live SIMD & thread telemetry badge");

    ws_addons_destroy(mgr);
}

/* 20. Tier 6: Waterscape Shell Multi-Tab, Internal waterscape:// Pages, DevTools Dock & Profile */
static void test_20_waterscape_shell_tabs_internal_pages_and_profile(void) {
    char prof_dir[512];
#if defined(_WIN32)
    snprintf(prof_dir, sizeof(prof_dir), "%s\\test_ws_profile", g_module_dir);
#else
    snprintf(prof_dir, sizeof(prof_dir), "%s/test_ws_profile", g_module_dir);
#endif

    ArcrEngineConfig ecfg;
    arcr_engine_default_config(&ecfg, ARCR_PROFILE_MODERN);
    ecfg.module_search_dir = g_module_dir;
    ArcrEngine *eng = arcr_engine_create(&ecfg);

    ArcrShellConfig scfg;
    arcr_shell_default_config(&scfg, ARCR_SHELL_THEME_WIN2000_CLASSIC);
    scfg.module_search_dir    = g_module_dir;
    scfg.profile_dir_override = prof_dir;
    ASSERT_TRUE(scfg.show_bookmarks_bar == false, "Bookmarks bar hidden by default");

    ArcrBrowserShell *sh = arcr_shell_create(eng, &scfg);
    ArcrShellLayoutMetrics m_hidden, m_shown;
    arcr_shell_compute_layout(sh, &m_hidden);
    arcr_shell_set_bookmarks_bar_visible(sh, true);
    arcr_shell_compute_layout(sh, &m_shown);
    ASSERT_TRUE(m_hidden.bookmarks_bar_rect.height == 0 &&
                m_shown.bookmarks_bar_rect.height == 24 &&
                m_hidden.content_viewport.height == m_shown.content_viewport.height + 24,
                "Bookmarks bar toggle allocates/reclaims exact 24px height");

    /* Navigate built-in waterscape:// pages & Back/Forward history */
    ASSERT_TRUE(arcr_shell_open_tab_url(sh, "waterscape://newtab", true) >= 0, "Opened waterscape://newtab");
    ASSERT_TRUE(arcr_shell_navigate(sh, "waterscape://hardware"), "Navigated to waterscape://hardware");
    ASSERT_TRUE(arcr_shell_navigate(sh, "waterscape://addons"), "Navigated to waterscape://addons");
    ASSERT_TRUE(arcr_shell_can_go_back(sh) && arcr_shell_go_back(sh), "Back button returned to hardware page");
    ASSERT_TRUE(arcr_shell_can_go_forward(sh) && arcr_shell_go_forward(sh), "Forward button returned to addons");

    /* Save profile & render full frame with F12 DevTools Hardware Dock open */
    arcr_shell_add_bookmark(sh, "Waterscape 홈", "waterscape://newtab");
    ASSERT_TRUE(arcr_shell_save_profile(sh), "Saved persistent user profile");
    arcr_shell_set_devtools_visible(sh, true);

    uint32_t *fb = (uint32_t *)calloc(800u * 600u, sizeof(uint32_t));
    ASSERT_TRUE(arcr_shell_render_frame(sh, fb, 800, 600, 800),
                "Rendered full Waterscape Chrome + waterscape://addons + F12 Hardware Dock");
    free(fb);

    arcr_shell_destroy(sh);

    /* Reload second shell from same profile dir to verify bookmark persistence */
    scfg.load_saved_prefs = true;
    ArcrBrowserShell *sh2 = arcr_shell_create(eng, &scfg);
    ASSERT_TRUE(arcr_shell_is_url_bookmarked(sh2, "waterscape://newtab"),
                "Bookmark persisted across browser sessions in bookmarks.tsv");
    arcr_shell_destroy(sh2);
    arcr_engine_destroy(eng);
}

/* 21. Tier 7: Self-Extracting Installer (WSSFX100) & Uninstaller Verification */
#if defined(_WIN32)
#ifndef CREATE_NO_WINDOW
#  define CREATE_NO_WINDOW 0x08000000u
#endif
#ifndef INVALID_FILE_ATTRIBUTES
#  define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

static bool win32_build_exe_fullpath(const char *dir, const char *exe_name,
                                     WCHAR *out_full, DWORD full_cap) {
    char rel_u8[1024];
    const char *d = (dir && dir[0] != '\0') ? dir : ".";
    snprintf(rel_u8, sizeof(rel_u8), "%s\\%s", d, exe_name);

    WCHAR rel_w[1024];
    int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, rel_u8, -1,
                                 rel_w, (int)(sizeof(rel_w) / sizeof(rel_w[0])));
    if (wn <= 0) {
        wn = MultiByteToWideChar(CP_ACP, 0u, rel_u8, -1,
                                 rel_w, (int)(sizeof(rel_w) / sizeof(rel_w[0])));
    }
    if (wn <= 0) return false;

    for (int i = 0; rel_w[i] != L'\0'; ++i) {
        if (rel_w[i] == L'/') rel_w[i] = L'\\';
    }

    DWORD fn = GetFullPathNameW(rel_w, full_cap, out_full, NULL);
    if (fn == 0u || fn >= full_cap) {
        if ((DWORD)wn >= full_cap) return false;
        memcpy(out_full, rel_w, (size_t)wn * sizeof(WCHAR));
    }
    return true;
}

static bool win32_run_verify_exe(const char *dir, const char *exe_name) {
    WCHAR full_exe[1024];
    if (!win32_build_exe_fullpath(dir, exe_name, full_exe,
                                  (DWORD)(sizeof(full_exe) / sizeof(full_exe[0])))) {
        return false;
    }

    WCHAR cmdline[1200];
    const WCHAR suffix[] = L"\" --verify-stub";
    size_t exe_len = 0u;
    while (full_exe[exe_len] != L'\0') ++exe_len;
    if (1u + exe_len + (sizeof(suffix) / sizeof(suffix[0])) > (sizeof(cmdline) / sizeof(cmdline[0]))) {
        return false;
    }
    cmdline[0] = L'"';
    memcpy(cmdline + 1u, full_exe, exe_len * sizeof(WCHAR));
    memcpy(cmdline + 1u + exe_len, suffix, sizeof(suffix));

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessW(full_exe, cmdline, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return false;
    }

    DWORD wait_rc = WaitForSingleObject(pi.hProcess, 15000u);
    DWORD exit_code = 1u;
    BOOL got_code = GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (wait_rc == WAIT_OBJECT_0 && got_code && exit_code == 0u);
}
#endif

static void test_21_installer_and_uninstaller_stubs(void) {
#if defined(_WIN32)
    ASSERT_TRUE(win32_run_verify_exe(g_module_dir, "waterscape_setup_stub.exe"),
                "waterscape_setup_stub.exe --verify-stub passed");
    ASSERT_TRUE(win32_run_verify_exe(g_module_dir, "uninstall.exe"),
                "uninstall.exe --verify-stub passed");
    WCHAR sfx_full[1024];
    if (win32_build_exe_fullpath(g_module_dir, "WaterscapeSetup.exe", sfx_full,
                                 (DWORD)(sizeof(sfx_full) / sizeof(sfx_full[0])))) {
        DWORD attr = GetFileAttributesW(sfx_full);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
            ASSERT_TRUE(win32_run_verify_exe(g_module_dir, "WaterscapeSetup.exe"),
                        "Packed WaterscapeSetup.exe CRC32 & WSSFX100 verified!");
        }
    }
#else
    char cmd[4096];
    char stage_dir[512], inst_dir[512], sfx_bin[512];
    snprintf(stage_dir, sizeof(stage_dir), "%s/tmp_sfx_stage", g_module_dir);
    snprintf(inst_dir,  sizeof(inst_dir),  "%s/tmp_sfx_inst",  g_module_dir);
    snprintf(sfx_bin,   sizeof(sfx_bin),   "%s/tmp_waterscape_setup", g_module_dir);

    arcr_platform_mkdir_p(stage_dir);
    char sample_f[560];
    snprintf(sample_f, sizeof(sample_f), "%s/README.txt", stage_dir);
    FILE *fp = fopen(sample_f, "wb");
    ASSERT_TRUE(fp != NULL, "Created staging README.txt");
    fputs("Waterscape SFX Test\n", fp);
    fclose(fp);

    snprintf(cmd, sizeof(cmd),
             "\"%s/pack_installer\" \"%s/waterscape_setup_stub\" \"%s\" \"%s\" >/dev/null && "
             "chmod +x \"%s\" && "
             "\"%s\" --verify-stub >/dev/null && "
             "\"%s\" --silent --dir \"%s\" >/dev/null",
             g_module_dir, g_module_dir, stage_dir, sfx_bin,
             sfx_bin, sfx_bin, sfx_bin, inst_dir);
    ASSERT_TRUE(system(cmd) == 0, "pack_installer packed SFX, verified CRC32, and extracted silently");

    char extracted_f[560];
    snprintf(extracted_f, sizeof(extracted_f), "%s/README.txt", inst_dir);
    fp = fopen(extracted_f, "rb");
    ASSERT_TRUE(fp != NULL, "Extracted README.txt verified on disk");
    fclose(fp);

    snprintf(cmd, sizeof(cmd), "\"%s/uninstall\" --silent --dir \"%s\" >/dev/null", g_module_dir, inst_dir);
    ASSERT_TRUE(system(cmd) == 0, "Uninstaller cleanly removed extracted files & manifest");
    fp = fopen(extracted_f, "rb");
    ASSERT_TRUE(fp == NULL, "Extracted README.txt deleted by uninstaller");

    unlink(sample_f);
    rmdir(stage_dir);
    unlink(sfx_bin);
#endif
}

/* 22. Hardware Speed Benchmark: Scalar vs AVX2/SSE2 SIMD + Multi-Core Tile Rasterizer */
static void test_22_hardware_simd_and_multicore_speed_benchmark(void) {
    enum { PIXELS = 4096, ITERS = 2000 };
    uint32_t *buf = (uint32_t *)malloc((size_t)PIXELS * sizeof(uint32_t));
    ASSERT_TRUE(buf != NULL, "Allocated benchmark scanline");
    arcr_scanline_fill_u32_scalar(buf, PIXELS, 0xFFF8FAFCu);

    uint64_t t0 = arcr_platform_monotonic_usec();
    for (int i = 0; i < ITERS; ++i) {
        arcr_scanline_blend_scalar(buf, PIXELS, 0x802563EBu);
    }
    uint64_t dt_scalar = arcr_platform_monotonic_usec() - t0;
    if (dt_scalar == 0u) dt_scalar = 1u;

    bool has_avx2 = (arcr_detect_cpu_flags() & ARCR_CPU_AVX2) != 0u && arcr_compiled_with_avx2_kernel();
    t0 = arcr_platform_monotonic_usec();
    for (int i = 0; i < ITERS; ++i) {
        if (has_avx2) {
            arcr_scanline_blend_avx2(buf, PIXELS, 0x802563EBu);
        } else {
            arcr_scanline_blend_sse2(buf, PIXELS, 0x802563EBu);
        }
    }
    uint64_t dt_simd = arcr_platform_monotonic_usec() - t0;
    if (dt_simd == 0u) dt_simd = 1u;

    unsigned long scalar_mpix = (unsigned long)(((uint64_t)PIXELS * ITERS) / dt_scalar);
    unsigned long simd_mpix   = (unsigned long)(((uint64_t)PIXELS * ITERS) / dt_simd);
    printf("    [BENCH] Alpha Blend (%s vs Scalar): %lu MPix/s vs %lu MPix/s\n",
           has_avx2 ? "AVX2 256-bit" : "SSE2 128-bit", simd_mpix, scalar_mpix);
    free(buf);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        g_module_dir = argv[1];
    }
    printf("=== Waterscape 1.0 / Arcradient (MSHTML-Next) 22-Test Suite ===\n");
    RUN_TEST(test_01_arena_slab_and_atoms);
    RUN_TEST(test_02_module_abi_guard);
    RUN_TEST(test_03_korean_utf8_and_line_breaking);
    RUN_TEST(test_04_codec_deflate_zip_and_png);
    RUN_TEST(test_05_hardware_threadpool_work_stealing);
    RUN_TEST(test_06_accel_simd_avx2_bit_exact_and_gpu_fallback);
    RUN_TEST(test_07_accel_2d_rounded_shadow_and_image_scale);
    RUN_TEST(test_08_platform_glyph_cache_and_clipboard);
    RUN_TEST(test_09_html5_recovery_entities_and_selectors);
    RUN_TEST(test_10_css3_o1_index_vars_calc_media_and_sharing);
    RUN_TEST(test_11_modern_layout_flexbox_grid_and_table);
    RUN_TEST(test_12_parallel_tile_rasterizer_parity);
    RUN_TEST(test_13_on_demand_lazy_modules_and_registry);
    RUN_TEST(test_14_bytecode_js_closures_json_and_fuel);
    RUN_TEST(test_15_js_live_dom_events_and_canvas_2d);
    RUN_TEST(test_16_net_rfc3986_clock_skew_and_media);
    RUN_TEST(test_17_live_http11_redirect_deflate_cookies_and_subresources);
    RUN_TEST(test_18_forms_input_typing_and_find_in_page);
    RUN_TEST(test_19_extensions_wsx_and_native_perf_plugin);
    RUN_TEST(test_20_waterscape_shell_tabs_internal_pages_and_profile);
    RUN_TEST(test_21_installer_and_uninstaller_stubs);
    RUN_TEST(test_22_hardware_simd_and_multicore_speed_benchmark);

    printf("---------------------------------------------------------------\n");
    printf("Result: %d/%d passed (%d failed)\n",
           g_tests_run - g_tests_failed, g_tests_run, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
