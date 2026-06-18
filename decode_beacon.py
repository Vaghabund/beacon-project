#!/usr/bin/env python3
"""
decode_beacon.py — recover the hidden WiFi scan from a beacon BMP.

The device hides the exact scan data in the pixel LSBs of the saved image (the
recoverable layer that lives alongside the visible "rings" artwork). This reads
it back.

Frame format — must match wifi_data.h / wifi_data.c on the device:

    magic 'WBCN' | version(1) | n_nets(1)
    per net: ssid_len(1) | ssid | bssid[6] | rssi(int8) | channel(1)
    crc16 (CCITT-FALSE, big-endian) over every preceding byte

LSBs are read MSB-first per byte, walking channels in r,g,b row-major order —
the same order the firmware embeds them. The image MUST be a lossless format
(BMP/PNG); a JPEG re-save destroys the LSBs.

Usage:
    python decode_beacon.py beacon_0001.bmp
    python decode_beacon.py *.bmp --json

Requires: pillow, numpy   (pip install pillow numpy)
"""

import argparse
import glob
import json
import sys

import numpy as np
from PIL import Image

MAGIC = b"WBCN"
VERSION = 1


def crc16_ccitt_false(data: bytes) -> int:
    """CRC-16/CCITT-FALSE — poly 0x1021, init 0xFFFF. Matches wbcn_crc16()."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def extract_lsb_bytes(path: str, max_bytes: int = 2048) -> bytes:
    """Pull the first max_bytes of the LSB stream out of the image.

    Order matches the firmware: channels flattened r,g,b row-major (top-down),
    8 LSBs per output byte, MSB first.
    """
    img = np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)
    flat = img.reshape(-1)                       # [y][x][c] in C order = r,g,b row-major
    n_bits = min(max_bytes * 8, flat.size - (flat.size % 8))
    bits = (flat[:n_bits] & 1).astype(np.uint8)
    # pack 8 bits -> 1 byte, MSB first
    return np.packbits(bits).tobytes()


def parse_frame(buf: bytes):
    """Parse the self-delimiting frame. Raises ValueError on any mismatch."""
    if buf[:4] != MAGIC:
        raise ValueError(f"no WBCN magic (found {buf[:4]!r}) — not a beacon image, "
                         "or it was re-saved lossily")
    ver = buf[4]
    if ver != VERSION:
        raise ValueError(f"unsupported version {ver} (decoder expects {VERSION})")
    n = buf[5]

    p = 6
    nets = []
    for _ in range(n):
        ssid_len = buf[p]; p += 1
        ssid = buf[p:p + ssid_len]; p += ssid_len
        bssid = buf[p:p + 6]; p += 6
        rssi = buf[p] - 256 if buf[p] > 127 else buf[p]; p += 1   # int8
        channel = buf[p]; p += 1
        nets.append({
            "ssid": ssid.decode("utf-8", errors="replace"),
            "bssid": ":".join(f"{b:02X}" for b in bssid),
            "rssi": rssi,
            "channel": channel,
        })

    stored = (buf[p] << 8) | buf[p + 1]
    calc = crc16_ccitt_false(buf[:p])
    if stored != calc:
        raise ValueError(f"CRC mismatch (stored 0x{stored:04X}, computed 0x{calc:04X}) "
                         "— payload corrupted")
    return {"version": ver, "count": n, "networks": nets}


def decode_file(path: str):
    buf = extract_lsb_bytes(path)
    return parse_frame(buf)


def print_human(path: str, result: dict):
    print(f"\n{path} — {result['count']} network(s), CRC OK")
    if not result["networks"]:
        return
    print(f"  {'SSID':<24} {'BSSID':<18} {'RSSI':>5} {'CH':>3}")
    print(f"  {'-'*24} {'-'*17} {'-'*5} {'-'*3}")
    for net in result["networks"]:
        ssid = net["ssid"] if net["ssid"] else "<hidden>"
        print(f"  {ssid:<24} {net['bssid']:<18} {net['rssi']:>5} {net['channel']:>3}")


def main():
    ap = argparse.ArgumentParser(description="Recover hidden WiFi scan from beacon BMP(s).")
    ap.add_argument("paths", nargs="+", help="BMP file(s) or glob(s)")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of a table")
    args = ap.parse_args()

    files = []
    for pat in args.paths:
        files.extend(glob.glob(pat))
    if not files:
        print("no matching files", file=sys.stderr)
        sys.exit(1)

    results, had_error = {}, False
    for path in files:
        try:
            res = decode_file(path)
            results[path] = res
            if not args.json:
                print_human(path, res)
        except (ValueError, OSError) as e:
            had_error = True
            if args.json:
                results[path] = {"error": str(e)}
            else:
                print(f"\n{path} — FAILED: {e}", file=sys.stderr)

    if args.json:
        print(json.dumps(results, indent=2))
    sys.exit(1 if had_error else 0)


if __name__ == "__main__":
    main()
