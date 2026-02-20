#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

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
    uint32_t retransmit_count = 0;
    uint32_t rx_count = 0;
    uint32_t duty_cycle_ms = 0;

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

    size_t serialize_metrics(uint8_t *buf, size_t max_len) const
    {
        // Format: [{tx:4, fail:4, latency:4, retx:4, rx:4, duty:4}*2] = 48 bytes
        if (max_len < 48)
            return 0;
        auto write = [&](size_t off, const TransportMetrics &m)
        {
            memcpy(buf + off, &m.tx_count, 4);
            memcpy(buf + off + 4, &m.tx_fail_count, 4);
            memcpy(buf + off + 8, &m.total_latency_ms, 4);
            memcpy(buf + off + 12, &m.retransmit_count, 4);
            memcpy(buf + off + 16, &m.rx_count, 4);
            memcpy(buf + off + 20, &m.duty_cycle_ms, 4);
        };
        write(0, ble_metrics_);
        write(24, lora_metrics_);
        return 48;
    }

  private:
    TransportMetrics ble_metrics_;
    TransportMetrics lora_metrics_;
    int8_t reliability_bias_ = 0; // +ve favors BLE, -ve favors LoRa
};

} // namespace flp
