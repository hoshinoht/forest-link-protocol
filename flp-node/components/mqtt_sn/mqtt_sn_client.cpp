#include "mqtt_sn_client.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "packet.hpp"
#include "freertos/FreeRTOS.h"

static const char *TAG = "mqtt_sn";

// Max data bytes per MQTT chunk (2-byte seq header + payload)
static constexpr size_t MQTT_CHUNK_PAYLOAD = 500;

namespace flp
{

void MqttSnClient::notify()
{
    if (task_)
    {
        xTaskNotifyGive(task_);
    }
}

void MqttSnClient::init()
{
    // Create publish queues
    publish_queue_ = xQueueCreate(16, sizeof(MqttPublishItem));
    file_publish_queue_ = xQueueCreate(2, sizeof(FilePublishRequest));
    fragment_publish_queue_ = xQueueCreate(64, sizeof(FragmentPublishRequest));
    nack_queue_ = xQueueCreate(16, sizeof(CloudNackItem));
    cmd_queue_ = xQueueCreate(8, sizeof(MeshCmdItem));

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
            notify(); // wake run() to drain queued items
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

            // Admin command handler — binary format: [target:2LE][cmd_id:1][data:N]
            if (strcmp(topic_buf, "flp/admin/cmd") == 0 && event->data &&
                event->data_len >= 3)
            {
                MeshCmdItem cmd = {};
                memcpy(&cmd.target_addr, event->data, 2); // little-endian
                cmd.cmd_id = static_cast<uint8_t>(event->data[2]);
                cmd.data_len = static_cast<size_t>(event->data_len) - 3;
                if (cmd.data_len > sizeof(cmd.data))
                    cmd.data_len = sizeof(cmd.data);
                if (cmd.data_len > 0)
                    memcpy(cmd.data, event->data + 3, cmd.data_len);

                ESP_LOGI(TAG,
                         "Admin cmd: target=0x%04X cmd=%u len=%zu",
                         cmd.target_addr,
                         cmd.cmd_id,
                         cmd.data_len);

                if (cmd_queue_ &&
                    xQueueSend(cmd_queue_, &cmd, 0) != pdTRUE)
                {
                    ESP_LOGW(TAG, "Cmd queue full, dropping");
                }
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
                            notify();
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
        notify();
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
        notify();
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

void MqttSnClient::publish_transfer_meta(uint32_t session_id,
                                          const char *filename,
                                          uint16_t src_node,
                                          uint16_t exit_node,
                                          uint32_t total_size,
                                          uint16_t chunk_count,
                                          uint16_t fragment_size,
                                          uint32_t crc32)
{
    char meta_topic[64];
    snprintf(meta_topic, sizeof(meta_topic), "flp/%04x/file/meta", node_addr_);

    char meta_json[256];
    int meta_len = snprintf(
        meta_json, sizeof(meta_json),
        "{\"session_id\":%" PRIu32 ",\"filename\":\"%s\","
        "\"src_node\":\"0x%04X\",\"exit_node\":\"0x%04X\","
        "\"total_size\":%" PRIu32 ",\"chunk_count\":%u,"
        "\"fragment_size\":%u,\"crc32\":%" PRIu32 "}",
        session_id, filename, src_node, exit_node,
        total_size, chunk_count, fragment_size, crc32);

    if (connected_ && client_)
    {
        esp_mqtt_client_publish(client_, meta_topic, meta_json, meta_len, 1, 0);
    }

    // Mark meta as published so process_fragment_publish doesn't re-publish
    meta_published_ = true;
    last_meta_session_id_ = session_id;

    ESP_LOGI(TAG,
             "Published transfer meta: session=%" PRIu32 " file=%s size=%" PRIu32
             " chunks=%u crc=%" PRIu32,
             session_id, filename, total_size, chunk_count, crc32);
}

void MqttSnClient::publish_fragment(uint32_t session_id, uint16_t seq,
                                     uint16_t src_node, const uint8_t *data,
                                     size_t len, const char *filename)
{
    FragmentPublishRequest req = {};
    req.session_id = session_id;
    req.seq = seq;
    req.src_node = src_node;
    if (len > MAX_MTU) len = MAX_MTU;
    memcpy(req.data, data, len);
    req.len = len;
    strncpy(req.filename, filename, sizeof(req.filename) - 1);

    if (xQueueSend(fragment_publish_queue_, &req, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "Fragment publish queue full, dropping seq=%u", seq);
    }
    else
    {
        notify();
    }
}

void MqttSnClient::process_fragment_publish()
{
    if (!connected_ || !client_)
        return;

    FragmentPublishRequest req;
    while (xQueueReceive(fragment_publish_queue_, &req, 0) == pdTRUE)
    {
        // Publish meta on first fragment of a new session
        if (!meta_published_ || last_meta_session_id_ != req.session_id)
        {
            char meta_topic[64];
            snprintf(meta_topic, sizeof(meta_topic), "flp/%04x/file/meta", node_addr_);

            char meta_json[256];
            int meta_len = snprintf(meta_json, sizeof(meta_json),
                "{\"session_id\":%" PRIu32 ",\"filename\":\"%s\","
                "\"src_node\":\"0x%04X\",\"exit_node\":\"0x%04X\"}",
                req.session_id, req.filename, req.src_node, node_addr_);

            esp_mqtt_client_publish(client_, meta_topic, meta_json, meta_len, 1, 0);
            meta_published_ = true;
            last_meta_session_id_ = req.session_id;
            ESP_LOGI(TAG, "Published fragment meta: session=%" PRIu32 " file=%s",
                     req.session_id, req.filename);
        }

        // Publish chunk: [2-byte seq_le][data]
        char data_topic[64];
        snprintf(data_topic, sizeof(data_topic), "flp/%04x/file/data", node_addr_);

        uint8_t chunk_buf[2 + MAX_MTU];
        chunk_buf[0] = static_cast<uint8_t>(req.seq & 0xFF);
        chunk_buf[1] = static_cast<uint8_t>((req.seq >> 8) & 0xFF);
        memcpy(chunk_buf + 2, req.data, req.len);

        esp_mqtt_client_publish(client_, data_topic,
                                (const char *)chunk_buf, 2 + req.len, 1, 0);

        ESP_LOGD(TAG, "Published fragment seq=%u len=%zu", req.seq, req.len);
    }
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
    task_ = xTaskGetCurrentTaskHandle();

    TickType_t last_status_tick = xTaskGetTickCount();
    const TickType_t status_interval = pdMS_TO_TICKS(30000);

    while (true)
    {
        // Block until a producer notifies us or the heartbeat interval elapses.
        // Compute remaining time until next heartbeat so we never oversleep it.
        // Floor of 1 tick prevents spin when heartbeat is due or notifications
        // are pending (ulTaskNotifyTake returns immediately if count > 0).
        TickType_t now = xTaskGetTickCount();
        TickType_t elapsed = now - last_status_tick;
        TickType_t wait =
            (elapsed >= status_interval) ? 1 : (status_interval - elapsed);
        if (wait == 0)
        {
            wait = 1;
        }
        ulTaskNotifyTake(pdTRUE, wait);

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

        // Process fragment publish requests (exit node mode)
        process_fragment_publish();

        // Process cloud NACK retransmissions
        process_nack_retransmit();

        // Periodic status heartbeat
        now = xTaskGetTickCount();
        if ((now - last_status_tick) >= status_interval)
        {
            last_status_tick = now;
            if (connected_)
            {
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
        }

        // Yield for 1 tick to guarantee IDLE task runs and feeds the
        // watchdog, even if notifications arrive every iteration.
        vTaskDelay(1);
    }
}

bool MqttSnClient::receive_cmd(MeshCmdItem &out)
{
    if (!cmd_queue_)
        return false;
    return xQueueReceive(cmd_queue_, &out, 0) == pdTRUE;
}

} // namespace flp
