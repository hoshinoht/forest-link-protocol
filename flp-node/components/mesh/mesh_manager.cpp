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

#include <cinttypes>
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_sn_client.hpp"

using namespace flp;

static const char *TAG = "mesh_mgr";

// -- Task 1: WiFi status setter -----------------------------------------------

void MeshManager::set_has_internet(bool v)
{
    if (has_internet_ != v)
    {
        has_internet_ = v;
        ESP_LOGI(TAG, "has_internet_ = %s", v ? "true" : "false");
    }
}

// -- Init ---------------------------------------------------------------------

void MeshManager::init()
{
    // Derive node address from MAC (lower 16 bits)
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    my_addr_ = static_cast<uint16_t>((mac[4] << 8) | mac[5]);
    ESP_LOGI(TAG, "Node addr: 0x%04X", my_addr_);

    // Init buffer pool
    buffer_pool_.init();

    // Create unified inbound packet queue (16 slots, pointer-based)
    packet_queue_ = xQueueCreate(16, sizeof(BufferSlab *));
    assert(packet_queue_);

    // Create event group
    events_ = xEventGroupCreate();
    assert(events_);

    // Pass unified queue and buffer pool to transports before init
    ble_.set_packet_queue(packet_queue_);
    ble_.set_buffer_pool(&buffer_pool_);
    lora_.set_packet_queue(packet_queue_);
    lora_.set_buffer_pool(&buffer_pool_);

    // Init transports
    ble_.init();
    lora_.init(lora_rx_priority_);

    // Start BLE advertising + scanning
    ble_.start_advertise();
    ble_.start_scan();

    // Init protocol selector
    protocol_selector_.init();

    // Init transfer engine
    transfer_engine_.init(
        events_,
        my_addr_,
        [this](uint16_t dst,
               PacketType type,
               const uint8_t *payload,
               size_t payload_len)
        { send_packet(dst, type, payload, payload_len); },
        [this](Transport t,
               const uint8_t *data,
               size_t len,
               uint16_t peer_addr)
        { send_raw(t, data, len, peer_addr); },
        [this](int8_t rssi, uint8_t hops, size_t payload_size)
        {
            return protocol_selector_.select(
                rssi, hops, payload_size, 100.0f);
        });

    // Wire up fragment forwarding to MQTT
    transfer_engine_.set_forward_to_mqtt(
        [this](uint32_t session_id, uint16_t seq, uint16_t src_node,
               const uint8_t *data, size_t len, const char *filename) {
            if (mqtt_client_)
                mqtt_client_->publish_fragment(session_id, seq, src_node,
                                               data, len, filename);
        });

    discovery_timer_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    ESP_LOGI(TAG, "MeshManager initialized");
}

// -- Main loop ----------------------------------------------------------------

void MeshManager::run()
{
    while (true)
    {
        // NFR-MESH2: Single blocking receive on unified queue — transports
        // post BufferSlab* items directly, no polling indirection.
        // Timeout drives periodic tasks (discovery, prune, ARQ tick).
        BufferSlab *slab = nullptr;
        for (uint8_t drain = 0; drain < 8; drain++)
        {
            TickType_t wait = (drain == 0) ? pdMS_TO_TICKS(100) : 0;
            if (xQueueReceive(packet_queue_, &slab, wait) != pdTRUE)
            {
                break;
            }
            process_slab(slab);
            buffer_pool_.release(slab);
        }

        // Periodic tasks
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);

        // Discovery broadcast every 10 seconds
        if (now - discovery_timer_ms_ > 10000)
        {
            send_discovery();
            discovery_timer_ms_ = now;
        }

        // Prune stale neighbors (30s timeout) — only every 5 seconds to
        // avoid O(n) scan on every loop iteration.
        if (now - prune_timer_ms_ > 5000)
        {
            route_table_.prune_stale(30000);
            prune_timer_ms_ = now;
        }

        // Recalculate protocol bias every 10s
        if (now - bias_timer_ms_ > 10000)
        {
            protocol_selector_.recalculate_bias();
            bias_timer_ms_ = now;
        }

        // Transfer engine tick (ARQ, election, broadcast retry, fragment feed)
        transfer_engine_.tick(now);
    }
}

// -- Packet dispatch ----------------------------------------------------------

void MeshManager::process_slab(BufferSlab *slab)
{
    if (slab->len < PACKET_HEADER_SIZE)
    {
        ESP_LOGW(TAG, "Packet too short: %zu bytes", slab->len);
        return;
    }

    PacketHeader hdr;
    memcpy(&hdr, slab->data, PACKET_HEADER_SIZE);

    // Drop our own packets (e.g. LoRa broadcast echoes back to sender)
    if (hdr.src_addr == my_addr_)
    {
        return;
    }

    if (hdr.version() != PROTOCOL_VERSION)
    {
        ESP_LOGW(TAG, "Unknown version %u", hdr.version());
        return;
    }

    if (hdr.ttl() == 0)
    {
        ESP_LOGD(TAG, "Dropping packet, TTL=0");
        return;
    }

    // Update route table with source info
    bool via_ble = (slab->source == RxTransport::BLE);
    bool via_lora = (slab->source == RxTransport::LORA);
    route_table_.update_neighbor(
        hdr.src_addr, slab->rssi, hdr.hop_count(), via_ble, via_lora);

    const uint8_t *payload = slab->data + PACKET_HEADER_SIZE;
    size_t payload_len = slab->len - PACKET_HEADER_SIZE;

    // Is this packet for us or broadcast?
    if (hdr.dst_addr == my_addr_ || hdr.dst_addr == BROADCAST_ADDR)
    {
        switch (hdr.type())
        {
            case PacketType::DISCOVERY:
                handle_discovery(hdr, payload, payload_len);
                break;
            case PacketType::TRANSFER_AD:
                transfer_engine_.handle_transfer_ad(
                    hdr, payload, payload_len, has_internet_);
                break;
            case PacketType::TRANSFER_ACK:
                transfer_engine_.handle_transfer_ack(
                    hdr, payload, payload_len);
                break;
            case PacketType::DATA:
                transfer_engine_.handle_data(hdr, payload, payload_len);
                // Exit nodes forward fragments individually via forward_to_mqtt callback
                // No reassembly check needed here
                break;
            case PacketType::ACK:
                transfer_engine_.handle_ack(hdr.seq_num, hdr.src_addr);
                break;
            case PacketType::NACK:
                transfer_engine_.handle_nack(hdr.seq_num, hdr.src_addr);
                break;
            default:
                ESP_LOGD(TAG,
                         "Unhandled packet type 0x%02X",
                         static_cast<uint8_t>(hdr.type()));
                break;
        }
    }

    // Forward if not for us (and not broadcast)
    if (hdr.dst_addr != my_addr_ && hdr.dst_addr != BROADCAST_ADDR)
    {
        forward_packet(slab, hdr);
    }
}

// -- Task 3: Discovery with hops_to_internet tracking -------------------------

void MeshManager::handle_discovery(const PacketHeader &hdr,
                                   const uint8_t *payload,
                                   size_t payload_len)
{
    if (payload_len < sizeof(DiscoveryPayload))
    {
        return;
    }

    DiscoveryPayload disc;
    memcpy(&disc, payload, sizeof(disc));

    ESP_LOGI(TAG,
             "Discovery from 0x%04X: inet=%u hops_inet=%u rssi=%d",
             hdr.src_addr,
             disc.flags & 0x01,
             disc.hops_to_internet,
             disc.rssi);

    if (disc.flags & 0x01)
    {
        route_table_.set_has_internet(hdr.src_addr, true);
    }

    // Store hops_to_internet from the discovery payload.
    // process_slab already called update_neighbor with the correct
    // transport/RSSI/hops from the BufferSlab, so we must not call
    // update_neighbor again with hardcoded values that would overwrite them.
    route_table_.set_hops_to_internet(hdr.src_addr, disc.hops_to_internet);

    // Respond with our own discovery (unicast back)
    DiscoveryPayload resp = {};
    resp.flags = has_internet_ ? 0x01 : 0x00;

    if (has_internet_)
    {
        resp.hops_to_internet = 0;
    }
    else
    {
        uint8_t min_h = route_table_.min_hops_to_internet();
        resp.hops_to_internet =
            (min_h < 0xFE) ? static_cast<uint8_t>(min_h + 1) : 0xFF;
    }
    resp.rssi = 0;

    send_packet(hdr.src_addr,
                PacketType::DISCOVERY,
                reinterpret_cast<const uint8_t *>(&resp),
                sizeof(resp));
}

// -- Forwarding ---------------------------------------------------------------

void MeshManager::forward_packet(BufferSlab *slab, const PacketHeader &hdr)
{
    uint16_t next = route_table_.next_hop(hdr.dst_addr);
    if (next == BROADCAST_ADDR)
    {
        ESP_LOGW(TAG, "No route to 0x%04X, dropping", hdr.dst_addr);
        return;
    }

    // Mutate in-place
    PacketHeader *fwd_hdr = reinterpret_cast<PacketHeader *>(slab->data);
    fwd_hdr->set_ttl_hops(fwd_hdr->ttl() - 1, fwd_hdr->hop_count() + 1);

    NeighborEntry neighbor;
    int8_t rssi = -90;
    if (route_table_.get_neighbor(next, neighbor))
    {
        rssi = neighbor.rssi;
    }

    Transport t =
        protocol_selector_.select(rssi, fwd_hdr->hop_count(), slab->len, 100.0f);

    ESP_LOGD(TAG,
             "Forwarding to 0x%04X via 0x%04X (%s), ttl=%u",
             hdr.dst_addr,
             next,
             (t == Transport::BLE) ? "BLE" : "LoRa",
             fwd_hdr->ttl());

    send_raw(t, slab->data, slab->len, next);
}

// -- Task 3: Discovery broadcast with computed hops_to_internet ---------------

void MeshManager::send_discovery()
{
    DiscoveryPayload disc = {};
    disc.flags = has_internet_ ? 0x01 : 0x00;

    if (has_internet_)
    {
        disc.hops_to_internet = 0;
    }
    else
    {
        uint8_t min_h = route_table_.min_hops_to_internet();
        disc.hops_to_internet =
            (min_h < 0xFE) ? static_cast<uint8_t>(min_h + 1) : 0xFF;
    }
    disc.rssi = 0;

    // Single send — discovery is already periodic (every 10s), so retrying
    // here would block the main loop for up to 3.5s unnecessarily.
    // send_broadcast_with_retry is reserved for infrequent TRANSFER_AD
    // broadcasts where the extra reliability is worth the delay.
    send_packet(BROADCAST_ADDR,
                PacketType::DISCOVERY,
                reinterpret_cast<const uint8_t *>(&disc),
                sizeof(disc));

    ESP_LOGD(TAG,
             "Sent discovery broadcast (hops_to_inet=%u)",
             disc.hops_to_internet);
}

// -- Start file transfer (delegates to TransferEngine) ------------------------

void MeshManager::start_file_transfer(const char *filename,
                                      const uint8_t *data,
                                      size_t size)
{
    Transport t = protocol_selector_.select(0, 0, size, 100.0f);
    transfer_engine_.start_file_transfer(filename, data, size, t);
}

// -- send_packet / send_raw ---------------------------------------------------

void MeshManager::send_packet(uint16_t dst,
                              PacketType type,
                              const uint8_t *payload,
                              size_t payload_len)
{
    if (PACKET_HEADER_SIZE + payload_len > MAX_MTU)
    {
        ESP_LOGW(TAG,
                 "send_packet: payload %zu exceeds MAX_MTU, dropping",
                 payload_len);
        return;
    }

    uint8_t buf[MAX_MTU];
    PacketHeader hdr = {};
    hdr.set_ver_type(PROTOCOL_VERSION, type);
    hdr.src_addr = my_addr_;
    hdr.dst_addr = dst;
    hdr.set_ttl_hops(DEFAULT_TTL, 0);
    hdr.seq_num = 0;

    memcpy(buf, &hdr, PACKET_HEADER_SIZE);
    if (payload && payload_len > 0)
    {
        memcpy(buf + PACKET_HEADER_SIZE, payload, payload_len);
    }

    size_t total = PACKET_HEADER_SIZE + payload_len;

    if (dst == BROADCAST_ADDR)
    {
        send_raw(Transport::LORA, buf, total, dst);
    }
    else
    {
        NeighborEntry neighbor;
        int8_t rssi = -90;
        uint8_t hops = 0xFF;
        if (route_table_.get_neighbor(dst, neighbor))
        {
            rssi = neighbor.rssi;
            hops = neighbor.hop_count;
        }
        Transport t = protocol_selector_.select(rssi, hops, total, 100.0f);
        send_raw(t, buf, total, dst);
    }
}

void MeshManager::send_raw(Transport transport,
                           const uint8_t *data,
                           size_t len,
                           uint16_t peer_addr)
{
    uint32_t t0 = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    int rc;

    if (transport == Transport::BLE)
    {
        rc = ble_.send(peer_addr, data, len);
    }
    else
    {
        rc = lora_.send(peer_addr, data, len);
    }

    uint32_t latency = static_cast<uint32_t>(esp_timer_get_time() / 1000) - t0;
    protocol_selector_.report_tx_result(transport, rc == 0, latency);
}
