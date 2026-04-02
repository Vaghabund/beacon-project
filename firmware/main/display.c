/**
 * @file display.c
 * @brief GC9D01 SPI LCD driver for the Waveshare ESP32-S3-LCD-1.85.
 *
 * The GC9D01 is a 360×360 round-display controller with a SPI interface.
 * ESP-IDF does not ship a built-in panel driver for it, so the initialisation
 * sequence is implemented here using the low-level esp_lcd_panel_io SPI API.
 *
 * Init command sequence derived from publicly-available GC9D01 application
 * notes and Waveshare example code.  Verify against your exact panel revision.
 */
#include "display.h"
#include "config.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

// GC9D01 GRAM write commands
#define GC9D01_CMD_MEM_WRITE          0x2C
#define GC9D01_CMD_MEM_WRITE_CONT     0x3C  // Memory Write Continue (no re-send of 0x2C)

static const char *TAG = "display";

// ─── Module state ──────────────────────────────────────────────────────────

static esp_lcd_panel_io_handle_t s_io_handle  = NULL;

// ─── GC9D01 command helpers ───────────────────────────────────────────────
{
    esp_lcd_panel_io_tx_param(s_io_handle, cmd, NULL, 0);
}

static void _cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_lcd_panel_io_tx_param(s_io_handle, cmd, data, len);
}

// Convenience wrappers for 1- and 2-byte data payloads
#define CMD1(c, d0)         _cmd_data((c), (const uint8_t[]){(d0)}, 1)
#define CMD2(c, d0, d1)     _cmd_data((c), (const uint8_t[]){(d0),(d1)}, 2)

// ─── GC9D01 initialisation sequence ───────────────────────────────────────
// Based on the GC9D01 datasheet application note and publicly available
// Waveshare C SDK example (github.com/waveshare/…).
// Adjust timing/register values if your panel revision differs.

static void _gc9d01_init(void)
{
    // Software reset, then wait for the panel to settle
    _cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Sleep out
    _cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));

    // ── Manufacturer command access ──────────────────────────────────────
    CMD1(0xFE, 0x01);   // inter-register enable 1
    CMD1(0xFE, 0x02);   // inter-register enable 2

    // ── Power / VCOM ─────────────────────────────────────────────────────
    CMD1(0xC3, 0x47);   // Power control 1
    CMD1(0xC4, 0x07);   // Power control 2
    CMD1(0xC9, 0x08);   // Source driver timing

    CMD1(0xBE, 0x11);   // VCOM1
    CMD1(0xE1, 0x10);   // VCOM2
    CMD1(0xDF, 0x21);   // VCOM3 (memory access mirror)

    // ── Gamma ────────────────────────────────────────────────────────────
    _cmd_data(0xE0, (const uint8_t[]){
        0x45, 0x09, 0x08, 0x08, 0x26, 0x2A
    }, 6);
    _cmd_data(0xE1, (const uint8_t[]){
        0x43, 0x70, 0x72, 0x36, 0x37, 0x6F
    }, 6);
    _cmd_data(0xE2, (const uint8_t[]){
        0x45, 0x09, 0x08, 0x08, 0x26, 0x2A
    }, 6);
    _cmd_data(0xE3, (const uint8_t[]){
        0x43, 0x70, 0x72, 0x36, 0x37, 0x6F
    }, 6);

    // ── Display function control ─────────────────────────────────────────
    CMD1(0x36, 0x00);   // Memory access control (no rotation)
    CMD1(0x3A, 0x05);   // Pixel format: 16-bit RGB565

    // ── Column / row address (full 360×360 canvas) ───────────────────────
    _cmd_data(0x2A, (const uint8_t[]){0x00, 0x00, 0x01, 0x67}, 4);  // col 0–359
    _cmd_data(0x2B, (const uint8_t[]){0x00, 0x00, 0x01, 0x67}, 4);  // row 0–359

    // Display on
    _cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(20));
}

// ─── Public API ────────────────────────────────────────────────────────────

esp_err_t display_init(void)
{
    esp_err_t ret;

    // ── SPI bus ──────────────────────────────────────────────────────────
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = LCD_MOSI_GPIO,
        .miso_io_num     = -1,            // LCD is write-only
        .sclk_io_num     = LCD_SCLK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        // Max DMA transfer: one stripe of LCD_DRAW_LINES lines
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_LINES * 2,
    };
    ret = spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // ── Panel I/O ────────────────────────────────────────────────────────
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num        = LCD_DC_GPIO,
        .cs_gpio_num        = LCD_CS_GPIO,
        .pclk_hz            = LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits       = 8,
        .lcd_param_bits     = 8,
        .spi_mode           = 0,
        .trans_queue_depth  = 10,
        .on_color_trans_done = NULL,
        .user_ctx           = NULL,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                   &io_cfg, &s_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // ── Hardware reset (if wired) ────────────────────────────────────────
    if (LCD_RST_GPIO >= 0) {
        gpio_reset_pin(LCD_RST_GPIO);
        gpio_set_direction(LCD_RST_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(LCD_RST_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(LCD_RST_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    // ── GC9D01 init sequence ─────────────────────────────────────────────
    _gc9d01_init();

    // ── Backlight on ─────────────────────────────────────────────────────
    gpio_reset_pin(LCD_BL_GPIO);
    gpio_set_direction(LCD_BL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL_GPIO, 1);

    ESP_LOGI(TAG, "GC9D01 360×360 LCD ready");
    return ESP_OK;
}

esp_err_t display_draw_frame(const uint16_t *rgb565)
{
    if (!rgb565) {
        return ESP_ERR_INVALID_ARG;
    }

    // Set full-screen write window
    _cmd_data(0x2A, (const uint8_t[]){0x00, 0x00, 0x01, 0x67}, 4);
    _cmd_data(0x2B, (const uint8_t[]){0x00, 0x00, 0x01, 0x67}, 4);

    // Memory write command
    _cmd(GC9D01_CMD_MEM_WRITE);

    // Push pixel data in stripes so we stay within the DMA transfer limit.
    // esp_lcd_panel_io_tx_color() accepts a byte count.
    const int stripe_lines = LCD_DRAW_LINES;
    const uint8_t *ptr = (const uint8_t *)rgb565;

    for (int y = 0; y < LCD_V_RES; y += stripe_lines) {
        int lines = stripe_lines;
        if (y + lines > LCD_V_RES) {
            lines = LCD_V_RES - y;
        }
        // Use GC9D01_CMD_MEM_WRITE_CONT for all stripes after the first to
        // avoid resending the full address window on each DMA transaction.
        uint8_t cmd = (y == 0) ? GC9D01_CMD_MEM_WRITE : GC9D01_CMD_MEM_WRITE_CONT;
        esp_lcd_panel_io_tx_color(s_io_handle, cmd,
                                  ptr, (size_t)(LCD_H_RES * lines * 2));
        ptr += LCD_H_RES * lines * 2;
    }

    return ESP_OK;
}
