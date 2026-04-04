#include "uart_ingest.hpp"

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mesh_manager.hpp"
#if CONFIG_FLP_SD_ENABLED
#include "sdcard.hpp"
#endif

static const char *TAG = "uart_ingest";

namespace
{
#ifdef FLP_INGEST_MAX_SIZE
constexpr size_t FLP_INGEST_MAX_SIZE_BYTES =
    static_cast<size_t>(FLP_INGEST_MAX_SIZE);
#else
constexpr size_t FLP_INGEST_MAX_SIZE_BYTES = 3U * 1024U * 1024U;
#endif

constexpr uint16_t EVENT_QUEUE_DEPTH = 16;
constexpr uint32_t MESH_POLL_INTERVAL_MS = 10000;
constexpr uint8_t  PROGRESS_REPORT_STEP = 5; /* emit event every 5% */

static uint32_t now_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}
} /* namespace */

namespace flp
{

/* ══════════════════════════════════════════════════════════════════════════
 * Init
 * ══════════════════════════════════════════════════════════════════════════ */

void UartIngest::init(uart_port_t port,
                      int tx_pin,
                      int rx_pin,
                      MeshManager *mgr)
{
    port_ = port;
    mgr_ = mgr;

    uart_config_t uart_cfg = {};
#ifdef CONFIG_FLP_UART_BAUD_RATE
    uart_cfg.baud_rate = CONFIG_FLP_UART_BAUD_RATE;
#else
    uart_cfg.baud_rate = 115200;
#endif
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_param_config(port_, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(
        port_, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(port_, 2048, 1024, 0, nullptr, 0));

    event_queue_ = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(UartEvent));

    ESP_LOGI(TAG, "UART%d initialized (TX=%d, RX=%d)", port_, tx_pin, rx_pin);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Run loop — polls UART bytes, transfer progress, mesh state, event queue
 * ══════════════════════════════════════════════════════════════════════════ */

void UartIngest::run()
{
    uint8_t byte;
    while (true)
    {
        int n = uart_read_bytes(port_, &byte, 1, pdMS_TO_TICKS(10));
        if (n > 0)
        {
            switch (state_)
            {
                case ParseState::SYNC1:
                    if (byte == UART_SYNC1)
                        state_ = ParseState::SYNC2;
                    break;

                case ParseState::SYNC2:
                    state_ = (byte == UART_SYNC2)
                                 ? ParseState::CMD
                                 : ParseState::SYNC1;
                    break;

                case ParseState::CMD:
                    frame_cmd_ = byte;
                    state_ = ParseState::LEN_LO;
                    break;

                case ParseState::LEN_LO:
                    frame_len_ = byte;
                    state_ = ParseState::LEN_HI;
                    break;

                case ParseState::LEN_HI:
                    frame_len_ |= (uint16_t)byte << 8;
                    if (frame_len_ == 0)
                    {
                        handle_frame();
                        reset_parser();
                    }
                    else if (frame_len_ > sizeof(payload_buf_))
                    {
                        ESP_LOGW(TAG, "Frame too large: %u", frame_len_);
                        reset_parser();
                    }
                    else
                    {
                        payload_idx_ = 0;
                        state_ = ParseState::PAYLOAD;
                    }
                    break;

                case ParseState::PAYLOAD:
                    payload_buf_[payload_idx_++] = byte;
                    if (payload_idx_ >= frame_len_)
                    {
                        handle_frame();
                        reset_parser();
                    }
                    break;
            }
        }

        /* Poll transfer progress (non-blocking FILE_END tracking) */
        poll_transfer_progress();

        /* Poll mesh state changes */
        poll_mesh_state();

        /* Drain async event queue */
        drain_event_queue();
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Frame parser helpers
 * ══════════════════════════════════════════════════════════════════════════ */

void UartIngest::reset_parser()
{
    state_ = ParseState::SYNC1;
    frame_cmd_ = 0;
    frame_len_ = 0;
    payload_idx_ = 0;
}

void UartIngest::handle_frame()
{
    UartResult res;
    switch (frame_cmd_)
    {
        case UART_CMD_FILE_BEGIN:
        {
            if (frame_len_ < 5)
            {
                send_nack(UART_CMD_FILE_BEGIN, UART_ERR_ALLOC_FAIL);
                return;
            }
            uint32_t fsize = payload_buf_[0] |
                             ((uint32_t)payload_buf_[1] << 8) |
                             ((uint32_t)payload_buf_[2] << 16) |
                             ((uint32_t)payload_buf_[3] << 24);
            char fname[64];
            size_t name_len =
                strnlen((const char *)&payload_buf_[4], frame_len_ - 4);
            if (name_len >= sizeof(fname))
                name_len = sizeof(fname) - 1;
            memcpy(fname, &payload_buf_[4], name_len);
            fname[name_len] = '\0';

            res = file_begin(fsize, fname);
            if (res == UartResult::OK)
                send_ack(UART_CMD_FILE_BEGIN);
            else
                send_nack(UART_CMD_FILE_BEGIN, static_cast<uint8_t>(res));
            break;
        }

        case UART_CMD_FILE_DATA:
            res = file_data(payload_buf_, frame_len_);
            if (res == UartResult::OK)
                send_ack(UART_CMD_FILE_DATA);
            else
                send_nack(UART_CMD_FILE_DATA, static_cast<uint8_t>(res));
            break;

        case UART_CMD_FILE_END:
            res = file_end();
            if (res == UartResult::OK)
                send_ack(UART_CMD_FILE_END);
            else
                send_nack(UART_CMD_FILE_END, static_cast<uint8_t>(res));
            break;

        case UART_CMD_STATUS:
        {
            StatusResp s = query_status();
            uint8_t buf[13];
            buf[0]  = s.has_internet;
            buf[1]  = s.neighbor_count;
            buf[2]  = s.hops_to_gateway;
            buf[3]  = s.transfer_active;
            buf[4]  = s.transfer_progress;
            buf[5]  = s.is_exit_node;
            buf[6]  = s.node_addr & 0xFF;
            buf[7]  = (s.node_addr >> 8) & 0xFF;
            buf[8]  = s.free_heap & 0xFF;
            buf[9]  = (s.free_heap >> 8) & 0xFF;
            buf[10] = (s.free_heap >> 16) & 0xFF;
            buf[11] = (s.free_heap >> 24) & 0xFF;
            buf[12] = s.mqtt_connected;
            send_frame(UART_RESP_STATUS_RESP, buf, sizeof(buf));
            ESP_LOGD(TAG, "STATUS_RESP sent");
            break;
        }

        case UART_CMD_ABORT:
            res = abort_transfer();
            if (res == UartResult::OK)
                send_ack(UART_CMD_ABORT);
            else
                send_nack(UART_CMD_ABORT, static_cast<uint8_t>(res));
            break;

        default:
            ESP_LOGW(TAG, "Unknown command: 0x%02X", frame_cmd_);
            send_nack(frame_cmd_, 0xFF);
            break;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public programmatic API
 * ══════════════════════════════════════════════════════════════════════════ */

UartResult UartIngest::file_begin(uint32_t file_size, const char *filename)
{
    if (transfer_started_)
    {
        ESP_LOGW(TAG, "FILE_BEGIN while transfer in progress");
        return UartResult::ERR_BUSY;
    }

    if (ingest_active_)
    {
        ESP_LOGW(TAG, "FILE_BEGIN while ingest active, dropping old");
        if (ingest_buf_)
        {
            heap_caps_free(ingest_buf_);
            ingest_buf_ = nullptr;
        }
        if (ingest_file_)
        {
            fclose(ingest_file_);
            ingest_file_ = nullptr;
        }
#if CONFIG_FLP_SD_ENABLED
        if (ingest_to_sd_ && ingest_path_[0] != '\0')
        {
            std::remove(ingest_path_);
        }
#endif
        ingest_to_sd_ = false;
        ingest_path_[0] = '\0';
        ingest_active_ = false;
    }

    if (file_size == 0 || file_size > FLP_INGEST_MAX_SIZE_BYTES)
    {
        ESP_LOGE(TAG, "Invalid file size: %lu", (unsigned long)file_size);
        return UartResult::ERR_ALLOC;
    }

    size_t name_len = strnlen(filename, sizeof(filename_) - 1);
    memcpy(filename_, filename, name_len);
    filename_[name_len] = '\0';

    ingest_to_sd_ = false;
    ingest_path_[0] = '\0';

    size_t available = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ingest_buf_ = static_cast<uint8_t *>(
        heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM));
    if (ingest_buf_)
    {
        ingest_size_ = file_size;
        received_size_ = 0;
        ingest_active_ = true;

        ESP_LOGI(TAG, "FILE_BEGIN: \"%s\" (%lu bytes, PSRAM)",
                 filename_, (unsigned long)file_size);
        return UartResult::OK;
    }

#if CONFIG_FLP_SD_ENABLED
    esp_err_t sd_err = sdcard_init();
    if (sd_err != ESP_OK)
    {
        ESP_LOGE(TAG,
                 "Not enough PSRAM: need %lu, largest block %zu; SD init failed: %s",
                 (unsigned long)file_size,
                 available,
                 esp_err_to_name(sd_err));
        return UartResult::ERR_ALLOC;
    }

    std::snprintf(ingest_path_, sizeof(ingest_path_),
                  "/sdcard/.flp_ingest_%08lx.bin",
                  static_cast<unsigned long>(esp_timer_get_time() & 0xFFFFFFFF));
    ingest_file_ = std::fopen(ingest_path_, "wb");
    if (!ingest_file_)
    {
        ESP_LOGE(TAG,
                 "PSRAM alloc failed for %lu bytes (largest %zu) and SD spool open failed: %s",
                 (unsigned long)file_size,
                 available,
                 ingest_path_);
        return UartResult::ERR_ALLOC;
    }

    ingest_size_ = file_size;
    received_size_ = 0;
    ingest_active_ = true;
    ingest_to_sd_ = true;

    ESP_LOGW(TAG,
             "FILE_BEGIN: \"%s\" (%lu bytes, SD spool %s; largest_psram=%zu)",
             filename_,
             (unsigned long)file_size,
             ingest_path_,
             available);
    return UartResult::OK;
#else
    ESP_LOGE(TAG, "Not enough PSRAM: need %lu, largest block %zu",
             (unsigned long)file_size, available);
    return UartResult::ERR_ALLOC;
#endif
}

UartResult UartIngest::file_data(const uint8_t *data, uint16_t len)
{
    if (!ingest_active_)
        return UartResult::ERR_NO_TRANSFER;

    if (transfer_started_)
        return UartResult::ERR_BUSY;

    if (received_size_ + len > ingest_size_)
    {
        ESP_LOGE(TAG, "FILE_DATA overflow: %zu + %u > %zu",
                 received_size_, len, ingest_size_);
        return UartResult::ERR_OVERFLOW;
    }

    if (ingest_to_sd_)
    {
        if (!ingest_file_)
        {
            return UartResult::ERR_NO_TRANSFER;
        }
        size_t wrote = std::fwrite(data, 1, len, ingest_file_);
        if (wrote != len)
        {
            ESP_LOGE(TAG, "FILE_DATA SD write failed: %zu/%u", wrote, len);
            return UartResult::ERR_ALLOC;
        }
    }
    else
    {
        if (!ingest_buf_)
        {
            return UartResult::ERR_NO_TRANSFER;
        }
        memcpy(ingest_buf_ + received_size_, data, len);
    }

    received_size_ += len;
    return UartResult::OK;
}

UartResult UartIngest::file_end()
{
    if (!ingest_active_)
        return UartResult::ERR_NO_TRANSFER;

    if (transfer_started_)
        return UartResult::ERR_BUSY;

    ESP_LOGI(TAG, "FILE_END: \"%s\" received %zu/%zu bytes",
             filename_, received_size_, ingest_size_);

    if (!mgr_)
    {
        ESP_LOGE(TAG, "FILE_END with null MeshManager");
        return UartResult::ERR_NO_TRANSFER;
    }

    EventGroupHandle_t events = mgr_->get_events();
    if (events)
        xEventGroupClearBits(events, FLP_EVT_TRANSFER_COMPLETE);

    bool started = false;
    if (ingest_to_sd_)
    {
#if CONFIG_FLP_SD_ENABLED
        if (!ingest_file_)
        {
            return UartResult::ERR_NO_TRANSFER;
        }
        std::fflush(ingest_file_);
        std::fclose(ingest_file_);
        ingest_file_ = nullptr;

        started = mgr_->start_file_transfer(
            filename_,
            received_size_,
            [this](uint8_t *buf, size_t offset, size_t len) -> size_t
            {
                return sdcard_read_chunk(ingest_path_, buf, offset, len);
            });
#else
        return UartResult::ERR_NO_TRANSFER;
#endif
    }
    else
    {
        if (!ingest_buf_)
        {
            return UartResult::ERR_NO_TRANSFER;
        }
        started = mgr_->start_file_transfer(
            filename_,
            received_size_,
            TransferEngine::make_buffer_reader(ingest_buf_, received_size_));
    }

    if (!started)
        return UartResult::ERR_NO_TRANSFER;

    /* Non-blocking: track progress via polling in run() loop */
    transfer_started_ = true;
    transfer_start_ms_ = now_ms();
    last_progress_pct_ = 0;

    return UartResult::OK;
}

StatusResp UartIngest::query_status() const
{
    StatusResp s = {};
    if (mgr_)
    {
        s.has_internet      = mgr_->has_internet() ? 1 : 0;
        s.neighbor_count    = mgr_->get_neighbor_count();
        s.hops_to_gateway   = mgr_->get_hops_to_internet();
        s.transfer_active   = mgr_->is_transfer_active() ? 1 : 0;
        s.transfer_progress = mgr_->get_transfer_progress();
        s.is_exit_node      = mgr_->is_exit_node() ? 1 : 0;
        s.node_addr         = mgr_->get_addr();
        s.free_heap         = static_cast<uint32_t>(
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        s.mqtt_connected    = mgr_->is_mqtt_connected() ? 1 : 0;
    }
    return s;
}

UartResult UartIngest::abort_transfer()
{
    if (!ingest_active_)
        return UartResult::ERR_NO_TRANSFER;

    ESP_LOGI(TAG, "ABORT: cancelling ingest \"%s\"", filename_);

    if (ingest_buf_ && !transfer_started_)
    {
        /* Buffer not yet handed to transfer engine — safe to free */
        heap_caps_free(ingest_buf_);
        ingest_buf_ = nullptr;
    }
    if (ingest_file_)
    {
        std::fclose(ingest_file_);
        ingest_file_ = nullptr;
    }
#if CONFIG_FLP_SD_ENABLED
    if (ingest_to_sd_ && !transfer_started_ && ingest_path_[0] != '\0')
    {
        std::remove(ingest_path_);
        ingest_path_[0] = '\0';
    }
#endif
    /* If transfer_started_, the read closure owns the source for the
     * transfer lifetime. poll_transfer_progress() will clean it up once
     * is_transfer_active() returns false. */

    ingest_active_ = false;
    transfer_started_ = false;
    received_size_ = 0;
    ingest_size_ = 0;

    return UartResult::OK;
}

void UartIngest::push_event(const UartEvent &evt)
{
    if (event_queue_)
    {
        if (xQueueSend(event_queue_, &evt, 0) != pdTRUE)
            ESP_LOGD(TAG, "Event queue full, dropping evt=0x%02X", evt.type);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Polling helpers (called from run loop)
 * ══════════════════════════════════════════════════════════════════════════ */

void UartIngest::poll_transfer_progress()
{
    if (!transfer_started_ || !mgr_)
        return;

    if (mgr_->is_transfer_active())
    {
        uint8_t pct = mgr_->get_transfer_progress();
        if (pct >= last_progress_pct_ + PROGRESS_REPORT_STEP)
        {
            last_progress_pct_ = pct;
            send_frame(UART_EVT_TRANSFER_PROGRESS, &pct, 1);
        }
    }
    else
    {
        /* Transfer finished */
        uint32_t duration_ms = now_ms() - transfer_start_ms_;
        uint8_t success = 1;

        uint8_t buf[5];
        buf[0] = success;
        buf[1] = duration_ms & 0xFF;
        buf[2] = (duration_ms >> 8) & 0xFF;
        buf[3] = (duration_ms >> 16) & 0xFF;
        buf[4] = (duration_ms >> 24) & 0xFF;
        send_frame(UART_EVT_TRANSFER_COMPLETE, buf, sizeof(buf));

        ESP_LOGI(TAG, "Transfer complete: %s (%lu ms)",
                 success ? "success" : "fail",
                 (unsigned long)duration_ms);

        if (ingest_buf_)
        {
            heap_caps_free(ingest_buf_);
            ingest_buf_ = nullptr;
        }
        if (ingest_file_)
        {
            std::fclose(ingest_file_);
            ingest_file_ = nullptr;
        }
#if CONFIG_FLP_SD_ENABLED
        if (ingest_to_sd_ && ingest_path_[0] != '\0')
        {
            std::remove(ingest_path_);
            ingest_path_[0] = '\0';
        }
#endif
        ingest_active_ = false;
        transfer_started_ = false;
        received_size_ = 0;
        ingest_size_ = 0;
        ingest_to_sd_ = false;
    }
}

void UartIngest::poll_mesh_state()
{
    if (!mgr_)
        return;

    uint32_t now = now_ms();
    if ((now - last_mesh_poll_ms_) < MESH_POLL_INTERVAL_MS)
        return;
    last_mesh_poll_ms_ = now;

    uint8_t hops = mgr_->get_hops_to_internet();
    uint8_t nbrs = mgr_->get_neighbor_count();

    if (hops != last_mesh_hops_ || nbrs != last_mesh_neighbors_)
    {
        uint8_t gateway_found = (hops < 0xFF) ? 1 : 0;
        uint8_t buf[3] = {gateway_found, hops, nbrs};
        send_frame(UART_EVT_MESH_STATE, buf, sizeof(buf));
        last_mesh_hops_ = hops;
        last_mesh_neighbors_ = nbrs;
    }
}

void UartIngest::drain_event_queue()
{
    if (!event_queue_)
        return;

    UartEvent evt;
    for (int i = 0; i < 4; i++)
    {
        if (xQueueReceive(event_queue_, &evt, 0) != pdTRUE)
            break;
        send_frame(evt.type, evt.payload, evt.len);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Wire framing
 * ══════════════════════════════════════════════════════════════════════════ */

void UartIngest::send_ack(uint8_t original_cmd)
{
    send_frame(UART_RESP_ACK, &original_cmd, 1);
}

void UartIngest::send_nack(uint8_t original_cmd, uint8_t error)
{
    uint8_t payload[2] = {original_cmd, error};
    send_frame(UART_RESP_NACK, payload, 2);
}

void UartIngest::send_frame(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    uint8_t header[5];
    header[0] = UART_SYNC1;
    header[1] = UART_SYNC2;
    header[2] = cmd;
    header[3] = len & 0xFF;
    header[4] = (len >> 8) & 0xFF;

    uart_write_bytes(port_, header, sizeof(header));
    if (len > 0 && payload)
        uart_write_bytes(port_, payload, len);
}

} /* namespace flp */
