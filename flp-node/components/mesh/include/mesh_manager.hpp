#pragma once

/*
 * =============================================================================
 * mesh_manager.hpp — Role 2: ESP32 Mesh Brain
 *
 * Implements:
 * FR-MESH4  — Parse intent packets, reply as exit node if we have MQTT
 * FR-MESH6  — Adaptive protocol selection (ESP-NOW vs LoRa)
 * FR-MESH7  — Intent broadcast retry, max 3 attempts
 * FR-MESH8  — Route packet to self (consume) or relay forward
 * NFR-MESH2 — Interrupt-driven via FreeRTOS queue (no polling)
 *
 * Calls into (does NOT implement):
 * EspNowTransport::send()    — Role 3
 * LoraTransport::send()      — Role 4
 * MqttClient::publish()      — Role 1
 * ProtocolSelector::select() — shared component
 * =============================================================================
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "buffer_pool.hpp"
#include "espnow_transport.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "heap_monitor.hpp"
#include "lora_transport.hpp"
#include "packet.hpp"
#include "protocol_selector.hpp"
#include "route_table.hpp"
#include "transfer_engine.hpp"

inline constexpr EventBits_t FLP_EVT_WIFI_CONNECTED = BIT0;
inline constexpr EventBits_t FLP_EVT_TRANSFER_COMPLETE = BIT1;
inline constexpr EventBits_t FLP_EVT_EXIT_NODE_ELECTED = BIT2;

namespace flp
{

class MqttClient; /* forward declaration */

class MeshManager
{
  public:
    MeshManager() = default;

    void init();
    void run(); /* main loop -- called from FreeRTOS task */

    /* Task 1: WiFi status wiring */
    void set_has_internet(bool v);

    /*
     * Fix 7: Schedule an ESP-NOW broadcast peer update to be executed from
     * the mesh task — NOT directly from the WiFi event callback which runs
     * on the event loop task and may hold WiFi driver internals.
     * Set this flag and let run() drain it safely.
     */
    void request_espnow_peer_update()
    {
        espnow_peer_update_pending_.store(true, std::memory_order_relaxed);
    }

    /* Direct call — only safe from the mesh task or during init */
    void update_espnow_broadcast_peer();

    /* Task 5: File transfer API */
    void start_file_transfer(const char *filename,
                             size_t size,
                             ReadChunkFn read_chunk);

    /* Task 6: MQTT bridge wiring */
    void set_mqtt_client(MqttClient *client)
    {
        mqtt_client_ = client;
    }

    void subscribe_topic(const char *topic);

    void set_lora_rx_priority(uint8_t p)
    {
        lora_rx_priority_ = p;
    }

    QueueHandle_t get_packet_queue() const
    {
        return packet_queue_;
    }
    uint16_t get_addr() const
    {
        return my_addr_;
    }
    EventGroupHandle_t get_events() const
    {
        return events_;
    }

    bool has_internet() const
    {
        return has_internet_.load();
    }
    bool is_mqtt_connected() const;
    uint8_t get_neighbor_count() const
    {
        return route_table_.get_count();
    }
    uint8_t get_espnow_peer_count() const
    {
        return espnow_.get_peer_count();
    }
    uint8_t get_hops_to_internet() const
    {
        if (has_internet_) { return 0; }
        const uint8_t min_h = route_table_.min_hops_to_internet();
        return (min_h < 0xFE) ? static_cast<uint8_t>(min_h + 1) : 0xFF;
    }
    bool has_recent_cloud_cmd() const
    {
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        return last_cloud_cmd_ms_ > 0 && (now - last_cloud_cmd_ms_) < 3000;
    }
    bool is_transfer_active() const
    {
        return transfer_engine_.is_transfer_active();
    }
    const char *get_transfer_filename() const
    {
        return transfer_engine_.current_filename();
    }
    uint8_t get_transfer_progress() const
    {
        return transfer_engine_.get_progress_pct();
    }

    int send_packet(uint16_t dst,
                    PacketType type,
                    const uint8_t *payload,
                    size_t payload_len,
                    uint16_t seq_num = 0);

  private:
    void process_slab(BufferSlab *slab);
    void handle_discovery(const PacketHeader &hdr,
                          RxTransport source,
                          const uint8_t *payload,
                          size_t payload_len);
    void handle_mesh_pub(const PacketHeader &hdr,
                         const uint8_t *payload,
                         size_t payload_len);
    void handle_mesh_cmd(const PacketHeader &hdr,
                         const uint8_t *payload,
                         size_t payload_len);
    void handle_route_error(const PacketHeader &hdr,
                            const uint8_t *payload,
                            size_t payload_len);
    void forward_packet(BufferSlab *slab, const PacketHeader &hdr);
    void send_discovery();
    int send_raw(Transport transport,
                 const uint8_t *data,
                 size_t len,
                 uint16_t peer_addr);
    void send_route_error(uint16_t dead_addr, uint16_t inet_origin,
                          uint16_t last_seq);
    void broadcast_exit_offline();

    /*
     * Generic relay: publishes via MQTT if exit node, else routes
     * through mesh to nearest exit node.
     */
    void relay_publish(uint8_t relay_topic, const uint8_t *data, size_t len);
    void publish_all_telemetry();
    void drain_cmd_queue();
    void handle_topic_msg(const uint8_t *data, size_t len);

    RouteTable route_table_;
    EspNowTransport espnow_;
    LoraTransport lora_;
    ProtocolSelector protocol_selector_;
    TransferEngine transfer_engine_;
    BufferPool buffer_pool_;

    /* Step 8: Dual-priority queues */
    QueueHandle_t hi_pri_queue_ = nullptr;
    QueueHandle_t lo_pri_queue_ = nullptr;
    QueueHandle_t packet_queue_ = nullptr; /* kept for get_packet_queue() compat */
    EventGroupHandle_t events_ = nullptr;
    uint16_t my_addr_ = 0;
    uint32_t discovery_timer_ms_ = 0;
    uint32_t prune_timer_ms_ = 0;
    uint32_t bias_timer_ms_ = 0;
    uint32_t topo_metrics_timer_ms_ = 0;
    std::atomic<bool> has_internet_{false};
    uint8_t lora_rx_priority_ = 5;

    /* Step 1d: DSDV sequence number for internet route */
    uint16_t my_inet_seq_ = 0;

    /* Hysteresis: track current preferred parent for check_better_route() */
    uint16_t preferred_parent_ = BROADCAST_ADDR;

    /* Fix 2: track current WiFi channel to avoid redundant set_channel calls */
    uint8_t current_channel_ = 0;

    /* Channel hopping for relay bootstrap (find gateway's ESP-NOW channel) */
    uint32_t channel_hop_timer_ms_ = 0;
    uint8_t channel_hop_idx_ = 0;

    /* Fix 7: deferred ESP-NOW peer update flag (set from WiFi event task) */
    std::atomic<bool> espnow_peer_update_pending_{false};

    /* Deferred EXIT_OFFLINE broadcast (set from WiFi event task) */
    std::atomic<bool> exit_offline_pending_{false};

    /* Heap monitor */
    HeapMonitor heap_monitor_;

    /* MQTT bridge */
    MqttClient *mqtt_client_ = nullptr;

    /* Topic subscriptions for cloud-to-deep-node messaging */
    char subscribed_topics_[4][32] = {};
    uint8_t subscribed_topic_count_ = 0;

    /* P6: Command dedup — ignore duplicate MESH_CMD within 2 seconds */
    uint8_t last_mesh_cmd_id_ = 0xFF;
    uint32_t last_mesh_cmd_ms_ = 0;

    /* Cloud command indicator (display auto-clears after 3s) */
    uint32_t last_cloud_cmd_ms_ = 0;

    /* Lab peer blacklist — drop direct (hop_count==0) packets from these addrs */
    std::vector<uint16_t> blocked_peers_; /* empty = disabled */

    /*
     * Step 4: Enlarged dedup cache with timestamps.
     * Prevents broadcast storm by dropping packets we've already forwarded.
     */
    struct SeenEntry
    {
        uint16_t src;
        uint16_t dst;
        uint8_t type;
        uint16_t seq;
        uint32_t time_ms;
    };
    static constexpr uint8_t SEEN_CACHE_SIZE = 64;
    SeenEntry seen_cache_[SEEN_CACHE_SIZE] = {};
    uint8_t seen_idx_ = 0;

    bool already_seen(uint16_t src, uint16_t dst, uint8_t type, uint16_t seq,
                      uint32_t window_ms)
    {
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        for (uint8_t i = 0; i < SEEN_CACHE_SIZE; i++)
        {
            if (seen_cache_[i].src == src && seen_cache_[i].dst == dst &&
                seen_cache_[i].type == type && seen_cache_[i].seq == seq &&
                (now - seen_cache_[i].time_ms) < window_ms)
            {
                return true;
            }
        }
        /* Prefer overwriting expired entries over round-robin */
        uint8_t slot = seen_idx_;
        for (uint8_t i = 0; i < SEEN_CACHE_SIZE; i++)
        {
            if ((now - seen_cache_[i].time_ms) > 10000)
            {
                slot = i;
                break;
            }
        }
        seen_cache_[slot] = {src, dst, type, seq, now};
        if (slot == seen_idx_)
            seen_idx_ = (seen_idx_ + 1) % SEEN_CACHE_SIZE;
        return false;
    }
};

} /* namespace flp */
