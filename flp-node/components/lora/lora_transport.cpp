#include "lora_transport.hpp"

#include <cstring>

#include "esp_log.h"

static const char *TAG = "lora_xport";

namespace flp
{

// ── SX1280 SPI helpers ──────────────────────────────────────────────────

void LoraTransport::wait_busy()
{
    // BUSY typically clears in <1ms; calibration after reset can take ~3.5ms
    for (int i = 0; i < 1000; i++)
    {
        if (gpio_get_level(busy_pin_) == 0)
        {
            return;
        }
        esp_rom_delay_us(10); // total max ~10ms
    }
    ESP_LOGW(TAG, "BUSY pin timeout");
}

void LoraTransport::write_command(uint8_t cmd,
                                  const uint8_t *params,
                                  size_t len)
{
    wait_busy();

    uint8_t tx[16];
    tx[0] = cmd;
    if (params && len > 0)
    {
        memcpy(&tx[1], params, len);
    }

    spi_transaction_t t = {};
    t.length = (1 + len) * 8;
    t.tx_buffer = tx;

    xSemaphoreTake(spi_mutex_, portMAX_DELAY);
    spi_device_transmit(spi_, &t);
    xSemaphoreGive(spi_mutex_);
}

void LoraTransport::read_command(uint8_t cmd, uint8_t *result, size_t len)
{
    wait_busy();

    // [CMD][NOP][result0]...[resultN-1]
    uint8_t tx[16] = {};
    uint8_t rx[16] = {};
    tx[0] = cmd;

    spi_transaction_t t = {};
    t.length = (2 + len) * 8; // cmd + NOP + result bytes
    t.tx_buffer = tx;
    t.rx_buffer = rx;

    xSemaphoreTake(spi_mutex_, portMAX_DELAY);
    spi_device_transmit(spi_, &t);
    xSemaphoreGive(spi_mutex_);

    memcpy(result, &rx[2], len);
}

void LoraTransport::write_buffer(uint8_t offset,
                                 const uint8_t *data,
                                 size_t len)
{
    wait_busy();

    uint8_t tx[258]; // cmd + offset + 256 max
    tx[0] = sx1280::CMD_WRITE_BUFFER;
    tx[1] = offset;
    memcpy(&tx[2], data, len);

    spi_transaction_t t = {};
    t.length = (2 + len) * 8;
    t.tx_buffer = tx;

    xSemaphoreTake(spi_mutex_, portMAX_DELAY);
    spi_device_transmit(spi_, &t);
    xSemaphoreGive(spi_mutex_);
}

void LoraTransport::read_buffer(uint8_t offset, uint8_t *data, size_t len)
{
    wait_busy();

    // [CMD][offset][NOP][data0]...[dataN-1]
    uint8_t tx[259] = {};
    uint8_t rx[259] = {};
    tx[0] = sx1280::CMD_READ_BUFFER;
    tx[1] = offset;

    spi_transaction_t t = {};
    t.length = (3 + len) * 8; // cmd + offset + NOP + data
    t.tx_buffer = tx;
    t.rx_buffer = rx;

    xSemaphoreTake(spi_mutex_, portMAX_DELAY);
    spi_device_transmit(spi_, &t);
    xSemaphoreGive(spi_mutex_);

    memcpy(data, &rx[3], len);
}

void LoraTransport::read_register(uint16_t addr, uint8_t *data, size_t len)
{
    wait_busy();

    // [CMD][addr15:8][addr7:0][NOP][data0]...[dataN-1]
    size_t total = 4 + len;
    uint8_t tx[16] = {};
    uint8_t rx[16] = {};
    tx[0] = sx1280::CMD_READ_REGISTER;
    tx[1] = (uint8_t) (addr >> 8);
    tx[2] = (uint8_t) (addr);

    spi_transaction_t t = {};
    t.length = total * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;

    xSemaphoreTake(spi_mutex_, portMAX_DELAY);
    spi_device_transmit(spi_, &t);
    xSemaphoreGive(spi_mutex_);

    memcpy(data, &rx[4], len);
}

// ── Chip control ────────────────────────────────────────────────────────

void LoraTransport::reset_chip()
{
    gpio_set_level(rst_pin_, 0);
    esp_rom_delay_us(50);
    gpio_set_level(rst_pin_, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    wait_busy();
}

void LoraTransport::set_frequency(uint32_t freq_hz)
{
    // RF_freq = freq_hz * 2^18 / 52 MHz (SX1280 XTAL = 52 MHz)
    uint32_t rf_freq =
        (uint32_t) ((double) freq_hz / 52000000.0 * (1 << 18));
    uint8_t params[3] = {
        (uint8_t) (rf_freq >> 16),
        (uint8_t) (rf_freq >> 8),
        (uint8_t) (rf_freq),
    };
    write_command(sx1280::CMD_SET_RF_FREQUENCY, params, 3);
}

void LoraTransport::set_tx_power(int8_t dbm)
{
    // SX1280 TX power: -18 to +13 dBm
    if (dbm < -18)
    {
        dbm = -18;
    }
    if (dbm > 13)
    {
        dbm = 13;
    }
    uint8_t params[2] = {
        (uint8_t) (dbm + 18),
        sx1280::TX_RAMP_20_US,
    };
    write_command(sx1280::CMD_SET_TX_PARAMS, params, 2);
}

void LoraTransport::set_modulation_params(uint8_t sf, uint8_t bw, uint8_t cr)
{
    uint8_t params[3] = {sf, bw, cr};
    write_command(sx1280::CMD_SET_MOD_PARAMS, params, 3);
}

void LoraTransport::set_packet_params(uint8_t payload_len)
{
    uint8_t params[5] = {
        sx1280::LORA_PREAMBLE_12,
        sx1280::LORA_HEADER_EXPLICIT,
        payload_len,
        sx1280::LORA_CRC_ON,
        sx1280::LORA_IQ_STD,
    };
    write_command(sx1280::CMD_SET_PACKET_PARAMS, params, 5);
}

void LoraTransport::enter_rx_continuous()
{
    // Max-length packet params for RX
    set_packet_params(LORA_MAX_PACKET);

    // Route TxDone + RxDone + CrcError to DIO1
    uint16_t irq_mask =
        sx1280::IRQ_TX_DONE | sx1280::IRQ_RX_DONE | sx1280::IRQ_CRC_ERROR;
    uint8_t params[8] = {
        (uint8_t) (irq_mask >> 8), (uint8_t) (irq_mask), // IRQ mask
        (uint8_t) (irq_mask >> 8), (uint8_t) (irq_mask), // DIO1 mask
        0x00, 0x00,                                       // DIO2 (unused)
        0x00, 0x00,                                       // DIO3 (unused)
    };
    write_command(sx1280::CMD_SET_DIO_IRQ_PARAMS, params, 8);

    // Clear pending IRQs
    uint8_t clr[2] = {0xFF, 0xFF};
    write_command(sx1280::CMD_CLR_IRQ_STATUS, clr, 2);

    // Continuous RX (periodBaseCount = 0xFFFF)
    uint8_t rx_params[3] = {0x00, 0xFF, 0xFF};
    write_command(sx1280::CMD_SET_RX, rx_params, 3);
}

// ── ISR and RX task ─────────────────────────────────────────────────────

void IRAM_ATTR LoraTransport::dio1_isr_handler(void *arg)
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
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Read 16-bit IRQ status
        uint8_t irq_raw[2];
        self->read_command(sx1280::CMD_GET_IRQ_STATUS, irq_raw, 2);
        uint16_t irq = (irq_raw[0] << 8) | irq_raw[1];

        // Clear the flags we just read
        uint8_t clr[2] = {irq_raw[0], irq_raw[1]};
        self->write_command(sx1280::CMD_CLR_IRQ_STATUS, clr, 2);

        if (irq & sx1280::IRQ_TX_DONE)
        {
            if (self->tx_done_sem_)
            {
                xSemaphoreGive(self->tx_done_sem_);
            }
            continue;
        }

        if (irq & sx1280::IRQ_RX_DONE)
        {
            if (irq & sx1280::IRQ_CRC_ERROR)
            {
                ESP_LOGW(TAG, "CRC error, dropping packet");
                continue;
            }

            // GetRxBufferStatus → {payloadLen, rxStartOffset}
            uint8_t rx_status[2];
            self->read_command(
                sx1280::CMD_GET_RX_BUFFER_STATUS, rx_status, 2);
            uint8_t pkt_len = rx_status[0];
            uint8_t rx_start = rx_status[1];

            BufferSlab *slab =
                self->buffer_pool_ ? self->buffer_pool_->acquire() : nullptr;
            if (!slab)
            {
                ESP_LOGW(TAG, "Buffer pool exhausted, dropping LoRa RX");
                continue;
            }

            slab->len = (pkt_len > MAX_MTU) ? MAX_MTU : pkt_len;
            slab->source = RxTransport::LORA;

            if (pkt_len > 0 && pkt_len <= LORA_MAX_PACKET)
            {
                self->read_buffer(rx_start, slab->data, slab->len);
            }

            // GetPacketStatus for LoRa → 5 bytes; [0]=rssiSync, [1]=snr
            uint8_t pkt_status[5];
            self->read_command(
                sx1280::CMD_GET_PACKET_STATUS, pkt_status, 5);
            int rssi = -(int) pkt_status[0] / 2;
            slab->rssi =
                static_cast<int8_t>(rssi < -128 ? -128 : rssi);

            ESP_LOGD(TAG, "RX %zu bytes, RSSI=%d", slab->len, slab->rssi);

            if (self->packet_queue_)
            {
                xQueueSend(self->packet_queue_, &slab, 0);
            }
            else
            {
                self->buffer_pool_->release(slab);
            }
        }
    }
}

// ── Init / Deinit ───────────────────────────────────────────────────────

void LoraTransport::init(uint8_t rx_task_priority)
{
    if (initialized_)
    {
        return;
    }

    spi_mutex_ = xSemaphoreCreateMutex();
    tx_done_sem_ = xSemaphoreCreateBinary();

    cs_pin_ = (gpio_num_t) CONFIG_FLP_LORA_CS;
    rst_pin_ = (gpio_num_t) CONFIG_FLP_LORA_RST;
    dio1_pin_ = (gpio_num_t) CONFIG_FLP_LORA_DIO0; // DIO1 on SX1280, same GPIO
    busy_pin_ = (gpio_num_t) CONFIG_FLP_LORA_BUSY;

    // Configure RST pin as output
    gpio_config_t rst_cfg = {};
    rst_cfg.pin_bit_mask = 1ULL << rst_pin_;
    rst_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&rst_cfg);
    gpio_set_level(rst_pin_, 1);

    // Configure BUSY pin as input
    gpio_config_t busy_cfg = {};
    busy_cfg.pin_bit_mask = 1ULL << busy_pin_;
    busy_cfg.mode = GPIO_MODE_INPUT;
    busy_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    busy_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&busy_cfg);

    // Configure SPI bus
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = CONFIG_FLP_LORA_MOSI;
    bus_cfg.miso_io_num = CONFIG_FLP_LORA_MISO;
    bus_cfg.sclk_io_num = CONFIG_FLP_LORA_SCK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 260;

    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return;
    }

    // Add SX1280 device (SPI mode 0, 8 MHz)
    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = 8 * 1000 * 1000;
    dev_cfg.mode = 0;
    dev_cfg.spics_io_num = cs_pin_;
    dev_cfg.queue_size = 1;

    ret = spi_bus_add_device(SPI3_HOST, &dev_cfg, &spi_);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI add device failed: %s", esp_err_to_name(ret));
        return;
    }

    // Reset and wait for chip ready
    reset_chip();

    // Enter standby
    uint8_t stdby = sx1280::STDBY_RC;
    write_command(sx1280::CMD_SET_STANDBY, &stdby, 1);

    // Verify chip: read firmware version at register 0x0153
    uint8_t fw[2];
    read_register(0x0153, fw, 2);
    if (fw[0] == 0x00 || fw[0] == 0xFF)
    {
        ESP_LOGE(TAG, "SX1280 not detected (reg 0x0153 = 0x%02X%02X)",
                 fw[0], fw[1]);
        return;
    }
    ESP_LOGI(TAG, "SX1280 detected (FW: 0x%02X%02X)", fw[0], fw[1]);

    // Set packet type: LoRa
    uint8_t pkt_type = sx1280::PACKET_TYPE_LORA;
    write_command(sx1280::CMD_SET_PACKET_TYPE, &pkt_type, 1);

    // Frequency: 2450 MHz (2.4 GHz ISM band)
    set_frequency(2450000000);

    // Modulation: SF7, BW 800 kHz, CR 4/5
    set_modulation_params(
        sx1280::LORA_SF7, sx1280::LORA_BW_800, sx1280::LORA_CR_4_5);

    // TX power: 13 dBm (max for SX1280)
    set_tx_power(13);

    // Buffer base addresses (half-duplex, share full 256-byte buffer)
    uint8_t buf_base[2] = {0x00, 0x00};
    write_command(sx1280::CMD_SET_BUFFER_BASE, buf_base, 2);

    // Create RX task before enabling interrupts
    xTaskCreate(
        rx_task_func, "lora_rx", 4096, this, rx_task_priority, &rx_task_);

    // Configure DIO1 interrupt (posedge)
    gpio_config_t dio1_cfg = {};
    dio1_cfg.pin_bit_mask = 1ULL << dio1_pin_;
    dio1_cfg.mode = GPIO_MODE_INPUT;
    dio1_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    dio1_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    dio1_cfg.intr_type = GPIO_INTR_POSEDGE;
    gpio_config(&dio1_cfg);

    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s",
                 esp_err_to_name(isr_ret));
        return;
    }
    gpio_isr_handler_add(dio1_pin_, dio1_isr_handler, this);

    // Enter continuous RX mode
    enter_rx_continuous();

    initialized_ = true;
    ESP_LOGI(TAG, "LoRa transport initialized (SX1280, 2450MHz, SF7, BW800kHz)");
}

void LoraTransport::deinit()
{
    if (!initialized_)
    {
        return;
    }

    // Put radio to sleep
    uint8_t sleep_cfg = 0x00;
    write_command(sx1280::CMD_SET_SLEEP, &sleep_cfg, 1);

    // Remove ISR and delete RX task
    gpio_isr_handler_remove(dio1_pin_);
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

    if (tx_done_sem_)
    {
        vSemaphoreDelete(tx_done_sem_);
        tx_done_sem_ = nullptr;
    }
    if (spi_mutex_)
    {
        vSemaphoreDelete(spi_mutex_);
        spi_mutex_ = nullptr;
    }

    initialized_ = false;
    ESP_LOGI(TAG, "LoRa transport deinitialized");
}

// ── Configure ───────────────────────────────────────────────────────────

void LoraTransport::configure(uint32_t freq_hz, uint8_t sf, uint32_t bw_hz)
{
    uint8_t stdby = sx1280::STDBY_RC;
    write_command(sx1280::CMD_SET_STANDBY, &stdby, 1);

    set_frequency(freq_hz);

    // Map SF number to SX1280 register value
    uint8_t sf_val;
    if (sf <= 5)
    {
        sf_val = sx1280::LORA_SF5;
    }
    else if (sf <= 6)
    {
        sf_val = sx1280::LORA_SF6;
    }
    else if (sf <= 7)
    {
        sf_val = sx1280::LORA_SF7;
    }
    else if (sf <= 8)
    {
        sf_val = sx1280::LORA_SF8;
    }
    else if (sf <= 9)
    {
        sf_val = sx1280::LORA_SF9;
    }
    else if (sf <= 10)
    {
        sf_val = sx1280::LORA_SF10;
    }
    else if (sf <= 11)
    {
        sf_val = sx1280::LORA_SF11;
    }
    else
    {
        sf_val = sx1280::LORA_SF12;
    }

    // Map BW in Hz to SX1280 register value (2.4 GHz bandwidths)
    uint8_t bw_val;
    if (bw_hz <= 200000)
    {
        bw_val = sx1280::LORA_BW_200;
    }
    else if (bw_hz <= 400000)
    {
        bw_val = sx1280::LORA_BW_400;
    }
    else if (bw_hz <= 800000)
    {
        bw_val = sx1280::LORA_BW_800;
    }
    else
    {
        bw_val = sx1280::LORA_BW_1600;
    }

    set_modulation_params(sf_val, bw_val, sx1280::LORA_CR_4_5);

    enter_rx_continuous();

    ESP_LOGI(TAG, "Configured: freq=%luHz SF=%u BW=%luHz", freq_hz, sf, bw_hz);
}

// ── Send ────────────────────────────────────────────────────────────────

int LoraTransport::send_raw(const uint8_t *data, size_t len)
{
    if (len > LORA_MAX_PACKET)
    {
        ESP_LOGE(TAG, "Payload too large: %zu > %zu", len, LORA_MAX_PACKET);
        return -1;
    }

    // Switch to standby
    uint8_t stdby = sx1280::STDBY_RC;
    write_command(sx1280::CMD_SET_STANDBY, &stdby, 1);

    // Set packet params with actual payload length
    set_packet_params((uint8_t) len);

    // Write payload to buffer at offset 0
    write_buffer(0x00, data, len);

    // Clear IRQ flags
    uint8_t clr[2] = {0xFF, 0xFF};
    write_command(sx1280::CMD_CLR_IRQ_STATUS, clr, 2);

    // Route TxDone to DIO1
    uint16_t irq_mask = sx1280::IRQ_TX_DONE;
    uint8_t irq_params[8] = {
        (uint8_t) (irq_mask >> 8), (uint8_t) (irq_mask),
        (uint8_t) (irq_mask >> 8), (uint8_t) (irq_mask),
        0x00, 0x00,
        0x00, 0x00,
    };
    write_command(sx1280::CMD_SET_DIO_IRQ_PARAMS, irq_params, 8);

    // Drain stale semaphore
    xSemaphoreTake(tx_done_sem_, 0);

    // Enter TX (periodBase=1ms, count=5000 → 5s hardware timeout)
    uint8_t tx_params[3] = {0x02, 0x13, 0x88};
    write_command(sx1280::CMD_SET_TX, tx_params, 3);

    // Wait for TxDone via ISR → semaphore
    BaseType_t got = xSemaphoreTake(tx_done_sem_, pdMS_TO_TICKS(5000));

    if (got != pdTRUE)
    {
        ESP_LOGE(TAG, "TX timeout");
        write_command(sx1280::CMD_SET_STANDBY, &stdby, 1);
        enter_rx_continuous();
        return -1;
    }

    // Return to RX
    enter_rx_continuous();

    ESP_LOGD(TAG, "Sent %zu bytes", len);
    return 0;
}

} // namespace flp
