#include "lora_transport.hpp"

#include <cstring>

#include "esp_log.h"

static const char *TAG = "lora_xport";

namespace flp
{

// ── SPI register helpers ─────────────────────────────────────────────────

uint8_t LoraTransport::read_reg(uint8_t addr)
{
    uint8_t tx[2] = {(uint8_t) (addr & 0x7F), 0x00};
    uint8_t rx[2] = {};

    spi_transaction_t t = {};
    t.length = 16;
    t.tx_buffer = tx;
    t.rx_buffer = rx;

    xSemaphoreTake(spi_mutex_, portMAX_DELAY);
    spi_device_transmit(spi_, &t);
    xSemaphoreGive(spi_mutex_);

    return rx[1];
}

void LoraTransport::write_reg(uint8_t addr, uint8_t val)
{
    uint8_t tx[2] = {(uint8_t) (addr | 0x80), val};

    spi_transaction_t t = {};
    t.length = 16;
    t.tx_buffer = tx;

    xSemaphoreTake(spi_mutex_, portMAX_DELAY);
    spi_device_transmit(spi_, &t);
    xSemaphoreGive(spi_mutex_);
}

void LoraTransport::write_fifo(const uint8_t *data, size_t len)
{
    uint8_t tx[256];
    tx[0] = sx1276::REG_FIFO | 0x80;
    memcpy(&tx[1], data, len);

    spi_transaction_t t = {};
    t.length = (len + 1) * 8;
    t.tx_buffer = tx;

    xSemaphoreTake(spi_mutex_, portMAX_DELAY);
    spi_device_transmit(spi_, &t);
    xSemaphoreGive(spi_mutex_);
}

void LoraTransport::read_fifo(uint8_t *data, size_t len)
{
    uint8_t tx[256] = {};
    uint8_t rx[256] = {};
    tx[0] = sx1276::REG_FIFO & 0x7F;

    spi_transaction_t t = {};
    t.length = (len + 1) * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;

    xSemaphoreTake(spi_mutex_, portMAX_DELAY);
    spi_device_transmit(spi_, &t);
    xSemaphoreGive(spi_mutex_);

    memcpy(data, &rx[1], len);
}

// ── Chip control ─────────────────────────────────────────────────────────

void LoraTransport::reset_chip()
{
    gpio_set_level(rst_pin_, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(rst_pin_, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
}

void LoraTransport::set_frequency(uint32_t freq_hz)
{
    uint32_t frf = (uint32_t) ((double) freq_hz / 61.035);
    write_reg(sx1276::REG_FRF_MSB, (uint8_t) (frf >> 16));
    write_reg(sx1276::REG_FRF_MID, (uint8_t) (frf >> 8));
    write_reg(sx1276::REG_FRF_LSB, (uint8_t) (frf));
}

void LoraTransport::set_tx_power(int8_t dbm)
{
    // PA_BOOST pin, max 17 dBm
    if (dbm < 2)
    {
        dbm = 2;
    }
    if (dbm > 17)
    {
        dbm = 17;
    }
    write_reg(sx1276::REG_PA_CONFIG, (uint8_t) (0x80 | (dbm - 2)));
}

void LoraTransport::enter_rx_continuous()
{
    // Clear all IRQ flags
    write_reg(sx1276::REG_IRQ_FLAGS, sx1276::IRQ_ALL);

    // Set FIFO RX base address
    write_reg(sx1276::REG_FIFO_RX_BASE, 0x00);
    write_reg(sx1276::REG_FIFO_ADDR_PTR, 0x00);

    // Map DIO0 to RxDone (bits 7:6 = 00)
    write_reg(sx1276::REG_DIO_MAPPING1, 0x00);

    // Enter continuous RX mode
    write_reg(sx1276::REG_OP_MODE, sx1276::MODE_RX_CONT);
}

// ── ISR and RX task ──────────────────────────────────────────────────────

void IRAM_ATTR LoraTransport::dio0_isr_handler(void *arg)
{
    auto *self = static_cast<LoraTransport *>(arg);
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(self->rx_task_, &woken);
    portYIELD_FROM_ISR(woken);
}

void LoraTransport::rx_task_func(void *arg)
{
    auto *self = static_cast<LoraTransport *>(arg);

    while (true)
    {
        // Wait for DIO0 interrupt notification
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint8_t irq_flags = self->read_reg(sx1276::REG_IRQ_FLAGS);

        if (irq_flags & sx1276::IRQ_RX_DONE)
        {
            // Check for CRC error (bit 5)
            if (irq_flags & 0x20)
            {
                ESP_LOGW(TAG, "CRC error, dropping packet");
                self->write_reg(sx1276::REG_IRQ_FLAGS, sx1276::IRQ_ALL);
                continue;
            }

            LoraRxItem item = {};
            item.len = self->read_reg(sx1276::REG_RX_NB_BYTES);

            // Set FIFO address to current RX address
            uint8_t rx_addr = self->read_reg(sx1276::REG_FIFO_RX_CURRENT);
            self->write_reg(sx1276::REG_FIFO_ADDR_PTR, rx_addr);

            // Read payload from FIFO
            if (item.len > 0 && item.len <= LORA_MAX_PACKET)
            {
                self->read_fifo(item.data, item.len);
            }

            // Read packet RSSI: -157 + reg value (for HF port, > 862 MHz)
            uint8_t rssi_raw = self->read_reg(sx1276::REG_PKT_RSSI_VALUE);
            item.rssi = -157 + rssi_raw;

            ESP_LOGD(TAG, "RX %u bytes, RSSI=%d", item.len, item.rssi);

            // Post to queue
            xQueueSend(self->rx_queue_, &item, 0);
        }

        // Clear all IRQ flags
        self->write_reg(sx1276::REG_IRQ_FLAGS, sx1276::IRQ_ALL);
    }
}

// ── Init / Deinit ────────────────────────────────────────────────────────

void LoraTransport::init(uint8_t rx_task_priority)
{
    if (initialized_)
    {
        return;
    }

    spi_mutex_ = xSemaphoreCreateMutex();
    rx_queue_ = xQueueCreate(LORA_RX_QUEUE_DEPTH, sizeof(LoraRxItem));

    // Pin config
    cs_pin_ = (gpio_num_t) CONFIG_FLP_LORA_CS;
    rst_pin_ = (gpio_num_t) CONFIG_FLP_LORA_RST;
    dio0_pin_ = (gpio_num_t) CONFIG_FLP_LORA_DIO0;

    // Configure RST pin as output
    gpio_config_t rst_cfg = {};
    rst_cfg.pin_bit_mask = 1ULL << rst_pin_;
    rst_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&rst_cfg);
    gpio_set_level(rst_pin_, 1);

    // Configure SPI bus
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = CONFIG_FLP_LORA_MOSI;
    bus_cfg.miso_io_num = CONFIG_FLP_LORA_MISO;
    bus_cfg.sclk_io_num = CONFIG_FLP_LORA_SCK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 256;

    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return;
    }

    // Add SX1276 device
    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = 8 * 1000 * 1000; // 8 MHz
    dev_cfg.mode = 0;
    dev_cfg.spics_io_num = cs_pin_;
    dev_cfg.queue_size = 1;

    ret = spi_bus_add_device(SPI3_HOST, &dev_cfg, &spi_);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI add device failed: %s", esp_err_to_name(ret));
        return;
    }

    // Reset the chip
    reset_chip();

    // Verify chip version
    uint8_t version = read_reg(sx1276::REG_VERSION);
    if (version != 0x12)
    {
        ESP_LOGE(TAG,
                 "SX1276 not detected (version=0x%02X, expected 0x12)",
                 version);
        return;
    }
    ESP_LOGI(TAG, "SX1276 detected, version=0x%02X", version);

    // Enter sleep + LoRa mode
    write_reg(sx1276::REG_OP_MODE, sx1276::MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Default configuration: 915 MHz, SF7, 125 kHz BW
    set_frequency(915000000);

    // Modem config: BW=125kHz(0x70), CR=4/5(0x02), implicit header off(0x00)
    write_reg(sx1276::REG_MODEM_CONFIG1, 0x72);
    // SF7(0x70), CRC on(0x04)
    write_reg(sx1276::REG_MODEM_CONFIG2, 0x74);

    // TX power: 17 dBm
    set_tx_power(17);

    // Set FIFO base addresses
    write_reg(sx1276::REG_FIFO_TX_BASE, 0x80);
    write_reg(sx1276::REG_FIFO_RX_BASE, 0x00);

    // Create RX task before enabling interrupts
    xTaskCreate(
        rx_task_func, "lora_rx", 4096, this, rx_task_priority, &rx_task_);

    // Configure DIO0 interrupt for RxDone
    gpio_config_t dio0_cfg = {};
    dio0_cfg.pin_bit_mask = 1ULL << dio0_pin_;
    dio0_cfg.mode = GPIO_MODE_INPUT;
    dio0_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    dio0_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    dio0_cfg.intr_type = GPIO_INTR_POSEDGE;
    gpio_config(&dio0_cfg);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(dio0_pin_, dio0_isr_handler, this);

    // Enter continuous RX mode
    enter_rx_continuous();

    initialized_ = true;
    ESP_LOGI(TAG, "LoRa transport initialized (915MHz, SF7, BW125kHz)");
}

void LoraTransport::deinit()
{
    if (!initialized_)
    {
        return;
    }

    // Put radio to sleep
    write_reg(sx1276::REG_OP_MODE, sx1276::MODE_SLEEP);

    // Remove ISR and delete RX task
    gpio_isr_handler_remove(dio0_pin_);
    if (rx_task_)
    {
        vTaskDelete(rx_task_);
        rx_task_ = nullptr;
    }

    // Free SPI
    if (spi_)
    {
        spi_bus_remove_device(spi_);
        spi_ = nullptr;
    }
    spi_bus_free(SPI3_HOST);

    if (rx_queue_)
    {
        vQueueDelete(rx_queue_);
        rx_queue_ = nullptr;
    }
    if (spi_mutex_)
    {
        vSemaphoreDelete(spi_mutex_);
        spi_mutex_ = nullptr;
    }

    initialized_ = false;
    ESP_LOGI(TAG, "LoRa transport deinitialized");
}

// ── Configure ────────────────────────────────────────────────────────────

void LoraTransport::configure(uint32_t freq_hz, uint8_t sf, uint32_t bw_hz)
{
    // Go to standby for configuration
    write_reg(sx1276::REG_OP_MODE, sx1276::MODE_STANDBY);
    vTaskDelay(pdMS_TO_TICKS(1));

    set_frequency(freq_hz);

    // Map BW in Hz to register value
    uint8_t bw_bits;
    if (bw_hz <= 7800)
    {
        bw_bits = 0x00;
    }
    else if (bw_hz <= 10400)
    {
        bw_bits = 0x10;
    }
    else if (bw_hz <= 15600)
    {
        bw_bits = 0x20;
    }
    else if (bw_hz <= 20800)
    {
        bw_bits = 0x30;
    }
    else if (bw_hz <= 31250)
    {
        bw_bits = 0x40;
    }
    else if (bw_hz <= 41700)
    {
        bw_bits = 0x50;
    }
    else if (bw_hz <= 62500)
    {
        bw_bits = 0x60;
    }
    else if (bw_hz <= 125000)
    {
        bw_bits = 0x70;
    }
    else if (bw_hz <= 250000)
    {
        bw_bits = 0x80;
    }
    else
    {
        bw_bits = 0x90; // 500 kHz
    }

    // ModemConfig1: BW | CodingRate 4/5 (0x02) | implicit header off
    write_reg(sx1276::REG_MODEM_CONFIG1, bw_bits | 0x02);

    // ModemConfig2: SF | CRC on (0x04)
    uint8_t sf_bits = (sf & 0x0F) << 4;
    write_reg(sx1276::REG_MODEM_CONFIG2, sf_bits | 0x04);

    // Return to RX continuous
    enter_rx_continuous();

    ESP_LOGI(TAG, "Configured: freq=%luHz SF=%u BW=%luHz", freq_hz, sf, bw_hz);
}

// ── Receive callback ─────────────────────────────────────────────────────

void LoraTransport::on_receive(RxCallback cb)
{
    rx_cb_ = cb;
}

// ── Send ─────────────────────────────────────────────────────────────────

int LoraTransport::send(const uint8_t *data, size_t len)
{
    if (len > LORA_MAX_PACKET)
    {
        ESP_LOGE(TAG, "Payload too large: %zu > %zu", len, LORA_MAX_PACKET);
        return -1;
    }

    // Switch to standby
    write_reg(sx1276::REG_OP_MODE, sx1276::MODE_STANDBY);
    vTaskDelay(pdMS_TO_TICKS(1));

    // Clear IRQ flags
    write_reg(sx1276::REG_IRQ_FLAGS, sx1276::IRQ_ALL);

    // Set FIFO TX base and pointer
    write_reg(sx1276::REG_FIFO_TX_BASE, 0x80);
    write_reg(sx1276::REG_FIFO_ADDR_PTR, 0x80);

    // Write payload to FIFO
    write_fifo(data, len);

    // Set payload length
    write_reg(sx1276::REG_PAYLOAD_LENGTH, (uint8_t) len);

    // Map DIO0 to TxDone (bits 7:6 = 01)
    write_reg(sx1276::REG_DIO_MAPPING1, 0x40);

    // Enter TX mode
    write_reg(sx1276::REG_OP_MODE, sx1276::MODE_TX);

    // Wait for TxDone (poll with timeout)
    int timeout_ms = 5000;
    while (timeout_ms > 0)
    {
        uint8_t flags = read_reg(sx1276::REG_IRQ_FLAGS);
        if (flags & sx1276::IRQ_TX_DONE)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        timeout_ms -= 10;
    }

    if (timeout_ms <= 0)
    {
        ESP_LOGE(TAG, "TX timeout");
        write_reg(sx1276::REG_OP_MODE, sx1276::MODE_STANDBY);
        enter_rx_continuous();
        return -1;
    }

    // Clear IRQ and return to RX
    write_reg(sx1276::REG_IRQ_FLAGS, sx1276::IRQ_ALL);
    enter_rx_continuous();

    ESP_LOGD(TAG, "Sent %zu bytes", len);
    return 0;
}

} // namespace flp
