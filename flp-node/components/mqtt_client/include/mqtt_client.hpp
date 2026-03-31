#pragma once

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "packet.hpp"
#include "topic_table.hpp"

namespace flp
{

/* Callback for received MQTT messages */
using MqttRxCallback = void (*)(const char *topic,
                                const uint8_t *data,
                                size_t len);

struct MqttPublishItem
{
    uint16_t topic_id;
    uint8_t data[MAX_MTU];
    size_t len;
    int qos;
};

/* Request to publish a file asynchronously (mesh task -> MQTT task).
 * Data is COPIED into the struct — caller may free its buffer immediately. */
struct FilePublishRequest
{
    char filename[33];     /* 32 chars + NUL (matches wire format) */
    uint8_t data[MAX_MTU]; /* chunk data (copied, not borrowed) */
    size_t size;
    uint16_t src_node;
};

/* Request to publish a single fragment (exit node -> MQTT) */
struct FragmentPublishRequest
{
    uint16_t session_id;
    uint16_t seq;
    uint16_t src_node;
    uint8_t data[MAX_MTU];
    size_t len;
    char filename[33]; /* 32 chars + NUL (matches wire format) */
};

/* Cloud ACK/NACK for selective-repeat ARQ */
struct CloudAckItem
{
    uint16_t session_id;
    uint16_t seq;
};

struct CloudNackItem
{
    uint16_t session_id;
    uint16_t seq;
};

struct FragmentAckItem
{
    uint16_t session_id;
    uint16_t seq;
};

struct TransferCompleteItem
{
    uint16_t session_id;
};

/* Inbound mesh command from cloud (MQTT -> exit node -> mesh) */
struct MeshCmdItem
{
    uint16_t target_addr;
    uint8_t cmd_id;
    uint8_t data[64];
    size_t data_len;
};

class MqttClient
{
  public:
    MqttClient() = default;
    MqttClient(const MqttClient &) = delete;
    MqttClient &operator=(const MqttClient &) = delete;

    void init();
    void run();

    int
    publish(uint16_t topic_id, const uint8_t *data, size_t len, int qos = 1);
    int publish_raw(const char *topic,
                    const uint8_t *data,
                    size_t len,
                    int qos = 1);

    /* Task 6: Queue reassembled file for async cloud upload (non-blocking) */
    void publish_file(const char *filename,
                      const uint8_t *data,
                      size_t size,
                      uint16_t src_node);

    /* Fragment-level publish for exit nodes (returns false if queue full) */
    bool publish_fragment(uint16_t session_id, uint16_t seq, uint16_t src_node,
                          const uint8_t *data, size_t len, const char *filename);

    /* Publish complete transfer meta (called when exit node receives fragment 0) */
    void publish_transfer_meta(uint16_t session_id, const char *filename,
                               uint16_t src_node, uint16_t exit_node,
                               uint32_t total_size, uint16_t chunk_count,
                               uint16_t fragment_size, uint32_t crc32);

    void set_rx_callback(MqttRxCallback cb)
    {
        rx_callback_ = cb;
    }

    /*
     * Fix 13: Register an event group bit to be set when MQTT first connects.
     * Allows waiters (e.g. auto_demo_task) to block on the event group instead
     * of polling is_connected() with vTaskDelay.
     */
    void set_connected_event_group(EventGroupHandle_t eg, EventBits_t bit)
    {
        connected_event_group_ = eg;
        connected_event_bit_ = bit;
    }
    bool is_connected() const
    {
        return connected_;
    }

    /* Drain one pending mesh command (returns true if item was available) */
    bool receive_cmd(MeshCmdItem &out);

    /* Drain one cloud ACK (returns true if item was available) */
    bool drain_cloud_ack(uint16_t session_id, uint16_t &seq_out);

    /* Drain one cloud NACK (returns true if item was available) */
    bool drain_cloud_nack(uint16_t session_id, uint16_t &seq_out);

    /* Drain one deferred fragment ACK (exit node: accepted by local MQTT
     * client/outbox). */
    bool drain_fragment_ack(uint16_t session_id, uint16_t &seq_out);

    /* Session consensus: cloud confirmed transfer complete.
     * Returns true (once) when the cloud has published TRANSFER_COMPLETE
     * for any session.  Exit node polls this and forwards a
     * TRANSFER_DONE packet to the source via mesh. */
    bool consume_transfer_complete(uint16_t session_id);

    void activate_transfer_session(uint16_t session_id);
    void clear_transfer_session(uint16_t session_id);

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
    QueueHandle_t ack_queue_ = nullptr;
    QueueHandle_t nack_queue_ = nullptr;
    QueueHandle_t cmd_queue_ = nullptr; /* inbound mesh commands from cloud */
    std::atomic<TaskHandle_t> task_{nullptr};
    MqttRxCallback rx_callback_ = nullptr;
    std::atomic<bool> connected_{false};
    uint16_t node_addr_ = 0;

    /* Captured topic IDs from register_default_topics() */
    uint16_t status_topic_id_ = 0;
    uint16_t file_data_topic_id_ = 0;
    uint16_t file_meta_topic_id_ = 0;

    static void mqtt_event_handler(void *handler_args,
                                   esp_event_base_t base,
                                   int32_t event_id,
                                   void *event_data);
    void handle_mqtt_event(esp_mqtt_event_handle_t event);
    void register_default_topics();
    void process_file_publish(const FilePublishRequest &req);
    void process_fragment_publish();
    int outbox_size_bytes() const;
    void reset_transfer_runtime_state();
    void notify();

    /* Reusable scratch buffer for fragment chunk publishing — avoids 1474B
     * stack alloc in process_fragment_publish (mqtt_task only). */
    uint8_t chunk_scratch_[4 + MAX_MTU] = {};

    /* Fix 13: optional event group signalled on first MQTT connect */
    EventGroupHandle_t connected_event_group_ = nullptr;
    EventBits_t connected_event_bit_ = 0;

    /* Fragment publish queue (exit node mode) */
    QueueHandle_t fragment_publish_queue_ = nullptr;

    /* Deferred ACK queue: seq numbers of relay fragments accepted by the
     * local MQTT client/outbox. Drained by TransferEngine to send mesh ACKs
     * without waiting for broker PUBACK latency. */
    QueueHandle_t fragment_ack_queue_ = nullptr;
    QueueHandle_t transfer_complete_queue_ = nullptr;
    std::atomic<uint16_t> active_transfer_session_{0};
};

} /* namespace flp */
