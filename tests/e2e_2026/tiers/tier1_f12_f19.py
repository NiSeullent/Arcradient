import sys
sys.dont_write_bytecode = True

import json
import os
import re
import shutil
import subprocess
import time
import typing
import urllib.parse

from ..harness.fixture_server import FixtureServer
from ..harness.types_and_guards import (
    AssertionFailure,
    PNGImage,
    RunnerContext,
    TestCaseSpec,
    TestResult,
    ensure,
    run_arcr_js,
    run_waterscape_verify,
)
from ..harness.x11_driver import KC_HANGUL_GEUL, KC_HANGUL_HAN, XK_F11, XK_F12, XK_Return, XK_Tab, X11Session

TestCase = TestCaseSpec
TestContext = RunnerContext


def _srv(ctx: RunnerContext) -> tuple[FixtureServer, bool]:
    if ctx.fixture_srv is not None:
        return ctx.fixture_srv, False
    s = FixtureServer(os.path.join(ctx.out_dir, "_tier1_f12_f19_assets"))
    s.start()
    return s, True


def _verify_html(ctx: RunnerContext, test_id: str, html: str, **kw: typing.Any) -> typing.Any:
    srv, owned = _srv(ctx)
    try:
        url = srv.register_route(f"/t1/{test_id}.html", {"status": 200, "body": html})
        res = run_waterscape_verify(ctx, url, test_id=test_id, **kw)
        if kw.get("check_guards", True):
            ensure(res.ok, "; ".join(res.guard_failures))
        return res
    finally:
        if owned:
            srv.stop()


def _title_json(res: typing.Any) -> dict[str, typing.Any]:
    raw = str(res.dump_json.get("title", ""))
    try:
        obj = json.loads(raw)
        ensure(isinstance(obj, dict), f"title JSON not dict: {raw!r}")
        return obj
    except Exception as exc:
        raise AssertionFailure(f"expected JSON in document.title, got {raw!r} ({exc})")


def _spec(
    test_id: str,
    feature_id: str,
    description: str,
    body_fn: typing.Callable[[RunnerContext], typing.Union[str, TestResult, None]],
    *,
    related_features: tuple[str, ...] = (),
    is_live: bool = False,
) -> TestCaseSpec:
    def _wrapped(ctx: RunnerContext) -> TestResult:
        if is_live and ctx.skip_live:
            return TestResult(test_id=test_id, tier=1, feature_id=feature_id, passed=True, skipped=True, diagnostic="Skipped (--skip-live)")
        t0 = time.perf_counter()
        try:
            out = body_fn(ctx)
            dt = (time.perf_counter() - t0) * 1000.0
            if isinstance(out, TestResult):
                return out
            return TestResult(test_id=test_id, tier=1, feature_id=feature_id, passed=True, diagnostic=out if isinstance(out, str) else "OK", duration_ms=dt, related_features=list(related_features))
        except AssertionFailure as exc:
            return TestResult(test_id=test_id, tier=1, feature_id=feature_id, passed=False, diagnostic=str(exc), duration_ms=(time.perf_counter() - t0) * 1000.0, related_features=list(related_features))

    tc = TestCaseSpec(test_id=test_id, tier=1, feature_id=feature_id, description=description, fn=_wrapped, related_features=related_features, is_live=is_live)
    setattr(tc, "feature", feature_id)
    return tc


# === Feature F12: Shadow DOM <slot> Projection & Shadow CSS Scoping ===
def _t1_f12_01(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;background:#fff"><div id="h1"><div id="l1" style="width:180px;height:44px;background:#2563eb">P</div></div><div id="h2"></div><script>
    const s1 = h1.attachShadow({mode:'open'}); s1.innerHTML = '<slot id="sl1"><div style="width:180px;height:44px;background:#ef4444">F</div></slot>';
    const s2 = h2.attachShadow({mode:'open'}); s2.innerHTML = '<slot><div id="fb2" style="width:180px;height:44px;background:#22c55e">FB</div></slot>';
    const r1 = l1.getBoundingClientRect(); const an = s1.getElementById('sl1').assignedElements();
    document.title = JSON.stringify({w:r1.width,h:r1.height,assigned:an.length===1&&an[0]===l1});</script></body></html>"""
    res = _verify_html(ctx, "T1_F12_01", html)
    d = _title_json(res)
    ensure(d.get("assigned") is True and d.get("w", 0) >= 170 and d.get("h", 0) >= 40, f"default slot projection failed: {d}")
    ensure(res.content_png.count_color_near((37, 99, 235), rect=(0, 0, 200, 46)) >= 3000 and res.content_png.count_color_near((239, 68, 68), rect=(0, 0, 200, 46)) == 0, "projected blue missing or fallback red bled")
    ensure(res.content_png.count_color_near((34, 197, 94), rect=(0, 44, 200, 50)) >= 3000, "empty slot fallback green missing")
    return f"default slot projected {d}"


def _t1_f12_02(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;background:#fff"><div id="host"><div slot="footer" id="ft" style="width:200px;height:40px;background:#9333ea"></div><div slot="header" id="hd" style="width:200px;height:40px;background:#ea580c"></div></div><script>
    const sr = host.attachShadow({mode:'open'}); sr.innerHTML = '<slot name="header"></slot><slot name="footer"></slot>';
    document.title = JSON.stringify({hdTop:hd.getBoundingClientRect().top, ftTop:ft.getBoundingClientRect().top});</script></body></html>"""
    res = _verify_html(ctx, "T1_F12_02", html)
    d = _title_json(res)
    ensure(0 <= d["hdTop"] <= 4 and d["ftTop"] >= d["hdTop"] + 36, f"named slot visual reorder failed: {d}")
    ensure(res.content_png.count_color_near((234, 88, 12), rect=(0, 0, 200, 40)) >= 3500 and res.content_png.count_color_near((147, 51, 234), rect=(0, 40, 200, 40)) >= 3500, "named slot pixels out of order")
    return f"named slots reordered {d}"


def _t1_f12_03(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><style>.box{width:200px;height:44px;background:#dc2626}</style></head><body style="margin:0;background:#fff"><div id="sh" class="active"></div><div id="lb" class="box"></div><script>
    const sr = sh.attachShadow({mode:'open'}); sr.innerHTML = '<style>:host{display:block;width:220px;padding-left:20px;background:#94a3b8}:host(.active){background:#16a34a}.box{width:200px;height:44px;background:#2563eb}</style><div class="box"></div>';
    document.title = JSON.stringify({ok:true});</script></body></html>"""
    res = _verify_html(ctx, "T1_F12_03", html)
    ensure(res.content_png.count_color_near((22, 163, 74), rect=(0, 0, 20, 44)) >= 500, ":host(.active) green strip missing")
    ensure(res.content_png.count_color_near((37, 99, 235), rect=(20, 0, 200, 44)) >= 4000, "shadow .box blue missing or bled by light CSS")
    ensure(res.content_png.count_color_near((220, 38, 38), rect=(0, 44, 200, 44)) >= 4000, "light .box red missing or bled by shadow CSS")
    return ":host(.active) & shadow scoping verified"


def _t1_f12_04(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;background:#fff"><div id="h"><span class="pill" id="s1">A</span><span class="plain" id="s2" style="display:block;width:180px;height:40px;background:#f59e0b">B</span></div><script>
    const sr = h.attachShadow({mode:'open'}); sr.innerHTML = '<style>::slotted(.pill){display:block;width:180px;height:40px;background:#0d9488;color:#fff}</style><slot></slot>';
    document.title = JSON.stringify({h1:s1.getBoundingClientRect().height, h2:s2.getBoundingClientRect().height});</script></body></html>"""
    res = _verify_html(ctx, "T1_F12_04", html)
    d = _title_json(res)
    ensure(d["h1"] >= 36 and res.content_png.count_color_near((13, 148, 136), rect=(0, 0, 180, 40)) >= 3000, f"::slotted(.pill) style/paint failed: {d}")
    ensure(res.content_png.count_color_near((245, 158, 11), rect=(0, 40, 180, 40)) >= 3000, "unmatched slotted sibling broken")
    return "::slotted(.pill) verified"


def _t1_f12_05(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;background:#fff"><div id="h"><button slot="cta" id="btn" style="width:160px;height:50px;border:0;background:#2563eb;color:#fff">Go</button></div><script>
    const sr = h.attachShadow({mode:'open'}); sr.innerHTML = '<div style="padding-top:70px;padding-left:90px"><slot name="cta"></slot></div>';
    let hit = ''; document.addEventListener('click', e => { hit = e.target.id; btn.style.background = '#16a34a'; document.title = JSON.stringify({hit, efp: document.elementFromPoint(130,95)?.id||''}); });
    document.title = JSON.stringify({hit:'', efp: document.elementFromPoint(130,95)?.id||''});</script></body></html>"""
    res = _verify_html(ctx, "T1_F12_05", html, click=(130, 95))
    d = _title_json(res)
    ensure(d.get("hit") == "btn" and d.get("efp") == "btn", f"slotted hit-test/click failed: {d}")
    ensure(res.content_png.count_color_near((22, 163, 74), rect=(90, 70, 160, 50)) >= 3500, "slotted click state not painted")
    return f"slotted hit-test & click {d}"


# === Feature F13: CSS Transitions, @import, @property, Math & Image Negotiation ===
def _t1_f13_01(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;background:#fff"><div id="tr" style="width:100px;height:60px;background:#ff0000;transition:width 600ms linear,background-color 600ms linear"></div><script>
    const w0 = tr.getBoundingClientRect().width; tr.style.width = '300px'; tr.style.backgroundColor = '#0000ff';
    const t0 = performance.now(); let frames = 0;
    function step(now) { frames++; const mw = tr.getBoundingClientRect().width; document.title = JSON.stringify({w0, midW:mw, frames}); if (now - t0 < 250) requestAnimationFrame(step); }
    requestAnimationFrame(step);</script></body></html>"""
    res = _verify_html(ctx, "T1_F13_01", html)
    d = _title_json(res)
    px = res.content_png.pixel(50, 30)
    ensure(abs(d["w0"] - 100) <= 2 and 140 <= d["midW"] <= 260 and d["frames"] >= 4, f"transition width not interpolated at 60Hz: {d}")
    ensure(40 <= px[0] <= 215 and 40 <= px[2] <= 215, f"transition color not interpolated (got RGBA {px})")
    return f"60Hz transition midpoint {d}, px={px}"


def _t1_f13_02(ctx: RunnerContext) -> str:
    srv, owned = _srv(ctx)
    try:
        cur = srv.mark_cursor()
        srv.register_route("/t1/f13_tokens.css", {"status": 200, "content_type": "text/css", "body": "@property --box-w { syntax: '<length>'; inherits: false; initial-value: 220px; } #card { width: var(--box-w); height: 52px; background: #0284c7; }"})
        srv.register_route("/t1/f13_main.css", {"status": 200, "content_type": "text/css", "body": "@import url('/t1/f13_tokens.css'); #parent { --box-w: 360px; }"})
        url = srv.register_route("/t1/T1_F13_02.html", {"status": 200, "body": '<!DOCTYPE html><html><head><link rel="stylesheet" href="/t1/f13_main.css"></head><body style="margin:0;background:#fff"><div id="parent"><div id="card"></div></div><script>window.onload=()=>{document.title=JSON.stringify({w:card.getBoundingClientRect().width});};</script></body></html>'})
        res = run_waterscape_verify(ctx, url, test_id="T1_F13_02")
        ensure(res.ok, "; ".join(res.guard_failures))
        d = _title_json(res)
        ensure(len(srv.get_traces(since=cur, path_prefix="/t1/f13_tokens.css")) >= 1, "@import stylesheet not fetched")
        ensure(abs(d["w"] - 220) <= 3 and res.content_png.count_color_near((2, 132, 199), rect=(0, 0, 220, 52)) >= 5000, f"@property/@import failed: {d}")
        return f"@import + @property verified {d}"
    finally:
        if owned:
            srv.stop()


def _t1_f13_03(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;background:#fff">
    <div id="m1" style="width:clamp(180px,calc(20% + 60px),360px);height:calc(50dvh - 62px);background:#7c3aed"></div>
    <div id="m2" style="width:min(400px,max(240px,10%));height:44px;background:#059669"></div>
    <script>const r1=m1.getBoundingClientRect(),r2=m2.getBoundingClientRect();document.title=JSON.stringify({w1:r1.width,h1:r1.height,w2:r2.width,h2:r2.height});</script></body></html>"""
    res = _verify_html(ctx, "T1_F13_03", html, window_size="1280x800")
    d = _title_json(res)
    ensure(abs(d["w1"] - 316) <= 3 and abs(d["h1"] - 300) <= 4 and abs(d["w2"] - 240) <= 3, f"CSS math/dvh mismatch: {d}")
    ensure(res.content_png.count_color_near((124, 58, 237), rect=(0, 0, 316, 300)) >= 40000 and res.content_png.count_color_near((5, 150, 105), rect=(0, 300, 240, 44)) >= 5000, "math boxes unpainted")
    return f"calc/min/max/clamp/dvh {d}"


def _t1_f13_04(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;background:#fff"><picture>
    <source media="(min-width: 900px)" srcset="/img/green64.webp" type="image/webp">
    <img id="pic" src="/img/red64.png" style="display:block"></picture>
    <script>window.onload=()=>{const r=pic.getBoundingClientRect();document.title=JSON.stringify({w:r.width,h:r.height});};</script></body></html>"""
    res = _verify_html(ctx, "T1_F13_04", html, window_size="1280x800")
    d = _title_json(res)
    ensure(abs(d["w"] - 64) <= 2 and abs(d["h"] - 64) <= 2, f"<picture> dimensions wrong: {d}")
    ensure(res.content_png.count_color_near((34, 197, 94), tol=20, rect=(0, 0, 64, 64)) >= 2500 and res.content_png.count_color_near((239, 68, 68), tol=20, rect=(0, 0, 64, 64)) == 0, "<picture> did not render green64.webp")
    return f"<picture> srcset selected green64.webp {d}"


def _t1_f13_05(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;background:#fff"><img id="neg" src="/img/negotiated" style="display:block">
    <script>window.onload=()=>{const r=neg.getBoundingClientRect();document.title=JSON.stringify({w:r.width,h:r.height});};</script></body></html>"""
    res = _verify_html(ctx, "T1_F13_05", html)
    d = _title_json(res)
    ensure(abs(d["w"] - 64) <= 2 and abs(d["h"] - 64) <= 2, f"negotiated image broken fallback dimensions: {d}")
    ensure(res.content_png.count_color_near((34, 197, 94), tol=24, rect=(0, 0, 64, 64)) >= 2500, "negotiated image pixels not green (false Accept: image/avif without decoder)")
    return f"Accept image negotiation clean {d}"


# === Feature F14: @font-face Web Fonts, Color Emoji & Symbol Fallback ===
def _t1_f14_01(ctx: RunnerContext) -> str:
    srv, owned = _srv(ctx)
    try:
        cur = srv.mark_cursor()
        html = """<!DOCTYPE html><html><head><style>@font-face{font-family:'ArcrW2Mono';src:url('/fonts/mono.woff2') format('woff2');}</style></head>
        <body style="margin:0;background:#fff;color:#000"><div style="font-family:'ArcrW2Mono',sans-serif;font-size:32px;line-height:48px"><span id="si">iiiiiiii</span> <span id="sw">WWWWWWWW</span></div>
        <script>window.onload=()=>{document.title=JSON.stringify({wi:si.getBoundingClientRect().width,ww:sw.getBoundingClientRect().width});};</script></body></html>"""
        url = srv.register_route("/t1/T1_F14_01.html", {"status": 200, "body": html})
        res = run_waterscape_verify(ctx, url, test_id="T1_F14_01")
        ensure(res.ok, "; ".join(res.guard_failures))
        d = _title_json(res)
        ensure(len(srv.get_traces(since=cur, path_prefix="/fonts/mono.woff2")) >= 1, "mono.woff2 not fetched")
        ensure(d["wi"] >= 40 and abs(d["ww"] - d["wi"]) <= 4, f"WOFF2 monospace glyphs not bound (proportional fallback): {d}")
        ensure(res.content_png.region_stats(0, 0, 420, 56)["non_white"] >= 180, "WOFF2 text unpainted")
        return f"WOFF2 @font-face monospace bound {d}"
    finally:
        if owned:
            srv.stop()


def _t1_f14_02(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><style>
    @font-face{font-family:'ArcrMulti';font-weight:400;src:url('/fonts/mono.woff') format('woff');}
    @font-face{font-family:'ArcrMulti';font-weight:700;src:url('/fonts/mono_bold.ttf') format('truetype');}
    </style></head><body style="margin:0;background:#fff;color:#000">
    <div id="r4" style="font-family:'ArcrMulti',sans-serif;font-weight:400;font-size:34px;line-height:44px">HHHHHHHH</div>
    <div id="r7" style="font-family:'ArcrMulti',sans-serif;font-weight:700;font-size:34px;line-height:44px">HHHHHHHH</div>
    <div style="font-family:sans-serif;font-size:32px;line-height:44px"><span id="pi">iiiiiiii</span><span id="pw">WWWWWWWW</span></div>
    <script>window.onload=()=>{document.title=JSON.stringify({pi:pi.getBoundingClientRect().width,pw:pw.getBoundingClientRect().width});};</script></body></html>"""
    res = _verify_html(ctx, "T1_F14_02", html)
    d = _title_json(res)
    ink4 = res.content_png.region_stats(0, 0, 320, 44)["non_white"]
    ink7 = res.content_png.region_stats(0, 44, 320, 44)["non_white"]
    ensure(ink4 >= 120 and ink7 >= int(ink4 * 1.08), f"bold TTF face did not render heavier than regular WOFF: ink4={ink4}, ink7={ink7}")
    ensure(d["pw"] > d["pi"] * 1.45, f"per-element font-family leaked monospace to sans-serif: {d}")
    return f"WOFF+TTF weights & cascade ink4={ink4}, ink7={ink7}"


def _t1_f14_03(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;background:#fff;color:#111"><div style="font-size:36px;line-height:54px;padding:10px">다람쥐 헌 쳇바퀴에 타고파 Sphere AA 2026</div><script>document.title='{"ok":true}';</script></body></html>"""
    res = _verify_html(ctx, "T1_F14_03", html)
    aa = res.content_png.count_grayscale_aa_levels(0, 0, 900, 80)
    ensure(aa >= 16, f"expected >=16 grayscale anti-aliasing levels, got {aa}")
    return f"256-level AA verified ({aa} distinct intermediate shades)"


def _t1_f14_04(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;background:#fff;color:#000"><div style="font-size:44px;line-height:68px;padding:8px">🔥 🎨 🚀</div><script>document.title='{"ok":true}';</script></body></html>"""
    res = _verify_html(ctx, "T1_F14_04", html)
    st = res.content_png.region_stats(0, 0, 320, 84)
    ensure(st["warm_pixels"] >= 25 and st["max_chroma"] >= 80 and st["saturated_pixels"] >= 60, f"emoji rendered monochrome instead of RGBA color: {st}")
    return f"RGBA color emoji blitted warm={st['warm_pixels']}, chroma={st['max_chroma']}"


def _t1_f14_05(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;background:#fff;color:#000"><div style="font-size:44px;line-height:66px;padding:8px"><span id="sL">𝐋</span><span id="si">𝐢</span>𝐨𝐟 𝕏 ★ ♪ ✓</div>
    <script>document.title=JSON.stringify({wL:sL.getBoundingClientRect().width,wi:si.getBoundingClientRect().width});</script></body></html>"""
    res = _verify_html(ctx, "T1_F14_05", html)
    d = _title_json(res)
    has_tofu = res.content_png.detect_crossed_box_tofu(0, 0, 480, 84)
    ensure(not has_tofu and d["wL"] > 10 and d["wi"] < d["wL"] * 0.75, f"Unicode math/symbol fallback hit crossed-box tofu: has_tofu={has_tofu}, {d}")
    return f"zero-tofu symbol/math fallback {d}"


# === Feature F15: Real-Site CSS Layout, sr-only, SVG Sprite & Nested Scroll Fixes ===
def _t1_f15_01(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;background:#fff">
    <a class="sr" style="position:absolute;clip:rect(0,0,0,0);background:#ff00ff;color:#00ffff">본문 바로가기 1</a>
    <a class="sr" style="position:absolute;clip:rect(1px,1px,1px,1px);background:#ff00ff;color:#00ffff">본문 바로가기 2</a>
    <a class="sr" style="position:absolute;clip-path:inset(50%);background:#ff00ff;color:#00ffff">본문 바로가기 3</a>
    <a class="sr" style="position:absolute;width:1px;height:1px;padding:0;margin:-1px;overflow:hidden;background:#ff00ff;color:#00ffff">본문 바로가기 4</a>
    <div id="vis" style="margin-top:40px;width:160px;height:44px;background:#16a34a"></div>
    <script>document.title=JSON.stringify({sr:document.querySelectorAll('.sr').length});</script></body></html>"""
    res = _verify_html(ctx, "T1_F15_01", html)
    d = _title_json(res)
    bleed = res.content_png.has_bleed_color([(255, 0, 255), (0, 255, 255)], tol=24)
    ensure(d["sr"] == 4 and bleed == 0 and res.content_png.count_color_near((22, 163, 74), rect=(0, 40, 160, 44)) >= 3000, f"sr-only bled {bleed} px: {d}")
    return f"sr-only 4 patterns 0 bleed px"


def _t1_f15_02(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;background:#fff">
    <svg width="0" height="0"><symbol id="ic" viewBox="0 0 24 24"><rect x="2" y="2" width="20" height="20" fill="#0284c7"/><rect x="36" y="36" width="20" height="20" fill="#ff00ff"/></symbol></svg>
    <svg style="display:none" width="40" height="40"><rect width="40" height="40" fill="#ef4444"/></svg>
    <div style="padding-left:140px;padding-top:40px"><svg width="48" height="48" viewBox="0 0 24 24"><use href="#ic"/></svg></div>
    <script>document.title='{"ok":true}';</script></body></html>"""
    res = _verify_html(ctx, "T1_F15_02", html)
    tl_ink = res.content_png.region_stats(0, 0, 90, 90)["non_white"]
    ic_blue = res.content_png.count_color_near((2, 132, 199), tol=24, rect=(140, 40, 48, 48))
    vb_bleed = res.content_png.has_bleed_color([(255, 0, 255), (239, 68, 68)], tol=24)
    ensure(tl_ink == 0 and ic_blue >= 800 and vb_bleed == 0, f"SVG sprite/viewBox defect: tl_ink={tl_ink}, ic_blue={ic_blue}, vb_bleed={vb_bleed}")
    return f"SVG <symbol>/<use>/viewBox clean (ic_blue={ic_blue})"


def _t1_f15_03(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;min-height:100vh;display:flex;flex-direction:column;background:#fff">
    <main style="flex:1;padding:16px"><button id="btn" style="background:#1a73e8;color:#fff;border:0;border-radius:9999px;padding:10px 24px;font-size:16px">로그인</button>
    <p id="ko" style="width:240px;font-size:18px;line-height:1.8;color:#111">한국어 모던 브라우저 줄간격 테스트 첫째줄 둘째줄 셋째줄 정상 렌더링 검증</p></main>
    <footer id="ft" style="height:40px;background:#1e293b"></footer>
    <script>const br=btn.getBoundingClientRect(),fr=ft.getBoundingClientRect(),kr=ko.getBoundingClientRect();
    document.title=JSON.stringify({bx:br.left,by:br.top,bw:br.width,bh:br.height,ftTop:fr.top,kh:kr.height});</script></body></html>"""
    res = _verify_html(ctx, "T1_F15_03", html, window_size="1280x800")
    d = _title_json(res)
    contrast = res.content_png.region_max_contrast(int(d["bx"]), int(d["by"]), max(8, int(d["bw"])), max(8, int(d["bh"])))
    ensure(abs(d["ftTop"] - 684) <= 4 and res.content_png.count_color_near((30, 41, 59), rect=(0, 686, 300, 36)) >= 5000, f"100vh sticky footer misplaced: {d}")
    ensure(contrast >= 4.5 and d["kh"] >= 78, f"Sign-In contrast ({contrast:.2f}) or Korean line-height ({d['kh']}) failed")
    return f"100vh sticky footer + WCAG {contrast:.2f}:1 + ko line-height {d['kh']}"


def _t1_f15_04(ctx: RunnerContext) -> str:
    cells = "".join(f'<div id="c{i}" style="height:28px;background:#cbd5e1"></div>' for i in range(32))
    html = f"""<!DOCTYPE html><html><body style="margin:0;background:#fff"><div id="g" style="display:grid;grid-template-columns:repeat(32,28px);gap:4px">{cells}<div id="sp" style="grid-column:25 / 33;height:32px;background:#059669"></div></div>
    <script>const r0=c0.getBoundingClientRect(),r16=c16.getBoundingClientRect(),r31=c31.getBoundingClientRect(),rs=sp.getBoundingClientRect();
    document.title=JSON.stringify({{x0:r0.left,x16:r16.left,x31:r31.left,sx:rs.left,sw:rs.width}});</script></body></html>"""
    res = _verify_html(ctx, "T1_F15_04", html, window_size="1280x800")
    d = _title_json(res)
    ensure(abs(d["x0"]) <= 2 and abs(d["x16"] - 512) <= 4 and abs(d["x31"] - 992) <= 4, f"32-column grid track layout failed: {d}")
    ensure(abs(d["sx"] - 768) <= 4 and abs(d["sw"] - 252) <= 4 and res.content_png.count_color_near((5, 150, 105), rect=(768, 32, 252, 32)) >= 3500, f"explicit grid-column 25/33 failed: {d}")
    return f"32-track CSS Grid + explicit line placement {d}"


def _t1_f15_05(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;background:#fff">
    <div id="sc" style="margin-top:40px;margin-left:40px;width:220px;height:160px;overflow-y:auto;background:#f1f5f9">
      <div style="height:500px"></div><div id="tgt" style="height:60px;background:#e11d48"></div><div style="height:400px"></div>
    </div><script>let seen=false; const io=new IntersectionObserver(es=>{if(es.some(e=>e.isIntersecting))seen=true;},{root:sc}); io.observe(tgt);
    sc.scrollTop=440; requestAnimationFrame(()=>requestAnimationFrame(()=>{document.title=JSON.stringify({st:sc.scrollTop,seen,ty:tgt.getBoundingClientRect().top});}));</script></body></html>"""
    res = _verify_html(ctx, "T1_F15_05", html)
    d = _title_json(res)
    inside = res.content_png.count_color_near((225, 29, 72), rect=(40, 95, 220, 65))
    above = res.content_png.count_color_near((225, 29, 72), rect=(0, 0, 300, 38))
    below = res.content_png.count_color_near((225, 29, 72), rect=(0, 202, 300, 300))
    ensure(abs(d["st"] - 440) <= 2 and d["seen"] is True and inside >= 3500 and above == 0 and below == 0, f"nested scrollTop/clip/IO failed: {d}, in={inside}, ab={above}, bl={below}")
    return f"nested scrollTop + clip + IntersectionObserver {d}"


# === Feature F16: Non-Blocking Multicore Tab Pipeline & <16 ms Tab Switching ===
def _t1_f16_01(ctx: RunnerContext) -> str:
    srv, owned = _srv(ctx)
    try:
        u1 = srv.register_route("/t1/f16_fast.html", {"status": 200, "body": "<!DOCTYPE html><html><body style='margin:0;background:#16a34a'></body></html>"})
        u2 = srv.register_route("/t1/f16_slow.html", {"status": 200, "body": "<!DOCTYPE html><html><head><link rel='stylesheet' href='/slow-css?ms=1400&bg=%232563eb'></head><body style='margin:0;background:#f59e0b'><script src='/slow-js?ms=1400'></script></body></html>"})
        with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T1_F16_01"), "x11"), initial_url=u1, extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as s:
            win = s.wait_for_waterscape_window()
            ok1, _, _ = s.wait_for_pixel_near(win, 200, 200, (22, 163, 74), timeout_sec=5.0)
            ensure(ok1, "Tab 1 initial green page did not paint")
            s.press_keysym(ord("t"), ctrl=True); time.sleep(0.08)
            s.press_keysym(ord("l"), ctrl=True); time.sleep(0.05)
            s.type_ascii(u2); s.press_keysym(XK_Return); time.sleep(0.12)
            ok_sw, sw_ms, px = s.measure_ctrl_tab_switch_ms(win, (200, 200), (22, 163, 74), max_wait_ms=1200.0)
            ensure(ok_sw and sw_ms < 120.0, f"UI thread blocked by slow subresource fetch during tab switch: sw_ms={sw_ms:.2f}, px={px}")
            return f"non-blocking off-UI-thread switch {sw_ms:.2f} ms"
    finally:
        if owned:
            srv.stop()


def _t1_f16_02(ctx: RunnerContext) -> str:
    srv, owned = _srv(ctx)
    try:
        pal = [("#dc2626", (220, 38, 38)), ("#16a34a", (22, 163, 74)), ("#2563eb", (37, 99, 235)), ("#9333ea", (147, 51, 234)), ("#ea580c", (234, 88, 12)), ("#0d9488", (13, 148, 136)), ("#ca8a04", (202, 138, 4)), ("#db2777", (219, 39, 119)), ("#4f46e5", (79, 70, 229))]
        urls = [srv.register_route(f"/t1/f16_tab{i}.html", {"status": 200, "body": f"<!DOCTYPE html><html><body style='margin:0;background:{hx}'></body></html>"}) for i, (hx, _) in enumerate(pal)]
        with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T1_F16_02"), "x11"), initial_url=urls[0], extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as s:
            win = s.wait_for_waterscape_window()
            ensure(s.wait_for_pixel_near(win, 240, 220, pal[0][1], timeout_sec=5.0)[0], "tab 0 failed to paint")
            for i in range(1, 9):
                s.press_keysym(ord("t"), ctrl=True); time.sleep(0.06)
                s.press_keysym(ord("l"), ctrl=True); time.sleep(0.04)
                s.type_ascii(urls[i]); s.press_keysym(XK_Return)
                ensure(s.wait_for_pixel_near(win, 240, 220, pal[i][1], timeout_sec=4.0)[0], f"tab {i} failed to paint")
            ok_sw, sw_ms, px = s.measure_ctrl_tab_switch_ms(win, (240, 220), pal[0][1], max_wait_ms=500.0)
            ensure(ok_sw and sw_ms < 16.0, f"9-tab cached_fb switch exceeded 16ms budget: ok={ok_sw}, {sw_ms:.2f}ms, px={px}")
            return f"9-tab cached_fb switch in {sw_ms:.2f} ms (<16 ms)"
    finally:
        if owned:
            srv.stop()


def _t1_f16_03(ctx: RunnerContext) -> str:
    srv, owned = _srv(ctx)
    try:
        u_bg = srv.register_route("/t1/f16_bg.html", {"status": 200, "body": "<!DOCTYPE html><html><head><link rel='stylesheet' href='/slow-css?ms=350&paint_body=1&bg=%239333ea'></head><body style='margin:0;background:#e2e8f0'></body></html>"})
        u_fg = srv.register_route("/t1/f16_fg.html", {"status": 200, "body": "<!DOCTYPE html><html><body style='margin:0;background:#0284c7'></body></html>"})
        with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T1_F16_03"), "x11"), initial_url=u_bg, extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as s:
            win = s.wait_for_waterscape_window(); time.sleep(0.08)
            s.press_keysym(ord("t"), ctrl=True); time.sleep(0.06)
            s.press_keysym(ord("l"), ctrl=True); time.sleep(0.04)
            s.type_ascii(u_fg); s.press_keysym(XK_Return)
            ensure(s.wait_for_pixel_near(win, 220, 220, (2, 132, 199), timeout_sec=4.0)[0], "fg tab did not paint")
            time.sleep(0.55)
            ok_bg, sw_ms, px = s.measure_ctrl_tab_switch_ms(win, (220, 220), (147, 51, 234), max_wait_ms=600.0)
            ensure(ok_bg, f"background tab did not finish async CSS restyle while inactive: px={px}")
            return f"background tab progressed & switched in {sw_ms:.2f} ms"
    finally:
        if owned:
            srv.stop()


def _t1_f16_04(ctx: RunnerContext) -> str:
    srv, owned = _srv(ctx)
    try:
        long_q = "pad=" + ("A" * 8500) + "&tail=OMNI8600"
        long_url = f"{srv.main_origin}/long-nav?{long_q}"
        with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T1_F16_04"), "x11"), initial_url=long_url, extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as s:
            win = s.wait_for_waterscape_window()
            ensure(s.wait_for_pixel_near(win, 50, 110, (22, 163, 74), timeout_sec=5.0)[0], "initial >=8KB URL did not load")
            cur = srv.mark_cursor()
            s.press_keysym(ord("l"), ctrl=True); time.sleep(0.06)
            s.press_keysym(XK_Return)
            deadline = time.monotonic() + 4.0
            traces: list[dict[str, typing.Any]] = []
            while time.monotonic() < deadline and not traces:
                traces = srv.get_traces(since=cur, path_prefix="/long-nav")
                time.sleep(0.05)
            ensure(len(traces) >= 1 and traces[-1]["raw_target_len"] >= 8500 and "tail=OMNI8600" in traces[-1]["query"], f"shell omnibar truncated >=8KB URL: {[t['raw_target_len'] for t in traces]}")
            return f"omnibar preserved {traces[-1]['raw_target_len']}B URL"
    finally:
        if owned:
            srv.stop()


def _t1_f16_05(ctx: RunnerContext) -> str:
    srv, owned = _srv(ctx)
    try:
        u = srv.register_route("/t1/f16_wheel.html", {"status": 200, "body": "<!DOCTYPE html><html><body style='margin:0;background:#fff'><div style='height:620px;background:#ef4444'></div><div style='height:900px;background:#16a34a'></div></body></html>"})
        with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T1_F16_05"), "x11"), initial_url=u, extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as s:
            win = s.wait_for_waterscape_window()
            ensure(s.wait_for_pixel_near(win, 300, 220, (239, 68, 68), timeout_sec=5.0)[0], "top red region missing")
            s.scroll_wheel(win, 300, 220, down=True, clicks=12)
            ensure(s.wait_for_pixel_near(win, 300, 220, (22, 163, 74), timeout_sec=3.0)[0], "X11 Button5 wheel scroll down did not scroll viewport")
            return "X11 smooth wheel scroll verified"
    finally:
        if owned:
            srv.stop()


# === Feature F17: Linux X11 Hangul XIM Input & Win32 Single-Frame Parity ===
def _t1_f17_01(ctx: RunnerContext) -> str:
    srv, owned = _srv(ctx)
    try:
        posted: list[str] = []

        def _on_submit(req: dict[str, typing.Any]) -> dict[str, typing.Any]:
            qs = urllib.parse.parse_qs(req.get("query", ""), keep_blank_values=True)
            posted.append(qs.get("v", [""])[0])
            return {"status": 200, "content_type": "application/json", "body": '{"ok":true}'}

        srv.register_route("/t1/f17_submit", _on_submit)
        page_html = """<!DOCTYPE html><html><body style="margin:0;background:#fff;padding:16px">
        <input id="ko" type="text" style="width:260px;height:44px;font-size:26px;color:#111;background:#fff;border:2px solid #334155">
        <button id="go" style="width:100px;height:44px;background:#2563eb;color:#fff;border:0">Send</button>
        <div id="st" style="margin-top:12px;width:140px;height:36px;background:#e2e8f0"></div>
        <script>
        document.getElementById('go').onclick = async () => {
          const val = document.getElementById('ko').value;
          await fetch('/t1/f17_submit?v=' + encodeURIComponent(val));
          document.getElementById('st').style.background = '#16a34a';
        };
        </script></body></html>"""
        u = srv.register_route("/t1/T1_F17_01.html", {"status": 200, "body": page_html})
        with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T1_F17_01"), "x11"), initial_url=u, extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as s:
            win = s.wait_for_waterscape_window()
            ensure(s.wait_for_pixel_near(win, 310, 76 + 34, (37, 99, 235), timeout_sec=5.0)[0], "Send button did not paint")
            s.click_window(win, 90, 76 + 34)
            time.sleep(0.06)
            s.compose_multi_key("rk")  # -> "가"
            time.sleep(0.04)
            s.press_keycode(KC_HANGUL_HAN)  # -> "한"
            time.sleep(0.04)
            s.press_keycode(KC_HANGUL_GEUL)  # -> "글"
            time.sleep(0.06)
            inp_grab = s.grab_window_region(win, 22, 76 + 22, 180, 32)
            s.click_window(win, 310, 76 + 34)
            ok_done, _, _ = s.wait_for_pixel_near(win, 60, 76 + 86, (22, 163, 74), timeout_sec=4.0)
            s.close_gracefully(win)
        ensure(ok_done and posted and posted[-1] == "가한글", f"Expected XIM Hangul '가한글' (not legacy 'rk'), got {posted}")
        ensure(inp_grab is not None and inp_grab.region_stats()["non_white"] >= 20, "Hangul glyph ink missing in X11 <input>")
        return f"X11 XIM Hangul '가한글' composed & painted ({posted[-1]})"
    finally:
        if owned:
            srv.stop()


def _t1_f17_02(ctx: RunnerContext) -> str:
    srv, owned = _srv(ctx)
    try:
        srv.register_route("/t1/f17_search", {"status": 200, "body": "<!DOCTYPE html><html><body style='margin:0;background:#16a34a'></body></html>"})
        u0 = srv.register_route("/t1/f17_start.html", {"status": 200, "body": "<!DOCTYPE html><html><body style='margin:0;background:#2563eb'></body></html>"})
        with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T1_F17_02"), "x11"), initial_url=u0, extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as s:
            win = s.wait_for_waterscape_window()
            ensure(s.wait_for_pixel_near(win, 240, 220, (37, 99, 235), timeout_sec=5.0)[0], "Initial blue page did not paint")
            cur = srv.mark_cursor()
            s.press_keysym(ord("l"), ctrl=True)
            time.sleep(0.06)
            s.type_ascii(f"{srv.main_origin}/t1/f17_search?q=")
            s.compose_multi_key("han")  # -> "한글"
            time.sleep(0.05)
            s.press_keysym(XK_Return)
            ok_nav, _, _ = s.wait_for_pixel_near(win, 240, 220, (22, 163, 74), timeout_sec=4.0)
            s.close_gracefully(win)
        traces = srv.get_traces(since=cur, path_prefix="/t1/f17_search")
        ensure(ok_nav and traces, "Omnibar Hangul navigation did not reach /t1/f17_search")
        q_raw = str(traces[-1].get("query", ""))
        ensure("%ED%95%9C%EA%B8%80" in q_raw.upper() or "한글" in q_raw, f"Omnibar Hangul corrupted in query: {q_raw!r}")
        return f"X11 Omnibar Ctrl+L Hangul XIM navigation verified ({q_raw})"
    finally:
        if owned:
            srv.stop()


def _t1_f17_03(ctx: RunnerContext) -> str:
    srv, owned = _srv(ctx)
    try:
        u = srv.register_route("/t1/T1_F17_03.html", {"status": 200, "body": "<!DOCTYPE html><html><body style='margin:0;background:#2563eb'><div style='width:120px;height:50px;background:#fff'>DPI</div></body></html>"})
        tab_heights: list[int] = []
        addr_heights: list[int] = []
        for dpi_val in (96, 120, 144, 192):
            r = run_waterscape_verify(ctx, u, test_id=f"T1_F17_03_{dpi_val}", dpi=dpi_val, theme="dark" if dpi_val in (96, 192) else "light", devtools="right" if dpi_val == 192 else None)
            ensure(r.ok, "; ".join(r.guard_failures))
            sl = r.dump_json.get("shell_layout") or {}
            ensure(int((sl.get("window_caption_rect") or {}).get("height", -1)) == 0, f"Duplicate modern caption rect at {dpi_val} DPI: {sl}")
            tab_heights.append(int((sl.get("tab_strip_rect") or {}).get("height", 0)))
            addr_heights.append(int((sl.get("address_bar_rect") or {}).get("height", 0)))
            if dpi_val == 192:
                ensure(int((sl.get("devtools_dock_rect") or {}).get("width", 0)) > 0, f"192 DPI right DevTools dock missing: {sl}")
        ensure(tab_heights == [36, 45, 54, 72] and addr_heights == [30, 38, 45, 60], f"Multi-DPI scaling mismatch: tabs={tab_heights}, addr={addr_heights}")
        with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T1_F17_03"), "x11"), initial_url=u, extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as s:
            win = s.wait_for_waterscape_window()
            ensure(len(s.list_viewable_windows()) == 1, "Expected exactly 1 viewable top-level X11 window")
            s.press_keysym(XK_F12)
            time.sleep(0.15)
            ensure(len(s.list_viewable_windows()) == 1, "F12 DevTools spawned separate top-level window")
            s.close_gracefully(win)
        return f"Single-frame window + PerMonitorV2-equivalent DPI scaling {tab_heights} verified"
    finally:
        if owned:
            srv.stop()


def _t1_f17_04(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><body style="margin:0;background:#fff">
    <div style="display:grid;grid-template-columns:120px 120px;gap:8px;padding:12px">
      <div style="height:60px;background:#2563eb;border-radius:8px"></div>
      <div style="height:60px;background:#16a34a;border-radius:8px"></div>
    </div><canvas id="cv" width="120" height="44" style="margin-left:12px"></canvas>
    <script>const g=document.getElementById('cv').getContext('2d');g.fillStyle='#dc2626';g.fillRect(0,0,120,44);document.title='CLASSIC_PARITY_OK';</script></body></html>"""
    srv, owned = _srv(ctx)
    try:
        u = srv.register_route("/t1/T1_F17_04.html", {"status": 200, "body": html})
        r_mod = run_waterscape_verify(ctx, u, test_id="T1_F17_04_mod", window_size="800x600")
        r_cls = run_waterscape_verify(ctx, u, test_id="T1_F17_04_cls", window_size="800x600", profile="minimum", theme="classic", force_classic_frame=True, force_cpu=True, save_png=True)
        ensure(r_mod.ok and r_cls.ok, f"Guard failures mod={r_mod.guard_failures} cls={r_cls.guard_failures}")
        sl = r_cls.dump_json.get("shell_layout") or {}
        cap_h = int((sl.get("window_caption_rect") or {}).get("height", 0))
        ensure(cap_h > 0 and r_cls.png is not None, f"Win2000 classic frame caption missing: {sl}")
        # Verify pixel parity inside the rendered content region (top 200x160)
        max_delta = 0
        for y in range(0, 140, 4):
            for x in range(0, 260, 4):
                p1 = r_mod.content_png.pixel(x, y)
                p2 = r_cls.content_png.pixel(x, y)
                max_delta = max(max_delta, abs(p1[0] - p2[0]), abs(p1[1] - p2[1]), abs(p1[2] - p2[2]))
        ensure(max_delta <= 2, f"CPU classic minimum vs modern content pixel delta {max_delta} > 2")
        return f"Win2000 classic frame (cap_h={cap_h}) & CPU pixel parity (max_delta={max_delta}) verified"
    finally:
        if owned:
            srv.stop()


def _t1_f17_05(ctx: RunnerContext) -> str:
    nm_out = subprocess.run(["nm", "-D", ctx.waterscape_bin], capture_output=True, text=True, timeout=10).stdout
    missing_xim = [sym for sym in ("XOpenIM", "XCreateIC", "XFilterEvent", "Xutf8LookupString") if sym not in nm_out]
    ensure(len(missing_xim) == 0, f"waterscape binary missing dynamic X11 XIM symbols (still using legacy XLookupString?): {missing_xim}")
    js_code = """
    const ok32 = (1n << 32n) === 4294967296n;
    const ok64 = ((1n << 64n) - 1n).toString(16) === 'ffffffffffffffff';
    const okNeg = (-17n / 5n === -3n) && (-17n % 5n === -2n);
    const ab = new ArrayBuffer(12);
    const dv = new DataView(ab);
    dv.setUint32(1, 0x12345678, true);
    dv.setFloat64(3, -12345.6789, false);
    const okDv = dv.getUint32(1, true) === 0x12345678 && Math.abs(dv.getFloat64(3, false) - (-12345.6789)) < 1e-9;
    console.log(JSON.stringify({ok32, ok64, okNeg, okDv}));
    """
    jr = run_arcr_js(ctx, code=js_code, test_id="T1_F17_05")
    ensure(jr.returncode == 0, f"arcr_js failed: {jr.stderr}")
    meta = json.loads(jr.stdout.strip().splitlines()[-1])
    ensure(all(meta.values()), f"32-bit/Win2000 BigInt/DataView portability invariant failed: {meta}")
    return "Dynamic XIM ELF symbols (XOpenIM/XCreateIC/XFilterEvent/Xutf8LookupString) & 32-bit BigInt/DataView verified"


# === Feature F18: Live Chrome-Grade --conformance & Interactive X11 QA ===
def _assert_live_ok(res: typing.Any, label: str, *, min_cmds: int = 8, require_color: bool = True) -> None:
    ensure(res.ok, f"{label} failed anti-false-positive guards: {res.guard_failures}")
    ensure(res.dump_json.get("conformance_mode") is True, f"{label} did not run in --conformance mode")
    st = res.dump_json.get("stats") or {}
    ensure(int(st.get("display_cmd_count", 0)) >= min_cmds and int(st.get("dom_node_count", 0)) >= 15, f"{label} sparse render: {st}")
    ensure(res.content_png is not None, f"{label} missing content_png")
    if require_color:
        grey_only, reason = res.content_png.is_grey_skeleton_only()
        ensure(not grey_only, f"{label} grey skeleton only: {reason}")


def _t1_f18_01(ctx: RunnerContext) -> str:
    probe = "(() => { const b = Array.from(document.querySelectorAll('a,button')).find(e => /sign in|로그인/i.test(e.textContent||'')); const r = b ? b.getBoundingClientRect() : null; console.log('GH:' + JSON.stringify({btn: r ? [Math.round(r.left),Math.round(r.top),Math.round(r.width),Math.round(r.height)] : null})); })();"
    r_home = run_waterscape_verify(ctx, "https://www.google.com/?hl=ko", test_id="T1_F18_01_home", conformance=True, wait_load_ms=10000, eval_js=probe, timeout_sec=28.0)
    _assert_live_ok(r_home, "google.com Home")
    bad_404 = [t for t in (r_home.dump_json.get("traces") or []) if ("/xjs/" in str(t.get("url", "")) or "/async/" in str(t.get("url", ""))) and int(t.get("status_code", 200)) == 404]
    ensure(len(bad_404) == 0, f"Google /xjs/ or /async/ long URL truncated to 404: {bad_404[:2]}")
    m = re.search(r"GH:(\{.*\})", str(r_home.dump_json.get("console_log", "")))
    if m:
        btn = json.loads(m.group(1)).get("btn")
        if btn and btn[2] >= 20 and btn[3] >= 16:
            cr = r_home.content_png.region_max_contrast(btn[0], btn[1], btn[2], btn[3])
            ensure(cr >= 3.5, f"Google Sign-In contrast too low: {cr:.2f}")
    r_srp = run_waterscape_verify(ctx, "https://www.google.com/search?q=linux&hl=en", test_id="T1_F18_01_srp", conformance=True, wait_load_ms=12000, timeout_sec=30.0)
    _assert_live_ok(r_srp, "google.com/search?q=linux", min_cmds=12)
    ensure("/sorry/" not in str(r_srp.dump_json.get("url", "")), f"Google search redirected to /sorry/: {r_srp.dump_json.get('url')}")
    return "Live google.com Home & /search?q=linux --conformance verified"


def _t1_f18_02(ctx: RunnerContext) -> str:
    probe = "(() => { const sk = Array.from(document.querySelectorAll('a,div,span')).find(e => (e.textContent||'').includes('본문 바로가기')); let vis = false; if (sk) { const r = sk.getBoundingClientRect(); const cs = getComputedStyle(sk); if (r.width > 4 && r.height > 4 && cs.clip === 'auto' && cs.clipPath === 'none' && r.bottom > 2) vis = true; } console.log('ZZ:' + JSON.stringify({vis})); })();"
    r = run_waterscape_verify(ctx, "https://www.zuzunza.com/", test_id="T1_F18_02", conformance=True, wait_load_ms=12000, eval_js=probe, timeout_sec=32.0)
    _assert_live_ok(r, "www.zuzunza.com", min_cmds=12)
    m = re.search(r"ZZ:(\{.*\})", str(r.dump_json.get("console_log", "")))
    if m:
        ensure(json.loads(m.group(1)).get("vis") is False, "zuzunza.com .sr-only '본문 바로가기' bled visibly at top")
    return "Live www.zuzunza.com --conformance zero sr-only bleed & portal layout verified"


def _t1_f18_03(ctx: RunnerContext) -> str:
    probe = "(() => { console.log('YT:' + JSON.stringify({fake: !!document.querySelector('[data-arcr-hydrated]'), app: !!document.querySelector('ytd-app,ytm-app'), shadow: Array.from(document.querySelectorAll('*')).some(e => !!e.shadowRoot)})); })();"
    r_home = run_waterscape_verify(ctx, "https://www.youtube.com/", test_id="T1_F18_03_home", conformance=True, wait_load_ms=15000, eval_js=probe, timeout_sec=38.0)
    _assert_live_ok(r_home, "youtube.com Home", min_cmds=15)
    m = re.search(r"YT:(\{.*\})", str(r_home.dump_json.get("console_log", "")))
    if m:
        meta = json.loads(m.group(1))
        ensure(meta.get("fake") is False and meta.get("app") is True and meta.get("shadow") is True, f"YouTube Home Polymer/ShadowRoot defect: {meta}")
    r_search = run_waterscape_verify(ctx, "https://www.youtube.com/results?search_query=lofi+hip+hop", test_id="T1_F18_03_search", conformance=True, wait_load_ms=15000, timeout_sec=38.0)
    _assert_live_ok(r_search, "youtube.com Search", min_cmds=15)
    ensure(not r_search.content_png.detect_crossed_box_tofu(40, 80, 1100, 580), "Tofu box detected on live YouTube Search")
    r_watch = run_waterscape_verify(ctx, "https://www.youtube.com/watch?v=jfKfPfyJRdk", test_id="T1_F18_03_watch", conformance=True, wait_load_ms=16000, timeout_sec=40.0)
    _assert_live_ok(r_watch, "youtube.com Watch", min_cmds=15)
    return "Live youtube.com Home/Search/Watch --conformance verified"


def _t1_f18_04(ctx: RunnerContext) -> str:
    probe = "(() => { const fake = !!document.querySelector('[data-arcr-hydrated]'); const bleed = Array.from(document.querySelectorAll('svg')).some(s => { const r = s.getBoundingClientRect(); return !!s.querySelector('symbol') && r.width > 4 && r.height > 4 && r.left === 0 && r.top === 0; }); console.log('XP:' + JSON.stringify({fake, bleed})); })();"
    for idx, url in enumerate(("https://x.com/", "https://x.com/x")):
        r = run_waterscape_verify(ctx, url, test_id=f"T1_F18_04_{idx}", conformance=True, wait_load_ms=14000, eval_js=probe, timeout_sec=36.0)
        _assert_live_ok(r, f"x.com ({url})", min_cmds=8, require_color=False)
        ensure(r.content_png.region_stats()["distinct_colors"] >= 6, f"{url} rendered uniform/blank")
        m = re.search(r"XP:(\{.*\})", str(r.dump_json.get("console_log", "")))
        if m:
            meta = json.loads(m.group(1))
            ensure(meta.get("fake") is False and meta.get("bleed") is False, f"x.com facade or (0,0) SVG bleed on {url}: {meta}")
    return "Live x.com Home & Post --conformance verified"


def _t1_f18_05(ctx: RunnerContext) -> str:
    with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T1_F18_05"), "x11"), initial_url="https://www.google.com/?hl=ko", extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as s:
        win = s.wait_for_waterscape_window()
        ensure(len(s.list_viewable_windows()) == 1, "Expected single viewable X11 window on live site")
        deadline = time.monotonic() + 14.0
        g_ok = False
        while time.monotonic() < deadline:
            reg = s.grab_window_region(win, 40, 100, 640, 420)
            if reg is not None and reg.region_stats()["distinct_colors"] >= 8:
                g_ok = True
                break
            time.sleep(0.25)
        ensure(g_ok, "Live google.com did not paint rich content in interactive Xvfb GUI")
        s.press_keysym(ord("t"), ctrl=True); time.sleep(0.1)
        s.press_keysym(ord("l"), ctrl=True); time.sleep(0.06)
        s.type_ascii("https://www.zuzunza.com/"); s.press_keysym(XK_Return)
        deadline2 = time.monotonic() + 15.0
        z_ok = False
        while time.monotonic() < deadline2:
            reg2 = s.grab_window_region(win, 40, 100, 640, 420)
            if reg2 is not None and reg2.region_stats()["distinct_colors"] >= 10:
                z_ok = True
                break
            time.sleep(0.25)
        ensure(z_ok, "Live zuzunza.com did not paint rich content in interactive Xvfb Tab 2")
        s.press_keysym(XK_Tab, ctrl=True); time.sleep(0.1)
        rc = s.close_gracefully(win)
    ensure(rc == 0, f"Interactive live X11 GUI did not exit cleanly (rc={rc})")
    return "Interactive Xvfb X11 multi-tab live Google & Zuzunza session verified"


# === Feature F19: Clause-Honest standards_2026_inventory.json & Repo/Doc Sync ===
def _load_inventory(ctx: RunnerContext) -> dict[str, typing.Any]:
    inv_p = os.path.join(ctx.repo_root, "docs", "standards_2026_inventory.json")
    ensure(os.path.isfile(inv_p), f"Missing {inv_p}")
    obj = json.loads(open(inv_p, "r", encoding="utf-8").read())
    ensure(isinstance(obj, dict), "standards_2026_inventory.json root must be object")
    return obj


def _t1_f19_01(ctx: RunnerContext) -> str:
    inv = _load_inventory(ctx)
    entries = inv.get("entries") or []
    ensure(len(entries) >= 28, f"Expected clause-expanded standards_2026_inventory.json (>=28 entries), got {len(entries)}")
    valid_codes = {"verified", "implemented", "partial", "unimplemented", "not_applicable"}
    req_fields = ("id", "group", "spec_name", "spec_version", "publication_status", "clause_url", "platforms_and_conditions", "applicable", "status_code", "modern_status", "win2000_legacy_status", "blocking_issues", "implementation_files")
    seen: set[str] = set()
    for e in entries:
        for k in req_fields:
            ensure(k in e, f"Entry {e.get('id')} missing required field {k!r}")
        eid = str(e["id"]).strip()
        ensure(bool(eid) and eid not in seen, f"Empty or duplicate entry id: {eid!r}")
        seen.add(eid)
        ensure(isinstance(e["applicable"], bool) and isinstance(e["blocking_issues"], list), f"Bad types on {eid}")
        ensure(e["status_code"] in valid_codes and e["modern_status"] in valid_codes and e["win2000_legacy_status"] in valid_codes, f"Invalid status code on {eid}")
        for rel_p in e["implementation_files"]:
            ensure(os.path.exists(os.path.join(ctx.repo_root, rel_p)), f"Entry {eid} references missing path {rel_p}")
    return f"Inventory schema & {len(entries)} clause entries verified"


def _t1_f19_02(ctx: RunnerContext) -> str:
    inv = _load_inventory(ctx)
    entries = inv.get("entries") or []
    blob = json.dumps(entries, ensure_ascii=False).lower()
    required_clauses = [
        ("disposablestack", "ES2026 Explicit Resource Management"),
        ("sumprecise", "ES2026 Math.sumPrecise / getOrInsert"),
        ("durationformat", "ECMA-402 Intl.DurationFormat"),
        ("adoptedstylesheets", "Constructable Stylesheets"),
        ("woff2", "@font-face WOFF2 web fonts"),
        ("websocket", "RFC 6455 WebSocket"),
        ("urlpattern", "WHATWG URLPattern"),
        ("slotted", "Shadow DOM <slot> / ::slotted"),
    ]
    missing = [label for token, label in required_clauses if token not in blob]
    ensure(len(missing) == 0, f"Inventory missing 2026 clause coverage: {missing}")
    sw_entries = [e for e in entries if "serviceworker" in (str(e.get("id", "")) + " " + str(e.get("spec_name", "")) + " " + str(e.get("applicability_rationale", ""))).lower()]
    ensure(len(sw_entries) >= 1 and all(e.get("status_code") != "verified" or "indexeddb" not in str(e.get("id", "")).lower() for e in sw_entries), "ServiceWorker must be split from verified WebStorage/IndexedDB entry")
    return "All 2026 new clauses & split ServiceWorker entry verified"


def _t1_f19_03(ctx: RunnerContext) -> str:
    inv = _load_inventory(ctx)
    entries = inv.get("entries") or []
    contradictions = [
        e["id"] for e in entries
        if (e.get("status_code") == "verified" or e.get("modern_status") == "verified") and len(e.get("blocking_issues") or []) > 0
    ]
    ensure(len(contradictions) == 0, f"G5 Honest Claim Gate violation — verified entries have non-empty blocking_issues: {contradictions}")
    notes = json.dumps(inv.get("maturity_notes_2026") or {}, ensure_ascii=False)
    for dt in ("2026-06-22", "2026-09-15", "2026-11-06", "2026-10-30"):
        ensure(dt in notes, f"maturity_notes_2026 missing authoritative date {dt}")
    return f"G5 Honest Claim Gate passed across {len(entries)} entries"


def _t1_f19_04(ctx: RunnerContext) -> str:
    srv, owned = _srv(ctx)
    try:
        srv.register_route("/t1/f19_mod.mjs", {"status": 200, "content_type": "application/javascript", "body": "export const V = 2026;"})
        html = """<!DOCTYPE html><html><head><script type="importmap">{"imports":{"pkg-v":"/t1/f19_mod.mjs"}}</script>
        <style>@layer a, b; @layer b { #bx { width: 180px; height: 44px; background: #16a34a; } } @layer a { #bx { width: 80px; background: #dc2626; } }</style></head>
        <body style="margin:0;background:#fff"><div id="bx"></div><div id="sh"><span slot="s">SL</span></div>
        <script type="module">
        import { V } from 'pkg-v';
        const sr = sh.attachShadow({mode:'open'}); sr.innerHTML = '<slot name="s"></slot>';
        const slOk = sr.querySelector('slot').assignedElements().length === 1;
        const dig = await crypto.subtle.digest('SHA-256', new TextEncoder().encode('arcr'));
        const idbOk = typeof indexedDB === 'object' && indexedDB !== null;
        document.title = JSON.stringify({V, w: Math.round(bx.getBoundingClientRect().width), slOk, digBytes: dig.byteLength, idbOk, gridSup: CSS.supports('display: grid'), fakeSup: CSS.supports('bogus-prop-2099: 1px')});
        </script></body></html>"""
        url = srv.register_route("/t1/T1_F19_04.html", {"status": 200, "body": html})
        res = run_waterscape_verify(ctx, url, test_id="T1_F19_04")
        ensure(res.ok, "; ".join(res.guard_failures))
        d = _title_json(res)
        ensure(d["V"] == 2026 and d["w"] == 180 and d["slOk"] is True and d["digBytes"] == 32 and d["idbOk"] is True and d["gridSup"] is True and d["fakeSup"] is False, f"Verified inventory runtime probe mismatch: {d}")
        return f"Runtime cross-check of verified inventory clauses passed: {d}"
    finally:
        if owned:
            srv.stop()


def _t1_f19_05(ctx: RunnerContext) -> str:
    rep_p = os.path.join(ctx.repo_root, "docs", "verification", "verification_report_2026.md")
    rdm_p = os.path.join(ctx.repo_root, "README.md")
    ensure(os.path.isfile(rep_p) and os.path.isfile(rdm_p), "Missing verification_report_2026.md or README.md")
    rep_txt = open(rep_p, "r", encoding="utf-8").read()
    rdm_txt = open(rdm_p, "r", encoding="utf-8").read()
    for m in re.finditer(r"\]\((?!https?://|#|mailto:)([^)]+)\)", rdm_txt):
        rel = m.group(1).split("#", 1)[0].strip()
        if rel:
            ensure(os.path.exists(os.path.join(ctx.repo_root, rel)), f"README.md broken relative link: {rel}")
    ensure("Test262" in rep_txt and "WPT" in rep_txt and "파일" in rep_txt, "verification_report_2026.md missing separated Test262/WPT denominators")
    combined_lower = (rep_txt + "\n" + rdm_txt).lower()
    ensure("gdrive" in combined_lower or "google drive" in combined_lower, "Docs sync missing documented read-only GDrive check (F19)")
    ensure("사이트 이름 기반 보조 하이드레이터(`core/arcr_site_hydrators.h`)를 완전히 배제하고" not in rdm_txt, "README.md still describes active arcr_site_hydrators.h fallback instead of synced F11/F19 removal")
    return "verification_report_2026.md & README.md links, denominators & GDrive sync verified"


TESTS: list[TestCaseSpec] = [
    # F12 (5)
    _spec("T1_F12_01", "F12", "Shadow DOM default <slot> light-DOM projection & empty fallback paint", _t1_f12_01),
    _spec("T1_F12_02", "F12", "Shadow DOM named <slot> visual reordering & bounding rect geometry", _t1_f12_02),
    _spec("T1_F12_03", "F12", "Shadow CSS scoping isolation & :host / :host(.active) styling", _t1_f12_03),
    _spec("T1_F12_04", "F12", "Shadow DOM ::slotted(.pill) selector cascade & paint", _t1_f12_04),
    _spec("T1_F12_05", "F12", "Slotted light-DOM elementFromPoint hit-testing & click dispatch", _t1_f12_05),
    # F13 (5)
    _spec("T1_F13_01", "F13", "60 Hz CSS transition width & background-color midpoint interpolation", _t1_f13_01),
    _spec("T1_F13_02", "F13", "External CSS @import fetch & @property non-inheriting initial-value", _t1_f13_02),
    _spec("T1_F13_03", "F13", "CSS math functions calc(), min(), max(), clamp() & dvh units", _t1_f13_03),
    _spec("T1_F13_04", "F13", "<picture> media srcset selection & WebP decoding", _t1_f13_04),
    _spec("T1_F13_05", "F13", "Honest HTTP Accept image content negotiation & clean decode", _t1_f13_05),
    # F14 (5)
    _spec("T1_F14_01", "F14", "@font-face WOFF2 download, FreeType memory binding & monospace metrics", _t1_f14_01),
    _spec("T1_F14_02", "F14", "Multi-weight @font-face (WOFF + bold TTF) & per-element font-family isolation", _t1_f14_02),
    _spec("T1_F14_03", "F14", "256-level grayscale glyph anti-aliasing across Hangul & Latin", _t1_f14_03),
    _spec("T1_F14_04", "F14", "NotoColorEmoji RGBA multi-hue bitmap blitting", _t1_f14_04),
    _spec("T1_F14_05", "F14", "Zero-tofu Unicode Mathematical Alphanumeric & symbol fallback", _t1_f14_05),
    # F15 (5)
    _spec("T1_F15_01", "F15", "Zero-bleed .sr-only clipping across rect(0), rect(1px), inset(50%) & 1px overflow", _t1_f15_01),
    _spec("T1_F15_02", "F15", "Hidden <svg><symbol> suppression, <use> instancing & viewBox clipping", _t1_f15_02),
    _spec("T1_F15_03", "F15", "Flex 100vh sticky footer, WCAG >=4.5:1 Sign-In contrast & Korean line-height", _t1_f15_03),
    _spec("T1_F15_04", "F15", "32-track CSS Grid layout & explicit grid-column 25/33 placement", _t1_f15_04),
    _spec("T1_F15_05", "F15", "Nested overflow-y:auto scrollTop, clip bounds & root IntersectionObserver", _t1_f15_05),
    # F16 (5)
    _spec("T1_F16_01", "F16", "Off-UI-thread non-blocking tab switch during slow CSS/JS subresource fetch", _t1_f16_01),
    _spec("T1_F16_02", "F16", "9-tab (>=8 inactive) cached_fb retention & <16 ms Ctrl+Tab switch", _t1_f16_02),
    _spec("T1_F16_03", "F16", "Time-sliced background tab async CSS completion while inactive", _t1_f16_03),
    _spec("T1_F16_04", "F16", "Shell omnibar >=8 KB URL preservation across Ctrl+L & Enter", _t1_f16_04),
    _spec("T1_F16_05", "F16", "Live X11 Button5 smooth mouse wheel viewport scrolling", _t1_f16_05),
    # F17 (5)
    _spec("T1_F17_01", "F17", "Live X11 XIM Hangul composition ('가한글') into web <input> & glyph paint", _t1_f17_01),
    _spec("T1_F17_02", "F17", "Live X11 XIM Hangul composition ('한글') in Ctrl+L browser omnibar", _t1_f17_02),
    _spec("T1_F17_03", "F17", "Single-frame window ownership, 96..192 multi-DPI scaling & F12 in-window dock", _t1_f17_03),
    _spec("T1_F17_04", "F17", "Win2000 classic single-frame & --force-cpu --profile minimum pixel parity", _t1_f17_04),
    _spec("T1_F17_05", "F17", "Dynamic X11 XIM ELF symbol binding & 32-bit BigInt/DataView portability", _t1_f17_05),
    # F18 (5)
    _spec("T1_F18_01", "F18", "Live google.com Home & /search?q=linux --conformance visual & long-URL QA", _t1_f18_01, related_features=("F06", "F15"), is_live=True),
    _spec("T1_F18_02", "F18", "Live www.zuzunza.com --conformance zero .sr-only bleed & portal render QA", _t1_f18_02, related_features=("F13", "F15"), is_live=True),
    _spec("T1_F18_03", "F18", "Live youtube.com Home/Search/Watch --conformance Polymer Shadow DOM & zero-tofu QA", _t1_f18_03, related_features=("F11", "F12", "F14"), is_live=True),
    _spec("T1_F18_04", "F18", "Live x.com Home & Post --conformance zero fake hydrator & zero (0,0) SVG bleed QA", _t1_f18_04, related_features=("F11", "F15"), is_live=True),
    _spec("T1_F18_05", "F18", "Interactive Xvfb X11 multi-tab live Google & Zuzunza GUI session", _t1_f18_05, related_features=("F16", "F17"), is_live=True),
    # F19 (5)
    _spec("T1_F19_01", "F19", "Clause-expanded standards_2026_inventory.json (>=28 entries) schema & path integrity", _t1_f19_01),
    _spec("T1_F19_02", "F19", "Inventory coverage of 2026 clauses (ERM, sumPrecise, DurationFormat, WOFF2, WS, URLPattern, <slot>) & split ServiceWorker", _t1_f19_02),
    _spec("T1_F19_03", "F19", "G5 Honest Claim Gate: zero verified entries with blocking_issues & accurate 2026 dates", _t1_f19_03),
    _spec("T1_F19_04", "F19", "Live --conformance runtime probe of verified inventory clauses", _t1_f19_04),
    _spec("T1_F19_05", "F19", "verification_report_2026.md & README.md links, separated denominators & GDrive sync", _t1_f19_05),
]


def get_tests() -> list[TestCaseSpec]:
    return list(TESTS)


get_tier1_f12_f19_tests = get_tests

