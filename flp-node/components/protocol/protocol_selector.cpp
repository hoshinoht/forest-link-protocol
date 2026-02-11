#include "protocol_selector.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "proto_sel";

namespace flp {

void ProtocolSelector::init()
{
    ESP_LOGI(TAG, "ProtocolSelector initialized");
    // TODO: Load thresholds from NVS or defaults
}

void ProtocolSelector::run()
{
    while (true) {
        // TODO: Monitor link quality metrics
        // TODO: Trigger transport switches when thresholds crossed
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

Transport ProtocolSelector::select(int8_t rssi, uint8_t hop_count, size_t payload_size, float battery_pct)
{
    // TODO: Implement scoring algorithm
    // Heuristic: prefer BLE for high RSSI + short range, LoRa for long range / low power
    ESP_LOGD(TAG, "select: rssi=%d hops=%u size=%zu batt=%.1f%%", rssi, hop_count, payload_size, battery_pct);
    return (rssi > -70 && hop_count <= 2) ? Transport::BLE : Transport::LORA;
}

} // namespace flp
