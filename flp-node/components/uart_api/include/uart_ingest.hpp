#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace flp
{

class MeshManager; /* forward declaration */

/* ── UART frame commands (external device → ESP32) ─────────────────────── */
constexpr uint8_t UART_CMD_FILE_BEGIN = 0x01;
constexpr uint8_t UART_CMD_FILE_DATA  = 0x02;
constexpr uint8_t UART_CMD_FILE_END   = 0x03;
constexpr uint8_t UART_CMD_STATUS     = 0x04;
constexpr uint8_t UART_CMD_ABORT      = 0x05;

/* ── UART frame responses (ESP32 → external device) ───────────────────── */
constexpr uint8_t UART_RESP_ACK         = 0x80;
constexpr uint8_t UART_RESP_NACK        = 0x81;
constexpr uint8_t UART_RESP_STATUS_RESP = 0x82;

/* ── Async events (ESP32 → external device, unsolicited) ───────────────── */
constexpr uint8_t UART_EVT_TRANSFER_PROGRESS = 0xC0;
constexpr uint8_t UART_EVT_TRANSFER_COMPLETE = 0xC1;
constexpr uint8_t UART_EVT_DOWNLINK_DATA     = 0xC2;
constexpr uint8_t UART_EVT_MESH_STATE        = 0xC3;

/* ── NACK error codes ──────────────────────────────────────────────────── */
constexpr uint8_t UART_ERR_ALLOC_FAIL      = 0x01;
constexpr uint8_t UART_ERR_OVERFLOW        = 0x02;
constexpr uint8_t UART_ERR_NO_TRANSFER     = 0x03;
constexpr uint8_t UART_ERR_TRANSFER_ACTIVE = 0x04;

/* ── Sync bytes ────────────────────────────────────────────────────────── */
constexpr uint8_t UART_SYNC1 = 0xAA;
constexpr uint8_t UART_SYNC2 = 0x55;

/* ── Public API result codes ───────────────────────────────────────────── */
enum class UartResult : uint8_t
{
    OK,
    ERR_ALLOC,
    ERR_OVERFLOW,
    ERR_NO_TRANSFER,
    ERR_BUSY,
};

/* STATUS response struct (13 bytes on wire) */
struct StatusResp
{
    uint8_t  has_internet;       /* 0/1 */
    uint8_t  neighbor_count;
    uint8_t  hops_to_gateway;    /* 0xFF = no gateway */
    uint8_t  transfer_active;    /* 0/1 */
    uint8_t  transfer_progress;  /* 0-100 */
    uint8_t  is_exit_node;       /* 0/1 */
    uint16_t node_addr;
    uint32_t free_heap;
    uint8_t  mqtt_connected;     /* 0/1 */
};

/* Async event (queued from other tasks, drained by UART run loop) */
struct UartEvent
{
    uint8_t type;
    uint8_t payload[64];
    uint16_t len;
};

class UartIngest
{
  public:
    UartIngest() = default;

    void init(uart_port_t port, int tx_pin, int rx_pin, MeshManager *mgr);
    void run(); /* FreeRTOS task loop */

    /* ── Public programmatic API (used by FlpClient or wire parser) ───── */

    /* Begin a file ingest. Allocates PSRAM buffer.
     * filename must be null-terminated, max 63 chars. */
    UartResult file_begin(uint32_t file_size, const char *filename);

    /* Append data to the ingest buffer. */
    UartResult file_data(const uint8_t *data, uint16_t len);

    /* Finalize ingest and start mesh transfer (non-blocking). */
    UartResult file_end();

    /* Query node status. */
    StatusResp query_status() const;

    /* Cancel active ingest or transfer. */
    UartResult abort_transfer();

    /* Push an async event (thread-safe, called from mesh task etc.) */
    void push_event(const UartEvent &evt);

    /* Check if an ingest/transfer is in progress */
    bool is_busy() const { return ingest_active_; }

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
    void send_ack(uint8_t original_cmd);
    void send_nack(uint8_t original_cmd, uint8_t error);
    void send_frame(uint8_t cmd, const uint8_t *payload, uint16_t len);
    void poll_transfer_progress();
    void poll_mesh_state();
    void drain_event_queue();

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
    size_t ingest_size_ = 0;     /* expected total size */
    size_t received_size_ = 0;   /* bytes received so far */
    char filename_[64] = {};
    bool ingest_active_ = false;

    /* Async transfer tracking (non-blocking FILE_END) */
    bool transfer_started_ = false;
    uint32_t transfer_start_ms_ = 0;
    uint8_t last_progress_pct_ = 0;

    /* Mesh state polling */
    uint32_t last_mesh_poll_ms_ = 0;
    uint8_t last_mesh_hops_ = 0xFF;
    uint8_t last_mesh_neighbors_ = 0;

    /* Event queue for async notifications */
    QueueHandle_t event_queue_ = nullptr;
};

} /* namespace flp */
