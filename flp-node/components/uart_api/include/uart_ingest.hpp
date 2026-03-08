#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/uart.h"

namespace flp
{

class MeshManager; /* forward declaration */

/* UART frame commands (Pico W → ESP32) */
constexpr uint8_t UART_CMD_FILE_BEGIN = 0x01;
constexpr uint8_t UART_CMD_FILE_DATA = 0x02;
constexpr uint8_t UART_CMD_FILE_END = 0x03;
constexpr uint8_t UART_CMD_STATUS = 0x04;

/* UART frame responses (ESP32 → Pico W) */
constexpr uint8_t UART_RESP_ACK = 0x80;
constexpr uint8_t UART_RESP_NACK = 0x81;
constexpr uint8_t UART_RESP_STATUS_RESP = 0x82;

/* NACK error codes */
constexpr uint8_t UART_ERR_ALLOC_FAIL = 0x01;
constexpr uint8_t UART_ERR_OVERFLOW = 0x02;
constexpr uint8_t UART_ERR_NO_TRANSFER = 0x03;
constexpr uint8_t UART_ERR_TRANSFER_BUSY = 0x04;

/* Sync bytes */
constexpr uint8_t UART_SYNC1 = 0xAA;
constexpr uint8_t UART_SYNC2 = 0x55;

class UartIngest
{
  public:
    UartIngest() = default;

    void init(uart_port_t port, int tx_pin, int rx_pin, MeshManager *mgr);
    void run(); /* FreeRTOS task loop */

  private:
    enum class ParseState : uint8_t
    {
        SYNC1,
        SYNC2,
        CMD,
        LEN_LO,
        LEN_HI,
        PAYLOAD,
    };

    void reset_parser();
    void handle_frame();
    void handle_file_begin(const uint8_t *payload, uint16_t len);
    void handle_file_data(const uint8_t *payload, uint16_t len);
    void handle_file_end();
    void handle_status();
    void send_ack(uint8_t original_cmd);
    void send_nack(uint8_t original_cmd, uint8_t error);
    void send_frame(uint8_t cmd, const uint8_t *payload, uint16_t len);

    uart_port_t port_ = UART_NUM_2;
    MeshManager *mgr_ = nullptr;

    /* Frame parser state */
    ParseState state_ = ParseState::SYNC1;
    uint8_t frame_cmd_ = 0;
    uint16_t frame_len_ = 0;
    uint16_t payload_idx_ = 0;
    uint8_t payload_buf_[1024 + 4]; /* max payload per frame */

    /* Ingest buffer (PSRAM) */
    uint8_t *ingest_buf_ = nullptr;
    size_t ingest_size_ = 0; /* expected total size */
    size_t received_size_ = 0; /* bytes received so far */
    char filename_[64] = {};
    bool ingest_active_ = false;
};

} /* namespace flp */
