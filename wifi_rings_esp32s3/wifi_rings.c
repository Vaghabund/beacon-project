#include "wifi_rings.h"
#include <string.h>
#include <math.h>

// ─────────────────────────────────────────────
//  Sin/Cos LUT — internal SRAM, hot path
//  Placed in .data section via IRAM_ATTR on ESP32
// ─────────────────────────────────────────────
static float g_cos[LUT_SIZE];
static float g_sin[LUT_SIZE];

void rings_init(void) {
    for (int i = 0; i < LUT_SIZE; i++) {
        float a = (2.0f * 3.14159265f * i) / LUT_SIZE;
        g_cos[i] = cosf(a);
        g_sin[i] = sinf(a);
    }
}

// Map ray index ai out of total_rays → LUT index
static inline float lcos(int ai, int total) {
    return g_cos[((int64_t)ai * LUT_SIZE / total) & LUT_MASK];
}
static inline float lsin(int ai, int total) {
    return g_sin[((int64_t)ai * LUT_SIZE / total) & LUT_MASK];
}

// ─────────────────────────────────────────────
//  Sort keys
// ─────────────────────────────────────────────
static inline float key_lum(uint8_t r, uint8_t g, uint8_t b) {
    return 0.299f*r + 0.587f*g + 0.114f*b;
}
static inline float key_hue(uint8_t r, uint8_t g, uint8_t b) {
    float nr=r/255.0f, ng=g/255.0f, nb=b/255.0f;
    float mx=nr>ng?(nr>nb?nr:nb):(ng>nb?ng:nb);
    float mn=nr<ng?(nr<nb?nr:nb):(ng<nb?ng:nb);
    float d=mx-mn;
    if (d==0.0f) return 0.0f;
    if (mx==nr) return fmodf((ng-nb)/d+6.0f,6.0f);
    if (mx==ng) return (nb-nr)/d+2.0f;
    return (nr-ng)/d+4.0f;
}
static inline float key_sat(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t mx=r>g?(r>b?r:b):(g>b?g:b);
    uint8_t mn=r<g?(r<b?r:b):(g<b?g:b);
    return mx==0?0.0f:(float)(mx-mn)/mx;
}
static inline float sort_key(uint8_t r, uint8_t g, uint8_t b, uint8_t dir) {
    switch(dir) {
        case 0: return  key_lum(r,g,b);
        case 1: return -key_lum(r,g,b);
        case 2: return  key_hue(r,g,b);
        default: return key_sat(r,g,b);
    }
}

// ─────────────────────────────────────────────
//  Insertion sort — fast for small n (< ~30)
//  Operates on stack array, zero heap
// ─────────────────────────────────────────────
typedef struct { Pixel px; float key; } SE; // sort entry

static void isort(SE* a, int n) {
    for (int i=1; i<n; i++) {
        SE tmp=a[i]; int j=i-1;
        while (j>=0 && a[j].key > tmp.key) { a[j+1]=a[j]; j--; }
        a[j+1]=tmp;
    }
}

// ─────────────────────────────────────────────
//  Network identity → symbol string → per-ray sort depth
//  The visible pattern is a DIRECT readout of the network's data, not a hash:
//  the SSID, BSSID and channel are concatenated into one symbol string, that
//  string is mapped around the ring, and each symbol picks a sort depth. So the
//  same symbol always makes the same-looking spoke within a band — "a truth in
//  there" you could eyeball, without being machine-readable.
//  (Exact, recoverable data still lives in the LSB layer; this is the artwork.)
//
//  Parametric:
//    DEPTH_LEVELS    — how many distinct depth steps a symbol can pick (coarser
//                      = fewer, more obviously-equal shapes)
//    PATTERN_STRETCH — 1: the string wraps the ring exactly once (each symbol is
//                      one arc, no repeat); 0: the string tiles around the ring
// ─────────────────────────────────────────────
#define DEPTH_LEVELS    16   // distinct depth steps a spoke can take (digit base)
#define BYTE_DIGITS      2   // each identity byte → this many base-DEPTH_LEVELS digits
                             // base-16 × 2 = the two hex nibbles: full-range, no collisions
#define PATTERN_STRETCH  0
// upper bound on emitted symbols: (SSID + BSSID[6] + channel[1]) × digits
#define MAX_SYMBOLS     ((MAX_SSID_LEN + 7) * BYTE_DIGITS)

// Route A (1): band thickness AND sort depth scale with signal — strong = thick,
//              deeply-sorted band; weak = thin, shallow. Rings stack by thickness.
// Route B (0): fixed thickness, only sort depth scales with signal.
#define THICK_SCALES_SIGNAL 0

// Build the symbol string from the identity. Each byte is expanded to BYTE_DIGITS
// digits in base DEPTH_LEVELS, most-significant first — e.g. 'S'=83 → 0,8,3. Three
// decimal digits represent any 0..255 byte uniquely, so distinct bytes never
// collide (unlike byte%10), and every digit drives its own spoke, tripling the
// detail packed around the ring. dBm is left out — it already drives ring size.
static int net_symbols(const WifiNetwork* net, uint8_t* out, int max) {
    uint8_t raw[MAX_SSID_LEN + 7];
    int m = 0;
    for (int i = 0; i < MAX_SSID_LEN && net->ssid[i]; i++) raw[m++] = (uint8_t)net->ssid[i];
    for (int i = 0; i < 6; i++) raw[m++] = net->bssid[i];
    raw[m++] = net->channel;

    int n = 0;
    for (int i = 0; i < m; i++) {
        for (int k = BYTE_DIGITS - 1; k >= 0 && n < max; k--) {
            int div = 1;
            for (int p = 0; p < k; p++) div *= DEPTH_LEVELS;
            out[n++] = (uint8_t)((raw[i] / div) % DEPTH_LEVELS);   // one base-N digit
        }
    }
    return n;
}

// One symbol (already a 0..DEPTH_LEVELS-1 level) → sort depth (px), fraction of
// reach. BASE_FILL is the minimum fraction every spoke sorts (0 = pure, so a low
// hex makes a short spoke and the band's outer part shows as a gap; raise toward
// 1 to connect the bands — the hex then varies length on top of a filled base).
#define BASE_FILL 0.0f
static inline int symbol_depth(uint8_t level, float reach) {
    const float t = BASE_FILL
                  + (1.0f - BASE_FILL) * ((float)level / (float)(DEPTH_LEVELS - 1));
    return (int)(t * reach + 0.5f);
}

// ─────────────────────────────────────────────
//  Nearest-neighbour sample from frozen src
// ─────────────────────────────────────────────
static inline Pixel sample(const Pixel* src, int x, int y) {
    if (x<0) x=0; else if (x>=IMG_W) x=IMG_W-1;
    if (y<0) y=0; else if (y>=IMG_H) y=IMG_H-1;
    return src[y*IMG_W+x];
}

// ─────────────────────────────────────────────
//  Per-ring angle offsets — irrational values
//  break cos/sin 4-fold symmetry axes
// ─────────────────────────────────────────────
static const float k_offsets[12] = {
    0.37f,1.13f,2.09f,3.31f,4.57f,5.83f,
    0.97f,2.71f,1.61f,3.89f,0.53f,4.19f
};

// ─────────────────────────────────────────────
//  Network sort — insertion, n is tiny
// ─────────────────────────────────────────────
void rings_sort_networks(WifiNetwork* nets, int n) {
    for (int i=1; i<n; i++) {
        WifiNetwork tmp=nets[i]; int j=i-1;
        while (j>=0 && nets[j].dbm < tmp.dbm) { nets[j+1]=nets[j]; j--; }
        nets[j+1]=tmp;
    }
}

// ─────────────────────────────────────────────
//  Main encode
// ─────────────────────────────────────────────
void rings_encode(
    const Pixel*       src,
    Pixel*             dst,
    const WifiNetwork* nets,
    int                n_nets,
    const RingConfig*  cfg)
{
    // start with exact copy — unsorted pixels stay original
    memcpy(dst, src, IMG_PIXELS * sizeof(Pixel));

    const float cx   = IMG_W * 0.5f;
    const float cy   = IMG_H * 0.5f;
    const float maxr = (IMG_W < IMG_H ? IMG_W : IMG_H) * 0.90f;

    // stack-allocated working buffers — reused every ray, zero heap
    SE      ray_buf[MAX_RAY_LEN];
    int     dest_idx[MAX_RAY_LEN];

#if THICK_SCALES_SIGNAL
    float ring_inner = (float)cfg->inner_radius;   // accumulates signal-scaled thicknesses
#endif

    for (int ri = 0; ri < n_nets && ri < MAX_NETWORKS; ri++) {

        const WifiNetwork* net = &nets[ri];
        float sig_t = ((float)net->dbm + 100.0f) / 70.0f;
        if (sig_t < 0.0f) sig_t = 0.0f;
        if (sig_t > 1.0f) sig_t = 1.0f;

#if THICK_SCALES_SIGNAL
        // Route A: band thickness tracks signal (absolute dBm); the whole band is
        // available to the symbol pattern, so sort depth scales with signal via the
        // band width. Rings stack by accumulating each variable thickness.
        float band = sig_t * (float)cfg->ring_thickness;
        if (band < 3.0f) band = 3.0f;                      // floor so weak rings still show
        const float inner = ring_inner;
        const float reach = band;                          // whole (signal-scaled) band usable
#else
        // Route B: fixed thickness; only the sort depth scales with signal. reach
        // is a signal-driven fraction of the fixed band — min_displace% (weak) to
        // max_displace% (strong), as percentages of band width.
        const float frac_lo = (float)cfg->min_displace / 100.0f;
        const float frac_hi = (float)cfg->max_displace / 100.0f;
        float frac = frac_lo + sig_t * (frac_hi - frac_lo);
        if (frac < 0.0f) frac = 0.0f; else if (frac > 1.0f) frac = 1.0f;
        const float inner = cfg->inner_radius
                          + ri * (float)(cfg->ring_thickness + cfg->ring_gap);
        const float band  = (float)cfg->ring_thickness;    // fixed width
        const float reach = frac * band;                   // signal-scaled depth ≤ band
#endif
        const float drag  = band;                           // source gather stays within band

        if (inner >= maxr) break;
#if THICK_SCALES_SIGNAL
        ring_inner += band + (float)cfg->ring_gap;         // advance for the next ring
#endif

        uint8_t sym[MAX_SYMBOLS];
        const int n_sym = net_symbols(net, sym, sizeof(sym));
        if (n_sym == 0) continue;

        // ray count = ring-band circumference in pixels (stable ray density)
        const int total_rays = (int)(2.0f * 3.14159265f * (inner + band) + 0.5f);
        const float ang_off  = k_offsets[ri % 12];

        for (int ai = 0; ai < total_rays; ai++) {

            const float ca = lcos(ai, total_rays);
            const float sa = lsin(ai, total_rays);

            // map this spoke to a symbol of the identity string — stretched once
            // around the ring (each symbol = one arc) or tiled — then that symbol
            // to a sort depth. Equal symbols → equal depth within the band.
#if PATTERN_STRETCH
            const int si = (int)((int64_t)ai * n_sym / total_rays);
#else
            const int si = ai % n_sym;
#endif
            const float sort_reach = (float)symbol_depth(sym[si], reach); // px of radius
            if (sort_reach < 1.0f) continue;   // low symbol / weak signal → bare spoke

            // ── collect displaced source reads ──
            // Each destination position on the ray samples from a source
            // position that is offset by (progress × disp) pixels outward.
            // This is what creates the radial drag/smear effect.
            // We read from the FROZEN src — never from dst.
            // 1px steps so sort_reach is a true radial pixel count (0.5px steps
            // were terminating at half the intended radius — n_px counted steps not px).

            int n_px = 0;

            for (float r = inner; r <= inner + sort_reach && n_px < MAX_RAY_LEN; r += 1.0f) {

                // destination pixel coords
                int dpx = (int)(cx + ca * r);
                int dpy = (int)(cy + sa * r);
                if (dpx<0||dpx>=IMG_W||dpy<0||dpy>=IMG_H) continue;

                float rr = (r - inner);

                if (n_px >= MAX_RAY_LEN) break;

                // source pulled progressively further out across the written
                // smear — gather depth (drag) is wider than the band for contrast,
                // but only the band-contained positions (r) are written to
                float progress = rr / sort_reach;     // 0..1 across the written smear
                float d_px     = progress * drag;     // source gather offset in pixels

                float src_x, src_y;
                if (cfg->disp_mode == 0) {
                    // radial only — pure LUT, fastest path
                    src_x = cx + ca * (r + d_px);
                    src_y = cy + sa * (r + d_px);
                } else {
                    // angular or combined — needs atan2 (slower, use disp_mode=0 on S3)
                    float base_angle = atan2f(sa, ca);
                    if (cfg->disp_mode == 1) {
                        float ao = r > 0.5f ? d_px / r : 0.0f;
                        src_x = cx + cosf(base_angle + ao) * r;
                        src_y = cy + sinf(base_angle + ao) * r;
                    } else {
                        float sr = r + d_px * 0.6f;
                        float ao = r > 0.5f ? (d_px * 0.4f) / r : 0.0f;
                        src_x = cx + cosf(base_angle + ao) * sr;
                        src_y = cy + sinf(base_angle + ao) * sr;
                    }
                }

                Pixel sp = sample(src, (int)src_x, (int)src_y);

                ray_buf[n_px].px  = sp;
                ray_buf[n_px].key = sort_key(sp.r, sp.g, sp.b, cfg->sort_dir);
                dest_idx[n_px]    = dpy * IMG_W + dpx;
                n_px++;
            }

            if (n_px < 2) continue;

            // ── insertion sort on displaced source samples ──
            isort(ray_buf, n_px);

            // ── write sorted samples into dst at the ray positions ──
            for (int k = 0; k < n_px; k++) {
                dst[dest_idx[k]] = ray_buf[k].px;
            }
        }
    }
}

// ─────────────────────────────────────────────
//  RGB888 → RGB565  (one scan line)
//  out must be in internal SRAM (DMA-capable)
// ─────────────────────────────────────────────
void rings_to_rgb565_line(const Pixel* row, uint16_t* out, int width) {
    for (int x = 0; x < width; x++) {
        out[x] = ((uint16_t)(row[x].r >> 3) << 11)
               | ((uint16_t)(row[x].g >> 2) << 5)
               |  (uint16_t)(row[x].b >> 3);
    }
}
