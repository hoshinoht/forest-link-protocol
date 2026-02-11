#include "mqtt_sn_client.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mqtt_sn";

namespace flp {

void MqttSnClient::init()
{
    ESP_LOGI(TAG, "MQTT-SN client initialized");
    // TODO: Configure MQTT client with broker URI from Kconfig
    // TODO: Register topic table entries
}

void MqttSnClient::run()
{
    while (true) {
        // TODO: Check WiFi connectivity
        // TODO: Process outbound publish queue
        // TODO: Handle incoming subscriptions
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

int MqttSnClient::publish(uint16_t topic_id, const uint8_t *data, size_t len)
{
    // TODO: Publish via MQTT if WiFi connected
    ESP_LOGD(TAG, "publish topic=%u, %zu bytes", topic_id, len);
    return 0;
}

} // namespace flp
