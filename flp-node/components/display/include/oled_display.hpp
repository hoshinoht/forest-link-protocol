#pragma once

#include <cstdint>

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

namespace flp
{

struct NodeStatus
{
    uint16_t node_addr;
    bool wifi_connected;
    uint8_t espnow_peers;
    uint8_t neighbor_count;
    uint8_t control_hops_to_internet; /* 0xFF = unknown */
    uint8_t data_hops_to_internet;    /* 0xFF = unknown */
    bool transfer_active;
    const char *filename;
    uint8_t transfer_pct; /* 0-100 */
    uint32_t free_heap_kb;
    uint32_t uptime_s;
    bool cloud_cmd_received;
    bool config_cmd_received;
};

class OledDisplay
{
  public:
    OledDisplay() = default;

    void init(int sda_pin, int scl_pin, int rst_pin = -1);
    void clear();
    void update(const NodeStatus &status);

    bool is_initialized() const { return initialized_; }

    /* LVGL requires periodic ticking from the application */
    static void tick_timer_cb(void *arg);

    /* Public for flush callback access */
    esp_lcd_panel_io_handle_t io_handle_ = nullptr;
    esp_lcd_panel_handle_t panel_handle_ = nullptr;
    lv_display_t *display_ = nullptr;
    static constexpr int OLED_W = 128;
    static constexpr int OLED_H = 64;
    uint8_t oled_buf_[OLED_W * OLED_H / 8] = {};

  private:
    enum class State : uint8_t { SPLASH, STATUS, TRANSFER };

    void create_splash_screen();
    void create_status_screen();
    void create_transfer_screen();

    void show_splash(const NodeStatus &s);
    void show_status(const NodeStatus &s);
    void show_transfer(const NodeStatus &s);

    /* LVGL screens */
    lv_obj_t *scr_splash_ = nullptr;
    lv_obj_t *scr_status_ = nullptr;
    lv_obj_t *scr_transfer_ = nullptr;

    /* Splash screen widgets */
    lv_obj_t *splash_addr_label_ = nullptr;
    lv_obj_t *splash_title_label_ = nullptr;
    lv_obj_t *splash_version_label_ = nullptr;

    /* Status screen widgets */
    lv_obj_t *status_title_label_ = nullptr;
    lv_obj_t *status_wifi_label_ = nullptr;
    lv_obj_t *status_mesh_label_ = nullptr;
    lv_obj_t *status_transfer_label_ = nullptr;
    lv_obj_t *status_cmd_label_ = nullptr;
    lv_obj_t *status_heap_label_ = nullptr;
    lv_obj_t *status_uptime_label_ = nullptr;

    /* Transfer screen widgets */
    lv_obj_t *xfer_title_label_ = nullptr;
    lv_obj_t *xfer_filename_label_ = nullptr;
    lv_obj_t *xfer_pct_label_ = nullptr;
    lv_obj_t *xfer_bar_ = nullptr;
    lv_obj_t *xfer_mesh_label_ = nullptr;
    lv_obj_t *xfer_heap_label_ = nullptr;

    bool initialized_ = false;
    State state_ = State::SPLASH;
    int64_t splash_start_us_ = 0;
    bool splash_anim_started_ = false;
    bool transfer_was_active_ = false;
    bool show_complete_ = false;
    int64_t transfer_done_us_ = 0;

    /* LVGL API mutex (LVGL is not thread-safe) */
    static _lock_t lvgl_lock_;
};

} /* namespace flp */
