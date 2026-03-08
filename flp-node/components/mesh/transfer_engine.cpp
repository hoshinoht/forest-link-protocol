// =============================================================================
// transfer_engine.cpp — File transfer state machine
// =============================================================================

#include "transfer_engine.hpp"

#include <cinttypes>
#include <cstring>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"

using namespace flp;

static const char *TAG = "xfer_eng";
static constexpr uint32_t FAST_ARQ_TIMEOUT_MS = 700;
static constexpr uint32_t ELECTION_TIMEOUT_MS = 3000;
static constexpr uint32_t BROADCAST_INITIAL_BACKOFF_MS = 500;

constexpr EventBits_t FLP_EVT_TRANSFER_COMPLETE = BIT1;
constexpr EventBits_t FLP_EVT_EXIT_NODE_ELECTED = BIT2;

void TransferEngine::init(EventGroupHandle_t events,
                          uint16_t my_addr,
                          SendPacketFn send_fn)
{
    events_ = events;
    my_addr_ = my_addr;
    send_fn_ = send_fn;

    for (int i = 0; i < MAX_EXIT_NODES; i++)
    {
        arq_[i].init(ARQ_WINDOW, FAST_ARQ_TIMEOUT_MS);
        arq_[i].set_send_callback(
            [this](uint16_t dst,
                   PacketType type,
                   uint16_t seq,
                   const uint8_t *data,
                   size_t len)
            {
                // Route through mesh — send_fn_ handles header construction,
                // route table lookup, transport selection, and next-hop relay.
                send_fn_(dst, type, data, len, seq);
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
                 sizeof(ack),
                 0);

        // Set up as exit node -- NO PSRAM allocation
        is_exit_node_ = true;
        active_session_id_ = ad.session_id;
        source_addr_ = hdr.src_addr;

        // Set up ACK path: arq_[0] for sending ACKs back to source
        arq_[0].set_peer_addr(hdr.src_addr);

        // Publish complete transfer meta to MQTT (Fix 5)
        if (forward_meta_fn_)
        {
            forward_meta_fn_(ad.session_id,
                             ad.filename,
                             hdr.src_addr,
                             ad.file_size,
                             ad.fragment_count,
                             ad.fragment_size,
                             ad.crc32);
        }

        ESP_LOGI(TAG,
                 "Exit node mode: session=%" PRIu32 " source=0x%04X",
                 active_session_id_,
                 source_addr_);
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
        // Send ACK back to source — routed through mesh
        send_fn_(hdr.src_addr, PacketType::ACK, nullptr, 0, hdr.seq_num);

        // Forward fragment to MQTT
        if (forward_to_mqtt_fn_)
        {
            forward_to_mqtt_fn_(active_session_id_,
                                hdr.seq_num,
                                source_addr_,
                                payload,
                                payload_len,
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
    {
        arq_[idx].handle_nack(seq);
    }
}

int8_t TransferEngine::arq_index_for_peer(uint16_t addr) const
{
    for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
    {
        if (transfer_.exit_nodes[i] == addr)
        {
            return static_cast<int8_t>(i);
        }
    }
    return -1;
}

// -- Start file transfer ------------------------------------------------------

void TransferEngine::start_file_transfer(const char *filename,
                                         const uint8_t *data,
                                         size_t size,
                                         bool has_internet,
                                         bool has_mqtt)
{
    if (transfer_.active)
    {
        ESP_LOGW(TAG, "Transfer already in progress");
        return;
    }

    fec_encoder_.reset();

    // Use payload size that is safe for both LoRa and ESP-NOW so the
    // adaptive selector can choose ESP-NOW fast path without drops.
    size_t frag_payload = ESPNOW_MAX_PAYLOAD;

    transfer_.data = data;
    transfer_.size = size;
    transfer_.fragment_size = static_cast<uint16_t>(frag_payload);
    uint16_t data_frags =
        static_cast<uint16_t>((size + frag_payload - 1) / frag_payload);
    transfer_.next_fragment = 0;
    transfer_.active = true;
    transfer_.session_id = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    strncpy(transfer_.filename, filename, sizeof(transfer_.filename) - 1);
    transfer_.filename[sizeof(transfer_.filename) - 1] = '\0';

    // Local-exit fast path: source node has internet + MQTT, skip mesh entirely
    local_exit_ = (has_internet && has_mqtt && forward_to_mqtt_fn_);
    if (local_exit_)
    {
        // No FEC parity needed — no lossy channel
        transfer_.fragment_count = data_frags;

        ESP_LOGI(TAG,
                 "Local-exit transfer: %s (%zu bytes, %u fragments, "
                 "session=%" PRIu32 ")",
                 filename,
                 size,
                 transfer_.fragment_count,
                 transfer_.session_id);

        // Publish transfer meta directly
        uint32_t crc = esp_rom_crc32_le(0, data, size);
        if (forward_meta_fn_)
        {
            forward_meta_fn_(transfer_.session_id,
                             filename,
                             my_addr_,
                             static_cast<uint32_t>(size),
                             transfer_.fragment_count,
                             transfer_.fragment_size,
                             crc);
        }
        return; // transfer_tick() will publish fragments
    }

    // Normal mesh path — include FEC parity fragments
    uint16_t parity_frags = static_cast<uint16_t>(
        (data_frags + FEC_GROUP_SIZE - 1) / FEC_GROUP_SIZE);
    transfer_.fragment_count = static_cast<uint16_t>(data_frags + parity_frags);

    ESP_LOGI(
        TAG,
        "Starting file transfer: %s (%zu bytes, %u fragments, session=%" PRIu32
        ")",
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
    ad.crc32 = esp_rom_crc32_le(0, data, size);
    strncpy(ad.filename, filename, sizeof(ad.filename) - 1);

    // Start election
    candidate_count_ = 0;
    election_active_ = true;
    election_start_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    // Fix 1: Send to EXIT_ANY_ADDR so relays forward toward exit nodes.
    // BROADCAST_ADDR is excluded from forwarding in process_slab, meaning
    // exit nodes >1 hop away would never see the ad.
    send_broadcast_with_retry(PacketType::TRANSFER_AD,
                              reinterpret_cast<const uint8_t *>(&ad),
                              sizeof(ad),
                              EXIT_ANY_ADDR);
}

// -- Periodic tick ------------------------------------------------------------

void TransferEngine::tick(uint32_t now_ms)
{
    // ARQ timeout retransmits — tick ALL instances
    for (uint8_t i = 0; i < MAX_EXIT_NODES; i++)
    {
        arq_[i].tick();
    }

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
    if (!election_active_ ||
        (now_ms - election_start_ms_ <= ELECTION_TIMEOUT_MS))
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
        {
            arq_[i].reset_sender();
        }
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

    // Local-exit fast path: publish fragments directly to MQTT, no ARQ
    if (local_exit_)
    {
        // Pace: publish up to 4 fragments per tick to avoid starving other tasks
        constexpr uint8_t kLocalExitBatchSize = 2;
        uint8_t sent = 0;
        while (transfer_.next_fragment < transfer_.fragment_count &&
               sent < kLocalExitBatchSize)
        {
            uint16_t seq = transfer_.next_fragment;
            size_t offset =
                static_cast<size_t>(seq) * transfer_.fragment_size;
            size_t remain = transfer_.size - offset;
            size_t frag_len = (remain < transfer_.fragment_size)
                                  ? remain
                                  : transfer_.fragment_size;

            forward_to_mqtt_fn_(transfer_.session_id,
                                seq,
                                my_addr_,
                                transfer_.data + offset,
                                frag_len,
                                transfer_.filename);
            transfer_.next_fragment++;
            sent++;
        }

        if (transfer_.next_fragment >= transfer_.fragment_count)
        {
            ESP_LOGI(TAG,
                     "Local-exit transfer complete: %s",
                     transfer_.filename);
            transfer_.active = false;
            transfer_.data = nullptr;
            local_exit_ = false;
            xEventGroupSetBits(events_, FLP_EVT_TRANSFER_COMPLETE);
        }
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
        {
            alive_count++;
        }
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
    // Sequence layout: every (FEC_GROUP_SIZE+1)th seq is a parity slot
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
                uint8_t try_idx = (arq_idx + j) % transfer_.exit_node_count;
                if (transfer_.exit_node_alive[try_idx])
                {
                    arq_idx = try_idx;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                break;
            }
        }
        if (arq_[arq_idx].sender_window_full())
        {
            break;
        }

        uint16_t seq = transfer_.next_fragment;
        bool is_parity_slot = (seq % (FEC_GROUP_SIZE + 1) == FEC_GROUP_SIZE);

        if (is_parity_slot)
        {
            // Send parity fragment
            int ret = arq_[arq_idx].send_fragment(
                seq, fec_encoder_.parity_data(), fec_encoder_.parity_len());
            if (ret < 0)
            {
                break;
            }
            fec_encoder_.reset();
        }
        else
        {
            // Map seq to data index (skip parity slots)
            uint16_t group = seq / (FEC_GROUP_SIZE + 1);
            uint16_t idx_in_group = seq % (FEC_GROUP_SIZE + 1);
            uint16_t data_idx = group * FEC_GROUP_SIZE + idx_in_group;

            size_t offset =
                static_cast<size_t>(data_idx) * transfer_.fragment_size;
            if (offset >= transfer_.size)
            {
                // Past end of file data — skip (tail parity will follow)
                transfer_.next_fragment++;
                continue;
            }
            size_t remain = transfer_.size - offset;
            size_t frag_len = (remain < transfer_.fragment_size)
                                  ? remain
                                  : transfer_.fragment_size;

            int ret = arq_[arq_idx].send_fragment(
                seq, transfer_.data + offset, frag_len);
            if (ret < 0)
            {
                break;
            }

            // Ingest into FEC encoder
            fec_encoder_.ingest(transfer_.data + offset, frag_len);

            // If this was the last data fragment and the group is not
            // complete, the next seq should be a parity slot emitted by
            // the tail-flush logic below
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
            {
                continue;
            }
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
            {
                arq_[i].reset_sender();
            }
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
        {
            continue;
        }

        // Check if this ARQ has unacked in-flight fragments
        bool has_pending = arq_[i].get_base_seq() < arq_[i].get_next_seq();
        if (!has_pending)
        {
            continue;
        }

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
            {
                first_alive = i;
            }
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
        size_t offset = static_cast<size_t>(seq) * transfer_.fragment_size;
        size_t remain = transfer_.size - offset;
        size_t frag_len = (remain < transfer_.fragment_size)
                              ? remain
                              : transfer_.fragment_size;

        if (!arq_[target].sender_window_full())
        {
            arq_[target].send_fragment(seq, transfer_.data + offset, frag_len);
            redistributed++;
        }
        else if (redist_count_ <
                 sizeof(redist_pending_) / sizeof(redist_pending_[0]))
        {
            // Queue for retry on next tick
            redist_pending_[redist_count_++] = seq;
        }
        else
        {
            ESP_LOGE(
                TAG, "Redistribution queue full, fragment seq=%u lost", seq);
        }
    }

    ESP_LOGI(TAG,
             "Redistributed %u fragments from dead exit 0x%04X to %u survivors"
             " (%u pending)",
             redistributed,
             transfer_.exit_nodes[dead_idx],
             alive_count,
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

    send_fn_(broadcast_retry_.dst_addr,
             broadcast_retry_.type,
             broadcast_retry_.payload,
             broadcast_retry_.payload_len,
             0);
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
                                               uint16_t dst_addr,
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
    broadcast_retry_.dst_addr = dst_addr;
    broadcast_retry_.max_retries = max_retries;
    broadcast_retry_.attempt = 0;
    broadcast_retry_.backoff_ms = BROADCAST_INITIAL_BACKOFF_MS;
    broadcast_retry_.next_send_ms = 0;
    broadcast_retry_.active = true;
}
