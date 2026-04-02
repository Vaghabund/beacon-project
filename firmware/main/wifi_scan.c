/**
 * @file wifi_scan.c
 * @brief WiFi station-mode network scan.
 */
#include "wifi_scan.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "wifi_scan";

// Track whether subsystems have already been initialised so that
// wifi_scan_init() is safe to call more than once.
static bool s_nvs_inited      = false;
static bool s_netif_inited    = false;
static bool s_evloop_inited   = false;
static bool s_wifi_inited     = false;

esp_err_t wifi_scan_init(void)
{
    esp_err_t ret;

    // NVS flash (required by WiFi driver)
    if (!s_nvs_inited) {
        ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "NVS partition truncated, erasing…");
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);
        s_nvs_inited = true;
    }

    // TCP/IP stack
    if (!s_netif_inited) {
        ESP_ERROR_CHECK(esp_netif_init());
        s_netif_inited = true;
    }

    // Default event loop
    if (!s_evloop_inited) {
        ret = esp_event_loop_create_default();
        // Ignore ESP_ERR_INVALID_STATE: it means the loop was already created
        // by another subsystem; treat all other errors as fatal.
        if (ret != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(ret);
        }
        s_evloop_inited = true;
    }

    if (!s_wifi_inited) {
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_inited = true;
    }

    ESP_LOGI(TAG, "WiFi scan subsystem ready");
    return ESP_OK;
}

// Comparator for qsort: descending RSSI (strongest first)
static int _ap_rssi_desc(const void *a, const void *b)
{
    const wifi_ap_record_t *ap_a = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *ap_b = (const wifi_ap_record_t *)b;
    return (int)ap_b->rssi - (int)ap_a->rssi;
}

int wifi_scan_run(wifi_ap_record_t *records, uint16_t max_records)
{
    if (!records || max_records == 0) {
        return -1;
    }

    // Blocking scan (NULL config = scan all channels with default dwell)
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t found = max_records;
    err = esp_wifi_scan_get_ap_records(&found, records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(err));
        esp_wifi_scan_stop();
        return -1;
    }

    // Sort strongest → weakest
    qsort(records, found, sizeof(wifi_ap_record_t), _ap_rssi_desc);

    ESP_LOGI(TAG, "Scan complete: %d APs found", (int)found);
    for (int i = 0; i < (int)found; i++) {
        ESP_LOGI(TAG, "  [%2d] RSSI %4d  CH %2d  SSID \"%s\"",
                 i, records[i].rssi, records[i].primary,
                 (char *)records[i].ssid);
    }

    return (int)found;
}
