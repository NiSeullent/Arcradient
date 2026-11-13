import sys
sys.dont_write_bytecode = True

import hashlib
import json
import os
import re
import shlex
import shutil
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
    ensure,
    run_arcr_js,
    run_waterscape_verify,
)

TestCase = TestCaseSpec
TestContext = RunnerContext

ENV_SETUP_PATH = "/home/antigravity/env-setup.sh"
REAL_BASHRC_PATH = "/home/antigravity/.bashrc"


def _env_json_path(ctx: RunnerContext) -> str:
    cand = os.path.join(ctx.repo_root, ".agents", "environment.json")
    if os.path.exists(cand):
        return cand
    return "/home/antigravity/arcradient/.agents/environment.json"


def _ensure_fixture(ctx: RunnerContext) -> tuple[FixtureServer, bool]:
    if ctx.fixture_srv is not None:
        return ctx.fixture_srv, False
    srv = FixtureServer(os.path.join(ctx.out_dir, "_tier1_f01_f11_assets"))
    srv.start()
    return srv, True


def _fnv1a32(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def _spec(
    test_id: str,
    feature_id: str,
    description: str,
    body_fn: typing.Callable[[RunnerContext], typing.Union[str, TestResult, None]],
    *,
    related_features: tuple[str, ...] = (),
) -> TestCaseSpec:
    def _wrapped(ctx: RunnerContext) -> TestResult:
        t0 = time.perf_counter()
        try:
            out = body_fn(ctx)
            dt = (time.perf_counter() - t0) * 1000.0
            if isinstance(out, TestResult):
                if out.duration_ms <= 0.0:
                    out.duration_ms = dt
                return out
            diag = out if isinstance(out, str) else "OK"
            return TestResult(
                test_id=test_id,
                tier=1,
                feature_id=feature_id,
                passed=True,
                diagnostic=diag,
                duration_ms=dt,
                related_features=list(related_features),
            )
        except AssertionFailure as exc:
            dt = (time.perf_counter() - t0) * 1000.0
            return TestResult(
                test_id=test_id,
                tier=1,
                feature_id=feature_id,
                passed=False,
                diagnostic=str(exc),
                duration_ms=dt,
                related_features=list(related_features),
            )

    tc = TestCaseSpec(
        test_id=test_id,
        tier=1,
        feature_id=feature_id,
        description=description,
        fn=_wrapped,
        related_features=related_features,
        is_live=False,
    )
    setattr(tc, "feature", feature_id)
    return tc


def _uncommented_lines(script_text: str) -> list[str]:
    out: list[str] = []
    for raw in script_text.splitlines():
        s = raw.strip()
        if not s or s.startswith("#"):
            continue
        out.append(s)
    return out


# =============================================================================
# Feature F01: Cloud /env-setup Automation (Tier 1: T1_F01_01..T1_F01_05)
# =============================================================================

def _t1_f01_01(ctx: RunnerContext) -> str:
    ensure(os.path.isfile(ENV_SETUP_PATH), f"Missing {ENV_SETUP_PATH}")
    raw = open(ENV_SETUP_PATH, "r", encoding="utf-8", errors="replace").read()
    ensure(len(raw.encode("utf-8")) >= 120, f"env-setup.sh too short ({len(raw)} bytes)")
    lines = raw.splitlines()
    ensure(len(lines) >= 3 and lines[0].strip() == "#!/usr/bin/env bash", "Line 1 must be #!/usr/bin/env bash")
    active = _uncommented_lines(raw)
    ensure(
        any(re.search(r"\bset\s+-euo\s+pipefail\b", ln) for ln in active),
        "Active 'set -euo pipefail' missing in env-setup.sh",
    )
    joined_active = "\n".join(active)
    for kw in ("cmake", "python3"):
        ensure(kw in joined_active, f"env-setup.sh missing toolchain check for {kw!r}")
    ensure(
        any(c in joined_active for c in ("gcc", "clang", "cc")),
        "env-setup.sh missing C compiler verification",
    )
    proc = subprocess.run(["bash", "-n", ENV_SETUP_PATH], capture_output=True, text=True, timeout=10)
    ensure(proc.returncode == 0 and proc.stderr.strip() == "", f"bash -n failed: {proc.stderr.strip()}")
    return "env-setup.sh shebang, strict mode, syntax, and toolchain checks verified"


def _t1_f01_02(ctx: RunnerContext) -> str:
    ej_path = _env_json_path(ctx)
    ensure(os.path.isfile(ej_path), f"Missing {ej_path}")
    try:
        data = json.loads(open(ej_path, "r", encoding="utf-8").read())
    except Exception as exc:
        raise AssertionFailure(f"Invalid JSON in {ej_path}: {exc}")
    ensure(isinstance(data, dict), ".agents/environment.json root must be a JSON object")
    install_cmd = data.get("install")
    ensure(isinstance(install_cmd, str) and len(install_cmd.strip()) >= 40, "Invalid or stub 'install' command")
    for kw in ("cmake", "python3", "PATH"):
        ensure(kw in install_cmd, f"environment.json['install'] missing {kw!r}")
    proc = subprocess.run(["bash", "-n", "-c", install_cmd], capture_output=True, text=True, timeout=10)
    ensure(proc.returncode == 0 and proc.stderr.strip() == "", f"bash -n on install failed: {proc.stderr.strip()}")
    return "environment.json parsed and bash -n verified"


def _t1_f01_03(ctx: RunnerContext) -> str:
    ensure(os.path.isfile(ENV_SETUP_PATH), f"Missing {ENV_SETUP_PATH}")
    raw = open(ENV_SETUP_PATH, "r", encoding="utf-8", errors="replace").read()
    active = "\n".join(_uncommented_lines(raw))
    ensure("sed" in active and "case" in active and "esac" in active, "Missing sed strip for case $- in .. esac")
    ensure(
        re.search(r"\bgrep\s+-[A-Za-z]*q[A-Za-z]*F", active) is not None,
        "Missing idempotent grep -qF / -qxF guard before appending PATH to .bashrc",
    )
    ensure(os.path.isfile(REAL_BASHRC_PATH), f"Missing {REAL_BASHRC_PATH}")
    env = os.environ.copy()
    env["BASH_ENV"] = REAL_BASHRC_PATH
    probe = subprocess.run(
        [
            "bash",
            "-c",
            'command -v cmake && command -v python3 && (command -v ninja || command -v make) && printf "PATH=%s\\n" "$PATH"',
        ],
        env=env,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=10,
    )
    ensure(probe.returncode == 0, f"BASH_ENV subshell tool probe failed: {probe.stderr.strip()}")
    ensure(
        ".local/bin" in probe.stdout or "/opt/antigravity/bin" in probe.stdout,
        f"Expected user/plugin bin path in BASH_ENV PATH, got: {probe.stdout.strip()}",
    )
    return "Non-interactive BASH_ENV PATH & guard removal verified"


def _t1_f01_04(ctx: RunnerContext) -> str:
    ensure(os.path.isfile(ENV_SETUP_PATH), f"Missing {ENV_SETUP_PATH}")
    ej_path = _env_json_path(ctx)
    ensure(os.path.isfile(ej_path), f"Missing {ej_path}")
    raw_sh = "\n".join(_uncommented_lines(open(ENV_SETUP_PATH, "r", encoding="utf-8").read()))
    try:
        ej = json.loads(open(ej_path, "r", encoding="utf-8").read())
    except Exception as exc:
        raise AssertionFailure(f"Corrupt environment.json: {exc}")
    install_sh = str(ej.get("install", ""))

    for label, text in (("env-setup.sh", raw_sh), ("environment.json", install_sh)):
        # Split shell segments on ; && || | and newlines
        segs = [s.strip() for s in re.split(r"(?:&&|\|\||;|\||\n)", text) if s.strip()]
        mkdir_seen = 0
        for seg in segs:
            toks = seg.split()
            if not toks:
                continue
            # Strip leading [ ... ] or if/then/do keywords
            while toks and toks[0] in ("then", "do", "else", "{"):
                toks = toks[1:]
            if not toks:
                continue
            if toks[0] == "mkdir":
                mkdir_seen += 1
                flags = [t for t in toks[1:] if t.startswith("-")]
                has_p = any("p" in f.lstrip("-") for f in flags if not f.startswith("--")) or ("--parents" in flags)
                ensure(has_p, f"{label}: bare mkdir without -p in segment: {seg!r}")
            if toks[0] == "ln":
                flags = "".join(t.lstrip("-") for t in toks[1:] if t.startswith("-") and not t.startswith("--"))
                if "s" in flags:
                    ensure("f" in flags, f"{label}: ln -s without -f wake-safety in segment: {seg!r}")
        ensure(mkdir_seen >= 1, f"{label}: expected at least one mkdir -p guard")
    return "Wake-persistence guards (mkdir -p, ln -sf/-sfn) verified across env-setup.sh & environment.json"


def _t1_f01_05(ctx: RunnerContext) -> str:
    ensure(os.path.isfile(ENV_SETUP_PATH), f"Missing {ENV_SETUP_PATH}")
    ej_path = _env_json_path(ctx)
    ensure(os.path.isfile(ej_path), f"Missing {ej_path}")
    try:
        install_cmd = json.loads(open(ej_path, "r", encoding="utf-8").read())["install"]
    except Exception as exc:
        raise AssertionFailure(f"Invalid environment.json: {exc}")

    tdir = ctx.test_dir("T1_F01_05")
    iso_home = os.path.join(tdir, "iso_home")
    os.makedirs(iso_home, exist_ok=True)
    if os.path.isfile(REAL_BASHRC_PATH):
        shutil.copy2(REAL_BASHRC_PATH, os.path.join(iso_home, ".bashrc"))
    # Symlink repo so $HOME/arcradient resolves cleanly inside isolated HOME without mutating real HOME
    os.symlink(ctx.repo_root, os.path.join(iso_home, "arcradient"))

    def git_status() -> str:
        p = subprocess.run(
            ["git", "-C", ctx.repo_root, "status", "--porcelain"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        lines = [
            ln
            for ln in p.stdout.splitlines()
            if ".agents/environment.json" not in ln and "tests/e2e_2026/" not in ln
        ]
        return "\n".join(lines)

    before_git = git_status()
    env = os.environ.copy()
    env["HOME"] = iso_home

    r1 = subprocess.run(["bash", ENV_SETUP_PATH], env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
    ensure(r1.returncode == 0, f"env-setup.sh run 1 failed ({r1.returncode}): {r1.stderr[:240]}")
    brc_path = os.path.join(iso_home, ".bashrc")
    ensure(os.path.isfile(brc_path), "iso_home/.bashrc missing after run 1")
    h1 = hashlib.sha256(open(brc_path, "rb").read()).hexdigest()

    r2 = subprocess.run(["bash", ENV_SETUP_PATH], env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
    ensure(r2.returncode == 0, f"env-setup.sh run 2 failed ({r2.returncode}): {r2.stderr[:240]}")
    h2 = hashlib.sha256(open(brc_path, "rb").read()).hexdigest()
    ensure(h1 == h2, f".bashrc mutated on second run (sha256 {h1} != {h2})")

    r3 = subprocess.run(["bash", "-e", "-c", install_cmd], cwd=ctx.repo_root, env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
    ensure(r3.returncode == 0, f"environment.json install run 1 failed ({r3.returncode}): {r3.stderr[:240]}")
    h3 = hashlib.sha256(open(brc_path, "rb").read()).hexdigest()

    r4 = subprocess.run(["bash", "-e", "-c", install_cmd], cwd=ctx.repo_root, env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
    ensure(r4.returncode == 0, f"environment.json install run 2 failed ({r4.returncode}): {r4.stderr[:240]}")
    h4 = hashlib.sha256(open(brc_path, "rb").read()).hexdigest()
    ensure(h3 == h4, f".bashrc mutated across environment.json double-run ({h3} != {h4})")

    after_git = git_status()
    ensure(before_git == after_git, f"Repository working tree polluted by env-setup:\n{after_git}")
    shutil.rmtree(iso_home, ignore_errors=True)
    return "4/4 back-to-back runs exited 0 with identical .bashrc SHA-256 and zero repo pollution"


# =============================================================================
# Feature F02: ArcradientJS O(1) Mega-Bundle Compiler & Slot Scaling (Tier 1)
# =============================================================================

def _t1_f02_01(ctx: RunnerContext) -> str:
    tdir = ctx.test_dir("T1_F02_01")
    js_path = os.path.join(tdir, "f02_consts_50k.js")
    str_elems = ",".join(f'"k_{i:05d}"' for i in range(50000))
    num_elems = ",".join(f"{100000 + i}.5" for i in range(50000))
    script = (
        f"const s = [{str_elems}];\n"
        f"const n = [{num_elems}];\n"
        "let sum = 0;\n"
        "for (let i = 0; i < n.length; i++) sum += n[i];\n"
        "print([s[0], s[32768], s[49999], sum].join(','));\n"
    )
    with open(js_path, "w", encoding="utf-8") as f:
        f.write(script)
    res = run_arcr_js(ctx, script_path=js_path, heap_limit_mb=256, timeout_sec=8.0)
    ensure(res.returncode == 0, f"arcr_js failed (rc={res.returncode}, dt={res.duration_ms:.0f}ms): {res.stderr.strip()[:240]}")
    ensure(res.duration_ms < 3500.0, f"50k constants took {res.duration_ms:.1f}ms (expected < 3500ms)")
    got = res.stdout.strip()
    ensure(got == "k_00000,k_32768,k_49999,6250000000", f"Unexpected output: {got!r}")
    return f"50k string + 50k numeric constants verified in {res.duration_ms:.1f}ms"


def _t1_f02_02(ctx: RunnerContext) -> str:
    tdir = ctx.test_dir("T1_F02_02")
    js_path = os.path.join(tdir, "f02_fn_locals_35k.js")
    decls = "\n".join(f"let v{i} = {i + 1};" for i in range(35000))
    script = (
        "function mega() {\n"
        f"{decls}\n"
        "v0 += 7; v32767 += 11; v32768 += 13; v32769 += 17; v34999 += 19;\n"
        "return [v0, v32767, v32768, v32769, v34999, v0 + v17500 + v32768 + v34999].join(',');\n"
        "}\n"
        "print(mega());\n"
    )
    with open(js_path, "w", encoding="utf-8") as f:
        f.write(script)
    res = run_arcr_js(ctx, script_path=js_path, heap_limit_mb=256, timeout_sec=8.0)
    ensure(res.returncode == 0, f"35k function locals failed (rc={res.returncode}): {res.stderr.strip()[:240]}")
    ensure(res.duration_ms < 3500.0, f"35k function locals took {res.duration_ms:.1f}ms (expected < 3500ms)")
    got = res.stdout.strip()
    ensure(got == "8,32779,32782,32787,35019,85310", f"Unexpected slot values: {got!r}")
    return f"35,000 function locals scaled past uint16 wrap in {res.duration_ms:.1f}ms"


def _t1_f02_03(ctx: RunnerContext) -> str:
    tdir = ctx.test_dir("T1_F02_03")
    mjs_path = os.path.join(tdir, "f02_top_35k.mjs")
    decls = "\n".join(f"const c_{i} = {i * 3 + 1};" for i in range(35000))
    script = f"{decls}\nexport const probe = [c_0, c_32767, c_32768, c_34999];\nprint(probe.join(':'));\n"
    with open(mjs_path, "w", encoding="utf-8") as f:
        f.write(script)
    res = run_arcr_js(ctx, script_path=mjs_path, module=True, heap_limit_mb=256, timeout_sec=8.0)
    ensure(res.returncode == 0, f"35k module top-level consts failed (rc={res.returncode}): {res.stderr.strip()[:240]}")
    ensure(res.duration_ms < 3500.0, f"35k module bindings took {res.duration_ms:.1f}ms")
    got = res.stdout.strip()
    ensure(got == "1:98302:98305:104998", f"Unexpected module probe output: {got!r}")
    return f"35,000 ESM top-level lexical bindings verified in {res.duration_ms:.1f}ms"


def _t1_f02_04(ctx: RunnerContext) -> str:
    tdir = ctx.test_dir("T1_F02_04")
    js_path = os.path.join(tdir, "f02_nested_160.js")
    depth = 160
    lines = ["function runNested() {", "  const fns = [];"]
    for d in range(depth):
        lines.append(f"  {{ let x = {d}; let s_{d} = {d * 2}; fns.push(() => x + s_{d}); s_{d} += 3;")
    lines.append("  let tail = s_0 + s_64 + s_127 + s_159;")
    for _ in range(depth):
        lines.append("  }")
    lines.append("  let total = 0; for (let i = 0; i < fns.length; i++) total += fns[i]();")
    lines.append("  return total + ':' + fns[0]() + ':' + fns[159]();")
    lines.append("}")
    lines.append("print(runNested());")
    with open(js_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    expected_total = sum(d + (d * 2 + 3) for d in range(depth))
    expected = f"{expected_total}:3:{159 + 159 * 2 + 3}"
    res = run_arcr_js(ctx, script_path=js_path, heap_limit_mb=128, timeout_sec=8.0)
    ensure(res.returncode == 0, f"160-level nested scope failed (rc={res.returncode}): {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == expected, f"Expected {expected!r}, got {res.stdout.strip()!r}")
    return "160-level nested lexical & captured closure upvalue resolution verified"


def _t1_f02_05(ctx: RunnerContext) -> str:
    tdir = ctx.test_dir("T1_F02_05")

    def write_bench(n: int, fname: str) -> tuple[str, str]:
        p = os.path.join(tdir, fname)
        body = "\n".join(f'let b_{i} = "s_{i}";' for i in range(n))
        code = f"function run() {{\n{body}\nreturn b_0 + '|' + b_{n // 2} + '|' + b_{n - 1};\n}}\nprint(run());\n"
        with open(p, "w", encoding="utf-8") as f:
            f.write(code)
        return p, f"s_0|s_{n // 2}|s_{n - 1}"

    p10, exp10 = write_bench(10000, "bench_10k.js")
    p40, exp40 = write_bench(40000, "bench_40k.js")

    t10_runs: list[float] = []
    t40_runs: list[float] = []
    for _ in range(3):
        r10 = run_arcr_js(ctx, script_path=p10, heap_limit_mb=256, timeout_sec=8.0)
        ensure(r10.returncode == 0 and r10.stdout.strip() == exp10, f"bench_10k failed: {r10.stderr[:200]}")
        t10_runs.append(r10.duration_ms)

        r40 = run_arcr_js(ctx, script_path=p40, heap_limit_mb=256, timeout_sec=8.0)
        ensure(r40.returncode == 0 and r40.stdout.strip() == exp40, f"bench_40k failed: {r40.stderr[:200]}")
        t40_runs.append(r40.duration_ms)

    med10 = sorted(t10_runs)[1]
    med40 = sorted(t40_runs)[1]
    ratio = med40 / max(med10, 15.0)
    ensure(med40 < 2500.0, f"bench_40k too slow: {med40:.1f}ms (expected < 2500ms)")
    ensure(ratio < 7.5, f"Super-linear O(N^2) scaling detected: T(40k)={med40:.1f}ms, T(10k)={med10:.1f}ms, ratio={ratio:.2f}x >= 7.5x")
    return f"O(1) scaling verified: T(10k)={med10:.1f}ms, T(40k)={med40:.1f}ms, ratio={ratio:.2f}x"


# =============================================================================
# Feature F03: ArcradientJS Bytecode & GC Mega-Bundle Throughput (Tier 1)
# =============================================================================

def _build_realistic_bundle(num_classes: int, num_consts: int, num_regs: int, pad_target_bytes: int) -> tuple[str, int]:
    parts: list[str] = ["(function(){\n'use strict';\nconst REG = [];\n"]
    expected_acc = 0
    for c in range(num_classes):
        v = (c * 17 + 5) & 0xFFFF
        expected_acc = (expected_acc + v + (c & 7)) & 0x7FFFFFFF
        base_cls = f" extends C_{c - 1}" if (c > 0 and c % 5 != 0) else ""
        parts.append(
            f"class C_{c}{base_cls} {{ #id = {v}; m{c}(k) {{ const fn = (d) => this.#id + k + d; return fn({c & 7}); }} }}\n"
            f"REG.push(new C_{c}().m{c}(0));\n"
        )
    for k in range(num_consts):
        s_val = f"bundle_const_{k:05d}_payload_segment_alpha"
        parts.append(f'const K_{k} = "{s_val}";\n')
    for r in range(num_regs):
        parts.append(f"let L_{r} = {r & 1023};\n")
    cur_len = sum(len(x) for x in parts)
    idx = 0
    while cur_len < pad_target_bytes:
        filler = f'function helper_mod_{idx}(a) {{ const tag = "mod_chunk_{idx:05d}_" + "x".repeat(4); return a + {idx & 31} + tag.length * 0; }}\n'
        parts.append(filler)
        cur_len += len(filler)
        idx += 1
    parts.append(
        "let acc = 0; for (let i = 0; i < REG.length; i++) acc = (acc + REG[i]) & 0x7fffffff;\n"
        f"if (K_0.length === 0 || K_{num_consts - 1}.length === 0 || L_{num_regs - 1} < 0) acc = -1;\n"
        "print('BUNDLE_DIGEST:' + acc + ':' + REG.length);\n})();\n"
    )
    return "".join(parts), expected_acc


def _t1_f03_01(ctx: RunnerContext) -> str:
    tdir = ctx.test_dir("T1_F03_01")
    js_path = os.path.join(tdir, "f03_bundle_8mb.js")
    src, exp_acc = _build_realistic_bundle(1500, 52000, 34000, 8_400_000)
    with open(js_path, "w", encoding="utf-8") as f:
        f.write(src)
    sz_mb = os.path.getsize(js_path) / (1024 * 1024)
    ensure(sz_mb >= 8.0, f"Generated bundle too small: {sz_mb:.2f} MB")
    res = run_arcr_js(ctx, script_path=js_path, heap_limit_mb=256, timeout_sec=12.0)
    ensure(res.returncode == 0, f"8MB bundle failed (rc={res.returncode}): {res.stderr.strip()[:240]}")
    ensure(res.duration_ms < 5500.0, f"8MB bundle took {res.duration_ms:.1f}ms (expected < 5500ms)")
    expected_out = f"BUNDLE_DIGEST:{exp_acc}:1500"
    ensure(res.stdout.strip() == expected_out, f"Expected {expected_out!r}, got {res.stdout.strip()!r}")
    return f"{sz_mb:.2f} MB bundle compiled & executed in {res.duration_ms:.1f}ms"


def _t1_f03_02(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        chunks = ["(function(){\nlet count = 0;\n"]
        for i in range(800):
            chunks.append(
                f"function Comp_{i}() {{ const label = 'component_payload_{i:04d}_' + 'z'; return label.length > 0 ? 1 : 0; }}\n"
                f"count += Comp_{i}();\n"
            )
        cur = sum(len(c) for c in chunks)
        j = 0
        while cur < 4_200_000:
            line = f'const B4_PAD_{j} = "bundle4mb_segment_{j:06d}_abcdefghijklmnopqrstuvwxyz0123456789";\n'
            chunks.append(line)
            cur += len(line)
            j += 1
        chunks.append(
            "const box = document.getElementById('app');\n"
            "box.textContent = 'READY_' + count;\n"
            "document.title = 'BUNDLE_4MB_DOM_OK:' + count;\n})();\n"
        )
        bundle_bytes = "".join(chunks).encode("utf-8")
        srv.register_route(
            "/f03/bundle-4mb.js",
            {"status": 200, "content_type": "application/javascript; charset=utf-8", "body": bundle_bytes},
        )
        page_url = srv.register_route(
            "/f03/page-4mb",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": (
                    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>LOADING</title></head>"
                    "<body><div id='app' style='width:240px;height:80px;background:#2563eb;color:#fff'>INIT</div>"
                    "<script src='/f03/bundle-4mb.js'></script></body></html>"
                ),
            },
        )
        wres = run_waterscape_verify(ctx, page_url, test_id="T1_F03_02", wait_load_ms=8000, timeout_sec=20.0)
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "BUNDLE_4MB_DOM_OK:800", f"Unexpected title: {wres.dump_json.get('title')!r}")
        b_traces = [t for t in (wres.dump_json.get("traces") or []) if "/f03/bundle-4mb.js" in str(t.get("url", ""))]
        ensure(len(b_traces) >= 1 and int(b_traces[0].get("decoded_bytes", 0)) >= 4_000_000, f"Missing 4MB trace: {b_traces}")
        return f"4.1 MB HTTP script executed cleanly in browser ({len(bundle_bytes)} bytes)"
    finally:
        if owns:
            srv.stop()


def _t1_f03_03(ctx: RunnerContext) -> str:
    code = r"""
    const live = new Map();
    for (let s = 0; s < 500; s++) {
        live.set(s, { id: s, tag: "sentinel_" + s, sum: s * 3 + 1 });
    }
    for (let iter = 0; iter < 60; iter++) {
        const temp = [];
        for (let j = 0; j < 2048; j++) {
            temp.push({ k: iter + ":" + j, payload: "x".repeat(1600) + iter });
        }
        if (temp.length !== 2048) throw new Error("bad temp");
    }
    let ok = true;
    let total = 0;
    for (let s = 0; s < 500; s++) {
        const obj = live.get(s);
        if (!obj || obj.id !== s || obj.tag !== "sentinel_" + s) { ok = false; break; }
        total += obj.sum;
    }
    if (ok && live.size === 500 && total === 374750) {
        print("CHURN_SURVIVED:500:CHECKSUM_OK");
    } else {
        print("CHURN_BAD:" + ok + ":" + total);
    }
    """
    res = run_arcr_js(ctx, code=code, heap_limit_mb=64, timeout_sec=15.0)
    ensure(res.returncode == 0, f"GC churn failed under 64MB cap (rc={res.returncode}): {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "CHURN_SURVIVED:500:CHECKSUM_OK", f"Unexpected output: {res.stdout.strip()!r}")
    return "240 MB cumulative churn survived inside 64 MB heap with 500 live sentinels intact"


def _t1_f03_04(ctx: RunnerContext) -> str:
    code = r"""
    const cleaned = [];
    const fr = new FinalizationRegistry((held) => { cleaned.push(held); });
    const liveObj = { role: "live" };
    const wrLive = new WeakRef(liveObj);
    fr.register(liveObj, "live-token");
    let wrDead;
    (function makeDead() {
        let deadObj = { role: "dead", pad: "y".repeat(512) };
        wrDead = new WeakRef(deadObj);
        fr.register(deadObj, "dead-token");
        deadObj = null;
    })();
    Promise.resolve().then(() => {
        if (typeof $262 !== "undefined" && typeof $262.gc === "function") $262.gc();
        for (let i = 0; i < 4000; i++) ({ g: i });
        if (typeof $262 !== "undefined" && typeof $262.gc === "function") $262.gc();
        Promise.resolve().then(() => {
            const liveOk = wrLive.deref() === liveObj;
            const deadCleared = wrDead.deref() === undefined;
            const hasDead = cleaned.includes("dead-token");
            const noLive = !cleaned.includes("live-token");
            print([liveOk, deadCleared, hasDead, noLive].join(","));
        });
    });
    """
    res = run_arcr_js(ctx, code=code, test262=True, timeout_sec=10.0)
    ensure(res.returncode == 0, f"WeakRef/FinalizationRegistry failed (rc={res.returncode}): {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "true,true,true,true", f"Expected 'true,true,true,true', got {res.stdout.strip()!r}")
    return "WeakRef deref/clearing and FinalizationRegistry callback verified"


def _t1_f03_05(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        cursor = srv.mark_cursor()
        srv.register_route(
            "/f03/esm/a.mjs",
            {
                "status": 200,
                "content_type": "application/javascript; charset=utf-8",
                "body": (
                    "import { bVal } from '/f03/esm/b.mjs';\n"
                    "export let aVal = 'A_INIT';\n"
                    "aVal = 'A_READY';\n"
                    "const lazyMod = await import('@app/lazy');\n"
                    "window.__esmResult = aVal + '|' + bVal + '|' + lazyMod.lazyTag;\n"
                    "document.title = (window.__esmResult === 'A_READY|B_TLA_OK:A_INIT|LAZY_OK') ? 'ESM_CYCLE_TLA_DYN_OK' : ('BAD:' + window.__esmResult);\n"
                ),
            },
        )
        srv.register_route(
            "/f03/esm/b.mjs",
            {
                "status": 200,
                "content_type": "application/javascript; charset=utf-8",
                "body": (
                    "import * as nsA from '@app/core';\n"
                    "let observedEarly = 'ERR';\n"
                    "try { observedEarly = nsA.aVal; } catch (e) { observedEarly = 'TDZ'; }\n"
                    "for (let i = 0; i < 6000; i++) { const tmp = { idx: i, s: 'p'.repeat(1200) }; if (!tmp) break; }\n"
                    "const awaited = await Promise.resolve('B_TLA_OK');\n"
                    "export const bVal = awaited + ':' + (observedEarly === 'TDZ' ? 'A_INIT' : observedEarly);\n"
                ),
            },
        )
        srv.register_route(
            "/f03/esm/lazy.mjs",
            {
                "status": 200,
                "content_type": "application/javascript; charset=utf-8",
                "body": "export const lazyTag = 'LAZY_OK';\n",
            },
        )
        page_url = srv.register_route(
            "/f03/esm/index.html",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": (
                    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>INIT</title>"
                    "<script type='importmap'>{\"imports\":{\"@app/core\":\"/f03/esm/a.mjs\",\"@app/lazy\":\"/f03/esm/lazy.mjs\"}}</script>"
                    "</head><body><div style='width:180px;height:60px;background:#0ea5e9;color:#fff'>ESM</div>"
                    "<script type='module'>import '@app/core';</script></body></html>"
                ),
            },
        )
        wres = run_waterscape_verify(ctx, page_url, test_id="T1_F03_05", wait_load_ms=5000)
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "ESM_CYCLE_TLA_DYN_OK", f"Unexpected title: {wres.dump_json.get('title')!r}")
        a_hits = srv.get_traces(since=cursor, path_prefix="/f03/esm/a.mjs")
        b_hits = srv.get_traces(since=cursor, path_prefix="/f03/esm/b.mjs")
        ensure(len(a_hits) == 1 and len(b_hits) == 1, f"Expected 1 fetch each for cyclic modules, got a={len(a_hits)}, b={len(b_hits)}")
        return "Cyclic ESM + Top-Level await + dynamic import() + importmap under GC pressure verified"
    finally:
        if owns:
            srv.stop()


# =============================================================================
# Feature F04: ES2026 Explicit Resource Management (Tier 1)
# =============================================================================

def _t1_f04_01(ctx: RunnerContext) -> str:
    code = r"""
    const d1 = Object.getOwnPropertyDescriptor(Symbol, "dispose");
    const d2 = Object.getOwnPropertyDescriptor(Symbol, "asyncDispose");
    const descOk = typeof Symbol.dispose === "symbol" && typeof Symbol.asyncDispose === "symbol"
        && d1 && !d1.writable && !d1.enumerable && !d1.configurable
        && d2 && !d2.writable && !d2.enumerable && !d2.configurable;
    const log = [];
    const mk = (id) => ({ [Symbol.dispose]() { log.push("dispose:" + id); } });
    {
        using a = mk("A");
        using b = mk("B");
        using c = mk("C");
        log.push("BODY");
    }
    const log2 = [];
    function earlyExit() {
        using outer = { [Symbol.dispose]() { log2.push("outer"); } };
        for (let i = 0; i < 3; i++) {
            using inner = { [Symbol.dispose]() { log2.push("inner:" + i); } };
            if (i === 1) return "RET";
        }
    }
    const r = earlyExit();
    print([descOk, log.join(","), r, log2.join(",")].join("|"));
    """
    res = run_arcr_js(ctx, code=code)
    ensure(res.returncode == 0, f"using/Symbol.dispose failed (rc={res.returncode}): {res.stderr.strip()[:240]}")
    expected = "true|BODY,dispose:C,dispose:B,dispose:A|RET|inner:0,inner:1,outer"
    ensure(res.stdout.strip() == expected, f"Expected {expected!r}, got {res.stdout.strip()!r}")
    return "Symbol.dispose/asyncDispose descriptors and LIFO block/return unwinding verified"


def _t1_f04_02(ctx: RunnerContext) -> str:
    code = r"""
    const log = [];
    const tick = () => new Promise((r) => Promise.resolve().then(r));
    {
        await using r1 = { async [Symbol.asyncDispose]() { await tick(); log.push("async:1"); } };
        await using r2 = { [Symbol.dispose]() { log.push("syncFallback:2"); } };
        await using r3 = { [Symbol.asyncDispose]() { return tick().then(() => { log.push("async:3"); }); } };
        log.push("body");
    }
    print(log.join(","));
    """
    res = run_arcr_js(ctx, code=code, module=True)
    ensure(res.returncode == 0, f"await using failed (rc={res.returncode}): {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "body,async:3,syncFallback:2,async:1", f"Unexpected order: {res.stdout.strip()!r}")
    return "await using sequential async LIFO disposal and sync fallback verified"


def _t1_f04_03(ctx: RunnerContext) -> str:
    code = r"""
    const log = [];
    const s = new DisposableStack();
    const tagOk = s[Symbol.toStringTag] === "DisposableStack" && s.dispose === s[Symbol.dispose];
    const r1 = { id: 1, [Symbol.dispose]() { log.push("use:1"); } };
    const uRet = s.use(r1);
    const aRet = s.adopt(42, (v) => log.push("adopt:" + v));
    s.defer(() => log.push("defer"));
    const s2 = s.move();
    const stateAfterMove = [s.disposed, s2.disposed, log.length, uRet === r1, aRet === 42].join(",");
    {
        using _ = s2;
    }
    const afterBlock = [s2.disposed, log.join(",")].join("|");
    s2.dispose(); // idempotent second dispose
    print([tagOk, stateAfterMove, afterBlock, log.length].join("||"));
    """
    res = run_arcr_js(ctx, code=code)
    ensure(res.returncode == 0, f"DisposableStack failed (rc={res.returncode}): {res.stderr.strip()[:240]}")
    expected = "true||true,false,0,true,true||true|defer,adopt:42,use:1||3"
    ensure(res.stdout.strip() == expected, f"Expected {expected!r}, got {res.stdout.strip()!r}")
    return "DisposableStack use/adopt/defer/move/dispose lifecycle verified"


def _t1_f04_04(ctx: RunnerContext) -> str:
    code = r"""
    const log = [];
    const as = new AsyncDisposableStack();
    const tagOk = as[Symbol.toStringTag] === "AsyncDisposableStack" && as.disposeAsync === as[Symbol.asyncDispose];
    as.use({ async [Symbol.asyncDispose]() { await Promise.resolve(); log.push("useAsync"); } });
    as.adopt("V", async (v) => { await Promise.resolve(); log.push("adoptAsync:" + v); });
    as.defer(async () => { await Promise.resolve(); log.push("deferAsync"); });
    const as2 = as.move();
    const movedOk = as.disposed === true && as2.disposed === false && log.length === 0;
    await as2.disposeAsync();
    await as2.disposeAsync();
    print([tagOk, movedOk, as2.disposed, log.join(",")].join("|"));
    """
    res = run_arcr_js(ctx, code=code, module=True)
    ensure(res.returncode == 0, f"AsyncDisposableStack failed (rc={res.returncode}): {res.stderr.strip()[:240]}")
    expected = "true|true|true|deferAsync,adoptAsync:V,useAsync"
    ensure(res.stdout.strip() == expected, f"Expected {expected!r}, got {res.stdout.strip()!r}")
    return "AsyncDisposableStack full async lifecycle verified"


def _t1_f04_05(ctx: RunnerContext) -> str:
    js_body = r"""
    const log = [];
    const items = [1, 2, 3].map((id) => ({ id, [Symbol.dispose]() { log.push("disp:" + id); } }));
    for (using item of items) {
        log.push("iter:" + item.id);
    }
    const using = 10;
    const obj = { using: 20 };
    function f(using) { return using + obj.using; }
    let x = 0;
    let usingVal = 1;
    let u2
    x = 5;
    const summary = log.join(",") + "|" + (using + f(7) + x);
    """
    r_cli = run_arcr_js(ctx, code=js_body + "\nprint(summary);")
    ensure(r_cli.returncode == 0, f"arcr_js for(using..of) failed: {r_cli.stderr.strip()[:240]}")
    expected = "iter:1,disp:1,iter:2,disp:2,iter:3,disp:3|42"
    ensure(r_cli.stdout.strip() == expected, f"Expected {expected!r}, got {r_cli.stdout.strip()!r}")

    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f04/contextual-using",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": (
                    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>INIT</title></head>"
                    "<body><div style='width:140px;height:50px;background:#16a34a;color:#fff'>F04</div>"
                    f"<script>{js_body}\ndocument.title = summary;</script></body></html>"
                ),
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F04_05")
        ensure(wres.ok, f"Browser contextual using guard failure: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == expected, f"Browser title mismatch: {wres.dump_json.get('title')!r}")
        return "for (using x of iter) per-iteration disposal & contextual keyword verified in CLI + browser"
    finally:
        if owns:
            srv.stop()


# =============================================================================
# Feature F05: ES2026 Built-ins & ECMA-402 13th Ed. Completion (Tier 1)
# =============================================================================

def _t1_f05_01(ctx: RunnerContext) -> str:
    code = r"""
    function* gen() { yield 1e100; yield 1e-100; yield -1e100; }
    const a = Math.sumPrecise([1e20, 1, -1e20]) === 1;
    const b = Math.sumPrecise([1e308, 1, -1e308]) === 1;
    const c = Math.sumPrecise(new Set([0.1, 0.2])) === (0.1 + 0.2);
    const d = Math.sumPrecise(gen()) === 1e-100;
    const meta = Math.sumPrecise.length === 1 && Math.sumPrecise.name === "sumPrecise";
    print([a, b, c, d, meta].join(","));
    """
    res = run_arcr_js(ctx, code=code)
    ensure(res.returncode == 0, f"Math.sumPrecise failed (rc={res.returncode}): {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "true,true,true,true,true", f"Got: {res.stdout.strip()!r}")
    return "Math.sumPrecise exact Shewchuk cancellation & iterable protocol verified"


def _t1_f05_02(ctx: RunnerContext) -> str:
    code = r"""
    const r = [
        Object.is(Math.sumPrecise([]), -0),
        Object.is(Math.sumPrecise([-0]), -0),
        Object.is(Math.sumPrecise([-0, -0]), -0),
        Object.is(Math.sumPrecise([0]), +0),
        Object.is(Math.sumPrecise([-0, 0]), +0),
        Object.is(Math.sumPrecise([1, -1]), +0),
    ];
    print(r.join(","));
    """
    res = run_arcr_js(ctx, code=code)
    ensure(res.returncode == 0, f"Math.sumPrecise signed-zero failed: {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "true,true,true,true,true,true", f"Got: {res.stdout.strip()!r}")
    return "Math.sumPrecise -0/+0 state machine verified"


def _t1_f05_03(ctx: RunnerContext) -> str:
    code = r"""
    const m = new Map();
    const v1 = m.getOrInsert("k1", 101);
    const v2 = m.getOrInsert("k1", 999);
    m.set("undefKey", undefined);
    let calls = 0;
    let seenKey = null;
    let seenThis = "NOT_SET";
    const vUndef = m.getOrInsertComputed("undefKey", function(k) { "use strict"; calls++; return 777; });
    const vComp = m.getOrInsertComputed("compKey", function(k) {
        "use strict";
        calls++;
        seenKey = k;
        seenThis = this;
        return "computed:" + k;
    });
    const mZero = new Map();
    mZero.getOrInsert(-0, "zeroVal");
    const canonZero = Object.is([...mZero.keys()][0], +0) && mZero.get(+0) === "zeroVal";
    print([v1 === 101, v2 === 101, vUndef === undefined, calls === 1, vComp === "computed:compKey", seenKey === "compKey", seenThis === undefined, canonZero].join(","));
    """
    res = run_arcr_js(ctx, code=code)
    ensure(res.returncode == 0, f"Map.getOrInsert* failed: {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "true,true,true,true,true,true,true,true", f"Got: {res.stdout.strip()!r}")
    return "Map.prototype.getOrInsert & getOrInsertComputed verified (including undefined value & -0 key)"


def _t1_f05_04(ctx: RunnerContext) -> str:
    code = r"""
    const wm = new WeakMap();
    const objKey = {};
    const symKey = Symbol("wk");
    const a1 = wm.getOrInsert(objKey, "objV1");
    const a2 = wm.getOrInsert(objKey, "objV2");
    const s1 = wm.getOrInsert(symKey, "symV1");
    let calls = 0;
    const s2 = wm.getOrInsertComputed(symKey, () => { calls++; return "never"; });
    const reKey = {};
    const reVal = wm.getOrInsertComputed(reKey, (k) => {
        calls++;
        wm.set(k, "inner");
        return "outer";
    });
    print([a1 === "objV1", a2 === "objV1", s1 === "symV1", s2 === "symV1", calls === 1, reVal === "outer", wm.get(reKey) === "outer"].join(","));
    """
    res = run_arcr_js(ctx, code=code)
    ensure(res.returncode == 0, f"WeakMap.getOrInsert* failed: {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "true,true,true,true,true,true,true", f"Got: {res.stdout.strip()!r}")
    return "WeakMap.prototype.getOrInsert & getOrInsertComputed verified with Object & Symbol keys"


def _t1_f05_05(ctx: RunnerContext) -> str:
    js_body = r"""
    const dfDig = new Intl.DurationFormat("en-US", { style: "digital" });
    const dig = dfDig.format({ hours: 1, minutes: 5, seconds: 9 });
    const digIso = dfDig.format("PT1H5M9S");
    const dfEnLong = new Intl.DurationFormat("en-US", { style: "long" });
    const enLong = dfEnLong.format({ hours: 1, minutes: 46, seconds: 40 });
    const dfKo = new Intl.DurationFormat("ko-KR", { style: "long" });
    const koLong = dfKo.format({ hours: 2, minutes: 3, seconds: 4 });
    const parts = dfEnLong.formatToParts({ hours: 1, minutes: 46, seconds: 40 });
    const joinedParts = parts.map((p) => p.value).join("");
    const hasUnit = parts.some((p) => p.unit === "hour" || p.unit === "hours");
    const ro = dfDig.resolvedOptions();
    const ok = (dig === "1:05:09")
        && (digIso === dig)
        && enLong.includes("1 hour") && enLong.includes("46 minutes") && enLong.includes("40 seconds")
        && (koLong.includes("시간") && koLong.includes("분") && koLong.includes("초"))
        && (joinedParts === enLong) && hasUnit && (ro.style === "digital");
    const out = ok ? "DURATION_FORMAT_OK" : ("BAD:" + JSON.stringify({ dig, digIso, enLong, koLong, joinedParts, ro }));
    """
    r_cli = run_arcr_js(ctx, code=js_body + "\nprint(out);")
    ensure(r_cli.returncode == 0, f"Intl.DurationFormat CLI failed: {r_cli.stderr.strip()[:240]}")
    ensure(r_cli.stdout.strip() == "DURATION_FORMAT_OK", f"CLI got: {r_cli.stdout.strip()!r}")

    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f05/duration",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": (
                    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>INIT</title></head>"
                    "<body><div style='width:140px;height:50px;background:#0284c7;color:#fff'>F05</div>"
                    f"<script>{js_body}\ndocument.title = out;</script></body></html>"
                ),
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F05_05")
        ensure(wres.ok, f"Browser guard failure: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "DURATION_FORMAT_OK", f"Browser title: {wres.dump_json.get('title')!r}")
        return "Intl.DurationFormat digital/long/ko-KR/ISO/formatToParts verified in CLI + browser"
    finally:
        if owns:
            srv.stop()


# =============================================================================
# Feature F06: Long URL Support (>= 32 KB) (Tier 1)
# =============================================================================

def _make_exact_url(origin: str, path: str, target_total_len: int, seed_char: str = "a") -> tuple[str, str]:
    prefix = f"{origin}{path}?q="
    rem = target_total_len - len(prefix)
    assert rem > 16
    alphabet = "abcdefghijklmnopqrstuvwxyz0123456789"
    q_val = "".join(alphabet[(i + ord(seed_char)) % len(alphabet)] for i in range(rem))
    query = "q=" + q_val
    full_url = f"{origin}{path}?{query}"
    assert len(full_url) == target_total_len
    return full_url, hashlib.sha256(query.encode("utf-8")).hexdigest()


def _t1_f06_01(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        for idx, target_len in enumerate((8192, 16384, 32768)):
            url, q_sha = _make_exact_url(srv.main_origin, "/long-nav", target_len, chr(ord("a") + idx))
            wres = run_waterscape_verify(ctx, url, test_id=f"T1_F06_01_{target_len}")
            ensure(wres.ok, f"{target_len}B nav guard failure: {wres.guard_failures}")
            exp_title = f"LONG_NAV:{target_len}:{target_len}:{q_sha[:16]}"
            ensure(
                wres.dump_json.get("title") == exp_title,
                f"{target_len}B title mismatch: expected {exp_title!r}, got {wres.dump_json.get('title')!r}",
            )
            ensure(
                len(str(wres.dump_json.get("url", ""))) == target_len,
                f"dump_json['url'] truncated: len={len(str(wres.dump_json.get('url', '')))} != {target_len}",
            )
        return "Top-level navigation with 8KB, 16KB, and 32KB URLs preserved exact length & SHA-256"
    finally:
        if owns:
            srv.stop()


def _t1_f06_02(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        q_body = "k=xjs.s.ko.2026&m=" + ("cdos_module_" * 1025)[:12270]
        assert len(q_body) == 12288
        exp_sha = hashlib.sha256(q_body.encode("utf-8")).hexdigest()[:16]

        def xjs_handler(req: dict[str, typing.Any]) -> dict[str, typing.Any]:
            q = req["query"]
            got_sha = hashlib.sha256(q.encode("utf-8")).hexdigest()[:16]
            if len(q) != 12288 or got_sha != exp_sha:
                return {"status": 404, "content_type": "text/plain", "body": f"truncated:{len(q)}"}
            return {
                "status": 200,
                "content_type": "application/javascript; charset=utf-8",
                "body": f"window.__xjsLoaded = '{got_sha}'; document.title = 'XJS_12KB_OK:{got_sha}';\n",
            }

        srv.register_route("/xjs/_/js/bundle", xjs_handler)
        page_url = srv.register_route(
            "/f06/xjs-page",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": (
                    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>INIT</title></head>"
                    "<body><div style='width:160px;height:60px;background:#2563eb;color:#fff'>XJS</div>"
                    f"<script src='/xjs/_/js/bundle?{q_body}'></script></body></html>"
                ),
            },
        )
        wres = run_waterscape_verify(ctx, page_url, test_id="T1_F06_02")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == f"XJS_12KB_OK:{exp_sha}", f"Got title: {wres.dump_json.get('title')!r}")
        return f"12KB <script src='/xjs/...'> fetched & executed with intact SHA-256 ({exp_sha})"
    finally:
        if owns:
            srv.stop()


def _t1_f06_03(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        q_css = "v=" + ("styletoken2026_" * 820)[:12286]
        assert len(q_css) == 12288
        exp_sha = hashlib.sha256(q_css.encode("utf-8")).hexdigest()[:16]

        def css_handler(req: dict[str, typing.Any]) -> dict[str, typing.Any]:
            q = req["query"]
            if len(q) != 12288 or hashlib.sha256(q.encode("utf-8")).hexdigest()[:16] != exp_sha:
                return {"status": 404, "content_type": "text/plain", "body": "truncated css url"}
            return {
                "status": 200,
                "content_type": "text/css; charset=utf-8",
                "body": "body { margin: 0; background: #ffffff; } #target { width: 345px; height: 120px; background-color: rgb(12, 200, 90); }\n",
            }

        srv.register_route("/f06/long-css", css_handler)
        page_url = srv.register_route(
            "/f06/css-page",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": (
                    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>INIT</title>"
                    f"<link rel='stylesheet' href='/f06/long-css?{q_css}'></head>"
                    "<body><div id='target'></div>"
                    "<script>window.addEventListener('load', () => {"
                    "  const w = Math.round(document.getElementById('target').getBoundingClientRect().width);"
                    "  document.title = 'CSS_12KB_W:' + w;"
                    "});</script></body></html>"
                ),
            },
        )
        wres = run_waterscape_verify(ctx, page_url, test_id="T1_F06_03", save_content_png=True)
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "CSS_12KB_W:345", f"Expected width 345, got {wres.dump_json.get('title')!r}")
        ensure(wres.content_png is not None, "Missing content PNG")
        green_px = wres.content_png.count_color_near((12, 200, 90), tol=18, rect=(5, 5, 330, 110))
        ensure(green_px >= 5000, f"Expected painted rgb(12,200,90) in #target, found {green_px} px")
        return f"12KB <link rel='stylesheet'> applied to layout (345px) and rasterizer ({green_px} px)"
    finally:
        if owns:
            srv.stop()


def _t1_f06_04(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        q_rel = "r=" + ("rel_hpba_24k_" * 1900)[:24574]
        q_abs = "a=" + ("abs_hpba_24k_" * 1900)[:24574]
        sha_rel = hashlib.sha256(q_rel.encode("utf-8")).hexdigest()
        sha_abs = hashlib.sha256(q_abs.encode("utf-8")).hexdigest()
        page_url = srv.register_route(
            "/f06/fetch-24k",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#7c3aed;color:#fff">FETCH24K</div>
<script>
(async () => {{
  const r1 = await fetch("/async/hpba?{q_rel}");
  const j1 = await r1.json();
  const r2 = await fetch("{srv.main_origin}/async/hpba?{q_abs}");
  const j2 = await r2.json();
  const ok = r1.status === 200 && r2.status === 200
    && r1.url.length >= 24576 && r2.url.length >= 24576
    && j1.query_len === {len(q_rel)} && j1.sha256 === "{sha_rel}"
    && j2.query_len === {len(q_abs)} && j2.sha256 === "{sha_abs}";
  document.title = ok ? "FETCH_24KB_OK" : ("BAD:" + JSON.stringify([j1, j2]));
}})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, page_url, test_id="T1_F06_04", wait_load_ms=5000)
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "FETCH_24KB_OK", f"Got title: {wres.dump_json.get('title')!r}")
        return "24KB relative & absolute fetch('/async/hpba?...') verified with SHA-256 match"
    finally:
        if owns:
            srv.stop()


def _t1_f06_05(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        land_url, land_sha = _make_exact_url(srv.main_origin, "/f06/landed", 20480, "r")
        rel_loc = land_url[len(srv.main_origin) :]
        a_query = "anchor=" + ("link18k_" * 2310)[:18424]
        a_sha = hashlib.sha256(a_query.encode("utf-8")).hexdigest()[:16]

        srv.register_route(
            "/f06/start-redir",
            {"status": 302, "content_type": "text/plain", "body": b"", "headers": {"Location": rel_loc}},
        )

        def landed_handler(req: dict[str, typing.Any]) -> dict[str, typing.Any]:
            got_sha = hashlib.sha256(req["query"].encode("utf-8")).hexdigest()
            return {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><a id="lnk" href="/f06/dest?{a_query}" style="display:block;width:160px;height:50px;background:#059669;color:#fff">LINK</a>
<script>
const u = new URL("/f06/dest?{a_query}", location.href);
const aHref = document.getElementById("lnk").href;
const ok = ("{got_sha}" === "{land_sha}")
  && document.URL.length === 20480
  && u.search.slice(1).length === {len(a_query)}
  && aHref === u.href;
document.title = ok ? "REDIR_20KB_URL_OK:{a_sha}" : ("BAD:" + document.URL.length + ":" + aHref.length);
</script></body></html>""",
            }

        srv.register_route("/f06/landed", landed_handler)
        wres = run_waterscape_verify(ctx, f"{srv.main_origin}/f06/start-redir", test_id="T1_F06_05")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == f"REDIR_20KB_URL_OK:{a_sha}", f"Got: {wres.dump_json.get('title')!r}")
        return "302 redirect to 20KB Location + new URL() + 18KB <a href> verified"
    finally:
        if owns:
            srv.stop()


# =============================================================================
# Feature F07: Unified Cookie Jar & Private Ephemeral Cookies (Tier 1)
# =============================================================================

def _t1_f07_01(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/cookie/set-page",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "headers": [
                    ("Set-Cookie", "theme=dark; Path=/"),
                    ("Set-Cookie", "lang=ko-KR; Path=/cookie"),
                ],
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#2563eb;color:#fff">COOKIE</div>
<script>
const dc = document.cookie;
const parts = dc.split("; ").map(s => s.trim()).filter(Boolean);
const hasTheme = parts.includes("theme=dark");
const hasLang = parts.includes("lang=ko-KR");
const noAttr = !dc.toLowerCase().includes("path=");
document.title = (hasTheme && hasLang && noAttr) ? "DOC_COOKIE_OK:dark:ko-KR" : ("BAD:" + dc);
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F07_01")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "DOC_COOKIE_OK:dark:ko-KR", f"Got: {wres.dump_json.get('title')!r}")
        return "HTTP Set-Cookie -> document.cookie visibility & attribute stripping verified"
    finally:
        if owns:
            srv.stop()


def _t1_f07_02(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        def land_handler(req: dict[str, typing.Any]) -> dict[str, typing.Any]:
            ck = req["headers"].get("cookie", "")
            ok = ("csrf_tok=xyz987" in ck) and ("step=1" in ck) and ("fetch_seen=yes" in ck)
            title = "JS_COOKIE_WIRE_OK" if ok else f"BAD_WIRE:{ck}"
            return {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": f"<!DOCTYPE html><html><head><meta charset='utf-8'><title>{title}</title></head>"
                        "<body><div style='width:160px;height:60px;background:#16a34a;color:#fff'>LAND</div></body></html>",
            }

        srv.register_route("/f07/land", land_handler)
        start_url = srv.register_route(
            "/f07/start",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>START</title></head>
<body><div style="width:160px;height:60px;background:#0284c7;color:#fff">START</div>
<script>
(async () => {
  document.cookie = "csrf_tok=xyz987; Path=/";
  document.cookie = "step=1; Path=/";
  const r = await fetch("/cookie/echo");
  const j = await r.json();
  if (j.cookie.includes("csrf_tok=xyz987") && j.cookie.includes("step=1")) {
    document.cookie = "fetch_seen=yes; Path=/";
  }
  location.href = "/f07/land";
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, start_url, test_id="T1_F07_02", wait_load_ms=4500)
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "JS_COOKIE_WIRE_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "document.cookie -> HTTP Cookie header verified across both fetch() and location.href navigation"
    finally:
        if owns:
            srv.stop()


def _t1_f07_03(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        srv.register_route(
            "/f07/auth/login",
            {
                "status": 302,
                "content_type": "text/plain",
                "body": b"",
                "headers": [
                    ("Set-Cookie", "sid=ephem_42; Path=/; HttpOnly"),
                    ("Set-Cookie", "pub=vis_1; Path=/"),
                    ("Location", "/f07/auth/hop2"),
                ],
            },
        )

        def hop2(req: dict[str, typing.Any]) -> dict[str, typing.Any]:
            ck = req["headers"].get("cookie", "")
            if "sid=ephem_42" not in ck or "pub=vis_1" not in ck:
                return {"status": 403, "content_type": "text/plain", "body": f"missing hop1 cookies: {ck}"}
            return {
                "status": 302,
                "content_type": "text/plain",
                "body": b"",
                "headers": [
                    ("Set-Cookie", "hop2_ok=yes; Path=/"),
                    ("Location", "/f07/auth/dashboard"),
                ],
            }

        def dashboard(req: dict[str, typing.Any]) -> dict[str, typing.Any]:
            ck = req["headers"].get("cookie", "")
            wire_ok = ("sid=ephem_42" in ck) and ("pub=vis_1" in ck) and ("hop2_ok=yes" in ck)
            return {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:180px;height:60px;background:#16a34a;color:#fff">DASH</div>
<script>
const dc = document.cookie;
const jsOk = dc.includes("pub=vis_1") && dc.includes("hop2_ok=yes") && !dc.includes("sid=");
document.title = ({'true' if wire_ok else 'false'} && jsOk) ? "AUTH_REDIRECT_3HOP_OK" : ("BAD:" + dc);
</script></body></html>""",
            }

        srv.register_route("/f07/auth/hop2", hop2)
        srv.register_route("/f07/auth/dashboard", dashboard)
        wres = run_waterscape_verify(ctx, f"{srv.main_origin}/f07/auth/login", test_id="T1_F07_03", private=True)
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "AUTH_REDIRECT_3HOP_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "--private 3-hop 302 redirect ephemeral session cookie retention verified"
    finally:
        if owns:
            srv.stop()


def _t1_f07_04(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        def echo_sub(req: dict[str, typing.Any]) -> dict[str, typing.Any]:
            return {
                "status": 200,
                "content_type": "application/json",
                "body": json.dumps({"cookie": req["headers"].get("cookie", "")}),
            }

        srv.register_route("/admin/echo", echo_sub)
        srv.register_route("/app/echo", echo_sub)
        url = srv.register_route(
            "/app/index.html",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "headers": [
                    ("Set-Cookie", "c_root=1; Path=/"),
                    ("Set-Cookie", "c_admin=secret; Path=/admin"),
                    ("Set-Cookie", "c_temp=bye; Path=/"),
                ],
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#9333ea;color:#fff">PATH</div>
<script>
(async () => {
  const dc1 = document.cookie;
  const rAdmin = await (await fetch("/admin/echo")).json();
  const rApp1 = await (await fetch("/app/echo")).json();
  document.cookie = "c_temp=; Path=/; Max-Age=0";
  const dc2 = document.cookie;
  const rApp2 = await (await fetch("/app/echo")).json();
  const ok = dc1.includes("c_root=1") && dc1.includes("c_temp=bye") && !dc1.includes("c_admin")
    && rAdmin.cookie.includes("c_admin=secret") && rAdmin.cookie.includes("c_root=1")
    && !rApp1.cookie.includes("c_admin")
    && !dc2.includes("c_temp") && !rApp2.cookie.includes("c_temp") && rApp2.cookie.includes("c_root=1");
  document.title = ok ? "PATH_AND_MAXAGE0_OK" : ("BAD:" + JSON.stringify({dc1, dc2, rAdmin, rApp2}));
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F07_04")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "PATH_AND_MAXAGE0_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Cookie Path scoping (/admin vs /app) and Max-Age=0 immediate eviction verified"
    finally:
        if owns:
            srv.stop()


def _t1_f07_05(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        srv.register_route(
            "/f07/bulk-set-2",
            {
                "status": 200,
                "content_type": "application/json",
                "headers": [("Set-Cookie", f"ck_{i:03d}=v{i}; Path=/") for i in range(75, 150)],
                "body": b'{"ok":true}',
            },
        )
        url = srv.register_route(
            "/f07/bulk-300",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "headers": [("Set-Cookie", f"ck_{i:03d}=v{i}; Path=/") for i in range(0, 75)],
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#ea580c;color:#fff">300</div>
<script>
(async () => {
  await fetch("/f07/bulk-set-2");
  for (let i = 150; i < 300; i++) {
    const k = "ck_" + String(i).padStart(3, "0");
    document.cookie = k + "=v" + i + "; Path=/";
  }
  const dc = document.cookie;
  const r = await (await fetch("/cookie/echo")).json();
  const ok = dc.includes("ck_000=v0") && dc.includes("ck_149=v149") && dc.includes("ck_299=v299")
    && r.count >= 300 && r.cookie.includes("ck_000=v0") && r.cookie.includes("ck_299=v299");
  document.title = ok ? ("BULK_300_OK:" + r.count) : ("BAD:" + r.count);
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F07_05", wait_load_ms=5000)
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "BULK_300_OK:300", f"Got: {wres.dump_json.get('title')!r}")
        return "300 concurrent cookies retained in jar & echoed over HTTP without 128-cap eviction"
    finally:
        if owns:
            srv.stop()


# =============================================================================
# Feature F08: Dynamic <script> / <link> Loading & document.currentScript (Tier 1)
# =============================================================================

def _t1_f08_01(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        srv.register_route(
            "/f08/dyn/chunk1.js",
            {
                "status": 200,
                "content_type": "application/javascript; charset=utf-8",
                "body": "window.__chunk1Val = 777; window.__f08Log.push('exec:' + window.__chunk1Val);\n",
            },
        )
        url = srv.register_route(
            "/f08/dyn-script",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#2563eb;color:#fff">DYN_JS</div>
<script>
window.__f08Log = [];
const s = document.createElement("script");
s.src = "/f08/dyn/chunk1.js";
s.onload = () => {
  window.__f08Log.push("onload:" + window.__chunk1Val);
  document.title = (window.__f08Log.join(",") === "exec:777,onload:777") ? "DYN_SCRIPT_OK" : ("BAD:" + window.__f08Log.join(","));
};
document.head.appendChild(s);
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F08_01")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "DYN_SCRIPT_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Dynamic appendChild(<script src>) fetch, execution, and post-exec onload verified"
    finally:
        if owns:
            srv.stop()


def _t1_f08_02(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        srv.register_route(
            "/f08/dyn/theme.css",
            {
                "status": 200,
                "content_type": "text/css; charset=utf-8",
                "body": "body { margin: 0; background: #ffffff; } #box { width: 280px; height: 100px; background-color: rgb(220, 40, 60); }\n",
            },
        )
        url = srv.register_route(
            "/f08/dyn-link",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div id="box"></div>
<script>
const link = document.createElement("link");
link.rel = "stylesheet";
link.href = "/f08/dyn/theme.css";
link.addEventListener("load", () => {
  const w = getComputedStyle(document.getElementById("box")).width;
  document.title = "DYN_CSS_OK:" + w;
});
document.head.insertBefore(link, document.head.firstChild);
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F08_02", save_content_png=True)
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "DYN_CSS_OK:280px", f"Got: {wres.dump_json.get('title')!r}")
        ensure(wres.content_png is not None, "Missing content PNG")
        red_px = wres.content_png.count_color_near((220, 40, 60), tol=18, rect=(5, 5, 260, 90))
        ensure(red_px >= 4000, f"Expected rgb(220,40,60) in #box, found {red_px} px")
        return f"Dynamic insertBefore(<link rel='stylesheet'>) load event, cascade & paint ({red_px} px) verified"
    finally:
        if owns:
            srv.stop()


def _t1_f08_03(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f08/current-inline",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body>
<div id="marker" style="width:160px;height:60px;background:#0d9488;color:#fff">M</div>
<script id="boot" data-cfg="x2026">
const cs = document.currentScript;
window.__csInfo = cs ? [cs.id, cs.getAttribute("data-cfg"), cs.previousElementSibling ? cs.previousElementSibling.id : "NONE"] : ["NULL"];
if (cs) cs.remove();
window.__stillInDom = !!document.getElementById("boot");
</script>
<script>
const afterNull = document.currentScript !== null && document.currentScript.id !== "boot";
window.addEventListener("load", () => {
  const afterLoadCs = document.currentScript;
  const ok = window.__csInfo.join(":") === "boot:x2026:marker"
    && window.__stillInDom === false
    && afterNull === true
    && afterLoadCs === null;
  document.title = ok ? "INLINE_CS_OK" : ("BAD:" + JSON.stringify([window.__csInfo, window.__stillInDom, afterLoadCs]));
});
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F08_03")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "INLINE_CS_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "document.currentScript live DOM node identity & self-remove() verified"
    finally:
        if owns:
            srv.stop()


def _t1_f08_04(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        srv.register_route(
            "/f08/s1.js",
            {
                "status": 200,
                "content_type": "application/javascript; charset=utf-8",
                "body": """
const cs1 = document.currentScript;
window.__ext1 = cs1 ? (cs1.id + "|" + cs1.src.endsWith("/f08/s1.js")) : "NULL";
const s2 = document.createElement("script");
s2.id = "ext2";
s2.src = "/f08/s2.js";
document.body.appendChild(s2);
""",
            },
        )
        srv.register_route(
            "/f08/s2.js",
            {
                "status": 200,
                "content_type": "application/javascript; charset=utf-8",
                "body": """
const cs2 = document.currentScript;
window.__ext2 = cs2 ? (cs2.id + "|" + cs2.src.endsWith("/f08/s2.js")) : "NULL";
setTimeout(() => {
  const postNull = document.currentScript === null;
  document.title = (window.__ext1 === "ext1|true" && window.__ext2 === "ext2|true" && postNull)
    ? "EXT_DYN_CS_OK"
    : ("BAD:" + window.__ext1 + ":" + window.__ext2 + ":" + postNull);
}, 0);
""",
            },
        )
        url = srv.register_route(
            "/f08/ext-cs-page",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#4f46e5;color:#fff">CS</div>
<script id="ext1" src="/f08/s1.js"></script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F08_04")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "EXT_DYN_CS_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "document.currentScript verified in both static external and dynamically appended external scripts"
    finally:
        if owns:
            srv.stop()


def _t1_f08_05(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f08/inline-dyn",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#db2777;color:#fff">SYNC_DYN</div>
<script>
const s = document.createElement("script");
window.__targetS = s;
s.textContent = "window.__inlineDyn = (window.__inlineDyn || 0) + 1; window.__inlineCs = (document.currentScript === window.__targetS);";
document.body.appendChild(s);
const syncAfterAppend = window.__inlineDyn;
s.textContent = "window.__inlineDyn += 100;";
const ok = (syncAfterAppend === 1) && (window.__inlineDyn === 1) && (window.__inlineCs === true);
document.title = ok ? "DYN_INLINE_SYNC_OK" : ("BAD:" + syncAfterAppend + ":" + window.__inlineDyn + ":" + window.__inlineCs);
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F08_05")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "DYN_INLINE_SYNC_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Dynamic inline <script> synchronous execution & 'already started' guard verified"
    finally:
        if owns:
            srv.stop()


# =============================================================================
# Feature F09: Complete Web IDL Globals, Prototypes & Legacy Event Init (Tier 1)
# =============================================================================

def _t1_f09_01(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f09/prototypes",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#2563eb;color:#fff">IDL</div>
<script nomodule>window.__ranNoModule = true;</script>
<script type="module">
window.__ranModule = true;
const sc = document.createElement("script");
const lnk = document.createElement("link");
const sty = document.createElement("style");
const slt = document.createElement("slot");
const ok = ("noModule" in sc)
  && (sc instanceof HTMLScriptElement) && (sc instanceof HTMLElement)
  && (lnk instanceof HTMLLinkElement)
  && (sty instanceof HTMLStyleElement)
  && (slt instanceof HTMLSlotElement)
  && (typeof CDATASection === "function") && (CDATASection.prototype instanceof CharacterData)
  && (typeof ProcessingInstruction === "function") && (ProcessingInstruction.prototype instanceof CharacterData)
  && (window.__ranNoModule === undefined)
  && (window.__ranModule === true);
document.title = ok ? "WEBIDL_PROTOS_OK" : "BAD_PROTOS";
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F09_01")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "WEBIDL_PROTOS_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "HTMLScriptElement.noModule, HTMLLink/Style/SlotElement, CDATASection & ProcessingInstruction verified"
    finally:
        if owns:
            srv.stop()


def _t1_f09_02(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f09/events",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div id="tgt" style="width:160px;height:60px;background:#059669;color:#fff">EV</div>
<script>
const tgt = document.getElementById("tgt");
const ev1 = document.createEvent("Event");
ev1.initEvent("legacy-ev", true, true);
const cev = document.createEvent("CustomEvent");
cev.initCustomEvent("my-ev", true, true, { k: 42 });
let receivedDetail = 0;
tgt.addEventListener("my-ev", (e) => { receivedDetail = e.detail.k; e.preventDefault(); });
tgt.dispatchEvent(cev);
const mev = document.createEvent("MouseEvents");
mev.initMouseEvent("click", true, true, window, 1, 10, 20, 30, 40, false, false, false, false, 0, null);
const pev = new PointerEvent("pointerdown", { pointerId: 7, pointerType: "mouse", clientX: 50, isPrimary: true });
const iev = new InputEvent("beforeinput", { data: "가", inputType: "insertCompositionText", isComposing: true });
const ok = ev1.type === "legacy-ev" && ev1.bubbles && ev1.cancelable
  && receivedDetail === 42 && cev.defaultPrevented === true
  && mev.clientX === 30 && mev.clientY === 40
  && (pev instanceof MouseEvent) && pev.pointerId === 7 && pev.pointerType === "mouse" && pev.isPrimary === true
  && (iev instanceof UIEvent) && iev.data === "가" && iev.inputType === "insertCompositionText" && iev.isComposing === true;
document.title = ok ? "EVENTS_INIT_POINTER_INPUT_OK" : "BAD_EVENTS";
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F09_02")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "EVENTS_INIT_POINTER_INPUT_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "initEvent/initCustomEvent/initMouseEvent, PointerEvent & InputEvent verified"
    finally:
        if owns:
            srv.stop()


def _t1_f09_03(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f09/range-sel",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div id="ed" style="font-size:20px;padding:10px;background:#f1f5f9;color:#0f172a">Hello <b id="btag">Waterscape</b> 2026</div>
<script>
const ed = document.getElementById("ed");
const btag = document.getElementById("btag");
const r = document.createRange();
r.selectNodeContents(btag);
const str1 = r.toString();
const rect = r.getBoundingClientRect();
const sel = window.getSelection();
sel.removeAllRanges();
sel.addRange(r);
const selOk = sel.rangeCount === 1 && sel.getRangeAt(0) === r && sel.toString() === "Waterscape";
const frag = r.cloneContents();
const clonedTxt = frag.textContent;
r.deleteContents();
const span = document.createElement("span");
span.textContent = "Arcradient";
r.insertNode(span);
const finalTxt = ed.textContent.trim();
const ok = str1 === "Waterscape" && rect.width > 0 && selOk && clonedTxt === "Waterscape" && finalTxt === "Hello Arcradient 2026";
document.title = ok ? "RANGE_SELECTION_OK" : ("BAD:" + JSON.stringify({str1, w: rect.width, selOk, clonedTxt, finalTxt}));
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F09_03")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "RANGE_SELECTION_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Range & Selection DOM mutation and getBoundingClientRect verified"
    finally:
        if owns:
            srv.stop()


def _t1_f09_04(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f09/urlpattern",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#7c3aed;color:#fff">URLP</div>
<script>
const p1 = new URLPattern({ pathname: "/watch/:id", search: "t=:sec*" });
const p2 = new URLPattern("https://*.example.com/users/:uid(\\\\d+)");
const t1 = p2.test("https://m.example.com/users/42") === true;
const t2 = p2.test("https://m.example.com/users/abc") === false;
const ex = p1.exec({ pathname: "/watch/dQw4w9WgXcQ", search: "t=30" });
const idOk = ex && ex.pathname && ex.pathname.groups && ex.pathname.groups.id === "dQw4w9WgXcQ";
document.title = (t1 && t2 && idOk) ? "URLPATTERN_OK" : "BAD_URLPATTERN";
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F09_04")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "URLPATTERN_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "URLPattern wildcard subdomain, regex groups, test() and exec() verified"
    finally:
        if owns:
            srv.stop()


def _t1_f09_05(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        ws_before = len(srv.get_ws_traces())
        url = srv.register_route(
            "/f09/ws-sse",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#0284c7;color:#fff">WS_SSE</div>
<script>
(async () => {{
  const wsDone = new Promise((resolve) => {{
    const ws = new WebSocket("{srv.ws_origin}/ws-echo");
    ws.binaryType = "arraybuffer";
    const got = [];
    ws.onopen = () => {{
      ws.send("안녕 WS");
      ws.send(new Uint8Array([0, 255, 128, 42]));
      const big = new Uint8Array(300);
      for (let i = 0; i < 300; i++) big[i] = i & 255;
      ws.send(big);
    }};
    ws.onmessage = (e) => {{
      if (typeof e.data === "string") got.push("str:" + e.data);
      else if (e.data instanceof ArrayBuffer) {{
        const u = new Uint8Array(e.data);
        got.push("bin:" + u.length + ":" + u[0] + ":" + u[u.length - 1]);
      }}
      if (got.length === 3) ws.close(1000, "done");
    }};
    ws.onclose = (ev) => resolve({{ got, wasClean: ev.wasClean, code: ev.code }});
    ws.onerror = () => resolve({{ err: true }});
  }});
  const sseDone = new Promise((resolve) => {{
    const es = new EventSource("/sse-stream");
    let tickEvt = "";
    es.addEventListener("tick", (e) => {{ tickEvt = e.data + "|" + e.lastEventId; }});
    es.onmessage = (e) => {{
      if (e.data === "done") {{
        es.close();
        resolve(tickEvt + "|done");
      }}
    }};
    es.onerror = () => {{ es.close(); resolve("SSE_ERR:" + tickEvt); }};
  }});
  const [wr, sr] = await Promise.all([wsDone, sseDone]);
  const wsOk = wr.got && wr.got.join(",") === "str:안녕 WS,bin:4:0:42,bin:300:0:43" && wr.wasClean === true && wr.code === 1000;
  const sseOk = sr === '{{"n":1}}|9|done';
  document.title = (wsOk && sseOk) ? "WS_AND_SSE_OK" : ("BAD:" + JSON.stringify([wr, sr]));
}})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F09_05", wait_load_ms=5000)
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "WS_AND_SSE_OK", f"Got: {wres.dump_json.get('title')!r}")
        ws_traces = srv.get_ws_traces()[ws_before:]
        ensure(len(ws_traces) >= 1 and ws_traces[-1].get("masked_all") is True, f"Client frames must be masked per RFC 6455: {ws_traces}")
        return "RFC 6455 WebSocket (UTF-8 + 4B/300B binary + client masking + 1000 close) and EventSource SSE verified"
    finally:
        if owns:
            srv.stop()


def _t1_f09_06(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        stream_url = srv.register_route(
            "/f09/streams-cjk",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#0f766e;color:#fff">CJK_STREAM</div>
<script>
(async () => {
  async function roundTrip(fmt, text) {
    const src = new TextEncoder().encode(text);
    const cs = new CompressionStream(fmt);
    const w = cs.writable.getWriter();
    w.write(src);
    w.close();
    const compBuf = await new Response(cs.readable).arrayBuffer();
    const ds = new DecompressionStream(fmt);
    const dw = ds.writable.getWriter();
    dw.write(new Uint8Array(compBuf));
    dw.close();
    const decBuf = await new Response(ds.readable).arrayBuffer();
    const outTxt = new TextDecoder().decode(decBuf);
    return { compLen: compBuf.byteLength, srcLen: src.byteLength, match: outTxt === text };
  }
  const bigTxt = "Arcradient2026CompressionStreamTest_한글_日本語_".repeat(400);
  const g = await roundTrip("gzip", bigTxt);
  const d = await roundTrip("deflate", bigTxt);
  const encs = ["euc-kr", "shift_jis", "euc-jp", "gb18030", "big5"];
  let tdOk = true;
  for (const enc of encs) {
    const info = await (await fetch("/charset-bytes/" + enc)).json();
    const decoded = new TextDecoder(info.label).decode(new Uint8Array(info.bytes));
    if (decoded !== info.expected) tdOk = false;
  }
  const ok = g.match && g.compLen < g.srcLen * 0.4 && d.match && d.compLen < d.srcLen * 0.4 && tdOk;
  document.title = ok ? "COMPRESS_AND_TEXTDECODER_OK" : ("BAD:" + JSON.stringify({g, d, tdOk}));
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, stream_url, test_id="T1_F09_06_streams", wait_load_ms=5000)
        ensure(wres.ok, f"Streams/TextDecoder guard failure: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "COMPRESS_AND_TEXTDECODER_OK", f"Got: {wres.dump_json.get('title')!r}")

        expected_phrases = {
            "euc-kr": "안녕하세요 한글 2026",
            "shift_jis": "こんにちは世界 2026",
            "euc-jp": "東京タワー 2026",
            "gb18030": "你好世界 2026",
            "big5": "繁體中文測試 2026",
        }
        for key, phrase in expected_phrases.items():
            pres = run_waterscape_verify(ctx, f"{srv.main_origin}/charset/{key}", test_id=f"T1_F09_06_{key}")
            ensure(pres.ok, f"HTML charset {key} guard failure: {pres.guard_failures}")
            ensure(pres.dump_json.get("title") == phrase, f"Charset {key} title mismatch: {pres.dump_json.get('title')!r} != {phrase!r}")
        return "CompressionStream/DecompressionStream + 5 CJK HTML & TextDecoder charsets verified"
    finally:
        if owns:
            srv.stop()


# =============================================================================
# Feature F10: Constructable Stylesheets & Web Animations API (Tier 1)
# =============================================================================

def _t1_f10_01(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f10/adopted-sync",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body style="margin:0;background:#ffffff"><div id="target"></div>
<script>
const sheet = new CSSStyleSheet();
sheet.replaceSync("#target { width: 310px; height: 90px; background-color: rgb(30, 144, 255); }");
document.adoptedStyleSheets = [sheet];
const rect = document.getElementById("target").getBoundingClientRect();
const ok = sheet.cssRules.length === 1 && Math.round(rect.width) === 310 && Math.round(rect.height) === 90;
document.title = ok ? "ADOPTED_SYNC_OK:310x90" : ("BAD:" + rect.width + "x" + rect.height);
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F10_01", save_content_png=True)
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "ADOPTED_SYNC_OK:310x90", f"Got: {wres.dump_json.get('title')!r}")
        ensure(wres.content_png is not None, "Missing content PNG")
        blue_px = wres.content_png.count_color_near((30, 144, 255), tol=18, rect=(5, 5, 300, 80))
        ensure(blue_px >= 4000, f"Expected rgb(30,144,255) pixels in #target, found {blue_px}")
        return f"new CSSStyleSheet().replaceSync + document.adoptedStyleSheets layout & paint ({blue_px} px) verified"
    finally:
        if owns:
            srv.stop()


def _t1_f10_02(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f10/adopted-async",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div id="box" style="width:120px;background:#1e293b">ASYNC</div>
<script>
(async () => {
  const sheet = new CSSStyleSheet();
  document.adoptedStyleSheets = [sheet];
  const p = sheet.replace("#box { height: 164px; color: rgb(255, 80, 20); }");
  const isProm = p instanceof Promise;
  const resolved = await p;
  const cs = getComputedStyle(document.getElementById("box"));
  const ok = isProm && (resolved === sheet) && cs.height === "164px" && cs.color === "rgb(255, 80, 20)";
  document.title = ok ? "ADOPTED_ASYNC_OK" : ("BAD:" + cs.height + "|" + cs.color);
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F10_02")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "ADOPTED_ASYNC_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "CSSStyleSheet.prototype.replace() Promise resolution & restyle verified"
    finally:
        if owns:
            srv.stop()


def _t1_f10_03(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f10/shadow-adopted",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title>
<style>.badge { width: 50px; height: 30px; background: #16a34a; }</style></head>
<body>
<div class="badge" id="lightBadge"></div>
<div id="h1"></div>
<div id="h2"></div>
<script>
const shared = new CSSStyleSheet();
shared.replaceSync(".badge { width: 222px; height: 40px; background-color: rgb(140, 40, 200); }");
const sr1 = document.getElementById("h1").attachShadow({ mode: "open" });
const sr2 = document.getElementById("h2").attachShadow({ mode: "open" });
sr1.innerHTML = "<div class='badge' id='b1'>S1</div>";
sr2.innerHTML = "<div class='badge' id='b2'>S2</div>";
sr1.adoptedStyleSheets = [shared];
sr2.adoptedStyleSheets = [shared];
const w1a = Math.round(sr1.getElementById("b1").getBoundingClientRect().width);
const w2a = Math.round(sr2.getElementById("b2").getBoundingClientRect().width);
const lightA = Math.round(document.getElementById("lightBadge").getBoundingClientRect().width);
shared.replaceSync(".badge { width: 333px; height: 40px; background-color: rgb(140, 40, 200); }");
const w1b = Math.round(sr1.getElementById("b1").getBoundingClientRect().width);
const w2b = Math.round(sr2.getElementById("b2").getBoundingClientRect().width);
const lightB = Math.round(document.getElementById("lightBadge").getBoundingClientRect().width);
const ok = w1a === 222 && w2a === 222 && lightA === 50 && w1b === 333 && w2b === 333 && lightB === 50;
document.title = ok ? "SHADOW_SHARED_ADOPTED_OK" : ("BAD:" + [w1a, w2a, lightA, w1b, w2b, lightB].join(","));
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F10_03")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "SHADOW_SHARED_ADOPTED_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "shadowRoot.adoptedStyleSheets live multi-instance sharing & light-DOM isolation verified"
    finally:
        if owns:
            srv.stop()


def _t1_f10_04(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        srv.register_route(
            "/f10/s1.css",
            {"status": 200, "content_type": "text/css; charset=utf-8", "body": "#a { height: 60px; background: #2563eb; }\n"},
        )
        url = srv.register_route(
            "/f10/doc-stylesheets",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title>
<link rel="stylesheet" href="/f10/s1.css">
<style id="st1">#a { width: 100px; }</style></head>
<body><div id="a">BOX</div>
<script>
window.addEventListener("load", () => {
  const len2 = document.styleSheets.length;
  const st2 = document.createElement("style");
  st2.textContent = "#a { color: rgb(255, 255, 255); }";
  document.head.appendChild(st2);
  const len3 = document.styleSheets.length;
  const sheet2 = st2.sheet || document.styleSheets[2];
  sheet2.insertRule("#a { width: 250px; }", sheet2.cssRules.length);
  const wInserted = Math.round(document.getElementById("a").getBoundingClientRect().width);
  sheet2.disabled = true;
  const wDisabled = Math.round(document.getElementById("a").getBoundingClientRect().width);
  const ok = len2 === 2 && len3 === 3 && wInserted === 250 && wDisabled === 100;
  document.title = ok ? "DOC_STYLESHEETS_LIVE_OK" : ("BAD:" + [len2, len3, wInserted, wDisabled].join(","));
});
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F10_04")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "DOC_STYLESHEETS_LIVE_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "document.styleSheets live length, insertRule, and sheet.disabled verified"
    finally:
        if owns:
            srv.stop()


def _t1_f10_05(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f10/element-animate",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div id="animBox" style="width:100px;height:60px;background:#e11d48;color:#fff">ANIM</div>
<script>
(async () => {
  const box = document.getElementById("animBox");
  const anim = box.animate(
    [
      { opacity: 0.2, width: "100px" },
      { opacity: 1.0, width: "300px" }
    ],
    { duration: 240, fill: "forwards", easing: "linear" }
  );
  const initialState = anim.playState;
  const samples = [];
  let done = false;
  anim.finished.then(() => { done = true; });
  while (!done) {
    await new Promise((r) => requestAnimationFrame(r));
    const w = parseFloat(getComputedStyle(box).width);
    samples.push(w);
    if (samples.length > 120) break;
  }
  const resolved = await anim.finished;
  const finalW = Math.round(parseFloat(getComputedStyle(box).width));
  const sawMid = samples.some((w) => w > 105 && w < 295);
  const ok = initialState === "running" && sawMid && resolved === anim && anim.playState === "finished" && finalW === 300;
  document.title = ok ? "ELEMENT_ANIMATE_OK" : ("BAD:" + JSON.stringify({initialState, sawMid, finalW, samples: samples.slice(0, 8)}));
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F10_05", wait_load_ms=4500)
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "ELEMENT_ANIMATE_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Element.prototype.animate() intermediate rAF interpolation & finished Promise verified"
    finally:
        if owns:
            srv.stop()


# =============================================================================
# Feature F11: Modern JS Heap/Turn Scaling & Removal of Deceptive Facades (Tier 1)
# =============================================================================

def _t1_f11_01(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f11/heap-96mb",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#2563eb;color:#fff">96MB</div>
<script>
const chunks = [];
let csum = 0;
for (let i = 0; i < 96; i++) {
  const u = new Uint8Array(1048576);
  u[0] = (i + 1) & 255;
  u[524288] = (i * 3) & 255;
  u[1048575] = (i * 7 + 11) & 255;
  chunks.push(u);
}
for (let i = 0; i < chunks.length; i++) {
  csum += chunks[i][0] + chunks[i][524288] + chunks[i][1048575];
}
window.__retained96 = chunks;
document.title = "HEAP_96MB_OK:" + csum + ":" + chunks.length;
</script></body></html>""",
            },
        )
        exp_csum = sum(((i + 1) & 255) + ((i * 3) & 255) + ((i * 7 + 11) & 255) for i in range(96))
        wres = run_waterscape_verify(ctx, url, test_id="T1_F11_01", profile="modern", wait_load_ms=8000, timeout_sec=20.0)
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == f"HEAP_96MB_OK:{exp_csum}:96", f"Got: {wres.dump_json.get('title')!r}")
        heap_b = int((wres.dump_json.get("stats") or {}).get("js_heap_bytes", 0))
        ensure(heap_b >= 67_108_864, f"Expected stats.js_heap_bytes >= 64MB, got {heap_b}")
        return f"96 MB live JS allocation succeeded in browser (js_heap_bytes={heap_b})"
    finally:
        if owns:
            srv.stop()


def _t1_f11_02(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f11/turn-1250ms",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#059669;color:#fff">TURN</div>
<script>
const t0 = performance.now();
let h = 0x811c9dc5 | 0;
let rounds = 0;
while ((performance.now() - t0) < 1250) {
  for (let i = 0; i < 20000; i++) {
    h = Math.imul(h ^ (i & 255), 0x01000193);
  }
  rounds++;
}
const elapsed = Math.round(performance.now() - t0);
document.title = (elapsed >= 1200 && rounds > 0) ? ("TURN_1250MS_OK:" + (h >>> 0 > 0 ? "1" : "1")) : ("SHORT:" + elapsed);
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F11_02", profile="modern", wait_load_ms=6000)
        ensure(wres.ok, f"Guard failures (must not hit 750ms interrupt!): {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "TURN_1250MS_OK:1", f"Got: {wres.dump_json.get('title')!r}")
        return "1,250 ms synchronous JS turn completed cleanly without 750ms deadline interrupt"
    finally:
        if owns:
            srv.stop()


def _t1_f11_03(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        yt_html = """<!DOCTYPE html><html><head><meta charset="utf-8"><title>REAL_YT_TITLE</title></head>
<body><div id="real-app" style="width:220px;height:70px;background:#dc2626;color:#fff">ORIGINAL_SSR_DOM</div>
<script>
var ytInitialData = {"contents":{"twoColumnWatchNextResults":{}}};
var ytInitialPlayerResponse = {"videoDetails":{"videoId":"dQw4w9WgXcQ","title":"FakeIfHydrated"}};
window.addEventListener("load", () => {
  const bodyHtml = document.body.innerHTML;
  const intact = !!document.getElementById("real-app")
    && bodyHtml.includes("ORIGINAL_SSR_DOM")
    && !bodyHtml.includes("arcr-yt-")
    && !bodyHtml.includes("ws2026")
    && document.title === "REAL_YT_TITLE";
  document.title = intact ? "NO_HYDRATOR_YT_OK" : ("HYDRATED_BAD:" + bodyHtml.slice(0, 120));
});
</script></body></html>"""
        x_html = """<!DOCTYPE html><html><head><meta charset="utf-8"><title>REAL_X_TITLE</title></head>
<body><div id="react-root"><div id="real-x" style="width:220px;height:70px;background:#0284c7;color:#fff">ORIGINAL_X_DOM</div></div>
<script>
window.__INITIAL_STATE__ = {"entities":{"tweets":{}}};
window.addEventListener("load", () => {
  const html = document.body.innerHTML;
  const intact = !!document.getElementById("real-x") && html.includes("ORIGINAL_X_DOM") && !html.includes("arcr-x-");
  document.title = intact ? "NO_HYDRATOR_X_OK" : "HYDRATED_X_BAD";
});
</script></body></html>"""

        yt_url = srv.register_route("/watch", {"status": 200, "content_type": "text/html; charset=utf-8", "body": yt_html})
        x_url = srv.register_route("/jack/status/20", {"status": 200, "content_type": "text/html; charset=utf-8", "body": x_html})

        for conf_flag in (False, True):
            r_yt = run_waterscape_verify(ctx, f"{yt_url}?v=dQw4w9WgXcQ", test_id=f"T1_F11_03_yt_{int(conf_flag)}", conformance=conf_flag)
            ensure(r_yt.ok, f"YT mode conformance={conf_flag} guard failure: {r_yt.guard_failures}")
            ensure(r_yt.dump_json.get("title") == "NO_HYDRATOR_YT_OK", f"YT conformance={conf_flag} title: {r_yt.dump_json.get('title')!r}")

            r_x = run_waterscape_verify(ctx, x_url, test_id=f"T1_F11_03_x_{int(conf_flag)}", conformance=conf_flag)
            ensure(r_x.ok, f"X mode conformance={conf_flag} guard failure: {r_x.guard_failures}")
            ensure(r_x.dump_json.get("title") == "NO_HYDRATOR_X_OK", f"X conformance={conf_flag} title: {r_x.dump_json.get('title')!r}")

        return "Zero hardcoded DOM replacement hydrators verified in BOTH default and --conformance modes"
    finally:
        if owns:
            srv.stop()


def _t1_f11_04(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/search",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title>
<style>
body { margin: 0; background: #ffffff; }
input[type="hidden"] { color: rgb(255, 0, 255); background: rgb(255, 0, 255); }
#vis-q { width: 200px; height: 36px; background: rgb(37, 99, 235); color: #ffffff; border: none; }
</style></head>
<body>
<form action="/search">
  <input id="hid-q" type="hidden" name="q" value="EhD9o-ciSecretToken">
  <input id="vis-q" type="text" name="visible_q" value="search">
</form>
<script>
const hid = document.getElementById("hid-q");
const disp = getComputedStyle(hid).display;
const r = hid.getBoundingClientRect();
const ok = disp === "none" && r.width === 0 && r.height === 0;
document.title = ok ? "HIDDEN_INPUT_Q_OK" : ("EXPOSED:" + disp + ":" + r.width + "x" + r.height);
</script></body></html>""",
            },
        )
        for conf_flag in (False, True):
            wres = run_waterscape_verify(ctx, url, test_id=f"T1_F11_04_{int(conf_flag)}", conformance=conf_flag, save_content_png=True)
            ensure(wres.ok, f"conformance={conf_flag} guard failure: {wres.guard_failures}")
            ensure(wres.dump_json.get("title") == "HIDDEN_INPUT_Q_OK", f"conformance={conf_flag} got: {wres.dump_json.get('title')!r}")
            ensure(wres.content_png is not None, "Missing content PNG")
            magenta = wres.content_png.count_color_near((255, 0, 255), tol=20)
            ensure(magenta == 0, f"Hidden input[name='q'] bled {magenta} magenta pixels onto canvas (conformance={conf_flag})!")
        return "input[type='hidden'][name='q'] remains display:none & 0x0 px in default & --conformance modes"
    finally:
        if owns:
            srv.stop()


def _t1_f11_05(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f11/pure-static.html",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>STATIC_NO_JS</title>
<style>
body { margin: 20px; background: #f8fafc; font-family: sans-serif; }
.card { width: 320px; padding: 16px; background: #2563eb; color: #ffffff; border-radius: 8px; }
ul li { margin: 4px 0; color: #0f172a; }
</style></head>
<body>
<header><h1>Pure HTML + CSS Document</h1></header>
<main class="card"><p>Zero script elements anywhere.</p><img src="/img/green64.png" width="64" height="64" alt="g"></main>
<ul><li>Item 1</li><li>Item 2</li><li>Item 3</li></ul>
</body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T1_F11_05")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        stats = wres.dump_json.get("stats") or {}
        ensure(int(stats.get("dom_node_count", 0)) >= 10, f"Expected >=10 DOM nodes, got {stats.get('dom_node_count')}")
        ensure(int(stats.get("display_cmd_count", 0)) >= 4, f"Expected >=4 display cmds, got {stats.get('display_cmd_count')}")
        ensure(stats.get("js_module_loaded") is False, f"JS module loaded eagerly on scriptless page: {stats}")
        ensure(int(stats.get("js_heap_bytes", -1)) == 0, f"Expected js_heap_bytes == 0 on scriptless page, got {stats.get('js_heap_bytes')}")
        return "Scriptless HTML document rendered cleanly with js_module_loaded == false and js_heap_bytes == 0"
    finally:
        if owns:
            srv.stop()


def _t1_f11_06(ctx: RunnerContext) -> str:
    forbidden_syms = (
        "JS_NewRuntime",
        "JS_NewContext",
        "JS_EvalFunction",
        "js_std_loop",
        "_ZN2v8",
        "JSEvaluateScript",
        "JSGlobalContextCreate",
        "CreateCoreWebView2Environment",
    )
    targets = [
        ctx.arcr_js_bin,
        ctx.waterscape_bin,
        os.path.join(ctx.module_dir, "libarcr_engine.so"),
        os.path.join(ctx.module_dir, "libarcr_js_portable.so"),
        os.path.join(ctx.module_dir, "libarcr_net.so"),
        os.path.join(ctx.module_dir, "libarcr_media.so"),
    ]
    checked = 0
    for bin_path in targets:
        ensure(os.path.isfile(bin_path), f"Expected compiled binary/module missing: {bin_path}")
        checked += 1
        nm_dyn = subprocess.run(["nm", "-D", bin_path], capture_output=True, text=True, timeout=10)
        nm_all = subprocess.run(["nm", bin_path], capture_output=True, text=True, timeout=10)
        combined = nm_dyn.stdout + "\n" + nm_all.stdout
        for bad in forbidden_syms:
            ensure(bad not in combined, f"Forbidden third-party engine symbol {bad!r} found in {os.path.basename(bin_path)}")

    readelf_js = subprocess.run(["readelf", "-d", ctx.arcr_js_bin], capture_output=True, text=True, timeout=10)
    ensure(readelf_js.returncode == 0, f"readelf -d arcr_js failed: {readelf_js.stderr}")
    needed_libs = re.findall(r"Shared library:\s*\[([^\]]+)\]", readelf_js.stdout)
    ensure(len(needed_libs) >= 1, "Expected NEEDED shared library entries on arcr_js")
    for lib in needed_libs:
        for forbidden_dep in ("libarcr_engine", "libX11", "libssl", "libcrypto", "libquickjs", "libv8", "libwebkit"):
            ensure(forbidden_dep not in lib, f"Standalone arcr_js coupled to forbidden library {lib!r}")
    return f"Clean-room ELF symbol & NEEDED dependency audit passed across {checked} binaries (arcr_js deps={needed_libs})"


TESTS: list[TestCaseSpec] = [
    # F01 (5)
    _spec("T1_F01_01", "F01", "env-setup.sh exists, #!/usr/bin/env bash, set -euo pipefail, bash -n & toolchain checks", _t1_f01_01),
    _spec("T1_F01_02", "F01", ".agents/environment.json valid JSON install command & bash -n syntax check", _t1_f01_02),
    _spec("T1_F01_03", "F01", "~/.bashrc non-interactive guard strip & BASH_ENV subshell PATH resolution", _t1_f01_03),
    _spec("T1_F01_04", "F01", "Cloud VM stop/wake persistence guards (mkdir -p, ln -sf/-sfn) in env-setup", _t1_f01_04),
    _spec("T1_F01_05", "F01", "Back-to-back double execution exit 0, .bashrc SHA-256 idempotency & clean repo", _t1_f01_05),
    # F02 (5)
    _spec("T1_F02_01", "F02", "50,000 distinct string & numeric constants pool without uint16 wrap", _t1_f02_01),
    _spec("T1_F02_02", "F02", "35,000 function-scope local variables crossing slot_cap 32,768 wrap boundary", _t1_f02_02),
    _spec("T1_F02_03", "F02", "35,000 ESM top-level const lexical bindings in module mode", _t1_f02_03),
    _spec("T1_F02_04", "F02", "160-level nested lexical & captured closure upvalue resolution", _t1_f02_04),
    _spec("T1_F02_05", "F02", "Empirical O(1) compiler hash scaling ratio T(40k)/T(10k) < 7.5", _t1_f02_05),
    # F03 (5)
    _spec("T1_F03_01", "F03", "8.2 MB realistic class/closure/private-field mega-bundle in arcr_js", _t1_f03_01, related_features=("F02",)),
    _spec("T1_F03_02", "F03", "4.1 MB HTTP script bundle loaded & executed in waterscape --conformance", _t1_f03_02, related_features=("F02", "F11")),
    _spec("T1_F03_03", "F03", "Mark-sweep GC 240 MB allocation churn inside 64 MB cap retaining 500 sentinels", _t1_f03_03),
    _spec("T1_F03_04", "F03", "WeakRef liveness/clearing and FinalizationRegistry cleanup job callbacks", _t1_f03_04),
    _spec("T1_F03_05", "F03", "Cyclic ESM + Top-Level await + dynamic import() + importmap under GC pressure", _t1_f03_05),
    # F04 (5)
    _spec("T1_F04_01", "F04", "Symbol.dispose/asyncDispose descriptors & block 'using' LIFO unwinding", _t1_f04_01),
    _spec("T1_F04_02", "F04", "'await using' sequential async LIFO disposal & sync [Symbol.dispose] fallback", _t1_f04_02),
    _spec("T1_F04_03", "F04", "DisposableStack full lifecycle (use, adopt, defer, move, dispose, disposed)", _t1_f04_03),
    _spec("T1_F04_04", "F04", "AsyncDisposableStack full lifecycle (use, adopt, defer, move, disposeAsync)", _t1_f04_04),
    _spec("T1_F04_05", "F04", "for (using x of iter) per-iteration disposal & contextual keyword 'using'", _t1_f04_05),
    # F05 (5)
    _spec("T1_F05_01", "F05", "Math.sumPrecise catastrophic cancellation ([1e308, 1, -1e308]) & iterables", _t1_f05_01),
    _spec("T1_F05_02", "F05", "Math.sumPrecise IEEE-754 signed-zero & empty iterable rules", _t1_f05_02),
    _spec("T1_F05_03", "F05", "Map.prototype.getOrInsert & getOrInsertComputed (undefined value & -0 key)", _t1_f05_03),
    _spec("T1_F05_04", "F05", "WeakMap.prototype.getOrInsert & getOrInsertComputed (Object & Symbol keys)", _t1_f05_04),
    _spec("T1_F05_05", "F05", "Intl.DurationFormat digital/long/ko-KR/ISO-8601 & formatToParts", _t1_f05_05),
    # F06 (5)
    _spec("T1_F06_01", "F06", "Top-level browser navigation with exact 8 KB, 16 KB, and 32 KB URLs", _t1_f06_01),
    _spec("T1_F06_02", "F06", "12 KB <script src='/xjs/...'> subresource fetch & SHA-256 verification", _t1_f06_02),
    _spec("T1_F06_03", "F06", "12 KB <link rel='stylesheet'> fetch, layout width & pixel color verification", _t1_f06_03),
    _spec("T1_F06_04", "F06", "24 KB relative & absolute fetch('/async/hpba?...') without 2KB truncation", _t1_f06_04),
    _spec("T1_F06_05", "F06", "HTTP 302 redirect to 20 KB Location + new URL() + 18 KB <a href>", _t1_f06_05),
    # F07 (5)
    _spec("T1_F07_01", "F07", "HTTP Set-Cookie headers visible in document.cookie with attributes stripped", _t1_f07_01),
    _spec("T1_F07_02", "F07", "document.cookie setter sent on HTTP Cookie header in fetch() & navigation", _t1_f07_02),
    _spec("T1_F07_03", "F07", "--private 3-hop 302 redirect ephemeral session cookie retention", _t1_f07_03),
    _spec("T1_F07_04", "F07", "Cookie Path scoping (/admin vs /app) and Max-Age=0 immediate deletion", _t1_f07_04),
    _spec("T1_F07_05", "F07", "300 concurrent cookies stored & echoed without 128-cookie eviction", _t1_f07_05),
    # F08 (5)
    _spec("T1_F08_01", "F08", "Dynamic appendChild(<script src>) fetch, execution & onload ordering", _t1_f08_01),
    _spec("T1_F08_02", "F08", "Dynamic insertBefore(<link rel='stylesheet'>) fetch, cascade, paint & load", _t1_f08_02),
    _spec("T1_F08_03", "F08", "document.currentScript live DOM node identity & self-remove() in inline script", _t1_f08_03),
    _spec("T1_F08_04", "F08", "document.currentScript identity in static & dynamically appended external scripts", _t1_f08_04),
    _spec("T1_F08_05", "F08", "Dynamic inline <script> synchronous execution & 'already started' guard", _t1_f08_05),
    # F09 (6)
    _spec("T1_F09_01", "F09", "HTMLScriptElement.noModule, Link/Style/Slot protos, CDATASection & PI", _t1_f09_01),
    _spec("T1_F09_02", "F09", "Legacy createEvent/init*Event, PointerEvent & InputEvent constructors", _t1_f09_02),
    _spec("T1_F09_03", "F09", "Range & Selection DOM mutation and getBoundingClientRect()", _t1_f09_03),
    _spec("T1_F09_04", "F09", "URLPattern wildcard subdomain, regex groups, test() & exec()", _t1_f09_04),
    _spec("T1_F09_05", "F09", "RFC 6455 WebSocket text/binary masked echo & EventSource SSE stream", _t1_f09_05),
    _spec("T1_F09_06", "F09", "CompressionStream/DecompressionStream + 5 CJK HTML & TextDecoder charsets", _t1_f09_06),
    # F10 (5)
    _spec("T1_F10_01", "F10", "new CSSStyleSheet(), replaceSync() & document.adoptedStyleSheets layout + paint", _t1_f10_01),
    _spec("T1_F10_02", "F10", "Async CSSStyleSheet.prototype.replace() Promise resolution & restyle", _t1_f10_02),
    _spec("T1_F10_03", "F10", "shadowRoot.adoptedStyleSheets multi-root sharing & light-DOM encapsulation", _t1_f10_03),
    _spec("T1_F10_04", "F10", "Live document.styleSheets collection, insertRule() & sheet.disabled", _t1_f10_04),
    _spec("T1_F10_05", "F10", "Element.prototype.animate() intermediate rAF interpolation & finished Promise", _t1_f10_05),
    # F11 (6)
    _spec("T1_F11_01", "F11", "96 MB live JS heap allocation inside browser WebView without OOM", _t1_f11_01),
    _spec("T1_F11_02", "F11", "1,250 ms synchronous JS compute turn without 750 ms deadline interrupt", _t1_f11_02),
    _spec("T1_F11_03", "F11", "Zero hardcoded site hydrators on YouTube/X HTML in default & --conformance modes", _t1_f11_03),
    _spec("T1_F11_04", "F11", "Hidden input[type='hidden'][name='q'] zero-pixel visibility in both modes", _t1_f11_04),
    _spec("T1_F11_05", "F11", "Scriptless HTML lazy-load preservation (js_module_loaded == false)", _t1_f11_05),
    _spec("T1_F11_06", "F11", "Clean-room ELF symbol & NEEDED shared-library dependency audit", _t1_f11_06),
]


def get_tests() -> list[TestCaseSpec]:
    return list(TESTS)


get_tier1_f01_f11_tests = get_tests
