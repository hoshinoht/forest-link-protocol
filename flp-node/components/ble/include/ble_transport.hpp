#pragma once

#include <cstdint>
#include <cstddef>

namespace flp {

class BleTransport {
public:
    BleTransport() = default;

    void init();
    void deinit();

    // TODO: Send data to a connected peer
    int send(uint16_t peer_addr, const uint8_t *data, size_t len);

    // TODO: Register receive callback
    // using RxCallback = void (*)(uint16_t src_addr, const uint8_t *data, size_t len);
    // void on_receive(RxCallback cb);

    // TODO: Start/stop scanning and advertising
    void start_scan();
    void stop_scan();
    void start_advertise();

private:
    // TODO: NimBLE handles, connection state
};

} // namespace flp
