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

## 3. Theoretical Range by Node Count — Singapore Tropical Forest

Reference environment: **Singapore lowland tropical rainforest** (Bukit Timah Nature Reserve, Central Catchment Nature Reserve, MacRitchie Reservoir Park).

### Singapore Forest Characteristics

| Parameter | Value |
|-|-|
| Forest type | Lowland dipterocarp tropical rainforest |
| Canopy layers | 4 (emergent 40–50 m, canopy 25–35 m, understory 10–20 m, ground cover) |
| Mean humidity | 84% RH (peaks >95% during rain) |
| Annual rainfall | ~2,400 mm |
| Temperature | 25–32 °C year-round |
| Foliage density | Very high — multi-layer canopy with minimal gaps |

### Singapore-Specific Propagation

The combination of dense multi-layer canopy and persistently high humidity makes Singapore one of the more challenging RF environments for forest IoT.

**BLE at 2.4 GHz (Singapore):**

| Parameter | Value |
|-|-|
| Foliage attenuation | ~0.4–0.5 dB/m (denser than temperate forest) |
| Humidity absorption penalty | +3 dB over dry forest baseline |
| Usable link budget | 106 − 10 (fade margin) − 3 (humidity) = **93 dB** |

| Distance | FSPL | Foliage Loss | Total | Margin (93 dB) |
|-|-|-|-|-|
| 15 m | 56 dB | 7 dB | 63 dB | +30 dB (reliable) |
| 25 m | 60 dB | 11 dB | 71 dB | +22 dB (reliable) |
| 30 m | 62 dB | 14 dB | 76 dB | +17 dB (good) |
| 40 m | 64 dB | 18 dB | 82 dB | +11 dB (marginal) |
| 50 m | 66 dB | 23 dB | 89 dB | +4 dB (unreliable) |

**Reliable BLE hop distance in Singapore: 25–35 m.** Conservative planning value: **30 m.**

**LoRa SF7 at 915 MHz (Singapore):**

| Parameter | Value |
|-|-|
| Foliage attenuation | ~0.12–0.18 dB/m |
| Humidity absorption penalty | +2 dB (lower impact at sub-GHz) |
| Usable link budget | 140 − 10 (fade) − 2 (humidity) = **128 dB** |

| Distance | FSPL | Foliage Loss | Total | Margin (128 dB) |
|-|-|-|-|-|
| 100 m | 52 dB | 15 dB | 67 dB | +61 dB (excellent) |
| 200 m | 58 dB | 30 dB | 88 dB | +40 dB (solid) |
| 300 m | 62 dB | 45 dB | 107 dB | +21 dB (good) |
| 400 m | 64 dB | 60 dB | 124 dB | +4 dB (marginal) |

**Reliable LoRa SF7 hop distance in Singapore: 200–300 m.** Conservative planning value: **200 m.**

### Range and Transfer Time by Node Count

Linear chain topology (maximises range, 1 exit node at far end). Transfer time is for a **3 MB file using BLE data relay** with the post-optimization parameters.

**Throughput model:**

```
Per-hop BLE relay throughput (Singapore):   ~25 KB/s (34 KB/s lab × 0.75 forest factor)
ARQ pipeline:                               32 × 247 B / RTT
Per-hop loss after FEC:                     ~1.5% (5% raw, FEC recovers most)
Pipeline fill delay:                        hops × ~15 ms (negligible vs transfer)
Total fragments (3 MB + FEC 8/7):           14,552
Total bytes on wire:                        14,552 × 255 = 3,710,760 B
```

| Nodes | Hops | BLE Range | LoRa Range | Mixed Range | RTT | ARQ Pipeline | Effective Throughput | 3 MB Time | Reliability |
|-|-|-|-|-|-|-|-|-|-|
| 2 | 1 | 30 m | 200 m | 200 m | 30 ms | 263 KB/s | 30 KB/s | ~2.0 min | >98% |
| 3 | 2 | 60 m | 400 m | 400 m | 60 ms | 132 KB/s | 25 KB/s | ~2.5 min | >97% |
| 4 | 3 | 90 m | 600 m | 600 m | 100 ms | 79 KB/s | 24 KB/s | ~2.6 min | >95% |
| 5 | 4 | 120 m | 800 m | 800 m | 140 ms | 56 KB/s | 24 KB/s | ~2.7 min | ~93% |
| 6 | 5 | 150 m | 1.0 km | 1.0 km | 180 ms | 44 KB/s | 23 KB/s | ~2.8 min | ~91% |
| 7 | 6 | 180 m | 1.2 km | 1.2 km | 220 ms | 36 KB/s | 23 KB/s | ~2.8 min | ~88% |
| 8 | 7 | 210 m | 1.4 km | 1.4 km | 260 ms | 30 KB/s | 22 KB/s | ~2.9 min | ~85% |
| 9 | 8 | 240 m | 1.6 km | 1.6 km | 300 ms | 26 KB/s | 22 KB/s | ~3.0 min | ~82% |

> **Key insight:** Transfer time scales slowly with hop count (2.0 → 3.0 min across 1–8 hops) because the pipelined ARQ keeps all relay links busy in parallel. The relay node's per-hop throughput (~25 KB/s), not the number of hops, is the dominant bottleneck.

### Multi-Exit Configurations

When the network has enough nodes to support multiple exit paths, throughput improves by striping fragments across exits. Each exit path runs an independent ARQ session.

| Nodes | Topology | Exit Nodes | BLE Diameter | LoRa Diameter | 3 MB Time |
|-|-|-|-|-|-|
| 3 | A → R → E | 1 | 60 m | 400 m | ~2.5 min |
| 4 | A → R → E₁, A → E₂ | 2 | 60 m | 400 m | ~1.8 min |
| 5 | Star: A → E₁/E₂/E₃/R | 3 | 30 m | 200 m | ~1.2 min |
| 6 | A → R₁ → E₁, A → R₂ → E₂ | 2 | 60 m | 400 m | ~1.6 min |
| 7 | Tree: 2 branches × 3 hops + exit | 2 | 90 m | 600 m | ~1.7 min |
| 9 | 3×3 grid, 4 corner exits | 4 | 60 m | 400 m | ~0.9 min |
| 12 | 4×3 grid, 4 edge exits | 4 | 90 m | 600 m | ~1.0 min |
| 16 | 4×4 grid, 4 edge exits | 4 | 90 m | 600 m | ~1.1 min |
| 25 | 5×5 grid, 4 edge exits | 4 | 120 m | 800 m | ~1.3 min |

### Singapore Nature Reserve Coverage

How many nodes are needed to span key Singapore forest sites:

**Bukit Timah Nature Reserve** (~1.6 km × 1.0 km):

```
BLE linear chain:   1600 / 30  = 54 hops — exceeds TTL=8, not viable alone
LoRa linear chain:  1600 / 200 = 8 hops — just fits TTL=8

Recommended: 9 LoRa-spaced nodes in a line, or a 5×3 grid of 15 nodes
at ~200m LoRa spacing with BLE for local relay.
Coverage: 15 nodes, 4 exits along the park boundary near roads.
3 MB transfer: ~1.5 min (4 exits, max 4 hops)
```

**Central Catchment Nature Reserve** (~4 km × 3 km):

```
LoRa linear chain:  4000 / 200 = 20 hops — exceeds TTL=8

Recommended: Segment into 3–4 independent FLP clusters.
Each cluster: ~15–20 nodes covering ~1.5 km diameter.
Cluster exits placed at trail intersections near park boundaries.
Total: ~50–60 nodes across 3 clusters, 12 exit nodes.
3 MB transfer within cluster: ~2 min
Cross-cluster relay: requires exit-to-cloud-to-exit hop
```

**MacRitchie Reservoir Park** (trail loop ~11 km):

```
Trail monitoring (linear):
  11,000 / 200 = 55 LoRa hops — far exceeds TTL

Recommended: 5 clusters along the trail loop, each ~2 km.
Per cluster: 10–12 nodes, 2 exits near trail entrances/boardwalks.
Total: ~50–60 nodes, 10 exit nodes.
Inter-cluster data reaches cloud via nearest cluster exit.
```

### Node Count vs Coverage Summary (Singapore)

| Nodes | Best Topology | BLE Coverage | LoRa Coverage | Target Area |
|-|-|-|-|-|
| 3–5 | Linear chain | 60–120 m | 400–800 m | Single trail segment |
| 6–9 | Linear or small grid | 150–240 m | 1.0–1.6 km | Small reserve (Bukit Timah width) |
| 10–15 | Grid with 2–4 exits | 120–240 m | 0.8–1.6 km | Bukit Timah full coverage |
| 15–25 | Grid with 4 exits | 120–360 m | 0.8–2.4 km | Large reserve section |
| 25–50 | Multi-cluster | Per cluster | Per cluster | Central Catchment (segmented) |
| 50–60 | 3–4 clusters | Per cluster | Per cluster | Full Central Catchment / MacRitchie |

## 4. Maximum Node Count

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

## 5. Throughput in Dense Forest

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

## 6. Power Considerations

Tighter BLE connection intervals (7.5–15 ms vs 30–50 ms) increase power consumption:

| Parameter | Before | After | Power Impact |
|-|-|-|-|
| BLE CI | 30–50 ms | 7.5–15 ms | ~3–4× more radio-on time |
| Transfer duration | ~7–8 min | ~1–2 min | ~4× shorter active period |
| Net energy per transfer | Baseline | ~similar | Shorter duration offsets higher rate |

For battery-powered nodes, the net energy per 3 MB transfer is roughly equivalent — higher instantaneous power but much shorter duration. Idle power is unchanged.

## 7. Deployment Topology Recommendations

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
