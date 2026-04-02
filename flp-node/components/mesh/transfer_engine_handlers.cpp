#include "transfer_engine.hpp"

#include <cinttypes>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"

using namespace flp;

static const char *TAG = "xfer_eng";

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

        /* Reject if already serving a DIFFERENT session — accepting would
         * overwrite the active session's state, orphaning the source that
         * is still expecting ACKs for the prior session.  The source's
         * session will eventually time out and retry with a fresh AD. */
        if ((is_exit_node_ || exit_pending_) && active_session_id_ != ad.session_id)
        {
            ESP_LOGW(TAG,
                     "Declining exit role: already serving session=%u "
                     "(requested=%u)",
                     active_session_id_, ad.session_id);
            return;
        }

        /* Dedup: if already set up for this session, just re-send ACK */
        if ((is_exit_node_ || exit_pending_) && active_session_id_ == ad.session_id)
        {
            TransferAckPayload ack = {};
            ack.session_id = ad.session_id;
            ack.exit_node_addr = my_addr_;
            ack.hops_to_gw = 0;
            ack.rssi_to_gw = 0;
            ack.active_transfers = 1;
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
        ack.active_transfers = 0;

        send_fn_(hdr.src_addr,
                 PacketType::TRANSFER_ACK,
                 reinterpret_cast<const uint8_t *>(&ack),
                 sizeof(ack),
                 0);

        /* Set up as exit node -- NO PSRAM allocation */
        exit_pending_ = true;
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

        ESP_LOGI(TAG,
                 "Exit node pending: session=%u source=0x%04X file=%s",
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
        uint8_t candidate_hops =
            (ack.hops_to_gw != 0) ? ack.hops_to_gw : source_hops_to_exit_est_;
        candidates_[candidate_count_] = {
            ack.exit_node_addr,
            ack.rssi_to_gw,
            candidate_hops,
            ack.active_transfers};
        candidate_count_++;
        ESP_LOGI(TAG,
                 "Exit candidate #%u: 0x%04X hops=%u rssi=%d active=%u",
                 candidate_count_,
                 ack.exit_node_addr,
                 candidate_hops,
                 ack.rssi_to_gw,
                 ack.active_transfers);
    }
}

void TransferEngine::handle_data(const PacketHeader &hdr,
                                 const uint8_t *payload,
                                 size_t payload_len)
{
    if (is_exit_node_ || exit_pending_)
    {
        if (exit_pending_ && !is_exit_node_ && active_session_id_ != 0)
        {
            is_exit_node_ = true;
            exit_pending_ = false;
            /* Publish transfer meta now that we know the source elected us */
            if (forward_meta_fn_)
            {
                forward_meta_fn_(active_session_id_,
                                 transfer_.filename,
                                 source_addr_,
                                 static_cast<uint32_t>(transfer_.size),
                                 transfer_.fragment_count,
                                 transfer_.fragment_size,
                                 transfer_.crc32);
            }
            ESP_LOGI(TAG,
                     "Exit node activated: session=%u source=0x%04X",
                     active_session_id_, source_addr_);
        }

        /*
         * Custody ACK architecture: enqueue fragment for MQTT publish but do
         * NOT send the mesh ACK inline here. The MQTT task pushes the seq into
         * fragment_ack_queue_ once esp_mqtt_client_publish() accepts it into
         * the local MQTT client/outbox. TransferEngine::tick() drains that
         * queue and sends mesh ACKs.
         *
         * This keeps the sender coupled to the exit node's local relay
         * capacity without waiting for broker PUBACK latency on every
         * fragment.
         *
         * If the MQTT publish queue is full, stay silent — the sender's ARQ
         * timeout will retransmit after the rate-limited delay.
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
            /* ACK is deferred — sent once local MQTT accepts the fragment */
            last_cloud_activity_ms_ =
                static_cast<uint32_t>(esp_timer_get_time() / 1000);
        }
        else
        {
            /* MQTT queue full — stay silent (no NACK).
             *
             * Sending an explicit NACK here causes a positive feedback loop:
             * NACK → immediate retransmit → queue still full → NACK → ...
             * which saturates both the mesh and MQTT channels, preventing
             * any forward progress.
             *
             * Instead, let the sender's ARQ timeout handle recovery with
             * exponential backoff, giving the MQTT queue time to drain.
             * signal_congestion() throttles the sender's new-fragment
             * feed rate via congestion_backoff_ticks_. */
            signal_congestion();
            ESP_LOGD(TAG, "MQTT queue full, suppressed NACK seq=%u (silent backpressure)",
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
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        uint16_t pre_base = arq_[idx].get_base_seq();
        uint16_t next_seq = arq_[idx].get_next_seq();

        arq_[idx].handle_ack(seq);

        if (seq >= pre_base && seq < next_seq)
        {
            /* Phase 3: compute RTT sample from send timestamp */
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
            transfer_.last_ack_ms[idx] = now;
        }
    }
}

void TransferEngine::handle_nack(uint16_t seq, uint16_t from_addr)
{
    int8_t idx = arq_index_for_peer(from_addr);

    /* Relay-originated congestion NACK: from_addr is the relay, not an exit
     * node, so arq_index_for_peer() returns -1.  Scan active ARQ instances
     * to find which one owns this seq.  This enables relay congestion echo
     * (Fix #1) where relays immediately NACK dropped DATA fragments. */
    if (idx < 0 && transfer_.active)
    {
        for (uint8_t i = 0; i < transfer_.exit_node_count; i++)
        {
            if (seq >= arq_[i].get_base_seq() && seq < arq_[i].get_next_seq())
            {
                idx = static_cast<int8_t>(i);
                break;
            }
        }
        if (idx < 0)
        {
            return;
        }
    }
    else if (idx < 0)
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
