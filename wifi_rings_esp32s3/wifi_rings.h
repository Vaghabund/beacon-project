#pragma once
#include <stdint.h>
#include <stdbool.h>

// ─────────────────────────────────────────────
//  Image dimensions — match your display exactly
// ─────────────────────────────────────────────
#define IMG_W        320
#define IMG_H        240
#define IMG_PIXELS   (IMG_W * IMG_H)
#define IMG_BYTES    (IMG_PIXELS * 3)   // RGB888

// ─────────────────────────────────────────────
//  Trig LUT
// ─────────────────────────────────────────────
#define LUT_SIZE     1024
#define LUT_MASK     (LUT_SIZE - 1)

// ─────────────────────────────────────────────
//  Limits
// ─────────────────────────────────────────────
#define MAX_NETWORKS    12
#define MAX_SSID_LEN    33    // 32 chars + null
#define MAX_RAY_LEN     64    // max pixels along one radial ray

// ─────────────────────────────────────────────
//  Types
// ─────────────────────────────────────────────

// RGB888 pixel — matches fmt2rgb888 output layout
typedef struct {
    uint8_t r, g, b;
} Pixel;

// One detected network
typedef struct {
    char    ssid[MAX_SSID_LEN];
    uint8_t bssid[6];   // MAC address — the unique per-AP signature
    int8_t  dbm;        // e.g. -65
    uint8_t channel;
} WifiNetwork;

// Encoder parameters
typedef struct {
    uint8_t inner_radius;     // px — inner edge of ring 0
    uint8_t ring_thickness;   // px — radial depth of each ring band
    uint8_t ring_gap;         // px — gap between rings
    uint8_t max_displace;     // smear as % of band width, strongest signal (≤100)
    uint8_t min_displace;     // smear as % of band width, weakest signal
    uint8_t sort_dir;         // 0=lum_asc  1=lum_desc  2=hue  3=sat
    uint8_t disp_mode;        // 0=radial  1=angular  2=both
} RingConfig;

// Sensible defaults for 320×240 at ~150 PPI.
// The smear is contained within each ring band (no overflow). Signal strength
// fills the band from min_displace% (weak) up to max_displace% (strong). Wider
// bands + a gap give the smear room to read; per-ray detail comes from the
// network fingerprint hash.
static const RingConfig RING_CONFIG_DEFAULT = {
    .inner_radius   = 14,
    .ring_thickness = 30,    // also the max sort depth (hex F at full signal)
    .ring_gap       = 0,     // bands touch — continuous concentric field
    .max_displace   = 100,   // strongest signal: hex F = full band (no overflow)
    .min_displace   = 10,    // weakest still shows a thin smear
    .sort_dir       = 0,
    .disp_mode      = 0,
};

// ─────────────────────────────────────────────
//  API
// ─────────────────────────────────────────────
#ifdef __cplusplus
extern "C" {
#endif

// Call once at boot — fills sin/cos LUT into internal SRAM
void rings_init(void);

// Sort networks strongest-first in place (tiny n, insertion sort)
void rings_sort_networks(WifiNetwork* nets, int n);

// Main encode:
//   src  — RGB888, IMG_W×IMG_H, in PSRAM (read-only)
//   dst  — RGB888, IMG_W×IMG_H, in PSRAM (write)
//   nets — sorted strongest-first
//   n    — number of networks
//   cfg  — encoder config
void rings_encode(
    const Pixel*       src,
    Pixel*             dst,
    const WifiNetwork* nets,
    int                n,
    const RingConfig*  cfg
);

// Convert one RGB888 scan line → RGB565 for SPI DMA
// out must point to internal SRAM (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
void rings_to_rgb565_line(
    const Pixel* src_row,
    uint16_t*    out,
    int          width
);

#ifdef __cplusplus
}
#endif
