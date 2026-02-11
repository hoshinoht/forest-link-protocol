#pragma once

#include <cstdint>

namespace flp {

enum class Transport : uint8_t {
    BLE,
    LORA,
};

class ProtocolSelector {
public:
    ProtocolSelector() = default;

    void init();
    void run(); // main loop — called from FreeRTOS task

    // TODO: Select optimal transport based on power budget, RSSI, hop count, packet size
    Transport select(int8_t rssi, uint8_t hop_count, size_t payload_size, float battery_pct);

private:
    // TODO: Internal scoring state, thresholds
};

} // namespace flp
