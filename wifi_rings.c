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
//  dBm → normalised signal strength [0..1]
// ─────────────────────────────────────────────
static inline float dbm_t(int8_t dbm) {
    float t = ((float)dbm + 100.0f) / 70.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
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
//  SSID → digit sequence
//  One digit (0-9) per raw byte: value % 10.
//  Works for any encoding — ASCII, UTF-8, binary.
//  'F'=0x46 → 6,  0xC3 0xA9 ('é' in UTF-8) → 3, 9
// ─────────────────────────────────────────────
static int ssid_digits(const char* ssid, uint8_t* out, int max) {
    int n=0;
    for (int i=0; ssid[i] && n<max; i++) {
        out[n++] = (uint8_t)ssid[i] % 10;
    }
    return n;
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
    const float maxr = (IMG_W < IMG_H ? IMG_W : IMG_H) * 0.72f;

    // stack-allocated working buffers — reused every ray, zero heap
    SE      ray_buf[MAX_RAY_LEN];
    int     dest_idx[MAX_RAY_LEN];
    uint8_t digits[MAX_SSID_LEN * 2];

    for (int ri = 0; ri < n_nets && ri < MAX_NETWORKS; ri++) {

        const WifiNetwork* net = &nets[ri];
        const float sig_t = dbm_t(net->dbm);

        // displacement range — signal strength controls how far pixels travel
        const float disp = cfg->min_displace
                         + sig_t * (float)(cfg->max_displace - cfg->min_displace);

        const float inner = cfg->inner_radius
                          + ri * (float)(cfg->ring_thickness + cfg->ring_gap);
        const float outer = inner + cfg->ring_thickness;
        const float rlen  = outer - inner;

        if (inner >= maxr) break;

        const int n_dig = ssid_digits(net->ssid, digits, sizeof(digits));
        if (n_dig == 0) continue;

        // ray count = outer circumference in pixels
        const int total_rays = (int)(2.0f * 3.14159265f * outer + 0.5f);
        const float ang_off  = k_offsets[ri % 12];

        for (int ai = 0; ai < total_rays; ai++) {

            const uint8_t d = digits[ai % n_dig];
            if (d == 0) continue;   // no sort, no effect on this ray

            const float ca = lcos(ai, total_rays);
            const float sa = lsin(ai, total_rays);

            // how deep along the ring to sort (digit/9 × ring thickness)
            const int sort_len = (int)(((float)d / 9.0f) * rlen + 0.5f);
            if (sort_len < 2) continue;

            // ── collect displaced source reads ──
            // Each destination position on the ray samples from a source
            // position that is offset by (progress × disp) pixels outward.
            // This is what creates the radial drag/smear effect.
            // We read from the FROZEN src — never from dst.

            int n_px = 0;
            float prev_r = -999.0f;

            for (float r = inner; r <= outer && n_px < sort_len; r += 0.5f) {

                // destination pixel coords
                int dpx = (int)(cx + ca * r);
                int dpy = (int)(cy + sa * r);
                if (dpx<0||dpx>=IMG_W||dpy<0||dpy>=IMG_H) continue;

                // deduplicate — 0.5px steps can land on same pixel twice
                float rr = (r - inner);
                if (rr - prev_r < 0.49f) continue;
                prev_r = rr;

                if (n_px >= MAX_RAY_LEN) break;

                // displaced source position — grows toward outer edge
                float progress = rr / rlen;           // 0..1
                float d_px     = progress * disp;     // displacement in pixels

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
