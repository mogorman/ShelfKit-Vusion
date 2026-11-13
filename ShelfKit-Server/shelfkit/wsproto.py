"""A just-barely-enough WebSocket server: the handshake and the frame format.

It exists so a hub can ride an ESP32 bridge instead of a serial cable, and so we
don't have to pull a WS dependency into the server. Only what the bridge needs is
here: an upgrade handshake, one unmasked frame send, and one masked-frame read.
Nothing else (no subprotocols, no extensions, no client-side masking).

Frame layout is RFC 6455. Client-to-server frames are masked by the client; we
unmask them. Server-to-client frames are unmasked, per the spec.
"""

from __future__ import annotations

import base64
import hashlib
import struct
import threading

# Opcodes. Only the data ones plus the four control ones matter here.
OP_CONT = 0x0
OP_TEXT = 0x1
OP_BINARY = 0x2
OP_CLOSE = 0x8
OP_PING = 0x9
OP_PONG = 0xA

_WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
_MAX_FRAME = 1 << 20        # one frame is a serial burst, not a file; 1 MiB is plenty


def accept_key(key: str) -> str:
    """The Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key (RFC 6455)."""
    digest = hashlib.sha1((key + _WS_MAGIC).encode("latin-1")).digest()
    return base64.b64encode(digest).decode("ascii")


def read_handshake(sock) -> tuple:
    """Read the HTTP upgrade request. Returns (rfile, request_line, headers).

    The rfile is handed on to `Conn` so the socket has exactly one buffered reader
    (two would let the first frame's bytes get swallowed by the wrong one).
    """
    rfile = sock.makefile("rb")
    line = rfile.readline()
    if not line or b" " not in line:
        raise ConnectionError("not an HTTP request")
    request_line = line.decode("latin-1").strip()
    headers: dict[str, str] = {}
    while True:
        header_line = rfile.readline()
        if header_line in (b"\r\n", b"\n", b""):
            break
        key, _, value = header_line.decode("latin-1").partition(":")
        headers[key.strip().lower()] = value.strip()
    return rfile, request_line, headers


def handshake_ok(sock, key: str) -> None:
    """Send the 101 once the client's key has been checked and the accept value set."""
    reply = ("HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             f"Sec-WebSocket-Accept: {accept_key(key)}\r\n\r\n")
    sock.sendall(reply.encode("latin-1"))


class Conn:
    """One accepted WebSocket, wrapped to read and send single frames.

    Reading is on a buffered file object; sending is on the raw socket under a
    lock, because both the data bytes and the control frames (boot/reset/baud)
    go out the same socket from different call sites.
    """

    def __init__(self, sock, rfile=None) -> None:
        self._sock = sock
        self._rfile = rfile if rfile is not None else sock.makefile("rb")
        self._write_lock = threading.Lock()

    # ── in ─────────────────────────────────────────────────────────────────
    def _recv(self, n: int) -> bytes:
        data = self._rfile.read(n)
        if data is None or len(data) < n:
            raise ConnectionError("socket closed mid-frame")
        return data

    def recv_frame(self) -> tuple[int, bytes]:
        """One frame from the peer: (opcode, payload). Raises on close or EOF."""
        first = self._recv(2)
        opcode = first[0] & 0x0F
        masked = first[1] & 0x80
        length = first[1] & 0x7F
        if length == 126:
            length = struct.unpack(">H", self._recv(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", self._recv(8))[0]
        if length > _MAX_FRAME:
            raise ConnectionError("frame too large")
        mask = self._recv(4) if masked else b"\x00\x00\x00\x00"
        payload = bytearray(self._recv(length)) if length else bytearray()
        if masked:
            for i in range(len(payload)):
                payload[i] ^= mask[i & 3]
        if opcode == OP_CLOSE:
            raise ConnectionError("peer sent close")
        return opcode, bytes(payload)

    # ── out ────────────────────────────────────────────────────────────────
    def send(self, payload: bytes, opcode: int = OP_BINARY) -> None:
        """One unmasked frame to the peer. Data goes out as binary by default."""
        head = bytearray([0x80 | opcode])
        n = len(payload)
        if n < 126:
            head.append(n)
        elif n < 1 << 16:
            head.append(126)
            head += struct.pack(">H", n)
        else:
            head.append(127)
            head += struct.pack(">Q", n)
        with self._write_lock:
            self._sock.sendall(bytes(head) + payload)

    def close(self) -> None:
        """Best effort; closing a bridge never raises into its caller."""
        try:
            with self._write_lock:
                self._sock.shutdown(2)
        except OSError:
            pass
        finally:
            try:
                self._sock.close()
            except OSError:
                pass
