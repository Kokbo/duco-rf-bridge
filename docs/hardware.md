# Hardware

## Supported board

**LilyGO T3 v1.6.1** (sold as "TTGO LoRa32 V2.1_1.6"), **868 MHz** version: ESP32
+ Semtech SX1276, with an SMA antenna connector. PlatformIO board id `ttgo-lora32-v21`.

| SX1276 pin | ESP32 GPIO |
|---|---|
| SCK | 5 |
| MISO | 19 |
| MOSI | 27 |
| NSS / CS | 18 |
| RESET | 23 |
| DIO0 | 26 |
| DIO1 | 33 |

- Use the **868 MHz** variant and an 868 MHz antenna: the RF matching is built
  for one band, and a 433/915 MHz board hears 868 MHz badly.
- Never transmit without the antenna connected.
- Power from USB (5 V, < 200 mA). Place it roughly where the DucoBox was, or
  anywhere the nodes reach. Nodes route through each other (repeaters), so
  central is good but not required. The `[stats]` line shows RSSI and routes.

## Other boards

Any ESP32 with an SX1276/RFM95 on 868 MHz should work after changing the pin
`#define`s at the top of both `main.cpp` files. Not tested yet; see PLAN.md
(Phase 5) for making the pins configurable.

## Radio settings (fixed in firmware)

GFSK 38.4 kbps, deviation 20.63 kHz, RX bandwidth 166.7 kHz, AFC on (125 kHz,
triggered on preamble detect), sync `D3 91`, CRC in software. The transmit
frequency comes from `DUCO_TX_FREQ_MHZ` (measured by the sniffer), the power
from `DUCO_TX_POWER_DBM` (default +14 dBm).

## Regulatory

868.0–868.6 MHz (ETSI EN 300 220, sub-band g1) allows **25 mW ERP (+14 dBm)** and
a **1 % duty cycle**. Keep `DUCO_TX_POWER_DBM` at 14 or below. Raising it is
your responsibility, and placement is the better fix for a node that's out of
reach. Each bridge frame is ~4–5 ms on air. A poll every 1.5 s, plus an ACK for
each reply and the occasional retry, comes to roughly 0.6 % of airtime. Don't
shorten the poll interval or add a fast heartbeat; the nodes go quiet if you do
(docs/protocol.md).
