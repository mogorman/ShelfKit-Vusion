#!/usr/bin/env python3
"""WebSocket bridge tests: the protocol primitives and the ESP32 bridge, over loopback.

    python ShelfKit-Server/tests/ws_test.py

No pytest, no hardware, no ESP32: a fake WebSocket client plays the hub's bridge,
so the host side (wsproto + WsBridge + SerialWorker) runs its real code. It covers
the handshake token guard, the masked/unmasked frame paths, the DTR/RTS -> BOOT/
RESET control, the baud announcement the flash relies on, and a worker reading a
hub line. Exit 0 = pass.
"""

from __future__ import annotations

import base64
import json
import os
import socket
import struct
import sys
import time
from pathlib import Path

SERVER = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVER))

from shelfkit import serial_link, ws_bridge, wsproto  # noqa: E402
from shelfkit.serial_link import PortSpec  # noqa: E402

FAILED: list[str] = []
TOKEN = "test-token-123"
os.environ["SHELFKIT_HUB_TOKEN"] = TOKEN


def check(name: str, ok: bool, detail=None) -> None:
    if ok:
        print(f"  PASS {name}")
    else:
        FAILED.append(name)
        print(f"  FAIL {name}" + (f"  <<< {detail}" if detail is not None else ""))


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class FakeHub:
    """Plays the ESP32: a minimal masked WebSocket client on a raw socket."""

    def __init__(self, port: int, token: str = TOKEN) -> None:
        self._s = socket.create_connection(("127.0.0.1", port), timeout=3)
        self._s.settimeout(None)
        self._key = base64.b64encode(b"0123456789abcdef").decode()
        query = f"?token={token}" if token else ""
        request = (f"GET /{query} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
                   f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {self._key}\r\n"
                   f"Sec-WebSocket-Version: 13\r\n\r\n")
        self._s.sendall(request.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            head += self._s.recv(1)
        self.handshake_ok = head.startswith(b"HTTP/1.1 101")
        self._buf = head.split(b"\r\n\r\n", 1)[1]

    def send_binary(self, data: bytes) -> None:
        self._send(wsproto.OP_BINARY, data)

    def send_text(self, text: str) -> None:
        self._send(wsproto.OP_TEXT, text.encode())

    def send_ping(self) -> None:
        self._send(wsproto.OP_PING, b"")

    def _send(self, opcode: int, payload: bytes) -> None:
        head = bytearray([0x80 | opcode])
        n = len(payload)
        mask = b"\x11\x22\x33\x44"
        if n < 126:
            head.append(0x80 | n)
        elif n < 1 << 16:
            head.append(0x80 | 126)
            head += struct.pack(">H", n)
        else:
            head.append(0x80 | 127)
            head += struct.pack(">Q", n)
        head += mask
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self._s.sendall(bytes(head) + masked)

    def _recv(self, n: int) -> bytes:
        while len(self._buf) < n:
            chunk = self._s.recv(4096)
            if not chunk:
                raise ConnectionError("closed")
            self._buf += chunk
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def recv_frame(self, timeout: float = 3.0) -> tuple[int, bytes]:
        self._s.settimeout(timeout)
        try:
            first = self._recv(2)
            opcode = first[0] & 0x0F
            length = first[1] & 0x7F
            masked = first[1] & 0x80
            if length == 126:
                length = struct.unpack(">H", self._recv(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._recv(8))[0]
            mask = self._recv(4) if masked else b"\x00" * 4
            payload = bytearray(self._recv(length)) if length else bytearray()
            if masked:
                for i in range(len(payload)):
                    payload[i] ^= mask[i & 3]
            return opcode, bytes(payload)
        finally:
            self._s.settimeout(None)

    def recv_binary(self, timeout: float = 3.0) -> bytes:
        """The next binary frame, skipping any text control frames in front of it."""
        while True:
            opcode, payload = self.recv_frame(timeout)
            if opcode == wsproto.OP_BINARY:
                return payload

    def close(self) -> None:
        try:
            self._s.close()
        except OSError:
            pass


def test_protocol() -> None:
    # The RFC 6455 worked example, so a broken key derivation fails loudly.
    check("accept_key matches the RFC vector",
          wsproto.accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
          wsproto.accept_key("dGhlIHNhbXBsZSBub25jZQ=="))
    check("is_ws recognises the URL forms",
          ws_bridge.is_ws("ws://h:8766") and ws_bridge.is_ws("wss://h")
          and not ws_bridge.is_ws("/dev/ttyUSB0"))


def test_bridge_control() -> None:
    port = free_port()
    url = f"ws://127.0.0.1:{port}"
    bridge = ws_bridge.bridge_for(PortSpec("hub", url))
    bridge.open()
    hub = FakeHub(port)
    check("a correct token gets the 101", hub.handshake_ok)
    time.sleep(0.1)

    bridge.dtr = True
    op, p = hub.recv_frame()
    check("dtr True is a boot control frame",
          op == wsproto.OP_TEXT and json.loads(p) == {"boot": True}, p)
    bridge.rts = True
    op, p = hub.recv_frame()
    check("rts True is a reset control frame",
          op == wsproto.OP_TEXT and json.loads(p) == {"reset": True}, p)

    # A reopen re-announces the baud the flasher depends on (115200 -> 38400 -> back).
    bridge.set_spec(PortSpec("hub", url, 38400))
    bridge.close()
    bridge.open()
    op, p = hub.recv_frame()
    check("reopen announces the bootloader baud",
          op == wsproto.OP_TEXT and json.loads(p) == {"baud": 38400}, p)

    # host -> hub data rides binary frames
    bridge.write(b"PING-HUB")
    check("a bridge write reaches the hub as binary", hub.recv_binary() == b"PING-HUB")

    # hub -> hub data lands in the reader
    want = b"hello from the hub"
    hub.send_binary(want)
    got = b""
    deadline = time.time() + 3
    while len(got) < len(want) and time.time() < deadline:
        got += bridge.read(len(want) - len(got))
    check("hub bytes are readable", got == want, got)
    check("in_waiting is 0 after the read", bridge.in_waiting == 0)

    # ping gets a pong (the keep-alive the bridge must answer)
    hub.send_ping()
    op, p = hub.recv_frame()
    check("a ping is answered with a pong", op == wsproto.OP_PONG, op)

    bridge.close()
    hub.close()


def test_token_guard() -> None:
    port = free_port()
    bridge = ws_bridge.bridge_for(PortSpec("hub", f"ws://127.0.0.1:{port}"))
    bridge.open()
    bad = FakeHub(port, token="wrong")
    check("a wrong token is refused (no 101)", not bad.handshake_ok)
    bad.close()
    ok = FakeHub(port, token=TOKEN)
    check("the right token is accepted", ok.handshake_ok)
    ok.close()
    bridge.close()


def test_worker_over_bridge() -> None:
    port = free_port()
    url = f"ws://127.0.0.1:{port}"
    lines: list[str] = []
    worker = serial_link.SerialWorker(PortSpec("hub", url), on_line=lines.append)
    worker.open()
    hub = FakeHub(port)
    check("the worker's bridge connected", hub.handshake_ok)
    time.sleep(0.1)

    worker.write(b"\x01\x02\x03")
    check("a worker write reaches the hub", hub.recv_binary() == b"\x01\x02\x03")

    hub.send_binary(b"*** ShelfKit hub, node 0001 net 5a\r\n")
    deadline = time.time() + 3
    while time.time() < deadline and not any("ShelfKit hub" in l for l in lines):
        time.sleep(0.05)
    check("a hub line reaches the worker callback", any("ShelfKit hub" in l for l in lines),
          lines)
    check("the pane reports open", worker.is_open)

    worker.close()
    hub.close()


def test_two_bridges_independent() -> None:
    """A hub and a tag can each ride their own ESP32 on their own port at once."""
    hub_url = f"ws://127.0.0.1:{free_port()}"
    tag_url = f"ws://127.0.0.1:{free_port()}"
    hub_bridge = ws_bridge.bridge_for(PortSpec("hub", hub_url))
    tag_bridge = ws_bridge.bridge_for(PortSpec("tag", tag_url))
    check("two URLs are two independent bridges", hub_bridge is not tag_bridge)
    check("the same URL is the same bridge (singleton)",
          ws_bridge.bridge_for(PortSpec("hub", hub_url)) is hub_bridge)

    hub_bridge.open()
    tag_bridge.open()
    hub = FakeHub(int(hub_url.rsplit(":", 1)[1]))
    tag = FakeHub(int(tag_url.rsplit(":", 1)[1]))
    time.sleep(0.1)

    hub_bridge.write(b"HUB-BYTE")
    tag_bridge.write(b"TAG-BYTE")
    check("the hub bridge reaches its own ESP32", hub.recv_binary() == b"HUB-BYTE")
    check("the tag bridge reaches its own ESP32", tag.recv_binary() == b"TAG-BYTE")

    hub_want = b"from the hub"
    tag_want = b"from the tag"
    hub.send_binary(hub_want)
    tag.send_binary(tag_want)
    deadline = time.time() + 3
    hub_got = b""
    while len(hub_got) < len(hub_want) and time.time() < deadline:
        hub_got += hub_bridge.read(len(hub_want) - len(hub_got))
    tag_got = b""
    while len(tag_got) < len(tag_want) and time.time() < deadline:
        tag_got += tag_bridge.read(len(tag_want) - len(tag_got))
    check("the hub reads only its own bytes", hub_got == hub_want, hub_got)
    check("the tag reads only its own bytes", tag_got == tag_want, tag_got)

    hub_bridge.close()
    tag_bridge.close()
    hub.close()
    tag.close()


def main() -> int:
    test_protocol()
    test_bridge_control()
    test_token_guard()
    test_worker_over_bridge()
    test_two_bridges_independent()
    print("\nRESULT:", "PASS" if not FAILED else f"FAIL {FAILED}")
    return 0 if not FAILED else 1


if __name__ == "__main__":
    sys.exit(main())
