#!/usr/bin/env python3
"""
ArcradientJS 1.0 - Automated Test262 & Built-in ES2026/ESM/Intl Conformance Runner
tools/run_test262.py

Supports:
1. Official TC39 Test262 trees (`--suite /path/to/test262/test`) with full YAML
   frontmatter parsing (`flags`, `negative` parse/early/resolution/runtime phases,
   `includes`, `async` `$DONE` verification, strict + sloppy dual execution).
2. Built-in 130+ test self-contained ES2026 + Annex B + ES Modules + Top-Level Await
   + ECMA-402 Intl + Cross-Realm `$262` + GC/WeakMap/FinalizationRegistry suite
   when `--suite` is omitted.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import fnmatch
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

FRONTMATTER_RE = re.compile(r"/\*---(.*?)---\*/", re.DOTALL)

BUILTIN_HARNESS_FILES: Dict[str, str] = {
    "sta.js": r"""
function Test262Error(message) {
  this.message = message === undefined ? "" : String(message);
  this.name = "Test262Error";
}
Test262Error.prototype = Object.create(Error.prototype);
Test262Error.prototype.constructor = Test262Error;
Test262Error.prototype.toString = function () {
  return this.message ? "Test262Error: " + this.message : "Test262Error";
};
Test262Error.thrower = function (message) {
  throw new Test262Error(message);
};
function $DONOTEVALUATE() {
  throw "Test262: This statement should not be evaluated.";
}
""",
    "assert.js": r"""
function assert(mustBeTrue, message) {
  if (mustBeTrue === true) return;
  throw new Test262Error(message || ("Expected true but got " + String(mustBeTrue)));
}
assert._isSameValue = function (a, b) {
  if (a === b) return a !== 0 || 1 / a === 1 / b;
  return a !== a && b !== b;
};
assert.sameValue = function (actual, expected, message) {
  if (assert._isSameValue(actual, expected)) return;
  throw new Test262Error(
    (message ? message + " - " : "") +
    "Expected SameValue(" + String(actual) + ", " + String(expected) + ") to be true"
  );
};
assert.notSameValue = function (actual, unexpected, message) {
  if (!assert._isSameValue(actual, unexpected)) return;
  throw new Test262Error(
    (message ? message + " - " : "") +
    "Expected SameValue(" + String(actual) + ", " + String(unexpected) + ") to be false"
  );
};
assert.throws = function (expectedErrorConstructor, func, message) {
  if (typeof func !== "function") {
    throw new Test262Error("assert.throws requires a function");
  }
  var threw = false;
  var caught = null;
  try {
    func();
  } catch (e) {
    threw = true;
    caught = e;
  }
  if (!threw) {
    throw new Test262Error((message ? message + ": " : "") + "Expected function to throw " + expectedErrorConstructor.name);
  }
  if (caught === null || typeof caught !== "object" || caught.constructor !== expectedErrorConstructor) {
    if (!(caught instanceof expectedErrorConstructor)) {
      throw new Test262Error(
        (message ? message + ": " : "") +
        "Expected " + expectedErrorConstructor.name + " but caught " +
        (caught && caught.name ? caught.name : String(caught))
      );
    }
  }
};
""",
    "doneprintHandle.js": r"""
function $DONE(error) {
  if (error) {
    if (typeof error === "object" && error !== null && "name" in error) {
      print("Test262:AsyncTestFailure:" + error.name + ": " + error.message);
    } else {
      print("Test262:AsyncTestFailure:Test262Error: " + String(error));
    }
  } else {
    print("Test262:AsyncTestComplete");
  }
}
""",
    "compareArray.js": r"""
assert.compareArray = function (actual, expected, message) {
  assert.sameValue(actual.length, expected.length, (message || "") + " (length)");
  for (var i = 0; i < expected.length; i++) {
    assert.sameValue(actual[i], expected[i], (message || "") + " (index " + i + ")");
  }
};
""",
    "propertyHelper.js": r"""
function verifyProperty(obj, name, desc) {
  var d = Object.getOwnPropertyDescriptor(obj, name);
  assert(d !== undefined, "Expected own property " + String(name));
  if ("value" in desc) assert.sameValue(d.value, desc.value, "value of " + String(name));
  if ("writable" in desc) assert.sameValue(d.writable, desc.writable, "writable of " + String(name));
  if ("enumerable" in desc) assert.sameValue(d.enumerable, desc.enumerable, "enumerable of " + String(name));
  if ("configurable" in desc) assert.sameValue(d.configurable, desc.configurable, "configurable of " + String(name));
}
""",
}


@dataclass
class TestMeta:
    flags: List[str] = field(default_factory=list)
    includes: List[str] = field(default_factory=list)
    features: List[str] = field(default_factory=list)
    neg_phase: Optional[str] = None
    neg_type: Optional[str] = None


@dataclass
class TestCase:
    test_id: str
    category: str
    file_path: str
    meta: TestMeta
    strict_mode: bool


@dataclass
class TestResult:
    test_id: str
    category: str
    strict_mode: bool
    passed: bool
    duration_ms: float
    reason: str = ""


def _parse_inline_list(raw: str) -> List[str]:
    raw = raw.strip()
    if raw.startswith("[") and raw.endswith("]"):
        inner = raw[1:-1].strip()
        if not inner:
            return []
        return [x.strip().strip("'\"") for x in inner.split(",") if x.strip()]
    return [raw.strip("'\"")] if raw else []


def parse_frontmatter(source: str) -> TestMeta:
    meta = TestMeta()
    m = FRONTMATTER_RE.search(source)
    if not m:
        return meta
    block = m.group(1)
    section: Optional[str] = None
    for raw_line in block.splitlines():
        stripped = raw_line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(raw_line) - len(raw_line.lstrip(" "))
        if indent == 0 and ":" in stripped:
            key, val = stripped.split(":", 1)
            key = key.strip()
            val = val.strip()
            section = key
            if key == "flags" and val:
                meta.flags.extend(_parse_inline_list(val))
                section = None
            elif key == "includes" and val:
                meta.includes.extend(_parse_inline_list(val))
                section = None
            elif key == "features" and val:
                meta.features.extend(_parse_inline_list(val))
                section = None
        elif indent > 0 and section:
            if stripped.startswith("-"):
                item = stripped[1:].strip().strip("'\"")
                if section == "flags":
                    meta.flags.append(item)
                elif section == "includes":
                    meta.includes.append(item)
                elif section == "features":
                    meta.features.append(item)
            elif section == "negative" and ":" in stripped:
                nk, nv = stripped.split(":", 1)
                nk = nk.strip()
                nv = nv.strip().strip("'\"")
                if nk == "phase":
                    meta.neg_phase = nv
                elif nk == "type":
                    meta.neg_type = nv
    return meta


def expand_test_variants(test_id: str, category: str, file_path: str, source: str) -> List[TestCase]:
    meta = parse_frontmatter(source)
    flags = set(meta.flags)
    if "module" in flags or "raw" in flags or "noStrict" in flags:
        return [TestCase(test_id, category, file_path, meta, False)]
    if "onlyStrict" in flags:
        return [TestCase(test_id, category, file_path, meta, True)]
    return [
        TestCase(test_id, category, file_path, meta, False),
        TestCase(test_id, category, file_path, meta, True),
    ]


def ensure_cli_binary(repo_root: Path, requested_cli: Optional[str]) -> Path:
    if requested_cli:
        p = Path(requested_cli).resolve()
        if p.is_file() and os.access(p, os.X_OK):
            return p
    for cand in [
        repo_root / "build" / "arcr_js",
        repo_root / "build" / "arcr_js_cli",
        repo_root / "arcr_js",
        repo_root / "arcr_js_cli",
        Path("/tmp/arcr_js_cli"),
    ]:
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand.resolve()

    out_bin = Path("/tmp/arcr_js_cli")
    obj_dir = Path(tempfile.mkdtemp(prefix="ajs_cli_build_"))
    try:
        c_files = (
            sorted(glob.glob(str(repo_root / "js" / "vm" / "*.c")))
            + sorted(glob.glob(str(repo_root / "js" / "builtins" / "*.c")))
            + sorted(glob.glob(str(repo_root / "js" / "modules" / "*.c")))
            + [str(repo_root / "tools" / "arcr_js_cli.c")]
        )
        objs: List[str] = []
        cc = os.environ.get("CC", "cc")
        for cf in c_files:
            obj = str(obj_dir / (Path(cf).stem + ".o"))
            cmd = [
                cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Wvla",
                "-O2", "-Werror", "-D_POSIX_C_SOURCE=200809L",
                f"-I{repo_root}", "-c", cf, "-o", obj,
            ]
            subprocess.run(cmd, check=True)
            objs.append(obj)
        subprocess.run([cc, *objs, "-lm", "-o", str(out_bin)], check=True)
        return out_bin
    finally:
        shutil.rmtree(obj_dir, ignore_errors=True)


def run_single_case(cli_bin: str, harness_dir: Path, tc: TestCase, timeout_sec: float) -> TestResult:
    t0 = time.perf_counter()
    flags = set(tc.meta.flags)
    cmd: List[str] = [cli_bin, "--test262", "--max-time-ms", str(int(timeout_sec * 1000))]

    if "raw" not in flags:
        includes = ["sta.js", "assert.js"]
        if "async" in flags and "doneprintHandle.js" not in tc.meta.includes:
            includes.append("doneprintHandle.js")
        for inc in tc.meta.includes:
            if inc not in includes:
                includes.append(inc)
        for inc in includes:
            hp = harness_dir / inc
            if hp.is_file():
                cmd.extend(["--harness", str(hp)])

    if "module" in flags:
        cmd.append("--module")
    elif tc.strict_mode:
        cmd.append("--strict")

    cmd.append(tc.file_path)

    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout_sec + 1.0,
        )
    except subprocess.TimeoutExpired:
        dt = (time.perf_counter() - t0) * 1000.0
        return TestResult(tc.test_id, tc.category, tc.strict_mode, False, dt, "Timeout expired")

    dt = (time.perf_counter() - t0) * 1000.0
    rc = proc.returncode
    out = proc.stdout or ""
    err = proc.stderr or ""
    combined_err = (err + "\n" + out).strip()

    if tc.meta.neg_phase:
        phase = tc.meta.neg_phase
        err_type = tc.meta.neg_type or "Error"
        if rc == 0:
            return TestResult(tc.test_id, tc.category, tc.strict_mode, False, dt,
                              f"Expected negative ({phase}/{err_type}) but exited 0")
        if phase in ("parse", "early", "resolution") and rc != 2:
            return TestResult(tc.test_id, tc.category, tc.strict_mode, False, dt,
                              f"Expected exit code 2 for phase={phase}, got rc={rc}: {combined_err[:160]}")
        if phase == "runtime" and rc != 1:
            return TestResult(tc.test_id, tc.category, tc.strict_mode, False, dt,
                              f"Expected exit code 1 for phase=runtime, got rc={rc}: {combined_err[:160]}")
        if err_type not in combined_err:
            return TestResult(tc.test_id, tc.category, tc.strict_mode, False, dt,
                              f"Expected {err_type}, got: {combined_err[:160]}")
        return TestResult(tc.test_id, tc.category, tc.strict_mode, True, dt)

    if rc != 0:
        return TestResult(tc.test_id, tc.category, tc.strict_mode, False, dt,
                          f"Exited {rc}: {combined_err[:200]}")

    if "async" in flags:
        if "Test262:AsyncTestFailure:" in out:
            return TestResult(tc.test_id, tc.category, tc.strict_mode, False, dt,
                              out.strip()[:200])
        if "Test262:AsyncTestComplete" not in out:
            return TestResult(tc.test_id, tc.category, tc.strict_mode, False, dt,
                              f"Missing Test262:AsyncTestComplete in stdout: {out.strip()[:160]}")

    return TestResult(tc.test_id, tc.category, tc.strict_mode, True, dt)


# ============================================================================
# Built-in Self-Contained Test262 Conformance Suite (130+ Specs + Fixtures)
# ============================================================================

def build_builtin_suite(root: Path) -> Tuple[Path, Path]:
    harness_dir = root / "harness"
    test_dir = root / "test"
    harness_dir.mkdir(parents=True, exist_ok=True)
    test_dir.mkdir(parents=True, exist_ok=True)

    for hname, hsrc in BUILTIN_HARNESS_FILES.items():
        (harness_dir / hname).write_text(hsrc.lstrip(), encoding="utf-8")

    def w(rel: str, code: str) -> None:
        p = test_dir / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(code.lstrip(), encoding="utf-8")

    # --- 1. Core ES2026 Language, Classes, Private Fields & Syntax (18 tests) ---
    w("language/let-const-tdz.js", """/*---
flags: [onlyStrict]
---*/
assert.throws(ReferenceError, () => { let x = y; let y = 1; void x; });
let acc = [];
for (let i = 0; i < 3; i++) acc.push(() => i);
assert.sameValue(acc[0]() + acc[1]() + acc[2](), 3);
""")
    w("language/destructuring-defaults.js", """/*---
---*/
const { a = 10, b: [c, d = 20] = [5] } = { b: [7] };
assert.sameValue(a, 10);
assert.sameValue(c, 7);
assert.sameValue(d, 20);
const fnArr = ([x = 3] = [11]) => x;
const fnObj = ({ p: [q = 4] = [9] } = {}) => q;
assert.sameValue(fnArr(), 11);
assert.sameValue(fnArr([]), 3);
assert.sameValue(fnObj(), 9);
assert.sameValue(fnObj({ p: [] }), 4);
let out = 0;
[{ k: out } = { k: 42 }] = [];
assert.sameValue(out, 42);
""")
    w("language/optional-nullish-logical-assign.js", """/*---
---*/
let o = { a: { b: 0, fn(x) { return x + 1; } } };
assert.sameValue(o?.a?.b ?? 99, 0);
assert.sameValue(o?.missing?.b ?? 99, 99);
assert.sameValue(o.a.fn?.(41), 42);
let x = null, y = 0, z = 5;
x ??= 11; y ||= 22; z &&= 33;
assert.sameValue(x + y + z, 66);
""")
    w("language/classes-private-static-blocks.js", """/*---
---*/
class Base {
  constructor(v) { this.v = v; }
  tag() { return "B:" + this.v; }
}
let initSecret;
class Sub extends Base {
  #secret = 10;
  static count = 0;
  static {
    Sub.count = 40;
    initSecret = (inst) => inst.#secret;
  }
  constructor(v) {
    super(v);
    this.#secret += v;
  }
  get val() { return super.tag() + ":" + this.#secret; }
  static has(o) { return #secret in o; }
}
const s = new Sub(5);
assert.sameValue(Sub.count, 40);
assert.sameValue(s.val, "B:5:15");
assert.sameValue(initSecret(s), 15);
assert.sameValue(Sub.has(s), true);
assert.sameValue(Sub.has({}), false);
""")
    w("language/early-dup-let-syntax-error.js", """/*---
negative:
  phase: parse
  type: SyntaxError
---*/
let dupVar = 1;
let dupVar = 2;
""")
    w("language/strict-octal-syntax-error.js", """/*---
flags: [onlyStrict]
negative:
  phase: parse
  type: SyntaxError
---*/
var bad = 010;
""")
    w("language/sloppy-mapped-arguments.js", """/*---
flags: [noStrict]
---*/
function f(a) { arguments[0] = 99; return a; }
assert.sameValue(f(1), 99);
""")
    w("language/strict-unmapped-arguments.js", """/*---
flags: [onlyStrict]
---*/
function f(a) { arguments[0] = 99; return a; }
assert.sameValue(f(1), 1);
""")
    w("language/tagged-templates-raw.js", r"""/*---
---*/
function tag(strs, v) { return strs.raw[0] + ":" + strs[0] + ":" + v; }
assert.sameValue(tag`\n${7}`, "\\n:\n:7");
""")
    w("language/exponentiation-numeric-sep.js", """/*---
---*/
assert.sameValue(2 ** 3 ** 2, 512);
assert.sameValue(1_000_000 + 0xFF_00, 1065280);
""")
    w("language/for-of-iterator-close.js", """/*---
---*/
let closed = false;
const iterable = {
  [Symbol.iterator]() {
    let i = 0;
    return {
      next() { return { value: ++i, done: false }; },
      return() { closed = true; return { done: true }; }
    };
  }
};
for (const x of iterable) {
  if (x === 2) break;
}
assert.sameValue(closed, true);
""")
    w("language/error-cause-suppressed.js", """/*---
---*/
const e = new TypeError("boom", { cause: "root" });
assert.sameValue(e.cause, "root");
const agg = new AggregateError([1, 2], "multi");
assert.sameValue(agg.errors.length, 2);
""")

    # --- 2. Generators, Async/Await, Promises & ES2025 Iterator Helpers (12 tests) ---
    w("async/generator-yield-star.js", """/*---
---*/
function* inner() { yield 2; yield 3; return "ret"; }
function* outer() {
  yield 1;
  const r = yield* inner();
  yield r;
}
const out = [...outer()];
assert.sameValue(out.join(","), "1,2,3,ret");
""")
    w("async/async-await-microtask-order.js", """/*---
flags: [async]
---*/
const log = [];
async function run() {
  log.push("a");
  await Promise.resolve();
  log.push("c");
}
run().then(() => {
  assert.sameValue(log.join(","), "a,b,c");
  $DONE();
}).catch($DONE);
log.push("b");
""")
    w("async/async-generator-for-await.js", """/*---
flags: [async]
---*/
async function* agen() {
  yield await Promise.resolve(10);
  yield 20;
  yield await Promise.resolve(12);
}
(async () => {
  let sum = 0;
  for await (const v of agen()) sum += v;
  assert.sameValue(sum, 42);
})().then(() => $DONE(), $DONE);
""")
    w("async/promise-with-resolvers-and-try.js", """/*---
flags: [async]
---*/
const { promise, resolve } = Promise.withResolvers();
resolve(21);
Promise.all([
  promise,
  Promise.try(() => 21),
  Promise.try(() => { throw new RangeError("x"); }).catch(e => e instanceof RangeError ? 8 : 0)
]).then((res) => {
  assert.sameValue(res[0] + res[1] + res[2], 50);
  $DONE();
}).catch($DONE);
""")
    w("async/iterator-helpers-es2025.js", """/*---
---*/
function* naturals() { let n = 1; while (true) yield n++; }
const arr = naturals()
  .drop(1)
  .filter(x => x % 2 === 0)
  .map(x => x * 10)
  .take(4)
  .toArray();
assert.sameValue(arr.join(","), "20,40,60,80");
assert.sameValue(Iterator.from([1, 2, 3]).some(x => x === 2), true);
assert.sameValue(Iterator.from([1, 2, 3]).every(x => x > 0), true);
assert.sameValue(Iterator.from([1, 2, 3]).reduce((a, b) => a + b, 0), 6);
""")

    # --- 3. Built-ins: Collections, ES2025 Set, Copy-Array, TypedArray, BigInt, RegExp, Proxy (18 tests) ---
    w("builtins/set-methods-es2025.js", """/*---
---*/
const a = new Set([1, 2, 3]);
const b = new Set([3, 4, 5]);
assert.sameValue([...a.union(b)].join(","), "1,2,3,4,5");
assert.sameValue([...a.intersection(b)].join(","), "3");
assert.sameValue([...a.difference(b)].join(","), "1,2");
assert.sameValue([...a.symmetricDifference(b)].join(","), "1,2,4,5");
assert.sameValue(new Set([1, 2]).isSubsetOf(a), true);
assert.sameValue(a.isDisjointFrom(new Set([8, 9])), true);
""")
    w("builtins/object-map-groupby.js", """/*---
---*/
const items = [{ k: "even", v: 2 }, { k: "odd", v: 3 }, { k: "even", v: 4 }];
const g = Object.groupBy(items, x => x.k);
assert.sameValue(g.even.length, 2);
assert.sameValue(g.odd[0].v, 3);
const mg = Map.groupBy([1, 2, 3, 4], x => x % 2);
assert.sameValue(mg.get(0).join(","), "2,4");
""")
    w("builtins/change-array-by-copy.js", """/*---
---*/
const orig = [3, 1, 2];
assert.sameValue(orig.toSorted().join(","), "1,2,3");
assert.sameValue(orig.toReversed().join(","), "2,1,3");
assert.sameValue(orig.toSpliced(1, 1, 9, 8).join(","), "3,9,8,2");
assert.sameValue(orig.with(-1, 42).join(","), "3,1,42");
assert.sameValue(orig.join(","), "3,1,2");
assert.sameValue(orig.findLast(x => x < 3), 2);
assert.sameValue(orig.at(-1), 2);
""")
    w("builtins/string-wellformed-unicode.js", """/*---
---*/
assert.sameValue("abc😀".isWellFormed(), true);
assert.sameValue("\\uD800".isWellFormed(), false);
assert.sameValue("\\uD800".toWellFormed(), "\\uFFFD");
assert.sameValue("a-b-a".replaceAll("a", "z"), "z-b-z");
""")
    w("builtins/resizable-arraybuffer-transfer.js", """/*---
---*/
const rab = new ArrayBuffer(4, { maxByteLength: 16 });
assert.sameValue(rab.resizable, true);
const u8 = new Uint8Array(rab);
u8.set([10, 20, 30, 40]);
rab.resize(8);
assert.sameValue(u8.length, 8);
assert.sameValue(u8[2], 30);
const moved = rab.transfer(6);
assert.sameValue(rab.detached, true);
assert.sameValue(moved.byteLength, 6);
assert.sameValue(new Uint8Array(moved)[1], 20);
""")
    w("builtins/dataview-typedarray-bigint64.js", """/*---
---*/
const ab = new ArrayBuffer(16);
const dv = new DataView(ab);
dv.setBigInt64(0, -1234567890123456789n, true);
assert.sameValue(dv.getBigInt64(0, true), -1234567890123456789n);
const b64 = new BigInt64Array(ab);
assert.sameValue(b64[0], -1234567890123456789n);
""")
    w("builtins/bigint-arbitrary-precision.js", """/*---
---*/
const p = (1n << 128n) + 12345678901234567890n;
assert.sameValue((p - 12345678901234567890n) >> 128n, 1n);
assert.sameValue(BigInt.asIntN(8, 255n), -1n);
assert.sameValue(BigInt.asUintN(8, -1n), 255n);
""")
    w("builtins/regexp-named-groups-lookbehind-unicode.js", """/*---
---*/
const re = /(?<=\\$)(?<dollars>\\d+)\\.(?<cents>\\d{2})/u;
const m = re.exec("Price: $42.95!");
assert.sameValue(m.groups.dollars, "42");
assert.sameValue(m.groups.cents, "95");
assert.sameValue(/\\p{Script=Hangul}+/u.test("아크래디언트"), true);
assert.sameValue(/[\\p{ASCII}&&[^0-9]]+/v.test("abc"), true);
assert.sameValue(/[\\p{ASCII}&&[^0-9]]+/v.test("123"), false);
""")
    w("builtins/proxy-reflect-invariants.js", """/*---
---*/
const log = [];
const target = { x: 10 };
const { proxy, revoke } = Proxy.revocable(target, {
  get(t, k, r) { log.push("get:" + String(k)); return Reflect.get(t, k, r); },
  set(t, k, v, r) { log.push("set:" + String(k)); return Reflect.set(t, k, v, r); }
});
proxy.x = proxy.x + 5;
assert.sameValue(target.x, 15);
assert.sameValue(log.join(","), "get:x,set:x");
revoke();
assert.throws(TypeError, () => proxy.x);
""")
    w("builtins/json-parse-stringify-reviver.js", """/*---
---*/
const s = JSON.stringify({ a: 1, b: [true, "hi", null], s: "\\uD800" });
const o = JSON.parse(s, (k, v) => typeof v === "number" ? v * 2 : v);
assert.sameValue(o.a, 2);
assert.sameValue(o.s, "\\uD800");
""")
    w("builtins/object-hasown-structured-descriptors.js", """/*---
---*/
const o = Object.create({ inherited: 1 });
o.own = 2;
assert.sameValue(Object.hasOwn(o, "own"), true);
assert.sameValue(Object.hasOwn(o, "inherited"), false);
assert.sameValue(Object.fromEntries([["k", 7]]).k, 7);
""")

    # --- 4. Annex B Web Reality Compatibility (4 tests) ---
    w("annexB/proto-accessor-and-literal.js", """/*---
---*/
const base = { p: 42 };
const sub = { __proto__: base };
assert.sameValue(sub.p, 42);
const x = {};
x.__proto__ = base;
assert.sameValue(Object.getPrototypeOf(x), base);
""")
    w("annexB/string-substr-html-escape.js", r"""/*---
---*/
assert.sameValue("abcdef".substr(2, 3), "cde");
assert.sameValue("abcdef".substr(-2), "ef");
assert.sameValue("hi".bold(), "<b>hi</b>");
assert.sameValue("x\"y".link("a\"b"), '<a href="a&quot;b">x"y</a>');
assert.sameValue(unescape(escape("한글123")), "한글123");
assert.sameValue($262.IsHTMLDDA(), null);
""")

    # --- 5. ES Modules, Live Circular Bindings, Namespace, Dynamic Import & TLA ---
    # Fixture helper files placed in `_fixtures/` (prefixed with `_` so discovery skips them as standalone tests)
    w("modules/_fixtures/math_dep.js", """
export let counter = 10;
export function inc( step = 1 ) { counter += step; return counter; }
export default function mul(a, b) { return a * b; }
""")
    w("modules/basic-named-default-live-binding.js", """/*---
flags: [module]
---*/
import mul, { counter, inc } from "./_fixtures/math_dep.js";
assert.sameValue(counter, 10);
assert.sameValue(inc(5), 15);
assert.sameValue(counter, 15);
assert.sameValue(mul(6, 7), 42);
assert.throws(TypeError, () => { counter = 99; });
""")

    w("modules/_fixtures/circ_even.js", """
import { odd } from "./circ_odd.js";
export let evenVisits = 0;
export function even(n) { evenVisits++; return n === 0 ? true : odd(n - 1); }
""")
    w("modules/_fixtures/circ_odd.js", """
import { even } from "./circ_even.js";
export function odd(n) { return n === 0 ? false : even(n - 1); }
""")
    w("modules/circular-hoisted-functions-and-live-let.js", """/*---
flags: [module]
---*/
import { even, evenVisits } from "./_fixtures/circ_even.js";
assert.sameValue(even(6), true);
assert.sameValue(even(5), false);
assert.sameValue(evenVisits, 7);
""")

    w("modules/_fixtures/tdz_a.js", """
import { bVal } from "./tdz_b.js";
export function readB() { return bVal; }
export let aVal = "A_READY";
""")
    w("modules/_fixtures/tdz_b.js", """
import { aVal, readB } from "./tdz_a.js";
export let observedTdz = false;
try {
  void aVal;
} catch (e) {
  observedTdz = (e instanceof ReferenceError);
}
export const bVal = "B_READY";
export const afterInitB = readB();
""")
    w("modules/circular-lexical-tdz.js", """/*---
flags: [module]
---*/
import { aVal } from "./_fixtures/tdz_a.js";
import { observedTdz, afterInitB } from "./_fixtures/tdz_b.js";
assert.sameValue(observedTdz, true);
assert.sameValue(afterInitB, "B_READY");
assert.sameValue(aVal, "A_READY");
""")

    w("modules/_fixtures/star_leaf1.js", "export const x = 1; export const shared = 'L1';\n")
    w("modules/_fixtures/star_leaf2.js", "export const y = 2; export const shared = 'L2';\n")
    w("modules/_fixtures/star_reexport_ok.js", """
export * from "./star_leaf1.js";
export { y } from "./star_leaf2.js";
export * as leaf1Ns from "./star_leaf1.js";
""")
    w("modules/module-namespace-exotic-and-star.js", """/*---
flags: [module]
---*/
import * as ns from "./_fixtures/star_reexport_ok.js";
assert.sameValue(ns[Symbol.toStringTag], "Module");
assert.sameValue(Object.isExtensible(ns), false);
assert.sameValue(Object.getPrototypeOf(ns), null);
assert.sameValue(Object.keys(ns).join(","), "leaf1Ns,shared,x,y");
assert.sameValue(ns.x + ns.y, 3);
assert.sameValue(ns.leaf1Ns.shared, "L1");
""")

    w("modules/_fixtures/star_ambig.js", """
export * from "./star_leaf1.js";
export * from "./star_leaf2.js";
""")
    w("modules/ambiguous-star-export-syntax-error.js", """/*---
flags: [module]
negative:
  phase: resolution
  type: SyntaxError
---*/
import { shared } from "./_fixtures/star_ambig.js";
void shared;
""")
    w("modules/missing-export-syntax-error.js", """/*---
flags: [module]
negative:
  phase: resolution
  type: SyntaxError
---*/
import { doesNotExist } from "./_fixtures/math_dep.js";
void doesNotExist;
""")

    w("modules/import-meta-url.js", """/*---
flags: [module]
---*/
assert.sameValue(typeof import.meta, "object");
assert.sameValue(Object.getPrototypeOf(import.meta), null);
assert(import.meta.url.endsWith("import-meta-url.js"), "import.meta.url ends with filename");
assert.sameValue(import.meta, import.meta);
""")

    w("modules/dynamic-import-and-abrupt-specifier.js", """/*---
flags: [module, async]
---*/
const dynImport = (spec) => $262.importModule(spec, import.meta.url);
const badSpec = { toString() { throw new RangeError("bad spec"); } };
Promise.all([
  dynImport("./_fixtures/math_dep.js"),
  dynImport(badSpec).then(() => "unexpected", e => e instanceof RangeError ? "caught" : "wrong"),
  dynImport("./_fixtures/nonexistent_404.js").then(() => "unexpected", e => e instanceof TypeError ? "missing_ok" : "wrong")
]).then((res) => {
  const ns = res[0];
  assert.sameValue(ns[Symbol.toStringTag], "Module");
  assert.sameValue(ns.default(3, 9), 27);
  assert.sameValue(res[1], "caught");
  assert.sameValue(res[2], "missing_ok");
  $DONE();
}).catch($DONE);
""")

    w("modules/_fixtures/tla_order_log.js", "export const order = [];\n")
    w("modules/_fixtures/tla_child_a.js", """
import { order } from "./tla_order_log.js";
order.push("a:start");
await Promise.resolve();
order.push("a:end");
export const aDone = 20;
""")
    w("modules/_fixtures/tla_child_b.js", """
import { order } from "./tla_order_log.js";
order.push("b:sync");
export const bDone = 22;
""")
    w("modules/top-level-await-execution-order.js", """/*---
flags: [module, async]
---*/
import { order } from "./_fixtures/tla_order_log.js";
import { aDone } from "./_fixtures/tla_child_a.js";
import { bDone } from "./_fixtures/tla_child_b.js";
order.push("root:start");
const bonus = await Promise.resolve(aDone + bDone);
order.push("root:end:" + bonus);
assert.sameValue(order.join("|"), "a:start|b:sync|a:end|root:start|root:end:42");
$DONE();
""")

    w("modules/top-level-await-rejection.js", """/*---
flags: [module]
negative:
  phase: runtime
  type: URIError
---*/
await Promise.resolve();
throw new URIError("async module failure");
""")

    # --- 6. ECMA-402 Intl Suite ---
    w("intl/canonical-locales-and-supported-values.js", """/*---
---*/
const locs = Intl.getCanonicalLocales(["ko-kr", "EN-us-u-nu-latn-ca-gregory", "iw-IL", "en-UK"]);
assert.sameValue(locs.join(","), "ko-KR,en-US-u-ca-gregory-nu-latn,he-IL,en-GB");
assert.throws(RangeError, () => Intl.getCanonicalLocales("en--US"));
assert.throws(RangeError, () => Intl.getCanonicalLocales("123"));
assert.throws(RangeError, () => Intl.getCanonicalLocales("x-priv"));

const cals = Intl.supportedValuesOf("calendar");
assert(cals.includes("gregory") && cals.includes("dangi"), "calendars");
const tzs = Intl.supportedValuesOf("timeZone");
assert(tzs.includes("Asia/Seoul") && tzs.includes("UTC"), "timeZones");
assert.throws(RangeError, () => Intl.supportedValuesOf("invalidKey"));
""")

    w("intl/locale-subtags-maximize-minimize.js", """/*---
---*/
assert.throws(TypeError, () => Intl.Locale("ko-KR"));
const loc = new Intl.Locale("ko-kr-u-hc-h23-ca-dangi", { numeric: true });
assert.sameValue(loc.language, "ko");
assert.sameValue(loc.region, "KR");
assert.sameValue(loc.baseName, "ko-KR");
assert.sameValue(loc.calendar, "dangi");
assert.sameValue(loc.hourCycle, "h23");
assert.sameValue(loc.numeric, true);
assert.sameValue(new Intl.Locale("ko").maximize().toString(), "ko-Kore-KR");
assert.sameValue(new Intl.Locale("ko-Kore-KR").minimize().toString(), "ko");
assert.sameValue(new Intl.Locale("zh-TW").maximize().script, "Hant");
""")

    w("intl/collator-numeric-sensitivity-localeCompare.js", """/*---
---*/
const numCol = new Intl.Collator("en", { numeric: true });
const files = ["file10", "file2", "file1"];
files.sort(numCol.compare);
assert.sameValue(files.join(","), "file1,file2,file10");

const baseCol = new Intl.Collator("fr", { sensitivity: "base" });
assert.sameValue(baseCol.compare("e", "é"), 0);
assert.sameValue(baseCol.compare("a", "A"), 0);

const accentCol = new Intl.Collator("fr", { sensitivity: "accent" });
assert.notSameValue(accentCol.compare("e", "é"), 0);
assert.sameValue(accentCol.compare("e", "E"), 0);

const punctCol = new Intl.Collator("en", { ignorePunctuation: true });
assert.sameValue(punctCol.compare("a-b", "ab"), 0);
assert.sameValue("가".localeCompare("나", "ko") < 0, true);
""")

    w("intl/numberformat-currency-compact-bigint-parts.js", """/*---
---*/
assert.sameValue(
  new Intl.NumberFormat("ko-KR", { style: "currency", currency: "KRW" }).format(1234567),
  "₩1,234,567"
);
assert.sameValue(
  new Intl.NumberFormat("en-US", { style: "currency", currency: "USD" }).format(1234.5),
  "$1,234.50"
);
assert.throws(TypeError, () => new Intl.NumberFormat("en-US", { style: "currency" }));
assert.throws(RangeError, () => new Intl.NumberFormat("en-US", { style: "currency", currency: "US" }));

const koComp = new Intl.NumberFormat("ko-KR", { notation: "compact" });
assert.sameValue(koComp.format(50000), "5만");
const enComp = new Intl.NumberFormat("en-US", { notation: "compact" });
assert.sameValue(enComp.format(2500000), "2.5M");

const huge = 123456789012345678901234567890n;
assert.sameValue(
  huge.toLocaleString("en-US"),
  "123,456,789,012,345,678,901,234,567,890"
);
const parts = new Intl.NumberFormat("en-US", { style: "unit", unit: "kilometer-per-hour" }).formatToParts(88);
assert(parts.some(p => p.type === "unit" && p.value === "km/h"), "unit part km/h");
assert.sameValue(new Intl.NumberFormat("en-US").formatRange(10, 20), "10–20");
assert.sameValue((12345.678).toLocaleString("de-DE", { maximumFractionDigits: 2 }), "12.345,68");
""")

    w("intl/datetimeformat-timezones-dst-locales.js", """/*---
---*/
const ts = Date.UTC(2026, 10, 13, 0, 30, 0); // 2026-11-13T00:30:00Z -> 09:30 KST
const koFmt = new Intl.DateTimeFormat("ko-KR", {
  timeZone: "Asia/Seoul",
  year: "numeric", month: "long", day: "numeric",
  hour: "numeric", minute: "2-digit", hour12: true
});
assert.sameValue(koFmt.format(ts), "2026년 11월 13일 오전 9:30");

// Summer EDT (-04:00) vs Winter EST (-05:00) in America/New_York
const summerUtc = Date.UTC(2026, 6, 15, 16, 0, 0);
const winterUtc = Date.UTC(2026, 0, 15, 16, 0, 0);
const nyFmt = new Intl.DateTimeFormat("en-US", {
  timeZone: "America/New_York",
  hour: "2-digit", minute: "2-digit", hour12: false, timeZoneName: "short"
});
assert.sameValue(nyFmt.format(summerUtc), "12:00 EDT");
assert.sameValue(nyFmt.format(winterUtc), "11:00 EST");

const isoOff = new Intl.DateTimeFormat("en-US", { timeZone: "+09:00", hour: "2-digit", minute: "2-digit", hour12: false });
assert.sameValue(isoOff.resolvedOptions().timeZone, "+09:00");
assert.sameValue(isoOff.format(0), "09:00");

assert.throws(RangeError, () => new Intl.DateTimeFormat("en-US", { timeZone: "Mars/Olympus_Mons" }));
assert.throws(TypeError, () => new Intl.DateTimeFormat("en-US", { dateStyle: "short", year: "numeric" }));
assert.sameValue(new Date(NaN).toLocaleString(), "Invalid Date");
""")

    w("intl/pluralrules-cardinal-and-ordinal.js", """/*---
---*/
assert.throws(TypeError, () => Intl.PluralRules("en"));
const enCard = new Intl.PluralRules("en-US");
assert.sameValue(enCard.select(1), "one");
assert.sameValue(enCard.select(2), "other");
const enFixed = new Intl.PluralRules("en-US", { minimumFractionDigits: 1 });
assert.sameValue(enFixed.select(1), "other");

const enOrd = new Intl.PluralRules("en-US", { type: "ordinal" });
assert.sameValue(enOrd.select(1), "one");
assert.sameValue(enOrd.select(2), "two");
assert.sameValue(enOrd.select(3), "few");
assert.sameValue(enOrd.select(11), "other");
assert.sameValue(enOrd.select(22), "two");

const koPr = new Intl.PluralRules("ko-KR");
assert.sameValue(koPr.select(1), "other");
assert.sameValue(enCard.selectRange(1, 5), "other");
""")

    w("intl/relativetimeformat-en-ko-parts.js", """/*---
---*/
assert.throws(TypeError, () => Intl.RelativeTimeFormat("ko"));
const koAuto = new Intl.RelativeTimeFormat("ko", { numeric: "auto" });
assert.sameValue(koAuto.format(-1, "day"), "어제");
assert.sameValue(koAuto.format(0, "day"), "오늘");
assert.sameValue(koAuto.format(1, "day"), "내일");
assert.sameValue(koAuto.format(-3, "days"), "3일 전");

const enAuto = new Intl.RelativeTimeFormat("en", { numeric: "auto" });
assert.sameValue(enAuto.format(-1, "day"), "yesterday");
assert.sameValue(enAuto.format(2, "hours"), "in 2 hours");
const rtfParts = enAuto.formatToParts(-5, "seconds");
assert(rtfParts.some(p => p.type === "integer" && p.value === "5" && p.unit === "second"), "rtf unit part");
assert.throws(RangeError, () => enAuto.format(NaN, "day"));
assert.throws(RangeError, () => enAuto.format(1, "decades"));
""")

    w("intl/segmenter-grapheme-word-sentence.js", """/*---
---*/
const gSeg = new Intl.Segmenter("ko", { granularity: "grapheme" });
const gArr = [...gSeg.segment("가😀🇰🇷👨‍👩‍👧")].map(x => x.segment);
assert.sameValue(gArr.length, 4);
assert.sameValue(gArr[0], "가");
assert.sameValue(gArr[1], "😀");
assert.sameValue(gArr[2], "🇰🇷");
assert.sameValue(gArr[3], "👨‍👩‍👧");

const segs = gSeg.segment("A😀B");
assert.sameValue(segs.containing(0).segment, "A");
assert.sameValue(segs.containing(1).segment, "😀");
assert.sameValue(segs.containing(2).segment, "😀");
assert.sameValue(segs.containing(2).index, 1);
assert.sameValue(segs.containing(3).segment, "B");
assert.sameValue(segs.containing(99), undefined);
assert.sameValue("isWordLike" in segs.containing(0), false);

const wSeg = new Intl.Segmenter("en", { granularity: "word" });
const words = [...wSeg.segment("Hello, 아크래디언트 2026!")].filter(x => x.isWordLike).map(x => x.segment);
assert.sameValue(words.join("|"), "Hello|아크래디언트|2026");

const sSeg = new Intl.Segmenter("en", { granularity: "sentence" });
const sents = [...sSeg.segment("First! Second? Third.")].map(x => x.segment);
assert.sameValue(sents.length, 3);
""")

    w("intl/displaynames-and-listformat.js", """/*---
---*/
assert.throws(TypeError, () => new Intl.DisplayNames("en"));
const dnKo = new Intl.DisplayNames("ko", { type: "region" });
assert.sameValue(dnKo.of("KR"), "대한민국");
assert.sameValue(dnKo.of("US"), "미국");
const dnEn = new Intl.DisplayNames("en", { type: "language" });
assert.sameValue(dnEn.of("ko"), "Korean");
const dnNone = new Intl.DisplayNames("en", { type: "region", fallback: "none" });
assert.sameValue(dnNone.of("999"), undefined);

const lfEn = new Intl.ListFormat("en", { style: "long", type: "conjunction" });
assert.sameValue(lfEn.format(["A", "B", "C"]), "A, B, and C");
const lfKo = new Intl.ListFormat("ko", { style: "long", type: "disjunction" });
assert.sameValue(lfKo.format(["사과", "배"]), "사과 또는 배");
assert.throws(TypeError, () => lfEn.format(["ok", 123]));
""")

    # --- 7. Cross-Realm `$262` Host Capabilities (3 tests) ---
    w("realm/cross-realm-intrinsics-and-isarray.js", """/*---
---*/
const other = $262.createRealm();
const otherArr = other.evalScript("[10, 20, 30]");
assert.sameValue(otherArr instanceof Array, false);
assert.sameValue(Array.isArray(otherArr), true);
assert.sameValue(other.global !== globalThis, true);

let caught = null;
try {
  other.evalScript("throw new TypeError('from child')");
} catch (e) {
  caught = e;
}
assert.sameValue(caught instanceof other.global.TypeError, true);
assert.sameValue(caught instanceof TypeError, false);
""")

    w("realm/detach-arraybuffer-host-hook.js", """/*---
---*/
const ab = new ArrayBuffer(32);
const i32 = new Int32Array(ab);
i32[0] = 777;
$262.detachArrayBuffer(ab);
assert.sameValue(ab.detached, true);
assert.sameValue(ab.byteLength, 0);
assert.sameValue(i32.length, 0);
""")

    # --- 8. Tracing GC, Cyclic Graphs, WeakMap Ephemerons & FinalizationRegistry (3 tests) ---
    w("gc/cyclic-garbage-and-weakref.js", """/*---
---*/
let wr;
(function allocateCycle() {
  const a = { name: "nodeA" };
  const b = { name: "nodeB", peer: a };
  a.peer = b;
  wr = new WeakRef(a);
  assert.sameValue(wr.deref().peer.name, "nodeB");
})();
$262.gc();
assert.sameValue(wr.deref(), undefined);
""")

    w("gc/weakmap-ephemeron-chain.js", """/*---
---*/
const wm = new WeakMap();
let rootKey = {};
let valWr;
(function buildChain() {
  let k1 = rootKey;
  let k2 = { step: 2 };
  let k3 = { step: 3, secret: 9001 };
  wm.set(k1, k2);
  wm.set(k2, k3);
  valWr = new WeakRef(k3);
})();
// While rootKey is alive, ephemeron fixed-point marking MUST keep k2 & k3 alive!
$262.gc();
assert.sameValue(valWr.deref().secret, 9001);
assert.sameValue(wm.get(wm.get(rootKey)).secret, 9001);

// Once rootKey is dropped, GC reclaims the entire ephemeron chain!
rootKey = null;
$262.gc();
assert.sameValue(valWr.deref(), undefined);
""")

    w("gc/finalization-registry-cleanup-microtask.js", """/*---
flags: [async]
---*/
const cleaned = [];
const fr = new FinalizationRegistry((held) => {
  cleaned.push(held);
  if (cleaned.join(",") === "token-A") {
    $DONE();
  }
});
const cancelTok = {};
(function () {
  const dead1 = {};
  const dead2 = {};
  fr.register(dead1, "token-A");
  fr.register(dead2, "token-B-unregistered", cancelTok);
  fr.unregister(cancelTok);
})();
$262.gc();
""")

    # --- 9. Extended ECMA-402, ESM Diamond/TLA-SCC, & ES2026 Coverage ---
    w("intl/numberformat-scientific-engineering-signdisplay.js", """/*---
---*/
const sci = new Intl.NumberFormat("en-US", { notation: "scientific" });
assert.sameValue(sci.format(12500), "1.25E4");
const eng = new Intl.NumberFormat("en-US", { notation: "engineering" });
assert.sameValue(eng.format(12500), "12.5E3");
const pct = new Intl.NumberFormat("en-US", { style: "percent", signDisplay: "exceptZero" });
assert.sameValue(pct.format(0.25), "+25%");
assert.sameValue(pct.format(0), "0%");
const strNum = new Intl.NumberFormat("en-US", { minimumIntegerDigits: 4 }).format("42");
assert.sameValue(strNum, "0,042");
""")

    w("intl/datetimeformat-datestyle-timestyle-parts.js", """/*---
---*/
const ts = Date.UTC(2026, 4, 5, 14, 7, 9);
const dtf = new Intl.DateTimeFormat("en-US", {
  timeZone: "UTC",
  dateStyle: "medium",
  timeStyle: "short"
});
const parts = dtf.formatToParts(ts);
assert(parts.some(p => p.type === "year" && p.value === "2026"), "year part 2026");
assert(parts.some(p => p.type === "minute" && p.value === "07"), "minute part 07");
const d = new Date(ts);
assert(d.toLocaleDateString("ko-KR", { timeZone: "UTC" }).includes("2026"), "toLocaleDateString ko-KR");
assert(d.toLocaleTimeString("ja-JP", { timeZone: "Asia/Tokyo", hour12: false }).includes("23:07"), "toLocaleTimeString ja-JP");
""")

    w("intl/pluralrules-fr-ru-ar.js", """/*---
---*/
const fr = new Intl.PluralRules("fr");
assert.sameValue(fr.select(0), "one");
assert.sameValue(fr.select(1), "one");
assert.sameValue(fr.select(2), "other");

const ru = new Intl.PluralRules("ru");
assert.sameValue(ru.select(1), "one");
assert.sameValue(ru.select(3), "few");
assert.sameValue(ru.select(5), "many");
assert.sameValue(ru.select(21), "one");

const ar = new Intl.PluralRules("ar");
assert.sameValue(ar.select(0), "zero");
assert.sameValue(ar.select(2), "two");
assert.sameValue(ar.select(7), "few");
assert.sameValue(ar.select(15), "many");
""")

    w("intl/segmenter-crlf-hangul-jamo-iterator.js", r"""/*---
---*/
const gSeg = new Intl.Segmenter("ko", { granularity: "grapheme" });
// CRLF (\r\n) must stay a single grapheme cluster, and Hangul L+V+T jamo sequence must cluster
const crlf = [...gSeg.segment("A\r\nB")].map(s => s.segment);
assert.sameValue(crlf.length, 3);
assert.sameValue(crlf[1], "\r\n");

const jamo = "\u1100\u1161\u11A8"; // ㄱ + ㅏ + ㄱ
assert.sameValue([...gSeg.segment(jamo)].length, 1);
""")

    w("intl/supportedlocalesof-and-tostringtag.js", """/*---
---*/
assert.sameValue(Intl[Symbol.toStringTag], "Intl");
assert.sameValue(Intl.Collator.supportedLocalesOf(["ko-kr", "en-US"]).join(","), "ko-KR,en-US");
assert.sameValue(Intl.NumberFormat.supportedLocalesOf("ja-jp")[0], "ja-JP");
assert.sameValue(Intl.DateTimeFormat.supportedLocalesOf("zh-cn")[0], "zh-CN");
const nf = new Intl.NumberFormat("en-US");
const detachedFormat = nf.format;
assert.sameValue(detachedFormat(1000), "1,000");
assert.sameValue(nf.format, detachedFormat);
""")

    w("modules/_fixtures/diamond_root_val.js", "export const sharedDiamond = 777;\n")
    w("modules/_fixtures/diamond_left.js", "export * from './diamond_root_val.js';\n")
    w("modules/_fixtures/diamond_right.js", "export * from './diamond_root_val.js';\n")
    w("modules/_fixtures/diamond_join.js", """
export * from "./diamond_left.js";
export * from "./diamond_right.js";
""")
    w("modules/diamond-star-reexport-same-binding.js", """/*---
flags: [module]
---*/
// ECMA-262 §16.2.1.6.3: two `export *` paths resolving to the SAME `{module, bindingName}` are NOT ambiguous!
import { sharedDiamond } from "./_fixtures/diamond_join.js";
assert.sameValue(sharedDiamond, 777);
""")

    w("modules/duplicate-export-early-syntax-error.js", """/*---
flags: [module]
negative:
  phase: parse
  type: SyntaxError
---*/
const a = 1, b = 2;
export { a as dup, b as dup };
""")

    w("modules/_fixtures/ns_tdz_a.js", """
import * as bNs from "./ns_tdz_b.js";
export let sawRefErr = false;
try {
  void bNs.uninitLex;
} catch (e) {
  sawRefErr = (e instanceof ReferenceError);
}
export const aReady = true;
""")
    w("modules/_fixtures/ns_tdz_b.js", """
import { aReady } from "./ns_tdz_a.js";
export const uninitLex = aReady ? 99 : 0;
""")
    w("modules/namespace-tdz-reference-error.js", """/*---
flags: [module]
---*/
import { uninitLex } from "./_fixtures/ns_tdz_b.js";
import { sawRefErr } from "./_fixtures/ns_tdz_a.js";
assert.sameValue(sawRefErr, true);
assert.sameValue(uninitLex, 99);
""")

    w("modules/_fixtures/tla_scc_1.js", """
import { v2 } from "./tla_scc_2.js";
await Promise.resolve();
export const v1 = 10;
export function sumScc() { return v1 + v2; }
""")
    w("modules/_fixtures/tla_scc_2.js", """
import { sumScc } from "./tla_scc_1.js";
await Promise.resolve();
export const v2 = 32;
export { sumScc };
""")
    w("modules/circular-top-level-await-scc.js", """/*---
flags: [module, async]
---*/
import { sumScc } from "./_fixtures/tla_scc_2.js";
assert.sameValue(sumScc(), 42);
$DONE();
""")

    w("realm/agent-monotonic-and-child-print.js", """/*---
---*/
const t0 = $262.agent.monotonicNow();
const t1 = $262.agent.monotonicNow();
assert(typeof t0 === "number" && t1 >= t0, "monotonicNow non-decreasing");
assert.sameValue($262.agent.getReport(), null);
const child = $262.createRealm();
assert.sameValue(typeof child.global.Intl.NumberFormat, "function");
assert.sameValue(child.evalScript("new Intl.NumberFormat('ko-KR').format(1000)"), "1,000");
child.destroy();
""")

    w("language/symbols-wellknown-tostringtag-hasinstance.js", """/*---
---*/
class CustomTag {
  get [Symbol.toStringTag]() { return "ArcWidget"; }
  static [Symbol.hasInstance](x) { return Boolean(x && x.arcBrand === 1); }
}
assert.sameValue(Object.prototype.toString.call(new CustomTag()), "[object ArcWidget]");
assert.sameValue({ arcBrand: 1 } instanceof CustomTag, true);
assert.sameValue({ arcBrand: 0 } instanceof CustomTag, false);
""")

    w("language/generators-return-finally.js", """/*---
---*/
const steps = [];
function* gen() {
  try {
    steps.push("try");
    yield 1;
    steps.push("unreachable");
  } finally {
    steps.push("finally");
  }
}
const g = gen();
assert.sameValue(g.next().value, 1);
assert.sameValue(g.return(99).value, 99);
assert.sameValue(steps.join(","), "try,finally");
""")

    w("builtins/weakset-map-iteration-order.js", """/*---
---*/
const ws = new WeakSet();
const k = {};
ws.add(k);
assert.sameValue(ws.has(k), true);
ws.delete(k);
assert.sameValue(ws.has(k), false);

const m = new Map([["a", 1], ["b", 2], ["c", 3]]);
m.delete("b");
m.set("b", 20);
assert.sameValue([...m.keys()].join(","), "a,c,b");
assert.sameValue(m.get("b"), 20);
""")

    w("builtins/promise-any-allsettled-race.js", """/*---
flags: [async]
---*/
Promise.any([
  Promise.reject("e1"),
  Promise.resolve("winner"),
  Promise.reject("e2")
]).then((win) => {
  assert.sameValue(win, "winner");
  return Promise.allSettled([Promise.resolve(10), Promise.reject("boom")]);
}).then((settled) => {
  assert.sameValue(settled[0].status, "fulfilled");
  assert.sameValue(settled[0].value, 10);
  assert.sameValue(settled[1].status, "rejected");
  assert.sameValue(settled[1].reason, "boom");
  $DONE();
}).catch($DONE);
""")

    w("intl/collator-korean-japanese-usage-search.js", """/*---
---*/
const searchCol = new Intl.Collator("ko-KR", { usage: "search", sensitivity: "case" });
assert.sameValue(searchCol.resolvedOptions().usage, "search");
assert.sameValue(searchCol.resolvedOptions().sensitivity, "case");
assert.notSameValue(searchCol.compare("a", "A"), 0);
assert.sameValue(searchCol.compare("e", "é"), 0);
""")

    w("intl/displaynames-script-currency-fallback-code.js", """/*---
---*/
const scDn = new Intl.DisplayNames("ko", { type: "script" });
assert.sameValue(scDn.of("Hang"), "한글");
const curDn = new Intl.DisplayNames("en", { type: "currency", fallback: "code" });
assert.sameValue(curDn.of("KRW"), "South Korean won");
assert.sameValue(curDn.of("XYZ"), "XYZ");
""")

    w("intl/listformat-unit-narrow-parts.js", """/*---
---*/
const lf = new Intl.ListFormat("en-US", { type: "unit", style: "short" });
const parts = lf.formatToParts(["5 km", "12 m"]);
assert(parts.some(p => p.type === "element" && p.value === "5 km"), "first unit element");
assert(parts.some(p => p.type === "literal"), "unit literal separator");
""")

    w("intl/bigint-and-number-tolocalestring-options.js", """/*---
---*/
assert.sameValue(
  98000n.toLocaleString("ko-KR", { style: "currency", currency: "KRW" }),
  "₩98,000"
);
assert.sameValue(
  (1500).toLocaleString("ja-JP", { style: "currency", currency: "JPY" }),
  "¥1,500"
);
""")

    w("modules/_fixtures/once_counter.js", """
export let evalCount = 0;
evalCount += 1;
""")
    w("modules/_fixtures/once_mid1.js", "import './once_counter.js'; export { evalCount as c1 } from './once_counter.js';\n")
    w("modules/_fixtures/once_mid2.js", "import './once_counter.js'; export { evalCount as c2 } from './once_counter.js';\n")
    w("modules/cached-evaluation-idempotent.js", """/*---
flags: [module]
---*/
import { c1 } from "./_fixtures/once_mid1.js";
import { c2 } from "./_fixtures/once_mid2.js";
import { evalCount } from "./_fixtures/once_counter.js";
assert.sameValue(c1, 1);
assert.sameValue(c2, 1);
assert.sameValue(evalCount, 1);
""")

    w("modules/_fixtures/reexport_alias.js", """
export { counter as aliasedCounter, inc as stepCounter, default as mathDefault } from "./math_dep.js";
""")
    w("modules/reexport-rename-and-default.js", """/*---
flags: [module]
---*/
import { aliasedCounter, stepCounter, mathDefault } from "./_fixtures/reexport_alias.js";
const base = aliasedCounter;
stepCounter(4);
assert.sameValue(aliasedCounter, base + 4);
assert.sameValue(mathDefault(7, 8), 56);
""")

    w("language/try-finally-return-override-and-break.js", """/*---
---*/
function f() {
  try {
    return 1;
  } finally {
    return 42;
  }
}
assert.sameValue(f(), 42);
let log = "";
for (let i = 0; i < 3; i++) {
  try {
    if (i === 1) break;
    log += i;
  } finally {
    log += "F";
  }
}
assert.sameValue(log, "0FF");
""")

    w("builtins/math-ieee754-and-object-is.js", """/*---
---*/
assert.sameValue(Math.imul(0xffffffff, 5), -5);
assert.sameValue(Math.clz32(1), 31);
assert.sameValue(Math.hypot(3, 4), 5);
assert.sameValue(Object.is(-0, +0), false);
assert.sameValue(Object.is(NaN, NaN), true);
""")

    # ------------------------------------------------------------------
    # F02 & F03: Mega-Bundle Compiler Scaling (>10.5 MB, >50K consts, >32K locals, >64 KiB jump, GC pacing)
    # ------------------------------------------------------------------
    mega_parts: List[str] = [
        "/*---\nflags: [onlyStrict]\n---*/\n",
        "(function() {\n",
    ]
    # 52,000 distinct string constants (~9.1 MB)
    mega_parts.append("const K = [\n")
    mega_parts.extend(
        f'  "arcr_mega_const_{i:05d}_payload_' + ('x' * 150) + '",\n'
        for i in range(52000)
    )
    mega_parts.append("];\n")
    # 36,000 distinct lexical bindings crossing 32,767 -> 32,768 (~1.5 MB)
    mega_parts.extend(f"let loc_{i:05d} = {i};\n" for i in range(36000))
    # > 64 KiB bytecode forward jump block
    mega_parts.append("let branchTaken = 0;\nif (loc_00000 === 999999) {\n")
    mega_parts.extend(f"  loc_{(i % 1000):05d} += {i + 1};\n" for i in range(9000))
    mega_parts.append("} else {\n  branchTaken = 1;\n}\n")
    mega_parts.append(
        "$262.gc();\n"
        "assert.sameValue(branchTaken, 1);\n"
        "assert.sameValue(K.length, 52000);\n"
        'assert.sameValue(K[0].slice(0, 22), "arcr_mega_const_00000_");\n'
        'assert.sameValue(K[51999].slice(0, 22), "arcr_mega_const_51999_");\n'
        "assert.sameValue(loc_00000, 0);\n"
        "assert.sameValue(loc_32767, 32767);\n"
        "assert.sameValue(loc_32768, 32768);\n"
        "assert.sameValue(loc_35999, 35999);\n"
        "})();\n"
    )
    w("scale/mega-bundle-50k-consts-36k-locals.js", "".join(mega_parts))

    closure_32k_parts: List[str] = [
        "/*---\nflags: [onlyStrict]\n---*/\n",
        "function outer() {\n",
    ]
    closure_32k_parts.extend(f"  let s_{i:05d} = {i};\n" for i in range(33600))
    closure_32k_parts.append(
        "  return {\n"
        "    read: () => [s_00000, s_32767, s_32768, s_33500],\n"
        "    bump: () => { s_00000 += 1; s_32767 += 2; s_32768 += 3; s_33500 += 4; }\n"
        "  };\n"
        "}\n"
        "const h = outer();\n"
        "$262.gc();\n"
        "h.bump();\n"
        "$262.gc();\n"
        "const r = h.read();\n"
        "assert.sameValue(r[0], 1);\n"
        "assert.sameValue(r[1], 32769);\n"
        "assert.sameValue(r[2], 32771);\n"
        "assert.sameValue(r[3], 33504);\n"
    )
    w("scale/closures-capturing-locals-above-32k.js", "".join(closure_32k_parts))

    w("scale/gc-pacing-multi-megabyte-churn.js", """/*---
flags: [onlyStrict]
---*/
const survivor = { tag: "keep-alive" };
const survivorRef = new WeakRef(survivor);
let deadRef;
(() => {
  const transient = { tag: "ephemeral" };
  deadRef = new WeakRef(transient);
  const waves = [];
  for (let w = 0; w < 12; w++) {
    const batch = [];
    for (let i = 0; i < 2000; i++) {
      batch.push({ idx: i, text: "wave_" + w + "_item_" + i });
    }
    waves.push(batch);
    if (waves.length > 3) waves.shift();
  }
})();
$262.gc();
assert.sameValue(survivorRef.deref(), survivor);
assert.sameValue(deadRef.deref(), undefined);
""")

    # ------------------------------------------------------------------
    # F05: Math.sumPrecise, Map/WeakMap getOrInsert*, Weak Symbol Keys, Intl.DurationFormat
    # ------------------------------------------------------------------
    w("builtins/math-sum-precise-ieee754-and-errors.js", """/*---
---*/
assert.sameValue(typeof Math.sumPrecise, "function");
assert.sameValue(Math.sumPrecise.length, 1);
assert.sameValue(Math.sumPrecise.name, "sumPrecise");
assert.sameValue(Object.is(Math.sumPrecise([]), -0), true);
assert.sameValue(Object.is(Math.sumPrecise([-0, -0]), -0), true);
assert.sameValue(Object.is(Math.sumPrecise([-0, +0]), +0), true);
assert.sameValue(Object.is(Math.sumPrecise([1, -1]), +0), true);
assert.sameValue(Math.sumPrecise([1e20, 0.1, -1e20]), 0.1);
assert.sameValue(Math.sumPrecise([1, 1e100, 1, -1e100]), 2);
assert.sameValue(Math.sumPrecise([1 + 2 ** -52, 2 ** -53, 2 ** -1022]), 1 + 2 ** -51);
assert.sameValue(Math.sumPrecise([1, 2 ** -53, 2 ** -1022]), 1 + 2 ** -52);
assert.sameValue(Math.sumPrecise([Number.MAX_VALUE, Number.MAX_VALUE, -Number.MAX_VALUE]), Number.MAX_VALUE);
assert.sameValue(Math.sumPrecise([Infinity, 1]), Infinity);
assert.sameValue(Math.sumPrecise([-Infinity, 1]), -Infinity);
assert.sameValue(Number.isNaN(Math.sumPrecise([Infinity, -Infinity])), true);
assert.sameValue(Number.isNaN(Math.sumPrecise([NaN, 1])), true);

function* genNums() { yield 10; yield 20.5; yield -0.5; }
assert.sameValue(Math.sumPrecise(genNums()), 30);

assert.throws(TypeError, () => Math.sumPrecise());
assert.throws(TypeError, () => Math.sumPrecise(null));
assert.throws(TypeError, () => Math.sumPrecise(123));

let closedCount = 0;
const badIter = {
  [Symbol.iterator]() {
    let i = 0;
    return {
      next() {
        i += 1;
        if (i === 1) return { value: 1, done: false };
        return { value: "2", done: false };
      },
      return() { closedCount += 1; return { done: true }; }
    };
  }
};
assert.throws(TypeError, () => Math.sumPrecise(badIter));
assert.sameValue(closedCount, 1);
assert.throws(TypeError, () => Math.sumPrecise([1n]));
assert.throws(TypeError, () => Math.sumPrecise([new Number(1)]));
""")

    w("builtins/map-get-or-insert-and-computed.js", """/*---
---*/
assert.sameValue(Map.prototype.getOrInsert.length, 2);
assert.sameValue(Map.prototype.getOrInsertComputed.length, 2);

const m = new Map();
assert.sameValue(m.getOrInsert(-0, "first"), "first");
assert.sameValue(m.getOrInsert(+0, "second"), "first");
assert.sameValue(Object.is([...m.keys()][0], +0), true);

m.set("undefKey", undefined);
assert.sameValue(m.getOrInsert("undefKey", "fallback"), undefined);

let cbCalls = 0;
assert.sameValue(m.getOrInsertComputed("undefKey", () => { cbCalls += 1; return "x"; }), undefined);
assert.sameValue(cbCalls, 0);

let seenKey;
assert.sameValue(m.getOrInsertComputed(-0, () => "unreachable"), "first");
m.clear();
assert.sameValue(m.getOrInsertComputed(-0, (k) => { seenKey = k; return "zeroVal"; }), "zeroVal");
assert.sameValue(Object.is(seenKey, +0), true);

assert.sameValue(
  m.getOrInsertComputed("reentrant", (k) => {
    m.set(k, "duringCallback");
    return "finalValue";
  }),
  "finalValue"
);
assert.sameValue(m.get("reentrant"), "finalValue");

assert.throws(TypeError, () => m.getOrInsertComputed("undefKey", null));
assert.throws(TypeError, () => Map.prototype.getOrInsert.call({}, "a", 1));
assert.throws(TypeError, () => Map.prototype.getOrInsertComputed.call({}, "a", () => 1));
""")

    w("builtins/weakmap-get-or-insert-and-weak-symbol-keys.js", """/*---
---*/
assert.sameValue(WeakMap.prototype.getOrInsert.length, 2);
assert.sameValue(WeakMap.prototype.getOrInsertComputed.length, 2);

const wm = new WeakMap();
const ws = new WeakSet();
const sym = Symbol("unregistered");

assert.sameValue(wm.getOrInsert(sym, 101), 101);
assert.sameValue(wm.getOrInsert(sym, 202), 101);
assert.sameValue(wm.get(sym), 101);

assert.sameValue(wm.getOrInsertComputed(Symbol.iterator, (k) => k === Symbol.iterator ? "iterVal" : "bad"), "iterVal");
assert.sameValue(wm.get(Symbol.iterator), "iterVal");

ws.add(sym);
ws.add(Symbol.dispose);
assert.sameValue(ws.has(sym), true);
assert.sameValue(ws.has(Symbol.dispose), true);

const regSym = Symbol.for("arcr.global.sym");
assert.throws(TypeError, () => wm.set(regSym, 1));
assert.throws(TypeError, () => wm.getOrInsert(regSym, 1));
assert.throws(TypeError, () => wm.getOrInsertComputed(regSym, () => 1));
assert.throws(TypeError, () => ws.add(regSym));
assert.throws(TypeError, () => new WeakRef(regSym));
assert.throws(TypeError, () => wm.getOrInsert("primitive", 1));
assert.throws(TypeError, () => wm.getOrInsertComputed(sym, "notCallable"));

const fr = new FinalizationRegistry(() => {});
const tok = Symbol("token");
fr.register(Symbol("target"), "held", tok);
assert.sameValue(fr.unregister(tok), true);
assert.throws(TypeError, () => fr.register(regSym, "held"));

let symRef = (() => new WeakRef(Symbol("ephemeral-sym")))();
$262.gc();
assert.sameValue(symRef.deref(), undefined);
""")

    w("intl/durationformat-styles-locales-iso-and-parts.js", """/*---
---*/
assert.sameValue(typeof Intl.DurationFormat, "function");
assert.sameValue(Intl.DurationFormat.length, 0);
assert.throws(TypeError, () => Intl.DurationFormat());

const dfDefault = new Intl.DurationFormat("en-US");
assert.sameValue(dfDefault[Symbol.toStringTag], "Intl.DurationFormat");
assert.sameValue(Reflect.ownKeys(dfDefault).length, 0);
assert.sameValue(dfDefault.resolvedOptions().style, "short");
assert.sameValue(dfDefault.format({ hours: 1, minutes: 46, seconds: 40 }), "1 hr, 46 min, 40 sec");
assert.sameValue(dfDefault.format("PT1H46M40S"), "1 hr, 46 min, 40 sec");

const dfLong = new Intl.DurationFormat("en-US", { style: "long" });
assert.sameValue(dfLong.format({ years: 1, days: 2, seconds: 1 }), "1 year, 2 days, 1 second");

const dfNarrow = new Intl.DurationFormat("en-US", { style: "narrow" });
assert.sameValue(dfNarrow.format({ hours: 2, minutes: 5, seconds: 9 }), "2h 5m 9s");

const dfDig = new Intl.DurationFormat("en-US", { style: "digital" });
assert.sameValue(dfDig.format({ minutes: 5, seconds: 9 }), "0:05:09");
assert.sameValue(dfDig.format("PT1H5M9.25S"), "1:05:09.25");

const dfDigFd = new Intl.DurationFormat("en-US", { style: "digital", fractionalDigits: 3 });
assert.sameValue(dfDigFd.resolvedOptions().fractionalDigits, 3);
assert.sameValue(dfDigFd.format({ seconds: 4, milliseconds: 50 }), "0:00:04.050");

const dfKo = new Intl.DurationFormat("ko-KR", { style: "long" });
assert.sameValue(dfKo.format({ hours: 1, minutes: 30 }), "1시간 30분");

const dfJa = new Intl.DurationFormat("ja-JP", { style: "short" });
assert.sameValue(dfJa.format({ days: 3, hours: 4 }), "3日 4時間");

const negParts = dfDefault.formatToParts({ hours: -2, minutes: -15 });
assert.sameValue(dfDefault.format({ hours: -2, minutes: -15 }), "-2 hr, 15 min");
assert.sameValue(negParts[0].type, "minusSign");
assert.sameValue(negParts[0].unit, "hour");
assert(negParts.some((p) => p.type === "unit" && p.value === "min" && p.unit === "minute"), "minute unit part");
assert(negParts.some((p) => p.type === "literal" && p.value === ", " && !("unit" in p)), "list literal part");

assert.throws(TypeError, () => dfDefault.format({}));
assert.throws(TypeError, () => dfDefault.format(42));
assert.throws(RangeError, () => dfDefault.format({ hours: 1, minutes: -5 }));
assert.throws(RangeError, () => dfDefault.format({ hours: 1.5 }));
assert.throws(RangeError, () => dfDefault.format("P"));
""")

    # ------------------------------------------------------------------
    # F04: Explicit Resource Management (using, await using, DisposableStack, AsyncDisposableStack, SuppressedError)
    # ------------------------------------------------------------------
    w("language/using-block-lifo-nullish-and-iterator-dispose.js", """/*---
---*/
const log = [];
{
  using a = { [Symbol.dispose]() { log.push("a"); } };
  using n1 = null;
  using n2 = undefined;
  assert.sameValue(n1, null);
  assert.sameValue(n2, undefined);
  {
    using b = { [Symbol.dispose]() { log.push("b"); } };
    using c = { [Symbol.dispose]() { log.push("c"); } };
    log.push("inner");
  }
  log.push("outer");
}
assert.sameValue(log.join(","), "inner,c,b,outer,a");

let genClosed = false;
function* makeGen() {
  try {
    yield 1;
    yield 2;
  } finally {
    genClosed = true;
  }
}
{
  using g = makeGen();
  assert.sameValue(g.next().value, 1);
}
assert.sameValue(genClosed, true);

assert.throws(TypeError, () => { using bad = 42; });
assert.throws(TypeError, () => { using badObj = {}; });
""")

    w("language/using-control-flow-return-break-continue-try-finally.js", """/*---
---*/
const trace = [];
function runEarlyReturn(flag) {
  using r1 = { [Symbol.dispose]() { trace.push("r1"); } };
  try {
    using r2 = { [Symbol.dispose]() { trace.push("r2"); } };
    if (flag) return "early";
  } finally {
    trace.push("finally");
  }
  return "normal";
}
assert.sameValue(runEarlyReturn(true), "early");
assert.sameValue(trace.join(","), "r2,finally,r1");

trace.length = 0;
outerLoop: for (let i = 0; i < 3; i++) {
  using outerRes = { [Symbol.dispose]() { trace.push("o" + i); } };
  for (let j = 0; j < 2; j++) {
    using innerRes = { [Symbol.dispose]() { trace.push("i" + i + j); } };
    if (i === 0 && j === 0) continue;
    if (i === 1 && j === 1) break outerLoop;
  }
}
assert.sameValue(trace.join(","), "i00,i01,o0,i10,i11,o1");
""")

    w("language/using-for-of-and-c-for-loop-per-iteration.js", """/*---
---*/
const order = [];
const items = [1, 2, 3].map((id) => ({
  id,
  [Symbol.dispose]() { order.push("d" + id); }
}));

for (using x of items) {
  order.push("b" + x.id);
  if (x.id === 2) break;
}
assert.sameValue(order.join(","), "b1,d1,b2,d2");

const cOrder = [];
let stepIdx = 0;
for (using c = { [Symbol.dispose]() { cOrder.push("cd" + stepIdx); } }; stepIdx < 2; stepIdx++) {
  cOrder.push("cb" + stepIdx);
}
assert.sameValue(cOrder.join(","), "cb0,cb1,cd2");
""")

    w("language/using-suppressed-error-chaining.js", """/*---
---*/
const se = new SuppressedError("newErr", "oldErr", "suppressed msg");
assert(se instanceof SuppressedError, "instanceof SuppressedError");
assert(se instanceof Error, "instanceof Error");
assert.sameValue(Error.isError(se), true);
assert.sameValue(SuppressedError.length, 3);
assert.sameValue(se.name, "SuppressedError");
assert.sameValue(se.error, "newErr");
assert.sameValue(se.suppressed, "oldErr");
assert.sameValue(se.message, "suppressed msg");

const eBody = new Error("body");
const eD1 = new Error("d1");
const eD2 = new Error("d2");
let caught = null;
try {
  using r1 = { [Symbol.dispose]() { throw eD1; } };
  using r2 = { [Symbol.dispose]() { throw eD2; } };
  throw eBody;
} catch (err) {
  caught = err;
}
assert(caught instanceof SuppressedError, "outer SuppressedError");
assert.sameValue(caught.error, eD1);
assert(caught.suppressed instanceof SuppressedError, "inner SuppressedError");
assert.sameValue(caught.suppressed.error, eD2);
assert.sameValue(caught.suppressed.suppressed, eBody);
""")

    w("language/using-without-init-syntax-error.js", """/*---
negative:
  phase: parse
  type: SyntaxError
---*/
$DONOTEVALUATE();
{
  using x;
}
""")

    w("language/using-binding-pattern-syntax-error.js", """/*---
negative:
  phase: parse
  type: SyntaxError
---*/
$DONOTEVALUATE();
{
  using { a } = { a: 1, [Symbol.dispose]() {} };
}
""")

    w("language/using-for-in-syntax-error.js", """/*---
negative:
  phase: parse
  type: SyntaxError
---*/
$DONOTEVALUATE();
for (using x in { a: 1 }) {}
""")

    w("async/await-using-lifo-sync-fallback-and-for-await.js", """/*---
flags: [async]
---*/
(async () => {
  const events = [];
  {
    await using syncFallback = {
      [Symbol.dispose]() { events.push("sync-fallback"); }
    };
    await using asyncRes = {
      async [Symbol.asyncDispose]() {
        await Promise.resolve();
        events.push("async-res");
      }
    };
    await using nul = null;
    assert.sameValue(nul, null);
    events.push("body");
  }
  assert.sameValue(events.join(","), "body,async-res,sync-fallback");

  let agenClosed = false;
  async function* makeAsyncGen() {
    try {
      yield 10;
      yield 20;
    } finally {
      agenClosed = true;
    }
  }
  {
    await using ag = makeAsyncGen();
    const first = await ag.next();
    assert.sameValue(first.value, 10);
  }
  assert.sameValue(agenClosed, true);

  let caught = null;
  try {
    await using r1 = { [Symbol.asyncDispose]() { return Promise.reject("rej1"); } };
    throw "bodyErr";
  } catch (e) {
    caught = e;
  }
  assert(caught instanceof SuppressedError, "async SuppressedError");
  assert.sameValue(caught.error, "rej1");
  assert.sameValue(caught.suppressed, "bodyErr");

  const faLog = [];
  async function* asyncItems() {
    yield { id: 1, async [Symbol.asyncDispose]() { await Promise.resolve(); faLog.push("ad1"); } };
    yield { id: 2, async [Symbol.asyncDispose]() { await Promise.resolve(); faLog.push("ad2"); } };
  }
  for await (await using item of asyncItems()) {
    faLog.push("ab" + item.id);
  }
  assert.sameValue(faLog.join(","), "ab1,ad1,ab2,ad2");
})().then(() => $DONE(), $DONE);
""")

    w("modules/_fixtures/using_dep.js", """
export const modEvents = [];
{
  using s = { [Symbol.dispose]() { modEvents.push("dep-sync"); } };
  modEvents.push("dep-body");
}
await using topAsync = {
  async [Symbol.asyncDispose]() {
    await Promise.resolve();
    modEvents.push("dep-top-async");
  }
};
modEvents.push("dep-after-decl");
""")
    w("modules/top-level-using-and-await-using.js", """/*---
flags: [module, async]
---*/
import { modEvents } from "./_fixtures/using_dep.js";
assert.sameValue(modEvents.join(","), "dep-body,dep-sync,dep-after-decl,dep-top-async");
$DONE();
""")

    w("builtins/disposable-stack-and-async-disposable-stack.js", """/*---
flags: [async]
---*/
const syncLog = [];
const ds = new DisposableStack();
assert.sameValue(ds.disposed, false);
assert.sameValue(DisposableStack.prototype[Symbol.dispose], DisposableStack.prototype.dispose);

const rObj = { id: 1, [Symbol.dispose]() { syncLog.push("use"); } };
assert.sameValue(ds.use(rObj), rObj);
assert.sameValue(ds.use(null), null);
assert.sameValue(ds.adopt("handle", (v) => syncLog.push("adopt:" + v)), "handle");
ds.defer(() => syncLog.push("defer"));

assert.throws(TypeError, () => ds.adopt("x", null));
assert.throws(TypeError, () => ds.defer(123));

const moved = ds.move();
assert.sameValue(ds.disposed, true);
assert.sameValue(moved.disposed, false);
assert.throws(ReferenceError, () => ds.use(null));
assert.throws(ReferenceError, () => ds.move());

moved.dispose();
assert.sameValue(moved.disposed, true);
moved.dispose();
assert.sameValue(syncLog.join(","), "defer,adopt:handle,use");

(async () => {
  const asyncLog = [];
  const ads = new AsyncDisposableStack();
  assert.sameValue(ads.disposed, false);
  assert.sameValue(AsyncDisposableStack.prototype[Symbol.asyncDispose], AsyncDisposableStack.prototype.disposeAsync);

  ads.use({ [Symbol.dispose]() { asyncLog.push("sync-use"); } });
  ads.use({ async [Symbol.asyncDispose]() { await Promise.resolve(); asyncLog.push("async-use"); } });
  ads.adopt(7, async (v) => { await Promise.resolve(); asyncLog.push("adopt:" + v); });
  ads.defer(async () => { await Promise.resolve(); asyncLog.push("defer"); });

  const amoved = ads.move();
  assert.sameValue(ads.disposed, true);
  await amoved.disposeAsync();
  assert.sameValue(amoved.disposed, true);
  assert.sameValue(asyncLog.join(","), "defer,adopt:7,async-use,sync-use");
})().then(() => $DONE(), $DONE);
""")

    return harness_dir, test_dir


def discover_tests(test_dir: Path, filter_pat: Optional[str]) -> List[TestCase]:
    cases: List[TestCase] = []
    for p in sorted(test_dir.rglob("*.js")):
        rel = p.relative_to(test_dir).as_posix()
        # Skip Test262 fixture files (`*_FIXTURE.js` or `_fixtures/` directories)
        if p.name.endswith("_FIXTURE.js") or any(part.startswith("_") for part in p.relative_to(test_dir).parts):
            continue
        if filter_pat and (filter_pat not in rel and not fnmatch.fnmatch(rel, filter_pat)):
            continue
        source = p.read_text(encoding="utf-8", errors="replace")
        cat = p.relative_to(test_dir).parts[0] if len(p.relative_to(test_dir).parts) > 1 else "root"
        cases.extend(expand_test_variants(rel, cat, str(p), source))
    return cases


def main() -> int:
    ap = argparse.ArgumentParser(description="ArcradientJS 1.0 Test262 & Conformance Runner")
    ap.add_argument("--cli", "--binary", dest="cli", help="Path to arcr_js binary (auto-built if omitted)")
    ap.add_argument("--suite", "--test262-dir", dest="suite", help="Path to official Test262 test/ directory")
    ap.add_argument("--harness-dir", help="Path to Test262 harness/ directory")
    ap.add_argument("-j", "--jobs", type=int, default=max(1, os.cpu_count() or 4), help="Parallel worker count")
    ap.add_argument("-f", "--filter", help="Substring or glob filter on test ID")
    ap.add_argument("--timeout", "--timeout-sec", dest="timeout", type=float, default=10.0, help="Per-test timeout in seconds")
    ap.add_argument("--json-out", "--out-json", dest="json_out", help="Write JSON report to file")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    cli_path = ensure_cli_binary(repo_root, args.cli)

    tmp_suite_dir: Optional[Path] = None
    try:
        if args.suite:
            suite_path = Path(args.suite).resolve()
            if (suite_path / "test").is_dir() and (suite_path / "harness").is_dir():
                test_dir = suite_path / "test"
                harness_dir = Path(args.harness_dir).resolve() if args.harness_dir else (suite_path / "harness")
            else:
                test_dir = suite_path
                if args.harness_dir:
                    harness_dir = Path(args.harness_dir).resolve()
                elif (suite_path.parent / "harness").is_dir():
                    harness_dir = suite_path.parent / "harness"
                else:
                    tmp_suite_dir = Path(tempfile.mkdtemp(prefix="ajs_t262_harness_"))
                    harness_dir = tmp_suite_dir
                    for hname, hsrc in BUILTIN_HARNESS_FILES.items():
                        (harness_dir / hname).write_text(hsrc.lstrip(), encoding="utf-8")
            suite_label = f"External Test262 ({test_dir})"
        else:
            tmp_suite_dir = Path(tempfile.mkdtemp(prefix="ajs_builtin_t262_"))
            harness_dir, test_dir = build_builtin_suite(tmp_suite_dir)
            suite_label = "Built-in ES2026 + Annex B + ESM/TLA + ECMA-402 Intl + $262 + GC Suite"

        cases = discover_tests(test_dir, args.filter)
        if not cases:
            print("No matching tests found.", file=sys.stderr)
            return 2

        print(f"=== ArcradientJS Test262 Runner ===")
        print(f"CLI Binary : {cli_path}")
        print(f"Suite      : {suite_label}")
        print(f"Executions : {len(cases)} (workers={args.jobs})")

        t_start = time.perf_counter()
        results: List[TestResult] = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futs = [pool.submit(run_single_case, str(cli_path), harness_dir, tc, args.timeout) for tc in cases]
            for fut in concurrent.futures.as_completed(futs):
                results.append(fut.result())

        elapsed_s = time.perf_counter() - t_start
        results.sort(key=lambda r: (r.category, r.test_id, r.strict_mode))

        by_cat: Dict[str, List[TestResult]] = {}
        for r in results:
            by_cat.setdefault(r.category, []).append(r)

        print("\nCategory Breakdown:")
        for cat, items in sorted(by_cat.items()):
            p_cnt = sum(1 for x in items if x.passed)
            pct = (100.0 * p_cnt / len(items)) if items else 100.0
            print(f"  {cat:<14} {p_cnt:>4}/{len(items):<4} ({pct:5.1f}%)")

        passed = sum(1 for r in results if r.passed)
        failed = [r for r in results if not r.passed]
        rate = 100.0 * passed / len(results)
        print(f"\nTotal: {passed}/{len(results)} passed ({rate:.2f}%), {len(failed)} failed in {elapsed_s:.2f}s")

        if failed:
            print("\nFailures (up to 25):")
            for f in failed[:25]:
                mode = "strict" if f.strict_mode else "default"
                print(f"  [FAIL] {f.test_id} ({mode}): {f.reason}")

        if args.json_out:
            report = {
                "engine": "ArcradientJS/1.0",
                "total": len(results),
                "passed": passed,
                "failed": len(failed),
                "pass_rate_pct": round(rate, 2),
                "elapsed_sec": round(elapsed_s, 3),
                "categories": {
                    cat: {
                        "passed": sum(1 for x in items if x.passed),
                        "total": len(items),
                    }
                    for cat, items in sorted(by_cat.items())
                },
                "failures": [
                    {"id": f.test_id, "strict": f.strict_mode, "reason": f.reason}
                    for f in failed
                ],
            }
            Path(args.json_out).write_text(json.dumps(report, indent=2), encoding="utf-8")

        return 0 if not failed else 1
    finally:
        if tmp_suite_dir and tmp_suite_dir.exists():
            shutil.rmtree(tmp_suite_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
