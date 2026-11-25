#ifndef AX_PROTO_H
#define AX_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

// Wire schedule for the AX8052 debug port. No GPIO here.
// Code read, code-word program, and bulk erase are built here.
// One transfer chunk stays far below a 64 KB schedule.
static const uint32_t kTransferChunk = 128;
// Code bytes at and above this address are the calibration page.
static const uint32_t kCalibStart = 0xFC00;

enum OpType {
  kOpTx = 1,
  kOpIdleHigh = 2,
  kOpSample = 3
};

struct Op {
  OpType type;
  uint8_t byte;
  uint32_t clocks;
  uint8_t expect_replies;

  static Op Tx(uint8_t b) {
    Op o;
    o.type = kOpTx;
    o.byte = b;
    o.clocks = 0;
    o.expect_replies = 0;
    return o;
  }
  static Op IdleHigh(uint32_t clocks) {
    Op o;
    o.type = kOpIdleHigh;
    o.byte = 0;
    o.clocks = clocks;
    o.expect_replies = 0;
    return o;
  }
  static Op Sample(uint32_t clocks, uint8_t expect_replies) {
    Op o;
    o.type = kOpSample;
    o.byte = 0;
    o.clocks = clocks;
    o.expect_replies = expect_replies;
    return o;
  }
};

struct LineDrive {
  uint8_t data_high;   // 1 = DBG_DATA high
  uint8_t release_oe;  // 1 = stop driving (OE pin high, active low)
};

static const uint32_t kResyncIdleClocks = 15000;
static const uint8_t kStatusUnlocked = 0x20;
static const uint8_t kStatusHalt = 0x02;
static const uint8_t kAck = 0x01;

// A read gap of argument N is N*8 clocks with the data line released.
// The resync idle is 15000 clocks and does not use this scale.
inline uint32_t gap_clocks(unsigned argument) {
  return (uint32_t)argument * 8u;
}

inline LineDrive line_idle_high() {
  LineDrive d;
  d.data_high = 1;
  d.release_oe = 0;
  return d;
}

inline LineDrive line_sample() {
  LineDrive d;
  d.data_high = 1;
  d.release_oe = 1;
  return d;
}

void tx_frame_bits(uint8_t data, uint8_t out[16]);

struct RxFrame {
  bool ok;
  uint8_t byte;
  int consumed;
  const char* error;
};

RxFrame rx_frame(const uint8_t* bits, int n);
bool rx_frames(const uint8_t* bits, int n, int expect, std::vector<uint8_t>* out,
               const char** error);

void append_link_setup(std::vector<Op>* ops);
void append_status(std::vector<Op>* ops);
void append_unlock(std::vector<Op>* ops, const uint8_t key[8]);
void append_halt(std::vector<Op>* ops);
void append_simple(std::vector<Op>* ops, uint8_t opcode);
void append_read_pc(std::vector<Op>* ops);
void append_write_pc(std::vector<Op>* ops, uint16_t addr);
void append_exec(std::vector<Op>* ops, const uint8_t* insn, int len);
void append_code_config(std::vector<Op>* ops);
void append_code_byte(std::vector<Op>* ops, uint16_t addr);

// Programs one code word. A word of 0xFFFF emits nothing.
// The first byte of the caller buffer is the even address.
void append_code_word(std::vector<Op>* ops, uint16_t addr, uint16_t word);
// Even address, even length, length <= kTransferChunk, and the range stays
// below kCalibStart. Returns 0 and appends nothing when those checks fail.
int append_code_bytes(std::vector<Op>* ops, uint16_t addr, const uint8_t* data, uint32_t len);
// Writes the debug key as four flash-info halves at 0x10, 0x12, 0x14, 0x16.
// key[0] is the least significant byte. A null key appends nothing.
int append_set_key(std::vector<Op>* ops, const uint8_t key[8]);
// One TX byte 0x54. The command itself returns no reply byte.
void append_bulk_erase(std::vector<Op>* ops);

bool writes_wbtest(const uint8_t* insn, int len);
bool schedule_has_tx(const std::vector<Op>& ops, uint8_t byte);

// 16 hex digits, numeric 64-bit value. key[0] is the least significant byte.
int parse_key_hex(const char* text, uint8_t key[8]);
int take_ack(const uint8_t* replies, int n);
int take_pc(const uint8_t* replies, int n, uint16_t* pc);

enum ConnectPhase {
  kConnWaitStatus = 1,
  kConnWaitUnlockStatus = 2,
  kConnWaitHalt = 3,
  kConnReady = 4,
  kConnStopped = 5
};

class ConnectSession {
 public:
  explicit ConnectSession(const uint8_t key[8]);
  const std::vector<Op>& pending() const { return pending_; }
  bool ready() const { return phase_ == kConnReady; }
  bool stopped() const { return phase_ == kConnStopped; }
  bool have_status() const { return have_status_; }
  uint8_t last_status() const { return last_status_; }
  const char* reason() const { return reason_; }
  void on_replies(const std::vector<uint8_t>& replies);

 private:
  void stop(const char* why);
  void handle_status(uint8_t status);

  uint8_t key_[8];
  bool unlock_sent_;
  bool have_status_;
  uint8_t last_status_;
  int phase_;
  const char* reason_;
  std::vector<Op> pending_;
};

struct DumpGate {
  bool ok;
  std::string detail;
  std::vector<Op> next;
};

struct CodeByteTake {
  bool ok;
  uint8_t value;
  const char* error;
};

CodeByteTake take_code_byte(const uint8_t* replies, int n);

struct WordWriteTake {
  bool ok;
  const char* error;
};

// Seven replies for a programmed word: five acks, the SFR 0x91 byte, one ack.
// A word of 0xFFFF accepts an empty reply and nothing else.
WordWriteTake take_code_word(const uint8_t* replies, int n, uint16_t word);
bool take_code_bytes(const uint8_t* replies, int n, const uint8_t* data, uint32_t len,
                     const char** error);
// Four programmed info halves, each with its own NVKEY prefix. 0xFFFF is programmed too.
bool take_set_key(const uint8_t* replies, int n, const char** error);
// Erase success is an empty reply. Any byte is rejected.
bool take_bulk_erase(const uint8_t* replies, int n);
// The released-line poll after opcode 0x54 is complete only when a frame
// arrived and its byte is 0x01.
int erase_completed(int got_frame, uint8_t byte);

bool collect_dump_bytes(const uint8_t* replies, int n, uint32_t len,
                        std::vector<uint8_t>* out, const char** error);
DumpGate gate_dump(uint8_t got, uint8_t expect, uint16_t dump_addr, uint32_t dump_len);
DumpGate finish_probe(const uint8_t* replies, int nreply, uint8_t expect,
                      uint16_t dump_addr, uint32_t dump_len);

#endif
