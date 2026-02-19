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

    for (int i = 0; i < MAX_EXIT_NODES; i++)
    {
        arq_[i].init(ARQ_WINDOW, ARQ_TIMEOUT);
        arq_[i].set_send_callback(
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
    }

    ESP_LOGI(TAG, "TransferEngine initialized");
}

// -- Packet handlers ----------------------------------------------------------

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
             "Transfer ad from 0x%04X: file=%s size=%" PRIu32
             " frags=%u session=%" PRIu32,
             hdr.src_addr,
             ad.filename,
             ad.file_size,
             ad.fragment_count,
             ad.session_id);

    // Store the filename so handle_data can use it for MQTT publish
    strncpy(transfer_.filename, ad.filename, sizeof(transfer_.filename) - 1);
    transfer_.filename[sizeof(transfer_.filename) - 1] = '\0';

    // If we have internet, respond as exit node candidate
    if (has_internet)
    {
        TransferAckPayload ack = {};
        ack.session_id = ad.session_id;
        ack.exit_node_addr = my_addr_;
        ack.hops_to_gw = 0;
        ack.rssi_to_gw = 0;

        send_fn_(hdr.src_addr,
                 PacketType::TRANSFER_ACK,
                 reinterpret_cast<const uint8_t *>(&ack),
                 sizeof(ack));

        // Set up as exit node -- NO PSRAM allocation
        is_exit_node_ = true;
        active_session_id_ = ad.session_id;
        source_addr_ = hdr.src_addr;

        // Set up ACK path: arq_[0] for sending ACKs back to source
        arq_[0].set_peer_addr(hdr.src_addr);

        ESP_LOGI(TAG, "Exit node mode: session=%" PRIu32 " source=0x%04X",
                 active_session_id_, source_addr_);
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
    if (is_exit_node_)
    {
        // Send ACK back to source
        {
            uint8_t buf[MAX_MTU];
            PacketHeader ack_hdr = {};
            ack_hdr.set_ver_type(PROTOCOL_VERSION, PacketType::ACK);
            ack_hdr.src_addr = my_addr_;
            ack_hdr.dst_addr = hdr.src_addr;
            ack_hdr.set_ttl_hops(DEFAULT_TTL, 0);
            ack_hdr.seq_num = hdr.seq_num;
            memcpy(buf, &ack_hdr, PACKET_HEADER_SIZE);
            Transport t = select_fn_(-90, 0xFF, PACKET_HEADER_SIZE);
            send_raw_fn_(t, buf, PACKET_HEADER_SIZE, hdr.src_addr);
        }

        // Forward fragment to MQTT
        if (forward_to_mqtt_fn_)
        {
            forward_to_mqtt_fn_(active_session_id_, hdr.seq_num,
                               source_addr_, payload, payload_len,
                               transfer_.filename);
        }
        return;
    }

    // Legacy receiver path (non-exit-node, for backward compat)
    arq_[0].set_peer_addr(hdr.src_addr);
    arq_[0].receive_fragment(hdr.seq_num, payload, payload_len);
}

void TransferEngine::handle_ack(uint16_t seq, uint16_t from_addr)
{
    int8_t idx = arq_index_for_peer(from_addr);
    if (idx >= 0)
    {
        arq_[idx].handle_ack(seq);
        transfer_.last_ack_ms[idx] =
            static_cast<uint32_t>(esp_timer_get_time() / 1000);
    }
}

void TransferEngine::handle_nack(uint16_t seq, uint16_t from_addr)
{
    int8_t idx = arq_index_for_peer(from_addr);
    if (idx >= 0)
        arq_[idx].handle_nack(seq);
}

int8_t TransferEngine::arq_index_for_peer(uint16_t addr) const
{
    for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
    {
        if (transfer_.exit_nodes[i] == addr)
            return static_cast<int8_t>(i);
    }
    return -1;
}

// -- Start file transfer ------------------------------------------------------

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
    transfer_.session_id = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    strncpy(transfer_.filename, filename, sizeof(transfer_.filename) - 1);
    transfer_.filename[sizeof(transfer_.filename) - 1] = '\0';

    ESP_LOGI(TAG,
             "Starting file transfer: %s (%zu bytes, %u fragments, session=%" PRIu32 ")",
             filename,
             size,
             transfer_.fragment_count,
             transfer_.session_id);

    // Broadcast TRANSFER_AD with retry
    TransferAdPayload ad = {};
    ad.session_id = transfer_.session_id;
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

// -- Periodic tick ------------------------------------------------------------

void TransferEngine::tick(uint32_t now_ms)
{
    // ARQ timeout retransmits — tick ALL instances
    for (uint8_t i = 0; i < MAX_EXIT_NODES; i++)
        arq_[i].tick();

    // Broadcast retry
    broadcast_retry_tick(now_ms);

    // Election timeout
    election_timeout_tick(now_ms);

    // Check for dead exit nodes and redistribute
    exit_node_health_tick(now_ms);

    // Feed fragments into ARQ window
    transfer_tick();
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
        for (uint8_t i = 0; i < MAX_EXIT_NODES; i++)
            arq_[i].reset_sender();
    }
    else
    {
        // Use ALL candidates as exit nodes
        transfer_.exit_node_count = candidate_count_;
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        for (uint8_t i = 0; i < candidate_count_; i++)
        {
            transfer_.exit_nodes[i] = candidates_[i].addr;
            transfer_.exit_node_alive[i] = true;
            transfer_.last_ack_ms[i] = now;
            arq_[i].reset_sender();
            arq_[i].set_peer_addr(candidates_[i].addr);
        }
        ESP_LOGI(TAG, "Elected %u exit nodes", transfer_.exit_node_count);

        xEventGroupSetBits(events_, FLP_EVT_EXIT_NODE_ELECTED);
        transfer_.next_fragment = 0;
    }
}

void TransferEngine::transfer_tick()
{
    if (!transfer_.active || election_active_)
    {
        return;
    }
    if (transfer_.exit_node_count == 0)
    {
        return;
    }

    // Count alive exit nodes
    uint8_t alive_count = 0;
    for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
    {
        if (transfer_.exit_node_alive[i])
            alive_count++;
    }
    if (alive_count == 0)
    {
        ESP_LOGE(TAG, "All exit nodes dead, aborting transfer");
        transfer_.active = false;
        transfer_.data = nullptr;
        return;
    }

    // Drain pending redistribution queue first
    if (redist_count_ > 0)
    {
        uint8_t new_count = 0;
        for (uint8_t r = 0; r < redist_count_; r++)
        {
            uint16_t seq = redist_pending_[r];
            // Pick a surviving ARQ
            uint8_t target = 0;
            uint8_t rr = seq % alive_count;
            uint8_t cnt = 0;
            for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
            {
                if (transfer_.exit_node_alive[i])
                {
                    if (cnt == rr)
                    {
                        target = i;
                        break;
                    }
                    cnt++;
                }
            }

            if (!arq_[target].sender_window_full())
            {
                size_t offset =
                    static_cast<size_t>(seq) * transfer_.fragment_size;
                size_t remain = transfer_.size - offset;
                size_t frag_len = (remain < transfer_.fragment_size)
                                      ? remain
                                      : transfer_.fragment_size;
                arq_[target].send_fragment(
                    seq, transfer_.data + offset, frag_len);
            }
            else
            {
                // Still can't fit, keep in queue
                redist_pending_[new_count++] = seq;
            }
        }
        redist_count_ = new_count;
    }

    // Round-robin fragment assignment across alive exit nodes
    while (transfer_.next_fragment < transfer_.fragment_count)
    {
        // Find next alive exit node for this fragment
        uint8_t arq_idx = transfer_.next_fragment % transfer_.exit_node_count;
        if (!transfer_.exit_node_alive[arq_idx])
        {
            // Find next alive node
            bool found = false;
            for (uint8_t j = 1; j < transfer_.exit_node_count; j++)
            {
                uint8_t try_idx =
                    (arq_idx + j) % transfer_.exit_node_count;
                if (transfer_.exit_node_alive[try_idx])
                {
                    arq_idx = try_idx;
                    found = true;
                    break;
                }
            }
            if (!found)
                break;
        }
        if (arq_[arq_idx].sender_window_full())
            break;

        size_t offset = static_cast<size_t>(transfer_.next_fragment) *
                        transfer_.fragment_size;
        size_t remain = transfer_.size - offset;
        size_t frag_len = (remain < transfer_.fragment_size)
                              ? remain
                              : transfer_.fragment_size;

        int ret = arq_[arq_idx].send_fragment(
            transfer_.next_fragment, transfer_.data + offset, frag_len);
        if (ret < 0)
        {
            break;
        }

        transfer_.next_fragment++;
    }

    // Check if all fragments sent and acknowledged (only check alive exits)
    if (transfer_.next_fragment >= transfer_.fragment_count)
    {
        bool all_done = true;
        for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
        {
            if (!transfer_.exit_node_alive[i])
                continue;
            if (arq_[i].get_base_seq() < transfer_.fragment_count)
            {
                all_done = false;
                break;
            }
        }
        if (all_done)
        {
            ESP_LOGI(TAG, "File transfer complete: %s", transfer_.filename);
            transfer_.active = false;
            transfer_.data = nullptr;
            for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
                arq_[i].reset_sender();
            xEventGroupSetBits(events_, FLP_EVT_TRANSFER_COMPLETE);
        }
    }
}

void TransferEngine::exit_node_health_tick(uint32_t now_ms)
{
    if (!transfer_.active || election_active_ || transfer_.exit_node_count <= 1)
    {
        return;
    }

    for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
    {
        if (!transfer_.exit_node_alive[i])
            continue;

        // Check if this ARQ has unacked in-flight fragments
        bool has_pending =
            arq_[i].get_base_seq() < arq_[i].get_next_seq();
        if (!has_pending)
            continue;

        if ((now_ms - transfer_.last_ack_ms[i]) > EXIT_NODE_TIMEOUT_MS)
        {
            ESP_LOGW(TAG,
                     "Exit node 0x%04X timed out (no ACK for %" PRIu32
                     "ms), redistributing",
                     transfer_.exit_nodes[i],
                     now_ms - transfer_.last_ack_ms[i]);
            redistribute_dead_exit(i);
        }
    }
}

void TransferEngine::redistribute_dead_exit(uint8_t dead_idx)
{
    transfer_.exit_node_alive[dead_idx] = false;

    // Collect unacked fragment sequences from the dead ARQ's window
    uint16_t base = arq_[dead_idx].get_base_seq();
    uint16_t next = arq_[dead_idx].get_next_seq();
    arq_[dead_idx].reset_sender();

    // Count surviving exit nodes
    uint8_t alive_count = 0;
    uint8_t first_alive = 0;
    for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
    {
        if (transfer_.exit_node_alive[i])
        {
            if (alive_count == 0)
                first_alive = i;
            alive_count++;
        }
    }

    if (alive_count == 0)
    {
        ESP_LOGE(TAG, "No surviving exit nodes, transfer will fail");
        return;
    }

    // Re-enqueue unacked fragments by resending them through surviving ARQs.
    // The fragments between base and next were in the dead ARQ's window but
    // never ACKed. We re-send them via surviving exit nodes.
    uint16_t redistributed = 0;
    for (uint16_t seq = base; seq < next; seq++)
    {
        // Pick a surviving ARQ via round-robin among alive nodes
        uint8_t target = first_alive;
        uint8_t rr = seq % alive_count;
        uint8_t count = 0;
        for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
        {
            if (transfer_.exit_node_alive[i])
            {
                if (count == rr)
                {
                    target = i;
                    break;
                }
                count++;
            }
        }

        // Compute fragment data offset
        size_t offset =
            static_cast<size_t>(seq) * transfer_.fragment_size;
        size_t remain = transfer_.size - offset;
        size_t frag_len = (remain < transfer_.fragment_size)
                              ? remain
                              : transfer_.fragment_size;

        if (!arq_[target].sender_window_full())
        {
            arq_[target].send_fragment(
                seq, transfer_.data + offset, frag_len);
            redistributed++;
        }
        else if (redist_count_ < sizeof(redist_pending_) /
                                      sizeof(redist_pending_[0]))
        {
            // Queue for retry on next tick
            redist_pending_[redist_count_++] = seq;
        }
        else
        {
            ESP_LOGE(TAG, "Redistribution queue full, fragment seq=%u lost",
                     seq);
        }
    }

    ESP_LOGI(TAG,
             "Redistributed %u fragments from dead exit 0x%04X to %u survivors"
             " (%u pending)",
             redistributed, transfer_.exit_nodes[dead_idx], alive_count,
             redist_count_);
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
