#!/usr/bin/env python3
"""
make_sample.py — generate a full device-style capture for testing the decoder.

Produces what the firmware actually saves: a photo with the artistic rings baked
in (real wifi_rings.c encoder) AND the recoverable WBCN payload embedded in the
pixel LSBs, written as a 24-bit BMP. Drop the result into web/index.html, or run
the decoders on it.

    python tools/make_sample.py
    python tools/make_sample.py myphoto.jpg --out output/sample_beacon.bmp

Setup: pip install ziglang pillow numpy
"""
import argparse
import json
import os

import numpy as np

import rings_preview as rp                                   # real C rings encoder (ctypes)
from test_data_roundtrip import build_payload, embed, save_bmp_manual  # verified embed + BMP writer


def to_payload_net(n):
    """mock JSON net -> the dict build_payload() expects."""
    try:
        bssid = bytes(int(x, 16) for x in n.get("bssid", "").split(":"))[:6].ljust(6, b"\x00")
    except ValueError:
        bssid = b"\x00" * 6
    return {"ssid": n.get("ssid", ""), "bssid": bssid,
            "rssi": n.get("rssi", n.get("dbm", -100)), "ch": n.get("channel", 0)}


def main():
    ap = argparse.ArgumentParser(description="Make a device-style beacon BMP (rings + hidden data).")
    ap.add_argument("image", nargs="?",
                    default=os.path.join(rp.ROOT, "sample-images", "mountains.jpg"))
    ap.add_argument("--nets", default=os.path.join(rp.HERE, "mock_wifi_networks.json"))
    ap.add_argument("--out", default=os.path.join(rp.ROOT, "output", "sample_beacon.bmp"))
    args = ap.parse_args()

    rp.build_lib()
    lib = rp.load_lib()
    assert lib.viz_sizeof_network() == 41
    w, h = lib.viz_img_w(), lib.viz_img_h()

    nets_all = json.load(open(args.nets, encoding="utf-8"))
    # device sorts strongest-first and keeps at most MAX_NETWORKS (12)
    nets = sorted(nets_all, key=lambda n: n.get("rssi", n.get("dbm", -100)), reverse=True)[:12]

    src = rp.load_image(args.image, w, h)
    cfg = dict(rp.DEFAULTS, ring_thickness=26, max_displace=48)  # a touch stronger so rings read well
    rings = rp.encode(lib, src, nets, cfg)                       # 1) artistic layer (real C)

    arr = np.asarray(rings, dtype=np.uint8).copy()
    payload = build_payload([to_payload_net(n) for n in nets])   # 2) hidden layer, after rings
    arr = embed(arr, payload)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    save_bmp_manual(args.out, arr)                               # 3) lossless 24-bit BMP

    print(f"wrote {args.out}")
    print(f"  {len(nets)} networks, payload {len(payload)} B, {w}x{h} 24-bit BMP")
    print(f"  drop it into web/index.html, or: python tools/decode_beacon.py {args.out}")


if __name__ == "__main__":
    main()
