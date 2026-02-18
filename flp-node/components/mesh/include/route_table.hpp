#pragma once

#include <cstdint>
#include <cstring>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "packet.hpp"

namespace flp
{

struct NeighborEntry
{
    uint16_t addr;
    int8_t rssi;
    uint8_t hop_count;
    uint8_t hops_to_internet;
    uint32_t last_seen_ms;
    bool ble_reachable;
    bool lora_reachable;
    bool has_internet;
};

class RouteTable
{
  public:
    RouteTable()
    {
        mutex_ = xSemaphoreCreateMutex();
        memset(neighbors_, 0, sizeof(neighbors_));
    }

    void update_neighbor(uint16_t addr,
                         int8_t rssi,
                         uint8_t hops,
                         bool ble,
                         bool lora,
                         uint8_t hops_to_inet = 0xFF)
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);

        // Search for existing entry
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == addr)
            {
                neighbors_[i].rssi = rssi;
                neighbors_[i].hop_count = hops;
                neighbors_[i].last_seen_ms = now;
                neighbors_[i].ble_reachable = ble;
                neighbors_[i].lora_reachable = lora;
                if (hops_to_inet != 0xFF)
                {
                    neighbors_[i].hops_to_internet = hops_to_inet;
                }
                xSemaphoreGive(mutex_);
                return;
            }
        }

        // Add new entry
        if (count_ < MAX_NEIGHBORS)
        {
            neighbors_[count_] = {
                addr, rssi, hops, hops_to_inet, now, ble, lora, false};
            count_++;
        }
        else
        {
            // Evict oldest entry (largest time delta)
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
                addr, rssi, hops, hops_to_inet, now, ble, lora, false};
        }

        xSemaphoreGive(mutex_);
    }

    uint16_t next_hop(uint16_t dst_addr) const
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);

        // Direct neighbor check
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == dst_addr)
            {
                xSemaphoreGive(mutex_);
                return dst_addr;
            }
        }

        // Find neighbor with lowest hop count as relay
        uint16_t best_addr = BROADCAST_ADDR;
        uint8_t best_hops = 0xFF;
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].hop_count < best_hops)
            {
                best_hops = neighbors_[i].hop_count;
                best_addr = neighbors_[i].addr;
            }
        }

        xSemaphoreGive(mutex_);
        return best_addr;
    }

    void prune_stale(uint32_t max_age_ms)
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
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

        xSemaphoreGive(mutex_);
    }

    bool get_neighbor(uint16_t addr, NeighborEntry &out) const
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == addr)
            {
                out = neighbors_[i];
                xSemaphoreGive(mutex_);
                return true;
            }
        }
        xSemaphoreGive(mutex_);
        return false;
    }

    uint8_t get_count() const
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        uint8_t c = count_;
        xSemaphoreGive(mutex_);
        return c;
    }

    bool has_internet_neighbor() const
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].has_internet)
            {
                xSemaphoreGive(mutex_);
                return true;
            }
        }
        xSemaphoreGive(mutex_);
        return false;
    }

    void set_has_internet(uint16_t addr, bool val)
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == addr)
            {
                neighbors_[i].has_internet = val;
                break;
            }
        }
        xSemaphoreGive(mutex_);
    }

    void set_hops_to_internet(uint16_t addr, uint8_t hops)
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == addr)
            {
                neighbors_[i].hops_to_internet = hops;
                break;
            }
        }
        xSemaphoreGive(mutex_);
    }

    uint8_t min_hops_to_internet() const
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        uint8_t best = 0xFF;
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].has_internet ||
                neighbors_[i].hops_to_internet < 0xFF)
            {
                uint8_t h = neighbors_[i].hops_to_internet;
                if (h < best)
                {
                    best = h;
                }
            }
        }
        xSemaphoreGive(mutex_);
        return best;
    }

  private:
    NeighborEntry neighbors_[MAX_NEIGHBORS] = {};
    uint8_t count_ = 0;
    mutable SemaphoreHandle_t mutex_ = nullptr;
};

} // namespace flp
