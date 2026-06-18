#!/usr/bin/env python3
"""
rings_preview.py — preview the wifi_rings encoder on your laptop.

Compiles the REAL firmware encoder (wifi_rings.c, via `python -m ziglang cc`)
into a shared library and runs it over a photo with mock WiFi data, so you can
see and tune the rings look without flashing the device. Same C as the board.

Examples:
    python rings_preview.py                          # sample photo + mock nets
    python rings_preview.py myphoto.jpg              # your photo
    python rings_preview.py --ring-thickness 30 --max-displace 50
    python rings_preview.py --sort-dir 2             # 0=dark 1=bright 2=hue 3=sat
    python rings_preview.py --sweep                  # 2x2 grid over all sort_dir
    python rings_preview.py --rebuild                # force recompile the lib

Setup (one time):  pip install ziglang pillow numpy
"""
import argparse
import ctypes
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageOps

HERE = os.path.dirname(os.path.abspath(__file__))   # tools/
ROOT = os.path.dirname(HERE)                         # repo root
FW = os.path.join(ROOT, "wifi_rings_esp32s3")        # Arduino sketch — firmware C lives here
LIB = os.path.join(HERE, "rings_host.dll" if os.name == "nt" else "librings_host.so")
SRCS = [os.path.join(HERE, "rings_host.c"), os.path.join(FW, "wifi_rings.c")]

# RING_CONFIG_DEFAULT, mirrored from wifi_rings.h
DEFAULTS = dict(inner_radius=18, ring_thickness=22, ring_gap=2,
                max_displace=36, min_displace=2, sort_dir=0, disp_mode=0)


def build_lib(force=False):
    fresh = (os.path.exists(LIB) and not force
             and all(os.path.getmtime(LIB) >= os.path.getmtime(s) for s in SRCS))
    if fresh:
        return
    cmd = [sys.executable, "-m", "ziglang", "cc", "-shared", "-O2", "-I", FW,
           *SRCS, "-o", LIB, "-lm"]
    print("building:", " ".join(cmd[2:]))
    subprocess.run(cmd, check=True)


def load_lib():
    lib = ctypes.CDLL(LIB)
    lib.viz_img_w.restype = ctypes.c_int
    lib.viz_img_h.restype = ctypes.c_int
    lib.viz_sizeof_network.restype = ctypes.c_int
    lib.viz_encode.restype = None
    lib.viz_encode.argtypes = [
        ctypes.c_char_p, ctypes.c_char_p,           # src, dst
        ctypes.c_char_p, ctypes.c_int,              # nets_packed, n_nets
        ctypes.c_int, ctypes.c_int, ctypes.c_int,   # inner, thickness, gap
        ctypes.c_int, ctypes.c_int,                 # max_disp, min_disp
        ctypes.c_int, ctypes.c_int,                 # sort_dir, disp_mode
    ]
    return lib


def pack_net(n):
    """One WifiNetwork record: char ssid[33] | uint8 bssid[6] | int8 dbm | uint8 ch."""
    ssid = n.get("ssid", "").encode("utf-8")[:32].ljust(33, b"\x00")
    bs = n.get("bssid", "00:00:00:00:00:00")
    try:
        bssid = bytes(int(b, 16) for b in bs.split(":"))[:6].ljust(6, b"\x00")
    except ValueError:
        bssid = b"\x00" * 6
    dbm = n.get("rssi", n.get("dbm", -100)) & 0xFF      # int8 two's complement
    ch = n.get("channel", 0) & 0xFF
    return ssid + bssid + bytes([dbm, ch])


def load_image(path, w, h):
    img = Image.open(path).convert("RGB")
    return ImageOps.fit(img, (w, h), Image.LANCZOS)     # crop-to-fill, keep aspect


def encode(lib, src_img, nets, cfg):
    w, h = src_img.size
    src = src_img.tobytes()                              # RGB888
    dst = ctypes.create_string_buffer(len(src))
    packed = b"".join(pack_net(n) for n in nets)
    lib.viz_encode(src, dst, packed, len(nets),
                   cfg["inner_radius"], cfg["ring_thickness"], cfg["ring_gap"],
                   cfg["max_displace"], cfg["min_displace"],
                   cfg["sort_dir"], cfg["disp_mode"])
    return Image.frombytes("RGB", (w, h), dst.raw)


def main():
    ap = argparse.ArgumentParser(description="Preview the wifi_rings encoder on a photo.")
    ap.add_argument("image", nargs="?",
                    default=os.path.join(ROOT, "sample-images", "mountains.jpg"))
    ap.add_argument("--nets", default=os.path.join(HERE, "mock_wifi_networks.json"),
                    help="JSON list of networks (ssid/rssi/bssid/channel)")
    ap.add_argument("--out", default=os.path.join(ROOT, "output", "rings_preview.png"))
    for k, v in DEFAULTS.items():
        ap.add_argument("--" + k.replace("_", "-"), type=int, default=v)
    ap.add_argument("--sweep", action="store_true",
                    help="render a 2x2 grid over sort_dir 0..3")
    ap.add_argument("--rebuild", action="store_true", help="force recompile the lib")
    args = ap.parse_args()

    build_lib(force=args.rebuild)
    lib = load_lib()

    assert lib.viz_sizeof_network() == 41, \
        f"WifiNetwork is {lib.viz_sizeof_network()}B, expected 41 (struct padding?)"
    w, h = lib.viz_img_w(), lib.viz_img_h()

    nets = json.load(open(args.nets, encoding="utf-8"))
    src_img = load_image(args.image, w, h)
    cfg = {k: getattr(args, k) for k in DEFAULTS}
    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    if args.sweep:
        labels = {0: "dark-inward", 1: "bright-inward", 2: "hue", 3: "saturation"}
        tiles = []
        for sd in range(4):
            c = dict(cfg, sort_dir=sd)
            tile = encode(lib, src_img, nets, c).convert("RGB")
            d = ImageDraw.Draw(tile)
            cap = f"sort_dir={sd} ({labels[sd]})"
            d.rectangle([0, 0, len(cap) * 6 + 6, 12], fill=(0, 0, 0))
            d.text((3, 2), cap, fill=(255, 255, 255))
            tiles.append(tile)
        grid = Image.new("RGB", (w * 2 + 4, h * 2 + 4), (30, 30, 30))
        for i, t in enumerate(tiles):
            grid.paste(t, ((i % 2) * (w + 4), (i // 2) * (h + 4)))
        out = args.out.replace(".png", "_sweep.png")
        grid.save(out)
        print(f"saved {out}  ({len(nets)} networks, {w}x{h} tiles)")
    else:
        encode(lib, src_img, nets, cfg).save(args.out)
        print(f"saved {args.out}  ({len(nets)} networks, cfg={cfg})")


if __name__ == "__main__":
    main()
