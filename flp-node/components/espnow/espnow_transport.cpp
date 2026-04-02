#include "espnow_transport.hpp"

#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "packet.hpp"

static const char *TAG = "espnow_xport";
static const uint8_t ESPNOW_BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Singleton pointer for C callback trampolines */
static flp::EspNowTransport *s_instance = nullptr;

namespace flp
{

void EspNowTransport::maybe_log_tx_diag(const char *reason, uint16_t peer_addr)
{
    uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (last_tx_diag_ms_ != 0 && (now - last_tx_diag_ms_) < 1000)
    {
        return;
    }
    last_tx_diag_ms_ = now;

    uint32_t submit = tx_send_submit_count_.exchange(0, std::memory_order_relaxed);
    uint32_t done = tx_send_complete_count_.exchange(0, std::memory_order_relaxed);
    uint32_t fail_status =
        tx_send_fail_status_count_.exchange(0, std::memory_order_relaxed);
    uint32_t sem_full =
        tx_semaphore_full_count_.exchange(0, std::memory_order_relaxed);
    uint32_t send_err = tx_send_error_count_.exchange(0, std::memory_order_relaxed);
    uint16_t last_full_peer = tx_last_full_peer_.load(std::memory_order_relaxed);

    if (submit == 0 && done == 0 && fail_status == 0 && sem_full == 0 &&
        send_err == 0)
    {
        return;
    }

    ESP_LOGI(TAG,
             "TX diag(%s): slots=%u submit=%lu done=%lu done_fail=%lu sem_full=%lu send_err=%lu last_full=0x%04X peer=0x%04X",
             reason,
             get_tx_slots_available(),
             static_cast<unsigned long>(submit),
             static_cast<unsigned long>(done),
             static_cast<unsigned long>(fail_status),
             static_cast<unsigned long>(sem_full),
             static_cast<unsigned long>(send_err),
             last_full_peer,
             peer_addr);
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

uint16_t EspNowTransport::addr_from_mac(const uint8_t *mac) const
{
    /*
     * Use lower 16 bits of MAC (bytes 4-5) as mesh address — matches
     * MeshManager's derivation from esp_efuse_mac_get_default().
     */
    return static_cast<uint16_t>((mac[4] << 8) | mac[5]);
}

bool EspNowTransport::find_mac(uint16_t addr, uint8_t *mac_out) const
{
    for (uint8_t i = 0; i < ESPNOW_MAX_PEERS; i++)
    {
        if (peers_[i].active && peers_[i].addr == addr)
        {
            memcpy(mac_out, peers_[i].mac, 6);
            return true;
        }
    }
    return false;
}

void EspNowTransport::add_peer_if_new(const uint8_t *mac, int8_t rssi)
{
    uint16_t addr = addr_from_mac(mac);

    /* Update existing peer RSSI */
    for (uint8_t i = 0; i < ESPNOW_MAX_PEERS; i++)
    {
        if (peers_[i].active && peers_[i].addr == addr)
        {
            peers_[i].rssi = rssi;
            return;
        }
    }

    /* Find empty slot */
    for (uint8_t i = 0; i < ESPNOW_MAX_PEERS; i++)
    {
        if (!peers_[i].active)
        {
            memcpy(peers_[i].mac, mac, 6);
            peers_[i].addr = addr;
            peers_[i].rssi = rssi;
            peers_[i].active = true;
            peer_count_++;

            /* Register with ESP-NOW (required before unicast send) */
            esp_now_peer_info_t peer_info = {};
            memcpy(peer_info.peer_addr, mac, 6);
            peer_info.channel = 0; /* use current channel */
            peer_info.ifidx = WIFI_IF_STA;
            peer_info.encrypt = false;
            esp_err_t err = esp_now_add_peer(&peer_info);
            if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST)
            {
                ESP_LOGW(
                    TAG, "esp_now_add_peer failed: %s", esp_err_to_name(err));
            }

            ESP_LOGI(TAG,
                     "New peer 0x%04X (MAC %02X:%02X:%02X:%02X:%02X:%02X)",
                     addr,
                     mac[0],
                     mac[1],
                     mac[2],
                     mac[3],
                     mac[4],
                     mac[5]);
            return;
        }
    }

    ESP_LOGW(TAG,
             "Peer table full (%d), ignoring new peer 0x%04X",
             ESPNOW_MAX_PEERS,
             addr);
}

/* ── Init / Deinit ──────────────────────────────────────────────────────── */

void EspNowTransport::init()
{
    if (initialized_)
    {
        return;
    }

    s_instance = this;

    /* Create deferred peer registration queue before registering callbacks */
    pending_peer_queue_ = xQueueCreate(ESPNOW_PENDING_PEER_QUEUE_DEPTH,
                                       sizeof(PendingPeer));
    if (!pending_peer_queue_)
    {
        ESP_LOGE(TAG, "Failed to create pending peer queue");
        return;
    }

    /* TX flow control semaphore: sized to ESP-NOW's internal TX queue depth.
     * send() takes a slot, on_send() callback returns it.  This makes
     * NO_MEM structurally impossible and self-adjusts to actual radio
     * throughput — no budget constants needed in upper layers. */
    tx_slots_ = xSemaphoreCreateCounting(TX_SLOT_DEPTH, TX_SLOT_DEPTH);
    if (!tx_slots_)
    {
        ESP_LOGE(TAG, "Failed to create TX semaphore");
        return;
    }

    /* Derive node address from base MAC */
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    node_addr_ = static_cast<uint16_t>((mac[4] << 8) | mac[5]);

    /* WiFi must already be started (by main.cpp) before calling esp_now_init() */
    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(tx_slots_);
        tx_slots_ = nullptr;
        vQueueDelete(pending_peer_queue_);
        pending_peer_queue_ = nullptr;
        return;
    }

    /* Register callbacks */
    esp_now_register_recv_cb(EspNowTransport::on_recv);
    esp_now_register_send_cb(EspNowTransport::on_send);

    /* Add broadcast peer (required for ESP-NOW broadcast send) */
    esp_now_peer_info_t bcast_peer = {};
    memcpy(bcast_peer.peer_addr, ESPNOW_BCAST_MAC, 6);
    bcast_peer.channel = 0;
    bcast_peer.ifidx = WIFI_IF_STA;
    bcast_peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&bcast_peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST)
    {
        ESP_LOGW(TAG, "Failed to add broadcast peer: %s", esp_err_to_name(err));
    }

    initialized_ = true;
    ESP_LOGI(
        TAG, "ESP-NOW transport initialized, node_addr=0x%04X", node_addr_);
}

void EspNowTransport::update_broadcast_peer()
{
    if (!initialized_)
    {
        return;
    }

    /* Remove existing broadcast peer (if any) */
    esp_now_del_peer(ESPNOW_BCAST_MAC); /* Ignore errors */

    /* Re-add with current WiFi channel */
    esp_now_peer_info_t bcast_peer = {};
    memcpy(bcast_peer.peer_addr, ESPNOW_BCAST_MAC, 6);
    bcast_peer.channel = 0; /* Use current channel */
    bcast_peer.ifidx = WIFI_IF_STA;
    bcast_peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&bcast_peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST)
    {
        ESP_LOGW(
            TAG, "Failed to re-add broadcast peer: %s", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "Broadcast peer updated");
    }
}

void EspNowTransport::drain_pending_peers()
{
    if (!pending_peer_queue_)
    {
        return;
    }
    PendingPeer pending;
    while (xQueueReceive(pending_peer_queue_, &pending, 0) == pdTRUE)
    {
        add_peer_if_new(pending.mac, pending.rssi);
    }
}

void EspNowTransport::deinit()
{
    if (!initialized_)
    {
        return;
    }

    esp_now_deinit();
    if (pending_peer_queue_)
    {
        vQueueDelete(pending_peer_queue_);
        pending_peer_queue_ = nullptr;
    }
    if (tx_slots_)
    {
        vSemaphoreDelete(tx_slots_);
        tx_slots_ = nullptr;
    }
    s_instance = nullptr;
    initialized_ = false;
    ESP_LOGI(TAG, "ESP-NOW transport deinitialized");
}

/* ── Callbacks ──────────────────────────────────────────────────────────── */

void EspNowTransport::on_recv(const esp_now_recv_info_t *info,
                              const uint8_t *data,
                              int len)
{
    if (!s_instance || !s_instance->packet_queue_ || !s_instance->buffer_pool_)
    {
        return;
    }

    if (len <= 0 || static_cast<size_t>(len) > MAX_MTU)
    {
        ESP_LOGW(TAG, "ESP-NOW RX invalid len=%d", len);
        return;
    }

    /*
     * Do NOT call add_peer_if_new() here — on_recv runs in the WiFi driver
     * task which already holds ESP-NOW internal locks. Calling esp_now_add_peer
     * from this context would attempt to re-acquire the same lock → deadlock.
     *
     * Instead, enqueue the source MAC into pending_peer_queue_ so that
     * drain_pending_peers() (called from the mesh task) can register the peer
     * safely outside the WiFi driver task context.
     */
    int8_t rssi = (info->rx_ctrl) ? info->rx_ctrl->rssi : -90;
    if (s_instance->pending_peer_queue_)
    {
        PendingPeer pending = {};
        memcpy(pending.mac, info->src_addr, 6);
        pending.rssi = rssi;
        /* Non-blocking: if the queue is full the peer will be re-registered
         * the next time a packet from it is received. */
        xQueueSendFromISR(s_instance->pending_peer_queue_, &pending, nullptr);
    }

    BufferSlab *slab = s_instance->buffer_pool_->acquire();
    if (!slab)
    {
        ESP_LOGW(TAG, "Buffer pool exhausted, dropping ESP-NOW RX packet");
        return;
    }

    slab->len = static_cast<size_t>(len);
    memcpy(slab->data, data, slab->len);
    slab->source = RxTransport::ESPNOW;
    slab->rssi = rssi;

    /* Step 8: Route to priority queue based on packet type */
    if (s_instance->hi_pri_queue_ && s_instance->lo_pri_queue_ &&
        slab->len >= 1)
    {
        PacketType ptype = static_cast<PacketType>(slab->data[0] & 0x3F);
        bool is_high_pri =
            (ptype == PacketType::ACK || ptype == PacketType::NACK ||
             ptype == PacketType::DISCOVERY ||
             ptype == PacketType::ROUTE_ERROR ||
             ptype == PacketType::TRANSFER_ACK ||
             ptype == PacketType::ROUTE_REPLY);
        QueueHandle_t target =
            is_high_pri ? s_instance->hi_pri_queue_ : s_instance->lo_pri_queue_;
        if (xQueueSend(target, &slab, 0) != pdTRUE)
        {
            s_instance->buffer_pool_->release(slab);
        }
    }
    else
    {
        xQueueSend(s_instance->packet_queue_, &slab, 0);
    }
}

void EspNowTransport::on_send(const esp_now_send_info_t *info,
                              esp_now_send_status_t status)
{
    /* Return the TX slot to the semaphore — fires from the WiFi task
     * after the frame has been transmitted (success or failure). */
    if (s_instance && s_instance->tx_slots_)
    {
        xSemaphoreGive(s_instance->tx_slots_);
    }

    if (s_instance)
    {
        s_instance->tx_send_complete_count_.fetch_add(
            1, std::memory_order_relaxed);
        if (status != ESP_NOW_SEND_SUCCESS)
        {
            s_instance->tx_send_fail_status_count_.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    if (status != ESP_NOW_SEND_SUCCESS)
    {
        const uint8_t *mac = info->des_addr;
        ESP_LOGD(TAG,
                 "ESP-NOW send failed to %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0],
                 mac[1],
                 mac[2],
                 mac[3],
                 mac[4],
                 mac[5]);
    }
}

/* ── Send ───────────────────────────────────────────────────────────────── */

int EspNowTransport::send(uint16_t peer_addr, const uint8_t *data, size_t len)
{
    if (len > ESP_NOW_MAX_DATA_LEN_V2)
    {
        ESP_LOGW(TAG,
                 "Payload %zu exceeds ESP-NOW max (%d), dropping",
                 len,
                 ESP_NOW_MAX_DATA_LEN_V2);
        return -1;
    }

    /* Acquire a TX slot from the counting semaphore.
     * Brief blocking wait (5ms): gives on_send() callback time to
     * return a slot when the pipeline is saturated, instead of
     * failing instantly.  5ms is ~2 frame TX times at 1Mbps. */
    if (tx_slots_ && xSemaphoreTake(tx_slots_, pdMS_TO_TICKS(5)) != pdTRUE)
    {
        tx_semaphore_full_count_.fetch_add(1, std::memory_order_relaxed);
        tx_last_full_peer_.store(peer_addr, std::memory_order_relaxed);
        ESP_LOGW(TAG, "TX semaphore full (%u slots), deferring send to 0x%04X",
                 TX_SLOT_DEPTH, peer_addr);
        maybe_log_tx_diag("sem_full", peer_addr);
        return -1;
    }

    /* Broadcast */
    if (peer_addr == 0xFFFF)
    {
        if (!esp_now_is_peer_exist(ESPNOW_BCAST_MAC))
        {
            update_broadcast_peer();
        }

        esp_err_t err = esp_now_send(ESPNOW_BCAST_MAC, data, len);
        if (err != ESP_OK)
        {
            tx_send_error_count_.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGE(
                TAG, "ESP-NOW broadcast send failed: %s", esp_err_to_name(err));
            /* Give back the slot — esp_now_send failed, on_send won't fire */
            if (tx_slots_) { xSemaphoreGive(tx_slots_); }
            maybe_log_tx_diag("bcast_err", peer_addr);
            return -1;
        }
        tx_send_submit_count_.fetch_add(1, std::memory_order_relaxed);
        maybe_log_tx_diag("bcast_ok", peer_addr);
        ESP_LOGD(TAG, "Broadcast %zu bytes", len);
        return 0;
    }

    /* Unicast — look up MAC */
    uint8_t mac[6];
    if (!find_mac(peer_addr, mac))
    {
        tx_send_error_count_.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGW(TAG, "Peer 0x%04X not found in table", peer_addr);
        if (tx_slots_) { xSemaphoreGive(tx_slots_); }
        maybe_log_tx_diag("peer_miss", peer_addr);
        return -1;
    }

    esp_err_t err = esp_now_send(mac, data, len);
    if (err != ESP_OK)
    {
        tx_send_error_count_.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGE(TAG,
                 "ESP-NOW send to 0x%04X failed: %s",
                 peer_addr,
                 esp_err_to_name(err));
        /* Give back — esp_now_send failed, on_send won't fire */
        if (tx_slots_) { xSemaphoreGive(tx_slots_); }
        maybe_log_tx_diag("send_err", peer_addr);
        return -1;
    }

    tx_send_submit_count_.fetch_add(1, std::memory_order_relaxed);
    maybe_log_tx_diag("send_ok", peer_addr);
    ESP_LOGD(TAG, "Sent %zu bytes to peer 0x%04X", len, peer_addr);
    return 0;
}

/* ── Peer RSSI ──────────────────────────────────────────────────────────── */

int8_t EspNowTransport::get_peer_rssi(uint16_t peer_addr) const
{
    for (uint8_t i = 0; i < ESPNOW_MAX_PEERS; i++)
    {
        if (peers_[i].active && peers_[i].addr == peer_addr)
        {
            return peers_[i].rssi;
        }
    }
    return -127; /* not found */
}

} /* namespace flp */
