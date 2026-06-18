// ─────────────────────────────────────────────────────────────────────────────
//  rings_host.c — host shim to run the REAL firmware encoder on a PC.
//
//  Compiled together with wifi_rings.c into a shared library and called from
//  rings_preview.py via ctypes. This runs the exact same rings_encode() that
//  ships on the device — so the preview is faithful, not a reimplementation.
// ─────────────────────────────────────────────────────────────────────────────
#include "wifi_rings.h"
#include <string.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

EXPORT int viz_img_w(void)            { return IMG_W; }
EXPORT int viz_img_h(void)            { return IMG_H; }
EXPORT int viz_max_networks(void)     { return MAX_NETWORKS; }
// Lets Python assert its packed-record size matches the C struct (no padding).
EXPORT int viz_sizeof_network(void)   { return (int)sizeof(WifiNetwork); }

// src/dst: RGB888, IMG_W*IMG_H*3 bytes.
// nets_packed: n_nets records, each laid out exactly like WifiNetwork.
EXPORT void viz_encode(
    const unsigned char* src, unsigned char* dst,
    const unsigned char* nets_packed, int n_nets,
    int inner_radius, int ring_thickness, int ring_gap,
    int max_displace, int min_displace, int sort_dir, int disp_mode)
{
    static int inited = 0;
    if (!inited) { rings_init(); inited = 1; }

    if (n_nets < 0) n_nets = 0;
    if (n_nets > MAX_NETWORKS) n_nets = MAX_NETWORKS;

    WifiNetwork nets[MAX_NETWORKS];
    memset(nets, 0, sizeof(nets));
    if (n_nets > 0) memcpy(nets, nets_packed, (size_t)n_nets * sizeof(WifiNetwork));
    rings_sort_networks(nets, n_nets);

    RingConfig cfg;
    cfg.inner_radius   = (unsigned char)inner_radius;
    cfg.ring_thickness = (unsigned char)ring_thickness;
    cfg.ring_gap       = (unsigned char)ring_gap;
    cfg.max_displace   = (unsigned char)max_displace;
    cfg.min_displace   = (unsigned char)min_displace;
    cfg.sort_dir       = (unsigned char)sort_dir;
    cfg.disp_mode      = (unsigned char)disp_mode;

    rings_encode((const Pixel*)src, (Pixel*)dst, nets, n_nets, &cfg);
}
