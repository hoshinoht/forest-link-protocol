// =============================================================================
// transfer_engine.cpp — File transfer state machine
// =============================================================================

#include "transfer_engine.hpp"

#include <cinttypes>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"

using namespace flp;

static const char *TAG = "xfer_eng";

#define FLP_EVT_TRANSFER_COMPLETE BIT1
#define FLP_EVT_EXIT_NODE_ELECTED BIT2

void TransferEngine::init(EventGroupHandle_t events,
                          uint16_t my_addr,
                          SendPacketFn send_fn,
                          SendRawFn send_raw_fn,
                          SelectTransportFn select_fn)
{
    events_ = events;
    my_addr_ = my_addr;
    send_fn_ = send_fn;
    send_raw_fn_ = send_raw_fn;
    select_fn_ = select_fn;

    arq_.init(ARQ_WINDOW, ARQ_TIMEOUT);
    arq_.set_send_callback(
        [this](uint16_t dst,
               PacketType type,
               uint16_t seq,
               const uint8_t *data,
               size_t len)
        {
            if (PACKET_HEADER_SIZE + len > MAX_MTU)
            {
                ESP_LOGW(TAG,
                         "ARQ send: payload %zu exceeds MAX_MTU, dropping",
                         len);
                return;
            }

            uint8_t buf[MAX_MTU];
            PacketHeader hdr = {};
            hdr.set_ver_type(PROTOCOL_VERSION, type);
            hdr.src_addr = my_addr_;
            hdr.dst_addr = dst;
            hdr.set_ttl_hops(DEFAULT_TTL, 0);
            hdr.seq_num = seq;

            memcpy(buf, &hdr, PACKET_HEADER_SIZE);
            if (data && len > 0)
            {
                memcpy(buf + PACKET_HEADER_SIZE, data, len);
            }

            size_t total = PACKET_HEADER_SIZE + len;
            Transport t = select_fn_(-90, 0xFF, total);
            send_raw_fn_(t, buf, total, dst);
        });

    ESP_LOGI(TAG, "TransferEngine initialized");
}

// ── Packet handlers ──────────────────────────────────────────────────────────

void TransferEngine::handle_transfer_ad(const PacketHeader &hdr,
                                        const uint8_t *payload,
                                        size_t payload_len,
                                        bool has_internet)
{
    if (payload_len < sizeof(TransferAdPayload))
    {
        return;
    }

    TransferAdPayload ad;
    memcpy(&ad, payload, sizeof(ad));

    ESP_LOGI(TAG,
             "Transfer ad from 0x%04X: file=%s size=%" PRIu32 " frags=%u",
             hdr.src_addr,
             ad.filename,
             ad.file_size,
             ad.fragment_count);

    // Store the filename so handle_data can use it for MQTT publish
    strncpy(transfer_.filename, ad.filename, sizeof(transfer_.filename) - 1);
    transfer_.filename[sizeof(transfer_.filename) - 1] = '\0';

    // If we have internet, respond as exit node candidate
    if (has_internet)
    {
        TransferAckPayload ack = {};
        ack.exit_node_addr = my_addr_;
        ack.hops_to_gw = 0;
        ack.rssi_to_gw = 0;

        send_fn_(hdr.src_addr,
                 PacketType::TRANSFER_ACK,
                 reinterpret_cast<const uint8_t *>(&ack),
                 sizeof(ack));

        // Item 6: Track when receiver was initialized for cleanup timeout
        arq_.init_receiver(ad.fragment_count, ad.fragment_size, ad.file_size);
        arq_.set_peer_addr(hdr.src_addr);
        receiver_init_ms_ =
            static_cast<uint32_t>(esp_timer_get_time() / 1000);
        receiver_waiting_ = true;

        ESP_LOGI(
            TAG, "Responded as exit node candidate to 0x%04X", hdr.src_addr);
    }
}

void TransferEngine::handle_transfer_ack(const PacketHeader &hdr,
                                         const uint8_t *payload,
                                         size_t payload_len)
{
    if (payload_len < sizeof(TransferAckPayload))
    {
        return;
    }
    if (!election_active_)
    {
        return;
    }

    TransferAckPayload ack;
    memcpy(&ack, payload, sizeof(ack));

    if (candidate_count_ < candidates_.size())
    {
        candidates_[candidate_count_] = {
            ack.exit_node_addr, ack.rssi_to_gw, ack.hops_to_gw};
        candidate_count_++;
        ESP_LOGI(TAG,
                 "Exit candidate #%u: 0x%04X hops=%u rssi=%d",
                 candidate_count_,
                 ack.exit_node_addr,
                 ack.hops_to_gw,
                 ack.rssi_to_gw);
    }
}

void TransferEngine::handle_data(const PacketHeader &hdr,
                                 const uint8_t *payload,
                                 size_t payload_len)
{
    arq_.set_peer_addr(hdr.src_addr);
    arq_.receive_fragment(hdr.seq_num, payload, payload_len);

    // Cancel cleanup timeout — data is arriving
    receiver_waiting_ = false;
}

void TransferEngine::handle_ack(uint16_t seq)
{
    arq_.handle_ack(seq);
}

void TransferEngine::handle_nack(uint16_t seq)
{
    arq_.handle_nack(seq);
}

// ── Start file transfer ──────────────────────────────────────────────────────

void TransferEngine::start_file_transfer(const char *filename,
                                         const uint8_t *data,
                                         size_t size,
                                         Transport preferred_transport)
{
    if (transfer_.active)
    {
        ESP_LOGW(TAG, "Transfer already in progress");
        return;
    }

    size_t frag_payload =
        (preferred_transport == Transport::BLE) ? BLE_MAX_PAYLOAD
                                                : LORA_MAX_PAYLOAD;

    transfer_.data = data;
    transfer_.size = size;
    transfer_.fragment_size = static_cast<uint16_t>(frag_payload);
    transfer_.fragment_count =
        static_cast<uint16_t>((size + frag_payload - 1) / frag_payload);
    transfer_.next_fragment = 0;
    transfer_.active = true;
    strncpy(transfer_.filename, filename, sizeof(transfer_.filename) - 1);
    transfer_.filename[sizeof(transfer_.filename) - 1] = '\0';

    ESP_LOGI(TAG,
             "Starting file transfer: %s (%zu bytes, %u fragments)",
             filename,
             size,
             transfer_.fragment_count);

    // Broadcast TRANSFER_AD with retry
    TransferAdPayload ad = {};
    ad.file_size = static_cast<uint32_t>(size);
    ad.fragment_count = transfer_.fragment_count;
    ad.fragment_size = transfer_.fragment_size;
    strncpy(ad.filename, filename, sizeof(ad.filename) - 1);

    // Start election
    candidate_count_ = 0;
    election_active_ = true;
    election_start_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    send_broadcast_with_retry(PacketType::TRANSFER_AD,
                              reinterpret_cast<const uint8_t *>(&ad),
                              sizeof(ad));
}

// ── Periodic tick ────────────────────────────────────────────────────────────

void TransferEngine::tick(uint32_t now_ms)
{
    // ARQ timeout retransmits
    arq_.tick();

    // Broadcast retry
    broadcast_retry_tick(now_ms);

    // Election timeout
    election_timeout_tick(now_ms);

    // Feed fragments into ARQ window
    transfer_tick();

    // Item 6: Cleanup PSRAM if no DATA arrives within 10s of responding
    if (receiver_waiting_ && (now_ms - receiver_init_ms_ > 10000))
    {
        ESP_LOGW(TAG, "No DATA received after election response, cleaning up");
        arq_.cleanup_receiver();
        receiver_waiting_ = false;
    }
}

void TransferEngine::election_timeout_tick(uint32_t now_ms)
{
    if (!election_active_ || (now_ms - election_start_ms_ <= 3000))
    {
        return;
    }

    election_active_ = false;

    if (candidate_count_ == 0)
    {
        ESP_LOGW(TAG, "Election timeout: no exit node candidates");
        transfer_.active = false;
        transfer_.data = nullptr;
        arq_.reset_sender();
    }
    else
    {
        // Pick best: lowest hops_to_gw, then best rssi_to_gw as tiebreaker
        uint8_t best_idx = 0;
        for (uint8_t i = 1; i < candidate_count_; i++)
        {
            if (candidates_[i].hops_to_gw <
                    candidates_[best_idx].hops_to_gw ||
                (candidates_[i].hops_to_gw ==
                     candidates_[best_idx].hops_to_gw &&
                 candidates_[i].rssi_to_gw >
                     candidates_[best_idx].rssi_to_gw))
            {
                best_idx = i;
            }
        }

        transfer_.exit_node = candidates_[best_idx].addr;
        ESP_LOGI(TAG,
                 "Elected exit node: 0x%04X (hops=%u rssi=%d)",
                 transfer_.exit_node,
                 candidates_[best_idx].hops_to_gw,
                 candidates_[best_idx].rssi_to_gw);

        xEventGroupSetBits(events_, FLP_EVT_EXIT_NODE_ELECTED);

        arq_.reset_sender();
        arq_.set_peer_addr(transfer_.exit_node);
        transfer_.next_fragment = 0;
    }
}

void TransferEngine::transfer_tick()
{
    if (!transfer_.active || election_active_)
    {
        return;
    }
    if (transfer_.exit_node == 0)
    {
        return;
    }

    while (transfer_.next_fragment < transfer_.fragment_count &&
           !arq_.sender_window_full())
    {
        size_t offset = static_cast<size_t>(transfer_.next_fragment) *
                        transfer_.fragment_size;
        size_t remain = transfer_.size - offset;
        size_t frag_len = (remain < transfer_.fragment_size)
                              ? remain
                              : transfer_.fragment_size;

        int ret = arq_.send_fragment(
            transfer_.next_fragment, transfer_.data + offset, frag_len);
        if (ret < 0)
        {
            break;
        }

        transfer_.next_fragment++;
    }

    // Check if all fragments sent and acknowledged
    if (transfer_.next_fragment >= transfer_.fragment_count &&
        arq_.get_base_seq() >= transfer_.fragment_count)
    {
        ESP_LOGI(TAG, "File transfer complete: %s", transfer_.filename);
        transfer_.active = false;
        transfer_.data = nullptr;
        arq_.reset_sender();
        xEventGroupSetBits(events_, FLP_EVT_TRANSFER_COMPLETE);
    }
}

void TransferEngine::broadcast_retry_tick(uint32_t now_ms)
{
    if (!broadcast_retry_.active)
    {
        return;
    }
    if (now_ms < broadcast_retry_.next_send_ms)
    {
        return;
    }

    send_fn_(BROADCAST_ADDR,
             broadcast_retry_.type,
             broadcast_retry_.payload,
             broadcast_retry_.payload_len);
    broadcast_retry_.attempt++;

    if (broadcast_retry_.attempt >= broadcast_retry_.max_retries)
    {
        broadcast_retry_.active = false;
    }
    else
    {
        ESP_LOGD(TAG,
                 "Broadcast retry %u/%u, backoff %" PRIu32 "ms",
                 broadcast_retry_.attempt,
                 broadcast_retry_.max_retries,
                 broadcast_retry_.backoff_ms);
        broadcast_retry_.next_send_ms = now_ms + broadcast_retry_.backoff_ms;
        broadcast_retry_.backoff_ms *= 2;
    }
}

void TransferEngine::send_broadcast_with_retry(PacketType type,
                                               const uint8_t *payload,
                                               size_t payload_len,
                                               uint8_t max_retries)
{
    if (payload_len > sizeof(broadcast_retry_.payload))
    {
        ESP_LOGE(TAG, "Broadcast payload too large: %zu", payload_len);
        return;
    }

    memcpy(broadcast_retry_.payload, payload, payload_len);
    broadcast_retry_.payload_len = payload_len;
    broadcast_retry_.type = type;
    broadcast_retry_.max_retries = max_retries;
    broadcast_retry_.attempt = 0;
    broadcast_retry_.backoff_ms = 500;
    broadcast_retry_.next_send_ms = 0;
    broadcast_retry_.active = true;
}
