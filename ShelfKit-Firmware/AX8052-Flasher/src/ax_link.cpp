#include "ax_link.h"

#include <Arduino.h>

#include "ax_pins.h"
#include "soc/gpio_reg.h"

static void half_delay(void) { delayMicroseconds(kClockHalfUs); }

static uint8_t read_bit(int pin);

static AxTrace* g_trace = 0;

void ax_trace_bind(AxTrace* trace) { g_trace = trace; }

static void note_clock(int level) {
  if (g_trace == 0) return;
  if (level) {
    g_trace->edges++;
    if (read_bit(kPinClk)) g_trace->clk_high++;
  } else if (read_bit(kPinClk) == 0) {
    g_trace->clk_low++;
  }
  g_trace->data_reads++;
  if (read_bit(kPinDataIn) == 0) g_trace->data_low++;
}

static void clock_high(void) {
  digitalWrite(kPinClk, HIGH);
  if (g_trace) note_clock(1);
}
static void clock_low(void) {
  digitalWrite(kPinClk, LOW);
  if (g_trace) note_clock(0);
}

static void set_oe_released(int release) {
  digitalWrite(kPinOe, ax_oe_pin_level(release) ? HIGH : LOW);
}

static void od_level(int pin, int high);
static void od_frame(int pin, uint8_t byte);

static uint8_t read_bit(int pin) {
  return (uint8_t)(digitalRead(pin) == HIGH ? 1 : 0);
}

// 0 if the open-drain line dips at any point in the spin, else 1.
static uint8_t spin_bit(int pin) {
  int i;
  uint32_t mask;
  if (pin < 0 || pin > 31) return read_bit(pin);
  mask = 1u << (uint32_t)pin;
  for (i = 0; i < 64; i++) {
    if ((REG_READ(GPIO_IN_REG) & mask) == 0) return 0;
  }
  return 1;
}

// The build that returned a status byte samples at the end of the low half.
static void pulse_clocks(uint32_t clocks, int sample, std::vector<uint8_t>* bits) {
  uint32_t i;
  for (i = 0; i < clocks; i++) {
    clock_high();
    half_delay();
    clock_low();
    if (sample) {
      half_delay();
      bits->push_back(read_bit(kPinDataIn));
    } else {
      half_delay();
    }
  }
}

static void shift_frame(uint8_t byte) {
  // Single wire on GPIO6. High is the pull-up, low is driven.
  set_oe_released(1);
  od_frame(kPinDataIn, byte);
}

void ax_pins_begin(void) {
  pinMode(kPinClk, OUTPUT);
  pinMode(kPinDataOut, OUTPUT);
  pinMode(kPinDataIn, INPUT_PULLUP);
  pinMode(kPinOe, OUTPUT);
  pinMode(kPinResetN, OUTPUT);
  pinMode(kPinDbgEn, OUTPUT);
  digitalWrite(kPinClk, LOW);
  digitalWrite(kPinDataOut, HIGH);
  set_oe_released(1);
  digitalWrite(kPinResetN, HIGH);
  digitalWrite(kPinDbgEn, LOW);
}

void ax_enter_debug(void) {
  digitalWrite(kPinDbgEn, HIGH);
  if (g_trace) {
    g_trace->dbg_en = read_bit(kPinDbgEn);
    g_trace->data_dbg = read_bit(kPinDataIn);
  }
  digitalWrite(kPinResetN, LOW);
  delay(10);
  if (g_trace) {
    g_trace->reset_low = read_bit(kPinResetN);
    g_trace->data_reset_low = read_bit(kPinDataIn);
  }
  digitalWrite(kPinResetN, HIGH);
  delay(10);
  if (g_trace) {
    g_trace->reset_high = read_bit(kPinResetN);
    g_trace->data_reset_high = read_bit(kPinDataIn);
  }
}

void ax_leave_debug(void) {
  set_oe_released(1);
  pinMode(kPinDataOut, OUTPUT);
  digitalWrite(kPinDataOut, HIGH);
  digitalWrite(kPinClk, LOW);
  digitalWrite(kPinResetN, HIGH);
  digitalWrite(kPinDbgEn, LOW);
}

void ax_loopback(uint8_t out[3]) {
  ax_enter_debug();
  pinMode(kPinDataOut, OUTPUT);
  set_oe_released(0);
  digitalWrite(kPinDataOut, HIGH);
  delayMicroseconds(50);
  out[0] = (uint8_t)(digitalRead(kPinDataIn) == HIGH ? 1 : 0);
  digitalWrite(kPinDataOut, LOW);
  delayMicroseconds(50);
  out[1] = (uint8_t)(digitalRead(kPinDataIn) == HIGH ? 1 : 0);
  set_oe_released(1);
  delayMicroseconds(50);
  out[2] = (uint8_t)(digitalRead(kPinDataIn) == HIGH ? 1 : 0);
  ax_leave_debug();
}

static void od_level(int pin, int high) {
  if (high) {
    pinMode(pin, INPUT_PULLUP);
    return;
  }
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
}

int ax_poll_frame(uint32_t max_clocks, uint8_t* out) {
  uint32_t i;
  int state = 0;
  int bits = 0;
  uint8_t byte = 0;
  set_oe_released(1);
  od_level(kPinDataIn, 1);
  for (i = 0; i < max_clocks; i++) {
    uint8_t bit;
    clock_high();
    half_delay();
    clock_low();
    half_delay();
    bit = read_bit(kPinDataIn);
    if (state == 0) {
      if (bit == 0) {
        state = 1;
        bits = 0;
        byte = 0;
      }
    } else if (state == 1) {
      byte = (uint8_t)(byte | (bit << bits));
      bits++;
      if (bits == 8) state = 2;
    } else if (bit == 1) {
      if (out) *out = byte;
      return 1;
    } else {
      state = 1;
      bits = 0;
      byte = 0;
    }
    if ((i & 2047u) == 2047u) yield();
  }
  return 0;
}

void ax_clock_listen(uint32_t clocks) {
  uint32_t i;
  set_oe_released(1);
  od_level(kPinDataIn, 1);
  for (i = 0; i < clocks; i++) {
    clock_high();
    half_delay();
    clock_low();
    if (g_trace) {
      g_trace->listen_clocks++;
      if (spin_bit(kPinDataIn) == 0) g_trace->listen_low++;
    }
    half_delay();
  }
}

static void od_frame(int pin, uint8_t byte) {
  uint8_t bits[16];
  int i;
  tx_frame_bits(byte, bits);
  for (i = 0; i < 16; i++) {
    od_level(pin, bits[i]);
    half_delay();
    clock_high();
    half_delay();
    clock_low();
  }
}

void ax_sense_pins(AxSense* out) {
  const int pins[4] = {kPinClk, kPinDataIn, kPinDbgEn, kPinResetN};
  int i;
  int sample;
  int lows = 0;
  if (out == 0) return;
  for (i = 0; i < 4; i++) {
    out->mv_min[i] = 10000;
    out->mv_max[i] = 0;
    pinMode(pins[i], INPUT);
  }
  delay(2);
  for (sample = 0; sample < 8; sample++) {
    for (i = 0; i < 4; i++) {
      int mv = analogReadMilliVolts(pins[i]);
      if (mv < out->mv_min[i]) out->mv_min[i] = mv;
      if (mv > out->mv_max[i]) out->mv_max[i] = mv;
    }
  }
  for (i = 0; i < 4; i++) pinMode(pins[i], INPUT_PULLDOWN);
  delay(2);
  for (i = 0; i < 4; i++) out->pulldown[i] = (uint8_t)(digitalRead(pins[i]) == HIGH ? 1 : 0);
  for (i = 0; i < 4; i++) pinMode(pins[i], INPUT_PULLUP);
  delay(2);
  for (i = 0; i < 4; i++) out->pullup[i] = (uint8_t)(digitalRead(pins[i]) == HIGH ? 1 : 0);
  digitalWrite(kPinResetN, HIGH);
  digitalWrite(kPinDbgEn, HIGH);
  pinMode(kPinResetN, OUTPUT);
  pinMode(kPinDbgEn, OUTPUT);
  digitalWrite(kPinResetN, HIGH);
  digitalWrite(kPinDbgEn, HIGH);
  pinMode(kPinDataIn, INPUT_PULLDOWN);
  delay(2);
  out->data_dbg[0] = (uint8_t)(digitalRead(kPinDataIn) == HIGH ? 1 : 0);
  pinMode(kPinDataIn, INPUT_PULLUP);
  delay(2);
  out->data_dbg[1] = (uint8_t)(digitalRead(kPinDataIn) == HIGH ? 1 : 0);
  digitalWrite(kPinClk, LOW);
  pinMode(kPinClk, OUTPUT);
  for (i = 0; i < 64; i++) {
    digitalWrite(kPinClk, HIGH);
    delayMicroseconds(20);
    digitalWrite(kPinClk, LOW);
    delayMicroseconds(20);
    if (digitalRead(kPinDataIn) == LOW) lows++;
  }
  out->clock_lows = lows;
  digitalWrite(kPinClk, HIGH);
  delay(2);
  out->clk_drive_high = (uint8_t)(digitalRead(kPinClk) == HIGH ? 1 : 0);
  digitalWrite(kPinClk, LOW);
  delay(2);
  out->clk_drive_low = (uint8_t)(digitalRead(kPinClk) == HIGH ? 1 : 0);
  digitalWrite(kPinClk, LOW);
  digitalWrite(kPinResetN, HIGH);
  digitalWrite(kPinDbgEn, LOW);
  pinMode(kPinClk, OUTPUT);
  pinMode(kPinDataIn, INPUT_PULLUP);
  pinMode(kPinResetN, OUTPUT);
  pinMode(kPinDbgEn, OUTPUT);
  digitalWrite(kPinClk, LOW);
  digitalWrite(kPinResetN, HIGH);
  digitalWrite(kPinDbgEn, LOW);
  set_oe_released(1);
}

void ax_scan_pins(AxScanPin* out) {
  static const int kPins[kScanPinCount] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
                                            15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 45, 46, 47, 48};
  int i;
  int sample;
  if (out == 0) return;
  for (i = 0; i < kScanPinCount; i++) {
    out[i].pin = kPins[i];
    out[i].pulldown = 0;
    out[i].pullup = 0;
    out[i].mv_min = -1;
    out[i].mv_max = -1;
    pinMode(kPins[i], INPUT);
  }
  delay(2);
  for (sample = 0; sample < 4; sample++) {
    for (i = 0; i < kScanPinCount; i++) {
      int mv;
      if (kPins[i] < 1 || kPins[i] > 20) continue;
      mv = analogReadMilliVolts(kPins[i]);
      if (out[i].mv_min < 0 || mv < out[i].mv_min) out[i].mv_min = mv;
      if (mv > out[i].mv_max) out[i].mv_max = mv;
    }
  }
  for (i = 0; i < kScanPinCount; i++) pinMode(kPins[i], INPUT_PULLDOWN);
  delay(2);
  for (i = 0; i < kScanPinCount; i++) {
    out[i].pulldown = (uint8_t)(digitalRead(kPins[i]) == HIGH ? 1 : 0);
  }
  for (i = 0; i < kScanPinCount; i++) pinMode(kPins[i], INPUT_PULLUP);
  delay(2);
  for (i = 0; i < kScanPinCount; i++) {
    out[i].pullup = (uint8_t)(digitalRead(kPins[i]) == HIGH ? 1 : 0);
  }
  ax_pins_begin();
}

void ax_wire_self(int pin, uint8_t out[2]) {
  od_level(pin, 0);
  delayMicroseconds(50);
  out[0] = (uint8_t)(digitalRead(pin) == HIGH ? 1 : 0);
  od_level(pin, 1);
  delayMicroseconds(50);
  out[1] = (uint8_t)(digitalRead(pin) == HIGH ? 1 : 0);
  pinMode(pin, INPUT_PULLUP);
}

int ax_wire_capture(int pin, uint8_t* fall, int n, uint8_t* late, uint8_t* high, uint8_t* high_spin,
                      uint8_t* low_spin) {
  int i;
  set_oe_released(1);
  ax_enter_debug();
  od_frame(pin, 0xFF);
  od_level(pin, 1);
  pulse_clocks(kResyncIdleClocks, 0, 0);
  for (i = 0; i < 16; i++) od_frame(pin, 0xFF);
  od_frame(pin, 0x35);
  od_level(pin, 1);
  for (i = 0; i < n; i++) {
    clock_high();
    if (high_spin) high_spin[i] = spin_bit(pin);
    else half_delay();
    if (high) high[i] = read_bit(pin);
    clock_low();
    if (fall) fall[i] = read_bit(pin);
    if (low_spin) low_spin[i] = spin_bit(pin);
    else half_delay();
    if (late) late[i] = read_bit(pin);
  }
  pinMode(pin, INPUT_PULLUP);
  ax_leave_debug();
  return n;
}

int ax_capture(uint8_t* bits, int n, int open_drain) {
  std::vector<Op> ops;
  int i;
  append_link_setup(&ops);
  ops.push_back(Op::Tx(0x35));
  ax_enter_debug();
  RunOps ran = ax_run(ops);
  if (!ran.ok) {
    ax_leave_debug();
    return -1;
  }
  if (open_drain) {
    pinMode(kPinDataOut, INPUT_PULLUP);
  } else {
    digitalWrite(kPinDataOut, HIGH);
  }
  set_oe_released(1);
  for (i = 0; i < n; i++) {
    clock_high();
    half_delay();
    clock_low();
    half_delay();
    bits[i] = read_bit(kPinDataIn);
  }
  ax_leave_debug();
  return n;
}

RunOps ax_run(const std::vector<Op>& ops) {
  RunOps result;
  result.ok = false;
  result.error = 0;
  size_t i;
  for (i = 0; i < ops.size(); i++) {
    const Op& op = ops[i];
    if (op.type == kOpTx) {
      shift_frame(op.byte);
      continue;
    }
    if (op.type == kOpIdleHigh) {
      set_oe_released(1);
      od_level(kPinDataIn, 1);
      pulse_clocks(op.clocks, 0, 0);
      continue;
    }
    if (op.type == kOpSample) {
      set_oe_released(1);
      od_level(kPinDataIn, 1);
      std::vector<uint8_t> bits;
      bits.reserve(op.clocks);
      pulse_clocks(op.clocks, 1, &bits);
      std::vector<uint8_t> bytes;
      const char* err = 0;
      if (!rx_frames(bits.empty() ? 0 : &bits[0], (int)bits.size(), op.expect_replies, &bytes,
                     &err)) {
        result.error = err ? err : "framing error";
        result.replies.clear();
        result.sample_bits.swap(bits);
        return result;
      }
      size_t b;
      for (b = 0; b < bytes.size(); b++) result.replies.push_back(bytes[b]);
      continue;
    }
    result.error = "bad op";
    return result;
  }
  result.ok = true;
  return result;
}
