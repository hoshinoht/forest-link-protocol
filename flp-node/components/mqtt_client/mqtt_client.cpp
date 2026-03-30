#include "mqtt_client.hpp"

#include <cinttypes>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "packet.hpp"

static const char *TAG = "mqtt";

/* Max data bytes per MQTT chunk (2-byte seq header + payload) */
static constexpr size_t MQTT_CHUNK_PAYLOAD = 500;
static constexpr UBaseType_t MQTT_PUBLISH_QUEUE_DEPTH = 16;
static constexpr UBaseType_t MQTT_FILE_QUEUE_DEPTH = 2;
static constexpr UBaseType_t MQTT_FRAGMENT_QUEUE_DEPTH = 128;
static constexpr UBaseType_t MQTT_ACK_QUEUE_DEPTH = 32;
static constexpr UBaseType_t MQTT_NACK_QUEUE_DEPTH = 16;
static constexpr UBaseType_t MQTT_CMD_QUEUE_DEPTH = 8;
static constexpr TickType_t MQTT_HEARTBEAT_INTERVAL = pdMS_TO_TICKS(30000);
static constexpr TickType_t MQTT_IDLE_YIELD_TICKS = 1;
static constexpr TickType_t MQTT_CHUNK_PUBLISH_DELAY = pdMS_TO_TICKS(10);

namespace flp
{

void MqttClient::notify()
{
    TaskHandle_t t = task_.load(std::memory_order_relaxed);
    if (t)
    {
        xTaskNotifyGive(t);
    }
}

void MqttClient::init()
{
    /*
     * Large queues (MqttPublishItem=526B, FragmentPublishRequest=546B) are
     * allocated in PSRAM to free ~44 KB of internal SRAM.  Small queues
     * (ACK/NACK/CMD, 2-68 bytes per item) stay in internal RAM.
     */
    publish_queue_ = xQueueCreateWithCaps(
        MQTT_PUBLISH_QUEUE_DEPTH, sizeof(MqttPublishItem),
        MALLOC_CAP_SPIRAM);
    file_publish_queue_ =
        xQueueCreate(MQTT_FILE_QUEUE_DEPTH, sizeof(FilePublishRequest));
    fragment_publish_queue_ = xQueueCreateWithCaps(
        MQTT_FRAGMENT_QUEUE_DEPTH, sizeof(FragmentPublishRequest),
        MALLOC_CAP_SPIRAM);
    ack_queue_ = xQueueCreate(MQTT_ACK_QUEUE_DEPTH, sizeof(CloudAckItem));
    nack_queue_ = xQueueCreate(MQTT_NACK_QUEUE_DEPTH, sizeof(CloudNackItem));
    cmd_queue_ = xQueueCreate(MQTT_CMD_QUEUE_DEPTH, sizeof(MeshCmdItem));
    fragment_ack_queue_ = xQueueCreate(MQTT_FRAGMENT_QUEUE_DEPTH, sizeof(uint16_t));

    if (!publish_queue_ || !file_publish_queue_ || !fragment_publish_queue_ ||
        !ack_queue_ || !nack_queue_ || !cmd_queue_ || !fragment_ack_queue_)
    {
        ESP_LOGE(TAG, "Failed to create one or more MQTT queues");
        return;
    }

    /* Configure and start ESP-IDF MQTT client */
    esp_mqtt_client_config_t mqtt_cfg = {};
#if !CONFIG_FLP_WIFI_DISABLED
    mqtt_cfg.broker.address.uri = CONFIG_FLP_MQTT_BROKER_URI;
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    mqtt_cfg.credentials.username = CONFIG_FLP_MQTT_USERNAME;
    mqtt_cfg.credentials.authentication.password = CONFIG_FLP_MQTT_PASSWORD;
#endif
    /*
     * The default ESP-MQTT internal task stack (6144) is too small for WSS
     * transport (mbedtls + WebSocket + VFS select). Stack overflow corrupts
     * adjacent heap metadata, causing "free() target pointer is outside heap
     * areas" in usb_serial_jtag_end_select during esp_vfs_select().
     */
    mqtt_cfg.task.stack_size = 8192;
    mqtt_cfg.outbox.limit = 32768; /* headroom for QoS 1 in-flight fragments */

    client_ = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(
        client_, MQTT_EVENT_ANY, mqtt_event_handler, this);
    esp_mqtt_client_start(client_);

    /* Register predefined topics */
    register_default_topics();

#if !CONFIG_FLP_WIFI_DISABLED
    ESP_LOGI(
        TAG, "MQTT client initialized, broker=%s", CONFIG_FLP_MQTT_BROKER_URI);
#else
    ESP_LOGI(TAG, "MQTT client initialized (no broker)");
#endif
}

void MqttClient::register_default_topics()
{
    char topic_buf[64];

    /* Topic 1: flp/<node_id>/status (Node->Cloud, QoS 0) */
    snprintf(topic_buf, sizeof(topic_buf), "flp/%04x/status", node_addr_);
    status_topic_id_ = topic_table_.register_topic(topic_buf);

    /* Topic 2: flp/<node_id>/file/data (Node->Cloud, QoS 1) */
    snprintf(topic_buf, sizeof(topic_buf), "flp/%04x/file/data", node_addr_);
    file_data_topic_id_ = topic_table_.register_topic(topic_buf);

    /* Topic 3: flp/<node_id>/file/meta (Node->Cloud, QoS 1) */
    snprintf(topic_buf, sizeof(topic_buf), "flp/%04x/file/meta", node_addr_);
    file_meta_topic_id_ = topic_table_.register_topic(topic_buf);

    /* Topic 4: flp/admin/cmd (Cloud->Node, QoS 1) */
    topic_table_.register_topic("flp/admin/cmd");

    /* Topic 5: flp/admin/ack/+ (Cloud->Node, QoS 1)
     * D2 fix: session-scoped ACK topic — subscribe with wildcard */
    topic_table_.register_topic("flp/admin/ack/+");
}

void MqttClient::mqtt_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data)
{
    auto *self = static_cast<MqttClient *>(handler_args);
    auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
    self->handle_mqtt_event(event);
}

void MqttClient::handle_mqtt_event(esp_mqtt_event_handle_t event)
{
    if (!event)
    {
        ESP_LOGW(TAG, "MQTT event handler called with null event");
        return;
    }

    switch (event->event_id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker");
            connected_ = true;
            /* Subscribe to admin topics */
            esp_mqtt_client_subscribe(client_, "flp/admin/cmd", 1);
            /* D2 fix: session-scoped ACK topic */
            esp_mqtt_client_subscribe(client_, "flp/admin/ack/+", 1);
            /* Fix 13: unblock any task waiting for first MQTT connection */
            if (connected_event_group_)
            {
                xEventGroupSetBits(connected_event_group_,
                                   connected_event_bit_);
            }
            notify(); /* wake run() to drain queued items */
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

            /* Null-terminate topic for comparison and callback */
            char topic_buf[128];
            size_t tlen = (event->topic_len < sizeof(topic_buf) - 1)
                              ? static_cast<size_t>(event->topic_len)
                              : sizeof(topic_buf) - 1;
            if (event->topic)
            {
                memcpy(topic_buf, event->topic, tlen);
            }
            topic_buf[tlen] = '\0';

            /*
             * Admin command handler — binary format:
             * [target:2LE][cmd_id:1][data:N]
             */
            if (strcmp(topic_buf, "flp/admin/cmd") == 0 && event->data &&
                event->data_len >= 3)
            {
                MeshCmdItem cmd = {};
                memcpy(&cmd.target_addr, event->data, 2); /* little-endian */
                cmd.cmd_id = static_cast<uint8_t>(event->data[2]);
                cmd.data_len = static_cast<size_t>(event->data_len) - 3;
                if (cmd.data_len > sizeof(cmd.data))
                {
                    cmd.data_len = sizeof(cmd.data);
                }
                if (cmd.data_len > 0)
                {
                    memcpy(cmd.data, event->data + 3, cmd.data_len);
                }

                ESP_LOGI(TAG,
                         "Admin cmd: target=0x%04X cmd=%u len=%zu",
                         cmd.target_addr,
                         cmd.cmd_id,
                         cmd.data_len);

                if (cmd_queue_ && xQueueSend(cmd_queue_, &cmd, 0) != pdTRUE)
                {
                    ESP_LOGW(TAG, "Cmd queue full, dropping");
                }
            }

            /* Cloud ACK/NACK handler — parse {"type":"ACK"|"NACK","seq":N}
             * D2 fix: topic is now flp/admin/ack/<session_id> */
            else if (strncmp(topic_buf, "flp/admin/ack/", 14) == 0 &&
                     event->data && event->data_len > 0)
            {
                /* Null-terminate data for safe string operations */
                char ack_buf[256];
                size_t alen = (event->data_len < sizeof(ack_buf) - 1)
                                  ? static_cast<size_t>(event->data_len)
                                  : sizeof(ack_buf) - 1;
                memcpy(ack_buf, event->data, alen);
                ack_buf[alen] = '\0';

                const char *seq_str = strstr(ack_buf, "\"seq\":");
                if (seq_str)
                {
                    char *end = nullptr;
                    long seq_val = strtol(seq_str + 6, &end, 10);
                    if (end == seq_str + 6 || seq_val < 0 || seq_val > UINT16_MAX)
                    {
                        ESP_LOGW(TAG, "Malformed seq in cloud ACK/NACK");
                    }
                    else if (strstr(ack_buf, "\"ACK\""))
                    {
                        CloudAckItem ack_item = {};
                        ack_item.seq = static_cast<uint16_t>(seq_val);
                        if (xQueueSend(ack_queue_, &ack_item, 0) == pdTRUE)
                        {
                            ESP_LOGD(TAG,
                                     "Cloud ACK received for seq=%u",
                                     ack_item.seq);
                        }
                    }
                    else if (strstr(ack_buf, "\"NACK\""))
                    {
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

            /* D1+B3 fix: cloud-to-exit NACK bridge.
             * Topic: flp/admin/transfer_nack/<session_id>
             * Payload: {"session_id":"N","seqs":[s1,s2,...]}
             * Enqueue each seq into nack_queue_ so TransferEngine can
             * send mesh NACKs back to source for retransmission. */
            else if (strncmp(topic_buf, "flp/admin/transfer_nack/", 24) == 0 &&
                     event->data && event->data_len > 0)
            {
                char nack_buf[512];
                size_t nlen = (event->data_len < sizeof(nack_buf) - 1)
                                  ? static_cast<size_t>(event->data_len)
                                  : sizeof(nack_buf) - 1;
                memcpy(nack_buf, event->data, nlen);
                nack_buf[nlen] = '\0';

                /* Simple JSON array parser for "seqs":[...] */
                const char *seqs_str = strstr(nack_buf, "\"seqs\":[");
                if (seqs_str)
                {
                    const char *p = seqs_str + 8; /* skip "seqs":[ */
                    int queued = 0;
                    while (*p && *p != ']')
                    {
                        char *end = nullptr;
                        long val = strtol(p, &end, 10);
                        if (end == p)
                        {
                            p++;
                            continue;
                        }
                        if (val >= 0 && val <= UINT16_MAX && nack_queue_)
                        {
                            CloudNackItem nack = {};
                            nack.seq = static_cast<uint16_t>(val);
                            if (xQueueSend(nack_queue_, &nack, 0) == pdTRUE)
                            {
                                queued++;
                            }
                        }
                        p = end;
                        if (*p == ',') p++;
                    }
                    if (queued > 0)
                    {
                        ESP_LOGI(TAG,
                                 "Cloud transfer NACK: enqueued %d seqs for retx",
                                 queued);
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

int MqttClient::publish(uint16_t topic_id,
                        const uint8_t *data,
                        size_t len,
                        int qos)
{
    if ((data == nullptr) && (len > 0U))
    {
        ESP_LOGW(TAG, "publish: null data with len=%zu", len);
        return -1;
    }

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

    /* Queue for later if not connected */
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

int MqttClient::publish_raw(const char *topic,
                            const uint8_t *data,
                            size_t len,
                            int qos)
{
    if ((data == nullptr) && (len > 0U))
    {
        ESP_LOGW(TAG, "publish_raw: null data with len=%zu", len);
        return -1;
    }

    if (!connected_ || !client_)
    {
        ESP_LOGW(TAG, "publish_raw: not connected");
        return -1;
    }
    return esp_mqtt_client_publish(
        client_, topic, (const char *) data, len, qos, 0);
}

/* ── Task 6: Enqueue file for async cloud upload ────────────────────────────── */

void MqttClient::publish_file(const char *filename,
                              const uint8_t *data,
                              size_t size,
                              uint16_t src_node)
{
    if (!filename || !data || (size == 0U))
    {
        ESP_LOGW(TAG, "publish_file: invalid args");
        return;
    }

    FilePublishRequest req = {};
    strncpy(req.filename, filename, sizeof(req.filename) - 1);
    req.filename[sizeof(req.filename) - 1] = '\0';
    size_t copy_len = (size <= sizeof(req.data)) ? size : sizeof(req.data);
    memcpy(req.data, data, copy_len);
    req.size = copy_len;
    req.src_node = src_node;
    if (xQueueSend(file_publish_queue_, &req, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "publish_file: file publish queue full, dropping");
    }
    else
    {
        ESP_LOGI(
            TAG, "Queued file for MQTT upload: %s (%zu bytes)", filename, size);
        notify();
    }
}

void MqttClient::process_file_publish(const FilePublishRequest &req)
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

    if (meta_len <= 0 || meta_len >= (int)sizeof(meta_json))
    {
        ESP_LOGE(TAG, "process_file_publish: meta_json truncated or error");
        return;
    }
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

        vTaskDelay(MQTT_CHUNK_PUBLISH_DELAY);
    }

    ESP_LOGI(
        TAG, "Published all %u file chunks for %s", chunk_count, req.filename);
}

void MqttClient::publish_transfer_meta(uint16_t session_id,
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
    int meta_len = snprintf(meta_json,
                            sizeof(meta_json),
                            "{\"session_id\":%u,\"filename\":\"%s\","
                            "\"src_node\":\"0x%04X\",\"exit_node\":\"0x%04X\","
                            "\"total_size\":%" PRIu32 ",\"chunk_count\":%u,"
                            "\"fragment_size\":%u,\"crc32\":%" PRIu32 "}",
                            session_id,
                            filename,
                            src_node,
                            exit_node,
                            total_size,
                            chunk_count,
                            fragment_size,
                            crc32);

    if (meta_len <= 0 || meta_len >= (int)sizeof(meta_json))
    {
        ESP_LOGE(TAG, "publish_transfer_meta: meta_json truncated or error");
        return;
    }
    if (connected_ && client_)
    {
        esp_mqtt_client_publish(client_, meta_topic, meta_json, meta_len, 1, 0);

        /* D1+B3 fix: subscribe to cloud-to-exit NACK bridge topic so the
         * cloud can request retransmission for fragments lost in transit. */
        char nack_topic[64];
        snprintf(nack_topic, sizeof(nack_topic),
                 "flp/admin/transfer_nack/%u", session_id);
        esp_mqtt_client_subscribe(client_, nack_topic, 1);
        ESP_LOGI(TAG, "Subscribed to %s", nack_topic);
    }

    ESP_LOGI(TAG,
             "Published transfer meta: session=%u"
             " file=%s size=%" PRIu32 " chunks=%u crc=%" PRIu32,
             session_id,
             filename,
             total_size,
             chunk_count,
             crc32);
}

bool MqttClient::publish_fragment(uint16_t session_id,
                                  uint16_t seq,
                                  uint16_t src_node,
                                  const uint8_t *data,
                                  size_t len,
                                  const char *filename)
{
    if (!data || !filename || (len == 0U))
    {
        ESP_LOGW(TAG, "publish_fragment: invalid args for seq=%u", seq);
        return false;
    }

    FragmentPublishRequest req = {};
    req.session_id = session_id;
    req.seq = seq;
    req.src_node = src_node;
    if (len > MAX_MTU)
    {
        ESP_LOGW(TAG,
                 "publish_fragment: len %zu > MAX_MTU, truncating", len);
        len = MAX_MTU;
    }
    memcpy(req.data, data, len);
    req.len = len;
    strncpy(req.filename, filename, sizeof(req.filename) - 1);
    req.filename[sizeof(req.filename) - 1] = '\0';

    if (xQueueSend(fragment_publish_queue_, &req, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "Fragment publish queue full, NACK seq=%u", seq);
        return false;
    }

    notify();
    return true;
}

void MqttClient::process_fragment_publish()
{
    if (!connected_ || !client_)
    {
        return;
    }

    /*
     * Limit publishes per run() iteration so we don't stuff the ESP-MQTT
     * outbox faster than TLS can drain it.  Each fragment is ~250 bytes
     * with QoS 1 overhead; the outbox is 32 KB.  Cap at 8 per tick to
     * keep outbox utilisation low and avoid the "outbox full, deferring"
     * stalls that delay deferred mesh ACKs.
     */
    static constexpr uint8_t MAX_PUBLISHES_PER_TICK = 8;
    uint8_t published = 0;

    FragmentPublishRequest req;
    while (published < MAX_PUBLISHES_PER_TICK &&
           xQueueReceive(fragment_publish_queue_, &req, 0) == pdTRUE)
    {
        /* B4 fix: Publish chunk with session ID prefix:
         * [session_id:2LE][seq:2LE][data] — enables cloud to filter
         * stale chunks from previous sessions. */
        char data_topic[64];
        snprintf(
            data_topic, sizeof(data_topic), "flp/%04x/file/data", node_addr_);

        uint8_t chunk_buf[4 + MAX_MTU];
        chunk_buf[0] = static_cast<uint8_t>(req.session_id & 0xFF);
        chunk_buf[1] = static_cast<uint8_t>((req.session_id >> 8) & 0xFF);
        chunk_buf[2] = static_cast<uint8_t>(req.seq & 0xFF);
        chunk_buf[3] = static_cast<uint8_t>((req.seq >> 8) & 0xFF);
        memcpy(chunk_buf + 4, req.data, req.len);

        int msg_id = esp_mqtt_client_publish(
            client_, data_topic, (const char *) chunk_buf, 4 + req.len, 1, 0);

        if (msg_id < 0)
        {
            /* Outbox full — put fragment back and retry next tick */
            ESP_LOGW(TAG,
                     "MQTT outbox full, deferring seq=%u",
                     req.seq);
            if (xQueueSendToFront(fragment_publish_queue_, &req, 0) != pdTRUE)
            {
                ESP_LOGE(TAG,
                         "Fragment re-queue failed, seq=%u dropped", req.seq);
            }
            break;
        }

        ESP_LOGI(TAG,
                 "Published fragment seq=%u len=%zu msg_id=%d",
                 req.seq,
                 req.len,
                 msg_id);

        /* Signal that this fragment was successfully handed to ESP-MQTT.
         * TransferEngine drains this queue and sends mesh ACKs, providing
         * end-to-end backpressure: sender can't outrun MQTT throughput.
         *
         * Only push for relay-forwarded fragments (src_node != our addr).
         * Local-exit fragments have their own cloud ACK path (ack_queue_)
         * and must NOT pollute the deferred mesh-ACK queue — otherwise
         * the relay receives ACKs with wrong seq numbers. */
        if (fragment_ack_queue_ && req.src_node != node_addr_)
        {
            uint16_t ack_seq = req.seq;
            xQueueSend(fragment_ack_queue_, &ack_seq, 0);
        }
        published++;
    }
}

void MqttClient::run()
{
    task_.store(xTaskGetCurrentTaskHandle(), std::memory_order_relaxed);

    TickType_t last_status_tick = xTaskGetTickCount();
    const TickType_t status_interval = MQTT_HEARTBEAT_INTERVAL;

    while (true)
    {
        /*
         * Block until a producer notifies us or the heartbeat interval elapses.
         * Compute remaining time until next heartbeat so we never oversleep it.
         * Floor of 1 tick prevents spin when heartbeat is due or notifications
         * are pending (ulTaskNotifyTake returns immediately if count > 0).
         */
        TickType_t now = xTaskGetTickCount();
        TickType_t elapsed = now - last_status_tick;
        TickType_t wait =
            (elapsed >= status_interval) ? 1 : (status_interval - elapsed);
        if (wait == 0)
        {
            wait = 1;
        }
        ulTaskNotifyTake(pdTRUE, wait);

        /* Drain publish queue when connected */
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

        /* Process file publish requests (one per iteration to stay responsive) */
        if (connected_ && client_)
        {
            FilePublishRequest freq;
            if (xQueueReceive(file_publish_queue_, &freq, 0) == pdTRUE)
            {
                process_file_publish(freq);
            }
        }

        /* Process fragment publish requests (exit node mode) */
        process_fragment_publish();

        /* Periodic status heartbeat */
        now = xTaskGetTickCount();
        if ((now - last_status_tick) >= status_interval)
        {
            last_status_tick = now;
            if (connected_)
            {
                const char *status_topic =
                    topic_table_.lookup(status_topic_id_);
                if (status_topic)
                {
                    const char *msg = "online";
                    esp_mqtt_client_publish(
                        client_, status_topic, msg, strlen(msg), 0, 0);
                    ESP_LOGD(TAG, "Published status heartbeat");
                }
            }
        }

        /*
         * Yield for 1 tick to guarantee IDLE task runs and feeds the
         * watchdog, even if notifications arrive every iteration.
         */
        vTaskDelay(MQTT_IDLE_YIELD_TICKS);
    }
}

bool MqttClient::receive_cmd(MeshCmdItem &out)
{
    if (!cmd_queue_)
    {
        return false;
    }
    return xQueueReceive(cmd_queue_, &out, 0) == pdTRUE;
}

bool MqttClient::drain_cloud_ack(uint16_t &seq_out)
{
    if (!ack_queue_)
    {
        return false;
    }
    CloudAckItem item;
    if (xQueueReceive(ack_queue_, &item, 0) == pdTRUE)
    {
        seq_out = item.seq;
        return true;
    }
    return false;
}

bool MqttClient::drain_cloud_nack(uint16_t &seq_out)
{
    if (!nack_queue_)
    {
        return false;
    }
    CloudNackItem item;
    if (xQueueReceive(nack_queue_, &item, 0) == pdTRUE)
    {
        seq_out = item.seq;
        return true;
    }
    return false;
}

bool MqttClient::drain_fragment_ack(uint16_t &seq_out)
{
    if (!fragment_ack_queue_)
    {
        return false;
    }
    return xQueueReceive(fragment_ack_queue_, &seq_out, 0) == pdTRUE;
}

} /* namespace flp */
