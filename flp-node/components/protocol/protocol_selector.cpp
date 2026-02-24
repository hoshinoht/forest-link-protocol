#include "protocol_selector.hpp"

#include <cinttypes>

#include "esp_log.h"
#include "packet.hpp"

static const char *TAG = "proto_sel";

namespace flp
{

void ProtocolSelector::init()
{
    ESP_LOGI(TAG, "ProtocolSelector initialized");
}

void ProtocolSelector::recalculate_bias()
{
    float espnow_rate = espnow_metrics_.success_rate();
    float lora_rate = lora_metrics_.success_rate();

    if (espnow_metrics_.tx_count > 5 && lora_metrics_.tx_count > 5)
    {
        if (espnow_rate > lora_rate + 0.1f)
        {
            reliability_bias_ = 2;
        }
        else if (lora_rate > espnow_rate + 0.1f)
        {
            reliability_bias_ = -2;
        }
        else
        {
            reliability_bias_ = 0;
        }
    }

    ESP_LOGD(TAG,
             "Metrics: ESPNOW(tx=%" PRIu32 " ok=%.0f%% lat=%" PRIu32
             "ms) LoRa(tx=%" PRIu32 " ok=%.0f%% lat=%" PRIu32 "ms) bias=%d",
             espnow_metrics_.tx_count,
             espnow_rate * 100.0f,
             espnow_metrics_.avg_latency_ms(),
             lora_metrics_.tx_count,
             lora_rate * 100.0f,
             lora_metrics_.avg_latency_ms(),
             reliability_bias_);
}

void ProtocolSelector::report_tx_result(Transport t,
                                        bool success,
                                        uint32_t latency_ms)
{
    TransportMetrics &m = (t == Transport::ESPNOW) ? espnow_metrics_ : lora_metrics_;
    m.tx_count++;
    if (!success)
    {
        m.tx_fail_count++;
    }
    m.total_latency_ms += latency_ms;
}

Transport ProtocolSelector::select(int8_t rssi,
                                   uint8_t hop_count,
                                   size_t payload_size,
                                   float battery_pct)
{
    // Hard-gate: LoRa hardware cannot send > 255 bytes total
    if (payload_size > LORA_MAX_PAYLOAD)
    {
        return Transport::ESPNOW;
    }

    int espnow_score = 0, lora_score = 0;

    // RSSI scoring
    if (rssi > -60)
    {
        espnow_score += 3;
    }
    else if (rssi > -80)
    {
        espnow_score += 1;
    }
    else
    {
        lora_score += 3;
    }

    // Hop count scoring
    if (hop_count <= 1)
    {
        espnow_score += 2;
    }
    else if (hop_count > 3)
    {
        lora_score += 3;
    }

    // Payload size scoring
    if (payload_size > LORA_MAX_PAYLOAD / 2)
    {
        espnow_score += 2;
    }

    // Battery scoring — ESP-NOW uses less TX power than LoRa
    if (battery_pct < 20.0f)
    {
        espnow_score += 2;
    }

    // Task 7: Apply reliability bias from feedback loop
    if (reliability_bias_ > 0)
    {
        espnow_score += reliability_bias_;
    }
    else
    {
        lora_score += (-reliability_bias_);
    }

    ESP_LOGD(TAG,
             "select: rssi=%d hops=%u size=%zu batt=%.1f%% -> ESPNOW=%d LoRa=%d",
             rssi,
             hop_count,
             payload_size,
             battery_pct,
             espnow_score,
             lora_score);

    return (espnow_score >= lora_score) ? Transport::ESPNOW : Transport::LORA;
}

} // namespace flp
