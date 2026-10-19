#!/usr/bin/env python3
"""Decode Duco RF frames from a Flipper Zero RAW .sub capture.

Radio (per arnemauer/Duco-RF-protocol): CC1101, 868.294 MHz, GFSK, 38.4 kbps,
sync word 0xD391, variable length, CRC-16, no whitening, no Manchester.

Flipper's RAW capture is the demodulated bit stream as signed durations in
microseconds (positive = high, negative = low). This script rebuilds the bits,
searches for the sync word, reads the length byte + payload + CRC and prints
each frame. Field splitting follows the repo's overview and is UNVERIFIED:
always check the raw hex.

Usage:
    python3 duco_decode.py capture.sub
    python3 duco_decode.py capture.sub --invert      # if nothing is found
    python3 duco_decode.py --selftest
    python3 duco_decode.py --make-test duco_test_frame.sub   # Flipper TX test file
"""
import argparse
import os
import sys
import tempfile

BITRATE = 38400.0
BIT_US = 1e6 / BITRATE
SYNC = 0xD391
MAX_RUN_BITS = 64  # clamp idle gaps

MSG_TYPES = {
    0x00: "network message (box broadcast)",
    0x01: "join request",
    0x02: "assign network ID",
    0x03: "confirm network ID",
    0x04: "assign node address",
    0x06: "ACK",
    0x07: "command/data",
}
CMDS = {
    0x01: "node reset", 0x02: "node save", 0x12: "confirm vent mode",
    0x22: "vent mode", 0x3B: "disjoin", 0x40: "read param",
    0x41: "param reply", 0x42: "param missing", 0x48: "write param",
    0x50: "LED off", 0x51: "LED on", 0x61: "firmware packet",
}
REGS = {0x49: "temperature (/10 C)", 0x4A: "CO2 (ppm)", 0x4B: "humidity (/100 %)"}


def read_raw(path):
    vals = []
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("RAW_Data:"):
                vals += [int(x) for x in line.split(":", 1)[1].split()]
    return vals


def to_bits(vals, invert=False):
    bits = []
    for v in vals:
        level = 1 if v > 0 else 0
        if invert:
            level ^= 1
        n = min(int(round(abs(v) / BIT_US)), MAX_RUN_BITS)
        bits.extend([level] * n)
    return "".join(map(str, bits))


def crc16(data, init=0xFFFF):
    """CC1101 CRC-16, poly 0x8005, MSB first (assumed; see --crc-init)."""
    crc = init
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def bytes_from_bits(s, pos, n):
    if pos + 8 * n > len(s):
        return None
    return bytes(int(s[pos + 8 * i: pos + 8 * i + 8], 2) for i in range(n))


def find_frames(bitstr, crc_init=0xFFFF, max_len=32):
    pat = format(SYNC, "016b")
    frames = []
    i = bitstr.find(pat)
    while i != -1:
        pos = i + 16
        hdr = bytes_from_bits(bitstr, pos, 1)
        if hdr and 1 <= hdr[0] <= max_len:
            n = hdr[0]
            body = bytes_from_bits(bitstr, pos + 8, n + 2)
            if body:
                payload, crc_rx = body[:n], (body[n] << 8) | body[n + 1]
                crc_calc = crc16(bytes([n]) + payload, crc_init)
                frames.append({"bit_offset": i, "len": n, "payload": payload,
                               "crc_ok": crc_rx == crc_calc})
        i = bitstr.find(pat, i + 1)
    return frames


def describe(f):
    p = f["payload"]
    out = [f"@bit {f['bit_offset']:>7}  len={f['len']:>2}  CRC {'OK ' if f['crc_ok'] else 'BAD'}  {p.hex(' ')}"]
    if len(p) >= 11:
        t = p[0]
        out.append(
            f"    type=0x{t:02X} ({MSG_TYPES.get(t, '?')}) net={p[1:5].hex()} "
            f"src={p[5]} dst={p[6]} orig.src={p[7]} orig.dst={p[8]} ctr={p[9]} rssi={p[10]}  [fields unverified]"
        )
        cmd = p[11:]
        if cmd:
            c = cmd[0]
            line = f"    cmd bytes: {cmd.hex(' ')}  (first=0x{c:02X} {CMDS.get(c, '?')})"
            if c in (0x40, 0x41, 0x42) and len(cmd) >= 3:
                line += f"  reg=0x{cmd[2]:02X} {REGS.get(cmd[2], '')}"
                if c == 0x41 and len(cmd) >= 5:
                    line += f"  value={(cmd[3] << 8) | cmd[4]}"
            out.append(line)
    return "\n".join(out)


def decode_file(path, invert=False, crc_init=0xFFFF, show_bad=True):
    vals = read_raw(path)
    if not vals:
        print("No RAW_Data lines found in file.")
        return []
    frames = find_frames(to_bits(vals, invert), crc_init)
    shown = [f for f in frames if f["crc_ok"] or show_bad]
    for f in shown:
        print(describe(f))
    print(f"\n{len(frames)} candidate frame(s), {sum(f['crc_ok'] for f in frames)} with valid CRC.")
    if not frames:
        print("Nothing found: try --invert, check the preset/frequency, or capture closer to a sensor.")
    return frames


# ------------------------------------------------- synthetic frames / test TX
# Same custom preset as setting_user_duco.txt, embedded so the .sub is standalone.
PRESET_DATA = ("02 0D 03 47 04 D3 05 91 06 20 07 0C 08 32 09 00 0A 00 0B 06 0C 00 0D 21 "
               "0E 65 0F 5C 10 CA 11 83 12 10 13 22 14 F8 15 35 16 07 17 30 18 18 19 16 "
               "1A 6C 1B 43 1C 40 1D 91 21 56 22 10 23 E9 24 2A 25 00 26 1F 29 59 2A 7F "
               "2B 3F 2C 81 2D 35 2E 09 00 00 C5 00 00 00 00 00 00 00")

# type 7, network 00089444, src 2 -> dst 1, ctr 13, rssi 0xA0, cmd 0x41 reg 0x4A = 798 ppm
TEST_PAYLOAD = bytes([0x07, 0x00, 0x08, 0x94, 0x44, 0x02, 0x01, 0x02, 0x01, 0x0D, 0xA0,
                      0x41, 0x00, 0x4A, 0x03, 0x1E])


def frame_bits(payload):
    """Bits as a CC1101 with this config would send them: 4 preamble bytes,
    sync word twice (sync mode 30/32), length, payload, CRC-16."""
    body = bytes([len(payload)]) + payload
    crc = crc16(body)
    raw = bytes([0xAA] * 4) + bytes([SYNC >> 8, SYNC & 0xFF] * 2) + body + bytes([crc >> 8, crc & 0xFF])
    return "".join(f"{b:08b}" for b in raw)


def bits_to_durations(bits):
    vals = []
    for b in bits:
        d = BIT_US if b == "1" else -BIT_US
        if vals and (vals[-1] > 0) == (d > 0):
            vals[-1] += d
        else:
            vals.append(d)
    return vals


def write_sub(payload, path, repeats=1, gap_us=2000, preset=False):
    vals = []
    for _ in range(repeats):
        for d in bits_to_durations(frame_bits(payload)) + [-gap_us]:
            if vals and (vals[-1] > 0) == (d > 0):
                vals[-1] += d
            else:
                vals.append(d)
    ints = [int(round(v)) for v in vals]
    with open(path, "w") as f:
        f.write("Filetype: Flipper SubGhz RAW File\nVersion: 1\nFrequency: 868294000\n")
        if preset:
            f.write("Preset: FuriHalSubGhzPresetCustom\nCustom_preset_module: CC1101\n")
            f.write(f"Custom_preset_data: {PRESET_DATA}\n")
        else:
            f.write("Preset: FuriHalSubGhzPresetOok650Async\n")
        f.write("Protocol: RAW\n")
        for i in range(0, len(ints), 500):  # Flipper reads RAW_Data in chunks
            f.write("RAW_Data: " + " ".join(map(str, ints[i:i + 500])) + "\n")


def selftest():
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "synthetic.sub")
        write_sub(TEST_PAYLOAD, path, repeats=3)
        frames = [f for f in find_frames(to_bits(read_raw(path))) if f["crc_ok"]]
    assert len(frames) == 3, frames
    assert all(f["payload"] == TEST_PAYLOAD for f in frames)
    print(describe(frames[0]))
    print("\nselftest OK (checks framing/CRC consistency only, not the real chip's CRC)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="?")
    ap.add_argument("--invert", action="store_true", help="invert signal polarity")
    ap.add_argument("--crc-init", type=lambda x: int(x, 0), default=0xFFFF, help="CRC init value (default 0xFFFF)")
    ap.add_argument("--only-good", action="store_true", help="hide frames with a bad CRC")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--make-test", metavar="OUT.sub",
                    help="write a Flipper .sub that transmits a synthetic Duco frame 20x (for testing a receiver)")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.make_test:
        write_sub(TEST_PAYLOAD, a.make_test, repeats=20, gap_us=100000, preset=True)
        print(f"wrote {a.make_test}")
        return
    if not a.file:
        ap.error("give a .sub file, --make-test or --selftest")
    decode_file(a.file, a.invert, a.crc_init, show_bad=not a.only_good)


if __name__ == "__main__":
    sys.exit(main())
