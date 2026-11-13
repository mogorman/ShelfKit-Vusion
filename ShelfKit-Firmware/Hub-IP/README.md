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

1. `include/config.h`: set `WIFI_SSID`, `WIFI_PASS`, `HOST_ADDR` (the ShelfKit
   host's LAN IP), and `HUB_TOKEN`.
2. On the host, set the **same** token: `export SHELFKIT_HUB_TOKEN=...`.
   (Leave it unset on both sides to run open, e.g. on a trusted dev box.)
3. In the ShelfKit web UI, set the hub's port to the host's WS URL, e.g.
   `ws://<host-lan-ip>:8766`. The host listens there and the ESP32 dials in.

## Flash

`pio run -t upload` (or the PlatformIO IDE) builds and flashes the ESP32.

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
