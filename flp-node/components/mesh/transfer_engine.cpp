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
static constexpr uint32_t BASE_ARQ_TIMEOUT_MS = 700;
static constexpr uint32_t PER_HOP_ARQ_TIMEOUT_MS = 400;
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
        arq_[i].init(ARQ_WINDOW, BASE_ARQ_TIMEOUT_MS);
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
    if (payload_len < MIN_TRANSFER_AD_LEN)
    {
        return;
    }

    TransferAdPayload ad = {};
    size_t copy_len = (payload_len < sizeof(ad)) ? payload_len : sizeof(ad);
    memcpy(&ad, payload, copy_len);

    uint16_t frag_size = static_cast<uint16_t>(ad.fragment_size_d8) * 8;
    uint16_t frag_count = ad.fragment_count;

    ESP_LOGI(TAG,
             "Transfer ad from 0x%04X: size=%" PRIu32
             " frag_size=%u frags=%u session=%u crc32=%" PRIu32,
             hdr.src_addr,
             ad.file_size,
             frag_size,
             frag_count,
             ad.session_id,
             ad.crc32);

    /* If we have internet, respond as exit node candidate */
    if (has_internet)
    {
        /* Reject if we're doing our own transfer — the MQTT pipeline
         * (fragment_publish_queue_ + fragment_ack_queue_) is shared and
         * cannot serve two transfers simultaneously without pollution. */
        if (transfer_.active)
        {
            ESP_LOGW(TAG,
                     "Declining exit role: own transfer in progress "
                     "(session=%u)",
                     transfer_.session_id);
            return;
        }

        /* Dedup: if already set up for this session, just re-send ACK */
        if (is_exit_node_ && active_session_id_ == ad.session_id)
        {
            TransferAckPayload ack = {};
            ack.session_id = ad.session_id;
            ack.exit_node_addr = my_addr_;
            ack.hops_to_gw = 0;
            ack.rssi_to_gw = 0;
            send_fn_(hdr.src_addr,
                     PacketType::TRANSFER_ACK,
                     reinterpret_cast<const uint8_t *>(&ack),
                     sizeof(ack), 0);
            return;
        }

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
        transfer_.size = ad.file_size;
        transfer_.fragment_count = frag_count;
        transfer_.fragment_size = frag_size;
        transfer_.crc32 = ad.crc32;
        {
            uint32_t now_ts =
                static_cast<uint32_t>(esp_timer_get_time() / 1000);
            last_cloud_activity_ms_ = now_ts;
            last_meta_publish_ms_ = now_ts;
        }

        /* Set up ACK path: arq_[0] for sending ACKs back to source */
        arq_[0].set_peer_addr(hdr.src_addr);

        /* Extract filename from ad */
        if (ad.filename_len > 0 &&
            ad.filename_len <= sizeof(ad.filename) &&
            ad.filename_len < sizeof(transfer_.filename))
        {
            memcpy(transfer_.filename, ad.filename, ad.filename_len);
            transfer_.filename[ad.filename_len] = '\0';
        }
        else
        {
            transfer_.filename[0] = '\0';
        }

        /* Publish transfer meta immediately */
        if (forward_meta_fn_)
        {
            forward_meta_fn_(ad.session_id,
                             transfer_.filename,
                             hdr.src_addr,
                             ad.file_size,
                             frag_count,
                             frag_size,
                             ad.crc32);
        }
        ESP_LOGI(TAG,
                 "Exit node mode: session=%u source=0x%04X file=%s",
                 active_session_id_,
                 source_addr_,
                 transfer_.filename);
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

    /* Dedup: ignore duplicate responses from the same exit node */
    for (uint8_t i = 0; i < candidate_count_; i++)
    {
        if (candidates_[i].addr == ack.exit_node_addr)
        {
            return;
        }
    }

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
        /*
         * Deferred-ACK architecture: enqueue fragment for MQTT publish but
         * do NOT send a mesh ACK here.  The MQTT task pushes the seq into
         * fragment_ack_queue_ after successful esp_mqtt_client_publish().
         * TransferEngine::tick() drains that queue and sends mesh ACKs.
         *
         * This makes the sender's ARQ window track actual MQTT throughput:
         * the window only opens when MQTT has capacity, preventing the
         * queue-full / ESP_NO_MEM cascade that occurred when ACKs raced
         * ahead of MQTT draining.
         *
         * If the MQTT publish queue is full, stay silent — the sender's
         * ARQ timeout will retransmit after the rate-limited delay.
         */
        bool queued = false;
        if (forward_to_mqtt_fn_)
        {
            queued = forward_to_mqtt_fn_(active_session_id_,
                                         hdr.seq_num,
                                         source_addr_,
                                         payload,
                                         payload_len,
                                         transfer_.filename);
        }

        if (queued)
        {
            /* ACK is deferred — sent when MQTT task confirms publish */
            last_cloud_activity_ms_ =
                static_cast<uint32_t>(esp_timer_get_time() / 1000);
        }
        else
        {
            ESP_LOGW(TAG, "MQTT queue full, dropping seq=%u (ARQ will retry)",
                     hdr.seq_num);
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
        /* Phase 3: compute RTT sample from send timestamp */
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        uint32_t send_time = arq_[idx].get_send_time(seq);
        if (send_time > 0 && now > send_time)
        {
            float rtt_sample = static_cast<float>(now - send_time);
            constexpr float ALPHA = 0.3f;
            path_stats_[idx].ewma_rtt_ms =
                ALPHA * rtt_sample +
                (1.0f - ALPHA) * path_stats_[idx].ewma_rtt_ms;
        }
        path_stats_[idx].acked++;

        arq_[idx].handle_ack(seq);
        transfer_.last_ack_ms[idx] = now;
    }
}

void TransferEngine::handle_nack(uint16_t seq, uint16_t from_addr)
{
    int8_t idx = arq_index_for_peer(from_addr);
    if (idx < 0)
    {
        return;
    }

    path_stats_[idx].nacked++; /* Phase 3 */

    /* If seq is still within the ARQ window, normal retransmit */
    if (seq >= arq_[idx].get_base_seq())
    {
        arq_[idx].handle_nack(seq);
        return;
    }

    /*
     * Out-of-window NACK: the mesh ARQ already advanced past this seq
     * (exit node ACK'd it on receipt), but the cloud later NACK'd it.
     * Queue for throttled re-send in tick_mesh_arq() instead of firing
     * immediately, which overwhelms ESP-NOW and the exit node's MQTT queue.
     */
    if (!transfer_.active || seq >= transfer_.fragment_count)
    {
        return;
    }

    /* Skip parity — cannot reconstruct without re-XOR of entire group */
    if (seq % (FEC_GROUP_SIZE + 1) == FEC_GROUP_SIZE)
    {
        return;
    }

    /* Dedup: don't queue if already pending */
    for (uint8_t i = 0; i < oow_retx_count_; i++)
    {
        if (oow_retx_queue_[i] == seq)
        {
            return;
        }
    }

    if (oow_retx_count_ < OOW_RETX_QUEUE_SIZE)
    {
        oow_retx_queue_[oow_retx_count_++] = seq;
        ESP_LOGI(TAG, "Queued out-of-window seq=%u for retx (%u pending)",
                 seq, oow_retx_count_);
    }
    else
    {
        ESP_LOGW(TAG,
                 "Out-of-window retx queue full, dropping seq=%u", seq);
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

    /* Reject if serving as exit node — the MQTT pipeline is shared */
    if (is_exit_node_)
    {
        ESP_LOGW(TAG, "Cannot start transfer: serving as exit node "
                 "(session=%u)", active_session_id_);
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
    static uint16_t s_session_counter = 0;
    transfer_.session_id = ++s_session_counter ? s_session_counter
                                               : ++s_session_counter; /* skip 0 */
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

    transfer_.crc32 = crc;
    last_meta_publish_ms_ =
        static_cast<uint32_t>(esp_timer_get_time() / 1000);

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
        last_cloud_activity_ms_ =
            static_cast<uint32_t>(esp_timer_get_time() / 1000);

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

    /* Build TRANSFER_AD with full CRC32, fragment count, and filename */
    TransferAdPayload ad = {};
    ad.session_id = transfer_.session_id;
    ad.file_size = static_cast<uint32_t>(size);
    ad.fragment_size_d8 = static_cast<uint8_t>(transfer_.fragment_size / 8);
    ad.crc32 = crc;
    ad.fragment_count = transfer_.fragment_count;
    ad.filename_len = static_cast<uint8_t>(
        strnlen(filename, sizeof(ad.filename)));
    memcpy(ad.filename, filename, ad.filename_len);

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

    /* P8: Adaptive ARQ timeout — increase for multi-hop to avoid
     * spurious retransmissions when round-trip exceeds 700ms. */
    uint32_t arq_timeout = BASE_ARQ_TIMEOUT_MS +
        (hops_to_internet * PER_HOP_ARQ_TIMEOUT_MS);
    if (arq_timeout > MAX_ARQ_TIMEOUT_MS)
    {
        arq_timeout = MAX_ARQ_TIMEOUT_MS;
    }
    for (int i = 0; i < MAX_EXIT_NODES; i++)
    {
        arq_[i].set_timeout(arq_timeout);
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
    /* Shared per-tick send budget: all data-carrying send paths
     * (ARQ retransmits, OOW retransmits, new fragments) decrement
     * from this single counter.  Keeps total ESP-NOW TX pressure
     * within what the radio can buffer without ESP_ERR_ESPNOW_NO_MEM. */
    tick_send_budget_ = MAX_SENDS_PER_TICK;

    /* ARQ timeout retransmits — tick ALL instances, sharing the budget */
    for (uint8_t i = 0; i < MAX_EXIT_NODES; i++)
    {
        if (tick_send_budget_ == 0) { break; }
        uint8_t used = arq_[i].tick(tick_send_budget_);
        tick_send_budget_ -= used;
    }

    /* Abort transfer if any ARQ instance has fatally failed */
    if (transfer_.active && !local_exit_)
    {
        for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
        {
            if (arq_[i].is_sender_failed())
            {
                ESP_LOGE(TAG, "Transfer aborted: ARQ[%u] max retries exceeded", i);
                transfer_.active = false;
                is_exit_node_ = false;
                local_exit_ = false;
                transfer_.exit_node_count = 0;
                for (uint8_t j = 0; j < MAX_EXIT_NODES; j++)
                {
                    arq_[j].reset_sender();
                }
                return;
            }
        }
    }

    /* Periodic meta re-publish: if the cloud restarted mid-transfer, it
     * has no session state. Re-publishing meta lets it pick up the session
     * and replay any staged chunks.
     * Only re-publish if data is still actively flowing (cloud_activity
     * within the last 15s). This prevents ghost meta re-publishes after
     * the source transfer has completed but exit node role hasn't cleared. */
    if (forward_meta_fn_ && last_meta_publish_ms_ > 0 &&
        (now_ms - last_meta_publish_ms_) > META_REPUBLISH_INTERVAL_MS)
    {
        bool data_still_flowing = last_cloud_activity_ms_ > 0 &&
            (now_ms - last_cloud_activity_ms_) < 15000;
        bool should_republish =
            data_still_flowing &&
            ((local_exit_ && transfer_.active) || is_exit_node_);
        if (should_republish)
        {
            uint16_t sid = local_exit_ ? transfer_.session_id
                                       : active_session_id_;
            uint16_t src = local_exit_ ? my_addr_ : source_addr_;
            forward_meta_fn_(sid,
                             transfer_.filename,
                             src,
                             static_cast<uint32_t>(transfer_.size),
                             transfer_.fragment_count,
                             transfer_.fragment_size,
                             transfer_.crc32);
            last_meta_publish_ms_ = now_ms;
            ESP_LOGI(TAG, "Re-published transfer meta (session=%u)", sid);
        }
    }

    /* Cloud stall timeout: if the cloud hasn't ACK'd anything for
     * CLOUD_STALL_TIMEOUT_MS, abort local-exit transfer or exit-node role.
     * Prevents the firmware from being stuck forever when the cloud is
     * down, restarted, or unreachable. */
    if (last_cloud_activity_ms_ > 0 &&
        (now_ms - last_cloud_activity_ms_) > CLOUD_STALL_TIMEOUT_MS)
    {
        if (local_exit_ && transfer_.active)
        {
            ESP_LOGW(TAG,
                     "Local-exit cloud stall: no ACK for %" PRIu32
                     "ms, aborting transfer",
                     now_ms - last_cloud_activity_ms_);
            transfer_.active = false;
            transfer_.read_chunk = nullptr;
            local_exit_ = false;
            memset(cloud_ack_bitmap_, 0, sizeof(cloud_ack_bitmap_));
            cloud_base_seq_ = 0;
            cloud_next_send_ = 0;
            cloud_retx_count_ = 0;
            last_cloud_activity_ms_ = 0;
            xEventGroupSetBits(events_, FLP_EVT_TRANSFER_COMPLETE);
            return;
        }
        if (is_exit_node_)
        {
            ESP_LOGW(TAG,
                     "Exit node cloud stall: no activity for %" PRIu32
                     "ms, clearing exit-node role",
                     now_ms - last_cloud_activity_ms_);
            is_exit_node_ = false;
            active_session_id_ = 0;
            last_cloud_activity_ms_ = 0;
        }
    }

    /* D1+B3 fix: exit node drains cloud transfer NACKs and forwards as
     * mesh NACKs to source, bridging the end-to-end reliability gap. */
    if (is_exit_node_ && drain_cloud_nack_fn_)
    {
        uint16_t nack_seq = 0;
        while (drain_cloud_nack_fn_(nack_seq))
        {
            send_fn_(source_addr_, PacketType::NACK, nullptr, 0, nack_seq);
            ESP_LOGI(TAG, "Exit->source NACK for seq=%u", nack_seq);
        }
    }

    /* Deferred fragment ACK: MQTT task published a fragment successfully.
     * Send the mesh ACK back to the source now.  This closes the
     * backpressure loop: sender ARQ window only advances when MQTT has
     * actually consumed the fragment, preventing queue overflow. */
    if (is_exit_node_ && drain_fragment_ack_fn_)
    {
        uint16_t ack_seq = 0;
        while (drain_fragment_ack_fn_(ack_seq))
        {
            send_fn_(source_addr_, PacketType::ACK, nullptr, 0, ack_seq);
            ESP_LOGD(TAG, "Deferred ACK for seq=%u", ack_seq);
        }
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
        /* Phase 3: Quality-filtered election.
         * Score each candidate using RSSI and hops, then reject candidates
         * whose score is more than 2x worse than the best. */

        /* 1. Score candidates: higher is better */
        int16_t scores[MAX_EXIT_NODES] = {};
        int16_t best_score = -32000;
        for (uint8_t i = 0; i < candidate_count_; i++)
        {
            scores[i] = static_cast<int16_t>(candidates_[i].rssi_to_gw) -
                         static_cast<int16_t>(6 * candidates_[i].hops_to_gw);
            if (scores[i] > best_score)
            {
                best_score = scores[i];
            }
        }

        /* 2. Simple insertion sort by score descending (max 4 elements) */
        for (uint8_t i = 1; i < candidate_count_; i++)
        {
            ExitCandidate tc = candidates_[i];
            int16_t ts = scores[i];
            int8_t j = static_cast<int8_t>(i) - 1;
            while (j >= 0 && scores[j] < ts)
            {
                candidates_[j + 1] = candidates_[j];
                scores[j + 1] = scores[j];
                j--;
            }
            candidates_[j + 1] = tc;
            scores[j + 1] = ts;
        }

        /* 3. Accept candidates within quality threshold */
        int16_t threshold = best_score / 2;
        uint8_t accepted = 0;
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        for (uint8_t i = 0; i < candidate_count_ && accepted < MAX_EXIT_NODES; i++)
        {
            if (scores[i] >= threshold)
            {
                transfer_.exit_nodes[accepted] = candidates_[i].addr;
                transfer_.exit_node_alive[accepted] = true;
                transfer_.last_ack_ms[accepted] = now;
                arq_[accepted].reset_sender();
                arq_[accepted].set_peer_addr(candidates_[i].addr);

                /* Initialize path stats prior from election info */
                path_stats_[accepted] = ExitPathStats{};
                path_stats_[accepted].ewma_rtt_ms =
                    static_cast<float>(BASE_ARQ_TIMEOUT_MS +
                        candidates_[i].hops_to_gw * PER_HOP_ARQ_TIMEOUT_MS);

                accepted++;
                ESP_LOGI(TAG, "Accepted exit #%u: 0x%04X score=%d rtt_prior=%.0f",
                         accepted, candidates_[i].addr, scores[i],
                         path_stats_[accepted - 1].ewma_rtt_ms);
            }
            else
            {
                ESP_LOGI(TAG, "Rejected exit 0x%04X: score=%d < threshold=%d",
                         candidates_[i].addr, scores[i], threshold);
            }
        }

        transfer_.exit_node_count = accepted;
        weight_recompute_counter_ = 0;

        /* Set stride for multi-exit ARQ ownership */
        for (uint8_t i = 0; i < accepted; i++)
        {
            arq_[i].set_exit_stride(accepted, i);
        }

        ESP_LOGI(TAG, "Elected %u exit nodes (of %u candidates)",
                 accepted, candidate_count_);

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

    /* Congestion backoff: skip fragment feeding for N ticks per signal.
     * Cap at 20 ticks (~200-400ms) to avoid stalling the transfer
     * indefinitely while still giving the exit node time to drain. */
    if (congestion_backoff_ticks_ > 0)
    {
        congestion_backoff_ticks_--;
        return;
    }

    if (local_exit_)
    {
        tick_local_exit_arq();
    }
    else
    {
        tick_mesh_arq();
    }
}

/* --------------------------------------------------------------------------
 * Local-exit selective-repeat ARQ: sliding window with cloud ACK/NACK.
 * Source node has internet + MQTT — fragments go directly to broker.
 * ----------------------------------------------------------------------- */
void TransferEngine::tick_local_exit_arq()
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
                last_cloud_activity_ms_ =
                    static_cast<uint32_t>(esp_timer_get_time() / 1000);
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

        size_t got = transfer_.read_chunk(frag_buf_, offset, frag_len);
        if (!forward_to_mqtt_fn_ ||
            !forward_to_mqtt_fn_(transfer_.session_id,
                                 seq,
                                 my_addr_,
                                 frag_buf_,
                                 got,
                                 transfer_.filename))
        {
            break; /* queue full or no callback, retry next tick */
        }
        ESP_LOGI(TAG, "Retransmitted seq=%u", seq);
        sent++;
    }
    compact_retx_queue(sent);

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

        size_t got = transfer_.read_chunk(frag_buf_, offset, frag_len);
        if (!forward_to_mqtt_fn_ ||
            !forward_to_mqtt_fn_(transfer_.session_id,
                                 seq,
                                 my_addr_,
                                 frag_buf_,
                                 got,
                                 transfer_.filename))
        {
            break; /* queue full or no callback, retry next tick */
        }
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
}

/*
 * Remove sent retransmissions from the cloud retx queue.
 * Entries that have been ACK'd or retransmitted this tick are dropped;
 * unsent entries are compacted toward the front.
 */
void TransferEngine::compact_retx_queue(uint8_t sent)
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

/* --------------------------------------------------------------------------
 * Phase 3: Recompute per-exit scheduling weights from EWMA RTT and loss.
 * Score = 1 / (rtt * (1 + loss * penalty)). Weights are normalized [0,1].
 * ----------------------------------------------------------------------- */
void TransferEngine::recompute_weights()
{
    float total_score = 0;
    float scores[MAX_EXIT_NODES] = {};
    constexpr float LOSS_PENALTY = 5.0f;
    constexpr float ALPHA = 0.3f;

    for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
    {
        if (!transfer_.exit_node_alive[i])
        {
            continue;
        }
        auto &s = path_stats_[i];

        /* Update loss EWMA from recent counters */
        float loss = (s.sent > 0)
                         ? static_cast<float>(s.nacked) /
                               static_cast<float>(s.sent)
                         : 0.0f;
        s.ewma_loss = ALPHA * loss + (1.0f - ALPHA) * s.ewma_loss;

        /* Guard: minimum RTT of 10ms to avoid division by near-zero */
        float rtt = (s.ewma_rtt_ms > 10.0f) ? s.ewma_rtt_ms : 10.0f;
        scores[i] = 1.0f / (rtt * (1.0f + s.ewma_loss * LOSS_PENALTY));
        total_score += scores[i];
    }

    if (total_score > 0)
    {
        for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
        {
            path_stats_[i].weight = transfer_.exit_node_alive[i]
                                        ? scores[i] / total_score
                                        : 0.0f;
        }
    }
}

/* --------------------------------------------------------------------------
 * Mesh-path multi-exit: feed fragments across alive exit nodes
 * via SelectiveRepeat ARQ over the mesh network.
 * ----------------------------------------------------------------------- */
void TransferEngine::tick_mesh_arq()
{
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

    /* Drain out-of-window retransmit queue (cloud NACKs for seqs the ARQ
     * has already advanced past).  Uses the shared per-tick send budget
     * to avoid overwhelming ESP-NOW.  On send failure (NO_MEM), the seq
     * is kept in the queue for retry on the next tick. */
    if (oow_retx_count_ > 0 && transfer_.read_chunk && tick_send_budget_ > 0)
    {
        uint8_t sent = 0;
        uint8_t kept = 0;
        for (uint8_t i = 0; i < oow_retx_count_; i++)
        {
            uint16_t seq = oow_retx_queue_[i];

            if (tick_send_budget_ == 0)
            {
                oow_retx_queue_[kept++] = seq;
                continue;
            }

            uint16_t group = seq / (FEC_GROUP_SIZE + 1);
            uint16_t idx_in_group = seq % (FEC_GROUP_SIZE + 1);
            uint16_t data_idx = group * FEC_GROUP_SIZE + idx_in_group;
            size_t offset =
                static_cast<size_t>(data_idx) * transfer_.fragment_size;
            if (offset >= transfer_.size)
            {
                continue; /* drop invalid */
            }

            size_t remain = transfer_.size - offset;
            size_t frag_len = (remain < transfer_.fragment_size)
                                  ? remain
                                  : transfer_.fragment_size;
            size_t got = transfer_.read_chunk(frag_buf_, offset, frag_len);

            /* Pick the first alive exit node for re-send */
            uint16_t dst = transfer_.exit_nodes[0];
            for (uint8_t e = 0; e < transfer_.exit_node_count; e++)
            {
                if (transfer_.exit_node_alive[e])
                {
                    dst = transfer_.exit_nodes[e];
                    break;
                }
            }

            int rc = send_fn_(dst, PacketType::DATA, frag_buf_, got, seq);
            if (rc < 0)
            {
                /* Send failed (NO_MEM) — keep in queue for next tick */
                oow_retx_queue_[kept++] = seq;
                tick_send_budget_ = 0; /* stop all further sends this tick */
                break;
            }
            ESP_LOGI(TAG, "Re-sent out-of-window seq=%u to 0x%04X", seq, dst);
            sent++;
            tick_send_budget_--;
        }
        oow_retx_count_ = kept;
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
                /* B2 fix: map seq to data index (skip parity slots) */
                uint16_t rgroup = seq / (FEC_GROUP_SIZE + 1);
                uint16_t ridx   = seq % (FEC_GROUP_SIZE + 1);
                uint16_t data_idx = rgroup * FEC_GROUP_SIZE + ridx;
                size_t offset =
                    static_cast<size_t>(data_idx) * transfer_.fragment_size;
                size_t remain = transfer_.size - offset;
                size_t frag_len = (remain < transfer_.fragment_size)
                                      ? remain
                                      : transfer_.fragment_size;
                size_t got = transfer_.read_chunk(frag_buf_, offset, frag_len);
                arq_[target].send_fragment(seq, frag_buf_, got);
            }
            else
            {
                redist_pending_[new_count++] = seq;
            }
        }
        redist_count_ = new_count;
    }

    /*
     * Phase 3: Window-aware weighted fragment assignment.
     * Replaces blind round-robin with score = weight * free_window_slots.
     * Combines long-term quality (EWMA RTT+loss via weight) with transient
     * load awareness (current ARQ window occupancy).
     */

    /* Periodically recompute weights from accumulated stats */
    if (weight_recompute_counter_ >= WEIGHT_RECOMPUTE_INTERVAL)
    {
        recompute_weights();
        weight_recompute_counter_ = 0;
    }

    /* New fragment sends share the per-tick budget with ARQ retransmits
     * and OOW retransmits above. */
    while (transfer_.next_fragment < transfer_.fragment_count &&
           tick_send_budget_ > 0)
    {
        /* Score all alive exits: weight * free_slots */
        uint8_t arq_idx = 0;
        float best_score = -1.0f;
        for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
        {
            if (!transfer_.exit_node_alive[i])
            {
                continue;
            }
            uint16_t used = arq_[i].sender_window_used();
            uint16_t free_slots = (used < ARQ_WINDOW) ?
                static_cast<uint16_t>(ARQ_WINDOW - used) : 0;
            float score = path_stats_[i].weight *
                          static_cast<float>(free_slots);
            if (score > best_score)
            {
                best_score = score;
                arq_idx = i;
            }
        }

        if (best_score <= 0)
        {
            break; /* all windows full or no alive exits, wait for ACKs */
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

            size_t got = transfer_.read_chunk(frag_buf_, offset, frag_len);

            int ret = arq_[arq_idx].send_fragment(seq, frag_buf_, got);
            if (ret < 0)
            {
                break;
            }
            fec_encoder_.ingest(frag_buf_, got);
        }

        /* Phase 3: track per-exit send count and weight recompute interval */
        path_stats_[arq_idx].sent++;
        weight_recompute_counter_++;
        transfer_.next_fragment++;
        tick_send_budget_--;
    }

    /* Check if all fragments sent and acknowledged (only check alive exits).
     * Use sender_window_used()==0 instead of base_seq >= fragment_count
     * because some trailing seqs (data past EOF, final parity) are skipped
     * by the feeding loop and never enqueued into the ARQ window — so
     * base_seq can never advance past them via handle_ack(). */
    if (transfer_.next_fragment >= transfer_.fragment_count)
    {
        bool all_done = true;
        for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
        {
            if (!transfer_.exit_node_alive[i])
            {
                continue;
            }
            if (arq_[i].sender_window_used() > 0)
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
    /* D5 fix: removed exit_node_count <= 1 guard so single-exit transfers
     * get fast failure detection (10s timeout) instead of waiting for
     * per-fragment ARQ MAX_RETRIES cascade (~67s). */
    if (!transfer_.active || election_active_ || transfer_.exit_node_count == 0)
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

void TransferEngine::handle_exit_offline(uint16_t exit_addr,
                                         uint16_t session_id)
{
    if (!transfer_.active)
    {
        return;
    }
    if (session_id != 0 && session_id != transfer_.session_id)
    {
        return; /* different transfer session */
    }
    for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
    {
        if (transfer_.exit_nodes[i] == exit_addr &&
            transfer_.exit_node_alive[i])
        {
            ESP_LOGW(TAG,
                     "Exit 0x%04X reported offline, redistributing",
                     exit_addr);
            redistribute_dead_exit(i);
            return;
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
        /* Skip parity slots — parity cannot be reconstructed here without
         * re-XOR-ing all group members.  The receiver either has all data
         * fragments or will get them via normal ARQ retransmit. */
        if (seq % (FEC_GROUP_SIZE + 1) == FEC_GROUP_SIZE)
        {
            continue;
        }

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

        /* B2 fix: map seq to data index (skip parity slots) */
        uint16_t rgroup = seq / (FEC_GROUP_SIZE + 1);
        uint16_t ridx   = seq % (FEC_GROUP_SIZE + 1);
        uint16_t data_idx = rgroup * FEC_GROUP_SIZE + ridx;
        size_t offset = static_cast<size_t>(data_idx) * transfer_.fragment_size;
        size_t remain = transfer_.size - offset;
        size_t frag_len = (remain < transfer_.fragment_size)
                              ? remain
                              : transfer_.fragment_size;

        if (!arq_[target].sender_window_full())
        {
            size_t got = transfer_.read_chunk(frag_buf_, offset, frag_len);
            arq_[target].send_fragment(seq, frag_buf_, got);
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
    if (static_cast<int32_t>(now_ms - broadcast_retry_.next_send_ms) < 0)
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
