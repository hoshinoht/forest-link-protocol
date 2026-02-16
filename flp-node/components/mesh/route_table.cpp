// =============================================================================
// route_table.cpp
// Neighbour/route table for the mesh brain.
// Also part of Role 2's responsibility — mesh_manager owns this data.
// =============================================================================

#include "route_table.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstring>

static const char *TAG = "route_tbl";

// Maximum neighbours we track (matches FLP_MAX_NEIGHBORS in flp_config.h)
static constexpr size_t MAX_NEIGHBORS = 16;

namespace flp {

// We store the table as a simple fixed-size array — no heap allocation.
// On ESP32-S3 this is fine; 16 entries * ~20 bytes = 320 bytes of stack/BSS.
static NeighborEntry table[MAX_NEIGHBORS];
static size_t        table_size = 0;

void RouteTable::update_neighbor(uint16_t addr, int8_t rssi,
                                  uint8_t hops, bool ble, bool lora)
{
    // Check if we already have an entry for this address
    for (size_t i = 0; i < table_size; i++) {
        if (table[i].addr == addr) {
            // Update existing entry
            table[i].rssi           = rssi;
            table[i].hop_count      = hops;
            table[i].last_seen_ms   = static_cast<uint32_t>(
                esp_timer_get_time() / 1000);
            table[i].ble_reachable  = ble;
            table[i].lora_reachable = lora;
            ESP_LOGD(TAG, "Updated neighbor 0x%04X rssi=%d hops=%u", addr, rssi, hops);
            return;
        }
    }

    // New entry
    if (table_size < MAX_NEIGHBORS) {
        table[table_size].addr           = addr;
        table[table_size].rssi           = rssi;
        table[table_size].hop_count      = hops;
        table[table_size].last_seen_ms   = static_cast<uint32_t>(
            esp_timer_get_time() / 1000);
        table[table_size].ble_reachable  = ble;
        table[table_size].lora_reachable = lora;
        table_size++;
        ESP_LOGI(TAG, "New neighbor 0x%04X rssi=%d hops=%u (table=%zu/%zu)",
                 addr, rssi, hops, table_size, MAX_NEIGHBORS);
    } else {
        ESP_LOGW(TAG, "Neighbor table full — dropping 0x%04X", addr);
    }
}

uint16_t RouteTable::next_hop(uint16_t dst_addr) const
{
    // Direct neighbor? Return them directly.
    for (size_t i = 0; i < table_size; i++) {
        if (table[i].addr == dst_addr) return dst_addr;
    }

    // Not a direct neighbor — return the neighbor with the best RSSI
    // as a best-effort next hop. In a fuller implementation this would
    // be a proper routing table with learned paths.
    if (table_size == 0) return 0; // No neighbors at all

    size_t best = 0;
    for (size_t i = 1; i < table_size; i++) {
        if (table[i].rssi > table[best].rssi) best = i;
    }
    return table[best].addr;
}

NeighborEntry *RouteTable::find(uint16_t addr)
{
    for (size_t i = 0; i < table_size; i++) {
        if (table[i].addr == addr) return &table[i];
    }
    return nullptr;
}

void RouteTable::prune_stale(uint32_t max_age_ms)
{
    uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    size_t i = 0;
    while (i < table_size) {
        uint32_t age = now_ms - table[i].last_seen_ms;
        if (age > max_age_ms) {
            ESP_LOGI(TAG, "Pruning stale neighbor 0x%04X (age=%lums)",
                     table[i].addr, age);
            // Remove by swapping with last entry
            table[i] = table[table_size - 1];
            table_size--;
        } else {
            i++;
        }
    }
}

} // namespace flp
