#pragma once

#include "route_table.hpp"
#include "packet.hpp"
#include "ble_transport.hpp"
#include "lora_transport.hpp"
#include "protocol_selector.hpp"
#include "selective_repeat.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include <array>

#define FLP_EVT_WIFI_CONNECTED    BIT0
#define FLP_EVT_TRANSFER_COMPLETE BIT1
#define FLP_EVT_EXIT_NODE_ELECTED BIT2

namespace flp {

class MqttSnClient; // forward declaration

enum class RxTransport : uint8_t {
    BLE,
    LORA,
};

struct RxPacket {
    uint8_t     data[MAX_MTU];
    size_t      len;
    int8_t      rssi;
    RxTransport source;
};

// Exit node election candidate
struct ExitCandidate {
    uint16_t addr;
    int8_t   rssi_to_gw;
    uint8_t  hops_to_gw;
};

// Active file transfer state (sender side)
struct ActiveTransfer {
    const uint8_t *data        = nullptr;
    size_t         size        = 0;
    uint16_t       fragment_count = 0;
    uint16_t       fragment_size  = 0;
    uint16_t       next_fragment  = 0;
    uint16_t       exit_node      = 0;
    char           filename[20]   = {};
    bool           active         = false;
};

class MeshManager {
public:
    MeshManager() = default;

    void init();
    void run(); // main loop -- called from FreeRTOS task

    // Task 1: WiFi status wiring
    void set_has_internet(bool v);

    // Task 5: File transfer API
    void start_file_transfer(const char *filename, const uint8_t *data, size_t size);

    // Task 6: MQTT bridge wiring
    void set_mqtt_client(MqttSnClient *client) { mqtt_client_ = client; }

    void set_lora_rx_priority(uint8_t p) { lora_rx_priority_ = p; }

    QueueHandle_t get_packet_queue() const { return packet_queue_; }
    uint16_t get_addr() const { return my_addr_; }
    EventGroupHandle_t get_events() const { return events_; }

    void send_packet(uint16_t dst, PacketType type, const uint8_t *payload,
                     size_t payload_len);

private:
    void process_packet(const RxPacket &pkt);
    void handle_discovery(const PacketHeader &hdr, const uint8_t *payload, size_t payload_len);
    void handle_transfer_ad(const PacketHeader &hdr, const uint8_t *payload, size_t payload_len);
    void handle_transfer_ack(const PacketHeader &hdr, const uint8_t *payload, size_t payload_len);
    void handle_data(const PacketHeader &hdr, const uint8_t *payload, size_t payload_len);
    void forward_packet(const RxPacket &pkt, const PacketHeader &hdr);
    void send_discovery();
    void send_broadcast_with_retry(PacketType type, const uint8_t *payload,
                                   size_t payload_len, uint8_t max_retries = 3);
    void transfer_tick();
    void send_raw(Transport transport, const uint8_t *data, size_t len,
                  uint16_t peer_addr);

    RouteTable       route_table_;
    BleTransport     ble_;
    LoraTransport    lora_;
    ProtocolSelector protocol_selector_;
    SelectiveRepeat  arq_;

    QueueHandle_t      packet_queue_      = nullptr;
    EventGroupHandle_t events_            = nullptr;
    uint16_t           my_addr_           = 0;
    uint32_t           discovery_timer_ms_ = 0;
    bool               has_internet_      = false;
    uint8_t            lora_rx_priority_  = 5;

    // Task 4: Exit node election state
    std::array<ExitCandidate, 4> candidates_ = {};
    uint8_t  candidate_count_   = 0;
    uint32_t election_start_ms_ = 0;
    bool     election_active_   = false;

    // Task 5: Active file transfer state
    ActiveTransfer transfer_ = {};

    // Task 6: MQTT bridge
    MqttSnClient *mqtt_client_ = nullptr;
};

} // namespace flp
