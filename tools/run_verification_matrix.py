#!/usr/bin/env python3
"""
Arcradient 2026 End-to-End Verification Matrix, Benchmark & Live-Site Runner
Covers 05_VERIFICATION.md IDs: N01, N02, N03, H01, S01, S02, S03, S04, S05,
C01, C02, A01, A02, W01, R01 + Cold/Warm P50/P95 Benchmarks + Honest Live-Site Audit.
"""
import argparse
import json
import os
import resource
import shutil
import statistics
import subprocess
import sys
import tempfile
import time


def start_fixture_server(repo_root: str, cert_dir: str, manifest_path: str):
    srv_script = os.path.join(repo_root, "tools", "fixture_server.py")
    proc = subprocess.Popen(
        [
            sys.executable,
            srv_script,
            "--port", "0",
            "--cors-port", "0",
            "--https-port", "0",
            "--cert-dir", cert_dir,
            "--write-manifest", manifest_path,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    deadline = time.time() + 10.0
    while time.time() < deadline:
        if os.path.exists(manifest_path) and os.path.getsize(manifest_path) > 10:
            try:
                with open(manifest_path, "r", encoding="utf-8") as f:
                    info = json.load(f)
                if info.get("http_port"):
                    return proc, info
            except Exception:
                pass
        if proc.poll() is not None:
            out, err = proc.communicate()
            raise RuntimeError(f"fixture_server exited early: {out}\n{err}")
        time.sleep(0.05)
    proc.kill()
    raise RuntimeError("Timed out waiting for fixture_server manifest")


def run_waterscape_verify(
    binary: str,
    mod_dir: str,
    url: str,
    out_dir: str,
    tag: str,
    extra_args=None,
    env_overrides=None,
    conformance=True,
    timeout_sec=20.0,
):
    json_path = os.path.join(out_dir, f"{tag}.json")
    png_path = os.path.join(out_dir, f"{tag}.png")
    cmd = [
        binary,
        "--module-dir", mod_dir,
        "--verify-url", url,
        "--dump-json", json_path,
        "--save-png", png_path,
    ]
    if conformance:
        cmd.append("--conformance")
    if extra_args:
        cmd.extend(extra_args)

    env = os.environ.copy()
    if env_overrides:
        for k, v in env_overrides.items():
            if v is None:
                env.pop(k, None)
            else:
                env[k] = v

    t0 = time.perf_counter()
    res = subprocess.run(
        cmd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout_sec,
    )
    wall_ms = (time.perf_counter() - t0) * 1000.0
    data = {}
    if os.path.exists(json_path):
        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    data["_exit_code"] = res.returncode
    data["_wall_ms"] = round(wall_ms, 2)
    data["_json_path"] = json_path
    data["_png_path"] = png_path
    data["_cmd"] = " ".join(cmd)
    return data


def run_fixture_matrix(binary: str, mod_dir: str, info: dict, out_dir: str):
    http_orig = info["main_origin"]
    https_orig = info["https_origin"]
    ca_cert = info["ca_cert"]
    results = []

    def record(case_id: str, passed: bool, expected: str, actual: str, cmd: str, evidence: str):
        results.append({
            "id": case_id,
            "passed": bool(passed),
            "expected": expected,
            "actual": actual,
            "cmd": cmd,
            "evidence": evidence,
        })
        status = "PASS" if passed else "FAIL"
        print(f"  [{status}] {case_id:<18} | actual: {actual}")

    # ---------------- N01 ----------------
    d_n01_http = run_waterscape_verify(binary, mod_dir, f"{http_orig}/n01/start", out_dir, "n01_http")
    tr_urls = [t.get("final_url", "") for t in d_n01_http.get("traces", [])]
    has_css = any(u.endswith("/n01/assets/style.css") for u in tr_urls)
    has_img = any(u.endswith("/n01/assets/dot.png") for u in tr_urls)
    ok_n01_http = (
        d_n01_http.get("url", "").endswith("/n01/sub/page.html")
        and d_n01_http.get("title") == "N01 한글 리디렉션 검증"
        and has_css
        and has_img
    )
    record(
        "N01-http",
        ok_n01_http,
        "302->/n01/sub/page.html, base /n01/assets/ CSS+PNG 200, Korean title",
        f"url={d_n01_http.get('url')} title={d_n01_http.get('title')} css={has_css} img={has_img}",
        d_n01_http["_cmd"],
        d_n01_http["_json_path"],
    )

    d_n01_https = run_waterscape_verify(
        binary, mod_dir, f"{https_orig}/n01/start", out_dir, "n01_https_trusted",
        env_overrides={"ARCR_SSL_CA_FILE": ca_cert},
    )
    ok_n01_https = (
        d_n01_https.get("security_state") == 2
        and d_n01_https.get("title") == "N01 한글 리디렉션 검증"
    )
    record(
        "N01-https-ok",
        ok_n01_https,
        "HTTPS verified (security_state=2) + Korean title",
        f"sec={d_n01_https.get('security_state')} title={d_n01_https.get('title')}",
        d_n01_https["_cmd"],
        d_n01_https["_json_path"],
    )

    d_n01_bad_tls = run_waterscape_verify(
        binary, mod_dir, f"{https_orig}/n01/start", out_dir, "n01_https_untrusted",
        env_overrides={"ARCR_SSL_CA_FILE": "/nonexistent_ca_bundle.pem"},
    )
    tls_blocked = any(t.get("blocked_reason") == "tls-error" for t in d_n01_bad_tls.get("traces", []))
    ok_n01_bad_tls = (d_n01_bad_tls.get("security_state") == 3) or tls_blocked
    record(
        "N01-tls-fail",
        ok_n01_bad_tls,
        "Untrusted cert surfaces security_state=3 or blocked_reason=tls-error",
        f"sec={d_n01_bad_tls.get('security_state')} tls_blocked={tls_blocked}",
        d_n01_bad_tls["_cmd"],
        d_n01_bad_tls["_json_path"],
    )

    cred_url = http_orig.replace("http://", "http://alice:TopSecret2026@") + "/n01/sub/page.html"
    d_n01_cred = run_waterscape_verify(binary, mod_dir, cred_url, out_dir, "n01_cred_sanitize")
    raw_dump = open(d_n01_cred["_json_path"], "r", encoding="utf-8").read()
    ok_cred = "TopSecret2026" not in raw_dump and "alice:" not in raw_dump
    record(
        "N01-cred-strip",
        ok_cred,
        "URL credentials stripped from request traces & telemetry",
        f"leaked={'no' if ok_cred else 'YES'}",
        "waterscape --verify-url http://<redacted>@127.0.0.1/n01/sub/page.html",
        d_n01_cred["_json_path"],
    )

    # ---------------- N02 ----------------
    for sub, exp_title in [
        ("gzip", "N02-GZIP-OK"),
        ("brotli", "N02-BROTLI-OK"),
        ("chunked", "N02-CHUNKED-OK"),
        ("euckr", "N02-EUCKR-한글완료"),
        ("meta-euckr", "N02-META-EUCKR-한글완료"),
        ("cp1252", "N02-CP1252-€-\u201cOK\u201d"),
        ("404", "N02-404"),
    ]:
        d = run_waterscape_verify(binary, mod_dir, f"{http_orig}/n02/{sub}", out_dir, f"n02_{sub}")
        ok = (d.get("title") == exp_title)
        if sub == "404":
            ok = ok and any(t.get("status_code") == 404 for t in d.get("traces", []))
        record(
            f"N02-{sub}",
            ok,
            f"title={exp_title}",
            f"title={d.get('title')}",
            d["_cmd"],
            d["_json_path"],
        )

    d_badlen = run_waterscape_verify(binary, mod_dir, f"{http_orig}/n02/bad-length", out_dir, "n02_badlen")
    record(
        "N02-bad-length",
        d_badlen.get("_exit_code") == 0,
        "Graceful handling of Content-Length mismatch (exit 0)",
        f"exit={d_badlen.get('_exit_code')} title={d_badlen.get('title')}",
        d_badlen["_cmd"],
        d_badlen["_json_path"],
    )

    # ---------------- N03 ----------------
    d_n03 = run_waterscape_verify(
        binary, mod_dir, f"{http_orig}/n03/index.html", out_dir, "n03_nonblocking",
        extra_args=["--early-click", "60,65", "--wait-load-ms", "1500"],
    )
    fp_ms = d_n03.get("first_paint_ms", 9999.0)
    ok_n03 = (
        fp_ms < 450.0
        and d_n03.get("title_at_first_paint") == "N03-FAST-READY"
        and d_n03.get("title_after_early_click") == "N03-CLICKED"
        and d_n03.get("ready_state") == "complete"
    )
    record(
        "N03-nonblocking",
        ok_n03,
        "first_paint_ms<450 (despite 700ms image), early click->N03-CLICKED, final complete",
        f"fp={fp_ms}ms init={d_n03.get('title_at_first_paint')} click={d_n03.get('title_after_early_click')} rs={d_n03.get('ready_state')}",
        d_n03["_cmd"],
        d_n03["_json_path"],
    )

    d_n03_cancel = run_waterscape_verify(
        binary, mod_dir, f"{http_orig}/n03/index.html", out_dir, "n03_cancel",
        extra_args=["--second-url", f"{http_orig}/n03/page2.html", "--wait-load-ms", "900"],
    )
    sec_ms = d_n03_cancel.get("second_nav_ms", 9999.0)
    ok_n03_cancel = (
        sec_ms < 250.0
        and d_n03_cancel.get("title") == "N03-PAGE2-CLEAN"
        and d_n03_cancel.get("url", "").endswith("/n03/page2.html")
    )
    record(
        "N03-cancel",
        ok_n03_cancel,
        "second_nav_ms<250ms (<=50ms socket cancel) & stale slow.png never corrupts page2",
        f"second_nav_ms={sec_ms}ms title={d_n03_cancel.get('title')}",
        d_n03_cancel["_cmd"],
        d_n03_cancel["_json_path"],
    )

    # ---------------- H01 ----------------
    h01_js = (
        "(() => {"
        "const p1 = document.getElementById('p1');"
        "const p2 = document.getElementById('p2');"
        "const ent = document.getElementById('entities').textContent;"
        "const fos = document.getElementById('fostered');"
        "const tbl = document.getElementById('tbl');"
        "const tpl = document.getElementById('inside-tpl');"
        "const svg = document.getElementById('inline-svg');"
        "const pOk = p1 && p2 && p1.parentNode === p2.parentNode;"
        "const entOk = ent.includes('©') && ent.includes('가나다');"
        "const fosOk = fos && tbl && fos.parentNode !== tbl;"
        "const tplOk = (tpl === null);"
        "const svgOk = !!svg;"
        "document.title = `H01:p=${pOk}:ent=${entOk}:fos=${fosOk}:tpl=${tplOk}:svg=${svgOk}`;"
        "})();"
    )
    d_h01 = run_waterscape_verify(
        binary, mod_dir, f"{http_orig}/h01/index.html", out_dir, "h01_parser",
        extra_args=["--eval-js", h01_js],
    )
    exp_h01 = "H01:p=true:ent=true:fos=true:tpl=true:svg=true"
    record(
        "H01-html5",
        d_h01.get("title") == exp_h01,
        exp_h01,
        f"title={d_h01.get('title')}",
        d_h01["_cmd"],
        d_h01["_json_path"],
    )

    # ---------------- S01 ----------------
    d_s01 = run_waterscape_verify(binary, mod_dir, f"{http_orig}/s01/index.html", out_dir, "s01_order")
    exp_s01 = "inline1:loading|sync:false|inline2:true|defer1:interactive|mod:interactive|defer2:42|dcl:interactive|load:complete"
    record(
        "S01-order",
        d_s01.get("title") == exp_s01,
        exp_s01,
        f"title={d_s01.get('title')}",
        d_s01["_cmd"],
        d_s01["_json_path"],
    )

    # ---------------- S02 ----------------
    for sub, path, exp_t in [
        ("importmap", "/s02/index.html", "esm-ok"),
        ("circular", "/s02/circular.html", "circ:6:25"),
        ("tla", "/s02/tla.html", "tla:awaited-2026"),
        ("dynamic", "/s02/dyn.html", "dyn:esm-ok"),
        ("bad-mime", "/s02/bad_mime.html", "mime-unchanged"),
        ("link-err", "/s02/link_err.html", "link-unchanged"),
    ]:
        d = run_waterscape_verify(binary, mod_dir, f"{http_orig}{path}", out_dir, f"s02_{sub}")
        ok = d.get("title") == exp_t
        if sub == "bad-mime":
            ok = ok and any(t.get("blocked_reason") == "bad-mime" for t in d.get("traces", []))
        if sub == "link-err":
            ok = ok and ("missingExport" in d.get("console_log", "") or "SyntaxError" in d.get("console_log", ""))
        record(
            f"S02-{sub}",
            ok,
            f"title={exp_t}",
            f"title={d.get('title')} console_len={len(d.get('console_log', ''))}",
            d["_cmd"],
            d["_json_path"],
        )

    # ---------------- S03 ----------------
    d_s03 = run_waterscape_verify(binary, mod_dir, f"{http_orig}/s03/index.html", out_dir, "s03_events")
    t_s03 = d_s03.get("title", "")
    ok_s03 = (
        "ce-connected:3" in t_s03
        and "ce-attr:4" in t_s03
        and "retarget:true" in t_s03
        and "promise1" in t_s03
        and "micro1" in t_s03
        and "mo:1" in t_s03
        and t_s03.endswith("timer,raf")
    )
    record(
        "S03-dom-loop",
        ok_s03,
        "ce-connected:3, ce-attr:4, retarget:true, microtasks+MO before timer,raf",
        f"title={t_s03}",
        d_s03["_cmd"],
        d_s03["_json_path"],
    )

    # ---------------- S04 ----------------
    d_s04 = run_waterscape_verify(
        binary, mod_dir, f"{http_orig}/s04/index.html", out_dir, "s04_fetch",
        extra_args=["--wait-load-ms", "3000"],
    )
    exp_s04 = "redir:true:true:redirected-ok|stream:true|corsOk:true|corsDeny:true|abort:true|xhr:true"
    record(
        "S04-fetch-cors",
        d_s04.get("title") == exp_s04,
        exp_s04,
        f"title={d_s04.get('title')}",
        d_s04["_cmd"],
        d_s04["_json_path"],
    )

    # ---------------- S05 ----------------
    d_s05 = run_waterscape_verify(
        binary, mod_dir, f"{http_orig}/s05/index.html", out_dir, "s05_storage_worker",
        extra_args=["--wait-load-ms", "3000"],
    )
    exp_s05 = "clone:true|storage:true|idb:true|bc:true|worker:true"
    record(
        "S05-storage-wk",
        d_s05.get("title") == exp_s05,
        exp_s05,
        f"title={d_s05.get('title')}",
        d_s05["_cmd"],
        d_s05["_json_path"],
    )

    # ---------------- C01 ----------------
    d_c01 = run_waterscape_verify(binary, mod_dir, f"{http_orig}/c01/index.html", out_dir, "c01_css2026")
    exp_c01 = "layer:280|col1:240|col2x:true|cq:320|sup:true"
    record(
        "C01-css2026",
        d_c01.get("title") == exp_c01,
        exp_c01,
        f"title={d_c01.get('title')}",
        d_c01["_cmd"],
        d_c01["_json_path"],
    )

    # ---------------- C02 ----------------
    d_c02 = run_waterscape_verify(
        binary, mod_dir, f"{http_orig}/c02/index.html", out_dir, "c02_fixed_scroll",
        extra_args=["--window-size", "1280x800", "--scroll-y", "600", "--click", "1224,24"],
    )
    record(
        "C02-fixed-hit",
        d_c02.get("title") == "N03-PAGE2-CLEAN",
        "Click fixed>abs #pin-link at scroll_y=600 navigates to N03-PAGE2-CLEAN",
        f"title={d_c02.get('title')} url={d_c02.get('url')}",
        d_c02["_cmd"],
        d_c02["_json_path"],
    )

    # ---------------- A01 ----------------
    a01_flow_js = (
        "(async () => {"
        "  document.getElementById('inc-btn').click();"
        "  document.getElementById('add-form').dispatchEvent(new Event('submit', {cancelable:true, bubbles:true}));"
        "  await new Promise(r => setTimeout(r, 80));"
        "  document.getElementById('nav-details').click();"
        "  const atDetails = document.title;"
        "  history.back();"
        "  await new Promise(r => setTimeout(r, 30));"
        "  document.title = atDetails + '->' + document.title;"
        "})();"
    )
    d_a01 = run_waterscape_verify(
        binary, mod_dir, f"{http_orig}/a01/index.html", out_dir, "a01_spa_hydration",
        extra_args=["--eval-js", a01_flow_js],
    )
    exp_a01 = "A01-HYDRATED:details:c=1:items=3->A01-HYDRATED:home:c=1:items=3"
    record(
        "A01-hydration",
        d_a01.get("title") == exp_a01,
        exp_a01,
        f"title={d_a01.get('title')}",
        d_a01["_cmd"],
        d_a01["_json_path"],
    )

    # ---------------- A02 ----------------
    d_a02 = run_waterscape_verify(binary, mod_dir, f"{http_orig}/a02/index.html", out_dir, "a02_media_canvas")
    exp_a02 = "A02:canvas=true:audio=true:gpuHonest=true"
    record(
        "A02-media-gpu",
        d_a02.get("title") == exp_a02,
        exp_a02,
        f"title={d_a02.get('title')}",
        d_a02["_cmd"],
        d_a02["_json_path"],
    )

    # ---------------- W01 ----------------
    sl_96 = d_c01.get("shell_layout", {})
    d_dpi192 = run_waterscape_verify(
        binary, mod_dir, "waterscape://addons", out_dir, "w01_dpi192_dock_right",
        extra_args=["--dpi", "192", "--devtools", "right", "--window-size", "1280x800"],
    )
    sl_192 = d_dpi192.get("shell_layout", {})
    ok_w01 = (
        sl_96.get("frame_mode") == 0
        and sl_96.get("window_caption_rect", {}).get("height", -1) == 0
        and sl_192.get("tab_strip_rect", {}).get("height", 0) >= sl_96.get("tab_strip_rect", {}).get("height", 1) * 2 - 4
        and sl_192.get("devtools_dock_rect", {}).get("width", 0) > 200
    )
    record(
        "W01-frame-dpi",
        ok_w01,
        "Single-frame (window_caption_rect.h==0), 192 DPI 2x chrome scale, right DevTools dock",
        f"cap_h={sl_96.get('window_caption_rect', {}).get('height')} tab96={sl_96.get('tab_strip_rect', {}).get('height')} tab192={sl_192.get('tab_strip_rect', {}).get('height')} dock_w={sl_192.get('devtools_dock_rect', {}).get('width')}",
        d_dpi192["_cmd"],
        d_dpi192["_json_path"],
    )

    # ---------------- R01 ----------------
    d_r01 = run_waterscape_verify(binary, mod_dir, "waterscape://hardware", out_dir, "r01_lazy_tier4")
    ok_r01 = (
        d_r01.get("_exit_code") == 0
        and d_r01.get("stats", {}).get("js_module_loaded") is False
    )
    record(
        "R01-tier4-lazy",
        ok_r01,
        "Scriptless page keeps js_module_loaded==false & renders cleanly",
        f"exit={d_r01.get('_exit_code')} js_loaded={d_r01.get('stats', {}).get('js_module_loaded')}",
        d_r01["_cmd"],
        d_r01["_json_path"],
    )

    return results


def run_perf_benchmarks(binary: str, mod_dir: str, info: dict, out_dir: str, iterations: int = 9):
    http_orig = info["main_origin"]
    targets = [
        ("static_n01", f"{http_orig}/n01/sub/page.html"),
        ("css2026_c01", f"{http_orig}/c01/index.html"),
        ("spa_a01", f"{http_orig}/a01/index.html"),
        ("media_a02", f"{http_orig}/a02/index.html"),
    ]
    bench_summary = {}
    for label, url in targets:
        first_paints = []
        totals = []
        arenas = []
        for idx in range(iterations):
            d = run_waterscape_verify(
                binary, mod_dir, url, out_dir, f"bench_{label}_{idx}",
                extra_args=["--wait-load-ms", "1500"],
            )
            first_paints.append(float(d.get("first_paint_ms", 0.0)))
            totals.append(float(d.get("total_load_ms", 0.0)))
            arenas.append(int(d.get("stats", {}).get("arena_allocated_bytes", 0)))
        fp_sorted = sorted(first_paints)
        tot_sorted = sorted(totals)
        p95_idx = min(len(fp_sorted) - 1, max(0, int(round(0.95 * (len(fp_sorted) - 1)))))
        bench_summary[label] = {
            "iterations": iterations,
            "cold_first_paint_ms": round(first_paints[0], 3),
            "warm_p50_first_paint_ms": round(statistics.median(first_paints[1:] or first_paints), 3),
            "p95_first_paint_ms": round(fp_sorted[p95_idx], 3),
            "p50_total_load_ms": round(statistics.median(totals), 3),
            "p95_total_load_ms": round(tot_sorted[p95_idx], 3),
            "peak_arena_kib": round(max(arenas) / 1024.0, 1),
        }
        print(f"  [BENCH] {label:<14} | P50 fp={bench_summary[label]['warm_p50_first_paint_ms']}ms P95 fp={bench_summary[label]['p95_first_paint_ms']}ms arena={bench_summary[label]['peak_arena_kib']}KiB")
    rusage = resource.getrusage(resource.RUSAGE_CHILDREN)
    bench_summary["child_max_rss_kib"] = int(rusage.ru_maxrss)
    return bench_summary


def run_live_site_checks(binary: str, mod_dir: str, out_dir: str):
    sites = [
        ("zuzunza", "https://www.zuzunza.com/"),
        ("google", "https://www.google.com/"),
        ("youtube", "https://www.youtube.com/"),
        ("x_com", "https://x.com/"),
    ]
    live_results = []
    for name, url in sites:
        for mode in ("conformance", "default"):
            tag = f"live_{name}_{mode}"
            try:
                d = run_waterscape_verify(
                    binary, mod_dir, url, out_dir, tag,
                    extra_args=["--wait-load-ms", "4000"],
                    conformance=(mode == "conformance"),
                    timeout_sec=45.0,
                )
                traces = d.get("traces", [])
                nav_status = traces[0].get("status_code", 0) if traces else 0
                nav_blocked = traces[0].get("blocked_reason", "") if traces else "no-trace"
                live_results.append({
                    "site": name,
                    "mode": mode,
                    "requested_url": url,
                    "final_url": d.get("url", ""),
                    "http_status": nav_status,
                    "blocked_reason": nav_blocked,
                    "title": d.get("title", ""),
                    "dom_nodes": d.get("stats", {}).get("dom_node_count", 0),
                    "first_paint_ms": d.get("first_paint_ms", 0.0),
                    "total_requests": len(traces),
                    "png_path": d.get("_png_path", ""),
                })
                print(f"  [LIVE] {name:<8} ({mode:<11}) | status={nav_status} dom={d.get('stats', {}).get('dom_node_count', 0)} title={d.get('title', '')[:48]}")
            except Exception as exc:
                live_results.append({
                    "site": name,
                    "mode": mode,
                    "requested_url": url,
                    "error": str(exc),
                })
                print(f"  [LIVE] {name:<8} ({mode:<11}) | error={exc}")
    return live_results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./build/waterscape")
    ap.add_argument("--module-dir", default="./build")
    ap.add_argument("--out-dir", default="/tmp/arcr_verify_2026")
    ap.add_argument("--benchmarks", action="store_true")
    ap.add_argument("--live-sites", action="store_true")
    ap.add_argument("--report-json", default="")
    args = ap.parse_args()

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    os.makedirs(args.out_dir, exist_ok=True)
    cert_dir = os.path.join(args.out_dir, "certs")
    os.makedirs(cert_dir, exist_ok=True)
    manifest_path = os.path.join(args.out_dir, "server_manifest.json")

    srv_proc, info = start_fixture_server(repo_root, cert_dir, manifest_path)
    try:
        print("=== Running Arcradient 2026 Verification Matrix (N01..R01) ===")
        matrix = run_fixture_matrix(args.binary, args.module_dir, info, args.out_dir)
        passed = sum(1 for r in matrix if r["passed"])
        failed = len(matrix) - passed
        print(f"=== Matrix Summary: {passed}/{len(matrix)} PASSED, {failed} FAILED ===")

        bench = {}
        if args.benchmarks:
            print("=== Running Cold/Warm P50/P95 Benchmarks ===")
            bench = run_perf_benchmarks(args.binary, args.module_dir, info, args.out_dir)

        live = []
        if args.live_sites:
            print("=== Running Honest Live-Site Checks ===")
            live = run_live_site_checks(args.binary, args.module_dir, args.out_dir)

        payload = {
            "total": len(matrix),
            "passed": passed,
            "failed": failed,
            "matrix": matrix,
            "benchmarks": bench,
            "live_sites": live,
        }
        report_path = args.report_json or os.path.join(args.out_dir, "verification_matrix_report.json")
        with open(report_path, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2, ensure_ascii=False)
        print(f"Saved report -> {report_path}")
        return 0 if failed == 0 else 1
    finally:
        srv_proc.terminate()
        try:
            srv_proc.wait(timeout=3.0)
        except Exception:
            srv_proc.kill()


if __name__ == "__main__":
    sys.exit(main())
