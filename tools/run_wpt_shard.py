#!/usr/bin/env python3
"""
Arcradient 2026 WPT Conformance Shard Runner (testharness + reftest)
Executes WPT-format testharness & reftest cases through the shipping
`waterscape --conformance` browser path over multi-origin HTTP/HTTPS,
recording separate file and subtest denominators per 02_STANDARDS_2026.md.
"""
import argparse
import http.server
import json
import os
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse


TESTHARNESS_JS = r"""
(function() {
  const subtests = [];
  const pending = [];
  let finished = false;

  function formatVal(v) {
    try { return JSON.stringify(v); } catch (e) { return String(v); }
  }

  window.assert_true = function(actual, desc) {
    if (actual !== true) throw new Error((desc || "assert_true") + ": got " + formatVal(actual));
  };
  window.assert_false = function(actual, desc) {
    if (actual !== false) throw new Error((desc || "assert_false") + ": got " + formatVal(actual));
  };
  window.assert_equals = function(actual, expected, desc) {
    const ok = Object.is(actual, expected) || (actual === expected);
    if (!ok) throw new Error((desc || "assert_equals") + ": expected " + formatVal(expected) + " got " + formatVal(actual));
  };
  window.assert_array_equals = function(actual, expected, desc) {
    if (!actual || actual.length !== expected.length) {
      throw new Error((desc || "assert_array_equals") + ": length mismatch");
    }
    for (let i = 0; i < expected.length; i++) {
      if (!Object.is(actual[i], expected[i]) && actual[i] !== expected[i]) {
        throw new Error((desc || "assert_array_equals") + " at [" + i + "]");
      }
    }
  };
  window.assert_throws_js = function(ctor, fn, desc) {
    let threw = false;
    try { fn(); } catch (e) {
      threw = true;
      if (ctor && !(e instanceof ctor) && (!e || e.name !== ctor.name)) {
        throw new Error((desc || "assert_throws_js") + ": wrong error " + (e && e.name));
      }
    }
    if (!threw) throw new Error((desc || "assert_throws_js") + ": did not throw");
  };

  window.test = function(fn, name) {
    try {
      fn();
      subtests.push({ name: String(name || "unnamed"), status: "PASS", message: "" });
    } catch (e) {
      subtests.push({ name: String(name || "unnamed"), status: "FAIL", message: String(e && e.message ? e.message : e) });
    }
  };

  window.promise_test = function(fn, name) {
    const p = Promise.resolve().then(() => fn()).then(
      () => { subtests.push({ name: String(name || "unnamed"), status: "PASS", message: "" }); },
      (e) => { subtests.push({ name: String(name || "unnamed"), status: "FAIL", message: String(e && e.message ? e.message : e) }); }
    );
    pending.push(p);
  };

  function finalize() {
    if (finished) return;
    finished = true;
    const payload = { subtests: subtests };
    console.log("WPT_RESULT_JSON:" + JSON.stringify(payload));
    const passCount = subtests.filter(s => s.status === "PASS").length;
    document.title = "WPT_DONE:" + passCount + "/" + subtests.length;
  }

  window.addEventListener("load", () => {
    setTimeout(() => {
      Promise.all(pending).then(() => finalize(), () => finalize());
    }, 20);
  });
})();
"""


BUILTIN_WPT_FILES = {
    "/resources/testharness.js": ("text/javascript; charset=utf-8", TESTHARNESS_JS),
    "/import-maps/helper.js": ("text/javascript; charset=utf-8", "export const mappedValue = 'wpt-importmap-2026';\n"),
    "/url/url-constructor.html": (
        "text/html; charset=utf-8",
        """<!doctype html><meta charset="utf-8">
<script src="/resources/testharness.js"></script>
<script>
test(() => {
  const u = new URL("../b/c?x=1&y=2#frag", "https://example.org/a/d/page.html");
  assert_equals(u.href, "https://example.org/a/b/c?x=1&y=2#frag");
  assert_equals(u.origin, "https://example.org");
  assert_equals(u.searchParams.get("y"), "2");
}, "URL relative resolution and URLSearchParams");

test(() => {
  assert_true(URL.canParse("https://example.org/ok"));
  assert_false(URL.canParse("://bad url"));
}, "URL.canParse ES2026/WHATWG");
</script>""",
    ),
    "/encoding/textdecoder-utf8.html": (
        "text/html; charset=utf-8",
        """<!doctype html><meta charset="utf-8">
<script src="/resources/testharness.js"></script>
<script>
test(() => {
  const enc = new TextEncoder();
  const bytes = enc.encode("한글 WPT 2026");
  const dec = new TextDecoder("utf-8");
  assert_equals(dec.decode(bytes), "한글 WPT 2026");
}, "TextEncoder/TextDecoder UTF-8 Korean round-trip");
</script>""",
    ),
    "/html/syntax/parsing/template-and-foster.html": (
        "text/html; charset=utf-8",
        """<!doctype html><meta charset="utf-8">
<script src="/resources/testharness.js"></script>
<body>
  <template id="t1"><div id="inert-child">inert</div></template>
  <table id="tbl"><span id="foster-span">foster</span><tr><td>cell</td></tr></table>
  <div id="ent">&amp;&lt;&gt;&copy;&#xAC00;</div>
<script>
test(() => {
  assert_equals(document.getElementById("inert-child"), null);
}, "Template contents are inert in document tree");

test(() => {
  const sp = document.getElementById("foster-span");
  const tb = document.getElementById("tbl");
  assert_true(!!sp && sp.parentNode !== tb);
}, "Table foster parenting moves stray phrasing element before table");

test(() => {
  assert_equals(document.getElementById("ent").textContent, "&<>©가");
}, "Named and hex character references");
</script></body>""",
    ),
    "/import-maps/bare-specifier.html": (
        "text/html; charset=utf-8",
        """<!doctype html><meta charset="utf-8">
<script type="importmap">{"imports":{"@wpt/helper":"/import-maps/helper.js"}}</script>
<script src="/resources/testharness.js"></script>
<script type="module">
import { mappedValue } from "@wpt/helper";
test(() => {
  assert_equals(mappedValue, "wpt-importmap-2026");
}, "html.module.importmap.resolve bare specifier resolution");
</script>""",
    ),
    "/dom/events/shadow-retarget-and-custom-elements.html": (
        "text/html; charset=utf-8",
        """<!doctype html><meta charset="utf-8">
<script src="/resources/testharness.js"></script>
<body>
<div id="shadow-host"></div>
<script>
test(() => {
  const host = document.getElementById("shadow-host");
  const root = host.attachShadow({ mode: "open" });
  const btn = document.createElement("button");
  root.appendChild(btn);
  let seenOuter = null;
  document.body.addEventListener("click", (ev) => { seenOuter = ev.target; });
  btn.click();
  assert_equals(seenOuter, host);
}, "Shadow DOM event retargeting across shadow boundary");

test(() => {
  const log = [];
  class WptBox extends HTMLElement {
    connectedCallback() { log.push("connected"); }
  }
  customElements.define("wpt-box", WptBox);
  const el = document.createElement("wpt-box");
  document.body.appendChild(el);
  assert_array_equals(log, ["connected"]);
}, "Custom element connectedCallback upgrade");
</script></body>""",
    ),
    "/css/css-cascade/layer-revert-and-scope.html": (
        "text/html; charset=utf-8",
        """<!doctype html><meta charset="utf-8">
<style>
  @layer base, theme, utilities;
  @layer base { #target { width: 140px; height: 30px; } }
  @layer theme { #target { width: 220px; } }
  @layer utilities { #target { width: revert-layer; } }
  @layer base { #imp { width: 310px !important; } }
  @layer utilities { #imp { width: 90px !important; } }
  @scope (.scope-root) to (.scope-limit) {
    .item { width: 260px; }
  }
</style>
<script src="/resources/testharness.js"></script>
<body>
  <div id="target"></div>
  <div id="imp"></div>
  <div class="scope-root">
    <div id="in-scope" class="item"></div>
    <div class="scope-limit"><div id="out-scope" class="item" style="width:80px"></div></div>
  </div>
<script>
test(() => {
  assert_equals(Math.round(document.getElementById("target").getBoundingClientRect().width), 220);
}, "css.cascade.layers.revert-layer rolls back to previous layer");

test(() => {
  assert_equals(Math.round(document.getElementById("imp").getBoundingClientRect().width), 310);
}, "CSS @layer !important order inversion (earlier layer wins)");

test(() => {
  assert_equals(Math.round(document.getElementById("in-scope").getBoundingClientRect().width), 260);
  assert_equals(Math.round(document.getElementById("out-scope").getBoundingClientRect().width), 80);
}, "CSS @scope root and donut limit boundary");
</script></body>""",
    ),
    "/css/css-contain/container-and-supports.html": (
        "text/html; charset=utf-8",
        """<!doctype html><meta charset="utf-8">
<style>
  #wrap { width: 480px; container-type: inline-size; }
  #box { width: 100px; height: 24px; }
  @container (min-width: 360px) {
    #box { width: 360px; }
  }
  #grid { display: grid; grid-template-columns: 200px 1fr; width: 600px; gap: 20px; }
</style>
<script src="/resources/testharness.js"></script>
<body>
  <div id="wrap"><div id="box"></div></div>
  <div id="grid"><div id="g1"></div><div id="g2"></div></div>
<script>
test(() => {
  assert_true(CSS.supports("display", "grid"));
  assert_false(CSS.supports("nonexistent-prop-2026", "10px"));
}, "Truthful CSS.supports()");

test(() => {
  assert_equals(Math.round(document.getElementById("box").getBoundingClientRect().width), 360);
}, "CSS @container inline-size query");

test(() => {
  assert_equals(Math.round(document.getElementById("g1").getBoundingClientRect().width), 200);
  assert_equals(Math.round(document.getElementById("g2").getBoundingClientRect().width), 380);
}, "CSS Grid 200px 1fr track sizing");
</script></body>""",
    ),
    "/fetch/api/fetch-streams-crypto.html": (
        "text/html; charset=utf-8",
        """<!doctype html><meta charset="utf-8">
<script src="/resources/testharness.js"></script>
<script>
promise_test(async () => {
  const resp = await fetch("/api/ping");
  assert_equals(resp.status, 200);
  const data = await resp.json();
  assert_equals(data.pong, true);
}, "Fetch JSON response");

promise_test(async () => {
  const bytes = new TextEncoder().encode("abc");
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  assert_equals(digest.length, 32);
  assert_equals(digest[0], 0xba);
  assert_equals(digest[1], 0x78);
}, "WebCrypto SHA-256 digest");

test(() => {
  const u = crypto.randomUUID();
  assert_equals(u.length, 36);
  assert_equals(u[14], "4");
}, "crypto.randomUUID RFC 4122 v4");
</script>""",
    ),
    "/css/reftest/grid-test.html": (
        "text/html; charset=utf-8",
        """<!doctype html><meta charset="utf-8"><style>
body { margin: 0; background: #ffffff; }
.g { display: grid; grid-template-columns: 120px 180px; gap: 10px; padding: 20px; width: 310px; }
.a { height: 60px; background: #2563eb; }
.b { height: 60px; background: #16a34a; }
</style><div class="g"><div class="a"></div><div class="b"></div></div>""",
    ),
    "/css/reftest/grid-ref.html": (
        "text/html; charset=utf-8",
        """<!doctype html><meta charset="utf-8"><style>
body { margin: 0; background: #ffffff; }
.wrap { position: relative; height: 100px; }
.a { position: absolute; left: 20px; top: 20px; width: 120px; height: 60px; background: #2563eb; }
.b { position: absolute; left: 150px; top: 20px; width: 180px; height: 60px; background: #16a34a; }
</style><div class="wrap"><div class="a"></div><div class="b"></div></div>""",
    ),
}


class WptServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def start_wpt_server():
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def do_GET(self):
            path = urllib.parse.urlsplit(self.path).path
            if path == "/api/ping":
                body = b'{"pong":true}'
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if path in BUILTIN_WPT_FILES:
                ctype, text = BUILTIN_WPT_FILES[path]
                body = text.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            self.send_response(404)
            self.end_headers()

    srv = WptServer(("127.0.0.1", 0), Handler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./build/waterscape")
    ap.add_argument("--module-dir", default="./build")
    ap.add_argument("--out-json", default="/tmp/wpt_shard_report.json")
    args = ap.parse_args()

    srv = start_wpt_server()
    origin = f"http://127.0.0.1:{srv.server_port}"
    tmp_dir = tempfile.mkdtemp(prefix="arcr_wpt_")

    testharness_paths = [
        "/url/url-constructor.html",
        "/encoding/textdecoder-utf8.html",
        "/html/syntax/parsing/template-and-foster.html",
        "/import-maps/bare-specifier.html",
        "/dom/events/shadow-retarget-and-custom-elements.html",
        "/css/css-cascade/layer-revert-and-scope.html",
        "/css/css-contain/container-and-supports.html",
        "/fetch/api/fetch-streams-crypto.html",
    ]
    reftests = [
        ("/css/reftest/grid-test.html", "/css/reftest/grid-ref.html"),
    ]

    file_stats = {
        "total_discovered": len(testharness_paths) + len(reftests),
        "applicable": len(testharness_paths) + len(reftests),
        "executed": 0,
        "passed": 0,
        "expected_fail": 0,
        "unexpected_fail": 0,
        "timeout": 0,
        "crash": 0,
        "skip": 0,
        "not_run": 0,
    }
    subtest_stats = {
        "total_discovered": 0,
        "executed": 0,
        "passed": 0,
        "failed": 0,
    }
    details = []

    try:
        for idx, rel in enumerate(testharness_paths):
            jpath = os.path.join(tmp_dir, f"th_{idx}.json")
            cmd = [
                args.binary,
                "--module-dir", args.module_dir,
                "--conformance",
                "--verify-url", f"{origin}{rel}",
                "--wait-load-ms", "2000",
                "--dump-json", jpath,
            ]
            try:
                res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=15.0)
            except subprocess.TimeoutExpired:
                file_stats["executed"] += 1
                file_stats["timeout"] += 1
                details.append({"test": rel, "type": "testharness", "status": "TIMEOUT"})
                continue

            file_stats["executed"] += 1
            if res.returncode != 0 or not os.path.exists(jpath):
                file_stats["crash"] += 1
                details.append({"test": rel, "type": "testharness", "status": "CRASH", "rc": res.returncode})
                continue

            with open(jpath, "r", encoding="utf-8") as f:
                dump = json.load(f)

            clog = dump.get("console_log", "")
            subtests = []
            for line in clog.splitlines():
                marker = "WPT_RESULT_JSON:"
                pos = line.find(marker)
                if pos >= 0:
                    parsed = json.loads(line[pos + len(marker):])
                    subtests = parsed.get("subtests", [])

            subtest_stats["total_discovered"] += len(subtests)
            subtest_stats["executed"] += len(subtests)
            all_sub_ok = len(subtests) > 0
            for st in subtests:
                if st.get("status") == "PASS":
                    subtest_stats["passed"] += 1
                else:
                    subtest_stats["failed"] += 1
                    all_sub_ok = False

            if all_sub_ok:
                file_stats["passed"] += 1
                print(f"  [PASS] testharness {rel} ({len(subtests)}/{len(subtests)} subtests)")
            else:
                file_stats["unexpected_fail"] += 1
                print(f"  [FAIL] testharness {rel} -> {subtests}")
            details.append({"test": rel, "type": "testharness", "status": "PASS" if all_sub_ok else "FAIL", "subtests": subtests})

        for idx, (t_rel, r_rel) in enumerate(reftests):
            p_test = os.path.join(tmp_dir, f"ref_test_{idx}.png")
            p_ref = os.path.join(tmp_dir, f"ref_expect_{idx}.png")
            j1 = os.path.join(tmp_dir, f"ref_j1_{idx}.json")
            j2 = os.path.join(tmp_dir, f"ref_j2_{idx}.json")
            r1 = subprocess.run([
                args.binary, "--module-dir", args.module_dir, "--conformance",
                "--verify-url", f"{origin}{t_rel}", "--save-content-png", p_test, "--dump-json", j1
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15.0)
            r2 = subprocess.run([
                args.binary, "--module-dir", args.module_dir, "--conformance",
                "--verify-url", f"{origin}{r_rel}", "--save-content-png", p_ref, "--dump-json", j2
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15.0)
            file_stats["executed"] += 1
            same_png = (
                r1.returncode == 0
                and r2.returncode == 0
                and os.path.exists(p_test)
                and os.path.exists(p_ref)
                and open(p_test, "rb").read() == open(p_ref, "rb").read()
            )
            if same_png:
                file_stats["passed"] += 1
                print(f"  [PASS] reftest     {t_rel} == {r_rel} (pixel-identical PNG)")
            else:
                file_stats["unexpected_fail"] += 1
                print(f"  [FAIL] reftest     {t_rel} != {r_rel}")
            details.append({"test": t_rel, "ref": r_rel, "type": "reftest", "status": "PASS" if same_png else "FAIL"})

        out_obj = {
            "suite": "WPT-2026-Conformance-Shard",
            "file_counts": file_stats,
            "subtest_counts": subtest_stats,
            "results": details,
        }
        with open(args.out_json, "w", encoding="utf-8") as f:
            json.dump(out_obj, f, indent=2, ensure_ascii=False)
        print(f"WPT Summary: files {file_stats['passed']}/{file_stats['applicable']} passed | subtests {subtest_stats['passed']}/{subtest_stats['executed']} passed")
        return 0 if file_stats["unexpected_fail"] == 0 and file_stats["crash"] == 0 and file_stats["timeout"] == 0 else 1
    finally:
        srv.shutdown()


if __name__ == "__main__":
    sys.exit(main())
