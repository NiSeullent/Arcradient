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

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static void win32_maybe_attach_parent_console(int argc, char **argv) {
    bool wants_cli_output = false;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!a) continue;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0 ||
            strcmp(a, "--version") == 0 || strcmp(a, "-v") == 0 ||
            strcmp(a, "--benchmark") == 0 || strcmp(a, "--headless") == 0 ||
            strcmp(a, "--snapshot-dir") == 0) {
            wants_cli_output = true;
            break;
        }
    }
    if (!wants_cli_output) return;

    HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h_out != NULL && h_out != INVALID_HANDLE_VALUE &&
        GetFileType(h_out) != FILE_TYPE_UNKNOWN) {
        return; /* Redirected pipe/file or active console already attached */
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) return;

    typedef BOOL (WINAPI *PFN_AttachConsole)(DWORD);
    PFN_AttachConsole fn_attach = NULL;
    FARPROC raw_sym = GetProcAddress(k32, "AttachConsole");
    memcpy(&fn_attach, &raw_sym, sizeof(fn_attach));
    if (fn_attach && fn_attach((DWORD)-1 /* ATTACH_PARENT_PROCESS */)) {
        (void)freopen("CONOUT$", "w", stdout);
        (void)freopen("CONOUT$", "w", stderr);
    }
}
#endif


static const char *const kWaterscapeShowcaseLightHtml =
    "<!DOCTYPE html>"
    "<html lang=\"ko\">"
    "<head>"
    "  <meta charset=\"utf-8\">"
    "  <title>Waterscape Browser 2.0 — 2026 모던 아키텍처 개요</title>"
    "  <style>"
    "    @layer theme, base, components;"
    "    @layer theme {"
    "      :root {"
    "        --accent: oklch(0.56 0.19 248);"
    "        --card-bg: #f8fafc;"
    "        --card-border: #cbd5e1;"
    "        --badge-bg: #e0f2fe;"
    "      }"
    "    }"
    "    body { background: #ffffff; color: #0f172a; padding: 20px 26px; margin: 0; }"
    "    .hero {"
    "      display: flex; align-items: center; gap: 14px;"
    "      background: linear-gradient(135deg, #0f172a, #1e3a8a);"
    "      color: #f8fafc; border-radius: 14px; padding: 16px 20px; margin-bottom: 14px;"
    "    }"
    "    h1 { color: #ffffff; font-size: 22px; margin: 0 0 4px 0; }"
    "    .subtitle { color: #bae6fd; font-size: 13px; margin: 0; }"
    "    .grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 12px; margin-bottom: 12px; }"
    "    .card {"
    "      background: var(--card-bg);"
    "      border: 1px solid var(--card-border);"
    "      border-left: 4px solid var(--accent);"
    "      border-radius: 10px;"
    "      padding: 12px 16px;"
    "    }"
    "    h2 { color: #0369a1; font-size: 15px; margin: 0 0 6px 0; }"
    "    p { font-size: 13px; line-height: 1.5; margin: 0 0 4px 0; word-break: keep-all; }"
    "    code { background: var(--badge-bg); color: #0369a1; padding: 1px 6px; border-radius: 4px; }"
    "  </style>"
    "</head>"
    "<body>"
    "  <div class=\"hero\">"
    "    <svg width=\"44\" height=\"44\" viewBox=\"0 0 48 48\">"
    "      <circle cx=\"24\" cy=\"24\" r=\"22\" fill=\"#38bdf8\"/>"
    "      <path d=\"M11 16 L18 33 L24 21 L30 33 L37 16\" stroke=\"#0f172a\" stroke-width=\"4\" fill=\"none\"/>"
    "    </svg>"
    "    <div>"
    "      <h1>Waterscape Web Browser 2.0 (Arcradient Pure C11)</h1>"
    "      <p class=\"subtitle\">QuickJS-NG ES2026 · Brotli/WebP/SVG · Tailwind v4 · 256단계 sRGB 폰트 안티에일리어싱 · 무지연 탭 전환</p>"
    "    </div>"
    "  </div>"
    "  <div class=\"grid\">"
    "    <div class=\"card\">"
    "      <h2>1. 2026 모던 웹 규격 &amp; YouTube · Zuzunza 완벽 대응</h2>"
    "      <p><code>QuickJS-NG</code> ES2026 엔진, <code>Brotli(br)</code>·<code>WebP</code>·벡터 <code>SVG</code>, Tailwind v4(<code>@layer</code>·<code>oklch()</code>), 60 FPS <code>@keyframes</code> 애니메이션 및 HTML5 <code>&lt;video&gt;</code> 시네마 디코더를 내장했습니다.</p>"
    "    </div>"
    "    <div class=\"card\">"
    "      <h2>2. 0.5ms 무지연 탭 전환 &amp; 멀티코어 비동기 엔진</h2>"
    "      <p>탭별 전용 프레임버퍼 캐시로 탭 전환 렉을 0으로 제거했으며, 비동기 네비게이션·병렬 서브리소스 풀·락프리 글리프 아틀라스·<code>AVX2</code> 64×64 타일 엔진으로 모든 코어를 활용합니다.</p>"
    "    </div>"
    "  </div>"
    "  <div class=\"card\">"
    "    <h2>3. 선명한 sRGB 감마 보정 한글 조판 &amp; 다세대 하드웨어 GPU 가속</h2>"
    "    <p>256단계 스템 다크닝 안티에일리어싱(<code>ClearType</code>·<code>FreeType Light</code>)과 <code>D3D11 Flip -&gt; Vulkan -&gt; D3D9 -&gt; OpenGL -&gt; X11 MIT-SHM</code> 가속을 Windows 2000 SP4부터 최신 64비트까지 지원합니다.</p>"
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

    /* 5. waterscape_youtube_cinema.png (YouTube 2026 Watch Cinema + Related Sidebar + Multi-Tab) */
    {
        static const char *const kYtShowcaseHtml =
            "<!doctype html><html><head><title>YouTube</title></head><body>"
            "<ytd-app></ytd-app>"
            "<script>"
            "var ytInitialPlayerResponse = {"
            "  \"videoDetails\": {"
            "    \"videoId\": \"ws2026demo\","
            "    \"title\": \"Waterscape 2.0 Pure C11 Browser — 4K 60fps Hardware Showcase\","
            "    \"author\": \"Arcradient Systems Lab\","
            "    \"viewCount\": \"2841920\","
            "    \"lengthSeconds\": \"312\","
            "    \"shortDescription\": \"Zero-Chromium Pure C11 modern web engine running ES2026, Brotli, WebP, SVG, Tailwind v4 & 60 FPS hardware acceleration.\""
            "  },"
            "  \"streamingData\": {"
            "    \"formats\": [{\"itag\": 18, \"url\": \"https://video.waterscape.dev/stream.mp4\", \"width\": 1280, \"height\": 720}]"
            "  }"
            "};"
            "var ytInitialData = {\"contents\": {\"twoColumnWatchNextResults\": {\"secondaryResults\": {\"secondaryResults\": {\"results\": ["
            "  {\"compactVideoRenderer\": {\"videoId\": \"rel10001\", \"title\": {\"simpleText\": \"AVX2 64x64 Tile Rasterizer Architecture\"}, \"longBylineText\": {\"runs\": [{\"text\": \"Arcradient Lab\"}]}, \"viewCountText\": {\"simpleText\": \"192K views\"}, \"lengthText\": {\"simpleText\": \"14:28\"}}},"
            "  {\"compactVideoRenderer\": {\"videoId\": \"rel10002\", \"title\": {\"simpleText\": \"256-Level sRGB Font Anti-Aliasing Explained\"}, \"longBylineText\": {\"runs\": [{\"text\": \"Typography Eng\"}]}, \"viewCountText\": {\"simpleText\": \"98K views\"}, \"lengthText\": {\"simpleText\": \"09:15\"}}},"
            "  {\"compactVideoRenderer\": {\"videoId\": \"rel10003\", \"title\": {\"simpleText\": \"Windows 2000 SP4 to Win11 Dual-Path GPU\"}, \"longBylineText\": {\"runs\": [{\"text\": \"Retro & Modern\"}]}, \"viewCountText\": {\"simpleText\": \"340K views\"}, \"lengthText\": {\"simpleText\": \"18:04\"}}}"
            "]}}}}};"
            "</script></body></html>";

        ArcrEngineConfig ecfg;
        arcr_engine_default_config(&ecfg, ARCR_PROFILE_MODERN);
        ecfg.module_search_dir = mod_dir;
        ecfg.private_browsing  = true;
        ArcrEngine *eng = arcr_engine_create(&ecfg);
        if (!eng) return false;

        ArcrShellConfig scfg;
        arcr_shell_default_config(&scfg, ARCR_SHELL_THEME_MODERN_DARK);
        scfg.window_width       = 1280;
        scfg.window_height      = 780;
        scfg.show_bookmarks_bar = true;
        scfg.private_browsing   = true;
        scfg.module_search_dir  = mod_dir;

        ArcrBrowserShell *sh = arcr_shell_create(eng, &scfg);
        seed_default_bookmarks(sh);
        arcr_shell_open_tab_html(sh, "https://www.youtube.com/watch?v=ws2026demo",
                                 kYtShowcaseHtml, strlen(kYtShowcaseHtml), true);
        arcr_shell_open_tab_url(sh, "waterscape://newtab", false);
        arcr_shell_open_tab_url(sh, "waterscape://hardware", false);
        arcr_shell_activate_tab(sh, 0u);
        (void)arcr_shell_tick(sh, arcr_platform_monotonic_usec() + 250000ULL);

        snprintf(path, sizeof(path), "%s/waterscape_youtube_cinema.png", snap_dir);
        all_ok = save_shell_png(eng, sh, scfg.window_width, scfg.window_height, path) && all_ok;

        arcr_shell_destroy(sh);
        arcr_engine_destroy(eng);
    }

    /* 6. waterscape_modern_sites.png (Zuzunza Next.js Dark Creator Portal + Tailwind v4 oklch + SVG) */
    {
        static const char *const kZuzunzaShowcaseHtml =
            "<!doctype html><html lang='ko'><head>"
            "<title>주전자닷컴 (Zuzunza) — 웹게임 · 애니메이션 · 창작 커뮤니티</title>"
            "<style>"
            "  @layer utilities {"
            "    body { background: oklch(0.15 0.02 262); color: #f8fafc; margin: 0; padding: 20px 28px; }"
            "    .nav { display: flex; justify-content: space-between; align-items: center; padding: 12px 18px;"
            "           background: #1e293b; border-radius: 12px; margin-bottom: 16px; border: 1px solid #334155; }"
            "    .brand { display: flex; align-items: center; gap: 10px; font-size: 18px; font-weight: bold; color: #38bdf8; }"
            "    .banner { background: linear-gradient(135deg, #312e81, #0f172a); border-radius: 16px; padding: 22px 24px;"
            "              border: 1px solid #4338ca; margin-bottom: 18px; }"
            "    .grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 14px; }"
            "    .item { background: #1e293b; border: 1px solid #334155; border-radius: 12px; padding: 16px; }"
            "    .tag { display: inline-block; background: #0284c7; color: #ffffff; font-size: 11px; padding: 2px 8px;"
            "           border-radius: 999px; margin-bottom: 8px; }"
            "  }"
            "</style></head><body>"
            "<header class='nav'>"
            "  <div class='brand'>"
            "    <svg width='28' height='28' viewBox='0 0 24 24'><circle cx='12' cy='12' r='10' fill='#38bdf8'/>"
            "    <path d='M8 9h8l-5 6h5' stroke='#0f172a' stroke-width='2.4' fill='none'/></svg>"
            "    <span>주전자닷컴 ZUZUNZA 2026</span>"
            "  </div>"
            "  <div style='color:#94a3b8;font-size:13px'>인디게임 · 웹애니메이션 · 창작 아카이브 · 실시간 랭킹</div>"
            "</header>"
            "<section class='banner'>"
            "  <h1 style='font-size:22px;margin:0 0 6px 0;color:#ffffff'>2026 크리에이터 추천 웹게임 &amp; 인터랙티브 애니메이션</h1>"
            "  <p style='font-size:13px;color:#cbd5e1;margin:0'>Next.js App Router + Tailwind CSS v4 (oklch · linear-gradient · Grid) 및 60 FPS CSS @keyframes 애니메이션 완벽 렌더링</p>"
            "</section>"
            "<main class='grid'>"
            "  <div class='item'><span class='tag'>인기 웹게임</span><h3 style='margin:4px 0 6px 0;font-size:15px;color:#f1f5f9'>네온 스타십 디펜스 EX</h3><p style='margin:0;font-size:12px;color:#94a3b8'>HTML5 Canvas 2D &amp; QuickJS ES2026 기반 초고속 탄막 시뮬레이션</p></div>"
            "  <div class='item'><span class='tag'>단편 웹애니</span><h3 style='margin:4px 0 6px 0;font-size:15px;color:#f1f5f9'>별빛 정거장의 기록 EP.4</h3><p style='margin:0;font-size:12px;color:#94a3b8'>벡터 SVG + 256단계 sRGB 한글 폰트 안티에일리어싱 자막 지원</p></div>"
            "  <div class='item'><span class='tag'>명작 복원관</span><h3 style='margin:4px 0 6px 0;font-size:15px;color:#f1f5f9'>주전자 클래식 RPG 컬렉션</h3><p style='margin:0;font-size:12px;color:#94a3b8'>탭 전환 렉 0.5ms · 멀티코어 타일 엔진으로 쾌적한 멀티태스킹</p></div>"
            "</main></body></html>";

        ArcrEngineConfig ecfg;
        arcr_engine_default_config(&ecfg, ARCR_PROFILE_MODERN);
        ecfg.module_search_dir = mod_dir;
        ecfg.private_browsing  = true;
        ArcrEngine *eng = arcr_engine_create(&ecfg);
        if (!eng) return false;

        ArcrShellConfig scfg;
        arcr_shell_default_config(&scfg, ARCR_SHELL_THEME_MODERN_DARK);
        scfg.window_width       = 1280;
        scfg.window_height      = 720;
        scfg.show_bookmarks_bar = true;
        scfg.private_browsing   = true;
        scfg.module_search_dir  = mod_dir;

        ArcrBrowserShell *sh = arcr_shell_create(eng, &scfg);
        seed_default_bookmarks(sh);
        arcr_shell_open_tab_html(sh, "https://www.zuzunza.com/",
                                 kZuzunzaShowcaseHtml, strlen(kZuzunzaShowcaseHtml), true);
        arcr_shell_open_tab_url(sh, "https://www.youtube.com/", false);
        arcr_shell_open_tab_url(sh, "https://www.google.com/", false);
        arcr_shell_open_tab_url(sh, "https://x.com/", false);
        arcr_shell_activate_tab(sh, 0u);

        snprintf(path, sizeof(path), "%s/waterscape_modern_sites.png", snap_dir);
        all_ok = save_shell_png(eng, sh, scfg.window_width, scfg.window_height, path) && all_ok;

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

    printf("\n=== Waterscape 2.0 Multi-Core Tile + SIMD Rasterization Benchmark ===\n");
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
#if defined(_WIN32)
    win32_maybe_attach_parent_console(argc, argv);
#endif

    char exe_dir[512];
    exe_dir[0] = '\0';
    const char *mod_dir = (arcr_platform_get_executable_dir(exe_dir, sizeof(exe_dir)) &&
                           exe_dir[0] != '\0')
                              ? exe_dir
                              : ".";

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
        if (headless) {
            arcr_shell_open_tab_url(sh, initial_url, true);
        } else {
            arcr_shell_open_tab_url_async(sh, initial_url, true);
        }
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
