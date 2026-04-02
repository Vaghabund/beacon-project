/**
 * @file image_gen.c
 * @brief WiFi-ring overlay renderer (C port of wifi_visualizer.py).
 *
 * Algorithm (mirrors Python implementation):
 *   1. Sort APs by RSSI descending (done by wifi_scan_run already).
 *   2. Compute ring radii: strongest → min_radius, weakest → max_radius.
 *   3. For each AP, distribute `num_pixels` dots around the ring.
 *   4. Each dot's colour = HSV(ssid_hue, saturation, value) where value is
 *      modulated by the metadata byte at that dot's position.
 *   5. Draw dots from outermost → innermost so inner rings composite on top.
 */
#include "image_gen.h"
#include "config.h"

#include "mbedtls/md5.h"
#include "esp_log.h"

#include <math.h>
#include <string.h>
#include <stdint.h>

static const char *TAG = "image_gen";

// ─── Helpers ───────────────────────────────────────────────────────────────

// HSV → RGB888  (h, s, v all in [0, 1])
static void hsv_to_rgb(float h, float s, float v,
                       uint8_t *r_out, uint8_t *g_out, uint8_t *b_out)
{
    float r = 0, g = 0, b = 0;

    if (s == 0.0f) {
        r = g = b = v;
    } else {
        int   i = (int)(h * 6.0f);
        float f = h * 6.0f - (float)i;
        float p = v * (1.0f - s);
        float q = v * (1.0f - f * s);
        float t = v * (1.0f - (1.0f - f) * s);

        switch (i % 6) {
            case 0: r = v; g = t; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = t; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = t; g = p; b = v; break;
            case 5: r = v; g = p; b = q; break;
            default: break;
        }
    }

    *r_out = (uint8_t)(r * 255.0f);
    *g_out = (uint8_t)(g * 255.0f);
    *b_out = (uint8_t)(b * 255.0f);
}

// Deterministic hue from SSID: MD5(ssid) → uint32 → mod 360 / 360
static float ssid_to_hue(const char *ssid)
{
    uint8_t digest[16];
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts_ret(&ctx);
    mbedtls_md5_update_ret(&ctx, (const uint8_t *)ssid, strlen(ssid));
    mbedtls_md5_finish_ret(&ctx, digest);
    mbedtls_md5_free(&ctx);

    // Use first four bytes as a big-endian uint32
    uint32_t hash = ((uint32_t)digest[0] << 24) |
                    ((uint32_t)digest[1] << 16) |
                    ((uint32_t)digest[2] <<  8) |
                     (uint32_t)digest[3];

    return (float)(hash % 360u) / 360.0f;
}

// Normalise RSSI to [0, 1];  -30 dBm → 1.0,  -90 dBm → 0.0
static float normalize_rssi(int8_t rssi)
{
    const float min_r = -90.0f;
    const float max_r = -30.0f;
    float n = ((float)rssi - min_r) / (max_r - min_r);
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;
    return n;
}

// Build a compact metadata byte-string for an AP (mirrors to_metadata_string())
static int ap_to_metadata(const wifi_ap_record_t *ap,
                           uint8_t *buf, int buf_size)
{
    return snprintf((char *)buf, (size_t)buf_size,
                    "SSID:%s|BSSID:%02x:%02x:%02x:%02x:%02x:%02x|CH:%d",
                    (const char *)ap->ssid,
                    ap->bssid[0], ap->bssid[1], ap->bssid[2],
                    ap->bssid[3], ap->bssid[4], ap->bssid[5],
                    (int)ap->primary);
}

// Write a square pixel block of size (2*r+1)² centred on (cx, cy)
static inline void paint_dot(uint8_t *rgb888,
                              int cx, int cy, int radius,
                              uint8_t r, uint8_t g, uint8_t b)
{
    for (int dy = -radius; dy <= radius; dy++) {
        int ny = cy + dy;
        if (ny < 0 || ny >= FRAME_HEIGHT) continue;
        for (int dx = -radius; dx <= radius; dx++) {
            int nx = cx + dx;
            if (nx < 0 || nx >= FRAME_WIDTH) continue;
            int off = (ny * FRAME_WIDTH + nx) * 3;
            rgb888[off + 0] = r;
            rgb888[off + 1] = g;
            rgb888[off + 2] = b;
        }
    }
}

// ─── Public API ────────────────────────────────────────────────────────────

void image_gen_apply_wifi_rings(uint8_t *rgb888,
                                const wifi_ap_record_t *aps,
                                int num_aps)
{
    if (!rgb888 || !aps || num_aps <= 0) {
        return;
    }

    const int cx = FRAME_WIDTH  / 2;
    const int cy = FRAME_HEIGHT / 2;

    // Reserve 20 px padding; innermost ring sits at min_radius
    const float max_radius = (float)(FRAME_WIDTH < FRAME_HEIGHT
                                         ? FRAME_WIDTH : FRAME_HEIGHT) / 2.0f - 20.0f;
    const float min_radius = 20.0f;
    const float gap = (num_aps > 1)
                          ? (max_radius - min_radius) / (float)(num_aps - 1)
                          : 0.0f;

    // Metadata scratch buffer (enough for SSID+BSSID+channel string)
    uint8_t meta[256];

    // Draw from outermost → innermost so stronger (inner) rings always win
    for (int idx = num_aps - 1; idx >= 0; idx--) {
        const wifi_ap_record_t *ap = &aps[idx];
        float norm        = normalize_rssi(ap->rssi);
        float ring_radius = min_radius + (float)idx * gap;
        float hue         = ssid_to_hue((const char *)ap->ssid);

        int meta_len = ap_to_metadata(ap, meta, (int)sizeof(meta));
        if (meta_len <= 0) meta_len = 1;

        // Thickness and dot radius scale with signal strength
        int dot_radius  = 2 + (int)(6.0f * norm);      // 2–8 px
        int num_pixels  = 20 + (int)(60.0f * norm);     // 20–80 dots

        ESP_LOGD(TAG,
                 "Ring %d: SSID=\"%s\" rssi=%d norm=%.2f r=%.1f dots=%d",
                 idx, (char *)ap->ssid, ap->rssi, norm, ring_radius, num_pixels);

        // Precompute the angle step for this ring to avoid redundant division
        float angle_step = (float)(2.0 * M_PI) / (float)num_pixels;

        for (int i = 0; i < num_pixels; i++) {
            float angle    = angle_step * (float)i;
            uint8_t byte_v = meta[i % meta_len];

            float sat = 0.5f + 0.5f * norm;
            float val = byte_v / 255.0f * (0.6f + 0.4f * norm);
            if (val < 0.15f) val = 0.15f;

            uint8_t r, g, b;
            hsv_to_rgb(hue, sat, val, &r, &g, &b);

            int px = cx + (int)(ring_radius * cosf(angle));
            int py = cy + (int)(ring_radius * sinf(angle));

            paint_dot(rgb888, px, py, dot_radius, r, g, b);
        }
    }
}

void image_gen_rgb888_to_rgb565(const uint8_t *rgb888,
                                uint16_t      *rgb565,
                                int            pixels)
{
    for (int i = 0; i < pixels; i++) {
        uint8_t r = rgb888[i * 3 + 0];
        uint8_t g = rgb888[i * 3 + 1];
        uint8_t b = rgb888[i * 3 + 2];
        // Pack into RGB565: RRRRRGGGGGGBBBBB
        rgb565[i] = (uint16_t)(((r >> 3) << 11) |
                               ((g >> 2) <<  5) |
                               ( b >> 3));
    }
}
