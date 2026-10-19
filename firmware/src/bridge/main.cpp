// ============================================================================
// duco-rf-bridge: bridge firmware   LilyGO T3 v1.6.1 (ESP32 + SX1276, 868 MHz)
// ============================================================================
//
// The T3 takes over the RF side of a DucoBox mainboard: it acts as the box on
// your Duco RF network (DUCO_NET in config.h) and publishes every node's state
// to MQTT with Home Assistant auto-discovery.
//
//   >>> The old Duco box must stay OFF. Two boxes on one network ID clash. <<<
//
// It behaves like the real box, as recorded in a reference capture
// (docs/protocol.md):
//   - no 1 s heartbeat: a broadcast 0x52 + network message only every ~82 s.
//     The old 1 s heartbeat had every node repeating it all the time, and they
//     then never sent their own messages (button presses, register replies).
//   - one directed message at a time (0x52 keep-alive, 0x40 register read, 0x12
//     confirm), resent every 300 ms until the node ACKs it
//   - routes: a node that talks to us through a repeater is addressed through it
//   - a press (0x22 with the 0x08 request flag): ACK at once, then ~170 ms
//     later 0x12 <mode> to the requester, then to every other node
//
// Radio: DUCO_TX_FREQ_MHZ (your box's measured frequency, see the sniffer), sync
// D3 91 (TX sends it doubled), GFSK 38.4 kbps, CRC in software. Frame builders
// are checked at boot against the layout of frames a real box sent.
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <RadioLib.h>
#include <Preferences.h>
#include "config.h"
#include "duco_box.h"

#define FW_VERSION "0.1.0"

#define LORA_SCK 5
#define LORA_MISO 19
#define LORA_MOSI 27
#define LORA_CS 18
#define LORA_RST 23
#define LORA_DIO0 26
#define LORA_DIO1 33

static const float    BITRATE_KBPS = 38.4f;
static const float    FREQ_DEV_KHZ = 20.63f;
static const float    RX_BW_KHZ    = 166.7f;
static const float    AFC_BW_KHZ   = 125.0f;
static const uint8_t  CAPTURE_LEN  = 255;  // RX packet length (we drain the 64-byte FIFO as it fills)
static const uint8_t  TAIL_BYTES   = 32;   // keep listening 6.7 ms past a frame for one sent back to back
// Timing, from the reference capture of the real box.
static const uint32_t NETMSG_MS        = 82000;  // broadcast 0x52 + network message
static const uint32_t POLL_GAP_MS      = 1500;   // between two poll items (when nothing else is queued)
static const uint32_t RETRY_MS         = 300;    // resend a directed message until it is ACKed
static const uint8_t  MAX_TRIES        = 6;
static const uint32_t CONFIRM_DELAY_MS = 170;    // press ACKed -> first 0x12
// RX matches ONE D391; TX sends it doubled by putting the second D391 at the
// start of the packet.
static uint8_t SYNC_WORD[] = {0xD3, 0x91};

static const bool LOG_RX = true;    // log received frames (set false once happy)
static const bool LOG_TX = true;    // log every directed message we send

// kind = sensor: 'c' CO2, 'h' humidity, 'b' both; id = MQTT/HA slug, name = display name
struct NodeCfg { uint8_t addr; char kind; const char* id; const char* name; };
static bool hasCo2(const NodeCfg& n) { return n.kind == 'c' || n.kind == 'b'; }
static bool hasRh(const NodeCfg& n)  { return n.kind == 'h' || n.kind == 'b'; }
static NodeCfg NODES[] = DUCO_NODES;
static const int N_NODES = sizeof(NODES) / sizeof(NODES[0]);

// Accumulated state per node, so every MQTT publish carries ALL known fields
// (publishing only the newest field would blank the others in HA).
struct NodeState {
  bool haveTemp = false;   float   tempC = 0;
  bool haveDemand = false; uint8_t demand = 0;
  bool haveMode = false;   uint8_t mode = 0;
  bool haveCo2 = false;    uint16_t co2 = 0;
  bool haveRh = false;     float   rh = 0;
};
static NodeState STATE[8];

// Flat poll schedule: for each node a 0x52 keep-alive + the registers it has.
struct PollItem { uint8_t node; uint8_t action; };  // action 0x52, or a register number
static PollItem POLL[8 * 3];
static int nPoll = 0, pollIdx = 0;

// The repeater each node talks to us through (0 = direct), learned from its frames.
static uint8_t ROUTE[32];

// Directed messages: one in flight at a time, resent until ACKed. Confirms
// (0x12) wait in a queue and go before any poll.
struct TxMsg { uint8_t node; uint8_t n; uint8_t cmd[3]; };
static TxMsg CONFIRMQ[16];
static int cqHead = 0, cqCount = 0;
struct InFlight { bool active; TxMsg m; uint8_t ctr, tries; uint32_t lastTx; };
static InFlight cur = {};
static uint32_t notBefore = 0, lastPollStart = 0;

// Keep our own Module handle: the RX path reads the FIFO registers directly.
static Module* radioMod = new Module(LORA_CS, LORA_DIO0, LORA_RST, LORA_DIO1);
SX1276 radio(radioMod);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// Per-address RX counters (by the frame's src, i.e. who actually transmitted).
struct RxStat { uint16_t frames, singleSync; int16_t lastRssi; };
static RxStat RXSTAT[32];
static const uint32_t STATS_MS = 30000;
static uint16_t rxJunk = 0, rxOverrun = 0, rxRepaired = 0;
static uint16_t txSent = 0, txAcked = 0, txGaveUp = 0;

static uint8_t txCtr = 0;
static uint32_t lastNetMsg = 0, lastWifiTry = 0, lastStats = 0;
static bool netMsgSent = false, discoverySent = false;
static uint8_t systemMode = duco::MODE_AUTO;
static bool    systemPerm = false;    // permanent (long press): 0x08 in our 0x12 confirms
// Every 0x22 since boot, so a log that starts late still shows whether presses arrived.
static uint16_t n22 = 0; static uint8_t last22Node = 0; static uint32_t last22At = 0;

static const char* BOX_TOPIC     = "duco/box";            // {"mode":..,"source":..}
static const char* BOX_SET_TOPIC = "duco/box/mode/set";   // HA select writes auto/low/middle/high/away
static const char* EVENT_TOPIC   = "duco/debug/event";    // latest event line (0x22 / mode change), retained

// The serial monitor tends to attach after boot, so keep the important events in
// RAM and print them again with every [stats] line (and publish the newest to MQTT).
static const int N_EVENTS = 16;
static char EVENTS[N_EVENTS][280];
static int nEvents = 0;               // total ever logged; ring index = nEvents % N_EVENTS
static void eventLog(const char* fmt, ...) {
  char* e = EVENTS[nEvents % N_EVENTS];
  int w = snprintf(e, sizeof(EVENTS[0]), "[%8.1fs] ", millis() / 1000.0);
  va_list ap; va_start(ap, fmt);
  vsnprintf(e + w, sizeof(EVENTS[0]) - w, fmt, ap);
  va_end(ap);
  nEvents++;
  Serial.println(e);
  if (mqtt.connected()) mqtt.publish(EVENT_TOPIC, (const uint8_t*)e, strlen(e), true);
}
static void printEvents() {
  int first = nEvents > N_EVENTS ? nEvents - N_EVENTS : 0;
  if (nEvents == 0) return;
  Serial.printf("    [events] %d since boot, last %d:\n", nEvents, nEvents - first);
  for (int i = first; i < nEvents; i++) Serial.printf("      %s\n", EVENTS[i % N_EVENTS]);
}

static int nodeIndex(uint8_t addr) { for (int i = 0; i < N_NODES; i++) if (NODES[i].addr == addr) return i; return -1; }
static const char* nodeName(uint8_t addr) { int i = nodeIndex(addr); return i >= 0 ? NODES[i].name : "(not in DUCO_NODES)"; }

// RadioLib caps the FSK packet length at the FIFO size (64), so write our longer
// capture length straight into RegPayloadLength.
static void startRx() {
  radio.fixedPacketLengthMode(64);
  radioMod->SPIwriteRegister(RADIOLIB_SX127X_REG_PAYLOAD_LENGTH_FSK, CAPTURE_LEN);
  radio.startReceive();
}
static void txFrame(const uint8_t* f, uint8_t len) {
  uint8_t out[2 + duco::MAX_LEN + 3] = {0xD3, 0x91};   // second sync word, then the frame
  memcpy(out + 2, f, len);
  radio.standby(); radio.fixedPacketLengthMode(len + 2);
  radio.transmit(out, len + 2); startRx();
}

static void buildPollSchedule() {
  for (int i = 0; i < N_NODES; i++) {
    POLL[nPoll++] = {NODES[i].addr, 0x52};              // keep-alive, as the real box does
    if (hasCo2(NODES[i])) POLL[nPoll++] = {NODES[i].addr, duco::REG_CO2};
    if (hasRh(NODES[i]))  POLL[nPoll++] = {NODES[i].addr, duco::REG_RH};
  }
}

// ---- MQTT ------------------------------------------------------------------
static void addDev(JsonDocument& doc) {
  JsonObject dev = doc["dev"].to<JsonObject>();
  dev["ids"][0] = "duco_box_t3"; dev["name"] = "Duco Ventilation"; dev["mdl"] = "T3 RF bridge";
}
static void pubDiscovery1(const char* uid, const char* name, const char* stateTopic,
                          const char* tmpl, const char* unit, const char* devclass) {
  char cfg[112]; snprintf(cfg, sizeof(cfg), "homeassistant/sensor/%s/config", uid);
  JsonDocument doc;
  doc["name"] = name; doc["uniq_id"] = uid; doc["stat_t"] = stateTopic; doc["val_tpl"] = tmpl;
  if (unit && unit[0]) doc["unit_of_meas"] = unit;
  if (devclass && devclass[0]) doc["dev_cla"] = devclass;
  addDev(doc);
  char buf[512]; size_t n = serializeJson(doc, buf);
  mqtt.publish(cfg, (const uint8_t*)buf, n, true);
}

static void publishDiscovery() {
  for (int i = 0; i < N_NODES; i++) {
    const NodeCfg& nd = NODES[i];
    char st[64]; snprintf(st, sizeof(st), "duco/%s", nd.id);
    char uid[64], nm[64];
    snprintf(uid, sizeof(uid), "duco_%s_temp", nd.id);   snprintf(nm, sizeof(nm), "%s temperature", nd.name);
    pubDiscovery1(uid, nm, st, "{{ value_json.temp_c }}", "°C", "temperature");
    snprintf(uid, sizeof(uid), "duco_%s_demand", nd.id); snprintf(nm, sizeof(nm), "%s demand", nd.name);
    pubDiscovery1(uid, nm, st, "{{ value_json.demand }}", "%", "");
    snprintf(uid, sizeof(uid), "duco_%s_mode", nd.id);   snprintf(nm, sizeof(nm), "%s mode", nd.name);
    pubDiscovery1(uid, nm, st, "{{ value_json.mode }}", "", "");
    // A sensor the node doesn't have gets its retained config cleared, so HA
    // drops e.g. a "Bathroom CO2" that would only ever read 0.
    char cfg[112];
    snprintf(uid, sizeof(uid), "duco_%s_co2", nd.id);    snprintf(nm, sizeof(nm), "%s CO2", nd.name);
    if (hasCo2(nd)) pubDiscovery1(uid, nm, st, "{{ value_json.co2_ppm }}", "ppm", "carbon_dioxide");
    else { snprintf(cfg, sizeof(cfg), "homeassistant/sensor/%s/config", uid); mqtt.publish(cfg, (const uint8_t*)"", 0, true); }
    snprintf(uid, sizeof(uid), "duco_%s_rh", nd.id);     snprintf(nm, sizeof(nm), "%s humidity", nd.name);
    if (hasRh(nd)) pubDiscovery1(uid, nm, st, "{{ value_json.rh_pct }}", "%", "humidity");
    else { snprintf(cfg, sizeof(cfg), "homeassistant/sensor/%s/config", uid); mqtt.publish(cfg, (const uint8_t*)"", 0, true); }
  }
  // The box's own ventilation mode: what the T3 confirms to the nodes. A
  // select, so HA shows it AND can set it.
  JsonDocument doc;
  doc["name"] = "Box mode"; doc["uniq_id"] = "duco_box_mode";
  doc["stat_t"] = BOX_TOPIC; doc["val_tpl"] = "{{ value_json.mode }}"; doc["cmd_t"] = BOX_SET_TOPIC;
  JsonArray ops = doc["ops"].to<JsonArray>();
  for (const char* o : {"auto", "low", "middle", "high", "away"}) ops.add(o);
  addDev(doc);
  char buf[512]; size_t n = serializeJson(doc, buf);
  mqtt.publish("homeassistant/select/duco_box_mode/config", (const uint8_t*)buf, n, true);
  Serial.println("MQTT discovery published.");
}

static void publishBox(const char* source) {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["mode"] = duco::modeLower(systemMode); doc["source"] = source;
  doc["permanent"] = systemPerm;
  char buf[128]; size_t n = serializeJson(doc, buf);
  mqtt.publish(BOX_TOPIC, (const uint8_t*)buf, n, true);
}

static void publishState(int idx) {
  const NodeState& s = STATE[idx];
  char topic[64]; snprintf(topic, sizeof(topic), "duco/%s", NODES[idx].id);
  JsonDocument doc;
  if (s.haveTemp)   doc["temp_c"]  = s.tempC;
  if (s.haveDemand) doc["demand"]  = s.demand;
  if (s.haveMode)   doc["mode"]    = duco::modeLower(s.mode);
  if (s.haveCo2)    doc["co2_ppm"] = s.co2;
  if (s.haveRh)     doc["rh_pct"]  = s.rh;
  char buf[256]; size_t n = serializeJson(doc, buf);
  mqtt.publish(topic, (const uint8_t*)buf, n, true);
}

// ---- sending -----------------------------------------------------------------
static uint8_t nextCtr() { uint8_t c = txCtr; txCtr = (txCtr + 1) & 0x0F; return c; }

static void transmitCur() {
  uint8_t f[40];
  uint8_t via = ROUTE[cur.m.node & 31];
  uint8_t len = duco::buildMessage(f, DUCO_NET, cur.m.node, cur.ctr, cur.m.cmd, cur.m.n, via);
  txFrame(f, len);
  cur.tries++; cur.lastTx = millis(); txSent++;
  if (LOG_TX) {
    Serial.printf("[%8.1fs] [tx] cmd %02x", millis() / 1000.0, cur.m.cmd[0]);
    for (uint8_t i = 1; i < cur.m.n; i++) Serial.printf(" %02x", cur.m.cmd[i]);
    Serial.printf(" -> node %u %s", cur.m.node, nodeName(cur.m.node));
    if (via) Serial.printf(" via %u", via);
    Serial.printf("  ctr=%u try %u\n", cur.ctr, cur.tries);
  }
}

static void startMsg(const TxMsg& m) {
  cur.active = true; cur.m = m; cur.ctr = nextCtr(); cur.tries = 0;
  transmitCur();
}

// The in-flight message was ACKed by its node.
static void onAcked() {
  txAcked++;
  cur.active = false;
  if (cur.m.cmd[0] == 0x12) {           // the node latched this mode
    int idx = nodeIndex(cur.m.node);
    if (idx >= 0) { STATE[idx].haveMode = true; STATE[idx].mode = cur.m.cmd[1] & 7; publishState(idx); }
    eventLog("node %u %s ACKed confirm %s%s (try %u)", cur.m.node, nodeName(cur.m.node),
             duco::modeLower(cur.m.cmd[1]), (cur.m.cmd[1] & 0x08) ? " perm" : "", cur.tries);
  }
}

static void queueConfirm(uint8_t node) {
  if (cqCount >= (int)(sizeof(CONFIRMQ) / sizeof(CONFIRMQ[0]))) return;
  TxMsg& m = CONFIRMQ[(cqHead + cqCount++) % (sizeof(CONFIRMQ) / sizeof(CONFIRMQ[0]))];
  m.node = node; m.n = 2; m.cmd[0] = 0x12; m.cmd[1] = systemMode | (systemPerm ? 0x08 : 0);
}

// Every ~82 s, like the real box: a broadcast keep-alive and a network message
// (payload 00). Nodes relay both; neither is ACKed.
static void sendNetworkMsg() {
  uint8_t f[40];
  txFrame(f, duco::buildPoll52(f, DUCO_NET, 0, nextCtr()));
  delay(40);
  txFrame(f, duco::buildHeartbeat(f, DUCO_NET, nextCtr(), 0x00));
  if (LOG_TX) Serial.printf("[%8.1fs] [tx] broadcast 52 00 + network message\n", millis() / 1000.0);
}

static void serviceTx() {
  uint32_t now = millis();
  if (cur.active) {
    if (now - cur.lastTx < RETRY_MS) return;
    if (cur.tries < MAX_TRIES) { transmitCur(); return; }
    txGaveUp++;
    eventLog("no ACK from node %u %s for cmd %02x %02x after %u tries", cur.m.node, nodeName(cur.m.node),
             cur.m.cmd[0], cur.m.cmd[1], cur.tries);
    cur.active = false;
    return;
  }
  if ((int32_t)(now - notBefore) < 0) return;
  if (!netMsgSent || now - lastNetMsg >= NETMSG_MS) {
    netMsgSent = true; lastNetMsg = now;
    sendNetworkMsg();
    notBefore = now + 300;              // let the nodes relay it
    return;
  }
  if (cqCount) {
    TxMsg m = CONFIRMQ[cqHead];
    cqHead = (cqHead + 1) % (sizeof(CONFIRMQ) / sizeof(CONFIRMQ[0])); cqCount--;
    startMsg(m);
    return;
  }
  if (nPoll && now - lastPollStart >= POLL_GAP_MS) {
    lastPollStart = now;
    PollItem& p = POLL[pollIdx];
    pollIdx = (pollIdx + 1) % nPoll;
    TxMsg m;
    m.node = p.node;
    if (p.action == 0x52) { m.n = 2; m.cmd[0] = 0x52; m.cmd[1] = 0x00; }
    else                  { m.n = 3; m.cmd[0] = 0x40; m.cmd[1] = 0x00; m.cmd[2] = p.action; }
    startMsg(m);
  }
}

// ---- box mode ------------------------------------------------------------------
// The box mode survives a reboot, so a restart doesn't undo the user's setting.
static Preferences prefs;
static void saveMode() {
  if (prefs.getUChar("mode", 0xFF) != systemMode) prefs.putUChar("mode", systemMode);
  if (prefs.getBool("perm", false) != systemPerm) prefs.putBool("perm", systemPerm);
}

// Adopt a mode and confirm it (0x12) to every node, the requester first, each
// resent until ACKed: what the real box does after a press. A new mode replaces
// any confirms still queued for an older one.
static void setSystemMode(uint8_t mode, bool perm, const char* source, uint8_t requester = 0) {
  systemMode = mode & 7; systemPerm = perm;
  saveMode();
  eventLog(">> box mode -> %s%s (from %s); confirming to all nodes",
           duco::modeLower(systemMode), systemPerm ? " (permanent)" : "", source);
  cqHead = cqCount = 0;
  if (cur.active && cur.m.cmd[0] == 0x12) cur.active = false;
  if (requester) queueConfirm(requester);
  for (int k = 0; k < N_NODES; k++)
    if (NODES[k].addr != requester) queueConfirm(NODES[k].addr);
  if (requester) notBefore = millis() + CONFIRM_DELAY_MS;
  publishBox(source);
}

static void onMqtt(char* topic, uint8_t* payload, unsigned int len) {
  if (strcmp(topic, BOX_SET_TOPIC) != 0) return;
  char v[12]; unsigned n = len < sizeof(v) - 1 ? len : sizeof(v) - 1;
  memcpy(v, payload, n); v[n] = 0;
  static const uint8_t modes[] = {duco::MODE_AUTO, duco::MODE_LOW, duco::MODE_MIDDLE, duco::MODE_HIGH, duco::MODE_AWAY};
  for (uint8_t m : modes)
    if (strcmp(v, duco::modeLower(m)) == 0) { setSystemMode(m, false, "ha"); return; }
  Serial.printf("MQTT: ignoring unknown mode '%s'\n", v);
}

static void ensureConnections() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiTry > 5000) { lastWifiTry = millis(); Serial.println("WiFi connecting..."); WiFi.begin(WIFI_SSID, WIFI_PASS); }
    return;
  }
  if (!mqtt.connected()) {
    Serial.print("MQTT connecting... ");
    bool ok = (MQTT_USER[0]) ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS) : mqtt.connect(MQTT_CLIENT_ID);
    Serial.println(ok ? "connected" : "failed");
    if (!ok) return;
    if (!discoverySent) { publishDiscovery(); discoverySent = true; }
    mqtt.subscribe(BOX_SET_TOPIC);
    publishBox("boot");
  }
}

// ---- boot self-tests ------------------------------------------------------------
// Frame builders against frames a REAL box sent (reference capture), byte for
// byte: type, then (network ID, filled in from DUCO_NET), then the addressing
// and command bytes as captured. The CRC follows from them.
static bool selfTestBuilders() {
  struct Ref { const char* what; uint8_t len; uint8_t type; uint8_t rest[11]; };   // rest = bytes 5..len-1
  static const Ref refs[] = {
    {"read reg 0x80, node 2",     14, 0x07, {0x08, 0x82, 0x24, 0xff, 0x30, 0x00, 0x40, 0x00, 0x80}},
    {"confirm low, node 3 via 5", 13, 0x07, {0x09, 0x42, 0x38, 0xff, 0x20, 0x00, 0x12, 0x04}},
    {"ACK node 3 via 5",           9, 0x06, {0x09, 0x42, 0x36, 0xff}},
    {"broadcast 52 00",           13, 0x07, {0x08, 0x02, 0x04, 0xff, 0x20, 0x00, 0x52, 0x00}},
    {"52 00, node 4 via 5",       13, 0x07, {0x09, 0x42, 0x49, 0xff, 0x20, 0x00, 0x52, 0x00}},
  };
  const uint8_t net[4] = {(uint8_t)(DUCO_NET >> 24), (uint8_t)(DUCO_NET >> 16), (uint8_t)(DUCO_NET >> 8), (uint8_t)DUCO_NET};
  uint8_t f[5][40];
  uint8_t n[5];
  n[0] = duco::buildReadParam(f[0], DUCO_NET, 2, 4, 0x80);
  n[1] = duco::buildConfirmMode(f[1], DUCO_NET, 3, 8, duco::MODE_LOW, 5);
  n[2] = duco::buildAck(f[2], DUCO_NET, 3, 6, 5);
  n[3] = duco::buildPoll52(f[3], DUCO_NET, 0, 4);
  n[4] = duco::buildPoll52(f[4], DUCO_NET, 4, 9, 5);
  bool all = true;
  for (int i = 0; i < 5; i++) {
    const Ref& r = refs[i];
    bool ok = n[i] == r.len + 3 && f[i][0] == r.len && f[i][1] == r.type &&
              memcmp(f[i] + 2, net, 4) == 0 && memcmp(f[i] + 6, r.rest, r.len - 5) == 0;
    if (!ok) Serial.printf("Builder self-test FAIL: %s\n", refs[i].what);
    all = all && ok;
  }
  Serial.printf("Builder self-test: %s (5 frames vs the real box)\n", all ? "PASS" : "FAIL");
  return all;
}

// Streaming capture: a frame with a node message 13 bits behind it, not
// byte-aligned, as the radio would hand them over. Feed it byte by byte like
// readRxFrame and check the capture runs on to cover both and scan() finds both.
static size_t streamCapture(const uint8_t* st, size_t cap) {   // the readRxFrame loop, minus the radio
  size_t n = 0, need = 1;
  while (n < need && n < cap) {
    n++;
    if (n == need) { size_t w = duco::captureNeed(st, n, TAIL_BYTES); need = w > n ? w : n; }
  }
  return n;
}

static bool doubledSync(const uint8_t* buf, size_t n, uint16_t bitOffset);

static bool selfTestCapture() {
  // A fixed made-up network ID, so the test bits are the same on every install
  // (the expected numbers below were worked out for exactly this stream).
  const uint32_t TEST_NET = 0x00123456;
  uint8_t hb[16], msg[24], st[128] = {0};
  uint8_t nHb = duco::buildHeartbeat(hb, TEST_NET, 3, duco::MODE_AUTO);
  uint8_t pl[17] = {0x07, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0x60, 0x00, 0x22, 10, 0x04, 0xC8, 0x00, 0x11};
  pl[1] = TEST_NET >> 24; pl[2] = TEST_NET >> 16; pl[3] = TEST_NET >> 8; pl[4] = TEST_NET;
  duco::packAddr(pl + 5, 2, 1, 2, 1, 5);
  uint8_t nMsg = duco::frame(msg, pl, sizeof(pl));

  size_t bit = 0;
  auto putBits = [&](uint32_t v, int nbits) {
    for (int k = nbits - 1; k >= 0; k--, bit++)
      if ((v >> k) & 1) st[bit >> 3] |= 0x80 >> (bit & 7);
  };
  static const uint8_t pre[] = {0xAA, 0xAA, 0xAA, 0xAA, 0xD3, 0x91, 0xD3, 0x91};
  for (int i = 0; i < nHb; i++) putBits(hb[i], 8);
  putBits(0x0B54, 13);                                        // 13-bit gap: next frame unaligned
  for (uint8_t b : pre) putBits(b, 8);
  for (int i = 0; i < nMsg; i++) putBits(msg[i], 8);
  size_t msgEnd = (bit + 7) / 8;
  for (size_t i = msgEnd; i < sizeof(st); i++) st[i] = (uint8_t)(i * 37);   // noise behind it

  size_t n = streamCapture(st, sizeof(st));
  duco::Found f[8];
  int nf = duco::scan(st, n, f, 8);
  bool ok = n == msgEnd + TAIL_BYTES && nf == 2 &&
            f[0].f.payload[0] == 0x00 && f[1].f.payload[0] == 0x07 && f[1].f.payload[11] == 0x22;
  // A lone frame stops TAIL_BYTES after its end; a false sync stops at once.
  uint8_t lone[64] = {0};
  memcpy(lone, hb, nHb);
  size_t nLone = streamCapture(lone, sizeof(lone));
  uint8_t junk[] = {0xC4, 1, 2};
  size_t nJunk = streamCapture(junk, sizeof(junk));
  ok = ok && nLone == (size_t)nHb + TAIL_BYTES && nJunk == 1;
  // RX matches one D391, so a doubled-sync frame arrives as D3 91 len ...
  uint8_t dbl[64] = {0xD3, 0x91};
  memcpy(dbl + 2, hb, nHb);
  size_t nDbl = streamCapture(dbl, sizeof(dbl));
  int nfDbl = duco::scan(dbl, nDbl, f + 2, 1);
  ok = ok && nDbl == (size_t)nHb + 2 + TAIL_BYTES && nfDbl == 1 && doubledSync(dbl, nDbl, f[2].bitOffset);
  ok = ok && !doubledSync(st, n, f[0].bitOffset) && doubledSync(st, n, f[1].bitOffset);
  // As seen on air: second sync byte mangled (D3 80) and one bit of a network ID
  // byte flipped. The capture must still run to the end, and repair must flip
  // that bit back.
  uint8_t netByte = dbl[2 + 4];
  dbl[1] = 0x80; dbl[2 + 4] ^= 0x04;
  size_t nBad = streamCapture(dbl, sizeof(dbl));
  int rep = duco::repairFrame(dbl, nBad, 2, f[3]);
  bool repOk = nBad == (size_t)nHb + 2 + TAIL_BYTES && rep == 1 + 4 * 8 + 5 && f[3].data[4] == netByte;
  Serial.printf("Repair self-test: %s (captured %u, want %u; repair %d, want %d)\n", repOk ? "PASS" : "FAIL",
                (unsigned)nBad, (unsigned)(nHb + 2 + TAIL_BYTES), rep, 1 + 4 * 8 + 5);
  ok = ok && repOk;
  Serial.printf("Capture self-test: %s (pair: %u bytes/%d frames, want %u/2; lone: %u, want %u; "
                "doubled: %u, want %u; junk: %u, want 1)\n",
                ok ? "PASS" : "FAIL", (unsigned)n, nf, (unsigned)(msgEnd + TAIL_BYTES),
                (unsigned)nLone, (unsigned)(nHb + TAIL_BYTES), (unsigned)nDbl,
                (unsigned)(nHb + 2 + TAIL_BYTES), (unsigned)nJunk);
  return ok;
}

static void check(int16_t s, const char* what) {
  if (s == RADIOLIB_ERR_NONE) return;
  Serial.printf("RADIO ERROR: %s = %d -- halting\n", what, s); while (true) delay(1000);
}

void setup() {
  Serial.setTxBufferSize(8192);   // logging must never stall the radio loop
  Serial.begin(115200); delay(300);
  Serial.println("\n=== duco-rf-bridge " FW_VERSION " (bridge) ===  (old Duco box must stay OFF)");
  if (DUCO_NET == 0) {                  // never transmit on a made-up network
    Serial.println("DUCO_NET is not set in config.h. Run the sniffer firmware with your old box on,");
    Serial.println("copy the config it suggests into src/bridge/config.h and flash again. Halting.");
    while (true) delay(1000);
  }
  buildPollSchedule();
  bool captureOk = selfTestCapture();
  bool buildersOk = selfTestBuilders();
  bool selfOk = captureOk && buildersOk;
  prefs.begin("duco", false);
  uint8_t saved = prefs.getUChar("mode", duco::MODE_AUTO);
  if (saved == duco::MODE_AUTO || (saved >= duco::MODE_LOW && saved <= duco::MODE_AWAY)) systemMode = saved;
  systemPerm = prefs.getBool("perm", false);
  eventLog("boot " FW_VERSION ", self-tests %s, box mode %s%s (saved)", selfOk ? "PASS" : "FAIL",
           duco::modeLower(systemMode), systemPerm ? " perm" : "");

  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  mqtt.setServer(MQTT_HOST, MQTT_PORT); mqtt.setBufferSize(768); mqtt.setCallback(onMqtt);

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  check(radio.beginFSK(DUCO_TX_FREQ_MHZ, BITRATE_KBPS, FREQ_DEV_KHZ, RX_BW_KHZ, DUCO_TX_POWER_DBM, 32, false), "beginFSK");
  check(radio.setSyncWord(SYNC_WORD, sizeof(SYNC_WORD)), "setSyncWord");
  check(radio.setEncoding(RADIOLIB_ENCODING_NRZ), "setEncoding");
  check(radio.setDataShaping(RADIOLIB_SHAPING_0_5), "setDataShaping");
  check(radio.setCRC(false), "setCRC");
  check(radio.setAFCBandwidth(AFC_BW_KHZ), "setAFCBandwidth");
  check(radio.setAFC(true), "setAFC");
  check(radio.setAFCAGCTrigger(RADIOLIB_SX127X_RX_TRIGGER_PREAMBLE_DETECT), "setAFCAGCTrigger");
  check(radio.setOutputPower(DUCO_TX_POWER_DBM), "setOutputPower");
  startRx();
  Serial.printf("Box up on %08lX @ %.4f MHz. %d nodes, %d poll items.\n",
                (unsigned long)DUCO_NET, DUCO_TX_FREQ_MHZ, N_NODES, nPoll);
}

// ---- receive ------------------------------------------------------------------
// Frames often come back to back (a relay right behind the original). Restarting
// RX at the end of each frame takes longer than the gap, so stream the FIFO into
// one long capture that only stops TAIL_BYTES after the last frame in it (see
// duco::captureNeed). scan() then finds every frame in the capture.
static uint8_t readRxFrame(uint8_t* buf, int16_t& rssi) {
  uint8_t f2 = radioMod->SPIreadRegister(RADIOLIB_SX127X_REG_IRQ_FLAGS_2);
  if (f2 & RADIOLIB_SX127X_FLAG_FIFO_OVERRUN) { rxOverrun++; return 0; }   // caller restarts RX
  if (f2 & RADIOLIB_SX127X_FLAG_FIFO_EMPTY) return 0;
  rssi = -(int16_t)radioMod->SPIreadRegister(RADIOLIB_SX127X_REG_RSSI_VALUE_FSK) / 2;
  size_t n = 0, need = 1;
  uint32_t tByte = micros();
  while (n < need && n < CAPTURE_LEN) {
    f2 = radioMod->SPIreadRegister(RADIOLIB_SX127X_REG_IRQ_FLAGS_2);
    if (f2 & RADIOLIB_SX127X_FLAG_FIFO_OVERRUN) { rxOverrun++; break; }
    if (!(f2 & RADIOLIB_SX127X_FLAG_FIFO_EMPTY)) {
      buf[n++] = radioMod->SPIreadRegister(RADIOLIB_SX127X_REG_FIFO);
      tByte = micros();
      if (n == need) { size_t w = duco::captureNeed(buf, n, TAIL_BYTES); need = w > n ? w : n; }
    } else if (f2 & RADIOLIB_SX127X_FLAG_PAYLOAD_READY) {
      break;                                   // whole capture length used up
    } else if (micros() - tByte > 3000) {
      break;                                   // bytes stopped coming (one is due every 208 us)
    }
  }
  return (uint8_t)n;
}

// Was this frame sent with a doubled sync (D391 D391 before its length byte)?
static bool doubledSync(const uint8_t* buf, size_t n, uint16_t bitOffset) {
  if (bitOffset == 16) return true;                     // radio matched the first of two
  if (bitOffset < 32) return false;
  bool ok = true;
  return duco::getByteAt(buf, n * 8, bitOffset - 32, ok) == 0xD3 &&
         duco::getByteAt(buf, n * 8, bitOffset - 24, ok) == 0x91 && ok;
}

// Remember how a node reaches us: a frame from it headed for the box either
// comes straight (src = node, dst = 1), goes to a repeater first (dst = rep), or
// arrives relayed (src = rep).
static void learnRoute(const duco::Header& h) {
  if (h.odst != 1 || h.osrc < 2) return;
  uint8_t via = (h.src == h.osrc) ? (h.dst == 1 ? 0 : h.dst) : h.src;
  if (ROUTE[h.osrc] != via) {
    if (LOG_RX) {
      if (via) Serial.printf("[%8.1fs] route: node %u %s via node %u\n", millis() / 1000.0, h.osrc, nodeName(h.osrc), via);
      else     Serial.printf("[%8.1fs] route: node %u %s direct\n", millis() / 1000.0, h.osrc, nodeName(h.osrc));
    }
    ROUTE[h.osrc] = via;
  }
}

static void handleFrame(const duco::Frame& fr, int16_t rssi, bool dblSync) {
  duco::Header h = duco::header(fr);
  if (!h.valid || h.net != DUCO_NET) return;
  RxStat& rs = RXSTAT[h.src & 31];
  rs.frames++; if (!dblSync) rs.singleSync++; rs.lastRssi = rssi;
  if (LOG_RX && h.src != 1) {
    static char line[640];
    duco::describe(fr, line, sizeof(line));
    Serial.printf("[%8.1fs] [rx %4ddBm %ssync] %s\n", millis() / 1000.0, rssi, dblSync ? "2x" : "1x", line);
  }
  if (h.src == 1 || h.osrc == 1) return;                    // our own frame, or a relay of it
  learnRoute(h);

  // An ACK for the message we have in flight?
  if (h.type == 0x06 && h.odst == 1 && cur.active && h.osrc == cur.m.node && h.counter == cur.ctr) {
    onAcked();
    return;
  }

  if (h.dst == 1 && h.type == 0x07)                         // ACK directed frames to us at once,
    { uint8_t a[16];                                        // back via the repeater it came through
      txFrame(a, duco::buildAck(a, DUCO_NET, h.osrc, h.counter, h.src != h.osrc ? h.src : 0)); }

  duco::NodeStatus s;
  if (!duco::parseStatus(fr, s)) return;
  int idx = nodeIndex(s.node);
  static NodeState scratch;             // nodes not in DUCO_NODES: decode + log, don't publish
  NodeState& ns = idx >= 0 ? STATE[idx] : scratch;

  if (s.hasStatus && h.dst == 1) {      // count each 0x22 once (not also its first hop)
    ns.haveDemand = true; ns.demand = s.demandPct;
    ns.haveMode = true;   ns.mode = s.mode;
    ns.haveTemp = true;   ns.tempC = s.tempC;
    n22++; last22Node = s.node; last22At = millis();
    char raw[3 * duco::MAX_LEN + 1]; size_t rw = 0;
    for (uint8_t i = 0; i < fr.len && rw + 4 <= sizeof(raw); i++) rw += snprintf(raw + rw, sizeof(raw) - rw, "%02x ", fr.payload[i]);
    eventLog("0x22 node %u %s (via %u): %s pct=%u mode=%s%s flags=0x%02X button=0x%02X temp=%.1fC | %s",
             s.node, nodeName(s.node), h.src, s.request ? "PRESS" : "status", s.demandPct,
             duco::modeLower(s.mode), s.permanent ? " perm" : "", s.flags, s.button, s.tempC, raw);
    // A press (0x08 request flag): adopt the mode and confirm it to every node,
    // the requester first. A status report is only ACKed, like the real box does.
    if (s.request) {
      char src[32]; snprintf(src, sizeof(src), "%s (node %u)", nodeName(s.node), s.node);
      setSystemMode(s.mode, s.permanent, src, s.node);
    }
  }
  if (s.hasReg && h.dst == 1) {
    if (s.reg == duco::REG_CO2)  { ns.haveCo2 = true; ns.co2 = s.regValue; }
    else if (s.reg == duco::REG_RH) { ns.haveRh = true; ns.rh = s.regValue / 100.0f; }
    else if (s.reg == duco::REG_TEMP) { ns.haveTemp = true; ns.tempC = s.regValue / 10.0f; }
  }
  if (h.dst != 1) return;
  if (idx >= 0) publishState(idx);
  Serial.printf("node %u %s: demand=%u%% mode=%s temp=%.1fC", s.node, nodeName(s.node),
                ns.demand, duco::modeLower(ns.mode), ns.tempC);
  if (ns.haveCo2) Serial.printf(" co2=%uppm", ns.co2);
  if (ns.haveRh)  Serial.printf(" rh=%.1f%%", ns.rh);
  Serial.println();
}

static void serviceRx() {
  uint8_t buf[CAPTURE_LEN]; int16_t rssi = 0;
  uint16_t ovr = rxOverrun;
  uint8_t n = readRxFrame(buf, rssi);
  if (!n && ovr == rxOverrun) return;
  radio.startReceive();                 // back to RX first (clears the FIFO), then decode
  if (!n) return;
  duco::Found found[8];
  int nf = duco::scan(buf, n, found, 8);
  if (nf == 0 && n > 1) {               // no CRC-valid frame: a bad 2nd sync byte or one flipped bit?
    int r = duco::repairFrame(buf, n, buf[0] == 0xD3 ? 2 : 0, found[0]);
    duco::Header rh = duco::header(found[0].f);
    if (r >= 0 && rh.valid && rh.net == DUCO_NET) {
      nf = 1; rxRepaired++;
      char what[48];
      if (r == 0) snprintf(what, sizeof(what), "CRC ok, second sync byte was 0x%02X", buf[1]);
      else        snprintf(what, sizeof(what), "flipped bit %d (byte %d)", r - 1, (r - 1) / 8);
      if (rh.type == 0x07 && rh.osrc != 1)   // a node message: keep it in the event ring
        eventLog("repaired type 0x07 from node %u: %s", rh.osrc, what);
      else if (LOG_RX)
        Serial.printf("[%8.1fs] [rx %4ddBm] repaired type 0x%02X from node %u: %s\n", millis() / 1000.0,
                      rssi, rh.type, rh.src, what);
    }
  }
  if (nf == 0 && n > 1) {               // plausible length, but no CRC-valid frame in it
    rxJunk++;
    static uint32_t lastJunkLog = 0;
    if (LOG_RX && millis() - lastJunkLog > 1000) {
      lastJunkLog = millis();
      Serial.printf("[%8.1fs] [rx %4ddBm] no valid frame in %u bytes:", millis() / 1000.0, rssi, n);
      for (int i = 0; i < n && i < 72; i++) Serial.printf(" %02x", buf[i]);
      Serial.println();
    }
  }
  for (int i = 0; i < nf; i++) handleFrame(found[i].f, rssi, doubledSync(buf, n, found[i].bitOffset));
}

static void printStats() {
  Serial.printf("[%8.1fs] [stats] heard (last %lus):", millis() / 1000.0, (unsigned long)(STATS_MS / 1000));
  bool any = false;
  for (int a = 2; a < 32; a++) {
    RxStat& rs = RXSTAT[a];
    if (!rs.frames) continue;
    Serial.printf("  node%d=%u %ddBm", a, rs.frames, rs.lastRssi);
    any = true;
  }
  if (!any) Serial.print("  nothing from any node");
  Serial.printf("  | tx=%u acked=%u gave-up=%u | junk=%u repaired=%u overrun=%u | 0x22 since boot=%u",
                txSent, txAcked, txGaveUp, rxJunk, rxRepaired, rxOverrun, n22);
  if (n22) Serial.printf(" (last: node %u, %lus ago)", last22Node, (unsigned long)((millis() - last22At) / 1000));
  Serial.printf(" | box=%s%s | routes:", duco::modeLower(systemMode), systemPerm ? " perm" : "");
  for (int k = 0; k < N_NODES; k++) {
    uint8_t a = NODES[k].addr;
    if (ROUTE[a]) Serial.printf(" %u>%u", a, ROUTE[a]); else Serial.printf(" %u direct", a);
  }
  Serial.println();
  printEvents();
  memset(RXSTAT, 0, sizeof(RXSTAT)); rxJunk = rxOverrun = rxRepaired = 0;
  txSent = txAcked = txGaveUp = 0;
}

void loop() {
  serviceRx();
  ensureConnections();
  mqtt.loop();
  serviceRx();
  serviceTx();
  if (millis() - lastStats >= STATS_MS) { lastStats = millis(); printStats(); }
}
