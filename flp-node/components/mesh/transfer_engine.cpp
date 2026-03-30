/*
 * =============================================================================
 * transfer_engine.cpp — File transfer state machine
 * =============================================================================
 */

#include "transfer_engine.hpp"

#include <cinttypes>
#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"

using namespace flp;

static const char *TAG = "xfer_eng";
static constexpr uint32_t BROADCAST_INITIAL_BACKOFF_MS = 500;

constexpr EventBits_t FLP_EVT_TRANSFER_COMPLETE = BIT1;
constexpr EventBits_t FLP_EVT_EXIT_NODE_ELECTED = BIT2;

uint8_t TransferEngine::sanitize_hops_to_exit(uint8_t hops)
{
    if (hops == 0)
    {
        return 1;
    }
    if (hops == 0xFF)
    {
        return 2;
    }
    return hops;
}

uint32_t TransferEngine::compute_election_timeout_ms(uint8_t hops_to_exit)
{
    uint32_t timeout = BASE_ELECTION_MS +
                       static_cast<uint32_t>(hops_to_exit) *
                           PER_HOP_ELECTION_MS;
    return (timeout > MAX_ELECTION_MS) ? MAX_ELECTION_MS : timeout;
}

uint32_t TransferEngine::compute_arq_timeout_ms(uint8_t hops_to_exit)
{
    uint32_t timeout = BASE_ARQ_TIMEOUT_MS +
                       static_cast<uint32_t>(hops_to_exit) *
                           PER_HOP_ARQ_TIMEOUT_MS;
    return (timeout > MAX_ARQ_TIMEOUT_MS)
               ? MAX_ARQ_TIMEOUT_MS
               : timeout;
}

uint32_t TransferEngine::compute_exit_timeout_ms(uint8_t hops_to_exit)
{
    uint32_t timeout = BASE_EXIT_NODE_TIMEOUT_MS +
                       static_cast<uint32_t>(hops_to_exit) *
                           PER_HOP_EXIT_NODE_TIMEOUT_MS;
    return (timeout > MAX_EXIT_NODE_TIMEOUT_MS)
               ? MAX_EXIT_NODE_TIMEOUT_MS
               : timeout;
}

void TransferEngine::reset_sender_transfer_state(bool signal_complete)
{
    uint16_t finished_session = transfer_.session_id;

    transfer_.active = false;
    transfer_.read_chunk = nullptr;
    transfer_.size = 0;
    transfer_.crc32 = 0;
    transfer_.fragment_count = 0;
    transfer_.fragment_size = 0;
    transfer_.next_fragment = 0;
    transfer_.exit_node_count = 0;
    transfer_.session_id = 0;
    transfer_.filename[0] = '\0';
    source_hops_to_exit_est_ = 1;
    exit_node_timeout_ms_ = BASE_EXIT_NODE_TIMEOUT_MS;
    local_exit_ = false;
    election_active_ = false;
    candidate_count_ = 0;
    congestion_backoff_ticks_ = 0;
    redist_count_ = 0;
    oow_retx_count_ = 0;
    weight_recompute_counter_ = 0;
    memset(cloud_ack_bitmap_, 0, sizeof(cloud_ack_bitmap_));
    cloud_base_seq_ = 0;
    cloud_next_send_ = 0;
    cloud_retx_count_ = 0;
    last_cloud_activity_ms_ = 0;
    mesh_upload_done_ = false;
    last_meta_publish_ms_ = 0;
    exit_pending_ = false;

    for (uint8_t i = 0; i < MAX_EXIT_NODES; i++)
    {
        transfer_.exit_nodes[i] = 0;
        transfer_.exit_node_alive[i] = true;
        transfer_.last_ack_ms[i] = 0;
        path_stats_[i] = ExitPathStats{};
        arq_[i].reset_sender();
    }

    if (finished_session != 0 && mqtt_session_end_fn_)
    {
        mqtt_session_end_fn_(finished_session);
    }

    if (signal_complete && events_)
    {
        xEventGroupSetBits(events_, FLP_EVT_TRANSFER_COMPLETE);
    }
}

void TransferEngine::clear_exit_node_state()
{
    uint16_t finished_session = active_session_id_;

    is_exit_node_ = false;
    exit_pending_ = false;
    active_session_id_ = 0;
    source_addr_ = 0;
    transfer_.size = 0;
    transfer_.crc32 = 0;
    transfer_.fragment_count = 0;
    transfer_.fragment_size = 0;
    transfer_.filename[0] = '\0';
    last_cloud_activity_ms_ = 0;
    last_meta_publish_ms_ = 0;
    congestion_backoff_ticks_ = 0;
    oow_retx_count_ = 0;
    if (finished_session != 0 && mqtt_session_end_fn_)
    {
        mqtt_session_end_fn_(finished_session);
    }
}

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
                   size_t len) -> int
            {
                return send_fn_(dst, type, data, len, seq);
            });
    }

    ESP_LOGI(TAG, "TransferEngine initialized");
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

/* Session consensus: cloud confirmed transfer complete via exit node.
 * The source marks its transfer as done and resets state.  This
 * prevents zombie sessions where the source keeps retransmitting
 * into a completed cloud session. */
void TransferEngine::handle_transfer_done(uint16_t sender,
                                         const uint8_t *payload,
                                         size_t payload_len)
{
    if (!transfer_.active)
    {
        ESP_LOGD(TAG, "TRANSFER_DONE from 0x%04X but no active transfer",
                 sender);
        return;
    }

    if (payload_len < sizeof(TransferDonePayload))
    {
        ESP_LOGW(TAG, "TRANSFER_DONE from 0x%04X missing payload", sender);
        return;
    }

    TransferDonePayload done = {};
    memcpy(&done, payload, sizeof(done));

    if (done.session_id != transfer_.session_id)
    {
        ESP_LOGW(TAG,
                 "TRANSFER_DONE session mismatch from 0x%04X: got=%u expected=%u",
                 sender,
                 done.session_id,
                 transfer_.session_id);
        return;
    }

    if (sender != done.exit_node_addr || arq_index_for_peer(done.exit_node_addr) < 0)
    {
        ESP_LOGW(TAG,
                 "TRANSFER_DONE from invalid exit 0x%04X (payload exit=0x%04X)",
                 sender,
                 done.exit_node_addr);
        return;
    }

    ESP_LOGI(TAG, "TRANSFER_DONE from 0x%04X — cloud confirmed complete, "
             "stopping transfer",
             sender);

    reset_sender_transfer_state(true);
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

    uint8_t effective_hops_to_exit = sanitize_hops_to_exit(hops_to_internet);

    /*
     * Fragment size is derived from the current mesh MTU and aligned to the
     * d8 wire encoding used in TransferAdPayload.
     */
    size_t frag_payload = DEFAULT_TRANSFER_FRAGMENT_SIZE;

    transfer_.read_chunk = read_chunk;
    transfer_.size = size;
    transfer_.fragment_size = static_cast<uint16_t>(frag_payload);
    uint16_t data_frags =
        static_cast<uint16_t>((size + frag_payload - 1) / frag_payload);
    transfer_.next_fragment = 0;
    transfer_.active = true;
    uint16_t raw = static_cast<uint16_t>(esp_random());
    transfer_.session_id = (raw != 0) ? raw : 1;
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
    source_hops_to_exit_est_ = effective_hops_to_exit;
    exit_node_timeout_ms_ = compute_exit_timeout_ms(effective_hops_to_exit);

    /* Local-exit fast path: source node has internet + MQTT, skip mesh entirely */
    local_exit_ = (has_internet && has_mqtt && forward_to_mqtt_fn_);
    if (local_exit_)
    {
        source_hops_to_exit_est_ = 0;
        exit_node_timeout_ms_ = BASE_EXIT_NODE_TIMEOUT_MS;
        /* No FEC parity needed — no lossy channel */
        if (data_frags > MAX_CLOUD_FRAGMENTS)
        {
            ESP_LOGE(TAG,
                     "Local-exit transfer rejected: %u fragments exceeds bitmap capacity %u",
                     data_frags, MAX_CLOUD_FRAGMENTS);
            transfer_.active = false;
            return;
        }
        transfer_.fragment_count = data_frags;
        if (transfer_.fragment_count > 60000)
        {
            ESP_LOGE(TAG,
                     "Transfer rejected: %u fragments too close to uint16_t wrap",
                     transfer_.fragment_count);
            transfer_.active = false;
            return;
        }

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
    if (transfer_.fragment_count > 60000)
    {
        ESP_LOGE(TAG,
                 "Transfer rejected: %u fragments too close to uint16_t wrap",
                 transfer_.fragment_count);
        transfer_.active = false;
        return;
    }

    ESP_LOGI(TAG,
             "Starting file transfer: %s (%zu bytes, %u fragments, session=%u, est_exit_hops=%u)",
             filename,
             size,
             transfer_.fragment_count,
             transfer_.session_id,
             effective_hops_to_exit);

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

    election_timeout_ms_ = compute_election_timeout_ms(effective_hops_to_exit);

    /* P8: Adaptive ARQ timeout — increase for multi-hop to avoid
     * spurious retransmissions when round-trip exceeds 700ms. */
    uint32_t arq_timeout = compute_arq_timeout_ms(effective_hops_to_exit);
    for (int i = 0; i < MAX_EXIT_NODES; i++)
    {
        arq_[i].set_timeout(arq_timeout);
    }

    ESP_LOGI(TAG,
             "Transfer timers: election=%" PRIu32 "ms arq=%" PRIu32 "ms exit_timeout=%" PRIu32 "ms",
             election_timeout_ms_,
             arq_timeout,
             exit_node_timeout_ms_);

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
    /* ARQ timeout retransmits — tick ALL instances.
     * Flow control is handled by the ESP-NOW TX semaphore: send()
     * returns -1 when no TX slots are available, and tick() stops
     * on the first failure.  Per-instance cap (8) prevents one ARQ
     * from starving others. */
    for (uint8_t i = 0; i < MAX_EXIT_NODES; i++)
    {
        arq_[i].tick(8);
    }

    /* Abort transfer if any ARQ instance has fatally failed */
    if (transfer_.active && !local_exit_)
    {
        for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
        {
            if (arq_[i].is_sender_failed())
            {
                ESP_LOGE(TAG, "Transfer aborted: ARQ[%u] max retries exceeded", i);
                reset_sender_transfer_state(false);
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
            reset_sender_transfer_state(true);
            return;
        }
        if (mesh_upload_done_ && transfer_.active && !local_exit_)
        {
            ESP_LOGW(TAG,
                     "Post-upload cloud stall: no activity for %" PRIu32
                     "ms after mesh upload, aborting transfer",
                     now_ms - last_cloud_activity_ms_);
            reset_sender_transfer_state(false);
            return;
        }
        if (is_exit_node_)
        {
            ESP_LOGW(TAG,
                     "Exit node cloud stall: no activity for %" PRIu32
                     "ms, clearing exit-node role",
                     now_ms - last_cloud_activity_ms_);
            clear_exit_node_state();
        }
    }

    /* D1+B3 fix: exit node drains cloud transfer NACKs and forwards as
     * mesh NACKs to source, bridging the end-to-end reliability gap. */
    if (is_exit_node_ && drain_cloud_nack_fn_)
    {
        uint16_t nack_seq = 0;
        while (drain_cloud_nack_fn_(active_session_id_, nack_seq))
        {
            send_fn_(source_addr_, PacketType::NACK, nullptr, 0, nack_seq);
            ESP_LOGI(TAG, "Exit->source NACK for seq=%u", nack_seq);
        }
    }

    /* Deferred fragment ACK: MQTT task accepted a fragment into the local
     * MQTT client/outbox. Send the mesh ACK back to the source now. This
     * closes the backpressure loop without waiting for broker PUBACK on every
     * fragment. */
    if (is_exit_node_ && drain_fragment_ack_fn_)
    {
        uint16_t ack_seq = 0;
        while (drain_fragment_ack_fn_(active_session_id_, ack_seq))
        {
            int rc = send_fn_(source_addr_, PacketType::ACK,
                              nullptr, 0, ack_seq);
            if (rc < 0)
            {
                ESP_LOGD(TAG, "Deferred ACK paused: TX slots full (seq=%u)",
                         ack_seq);
                break; /* TX slots full — drain more next tick */
            }
            ESP_LOGD(TAG, "Deferred ACK for seq=%u", ack_seq);
        }
    }

    /* Session consensus: if cloud published TRANSFER_COMPLETE,
     * forward TRANSFER_DONE to the source via mesh so it stops sending. */
    if (is_exit_node_ && transfer_complete_fn_ &&
        transfer_complete_fn_(active_session_id_))
    {
        uint16_t finished_session = active_session_id_;
        TransferDonePayload done_payload = {};
        done_payload.session_id = active_session_id_;
        done_payload.exit_node_addr = my_addr_;
        ESP_LOGI(TAG, "Cloud TRANSFER_COMPLETE → sending TRANSFER_DONE to source 0x%04X",
                 source_addr_);
        send_fn_(source_addr_, PacketType::TRANSFER_DONE,
                 reinterpret_cast<const uint8_t *>(&done_payload),
                 sizeof(done_payload), 0);
        clear_exit_node_state();
        ESP_LOGI(TAG, "Cleared exit-node state for completed session=%u",
                 finished_session);
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
        ESP_LOGW(TAG,
                 "Election timeout: no exit node candidates (est_exit_hops=%u, timeout=%" PRIu32 "ms)",
                 source_hops_to_exit_est_,
                 election_timeout_ms_);
        reset_sender_transfer_state(false);
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
                        static_cast<int16_t>(6 * candidates_[i].hops_to_gw) -
                        static_cast<int16_t>(32 * candidates_[i].active_transfers);
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

        /* 3. Accept candidates within quality threshold. For multi-hop source
         * paths, keep the first rollout conservative and cap the session to a
         * single exit to avoid striping across delayed relay paths. */
        int16_t threshold = best_score / 2;
        uint8_t accepted = 0;
        uint8_t accept_limit =
            (source_hops_to_exit_est_ >= 2) ? 1 : MAX_EXIT_NODES;
        if (source_hops_to_exit_est_ >= 2 && candidate_count_ > 1)
        {
            ESP_LOGI(TAG,
                     "Multi-hop source path (%u hops): capping exit election to 1 candidate",
                     source_hops_to_exit_est_);
        }
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        for (uint8_t i = 0; i < candidate_count_ && accepted < accept_limit; i++)
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
                    static_cast<float>(compute_arq_timeout_ms(candidates_[i].hops_to_gw));

                accepted++;
                ESP_LOGI(TAG,
                         "Accepted exit #%u: 0x%04X score=%d active=%u rtt_prior=%.0f",
                         accepted,
                         candidates_[i].addr,
                         scores[i],
                         candidates_[i].active_transfers,
                         path_stats_[accepted - 1].ewma_rtt_ms);
            }
            else
            {
                ESP_LOGI(TAG, "Rejected exit 0x%04X: score=%d < threshold=%d",
                         candidates_[i].addr, scores[i], threshold);
            }
        }

        if (accepted == 0 && candidate_count_ > 0)
        {
            uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            transfer_.exit_nodes[0] = candidates_[0].addr;
            transfer_.exit_node_alive[0] = true;
            transfer_.last_ack_ms[0] = now;
            arq_[0].reset_sender();
            arq_[0].set_peer_addr(candidates_[0].addr);
            path_stats_[0] = ExitPathStats{};
            path_stats_[0].ewma_rtt_ms =
                static_cast<float>(compute_arq_timeout_ms(candidates_[0].hops_to_gw));
            accepted = 1;
            ESP_LOGW(TAG,
                     "No exits met threshold; falling back to best candidate 0x%04X score=%d",
                     candidates_[0].addr,
                     scores[0]);
        }

        transfer_.exit_node_count = accepted;
        weight_recompute_counter_ = 0;

        /* Set stride for multi-exit ARQ ownership */
        for (uint8_t i = 0; i < accepted; i++)
        {
            arq_[i].set_exit_stride(accepted, i);
        }

        ESP_LOGI(TAG,
                 "Elected %u exit nodes (of %u candidates, est_exit_hops=%u)",
                 accepted,
                 candidate_count_,
                 source_hops_to_exit_est_);

        xEventGroupSetBits(events_, FLP_EVT_EXIT_NODE_ELECTED);
        transfer_.next_fragment = 0;
    }
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
