#!/usr/bin/env python3
"""
Arcradient / Waterscape 2026 Deterministic Local Fixture Server (Zero External Dependencies)
Serves HTTP (main origin), HTTP (cross-origin CORS), and HTTPS (ephemeral test CA + leaf cert)
covering verification matrix IDs: N01-N03, H01, S01-S05, C01-C02, A01-A02, R01.
"""

import argparse
import gzip
import http.server
import json
import os
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

# Pre-compressed Brotli stream for b"brotli-decoded-ok-2026"
# Generated via RFC 7932 uncompressed/literal metablock:
# ISLAST=1 (1), ISLASTI=0 (0), MNIBBLES=00 (4 nibbles -> 22 bytes -> MLEN-1 = 21 = 0x0015),
# ISUNCOMPRESSED=1 -> bit packing for uncompressed 22-byte Brotli stream:
# Let's construct a valid RFC 7932 uncompressed single-metablock stream dynamically!
def encode_brotli_uncompressed(raw: bytes) -> bytes:
    if not raw:
        return b"\x06"  # ISLAST=1 (1), ISLASTI=1 (1) -> 0b00000011? Wait: bit0=1(ISLAST), bit1=1(ISLASTI) => 0x03!
    n = len(raw)
    # BitWriter (LSB first)
    bits = 0
    nbits = 0
    out = bytearray()

    def put_bits(val: int, count: int):
        nonlocal bits, nbits
        bits |= (val & ((1 << count) - 1)) << nbits
        nbits += count
        while nbits >= 8:
            out.append(bits & 0xFF)
            bits >>= 8
            nbits -= 8

    # WBITS: 16 (bit0 = 0 -> WBITS=16)
    put_bits(0, 1)
    # Meta-block header:
    # ISLAST = 0 (so ISUNCOMPRESSED bit is present! RFC 7932 §9.2: ISUNCOMPRESSED is ONLY present if ISLAST == 0!)
    put_bits(0, 1)
    # MNIBBLES: 4 nibbles (00 in 2 bits -> 4 nibbles = 16 bits, up to 65536 bytes)
    put_bits(0, 2)
    mlen_minus_1 = n - 1
    put_bits(mlen_minus_1, 16)
    # ISUNCOMPRESSED = 1
    put_bits(1, 1)
    # Align to byte boundary with zero bits
    if nbits > 0:
        put_bits(0, 8 - nbits)
    # Raw uncompressed bytes
    out.extend(raw)
    # Final empty meta-block: ISLAST = 1, ISLASTI = 1
    put_bits(1, 1)
    put_bits(1, 1)
    if nbits > 0:
        put_bits(0, 8 - nbits)
    return bytes(out)


# Tiny valid 4x4 solid-teal PNG
TINY_PNG = (
    b"\x89PNG\r\n\x1a\n"
    b"\x00\x00\x00\rIHDR\x00\x00\x00\x04\x00\x00\x00\x04\x08\x02\x00\x00\x00&\x93\t)"
    b"\x00\x00\x00\x16IDATx\x9cc`\xa0\x1f\xc4\x8fA\x01\x861\x99\t\x05\x00.\x90\x04\x01\xc2\x91\x8d\xdf"
    b"\x00\x00\x00\x00IEND\xaeB`\x82"
)


class ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def build_routes(main_port: int, cors_port: int, https_port: int):
    main_origin = f"http://127.0.0.1:{main_port}"
    cors_origin = f"http://127.0.0.1:{cors_port}"
    https_origin = f"https://127.0.0.1:{https_port}"

    a01_items = [
        {"id": 1, "title": "Arcradient Pure C11 Engine"},
        {"id": 2, "title": "Waterscape Single-Frame UI"},
    ]

    return {
        "main_origin": main_origin,
        "cors_origin": cors_origin,
        "https_origin": https_origin,
        "a01_items": a01_items,
    }


def make_handler(ctx: dict, is_cors_server: bool = False):
    class FixtureHandler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            if ctx.get("verbose"):
                sys.stderr.write(f"[fixture:{self.server.server_port}] " + (fmt % args) + "\n")

        def send_bytes(self, status: int, content_type: str, body: bytes, extra_headers=None):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            if extra_headers:
                for k, v in extra_headers.items():
                    self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def send_text(self, status: int, content_type: str, text: str, extra_headers=None):
            self.send_bytes(status, content_type, text.encode("utf-8"), extra_headers)

        def do_OPTIONS(self):
            origin = self.headers.get("Origin", "*")
            path = urllib.parse.urlsplit(self.path).path
            if path.startswith("/cors-ok") or path.startswith("/cors-cred"):
                hdrs = {
                    "Access-Control-Allow-Origin": origin if "cred" in path else "*",
                    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
                    "Access-Control-Allow-Headers": "Content-Type, X-Custom-Req",
                    "Access-Control-Expose-Headers": "X-Custom-Resp",
                }
                if "cred" in path:
                    hdrs["Access-Control-Allow-Credentials"] = "true"
                self.send_bytes(204, "text/plain", b"", hdrs)
            else:
                self.send_bytes(204, "text/plain", b"")

        def do_HEAD(self):
            self.do_GET()

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            raw_body = self.rfile.read(length) if length > 0 else b""
            parsed = urllib.parse.urlsplit(self.path)
            path = parsed.path

            if path == "/api/items":
                try:
                    payload = json.loads(raw_body.decode("utf-8") or "{}")
                except Exception:
                    payload = {"title": raw_body.decode("utf-8", errors="replace")}
                new_item = {
                    "id": len(ctx["a01_items"]) + 1,
                    "title": str(payload.get("title", "Untitled")),
                }
                ctx["a01_items"].append(new_item)
                self.send_text(
                    201,
                    "application/json; charset=utf-8",
                    json.dumps({"ok": True, "item": new_item, "items": ctx["a01_items"]}),
                )
                return

            if path == "/s04/echo":
                self.send_text(
                    200,
                    "application/json; charset=utf-8",
                    json.dumps({
                        "method": "POST",
                        "body": raw_body.decode("utf-8", errors="replace"),
                        "ctype": self.headers.get("Content-Type", ""),
                    }),
                    {"X-Custom-Resp": "echo-ok-2026"},
                )
                return

            self.send_text(404, "text/plain; charset=utf-8", "POST route not found")

        def do_GET(self):
            parsed = urllib.parse.urlsplit(self.path)
            path = parsed.path
            qs = urllib.parse.parse_qs(parsed.query)
            delay_ms = int(qs.get("delay_ms", ["0"])[0])
            if delay_ms > 0:
                time.sleep(min(delay_ms, 5000) / 1000.0)

            # ---------------- CORS secondary origin routes ----------------
            if is_cors_server:
                origin = self.headers.get("Origin", "")
                if path == "/cors-ok":
                    self.send_text(
                        200,
                        "application/json; charset=utf-8",
                        json.dumps({"cors": "allowed", "origin": origin}),
                        {
                            "Access-Control-Allow-Origin": "*",
                            "Access-Control-Expose-Headers": "X-Custom-Resp",
                            "X-Custom-Resp": "cors-header-visible",
                        },
                    )
                    return
                if path == "/cors-cred":
                    self.send_text(
                        200,
                        "application/json; charset=utf-8",
                        json.dumps({"cors": "cred-ok", "cookie": self.headers.get("Cookie", "")}),
                        {
                            "Access-Control-Allow-Origin": origin or ctx["main_origin"],
                            "Access-Control-Allow-Credentials": "true",
                        },
                    )
                    return
                if path == "/cors-deny":
                    # Intentionally omit Access-Control-Allow-Origin!
                    self.send_text(
                        200,
                        "application/json; charset=utf-8",
                        json.dumps({"secret": "should-be-blocked-by-cors"}),
                    )
                    return
                if path == "/mod-cors.js":
                    self.send_text(
                        200,
                        "text/javascript; charset=utf-8",
                        'export const crossVal = "cross-origin-esm-ok";\n',
                        {"Access-Control-Allow-Origin": "*"},
                    )
                    return
                if path == "/mod-cors-deny.js":
                    self.send_text(
                        200,
                        "text/javascript; charset=utf-8",
                        'export const denied = "blocked";\n',
                    )
                    return

            # ---------------- Healthcheck ----------------
            if path == "/healthz":
                self.send_text(200, "application/json", json.dumps({
                    "ok": True,
                    "main_origin": ctx["main_origin"],
                    "cors_origin": ctx["cors_origin"],
                    "https_origin": ctx["https_origin"],
                }))
                return

            # ---------------- N01: Redirect + <base> + Korean + CSS + Image ----------------
            if path == "/n01/start":
                self.send_response(302)
                self.send_header("Location", "sub/page.html")
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return
            if path == "/n01/sub/page.html":
                html = """<!doctype html>
<html lang="ko">
<head>
  <meta charset="utf-8">
  <base href="/n01/assets/">
  <title>N01 한글 리디렉션 검증</title>
  <link rel="stylesheet" href="style.css">
</head>
<body>
  <h1 id="heading">아크래디언트 표준 문서 로더 검증</h1>
  <p id="desc">상대 302 리디렉션과 base URL 해석이 정확합니다.</p>
  <img id="logo" src="dot.png" width="32" height="32" alt="로고">
</body>
</html>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return
            if path == "/n01/assets/style.css":
                self.send_text(200, "text/css; charset=utf-8", "body { background: #f8fafc; color: #0f172a; } h1 { color: #2563eb; }")
                return
            if path == "/n01/assets/dot.png":
                self.send_bytes(200, "image/png", TINY_PNG)
                return

            # ---------------- N02: Gzip, Brotli, Chunked, Charset, 304, 404, Bad Length ----------------
            if path == "/n02/gzip":
                html = b"<!doctype html><meta charset='utf-8'><title>N02-GZIP-OK</title><p>gzip-decoded-ok-2026</p>"
                gz = gzip.compress(html)
                self.send_bytes(200, "text/html; charset=utf-8", gz, {"Content-Encoding": "gzip"})
                return
            if path == "/n02/brotli":
                html = b"<!doctype html><meta charset='utf-8'><title>N02-BROTLI-OK</title><p>brotli-decoded-ok-2026</p>"
                br = encode_brotli_uncompressed(html)
                self.send_bytes(200, "text/html; charset=utf-8", br, {"Content-Encoding": "br"})
                return
            if path == "/n02/chunked":
                chunks = [
                    b"<!doctype html><meta charset='utf-8'>",
                    b"<title>N02-CHUNKED-OK</title>",
                    b"<p id='msg'>chunked-transfer-ok-2026</p>",
                ]
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Transfer-Encoding", "chunked")
                self.send_header("Connection", "close")
                self.end_headers()
                for ch in chunks:
                    self.wfile.write(f"{len(ch):X}\r\n".encode("ascii") + ch + b"\r\n")
                self.wfile.write(b"0\r\n\r\n")
                return
            if path == "/n02/etag":
                inm = self.headers.get("If-None-Match", "")
                if inm == '"n02-v1"':
                    self.send_response(304)
                    self.send_header("ETag", '"n02-v1"')
                    self.send_header("Connection", "close")
                    self.end_headers()
                    return
                self.send_text(
                    200,
                    "text/html; charset=utf-8",
                    "<!doctype html><title>N02-ETAG-OK</title><p>cached-v1</p>",
                    {"ETag": '"n02-v1"', "Cache-Control": "max-age=0, must-revalidate"},
                )
                return
            if path == "/n02/euckr":
                kr_html = "<!doctype html><html><head><title>N02-EUCKR-한글완료</title></head><body><p>아크래디언트 CP949 디코딩 성공</p></body></html>"
                self.send_bytes(200, "text/html; charset=euc-kr", kr_html.encode("cp949"))
                return
            if path == "/n02/meta-euckr":
                kr_html = "<!doctype html><html><head><meta charset='euc-kr'><title>N02-META-EUCKR-한글완료</title></head><body><p>메타 태그 인코딩 스니핑 성공</p></body></html>"
                self.send_bytes(200, "text/html", kr_html.encode("cp949"))
                return
            if path == "/n02/cp1252":
                # 0x80 = Euro (€), 0x93/0x94 = left/right double smart quotes (“ ”)
                cp_body = (
                    b"<!doctype html><html><head><meta charset='windows-1252'>"
                    b"<title>N02-CP1252-\x80-\x93OK\x94</title></head>"
                    b"<body><p>Price: \x80100 \x93SmartQuotes\x94</p></body></html>"
                )
                self.send_bytes(200, "text/html", cp_body)
                return
            if path == "/n02/404":
                self.send_text(404, "text/html; charset=utf-8", "<!doctype html><title>N02-404</title><h1>404 Not Found</h1>")
                return
            if path == "/n02/bad-length":
                body = b"<!doctype html><title>N02-PARTIAL</title><p>short</p>"
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body) + 128))  # intentional mismatch!
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)
                return

            # ---------------- N03: Slow Image + Fast HTML + Blocking Script ----------------
            if path == "/n03/index.html":
                html = """<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>N03-INIT</title>
  <script src="/n03/fast.js"></script>
</head>
<body>
  <h1 id="fast-heading">Fast HTML Ready</h1>
  <button id="btn" onclick="document.title='N03-CLICKED'">Click Immediately</button>
  <img id="slow-img" src="/n03/slow.png?delay_ms=700" width="64" height="64" alt="slow">
</body>
</html>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return
            if path == "/n03/fast.js":
                self.send_text(200, "text/javascript; charset=utf-8", "document.title = 'N03-FAST-READY';\n")
                return
            if path == "/n03/slow.png":
                self.send_bytes(200, "image/png", TINY_PNG)
                return
            if path == "/n03/page2.html":
                self.send_text(200, "text/html; charset=utf-8", "<!doctype html><title>N03-PAGE2-CLEAN</title><h1>Second Navigation</h1>")
                return

            # ---------------- H01: Malformed HTML, Template, SVG/MathML, Table Fostering, Entities ----------------
            if path == "/h01/index.html":
                html = """<!doctype html>
<html lang="ko">
<head><meta charset="utf-8"><title>H01-PARSER</title></head>
<body>
  <div id="unclosed"><p id="p1">First paragraph<p id="p2">Second paragraph</div>
  <template id="tpl"><span id="inside-tpl">ShouldNotRenderDirectly</span></template>
  <div id="entities">&amp; &lt; &gt; &copy; &#xAC00;&#45208;&#45796;</div>
  <picture id="pic">
    <source srcset="/n01/assets/dot.png 2x" media="(min-width: 600px)">
    <img id="pic-img" src="/n01/assets/dot.png" width="24" height="24" alt="반응형 이미지">
  </picture>
  <svg id="inline-svg" width="24" height="24" viewBox="0 0 24 24"><circle cx="12" cy="12" r="10" fill="#2563eb"/></svg>
  <math id="inline-math"><mrow><mi>x</mi><mo>=</mo><mn>42</mn></mrow></math>
  <table id="tbl">
    <div id="fostered">Fostered Before Table</div>
    <tr><td id="cell1">Cell-1</td><td id="cell2">Cell-2</td></tr>
  </table>
  <script>
    const tpl = document.getElementById('tpl');
    const inDoc = document.getElementById('inside-tpl');
    const ent = document.getElementById('entities').textContent.trim();
    const p1p = document.getElementById('p1').parentNode === document.getElementById('p2').parentNode;
    const hasContent = !!(tpl && tpl.content);
    document.title = (p1p && !inDoc && hasContent && ent.includes('가나다')) ? 'H01-OK' : 'H01-FAIL:' + [p1p, !!inDoc, hasContent, ent].join(',');
  </script>
</body>
</html>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return

            # ---------------- S01: Script Scheduling Order & readyState ----------------
            if path == "/s01/index.html":
                html = """<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>S01-INIT</title>
  <script type="importmap">{"imports":{"@s01/mod":"/s01/mod.js"}}</script>
  <script type="application/json" id="json-data">{"answer":42}</script>
  <script>
    window.__log = ['inline1:' + document.readyState];
    document.addEventListener('DOMContentLoaded', () => {
      window.__log.push('dcl:' + document.readyState);
    });
    window.addEventListener('load', () => {
      window.__log.push('load:' + document.readyState);
      document.title = window.__log.join('|');
    });
  </script>
  <script src="/s01/sync.js"></script>
  <script defer src="/s01/defer1.js"></script>
  <script type="module" src="/s01/entry.js"></script>
  <script defer src="/s01/defer2.js"></script>
  <script nomodule>window.__log.push('ERROR-NOMODULE-RAN');</script>
</head>
<body>
  <div id="target">S01 Body Parsed</div>
  <script>window.__log.push('inline2:' + !!document.getElementById('target'));</script>
</body>
</html>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return
            if path == "/s01/sync.js":
                self.send_text(200, "text/javascript; charset=utf-8", "window.__log.push('sync:' + !!document.getElementById('target'));\n")
                return
            if path == "/s01/defer1.js":
                self.send_text(200, "text/javascript; charset=utf-8", "window.__log.push('defer1:' + document.readyState);\n")
                return
            if path == "/s01/defer2.js":
                self.send_text(200, "text/javascript; charset=utf-8", "window.__log.push('defer2:' + JSON.parse(document.getElementById('json-data').textContent).answer);\n")
                return
            if path == "/s01/mod.js":
                self.send_text(200, "text/javascript; charset=utf-8", "export const tag = 'mod';\n")
                return
            if path == "/s01/entry.js":
                self.send_text(200, "text/javascript; charset=utf-8", "import { tag } from '@s01/mod'; window.__log.push(tag + ':' + document.readyState);\n")
                return

            # ---------------- S02: First External ESM Fixture (05_VERIFICATION.md §51) + Circular + TLA + Dynamic + Errors ----------------
            if path == "/s02/index.html":
                html = """<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>before-esm</title>
  <script type="importmap">{"imports":{"test-value":"./value.js"}}</script>
  <script type="module" src="./entry.js"></script>
</head>
<body>
  <div id="classic-marker">uninitialized</div>
  <script>
    document.getElementById('classic-marker').textContent = 'classic-ok';
  </script>
</body>
</html>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return
            if path == "/s02/value.js":
                self.send_text(200, "text/javascript; charset=utf-8", 'export const value = "esm-ok";\nwindow.__valueEvalCount = (window.__valueEvalCount || 0) + 1;\n')
                return
            if path == "/s02/entry.js":
                self.send_text(200, "text/javascript; charset=utf-8", 'import { value } from "test-value";\nimport { value as v2 } from "./value.js";\ndocument.title = (value === v2 && window.__valueEvalCount === 1) ? value : "dedup-fail";\n')
                return
            if path == "/s02/circular.html":
                html = """<!doctype html><meta charset="utf-8"><title>circ-init</title>
<script type="module" src="/s02/circ_a.js"></script>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return
            if path == "/s02/circ_a.js":
                self.send_text(200, "text/javascript; charset=utf-8",
                    'import { bVal, bumpB, readAFromB } from "./circ_b.js";\n'
                    'export let aVal = 10;\n'
                    'bumpB();\n'
                    'aVal = 25;\n'
                    'document.title = `circ:${bVal}:${readAFromB()}`;\n')
                return
            if path == "/s02/circ_b.js":
                self.send_text(200, "text/javascript; charset=utf-8",
                    'import { aVal } from "./circ_a.js";\n'
                    'export let bVal = 1;\n'
                    'export function bumpB() { bVal += 5; }\n'
                    'export function readAFromB() { return aVal; }\n')
                return
            if path == "/s02/tla.html":
                html = """<!doctype html><meta charset="utf-8"><title>tla-init</title>
<script type="module">
  import { asyncVal } from "./tla_dep.js";
  document.title = "tla:" + asyncVal;
</script>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return
            if path == "/s02/tla_dep.js":
                self.send_text(200, "text/javascript; charset=utf-8",
                    'const v = await Promise.resolve("awaited-2026");\nexport const asyncVal = v;\n')
                return
            if path == "/s02/dyn.html":
                html = """<!doctype html><meta charset="utf-8"><title>dyn-init</title>
<script type="module">
  const metaOk = import.meta.url.endsWith("/s02/dyn.html");
  const m = await import("./value.js");
  document.title = metaOk ? ("dyn:" + m.value) : "dyn-meta-fail";
</script>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return
            if path == "/s02/bad_mime.html":
                html = """<!doctype html><meta charset="utf-8"><title>mime-unchanged</title>
<script type="module" src="/s02/plain.txt"></script>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return
            if path == "/s02/plain.txt":
                self.send_text(200, "text/plain; charset=utf-8", 'document.title = "SHOULD-NOT-RUN";\n')
                return
            if path == "/s02/link_err.html":
                html = """<!doctype html><meta charset="utf-8"><title>link-unchanged</title>
<script type="module">
  import { missingExport } from "./value.js";
  document.title = String(missingExport);
</script>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return

            # ---------------- S03: Event Loop, Microtasks, MutationObserver, CustomElements, Shadow DOM ----------------
            if path == "/s03/index.html":
                html = """<!doctype html>
<html>
<head><meta charset="utf-8"><title>S03-INIT</title></head>
<body>
  <div id="host"></div>
  <ws-counter id="ce" count="3"></ws-counter>
  <script>
    const seq = [];
    class WsCounter extends HTMLElement {
      static get observedAttributes() { return ['count']; }
      connectedCallback() { seq.push('ce-connected:' + this.getAttribute('count')); }
      attributeChangedCallback(n, o, v) { if (o !== null) seq.push('ce-attr:' + v); }
    }
    customElements.define('ws-counter', WsCounter);
    document.getElementById('ce').setAttribute('count', '4');

    const host = document.getElementById('host');
    const sr = host.attachShadow({ mode: 'open' });
    const innerBtn = document.createElement('button');
    innerBtn.textContent = 'ShadowBtn';
    sr.appendChild(innerBtn);
    let retargetedOk = false;
    document.body.addEventListener('click', (e) => {
      retargetedOk = (e.target === host);
      seq.push('retarget:' + retargetedOk);
    });

    const mo = new MutationObserver((recs) => {
      seq.push('mo:' + recs.length);
    });
    mo.observe(host, { attributes: true });

    Promise.resolve().then(() => seq.push('promise1'));
    queueMicrotask(() => seq.push('micro1'));
    host.setAttribute('data-test', '1');
    innerBtn.click();

    setTimeout(() => {
      seq.push('timer');
      requestAnimationFrame(() => {
        seq.push('raf');
        document.title = seq.join(',');
      });
    }, 0);
  </script>
</body>
</html>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return

            # ---------------- S04: Fetch, Streams, CORS, Redirect, AbortController, XHR ----------------
            if path == "/s04/redir":
                self.send_response(302)
                self.send_header("Location", "/s04/final-json")
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return
            if path == "/s04/final-json":
                self.send_text(
                    200,
                    "application/json; charset=utf-8",
                    json.dumps({"step": "redirected-ok"}),
                    {"X-Custom-Header": "arcr-hdr-2026"},
                )
                return
            if path == "/s04/stream":
                self.send_text(200, "text/plain; charset=utf-8", "chunkA-chunkB-chunkC")
                return
            if path == "/s04/index.html":
                html = f"""<!doctype html>
<html><head><meta charset="utf-8"><title>S04-INIT</title></head>
<body>
<script type="module">
  const results = [];
  // 1. Redirect + custom header
  const r1 = await fetch('/s04/redir');
  const j1 = await r1.json();
  results.push('redir:' + r1.url.endsWith('/s04/final-json') + ':' + (r1.headers.get('x-custom-header') === 'arcr-hdr-2026') + ':' + j1.step);

  // 2. ReadableStream reader
  const r2 = await fetch('/s04/stream');
  const reader = r2.body.getReader();
  const dec = new TextDecoder();
  let acc = '';
  while (true) {{
    const {{ done, value }} = await reader.read();
    if (done) break;
    acc += dec.decode(value, {{ stream: true }});
  }}
  results.push('stream:' + (acc === 'chunkA-chunkB-chunkC'));

  // 3. CORS allow vs deny
  const r3 = await fetch('{ctx["cors_origin"]}/cors-ok');
  const j3 = await r3.json();
  results.push('corsOk:' + (j3.cors === 'allowed'));

  let corsBlocked = false;
  try {{
    await fetch('{ctx["cors_origin"]}/cors-deny');
  }} catch (e) {{
    corsBlocked = true;
  }}
  results.push('corsDeny:' + corsBlocked);

  // 4. AbortController
  const ac = new AbortController();
  const pAbort = fetch('/n03/slow.png?delay_ms=600', {{ signal: ac.signal }});
  ac.abort();
  let abortedOk = false;
  try {{
    await pAbort;
  }} catch (e) {{
    abortedOk = true;
  }}
  results.push('abort:' + abortedOk);

  // 5. XHR response headers
  const xhrOk = await new Promise((resolve) => {{
    const x = new XMLHttpRequest();
    x.open('GET', '/s04/final-json');
    x.onload = () => resolve(x.getResponseHeader('x-custom-header') === 'arcr-hdr-2026');
    x.onerror = () => resolve(false);
    x.send();
  }});
  results.push('xhr:' + xhrOk);

  document.title = results.join('|');
</script>
</body></html>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return

            # ---------------- S05: Storage, IndexedDB, BroadcastChannel, Worker, structuredClone ----------------
            if path == "/s05/worker.js":
                self.send_text(200, "text/javascript; charset=utf-8",
                    "self.onmessage = (e) => { self.postMessage({ reply: e.data.n * 2 }); };\n")
                return
            if path == "/s05/index.html":
                html = """<!doctype html>
<html><head><meta charset="utf-8"><title>S05-INIT</title></head>
<body>
<script type="module">
  const out = [];
  // structuredClone
  const orig = { a: 1, nested: { b: [2, 3] } };
  const cloned = structuredClone(orig);
  cloned.nested.b.push(4);
  out.push('clone:' + (orig.nested.b.length === 2 && cloned.nested.b.length === 3));

  // Storage
  localStorage.setItem('s05_k', 'persisted');
  sessionStorage.setItem('s05_s', 'session');
  out.push('storage:' + (localStorage.getItem('s05_k') === 'persisted' && sessionStorage.getItem('s05_s') === 'session'));

  // IndexedDB
  const idbVal = await new Promise((resolve, reject) => {
    const req = indexedDB.open('s05_db', 1);
    req.onupgradeneeded = () => {
      req.result.createObjectStore('kv', { keyPath: 'k' });
    };
    req.onsuccess = () => {
      const db = req.result;
      const tx = db.transaction('kv', 'readwrite');
      const st = tx.objectStore('kv');
      st.put({ k: 'greeting', v: 'idb-ok-2026' });
      const g = st.get('greeting');
      g.onsuccess = () => resolve(g.result ? g.result.v : 'missing');
      g.onerror = () => reject(g.error);
    };
    req.onerror = () => reject(req.error);
  });
  out.push('idb:' + (idbVal === 'idb-ok-2026'));

  // BroadcastChannel
  const bcVal = await new Promise((resolve) => {
    const c1 = new BroadcastChannel('ch1');
    const c2 = new BroadcastChannel('ch1');
    c2.onmessage = (e) => { c1.close(); c2.close(); resolve(e.data); };
    c1.postMessage('bc-ok');
  });
  out.push('bc:' + (bcVal === 'bc-ok'));

  // Dedicated Worker
  const wkVal = await new Promise((resolve) => {
    const w = new Worker('/s05/worker.js');
    w.onmessage = (e) => { w.terminate(); resolve(e.data.reply); };
    w.postMessage({ n: 21 });
  });
  out.push('worker:' + (wkVal === 42));

  document.title = out.join('|');
</script>
</body></html>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return

            # ---------------- C01: Tailwind v4 Layers, Variables, Grid, @supports, @container, @scope, oklch ----------------
            if path == "/c01/index.html":
                html = """<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>C01-INIT</title>
  <style>
    @layer base, components, utilities;
    :root { --brand: oklch(0.62 0.19 255); --gap: 16px; }
    @layer utilities {
      .box { width: 280px; }
    }
    @layer base {
      .box { width: 120px; height: 48px; }
    }
    @supports (display: grid) {
      #grid { display: grid; grid-template-columns: 240px 1fr; gap: var(--gap); }
    }
    @supports (totally-fake-property-2099: bogus) {
      #grid { width: 13px !important; }
    }
    #cq-wrap { width: 500px; container-type: inline-size; }
    #cq-child { width: 100px; height: 30px; }
    @container (min-width: 400px) {
      #cq-child { width: 320px; }
    }
    @scope (.card) to (.slot) {
      p { height: 44px; }
    }
  </style>
</head>
<body>
  <div id="layer-box" class="box">Layer Box</div>
  <div id="grid">
    <aside id="col1">Sidebar</aside>
    <main id="col2">Main</main>
  </div>
  <div id="cq-wrap"><div id="cq-child">Container Child</div></div>
  <div class="card">
    <p id="scoped-p">In Scope</p>
    <div class="slot"><p id="slotted-p">Out of Scope Limit</p></div>
  </div>
  <script>
    const lb = document.getElementById('layer-box').getBoundingClientRect();
    const c1 = document.getElementById('col1').getBoundingClientRect();
    const c2 = document.getElementById('col2').getBoundingClientRect();
    const cq = document.getElementById('cq-child').getBoundingClientRect();
    const supOk = CSS.supports('display', 'grid') && !CSS.supports('totally-fake-property-2099', 'bogus');
    document.title = `layer:${Math.round(lb.width)}|col1:${Math.round(c1.width)}|col2x:${Math.round(c2.x) > Math.round(c1.x)}|cq:${Math.round(cq.width)}|sup:${supOk}`;
  </script>
</body>
</html>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return

            # ---------------- C02: Korean/Emoji/RTL, SVG/Canvas, Fixed+Absolute Scroll HitTest (test_26) ----------------
            if path == "/c02/index.html":
                html = """<!doctype html>
<html lang="ko">
<head>
  <meta charset="utf-8">
  <title>C02-READY</title>
  <style>
    body { margin: 0; height: 2200px; font-family: sans-serif; }
    #hdr { position: fixed; top: 0; left: 0; right: 0; height: 48px; background: #1e293b; color: #fff; z-index: 10; }
    #abs { position: absolute; top: 12px; right: 16px; width: 80px; height: 24px; }
    #abs a { color: #38bdf8; text-decoration: none; }
    #spacer { margin-top: 80px; padding: 16px; }
  </style>
</head>
<body>
  <header id="hdr">
    <span>한글 UI · 🚀 · <bdo dir="rtl">שלום</bdo></span>
    <div id="abs"><a id="pin-link" href="/n03/page2.html">Pinned</a></div>
  </header>
  <div id="spacer">
    <canvas id="cv" width="64" height="32"></canvas>
  </div>
  <script>
    const ctx = document.getElementById('cv').getContext('2d');
    ctx.fillStyle = '#22c55e';
    ctx.fillRect(0, 0, 64, 32);
  </script>
</body>
</html>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return

            # ---------------- A01: Neutral-Host SSR + ESM Hydration + Fetch + History Routing ----------------
            if path in ("/a01/index.html", "/a01/details"):
                items_li = "".join(
                    f'<li class="item" data-id="{it["id"]}">{it["title"]}</li>'
                    for it in ctx["a01_items"]
                )
                route_label = "details" if path.endswith("/details") else "home"
                html = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>A01-SSR:{route_label}</title>
  <script type="importmap">{{"imports":{{"@app/runtime":"/a01/runtime.js"}}}}</script>
  <style>
    @layer base, utilities;
    @layer base {{ body {{ margin: 0; font-family: system-ui, sans-serif; background: #f8fafc; color: #0f172a; }} }}
    @layer utilities {{
      .shell {{ display: grid; grid-template-columns: 220px 1fr; gap: 16px; padding: 16px; }}
      .btn {{ padding: 8px 14px; background: #2563eb; color: #ffffff; border: none; border-radius: 6px; }}
    }}
  </style>
</head>
<body>
  <div id="app" class="shell" data-ssr-route="{route_label}">
    <nav>
      <button id="nav-details" class="btn" type="button">Go Details</button>
    </nav>
    <main>
      <h1 id="view-title">Route: {route_label}</h1>
      <p id="counter" data-count="0">Count: 0</p>
      <button id="inc-btn" class="btn" type="button">Increment</button>
      <form id="add-form">
        <input id="item-input" name="title" value="Hydrated Task" />
        <button id="add-btn" class="btn" type="submit">Add Item</button>
      </form>
      <ul id="item-list">{items_li}</ul>
    </main>
  </div>
  <script type="module" src="/a01/hydrate.js"></script>
</body>
</html>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return
            if path == "/a01/runtime.js":
                js = """export function createStore(initial) {
  let state = { ...initial };
  const subs = new Set();
  return {
    getState: () => state,
    setState(patch) {
      state = { ...state, ...patch };
      for (const fn of subs) fn(state);
    },
    subscribe(fn) { subs.add(fn); return () => subs.delete(fn); }
  };
}
"""
                self.send_text(200, "text/javascript; charset=utf-8", js)
                return
            if path == "/a01/hydrate.js":
                js = """import { createStore } from '@app/runtime';

const appEl = document.getElementById('app');
const ssrList = document.getElementById('item-list');
const store = createStore({
  route: location.pathname.endsWith('/details') ? 'details' : 'home',
  count: 0,
  hydrated: true,
  preservedSsrNode: ssrList
});

function render(st) {
  document.getElementById('view-title').textContent = 'Route: ' + st.route;
  document.getElementById('counter').textContent = 'Count: ' + st.count;
  document.title = `A01-HYDRATED:${st.route}:c=${st.count}:items=${document.querySelectorAll('#item-list li').length}`;
}

document.getElementById('inc-btn').addEventListener('click', () => {
  store.setState({ count: store.getState().count + 1 });
});

document.getElementById('add-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const title = document.getElementById('item-input').value;
  const resp = await fetch('/api/items', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ title })
  });
  const data = await resp.json();
  if (data && data.item) {
    const li = document.createElement('li');
    li.className = 'item';
    li.textContent = data.item.title;
    ssrList.appendChild(li);
    render(store.getState());
  }
});

document.getElementById('nav-details').addEventListener('click', () => {
  history.pushState({ route: 'details' }, '', '/a01/details');
  store.setState({ route: 'details' });
});

window.addEventListener('popstate', () => {
  const r = location.pathname.endsWith('/details') ? 'details' : 'home';
  store.setState({ route: r });
});

store.subscribe(render);
render(store.getState());
"""
                self.send_text(200, "text/javascript; charset=utf-8", js)
                return
            if path == "/api/items":
                self.send_text(200, "application/json; charset=utf-8", json.dumps({"items": ctx["a01_items"]}))
                return

            # ---------------- A02: Video, WebAudio, Canvas, Honest GPU Detection ----------------
            if path == "/a02/index.html":
                html = """<!doctype html>
<html><head><meta charset="utf-8"><title>A02-INIT</title></head>
<body>
  <video id="vid" width="160" height="90" preload="metadata"></video>
  <canvas id="cv2" width="64" height="64"></canvas>
  <script type="module">
    const cv = document.getElementById('cv2');
    const c2d = cv.getContext('2d');
    c2d.fillStyle = '#ef4444';
    c2d.fillRect(0, 0, 64, 64);
    const px = c2d.getImageData(10, 10, 1, 1).data;
    const canvasOk = (px[0] > 200 && px[3] === 255);

    let audioOk = false;
    if (typeof AudioContext === 'function') {
      const ac = new AudioContext();
      const osc = ac.createOscillator();
      const gain = ac.createGain();
      osc.connect(gain);
      gain.connect(ac.destination);
      audioOk = (ac.sampleRate > 0);
      if (ac.close) await ac.close();
    }

    let gpuHonest = true;
    if (navigator.gpu && typeof navigator.gpu.requestAdapter === 'function') {
      const ad = await navigator.gpu.requestAdapter();
      gpuHonest = (ad === null || typeof ad.requestDevice === 'function');
    }
    document.title = `A02:canvas=${canvasOk}:audio=${audioOk}:gpuHonest=${gpuHonest}`;
  </script>
</body></html>"""
                self.send_text(200, "text/html; charset=utf-8", html)
                return

            self.send_text(404, "text/plain; charset=utf-8", f"Not Found: {path}")

    return FixtureHandler


def generate_tls_certs(cert_dir: str):
    ca_key = os.path.join(cert_dir, "ca.key")
    ca_crt = os.path.join(cert_dir, "ca.crt")
    srv_key = os.path.join(cert_dir, "server.key")
    srv_csr = os.path.join(cert_dir, "server.csr")
    srv_crt = os.path.join(cert_dir, "server.crt")
    ext_cfg = os.path.join(cert_dir, "ext.cnf")

    with open(ext_cfg, "w", encoding="utf-8") as f:
        f.write("subjectAltName=IP:127.0.0.1,DNS:localhost\nbasicConstraints=CA:FALSE\n")

    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", ca_key, "-out", ca_crt, "-days", "3650",
         "-subj", "/CN=Arcradient Local Test Root CA"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    subprocess.run(
        ["openssl", "req", "-newkey", "rsa:2048", "-nodes",
         "-keyout", srv_key, "-out", srv_csr,
         "-subj", "/CN=127.0.0.1"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    subprocess.run(
        ["openssl", "x509", "-req", "-in", srv_csr,
         "-CA", ca_crt, "-CAkey", ca_key, "-CAcreateserial",
         "-out", srv_crt, "-days", "3650", "-extfile", ext_cfg],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    return ca_crt, srv_crt, srv_key


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18080)
    ap.add_argument("--cors-port", type=int, default=18081)
    ap.add_argument("--https-port", type=int, default=18443)
    ap.add_argument("--cert-dir", type=str, default="")
    ap.add_argument("--write-manifest", type=str, default="")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    tmp_cert_dir = None
    cert_dir = args.cert_dir
    if not cert_dir:
        tmp_cert_dir = tempfile.mkdtemp(prefix="arcr_tls_")
        cert_dir = tmp_cert_dir
    else:
        os.makedirs(cert_dir, exist_ok=True)

    ca_crt, srv_crt, srv_key = generate_tls_certs(cert_dir)

    ctx = build_routes(args.port, args.cors_port, args.https_port)
    ctx["verbose"] = args.verbose

    main_srv = ThreadedHTTPServer(("127.0.0.1", args.port), make_handler(ctx, False))
    cors_srv = ThreadedHTTPServer(("127.0.0.1", args.cors_port), make_handler(ctx, True))
    https_srv = ThreadedHTTPServer(("127.0.0.1", args.https_port), make_handler(ctx, False))

    ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ssl_ctx.load_cert_chain(certfile=srv_crt, keyfile=srv_key)
    https_srv.socket = ssl_ctx.wrap_socket(https_srv.socket, server_side=True)

    info = {
        "http_port": main_srv.server_port,
        "cors_port": cors_srv.server_port,
        "https_port": https_srv.server_port,
        "main_origin": f"http://127.0.0.1:{main_srv.server_port}",
        "cors_origin": f"http://127.0.0.1:{cors_srv.server_port}",
        "https_origin": f"https://127.0.0.1:{https_srv.server_port}",
        "ca_cert": ca_crt,
    }
    ctx["main_origin"] = info["main_origin"]
    ctx["cors_origin"] = info["cors_origin"]
    ctx["https_origin"] = info["https_origin"]

    if args.write_manifest:
        with open(args.write_manifest, "w", encoding="utf-8") as f:
            json.dump(info, f, indent=2)

    print(json.dumps(info), flush=True)

    for s in (cors_srv, https_srv):
        t = threading.Thread(target=s.serve_forever, daemon=True)
        t.start()

    try:
        main_srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
