#pragma once

#include <cstddef>
#include <cstdint>

namespace flp
{

enum class Transport : uint8_t
{
    BLE,
    LORA,
};

// Task 7: Runtime transport metrics
struct TransportMetrics
{
    uint32_t tx_count = 0;
    uint32_t tx_fail_count = 0;
    uint32_t total_latency_ms = 0;

    uint32_t avg_latency_ms() const
    {
        return (tx_count > 0) ? (total_latency_ms / tx_count) : 0;
    }

    float success_rate() const
    {
        return (tx_count > 0)
                   ? (1.0f - static_cast<float>(tx_fail_count) / tx_count)
                   : 1.0f;
    }
};

class ProtocolSelector
{
  public:
    ProtocolSelector() = default;

    void init();
    void recalculate_bias(); // call periodically from MeshManager::run()

    Transport select(int8_t rssi,
                     uint8_t hop_count,
                     size_t payload_size,
                     float battery_pct);

    // Task 7: Report TX outcome for feedback loop
    void report_tx_result(Transport t, bool success, uint32_t latency_ms);

  private:
    TransportMetrics ble_metrics_;
    TransportMetrics lora_metrics_;
    int8_t reliability_bias_ = 0; // +ve favors BLE, -ve favors LoRa
};

} // namespace flp
