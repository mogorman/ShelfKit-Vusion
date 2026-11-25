#ifndef AX_LINK_H
#define AX_LINK_H

#include <stdint.h>
#include <vector>

#include "ax_proto.h"

struct RunOps {
  bool ok;
  std::vector<uint8_t> replies;
  // Bits from the sample that failed to frame. Empty when the run succeeded
  // or failed before a sample.
  std::vector<uint8_t> sample_bits;
  const char* error;
};

void ax_pins_begin(void);
void ax_enter_debug(void);
void ax_leave_debug(void);
void ax_loopback(uint8_t out[3]);
int ax_capture(uint8_t* bits, int n, int open_drain);
void ax_wire_self(int pin, uint8_t out[2]);
// fall is sampled on the falling edge, late after the low spin,
// high at the end of the high spin. Unused traces may be null.
// high_spin and low_spin are 0 where the line dipped during that half.
int ax_wire_capture(int pin, uint8_t* fall, int n, uint8_t* late, uint8_t* high, uint8_t* high_spin,
                    uint8_t* low_spin);
// Pin order: CLK, DATA, DBG_EN, RESET_N.
// Millivolts are 8 samples with the pins floating, before either pull is enabled.
struct AxSense {
  uint8_t pulldown[4];
  uint8_t pullup[4];
  uint8_t data_dbg[2];
  int clock_lows;
  int mv_min[4];
  int mv_max[4];
  uint8_t clk_drive_high;
  uint8_t clk_drive_low;
};

void ax_sense_pins(AxSense* out);

// Header GPIOs read as inputs. Skips USB, UART0, and the module flash/PSRAM bus.
static const int kScanPinCount = 29;
struct AxScanPin {
  int pin;
  uint8_t pulldown;
  uint8_t pullup;
  int mv_min;
  int mv_max;
};

void ax_scan_pins(AxScanPin* out);

// Filled by the real ax_enter_debug and clock edges while bound.
struct AxTrace {
  uint8_t dbg_en;
  uint8_t reset_low;
  uint8_t reset_high;
  uint8_t data_dbg;
  uint8_t data_reset_low;
  uint8_t data_reset_high;
  uint32_t clk_high;
  uint32_t clk_low;
  uint32_t edges;
  uint32_t data_low;
  uint32_t data_reads;
  uint32_t listen_clocks;
  uint32_t listen_low;
};

void ax_trace_bind(AxTrace* trace);
// Clocks with DATA released. Counts lows in the bound trace, including dips
// during the low half. Does not decode a frame and does not send a command.
void ax_clock_listen(uint32_t clocks);
// After opcode 0x54 the link clocks with the line released until a framed
// byte arrives. Returns 1 and stores that byte, or 0 on timeout.
int ax_poll_frame(uint32_t max_clocks, uint8_t* out);
RunOps ax_run(const std::vector<Op>& ops);

#endif
