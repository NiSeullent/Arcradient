import sys
sys.dont_write_bytecode = True

import ctypes
import os
import signal
import subprocess
import threading
import time
import typing

from .types_and_guards import PNGImage


# X11 Keysyms
XK_BackSpace = 0xFF08
XK_Tab = 0xFF09
XK_Return = 0xFF0D
XK_Escape = 0xFF1B
XK_Multi_key = 0xFF20
XK_F6 = 0xFFC3
XK_F11 = 0xFFC8
XK_F12 = 0xFFC9
XK_Shift_L = 0xFFE1
XK_Control_L = 0xFFE3
XK_Alt_L = 0xFFE9

# Pre-mapped custom keycodes on Xvfb display
KC_MULTI_KEY = 249
KC_HANGUL_HAN = 250  # U+D55C '한' -> 0x0100D55C
KC_HANGUL_GEUL = 251  # U+AE00 '글' -> 0x0100AE00


class _XWindowAttributes(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_int),
        ("y", ctypes.c_int),
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("border_width", ctypes.c_int),
        ("depth", ctypes.c_int),
        ("visual", ctypes.c_void_p),
        ("root", ctypes.c_ulong),
        ("c_class", ctypes.c_int),
        ("bit_gravity", ctypes.c_int),
        ("win_gravity", ctypes.c_int),
        ("backing_store", ctypes.c_int),
        ("backing_planes", ctypes.c_ulong),
        ("backing_pixel", ctypes.c_ulong),
        ("save_under", ctypes.c_int),
        ("colormap", ctypes.c_ulong),
        ("map_installed", ctypes.c_int),
        ("map_state", ctypes.c_int),
        ("all_event_masks", ctypes.c_long),
        ("your_event_mask", ctypes.c_long),
        ("do_not_propagate_mask", ctypes.c_long),
        ("override_redirect", ctypes.c_int),
        ("screen", ctypes.c_void_p),
    ]


class _XClientMessageData(ctypes.Union):
    _fields_ = [
        ("b", ctypes.c_char * 20),
        ("s", ctypes.c_short * 10),
        ("l", ctypes.c_long * 5),
    ]


class _XClientMessageEvent(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int),
        ("serial", ctypes.c_ulong),
        ("send_event", ctypes.c_int),
        ("display", ctypes.c_void_p),
        ("window", ctypes.c_ulong),
        ("message_type", ctypes.c_ulong),
        ("format", ctypes.c_int),
        ("data", _XClientMessageData),
        ("pad", ctypes.c_long * 12),
    ]


_XErrorHandlerType = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)
_LAST_X_ERROR = 0


@_XErrorHandlerType
def _silent_x_error_handler(_dpy: int, _err_ev: int) -> int:
    global _LAST_X_ERROR
    _LAST_X_ERROR += 1
    return 0


_X11_ENV_LOCK = threading.Lock()
_LIBX11 = None
_LIBXTST = None


def _load_x11_libs() -> tuple[ctypes.CDLL, ctypes.CDLL]:
    global _LIBX11, _LIBXTST
    if _LIBX11 is not None and _LIBXTST is not None:
        return _LIBX11, _LIBXTST
    x11 = ctypes.CDLL("libX11.so.6")
    xtst = ctypes.CDLL("libXtst.so.6")

    x11.XSetErrorHandler.argtypes = [_XErrorHandlerType]
    x11.XSetErrorHandler.restype = ctypes.c_void_p
    x11.XSetErrorHandler(_silent_x_error_handler)

    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    x11.XCloseDisplay.restype = ctypes.c_int
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x11.XDefaultRootWindow.restype = ctypes.c_ulong
    x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XSync.restype = ctypes.c_int
    x11.XFlush.argtypes = [ctypes.c_void_p]
    x11.XFlush.restype = ctypes.c_int

    x11.XQueryTree.argtypes = [
        ctypes.c_void_p,
        ctypes.c_ulong,
        ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
        ctypes.POINTER(ctypes.c_uint),
    ]
    x11.XQueryTree.restype = ctypes.c_int
    x11.XFree.argtypes = [ctypes.c_void_p]
    x11.XFree.restype = ctypes.c_int

    x11.XFetchName.argtypes = [
        ctypes.c_void_p,
        ctypes.c_ulong,
        ctypes.POINTER(ctypes.c_char_p),
    ]
    x11.XFetchName.restype = ctypes.c_int

    x11.XGetWindowAttributes.argtypes = [
        ctypes.c_void_p,
        ctypes.c_ulong,
        ctypes.POINTER(_XWindowAttributes),
    ]
    x11.XGetWindowAttributes.restype = ctypes.c_int

    x11.XTranslateCoordinates.argtypes = [
        ctypes.c_void_p,
        ctypes.c_ulong,
        ctypes.c_ulong,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_ulong),
    ]
    x11.XTranslateCoordinates.restype = ctypes.c_int

    x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    x11.XSetInputFocus.restype = ctypes.c_int

    x11.XResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_uint, ctypes.c_uint]
    x11.XResizeWindow.restype = ctypes.c_int

    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x11.XInternAtom.restype = ctypes.c_ulong

    x11.XGetAtomName.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x11.XGetAtomName.restype = ctypes.c_void_p

    x11.XGetWindowProperty.argtypes = [
        ctypes.c_void_p,
        ctypes.c_ulong,
        ctypes.c_ulong,
        ctypes.c_long,
        ctypes.c_long,
        ctypes.c_int,
        ctypes.c_ulong,
        ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    x11.XGetWindowProperty.restype = ctypes.c_int

    x11.XSendEvent.argtypes = [
        ctypes.c_void_p,
        ctypes.c_ulong,
        ctypes.c_int,
        ctypes.c_long,
        ctypes.c_void_p,
    ]
    x11.XSendEvent.restype = ctypes.c_int

    x11.XGetImage.argtypes = [
        ctypes.c_void_p,
        ctypes.c_ulong,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_uint,
        ctypes.c_uint,
        ctypes.c_ulong,
        ctypes.c_int,
    ]
    x11.XGetImage.restype = ctypes.c_void_p
    x11.XGetPixel.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    x11.XGetPixel.restype = ctypes.c_ulong
    x11.XDestroyImage.argtypes = [ctypes.c_void_p]
    x11.XDestroyImage.restype = ctypes.c_int

    x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x11.XKeysymToKeycode.restype = ctypes.c_ubyte
    x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
    x11.XStringToKeysym.restype = ctypes.c_ulong

    x11.XChangeKeyboardMapping.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_ulong),
        ctypes.c_int,
    ]
    x11.XChangeKeyboardMapping.restype = ctypes.c_int

    xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
    xtst.XTestFakeKeyEvent.restype = ctypes.c_int
    xtst.XTestFakeMotionEvent.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_ulong,
    ]
    xtst.XTestFakeMotionEvent.restype = ctypes.c_int
    xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
    xtst.XTestFakeButtonEvent.restype = ctypes.c_int

    _LIBX11, _LIBXTST = x11, xtst
    return x11, xtst


_SHIFTED_ASCII: dict[str, str] = {
    ":": "colon",
    "?": "question",
    "&": "ampersand",
    "=": None,  # unshifted equal!
    "+": "plus",
    "_": "underscore",
    "%": "percent",
    "#": "numbersign",
    "!": "exclam",
    "@": "at",
    "~": "asciitilde",
    "(": "parenleft",
    ")": "parenright",
}

_UNSHIFTED_PUNCT: dict[str, str] = {
    "/": "slash",
    ".": "period",
    "-": "minus",
    "=": "equal",
    ",": "comma",
    ";": "semicolon",
    " ": "space",
}


class X11Session:
    """
    Context-managed live `xvfb-run -a -s "-screen 0 1600x1000x24"` desktop session
    with ctypes libX11 + libXtst input, Hangul XCOMPOSEFILE mapping, and XGetImage verification.
    """

    def __init__(
        self,
        waterscape_bin: str,
        module_dir: str,
        session_dir: str,
        *,
        initial_url: typing.Optional[str] = None,
        extra_args: typing.Optional[list[str]] = None,
        env_overrides: typing.Optional[dict[str, str]] = None,
        screen_spec: str = "-screen 0 1600x1000x24",
    ):
        self.waterscape_bin = waterscape_bin
        self.module_dir = module_dir
        self.session_dir = session_dir
        self.initial_url = initial_url
        self.extra_args = list(extra_args or [])
        self.env_overrides = dict(env_overrides or {})
        self.screen_spec = screen_spec

        self.x11, self.xtst = _load_x11_libs()
        self.dpy: typing.Optional[int] = None
        self.root_win: int = 0
        self.proc: typing.Optional[subprocess.Popen] = None
        self.display_str = ""
        self.profile_dir = os.path.join(self.session_dir, "profile")
        self.home_dir = os.path.join(self.session_dir, "home")
        self.compose_path = os.path.join(self.session_dir, "Compose")
        self.stdout_path = os.path.join(self.session_dir, "ws_stdout.log")
        self.stderr_path = os.path.join(self.session_dir, "ws_stderr.log")
        self._stdout_f: typing.Any = None
        self._stderr_f: typing.Any = None

    def __enter__(self) -> "X11Session":
        os.makedirs(self.session_dir, exist_ok=True)
        os.makedirs(self.profile_dir, exist_ok=True)
        os.makedirs(self.home_dir, exist_ok=True)
        os.makedirs(os.path.join(self.session_dir, "downloads"), exist_ok=True)

        with open(self.compose_path, "w", encoding="utf-8") as f:
            f.write(
                '<Multi_key> <r> <k> : "가" UAC00\n'
                '<Multi_key> <g> <k> <s> : "한" UD55C\n'
                '<Multi_key> <h> <a> <n> : "한글"\n'
            )

        env_file = os.path.join(self.session_dir, "x11_env.txt")
        go_file = os.path.join(self.session_dir, "x11_go")

        ws_cmd = [self.waterscape_bin, "--module-dir", self.module_dir]
        ws_cmd.extend(self.extra_args)
        if self.initial_url:
            ws_cmd.extend(["--url", self.initial_url])

        wrapper_script = (
            f'printf "%s\\n%s\\n" "$DISPLAY" "$XAUTHORITY" > "{env_file}"; '
            f'while [ ! -f "{go_file}" ]; do sleep 0.015; done; '
            'exec "$@"'
        )
        full_cmd = [
            "/usr/bin/xvfb-run",
            "-a",
            "-s",
            self.screen_spec,
            "bash",
            "-c",
            wrapper_script,
            "--",
            *ws_cmd,
        ]

        env = os.environ.copy()
        env["LC_ALL"] = "C.UTF-8"
        env["LANG"] = "C.UTF-8"
        env["XMODIFIERS"] = "@im=none"
        env["XCOMPOSEFILE"] = self.compose_path
        env["HOME"] = self.home_dir
        env["WATERSCAPE_PROFILE_DIR"] = self.profile_dir
        env["WATERSCAPE_DOWNLOADS_DIR"] = os.path.join(self.session_dir, "downloads")
        env.update(self.env_overrides)

        self._stdout_f = open(self.stdout_path, "wb")
        self._stderr_f = open(self.stderr_path, "wb")
        self.proc = subprocess.Popen(
            full_cmd,
            cwd=self.session_dir,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=self._stdout_f,
            stderr=self._stderr_f,
            start_new_session=True,
        )

        deadline = time.monotonic() + 8.0
        while not os.path.exists(env_file) and time.monotonic() < deadline:
            if self.proc.poll() is not None:
                break
            time.sleep(0.02)
        if not os.path.exists(env_file):
            self._cleanup()
            raise RuntimeError("xvfb-run failed to publish DISPLAY/XAUTHORITY")

        lines = open(env_file, "r", encoding="utf-8").read().splitlines()
        self.display_str = lines[0].strip() if lines else ""
        xauth_str = lines[1].strip() if len(lines) >= 2 else ""

        with _X11_ENV_LOCK:
            prev_xauth = os.environ.get("XAUTHORITY")
            try:
                if xauth_str:
                    os.environ["XAUTHORITY"] = xauth_str
                self.dpy = self.x11.XOpenDisplay(self.display_str.encode("ascii"))
            finally:
                if prev_xauth is None:
                    os.environ.pop("XAUTHORITY", None)
                else:
                    os.environ["XAUTHORITY"] = prev_xauth

        if not self.dpy:
            self._cleanup()
            raise RuntimeError(f"XOpenDisplay({self.display_str!r}) failed")

        self.root_win = self.x11.XDefaultRootWindow(self.dpy)

        # Pre-map Multi_key (249) and Unicode Hangul keysyms '한' (250), '글' (251) BEFORE waterscape starts!
        syms = (ctypes.c_ulong * 3)(XK_Multi_key, 0x0100D55C, 0x0100AE00)
        self.x11.XChangeKeyboardMapping(self.dpy, KC_MULTI_KEY, 1, syms, 3)
        self.x11.XSync(self.dpy, 0)

        with open(go_file, "w", encoding="utf-8") as gf:
            gf.write("go\n")

        return self

    def _cleanup(self) -> None:
        if self.dpy:
            try:
                self.x11.XCloseDisplay(self.dpy)
            except Exception:
                pass
            self.dpy = None
        if self.proc is not None:
            if self.proc.poll() is None:
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except Exception:
                    try:
                        self.proc.kill()
                    except Exception:
                        pass
                try:
                    self.proc.wait(timeout=2.0)
                except Exception:
                    pass
        for fh in (self._stdout_f, self._stderr_f):
            if fh is not None:
                try:
                    fh.close()
                except Exception:
                    pass

    def __exit__(self, exc_type: typing.Any, exc: typing.Any, tb: typing.Any) -> None:
        self._cleanup()

    def read_logs(self) -> tuple[str, str]:
        for fh in (self._stdout_f, self._stderr_f):
            if fh:
                try:
                    fh.flush()
                except Exception:
                    pass
        out_s = open(self.stdout_path, "r", encoding="utf-8", errors="replace").read() if os.path.exists(self.stdout_path) else ""
        err_s = open(self.stderr_path, "r", encoding="utf-8", errors="replace").read() if os.path.exists(self.stderr_path) else ""
        return out_s, err_s

    def _window_name(self, win: int) -> str:
        name_p = ctypes.c_char_p()
        if self.x11.XFetchName(self.dpy, win, ctypes.byref(name_p)) != 0 and name_p.value:
            val = name_p.value.decode("utf-8", errors="replace")
            self.x11.XFree(ctypes.cast(name_p, ctypes.c_void_p))
            return val
        # Also check _NET_WM_NAME (UTF8_STRING)
        atom_prop = self.x11.XInternAtom(self.dpy, b"_NET_WM_NAME", 0)
        atom_utf8 = self.x11.XInternAtom(self.dpy, b"UTF8_STRING", 0)
        act_type = ctypes.c_ulong()
        act_fmt = ctypes.c_int()
        nitems = ctypes.c_ulong()
        bytes_after = ctypes.c_ulong()
        prop_p = ctypes.c_void_p()
        rc = self.x11.XGetWindowProperty(
            self.dpy,
            win,
            atom_prop,
            0,
            256,
            0,
            atom_utf8,
            ctypes.byref(act_type),
            ctypes.byref(act_fmt),
            ctypes.byref(nitems),
            ctypes.byref(bytes_after),
            ctypes.byref(prop_p),
        )
        if rc == 0 and prop_p.value and nitems.value > 0:
            raw = ctypes.string_at(prop_p.value, nitems.value)
            self.x11.XFree(prop_p)
            return raw.decode("utf-8", errors="replace")
        if prop_p.value:
            self.x11.XFree(prop_p)
        return ""

    def list_viewable_windows(self) -> list[dict[str, typing.Any]]:
        root_r = ctypes.c_ulong()
        parent_r = ctypes.c_ulong()
        children_p = ctypes.POINTER(ctypes.c_ulong)()
        nchildren = ctypes.c_uint()
        out: list[dict[str, typing.Any]] = []
        if (
            self.x11.XQueryTree(
                self.dpy,
                self.root_win,
                ctypes.byref(root_r),
                ctypes.byref(parent_r),
                ctypes.byref(children_p),
                ctypes.byref(nchildren),
            )
            == 0
        ):
            return out
        try:
            for i in range(nchildren.value):
                wid = children_p[i]
                attr = _XWindowAttributes()
                if self.x11.XGetWindowAttributes(self.dpy, wid, ctypes.byref(attr)) != 0:
                    if attr.map_state == 2 and attr.width >= 80 and attr.height >= 80:
                        out.append(
                            {
                                "id": wid,
                                "name": self._window_name(wid),
                                "x": attr.x,
                                "y": attr.y,
                                "width": attr.width,
                                "height": attr.height,
                            }
                        )
        finally:
            if children_p:
                self.x11.XFree(ctypes.cast(children_p, ctypes.c_void_p))
        return out

    def wait_for_waterscape_window(self, timeout_sec: float = 8.0) -> int:
        deadline = time.monotonic() + timeout_sec
        while time.monotonic() < deadline:
            if self.proc is not None and self.proc.poll() is not None:
                out_s, err_s = self.read_logs()
                raise RuntimeError(f"waterscape exited early rc={self.proc.returncode} stderr={err_s[:200]}")
            for w in self.list_viewable_windows():
                if "Waterscape" in w["name"] or w["width"] >= 400:
                    self.focus_window(w["id"])
                    return w["id"]
            time.sleep(0.04)
        raise RuntimeError("Timed out waiting for viewable Waterscape X11 window")

    def focus_window(self, win: int) -> None:
        self.x11.XSetInputFocus(self.dpy, win, 1, 0)
        self.x11.XSync(self.dpy, 0)

    def send_focus_cycle(self, win: int) -> None:
        self.x11.XSetInputFocus(self.dpy, self.root_win, 1, 0)
        self.x11.XSync(self.dpy, 0)
        time.sleep(0.03)
        self.x11.XSetInputFocus(self.dpy, win, 1, 0)
        self.x11.XSync(self.dpy, 0)
        time.sleep(0.03)

    def get_window_geometry(self, win: int) -> tuple[int, int, int, int]:
        attr = _XWindowAttributes()
        self.x11.XGetWindowAttributes(self.dpy, win, ctypes.byref(attr))
        return (attr.x, attr.y, attr.width, attr.height)

    def get_net_wm_state_names(self, win: int) -> list[str]:
        atom_prop = self.x11.XInternAtom(self.dpy, b"_NET_WM_STATE", 0)
        atom_type = self.x11.XInternAtom(self.dpy, b"ATOM", 0)
        act_type = ctypes.c_ulong()
        act_fmt = ctypes.c_int()
        nitems = ctypes.c_ulong()
        bytes_after = ctypes.c_ulong()
        prop_p = ctypes.c_void_p()
        rc = self.x11.XGetWindowProperty(
            self.dpy,
            win,
            atom_prop,
            0,
            32,
            0,
            atom_type,
            ctypes.byref(act_type),
            ctypes.byref(act_fmt),
            ctypes.byref(nitems),
            ctypes.byref(bytes_after),
            ctypes.byref(prop_p),
        )
        names: list[str] = []
        if rc == 0 and prop_p.value and nitems.value > 0:
            atoms = ctypes.cast(prop_p.value, ctypes.POINTER(ctypes.c_ulong))
            for i in range(nitems.value):
                np = self.x11.XGetAtomName(self.dpy, atoms[i])
                if np:
                    names.append(ctypes.string_at(np).decode("ascii", errors="replace"))
                    self.x11.XFree(np)
        if prop_p.value:
            self.x11.XFree(prop_p)
        return names

    def resize_window(self, win: int, width: int, height: int) -> None:
        self.x11.XResizeWindow(self.dpy, win, width, height)
        self.x11.XSync(self.dpy, 0)

    def read_window_pixel(self, win: int, x: int, y: int) -> typing.Optional[tuple[int, int, int]]:
        img = self.x11.XGetImage(self.dpy, win, x, y, 1, 1, 0xFFFFFFFF, 2)
        if not img:
            return None
        try:
            px = self.x11.XGetPixel(img, 0, 0)
            return ((px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF)
        finally:
            self.x11.XDestroyImage(img)

    def grab_window_region(self, win: int, x: int, y: int, w: int, h: int) -> typing.Optional[PNGImage]:
        img = self.x11.XGetImage(self.dpy, win, x, y, w, h, 0xFFFFFFFF, 2)
        if not img:
            return None
        try:
            rgba = bytearray(w * h * 4)
            di = 0
            for ry in range(h):
                for rx in range(w):
                    px = self.x11.XGetPixel(img, rx, ry)
                    rgba[di] = (px >> 16) & 0xFF
                    rgba[di + 1] = (px >> 8) & 0xFF
                    rgba[di + 2] = px & 0xFF
                    rgba[di + 3] = 255
                    di += 4
            return PNGImage(w, h, rgba)
        finally:
            self.x11.XDestroyImage(img)

    def wait_for_pixel_near(
        self,
        win: int,
        x: int,
        y: int,
        target_rgb: tuple[int, int, int],
        *,
        tol: int = 28,
        timeout_sec: float = 5.0,
    ) -> tuple[bool, tuple[int, int, int], float]:
        t0 = time.perf_counter()
        last = (0, 0, 0)
        while time.perf_counter() - t0 < timeout_sec:
            px = self.read_window_pixel(win, x, y)
            if px is not None:
                last = px
                if (
                    abs(px[0] - target_rgb[0]) <= tol
                    and abs(px[1] - target_rgb[1]) <= tol
                    and abs(px[2] - target_rgb[2]) <= tol
                ):
                    return True, px, (time.perf_counter() - t0) * 1000.0
            time.sleep(0.015)
        return False, last, (time.perf_counter() - t0) * 1000.0

    def click_window(self, win: int, cx: int, cy: int, button: int = 1) -> None:
        rx = ctypes.c_int()
        ry = ctypes.c_int()
        child = ctypes.c_ulong()
        self.x11.XTranslateCoordinates(
            self.dpy, win, self.root_win, cx, cy, ctypes.byref(rx), ctypes.byref(ry), ctypes.byref(child)
        )
        self.xtst.XTestFakeMotionEvent(self.dpy, -1, rx.value, ry.value, 0)
        self.xtst.XTestFakeButtonEvent(self.dpy, button, 1, 0)
        self.xtst.XTestFakeButtonEvent(self.dpy, button, 0, 0)
        self.x11.XSync(self.dpy, 0)

    def scroll_wheel(self, win: int, cx: int, cy: int, *, down: bool = True, clicks: int = 4) -> None:
        rx = ctypes.c_int()
        ry = ctypes.c_int()
        child = ctypes.c_ulong()
        self.x11.XTranslateCoordinates(
            self.dpy, win, self.root_win, cx, cy, ctypes.byref(rx), ctypes.byref(ry), ctypes.byref(child)
        )
        self.xtst.XTestFakeMotionEvent(self.dpy, -1, rx.value, ry.value, 0)
        btn = 5 if down else 4
        for _ in range(clicks):
            self.xtst.XTestFakeButtonEvent(self.dpy, btn, 1, 0)
            self.xtst.XTestFakeButtonEvent(self.dpy, btn, 0, 0)
            self.x11.XSync(self.dpy, 0)
            time.sleep(0.015)

    def press_keycode(self, kc: int, *, ctrl: bool = False, shift: bool = False, alt: bool = False) -> None:
        ctrl_kc = self.x11.XKeysymToKeycode(self.dpy, XK_Control_L) if ctrl else 0
        shift_kc = self.x11.XKeysymToKeycode(self.dpy, XK_Shift_L) if shift else 0
        alt_kc = self.x11.XKeysymToKeycode(self.dpy, XK_Alt_L) if alt else 0
        if ctrl_kc:
            self.xtst.XTestFakeKeyEvent(self.dpy, ctrl_kc, 1, 0)
        if shift_kc:
            self.xtst.XTestFakeKeyEvent(self.dpy, shift_kc, 1, 0)
        if alt_kc:
            self.xtst.XTestFakeKeyEvent(self.dpy, alt_kc, 1, 0)
        self.xtst.XTestFakeKeyEvent(self.dpy, kc, 1, 0)
        self.xtst.XTestFakeKeyEvent(self.dpy, kc, 0, 0)
        if alt_kc:
            self.xtst.XTestFakeKeyEvent(self.dpy, alt_kc, 0, 0)
        if shift_kc:
            self.xtst.XTestFakeKeyEvent(self.dpy, shift_kc, 0, 0)
        if ctrl_kc:
            self.xtst.XTestFakeKeyEvent(self.dpy, ctrl_kc, 0, 0)
        self.x11.XSync(self.dpy, 0)

    def press_keysym(self, ks: int, *, ctrl: bool = False, shift: bool = False, alt: bool = False) -> None:
        kc = self.x11.XKeysymToKeycode(self.dpy, ks)
        if kc == 0:
            raise RuntimeError(f"Unmapped keysym 0x{ks:x}")
        self.press_keycode(kc, ctrl=ctrl, shift=shift, alt=alt)

    def type_ascii(self, text: str) -> None:
        for ch in text:
            if ch.isalpha():
                ks = self.x11.XStringToKeysym(ch.lower().encode("ascii"))
                self.press_keysym(ks, shift=ch.isupper())
            elif ch.isdigit():
                ks = self.x11.XStringToKeysym(ch.encode("ascii"))
                self.press_keysym(ks, shift=False)
            elif ch in _UNSHIFTED_PUNCT:
                ks = self.x11.XStringToKeysym(_UNSHIFTED_PUNCT[ch].encode("ascii"))
                self.press_keysym(ks, shift=False)
            elif ch in _SHIFTED_ASCII and _SHIFTED_ASCII[ch]:
                ks = self.x11.XStringToKeysym(_SHIFTED_ASCII[ch].encode("ascii"))
                self.press_keysym(ks, shift=True)
            else:
                ks = ord(ch)
                self.press_keysym(ks, shift=False)
            time.sleep(0.004)

    def compose_multi_key(self, letters: str) -> None:
        self.press_keycode(KC_MULTI_KEY)
        time.sleep(0.012)
        for ch in letters:
            ks = self.x11.XStringToKeysym(ch.encode("ascii"))
            self.press_keysym(ks)
            time.sleep(0.012)

    def measure_ctrl_tab_switch_ms(
        self,
        win: int,
        sample_xy: tuple[int, int],
        target_rgb: tuple[int, int, int],
        *,
        tol: int = 28,
        max_wait_ms: float = 1500.0,
    ) -> tuple[bool, float, tuple[int, int, int]]:
        sx, sy = sample_xy
        t0 = time.perf_counter()
        self.press_keysym(XK_Tab, ctrl=True)
        last = (0, 0, 0)
        while (time.perf_counter() - t0) * 1000.0 < max_wait_ms:
            px = self.read_window_pixel(win, sx, sy)
            if px is not None:
                last = px
                if (
                    abs(px[0] - target_rgb[0]) <= tol
                    and abs(px[1] - target_rgb[1]) <= tol
                    and abs(px[2] - target_rgb[2]) <= tol
                ):
                    return True, (time.perf_counter() - t0) * 1000.0, px
            time.sleep(0.0005)
        return False, (time.perf_counter() - t0) * 1000.0, last

    def close_gracefully(self, win: int, timeout_sec: float = 5.0) -> int:
        wm_protocols = self.x11.XInternAtom(self.dpy, b"WM_PROTOCOLS", 0)
        wm_delete = self.x11.XInternAtom(self.dpy, b"WM_DELETE_WINDOW", 0)
        ev = _XClientMessageEvent()
        ev.type = 33  # ClientMessage
        ev.window = win
        ev.message_type = wm_protocols
        ev.format = 32
        ev.data.l[0] = wm_delete
        ev.data.l[1] = 0
        self.x11.XSendEvent(self.dpy, win, 0, 0, ctypes.byref(ev))
        self.x11.XFlush(self.dpy)
        if self.dpy:
            self.x11.XCloseDisplay(self.dpy)
            self.dpy = None
        if self.proc is None:
            return -1
        try:
            return self.proc.wait(timeout=timeout_sec)
        except subprocess.TimeoutExpired:
            return -9
