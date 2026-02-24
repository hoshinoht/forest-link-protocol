#pragma once

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mqtt_client.h"
#include "topic_table.hpp"

namespace flp
{

// Callback for received MQTT messages
using MqttRxCallback = void (*)(const char *topic,
                                const uint8_t *data,
                                size_t len);

struct MqttPublishItem
{
    uint16_t topic_id;
    uint8_t data[512];
    size_t len;
    int qos;
};

// Request to publish a file asynchronously (mesh task -> MQTT task)
struct FilePublishRequest
{
    const char *filename;
    const uint8_t *data;
    size_t size;
    uint16_t src_node;
};

// Request to publish a single fragment (exit node -> MQTT)
struct FragmentPublishRequest
{
    uint32_t session_id;
    uint16_t seq;
    uint16_t src_node;
    uint8_t data[512];
    size_t len;
    char filename[20];
};

// Cloud NACK for retransmitting a file chunk
struct CloudNackItem
{
    uint16_t seq;
};

// Inbound mesh command from cloud (MQTT → exit node → mesh)
struct MeshCmdItem
{
    uint16_t target_addr;
    uint8_t cmd_id;
    uint8_t data[64];
    size_t data_len;
};

class MqttSnClient
{
  public:
    MqttSnClient() = default;

    void init();
    void run();

    int
    publish(uint16_t topic_id, const uint8_t *data, size_t len, int qos = 1);
    int publish_raw(const char *topic,
                    const uint8_t *data,
                    size_t len,
                    int qos = 1);

    // Task 6: Queue reassembled file for async cloud upload (non-blocking)
    void publish_file(const char *filename,
                      const uint8_t *data,
                      size_t size,
                      uint16_t src_node);

    // Fragment-level publish for exit nodes (no reassembly needed)
    void publish_fragment(uint32_t session_id, uint16_t seq, uint16_t src_node,
                          const uint8_t *data, size_t len, const char *filename);

    // Publish complete transfer meta (called when exit node receives TRANSFER_AD)
    void publish_transfer_meta(uint32_t session_id, const char *filename,
                               uint16_t src_node, uint16_t exit_node,
                               uint32_t total_size, uint16_t chunk_count,
                               uint16_t fragment_size, uint32_t crc32);

    void set_rx_callback(MqttRxCallback cb)
    {
        rx_callback_ = cb;
    }
    bool is_connected() const
    {
        return connected_;
    }

    // Drain one pending mesh command (returns true if item was available)
    bool receive_cmd(MeshCmdItem &out);

    TopicTable &topic_table()
    {
        return topic_table_;
    }
    void set_node_addr(uint16_t addr)
    {
        node_addr_ = addr;
    }

  private:
    TopicTable topic_table_;
    esp_mqtt_client_handle_t client_ = nullptr;
    QueueHandle_t publish_queue_ = nullptr;
    QueueHandle_t file_publish_queue_ = nullptr;
    QueueHandle_t nack_queue_ = nullptr;
    QueueHandle_t cmd_queue_ = nullptr; // inbound mesh commands from cloud
    MqttRxCallback rx_callback_ = nullptr;
    std::atomic<bool> connected_{false};
    uint16_t node_addr_ = 0;

    static void mqtt_event_handler(void *handler_args,
                                   esp_event_base_t base,
                                   int32_t event_id,
                                   void *event_data);
    void handle_mqtt_event(esp_mqtt_event_handle_t event);
    void register_default_topics();
    void process_file_publish(const FilePublishRequest &req);
    void process_fragment_publish();
    void process_nack_retransmit();

    // Last published file data (retained for NACK retransmission)
    const uint8_t *last_file_data_ = nullptr;
    size_t last_file_size_ = 0;

    // Fragment publish queue (exit node mode)
    QueueHandle_t fragment_publish_queue_ = nullptr;
    bool meta_published_ = false;
    uint32_t last_meta_session_id_ = 0;
};

} // namespace flp
