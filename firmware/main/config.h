/**
 * @file config.h
 * @brief Central pin definitions and compile-time constants for the
 *        Waveshare ESP32-S3-LCD-1.85 beacon glitch-art scanner.
 *
 * Verify every GPIO against your physical schematic before flashing.
 * All pin numbers refer to the ESP32-S3 GPIO pad number.
 */
#pragma once

#include <stdint.h>

// ─── Display (GC9D01, 360×360 round, SPI2) ────────────────────────────────
#define LCD_HOST          SPI2_HOST
#define LCD_MOSI_GPIO     13
#define LCD_SCLK_GPIO     12
#define LCD_CS_GPIO       11
#define LCD_DC_GPIO       10
#define LCD_RST_GPIO      (-1)   // tied to EN; -1 = software reset not used
#define LCD_BL_GPIO       46     // active-high backlight
#define LCD_H_RES         360
#define LCD_V_RES         360
// GC9D01 SPI max clock: 80 MHz; use 40 MHz for reliable signal integrity
#define LCD_PIXEL_CLK_HZ  (40 * 1000 * 1000)
// Double-buffer: send one line-batch while filling the next
#define LCD_DRAW_LINES    20

// ─── OV2640 camera ────────────────────────────────────────────────────────
#define CAM_PIN_PWDN      (-1)   // power-down not wired
#define CAM_PIN_RESET     (-1)   // reset not wired
#define CAM_PIN_XCLK      15
#define CAM_PIN_SIOD       4    // SDA (I2C config bus)
#define CAM_PIN_SIOC       5    // SCL
#define CAM_PIN_D7        16
#define CAM_PIN_D6        17
#define CAM_PIN_D5        18
#define CAM_PIN_D4        19
#define CAM_PIN_D3        20
#define CAM_PIN_D2         6
#define CAM_PIN_D1         7
#define CAM_PIN_D0         8
#define CAM_PIN_VSYNC     21
#define CAM_PIN_HREF       3
#define CAM_PIN_PCLK       2
// Capture at QVGA (320×240) → scale to 360×360 in image_gen
#define CAM_XCLK_FREQ_HZ  (20 * 1000 * 1000)

// ─── SD card (SPI3 / VSPI) ────────────────────────────────────────────────
#define SD_HOST           SPI3_HOST
#define SD_MOSI_GPIO      38
#define SD_MISO_GPIO      39
#define SD_SCLK_GPIO      40
#define SD_CS_GPIO        41
#define SD_MOUNT_POINT    "/sdcard"
#define SD_MAX_FILES       5

// ─── Button ──────────────────────────────────────────────────────────────
// GPIO 0 doubles as the BOOT button on most ESP32-S3 boards
#define BTN_GPIO           0
// Debounce window after the first edge (milliseconds)
#define BTN_DEBOUNCE_MS    300

// ─── Battery ADC ──────────────────────────────────────────────────────────
// NOTE: ESP32-S3 ADC1 is available on GPIO 1–10, ADC2 on GPIO 11–20.
//       GPIO 35 is NOT an ADC-capable pin on ESP32-S3.
//       Connect the battery voltage-divider output to an ADC1 pin (e.g. GPIO 9)
//       and update BATT_ADC_CHANNEL accordingly.
#define BATT_ADC_UNIT      ADC_UNIT_1
// ADC1_CHANNEL_8 corresponds to GPIO 9 on ESP32-S3
// (ESP32-S3 ADC1 channels 0–9 map to GPIO 1–10 respectively)
#define BATT_ADC_CHANNEL   ADC_CHANNEL_8   // ADC1 channel 8 → GPIO 9
#define BATT_ADC_ATTEN     ADC_ATTEN_DB_12 // 0–3.3 V range
// Voltage-divider ratio: if using 100k/100k then ratio = 2.0
#define BATT_DIVIDER_RATIO 2.0f
// LiPo thresholds (mV after divider correction)
#define BATT_LOW_MV        3500
#define BATT_CRITICAL_MV   3300

// ─── Image dimensions ─────────────────────────────────────────────────────
#define FRAME_WIDTH        LCD_H_RES   // 360
#define FRAME_HEIGHT       LCD_V_RES   // 360
// RGB888 frame buffer size (used for ring rendering and PPM save)
#define FRAME_RGB888_BYTES (FRAME_WIDTH * FRAME_HEIGHT * 3u)
// RGB565 frame buffer size (used for LCD transfer)
#define FRAME_RGB565_BYTES (FRAME_WIDTH * FRAME_HEIGHT * 2u)

// ─── WiFi scan ────────────────────────────────────────────────────────────
#define WIFI_SCAN_MAX_APS  20
