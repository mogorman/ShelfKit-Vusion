# Hub-IP

Run a ShelfKit hub over Wi-Fi instead of a USB-serial cable. This ESP32 sits
between the hub and the network: it pumps the hub's UART to the ShelfKit host as
a WebSocket, and turns a couple of control messages into the hub's BOOT/RESET
pins so the host can **flash the hub over IP** too.

The host side is the ShelfKit server itself: it runs the WebSocket and presents
the hub as an ordinary (ws://) serial pane. No new Python dependency.

## Wiring

The hub's serial is 3.3 V, so wire directly — no level shifter.

| ESP32 pin | hub pin | note |
|-----------|---------|------|
| GPIO17 (TX) | RX (PB5) | crossed |
| GPIO16 (RX) | TX (PB4) | crossed |
| GPIO5   | BOOT (PB3) | held active through a reset to enter the mask-ROM bootloader |
| GPIO18  | RESET (nRST) | pulsed to reset |
| 3V3     | 3V3     | |
| GND     | GND     | |

The GPIO numbers are in `include/config.h` (`UART_TX_PIN`, `UART_RX_PIN`,
`BOOT_PIN`, `RESET_PIN`) — re-map them to whatever you wired.

## Configure

All settings live on a built-in web page — no re-flash to change them.

1. On **first boot** (or after *Factory reset*) the module goes straight into
   **config mode**: it starts an access point named `ShelfKit-<hub|device>-<port>`
   (open, no password). Join it and open `http://192.168.4.1/`.
2. Set: **role** (hub or device), **SSID** + **password**, **DHCP or static** IP,
   the **WS host** + **port**, and the **token**. *Save + reboot*. It now connects
   to your Wi-Fi, and the page is reachable at its LAN IP (DHCP) or the static IP
   you set — the config AP only reappears later if Wi-Fi is ever unreachable.
3. On the host, set the **same** token: `export SHELFKIT_HUB_TOKEN=...`
   (leave it empty on both sides to run open, e.g. on a trusted dev box).
4. In the ShelfKit web UI, set the hub's port to the host's WS URL, e.g.
   `ws://<host-lan-ip>:8766`. The host listens there and the ESP32 dials in.

`include/config.h` only holds the *first-boot* defaults (Wi-Fi/host/token) plus
the board's pin map and control-line polarity. Once it has booted once, the
stored config wins — the page is the source of truth.

## Flash

`pio run -t upload` (or the PlatformIO IDE) builds and flashes the ESP32.

### ESP32-S2 Mini (WEMOS)

Build the S2 Mini variant instead:

```
pio run -e esp32-s2-mini -t upload
```

It reuses the same source and protocol — only the pins differ, so the host-side
config (token, `ws://` port) is unchanged. Wire the hub to these S2 pins (kept off
the USB/strapping pins 0, 19, 20, 45, 46):

| S2 pin  | hub pin | note |
|---------|---------|------|
| GPIO17 (TX) | RX (PB5) | crossed |
| GPIO16 (RX) | TX (PB4) | crossed |
| GPIO35  | BOOT (PB3) | |
| GPIO36  | RESET (nRST) | |
| 3V3     | 3V3     | |
| GND     | GND     | |

The pin map is the `[env:esp32-s2-mini]` `build_flags` in `platformio.ini`, which
overrides the same `UART_TX_PIN`/`UART_RX_PIN`/`BOOT_PIN`/`RESET_PIN` in
`config.h`.

After that, everything in the server's UI works as it does on a cable: the hub
pane, image pushes, and the serial bootloader **Flash** action. Flashing is the
only place the control pins and the baud switch are used:

- the server closes the hub pane and tells the ESP32 `{"baud":38400}`,
- holds BOOT and pulses RESET (the DTR/RTS of the flash recipe),
- drives the Intel-hex image over the WebSocket, then `{"baud":115200}` on the way back.

## Running the tag too (both wireless)

The tag debug port rides the exact same bridge. A second ESP32 wired to the tag's
UART dials a *different* host port, and both run at once as independent bridges:

- Flash a second copy of this project with `HOST_PORT 8767` and the tag's wiring
  (its `BOOT` is PB3, active-low; `RESET` is its nRST — see `../Vusion/README.md`).
- On the host, set the tag port to `ws://0.0.0.0:8767` (Settings → tag port, or
  `tag=ws://0.0.0.0:8767` on the command line).
- Same `HUB_TOKEN` on both ESP32s and `SHELFKIT_HUB_TOKEN` on the host.

The hub and tag must use *different* host ports; the same URL is one shared bridge.

## Serial console (debugging)

A small command console on `Serial` mirrors the web page — on the S2 Mini that's
the USB port (CDC on boot), on a classic ESP32 it's UART0. Open a serial monitor
at 115200; it prints the settings and status at boot, then:

| command  | does (same as the page / flasher)                       |
|----------|----------------------------------------------------------|
| `status` | wifi / ws / ip                                            |
| `config` | the current settings                                      |
| `reset`  | reboot the device, coming up in **firmware**              |
| `boot`   | hold BOOT through a reset → the hub's **serial bootloader** |
| `reboot` | reboot this module                                        |
| `factory`| wipe settings, reboot to config mode                      |

`boot` and `reset` are the same control-line triggers the host's flasher drives
over the WebSocket (`{"boot"}`/`{"reset"}`), so the whole flash flow still works
over IP; these just let you fire them by hand while debugging.

## Bench-verify

`BOOT_ACTIVE_LEVEL` / `RESET_ACTIVE_LEVEL` in `config.h` are the two values to
confirm against your board (the flash recipe in `Vusion/README.md` is the
reference). If the hub won't enter the bootloader, or resets at the wrong
moment, flip one of those levels — nothing else changes.

## v1 limits

- One hub (ESP32) per host.
- If the ESP32's link drops, host→hub writes are dropped until it reconnects
  (the hub→host direction is unaffected); the link auto-reconnects.
- The browser UI still lives on the host (127.0.0.1:8765); only the hub's UART
  rides the network.
