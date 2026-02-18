#include "ble_transport.hpp"
#include "packet.hpp"
#include "esp_log.h"
#include "esp_mac.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <cstring>

static const char *TAG = "ble_xport";

// Singleton pointer for C callback trampolines
static flp::BleTransport *s_instance = nullptr;

namespace flp {

// ── GATT service definition ──────────────────────────────────────────────

static const struct ble_gatt_svc_def kGattServices[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kFlpServiceUuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // TX characteristic — server notifies peers
                .uuid       = &kFlpTxCharUuid.u,
                .access_cb  = nullptr,
                .arg        = nullptr,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = nullptr, // patched in register_gatt_services
            },
            {
                // RX characteristic — peers write packets to us
                .uuid       = &kFlpRxCharUuid.u,
                .access_cb  = BleTransport::on_gatt_rx_write,
                .arg        = nullptr,
                .flags      = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 } // sentinel
        },
    },
    { 0 } // sentinel
};

// ── Helpers ──────────────────────────────────────────────────────────────

uint16_t BleTransport::addr_from_ble(const uint8_t *ble_addr) const
{
    // Use lower 16 bits of BLE address as mesh address
    return (uint16_t)(ble_addr[1] << 8 | ble_addr[0]);
}

// ── Init / Deinit ────────────────────────────────────────────────────────

void BleTransport::init()
{
    if (initialized_) {
        return;
    }

    s_instance = this;
    rx_queue_ = xQueueCreate(BLE_RX_QUEUE_DEPTH, sizeof(BleRxItem));

    // Derive node address from base MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    node_addr_ = (uint16_t)(mac[5] << 8 | mac[4]);

    // Init NimBLE
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    // Set device name: "FLP-XXXX"
    char name[16];
    snprintf(name, sizeof(name), "FLP-%04X", node_addr_);
    ble_svc_gap_device_name_set(name);

    register_gatt_services();

    // Set preferred MTU
    ble_att_set_preferred_mtu(MAX_MTU);

    // Start NimBLE host task
    nimble_port_freertos_init([](void *) {
        nimble_port_run();
        nimble_port_freertos_deinit();
    });

    initialized_ = true;
    ESP_LOGI(TAG, "BLE transport initialized, node_addr=0x%04X name=%s", node_addr_, name);
}

void BleTransport::deinit()
{
    if (!initialized_) {
        return;
    }

    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
    }

    if (rx_queue_) {
        vQueueDelete(rx_queue_);
        rx_queue_ = nullptr;
    }

    s_instance = nullptr;
    initialized_ = false;
    ESP_LOGI(TAG, "BLE transport deinitialized");
}

// ── GATT registration ────────────────────────────────────────────────────

void BleTransport::register_gatt_services()
{
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // We need to patch the val_handle pointer for the TX characteristic
    // Unfortunately the static array approach doesn't let us easily do this,
    // so we use a mutable copy technique: declare as non-const above then
    // patch here. Since kGattServices is file-static and mutable for the
    // characteristic array, we can cast:
    auto *tx_chr = const_cast<struct ble_gatt_chr_def *>(
        &kGattServices[0].characteristics[0]);
    tx_chr->val_handle = &tx_chr_val_handle_;

    int rc = ble_gatts_count_cfg(kGattServices);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return;
    }

    rc = ble_gatts_add_svcs(kGattServices);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return;
    }
}

// ── Callbacks ────────────────────────────────────────────────────────────

void BleTransport::on_receive(RxCallback cb)
{
    rx_cb_ = cb;
}

int BleTransport::on_gatt_rx_write(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (!s_instance || !s_instance->rx_queue_) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    struct os_mbuf *om = ctxt->om;
    BleRxItem item = {};
    item.len = (uint16_t)OS_MBUF_PKTLEN(om);
    if (item.len > sizeof(item.data)) {
        item.len = sizeof(item.data);
    }
    os_mbuf_copydata(om, 0, item.len, item.data);

    // Determine source address from connection
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) == 0) {
        item.src_addr = s_instance->addr_from_ble(desc.peer_id_addr.val);
    }

    // ISR-safe queue send (this callback runs in NimBLE context)
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_instance->rx_queue_, &item, &woken);
    portYIELD_FROM_ISR(woken);

    return 0;
}

int BleTransport::on_gap_event(struct ble_gap_event *event, void *arg)
{
    if (!s_instance) {
        return 0;
    }

    auto *self = s_instance;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            uint16_t ch = event->connect.conn_handle;
            struct ble_gap_conn_desc desc;
            ble_gap_conn_find(ch, &desc);

            // Store peer
            for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
                if (!self->peers_[i].connected) {
                    self->peers_[i].conn_handle = ch;
                    memcpy(self->peers_[i].peer_addr, desc.peer_id_addr.val, 6);
                    self->peers_[i].rssi = 0;
                    self->peers_[i].connected = true;
                    ESP_LOGI(TAG, "Peer connected, handle=%d slot=%d", ch, i);
                    break;
                }
            }

            // Exchange MTU
            ble_gattc_exchange_mtu(ch, nullptr, nullptr);
        } else {
            ESP_LOGW(TAG, "Connection failed, status=%d", event->connect.status);
        }

        // Resume advertising to accept more connections
        self->start_advertise();
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Peer disconnected, handle=%d reason=%d",
                 event->disconnect.conn.conn_handle,
                 event->disconnect.reason);
        for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
            if (self->peers_[i].conn_handle == event->disconnect.conn.conn_handle) {
                self->peers_[i].connected = false;
                break;
            }
        }
        // Resume advertising
        self->start_advertise();
        break;

    case BLE_GAP_EVENT_DISC: {
        // Scan result — check if it advertises FLP service UUID
        const struct ble_hs_adv_fields *fields = nullptr;
        struct ble_hs_adv_fields parsed;
        int rc = ble_hs_adv_parse_fields(&parsed, event->disc.data,
                                          event->disc.length_data);
        if (rc == 0) {
            fields = &parsed;
        }

        if (fields && fields->num_uuids128 > 0) {
            for (int i = 0; i < fields->num_uuids128; i++) {
                if (ble_uuid_cmp(&fields->uuids128[i].u, &kFlpServiceUuid.u) == 0) {
                    ESP_LOGI(TAG, "Found FLP peer, RSSI=%d", event->disc.rssi);

                    // Attempt connection
                    struct ble_gap_conn_params params = {};
                    params.scan_itvl = 0x0010;
                    params.scan_window = 0x0010;
                    params.itvl_min = 24;  // 30ms
                    params.itvl_max = 40;  // 50ms
                    params.latency = 0;
                    params.supervision_timeout = 256; // ~2.56s

                    self->stop_scan();
                    ble_gap_connect(BLE_OWN_ADDR_PUBLIC,
                                   &event->disc.addr,
                                   30000, // timeout 30s
                                   &params,
                                   BleTransport::on_gap_event,
                                   nullptr);
                    break;
                }
            }
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "Scan complete");
        break;

    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.status != 0) {
            ESP_LOGW(TAG, "Notify TX failed, status=%d", event->notify_tx.status);
        }
        break;

    default:
        break;
    }

    return 0;
}

// ── Scanning ─────────────────────────────────────────────────────────────

void BleTransport::start_scan()
{
    struct ble_gap_disc_params params = {};
    params.passive = 0;        // active scan
    params.itvl = 0;           // default
    params.window = 0;         // default
    params.filter_duplicates = 1;
    params.limited = 0;

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC,
                          BLE_HS_FOREVER,
                          &params,
                          BleTransport::on_gap_event,
                          nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "start_scan failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "Scanning started");
    }
}

void BleTransport::stop_scan()
{
    ble_gap_disc_cancel();
}

// ── Advertising ──────────────────────────────────────────────────────────

void BleTransport::start_advertise()
{
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = const_cast<ble_uuid128_t *>(&kFlpServiceUuid);
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; // connectable undirected
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; // general discoverable

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC,
                           nullptr, // any peer
                           BLE_HS_FOREVER,
                           &adv_params,
                           BleTransport::on_gap_event,
                           nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv_start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "Advertising started");
    }
}

// ── Send ─────────────────────────────────────────────────────────────────

int BleTransport::send(uint16_t peer_addr, const uint8_t *data, size_t len)
{
    // Find peer by address
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
        if (peers_[i].connected && addr_from_ble(peers_[i].peer_addr) == peer_addr) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
            if (!om) {
                ESP_LOGE(TAG, "Failed to allocate mbuf for send");
                return -1;
            }

            int rc = ble_gatts_notify_custom(peers_[i].conn_handle,
                                             tx_chr_val_handle_, om);
            if (rc != 0) {
                ESP_LOGE(TAG, "ble_gatts_notify_custom failed: %d", rc);
                return -1;
            }

            ESP_LOGD(TAG, "Sent %zu bytes to peer 0x%04X", len, peer_addr);
            return 0;
        }
    }

    ESP_LOGW(TAG, "Peer 0x%04X not found or not connected", peer_addr);
    return -1;
}

// ── Peer RSSI ────────────────────────────────────────────────────────────

int8_t BleTransport::get_peer_rssi(uint16_t peer_addr) const
{
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
        if (peers_[i].connected && addr_from_ble(peers_[i].peer_addr) == peer_addr) {
            // Read live RSSI from controller
            int8_t rssi = 0;
            int rc = ble_gap_conn_rssi(peers_[i].conn_handle, &rssi);
            if (rc == 0) {
                return rssi;
            }
            return peers_[i].rssi;
        }
    }
    return -127; // not found
}

} // namespace flp
