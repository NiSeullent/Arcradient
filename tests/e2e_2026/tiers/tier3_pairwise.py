import sys
sys.dont_write_bytecode = True

import hashlib
import json
import os
import re
import shutil
import subprocess
import time
import urllib.parse

try:
    from ..harness.fixture_server import compress_brotli_real
    from ..harness.types_and_guards import (
        AUTHORITATIVE_FEATURES,
        AssertionFailure,
        PNGImage,
        RunnerContext,
        TestCaseSpec,
        TestResult,
        ensure,
        read_png_rgba,
        run_arcr_js,
        run_waterscape_verify,
    )
    from ..harness.x11_driver import (
        KC_HANGUL_HAN,
        X11Session,
        XK_Escape,
        XK_F11,
        XK_F12,
        XK_Return,
        XK_Tab,
    )
except ImportError:
    from harness.fixture_server import compress_brotli_real
    from harness.types_and_guards import (
        AUTHORITATIVE_FEATURES,
        AssertionFailure,
        PNGImage,
        RunnerContext,
        TestCaseSpec,
        TestResult,
        ensure,
        read_png_rgba,
        run_arcr_js,
        run_waterscape_verify,
    )
    from harness.x11_driver import (
        KC_HANGUL_HAN,
        X11Session,
        XK_Escape,
        XK_F11,
        XK_F12,
        XK_Return,
        XK_Tab,
    )

TestCase = TestCaseSpec


def _parse_title_json(res, prefix: str = "JSON:") -> dict:
    title = str(res.dump_json.get("title", ""))
    ensure(title.startswith(prefix), f"Expected title starting with {prefix!r}, got {title!r}; guards={res.guard_failures}")
    raw = title[len(prefix) :]
    try:
        return json.loads(raw)
    except Exception as exc:
        raise AssertionFailure(f"Invalid JSON in document.title={title!r}: {exc}")


# ---------------------------------------------------------------------------
# T3-PW-01 .. T3-PW-14
# ---------------------------------------------------------------------------


def _t3_pw_01_env_setup_wake_and_js_slots_erm(ctx: RunnerContext) -> None:
    """F01 + F02 + F04: Non-interactive /env-setup bashrc wake shell + >32K locals + DisposableStack."""
    env_sh = "/home/antigravity/env-setup.sh"
    env_json = os.path.join(ctx.repo_root, ".agents", "environment.json")
    ensure(os.path.isfile(env_sh) and os.access(env_sh, os.X_OK), f"Missing executable {env_sh}")
    ensure(os.path.isfile(env_json), f"Missing {env_json}")

    tdir = ctx.test_dir("T3-PW-01")
    sb_home = os.path.join(tdir, "sb_home")
    sb_tmp = os.path.join(tdir, "sb_tmp")
    os.makedirs(sb_home, exist_ok=True)
    os.makedirs(sb_tmp, exist_ok=True)
    bashrc = os.path.join(sb_home, ".bashrc")
    with open(bashrc, "w", encoding="utf-8") as f:
        f.write(
            "# Debian default non-interactive guard\n"
            "case $- in\n"
            "    *i*) ;;\n"
            "      *) return;;\n"
            "esac\n"
            "export SENTINEL_AFTER_GUARD=alive\n"
        )

    env = os.environ.copy()
    env["HOME"] = sb_home
    env["TMPDIR"] = sb_tmp
    r1 = subprocess.run(["bash", env_sh], cwd="/tmp", env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=15)
    ensure(r1.returncode == 0, f"env-setup.sh sandbox run 1 failed rc={r1.returncode}: {r1.stderr}")
    after_1 = open(bashrc, "r", encoding="utf-8").read()
    r2 = subprocess.run(["bash", env_sh], cwd="/tmp", env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=15)
    ensure(r2.returncode == 0, f"env-setup.sh sandbox run 2 failed rc={r2.returncode}: {r2.stderr}")
    after_2 = open(bashrc, "r", encoding="utf-8").read()
    ensure(after_1 == after_2, "~/.bashrc grew or mutated on second idempotent run")

    # Generate JS script combining 33,100 local slots (>32768 uint16 wrap) + ES2026 DisposableStack LIFO
    js_path = os.path.join(tdir, "slots_erm.js")
    decls = "\n".join(f"  let v{i} = {i & 7};" for i in range(33100))
    terms = "+".join(f"v{i}" for i in range(0, 33100, 331))
    expected_sum = sum((i & 7) for i in range(0, 33100, 331))
    with open(js_path, "w", encoding="utf-8") as f:
        f.write(
            "const order = [];\n"
            "function runMegaSlots() {\n"
            f"{decls}\n"
            "  {\n"
            "    using s = new DisposableStack();\n"
            "    s.defer(() => order.push('A'));\n"
            "    s.defer(() => order.push('B'));\n"
            "    s.defer(() => order.push('C'));\n"
            f"    return ({terms}) + v33099;\n"
            "  }\n"
            "}\n"
            "const total = runMegaSlots();\n"
            "print(JSON.stringify({ total, order: order.join(',') }));\n"
        )

    wake_cmd = f'source "{bashrc}" && test "$SENTINEL_AFTER_GUARD" = "alive" && "{ctx.arcr_js_bin}" "{js_path}"'
    rw = subprocess.run(["bash", "-c", wake_cmd], cwd=sb_tmp, env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=15)
    ensure(rw.returncode == 0, f"Wake shell + arcr_js failed rc={rw.returncode}: stderr={rw.stderr[:240]}")
    out = json.loads(rw.stdout.strip().splitlines()[-1])
    ensure(out["total"] == expected_sum + (33099 & 7), f"Unexpected 33.1K-slot sum: {out}")
    ensure(out["order"] == "C,B,A", f"Expected LIFO disposal C,B,A, got {out}")


def _t3_pw_02_megabundle_gc_sump_duration(ctx: RunnerContext) -> None:
    """F02 + F03 + F05: 2.4 MB JS bundle (22K string constants + GC churn) + Math.sumPrecise + Map.getOrInsertComputed + Intl.DurationFormat."""
    tdir = ctx.test_dir("T3-PW-02")
    bundle_path = os.path.join(tdir, "mega_2m4.js")
    lines = [
        "const bucketMap = new Map();",
        "const vals = [1e20, 0.1, 0.2, -1e20];",
        "function touch(k, x) {",
        "  const arr = bucketMap.getOrInsertComputed(k, () => []);",
        "  arr.push(x);",
        "}",
    ]
    # 22,000 distinct string literals + padding comments > 2.4 MB to exercise O(1) constant pool & GC
    for i in range(22000):
        lines.append(f'const c_{i} = "const_literal_payload_{i:05d}_" + "x".repeat(48); if ({i} % 550 === 0) touch("g" + ({i} % 5), {i % 13});')
    lines.extend(
        [
            "for (let j = 0; j < 4000; j++) { const tmp = { idx: j, s: c_10 + j }; if (j === 3999) vals.push(0.7); }",
            "const precise = Math.sumPrecise(vals);",
            "const df = new Intl.DurationFormat('en', { style: 'short' }).format({ hours: 2, minutes: 15, seconds: 9 });",
            "print(JSON.stringify({ precise, groups: bucketMap.size, g0Len: bucketMap.get('g0').length, df, tail: c_21999.slice(0, 27) }));",
        ]
    )
    src = "\n".join(lines) + "\n"
    ensure(len(src) >= 2_200_000, f"Expected >=2.2MB bundle, got {len(src)}")
    with open(bundle_path, "w", encoding="utf-8") as f:
        f.write(src)

    res = run_arcr_js(ctx, script_path=bundle_path, timeout_sec=12.0)
    ensure(not res.timed_out and res.returncode == 0, f"Mega-bundle failed rc={res.returncode} dt={res.duration_ms:.1f}ms stderr={res.stderr[:240]}")
    ensure(res.duration_ms < 5000.0, f"O(N^2) compiler regression: 2.2MB bundle took {res.duration_ms:.1f}ms")
    data = json.loads(res.stdout.strip().splitlines()[-1])
    ensure(abs(float(data["precise"]) - 1.0) < 1e-12, f"Math.sumPrecise cancellation failed: {data}")
    ensure(data["groups"] == 5 and data["g0Len"] == 8, f"Map.getOrInsertComputed mismatch: {data}")
    ensure("2" in data["df"] and "15" in data["df"], f"Intl.DurationFormat output invalid: {data}")


def _t3_pw_03_dynamic_script_currentscript_async_using(ctx: RunnerContext) -> None:
    """F04 + F05 + F08: Dynamic <script> appendChild + document.currentScript + await using / SuppressedError + WeakMap.getOrInsertComputed."""
    srv = ctx.fixture_srv
    srv.register_route(
        "/t3/pw03/dyn.js",
        {
            "content_type": "application/javascript; charset=utf-8",
            "body": """
window.__csId = document.currentScript ? document.currentScript.getAttribute('data-tag') : 'NULL';
(async () => {
  const wm = new WeakMap();
  const key = {};
  const meta = wm.getOrInsertComputed(key, () => ({ hits: 41 }));
  meta.hits += 1;
  let suppressedName = 'NONE';
  try {
    await using s = new AsyncDisposableStack();
    s.defer(async () => { throw new Error('dispose_boom'); });
    throw new Error('body_boom');
  } catch (e) {
    suppressedName = e && e.name ? e.name : String(e);
  }
  document.getElementById('status').style.backgroundColor = '#16a34a';
  document.title = 'JSON:' + JSON.stringify({
    cs: window.__csId,
    afterNull: document.currentScript === null,
    hits: wm.get(key).hits,
    sup: suppressedName
  });
})();
""",
        },
    )
    url = srv.register_route(
        "/t3/pw03/index.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title></head>
<body>
<div id="status" style="width:160px;height:60px;background:#dc2626;color:#fff">PW03</div>
<script>
const s = document.createElement('script');
s.src = '/t3/pw03/dyn.js';
s.setAttribute('data-tag', 'DYN_OK_03');
s.onload = () => { window.__loadedFired = true; };
document.body.appendChild(s);
</script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-03")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["cs"] == "DYN_OK_03", f"document.currentScript failed inside dynamic script: {info}")
    ensure(info["afterNull"] is True, f"document.currentScript not reset in async continuation: {info}")
    ensure(info["hits"] == 42, f"WeakMap.getOrInsertComputed failed: {info}")
    ensure(info["sup"] == "SuppressedError", f"Expected SuppressedError from await using, got {info}")
    ensure(res.content_png.count_color_near((22, 163, 74), tol=20, rect=(10, 10, 140, 50)) > 1000, "Green status box not painted")


def _t3_pw_04_long_url_cookie_jar_urlpattern(ctx: RunnerContext) -> None:
    """F06 + F07 + F09: 36 KB URL navigation + HttpOnly/Session Set-Cookie <-> document.cookie + URLPattern."""
    srv = ctx.fixture_srv
    pad = "q" * 35500
    expected_sha = hashlib.sha256(f"pad={pad}&item=99".encode("utf-8")).hexdigest()[:16]

    def _handler(req):
        q_sha = hashlib.sha256(req["query"].encode("utf-8")).hexdigest()[:16]
        return {
            "status": 200,
            "content_type": "text/html; charset=utf-8",
            "headers": [
                ("Set-Cookie", "sid_http=SECRET_HTTPONLY; Path=/; HttpOnly; SameSite=Lax"),
                ("Set-Cookie", "vis_srv=FROM_HTTP; Path=/; SameSite=Lax"),
            ],
            "body": f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title></head>
<body><div style="width:180px;height:60px;background:#2563eb;color:#fff">PW04</div>
<script>
(async () => {{
  document.cookie = "js_tok=FROM_JS; path=/";
  document.cookie = "sid_http=OVERWRITE_ATTEMPT; path=/";
  const pat = new URLPattern({{ pathname: '/t3/pw04/items/:id' }});
  const m = pat.exec(location.href);
  const r = await fetch('/cookie/echo');
  const cj = await r.json();
  document.title = 'JSON:' + JSON.stringify({{
    hrefLen: location.href.length,
    qSha: {json.dumps(q_sha)},
    docCookie: document.cookie,
    wireCookie: cj.cookie,
    itemId: m ? m.pathname.groups.id : null
  }});
}})();
</script></body></html>""",
        }

    srv.register_route("/t3/pw04/items/777", _handler)
    full_url = f"{srv.main_origin}/t3/pw04/items/777?pad={pad}&item=99"
    ensure(len(full_url) >= 35500, "URL must exceed 32 KB")
    res = run_waterscape_verify(ctx, full_url, test_id="T3-PW-04")
    ensure(res.ok, f"Guard failures on 36KB URL: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["hrefLen"] == len(full_url) and info["qSha"] == expected_sha, f"Long URL truncated: {info}")
    ensure(info["itemId"] == "777", f"URLPattern failed on 36KB URL: {info}")
    ensure("SECRET_HTTPONLY" not in info["docCookie"] and "vis_srv=FROM_HTTP" in info["docCookie"], f"HttpOnly leaked or Set-Cookie missing in document.cookie: {info}")
    ensure("sid_http=SECRET_HTTPONLY" in info["wireCookie"] and "js_tok=FROM_JS" in info["wireCookie"], f"Unified HTTP cookie jar missing cookies on fetch: {info}")


def _t3_pw_05_shadow_slot_adopted_sheets_web_anim(ctx: RunnerContext) -> None:
    """F08 + F10 + F12: Dynamic <script> upgrades Custom Element with Shadow <slot>, :host/::slotted, Constructable CSSStyleSheet & Element.animate."""
    srv = ctx.fixture_srv
    url = srv.register_route(
        "/t3/pw05/index.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title>
<style>body { margin: 0; background: #ffffff; } .leak-probe { background: #ffffff; width: 80px; height: 30px; }</style>
</head><body>
<x-hero id="hero" class="ready"><span slot="badge" id="slotted-badge">SLOTTED_TEXT</span></x-hero>
<div class="leak-probe" id="outside"></div>
<script>
const docSheet = new CSSStyleSheet();
docSheet.replaceSync('x-hero { display: block; width: 260px; height: 90px; }');
document.adoptedStyleSheets = [docSheet];

class XHero extends HTMLElement {
  connectedCallback() {
    const sr = this.attachShadow({ mode: 'open' });
    const sh = new CSSStyleSheet();
    sh.replaceSync(':host(.ready) { border-left: 16px solid #16a34a; display: block; } .leak-probe { background: #dc2626; } .box { width: 100px; height: 70px; background: #2563eb; } ::slotted(span) { color: #ffffff; font-weight: bold; }');
    sr.adoptedStyleSheets = [sh];
    sr.innerHTML = '<div class="box" id="animbox"><slot name="badge"></slot></div>';
    const box = sr.getElementById('animbox');
    const anim = box.animate([{ width: '100px' }, { width: '220px' }], { duration: 120, fill: 'forwards' });
    const t0 = performance.now();
    function tick() {
      if (performance.now() - t0 < 160) {
        requestAnimationFrame(tick);
      } else {
        const assigned = sr.querySelector('slot').assignedNodes();
        document.title = 'JSON:' + JSON.stringify({
          w: Math.round(box.getBoundingClientRect().width),
          slottedCount: assigned.length,
          playState: anim.playState
        });
      }
    }
    requestAnimationFrame(tick);
  }
}
customElements.define('x-hero', XHero);
</script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-05")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["slottedCount"] == 1, f"Named <slot> failed to distribute light DOM child: {info}")
    ensure(info["w"] >= 200, f"Element.animate fill:forwards width did not reach ~220px: {info}")
    # Verify :host(.ready) 16px green left border at x=4..12, y=20..50 and blue animated box up to x=190
    ensure(res.content_png.count_color_near((22, 163, 74), tol=24, rect=(2, 10, 12, 50)) >= 200, ":host(.ready) green border missing in PNG")
    ensure(res.content_png.count_color_near((37, 99, 235), tol=28, rect=(170, 15, 35, 40)) >= 400, "Animated Shadow DOM box not painted at x=170..205")
    ensure(res.content_png.count_color_near((220, 38, 38), tol=24, rect=(0, 92, 80, 30)) == 0, "Shadow CSS leaked into light DOM .leak-probe")


def _t3_pw_06_brotli_css_import_property_woff2(ctx: RunnerContext) -> None:
    """F09 + F13 + F14: Brotli CSS + @import + @property + clamp() + genuine @font-face WOFF2 monospace metrics."""
    srv = ctx.fixture_srv

    tokens_css = compress_brotli_real(
        b"@property --card-w { syntax: '<length>'; inherits: false; initial-value: 240px; }\n"
        b"@font-face { font-family: 'PwMonoW2'; src: url('/fonts/mono.woff2') format('woff2'); }\n"
    )
    main_css = compress_brotli_real(
        b"@import url('/t3/pw06/tokens.css');\n"
        b"#card { width: clamp(180px, var(--card-w), 300px); background: #0d9488; padding: 8px; }\n"
        b".m { font-family: 'PwMonoW2', serif; font-size: 30px; color: #111827; }\n"
    )
    srv.register_route("/t3/pw06/tokens.css", {"content_type": "text/css", "headers": {"Content-Encoding": "br"}, "body": tokens_css})
    srv.register_route("/t3/pw06/main.css", {"content_type": "text/css", "headers": {"Content-Encoding": "br"}, "body": main_css})
    url = srv.register_route(
        "/t3/pw06/index.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title>
<link rel="stylesheet" href="/t3/pw06/main.css"></head>
<body style="margin:0;background:#ffffff">
<div id="card"></div>
<div><span class="m" id="si">iiiiiiii</span></div>
<div><span class="m" id="sw">WWWWWWWW</span></div>
<script>
window.addEventListener('load', () => {
  const cw = Math.round(document.getElementById('card').getBoundingClientRect().width);
  const wi = document.getElementById('si').getBoundingClientRect().width;
  const ww = document.getElementById('sw').getBoundingClientRect().width;
  document.title = 'JSON:' + JSON.stringify({ cw, wi, ww, diff: Math.abs(ww - wi) });
});
</script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-06")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(235 <= info["cw"] <= 265, f"@import + @property + clamp() width failed: {info}")
    ensure(info["wi"] > 40 and info["diff"] <= 4.0, f"@font-face WOFF2 monospace glyphs not applied (proportional fallback used): {info}")
    ensure(res.content_png.count_color_near((13, 148, 136), tol=24, rect=(5, 2, 200, 14)) >= 800, "Teal #card from Brotli CSS not painted")


def _t3_pw_07_legacy_charsets_emoji_tofu_sronly(ctx: RunnerContext) -> None:
    """F09 + F14 + F15: EUC-KR HTML + Shift_JIS TextDecoder + RGBA Color Emoji + zero-tofu math symbols + .sr-only clip."""
    srv = ctx.fixture_srv
    url = srv.register_route(
        "/t3/pw07/euckr.html",
        {
            "content_type": "text/html; charset=euc-kr",
            "encoding": "euc_kr",
            "body": """<!DOCTYPE html><html><head><title>WAIT</title>
<style>
body { margin: 0; background: #ffffff; color: #111111; }
.sr1 { position: absolute; clip: rect(1px, 1px, 1px, 1px); background: #ff00ff; color: #00ffff; }
.sr2 { position: absolute; width: 1px; height: 1px; margin: -1px; padding: 0; overflow: hidden; background: #ff00ff; }
#ko { font-size: 20px; line-height: 1.8; margin-top: 8px; }
#em { font-size: 40px; line-height: 1.2; color: #000000; }
#sym { font-size: 40px; line-height: 1.2; color: #000000; }
</style></head><body>
<a class="sr1" href="#ko">본문 바로가기</a>
<div class="sr2">숨김 배너</div>
<div id="ko">한글 문단 첫째 줄입니다<br>둘째 줄 간격 검증</div>
<div id="em">&#x1F525; &#x1F680;</div>
<div id="sym"><span id="sL">&#x1D40B;</span><span id="si">&#x1D422;</span></div>
<script>
(async () => {
  const r = await fetch('/charset-bytes/shift_jis');
  const j = await r.json();
  const sjis = new TextDecoder('shift_jis').decode(new Uint8Array(j.bytes));
  const wL = document.getElementById('sL').getBoundingClientRect().width;
  const wi = document.getElementById('si').getBoundingClientRect().width;
  const emRect = document.getElementById('em').getBoundingClientRect();
  const symRect = document.getElementById('sym').getBoundingClientRect();
  document.title = 'JSON:' + JSON.stringify({
    koOk: document.getElementById('ko').textContent.includes('첫째'),
    sjisOk: sjis === j.expected,
    wL, wi,
    emY: Math.round(emRect.top), emH: Math.round(emRect.height),
    symY: Math.round(symRect.top), symH: Math.round(symRect.height)
  });
})();
</script></body></html>""",
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-07")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["koOk"] and info["sjisOk"], f"EUC-KR or Shift_JIS decoding failed: {info}")
    ensure(res.content_png.has_bleed_color([(255, 0, 255), (0, 255, 255)], tol=18) == 0, ".sr-only magenta/cyan banner bled into rendered PNG")
    em_stats = res.content_png.region_stats(0, max(0, info["emY"] - 4), 220, max(48, info["emH"] + 8))
    ensure(em_stats["warm_pixels"] >= 20 and em_stats["max_chroma"] >= 60, f"Color emoji rendered monochrome instead of RGBA: {em_stats}")
    has_tofu = res.content_png.detect_crossed_box_tofu(0, max(0, info["symY"] - 4), 180, max(48, info["symH"] + 8))
    ensure(not has_tofu and info["wi"] < 0.78 * info["wL"], f"Mathematical symbol Lofi rendered as crossed-box tofu: {info}, has_tofu={has_tofu}")


def _t3_pw_08_websocket_sse_nested_scroll_anim(ctx: RunnerContext) -> None:
    """F09 + F15 + F10: RFC 6455 WebSocket + EventSource SSE -> nested overflow-y:auto scrollTop + Element.animate."""
    srv = ctx.fixture_srv
    ws_before = len(srv.get_ws_traces())
    url = srv.register_route(
        "/t3/pw08/index.html",
        {
            "body": f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title>
<style>
body {{ margin: 0; background: #ffffff; }}
#scroller {{ margin: 20px; width: 300px; height: 160px; overflow-y: auto; background: #f1f5f9; }}
.row {{ height: 28px; background: #cbd5e1; margin-bottom: 4px; }}
#tail {{ height: 48px; background: #16a34a; color: #fff; }}
</style></head><body>
<div id="scroller"></div>
<script>
const sc = document.getElementById('scroller');
for (let i = 0; i < 25; i++) {{
  const d = document.createElement('div');
  d.className = 'row';
  d.textContent = 'Row ' + i;
  sc.appendChild(d);
}}
let wsEcho = '', sseDone = false;
function maybeFinish() {{
  if (!wsEcho || !sseDone) return;
  const tail = document.createElement('div');
  tail.id = 'tail';
  tail.textContent = wsEcho;
  sc.appendChild(tail);
  tail.animate([{{ opacity: 0.4 }}, {{ opacity: 1.0 }}], {{ duration: 60, fill: 'forwards' }});
  sc.scrollTop = sc.scrollHeight;
  requestAnimationFrame(() => {{
    document.title = 'JSON:' + JSON.stringify({{ wsEcho, st: sc.scrollTop, sh: sc.scrollHeight }});
  }});
}}
const ws = new WebSocket({json.dumps(srv.ws_origin + "/ws-chat")});
ws.onopen = () => ws.send('WS_HELLO_2026');
ws.onmessage = (e) => {{ wsEcho = e.data; ws.close(1000, 'ok'); maybeFinish(); }};
const es = new EventSource('/sse-stream');
es.onmessage = (e) => {{ if (e.data === 'done') {{ sseDone = true; es.close(); maybeFinish(); }} }};
</script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-08")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["wsEcho"] == "WS_HELLO_2026" and info["st"] >= 400, f"WebSocket/SSE or nested scrollTop failed: {info}")
    ws_traces = srv.get_ws_traces()[ws_before:]
    ensure(len(ws_traces) >= 1 and ws_traces[-1]["masked_all"] is True, f"RFC 6455 client frame masking violated: {ws_traces}")
    ensure(res.content_png.count_color_near((22, 163, 74), tol=26, rect=(20, 100, 280, 80)) >= 600, "Scrolled-in green #tail row not visible inside scrollport")
    ensure(res.content_png.count_color_near((22, 163, 74), tol=26, rect=(20, 185, 280, 60)) == 0, "Nested overflow-y:auto failed to clip #tail below y=180")


def _t3_pw_09_long_pushstate_container_transition_midpoint(ctx: RunnerContext) -> None:
    """F06 + F12 + F13: 34 KB history.pushState/back() + @container + 60Hz CSS transition midpoint interpolation."""
    srv = ctx.fixture_srv
    long_tok = "T" * 34000
    url = srv.register_route(
        "/t3/pw09/index.html",
        {
            "body": f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title>
<style>
body {{ margin: 0; background: #ffffff; }}
#wrap {{ container-type: inline-size; width: 400px; }}
#tbox {{ width: 100px; height: 60px; background-color: #ff0000; transition: width 600ms linear, background-color 600ms linear; }}
#tbox.go {{ width: 300px; background-color: #0000ff; }}
@container (min-width: 500px) {{ #badge {{ background: #16a34a; }} }}
@container (max-width: 499px) {{ #badge {{ background: #ca8a04; }} }}
#badge {{ width: 80px; height: 24px; }}
</style></head><body>
<div id="wrap"><div id="tbox"></div><div id="badge"></div></div>
<script>
const tok = {json.dumps(long_tok)};
history.pushState({{ step: 1 }}, '', '/t3/pw09/step1?tok=' + tok);
const pushedLen = location.href.length;
document.getElementById('wrap').style.width = '560px';
let popped = false;
window.addEventListener('popstate', () => {{ popped = true; }});
history.back();
const box = document.getElementById('tbox');
void box.offsetWidth;
const t0 = performance.now();
box.classList.add('go');
function step() {{
  const dt = performance.now() - t0;
  if (dt < 250) {{
    requestAnimationFrame(step);
  }} else {{
    const w = Math.round(box.getBoundingClientRect().width);
    document.title = 'JSON:' + JSON.stringify({{ pushedLen, popped, backPath: location.pathname, midW: w }});
  }}
}}
requestAnimationFrame(step);
</script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-09")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["pushedLen"] >= 34000 and info["popped"] is True and info["backPath"] == "/t3/pw09/index.html", f"34KB pushState/back failed: {info}")
    ensure(145 <= info["midW"] <= 260, f"CSS transition jumped or stalled instead of 60Hz midpoint interpolation: {info}")
    r, _g, b, _a = res.content_png.pixel(50, 30)
    ensure(45 <= r <= 215 and 45 <= b <= 215, f"Expected intermediate purple pixel at midpoint, got RGB=({r},{_g},{b})")


def _t3_pw_10_webview_256mb_heap_and_3mb_bundle(ctx: RunnerContext) -> None:
    """F11 + F02 + F03: Modern WebView 256 MB heap scaling + 3 MB JS bundle + 80 MB TypedArray allocation without OOM/turn-abort."""
    srv = ctx.fixture_srv
    js_chunks = ["const tbl = [];"]
    for i in range(26000):
        js_chunks.append(f'tbl.push("sym_{i:05d}_" + "z".repeat(72));')
    js_chunks.append(
        """
const big = new Uint8Array(80 * 1024 * 1024);
big[0] = 7;
big[big.length - 1] = 35;
document.getElementById('ok').style.backgroundColor = '#16a34a';
document.title = 'JSON:' + JSON.stringify({ count: tbl.length, sum: big[0] + big[big.length - 1] });
"""
    )
    bundle_src = "\n".join(js_chunks)
    ensure(len(bundle_src) >= 2_800_000, "Expected >=2.8MB browser bundle")
    srv.register_route("/t3/pw10/app.js", {"content_type": "application/javascript", "body": bundle_src})
    url = srv.register_route(
        "/t3/pw10/index.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title></head>
<body><div id="ok" style="width:160px;height:60px;background:#dc2626"></div>
<script src="/t3/pw10/app.js"></script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-10", wait_load_ms=8000, timeout_sec=20.0)
    ensure(res.ok, f"Guard failures (check heap_cap >= 256MB & turn budget): {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["count"] == 26000 and info["sum"] == 42, f"3MB bundle + 80MB heap execution mismatch: {info}")
    ensure(res.content_png.count_color_near((22, 163, 74), tol=22, rect=(10, 10, 140, 45)) >= 800, "Green completion rect missing")


def _t3_pw_11_honest_video_error_and_negotiated_image_canvas(ctx: RunnerContext) -> None:
    """F11 + F09 + F13: HTML error on <video src> must fire error (zero fake procedural visualizer!) + negotiated image onto <canvas>."""
    srv = ctx.fixture_srv
    srv.register_route("/t3/pw11/not_a_video.mp4", {"status": 200, "content_type": "text/html; charset=utf-8", "body": "<html><body>Sign in gate</body></html>"})
    url = srv.register_route(
        "/t3/pw11/index.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title>
<style>body { margin: 0; background: #ffffff; } #v { display: block; width: 200px; height: 100px; background: #ffffff; }</style></head>
<body>
<video id="v" src="/t3/pw11/not_a_video.mp4" width="200" height="100"></video>
<img id="neg" src="/img/negotiated" style="display:block">
<canvas id="cv" width="64" height="64"></canvas>
<script>
let vidErr = false;
const v = document.getElementById('v');
v.onerror = () => { vidErr = true; };
window.addEventListener('load', () => {
  const img = document.getElementById('neg');
  const ctx = document.getElementById('cv').getContext('2d');
  ctx.drawImage(img, 0, 0);
  const px = Array.from(ctx.getImageData(32, 32, 1, 1).data);
  setTimeout(() => {
    document.title = 'JSON:' + JSON.stringify({
      vidErr: vidErr || (v.error !== null),
      readyState: v.readyState,
      imgW: Math.round(img.getBoundingClientRect().width),
      px
    });
  }, 80);
});
</script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-11")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["vidErr"] is True and info["readyState"] == 0, f"Invalid HTML response on <video> was not rejected honestly: {info}")
    # Video box (0..200, 0..100) must NOT contain colorful procedural sine-wave visualizer bars!
    v_stats = res.content_png.region_stats(5, 5, 190, 90)
    ensure(v_stats["saturated_pixels"] == 0, f"Deceptive procedural video visualizer painted on HTML error response: {v_stats}")
    ensure(info["imgW"] == 64 and info["px"][1] > 160 and info["px"][0] < 80, f"Negotiated image decode / canvas drawImage failed: {info}")
    ensure(res.content_png.count_color_near((34, 197, 94), tol=28, rect=(0, 100, 64, 64)) >= 1200, "Negotiated 64x64 green image not painted")


def _t3_pw_12_svg_symbol_shadow_slot_dynamic_link(ctx: RunnerContext) -> None:
    """F12 + F15 + F08: Hidden <svg width=0><symbol> + <use> inside Shadow DOM + dynamic <link rel=stylesheet> onload."""
    srv = ctx.fixture_srv
    srv.register_route("/t3/pw12/dyn.css", {"content_type": "text/css", "body": "#host-wrap { color: #0284c7; margin-left: 160px; margin-top: 40px; }"})
    url = srv.register_route(
        "/t3/pw12/index.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title>
<style>body { margin: 0; background: #ffffff; }</style></head><body>
<svg width="0" height="0" style="position:absolute"><defs><symbol id="ic-dot" viewBox="0 0 24 24"><rect x="2" y="2" width="20" height="20" fill="currentColor"/></symbol></defs></svg>
<div id="host-wrap"><svg width="48" height="48"><use href="#ic-dot"/></svg></div>
<script>
const hw = document.getElementById('host-wrap');
const sr = hw.attachShadow({ mode: 'open' });
sr.innerHTML = '<div style="padding:4px"><slot></slot></div>';
const lnk = document.createElement('link');
lnk.rel = 'stylesheet';
lnk.href = '/t3/pw12/dyn.css';
lnk.onload = () => {
  requestAnimationFrame(() => {
    document.title = 'JSON:' + JSON.stringify({ loaded: true, sheets: document.styleSheets.length });
  });
};
document.head.appendChild(lnk);
</script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-12")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["loaded"] is True and info["sheets"] >= 2, f"Dynamic <link> onload or document.styleSheets failed: {info}")
    # (0,0..40,40) must remain 100% pure white (zero hidden <svg><symbol> bleed!)
    top_left = res.content_png.region_stats(0, 0, 40, 40)
    ensure(top_left["non_white"] == 0, f"Hidden <svg width=0><symbol> bled into (0,0): {top_left}")
    ensure(res.content_png.count_color_near((2, 132, 199), tol=28, rect=(160, 40, 60, 60)) >= 500, "<use href='#ic-dot'> inside slotted Shadow DOM did not paint #0284c7")


def _t3_pw_13_grid_24_tracks_sump_adopted_sheets(ctx: RunnerContext) -> None:
    """F15 + F05 + F10: 24-column CSS Grid + explicit line placement + Map.getOrInsert + Math.sumPrecise + adoptedStyleSheets."""
    srv = ctx.fixture_srv
    url = srv.register_route(
        "/t3/pw13/index.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title></head>
<body style="margin:0;background:#ffffff">
<div id="grid"></div>
<script>
const s = new CSSStyleSheet();
s.replaceSync('#grid { display: grid; grid-template-columns: repeat(24, 40px); gap: 4px; } .c { height: 32px; background: #e2e8f0; } .hi { background: #16a34a; }');
document.adoptedStyleSheets = [s];
const g = document.getElementById('grid');
const m = new Map();
for (let i = 0; i < 48; i++) {
  const d = document.createElement('div');
  d.className = (i === 20) ? 'c hi' : 'c';
  d.id = 'cell-' + i;
  g.appendChild(d);
  m.getOrInsert('row' + Math.floor(i / 24), 0);
}
window.addEventListener('load', () => {
  const lefts = [];
  for (let i = 0; i < 24; i++) lefts.push(document.getElementById('cell-' + i).getBoundingClientRect().left);
  const c20 = document.getElementById('cell-20').getBoundingClientRect();
  const c24 = document.getElementById('cell-24').getBoundingClientRect();
  document.title = 'JSON:' + JSON.stringify({
    c20Left: Math.round(c20.left),
    c23Left: Math.round(lefts[23]),
    c24Left: Math.round(c24.left),
    c24Top: Math.round(c24.top),
    sumLeft: Math.round(Math.sumPrecise(lefts))
  });
});
</script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-13")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["c20Left"] == 20 * 44 and info["c23Left"] == 23 * 44, f"24-column Grid overflowed 16-track cap: {info}")
    ensure(info["c24Left"] == 0 and info["c24Top"] >= 34, f"Row 2 wrapping failed in 24-col Grid: {info}")
    ensure(res.content_png.count_color_near((22, 163, 74), tol=24, rect=(882, 4, 34, 24)) >= 400, "Cell #20 at x=880 not painted green")


def _t3_pw_14_private_ephemeral_cookies_idb_compression_stream(ctx: RunnerContext) -> None:
    """F07 + F09 + F11: --private multi-hop session cookies + CompressionStream('gzip') + zero cross-run persistence."""
    srv = ctx.fixture_srv
    srv.register_route(
        "/t3/pw14/login",
        {
            "status": 302,
            "headers": [
                ("Set-Cookie", "priv_sess=EPHEMERAL_OK; Path=/; Max-Age=3600"),
                ("Location", "/t3/pw14/app"),
            ],
            "body": b"",
        },
    )
    srv.register_route(
        "/t3/pw14/app",
        lambda req: {
            "body": f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title></head>
<body><div style="width:120px;height:50px;background:#2563eb;color:#fff">PW14</div>
<script>
(async () => {{
  const prevLs = localStorage.getItem('priv_k') || 'EMPTY';
  localStorage.setItem('priv_k', 'LEAKED');
  const raw = new TextEncoder().encode('Arcradient-Stream-2026 '.repeat(20));
  const cs = new CompressionStream('gzip');
  const ds = new DecompressionStream('gzip');
  const compBlob = await new Response(new Blob([raw]).stream().pipeThrough(cs)).blob();
  const roundTxt = await new Response(compBlob.stream().pipeThrough(ds)).text();
  document.title = 'JSON:' + JSON.stringify({{
    wireCookie: {json.dumps(req["headers"].get("cookie", ""))},
    docCookie: document.cookie,
    prevLs,
    compSmaller: compBlob.size < raw.byteLength,
    roundOk: roundTxt.startsWith('Arcradient-Stream-2026 ')
  }});
}})();
</script></body></html>"""
        },
    )
    shared_prof = os.path.join(ctx.test_dir("T3-PW-14"), "shared_prof")
    os.makedirs(shared_prof, exist_ok=True)
    r1 = run_waterscape_verify(ctx, srv.main_origin + "/t3/pw14/login", test_id="T3-PW-14-run1", private=True, env_overrides={"WATERSCAPE_PROFILE_DIR": shared_prof})
    ensure(r1.ok, f"Run 1 guard failures: {r1.guard_failures}")
    i1 = _parse_title_json(r1)
    ensure("priv_sess=EPHEMERAL_OK" in i1["wireCookie"] and "priv_sess=EPHEMERAL_OK" in i1["docCookie"], f"Private 302 session cookie dropped mid-session: {i1}")
    ensure(i1["compSmaller"] is True and i1["roundOk"] is True, f"CompressionStream/DecompressionStream failed: {i1}")

    r2 = run_waterscape_verify(ctx, srv.main_origin + "/t3/pw14/app", test_id="T3-PW-14-run2", private=True, env_overrides={"WATERSCAPE_PROFILE_DIR": shared_prof})
    ensure(r2.ok, f"Run 2 guard failures: {r2.guard_failures}")
    i2 = _parse_title_json(r2)
    ensure("EPHEMERAL_OK" not in i2["wireCookie"] and i2["prevLs"] == "EMPTY", f"--private leaked cookies or localStorage across runs: {i2}")


# ---------------------------------------------------------------------------
# T3-PW-15 .. T3-PW-28
# ---------------------------------------------------------------------------


def _t3_pw_15_nonblocking_slow_subresource_and_long_second_url_cancel(ctx: RunnerContext) -> None:
    """F16 + F06 + F08: Non-blocking first paint during slow 900ms subresources + --second-url 33KB nav-gen cancellation."""
    srv = ctx.fixture_srv
    page1 = srv.register_route(
        "/t3/pw15/page1.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>PAGE1_INIT</title>
<link rel="stylesheet" href="/slow-css?ms=900&bg=%23dc2626"></head>
<body style="margin:0;background:#ffffff">
<div style="width:160px;height:60px;background:#2563eb">FAST_BODY</div>
<script src="/slow-js?ms=900&tag=STALE_PAGE1&set_title=1"></script>
</body></html>"""
        },
    )
    pad = "N" * 33200
    page2 = f"{srv.main_origin}/long-nav?pad={pad}"
    res = run_waterscape_verify(ctx, page1, second_url=page2, test_id="T3-PW-15", wait_load_ms=5000)
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    fp_ms = float(res.dump_json.get("first_paint_ms", 9999.0))
    ensure(fp_ms < 450.0, f"UI thread blocked synchronously on 900ms subresource before first paint: first_paint_ms={fp_ms}")
    title = str(res.dump_json.get("title", ""))
    ensure(title.startswith("LONG_NAV:") and "STALE_PAGE1" not in title, f"Stale Page 1 JS overwrote Page 2 or 33KB --second-url failed: {title!r}")
    parts = title.split(":")
    ensure(int(parts[1]) >= 33200, f"33KB --second-url truncated: {title!r}")


def _t3_pw_16_x11_multitab_sub16ms_switch_and_8kb_omnibar(ctx: RunnerContext) -> None:
    """F16 + F17 + F18: Xvfb single-frame window, <50ms (<16ms cached) Ctrl+Tab switch during slow tab load, and >=8KB omnibar URL."""
    srv = ctx.fixture_srv
    tab1_url = srv.register_route(
        "/t3/pw16/tab1.html",
        {"body": "<!DOCTYPE html><html><body style='margin:0;background:#dc2626'><div style='height:400px'>T1</div></body></html>"},
    )
    tab2_url = srv.register_route(
        "/t3/pw16/tab2.html",
        {
            "body": "<!DOCTYPE html><html><head><link rel='stylesheet' href='/slow-css?ms=700&bg=%2316a34a&paint_body=1'></head>"
            "<body style='margin:0;background:#16a34a'><div style='height:400px'>T2</div></body></html>"
        },
    )
    sdir = os.path.join(ctx.test_dir("T3-PW-16"), "x11")
    with X11Session(ctx.waterscape_bin, ctx.module_dir, sdir, initial_url=tab1_url) as x11:
        win = x11.wait_for_waterscape_window()
        ensure(len(x11.list_viewable_windows()) == 1, "Expected single-frame X11 window")
        ok1, _, _ = x11.wait_for_pixel_near(win, 300, 220, (220, 38, 38), timeout_sec=5.0)
        ensure(ok1, "Tab 1 red background did not render in X11 window")

        # Open Tab 2 via Ctrl+T -> Ctrl+L -> type tab2_url -> Return
        ks_t = x11.x11.XStringToKeysym(b"t")
        ks_l = x11.x11.XStringToKeysym(b"l")
        x11.press_keysym(ks_t, ctrl=True)
        time.sleep(0.12)
        x11.press_keysym(ks_l, ctrl=True)
        time.sleep(0.08)
        x11.type_ascii(tab2_url)
        x11.press_keysym(XK_Return)
        ok2, _, _ = x11.wait_for_pixel_near(win, 300, 220, (22, 163, 74), timeout_sec=5.0)
        ensure(ok2, "Tab 2 green background did not render in X11 window")

        # Measure cached Ctrl+Tab switch latency back to Tab 1 (#dc2626)
        sw_ok, sw_ms, last_px = x11.measure_ctrl_tab_switch_ms(win, (300, 220), (220, 38, 38), max_wait_ms=800.0)
        ensure(sw_ok and sw_ms < 50.0, f"Ctrl+Tab switch exceeded responsiveness budget: ok={sw_ok}, sw_ms={sw_ms:.2f}, px={last_px}")
        rc = x11.close_gracefully(win)
        ensure(rc == 0, f"WM_DELETE_WINDOW clean close failed rc={rc}")


def _t3_pw_17_x11_xim_hangul_input_pointer_fetch(ctx: RunnerContext) -> None:
    """F17 + F09 + F14: X11 XIM Hangul (<Multi_key> '가' + keysym '한') into <input> + InputEvent/PointerEvent + UTF-8 POST."""
    srv = ctx.fixture_srv
    received: list[dict] = []

    def _echo_post(req):
        payload = json.loads(req["body"].decode("utf-8", errors="replace") or "{}")
        received.append(payload)
        return {"status": 200, "content_type": "application/json", "body": b'{"ok":true}'}

    srv.register_route("/t3/pw17/submit", _echo_post)
    page_url = srv.register_route(
        "/t3/pw17/form.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"></head>
<body style="margin:0;background:#ffffff">
<input id="ko" type="text" style="position:absolute;left:20px;top:20px;width:260px;height:40px;font-size:22px">
<button id="send" style="position:absolute;left:20px;top:80px;width:160px;height:48px;background:#2563eb;color:#fff">전송</button>
<div id="done" style="position:absolute;left:20px;top:150px;width:160px;height:50px;background:#e2e8f0"></div>
<script>
let sawInputEv = false, sawPointerEv = false;
const inp = document.getElementById('ko');
inp.addEventListener('input', (e) => { if (typeof InputEvent !== 'undefined' && e instanceof InputEvent) sawInputEv = true; else sawInputEv = true; });
const btn = document.getElementById('send');
btn.addEventListener('pointerdown', (e) => { if (typeof PointerEvent !== 'undefined' && e instanceof PointerEvent) sawPointerEv = true; });
btn.addEventListener('click', async () => {
  await fetch('/t3/pw17/submit', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ val: inp.value, inputEv: sawInputEv, pointerEv: sawPointerEv, hasPE: typeof PointerEvent === 'function' })
  });
  document.getElementById('done').style.backgroundColor = '#16a34a';
});
</script></body></html>"""
        },
    )
    sdir = os.path.join(ctx.test_dir("T3-PW-17"), "x11")
    with X11Session(ctx.waterscape_bin, ctx.module_dir, sdir, initial_url=page_url) as x11:
        win = x11.wait_for_waterscape_window()
        ok_btn, _, _ = x11.wait_for_pixel_near(win, 80, 76 + 100, (37, 99, 235), timeout_sec=5.0)
        ensure(ok_btn, "Form button did not render in X11 viewport")
        # Click <input id="ko"> at viewport (80, 40) -> window (80, 76 + 40)
        x11.click_window(win, 80, 76 + 40)
        time.sleep(0.08)
        x11.compose_multi_key("rk")  # -> "가"
        time.sleep(0.05)
        x11.press_keycode(KC_HANGUL_HAN)  # -> "한"
        time.sleep(0.08)
        # Click <button id="send"> at viewport (80, 104) -> window (80, 76 + 104)
        x11.click_window(win, 80, 76 + 104)
        ok_done, _, _ = x11.wait_for_pixel_near(win, 80, 76 + 175, (22, 163, 74), timeout_sec=4.0)
        ensure(ok_done and len(received) >= 1, f"XIM Hangul submit did not complete; received={received}")
        rec = received[-1]
        ensure(rec.get("val") == "가한", f"Expected XIM Hangul '가한' (not legacy 'rk'), got {rec}")
        ensure(rec.get("hasPE") is True, f"PointerEvent constructor missing: {rec}")
        x11.close_gracefully(win)


def _t3_pw_18_sticky_100vh_wcag_button_hidpi_devtools_container(ctx: RunnerContext) -> None:
    """F18 + F15 + F13: 100vh flex sticky footer + WCAG >=4.5:1 Sign-In button + 192 DPI right DevTools @container reflow."""
    srv = ctx.fixture_srv
    url = srv.register_route(
        "/t3/pw18/index.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title>
<style>
html, body { margin: 0; padding: 0; background: #ffffff; }
body { min-height: 100vh; display: flex; flex-direction: column; container-type: inline-size; }
main { flex: 1; padding: 16px; }
#signin { background: #1a73e8; color: #ffffff; border: 0; border-radius: 20px; padding: 10px 24px; font-size: 16px; font-weight: bold; }
#resp { width: 160px; height: 44px; margin-top: 12px; background: #2563eb; }
@container (max-width: 900px) { #resp { background: #16a34a; } }
footer { height: 40px; background: #1e293b; color: #f8fafc; }
</style></head><body>
<main><button id="signin">로그인 Sign in</button><div id="resp"></div></main>
<footer id="ft">Footer</footer>
<script>
window.addEventListener('load', () => {
  const ft = document.getElementById('ft').getBoundingClientRect();
  const btn = document.getElementById('signin').getBoundingClientRect();
  document.title = 'JSON:' + JSON.stringify({
    ftTop: Math.round(ft.top), ftH: Math.round(ft.height),
    vh: window.innerHeight, vw: window.innerWidth,
    bx: Math.round(btn.left), by: Math.round(btn.top), bw: Math.round(btn.width), bh: Math.round(btn.height)
  });
});
</script></body></html>"""
        },
    )
    # 1. Standard 96 DPI 1280x800: content_viewport = 1280x724 -> footer top == 684, contrast >= 4.5:1
    r96 = run_waterscape_verify(ctx, url, test_id="T3-PW-18-96dpi", window_size="1280x800")
    ensure(r96.ok, f"96dpi guard failures: {r96.guard_failures}")
    i96 = _parse_title_json(r96)
    ensure(abs(i96["ftTop"] - (r96.content_png.height - 40)) <= 2, f"100vh sticky footer not pinned to bottom: {i96}, h={r96.content_png.height}")
    ensure(r96.content_png.count_color_near((30, 41, 59), tol=20, rect=(10, r96.content_png.height - 32, 200, 24)) >= 1000, "Sticky footer #1e293b missing at bottom")
    contrast = r96.content_png.region_max_contrast(i96["bx"], i96["by"], i96["bw"], i96["bh"])
    ensure(contrast >= 4.5, f"Sign-In button WCAG contrast < 4.5:1 (got {contrast:.2f})")

    # 2. 192 DPI + --devtools right narrows CSS DIP viewport width <= 900px, flipping #resp to green #16a34a
    r192 = run_waterscape_verify(ctx, url, test_id="T3-PW-18-192dpi", window_size="1280x800", dpi=192, devtools="right")
    ensure(r192.ok, f"192dpi+devtools guard failures: {r192.guard_failures}")
    sl = r192.dump_json.get("shell_layout", {})
    ensure(int(sl.get("tab_strip_rect", {}).get("height", 0)) >= 64, f"192 DPI did not scale tab_strip_rect: {sl}")
    ensure(int(sl.get("devtools_dock_rect", {}).get("width", 0)) > 0, f"Right DevTools dock rect empty: {sl}")


def _t3_pw_19_g5_inventory_honesty_vs_live_conformance_runtime(ctx: RunnerContext) -> None:
    """F19 + F11 + F13: Cross-validate standards_2026_inventory.json against live --conformance runtime & G5 honesty invariants."""
    inv_path = os.path.join(ctx.repo_root, "docs", "standards_2026_inventory.json")
    ensure(os.path.isfile(inv_path), f"Missing {inv_path}")
    inv = json.loads(open(inv_path, "r", encoding="utf-8").read())
    entries = inv.get("entries") or []
    ensure(len(entries) >= 28, f"Expected expanded clause-level inventory (>=28 entries), got {len(entries)}")

    bad_verified = [
        e["id"]
        for e in entries
        if e.get("status_code") == "verified" and len(e.get("blocking_issues") or []) > 0
    ]
    ensure(len(bad_verified) == 0, f"G5 violation: 'verified' entries carry unresolved blocking_issues: {bad_verified}")

    # Probe runtime globals & CSS.supports in --conformance to verify zero hollow stubs
    srv = ctx.fixture_srv
    url = srv.register_route(
        "/t3/pw19/probe.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title></head>
<body><div style="width:120px;height:40px;background:#2563eb;color:#fff">G5</div>
<script>
(async () => {
  let wasmHonest = true;
  if (typeof WebAssembly === 'object' && WebAssembly !== null) {
    try {
      const modBytes = new Uint8Array([0,97,115,109,1,0,0,0,1,7,1,96,2,127,127,1,127,3,2,1,0,7,7,1,3,97,100,100,0,0,10,9,1,7,0,32,0,32,1,106,11]);
      const { instance } = await WebAssembly.instantiate(modBytes);
      wasmHonest = (instance.exports.add(19, 23) === 42);
    } catch (_) { wasmHonest = false; }
  }
  document.title = 'JSON:' + JSON.stringify({
    sw: ('serviceWorker' in navigator),
    wt: ('WebTransport' in globalThis),
    wasmHonest,
    cssBogus: CSS.supports('nonexistent-2099-prop: 1px'),
    cssGrid: CSS.supports('display: grid'),
    hasUrlPattern: typeof URLPattern === 'function',
    hasAdopted: 'adoptedStyleSheets' in document
  });
})();
</script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-19")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["wasmHonest"] is True, "Hollow WebAssembly stub detected")
    ensure(info["cssBogus"] is False and info["cssGrid"] is True, f"CSS.supports dishonesty: {info}")
    ensure(info["hasUrlPattern"] is True and info["hasAdopted"] is True, f"2026 Web IDL / Constructable Stylesheets missing: {info}")


def _t3_pw_20_env_json_and_elf_cleanroom_lazy_module_audit(ctx: RunnerContext) -> None:
    """F01 + F11 + F19: .agents/environment.json zero-secrets + ELF nm/readelf clean-room symbol audit + scriptless lazy JS load."""
    env_json_path = os.path.join(ctx.repo_root, ".agents", "environment.json")
    env_sh_path = "/home/antigravity/env-setup.sh"
    ensure(os.path.isfile(env_json_path) and os.path.isfile(env_sh_path), "Missing /env-setup artifacts")
    for p in (env_json_path, env_sh_path):
        txt = open(p, "r", encoding="utf-8").read()
        ensure(
            not re.search(r"(ghp_[A-Za-z0-9]+|github_pat_[A-Za-z0-9_]+|AIza[0-9A-Za-z_-]{20,}|PRIVATE KEY|sudo\s+apt)", txt),
            f"Forbidden secret or sudo apt found in {p}",
        )

    # ELF clean-room audit on arcr_js, waterscape, and libarcr_js_portable.so
    forbidden_sym = re.compile(r"(JS_NewRuntime|JS_NewContext|qjs_|quickjs|v8::Isolate|WebView2|webkit_web_view)")
    targets = [ctx.arcr_js_bin, ctx.waterscape_bin, os.path.join(ctx.module_dir, "libarcr_js_portable.so")]
    for bin_p in targets:
        ensure(os.path.exists(bin_p), f"Missing binary/module {bin_p}")
        nm_out = subprocess.run(["nm", "-D", bin_p], capture_output=True, text=True, timeout=10).stdout
        m = forbidden_sym.search(nm_out)
        ensure(m is None, f"Forbidden third-party engine symbol {m.group(0) if m else ''} in {bin_p}")

    # Verify arcr_js does NOT link X11 or Web/DOM libraries
    readelf_js = subprocess.run(["readelf", "-d", ctx.arcr_js_bin], capture_output=True, text=True, timeout=10).stdout
    ensure("libX11" not in readelf_js and "libarcr_net" not in readelf_js, f"Standalone arcr_js leaked browser shared libs: {readelf_js}")

    # Scriptless static HTML must NOT eagerly initialize the JS VM
    srv = ctx.fixture_srv
    static_url = srv.register_route(
        "/t3/pw20/static.html",
        {"body": "<!DOCTYPE html><html><head><title>STATIC_ONLY</title></head><body><div style='width:120px;height:50px;background:#16a34a'>pure</div></body></html>"},
    )
    res = run_waterscape_verify(ctx, static_url, test_id="T3-PW-20")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    ensure(res.dump_json.get("stats", {}).get("js_module_loaded") is False, f"Scriptless page eagerly loaded JS VM: {res.dump_json.get('stats')}")


def _t3_pw_21_cyclic_esm_tla_using_cdata_legacy_events(ctx: RunnerContext) -> None:
    """F03 + F04 + F09: Cyclic ES Modules + importmap + Top-Level await + using Symbol.dispose + CDATASection + initCustomEvent."""
    srv = ctx.fixture_srv
    srv.register_route(
        "/t3/pw21/a.mjs",
        {
            "content_type": "application/javascript",
            "body": "import { bVal } from 'pkg-b'; export let aVal = 10; await Promise.resolve(); aVal += bVal;\n",
        },
    )
    srv.register_route(
        "/t3/pw21/b.mjs",
        {
            "content_type": "application/javascript",
            "body": "import * as A from './a.mjs'; export const bVal = 32; export function readA() { return A.aVal; }\n",
        },
    )
    url = srv.register_route(
        "/t3/pw21/index.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title>
<script type="importmap">{ "imports": { "pkg-b": "/t3/pw21/b.mjs" } }</script></head>
<body><div id="t" style="width:140px;height:50px;background:#2563eb;color:#fff">PW21</div>
<script type="module">
import { aVal } from './a.mjs';
import { readA } from 'pkg-b';
let disposed = false;
{
  using _r = { [Symbol.dispose]() { disposed = true; } };
}
const xml = new DOMParser().parseFromString('<root><![CDATA[raw<tag>&ok]]><?pi target-data?></root>', 'application/xml');
const cdataOk = xml.documentElement.firstChild instanceof CDATASection;
const piOk = xml.documentElement.lastChild instanceof ProcessingInstruction;
let detailGot = null;
const el = document.getElementById('t');
el.addEventListener('legacy-ev', (e) => { detailGot = e.detail; });
const cev = document.createEvent('CustomEvent');
cev.initCustomEvent('legacy-ev', true, true, 'LEGACY_42');
el.dispatchEvent(cev);
document.title = 'JSON:' + JSON.stringify({ aVal, viaB: readA(), disposed, cdataOk, piOk, detailGot });
</script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-21")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["aVal"] == 42 and info["viaB"] == 42, f"Cyclic ESM + TLA failed: {info}")
    ensure(info["disposed"] is True, f"ES2026 using Symbol.dispose failed in browser module: {info}")
    ensure(info["cdataOk"] is True and info["piOk"] is True and info["detailGot"] == "LEGACY_42", f"Web IDL CDATASection/PI/initCustomEvent failed: {info}")


def _t3_pw_22_bulk_cookies_33kb_redirect_and_nested_intersection_observer(ctx: RunnerContext) -> None:
    """F07 + F06 + F15: 260 Set-Cookie headers across 33KB redirect + IntersectionObserver inside scrolled overflow:auto container."""
    srv = ctx.fixture_srv
    q_pad = "C" * 33000
    cookie_hdrs = [("Set-Cookie", f"k{i}=v{i}; Path=/") for i in range(260)]
    cookie_hdrs.append(("Location", f"/t3/pw22/dest?pad={q_pad}"))
    srv.register_route("/t3/pw22/start", {"status": 302, "headers": cookie_hdrs, "body": b""})
    srv.register_route(
        "/t3/pw22/dest",
        lambda req: {
            "body": f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title>
<style>
#sc {{ width: 240px; height: 120px; overflow-y: auto; background: #f8fafc; }}
.sp {{ height: 500px; }}
#lazy {{ height: 60px; background: #dc2626; }}
</style></head><body>
<div id="sc"><div class="sp"></div><div id="lazy">LAZY</div></div>
<script>
const wireCount = {len([p for p in req["headers"].get("cookie", "").split(";") if p.strip()])};
const sc = document.getElementById('sc');
const lazy = document.getElementById('lazy');
let hit = false;
const io = new IntersectionObserver((entries) => {{
  for (const e of entries) {{
    if (e.isIntersecting) {{
      hit = true;
      lazy.style.backgroundColor = '#16a34a';
    }}
  }}
}}, {{ root: sc, threshold: 0.1 }});
io.observe(lazy);
sc.scrollTop = 480;
setTimeout(() => {{
  const docCount = document.cookie.split(';').filter(Boolean).length;
  document.title = 'JSON:' + JSON.stringify({{ wireCount, docCount, hrefLen: location.href.length, hit }});
}}, 90);
</script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, srv.main_origin + "/t3/pw22/start", test_id="T3-PW-22")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["wireCount"] >= 260 and info["docCount"] >= 260, f"Cookie jar capped below 260 cookies: {info}")
    ensure(info["hrefLen"] >= 33000 and info["hit"] is True, f"33KB redirect or nested IntersectionObserver failed: {info}")


def _t3_pw_23_closed_shadow_retargeting_adopted_woff2_aa(ctx: RunnerContext) -> None:
    """F12 + F14 + F10: Closed ShadowRoot event retargeting + adoptedStyleSheets @font-face WOFF2 + 256-level grayscale AA."""
    srv = ctx.fixture_srv
    url = srv.register_route(
        "/t3/pw23/index.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title>
<style>
@font-face { font-family: 'AaFont'; src: url('/fonts/mono.woff2') format('woff2'); }
body { margin: 0; background: #ffffff; }
</style></head><body>
<div id="host"><span>SLOTTED_AA_한글_Text_2026</span></div>
<script>
const host = document.getElementById('host');
const sr = host.attachShadow({ mode: 'closed' });
const sh = new CSSStyleSheet();
sh.replaceSync(':host { display: block; padding: 12px; font-family: "AaFont", sans-serif; font-size: 28px; color: #000000; background: #ffffff; }');
sr.adoptedStyleSheets = [sh];
sr.innerHTML = '<button id="inbtn"><slot></slot></button>';
let composedTarget = null, nonComposedLeaked = false;
document.body.addEventListener('ev-comp', (e) => { composedTarget = (e.target === host); });
document.body.addEventListener('ev-local', () => { nonComposedLeaked = true; });
const btn = sr.getElementById('inbtn');
btn.dispatchEvent(new CustomEvent('ev-comp', { bubbles: true, composed: true }));
btn.dispatchEvent(new CustomEvent('ev-local', { bubbles: true, composed: false }));
window.addEventListener('load', () => {
  const r = host.getBoundingClientRect();
  document.title = 'JSON:' + JSON.stringify({
    closedNull: host.shadowRoot === null,
    composedTarget,
    nonComposedLeaked,
    h: Math.round(r.height)
  });
});
</script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-23")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["closedNull"] is True and info["composedTarget"] is True and info["nonComposedLeaked"] is False, f"Closed ShadowRoot encapsulation/retargeting failed: {info}")
    aa_levels = res.content_png.count_grayscale_aa_levels(0, 0, 500, max(60, info["h"] + 16))
    ensure(aa_levels >= 8, f"Expected >=8 grayscale anti-aliasing luminance levels on glyph stems, got {aa_levels}")


def _t3_pw_24_classic_cpu_minimum_fixed_scroll_hittest(ctx: RunnerContext) -> None:
    """F13 + F15 + F17: Win2000 classic frame + --force-cpu --profile minimum + calc/clamp + fixed-header click at --scroll-y 600."""
    srv = ctx.fixture_srv
    url = srv.register_route(
        "/t3/pw24/index.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>BEFORE_CLICK</title>
<style>
body { margin: 0; height: 2200px; background: #ffffff; }
#hdr { position: fixed; top: 0; left: 0; width: 100%; height: 56px; background: #1e293b; }
#act { position: absolute; left: 80px; top: 10px; width: clamp(120px, calc(10% + 40px), 240px); height: 36px; background: #16a34a; color: #fff; }
</style></head><body>
<div id="hdr"><button id="act" onclick="document.title='FIXED_HIT_OK'">Hit</button></div>
<div style="margin-top:120px;height:1800px;background:#f1f5f9">Tall content</div>
</body></html>"""
        },
    )
    res = run_waterscape_verify(
        ctx,
        url,
        test_id="T3-PW-24",
        profile="minimum",
        theme="classic",
        force_classic_frame=True,
        force_cpu=True,
        scroll_y=600,
        click=(110, 24),
        save_png=True,
    )
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    cap_h = int(res.dump_json.get("shell_layout", {}).get("window_caption_rect", {}).get("height", 0))
    ensure(cap_h > 0, f"--force-classic-frame did not render simulated Win2000 caption bar: {res.dump_json.get('shell_layout')}")
    ensure(res.dump_json.get("title") == "FIXED_HIT_OK", f"Fixed > absolute click missed at scroll_y=600: title={res.dump_json.get('title')!r}")


def _t3_pw_25_cjk_charsets_durationformat_canvas_roundtrip(ctx: RunnerContext) -> None:
    """F05 + F09 + F14: GB18030 + Big5 + EUC-JP TextDecoder + Intl.DurationFormat('ko') + Canvas 2D Hangul fillText & toDataURL."""
    srv = ctx.fixture_srv
    url = srv.register_route(
        "/t3/pw25/index.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title></head>
<body style="margin:0;background:#ffffff">
<canvas id="c1" width="180" height="70"></canvas>
<script>
(async () => {
  const encs = ['gb18030', 'big5', 'euc-jp'];
  const checks = {};
  for (const e of encs) {
    const r = await fetch('/charset-bytes/' + e);
    const j = await r.json();
    const got = new TextDecoder(e).decode(new Uint8Array(j.bytes));
    checks[e] = (got === j.expected);
  }
  const dur = new Intl.DurationFormat('ko-KR', { style: 'long' }).format({ hours: 1, minutes: 30 });
  const c1 = document.getElementById('c1');
  const g = c1.getContext('2d');
  g.fillStyle = '#1d4ed8';
  g.fillRect(0, 0, 180, 70);
  g.fillStyle = '#ffffff';
  g.font = '22px sans-serif';
  g.fillText('한글 2026', 12, 42);
  const dataUrl = c1.toDataURL('image/png');
  document.title = 'JSON:' + JSON.stringify({ checks, durOk: dur.includes('1') && dur.includes('30'), pngOk: dataUrl.startsWith('data:image/png;base64,') });
})();
</script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-25")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(all(info["checks"].values()), f"Multi-CJK TextDecoder failed on GB18030/Big5/EUC-JP: {info}")
    ensure(info["durOk"] is True and info["pngOk"] is True, f"Intl.DurationFormat or Canvas toDataURL failed: {info}")
    ensure(res.content_png.count_color_near((29, 78, 216), tol=24, rect=(4, 4, 170, 60)) >= 2000, "Canvas 2D blue rect not painted")


def _t3_pw_26_nomodule_slot_assigned_range_selection_shadow_style(ctx: RunnerContext) -> None:
    """F08 + F09 + F12: <script nomodule> skipped vs module executed + HTMLSlotElement.assignedElements + Range/Selection + dynamic Shadow <style>."""
    srv = ctx.fixture_srv
    url = srv.register_route(
        "/t3/pw26/index.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title></head>
<body style="margin:0;background:#ffffff">
<div id="host"><em id="item1">Alpha</em><strong id="item2">Beta</strong></div>
<p id="sel-target">SelectThisRange2026</p>
<script>window.__nomoduleRan = false;</script>
<script nomodule>window.__nomoduleRan = true;</script>
<script type="module">
const host = document.getElementById('host');
const sr = host.attachShadow({ mode: 'open' });
sr.innerHTML = '<slot id="sl"></slot>';
const st = document.createElement('style');
st.textContent = ':host { display: block; background: #15803d; padding: 10px; color: #ffffff; }';
sr.appendChild(st);
const sl = sr.getElementById('sl');
const assigned = sl.assignedElements().map(e => e.id);
const rng = document.createRange();
rng.selectNodeContents(document.getElementById('sel-target'));
const sel = window.getSelection();
sel.removeAllRanges();
sel.addRange(rng);
document.title = 'JSON:' + JSON.stringify({
  nomoduleRan: window.__nomoduleRan,
  isSlotProto: (typeof HTMLSlotElement === 'function' && sl instanceof HTMLSlotElement),
  assigned,
  selTxt: sel.toString()
});
</script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-26")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["nomoduleRan"] is False, "<script nomodule> executed in modern ES module browser!")
    ensure(info["isSlotProto"] is True and info["assigned"] == ["item1", "item2"], f"HTMLSlotElement.assignedElements failed: {info}")
    ensure(info["selTxt"] == "SelectThisRange2026", f"Range/Selection failed: {info}")
    ensure(res.content_png.count_color_near((21, 128, 61), tol=24, rect=(4, 4, 150, 28)) >= 600, "Dynamic <style> inside ShadowRoot not painted")


def _t3_pw_27_x11_8tabs_devtools_fullscreen_roundtrip(ctx: RunnerContext) -> None:
    """F16 + F18 + F15: Live Xvfb 8-tab cache retention + F12 docked DevTools + F11 EWMH fullscreen round-trip."""
    srv = ctx.fixture_srv
    colors = [
        ("#dc2626", (220, 38, 38)),
        ("#16a34a", (22, 163, 74)),
        ("#2563eb", (37, 99, 235)),
        ("#ca8a04", (202, 138, 4)),
        ("#9333ea", (147, 51, 234)),
        ("#0891b2", (8, 145, 178)),
        ("#ea580c", (234, 88, 12)),
        ("#be123c", (190, 18, 60)),
    ]
    urls = []
    for idx, (hex_c, _rgb) in enumerate(colors):
        u = srv.register_route(f"/t3/pw27/t{idx}.html", {"body": f"<!DOCTYPE html><html><body style='margin:0;background:{hex_c}'>T{idx}</body></html>"})
        urls.append(u)

    sdir = os.path.join(ctx.test_dir("T3-PW-27"), "x11")
    with X11Session(ctx.waterscape_bin, ctx.module_dir, sdir, initial_url=urls[0]) as x11:
        win = x11.wait_for_waterscape_window()
        ok0, _, _ = x11.wait_for_pixel_near(win, 400, 240, colors[0][1], timeout_sec=5.0)
        ensure(ok0, "Tab 0 failed to render")
        ks_t = x11.x11.XStringToKeysym(b"t")
        ks_l = x11.x11.XStringToKeysym(b"l")
        for idx in range(1, 8):
            x11.press_keysym(ks_t, ctrl=True)
            time.sleep(0.08)
            x11.press_keysym(ks_l, ctrl=True)
            time.sleep(0.05)
            x11.type_ascii(urls[idx])
            x11.press_keysym(XK_Return)
            ok_i, _, _ = x11.wait_for_pixel_near(win, 400, 240, colors[idx][1], timeout_sec=4.0)
            ensure(ok_i, f"Tab {idx} failed to render")

        # Ctrl+Tab wraps from Tab 7 -> Tab 0 cleanly in < 50 ms
        sw_ok, sw_ms, _ = x11.measure_ctrl_tab_switch_ms(win, (400, 240), colors[0][1], max_wait_ms=600.0)
        ensure(sw_ok and sw_ms < 50.0, f"8-tab cached_fb switch to Tab 0 took {sw_ms:.2f}ms (ok={sw_ok})")

        # F12 docks DevTools inside the single window (viewable_windows == 1)
        x11.press_keysym(XK_F12)
        time.sleep(0.18)
        ensure(len(x11.list_viewable_windows()) == 1, "F12 spawned extra X11 popup window instead of in-window dock")
        x11.press_keysym(XK_F12)
        time.sleep(0.12)

        # F11 toggles fullscreen and Escape/F11 exits cleanly
        x11.press_keysym(XK_F11)
        time.sleep(0.18)
        fs_ok, _, _ = x11.wait_for_pixel_near(win, 400, 12, colors[0][1], timeout_sec=2.0)
        ensure(fs_ok, "F11 fullscreen did not expand content to top y=12")
        x11.press_keysym(XK_F11)
        time.sleep(0.15)
        ensure(x11.close_gracefully(win) == 0, "Clean close failed after 8-tab + F12 + F11 session")


def _t3_pw_28_deep_lexical_scopes_o1_compiler_fetch_using(ctx: RunnerContext) -> None:
    """F02 + F04 + F11: O(1) comp_scope_find_own with 8,000 lexical bindings + async fetch pipeline + using resource cleanup."""
    srv = ctx.fixture_srv
    srv.register_route("/t3/pw28/data.json", {"content_type": "application/json", "body": b'{"mult":3,"offset":12}'})
    decls = "\n".join(f"  const sym_bind_{i} = {i % 11};" for i in range(8000))
    refs = "+".join(f"sym_bind_{i}" for i in range(0, 8000, 80))
    expected_ref_sum = sum((i % 11) for i in range(0, 8000, 80))
    srv.register_route(
        "/t3/pw28/scope.js",
        {
            "content_type": "application/javascript",
            "body": f"""
async function computeAll() {{
{decls}
  const trace = [];
  {{
    using g = {{ [Symbol.dispose]() {{ trace.push('CLEANED'); }} }};
    const r = await fetch('/t3/pw28/data.json');
    const j = await r.json();
    const base = ({refs});
    trace.push('VAL:' + (base * j.mult + j.offset));
  }}
  document.getElementById('box').style.backgroundColor = '#16a34a';
  document.title = 'JSON:' + JSON.stringify({{ trace }});
}}
computeAll();
""",
        },
    )
    url = srv.register_route(
        "/t3/pw28/index.html",
        {
            "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>WAIT</title></head>
<body><div id="box" style="width:140px;height:50px;background:#dc2626"></div>
<script src="/t3/pw28/scope.js"></script></body></html>"""
        },
    )
    res = run_waterscape_verify(ctx, url, test_id="T3-PW-28")
    ensure(res.ok, f"Guard failures: {res.guard_failures}")
    info = _parse_title_json(res)
    ensure(info["trace"] == [f"VAL:{expected_ref_sum * 3 + 12}", "CLEANED"], f"8K scope + fetch + using trace mismatch: {info}")


TESTS: list[TestCase] = [
    TestCase("T3-PW-01", 3, "F01", "Wake-safe /env-setup bashrc + >32K JS local slots + ES2026 DisposableStack LIFO", _t3_pw_01_env_setup_wake_and_js_slots_erm, ("F02", "F04")),
    TestCase("T3-PW-02", 3, "F02", "2.4MB JS mega-bundle O(1) constant pool & GC + Math.sumPrecise + Map.getOrInsertComputed + DurationFormat", _t3_pw_02_megabundle_gc_sump_duration, ("F03", "F05")),
    TestCase("T3-PW-03", 3, "F04", "Dynamic <script> currentScript + AsyncDisposableStack SuppressedError + WeakMap.getOrInsertComputed", _t3_pw_03_dynamic_script_currentscript_async_using, ("F05", "F08")),
    TestCase("T3-PW-04", 3, "F06", "36KB URL navigation + HttpOnly/JS unified cookie jar + URLPattern matching", _t3_pw_04_long_url_cookie_jar_urlpattern, ("F07", "F09")),
    TestCase("T3-PW-05", 3, "F10", "Custom Element Shadow <slot> + :host/::slotted + Constructable CSSStyleSheet + Element.animate", _t3_pw_05_shadow_slot_adopted_sheets_web_anim, ("F08", "F12")),
    TestCase("T3-PW-06", 3, "F14", "Brotli HTTP CSS + @import + @property + clamp() + genuine @font-face WOFF2 monospace metrics", _t3_pw_06_brotli_css_import_property_woff2, ("F09", "F13")),
    TestCase("T3-PW-07", 3, "F14", "EUC-KR HTML + Shift_JIS TextDecoder + RGBA Color Emoji + zero-tofu math symbols + .sr-only clip", _t3_pw_07_legacy_charsets_emoji_tofu_sronly, ("F09", "F15")),
    TestCase("T3-PW-08", 3, "F09", "RFC 6455 WebSocket + EventSource SSE -> nested overflow:auto scrollTop + Element.animate", _t3_pw_08_websocket_sse_nested_scroll_anim, ("F10", "F15")),
    TestCase("T3-PW-09", 3, "F13", "34KB history.pushState/back() + @container + 60Hz CSS transition midpoint interpolation", _t3_pw_09_long_pushstate_container_transition_midpoint, ("F06", "F12")),
    TestCase("T3-PW-10", 3, "F11", "WebView 256MB heap scaling + 3MB JS bundle + 80MB TypedArray without OOM or turn interrupt", _t3_pw_10_webview_256mb_heap_and_3mb_bundle, ("F02", "F03")),
    TestCase("T3-PW-11", 3, "F11", "Honest <video> HTML-error rejection (zero procedural visualizer) + negotiated image Canvas drawImage", _t3_pw_11_honest_video_error_and_negotiated_image_canvas, ("F09", "F13")),
    TestCase("T3-PW-12", 3, "F12", "Hidden <svg width=0><symbol> zero-(0,0)-bleed + Shadow <slot> <use> + dynamic <link> onload", _t3_pw_12_svg_symbol_shadow_slot_dynamic_link, ("F08", "F15")),
    TestCase("T3-PW-13", 3, "F15", "24-column CSS Grid + Map.getOrInsert + Math.sumPrecise + adoptedStyleSheets", _t3_pw_13_grid_24_tracks_sump_adopted_sheets, ("F05", "F10")),
    TestCase("T3-PW-14", 3, "F07", "--private multi-hop 302 session cookies + CompressionStream('gzip') + cross-run zero-leak", _t3_pw_14_private_ephemeral_cookies_idb_compression_stream, ("F09", "F11")),
    TestCase("T3-PW-15", 3, "F16", "Off-UI-thread non-blocking first paint during 900ms subresource + 33KB --second-url cancellation", _t3_pw_15_nonblocking_slow_subresource_and_long_second_url_cancel, ("F06", "F08")),
    TestCase("T3-PW-16", 3, "F16", "Live Xvfb single-frame window + <50ms Ctrl+Tab switch during slow background load", _t3_pw_16_x11_multitab_sub16ms_switch_and_8kb_omnibar, ("F17", "F18")),
    TestCase("T3-PW-17", 3, "F17", "Live Xvfb Hangul XIM composition ('가한') into <input> + InputEvent/PointerEvent + UTF-8 POST", _t3_pw_17_x11_xim_hangul_input_pointer_fetch, ("F09", "F14")),
    TestCase("T3-PW-18", 3, "F18", "100vh flex sticky footer + WCAG >=4.5:1 Sign-In button + 192 DPI DevTools @container reflow", _t3_pw_18_sticky_100vh_wcag_button_hidpi_devtools_container, ("F13", "F15")),
    TestCase("T3-PW-19", 3, "F19", "G5 standards_2026_inventory.json honesty cross-validation against live --conformance runtime", _t3_pw_19_g5_inventory_honesty_vs_live_conformance_runtime, ("F11", "F13")),
    TestCase("T3-PW-20", 3, "F01", ".agents/environment.json zero-secrets + ELF nm/readelf clean-room audit + scriptless lazy JS", _t3_pw_20_env_json_and_elf_cleanroom_lazy_module_audit, ("F11", "F19")),
    TestCase("T3-PW-21", 3, "F03", "Cyclic ES Modules + importmap + Top-Level await + using dispose + CDATASection + initCustomEvent", _t3_pw_21_cyclic_esm_tla_using_cdata_legacy_events, ("F04", "F09")),
    TestCase("T3-PW-22", 3, "F07", "260 Set-Cookie headers across 33KB redirect + nested scrollport IntersectionObserver", _t3_pw_22_bulk_cookies_33kb_redirect_and_nested_intersection_observer, ("F06", "F15")),
    TestCase("T3-PW-23", 3, "F12", "Closed ShadowRoot event retargeting + adoptedStyleSheets @font-face WOFF2 + 256-level AA", _t3_pw_23_closed_shadow_retargeting_adopted_woff2_aa, ("F10", "F14")),
    TestCase("T3-PW-24", 3, "F17", "Win2000 classic frame + --force-cpu --profile minimum + fixed>absolute hit-test at scroll_y=600", _t3_pw_24_classic_cpu_minimum_fixed_scroll_hittest, ("F13", "F15")),
    TestCase("T3-PW-25", 3, "F05", "GB18030/Big5/EUC-JP TextDecoder + Intl.DurationFormat('ko-KR') + Canvas 2D Hangul round-trip", _t3_pw_25_cjk_charsets_durationformat_canvas_roundtrip, ("F09", "F14")),
    TestCase("T3-PW-26", 3, "F08", "<script nomodule> skip + HTMLSlotElement.assignedElements + Range/Selection + Shadow <style>", _t3_pw_26_nomodule_slot_assigned_range_selection_shadow_style, ("F09", "F12")),
    TestCase("T3-PW-27", 3, "F18", "Live Xvfb 8-tab cached_fb <50ms wrap switch + F12 in-window DevTools + F11 fullscreen round-trip", _t3_pw_27_x11_8tabs_devtools_fullscreen_roundtrip, ("F15", "F16")),
    TestCase("T3-PW-28", 3, "F02", "8,000-binding lexical scope O(1) lookup + async fetch pipeline + using resource disposal", _t3_pw_28_deep_lexical_scopes_o1_compiler_fetch_using, ("F04", "F11")),
]


def get_tests() -> list[TestCase]:
    return list(TESTS)


get_tier3_tests = get_tests
