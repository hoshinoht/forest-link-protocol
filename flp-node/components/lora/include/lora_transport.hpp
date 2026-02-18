#pragma once

#include <cstdint>
#include <cstddef>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

namespace flp {

static constexpr int LORA_RX_QUEUE_DEPTH = 16;
static constexpr size_t LORA_MAX_PACKET = 255;

// SX1276 register addresses
namespace sx1276 {
    static constexpr uint8_t REG_FIFO            = 0x00;
    static constexpr uint8_t REG_OP_MODE         = 0x01;
    static constexpr uint8_t REG_FRF_MSB         = 0x06;
    static constexpr uint8_t REG_FRF_MID         = 0x07;
    static constexpr uint8_t REG_FRF_LSB         = 0x08;
    static constexpr uint8_t REG_PA_CONFIG       = 0x09;
    static constexpr uint8_t REG_FIFO_ADDR_PTR   = 0x0D;
    static constexpr uint8_t REG_FIFO_TX_BASE    = 0x0E;
    static constexpr uint8_t REG_FIFO_RX_BASE    = 0x0F;
    static constexpr uint8_t REG_FIFO_RX_CURRENT = 0x10;
    static constexpr uint8_t REG_IRQ_FLAGS       = 0x12;
    static constexpr uint8_t REG_RX_NB_BYTES     = 0x13;
    static constexpr uint8_t REG_PKT_RSSI_VALUE  = 0x1A;
    static constexpr uint8_t REG_MODEM_CONFIG1   = 0x1D;
    static constexpr uint8_t REG_MODEM_CONFIG2   = 0x1E;
    static constexpr uint8_t REG_PAYLOAD_LENGTH  = 0x22;
    static constexpr uint8_t REG_DIO_MAPPING1    = 0x40;
    static constexpr uint8_t REG_VERSION         = 0x42;

    // OpMode bits
    static constexpr uint8_t MODE_SLEEP      = 0x80; // LoRa + Sleep
    static constexpr uint8_t MODE_STANDBY    = 0x81; // LoRa + Standby
    static constexpr uint8_t MODE_TX         = 0x83; // LoRa + TX
    static constexpr uint8_t MODE_RX_CONT    = 0x85; // LoRa + Continuous RX

    // IRQ flags
    static constexpr uint8_t IRQ_TX_DONE     = 0x08;
    static constexpr uint8_t IRQ_RX_DONE     = 0x40;
    static constexpr uint8_t IRQ_ALL         = 0xFF;
}

struct LoraRxItem {
    uint16_t len;
    int      rssi;
    uint8_t  data[LORA_MAX_PACKET];
};

class LoraTransport {
public:
    LoraTransport() = default;

    void init(uint8_t rx_task_priority = 5);
    void deinit();

    int send(const uint8_t *data, size_t len);

    using RxCallback = void (*)(const uint8_t *data, size_t len, int rssi);
    void on_receive(RxCallback cb);

    void configure(uint32_t freq_hz, uint8_t sf, uint32_t bw_hz);

    QueueHandle_t get_rx_queue() const { return rx_queue_; }

private:
    uint8_t read_reg(uint8_t addr);
    void    write_reg(uint8_t addr, uint8_t val);
    void    write_fifo(const uint8_t *data, size_t len);
    void    read_fifo(uint8_t *data, size_t len);
    void    reset_chip();
    void    set_frequency(uint32_t freq_hz);
    void    set_tx_power(int8_t dbm);
    void    enter_rx_continuous();

    static void IRAM_ATTR dio0_isr_handler(void *arg);
    static void rx_task_func(void *arg);

    spi_device_handle_t spi_        = nullptr;
    gpio_num_t          cs_pin_     = GPIO_NUM_NC;
    gpio_num_t          rst_pin_    = GPIO_NUM_NC;
    gpio_num_t          dio0_pin_   = GPIO_NUM_NC;
    QueueHandle_t       rx_queue_   = nullptr;
    SemaphoreHandle_t   spi_mutex_  = nullptr;
    TaskHandle_t        rx_task_    = nullptr;
    RxCallback          rx_cb_      = nullptr;
    bool                initialized_ = false;
};

} // namespace flp
