#include "ax_proto.h"

#include <stdio.h>

void tx_frame_bits(uint8_t data, uint8_t out[16]) {
  int i;
  for (i = 0; i < 6; i++) out[i] = 1;
  out[6] = 0;
  for (i = 0; i < 8; i++) out[7 + i] = (uint8_t)((data >> i) & 1);
  out[15] = 1;
}

RxFrame rx_frame(const uint8_t* bits, int n) {
  RxFrame r;
  r.ok = false;
  r.byte = 0;
  r.consumed = 0;
  r.error = "framing error";
  if (bits == 0 || n <= 0) return r;

  int i = 0;
  while (i < n && bits[i] != 0) i++;
  if (i >= n) return r;
  i++;
  if (i + 9 > n) return r;

  uint8_t byte = 0;
  int b;
  for (b = 0; b < 8; b++) {
    if (bits[i + b] > 1) {
      r.error = "framing error";
      return r;
    }
    byte = (uint8_t)(byte | (bits[i + b] << b));
  }
  i += 8;
  if (bits[i] != 1) return r;
  i++;
  r.ok = true;
  r.byte = byte;
  r.consumed = i;
  r.error = 0;
  return r;
}

bool rx_frames(const uint8_t* bits, int n, int expect, std::vector<uint8_t>* out,
               const char** error) {
  out->clear();
  int off = 0;
  int k;
  for (k = 0; k < expect; k++) {
    RxFrame f = rx_frame(bits + off, n - off);
    if (!f.ok) {
      if (error) *error = f.error ? f.error : "framing error";
      out->clear();
      return false;
    }
    out->push_back(f.byte);
    off += f.consumed;
  }
  if (error) *error = 0;
  return true;
}

static void add_sample(std::vector<Op>* ops, unsigned gap_arg, uint8_t replies) {
  ops->push_back(Op::Sample(gap_clocks(gap_arg), replies));
}

void append_link_setup(std::vector<Op>* ops) {
  int i;
  ops->push_back(Op::Tx(0xFF));
  ops->push_back(Op::IdleHigh(kResyncIdleClocks));
  for (i = 0; i < 16; i++) ops->push_back(Op::Tx(0xFF));
}

void append_status(std::vector<Op>* ops) {
  ops->push_back(Op::Tx(0x35));
  add_sample(ops, 4, 1);
}

void append_unlock(std::vector<Op>* ops, const uint8_t key[8]) {
  int i;
  ops->push_back(Op::Tx(0x3A));
  for (i = 0; i < 8; i++) ops->push_back(Op::Tx(key[i]));
  add_sample(ops, 4, 1);
}

void append_halt(std::vector<Op>* ops) {
  ops->push_back(Op::Tx(0x56));
  add_sample(ops, 4, 1);
}

void append_simple(std::vector<Op>* ops, uint8_t opcode) {
  if (ops == 0) return;
  ops->push_back(Op::Tx(opcode));
  add_sample(ops, 4, 1);
}

void append_read_pc(std::vector<Op>* ops) {
  if (ops == 0) return;
  ops->push_back(Op::Tx(0x5D));
  add_sample(ops, 4, 1);
  ops->push_back(Op::Tx(0x5C));
  add_sample(ops, 4, 1);
}

int take_ack(const uint8_t* replies, int n) {
  return replies != 0 && n == 1 && replies[0] == kAck;
}

int take_pc(const uint8_t* replies, int n, uint16_t* pc) {
  if (replies == 0 || n != 2 || pc == 0) return 0;
  *pc = (uint16_t)(((uint16_t)replies[0] << 8) | replies[1]);
  return 1;
}

void append_write_pc(std::vector<Op>* ops, uint16_t addr) {
  ops->push_back(Op::Tx(0x5E));
  ops->push_back(Op::Tx((uint8_t)(addr & 0xFF)));
  add_sample(ops, 4, 1);
  ops->push_back(Op::Tx(0x5F));
  ops->push_back(Op::Tx((uint8_t)(addr >> 8)));
  add_sample(ops, 4, 1);
}

bool writes_wbtest(const uint8_t* insn, int len) {
  if (insn == 0 || len < 2) return false;
  if (len == 2 && insn[0] == 0xF5 && insn[1] == 0x8F) return true;
  if (len == 2 && (insn[0] == 0x86 || insn[0] == 0x87) && insn[1] == 0x8F) return true;
  if (len == 2 && insn[0] >= 0x88 && insn[0] <= 0x8F && insn[1] == 0x8F) return true;
  if (len == 3 && insn[0] == 0x75 && insn[1] == 0x8F) return true;
  if (len == 3 && insn[0] == 0x85 && insn[2] == 0x8F) return true;
  return false;
}

void append_exec(std::vector<Op>* ops, const uint8_t* insn, int len) {
  int i;
  if (insn == 0 || len < 1 || len > 3) return;
  ops->push_back(Op::Tx((uint8_t)(0x60 + (len - 1))));
  for (i = 0; i < len; i++) ops->push_back(Op::Tx(insn[i]));
  if (writes_wbtest(insn, len)) add_sample(ops, 6, 2);
  else add_sample(ops, 4, 1);
}

void append_code_config(std::vector<Op>* ops) {
  const uint8_t insn[3] = {0x75, 0x97, 0xD0};
  append_exec(ops, insn, 3);
}

void append_code_byte(std::vector<Op>* ops, uint16_t addr) {
  const uint8_t movc[1] = {0x83};
  const uint8_t mov_wb[2] = {0xF5, 0x8F};
  append_write_pc(ops, addr);
  append_exec(ops, movc, 1);
  append_exec(ops, mov_wb, 2);
}

// A code word is five MOV instructions, a gap of argument 0x16, then
// MOV WBTEST,SFR 0x91. A word of 0xFFFF emits nothing. Success is the
// WBTEST byte with bit 1 set and bit 0 clear. The five MOV acks and the
// final ack must be 0x01. Bulk erase is not part of this sequence.
static const unsigned kFlashWaitGap = 0x16;

// Two writes to NVKEY, then NVSTATUS bit 1 must be set.
static void append_flash_unlock(std::vector<Op>* ops) {
  const uint8_t key_lo[3] = {0x75, 0x96, 0x41};
  const uint8_t key_hi[3] = {0x75, 0x96, 0x78};
  const uint8_t status[3] = {0x85, 0x91, 0x8F};
  append_exec(ops, key_lo, 3);
  append_exec(ops, key_hi, 3);
  append_exec(ops, status, 3);
}

void append_code_word(std::vector<Op>* ops, uint16_t addr, uint16_t word) {
  uint8_t addr_hi[3];
  uint8_t addr_lo[3];
  uint8_t data_hi[3];
  uint8_t data_lo[3];
  uint8_t trigger[3];
  uint8_t status[3];
  if (ops == 0 || word == 0xFFFF) return;
  addr_hi[0] = 0x75;
  addr_hi[1] = 0x93;
  addr_hi[2] = (uint8_t)(addr >> 8);
  addr_lo[0] = 0x75;
  addr_lo[1] = 0x92;
  addr_lo[2] = (uint8_t)(addr & 0xFF);
  data_hi[0] = 0x75;
  data_hi[1] = 0x95;
  data_hi[2] = (uint8_t)(word >> 8);
  data_lo[0] = 0x75;
  data_lo[1] = 0x94;
  data_lo[2] = (uint8_t)(word & 0xFF);
  trigger[0] = 0x75;
  trigger[1] = 0x91;
  trigger[2] = 0x30;
  status[0] = 0x85;
  status[1] = 0x91;
  status[2] = 0x8F;
  append_exec(ops, addr_hi, 3);
  append_exec(ops, addr_lo, 3);
  append_exec(ops, data_hi, 3);
  append_exec(ops, data_lo, 3);
  append_exec(ops, trigger, 3);
  add_sample(ops, kFlashWaitGap, 0);
  append_exec(ops, status, 3);
}

int append_code_bytes(std::vector<Op>* ops, uint16_t addr, const uint8_t* data, uint32_t len) {
  uint32_t i;
  int program = 0;
  if (ops == 0 || data == 0 || len == 0) return 0;
  if ((addr & 1u) || (len & 1u)) return 0;
  if (len > kTransferChunk) return 0;
  if ((uint32_t)addr + len > kCalibStart) return 0;
  for (i = 0; i < len; i += 2) {
    uint16_t word = (uint16_t)(data[i] | ((uint16_t)data[i + 1] << 8));
    if (word != 0xFFFF) program = 1;
  }
  if (program) append_flash_unlock(ops);
  for (i = 0; i < len; i += 2) {
    uint16_t word = (uint16_t)(data[i] | ((uint16_t)data[i + 1] << 8));
    append_code_word(ops, (uint16_t)(addr + i), word);
  }
  return 1;
}

// Flash-info write, trigger 0x34. Address high stays 0.
// A half of 0xFFFF is still transmitted.
static void append_key_half(std::vector<Op>* ops, uint8_t index, uint16_t word) {
  uint8_t addr_hi[3];
  uint8_t addr_lo[3];
  uint8_t data_hi[3];
  uint8_t data_lo[3];
  uint8_t trigger[3];
  uint8_t status[3];
  addr_hi[0] = 0x75;
  addr_hi[1] = 0x93;
  addr_hi[2] = 0x00;
  addr_lo[0] = 0x75;
  addr_lo[1] = 0x92;
  addr_lo[2] = index;
  data_hi[0] = 0x75;
  data_hi[1] = 0x95;
  data_hi[2] = (uint8_t)(word >> 8);
  data_lo[0] = 0x75;
  data_lo[1] = 0x94;
  data_lo[2] = (uint8_t)(word & 0xFF);
  trigger[0] = 0x75;
  trigger[1] = 0x91;
  trigger[2] = 0x34;
  status[0] = 0x85;
  status[1] = 0x91;
  status[2] = 0x8F;
  append_exec(ops, addr_hi, 3);
  append_exec(ops, addr_lo, 3);
  append_exec(ops, data_hi, 3);
  append_exec(ops, data_lo, 3);
  append_exec(ops, trigger, 3);
  add_sample(ops, kFlashWaitGap, 0);
  append_exec(ops, status, 3);
}

int append_set_key(std::vector<Op>* ops, const uint8_t key[8]) {
  int i;
  if (ops == 0 || key == 0) return 0;
  for (i = 0; i < 4; i++) {
    uint16_t word = (uint16_t)(key[i * 2] | ((uint16_t)key[i * 2 + 1] << 8));
    append_flash_unlock(ops);
    append_key_half(ops, (uint8_t)(0x10 + i * 2), word);
  }
  return 1;
}

void append_bulk_erase(std::vector<Op>* ops) {
  if (ops == 0) return;
  ops->push_back(Op::Tx(0x54));
}

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int parse_key_hex(const char* text, uint8_t key[8]) {
  int i;
  if (text == 0 || key == 0) return 0;
  for (i = 0; i < 16; i++) {
    if (hex_nibble(text[i]) < 0) return 0;
  }
  if (text[16] != 0) return 0;
  for (i = 0; i < 8; i++) {
    int hi = hex_nibble(text[i * 2]);
    int lo = hex_nibble(text[i * 2 + 1]);
    key[7 - i] = (uint8_t)((hi << 4) | lo);
  }
  return 1;
}

bool schedule_has_tx(const std::vector<Op>& ops, uint8_t byte) {
  size_t i;
  for (i = 0; i < ops.size(); i++) {
    if (ops[i].type == kOpTx && ops[i].byte == byte) return true;
  }
  return false;
}

ConnectSession::ConnectSession(const uint8_t key[8])
    : unlock_sent_(false),
      have_status_(false),
      last_status_(0),
      phase_(kConnWaitStatus),
      reason_(0) {
  int i;
  for (i = 0; i < 8; i++) key_[i] = key ? key[i] : 0xFF;
  append_link_setup(&pending_);
  append_status(&pending_);
}

void ConnectSession::stop(const char* why) {
  phase_ = kConnStopped;
  reason_ = why;
  pending_.clear();
}

void ConnectSession::handle_status(uint8_t status) {
  have_status_ = true;
  last_status_ = status;
  pending_.clear();
  if ((status & kStatusUnlocked) == 0) {
    if (!unlock_sent_) {
      unlock_sent_ = true;
      append_unlock(&pending_, key_);
      append_status(&pending_);
      phase_ = kConnWaitUnlockStatus;
      return;
    }
    stop("locked: status bit 0x20 still clear");
    return;
  }
  if ((status & kStatusHalt) == 0) {
    append_halt(&pending_);
    phase_ = kConnWaitHalt;
    return;
  }
  phase_ = kConnReady;
  reason_ = 0;
}

void ConnectSession::on_replies(const std::vector<uint8_t>& replies) {
  if (phase_ == kConnReady || phase_ == kConnStopped) return;
  if (phase_ == kConnWaitStatus) {
    if (replies.size() != 1) {
      stop("short status");
      return;
    }
    handle_status(replies[0]);
    return;
  }
  if (phase_ == kConnWaitUnlockStatus) {
    if (replies.size() != 2) {
      stop("short unlock reply");
      return;
    }
    if (replies[0] != kAck) {
      stop("unlock ack");
      return;
    }
    handle_status(replies[1]);
    return;
  }
  if (phase_ == kConnWaitHalt) {
    if (replies.size() != 1 || replies[0] != kAck) {
      stop("halt ack");
      return;
    }
    pending_.clear();
    phase_ = kConnReady;
    reason_ = 0;
  }
}

CodeByteTake take_code_byte(const uint8_t* replies, int n) {
  CodeByteTake t;
  t.ok = false;
  t.value = 0;
  t.error = "short code reply";
  if (replies == 0 || n != 5) return t;
  if (replies[0] != kAck || replies[1] != kAck || replies[2] != kAck || replies[4] != kAck) {
    t.error = "code ack";
    return t;
  }
  t.ok = true;
  t.value = replies[3];
  t.error = 0;
  return t;
}

static int word_reply_len(uint16_t word) {
  return word == 0xFFFF ? 0 : 7;
}

WordWriteTake take_code_word(const uint8_t* replies, int n, uint16_t word) {
  WordWriteTake t;
  int i;
  t.ok = false;
  t.error = "short write reply";
  if (word == 0xFFFF) {
    if (n != 0) {
      t.error = "write ack";
      return t;
    }
    t.ok = true;
    t.error = 0;
    return t;
  }
  if (replies == 0 || n != 7) return t;
  for (i = 0; i < 5; i++) {
    if (replies[i] != kAck) {
      t.error = "write ack";
      return t;
    }
  }
  if (replies[6] != kAck) {
    t.error = "write ack";
    return t;
  }
  if ((replies[5] & 0x02) == 0 || (replies[5] & 0x01) != 0) {
    t.error = "flash status";
    return t;
  }
  t.ok = true;
  t.error = 0;
  return t;
}

static int flash_unlock_ok(const uint8_t* replies) {
  if (replies[0] != kAck || replies[1] != kAck || replies[3] != kAck) return 0;
  if ((replies[2] & 0x02) == 0) return 0;
  return 1;
}

bool take_code_bytes(const uint8_t* replies, int n, const uint8_t* data, uint32_t len,
                     const char** error) {
  uint32_t i;
  int off = 0;
  int program = 0;
  if ((len & 1u) || len == 0 || data == 0) {
    if (error) *error = "short write reply";
    return false;
  }
  for (i = 0; i < len; i += 2) {
    uint16_t word = (uint16_t)(data[i] | ((uint16_t)data[i + 1] << 8));
    if (word != 0xFFFF) program = 1;
  }
  if (program) {
    if (replies == 0 || n < 4 || !flash_unlock_ok(replies)) {
      if (error) *error = (replies == 0 || n < 4) ? "short write reply" : "flash unlock";
      return false;
    }
    off = 4;
  }
  for (i = 0; i < len; i += 2) {
    uint16_t word = (uint16_t)(data[i] | ((uint16_t)data[i + 1] << 8));
    int need = word_reply_len(word);
    WordWriteTake t;
    if (off + need > n) {
      if (error) *error = "short write reply";
      return false;
    }
    t = take_code_word(replies == 0 ? 0 : replies + off, need, word);
    if (!t.ok) {
      if (error) *error = t.error;
      return false;
    }
    off += need;
  }
  if (off != n) {
    if (error) *error = "write ack";
    return false;
  }
  if (error) *error = 0;
  return true;
}

bool take_set_key(const uint8_t* replies, int n, const char** error) {
  int i;
  if (replies == 0 || n != 44) {
    if (error) *error = "short key reply";
    return false;
  }
  for (i = 0; i < 4; i++) {
    const uint8_t* group = replies + i * 11;
    WordWriteTake t;
    if (!flash_unlock_ok(group)) {
      if (error) *error = "flash unlock";
      return false;
    }
    // Word 0 selects the programmed-word reply check. 0xFFFF would skip it.
    t = take_code_word(group + 4, 7, 0);
    if (!t.ok) {
      if (error) *error = t.error;
      return false;
    }
  }
  if (error) *error = 0;
  return true;
}

bool take_bulk_erase(const uint8_t* replies, int n) {
  (void)replies;
  return n == 0;
}

int erase_completed(int got_frame, uint8_t byte) {
  return got_frame && byte == kAck;
}

bool collect_dump_bytes(const uint8_t* replies, int n, uint32_t len,
                        std::vector<uint8_t>* out, const char** error) {
  uint32_t i;
  out->clear();
  if ((uint32_t)n != len * 5u) {
    if (error) *error = "short dump reply";
    return false;
  }
  for (i = 0; i < len; i++) {
    CodeByteTake t = take_code_byte(replies + (i * 5u), 5);
    if (!t.ok) {
      if (error) *error = t.error;
      out->clear();
      return false;
    }
    out->push_back(t.value);
  }
  if (error) *error = 0;
  return true;
}

DumpGate gate_dump(uint8_t got, uint8_t expect, uint16_t dump_addr, uint32_t dump_len) {
  DumpGate g;
  g.ok = false;
  char buf[80];
  if ((uint32_t)dump_addr + dump_len > 0x10000u) {
    g.detail = "range exceeds code space";
    return g;
  }
  if (got != expect) {
    sprintf(buf, "mismatch got %02X expect %02X", (unsigned)got, (unsigned)expect);
    g.detail = buf;
    return g;
  }
  uint32_t i;
  for (i = 0; i < dump_len; i++) {
    append_code_byte(&g.next, (uint16_t)(dump_addr + i));
  }
  g.ok = true;
  g.detail = "ok";
  return g;
}

DumpGate finish_probe(const uint8_t* replies, int nreply, uint8_t expect,
                      uint16_t dump_addr, uint32_t dump_len) {
  CodeByteTake t = take_code_byte(replies, nreply);
  if (!t.ok) {
    DumpGate g;
    g.ok = false;
    g.detail = t.error ? t.error : "short code reply";
    return g;
  }
  return gate_dump(t.value, expect, dump_addr, dump_len);
}
