#pragma once

#include <cstdint>
#include <cstring>
#include "esp_timer.h"
#include "packet.hpp"

namespace flp
{

/* RFC 1982 serial number arithmetic: returns true if 'a' is strictly newer
 * than 'b' using the half-window rule for uint16_t (serial space 2^16). */
static inline bool seq_newer(uint16_t a, uint16_t b)
{
    return static_cast<int16_t>(a - b) > 0;
}

/* 8-bit RFC 1982 serial arithmetic for the gateway incarnation field.
 * Wraparound at 256 reboots is unlikely in practice but the half-window
 * rule keeps the comparison correct even if it does occur. */
static inline bool inc_newer(uint8_t a, uint8_t b)
{
    return static_cast<int8_t>(a - b) > 0;
}

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
    uint16_t inet_seq;       /* sequence number of the internet route */
    uint16_t inet_origin;    /* which exit node this route comes from */
    uint8_t inet_incarnation;/* origin gateway's reboot counter — fences DSDV state across gateway restarts (commit 30c36212 race fix) */
    uint16_t tx_count;       /* packets sent to this neighbor */
    uint16_t tx_success;     /* successful transmissions */
    uint16_t etx_x100;      /* ETX * 100 (fixed-point, e.g. 150 = 1.5 ETX) */
    uint8_t queue_load;      /* forwarding load reported by this neighbor */
    bool early_stale_sent;   /* true if we already sent ROUTE_ERROR for this neighbor */
};

/* All RouteTable accesses occur on the single mesh_task — no mutex needed. */
class RouteTable
{
  public:
    static constexpr uint8_t kSerializedNeighborSize = 7;
    static constexpr uint8_t kRouteHysteresisCycles = 3;
    static constexpr uint16_t kMinRouteCostImprovement = 50;

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
                /* OR flags — a peer heard via both ESP-NOW and LoRa must
                 * keep both flags set, not have one overwritten by the
                 * latest packet's transport. */
                neighbors_[i].espnow_reachable |= espnow;
                neighbors_[i].lora_reachable |= lora;
                neighbors_[i].early_stale_sent = false;
                if (hops_to_inet != ROUTE_HOPS_UNKNOWN)
                {
                    neighbors_[i].hops_to_internet = hops_to_inet;
                }
                return;
            }
        }

        NeighborEntry entry = {};
        entry.addr = addr;
        entry.rssi = rssi;
        entry.hop_count = hops;
        entry.hops_to_internet = hops_to_inet;
        entry.last_seen_ms = now;
        entry.espnow_reachable = espnow;
        entry.lora_reachable = lora;
        entry.etx_x100 = 100; /* default ETX = 1.0 */

        if (count_ < MAX_NEIGHBORS)
        {
            neighbors_[count_] = entry;
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
            neighbors_[oldest_idx] = entry;
        }
    }

    /* Step 1c: DSDV sequence-numbered route update.
     *
     * incarnation fencing (commit 30c36212 fix): when an advertisement
     * arrives with a strictly newer incarnation for the same origin, the
     * gateway has rebooted and reset its inet_seq space. Accept the new
     * (origin, seq, incarnation) tuple unconditionally — neighbors that
     * still hold the gateway's pre-reboot seq must not gate the fresh
     * advertisement.
     *
     * For same-incarnation updates, fall through to the existing serial
     * arithmetic on inet_seq (unchanged behavior).
     */
    bool update_inet_route(uint16_t neighbor_addr, uint8_t hops_inet,
                           uint16_t seq, uint16_t origin,
                           uint8_t incarnation = 0)
    {
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr != neighbor_addr)
            {
                continue;
            }
            /* Same origin, newer incarnation: gateway rebooted, fence stale state */
            if (origin == neighbors_[i].inet_origin &&
                inc_newer(incarnation, neighbors_[i].inet_incarnation))
            {
                neighbors_[i].hops_to_internet = hops_inet;
                neighbors_[i].inet_seq = seq;
                neighbors_[i].inet_origin = origin;
                neighbors_[i].inet_incarnation = incarnation;
                return true;
            }
            /* Same origin, older incarnation: caller is reporting state from
             * before a reboot we already learned about. Reject. */
            if (origin == neighbors_[i].inet_origin &&
                inc_newer(neighbors_[i].inet_incarnation, incarnation))
            {
                return false;
            }
            /* Higher seq from same origin (and same incarnation): accept */
            if (origin == neighbors_[i].inet_origin && seq_newer(seq, neighbors_[i].inet_seq))
            {
                neighbors_[i].hops_to_internet = hops_inet;
                neighbors_[i].inet_seq = seq;
                neighbors_[i].inet_origin = origin;
                neighbors_[i].inet_incarnation = incarnation;
                return true;
            }
            /* Same seq: accept only if lower hop count */
            if (origin == neighbors_[i].inet_origin && seq == neighbors_[i].inet_seq)
            {
                if (hops_inet < neighbors_[i].hops_to_internet)
                {
                    neighbors_[i].hops_to_internet = hops_inet;
                    return true;
                }
                return false;
            }
            /* Different origin: accept if better route. Note: incarnations
             * are per-origin so we cannot compare them across origins; we
             * adopt the new origin's incarnation alongside its seq. */
            if (origin != neighbors_[i].inet_origin)
            {
                if (seq_newer(seq, neighbors_[i].inet_seq) ||
                    hops_inet < neighbors_[i].hops_to_internet)
                {
                    neighbors_[i].hops_to_internet = hops_inet;
                    neighbors_[i].inet_seq = seq;
                    neighbors_[i].inet_origin = origin;
                    neighbors_[i].inet_incarnation = incarnation;
                    return true;
                }
                return false;
            }
            /* seq < current: stale, reject */
            return false;
        }
        return false;
    }

    /*
     * LoRa-only penalty: neighbors reachable only via LoRa cannot carry
     * data fragments (1456 bytes > 255 byte LoRa MTU). Add a large cost
     * penalty so ESP-NOW-reachable relay paths are always preferred when
     * available.  The penalty is additive to hops/ETX/queue cost.
     */
    static constexpr uint32_t LORA_ONLY_PENALTY = 500;

    uint32_t route_cost(const NeighborEntry &n) const
    {
        uint32_t cost = (uint32_t)n.hops_to_internet * 100
                      + n.etx_x100
                      + n.queue_load;
        if (n.lora_reachable && !n.espnow_reachable)
        {
            cost += LORA_ONLY_PENALTY;
        }
        return cost;
    }

    /* Step 3c: ETX-weighted composite cost routing
     * my_addr: this node's address, used for loop avoidance (0 = disabled). */
    uint16_t next_hop(uint16_t dst_addr, uint16_t my_addr = 0) const
    {
        /* Direct neighbor: always use direct path */
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == dst_addr)
            {
                return dst_addr;
            }
        }

        /*
         * When routing toward the internet (EXIT_ANY_ADDR), prefer the
         * neighbor with lowest hops_to_internet — standard behaviour.
         *
         * When routing toward a specific node (unicast), skip exit-node
         * neighbors (hops_to_internet == 0) to avoid bouncing packets
         * between exit nodes instead of routing toward deep-field relays.
         * Falls back to any candidate if no non-exit neighbor qualifies.
         */
        bool toward_internet = (dst_addr == EXIT_ANY_ADDR);

        uint16_t best_addr = BROADCAST_ADDR;
        uint32_t best_cost = UINT32_MAX;
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].hops_to_internet >= ROUTE_HOPS_UNKNOWN)
            {
                continue;
            }
            if (!toward_internet && neighbors_[i].hops_to_internet == 0)
            {
                continue; /* skip other exit nodes for unicast routing */
            }
            /* Loop avoidance: skip neighbors whose route leads back to us */
            if (toward_internet && my_addr != 0 &&
                neighbors_[i].inet_origin == my_addr)
            {
                continue;
            }
            uint32_t cost = route_cost(neighbors_[i]);
            if (cost < best_cost)
            {
                best_cost = cost;
                best_addr = neighbors_[i].addr;
            }
        }

        /* Fallback: if all candidates were exit nodes, allow them */
        if (best_addr == BROADCAST_ADDR && !toward_internet)
        {
            for (uint8_t i = 0; i < count_; i++)
            {
                if (neighbors_[i].hops_to_internet >= ROUTE_HOPS_UNKNOWN)
                {
                    continue;
                }
                uint32_t cost = route_cost(neighbors_[i]);
                if (cost < best_cost)
                {
                    best_cost = cost;
                    best_addr = neighbors_[i].addr;
                }
            }
        }

        return best_addr;
    }

    /* Step 3b: Report TX outcome for ETX calculation */
    void report_link_tx(uint16_t addr, bool success)
    {
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == addr)
            {
                neighbors_[i].tx_count++;
                if (success)
                {
                    neighbors_[i].tx_success++;
                }
                uint16_t succ = neighbors_[i].tx_success > 0
                                    ? neighbors_[i].tx_success
                                    : 1;
                neighbors_[i].etx_x100 =
                    static_cast<uint16_t>((neighbors_[i].tx_count * 100) / succ);
                if (neighbors_[i].etx_x100 > 1000)
                {
                    neighbors_[i].etx_x100 = 1000;
                }
                return;
            }
        }
    }

    /* Step 2c: Detect early stale neighbors for route error propagation */
    template <typename Callback>
    void detect_early_stale(uint32_t early_ms, Callback cb)
    {
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].early_stale_sent)
            {
                continue;
            }
            if ((now - neighbors_[i].last_seen_ms) > early_ms)
            {
                neighbors_[i].early_stale_sent = true;
                cb(neighbors_[i].addr,
                   neighbors_[i].inet_origin,
                   neighbors_[i].inet_seq);
            }
        }
    }

    /* Step 1f: Get best inet route info (hops, seq, origin, incarnation) */
    struct InetRouteInfo
    {
        uint8_t hops;
        uint16_t seq;
        uint16_t origin;
        uint8_t incarnation;
    };

    InetRouteInfo best_inet_route() const
    {
        InetRouteInfo best = {ROUTE_HOPS_UNKNOWN, 0, 0, 0};
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].hops_to_internet < best.hops)
            {
                best.hops = neighbors_[i].hops_to_internet;
                best.seq = neighbors_[i].inet_seq;
                best.origin = neighbors_[i].inet_origin;
                best.incarnation = neighbors_[i].inet_incarnation;
            }
        }
        return best;
    }

    /* Step 2e: Invalidate routes through a dead neighbor.
     * Returns true only if a route was actually changed (not already invalid),
     * preventing ROUTE_ERROR rebroadcast storms. */
    bool invalidate_route_via(uint16_t dead_addr)
    {
        bool affected = false;
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == dead_addr &&
                neighbors_[i].hops_to_internet < ROUTE_HOPS_UNKNOWN)
            {
                neighbors_[i].hops_to_internet = ROUTE_HOPS_UNKNOWN;
                affected = true;
            }
        }
        return affected;
    }

    /* Step 5a: Count ESP-NOW reachable neighbors */
    uint8_t get_espnow_neighbor_count() const
    {
        uint8_t count = 0;
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].espnow_reachable)
            {
                count++;
            }
        }
        return count;
    }

    /* Step 5a: Age of oldest contact (ms since last heard from any neighbor) */
    uint32_t oldest_contact_age_ms() const
    {
        if (count_ == 0)
        {
            return UINT32_MAX;
        }
        uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        uint32_t newest = 0;
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].last_seen_ms > newest)
            {
                newest = neighbors_[i].last_seen_ms;
            }
        }
        return now - newest;
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

    /* Test-only: not used by production firmware. */
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

    /* Test-only: not used by production firmware. */
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

    void set_queue_load(uint16_t addr, uint8_t load)
    {
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == addr)
            {
                neighbors_[i].queue_load = load;
                break;
            }
        }
    }

    /* --- Hysteresis: better route tracking --- */
    struct BetterRouteResult
    {
        bool should_switch;
        uint16_t new_addr;
    };

    /* Call once per discovery cycle. Compares current best next-hop cost
     * against all alternatives. Returns should_switch=true only after
     * SWITCH_THRESHOLD_CYCLES consecutive improvements. */
    BetterRouteResult check_better_route(uint16_t current_best, uint16_t my_addr = 0)
    {
        /* Compute cost of current best */
        uint32_t current_cost = UINT32_MAX;
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == current_best &&
                neighbors_[i].hops_to_internet < ROUTE_HOPS_UNKNOWN)
            {
                current_cost = route_cost(neighbors_[i]);
                break;
            }
        }

        /* Find the cheapest alternative (excluding current best) */
        uint16_t alt_addr = 0;
        uint32_t alt_cost = UINT32_MAX;
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].addr == current_best)
            {
                continue;
            }
            if (neighbors_[i].hops_to_internet >= ROUTE_HOPS_UNKNOWN)
            {
                continue;
            }
            if (my_addr != 0 && neighbors_[i].inet_origin == my_addr)
            {
                continue;
            }
            uint32_t cost = route_cost(neighbors_[i]);
            if (cost < alt_cost)
            {
                alt_cost = cost;
                alt_addr = neighbors_[i].addr;
            }
        }

        /* Check if alternative is meaningfully better */
        if (alt_addr != 0 && current_cost > alt_cost &&
            (current_cost - alt_cost) >= kMinRouteCostImprovement)
        {
            if (better_route_.candidate_addr == alt_addr)
            {
                better_route_.consecutive_cycles++;
            }
            else
            {
                better_route_.candidate_addr = alt_addr;
                better_route_.current_best_addr = current_best;
                better_route_.consecutive_cycles = 1;
            }

            if (better_route_.consecutive_cycles >= kRouteHysteresisCycles)
            {
                uint16_t result = better_route_.candidate_addr;
                better_route_ = {}; /* reset after switch */
                return {true, result};
            }
        }
        else
        {
            better_route_ = {}; /* reset — no sustained improvement */
        }

        return {false, 0};
    }

    size_t serialize(uint8_t *buf, size_t max_len) const
    {
        /*
         * Format: [count:1][{addr:2(LE), rssi:1, hops:1, hops_inet:1,
         * flags:1, queue_load:1}*N] flags: bit0=espnow_reachable,
         * bit1=lora_reachable, bit2=has_internet
         */
        size_t needed = 1 + count_ * kSerializedNeighborSize;
        if (needed > max_len)
        {
            return 0;
        }
        buf[0] = count_;
        for (uint8_t i = 0; i < count_; i++)
        {
            size_t off = 1 + i * kSerializedNeighborSize;
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
            buf[off + 6] = neighbors_[i].queue_load;
        }
        return needed;
    }

    /* Minimum control-plane hops to internet.
     * Control traffic can use either LoRa or ESP-NOW, so count any
     * valid route advertisement. */
    uint8_t min_control_hops_to_internet() const
    {
        uint8_t best_any = ROUTE_HOPS_UNKNOWN;
        for (uint8_t i = 0; i < count_; i++)
        {
            if (neighbors_[i].has_internet ||
                neighbors_[i].hops_to_internet < ROUTE_HOPS_UNKNOWN)
            {
                uint8_t h = neighbors_[i].hops_to_internet;
                if (h < best_any)
                {
                    best_any = h;
                }
            }
        }
        return best_any;
    }

    /* Minimum data-plane hops to internet.
     * Bulk file DATA/PARITY packets are constrained to ESP-NOW, so only
     * count routes whose next hop is ESP-NOW reachable. */
    uint8_t min_data_hops_to_internet() const
    {
        uint8_t best_espnow = ROUTE_HOPS_UNKNOWN;
        for (uint8_t i = 0; i < count_; i++)
        {
            if ((neighbors_[i].has_internet ||
                 neighbors_[i].hops_to_internet < ROUTE_HOPS_UNKNOWN) &&
                neighbors_[i].espnow_reachable)
            {
                uint8_t h = neighbors_[i].hops_to_internet;
                if (h < best_espnow)
                {
                    best_espnow = h;
                }
            }
        }
        return best_espnow;
    }

    /* Backward-compatible accessor: use data-plane hops for transfer logic. */
    uint8_t min_hops_to_internet() const
    {
        return min_data_hops_to_internet();
    }

  private:
    struct BetterRouteCandidate
    {
        uint16_t candidate_addr = 0;
        uint16_t current_best_addr = 0;
        uint8_t consecutive_cycles = 0;
    };

    NeighborEntry neighbors_[MAX_NEIGHBORS] = {};
    uint8_t count_ = 0;
    BetterRouteCandidate better_route_ = {};
};

} /* namespace flp */
