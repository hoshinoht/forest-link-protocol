/*
 * =============================================================================
 * transfer_engine.cpp — File transfer state machine
 * =============================================================================
 */

#include "transfer_engine.hpp"

#include <cinttypes>
#include <cstring>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"

using namespace flp;

static const char *TAG = "xfer_eng";
static constexpr uint32_t FAST_ARQ_TIMEOUT_MS = 700;
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
                send_fn_(dst, type, data, len, seq);
            });
    }

    ESP_LOGI(TAG, "TransferEngine initialized");
}

/* -- Packet handlers ---------------------------------------------------------- */

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

    uint16_t frag_size = static_cast<uint16_t>(ad.fragment_size_d8) * 8;
    uint16_t frag_count = (frag_size > 0)
        ? static_cast<uint16_t>((ad.file_size + frag_size - 1) / frag_size)
        : 0;

    ESP_LOGI(TAG,
             "Transfer ad from 0x%04X: size=%" PRIu32
             " frag_size=%u frags=%u session=%u",
             hdr.src_addr,
             ad.file_size,
             frag_size,
             frag_count,
             ad.session_id);

    /* If we have internet, respond as exit node candidate */
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

        /* Set up as exit node -- NO PSRAM allocation */
        is_exit_node_ = true;
        active_session_id_ = ad.session_id;
        source_addr_ = hdr.src_addr;

        /* Set up ACK path: arq_[0] for sending ACKs back to source */
        arq_[0].set_peer_addr(hdr.src_addr);

        /*
         * Defer meta publication until fragment 0 arrives with filename.
         * Store ad info for later.
         */
        pending_meta_.file_size = ad.file_size;
        pending_meta_.fragment_count = frag_count;
        pending_meta_.fragment_size = frag_size;
        pending_meta_.crc16 = ad.crc16;
        pending_meta_.waiting = true;
        transfer_.filename[0] = '\0'; /* will be filled by frag 0 */

        ESP_LOGI(TAG,
                 "Exit node mode: session=%u source=0x%04X (awaiting filename)",
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
        /* Send ACK back to source — routed through mesh */
        send_fn_(hdr.src_addr, PacketType::ACK, nullptr, 0, hdr.seq_num);

        const uint8_t *fwd_data = payload;
        size_t fwd_len = payload_len;

        /*
         * Fragment 0 carries filename prefix: [len:1][filename:N][data...]
         * Extract filename and publish deferred meta.
         */
        if (hdr.seq_num == 0 && pending_meta_.waiting && payload_len >= 2)
        {
            uint8_t name_len = payload[0];
            if (name_len > 0 && (1U + name_len) <= payload_len &&
                name_len < sizeof(transfer_.filename))
            {
                memcpy(transfer_.filename, payload + 1, name_len);
                transfer_.filename[name_len] = '\0';

                /* Advance past filename prefix for MQTT forwarding */
                fwd_data = payload + 1 + name_len;
                fwd_len = payload_len - 1 - name_len;

                /* Now publish the deferred transfer meta */
                if (forward_meta_fn_)
                {
                    forward_meta_fn_(active_session_id_,
                                     transfer_.filename,
                                     source_addr_,
                                     pending_meta_.file_size,
                                     pending_meta_.fragment_count,
                                     pending_meta_.fragment_size,
                                     static_cast<uint32_t>(pending_meta_.crc16));
                }
                pending_meta_.waiting = false;

                ESP_LOGI(TAG, "Extracted filename from frag 0: %s",
                         transfer_.filename);
            }
        }

        /* Forward fragment to MQTT */
        if (forward_to_mqtt_fn_)
        {
            forward_to_mqtt_fn_(active_session_id_,
                                hdr.seq_num,
                                source_addr_,
                                fwd_data,
                                fwd_len,
                                transfer_.filename);
        }
        return;
    }

    /* Legacy receiver path (non-exit-node, for backward compat) */
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

/* -- Start file transfer ------------------------------------------------------ */

ReadChunkFn TransferEngine::make_buffer_reader(const uint8_t *data, size_t size)
{
    return [data, size](uint8_t *buf, size_t offset, size_t len) -> size_t
    {
        if (offset >= size) { return 0; }
        size_t avail = size - offset;
        if (len > avail) { len = avail; }
        memcpy(buf, data + offset, len);
        return len;
    };
}

void TransferEngine::start_file_transfer(const char *filename,
                                         size_t size,
                                         ReadChunkFn read_chunk,
                                         bool has_internet,
                                         bool has_mqtt,
                                         uint8_t hops_to_internet)
{
    if (transfer_.active)
    {
        ESP_LOGW(TAG, "Transfer already in progress");
        return;
    }

    fec_encoder_.reset();

    /*
     * Align fragment size to multiples of 8 for compact d8 encoding.
     * ESPNOW_MAX_PAYLOAD = 242, aligned down to 240.
     */
    size_t frag_payload = (ESPNOW_MAX_PAYLOAD / 8) * 8;

    transfer_.read_chunk = read_chunk;
    transfer_.size = size;
    transfer_.fragment_size = static_cast<uint16_t>(frag_payload);
    uint16_t data_frags =
        static_cast<uint16_t>((size + frag_payload - 1) / frag_payload);
    transfer_.next_fragment = 0;
    transfer_.active = true;
    transfer_.session_id =
        static_cast<uint16_t>(esp_timer_get_time() / 1000);
    strncpy(transfer_.filename, filename, sizeof(transfer_.filename) - 1);
    transfer_.filename[sizeof(transfer_.filename) - 1] = '\0';

    /* Compute CRC32 incrementally via chunk reads */
    uint32_t crc = 0;
    {
        uint8_t crc_buf[512];
        size_t off = 0;
        while (off < size)
        {
            size_t chunk = (size - off < sizeof(crc_buf))
                               ? (size - off)
                               : sizeof(crc_buf);
            size_t got = transfer_.read_chunk(crc_buf, off, chunk);
            if (got == 0) { break; }
            crc = esp_rom_crc32_le(crc, crc_buf, got);
            off += got;
        }
    }

    /* Local-exit fast path: source node has internet + MQTT, skip mesh entirely */
    local_exit_ = (has_internet && has_mqtt && forward_to_mqtt_fn_);
    if (local_exit_)
    {
        /* No FEC parity needed — no lossy channel */
        transfer_.fragment_count = data_frags;

        ESP_LOGI(TAG,
                 "Local-exit transfer: %s (%zu bytes, %u fragments, "
                 "session=%u)",
                 filename,
                 size,
                 transfer_.fragment_count,
                 transfer_.session_id);

        /* Initialize cloud selective-repeat ARQ state */
        memset(cloud_ack_bitmap_, 0, sizeof(cloud_ack_bitmap_));
        cloud_base_seq_ = 0;
        cloud_next_send_ = 0;
        cloud_retx_count_ = 0;

        /* Publish transfer meta directly */
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
        return; /* transfer_tick() will publish fragments */
    }

    /* Normal mesh path — include FEC parity fragments */
    uint16_t parity_frags = static_cast<uint16_t>(
        (data_frags + FEC_GROUP_SIZE - 1) / FEC_GROUP_SIZE);
    transfer_.fragment_count = static_cast<uint16_t>(data_frags + parity_frags);

    ESP_LOGI(
        TAG,
        "Starting file transfer: %s (%zu bytes, %u fragments, session=%u)",
        filename,
        size,
        transfer_.fragment_count,
        transfer_.session_id);

    /* Build compact TRANSFER_AD (no filename, no fragment_count) */
    TransferAdPayload ad = {};
    ad.session_id = transfer_.session_id;
    ad.file_size = static_cast<uint32_t>(size);
    ad.fragment_size_d8 = static_cast<uint8_t>(transfer_.fragment_size / 8);
    ad.crc16 = static_cast<uint16_t>(crc & 0xFFFF);

    /* Start election with adaptive timeout */
    candidate_count_ = 0;
    election_active_ = true;
    election_start_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    static constexpr uint32_t BASE_ELECTION_MS = 3000;
    static constexpr uint32_t PER_HOP_ELECTION_MS = 1000;
    static constexpr uint32_t MAX_ELECTION_MS = 8000;
    election_timeout_ms_ = BASE_ELECTION_MS +
        (hops_to_internet * PER_HOP_ELECTION_MS);
    if (election_timeout_ms_ > MAX_ELECTION_MS)
    {
        election_timeout_ms_ = MAX_ELECTION_MS;
    }

    /*
     * Send to EXIT_ANY_ADDR so relays forward toward exit nodes.
     * BROADCAST_ADDR is excluded from forwarding in process_slab.
     */
    send_broadcast_with_retry(PacketType::TRANSFER_AD,
                              reinterpret_cast<const uint8_t *>(&ad),
                              sizeof(ad),
                              EXIT_ANY_ADDR);
}

/* -- Periodic tick ------------------------------------------------------------ */

void TransferEngine::tick(uint32_t now_ms)
{
    /* ARQ timeout retransmits — tick ALL instances */
    for (uint8_t i = 0; i < MAX_EXIT_NODES; i++)
    {
        arq_[i].tick();
    }

    /* Broadcast retry */
    broadcast_retry_tick(now_ms);

    /* Election timeout */
    election_timeout_tick(now_ms);

    /* Check for dead exit nodes and redistribute */
    exit_node_health_tick(now_ms);

    /* Feed fragments into ARQ window */
    transfer_tick();
}

void TransferEngine::election_timeout_tick(uint32_t now_ms)
{
    if (!election_active_ ||
        (now_ms - election_start_ms_ <= election_timeout_ms_))
    {
        return;
    }

    election_active_ = false;

    if (candidate_count_ == 0)
    {
        ESP_LOGW(TAG, "Election timeout: no exit node candidates");
        transfer_.active = false;
        transfer_.read_chunk = nullptr;
        for (uint8_t i = 0; i < MAX_EXIT_NODES; i++)
        {
            arq_[i].reset_sender();
        }
    }
    else
    {
        /* Use ALL candidates as exit nodes */
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

    /* Local-exit selective-repeat ARQ: sliding window with cloud ACK/NACK */
    if (local_exit_)
    {
        /* 1. Drain cloud ACKs — mark in bitmap, advance base */
        if (drain_cloud_ack_fn_)
        {
            uint16_t ack_seq = 0;
            while (drain_cloud_ack_fn_(ack_seq))
            {
                if (ack_seq < transfer_.fragment_count)
                {
                    cloud_ack_bitmap_[ack_seq / 8] |=
                        static_cast<uint8_t>(1U << (ack_seq % 8));
                    ESP_LOGD(TAG, "Cloud ACK: seq=%u", ack_seq);
                }
            }
        }

        /* Advance base past contiguously ACK'd fragments */
        while (cloud_base_seq_ < transfer_.fragment_count)
        {
            uint16_t s = cloud_base_seq_;
            if (cloud_ack_bitmap_[s / 8] & (1U << (s % 8)))
            {
                cloud_base_seq_++;
            }
            else
            {
                break;
            }
        }

        /* 2. Drain cloud NACKs — enqueue for retransmission */
        if (drain_cloud_nack_fn_)
        {
            uint16_t nack_seq = 0;
            while (drain_cloud_nack_fn_(nack_seq))
            {
                if (nack_seq >= transfer_.fragment_count)
                {
                    continue;
                }
                /* Only retransmit if not already ACK'd */
                if (cloud_ack_bitmap_[nack_seq / 8] & (1U << (nack_seq % 8)))
                {
                    continue;
                }
                /* Avoid duplicates in retransmit queue */
                bool already_queued = false;
                for (uint8_t i = 0; i < cloud_retx_count_; i++)
                {
                    if (cloud_retx_queue_[i] == nack_seq)
                    {
                        already_queued = true;
                        break;
                    }
                }
                if (!already_queued &&
                    cloud_retx_count_ < CLOUD_RETX_QUEUE_SIZE)
                {
                    cloud_retx_queue_[cloud_retx_count_++] = nack_seq;
                    ESP_LOGI(TAG, "Cloud NACK: queued retx seq=%u", nack_seq);
                }
            }
        }

        /* 3. Send retransmissions first (priority over new fragments) */
        uint8_t sent = 0;
        for (uint8_t i = 0; i < cloud_retx_count_ && sent < CLOUD_WINDOW_SIZE;
             i++)
        {
            uint16_t seq = cloud_retx_queue_[i];
            /* Skip if already ACK'd in the meantime */
            if (cloud_ack_bitmap_[seq / 8] & (1U << (seq % 8)))
            {
                continue;
            }

            size_t offset =
                static_cast<size_t>(seq) * transfer_.fragment_size;
            size_t remain = transfer_.size - offset;
            size_t frag_len = (remain < transfer_.fragment_size)
                                  ? remain
                                  : transfer_.fragment_size;

            uint8_t frag_buf[MAX_MTU];
            size_t got = transfer_.read_chunk(frag_buf, offset, frag_len);
            forward_to_mqtt_fn_(transfer_.session_id,
                                seq,
                                my_addr_,
                                frag_buf,
                                got,
                                transfer_.filename);
            ESP_LOGI(TAG, "Retransmitted seq=%u", seq);
            sent++;
        }
        /* Remove sent retransmissions from queue (keep unsent ones) */
        {
            uint8_t kept = 0;
            uint8_t skip = 0;
            for (uint8_t i = 0; i < cloud_retx_count_; i++)
            {
                uint16_t seq = cloud_retx_queue_[i];
                if (cloud_ack_bitmap_[seq / 8] & (1U << (seq % 8)))
                {
                    continue; /* ACK'd, drop */
                }
                if (skip < sent)
                {
                    skip++; /* was retransmitted this tick, drop from queue */
                    continue;
                }
                cloud_retx_queue_[kept++] = seq;
            }
            cloud_retx_count_ = kept;
        }

        /* 4. Send new fragments if window not full */
        uint16_t window_end = cloud_base_seq_ + CLOUD_WINDOW_SIZE;
        while (cloud_next_send_ < transfer_.fragment_count &&
               cloud_next_send_ < window_end &&
               sent < CLOUD_WINDOW_SIZE)
        {
            uint16_t seq = cloud_next_send_;
            size_t offset =
                static_cast<size_t>(seq) * transfer_.fragment_size;
            size_t remain = transfer_.size - offset;
            size_t frag_len = (remain < transfer_.fragment_size)
                                  ? remain
                                  : transfer_.fragment_size;

            uint8_t frag_buf[MAX_MTU];
            size_t got = transfer_.read_chunk(frag_buf, offset, frag_len);
            forward_to_mqtt_fn_(transfer_.session_id,
                                seq,
                                my_addr_,
                                frag_buf,
                                got,
                                transfer_.filename);
            cloud_next_send_++;
            sent++;
        }

        /* 5. Check completion: all fragments ACK'd */
        if (cloud_base_seq_ >= transfer_.fragment_count)
        {
            ESP_LOGI(TAG,
                     "Local-exit transfer complete (all ACK'd): %s",
                     transfer_.filename);
            transfer_.active = false;
            transfer_.read_chunk = nullptr;
            local_exit_ = false;
            /* Reset cloud ARQ state */
            memset(cloud_ack_bitmap_, 0, sizeof(cloud_ack_bitmap_));
            cloud_base_seq_ = 0;
            cloud_next_send_ = 0;
            cloud_retx_count_ = 0;
            xEventGroupSetBits(events_, FLP_EVT_TRANSFER_COMPLETE);
        }
        return;
    }

    if (transfer_.exit_node_count == 0)
    {
        return;
    }

    /* Count alive exit nodes */
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
        transfer_.read_chunk = nullptr;
        return;
    }

    /* Drain pending redistribution queue first */
    if (redist_count_ > 0)
    {
        uint8_t new_count = 0;
        for (uint8_t r = 0; r < redist_count_; r++)
        {
            uint16_t seq = redist_pending_[r];
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
                uint8_t frag_buf[MAX_MTU];
                size_t got = transfer_.read_chunk(frag_buf, offset, frag_len);
                arq_[target].send_fragment(seq, frag_buf, got);
            }
            else
            {
                redist_pending_[new_count++] = seq;
            }
        }
        redist_count_ = new_count;
    }

    /*
     * Round-robin fragment assignment across alive exit nodes.
     * Fragment 0 carries a filename prefix: [len:1][filename:N][data...]
     */
    while (transfer_.next_fragment < transfer_.fragment_count)
    {
        /* Find next alive exit node for this fragment */
        uint8_t arq_idx = transfer_.next_fragment % transfer_.exit_node_count;
        if (!transfer_.exit_node_alive[arq_idx])
        {
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
            /* Map seq to data index (skip parity slots) */
            uint16_t group = seq / (FEC_GROUP_SIZE + 1);
            uint16_t idx_in_group = seq % (FEC_GROUP_SIZE + 1);
            uint16_t data_idx = group * FEC_GROUP_SIZE + idx_in_group;

            size_t offset =
                static_cast<size_t>(data_idx) * transfer_.fragment_size;
            if (offset >= transfer_.size)
            {
                transfer_.next_fragment++;
                continue;
            }
            size_t remain = transfer_.size - offset;
            size_t frag_len = (remain < transfer_.fragment_size)
                                  ? remain
                                  : transfer_.fragment_size;

            /*
             * Fragment 0 (seq=0): prepend filename so exit node
             * can publish transfer meta after receiving it.
             */
            uint8_t frag_buf[MAX_MTU];
            size_t got = transfer_.read_chunk(frag_buf, offset, frag_len);

            if (seq == 0)
            {
                uint8_t name_len = static_cast<uint8_t>(
                    strnlen(transfer_.filename, sizeof(transfer_.filename) - 1));
                uint8_t frag0_buf[MAX_MTU];
                frag0_buf[0] = name_len;
                memcpy(frag0_buf + 1, transfer_.filename, name_len);
                memcpy(frag0_buf + 1 + name_len, frag_buf, got);
                size_t total_len = 1 + name_len + got;

                int ret = arq_[arq_idx].send_fragment(
                    seq, frag0_buf, total_len);
                if (ret < 0)
                {
                    break;
                }
                fec_encoder_.ingest(frag_buf, got);
            }
            else
            {
                int ret = arq_[arq_idx].send_fragment(
                    seq, frag_buf, got);
                if (ret < 0)
                {
                    break;
                }
                fec_encoder_.ingest(frag_buf, got);
            }
        }

        transfer_.next_fragment++;
    }

    /* Check if all fragments sent and acknowledged (only check alive exits) */
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
            transfer_.read_chunk = nullptr;
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

    uint16_t base = arq_[dead_idx].get_base_seq();
    uint16_t next = arq_[dead_idx].get_next_seq();
    arq_[dead_idx].reset_sender();

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

    uint16_t redistributed = 0;
    for (uint16_t seq = base; seq < next; seq++)
    {
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

        size_t offset = static_cast<size_t>(seq) * transfer_.fragment_size;
        size_t remain = transfer_.size - offset;
        size_t frag_len = (remain < transfer_.fragment_size)
                              ? remain
                              : transfer_.fragment_size;

        if (!arq_[target].sender_window_full())
        {
            uint8_t frag_buf[MAX_MTU];
            size_t got = transfer_.read_chunk(frag_buf, offset, frag_len);
            arq_[target].send_fragment(seq, frag_buf, got);
            redistributed++;
        }
        else if (redist_count_ <
                 sizeof(redist_pending_) / sizeof(redist_pending_[0]))
        {
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
