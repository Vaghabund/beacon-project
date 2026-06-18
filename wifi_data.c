#include "wifi_data.h"
#include <string.h>

// ─────────────────────────────────────────────────────────────────────────────
//  CRC-16/CCITT-FALSE  (poly 0x1021, init 0xFFFF, no reflect, no xorout)
//  Bitwise — n is tiny (< ~500 bytes), no need for a table.
// ─────────────────────────────────────────────────────────────────────────────
uint16_t wbcn_crc16(const uint8_t* data, int len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Build framed payload
// ─────────────────────────────────────────────────────────────────────────────
int data_build_payload(const WifiNetwork* nets, int n,
                       uint8_t* out, int out_max) {
    if (n < 0) n = 0;
    if (n > MAX_NETWORKS) n = MAX_NETWORKS;

    int p = 0;

    // need at least header(6) + crc(2) even for an empty scan
    if (out_max < 8) return -1;

    out[p++] = WBCN_MAGIC0;
    out[p++] = WBCN_MAGIC1;
    out[p++] = WBCN_MAGIC2;
    out[p++] = WBCN_MAGIC3;
    out[p++] = WBCN_VERSION;
    out[p++] = (uint8_t)n;

    for (int i = 0; i < n; i++) {
        const WifiNetwork* net = &nets[i];

        // ssid_len = strlen, clamped to 32
        int sl = 0;
        while (sl < (MAX_SSID_LEN - 1) && net->ssid[sl]) sl++;

        // record needs 1 + sl + 6 + 1 + 1 bytes, plus the trailing crc(2)
        if (p + (1 + sl + 6 + 1 + 1) + 2 > out_max) return -1;

        out[p++] = (uint8_t)sl;
        memcpy(&out[p], net->ssid, sl);          p += sl;
        memcpy(&out[p], net->bssid, 6);          p += 6;
        out[p++] = (uint8_t)net->dbm;            // int8 → byte
        out[p++] = net->channel;
    }

    if (p + 2 > out_max) return -1;
    uint16_t crc = wbcn_crc16(out, p);
    out[p++] = (uint8_t)(crc >> 8);              // big-endian
    out[p++] = (uint8_t)(crc & 0xFF);

    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
//  LSB embed — MSB-first within each byte, channels in r,g,b row-major order.
//  Pixel is a packed 3-byte struct (static_assert in the .ino guarantees it),
//  so the image is just a flat array of channel bytes.
// ─────────────────────────────────────────────────────────────────────────────
int data_embed_lsb(Pixel* img, int n_pixels, const uint8_t* buf, int len) {
    const long cap_bytes = (long)n_pixels * 3;   // one bit per channel byte
    if ((long)len * 8 > cap_bytes) return -1;

    uint8_t* ch = (uint8_t*)img;
    long bit = 0;
    for (int i = 0; i < len; i++) {
        const uint8_t byte = buf[i];
        for (int b = 7; b >= 0; b--) {
            const uint8_t v = (byte >> b) & 1;
            ch[bit] = (uint8_t)((ch[bit] & 0xFE) | v);
            bit++;
        }
    }
    return (int)bit;
}

// ─────────────────────────────────────────────────────────────────────────────
//  LSB extract + validate
//  Reads channel bytes back into whole payload bytes, parses the self-
//  delimiting frame to learn its length, then checks magic + CRC.
// ─────────────────────────────────────────────────────────────────────────────

// pull byte index `idx` of the LSB stream out of the image
static uint8_t read_byte(const uint8_t* ch, long idx) {
    uint8_t v = 0;
    long bit = idx * 8;
    for (int b = 0; b < 8; b++) {
        v = (uint8_t)((v << 1) | (ch[bit] & 1));
        bit++;
    }
    return v;
}

int data_extract_lsb(const Pixel* img, int n_pixels,
                     uint8_t* out, int out_max) {
    const long total_bytes = (long)n_pixels * 3 / 8;  // max readable payload bytes
    const uint8_t* ch = (const uint8_t*)img;

    // Read enough for the header first.
    if (out_max < 6 || total_bytes < 6) return -1;
    for (int i = 0; i < 6; i++) out[i] = read_byte(ch, i);

    if (out[0] != WBCN_MAGIC0 || out[1] != WBCN_MAGIC1 ||
        out[2] != WBCN_MAGIC2 || out[3] != WBCN_MAGIC3) return -1;
    if (out[4] != WBCN_VERSION) return -1;

    const int n = out[5];
    if (n > MAX_NETWORKS) return -1;

    // Walk the records to compute total frame length.
    int p = 6;
    for (int i = 0; i < n; i++) {
        if (p + 1 > out_max || p >= total_bytes) return -1;
        out[p] = read_byte(ch, p);
        const int sl = out[p];
        p += 1;
        const int rec_rest = sl + 6 + 1 + 1;   // ssid + bssid + rssi + ch
        if (p + rec_rest + 2 > out_max || (long)(p + rec_rest + 2) > total_bytes)
            return -1;
        for (int k = 0; k < rec_rest; k++) { out[p] = read_byte(ch, p); p++; }
    }

    // CRC trailer
    if (p + 2 > out_max || (long)(p + 2) > total_bytes) return -1;
    out[p]     = read_byte(ch, p);
    out[p + 1] = read_byte(ch, p + 1);
    const uint16_t stored = (uint16_t)(out[p] << 8) | out[p + 1];
    const uint16_t calc   = wbcn_crc16(out, p);
    if (stored != calc) return -1;

    return p + 2;
}
