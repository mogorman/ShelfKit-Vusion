# Talking to the devices — the full host-side surface

Everything a host must implement to drive the ShelfKit fleet: the hub's serial
command channel (and its log), the wireless link it triggers, the DATA payloads
it builds, the image pipeline that produces the bytes, chip flashing, the
ESP32 hub-over-IP bridge, and the tag's debug UART.

This is the specification for reimplementing the host elsewhere (the Elixir
port). The Python server is the reference implementation; every section names
its source of truth so each claim can be checked line by line.

```
  tag (CR2450)                              hub (USB serial)               host
  ┌────────────────┐   POLL  ────────────►  ┌──────────────┐  hex lines  ┌──────────────┐
  │ AX8052F143     │  ◄──────── POLL_RSP    │ AX8052F143   │  ◄───────── │ the server   │
  │ + GDEW026Z39   │  ◄──── DATA chunks ─── │ + UART0 TX/RX│  ─────────► │              │
  └────────────────┘  ────────► ACK         └──────────────┘             └──────────────┘
        │ 868.3 MHz                              │ 115200 8N1 (or via ESP32 over IP)
        │                                        │
        └── its own debug UART (115200) ─────────┴── optional second serial port on the host
```

Division of labour, which shapes everything below:

- **Tags poll; the hub answers.** A tag wakes, sends a `POLL`, and listens for a
  short window. If the hub's reply says "data follows", the tag keeps its
  receiver open and the hub streams what the host queued.
- **The hub decides nothing and stores nothing.** It relays polls to the host
  and forwards DATA frames the host orders. Every command the host issues
  waits for its tag's next check-in.
- **The host's only levers** are the hub's serial line (directly, or through
  the ESP32 bridge) and the tag's debug UART (log only, plus the bootloader).

The wireless half never needs a host implementation — the hub's firmware owns
the radio. The host just builds payload bytes and obeys the timings in §2.5.

## 1. The hub serial channel

Sources: `ShelfKit-Server/shelfkit/serial_link.py`,
`ShelfKit-Server/shelfkit/hubhost.py`, `ShelfKit-Firmware/Vusion-Hub/src/main.c`.

### 1.1 Port settings

- **115200 baud, 8N1**, the hub's application rate (`serial_link.py:19`).
  The mask-ROM bootloader on the same wire runs at **38400** (`flasher.py:43`);
  its boot lines appear as noise on a 115200 port.
- **DTR is the board's BOOT pin, RTS is its RESET line**, both defaulting to
  released (`False`) (`serial_link.py:91`). Opening a port re-applies the modem
  lines, so a glitch on RTS is a reset pulse: the server keeps one long-lived
  open port per board and never closes/reopens it for a transfer
  (`app.py:1674-1679`).
- No hardware flow control (`dsrdtr=False, rtscts=False`).
- The hub's UART **RX is a one-byte holding register polled in the main loop**
  (`main.c:439-453`). The protocol is therefore strictly one command in
  flight, wait for the answer; while the hub prints a `POLL` line (~18 ms)
  incoming bytes can be lost, which the checksum plus host retry covers
  (`wireless-link.md`, "Image push").
- Max line length the hub accepts is **257 chars** (`LINE_MAX`,
  `main.c:130`); an overlong line is answered `ERR len` and discarded
  (`main.c:381-382`).

### 1.2 Line format

One command per line, ASCII, `\n`-terminated; `\r` is ignored. Everything after
the command character is lowercase/uppercase **hex** (`main.c:154-163`).
Checksummed commands end in a `chk` byte that makes the sum of **all decoded
bytes** (address, payload and the checksum itself) 0 mod 256 — i.e. `chk =
(-sum(body)) & 0xFF` (`hubhost.py:100-113`, `main.c:195-199`).

### 1.3 Command table

| Line | Hub answer | Meaning |
|---|---|---|
| `P` | `PONG` | liveness probe for the port |
| `V` | the boot banner again (see §2.1) | ask a hub booted before the host for its capabilities |
| `Q<hhhh>` | `OK data` · `ERR full` · `ERR hex` | **pre-arm**: the next poll from tag `hhhh` is answered with cmd=01 ("data follows"). 4 slots (`PREARM_SLOTS`, `main.c:35`); full → `ERR full`, which must be surfaced, not swallowed — an unpre-armed tag closes its window and the push misses |
| `L<hh>` | `OK delay` · `ERR hex` | ask **every** tag to wait at least `hh × 10 s` before its next poll; carried in each `POLL_RSP` until consumed |
| `S<addr:4><payload 1..32 B><chk:2>` | `SENT <addr> ok\|fail att=N` · `ERR len\|hex\|chk\|addr\|busy\|send` | one reliable DATA frame to node `addr` (the hub runs the air retries itself, up to 4 transmissions ≈ 4.5 s, then answers). `addr` is 2 bytes little-endian; `addr` may not be the hub itself or 0 |
| `B<addr:4><count:2>(<len:2><payload>)…<chk:2>` | one `SENT <addr> ok\|fail att=N` for the whole batch · `BAT <i>/<n> no ack` on a failure · same `ERR` set | up to **16 frames** of ≤32 payload bytes each, sent back to back with the same air retries; removes the per-frame serial round-trip (~41 of 44 ms per frame). One batch at a time; an `S` while a batch is in flight gets `ERR busy` |
| `D<msg 1..131 B><chk:2>` | `OK img` · `ERR len\|hex\|chk\|cmd` | image message for the **hub's own panel**: exactly the same `SM_OP_IMG_*` payloads a tag would receive, but chunks may be up to 128 bytes (`IMG_SER_CHUNK`, `main.c:249`) and **never compressed**. `OK img` is printed after the message is handled — BEGIN resets the panel (~1.5 s) and END blocks for the whole refresh (~15 s), during which the host must write nothing (PB5 doubles as the panel's reset line) |

Any other first character → `ERR cmd`. A bad line is answered and ignored;
the host retries.

### 1.4 Log lines the host must parse

The hub prints, on the same port, what it sees over the air. This is the only
channel *back* from the tags (flash read-back bytes ride in here).

```
*** ShelfKit hub, node 0001 net 5a dump=1 batch=1 show=1 fw=1.0.0-12     (boot, and on V)
POLL src=0100 seq=09 rssi=-36 up=72s att=1 hubrssi=-35 vdd=0300           (a tag announced)
RSP 0100 cmd=01                                                            (what the hub answered)
DATA src=0102 seq=1a rssi=-30 flash=0010000a534b494401010209e7             (a dump frame relayed)
SENT 0100 ok att=1                                                          (answers to S/B)
BAT 2/8 no ack                                                              (which batch frame failed)
ERR len|hex|chk|addr|busy|send|cmd|full                                     (bad/declined line)
```

Details (`main.c:53-110`, `main.c:516-520`):

- **Banner** = capability discovery. `dump=1` → the hub forwards tag `DATA`
  payloads as `flash=…` (flash read-back needs it); `batch=1` → accepts `B`
  lines; `show=1` → accepts `D` lines. Older hubs print fewer markers; read
  the banner, don't assume. The banner prints once at boot, so a host that
  starts later must send `V` to get it (`app.py:1316-1343`).
- **POLL line**: `src` = tag node id (4 hex), `seq` (1 hex), `rssi` (signed dBm
  the hub measured), optional `retry` (frame's retry flag) and `dup` (duplicate
  filter hit). Payload fields are printed only when the bytes are present:
  `up=<uptime s>` `att=<attempts the tag's previous poll needed>`
  `hubrssi=<signed dBm>` — and, only on the 6-byte and 15-byte polls,
  `vdd=<volts×256 hex>` and, only on an NFC-tap-started poll,
  `fw=<A.B.C-BB> avg=<N>uA life=<N>d` (`main.c:76-108`).
- **RSP line**: `cmd=00` = "nothing queued" (the tag will close its receiver
  after ~500 ms), `cmd=01` = "data follows" (the tag stays awake). This one
  field decides whether a push lands; the server refuses to push to a tag it
  just answered with cmd=00 within the last 3 s (`app.py:2078-2083`).
- **DATA … flash=**: 2 bytes address (LE 24) + 1 byte count + count bytes of
  SPI flash, from a tag's `SM_OP_FLASH_DATA` reply. This is the *only* path
  those bytes have back to the host (`main.c:66-73`).

Banners from the other ends of the fleet, for the tag's debug port and reset
detection (`app.py:88-90`, tag `main.c:544-557`):

```
*** ShelfKit tag, node 0100 net 5a dump=1 fw=1.0.0-12        (tag firmware)
AX8052 Bootloader …                                          (mask-ROM bootloader)
```

### 1.5 Timing constants (host side)

All values from `hubhost.py` and `app.py`; the comments there say why each
exists. A port that changes them must keep the reasons or it breaks silently.

| Constant | Value | Why |
|---|---|---|
| `PUSH_SETTLE_S` | 80 ms | wait before the first frame after a poll: the tag is still settling into receive; without it every pre-rendered push failed 4/4 |
| `BEGIN_SETTLE_S` | 2.0 s | after IMG/FW BEGIN the tag resets/erases and answers nothing meanwhile (measured ~0.9 s) |
| `END_SETTLE_S` | 20.0 s | after IMG END the tag runs the refresh waveform (~15 s, slower cold) |
| `FLASH_ERASE_SETTLE_S` | 0.6 s | the tag's flash erase after FW BEGIN |
| `CMD_TIMEOUT_S` | 6.0 s | the hub answers `SENT` only after all 4 air attempts run out (~4.5 s when the tag is asleep) |
| `CMD_RETRY_WINDOW_S` | 180.0 s | a tag sleeps ~120 s + window between polls, so a frame may wait a whole cycle before the attempt is abandoned |
| `CMD_RETRY_PAUSE_S` | 0.3 s | pause between S-line retries (hub busy / line damaged) |
| `BATCH_RETRY_S` | 0.3 s | pace failed batches: the tag's post-poll window is short, fast re-sends burn it |
| decode settle after ZEND | 0.5 s + 0.8 ms/byte | the tag decodes the compressed plane into the panel and answers nothing meanwhile |
| `ID_CONFIRM_S` | 6.0 s | wait for the tag's own confirmation line after SET_ID / WIPE |
| `DUMP_PAGE_TIMEOUT_S` | 12 s + (want/512) s | one flash page, streamed in ~26-byte frames |
| `SHOW_BEGIN_TIMEOUT_S` | 15.0 s | hub-panel BEGIN: rail cycle + init |
| `SHOW_END_TIMEOUT_S` | 120.0 s | hub-panel END blocks the hub for the whole refresh (up to a minute cold) |
| pre-arm: tries × wait | 4 × 0.6 s | a `Q` nobody answered is a push that silently misses its poll; retry, then log that the tag was NOT armed |
| pre-arm slots | 4 | `PREARM_SLOTS` in the hub's `main.c` |
| `RESET_PULSE_S` | 100 ms | how long a reset pulse holds RTS asserted |
| `RESET_ATTRIBUTION_S` | 4.0 s | window in which a boot banner is attributed to a reset pulse |
| job retries | 2 attempts, 5 s apart | a failed queued delivery re-enters the queue for the tag's next poll |
| yield window | 1.5 s | a tag counts as "listening" this long after its poll; that's when another tag's transfer may interleave |

### 1.6 Queue semantics (why everything is queued)

- A tag listens for well under 2 s after its poll, so a command fired at an
  arbitrary moment is lost. **Every outbound command waits in a queue and is
  delivered when its tag announces itself** (`app.py:1853-1856`).
- Before queueing, the host pre-arms the tag (`Q`), so the poll that triggers
  delivery says "data follows" (`app.py:1922`).
- A pre-arm is **consumed by the poll it was sent for**. Before retrying a
  failed frame/batch the host must re-arm, or every retry hammers an asleep
  tag (`app.py:1795-1799`, `1828-1829`).
- A pre-arm is lost when the hub resets (re-arm every pending tag when the
  hub's banner appears), when it was sent while the hub display owned the
  port, and for a tag's second queued command after the first started
  (`app.py:2023-2041`).
- **One transfer at a time per hub port**: push, firmware, dump, hub-panel
  redraw and a serial flash through the hub port are mutually exclusive
  (`app.py:2071-2076`, `flasher.py:260-265`). The queue holds its commands
  until the blocker clears.
- **Alternating transfers**: while a running transfer waits between batches,
  another tag that just polled may start its own and advance one batch at a
  time — the radio is half-duplex, so one interloper session is enough
  (`app.py:1721-1782`).
- Queues are **in memory**: a restart forgets pending pushes (README "Honest
  gaps").

## 2. The wireless link (context: what the host triggers)

Source: `Documentation/wireless-link.md`,
`ShelfKit-Firmware/Shared/firmware/shelflink/inc/shelflink.h`, `shelfmsg.h`.
The host implements none of this — it must understand it to reason about the
timings and the logs.

### 2.1 PHY and addressing

- 868.300 MHz, FSK, 25 kHz deviation, **100 kbit/s**, 15 dBm, sync word
  `CC AA CC AA`, **3-byte hardware-filtered address** (mask FF FF FF),
  hardware CRC-16. 868.3 MHz sits in the 1 % duty-cycle sub-band.
- Node ids: hub `0x0001`, tags `0x0100` and up; network id `0x5A` on both ends.
  0 and 1 are reserved.
- Air frame: length byte, 3-byte address (dest lo, dest hi, network id),
  payload, CRC-16 (hardware). Payload:

```
[0]     version (high nibble, =1) | type (low nibble)
[1]     flags            bit0 = retry
[2..3]  source node id, little-endian
[4]     sequence number
[5..]   data, 0..32 bytes   (SL_MAX_PAYLOAD)
```

- Types: `1 POLL`, `2 POLL_RSP`, `3 DATA`, `4 ACK` (automatic, no payload).
- **Reliability** (`shelflink.h:53-59`, `wireless-link.md` "Reliability"):
  initiator opens a **60 ms** response window; on no match it waits a random
  `5 + (0..20×attempt)` ms and retransmits (same sequence number, retry flag
  set), up to **4 transmissions**, then reports no-response. A response
  matches on source node **and** sequence number, so a late answer to an
  earlier attempt still counts. DATA is ACKed automatically (duplicates too);
  POLL is delivered every time, `dup` set on repeats, and the application
  answers it. Duplicate filter: last sequence per source, **8 sources**,
  round-robin eviction — this bounds how many tags the hub filters at once.

### 2.2 Application payloads (air side)

- **POLL** (tag→hub, 6 B): uptime s (u16 LE, wraps after 18 h), attempts the
  previous poll needed, RSSI of the hub's previous answer (i8), supply voltage
  (u16 LE, volts×256). The **15-byte extension** — only on a poll the user
  started by NFC tap — adds firmware `A.B.C`, build (u16 LE), average current
  (u16 LE µA), estimated life (u16 LE days, 0xFFFF = n/a).
- **POLL_RSP** (hub→tag, 3 B): command (`0` none / `0x01` data follows),
  RSSI the hub measured (i8), delay hint in 10 s units (0 = none; the `L`
  command's value).
- **Tag behaviour**: adaptive poll — 120 s default; 30 s while the hub has
  data, 3 min cool-down, 5 min idle, 10 min on a weak link, then hourly/daily
  with no hub in range. After a POLL_RSP with cmd=00 the tag closes its
  receiver after a ~500 ms grace; with cmd=01 it stays awake; during a
  transfer its idle timeout is 30 s. It does not poll while a transfer runs.

## 3. DATA payloads the host builds

Source: `shelfmsg.h`, `firmware.py`, `flashdump.py`, `hubhost.py:116-142`.
These are the bytes that go in `S`/`B` lines (and `D` lines, unaddressed).
All multi-byte values little-endian. Max DATA payload on the air is **32
bytes**, so every chunk size below is 32 minus its header.

### 3.1 Image (tag's panel)

```
IMG_BEGIN            [1]                                (1 B)
IMG_CHUNK            [2] plane, off16, data 1..28 B     (4+28 B)
IMG_END              [3]                                (1 B)
IMG_ZCHUNK           [11] plane, off16, data 1..28 B
IMG_ZEND             [12] plane, len16
```

- Order: BEGIN, plane 0 (black/white), plane 1 (red), END. Planes are the
  5624-byte panel frames of §5.
- Chunks must arrive **in order**: the tag's offset must equal the next
  expected one, otherwise the chunk is ignored (that's what makes repeats
  safe). Out-of-order or repeated chunks are dropped, never an error.
- END refreshes the panel **only when both planes are complete**.
- A plane may instead be sent compressed as ZCHUNKs + one ZEND, with at most
  **2560 B** per compressed plane (`SM_ECO_MAX`, the tag's buffer). The host
  falls back to plain CHUNKs when the plane doesn't compress or doesn't fit.
- `off`/`len` in ZEND are over the *compressed* file, not the plane.
- A ZEND **closes its batch**: the tag decodes the plane right after it and
  cannot acknowledge frames that would follow in the same `B` line, so a batch
  must be cut at the ZEND (`app.py:2375-2381`).
- A full uncompressed image is ~404 messages, ~20–30 s; a compressed label is
  ~30 frames.

### 3.2 Image (hub's own panel, `D` lines)

Same opcodes, same order, but chunks up to **128 bytes** and **never
compressed** (`hubhost.py:244-247`: `SER_CHUNK=128, compress=False`).

### 3.3 Firmware (OTA)

```
FW_BEGIN             [4] imglen16, crc16          (5 B)
FW_CHUNK             [5] off16, data 1..28 B      (3+28 B)
FW_END               [6]                          (1 B)
```

- `crc16` = CRC-16/CCITT (poly 0x1021, init 0xFFFF) over the whole image.
- The image must be the application's raw binary: **byte 0 must be `0x02`**
  (a reset jump — the bootloader moves bytes 0..2 into its jump vector), and
  the image must be ≤ **0xA7F0** bytes (the window 0x3000..0xD7FE in the tag's
  SPI flash, minus the bootloader's jump vector) (`firmware.py:138-154`,
  `shelfota.h:62`).
- BEGIN makes the tag erase its image sectors: give it 0.6 s before the first
  chunk. END verifies the CRC, writes the magic, resets into the bootloader,
  which programs the image and clears the header, so the update runs exactly
  once. An interrupted transfer leaves no magic; the old app boots.
- The tag confirms in its own log: `fw begin len=… crc=…`, then `fw stored,
  resetting` (or `fw crc bad …` / `fw short got=…/…`).

### 3.4 Identity, wipe, flash read-back

```
SET_ID               [7] nodeid16                 (3 B)
WIPE                 [8]                          (1 B)
FLASH_DUMP (req)     [9] addr24, len16            (6 B)
FLASH_DATA (reply)   [10] addr24, n, n bytes      (6+n B, n ≤ 26)
```

- **SET_ID**: the tag stores the id, reads it back, and *only then* moves its
  radio address — so from the next poll the hub sees the new id. The link ACK
  only proves the frame arrived; wait for the tag's log line `id set <id>` (or
  `id set FAILED, …`).
- **WIPE**: erases everything the firmware owns and resets; the tag comes back
  with a fresh random id. Confirm on the tag's log `factory reset`. The host
  must drop its own record of the old id (it will never poll again).
- **FLASH_DUMP**: request one page, at most **4096 B** (`SM_DUMP_MAX`); the
  tag replies with a stream of FLASH_DATA frames (26-byte chunks) addressed
  back to the hub, which prints them as `flash=…` log lines (§1.4). The host
  assembles by address; bytes that never arrive stay 0xFF. Requires a hub with
  `dump=1`. A default read is 0x0000..0x3000 (OTA header + identity +
  reserved sector).

### 3.5 The tag's SPI flash map

Source: `shelfota.h`, `shelfid.h`, `flashdump.py:80-117`.

| Offset | Contents |
|---|---|
| `0x00000` | OTA header, 16 B: `SKFW`, version, flags, image length (u16 LE), image CRC-16 (u16 LE), CRC-16 of bytes 0..9, zeros. Magic written last, so a torn transfer leaves no magic |
| `0x001000` | Identity record, 10 B: `SKID`, version, flags (bit 0 = assigned by server vs random), node id (u16 LE), CRC-16 of bytes 0..7. Failing CRC = absent → fresh random id |
| `0x002000` | Reserved sector, kept free on purpose |
| `0x003000` | OTA image (byte 0 = internal flash address 0), ≤ 0xA7F0 B |

Firmware images also carry a 10-byte version block found by its magic: `SKFW`
+ major, minor, patch, build (u16 LE) + a check byte making the ten bytes sum
to 0 mod 256 (`firmware.py:98-114`).

## 4. The image pipeline (producing the bytes)

Source: `png2epd.py`, `preview.py`, `eco.py`, `img_rx.c`.

### 4.1 Panel geometry and plane format

- **152 × 296** (the panel is driven rotated; only that portrait footprint is
  reachable), three colours: black `(0,0,0)`, red `(255,0,0)`, white.
- Two 1-bit planes, **5624 bytes each** (19 bytes/row × 296 rows),
  row-major, **MSB of each byte = leftmost pixel**, **0 = ink, 1 = white**.
  Plane 0 is black/white (panel command 0x10), plane 1 is red (0x13).
- Orientation: a 296×152 landscape source needs **rotate 90** (default; the
  source's left edge becomes the top of the frame); a 152×296 portrait source
  needs rotate 0. The host's fit modes: `cover`, `contain`, `stretch`.
  `stretch` is used for generated labels (service labels, app screens).

### 4.2 Colour decision (threshold dither)

Alpha is first composited over the white substrate (`png2epd.py:52-58`):

```
if   r > 110 and r > g + 40 and r > b + 40:   red
elif (3r + 6g + b) // 10 < 110:               black
else:                                         white
```

`floyd-steinberg` and `bayer4` are the other dither modes (`preview.py:32`);
Floyd–Steinberg here is the classic 7/3/5/1 error diffusion to the same
3-colour palette (`png2epd.py:74-106`).

### 4.3 ECO compression

Source: `eco.py`, `Vusion/src/eco_rx.c`, `Documentation/ECO-ESL-ImageCompression.pdf`.

- Per-plane binary chain coding (move-to-front → downward chain code → A/B/C
  item transform → VarInt bitstream). A label compresses to ~0.5–0.8 KB per
  plane vs 5624 raw; dithered photos usually don't compress.
- The host sends a plane compressed **only when it fits ≤ 2560 B and is
  actually smaller** than raw (`hubhost.py:126-134`).
- Two limits come from the tag's 8 KB of XRAM: B items look back at most
  `RING = 32` entries, and the tag rebuilds the plane in bands of
  `BAND_BYTES = 760` (40 rows) — the port must produce what `eco_rx.c`
  decodes, band for band. `eco.py` contains the matching `decode()`, so a
  round trip through it is the test.

### 4.4 Frame sequence builder

`hubhost.py:116-142` is the exact generator: yield `BEGIN`; for each plane
(BW, then RED) yield ZCHUNKs+ZEND or CHUNKs; yield `END`. A port should
replicate this one function, plus the batch-cutting rule (§3.1) and the
per-message settle waits (§1.5).

## 5. Chip flashing (wired)

Source: `flasher.py`, `ShelfKit-Firmware/Tooling/axsem-flasher.py`,
`firmware.py:38-78`.

### 5.1 Serial bootloader (mask ROM)

- **38400 8N1**. Entry: hold BOOT (DTR asserted) through a RESET pulse
  (RTS), then release BOOT (`flasher.py:437-444`).
- `?` → banner containing `Bootloader` (try 3×, draining first).
- `K` → erase; wait up to 15 s for a line containing `OK`.
- Send the Intel HEX file **line by line, 10 ms apart** — the bootloader has
  no flow control.
- `R` → run.
- **BOOT polarity is not trustworthy**: which DTR level means "released"
  depends on the adapter. The server's default is DTR released = firmware,
  and it *learns* — after its own reset pulse, if the bootloader banner
  appears within 4 s, it flips its remembered BOOT level, re-asserts it, and
  resets once more (`app.py:977-1017`). A port needs the same probe-and-learn
  loop; hard-coding the level breaks on some cables.

### 5.2 AXSDB debugger (vendor tool, Windows)

- Runs `axsdb --eraseall --flashprog <file.hex>` as a subprocess **from its
  own `bin` folder** (it reads `init.tcl` from there), 180 s timeout.
- Phase lines map to progress: `Programming Serial <no>` → connect, `Bulk
  erase…` → erase, `Flash program…` → program, `Run program…` → run; the exit
  code decides success.
- `--eraseall` wipes the **whole chip, bootloader included**: only a
  `*-full-*.hex` (bootloader + application) is safe; anything else needs an
  explicit confirmation (`flasher.py:238-242`).
- The debugger and a serial flash through another port never touch the hub,
  so they may run while the hub works; a serial flash **through the hub's
  own port** must wait for the hub to be idle.

### 5.3 Intel HEX → binary

`firmware.py:38-78`: records arrive out of order with gaps; collect by
address (supporting record types 0x00 data, 0x01 end, 0x02/0x04 extended
address, 0x03/0x05 extended segment), fill holes with **0xFF** (erased-flash
value), verify each record's checksum, reject > 64 KB. The result is checked
for the `0x02` first byte and the 0xA7F0 limit (§3.3).

## 6. Hub over IP (ESP32 bridge)

Source: `ws_bridge.py`, `wsproto.py`, `ShelfKit-Firmware/Hub-IP/src/main.cpp`.

An ESP32 sits between the hub's UART and the host, so the hub needs no cable.
The **host runs the WebSocket server**; the ESP32 dials in.

- The host listens on the URL's `host:port` (default port **8766** when
  absent). The ESP32 connects to `/?token=<token>`; if the host has
  `SHELFKIT_HUB_TOKEN` set, a mismatch gets 401. **One hub per host (v1)**: a
  second connection replaces the first.
- **Binary frames = raw UART bytes**, both directions. The ESP32 reads its
  UART in ≤256-byte chunks and sends each as one frame; the host buffers
  them and line-splits exactly as for a real port.
- **Control = short JSON text frames** from host to ESP32: `{"baud": N}`,
  `{"boot": 0|1}`, `{"reset": 0|1}`. The ESP32 treats any frame <64 bytes
  starting with `{` and ending with `}` as control and everything else as
  UART bytes (`main.cpp:256-277`). `boot`/`reset` drive the hub's pins
  (polarity handled in the ESP32's config), `baud` reconfigures the UART —
  which is how the host flashes through the bridge (reopen at 38400, flash,
  reopen at 115200; the bridge object is a per-port singleton that stays
  connected across that).
- WebSocket mechanics: RFC 6455 handshake, client→server frames masked,
  server→client unmasked, 1 MiB max frame. A port can skip a WS library by
  implementing just this (the Python reference is 135 lines, `wsproto.py`).
- The bridge detaches on "close" but keeps the WebSocket up, so a reopen
  re-attaches to the same ESP32 and re-asserts the baud and the last
  boot/reset levels.

## 7. The tag's debug UART (second port)

Source: tag `main.c`, `img_rx.c`, `fw_rx.c`, `shelf_id.c`, `nfc_cfg.c`;
`serial_link.py`, `app.py` (pane model).

- **115200 8N1, log only** — the tag's firmware has no command channel. Its
  value: (a) it proves what a link-layer ACK cannot, and (b) its BOOT/RESET
  pins make it a second flashing path.
- Lines the host acts on (`app.py:40-41`, tag sources):

| Line | Proves |
|---|---|
| `id set <hex>` / `id set FAILED, …` | SET_ID was actually stored (wait up to 6 s) |
| `factory reset` | WIPE ran |
| `fw begin len=… crc=…` … `fw stored, resetting` (or `fw crc bad`, `fw short`, `fw erase/write FAILED, flash busy`) | OTA transfer outcome |
| `img begin` / `img refresh` / `img done busy=N N.Ns` | the panel really ran a refresh: `busy=1` real waveform, `busy=0` IL0373 never pulled BUSY (check PA5/PB5), `busy=2` waveform still running at timeout |
| `img zplane <n>B <s>s` | compressed-plane decode time; tune the decode settle from it |
| `poll ok att=N rssi=… [hubrssi=…]` / `poll FAILED after N attempts` | link health |
| `*** ShelfKit tag, node … net 5a dump=1 fw=…` | boot banner (reset attribution, BOOT-polarity learning) |

- **Reset**: assert RTS for 100 ms, release on a timer (never leave it
  held), and first put DTR at the learned "firmware" level — otherwise the
  reset can come up in the bootloader and the log looks like "it keeps
  resetting" (`app.py:941-975`).
- The tag also logs NFC activity (`nfc ndef …`, `nfc fd …`, `wake`) and
  periodic `sleep` lines. Battery on the host comes from the hub's `vdd=`
  field, not from this port; the host maps volts → % on a CR2450 curve
  (3.0 V = 100 % … 2.3 V = 0 %, linear between, 620 mAh for µA estimates —
  `app.py:68-85`).

## 8. Work breakdown for the port

Units of work, in dependency order. "Source" is the reference implementation
to diff against; the acceptance check is what "done" means.

| # | Unit | Source | Acceptance check |
|---|---|---|---|
| 1 | Port abstraction: open/close, line reader, DTR/RTS, one-port-per-board | `serial_link.py` | boot banner appears after open; closing never leaves RTS held |
| 2 | Hub line protocol: encode/decode P/V/Q/L/S/B/D + checksum, all `ERR` reasons | `hubhost.py:100-155`, hub `main.c:165-437` | a scripted fake-hub round-trip of every command and error |
| 3 | Hub log parser → structured events (POLL/RSP/DATA-flash/banners/SENT/BAT/ERR) | `app.py:57-90,615-680`, `flashdump.py:26-52` | parse fixtures captured from a real hub (see `Documentation/screenshots`) without dropping a field |
| 4 | Pre-arm + queue state machine: arm, consume, re-arm on retry, re-arm after hub reset, slot=4, refuse push after cmd=00 | `app.py:1853-2172` | the push-landed rate against a real tag; `ERR full` surfaced, not swallowed |
| 5 | Frame builders: image (raw+ECO), FW, SET_ID, WIPE, FLASH_DUMP | `hubhost.py:116-142`, `firmware.py:157-166`, `flashdump.py:29-34` | byte-identical output vs the Python functions on the same inputs |
| 6 | Image pipeline: rotate/fit/threshold/Floyd–Steinberg/Bayer → 2×5624 B planes | `png2epd.py`, `preview.py` | byte-identical planes vs `to_planes()` for a set of sample images (in `Resources/`) |
| 7 | ECO compress/decode | `eco.py` | `decode(compress(plane)) == plane` round trip, and ≤2560 B output when it compresses |
| 8 | Transfer runner: settle waits, S/B retries with re-arm, batch cut at ZEND, alternating transfers, cancellation | `app.py:2228-2381`, `hubhost.py:201-262` | one real image push to a real tag; progress = frames/total; cancel stops after the current frame |
| 9 | OTA runner: HEX→binary, `02`/size checks, FW frames with 0.6 s erase settle | `firmware.py`, `app.py:1213-1255` | tag boots the new version (its next poll banner says so) |
| 10 | Flash read-back: page requests, `flash=` assembly, 0xFF gaps, SKFW/SKID describe | `flashdump.py`, `app.py:1379-1465` | read 0x0000..0x3000 from a known tag; identity/OTA header parse matches `flashdump.describe` |
| 11 | ID assignment + wipe with log confirmation; rename records on success | `app.py:1076-1144,1506-1539` | old id stops polling, new id polls and keeps its identity across a reset |
| 12 | Chip flasher: bootloader sequence + BOOT-polarity learning; AXSDB subprocess (or document it as out of scope on non-Windows) | `flasher.py`, `app.py:977-1017` | flash a `*-full-*.hex` through the tag's port; board comes up in firmware, not bootloader |
| 13 | Hub-over-IP bridge: WS server, token, binary pump, `{"baud"}/{"boot"}/{"reset"}` | `ws_bridge.py`, `wsproto.py` | a hub on Wi-Fi drives end-to-end, including a 38400 flash through the bridge |
| 14 | Tag log observer: second pane, reset pulse, banner attribution | `serial_link.py`, `app.py:612-680,941-1017` | reset pulse → banner attributed within 4 s; wrong BOOT level self-corrects |
| 15 | Concurrency rules: single writer to the hub port; busy flags (transfer/dump/display/flash) gate everything; queue holds while busy | `app.py:2071-2076`, `flasher.py:253-265`, `hub_display.py` | no interleaved writes under concurrent actions; a hub-panel redraw blocks for its ~15 s |

## 9. Hazards and open gaps

Things that will bite a reimplementation even when every byte is right:

- **The hub goes blind while it prints.** A `POLL` line takes ~18 ms during
  which RX bytes are lost; a command sent in reaction to that line can be
  lost whole. Checksum + retry is the only cover (`wireless-link.md`).
- **PB5 is two things**: the hub's UART RX *and* the hub panel's reset. Any
  serial byte during a panel refresh corrupts the refresh; the host must
  stay silent through `D`-line END (`hubhost.py:25-26`).
- **One command in flight**, one batch at a time: the hub's RX register holds
  one byte. No pipelining.
- **Pre-arm slots = 4, dup-filter sources = 8.** Beyond that, the hub
  answers `ERR full` / stops filtering; a large fleet needs the port to cope.
- **Everything outbound is in memory**: a restart drops pending commands.
- **Duty cycle**: 868.3 MHz is a 1 % sub-band; retries × many tags must be
  budgeted (`wireless-link.md` "Open questions").
- **The 38400 bootloader on a 115200 port** prints garbage boot lines; the
  port parser must not trip on them.
- **AXSDB is a Windows binary**; on other hosts only the serial bootloader
  path exists.
- **NFC link**: the tag rewrites its NFC chip's URL at boot and (as of this
  writing) doesn't yet fill in its own id — one shared URL for every tag
  until the firmware lands (README §3). The host's only NFC duty is serving
  the tag pages that URL points at.
- **Unmeasured**: long-run packet loss, range, hub RX→TX→RX turnaround vs the
  tag's 60 ms window (`wireless-link.md`).
