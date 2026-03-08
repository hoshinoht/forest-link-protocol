#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

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
    size_t len;
    uint32_t send_time_ms;
    bool acked;
    bool sent;
    uint8_t retries;
};

/* Callback for sending packets (ACK, NACK, retransmit data) */
using SendCallback = std::function<void(uint16_t dst,
                                        PacketType type,
                                        uint16_t seq,
                                        const uint8_t *data,
                                        size_t len)>;

class SelectiveRepeat
{
  public:
    SelectiveRepeat() = default;

    void init(uint8_t window_size, uint32_t timeout_ms);

    /* Sender API */
    int send_fragment(uint16_t seq, const uint8_t *data, size_t len);
    void handle_ack(uint16_t seq);
    void handle_nack(uint16_t seq);
    void tick();

    uint16_t get_base_seq() const
    {
        return base_seq_;
    }
    uint16_t get_next_seq() const
    {
        return next_seq_;
    }
    bool sender_window_full() const;
    void reset_sender();

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

  private:
    uint32_t now_ms() const
    {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000);
    }

    /* Sender state */
    FragmentSlot window_[ARQ_WINDOW] = {};
    uint8_t window_size_ = 0;
    uint32_t timeout_ms_ = 0;
    uint16_t base_seq_ = 0;
    uint16_t next_seq_ = 0;

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
