# FLP v3.7 — Lab Environment Performance Analysis

Theoretical throughput analysis for a **3 MB file transfer over 3 hops** in a controlled lab environment (clear RF, short range, no foliage attenuation).

## System Parameters

| Parameter | Value |
|-|-|
| ESP-NOW | WiFi PHY (1 Mbps effective) |
| ESP-NOW MTU | 250 bytes |
| LoRa | SF7, BW 800 kHz, CR 4/5, 13 dBm |
| Fragment payload | 247 bytes (LORA_MAX_PAYLOAD, fits any transport) |
| Packet header | 8 bytes |
| ARQ Window | 32 fragments |
| ARQ Timeout | 2000 ms |
| FEC Group | 7 data + 1 parity |
| Max Exit Nodes | 4 |
| MQTT run() cycle | 10 ms |
| Fragment publish queue | 64 entries |

## Before Optimization (Baseline)

| Component | Configuration | Throughput |
|-|-|-|
| ESP-NOW MTU | 250 bytes (hardware fixed) | ATT payload capped at 247 B |
| ESP-NOW send interval | 30–50 ms between sends | ~20–33 TX events/s |
| ARQ Window | 8 fragments | 8 × 247 / 0.3 s RTT = 6.6 KB/s pipeline |
| MQTT run() delay | 500 ms | ~2 fragment drains/s |
| Fragment queue | 8 entries | Overflows under burst arrivals |

### Bottleneck Chain (Before)

```
MQTT drain (2 frag/s = ~0.5 KB/s)             ← primary bottleneck
  → ARQ pipeline (6.6 KB/s)                   ← secondary
    → ESP-NOW effective (~8 KB/s per hop)      ← capped by small ARQ window
```

### 3 MB Transfer Time (Before)

```
Data fragments:     3,145,728 / 247 = 12,733
With FEC (8/7):     12,733 × 8/7 = 14,552 total fragments
MQTT drain rate:    ~2 fragments/s
Time (1 exit):      14,552 / 2 = 7,276 s ≈ 121 minutes (!)
Time (4 exits):     14,552 / (2 × 4) ≈ 30 minutes

Actual bottleneck shifts to ARQ once MQTT isn't saturated:
ARQ-limited:        14,552 × 247 / 6,600 = ~544 s ≈ 9 minutes (1 exit)
Combined:           ~7–8 minutes (4 exits, ARQ + MQTT interleaved)
```

**Estimated: ~7–8 minutes** (4 exit nodes, lab conditions)

## After Optimization

| Component | Configuration | Throughput |
|-|-|-|
| ESP-NOW send pipeline | Burst send with 10 ms inter-packet gap | ~100 TX events/s |
| ARQ Window | 32 fragments | 32 × 247 / 0.1 s RTT = 79 KB/s pipeline |
| MQTT run() delay | 10 ms | ~100 drain cycles/s |
| Fragment queue | 64 entries | ~1.5 s buffer at peak ESP-NOW rate |

### Per-Component Throughput (After)

**ESP-NOW link (lab, single direction):**

```
PHY rate:                       1 Mbps
ESP-NOW frame efficiency:       250 / (250 + 39 overhead) = 87%
Channel utilization:            ~70% (CSMA backoff, scheduling)
Gross per-radio:                1000 × 0.87 × 0.70 ≈ 651 kbps ≈ 81 KB/s
```

**Relay node (2 directions sharing 1 radio):**

```
Per-direction:                  81 / 2 = ~40 KB/s
With protocol overhead:         40 × (247/255) × (7/8) = ~34 KB/s effective
```

**ARQ pipeline (3 hops, lab RTT ~100 ms):**

```
Window:     32 × 247 B = 7,904 B in flight
Pipeline:   7,904 / 0.100 = 79 KB/s — no longer the bottleneck
```

**MQTT exit drain:**

```
run() at 10 ms:                 100 iterations/s
process_fragment_publish():     drains full queue each iteration
ESP-MQTT QoS 1 (LAN broker):   200–500 msg/s capacity
Queue depth 64:                 absorbs burst without overflow
→ No longer the bottleneck
```

### Bottleneck Chain (After)

```
ESP-NOW air-time sharing at relay nodes (~34 KB/s per direction)  ← new bottleneck
  → ARQ pipeline (79 KB/s)                                        ← headroom
    → MQTT drain (200+ frag/s)                                    ← headroom
```

The bottleneck has shifted from software (MQTT delay, ARQ window) to physics (ESP-NOW radio sharing at relay nodes).

### 3 MB Transfer Time (After)

```
Total data:         3,145,728 bytes
Data fragments:     12,733
With FEC (8/7):     14,552 total fragments

Single exit path:
  14,552 / (34,000 / 255) = 14,552 / 133 frag/s = ~109 s

4-exit parallel striping:
  Source radio: 81 KB/s total → (14,552 × 255) / 81,000 = ~46 s
  Relay contention (~70% efficiency): ~66 s
  Retransmissions (~2% loss, lab): × 1.02 = ~67 s
```

**Estimated: ~1–2 minutes** (4 exit nodes, lab conditions)

## Improvement Summary

| Metric | Before | After | Improvement |
|-|-|-|-|
| ESP-NOW effective throughput | ~8 KB/s | ~34 KB/s | 4.3× |
| ARQ pipeline capacity | 6.6 KB/s | 79 KB/s | 12× |
| MQTT drain rate | ~2 frag/s | 200+ frag/s | 100× |
| Fragment queue buffer | ~0.1 s | ~1.5 s | 15× |
| Primary bottleneck | MQTT drain | ESP-NOW air-time | Shifted to physics |
| 3 MB / 3 hops (4 exits) | ~7–8 min | ~1–2 min | 4–5× faster |

## What Changed

| # | Change | File | Risk |
|-|-|-|-|
| 1 | `ARQ_WINDOW` 8 → 32 | `packet.hpp` | +50 KB PSRAM (67 KB total) |
| 2 | MQTT `run()` delay 500 → 10 ms | `mqtt_sn_client.cpp` | Higher CPU on exit node |
| 3 | Fragment queue 8 → 64 | `mqtt_sn_client.cpp` | +30 KB RAM |
| 4 | ESP-NOW inter-packet gap tuned | `espnow_transport.cpp` | None |
| 5 | ARQ window size aligned to ESP-NOW MTU | `packet.hpp` | None |
