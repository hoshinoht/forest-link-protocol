#include "sdcard.hpp"

#include <cstdio>
#include <cstring>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";
static const char *MOUNT_POINT = "/sdcard";
static constexpr size_t SD_MAX_FILE_SIZE = 10 * 1024 * 1024; /* 10 MB */
static bool s_mounted = false;

bool flp::sdcard_init()
{
#if !CONFIG_FLP_SD_ENABLED
    ESP_LOGI(TAG, "SD card disabled in config");
    return false;
#else
    if (s_mounted)
    {
        return true;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;

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
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = static_cast<gpio_num_t>(CONFIG_FLP_SD_CS);
    slot_config.host_id = static_cast<spi_host_device_t>(host.slot);

    sdmmc_card_t *card = nullptr;
    ret = esp_vfs_fat_sdspi_mount(
        MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        spi_bus_free(static_cast<spi_host_device_t>(host.slot));
        return false;
    }

    sdmmc_card_print_info(stdout, card);
    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    return true;
#endif
}

bool flp::sdcard_read_file(
    const char *path, uint8_t **buf_out, size_t *size_out)
{
    if (!s_mounted || path == nullptr ||
        buf_out == nullptr || size_out == nullptr)
    {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (f == nullptr)
    {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0)
    {
        ESP_LOGE(TAG, "File %s is empty or unreadable", path);
        fclose(f);
        return false;
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
        return false;
    }

    uint8_t *buf = static_cast<uint8_t *>(
        heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (buf == nullptr)
    {
        ESP_LOGE(TAG, "PSRAM alloc failed for %u bytes", (unsigned) size);
        fclose(f);
        return false;
    }

    size_t read = fread(buf, 1, size, f);
    fclose(f);

    if (read != size)
    {
        ESP_LOGE(TAG,
                 "Short read: got %u of %u bytes",
                 (unsigned) read,
                 (unsigned) size);
        free(buf);
        return false;
    }

    *buf_out = buf;
    *size_out = size;
    ESP_LOGI(TAG, "Read %s: %u bytes into PSRAM", path, (unsigned) size);
    return true;
}
