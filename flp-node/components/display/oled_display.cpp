#include "oled_display.hpp"

#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "flp_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "oled";

static const int32_t OLED_WIDTH = 128;
static const int32_t OLED_HEIGHT = 64;
static const int32_t PROGRESS_BAR_INNER_W = OLED_WIDTH - 4; /* 124 */
static const int32_t PROGRESS_BAR_FILL_W  = OLED_WIDTH - 2; /* 126 */

/* ── Icon bitmaps (8x8, column-major, LSB = top) ──────────────────────── */
static const uint8_t ICON_WIFI_ON[8] = {
    0x00, 0x7E, 0x42, 0x3C, 0x24, 0x18, 0x10, 0x10};
static const uint8_t ICON_WIFI_OFF[8] = {
    0x00, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x00};
static const uint8_t ICON_LORA[8] = {
    0x00, 0x08, 0x08, 0x08, 0x1C, 0x2A, 0x49, 0x08};

static constexpr int64_t SPLASH_DURATION_US = 2000000; /* 2 seconds */
static constexpr int64_t TRANSFER_HOLD_US = 3000000; /* 3 seconds */
static constexpr int CONTRAST_DIM = 0x10;
static constexpr int CONTRAST_BRIGHT = 0xCF;

namespace flp
{

/* ── Init / Clear (unchanged except splash timestamp) ──────────────────── */

void OledDisplay::init(int sda_pin, int scl_pin, int rst_pin)
{
    ESP_LOGI(TAG,
             "Initializing SSD1306 (SDA=%d SCL=%d RST=%d)...",
             sda_pin,
             scl_pin,
             rst_pin);

    /* Hardware reset (high-low-high pulse) */
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

    /* I2C bus */
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

    /* Add SSD1306 device (I2C addr 0x3C) */
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

    /* Wire handles into the library's device struct */
    dev_._address = 0x3C;
    dev_._flip = false;
    dev_._i2c_num = I2C_NUM_0;
    dev_._i2c_bus_handle = bus;
    dev_._i2c_dev_handle = i2c_dev;

    /* SSD1306 init sequence + clear */
    ssd1306_init(&dev_, OLED_WIDTH, OLED_HEIGHT);
    ssd1306_clear_screen(&dev_, false);

    splash_start_us_ = esp_timer_get_time();
    state_ = State::SPLASH;
    dimmed_ = false;

    initialized_ = true;
    ESP_LOGI(TAG,
             "SSD1306 128x64 OLED initialized (SDA=%d SCL=%d RST=%d)",
             sda_pin,
             scl_pin,
             rst_pin);
}

void OledDisplay::clear()
{
    ssd1306_clear_screen(&dev_, false);
}

/* ── Helpers ───────────────────────────────────────────────────────────── */

void OledDisplay::draw_title_bar(const char *text)
{
    char padded[17];
    snprintf(padded, sizeof(padded), "%-16s", text);
    ssd1306_display_text(&dev_, 0, padded, 16, true);
}

void OledDisplay::draw_progress_bar(int page, uint8_t pct)
{
    if (pct > 100)
    {
        pct = 100;
    }
    int32_t fill = (pct * PROGRESS_BAR_INNER_W) / 100;

    uint8_t bar[OLED_WIDTH];
    bar[0] = 0xFF;
    bar[OLED_WIDTH - 1] = 0xFF;
    for (int32_t i = 1; i <= PROGRESS_BAR_FILL_W; i++)
    {
        bar[i] = (i - 1 < fill) ? 0xFF : 0x81;
    }
    ssd1306_display_image(&dev_, page, 0, bar, OLED_WIDTH);
}

/* ── Screen renderers ──────────────────────────────────────────────────── */

void OledDisplay::render_splash(const NodeStatus &s)
{
    ssd1306_clear_screen(&dev_, false);

    /* Large node address on pages 2-4 (x3 font) */
    char addr[6];
    snprintf(addr, sizeof(addr), "%04X", s.node_addr);
    ssd1306_display_text_x3(&dev_, 2, addr, strlen(addr), false);

    /* Label text on pages 6-7 */
    char version[20];
    snprintf(version, sizeof(version), "  Protocol v%s", FLP_VERSION);
    ssd1306_display_text(&dev_, 6, "  Forest Link", 13, false);
    ssd1306_display_text(&dev_, 7, version, strlen(version), false);
}

void OledDisplay::render_status(const NodeStatus &s)
{
    ssd1306_clear_screen(&dev_, false);
    char line[17];

    /* Page 0: inverted title bar */
#if CONFIG_FLP_WIFI_DISABLED
    snprintf(line, sizeof(line), "FLP-%04X  RELAY", s.node_addr);
#else
    snprintf(line, sizeof(line), "FLP-%04X GATEWAY", s.node_addr);
#endif
    draw_title_bar(line);

    /* Page 1: icons + peer count */
    const uint8_t *wifi_icon = s.wifi_connected ? ICON_WIFI_ON : ICON_WIFI_OFF;
    ssd1306_display_image(&dev_, 1, 0, wifi_icon, 8);
    ssd1306_display_image(&dev_, 1, 16, ICON_LORA, 8);

    snprintf(line, sizeof(line), "P:%02u", s.espnow_peers);
    /*
     * Peer count text starting at character position 5 (seg 40)
     * Display on page 1 — use display_image trick: render text separately
     * We'll use a small text rendered at page offset
     * Actually, ssd1306_display_text always starts at seg 0, so we write padded
     * text
     */
    char peer_line[17];
    snprintf(peer_line, sizeof(peer_line), "     P:%02u", s.espnow_peers);
    ssd1306_display_text(&dev_, 1, peer_line, strlen(peer_line), false);
    /*
     * Re-draw icons over the first chars (display_image overwrites at specific
     * seg)
     */
    ssd1306_display_image(&dev_, 1, 0, wifi_icon, 8);
    ssd1306_display_image(&dev_, 1, 16, ICON_LORA, 8);

    /* Page 2: mesh info */
    if (s.hops_to_internet == 0xFF)
    {
        snprintf(line, sizeof(line), "Nbrs:%-3u GW:--", s.neighbor_count);
    }
    else
    {
        snprintf(line,
                 sizeof(line),
                 "Nbrs:%-3u GW:%uh",
                 s.neighbor_count,
                 s.hops_to_internet);
    }
    ssd1306_display_text(&dev_, 2, line, strlen(line), false);

    /* Page 3: horizontal separator */
    _ssd1306_line(&dev_, 0, 28, 127, 28, false);

    /* Page 4: transfer summary */
    if (s.transfer_active && s.filename)
    {
        snprintf(line, sizeof(line), "%.10s %3u%%", s.filename, s.transfer_pct);
    }
    else
    {
        snprintf(line, sizeof(line), "No transfer");
    }
    ssd1306_display_text(&dev_, 4, line, strlen(line), false);

    /* Page 5: cloud command indicator */
    if (s.cloud_cmd_received)
    {
        snprintf(line, sizeof(line), ">> Cloud CMD RX");
        ssd1306_display_text(&dev_, 5, line, strlen(line), false);
    }

    /* Page 6: heap */
    snprintf(line, sizeof(line), "Heap: %lukB", (unsigned long) s.free_heap_kb);
    ssd1306_display_text(&dev_, 6, line, strlen(line), false);

    /* Page 7: uptime */
    uint32_t h = s.uptime_s / 3600;
    uint32_t m = (s.uptime_s % 3600) / 60;
    uint32_t sec = s.uptime_s % 60;
    snprintf(line,
             sizeof(line),
             "Up %02lu:%02lu:%02lu",
             (unsigned long) h,
             (unsigned long) m,
             (unsigned long) sec);
    ssd1306_display_text(&dev_, 7, line, strlen(line), false);

    /* Flush line-drawing buffer (separator) */
    ssd1306_show_buffer(&dev_);
}

void OledDisplay::render_transfer(const NodeStatus &s)
{
    ssd1306_clear_screen(&dev_, false);
    char line[17];

    /* Page 0: inverted title bar */
#if CONFIG_FLP_WIFI_DISABLED
    snprintf(line, sizeof(line), "FLP-%04X  RELAY", s.node_addr);
#else
    snprintf(line, sizeof(line), "FLP-%04X GATEWAY", s.node_addr);
#endif
    draw_title_bar(line);

    /* Page 1: filename (scrolling if > 16 chars) */
    const char *fname = s.filename ? s.filename : "unknown";
    int flen = strlen(fname);
    if (flen <= 16)
    {
        ssd1306_display_text(&dev_, 1, fname, flen, false);
    }
    else
    {
        /* Circular scroll: "filename   filename", show 16-char window */
        char scroll_buf[64];
        snprintf(scroll_buf, sizeof(scroll_buf), "%s   %s", fname, fname);
        int total_len = flen + 3; /* length of one cycle */
        int offset = scroll_offset_ % total_len;
        char window[17];
        memcpy(window, scroll_buf + offset, 16);
        window[16] = '\0';
        ssd1306_display_text(&dev_, 1, window, 16, false);
    }

    /* Page 3: percentage text (centered) / completion message */
    if (show_complete_)
    {
        snprintf(line, sizeof(line), "    COMPLETE");
        ssd1306_display_text(&dev_, 3, line, strlen(line), false);
        draw_progress_bar(4, 100);
    }
    else
    {
        snprintf(line, sizeof(line), "      %3u%%", s.transfer_pct);
        ssd1306_display_text(&dev_, 3, line, strlen(line), false);
        draw_progress_bar(4, s.transfer_pct);
    }

    /* Page 6: condensed mesh info */
    if (s.hops_to_internet == 0xFF)
    {
        snprintf(line, sizeof(line), "Nbrs:%-3u GW:--", s.neighbor_count);
    }
    else
    {
        snprintf(line,
                 sizeof(line),
                 "Nbrs:%-3u GW:%uh",
                 s.neighbor_count,
                 s.hops_to_internet);
    }
    ssd1306_display_text(&dev_, 6, line, strlen(line), false);

    /* Page 7: heap */
    snprintf(line, sizeof(line), "Heap: %lukB", (unsigned long) s.free_heap_kb);
    ssd1306_display_text(&dev_, 7, line, strlen(line), false);
}

/* ── State machine ─────────────────────────────────────────────────────── */

void OledDisplay::update(const NodeStatus &s)
{
    if (!initialized_)
    {
        return;
    }

    int64_t now = esp_timer_get_time();

    switch (state_)
    {
        case State::SPLASH:
            if (now - splash_start_us_ >= SPLASH_DURATION_US)
            {
                state_ = State::STATUS;
                /* Don't dim right away — let status render once first */
            }
            else
            {
                if (dimmed_)
                {
                    ssd1306_contrast(&dev_, CONTRAST_BRIGHT);
                    dimmed_ = false;
                }
                render_splash(s);
                return;
            }
            break; /* fall through to STATUS */

        case State::STATUS:
            if (s.transfer_active)
            {
                state_ = State::TRANSFER;
                scroll_offset_ = 0;
                /* Brighten on transfer start */
                if (dimmed_)
                {
                    ssd1306_contrast(&dev_, CONTRAST_BRIGHT);
                    dimmed_ = false;
                }
            }
            break;

        case State::TRANSFER:
            if (!s.transfer_active)
            {
                if (transfer_was_active_)
                {
                    /* Transfer just completed — start hold timer */
                    transfer_done_us_ = now;
                    transfer_was_active_ = false;
                    show_complete_ = true;
                }

                if (now - transfer_done_us_ >= TRANSFER_HOLD_US)
                {
                    state_ = State::STATUS;
                    show_complete_ = false;
                }
            }
            else
            {
                transfer_was_active_ = true;
            }
            break;
    }

    /* Contrast dimming: dim when idle on STATUS screen */
    if (state_ == State::STATUS && !s.transfer_active)
    {
        if (!dimmed_)
        {
            ssd1306_contrast(&dev_, CONTRAST_DIM);
            dimmed_ = true;
        }
    }
    else if (state_ == State::TRANSFER)
    {
        if (dimmed_)
        {
            ssd1306_contrast(&dev_, CONTRAST_BRIGHT);
            dimmed_ = false;
        }
    }

    /* Render current screen */
    switch (state_)
    {
        case State::SPLASH:
            render_splash(s);
            break;
        case State::STATUS:
            render_status(s);
            break;
        case State::TRANSFER:
            scroll_offset_++;
            render_transfer(s);
            break;
    }
}

} /* namespace flp */
