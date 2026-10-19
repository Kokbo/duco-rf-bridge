# duco-rf-bridge

**Replace the RF side of a DucoBox mainboard with a €25 ESP32 + LoRa board, and
bring your Duco switches and sensors into Home Assistant.**

Duco ventilation units (DucoBox) talk to their wireless switches, CO₂ and
humidity sensors over a proprietary 868 MHz protocol. This project:

- **learns** your existing Duco network from the running box (network ID,
  nodes, the box's exact frequency), receive-only, so nothing changes until you
  decide to switch
- **acts as the box**: polls the sensors, reads CO₂/humidity, and handles button
  presses (short and long) the way the real box does, so the switches keep working
- **publishes everything to Home Assistant** (MQTT discovery): temperature,
  demand, CO₂, humidity and mode per room, plus a **Box mode** select you can
  automate

> Status: **v0.1, works in one household** (4 nodes: switches with CO₂ or
> humidity sensors). Setup still means editing a config file. [PLAN.md](PLAN.md)
> describes the road to browser-only setup. Testers with other Duco setups are
> very welcome.

## How it works

```
 Duco switches / sensors  ◄── 868 MHz ──►  LilyGO T3 (ESP32 + SX1276)  ── WiFi/MQTT ──►  Home Assistant
   (unchanged, no re-pairing)              acts as the box on your network
```

The bridge reuses your box's network ID, so the sensors accept it without
re-pairing. The old box must then stay **off** (two boxes on one network clash).
Driving the fan motor itself is up to your fan controller. The bridge gives
you the demand and mode to drive it from (see [homeassistant/](homeassistant/)).

## Quick start

1. **Hardware:** a LilyGO T3 v1.6.1, **868 MHz** version, with antenna ([docs/hardware.md](docs/hardware.md)).
2. **Learn** with your old box still on:
   `cd firmware && pio run -e sniffer -t upload -t monitor`. Press a button on
   each switch, then copy the suggested config it prints.
3. **Configure:** `firmware/src/bridge/config.example.h` → `config.h`
   (WiFi, MQTT, network, room names).
4. **Swap:** unplug the old box, then `pio run -e bridge -t upload -t monitor`.

Full walkthrough with expected output: **[docs/getting-started.md](docs/getting-started.md)**.

## Repository

| folder | what |
|---|---|
| [`firmware/`](firmware/) | one PlatformIO project: `bridge` (the box replacement) and `sniffer` (learn + capture, receive only), sharing the protocol library `lib/duco` |
| [`docs/`](docs/) | [getting started](docs/getting-started.md), [protocol](docs/protocol.md), [hardware](docs/hardware.md) |
| [`homeassistant/`](homeassistant/) | entities, MQTT topics, dashboard and automation examples |
| [`tools/`](tools/) | `sanitize_log.py` (strip network IDs from logs), `duco_decode.py` + `flipper/` (Flipper Zero captures) |
| [`PLAN.md`](PLAN.md) | architecture and roadmap |

## The protocol

The full reverse-engineered protocol is in **[docs/protocol.md](docs/protocol.md)**:
radio settings, frame layout, addressing and routing, commands, the button
and mode bits, and how the real box times its traffic. The last part was the
key to making the switches work.

## Contributing

- **Logs and captures** are the most useful thing you can share, especially from
  other Duco models, a pairing, or a box with different behaviour. Run
  `python3 tools/sanitize_log.py <log>` first: logs contain your network ID and
  your neighbours'.
- Keep `lib/duco` free of Arduino dependencies (it's meant to be host-tested and
  reused by an ESPHome component).
- Check the boot self-tests (`... self-test: PASS`) before and after a change.

## Credits

- [arnemauer/Duco-RF-protocol](https://github.com/arnemauer/Duco-RF-protocol): the
  starting point for the radio settings and message types.
- [RadioLib](https://github.com/jgromes/RadioLib), [PubSubClient](https://github.com/knolleary/pubsubclient),
  [ArduinoJson](https://arduinojson.org/).

## Disclaimer

Not affiliated with, endorsed by, or supported by Duco. "Duco" and "DucoBox" are
trademarks of their owner, used here only to describe compatibility. The
DucoBox runs on mains voltage: only unplug it at the socket and leave the cover
on. You are responsible for staying within the 868 MHz radio rules (+14 dBm,
1 % duty cycle; see [docs/hardware.md](docs/hardware.md)). Keep your old box so
you can always go back.

License: not chosen yet (see PLAN.md §15).
