// ShelfKit Hub-IP: the hub's UART, carried over a WebSocket to ShelfKit-Server.
//
// The ESP32 end of the bridge. It dials the host, pumps raw UART bytes both ways
// as binary frames, and turns short JSON text frames into the hub's BOOT/RESET
// control and a UART baud change (so the host can flash over IP).
//
// Configuration (role, Wi-Fi, static IP, WS endpoint, token) is set at runtime
// on the built-in web page:  http://<esp-ip>/   (or the config AP if Wi-Fi
// isn't reachable yet). config.h only holds the first-boot defaults and the
// board's pin map / control-line polarity.
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include "config.h"

// ── stored configuration (NVS) ───────────────────────────────────────────────
struct Cfg {
  uint8_t  role;        // 0 = hub, 1 = device
  uint8_t  use_static;  // 1 = apply the ip/gw/subnet below
  char     ssid[33];
  char     pass[65];
  char     ip[16];
  char     gw[16];
  char     subnet[16];
  char     host[16];
  uint16_t host_port;
  char     token[65];
  uint32_t magic;
};
static const uint32_t CFG_MAGIC = 0x53484B31;   // 'SHK1'
static Cfg cfg;
static Preferences prefs;

static WebSocketsClient ws;
static WebServer server(80);
static HardwareSerial hubUart(2);   // dedicated UART for the hub (works on USB-CDC boards too)
static bool wsConnected = false;
static bool apMode = false;
static uint32_t lastWifi = 0, lastWs = 0, lastAp = 0;

// ── persist / load ───────────────────────────────────────────────────────────
static void saveCfg() { prefs.putBytes("c", &cfg, sizeof cfg); }

static void loadCfg() {
  prefs.begin("shk", false);
  memset(&cfg, 0, sizeof cfg);
  if (prefs.getBytes("c", &cfg, sizeof cfg) == sizeof cfg && cfg.magic == CFG_MAGIC) {
    return;   // prefs stay open for saveCfg()
  }
  // first boot: seed from the compile-time defaults in config.h
  cfg.role       = 0;
  cfg.use_static = 0;
  snprintf(cfg.ssid,   sizeof cfg.ssid,   "%s", WIFI_SSID);
  snprintf(cfg.pass,   sizeof cfg.pass,   "%s", WIFI_PASS);
  snprintf(cfg.ip,     sizeof cfg.ip,     "%s", "192.168.1.50");
  snprintf(cfg.gw,     sizeof cfg.gw,     "%s", "192.168.1.1");
  snprintf(cfg.subnet, sizeof cfg.subnet, "%s", "255.255.255.0");
  snprintf(cfg.host,   sizeof cfg.host,   "%s", HOST_ADDR);
  cfg.host_port        = HOST_PORT;
  snprintf(cfg.token,  sizeof cfg.token,  "%s", HUB_TOKEN);
  cfg.magic = CFG_MAGIC;
  saveCfg();
}

// ── network ──────────────────────────────────────────────────────────────────
// Parse "a.b.c.d" into an IPAddress (ESP32's IPAddress has no string ctor).
static bool ipFrom(const char* s, IPAddress& out) {
  uint8_t a = 0, b = 0, c = 0, d = 0;
  if (sscanf(s, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) return false;
  out = IPAddress(a, b, c, d);
  return true;
}

static void connectWifi() {
  if (cfg.use_static && cfg.ip[0]) {
    IPAddress ip, gw, sn;
    if (ipFrom(cfg.ip, ip) && ipFrom(cfg.gw, gw) && ipFrom(cfg.subnet, sn))
      WiFi.config(ip, gw, sn);
  }
  WiFi.begin(cfg.ssid, cfg.pass);
}

static void startAp() {
  String name = String("ShelfKit-") + (cfg.role ? "device" : "hub") + String(cfg.host_port);
  WiFi.mode(WIFI_AP_STA);                  // keep the STA side trying to connect
  WiFi.softAP(name.c_str());
  apMode = true;
}

// ── config web page ──────────────────────────────────────────────────────────
static String page() {
  String h;
  h.reserve(1000);
  h += String("<html><head><meta charset=utf-8><title>ShelfKit ") + (cfg.role ? "device" : "hub") +
       "</title></head><body>";
  h += String("<h2>ShelfKit ") + (cfg.role ? "device" : "hub") + "</h2>";
  h += "<form method=post action=/config>";
  h += String("Role: <select name=role><option value=0") + (cfg.role == 0 ? " selected" : "") +
       ">hub</option><option value=1" + (cfg.role == 1 ? " selected" : "") + ">device</option></select><br>";
  h += "SSID: <input name=ssid size=24 value=\"" + String(cfg.ssid) + "\">";
  h += " Password: <input name=pass type=password size=24 value=\"" + String(cfg.pass) + "\"><br>";
  h += String("IP: <select name=net><option value=0") + (cfg.use_static == 0 ? " selected" : "") +
       ">DHCP</option><option value=1" + (cfg.use_static == 1 ? " selected" : "") + ">static</option></select>";
  h += " IP <input name=ip size=15 value=\"" + String(cfg.ip) + "\">";
  h += " GW <input name=gw size=15 value=\"" + String(cfg.gw) + "\">";
  h += " Mask <input name=subnet size=15 value=\"" + String(cfg.subnet) + "\"><br>";
  h += "WS host: <input name=host size=16 value=\"" + String(cfg.host) + "\">";
  h += " Port: <input name=port size=6 value=" + String(cfg.host_port) + "><br>";
  h += "Token: <input name=token size=40 value=\"" + String(cfg.token) + "\"><br>";
  bool onAp = apMode && WiFi.status() != WL_CONNECTED;
  IPAddress shown = onAp ? WiFi.softAPIP() : WiFi.localIP();
  h += "<p>WiFi " + String(WiFi.status() == WL_CONNECTED ? "up" : "down") +
       " &middot; WS " + String(wsConnected ? "up" : "down") +
       " &middot; " + (onAp ? "config AP: " : "IP: ") +
       String(shown) + "</p>";
  h += "<p><input type=submit value=Save + reboot></form>";
  h += "</body></html>";
  return h;
}

static void handleConfig() {
  cfg.role       = server.arg("role") == "1" ? 1 : 0;
  cfg.use_static = server.arg("net") == "1" ? 1 : 0;
  snprintf(cfg.ssid,   sizeof cfg.ssid,   "%s", server.arg("ssid").c_str());
  snprintf(cfg.pass,   sizeof cfg.pass,   "%s", server.arg("pass").c_str());
  snprintf(cfg.ip,     sizeof cfg.ip,     "%s", server.arg("ip").c_str());
  snprintf(cfg.gw,     sizeof cfg.gw,     "%s", server.arg("gw").c_str());
  snprintf(cfg.subnet, sizeof cfg.subnet, "%s", server.arg("subnet").c_str());
  snprintf(cfg.host,   sizeof cfg.host,   "%s", server.arg("host").c_str());
  snprintf(cfg.token,  sizeof cfg.token,  "%s", server.arg("token").c_str());
  long port = server.arg("port").toInt();
  cfg.host_port = (uint16_t)(port < 1 ? 1 : port > 65535 ? 65535 : port);
  cfg.magic = CFG_MAGIC;
  saveCfg();
  server.send(200, "text/plain", "Saved. Rebooting...");
  delay(300);
  ESP.restart();
}

// ── the bridge ───────────────────────────────────────────────────────────────
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
  if (baud > 0) hubUart.begin(baud, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  long boot = jsonNum(s, "boot");
  if (boot >= 0) digitalWrite(BOOT_PIN, boot ? BOOT_ACTIVE_LEVEL : BOOT_IDLE_LEVEL);
  long reset = jsonNum(s, "reset");
  if (reset >= 0) digitalWrite(RESET_PIN, reset ? RESET_ACTIVE_LEVEL : RESET_IDLE_LEVEL);
}

static void onEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      wsConnected = true;
      break;
    case WStype_DISCONNECTED:
    case WStype_ERROR:
      wsConnected = false;
      break;
    case WStype_TEXT:
    case WStype_BIN:
      if (looksLikeControl(payload, length)) {
        applyControl(String((const char*)payload, length));
      } else if (length) {
        hubUart.write(payload, length);
      }
      break;
    default:
      break;   // fragmented frames are dropped (the host sends complete frames)
  }
}

void setup() {
  // Start the dedicated hub UART (crossed: our TX -> its RX).
  hubUart.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  pinMode(BOOT_PIN, OUTPUT);
  pinMode(RESET_PIN, OUTPUT);
  digitalWrite(BOOT_PIN, BOOT_IDLE_LEVEL);   // hub runs, not in the bootloader
  digitalWrite(RESET_PIN, RESET_IDLE_LEVEL);  // hub not held in reset

  loadCfg();
  ws.onEvent(onEvent);

  WiFi.mode(WIFI_STA);
  connectWifi();

  server.on("/", HTTP_GET, []() { server.send(200, "text/html", page()); });
  server.on("/config", HTTP_POST, handleConfig);
  server.begin();
}

void loop() {
  uint32_t now = millis();
  server.handleClient();

  if (now - lastWifi > 3000) {
    lastWifi = now;
    if (WiFi.status() != WL_CONNECTED) {
      if (!apMode && now - lastAp > 8000) {
        lastAp = now;
        startAp();                        // no Wi-Fi yet: offer a config portal
      }
      connectWifi();                      // keep the STA side trying to connect
    }
  }
  if (WiFi.status() == WL_CONNECTED && !wsConnected && now - lastWs > 2000) {
    lastWs = now;
    ws.begin(String(cfg.host), cfg.host_port, String("/?token=") + cfg.token);
  }
  ws.loop();

  // Hub -> host: forward whatever the UART has as one binary frame.
  if (wsConnected && hubUart.available()) {
    uint8_t buf[256];
    int avail = hubUart.available();
    int n = avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail;
    int got = hubUart.readBytes(buf, n);
    if (got > 0) ws.sendBIN(buf, got);
  }
  delay(1);
}
