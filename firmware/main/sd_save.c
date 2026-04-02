/**
 * @file sd_save.c
 * @brief SD card (SPI) mount and PPM save implementation.
 */
#include "sd_save.h"
#include "config.h"

#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "sd_save";

static sdmmc_card_t *s_card = NULL;
static int s_file_counter   = 0;

// ─── Init ──────────────────────────────────────────────────────────────────

esp_err_t sd_init(void)
{
    if (s_card) {
        return ESP_OK;  // already mounted
    }

    // SPI bus for SD card (SPI3_HOST / VSPI)
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = SD_MOSI_GPIO,
        .miso_io_num     = SD_MISO_GPIO,
        .sclk_io_num     = SD_SCLK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t ret = spi_bus_initialize(SD_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Mount FAT filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = SD_MAX_FILES,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host       = SDSPI_HOST_DEFAULT();
    host.slot               = SD_HOST;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs        = SD_CS_GPIO;
    slot_cfg.host_id        = SD_HOST;

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg,
                                  &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount SD filesystem – check card format (FAT32)");
        } else {
            ESP_LOGE(TAG, "SD mount failed: %s (0x%x)", esp_err_to_name(ret), ret);
        }
        spi_bus_free(SD_HOST);
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    return ESP_OK;
}

// ─── PPM save ─────────────────────────────────────────────────────────────

esp_err_t sd_save_ppm(const uint8_t *rgb888, size_t buf_bytes)
{
    if (!rgb888) {
        return ESP_ERR_INVALID_ARG;
    }
    if (buf_bytes < FRAME_RGB888_BYTES) {
        ESP_LOGE(TAG, "Buffer too small: %zu < %zu", buf_bytes,
                 (size_t)FRAME_RGB888_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_card) {
        ESP_LOGE(TAG, "SD card not mounted – call sd_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    char path[64];
    snprintf(path, sizeof(path),
             "%s/beacon_%04d.ppm", SD_MOUNT_POINT, s_file_counter);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot open %s for writing", path);
        return ESP_FAIL;
    }

    // PPM P6 header: "P6\n<width> <height>\n255\n"
    fprintf(fp, "P6\n%d %d\n255\n", FRAME_WIDTH, FRAME_HEIGHT);

    // Binary RGB pixel data (row-major, R G B per pixel)
    size_t written = fwrite(rgb888, 1, FRAME_RGB888_BYTES, fp);
    fclose(fp);

    if (written != FRAME_RGB888_BYTES) {
        ESP_LOGE(TAG, "Short write: %zu / %zu bytes", written,
                 (size_t)FRAME_RGB888_BYTES);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved %s (%zu bytes)", path, written);
    s_file_counter++;
    return ESP_OK;
}
