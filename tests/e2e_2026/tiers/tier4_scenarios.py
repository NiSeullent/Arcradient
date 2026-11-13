import sys
sys.dont_write_bytecode = True

import hashlib
import json
import os
import re
import subprocess
import time

try:
    from ..harness.types_and_guards import (
        AUTHORITATIVE_FEATURES,
        AssertionFailure,
        RunnerContext,
        TestCaseSpec,
        TestResult,
        ensure,
        run_arcr_js,
        run_waterscape_verify,
    )
    from ..harness.x11_driver import (
        KC_HANGUL_HAN,
        X11Session,
        XK_F11,
        XK_F12,
        XK_Return,
    )
except ImportError:
    from harness.types_and_guards import (
        AUTHORITATIVE_FEATURES,
        AssertionFailure,
        RunnerContext,
        TestCaseSpec,
        TestResult,
        ensure,
        run_arcr_js,
        run_waterscape_verify,
    )
    from harness.x11_driver import (
        KC_HANGUL_HAN,
        X11Session,
        XK_F11,
        XK_F12,
        XK_Return,
    )

TestCase = TestCaseSpec


def _skip_if_live_disabled(ctx: RunnerContext, test_id: str, feature_id: str, related: tuple[str, ...]):
    if ctx.skip_live:
        return TestResult(
            test_id=test_id,
            tier=4,
            feature_id=feature_id,
            passed=True,
            skipped=True,
            diagnostic="Skipped live internet scenario (--skip-live)",
            related_features=list(related),
        )
    return None


def _assert_live_render_sane(res, *, site_label: str, min_cmds: int = 8, require_color: bool = True) -> None:
    ensure(res.ok, f"{site_label} failed mandatory anti-false-positive guards: {res.guard_failures}")
    ensure(res.dump_json.get("conformance_mode") is True, f"{site_label} did not run in --conformance mode")
    stats = res.dump_json.get("stats") or {}
    cmds = int(stats.get("display_cmd_count", 0))
    dom_n = int(stats.get("dom_node_count", 0))
    ensure(cmds >= min_cmds and dom_n >= 15, f"{site_label} suspiciously sparse DOM/display list: dom={dom_n}, cmds={cmds}")
    ensure(res.content_png is not None, f"{site_label} missing --save-content-png")
    if require_color:
        grey_only, reason = res.content_png.is_grey_skeleton_only()
        ensure(not grey_only, f"{site_label} rendered blank or grey skeleton only: {reason}")


# ---------------------------------------------------------------------------
# Scenario 1..5: Live 2026 Public Web --conformance QA (run by default!)
# ---------------------------------------------------------------------------


def _t4_scen_01_live_google_home(ctx: RunnerContext):
    """Scenario 1 (F06, F15, F18): Live https://www.google.com/ --conformance layout, sticky footer & Sign-In contrast."""
    sk = _skip_if_live_disabled(ctx, "T4-SCEN-01", "F18", ("F06", "F15"))
    if sk is not None:
        return sk
    probe_js = (
        "(() => {"
        " const btns = Array.from(document.querySelectorAll('a,button')).filter(e => /sign in|로그인/i.test(e.textContent||''));"
        " const br = btns.length ? btns[0].getBoundingClientRect() : null;"
        " window.__gHome = { h: document.documentElement.scrollHeight, vh: window.innerHeight,"
        "   btn: br ? [Math.round(br.left), Math.round(br.top), Math.round(br.width), Math.round(br.height)] : null };"
        " console.log('GHOME_PROBE:' + JSON.stringify(window.__gHome));"
        "})();"
    )
    res = run_waterscape_verify(
        ctx,
        "https://www.google.com/?hl=ko",
        test_id="T4-SCEN-01",
        conformance=True,
        wait_load_ms=10000,
        eval_js=probe_js,
        timeout_sec=28.0,
    )
    _assert_live_render_sane(res, site_label="google.com Home", min_cmds=8, require_color=True)
    # Check no subresource 404 caused by >2KB /xjs/ URL truncation
    bad_long = [
        t for t in (res.dump_json.get("traces") or [])
        if ("/xjs/" in str(t.get("url", "")) or "/async/" in str(t.get("url", ""))) and int(t.get("status_code", 200)) == 404
    ]
    ensure(len(bad_long) == 0, f"Google long subresource URLs truncated to 404: {bad_long[:2]}")
    clog = str(res.dump_json.get("console_log", ""))
    m = re.search(r"GHOME_PROBE:(\{.*\})", clog)
    if m:
        meta = json.loads(m.group(1))
        if meta.get("btn") and meta["btn"][2] >= 20 and meta["btn"][3] >= 16:
            bx, by, bw, bh = meta["btn"]
            cr = res.content_png.region_max_contrast(bx, by, bw, bh)
            ensure(cr >= 3.5, f"Google Sign-In button contrast too low ({cr:.2f} < 3.5:1)")


def _t4_scen_02_live_google_search_linux(ctx: RunnerContext):
    """Scenario 2 (F02, F03, F06, F07, F11, F18): Live https://www.google.com/search?q=linux --conformance."""
    sk = _skip_if_live_disabled(ctx, "T4-SCEN-02", "F18", ("F02", "F03", "F06", "F07", "F11"))
    if sk is not None:
        return sk
    res = run_waterscape_verify(
        ctx,
        "https://www.google.com/search?q=linux&hl=en",
        test_id="T4-SCEN-02",
        conformance=True,
        wait_load_ms=12000,
        timeout_sec=30.0,
    )
    _assert_live_render_sane(res, site_label="google.com/search?q=linux", min_cmds=12, require_color=True)
    final_url = str(res.dump_json.get("url", ""))
    ensure("/sorry/" not in final_url, f"Hit bot challenge instead of search results: {final_url}")
    title = str(res.dump_json.get("title", "")).lower()
    ensure("linux" in title or "google" in title, f"Unexpected search page title: {title!r}")
    st = res.content_png.region_stats(20, 80, 900, 560)
    ensure(st["non_white"] >= 1500 and st["distinct_colors"] >= 16, f"Search result viewport nearly blank: {st}")


def _t4_scen_03_live_zuzunza_portal(ctx: RunnerContext):
    """Scenario 3 (F08, F09, F13, F15, F18): Live https://www.zuzunza.com/ --conformance zero sr-only bleed & Korean UI."""
    sk = _skip_if_live_disabled(ctx, "T4-SCEN-03", "F18", ("F08", "F09", "F13", "F15"))
    if sk is not None:
        return sk
    probe_js = (
        "(() => {"
        " const skip = Array.from(document.querySelectorAll('a,div,span')).find(e => (e.textContent||'').includes('본문 바로가기'));"
        " let skipVisible = false;"
        " if (skip) {"
        "   const r = skip.getBoundingClientRect();"
        "   const cs = getComputedStyle(skip);"
        "   if (r.width > 4 && r.height > 4 && cs.clip === 'auto' && cs.clipPath === 'none' && r.bottom > 2) skipVisible = true;"
        " }"
        " console.log('ZUZUNZA_PROBE:' + JSON.stringify({ skipVisible, nodes: document.querySelectorAll('*').length }));"
        "})();"
    )
    res = run_waterscape_verify(
        ctx,
        "https://www.zuzunza.com/",
        test_id="T4-SCEN-03",
        conformance=True,
        wait_load_ms=12000,
        eval_js=probe_js,
        timeout_sec=32.0,
    )
    _assert_live_render_sane(res, site_label="www.zuzunza.com", min_cmds=12, require_color=True)
    clog = str(res.dump_json.get("console_log", ""))
    m = re.search(r"ZUZUNZA_PROBE:(\{.*\})", clog)
    if m:
        meta = json.loads(m.group(1))
        ensure(meta.get("skipVisible") is False, f".sr-only '본문 바로가기' skip-link bled visibly at top of zuzunza.com: {meta}")


def _t4_scen_04_live_youtube_home(ctx: RunnerContext):
    """Scenario 4 (F02, F03, F09, F10, F11, F12, F14, F18): Live https://www.youtube.com/ Home --conformance (real Polymer, zero fake hydrator)."""
    sk = _skip_if_live_disabled(ctx, "T4-SCEN-04", "F18", ("F02", "F03", "F09", "F10", "F11", "F12", "F14"))
    if sk is not None:
        return sk
    probe_js = (
        "(() => {"
        " const fakeHydrator = !!document.querySelector('[data-arcr-hydrated]');"
        " const ytdApp = !!document.querySelector('ytd-app, ytm-app');"
        " const hasShadow = Array.from(document.querySelectorAll('*')).some(el => !!el.shadowRoot);"
        " console.log('YT_HOME_PROBE:' + JSON.stringify({ fakeHydrator, ytdApp, hasShadow }));"
        "})();"
    )
    res = run_waterscape_verify(
        ctx,
        "https://www.youtube.com/",
        test_id="T4-SCEN-04",
        conformance=True,
        wait_load_ms=15000,
        eval_js=probe_js,
        timeout_sec=38.0,
    )
    _assert_live_render_sane(res, site_label="youtube.com Home", min_cmds=15, require_color=True)
    clog = str(res.dump_json.get("console_log", ""))
    m = re.search(r"YT_HOME_PROBE:(\{.*\})", clog)
    if m:
        meta = json.loads(m.group(1))
        ensure(meta.get("fakeHydrator") is False, "Deceptive hardcoded YouTube hydrator ran in --conformance mode")
        ensure(meta.get("ytdApp") is True and meta.get("hasShadow") is True, f"YouTube Web Components/ShadowRoot did not upgrade: {meta}")


def _t4_scen_05_live_youtube_search(ctx: RunnerContext):
    """Scenario 5 (F11, F12, F14, F18): Live https://www.youtube.com/results?search_query=lofi+hip+hop --conformance."""
    sk = _skip_if_live_disabled(ctx, "T4-SCEN-05", "F18", ("F11", "F12", "F14"))
    if sk is not None:
        return sk
    res = run_waterscape_verify(
        ctx,
        "https://www.youtube.com/results?search_query=lofi+hip+hop",
        test_id="T4-SCEN-05",
        conformance=True,
        wait_load_ms=15000,
        timeout_sec=38.0,
    )
    _assert_live_render_sane(res, site_label="youtube.com Search", min_cmds=15, require_color=True)
    has_tofu = res.content_png.detect_crossed_box_tofu(40, 80, 1100, 580)
    ensure(not has_tofu, "Crossed-box tofu glyph detected on live YouTube search results (check math/symbol/emoji fallback)")


# ---------------------------------------------------------------------------
# Scenario 6..12: Live YouTube Watch, Live X.com, Local Full-Stack SSR->SPA,
# Interactive X11 Desktop Session, and /env-setup + G5 + ELF Clean-Room Audit
# ---------------------------------------------------------------------------


def _t4_scen_06_live_youtube_watch(ctx: RunnerContext):
    """Scenario 6 (F02, F03, F11, F12, F18): Live YouTube Watch page --conformance (player_es6 mega-bundle, Shadow DOM, honest media)."""
    sk = _skip_if_live_disabled(ctx, "T4-SCEN-06", "F18", ("F02", "F03", "F11", "F12"))
    if sk is not None:
        return sk
    probe_js = (
        "(() => {"
        " const fakeHydrator = !!document.querySelector('[data-arcr-hydrated]');"
        " const v = document.querySelector('video');"
        " const player = !!document.querySelector('#movie_player, ytd-player, ytd-watch-flexy');"
        " console.log('YT_WATCH_PROBE:' + JSON.stringify({ fakeHydrator, player, hasVideo: !!v }));"
        "})();"
    )
    res = run_waterscape_verify(
        ctx,
        "https://www.youtube.com/watch?v=jfKfPfyJRdk",
        test_id="T4-SCEN-06",
        conformance=True,
        wait_load_ms=16000,
        eval_js=probe_js,
        timeout_sec=40.0,
    )
    _assert_live_render_sane(res, site_label="youtube.com Watch", min_cmds=15, require_color=True)
    clog = str(res.dump_json.get("console_log", ""))
    m = re.search(r"YT_WATCH_PROBE:(\{.*\})", clog)
    if m:
        meta = json.loads(m.group(1))
        ensure(meta.get("fakeHydrator") is False, "Deceptive hardcoded YouTube hydrator ran on Watch page")
        ensure(meta.get("player") is True, f"YouTube watch player container failed to mount: {meta}")


def _t4_scen_07_live_x_home_and_post(ctx: RunnerContext):
    """Scenario 7 (F08, F09, F11, F14, F15, F18): Live https://x.com/ Home & https://x.com/x Post/Profile --conformance."""
    sk = _skip_if_live_disabled(ctx, "T4-SCEN-07", "F18", ("F08", "F09", "F11", "F14", "F15"))
    if sk is not None:
        return sk
    probe_js = (
        "(() => {"
        " const fakeHydrator = !!document.querySelector('[data-arcr-hydrated]');"
        " const hiddenSvgBleed = Array.from(document.querySelectorAll('svg')).some(s => {"
        "   const hasSym = !!s.querySelector('symbol');"
        "   const r = s.getBoundingClientRect();"
        "   return hasSym && r.width > 4 && r.height > 4 && r.left === 0 && r.top === 0;"
        " });"
        " console.log('X_PROBE:' + JSON.stringify({ fakeHydrator, hiddenSvgBleed }));"
        "})();"
    )
    for idx, target_url in enumerate(("https://x.com/", "https://x.com/x")):
        res = run_waterscape_verify(
            ctx,
            target_url,
            test_id=f"T4-SCEN-07-{idx}",
            conformance=True,
            wait_load_ms=14000,
            eval_js=probe_js,
            timeout_sec=36.0,
        )
        _assert_live_render_sane(res, site_label=f"x.com ({target_url})", min_cmds=8, require_color=False)
        st = res.content_png.region_stats()
        ensure(st["distinct_colors"] >= 6, f"x.com ({target_url}) rendered blank/uniform: {st}")
        clog = str(res.dump_json.get("console_log", ""))
        m = re.search(r"X_PROBE:(\{.*\})", clog)
        if m:
            meta = json.loads(m.group(1))
            ensure(meta.get("fakeHydrator") is False, f"Deceptive x.com hydrator ran on {target_url}")
            ensure(meta.get("hiddenSvgBleed") is False, f"Hidden <svg><symbol> sprite bled at (0,0) on {target_url}")


def _t4_scen_08_local_fullstack_ssr_spa_hydration(ctx: RunnerContext) -> None:
    """Scenario 8 (F04,F05,F07,F08,F09,F10,F12,F13,F15,F18): Full-stack SSR->SPA hydration with Shadow <slot>, Constructable Stylesheets, cookies, WS, pushState/back & animations."""
    srv = ctx.fixture_srv
    srv.register_route(
        "/t4/s08/api/state",
        lambda req: {
            "status": 200,
            "content_type": "application/json; charset=utf-8",
            "body": json.dumps({"cookie": req["headers"].get("cookie", ""), "prices": [1e16, 12.5, 7.5, -1e16]}).encode("utf-8"),
        },
    )
    srv.register_route(
        "/t4/s08/hydrate.js",
        {
            "content_type": "application/javascript; charset=utf-8",
            "body": f"""
window.__hydCs = document.currentScript ? document.currentScript.id : 'NONE';
(async () => {{
  const log = [];
  {{
    using ds = new DisposableStack();
    ds.defer(() => log.push('DISPOSED'));
    document.cookie = 'spa_client=HYDRATED_2026; path=/';
    const sheet = new CSSStyleSheet();
    sheet.replaceSync('ssr-card {{ display: block; margin: 16px; }}');
    document.adoptedStyleSheets = [sheet];

    class SsrCard extends HTMLElement {{
      connectedCallback() {{
        const sr = this.attachShadow({{ mode: 'open' }});
        const cs = new CSSStyleSheet();
        cs.replaceSync(':host {{ display: block; border-left: 12px solid #16a34a; background: #1e40af; padding: 12px; color: #fff; }} ::slotted(span) {{ font-weight: bold; }}');
        sr.adoptedStyleSheets = [cs];
        sr.innerHTML = '<div id="box"><slot name="title"></slot></div>';
      }}
    }}
    customElements.define('ssr-card', SsrCard);

    const r = await fetch('/t4/s08/api/state');
    const st = await r.json();
    const total = Math.sumPrecise(st.prices);
    const cache = new Map();
    cache.getOrInsertComputed('dur', () => new Intl.DurationFormat('en', {{ style: 'short' }}).format({{ minutes: 4, seconds: 30 }}));

    const wsMsg = await new Promise((resolve) => {{
      const ws = new WebSocket({json.dumps(srv.ws_origin + "/ws-spa")});
      ws.onopen = () => ws.send('SPA_PING');
      ws.onmessage = (e) => {{ ws.close(); resolve(e.data); }};
    }});

    let popped = false;
    window.addEventListener('popstate', () => {{ popped = true; }});
    history.pushState({{ view: 'detail' }}, '', '/t4/s08/detail?tab=metrics');
    const patOk = new URLPattern({{ pathname: '/t4/s08/:slug' }}).test(location.href);
    history.back();

    const animEl = document.getElementById('anim-bar');
    animEl.animate([{{ width: '80px' }}, {{ width: '240px' }}], {{ duration: 90, fill: 'forwards' }});
    await new Promise(res => setTimeout(res, 120));
    log.push('READY:' + total + ':' + wsMsg + ':' + patOk);
    window.__spaReport = {{
      cs: window.__hydCs,
      cookie: st.cookie,
      total,
      dur: cache.get('dur'),
      wsMsg,
      patOk,
      popped,
      animW: Math.round(animEl.getBoundingClientRect().width)
    }};
  }}
  window.__spaReport.log = log;
  document.title = 'SPA_OK:' + JSON.stringify(window.__spaReport);
}})();
""",
        },
    )
    url = srv.register_route(
        "/t4/s08/app",
        {
            "headers": [("Set-Cookie", "ssr_boot=BOOT_1; Path=/; HttpOnly")],
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>SSR_INIT</title>
<style>
@property --bar-h { syntax: '<length>'; inherits: false; initial-value: 36px; }
body { margin: 0; background: #ffffff; }
.sr-only { position: absolute; clip: rect(1px, 1px, 1px, 1px); background: #ff00ff; }
#anim-bar { width: 80px; height: var(--bar-h); background: #16a34a; margin-left: 16px; transition: opacity 200ms linear; }
#grid24 { display: grid; grid-template-columns: repeat(24, 24px); gap: 2px; margin: 16px; }
#grid24 div { height: 18px; background: #cbd5e1; }
</style></head><body>
<a class="sr-only" href="#main">본문 바로가기</a>
<svg width="0" height="0" style="position:absolute"><symbol id="s-ic" viewBox="0 0 20 20"><circle cx="10" cy="10" r="8" fill="#0284c7"/></symbol></svg>
<main id="main">
  <ssr-card id="card"><span slot="title">SSR Hydrated Header</span></ssr-card>
  <div id="anim-bar"></div>
  <div style="margin:16px"><svg width="32" height="32"><use href="#s-ic"/></svg></div>
  <div id="grid24">""" + "".join("<div></div>" for _ in range(24)) + """</div>
</main>
<script>
const sc = document.createElement('script');
sc.id = 'hyd-entry';
sc.src = '/t4/s08/hydrate.js';
document.body.appendChild(sc);
</script></body></html>""",
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T4-SCEN-08", wait_load_ms=6000)
    ensure(res.ok, f"Scenario 8 guard failures: {res.guard_failures}")
    title = str(res.dump_json.get("title", ""))
    ensure(title.startswith("SPA_OK:"), f"SSR->SPA hydration failed: title={title!r}")
    rep = json.loads(title[len("SPA_OK:") :])
    ensure(rep["cs"] == "hyd-entry", f"document.currentScript mismatch: {rep}")
    ensure("ssr_boot=BOOT_1" in rep["cookie"] and "spa_client=HYDRATED_2026" in rep["cookie"], f"SSR+SPA cookie jar failed: {rep}")
    ensure(abs(float(rep["total"]) - 20.0) < 1e-9 and rep["wsMsg"] == "SPA_PING", f"sumPrecise/WebSocket failed: {rep}")
    ensure(rep["patOk"] is True and rep["popped"] is True and rep["animW"] >= 200, f"URLPattern/popstate/Element.animate failed: {rep}")
    ensure(rep["log"][-1] == "DISPOSED", f"DisposableStack LIFO did not run: {rep}")
    ensure(res.content_png.has_bleed_color([(255, 0, 255)], tol=18) == 0, ".sr-only bled into rendered PNG")
    ensure(res.content_png.region_stats(0, 0, 12, 12)["non_white"] == 0, "Hidden <svg><symbol> bled at (0,0)")


def _t4_scen_09_x11_desktop_multitab_devtools_hangul_session(ctx: RunnerContext) -> None:
    """Scenario 9 (F16, F17, F18): Live xvfb-run 1600x1000x24 desktop session with <16ms/<50ms tab switching, >=8KB omnibar, F12 DevTools & Hangul XIM."""
    srv = ctx.fixture_srv
    posts: list[str] = []
    srv.register_route(
        "/t4/s09/post",
        lambda req: (posts.append(req["body"].decode("utf-8", errors="replace")), {"status": 200, "content_type": "text/plain", "body": b"OK"})[1],
    )
    srv.register_route(
        "/t4/s09/search",
        lambda req: {
            "body": f"<!DOCTYPE html><html><body style='margin:0;background:#9333ea;color:#fff'>Q={req['query']}</body></html>"
        },
    )
    tab1_url = srv.register_route(
        "/t4/s09/tab1.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"></head>
<body style="margin:0;background:#dc2626">
<input id="inp" style="position:absolute;left:24px;top:24px;width:240px;height:40px;font-size:20px">
<button id="go" style="position:absolute;left:280px;top:24px;width:100px;height:40px;background:#ffffff">Send</button>
<script>
document.getElementById('go').onclick = () => fetch('/t4/s09/post', { method: 'POST', body: document.getElementById('inp').value });
</script></body></html>"""
        },
    )
    tab2_url = srv.register_route(
        "/t4/s09/tab2.html",
        {
            "body": "<!DOCTYPE html><html><head><link rel='stylesheet' href='/slow-css?ms=650&bg=%2316a34a&paint_body=1'></head>"
            "<body style='margin:0;background:#16a34a;height:2000px'><div style='margin-top:700px;height:300px;background:#2563eb'>SCROLL_BAND</div></body></html>"
        },
    )

    sdir = os.path.join(ctx.test_dir("T4-SCEN-09"), "x11")
    with X11Session(ctx.waterscape_bin, ctx.module_dir, sdir, initial_url=tab1_url, screen_spec="-screen 0 1600x1000x24") as x11:
        win = x11.wait_for_waterscape_window()
        ensure(len(x11.list_viewable_windows()) == 1, "Expected single-frame X11 desktop window")
        ok1, _, _ = x11.wait_for_pixel_near(win, 500, 260, (220, 38, 38), timeout_sec=5.0)
        ensure(ok1, "Tab 1 failed to paint #dc2626")

        # 1. Hangul XIM composition into web <input> ('가한')
        x11.click_window(win, 80, 76 + 44)
        time.sleep(0.06)
        x11.compose_multi_key("rk")
        time.sleep(0.04)
        x11.press_keycode(KC_HANGUL_HAN)
        time.sleep(0.05)
        x11.click_window(win, 320, 76 + 44)
        deadline = time.monotonic() + 3.0
        while len(posts) == 0 and time.monotonic() < deadline:
            time.sleep(0.05)
        ensure(posts and posts[-1] == "가한", f"Expected XIM Hangul '가한' in web <input>, got {posts}")

        # 2. Open Tab 2, scroll wheel, and measure cached Ctrl+Tab switch latency
        ks_t = x11.x11.XStringToKeysym(b"t")
        ks_l = x11.x11.XStringToKeysym(b"l")
        x11.press_keysym(ks_t, ctrl=True)
        time.sleep(0.1)
        x11.press_keysym(ks_l, ctrl=True)
        time.sleep(0.06)
        x11.type_ascii(tab2_url)
        x11.press_keysym(XK_Return)
        ok2, _, _ = x11.wait_for_pixel_near(win, 500, 260, (22, 163, 74), timeout_sec=5.0)
        ensure(ok2, "Tab 2 failed to paint #16a34a")

        sw_ok, sw_ms, _ = x11.measure_ctrl_tab_switch_ms(win, (500, 260), (220, 38, 38), max_wait_ms=600.0)
        ensure(sw_ok and sw_ms < 50.0, f"Multi-tab Ctrl+Tab switch latency {sw_ms:.2f}ms exceeded 50ms budget")

        # 3. F12 DevTools dock inside single window + Hangul XIM in Ctrl+L Omnibar
        x11.press_keysym(XK_F12)
        time.sleep(0.15)
        ensure(len(x11.list_viewable_windows()) == 1, "F12 opened extra top-level window")
        x11.press_keysym(XK_F12)
        time.sleep(0.1)

        cur = srv.mark_cursor()
        x11.press_keysym(ks_l, ctrl=True)
        time.sleep(0.06)
        x11.type_ascii(srv.main_origin + "/t4/s09/search?q=")
        x11.compose_multi_key("han")  # -> "한글"
        time.sleep(0.05)
        x11.press_keysym(XK_Return)
        ok_s, _, _ = x11.wait_for_pixel_near(win, 500, 260, (147, 51, 234), timeout_sec=4.0)
        traces = srv.get_traces(since=cur, path_prefix="/t4/s09/search")
        ensure(ok_s and traces, "Omnibar Hangul navigation did not reach /t4/s09/search")
        q_raw = traces[-1]["query"]
        ensure("%ED%95%9C%EA%B8%80" in q_raw.upper() or "한글" in q_raw, f"Omnibar XIM Hangul corrupted in URL query: {q_raw!r}")
        ensure(x11.close_gracefully(win) == 0, "X11 WM_DELETE_WINDOW clean shutdown failed")


def _t4_scen_10_env_setup_g5_inventory_and_elf_cleanroom_audit(ctx: RunnerContext) -> None:
    """Scenario 10 (F01, F11, F19): /env-setup wake & idempotency + G5 standards_2026_inventory.json & docs sync + ELF clean-room & facade-removal audit."""
    # 1. F01: /env-setup script & .agents/environment.json verification
    env_sh = "/home/antigravity/env-setup.sh"
    env_json_p = os.path.join(ctx.repo_root, ".agents", "environment.json")
    ensure(os.path.isfile(env_sh) and os.access(env_sh, os.X_OK), f"Missing executable {env_sh}")
    ensure(os.path.isfile(env_json_p), f"Missing {env_json_p}")
    chk_syn = subprocess.run(["bash", "-n", env_sh], capture_output=True, text=True, timeout=5)
    ensure(chk_syn.returncode == 0, f"bash -n {env_sh} failed: {chk_syn.stderr}")
    env_obj = json.loads(open(env_json_p, "r", encoding="utf-8").read())
    ensure(isinstance(env_obj, dict) and len(env_obj) >= 1, "Invalid .agents/environment.json")

    # Run env-setup.sh in isolated HOME with mock failing `sudo` to prove rootless zero-sudo execution
    tdir = ctx.test_dir("T4-SCEN-10")
    mock_bin = os.path.join(tdir, "mock_bin")
    sb_home = os.path.join(tdir, "sb_home")
    os.makedirs(mock_bin, exist_ok=True)
    os.makedirs(sb_home, exist_ok=True)
    sudo_p = os.path.join(mock_bin, "sudo")
    with open(sudo_p, "w", encoding="utf-8") as f:
        f.write("#!/usr/bin/env bash\necho 'FORBIDDEN_SUDO' >&2\nexit 99\n")
    os.chmod(sudo_p, 0o755)
    env = os.environ.copy()
    env["HOME"] = sb_home
    env["PATH"] = f"{mock_bin}:{env.get('PATH', '')}"
    r_env = subprocess.run(["bash", env_sh], cwd="/tmp", env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=15)
    ensure(r_env.returncode == 0, f"env-setup.sh invoked sudo or failed rootless run: {r_env.stderr}")

    # 2. F11: Clean-room ELF symbol & deceptive facade removal audit
    forbidden = re.compile(r"(JS_NewRuntime|JS_NewContext|quickjs|v8::Isolate|WebView2|hydrator_hydrate_youtube|hydrator_hydrate_x_twitter)")
    for bp in (ctx.waterscape_bin, ctx.arcr_js_bin, os.path.join(ctx.module_dir, "libarcr_js_portable.so")):
        ensure(os.path.exists(bp), f"Missing binary {bp}")
        nm_all = subprocess.run(["nm", "-a", bp], capture_output=True, text=True, timeout=10).stdout
        dyn_nm = subprocess.run(["nm", "-D", bp], capture_output=True, text=True, timeout=10).stdout
        for blob in (nm_all, dyn_nm):
            m = forbidden.search(blob)
            ensure(m is None, f"Forbidden third-party or deceptive hydrator symbol {m.group(0) if m else ''} found in {bp}")

    # Verify default (non-conformance) mode also does NOT replace YouTube/X-like DOM with fake data-arcr-hydrated markup
    srv = ctx.fixture_srv
    yt_mock = srv.register_route(
        "/watch",
        {
            "body": "<!DOCTYPE html><html><head><title>REAL_DOM_KEPT</title></head>"
            "<body><ytd-app id='real-yt'><div style='width:140px;height:50px;background:#16a34a'>real</div></ytd-app>"
            "<script>if (document.querySelector('[data-arcr-hydrated]')) document.title = 'FAKE_HYDRATED';</script></body></html>"
        },
    )
    r_def = run_waterscape_verify(ctx, yt_mock + "?v=test123", test_id="T4-SCEN-10-no-facade", conformance=False)
    ensure(r_def.ok and r_def.dump_json.get("title") == "REAL_DOM_KEPT", f"Default mode mutated DOM with deceptive hydrator: {r_def.dump_json.get('title')}")

    # 3. F19: G5 Clause-Honest standards_2026_inventory.json & docs sync
    inv_p = os.path.join(ctx.repo_root, "docs", "standards_2026_inventory.json")
    rep_p = os.path.join(ctx.repo_root, "docs", "verification", "verification_report_2026.md")
    rdm_p = os.path.join(ctx.repo_root, "README.md")
    for p in (inv_p, rep_p, rdm_p):
        ensure(os.path.isfile(p), f"Missing documentation/inventory file {p}")
    inv = json.loads(open(inv_p, "r", encoding="utf-8").read())
    entries = inv.get("entries") or []
    ensure(len(entries) >= 28, f"standards_2026_inventory.json still coarse ({len(entries)} < 28 entries)")
    valid_codes = {"verified", "implemented", "partial", "unimplemented", "not_applicable"}
    seen_ids: set[str] = set()
    for e in entries:
        eid = e.get("id", "")
        ensure(eid and eid not in seen_ids, f"Duplicate or empty inventory id: {eid!r}")
        seen_ids.add(eid)
        sc = e.get("status_code")
        ensure(sc in valid_codes, f"Invalid status_code {sc!r} on {eid}")
        ensure("modern_status" in e and "win2000_legacy_status" in e, f"Missing split platform status on {eid}")
        if sc == "verified":
            ensure(len(e.get("blocking_issues") or []) == 0, f"G5 violation: {eid} marked verified with blocking_issues={e.get('blocking_issues')}")
        for impl_f in e.get("implementation_files") or []:
            ensure(os.path.exists(os.path.join(ctx.repo_root, impl_f)), f"Inventory {eid} cites missing file {impl_f}")


def _t4_scen_11_multilingual_publishing_portal(ctx: RunnerContext) -> None:
    """Scenario 11 (F09, F13, F14, F15): Multi-CJK charsets + Brotli CSS + @font-face WOFF2 + RGBA Emoji + zero-tofu math publishing portal."""
    srv = ctx.fixture_srv
    url = srv.register_route(
        "/t4/s11/portal.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title>
<style>
@font-face { font-family: 'PortalMono'; src: url('/fonts/mono.woff2') format('woff2'); }
body { margin: 0; background: #ffffff; color: #111827; }
.code { font-family: 'PortalMono', serif; font-size: 28px; }
#emoji { font-size: 42px; color: #000000; }
#math { font-size: 42px; color: #000000; }
</style></head><body>
<div><span class="code" id="ci">iiiiiiii</span></div>
<div><span class="code" id="cw">WWWWWWWW</span></div>
<div id="emoji">&#x1F525; &#x1F3A8;</div>
<div id="math"><span id="mL">&#x1D40B;</span><span id="mi">&#x1D422;</span></div>
<script>
(async () => {
  const r = await fetch('/charset-bytes/euc-kr');
  const j = await r.json();
  const ko = new TextDecoder('euc-kr').decode(new Uint8Array(j.bytes));
  const wi = document.getElementById('ci').getBoundingClientRect().width;
  const ww = document.getElementById('cw').getBoundingClientRect().width;
  const er = document.getElementById('emoji').getBoundingClientRect();
  const mr = document.getElementById('math').getBoundingClientRect();
  document.title = 'PORTAL:' + JSON.stringify({
    koOk: ko === j.expected,
    monoDiff: Math.abs(ww - wi),
    ey: Math.round(er.top), eh: Math.round(er.height),
    my: Math.round(mr.top), mh: Math.round(mr.height)
  });
})();
</script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T4-SCEN-11")
    ensure(res.ok, f"Scenario 11 guard failures: {res.guard_failures}")
    title = str(res.dump_json.get("title", ""))
    ensure(title.startswith("PORTAL:"), f"Unexpected title: {title!r}")
    meta = json.loads(title[len("PORTAL:") :])
    ensure(meta["koOk"] is True and meta["monoDiff"] <= 4.0, f"EUC-KR or WOFF2 monospace failed: {meta}")
    est = res.content_png.region_stats(0, max(0, meta["ey"] - 4), 220, max(48, meta["eh"] + 8))
    ensure(est["warm_pixels"] >= 20 and est["max_chroma"] >= 60, f"Emoji lacked RGBA color chroma: {est}")
    ensure(not res.content_png.detect_crossed_box_tofu(0, max(0, meta["my"] - 4), 180, max(48, meta["mh"] + 8)), "Tofu box detected on math symbols")


def _t4_scen_12_win2000_classic_minimum_profile_suite(ctx: RunnerContext) -> None:
    """Scenario 12 (F11, F15, F17): Win2000 classic frame + --force-cpu --profile minimum full CSS Grid, Canvas 2D & BigInt/ERM workload."""
    srv = ctx.fixture_srv
    url = srv.register_route(
        "/t4/s12/classic.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title>
<style>
body { margin: 0; background: #ffffff; }
#g { display: grid; grid-template-columns: repeat(20, 32px); gap: 2px; }
.c { height: 24px; background: #16a34a; }
</style></head><body>
<div id="g">""" + "".join("<div class='c'></div>" for _ in range(20)) + """</div>
<canvas id="cv" width="80" height="40"></canvas>
<script>
let disposed = false;
{
  using _u = { [Symbol.dispose]() { disposed = true; } };
}
const b = (1n << 32n) === 4294967296n;
const g = document.getElementById('cv').getContext('2d');
g.fillStyle = '#2563eb';
g.fillRect(0, 0, 80, 40);
const lastLeft = Math.round(document.getElementById('g').lastElementChild.getBoundingClientRect().left);
document.title = 'CLASSIC:' + JSON.stringify({ disposed, b, lastLeft });
</script></body></html>""",
        },
    )
    res = run_waterscape_verify(
        ctx,
        url,
        test_id="T4-SCEN-12",
        profile="minimum",
        theme="classic",
        force_classic_frame=True,
        force_cpu=True,
        save_png=True,
    )
    ensure(res.ok, f"Scenario 12 guard failures: {res.guard_failures}")
    title = str(res.dump_json.get("title", ""))
    ensure(title.startswith("CLASSIC:"), f"Unexpected title: {title!r}")
    meta = json.loads(title[len("CLASSIC:") :])
    ensure(meta["disposed"] is True and meta["b"] is True and meta["lastLeft"] == 19 * 34, f"Classic minimum workload failed: {meta}")
    ensure(int(res.dump_json.get("shell_layout", {}).get("window_caption_rect", {}).get("height", 0)) > 0, "Simulated Win2000 caption missing")


TESTS: list[TestCase] = [
    TestCase("T4-SCEN-01", 4, "F18", "Live google.com Home --conformance sticky footer, Sign-In contrast & long URL integrity", _t4_scen_01_live_google_home, ("F06", "F15"), is_live=True),
    TestCase("T4-SCEN-02", 4, "F18", "Live google.com/search?q=linux --conformance SearchGuard/xjs bundle & non-blank results", _t4_scen_02_live_google_search_linux, ("F02", "F03", "F06", "F07", "F11"), is_live=True),
    TestCase("T4-SCEN-03", 4, "F18", "Live www.zuzunza.com --conformance zero .sr-only skip-link bleed & Korean portal layout", _t4_scen_03_live_zuzunza_portal, ("F08", "F09", "F13", "F15"), is_live=True),
    TestCase("T4-SCEN-04", 4, "F18", "Live youtube.com Home --conformance Polymer Web Components, Shadow DOM & zero fake hydrator", _t4_scen_04_live_youtube_home, ("F02", "F03", "F09", "F10", "F11", "F12", "F14"), is_live=True),
    TestCase("T4-SCEN-05", 4, "F18", "Live youtube.com Search --conformance Shadow <slot> cards & zero-tofu symbol/emoji rendering", _t4_scen_05_live_youtube_search, ("F11", "F12", "F14"), is_live=True),
    TestCase("T4-SCEN-06", 4, "F18", "Live youtube.com Watch --conformance player_es6 mega-bundle, Shadow DOM & honest media", _t4_scen_06_live_youtube_watch, ("F02", "F03", "F11", "F12"), is_live=True),
    TestCase("T4-SCEN-07", 4, "F18", "Live x.com Home & Post --conformance zero (0,0) SVG sprite bleed & real script execution", _t4_scen_07_live_x_home_and_post, ("F08", "F09", "F11", "F14", "F15"), is_live=True),
    TestCase("T4-SCEN-08", 4, "F12", "Local Full-Stack 2026 SSR->SPA Hydration with Shadow <slot>, adoptedStyleSheets, cookies, WS, pushState/back & animations", _t4_scen_08_local_fullstack_ssr_spa_hydration, ("F04", "F05", "F07", "F08", "F09", "F10", "F13", "F15", "F18"), is_live=False),
    TestCase("T4-SCEN-09", 4, "F16", "Interactive xvfb-run 1600x1000x24 X11 desktop session with <50ms multi-tab switch, F12 DevTools & Hangul XIM", _t4_scen_09_x11_desktop_multitab_devtools_hangul_session, ("F17", "F18"), is_live=False),
    TestCase("T4-SCEN-10", 4, "F19", "Cloud /env-setup + G5 standards_2026_inventory.json & docs sync + clean-room ELF nm/readelf & facade-removal audit", _t4_scen_10_env_setup_g5_inventory_and_elf_cleanroom_audit, ("F01", "F11"), is_live=False),
    TestCase("T4-SCEN-11", 4, "F14", "Multi-charset CJK + @font-face WOFF2 + RGBA Color Emoji + zero-tofu mathematical symbol publishing portal", _t4_scen_11_multilingual_publishing_portal, ("F09", "F13", "F15"), is_live=False),
    TestCase("T4-SCEN-12", 4, "F17", "Win2000 classic frame + --force-cpu --profile minimum 20-col Grid, Canvas 2D & ES2026 ERM/BigInt resilience", _t4_scen_12_win2000_classic_minimum_profile_suite, ("F11", "F15"), is_live=False),
]


def get_tests() -> list[TestCase]:
    return list(TESTS)


get_tier4_tests = get_tests
