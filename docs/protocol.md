# The Duco RF protocol (as used by a DucoBox and its sensors)

This is what the box and its nodes say to each other on 868 MHz, worked out
from on-air captures of a real DucoBox (mainboard DUCO_BMB_BASIC-2v3) and its
sensors, building on [arnemauer/Duco-RF-protocol](https://github.com/arnemauer/Duco-RF-protocol).
Everything marked **confirmed** has been seen on air and is implemented in
`firmware/lib/duco`. Example frames use the made-up network ID `00 12 34 56`.

## Radio

| | |
|---|---|
| Chip on the Duco side | TI CC1101 |
| Frequency | nominal **868.2943 MHz**; every box and node is a few kHz off (seen: up to ~±15 kHz). The receiver needs **AFC on**. The bridge transmits on the box's measured frequency. |
| Modulation | GFSK, **38.4 kbps**, deviation ≈ 20.6 kHz, no whitening, no Manchester |
| Preamble | 4 bytes `AA` |
| Sync | `D3 91` **sent twice** (`D3 91 D3 91`). Receive on 2 bytes and expect `D3 91` again at the start (it sometimes arrives with a bit error); **transmit all 4**. |
| Length | variable: first byte after sync = payload length (9…~32 seen; accept up to 60) |
| CRC | CRC-16, poly `0x8005`, init `0xFFFF`, over `len + payload`, big-endian after the payload (**confirmed**) |

## Frame

```
len | type | net(4) | addr(3) | rssi | [cmd lengths(2) | commands...] | crc(2)
```

- **net**: the network ID = the box's device ID, 4 bytes.
- **addr**: 24 bits, MSB first: `src(5) dst(5) orig.src(5) orig.dst(5) counter(4)`.
  `src/dst` is this hop, `orig.*` the end-to-end route. Box = address **1**,
  nodes 2–31, `0` = broadcast. (This differs from the byte-per-field table in
  the arnemauer docs; it fits every frame captured.)
- **rssi**: `FF`, unless a repeater filled in the RSSI it received the frame with.
- **cmd lengths**: 4 nibbles = length of up to 4 commands (high nibble first),
  then the commands back to back. Example `20 00` = one 2-byte command; `55 00` =
  two 5-byte commands (a node sometimes sends the same reply twice in one frame).

### Message types

| type | meaning |
|---|---|
| `00` | network message (box broadcast; nodes relay it). Payload 1 byte, `00` normally (once `04`, meaning unknown). |
| `01`–`04` | join / pairing (not implemented yet) |
| `06` | ACK; echoes the counter of the frame it acknowledges |
| `07` | directed message carrying commands |

### Commands (inside type `07`)

| cmd | direction | bytes | meaning |
|---|---|---|---|
| `52 aa` | box → node (or broadcast) | `52 00` | keep-alive / poll; node ACKs. `52 01` also seen. |
| `40 00 rr` | box → node | read parameter register `rr` | |
| `41 00 rr hi lo` | node → box | register value | |
| `42 00 rr ...` | node → box | register does not exist | |
| `48 00 rr hi lo` | box → node | write register (prefixed by `02`) | |
| `22 pp ff tt bb kk` | node → box | status / mode request (below) | |
| `12 mm` | box → node | confirm ventilation mode (below) | |
| `50` / `51` | box → node | LED off / on (unverified) | |

Registers seen: `0x49` temperature (/10 °C), **`0x4A` CO₂ ppm**, **`0x4B` humidity
(/100 %)**, `0x89` CO₂ setpoint (1000 ppm). The real box also reads `0x80`,
`0xA4`, `0xA6`, `0x05` (meaning unknown). A node without a given sensor answers
0 for it.

### `0x22`: node status and button presses (**confirmed**)

`22 <pct> <flags> <temp> <boot> <button>`

- `pct`: the node's ventilation demand, 0–100 %
- `temp`: °C = (byte + 100) / 10
- `boot`: `00`, or `FF` right after the node restarted
- `flags`:
  - bits 0–2: mode: `0` auto, `4` low, `5` middle, `6` high, `7` away
  - `0x08`: **request**: this is a button press asking for that mode
  - `0x20`: **permanent** (long press)
  - `0x80`: seen in a node's boot report
- `button`: `0x00` for a status report; for a press, `0x08 | button number`
  (0 auto, 1, 2, 3), plus `0x04` for a long press

| press | flags | button |
|---|---|---|
| short low | `0C` | `09` |
| short middle | `0D` | `0A` |
| short high | `0E` | `0B` |
| short auto | `08` | `08` |
| long middle | `2D` | `0E` |
| long high | `2E` | `0F` |

### `0x12`: box confirms a mode (**confirmed**)

`12 <mode>`: mode `0/4/5/6/7` as above, **`+0x08` = permanent**. (In `0x22`
permanent is `0x20`; in `0x12` it is `0x08`.) `12 80` was also seen once,
meaning unknown.

## How the real box behaves (reference capture)

This is the part that matters for replacing the box. A box that transmits too
much silences the network: with a network message every second, every node
spent its time relaying it and **never sent its own messages** (no presses, no
register replies). The real box is quiet:

- **Network message every ~82 s**: a broadcast `07 … 1->0  52 00` plus a type
  `00` network message. The nodes relay both. Nothing else is broadcast.
- **One directed message at a time, resent every ~300 ms until ACKed.** A node
  ACKs within ~8 ms. A register reply (`41`) follows ~300 ms after the ACK and is
  ACKed by the box within ~8 ms. A node that never answers gets a few tries.
- **Routing.** The box reaches some nodes through a repeater: it sends
  `1->5 (orig 1->3)` to reach node 3 via node 5, and node 5 relays `5->3 (orig 1->3)`.
  ACKs go back the same way: box → node 3 via 5 = `src 1, dst 5, orig.src 1, orig.dst 3`.
  Nodes pick their own route to the box (direct or via another node) and may switch.
- **A button press**:
  1. the node sends `22 … flags|0x08 … button` to the box
  2. the box ACKs within ~7 ms
  3. ~170 ms later: `12 <mode>` to the node that was pressed, resent until ACKed
  4. then `12 <mode>` to every other node (through repeaters where needed), each resent until ACKed
  5. each node ACKs and then reports `22` status with the new mode (no `0x08`),
     which the box only ACKs, never confirms.

Example (network `00 12 34 56`, node 2 presses "low"):

```
node 2 -> box   07 00 12 34 56 10 44 1e ff 60 00 22 00 0c 70 00 09   (22: pct 0, flags 0C, 21.2 C, button 09)
box -> node 2   06 00 12 34 56 08 82 2e ff                          (ACK, counter 14 echoed)
box -> node 2   07 00 12 34 56 08 82 27 ff 20 00 12 04              (12 04: confirm LOW, counter 7)
node 2 -> box   06 00 12 34 56 10 44 17 ff                          (ACK, counter 7 echoed)
box -> node 3   07 00 12 34 56 09 42 38 ff 20 00 12 04              (via node 5)
...
```

## Open questions

- `0x22` flag bits `0x10`, `0x40`; `12 80`; `52 01` vs `52 00`; network message payload `04`
- registers `0x05`, `0x80`, `0xA4`, `0xA6`
- joining new nodes (types `01`–`04`), so sensors can be added without the old box
- whether a node without a box (dead mainboard) still announces itself on a press,
  which would let you learn the network ID from the sensors alone

Captures that answer any of these are very welcome (see the README, and
sanitize logs with `tools/sanitize_log.py` first).
