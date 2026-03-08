#pragma once

#include <cstdint>
#include <cstring>

#include "esp_timer.h"
#include "packet.hpp"

namespace flp
{

inline constexpr uint8_t ROUTE_HOPS_UNKNOWN = 0xFF;
inline constexpr int8_t ROUTE_RSSI_INVALID = -127;
inline constexpr uint8_t ROUTE_FLAG_ESPNOW = 0x01;
inline constexpr uint8_t ROUTE_FLAG_LORA = 0x02;
inline constexpr uint8_t ROUTE_FLAG_INTERNET = 0x04;

struct NeighborEntry
{
    uint16_t addr;
    int8_t rssi;
    uint8_t hop_count;
    uint8_t hops_to_internet;
    uint32_t last_seen_ms;
    bool espnow_reachable;
    bool lora_reachable;
    bool has_internet;
};

/* All RouteTable accesses occur on the single mesh_task — no mutex needed. */
class RouteTable
{
  public:
    RouteTable()
    {
        memset(neighbors_, 0, sizeof(neighbors_));
    }

    void update_neighbor(uint16_t addr,
                         int8_t rssi,
                         uint8_t hops,
                         bool espnow,
                         bool lora,
                         uint8_t hops_to_inet = ROUTE_HOPS_UNKNOWN)
    {
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);

        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == addr)
            {
                neighbors_[i].rssi = rssi;
                neighbors_[i].hop_count = hops;
                neighbors_[i].last_seen_ms = now;
                neighbors_[i].espnow_reachable = espnow;
                neighbors_[i].lora_reachable = lora;
                if (hops_to_inet != ROUTE_HOPS_UNKNOWN)
                {
                    neighbors_[i].hops_to_internet = hops_to_inet;
                }
                return;
            }
        }

        if (count_ < MAX_NEIGHBORS)
        {
            neighbors_[count_] = {
                addr, rssi, hops, hops_to_inet, now, espnow, lora, false};
            count_++;
        }
        else
        {
            uint8_t oldest_idx = 0;
            uint32_t oldest_delta = 0;
            for (uint8_t i = 0; i < count_; i++)
            {
                uint32_t delta = now - neighbors_[i].last_seen_ms;
                if (delta > oldest_delta)
                {
                    oldest_delta = delta;
                    oldest_idx = i;
                }
            }
            neighbors_[oldest_idx] = {
                addr, rssi, hops, hops_to_inet, now, espnow, lora, false};
        }
    }

    uint16_t next_hop(uint16_t dst_addr) const
    {
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == dst_addr)
            {
                return dst_addr;
            }
        }

        uint16_t best_addr = BROADCAST_ADDR;
        uint8_t best_hops_inet = ROUTE_HOPS_UNKNOWN;
        int8_t best_rssi = ROUTE_RSSI_INVALID;
        for (uint8_t i = 0; i < count_; i++)
        {
            uint8_t h = neighbors_[i].hops_to_internet;
            int8_t r = neighbors_[i].rssi;
            if (h < best_hops_inet || (h == best_hops_inet && r > best_rssi))
            {
                best_hops_inet = h;
                best_rssi = r;
                best_addr = neighbors_[i].addr;
            }
        }

        return best_addr;
    }

    void prune_stale(uint32_t max_age_ms)
    {
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);

        uint8_t write = 0;
        for (uint8_t read = 0; read < count_; read++)
        {
            if ((now - neighbors_[read].last_seen_ms) <= max_age_ms)
            {
                if (write != read)
                {
                    neighbors_[write] = neighbors_[read];
                }
                write++;
            }
        }
        count_ = write;
    }

    bool get_neighbor(uint16_t addr, NeighborEntry &out) const
    {
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == addr)
            {
                out = neighbors_[i];
                return true;
            }
        }
        return false;
    }

    uint8_t get_count() const
    {
        return count_;
    }

    bool has_internet_neighbor() const
    {
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].has_internet)
            {
                return true;
            }
        }
        return false;
    }

    void set_has_internet(uint16_t addr, bool val)
    {
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == addr)
            {
                neighbors_[i].has_internet = val;
                if (val)
                {
                    neighbors_[i].hops_to_internet = 0;
                }
                break;
            }
        }
    }

    void set_hops_to_internet(uint16_t addr, uint8_t hops)
    {
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == addr)
            {
                neighbors_[i].hops_to_internet = hops;
                break;
            }
        }
    }

    size_t serialize(uint8_t *buf, size_t max_len) const
    {
        /*
         * Format: [count:1][{addr:2(LE), rssi:1, hops:1, hops_inet:1,
         * flags:1}*N] flags: bit0=espnow_reachable, bit1=lora_reachable,
         * bit2=has_internet
         */
        size_t needed = 1 + count_ * 6;
        if (needed > max_len)
        {
            return 0;
        }
        buf[0] = count_;
        for (uint8_t i = 0; i < count_; i++)
        {
            size_t off = 1 + i * 6;
            memcpy(buf + off, &neighbors_[i].addr, 2); /* little-endian on ESP32 */
            buf[off + 2] = static_cast<uint8_t>(neighbors_[i].rssi);
            buf[off + 3] = neighbors_[i].hop_count;
            buf[off + 4] = neighbors_[i].hops_to_internet;
            uint8_t flags = 0;
            if (neighbors_[i].espnow_reachable)
            {
                flags |= ROUTE_FLAG_ESPNOW;
            }
            if (neighbors_[i].lora_reachable)
            {
                flags |= ROUTE_FLAG_LORA;
            }
            if (neighbors_[i].has_internet)
            {
                flags |= ROUTE_FLAG_INTERNET;
            }
            buf[off + 5] = flags;
        }
        return needed;
    }

    uint8_t min_hops_to_internet() const
    {
        uint8_t best = ROUTE_HOPS_UNKNOWN;
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].has_internet ||
                neighbors_[i].hops_to_internet < ROUTE_HOPS_UNKNOWN)
            {
                uint8_t h = neighbors_[i].hops_to_internet;
                if (h < best)
                {
                    best = h;
                }
            }
        }
        return best;
    }

  private:
    NeighborEntry neighbors_[MAX_NEIGHBORS] = {};
    uint8_t count_ = 0;
};

} /* namespace flp */
