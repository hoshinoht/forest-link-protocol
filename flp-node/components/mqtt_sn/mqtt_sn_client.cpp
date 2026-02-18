#include "mqtt_sn_client.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mqtt_sn";

// Max data bytes per MQTT chunk (2-byte seq header + payload)
static constexpr size_t MQTT_CHUNK_PAYLOAD = 500;

namespace flp
{

void MqttSnClient::init()
{
    // Create publish queues
    publish_queue_ = xQueueCreate(16, sizeof(MqttPublishItem));
    file_publish_queue_ = xQueueCreate(2, sizeof(FilePublishRequest));
    nack_queue_ = xQueueCreate(16, sizeof(CloudNackItem));

    // Configure and start ESP-IDF MQTT client
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = CONFIG_FLP_MQTT_BROKER_URI;

    client_ = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(
        client_, MQTT_EVENT_ANY, mqtt_event_handler, this);
    esp_mqtt_client_start(client_);

    // Register predefined topics
    register_default_topics();

    ESP_LOGI(
        TAG, "MQTT client initialized, broker=%s", CONFIG_FLP_MQTT_BROKER_URI);
}

void MqttSnClient::register_default_topics()
{
    char topic_buf[64];

    // Topic 1: flp/<node_id>/status (Node->Cloud, QoS 0)
    snprintf(topic_buf, sizeof(topic_buf), "flp/%04x/status", node_addr_);
    topic_table_.register_topic(topic_buf);

    // Topic 2: flp/<node_id>/file/data (Node->Cloud, QoS 1)
    snprintf(topic_buf, sizeof(topic_buf), "flp/%04x/file/data", node_addr_);
    topic_table_.register_topic(topic_buf);

    // Topic 3: flp/<node_id>/file/meta (Node->Cloud, QoS 1)
    snprintf(topic_buf, sizeof(topic_buf), "flp/%04x/file/meta", node_addr_);
    topic_table_.register_topic(topic_buf);

    // Topic 4: flp/admin/cmd (Cloud->Node, QoS 1)
    topic_table_.register_topic("flp/admin/cmd");

    // Topic 5: flp/admin/ack (Cloud->Node, QoS 1)
    topic_table_.register_topic("flp/admin/ack");
}

void MqttSnClient::mqtt_event_handler(void *handler_args,
                                      esp_event_base_t base,
                                      int32_t event_id,
                                      void *event_data)
{
    auto *self = static_cast<MqttSnClient *>(handler_args);
    auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
    self->handle_mqtt_event(event);
}

void MqttSnClient::handle_mqtt_event(esp_mqtt_event_handle_t event)
{
    switch (event->event_id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker");
            connected_ = true;
            // Subscribe to admin topics
            esp_mqtt_client_subscribe(client_, "flp/admin/cmd", 1);
            esp_mqtt_client_subscribe(client_, "flp/admin/ack", 1);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected from broker");
            connected_ = false;
            break;

        case MQTT_EVENT_DATA:
        {
            ESP_LOGD(TAG,
                     "MQTT data: topic=%.*s, len=%d",
                     event->topic_len,
                     event->topic,
                     event->data_len);

            // Null-terminate topic for comparison and callback
            char topic_buf[128];
            size_t tlen = (event->topic_len < sizeof(topic_buf) - 1)
                              ? static_cast<size_t>(event->topic_len)
                              : sizeof(topic_buf) - 1;
            if (event->topic)
            {
                memcpy(topic_buf, event->topic, tlen);
            }
            topic_buf[tlen] = '\0';

            // Admin command handler
            if (strcmp(topic_buf, "flp/admin/cmd") == 0 && event->data)
            {
                ESP_LOGI(TAG, "Admin cmd: %.*s", event->data_len, event->data);
            }

            // Cloud NACK handler — parse {"type":"NACK","seq":N}
            if (strcmp(topic_buf, "flp/admin/ack") == 0 && event->data &&
                event->data_len > 0)
            {
                // Null-terminate data for safe string operations
                char ack_buf[256];
                size_t alen = (event->data_len < sizeof(ack_buf) - 1)
                                  ? static_cast<size_t>(event->data_len)
                                  : sizeof(ack_buf) - 1;
                memcpy(ack_buf, event->data, alen);
                ack_buf[alen] = '\0';

                if (strstr(ack_buf, "\"NACK\""))
                {
                    const char *seq_str = strstr(ack_buf, "\"seq\":");
                    if (seq_str)
                    {
                        int seq_val = atoi(seq_str + 6);
                        CloudNackItem nack = {};
                        nack.seq = static_cast<uint16_t>(seq_val);
                        if (xQueueSend(nack_queue_, &nack, 0) == pdTRUE)
                        {
                            ESP_LOGI(TAG,
                                     "Cloud NACK received for seq=%u",
                                     nack.seq);
                        }
                    }
                }
            }

            if (rx_callback_ && event->topic && event->data)
            {
                rx_callback_(
                    topic_buf, (const uint8_t *) event->data, event->data_len);
            }
            break;
        }

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error event");
            break;

        default:
            break;
    }
}

int MqttSnClient::publish(uint16_t topic_id,
                          const uint8_t *data,
                          size_t len,
                          int qos)
{
    const char *topic = topic_table_.lookup(topic_id);
    if (!topic)
    {
        ESP_LOGW(TAG, "publish: unknown topic_id=%u", topic_id);
        return -1;
    }

    if (connected_ && client_)
    {
        return esp_mqtt_client_publish(
            client_, topic, (const char *) data, len, qos, 0);
    }

    // Queue for later if not connected
    if (len <= sizeof(MqttPublishItem::data))
    {
        MqttPublishItem item = {};
        item.topic_id = topic_id;
        memcpy(item.data, data, len);
        item.len = len;
        item.qos = qos;
        if (xQueueSend(publish_queue_, &item, 0) != pdTRUE)
        {
            ESP_LOGW(TAG, "publish queue full, dropping message");
            return -1;
        }
    }
    return 0;
}

int MqttSnClient::publish_raw(const char *topic,
                              const uint8_t *data,
                              size_t len,
                              int qos)
{
    if (!connected_ || !client_)
    {
        ESP_LOGW(TAG, "publish_raw: not connected");
        return -1;
    }
    return esp_mqtt_client_publish(
        client_, topic, (const char *) data, len, qos, 0);
}

// ── Task 6: Enqueue file for async cloud upload ──────────────────────────────

void MqttSnClient::publish_file(const char *filename,
                                const uint8_t *data,
                                size_t size,
                                uint16_t src_node)
{
    FilePublishRequest req = {filename, data, size, src_node};
    if (xQueueSend(file_publish_queue_, &req, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "publish_file: file publish queue full, dropping");
    }
    else
    {
        ESP_LOGI(TAG,
                 "Queued file for MQTT upload: %s (%zu bytes)",
                 filename,
                 size);
    }
}

void MqttSnClient::process_file_publish(const FilePublishRequest &req)
{
    if (!connected_ || !client_)
    {
        ESP_LOGW(TAG, "process_file_publish: not connected to broker");
        return;
    }

    uint16_t chunk_count = static_cast<uint16_t>(
        (req.size + MQTT_CHUNK_PAYLOAD - 1) / MQTT_CHUNK_PAYLOAD);
    uint32_t crc = esp_rom_crc32_le(0, req.data, req.size);
    uint32_t session_id = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    char meta_topic[64];
    snprintf(meta_topic, sizeof(meta_topic), "flp/%04x/file/meta", node_addr_);

    char meta_json[256];
    int meta_len = snprintf(
        meta_json,
        sizeof(meta_json),
        "{\"session_id\":%" PRIu32 ",\"filename\":\"%s\",\"total_size\":%u,"
        "\"chunk_count\":%u,\"src_node\":\"0x%04X\",\"crc32\":%" PRIu32 "}",
        session_id,
        req.filename,
        (unsigned) req.size,
        chunk_count,
        req.src_node,
        crc);

    esp_mqtt_client_publish(client_, meta_topic, meta_json, meta_len, 1, 0);
    ESP_LOGI(TAG,
             "Published file meta: %s (%zu bytes, %u chunks)",
             req.filename,
             req.size,
             chunk_count);

    char data_topic[64];
    snprintf(data_topic, sizeof(data_topic), "flp/%04x/file/data", node_addr_);

    uint8_t chunk_buf[2 + MQTT_CHUNK_PAYLOAD];
    for (uint16_t seq = 0; seq < chunk_count; seq++)
    {
        size_t offset = static_cast<size_t>(seq) * MQTT_CHUNK_PAYLOAD;
        size_t remain = req.size - offset;
        size_t chunk_len =
            (remain < MQTT_CHUNK_PAYLOAD) ? remain : MQTT_CHUNK_PAYLOAD;

        chunk_buf[0] = static_cast<uint8_t>(seq & 0xFF);
        chunk_buf[1] = static_cast<uint8_t>((seq >> 8) & 0xFF);
        memcpy(chunk_buf + 2, req.data + offset, chunk_len);

        esp_mqtt_client_publish(
            client_, data_topic, (const char *) chunk_buf, 2 + chunk_len, 1, 0);

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Retain file data pointer for NACK retransmission
    last_file_data_ = req.data;
    last_file_size_ = req.size;

    ESP_LOGI(TAG, "Published all %u file chunks for %s", chunk_count, req.filename);
}

void MqttSnClient::process_nack_retransmit()
{
    if (!connected_ || !client_ || !last_file_data_ || last_file_size_ == 0)
    {
        return;
    }

    CloudNackItem nack;
    while (xQueueReceive(nack_queue_, &nack, 0) == pdTRUE)
    {
        size_t offset = static_cast<size_t>(nack.seq) * MQTT_CHUNK_PAYLOAD;
        if (offset >= last_file_size_)
        {
            ESP_LOGW(TAG,
                     "NACK seq=%u out of range (size=%zu)",
                     nack.seq,
                     last_file_size_);
            continue;
        }

        size_t remain = last_file_size_ - offset;
        size_t chunk_len =
            (remain < MQTT_CHUNK_PAYLOAD) ? remain : MQTT_CHUNK_PAYLOAD;

        char data_topic[64];
        snprintf(
            data_topic, sizeof(data_topic), "flp/%04x/file/data", node_addr_);

        uint8_t chunk_buf[2 + MQTT_CHUNK_PAYLOAD];
        chunk_buf[0] = static_cast<uint8_t>(nack.seq & 0xFF);
        chunk_buf[1] = static_cast<uint8_t>((nack.seq >> 8) & 0xFF);
        memcpy(chunk_buf + 2, last_file_data_ + offset, chunk_len);

        esp_mqtt_client_publish(
            client_, data_topic, (const char *) chunk_buf, 2 + chunk_len, 1, 0);

        ESP_LOGI(TAG, "Retransmitted NACK'd chunk seq=%u", nack.seq);
    }
}

void MqttSnClient::run()
{
    TickType_t last_status_tick = xTaskGetTickCount();
    const TickType_t status_interval = pdMS_TO_TICKS(30000);

    while (true)
    {
        // Drain publish queue when connected
        if (connected_ && client_)
        {
            MqttPublishItem item;
            while (xQueueReceive(publish_queue_, &item, 0) == pdTRUE)
            {
                const char *topic = topic_table_.lookup(item.topic_id);
                if (topic)
                {
                    esp_mqtt_client_publish(client_,
                                            topic,
                                            (const char *) item.data,
                                            item.len,
                                            item.qos,
                                            0);
                }
            }
        }

        // Process file publish requests (one per iteration to stay responsive)
        if (connected_ && client_)
        {
            FilePublishRequest freq;
            if (xQueueReceive(file_publish_queue_, &freq, 0) == pdTRUE)
            {
                process_file_publish(freq);
            }
        }

        // Process cloud NACK retransmissions
        process_nack_retransmit();

        // Periodic status publish (every 30s)
        TickType_t now = xTaskGetTickCount();
        if (connected_ && (now - last_status_tick) >= status_interval)
        {
            last_status_tick = now;
            const char *status_topic =
                topic_table_.lookup(1); // topic ID 1 = status
            if (status_topic)
            {
                const char *msg = "online";
                esp_mqtt_client_publish(
                    client_, status_topic, msg, strlen(msg), 0, 0);
                ESP_LOGD(TAG, "Published status heartbeat");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

} // namespace flp
