/**
 * @file display.h
 * @brief SPI LCD driver for the GC9D01 360×360 round display found on the
 *        Waveshare ESP32-S3-LCD-1.85.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Initialise the SPI bus and GC9D01 panel.
 *
 * Configures SPI2_HOST, applies the GC9D01 init sequence, enables backlight,
 * and readies the LCD for pixel data.  Call once at start-up.
 *
 * @return ESP_OK on success.
 */
esp_err_t display_init(void);

/**
 * @brief Push a full 360×360 RGB565 frame to the LCD.
 *
 * The function blocks until the entire frame has been transferred via DMA.
 *
 * @param[in] rgb565  Frame buffer: FRAME_WIDTH × FRAME_HEIGHT × 2 bytes.
 *                    Must remain valid until the function returns.
 * @return ESP_OK on success.
 */
esp_err_t display_draw_frame(const uint16_t *rgb565);
