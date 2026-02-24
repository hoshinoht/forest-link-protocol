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
    uint8_t hops_to_internet; // 0xFF = unknown
    bool transfer_active;
    const char *filename;
    uint8_t transfer_pct; // 0-100
    uint32_t free_heap_kb;
    uint32_t uptime_s;
};

class OledDisplay
{
  public:
    OledDisplay() = default;

    void init(int sda_pin, int scl_pin, int rst_pin = -1);
    void clear();
    void draw_string(int x, int page, const char *str);
    void draw_hline(int x, int y, int width);
    void flush();
    void update(const NodeStatus &status);

    bool is_initialized() const { return initialized_; }

  private:
    SSD1306_t dev_ = {};
    bool initialized_ = false;
};

} // namespace flp
