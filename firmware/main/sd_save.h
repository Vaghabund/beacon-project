/**
 * @file sd_save.h
 * @brief SD card mount (SPI mode) and PPM image save.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Mount the SD card over SPI and register a FAT filesystem at
 *        SD_MOUNT_POINT ("/sdcard").
 *
 * @return ESP_OK on success.
 */
esp_err_t sd_init(void);

/**
 * @brief Save a 360×360 RGB888 frame as a binary PPM (P6) file.
 *
 * Files are written to SD_MOUNT_POINT with names "beacon_NNNN.ppm" where
 * NNNN is a monotonically incrementing counter (reset on power cycle).
 *
 * @param[in] rgb888     Frame buffer: FRAME_WIDTH × FRAME_HEIGHT × 3 bytes.
 * @param[in] buf_bytes  Size of @p rgb888 in bytes (must equal FRAME_RGB888_BYTES).
 * @return ESP_OK on success.
 */
esp_err_t sd_save_ppm(const uint8_t *rgb888, size_t buf_bytes);
