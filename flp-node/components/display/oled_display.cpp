#include "oled_display.hpp"

#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "oled";

namespace flp
{

void OledDisplay::init(int sda_pin, int scl_pin, int rst_pin)
{
    ESP_LOGI(TAG, "Initializing SSD1306 (SDA=%d SCL=%d RST=%d)...",
             sda_pin, scl_pin, rst_pin);

    // Hardware reset (high-low-high pulse)
    if (rst_pin >= 0)
    {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << rst_pin;
        cfg.mode = GPIO_MODE_OUTPUT;
        gpio_config(&cfg);

        gpio_set_level(static_cast<gpio_num_t>(rst_pin), 1);
        vTaskDelay(pdMS_TO_TICKS(1));
        gpio_set_level(static_cast<gpio_num_t>(rst_pin), 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(static_cast<gpio_num_t>(rst_pin), 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // I2C bus
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(sda_pin);
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(scl_pin);
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return;
    }

    // Add SSD1306 device (I2C addr 0x3C)
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = 0x3C;
    dev_cfg.scl_speed_hz = 400000;

    i2c_master_dev_handle_t i2c_dev = nullptr;
    ret = i2c_master_bus_add_device(bus, &dev_cfg, &i2c_dev);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(ret));
        return;
    }

    // Wire handles into the library's device struct
    dev_._address = 0x3C;
    dev_._flip = false;
    dev_._i2c_num = I2C_NUM_0;
    dev_._i2c_bus_handle = bus;
    dev_._i2c_dev_handle = i2c_dev;

    // SSD1306 init sequence + clear
    ssd1306_init(&dev_, 128, 64);
    ssd1306_clear_screen(&dev_, false);

    initialized_ = true;
    ESP_LOGI(TAG, "SSD1306 128x64 OLED initialized (SDA=%d SCL=%d RST=%d)",
             sda_pin, scl_pin, rst_pin);
}

void OledDisplay::clear()
{
    ssd1306_clear_screen(&dev_, false);
}

void OledDisplay::draw_string(int x, int page, const char *str)
{
    // x is ignored — library always starts at segment 0.
    // All current callers pass x=0.
    (void)x;
    if (page < 0 || page >= 8)
    {
        return;
    }
    int len = strlen(str);
    if (len > 16) len = 16;
    ssd1306_display_text(&dev_, page, str, len, false);
}

void OledDisplay::draw_hline(int x, int y, int width)
{
    _ssd1306_line(&dev_, x, y, x + width - 1, y, false);
}

void OledDisplay::flush()
{
    ssd1306_show_buffer(&dev_);
}

// ── Status rendering ────────────────────────────────────────────────────

void OledDisplay::update(const NodeStatus &s)
{
    if (!initialized_)
    {
        return;
    }

    ssd1306_clear_screen(&dev_, false);
    char line[17]; // 16 chars + null (8x8 font, 128px / 8 = 16 chars)

    // Line 0: node ID + version
    snprintf(line, sizeof(line), "FLP-%04X v%s", s.node_addr, "0.1.0");
    ssd1306_display_text(&dev_, 0, line, strlen(line), false);

    // Line 1: separator
    _ssd1306_line(&dev_, 0, 9, 127, 9, false);

    // Line 2: connectivity
    snprintf(line, sizeof(line), "W:%s N:%u L:OK",
             s.wifi_connected ? "OK" : "--", s.espnow_peers);
    ssd1306_display_text(&dev_, 2, line, strlen(line), false);

    // Line 3: mesh status
    if (s.hops_to_internet == 0xFF)
    {
        snprintf(line, sizeof(line), "Nbrs:%u GW:--", s.neighbor_count);
    }
    else
    {
        snprintf(line, sizeof(line), "Nbrs:%u GW:%uh",
                 s.neighbor_count, s.hops_to_internet);
    }
    ssd1306_display_text(&dev_, 3, line, strlen(line), false);

    // Line 4: separator
    _ssd1306_line(&dev_, 0, 33, 127, 33, false);

    // Line 5: transfer
    if (s.transfer_active && s.filename)
    {
        snprintf(line, sizeof(line), "%.10s %u%%", s.filename, s.transfer_pct);
    }
    else
    {
        snprintf(line, sizeof(line), "No transfer");
    }
    ssd1306_display_text(&dev_, 5, line, strlen(line), false);

    // Line 6: heap
    snprintf(line, sizeof(line), "Heap:%lukB", (unsigned long)s.free_heap_kb);
    ssd1306_display_text(&dev_, 6, line, strlen(line), false);

    // Line 7: uptime
    uint32_t h = s.uptime_s / 3600;
    uint32_t m = (s.uptime_s % 3600) / 60;
    uint32_t sec = s.uptime_s % 60;
    snprintf(line, sizeof(line), "Up %lu:%02lu:%02lu",
             (unsigned long)h, (unsigned long)m, (unsigned long)sec);
    ssd1306_display_text(&dev_, 7, line, strlen(line), false);

    // Flush line-drawing buffer (separators on pages 1 and 4)
    ssd1306_show_buffer(&dev_);
}

} // namespace flp
