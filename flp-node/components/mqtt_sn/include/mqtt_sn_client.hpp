#pragma once

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

    // Task 6: Publish reassembled file to cloud via MQTT
    void publish_file(const char *filename,
                      const uint8_t *data,
                      size_t size,
                      uint16_t src_node);

    void set_rx_callback(MqttRxCallback cb)
    {
        rx_callback_ = cb;
    }
    bool is_connected() const
    {
        return connected_;
    }

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
    MqttRxCallback rx_callback_ = nullptr;
    bool connected_ = false;
    uint16_t node_addr_ = 0;

    static void mqtt_event_handler(void *handler_args,
                                   esp_event_base_t base,
                                   int32_t event_id,
                                   void *event_data);
    void handle_mqtt_event(esp_mqtt_event_handle_t event);
    void register_default_topics();
};

} // namespace flp
