#include "sdcard.hpp"

#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
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
    bus_cfg.max_transfer_sz = SdReadCache::MAX_CACHE_SIZE;

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

    s_mounted = true;
    ESP_LOGI(TAG,
             "SD card mounted at %s (max=%ukHz real=%dkHz)",
             MOUNT_POINT,
             static_cast<unsigned>(card->max_freq_khz),
             card->real_freq_khz);
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

/* ── SdReadCache ──────────────────────────────────────────────────────── */

flp::SdReadCache::~SdReadCache()
{
    if (file_)
    {
        fclose(file_);
        file_ = nullptr;
    }
    if (cache_buf_)
    {
        heap_caps_free(cache_buf_);
        cache_buf_ = nullptr;
    }
}

esp_err_t flp::SdReadCache::open(const char *path)
{
    if (!s_mounted || path == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *f = fopen(path, "rb");
    if (!f)
    {
        ESP_LOGE(TAG, "SdReadCache: cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0)
    {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    /*
    * Allocate min(file_size, MAX_CACHE_SIZE) in PSRAM (currently 512 KB).
     * For files that fit entirely, this pre-loads the whole file so
     * every fragment read during transfer is a zero-cost PSRAM memcpy.
     */
    size_t alloc_size = static_cast<size_t>(fsize);
    if (alloc_size > MAX_CACHE_SIZE)
    {
        alloc_size = MAX_CACHE_SIZE;
    }

    /*
     * Graceful degradation: if the preferred cache size is unavailable,
     * progressively try smaller windows instead of failing outright.
     */
    constexpr size_t kMinCacheSize = 32 * 1024;
    size_t chosen_size = alloc_size;
    uint8_t *buf = nullptr;
    while (chosen_size >= kMinCacheSize)
    {
        buf = static_cast<uint8_t *>(
            heap_caps_malloc(chosen_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buf)
        {
            break;
        }
        chosen_size /= 2;
    }
    if (!buf)
    {
        ESP_LOGE(TAG,
                 "SdReadCache: PSRAM alloc failed (tried down to %zu bytes)",
                 kMinCacheSize);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    /* Release any previous state */
    if (file_)  { fclose(file_); }
    if (cache_buf_) { heap_caps_free(cache_buf_); }

    file_ = f;
    file_size_ = static_cast<size_t>(fsize);
    cache_buf_ = buf;
    cache_capacity_ = chosen_size;
        if (cache_capacity_ < alloc_size)
        {
            ESP_LOGW(TAG,
                     "SdReadCache: reduced cache from %zu KB to %zu KB due to PSRAM pressure",
                     alloc_size / 1024,
                     cache_capacity_ / 1024);
        }

    cache_start_ = 0;
    cache_len_ = 0;
    hits_ = 0;
    misses_ = 0;

    /* Prime the cache — if file fits, this loads everything */
    refill(0);

    bool full = (cache_capacity_ >= file_size_);
    ESP_LOGI(TAG,
             "SdReadCache: opened %s (%u bytes, %zu KB cache in PSRAM%s)",
             path, (unsigned) file_size_, cache_capacity_ / 1024,
             full ? ", fully resident" : "");
    return ESP_OK;
}

bool flp::SdReadCache::refill(size_t offset)
{
    if (!file_ || cache_capacity_ == 0)
    {
        return false;
    }

    /* Align to cache_capacity_ boundary for predictable sequential access */
    size_t aligned = (offset / cache_capacity_) * cache_capacity_;
    if (aligned >= file_size_)
    {
        return false;
    }

    int64_t t0 = esp_timer_get_time();

    if (fseek(file_, static_cast<long>(aligned), SEEK_SET) != 0)
    {
        ESP_LOGE(TAG, "SdReadCache: fseek to %u failed", (unsigned) aligned);
        return false;
    }

    size_t want = cache_capacity_;
    if (aligned + want > file_size_)
    {
        want = file_size_ - aligned;
    }

    size_t got = fread(cache_buf_, 1, want, file_);
    cache_start_ = aligned;
    cache_len_ = got;

    int64_t elapsed_us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG,
             "SdReadCache: refill @ %u, %u bytes in %lld us",
             (unsigned) aligned, (unsigned) got, elapsed_us);
    return got > 0;
}

size_t flp::SdReadCache::read(uint8_t *buf, size_t offset, size_t len)
{
    if (!cache_buf_ || !file_ || !buf || offset >= file_size_)
    {
        return 0;
    }

    /* Clamp to file boundary */
    if (offset + len > file_size_)
    {
        len = file_size_ - offset;
    }

    /* Check if the request fits entirely within the cached window */
    if (offset >= cache_start_ &&
        (offset + len) <= (cache_start_ + cache_len_))
    {
        /* Cache hit — fast PSRAM memcpy */
        memcpy(buf, cache_buf_ + (offset - cache_start_), len);
        hits_++;
        return len;
    }

    /* Cache miss — refill and retry */
    misses_++;
    if (!refill(offset))
    {
        return 0;
    }

    /* After refill the request should be within the new window */
    if (offset >= cache_start_ &&
        (offset + len) <= (cache_start_ + cache_len_))
    {
        memcpy(buf, cache_buf_ + (offset - cache_start_), len);
        return len;
    }

    /* Edge case: request spans two cache windows (shouldn't happen with
     * 1470-byte MAX_MTU-sized fragments and a 512 KB cache, but handle it
     * defensively). */
    size_t first = cache_start_ + cache_len_ - offset;
    if (first > len) { first = len; }
    memcpy(buf, cache_buf_ + (offset - cache_start_), first);

    /* Refill for the remainder */
    if (!refill(offset + first))
    {
        return first;
    }
    size_t second_off = offset + first;
    size_t second_len = len - first;
    if (second_off >= cache_start_ &&
        (second_off + second_len) <= (cache_start_ + cache_len_))
    {
        memcpy(buf + first,
               cache_buf_ + (second_off - cache_start_),
               second_len);
        return first + second_len;
    }

    return first;
}
