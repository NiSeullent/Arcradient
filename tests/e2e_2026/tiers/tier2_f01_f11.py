import sys
sys.dont_write_bytecode = True

import base64
import hashlib
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import time
import typing
import urllib.parse
import zlib

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
    srv = FixtureServer(os.path.join(ctx.out_dir, "_tier2_f01_f11_assets"))
    srv.start()
    return srv, True


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
                tier=2,
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
                tier=2,
                feature_id=feature_id,
                passed=False,
                diagnostic=str(exc),
                duration_ms=dt,
                related_features=list(related_features),
            )

    tc = TestCaseSpec(
        test_id=test_id,
        tier=2,
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


def validate_f01_artifacts(env_setup_path: str, env_json_path: str) -> tuple[bool, str]:
    """Structured F01 validator that returns (passed, diagnostic) without unhandled exceptions."""
    if not os.path.isfile(env_setup_path):
        return False, f"Missing env-setup script: {env_setup_path}"
    try:
        sh_txt = open(env_setup_path, "r", encoding="utf-8", errors="replace").read()
    except OSError as exc:
        return False, f"Unreadable env-setup script: {exc}"
    lines = sh_txt.splitlines()
    if not lines or lines[0].strip() != "#!/usr/bin/env bash":
        return False, "Line 1 must be #!/usr/bin/env bash"
    if not os.path.isfile(env_json_path):
        return False, f"Missing environment.json: {env_json_path}"
    try:
        parsed = json.loads(open(env_json_path, "r", encoding="utf-8", errors="replace").read())
    except Exception as exc:
        return False, f"Invalid JSON in environment.json: {exc}"
    if not isinstance(parsed, dict) or not isinstance(parsed.get("install"), str) or len(parsed["install"].strip()) < 20:
        return False, "Missing or trivial 'install' string in environment.json"
    return True, "F01 artifacts structurally valid"


# =============================================================================
# Feature F01: Cloud /env-setup Automation — Tier 2 Boundary & Negative Tests
# =============================================================================

def _t2_f01_01(ctx: RunnerContext) -> str:
    ensure(os.path.isfile(ENV_SETUP_PATH), f"Missing {ENV_SETUP_PATH}")
    ej_path = _env_json_path(ctx)
    ensure(os.path.isfile(ej_path), f"Missing {ej_path}")
    raw_sh = "\n".join(_uncommented_lines(open(ENV_SETUP_PATH, "r", encoding="utf-8").read()))
    try:
        install_cmd = str(json.loads(open(ej_path, "r", encoding="utf-8").read()).get("install", ""))
    except Exception as exc:
        raise AssertionFailure(f"Corrupt environment.json: {exc}")

    priv_re = re.compile(r"(?<![A-Za-z0-9_-])(sudo|su|apt|apt-get|dpkg|yum|apk)(?![A-Za-z0-9_-])")
    for label, txt in (("env-setup.sh", raw_sh), ("environment.json", install_cmd)):
        m = priv_re.search(txt)
        ensure(m is None, f"Forbidden privilege/package command {m.group(1) if m else ''!r} in {label}")

    tdir = ctx.test_dir("T2_F01_01")
    poison_bin = os.path.join(tdir, "poison_bin")
    iso_home = os.path.join(tdir, "iso_home")
    os.makedirs(poison_bin, exist_ok=True)
    os.makedirs(iso_home, exist_ok=True)
    sh_copy = os.path.join(tdir, "env-setup-copy.sh")
    shutil.copy2(ENV_SETUP_PATH, sh_copy)
    os.symlink(ctx.repo_root, os.path.join(iso_home, "arcradient"))

    for cmd_name in ("sudo", "su", "apt", "apt-get", "dpkg", "yum", "apk"):
        p = os.path.join(poison_bin, cmd_name)
        with open(p, "w", encoding="utf-8") as f:
            f.write('#!/usr/bin/env bash\necho "FORBIDDEN_PRIV_CMD:$0" >&2\nexit 99\n')
        os.chmod(p, 0o755)

    env = os.environ.copy()
    env["HOME"] = iso_home
    env["PATH"] = f"{poison_bin}:{env.get('PATH', '')}"

    r1 = subprocess.run(["bash", sh_copy], env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
    ensure(r1.returncode == 0 and "FORBIDDEN_PRIV_CMD:" not in r1.stderr, f"Poisoned sudo run failed: {r1.stderr[:240]}")
    r2 = subprocess.run(["bash", "-e", "-c", install_cmd], cwd=ctx.repo_root, env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
    ensure(r2.returncode == 0 and "FORBIDDEN_PRIV_CMD:" not in r2.stderr, f"Poisoned install run failed: {r2.stderr[:240]}")
    shutil.rmtree(tdir, ignore_errors=True)
    return "Rootless execution verified statically and dynamically under poisoned sudo/apt shims"


def _t2_f01_02(ctx: RunnerContext) -> str:
    ensure(os.path.isfile(ENV_SETUP_PATH), f"Missing {ENV_SETUP_PATH}")
    ej_path = _env_json_path(ctx)
    ensure(os.path.isfile(ej_path), f"Missing {ej_path}")
    secret_re = re.compile(
        r"(ghp_[A-Za-z0-9]{20,}|gho_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}"
        r"|AIza[0-9A-Za-z_-]{30,}|ya29\.[0-9A-Za-z_-]+|sk-[A-Za-z0-9]{20,}"
        r"|AKIA[0-9A-Z]{16}|-----BEGIN [A-Z ]*PRIVATE KEY-----|agy-plugin-tokens"
        r"|Bearer\s+[A-Za-z0-9._-]{16,})"
    )
    targets = [ENV_SETUP_PATH, ej_path]
    if os.path.isfile(REAL_BASHRC_PATH):
        targets.append(REAL_BASHRC_PATH)
    for p in targets:
        content = open(p, "r", encoding="utf-8", errors="replace").read()
        m = secret_re.search(content)
        ensure(m is None, f"Hardcoded secret/token pattern leaked in {os.path.basename(p)}")
    return "Zero hardcoded secrets, OAuth tokens, or plugin token paths found"


def _t2_f01_03(ctx: RunnerContext) -> str:
    ensure(os.path.isfile(ENV_SETUP_PATH), f"Missing {ENV_SETUP_PATH}")
    tdir = ctx.test_dir("T2_F01_03")
    woken_home = os.path.join(tdir, "woken_home")
    os.makedirs(os.path.join(woken_home, ".local", "bin"), exist_ok=True)
    with open(os.path.join(woken_home, ".local", "bin", "dummy_tool"), "w", encoding="utf-8") as f:
        f.write("#!/usr/bin/env bash\necho DUMMY_OK\n")
    os.chmod(os.path.join(woken_home, ".local", "bin", "dummy_tool"), 0o755)
    os.symlink("/nonexistent_old_root_2026", os.path.join(woken_home, ".local", "current"))
    os.symlink(ctx.repo_root, os.path.join(woken_home, "arcradient"))
    sh_copy = os.path.join(tdir, "env-setup-copy.sh")
    shutil.copy2(ENV_SETUP_PATH, sh_copy)

    brc = os.path.join(woken_home, ".bashrc")
    with open(brc, "w", encoding="utf-8") as f:
        f.write(
            "# Ubuntu /etc/skel/.bashrc guard\n"
            "case $- in\n"
            "    *i*) ;;\n"
            "      *) return;;\n"
            "esac\n"
            "export SENTINEL_AFTER_GUARD=alive_2026\n"
        )

    env = os.environ.copy()
    env["HOME"] = woken_home
    for idx in (1, 2):
        r = subprocess.run(["bash", sh_copy], env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
        ensure(r.returncode == 0, f"Woken VM run {idx} failed: {r.stderr[:240]}")
        if idx == 1:
            cnt1 = sum(1 for ln in open(brc, "r", encoding="utf-8").splitlines() if "export PATH=" in ln)
        else:
            cnt2 = sum(1 for ln in open(brc, "r", encoding="utf-8").splitlines() if "export PATH=" in ln)

    brc_after = open(brc, "r", encoding="utf-8").read()
    ensure("case $- in" not in brc_after, "Ubuntu non-interactive guard was not stripped from .bashrc")
    ensure(cnt1 == cnt2 and cnt1 >= 1, f"Duplicate export PATH lines added ({cnt1} vs {cnt2})")

    sub_env = env.copy()
    sub_env["BASH_ENV"] = brc
    probe = subprocess.run(
        ["bash", "-c", 'printf "%s:%s" "$SENTINEL_AFTER_GUARD" "$PATH"'],
        env=sub_env,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=10,
    )
    ensure(probe.returncode == 0 and probe.stdout.startswith("alive_2026:") and ".local/bin" in probe.stdout, f"Probe got: {probe.stdout!r}")
    shutil.rmtree(tdir, ignore_errors=True)
    return "Ubuntu case $- in guard strip & woken /home pre-existing state simulation verified"


def _t2_f01_04(ctx: RunnerContext) -> str:
    ensure(os.path.isfile(ENV_SETUP_PATH), f"Missing {ENV_SETUP_PATH}")
    tdir = ctx.test_dir("T2_F01_04")
    fresh_home = os.path.join(tdir, "fresh_home")
    os.makedirs(fresh_home, exist_ok=True)
    os.symlink(ctx.repo_root, os.path.join(fresh_home, "arcradient"))
    sh_copy = os.path.join(tdir, "env-setup-copy.sh")
    shutil.copy2(ENV_SETUP_PATH, sh_copy)
    brc = os.path.join(fresh_home, ".bashrc")
    ensure(not os.path.exists(brc), "fresh_home must start with no .bashrc")

    env = os.environ.copy()
    env.update({"HOME": fresh_home, "DEBIAN_FRONTEND": "noninteractive", "CI": "1", "TERM": "dumb"})
    r = subprocess.run(["bash", sh_copy], env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
    ensure(r.returncode == 0, f"Fresh missing-.bashrc run failed: {r.stderr[:240]}")
    ensure(os.path.isfile(brc), "env-setup.sh did not create .bashrc in fresh HOME")

    sub_env = env.copy()
    sub_env["BASH_ENV"] = brc
    p = subprocess.run(["bash", "-c", 'printf "%s" "$PATH"'], env=sub_env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)
    ensure(p.returncode == 0 and ".local/bin" in p.stdout, f"Fresh .bashrc did not export .local/bin: {p.stdout!r}")
    shutil.rmtree(tdir, ignore_errors=True)
    return "Fresh VM missing-.bashrc + closed-stdin stress passed cleanly"


def _t2_f01_05(ctx: RunnerContext) -> str:
    tdir = ctx.test_dir("T2_F01_05")
    missing_sh = os.path.join(tdir, "nonexistent_env_setup.sh")
    corrupt_json = os.path.join(tdir, "corrupt_environment.json")
    valid_sh = os.path.join(tdir, "stub_valid.sh")
    with open(corrupt_json, "w", encoding="utf-8") as f:
        f.write("{bad json")
    with open(valid_sh, "w", encoding="utf-8") as f:
        f.write("#!/usr/bin/env bash\nset -euo pipefail\necho ok\n")

    ok1, diag1 = validate_f01_artifacts(missing_sh, corrupt_json)
    ensure(ok1 is False and "Missing env-setup" in diag1, f"Expected clean missing-sh failure, got {(ok1, diag1)}")

    ok2, diag2 = validate_f01_artifacts(valid_sh, corrupt_json)
    ensure(ok2 is False and "Invalid JSON" in diag2, f"Expected clean corrupt-json failure, got {(ok2, diag2)}")

    real_ok, real_diag = validate_f01_artifacts(ENV_SETUP_PATH, _env_json_path(ctx))
    ensure(real_ok is True, f"Real F01 artifacts failed validation: {real_diag}")
    shutil.rmtree(tdir, ignore_errors=True)
    return "Missing-file & corrupt-JSON validator boundary verified alongside live F01 artifacts"


# =============================================================================
# Feature F02: ArcradientJS O(1) Mega-Bundle Compiler & Slot Scaling (Tier 2)
# =============================================================================

def _t2_f02_01(ctx: RunnerContext) -> str:
    tdir = ctx.test_dir("T2_F02_01")
    js_path = os.path.join(tdir, "f02_const_identity.js")
    fillers = ",".join(f'"f_{i}"' for i in range(40000))
    code = (
        "const negZeroA = -0;\n"
        "const posZeroA = +0;\n"
        "const nanA = NaN;\n"
        'const strZero = "0";\n'
        "const numZero = 0;\n"
        'const specialKeys = ["__proto__", "constructor", "toString", "valueOf"];\n'
        f"const filler = [{fillers}];\n"
        "const negZeroB = -0;\n"
        "const posZeroB = +0;\n"
        "const nanB = 0 / 0;\n"
        "const ok = Object.is(negZeroA, -0) && Object.is(negZeroB, -0)\n"
        "  && Object.is(posZeroA, +0) && Object.is(posZeroB, +0)\n"
        "  && (1 / negZeroA === -Infinity) && (1 / posZeroA === +Infinity)\n"
        "  && (1 / negZeroB === -Infinity) && (1 / posZeroB === +Infinity)\n"
        "  && Number.isNaN(nanA) && Number.isNaN(nanB)\n"
        '  && typeof strZero === "string" && typeof numZero === "number"\n'
        '  && specialKeys[0] === "__proto__" && filler.length === 40000;\n'
        'print(ok ? "IDENTITY_OK" : "IDENTITY_BAD");\n'
    )
    with open(js_path, "w", encoding="utf-8") as f:
        f.write(code)
    res = run_arcr_js(ctx, script_path=js_path, heap_limit_mb=256, timeout_sec=8.0)
    ensure(res.returncode == 0, f"Constant identity failed (rc={res.returncode}): {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "IDENTITY_OK", f"Got: {res.stdout.strip()!r}")
    return "-0 vs +0, NaN, '0' vs 0, and Object.prototype-key hash identity preserved across 40k resizes"


def _t2_f02_02(ctx: RunnerContext) -> str:
    tdir = ctx.test_dir("T2_F02_02")
    js_path = os.path.join(tdir, "f02_u16_cross.js")
    decls = "\n".join(f'let l_{i} = {i}; const k_{i} = "c_{i}";' for i in range(66000))
    code = (
        "function cross66k() {\n"
        f"{decls}\n"
        "return [l_0, l_32767, l_32768, l_65534, l_65535, l_65536, l_65999, k_0, k_65536].join(',');\n"
        "}\n"
        "print(cross66k());\n"
    )
    with open(js_path, "w", encoding="utf-8") as f:
        f.write(code)
    res = run_arcr_js(ctx, script_path=js_path, heap_limit_mb=256, timeout_sec=10.0)
    ensure(res.returncode == 0, f"66k uint16 wrap test failed (rc={res.returncode}): {res.stderr.strip()[:240]}")
    expected = "0,32767,32768,65534,65535,65536,65999,c_0,c_65536"
    ensure(res.stdout.strip() == expected, f"uint16 wrap corruption detected: got {res.stdout.strip()!r}")
    return "66,000 locals & constants crossed 32,768 and 65,536 boundaries without wrap"


def _t2_f02_03(ctx: RunnerContext) -> str:
    tdir = ctx.test_dir("T2_F02_03")
    js_path = os.path.join(tdir, "f02_dup_34500.js")
    decls = "\n".join(f"let a_{i} = {i};" for i in range(35000))
    code = f'print("MUST_NOT_RUN");\nfunction dupScope() {{\n{decls}\nlet a_34500 = 99;\nreturn a_0;\n}}\ndupScope();\n'
    with open(js_path, "w", encoding="utf-8") as f:
        f.write(code)
    res = run_arcr_js(ctx, script_path=js_path, heap_limit_mb=256, timeout_sec=8.0)
    ensure(res.returncode == 2, f"Expected parse/early SyntaxError exit 2, got rc={res.returncode}")
    ensure("MUST_NOT_RUN" not in res.stdout and res.stdout.strip() == "", f"Code executed before early error: {res.stdout!r}")
    ensure("SyntaxError" in res.stderr, f"Expected SyntaxError in stderr, got: {res.stderr.strip()[:200]}")
    return "Duplicate lexical declaration at slot #34,500 caught as early SyntaxError (exit 2)"


def _t2_f02_04(ctx: RunnerContext) -> str:
    tdir = ctx.test_dir("T2_F02_04")
    tdz_path = os.path.join(tdir, "f02_tdz_33333.js")
    pre = "\n".join(f"let v_{i} = {i};" for i in range(33333))
    post = "\n".join(f"let v_{i} = {i};" for i in range(33334, 34000))
    tdz_code = (
        "function checkTdz() {\n"
        f"{pre}\n"
        "let caught = 'NONE';\n"
        "try { caught = 'VAL:' + v_33333; } catch (e) { caught = e.name; }\n"
        "let v_33333 = 42;\n"
        f"{post}\n"
        "return caught + ':' + v_0 + ':' + v_33333;\n"
        "}\nprint(checkTdz());\n"
    )
    with open(tdz_path, "w", encoding="utf-8") as f:
        f.write(tdz_code)
    r_tdz = run_arcr_js(ctx, script_path=tdz_path, heap_limit_mb=256, timeout_sec=8.0)
    ensure(r_tdz.returncode == 0 and r_tdz.stdout.strip() == "ReferenceError:0:42", f"TDZ at #33333 failed: {r_tdz.stdout.strip()!r} / {r_tdz.stderr[:200]}")

    cst_path = os.path.join(tdir, "f02_const_33333.js")
    cdecls = "\n".join(f"const c_{i} = {i};" for i in range(34000))
    cst_code = (
        "function checkConst() {\n"
        f"{cdecls}\n"
        "try { c_33333 = 99; return 'NO_THROW:' + c_33333; } catch (e) { return e.name + ':' + c_33333; }\n"
        "}\nprint(checkConst());\n"
    )
    with open(cst_path, "w", encoding="utf-8") as f:
        f.write(cst_code)
    r_cst = run_arcr_js(ctx, script_path=cst_path, heap_limit_mb=256, timeout_sec=8.0)
    ensure(r_cst.returncode == 0 and r_cst.stdout.strip() == "TypeError:33333", f"const at #33333 failed: {r_cst.stdout.strip()!r} / {r_cst.stderr[:200]}")
    return "TDZ ReferenceError and const mutation TypeError verified at local slot #33,333"


def _t2_f02_05(ctx: RunnerContext) -> str:
    tdir = ctx.test_dir("T2_F02_05")
    js_path = os.path.join(tdir, "f02_mega_jumps.js")
    branch_t = "\n".join("acc = (acc + 1) | 0; acc = (acc ^ 0) | 0;" for _ in range(20000))
    branch_f = "\n".join("acc = (acc + 2) | 0; acc = (acc ^ 0) | 0;" for _ in range(20000))
    code = (
        "function megaBranch(cond) {\n"
        "  let acc = 0;\n"
        f"  if (cond) {{\n{branch_t}\n  }} else {{\n{branch_f}\n  }}\n"
        "  return acc;\n"
        "}\n"
        "function megaLoop() {\n"
        "  let acc = 0;\n"
        "  while (true) {\n"
        "    acc += 5;\n"
        "    if (acc === 15) break;\n"
        f"{branch_t}\n"
        "  }\n"
        "  return acc;\n"
        "}\n"
        "print('TRUE_BRANCH_OK:' + megaBranch(true) + '|FALSE_BRANCH_OK:' + megaBranch(false) + '|LOOP_BREAK_OK:' + megaLoop());\n"
    )
    with open(js_path, "w", encoding="utf-8") as f:
        f.write(code)
    res = run_arcr_js(ctx, script_path=js_path, heap_limit_mb=256, timeout_sec=10.0)
    ensure(res.returncode == 0, f"Mega-jump offset test failed (rc={res.returncode}): {res.stderr.strip()[:240]}")
    expected = "TRUE_BRANCH_OK:20000|FALSE_BRANCH_OK:40000|LOOP_BREAK_OK:15"
    ensure(res.stdout.strip() == expected, f"Expected {expected!r}, got {res.stdout.strip()!r}")
    return ">120 KB bytecode forward/backward/break jump offsets verified without 16-bit truncation"


# =============================================================================
# Feature F03: ArcradientJS Bytecode & GC Mega-Bundle Throughput (Tier 2)
# =============================================================================

def _t2_f03_01(ctx: RunnerContext) -> str:
    tdir = ctx.test_dir("T2_F03_01")
    js_path = os.path.join(tdir, "f03_bundle_10_5mb.js")
    parts = ["(function(){\n'use strict';\nlet acc = 0;\n"]
    exp_acc = 0
    for f_idx in range(2000):
        v = (f_idx * 13 + 3) & 1023
        exp_acc += v
        parts.append(f"function fn_{f_idx}(x) {{ const c = () => x + {v}; return c(); }}\nacc += fn_{f_idx}(0);\n")
    for k in range(55000):
        parts.append(f'const KC_{k} = "kevlar_scale_const_{k:05d}_alpha_beta_gamma";\n')
    for l_idx in range(36000):
        parts.append(f"let LV_{l_idx} = {l_idx & 255};\n")
    cur_sz = sum(len(s) for s in parts)
    p_idx = 0
    while cur_sz < 10_800_000:
        ln = f'const PAD10M_{p_idx} = "kevlar_payload_block_{p_idx:06d}_0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";\n'
        parts.append(ln)
        cur_sz += len(ln)
        p_idx += 1
    parts.append(
        f"if (KC_0.length === 0 || KC_54999.length === 0 || LV_35999 < 0) acc = -1;\n"
        "print('KEVLAR_10M_OK:' + acc);\n})();\n"
    )
    with open(js_path, "w", encoding="utf-8") as f:
        f.write("".join(parts))
    sz_mb = os.path.getsize(js_path) / (1024 * 1024)
    ensure(sz_mb >= 10.2, f"Expected >= 10.2 MB bundle, got {sz_mb:.2f} MB")
    res = run_arcr_js(ctx, script_path=js_path, heap_limit_mb=256, timeout_sec=12.0, measure_rss=True)
    ensure(res.returncode == 0, f"10.5 MB bundle failed (rc={res.returncode}): {res.stderr.strip()[:240]}")
    ensure(res.duration_ms < 8000.0, f"10.5 MB bundle took {res.duration_ms:.1f}ms (expected < 8000ms)")
    if res.peak_rss_kb > 0:
        ensure(res.peak_rss_kb < 524288, f"Peak RSS {res.peak_rss_kb} KB exceeded 512 MB ceiling")
    ensure(res.stdout.strip() == f"KEVLAR_10M_OK:{exp_acc}", f"Got: {res.stdout.strip()!r}")
    return f"{sz_mb:.2f} MB bundle executed in {res.duration_ms:.1f}ms (peak RSS={res.peak_rss_kb} KB)"


def _t2_f03_02(ctx: RunnerContext) -> str:
    code = 'const keep = []; while (true) { keep.push("x".repeat(524288) + ":" + keep.length); }'
    res = run_arcr_js(ctx, code=code, heap_limit_mb=8, timeout_sec=5.0)
    ensure(not res.timed_out, "arcr_js hung instead of enforcing --heap-limit 8")
    ensure(res.returncode == 1, f"Expected clean OOM exit code 1 (never segfault), got rc={res.returncode}")
    ensure(
        ("Out of memory" in res.stderr) or ("RangeError" in res.stderr) or ("memory" in res.stderr.lower()),
        f"Expected OOM diagnostic on stderr, got: {res.stderr.strip()[:200]}",
    )
    return f"Graceful OOM enforced under --heap-limit 8 in {res.duration_ms:.1f}ms (exit 1)"


def _t2_f03_03(ctx: RunnerContext) -> str:
    r1 = run_arcr_js(ctx, code="for (;;) {}", max_time_ms=150, timeout_sec=4.0)
    ensure(not r1.timed_out and r1.returncode == 1, f"Loop deadline failed: rc={r1.returncode}, timed_out={r1.timed_out}")
    ensure(90.0 <= r1.duration_ms <= 1500.0, f"Expected ~150ms interrupt, got {r1.duration_ms:.1f}ms")

    r2 = run_arcr_js(ctx, code="function a(){ return b(); } function b(){ while(true){} } a();", max_time_ms=150, timeout_sec=4.0)
    ensure(not r2.timed_out and r2.returncode == 1 and r2.duration_ms <= 1500.0, f"Call/loop deadline failed: rc={r2.returncode}")
    return f"Execution deadline (--max-time-ms 150) interrupted tight loop cleanly in {r1.duration_ms:.1f}ms"


def _t2_f03_04(ctx: RunnerContext) -> str:
    code = r"""
    const arr = [];
    for (let i = 0; i < 200; i++) {
        const raw = { v: ((i * 73 + 19) % 200), tag: "item_" + i };
        arr.push(new Proxy(raw, {
            get(target, prop) {
                if (prop === "v") {
                    const tmp = { pad: "g".repeat(2048) + target.v };
                    if (target.v % 17 === 0 && typeof $262 !== "undefined" && $262.gc) $262.gc();
                    if (!tmp.pad) return -1;
                }
                return target[prop];
            }
        }));
    }
    arr.sort((a, b) => {
        const garbage = ["z".repeat(4096), { k: a.v - b.v }];
        if (typeof $262 !== "undefined" && $262.gc) $262.gc();
        return garbage[1].k;
    });
    let monotonic = arr.length === 200;
    for (let i = 0; i < arr.length; i++) {
        if (arr[i].v !== i || typeof arr[i].tag !== "string") { monotonic = false; break; }
    }
    const jsonStr = JSON.stringify(arr.slice(0, 10), (k, val) => {
        if (typeof $262 !== "undefined" && $262.gc) $262.gc();
        return val;
    });
    print([monotonic, jsonStr.includes('"v":0') && jsonStr.includes('"v":9')].join(","));
    """
    res = run_arcr_js(ctx, code=code, test262=True, timeout_sec=12.0)
    ensure(res.returncode == 0, f"Re-entrant GC callback stress failed (rc={res.returncode}): {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "true,true", f"Got: {res.stdout.strip()!r}")
    return "GC C-stack root pinning verified across Proxy get + Array.sort + JSON.stringify replacer"


def _t2_f03_05(ctx: RunnerContext) -> str:
    code = r"""
    const fired = [];
    const fr = new FinalizationRegistry((held) => { fired.push(held); });
    const unregTok = {};
    let sameTurnAlive = false;
    (function setup() {
        let tUnreg = { name: "unreg" };
        let tKeep = { name: "ctrl" };
        fr.register(tUnreg, "UNREG_HELD", unregTok);
        fr.register(tKeep, "CTRL_HELD");
        const wr = new WeakRef(tUnreg);
        const derefed = wr.deref();
        sameTurnAlive = (derefed === tUnreg);
        const removed = fr.unregister(unregTok);
        if (!removed) throw new Error("unregister returned false");
        tUnreg = null;
        tKeep = null;
    })();
    Promise.resolve().then(() => {
        if (typeof $262 !== "undefined" && $262.gc) $262.gc();
        for (let i = 0; i < 4000; i++) ({ x: i });
        if (typeof $262 !== "undefined" && $262.gc) $262.gc();
        Promise.resolve().then(() => {
            const ok = sameTurnAlive && !fired.includes("UNREG_HELD") && fired.includes("CTRL_HELD") && fired.length === 1;
            print(ok ? "UNREG_AND_SAMETURN_OK" : ("BAD:" + fired.join(",")));
        });
    });
    """
    res = run_arcr_js(ctx, code=code, test262=True, timeout_sec=10.0)
    ensure(res.returncode == 0, f"FinalizationRegistry.unregister test failed: {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "UNREG_AND_SAMETURN_OK", f"Got: {res.stdout.strip()!r}")
    return "FinalizationRegistry.prototype.unregister and same-turn WeakRef stability verified"


# =============================================================================
# Feature F04: ES2026 Explicit Resource Management — Tier 2 Boundary & Errors
# =============================================================================

def _t2_f04_01(ctx: RunnerContext) -> str:
    code = r"""
    const errBody = new Error("bodyErr");
    const err2 = new Error("dispErr2");
    const err1 = new Error("dispErr1");
    let caught = null;
    try {
        using r1 = { [Symbol.dispose]() { throw err1; } };
        using r2 = { [Symbol.dispose]() { throw err2; } };
        throw errBody;
    } catch (e) {
        caught = e;
    }
    const chainOk = (caught instanceof SuppressedError)
        && (caught instanceof Error)
        && caught.error === err1
        && (caught.suppressed instanceof SuppressedError)
        && caught.suppressed.error === err2
        && caught.suppressed.suppressed === errBody;

    let stackErr = null;
    const eA = new Error("deferA");
    const eB = new Error("deferB");
    try {
        const st = new DisposableStack();
        st.defer(() => { throw eA; });
        st.defer(() => { throw eB; });
        st.dispose();
    } catch (e) {
        stackErr = e;
    }
    const stOk = (stackErr instanceof SuppressedError) && stackErr.error === eA && stackErr.suppressed === eB;
    print([chainOk, stOk].join(","));
    """
    res = run_arcr_js(ctx, code=code)
    ensure(res.returncode == 0, f"SuppressedError chain failed: {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "true,true", f"Got: {res.stdout.strip()!r}")
    return "Nested SuppressedError(disposeError, priorError) chain verified"


def _t2_f04_02(ctx: RunnerContext) -> str:
    code = r"""
    let nullOk = false;
    {
        using a = null;
        using b = undefined;
        await using c = null;
        const st = new DisposableStack();
        const u1 = st.use(null);
        const u2 = st.use(undefined);
        st.dispose();
        nullOk = (u1 === null && u2 === undefined);
    }
    const badInputs = [42, "str", {}, { [Symbol.dispose]: 123 }];
    let badAllTypeError = true;
    for (const v of badInputs) {
        let threwType = false;
        try {
            using x = v;
        } catch (e) {
            threwType = (e instanceof TypeError);
        }
        if (!threwType) badAllTypeError = false;
    }
    let priorDisposedOnInitError = false;
    try {
        using ok = { [Symbol.dispose]() { priorDisposedOnInitError = true; } };
        using bad = 42;
    } catch (e) {
        if (!(e instanceof TypeError)) priorDisposedOnInitError = false;
    }
    print([nullOk, badAllTypeError, priorDisposedOnInitError].join(","));
    """
    res = run_arcr_js(ctx, code=code, module=True)
    ensure(res.returncode == 0, f"Nullish vs invalid resource test failed: {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "true,true,true", f"Got: {res.stdout.strip()!r}")
    return "null/undefined no-op vs invalid-resource TypeError & prior-resource cleanup verified"


def _t2_f04_03(ctx: RunnerContext) -> str:
    code = r"""
    function expectErr(fn, Cls) {
        try { fn(); return false; } catch (e) { return e instanceof Cls; }
    }
    const s = new DisposableStack();
    s.dispose();
    const res = { [Symbol.dispose]() {} };
    const rUse = expectErr(() => s.use(res), ReferenceError);
    const rAdopt = expectErr(() => s.adopt(1, () => {}), ReferenceError);
    const rDefer = expectErr(() => s.defer(() => {}), ReferenceError);
    const rMove = expectErr(() => s.move(), ReferenceError);

    const sMoved = new DisposableStack();
    const sDest = sMoved.move();
    const rMovedUse = expectErr(() => sMoved.use(res), ReferenceError);
    sDest.dispose();

    const sLive = new DisposableStack();
    const tDeferBad = expectErr(() => sLive.defer("not_fn"), TypeError);
    const tAdoptBad = expectErr(() => sLive.adopt(1, null), TypeError);
    const tNoNew = expectErr(() => DisposableStack(), TypeError);
    print([rUse, rAdopt, rDefer, rMove, rMovedUse, tDeferBad, tAdoptBad, tNoNew].join(","));
    """
    res = run_arcr_js(ctx, code=code)
    ensure(res.returncode == 0, f"Disposed stack error check failed: {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "true,true,true,true,true,true,true,true", f"Got: {res.stdout.strip()!r}")
    return "ReferenceError on disposed/moved stack and eager TypeError on non-callable verified"


def _t2_f04_04(ctx: RunnerContext) -> str:
    for label, bad_syntax in (
        ("no_init", 'print("NO"); { using x; }'),
        ("obj_destruct", 'print("NO"); { using { a } = { a: null }; }'),
        ("arr_destruct", 'print("NO"); { using [a] = [null]; }'),
    ):
        r = run_arcr_js(ctx, code=bad_syntax)
        ensure(r.returncode == 2 and r.stdout == "", f"{label}: expected parse SyntaxError exit 2 with empty stdout, got rc={r.returncode}, out={r.stdout!r}")
        ensure("SyntaxError" in r.stderr, f"{label}: expected SyntaxError in stderr, got {r.stderr[:160]}")

    r_const = run_arcr_js(ctx, code='{ using x = null; x = null; }')
    ensure(r_const.returncode == 1 and "TypeError" in r_const.stderr, f"Expected TypeError on using reassignment, got rc={r_const.returncode}, err={r_const.stderr[:160]}")
    return "using grammar restrictions (SyntaxError exit 2) and immutable binding (TypeError exit 1) verified"


def _t2_f04_05(ctx: RunnerContext) -> str:
    code = r"""
    const errBody = new Error("asyncBody");
    const errB = new Error("rejectB");
    const errA = new Error("throwA");
    let caught = null;
    try {
        await using r1 = { async [Symbol.asyncDispose]() { throw errA; } };
        await using r2 = { [Symbol.asyncDispose]() { return Promise.reject(errB); } };
        throw errBody;
    } catch (e) {
        caught = e;
    }
    const ok = (caught instanceof SuppressedError)
        && caught.error === errA
        && (caught.suppressed instanceof SuppressedError)
        && caught.suppressed.error === errB
        && caught.suppressed.suppressed === errBody;
    print(ok ? "ASYNC_SUPPRESSED_OK" : "ASYNC_SUPPRESSED_BAD");
    """
    res = run_arcr_js(ctx, code=code, module=True)
    ensure(res.returncode == 0, f"Async SuppressedError failed: {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "ASYNC_SUPPRESSED_OK", f"Got: {res.stdout.strip()!r}")
    return "await using sync-throw + rejected-Promise unwrapping into SuppressedError verified"


# =============================================================================
# Feature F05: ES2026 Built-ins & ECMA-402 13th Ed. Completion (Tier 2)
# =============================================================================

def _t2_f05_01(ctx: RunnerContext) -> str:
    code = r"""
    function expectTypeErr(fn) {
        try { fn(); return false; } catch (e) { return e instanceof TypeError; }
    }
    const nan1 = Number.isNaN(Math.sumPrecise([Infinity, -Infinity]));
    const posInf = Math.sumPrecise([Infinity, 1e308]) === Infinity;
    const negInf = Math.sumPrecise([-Infinity, -1]) === -Infinity;
    const nan2 = Number.isNaN(Math.sumPrecise([1, NaN, 2]));
    const notIter = expectTypeErr(() => Math.sumPrecise(123));
    const strElem = expectTypeErr(() => Math.sumPrecise([1, "2", 3]));
    const bigIntElem = expectTypeErr(() => Math.sumPrecise([1n]));

    let returnCalled = 0;
    let steps = 0;
    const customIter = {
        [Symbol.iterator]() {
            const vals = [Infinity, -Infinity, "bad", 4];
            return {
                next() {
                    if (steps >= vals.length) return { done: true, value: undefined };
                    return { done: false, value: vals[steps++] };
                },
                return() {
                    returnCalled++;
                    return { done: true, value: undefined };
                }
            };
        }
    };
    const postInfTypeErr = expectTypeErr(() => Math.sumPrecise(customIter));
    const iterCloseOk = postInfTypeErr && steps === 3 && returnCalled === 1;
    print([nan1, posInf, negInf, nan2, notIter, strElem, bigIntElem, iterCloseOk].join(","));
    """
    res = run_arcr_js(ctx, code=code)
    ensure(res.returncode == 0, f"Math.sumPrecise boundary test failed: {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "true,true,true,true,true,true,true,true", f"Got: {res.stdout.strip()!r}")
    return "Math.sumPrecise Infinity/NaN, non-number TypeError, no-early-break & IteratorClose verified"


def _t2_f05_02(ctx: RunnerContext) -> str:
    code = r"""
    const a = Math.sumPrecise([Number.MAX_VALUE, Number.MAX_VALUE, -Number.MAX_VALUE]) === Number.MAX_VALUE;
    const b = Math.sumPrecise([Number.MAX_VALUE, Number.MAX_VALUE]) === Infinity;
    const c = Math.sumPrecise([-Number.MAX_VALUE, -Number.MAX_VALUE, Number.MAX_VALUE]) === -Number.MAX_VALUE;
    print([a, b, c].join(","));
    """
    res = run_arcr_js(ctx, code=code)
    ensure(res.returncode == 0, f"Math.sumPrecise MAX_VALUE overflow boundary failed: {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "true,true,true", f"Got: {res.stdout.strip()!r}")
    return "Math.sumPrecise [MAX_VALUE, MAX_VALUE, -MAX_VALUE] === MAX_VALUE exact accumulator verified"


def _t2_f05_03(ctx: RunnerContext) -> str:
    code = r"""
    function expectTypeErr(fn) {
        try { fn(); return false; } catch (e) { return e instanceof TypeError; }
    }
    const wm = new WeakMap();
    const p1 = expectTypeErr(() => wm.getOrInsert("prim", 1));
    const p2 = expectTypeErr(() => wm.getOrInsert(123, 1));
    const p3 = expectTypeErr(() => wm.getOrInsert(null, 1));
    const pRegSym = expectTypeErr(() => wm.getOrInsert(Symbol.for("registered_global"), 1));

    const m = new Map([["present", 42]]);
    const eagerMapCb = expectTypeErr(() => m.getOrInsertComputed("present", "not_callable"));
    const kObj = {};
    wm.set(kObj, 99);
    const eagerWmCb = expectTypeErr(() => wm.getOrInsertComputed(kObj, 12345));

    const badBrandMap = expectTypeErr(() => Map.prototype.getOrInsert.call({}, "a", 1));
    const badBrandWm = expectTypeErr(() => WeakMap.prototype.getOrInsert.call(new Map(), kObj, 1));
    print([p1, p2, p3, pRegSym, eagerMapCb, eagerWmCb, badBrandMap, badBrandWm].join(","));
    """
    res = run_arcr_js(ctx, code=code)
    ensure(res.returncode == 0, f"getOrInsert* brand & validation checks failed: {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "true,true,true,true,true,true,true,true", f"Got: {res.stdout.strip()!r}")
    return "Map/WeakMap getOrInsert* brand checks, Symbol.for rejection & eager IsCallable check verified"


def _t2_f05_04(ctx: RunnerContext) -> str:
    code = r"""
    function expectErr(fn, Cls) {
        try { fn(); return false; } catch (e) { return e instanceof Cls; }
    }
    const dfDig = new Intl.DurationFormat("en-US", { style: "digital" });
    const zeroStr = dfDig.format({ seconds: 0 });
    const negStr = dfDig.format({ hours: -1, minutes: -30 });
    const mixedErr = expectErr(() => dfDig.format({ hours: 1, minutes: -30 }), RangeError);
    const floatErr = expectErr(() => dfDig.format({ hours: 1.5 }), RangeError);
    const nanErr = expectErr(() => dfDig.format({ seconds: NaN }), RangeError);
    const infErr = expectErr(() => dfDig.format({ seconds: Infinity }), RangeError);
    const noNewErr = expectErr(() => Intl.DurationFormat("en-US"), TypeError);
    const dfFrac = new Intl.DurationFormat("en-US", { style: "digital", fractionalDigits: 3 });
    const fracStr = dfFrac.format({ seconds: 1, milliseconds: 250 });
    const ok = zeroStr === "0:00:00"
        && (negStr.includes("-") || negStr.includes("\u2212")) && negStr.includes("1:30:00")
        && mixedErr && floatErr && nanErr && infErr && noNewErr
        && fracStr.includes("250");
    print(ok ? "DURATION_EDGE_OK" : ("BAD:" + JSON.stringify({zeroStr, negStr, mixedErr, floatErr, nanErr, infErr, noNewErr, fracStr})));
    """
    res = run_arcr_js(ctx, code=code)
    ensure(res.returncode == 0, f"Intl.DurationFormat boundary test failed: {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "DURATION_EDGE_OK", f"Got: {res.stdout.strip()!r}")
    return "Intl.DurationFormat zero, uniform negative, mixed-sign/non-integer RangeError & fractionalDigits verified"


def _t2_f05_05(ctx: RunnerContext) -> str:
    code = r"""
    const c1 = Object.is(Number.MIN_VALUE * 0.5, +0);
    const c2 = Number.MIN_VALUE * 0.5000000000000001 === Number.MIN_VALUE;
    const base = 9007199254740992;
    const c3 = (base + 1) === 9007199254740992;
    const c4 = (base + 2) === 9007199254740994;
    const c5 = typeof Math.f16round === "function" && Math.f16round(1.337) === 1.3369140625 && Object.is(Math.f16round(-0), -0);
    print([c1, c2, c3, c4, c5].join(","));
    """
    res = run_arcr_js(ctx, code=code)
    ensure(res.returncode == 0, f"IEEE-754 precision check failed: {res.stderr.strip()[:240]}")
    ensure(res.stdout.strip() == "true,true,true,true,true", f"Got: {res.stdout.strip()!r}")
    return "IEEE-754 subnormal, round-to-even, and Math.f16round precision verified"


# =============================================================================
# Feature F06: Long URL Support (>= 32 KB) — Tier 2 Boundary & Negative Tests
# =============================================================================

def _t2_f06_01(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        lengths = [2047, 2048, 2049, 4095, 4096, 8192, 32767, 32768]
        prefix_len = len(srv.main_origin + "/long-echo?p=")
        pad_map = {tot: tot - prefix_len for tot in lengths}
        url = srv.register_route(
            "/f06/sweep",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#2563eb;color:#fff">SWEEP</div>
<script>
(async () => {{
  const map = {json.dumps(pad_map)};
  let allOk = true;
  const bad = [];
  for (const [totStr, padLen] of Object.entries(map)) {{
    const tot = Number(totStr);
    const u = "/long-echo?p=" + "x".repeat(padLen);
    const fj = await (await fetch(u)).json();
    if (fj.full_url_len !== tot) {{ allOk = false; bad.push("fetch:" + tot + "!=" + fj.full_url_len); }}
    const xj = await new Promise((resolve) => {{
      const xhr = new XMLHttpRequest();
      xhr.open("GET", u, true);
      xhr.onload = () => resolve(JSON.parse(xhr.responseText));
      xhr.onerror = () => resolve({{ full_url_len: -1 }});
      xhr.send();
    }});
    if (xj.full_url_len !== tot) {{ allOk = false; bad.push("xhr:" + tot + "!=" + xj.full_url_len); }}
  }}
  document.title = allOk ? "SWEEP_2047_TO_32768_OK" : ("BAD:" + bad.join(","));
}})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F06_01", wait_load_ms=6000)
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "SWEEP_2047_TO_32768_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Exact boundary lengths (2047..32768) verified on both fetch() and XMLHttpRequest without off-by-one chop"
    finally:
        if owns:
            srv.stop()


def _t2_f06_02(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f06/malformed",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#dc2626;color:#fff">MALFORMED</div>
<script>
(async () => {
  function syncThrowsType(fn) {
    try { fn(); return false; } catch (e) { return e instanceof TypeError; }
  }
  const u1 = syncThrowsType(() => new URL("://bad"));
  const u2 = syncThrowsType(() => new URL("http://[::1:bad"));
  const u3 = syncThrowsType(() => new URL("relative/no-base"));
  const cp = (typeof URL.canParse === "function") ? (URL.canParse("://bad") === false) : true;
  let f1Rej = false;
  try { await fetch("://invalid"); } catch (e) { f1Rej = e instanceof TypeError; }
  let f2Rej = false;
  try { await fetch("http://256.256.256.256:99999/x"); } catch (e) { f2Rej = e instanceof TypeError; }
  const ok = u1 && u2 && u3 && cp && f1Rej && f2Rej;
  document.title = ok ? "MALFORMED_URL_REJECTED_OK" : ("BAD:" + [u1, u2, u3, cp, f1Rej, f2Rej].join(","));
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F06_02")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "MALFORMED_URL_REJECTED_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Malformed URL scheme/host/port properly rejected with TypeError"
    finally:
        if owns:
            srv.stop()


def _t2_f06_03(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        css_pad = "/* " + ("pad_comment_block_2026_" * 950) + " */\n"
        css_raw = (css_pad + "#dataBox { width: 276px; height: 84px; background-color: rgb(20, 184, 166); }\n").encode("utf-8")
        css_b64 = base64.b64encode(css_raw).decode("ascii")
        assert len(css_b64) >= 28672

        # Build a valid 16x16 PNG containing a 22KB tEXt metadata chunk so base64 data URI exceeds 28 KB
        green_png = srv.assets["green64.png"]
        text_payload = b"Comment\x00" + (b"P" * 22000)
        text_crc = zlib.crc32(b"tEXt" + text_payload) & 0xFFFFFFFF
        text_chunk = struct.pack(">I4s", len(text_payload), b"tEXt") + text_payload + struct.pack(">I", text_crc)
        iend_pos = green_png.rfind(b"\x00\x00\x00\x00IEND")
        padded_png = green_png[:iend_pos] + text_chunk + green_png[iend_pos:]
        png_b64 = base64.b64encode(padded_png).decode("ascii")
        assert len(png_b64) >= 28672

        url = srv.register_route(
            "/f06/data-uri-28k",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title>
<link rel="stylesheet" href="data:text/css;base64,{css_b64}"></head>
<body>
<div id="dataBox">DATA_CSS</div>
<img id="dataImg" src="data:image/png;base64,{png_b64}">
<script>
window.addEventListener("load", () => {{
  const img = document.getElementById("dataImg");
  const w = Math.round(document.getElementById("dataBox").getBoundingClientRect().width);
  const ok = w === 276 && img.complete === true && img.naturalWidth === 64;
  document.title = ok ? "DATA_URI_28KB_OK" : ("BAD:" + w + ":" + img.complete + ":" + img.naturalWidth);
}});
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F06_03")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "DATA_URI_28KB_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "28 KB data:text/css and data:image/png base64 URIs decoded without truncation"
    finally:
        if owns:
            srv.stop()


def _t2_f06_04(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        cursor = srv.mark_cursor()
        pad = "z" * 20000
        nav_url = f"{srv.main_origin}/f06/unicode-frag?q={urllib.parse.quote('한글검색')}&pad={pad}#frag_tail_999"

        def handler(req: dict[str, typing.Any]) -> dict[str, typing.Any]:
            has_frag_wire = "#" in req["raw_target"]
            q_val = req["qs"].get("q", [""])[0]
            pad_len = len(req["qs"].get("pad", [""])[0])
            wire_ok = (not has_frag_wire) and (q_val == "한글검색") and (pad_len == 20000)
            return {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#0891b2;color:#fff">FRAG</div>
<script>
const u = new URL(location.href);
const jsOk = location.hash === "#frag_tail_999" && u.searchParams.get("q") === "한글검색" && u.searchParams.get("pad").length === 20000;
document.title = ({'true' if wire_ok else 'false'} && jsOk) ? "UNICODE_20KB_FRAG_OK" : ("BAD:" + location.hash + ":" + u.searchParams.get("q"));
</script></body></html>""",
            }

        srv.register_route("/f06/unicode-frag", handler)
        wres = run_waterscape_verify(ctx, nav_url, test_id="T2_F06_04")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "UNICODE_20KB_FRAG_OK", f"Got: {wres.dump_json.get('title')!r}")
        traces = srv.get_traces(since=cursor, path_prefix="/f06/unicode-frag")
        ensure(len(traces) >= 1 and traces[-1]["has_fragment_on_wire"] is False, "Fragment # leaked onto HTTP request line!")
        return "20 KB UTF-8 percent-encoded URL preserved query & stripped #fragment before HTTP wire"
    finally:
        if owns:
            srv.stop()


def _t2_f06_05(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f06/extreme-128k",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#4f46e5;color:#fff">128KB</div>
<script>
(async () => {
  const big = location.origin + "/long-echo?a=" + "a".repeat(131072);
  let urlHandled = false;
  try {
    const u = new URL(big);
    urlHandled = u.href.length > 131072;
  } catch (e) {
    urlHandled = (e instanceof TypeError);
  }
  let fetchSafe = false;
  try {
    const r = await fetch(big);
    fetchSafe = (r.status === 200 || r.status === 414);
  } catch (e) {
    fetchSafe = (e instanceof TypeError);
  }
  const ping = await (await fetch("/ping")).json();
  document.title = (urlHandled && fetchSafe && ping.ok === true) ? "EXTREME_128KB_SAFE_OK" : "BAD_128KB";
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F06_05", wait_load_ms=5000)
        ensure(wres.returncode == 0 and not wres.timed_out, f"Browser crashed/hung on 128KB URL: rc={wres.returncode}")
        ensure(wres.dump_json.get("title") == "EXTREME_128KB_SAFE_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "128 KB oversize URL memory-safety stress passed with zero crash and intact follow-up /ping"
    finally:
        if owns:
            srv.stop()


# =============================================================================
# Feature F07: Unified Cookie Jar & Private Ephemeral Cookies (Tier 2)
# =============================================================================

def _t2_f07_01(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f07/httponly-shield",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "headers": [
                    ("Set-Cookie", "__session=server_secret_99; Path=/; HttpOnly"),
                    ("Set-Cookie", "ui=light; Path=/"),
                ],
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#2563eb;color:#fff">HTTPONLY</div>
<script>
(async () => {
  const dc1 = document.cookie;
  const hiddenFromJs = !dc1.includes("__session") && dc1.includes("ui=light");
  document.cookie = "__session=evil_override; Path=/";
  document.cookie = "__session=; Path=/; Max-Age=0";
  const r = await (await fetch("/cookie/echo")).json();
  const intactOnWire = r.cookie.includes("__session=server_secret_99") && !r.cookie.includes("evil_override");
  document.title = (hiddenFromJs && intactOnWire) ? "HTTPONLY_IMMUNE_OK" : ("BAD:" + dc1 + "|" + r.cookie);
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F07_01")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "HTTPONLY_IMMUNE_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "HttpOnly cookie invisible to document.cookie AND immune to JS overwrite/deletion"
    finally:
        if owns:
            srv.stop()


def _t2_f07_02(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        tdir = ctx.test_dir("T2_F07_02")
        shared_home = os.path.join(tdir, "shared_priv_home")
        shared_prof = os.path.join(tdir, "shared_priv_prof")
        os.makedirs(shared_home, exist_ok=True)
        os.makedirs(shared_prof, exist_ok=True)

        url1 = srv.register_route(
            "/f07/priv-set",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "headers": [("Set-Cookie", "priv_secret=ephemeral_only_777; Path=/; Max-Age=86400")],
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#16a34a;color:#fff">P1</div>
<script>
(async () => {
  const r = await (await fetch("/cookie/echo")).json();
  document.title = r.cookie.includes("priv_secret=ephemeral_only_777") ? "PRIV_RUN1_OK" : "PRIV_RUN1_BAD";
})();
</script></body></html>""",
            },
        )
        url2 = srv.register_route(
            "/f07/priv-check",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#dc2626;color:#fff">P2</div>
<script>
(async () => {
  const r = await (await fetch("/cookie/echo")).json();
  const clean = (r.cookie === "" || !r.cookie.includes("priv_secret")) && !document.cookie.includes("priv_secret");
  document.title = clean ? "PRIV_ISOLATED_OK" : ("LEAKED:" + r.cookie);
})();
</script></body></html>""",
            },
        )

        ov = {"HOME": shared_home, "WATERSCAPE_PROFILE_DIR": shared_prof}
        r1 = run_waterscape_verify(ctx, url1, test_id="T2_F07_02_run1", private=True, env_overrides=ov)
        ensure(r1.ok and r1.dump_json.get("title") == "PRIV_RUN1_OK", f"Run 1 failed: {r1.dump_json.get('title')!r} / {r1.guard_failures}")

        r2 = run_waterscape_verify(ctx, url2, test_id="T2_F07_02_run2", private=True, env_overrides=ov)
        ensure(r2.ok and r2.dump_json.get("title") == "PRIV_ISOLATED_OK", f"Run 2 leaked private cookie: {r2.dump_json.get('title')!r}")

        for root, _, files in os.walk(tdir):
            for fn in files:
                if fn.startswith("dump_") or fn.endswith(".png"):
                    continue
                fp = os.path.join(root, fn)
                raw = open(fp, "rb").read()
                ensure(b"ephemeral_only_777" not in raw, f"Private cookie persisted to disk in {fp}")
        return "Cross-invocation --private cookie isolation & zero disk persistence verified"
    finally:
        if owns:
            srv.stop()


def _t2_f07_03(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        localhost_origin = f"http://localhost:{srv.http_port}"
        srv.register_route(
            "/f07/cors-cookie-probe",
            lambda req: {
                "status": 200,
                "content_type": "application/json",
                "headers": {
                    "Access-Control-Allow-Origin": req["headers"].get("origin", "*"),
                    "Access-Control-Allow-Credentials": "true",
                },
                "body": json.dumps({"cookie": req["headers"].get("cookie", "")}),
            },
        )
        url = srv.register_route(
            "/f07/host-isolation",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "headers": [
                    ("Set-Cookie", "host_only_tok=ip127; Path=/"),
                    ("Set-Cookie", "bad_dom=spoof; Domain=example.com; Path=/"),
                ],
                "body": f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#7c3aed;color:#fff">HOST</div>
<script>
(async () => {{
  const same = await (await fetch("/cookie/echo")).json();
  let crossCookie = "ERR";
  try {{
    const cr = await (await fetch("{localhost_origin}/f07/cors-cookie-probe", {{ credentials: "include" }})).json();
    crossCookie = cr.cookie;
  }} catch (e) {{
    crossCookie = "";
  }}
  const ok = same.cookie.includes("host_only_tok=ip127")
    && !same.cookie.includes("bad_dom")
    && !crossCookie.includes("host_only_tok");
  document.title = ok ? "HOST_DOMAIN_ISOLATION_OK" : ("BAD:" + same.cookie + "|" + crossCookie);
}})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F07_03")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "HOST_DOMAIN_ISOLATION_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Host-only isolation (127.0.0.1 vs localhost) & foreign Domain=example.com rejection verified"
    finally:
        if owns:
            srv.stop()


def _t2_f07_04(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f07/eq-update",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#0d9488;color:#fff">EQ</div>
<script>
(async () => {
  document.cookie = "tok=v1==; Path=/";
  document.cookie = "tok=v2=x=y; Path=/; SameSite=Lax";
  const entries = document.cookie.split(";").map(s => s.trim()).filter(Boolean);
  const tokEntries = entries.filter(e => e.startsWith("tok="));
  const r = await (await fetch("/cookie/echo")).json();
  const ok = tokEntries.length === 1 && tokEntries[0] === "tok=v2=x=y" && r.cookie.includes("tok=v2=x=y") && !r.cookie.includes("v1==");
  document.title = ok ? "COOKIE_EQ_UPDATE_OK" : ("BAD:" + JSON.stringify(tokEntries) + "|" + r.cookie);
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F07_04")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "COOKIE_EQ_UPDATE_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Same-name cookie in-place update & '=' inside cookie-value preserved"
    finally:
        if owns:
            srv.stop()


def _t2_f07_05(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f07/cap-1024",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#ea580c;color:#fff">1024</div>
<script>
(async () => {
  for (let i = 0; i < 1024; i++) {
    document.cookie = "c_" + i + "=v" + i + "; Path=/";
  }
  const r1024 = await (await fetch("/cookie/echo")).json();
  for (let i = 1024; i < 1074; i++) {
    document.cookie = "c_" + i + "=v" + i + "; Path=/";
  }
  const rOver = await (await fetch("/cookie/echo")).json();
  const hasLatest = rOver.cookie.includes("c_1073=v1073") && rOver.cookie.includes("c_1024=v1024");
  const ok = r1024.count >= 1024 && hasLatest && rOver.count >= 1024;
  document.title = ok ? ("JAR_1024_OK:" + r1024.count + ":" + rOver.count) : ("BAD:" + r1024.count + ":" + rOver.count);
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F07_05", wait_load_ms=6000)
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(str(wres.dump_json.get("title", "")).startswith("JAR_1024_OK:"), f"Got: {wres.dump_json.get('title')!r}")
        return f"1,024-cookie capacity & 1,074-cookie eviction boundary verified ({wres.dump_json.get('title')})"
    finally:
        if owns:
            srv.stop()


# =============================================================================
# Feature F08: Dynamic <script> / <link> & document.currentScript (Tier 2)
# =============================================================================

def _t2_f08_01(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        srv.register_route(
            "/f08/missing-404.js",
            {"status": 404, "content_type": "text/html; charset=utf-8", "body": "<html><body>404 Not Found</body></html>"},
        )
        srv.register_route(
            "/f08/missing-404.css",
            {"status": 404, "content_type": "text/html; charset=utf-8", "body": "<html><body>404 CSS</body></html>"},
        )
        url = srv.register_route(
            "/f08/err-events",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#dc2626;color:#fff">404_SUB</div>
<script>
(async () => {
  const sRes = new Promise((resolve) => {
    const s = document.createElement("script");
    s.src = "/f08/missing-404.js";
    s.onload = () => resolve("LOAD");
    s.onerror = () => resolve("ERROR");
    document.head.appendChild(s);
  });
  const lRes = new Promise((resolve) => {
    const l = document.createElement("link");
    l.rel = "stylesheet";
    l.href = "/f08/missing-404.css";
    l.onload = () => resolve("LOAD");
    l.onerror = () => resolve("ERROR");
    document.head.appendChild(l);
  });
  const [sr, lr] = await Promise.all([sRes, lRes]);
  document.title = (sr === "ERROR" && lr === "ERROR") ? "DYN_404_ERROR_EVENTS_OK" : ("BAD:" + sr + ":" + lr);
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F08_01")
        ensure(wres.ok, f"Guard failures (404 subresource must not cause SyntaxError!): {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "DYN_404_ERROR_EVENTS_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Dynamic <script> & <link> HTTP 404 fired error (not load) with zero HTML-as-JS SyntaxError"
    finally:
        if owns:
            srv.stop()


def _t2_f08_02(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f08/null-cs",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div id="btn" style="width:160px;height:60px;background:#2563eb;color:#fff">CS_NULL</div>
<script type="module">
window.__modCs = document.currentScript;
</script>
<script>
Promise.resolve().then(() => { window.__microCs = document.currentScript; });
document.getElementById("btn").addEventListener("click", () => { window.__evCs = document.currentScript; });
document.getElementById("btn").click();
setTimeout(() => {
  window.__timerCs = document.currentScript;
  const ok = window.__modCs === null && window.__microCs === null && window.__timerCs === null;
  document.title = ok ? "CS_NULL_MODULE_ASYNC_OK" : ("BAD:" + [window.__modCs, window.__microCs, window.__timerCs].join(","));
}, 20);
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F08_02")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "CS_NULL_MODULE_ASYNC_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "document.currentScript === null verified in ES modules, Promise microtasks, and setTimeout"
    finally:
        if owns:
            srv.stop()


def _t2_f08_03(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f08/nested-cs",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#059669;color:#fff">STACK</div>
<script id="outer">
const beforeId = document.currentScript ? document.currentScript.id : "NULL";
const inner = document.createElement("script");
inner.id = "inner";
inner.textContent = "window.__innerId = document.currentScript ? document.currentScript.id : 'NULL';";
document.body.appendChild(inner);
const afterId = document.currentScript ? document.currentScript.id : "NULL";
document.title = (beforeId === "outer" && window.__innerId === "inner" && afterId === "outer")
  ? "NESTED_CS_STACK_OK"
  : ("BAD:" + beforeId + ":" + window.__innerId + ":" + afterId);
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F08_03")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "NESTED_CS_STACK_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Nested synchronous currentScript stack push/pop restoration verified"
    finally:
        if owns:
            srv.stop()


def _t2_f08_04(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        cursor = srv.mark_cursor()
        srv.register_route(
            "/f08/never-connected.js",
            {"status": 200, "content_type": "application/javascript", "body": "window.__badDetached = true;\n"},
        )
        srv.register_route(
            "/f08/late-src.js",
            {"status": 200, "content_type": "application/javascript", "body": "window.__lateRan = 88;\n"},
        )
        url = srv.register_route(
            "/f08/late-vs-detached",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#9333ea;color:#fff">LATE</div>
<script>
const det = document.createElement("script");
det.src = "/f08/never-connected.js";
const s = document.createElement("script");
s.onload = () => {
  const ok = window.__lateRan === 88 && window.__badDetached === undefined;
  document.title = ok ? "LATE_SRC_AND_DETACHED_OK" : "BAD_LATE_DETACHED";
};
document.body.appendChild(s);
s.src = "/f08/late-src.js";
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F08_04")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "LATE_SRC_AND_DETACHED_OK", f"Got: {wres.dump_json.get('title')!r}")
        det_hits = srv.get_traces(since=cursor, path_prefix="/f08/never-connected.js")
        ensure(len(det_hits) == 0, f"Detached <script src> must not fetch, got {len(det_hits)} hits")
        return "Connected late s.src mutation executed while detached <script src> never fetched"
    finally:
        if owns:
            srv.stop()


def _t2_f08_05(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        cursor = srv.mark_cursor()
        srv.register_route(
            "/f08/dyn/dep.mjs",
            {"status": 200, "content_type": "application/javascript; charset=utf-8", "body": "export const depVal = 321;\n"},
        )
        srv.register_route(
            "/f08/dyn/mod.mjs",
            {
                "status": 200,
                "content_type": "application/javascript; charset=utf-8",
                "body": "import { depVal } from '/f08/dyn/dep.mjs';\nwindow.__modEvalCount = (window.__modEvalCount || 0) + 1;\nwindow.__modSum = depVal;\n",
            },
        )
        url = srv.register_route(
            "/f08/dyn-module-reinsert",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#0284c7;color:#fff">DYN_MOD</div>
<script>
const m = document.createElement("script");
m.type = "module";
m.src = "/f08/dyn/mod.mjs";
m.onload = () => {
  m.remove();
  document.body.appendChild(m);
  setTimeout(() => {
    const ok = window.__modEvalCount === 1 && window.__modSum === 321;
    document.title = ok ? "DYN_MODULE_ONCE_OK" : ("BAD:" + window.__modEvalCount + ":" + window.__modSum);
  }, 50);
};
document.head.appendChild(m);
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F08_05")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "DYN_MODULE_ONCE_OK", f"Got: {wres.dump_json.get('title')!r}")
        mod_hits = srv.get_traces(since=cursor, path_prefix="/f08/dyn/mod.mjs")
        ensure(len(mod_hits) == 1, f"Expected 1 fetch of /f08/dyn/mod.mjs across re-insert, got {len(mod_hits)}")
        return "Dynamic <script type='module' src> & 'already started' re-insert guard verified"
    finally:
        if owns:
            srv.stop()


# =============================================================================
# Feature F09: Complete Web IDL Globals, Prototypes & Legacy Event Init (Tier 2)
# =============================================================================

def _t2_f09_01(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f09/brand-checks",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#2563eb;color:#fff">BRAND</div>
<script>
function throwsType(fn) {
  try { fn(); return false; } catch (e) { return e instanceof TypeError; }
}
const descNoMod = Object.getOwnPropertyDescriptor(HTMLScriptElement.prototype, "noModule");
const b1 = descNoMod && throwsType(() => descNoMod.get.call({}));
const b2 = throwsType(() => Range.prototype.toString.call({}));
const b3 = throwsType(() => URLPattern.prototype.test.call({}, "https://example.com"));
const protoOk = Object.getPrototypeOf(PointerEvent.prototype) === MouseEvent.prototype;
const xmlDoc = new DOMParser().parseFromString("<r><![CDATA[a&b]]><?pi target-data?></r>", "application/xml");
const root = xmlDoc.documentElement;
let hasCdata = false, hasPi = false;
for (const n of Array.from(root.childNodes)) {
  if (n.nodeType === 4 && (n instanceof CDATASection) && n.data === "a&b") hasCdata = true;
  if (n.nodeType === 7 && (n instanceof ProcessingInstruction)) hasPi = true;
}
const ok = b1 && b2 && b3 && protoOk && hasCdata && hasPi;
document.title = ok ? "WEBIDL_BRAND_XML_OK" : ("BAD:" + [b1, b2, b3, protoOk, hasCdata, hasPi].join(","));
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F09_01")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "WEBIDL_BRAND_XML_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Web IDL Illegal invocation TypeError brand checks & XML CDATASection/ProcessingInstruction verified"
    finally:
        if owns:
            srv.stop()


def _t2_f09_02(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        cursor = srv.mark_cursor()
        srv.register_route(
            "/f09/must-not-run-legacy.js",
            {"status": 200, "content_type": "application/javascript", "body": "window.__legacyExecuted = true; document.title = 'LEGACY_RAN_BAD';\n"},
        )
        url = srv.register_route(
            "/f09/nomodule-src",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#16a34a;color:#fff">NOMOD</div>
<script nomodule src="/f09/must-not-run-legacy.js"></script>
<script>
window.addEventListener("load", () => {
  document.title = (window.__legacyExecuted === undefined) ? "MODERN_ONLY" : "LEGACY_RAN_BAD";
});
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F09_02")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "MODERN_ONLY", f"Got: {wres.dump_json.get('title')!r}")
        hits = srv.get_traces(since=cursor, path_prefix="/f09/must-not-run-legacy.js")
        ensure(len(hits) == 0, f"<script nomodule src=...> must not fetch over HTTP, got {len(hits)} hits")
        return "<script nomodule src> neither fetched nor executed"
    finally:
        if owns:
            srv.stop()


def _t2_f09_03(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f09/urlpattern-edge",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#7c3aed;color:#fff">UP_EDGE</div>
<script>
function throwsType(fn) {
  try { fn(); return false; } catch (e) { return e instanceof TypeError; }
}
const e1 = throwsType(() => new URLPattern("http://[invalid"));
const e2 = throwsType(() => new URLPattern({ pathname: "/:unclosed(" }));
const rel = new URLPattern("/items/:id", "https://shop.example/v1/");
const m1 = rel.test("https://shop.example/items/99") === true;
const m2 = rel.test("https://other.example/items/99") === false;
document.title = (e1 && e2 && m1 && m2) ? "URLPATTERN_EDGE_OK" : ("BAD:" + [e1, e2, m1, m2].join(","));
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F09_03")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "URLPATTERN_EDGE_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "URLPattern malformed syntax TypeError & baseURL origin scoping verified"
    finally:
        if owns:
            srv.stop()


def _t2_f09_04(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        tmp_s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        tmp_s.bind(("127.0.0.1", 0))
        closed_port = tmp_s.getsockname()[1]
        tmp_s.close()

        url = srv.register_route(
            "/f09/ws-reject",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#dc2626;color:#fff">WS_ERR</div>
<script>
(async () => {{
  let badUrlThrew = false;
  try {{ new WebSocket(":::invalid_ws_url"); }} catch (e) {{ badUrlThrew = (e instanceof DOMException) || (e instanceof SyntaxError); }}

  function probeFailWs(u) {{
    return new Promise((resolve) => {{
      let opened = false, errored = false;
      const ws = new WebSocket(u);
      ws.onopen = () => {{ opened = true; ws.close(); }};
      ws.onerror = () => {{ errored = true; }};
      ws.onclose = (ev) => resolve({{ opened, errored, wasClean: ev.wasClean }});
    }});
  }}
  const [rClosed, r200, rBadAccept] = await Promise.all([
    probeFailWs("ws://127.0.0.1:{closed_port}/ws"),
    probeFailWs("{srv.ws_origin}/ws-http-200"),
    probeFailWs("{srv.ws_origin}/ws-bad-accept"),
  ]);
  const allRejected = !rClosed.opened && rClosed.errored && !rClosed.wasClean
    && !r200.opened && r200.errored && !r200.wasClean
    && !rBadAccept.opened && rBadAccept.errored && !rBadAccept.wasClean;
  document.title = (badUrlThrew && allRejected) ? "WS_HANDSHAKE_ERRORS_OK" : ("BAD:" + JSON.stringify([badUrlThrew, rClosed, r200, rBadAccept]));
}})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F09_04", wait_load_ms=5000)
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "WS_HANDSHAKE_ERRORS_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "WebSocket invalid URL throw, closed port, HTTP 200, and invalid Sec-WebSocket-Accept rejection verified"
    finally:
        if owns:
            srv.stop()


def _t2_f09_05(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f09/corrupt-gzip",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#0f766e;color:#fff">BAD_GZ</div>
<script>
(async () => {
  let badFmt = false;
  try { new CompressionStream("lz4-nonexistent"); } catch (e) { badFmt = e instanceof TypeError; }
  let corruptRej = false;
  try {
    const ds = new DecompressionStream("gzip");
    const writer = ds.writable.getWriter();
    const reader = ds.readable.getReader();
    const readProm = (async () => { while (true) { const { done } = await reader.read(); if (done) break; } })();
    await writer.write(new Uint8Array([0x1f, 0x8b, 0x08, 0xff, 0x00, 0xde, 0xad]));
    await writer.close();
    await readProm;
  } catch (e) {
    corruptRej = e instanceof TypeError;
  }
  document.title = (badFmt && corruptRej) ? "CORRUPT_STREAM_REJECTED_OK" : ("BAD:" + badFmt + ":" + corruptRej);
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F09_05")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "CORRUPT_STREAM_REJECTED_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Unsupported CompressionStream format & corrupt gzip DecompressionStream TypeError verified"
    finally:
        if owns:
            srv.stop()


def _t2_f09_06(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        td_url = srv.register_route(
            "/f09/textdecoder-fatal",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#4f46e5;color:#fff">TD_FATAL</div>
<script>
function throwsType(fn) {
  try { fn(); return false; } catch (e) { return e instanceof TypeError; }
}
const u8Fatal = throwsType(() => new TextDecoder("utf-8", { fatal: true }).decode(new Uint8Array([0xff, 0xfe])));
const u8Repl = new TextDecoder("utf-8", { fatal: false }).decode(new Uint8Array([0xff])).includes("\uFFFD");
const sjisFatal = throwsType(() => new TextDecoder("shift_jis", { fatal: true }).decode(new Uint8Array([0x81, 0x20])));
const sjisRepl = new TextDecoder("shift_jis", { fatal: false }).decode(new Uint8Array([0x81, 0x20])).includes("\uFFFD");
const euckrFatal = throwsType(() => new TextDecoder("euc-kr", { fatal: true }).decode(new Uint8Array([0xc8, 0x20])));
document.title = (u8Fatal && u8Repl && sjisFatal && sjisRepl && euckrFatal) ? "TD_FATAL_AND_FFFD_OK" : "BAD_TD_FATAL";
</script></body></html>""",
            },
        )
        r_td = run_waterscape_verify(ctx, td_url, test_id="T2_F09_06_td")
        ensure(r_td.ok and r_td.dump_json.get("title") == "TD_FATAL_AND_FFFD_OK", f"TD fatal failed: {r_td.dump_json.get('title')!r}")

        r_meta = run_waterscape_verify(ctx, f"{srv.main_origin}/charset/shift_jis?via=meta", test_id="T2_F09_06_meta")
        ensure(r_meta.ok and r_meta.dump_json.get("title") == "こんにちは世界 2026", f"Meta Shift_JIS failed: {r_meta.dump_json.get('title')!r}")

        utf8_override_html = (
            '<!DOCTYPE html><html><head><meta charset="Shift_JIS"><title>HTTP_UTF8_WINS_한글</title></head>'
            "<body><div style='width:140px;height:50px;background:#16a34a;color:#fff'>UTF8</div></body></html>"
        ).encode("utf-8")
        ov_url = srv.register_route(
            "/f09/http-overrides-meta",
            {"status": 200, "content_type": "text/html; charset=UTF-8", "body": utf8_override_html},
        )
        r_ov = run_waterscape_verify(ctx, ov_url, test_id="T2_F09_06_override")
        ensure(r_ov.ok and r_ov.dump_json.get("title") == "HTTP_UTF8_WINS_한글", f"HTTP charset precedence failed: {r_ov.dump_json.get('title')!r}")
        return "TextDecoder fatal TypeError vs U+FFFD replacement & HTTP-over-meta charset precedence verified"
    finally:
        if owns:
            srv.stop()


# =============================================================================
# Feature F10: Constructable Stylesheets & Web Animations API (Tier 2)
# =============================================================================

def _t2_f10_01(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f10/anim-cancel",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title>
<style>#box { width: 80px; height: 50px; background: #2563eb; }</style></head>
<body><div id="box">C</div>
<script>
(async () => {
  const box = document.getElementById("box");
  const anim = box.animate([{ width: "100px" }, { width: "400px" }], { duration: 500, fill: "forwards" });
  let abortName = "NONE";
  const finCatch = anim.finished.then(() => "RESOLVED", (err) => { abortName = err ? err.name : "NULL"; return "REJECTED"; });
  await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
  anim.cancel();
  const outcome = await finCatch;
  const revertedW = getComputedStyle(box).width;
  const ok = anim.playState === "idle" && outcome === "REJECTED" && abortName === "AbortError" && revertedW === "80px";
  document.title = ok ? "ANIM_CANCEL_ABORT_OK" : ("BAD:" + [anim.playState, outcome, abortName, revertedW].join(","));
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F10_01")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "ANIM_CANCEL_ABORT_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Animation.cancel() idle playState, AbortError rejection & base style revert verified"
    finally:
        if owns:
            srv.stop()


def _t2_f10_02(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f10/anim-seek-finish",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div id="box" style="width:100px;height:50px;background:#059669">S</div>
<script>
(async () => {
  const box = document.getElementById("box");
  const anim = box.animate([{ width: "100px" }, { width: "300px" }], { duration: 2000, fill: "forwards", easing: "linear" });
  anim.pause();
  anim.currentTime = 1000;
  const pausedState = anim.playState;
  const midW = parseFloat(getComputedStyle(box).width);
  anim.finish();
  const finState = anim.playState;
  const endW = Math.round(parseFloat(getComputedStyle(box).width));
  await anim.finished;
  const ok = pausedState === "paused" && Math.abs(midW - 200) <= 3 && finState === "finished" && endW === 300;
  document.title = ok ? "ANIM_PAUSE_SEEK_FINISH_OK" : ("BAD:" + [pausedState, midW, finState, endW].join(","));
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F10_02")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "ANIM_PAUSE_SEEK_FINISH_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Animation.pause(), currentTime scrubbing (50% -> 200px), and finish() verified"
    finally:
        if owns:
            srv.stop()


def _t2_f10_03(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        cursor = srv.mark_cursor()
        srv.register_route(
            "/f10/evil.css",
            {"status": 200, "content_type": "text/css", "body": "#ok { width: 999px !important; }\n"},
        )
        url = srv.register_route(
            "/f10/import-and-nonconstructed",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title>
<style id="decl">#ok { height: 50px; background: #7c3aed; }</style></head>
<body><div id="ok">GUARD</div>
<script>
const cs = new CSSStyleSheet();
cs.replaceSync('@import url("/f10/evil.css"); #ok { width: 177px; }');
document.adoptedStyleSheets = [cs];
const w = Math.round(document.getElementById("ok").getBoundingClientRect().width);
let notAllowed = false;
try {
  document.styleSheets[0].replaceSync("#x {}");
} catch (e) {
  notAllowed = (e instanceof DOMException) && e.name === "NotAllowedError";
}
document.title = (w === 177 && notAllowed) ? "CONSTRUCTED_GUARD_OK" : ("BAD:" + w + ":" + notAllowed);
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F10_03")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "CONSTRUCTED_GUARD_OK", f"Got: {wres.dump_json.get('title')!r}")
        evil_hits = srv.get_traces(since=cursor, path_prefix="/f10/evil.css")
        ensure(len(evil_hits) == 0, f"@import inside constructable stylesheet must not fetch, got {len(evil_hits)} hits")
        return "@import ignored in constructable stylesheet & NotAllowedError on non-constructed replaceSync verified"
    finally:
        if owns:
            srv.stop()


def _t2_f10_04(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f10/cascade-order",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title>
<style>#p { width: 111px; height: 50px; background: #0284c7; }</style></head>
<body><div id="p">ORDER</div>
<script>
const p = document.getElementById("p");
const s1 = new CSSStyleSheet();
s1.replaceSync("#p { width: 222px; }");
const s2 = new CSSStyleSheet();
s2.replaceSync("#p { width: 333px; }");
document.adoptedStyleSheets = [s1, s2];
const wAdopted = Math.round(p.getBoundingClientRect().width);
document.adoptedStyleSheets = [s2, s1];
const wSwapped = Math.round(p.getBoundingClientRect().width);
document.adoptedStyleSheets = [];
const wCleared = Math.round(p.getBoundingClientRect().width);
const ok = wAdopted === 333 && wSwapped === 222 && wCleared === 111;
document.title = ok ? "CASCADE_ADOPTED_ORDER_OK" : ("BAD:" + [wAdopted, wSwapped, wCleared].join(","));
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F10_04")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "CASCADE_ADOPTED_ORDER_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Cascade order (document.styleSheets < adoptedStyleSheets[0] < adoptedStyleSheets[1]) & clear verified"
    finally:
        if owns:
            srv.stop()


def _t2_f10_05(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f10/prop-indexed-keyframes",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div id="el" style="width:100px;height:50px;background:#e11d48">KF</div>
<script>
(async () => {
  const el = document.getElementById("el");
  let negThrew = false;
  try {
    el.animate([{ opacity: 0 }, { opacity: 1 }], { duration: -10 });
  } catch (e) {
    negThrew = e instanceof TypeError;
  }
  const anim = el.animate({ width: ["100px", "200px", "260px"], opacity: [0.3, 0.7, 1.0] }, 120);
  await anim.finished;
  const ok = negThrew && anim.playState === "finished";
  document.title = ok ? "PROP_KEYFRAMES_AND_NEGDUR_OK" : ("BAD:" + negThrew + ":" + anim.playState);
})();
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F10_05")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "PROP_KEYFRAMES_AND_NEGDUR_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Property-indexed keyframes object syntax & negative duration TypeError verified"
    finally:
        if owns:
            srv.stop()


# =============================================================================
# Feature F11: Modern JS Heap/Turn Scaling & Removal of Deceptive Facades (Tier 2)
# =============================================================================

def _t2_f11_01(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        srv.register_route(
            "/f11/not-a-video.html",
            {"status": 200, "content_type": "text/html; charset=utf-8", "body": "<!DOCTYPE html><html><body>HTML error page</body></html>"},
        )
        url = srv.register_route(
            "/f11/video-honest",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title>
<style>body { margin: 0; background: #ffffff; } #v { display: block; width: 640px; height: 360px; background: #f1f5f9; }</style></head>
<body>
<video id="v" width="640" height="360" autoplay src="/f11/not-a-video.html"></video>
<script>
const v = document.getElementById("v");
v.addEventListener("error", () => {
  const ok = v.error !== null && v.readyState === 0;
  document.title = ok ? "HONEST_VIDEO_ERR_OK" : ("BAD_STATE:" + v.readyState);
});
</script></body></html>""",
            },
        )
        for conf_flag in (False, True):
            wres = run_waterscape_verify(ctx, url, test_id=f"T2_F11_01_{int(conf_flag)}", conformance=conf_flag, save_content_png=True)
            ensure(wres.ok, f"conformance={conf_flag} guard failure: {wres.guard_failures}")
            ensure(wres.dump_json.get("title") == "HONEST_VIDEO_ERR_OK", f"conformance={conf_flag} video did not fire error: {wres.dump_json.get('title')!r}")
            ensure(wres.content_png is not None, "Missing content PNG")
            st = wres.content_png.region_stats(10, 10, 620, 300)
            ensure(
                st["saturated_pixels"] == 0,
                f"Fake procedural video visualizer detected (saturated_pixels={st['saturated_pixels']}, max_chroma={st['max_chroma']}) in conformance={conf_flag}!",
            )
        return "<video src='/not-a-video.html'> fired honest error with 0 procedural visualizer pixels in both modes"
    finally:
        if owns:
            srv.stop()


def _t2_f11_02(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f11/honest-caps",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#2563eb;color:#fff">CAPS</div>
<script>
const v = document.createElement("video");
const bogusCodec = v.canPlayType("video/x-totally-fake-codec-9999");
const c = document.createElement("canvas");
const wgl2 = c.getContext("webgl2");
const wgl2Honest = (wgl2 === null) || (typeof WebGL2RenderingContext === "function" && (wgl2 instanceof WebGL2RenderingContext));
const gpuHonest = (navigator.gpu === undefined) || (navigator.gpu !== null && typeof navigator.gpu.requestAdapter === "function");
const wtHonest = (window.WebTransport === undefined) || (typeof window.WebTransport === "function");
const swHonest = (navigator.serviceWorker === undefined) || (typeof navigator.serviceWorker.register === "function");
const ok = bogusCodec === "" && wgl2Honest && gpuHonest && wtHonest && swHonest;
document.title = ok ? "HONEST_CAPABILITY_PROBES_OK" : ("BAD:" + JSON.stringify({bogusCodec, wgl2Honest, gpuHonest, wtHonest, swHonest}));
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F11_02")
        ensure(wres.ok, f"Guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "HONEST_CAPABILITY_PROBES_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Honest capability probing verified (bogus canPlayType === '', zero fake stub objects)"
    finally:
        if owns:
            srv.stop()


def _t2_f11_03(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f11/infinite-loop-guard",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body>
<script>while (true) {}</script>
<div id="after" style="width:180px;height:60px;background:#16a34a;color:#fff">AFTER_HANG</div>
<script>
if (document.getElementById("after")) {
  document.title = "RECOVERED_AFTER_HANG";
}
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(
            ctx,
            url,
            test_id="T2_F11_03",
            wait_load_ms=10000,
            timeout_sec=16.0,
            allow_console_errors=True,
        )
        ensure(not wres.timed_out and wres.returncode == 0, f"Browser hung or crashed on infinite loop: rc={wres.returncode}, timed_out={wres.timed_out}")
        ensure(wres.dump_json.get("ready_state") == "complete", f"ready_state={wres.dump_json.get('ready_state')!r}")
        ensure(wres.dump_json.get("title") == "RECOVERED_AFTER_HANG", f"Parser/script after loop did not run: {wres.dump_json.get('title')!r}")
        clog = str(wres.dump_json.get("console_log", ""))
        ensure("interrupted" in clog.lower() or "timeout" in clog.lower(), f"Expected interrupt log in console_log, got: {clog!r}")
        return f"Infinite while(true) loop interrupted cleanly in {wres.duration_ms:.0f}ms and subsequent DOM parsed"
    finally:
        if owns:
            srv.stop()


def _t2_f11_04(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f11/oom-bomb",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:180px;height:60px;background:#dc2626;color:#fff">OOM_GUARD</div>
<script>
let bomb = [];
let caughtName = "";
try {
  for (let i = 0; i < 200; i++) {
    bomb.push(new Uint8Array(4 * 1024 * 1024));
  }
} catch (e) {
  caughtName = e ? String(e.name || e.message || "ERR") : "ERR";
}
bomb = null;
document.title = caughtName ? ("CAUGHT_OOM:" + caughtName) : "UNCAPPED_ALLOC_BAD";
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(
            ctx,
            url,
            test_id="T2_F11_04",
            profile="modern",
            wait_load_ms=8000,
            timeout_sec=18.0,
            allow_console_errors=True,
        )
        ensure(not wres.timed_out and wres.returncode == 0, f"Browser crashed/killed on >300MB bomb: rc={wres.returncode}")
        ensure(wres.dump_json.get("ready_state") == "complete", f"ready_state={wres.dump_json.get('ready_state')!r}")
        title = str(wres.dump_json.get("title", ""))
        clog = str(wres.dump_json.get("console_log", ""))
        ensure(
            title.startswith("CAUGHT_OOM:") or ("Out of memory" in clog) or ("RangeError" in clog),
            f"Runaway 800MB allocation was neither caught nor bounded: title={title!r}, log={clog[:160]!r}",
        )
        return f"Browser WebView enforced 256MB heap ceiling cleanly without crash ({title or 'console OOM'})"
    finally:
        if owns:
            srv.stop()


def _t2_f11_05(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        ok_url = srv.register_route(
            "/f11/min-ok",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#059669;color:#fff">MIN2MB</div>
<script>
const buf = new Uint8Array(2 * 1024 * 1024);
buf[0] = 7; buf[buf.length - 1] = 9;
document.title = "MIN_2MB_OK:" + (buf[0] + buf[buf.length - 1]);
</script></body></html>""",
            },
        )
        r_ok = run_waterscape_verify(ctx, ok_url, test_id="T2_F11_05_ok", profile="minimum")
        ensure(r_ok.ok and r_ok.dump_json.get("title") == "MIN_2MB_OK:16", f"Minimum profile 2MB failed: {r_ok.dump_json.get('title')!r}")

        bomb_url = srv.register_route(
            "/f11/min-48mb",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body><div style="width:160px;height:60px;background:#ea580c;color:#fff">MIN_CAP</div>
<script>
let keep = [];
let capped = false;
try {
  for (let i = 0; i < 48; i++) keep.push(new Uint8Array(1048576));
} catch (e) {
  capped = true;
}
keep = null;
document.title = capped ? "MIN_CAPPED_OK" : "MIN_UNCAPPED";
</script></body></html>""",
            },
        )
        r_cap = run_waterscape_verify(ctx, bomb_url, test_id="T2_F11_05_cap", profile="minimum", allow_console_errors=True)
        ensure(r_cap.returncode == 0 and not r_cap.timed_out, f"Minimum profile crashed: rc={r_cap.returncode}")
        clog = str(r_cap.dump_json.get("console_log", ""))
        ensure(
            r_cap.dump_json.get("title") == "MIN_CAPPED_OK" or "Out of memory" in clog or "RangeError" in clog,
            f"Expected --profile minimum to enforce lower legacy heap cap, got title={r_cap.dump_json.get('title')!r}",
        )
        return "--profile minimum legacy memory cap distinction verified cleanly"
    finally:
        if owns:
            srv.stop()


def _t2_f11_06(ctx: RunnerContext) -> str:
    srv, owns = _ensure_fixture(ctx)
    try:
        url = srv.register_route(
            "/f11/spa-tiny-rerender",
            {
                "status": 200,
                "content_type": "text/html; charset=utf-8",
                "body": """<!DOCTYPE html><html><head><meta charset="utf-8"><title>INIT</title></head>
<body>
<div id="__next"><div id="root"><p>OLD_SSR_SNAPSHOT_TEXT_12345_VERY_LONG_INITIAL_CONTENT</p></div></div>
<script>
self.__next_f = self.__next_f || [];
self.__next_f.push([1, "ssr_payload"]);
document.getElementById("root").innerHTML = "<button id='tinyBtn' style='width:120px;height:40px;background:#2563eb;color:#fff'>TINY</button>";
window.addEventListener("load", () => {
  setTimeout(() => {
    const html = document.getElementById("root").innerHTML;
    const intact = html.includes("TINY") && !document.body.innerHTML.includes("OLD_SSR_SNAPSHOT_TEXT_12345");
    document.title = intact ? "SPA_TINY_NOT_CLOBBERED_OK" : ("CLOBBERED_BY_SSR:" + html.slice(0, 80));
  }, 30);
});
</script></body></html>""",
            },
        )
        wres = run_waterscape_verify(ctx, url, test_id="T2_F11_06", conformance=False)
        ensure(wres.ok, f"Default-mode guard failures: {wres.guard_failures}")
        ensure(wres.dump_json.get("title") == "SPA_TINY_NOT_CLOBBERED_OK", f"Got: {wres.dump_json.get('title')!r}")
        return "Default-mode SPA client re-render (<16 chars) preserved without stale SSR snapshot restore"
    finally:
        if owns:
            srv.stop()


TESTS: list[TestCaseSpec] = [
    # F01 (5)
    _spec("T2_F01_01", "F01", "Rootless execution static & poisoned-sudo/apt shim audit", _t2_f01_01),
    _spec("T2_F01_02", "F01", "Zero hardcoded secrets, OAuth tokens, or plugin token paths audit", _t2_f01_02),
    _spec("T2_F01_03", "F01", "Ubuntu case $- in guard strip & woken /home pre-existing state simulation", _t2_f01_03),
    _spec("T2_F01_04", "F01", "Fresh VM missing-.bashrc & closed-stdin non-interactive stress", _t2_f01_04),
    _spec("T2_F01_05", "F01", "Missing-file & corrupt-JSON F01 validator structured FAIL check", _t2_f01_05),
    # F02 (5)
    _spec("T2_F02_01", "F02", "Constant pool -0 vs +0, NaN, '0' vs 0 identity across 40k hash resizes", _t2_f02_01),
    _spec("T2_F02_02", "F02", "66,000 locals & constants crossing 32,768 and 65,536 uint16 boundaries", _t2_f02_02),
    _spec("T2_F02_03", "F02", "Duplicate lexical declaration early SyntaxError (exit 2) at slot #34,500", _t2_f02_03),
    _spec("T2_F02_04", "F02", "TDZ ReferenceError & const assignment TypeError at slot #33,333", _t2_f02_04),
    _spec("T2_F02_05", "F02", ">120 KB bytecode forward/backward/break jump offset integrity", _t2_f02_05),
    # F03 (5)
    _spec("T2_F03_01", "F03", "10.5 MB kevlar-scale bundle within 256 MB heap & 512 MB RSS budget", _t2_f03_01, related_features=("F02",)),
    _spec("T2_F03_02", "F03", "Graceful OOM enforcement (exit 1, zero crash) under --heap-limit 8", _t2_f03_02),
    _spec("T2_F03_03", "F03", "Deterministic execution deadline interrupt (--max-time-ms 150)", _t2_f03_03),
    _spec("T2_F03_04", "F03", "GC C-stack root safety across Proxy get, Array.sort & JSON.stringify", _t2_f03_04),
    _spec("T2_F03_05", "F03", "FinalizationRegistry.prototype.unregister & same-turn WeakRef stability", _t2_f03_05),
    # F04 (5)
    _spec("T2_F04_01", "F04", "Nested throws wrapped into SuppressedError(disposeErr, priorErr) chain", _t2_f04_01),
    _spec("T2_F04_02", "F04", "null/undefined no-op vs invalid resource TypeError & prior cleanup", _t2_f04_02),
    _spec("T2_F04_03", "F04", "ReferenceError on disposed/moved DisposableStack & eager callable check", _t2_f04_03),
    _spec("T2_F04_04", "F04", "using grammar restrictions (SyntaxError) & const-like immutability", _t2_f04_04),
    _spec("T2_F04_05", "F04", "await using sync throw + rejected Promise -> SuppressedError chain", _t2_f04_05),
    # F05 (5)
    _spec("T2_F05_01", "F05", "Math.sumPrecise Infinity/NaN, non-number TypeError & IteratorClose", _t2_f05_01),
    _spec("T2_F05_02", "F05", "Math.sumPrecise [MAX_VALUE, MAX_VALUE, -MAX_VALUE] overflow boundary", _t2_f05_02),
    _spec("T2_F05_03", "F05", "Map/WeakMap getOrInsert* brand checks, Symbol.for & eager IsCallable", _t2_f05_03),
    _spec("T2_F05_04", "F05", "Intl.DurationFormat zero, negative, mixed-sign RangeError & fractional", _t2_f05_04),
    _spec("T2_F05_05", "F05", "IEEE-754 subnormal, round-to-even & Math.f16round precision", _t2_f05_05),
    # F06 (5)
    _spec("T2_F06_01", "F06", "Exact power-of-two URL lengths (2047..32768) in fetch() & XMLHttpRequest", _t2_f06_01),
    _spec("T2_F06_02", "F06", "Malformed URL scheme/host/port synchronous & async TypeError rejection", _t2_f06_02),
    _spec("T2_F06_03", "F06", "28 KB data:text/css and data:image/png base64 URI decoding", _t2_f06_03),
    _spec("T2_F06_04", "F06", "20 KB UTF-8 percent-encoded URL & #fragment wire-stripping", _t2_f06_04),
    _spec("T2_F06_05", "F06", "128 KB extreme URL memory-safety stress without crash", _t2_f06_05),
    # F07 (5)
    _spec("T2_F07_01", "F07", "HttpOnly cookie invisible to document.cookie & immune to JS overwrite", _t2_f07_01),
    _spec("T2_F07_02", "F07", "Cross-invocation --private cookie isolation & zero disk persistence", _t2_f07_02),
    _spec("T2_F07_03", "F07", "Host-only cookie isolation (127.0.0.1 vs localhost) & bad Domain rejection", _t2_f07_03),
    _spec("T2_F07_04", "F07", "Same-name cookie update & '=' characters preserved inside cookie value", _t2_f07_04),
    _spec("T2_F07_05", "F07", "1,024-cookie jar ceiling & 1,074-cookie eviction without buffer overflow", _t2_f07_05),
    # F08 (5)
    _spec("T2_F08_01", "F08", "Dynamic <script> & <link> HTTP 404 fire error event without SyntaxError", _t2_f08_01),
    _spec("T2_F08_02", "F08", "document.currentScript === null in ES modules, microtasks & timers", _t2_f08_02),
    _spec("T2_F08_03", "F08", "Nested synchronous currentScript stack push/pop restoration", _t2_f08_03),
    _spec("T2_F08_04", "F08", "Deferred s.src on connected <script> vs detached <script> non-fetch", _t2_f08_04),
    _spec("T2_F08_05", "F08", "Dynamic <script type='module' src> & 'already started' re-insert guard", _t2_f08_05),
    # F09 (6)
    _spec("T2_F09_01", "F09", "Web IDL Illegal invocation TypeError brand checks & XML CDATA/PI nodes", _t2_f09_01),
    _spec("T2_F09_02", "F09", "<script nomodule src> must neither fetch nor execute", _t2_f09_02),
    _spec("T2_F09_03", "F09", "URLPattern invalid pattern TypeError & relative baseURL resolution", _t2_f09_03),
    _spec("T2_F09_04", "F09", "WebSocket bad URL, closed port, HTTP 200 & bad Sec-WebSocket-Accept rejection", _t2_f09_04),
    _spec("T2_F09_05", "F09", "Corrupt gzip DecompressionStream & unsupported CompressionStream TypeError", _t2_f09_05),
    _spec("T2_F09_06", "F09", "TextDecoder fatal TypeError vs U+FFFD & HTTP-over-meta charset precedence", _t2_f09_06),
    # F10 (5)
    _spec("T2_F10_01", "F10", "Animation.cancel() AbortError rejection & base style revert", _t2_f10_01),
    _spec("T2_F10_02", "F10", "Animation.pause(), currentTime scrubbing & synchronous finish()", _t2_f10_02),
    _spec("T2_F10_03", "F10", "@import ignored in constructable sheet & NotAllowedError on <style>.sheet", _t2_f10_03),
    _spec("T2_F10_04", "F10", "Cascade order (document.styleSheets < adoptedStyleSheets) & clear revert", _t2_f10_04),
    _spec("T2_F10_05", "F10", "Property-indexed keyframes object syntax & negative duration TypeError", _t2_f10_05),
    # F11 (6)
    _spec("T2_F11_01", "F11", "<video src='/not-a-video.html'> fires error with 0 fake visualizer pixels", _t2_f11_01),
    _spec("T2_F11_02", "F11", "Honest capability probing (bogus canPlayType === '', zero fake stubs)", _t2_f11_02),
    _spec("T2_F11_03", "F11", "Infinite while(true) loop turn deadline recovery without browser hang", _t2_f11_03),
    _spec("T2_F11_04", "F11", "Browser WebView >300 MB allocation bomb caught by 256 MB heap ceiling", _t2_f11_04),
    _spec("T2_F11_05", "F11", "--profile minimum legacy low-RAM heap cap distinction", _t2_f11_05),
    _spec("T2_F11_06", "F11", "Default-mode SPA tiny re-render (<16 chars) not clobbered by SSR restore", _t2_f11_06),
]


def get_tests() -> list[TestCaseSpec]:
    return list(TESTS)


get_tier2_f01_f11_tests = get_tests
