#pragma once

#include <cstddef>
#include <cstdint>

#include "buffer_pool.hpp"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "itransport.hpp"

namespace flp
{

static constexpr int ESPNOW_MAX_PEERS = 20;
/* Depth of the deferred peer registration queue (one slot per concurrent
 * new contact before the mesh task drains it). */
static constexpr int ESPNOW_PENDING_PEER_QUEUE_DEPTH = 8;

class EspNowTransport : public ITransport
{
  public:
    EspNowTransport() = default;

    void set_packet_queue(QueueHandle_t q)
    {
        packet_queue_ = q;
    }
    void set_buffer_pool(BufferPool *p)
    {
        buffer_pool_ = p;
    }

    /* Step 8: Dual-priority queue support */
    void set_hi_pri_queue(QueueHandle_t q) { hi_pri_queue_ = q; }
    void set_lo_pri_queue(QueueHandle_t q) { lo_pri_queue_ = q; }

    void init() override;
    void deinit() override;

    int send(uint16_t peer_addr, const uint8_t *data, size_t len) override;

    void update_broadcast_peer();

    /*
     * Drain pending peer registrations — MUST be called from the mesh task
     * (not from within the ESP-NOW receive callback) to avoid acquiring
     * ESP-NOW internal locks from the WiFi driver task context.
     */
    void drain_pending_peers();

    uint16_t get_node_addr() const
    {
        return node_addr_;
    }

    uint8_t get_peer_count() const
    {
        return peer_count_;
    }

    int8_t get_peer_rssi(uint16_t peer_addr) const;

  private:
    struct PeerInfo
    {
        uint8_t mac[6];
        uint16_t addr;
        int8_t rssi;
        bool active;
    };

    /* Item queued from on_recv (WiFi task) for deferred registration */
    struct PendingPeer
    {
        uint8_t mac[6];
        int8_t rssi;
    };

    PeerInfo peers_[ESPNOW_MAX_PEERS] = {};
    uint8_t peer_count_ = 0;
    QueueHandle_t packet_queue_ = nullptr;
    BufferPool *buffer_pool_ = nullptr;
    uint16_t node_addr_ = 0;
    bool initialized_ = false;
    QueueHandle_t hi_pri_queue_ = nullptr;
    QueueHandle_t lo_pri_queue_ = nullptr;
    QueueHandle_t pending_peer_queue_ = nullptr;

    void add_peer_if_new(const uint8_t *mac, int8_t rssi);
    uint16_t addr_from_mac(const uint8_t *mac) const;
    bool find_mac(uint16_t addr, uint8_t *mac_out) const;

    static void
    on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len);
    static void on_send(const esp_now_send_info_t *info,
                        esp_now_send_status_t status);
};

} /* namespace flp */
