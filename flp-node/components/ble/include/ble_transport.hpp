#pragma once

#include <cstddef>
#include <cstdint>

#include "buffer_pool.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/ble_hs.h"
#include "itransport.hpp"

namespace flp
{

static constexpr int BLE_MAX_CONNECTIONS = 4;
static constexpr int BLE_RX_QUEUE_DEPTH = 16;

// FLP custom 128-bit service UUID (little-endian)
// Full: 6f7a1001-b5d8-4a3f-9c12-e3b8f4c0d1a2
static const ble_uuid128_t kFlpServiceUuid = BLE_UUID128_INIT(0xa2,
                                                              0xd1,
                                                              0xc0,
                                                              0xf4,
                                                              0xb8,
                                                              0xe3,
                                                              0x12,
                                                              0x9c,
                                                              0x3f,
                                                              0x4a,
                                                              0xd8,
                                                              0xb5,
                                                              0x01,
                                                              0x10,
                                                              0x7a,
                                                              0x6f);

// TX characteristic UUID (notify)
// 6f7a1002-b5d8-4a3f-9c12-e3b8f4c0d1a2
static const ble_uuid128_t kFlpTxCharUuid = BLE_UUID128_INIT(0xa2,
                                                             0xd1,
                                                             0xc0,
                                                             0xf4,
                                                             0xb8,
                                                             0xe3,
                                                             0x12,
                                                             0x9c,
                                                             0x3f,
                                                             0x4a,
                                                             0xd8,
                                                             0xb5,
                                                             0x02,
                                                             0x10,
                                                             0x7a,
                                                             0x6f);

// RX characteristic UUID (write-no-response)
// 6f7a1003-b5d8-4a3f-9c12-e3b8f4c0d1a2
static const ble_uuid128_t kFlpRxCharUuid = BLE_UUID128_INIT(0xa2,
                                                             0xd1,
                                                             0xc0,
                                                             0xf4,
                                                             0xb8,
                                                             0xe3,
                                                             0x12,
                                                             0x9c,
                                                             0x3f,
                                                             0x4a,
                                                             0xd8,
                                                             0xb5,
                                                             0x03,
                                                             0x10,
                                                             0x7a,
                                                             0x6f);

struct PeerConn
{
    uint16_t conn_handle;
    uint8_t peer_addr[6];
    int8_t rssi;
    bool connected;
};

class BleTransport : public ITransport
{
  public:
    BleTransport() = default;

    void set_packet_queue(QueueHandle_t q)
    {
        packet_queue_ = q;
    }

    void set_buffer_pool(BufferPool *p) { buffer_pool_ = p; }

    void init() override;
    void deinit() override;

    int send(uint16_t peer_addr, const uint8_t *data, size_t len) override;

    void start_scan();
    void stop_scan();
    void start_advertise();

    int8_t get_peer_rssi(uint16_t peer_addr) const;

    // NimBLE callback trampolines (must be public for C callbacks)
    static int on_gap_event(struct ble_gap_event *event, void *arg);
    static int on_gatt_tx_access(uint16_t conn_handle,
                                 uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt,
                                 void *arg);
    static int on_gatt_rx_write(uint16_t conn_handle,
                                uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt,
                                void *arg);

    uint16_t get_node_addr() const
    {
        return node_addr_;
    }

  private:
    void register_gatt_services();
    uint16_t addr_from_ble(const uint8_t *ble_addr) const;

    PeerConn peers_[BLE_MAX_CONNECTIONS] = {};
    QueueHandle_t packet_queue_ = nullptr; // shared MeshManager queue
    BufferPool *buffer_pool_ = nullptr;
    uint16_t tx_chr_val_handle_ = 0;
    uint16_t node_addr_ = 0;
    bool initialized_ = false;
};

} // namespace flp
