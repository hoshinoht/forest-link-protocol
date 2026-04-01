#include "transfer_engine.hpp"

#include <cinttypes>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"

using namespace flp;

static const char *TAG = "xfer_eng";
constexpr EventBits_t FLP_EVT_TRANSFER_COMPLETE = BIT1;

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
        while (drain_cloud_ack_fn_(transfer_.session_id, ack_seq))
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
        while (drain_cloud_nack_fn_(transfer_.session_id, nack_seq))
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

        size_t offset = static_cast<size_t>(seq) * transfer_.fragment_size;
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
        size_t offset = static_cast<size_t>(seq) * transfer_.fragment_size;
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
        reset_sender_transfer_state(true);
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

    if (mesh_upload_done_)
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
        reset_sender_transfer_state(false);
        return;
    }

    /* Drain out-of-window retransmit queue (cloud NACKs for seqs the ARQ
     * has already advanced past).  Flow control is handled by the ESP-NOW
     * TX semaphore: send() returns -1 when no slots available, and we
     * keep the seq in the queue for retry on the next tick.
     *
     * Parity seqs (idx_in_group == FEC_GROUP_SIZE) are regenerated by
     * XOR-ing all data fragments in the group from the file cache. */
    if (oow_retx_count_ > 0 && transfer_.read_chunk)
    {
        uint8_t sent = 0;
        uint8_t kept = 0;
        for (uint8_t i = 0; i < oow_retx_count_; i++)
        {
            uint16_t seq = oow_retx_queue_[i];

            uint16_t group = seq / (FEC_GROUP_SIZE + 1);
            uint16_t idx_in_group = seq % (FEC_GROUP_SIZE + 1);
            bool is_parity = (idx_in_group == FEC_GROUP_SIZE);
            size_t got = 0;

            if (is_parity)
            {
                /* Regenerate parity: XOR all data fragments in the group */
                FecEncoder enc;
                enc.reset();
                bool ok = true;
                for (uint8_t d = 0; d < FEC_GROUP_SIZE; d++)
                {
                    uint16_t di = group * FEC_GROUP_SIZE + d;
                    size_t off =
                        static_cast<size_t>(di) * transfer_.fragment_size;
                    if (off >= transfer_.size)
                    {
                        ok = false;
                        break; /* incomplete trailing group */
                    }
                    size_t remain = transfer_.size - off;
                    size_t fl = (remain < transfer_.fragment_size)
                                    ? remain
                                    : transfer_.fragment_size;
                    size_t rd = transfer_.read_chunk(frag_buf_, off, fl);
                    enc.ingest(frag_buf_, rd);
                }
                if (!ok || enc.parity_len() == 0)
                {
                    continue; /* incomplete group, drop */
                }
                memcpy(frag_buf_, enc.parity_data(), enc.parity_len());
                got = enc.parity_len();
            }
            else
            {
                /* Data fragment: read directly from file cache */
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
                got = transfer_.read_chunk(frag_buf_, offset, frag_len);
            }

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
                /* Send failed (TX slots full) — keep in queue for next tick */
                oow_retx_queue_[kept++] = seq;
                ESP_LOGD(TAG, "OOW retx deferred: TX slots full (seq=%u)", seq);
                for (uint8_t j = i + 1; j < oow_retx_count_; j++)
                {
                    oow_retx_queue_[kept++] = oow_retx_queue_[j];
                }
                break;
            }
            ESP_LOGI(TAG, "Re-sent out-of-window seq=%u%s to 0x%04X",
                     seq, is_parity ? " (parity)" : "", dst);
            sent++;
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

    /* Time-based pacing: enforce a minimum interval between new fragment
     * sends so relay forwarding queues (48 slots each) are not overwhelmed.
     * Scales with hop count: more hops → more time for each relay to drain.
     * Retransmits (ARQ tick + OOW queue above) are NOT gated — they are
     * already rate-limited by their own budgets and backoff logic. */
    uint32_t now_pace = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    uint32_t pacing_interval_ms =
        10 + 5 * static_cast<uint32_t>(source_hops_to_exit_est_);
    if ((now_pace - last_mesh_frag_send_ms_) < pacing_interval_ms)
    {
        return; /* wait for pacing interval before sending new fragments */
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

    /* New fragment sends.  Flow control is handled by the ESP-NOW TX
     * semaphore — send() returns -1 when no slots available.
     *
     * Multi-hop throttle: each relay has a 48-slot queue; sending 48
     * fragments per tick overwhelms relay forwarding capacity, causing
     * queue drops and ARQ retransmit storms.  Scale the per-tick budget
     * down with estimated hop count so relay queues stay healthy. */
    uint8_t max_new = (source_hops_to_exit_est_ >= 2)
                          ? 1
                          : 2;
    const uint8_t MAX_NEW_FRAGS_PER_TICK = max_new;
    uint8_t new_sent = 0;
    while (transfer_.next_fragment < transfer_.fragment_count &&
           new_sent < MAX_NEW_FRAGS_PER_TICK)
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
        last_mesh_frag_send_ms_ = now_pace;
        new_sent++;
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
            mesh_upload_done_ = true;
            ESP_LOGI(TAG,
                     "Mesh upload drained for %s; waiting for cloud completion",
                     transfer_.filename);
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

        if ((now_ms - transfer_.last_ack_ms[i]) > exit_node_timeout_ms_)
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
