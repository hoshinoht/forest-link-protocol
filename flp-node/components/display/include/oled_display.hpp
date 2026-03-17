#pragma once

#include <cstdint>

#include "ssd1306.h"

namespace flp
{

struct NodeStatus
{
    uint16_t node_addr;
    bool wifi_connected;
    uint8_t espnow_peers;
    uint8_t neighbor_count;
    uint8_t hops_to_internet; /* 0xFF = unknown */
    bool transfer_active;
    const char *filename;
    uint8_t transfer_pct; /* 0-100 */
    uint32_t free_heap_kb;
    uint32_t uptime_s;
    bool cloud_cmd_received;
};

class OledDisplay
{
  public:
    OledDisplay() = default;

    void init(int sda_pin, int scl_pin, int rst_pin = -1);
    void clear();
    void update(const NodeStatus &status);

    bool is_initialized() const { return initialized_; }

  private:
    enum class State : uint8_t { SPLASH, STATUS, TRANSFER };

    void render_splash(const NodeStatus &s);
    void render_status(const NodeStatus &s);
    void render_transfer(const NodeStatus &s);
    void draw_title_bar(const char *text);
    void draw_progress_bar(int page, uint8_t pct);

    SSD1306_t dev_ = {};
    bool initialized_ = false;

    State state_ = State::SPLASH;
    int64_t splash_start_us_ = 0;
    bool dimmed_ = false;
    int scroll_offset_ = 0;
    bool transfer_was_active_ = false;
    bool show_complete_ = false;
    int64_t transfer_done_us_ = 0;
};

} /* namespace flp */
