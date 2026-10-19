// duco-rf-bridge: sniffer / learn firmware   LilyGO T3 v1.6.1 (ESP32 + SX1276, 868 MHz)
//
// Receive-only: never transmits, so it is safe to run next to a working
// DucoBox. Two jobs:
//
//  1. LEARN (step 1 of docs/getting-started.md). With your old DucoBox powered
//     on, it finds your network: the Duco network whose box (address 1) it hears
//     strongest. It measures that box's exact frequency, lists the nodes, and
//     every 60 s prints a "[suggested config]" block to paste into the bridge's
//     config.h. Press a button on each switch while it runs: the press shows up
//     as ">>> PRESS node N", which tells you which address is which room.
//
//  2. CAPTURE: prints every frame of your network in full (box messages,
//     repeats, polls, ACKs, node messages) with the time since the box's last
//     network message, RSSI, sync and AFC. That's how the protocol in
//     docs/protocol.md was worked out. Type a character in the serial monitor to
//     drop a MARK line into the log.
//
// Same receive path as the bridge: 2-byte sync D3 91, streaming capture that
// catches back-to-back frames, single-bit CRC repair.
//
// Logs can contain your network ID: run tools/sanitize_log.py before sharing one.
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "duco_frame.h"

#define FW_VERSION "0.1.0"

// --- T3 v1.6.1 LoRa pins (from LilyGO's board definitions) -----------------
#define LORA_SCK   5
#define LORA_MISO  19
#define LORA_MOSI  27
#define LORA_CS    18
#define LORA_RST   23
#define LORA_DIO0  26
#define LORA_DIO1  33

// --- Duco radio parameters ---------------------------------------------------
static const float FREQ_MHZ     = 868.2943f;  // nominal; AFC follows each transmitter's own offset
static const float BITRATE_KBPS = 38.4f;
static const float FREQ_DEV_KHZ = 20.63f;
static const float RX_BW_KHZ    = 166.7f;
static const float AFC_BW_KHZ   = 125.0f;
static uint8_t SYNC_WORD[]      = {0xD3, 0x91};
static const uint8_t CAPTURE_LEN = 255;       // RX packet length; we drain the 64-byte FIFO as it fills
static const uint8_t TAIL_BYTES  = 32;        // keep listening 6.7 ms past a frame
static const float   US_PER_BIT  = 1000.0f / BITRATE_KBPS;

// --- What to print -------------------------------------------------------------
static const uint32_t OWN_NET   = 0;           // 0 = learn it (the network whose box is strongest)
static const bool     ONLY_OWN  = true;        // print frames of your network only (summary lists all)
static const int16_t  JUNK_RSSI = -95;         // print undecodable captures stronger than this

static Module* radioMod = new Module(LORA_CS, LORA_DIO0, LORA_RST, LORA_DIO1);
SX1276 radio(radioMod);

// --- statistics ----------------------------------------------------------------
// Per network: frames, and what we heard from its box (address 1): how many
// frames, how strong, and its frequency offset (AFC, Hz) for the bridge config.
struct NetStat {
  uint32_t net, frames, boxFrames, nodes;      // nodes: bit n = address n seen as a node
  int16_t bestRssi, boxRssi;
  uint32_t lastSeen;
  double afcSum; uint32_t afcN;
};
static NetStat nets[12];
static uint8_t nNets = 0;
static uint32_t nCaptures = 0, nJunk = 0, nRepaired = 0, nOverrun = 0;

// own network, per original source: frames by type, and node messages by command
struct NodeStat { uint32_t frames, net00, ack06, msg07, cmd22, cmd41, cmd12, cmdOther, presses; int16_t rssi; };
static NodeStat NODE[32];

// the real box's network message (src 1, type 0x00): when, and how regularly
static uint32_t lastBoxHbUs = 0;
static bool haveBoxHb = false;
static uint32_t hbGapMin = UINT32_MAX, hbGapMax = 0, hbGapN = 0;
static uint64_t hbGapSum = 0;

static NetStat* findNet(uint32_t net) {
  for (uint8_t i = 0; i < nNets; i++) if (nets[i].net == net) return &nets[i];
  return nullptr;
}

// Your network: OWN_NET if set, else the one whose box we hear strongest.
static uint32_t ownNet() {
  if (OWN_NET) return OWN_NET;
  const NetStat* best = nullptr;
  for (uint8_t i = 0; i < nNets; i++)
    if (nets[i].boxFrames >= 3 && (!best || nets[i].boxRssi > best->boxRssi)) best = &nets[i];
  return best ? best->net : 0;
}

static NetStat* recordNetwork(uint32_t net, int16_t rssi) {
  NetStat* s = findNet(net);
  if (!s) {
    if (nNets >= sizeof(nets) / sizeof(nets[0])) return nullptr;
    s = &nets[nNets++];
    *s = {};
    s->net = net; s->bestRssi = s->boxRssi = -200;
    Serial.printf("*** new network %08lX ***\n", (unsigned long)net);
  }
  s->frames++; s->lastSeen = millis();
  if (rssi > s->bestRssi) s->bestRssi = rssi;
  return s;
}

// A config.h block for the bridge, from what we learned about your network.
static void printSuggestedConfig() {
  uint32_t own = ownNet();
  const NetStat* s = own ? findNet(own) : nullptr;
  if (!s) {
    Serial.println("  [suggested config] not yet: no Duco box heard (is the old box on? give it ~3 minutes)");
    return;
  }
  Serial.println("  [suggested config] paste into firmware/src/bridge/config.h:");
  Serial.printf("    #define DUCO_NET       0x%08lXUL\n", (unsigned long)s->net);
  Serial.print("    #define DUCO_NODES  {");
  bool first = true;
  for (int a = 2; a < 32; a++) {
    if (!(s->nodes & (1UL << a))) continue;
    Serial.printf("%s {%d, 'b', \"node_%d\", \"Node %d\"}", first ? "" : ", \\\n                     ", a, a, a);
    first = false;
  }
  Serial.println(" }");
  if (s->afcN)
    Serial.printf("    #define DUCO_TX_FREQ_MHZ   %.4ff   // box measured %+.1f kHz from nominal (n=%lu)\n",
                  FREQ_MHZ + s->afcSum / s->afcN / 1e6, s->afcSum / s->afcN / 1000.0, (unsigned long)s->afcN);
  Serial.println("  Rename the nodes after pressing a button on each switch (\">>> PRESS node N\" lines).");
  Serial.println("  'b' polls CO2 and humidity; change it to 'c' or 'h' once you see which one reads 0.");
}

static void printSummary() {
  uint32_t own = ownNet();
  Serial.printf("\n[summary %.0fs] captures=%lu junk=%lu repaired=%lu overrun=%lu\n", millis() / 1000.0,
                (unsigned long)nCaptures, (unsigned long)nJunk, (unsigned long)nRepaired, (unsigned long)nOverrun);
  for (uint8_t i = 0; i < nNets; i++) {
    const NetStat& s = nets[i];
    Serial.printf("  net %08lX%s: %lu frames, best %d dBm, box frames %lu (%d dBm), last %lus ago\n",
                  (unsigned long)s.net, s.net == own ? " (yours)" : "", (unsigned long)s.frames, s.bestRssi,
                  (unsigned long)s.boxFrames, s.boxFrames ? s.boxRssi : 0, (unsigned long)((millis() - s.lastSeen) / 1000));
  }
  if (hbGapN)
    Serial.printf("  box network msg interval: min %lu ms, avg %lu ms, max %lu ms (n=%lu)\n",
                  (unsigned long)(hbGapMin / 1000), (unsigned long)(hbGapSum / hbGapN / 1000),
                  (unsigned long)(hbGapMax / 1000), (unsigned long)hbGapN);
  for (int a = 1; a < 32; a++) {
    const NodeStat& s = NODE[a];
    if (!s.frames) continue;
    Serial.printf("  %s%-2d: %lu frames, %d dBm (net %lu, ack %lu, msg %lu: 0x22=%lu 0x41=%lu 0x12=%lu other=%lu) presses=%lu\n",
                  a == 1 ? "box " : "node", a, (unsigned long)s.frames, s.rssi, (unsigned long)s.net00,
                  (unsigned long)s.ack06, (unsigned long)s.msg07, (unsigned long)s.cmd22,
                  (unsigned long)s.cmd41, (unsigned long)s.cmd12, (unsigned long)s.cmdOther, (unsigned long)s.presses);
  }
  printSuggestedConfig();
  Serial.println();
}

// --- receive -------------------------------------------------------------------
// RadioLib caps the FSK packet length at the FIFO size (64), so write our longer
// capture length straight into RegPayloadLength.
static void startRx() {
  radio.fixedPacketLengthMode(64);
  radioMod->SPIwriteRegister(RADIOLIB_SX127X_REG_PAYLOAD_LENGTH_FSK, CAPTURE_LEN);
  radio.startReceive();
}

// Stream the FIFO into buf until TAIL_BYTES past the last frame in it.
// tStartUs = when the first byte came out (~ end of the sync word).
static uint8_t readCapture(uint8_t* buf, int16_t& rssi, uint32_t& tStartUs) {
  uint8_t f2 = radioMod->SPIreadRegister(RADIOLIB_SX127X_REG_IRQ_FLAGS_2);
  if (f2 & RADIOLIB_SX127X_FLAG_FIFO_OVERRUN) { nOverrun++; startRx(); return 0; }
  if (f2 & RADIOLIB_SX127X_FLAG_FIFO_EMPTY) return 0;
  tStartUs = micros();
  rssi = -(int16_t)radioMod->SPIreadRegister(RADIOLIB_SX127X_REG_RSSI_VALUE_FSK) / 2;
  size_t n = 0, need = 1;
  uint32_t tByte = micros();
  while (n < need && n < CAPTURE_LEN) {
    f2 = radioMod->SPIreadRegister(RADIOLIB_SX127X_REG_IRQ_FLAGS_2);
    if (f2 & RADIOLIB_SX127X_FLAG_FIFO_OVERRUN) { nOverrun++; break; }
    if (!(f2 & RADIOLIB_SX127X_FLAG_FIFO_EMPTY)) {
      buf[n++] = radioMod->SPIreadRegister(RADIOLIB_SX127X_REG_FIFO);
      tByte = micros();
      if (n == need) { size_t w = duco::captureNeed(buf, n, TAIL_BYTES); need = w > n ? w : n; }
    } else if (f2 & RADIOLIB_SX127X_FLAG_PAYLOAD_READY) {
      break;
    } else if (micros() - tByte > 3000) {
      break;
    }
  }
  return (uint8_t)n;
}

static bool doubledSync(const uint8_t* buf, size_t n, uint16_t bitOffset) {
  if (bitOffset == 16) return true;
  if (bitOffset < 32) return false;
  bool ok = true;
  return duco::getByteAt(buf, n * 8, bitOffset - 32, ok) == 0xD3 &&
         duco::getByteAt(buf, n * 8, bitOffset - 24, ok) == 0x91 && ok;
}

// Same as duco_box.h's firstCommand (the sniffer doesn't include the box header).
namespace duco {
inline const uint8_t* firstCommand(const Frame& f, uint8_t& cmdLen) {
  if (f.len < 12 || f.payload[0] != 0x07) return nullptr;
  uint8_t l0 = f.payload[9] >> 4;
  if (l0 == 0 || 11 + l0 > f.len) return nullptr;
  cmdLen = l0;
  return f.payload + 11;
}
}

static void countNode(const duco::Frame& f, const duco::Header& h, int16_t rssi) {
  NodeStat& s = NODE[h.osrc & 31];
  s.frames++;
  if (h.src == h.osrc) s.rssi = rssi;          // its own transmission, not a relay
  if (h.type == 0x00) s.net00++;
  else if (h.type == 0x06) s.ack06++;
  else if (h.type == 0x07) {
    s.msg07++;
    uint8_t cmdLen = 0;
    const uint8_t* c = duco::firstCommand(f, cmdLen);
    uint8_t cmd = c ? c[0] : 0;
    if (cmd == 0x22) s.cmd22++; else if (cmd == 0x41) s.cmd41++; else if (cmd == 0x12) s.cmd12++; else s.cmdOther++;
  }
}

// A button press: a 0x22 whose flags carry the 0x08 request bit. Printed loudly,
// once per press (not again for its relay), so you can map addresses to rooms.
static void notePress(const duco::Frame& f, const duco::Header& h) {
  uint8_t cmdLen = 0;
  const uint8_t* c = duco::firstCommand(f, cmdLen);
  if (!c || c[0] != 0x22 || cmdLen < 6 || !(c[2] & 0x08) || h.src != h.osrc) return;
  NODE[h.osrc & 31].presses++;
  Serial.printf("\n>>> PRESS node %u: asks for %s%s (button 0x%02X)\n\n", h.osrc, duco::modeName(c[2]),
                (c[2] & 0x20) ? " permanent" : "", c[5]);
}

static void handleFrame(const duco::Frame& f, uint32_t tUs, int16_t rssi, float afcKhz, bool primary,
                        bool dbl, int repairedBit) {
  duco::Header h = duco::header(f);
  if (!h.valid) return;
  NetStat* ns = recordNetwork(h.net, rssi);
  if (ns && h.src == 1) {                       // the box itself transmitted this
    ns->boxFrames++;
    if (rssi > ns->boxRssi) ns->boxRssi = rssi;
    if (primary) { ns->afcSum += afcKhz * 1000.0; ns->afcN++; }
  }
  if (ns) {                                     // remember node addresses (not the box, not broadcast)
    if (h.osrc >= 2) ns->nodes |= 1UL << h.osrc;
    if (h.odst >= 2) ns->nodes |= 1UL << h.odst;
  }
  uint32_t own = ownNet();
  if (h.net != own) { if (ONLY_OWN) return; }
  else { countNode(f, h, rssi); notePress(f, h); }

  bool boxHb = (h.net == own && h.src == 1 && h.type == 0x00);
  long sinceHb = haveBoxHb ? (long)((tUs - lastBoxHbUs) / 1000) : -1;
  if (boxHb) {
    if (haveBoxHb) {
      uint32_t gap = tUs - lastBoxHbUs;
      if (gap < hbGapMin) hbGapMin = gap;
      if (gap > hbGapMax) hbGapMax = gap;
      hbGapSum += gap; hbGapN++;
    }
    lastBoxHbUs = tUs; haveBoxHb = true;
  }

  static char line[700];
  duco::describe(f, line, sizeof(line));
  char afc[16] = "";
  if (primary) snprintf(afc, sizeof(afc), " afc%+.1fk", afcKhz);
  char rep[32] = "";
  if (repairedBit > 0) snprintf(rep, sizeof(rep), " [repaired bit %d]", repairedBit - 1);
  else if (repairedBit == 0) snprintf(rep, sizeof(rep), " [bad 2nd sync]");
  Serial.printf("[%9.3fs hb%+5ldms] %4ddBm %s%s%s%s %s\n", tUs / 1e6, boxHb ? 0L : sinceHb, rssi,
                dbl ? "2x" : "1x", afc, rep, boxHb ? "  <<< BOX NETWORK MSG" : "", line);
}


static void serviceRx() {
  uint8_t buf[CAPTURE_LEN]; int16_t rssi = 0; uint32_t tStartUs = 0;
  uint8_t n = readCapture(buf, rssi, tStartUs);
  if (!n) return;
  float afcKhz = radio.getAFCError() / 1000.0f;   // offset of the transmitter the radio locked onto
  startRx();
  nCaptures++;

  duco::Found found[8];
  int nf = duco::scan(buf, n, found, 8);
  int repaired = -1;
  if (nf == 0 && n > 1) {
    repaired = duco::repairFrame(buf, n, buf[0] == 0xD3 ? 2 : 0, found[0]);
    if (repaired >= 0) { nf = 1; nRepaired++; }
  }
  if (nf == 0) {
    if (n > 1) {
      nJunk++;
      if (rssi > JUNK_RSSI) {
        Serial.printf("[%9.3fs] %4ddBm no valid frame in %u bytes:", tStartUs / 1e6, rssi, n);
        for (int i = 0; i < n && i < 72; i++) Serial.printf(" %02x", buf[i]);
        Serial.println();
      }
    }
    return;
  }
  for (int i = 0; i < nf; i++) {
    // when this frame's length byte went by: capture start + its bit offset
    uint32_t tUs = tStartUs + (uint32_t)(found[i].bitOffset * US_PER_BIT);
    bool primary = found[i].bitOffset <= 16;
    handleFrame(found[i].f, tUs, rssi, afcKhz, primary, doubledSync(buf, n, found[i].bitOffset),
                i == 0 ? repaired : -1);
  }
}


static void check(int16_t state, const char* what) {
  if (state == RADIOLIB_ERR_NONE) return;
  Serial.printf("ERROR: %s failed, code %d\n", what, state);
  Serial.println("Check the board/pins (is this the 868 MHz T3 v1.6.1?). Halting.");
  while (true) delay(1000);
}

void setup() {
  Serial.setTxBufferSize(8192);   // printing must never stall the receive loop
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== duco-rf-bridge " FW_VERSION " (sniffer / learn, receive only) ===");

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  check(radio.beginFSK(FREQ_MHZ, BITRATE_KBPS, FREQ_DEV_KHZ, RX_BW_KHZ, 10, 32, false), "beginFSK");
  check(radio.setSyncWord(SYNC_WORD, sizeof(SYNC_WORD)), "setSyncWord");
  check(radio.setEncoding(RADIOLIB_ENCODING_NRZ), "setEncoding (no whitening)");
  check(radio.setDataShaping(RADIOLIB_SHAPING_0_5), "setDataShaping");
  check(radio.setCRC(false), "setCRC (checked in software)");
  // AFC on: each node transmits a few kHz off its own way.
  check(radio.setAFCBandwidth(AFC_BW_KHZ), "setAFCBandwidth");
  check(radio.setAFC(true), "setAFC");
  check(radio.setAFCAGCTrigger(RADIOLIB_SX127X_RX_TRIGGER_PREAMBLE_DETECT), "setAFCAGCTrigger");
  startRx();

  Serial.printf("Listening on %.4f MHz, %.1f kbps GFSK, 2-byte sync. %s\n", FREQ_MHZ, BITRATE_KBPS,
                OWN_NET ? "Own network set in OWN_NET." : "Learning your network (old box must be ON).");
  Serial.println("Line format: [time  hb+ms since the box's last network msg] RSSI sync afc frame...");
  Serial.println("Type a character (1, 2, 3 ...) to put a marker in the log before each button press.\n");
}

void loop() {
  static uint32_t lastSummary = 0;
  serviceRx();
  while (Serial.available()) {
    int c = Serial.read();
    if (c >= ' ' && c <= '~')
      Serial.printf("\n[%9.3fs] ======== MARK '%c' ========\n\n", micros() / 1e6, (char)c);
  }
  if (millis() - lastSummary > 60000) { lastSummary = millis(); printSummary(); }
}
