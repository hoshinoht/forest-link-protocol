#pragma once

// =============================================================================
// mesh_manager.hpp — Role 2: ESP32 Mesh Brain
//
// Implements:
//   FR-MESH4  — Parse intent packets, reply as exit node if we have MQTT
//   FR-MESH6  — Adaptive protocol selection (ESP-NOW vs LoRa)
//   FR-MESH7  — Intent broadcast retry, max 3 attempts
//   FR-MESH8  — Route packet to self (consume) or relay forward
//   NFR-MESH2 — Interrupt-driven via FreeRTOS queue (no polling)
//
// Calls into (does NOT implement):
//   EspNowTransport::send()    — Role 3
//   LoraTransport::send()      — Role 4
//   MqttSnClient::publish()    — Role 1
//   ProtocolSelector::select() — shared component
// =============================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "espnow_transport.hpp"
#include "buffer_pool.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "lora_transport.hpp"
#include "packet.hpp"
#include "protocol_selector.hpp"
#include "route_table.hpp"
#include "heap_monitor.hpp"
#include "transfer_engine.hpp"

#define FLP_EVT_WIFI_CONNECTED    BIT0
#define FLP_EVT_TRANSFER_COMPLETE BIT1
#define FLP_EVT_EXIT_NODE_ELECTED BIT2

namespace flp
{

class MqttSnClient; // forward declaration

class MeshManager
{
  public:
    MeshManager() = default;

    void init();
    void run(); // main loop -- called from FreeRTOS task

    // Task 1: WiFi status wiring
    void set_has_internet(bool v);

    // Task 5: File transfer API
    void
    start_file_transfer(const char *filename, const uint8_t *data, size_t size);

    // Task 6: MQTT bridge wiring
    void set_mqtt_client(MqttSnClient *client)
    {
        mqtt_client_ = client;
    }

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

    bool has_internet() const { return has_internet_.load(); }
    uint8_t get_neighbor_count() const { return route_table_.get_count(); }
    uint8_t get_espnow_peer_count() const { return espnow_.get_peer_count(); }
    uint8_t get_hops_to_internet() const
    {
        return route_table_.min_hops_to_internet();
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

    void send_packet(uint16_t dst,
                     PacketType type,
                     const uint8_t *payload,
                     size_t payload_len,
                     uint16_t seq_num = 0);

  private:
    void process_slab(BufferSlab *slab);
    void handle_discovery(const PacketHeader &hdr,
                          const uint8_t *payload,
                          size_t payload_len);
    void handle_mesh_pub(const PacketHeader &hdr,
                         const uint8_t *payload,
                         size_t payload_len);
    void handle_mesh_cmd(const PacketHeader &hdr,
                         const uint8_t *payload,
                         size_t payload_len);
    void forward_packet(BufferSlab *slab, const PacketHeader &hdr);
    void send_discovery();
    void send_raw(Transport transport,
                  const uint8_t *data,
                  size_t len,
                  uint16_t peer_addr);

    // Generic relay: publishes via MQTT if exit node, else routes
    // through mesh to nearest exit node.
    void relay_publish(uint8_t relay_topic,
                       const uint8_t *data,
                       size_t len);
    void publish_all_telemetry();
    void drain_cmd_queue();

    RouteTable route_table_;
    EspNowTransport espnow_;
    LoraTransport lora_;
    ProtocolSelector protocol_selector_;
    TransferEngine transfer_engine_;
    BufferPool buffer_pool_;

    QueueHandle_t packet_queue_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    uint16_t my_addr_ = 0;
    uint32_t discovery_timer_ms_ = 0;
    uint32_t prune_timer_ms_ = 0;
    uint32_t bias_timer_ms_ = 0;
    uint32_t topo_metrics_timer_ms_ = 0;
    std::atomic<bool> has_internet_{false};
    uint8_t lora_rx_priority_ = 5;

    // Heap monitor
    HeapMonitor heap_monitor_;
    uint32_t heap_timer_ms_ = 0;

    // MQTT bridge
    MqttSnClient *mqtt_client_ = nullptr;

    // Fix 4: Forwarding dedup cache — prevents broadcast storm by dropping
    // packets we've already forwarded. Ring buffer of recently-seen
    // (src, dst, type, seq) tuples.
    struct SeenEntry
    {
        uint16_t src;
        uint16_t dst;
        uint8_t type;
        uint16_t seq;
    };
    static constexpr uint8_t SEEN_CACHE_SIZE = 32;
    SeenEntry seen_cache_[SEEN_CACHE_SIZE] = {};
    uint8_t seen_idx_ = 0;

    bool already_seen(uint16_t src, uint16_t dst, uint8_t type, uint16_t seq)
    {
        for (uint8_t i = 0; i < SEEN_CACHE_SIZE; i++)
        {
            if (seen_cache_[i].src == src && seen_cache_[i].dst == dst &&
                seen_cache_[i].type == type && seen_cache_[i].seq == seq)
                return true;
        }
        seen_cache_[seen_idx_] = {src, dst, type, seq};
        seen_idx_ = (seen_idx_ + 1) % SEEN_CACHE_SIZE;
        return false;
    }
};

} // namespace flp
