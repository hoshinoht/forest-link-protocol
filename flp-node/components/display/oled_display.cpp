#include "oled_display.hpp"

#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "oled";

// ── 5x7 font, ASCII 32-126 (column-major, 5 bytes per glyph) ───────────

// clang-format off
static const uint8_t kFont5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 32 ' '
    {0x00,0x00,0x5F,0x00,0x00}, // 33 '!'
    {0x00,0x07,0x00,0x07,0x00}, // 34 '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // 35 '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // 36 '$'
    {0x23,0x13,0x08,0x64,0x62}, // 37 '%'
    {0x36,0x49,0x55,0x22,0x50}, // 38 '&'
    {0x00,0x05,0x03,0x00,0x00}, // 39 '''
    {0x00,0x1C,0x22,0x41,0x00}, // 40 '('
    {0x00,0x41,0x22,0x1C,0x00}, // 41 ')'
    {0x14,0x08,0x3E,0x08,0x14}, // 42 '*'
    {0x08,0x08,0x3E,0x08,0x08}, // 43 '+'
    {0x00,0x50,0x30,0x00,0x00}, // 44 ','
    {0x08,0x08,0x08,0x08,0x08}, // 45 '-'
    {0x00,0x60,0x60,0x00,0x00}, // 46 '.'
    {0x20,0x10,0x08,0x04,0x02}, // 47 '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // 48 '0'
    {0x00,0x42,0x7F,0x40,0x00}, // 49 '1'
    {0x42,0x61,0x51,0x49,0x46}, // 50 '2'
    {0x21,0x41,0x45,0x4B,0x31}, // 51 '3'
    {0x18,0x14,0x12,0x7F,0x10}, // 52 '4'
    {0x27,0x45,0x45,0x45,0x39}, // 53 '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // 54 '6'
    {0x01,0x71,0x09,0x05,0x03}, // 55 '7'
    {0x36,0x49,0x49,0x49,0x36}, // 56 '8'
    {0x06,0x49,0x49,0x29,0x1E}, // 57 '9'
    {0x00,0x36,0x36,0x00,0x00}, // 58 ':'
    {0x00,0x56,0x36,0x00,0x00}, // 59 ';'
    {0x08,0x14,0x22,0x41,0x00}, // 60 '<'
    {0x14,0x14,0x14,0x14,0x14}, // 61 '='
    {0x00,0x41,0x22,0x14,0x08}, // 62 '>'
    {0x02,0x01,0x51,0x09,0x06}, // 63 '?'
    {0x32,0x49,0x79,0x41,0x3E}, // 64 '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 65 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 66 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 67 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 68 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 69 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 70 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 71 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 72 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 73 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 74 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 75 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 76 'L'
    {0x7F,0x02,0x0C,0x02,0x7F}, // 77 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 78 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 79 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 80 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 81 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 82 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 83 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 84 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 85 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 86 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 87 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 88 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 89 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 90 'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // 91 '['
    {0x02,0x04,0x08,0x10,0x20}, // 92 '\'
    {0x00,0x41,0x41,0x7F,0x00}, // 93 ']'
    {0x04,0x02,0x01,0x02,0x04}, // 94 '^'
    {0x40,0x40,0x40,0x40,0x40}, // 95 '_'
    {0x00,0x01,0x02,0x04,0x00}, // 96 '`'
    {0x20,0x54,0x54,0x54,0x78}, // 97 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 98 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 99 'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 100 'd'
    {0x38,0x54,0x54,0x54,0x18}, // 101 'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 102 'f'
    {0x0C,0x52,0x52,0x52,0x3E}, // 103 'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 104 'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 105 'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 106 'j'
    {0x7F,0x10,0x28,0x44,0x00}, // 107 'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 108 'l'
    {0x7C,0x04,0x18,0x04,0x78}, // 109 'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 110 'n'
    {0x38,0x44,0x44,0x44,0x38}, // 111 'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 112 'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 113 'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 114 'r'
    {0x48,0x54,0x54,0x54,0x20}, // 115 's'
    {0x04,0x3F,0x44,0x40,0x20}, // 116 't'
    {0x3C,0x40,0x40,0x20,0x7C}, // 117 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 118 'v'
    {0x3C,0x40,0x30,0x40,0x3C}, // 119 'w'
    {0x44,0x28,0x10,0x28,0x44}, // 120 'x'
    {0x0C,0x50,0x50,0x50,0x3C}, // 121 'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 122 'z'
    {0x00,0x08,0x36,0x41,0x00}, // 123 '{'
    {0x00,0x00,0x7F,0x00,0x00}, // 124 '|'
    {0x00,0x41,0x36,0x08,0x00}, // 125 '}'
    {0x10,0x08,0x08,0x10,0x08}, // 126 '~'
};
// clang-format on

namespace flp
{

// ── Hardware reset ──────────────────────────────────────────────────────

void OledDisplay::reset_hw()
{
    if (rst_pin_ < 0)
    {
        return;
    }

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << rst_pin_;
    cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&cfg);

    gpio_set_level(static_cast<gpio_num_t>(rst_pin_), 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(static_cast<gpio_num_t>(rst_pin_), 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(static_cast<gpio_num_t>(rst_pin_), 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

// ── I2C helpers ─────────────────────────────────────────────────────────

void OledDisplay::send_cmd(uint8_t cmd)
{
    uint8_t buf[2] = {0x00, cmd}; // Co=0, D/C#=0 → command
    i2c_master_transmit(dev_, buf, 2, 100);
}

// ── Init ────────────────────────────────────────────────────────────────

void OledDisplay::init(int sda_pin, int scl_pin, int rst_pin)
{
    rst_pin_ = rst_pin;

    // Hardware reset
    reset_hw();

    // I2C bus
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(sda_pin);
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(scl_pin);
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus_);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return;
    }

    // SSD1306 device (I2C addr 0x3C)
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = 0x3C;
    dev_cfg.scl_speed_hz = 400000;

    ret = i2c_master_bus_add_device(bus_, &dev_cfg, &dev_);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(ret));
        return;
    }

    // SSD1306 init sequence (128x64)
    static const uint8_t init_cmds[] = {
        0xAE,       // display off
        0xD5, 0x80, // set clock div
        0xA8, 0x3F, // set multiplex (64-1)
        0xD3, 0x00, // set display offset
        0x40,       // set start line 0
        0x8D, 0x14, // charge pump on
        0x20, 0x00, // horizontal addressing mode
        0xA1,       // segment remap (col 127 = SEG0)
        0xC8,       // COM scan direction (remapped)
        0xDA, 0x12, // COM pins config
        0x81, 0xCF, // set contrast
        0xD9, 0xF1, // set precharge
        0xDB, 0x40, // set VCOMH deselect
        0xA4,       // output follows RAM
        0xA6,       // normal display (not inverted)
        0xAF,       // display on
    };

    for (size_t i = 0; i < sizeof(init_cmds); i++)
    {
        send_cmd(init_cmds[i]);
    }

    clear();
    flush();

    initialized_ = true;
    ESP_LOGI(TAG, "SSD1306 128x64 OLED initialized (SDA=%d SCL=%d RST=%d)",
             sda_pin, scl_pin, rst_pin);
}

// ── Framebuffer ops ─────────────────────────────────────────────────────

void OledDisplay::clear()
{
    memset(fb_, 0, sizeof(fb_));
}

void OledDisplay::draw_string(int x, int page, const char *str)
{
    if (page < 0 || page > 7)
    {
        return;
    }

    while (*str && x < 128)
    {
        char c = *str++;
        if (c < 32 || c > 126)
        {
            c = '?';
        }

        const uint8_t *glyph = kFont5x7[c - 32];
        for (int col = 0; col < 5 && (x + col) < 128; col++)
        {
            fb_[page * 128 + x + col] = glyph[col];
        }
        x += 6; // 5px glyph + 1px spacing
    }
}

void OledDisplay::draw_hline(int x, int y, int width)
{
    int page = y / 8;
    uint8_t bit = 1 << (y % 8);

    for (int i = 0; i < width && (x + i) < 128; i++)
    {
        fb_[page * 128 + x + i] |= bit;
    }
}

void OledDisplay::flush()
{
    if (!dev_)
    {
        return;
    }

    // Set column range 0-127
    send_cmd(0x21);
    send_cmd(0);
    send_cmd(127);
    // Set page range 0-7
    send_cmd(0x22);
    send_cmd(0);
    send_cmd(7);

    // Send framebuffer (control byte 0x40 + 1024 data bytes)
    static uint8_t buf[1025];
    buf[0] = 0x40; // Co=0, D/C#=1 → data
    memcpy(buf + 1, fb_, 1024);
    i2c_master_transmit(dev_, buf, sizeof(buf), 200);
}

// ── Status rendering ────────────────────────────────────────────────────

void OledDisplay::update(const NodeStatus &s)
{
    if (!initialized_)
    {
        return;
    }

    clear();
    char line[22]; // 21 chars + null

    // Line 0: node ID + version
    snprintf(line, sizeof(line), "FLP-%04X  v%s", s.node_addr, "0.1.0");
    draw_string(0, 0, line);

    // Line 1: separator
    draw_hline(0, 9, 128);

    // Line 2: connectivity
    snprintf(line, sizeof(line), "W:%s N:%u L:OK",
             s.wifi_connected ? "OK" : "--", s.espnow_peers);
    draw_string(0, 2, line);

    // Line 3: mesh status
    if (s.hops_to_internet == 0xFF)
    {
        snprintf(line, sizeof(line), "Nbrs:%u  GW:--", s.neighbor_count);
    }
    else
    {
        snprintf(line, sizeof(line), "Nbrs:%u  GW:%uh",
                 s.neighbor_count, s.hops_to_internet);
    }
    draw_string(0, 3, line);

    // Line 4: separator
    draw_hline(0, 33, 128);

    // Line 5: transfer
    if (s.transfer_active && s.filename)
    {
        snprintf(line, sizeof(line), "%.12s %u%%", s.filename, s.transfer_pct);
    }
    else
    {
        snprintf(line, sizeof(line), "No transfer");
    }
    draw_string(0, 5, line);

    // Line 6: heap
    snprintf(line, sizeof(line), "Heap: %lukB", (unsigned long) s.free_heap_kb);
    draw_string(0, 6, line);

    // Line 7: uptime
    uint32_t h = s.uptime_s / 3600;
    uint32_t m = (s.uptime_s % 3600) / 60;
    uint32_t sec = s.uptime_s % 60;
    snprintf(line, sizeof(line), "Up %lu:%02lu:%02lu",
             (unsigned long) h, (unsigned long) m, (unsigned long) sec);
    draw_string(0, 7, line);

    flush();
}

} // namespace flp
