import sys
sys.dont_write_bytecode = True

import dataclasses
from dataclasses import dataclass, field
import json
import math
import os
import re
import signal
import struct
import subprocess
import time
import typing
import zlib


# Authoritative Feature Inventory from /home/antigravity/.agents/teamwork/PROJECT.md (verbatim F01..F19)
AUTHORITATIVE_FEATURES: dict[str, dict[str, str]] = {
    "F01": {"milestone": "M1", "name": "Cloud /env-setup Automation"},
    "F02": {"milestone": "M1", "name": "ArcradientJS O(1) Mega-Bundle Compiler & Slot Scaling"},
    "F03": {"milestone": "M1", "name": "ArcradientJS Bytecode & GC Mega-Bundle Throughput"},
    "F04": {"milestone": "M1", "name": "ES2026 Explicit Resource Management"},
    "F05": {"milestone": "M1", "name": "ES2026 Built-ins & ECMA-402 13th Ed. Completion"},
    "F06": {"milestone": "M2", "name": "Long URL Support (>= 32 KB)"},
    "F07": {"milestone": "M2", "name": "Unified Cookie Jar & Private Ephemeral Cookies"},
    "F08": {"milestone": "M2", "name": "Dynamic <script> / <link> Loading & document.currentScript"},
    "F09": {"milestone": "M2", "name": "Complete Web IDL Globals, Prototypes & Legacy Event Init"},
    "F10": {"milestone": "M2", "name": "Constructable Stylesheets & Web Animations API"},
    "F11": {"milestone": "M2", "name": "Modern JS Heap/Turn Scaling & Removal of Deceptive Facades"},
    "F12": {"milestone": "M3", "name": "Shadow DOM <slot> Projection & Shadow CSS Scoping"},
    "F13": {"milestone": "M3", "name": "CSS Transitions, @import, @property, Math & Image Negotiation"},
    "F14": {"milestone": "M3", "name": "@font-face Web Fonts, Color Emoji & Symbol Fallback"},
    "F15": {"milestone": "M3", "name": "Real-Site CSS Layout, sr-only, SVG Sprite & Nested Scroll Fixes"},
    "F16": {"milestone": "M4", "name": "Non-Blocking Multicore Tab Pipeline & <16 ms Tab Switching"},
    "F17": {"milestone": "M4", "name": "Linux X11 Hangul XIM Input & Win32 Single-Frame Parity"},
    "F18": {"milestone": "M4", "name": "Live Chrome-Grade --conformance & Interactive X11 QA"},
    "F19": {"milestone": "M4", "name": "Clause-Honest standards_2026_inventory.json & Repo/Doc Sync"},
}


class AssertionFailure(Exception):
    """Raised inside a test function to report a clean requirement failure."""


def ensure(cond: typing.Any, msg: str) -> None:
    if not cond:
        raise AssertionFailure(msg)


@dataclass
class TestResult:
    test_id: str
    tier: int
    feature_id: str
    passed: bool
    diagnostic: str = ""
    skipped: bool = False
    duration_ms: float = 0.0
    related_features: list[str] = field(default_factory=list)
    details: dict[str, typing.Any] = field(default_factory=dict)


@dataclass
class TestCaseSpec:
    test_id: str
    tier: int
    feature_id: str
    description: str
    fn: typing.Callable[["RunnerContext"], typing.Union[TestResult, tuple[bool, str], None]]
    related_features: tuple[str, ...] = ()
    is_live: bool = False


@dataclass
class RunnerContext:
    waterscape_bin: str
    arcr_js_bin: str
    module_dir: str
    repo_root: str
    out_dir: str
    skip_live: bool
    fixture_srv: typing.Any = None

    def test_dir(self, test_id: str) -> str:
        safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", test_id)
        p = os.path.join(self.out_dir, safe)
        os.makedirs(p, exist_ok=True)
        return p


@dataclass
class ArcrJsRunResult:
    returncode: int
    stdout: str
    stderr: str
    duration_ms: float
    timed_out: bool = False
    peak_rss_kb: int = 0
    cmd: list[str] = field(default_factory=list)


def _kill_process_group(proc: subprocess.Popen) -> None:
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass


def _run_with_rusage(
    cmd: list[str],
    *,
    cwd: typing.Optional[str] = None,
    env: typing.Optional[dict[str, str]] = None,
    timeout_sec: float = 12.0,
) -> tuple[int, str, str, float, bool, int]:
    t0 = time.perf_counter()
    proc = subprocess.Popen(
        cmd,
        cwd=cwd,
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    peak_rss_kb = 0
    timed_out = False
    try:
        out_b, err_b = proc.communicate(timeout=timeout_sec)
    except subprocess.TimeoutExpired:
        timed_out = True
        _kill_process_group(proc)
        try:
            out_b, err_b = proc.communicate(timeout=2.0)
        except Exception:
            out_b, err_b = b"", b""
    finally:
        if proc.poll() is None:
            _kill_process_group(proc)
            try:
                proc.wait(timeout=1.0)
            except Exception:
                pass
    dt_ms = (time.perf_counter() - t0) * 1000.0
    rc = -9 if timed_out else (proc.returncode if proc.returncode is not None else -1)
    return (
        rc,
        out_b.decode("utf-8", errors="replace"),
        err_b.decode("utf-8", errors="replace"),
        dt_ms,
        timed_out,
        peak_rss_kb,
    )


def run_arcr_js(
    ctx: RunnerContext,
    *,
    code: typing.Optional[str] = None,
    script_path: typing.Optional[str] = None,
    module: bool = False,
    strict: bool = False,
    test262: bool = False,
    harness_files: typing.Optional[list[str]] = None,
    heap_limit_mb: typing.Optional[int] = None,
    max_time_ms: typing.Optional[int] = None,
    cwd: typing.Optional[str] = None,
    timeout_sec: float = 12.0,
    extra_args: typing.Optional[list[str]] = None,
    measure_rss: bool = False,
) -> ArcrJsRunResult:
    cmd: list[str] = []
    rss_file: typing.Optional[str] = None
    if measure_rss and os.path.exists("/usr/bin/time"):
        rss_file = os.path.join(ctx.out_dir, f"_rss_{os.getpid()}_{time.monotonic_ns()}.txt")
        cmd.extend(["/usr/bin/time", "-f", "%M", "-o", rss_file])
    cmd.append(ctx.arcr_js_bin)
    if module:
        cmd.append("-m")
    if strict:
        cmd.append("--strict")
    if test262:
        cmd.append("--test262")
    if heap_limit_mb is not None:
        cmd.extend(["--heap-limit", str(heap_limit_mb)])
    if max_time_ms is not None:
        cmd.extend(["--max-time-ms", str(max_time_ms)])
    if harness_files:
        for hf in harness_files:
            cmd.extend(["--harness", hf])
    if extra_args:
        cmd.extend(extra_args)
    if code is not None:
        cmd.extend(["-e", code])
    if script_path is not None:
        cmd.append(script_path)

    env = os.environ.copy()
    env["LC_ALL"] = "C.UTF-8"
    rc, out_s, err_s, dt_ms, timed_out, peak_rss = _run_with_rusage(
        cmd, cwd=cwd or ctx.out_dir, env=env, timeout_sec=timeout_sec
    )
    if rss_file:
        try:
            if os.path.exists(rss_file):
                raw_rss = open(rss_file, "r", encoding="utf-8").read().strip().splitlines()
                if raw_rss:
                    peak_rss = int(raw_rss[-1].strip())
        except Exception:
            pass
        finally:
            try:
                os.remove(rss_file)
            except OSError:
                pass
    return ArcrJsRunResult(
        returncode=rc,
        stdout=out_s,
        stderr=err_s,
        duration_ms=dt_ms,
        timed_out=timed_out,
        peak_rss_kb=peak_rss,
        cmd=cmd,
    )


class PNGImage:
    """Pure-Python RGBA8 PNG image wrapper with pixel & visual layout analyzers."""

    def __init__(self, width: int, height: int, rgba: typing.Union[bytes, bytearray, memoryview]):
        self.width = width
        self.height = height
        self.rgba = memoryview(rgba)
        self.stride = width * 4

    def pixel(self, x: int, y: int) -> tuple[int, int, int, int]:
        if x < 0 or x >= self.width or y < 0 or y >= self.height:
            raise IndexError(f"Pixel ({x}, {y}) outside {self.width}x{self.height}")
        off = y * self.stride + x * 4
        b = self.rgba
        return (b[off], b[off + 1], b[off + 2], b[off + 3])

    def _clamp_rect(
        self, rect: typing.Optional[tuple[int, int, int, int]]
    ) -> tuple[int, int, int, int]:
        if rect is None:
            return (0, 0, self.width, self.height)
        x0, y0, w, h = rect
        x1 = min(self.width, max(0, x0 + w))
        y1 = min(self.height, max(0, y0 + h))
        x0 = min(self.width, max(0, x0))
        y0 = min(self.height, max(0, y0))
        return (x0, y0, max(0, x1 - x0), max(0, y1 - y0))

    def region_stats(
        self, x0: int = 0, y0: int = 0, w: typing.Optional[int] = None, h: typing.Optional[int] = None
    ) -> dict[str, typing.Any]:
        rw = self.width - x0 if w is None else w
        rh = self.height - y0 if h is None else h
        x0, y0, rw, rh = self._clamp_rect((x0, y0, rw, rh))
        if rw == 0 or rh == 0:
            return {
                "count": 0,
                "mean_rgb": (0.0, 0.0, 0.0),
                "distinct_colors": 0,
                "non_white": 0,
                "max_chroma": 0,
                "warm_pixels": 0,
                "saturated_pixels": 0,
            }
        sr = sg = sb = 0
        non_white = 0
        max_chroma = 0
        warm_pixels = 0
        saturated_pixels = 0
        colors: set[int] = set()
        buf = self.rgba
        stride = self.stride
        for y in range(y0, y0 + rh):
            row_off = y * stride + x0 * 4
            for i in range(0, rw * 4, 4):
                r = buf[row_off + i]
                g = buf[row_off + i + 1]
                b = buf[row_off + i + 2]
                sr += r
                sg += g
                sb += b
                if len(colors) < 4096:
                    colors.add((r << 16) | (g << 8) | b)
                if r < 250 or g < 250 or b < 250:
                    non_white += 1
                mx = r if (r >= g and r >= b) else (g if g >= b else b)
                mn = r if (r <= g and r <= b) else (g if g <= b else b)
                ch = mx - mn
                if ch > max_chroma:
                    max_chroma = ch
                if ch >= 32:
                    saturated_pixels += 1
                if r > 180 and (r - b) >= 70:
                    warm_pixels += 1
        total = rw * rh
        return {
            "count": total,
            "mean_rgb": (sr / total, sg / total, sb / total),
            "distinct_colors": len(colors),
            "non_white": non_white,
            "max_chroma": max_chroma,
            "warm_pixels": warm_pixels,
            "saturated_pixels": saturated_pixels,
        }

    def count_color_near(
        self,
        target_rgb: tuple[int, int, int],
        tol: int = 16,
        rect: typing.Optional[tuple[int, int, int, int]] = None,
    ) -> int:
        x0, y0, rw, rh = self._clamp_rect(rect)
        tr, tg, tb = target_rgb
        cnt = 0
        buf = self.rgba
        stride = self.stride
        for y in range(y0, y0 + rh):
            row_off = y * stride + x0 * 4
            for i in range(0, rw * 4, 4):
                if (
                    abs(buf[row_off + i] - tr) <= tol
                    and abs(buf[row_off + i + 1] - tg) <= tol
                    and abs(buf[row_off + i + 2] - tb) <= tol
                ):
                    cnt += 1
        return cnt

    def has_bleed_color(
        self,
        bleed_rgbs: list[tuple[int, int, int]],
        tol: int = 20,
        rect: typing.Optional[tuple[int, int, int, int]] = None,
    ) -> int:
        return sum(self.count_color_near(c, tol=tol, rect=rect) for c in bleed_rgbs)

    @staticmethod
    def srgb_rel_luminance(rgb: tuple[int, int, int]) -> float:
        def chan(v: int) -> float:
            c = v / 255.0
            return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4

        return 0.2126 * chan(rgb[0]) + 0.7152 * chan(rgb[1]) + 0.0722 * chan(rgb[2])

    @classmethod
    def wcag_contrast_ratio(cls, c1: tuple[int, int, int], c2: tuple[int, int, int]) -> float:
        l1 = cls.srgb_rel_luminance(c1)
        l2 = cls.srgb_rel_luminance(c2)
        hi, lo = (l1, l2) if l1 >= l2 else (l2, l1)
        return (hi + 0.05) / (lo + 0.05)

    def region_max_contrast(self, x0: int, y0: int, w: int, h: int) -> float:
        x0, y0, rw, rh = self._clamp_rect((x0, y0, w, h))
        if rw == 0 or rh == 0:
            return 1.0
        min_lum = 1.0
        max_lum = 0.0
        buf = self.rgba
        stride = self.stride
        for y in range(y0, y0 + rh):
            row_off = y * stride + x0 * 4
            for i in range(0, rw * 4, 4):
                lum = self.srgb_rel_luminance((buf[row_off + i], buf[row_off + i + 1], buf[row_off + i + 2]))
                if lum < min_lum:
                    min_lum = lum
                if lum > max_lum:
                    max_lum = lum
        return (max_lum + 0.05) / (min_lum + 0.05)

    def count_grayscale_aa_levels(self, x0: int, y0: int, w: int, h: int) -> int:
        x0, y0, rw, rh = self._clamp_rect((x0, y0, w, h))
        levels: set[int] = set()
        buf = self.rgba
        stride = self.stride
        for y in range(y0, y0 + rh):
            row_off = y * stride + x0 * 4
            for i in range(0, rw * 4, 4):
                lum_i = (buf[row_off + i] * 77 + buf[row_off + i + 1] * 150 + buf[row_off + i + 2] * 29) >> 8
                if 8 <= lum_i <= 247:
                    levels.add(lum_i)
        return len(levels)

    def detect_crossed_box_tofu(
        self,
        x0: int,
        y0: int,
        w: int,
        h: int,
        bg_rgb: tuple[int, int, int] = (255, 255, 255),
    ) -> bool:
        """
        Detects synthetic fallback_glyph 4-walled rectangle/crossed-box tofu inside (x0, y0, w, h).
        Finds connected or column-separated glyph ink bounding boxes of size >= 10x12 and checks
        if all 4 outer perimeter edges (top, bottom, left, right) have >= 82% ink coverage AND
        all 4 corners have ink.
        """
        x0, y0, rw, rh = self._clamp_rect((x0, y0, w, h))
        if rw < 10 or rh < 12:
            return False
        br, bg_g, bb = bg_rgb
        buf = self.rgba
        stride = self.stride

        def is_ink(px: int, py: int) -> bool:
            off = py * stride + px * 4
            return (
                abs(buf[off] - br) > 35
                or abs(buf[off + 1] - bg_g) > 35
                or abs(buf[off + 2] - bb) > 35
            )

        # Segment horizontal runs of columns containing ink
        col_has_ink = [any(is_ink(x, y) for y in range(y0, y0 + rh)) for x in range(x0, x0 + rw)]
        spans: list[tuple[int, int]] = []
        in_span = False
        s_start = 0
        for idx, has in enumerate(col_has_ink):
            if has and not in_span:
                in_span = True
                s_start = x0 + idx
            elif not has and in_span:
                in_span = False
                spans.append((s_start, x0 + idx - 1))
        if in_span:
            spans.append((s_start, x0 + rw - 1))

        for sx0, sx1 in spans:
            gw = sx1 - sx0 + 1
            if gw < 8:
                continue
            ink_rows = [y for y in range(y0, y0 + rh) if any(is_ink(x, y) for x in range(sx0, sx1 + 1))]
            if not ink_rows:
                continue
            sy0, sy1 = ink_rows[0], ink_rows[-1]
            gh = sy1 - sy0 + 1
            if gh < 10:
                continue
            top_cov = sum(1 for x in range(sx0, sx1 + 1) if is_ink(x, sy0)) / gw
            bot_cov = sum(1 for x in range(sx0, sx1 + 1) if is_ink(x, sy1)) / gw
            left_cov = sum(1 for y in range(sy0, sy1 + 1) if is_ink(sx0, y)) / gh
            right_cov = sum(1 for y in range(sy0, sy1 + 1) if is_ink(sx1, y)) / gh
            corners = sum(
                1 for cx, cy in ((sx0, sy0), (sx1, sy0), (sx0, sy1), (sx1, sy1)) if is_ink(cx, cy)
            )
            if top_cov >= 0.82 and bot_cov >= 0.82 and left_cov >= 0.82 and right_cov >= 0.82 and corners == 4:
                return True
        return False

    def is_blank_or_uniform(self) -> tuple[bool, str]:
        st = self.region_stats()
        if st["count"] == 0:
            return True, "0x0 empty image"
        if st["distinct_colors"] <= 1:
            return True, f"Uniform single-color image (mean_rgb={st['mean_rgb']})"
        return False, ""

    def is_grey_skeleton_only(self) -> tuple[bool, str]:
        st = self.region_stats()
        if st["distinct_colors"] <= 1:
            return True, "Uniform blank image"
        if st["non_white"] < 40:
            return True, f"Almost completely white/empty ({st['non_white']} non-white px)"
        if st["max_chroma"] < 12 and st["saturated_pixels"] == 0:
            return True, f"Grey-skeleton / monochrome-only render (max_chroma={st['max_chroma']})"
        return False, ""


def read_png_rgba(path: str) -> PNGImage:
    """Pure-Python stdlib (struct + zlib) PNG decoder for RGBA8 (color_type=6) and RGB8 (color_type=2)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"Invalid PNG magic in {path}")
    pos = 8
    w = h = bit_depth = color_type = interlace = 0
    idat = bytearray()
    while pos + 8 <= len(data):
        length, ctype = struct.unpack_from(">I4s", data, pos)
        chunk_start = pos + 8
        chunk_end = chunk_start + length
        if chunk_end + 4 > len(data):
            raise ValueError(f"Truncated PNG chunk {ctype!r} in {path}")
        chunk = data[chunk_start:chunk_end]
        pos = chunk_end + 4
        if ctype == b"IHDR":
            w, h, bit_depth, color_type, _comp, _flt, interlace = struct.unpack_from(">IIBBBBB", chunk, 0)
            if bit_depth != 8 or color_type not in (2, 6) or interlace != 0:
                raise ValueError(
                    f"Unsupported PNG format bit_depth={bit_depth} color_type={color_type} interlace={interlace}"
                )
        elif ctype == b"IDAT":
            idat.extend(chunk)
        elif ctype == b"IEND":
            break
    if w <= 0 or h <= 0:
        raise ValueError(f"Invalid PNG dimensions {w}x{h} in {path}")
    bpp = 4 if color_type == 6 else 3
    raw = zlib.decompress(bytes(idat))
    in_stride = w * bpp
    expected_raw = h * (1 + in_stride)
    if len(raw) < expected_raw:
        raise ValueError(f"Decompressed IDAT too short ({len(raw)} < {expected_raw})")

    recon = bytearray(h * in_stride)
    rpos = 0
    for y in range(h):
        flt = raw[rpos]
        rpos += 1
        row = bytearray(raw[rpos : rpos + in_stride])
        rpos += in_stride
        prev_off = (y - 1) * in_stride
        if flt == 0:
            pass
        elif flt == 1:  # Sub
            for i in range(bpp, in_stride):
                row[i] = (row[i] + row[i - bpp]) & 0xFF
        elif flt == 2:  # Up
            if y > 0:
                for i in range(in_stride):
                    row[i] = (row[i] + recon[prev_off + i]) & 0xFF
        elif flt == 3:  # Average
            for i in range(in_stride):
                a = row[i - bpp] if i >= bpp else 0
                b = recon[prev_off + i] if y > 0 else 0
                row[i] = (row[i] + ((a + b) >> 1)) & 0xFF
        elif flt == 4:  # Paeth
            for i in range(in_stride):
                a = row[i - bpp] if i >= bpp else 0
                b = recon[prev_off + i] if y > 0 else 0
                c = recon[prev_off + i - bpp] if (y > 0 and i >= bpp) else 0
                p = a + b - c
                pa = p - a if p >= a else a - p
                pb = p - b if p >= b else b - p
                pc = p - c if p >= c else c - p
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[i] = (row[i] + pr) & 0xFF
        else:
            raise ValueError(f"Invalid PNG scanline filter {flt} at row {y}")
        recon[y * in_stride : (y + 1) * in_stride] = row

    if bpp == 4:
        return PNGImage(w, h, recon)
    out_rgba = bytearray(w * h * 4)
    si = 0
    di = 0
    for _ in range(w * h):
        out_rgba[di] = recon[si]
        out_rgba[di + 1] = recon[si + 1]
        out_rgba[di + 2] = recon[si + 2]
        out_rgba[di + 3] = 255
        si += 3
        di += 4
    return PNGImage(w, h, out_rgba)


def write_png_rgba(path: str, width: int, height: int, rgba: bytes) -> None:
    """Pure-Python RGBA8 PNG writer used by fixtures and self-tests."""
    if len(rgba) != width * height * 4:
        raise ValueError("rgba buffer length mismatch")

    def chunk(tag: bytes, payload: bytes) -> bytes:
        crc = zlib.crc32(tag + payload) & 0xFFFFFFFF
        return struct.pack(">I4s", len(payload), tag) + payload + struct.pack(">I", crc)

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)
        raw.extend(rgba[y * stride : (y + 1) * stride])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


_CONSOLE_ERROR_RE = re.compile(
    r"(SyntaxError|TypeError|ReferenceError|RangeError|Out of memory|Script execution interrupted|\[Unhandled Promise Rejection\])"
)


@dataclass
class WaterscapeVerifyResult:
    returncode: int
    stdout: str
    stderr: str
    duration_ms: float
    timed_out: bool
    dump_json: dict[str, typing.Any]
    png_path: typing.Optional[str]
    content_png_path: typing.Optional[str]
    png: typing.Optional[PNGImage]
    content_png: typing.Optional[PNGImage]
    guard_failures: list[str]
    cmd: list[str]

    @property
    def ok(self) -> bool:
        return len(self.guard_failures) == 0


def validate_anti_false_positive_guards(
    *,
    returncode: int,
    timed_out: bool,
    dump_json: typing.Optional[dict[str, typing.Any]],
    png_img: typing.Optional[PNGImage],
    expect_png: bool = True,
    allow_http_errors: bool = False,
    allow_console_errors: bool = False,
    allow_non_complete: bool = False,
    allow_blank_png: bool = False,
) -> list[str]:
    """
    Mandatory Anti-False-Positive Guards (TEST_INFRA.md §3.2):
    1. Clean exit == 0 (no crash signal or timeout).
    2. HTTP & Redirect integrity (no 403/404/5xx on navigation, no /sorry/index, no /_compat/entry).
    3. Zero uncaught JS console errors / OOM / turn-interrupts / unhandled rejections.
    4. Document lifecycle ready_state == 'complete'.
    5. Non-blank rendered PNG verification.
    """
    failures: list[str] = []
    if timed_out:
        failures.append("waterscape process timed out")
        return failures
    if returncode != 0:
        failures.append(f"waterscape exited with non-zero code {returncode}")
    if not dump_json or not isinstance(dump_json, dict):
        failures.append("missing or invalid --dump-json telemetry output")
        return failures

    final_url = str(dump_json.get("url", ""))
    if "/sorry/index" in final_url:
        failures.append(f"bot-challenge redirect detected in final url: {final_url}")
    if "/_compat/entry" in final_url:
        failures.append(f"legacy compatibility downgrade redirect detected in final url: {final_url}")

    if not allow_non_complete:
        rs = dump_json.get("ready_state")
        if rs != "complete":
            failures.append(f"document ready_state is {rs!r} (expected 'complete')")

    if not allow_http_errors:
        traces = dump_json.get("traces") or []
        nav_traces = [t for t in traces if t.get("initiator") == "navigation"]
        for nt in nav_traces:
            sc = int(nt.get("status_code", 0))
            turl = str(nt.get("final_url") or nt.get("url") or "")
            if "/sorry/index" in turl:
                failures.append(f"navigation trace hit /sorry/index ({turl})")
            if "/_compat/entry" in turl:
                failures.append(f"navigation trace hit /_compat/entry ({turl})")
            if sc == 403 or sc == 404 or sc >= 500 or sc == 0:
                if not turl.startswith(("waterscape://", "about:", "data:")):
                    failures.append(f"navigation HTTP failure status={sc} for {turl[:120]}")

    if not allow_console_errors:
        clog = str(dump_json.get("console_log", ""))
        m = _CONSOLE_ERROR_RE.search(clog)
        if m:
            snippet = clog.strip().replace("\n", " | ")[:240]
            failures.append(f"console JS error ({m.group(1)}): {snippet}")

    if expect_png and not allow_blank_png:
        stats = dump_json.get("stats") or {}
        if int(stats.get("display_cmd_count", 0)) <= 0:
            failures.append("stats.display_cmd_count <= 0 (nothing painted)")
        if png_img is None:
            failures.append("rendered PNG output missing or unreadable")
        else:
            is_blank, reason = png_img.is_blank_or_uniform()
            if is_blank:
                failures.append(f"rendered PNG failed anti-blank check: {reason}")
    return failures


def run_waterscape_verify(
    ctx: RunnerContext,
    url: str,
    *,
    test_id: str,
    conformance: bool = True,
    private: bool = True,
    profile: typing.Optional[str] = "modern",
    theme: typing.Optional[str] = None,
    window_size: str = "1280x800",
    wait_load_ms: int = 4000,
    second_url: typing.Optional[str] = None,
    early_click: typing.Optional[tuple[int, int]] = None,
    click: typing.Optional[tuple[int, int]] = None,
    eval_js: typing.Optional[str] = None,
    scroll_y: typing.Optional[int] = None,
    dpi: typing.Optional[int] = None,
    devtools: typing.Optional[str] = None,
    force_classic_frame: bool = False,
    force_cpu: bool = False,
    save_png: bool = False,
    save_content_png: bool = True,
    env_overrides: typing.Optional[dict[str, str]] = None,
    timeout_sec: float = 16.0,
    check_guards: bool = True,
    allow_http_errors: bool = False,
    allow_console_errors: bool = False,
    allow_non_complete: bool = False,
    allow_blank_png: bool = False,
) -> WaterscapeVerifyResult:
    tdir = ctx.test_dir(test_id)
    run_tag = str(time.monotonic_ns())
    json_path = os.path.join(tdir, f"dump_{run_tag}.json")
    png_path = os.path.join(tdir, f"win_{run_tag}.png") if save_png else None
    cpng_path = os.path.join(tdir, f"content_{run_tag}.png") if save_content_png else None

    cmd = [
        ctx.waterscape_bin,
        "--module-dir",
        ctx.module_dir,
        "--window-size",
        window_size,
        "--wait-load-ms",
        str(wait_load_ms),
        "--verify-url",
        url,
        "--dump-json",
        json_path,
    ]
    if conformance:
        cmd.append("--conformance")
    if private:
        cmd.append("--private")
    if profile:
        cmd.extend(["--profile", profile])
    if theme:
        cmd.extend(["--theme", theme])
    if second_url:
        cmd.extend(["--second-url", second_url])
    if early_click is not None:
        cmd.extend(["--early-click", f"{early_click[0]},{early_click[1]}"])
    if click is not None:
        cmd.extend(["--click", f"{click[0]},{click[1]}"])
    if eval_js is not None:
        cmd.extend(["--eval-js", eval_js])
    if scroll_y is not None:
        cmd.extend(["--scroll-y", str(scroll_y)])
    if dpi is not None:
        cmd.extend(["--dpi", str(dpi)])
    if devtools is not None:
        cmd.extend(["--devtools", devtools])
    if force_classic_frame:
        cmd.append("--force-classic-frame")
    if force_cpu:
        cmd.append("--force-cpu")
    if png_path:
        cmd.extend(["--save-png", png_path])
    if cpng_path:
        cmd.extend(["--save-content-png", cpng_path])

    iso_home = os.path.join(tdir, "iso_home")
    iso_prof = os.path.join(tdir, "ws_prof")
    os.makedirs(iso_home, exist_ok=True)
    os.makedirs(iso_prof, exist_ok=True)

    env = os.environ.copy()
    env["LC_ALL"] = "C.UTF-8"
    env["HOME"] = iso_home
    env["WATERSCAPE_PROFILE_DIR"] = iso_prof
    env["WATERSCAPE_DOWNLOADS_DIR"] = os.path.join(tdir, "downloads")
    if env_overrides:
        env.update(env_overrides)

    rc, out_s, err_s, dt_ms, timed_out, _ = _run_with_rusage(
        cmd, cwd=tdir, env=env, timeout_sec=timeout_sec
    )

    dump_obj: dict[str, typing.Any] = {}
    if os.path.exists(json_path):
        try:
            with open(json_path, "r", encoding="utf-8", errors="replace") as f:
                dump_obj = json.load(f)
        except Exception:
            dump_obj = {}

    png_img: typing.Optional[PNGImage] = None
    cpng_img: typing.Optional[PNGImage] = None
    if png_path and os.path.exists(png_path):
        try:
            png_img = read_png_rgba(png_path)
        except Exception:
            png_img = None
    if cpng_path and os.path.exists(cpng_path):
        try:
            cpng_img = read_png_rgba(cpng_path)
        except Exception:
            cpng_img = None

    guard_failures: list[str] = []
    if check_guards:
        primary_img = cpng_img if save_content_png else png_img
        guard_failures = validate_anti_false_positive_guards(
            returncode=rc,
            timed_out=timed_out,
            dump_json=dump_obj,
            png_img=primary_img,
            expect_png=(save_content_png or save_png),
            allow_http_errors=allow_http_errors,
            allow_console_errors=allow_console_errors,
            allow_non_complete=allow_non_complete,
            allow_blank_png=allow_blank_png,
        )

    return WaterscapeVerifyResult(
        returncode=rc,
        stdout=out_s,
        stderr=err_s,
        duration_ms=dt_ms,
        timed_out=timed_out,
        dump_json=dump_obj,
        png_path=png_path,
        content_png_path=cpng_path,
        png=png_img,
        content_png=cpng_img,
        guard_failures=guard_failures,
        cmd=cmd,
    )
