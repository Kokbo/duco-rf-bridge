# Home Assistant

The bridge uses MQTT discovery: once it's connected to your broker, the device
**Duco Ventilation** appears under Settings → Devices & services → MQTT.

## Entities

Per node in `DUCO_NODES` (`<Name>` = the display name you gave it):

| entity | unit | from |
|---|---|---|
| `<Name> temperature` | °C | the node's status reports (`0x22`) |
| `<Name> demand` | % | the node's own ventilation demand (CO₂/humidity driven) |
| `<Name> mode` | auto/low/middle/high/away | what the node reported, or the mode it last ACKed |
| `<Name> CO2` | ppm | register `0x4A` (nodes with sensor `'c'` or `'b'`) |
| `<Name> humidity` | % | register `0x4B` (nodes with sensor `'h'` or `'b'`) |

For the box itself:

| entity | |
|---|---|
| `Box mode` (select) | auto / low / middle / high / away. Setting it confirms the mode to every switch, exactly like a button press. Button presses on the switches update it. |

## MQTT topics

| topic | content |
|---|---|
| `duco/<id>` | `{"temp_c":20.5,"demand":10,"mode":"auto","co2_ppm":495,"rh_pct":58.8}` (retained) |
| `duco/box` | `{"mode":"middle","source":"Living room (node 4)","permanent":false}` (retained) |
| `duco/box/mode/set` | write `auto` / `low` / `middle` / `high` / `away` |
| `duco/debug/event` | the latest event line (press, mode change, give-up) for debugging |

Entity IDs follow the names, e.g. `sensor.living_room_co2`, `select.box_mode`.
Check yours under the device page and adjust the examples below.

## Dashboard card

```yaml
type: entities
title: Ventilation
entities:
  - entity: select.box_mode
  - type: section
    label: CO₂
  - sensor.living_room_co2
  - sensor.bedroom_co2
  - type: section
    label: Humidity
  - sensor.bathroom_humidity
```

## Automation examples

Boost when CO₂ is high, back to auto when it's fine again:

```yaml
alias: Ventilation - CO2 boost
trigger:
  - platform: numeric_state
    entity_id: sensor.bedroom_co2
    above: 1200
    for: "00:05:00"
action:
  - service: select.select_option
    target: { entity_id: select.box_mode }
    data: { option: high }
---
alias: Ventilation - CO2 back to auto
trigger:
  - platform: numeric_state
    entity_id: sensor.bedroom_co2
    below: 800
    for: "00:10:00"
action:
  - service: select.select_option
    target: { entity_id: select.box_mode }
    data: { option: auto }
```

Shower boost from the bathroom humidity:

```yaml
alias: Ventilation - shower boost
trigger:
  - platform: numeric_state
    entity_id: sensor.bathroom_humidity
    above: 75
action:
  - service: select.select_option
    target: { entity_id: select.box_mode }
    data: { option: high }
  - wait_for_trigger:
      - platform: numeric_state
        entity_id: sensor.bathroom_humidity
        below: 65
        for: "00:05:00"
    timeout: "01:00:00"
  - service: select.select_option
    target: { entity_id: select.box_mode }
    data: { option: auto }
```

> The bridge replaces the box's **RF side**: it talks to the switches and sensors.
> Driving the fan itself (the motor the old mainboard powered) is up to your fan
> controller. Use the `demand` sensors and `Box mode` to drive it from an
> automation. See PLAN.md, Phase 6.
