/**
 * @file battery.h
 * @brief Battery voltage monitoring via ADC1.
 *
 * The LiPo cell voltage is expected to be scaled through a resistor divider
 * before reaching the ADC pin (see BATT_DIVIDER_RATIO in config.h).
 *
 * NOTE: On ESP32-S3, ADC1 is available on GPIO 1–10 and ADC2 on GPIO 11–20.
 *       GPIO 35 is NOT an ADC-capable pin on this chip family.  Wire the
 *       battery divider output to an ADC1 pin (e.g. GPIO 9) and update
 *       BATT_ADC_CHANNEL in config.h accordingly.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Initialise the ADC1 channel used for battery monitoring.
 * @return ESP_OK on success.
 */
esp_err_t battery_init(void);

/**
 * @brief Read the (corrected) battery voltage in millivolts.
 *
 * Applies the resistor-divider correction factor BATT_DIVIDER_RATIO.
 * Returns 0 on error.
 */
uint32_t battery_read_mv(void);

/**
 * @brief Return true if the battery is below BATT_LOW_MV.
 */
bool battery_is_low(void);

/**
 * @brief Return true if the battery is below BATT_CRITICAL_MV.
 *        At this level the firmware should refuse to write to the SD card.
 */
bool battery_is_critical(void);
