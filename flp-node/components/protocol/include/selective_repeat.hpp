#pragma once

#include <cstdint>
#include <cstddef>

namespace flp {

class SelectiveRepeat {
public:
    SelectiveRepeat() = default;

    // TODO: Initialize ARQ window
    void init(uint8_t window_size, uint32_t timeout_ms);

    // TODO: Submit a fragment for reliable delivery
    int send_fragment(uint16_t seq, const uint8_t *data, size_t len);

    // TODO: Process incoming ACK/NACK
    void handle_ack(uint16_t seq);
    void handle_nack(uint16_t seq);

    // TODO: Check for timeouts, retransmit as needed
    void tick();

private:
    uint8_t  window_size_ = 0;
    uint32_t timeout_ms_ = 0;
    // TODO: Sliding window state, fragment buffer
};

} // namespace flp
