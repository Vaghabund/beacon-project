/**
 * @file wifi_scan.h
 * @brief WiFi station-mode network scan (SSID, BSSID, channel, RSSI).
 */
#pragma once

#include "esp_wifi.h"
#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Initialise the WiFi subsystem in station mode.
 *
 * Must be called once before wifi_scan_run().
 * Also initialises NVS flash, esp_netif and the default event loop if they
 * have not been initialised yet.
 *
 * @return ESP_OK on success.
 */
esp_err_t wifi_scan_init(void);

/**
 * @brief Perform a blocking WiFi scan and populate @p records.
 *
 * Scans all channels (passive scan, default dwell time) and fills up to
 * @p max_records entries sorted by RSSI descending (strongest first).
 *
 * @param[out] records    Caller-allocated array of wifi_ap_record_t.
 * @param[in]  max_records  Maximum number of records to return.
 * @return Number of APs found (≥ 0), or -1 on error.
 */
int wifi_scan_run(wifi_ap_record_t *records, uint16_t max_records);
