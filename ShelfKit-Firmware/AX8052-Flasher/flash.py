#!/usr/bin/env python3
"""Console for the AX8052 flasher on an ESP32-S3.

Uses pyserial. No other packages.
DTR and RTS stay off so opening the port does not reset the ESP32-S3.

    python flash.py COM5 help
    python flash.py COM5 unlock
    python flash.py COM5 read 0 40
    python flash.py COM5 read 0 8000 --out firmware.bin
    python flash.py COM5 write 0 00000000
    python flash.py COM5 flash firmware.bin --addr 0x200 --steps 4
    python flash.py COM5 erase
    python flash.py COM5 setkey FFFFFFFFFFFFFFFF
    python flash.py COM5 pc
    python flash.py COM5 steps 4

Addresses and lengths on read, dump, and write are hex, as on the console.
steps takes a decimal count from 1 to 16.
"""

from __future__ import print_function

import argparse
import os
import re
import serial
import sys
import time

READ_LINE = re.compile(r"^([0-9A-Fa-f]{4}):((?:\s+[0-9A-Fa-f]{2})+)\s*$")
HEX_KEY = re.compile(r"^[0-9A-Fa-f]{16}$")
HEX_BYTES = re.compile(r"^[0-9A-Fa-f]*$")
CHUNK = 128
CALIB = 0xFC00
SCAN_LINES = 29

FAIL_EXACT = {
    "ack",
    "pc reply",
    "calibration page",
    "write range",
    "write needs an even address and an even length",
    "range exceeds code space",
    "code config ack",
    "short dump reply",
    "setkey range",
    "setkey reply",
    "erase reply",
    "erase incomplete",
    "status after erase",
    "steps 1..16",
    "key needs 16 hex digits",
    "setkey needs 16 hex digits",
    "read <addr> <len>",
    "write <addr> <hexbytes>",
    "unknown, help",
    "line too long",
    "capture setup failed",
    "poll none",
}


class Port(object):
    def __init__(self, name):
        try:
            self.s = serial.Serial(name, 115200, timeout=0.02, write_timeout=2)
        except (serial.SerialException, OSError) as exc:
            raise OSError("cannot open %s (%s)" % (name, exc))
        self.s.dtr = False
        self.s.rts = False
        self.s.reset_input_buffer()
        self.s.reset_output_buffer()

    def close(self):
        if self.s is not None:
            self.s.close()
            self.s = None

    def write(self, text):
        data = text.encode("ascii")
        written = self.s.write(data)
        if written != len(data):
            raise OSError("short write (%d of %d)" % (written, len(data)))
        self.s.flush()

    def read_some(self):
        count = min(self.s.in_waiting, 4096)
        return self.s.read(count) if count else b""


def parse_hex(text):
    value = text.strip().lower()
    if value.startswith("0x"):
        value = value[2:]
    if not value or any(c not in "0123456789abcdef" for c in value):
        raise ValueError("not hex: %s" % text)
    return int(value, 16)


def parse_number(text):
    return int(text, 0)


def parse_dump(text):
    out = bytearray()
    for line in text.splitlines():
        match = READ_LINE.match(line.strip())
        if not match:
            continue
        for part in match.group(2).split():
            out.append(int(part, 16))
    return out


def failure_line(text):
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("locked:") or stripped.startswith("link:") or stripped.startswith("stop:"):
            return stripped
        if stripped.startswith("read:"):
            return stripped
        if stripped.startswith("write fail"):
            return stripped
        if stripped in FAIL_EXACT:
            return stripped
    return None


def drain(port):
    time.sleep(0.15)
    quiet = time.monotonic()
    while time.monotonic() - quiet < 0.15:
        chunk = port.read_some()
        if chunk:
            quiet = time.monotonic()
        else:
            time.sleep(0.02)


def transact(port, cmd, seconds, done):
    print(">> " + cmd)
    port.write(cmd + "\n")
    deadline = time.monotonic() + seconds
    got = ""
    while time.monotonic() < deadline:
        chunk = port.read_some()
        if chunk:
            got += chunk.decode("ascii", "replace")
            if failure_line(got) or done(got):
                time.sleep(0.2)
                extra = port.read_some()
                if extra:
                    got += extra.decode("ascii", "replace")
                sys.stdout.write(got)
                if got and not got.endswith("\n"):
                    sys.stdout.write("\n")
                bad = failure_line(got)
                if bad:
                    raise RuntimeError(bad)
                return got
        time.sleep(0.04)
    if got:
        sys.stdout.write(got)
        if not got.endswith("\n"):
            sys.stdout.write("\n")
    raise TimeoutError("timeout waiting after %s" % cmd)


def expect(port, cmd, seconds, needle, count=1):
    def done(text):
        return text.count(needle) >= count
    return transact(port, cmd, seconds, done)


def expect_retry(port, cmd, seconds, needle, count=1, retries=3):
    """expect(), re-sent after a stall or link error.

    A timed-out or desynced chunk is safe to repeat: the ESP32 re-enters debug and
    re-unlocks on every write, and the final read-back still has to match. The settle
    delay lets a brown-out reboot finish re-bannering before the next attempt.
    """
    attempt = 0
    while True:
        try:
            return expect(port, cmd, seconds, needle, count)
        except (TimeoutError, RuntimeError) as exc:
            attempt += 1
            if attempt > retries:
                raise
            reason = "timeout" if isinstance(exc, TimeoutError) else str(exc)
            print("  retry %d/%d after %s" % (attempt, retries, reason))
            time.sleep(2.0)
            drain(port)


def load_image(path, addr):
    with open(path, "rb") as fh:
        data = bytearray(fh.read())
    if not data:
        raise ValueError("empty image")
    if addr & 1:
        raise ValueError("address must be even")
    if len(data) & 1:
        data.append(0xFF)
    if addr + len(data) > CALIB:
        raise ValueError("image reaches the calibration page")
    return data


def load_hex(path):
    """Parse an Intel .hex into (bytes, base_addr) for the debug-port flash.

    Builds a byte array over [low, high] filled with FF, placing each present
    byte, so a sparse image (the bootloader is mostly FF) flashes the same as
    its .bin would. Gaps flash as FF no-ops; erase first to clear stale bits.
    """
    if not os.path.isfile(path):
        raise ValueError("hex file not found: %s" % path)
    base = 0
    data = {}
    for raw in open(path, "r"):
        raw = raw.strip()
        if not raw.startswith(":"):
            continue
        b = bytes.fromhex(raw[1:])
        n, addr, rtype = b[0], int.from_bytes(b[1:3], "big"), b[3]
        payload = b[4:4 + n]
        if rtype == 0:
            for i in range(len(payload)):
                data[base + addr + i] = payload[i]
        elif rtype == 2:
            base = int.from_bytes(payload, "big") << 16
    if not data:
        raise ValueError("empty hex: %s" % path)
    lo, hi = min(data), max(data)
    out = bytearray(b"\xFF" * (hi - lo + 1))
    for a, v in data.items():
        out[a - lo] = v
    if lo + len(out) > 0x10000:
        raise ValueError("hex exceeds code space")
    if hi + 1 > CALIB:
        raise ValueError("hex reaches the calibration page")
    return out, lo


def check_key(text):
    if not HEX_KEY.match(text):
        raise ValueError("key needs 16 hex digits")
    return text.upper()


def check_write_chunk(addr, hex_bytes):
    if addr & 1:
        raise ValueError("address must be even")
    if not HEX_BYTES.match(hex_bytes) or (len(hex_bytes) & 1) or len(hex_bytes) == 0:
        raise ValueError("write needs an even number of hex digits")
    nbytes = len(hex_bytes) // 2
    if nbytes > CHUNK or (nbytes & 1):
        raise ValueError("write takes an even length, at most 128 bytes")
    if addr + nbytes > CALIB:
        raise ValueError("image reaches the calibration page")


def read_memory(port, addr, length, out_path):
    if length <= 0 or addr + length > 0x10000:
        raise ValueError("range exceeds code space")
    last = addr + ((length - 1) // 16) * 16
    needle = "%04X:" % last
    seconds = 15 + (length * 8) // 100
    reply = expect(port, "read %X %X" % (addr, length), seconds, needle)
    data = parse_dump(reply)
    if len(data) != length:
        raise ValueError("read back %d bytes, asked for %d" % (len(data), length))
    if out_path:
        with open(out_path, "wb") as fh:
            fh.write(data)
        print("saved %s (%d bytes)" % (out_path, len(data)))
    return data


def program(port, addr, data, steps, retries=3):
    expect_retry(port, "unlock", 20, "unlocked 1", retries=retries)
    off = 0
    while off < len(data):
        n = min(CHUNK, len(data) - off)
        chunk = data[off:off + n]
        if any(b != 0xFF for b in chunk):  # an erased region is already FF; skip the pad
            hex_bytes = "".join("%02X" % b for b in chunk)
            expect_retry(port, "write %04X %s" % (addr + off, hex_bytes), 40, "write ok", retries=retries)
        off += n
    off = 0
    while off < len(data):
        n = min(CHUNK, len(data) - off)
        chunk = data[off:off + n]
        if any(b != 0xFF for b in chunk):  # trust the verified erase for all-FF regions
            reply = expect(port, "read %04X %X" % (addr + off, n), 15 + (n * 8) // 100, "%04X:" % (addr + off))
            back = parse_dump(reply)
            if len(back) != n:
                raise ValueError("read back %d bytes at %04X, expected %d" % (len(back), addr + off, n))
            for i in range(n):
                if back[i] != chunk[i]:
                    raise ValueError("mismatch at %04X: flash %02X image %02X" % (addr + off + i, back[i], chunk[i]))
        off += n
    print("image matches")
    if steps:
        expect(port, "steps %X" % steps, 40, "pc ", steps + 1)


def _relay_enter(port):
    print(">> uart")
    port.write("uart\n")
    time.sleep(0.5)
    drain(port)


def _relay_exit(port):
    port.write("Q\n")
    time.sleep(0.3)
    drain(port)


def _read_line(port, seconds):
    deadline = time.monotonic() + seconds
    buf = ""
    while time.monotonic() < deadline:
        chunk = port.read_some()
        if chunk:
            buf += chunk.decode("ascii", "replace")
            if "\n" in buf:
                return buf
        time.sleep(0.02)
    return buf


def uart_flash(port, hex_path, line_delay=0.01):
    """Flash a .hex file through the ESP32 UART0 bridge.

    The `uart` console command makes the ESP32 enter the AX8052 bootloader and
    relay this port to its UART0 at 38400. We then speak the bootloader protocol
    directly (same as axsem-flasher.py): ? -> banner, K -> erase OK, hex lines,
    R -> run. A lone Q closes the relay and returns to the flasher console.
    """
    if not os.path.isfile(hex_path):
        raise ValueError("hex file not found: %s" % hex_path)
    with open(hex_path, "r") as fh:
        lines = [ln.rstrip("\r\n") for ln in fh if ln.strip()]
    if not lines:
        raise ValueError("empty hex file")

    _relay_enter(port)
    try:
        banner = ""
        for _ in range(3):
            port.write("?")
            banner = _read_line(port, 3)
            if banner:
                sys.stdout.write(banner)
                if not banner.endswith("\n"):
                    sys.stdout.write("\n")
            if "Bootloader" in banner:
                break
        if "Bootloader" not in banner:
            raise RuntimeError("no bootloader banner (chip not in boot mode, or TX not wired back)")
        port.write("K")
        reply = _read_line(port, 30)
        if reply:
            sys.stdout.write(reply)
            if not reply.endswith("\n"):
                sys.stdout.write("\n")
        if "OK" not in reply:
            raise RuntimeError("erase not confirmed: %r" % reply.strip())
        print("programming %d hex lines" % len(lines))
        for ln in lines:
            port.write(ln + "\n")
            time.sleep(line_delay)
        time.sleep(0.2)
        port.write("R")
        time.sleep(0.3)
        print("done")
    finally:
        _relay_exit(port)


def uart_probe(port):
    """Enter the UART0 bridge, print the AX8052 bootloader banner, then exit.

    No erase, no flash -- just confirms the boot dance (GPIO36) and the
    PB3/PB4/PB5 wiring. Fails if the banner lacks 'Bootloader'.
    """
    _relay_enter(port)
    try:
        banner = ""
        for _ in range(3):
            port.write("?")
            line = _read_line(port, 3)
            if line:
                sys.stdout.write(line if line.endswith("\n") else line + "\n")
            if "Bootloader" in line:
                banner = line
                break
        if "Bootloader" not in banner:
            raise RuntimeError("no bootloader banner (check the PB3/PB4/PB5 wiring, or the chip has no bootloader yet)")
        print("banner OK -- chip is in the bootloader")
    finally:
        _relay_exit(port)


def uart_raw(port, seconds=9):
    """Enter the UART0 bridge and dump every raw byte for `seconds`.

    Diagnostic only: shows the chip's exact boot stream (jedec/hdr/none/banner,
    or a repeating jedec line if it is reset-looping) without the probe's
    one-line-per-pass parsing.
    """
    _relay_enter(port)
    try:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            d = port.read(512)
            if d:
                sys.stdout.write(d.decode("latin1"))
                sys.stdout.flush()
    finally:
        _relay_exit(port)


def run_command(port, args):
    cmd = args.cmd
    if cmd == "help":
        expect(port, "help", 5, "Do not power the AX8052 from 5 V.")
    elif cmd == "pins":
        expect(port, "pins", 5, "DBG_EN")
    elif cmd == "key":
        expect(port, "key " + check_key(args.hexkey), 5, "key ok")
    elif cmd == "setkey":
        expect(port, "setkey " + check_key(args.hexkey), 30, "setkey ok")
    elif cmd in ("unlock", "status"):
        expect(port, cmd, 20, "unlocked ")
    elif cmd in ("read", "dump"):
        read_memory(port, parse_hex(args.addr), parse_hex(args.length), args.out)
    elif cmd == "write":
        addr = parse_hex(args.addr)
        check_write_chunk(addr, args.hex)
        expect(port, "write %04X %s" % (addr, args.hex.upper()), 30, "write ok")
    elif cmd == "flash":
        if args.image.lower().endswith(".hex"):
            data, addr = load_hex(args.image)
        else:
            data = load_image(args.image, args.addr)
            addr = args.addr
        program(port, addr, data, args.steps, retries=getattr(args, "retries", 3))
    elif cmd == "erase":
        expect(port, "erase", 90, "erase done")
    elif cmd == "pc":
        expect(port, "pc", 20, "pc ")
    elif cmd == "reset":
        expect(port, "reset", 20, "pc ")
    elif cmd == "step":
        expect(port, "step", 20, "pc ")
    elif cmd == "steps":
        if args.n < 1 or args.n > 16:
            raise ValueError("steps must be 1..16")
        expect(port, "steps %X" % args.n, 40, "pc ", args.n + 1)
    elif cmd == "loop":
        expect(port, "loop", 10, "loop high")
    elif cmd in ("bits", "bitsod"):
        expect(port, cmd, 15, "zeros ")
    elif cmd in ("wire5", "wire6"):
        expect(port, cmd, 20, "zeros ", 5)
    elif cmd == "trace":
        expect(port, "trace", 20, "trace listen lows")
    elif cmd == "sense":
        expect(port, "sense", 15, "sense CLK drive")
    elif cmd == "scan":
        def done(text):
            return len(re.findall(r"(?m)^scan \d+ ", text)) >= SCAN_LINES
        transact(port, "scan", 30, done)
    elif cmd == "uart":
        uart_flash(port, args.hexfile, line_delay=getattr(args, "line_delay", 0.01))
    elif cmd == "probe":
        uart_probe(port)
    elif cmd == "raw":
        uart_raw(port, getattr(args, "seconds", 9))
    else:
        raise ValueError("unknown command %s" % cmd)


def build_parser():
    parser = argparse.ArgumentParser(
        description="Drive the AX8052 flasher console on an ESP32-S3.")
    parser.add_argument("port", help="serial port of the ESP32-S3")
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("help", help="print the console command list")
    sub.add_parser("pins", help="print the ESP32 GPIO numbers")

    key = sub.add_parser("key", help="keep the unlock key in the ESP32")
    key.add_argument("hexkey", help="16 hex digits, high byte first")
    setkey = sub.add_parser("setkey", help="program the debug key into the chip")
    setkey.add_argument("hexkey", help="16 hex digits, high byte first")

    sub.add_parser("unlock", help="open the port and print status")
    sub.add_parser("status", help="same as unlock")

    for name in ("read", "dump"):
        reader = sub.add_parser(name, help="read code flash")
        reader.add_argument("addr", help="hex start address")
        reader.add_argument("length", help="hex byte count")
        reader.add_argument("--out", help="also write the raw bytes to this file")

    writer = sub.add_parser("write", help="program one hex chunk, at most 128 bytes")
    writer.add_argument("addr", help="even hex address, below FC00")
    writer.add_argument("hex", help="even number of hex digits")

    image = sub.add_parser("flash", help="write a .bin, read it back, compare")
    image.add_argument("image", help="firmware file")
    image.add_argument("--addr", default="0", type=parse_number, help="start address, decimal or 0x hex")
    image.add_argument("--steps", default=0, type=int, help="after a match, step the core 1..16 times")
    image.add_argument("--retries", default=3, type=int,
                       help="re-send a timed-out or failed chunk this many times (default 3)")

    uart = sub.add_parser("uart", help="flash a .hex through the UART0 bootloader bridge")
    uart.add_argument("hexfile", help="Intel-hex firmware file")
    uart.add_argument("--line-delay", default=0.01, type=float,
                      help="seconds between hex lines (default 0.01)")
    sub.add_parser("probe", help="enter the UART bridge, print the bootloader banner, exit (no flash)")
    raw = sub.add_parser("raw", help="enter the UART bridge and dump raw bytes (diagnostic)")
    raw.add_argument("--seconds", default=9, type=int, help="how long to capture (default 9)")

    sub.add_parser("erase", help="secure-erase the main flash and the debug key")
    sub.add_parser("pc", help="print the program counter")
    sub.add_parser("reset", help="reset the CPU, leave it halted, print the counter")
    sub.add_parser("step", help="execute one instruction and print the counter")
    steps = sub.add_parser("steps", help="reset, then step N times, in one session")
    steps.add_argument("n", type=int, help="decimal count 1..16")

    sub.add_parser("loop", help="check that DATA_IN follows DATA_OUT")
    sub.add_parser("bits", help="96 clocks with the buffer driving")
    sub.add_parser("bitsod", help="96 clocks with DATA_OUT released")
    sub.add_parser("wire5", help="sample the GPIO5 data wire")
    sub.add_parser("wire6", help="sample the GPIO6 data wire")
    sub.add_parser("trace", help="trace CLK, DBG_EN, RESET_N, and DATA")
    sub.add_parser("sense", help="read the four debug pins")
    sub.add_parser("scan", help="read the header pins")
    return parser


def main(argv):
    # python flash.py COM5 firmware.bin  is the flash command.
    if len(argv) >= 2 and not argv[1].startswith("-") and os.path.isfile(argv[1]):
        argv = [argv[0], "flash"] + argv[1:]
    parser = build_parser()
    args = parser.parse_args(argv)
    if getattr(args, "steps", 0) < 0 or getattr(args, "steps", 0) > 16:
        parser.error("steps must be 0..16")
    port = Port(args.port)
    try:
        drain(port)
        run_command(port, args)
    finally:
        port.close()


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (OSError, ValueError, TimeoutError, RuntimeError) as exc:
        print(exc, file=sys.stderr)
        sys.exit(1)
