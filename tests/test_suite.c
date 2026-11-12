/*
 * Arcradient (MSHTML-Next)
 * tests/test_suite.c - Pure C11 Verification Suite for Phase 0 & Phase 1 Core Guarantees
 */
#include "../accel/arcr_accel.h"
#include "../base/arcr_base.h"
#include "../core/arcr_dom.h"
#include "../css/arcr_css.h"
#include "../include/arcr_module.h"
#include "../include/arcradient_embed.h"
#include "../media/arcr_media.h"
#include "../net/arcr_net.h"
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

/* 1. Verify Arena & Slab Pool reclamation + Atom string deduplication */
static void test_arena_slab_and_atoms(void) {
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

/* 2. Verify C Module ABI safety: rejects AVX2 module on SSE1-only Legacy x86 host! */
static void test_module_abi_guard(void) {
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

    /* Valid on Windows 2000 SP4 (0x0500) + Pentium III SSE1 */
    ASSERT_TRUE(arcr_module_validate(&desc, ARCR_ARCH_X86_32, ARCR_OS_WIN2000, ARCR_CPU_SSE1) == ARCR_MOD_OK,
                "Valid Win2000 x86 SSE1 module accepted");

    /* Reject x64 DLL accidentally placed in 32-bit x86 directory */
    ASSERT_TRUE(arcr_module_validate(&desc, ARCR_ARCH_X86_64, ARCR_OS_WIN10, ARCR_CPU_AVX2) == ARCR_MOD_ERR_ARCH_MISMATCH,
                "Architecture mismatch rejected");

    /* Reject AVX2-compiled module on SSE1-only Pentium III / Win2000 host */
    desc.required_cpu_flags = ARCR_CPU_SSE1 | ARCR_CPU_AVX2;
    ASSERT_TRUE(arcr_module_validate(&desc, ARCR_ARCH_X86_32, ARCR_OS_WIN2000, ARCR_CPU_SSE1) == ARCR_MOD_ERR_CPU_UNSUPPORTED,
                "AVX2 module cleanly rejected on SSE1-only CPU before SIGILL");

    /* Reject Win10-only module on Windows 2000 host */
    desc.required_cpu_flags = ARCR_CPU_SSE1;
    desc.min_os_major_minor = ARCR_OS_WIN10;
    ASSERT_TRUE(arcr_module_validate(&desc, ARCR_ARCH_X86_32, ARCR_OS_WIN2000, ARCR_CPU_SSE1) == ARCR_MOD_ERR_OS_TOO_OLD,
                "Win10-only module rejected on Windows 2000 host");
}

/* 3. Verify Korean UTF-8, word-break: keep-all vs normal, and CJK 금칙처리 */
static void test_korean_utf8_and_line_breaking(void) {
    const char *ko = "한글.";
    uint32_t cp1 = 0, cp2 = 0, cp3 = 0;
    size_t n1 = arcr_utf8_decode_one(ko, strlen(ko), &cp1);
    size_t n2 = arcr_utf8_decode_one(ko + n1, strlen(ko) - n1, &cp2);
    size_t n3 = arcr_utf8_decode_one(ko + n1 + n2, strlen(ko) - n1 - n2, &cp3);
    ASSERT_TRUE(cp1 == 0xD55Cu && cp2 == 0xAE00u && cp3 == '.' && n3 == 1u,
                "UTF-8 decodes '한'(U+D55C), '글'(U+AE00), '.'");
    ASSERT_TRUE(arcr_cp_is_hangul(cp1) && arcr_cp_is_hangul(cp2), "Hangul syllable detected");

    /* Normal mode wraps between '한' and '글', keep-all keeps Korean 어절 intact */
    ASSERT_TRUE(arcr_unicode_can_break_line(cp1, cp2, false) == true,
                "word-break: normal allows Hangul syllable wrap");
    ASSERT_TRUE(arcr_unicode_can_break_line(cp1, cp2, true) == false,
                "word-break: keep-all keeps Hangul 어절 intact");
    ASSERT_TRUE(arcr_unicode_can_break_line(' ', cp1, true) == true,
                "word-break: keep-all breaks cleanly after space");

    /* 행두 금칙처리: never break immediately before '.', ')', '”', '。' */
    ASSERT_TRUE(arcr_unicode_can_break_line(cp2, '.', false) == false,
                "Prohibits orphaning '.' at start of new line");
    ASSERT_TRUE(arcr_unicode_can_break_line(cp2, 0x3002u, false) == false,
                "Prohibits orphaning CJK full stop at start of new line");
    /* 행말 금칙처리: never break immediately after '(' or '“' */
    ASSERT_TRUE(arcr_unicode_can_break_line('(', cp1, false) == false,
                "Prohibits leaving '(' dangling at end of line");
}

/* 4. Verify Bit-Exact Equivalence between CPU Scalar, SSE1+SWAR, and SSE2 SIMD */
static void test_accel_simd_bit_exact_and_gpu_fallback(void) {
    enum { N = 67 }; /* Odd pixel count exercises both vector loops and scalar tails */
    uint32_t buf_scalar[N], buf_sse1[N], buf_sse2[N];
    for (size_t i = 0; i < N; ++i) {
        uint32_t seed = (uint32_t)(i * 0x45D9F3Bu + 0x12345678u);
        buf_scalar[i] = buf_sse1[i] = buf_sse2[i] = seed | 0xFF000000u;
    }

    const uint32_t colors[] = {
        0x00112233u, 0xFF336699u, 0x802563EBu, 0x37DC2626u, 0xFE10B981u
    };
    for (size_t c = 0; c < ARCR_ARRAY_LEN(colors); ++c) {
        arcr_scanline_blend_scalar(buf_scalar, N, colors[c]);
        arcr_scanline_blend_sse1_swar(buf_sse1, N, colors[c]);
        arcr_scanline_blend_sse2(buf_sse2, N, colors[c]);
    }

    ASSERT_TRUE(memcmp(buf_scalar, buf_sse1, sizeof(buf_scalar)) == 0,
                "SSE1+SWAR kernel matches CPU Scalar reference 100% bit-for-bit");
    ASSERT_TRUE(memcmp(buf_scalar, buf_sse2, sizeof(buf_scalar)) == 0,
                "SSE2 XMM kernel matches CPU Scalar reference 100% bit-for-bit");

    /* Cost router & GPU fault fallback test */
    ArcrAccelContext *accel = arcr_accel_create(0xFFFFFFFFu, false, true);
    ASSERT_TRUE(arcr_accel_is_gpu_healthy(accel), "GPU initially healthy");

    /* Small rect chooses CPU despite healthy GPU due to setup/sync cost */
    ArcrAccelBackendId small_route = arcr_accel_choose_route(
        accel, ARCR_OP_FILL_RECT, 32 * 32, false, NULL, NULL);
    ASSERT_TRUE(small_route <= ARCR_BACKEND_CPU_SSE2_SSE3,
                "Small rect stays on CPU rather than wasting GPU setup/sync");

    /* Large GPU-resident composite chooses GPU while healthy */
    ArcrAccelBackendId big_route = arcr_accel_choose_route(
        accel, ARCR_OP_LARGE_COMPOSITE, 1024 * 768, true, NULL, NULL);
    ASSERT_TRUE(big_route >= ARCR_BACKEND_GPU_D3D9,
                "Large GPU-resident composite routes to GPU");

    /* Inject D3DERR_DEVICELOST -> Seamless fallback to CPU without process termination! */
    arcr_accel_report_gpu_fault(accel, true);
    ASSERT_TRUE(!arcr_accel_is_gpu_healthy(accel) && arcr_accel_did_fallback_occur(accel),
                "GPU marked unhealthy after device loss");
    ArcrAccelBackendId recovered_route = arcr_accel_choose_route(
        accel, ARCR_OP_LARGE_COMPOSITE, 1024 * 768, true, NULL, NULL);
    ASSERT_TRUE(recovered_route <= ARCR_BACKEND_CPU_SSE2_SSE3,
                "Large composite seamlessly falls back to CPU after GPU fault");

    arcr_accel_destroy(accel);
}

/* 5. Verify HTML5 Error Recovery + CSS var() + Ancestor Bloom + Style Sharing */
static void test_html_recovery_css_vars_and_style_sharing(void) {
    ArcrAtomTable atoms;
    arcr_atom_table_init(&atoms, 128u);

    /* Deliberately unclosed <p> tags & sibling <li> tags + CSS variables + 5 identical .item nodes */
    const char *html =
        "<html><head><title>복구 테스트</title>"
        "<style>"
        "  :root { --brand: #1d4ed8; }"
        "  ul .item { color: var(--brand); padding: 6px; }"
        "</style></head><body>"
        "<p>첫 번째 문단 (닫는 태그 생략)"
        "<p>두 번째 문단 자동 분리"
        "<ul>"
        "  <li class='item'>항목 1"
        "  <li class='item'>항목 2"
        "  <li class='item'>항목 3"
        "  <li class='item'>항목 4"
        "  <li class='item'>항목 5"
        "</ul></body></html>";

    ArcrDocument *doc = arcr_html_parse(&atoms, html, strlen(html));
    ASSERT_TRUE(doc != NULL && doc->body != NULL, "HTML parsed with error recovery");
    ASSERT_TRUE(strcmp(doc->title, "복구 테스트") == 0, "Korean <title> extracted");
    ASSERT_TRUE(!doc->has_scripts && !doc->has_media, "No false-positive script/media flags");

    ArcrStylesheet *sheet = arcr_css_parse_document_styles(doc);
    size_t share_hits = 0u;
    arcr_css_resolve_tree(doc, sheet, &share_hits);
    ASSERT_TRUE(share_hits >= 4u, "4 sibling <li class='item'> nodes shared computed style pointer");

    arcr_dom_document_destroy(doc);
    arcr_atom_table_destroy(&atoms);
}

/* 6. Verify On-Demand Module Loading: Static Page = 0 JS Overhead; Script Page = Dynamic Load */
static void test_on_demand_js_and_media_modules(void) {
    ArcrEngineConfig cfg;
    arcr_engine_default_config(&cfg, ARCR_PROFILE_BALANCED);
    cfg.module_search_dir = g_module_dir;

    ArcrEngine *eng = arcr_engine_create(&cfg);
    ASSERT_TRUE(eng != NULL, "Engine created");

    ArcrWebView *view = arcr_webview_create(eng, 800, 600, NULL);
    const char *static_html = "<html><head><title>정적 문서</title></head><body><h1>순수 문서</h1></body></html>";
    ASSERT_TRUE(arcr_webview_load_html(view, "https://example.org/", static_html, strlen(static_html)),
                "Static HTML loaded");

    ArcrEngineStats st;
    arcr_engine_get_stats(eng, &st);
    ASSERT_TRUE(st.js_module_loaded == false && st.js_heap_bytes == 0u,
                "Static document NEVER loads arcr-js module (0 Byte JS overhead!)");
    ASSERT_TRUE(st.media_module_loaded == false,
                "Static document NEVER loads arcr-media module");

    /* Now load interactive HTML containing <script> and <video> */
    const char *dyn_html =
        "<html><head><title>초기 제목</title></head><body>"
        "<video src='clip.webm'></video>"
        "<script>document.title = \"동적 갱신 완료\";</script>"
        "</body></html>";
    ASSERT_TRUE(arcr_webview_load_html(view, "https://example.org/app", dyn_html, strlen(dyn_html)),
                "Interactive HTML loaded");

    arcr_engine_get_stats(eng, &st);
    ASSERT_TRUE(st.js_module_loaded == true && st.js_heap_bytes > 0u,
                "Script document dynamically loads arcr-js module");
    ASSERT_TRUE(st.media_module_loaded == true,
                "Video document dynamically loads arcr-media module");
    ASSERT_TRUE(strcmp(arcr_webview_get_title(view), "동적 갱신 완료") == 0,
                "ArcradientJS executed script and updated document.title");

    /* Memory pressure notification releases JS bytecode caches cleanly */
    size_t before_pressure = st.js_heap_bytes;
    arcr_engine_notify_memory_pressure(eng, 2);
    arcr_engine_get_stats(eng, &st);
    ASSERT_TRUE(st.js_heap_bytes < before_pressure,
                "Critical memory pressure reclaims JS VM caches");

    arcr_engine_destroy(eng);
}

/* 7. Verify Net Module Trust Store & Dead CMOS Battery Clock-Skew Guard + Adaptive Media */
static void test_net_clock_skew_and_adaptive_media(void) {
    char net_path[512];
    arcr_platform_format_module_path(net_path, sizeof(net_path), g_module_dir, "arcr_net");
    ArcrDynLib *net_lib = arcr_platform_dlopen(net_path);
    ASSERT_TRUE(net_lib != NULL, "arcr_net dynamic library opened");

    ArcrModuleQueryFn qfn = NULL;
    void *sym = arcr_platform_dlsym(net_lib, "arcr_module_query");
    memcpy(&qfn, &sym, sizeof(qfn));
    const ArcrModuleDescriptor *ndesc = qfn();
    const ArcrNetVTable *nvt = (const ArcrNetVTable *)ndesc->vtable;

    char reason[256];
    /* Simulate retro Windows 2000 PC with dead CR2032 battery reset to 2000-01-01 (946684800) */
    ArcrCertCheckInput cmos_reset = {
        .expected_host             = "arcradient.org",
        .cert_san_dns              = "*.arcradient.org",
        .trusted_root_signature_ok = true,
        .not_before_epoch          = 1735689600LL,
        .not_after_epoch           = 1893456000LL,
        .system_wall_epoch         = 946684800LL
    };
    ASSERT_TRUE(nvt->verify_tls_trust(&cmos_reset, reason, sizeof(reason)) == ARCR_SEC_CLOCK_SKEW_ERR,
                "Detects dead CMOS battery clock skew distinctly from generic cert error");

    /* Valid wildcard SAN + modern clock passes */
    cmos_reset.expected_host     = "docs.arcradient.org";
    cmos_reset.system_wall_epoch = 1794460000LL;
    ASSERT_TRUE(nvt->verify_tls_trust(&cmos_reset, reason, sizeof(reason)) == ARCR_SEC_HTTPS_VERIFIED,
                "Valid wildcard SAN + Root CA + valid clock passes TLS verification");
    arcr_platform_dlclose(net_lib);

    /* Adaptive Fluid Media test under 92% CPU overload */
    char med_path[512];
    arcr_platform_format_module_path(med_path, sizeof(med_path), g_module_dir, "arcr_media");
    ArcrDynLib *med_lib = arcr_platform_dlopen(med_path);
    ASSERT_TRUE(med_lib != NULL, "arcr_media dynamic library opened");
    sym = arcr_platform_dlsym(med_lib, "arcr_module_query");
    memcpy(&qfn, &sym, sizeof(qfn));
    const ArcrMediaVTable *mvt = (const ArcrMediaVTable *)qfn()->vtable;

    const ArcrStreamVariant streams[3] = {
        {640, 360, 600}, {1280, 720, 2200}, {1920, 1080, 5000}
    };
    ArcrMediaAdaptiveDecision dec;
    mvt->compute_adaptive_policy(ARCR_PROFILE_MINIMUM, streams, 3u, 800, 450, 92u, &dec);
    ASSERT_TRUE(dec.chosen_stream_index == 0u &&
                dec.frame_drop_mode == ARCR_DROP_NONREF_BFRAMES &&
                dec.scale_quality == ARCR_SCALE_NEAREST_FAST &&
                dec.keep_audio_continuous == true,
                "Overloaded legacy CPU selects 360p, drops non-ref frames, preserves audio");
    arcr_platform_dlclose(med_lib);
}

/* 8. Verify Browser Shell Layout (Compact Chrome + Bookmarks Toggle + Win2000 Classic) */
static void test_browser_shell_layout_and_render(void) {
    ArcrEngineConfig ecfg;
    arcr_engine_default_config(&ecfg, ARCR_PROFILE_MINIMUM);
    ecfg.module_search_dir = g_module_dir;
    ArcrEngine *eng = arcr_engine_create(&ecfg);

    ArcrShellConfig scfg;
    arcr_shell_default_config(&scfg, ARCR_SHELL_THEME_WIN2000_CLASSIC);
    ASSERT_TRUE(scfg.show_bookmarks_bar == false, "Bookmarks bar hidden by default");

    ArcrBrowserShell *sh = arcr_shell_create(eng, &scfg);
    ArcrShellLayoutMetrics m_hidden, m_shown;
    arcr_shell_compute_layout(sh, &m_hidden);
    arcr_shell_set_bookmarks_bar_visible(sh, true);
    arcr_shell_compute_layout(sh, &m_shown);

    ASSERT_TRUE(m_hidden.bookmarks_bar_rect.height == 0 &&
                m_shown.bookmarks_bar_rect.height == 24 &&
                m_hidden.content_viewport.height == m_shown.content_viewport.height + 24,
                "Toggling bookmarks bar cleanly reclaims/allocates 24px viewport height");

    const char *ko_doc = "<html><head><title>Win2000 테스트</title></head><body><h1>가볍고 빠른 브라우저</h1></body></html>";
    arcr_shell_open_tab_html(sh, "https://arcradient.org/", ko_doc, strlen(ko_doc), true);

    uint32_t *fb = (uint32_t *)calloc(320u * 240u, sizeof(uint32_t));
    ASSERT_TRUE(arcr_shell_render_frame(sh, fb, 320, 240, 320),
                "CPU renders Win2000 Classic shell + Korean document at 320x240 without GPU");
    free(fb);

    arcr_shell_destroy(sh);
    arcr_engine_destroy(eng);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        g_module_dir = argv[1];
    }
    printf("=== Arcradient (MSHTML-Next) Pure C11 Verification Suite ===\n");
    RUN_TEST(test_arena_slab_and_atoms);
    RUN_TEST(test_module_abi_guard);
    RUN_TEST(test_korean_utf8_and_line_breaking);
    RUN_TEST(test_accel_simd_bit_exact_and_gpu_fallback);
    RUN_TEST(test_html_recovery_css_vars_and_style_sharing);
    RUN_TEST(test_on_demand_js_and_media_modules);
    RUN_TEST(test_net_clock_skew_and_adaptive_media);
    RUN_TEST(test_browser_shell_layout_and_render);

    printf("------------------------------------------------------------\n");
    printf("Result: %d/%d passed (%d failed)\n",
           g_tests_run - g_tests_failed, g_tests_run, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
