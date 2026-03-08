# FLP v3.7 — Dense Forest Environment Analysis

Real-world performance projections for FLP v3.7 deployed in a dense forest ("Green Wall") environment. Covers radio range, maximum network extent, node capacity, and throughput under realistic propagation conditions.

## Radio Parameters

| Parameter | ESP-NOW (ESP32-S3) | LoRa (SX1280, SF7/BW800) |
|-|-|-|
| Frequency | 2.4 GHz | 2.4 GHz |
| TX Power | +9 dBm | +13 dBm |
| RX Sensitivity | -97 dBm | ~-99 dBm |
| Link Budget | 106 dB | ~112 dB |
| PHY Rate | 1 Mbps | ~50 kbps |
| Max Payload | 250 B | 255 B |

## 1. Range in Dense Forest

Propagation model: ITU-R P.833-9 specific attenuation for woodland combined with Weissberger's foliage loss model. All estimates include a **10 dB fade margin** for 90% link reliability under multipath and weather variation.

### ESP-NOW at 2.4 GHz

Foliage specific attenuation: ~0.3–0.5 dB/m (dense canopy, NLOS).

| Distance | Free-Space Path Loss | Foliage Loss | Total Loss | Link Margin |
|-|-|-|-|-|
| 15 m | 56 dB | 6 dB | 62 dB | +44 dB (excellent) |
| 30 m | 62 dB | 11 dB | 73 dB | +33 dB (reliable) |
| 50 m | 66 dB | 17 dB | 83 dB | +23 dB (good) |
| 80 m | 70 dB | 25 dB | 95 dB | +11 dB (marginal) |
| 100 m | 72 dB | 30 dB | 102 dB | +4 dB (unreliable) |

**Reliable ESP-NOW range: 30–50 m** in dense forest with fade margin.

### LoRa at 2.4 GHz

Both FLP transports operate at 2.4 GHz. LoRa's spread-spectrum coding gain gives it ~6 dB advantage over ESP-NOW at SF7, increasing to ~18 dB at SF12. Foliage attenuation at 2.4 GHz is the same as for ESP-NOW: ~0.3–0.5 dB/m.

| Distance | Free-Space Path Loss | Foliage Loss | Total Loss | Link Margin (112 dB budget) |
|-|-|-|-|-|
| 30 m | 62 dB | 11 dB | 73 dB | +39 dB (excellent) |
| 50 m | 66 dB | 17 dB | 83 dB | +29 dB (reliable) |
| 100 m | 72 dB | 30 dB | 102 dB | +10 dB (marginal) |
| 150 m | 75 dB | 45 dB | 120 dB | −8 dB (unreliable at SF7) |

**Reliable LoRa SF7/BW800 range: 50–150 m** in dense forest.

At SF12 (sensitivity ~-112 dBm, coding gain adds ~18 dB over SF7), range extends to ~200–300 m but bitrate drops significantly — viable only for control packets.

### Why ESP-NOW Is Still Preferred for Bulk Transfer

```
SF7/BW800 bitrate:      ~50 kbps = 6,250 B/s
Airtime per 255 B pkt:  ~41 ms
Throughput:             ~24 packets/s × 247 B = ~5,928 B/s ≈ 5.9 KB/s
3 MB at ~5.9 KB/s:      ~533 seconds ≈ 9 minutes (single path)
```

ESP-NOW at 34 KB/s (relay node, post-optimization) delivers a 3 MB file in under 2 minutes over 3 hops. Even though LoRa at 50 kbps is far more capable than narrowband LoRa, it is still ~6× slower than ESP-NOW at the relay layer. FLP uses **ESP-NOW for bulk data, LoRa for broadcast control and extended-range relay** where ESP-NOW cannot reach.

## 2. Maximum Network Diameter

With `DEFAULT_TTL = 8` (maximum 8 hops before packet expiry):

| Transport Mode | Per-Hop Range | Max Diameter (8 hops) |
|-|-|-|
| ESP-NOW-only | 30–50 m | 240–400 m |
| LoRa-only (SF7/BW800) | 50–150 m | 400 m–1.2 km |
| Mixed (ESP-NOW data + LoRa control) | Variable | **0.4–1.0 km** |

Typical FLP deployment uses ESP-NOW for bulk data relay between adjacent nodes and LoRa for broadcast advertisements and route discovery across longer distances. The effective network diameter is **0.4–1.0 km** with mixed transport.

### Extending Range

To push beyond 1 km:
- Increase `DEFAULT_TTL` beyond 8 (adds latency, more retransmissions)
- Use LoRa SF12 for control plane (extends discovery range to ~200–300 m/hop)
- Accept lower throughput at edge nodes (more hops = more relay contention)

Theoretical maximum with TTL=15 and LoRa SF12 control: ~3–4 km diameter, but end-to-end transfer time degrades significantly.

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

**ESP-NOW at 2.4 GHz (Singapore):**

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

**Reliable ESP-NOW hop distance in Singapore: 25–35 m.** Conservative planning value: **30 m.**

**LoRa SF7 at 2.4 GHz (Singapore):**

| Parameter | Value |
|-|-|
| Foliage attenuation | ~0.3–0.5 dB/m (same 2.4 GHz band as ESP-NOW) |
| Humidity absorption penalty | +3 dB (same 2.4 GHz band) |
| Usable link budget | 112 − 10 (fade) − 3 (humidity) = **99 dB** |

| Distance | FSPL | Foliage Loss | Total | Margin (99 dB) |
|-|-|-|-|-|
| 30 m | 62 dB | 12 dB | 74 dB | +25 dB (reliable) |
| 50 m | 66 dB | 20 dB | 86 dB | +13 dB (good) |
| 75 m | 69 dB | 30 dB | 99 dB | 0 dB (at limit) |
| 100 m | 72 dB | 40 dB | 112 dB | −13 dB (unreliable at SF7) |

**Reliable LoRa SF7 hop distance in Singapore: 50–100 m.** Conservative planning value: **75 m.**

### Range and Transfer Time by Node Count

Linear chain topology (maximises range, 1 exit node at far end). Transfer time is for a **3 MB file using ESP-NOW data relay** with the post-optimization parameters.

**Throughput model:**

```
Per-hop ESP-NOW relay throughput (Singapore):   ~25 KB/s (34 KB/s lab × 0.75 forest factor)
ARQ pipeline:                                   32 × 247 B / RTT
Per-hop loss after FEC:                         ~1.5% (5% raw, FEC recovers most)
Pipeline fill delay:                            hops × ~15 ms (negligible vs transfer)
Total fragments (3 MB + FEC 8/7):               14,552
Total bytes on wire:                            14,552 × 255 = 3,710,760 B
```

| Nodes | Hops | ESP-NOW Range | LoRa Range | Mixed Range | RTT | ARQ Pipeline | Effective Throughput | 3 MB Time | Reliability |
|-|-|-|-|-|-|-|-|-|-|
| 2 | 1 | 30 m | 75 m | 75 m | 30 ms | 263 KB/s | 30 KB/s | ~2.0 min | >98% |
| 3 | 2 | 60 m | 150 m | 150 m | 60 ms | 132 KB/s | 25 KB/s | ~2.5 min | >97% |
| 4 | 3 | 90 m | 225 m | 225 m | 100 ms | 79 KB/s | 24 KB/s | ~2.6 min | >95% |
| 5 | 4 | 120 m | 300 m | 300 m | 140 ms | 56 KB/s | 24 KB/s | ~2.7 min | ~93% |
| 6 | 5 | 150 m | 375 m | 375 m | 180 ms | 44 KB/s | 23 KB/s | ~2.8 min | ~91% |
| 7 | 6 | 180 m | 450 m | 450 m | 220 ms | 36 KB/s | 23 KB/s | ~2.8 min | ~88% |
| 8 | 7 | 210 m | 525 m | 525 m | 260 ms | 30 KB/s | 22 KB/s | ~2.9 min | ~85% |
| 9 | 8 | 240 m | 600 m | 600 m | 300 ms | 26 KB/s | 22 KB/s | ~3.0 min | ~82% |

> **Key insight:** Transfer time scales slowly with hop count (2.0 → 3.0 min across 1–8 hops) because the pipelined ARQ keeps all relay links busy in parallel. The relay node's per-hop throughput (~25 KB/s), not the number of hops, is the dominant bottleneck.

### Multi-Exit Configurations

When the network has enough nodes to support multiple exit paths, throughput improves by striping fragments across exits. Each exit path runs an independent ARQ session.

| Nodes | Topology | Exit Nodes | ESP-NOW Diameter | LoRa Diameter | 3 MB Time |
|-|-|-|-|-|-|
| 3 | A → R → E | 1 | 60 m | 150 m | ~2.5 min |
| 4 | A → R → E₁, A → E₂ | 2 | 60 m | 150 m | ~1.8 min |
| 5 | Star: A → E₁/E₂/E₃/R | 3 | 30 m | 75 m | ~1.2 min |
| 6 | A → R₁ → E₁, A → R₂ → E₂ | 2 | 60 m | 150 m | ~1.6 min |
| 7 | Tree: 2 branches × 3 hops + exit | 2 | 90 m | 225 m | ~1.7 min |
| 9 | 3×3 grid, 4 corner exits | 4 | 60 m | 150 m | ~0.9 min |
| 12 | 4×3 grid, 4 edge exits | 4 | 90 m | 225 m | ~1.0 min |
| 16 | 4×4 grid, 4 edge exits | 4 | 90 m | 225 m | ~1.1 min |
| 25 | 5×5 grid, 4 edge exits | 4 | 120 m | 300 m | ~1.3 min |

### Singapore Nature Reserve Coverage

How many nodes are needed to span key Singapore forest sites:

**Bukit Timah Nature Reserve** (~1.6 km × 1.0 km):

```
ESP-NOW linear chain:   1600 / 30  = 54 hops — exceeds TTL=8, not viable alone
LoRa linear chain:      1600 / 75  = 22 hops — exceeds TTL=8

Recommended: Segment into 3 clusters of ~500 m, LoRa spacing 75 m.
Per cluster: ~7–8 nodes covering ~500 m × 300 m.
Coverage: ~21–25 nodes total, 4 exits along the park boundary near roads.
3 MB transfer within cluster: ~1.5 min (4 exits, max 4 hops)
```

**Central Catchment Nature Reserve** (~4 km × 3 km):

```
LoRa linear chain:  4000 / 75 = 54 hops — far exceeds TTL=8

Recommended: Segment into 6–8 independent FLP clusters.
Each cluster: ~10–15 nodes covering ~500 m diameter.
Cluster exits placed at trail intersections near park boundaries.
Total: ~60–80 nodes across 6–8 clusters, 12–16 exit nodes.
3 MB transfer within cluster: ~2 min
Cross-cluster relay: requires exit-to-cloud-to-exit hop
```

**MacRitchie Reservoir Park** (trail loop ~11 km):

```
Trail monitoring (linear):
  11,000 / 75 = 147 LoRa hops — far exceeds TTL

Recommended: 8–10 clusters along the trail loop, each ~1 km.
Per cluster: 10–12 nodes, 2 exits near trail entrances/boardwalks.
Total: ~80–100 nodes, 16–20 exit nodes.
Inter-cluster data reaches cloud via nearest cluster exit.
```

### Node Count vs Coverage Summary (Singapore)

| Nodes | Best Topology | ESP-NOW Coverage | LoRa Coverage | Target Area |
|-|-|-|-|-|
| 3–5 | Linear chain | 60–120 m | 150–300 m | Single trail segment |
| 6–9 | Linear or small grid | 150–240 m | 375–600 m | Small reserve subsection |
| 10–15 | Grid with 2–4 exits | 120–240 m | 300–600 m | Bukit Timah cluster |
| 15–25 | Grid with 4 exits | 120–360 m | 300–900 m | Large reserve section |
| 25–50 | Multi-cluster | Per cluster | Per cluster | Central Catchment (segmented) |
| 60–80 | 6–8 clusters | Per cluster | Per cluster | Full Central Catchment / MacRitchie |

## 4. Maximum Node Count

### Hard Limits (Protocol Constants)

| Constraint | Value | Effect |
|-|-|-|
| `MAX_NEIGHBORS` | 16 | Routing table size per node |
| `ESP-NOW_MAX_PEERS` | 20 | Simultaneous ESP-NOW peers per radio |
| `DEFAULT_TTL` | 8 | Packet hop limit |
| `MAX_EXIT_NODES` | 4 | Parallel cloud drain paths |

### Soft Limits (RF Physics)

**ESP-NOW co-channel interference:**

ESP-NOW uses a fixed WiFi channel with CSMA/CA. Within a ~50 m interference radius in forest, nodes contend on the same channel.

```
Nodes per interference area before degradation:
  ESP-NOW uses fixed channel → CSMA backoff increases with density
  Practical limit: ~8–12 active transmitting nodes per hearing area
  With spatial separation: ~8–10 nodes practical limit per cluster cell
```

**LoRa ALOHA contention:**

FLP uses unslotted ALOHA for LoRa broadcasts (discovery, transfer ads). Maximum useful channel utilization is ~18% (1/2e).

```
Airtime per control packet:     ~41 ms (255 B at SF7/BW800)
Per-node control duty:          ~1 packet / 30 s = 0.14% duty
Max nodes per hearing area:     18% / 0.14% ≈ ~130 nodes
Practical cap (protocol limits):  ~50–60 nodes per cluster
```

At SF7/BW800, LoRa ALOHA contention is no longer the binding constraint — ESP-NOW peer limits and routing table size govern cluster sizing in practice.

### Deployment Sizing Guide

| Size | Nodes | Exit Nodes | Max Hops | Delivery Rate | Notes |
|-|-|-|-|-|-|
| Small | 8–15 | 1–2 | 3 | >95% | Optimal for FLP; all limits comfortable |
| Medium | 15–30 | 2–4 | 5 | ~90% | ESP-NOW relay saturation begins at interior nodes |
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

### Per-Hop ESP-NOW Throughput

```
PHY rate:                           1 Mbps
ESP-NOW frame efficiency (250 B):   250 / (250 + 39 overhead) = ~87%
Channel utilization:                ~70% (CSMA backoff, scheduling)
Gross per-radio:                    1000 × 0.87 × 0.70 ≈ 651 kbps ≈ 81 KB/s

Relay node (2 directions, 1 radio):
  Per-direction:                    81 / 2 = ~40 KB/s

Protocol overhead (8 B header + FEC 8/7):
  Effective:                        40 × (247/255) × (7/8) = ~34 KB/s per hop
```

### Forest-Specific Degradation Factors

| Factor | Impact | Multiplier |
|-|-|-|
| Packet loss (multipath fading) | Retransmissions needed | ×1.05–1.15 |
| RSSI fluctuation | Occasional peer drops, re-establishment | ×1.05 |
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

Shorter transfer duration with optimized ESP-NOW pipeline reduces net energy per transfer despite higher instantaneous radio activity:

| Parameter | Before | After | Power Impact |
|-|-|-|-|
| ESP-NOW send interval | 30–50 ms | 10 ms | ~3–4× more radio-on time during transfer |
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

- 1 hop each, all within ESP-NOW range of exit
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
