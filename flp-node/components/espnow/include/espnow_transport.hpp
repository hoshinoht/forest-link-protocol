#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "buffer_pool.hpp"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
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

    /* TX semaphore diagnostics: total slots currently available across both
     * pools (ctrl + data).  0 = pipeline fully saturated. */
    uint8_t get_tx_slots_available() const
    {
        uint8_t ctrl = tx_ctrl_slots_
                           ? static_cast<uint8_t>(uxSemaphoreGetCount(tx_ctrl_slots_))
                           : TX_CTRL_DEPTH;
        uint8_t data = tx_data_slots_
                           ? static_cast<uint8_t>(uxSemaphoreGetCount(tx_data_slots_))
                           : TX_DATA_DEPTH;
        return static_cast<uint8_t>(ctrl + data);
    }

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

    /* Dual-pool TX flow control: ctrl (ACK/NACK) and data (DATA) are
     * separate so fragment bursts can't starve ACK delivery.
     * tx_slot_class_ ring (internal SRAM) tells on_send() which pool to
     * return each slot to.  SPSC ring: write in send(), read in on_send(). */
    static constexpr uint8_t TX_CTRL_DEPTH = 8;
    static constexpr uint8_t TX_DATA_DEPTH = 20;
    static constexpr uint8_t TX_SLOT_DEPTH = TX_CTRL_DEPTH + TX_DATA_DEPTH;

    SemaphoreHandle_t tx_ctrl_slots_ = nullptr;
    SemaphoreHandle_t tx_data_slots_ = nullptr;

    bool    tx_slot_class_[TX_SLOT_DEPTH] = {}; /* true=ctrl, false=data */
    uint8_t tx_ring_write_ = 0;
    uint8_t tx_ring_read_  = 0;

    PeerInfo peers_[ESPNOW_MAX_PEERS] = {};
    uint8_t peer_count_ = 0;
    QueueHandle_t packet_queue_ = nullptr;
    BufferPool *buffer_pool_ = nullptr;
    uint16_t node_addr_ = 0;
    bool initialized_ = false;
    QueueHandle_t hi_pri_queue_ = nullptr;
    QueueHandle_t lo_pri_queue_ = nullptr;
    QueueHandle_t pending_peer_queue_ = nullptr;
    std::atomic<uint32_t> tx_send_submit_count_ = 0;
    std::atomic<uint32_t> tx_send_complete_count_ = 0;
    std::atomic<uint32_t> tx_send_fail_status_count_ = 0;
    std::atomic<uint32_t> tx_semaphore_full_count_ = 0;
    std::atomic<uint32_t> tx_send_error_count_ = 0;
    std::atomic<uint16_t> tx_last_full_peer_ = 0;
    uint32_t last_tx_diag_ms_ = 0;

    void add_peer_if_new(const uint8_t *mac, int8_t rssi);
    uint16_t addr_from_mac(const uint8_t *mac) const;
    bool find_mac(uint16_t addr, uint8_t *mac_out) const;
    void maybe_log_tx_diag(const char *reason, uint16_t peer_addr);

    static void
    on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len);
    static void on_send(const esp_now_send_info_t *info,
                        esp_now_send_status_t status);
};

} /* namespace flp */
