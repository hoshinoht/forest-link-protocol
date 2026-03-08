// =============================================================================
// mesh_manager.cpp — Role 2: ESP32 Mesh Brain
//
// FR-MESH4  — Parse intent, reply as exit node if we have MQTT
// FR-MESH6  — Adaptive protocol selection (ESP-NOW vs LoRa)
// FR-MESH7  — Intent broadcast retry, max 3 attempts
// FR-MESH8  — Consume packet if dst == us, else relay
// NFR-MESH2 — FreeRTOS queue-driven RX (interrupt-driven, no polling)
// =============================================================================

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
#include "mqtt_client.hpp"

using namespace flp;

static const char *TAG = "mesh_mgr";

namespace
{
constexpr uint8_t kNodeMacLowByteIdx = 5;
constexpr uint8_t kNodeMacHighByteIdx = 4;
constexpr uint8_t kQueueDepth = 16;
constexpr uint8_t kMaxQueueDrainPerLoop = 8;
constexpr uint32_t kQueueWaitMs = 100;
constexpr uint32_t kDiscoveryIntervalMs = 10000;
constexpr uint32_t kPruneIntervalMs = 5000;
constexpr uint32_t kNeighborStaleTimeoutMs = 30000;
constexpr uint32_t kBiasIntervalMs = 10000;
constexpr uint32_t kTelemetryIntervalMs = 30000;
constexpr uint8_t kDiscoveryInternetFlag = 0x01;
constexpr int8_t kDefaultRssi = -90;
constexpr float kLinkQualityPct = 100.0f;
constexpr uint8_t kMeshCmdMaxDataLen = 64;
constexpr size_t kMeshCmdBufLen = static_cast<size_t>(kMeshCmdMaxDataLen) + 1;
constexpr size_t kTopicBufLen = 32;

const char *rx_transport_name(RxTransport source)
{
    switch (source)
    {
        case RxTransport::ESPNOW:
            return "ESP-NOW";
        case RxTransport::LORA:
            return "LoRa";
        default:
            return "UNKNOWN";
    }
}

uint8_t compute_hops_to_internet(const RouteTable &route_table,
                                 bool has_internet)
{
    if (has_internet)
    {
        return 0;
    }

    const uint8_t min_hops = route_table.min_hops_to_internet();
    return (min_hops < 0xFE) ? static_cast<uint8_t>(min_hops + 1) : 0xFF;
}
} // namespace

// -- Task 1: WiFi status setter -----------------------------------------------

void MeshManager::set_has_internet(bool v)
{
    if (has_internet_ != v)
    {
        has_internet_ = v;
        ESP_LOGI(TAG, "has_internet_ = %s", v ? "true" : "false");
    }
}

bool MeshManager::is_mqtt_connected() const
{
    return mqtt_client_ && mqtt_client_->is_connected();
}

void MeshManager::update_espnow_broadcast_peer()
{
    espnow_.update_broadcast_peer();
}

// -- Init ---------------------------------------------------------------------

void MeshManager::init()
{
    // Derive node address from MAC (lower 16 bits)
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    my_addr_ = static_cast<uint16_t>((mac[kNodeMacHighByteIdx] << 8) |
                                     mac[kNodeMacLowByteIdx]);
    ESP_LOGI(TAG, "Node addr: 0x%04X", my_addr_);

    // Init buffer pool
    buffer_pool_.init();

    // Create unified inbound packet queue (16 slots, pointer-based)
    packet_queue_ = xQueueCreate(kQueueDepth, sizeof(BufferSlab *));
    assert(packet_queue_);

    // Create event group
    events_ = xEventGroupCreate();
    assert(events_);

    // Pass unified queue and buffer pool to transports before init
    espnow_.set_packet_queue(packet_queue_);
    espnow_.set_buffer_pool(&buffer_pool_);
    lora_.set_packet_queue(packet_queue_);
    lora_.set_buffer_pool(&buffer_pool_);

    // Init transports (WiFi must already be started for ESP-NOW)
    espnow_.init();
    lora_.init(lora_rx_priority_);

    // Init protocol selector
    protocol_selector_.init();

    // Init transfer engine
    transfer_engine_.init(
        events_,
        my_addr_,
        [this](uint16_t dst,
               PacketType type,
               const uint8_t *payload,
               size_t payload_len,
               uint16_t seq_num)
        { send_packet(dst, type, payload, payload_len, seq_num); });

    // Wire up fragment forwarding to MQTT
    transfer_engine_.set_forward_to_mqtt(
        [this](uint32_t session_id,
               uint16_t seq,
               uint16_t src_node,
               const uint8_t *data,
               size_t len,
               const char *filename)
        {
            if (mqtt_client_)
            {
                mqtt_client_->publish_fragment(
                    session_id, seq, src_node, data, len, filename);
            }
        });

    // Wire up transfer meta forwarding (exit node publishes complete meta)
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

    // Init heap monitor
    heap_monitor_.init();

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
        for (uint8_t drain = 0; drain < kMaxQueueDrainPerLoop; drain++)
        {
            TickType_t wait = (drain == 0) ? pdMS_TO_TICKS(kQueueWaitMs) : 0;
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
        if (now - discovery_timer_ms_ > kDiscoveryIntervalMs)
        {
            send_discovery();
            discovery_timer_ms_ = now;
        }

        // Prune stale neighbors (30s timeout) — only every 5 seconds to
        // avoid O(n) scan on every loop iteration.
        if (now - prune_timer_ms_ > kPruneIntervalMs)
        {
            route_table_.prune_stale(kNeighborStaleTimeoutMs);
            prune_timer_ms_ = now;
        }

        // Recalculate protocol bias every 10s
        if (now - bias_timer_ms_ > kBiasIntervalMs)
        {
            protocol_selector_.recalculate_bias();
            heap_monitor_.periodic_check();
            bias_timer_ms_ = now;
        }

        // Publish topology + metrics + heap every 30s via relay.
        // relay_publish() handles both exit nodes (direct MQTT) and
        // deep-field nodes (MESH_PUB routed through mesh to exit).
        if (now - topo_metrics_timer_ms_ > kTelemetryIntervalMs)
        {
            publish_all_telemetry();
            topo_metrics_timer_ms_ = now;
        }

        // Drain inbound mesh commands from cloud (exit nodes only)
        drain_cmd_queue();

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
        ESP_LOGW(TAG,
                 "Drop non-FLP frame: ver=%u raw_ver_type=0x%02X src=0x%04X "
                 "dst=0x%04X ttl=%u hops=%u len=%zu rssi=%d via=%s "
                 "head=[%02X %02X %02X %02X %02X %02X %02X %02X]",
                 hdr.version(),
                 hdr.ver_type,
                 hdr.src_addr,
                 hdr.dst_addr,
                 hdr.ttl(),
                 hdr.hop_count(),
                 slab->len,
                 slab->rssi,
                 rx_transport_name(slab->source),
                 slab->data[0],
                 slab->data[1],
                 slab->data[2],
                 slab->data[3],
                 slab->data[4],
                 slab->data[5],
                 slab->data[6],
                 slab->data[7]);
        return;
    }

    if (hdr.ttl() == 0)
    {
        ESP_LOGD(TAG, "Dropping packet, TTL=0");
        return;
    }

    // Update route table with source info
    bool via_espnow = (slab->source == RxTransport::ESPNOW);
    bool via_lora = (slab->source == RxTransport::LORA);
    route_table_.update_neighbor(
        hdr.src_addr, slab->rssi, hdr.hop_count(), via_espnow, via_lora);

    const uint8_t *payload = slab->data + PACKET_HEADER_SIZE;
    size_t payload_len = slab->len - PACKET_HEADER_SIZE;

    // EXIT_ANY_ADDR: consumed by exit nodes (has MQTT), relayed by others.
    bool is_exit = has_internet_ && mqtt_client_;
    bool for_us = (hdr.dst_addr == my_addr_) ||
                  (hdr.dst_addr == BROADCAST_ADDR) ||
                  (hdr.dst_addr == EXIT_ANY_ADDR && is_exit);

    if (for_us)
    {
        switch (hdr.type())
        {
            case PacketType::DISCOVERY:
                handle_discovery(hdr, slab->source, payload, payload_len);
                break;
            case PacketType::TRANSFER_AD:
                transfer_engine_.handle_transfer_ad(
                    hdr, payload, payload_len, has_internet_);
                break;
            case PacketType::TRANSFER_ACK:
                transfer_engine_.handle_transfer_ack(hdr, payload, payload_len);
                break;
            case PacketType::DATA:
            case PacketType::PARITY:
                transfer_engine_.handle_data(hdr, payload, payload_len);
                break;
            case PacketType::ACK:
                transfer_engine_.handle_ack(hdr.seq_num, hdr.src_addr);
                break;
            case PacketType::NACK:
                transfer_engine_.handle_nack(hdr.seq_num, hdr.src_addr);
                break;
            case PacketType::MESH_PUB:
                handle_mesh_pub(hdr, payload, payload_len);
                break;
            case PacketType::MESH_CMD:
                handle_mesh_cmd(hdr, payload, payload_len);
                break;
            default:
                ESP_LOGD(TAG,
                         "Unhandled packet type 0x%02X",
                         static_cast<uint8_t>(hdr.type()));
                break;
        }
    }

    // Forward if not for us (not unicast-to-us, not broadcast).
    // EXIT_ANY_ADDR packets are forwarded by non-exit nodes — next_hop()
    // won't find 0xFFFE in neighbors so it falls back to routing toward
    // the lowest hops_to_internet, which is exactly what we want.
    bool should_forward = (hdr.dst_addr != my_addr_) &&
                          (hdr.dst_addr != BROADCAST_ADDR) &&
                          !(hdr.dst_addr == EXIT_ANY_ADDR && is_exit);
    if (should_forward)
    {
        // Fix 4: Dedup — drop packets we've already forwarded to prevent
        // broadcast storm (O(TTL × relays) amplification per packet).
        if (already_seen(hdr.src_addr,
                         hdr.dst_addr,
                         static_cast<uint8_t>(hdr.type()),
                         hdr.seq_num))
        {
            ESP_LOGD(TAG,
                     "Dedup: dropping already-seen packet from 0x%04X seq=%u",
                     hdr.src_addr,
                     hdr.seq_num);
            return;
        }
        forward_packet(slab, hdr);
    }
}

// -- Task 3: Discovery with hops_to_internet tracking -------------------------

void MeshManager::handle_discovery(const PacketHeader &hdr,
                                   RxTransport source,
                                   const uint8_t *payload,
                                   size_t payload_len)
{
    if (payload_len < sizeof(DiscoveryPayload))
    {
        return;
    }

    DiscoveryPayload disc;
    memcpy(&disc, payload, sizeof(disc));

    const char *rx_transport = "UNKNOWN";
    if (source == RxTransport::ESPNOW)
    {
        rx_transport = "ESP-NOW";
    }
    else if (source == RxTransport::LORA)
    {
        rx_transport = "LoRa";
    }

    ESP_LOGI(TAG,
             "Discovery from 0x%04X via %s: inet=%u hops_inet=%u rssi=%d ch=%u",
             hdr.src_addr,
             rx_transport,
             (disc.flags & kDiscoveryInternetFlag) != 0U,
             disc.hops_to_internet,
             disc.rssi,
             disc.wifi_channel);

    if ((disc.flags & kDiscoveryInternetFlag) != 0U)
    {
        route_table_.set_has_internet(hdr.src_addr, true);
    }

    // Auto-sync ESP-NOW channel: if the peer advertises a valid channel
    // and we're not connected to an AP (relay node or disconnected exit),
    // switch to match so ESP-NOW can reach the mesh.
    if (disc.wifi_channel > 0 && !has_internet_)
    {
        uint8_t cur_ch = 0;
        wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&cur_ch, &sec);
        if (cur_ch != disc.wifi_channel)
        {
            esp_wifi_set_channel(disc.wifi_channel, WIFI_SECOND_CHAN_NONE);
            ESP_LOGI(TAG,
                     "ESP-NOW channel synced: %u -> %u",
                     cur_ch,
                     disc.wifi_channel);
        }
    }

    // Store hops_to_internet from the discovery payload.
    // process_slab already called update_neighbor with the correct
    // transport/RSSI/hops from the BufferSlab, so we must not call
    // update_neighbor again with hardcoded values that would overwrite them.
    route_table_.set_hops_to_internet(hdr.src_addr, disc.hops_to_internet);

    // Respond to discovery requests only. Unicast discovery packets are already
    // responses, so replying again causes a ping-pong storm.
    if (hdr.dst_addr == BROADCAST_ADDR)
    {
        DiscoveryPayload resp = {};
        resp.flags = has_internet_ ? kDiscoveryInternetFlag : 0x00;
        resp.hops_to_internet =
            compute_hops_to_internet(route_table_, has_internet_);
        resp.rssi = 0;

        // Include our current channel so the peer can sync
        uint8_t resp_ch = 0;
        wifi_second_chan_t resp_sec = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&resp_ch, &resp_sec);
        resp.wifi_channel = resp_ch;

        send_packet(hdr.src_addr,
                    PacketType::DISCOVERY,
                    reinterpret_cast<const uint8_t *>(&resp),
                    sizeof(resp));
    }
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
    int8_t rssi = kDefaultRssi;
    if (route_table_.get_neighbor(next, neighbor))
    {
        rssi = neighbor.rssi;
    }

    Transport t = protocol_selector_.select(
        rssi, fwd_hdr->hop_count(), slab->len, kLinkQualityPct);

    ESP_LOGD(TAG,
             "Forwarding to 0x%04X via 0x%04X (%s), ttl=%u",
             hdr.dst_addr,
             next,
             (t == Transport::ESPNOW) ? "ESPNOW" : "LoRa",
             fwd_hdr->ttl());

    send_raw(t, slab->data, slab->len, next);
}

// -- Task 3: Discovery broadcast with computed hops_to_internet ---------------

void MeshManager::send_discovery()
{
    DiscoveryPayload disc = {};
    disc.flags = has_internet_ ? kDiscoveryInternetFlag : 0x00;
    disc.hops_to_internet =
        compute_hops_to_internet(route_table_, has_internet_);
    disc.rssi = 0;

    // Include current WiFi channel so relay nodes can auto-sync for ESP-NOW
    uint8_t ch = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&ch, &sec);
    disc.wifi_channel = ch;

    // Single send — discovery is already periodic (every 10s), so retrying
    // here would block the main loop for up to 3.5s unnecessarily.
    // send_broadcast_with_retry is reserved for infrequent TRANSFER_AD
    // broadcasts where the extra reliability is worth the delay.
    send_packet(BROADCAST_ADDR,
                PacketType::DISCOVERY,
                reinterpret_cast<const uint8_t *>(&disc),
                sizeof(disc));

    ESP_LOGD(TAG,
             "Sent discovery broadcast (hops_to_inet=%u ch=%u)",
             disc.hops_to_internet,
             disc.wifi_channel);
}

// -- Mesh relay layer ---------------------------------------------------------

static const char *relay_topic_suffix(uint8_t topic_id)
{
    switch (topic_id)
    {
        case RelayTopic::HEAP:
            return "heap";
        case RelayTopic::METRICS:
            return "metrics";
        case RelayTopic::TOPOLOGY:
            return "topology";
        case RelayTopic::STATUS:
            return "status";
        default:
            return "unknown";
    }
}

void MeshManager::relay_publish(uint8_t relay_topic,
                                const uint8_t *data,
                                size_t len)
{
    if (has_internet_ && mqtt_client_)
    {
        // Exit node: publish directly to MQTT (may queue if temporarily
        // disconnected — matches is_exit predicate in process_slab)
        char topic[40];
        snprintf(topic,
                 sizeof(topic),
                 "flp/%04X/%s",
                 my_addr_,
                 relay_topic_suffix(relay_topic));
        mqtt_client_->publish_raw(topic, data, len);
    }
    else
    {
        // Deep-field node: wrap in MESH_PUB and route to nearest exit.
        // Payload: [relay_topic:1][data:N]
        uint8_t payload[MAX_MTU - PACKET_HEADER_SIZE];
        payload[0] = relay_topic;
        size_t copy_len = len;
        if (copy_len > sizeof(payload) - 1)
        {
            copy_len = sizeof(payload) - 1;
        }
        memcpy(payload + 1, data, copy_len);
        send_packet(EXIT_ANY_ADDR, PacketType::MESH_PUB, payload, 1 + copy_len);
    }
}

void MeshManager::handle_mesh_pub(const PacketHeader &hdr,
                                  const uint8_t *payload,
                                  size_t payload_len)
{
    // Exit node received a relayed publish from a deep-field node.
    // Forward to MQTT on behalf of the originator (hdr.src_addr).
    if (payload_len < 2 || !mqtt_client_)
    {
        return;
    }

    uint8_t relay_topic = payload[0];
    const uint8_t *data = payload + 1;
    size_t data_len = payload_len - 1;

    char topic[40];
    snprintf(topic,
             sizeof(topic),
             "flp/%04X/%s",
             hdr.src_addr,
             relay_topic_suffix(relay_topic));
    mqtt_client_->publish_raw(topic, data, data_len);

    ESP_LOGD(TAG,
             "Relayed MESH_PUB from 0x%04X topic=%s len=%zu",
             hdr.src_addr,
             relay_topic_suffix(relay_topic),
             data_len);
}

void MeshManager::handle_mesh_cmd(const PacketHeader &hdr,
                                  const uint8_t *payload,
                                  size_t payload_len)
{
    if (payload_len < 1)
    {
        return;
    }

    uint8_t cmd_id = payload[0];
    ESP_LOGI(TAG,
             "MESH_CMD from 0x%04X: cmd=%u len=%zu",
             hdr.src_addr,
             cmd_id,
             payload_len - 1);

    switch (cmd_id)
    {
        case MeshCmd::REQUEST_TELEMETRY:
            publish_all_telemetry();
            break;
        case MeshCmd::REBOOT:
            ESP_LOGW(TAG, "Remote reboot requested by 0x%04X", hdr.src_addr);
            esp_restart();
            break;
        case MeshCmd::TOPIC_MSG:
            handle_topic_msg(payload + 1, payload_len - 1);
            break;
        default:
            ESP_LOGD(TAG, "Unknown MESH_CMD %u", cmd_id);
            break;
    }
}

void MeshManager::subscribe_topic(const char *topic)
{
    const uint8_t max_topics = static_cast<uint8_t>(
        sizeof(subscribed_topics_) / sizeof(subscribed_topics_[0]));
    if (subscribed_topic_count_ >= max_topics)
    {
        ESP_LOGW(TAG, "Topic subscription table full, ignoring '%s'", topic);
        return;
    }
    strncpy(subscribed_topics_[subscribed_topic_count_],
            topic,
            sizeof(subscribed_topics_[0]) - 1);
    subscribed_topics_[subscribed_topic_count_]
                      [sizeof(subscribed_topics_[0]) - 1] = '\0';
    subscribed_topic_count_++;
    ESP_LOGI(TAG,
             "Subscribed to topic '%s' (%u/%u)",
             topic,
             subscribed_topic_count_,
             max_topics);
}

void MeshManager::handle_topic_msg(const uint8_t *data, size_t len)
{
    if (len < 1)
    {
        return;
    }

    uint8_t topic_len = data[0];
    if (topic_len == 0 || topic_len > (kTopicBufLen - 1) ||
        (1U + topic_len) > len)
    {
        ESP_LOGW(TAG, "TOPIC_MSG: invalid topic_len=%u", topic_len);
        return;
    }

    char topic[kTopicBufLen];
    memcpy(topic, data + 1, topic_len);
    topic[topic_len] = '\0';

    size_t payload_len = len - 1 - topic_len;
    (void) payload_len; // available for future use by handlers

    // Check against subscription list
    for (uint8_t i = 0; i < subscribed_topic_count_; i++)
    {
        if (strcmp(subscribed_topics_[i], topic) == 0)
        {
            ESP_LOGI(TAG,
                     "TOPIC_MSG matched '%s' payload_len=%zu",
                     topic,
                     payload_len);
            return;
        }
    }
    ESP_LOGD(TAG, "TOPIC_MSG topic '%s' not subscribed, ignoring", topic);
}

void MeshManager::publish_all_telemetry()
{
    // Topology
    uint8_t topo_buf[128];
    size_t topo_len = route_table_.serialize(topo_buf, sizeof(topo_buf));
    if (topo_len > 0)
    {
        relay_publish(RelayTopic::TOPOLOGY, topo_buf, topo_len);
    }

    // Protocol metrics
    uint8_t metrics_buf[48];
    size_t metrics_len =
        protocol_selector_.serialize_metrics(metrics_buf, sizeof(metrics_buf));
    if (metrics_len > 0)
    {
        relay_publish(RelayTopic::METRICS, metrics_buf, metrics_len);
    }

    // Heap stats
    uint8_t heap_buf[20];
    size_t heap_len = heap_monitor_.serialize(heap_buf, sizeof(heap_buf));
    if (heap_len > 0)
    {
        relay_publish(RelayTopic::HEAP, heap_buf, heap_len);
    }
}

void MeshManager::drain_cmd_queue()
{
    if (!mqtt_client_)
    {
        return;
    }

    // Exit nodes drain commands from MQTT and inject into mesh
    MeshCmdItem cmd;
    while (mqtt_client_->receive_cmd(cmd))
    {
        if (cmd.target_addr == my_addr_)
        {
            // Command is for this exit node itself
            uint8_t buf[kMeshCmdBufLen];
            buf[0] = cmd.cmd_id;
            size_t cmd_len = cmd.data_len;
            if (cmd_len > kMeshCmdMaxDataLen)
            {
                ESP_LOGW(TAG,
                         "MESH_CMD data too long from cloud (%zu), truncating",
                         cmd_len);
                cmd_len = kMeshCmdMaxDataLen;
            }
            if (cmd_len > 0)
            {
                memcpy(buf + 1, cmd.data, cmd_len);
            }

            PacketHeader fake_hdr = {};
            fake_hdr.src_addr = 0; // from cloud
            handle_mesh_cmd(fake_hdr, buf, 1 + cmd_len);
        }
        else
        {
            // Route command into mesh toward target node
            uint8_t payload[kMeshCmdBufLen];
            payload[0] = cmd.cmd_id;
            size_t plen = 1;
            if (cmd.data_len > 0 && cmd.data_len <= kMeshCmdMaxDataLen)
            {
                memcpy(payload + 1, cmd.data, cmd.data_len);
                plen += cmd.data_len;
            }
            send_packet(cmd.target_addr, PacketType::MESH_CMD, payload, plen);
            ESP_LOGI(TAG,
                     "Routed MESH_CMD to 0x%04X cmd=%u",
                     cmd.target_addr,
                     cmd.cmd_id);
        }
    }
}

// -- Start file transfer (delegates to TransferEngine) ------------------------

void MeshManager::start_file_transfer(const char *filename,
                                      const uint8_t *data,
                                      size_t size)
{
    bool mqtt_ready = mqtt_client_ && mqtt_client_->is_connected();
    transfer_engine_.start_file_transfer(
        filename, data, size, has_internet_, mqtt_ready);
}

// -- send_packet / send_raw ---------------------------------------------------

void MeshManager::send_packet(uint16_t dst,
                              PacketType type,
                              const uint8_t *payload,
                              size_t payload_len,
                              uint16_t seq_num)
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
    hdr.seq_num = seq_num;

    memcpy(buf, &hdr, PACKET_HEADER_SIZE);
    if (payload && payload_len > 0)
    {
        memcpy(buf + PACKET_HEADER_SIZE, payload, payload_len);
    }

    size_t total = PACKET_HEADER_SIZE + payload_len;

    if (dst == BROADCAST_ADDR)
    {
        // Broadcast over both transports for neighbor discovery
        send_raw(Transport::ESPNOW, buf, total, dst);
        send_raw(Transport::LORA, buf, total, dst);
    }
    else
    {
        NeighborEntry neighbor;
        int8_t rssi = kDefaultRssi;
        uint8_t hops = 0xFF;
        if (route_table_.get_neighbor(dst, neighbor))
        {
            rssi = neighbor.rssi;
            hops = neighbor.hop_count;
        }
        Transport t =
            protocol_selector_.select(rssi, hops, total, kLinkQualityPct);
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

    if (transport == Transport::ESPNOW)
    {
        rc = espnow_.send(peer_addr, data, len);
    }
    else
    {
        rc = lora_.send(peer_addr, data, len);
    }

    uint32_t latency = static_cast<uint32_t>(esp_timer_get_time() / 1000) - t0;
    protocol_selector_.report_tx_result(transport, rc == 0, latency);
}
