// Duco RF frame parsing - plain C++11, no Arduino dependencies (host-testable).
//
// On air (CC1101): preamble | sync D391 (often twice) | len | payload[len] | CRC16
// CRC-16 poly 0x8005, init 0xFFFF over len + payload: CONFIRMED on real captures.
//
// Payload layout. This differs from the byte-per-field table in
// arnemauer/Duco-RF-protocol docs/02 and is derived from real captures
// (fits every valid frame seen so far):
//   [0]      type
//   [1..4]   network ID (= DucoBox device ID)
//   [5..7]   24 bits: src(5) dst(5) orig.src(5) orig.dst(5) counter(4), MSB first
//   [8]      rssi field (0xFF unless a repeater/network message fills it in)
//   [9..10]  command lengths, one nibble each for up to 4 commands (high nibble first)
//   [11..]   commands, each starting with a command byte
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>

namespace duco {

// Longest frame we accept. Was 32 (arnemauer's CC1101 PKTLEN 0x20), but captures
// keep starting with length 0x22 / 0x33, so allow up to what a 64-byte CC1101
// FIFO can hold (len + 60 + 2 CRC + 1 spare). The CRC still rejects noise.
static const uint8_t MAX_LEN = 60;

inline uint16_t crc16(const uint8_t* d, size_t n, uint16_t crc = 0xFFFF) {
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x8005) : (uint16_t)(crc << 1);
  }
  return crc;
}

struct Frame {
  const uint8_t* payload;  // points into the capture buffer
  uint8_t len;
  bool crcOk;
  bool skippedSecondSync;
};

struct Header {
  bool valid;
  uint8_t type;
  uint32_t net;
  uint8_t src, dst, osrc, odst, counter, rssi;
};

// Parse a fixed-length capture that starts right after the first sync word.
inline bool parse(const uint8_t* buf, size_t n, Frame& f) {
  f.skippedSecondSync = false;
  if (n >= 2 && buf[0] == 0xD3 && buf[1] == 0x91) {  // sync word repeated twice
    buf += 2; n -= 2;
    f.skippedSecondSync = true;
  }
  if (n < 3) return false;
  uint8_t len = buf[0];
  if (len < 1 || len > MAX_LEN || n < (size_t)len + 3) return false;
  uint16_t rx = ((uint16_t)buf[1 + len] << 8) | buf[2 + len];
  f.len = len;
  f.payload = buf + 1;
  f.crcOk = (crc16(buf, (size_t)len + 1) == rx);
  return true;
}

// ---- bit-level scan of a whole capture ---------------------------------------
// A sensor replies within milliseconds of the box's message, while the radio is
// still filling the fixed-length capture that the box frame started. Those
// replies sit somewhere in the capture's tail, not byte-aligned. scan() finds
// every CRC-valid frame in the capture: one right at the start (the frame whose
// sync word the radio matched) plus any frame after a D391 sync anywhere later.
struct Found {
  uint8_t data[MAX_LEN + 3];  // len, payload, crc (copied, bit-aligned)
  Frame f;
  uint16_t bitOffset;         // where the length byte started in the capture
};

inline uint8_t getByteAt(const uint8_t* buf, size_t nbits, size_t bit, bool& ok) {
  if (bit + 8 > nbits) { ok = false; return 0; }
  uint8_t v = 0;
  for (int i = 0; i < 8; i++) {
    size_t b = bit + i;
    v = (uint8_t)((v << 1) | ((buf[b >> 3] >> (7 - (b & 7))) & 1));
  }
  return v;
}

inline bool tryFrameAt(const uint8_t* buf, size_t nbits, size_t bit, Found& out) {
  bool ok = true;
  uint8_t len = getByteAt(buf, nbits, bit, ok);
  if (!ok || len < 9 || len > MAX_LEN) return false;  // 9 = shortest Duco frame (ACK)
  out.data[0] = len;
  for (uint8_t i = 1; i < len + 3; i++) {
    out.data[i] = getByteAt(buf, nbits, bit + 8 * (size_t)i, ok);
    if (!ok) return false;
  }
  uint16_t rx = ((uint16_t)out.data[1 + len] << 8) | out.data[2 + len];
  out.f.len = len;
  out.f.payload = out.data + 1;
  out.f.crcOk = (crc16(out.data, (size_t)len + 1) == rx);
  out.f.skippedSecondSync = false;
  out.bitOffset = (uint16_t)bit;
  return out.f.crcOk;
}

// Returns the number of valid frames written to out (max maxOut).
inline int scan(const uint8_t* buf, size_t n, Found* out, int maxOut) {
  size_t nbits = n * 8;
  int count = 0;
  size_t nextFree = 0;  // don't report overlapping frames twice
  // candidate 0: frame starts right after the sync the radio matched
  if (count < maxOut && tryFrameAt(buf, nbits, 0, out[count])) {
    nextFree = 8 * (size_t)(out[count].f.len + 3);
    count++;
  }
  // every later position directly after a D391 sync word
  uint16_t window = 0;
  for (size_t b = 0; b < nbits && count < maxOut; b++) {
    window = (uint16_t)((window << 1) | ((buf[b >> 3] >> (7 - (b & 7))) & 1));
    if (b < 15 || window != 0xD391) continue;
    size_t start = b + 1;
    if (start < nextFree) continue;
    if (tryFrameAt(buf, nbits, start, out[count])) {
      out[count].f.skippedSecondSync = (start == 16);
      nextFree = start + 8 * (size_t)(out[count].f.len + 3);
      count++;
    }
  }
  return count;
}

// ---- streaming capture length ---------------------------------------------------
// Nodes answer back to back (heartbeat repeat, then their own message), with a
// gap shorter than the time it takes to restart the receiver. So the RX path
// does not stop at the end of the first frame: it keeps the capture running for
// tailBytes more, and if a D391 sync shows up in that tail, extends the capture
// to cover the frame behind it (then another tail, and so on). scan() then finds
// them all.
// Given the first n bytes of a capture, returns how many bytes it should hold.
// The radio matches a single D391, so a doubled sync leaves D3 91 at the start.
// Returns n when there is no plausible length byte (false sync: stop now).
inline size_t captureNeed(const uint8_t* buf, size_t n, size_t tailBytes) {
  if (n < 1) return 1;
  size_t first = 0;
  if (buf[0] == 0xD3) {                     // 0xD3 is never a length: second sync word
    if (n < 3) return 3;                    // (its 0x91 may arrive with a bit error: D3 80,
    first = 2;                              //  D3 FF seen at -63 dBm, so don't insist on it)
  }
  if (buf[first] < 9 || buf[first] > MAX_LEN) return n;
  size_t nbits = n * 8;
  size_t end = 8 * (first + (size_t)buf[first] + 3);   // bit where the last known frame ends
  size_t need = 0;                          // > 0 while a tail frame's len byte is pending
  uint16_t window = 0;
  for (size_t b = 0; b < nbits; b++) {
    window = (uint16_t)((window << 1) | ((buf[b >> 3] >> (7 - (b & 7))) & 1));
    if (b < 15 || window != 0xD391) continue;
    size_t start = b + 1;
    if (start < end) continue;              // sync-like bits inside a frame
    bool ok = true;
    uint8_t len = getByteAt(buf, nbits, start, ok);
    if (!ok) { need = (start + 8 + 7) / 8; break; }   // wait for its len byte
    if (len < 9 || len > MAX_LEN) continue;            // e.g. first half of a doubled sync
    end = start + 8 * ((size_t)len + 3);
  }
  size_t want = (end + 7) / 8 + tailBytes;
  return need > want ? need : want;
}

// ---- single-bit repair --------------------------------------------------------
// Strong frames (-61..-64 dBm) still arrive with one flipped bit now and then
// (e.g. a network ID byte off by one bit, or the second sync word mangled to
// D3 80 / D3 FF), and the CRC throws them away. CRC-16 (0x8005) catches every
// 2-bit error in frames this short, so a frame that one flipped bit makes valid
// is unambiguous; the caller still checks the network ID before trusting it.
// Takes the frame whose length byte is at byte `at` of a capture.
// Returns -1 if it can't be made valid, 0 if the CRC was fine as is, or
// 1 + the index of the bit (in len|payload|crc) that had to be flipped.
inline int repairFrame(const uint8_t* buf, size_t n, size_t at, Found& out) {
  if (at >= n) return -1;
  uint8_t len = buf[at];
  if (len < 9 || len > MAX_LEN || at + (size_t)len + 3 > n) return -1;
  for (size_t i = 0; i < (size_t)len + 3; i++) out.data[i] = buf[at + i];
  out.f.len = len; out.f.payload = out.data + 1; out.f.crcOk = true;
  out.f.skippedSecondSync = (at == 2);
  out.bitOffset = (uint16_t)(at * 8);
  auto ok = [&]() {
    uint16_t rx = ((uint16_t)out.data[1 + len] << 8) | out.data[2 + len];
    return crc16(out.data, (size_t)len + 1) == rx;
  };
  if (ok()) return 0;
  for (size_t bit = 8; bit < 8 * ((size_t)len + 3); bit++) {   // the length byte itself stays
    out.data[bit >> 3] ^= (uint8_t)(0x80 >> (bit & 7));
    if (ok()) return 1 + (int)bit;
    out.data[bit >> 3] ^= (uint8_t)(0x80 >> (bit & 7));
  }
  out.f.crcOk = false;
  return -1;
}

inline Header header(const Frame& f) {
  Header h = {};
  const uint8_t* p = f.payload;
  if (f.len < 9) return h;
  h.valid = true;
  h.type = p[0];
  h.net = ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | p[4];
  uint32_t a = ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
  h.src = (a >> 19) & 31; h.dst = (a >> 14) & 31;
  h.osrc = (a >> 9) & 31; h.odst = (a >> 4) & 31;
  h.counter = a & 15;
  h.rssi = p[8];
  return h;
}

inline const char* msgType(uint8_t t) {
  switch (t) {
    case 0x00: return "network msg";
    case 0x01: return "join request";
    case 0x02: return "assign net ID";
    case 0x03: return "confirm net ID";
    case 0x04: return "assign address";
    case 0x06: return "ACK";
    case 0x07: return "message";
    default:   return "?";
  }
}

inline const char* cmdName(uint8_t c) {
  switch (c) {
    case 0x01: return "node reset";        case 0x02: return "node save";
    case 0x12: return "confirm vent mode"; case 0x22: return "vent mode";
    case 0x3B: return "disjoin";           case 0x40: return "read param";
    case 0x41: return "param reply";       case 0x42: return "param missing";
    case 0x48: return "write param";       case 0x50: return "LED off?";
    case 0x51: return "LED on?";           case 0x52: return "unknown (periodic?)";
    case 0x61: return "firmware packet";
    default:   return "?";
  }
}

inline const char* regName(uint8_t r) {
  switch (r) {
    case 0x49: return "temperature";
    case 0x4A: return "CO2";
    case 0x4B: return "humidity";
    case 0x89: return "CO2 setpoint";
    default:   return "";
  }
}

struct Writer {  // tiny append-only formatter
  char* out; size_t cap; size_t w;
  void operator()(const char* fmt, ...) {
    if (w >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out + w, cap - w, fmt, ap);
    va_end(ap);
    if (r > 0) w += (size_t)r;
  }
};

inline const char* modeName(uint8_t m) {
  switch (m & 7) {
    case 0: return "AUTO"; case 4: return "LOW"; case 5: return "MIDDLE";
    case 6: return "HIGH"; case 7: return "AWAY"; default: return "?";
  }
}

inline void describeCommand(Writer& put, const uint8_t* c, uint8_t n) {
  put("\n      cmd 0x%02X %-19s", c[0], cmdName(c[0]));
  for (uint8_t i = 1; i < n; i++) put(" %02x", c[i]);
  // 0x12 <mode>: low 3 bits = mode, bit 3 = permanent (seen on a long press)
  if (c[0] == 0x12 && n >= 2)
    put("   mode %s%s", modeName(c[1]), (c[1] & 0x08) ? " (permanent)" : "");
  // 0x22 <pct> <flags> <temp> <boot> <button>: flags low 3 bits = mode, 0x08 =
  // permanent, high bits unknown; temp = (byte + 100) / 10. Button bits unknown.
  if (c[0] == 0x22 && n >= 6)
    put("   pct=%u flags=0x%02X (mode %s%s, hi=0x%X) temp=%.1fC boot=0x%02X button=0x%02X",
        c[1], c[2], modeName(c[2]), (c[2] & 0x08) ? " perm" : "", c[2] >> 4,
        (c[3] + 100) / 10.0, c[4], c[5]);
  if ((c[0] == 0x40 || c[0] == 0x41 || c[0] == 0x42 || c[0] == 0x48) && n >= 3) {
    put("   reg 0x%02X %s", c[2], regName(c[2]));
    if ((c[0] == 0x41 || c[0] == 0x48) && n >= 5) {
      unsigned v = ((unsigned)c[3] << 8) | c[4];
      put(" = %u", v);
      if (c[2] == 0x49) put(" (%.1f C)", v / 10.0);
      else if (c[2] == 0x4A || c[2] == 0x89) put(" ppm");
      else if (c[2] == 0x4B) put(" (%.2f %%)", v / 100.0);
    }
  }
}

// Human-readable description (always NUL-terminated).
inline void describe(const Frame& f, char* out, size_t cap) {
  if (cap) out[0] = 0;
  Writer put = {out, cap, 0};
  const uint8_t* p = f.payload;
  put("len=%2u CRC %s ", f.len, f.crcOk ? "OK " : "BAD");
  for (uint8_t i = 0; i < f.len; i++) put(" %02x", p[i]);
  Header h = header(f);
  if (!h.valid) return;
  put("\n    %s net=%08lX  %u->%u", msgType(h.type), (unsigned long)h.net, h.src, h.dst);
  if (h.osrc != h.src || h.odst != h.dst) put(" (orig %u->%u, via repeater)", h.osrc, h.odst);
  put("  ctr=%u", h.counter);
  if (h.rssi != 0xFF) put("  rssi-field=0x%02X", h.rssi);
  if (h.src == 1) put("  [from box]");
  if (h.dst == 0) put("  [broadcast]");

  // command block: 2 bytes of nibble lengths, then the commands
  if (f.len >= 11) {
    uint8_t lens[4] = {(uint8_t)(p[9] >> 4), (uint8_t)(p[9] & 15), (uint8_t)(p[10] >> 4), (uint8_t)(p[10] & 15)};
    unsigned total = lens[0] + lens[1] + lens[2] + lens[3];
    if (total == (unsigned)(f.len - 11)) {
      const uint8_t* c = p + 11;
      for (int i = 0; i < 4; i++) {
        if (lens[i]) describeCommand(put, c, lens[i]);
        c += lens[i];
      }
      return;
    }
  }
  if (f.len > 9) {
    put("\n      payload:");
    for (uint8_t i = 9; i < f.len; i++) put(" %02x", p[i]);
  }
}

}  // namespace duco
