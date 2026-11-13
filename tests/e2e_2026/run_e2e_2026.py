#!/usr/bin/env python3
import sys
sys.dont_write_bytecode = True

import argparse
import base64
from concurrent.futures import ThreadPoolExecutor, as_completed
import ctypes
import fnmatch
import gzip
import hashlib
import importlib
import json
import os
import re
import shutil
import socket
import ssl
import struct
import subprocess
import threading
import time
import typing
import urllib.parse
import zlib

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
TESTS_PARENT = os.path.dirname(THIS_DIR)
REPO_ROOT = os.path.dirname(TESTS_PARENT)
if TESTS_PARENT not in sys.path:
    sys.path.insert(0, TESTS_PARENT)

if not hasattr(ssl.SSLContext, "set_alpn_callback"):
    def _compat_set_alpn_callback(self: ssl.SSLContext, cb: typing.Callable[[typing.Any, list[str]], str]) -> None:
        self.set_alpn_protocols(["http/1.1"])

        def _on_sni(ssl_sock: typing.Any, _server_name: typing.Any, _ctx: typing.Any) -> None:
            try:
                cb(ssl_sock, ["http/1.1"])
            except Exception:
                pass

        self.set_servername_callback(_on_sni)

    setattr(ssl.SSLContext, "set_alpn_callback", _compat_set_alpn_callback)

from e2e_2026.harness.fixture_server import FixtureServer, WS_GUID
from e2e_2026.harness.types_and_guards import (
    AUTHORITATIVE_FEATURES,
    AssertionFailure,
    PNGImage,
    RunnerContext,
    TestCaseSpec,
    TestResult,
    ensure,
    read_png_rgba,
    validate_anti_false_positive_guards,
    write_png_rgba,
)

TIER_MODULE_MAP: dict[int, tuple[str, ...]] = {
    1: ("e2e_2026.tiers.tier1_f01_f11", "e2e_2026.tiers.tier1_f12_f19"),
    2: ("e2e_2026.tiers.tier2_f01_f11", "e2e_2026.tiers.tier2_f12_f19"),
    3: ("e2e_2026.tiers.tier3_pairwise",),
    4: ("e2e_2026.tiers.tier4_scenarios",),
}

_X11_SERIAL_LOCK = threading.Lock()


def _load_tests_for_tiers(tiers: list[int], *, lenient_missing: bool = False) -> list[TestCaseSpec]:
    loaded: list[TestCaseSpec] = []
    for t in tiers:
        for mod_path in TIER_MODULE_MAP[t]:
            try:
                mod = importlib.import_module(mod_path)
            except Exception as exc:
                if lenient_missing:
                    continue
                raise RuntimeError(f"Failed to import {mod_path}: {exc}") from exc
            getter = getattr(mod, "get_tests", None)
            items = list(getter()) if callable(getter) else list(getattr(mod, "TESTS", []))
            loaded.extend(items)
    return loaded


def _ws_client_roundtrip(ws_origin: str, path: str, text_msg: str) -> str:
    u = urllib.parse.urlsplit(ws_origin)
    host = u.hostname or "127.0.0.1"
    port = int(u.port or 80)
    raw_key = base64.b64encode(b"arcr_selftest_16").decode("ascii")
    expected_accept = base64.b64encode(hashlib.sha1((raw_key + WS_GUID).encode("ascii")).digest()).decode("ascii")
    with socket.create_connection((host, port), timeout=5.0) as sock:
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {raw_key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        sock.sendall(req.encode("ascii"))
        resp = bytearray()
        while b"\r\n\r\n" not in resp:
            ch = sock.recv(1024)
            if not ch:
                raise RuntimeError("WS handshake closed early")
            resp.extend(ch)
        hdr_txt = bytes(resp).split(b"\r\n\r\n", 1)[0].decode("iso-8859-1")
        if "101 Switching Protocols" not in hdr_txt or expected_accept not in hdr_txt:
            raise RuntimeError(f"WS handshake invalid: {hdr_txt!r}")

        # Send client-masked text frame (opcode=0x1, MASK=1)
        payload = text_msg.encode("utf-8")
        mask = b"\x37\xfa\x21\x3d"
        masked = bytes(payload[i] ^ mask[i & 3] for i in range(len(payload)))
        sock.sendall(bytes([0x81, 0x80 | len(payload)]) + mask + masked)

        b0, b1 = sock.recv(2)
        plen = b1 & 0x7F
        echo = sock.recv(plen).decode("utf-8")

        # Send masked Close (1000)
        close_pl = struct.pack(">H", 1000)
        masked_cl = bytes(close_pl[i] ^ mask[i & 3] for i in range(2))
        sock.sendall(bytes([0x88, 0x82]) + mask + masked_cl)
        return echo


def _http_get_raw(origin: str, path: str, *, headers: typing.Optional[dict[str, str]] = None, ca_cert: typing.Optional[str] = None) -> tuple[int, dict[str, str], bytes]:
    import http.client
    http.client._MAXLINE = 262144
    u = urllib.parse.urlsplit(origin)
    if u.scheme == "https":
        sctx = ssl.create_default_context(cafile=ca_cert)
        conn: http.client.HTTPConnection = http.client.HTTPSConnection(u.hostname or "127.0.0.1", u.port, context=sctx, timeout=8.0)
    else:
        conn = http.client.HTTPConnection(u.hostname or "127.0.0.1", u.port, timeout=8.0)
    try:
        conn.request("GET", path, headers=headers or {})
        resp = conn.getresponse()
        body = resp.read()
        hdrs = {k.lower(): v for k, v in resp.getheaders()}
        return resp.status, hdrs, body
    finally:
        conn.close()


def run_self_test(waterscape_bin: str, arcr_js_bin: str, out_dir: str) -> int:
    checks: list[tuple[str, bool, str]] = []

    def record(name: str, fn: typing.Callable[[], str]) -> None:
        try:
            detail = fn()
            checks.append((name, True, detail))
        except Exception as exc:
            checks.append((name, False, f"{type(exc).__name__}: {exc}"))

    srv_dir = os.path.join(out_dir, "selftest_srv")
    srv = FixtureServer(srv_dir)
    srv.start()
    try:
        record("binaries_present", lambda: (
            ensure(os.path.isfile(waterscape_bin) and os.access(waterscape_bin, os.X_OK), f"Missing {waterscape_bin}"),
            ensure(os.path.isfile(arcr_js_bin) and os.access(arcr_js_bin, os.X_OK), f"Missing {arcr_js_bin}"),
            "waterscape & arcr_js executable",
        )[-1])

        def _chk_http_comp() -> str:
            st, _, b_gz = _http_get_raw(srv.main_origin, "/compress/gzip")
            ensure(st == 200 and b"COMP_GZIP_OK" in gzip.decompress(b_gz), "gzip failed")
            st, _, b_df = _http_get_raw(srv.main_origin, "/compress/deflate")
            ensure(st == 200 and b"COMP_DEFLATE_OK" in zlib.decompress(b_df), "deflate failed")
            st, _, b_br = _http_get_raw(srv.main_origin, "/compress/br")
            dec_br = subprocess.run(["/usr/bin/brotli", "-d", "-c"], input=b_br, capture_output=True, check=True).stdout
            ensure(st == 200 and b"COMP_BR_OK" in dec_br, "brotli failed")
            return f"gzip={len(b_gz)}B deflate={len(b_df)}B br={len(b_br)}B"

        record("fixture_http_compression", _chk_http_comp)

        def _chk_long_url_64kb() -> str:
            q = "k=" + ("A" * 65600)
            exp_sha = hashlib.sha256(q.encode("utf-8")).hexdigest()
            st, _, body = _http_get_raw(srv.main_origin, f"/long-echo?{q}")
            obj = json.loads(body.decode("utf-8"))
            ensure(st == 200 and obj["query_len"] == len(q) and obj["query_sha256"] == exp_sha, f"64KB URL mismatch: {obj}")
            return f"query_len={obj['query_len']} sha256={exp_sha[:12]}"

        record("fixture_long_url_64kb", _chk_long_url_64kb)

        def _chk_cookies_charsets_sse_https() -> str:
            st, _, cb = _http_get_raw(srv.main_origin, "/cookie/echo", headers={"Cookie": "a=1; b=2; c=3"})
            ensure(st == 200 and json.loads(cb)["count"] == 3, "cookie echo failed")
            for enc in ("euc-kr", "shift_jis", "euc-jp", "gb18030", "big5", "windows-1252"):
                st_c, _, bb = _http_get_raw(srv.main_origin, f"/charset-bytes/{enc}")
                cj = json.loads(bb)
                ensure(st_c == 200 and len(cj["bytes"]) > 4, f"charset {enc} failed")
            st_s, hdr_s, b_sse = _http_get_raw(srv.main_origin, "/sse-stream")
            ensure(st_s == 200 and "text/event-stream" in hdr_s.get("content-type", "") and b"data: done" in b_sse, "SSE failed")
            st_h, _, _ = _http_get_raw(srv.https_origin, "/healthz", ca_cert=srv.ca_cert)
            ensure(st_h == 200, "HTTPS healthz failed")
            return "cookies(3), 6 charsets, SSE, TLS OK"

        record("fixture_cookies_charsets_sse_tls", _chk_cookies_charsets_sse_https)

        def _chk_ws_and_assets() -> str:
            echo = _ws_client_roundtrip(srv.ws_origin, "/ws-echo", "RFC6455_OK")
            time.sleep(0.03)
            wst = srv.get_ws_traces()
            ensure(echo == "RFC6455_OK" and wst and wst[-1]["masked_all"] is True, f"WS failed: {echo}, {wst}")
            _, _, w2 = _http_get_raw(srv.main_origin, "/fonts/mono.woff2")
            _, _, avif = _http_get_raw(srv.main_origin, "/img/green64.avif")
            _, _, mp4 = _http_get_raw(srv.main_origin, "/media/sample.mp4")
            ensure(w2[:4] == b"wOF2" and b"avif" in avif[:16] and b"ftyp" in mp4[:16], "WOFF2/AVIF/MP4 magic mismatch")
            ft = ctypes.CDLL("libfreetype.so.6")
            lib_p = ctypes.c_void_p()
            face_p = ctypes.c_void_p()
            ensure(ft.FT_Init_FreeType(ctypes.byref(lib_p)) == 0, "FT_Init_FreeType failed")
            try:
                buf = ctypes.create_string_buffer(w2, len(w2))
                rc = ft.FT_New_Memory_Face(lib_p, buf, ctypes.c_long(len(w2)), ctypes.c_long(0), ctypes.byref(face_p))
                ensure(rc == 0, f"FreeType rejected mono.woff2 rc={rc}")
            finally:
                if face_p.value:
                    ft.FT_Done_Face(face_p)
                if lib_p.value:
                    ft.FT_Done_FreeType(lib_p)
            return f"WS={echo}, woff2={len(w2)}B(FT_OK), avif={len(avif)}B, mp4={len(mp4)}B"

        record("fixture_ws_woff2_avif_mp4", _chk_ws_and_assets)

        def _chk_png_and_guards() -> str:
            # 1. PNG round-trip, contrast, AA levels, tofu positive & negative
            w, h = 64, 48
            rgba = bytearray([255, 255, 255, 255] * (w * h))
            # Draw 14x16 4-wall tofu box at (6, 8)
            for x in range(6, 20):
                for y in (8, 23):
                    off = (y * w + x) * 4
                    rgba[off : off + 3] = b"\x00\x00\x00"
            for y in range(8, 24):
                for x in (6, 19):
                    off = (y * w + x) * 4
                    rgba[off : off + 3] = b"\x00\x00\x00"
            # Draw open 'L' glyph at (34..46, 8..24) (not a 4-walled tofu box!) + colorful patch at (48..62, 8..28)
            for y in range(8, 24):
                for x in (34, 35):
                    off = (y * w + x) * 4
                    rgba[off : off + 3] = b"\x10\x10\x10"
            for x in range(34, 46):
                for y in (22, 23):
                    off = (y * w + x) * 4
                    rgba[off : off + 3] = b"\x10\x10\x10"
            for x in range(48, 62):
                for y in range(8, 28):
                    off = (y * w + x) * 4
                    rgba[off : off + 3] = bytes([30, 180, 70])
            for idx in range(12):
                v = 20 + idx * 16
                off = (36 * w + 34 + idx) * 4
                rgba[off : off + 3] = bytes([v, v, v])

            p_path = os.path.join(out_dir, "selftest_rgba.png")
            write_png_rgba(p_path, w, h, bytes(rgba))
            img = read_png_rgba(p_path)
            ensure(img.detect_crossed_box_tofu(2, 4, 24, 24) is True, "Failed to detect 4-wall tofu box")
            ensure(img.detect_crossed_box_tofu(32, 4, 15, 24) is False, "False-positive tofu on open 'L' glyph")
            ensure(img.count_grayscale_aa_levels(30, 32, 24, 10) >= 10, "AA level counter failed")
            ensure(img.region_max_contrast(4, 4, 24, 24) >= 15.0, "WCAG contrast calc failed")
            ensure(img.is_grey_skeleton_only()[0] is False, "Colorful image misclassified as grey skeleton")

            blank_img = PNGImage(16, 16, bytes([255, 255, 255, 255] * 256))
            grey_buf = bytearray([255, 255, 255, 255] * 256)
            for i in range(60):
                grey_buf[i * 4 : i * 4 + 3] = b"\xd0\xd0\xd0"
            grey_img = PNGImage(16, 16, bytes(grey_buf))
            ensure(blank_img.is_blank_or_uniform()[0] is True, "Blank PNG not caught")
            ensure(grey_img.is_grey_skeleton_only()[0] is True, "Grey-skeleton PNG not caught")

            # 2. Mandatory Anti-False-Positive Guards
            good_dump = {
                "url": "http://127.0.0.1:9999/ok",
                "ready_state": "complete",
                "console_log": "info: ready\n",
                "stats": {"display_cmd_count": 5},
                "traces": [{"initiator": "navigation", "status_code": 200, "final_url": "http://127.0.0.1:9999/ok"}],
            }
            ensure(len(validate_anti_false_positive_guards(returncode=0, timed_out=False, dump_json=good_dump, png_img=img)) == 0, "Clean dump flagged")
            ensure(len(validate_anti_false_positive_guards(returncode=0, timed_out=False, dump_json={**good_dump, "traces": [{"initiator": "navigation", "status_code": 404, "url": "http://x/404"}]}, png_img=img)) > 0, "HTTP 404 not caught")
            ensure(len(validate_anti_false_positive_guards(returncode=0, timed_out=False, dump_json={**good_dump, "url": "https://www.google.com/sorry/index?continue=x"}, png_img=img)) > 0, "/sorry/index not caught")
            ensure(len(validate_anti_false_positive_guards(returncode=0, timed_out=False, dump_json={**good_dump, "console_log": "Uncaught TypeError: x is undefined"}, png_img=img)) > 0, "Console TypeError not caught")
            ensure(len(validate_anti_false_positive_guards(returncode=0, timed_out=False, dump_json={**good_dump, "ready_state": "loading"}, png_img=img)) > 0, "Stuck ready_state='loading' not caught")
            ensure(len(validate_anti_false_positive_guards(returncode=0, timed_out=False, dump_json=good_dump, png_img=blank_img)) > 0, "Blank PNG guard not caught")
            return "PNG RGBA, tofu, AA, contrast & all 6 anti-false-positive guards verified"

        record("png_and_anti_false_positive_guards", _chk_png_and_guards)

        def _chk_tier_discovery() -> str:
            t1 = _load_tests_for_tiers([1], lenient_missing=False)
            t2 = _load_tests_for_tiers([2], lenient_missing=False)
            t3 = _load_tests_for_tiers([3], lenient_missing=False)
            t4 = _load_tests_for_tiers([4], lenient_missing=False)
            all_specs = [*t1, *t2, *t3, *t4]
            uniq_ids = {s.test_id for s in all_specs}
            ensure(len(t1) >= 95 and len(t2) >= 95 and len(t3) >= 25 and len(t4) >= 10, f"Tier counts low: T1={len(t1)} T2={len(t2)} T3={len(t3)} T4={len(t4)}")
            ensure(len(uniq_ids) == len(all_specs) and len(uniq_ids) >= 225, f"Duplicate or <225 test IDs: uniq={len(uniq_ids)} total={len(all_specs)}")
            for fid in AUTHORITATIVE_FEATURES:
                c1 = sum(1 for s in t1 if s.feature_id == fid)
                c2 = sum(1 for s in t2 if s.feature_id == fid)
                ensure(c1 >= 5 and c2 >= 5, f"Feature {fid} under-covered: T1={c1}, T2={c2}")
            return f"T1={len(t1)}, T2={len(t2)}, T3={len(t3)}, T4={len(t4)}, total_unique={len(uniq_ids)}"

        record("tier_module_discovery", _chk_tier_discovery)
    finally:
        srv.stop()

    print("=== Arcradient E2E 2026 Harness Self-Test ===")
    failed = 0
    for name, ok, detail in checks:
        tag = "PASS" if ok else "FAIL"
        if not ok:
            failed += 1
        print(f"  [{tag}] {name}: {detail}")
    print(f"Self-test summary: {len(checks) - failed}/{len(checks)} passed")
    return 0 if failed == 0 else 2


def _needs_x11_lock(spec: TestCaseSpec) -> bool:
    tid = spec.test_id.lower()
    desc = spec.description.lower()
    feats = {spec.feature_id, *spec.related_features}
    return ("x11" in tid or "xvfb" in desc or "x11" in desc or "F16" in feats or "F17" in feats)


def _execute_one_test(spec: TestCaseSpec, ctx: RunnerContext) -> TestResult:
    feats = list(spec.related_features)
    if ctx.skip_live and getattr(spec, "is_live", False):
        return TestResult(
            test_id=spec.test_id,
            tier=spec.tier,
            feature_id=spec.feature_id,
            passed=True,
            skipped=True,
            diagnostic="Skipped (--skip-live)",
            related_features=feats,
        )

    def _invoke() -> TestResult:
        t0 = time.perf_counter()
        try:
            ret = spec.fn(ctx)
            dt_ms = (time.perf_counter() - t0) * 1000.0
            if isinstance(ret, TestResult):
                if ret.duration_ms <= 0.0:
                    ret.duration_ms = dt_ms
                if not ret.related_features and feats:
                    ret.related_features = feats
                return ret
            if isinstance(ret, tuple) and len(ret) >= 2:
                ok_b, diag = bool(ret[0]), str(ret[1])
                return TestResult(spec.test_id, spec.tier, spec.feature_id, ok_b, diag, duration_ms=dt_ms, related_features=feats)
            if isinstance(ret, bool):
                return TestResult(spec.test_id, spec.tier, spec.feature_id, ret, "" if ret else "Test returned False", duration_ms=dt_ms, related_features=feats)
            return TestResult(spec.test_id, spec.tier, spec.feature_id, True, "OK", duration_ms=dt_ms, related_features=feats)
        except (AssertionFailure, AssertionError) as af:
            dt_ms = (time.perf_counter() - t0) * 1000.0
            return TestResult(spec.test_id, spec.tier, spec.feature_id, False, str(af), duration_ms=dt_ms, related_features=feats)
        except Exception as exc:
            dt_ms = (time.perf_counter() - t0) * 1000.0
            return TestResult(
                spec.test_id,
                spec.tier,
                spec.feature_id,
                False,
                f"Unhandled {type(exc).__name__}: {exc}",
                duration_ms=dt_ms,
                related_features=feats,
                details={"harness_error": True},
            )

    if _needs_x11_lock(spec):
        with _X11_SERIAL_LOCK:
            return _invoke()
    return _invoke()


def _matches_test_id(test_id: str, pattern: str) -> bool:
    if pattern in test_id or fnmatch.fnmatchcase(test_id, pattern):
        return True
    try:
        return re.search(pattern, test_id) is not None
    except re.error:
        return False


def _build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Arcradient / Waterscape 2026 Opaque-Box E2E Test Runner")
    p.add_argument("--binary", default=os.path.join(REPO_ROOT, "build", "waterscape"))
    p.add_argument("--js-binary", default=os.path.join(REPO_ROOT, "build", "arcr_js"))
    p.add_argument("--module-dir", default=None)
    p.add_argument("--tier", choices=("1", "2", "3", "4", "all"), default="all")
    p.add_argument("--feature", default=None, help="Comma-separated feature IDs e.g. F01,F07")
    p.add_argument("--test-id", default=None, help="Substring, glob, or regex filter on test_id")
    p.add_argument("--out-dir", default=None)
    p.add_argument("--json-report", default=None)
    p.add_argument("--jobs", type=int, default=1)
    p.add_argument("--skip-live", action="store_true")
    p.add_argument("--keep-artifacts", action="store_true")
    p.add_argument("--self-test", action="store_true")
    return p


def main(argv: typing.Optional[list[str]] = None) -> int:
    parser = _build_arg_parser()
    args = parser.parse_args(argv)

    if args.jobs < 1:
        print("ERROR: --jobs must be >= 1", file=sys.stderr)
        return 2

    waterscape_bin = os.path.abspath(args.binary)
    arcr_js_bin = os.path.abspath(args.js_binary)
    module_dir = os.path.abspath(args.module_dir) if args.module_dir else os.path.dirname(waterscape_bin)

    if not (os.path.isfile(waterscape_bin) and os.access(waterscape_bin, os.X_OK)):
        print(f"ERROR: Missing or non-executable --binary: {waterscape_bin}", file=sys.stderr)
        return 2
    if not (os.path.isfile(arcr_js_bin) and os.access(arcr_js_bin, os.X_OK)):
        print(f"ERROR: Missing or non-executable --js-binary: {arcr_js_bin}", file=sys.stderr)
        return 2

    out_dir = os.path.abspath(args.out_dir) if args.out_dir else f"/tmp/arcr_e2e_out_{os.getpid()}_{time.monotonic_ns()}"
    # Guard against accidental repo pollution if user passed --out-dir inside repo_root
    os.makedirs(out_dir, exist_ok=True)

    try:
        if args.self_test:
            return run_self_test(waterscape_bin, arcr_js_bin, out_dir)

        selected_tiers = [1, 2, 3, 4] if args.tier == "all" else [int(args.tier)]
        wanted_features: set[str] = set()
        if args.feature:
            for tok in (x.strip().upper() for x in args.feature.split(",") if x.strip()):
                if tok not in AUTHORITATIVE_FEATURES:
                    print(f"ERROR: Unknown feature ID {tok!r}; valid: F01..F19", file=sys.stderr)
                    return 2
                wanted_features.add(tok)

        try:
            specs = _load_tests_for_tiers(selected_tiers, lenient_missing=False)
        except Exception as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 2

        if wanted_features:
            specs = [
                s for s in specs
                if s.feature_id in wanted_features or any(rf in wanted_features for rf in s.related_features)
            ]
        if args.test_id:
            specs = [s for s in specs if _matches_test_id(s.test_id, args.test_id)]

        if not specs:
            print("ERROR: Zero test cases matched the requested filter", file=sys.stderr)
            return 2

        srv_assets = os.path.join(out_dir, "_fixture_assets")
        srv = FixtureServer(srv_assets)
        srv.start()
        t_start = time.perf_counter()
        results_by_idx: dict[int, TestResult] = {}
        try:
            ctx = RunnerContext(
                waterscape_bin=waterscape_bin,
                arcr_js_bin=arcr_js_bin,
                module_dir=module_dir,
                repo_root=REPO_ROOT,
                out_dir=out_dir,
                skip_live=bool(args.skip_live),
                fixture_srv=srv,
            )
            if args.jobs == 1:
                for idx, sp in enumerate(specs):
                    res = _execute_one_test(sp, ctx)
                    results_by_idx[idx] = res
                    tag = "SKIP" if res.skipped else ("PASS" if res.passed else "FAIL")
                    msg = f" — {res.diagnostic[:160]}" if (not res.passed or res.skipped) and res.diagnostic else ""
                    print(f"[{tag}] T{res.tier} {res.feature_id} {res.test_id} ({res.duration_ms:.0f}ms){msg}", flush=True)
            else:
                with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                    fut_map = {pool.submit(_execute_one_test, sp, ctx): idx for idx, sp in enumerate(specs)}
                    for fut in as_completed(fut_map):
                        idx = fut_map[fut]
                        res = fut.result()
                        results_by_idx[idx] = res
                        tag = "SKIP" if res.skipped else ("PASS" if res.passed else "FAIL")
                        msg = f" — {res.diagnostic[:160]}" if (not res.passed or res.skipped) and res.diagnostic else ""
                        print(f"[{tag}] T{res.tier} {res.feature_id} {res.test_id} ({res.duration_ms:.0f}ms){msg}", flush=True)
        finally:
            srv.stop()

        ordered_results = [results_by_idx[i] for i in range(len(specs))]
        wall_ms = (time.perf_counter() - t_start) * 1000.0

        passed_n = sum(1 for r in ordered_results if r.passed and not r.skipped)
        skipped_n = sum(1 for r in ordered_results if r.skipped)
        failed_n = sum(1 for r in ordered_results if not r.passed and not r.skipped)
        harness_err_n = sum(1 for r in ordered_results if r.details.get("harness_error"))

        per_tier: dict[str, dict[str, int]] = {}
        for t in (1, 2, 3, 4):
            tr = [r for r in ordered_results if r.tier == t]
            if tr:
                per_tier[f"tier_{t}"] = {
                    "total": len(tr),
                    "passed": sum(1 for r in tr if r.passed and not r.skipped),
                    "failed": sum(1 for r in tr if not r.passed and not r.skipped),
                    "skipped": sum(1 for r in tr if r.skipped),
                }

        per_feature: dict[str, dict[str, typing.Any]] = {}
        for fid, fmeta in AUTHORITATIVE_FEATURES.items():
            fr = [r for r in ordered_results if r.feature_id == fid or fid in r.related_features]
            if fr:
                per_feature[fid] = {
                    "milestone": fmeta["milestone"],
                    "name": fmeta["name"],
                    "total": len(fr),
                    "passed": sum(1 for r in fr if r.passed and not r.skipped),
                    "failed": sum(1 for r in fr if not r.passed and not r.skipped),
                    "skipped": sum(1 for r in fr if r.skipped),
                }

        print("\n=== E2E 2026 Summary ===")
        for tk, tv in per_tier.items():
            print(f"  {tk}: passed={tv['passed']} failed={tv['failed']} skipped={tv['skipped']} total={tv['total']}")
        print(f"  TOTAL: passed={passed_n} failed={failed_n} skipped={skipped_n} total={len(ordered_results)} ({wall_ms:.0f}ms)")

        if args.json_report:
            rep_path = os.path.abspath(args.json_report)
            os.makedirs(os.path.dirname(rep_path), exist_ok=True)
            report_payload = {
                "summary": {
                    "total": len(ordered_results),
                    "passed": passed_n,
                    "failed": failed_n,
                    "skipped": skipped_n,
                    "harness_errors": harness_err_n,
                    "wall_duration_ms": round(wall_ms, 2),
                },
                "per_tier": per_tier,
                "per_feature": per_feature,
                "results": [
                    {
                        "test_id": r.test_id,
                        "tier": r.tier,
                        "feature_id": r.feature_id,
                        "related_features": r.related_features,
                        "passed": r.passed,
                        "skipped": r.skipped,
                        "duration_ms": round(r.duration_ms, 2),
                        "diagnostic": r.diagnostic,
                    }
                    for r in ordered_results
                ],
            }
            with open(rep_path, "w", encoding="utf-8") as rf:
                json.dump(report_payload, rf, indent=2, ensure_ascii=False)

        return 0 if (failed_n == 0 and harness_err_n == 0) else 1
    finally:
        if not args.keep_artifacts and os.path.exists(out_dir) and out_dir.startswith("/tmp/"):
            shutil.rmtree(out_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
