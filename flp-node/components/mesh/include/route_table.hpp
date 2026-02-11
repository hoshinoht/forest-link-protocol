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

    // TODO: Add/update neighbor
    void update_neighbor(uint16_t addr, int8_t rssi, uint8_t hops, bool ble, bool lora);

    // TODO: Find next hop for destination
    uint16_t next_hop(uint16_t dst_addr) const;

    // TODO: Expire stale entries
    void prune_stale(uint32_t max_age_ms);

private:
    // TODO: neighbor storage
};

} // namespace flp
