#include "flp_client.hpp"

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "flp_config.h"
#include "mesh_manager.hpp"
#include "transfer_engine.hpp"
#include "uart_ingest.hpp"

static const char *TAG = "flp_client";

static constexpr const char *DEMO_FILENAME = "demo.txt";
static constexpr size_t FALLBACK_PAYLOAD_SIZE = 8192;
static constexpr size_t CHUNK_SIZE = 1024;

namespace flp
{

void FlpClient::init(UartIngest *api, MeshManager *mgr,
                     EventGroupHandle_t wifi_events)
{
    api_ = api;
    mgr_ = mgr;
    wifi_events_ = wifi_events;

    strncpy(filename_, DEMO_FILENAME, sizeof(filename_) - 1);
    filename_[sizeof(filename_) - 1] = '\0';

    load_demo_payload();
}

/* ── Demo payload loading (extracted from main.cpp) ──────────────────── */

void FlpClient::load_demo_payload()
{
    use_sd_stream_ = false;

#if CONFIG_FLP_SD_ENABLED
    esp_err_t sd_err = sdcard_init();
    if (sd_err == ESP_OK)
    {
        char sd_path[96] = {};
        std::snprintf(sd_path, sizeof(sd_path), "/sdcard/%s", filename_);
        esp_err_t cache_err = sd_cache_.open(sd_path);
        if (cache_err == ESP_OK)
        {
            payload_size_ = sd_cache_.file_size();

            /* Read entire file into a PSRAM buffer so we can feed it
             * through the UART API's file_begin/data/end sequence. */
            payload_buf_ = static_cast<uint8_t *>(
                heap_caps_malloc(payload_size_, MALLOC_CAP_SPIRAM));
            if (payload_buf_)
            {
                size_t read = sd_cache_.read(payload_buf_, 0, payload_size_);
                if (read != payload_size_)
                {
                    ESP_LOGW(TAG, "SD read short: %zu/%zu", read, payload_size_);
                    payload_size_ = read;
                }
                ESP_LOGI(TAG, "Loaded %s from SD card: %u bytes",
                         filename_, (unsigned)payload_size_);
            }
            else
            {
                /* Keep the cache resident and stream chunks during transfer. */
                use_sd_stream_ = true;
                ESP_LOGW(TAG,
                         "PSRAM alloc failed for full SD mirror, using streamed SD mode");
            }
        }
        else
        {
            ESP_LOGW(TAG,
                     "Failed to open %s from SD cache: %s (0x%x), using fallback",
                     filename_,
                     esp_err_to_name(cache_err),
                     cache_err);
        }
    }
    else
    {
        ESP_LOGW(TAG, "SD card init failed (0x%x), using fallback", sd_err);
    }
#endif

    /* Fallback: 8 KB synthetic pattern */
    if ((!payload_buf_ && !use_sd_stream_) || payload_size_ == 0)
    {
        payload_buf_ = static_cast<uint8_t *>(
            heap_caps_malloc(FALLBACK_PAYLOAD_SIZE, MALLOC_CAP_SPIRAM));
        if (payload_buf_)
        {
            for (size_t i = 0; i < FALLBACK_PAYLOAD_SIZE; i++)
                payload_buf_[i] = static_cast<uint8_t>('A' + (i % 26));
            payload_size_ = FALLBACK_PAYLOAD_SIZE;
            ESP_LOGI(TAG, "Using fallback payload: %u bytes",
                     (unsigned)payload_size_);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to allocate fallback payload in PSRAM");
        }
    }
}

/* ── Transfer via UART API ───────────────────────────────────────────── */

void FlpClient::do_transfer()
{
    if (!api_ || payload_size_ == 0 || (!payload_buf_ && !use_sd_stream_))
    {
        ESP_LOGE(TAG, "Cannot transfer: no payload loaded");
        return;
    }

    /* Wait for any previous transfer to finish */
    if (api_->is_busy())
    {
        ESP_LOGW(TAG, "API busy, waiting...");
        while (api_->is_busy())
            vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "Starting transfer: %s (%u bytes)",
             filename_, (unsigned)payload_size_);

    if (use_sd_stream_)
    {
#if CONFIG_FLP_SD_ENABLED
        if (!mgr_)
        {
            ESP_LOGE(TAG, "Cannot stream transfer: mesh manager unavailable");
            return;
        }

        ESP_LOGI(TAG,
                 "Starting streamed transfer from SD cache: %s (%u bytes)",
                 filename_,
                 (unsigned)payload_size_);

        bool started = mgr_->start_file_transfer(
            filename_,
            payload_size_,
            [this](uint8_t *buf, size_t offset, size_t len) -> size_t
            {
                return sd_cache_.read(buf, offset, len);
            });
        if (!started)
        {
            ESP_LOGE(TAG, "start_file_transfer (streamed) failed");
            return;
        }
#else
        ESP_LOGE(TAG, "SD stream mode unavailable in this build");
        return;
#endif
    }
    else
    {
        /* FILE_BEGIN */
        UartResult res = api_->file_begin(
            static_cast<uint32_t>(payload_size_), filename_);
        if (res != UartResult::OK)
        {
            ESP_LOGE(TAG, "file_begin failed: %u", static_cast<unsigned>(res));
            return;
        }

        /* FILE_DATA in chunks */
        size_t offset = 0;
        while (offset < payload_size_)
        {
            size_t remaining = payload_size_ - offset;
            uint16_t chunk_len = static_cast<uint16_t>(
                remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE);

            res = api_->file_data(payload_buf_ + offset, chunk_len);
            if (res != UartResult::OK)
            {
                ESP_LOGE(TAG, "file_data failed at offset %zu: %u",
                         offset, static_cast<unsigned>(res));
                api_->abort_transfer();
                return;
            }
            offset += chunk_len;
        }

        /* FILE_END (non-blocking — starts mesh transfer) */
        res = api_->file_end();
        if (res != UartResult::OK)
        {
            ESP_LOGE(TAG, "file_end failed: %u", static_cast<unsigned>(res));
            return;
        }
    }

    ESP_LOGI(TAG, "Transfer initiated, waiting for completion...");

    /* Wait for transfer to complete */
    while (mgr_->is_transfer_active())
        vTaskDelay(pdMS_TO_TICKS(500));

    /* Give UartIngest time to detect completion and free the ingest buffer */
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Transfer done");
}

/* ── Wait helpers ────────────────────────────────────────────────────── */

void FlpClient::wait_for_gateway()
{
    ESP_LOGI(TAG, "Waiting for gateway discovery...");
    while (mgr_->get_hops_to_internet() >= 0xFF)
        vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Gateway found at %u hops",
             mgr_->get_hops_to_internet());
}

void FlpClient::wait_for_mqtt()
{
#if !CONFIG_FLP_WIFI_DISABLED
    if (wifi_events_)
    {
        ESP_LOGI(TAG, "Waiting for MQTT connection...");
        /* MQTT_CONNECTED_BIT = BIT1 (from main.cpp) */
        xEventGroupWaitBits(wifi_events_, BIT1,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "MQTT connected");
    }
#endif
}

/* ── Main task loop ──────────────────────────────────────────────────── */

void FlpClient::run()
{
    const TickType_t interval =
#if CONFIG_FLP_DEMO_AUTO
        pdMS_TO_TICKS(CONFIG_FLP_DEMO_AUTO_INTERVAL_S * 1000);
#else
        pdMS_TO_TICKS(60000); /* fallback: 60s */
#endif

#if !CONFIG_FLP_WIFI_DISABLED
    /* Exit node path: wait for MQTT first */
    wait_for_mqtt();
#else
    /* Relay node path: wait for gateway discovery */
    wait_for_gateway();
#endif

    while (true)
    {
#if !CONFIG_FLP_WIFI_DISABLED
        if (!mgr_->is_mqtt_connected())
        {
            ESP_LOGW(TAG, "MQTT disconnected, skipping transfer");
            vTaskDelay(interval);
            continue;
        }

        /* Exit nodes with internet serve as relay forwarders.
         * Skip auto-demo to avoid polluting the shared MQTT pipeline. */
        if (mgr_->has_internet())
        {
            ESP_LOGI(TAG, "Skipping (exit node serves relay transfers)");
            vTaskDelay(interval);
            continue;
        }
#else
        if (mgr_->get_hops_to_internet() >= 0xFF)
        {
            ESP_LOGW(TAG, "No gateway, skipping transfer");
            vTaskDelay(interval);
            continue;
        }
#endif

        do_transfer();

        ESP_LOGI(TAG, "Next transfer in %lu ms",
                 (unsigned long)(interval * portTICK_PERIOD_MS));
        vTaskDelay(interval);
    }
}

} /* namespace flp */
