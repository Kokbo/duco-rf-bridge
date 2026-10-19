#!/usr/bin/env python3
"""Remove Duco network IDs from a duco-rf-bridge / sniffer serial log before you share it.

A network ID is your DucoBox's device ID. Logs also catch your neighbours'
boxes. Every ID found is swapped for a made-up one (AA000001, AA000002, ...),
the same everywhere in the file, so the log still reads consistently. The IDs
appear in two shapes and both are replaced:
    net=0011ABCD   network 0011ABCD   DUCO_NET 0x0011ABCDUL     (8 hex digits)
    07 00 11 ab cd 08 ...                                        (frame bytes)

Frames whose network bytes were replaced no longer match their CRC; the log
lines still say "CRC OK" because that was checked on the device.

Usage:
    python3 sanitize_log.py device-monitor.log              # writes device-monitor.sanitized.log
    python3 sanitize_log.py device-monitor.log -o out.log
    python3 sanitize_log.py device-monitor.log --list       # just show the IDs found
"""
import argparse
import re
import sys

# 8-hex-digit IDs next to a keyword the firmware prints.
ID_PATTERNS = [
    re.compile(r"(?i)(net[= ])([0-9a-f]{8})\b"),
    re.compile(r"(?i)(network )([0-9a-f]{8})\b"),
    re.compile(r"(?i)(DUCO_NET\s+0x)([0-9a-f]{8})"),
    re.compile(r"(?i)(OWN_NET\s*=\s*0x)([0-9a-f]{8})"),
]


# Frame dumps: "CRC OK   07 00 11 ab cd ..." and "no valid frame in 50 bytes: d3 91 0d 07 00 11 ab cd ...".
# The network ID is the 4 bytes after the type byte (after the length byte, for raw dumps).
FRAME_OK = re.compile(r"CRC (?:OK|BAD)\s+((?:[0-9a-f]{2} ?)+)", re.IGNORECASE)
RAW_DUMP = re.compile(r"no valid frame in \d+ bytes:((?: [0-9a-f]{2})+)", re.IGNORECASE)


def find_ids(text):
    ids = []

    def add(v):
        v = v.upper()
        if v != "00000000" and v not in ids:
            ids.append(v)

    for pat in ID_PATTERNS:
        for m in pat.finditer(text):
            add(m.group(2))
    for m in FRAME_OK.finditer(text):
        b = m.group(1).split()
        if len(b) >= 5 and b[0] in ("00", "06", "07"):
            add("".join(b[1:5]))
    for m in RAW_DUMP.finditer(text):
        b = m.group(1).split()
        if len(b) >= 2 and b[0].lower() == "d3":
            b = b[2:]                      # doubled sync: D3 xx first
        if len(b) >= 6 and b[1] in ("00", "06", "07"):
            add("".join(b[2:6]))
    return ids


def spaced(hex8, upper):
    pairs = [hex8[i:i + 2] for i in range(0, 8, 2)]
    return " ".join(p.upper() if upper else p.lower() for p in pairs)


def sanitize(text, ids):
    mapping = {real: "AA%06X" % (i + 1) for i, real in enumerate(ids)}
    for real, fake in mapping.items():
        # contiguous form, any case
        text = re.sub(re.escape(real), fake, text, flags=re.IGNORECASE)
        # spaced frame bytes, as the firmware prints them (lower case) and upper case
        for upper in (False, True):
            text = text.replace(spaced(real, upper), spaced(fake, upper))
    return text, mapping


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("-o", "--out")
    ap.add_argument("--list", action="store_true", help="only list the network IDs found")
    args = ap.parse_args()

    with open(args.log, encoding="utf-8", errors="replace") as f:
        text = f.read()
    ids = find_ids(text)
    if args.list:
        for i in ids:
            print(i)
        return 0
    if not ids:
        print("No network IDs found; nothing to replace.", file=sys.stderr)
    out, mapping = sanitize(text, ids)
    leftovers = [real for real in ids if real.lower() in out.lower() or spaced(real, False) in out]
    if leftovers:
        print("WARNING: still present after sanitizing: " + ", ".join(leftovers), file=sys.stderr)
        return 1
    dest = args.out or re.sub(r"(\.\w+)?$", r".sanitized\1", args.log, count=1)
    with open(dest, "w", encoding="utf-8", newline="") as f:
        f.write(out)
    for real, fake in mapping.items():
        print("%s -> %s" % (real[:2] + "......", fake))
    print("written: " + dest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
