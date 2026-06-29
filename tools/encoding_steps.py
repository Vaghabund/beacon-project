#!/usr/bin/env python3
"""
encoding_steps.py — generate the step-by-step figures for docs/ENCODING.md.

Every "render" figure here is produced by the REAL device encoder (wifi_rings.c,
compiled via rings_preview's ziglang/ctypes bridge), so the walkthrough shows
exactly what the hardware does — not a re-implementation. The diagram figures
(scan table, identity→nibbles, the straightened pixel-sort, the LSB layer) are
drawn from the same data and the real WBCN frame builder.

    pip install ziglang pillow numpy
    python tools/encoding_steps.py            # writes docs/encoding/*.png

Run from the repo root (or anywhere — paths are resolved relative to this file).
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))      # tools/
ROOT = os.path.dirname(HERE)
OUT  = os.path.join(ROOT, "docs", "encoding")
SAMPLE = os.path.join(ROOT, "sample-images", "mountains.jpg")

sys.path.insert(0, HERE)
import rings_preview as rp                              # the real-encoder bridge
from test_data_roundtrip import build_payload, embed    # the real WBCN frame + LSB embed

# ── palette (OSINT terminal) ────────────────────────────────────────────────
BG    = (8, 12, 10)
GREEN = (57, 255, 20)
DIM   = (93, 125, 118)
WHITE = (210, 247, 226)
AMBER = (255, 180, 84)
CYAN  = (65, 214, 255)
RED   = (255, 93, 93)

SC = 2                                                  # upscale factor for the renders

# A small curated scan with a wide signal spread, strongest first.
NETS = [
    {"ssid": "FRITZ!Box 6670",   "bssid": "34:E1:A9:04:2F:E0", "rssi": -47, "channel": 11},
    {"ssid": "Vodafone-FEE3",    "bssid": "A4:43:8C:0A:AD:4C", "rssi": -58, "channel": 1},
    {"ssid": "Shirowska",        "bssid": "9C:C8:FC:31:5D:36", "rssi": -66, "channel": 6},
    {"ssid": "MagentaWLAN-9QE6", "bssid": "20:37:F0:C6:39:80", "rssi": -74, "channel": 4},
    {"ssid": "eduroam",          "bssid": "00:11:22:33:44:55", "rssi": -84, "channel": 9},
]


def font(size):
    for p in ("C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"):
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()

F_TITLE = font(20)
F_BODY  = font(15)
F_SMALL = font(12)
F_TINY  = font(10)


def canvas(w, h):
    img = Image.new("RGB", (w, h), BG)
    return img, ImageDraw.Draw(img)


def title_bar(d, w, n, text, sub=""):
    d.rectangle([0, 0, w, 30], fill=(14, 22, 18))
    d.text((10, 7), f"{n}", font=F_TITLE, fill=GREEN)
    d.text((42, 9), text, font=F_BODY, fill=WHITE)
    if sub:
        tw = d.textlength(sub, font=F_SMALL)
        d.text((w - tw - 10, 10), sub, font=F_SMALL, fill=DIM)


def upscale(img):
    return img.resize((img.width * SC, img.height * SC), Image.NEAREST)


# Bands are widened vs the device default (ring_thickness 30) purely so the
# mechanism reads on a smooth photo — same encoder, exaggerated geometry.
DIDACTIC = dict(rp.DEFAULTS, inner_radius=10, ring_thickness=46, max_displace=100)


def render(nets, cfg=None):
    """Run the real encoder over the sample photo with `nets`. Returns a PIL RGB image."""
    return rp.encode(LIB, SRC, nets, dict(cfg or DIDACTIC))


def framed(render_img, n, title, sub="", caption=""):
    """Wrap a render in a title bar + optional caption strip."""
    up = upscale(render_img)
    cap_h = 0 if not caption else 26 * (caption.count("\n") + 1)
    img, d = canvas(up.width, 30 + up.height + cap_h)
    title_bar(d, img.width, n, title, sub)
    img.paste(up, (0, 30))
    if caption:
        for i, line in enumerate(caption.split("\n")):
            d.text((10, 30 + up.height + 4 + i * 22), line, font=F_SMALL, fill=DIM)
    return img


# ════════════════════════════════════════════════════════════════════════════
def fig1_source():
    img = framed(SRC, "1", "THE FROZEN FRAME", "320 x 240 RGB",
                 "One button press freezes a camera frame. Everything below is painted onto THIS image.")
    img.save(os.path.join(OUT, "01_source.png"))


def fig2_scan():
    rows = sorted(NETS, key=lambda n: -n["rssi"])
    w, h = 640, 64 + len(rows) * 30 + 60
    img, d = canvas(w, h)
    title_bar(d, w, "2", "THE WIFI SCAN", f"{len(rows)} networks")
    d.text((10, 40), "every network in range, sorted by signal (strongest first):", font=F_SMALL, fill=DIM)
    cols = [(10, "SSID"), (250, "BSSID (MAC)"), (470, "CH"), (520, "dBm"), (590, "RING")]
    y = 66
    for x, t in cols:
        d.text((x, y), t, font=F_SMALL, fill=GREEN)
    y += 22
    for i, n in enumerate(rows):
        d.line([10, y - 3, w - 10, y - 3], fill=(20, 34, 28))
        ssid = n["ssid"] or "<hidden>"
        d.text((10, y), ssid[:22], font=F_BODY, fill=WHITE if n["ssid"] else DIM)
        d.text((250, y), n["bssid"], font=F_SMALL, fill=CYAN)
        d.text((470, y), str(n["channel"]), font=F_BODY, fill=AMBER)
        d.text((520, y), str(n["rssi"]), font=F_BODY, fill=GREEN)
        ring = "inner" if i == 0 else ("outer" if i == len(rows) - 1 else str(i + 1))
        d.text((590, y), ring, font=F_SMALL, fill=DIM)
        y += 30
    d.text((10, y + 8), "strongest -> innermost ring. each row becomes one ring of the artwork.",
           font=F_SMALL, fill=DIM)
    img.save(os.path.join(OUT, "02_scan.png"))


def fig3_identity():
    """SSID + BSSID + channel -> each byte -> two digits 0..15 -> per-spoke sort depth."""
    n = NETS[0]
    macb = bytes(int(x, 16) for x in n["bssid"].split(":"))
    syms = [(c, ord(c), "char") for c in n["ssid"][:5]]
    syms += [(f"{b:02X}", b, "mac") for b in macb[:3]]
    syms += [(f"ch{n['channel']}", n["channel"], "ch")]

    cell = 64
    w = 96 + len(syms) * cell
    img, d = canvas(w, 432)
    title_bar(d, w, "3", "WHAT A RING HOLDS", n["ssid"])

    # worked example up top — spell out exactly how one letter becomes two spokes
    d.text((10, 40), "the ring's pattern is the network's identity turned into numbers. worked example:",
           font=F_SMALL, fill=DIM)
    d.text((10, 60), "letter 'F'  ->  its value 70  =  4x16 + 6  ->  two halves: 4 and 6  ->  two spokes",
           font=F_BODY, fill=WHITE)
    d.text((10, 84), "every byte (0..255) splits into two halves, each a single value 0..15 (16 possible levels).",
           font=F_SMALL, fill=DIM)
    d.text((10, 100), "each half's value IS the spoke's sort depth — 0 = barely a flick, 15 = deep across the band:",
           font=F_SMALL, fill=DIM)

    base_y = 318
    kindcol = {"char": WHITE, "mac": CYAN, "ch": AMBER}
    x = 96
    for label, val, kind in syms:
        hi, lo = val // 16, val % 16
        d.text((x + cell / 2 - d.textlength(label, F_BODY) / 2, 138), label, font=F_BODY, fill=kindcol[kind])
        d.text((x + cell / 2 - d.textlength(f"={val}", F_TINY) / 2, 158), f"={val}", font=F_TINY, fill=DIM)
        for j, dig in enumerate((hi, lo)):
            bx = x + j * (cell // 2) + 6
            bw = cell // 2 - 12
            bh = int((dig / 15) * 150) + 3
            d.rectangle([bx, base_y - bh, bx + bw, base_y], fill=GREEN)
            d.text((bx + bw / 2 - d.textlength(str(dig), F_SMALL) / 2, base_y + 5),
                   str(dig), font=F_SMALL, fill=GREEN)
        x += cell
    d.line([92, base_y, w - 6, base_y], fill=(40, 60, 50))
    d.text((10, base_y + 28),
           "each half is one value 0..15 (shown in decimal).  source:  white SSID letter · cyan MAC byte · amber channel.",
           font=F_SMALL, fill=DIM)
    d.text((10, base_y + 46),
           "the same byte anywhere makes the same two spokes — the 'truth' you can read by eye.",
           font=F_SMALL, fill=DIM)

    # standard-technique note + the hex reference (how 10..15 are normally written)
    d.text((10, base_y + 70),
           "a half = one 'nibble' (4 bits). splitting a byte this way is standard — same idea as a hex colour #RRGGBB.",
           font=F_SMALL, fill=DIM)
    hx = "decimal 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15   =   hex 0 1 2 3 4 5 6 7 8 9  A  B  C  D  E  F"
    d.text((10, base_y + 88), hx, font=F_SMALL, fill=AMBER)
    img.save(os.path.join(OUT, "03_identity.png"))


def fig4_pixelsort():
    """The core move, straightened: a strip of pixels sorted by luminance (sort_dir=0)."""
    src = np.asarray(SRC, dtype=np.uint8)
    N = 64
    lumimg = 0.299 * src[:, :, 0] + 0.587 * src[:, :, 1] + 0.114 * src[:, :, 2]
    # pick the highest-contrast horizontal window in the photo so the sort is obvious
    best = None
    for y in range(src.shape[0]):
        for x0 in range(0, src.shape[1] - N, 8):
            seg = lumimg[y, x0:x0 + N]
            rng = float(seg.max() - seg.min())
            if best is None or rng > best[0]:
                best = (rng, y, x0)
    _, y, x0 = best
    strip = src[y, x0:x0 + N].copy()
    lum = lumimg[y, x0:x0 + N]
    order = np.argsort(lum)                              # ascending: dark -> light (sort_dir 0)
    ordered = strip[order]
    lum_sorted = lum[order]

    px, sh = 9, 42
    w = 96 + N * px
    img, d = canvas(w, 30 + 62 + sh + 14 + sh + 16 + 12 + 60)
    title_bar(d, w, "4", "THE CORE MOVE: RADIAL PIXEL-SORT", "shown straightened")
    d.text((10, 40), "a spoke gathers the pixels along a ray and reorders them by brightness (dark -> light).",
           font=F_SMALL, fill=DIM)

    def strip_row(arr, yy, label):
        d.text((10, yy + sh / 2 - 6), label, font=F_SMALL, fill=GREEN)
        for i, p in enumerate(arr):
            d.rectangle([96 + i * px, yy, 96 + i * px + px - 1, yy + sh], fill=tuple(int(v) for v in p))

    y1 = 92
    strip_row(strip, y1, "BEFORE")
    strip_row(ordered, y1 + sh + 14, "AFTER")

    # grayscale brightness bar under AFTER — a clean black->white ramp proves the sort
    gy = y1 + sh + 14 + sh + 6
    d.text((10, gy - 1), "bright", font=F_TINY, fill=DIM)
    for i, l in enumerate(lum_sorted):
        g = int(l)
        d.rectangle([96 + i * px, gy, 96 + i * px + px - 1, gy + 9], fill=(g, g, g))

    d.text((10, gy + 22),
           "the brightness bar rises smoothly left->right: that monotonic ramp is the proof it's sorted.",
           font=F_SMALL, fill=DIM)
    d.text((10, gy + 40),
           "wrap this around a centre — many rays, each sorted to a different length — and you get a ring.",
           font=F_SMALL, fill=DIM)
    img.save(os.path.join(OUT, "04_pixelsort.png"))


def fig5_one_ring():
    img = framed(render(NETS[:1]), "5", "ONE NETWORK = ONE RING", NETS[0]["ssid"],
                 "the identity string is TILED around the band — same byte, same-looking spoke.")
    img.save(os.path.join(OUT, "05_one_ring.png"))


def fig6_reach():
    levels = [(-85, "weak"), (-65, "mid"), (-45, "strong")]
    tiles = []
    for dbm, _ in levels:
        net = dict(NETS[0]); net["rssi"] = dbm
        tiles.append(upscale(render([net])))
    gap = 12
    tw, th = tiles[0].width, tiles[0].height
    w = len(tiles) * tw + (len(tiles) - 1) * gap
    img, d = canvas(w, 30 + th + 36)
    title_bar(d, w, "6", "SIGNAL STRENGTH -> SORT REACH", "same network, three dBm")
    for i, (t, (dbm, name)) in enumerate(zip(tiles, levels)):
        x = i * (tw + gap)
        img.paste(t, (x, 30))
        d.text((x + 6, 30 + th + 8), f"{dbm} dBm · {name}", font=F_SMALL, fill=GREEN)
    img.save(os.path.join(OUT, "06_reach.png"))


def fig7_composite():
    img = framed(render(NETS), "7", "THE FULL BEACON", f"{len(NETS)} networks",
                 "all rings together: innermost = strongest. a one-way portrait of the radio room.")
    img.save(os.path.join(OUT, "07_composite.png"))


def fig8_lsb():
    """The hidden layer: the exact scan as a WBCN frame, written into pixel LSBs."""
    comp = np.asarray(render(NETS), dtype=np.uint8)
    payload = build_payload([
        {"ssid": n["ssid"],
         "bssid": bytes(int(x, 16) for x in n["bssid"].split(":")),
         "rssi": n["rssi"], "ch": n["channel"]} for n in NETS
    ])
    stego = embed(comp.copy(), payload)
    lsb = (stego & 1).astype(np.uint8) * 255            # the bit-plane, amplified

    img_up = upscale(Image.fromarray(stego))
    lsb_up = upscale(Image.fromarray(lsb))
    gap = 12
    w = img_up.width + gap + lsb_up.width
    img, d = canvas(w, 30 + img_up.height + 96)
    title_bar(d, w, "8", "THE HIDDEN LAYER", f"WBCN frame · {len(payload)} bytes")
    img.paste(img_up, (0, 30))
    img.paste(lsb_up, (img_up.width + gap, 30))
    d.text((6, 30 + img_up.height + 6), "saved image (looks untouched)", font=F_SMALL, fill=DIM)
    d.text((img_up.width + gap + 6, 30 + img_up.height + 6),
           "its pixel LSB bit-plane", font=F_SMALL, fill=DIM)

    y = 30 + img_up.height + 30
    d.text((6, y), "exact scan -> framed, CRC-checked, embedded MSB-first in the low bit of each channel:",
           font=F_SMALL, fill=DIM)
    hexs = " ".join(f"{b:02X}" for b in payload[:48])
    d.text((6, y + 20), hexs, font=F_TINY, fill=GREEN)
    d.text((6, y + 38), "magic 'WBCN' ... per-net{ ssid, bssid, rssi, channel } ... crc16   "
                        "(lossless PNG only — JPEG would wipe it)", font=F_TINY, fill=AMBER)
    img.save(os.path.join(OUT, "08_lsb.png"))


def main():
    os.makedirs(OUT, exist_ok=True)
    rp.build_lib()
    global LIB, SRC
    LIB = rp.load_lib()
    SRC = rp.load_image(SAMPLE, LIB.viz_img_w(), LIB.viz_img_h())
    for fn in (fig1_source, fig2_scan, fig3_identity, fig4_pixelsort,
               fig5_one_ring, fig6_reach, fig7_composite, fig8_lsb):
        fn()
        print("wrote", fn.__name__)
    print("done ->", OUT)


if __name__ == "__main__":
    main()
