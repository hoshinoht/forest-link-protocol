#pragma once

// =============================================================================
// mesh_manager.hpp — Role 2: ESP32 Mesh Brain
//
// Implements:
//   FR-MESH4  — Parse intent packets, reply as exit node if we have MQTT
//   FR-MESH6  — Adaptive protocol selection (BLE vs LoRa)
//   FR-MESH7  — Intent broadcast retry, max 3 attempts
//   FR-MESH8  — Route packet to self (consume) or relay forward
//   NFR-MESH2 — Interrupt-driven via FreeRTOS queue (no polling)
//
// Calls into (does NOT implement):
//   BleTransport::send()       — Role 3
//   LoraTransport::send()      — Role 4
//   MqttSnClient::publish()    — Role 1
//   ProtocolSelector::select() — shared component
// =============================================================================

#include <cstdint>
#include <cstddef>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "packet.hpp"
#include "route_table.hpp"
#include "ble_transport.hpp"
#include "lora_transport.hpp"
#include "protocol_selector.hpp"
#include "mqtt_sn_client.hpp"

namespace flp {

// =============================================================================
// Raw packet envelope — what sits in the FreeRTOS RX queue (NFR-MESH2)
// Posted by BLE/LoRa transport callbacks, consumed by run()
// =============================================================================
struct RxEnvelope {
    uint8_t   data[256];
    size_t    len;
    int8_t    rssi;
    Transport transport;
};

// =============================================================================
// MeshManager
// =============================================================================
class MeshManager {
public:
    MeshManager() = default;

    // Called once from app_main() before tasks start
    void init(BleTransport *ble, LoraTransport *lora,
              ProtocolSelector *selector, MqttSnClient *mqtt);

    // FreeRTOS task entry point — called from mesh_task in main.cpp
    void run();

    // Called by BLE/LoRa transport RX callbacks to feed packets in
    // Safe to call from any task context (uses xQueueSend, not blocking)
    void on_packet_received(const uint8_t *data, size_t len,
                            int8_t rssi, Transport transport);

private:
    // Transport + service handles
    BleTransport     *ble_      = nullptr;
    LoraTransport    *lora_     = nullptr;
    ProtocolSelector *selector_ = nullptr;
    MqttSnClient     *mqtt_     = nullptr;

    // This node's 16-bit address (derived from MAC in init())
    uint16_t node_addr_ = 0;

    // NFR-MESH2: incoming packets arrive here from ISR/transport callbacks
    QueueHandle_t rx_queue_ = nullptr;

    // Neighbour table — updated whenever we hear a packet
    RouteTable routes_;

    // FR-MESH7: track intent broadcast state
    struct IntentState {
        bool     active;
        uint32_t transfer_id;
        uint8_t  retries;        // how many broadcasts sent so far
        int64_t  last_sent_us;   // esp_timer_get_time() at last broadcast
    } intent_ = {};

    // --- FR-MESH4 ---
    void handle_transfer_ad(const PacketHeader *hdr, const uint8_t *payload);
    void reply_as_exit_node(const PacketHeader *req_hdr, const uint8_t *payload);
    bool has_mqtt_access();

    // --- FR-MESH6 ---
    // Returns 0 on success
    int adaptive_send(uint16_t dst_addr, const uint8_t *data,
                      size_t len, uint8_t priority);

    // --- FR-MESH7 ---
    void broadcast_intent(uint32_t transfer_id, uint32_t file_size, uint8_t priority);

    // --- FR-MESH8 ---
    void handle_data_fragment(const PacketHeader *hdr,
                              const uint8_t *payload, size_t payload_len);
    void relay_packet(const uint8_t *raw, size_t len);

    // Central dispatcher — reads packet type and calls the right handler
    void dispatch(const RxEnvelope &env);
};

} // namespace flp
