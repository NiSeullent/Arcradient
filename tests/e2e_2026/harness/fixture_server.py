import sys
sys.dont_write_bytecode = True

import base64
import gzip
import hashlib
import http.client
import http.server
from http import HTTPStatus
import json
import os
import shutil
import socket
import socketserver
import ssl
import struct
import subprocess
import threading
import time
import typing
import urllib.parse
import zlib

from .types_and_guards import write_png_rgba

# Raise Python stdlib HTTP client/server header & request-line ceilings to 256 KB / 2048 headers
http.client._MAXLINE = 262144
http.client._MAXHEADERS = 2048

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def _uint_base128(v: int) -> bytes:
    if v == 0:
        return b"\x00"
    parts: list[int] = []
    while v > 0:
        parts.append(v & 0x7F)
        v >>= 7
    parts.reverse()
    return bytes((c | (0x80 if i < len(parts) - 1 else 0)) for i, c in enumerate(parts))


def build_genuine_woff2(sfnt_path: str) -> bytes:
    """
    Constructs a spec-valid W3C WOFF 2.0 (0x774F4632 'wOF2') font binary using
    WOFF2 §4.2 transformVersion=3 (null transform) on glyf/loca and /usr/bin/brotli.
    Verified natively against FreeType 2 (FT_New_Memory_Face == 0).
    """
    with open(sfnt_path, "rb") as f:
        sfnt = f.read()
    flavor, num_tables = struct.unpack_from(">IH", sfnt, 0)
    recs = sorted(
        [struct.unpack_from(">4sIII", sfnt, 12 + i * 16) for i in range(num_tables)],
        key=lambda r: r[0],
    )
    tdir = bytearray()
    raw = bytearray()
    for tag, _csum, off, length in recs:
        flags = ((3 << 6) if tag in (b"glyf", b"loca") else 0) | 63
        tdir.append(flags)
        tdir.extend(tag)
        tdir.extend(_uint_base128(length))
        raw.extend(sfnt[off : off + length])
    comp = subprocess.run(
        ["/usr/bin/brotli", "-q", "6", "-c"],
        input=bytes(raw),
        capture_output=True,
        check=True,
    ).stdout
    sfnt_sz = 12 + 16 * num_tables + sum((l + 3) & ~3 for _, _, _, l in recs)
    unpadded = 48 + len(tdir) + len(comp)
    total_len = (unpadded + 3) & ~3
    hdr = struct.pack(
        ">4sIIHHIIHHIIIII",
        b"wOF2",
        flavor,
        total_len,
        num_tables,
        0,
        sfnt_sz,
        len(comp),
        1,
        0,
        0,
        0,
        0,
        0,
        0,
    )
    return hdr + bytes(tdir) + comp + (b"\x00" * (total_len - unpadded))


def build_genuine_woff1(sfnt_path: str) -> bytes:
    """
    Constructs a spec-valid W3C WOFF 1.0 (0x774F4646 'wOFF') font binary with zlib-compressed SFNT tables.
    """
    with open(sfnt_path, "rb") as f:
        sfnt = f.read()
    flavor, num_tables = struct.unpack_from(">IH", sfnt, 0)
    recs = sorted(
        [struct.unpack_from(">4sIII", sfnt, 12 + i * 16) for i in range(num_tables)],
        key=lambda r: r[0],
    )
    sfnt_sz = 12 + 16 * num_tables + sum((l + 3) & ~3 for _, _, _, l in recs)
    dir_offset = 44
    data_offset = dir_offset + 20 * num_tables
    tdir = bytearray()
    tdata = bytearray()
    cur_off = data_offset
    for tag, csum, off, length in recs:
        orig = sfnt[off : off + length]
        z = zlib.compress(orig, 6)
        if len(z) >= len(orig):
            stored = orig
        else:
            stored = z
        comp_len = len(stored)
        tdir.extend(struct.pack(">4sIIII", tag, cur_off, comp_len, length, csum))
        tdata.extend(stored)
        pad = ((comp_len + 3) & ~3) - comp_len
        if pad:
            tdata.extend(b"\x00" * pad)
        cur_off += comp_len + pad
    total_len = cur_off
    hdr = struct.pack(
        ">4sIIHHIHHIII",
        b"wOFF",
        flavor,
        total_len,
        num_tables,
        0,
        sfnt_sz,
        1,
        0,
        0,
        0,
        0,
    )
    return hdr + bytes(tdir) + bytes(tdata)


def compress_brotli_real(raw: bytes) -> bytes:
    return subprocess.run(
        ["/usr/bin/brotli", "-q", "6", "-c"],
        input=raw,
        capture_output=True,
        check=True,
    ).stdout


def generate_tls_certs(cert_dir: str) -> tuple[str, str, str]:
    ca_key = os.path.join(cert_dir, "ca.key")
    ca_crt = os.path.join(cert_dir, "ca.crt")
    srv_key = os.path.join(cert_dir, "server.key")
    srv_csr = os.path.join(cert_dir, "server.csr")
    srv_crt = os.path.join(cert_dir, "server.crt")
    ext_cfg = os.path.join(cert_dir, "ext.cnf")

    with open(ext_cfg, "w", encoding="utf-8") as f:
        f.write("subjectAltName=IP:127.0.0.1,DNS:localhost\nbasicConstraints=CA:FALSE\n")

    subprocess.run(
        [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-keyout",
            ca_key,
            "-out",
            ca_crt,
            "-days",
            "365",
            "-subj",
            "/CN=Arcradient E2E 2026 Test Root CA",
            "-addext",
            "basicConstraints=critical,CA:TRUE",
            "-addext",
            "keyUsage=critical,keyCertSign,cRLSign",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.run(
        [
            "openssl",
            "req",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-keyout",
            srv_key,
            "-out",
            srv_csr,
            "-subj",
            "/CN=127.0.0.1",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.run(
        [
            "openssl",
            "x509",
            "-req",
            "-in",
            srv_csr,
            "-CA",
            ca_crt,
            "-CAkey",
            ca_key,
            "-CAcreateserial",
            "-out",
            srv_crt,
            "-days",
            "365",
            "-extfile",
            ext_cfg,
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return ca_crt, srv_crt, srv_key


def generate_media_and_font_assets(asset_dir: str) -> dict[str, bytes]:
    os.makedirs(asset_dir, exist_ok=True)
    assets: dict[str, bytes] = {}

    # 1. Genuine Monospace TTF / WOFF1 / WOFF2 & Bold TTF / WOFF2
    mono_ttf_src = "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"
    mono_bold_src = "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf"
    if not os.path.exists(mono_ttf_src):
        mono_ttf_src = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
    if not os.path.exists(mono_bold_src):
        mono_bold_src = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"

    with open(mono_ttf_src, "rb") as f:
        assets["mono.ttf"] = f.read()
    assets["mono.woff"] = build_genuine_woff1(mono_ttf_src)
    assets["mono.woff2"] = build_genuine_woff2(mono_ttf_src)
    with open(mono_bold_src, "rb") as f:
        assets["mono_bold.ttf"] = f.read()
    assets["mono_bold.woff2"] = build_genuine_woff2(mono_bold_src)
    assets["corrupt.woff2"] = b"wOF2" + (b"\xff\x00\xaa\x55" * 24)

    # 2. Solid 64x64 green (#22c55e -> 34, 197, 94) PNG, WebP, AVIF & 64x64 red (#ef4444) PNG
    green_rgba = bytes([34, 197, 94, 255]) * (64 * 64)
    green_png_path = os.path.join(asset_dir, "green64.png")
    write_png_rgba(green_png_path, 64, 64, green_rgba)
    with open(green_png_path, "rb") as f:
        assets["green64.png"] = f.read()

    red_rgba = bytes([239, 68, 68, 255]) * (64 * 64)
    red_png_path = os.path.join(asset_dir, "red64.png")
    write_png_rgba(red_png_path, 64, 64, red_rgba)
    with open(red_png_path, "rb") as f:
        assets["red64.png"] = f.read()

    webp_path = os.path.join(asset_dir, "green64.webp")
    subprocess.run(
        [
            "/usr/bin/ffmpeg",
            "-y",
            "-v",
            "error",
            "-i",
            green_png_path,
            "-c:v",
            "libwebp",
            "-lossless",
            "1",
            webp_path,
        ],
        check=True,
    )
    with open(webp_path, "rb") as f:
        assets["green64.webp"] = f.read()

    avif_path = os.path.join(asset_dir, "green64.avif")
    subprocess.run(
        [
            "/usr/bin/ffmpeg",
            "-y",
            "-v",
            "error",
            "-f",
            "lavfi",
            "-i",
            "color=c=0x22c55e:s=64x64:d=0.1",
            "-frames:v",
            "1",
            "-c:v",
            "libaom-av1",
            "-still-picture",
            "1",
            "-pix_fmt",
            "yuv420p",
            avif_path,
        ],
        check=True,
    )
    with open(avif_path, "rb") as f:
        assets["green64.avif"] = f.read()

    # 3. Progressive MP4 (160x90, 0.8s blue -> orange) & Fragmented MP4 (init + m4s) & WAV
    mp4_path = os.path.join(asset_dir, "sample.mp4")
    subprocess.run(
        [
            "/usr/bin/ffmpeg",
            "-y",
            "-v",
            "error",
            "-f",
            "lavfi",
            "-i",
            "color=c=0x0080ff:s=160x90:r=10:d=0.6",
            "-c:v",
            "libx264",
            "-profile:v",
            "baseline",
            "-pix_fmt",
            "yuv420p",
            "-movflags",
            "+faststart",
            mp4_path,
        ],
        check=True,
    )
    with open(mp4_path, "rb") as f:
        assets["sample.mp4"] = f.read()

    fmp4_path = os.path.join(asset_dir, "sample_fmp4.mp4")
    subprocess.run(
        [
            "/usr/bin/ffmpeg",
            "-y",
            "-v",
            "error",
            "-f",
            "lavfi",
            "-i",
            "color=c=0x22c55e:s=128x72:r=10:d=0.5",
            "-c:v",
            "libx264",
            "-pix_fmt",
            "yuv420p",
            "-movflags",
            "frag_keyframe+empty_moov+default_base_moof",
            "-f",
            "mp4",
            fmp4_path,
        ],
        check=True,
    )
    with open(fmp4_path, "rb") as f:
        fmp4_bytes = f.read()
    assets["sample_fmp4.mp4"] = fmp4_bytes
    # Split fMP4 at first 'moof' box
    bpos = 0
    moof_off = len(fmp4_bytes)
    while bpos + 8 <= len(fmp4_bytes):
        bsz, btype = struct.unpack_from(">I4s", fmp4_bytes, bpos)
        if btype == b"moof":
            moof_off = bpos
            break
        if bsz < 8:
            break
        bpos += bsz
    assets["init.mp4"] = fmp4_bytes[:moof_off]
    assets["seg1.m4s"] = fmp4_bytes[moof_off:]

    wav_path = os.path.join(asset_dir, "tone.wav")
    subprocess.run(
        [
            "/usr/bin/ffmpeg",
            "-y",
            "-v",
            "error",
            "-f",
            "lavfi",
            "-i",
            "sine=frequency=440:sample_rate=22050:duration=0.25",
            "-c:a",
            "pcm_s16le",
            wav_path,
        ],
        check=True,
    )
    with open(wav_path, "rb") as f:
        assets["tone.wav"] = f.read()

    return assets


class _ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class _ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


def _ws_accept_key(sec_key: str) -> str:
    digest = hashlib.sha1((sec_key.strip() + WS_GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


def _ws_send_frame(sock: socket.socket, opcode: int, payload: bytes, fin: bool = True) -> None:
    b0 = (0x80 if fin else 0x00) | (opcode & 0x0F)
    n = len(payload)
    hdr = bytearray([b0])
    if n < 126:
        hdr.append(n)
    elif n <= 65535:
        hdr.append(126)
        hdr.extend(struct.pack(">H", n))
    else:
        hdr.append(127)
        hdr.extend(struct.pack(">Q", n))
    sock.sendall(bytes(hdr) + payload)


def _ws_recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("socket closed mid-frame")
        buf.extend(chunk)
    return bytes(buf)


class FixtureServer:
    """
    Multi-protocol local deterministic fixture server:
    - main_origin:  http://127.0.0.1:<port>  (supports >= 48 KB / 128 KB URLs & 1024+ cookies)
    - cors_origin:  http://127.0.0.1:<port>
    - https_origin: https://127.0.0.1:<port> (records TLS ClientHello ALPN offers)
    - ws_origin:    ws://127.0.0.1:<port>    (RFC 6455 validating Sec-WebSocket-Accept & client MASK==1)
    """

    def __init__(self, asset_dir: str):
        self.asset_dir = asset_dir
        os.makedirs(self.asset_dir, exist_ok=True)
        self.cert_dir = os.path.join(self.asset_dir, "tls")
        os.makedirs(self.cert_dir, exist_ok=True)
        self.ca_cert, self.srv_cert, self.srv_key = generate_tls_certs(self.cert_dir)
        self.assets = generate_media_and_font_assets(self.asset_dir)

        self._lock = threading.Lock()
        self._traces: list[dict[str, typing.Any]] = []
        self._ws_traces: list[dict[str, typing.Any]] = []
        self._tls_alpn_offers: list[list[str]] = []
        self._dynamic_routes: dict[str, typing.Any] = {}
        self._prefix_routes: list[tuple[str, typing.Any]] = []

        self._main_srv = _ThreadedHTTPServer(("127.0.0.1", 0), self._make_http_handler("main"))
        self._cors_srv = _ThreadedHTTPServer(("127.0.0.1", 0), self._make_http_handler("cors"))
        self._https_srv = _ThreadedHTTPServer(("127.0.0.1", 0), self._make_http_handler("https"))

        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain(certfile=self.srv_cert, keyfile=self.srv_key)

        def _alpn_cb(ssl_sock: typing.Any, protos: list[str]) -> str:
            with self._lock:
                self._tls_alpn_offers.append(list(protos))
            if "http/1.1" in protos:
                return "http/1.1"
            return protos[0] if protos else "http/1.1"

        if hasattr(ssl_ctx, "set_alpn_callback"):
            ssl_ctx.set_alpn_callback(_alpn_cb)
        else:
            ssl_ctx.set_alpn_protocols(["http/1.1"])
        self._https_srv.socket = ssl_ctx.wrap_socket(self._https_srv.socket, server_side=True)

        self._ws_srv = _ThreadedTCPServer(("127.0.0.1", 0), self._make_ws_handler())

        self.http_port = self._main_srv.server_port
        self.cors_port = self._cors_srv.server_port
        self.https_port = self._https_srv.server_port
        self.ws_port = self._ws_srv.server_address[1]

        self.main_origin = f"http://127.0.0.1:{self.http_port}"
        self.cors_origin = f"http://127.0.0.1:{self.cors_port}"
        self.https_origin = f"https://127.0.0.1:{self.https_port}"
        self.ws_origin = f"ws://127.0.0.1:{self.ws_port}"

        self._threads: list[threading.Thread] = []

    def start(self) -> None:
        for srv in (self._main_srv, self._cors_srv, self._https_srv, self._ws_srv):
            t = threading.Thread(target=srv.serve_forever, daemon=True)
            t.start()
            self._threads.append(t)

    def stop(self) -> None:
        for srv in (self._main_srv, self._cors_srv, self._https_srv, self._ws_srv):
            try:
                srv.shutdown()
            except Exception:
                pass
            try:
                srv.server_close()
            except Exception:
                pass

    def register_route(
        self,
        path: str,
        spec: typing.Union[
            typing.Callable[[typing.Any], None],
            dict[str, typing.Any],
        ],
        *,
        prefix_match: bool = False,
    ) -> str:
        """
        Register an exact path or prefix handler on main/cors/https servers.
        `spec` can be:
          - callable(req_ctx) -> dict | tuple
          - dict: {
              "status": 200,
              "content_type": "text/html; charset=utf-8",
              "body": str | bytes,
              "headers": list[tuple[str, str]] | dict[str, str],
              "delay_ms": 0,
            }
        Returns `self.main_origin + path`.
        """
        with self._lock:
            if prefix_match:
                self._prefix_routes.append((path, spec))
            else:
                self._dynamic_routes[path] = spec
        return self.main_origin + path

    def mark_cursor(self) -> int:
        with self._lock:
            return len(self._traces)

    def get_traces(
        self,
        *,
        since: int = 0,
        path_prefix: typing.Optional[str] = None,
    ) -> list[dict[str, typing.Any]]:
        with self._lock:
            items = list(self._traces[since:])
        if path_prefix is not None:
            items = [t for t in items if str(t.get("path", "")).startswith(path_prefix)]
        return items

    def get_ws_traces(self) -> list[dict[str, typing.Any]]:
        with self._lock:
            return list(self._ws_traces)

    def get_tls_alpn_offers(self) -> list[list[str]]:
        with self._lock:
            return list(self._tls_alpn_offers)

    def _record_trace(self, entry: dict[str, typing.Any]) -> None:
        with self._lock:
            self._traces.append(entry)

    def _lookup_dynamic(self, path: str) -> typing.Any:
        with self._lock:
            if path in self._dynamic_routes:
                return self._dynamic_routes[path]
            for pfx, spec in reversed(self._prefix_routes):
                if path.startswith(pfx):
                    return spec
        return None

    def _make_ws_handler(self):
        parent = self

        class WSHandler(socketserver.BaseRequestHandler):
            def handle(self) -> None:
                sock: socket.socket = self.request
                sock.settimeout(8.0)
                buf = bytearray()
                while b"\r\n\r\n" not in buf and len(buf) < 32768:
                    chunk = sock.recv(4096)
                    if not chunk:
                        return
                    buf.extend(chunk)
                head_raw = bytes(buf).split(b"\r\n\r\n", 1)[0].decode("iso-8859-1", errors="replace")
                lines = head_raw.split("\r\n")
                if not lines:
                    return
                parts = lines[0].split(" ")
                req_target = parts[1] if len(parts) >= 2 else "/"
                hdrs: dict[str, str] = {}
                for ln in lines[1:]:
                    if ":" in ln:
                        k, v = ln.split(":", 1)
                        hdrs[k.strip().lower()] = v.strip()

                path = urllib.parse.urlsplit(req_target).path
                sec_key = hdrs.get("sec-websocket-key", "")

                if path == "/ws-http-200":
                    resp = (
                        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK"
                    )
                    sock.sendall(resp.encode("ascii"))
                    return

                if path == "/ws-bad-accept":
                    resp = (
                        "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: aW52YWxpZF9hY2NlcHRfa2V5PT0=\r\n\r\n"
                    )
                    sock.sendall(resp.encode("ascii"))
                    return

                if hdrs.get("upgrade", "").lower() != "websocket" or not sec_key:
                    sock.sendall(b"HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n")
                    return

                accept_val = _ws_accept_key(sec_key)
                handshake = (
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    f"Sec-WebSocket-Accept: {accept_val}\r\n\r\n"
                )
                sock.sendall(handshake.encode("ascii"))

                ws_rec: dict[str, typing.Any] = {
                    "path": req_target,
                    "headers": hdrs,
                    "messages": [],
                    "masked_all": True,
                    "clean_close": False,
                }
                frag_opcode: typing.Optional[int] = None
                frag_buf = bytearray()

                try:
                    while True:
                        b0, b1 = _ws_recv_exact(sock, 2)
                        fin = bool(b0 & 0x80)
                        opcode = b0 & 0x0F
                        masked = bool(b1 & 0x80)
                        plen = b1 & 0x7F
                        if plen == 126:
                            (plen,) = struct.unpack(">H", _ws_recv_exact(sock, 2))
                        elif plen == 127:
                            (plen,) = struct.unpack(">Q", _ws_recv_exact(sock, 8))
                        if not masked:
                            ws_rec["masked_all"] = False
                            _ws_send_frame(sock, 0x8, struct.pack(">H", 1002) + b"unmasked client frame")
                            break
                        mask_key = _ws_recv_exact(sock, 4)
                        enc_payload = _ws_recv_exact(sock, plen) if plen > 0 else b""
                        payload = bytes(enc_payload[i] ^ mask_key[i & 3] for i in range(plen))

                        if opcode == 0x8:  # Close
                            code = struct.unpack(">H", payload[:2])[0] if len(payload) >= 2 else 1000
                            ws_rec["clean_close"] = True
                            ws_rec["close_code"] = code
                            _ws_send_frame(sock, 0x8, struct.pack(">H", code))
                            break
                        elif opcode == 0x9:  # Ping
                            _ws_send_frame(sock, 0xA, payload)
                        elif opcode == 0xA:  # Pong
                            continue
                        elif opcode in (0x1, 0x2):
                            if fin:
                                ws_rec["messages"].append(
                                    {"opcode": opcode, "len": len(payload), "text": payload.decode("utf-8", "replace") if opcode == 1 else None}
                                )
                                _ws_send_frame(sock, opcode, payload)
                            else:
                                frag_opcode = opcode
                                frag_buf = bytearray(payload)
                        elif opcode == 0x0 and frag_opcode is not None:
                            frag_buf.extend(payload)
                            if fin:
                                full = bytes(frag_buf)
                                ws_rec["messages"].append(
                                    {"opcode": frag_opcode, "len": len(full), "text": full.decode("utf-8", "replace") if frag_opcode == 1 else None}
                                )
                                _ws_send_frame(sock, frag_opcode, full)
                                frag_opcode = None
                                frag_buf.clear()
                except (EOFError, TimeoutError, OSError):
                    pass
                finally:
                    with parent._lock:
                        parent._ws_traces.append(ws_rec)

        return WSHandler

    def _make_http_handler(self, listener_kind: str):
        parent = self

        class LongURLHTTPHandler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            max_request_line = 262144  # 256 KB ceiling (>= 32 KB, >= 48 KB, >= 128 KB)

            def log_message(self, fmt: str, *args: typing.Any) -> None:
                return

            def handle_one_request(self) -> None:
                try:
                    self.raw_requestline = self.rfile.readline(self.max_request_line + 1)
                    if len(self.raw_requestline) > self.max_request_line:
                        self.requestline = ""
                        self.request_version = ""
                        self.command = ""
                        self.send_error(HTTPStatus.REQUEST_URI_TOO_LONG)
                        return
                    if not self.raw_requestline:
                        self.close_connection = True
                        return
                    if not self.parse_request():
                        return
                    mname = "do_" + self.command
                    if not hasattr(self, mname):
                        self.send_error(HTTPStatus.NOT_IMPLEMENTED, f"Unsupported method ({self.command!r})")
                        return
                    getattr(self, mname)()
                    self.wfile.flush()
                except (TimeoutError, ConnectionResetError, BrokenPipeError):
                    self.close_connection = True

            def _send(
                self,
                status: int,
                content_type: str,
                body: bytes,
                headers: typing.Optional[typing.Union[dict[str, str], list[tuple[str, str]]]] = None,
            ) -> None:
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                if headers:
                    it = headers.items() if isinstance(headers, dict) else headers
                    for k, v in it:
                        self.send_header(k, v)
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(body)

            def _record_and_read_body(self) -> tuple[urllib.parse.SplitResult, bytes, dict[str, str]]:
                clen = int(self.headers.get("Content-Length", "0") or "0")
                raw_body = self.rfile.read(clen) if clen > 0 else b""
                hdr_map = {k.lower(): v for k, v in self.headers.items()}
                parsed = urllib.parse.urlsplit(self.path)
                parent._record_trace(
                    {
                        "listener": listener_kind,
                        "method": self.command,
                        "raw_target": self.path,
                        "raw_target_len": len(self.path),
                        "has_fragment_on_wire": ("#" in self.path),
                        "path": parsed.path,
                        "query": parsed.query,
                        "headers": hdr_map,
                        "cookie": self.headers.get("Cookie", ""),
                        "accept": self.headers.get("Accept", ""),
                        "origin": self.headers.get("Origin", ""),
                        "host": self.headers.get("Host", ""),
                        "body_utf8": raw_body.decode("utf-8", errors="replace") if raw_body else "",
                        "ts": time.monotonic(),
                    }
                )
                return parsed, raw_body, hdr_map

            def do_OPTIONS(self) -> None:
                parsed, _, hdrs = self._record_and_read_body()
                origin = hdrs.get("origin", "*")
                if parsed.path.startswith("/cors-ok") or parsed.path.startswith("/cors-cred"):
                    resp_hdrs = {
                        "Access-Control-Allow-Origin": origin if "cred" in parsed.path else "*",
                        "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
                        "Access-Control-Allow-Headers": "Content-Type, X-Custom-Req",
                        "Access-Control-Expose-Headers": "X-Custom-Resp",
                    }
                    if "cred" in parsed.path:
                        resp_hdrs["Access-Control-Allow-Credentials"] = "true"
                    self._send(204, "text/plain", b"", resp_hdrs)
                else:
                    self._send(204, "text/plain", b"")

            def do_HEAD(self) -> None:
                self.do_GET()

            def do_POST(self) -> None:
                self._dispatch()

            def do_GET(self) -> None:
                self._dispatch()

            def _dispatch(self) -> None:
                parsed, raw_body, hdrs = self._record_and_read_body()
                path = parsed.path
                qs = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)

                # 1. Dynamic registered routes first
                dyn = parent._lookup_dynamic(path)
                if dyn is not None:
                    if callable(dyn):
                        res = dyn(
                            {
                                "method": self.command,
                                "raw_target": self.path,
                                "path": path,
                                "query": parsed.query,
                                "qs": qs,
                                "headers": hdrs,
                                "body": raw_body,
                                "server": parent,
                            }
                        )
                    else:
                        res = dyn
                    delay_ms = int(res.get("delay_ms", 0))
                    if delay_ms > 0:
                        time.sleep(delay_ms / 1000.0)
                    body = res.get("body", b"")
                    if isinstance(body, str):
                        body = body.encode(res.get("encoding", "utf-8"))
                    self._send(
                        int(res.get("status", 200)),
                        res.get("content_type", "text/html; charset=utf-8"),
                        body,
                        res.get("headers"),
                    )
                    return

                # 2. Health & Ping
                if path in ("/healthz", "/ping"):
                    self._send(200, "application/json", json.dumps({"ok": True, "len": len(self.path)}).encode())
                    return

                # 3. Long URL verification endpoints (>= 32 KB / 48 KB / 128 KB)
                if path == "/long-echo":
                    full_url = f"{parent.main_origin}{self.path}"
                    q_sha = hashlib.sha256(parsed.query.encode("utf-8")).hexdigest()
                    payload = {
                        "raw_target_len": len(self.path),
                        "full_url_len": len(full_url),
                        "query_len": len(parsed.query),
                        "query_sha256": q_sha,
                        "has_hash": ("#" in self.path),
                    }
                    self._send(200, "application/json; charset=utf-8", json.dumps(payload).encode("utf-8"))
                    return

                if path == "/long-nav":
                    full_url = f"{parent.main_origin}{self.path}"
                    q_sha = hashlib.sha256(parsed.query.encode("utf-8")).hexdigest()
                    html = f"""<!DOCTYPE html><html><head><meta charset="utf-8">
<title>INIT</title></head><body>
<div id="out" style="width:200px;height:80px;background:#16a34a;color:#fff">LONG_NAV</div>
<script>
const hrefLen = location.href.length;
document.title = `LONG_NAV:${{hrefLen}}:{len(full_url)}:{q_sha[:16]}`;
</script></body></html>"""
                    self._send(200, "text/html; charset=utf-8", html.encode("utf-8"))
                    return

                if path == "/async/hpba":
                    full_url = f"{parent.main_origin}{self.path}"
                    q_sha = hashlib.sha256(parsed.query.encode("utf-8")).hexdigest()
                    self._send(
                        200,
                        "application/json; charset=utf-8",
                        json.dumps({"full_len": len(full_url), "query_len": len(parsed.query), "sha256": q_sha}).encode(),
                    )
                    return

                # 4. Cookie echo & bulk cookie endpoints
                if path == "/cookie/echo":
                    ck = self.headers.get("Cookie", "")
                    pairs = [p.strip() for p in ck.split(";") if p.strip()]
                    self._send(
                        200,
                        "application/json; charset=utf-8",
                        json.dumps({"cookie": ck, "count": len(pairs)}).encode("utf-8"),
                    )
                    return

                # 5. Server-Sent Events (/sse-stream)
                if path == "/sse-stream":
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream; charset=utf-8")
                    self.send_header("Cache-Control", "no-cache")
                    self.send_header("Connection", "close")
                    self.end_headers()
                    chunks = [
                        b"event: tick\ndata: {\"n\":1}\nid: 9\n\n",
                        b"data: done\n\n",
                    ]
                    for ch in chunks:
                        self.wfile.write(ch)
                        self.wfile.flush()
                        time.sleep(0.02)
                    return

                # 6. Compression endpoints (/compress/gzip, /compress/deflate, /compress/br)
                if path.startswith("/compress/"):
                    alg = path.split("/")[-1]
                    raw_html = (
                        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                        f"<title>COMP_{alg.upper()}_OK</title></head>"
                        "<body><div style='width:180px;height:60px;background:#2563eb;color:#fff'>"
                        + ("Arcradient2026-" * 400)
                        + "</div></body></html>"
                    ).encode("utf-8")
                    if alg == "gzip":
                        comp = gzip.compress(raw_html, 6)
                    elif alg == "deflate":
                        comp = zlib.compress(raw_html, 6)
                    elif alg == "br":
                        comp = compress_brotli_real(raw_html)
                    else:
                        self._send(404, "text/plain", b"unknown compression")
                        return
                    self._send(200, "text/html; charset=utf-8", comp, {"Content-Encoding": alg})
                    return

                # 7. Legacy & CJK Charsets (/charset/<enc> and /charset-bytes/<enc>)
                charset_samples = {
                    "euc-kr": ("euc_kr", "EUC-KR", "안녕하세요 한글 2026"),
                    "shift_jis": ("shift_jis", "Shift_JIS", "こんにちは世界 2026"),
                    "euc-jp": ("euc_jp", "EUC-JP", "東京タワー 2026"),
                    "gb18030": ("gb18030", "GB18030", "你好世界 2026"),
                    "big5": ("big5", "Big5", "繁體中文測試 2026"),
                    "windows-1252": ("cp1252", "windows-1252", "Euro € “Quote” 2026"),
                }
                if path.startswith("/charset/"):
                    key = path.split("/")[-1].lower()
                    if key in charset_samples:
                        py_codec, label, phrase = charset_samples[key]
                        via_meta = qs.get("via", ["header"])[0] == "meta"
                        override_hdr = qs.get("hdr_charset", [""])[0]
                        meta_tag = f'<meta charset="{label}">' if via_meta else ""
                        html_txt = (
                            f"<!DOCTYPE html><html><head>{meta_tag}<title>{phrase}</title></head>"
                            f"<body><div id='txt' style='background:#0f766e;color:#fff;padding:12px'>{phrase}</div></body></html>"
                        )
                        raw_b = html_txt.encode(py_codec)
                        if override_hdr:
                            ct = f"text/html; charset={override_hdr}"
                        elif via_meta:
                            ct = "text/html"
                        else:
                            ct = f"text/html; charset={label}"
                        self._send(200, ct, raw_b)
                        return
                if path.startswith("/charset-bytes/"):
                    key = path.split("/")[-1].lower()
                    if key in charset_samples:
                        py_codec, label, phrase = charset_samples[key]
                        raw_b = phrase.encode(py_codec)
                        self._send(
                            200,
                            "application/json; charset=utf-8",
                            json.dumps({"label": label, "expected": phrase, "bytes": list(raw_b)}).encode("utf-8"),
                        )
                        return

                # 8. Fonts & Media & Negotiated Image Assets
                font_map = {
                    "/fonts/mono.ttf": ("font/ttf", "mono.ttf"),
                    "/fonts/mono.woff": ("font/woff", "mono.woff"),
                    "/fonts/mono.woff2": ("font/woff2", "mono.woff2"),
                    "/fonts/mono_bold.ttf": ("font/ttf", "mono_bold.ttf"),
                    "/fonts/mono_bold.woff2": ("font/woff2", "mono_bold.woff2"),
                    "/fonts/corrupt.woff2": ("font/woff2", "corrupt.woff2"),
                }
                if path in font_map:
                    delay = int(qs.get("ms", ["0"])[0])
                    if delay > 0:
                        time.sleep(delay / 1000.0)
                    ct, akey = font_map[path]
                    self._send(200, ct, parent.assets[akey], {"Access-Control-Allow-Origin": "*"})
                    return

                media_map = {
                    "/img/green64.png": ("image/png", "green64.png"),
                    "/img/red64.png": ("image/png", "red64.png"),
                    "/img/green64.webp": ("image/webp", "green64.webp"),
                    "/img/green64.avif": ("image/avif", "green64.avif"),
                    "/secret.png": ("image/png", "red64.png"),  # Intentionally NO CORS header!
                    "/media/sample.mp4": ("video/mp4", "sample.mp4"),
                    "/media/sample_fmp4.mp4": ("video/mp4", "sample_fmp4.mp4"),
                    "/media/init.mp4": ("video/mp4", "init.mp4"),
                    "/media/seg1.m4s": ("video/iso.segment", "seg1.m4s"),
                    "/media/tone.wav": ("audio/wav", "tone.wav"),
                }
                if path in media_map:
                    ct, akey = media_map[path]
                    self._send(200, ct, parent.assets[akey])
                    return

                if path == "/img/negotiated":
                    accept = hdrs.get("accept", "")
                    if "image/avif" in accept:
                        self._send(200, "image/avif", parent.assets["green64.avif"])
                    elif "image/webp" in accept:
                        self._send(200, "image/webp", parent.assets["green64.webp"])
                    else:
                        self._send(200, "image/png", parent.assets["green64.png"])
                    return

                # 9. Controllable Slow Subresources (/slow-css, /slow-js, /slow-img)
                if path == "/slow-css":
                    delay = int(qs.get("ms", ["800"])[0])
                    color = qs.get("bg", ["#2563eb"])[0]
                    time.sleep(delay / 1000.0)
                    css = f"#slow-box {{ width: 240px; height: 80px; background-color: {color}; }}\n"
                    if qs.get("paint_body", ["0"])[0] == "1":
                        css += f"body {{ background-color: {color} !important; }}\n"
                    self._send(200, "text/css; charset=utf-8", css.encode("utf-8"))
                    return

                if path == "/slow-js":
                    delay = int(qs.get("ms", ["800"])[0])
                    tag = qs.get("tag", ["SLOW_JS_DONE"])[0]
                    time.sleep(delay / 1000.0)
                    js = f"window.__slowJsTag = {json.dumps(tag)}; if (qs_title := {json.dumps(qs.get('set_title', ['0'])[0])} === '1') document.title = {json.dumps(tag)};\n"
                    # Valid JS syntax (avoid Python walrus in JS!)
                    set_t = "true" if qs.get("set_title", ["0"])[0] == "1" else "false"
                    js = f"window.__slowJsTag = {json.dumps(tag)}; if ({set_t}) {{ document.title = {json.dumps(tag)}; }}\n"
                    self._send(200, "application/javascript; charset=utf-8", js.encode("utf-8"))
                    return

                if path == "/slow-img":
                    delay = int(qs.get("ms", ["700"])[0])
                    time.sleep(delay / 1000.0)
                    self._send(200, "image/png", parent.assets["green64.png"])
                    return

                # 10. CORS endpoints
                if path == "/cors-ok":
                    self._send(
                        200,
                        "application/json",
                        b'{"cors":"ok"}',
                        {"Access-Control-Allow-Origin": "*"},
                    )
                    return
                if path == "/cors-deny":
                    self._send(200, "application/json", b'{"cors":"denied"}')
                    return

                self._send(404, "text/plain; charset=utf-8", f"Not Found: {path}".encode("utf-8"))

        return LongURLHTTPHandler
