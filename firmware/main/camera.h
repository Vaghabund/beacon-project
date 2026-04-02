/**
 * @file camera.h
 * @brief OV2640 camera initialisation, frame capture, and RGB565→RGB888
 *        nearest-neighbour upscale to the 360×360 display canvas.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Initialise the OV2640 camera via the esp32-camera component.
 *
 * Configures the sensor for QVGA (320×240) RGB565 capture; uses PSRAM for
 * the frame buffer.  Call once at start-up.
 *
 * @return ESP_OK on success.
 */
esp_err_t camera_init(void);

/**
 * @brief Capture one frame and write a 360×360 RGB888 image into @p out_buf.
 *
 * The raw QVGA RGB565 frame is:
 *   1. Decoded to RGB888
 *   2. Nearest-neighbour scaled to FRAME_WIDTH × FRAME_HEIGHT (360×360)
 *
 * The caller must supply a buffer of at least FRAME_RGB888_BYTES bytes
 * allocated in PSRAM (heap_caps_malloc with MALLOC_CAP_SPIRAM).
 *
 * @param[out] out_buf   Destination RGB888 frame buffer (360×360×3 bytes).
 * @param[in]  buf_size  Size of @p out_buf in bytes.
 * @return ESP_OK on success.
 */
esp_err_t camera_capture_to_rgb888(uint8_t *out_buf, size_t buf_size);
