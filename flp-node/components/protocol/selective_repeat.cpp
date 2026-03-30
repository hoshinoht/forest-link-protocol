#include "selective_repeat.hpp"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "arq";

namespace flp
{

SelectiveRepeat::~SelectiveRepeat()
{
    if (window_)
    {
        heap_caps_free(window_);
        window_ = nullptr;
    }
    cleanup_receiver();
}

void SelectiveRepeat::init(uint8_t window_size, uint32_t timeout_ms)
{
    window_size_ = (window_size > ARQ_WINDOW) ? ARQ_WINDOW : window_size;
    timeout_ms_ = timeout_ms;

    /* Allocate sender window on heap, preferring PSRAM */
    if (!window_)
    {
        window_ = static_cast<FragmentSlot *>(heap_caps_calloc(
            ARQ_WINDOW, sizeof(FragmentSlot),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!window_)
        {
            /* Fall back to internal RAM */
            window_ = static_cast<FragmentSlot *>(
                heap_caps_calloc(ARQ_WINDOW, sizeof(FragmentSlot),
                                 MALLOC_CAP_8BIT));
        }
        if (!window_)
        {
            ESP_LOGE(TAG, "Failed to allocate ARQ window (%zu bytes)",
                     ARQ_WINDOW * sizeof(FragmentSlot));
            return;
        }
        ESP_LOGI(TAG, "ARQ window allocated: %zu bytes",
                 ARQ_WINDOW * sizeof(FragmentSlot));
    }

    reset_sender();
    ESP_LOGI(
        TAG, "ARQ init: window=%u timeout=%lums", window_size_, timeout_ms_);
}

/* --- Sender --- */

void SelectiveRepeat::reset_sender()
{
    base_seq_ = 0;
    next_seq_ = 0;
    exit_stride_ = 1;
    exit_offset_ = 0;
    sender_failed_ = false;
    if (window_)
    {
        memset(window_, 0, ARQ_WINDOW * sizeof(FragmentSlot));
    }
}

bool SelectiveRepeat::sender_window_full() const
{
    return (next_seq_ - base_seq_) >= window_size_;
}

uint16_t SelectiveRepeat::sender_window_used() const
{
    return next_seq_ - base_seq_;
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

    /* Send via callback */
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

    /* Advance base while consecutive owned slots are acked */
    while (base_seq_ < next_seq_)
    {
        /* Skip sequences not assigned to this ARQ instance */
        if (exit_stride_ > 1 &&
            (base_seq_ % exit_stride_) != exit_offset_)
        {
            base_seq_++;
            continue;
        }
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

uint8_t SelectiveRepeat::tick(uint8_t max_sends)
{
    uint32_t now = now_ms();
    uint8_t retx_count = 0;

    for (uint16_t seq = base_seq_; seq < next_seq_; seq++)
    {
        /* Skip sequences not assigned to this ARQ */
        if (exit_stride_ > 1 &&
            (seq % exit_stride_) != exit_offset_)
        {
            continue;
        }

        uint8_t idx = seq % window_size_;
        FragmentSlot &slot = window_[idx];

        if (!slot.sent || slot.acked)
        {
            continue;
        }

        /*
         * Exponential backoff: timeout doubles with each retry.
         *   retry 0 (first send): timeout_ms_        (2000ms)
         *   retry 1:              timeout_ms_ * 2    (4000ms)
         *   retry 2:              timeout_ms_ * 4    (8000ms)
         *   retry 3:              timeout_ms_ * 8    (16000ms)  capped
         *   ...
         * This gives the deferred-ACK pipeline (ESP-NOW → MQTT queue →
         * TLS publish → fragment_ack_queue → mesh tick → ACK back) more
         * time to complete under load, instead of burning retries at a
         * fixed 2s interval that's too aggressive for the slowest path.
         */
        uint32_t backoff = timeout_ms_ << slot.retries; /* 2^retries */
        static constexpr uint32_t MAX_BACKOFF_MS = 16000;
        if (backoff > MAX_BACKOFF_MS)
        {
            backoff = MAX_BACKOFF_MS;
        }

        if ((now - slot.send_time_ms) >= backoff)
        {
            if (slot.retries >= MAX_RETRIES)
            {
                ESP_LOGE(TAG, "Timeout seq=%u max retries exceeded", seq);
                sender_failed_ = true;
                continue;
            }

            if (retx_count >= max_sends)
            {
                break; /* defer remaining retransmissions to next tick */
            }

            if (send_cb_)
            {
                int rc = send_cb_(
                    peer_addr_, PacketType::DATA, seq, slot.data, slot.len);
                if (rc < 0)
                {
                    /* Send failed (NO_MEM) — don't burn a retry, stop.
                     * The slot keeps its current retry count and timestamp
                     * so the next tick will re-attempt after backoff. */
                    break;
                }
            }

            slot.retries++;
            slot.send_time_ms = now;
            retx_count++;

            ESP_LOGW(TAG, "Timeout retransmit seq=%u retry=%u backoff=%lums",
                     seq, slot.retries, (unsigned long) backoff);
        }
    }
    return retx_count;
}

/* --- Receiver --- */

bool SelectiveRepeat::init_receiver(uint16_t total_fragments,
                                    size_t fragment_size,
                                    size_t file_size)
{
    cleanup_receiver();

    total_fragments_ = total_fragments;
    fragment_size_ = fragment_size;
    file_size_ = file_size;
    fragments_received_ = 0;

    /* Pre-allocation guard: verify PSRAM can hold the reassembly buffer */
    size_t available = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (file_size > available)
    {
        ESP_LOGE(TAG,
                 "Not enough PSRAM for reassembly: need %zu, largest block %zu",
                 file_size,
                 available);
        return false;
    }

    /* Try PSRAM first, fall back to regular heap */
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

    /* Bitmap: 1 bit per fragment, rounded up to bytes */
    size_t bitmap_bytes = (total_fragments + 7) / 8;
    recv_bitmap_ = static_cast<uint8_t *>(calloc(1, bitmap_bytes));
    if (!recv_bitmap_)
    {
        ESP_LOGE(TAG, "Failed to allocate bitmap (%zu bytes)", bitmap_bytes);
        heap_caps_free(reassembly_buf_);
        reassembly_buf_ = nullptr;
        return false;
    }

    /* Per-seq NACK cooldown timestamps */
    nack_sent_ms_ = static_cast<uint32_t *>(
        calloc(total_fragments, sizeof(uint32_t)));
    if (!nack_sent_ms_)
    {
        ESP_LOGE(TAG, "Failed to allocate NACK cooldown array");
        free(recv_bitmap_);
        recv_bitmap_ = nullptr;
        heap_caps_free(reassembly_buf_);
        reassembly_buf_ = nullptr;
        return false;
    }

    expected_seq_ = 0;
    receiver_active_ = true;
    fec_decoder_.reset();
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

    bool is_parity = (seq % (FEC_GROUP_SIZE + 1) == FEC_GROUP_SIZE);

    /* Feed to FEC decoder before anything else so recovery can reduce NACKs */
    bool recovered = fec_decoder_.ingest(seq, data, len, is_parity);

    /*
     * Fix 11: Process the recovered fragment inline instead of calling
     * receive_fragment() recursively to avoid unbounded stack growth.
     * A recovered fragment is always a data fragment (never a parity slot),
     * so we skip the FEC-ingest step for it and go straight to the
     * bitmap/reassembly logic below by temporarily substituting seq/data/len.
     */
    if (recovered)
    {
        uint16_t rec_seq = fec_decoder_.recovered_seq();
        const uint8_t *rec_data = fec_decoder_.recovered_data();
        size_t rec_len = fec_decoder_.recovered_len();

        ESP_LOGI(TAG, "FEC recovered seq=%u from group", rec_seq);

        /* Validate range */
        if (rec_seq < total_fragments_)
        {
            size_t bmap_bytes = (total_fragments_ + 7) / 8;
            uint16_t rbi = rec_seq / 8;
            uint8_t rbm = 1 << (rec_seq % 8);
            if (rbi < bmap_bytes && !(recv_bitmap_[rbi] & rbm))
            {
                recv_bitmap_[rbi] |= rbm;
                fragments_received_++;

                if (send_cb_)
                {
                    send_cb_(peer_addr_, PacketType::ACK, rec_seq, nullptr, 0);
                }

                /* Store in reassembly buffer */
                uint16_t rgroup = rec_seq / (FEC_GROUP_SIZE + 1);
                uint16_t ridx_in_group = rec_seq % (FEC_GROUP_SIZE + 1);
                uint16_t rdata_idx = rgroup * FEC_GROUP_SIZE + ridx_in_group;
                size_t rmax_data = (fragment_size_ > 0)
                                       ? (file_size_ / fragment_size_)
                                       : 0;
                if (rdata_idx < rmax_data)
                {
                    size_t roffset = static_cast<size_t>(rdata_idx) *
                                     fragment_size_;
                    size_t rcopy = rec_len;
                    if (roffset + rcopy > file_size_)
                    {
                        rcopy = file_size_ - roffset;
                    }
                    memcpy(reassembly_buf_ + roffset, rec_data, rcopy);
                }
            }
        }
    }

    /* Check for duplicate */
    size_t bitmap_bytes = (total_fragments_ + 7) / 8;
    uint16_t byte_idx = seq / 8;
    uint8_t bit_mask = 1 << (seq % 8);
    if (byte_idx >= bitmap_bytes)
    {
        ESP_LOGE(TAG,
                 "RX frag seq=%u byte_idx=%u exceeds bitmap size %zu, "
                 "dropping",
                 seq,
                 byte_idx,
                 bitmap_bytes);
        return false;
    }
    if (recv_bitmap_[byte_idx] & bit_mask)
    {
        ESP_LOGD(TAG, "Duplicate frag seq=%u, sending ACK", seq);
        if (send_cb_)
        {
            send_cb_(peer_addr_, PacketType::ACK, seq, nullptr, 0);
        }
        return false;
    }

    /* Mark received and send ACK */
    recv_bitmap_[byte_idx] |= bit_mask;
    fragments_received_++;

    if (send_cb_)
    {
        send_cb_(peer_addr_, PacketType::ACK, seq, nullptr, 0);
    }

    /* Parity fragments: ACK but do NOT store in reassembly buffer */
    if (is_parity)
    {
        ESP_LOGD(TAG, "RX parity seq=%u (%u/%u)", seq,
                 fragments_received_, total_fragments_);
    }
    else
    {
        /* Map seq to data index (skip parity slots) */
        uint16_t group = seq / (FEC_GROUP_SIZE + 1);
        uint16_t idx_in_group = seq % (FEC_GROUP_SIZE + 1);
        uint16_t data_idx = group * FEC_GROUP_SIZE + idx_in_group;

        /* Guard: data_idx must be within the data-only region.
         * total_fragments_ counts all slots (data + parity); the reassembly
         * buffer covers only data positions. An out-of-range data_idx would
         * cause unsigned underflow in the file_size_ - offset subtraction. */
        size_t max_data_fragments = (fragment_size_ > 0)
                                        ? (file_size_ / fragment_size_)
                                        : 0;
        if (data_idx >= max_data_fragments)
        {
            ESP_LOGE(TAG,
                     "RX frag seq=%u data_idx=%u out of range (max=%zu), "
                     "dropping",
                     seq,
                     data_idx,
                     max_data_fragments);
            return false;
        }

        /* Store fragment at correct offset in reassembly buffer */
        size_t offset = static_cast<size_t>(data_idx) * fragment_size_;
        size_t copy_len = len;
        if (offset + copy_len > file_size_)
        {
            copy_len = file_size_ - offset;
        }
        memcpy(reassembly_buf_ + offset, data, copy_len);

        ESP_LOGD(TAG,
                 "RX frag seq=%u data_idx=%u (%u/%u)",
                 seq,
                 data_idx,
                 fragments_received_,
                 total_fragments_);
    }

    /* Advance expected_seq_ past consecutive received fragments */
    while (expected_seq_ < total_fragments_)
    {
        uint8_t ebi = expected_seq_ / 8;
        uint8_t ebm = 1 << (expected_seq_ % 8);
        if (!(recv_bitmap_[ebi] & ebm))
        {
            break;
        }
        expected_seq_++;
    }

    /*
     * NACK only the gap at the receive window front (bounded by window size)
     * with a per-seq cooldown to avoid re-NACKing within timeout_ms_
     */
    if (seq > expected_seq_)
    {
        uint32_t now = now_ms();
        uint16_t nack_end =
            (expected_seq_ + window_size_ < seq)
                ? static_cast<uint16_t>(expected_seq_ + window_size_)
                : seq;
        for (uint16_t i = expected_seq_; i < nack_end; i++)
        {
            uint8_t ni = i / 8;
            uint8_t nm = 1 << (i % 8);
            if (!(recv_bitmap_[ni] & nm))
            {
                if ((now - nack_sent_ms_[i]) >= timeout_ms_)
                {
                    if (send_cb_)
                    {
                        send_cb_(peer_addr_, PacketType::NACK, i, nullptr, 0);
                    }
                    nack_sent_ms_[i] = now;
                }
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
        heap_caps_free(reassembly_buf_);
        reassembly_buf_ = nullptr;
    }
    if (recv_bitmap_)
    {
        free(recv_bitmap_);
        recv_bitmap_ = nullptr;
    }
    if (nack_sent_ms_)
    {
        free(nack_sent_ms_);
        nack_sent_ms_ = nullptr;
    }
    total_fragments_ = 0;
    fragments_received_ = 0;
    expected_seq_ = 0;
    receiver_active_ = false;
}

} /* namespace flp */
