#pragma once
/* ShelfKit Hub-IP configuration.
 *
 * The Wi-Fi / host / token values below are FIRST-BOOT DEFAULTS only: they seed
 * the device's stored config once. After that, edit everything at runtime on the
 * built-in web page (http://<esp-ip>/, or the "ShelfKit-<role>-<port>" config AP
 * if Wi-Fi isn't up yet): role (hub/device), SSID + password, DHCP or a static
 * IP, the WS host + port, and the token. Re-flash only to change the pin map or
 * polarity below. */

/* ── Wi-Fi (first-boot default) ───────────────────────────────────────────── */
const char* WIFI_SSID = "YOUR-SSID";
const char* WIFI_PASS = "YOUR-PASSWORD";

/* ── the ShelfKit host (first-boot default) ───────────────────────────────── */
/* The host listens on HOST_PORT; point SHELFKIT_HUB_TOKEN at HUB_TOKEN.       */
const char* HOST_ADDR = "192.168.1.100";
const int   HOST_PORT = 8766;
const char* HUB_TOKEN = "change-me-to-match-SHELFKIT_HUB_TOKEN";

/* ── hub wiring (ESP32 GPIO -> hub pin) ────────────────────────────────────
 * The hub's serial is 3.3 V logic, so no level shifter is needed.
 *   ESP32 TX -> hub RX (PB5)      ESP32 RX <- hub TX (PB4)
 * Re-map the GPIOs to whatever free pins you wired; keep TX/RX crossed.     */
#ifndef UART_TX_PIN
#define UART_TX_PIN 17
#endif   /* ESP32 TX -> hub RX */
#ifndef UART_RX_PIN
#define UART_RX_PIN 16
#endif   /* ESP32 RX <- hub TX */
#ifndef BOOT_PIN
#define BOOT_PIN 5
#endif   /* ESP32 GPIO -> hub BOOT (PB3) */
#ifndef RESET_PIN
#define RESET_PIN 18
#endif   /* ESP32 GPIO -> hub RESET (nRST) */

/* ── control-line polarity (BENCH-VERIFY these two against your board) ────
 * The flasher drives DTR=BOOT and RTS=RESET. The hub's mask-ROM bootloader
 * is entered with BOOT held active through a RESET pulse. "active level" is
 * the logic level that DOES the thing; idle is the resting level.
 * Defaults below match an active-low AX8052-style hub (BOOT low = bootloader,
 * nRST low = reset). If your board resets the other way, flip the one level.  */
const int BOOT_IDLE_LEVEL    = 1;   /* resting: not in the bootloader */
const int BOOT_ACTIVE_LEVEL  = 0;   /* held while resetting -> bootloader */
const int RESET_IDLE_LEVEL   = 1;   /* resting: not resetting */
const int RESET_ACTIVE_LEVEL = 0;   /* pulsed -> reset */
