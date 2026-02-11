#include "lora_transport.hpp"
#include "esp_log.h"

static const char *TAG = "lora_xport";

namespace flp {

void LoraTransport::init()
{
    ESP_LOGI(TAG, "LoRa transport initialized");
    // TODO: Configure SPI bus, reset SX127x, set default params
}

void LoraTransport::deinit()
{
    ESP_LOGI(TAG, "LoRa transport deinitialized");
    // TODO: Put SX127x to sleep, free SPI
}

int LoraTransport::send(const uint8_t *data, size_t len)
{
    // TODO: Write to SX127x FIFO and trigger TX
    ESP_LOGD(TAG, "send %zu bytes", len);
    return 0;
}

void LoraTransport::configure(uint32_t freq_hz, uint8_t sf, uint32_t bw_hz)
{
    // TODO: Write SX127x registers for frequency, SF, BW
    ESP_LOGI(TAG, "configure freq=%lu SF=%u BW=%lu", freq_hz, sf, bw_hz);
}

} // namespace flp
