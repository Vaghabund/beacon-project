"""
test_data_roundtrip.py — host-side self-test for the recoverable data layer.

Mirrors the firmware (wifi_data.c + save_bmp_sd in the .ino) byte-for-byte in
Python, writes a real BMP, then recovers it with decode_beacon.py. Validates:
  - payload framing + CRC-16/CCITT-FALSE
  - LSB embed bit-order (MSB-first, r,g,b row-major)
  - the hand-rolled BMP writer's orientation (bottom-up) and channel order (BGR)

No hardware needed.  Run:  python test_data_roundtrip.py
Requires: pillow, numpy
"""
import sys
import numpy as np
from decode_beacon import decode_file, crc16_ccitt_false

W, H = 320, 240


def put_le16(buf, off, v): buf[off] = v & 0xFF; buf[off + 1] = (v >> 8) & 0xFF
def put_le32(buf, off, v):
    buf[off] = v & 0xFF; buf[off + 1] = (v >> 8) & 0xFF
    buf[off + 2] = (v >> 16) & 0xFF; buf[off + 3] = (v >> 24) & 0xFF


def build_payload(nets):          # mirrors data_build_payload()
    out = bytearray(b"WBCN")
    out.append(1)                 # version
    out.append(len(nets))
    for n in nets:
        ssid = n["ssid"].encode("utf-8")[:32]
        out.append(len(ssid))
        out += ssid
        out += n["bssid"]         # 6 bytes
        out.append(n["rssi"] & 0xFF)
        out.append(n["ch"])
    crc = crc16_ccitt_false(bytes(out))
    out.append((crc >> 8) & 0xFF)
    out.append(crc & 0xFF)
    return bytes(out)


def embed(arr, payload):          # mirrors data_embed_lsb() — MSB-first, r,g,b row-major
    flat = arr.reshape(-1).copy()
    bit = 0
    for byte in payload:
        for b in range(7, -1, -1):
            flat[bit] = (flat[bit] & 0xFE) | ((byte >> b) & 1)
            bit += 1
    return flat.reshape(arr.shape)


def save_bmp_manual(path, arr):   # mirrors save_bmp_sd() — bottom-up, BGR, W=320 → pad 0
    img_size = W * 3 * H
    hdr = bytearray(54)
    hdr[0:2] = b"BM"
    put_le32(hdr, 2, 54 + img_size); put_le32(hdr, 10, 54)
    put_le32(hdr, 14, 40); put_le32(hdr, 18, W); put_le32(hdr, 22, H)
    put_le16(hdr, 26, 1); put_le16(hdr, 28, 24)
    put_le32(hdr, 34, img_size); put_le32(hdr, 38, 2835); put_le32(hdr, 42, 2835)
    body = np.ascontiguousarray(arr[::-1, :, ::-1], dtype=np.uint8).tobytes()  # flip rows, RGB→BGR
    with open(path, "wb") as f:
        f.write(hdr); f.write(body)


def main():
    nets = [
        {"ssid": "Home-5G",      "bssid": bytes([0xAA,0xBB,0xCC,0x11,0x22,0x33]), "rssi": -42, "ch": 36},
        {"ssid": "café_münchen", "bssid": bytes([0xDE,0xAD,0xBE,0xEF,0x00,0x01]), "rssi": -67, "ch": 6},
        {"ssid": "",             "bssid": bytes([0x00,0x11,0x22,0x33,0x44,0x55]), "rssi": -88, "ch": 11},
    ]

    rng = np.random.default_rng(1)
    img = rng.integers(0, 256, size=(H, W, 3), dtype=np.uint8)
    img = embed(img, build_payload(nets))
    save_bmp_manual("_roundtrip.bmp", img)

    res = decode_file("_roundtrip.bmp")
    ok = res["count"] == len(nets)
    for want, got in zip(nets, res["networks"]):
        exp = ":".join(f"{b:02X}" for b in want["bssid"])
        match = (got["ssid"] == want["ssid"] and got["bssid"] == exp
                 and got["rssi"] == want["rssi"] and got["channel"] == want["ch"])
        print(f"  {'OK ' if match else 'BAD'} {got}")
        ok = ok and match

    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
