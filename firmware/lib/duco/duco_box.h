// Box-side protocol helpers for the Duco RF network: build the frames the box
// sends, and parse the status the sensors send back. Plain C++ (host-testable).
//
// All frame layouts and the CRC are confirmed against on-air captures of a real
// DucoBox (docs/protocol.md). See duco_frame.h for the shared header/CRC/scan.
#pragma once
#include "duco_frame.h"
#include <string.h>

namespace duco {

// --- building the box's outgoing frames --------------------------------------
// Pack src/dst/orig.src/orig.dst (5 bits each) + counter (4 bits) into 3 bytes.
inline void packAddr(uint8_t* b, uint8_t src, uint8_t dst, uint8_t osrc, uint8_t odst, uint8_t ctr) {
  uint32_t v = ((uint32_t)(src & 31) << 19) | ((uint32_t)(dst & 31) << 14) |
               ((uint32_t)(osrc & 31) << 9) | ((uint32_t)(odst & 31) << 4) | (ctr & 15);
  b[0] = (v >> 16) & 0xFF; b[1] = (v >> 8) & 0xFF; b[2] = v & 0xFF;
}

// Wrap a payload as [len | payload | crc_hi | crc_lo]; returns total byte count.
inline uint8_t frame(uint8_t* out, const uint8_t* payload, uint8_t n) {
  out[0] = n;
  memcpy(out + 1, payload, n);
  uint16_t crc = crc16(out, (size_t)n + 1);
  out[1 + n] = crc >> 8; out[2 + n] = crc & 0xFF;
  return n + 3;
}

// Network heartbeat (type 0x00 broadcast). mode = current system mode byte.
inline uint8_t buildHeartbeat(uint8_t* out, uint32_t net, uint8_t ctr, uint8_t mode) {
  uint8_t pl[10];
  pl[0] = 0x00;
  pl[1] = net >> 24; pl[2] = net >> 16; pl[3] = net >> 8; pl[4] = net;
  packAddr(pl + 5, 1, 0, 1, 0, ctr);
  pl[8] = 0xFF; pl[9] = mode;
  return frame(out, pl, 10);
}

// ACK (type 0x06) back to a node, echoing the counter of the frame we received.
// via = the repeater the frame came through (0 = direct). The nodes route ACKs
// back the same way, e.g. 5->3 (orig 5->1) then 3->1 (orig 5->1).
inline uint8_t buildAck(uint8_t* out, uint32_t net, uint8_t node, uint8_t ctr, uint8_t via = 0) {
  uint8_t pl[9];
  pl[0] = 0x06;
  pl[1] = net >> 24; pl[2] = net >> 16; pl[3] = net >> 8; pl[4] = net;
  packAddr(pl + 5, 1, via ? via : node, 1, node, ctr);   // writes pl[5..7]
  pl[8] = 0xFF;                               // rssi field
  return frame(out, pl, 9);
}

// A directed box message (type 0x07) carrying one command of n bytes, to `node`,
// through repeater `via` (0 = direct). The real box sends e.g. node 3's
// messages as 1->5 (orig 1->3). node 0 = broadcast.
inline uint8_t buildMessage(uint8_t* out, uint32_t net, uint8_t node, uint8_t ctr,
                            const uint8_t* cmd, uint8_t n, uint8_t via = 0) {
  uint8_t pl[11 + 15];
  pl[0] = 0x07;
  pl[1] = net >> 24; pl[2] = net >> 16; pl[3] = net >> 8; pl[4] = net;
  packAddr(pl + 5, 1, via ? via : node, 1, node, ctr);
  pl[8] = 0xFF;
  pl[9] = (uint8_t)(n << 4); pl[10] = 0x00;   // one command of n bytes
  memcpy(pl + 11, cmd, n);
  return frame(out, pl, (uint8_t)(11 + n));
}

// Read a 16-bit parameter register from a node (command 0x40).
// Reply comes back as command 0x41 (see parseStatus / register meanings).
inline uint8_t buildReadParam(uint8_t* out, uint32_t net, uint8_t node, uint8_t ctr, uint8_t reg, uint8_t via = 0) {
  const uint8_t c[3] = {0x40, 0x00, reg};
  return buildMessage(out, net, node, ctr, c, 3, via);
}

// Confirm/set the ventilation mode to a node (command 0x12): what the box sends
// every node after a switch requests a mode, so they latch it.
// mode = 0 auto, 4 low, 5 middle, 6 high, 7 away; +0x08 = permanent (long press).
inline uint8_t buildConfirmMode(uint8_t* out, uint32_t net, uint8_t node, uint8_t ctr, uint8_t mode, uint8_t via = 0) {
  const uint8_t c[2] = {0x12, mode};
  return buildMessage(out, net, node, ctr, c, 2, via);
}

// The keep-alive/poll the box sends constantly (command 0x52 <arg>); the node
// ACKs it. arg 00 in normal operation (01 seen in the box's first minute).
inline uint8_t buildPoll52(uint8_t* out, uint32_t net, uint8_t node, uint8_t ctr, uint8_t via = 0, uint8_t arg = 0) {
  const uint8_t c[2] = {0x52, arg};
  return buildMessage(out, net, node, ctr, c, 2, via);
}

// --- parsing what a node sends -----------------------------------------------
enum Mode { MODE_AUTO = 0, MODE_LOW = 4, MODE_MIDDLE = 5, MODE_HIGH = 6, MODE_AWAY = 7 };

struct NodeStatus {
  bool     hasStatus = false;   // a 0x22 was decoded
  uint8_t  node = 0;
  uint8_t  demandPct = 0;       // 0..100 sensor demand
  // flags and button, confirmed from the reference capture of the real box:
  //   short low 0C/09, middle 0D/0A, high 0E/0B, auto 08/08; long middle 2D/0E
  uint8_t  mode = 0;            // flags bits 0-2: (requested) mode
  bool     request = false;     // flags 0x08: a button press asking for `mode`
  bool     permanent = false;   // flags 0x20: permanent (long press); 0x12 uses 0x08 for it
  uint8_t  flags = 0;           // raw flags byte
  float    tempC = 0;           // (temp byte + 100) / 10
  uint8_t  boot = 0;            // 0x00 normally, 0xFF after a switch reboot
  uint8_t  button = 0;          // 0 = status report; press: 0x08 | button no. (0 auto,
                                // 1, 2, 3), + 0x04 for a long press
  bool     hasReg = false;      // a 0x41 reply was decoded
  uint8_t  reg = 0;             // register number
  uint16_t regValue = 0;        // raw register value
};

// Find the first command byte in a type-0x07 payload (after the 2 length-nibble
// bytes), returning a pointer + remaining length, or null if not present.
inline const uint8_t* firstCommand(const Frame& f, uint8_t& cmdLen) {
  if (f.len < 12 || f.payload[0] != 0x07) return nullptr;
  uint8_t l0 = f.payload[9] >> 4;        // length of command 1
  if (l0 == 0 || 11 + l0 > f.len) return nullptr;
  cmdLen = l0;
  return f.payload + 11;
}

// Decode a node->box frame into status. Returns true if anything was parsed.
inline bool parseStatus(const Frame& f, NodeStatus& s) {
  Header h = header(f);
  if (!h.valid) return false;
  s.node = h.osrc;                        // the real originator (node), not a repeater
  uint8_t cmdLen = 0;
  const uint8_t* c = firstCommand(f, cmdLen);
  if (!c) return false;
  if (c[0] == 0x22 && cmdLen >= 6) {      // status / vent-mode report
    s.hasStatus = true;
    s.demandPct = c[1];
    s.mode = c[2] & 0x07;
    s.request = (c[2] & 0x08) != 0;
    s.permanent = (c[2] & 0x20) != 0;
    s.flags = c[2];
    s.tempC = (c[3] + 100) / 10.0f;
    s.boot = c[4];
    s.button = c[5];
    return true;
  }
  if (c[0] == 0x41 && cmdLen >= 5) {      // parameter reply (e.g. CO2 ppm)
    s.hasReg = true;
    s.reg = c[2];
    s.regValue = ((uint16_t)c[3] << 8) | c[4];
    return true;
  }
  return false;
}

inline const char* modeLower(uint8_t m) {  // lowercase, for MQTT (frame.h has an uppercase modeName)
  switch (m & 7) {
    case MODE_AUTO:   return "auto";
    case MODE_LOW:    return "low";
    case MODE_MIDDLE: return "middle";
    case MODE_HIGH:   return "high";
    case MODE_AWAY:   return "away";
    default:          return "unknown";
  }
}

// Registers worth reading (confirmed): 0x49 temp(/10 C), 0x4A CO2 ppm,
// 0x4B humidity(/100 %), 0x89 CO2 setpoint ppm.
static const uint8_t REG_TEMP = 0x49, REG_CO2 = 0x4A, REG_RH = 0x4B, REG_CO2_SETPOINT = 0x89;

}  // namespace duco
