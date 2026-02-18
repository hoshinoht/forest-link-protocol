#include "selective_repeat.hpp"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "arq";

namespace flp
{

void SelectiveRepeat::init(uint8_t window_size, uint32_t timeout_ms)
{
    window_size_ = (window_size > ARQ_WINDOW) ? ARQ_WINDOW : window_size;
    timeout_ms_ = timeout_ms;
    reset_sender();
    ESP_LOGI(
        TAG, "ARQ init: window=%u timeout=%lums", window_size_, timeout_ms_);
}

// --- Sender ---

void SelectiveRepeat::reset_sender()
{
    base_seq_ = 0;
    next_seq_ = 0;
    memset(window_, 0, sizeof(window_));
}

bool SelectiveRepeat::sender_window_full() const
{
    return (next_seq_ - base_seq_) >= window_size_;
}

int SelectiveRepeat::send_fragment(uint16_t seq,
                                   const uint8_t *data,
                                   size_t len)
{
    if (len > MAX_MTU)
    {
        ESP_LOGE(TAG, "Fragment too large: %zu > %zu", len, MAX_MTU);
        return -1;
    }

    uint8_t idx = seq % window_size_;
    FragmentSlot &slot = window_[idx];

    memcpy(slot.data, data, len);
    slot.len = len;
    slot.send_time_ms = now_ms();
    slot.acked = false;
    slot.sent = true;
    slot.retries = 0;

    if (seq >= next_seq_)
    {
        next_seq_ = seq + 1;
    }

    // Send via callback
    if (send_cb_)
    {
        send_cb_(peer_addr_, PacketType::DATA, seq, data, len);
    }

    ESP_LOGD(TAG,
             "TX frag seq=%u len=%zu base=%u next=%u",
             seq,
             len,
             base_seq_,
             next_seq_);
    return 0;
}

void SelectiveRepeat::handle_ack(uint16_t seq)
{
    if (seq < base_seq_ || seq >= next_seq_)
    {
        ESP_LOGW(
            TAG, "ACK seq=%u out of window [%u,%u)", seq, base_seq_, next_seq_);
        return;
    }

    uint8_t idx = seq % window_size_;
    window_[idx].acked = true;

    ESP_LOGD(TAG, "ACK seq=%u", seq);

    // Advance base while consecutive slots are acked
    while (base_seq_ < next_seq_)
    {
        uint8_t base_idx = base_seq_ % window_size_;
        if (!window_[base_idx].acked)
        {
            break;
        }
        window_[base_idx].sent = false;
        base_seq_++;
    }
}

void SelectiveRepeat::handle_nack(uint16_t seq)
{
    if (seq < base_seq_ || seq >= next_seq_)
    {
        ESP_LOGW(TAG,
                 "NACK seq=%u out of window [%u,%u)",
                 seq,
                 base_seq_,
                 next_seq_);
        return;
    }

    uint8_t idx = seq % window_size_;
    FragmentSlot &slot = window_[idx];

    if (slot.retries >= MAX_RETRIES)
    {
        ESP_LOGE(TAG, "NACK seq=%u max retries reached", seq);
        return;
    }

    slot.retries++;
    slot.send_time_ms = now_ms();

    ESP_LOGD(TAG, "NACK retransmit seq=%u retry=%u", seq, slot.retries);

    if (send_cb_)
    {
        send_cb_(peer_addr_, PacketType::DATA, seq, slot.data, slot.len);
    }
}

void SelectiveRepeat::tick()
{
    uint32_t now = now_ms();

    for (uint16_t seq = base_seq_; seq < next_seq_; seq++)
    {
        uint8_t idx = seq % window_size_;
        FragmentSlot &slot = window_[idx];

        if (!slot.sent || slot.acked)
        {
            continue;
        }

        if ((now - slot.send_time_ms) >= timeout_ms_)
        {
            if (slot.retries >= MAX_RETRIES)
            {
                ESP_LOGE(TAG, "Timeout seq=%u max retries exceeded", seq);
                continue;
            }

            slot.retries++;
            slot.send_time_ms = now;

            ESP_LOGW(
                TAG, "Timeout retransmit seq=%u retry=%u", seq, slot.retries);

            if (send_cb_)
            {
                send_cb_(
                    peer_addr_, PacketType::DATA, seq, slot.data, slot.len);
            }
        }
    }
}

// --- Receiver ---

bool SelectiveRepeat::init_receiver(uint16_t total_fragments,
                                    size_t fragment_size,
                                    size_t file_size)
{
    cleanup_receiver();

    total_fragments_ = total_fragments;
    fragment_size_ = fragment_size;
    file_size_ = file_size;
    fragments_received_ = 0;

    // Try PSRAM first, fall back to regular heap
    reassembly_buf_ =
        static_cast<uint8_t *>(heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM));
    if (!reassembly_buf_)
    {
        ESP_LOGW(TAG,
                 "PSRAM alloc failed (%zu bytes), trying regular heap",
                 file_size);
        reassembly_buf_ = static_cast<uint8_t *>(malloc(file_size));
    }
    if (!reassembly_buf_)
    {
        ESP_LOGE(
            TAG, "Failed to allocate reassembly buffer (%zu bytes)", file_size);
        return false;
    }

    // Bitmap: 1 bit per fragment, rounded up to bytes
    size_t bitmap_bytes = (total_fragments + 7) / 8;
    recv_bitmap_ = static_cast<uint8_t *>(calloc(1, bitmap_bytes));
    if (!recv_bitmap_)
    {
        ESP_LOGE(TAG, "Failed to allocate bitmap (%zu bytes)", bitmap_bytes);
        free(reassembly_buf_);
        reassembly_buf_ = nullptr;
        return false;
    }

    receiver_active_ = true;
    ESP_LOGI(TAG,
             "Receiver init: %u fragments, %zu bytes each, %zu total",
             total_fragments,
             fragment_size,
             file_size);
    return true;
}

bool SelectiveRepeat::receive_fragment(uint16_t seq,
                                       const uint8_t *data,
                                       size_t len)
{
    if (!receiver_active_ || seq >= total_fragments_)
    {
        ESP_LOGW(TAG,
                 "RX frag seq=%u invalid (active=%d total=%u)",
                 seq,
                 receiver_active_,
                 total_fragments_);
        return false;
    }

    // Check for duplicate
    uint8_t byte_idx = seq / 8;
    uint8_t bit_mask = 1 << (seq % 8);
    if (recv_bitmap_[byte_idx] & bit_mask)
    {
        ESP_LOGD(TAG, "Duplicate frag seq=%u, sending ACK", seq);
        if (send_cb_)
        {
            send_cb_(peer_addr_, PacketType::ACK, seq, nullptr, 0);
        }
        return false;
    }

    // Store fragment at correct offset
    size_t offset = static_cast<size_t>(seq) * fragment_size_;
    size_t copy_len = len;
    if (offset + copy_len > file_size_)
    {
        copy_len = file_size_ - offset;
    }
    memcpy(reassembly_buf_ + offset, data, copy_len);

    // Mark received
    recv_bitmap_[byte_idx] |= bit_mask;
    fragments_received_++;

    // Send ACK
    if (send_cb_)
    {
        send_cb_(peer_addr_, PacketType::ACK, seq, nullptr, 0);
    }

    ESP_LOGD(TAG,
             "RX frag seq=%u (%u/%u)",
             seq,
             fragments_received_,
             total_fragments_);

    // Check for gaps and send NACKs for missing earlier fragments
    for (uint16_t i = 0; i < seq; i++)
    {
        uint8_t bi = i / 8;
        uint8_t bm = 1 << (i % 8);
        if (!(recv_bitmap_[bi] & bm))
        {
            if (send_cb_)
            {
                send_cb_(peer_addr_, PacketType::NACK, i, nullptr, 0);
            }
        }
    }

    return true;
}

bool SelectiveRepeat::is_complete() const
{
    return receiver_active_ && (fragments_received_ == total_fragments_);
}

void SelectiveRepeat::cleanup_receiver()
{
    if (reassembly_buf_)
    {
        // Try heap_caps_free for PSRAM, but free() works for both
        free(reassembly_buf_);
        reassembly_buf_ = nullptr;
    }
    if (recv_bitmap_)
    {
        free(recv_bitmap_);
        recv_bitmap_ = nullptr;
    }
    total_fragments_ = 0;
    fragments_received_ = 0;
    receiver_active_ = false;
}

} // namespace flp
