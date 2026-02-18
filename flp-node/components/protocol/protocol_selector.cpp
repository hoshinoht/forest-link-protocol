#include "protocol_selector.hpp"
#include "packet.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cinttypes>

static const char *TAG = "proto_sel";

namespace flp {

void ProtocolSelector::init()
{
    ESP_LOGI(TAG, "ProtocolSelector initialized");
}

void ProtocolSelector::run()
{
    while (true) {
        // Task 7: Recalculate reliability bias every 10s based on recent success rates
        float ble_rate = ble_metrics_.success_rate();
        float lora_rate = lora_metrics_.success_rate();

        if (ble_metrics_.tx_count > 5 && lora_metrics_.tx_count > 5) {
            if (ble_rate > lora_rate + 0.1f) {
                reliability_bias_ = 2;
            } else if (lora_rate > ble_rate + 0.1f) {
                reliability_bias_ = -2;
            } else {
                reliability_bias_ = 0;
            }
        }

        ESP_LOGD(TAG, "Metrics: BLE(tx=%" PRIu32 " ok=%.0f%% lat=%" PRIu32 "ms) LoRa(tx=%" PRIu32 " ok=%.0f%% lat=%" PRIu32 "ms) bias=%d",
                 ble_metrics_.tx_count, ble_rate * 100.0f, ble_metrics_.avg_latency_ms(),
                 lora_metrics_.tx_count, lora_rate * 100.0f, lora_metrics_.avg_latency_ms(),
                 reliability_bias_);

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void ProtocolSelector::report_tx_result(Transport t, bool success, uint32_t latency_ms)
{
    TransportMetrics &m = (t == Transport::BLE) ? ble_metrics_ : lora_metrics_;
    m.tx_count++;
    if (!success) m.tx_fail_count++;
    m.total_latency_ms += latency_ms;
}

Transport ProtocolSelector::select(int8_t rssi, uint8_t hop_count,
                                   size_t payload_size, float battery_pct)
{
    // Hard-gate: LoRa hardware cannot send > 255 bytes total
    if (payload_size > LORA_MAX_PAYLOAD) {
        return Transport::BLE;
    }

    int ble_score = 0, lora_score = 0;

    // RSSI scoring
    if (rssi > -60) {
        ble_score += 3;
    } else if (rssi > -80) {
        ble_score += 1;
    } else {
        lora_score += 3;
    }

    // Hop count scoring
    if (hop_count <= 1) {
        ble_score += 2;
    } else if (hop_count > 3) {
        lora_score += 3;
    }

    // Payload size scoring
    if (payload_size > LORA_MAX_PAYLOAD / 2) {
        ble_score += 2;
    }

    // Battery scoring — BLE uses less TX power
    if (battery_pct < 20.0f) {
        ble_score += 2;
    }

    // Task 7: Apply reliability bias from feedback loop
    if (reliability_bias_ > 0) {
        ble_score += reliability_bias_;
    } else {
        lora_score += (-reliability_bias_);
    }

    ESP_LOGD(TAG, "select: rssi=%d hops=%u size=%zu batt=%.1f%% -> BLE=%d LoRa=%d",
             rssi, hop_count, payload_size, battery_pct, ble_score, lora_score);

    return (ble_score >= lora_score) ? Transport::BLE : Transport::LORA;
}

} // namespace flp
