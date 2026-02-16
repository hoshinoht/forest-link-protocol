// =============================================================================
// mesh_manager.cpp — Role 2: ESP32 Mesh Brain
//
// FR-MESH4  — Parse intent, reply as exit node if we have MQTT
// FR-MESH6  — Adaptive protocol selection (BLE vs LoRa)
// FR-MESH7  — Intent broadcast retry, max 3 attempts
// FR-MESH8  — Consume packet if dst == us, else relay
// NFR-MESH2 — FreeRTOS queue-driven RX (interrupt-driven, no polling)
// =============================================================================

#include "mesh_manager.hpp"

#include <cstring>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "flp_config.h"

static const char *TAG = "mesh_mgr";

// Intent broadcast timeout before retry
static constexpr int64_t INTENT_TIMEOUT_US = 5000000LL; // 5 seconds

// =============================================================================
// Packet payload structs
// __attribute__((packed)) — no padding, safe to cast directly from raw bytes.
// Share these definitions with your teammates so all nodes agree on layout.
// =============================================================================

// Sent by a node advertising it wants to transfer a file (the "intent")
struct __attribute__((packed)) TransferAdPayload {
    uint32_t transfer_id;
    uint32_t file_size;
    uint8_t  priority;
};

// Sent back by an exit node volunteering to receive the file
struct __attribute__((packed)) RouteReplyPayload {
    uint32_t transfer_id;
    uint8_t  hop_count;
};

// One fragment of a file being transferred
struct __attribute__((packed)) FragmentPayload {
    uint32_t transfer_id;
    uint16_t seq_num;
    uint16_t total_frags;
    uint16_t data_len;
    // Followed by data_len bytes of file data
};

// =============================================================================
// init()
// =============================================================================
void flp::MeshManager::init(BleTransport *ble, LoraTransport *lora,
                             ProtocolSelector *selector, MqttSnClient *mqtt)
{
    ble_      = ble;
    lora_     = lora;
    selector_ = selector;
    mqtt_     = mqtt;

    // Derive node address from last 2 bytes of MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    node_addr_ = (static_cast<uint16_t>(mac[4]) << 8) | mac[5];

    // NFR-MESH2: create the RX queue
    // BLE/LoRa transport callbacks post RxEnvelopes here instead of calling
    // us directly — this decouples the ISR from the mesh logic
    rx_queue_ = xQueueCreate(16, sizeof(RxEnvelope));
    configASSERT(rx_queue_ != nullptr);

    memset(&intent_, 0, sizeof(intent_));

    ESP_LOGI(TAG, "MeshManager init — node=0x%04X", node_addr_);
}

// =============================================================================
// on_packet_received()
// Called by BleTransport and LoraTransport from their RX callbacks/tasks.
// Just enqueues — run() does the actual work.
// =============================================================================
void flp::MeshManager::on_packet_received(const uint8_t *data, size_t len,
                                           int8_t rssi, Transport transport)
{
    if (len < sizeof(PacketHeader) || len > sizeof(RxEnvelope::data)) return;

    RxEnvelope env;
    memcpy(env.data, data, len);
    env.len       = len;
    env.rssi      = rssi;
    env.transport = transport;

    // Non-blocking — if queue is full we drop the packet rather than block
    // the transport callback (which may be running in a high-priority context)
    if (xQueueSend(rx_queue_, &env, 0) != pdTRUE) {
        ESP_LOGW(TAG, "RX queue full — packet dropped");
    }
}

// =============================================================================
// run()
// Runs inside mesh_task. Blocks on the RX queue — zero CPU when idle.
// NFR-MESH2: no polling loop; wakes only when a packet is enqueued.
// =============================================================================
void flp::MeshManager::run()
{
    ESP_LOGI(TAG, "mesh_task running");

    while (true) {
        RxEnvelope env;

        // Block for up to 100ms waiting for a packet.
        // 100ms timeout lets us also check intent retry state below.
        if (xQueueReceive(rx_queue_, &env, pdMS_TO_TICKS(100)) == pdTRUE) {
            dispatch(env);
        }

        // FR-MESH7: check if intent broadcast needs a retry
        if (intent_.active) {
            int64_t elapsed = esp_timer_get_time() - intent_.last_sent_us;
            if (elapsed > INTENT_TIMEOUT_US) {
                if (intent_.retries < FLP_BROADCAST_RETRIES) {
                    ESP_LOGW(TAG, "Intent timeout — retry %u/%u",
                             intent_.retries, FLP_BROADCAST_RETRIES);
                    broadcast_intent(intent_.transfer_id, 0, 0); // retransmit
                } else {
                    ESP_LOGE(TAG, "No exit node found after %u retries",
                             FLP_BROADCAST_RETRIES);
                    intent_.active = false;
                }
            }
        }
    }
}

// =============================================================================
// dispatch()
// Reads packet type and routes to the correct handler.
// =============================================================================
void flp::MeshManager::dispatch(const RxEnvelope &env)
{
    const auto *hdr = reinterpret_cast<const PacketHeader *>(env.data);

    // Ignore our own packets (LoRa is broadcast — we hear ourselves)
    if (hdr->src_addr == node_addr_) return;

    const uint8_t *payload     = env.data + sizeof(PacketHeader);
    size_t         payload_len = env.len  - sizeof(PacketHeader);

    // Update neighbour table with what we just heard
    bool is_ble  = (env.transport == Transport::BLE);
    bool is_lora = (env.transport == Transport::LORA);
    routes_.update_neighbor(hdr->src_addr, env.rssi,
                            hdr->hop_count, is_ble, is_lora);

    switch (hdr->type) {

        case PacketType::TRANSFER_AD:   // FR-MESH4
            handle_transfer_ad(hdr, payload);
            break;

        case PacketType::DATA:          // FR-MESH8
            handle_data_fragment(hdr, payload, payload_len);
            break;

        case PacketType::ROUTE_REPLY:
            // An exit node replied to our intent broadcast
            if (hdr->dst_addr == node_addr_ && intent_.active) {
                ESP_LOGI(TAG, "Exit node elected: 0x%04X", hdr->src_addr);
                intent_.active = false;
                // Sender logic continues from here — hand off to your
                // fragment send flow (or trigger via event group if needed)
            }
            break;

        case PacketType::ACK:
        case PacketType::NACK:
            // Handed to SelectiveRepeat — Role 1 owns ARQ logic
            // TODO: selective_repeat_.handle_ack/nack(hdr->seq_num)
            break;

        default:
            break;
    }
}

// =============================================================================
// FR-MESH4 — handle_transfer_ad()
// Another node is advertising intent to transfer a file.
// If we have MQTT access → volunteer as exit node.
// If TTL allows → relay the ad further to extend discovery range.
// =============================================================================
void flp::MeshManager::handle_transfer_ad(const PacketHeader *hdr,
                                           const uint8_t *payload)
{
    if (hdr->payload_len < sizeof(TransferAdPayload)) return;

    ESP_LOGI(TAG, "FR-MESH4: TRANSFER_AD from 0x%04X", hdr->src_addr);

    // If we have MQTT access, reply as exit node
    if (has_mqtt_access()) {
        reply_as_exit_node(hdr, payload);
    }

    // Relay onward if TTL allows (extends discovery beyond single hop)
    if (hdr->ttl > 1) {
        uint8_t relay[sizeof(PacketHeader) + sizeof(TransferAdPayload)];
        memcpy(relay, hdr, sizeof(PacketHeader) + sizeof(TransferAdPayload));

        auto *relay_hdr = reinterpret_cast<PacketHeader *>(relay);
        relay_hdr->ttl--;
        relay_hdr->hop_count++;

        lora_->send(relay, sizeof(relay));
        ESP_LOGD(TAG, "Relayed TRANSFER_AD (ttl=%u)", relay_hdr->ttl);
    }
}

// FR-MESH4 — build and send a ROUTE_REPLY back to the sender
void flp::MeshManager::reply_as_exit_node(const PacketHeader *req_hdr,
                                           const uint8_t *payload)
{
    const auto *ad = reinterpret_cast<const TransferAdPayload *>(payload);

    uint8_t buf[sizeof(PacketHeader) + sizeof(RouteReplyPayload)];

    auto *hdr        = reinterpret_cast<PacketHeader *>(buf);
    hdr->version     = 1;
    hdr->type        = PacketType::ROUTE_REPLY;
    hdr->src_addr    = node_addr_;
    hdr->dst_addr    = req_hdr->src_addr;
    hdr->hop_count   = 0;
    hdr->ttl         = FLP_MAX_HOPS;
    hdr->seq_num     = 0;
    hdr->payload_len = sizeof(RouteReplyPayload);

    auto *reply          = reinterpret_cast<RouteReplyPayload *>(buf + sizeof(PacketHeader));
    reply->transfer_id   = ad->transfer_id;
    reply->hop_count     = req_hdr->hop_count;

    // Use adaptive send to reply — FR-MESH6 applies here too
    adaptive_send(req_hdr->src_addr, buf, sizeof(buf), 255);

    ESP_LOGI(TAG, "FR-MESH4: Replied as exit node to 0x%04X", req_hdr->src_addr);
}

// =============================================================================
// FR-MESH6 — adaptive_send()
// Asks ProtocolSelector which transport to use, then calls it.
// Factors: RSSI, hop count, packet size, battery level.
// =============================================================================
int flp::MeshManager::adaptive_send(uint16_t dst_addr, const uint8_t *data,
                                     size_t len, uint8_t priority)
{
    NeighborEntry *entry = routes_.find(dst_addr);
    int8_t  rssi        = entry ? entry->rssi      : -120;
    uint8_t hops        = entry ? entry->hop_count : 255;
    float   battery_pct = 100.0f; // TODO: read from ADC

    Transport transport = selector_->select(rssi, hops, len, battery_pct);

    if (transport == Transport::BLE) {
        ESP_LOGD(TAG, "FR-MESH6: → BLE (rssi=%d hops=%u size=%zu)", rssi, hops, len);
        return ble_->send(dst_addr, data, len);
    } else {
        ESP_LOGD(TAG, "FR-MESH6: → LoRa (rssi=%d hops=%u size=%zu)", rssi, hops, len);
        return lora_->send(data, len);
    }
}

// =============================================================================
// FR-MESH7 — broadcast_intent()
// Broadcasts a TRANSFER_AD over LoRa. Called up to FLP_BROADCAST_RETRIES times.
// =============================================================================
void flp::MeshManager::broadcast_intent(uint32_t transfer_id,
                                         uint32_t file_size, uint8_t priority)
{
    uint8_t buf[sizeof(PacketHeader) + sizeof(TransferAdPayload)];

    auto *hdr        = reinterpret_cast<PacketHeader *>(buf);
    hdr->version     = 1;
    hdr->type        = PacketType::TRANSFER_AD;
    hdr->src_addr    = node_addr_;
    hdr->dst_addr    = 0xFFFF;              // broadcast
    hdr->hop_count   = 0;
    hdr->ttl         = FLP_BROADCAST_RETRIES;
    hdr->seq_num     = 0;
    hdr->payload_len = sizeof(TransferAdPayload);

    auto *ad         = reinterpret_cast<TransferAdPayload *>(buf + sizeof(PacketHeader));
    ad->transfer_id  = transfer_id;
    ad->file_size    = file_size;
    ad->priority     = priority;

    lora_->send(buf, sizeof(buf)); // Always LoRa — it's a broadcast

    intent_.transfer_id  = transfer_id;
    intent_.last_sent_us = esp_timer_get_time();
    intent_.retries++;
    intent_.active       = true;

    ESP_LOGI(TAG, "FR-MESH7: Intent broadcast #%u (transfer=0x%08lX)",
             intent_.retries, transfer_id);
}

// =============================================================================
// FR-MESH8 — handle_data_fragment()
// If dst == us: consume (pass to MQTT-SN layer).
// If dst != us: relay toward destination.
// =============================================================================
void flp::MeshManager::handle_data_fragment(const PacketHeader *hdr,
                                             const uint8_t *payload,
                                             size_t payload_len)
{
    if (hdr->dst_addr == node_addr_) {
        // This fragment is for us — we are the exit node
        // Pass to Role 1 (MQTT-SN) to publish
        if (payload_len >= sizeof(FragmentPayload)) {
            const auto *frag = reinterpret_cast<const FragmentPayload *>(payload);
            const uint8_t *file_data = payload + sizeof(FragmentPayload);

            ESP_LOGI(TAG, "FR-MESH8: Fragment %u/%u for us → MQTT",
                     frag->seq_num + 1, frag->total_frags);

            uint16_t topic_id = static_cast<uint16_t>(frag->transfer_id & 0xFFFF);
            mqtt_->publish(topic_id, file_data, frag->data_len);
        }
    } else {
        // FR-MESH8: not for us — relay it forward
        ESP_LOGD(TAG, "FR-MESH8: Relaying fragment toward 0x%04X", hdr->dst_addr);
        relay_packet(reinterpret_cast<const uint8_t *>(hdr),
                     sizeof(PacketHeader) + payload_len);
    }
}

void flp::MeshManager::relay_packet(const uint8_t *raw, size_t len)
{
    if (len > 256) return;

    uint8_t buf[256];
    memcpy(buf, raw, len);

    auto *hdr = reinterpret_cast<PacketHeader *>(buf);
    if (hdr->ttl == 0) {
        ESP_LOGW(TAG, "TTL expired — dropping relay");
        return;
    }
    hdr->ttl--;
    hdr->hop_count++;

    adaptive_send(hdr->dst_addr, buf, len, 128);
}

// =============================================================================
// has_mqtt_access()
// Asks MQTT-SN client if we currently have WiFi + broker connectivity.
// =============================================================================
bool flp::MeshManager::has_mqtt_access()
{
    // TODO: mqtt_->is_connected() once Role 1 implements it
    return false;
}
