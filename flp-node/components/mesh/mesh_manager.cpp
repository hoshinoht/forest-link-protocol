/*
 * =============================================================================
 * mesh_manager.cpp — Role 2: ESP32 Mesh Brain
 *
 * Lifecycle / init / main loop implementation.
 * Packet routing, bridge, and send-path logic live in dedicated files.
 * =============================================================================
 */

#include "mesh_manager.hpp"

#include <cinttypes>
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "itransport.hpp"
#include "mqtt_client.hpp"
#include "nvs.h"
#include "time_anchor.hpp"

using namespace flp;

static const char *TAG = "mesh_mgr";

namespace
{
constexpr uint8_t kNodeMacLowByteIdx = 5;
constexpr uint8_t kNodeMacHighByteIdx = 4;
constexpr uint8_t kMaxQueueDrainPerLoop = 64;
constexpr uint32_t kQueueWaitMs = 20;
constexpr uint32_t kDiscoveryIntervalMs = 10000;
constexpr uint32_t kPruneIntervalMs = 5000;
constexpr uint32_t kNeighborStaleTimeoutMs = 30000;
constexpr uint32_t kEarlyStaleTimeoutMs = 15000;
constexpr uint32_t kBiasIntervalMs = 10000;
constexpr uint32_t kTelemetryIntervalMs = 30000;
constexpr uint32_t kChannelHopIntervalMs = 1500;
constexpr uint8_t kMaxWifiChannels = 13;
/* How often to log the current anchor / extrapolated slot for
 * convergence debugging. */
constexpr uint32_t kAnchorLogIntervalMs = 10000;
} /* namespace */

void MeshManager::set_has_internet(bool v)
{
    if (has_internet_ != v)
    {
        /*
         * Defer EXIT_OFFLINE broadcast to the mesh task.  This function
         * runs on the WiFi event loop task; calling send_raw() here risks
         * priority inversion (same bug class as Fix 1 and Fix 7).
         */
        if (!v && has_internet_)
        {
            exit_offline_pending_.store(true, std::memory_order_release);
        }
        has_internet_ = v;
        ESP_LOGI(TAG, "has_internet_ = %s", v ? "true" : "false");
    }
}

void MeshManager::broadcast_exit_offline()
{
    uint16_t session = transfer_engine_.active_session_id();

    ExitOfflinePayload eop = {};
    eop.session_id = session;
    eop.exit_node_addr = my_addr_;

    uint8_t *buf = scratch_buf_;
    PacketHeader hdr = {};
    hdr.set_ver_type(PROTOCOL_VERSION, PacketType::EXIT_OFFLINE);
    hdr.src_addr = my_addr_;
    hdr.dst_addr = EXIT_ANY_ADDR; /* NOT BROADCAST — must enter forwarding path */
    hdr.set_ttl_hops(DEFAULT_TTL, 0);
    hdr.seq_num = 0;
    memcpy(buf, &hdr, PACKET_HEADER_SIZE);
    memcpy(buf + PACKET_HEADER_SIZE, &eop, sizeof(eop));
    size_t total = PACKET_HEADER_SIZE + sizeof(eop);

    send_raw(Transport::ESPNOW, buf, total, BROADCAST_ADDR);
    send_raw(Transport::LORA, buf, total, BROADCAST_ADDR);

    ESP_LOGW(TAG,
             "EXIT_OFFLINE broadcast: session=%u addr=0x%04X",
             session, my_addr_);
}

bool MeshManager::is_mqtt_connected() const
{
    return mqtt_client_ && mqtt_client_->is_connected();
}

void MeshManager::update_espnow_broadcast_peer()
{
    espnow_.update_broadcast_peer();
}

void MeshManager::init()
{
    /* Derive node address from MAC (lower 16 bits) */
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    my_addr_ = static_cast<uint16_t>((mac[kNodeMacHighByteIdx] << 8) |
                                     mac[kNodeMacLowByteIdx]);
    ESP_LOGI(TAG, "Node addr: 0x%04X", my_addr_);

    /*
     * Load and bump the gateway incarnation counter from NVS. This makes
     * every reboot of an exit node visible to the rest of the mesh as a
     * monotonically increasing value, which fences stale DSDV state held
     * by neighbors that did not see the reboot (commit 30c36212 race fix).
     *
     * NVS namespace "flp" is created on first write. We persist a 32-bit
     * value for monotonicity across many reboots even though the wire
     * field is 8-bit (compared via inc_newer / RFC1982).
     */
    {
        nvs_handle_t flp_nvs = 0;
        esp_err_t err = nvs_open("flp", NVS_READWRITE, &flp_nvs);
        if (err == ESP_OK)
        {
            uint32_t stored = 0;
            nvs_get_u32(flp_nvs, "incarnation", &stored); /* leaves 0 on miss */
            stored++;
            esp_err_t set_err = nvs_set_u32(flp_nvs, "incarnation", stored);
            if (set_err == ESP_OK)
            {
                nvs_commit(flp_nvs);
            }
            else
            {
                ESP_LOGW(TAG, "nvs_set incarnation failed: %s",
                         esp_err_to_name(set_err));
            }
            my_incarnation_ = static_cast<uint8_t>(stored & 0xFF);
            ESP_LOGI(TAG,
                     "gw incarnation = %u (boot count %" PRIu32 ")",
                     (unsigned) my_incarnation_,
                     stored);
            nvs_close(flp_nvs);
        }
        else
        {
            ESP_LOGW(TAG,
                     "nvs_open(\"flp\") failed: %s — incarnation will be 0",
                     esp_err_to_name(err));
        }
    }

    /* Try to load the persisted hop seed. If present, has_seed_ becomes
     * true and the slotted hopping path will be used (in commit 5);
     * otherwise we fall back to the legacy linear channel scan until
     * either MQTT delivers a fresh anchor or a seeded peer broadcasts
     * one to us. */
    load_hop_seed_from_nvs();

    /* Init buffer pool */
    buffer_pool_.init();

    /* Step 8: Create dual-priority queues */
    hi_pri_queue_ = xQueueCreate(kQueueDepth, sizeof(BufferSlab *));
    lo_pri_queue_ = xQueueCreate(kQueueDepth, sizeof(BufferSlab *));
    assert(hi_pri_queue_);
    assert(lo_pri_queue_);

    /* Legacy single queue kept for get_packet_queue() compatibility */
    packet_queue_ = hi_pri_queue_;

    /* Create event group */
    events_ = xEventGroupCreate();
    assert(events_);

    display_mutex_ = xSemaphoreCreateMutex();
    assert(display_mutex_);
    display_snapshot_.filename = display_filename_buf_;
    display_filename_buf_[0] = '\0';
    display_snapshot_.topic_msg = display_topic_msg_buf_;
    display_topic_msg_buf_[0] = '\0';

    /* Pass dual queues and buffer pool to transports */
    espnow_.set_packet_queue(hi_pri_queue_); /* fallback */
    espnow_.set_hi_pri_queue(hi_pri_queue_);
    espnow_.set_lo_pri_queue(lo_pri_queue_);
    espnow_.set_buffer_pool(&buffer_pool_);
    lora_.set_packet_queue(hi_pri_queue_); /* fallback */
    lora_.set_hi_pri_queue(hi_pri_queue_);
    lora_.set_lo_pri_queue(lo_pri_queue_);
    lora_.set_buffer_pool(&buffer_pool_);

    /* Init transports (WiFi must already be started for ESP-NOW) */
    espnow_.init();
    lora_.init(lora_rx_priority_);

    /* Init protocol selector */
    protocol_selector_.init();

    /* Init transfer engine */
    transfer_engine_.init(
        events_,
        my_addr_,
        [this](uint16_t dst,
               PacketType type,
               const uint8_t *payload,
               size_t payload_len,
               uint16_t seq_num) -> int
        {
            if (type == PacketType::ACK || type == PacketType::NACK)
            {
                bool congested = buffer_pool_.is_congested() ||
                                 (mqtt_client_ && mqtt_client_->is_fragment_queue_congested());
                seq_num = seq_with_congestion(seq_num, congested);
            }
            int rc = send_packet(dst, type, payload, payload_len, seq_num);
            if (rc == TRANSPORT_SEND_BACKPRESSURE)
            {
                transfer_engine_.note_local_backpressure();
            }
            return rc;
        });

    /* Wire up fragment forwarding to MQTT (returns false if queue full) */
    transfer_engine_.set_forward_to_mqtt(
        [this](uint16_t session_id,
               uint16_t seq,
               uint16_t src_node,
               const uint8_t *data,
               size_t len,
               const char *filename) -> bool
        {
            if (mqtt_client_)
            {
                return mqtt_client_->publish_fragment(
                    session_id, seq, src_node, data, len, filename);
            }
            return false;
        });

    /* Wire up cloud ACK/NACK drain for local-exit selective repeat */
    transfer_engine_.set_cloud_ack_drain(
        [this](uint16_t session_id, uint16_t &seq_out) -> bool
        {
            if (mqtt_client_)
            {
                return mqtt_client_->drain_cloud_ack(session_id, seq_out);
            }
            return false;
        });
    transfer_engine_.set_cloud_nack_drain(
        [this](uint16_t session_id, uint16_t &seq_out) -> bool
        {
            if (mqtt_client_)
            {
                return mqtt_client_->drain_cloud_nack(session_id, seq_out);
            }
            return false;
        });
    transfer_engine_.set_cloud_nack_requeue(
        [this](uint16_t session_id, uint16_t seq)
        {
            if (mqtt_client_)
            {
                mqtt_client_->requeue_cloud_nack(session_id, seq);
            }
        });
    transfer_engine_.set_cloud_nack_observer(
        [this](uint16_t session_id, uint16_t seq)
        {
            if (mqtt_client_)
            {
                mqtt_client_->mark_fragment_for_republish(session_id, seq);
            }
        });

    /* Wire up deferred fragment ACK drain (exit node: ACK after MQTT publish) */
    transfer_engine_.set_fragment_ack_drain(
        [this](uint16_t session_id, uint16_t &seq_out) -> bool
        {
            if (mqtt_client_)
            {
                return mqtt_client_->drain_fragment_ack(session_id, seq_out);
            }
            return false;
        });
    transfer_engine_.set_fragment_ack_requeue(
        [this](uint16_t session_id, uint16_t seq)
        {
            if (mqtt_client_)
            {
                mqtt_client_->requeue_fragment_ack(session_id, seq);
            }
        });

    /* Wire up session consensus: cloud TRANSFER_COMPLETE → exit → source.
     * Exit node polls MqttClient's flag each tick. */
    transfer_engine_.set_transfer_complete_fn(
        [this](uint16_t session_id) -> bool
        {
            if (mqtt_client_)
            {
                return mqtt_client_->consume_transfer_complete(session_id);
            }
            return false;
        });

    transfer_engine_.set_mqtt_session_end_fn(
        [this](uint16_t session_id)
        {
            if (mqtt_client_)
            {
                mqtt_client_->clear_transfer_session(session_id);
            }
        });

    /* Wire up transfer meta forwarding (exit node publishes complete meta) */
    transfer_engine_.set_forward_meta(
        [this](uint32_t session_id,
               const char *filename,
               uint16_t src_node,
               uint32_t total_size,
               uint16_t chunk_count,
               uint16_t fragment_size,
               uint32_t crc32)
        {
            if (mqtt_client_)
            {
                mqtt_client_->publish_transfer_meta(session_id,
                                                    filename,
                                                    src_node,
                                                    my_addr_,
                                                    total_size,
                                                    chunk_count,
                                                    fragment_size,
                                                    crc32);
            }
        });

    discovery_timer_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    /* Init channel hop state from current WiFi channel */
    {
        wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&current_channel_, &sec);
        channel_hop_idx_ = current_channel_;
        channel_hop_timer_ms_ = discovery_timer_ms_;
    }

    /* Parse blocked peer addresses from Kconfig (comma-separated hex, e.g. "A1B2, C3D4") */
    {
        const char *raw = CONFIG_FLP_BLOCKED_PEER;
        if (raw && raw[0] != '\0')
        {
            char buf[128];
            strncpy(buf, raw, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            char *saveptr = nullptr;
            char *tok = strtok_r(buf, ", ", &saveptr);
            while (tok)
            {
                unsigned long val = strtoul(tok, nullptr, 16);
                if (val > 0 && val <= 0xFFFF)
                {
                    blocked_peers_.push_back(static_cast<uint16_t>(val));
                    ESP_LOGW(TAG,
                             "Peer blacklist active: dropping direct packets "
                             "from 0x%04X",
                             static_cast<uint16_t>(val));
                }
                tok = strtok_r(nullptr, ", ", &saveptr);
            }
        }
    }

    /* Init heap monitor */
    heap_monitor_.init();

    ESP_LOGI(TAG, "MeshManager initialized");
}

void MeshManager::snapshot_display_state(NodeStatus &out) const
{
    if (display_mutex_ &&
        xSemaphoreTake(display_mutex_, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        out = display_snapshot_;
        xSemaphoreGive(display_mutex_);
    }
}

void MeshManager::run()
{
    while (true)
    {
        /*
         * Step 8: Drain high-priority queue first, then low-priority.
         * NFR-MESH2: Interrupt-driven via FreeRTOS queues, no polling.
         */
        BufferSlab *slab = nullptr;
        uint8_t drain = 0;

        /* Always drain high-priority queue first */
        while (drain < kMaxQueueDrainPerLoop)
        {
            TickType_t wait = (drain == 0) ? pdMS_TO_TICKS(10) : 0;
            if (xQueueReceive(hi_pri_queue_, &slab, wait) == pdTRUE)
            {
                process_slab(slab);
                buffer_pool_.release(slab);
                drain++;
            }
            else
            {
                break;
            }
        }

        /* Then drain low-priority queue */
        while (drain < kMaxQueueDrainPerLoop)
        {
            TickType_t wait = (drain == 0) ? pdMS_TO_TICKS(kQueueWaitMs) : 0;
            if (xQueueReceive(lo_pri_queue_, &slab, wait) == pdTRUE)
            {
                process_slab(slab);
                buffer_pool_.release(slab);
                drain++;
            }
            else
            {
                break;
            }
        }

        /*
         * Fix 1: Drain deferred ESP-NOW peer registrations here in the mesh
         * task — NOT inside on_recv (WiFi driver task) to avoid deadlock on
         * ESP-NOW internal locks.
         */
        espnow_.drain_pending_peers();

        /*
         * Fix 7: Drain deferred ESP-NOW broadcast peer update requested by
         * the WiFi event callback. The actual del/add peer calls must happen
         * here (mesh task) — not inside the WiFi event loop task.
         */
        if (espnow_peer_update_pending_.exchange(false,
                                                 std::memory_order_acq_rel))
        {
            espnow_.update_broadcast_peer();
        }

        /* Drain deferred EXIT_OFFLINE broadcast (same pattern as Fix 7) */
        if (exit_offline_pending_.exchange(false, std::memory_order_acq_rel))
        {
            broadcast_exit_offline();
        }

        /* Periodic tasks */
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);

        /* Discovery broadcast every 10 seconds */
        uint32_t effective_disc_interval =
            transfer_engine_.is_transfer_active()
                ? kDiscoveryIntervalMs * 3
                : kDiscoveryIntervalMs;
        if (now - discovery_timer_ms_ > effective_disc_interval)
        {
            send_discovery();
            discovery_timer_ms_ = now;
        }

        /*
         * Prune stale neighbors (30s timeout) — only every 5 seconds to
         * avoid O(n) scan on every loop iteration.
         * Step 2c: Also detect early stale (15s) for route error.
         */
        if (now - prune_timer_ms_ > kPruneIntervalMs)
        {
            /* Step 2c: Detect early stale neighbors and send ROUTE_ERROR */
            route_table_.detect_early_stale(kEarlyStaleTimeoutMs,
                [this](uint16_t dead_addr, uint16_t inet_origin, uint16_t seq) {
                    send_route_error(dead_addr, inet_origin, seq);
                });

            route_table_.prune_stale(kNeighborStaleTimeoutMs);
            prune_timer_ms_ = now;
        }

        /* Recalculate protocol bias every 10s + Step 7: ADR */
        if (now - bias_timer_ms_ > kBiasIntervalMs)
        {
            protocol_selector_.recalculate_bias();
            heap_monitor_.periodic_check();

            /* Fix 12: Log buffer pool exhaustion count (safe to log here —
             * mesh task context, not WiFi driver task). */
            uint32_t exhaustions = buffer_pool_.get_exhaustion_count();
            if (exhaustions > 0)
            {
                ESP_LOGW(TAG,
                         "Buffer pool exhausted %lu time(s) total",
                         exhaustions);
            }

            /* Periodic system state summary for diagnostics */
            ESP_LOGI(TAG,
                     "state: tx_slots=%u neighbors=%u transfer=%s",
                     espnow_.get_tx_slots_available(),
                     route_table_.get_count(),
                     transfer_engine_.is_transfer_active()
                         ? transfer_engine_.current_filename()
                         : "idle");

            /* Step 7: ADR-inspired adaptive spreading factor */
            uint8_t target_sf = 7;
            float lora_success = protocol_selector_.lora_success_rate();
            if (lora_success < 0.3f && route_table_.get_count() < 2)
            {
                target_sf = 10;
            }
            else if (lora_success < 0.6f)
            {
                target_sf = 9;
            }
            else if (lora_success > 0.9f && route_table_.get_count() >= 3)
            {
                target_sf = 7;
            }

            if (lora_.is_initialized() && target_sf != lora_.get_spreading_factor())
            {
                lora_.set_spreading_factor(target_sf);
                ESP_LOGI(TAG, "ADR: SF changed to %u (success=%.0f%%, neighbors=%u)",
                         target_sf, lora_success * 100, route_table_.get_count());
            }

            bias_timer_ms_ = now;
        }

        /*
         * Drain any hop schedule anchors the MQTT client received from
         * cloud. Only meaningful on exit nodes that actually have a
         * working uplink, but the drain is cheap on relays (queue is
         * always empty). Each drained item updates anchor_ + persists
         * the seed if it changed.
         */
        if (mqtt_client_)
        {
            CloudEpochItem ep_item = {};
            while (mqtt_client_->drain_cloud_epoch(ep_item))
            {
                apply_cloud_epoch(ep_item);
            }
        }

        /*
         * Periodic [anchor] log line for convergence debugging — once
         * the mesh starts hopping for real (commit 5) this will let us
         * verify that all nodes agree on the same slot at the same
         * wall-clock instant. No-op until we have a seed.
         */
        if (has_seed_ && (now - anchor_log_timer_ms_ > kAnchorLogIntervalMs))
        {
            uint64_t slot = time_anchor::current_slot(anchor_, now);
            uint8_t  ch = time_anchor::wifi_channel_for_slot(hop_seed_, slot);
            uint32_t fr = time_anchor::lora_freq_for_slot(hop_seed_, slot);
            ESP_LOGI(TAG,
                     "[anchor] epoch=%llu inc=%u origin=0x%04X slot=%llu "
                     "wifi_ch=%u lora_freq=%lu",
                     (unsigned long long) anchor_.cloud_epoch,
                     (unsigned) anchor_.cloud_incarnation,
                     anchor_.origin_node,
                     (unsigned long long) slot,
                     (unsigned) ch,
                     (unsigned long) fr);
            anchor_log_timer_ms_ = now;
        }

        /*
         * Publish topology + metrics + heap every 30s via relay.
         * relay_publish() handles both exit nodes (direct MQTT) and
         * deep-field nodes (MESH_PUB routed through mesh to exit).
         */
        if (now - topo_metrics_timer_ms_ > kTelemetryIntervalMs)
        {
            publish_all_telemetry();
            topo_metrics_timer_ms_ = now;
        }

        /*
         * Channel hopping for relay nodes: if we have no route to the
         * internet, cycle through WiFi channels 1-13 to find the
         * gateway's ESP-NOW channel.  Sends a discovery probe on each
         * channel to trigger an immediate response from any exit node.
         * Stops automatically once a neighbor with internet is found.
         */
        if (!has_internet_ &&
            route_table_.min_hops_to_internet() >= ROUTE_HOPS_UNKNOWN)
        {
            /*
             * Fast channel scan: dwell 1.5s per channel so a full
             * 13-channel sweep completes in ~20s instead of ~65s.
             * This ensures the relay finds the exit node quickly even
             * if the exit boots while the relay is scanning a different
             * channel.  Once a neighbor with internet is found, hopping
             * stops and the relay stays on that channel.
             */
            if (now - channel_hop_timer_ms_ > kChannelHopIntervalMs)
            {
                channel_hop_idx_ = (channel_hop_idx_ % kMaxWifiChannels) + 1;
                esp_wifi_set_channel(channel_hop_idx_, WIFI_SECOND_CHAN_NONE);
                current_channel_ = channel_hop_idx_;
                ESP_LOGI(TAG, "Channel hop: trying ch=%u", channel_hop_idx_);

                /* Probe immediately on the new channel */
                send_discovery();
                discovery_timer_ms_ = now;

                channel_hop_timer_ms_ = now;
            }
        }

        /* Drain inbound mesh commands from cloud (exit nodes only) */
        drain_cmd_queue();

        /* Transfer engine tick (ARQ, election, broadcast retry, fragment feed) */
        transfer_engine_.tick(now);

        /* Update display snapshot for the display task */
        if (display_mutex_ && xSemaphoreTake(display_mutex_, 0) == pdTRUE)
        {
            display_snapshot_.node_addr = my_addr_;
            display_snapshot_.wifi_connected = has_internet_;
            display_snapshot_.espnow_peers = espnow_.get_peer_count();
            display_snapshot_.neighbor_count = route_table_.get_count();
            display_snapshot_.control_hops_to_internet =
                get_control_hops_to_internet();
            display_snapshot_.data_hops_to_internet =
                get_data_hops_to_internet();
            display_snapshot_.transfer_active =
                transfer_engine_.is_transfer_active();
            display_snapshot_.transfer_pct = transfer_engine_.get_progress_pct();
            display_snapshot_.free_heap_kb = esp_get_free_heap_size() / 1024;
            display_snapshot_.uptime_s =
                static_cast<uint32_t>(esp_timer_get_time() / 1000000);
            display_snapshot_.cloud_cmd_received = has_recent_cloud_cmd();
            display_snapshot_.config_cmd_received = has_recent_config_cmd();
            display_snapshot_.topic_msg_received = has_recent_topic_msg();
            display_snapshot_.topic_msg = display_topic_msg_buf_;

            const char *fn = transfer_engine_.current_filename();
            if (fn)
            {
                strncpy(display_filename_buf_, fn, sizeof(display_filename_buf_) - 1);
                display_filename_buf_[sizeof(display_filename_buf_) - 1] = '\0';
            }
            else
            {
                display_filename_buf_[0] = '\0';
            }
            display_snapshot_.filename = display_filename_buf_;

            xSemaphoreGive(display_mutex_);
        }
    }
}

/* ── FTSP-style hop schedule anchor: load / persist / merge / apply ──── */

void MeshManager::load_hop_seed_from_nvs()
{
    nvs_handle_t flp_nvs = 0;
    if (nvs_open("flp", NVS_READONLY, &flp_nvs) != ESP_OK)
    {
        return; /* first boot, or NVS not initialised — no seed yet */
    }
    size_t len = HOP_SEED_SIZE;
    esp_err_t err = nvs_get_blob(flp_nvs, "hop_seed", hop_seed_, &len);
    nvs_close(flp_nvs);
    if (err == ESP_OK && len == HOP_SEED_SIZE)
    {
        has_seed_ = true;
        ESP_LOGI(TAG, "[anchor] loaded persisted hop seed (32 bytes)");
    }
}

void MeshManager::persist_hop_seed()
{
    nvs_handle_t flp_nvs = 0;
    if (nvs_open("flp", NVS_READWRITE, &flp_nvs) != ESP_OK)
    {
        return;
    }
    if (nvs_set_blob(flp_nvs, "hop_seed", hop_seed_, HOP_SEED_SIZE) == ESP_OK)
    {
        nvs_commit(flp_nvs);
    }
    nvs_close(flp_nvs);
}

void MeshManager::merge_anchor(const TimeAnchor &recv,
                               const uint8_t *seed_in)
{
    /* Capture a freshly-broadcast seed before evaluating the merge so
     * the very first anchor we ever receive (which carries the seed)
     * unlocks slotted hopping immediately. */
    if (seed_in != nullptr && !has_seed_)
    {
        memcpy(hop_seed_, seed_in, HOP_SEED_SIZE);
        has_seed_ = true;
        persist_hop_seed();
        ESP_LOGI(TAG,
                 "[anchor] received hop seed from peer 0x%04X (origin 0x%04X)",
                 recv.origin_node, recv.origin_node);
    }

    if (!time_anchor::should_adopt(recv, anchor_))
    {
        return;
    }

    /* Adopt: copy the (incarnation, epoch, origin) tuple from the sender,
     * but re-stamp origin_local_ms with OUR own monotonic clock. The
     * field is purely a local extrapolation reference — see
     * time_anchor.hpp for the rationale. */
    anchor_ = recv;
    anchor_.origin_local_ms =
        static_cast<uint32_t>(esp_timer_get_time() / 1000);
    ESP_LOGD(TAG,
             "[anchor] adopted via mesh: epoch=%llu inc=%u origin=0x%04X",
             (unsigned long long) anchor_.cloud_epoch,
             (unsigned) anchor_.cloud_incarnation,
             anchor_.origin_node);
}

void MeshManager::apply_cloud_epoch(const CloudEpochItem &item)
{
    /* Always capture the seed from cloud — it is authoritative.
     * If our previously-held seed differs, replace it. */
    bool seed_changed =
        !has_seed_ ||
        memcmp(hop_seed_, item.seed, HOP_SEED_SIZE) != 0;
    if (seed_changed)
    {
        memcpy(hop_seed_, item.seed, HOP_SEED_SIZE);
        has_seed_ = true;
        persist_hop_seed();
        ESP_LOGI(TAG, "[anchor] persisted hop seed from cloud");
    }

    /* Build a TimeAnchor from the cloud message. Origin is OUR address
     * because we are the node that just touched cloud — downstream
     * peers will see this as our authoritative anchor. */
    TimeAnchor a = {};
    a.cloud_epoch = item.epoch;
    a.cloud_incarnation = item.incarnation;
    a.origin_node = my_addr_;
    a.origin_local_ms = item.received_at_local_ms;
    a.seed_version = 1;
    a.flags = 0;
    /* Run the merge so we respect any newer anchor we already had —
     * e.g. if a different node fetched a later epoch concurrently. */
    merge_anchor(a, nullptr); /* seed already handled above */
    ESP_LOGI(TAG,
             "[anchor] applied cloud epoch: epoch=%llu inc=%u slot_ms=%u",
             (unsigned long long) item.epoch,
             (unsigned) item.incarnation,
             (unsigned) item.slot_ms);
}
