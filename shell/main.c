/*
 * Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
 * shell/main.c - Unified `waterscape` CLI, Snapshot Showcase & Benchmark Entrypoint
 */
#include "arcr_shell.h"
#include "waterscape_presenter.h"
#include "../accel/arcr_accel.h"
#include "../base/arcr_base.h"
#include "../platform/arcr_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const kWaterscapeShowcaseLightHtml =
    "<!DOCTYPE html>"
    "<html lang=\"ko\">"
    "<head>"
    "  <meta charset=\"utf-8\">"
    "  <title>Waterscape Browser 1.0 — 설계 개요</title>"
    "  <style>"
    "    :root {"
    "      --accent: #0284c7;"
    "      --card-bg: #f8fafc;"
    "      --card-border: #cbd5e1;"
    "      --badge-bg: #e0f2fe;"
    "    }"
    "    body { background: #ffffff; color: #0f172a; padding: 20px 26px; }"
    "    h1 { color: #0f172a; font-size: 22px; margin-top: 0px; margin-bottom: 4px; }"
    "    .subtitle { color: #475569; font-size: 13px; margin-bottom: 14px; }"
    "    .card {"
    "      background: var(--card-bg);"
    "      border: 1px solid var(--card-border);"
    "      border-left: 4px solid var(--accent);"
    "      padding: 12px 16px;"
    "      margin-bottom: 10px;"
    "    }"
    "    h2 { color: var(--accent); font-size: 16px; margin-top: 0px; margin-bottom: 5px; }"
    "    p { font-size: 14px; margin-bottom: 4px; word-break: keep-all; }"
    "    li { font-size: 13px; margin-bottom: 3px; }"
    "    code { background: var(--badge-bg); color: #0369a1; padding: 1px 6px; }"
    "  </style>"
    "</head>"
    "<body>"
    "  <h1>Waterscape Web Browser 1.0 (Arcradient Pure C11)</h1>"
    "  <p class=\"subtitle\">초경량 순수 C11 네이티브 코어 · 멀티코어 64x64 타일 + AVX2/SSE2/SSE1 · 하드웨어 GPU 프레젠테이션</p>"
    "  <div class=\"card\">"
    "    <h2>1. 구형 Windows 2000 SP4부터 최신 64비트까지 단일 아키텍처</h2>"
    "    <p>무거운 외부 프레임워크 없이 순수 C11로 작성되어 즉시 기동되며, 정적 문서에서는 <code>arcr_js_portable</code> 및 <code>arcr_media</code> 모듈을 적재하지 않아 <code>0 Byte</code> 오버헤드를 보장합니다.</p>"
    "  </div>"
    "  <div class=\"card\">"
    "    <h2>2. Firefox급 확장성(.wsx)과 내장 광고·추적 차단(Waterscape Shield)</h2>"
    "    <p>O(1) 도메인 해시 기반 <code>Waterscape Shield</code>, 다크 모드 변환기, 리더 모드, 네이티브 C11 성능 HUD 플러그인을 기본 내장하며 툴바에서 실시간 전환합니다.</p>"
    "  </div>"
    "  <div class=\"card\">"
    "    <h2>3. 완벽한 한국어 조판 및 다세대 GPU 무중단 폴백</h2>"
    "    <ul>"
    "      <li>한글 어절 유지(<code>word-break: keep-all</code>), 행두·행말 금칙처리 및 <code>Imm32</code> 조합 입력 내장</li>"
    "      <li>Windows <code>D3D11 Flip -&gt; D3D9 -&gt; OpenGL ICD -&gt; GDI</code> 및 Linux <code>X11 MIT-SHM</code> 무복사 출력</li>"
    "    </ul>"
    "  </div>"
    "</body>"
    "</html>";

static void seed_default_bookmarks(ArcrBrowserShell *sh) {
    if (!sh || arcr_shell_bookmark_count(sh) > 0u) return;
    arcr_shell_add_bookmark(sh, "★ 새 탭 홈", "waterscape://newtab");
    arcr_shell_add_bookmark(sh, "⚡ 하드웨어 가속", "waterscape://hardware");
    arcr_shell_add_bookmark(sh, "🧩 확장·플러그인", "waterscape://addons");
    arcr_shell_add_bookmark(sh, "⚙ 환경설정", "waterscape://settings");
}

static bool save_shell_png(ArcrEngine *eng, ArcrBrowserShell *sh,
                           int32_t w, int32_t h, const char *out_png) {
    uint32_t *fb = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!fb) return false;

    bool ok = arcr_shell_render_frame(sh, fb, w, h, w) &&
              arcr_platform_write_png(out_png, fb, w, h, w);

    ArcrEngineStats st;
    memset(&st, 0, sizeof(st));
    arcr_engine_get_stats(eng, &st);
    printf("  -> Saved %-42s (%dx%d) | tiles=%u layout=%luus paint=%luus arena=%luKiB\n",
           out_png, (int)w, (int)h,
           (unsigned)st.tiles_rendered_last_frame,
           (unsigned long)st.last_layout_usec,
           (unsigned long)st.last_paint_usec,
           (unsigned long)(st.arena_used_bytes / 1024u));

    free(fb);
    return ok;
}

static bool run_snapshot_suite(const char *mod_dir, const char *snap_dir) {
    arcr_platform_mkdir_p(snap_dir);
    bool all_ok = true;
    char path[640];
    char legacy_path[640];

    /* 1. waterscape_newtab_dark.png (+ shell_modern_dark.png) */
    {
        ArcrEngineConfig ecfg;
        arcr_engine_default_config(&ecfg, ARCR_PROFILE_MODERN);
        ecfg.module_search_dir = mod_dir;
        ecfg.private_browsing  = true;
        ArcrEngine *eng = arcr_engine_create(&ecfg);
        if (!eng) return false;

        ArcrShellConfig scfg;
        arcr_shell_default_config(&scfg, ARCR_SHELL_THEME_MODERN_DARK);
        scfg.window_width       = 1120;
        scfg.window_height      = 720;
        scfg.show_bookmarks_bar = true;
        scfg.private_browsing   = true;
        scfg.module_search_dir  = mod_dir;

        ArcrBrowserShell *sh = arcr_shell_create(eng, &scfg);
        seed_default_bookmarks(sh);
        arcr_shell_open_tab_url(sh, "waterscape://newtab", true);
        arcr_shell_open_tab_url(sh, "waterscape://hardware", false);
        arcr_shell_open_tab_url(sh, "waterscape://addons", false);
        arcr_shell_activate_tab(sh, 0u);

        snprintf(path, sizeof(path), "%s/waterscape_newtab_dark.png", snap_dir);
        all_ok = save_shell_png(eng, sh, scfg.window_width, scfg.window_height, path) && all_ok;
        snprintf(legacy_path, sizeof(legacy_path), "%s/shell_modern_dark.png", snap_dir);
        all_ok = save_shell_png(eng, sh, scfg.window_width, scfg.window_height, legacy_path) && all_ok;

        arcr_shell_destroy(sh);
        arcr_engine_destroy(eng);
    }

    /* 2. waterscape_showcase_light.png (+ shell_modern_light.png) */
    {
        ArcrEngineConfig ecfg;
        arcr_engine_default_config(&ecfg, ARCR_PROFILE_MODERN);
        ecfg.module_search_dir = mod_dir;
        ecfg.private_browsing  = true;
        ArcrEngine *eng = arcr_engine_create(&ecfg);
        if (!eng) return false;

        ArcrShellConfig scfg;
        arcr_shell_default_config(&scfg, ARCR_SHELL_THEME_MODERN_LIGHT);
        scfg.window_width       = 1120;
        scfg.window_height      = 720;
        scfg.show_bookmarks_bar = true;
        scfg.private_browsing   = false; /* Normal browsing UI without private badge */
        scfg.profile_dir_override = "";
        scfg.module_search_dir  = mod_dir;

        ArcrBrowserShell *sh = arcr_shell_create(eng, &scfg);
        seed_default_bookmarks(sh);
        arcr_shell_open_tab_html(sh, "https://waterscape.browser.org/ko/overview",
                                 kWaterscapeShowcaseLightHtml,
                                 strlen(kWaterscapeShowcaseLightHtml), true);
        arcr_shell_open_tab_url(sh, "waterscape://newtab", false);
        arcr_shell_open_tab_url(sh, "waterscape://settings", false);
        arcr_shell_activate_tab(sh, 0u);
        arcr_shell_set_status_overlay(sh, "링크 이동: https://waterscape.browser.org/ko/hardware-pipeline");

        snprintf(path, sizeof(path), "%s/waterscape_showcase_light.png", snap_dir);
        all_ok = save_shell_png(eng, sh, scfg.window_width, scfg.window_height, path) && all_ok;
        snprintf(legacy_path, sizeof(legacy_path), "%s/shell_modern_light.png", snap_dir);
        all_ok = save_shell_png(eng, sh, scfg.window_width, scfg.window_height, legacy_path) && all_ok;

        arcr_shell_destroy(sh);
        arcr_engine_destroy(eng);
    }

    /* 3. waterscape_addons_hardware.png (Add-ons Manager + F12 Hardware Telemetry Dock) */
    {
        ArcrEngineConfig ecfg;
        arcr_engine_default_config(&ecfg, ARCR_PROFILE_MODERN);
        ecfg.module_search_dir = mod_dir;
        ecfg.private_browsing  = true;
        ArcrEngine *eng = arcr_engine_create(&ecfg);
        if (!eng) return false;

        ArcrShellConfig scfg;
        arcr_shell_default_config(&scfg, ARCR_SHELL_THEME_MODERN_DARK);
        scfg.window_width       = 1120;
        scfg.window_height      = 760;
        scfg.show_bookmarks_bar = false;
        scfg.show_devtools_dock = true;
        scfg.private_browsing   = true;
        scfg.module_search_dir  = mod_dir;

        ArcrBrowserShell *sh = arcr_shell_create(eng, &scfg);
        /* Trigger one blocked tracker probe so Shield telemetry counter is non-zero */
        if (arcr_shell_get_addons(sh)) {
            (void)ws_addons_should_block_url(arcr_shell_get_addons(sh),
                                             "https://doubleclick.net/pixel.js",
                                             "example.com");
        }
        arcr_shell_open_tab_url(sh, "waterscape://addons", true);
        arcr_shell_open_tab_url(sh, "waterscape://hardware", false);
        arcr_shell_activate_tab(sh, 0u);

        snprintf(path, sizeof(path), "%s/waterscape_addons_hardware.png", snap_dir);
        all_ok = save_shell_png(eng, sh, scfg.window_width, scfg.window_height, path) && all_ok;

        arcr_shell_destroy(sh);
        arcr_engine_destroy(eng);
    }

    /* 4. waterscape_win2000_classic.png (+ shell_win2000_classic.png) */
    {
        ArcrEngineConfig ecfg;
        arcr_engine_default_config(&ecfg, ARCR_PROFILE_MINIMUM);
        ecfg.module_search_dir = mod_dir;
        ecfg.private_browsing  = true;
        ArcrEngine *eng = arcr_engine_create(&ecfg);
        if (!eng) return false;

        ArcrShellConfig scfg;
        arcr_shell_default_config(&scfg, ARCR_SHELL_THEME_WIN2000_CLASSIC);
        scfg.window_width         = 860;
        scfg.window_height        = 640;
        scfg.draw_os_window_frame = true;
        scfg.private_browsing     = true;
        scfg.module_search_dir    = mod_dir;

        ArcrBrowserShell *sh = arcr_shell_create(eng, &scfg);
        arcr_shell_open_tab_url(sh, "waterscape://hardware", true);
        arcr_shell_open_tab_html(sh, "https://waterscape.browser.org/win2000",
                                 kWaterscapeShowcaseLightHtml,
                                 strlen(kWaterscapeShowcaseLightHtml), false);
        arcr_shell_activate_tab(sh, 0u);

        snprintf(path, sizeof(path), "%s/waterscape_win2000_classic.png", snap_dir);
        all_ok = save_shell_png(eng, sh, scfg.window_width, scfg.window_height, path) && all_ok;
        snprintf(legacy_path, sizeof(legacy_path), "%s/shell_win2000_classic.png", snap_dir);
        all_ok = save_shell_png(eng, sh, scfg.window_width, scfg.window_height, legacy_path) && all_ok;

        arcr_shell_destroy(sh);
        arcr_engine_destroy(eng);
    }

    return all_ok;
}

static int run_raster_benchmark(const char *mod_dir, ArcrProfileMode profile, ArcrShellTheme theme) {
    enum { BENCH_W = 1280, BENCH_H = 800, BENCH_FRAMES = 100 };

    ArcrEngineConfig ecfg;
    arcr_engine_default_config(&ecfg, profile);
    ecfg.module_search_dir = mod_dir;
    ecfg.private_browsing  = true;

    ArcrEngine *eng = arcr_engine_create(&ecfg);
    if (!eng) return 1;

    ArcrShellConfig scfg;
    arcr_shell_default_config(&scfg, theme);
    scfg.window_width       = BENCH_W;
    scfg.window_height      = BENCH_H;
    scfg.show_bookmarks_bar = true;
    scfg.show_devtools_dock = true;
    scfg.private_browsing   = true;
    scfg.module_search_dir  = mod_dir;

    ArcrBrowserShell *sh = arcr_shell_create(eng, &scfg);
    if (!sh) {
        arcr_engine_destroy(eng);
        return 1;
    }
    seed_default_bookmarks(sh);
    arcr_shell_open_tab_html(sh, "https://waterscape.browser.org/bench",
                             kWaterscapeShowcaseLightHtml,
                             strlen(kWaterscapeShowcaseLightHtml), true);
    arcr_shell_open_tab_url(sh, "waterscape://hardware", false);
    arcr_shell_activate_tab(sh, 0u);

    uint32_t *fb = (uint32_t *)calloc((size_t)BENCH_W * (size_t)BENCH_H, sizeof(uint32_t));
    if (!fb) {
        arcr_shell_destroy(sh);
        arcr_engine_destroy(eng);
        return 1;
    }

    /* Warmup frame */
    arcr_shell_render_frame(sh, fb, BENCH_W, BENCH_H, BENCH_W);

    uint64_t t0 = arcr_platform_monotonic_usec();
    for (int i = 0; i < BENCH_FRAMES; ++i) {
        ArcrWebView *av = arcr_shell_active_webview(sh);
        if (av) arcr_webview_set_scroll(av, (i % 8) * 12);
        arcr_shell_render_frame(sh, fb, BENCH_W, BENCH_H, BENCH_W);
    }
    uint64_t elapsed_us = arcr_platform_monotonic_usec() - t0;
    if (elapsed_us == 0ULL) elapsed_us = 1ULL;

    double total_ms   = (double)elapsed_us / 1000.0;
    double avg_ms     = total_ms / (double)BENCH_FRAMES;
    double fps        = 1000000.0 * (double)BENCH_FRAMES / (double)elapsed_us;
    double total_mpix = ((double)BENCH_W * (double)BENCH_H * (double)BENCH_FRAMES) / 1000000.0;
    double mpix_sec   = total_mpix / ((double)elapsed_us / 1000000.0);

    ArcrEngineStats st;
    memset(&st, 0, sizeof(st));
    arcr_engine_get_stats(eng, &st);

    printf("\n=== Waterscape 1.0 Multi-Core Tile + SIMD Rasterization Benchmark ===\n");
    printf("  Resolution   : %dx%d (%d frames)\n", BENCH_W, BENCH_H, BENCH_FRAMES);
    printf("  Workers/SIMD : %u threads | %s\n",
           (unsigned)st.worker_thread_count, arcr_cpu_simd_tier_name(st.detected_cpu_flags));
    printf("  Elapsed Time : %.2f ms (%.3f ms / frame)\n", total_ms, avg_ms);
    printf("  Throughput   : %.1f FPS | %.1f MPix/sec\n", fps, mpix_sec);

    free(fb);
    arcr_shell_destroy(sh);
    arcr_engine_destroy(eng);
    return 0;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options] [url]\n\n", prog);
    printf("Options:\n");
    printf("  --url <url>               Initial URL or search query (default: waterscape://newtab)\n");
    printf("  --private                 Ephemeral private browsing (no disk history/prefs)\n");
    printf("  --theme <light|dark|classic>  UI theme override\n");
    printf("  --profile <minimum|balanced|modern>  Engine hardware resource profile\n");
    printf("  --module-dir <dir>        Directory containing Tier-4 modules & .wsx add-ons\n");
    printf("  --headless                Render 1 full frame offscreen, print telemetry & exit 0\n");
    printf("  --snapshot-dir <dir>      Render flagship Waterscape showcase PNGs into <dir>\n");
    printf("  --benchmark               Run 100-frame multi-core tile + SIMD benchmark\n");
    printf("  --auto-close-ms <ms>      Auto-close desktop window after <ms> milliseconds\n");
    printf("  --version                 Print browser & engine version\n");
    printf("  --help                    Show this help message\n");
}

int main(int argc, char **argv) {
    const char     *mod_dir       = ".";
    const char     *snap_dir      = NULL;
    const char     *initial_url   = NULL;
    bool            private_mode  = false;
    bool            headless      = false;
    bool            benchmark     = false;
    bool            theme_set     = false;
    bool            profile_set   = false;
    uint32_t        auto_close_ms = 0u;
    ArcrShellTheme  theme         = ARCR_SHELL_THEME_MODERN_DARK;
    ArcrProfileMode profile       = ARCR_PROFILE_MODERN;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0) {
            printf("Waterscape Web Browser %s (Arcradient Pure C11 Engine %s)\n",
                   WATERSCAPE_VERSION_STR, ARCRADIENT_VERSION_STR);
            return 0;
        }
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(arg, "--private") == 0) {
            private_mode = true;
        } else if (strcmp(arg, "--headless") == 0) {
            headless = true;
        } else if (strcmp(arg, "--benchmark") == 0) {
            benchmark = true;
        } else if (strcmp(arg, "--url") == 0 && i + 1 < argc) {
            initial_url = argv[++i];
        } else if (strcmp(arg, "--module-dir") == 0 && i + 1 < argc) {
            mod_dir = argv[++i];
        } else if (strcmp(arg, "--snapshot-dir") == 0 && i + 1 < argc) {
            snap_dir = argv[++i];
        } else if (strcmp(arg, "--auto-close-ms") == 0 && i + 1 < argc) {
            int32_t ms = 0;
            const char *ms_s = argv[++i];
            if (arcr_parse_i32(ms_s, strlen(ms_s), &ms) && ms > 0) {
                auto_close_ms = (uint32_t)ms;
            }
        } else if (strcmp(arg, "--theme") == 0 && i + 1 < argc) {
            const char *t = argv[++i];
            theme_set = true;
            if (arcr_str_eq_nocase(t, "dark"))         theme = ARCR_SHELL_THEME_MODERN_DARK;
            else if (arcr_str_eq_nocase(t, "classic")) theme = ARCR_SHELL_THEME_WIN2000_CLASSIC;
            else                                       theme = ARCR_SHELL_THEME_MODERN_LIGHT;
        } else if (strcmp(arg, "--profile") == 0 && i + 1 < argc) {
            const char *p = argv[++i];
            profile_set = true;
            if (arcr_str_eq_nocase(p, "minimum"))       profile = ARCR_PROFILE_MINIMUM;
            else if (arcr_str_eq_nocase(p, "balanced")) profile = ARCR_PROFILE_BALANCED;
            else                                        profile = ARCR_PROFILE_MODERN;
        } else if (arg[0] != '-' && !initial_url) {
            initial_url = arg;
        }
    }

    ArcrSystemUiMetrics ui_m;
    arcr_platform_query_ui_metrics(&ui_m);
    if (!profile_set) {
        profile = ui_m.prefer_classic_opaque ? ARCR_PROFILE_MINIMUM : ARCR_PROFILE_MODERN;
    }
    if (!theme_set) {
        theme = ui_m.prefer_classic_opaque ? ARCR_SHELL_THEME_WIN2000_CLASSIC
                                           : ARCR_SHELL_THEME_MODERN_LIGHT;
    }

    uint32_t cpu_flags = arcr_detect_cpu_flags();
    ArcrGpuProbeResult gpu_probe;
    memset(&gpu_probe, 0, sizeof(gpu_probe));
    arcr_gpu_probe_system(&gpu_probe);

    printf("Waterscape Web Browser %s — Powered by Arcradient Pure C11 Engine\n",
           WATERSCAPE_VERSION_STR);
    printf("  OS Platform  : %s (ver 0x%04X)\n",
           arcr_platform_os_name(), (unsigned)arcr_platform_os_version());
    printf("  Host SIMD    : %s (flags=0x%X)\n",
           arcr_cpu_simd_tier_name(cpu_flags), (unsigned)cpu_flags);
    printf("  GPU Probe    : %s (%s)\n",
           arcr_accel_backend_name(gpu_probe.best_gpu_backend),
           gpu_probe.adapter_description[0] ? gpu_probe.adapter_description : "Auto");

    if (snap_dir) {
        return run_snapshot_suite(mod_dir, snap_dir) ? 0 : 1;
    }

    if (benchmark) {
        return run_raster_benchmark(mod_dir, profile, theme);
    }

    ArcrEngineConfig ecfg;
    arcr_engine_default_config(&ecfg, profile);
    ecfg.module_search_dir = mod_dir;
    ecfg.private_browsing  = private_mode;

    ArcrEngine *eng = arcr_engine_create(&ecfg);
    if (!eng) {
        fprintf(stderr, "waterscape: failed to initialize Arcradient Engine.\n");
        return 1;
    }

    ArcrShellConfig scfg;
    arcr_shell_default_config(&scfg, theme);
    scfg.draw_os_window_frame = false; /* OS provides window border in desktop mode */
    scfg.private_browsing     = private_mode;
    scfg.load_saved_prefs     = (!private_mode && !theme_set);
    scfg.module_search_dir    = mod_dir;

    ArcrBrowserShell *sh = arcr_shell_create(eng, &scfg);
    if (!sh) {
        arcr_engine_destroy(eng);
        return 1;
    }

    seed_default_bookmarks(sh);
    if (initial_url && initial_url[0] != '\0') {
        arcr_shell_open_tab_url(sh, initial_url, true);
    } else {
        arcr_shell_open_tab_url(sh, "waterscape://newtab", true);
        arcr_shell_open_tab_url(sh, "waterscape://hardware", false);
        arcr_shell_activate_tab(sh, 0u);
    }

    if (headless) {
        int32_t w = scfg.window_width;
        int32_t h = scfg.window_height;
        uint32_t *fb = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
        bool ok = fb && arcr_shell_render_frame(sh, fb, w, h, w);
        ArcrEngineStats st;
        memset(&st, 0, sizeof(st));
        arcr_engine_get_stats(eng, &st);

        printf("  Headless Run : %s (%dx%d) | URL=%s\n",
               ok ? "OK" : "FAIL", (int)w, (int)h,
               arcr_shell_active_webview(sh)
                   ? arcr_webview_get_url(arcr_shell_active_webview(sh))
                   : "none");
        printf("  Telemetry    : workers=%u tiles=%u layout=%luus paint=%luus addons=%lu arena=%luB\n",
               (unsigned)st.worker_thread_count,
               (unsigned)st.tiles_rendered_last_frame,
               (unsigned long)st.last_layout_usec,
               (unsigned long)st.last_paint_usec,
               (unsigned long)(arcr_shell_get_addons(sh)
                                   ? ws_addons_count(arcr_shell_get_addons(sh))
                                   : 0u),
               (unsigned long)st.arena_used_bytes);

        free(fb);
        arcr_shell_destroy(sh);
        arcr_engine_destroy(eng);
        return ok ? 0 : 1;
    }

    WaterscapeWindowOptions wopts;
    memset(&wopts, 0, sizeof(wopts));
    wopts.window_title        = private_mode
                                    ? "Waterscape Browser (사생활 보호)"
                                    : "Waterscape Browser";
    wopts.initial_width       = scfg.window_width;
    wopts.initial_height      = scfg.window_height;
    wopts.force_cpu_presenter = ecfg.force_cpu_rendering;
    wopts.auto_close_ms       = auto_close_ms;

    int rc = waterscape_run_desktop_window(eng, sh, &wopts);
    arcr_shell_save_profile(sh);
    arcr_shell_destroy(sh);
    arcr_engine_destroy(eng);
    return rc;
}
