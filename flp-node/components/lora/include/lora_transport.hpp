#pragma once

#include <cstddef>
#include <cstdint>

#include "buffer_pool.hpp"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "itransport.hpp"
#include "packet.hpp"

namespace flp
{

static constexpr size_t LORA_MAX_PACKET = 255;

// SX1280 command opcodes and constants
namespace sx1280
{
// Command opcodes
static constexpr uint8_t CMD_SET_SLEEP = 0x84;
static constexpr uint8_t CMD_SET_STANDBY = 0x80;
static constexpr uint8_t CMD_SET_TX = 0x83;
static constexpr uint8_t CMD_SET_RX = 0x82;
static constexpr uint8_t CMD_SET_RF_FREQUENCY = 0x86;
static constexpr uint8_t CMD_SET_PACKET_TYPE = 0x8A;
static constexpr uint8_t CMD_SET_MOD_PARAMS = 0x8B;
static constexpr uint8_t CMD_SET_PACKET_PARAMS = 0x8C;
static constexpr uint8_t CMD_SET_TX_PARAMS = 0x8E;
static constexpr uint8_t CMD_SET_BUFFER_BASE = 0x8F;
static constexpr uint8_t CMD_SET_DIO_IRQ_PARAMS = 0x8D;
static constexpr uint8_t CMD_GET_IRQ_STATUS = 0x15;
static constexpr uint8_t CMD_CLR_IRQ_STATUS = 0x97;
static constexpr uint8_t CMD_GET_RX_BUFFER_STATUS = 0x17;
static constexpr uint8_t CMD_GET_PACKET_STATUS = 0x1D;
static constexpr uint8_t CMD_WRITE_BUFFER = 0x1A;
static constexpr uint8_t CMD_READ_BUFFER = 0x1B;
static constexpr uint8_t CMD_GET_STATUS = 0xC0;
static constexpr uint8_t CMD_READ_REGISTER = 0x19;

// Standby configs
static constexpr uint8_t STDBY_RC = 0x00;

// Packet types
static constexpr uint8_t PACKET_TYPE_LORA = 0x01;

// LoRa spreading factors (register encoding)
static constexpr uint8_t LORA_SF5 = 0x50;
static constexpr uint8_t LORA_SF6 = 0x60;
static constexpr uint8_t LORA_SF7 = 0x70;
static constexpr uint8_t LORA_SF8 = 0x80;
static constexpr uint8_t LORA_SF9 = 0x90;
static constexpr uint8_t LORA_SF10 = 0xA0;
static constexpr uint8_t LORA_SF11 = 0xB0;
static constexpr uint8_t LORA_SF12 = 0xC0;

// LoRa bandwidths
static constexpr uint8_t LORA_BW_200 = 0x34;
static constexpr uint8_t LORA_BW_400 = 0x26;
static constexpr uint8_t LORA_BW_800 = 0x18;
static constexpr uint8_t LORA_BW_1600 = 0x0A;

// LoRa coding rates
static constexpr uint8_t LORA_CR_4_5 = 0x01;
static constexpr uint8_t LORA_CR_4_6 = 0x02;
static constexpr uint8_t LORA_CR_4_7 = 0x03;
static constexpr uint8_t LORA_CR_4_8 = 0x04;

// LoRa packet params
static constexpr uint8_t LORA_PREAMBLE_12 = 0x20;
static constexpr uint8_t LORA_HEADER_EXPLICIT = 0x00;
static constexpr uint8_t LORA_CRC_ON = 0x20;
static constexpr uint8_t LORA_IQ_STD = 0x40;

// IRQ masks (16-bit)
static constexpr uint16_t IRQ_TX_DONE = 0x0001;
static constexpr uint16_t IRQ_RX_DONE = 0x0002;
static constexpr uint16_t IRQ_CRC_ERROR = 0x0040;
static constexpr uint16_t IRQ_ALL = 0xFFFF;

// TX ramp time
static constexpr uint8_t TX_RAMP_20_US = 0xE0;
} // namespace sx1280

class LoraTransport : public ITransport
{
  public:
    LoraTransport() = default;

    void set_packet_queue(QueueHandle_t q)
    {
        packet_queue_ = q;
    }

    void set_buffer_pool(BufferPool *p) { buffer_pool_ = p; }

    // ITransport interface
    void init() override { init(5); }
    void deinit() override;
    int send(uint16_t peer_addr, const uint8_t *data, size_t len) override
    {
        (void) peer_addr;
        return send_raw(data, len);
    }

    // LoRa-specific
    void init(uint8_t rx_task_priority);
    int send_raw(const uint8_t *data, size_t len);

    void configure(uint32_t freq_hz, uint8_t sf, uint32_t bw_hz);

  private:
    // SX1280 command-based SPI helpers
    void write_command(uint8_t cmd, const uint8_t *params, size_t len);
    void read_command(uint8_t cmd, uint8_t *result, size_t len);
    void write_buffer(uint8_t offset, const uint8_t *data, size_t len);
    void read_buffer(uint8_t offset, uint8_t *data, size_t len);
    void read_register(uint16_t addr, uint8_t *data, size_t len);
    void wait_busy();

    void reset_chip();
    void set_frequency(uint32_t freq_hz);
    void set_tx_power(int8_t dbm);
    void set_modulation_params(uint8_t sf, uint8_t bw, uint8_t cr);
    void set_packet_params(uint8_t payload_len);
    void enter_rx_continuous();

    static void IRAM_ATTR dio1_isr_handler(void *arg);
    static void rx_task_func(void *arg);

    spi_device_handle_t spi_ = nullptr;
    gpio_num_t cs_pin_ = GPIO_NUM_NC;
    gpio_num_t rst_pin_ = GPIO_NUM_NC;
    gpio_num_t dio1_pin_ = GPIO_NUM_NC;
    gpio_num_t busy_pin_ = GPIO_NUM_NC;
    QueueHandle_t packet_queue_ = nullptr; // shared MeshManager queue
    BufferPool *buffer_pool_ = nullptr;
    SemaphoreHandle_t spi_mutex_ = nullptr;
    TaskHandle_t rx_task_ = nullptr;
    SemaphoreHandle_t tx_done_sem_ = nullptr;
    bool initialized_ = false;
};

} // namespace flp
