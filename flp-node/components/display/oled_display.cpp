#include "oled_display.hpp"

#include <cstdio>
#include <cstring>
#include <sys/lock.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "flp_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

static const char *TAG = "oled";

static constexpr int OLED_WIDTH  = 128;
static constexpr int OLED_HEIGHT = 64;
static constexpr int I2C_HW_ADDR = 0x3C;
static constexpr int I2C_FREQ_HZ = 400 * 1000;
static constexpr int LVGL_TICK_MS = 5;
static constexpr int LVGL_PALETTE_SIZE = 8;

static constexpr int64_t SPLASH_DURATION_US = 2500000; /* 2.5 s */
static constexpr int64_t TRANSFER_HOLD_US   = 3000000; /* 3 s */

namespace flp
{

_lock_t OledDisplay::lvgl_lock_;

namespace
{

void format_hops(char *buf, size_t len, uint8_t hops)
{
    if (hops == 0xFF)
    {
        snprintf(buf, len, "--");
        return;
    }
    if (hops > 99)
    {
        snprintf(buf, len, "99");
        return;
    }
    snprintf(buf, len, "%u", hops);
}

} /* namespace */

/* ── LVGL flush: convert I1 horizontal → SSD1306 vertical column-major ─ */

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    auto *self = static_cast<OledDisplay *>(lv_display_get_user_data(disp));
    if (!self) return;

    esp_lcd_panel_handle_t panel = self->panel_handle_;
    uint8_t *oled_buf = self->oled_buf_;
    px_map += LVGL_PALETTE_SIZE;

    uint16_t hor_res = lv_display_get_physical_horizontal_resolution(disp);
    int x1 = area->x1, x2 = area->x2;
    int y1 = area->y1, y2 = area->y2;

    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            bool pixel_on = (px_map[(hor_res >> 3) * y + (x >> 3)] & (1 << (7 - (x % 8))));
            uint8_t *buf = oled_buf + hor_res * (y >> 3) + x;
            if (pixel_on) {
                *buf |= (1 << (y % 8));
            } else {
                *buf &= ~(1 << (y % 8));
            }
        }
    }
    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2 + 1, y2 + 1, oled_buf);
}

static bool notify_flush_ready(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    auto *disp = static_cast<lv_display_t *>(user_ctx);
    lv_display_flush_ready(disp);
    return false;
}

void OledDisplay::tick_timer_cb(void *arg) { lv_tick_inc(LVGL_TICK_MS); }

/* ══════════════════════════════════════════════════════════════════════════
 * Initialization
 * ══════════════════════════════════════════════════════════════════════ */

void OledDisplay::init(int sda_pin, int scl_pin, int rst_pin)
{
    ESP_LOGI(TAG, "Initializing LVGL OLED (SDA=%d SCL=%d RST=%d)...",
             sda_pin, scl_pin, rst_pin);

    if (rst_pin >= 0) {
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

    /*
     * I2C bus recovery — if a previous boot crashed mid-I2C-transaction
     * (e.g. PSRAM corruption reboot), the SSD1306 slave may be holding SDA
     * low waiting for clocks.  Toggle SCL 9+ times at GPIO level to clock
     * out the stuck byte, then generate a STOP condition.  Without this the
     * I2C master init succeeds but every subsequent transaction NAKs.
     */
    {
        gpio_config_t scl_cfg = {};
        scl_cfg.pin_bit_mask = 1ULL << scl_pin;
        scl_cfg.mode = GPIO_MODE_OUTPUT_OD;
        scl_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&scl_cfg);

        gpio_config_t sda_cfg = {};
        sda_cfg.pin_bit_mask = 1ULL << sda_pin;
        sda_cfg.mode = GPIO_MODE_OUTPUT_OD;
        sda_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&sda_cfg);

        auto scl = static_cast<gpio_num_t>(scl_pin);
        auto sda = static_cast<gpio_num_t>(sda_pin);

        /* Clock out up to 9 bits to free a stuck slave */
        gpio_set_level(sda, 1);
        for (int i = 0; i < 9; i++) {
            gpio_set_level(scl, 1);
            esp_rom_delay_us(5);
            gpio_set_level(scl, 0);
            esp_rom_delay_us(5);
        }

        /* Generate STOP condition: SDA low→high while SCL is high */
        gpio_set_level(sda, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(5);
        gpio_set_level(sda, 1);
        esp_rom_delay_us(5);

        /* Release pins so I2C driver can reconfigure them */
        gpio_reset_pin(scl);
        gpio_reset_pin(sda);
    }

    /* I2C */
    i2c_master_bus_handle_t i2c_bus = nullptr;
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(sda_pin);
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(scl_pin);
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    /* esp_lcd panel IO */
    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr = I2C_HW_ADDR;
    io_cfg.scl_speed_hz = I2C_FREQ_HZ;
    io_cfg.control_phase_bytes = 1;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.dc_bit_offset = 6;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &io_handle_));

    /* SSD1306 panel */
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.bits_per_pixel = 1;
    panel_cfg.reset_gpio_num = rst_pin;
    esp_lcd_panel_ssd1306_config_t ssd_cfg = {};
    ssd_cfg.height = OLED_HEIGHT;
    panel_cfg.vendor_config = &ssd_cfg;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle_, &panel_cfg, &panel_handle_));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle_));
    /*
     * After a crash/reboot the SSD1306 may retain stale state (RST=-1 means
     * no hardware reset pin).  Send a display-off command before init so the
     * controller re-runs its internal power-on sequence cleanly.
     */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle_, false));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle_));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle_, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle_, true, true));

    /*
     * Force the panel RAM to a known state before LVGL starts issuing partial
     * flushes. This avoids visible garbage after brownouts/reset loops where
     * the SSD1306 can retain stale contents across reboots.
     */
    memset(oled_buf_, 0, sizeof(oled_buf_));
    ESP_ERROR_CHECK(
        esp_lcd_panel_draw_bitmap(panel_handle_, 0, 0, OLED_WIDTH, OLED_HEIGHT, oled_buf_));

    /* LVGL */
    lv_init();
    display_ = lv_display_create(OLED_WIDTH, OLED_HEIGHT);
    lv_display_set_user_data(display_, this);
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_I1);

    size_t buf_sz = OLED_WIDTH * OLED_HEIGHT / 8 + LVGL_PALETTE_SIZE;
    void *draw_buf = heap_caps_calloc(1, buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!draw_buf)
        draw_buf = heap_caps_calloc(1, buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(draw_buf);

    /*
     * LVGL I1 format stores a 2-entry lv_color32_t palette at the start of
     * the draw buffer.  Index 0 = "off" pixel (black), index 1 = "on" pixel.
     * Without setting index 1 to white, every rendered pixel maps to black
     * and the display stays blank.
     */
    auto *palette = static_cast<lv_color32_t *>(draw_buf);
    palette[0] = lv_color_to_32(lv_color_black(), LV_OPA_COVER);
    palette[1] = lv_color_to_32(lv_color_white(), LV_OPA_COVER);

    lv_display_set_buffers(display_, draw_buf, nullptr, buf_sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display_, lvgl_flush_cb);

    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = notify_flush_ready };
    esp_lcd_panel_io_register_event_callbacks(io_handle_, &cbs, display_);

    const esp_timer_create_args_t tick_args = {
        .callback = &OledDisplay::tick_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lvgl_tick",
        .skip_unhandled_events = false,
    };
    esp_timer_handle_t tick_timer = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_MS * 1000));

    /* Build screens */
    create_splash_screen();
    create_status_screen();
    create_transfer_screen();

    splash_start_us_ = esp_timer_get_time();
    state_ = State::SPLASH;
    lv_screen_load(scr_splash_);

    initialized_ = true;
    ESP_LOGI(TAG, "LVGL OLED ready");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Helpers
 * ══════════════════════════════════════════════════════════════════════ */

static void strip_defaults(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

/* Create an 8px monospace label */
static lv_obj_t *label8(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* Create a full-width centered label */
static lv_obj_t *label8_center(lv_obj_t *parent, int y)
{
    lv_obj_t *l = label8(parent, 0, y);
    lv_obj_set_width(l, OLED_WIDTH);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

/* Horizontal line separator */
static void add_hline(lv_obj_t *parent, int y)
{
    static lv_point_precise_t pts[] = {{0, 0}, {127, 0}};
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, pts, 2);
    lv_obj_set_style_line_color(line, lv_color_white(), 0);
    lv_obj_set_style_line_width(line, 1, 0);
    lv_obj_set_pos(line, 0, y);
}

/* ══════════════════════════════════════════════════════════════════════════
 * SPLASH SCREEN
 *
 * ┌────────────────────────────┐
 * │                            │  y0
 * │                            │
 * │         7 E 4 0            │  centered, unscii_16 (big)
 * │                            │
 * │     ── Forest Link ──      │  y44, 8px, centered
 * │      Protocol v0.3.3       │  y54, 8px, centered
 * └────────────────────────────┘
 *
 * Fade-in via opacity animation on the address label.
 * ══════════════════════════════════════════════════════════════════════ */

void OledDisplay::create_splash_screen()
{
    scr_splash_ = lv_obj_create(nullptr);
    strip_defaults(scr_splash_);

    /* Big node address — 16px monospace, centered */
    splash_addr_label_ = lv_label_create(scr_splash_);
    lv_obj_set_style_text_font(splash_addr_label_, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(splash_addr_label_, lv_color_white(), 0);
    lv_obj_set_width(splash_addr_label_, OLED_WIDTH);
    lv_obj_set_style_text_align(splash_addr_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(splash_addr_label_, LV_ALIGN_CENTER, 0, -8);
    /* Start invisible for fade-in */
    lv_obj_set_style_opa(splash_addr_label_, LV_OPA_TRANSP, 0);

    /* Decorative line above text section */
    add_hline(scr_splash_, 42);

    /* "Forest Link" */
    splash_title_label_ = label8_center(scr_splash_, 46);
    lv_label_set_text_static(splash_title_label_, "Forest Link");

    /* Version */
    splash_version_label_ = label8_center(scr_splash_, 56);
}

/* ══════════════════════════════════════════════════════════════════════════
 * STATUS SCREEN
 *
 * ┌─ FLP-7E40 GATEWAY ────────┐  y0  inverted bar
 * │ W:OK  LoRa  P:02          │  y11
 * ├────────────────────────────┤  y20 separator
 * │ Mesh: 3 nbrs  GW: 1 hop   │  y22
 * │ Xfer: idle                 │  y32
 * │ >> Cloud CMD RX            │  y42
 * ├────────────────────────────┤  y51 separator
 * │ Heap:2000kB  Up 00:20:14  │  y54
 * └────────────────────────────┘
 * ══════════════════════════════════════════════════════════════════════ */

void OledDisplay::create_status_screen()
{
    scr_status_ = lv_obj_create(nullptr);
    strip_defaults(scr_status_);

    /* Title bar — label with white background, clip overflow */
    status_title_label_ = lv_label_create(scr_status_);
    lv_obj_set_style_text_font(status_title_label_, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(status_title_label_, lv_color_black(), 0);
    lv_obj_set_style_bg_color(status_title_label_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(status_title_label_, LV_OPA_COVER, 0);
    lv_label_set_long_mode(status_title_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(status_title_label_, OLED_WIDTH);
    lv_obj_set_pos(status_title_label_, 0, 0);

    /* Connectivity line */
    status_wifi_label_ = label8(scr_status_, 1, 11);

    /* Separator */
    add_hline(scr_status_, 20);

    /* Mesh info */
    status_mesh_label_ = label8(scr_status_, 1, 22);

    /* Transfer summary */
    status_transfer_label_ = label8(scr_status_, 1, 32);

    /* Cloud cmd */
    status_cmd_label_ = label8(scr_status_, 1, 42);

    /* Bottom separator */
    add_hline(scr_status_, 51);

    /* Heap + uptime on one line */
    status_heap_label_ = label8(scr_status_, 1, 54);
    status_uptime_label_ = label8(scr_status_, 0, 54);
    lv_obj_set_width(status_uptime_label_, OLED_WIDTH - 2);
    lv_obj_set_style_text_align(status_uptime_label_, LV_TEXT_ALIGN_RIGHT, 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * TRANSFER SCREEN
 *
 * ┌─ FLP-7E40 GATEWAY ────────┐  y0  inverted bar
 * │ demo.txt                   │  y12 (auto-scroll if long)
 * ├────────────────────────────┤  y21 separator
 * │           47%              │  y24 centered, 16px big number
 * │ [████████░░░░░░░░░░░░░░░]  │  y42 progress bar
 * ├────────────────────────────┤  y51 separator
 * │ Nbrs:3 GW:1h  Heap:2000kB │  y54
 * └────────────────────────────┘
 * ══════════════════════════════════════════════════════════════════════ */

void OledDisplay::create_transfer_screen()
{
    scr_transfer_ = lv_obj_create(nullptr);
    strip_defaults(scr_transfer_);

    /* Title bar — clip overflow */
    xfer_title_label_ = lv_label_create(scr_transfer_);
    lv_obj_set_style_text_font(xfer_title_label_, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(xfer_title_label_, lv_color_black(), 0);
    lv_obj_set_style_bg_color(xfer_title_label_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(xfer_title_label_, LV_OPA_COVER, 0);
    lv_label_set_long_mode(xfer_title_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(xfer_title_label_, OLED_WIDTH);
    lv_obj_set_pos(xfer_title_label_, 0, 0);

    /* Filename — auto-scroll for long names */
    xfer_filename_label_ = label8(scr_transfer_, 1, 12);
    lv_label_set_long_mode(xfer_filename_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(xfer_filename_label_, OLED_WIDTH - 2);

    /* Separator */
    add_hline(scr_transfer_, 21);

    /* Big percentage — 16px centered */
    xfer_pct_label_ = lv_label_create(scr_transfer_);
    lv_obj_set_style_text_font(xfer_pct_label_, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(xfer_pct_label_, lv_color_white(), 0);
    lv_obj_set_width(xfer_pct_label_, OLED_WIDTH);
    lv_obj_set_style_text_align(xfer_pct_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(xfer_pct_label_, 0, 24);

    /* Progress bar */
    xfer_bar_ = lv_bar_create(scr_transfer_);
    lv_obj_set_size(xfer_bar_, OLED_WIDTH - 6, 6);
    lv_obj_set_pos(xfer_bar_, 3, 43);
    lv_bar_set_range(xfer_bar_, 0, 100);
    lv_obj_set_style_bg_color(xfer_bar_, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(xfer_bar_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(xfer_bar_, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(xfer_bar_, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(xfer_bar_, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(xfer_bar_, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(xfer_bar_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(xfer_bar_, 0, LV_PART_INDICATOR);

    /* Bottom separator */
    add_hline(scr_transfer_, 51);

    /* Bottom info line: mesh + heap */
    xfer_mesh_label_ = label8(scr_transfer_, 1, 54);
    xfer_heap_label_ = label8(scr_transfer_, 0, 54);
    lv_obj_set_width(xfer_heap_label_, OLED_WIDTH - 2);
    lv_obj_set_style_text_align(xfer_heap_label_, LV_TEXT_ALIGN_RIGHT, 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Screen update helpers
 * ══════════════════════════════════════════════════════════════════════ */

void OledDisplay::show_splash(const NodeStatus &s)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%04X", s.node_addr);
    lv_label_set_text(splash_addr_label_, buf);

    snprintf(buf, sizeof(buf), "Protocol v%s", FLP_VERSION);
    lv_label_set_text(splash_version_label_, buf);
}

void OledDisplay::show_status(const NodeStatus &s)
{
    /* 128px / 8px per glyph = 16 chars max per line */
    char line[17];

#if CONFIG_FLP_WIFI_DISABLED
    snprintf(line, sizeof(line), "FLP-%04X  RELAY", s.node_addr);       /* 15 chars */
#else
    snprintf(line, sizeof(line), "FLP-%04X GATEWAY", s.node_addr);      /* 16 chars */
#endif
    lv_label_set_text(status_title_label_, line);

    /* Connectivity:  "W:OK Lo P:02"  = 12 chars */
    snprintf(line, sizeof(line), "%s Lo P:%u",
             s.wifi_connected ? "W:OK" : "W:--", s.espnow_peers);
    lv_label_set_text(status_wifi_label_, line);

    /* Mesh: "N16 C10 D10" = 11 chars max */
    char ctrl_hops[3];
    char data_hops[3];
    format_hops(ctrl_hops, sizeof(ctrl_hops), s.control_hops_to_internet);
    format_hops(data_hops, sizeof(data_hops), s.data_hops_to_internet);
    snprintf(line, sizeof(line), "N%u C%s D%s",
             s.neighbor_count, ctrl_hops, data_hops);
    lv_label_set_text(status_mesh_label_, line);

    /* Transfer:  "Xfer: idle" or "Xfer:name 47%" */
    if (s.transfer_active && s.filename)
        snprintf(line, sizeof(line), "%.8s %3u%%", s.filename, s.transfer_pct);
    else
        snprintf(line, sizeof(line), "Xfer: idle");
    lv_label_set_text(status_transfer_label_, line);

    /* Command / message indicator priority: msg topic > config > cloud cmd. */
    if (s.topic_msg_received && s.topic_msg && s.topic_msg[0] != '\0')
        lv_label_set_text(status_cmd_label_, s.topic_msg);
    else if (s.config_cmd_received)
        lv_label_set_text(status_cmd_label_, ">> CFG CMD");
    else if (s.cloud_cmd_received)
        lv_label_set_text(status_cmd_label_, ">> Cloud CMD");
    else
        lv_label_set_text(status_cmd_label_, "");

    /* Heap */
    snprintf(line, sizeof(line), "%lukB", (unsigned long)s.free_heap_kb);
    lv_label_set_text(status_heap_label_, line);

    /* Uptime */
    uint32_t h = s.uptime_s / 3600, m = (s.uptime_s % 3600) / 60, sec = s.uptime_s % 60;
    snprintf(line, sizeof(line), "%02lu:%02lu:%02lu",
             (unsigned long)h, (unsigned long)m, (unsigned long)sec);
    lv_label_set_text(status_uptime_label_, line);
}

void OledDisplay::show_transfer(const NodeStatus &s)
{
    char line[17];

#if CONFIG_FLP_WIFI_DISABLED
    snprintf(line, sizeof(line), "FLP-%04X  RELAY", s.node_addr);
#else
    snprintf(line, sizeof(line), "FLP-%04X GATEWAY", s.node_addr);
#endif
    lv_label_set_text(xfer_title_label_, line);

    lv_label_set_text(xfer_filename_label_, s.filename ? s.filename : "unknown");

    if (show_complete_) {
        lv_label_set_text(xfer_pct_label_, "DONE");
        lv_bar_set_value(xfer_bar_, 100, LV_ANIM_OFF);
    } else {
        snprintf(line, sizeof(line), "%u%%", s.transfer_pct);
        lv_label_set_text(xfer_pct_label_, line);
        lv_bar_set_value(xfer_bar_, s.transfer_pct, LV_ANIM_ON);
    }

    /* Mesh: "N16 C10 D10" = 11 chars max */
    char ctrl_hops[3];
    char data_hops[3];
    format_hops(ctrl_hops, sizeof(ctrl_hops), s.control_hops_to_internet);
    format_hops(data_hops, sizeof(data_hops), s.data_hops_to_internet);
    snprintf(line, sizeof(line), "N%u C%s D%s",
             s.neighbor_count, ctrl_hops, data_hops);
    lv_label_set_text(xfer_mesh_label_, line);

    snprintf(line, sizeof(line), "%lukB", (unsigned long)s.free_heap_kb);
    lv_label_set_text(xfer_heap_label_, line);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Clear
 * ══════════════════════════════════════════════════════════════════════ */

void OledDisplay::clear()
{
    if (display_) {
        _lock_acquire(&lvgl_lock_);
        lv_obj_clean(lv_screen_active());
        _lock_release(&lvgl_lock_);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * State machine — called from display_task every 500ms
 * ══════════════════════════════════════════════════════════════════════ */

void OledDisplay::update(const NodeStatus &s)
{
    if (!initialized_) return;

    _lock_acquire(&lvgl_lock_);

    int64_t now = esp_timer_get_time();

    switch (state_) {
        case State::SPLASH:
            show_splash(s);

            /* Trigger fade-in animation once */
            if (!splash_anim_started_) {
                splash_anim_started_ = true;
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, splash_addr_label_);
                lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
                lv_anim_set_duration(&a, 800);
                lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
                    lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj),
                                         static_cast<lv_opa_t>(v), 0);
                });
                lv_anim_start(&a);
            }

            if (now - splash_start_us_ >= SPLASH_DURATION_US) {
                state_ = State::STATUS;
                /* Animated screen transition — slide up */
                lv_screen_load_anim(scr_status_, LV_SCR_LOAD_ANIM_MOVE_TOP,
                                    300, 0, false);
            }
            break;

        case State::STATUS:
            show_status(s);
            if (s.transfer_active) {
                state_ = State::TRANSFER;
                lv_screen_load_anim(scr_transfer_, LV_SCR_LOAD_ANIM_MOVE_LEFT,
                                    200, 0, false);
            }
            break;

        case State::TRANSFER:
            if (!s.transfer_active) {
                if (transfer_was_active_) {
                    transfer_done_us_ = now;
                    transfer_was_active_ = false;
                    show_complete_ = true;
                }
                if (now - transfer_done_us_ >= TRANSFER_HOLD_US) {
                    state_ = State::STATUS;
                    show_complete_ = false;
                    lv_screen_load_anim(scr_status_, LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                                        200, 0, false);
                }
            } else {
                transfer_was_active_ = true;
            }
            show_transfer(s);
            break;
    }

    lv_timer_handler();
    _lock_release(&lvgl_lock_);
}

} /* namespace flp */
