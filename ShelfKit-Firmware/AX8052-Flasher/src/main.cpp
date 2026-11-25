#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ax_link.h"
#include "ax_pins.h"

static uint8_t g_key[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static int parse_u32(const char* s, uint32_t* out) {
  char* end = 0;
  unsigned long v;
  if (s == 0 || *s == 0) return 0;
  v = strtoul(s, &end, 16);
  if (end == s || *end != 0) return 0;
  *out = (uint32_t)v;
  return 1;
}

static void print_help(void) {
  Serial.println("AX8052 Flasher, ESP32-S3");
  Serial.println("pins");
  Serial.println("key <16 hex>       unlock key, LSB first. Default FFFFFFFFFFFFFFFF");
  Serial.println("setkey <16 hex>    write this key into the chip");
  Serial.println("unlock             one key, reports status bit 0x20");
  Serial.println("read <addr> <len>  read code, 128 bytes per chunk");
  Serial.println("write <addr> <hex> program words, even address, max 128 bytes, below FC00");
  Serial.println("erase              opcode 0x54 once, then poll for 0x01");
  Serial.println("pc                 read the program counter");
  Serial.println("reset              CPU reset, stays halted");
  Serial.println("step               one instruction, then PC");
  Serial.println("steps <n>          reset, then n steps, hex 1..10");
  Serial.println("status             same as unlock");
  Serial.println("dump <addr> <len>  same as read");
  Serial.println("loop               DATA_IN follows DATA_OUT: high, low, then OE released");
  Serial.println("bits               96 clocks after status, buffer driving");
  Serial.println("bitsod             same, DATA_OUT high-Z");
  Serial.println("wire5 / wire6      one data wire, open drain");
  Serial.println("trace              CLK, DBG_EN, RESET_N, DATA during status");
  Serial.println("sense              read CLK, DATA, DBG_EN, RESET_N only");
  Serial.println("scan               read header pins only");
  Serial.println("uart               enter the bootloader, relay the console to UART0 (Q to exit)");
  Serial.println("bridge             transparent console<->UART0 @115200 (hub app), until reboot");
  Serial.println("Do not power the AX8052 from 5 V.");
}

static void print_pins(void) {
  Serial.printf("CLK %d  DATA_OUT %d  DATA_IN %d  OE %d (active low)  RESET_N %d  DBG_EN %d\n",
                kPinClk, kPinDataOut, kPinDataIn, kPinOe, kPinResetN, kPinDbgEn);
}

static void print_bits(const uint8_t* bits, int n);

static int run_connect(ConnectSession* session) {
  ax_enter_debug();
  while (!session->ready() && !session->stopped()) {
    RunOps ran = ax_run(session->pending());
    if (!ran.ok) {
      Serial.printf("link: %s\n", ran.error ? ran.error : "fail");
      Serial.printf("replies %u\n", (unsigned)ran.replies.size());
      Serial.printf("sample bits %u\n", (unsigned)ran.sample_bits.size());
      if (!ran.sample_bits.empty()) print_bits(&ran.sample_bits[0], (int)ran.sample_bits.size());
      return 0;
    }
    session->on_replies(ran.replies);
  }
  if (session->stopped()) {
    Serial.printf("stop: %s\n", session->reason() ? session->reason() : "stopped");
    return 0;
  }
  return 1;
}

static void print_unlock(const ConnectSession& session) {
  Serial.printf("status %02X unlocked %d halt %d\n", session.last_status(),
                (session.last_status() & kStatusUnlocked) ? 1 : 0,
                (session.last_status() & kStatusHalt) ? 1 : 0);
}

// One caller key. Bit 0x20 clear refuses the access and does not claim success.
static int connect_for_access(ConnectSession* session, const char* what) {
  int ok = run_connect(session);
  if (session->have_status()) print_unlock(*session);
  if (!ok || (session->last_status() & kStatusUnlocked) == 0) {
    if (session->have_status() && (session->last_status() & kStatusUnlocked) == 0) {
      Serial.printf("locked: %s refused\n", what);
    }
    ax_leave_debug();
    return 0;
  }
  return 1;
}

static void print_bits(const uint8_t* bits, int n) {
  int i;
  int zeros = 0;
  for (i = 0; i < n; i++) {
    if (bits[i] == 0) zeros++;
    Serial.print(bits[i] ? '1' : '0');
  }
  Serial.println();
  Serial.printf("zeros %d of %d\n", zeros, n);
}

static void cmd_loop(void) {
  uint8_t level[3];
  ax_loopback(level);
  Serial.printf("loop high %u low %u release %u\n", level[0], level[1], level[2]);
}

static void cmd_sense(void) {
  AxSense sense;
  const char* name[4] = {"CLK", "DATA", "DBG_EN", "RESET_N"};
  int i;
  ax_sense_pins(&sense);
  for (i = 0; i < 4; i++) {
    Serial.printf("sense %s pd %u pu %u mv %d..%d\n", name[i], sense.pulldown[i], sense.pullup[i],
                  sense.mv_min[i], sense.mv_max[i]);
  }
  Serial.printf("sense DBG data pd %u pu %u\n", sense.data_dbg[0], sense.data_dbg[1]);
  Serial.printf("sense clock lows %d of 64\n", sense.clock_lows);
  Serial.printf("sense CLK drive high %u low %u\n", sense.clk_drive_high, sense.clk_drive_low);
}

static void cmd_scan(void) {
  AxScanPin pins[kScanPinCount];
  int i;
  ax_scan_pins(pins);
  for (i = 0; i < kScanPinCount; i++) {
    Serial.printf("scan %d pd %u pu %u mv %d..%d\n", pins[i].pin, pins[i].pulldown, pins[i].pullup,
                  pins[i].mv_min, pins[i].mv_max);
  }
}

// Enter the AX8052 bootloader: hold BOOT (PB3) LOW over a reset pulse and KEEP
// IT LOW afterwards. The bootloader runs first either way, but it only jumps to
// a valid app when BOOT reads HIGH in its main loop - so releasing BOOT high let
// a flashed app steal the reset and the banner never appeared.
static void ax_uart_boot_dance(void) {
  pinMode(kPinBoot, OUTPUT);
  pinMode(kPinResetN, OUTPUT);
  digitalWrite(kPinResetN, 1);  // reset released (idle)
  delay(50);
  digitalWrite(kPinBoot, 0);    // BOOT low -> force the bootloader
  delay(20);
  digitalWrite(kPinResetN, 0);  // assert reset
  delay(20);
  digitalWrite(kPinResetN, 1);  // release -> chip samples BOOT=low -> bootloader
  delay(20);
  digitalWrite(kPinBoot, 0);    // keep BOOT low -> stay in the bootloader
  pinMode(kPinBoot, INPUT);
  pinMode(kPinResetN, INPUT);
}

// Take the debug drive lines off the bus while a UART relay runs. The AX8052
// debug bus is separate from the PB4/PB5 UART, but a driven debug line (CLK low,
// DATA high) or a loaded DBG_EN holds the chip out of its normal boot. Leaving
// every one of them high-Z is electrically the same as unplugging the debug
// wires; restore the normal idle state with ax_pins_begin() when done.
static void ax_release_debug_bus(void) {
  pinMode(kPinClk, INPUT);
  pinMode(kPinDataOut, INPUT);
  pinMode(kPinDataIn, INPUT);
  pinMode(kPinOe, OUTPUT);
  digitalWrite(kPinOe, HIGH);  // buffer off (if fitted); a dangle if not
  pinMode(kPinDbgEn, INPUT);
}

// Relay the PC console to the AX8052 bootloader over UART1 (PB4 TX / PB5 RX).
// The PC drives the bootloader protocol (? / K / hex / R) through this relay; a
// lone 'Q' on the PC side closes it and returns to the flasher console.
static void cmd_uart(void) {
  ax_release_debug_bus();
  Serial.println("uart: entering bootloader (BOOT low over reset), relaying UART0 at 38400");
  ax_uart_boot_dance();
  Serial1.begin(38400, SERIAL_8N1, kPinUartRxd, kPinUartTxd);
  Serial.println("uart: relaying, send a lone Q to exit");
  for (;;) {
    if (Serial.available()) {
      int c = Serial.read();
      if (c == 'Q') {
        Serial1.flush();
        Serial1.end();
        ax_pins_begin();  // restore debug pins for the next command
        Serial.println("uart: relay closed");
        return;
      }
      Serial1.write((uint8_t)c);
    }
    while (Serial1.available()) Serial.write(Serial1.read());
    delay(1);
  }
}

// Transparent console<->UART0 bridge for the running hub app. The BOOT strap
// does not reliably reach the hub on this board (high or low both land in the
// bootloader), so instead of relying on it we reset the hub into the bootloader
// (BOOT low), wait for its banner (the ~6s OTA header check must finish before
// the command loop will act on 'R'), send 'R' at 38400 to run the flashed app,
// then switch the relay to the app's 115200. Active until the ESP32 reboots.
static void cmd_bridge(void) {
  ax_release_debug_bus();
  pinMode(kPinBoot, OUTPUT);
  digitalWrite(kPinBoot, 0);       // bootloader
  pinMode(kPinResetN, OUTPUT);
  digitalWrite(kPinResetN, 1);     // reset released
  // 1. reset the hub into the bootloader
  digitalWrite(kPinResetN, 0);
  delay(20);
  digitalWrite(kPinResetN, 1);
  // 2. talk to the bootloader at 38400
  Serial1.begin(38400, SERIAL_8N1, kPinUartRxd, kPinUartTxd);
  // 3. wait for the banner: it only prints after the ~6s OTA header check, at
  //    which point the command loop will actually process 'R'.
  bool in_loop = false;
  uint32_t t0 = millis();
  while (!in_loop && (millis() - t0) < 9000) {
    while (Serial1.available()) {
      if (Serial1.read() == 'r')  // "...Bootloader..."
        in_loop = true;
    }
    delay(1);
  }
  Serial1.write('R');               // run the flashed application
  delay(1200);                      // app boots and re-inits its UART @115200
  // 4. switch the relay to the app baud
  Serial1.updateBaudRate(115200);
  while (Serial1.available()) Serial1.read();
  // debug: hexdump the first bytes arriving at 115200 so we can see whether the
  // app is actually speaking here (banner) or the line is stuck/garbage.
  uint32_t dt = millis();
  int hd = 0;
  Serial.print("hex@115200: ");
  while (hd < 64 && (millis() - dt) < 3000) {
    if (Serial1.available()) {
      Serial.printf("%02x ", (unsigned)Serial1.read());
      hd++;
    }
  }
  Serial.print("\ncount=");
  Serial.println(hd);
  while (Serial.available()) Serial.read();
  Serial.println("bridge: transparent console<->UART0 @115200; reboot the ESP32 to leave");
  Serial.flush();
  for (;;) {
    if (Serial.available()) Serial1.write((uint8_t)Serial.read());
    if (Serial1.available()) Serial.write((uint8_t)Serial1.read());
    delay(1);
  }
}

static void cmd_wire(int pin) {
  uint8_t self[2];
  uint8_t fall[96];
  uint8_t late[96];
  uint8_t high[96];
  uint8_t high_spin[96];
  uint8_t low_spin[96];
  ax_wire_self(pin, self);
  ax_wire_capture(pin, fall, 96, late, high, high_spin, low_spin);
  Serial.printf("wire %d self_low %u self_high %u\n", pin, self[0], self[1]);
  Serial.println("fall");
  print_bits(fall, 96);
  Serial.println("late");
  print_bits(late, 96);
  Serial.println("high");
  print_bits(high, 96);
  Serial.println("highspin");
  print_bits(high_spin, 96);
  Serial.println("lowspin");
  print_bits(low_spin, 96);
}

static void cmd_bits(int open_drain) {
  uint8_t bits[96];
  int n = ax_capture(bits, 96, open_drain);
  if (n < 0) {
    Serial.println("capture setup failed");
    return;
  }
  Serial.println(open_drain ? "bitsod" : "bits");
  print_bits(bits, n);
}

static void cmd_erase(void) {
  ConnectSession session(g_key);
  std::vector<Op> ops;
  RunOps ran;
  int linked = run_connect(&session);

  // A framing error has no status byte: do not clock 0x54. A decoded status
  // with bit 0x20 clear still gets one opcode. Secure erase does not need the key.
  if (!linked && (!session.have_status() || !session.stopped())) {
    ax_leave_debug();
    return;
  }
  append_bulk_erase(&ops);
  ran = ax_run(ops);
  if (!ran.ok || !take_bulk_erase(ran.replies.empty() ? 0 : &ran.replies[0],
                                  (int)ran.replies.size())) {
    Serial.println(ran.error ? ran.error : "erase reply");
    ax_leave_debug();
    return;
  }
  Serial.println("tx 54");
  Serial.flush();
  {
    uint8_t polled = 0;
    int got = ax_poll_frame(1600000, &polled);
    std::vector<Op> status_ops;
    if (got) Serial.printf("poll %02X\n", polled);
    else Serial.println("poll none");
    append_status(&status_ops);
    ran = ax_run(status_ops);
    if (erase_completed(got, polled)) Serial.println("erase done");
    else Serial.println("erase incomplete");
    if (ran.ok && ran.replies.size() == 1) {
      // The erase clears the key. This session does not report the port unlocked.
      Serial.printf("status %02X\n", ran.replies[0]);
    } else {
      Serial.println(ran.error ? ran.error : "status after erase");
    }
  }
  ax_leave_debug();
}

static void cmd_status(void) {
  ConnectSession session(g_key);
  run_connect(&session);
  if (session.have_status()) print_unlock(session);
  ax_leave_debug();
}

static void cmd_trace(void) {
  AxTrace trace;
  ConnectSession session(g_key);
  memset(&trace, 0, sizeof(trace));
  ax_trace_bind(&trace);
  run_connect(&session);
  ax_clock_listen(4096);
  ax_trace_bind(0);
  Serial.printf("trace DBG_EN %u\n", trace.dbg_en);
  Serial.printf("trace RESET low %u high %u\n", trace.reset_low, trace.reset_high);
  Serial.printf("trace DATA at_dbg %u at_reset_low %u at_reset_high %u\n", trace.data_dbg,
                trace.data_reset_low, trace.data_reset_high);
  Serial.printf("trace CLK high %u low %u edges %u\n", (unsigned)trace.clk_high,
                (unsigned)trace.clk_low, (unsigned)trace.edges);
  Serial.printf("trace DATA lows %u of %u\n", (unsigned)trace.data_low, (unsigned)trace.data_reads);
  Serial.printf("trace listen lows %u of %u\n", (unsigned)trace.listen_low,
                (unsigned)trace.listen_clocks);
  if (session.ready()) {
    Serial.printf("status %02X unlocked %d halt %d\n", session.last_status(),
                  (session.last_status() & kStatusUnlocked) ? 1 : 0,
                  (session.last_status() & kStatusHalt) ? 1 : 0);
  }
  ax_leave_debug();
}

// A desynced chunk is recovered on the wire: re-enter debug and re-unlock, then
// redo the chunk. The host re-sending the whole command used to race the ESP32's
// 288-byte input buffer (line too long) and lose the link; doing the re-sync
// here keeps exactly one command in flight at a time.
static const int kMaxChunkRetries = 8;

static int resync_link(const char* what) {
  ConnectSession session(g_key);
  return connect_for_access(&session, what);
}

static void cmd_read(uint16_t addr, uint32_t len) {
  RunOps ran;
  uint32_t i;

  if (len == 0 || (uint32_t)addr + len > 0x10000u) {
    Serial.println("range exceeds code space");
    return;
  }

  ConnectSession session(g_key);
  if (!connect_for_access(&session, "read")) return;

  std::vector<Op> cfg;
  append_code_config(&cfg);
  ran = ax_run(cfg);
  if (!ran.ok || ran.replies.size() != 1 || ran.replies[0] != kAck) {
    Serial.println("code config ack");
    ax_leave_debug();
    return;
  }

  for (i = 0; i < len;) {
    uint32_t n = len - i;
    if (n > kTransferChunk) n = kTransferChunk;
    int attempt = 0;
    int ok = 0;
    std::vector<uint8_t> bytes;
    const char* err = 0;
    while (attempt < kMaxChunkRetries && !ok) {
      attempt++;
      uint32_t j;
      std::vector<Op> chunk;
      bytes.clear();
      for (j = 0; j < n; j++) append_code_byte(&chunk, (uint16_t)(addr + i + j));
      RunOps dumped = ax_run(chunk);
      if (dumped.ok && collect_dump_bytes(dumped.replies.empty() ? 0 : &dumped.replies[0],
                                          (int)dumped.replies.size(), n, &bytes, &err)) {
        ok = 1;
        break;
      }
      if (attempt >= kMaxChunkRetries) {
        Serial.printf("read: %s\n", dumped.error ? dumped.error : (err ? err : "fail"));
        break;
      }
      if (!resync_link("read")) {
        Serial.println("read: re-sync lost");
        break;
      }
      delay(1 + (attempt < 4 ? attempt : 4));
    }
    if (!ok) {
      ax_leave_debug();
      return;
    }
    for (uint32_t j = 0; j < bytes.size(); j++) {
      if ((j % 16u) == 0) Serial.printf("%04X:", (unsigned)(addr + i + j));
      Serial.printf(" %02X", bytes[j]);
      if ((j % 16u) == 15u || j + 1 == bytes.size()) Serial.println();
    }
    i += n;
  }
  ax_leave_debug();
}

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int parse_hex_bytes(const char* text, uint8_t* out, int cap, int* nout) {
  int n = 0;
  if (text == 0 || out == 0 || nout == 0) return 0;
  while (text[0] != 0) {
    int hi;
    int lo;
    if (text[1] == 0) return 0;
    hi = hex_nibble(text[0]);
    lo = hex_nibble(text[1]);
    if (hi < 0 || lo < 0) return 0;
    if (n >= cap) return 0;
    out[n++] = (uint8_t)((hi << 4) | lo);
    text += 2;
  }
  *nout = n;
  return n > 0;
}

static void cmd_write(uint16_t addr, const uint8_t* data, uint32_t len) {
  uint32_t off;
  if ((addr & 1u) || (len & 1u) || len == 0) {
    Serial.println("write needs an even address and an even length");
    return;
  }
  if ((uint32_t)addr + len > kCalibStart) {
    Serial.println("calibration page");
    return;
  }
  ConnectSession session(g_key);
  if (!connect_for_access(&session, "write")) return;
  for (off = 0; off < len;) {
    uint32_t n = len - off;
    if (n > kTransferChunk) n = kTransferChunk;
    int attempt = 0;
    int ok = 0;
    while (attempt < kMaxChunkRetries && !ok) {
      attempt++;
      std::vector<Op> ops;
      RunOps ran;
      const char* err = 0;
      if (!append_code_bytes(&ops, (uint16_t)(addr + off), data + off, n)) {
        Serial.println("write range");
        ax_leave_debug();
        return;
      }
      if (ops.empty()) {
        ok = 1;
        break;
      }
      ran = ax_run(ops);
      if (ran.ok && take_code_bytes(ran.replies.empty() ? 0 : &ran.replies[0],
                                    (int)ran.replies.size(), data + off, n, &err)) {
        ok = 1;
        break;
      }
      if (attempt >= kMaxChunkRetries) {
        size_t r;
        Serial.printf("write fail at %04X after %d tries\n", (unsigned)(addr + off), attempt);
        for (r = 0; r < ran.replies.size() && r < 16; r++) Serial.printf(" %02X", ran.replies[r]);
        if (!ran.replies.empty()) Serial.println();
        break;
      }
      if (!resync_link("write")) {
        Serial.printf("write fail at %04X: re-sync lost\n", (unsigned)(addr + off));
        break;
      }
      delay(1 + (attempt < 4 ? attempt : 4));
    }
    if (!ok) {
      ax_leave_debug();
      return;
    }
    off += n;
  }
  Serial.printf("write ok %04X %u\n", addr, (unsigned)len);
  ax_leave_debug();
}

static int run_acked(const std::vector<Op>& ops) {
  RunOps ran = ax_run(ops);
  if (!ran.ok || !take_ack(ran.replies.empty() ? 0 : &ran.replies[0], (int)ran.replies.size())) {
    Serial.println(ran.error ? ran.error : "ack");
    return 0;
  }
  return 1;
}

static int print_pc(void) {
  std::vector<Op> ops;
  RunOps ran;
  uint16_t pc = 0;
  append_read_pc(&ops);
  ran = ax_run(ops);
  if (!ran.ok || !take_pc(ran.replies.empty() ? 0 : &ran.replies[0], (int)ran.replies.size(), &pc)) {
    Serial.println(ran.error ? ran.error : "pc reply");
    return 0;
  }
  Serial.printf("pc %04X\n", pc);
  return 1;
}

static void cmd_pc(void) {
  ConnectSession session(g_key);
  if (!connect_for_access(&session, "pc")) return;
  print_pc();
  ax_leave_debug();
}

static void cmd_reset(void) {
  std::vector<Op> ops;
  ConnectSession session(g_key);
  if (!connect_for_access(&session, "reset")) return;
  append_simple(&ops, 0x59);
  if (!run_acked(ops)) {
    ax_leave_debug();
    return;
  }
  ops.clear();
  append_simple(&ops, 0x5A);
  if (!run_acked(ops)) {
    ax_leave_debug();
    return;
  }
  print_pc();
  ax_leave_debug();
}

static void cmd_steps(uint32_t n) {
  uint32_t i;
  std::vector<Op> ops;
  ConnectSession session(g_key);
  if (n == 0 || n > 16) {
    Serial.println("steps 1..16");
    return;
  }
  if (!connect_for_access(&session, "steps")) return;
  append_simple(&ops, 0x59);
  if (!run_acked(ops)) {
    ax_leave_debug();
    return;
  }
  ops.clear();
  append_simple(&ops, 0x5A);
  if (!run_acked(ops)) {
    ax_leave_debug();
    return;
  }
  if (!print_pc()) {
    ax_leave_debug();
    return;
  }
  for (i = 0; i < n; i++) {
    ops.clear();
    append_simple(&ops, 0x58);
    if (!run_acked(ops) || !print_pc()) {
      ax_leave_debug();
      return;
    }
  }
  ax_leave_debug();
}

static void cmd_step(void) {
  std::vector<Op> ops;
  ConnectSession session(g_key);
  if (!connect_for_access(&session, "step")) return;
  append_simple(&ops, 0x58);
  if (!run_acked(ops)) {
    ax_leave_debug();
    return;
  }
  print_pc();
  ax_leave_debug();
}

static void cmd_setkey(const uint8_t key[8]) {
  std::vector<Op> ops;
  RunOps ran;
  const char* err = 0;
  size_t r;
  ConnectSession session(g_key);
  if (!connect_for_access(&session, "setkey")) return;
  if (!append_set_key(&ops, key)) {
    Serial.println("setkey range");
    ax_leave_debug();
    return;
  }
  ran = ax_run(ops);
  if (!ran.ok || !take_set_key(ran.replies.empty() ? 0 : &ran.replies[0],
                               (int)ran.replies.size(), &err)) {
    Serial.println(err ? err : (ran.error ? ran.error : "setkey reply"));
    for (r = 0; r < ran.replies.size() && r < 16; r++) Serial.printf(" %02X", ran.replies[r]);
    if (!ran.replies.empty()) Serial.println();
    ax_leave_debug();
    return;
  }
  memcpy(g_key, key, 8);
  Serial.printf("setkey ok %02X%02X%02X%02X%02X%02X%02X%02X\n", key[7], key[6], key[5], key[4],
                key[3], key[2], key[1], key[0]);
  ax_leave_debug();
}

static void handle_line(char* line) {
  char* cmd;
  char* arg1;
  char* arg2;
  uint32_t a = 0;
  uint32_t b = 0;

  while (*line == ' ' || *line == '\t') line++;
  if (*line == 0) return;
  cmd = line;
  arg1 = 0;
  arg2 = 0;
  while (*line && *line != ' ' && *line != '\t') line++;
  if (*line) {
    *line++ = 0;
    while (*line == ' ' || *line == '\t') line++;
    if (*line) arg1 = line;
  }
  if (arg1) {
    line = arg1;
    while (*line && *line != ' ' && *line != '\t') line++;
    if (*line) {
      *line++ = 0;
      while (*line == ' ' || *line == '\t') line++;
      if (*line) arg2 = line;
    }
  }
  if (arg2) {
    line = arg2;
    while (*line && *line != ' ' && *line != '\t') line++;
    *line = 0;
  }

  if (strcmp(cmd, "help") == 0) {
    print_help();
    return;
  }
  if (strcmp(cmd, "pins") == 0) {
    print_pins();
    return;
  }
  if (strcmp(cmd, "key") == 0) {
    if (!parse_key_hex(arg1, g_key)) {
      Serial.println("key needs 16 hex digits");
      return;
    }
    Serial.println("key ok");
    return;
  }
  if (strcmp(cmd, "setkey") == 0) {
    uint8_t key[8];
    if (!parse_key_hex(arg1, key)) {
      Serial.println("setkey needs 16 hex digits");
      return;
    }
    cmd_setkey(key);
    return;
  }
  if (strcmp(cmd, "unlock") == 0 || strcmp(cmd, "status") == 0) {
    cmd_status();
    return;
  }
  if (strcmp(cmd, "read") == 0 || strcmp(cmd, "dump") == 0) {
    if (arg1 == 0 || arg2 == 0 || !parse_u32(arg1, &a) || !parse_u32(arg2, &b) || a > 0xFFFF) {
      Serial.println("read <addr> <len>");
      return;
    }
    cmd_read((uint16_t)a, b);
    return;
  }
  if (strcmp(cmd, "write") == 0) {
    uint8_t data[kTransferChunk];
    int n = 0;
    if (arg1 == 0 || arg2 == 0 || !parse_u32(arg1, &a) || a > 0xFFFF ||
        !parse_hex_bytes(arg2, data, (int)kTransferChunk, &n)) {
      Serial.println("write <addr> <hexbytes>");
      return;
    }
    cmd_write((uint16_t)a, data, (uint32_t)n);
    return;
  }
  if (strcmp(cmd, "trace") == 0) {
    cmd_trace();
    return;
  }
  if (strcmp(cmd, "erase") == 0) {
    cmd_erase();
    return;
  }
  if (strcmp(cmd, "pc") == 0) {
    cmd_pc();
    return;
  }
  if (strcmp(cmd, "reset") == 0) {
    cmd_reset();
    return;
  }
  if (strcmp(cmd, "step") == 0) {
    cmd_step();
    return;
  }
  if (strcmp(cmd, "steps") == 0) {
    if (arg1 == 0 || !parse_u32(arg1, &a) || a > 16) {
      Serial.println("steps 1..16");
      return;
    }
    cmd_steps(a);
    return;
  }
  if (strcmp(cmd, "loop") == 0) {
    cmd_loop();
    return;
  }
  if (strcmp(cmd, "bits") == 0) {
    cmd_bits(0);
    return;
  }
  if (strcmp(cmd, "bitsod") == 0) {
    cmd_bits(1);
    return;
  }
  if (strcmp(cmd, "wire5") == 0) {
    cmd_wire(kPinDataOut);
    return;
  }
  if (strcmp(cmd, "wire6") == 0) {
    cmd_wire(kPinDataIn);
    return;
  }
  if (strcmp(cmd, "sense") == 0) {
    cmd_sense();
    return;
  }
  if (strcmp(cmd, "scan") == 0) {
    cmd_scan();
    return;
  }
  if (strcmp(cmd, "uart") == 0) {
    cmd_uart();
    return;
  }
  if (strcmp(cmd, "bridge") == 0) {
    cmd_bridge();
    return;
  }
  Serial.println("unknown, help");
}

void setup(void) {
  Serial.begin(115200);
  ax_pins_begin();
  delay(200);
  print_help();
  print_pins();
}

void loop(void) {
  static char buf[288];
  static int n = 0;
  static int overflow = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[n] = 0;
      if (overflow) {
        Serial.println("line too long");
        overflow = 0;
        n = 0;
        continue;
      }
      n = 0;
      handle_line(buf);
      continue;
    }
    if (n < (int)sizeof(buf) - 1) buf[n++] = c;
    else overflow = 1;
  }
}
