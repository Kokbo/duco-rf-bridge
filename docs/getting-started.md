# Getting started

From a working DucoBox to a bridge in Home Assistant in four steps. Today this
means flashing two firmwares and editing one config file. [PLAN.md](../PLAN.md)
describes how this becomes a browser-only setup.

> **Safety.** The DucoBox mainboard runs on mains voltage. Only unplug or plug in
> the box at the wall socket, and leave the cover on. The bridge must **never**
> transmit while the old box is powered: two boxes on one network ID clash.

## 0. What you need

- A **LilyGO T3 v1.6.1, 868 MHz version** (ESP32 + SX1276) with its antenna. See
  [hardware.md](hardware.md). The 433/915 MHz variants won't work.
- A USB cable, and [PlatformIO](https://platformio.org/) (VS Code extension or CLI).
- Home Assistant with an MQTT broker (e.g. the Mosquitto add-on) and the MQTT integration.
- Your **old DucoBox, still working**, for step 1. (Box already dead? See the end.)

## 1. Learn your network (old box ON, bridge only listens)

The sniffer firmware never transmits, so it's safe next to the running box.

```bash
cd firmware
pio run -e sniffer -t upload -t monitor
```

1. Leave it running for **at least 3 minutes**. The box only sends its
   network message every ~82 s, but you'll see its polls and the nodes' replies
   right away.
2. **Press one button on each switch/sensor**, one room at a time. Each press
   shows up as
   ```
   >>> PRESS node 4: asks for LOW (button 0x09)
   ```
   Write down which node number is which room.
3. Every 60 s the sniffer prints a summary, ending with
   ```
   [suggested config] paste into firmware/src/bridge/config.h:
     #define DUCO_NET       0x00123456UL
     #define DUCO_NODES  { {2, 'b', "node_2", "Node 2"}, \
                           {3, 'b', "node_3", "Node 3"} }
     #define DUCO_TX_FREQ_MHZ   868.2871f   // box measured -7.2 kHz from nominal (n=57)
   ```
   "Your" network is the one whose box the sniffer hears strongest. The summary
   also lists any neighbours' networks. If it picked the wrong one, check
   which network your presses show up on.

## 2. Configure the bridge

```bash
cp firmware/src/bridge/config.example.h firmware/src/bridge/config.h
```

Edit `config.h`:

- WiFi and MQTT settings
- paste `DUCO_NET`, `DUCO_NODES` and `DUCO_TX_FREQ_MHZ` from step 1
- give each node a readable **id** (MQTT topic `duco/<id>`, lowercase) and **name**
  (what HA shows), e.g. `{4, 'b', "living_room", "Living room"}`

`'b'` polls both CO₂ and humidity. After the first run you'll see which one a
node lacks (it reads 0). Then set `'c'` (CO₂) or `'h'` (humidity) so HA only
shows the real sensor.

`config.h` is git-ignored: your passwords and network ID stay local.

## 3. Swap: old box OFF, bridge ON

1. **Unplug the old DucoBox.** Wait ~30 s.
2. Flash and watch the bridge:
   ```bash
   pio run -e bridge -t upload -t monitor
   ```
3. Check the boot lines:
   ```
   Repair self-test: PASS ...
   Capture self-test: PASS ...
   Builder self-test: PASS (5 frames vs the real box)
   ```
4. Within a minute every node should answer: `[tx] cmd 52 00 -> node N` followed by
   ACKs, and `node N <name>: ... co2=...ppm` lines. The `[stats]` line every 30 s
   shows `tx=… acked=… gave-up=…`, plus the route to each node.
5. **Press a button on a switch.** You should see
   ```
   0x22 node 4 Living room (via 4): PRESS ... mode=low
   >> box mode -> low (from Living room (node 4)); confirming to all nodes
   node 4 Living room ACKed confirm low (try 1)
   node 2 ... ACKed confirm low ...
   ```
   and the switch's LED stays on the mode you picked.

## 4. Home Assistant

The bridge announces itself through MQTT discovery as the device **Duco
Ventilation**:

- per node: temperature, demand (%), mode, and CO₂ (ppm) and/or humidity (%)
- **Box mode** (select: auto / low / middle / high / away): sets the mode on
  every switch, exactly as a button press would

See [../homeassistant/](../homeassistant/) for dashboard and automation examples.

## Troubleshooting

| symptom | look at |
|---|---|
| a self-test says FAIL | report it with the boot lines; don't use that build |
| `DUCO_NET is not set` | step 2: paste the network ID into `config.h` |
| `[stats]` shows `gave-up` for a node | check that node's power and range. A node the old box only reached via a repeater may need that repeater online. |
| presses don't change anything | is the old box really off? Is the press logged as `PRESS`? |
| HA shows no entities | MQTT credentials, and the `MQTT connecting... connected` line |
| a sensor reads 0 forever | that node doesn't have that sensor: set `'c'` or `'h'` |

When asking for help, attach a log **after** running
`python3 tools/sanitize_log.py <log>`, so your network ID doesn't end up online.

## Old box already dead?

Not supported yet. The bridge needs the network ID, and today the sniffer reads
it from the box. Nodes probably still send it when a button is pressed (they
address the box), which would make learning from the sensors alone possible.
That is on the roadmap in [PLAN.md](../PLAN.md). Captures from a setup with a
dead box are welcome.
