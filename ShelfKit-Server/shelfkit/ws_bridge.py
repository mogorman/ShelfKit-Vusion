"""A hub over IP: an ESP32 bridges the hub's UART to a WebSocket on this host.

The host runs the WebSocket *server*; the ESP32 dials in. `WsBridge` makes that
connection look like an open serial port, so `SerialWorker` and the bootloader
flasher drive it with no changes: open/close, read/write, in_waiting, and the
DTR/RTS lines become the hub's BOOT/RESET control.

It is a per-port singleton. The flasher closes the pane, opens the same port at
the bootloader's 38400, flashes, and reopens it at 115200 - all through this one
long-lived bridge. close() therefore detaches the reader only; it never tears the
WebSocket down, so the next open() re-attaches to the same ESP32.
"""

from __future__ import annotations

import json
import os
import socket
import threading
from urllib.parse import urlsplit

from shelfkit import wsproto
from shelfkit.serial_link import READ_TIMEOUT_S, PortSpec

_TOKEN_ENV = "SHELFKIT_HUB_TOKEN"

_BRIDGES: dict[str, "WsBridge"] = {}
_BRIDGES_LOCK = threading.Lock()


def is_ws(port: str) -> bool:
    return port.startswith(("ws://", "wss://"))


def bridge_for(spec: PortSpec) -> "WsBridge":
    """The one bridge for this port's URL, reconfigured for this spec's baud."""
    with _BRIDGES_LOCK:
        bridge = _BRIDGES.get(spec.port)
        if bridge is None:
            bridge = WsBridge(spec)
            _BRIDGES[spec.port] = bridge
    bridge.set_spec(spec)
    return bridge


class WsBridge:
    """A duck-typed serial port whose bytes travel over a WebSocket to an ESP32."""

    def __init__(self, spec: PortSpec) -> None:
        self._spec = spec
        host, port = self._host_port(spec.port)
        self._listen_host, self._listen_port = host, port
        self._token = os.environ.get(_TOKEN_ENV, "")
        # The one ESP32 we talk to, and where its bytes pile up between reads.
        self._conn: wsproto.Conn | None = None
        self._conn_lock = threading.Lock()
        self._buf = bytearray()
        self._buf_cond = threading.Condition()
        # The listener that accepts the ESP32 and keeps re-accepting after a drop.
        self._listener: socket.socket | None = None
        self._listen_lock = threading.Lock()
        self._open = False
        # Last-driven control levels, so a reopen re-asserts them.
        self._dtr = False
        self._rts = False

    # ── identity / configuration ───────────────────────────────────────────
    @staticmethod
    def _host_port(url: str) -> tuple[str, int]:
        parts = urlsplit(url)
        return (parts.hostname or "0.0.0.0"), (parts.port or 8766)

    def set_spec(self, spec: PortSpec) -> None:
        self._spec = spec          # on a reopen only the baud has moved

    # ── serial-port face ───────────────────────────────────────────────────
    @property
    def is_open(self) -> bool:
        return self._open

    @property
    def in_waiting(self) -> int:
        with self._buf_cond:
            return len(self._buf)

    @property
    def dtr(self) -> bool:
        return self._dtr

    @dtr.setter
    def dtr(self, value) -> None:
        self._dtr = bool(value)
        self._control("boot", self._dtr)

    @property
    def rts(self) -> bool:
        return self._rts

    @rts.setter
    def rts(self, value) -> None:
        self._rts = bool(value)
        self._control("reset", self._rts)

    def open(self) -> None:
        if self._open:
            return
        self._ensure_listener()      # raises OSError if the port is untakeable
        self._open = True
        # Tell the ESP32 the line rate this spec wants. On a reopen after a flash
        # this is what puts the hub back at 115200.
        self._control("baud", self._spec.baud)

    def close(self) -> None:
        # Detach the reader only. The WebSocket stays up so the flasher (and a
        # later reopen) reuse the same bridge and the same ESP32.
        self._open = False

    def write(self, data) -> int:
        payload = bytes(data)
        with self._conn_lock:
            conn = self._conn
        if conn is None:
            return 0                # the ESP32 isn't here; the bytes are dropped
        conn.send(payload, wsproto.OP_BINARY)
        return len(payload)

    def read(self, n: int = 1) -> bytes:
        if n <= 0:
            return b""
        with self._buf_cond:
            if not self._buf:
                self._buf_cond.wait(READ_TIMEOUT_S)
            if not self._buf:
                return b""
            out = bytes(self._buf[:n])
            del self._buf[:n]
        return out

    def flush(self) -> None:
        pass

    # ── the WebSocket side ─────────────────────────────────────────────────
    def _ensure_listener(self) -> None:
        with self._listen_lock:
            if self._listener is not None:
                return
            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((self._listen_host, self._listen_port))
            listener.listen(1)
            listener.settimeout(0.5)
            self._listener = listener
            threading.Thread(target=self._accept_loop,
                             name=f"ws-accept-{self._listen_port}", daemon=True).start()

    def _accept_loop(self) -> None:
        while self._listener is not None:
            try:
                client, _addr = self._listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self._handle_connect(client)

    def _handle_connect(self, client: socket.socket) -> None:
        try:
            rfile, request_line, headers = wsproto.read_handshake(client)
            key = headers.get("sec-websocket-key", "")
            if not key or headers.get("upgrade", "").lower() != "websocket":
                client.sendall(b"HTTP/1.1 400 Bad Request\r\n\r\n")
                client.close()
                return
            if not self._token_ok(request_line):
                client.sendall(b"HTTP/1.1 401 Unauthorized\r\n\r\n")
                client.close()
                return
            with self._conn_lock:
                if self._conn is not None:      # one hub per host (v1)
                    self._conn.close()
                    self._conn = None
            wsproto.handshake_ok(client, key)
            conn = wsproto.Conn(client, rfile)
            with self._conn_lock:
                self._conn = conn
            threading.Thread(target=self._pump, args=(conn,),
                             name="ws-hub-pump", daemon=True).start()
        except (ConnectionError, OSError):
            try:
                client.close()
            except OSError:
                pass

    def _token_ok(self, request_line: str) -> bool:
        if not self._token:
            return True                      # no token set: open (dev box)
        parts = request_line.split(" ", 2)
        path = parts[1] if len(parts) > 1 else ""
        if "?" in path:
            path = path.split("?", 1)[1]
        return any(pair.partition("=")[2] == self._token
                   for pair in path.split("&") if pair.partition("=")[0] == "token")

    def _pump(self, conn: wsproto.Conn) -> None:
        try:
            while True:
                opcode, payload = conn.recv_frame()
                if opcode == wsproto.OP_BINARY:
                    with self._buf_cond:
                        self._buf += payload
                        self._buf_cond.notify_all()
                elif opcode == wsproto.OP_PING:
                    conn.send(payload, wsproto.OP_PONG)
                # OP_TEXT from the hub: nothing to act on yet (v1); OP_PONG: ignore
        except (ConnectionError, OSError):
            pass
        finally:
            conn.close()
            with self._conn_lock:
                if self._conn is conn:        # a newer connection may have replaced us
                    self._conn = None

    def _control(self, name: str, value) -> None:
        with self._conn_lock:
            conn = self._conn
        if conn is None:
            return
        conn.send(json.dumps({name: value}).encode("ascii"), wsproto.OP_TEXT)
