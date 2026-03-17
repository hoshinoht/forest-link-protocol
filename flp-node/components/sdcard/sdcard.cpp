#include "sdcard.hpp"

#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";
static const char *MOUNT_POINT = "/sdcard";
static constexpr size_t SD_MAX_FILE_SIZE = 10 * 1024 * 1024; /* 10 MB */
static bool s_mounted = false;

esp_err_t flp::sdcard_init()
{
#if !CONFIG_FLP_SD_ENABLED
    ESP_LOGI(TAG, "SD card disabled in config");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_mounted)
    {
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;

    /*
     * LilyGo T3-S3: SD card on SPI2 (HSPI), separate from LoRa on SPI3.
     * Pins: CS=13, MOSI=11, MISO=2, SCK=14 (from LilyGo utilities.h)
     */
    gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_FLP_SD_MISO), GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_FLP_SD_MOSI), GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_FLP_SD_SCK),  GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_FLP_SD_CS),   GPIO_PULLUP_ONLY);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = CONFIG_FLP_SD_MOSI;
    bus_cfg.miso_io_num = CONFIG_FLP_SD_MISO;
    bus_cfg.sclk_io_num = CONFIG_FLP_SD_SCK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4096;

    esp_err_t ret = spi_bus_initialize(
        static_cast<spi_host_device_t>(host.slot), &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = static_cast<gpio_num_t>(CONFIG_FLP_SD_CS);
    slot_config.host_id = static_cast<spi_host_device_t>(host.slot);

    ESP_LOGI(TAG,
             "SPI SD init: CS=%d MOSI=%d MISO=%d SCK=%d",
             CONFIG_FLP_SD_CS,
             CONFIG_FLP_SD_MOSI,
             CONFIG_FLP_SD_MISO,
             CONFIG_FLP_SD_SCK);

    sdmmc_card_t *card = nullptr;
    ret = esp_vfs_fat_sdspi_mount(
        MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SD card mount failed: %s (0x%x)", esp_err_to_name(ret), ret);
        spi_bus_free(static_cast<spi_host_device_t>(host.slot));
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    return ESP_OK;
#endif
}

FILE *flp::sdcard_open(const char *path, size_t *size_out)
{
    if (!s_mounted || path == nullptr)
    {
        return nullptr;
    }

    FILE *f = fopen(path, "rb");
    if (f == nullptr)
    {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return nullptr;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0)
    {
        ESP_LOGE(TAG, "File %s is empty or unreadable", path);
        fclose(f);
        return nullptr;
    }

    size_t size = static_cast<size_t>(fsize);
    if (size > SD_MAX_FILE_SIZE)
    {
        ESP_LOGE(TAG,
                 "File %s too large: %u bytes (max %u)",
                 path,
                 (unsigned) size,
                 (unsigned) SD_MAX_FILE_SIZE);
        fclose(f);
        return nullptr;
    }

    if (size_out)
    {
        *size_out = size;
    }

    ESP_LOGI(TAG, "Opened %s: %u bytes", path, (unsigned) size);
    return f;
}

size_t flp::sdcard_read_chunk(const char *path,
                              uint8_t *buf,
                              size_t offset,
                              size_t len)
{
    if (!s_mounted || path == nullptr || buf == nullptr)
    {
        return 0;
    }

    FILE *f = fopen(path, "rb");
    if (f == nullptr)
    {
        return 0;
    }

    if (fseek(f, static_cast<long>(offset), SEEK_SET) != 0)
    {
        fclose(f);
        return 0;
    }

    size_t got = fread(buf, 1, len, f);
    fclose(f);
    return got;
}
