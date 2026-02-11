#include "ble_transport.hpp"
#include "esp_log.h"

static const char *TAG = "ble_xport";

namespace flp {

void BleTransport::init()
{
    ESP_LOGI(TAG, "BLE transport initialized");
    // TODO: Initialize NimBLE host, register GATT services
}

void BleTransport::deinit()
{
    ESP_LOGI(TAG, "BLE transport deinitialized");
    // TODO: Cleanup NimBLE resources
}

int BleTransport::send(uint16_t peer_addr, const uint8_t *data, size_t len)
{
    // TODO: Send via BLE GATT notification/indication
    ESP_LOGD(TAG, "send to 0x%04x, %zu bytes", peer_addr, len);
    return 0;
}

void BleTransport::start_scan()
{
    // TODO: Start NimBLE scanning for mesh peers
}

void BleTransport::stop_scan()
{
    // TODO: Stop scanning
}

void BleTransport::start_advertise()
{
    // TODO: Start advertising mesh service UUID
}

} // namespace flp
