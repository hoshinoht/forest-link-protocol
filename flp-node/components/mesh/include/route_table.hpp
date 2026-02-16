#pragma once

#include <cstdint>

namespace flp {

struct NeighborEntry {
    uint16_t addr;
    int8_t   rssi;
    uint8_t  hop_count;
    uint32_t last_seen_ms;
    bool     ble_reachable;
    bool     lora_reachable;
};

class RouteTable {
public:
    RouteTable() = default;

    // Add or update a neighbour entry
    void update_neighbor(uint16_t addr, int8_t rssi, uint8_t hops,
                         bool ble, bool lora);

    // Find the best next hop address toward dst_addr
    uint16_t next_hop(uint16_t dst_addr) const;

    // Find a neighbour entry by address — returns nullptr if not found
    // Used by MeshManager for RSSI and hop count lookup before adaptive_send()
    NeighborEntry *find(uint16_t addr);

    // Remove entries not seen within max_age_ms milliseconds
    void prune_stale(uint32_t max_age_ms);
};

} // namespace flp
