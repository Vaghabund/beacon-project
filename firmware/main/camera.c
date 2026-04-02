/**
 * @file camera.c
 * @brief OV2640 camera driver wrapper.
 *
 * Captures QVGA (320×240) RGB565 frames and scales them to the 360×360
 * canvas used by the display and image-generation pipeline.
 */
#include "camera.h"
#include "config.h"

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <string.h>

static const char *TAG = "camera";

// ─── Init ──────────────────────────────────────────────────────────────────

esp_err_t camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn       = CAM_PIN_PWDN,
        .pin_reset      = CAM_PIN_RESET,
        .pin_xclk       = CAM_PIN_XCLK,
        .pin_sscb_sda   = CAM_PIN_SIOD,
        .pin_sscb_scl   = CAM_PIN_SIOC,

        .pin_d7         = CAM_PIN_D7,
        .pin_d6         = CAM_PIN_D6,
        .pin_d5         = CAM_PIN_D5,
        .pin_d4         = CAM_PIN_D4,
        .pin_d3         = CAM_PIN_D3,
        .pin_d2         = CAM_PIN_D2,
        .pin_d1         = CAM_PIN_D1,
        .pin_d0         = CAM_PIN_D0,

        .pin_vsync      = CAM_PIN_VSYNC,
        .pin_href       = CAM_PIN_HREF,
        .pin_pclk       = CAM_PIN_PCLK,

        .xclk_freq_hz   = CAM_XCLK_FREQ_HZ,
        .ledc_timer     = LEDC_TIMER_0,
        .ledc_channel   = LEDC_CHANNEL_0,

        // Capture RGB565 directly; no JPEG decode needed
        .pixel_format   = PIXFORMAT_RGB565,
        // QVGA 320×240 – small enough to live in PSRAM, large enough for good
        // glitch detail when scaled to 360×360
        .frame_size     = FRAMESIZE_QVGA,
        .jpeg_quality   = 0,     // irrelevant for RGB565 mode
        .fb_count       = 2,     // ping-pong: one being read, one being filled
        .fb_location    = CAMERA_FB_IN_PSRAM,
        .grab_mode      = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t ret = esp_camera_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s (0x%x)", esp_err_to_name(ret), ret);
        return ret;
    }

    // Tune sensor for warmer, slightly over-exposed look (glitch aesthetic)
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        sensor->set_brightness(sensor,  1);
        sensor->set_contrast(sensor,    0);
        sensor->set_saturation(sensor,  1);
        sensor->set_whitebal(sensor,    1);
        sensor->set_awb_gain(sensor,    1);
        sensor->set_exposure_ctrl(sensor, 1);
    }

    ESP_LOGI(TAG, "OV2640 ready (QVGA RGB565, PSRAM frame buffers)");
    return ESP_OK;
}

// ─── RGB565 → RGB888 conversion helpers ───────────────────────────────────

// Inline expand a single RGB565 pixel to R8, G8, B8.
// ESP32-S3 stores RGB565 in little-endian byte order from the camera DMA.
static inline void rgb565_to_rgb888(uint16_t px,
                                    uint8_t *r, uint8_t *g, uint8_t *b)
{
    // Bit layout: RRRRRGGGGGGBBBBB (big-endian pixel value)
    uint8_t r5 = (px >> 11) & 0x1Fu;
    uint8_t g6 = (px >>  5) & 0x3Fu;
    uint8_t b5 = (px >>  0) & 0x1Fu;

    // Expand to 8-bit by replicating the MSBs into the vacated LSBs
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

// ─── Capture ───────────────────────────────────────────────────────────────

esp_err_t camera_capture_to_rgb888(uint8_t *out_buf, size_t buf_size)
{
    if (!out_buf) {
        return ESP_ERR_INVALID_ARG;
    }
    if (buf_size < FRAME_RGB888_BYTES) {
        ESP_LOGE(TAG, "Output buffer too small: got %zu, need %zu",
                 buf_size, (size_t)FRAME_RGB888_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "esp_camera_fb_get() returned NULL");
        return ESP_FAIL;
    }

    // fb->width × fb->height pixels in RGB565 (2 bytes per pixel)
    const int src_w = (int)fb->width;
    const int src_h = (int)fb->height;
    const uint16_t *src = (const uint16_t *)fb->buf;

    // Nearest-neighbour scale to FRAME_WIDTH × FRAME_HEIGHT
    for (int dy = 0; dy < FRAME_HEIGHT; dy++) {
        int sy = dy * src_h / FRAME_HEIGHT;
        const uint16_t *src_row = src + sy * src_w;

        for (int dx = 0; dx < FRAME_WIDTH; dx++) {
            int sx = dx * src_w / FRAME_WIDTH;
            uint16_t px = src_row[sx];

            // RGB565 from camera DMA is byte-swapped on ESP32-S3
            px = (px >> 8) | (px << 8);

            uint8_t r, g, b;
            rgb565_to_rgb888(px, &r, &g, &b);

            int off = (dy * FRAME_WIDTH + dx) * 3;
            out_buf[off + 0] = r;
            out_buf[off + 1] = g;
            out_buf[off + 2] = b;
        }
    }

    esp_camera_fb_return(fb);
    ESP_LOGD(TAG, "Frame captured: %dx%d → %dx%d RGB888",
             src_w, src_h, FRAME_WIDTH, FRAME_HEIGHT);
    return ESP_OK;
}
