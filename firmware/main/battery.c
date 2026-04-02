/**
 * @file battery.c
 * @brief Battery voltage monitoring using the ESP-IDF 5.x ADC oneshot API.
 */
#include "battery.h"
#include "config.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

#include <stdint.h>

// ADC reference values for the fallback linear conversion
#define ADC_REF_VOLTAGE_MV   3300   // full-scale voltage at 12 dB attenuation
#define ADC_MAX_RAW_12BIT    4095   // 12-bit ADC max raw count

static const char *TAG = "battery";

static adc_oneshot_unit_handle_t s_adc_handle  = NULL;
static adc_cali_handle_t         s_cali_handle = NULL;
static bool                      s_cali_ok     = false;

// ─── Calibration helper ────────────────────────────────────────────────────

static bool _init_calibration(adc_unit_t unit, adc_channel_t channel,
                               adc_atten_t atten,
                               adc_cali_handle_t *out_handle)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id  = unit,
        .chan     = channel,
        .atten    = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cfg, out_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration: curve-fitting scheme");
        return true;
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cfg = {
        .unit_id   = unit,
        .atten     = atten,
        .bitwidth  = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cfg, out_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration: line-fitting scheme");
        return true;
    }
#endif
    ESP_LOGW(TAG, "No ADC calibration scheme available – raw voltages only");
    return false;
}

// ─── Public API ────────────────────────────────────────────────────────────

esp_err_t battery_init(void)
{
    // Create ADC unit handle
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = BATT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure channel
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = BATT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_oneshot_config_channel(s_adc_handle, BATT_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Attempt calibration
    s_cali_ok = _init_calibration(BATT_ADC_UNIT, BATT_ADC_CHANNEL,
                                  BATT_ADC_ATTEN, &s_cali_handle);

    ESP_LOGI(TAG, "Battery ADC initialised (unit %d, channel %d)",
             (int)BATT_ADC_UNIT, (int)BATT_ADC_CHANNEL);
    return ESP_OK;
}

uint32_t battery_read_mv(void)
{
    if (!s_adc_handle) {
        return 0;
    }

    // Average 8 readings to reduce noise
    int32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, BATT_ADC_CHANNEL, &raw) != ESP_OK) {
            return 0;
        }
        sum += raw;
    }
    int32_t raw_avg = sum / 8;

    // Convert raw → voltage (mV)
    int voltage_mv = 0;
    if (s_cali_ok && s_cali_handle) {
        adc_cali_raw_to_voltage(s_cali_handle, raw_avg, &voltage_mv);
    } else {
        // Fallback: linear approximation for 12 dB attenuation
        voltage_mv = (int32_t)(raw_avg * ADC_REF_VOLTAGE_MV / ADC_MAX_RAW_12BIT);
    }

    // Apply resistor-divider correction
    uint32_t batt_mv = (uint32_t)((float)voltage_mv * BATT_DIVIDER_RATIO);

    ESP_LOGD(TAG, "raw_avg=%d adc_mv=%d batt_mv=%lu",
             raw_avg, voltage_mv, (unsigned long)batt_mv);
    return batt_mv;
}

bool battery_is_low(void)
{
    uint32_t mv = battery_read_mv();
    return (mv > 0) && (mv < BATT_LOW_MV);
}

bool battery_is_critical(void)
{
    uint32_t mv = battery_read_mv();
    return (mv > 0) && (mv < BATT_CRITICAL_MV);
}
