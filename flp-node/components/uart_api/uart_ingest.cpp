#include "uart_ingest.hpp"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mesh_manager.hpp"

static const char *TAG = "uart_ingest";

namespace
{
#ifdef FLP_INGEST_MAX_SIZE
constexpr size_t FLP_INGEST_MAX_SIZE_BYTES =
    static_cast<size_t>(FLP_INGEST_MAX_SIZE);
#else
constexpr size_t FLP_INGEST_MAX_SIZE_BYTES = 3U * 1024U * 1024U;
#endif
} /* namespace */

namespace flp
{

void UartIngest::init(uart_port_t port,
                      int tx_pin,
                      int rx_pin,
                      MeshManager *mgr)
{
    port_ = port;
    mgr_ = mgr;

    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate = 115200;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_param_config(port_, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(
        port_, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(port_, 2048, 1024, 0, nullptr, 0));

    ESP_LOGI(TAG, "UART%d initialized (TX=%d, RX=%d)", port_, tx_pin, rx_pin);
}

void UartIngest::run()
{
    uint8_t byte;
    while (true)
    {
        int n = uart_read_bytes(port_, &byte, 1, pdMS_TO_TICKS(100));
        if (n <= 0)
        {
            continue;
        }

        switch (state_)
        {
            case ParseState::SYNC1:
                if (byte == UART_SYNC1)
                {
                    state_ = ParseState::SYNC2;
                }
                break;

            case ParseState::SYNC2:
                state_ =
                    (byte == UART_SYNC2) ? ParseState::CMD : ParseState::SYNC1;
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
                frame_len_ |= (uint16_t) byte << 8;
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
}

void UartIngest::reset_parser()
{
    state_ = ParseState::SYNC1;
    frame_cmd_ = 0;
    frame_len_ = 0;
    payload_idx_ = 0;
}

void UartIngest::handle_frame()
{
    switch (frame_cmd_)
    {
        case UART_CMD_FILE_BEGIN:
            handle_file_begin(payload_buf_, frame_len_);
            break;
        case UART_CMD_FILE_DATA:
            handle_file_data(payload_buf_, frame_len_);
            break;
        case UART_CMD_FILE_END:
            handle_file_end();
            break;
        case UART_CMD_STATUS:
            handle_status();
            break;
        default:
            ESP_LOGW(TAG, "Unknown command: 0x%02X", frame_cmd_);
            send_nack(frame_cmd_, 0xFF);
            break;
    }
}

void UartIngest::handle_file_begin(const uint8_t *payload, uint16_t len)
{
    if (len < 5)
    { /* 4 bytes size + at least 1 byte filename */
        send_nack(UART_CMD_FILE_BEGIN, UART_ERR_ALLOC_FAIL);
        return;
    }

    if (ingest_active_)
    {
        ESP_LOGW(TAG, "FILE_BEGIN while ingest already active, dropping old");
        if (ingest_buf_)
        {
            heap_caps_free(ingest_buf_);
            ingest_buf_ = nullptr;
        }
        ingest_active_ = false;
    }

    uint32_t file_size = payload[0] | ((uint32_t) payload[1] << 8) |
                         ((uint32_t) payload[2] << 16) |
                         ((uint32_t) payload[3] << 24);

    if (file_size == 0 || file_size > FLP_INGEST_MAX_SIZE_BYTES)
    {
        ESP_LOGE(TAG, "Invalid file size: %lu", (unsigned long) file_size);
        send_nack(UART_CMD_FILE_BEGIN, UART_ERR_ALLOC_FAIL);
        return;
    }

    /* Pre-allocation guard: check largest contiguous PSRAM block */
    size_t available = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (file_size > available)
    {
        ESP_LOGE(TAG,
                 "Not enough PSRAM: need %lu, largest block %zu",
                 (unsigned long) file_size,
                 available);
        send_nack(UART_CMD_FILE_BEGIN, UART_ERR_ALLOC_FAIL);
        return;
    }

    /* Copy null-terminated filename */
    size_t name_len = strnlen((const char *) &payload[4], len - 4);
    if (name_len >= sizeof(filename_))
    {
        name_len = sizeof(filename_) - 1;
    }
    memcpy(filename_, &payload[4], name_len);
    filename_[name_len] = '\0';

    ingest_buf_ = (uint8_t *) heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!ingest_buf_)
    {
        ESP_LOGE(
            TAG, "PSRAM alloc failed for %lu bytes", (unsigned long) file_size);
        send_nack(UART_CMD_FILE_BEGIN, UART_ERR_ALLOC_FAIL);
        return;
    }

    ingest_size_ = file_size;
    received_size_ = 0;
    ingest_active_ = true;

    ESP_LOGI(TAG,
             "FILE_BEGIN: \"%s\" (%lu bytes)",
             filename_,
             (unsigned long) file_size);
    send_ack(UART_CMD_FILE_BEGIN);
}

void UartIngest::handle_file_data(const uint8_t *payload, uint16_t len)
{
    if (!ingest_active_ || !ingest_buf_)
    {
        send_nack(UART_CMD_FILE_DATA, UART_ERR_NO_TRANSFER);
        return;
    }

    if (received_size_ + len > ingest_size_)
    {
        ESP_LOGE(TAG,
                 "FILE_DATA overflow: %zu + %u > %zu",
                 received_size_,
                 len,
                 ingest_size_);
        send_nack(UART_CMD_FILE_DATA, UART_ERR_OVERFLOW);
        return;
    }

    memcpy(ingest_buf_ + received_size_, payload, len);
    received_size_ += len;
    send_ack(UART_CMD_FILE_DATA);
}

void UartIngest::handle_file_end()
{
    if (!ingest_active_ || !ingest_buf_)
    {
        send_nack(UART_CMD_FILE_END, UART_ERR_NO_TRANSFER);
        return;
    }

    ESP_LOGI(TAG,
             "FILE_END: \"%s\" received %zu/%zu bytes",
             filename_,
             received_size_,
             ingest_size_);

    if (!mgr_)
    {
        ESP_LOGE(TAG, "FILE_END with null MeshManager");
        send_nack(UART_CMD_FILE_END, UART_ERR_NO_TRANSFER);
        return;
    }

    /*
     * Hand off to mesh manager — it reads from this buffer throughout the
     * multi-minute transfer. Do NOT free it here.
     */
    mgr_->start_file_transfer(filename_, ingest_buf_, received_size_);
    send_ack(UART_CMD_FILE_END);

    /*
     * Wait for the mesh transfer to complete before freeing the buffer.
     * MeshManager sets FLP_EVT_TRANSFER_COMPLETE when all fragments are ACKed.
     */
    EventGroupHandle_t events = mgr_->get_events();
    if (events)
    {
        xEventGroupWaitBits(
            events, FLP_EVT_TRANSFER_COMPLETE, pdTRUE, pdTRUE, portMAX_DELAY);
    }

    heap_caps_free(ingest_buf_);
    ingest_buf_ = nullptr;
    ingest_active_ = false;
}

void UartIngest::handle_status()
{
    uint8_t resp[5];
    resp[0] =
        mgr_->get_events()
            ? (xEventGroupGetBits(mgr_->get_events()) & FLP_EVT_WIFI_CONNECTED)
                  ? 1
                  : 0
            : 0; /* has_inet */
    resp[1] = 0; /* neighbor count (TODO: expose from route table) */
    resp[2] = 0; /* transfer_active (TODO: expose from MeshManager) */
    uint16_t addr = mgr_->get_addr();
    resp[3] = addr & 0xFF;
    resp[4] = (addr >> 8) & 0xFF;

    send_frame(UART_RESP_STATUS_RESP, resp, sizeof(resp));
    ESP_LOGD(TAG, "STATUS_RESP sent");
}

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
    {
        uart_write_bytes(port_, payload, len);
    }
}

} /* namespace flp */
