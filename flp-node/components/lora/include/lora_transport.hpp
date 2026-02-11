#pragma once

#include <cstdint>
#include <cstddef>

namespace flp {

class LoraTransport {
public:
    LoraTransport() = default;

    void init();
    void deinit();

    // TODO: Send a packet (broadcast by default on LoRa)
    int send(const uint8_t *data, size_t len);

    // TODO: Register receive callback (ISR-safe)
    // using RxCallback = void (*)(const uint8_t *data, size_t len, int rssi);
    // void on_receive(RxCallback cb);

    // TODO: Set frequency, spreading factor, bandwidth
    void configure(uint32_t freq_hz, uint8_t sf, uint32_t bw_hz);

private:
    // TODO: SPI handle, SX127x register state
};

} // namespace flp
