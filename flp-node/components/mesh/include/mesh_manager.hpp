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

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "ble_transport.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "lora_transport.hpp"
#include "packet.hpp"
#include "protocol_selector.hpp"
#include "route_table.hpp"
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

    void send_packet(uint16_t dst,
                     PacketType type,
                     const uint8_t *payload,
                     size_t payload_len);

  private:
    void process_packet(const RxPacket &pkt);
    void handle_discovery(const PacketHeader &hdr,
                          const uint8_t *payload,
                          size_t payload_len);
    void forward_packet(const RxPacket &pkt, const PacketHeader &hdr);
    void send_discovery();
    void send_raw(Transport transport,
                  const uint8_t *data,
                  size_t len,
                  uint16_t peer_addr);

    RouteTable route_table_;
    BleTransport ble_;
    LoraTransport lora_;
    ProtocolSelector protocol_selector_;
    TransferEngine transfer_engine_;

    QueueHandle_t packet_queue_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    uint16_t my_addr_ = 0;
    uint32_t discovery_timer_ms_ = 0;
    uint32_t prune_timer_ms_ = 0;
    uint32_t bias_timer_ms_ = 0;
    std::atomic<bool> has_internet_{false};
    uint8_t lora_rx_priority_ = 5;

    // MQTT bridge
    MqttSnClient *mqtt_client_ = nullptr;
};

} // namespace flp
