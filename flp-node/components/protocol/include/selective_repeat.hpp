#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fec_codec.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "packet.hpp"

namespace flp
{

struct FragmentSlot
{
    uint8_t data[MAX_MTU];
    size_t len = 0;
    uint32_t send_time_ms = 0;
    bool acked = false;
    bool sent = false;
    uint8_t retries = 0;
};

/* Callback for sending packets (ACK, NACK, retransmit data).
 * Returns 0 on success, -1 on failure (e.g. ESP_ERR_ESPNOW_NO_MEM). */
using SendCallback = std::function<int(uint16_t dst,
                                       PacketType type,
                                       uint16_t seq,
                                       const uint8_t *data,
                                       size_t len)>;

class SelectiveRepeat
{
  public:
    SelectiveRepeat() = default;
    ~SelectiveRepeat();

    /* Non-copyable (owns heap memory) */
    SelectiveRepeat(const SelectiveRepeat &) = delete;
    SelectiveRepeat &operator=(const SelectiveRepeat &) = delete;

    void init(uint8_t window_size, uint32_t timeout_ms);

    /* Sender API */
    int send_fragment(uint16_t seq, const uint8_t *data, size_t len);
    void handle_ack(uint16_t seq);
    void handle_nack(uint16_t seq);
    /* Tick with external send budget.  Returns the number of
     * retransmissions actually sent so the caller can decrement a
     * shared budget across multiple send paths. */
    uint8_t tick(uint8_t max_sends);

    uint16_t get_base_seq() const
    {
        return base_seq_;
    }
    uint16_t get_next_seq() const
    {
        return next_seq_;
    }
    bool sender_window_full() const;
    uint16_t sender_window_used() const;
    void reset_sender();

    /* Phase 3: get send timestamp for RTT computation */
    uint32_t get_send_time(uint16_t seq) const
    {
        if (!window_) return 0;
        uint8_t idx = seq % window_size_;
        return window_[idx].send_time_ms;
    }

    /* Receiver API */
    bool init_receiver(uint16_t total_fragments,
                       size_t fragment_size,
                       size_t file_size);
    bool receive_fragment(uint16_t seq, const uint8_t *data, size_t len);
    bool is_complete() const;
    const uint8_t *get_reassembly_buffer() const
    {
        return reassembly_buf_;
    }
    size_t get_file_size() const
    {
        return file_size_;
    }
    void cleanup_receiver();

    /* Set the callback for outbound packets */
    void set_send_callback(SendCallback cb)
    {
        send_cb_ = cb;
    }

    /* Destination for sender-side retransmits and receiver-side ACKs */
    void set_peer_addr(uint16_t addr)
    {
        peer_addr_ = addr;
    }

    /* Update timeout (for adaptive ARQ based on hop count) */
    void set_timeout(uint32_t timeout_ms)
    {
        timeout_ms_ = timeout_ms;
    }

    /* True if any fragment has exceeded MAX_RETRIES without ACK */
    bool is_sender_failed() const
    {
        return sender_failed_;
    }

    /* Configure stride for multi-exit round-robin: this ARQ only owns
     * sequences where (seq % stride == offset).  stride=1 means all. */
    void set_exit_stride(uint8_t stride, uint8_t offset)
    {
        exit_stride_ = stride;
        exit_offset_ = offset;
    }

  private:
    uint32_t now_ms() const
    {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000);
    }

    /* Sender state — heap-allocated to keep BSS small (prefers PSRAM) */
    FragmentSlot *window_ = nullptr;
    uint8_t window_size_ = 0;
    uint32_t timeout_ms_ = 0;
    uint16_t base_seq_ = 0;
    uint16_t next_seq_ = 0;

    /* Multi-exit stride: skip non-owned seqs in base advancement / tick */
    uint8_t exit_stride_ = 1;   /* total exit nodes (1 = single-exit) */
    uint8_t exit_offset_ = 0;   /* this ARQ's index */
    bool sender_failed_ = false; /* set when any fragment exceeds MAX_RETRIES */

    /*
     * Adaptive RTO estimation (TCP-style, RFC 6298).
     *
     * Instead of fixed exponential backoff (2^retries * timeout_ms_),
     * we measure the actual round-trip time on each ACK and compute a
     * smoothed RTO.  This adapts to the real end-to-end latency
     * (source → mesh → exit → MQTT TLS → cloud ACK → exit → mesh → source)
     * which varies from ~200ms to ~5000ms depending on load.
     *
     * Lecture ref: "Failure Detectors" (Coulouris §15.1) — "use timeout
     * values that reflect the observed network delay conditions."
     */
    uint32_t srtt_ms_ = 0;          /* smoothed RTT */
    uint32_t rttvar_ms_ = 0;        /* RTT variance */
    uint32_t rto_ms_ = 0;           /* computed retransmit timeout */
    bool rtt_initialized_ = false;  /* first sample bootstraps SRTT */
    static constexpr uint32_t RTO_MIN_MS = 200;
    static constexpr uint32_t RTO_MAX_MS = 5000;

    /* Receiver state */
    uint8_t *reassembly_buf_ = nullptr;
    uint8_t *recv_bitmap_ = nullptr;
    uint32_t *nack_sent_ms_ = nullptr; /* Per-seq NACK cooldown timestamps */
    uint16_t total_fragments_ = 0;
    uint16_t fragments_received_ = 0;
    uint16_t expected_seq_ = 0; /* Next expected in-order fragment */
    size_t fragment_size_ = 0;
    size_t file_size_ = 0;
    bool receiver_active_ = false;

    FecDecoder fec_decoder_;

    SendCallback send_cb_ = nullptr;
    uint16_t peer_addr_ = BROADCAST_ADDR;
};

} /* namespace flp */
