# duco-rf-bridge: plan

Where this project goes from "works on one house with a config file" to
"anyone with a DucoBox can set it up from a browser". It covers the target user
journey, the architecture, the repository layout, and a phased roadmap with
acceptance criteria for each step.

## 1. Goal and scope

**Goal:** replace the RF side of a DucoBox mainboard with a cheap ESP32 + SX1276
board. The bridge talks to the existing Duco switches and sensors, learns your
network, and exposes everything to Home Assistant.

**In scope**
- learning an existing Duco RF network (network ID, nodes, box frequency) from a working box
- naming the nodes (rooms), with "press a button to identify it"
- acting as the box: polls, register reads, button presses → mode confirms
- Home Assistant integration (MQTT discovery first, ESPHome later)
- sharing captures safely (sanitized logs) to keep improving the protocol docs

**Out of scope (for now)**
- driving the fan motor: that's the job of whatever replaces the mainboard's
  power stage (Phase 6 covers the integration hooks)
- pairing brand-new Duco sensors (needs the join protocol, Phase 5)
- other Duco products or networks (DucoBox Focus/Silent/Energy probably share the
  protocol, but that's unverified)

**Name: `duco-rf-bridge`.** It says what it is: a bridge on Duco's RF
network. It doesn't claim to be a Duco product. Alternatives considered:
`ducobox-rf` (box-centric, but the box is what gets replaced), `open-ducobox`
(sounds like official firmware), `duco-ha` (HA-only; the protocol docs and
sniffer are useful without HA). The README carries a non-affiliation and
trademark note.

## 2. The user journey we're building towards

| step | today (v0.1) | target |
|---|---|---|
| 0. Hardware | LilyGO T3 868 MHz | same, plus documented alternatives (Heltec, generic ESP32 + RFM95) |
| 1. Flash | PlatformIO, two environments | **browser flashing** (ESP Web Tools on GitHub Pages), one firmware |
| 2. Connect | WiFi/MQTT in `config.h` | **captive portal** "duco-rf-bridge-XXXX": WiFi, MQTT (or ESPHome API later) |
| 3. Learn | sniffer env prints a config block | **Learn** button in the web UI: listens (never transmits), shows the networks it hears, picks yours, measures the box frequency, lists the nodes live |
| 4. Name | edit `DUCO_NODES` | in the learn screen: **"press a button on a switch"** → that node lights up → type the room name. Sensor type (CO₂ / RH) is detected later, in step 5. |
| 5. Take over | unplug box, flash bridge env | **Take over** button: "unplug your DucoBox now" → the bridge checks the old box is really silent → starts acting as the box → probes each node's sensors → shows ✓ per node |
| 6. Home Assistant | MQTT discovery, one device | one HA **device per room**, the box as hub, availability, diagnostics, OTA updates |
| later | — | add/rename/remove nodes, re-learn, backup/restore config as JSON |

## 3. Architecture

### One firmware, four modes

```
           ┌───────────┐   WiFi/MQTT saved   ┌──────────┐  user: "learn"  ┌─────────┐
  boot ──► │  SETUP    │ ──────────────────► │  IDLE    │ ──────────────► │  LEARN  │ (RX only)
           │ (portal)  │                     │ (no net) │ ◄── cancel ──── │         │
           └───────────┘                     └──────────┘                 └────┬────┘
                                                                               │ user picks network,
                                                                               │ names nodes, "take over"
                                                     ┌──────────┐  old box     ▼
                                                     │  BRIDGE  │ ◄─ silent ─ TAKEOVER CHECK
                                                     │ (= box)  │             (listen 2 min for src=1
                                                     └──────────┘              frames we didn't send)
  SNIFF (diagnostic, RX only, verbose log) can be entered from any mode and left again.
```

- **No TX outside BRIDGE.** LEARN and SNIFF are receive-only by construction: the
  TX function refuses unless mode == BRIDGE and a network ID is set.
- **Clash guard in BRIDGE:** if a frame with `src = 1` arrives that we didn't send
  (another box is on), stop transmitting, raise an HA problem sensor and a
  log event, and resume only after 5 minutes of silence from it.

### Components and where they live

| component | responsibility | folder (target) |
|---|---|---|
| **protocol** | frame layout, CRC, scan, capture length, repair, builders, parsers. Plain C++, no Arduino, host-tested. | `firmware/lib/duco/` |
| **radio** | SX1276 setup, streaming RX capture, TX with doubled sync, AFC readout | `firmware/lib/duco_radio/` |
| **box engine** | TX queue with retries, routes, poll schedule, network message timer, press → confirm flow, clash guard | `firmware/lib/duco_box/` (today: `src/bridge/main.cpp`) |
| **learn engine** | per-network stats, box detection, AFC averaging, node discovery, press detection | `firmware/lib/duco_learn/` (today: in `src/sniffer/main.cpp`) |
| **node registry** | network ID, box frequency, nodes {addr, id, name, sensors, route}; NVS + JSON import/export | `firmware/lib/registry/` |
| **integrations** | MQTT + HA discovery; later ESPHome | `firmware/lib/mqtt_ha/`, `esphome/` |
| **web UI** | captive portal, learn/naming screen, status, logs, OTA. Static files served from flash. | `firmware/web/` (built into LittleFS) |
| **app** | mode state machine, wiring | `firmware/src/main.cpp` |

The protocol library stays **pure C++**. That keeps it host-testable and reusable
by an ESPHome component or a Python port.

## 4. Repository layout

What's in this folder now (v0.1) and what each phase adds:

```
duco-rf-bridge/
├── README.md                 what it is, hardware, quick start, status, credits, disclaimer
├── PLAN.md                   this file
├── .gitignore                config.h, logs, build output
├── docs/
│   ├── getting-started.md    the 4-step process (today) + troubleshooting
│   ├── protocol.md           the RF protocol: radio, frames, commands, flags, box behaviour
│   ├── hardware.md           board, pins, antenna, regulatory
│   └── (Phase 2) web-ui.md, (Phase 3) home-assistant.md, (Phase 5) boards.md, pairing.md
├── firmware/                 one PlatformIO project
│   ├── platformio.ini        envs: bridge, sniffer   (+ native for host tests, Phase 1)
│   ├── lib/duco/             shared protocol library (duco_frame.h, duco_box.h)
│   ├── src/bridge/           box replacement + MQTT/HA (config.example.h → config.h)
│   ├── src/sniffer/          learn + capture, receive only
│   ├── test/                 (Phase 1) host unit tests against reference frames
│   └── web/                  (Phase 2) web UI sources
├── homeassistant/
│   └── README.md             entities, topics, dashboard + automation examples
├── tools/
│   ├── sanitize_log.py       strip network IDs from logs before sharing
│   ├── duco_decode.py        decode Flipper Zero RAW captures
│   └── flipper/              Flipper Sub-GHz preset + test frame
├── (Phase 4) web-flasher/    ESP Web Tools manifest + page (GitHub Pages)
├── (Phase 4) .github/        CI: build both firmwares, host tests, release binaries; issue templates
└── (Phase 5) esphome/        external component
```

Grouping rule: **firmware** (runs on the board), **docs** (read by people),
**homeassistant** (copied into HA), **tools** (run on your PC). Every log or
capture in the repo goes through `tools/sanitize_log.py`.

## 5. Node registry and persistence

Stored in NVS (namespace `duco`) as one JSON blob, exportable and importable
through the web UI and an MQTT command:

```json
{
  "version": 1,
  "net": "00123456",
  "freq_mhz": 868.2871,
  "tx_dbm": 14,
  "mode": { "value": "auto", "permanent": false },
  "nodes": [
    { "addr": 2, "id": "living_room", "name": "Living room", "sensors": ["co2"], "route": 0 },
    { "addr": 5, "id": "bathroom", "name": "Bathroom", "sensors": ["rh"], "route": 0 }
  ]
}
```

- `config.h` stays as a **seed** for people who prefer compiling: if NVS is
  empty, its values are imported once.
- Routes are learned at runtime (not persisted). They are only a starting
  hint after reboot.
- `sensors` is filled in by the probe in TAKEOVER (`0x4A`/`0x4B` non-zero → present);
  the user can override it.

## 6. Learn mode (receive only)

**Input:** the old box running. **Output:** a registry draft.

1. Listen with AFC on, nominal frequency, 2-byte sync, streaming capture + repair
   (all of this exists in `src/sniffer`).
2. Per network ID: frames, best RSSI, **box frames** (`src = 1`) and their RSSI,
   AFC readings of box frames, node addresses (any `orig.src`/`orig.dst` ≥ 2,
   including nodes the box polls that never answer), presses.
3. **Your network** = the one whose box (`src = 1`) is heard strongest with at
   least N box frames. The UI shows all candidates with RSSI, so the user can
   override, e.g. when a neighbour's box is closer. A press seen on a network is
   strong evidence it's yours.
4. **Box frequency** = nominal + mean AFC of box frames (trimmed mean, n ≥ 10).
5. **Done when:** ≥ 2 network-message intervals (~3 min) have passed, or every
   node has been heard at least once, and the user has had a chance to press buttons.
6. **Naming:** each `0x22` with the request flag (`0x08`) → "Node N was pressed"
   toast, highlight the row, focus its name field. Nodes without buttons (pure
   sensors) are named from their position in the list / their readings.

Already working in v0.1: steps 1–4 and press detection, with the result printed as a config block.

## 7. Takeover and bridge mode

1. User confirms "old box unplugged".
2. **Silence check:** listen 2 minutes; any `src = 1` frame on our network →
   "your old box is still on", abort.
3. Start BRIDGE: network message, then poll every node (`52 00`), then **probe
   sensors** once per node (`40 00 4A`, `40 00 4B`). A reply ≠ 0 marks the sensor present.
4. Show per node: ACKed ✓ / no answer ✗ (with RSSI and route).
5. Normal operation = the v0.1 bridge, which mirrors the real box
   (docs/protocol.md "How the real box behaves"):
   - network message every 82 s, nothing faster
   - one directed message at a time, resend every 300 ms until ACKed, max 6 tries
   - routes learned from the nodes' frames
   - press → ACK → 170 ms → `12 <mode>` to the requester, then to all nodes
   - status `0x22` → ACK only
   - box mode saved across reboots
6. Per-node health: last heard, RSSI, route, ACK rate → HA diagnostic entities.

## 8. Home Assistant integration

**Phase 1–3: MQTT discovery (exists, gets polished)**
- **One device per node** ("Duco Living room", model "Duco RF node", `via_device`
  = the bridge), plus the bridge device ("Duco RF bridge") holding `Box mode`.
  Today everything sits on one device.
- Per node: temperature, demand, mode, CO₂ and/or RH (from the probe), plus
  diagnostics: RSSI, route, last seen, ACK rate.
- Bridge: `Box mode` select, permanent switch (long-press equivalent), clash
  problem sensor, uptime, WiFi RSSI, firmware version, `update` entity (OTA).
- **Availability:** MQTT LWT `duco/bridge/status` online/offline; per-node
  availability goes stale after N missed polls.
- **Events:** `event` entity per node for button presses (short/long, button
  number), so automations can react to a press itself, not just the mode.
- Renaming in HA is the normal HA way; the bridge's names only seed it.

**Phase 5: ESPHome external component (optional, parallel)**
- `esphome/components/duco_rf/`: reuses `lib/duco`; YAML config for network and
  nodes; native API instead of MQTT; ESPHome handles WiFi, OTA and the portal.
- Learn mode as a YAML switch + logs. Some loss of the custom web UI.
- Worth doing because many HA users prefer ESPHome. Only start once the
  standalone firmware's timing is stable (the radio loop must never block).

## 9. Setup UX (web UI)

Served from the ESP32 (LittleFS, plain HTML/JS, no framework, < 100 kB):

1. **Setup:** WiFi scan + password, MQTT host/user/password (test button).
2. **Learn:** start/stop, live table of networks (yours highlighted) and nodes
   (address, RSSI, last frame, presses); box frequency; name fields; "Take over".
3. **Status:** mode, nodes with ✓/✗, RSSI, route; last events; `[stats]`.
4. **Logs:** live log over WebSocket, "download sanitized log" (sanitized on the device).
5. **Settings:** TX power, export/import JSON, factory reset, OTA upload.

Libraries: ESPAsyncWebServer + AsyncTCP (or the core WebServer if timing
suffers), LittleFS. A captive portal (DNSServer) in SETUP.

## 10. Distribution

- **GitHub Actions:** build `bridge` and `sniffer` (later a single firmware),
  run host tests, attach `.bin`s to releases on tags.
- **ESP Web Tools** page on GitHub Pages: "Connect → Install" from Chrome/Edge,
  no toolchain. Includes "Logs & console" for the first boot.
- Issue templates: "my nodes don't answer" (asks for a sanitized log),
  "new device/protocol capture".
- Versioning: semver; registry JSON has its own `version` for migrations.

## 11. Testing

| level | what | where |
|---|---|---|
| host unit tests | CRC, builders vs reference frames, parse, captureNeed, repair, route learning, press → confirm queue | `firmware/test/`, PlatformIO `native` + Unity, in CI |
| on-device self-tests | the same core checks at boot (exist today: capture, repair, 5 builders vs the real box) | `src/bridge` |
| replay tests | feed sanitized captures (bit streams) through the RX + box engine and compare decisions | `firmware/test/replay/` |
| field checklist | learn → takeover → each switch short/long → HA mode → 24 h run with `gave-up` near 0 | `docs/getting-started.md` |

## 12. Safety, regulatory, privacy

- Never two boxes: TX only in BRIDGE, silence check before, clash guard during.
- 868 MHz g1: +14 dBm default, 1 % duty cycle. A TX airtime counter in `[stats]`
  and HA, with a hard stop at 0.9 % per hour.
- Mains: the docs only ever say unplug/plug at the wall; the cover stays on.
- **Privacy:** network IDs identify a household's box, and logs catch the
  neighbours' too. Every shared log goes through `tools/sanitize_log.py`
  (and the web UI's sanitized download). Docs use the made-up ID `00 12 34 56`.
  `config.h` and `logs/` are git-ignored.
- Disclaimer: not affiliated with Duco; use at your own risk; keep the old box
  so you can go back.

## 13. Roadmap

Each phase ends in something people can use.

### Phase 0: extract and publish (this folder) ✅
- [x] restructure into firmware / docs / homeassistant / tools
- [x] sanitize: no real network IDs, names, ports, credentials, or calibration
- [x] sniffer suggests a ready-to-paste config (network, nodes, frequency) and flags presses
- [x] bridge refuses to transmit without a network ID; TX power default +14 dBm
- [x] log sanitizer
- [ ] **pick a license** (MIT suggested) and add `LICENSE`
- [ ] `git init`, first commit, push to GitHub; enable Discussions
- **Done when:** a second household gets it working with only the docs.

### Phase 1: runtime config and tests
- [ ] node registry in NVS with JSON import/export; `config.h` only seeds it
- [ ] serial commands: `net`, `freq`, `node add/rename/remove`, `mode`, `export`
- [ ] host tests (`native` env) for `lib/duco` + CI building both envs
- [ ] split `src/bridge/main.cpp` into radio / box engine / mqtt libraries
- **Done when:** a new node can be added and renamed without recompiling.

### Phase 2: one firmware, web UI, learn → takeover
- [ ] merge sniffer + bridge into one firmware with the mode state machine (§3)
- [ ] captive portal (WiFi/MQTT), learn screen with press-to-name, takeover with silence check
- [ ] sensor probe sets CO₂/RH per node automatically
- [ ] clash guard, airtime counter
- **Done when:** setup needs no editor and no PlatformIO, only a browser.

### Phase 3: Home Assistant polish
- [ ] device per node with `via_device`, availability (LWT), diagnostics, press `event` entities
- [ ] OTA (web UI + HA `update` entity)
- **Done when:** HA shows rooms as devices, and offline nodes show as unavailable.

### Phase 4: distribution
- [ ] GitHub Actions releases, ESP Web Tools page, issue templates
- **Done when:** install = open a web page and click Install.

### Phase 5: reach
- [ ] configurable pins + boards (Heltec WiFi LoRa 32 V2/V3, ESP32 + RFM95)
- [ ] ESPHome external component
- [ ] **dead-box recovery:** learn the network ID from node presses alone
- [ ] **pairing** new nodes (join types `01`–`04`; capture a pairing with a real box first)

### Phase 6: fan control (separate concern)
- [ ] document the replacement-fan options and HA automations driving them from
  `demand` + `Box mode` (examples exist in `homeassistant/`)
- [ ] optional: PWM / 0–10 V output from a spare ESP32 pin, if the hardware allows

## 14. Open protocol questions

Collected in [docs/protocol.md](docs/protocol.md#open-questions). Each answered
question becomes a short doc change plus, where it matters, a host test with
the captured frames.

## 15. Decisions to make

| decision | options | suggestion |
|---|---|---|
| license | MIT / Apache-2.0 / GPL-3.0 | **MIT**: simple, compatible with RadioLib (MIT), PubSubClient (MIT), ArduinoJson (MIT) |
| HA transport long-term | MQTT / ESPHome API / both | MQTT now; ESPHome as an add-on component, not a rewrite |
| web UI stack | plain HTML+JS / Preact build | plain HTML+JS, no build step |
| one firmware vs two | keep sniffer separate / merge | merge in Phase 2, keep a "sniff" mode |
