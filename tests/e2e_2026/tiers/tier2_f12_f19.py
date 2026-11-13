import sys
sys.dont_write_bytecode = True

import json
import os
import re
import subprocess
import time
import typing
import urllib.parse

from ..harness.fixture_server import FixtureServer
from ..harness.types_and_guards import (
    AssertionFailure,
    RunnerContext,
    TestCaseSpec,
    TestResult,
    WaterscapeVerifyResult,
    ensure,
    run_arcr_js,
    run_waterscape_verify,
)
from ..harness.x11_driver import KC_HANGUL_GEUL, KC_HANGUL_HAN, XK_BackSpace, XK_Escape, XK_F11, XK_F12, XK_Return, X11Session

TestCase = TestCaseSpec
TestContext = RunnerContext


def _ensure_fixture(ctx: RunnerContext) -> tuple[FixtureServer, bool]:
    if ctx.fixture_srv is not None:
        return ctx.fixture_srv, False
    srv = FixtureServer(os.path.join(ctx.out_dir, "_tier2_f12_f19_assets"))
    srv.start()
    return srv, True


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
            return TestResult(test_id=test_id, tier=2, feature_id=feature_id, passed=True, skipped=True, diagnostic="Skipped (--skip-live)")
        t0 = time.perf_counter()
        try:
            out = body_fn(ctx)
            dt = (time.perf_counter() - t0) * 1000.0
            if isinstance(out, TestResult):
                out.duration_ms = out.duration_ms or dt
                return out
            return TestResult(test_id=test_id, tier=2, feature_id=feature_id, passed=True, diagnostic=out or "OK", duration_ms=dt, related_features=list(related_features))
        except AssertionFailure as exc:
            dt = (time.perf_counter() - t0) * 1000.0
            return TestResult(test_id=test_id, tier=2, feature_id=feature_id, passed=False, diagnostic=str(exc), duration_ms=dt, related_features=list(related_features))

    tc = TestCaseSpec(test_id=test_id, tier=2, feature_id=feature_id, description=description, fn=_wrapped, related_features=related_features, is_live=is_live)
    setattr(tc, "feature", feature_id)
    return tc


def _verify_page(ctx: RunnerContext, tid: str, html: str, routes: typing.Optional[dict[str, dict[str, typing.Any]]] = None, **kw: typing.Any) -> WaterscapeVerifyResult:
    srv, owns = _ensure_fixture(ctx)
    try:
        for rpath, rspec in (routes or {}).items():
            srv.register_route(rpath, rspec)
        url = srv.register_route(f"/{tid.lower()}/page", {"status": 200, "content_type": "text/html; charset=utf-8", "body": html})
        return run_waterscape_verify(ctx, url, test_id=tid, **kw)
    finally:
        if owns:
            srv.stop()


# --- Feature F12: Shadow DOM <slot> Projection & Shadow CSS Scoping (Tier 2) ---
def _t2_f12_01(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head><body style="margin:0;background:#fff">
    <div id="host"></div><script>
    const sr = host.attachShadow({mode:'open'});
    sr.innerHTML = '<style>.fb{display:block;width:120px;height:44px;background:#dc2626}</style><slot name="b"><span class="fb">FB</span></slot>';
    const sl = sr.querySelector('slot'); const a0 = sl.assignedNodes().length;
    const sp = document.createElement('span'); sp.slot = 'b'; sp.textContent = 'LIVE'; host.appendChild(sp);
    const a1 = sl.assignedNodes().length; sp.remove(); const a2 = sl.assignedNodes().length;
    document.title = `FB:${a0}:${a1}:${a2}:${Math.round(sr.querySelector('.fb').getBoundingClientRect().height)}`;
    </script></body></html>"""
    w = _verify_page(ctx, "T2_F12_01", html)
    ensure(w.ok and w.dump_json.get("title") == "FB:0:1:0:44", f"Unexpected slot fallback state: {w.dump_json.get('title')!r} {w.guard_failures}")
    ensure(w.content_png is not None and w.content_png.count_color_near((220, 38, 38), rect=(0, 0, 120, 44)) >= 800, "Fallback slot #dc2626 not painted")
    return "Fallback <slot> 0->1->0 dynamic projection & paint verified"


def _t2_f12_02(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head><body style="margin:0;background:#fff">
    <div id="outer"><div slot="hdr" style="width:130px;height:42px;background:#16a34a">H</div></div><script>
    const osr = outer.attachShadow({mode:'closed'}); let dupErr = '';
    try { outer.attachShadow({mode:'open'}); } catch (e) { dupErr = e.name; }
    osr.innerHTML = '<div id="inner"><slot name="hdr" slot="in-h"></slot></div>';
    const isr = osr.getElementById('inner').attachShadow({mode:'closed'});
    isr.innerHTML = '<slot name="in-h"></slot><button id="b">B</button>';
    let leak = 0, retargetOk = false;
    document.addEventListener('priv', () => leak++);
    document.addEventListener('pub', (e) => { retargetOk = (e.target === outer); });
    isr.getElementById('b').dispatchEvent(new CustomEvent('priv', {bubbles:true, composed:false}));
    isr.getElementById('b').dispatchEvent(new CustomEvent('pub', {bubbles:true, composed:true}));
    document.title = `CLOSED:${outer.shadowRoot===null}:${dupErr}:${leak}:${retargetOk}`;
    </script></body></html>"""
    w = _verify_page(ctx, "T2_F12_02", html)
    ensure(w.ok and w.dump_json.get("title") == "CLOSED:true:NotSupportedError:0:true", f"Got {w.dump_json.get('title')!r} {w.guard_failures}")
    ensure(w.content_png is not None and w.content_png.count_color_near((22, 163, 74), rect=(0, 0, 130, 42)) >= 800, "Nested forwarded slot #16a34a missing")
    return "Closed shadow root, duplicate attachShadow rejection, composed retargeting & nested slot verified"


def _t2_f12_03(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"><style>div{background:#ff00ff !important}</style></head><body style="margin:0;background:#fff">
    <section id="host"><span id="ghost" slot="missing" style="display:block;width:120px;height:60px;background:#ff00ff">G</span></section><script>
    const sr = host.attachShadow({mode:'open'});
    sr.innerHTML = '<style>.p{width:140px;height:50px;background:#2563eb}</style><div class="p"><slot name="ok"></slot></div>';
    document.title = `UNSLOTTED:${Math.round(ghost.getBoundingClientRect().width)}`;
    </script></body></html>"""
    w = _verify_page(ctx, "T2_F12_03", html)
    ensure(w.ok and w.dump_json.get("title") == "UNSLOTTED:0", f"Unslotted light-DOM laid out: {w.dump_json.get('title')!r} {w.guard_failures}")
    ensure(w.content_png is not None and w.content_png.count_color_near((255, 0, 255), tol=18) == 0, "Bleed #ff00ff from unslotted/outer rule detected")
    ensure(w.content_png.count_color_near((37, 99, 235), rect=(0, 0, 140, 50)) >= 900, "Shadow .p #2563eb not isolated")
    return "Unmatched slot suppression & outer !important shadow isolation verified"


def _t2_f12_04(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"></head><body style="margin:0;background:#fff">
    <div id="h" class="danger"><p class="it"><span>TXT</span></p></div><script>
    const sr = h.attachShadow({mode:'open'});
    sr.innerHTML = '<style>:host{display:block;width:240px;height:60px;background:#16a34a}:host(.danger){background:#dc2626}::slotted(.it){margin:0;width:210px}::slotted(.it span){background:#ff00ff}</style><slot></slot>';
    document.title = `HOST:${Math.round(h.querySelector('.it').getBoundingClientRect().width)}`;
    </script></body></html>"""
    w = _verify_page(ctx, "T2_F12_04", html)
    ensure(w.ok and w.dump_json.get("title") == "HOST:210", f"Got {w.dump_json.get('title')!r} {w.guard_failures}")
    ensure(w.content_png is not None and w.content_png.count_color_near((255, 0, 255), tol=18) == 0, "Invalid ::slotted(.it span) was not rejected")
    ensure(w.content_png.count_color_near((220, 38, 38), rect=(0, 0, 240, 60)) >= 1000, ":host(.danger) did not override :host")
    return ":host(.danger) specificity & invalid descendant ::slotted() rejection verified"


def _t2_f12_05(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"></head><body style="margin:0;background:#fff">
    <div id="h"><button id="btn" slot="cta" style="width:100px;height:44px;border:0;background:#dc2626">GO</button></div><script>
    const sr = h.attachShadow({mode:'open'});
    sr.innerHTML = '<div style="margin-top:80px;margin-left:120px"><slot name="cta"></slot></div>';
    const hit = document.elementFromPoint(150, 100) === btn;
    btn.onclick = () => { btn.style.background = '#16a34a'; document.title = `HIT:${hit}:CLICKED`; };
    </script></body></html>"""
    w = _verify_page(ctx, "T2_F12_05", html, click=(150, 100))
    ensure(w.ok and w.dump_json.get("title") == "HIT:true:CLICKED", f"Slotted hit-test/click failed: {w.dump_json.get('title')!r} {w.guard_failures}")
    ensure(w.content_png is not None and w.content_png.count_color_near((22, 163, 74), rect=(120, 80, 100, 44)) >= 600, "Clicked slotted button not green")
    return "Slotted light-DOM elementFromPoint & --click dispatch verified"


# --- Feature F13: CSS Transitions, @import, @property, Math & Image Negotiation (Tier 2) ---
def _t2_f13_01(ctx: RunnerContext) -> str:
    routes = {
        "/f13/a.css": {"status": 200, "content_type": "text/css", "body": "@import url('/f13/b.css'); @import url('/f13/missing404.css'); #ok{width:220px;height:56px;background:#16a34a}"},
        "/f13/b.css": {"status": 200, "content_type": "text/css", "body": "@import url('/f13/a.css'); body{margin:0;background:#fff}"},
    }
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"><link rel="stylesheet" href="/f13/a.css"></head>
    <body><div id="ok"></div><script>window.onload=()=>document.title='IMP:'+Math.round(ok.getBoundingClientRect().width);</script></body></html>"""
    w = _verify_page(ctx, "T2_F13_01", html, routes=routes)
    ensure(w.ok and w.duration_ms < 5000 and w.dump_json.get("title") == "IMP:220", f"Circular/404 @import failed: {w.dump_json.get('title')!r} {w.guard_failures}")
    ensure(w.content_png is not None and w.content_png.count_color_near((22, 163, 74), rect=(0, 0, 220, 56)) >= 1200, "#ok green missing after @import cycle")
    return "Circular @import & 404 @import recovery verified"


def _t2_f13_02(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"><style>
    @property --box-w { syntax: '<length>'; inherits: false; initial-value: 184px; }
    body{margin:0;background:#fff} #p{--box-w:320px}
    #c1{height:30px;width:var(--box-w);background:#2563eb}
    #c2{--box-w:not-a-length;height:30px;width:var(--box-w);background:#2563eb}
    </style></head><body><div id="p"><div id="c1"></div><div id="c2"></div></div>
    <script>document.title=`PROP:${Math.round(c1.getBoundingClientRect().width)}:${Math.round(c2.getBoundingClientRect().width)}`;</script></body></html>"""
    w = _verify_page(ctx, "T2_F13_02", html)
    ensure(w.ok and w.dump_json.get("title") == "PROP:184:184", f"@property boundary failed: {w.dump_json.get('title')!r} {w.guard_failures}")
    ensure(w.content_png is not None and w.content_png.count_color_near((37, 99, 235), rect=(0, 0, 184, 60)) >= 2000, "#c1/#c2 not painted 184px")
    ensure(w.content_png.count_color_near((37, 99, 235), rect=(205, 0, 115, 60)) == 0, "--box-w leaked inherited 320px")
    return "@property inherits:false & invalid syntax initial-value fallback verified"


def _t2_f13_03(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"><style>
    body{margin:0;background:#fff} #bar{width:100px;height:50px;background:#2563eb;transition:width 600ms linear}
    </style></head><body><div id="bar"></div><script>
    let ends = 0, reversed = false; bar.addEventListener('transitionend', () => ends++);
    bar.getBoundingClientRect(); const t0 = performance.now(); bar.style.width = '300px';
    function step(now) {
      const dt = now - t0;
      if (!reversed && dt >= 160) { reversed = true; bar.style.width = '100px'; }
      if (dt < 250) requestAnimationFrame(step);
      else document.title = `REV:${Math.round(bar.getBoundingClientRect().width)}:${ends}`;
    }
    requestAnimationFrame(step);
    </script></body></html>"""
    w = _verify_page(ctx, "T2_F13_03", html)
    ensure(w.ok, f"Guard failures: {w.guard_failures}")
    m = re.match(r"^REV:(\d+):(\d+)$", str(w.dump_json.get("title", "")))
    ensure(m is not None and 100 <= int(m.group(1)) <= 215 and int(m.group(2)) == 0, f"Transition reversal failed: {w.dump_json.get('title')!r}")
    ensure(w.content_png is not None and w.content_png.count_color_near((37, 99, 235), rect=(250, 0, 50, 50)) == 0, "Aborted 300px target still painted at x>=250")
    return "Mid-flight CSS transition reversal & zero spurious transitionend verified"


def _t2_f13_04(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"><style>
    body{margin:0;background:#fff}
    #m{background:#0ea5e9;width:164px;width:calc(20px + );width:clamp(100px, 50%, );height:clamp(40px, calc(50dvh - 302px), 120px)}
    </style></head><body><div id="m"></div><script>
    const r = m.getBoundingClientRect(); document.title = `MATH:${Math.round(r.width)}x${Math.round(r.height)}`;
    </script></body></html>"""
    w = _verify_page(ctx, "T2_F13_04", html, window_size="1280x800")
    ensure(w.ok and w.dump_json.get("title") == "MATH:164x60", f"CSS math/dvh boundary failed: {w.dump_json.get('title')!r} {w.guard_failures}")
    ensure(w.content_png is not None and w.content_png.count_color_near((14, 165, 233), rect=(0, 0, 164, 60)) >= 1500, "#m #0ea5e9 box not painted")
    return "Malformed CSS math parse recovery & nested dvh clamp verified"


def _t2_f13_05(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        cur = srv.mark_cursor()
        html = """<!DOCTYPE html><html><head><meta charset="utf-8"></head><body style="margin:0;background:#fff">
        <picture><source srcset="/f13/skip.jxl" type="image/jxl"><source srcset="/f13/skip3k.png" media="(min-width:3000px)">
        <img id="neg" src="/img/negotiated" style="display:block"></picture>
        <script>window.onload=()=>{const r=neg.getBoundingClientRect();document.title=`PIC:${Math.round(r.width)}x${Math.round(r.height)}`;};</script></body></html>"""
        url = srv.register_route("/t2_f13_05/page", {"status": 200, "content_type": "text/html; charset=utf-8", "body": html})
        w = run_waterscape_verify(ctx, url, test_id="T2_F13_05")
        ensure(w.ok and w.dump_json.get("title") == "PIC:64x64", f"<picture>/Accept failed: {w.dump_json.get('title')!r} {w.guard_failures}")
        ensure(len(srv.get_traces(since=cur, path_prefix="/f13/skip")) == 0, "Browser fetched unsupported image/jxl or non-matching media source")
        ensure(w.content_png is not None and w.content_png.count_color_near((34, 197, 94), tol=20, rect=(0, 0, 64, 64)) >= 2500, "Negotiated image did not decode green pixels")
        return "<picture> unsupported MIME/media skip & honest Accept negotiation verified"
    finally:
        if owns:
            srv.stop()


# --- Feature F14: @font-face Web Fonts, Color Emoji & Symbol Fallback (Tier 2) ---
def _t2_f14_01(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"><style>
    @font-face { font-family: 'BadW2'; src: url('/fonts/corrupt.woff2') format('woff2'); }
    @font-face { font-family: 'MissW2'; src: url('/fonts/missing404.woff2') format('woff2'); }
    @font-face { font-family: 'GoodW2'; src: url('/fonts/mono.woff2') format('woff2'); }
    body { margin:0; background:#fff; color:#111; font: 32px 'BadW2', 'MissW2', 'GoodW2', sans-serif; }
    </style></head><body><div><span id="si">iiiiiiii</span></div><div><span id="sw">WWWWWWWW</span></div>
    <script>window.onload=()=>{
      const wi=Math.round(si.getBoundingClientRect().width), ww=Math.round(sw.getBoundingClientRect().width);
      document.title=`W2FB:${wi}:${ww}:${Math.abs(ww-wi)<=4 && wi>60}`;
    };</script></body></html>"""
    w = _verify_page(ctx, "T2_F14_01", html)
    ensure(w.ok and str(w.dump_json.get("title", "")).endswith(":true"), f"Corrupt/404 WOFF2 fallback failed: {w.dump_json.get('title')!r} {w.guard_failures}")
    ensure(w.content_png is not None and w.content_png.count_grayscale_aa_levels(0, 0, 320, 90) >= 8, "Missing 256-level AA on fallback WOFF2 glyphs")
    return "Corrupt & 404 @font-face WOFF2 fallback chain + AA verified"


def _t2_f14_02(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"><style>
    @font-face { font-family: 'DigitMono'; src: url('/fonts/mono.woff2') format('woff2'); unicode-range: U+0030-0039; }
    body { margin:0; background:#fff; color:#111; font: 32px 'DigitMono', sans-serif; }
    </style></head><body>
    <div><span id="d1">11111111</span> <span id="d8">88888888</span></div>
    <div><span id="li">iiiiiiii</span> <span id="lw">WWWWWWWW</span></div>
    <script>window.onload=()=>{
      const w1=d1.getBoundingClientRect().width, w8=d8.getBoundingClientRect().width;
      const wi=li.getBoundingClientRect().width, ww=lw.getBoundingClientRect().width;
      document.title=`UR:${Math.abs(w8-w1)<=4}:${ww>wi*1.6}`;
    };</script></body></html>"""
    w = _verify_page(ctx, "T2_F14_02", html)
    ensure(w.ok and w.dump_json.get("title") == "UR:true:true", f"unicode-range boundary failed: {w.dump_json.get('title')!r} {w.guard_failures}")
    return "@font-face unicode-range selective digit glyph loading verified"


def _t2_f14_03(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"></head><body style="margin:0;background:#fff">
    <div id="e1" style="font-size:42px;line-height:60px;color:#0000ff;background:#ffffff">🔥 🎨 🚀</div>
    <div id="e2" style="font-size:42px;line-height:60px;color:#00ff00;background:#111827">🔥</div>
    <script>document.title='EMOJI_COLOR_BOUNDARY';</script></body></html>"""
    w = _verify_page(ctx, "T2_F14_03", html)
    ensure(w.ok and w.content_png is not None, f"Guard failures: {w.guard_failures}")
    st1 = w.content_png.region_stats(0, 0, 260, 62)
    st2 = w.content_png.region_stats(0, 62, 120, 62)
    ensure(st1["warm_pixels"] >= 20 and st1["max_chroma"] >= 60, f"CSS color:#0000ff overwrote RGBA color emoji: {st1}")
    ensure(st2["warm_pixels"] >= 15, f"Color emoji failed RGBA compositing on dark #111827 bg: {st2}")
    return "RGBA color emoji immunity to CSS text color & dark-bg compositing verified"


def _t2_f14_04(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"></head><body style="margin:0;background:#fff;color:#111;font-size:44px;line-height:64px">
    <div id="row"><span id="ml">𝐋</span>𝐨𝐟<span id="mi">𝐢</span> 𝕏 𝄞 ★ ✓</div>
    <script>const wl=ml.getBoundingClientRect().width, wi=mi.getBoundingClientRect().width;
    document.title=`SYM:${wl>8}:${wi<wl*0.78}`;</script></body></html>"""
    w = _verify_page(ctx, "T2_F14_04", html)
    ensure(w.ok and w.dump_json.get("title") == "SYM:true:true", f"Astral math symbol proportional metrics failed (tofu square?): {w.dump_json.get('title')!r} {w.guard_failures}")
    ensure(w.content_png is not None and not w.content_png.detect_crossed_box_tofu(0, 0, 520, 72), "Crossed-box tofu (⊠) detected on 𝐋𝐨𝐟𝐢 / 𝕏 / 𝄞")
    return "Astral Mathematical Alphanumeric 𝐋𝐨𝐟𝐢 / 𝕏 / 𝄞 zero-tofu fallback verified"


def _t2_f14_05(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"></head><body style="margin:0;background:#fff;color:#111;font-size:34px;line-height:56px">
    <div><span id="zwj">👨‍💻</span> <span id="sk">👍🏽</span> <span id="kr">🇰🇷</span> <span id="ko">가나다라</span></div>
    <script>const r=ko.getBoundingClientRect();
    const lens=[zwj.textContent.length,sk.textContent.length,kr.textContent.length,ko.textContent.length].join(',');
    document.title=`ZWJ:${lens}:${Math.round(r.left)}:${Math.round(r.width)}`;</script></body></html>"""
    w = _verify_page(ctx, "T2_F14_05", html)
    ensure(w.ok, f"Guard failures: {w.guard_failures}")
    m = re.match(r"^ZWJ:5,4,4,4:(\d+):(\d+)$", str(w.dump_json.get("title", "")))
    ensure(m is not None and int(m.group(1)) >= 80 and int(m.group(2)) >= 90, f"ZWJ/surrogate metrics broken: {w.dump_json.get('title')!r}")
    kx, kw = int(m.group(1)), int(m.group(2))
    ensure(w.content_png is not None and w.content_png.count_grayscale_aa_levels(kx, 0, kw, 58) >= 8, "Hangul after ZWJ emojis missing AA ink")
    return "ZWJ / Skin-Tone / Flag astral UTF-16 surrogates & trailing Hangul AA verified"


# --- Feature F15: Real-Site CSS Layout, sr-only, SVG Sprite & Nested Scroll Fixes (Tier 2) ---
def _t2_f15_01(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"><style>
    body{margin:0;background:#fff} .sr{background:#ff00ff;color:#00ffff;font-size:24px}
    .s1{position:absolute;clip:rect(0,0,0,0)} .s2{position:absolute;clip:rect(1px,1px,1px,1px);width:1px;height:1px;overflow:hidden}
    .s3{position:absolute;clip-path:inset(50%)} .s4{position:absolute;clip-path:polygon(0 0,0 0,0 0)}
    .s5{position:absolute;width:1px;height:1px;padding:0;margin:-1px;overflow:hidden} .s6{position:absolute;top:0;left:0;transform:translateY(-100%)}
    </style></head><body>
    <a class="sr s1">본문1</a><a class="sr s2">본문2</a><a class="sr s3">본문3</a>
    <a class="sr s4">본문4</a><a class="sr s5">본문5</a><a class="sr s6">본문6</a>
    <main style="margin-top:60px;width:160px;height:50px;background:#16a34a"></main>
    <script>document.title='SR:'+document.querySelectorAll('.sr').length;</script></body></html>"""
    w = _verify_page(ctx, "T2_F15_01", html)
    ensure(w.ok and w.dump_json.get("title") == "SR:6", f"Guard/DOM failed: {w.dump_json.get('title')!r} {w.guard_failures}")
    ensure(w.content_png is not None and w.content_png.has_bleed_color([(255, 0, 255), (0, 255, 255)], tol=24) == 0, "sr-only skip-link bled into viewport")
    ensure(w.content_png.count_color_near((22, 163, 74), rect=(0, 60, 160, 50)) >= 1200, "Main green landmark missing")
    return "All 6 sr-only / skip-link clipping patterns verified zero-bleed"


def _t2_f15_02(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"></head><body style="margin:0;background:#fff">
    <svg width="0" height="0"><symbol id="ic" viewBox="0 0 24 24"><rect width="24" height="24" fill="currentColor"/></symbol></svg>
    <svg style="display:none" width="40" height="40"><rect width="40" height="40" fill="#ff00ff"/></svg>
    <svg width="24" height="24"><use href="#missing"/><use id="cyc" href="#cyc"/></svg>
    <div style="margin-left:160px;margin-top:20px;color:#2563eb"><svg width="48" height="48"><use href="#ic"/></svg></div>
    <script>document.title='SVG_SPRITE_OK';</script></body></html>"""
    w = _verify_page(ctx, "T2_F15_02", html)
    ensure(w.ok and w.duration_ms < 5000 and w.dump_json.get("title") == "SVG_SPRITE_OK", f"SVG sprite edge failed: {w.guard_failures}")
    ensure(w.content_png is not None and w.content_png.region_stats(0, 0, 56, 56)["non_white"] == 0, "Hidden width=0 <svg>/<symbol> bled at (0,0)")
    ensure(w.content_png.count_color_near((255, 0, 255), tol=20) == 0, "display:none <svg> bled")
    ensure(w.content_png.count_color_near((37, 99, 235), rect=(160, 44, 48, 48)) + w.content_png.count_color_near((37, 99, 235), rect=(160, 20, 48, 48)) >= 600, "<use href='#ic'> did not paint #2563eb")
    return "Hidden <svg>/<symbol> zero-(0,0)-bleed & cyclic/missing <use> safety verified"


def _t2_f15_03(ctx: RunnerContext) -> str:
    cells = "".join(f'<div id="c{i}" style="height:24px;background:#cbd5e1"></div>' for i in range(48))
    html = f"""<!DOCTYPE html><html><head><meta charset="utf-8"></head><body style="margin:0;background:#fff">
    <div style="display:grid;grid-template-columns:repeat(48,20px);gap:2px">{cells}
    <div id="tail" style="grid-column:-5 / -1;height:28px;background:#16a34a"></div></div>
    <script>const x0=Math.round(c0.getBoundingClientRect().left),x20=Math.round(c20.getBoundingClientRect().left),x47=Math.round(c47.getBoundingClientRect().left);
    const tr=tail.getBoundingClientRect(); document.title=`GRID:${{x0}}:${{x20}}:${{x47}}:${{Math.round(tr.left)}}:${{Math.round(tr.width)}}`;</script></body></html>"""
    w = _verify_page(ctx, "T2_F15_03", html)
    ensure(w.ok and w.dump_json.get("title") == "GRID:0:440:1034:968:86", f"48-col grid/negative line failed: {w.dump_json.get('title')!r} {w.guard_failures}")
    ensure(w.content_png is not None and w.content_png.count_color_near((22, 163, 74), rect=(968, 24, 86, 32)) >= 900, "grid-column:-5/-1 #16a34a span not painted")
    return "48-track CSS Grid (>=32 tracks) & negative explicit line placement verified"


def _t2_f15_04(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"></head><body style="margin:0;background:#fff">
    <div id="sc" style="margin:40px;width:180px;height:200px;overflow-y:auto;background:#e2e8f0">
      <div style="height:860px"></div><div style="height:140px;background:#ef4444"></div>
    </div><script>
    sc.scrollTop = -500; const clampMin = sc.scrollTop;
    sc.scrollTop = 999999; const clampMax = sc.scrollTop;
    document.title = `CLAMP:${clampMin}:${clampMax}`;
    </script></body></html>"""
    w = _verify_page(ctx, "T2_F15_04", html)
    ensure(w.ok and w.dump_json.get("title") == "CLAMP:0:800", f"scrollTop clamping failed: {w.dump_json.get('title')!r} {w.guard_failures}")
    ensure(w.content_png is not None and w.content_png.count_color_near((239, 68, 68), rect=(40, 100, 180, 140)) >= 2000, "Scrolled #ef4444 band not painted inside scroller")
    ensure(w.content_png.count_color_near((239, 68, 68), rect=(40, 0, 180, 38)) == 0 and w.content_png.count_color_near((239, 68, 68), rect=(40, 242, 180, 120)) == 0, "Overflow child bled outside scroll container clip")
    return "Nested scrollTop negative/overflow clamping & strict clip bounds verified"


def _t2_f15_05(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"></head><body style="margin:0;background:#fff">
    <div id="sc" style="margin:40px;width:220px;height:180px;overflow-y:auto;background:#f1f5f9">
      <button id="decoy" style="display:block;width:160px;height:50px;border:0;background:#dc2626">D</button>
      <div style="height:370px"></div>
      <button id="target" style="display:block;width:160px;height:50px;border:0;background:#2563eb">T</button>
      <div style="height:300px"></div>
    </div><script>
    const ioMap = {};
    const io = new IntersectionObserver((es) => { for (const e of es) ioMap[e.target.id] = e.isIntersecting; }, {root: sc, threshold: 0.5});
    io.observe(decoy); io.observe(target);
    sc.scrollTop = 400;
    let who = 'NONE';
    decoy.onclick = () => { who = 'DECOY'; document.title = 'BAD_DECOY'; };
    target.onclick = () => { target.style.background = '#16a34a'; setTimeout(() => { document.title = `SCHIT:${document.elementFromPoint(90,80)===target}:${ioMap.decoy}:${ioMap.target}`; }, 20); };
    </script></body></html>"""
    w = _verify_page(ctx, "T2_F15_05", html, click=(90, 80))
    ensure(w.ok and w.dump_json.get("title") == "SCHIT:true:false:true", f"Scrolled hit-test/IntersectionObserver failed: {w.dump_json.get('title')!r} {w.guard_failures}")
    ensure(w.content_png is not None and w.content_png.count_color_near((22, 163, 74), rect=(40, 50, 180, 70)) >= 800, "Scrolled target button did not turn green on click")
    return "Scrolled nested container hit-testing (--click + elementFromPoint) & root IntersectionObserver verified"


# --- Feature F16: Non-Blocking Multicore Tab Pipeline & <16 ms Tab Switching (Tier 2) ---
def _t2_f16_01(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        u1 = srv.register_route("/f16/p1", {"status": 200, "content_type": "text/html; charset=utf-8", "body": '<!DOCTYPE html><html><head><link rel="stylesheet" href="/slow-css?ms=950&bg=%23ff00ff&paint_body=1"></head><body><script src="/slow-js?ms=950&tag=STALE_GEN1&set_title=1"></script></body></html>'})
        u2 = srv.register_route("/f16/p2", {"status": 200, "content_type": "text/html; charset=utf-8", "body": '<!DOCTYPE html><html><head><title>GEN2_CLEAN</title></head><body style="margin:0;background:#16a34a"><div style="width:120px;height:60px;background:#fff">OK</div></body></html>'})
        w = run_waterscape_verify(ctx, u1, second_url=u2, test_id="T2_F16_01", wait_load_ms=3500)
        ensure(w.ok and w.dump_json.get("title") == "GEN2_CLEAN", f"Stale gen leaked: {w.dump_json.get('title')!r} {w.guard_failures}")
        ensure(w.content_png is not None and w.content_png.count_color_near((255, 0, 255), tol=20) == 0, "Stale slow-css #ff00ff painted into Gen 2")
        return "Stale subresource generation cancellation on rapid second navigation verified"
    finally:
        if owns:
            srv.stop()


def _t2_f16_02(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        u_fast = srv.register_route("/f16/fast", {"status": 200, "content_type": "text/html; charset=utf-8", "body": '<!DOCTYPE html><html><body style="margin:0;background:#16a34a"></body></html>'})
        u_slow = srv.register_route("/f16/slow", {"status": 200, "content_type": "text/html; charset=utf-8", "body": '<!DOCTYPE html><html><head><link rel="stylesheet" href="/slow-css?ms=2200&bg=%232563eb&paint_body=1"></head><body style="margin:0;background:#dc2626"></body></html>'})
        with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T2_F16_02"), "x11"), initial_url=u_fast, extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as xs:
            win = xs.wait_for_waterscape_window()
            ok0, _, _ = xs.wait_for_pixel_near(win, 200, 200, (22, 163, 74), timeout_sec=5.0)
            ensure(ok0, "Tab 1 #16a34a did not paint")
            xs.press_keysym(ord("t"), ctrl=True); time.sleep(0.08)
            xs.press_keysym(ord("l"), ctrl=True); time.sleep(0.05)
            xs.type_ascii(u_slow); xs.press_keysym(XK_Return); time.sleep(0.12)
            matched, dt_ms, px = xs.measure_ctrl_tab_switch_ms(win, (200, 200), (22, 163, 74), max_wait_ms=1500.0)
            xs.close_gracefully(win)
        ensure(matched and dt_ms < 16.0, f"Tab switch blocked or exceeded 16ms budget during slow subresource load: matched={matched} dt_ms={dt_ms:.2f} px={px}")
        return f"Non-blocking <16ms tab switch ({dt_ms:.2f}ms) during slow subresource fetch verified"
    finally:
        if owns:
            srv.stop()


def _t2_f16_03(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        pal = [(220, 38, 38), (22, 163, 74), (37, 99, 235), (217, 119, 6), (147, 51, 234), (13, 148, 136), (219, 39, 119), (79, 70, 229), (5, 150, 105)]
        urls = [srv.register_route(f"/f16/t{i}", {"status": 200, "content_type": "text/html; charset=utf-8", "body": f'<!DOCTYPE html><html><body style="margin:0;background:rgb({r},{g},{b})"></body></html>'}) for i, (r, g, b) in enumerate(pal)]
        with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T2_F16_03"), "x11"), initial_url=urls[0], extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as xs:
            win = xs.wait_for_waterscape_window()
            ensure(xs.wait_for_pixel_near(win, 200, 200, pal[0], timeout_sec=5.0)[0], "Tab 0 initial paint failed")
            for u, rgb in zip(urls[1:], pal[1:]):
                xs.press_keysym(ord("t"), ctrl=True); time.sleep(0.05)
                xs.press_keysym(ord("l"), ctrl=True); time.sleep(0.03)
                xs.type_ascii(u); xs.press_keysym(XK_Return)
                ensure(xs.wait_for_pixel_near(win, 200, 200, rgb, timeout_sec=4.0)[0], f"Tab {u} paint failed")
            t0 = time.perf_counter(); xs.press_keysym(ord("1"), ctrl=True)
            ok1, px1, _ = xs.wait_for_pixel_near(win, 200, 200, pal[0], timeout_sec=1.0)
            dt1 = (time.perf_counter() - t0) * 1000.0; xs.close_gracefully(win)
        ensure(ok1 and dt1 < 16.0, f"9-tab cached_fb retention/switch failed: ok={ok1} dt={dt1:.2f}ms px={px1}")
        return f"9-tab (>=8 inactive) cached_fb retention & {dt1:.2f}ms switch verified"
    finally:
        if owns:
            srv.stop()


def _t2_f16_04(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        blob = "A" * 9000
        long_url = f"{srv.main_origin}/long-nav?pad={blob}&tail=OMNI9K"
        with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T2_F16_04"), "x11"), initial_url=long_url, extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as xs:
            win = xs.wait_for_waterscape_window()
            ensure(xs.wait_for_pixel_near(win, 60, 110, (22, 163, 74), timeout_sec=5.0)[0], "Initial >9KB URL load failed")
            cur = srv.mark_cursor()
            xs.press_keysym(ord("l"), ctrl=True); time.sleep(0.06); xs.press_keysym(XK_Return); time.sleep(0.4)
            xs.close_gracefully(win)
        hits = [t for t in srv.get_traces(since=cur, path_prefix="/long-nav") if t.get("raw_target_len", 0) >= 9000 and "tail=OMNI9K" in t.get("query", "")]
        ensure(len(hits) >= 1, f"Omnibar truncated >=8KB URL on Ctrl+L -> Enter (hits={len(hits)})")
        return ">=8KB (9KB+) shell omnibar URL preservation without 512B truncation verified"
    finally:
        if owns:
            srv.stop()


def _t2_f16_05(ctx: RunnerContext) -> str:
    w = _verify_page(ctx, "T2_F16_05", '<!DOCTYPE html><html><body style="margin:0;background:#2563eb"><div style="width:80px;height:80px;background:#fff">R</div></body></html>', window_size="520x640")
    sl = w.dump_json.get("shell_layout") or {}
    ab, ts, nv, cv = sl.get("address_bar_rect") or {}, sl.get("tab_strip_rect") or {}, sl.get("nav_toolbar_rect") or {}, sl.get("content_viewport") or {}
    ensure(w.ok and ab.get("width", 0) >= 120 and ts.get("y", 0) + ts.get("height", 0) <= nv.get("y", 0) and nv.get("y", 0) + nv.get("height", 0) <= cv.get("y", 0), f"Narrow 520x640 shell overlap: {sl}")
    with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T2_F16_05"), "x11"), extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as xs:
        win = xs.wait_for_waterscape_window()
        for rw, rh in ((520, 640), (1400, 850), (640, 520), (800, 600)):
            xs.resize_window(win, rw, rh); time.sleep(0.05)
        time.sleep(0.15); _, _, gw, gh = xs.get_window_geometry(win); rc = xs.close_gracefully(win)
    ensure(rc == 0 and (gw, gh) == (800, 600), f"Resize storm failed: rc={rc} geom={(gw, gh)}")
    return "Narrow 520x640 non-overlapping chrome & rapid X11 ConfigureNotify resize storm verified"


def _t2_f16_06(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        u = srv.register_route(
            "/f16/wheel_burst",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": "<!DOCTYPE html><html><body style='margin:0;background:#fff'>"
                "<div style='height:620px;background:#2563eb'></div>"
                "<div style='height:900px;background:#16a34a'></div>"
                "</body></html>",
            },
        )
        with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T2_F16_06"), "x11"), initial_url=u, extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as xs:
            win = xs.wait_for_waterscape_window()
            ensure(xs.wait_for_pixel_near(win, 280, 220, (37, 99, 235), timeout_sec=5.0)[0], "Initial top blue band missing")
            # Overscroll upward burst at scrollY=0 must clamp safely at 0
            xs.scroll_wheel(win, 280, 220, down=False, clicks=10)
            ensure(xs.wait_for_pixel_near(win, 280, 220, (37, 99, 235), timeout_sec=2.0)[0], "Upward overscroll corrupted top band")
            # Rapid downward wheel burst into green region + overscroll clamping
            xs.scroll_wheel(win, 280, 220, down=True, clicks=24)
            ensure(xs.wait_for_pixel_near(win, 280, 220, (22, 163, 74), timeout_sec=3.0)[0], "Downward Button5 wheel burst did not scroll to green band")
            # Rapid upward wheel burst back to top blue region
            xs.scroll_wheel(win, 280, 220, down=False, clicks=28)
            ensure(xs.wait_for_pixel_near(win, 280, 220, (37, 99, 235), timeout_sec=3.0)[0], "Upward Button4 wheel burst did not return to top blue band")
            rc = xs.close_gracefully(win)
        ensure(rc == 0, f"X11 wheel burst session exited non-zero: {rc}")
        return "Rapid X11 Button4/Button5 wheel bursts & top/bottom overscroll clamping verified"
    finally:
        if owns:
            srv.stop()


# --- Feature F17: Linux X11 Hangul XIM Input & Win32 Single-Frame Parity (Tier 2) ---
def _t2_f17_01(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        submitted: list[str] = []

        def _on_post(req: dict[str, typing.Any]) -> dict[str, typing.Any]:
            qs = urllib.parse.parse_qs(req.get("query", ""), keep_blank_values=True)
            submitted.append(qs.get("v", [""])[0])
            return {"status": 200, "content_type": "application/json", "body": '{"ok":true}'}

        srv.register_route("/f17/xic_edge", _on_post)
        html = """<!DOCTYPE html><html><body style="margin:0;background:#fff;padding:16px">
        <input id="inp" type="text" style="width:240px;height:44px;font-size:24px">
        <button id="btn" style="width:100px;height:44px;background:#2563eb;color:#fff;border:0">OK</button>
        <div id="done" style="margin-top:10px;width:120px;height:32px;background:#cbd5e1"></div>
        <script>
        document.getElementById('btn').onclick = async () => {
          await fetch('/f17/xic_edge?v=' + encodeURIComponent(document.getElementById('inp').value));
          document.getElementById('done').style.background = '#16a34a';
        };
        </script></body></html>"""
        u = srv.register_route("/t2_f17_01/page", {"status": 200, "content_type": "text/html; charset=utf-8", "body": html})
        with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T2_F17_01"), "x11"), initial_url=u, extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as xs:
            win = xs.wait_for_waterscape_window()
            ensure(xs.wait_for_pixel_near(win, 290, 76 + 34, (37, 99, 235), timeout_sec=5.0)[0], "Button did not paint")
            xs.click_window(win, 80, 76 + 34)
            time.sleep(0.05)
            xs.send_focus_cycle(win)
            xs.compose_multi_key("zz")  # unmapped invalid sequence -> must not wedge XIC
            time.sleep(0.04)
            xs.compose_multi_key("rk")  # -> "가"
            time.sleep(0.04)
            xs.press_keycode(KC_HANGUL_HAN)  # -> "한"
            time.sleep(0.04)
            xs.press_keysym(XK_BackSpace)  # must delete full 3-byte UTF-8 codepoint "한", leaving clean "가"
            time.sleep(0.04)
            xs.press_keycode(KC_HANGUL_GEUL)  # -> "글" => "가글"
            time.sleep(0.05)
            xs.click_window(win, 290, 76 + 34)
            ok_done, _, _ = xs.wait_for_pixel_near(win, 50, 76 + 82, (22, 163, 74), timeout_sec=4.0)
            xs.close_gracefully(win)
        ensure(ok_done and submitted and "\ufffd" not in submitted[-1] and submitted[-1] == "가글", f"XIC focus/invalid-compose/UTF-8 BackSpace failed: {submitted}")
        return f"X11 XIC FocusOut/In + invalid Compose recovery + UTF-8 codepoint BackSpace verified ({submitted[-1]!r})"
    finally:
        if owns:
            srv.stop()


def _t2_f17_02(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        u = srv.register_route("/t2_f17_02/page", {"status": 200, "content_type": "text/html; charset=utf-8", "body": "<!DOCTYPE html><html><body style='margin:0;background:#16a34a'></body></html>"})
        with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T2_F17_02"), "x11"), initial_url=u, extra_args=["--conformance", "--private", "--window-size", "1280x800"]) as xs:
            win = xs.wait_for_waterscape_window()
            ensure(xs.wait_for_pixel_near(win, 300, 200, (22, 163, 74), timeout_sec=5.0)[0], "Initial green content missing")
            xs.press_keysym(XK_F11)
            time.sleep(0.18)
            fs_top, _, _ = xs.wait_for_pixel_near(win, 300, 12, (22, 163, 74), timeout_sec=2.0)
            ensure(fs_top, "F11 fullscreen did not expand content to top y=12")
            xs.press_keysym(XK_F11)
            time.sleep(0.18)
            px_chrome = xs.read_window_pixel(win, 300, 12)
            ensure(px_chrome is not None and abs(px_chrome[1] - 163) > 25, f"Exiting F11 did not restore top tab-strip chrome: {px_chrome}")
            xs.press_keysym(XK_F12)
            time.sleep(0.15)
            ensure(len(xs.list_viewable_windows()) == 1, "F12 spawned extra X11 top-level window")
            xs.press_keysym(XK_Escape)
            rc = xs.close_gracefully(win)
        ensure(rc == 0, f"F11/F12 round-trip session exited {rc}")
        return "X11 F11 fullscreen round-trip & F12 single-window DevTools dock verified"
    finally:
        if owns:
            srv.stop()


def _t2_f17_03(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        u = srv.register_route("/t2_f17_03/page", {"status": 200, "content_type": "text/html; charset=utf-8", "body": "<!DOCTYPE html><html><body style='margin:0;background:#2563eb;color:#fff;font-size:32px'>한글 CPU Presenter</body></html>"})
        with X11Session(ctx.waterscape_bin, ctx.module_dir, os.path.join(ctx.test_dir("T2_F17_03"), "x11"), initial_url=u, extra_args=["--conformance", "--private", "--force-cpu", "--window-size", "960x640"]) as xs:
            win = xs.wait_for_waterscape_window()
            ensure(xs.wait_for_pixel_near(win, 400, 260, (37, 99, 235), timeout_sec=5.0)[0], "--force-cpu XPutImage fallback did not paint blue viewport")
            rc = xs.close_gracefully(win)
        ensure(rc == 0, f"--force-cpu X11 session failed to close cleanly: rc={rc}")
        return "X11 --force-cpu XPutImage presenter fallback & Hangul render verified"
    finally:
        if owns:
            srv.stop()


def _t2_f17_04(ctx: RunnerContext) -> str:
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"></head><body style="margin:0;height:2200px;background:#fff">
    <header style="position:fixed;top:0;left:0;width:100%;height:56px;background:#1e293b">
      <button id="fbtn" style="position:absolute;left:40px;top:8px;width:120px;height:40px;border:0;background:#dc2626;color:#fff">FIX</button>
    </header>
    <script>const fb=document.getElementById('fbtn');fb.onclick=()=>{fb.style.background='#16a34a';document.title='FIXED_HIT_600';};</script></body></html>"""
    w = _verify_page(
        ctx,
        "T2_F17_04",
        html,
        window_size="520x640",
        profile="minimum",
        theme="classic",
        force_classic_frame=True,
        force_cpu=True,
        scroll_y=600,
        click=(80, 24),
        save_png=True,
    )
    ensure(w.ok and w.dump_json.get("title") == "FIXED_HIT_600", f"Classic CPU fixed>absolute hit-test at scroll_y=600 failed: {w.dump_json.get('title')!r} {w.guard_failures}")
    sl = w.dump_json.get("shell_layout") or {}
    cap = sl.get("window_caption_rect") or {}
    ts = sl.get("tab_strip_rect") or {}
    nv = sl.get("nav_toolbar_rect") or {}
    cv = sl.get("content_viewport") or {}
    ensure(cap.get("height", 0) > 0 and cap["y"] + cap["height"] <= ts["y"] and ts["y"] + ts["height"] <= nv["y"] and nv["y"] + nv["height"] <= cv["y"], f"520x640 classic-frame rect overlap: {sl}")
    return "Win2000 classic 520x640 non-overlap & --scroll-y 600 fixed>absolute hit-test verified"


def _t2_f17_05(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        u_static = srv.register_route("/f17/static_min", {"status": 200, "content_type": "text/html; charset=utf-8", "body": "<!DOCTYPE html><html><head><title>STATIC_MIN</title></head><body style='margin:0;background:#16a34a'><div style='width:100px;height:50px;background:#fff'>S</div></body></html>"})
        w_st = run_waterscape_verify(ctx, u_static, test_id="T2_F17_05_static", profile="minimum", force_cpu=True)
        st_stats = w_st.dump_json.get("stats") or {}
        ensure(w_st.ok and st_stats.get("js_module_loaded") is False and int(st_stats.get("js_heap_bytes", -1)) == 0, f"Minimum profile eagerly loaded JS on scriptless HTML: {st_stats}")
        big_dom = "".join(f"<span>n{i}</span>" for i in range(1200))
        u1 = srv.register_route("/f17/dom1", {"status": 200, "content_type": "text/html; charset=utf-8", "body": f"<!DOCTYPE html><html><body>{big_dom}</body></html>"})
        u2 = srv.register_route("/f17/dom2", {"status": 200, "content_type": "text/html; charset=utf-8", "body": "<!DOCTYPE html><html><head><title>ARENA_RESET_OK</title></head><body style='margin:0;background:#2563eb'><div style='width:80px;height:40px;background:#fff'>2</div></body></html>"})
        w_re = run_waterscape_verify(ctx, u1, second_url=u2, test_id="T2_F17_05_reset", profile="minimum", force_cpu=True)
        ensure(w_re.ok and w_re.dump_json.get("title") == "ARENA_RESET_OK", f"Minimum profile sequential arena reset failed: {w_re.guard_failures}")
        return "Minimum profile Tier-4 lazy JS guard & sequential navigation arena reset verified"
    finally:
        if owns:
            srv.stop()


# --- Feature F18: Live Chrome-Grade --conformance & Interactive X11 QA (Tier 2) ---
def _t2_f18_01(ctx: RunnerContext) -> str:
    probe = "(() => { const h = document.querySelector('input[type=\"hidden\"][name=\"q\"]'); const leak = h ? (getComputedStyle(h).display !== 'none' || h.getBoundingClientRect().height > 0) : false; console.log('GDEF:' + JSON.stringify({leak, fake: !!document.querySelector('[data-arcr-hydrated]')})); })();"
    for mode_name, conf in (("default", False), ("conformance", True)):
        w = run_waterscape_verify(ctx, "https://www.google.com/?hl=ko", test_id=f"T2_F18_01_{mode_name}", conformance=conf, wait_load_ms=10000, eval_js=probe, timeout_sec=28.0)
        ensure(w.ok, f"google.com ({mode_name}) guard failures: {w.guard_failures}")
        m = re.search(r"GDEF:(\{.*\})", str(w.dump_json.get("console_log", "")))
        if m:
            meta = json.loads(m.group(1))
            ensure(meta.get("leak") is False and meta.get("fake") is False, f"google.com ({mode_name}) hidden input[name=q] or facade leak: {meta}")
    return "Live google.com default vs --conformance parity & zero hidden-input leak verified"


def _t2_f18_02(ctx: RunnerContext) -> str:
    probe = "(() => { console.log('YTDEF:' + JSON.stringify({fake: !!document.querySelector('[data-arcr-hydrated]'), app: !!document.querySelector('ytd-app,ytm-app')})); })();"
    for mode_name, conf in (("default", False), ("conformance", True)):
        w = run_waterscape_verify(ctx, "https://www.youtube.com/watch?v=jfKfPfyJRdk", test_id=f"T2_F18_02_{mode_name}", conformance=conf, wait_load_ms=16000, eval_js=probe, timeout_sec=40.0)
        ensure(w.ok and w.content_png is not None, f"youtube.com Watch ({mode_name}) failed: {w.guard_failures}")
        grey_only, reason = w.content_png.is_grey_skeleton_only()
        ensure(not grey_only, f"youtube.com Watch ({mode_name}) grey skeleton: {reason}")
        m = re.search(r"YTDEF:(\{.*\})", str(w.dump_json.get("console_log", "")))
        if m:
            meta = json.loads(m.group(1))
            ensure(meta.get("fake") is False and meta.get("app") is True, f"youtube.com Watch ({mode_name}) deceptive hydrator or missing ytd-app: {meta}")
    return "Live youtube.com Watch default vs --conformance zero-hydrator parity verified"


def _t2_f18_03(ctx: RunnerContext) -> str:
    probe = "(() => { const fake = !!document.querySelector('[data-arcr-hydrated]'); const bleed = Array.from(document.querySelectorAll('svg')).some(s => { const r = s.getBoundingClientRect(); return !!s.querySelector('symbol') && r.width > 4 && r.height > 4 && r.left === 0 && r.top === 0; }); console.log('XDEF:' + JSON.stringify({fake, bleed})); })();"
    for mode_name, conf in (("default", False), ("conformance", True)):
        w = run_waterscape_verify(ctx, "https://x.com/", test_id=f"T2_F18_03_{mode_name}", conformance=conf, window_size="800x600", wait_load_ms=14000, eval_js=probe, timeout_sec=36.0)
        ensure(w.ok and w.content_png is not None and w.content_png.region_stats()["distinct_colors"] >= 6, f"x.com 800x600 ({mode_name}) failed: {w.guard_failures}")
        m = re.search(r"XDEF:(\{.*\})", str(w.dump_json.get("console_log", "")))
        if m:
            meta = json.loads(m.group(1))
            ensure(meta.get("fake") is False and meta.get("bleed") is False, f"x.com ({mode_name}) hydrator or (0,0) SVG sprite bleed: {meta}")
    return "Live x.com default vs --conformance 800x600 responsive & zero-bleed parity verified"


def _t2_f18_04(ctx: RunnerContext) -> str:
    w = run_waterscape_verify(ctx, "https://www.zuzunza.com/", test_id="T2_F18_04", conformance=True, dpi=192, scroll_y=480, wait_load_ms=12000, timeout_sec=32.0)
    ensure(w.ok and w.content_png is not None, f"zuzunza.com 192 DPI scrolled failed: {w.guard_failures}")
    grey_only, reason = w.content_png.is_grey_skeleton_only()
    ensure(not grey_only, f"zuzunza.com 192 DPI scrolled grey/blank: {reason}")
    sl = w.dump_json.get("shell_layout") or {}
    ensure(int((sl.get("tab_strip_rect") or {}).get("height", 0)) >= 64, f"192 DPI chrome did not scale on live zuzunza.com: {sl}")
    return "Live www.zuzunza.com 192 DPI HiDPI + scroll_y=480 --conformance verified"


def _t2_f18_05(ctx: RunnerContext) -> str:
    w = run_waterscape_verify(ctx, "https://www.youtube.com/", second_url="https://www.google.com/?hl=ko", test_id="T2_F18_05", conformance=True, wait_load_ms=12000, timeout_sec=38.0)
    ensure(w.ok and w.content_png is not None, f"Live YouTube -> Google rapid --second-url cancellation failed: {w.guard_failures}")
    final_u = str(w.dump_json.get("url", "")).lower()
    ensure("google." in final_u and "youtube." not in final_u, f"Stale first live navigation leaked into final URL: {final_u!r}")
    return "Live YouTube -> Google rapid cross-origin navigation cancellation verified"


# --- Feature F19: Clause-Honest standards_2026_inventory.json & Repo/Doc Sync (Tier 2) ---
def _read_inv(ctx: RunnerContext) -> dict[str, typing.Any]:
    inv_p = os.path.join(ctx.repo_root, "docs", "standards_2026_inventory.json")
    ensure(os.path.isfile(inv_p), f"Missing {inv_p}")
    return json.loads(open(inv_p, "r", encoding="utf-8").read())


def _t2_f19_01(ctx: RunnerContext) -> str:
    import socket as _sock
    import threading as _thr

    inv = _read_inv(ctx)
    entries = inv.get("entries") or []
    srv, owns = _ensure_fixture(ctx)
    try:
        srv.register_route("/f19/https_probe", {"status": 200, "content_type": "text/html; charset=utf-8", "body": "<!DOCTYPE html><html><head><title>TLS_OK</title></head><body style='margin:0;background:#16a34a'><div style='width:80px;height:40px;background:#fff'>H</div></body></html>"})
        https_url = f"{srv.https_origin}/f19/https_probe"
        w = run_waterscape_verify(ctx, https_url, test_id="T2_F19_01", env_overrides={"SSL_CERT_FILE": srv.ca_cert, "ARCR_SSL_CERT_FILE": srv.ca_cert})
        ensure(w.ok and w.dump_json.get("title") == "TLS_OK", f"Trusted HTTPS probe failed: {w.guard_failures}")
        # Capture raw TLS ClientHello wire bytes from waterscape to check whether ALPN offers 'h2'
        ch_bytes: list[bytes] = []
        lsock = _sock.socket(_sock.AF_INET, _sock.SOCK_STREAM)
        lsock.bind(("127.0.0.1", 0))
        lsock.listen(2)
        lsock.settimeout(4.0)
        lport = lsock.getsockname()[1]

        def _grab_ch() -> None:
            try:
                cs, _ = lsock.accept()
                cs.settimeout(2.0)
                ch_bytes.append(cs.recv(2048))
                cs.close()
            except Exception:
                pass
            finally:
                lsock.close()

        gt = _thr.Thread(target=_grab_ch, daemon=True)
        gt.start()
        run_waterscape_verify(ctx, f"https://127.0.0.1:{lport}/alpn", test_id="T2_F19_01_alpn", wait_load_ms=800, timeout_sec=5.0, check_guards=False)
        gt.join(timeout=2.0)
        raw_ch = ch_bytes[0] if ch_bytes else b""
        offered_h2 = b"\x02h2" in raw_ch
        if not offered_h2:
            overclaiming_h2 = [
                e["id"]
                for e in entries
                if e.get("status_code") == "verified"
                and "http/2" in (str(e.get("spec_name", "")) + " " + str(e.get("applicability_rationale", ""))).lower()
                and "unimplemented" not in str(e.get("applicability_rationale", "")).lower()
            ]
            ensure(len(overclaiming_h2) == 0, f"G5 HTTP/1.1 vs HTTP/2 contradiction: TLS ClientHello lacks h2 ALPN, yet verified entries claim HTTP/2: {overclaiming_h2}")
        return f"Empirical HTTPS + TLS ClientHello ALPN probe (offered_h2={offered_h2}) vs inventory verified"
    finally:
        if owns:
            srv.stop()


def _t2_f19_02(ctx: RunnerContext) -> str:
    inv = _read_inv(ctx)
    entries = inv.get("entries") or []
    html = """<!DOCTYPE html><html><head><meta charset="utf-8"></head><body style="margin:0;background:#2563eb"><div style="width:80px;height:40px;background:#fff">P</div><script>
    (async () => {
      const c = document.createElement('canvas');
      let wasmReal = false, wasmPresent = (typeof WebAssembly === 'object' && WebAssembly !== null);
      if (wasmPresent) {
        try {
          const m = new Uint8Array([0,97,115,109,1,0,0,0,1,7,1,96,2,127,127,1,127,3,2,1,0,7,7,1,3,97,100,100,0,0,10,9,1,7,0,32,0,32,1,106,11]);
          const { instance } = await WebAssembly.instantiate(m);
          wasmReal = instance.exports.add(20, 22) === 42;
        } catch (_) { wasmReal = false; }
      }
      document.title = JSON.stringify({
        wt: typeof WebTransport !== 'undefined',
        sw: 'serviceWorker' in navigator,
        gpu: 'gpu' in navigator && navigator.gpu !== null && navigator.gpu !== undefined,
        webgl2: c.getContext('webgl2') !== null,
        wasmHollow: wasmPresent && !wasmReal,
        cssBogus: CSS.supports('arcr-nonexistent-prop: 1px')
      });
    })();
    </script></body></html>"""
    w = _verify_page(ctx, "T2_F19_02", html)
    ensure(w.ok, f"Guard failures: {w.guard_failures}")
    probe = json.loads(str(w.dump_json.get("title", "{}")))
    ensure(probe.get("wasmHollow") is False and probe.get("cssBogus") is False, f"Hollow runtime stub detected: {probe}")
    unimpl_blob = " ".join(
        (str(e.get("id", "")) + " " + str(e.get("spec_name", "")) + " " + " ".join(e.get("blocking_issues") or [])).lower()
        for e in entries
        if e.get("status_code") in ("unimplemented", "partial")
    )
    if "webtransport" in unimpl_blob:
        ensure(probe["wt"] is False, "WebTransport exposed on globalThis despite unimplemented inventory status")
    if "serviceworker" in unimpl_blob:
        ensure(probe["sw"] is False, "navigator.serviceWorker exposed despite unimplemented inventory status")
    if "webgpu" in unimpl_blob:
        ensure(probe["gpu"] is False, "navigator.gpu non-null despite unimplemented inventory status")
    return f"Empirical unimplemented/partial API honesty probe matched inventory: {probe}"


def _t2_f19_03(ctx: RunnerContext) -> str:
    inv = _read_inv(ctx)
    entries = inv.get("entries") or []
    ensure(len(entries) >= 28, f"Inventory must be expanded to >=28 clause entries, got {len(entries)}")
    for e in entries:
        eid = str(e.get("id", ""))
        sc = e.get("status_code")
        bi = e.get("blocking_issues") or []
        if sc == "verified" or e.get("modern_status") == "verified":
            ensure(len(bi) == 0, f"G5 contradiction: {eid} marked verified with blocking_issues={bi}")
        if not e.get("applicable", True) or sc == "not_applicable":
            txt = (eid + " " + str(e.get("spec_name", ""))).lower()
            for std_kw in ("http/3", "webtransport", "webgpu", "webassembly", "serviceworker", "indexeddb", "websocket"):
                ensure(std_kw not in txt, f"Denominator abuse: standard web spec {std_kw!r} marked not_applicable in {eid}")
    return "Zero not_applicable denominator abuse & zero verified+blocking_issues contradictions verified"


def _t2_f19_04(ctx: RunnerContext) -> str:
    inv = _read_inv(ctx)
    legend = inv.get("status_legend") or {}
    expected_codes = {"verified", "implemented", "partial", "unimplemented", "not_applicable"}
    ensure(set(legend.keys()) == expected_codes, f"status_legend keys mismatch: {set(legend.keys())}")
    entries = inv.get("entries") or []
    tls_rows = [
        e for e in entries
        if "tls" in (str(e.get("id", "")) + " " + str(e.get("spec_name", "")) + " " + str(e.get("applicability_rationale", ""))).lower()
    ]
    ensure(len(tls_rows) >= 1, "Missing TLS entry in standards_2026_inventory.json")
    ensure(
        any(e.get("win2000_legacy_status") in ("partial", "unimplemented") for e in tls_rows),
        f"Win2000 legacy Schannel cannot claim full native TLS 1.3 verified across all TLS rows: {[ (e['id'], e.get('win2000_legacy_status')) for e in tls_rows ]}",
    )
    return "Platform split realism (modern_status vs win2000_legacy_status TLS 1.3) & status_legend verified"


def _t2_f19_05(ctx: RunnerContext) -> str:
    inv = _read_inv(ctx)
    sha = str(inv.get("baseline_investigation_sha", ""))
    ensure(bool(re.fullmatch(r"[0-9a-f]{7,40}", sha)), f"Invalid baseline_investigation_sha: {sha!r}")
    entries = inv.get("entries") or []
    ensure(len(entries) >= 28, f"Expected >=28 clause entries in inventory, got {len(entries)}")
    for e in entries:
        eid = e.get("id")
        if e.get("status_code") == "verified":
            tids = e.get("test_ids") or []
            vcmd = str(e.get("verification_command", "")).strip()
            ensure(len(tids) >= 1 and len(vcmd) >= 3 and vcmd.lower() != "n/a", f"Verified entry {eid} lacks test_ids or verification_command")
            for tid in tids:
                if isinstance(tid, str) and tid.startswith("WPT:/"):
                    wpt_rel = os.path.join("tests", "wpt", tid[len("WPT:/"):])
                    ensure(os.path.isfile(os.path.join(ctx.repo_root, wpt_rel)), f"Entry {eid} cites missing WPT test {wpt_rel}")
    rep_p = os.path.join(ctx.repo_root, "docs", "verification", "verification_report_2026.md")
    rep_txt = open(rep_p, "r", encoding="utf-8").read()
    ensure(bool(re.search(r"`[0-9a-f]{7,40}`", rep_txt)), "verification_report_2026.md missing valid commit SHA")
    return "Inventory SHA, verified test_ids, WPT file references & report SHA integrity verified"


TESTS: list[TestCaseSpec] = [
    # F12 (5)
    _spec("T2_F12_01", "F12", "Dynamic <slot> 0->1->0 light-DOM mutation & fallback repaint", _t2_f12_01),
    _spec("T2_F12_02", "F12", "Closed ShadowRoot encapsulation, duplicate attachShadow rejection & nested forwarded slot", _t2_f12_02),
    _spec("T2_F12_03", "F12", "Unmatched slot light-DOM suppression & outer !important shadow isolation", _t2_f12_03),
    _spec("T2_F12_04", "F12", ":host(.danger) specificity & invalid descendant ::slotted(.it span) rejection", _t2_f12_04),
    _spec("T2_F12_05", "F12", "Offset slotted light-DOM elementFromPoint & --click dispatch", _t2_f12_05),
    # F13 (5)
    _spec("T2_F13_01", "F13", "Circular @import cycle termination & 404 @import recovery", _t2_f13_01),
    _spec("T2_F13_02", "F13", "@property inherits:false isolation & invalid syntax initial-value fallback", _t2_f13_02),
    _spec("T2_F13_03", "F13", "Mid-flight CSS transition reversal & zero spurious transitionend", _t2_f13_03),
    _spec("T2_F13_04", "F13", "Malformed calc()/clamp() recovery & nested dvh clamp boundary", _t2_f13_04),
    _spec("T2_F13_05", "F13", "<picture> unsupported image/jxl & non-matching media skip + Accept negotiation", _t2_f13_05),
    # F14 (5)
    _spec("T2_F14_01", "F14", "Corrupt & 404 @font-face WOFF2 fallback chain to valid WOFF2 face", _t2_f14_01),
    _spec("T2_F14_02", "F14", "@font-face unicode-range U+0030-0039 selective glyph binding", _t2_f14_02),
    _spec("T2_F14_03", "F14", "RGBA color emoji immunity to CSS color override & dark-bg compositing", _t2_f14_03),
    _spec("T2_F14_04", "F14", "Astral Mathematical Alphanumeric (𝐋𝐨𝐟𝐢 / 𝕏 / 𝄞) proportional zero-tofu metrics", _t2_f14_04),
    _spec("T2_F14_05", "F14", "ZWJ, skin-tone & regional-flag emoji UTF-16 surrogates + trailing Hangul AA", _t2_f14_05),
    # F15 (5)
    _spec("T2_F15_01", "F15", "Zero-bleed across 6 .sr-only / skip-link clipping & offscreen patterns", _t2_f15_01),
    _spec("T2_F15_02", "F15", "Hidden width=0/display:none <svg> zero-(0,0)-bleed & cyclic/missing <use> safety", _t2_f15_02),
    _spec("T2_F15_03", "F15", "48-track CSS Grid (>=32 tracks) & negative explicit grid-column -5/-1 placement", _t2_f15_03),
    _spec("T2_F15_04", "F15", "Nested scrollTop negative/overflow clamping & strict scrollport clip bounds", _t2_f15_04),
    _spec("T2_F15_05", "F15", "Scrolled nested container hit-testing (--click + elementFromPoint) & root IntersectionObserver", _t2_f15_05),
    # F16 (6)
    _spec("T2_F16_01", "F16", "Stale subresource nav_gen cancellation on rapid --second-url navigation", _t2_f16_01),
    _spec("T2_F16_02", "F16", "Non-blocking <16 ms Ctrl+Tab switch during 2.2s slow subresource fetch", _t2_f16_02),
    _spec("T2_F16_03", "F16", "9-tab (>=8 inactive) cached_fb retention & <16 ms Ctrl+1 switch", _t2_f16_03),
    _spec("T2_F16_04", "F16", ">=8 KB (9 KB+) shell omnibar URL preservation without 512 B truncation", _t2_f16_04),
    _spec("T2_F16_05", "F16", "Narrow 520x640 non-overlapping chrome & rapid X11 ConfigureNotify resize storm", _t2_f16_05),
    _spec("T2_F16_06", "F16", "Rapid X11 Button4/Button5 wheel bursts & top/bottom overscroll clamping", _t2_f16_06),
    # F17 (5)
    _spec("T2_F17_01", "F17", "X11 XIC FocusOut/FocusIn, invalid Compose recovery & UTF-8 codepoint BackSpace", _t2_f17_01),
    _spec("T2_F17_02", "F17", "X11 F11 fullscreen round-trip & F12 single-window DevTools toggle", _t2_f17_02),
    _spec("T2_F17_03", "F17", "X11 --force-cpu XPutImage presenter fallback & Hangul rendering", _t2_f17_03),
    _spec("T2_F17_04", "F17", "Win2000 classic 520x640 non-overlap & --scroll-y 600 fixed>absolute hit-testing", _t2_f17_04),
    _spec("T2_F17_05", "F17", "Minimum profile scriptless Tier-4 lazy JS guard & sequential arena reset", _t2_f17_05),
    # F18 (5)
    _spec("T2_F18_01", "F18", "Live google.com default vs --conformance parity & zero hidden-input leak", _t2_f18_01, related_features=("F06", "F11"), is_live=True),
    _spec("T2_F18_02", "F18", "Live youtube.com Watch default vs --conformance zero-hydrator parity", _t2_f18_02, related_features=("F11", "F12"), is_live=True),
    _spec("T2_F18_03", "F18", "Live x.com default vs --conformance 800x600 responsive & zero-bleed parity", _t2_f18_03, related_features=("F11", "F15"), is_live=True),
    _spec("T2_F18_04", "F18", "Live www.zuzunza.com 192 DPI HiDPI + scroll_y=480 --conformance QA", _t2_f18_04, related_features=("F15", "F17"), is_live=True),
    _spec("T2_F18_05", "F18", "Live YouTube -> Google rapid --second-url cross-origin cancellation", _t2_f18_05, related_features=("F16",), is_live=True),
    # F19 (5)
    _spec("T2_F19_01", "F19", "Empirical HTTPS ALPN probe vs standards_2026_inventory.json HTTP/1.1 vs HTTP/2 claim", _t2_f19_01),
    _spec("T2_F19_02", "F19", "Empirical runtime probe vs unimplemented/partial inventory status (zero hollow stubs)", _t2_f19_02),
    _spec("T2_F19_03", "F19", "not_applicable denominator-abuse guard & blocking_issues vs verified contradiction check", _t2_f19_03),
    _spec("T2_F19_04", "F19", "Platform split realism (modern_status vs win2000_legacy_status TLS 1.3) & status_legend", _t2_f19_04),
    _spec("T2_F19_05", "F19", "Inventory SHA, verified test_ids, WPT file references & report SHA integrity", _t2_f19_05),
]


def get_tests() -> list[TestCaseSpec]:
    return list(TESTS)


get_tier2_f12_f19_tests = get_tests

