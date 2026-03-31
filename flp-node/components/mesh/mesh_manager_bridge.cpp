#include "mesh_manager.hpp"

#include <cinttypes>
#include <cstring>
#include <functional>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mqtt_client.hpp"

using namespace flp;

static const char *TAG = "mesh_mgr";

namespace
{
constexpr uint8_t kQueueDepth = 16;
constexpr int8_t kDefaultRssi = -90;
constexpr float kLinkQualityPct = 100.0f;
constexpr uint8_t kMeshCmdMaxDataLen = 64;
constexpr size_t kMeshCmdBufLen = static_cast<size_t>(kMeshCmdMaxDataLen) + 1;
constexpr size_t kTopicBufLen = 32;

const char *relay_topic_suffix(uint8_t topic_id)
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
} /* namespace */

void MeshManager::relay_publish(uint8_t relay_topic,
                                const uint8_t *data,
                                size_t len)
{
    if (has_internet_ && mqtt_client_)
    {
        /*
         * Exit node: publish directly to MQTT (may queue if temporarily
         * disconnected — matches is_exit predicate in process_slab)
         */
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
        /*
         * Deep-field node: wrap in MESH_PUB and route to nearest exit.
         * Payload: [relay_topic:1][data:N]
         */
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
    /*
     * Exit node received a relayed publish from a deep-field node.
     * Forward to MQTT on behalf of the originator (hdr.src_addr).
     */
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

    /*
     * P6 fix: Simple command dedup — when multiple exit nodes forward the
     * same command, the target receives duplicates (different src_addr
     * bypasses the packet dedup cache). Ignore identical cmd_id within 2s.
     */
    uint32_t cmd_now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (cmd_id == last_mesh_cmd_id_ && (cmd_now - last_mesh_cmd_ms_) < 2000)
    {
        ESP_LOGD(TAG, "Duplicate MESH_CMD cmd=%u from 0x%04X, ignoring",
                 cmd_id, hdr.src_addr);
        return;
    }
    last_mesh_cmd_id_ = cmd_id;
    last_mesh_cmd_ms_ = cmd_now;
    last_cloud_cmd_ms_ = cmd_now;

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
    (void) payload_len; /* available for future use by handlers */

    /* Check against subscription list */
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
    /* Topology */
    static constexpr size_t kTopoBufSize = 1 + MAX_NEIGHBORS * 7;
    uint8_t topo_buf[kTopoBufSize];
    size_t topo_len = route_table_.serialize(topo_buf, sizeof(topo_buf));
    if (topo_len > 0)
    {
        relay_publish(RelayTopic::TOPOLOGY, topo_buf, topo_len);
    }

    /* Protocol metrics */
    uint8_t metrics_buf[48];
    size_t metrics_len =
        protocol_selector_.serialize_metrics(metrics_buf, sizeof(metrics_buf));
    if (metrics_len > 0)
    {
        relay_publish(RelayTopic::METRICS, metrics_buf, metrics_len);
    }

    /* Heap stats */
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

    /* Exit nodes drain commands from MQTT and inject into mesh */
    MeshCmdItem cmd;
    while (mqtt_client_->receive_cmd(cmd))
    {
        if (cmd.target_addr == my_addr_)
        {
            /* Command is for this exit node itself */
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
            fake_hdr.src_addr = 0; /* from cloud */
            handle_mesh_cmd(fake_hdr, buf, 1 + cmd_len);
        }
        else
        {
            /* Route command into mesh toward target node */
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

void MeshManager::start_file_transfer(const char *filename,
                                      size_t size,
                                      ReadChunkFn read_chunk)
{
    bool mqtt_ready = mqtt_client_ && mqtt_client_->is_connected();
    transfer_engine_.start_file_transfer(
        filename, size, read_chunk, has_internet_, mqtt_ready, get_hops_to_internet());
}

int MeshManager::send_packet(uint16_t dst,
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
        return -1;
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
        /* Broadcast over both transports for neighbor discovery */
        send_raw(Transport::ESPNOW, buf, total, dst);
        send_raw(Transport::LORA, buf, total, dst);
        return 0;
    }

    /*
     * Use next_hop() to find the radio-level destination.
     * The packet header carries the final dst_addr; the radio-level
     * send must target the next relay, not the final destination.
     * Without this, ESP-NOW unicast to multi-hop destinations fails
     * (peer MAC not in table). LoRa accidentally works because it
     * ignores peer_addr and always broadcasts.
     */
    uint16_t radio_dst = route_table_.next_hop(dst, my_addr_);
    if (radio_dst == BROADCAST_ADDR)
    {
        /* No route known — broadcast if targeting EXIT_ANY_ADDR
         * (bootstrap: a relay may hear us and forward toward exits) */
        if (dst == EXIT_ANY_ADDR)
        {
            send_raw(Transport::ESPNOW, buf, total, BROADCAST_ADDR);
            send_raw(Transport::LORA, buf, total, BROADCAST_ADDR);
            return 0;
        }
        ESP_LOGW(TAG, "No route to 0x%04X, dropping", dst);
        return -1;
    }

    NeighborEntry neighbor;
    int8_t rssi = kDefaultRssi;
    uint8_t hops = 0xFF;
    if (route_table_.get_neighbor(radio_dst, neighbor))
    {
        rssi = neighbor.rssi;
        hops = neighbor.hop_count;
    }
    Transport t =
        protocol_selector_.select(rssi, hops, total, kLinkQualityPct);
    return send_raw(t, buf, total, radio_dst);
}

int MeshManager::send_raw(Transport transport,
                          const uint8_t *data,
                          size_t len,
                          uint16_t peer_addr)
{
    if (transport == Transport::LORA && !lora_.is_initialized())
    {
        return -1;
    }

    /* Fix 9: use int64_t to avoid truncation of esp_timer_get_time() µs
     * values before the subtraction; cast to ms only for the final result. */
    int64_t t0_us = esp_timer_get_time();
    int rc;

    if (transport == Transport::ESPNOW)
    {
        rc = espnow_.send(peer_addr, data, len);
    }
    else
    {
        rc = lora_.send(peer_addr, data, len);
    }

    uint32_t latency = static_cast<uint32_t>(
        (esp_timer_get_time() - t0_us) / 1000);
    protocol_selector_.report_tx_result(transport, rc == 0, latency);

    /* Step 3d: Report link TX result for ETX calculation */
    if (peer_addr != BROADCAST_ADDR)
    {
        route_table_.report_link_tx(peer_addr, rc == 0);
    }
    return rc;
}
