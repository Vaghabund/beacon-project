/**
 * @file image_gen.h
 * @brief WiFi-ring overlay renderer.
 *
 * Ports the Python WiFiVisualizer algorithm to C:
 *   – SSID hue  = MD5(ssid) mod 360 / 360
 *   – Ring pos  = normalised RSSI (strongest → innermost ring)
 *   – Pixel col = HSV(hue, 0.5+0.5*strength, byte_val/255 * (0.6+0.4*strength))
 *   – No anti-aliasing (intentional glitch aesthetic)
 */
#pragma once

#include "esp_wifi.h"
#include <stdint.h>

/**
 * @brief Overlay WiFi metadata rings onto a 360×360 RGB888 frame buffer.
 *
 * The function operates in-place: camera pixels beneath a ring dot are
 * replaced with the computed ring colour.
 *
 * @param[in,out] rgb888   360×360×3 byte frame buffer (R,G,B interleaved).
 * @param[in]     aps      Sorted array of AP records (strongest first).
 * @param[in]     num_aps  Number of entries in @p aps.
 */
void image_gen_apply_wifi_rings(uint8_t *rgb888,
                                const wifi_ap_record_t *aps,
                                int num_aps);

/**
 * @brief Convert an RGB888 frame buffer to RGB565 in-place (different buffer).
 *
 * @param[in]  rgb888  Source 360×360×3 bytes.
 * @param[out] rgb565  Destination 360×360×2 bytes.
 * @param[in]  pixels  Total pixel count (FRAME_WIDTH * FRAME_HEIGHT).
 */
void image_gen_rgb888_to_rgb565(const uint8_t *rgb888,
                                uint16_t      *rgb565,
                                int            pixels);
