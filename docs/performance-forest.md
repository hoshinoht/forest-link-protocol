# FLP v3.7 — Dense Forest Environment Analysis

Real-world performance projections for FLP v3.7 deployed in a dense forest ("Green Wall") environment. Covers radio range, maximum network extent, node capacity, and throughput under realistic propagation conditions.

## Radio Parameters

| Parameter | BLE (ESP32-S3) | LoRa (SX1276, SF7) |
|-|-|-|
| Frequency | 2.4 GHz | 915 MHz |
| TX Power | +9 dBm | +17 dBm |
| RX Sensitivity | -97 dBm | -123 dBm |
| Link Budget | 106 dB | 140 dB |
| PHY Rate | 1 Mbps | 5.47 kbps |
| Max Payload | 504 B | 247 B |

## 1. Range in Dense Forest

Propagation model: ITU-R P.833-9 specific attenuation for woodland combined with Weissberger's foliage loss model. All estimates include a **10 dB fade margin** for 90% link reliability under multipath and weather variation.

### BLE at 2.4 GHz

Foliage specific attenuation: ~0.3–0.5 dB/m (dense canopy, NLOS).

| Distance | Free-Space Path Loss | Foliage Loss | Total Loss | Link Margin |
|-|-|-|-|-|
| 15 m | 56 dB | 6 dB | 62 dB | +44 dB (excellent) |
| 30 m | 62 dB | 11 dB | 73 dB | +33 dB (reliable) |
| 50 m | 66 dB | 17 dB | 83 dB | +23 dB (good) |
| 80 m | 70 dB | 25 dB | 95 dB | +11 dB (marginal) |
| 100 m | 72 dB | 30 dB | 102 dB | +4 dB (unreliable) |

**Reliable BLE range: 30–50 m** in dense forest with fade margin.

> With BLE 5.0 Coded PHY (S=8), sensitivity improves by ~12 dB, extending range to ~70–100 m at the cost of throughput dropping to 125 kbps. FLP uses LE 1M for bulk transfer and could use Coded PHY for control signaling if needed.

### LoRa at 915 MHz

Foliage specific attenuation: ~0.1–0.15 dB/m (lower frequency penetrates better).

| Distance | Free-Space Path Loss | Foliage Loss | Total Loss | Link Margin |
|-|-|-|-|-|
| 100 m | 52 dB | 12 dB | 64 dB | +76 dB (excellent) |
| 200 m | 58 dB | 20 dB | 78 dB | +62 dB (excellent) |
| 500 m | 66 dB | 40 dB | 106 dB | +34 dB (solid) |
| 1000 m | 72 dB | 65 dB | 137 dB | +3 dB (marginal) |

**Reliable LoRa SF7 range: 200–500 m** in dense forest.

At SF12 (sensitivity -137 dBm, link budget 154 dB), range extends to ~1–2 km but bitrate drops to 293 bps — viable only for control packets, not bulk data.

### Why LoRa Can't Do Bulk Transfer

```
SF7/125 kHz bitrate:    5.47 kbps = 683 B/s
Airtime per 255 B pkt:  ~400 ms
Throughput:             ~2.5 packets/s × 247 B = 617 B/s
3 MB at 617 B/s:        5,097 seconds ≈ 85 minutes (single path)
```

This confirms the FLP design: **BLE for bulk data, LoRa for broadcast control and route discovery.**

## 2. Maximum Network Diameter

With `DEFAULT_TTL = 8` (maximum 8 hops before packet expiry):

| Transport Mode | Per-Hop Range | Max Diameter (8 hops) |
|-|-|-|
| BLE-only | 30–50 m | 240–400 m |
| LoRa-only (SF7) | 200–500 m | 1.6–4.0 km |
| Mixed (BLE data + LoRa control) | Variable | **1.5–3 km** |

Typical FLP deployment uses BLE for bulk data relay between adjacent nodes and LoRa for broadcast advertisements and route discovery across longer distances. The effective network diameter is **1.5–3 km** with mixed transport.

### Extending Range

To push beyond 3 km:
- Increase `DEFAULT_TTL` beyond 8 (adds latency, more retransmissions)
- Use LoRa SF12 for control plane (extends discovery range to ~2 km/hop)
- Accept lower throughput at edge nodes (more hops = more relay contention)

Theoretical maximum with TTL=15 and LoRa control: ~7–8 km diameter, but end-to-end transfer time degrades significantly.

## 3. Maximum Node Count

### Hard Limits (Protocol Constants)

| Constraint | Value | Effect |
|-|-|-|
| `MAX_NEIGHBORS` | 16 | Routing table size per node |
| `BLE_MAX_CONNECTIONS` | 4 | Simultaneous BLE links per radio |
| `DEFAULT_TTL` | 8 | Packet hop limit |
| `MAX_EXIT_NODES` | 4 | Parallel cloud drain paths |

### Soft Limits (RF Physics)

**BLE co-channel interference:**

BLE uses 37 data channels with adaptive frequency hopping. Within a ~50 m interference radius in forest, each node's 4 connections consume ~4/37 of the hopping space.

```
Nodes per interference area before degradation:
  37 channels / 4 connections per node = ~9 nodes
  With frequency hopping diversity: ~8–10 nodes practical limit
```

**LoRa ALOHA contention:**

FLP uses unslotted ALOHA for LoRa broadcasts (discovery, transfer ads). Maximum useful channel utilization is ~18% (1/2e).

```
Airtime per control packet:     ~400 ms (255 B at SF7/125 kHz)
Per-node control duty:          ~1 packet / 30 s = 1.3% duty
Max nodes per hearing area:     18% / 1.3% ≈ 14 nodes
```

### Deployment Sizing Guide

| Size | Nodes | Exit Nodes | Max Hops | Delivery Rate | Notes |
|-|-|-|-|-|-|
| Small | 8–15 | 1–2 | 3 | >95% | Optimal for FLP; all limits comfortable |
| Medium | 15–30 | 2–4 | 5 | ~90% | BLE relay saturation begins at interior nodes |
| Large | 30–50 | 4 | 6–8 | ~80% | LoRa contention + TTL pressure; edge nodes slower |
| Theoretical max | ~64 | 4 | 8 | ~70% | 8×8 grid; all hard limits engaged |

**Recommended: 10–25 nodes with 2–4 exit nodes.** This keeps all constraints within comfortable margins and delivers reliable multi-megabyte transfers.

### Scaling Beyond 50 Nodes

To support larger deployments:
1. Increase `MAX_NEIGHBORS` (16 → 32) and `DEFAULT_TTL` (8 → 12)
2. Add LoRa TDMA scheduling to reduce control-plane collisions
3. Deploy more exit nodes (currently capped at 4 per transfer)
4. Consider network segmentation: multiple independent FLP clusters with shared exit nodes

## 4. Throughput in Dense Forest

### Per-Hop BLE Throughput

```
PHY rate:                           1 Mbps
DLE efficiency (251 B PDU):         251 / (251 + 19 overhead) = 93%
Connection event utilization:       ~70% (IFS, hop, scheduling)
Gross per-radio:                    1000 × 0.93 × 0.70 = 651 kbps ≈ 81 KB/s

Relay node (2 connections, 1 radio):
  Per-direction:                    81 / 2 = ~40 KB/s

Protocol overhead (8 B header + FEC 8/7):
  Effective:                        40 × (247/255) × (7/8) = ~34 KB/s per hop
```

### Forest-Specific Degradation Factors

| Factor | Impact | Multiplier |
|-|-|-|
| Packet loss (multipath fading) | Retransmissions needed | ×1.05–1.15 |
| RSSI fluctuation | Occasional connection drops, re-establishment | ×1.05 |
| Humidity / rain | Increased 2.4 GHz attenuation | ×1.05–1.10 |
| Wildlife / wind (foliage movement) | Time-varying channel | ×1.02 |
| Combined | | ×1.10–1.35 |

Effective per-hop throughput in dense forest: **25–31 KB/s** (vs 34 KB/s in lab).

### 3 MB / 3 Hops Transfer Time

```
Total data:             3,145,728 bytes
Data fragments:         12,733
With FEC (8/7):         14,552 total fragments

Single exit path (dense forest):
  14,552 / (28,000 / 255) = 14,552 / 110 frag/s = ~132 s

4-exit parallel striping:
  Source radio output:  81 KB/s total
  Forest degradation:   × 0.80
  Effective:            65 KB/s
  Time = (14,552 × 255) / 65,000 = ~57 s
  Relay contention (~70%): ~82 s
```

### Transfer Time Summary

| Condition | 1 Exit Node | 4 Exit Nodes |
|-|-|-|
| Lab (clean RF, short range) | ~110 s | ~50 s |
| Moderate forest (partial canopy) | ~130 s | ~70 s |
| Dense forest (full canopy, dry) | ~150 s | ~85 s |
| Dense forest + rain | ~170 s | ~95 s |
| Worst case (heavy rain, high humidity) | ~200 s | ~110 s |

**All scenarios meet NFR-MESH1 (3 MB in <20 minutes).** Dense forest with 4 exits achieves **~1.5–2 minutes**, well within the target.

## 5. Power Considerations

Tighter BLE connection intervals (7.5–15 ms vs 30–50 ms) increase power consumption:

| Parameter | Before | After | Power Impact |
|-|-|-|-|
| BLE CI | 30–50 ms | 7.5–15 ms | ~3–4× more radio-on time |
| Transfer duration | ~7–8 min | ~1–2 min | ~4× shorter active period |
| Net energy per transfer | Baseline | ~similar | Shorter duration offsets higher rate |

For battery-powered nodes, the net energy per 3 MB transfer is roughly equivalent — higher instantaneous power but much shorter duration. Idle power is unchanged.

## 6. Deployment Topology Recommendations

### Linear Chain (River/Trail Monitoring)

```
[Sensor]--30m--[Relay]--30m--[Relay]--30m--[Exit]
```

- 3 hops, ~90 m total
- Single exit: ~2.5 min for 3 MB
- Simple, reliable, easy to maintain

### Star Cluster (Clearing with Surrounding Forest)

```
        [Sensor]
           |
[Sensor]--[Exit]--[Sensor]
           |
        [Sensor]
```

- 1 hop each, all within BLE range of exit
- Fastest possible: ~30 s for 3 MB
- Limited coverage area (~50 m radius)

### Tree Topology (Wide Area Coverage)

```
[S]  [S]  [S]  [S]
  \  /      \  /
  [R1]      [R2]
     \      /
      [Exit]
```

- 2 hops, 2 relay branches
- Good balance of coverage and throughput
- ~15–20 nodes, 2 exits recommended

### Grid (Maximum Coverage)

```
[S]-[R]-[R]-[S]
 |   |   |   |
[R]-[R]-[R]-[R]
 |   |   |   |
[S]-[R]-[Exit]-[S]
```

- Up to 64 nodes in 8×8 grid
- Multiple paths provide redundancy
- Interior relay nodes may become bottlenecks
- 4 exit nodes at edges recommended
