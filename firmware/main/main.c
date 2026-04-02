/**
 * @file main.c
 * @brief Beacon glitch-art scanner – main application entry point.
 *
 * One button press triggers the full pipeline:
 *   1. Capture frame from OV2640 → 360×360 RGB888 in PSRAM
 *   2. Scan nearby WiFi networks
 *   3. Overlay WiFi metadata rings onto the frame
 *   4. Convert RGB888 → RGB565 and push to LCD
 *   5. Save RGB888 as binary PPM to the SD card
 *
 * Battery voltage is checked before each write; a low-battery warning is
 * printed to the serial console and skips the SD write at critical level.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "config.h"
#include "wifi_scan.h"
#include "camera.h"
#include "image_gen.h"
#include "display.h"
#include "sd_save.h"
#include "battery.h"

static const char *TAG = "main";

// ─── Frame buffers (allocated in PSRAM) ───────────────────────────────────

static uint8_t  *s_rgb888_buf = NULL;  // 360×360×3 bytes
static uint16_t *s_rgb565_buf = NULL;  // 360×360×2 bytes

// ─── Button ISR ────────────────────────────────────────────────────────────

static volatile bool     s_capture_requested = false;
static SemaphoreHandle_t s_btn_sem           = NULL;

static void IRAM_ATTR _btn_isr_handler(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_btn_sem, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

// ─── Pipeline ──────────────────────────────────────────────────────────────

static void _run_pipeline(void)
{
    esp_err_t ret;
    int64_t t0 = esp_timer_get_time();

    // ── Battery check ────────────────────────────────────────────────────
    uint32_t batt_mv = battery_read_mv();
    ESP_LOGI(TAG, "Battery: %lu mV", (unsigned long)batt_mv);

    if (battery_is_low()) {
        ESP_LOGW(TAG, "⚠  Low battery: %lu mV (threshold %d mV)",
                 (unsigned long)batt_mv, BATT_LOW_MV);
    }

    // ── Camera capture ───────────────────────────────────────────────────
    ESP_LOGI(TAG, "Capturing frame…");
    ret = camera_capture_to_rgb888(s_rgb888_buf, FRAME_RGB888_BYTES);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera capture failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGD(TAG, "Capture: %lld µs", esp_timer_get_time() - t0);

    // ── WiFi scan ────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "Scanning WiFi…");
    wifi_ap_record_t aps[WIFI_SCAN_MAX_APS];
    int num_aps = wifi_scan_run(aps, WIFI_SCAN_MAX_APS);
    if (num_aps < 0) {
        ESP_LOGW(TAG, "WiFi scan failed – rendering without rings");
        num_aps = 0;
    }
    ESP_LOGD(TAG, "Scan: %lld µs", esp_timer_get_time() - t0);

    // ── Ring overlay ─────────────────────────────────────────────────────
    ESP_LOGI(TAG, "Rendering %d WiFi rings…", num_aps);
    image_gen_apply_wifi_rings(s_rgb888_buf, aps, num_aps);
    ESP_LOGD(TAG, "Render: %lld µs", esp_timer_get_time() - t0);

    // ── Convert RGB888 → RGB565 ──────────────────────────────────────────
    image_gen_rgb888_to_rgb565(s_rgb888_buf, s_rgb565_buf,
                               FRAME_WIDTH * FRAME_HEIGHT);

    // ── Display ──────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "Pushing frame to LCD…");
    ret = display_draw_frame(s_rgb565_buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "display_draw_frame failed: %s", esp_err_to_name(ret));
    }
    ESP_LOGD(TAG, "Display: %lld µs", esp_timer_get_time() - t0);

    // ── SD save ──────────────────────────────────────────────────────────
    if (battery_is_critical()) {
        ESP_LOGE(TAG, "⛔ Critical battery (%lu mV) – skipping SD write",
                 (unsigned long)batt_mv);
    } else {
        ESP_LOGI(TAG, "Saving PPM to SD card…");
        ret = sd_save_ppm(s_rgb888_buf, FRAME_RGB888_BYTES);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SD save failed: %s", esp_err_to_name(ret));
        }
    }

    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "Pipeline complete in %lld ms", elapsed_ms);
}

// ─── Main task ─────────────────────────────────────────────────────────────

static void _beacon_task(void *arg)
{
    ESP_LOGI(TAG, "Beacon task running – press button (GPIO %d) to capture",
             BTN_GPIO);

    while (true) {
        // Block until the button ISR fires
        if (xSemaphoreTake(s_btn_sem, portMAX_DELAY) == pdTRUE) {
            // Debounce: ignore bounces within BTN_DEBOUNCE_MS
            vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));
            // Drain any extra edges queued during debounce window
            while (xSemaphoreTake(s_btn_sem, 0) == pdTRUE) {}

            _run_pipeline();
        }
    }
}

// ─── app_main ──────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "=== Beacon Glitch Art Scanner ===");

    // ── Allocate PSRAM frame buffers ─────────────────────────────────────
    s_rgb888_buf = heap_caps_malloc(FRAME_RGB888_BYTES, MALLOC_CAP_SPIRAM);
    s_rgb565_buf = heap_caps_malloc(FRAME_RGB565_BYTES, MALLOC_CAP_SPIRAM);

    if (!s_rgb888_buf || !s_rgb565_buf) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM frame buffers "
                      "(rgb888=%p rgb565=%p)",
                 (void *)s_rgb888_buf, (void *)s_rgb565_buf);
        return;
    }
    ESP_LOGI(TAG, "PSRAM frame buffers: RGB888=%zu B  RGB565=%zu B",
             (size_t)FRAME_RGB888_BYTES, (size_t)FRAME_RGB565_BYTES);

    // ── Subsystem init ───────────────────────────────────────────────────
    ESP_ERROR_CHECK(battery_init());
    ESP_ERROR_CHECK(wifi_scan_init());
    ESP_ERROR_CHECK(camera_init());
    ESP_ERROR_CHECK(display_init());

    esp_err_t sd_ret = sd_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available – captures will still display "
                      "but won't be saved");
    }

    // ── Button GPIO ──────────────────────────────────────────────────────
    s_btn_sem = xSemaphoreCreateBinary();
    configASSERT(s_btn_sem);

    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,   // active-low button
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_GPIO, _btn_isr_handler, NULL));

    ESP_LOGI(TAG, "Init complete. Press the button to start a capture.");

    // ── Main task ────────────────────────────────────────────────────────
    xTaskCreatePinnedToCore(_beacon_task,
                            "beacon",
                            8192,
                            NULL,
                            5,
                            NULL,
                            1);  // pin to core 1; core 0 handles WiFi
}
