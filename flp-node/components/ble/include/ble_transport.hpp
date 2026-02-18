#pragma once

#include <cstdint>
#include <cstddef>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/ble_hs.h"

namespace flp {

static constexpr int BLE_MAX_CONNECTIONS = 4;
static constexpr int BLE_RX_QUEUE_DEPTH = 16;

// FLP custom 128-bit service UUID (little-endian)
// Full: 6f7a1001-b5d8-4a3f-9c12-e3b8f4c0d1a2
static const ble_uuid128_t kFlpServiceUuid =
    BLE_UUID128_INIT(0xa2, 0xd1, 0xc0, 0xf4, 0xb8, 0xe3, 0x12, 0x9c,
                     0x3f, 0x4a, 0xd8, 0xb5, 0x01, 0x10, 0x7a, 0x6f);

// TX characteristic UUID (notify)
// 6f7a1002-b5d8-4a3f-9c12-e3b8f4c0d1a2
static const ble_uuid128_t kFlpTxCharUuid =
    BLE_UUID128_INIT(0xa2, 0xd1, 0xc0, 0xf4, 0xb8, 0xe3, 0x12, 0x9c,
                     0x3f, 0x4a, 0xd8, 0xb5, 0x02, 0x10, 0x7a, 0x6f);

// RX characteristic UUID (write-no-response)
// 6f7a1003-b5d8-4a3f-9c12-e3b8f4c0d1a2
static const ble_uuid128_t kFlpRxCharUuid =
    BLE_UUID128_INIT(0xa2, 0xd1, 0xc0, 0xf4, 0xb8, 0xe3, 0x12, 0x9c,
                     0x3f, 0x4a, 0xd8, 0xb5, 0x03, 0x10, 0x7a, 0x6f);

struct BleRxItem {
    uint16_t src_addr;
    uint16_t len;
    uint8_t  data[512];
};

struct PeerConn {
    uint16_t conn_handle;
    uint8_t  peer_addr[6];
    int8_t   rssi;
    bool     connected;
};

class BleTransport {
public:
    BleTransport() = default;

    void init();
    void deinit();

    int send(uint16_t peer_addr, const uint8_t *data, size_t len);

    using RxCallback = void (*)(uint16_t src_addr, const uint8_t *data, size_t len);
    void on_receive(RxCallback cb);

    void start_scan();
    void stop_scan();
    void start_advertise();

    QueueHandle_t get_rx_queue() const { return rx_queue_; }
    int8_t get_peer_rssi(uint16_t peer_addr) const;

    // NimBLE callback trampolines (must be public for C callbacks)
    static int on_gap_event(struct ble_gap_event *event, void *arg);
    static int on_gatt_rx_write(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg);

    uint16_t get_node_addr() const { return node_addr_; }

private:
    void register_gatt_services();
    uint16_t addr_from_ble(const uint8_t *ble_addr) const;

    PeerConn     peers_[BLE_MAX_CONNECTIONS] = {};
    QueueHandle_t rx_queue_ = nullptr;
    RxCallback    rx_cb_    = nullptr;
    uint16_t     tx_chr_val_handle_ = 0;
    uint16_t     node_addr_ = 0;
    bool         initialized_ = false;
};

} // namespace flp
