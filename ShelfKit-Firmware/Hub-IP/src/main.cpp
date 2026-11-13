// ShelfKit Hub-IP: the hub's UART, carried over a WebSocket to ShelfKit-Server.
//
// This is the ESP32 end of the bridge. It dials the host, pumps raw UART bytes
// both ways as binary frames, and turns short JSON text frames into the hub's
// BOOT/RESET control and a UART baud change (so the host can flash over IP).
// See include/config.h for the wiring and the polarity to bench-verify.
#include <Arduino.h>
#include <WiFi.h>
#include <WebSockets.h>
#include "config.h"

static WebSocketsClient ws;
static String wsUrl;
static uint32_t lastWifi = 0;
static uint32_t lastWs = 0;

// Control frames from the host are short JSON objects; the hub's UART bytes are
// not. That keeps text control and binary data apart without a type channel.
static bool looksLikeControl(const uint8_t* p, size_t n) {
  return n > 0 && n < 64 && p[0] == '{' && p[n - 1] == '}';
}

// The integer following "key" in a JSON-ish string, or -1 when the key is absent.
static long jsonNum(const String& s, const char* key) {
  String needle = String('"') + key + '"';
  int at = s.indexOf(needle);
  if (at < 0) return -1;
  at = s.indexOf(':', at + needle.length());
  if (at < 0) return -1;
  return s.substring(at + 1).toInt();
}

static void applyControl(const String& s) {
  long baud = jsonNum(s, "baud");
  if (baud > 0) Serial.begin(baud, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  long boot = jsonNum(s, "boot");
  if (boot >= 0) digitalWrite(BOOT_PIN, boot ? BOOT_ACTIVE_LEVEL : BOOT_IDLE_LEVEL);
  long reset = jsonNum(s, "reset");
  if (reset >= 0) digitalWrite(RESET_PIN, reset ? RESET_ACTIVE_LEVEL : RESET_IDLE_LEVEL);
}

void onEvent(WebSocketsEvent event, uint8_t* payload, size_t length) {
  if (event != WS_EVT_DATA) return;
  if (looksLikeControl(payload, length)) {
    applyControl(String((const char*)payload, length));
  } else if (length) {
    Serial.write(payload, length);
  }
}

void setup() {
  // Re-point the default Serial at the hub's UART (crossed: our TX -> its RX).
  Serial.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  pinMode(BOOT_PIN, OUTPUT);
  pinMode(RESET_PIN, OUTPUT);
  digitalWrite(BOOT_PIN, BOOT_IDLE_LEVEL);   // hub runs, not in the bootloader
  digitalWrite(RESET_PIN, RESET_IDLE_LEVEL);  // hub not held in reset

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wsUrl = String("ws://") + HOST_ADDR + ':' + HOST_PORT + "/?token=" + HUB_TOKEN;
  ws.onEvent(onEvent);
}

void loop() {
  uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED && now - lastWifi > 3000) {
    lastWifi = now;
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
  if (WiFi.status() == WL_CONNECTED && !ws.connected() && now - lastWs > 2000) {
    lastWs = now;
    ws.begin(wsUrl);
  }
  ws.loop();

  // Hub -> host: forward whatever the UART has as one binary frame.
  if (ws.connected() && Serial.available()) {
    uint8_t buf[256];
    int avail = Serial.available();
    int n = avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail;
    int got = Serial.readBytes(buf, n);
    if (got > 0) ws.sendBinary(buf, got);
  }
  delay(1);
}
