# Forest Link Protocol (FLP)

Hybrid ESP-NOW + LoRa mesh networking for IoT sensor deployments in dense forest environments. Course project for **CSC2106 IoT Protocols and Networks** at the Singapore Institute of Technology.

## Overview

Dense forest canopy attenuates 2.4 GHz links, creating a bandwidth-reach paradox: WiFi/ESP-NOW is fast but short-range through foliage, while LoRa reaches further but lacks throughput. FLP bridges both with a hybrid mesh — **ESP-NOW** carries bulk data fragments, **LoRa** handles discovery, routing, and control, and **WiFi + MQTT** uplinks to the cloud.

**How a transfer works:** source node advertises over LoRa &rarr; up to 4 exit nodes are elected &rarr; file is split into 1456-byte fragments &rarr; fragments relay over ESP-NOW hops &rarr; Selective Repeat ARQ (window=64) with FEC parity (7+1) handles loss &rarr; exit nodes publish to MQTT &rarr; `cloud-admin` reassembles and verifies with CRC32.

**Key design choices:** DSDV routing with ETX/queue-aware cost &middot; `ROUTE_ERROR` propagation &middot; adaptive LoRa SF (SF7–SF10) &middot; 8-byte packet header, 1470-byte max MTU &middot; PSRAM-backed ARQ, buffering, and SD card LRU cache &middot; LVGL 9 OLED UI &middot; one active transfer at a time.

## Hardware

Targets the **LILYGO T3-S3 v1.2 SX1280** (ESP32-S3, 4 MB flash, 2 MB PSRAM, SX1280 2.4 GHz LoRa, SSD1306 OLED, microSD slot). Not compatible with older SX1276/SX1262 T3-S3 variants.

<details><summary>Default pinout</summary>

| Function | GPIO | | Function | GPIO |
| --- | --- | --- | --- | --- |
| LoRa MOSI | 6 | | SD MOSI | 11 |
| LoRa MISO | 3 | | SD MISO | 2 |
| LoRa SCK | 5 | | SD SCK | 14 |
| LoRa CS | 7 | | SD CS | 13 |
| LoRa RST | 8 | | OLED SDA | 18 |
| LoRa DIO1 | 9 | | OLED SCL | 17 |
| LoRa BUSY | 36 | | UART TX/RX | 43 / 44 |
| LED | 38 | | | |

</details>

## Quick Start

**Prerequisites:** ESP-IDF **v5.5.x**, Python 3.8+, Docker (for cloud-admin).

```bash
# 1. Install ESP-IDF (if needed)
git clone -b v5.5.3 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v5.5.3
~/esp/esp-idf-v5.5.3/install.sh esp32s3 && source ~/esp/esp-idf-v5.5.3/export.sh

# 2. Build & flash
cd flp-node
idf.py set-target esp32s3 && idf.py build
idf.py -p /dev/tty.usbmodemXXXX flash monitor

# 3. Configure (WiFi, MQTT, demo mode, GPIOs)
idf.py menuconfig   # → Forest Link Protocol

# Relay-only build
rm -f sdkconfig
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.relay" set-target esp32s3
idf.py build
```

## Cloud Backend

Mosquitto broker + Go dashboard/API + optional Cloudflare Tunnel. Ports: MQTT 1883 / WS 9001, dashboard HTTP 5050.

```bash
cd cloud-admin && cp .env.example .env
# set TUNNEL_TOKEN, MQTT_NODE_PASS, MQTT_ADMIN_PASS
./gen-passwd.sh && ./run.sh up
```

## Documentation

- [`TESTING.md`](TESTING.md) — experiment topologies and multi-node flashing
- [`docs/evalulation/`](docs/evalulation/) — experiment telemetry CSVs, plots, and analysis scripts
- [`docs.md`](docs.md) — project proposal / requirements
- `paper/main.tex` — LaTeX report

## Development Journal

Distilled from git history — tracks major milestones, not every bugfix.

| Date | Milestone |
| --- | --- |
| 2026-02-11 | Project skeleton created |
| 2026-02-16 | Mesh brain: routing, discovery, transfer coordination (PR #1) |
| 2026-02-19 | End-to-end FLP pipeline — LoRa + BLE mesh, cloud-admin (Python/Flask), Pico W UART bridge, multi-exit exfiltration with zero-copy buffer pool (PRs #2–5) |
| 2026-02-21 | BLE/ARQ/MQTT throughput optimizations; first 3 MB multi-hop transfer |
| 2026-02-24 | **BLE replaced with ESP-NOW** — throughput and reliability leap. SSD1306 OLED driver replaced with nopnop2002 library. Relay sdkconfig, testing guide, cloud-admin RPi deploy scripts, watchdog fixes (PRs #6–8) |
| 2026-03-03 | Code style pass (Barr-C), ESP-NOW debugging |
| 2026-03-08 | Auto-demo mode. **Mesh routing overhaul** — DSDV sequencing, route errors, ETX metric, ADR, priority queues. LoRa control packets compressed (TransferAd 36→9 B). LaTeX paper started. License → Apache 2.0. 4-topology experiment guide (PRs #9–12) |
| 2026-03-09 | Unity test suite added; ESP32 best-practice fixes (PR #14) |
| 2026-03-10 | **Cloud-admin rewritten in Go** with Docker support; Python version removed. PSRAM and SD card init stabilized (PR #16) |
| 2026-03-17 | OLED: RELAY/GATEWAY mode indicator, live transfer progress, mesh crawl display. Persistent benchmark capture (NACK/PDR/latency). Multi-exit ARQ deadlock and multi-hop routing fixes (PRs #17–21) |
| 2026-03-25 | **MQTT over WSS via Cloudflare Tunnel** (mqtt.hoshinoht.dev). Broker auth (flp-node / flp-admin accounts). ESP-MESH-inspired routing improvements, EXIT_OFFLINE handling |
| 2026-03-26 | Dashboard Basic Auth. Election dedup, ARQ infinite-retry fix. Heap corruption fix (MQTT stack overflow on WSS). Buffer pool rewritten as lock-free Treiber stack. TransferEngine refactored — god function extracted into sub-functions |
| 2026-03-27 | **Multi-exit reassembly** bug fixes, quality-weighted exit scheduling. **OLED replaced with LVGL 9** on esp_lcd. Cloud-admin reorganized for experiment 2/3 (PR #22) |
| 2026-03-30 | Exponential backoff, shared send budget, deferred ACK queue, out-of-window retx queue. PSRAM utilization expanded (larger ARQ window, buffer pool, full file cache). Parity retransmit fixes (PR #23) |
| 2026-03-31 | Transfer relaying stabilized, SX1280 PA bring-up. 15-fix preventive maintenance sweep on TransferEngine. Buffer pool exhaustion fix. Gateway boot crash and MQTT TLS alloc fix. OLED PSRAM heap corruption fix. Multi-peer blocking (PR #24) |
| 2026-04-01 | Split control/data hop paths for relay. Fragment pacing and ARQ resilience for large files. Architectural robustness improvements |
| 2026-04-02 | Relay queue depth 16→48, SD cache capped at 512 KB. Dead code removal, magic number cleanup. Memory safety sweep. UART API overhaul, FlpClient extracted. NACK amplification loop fix. Configurable `FLP_MAX_ELECTED_EXITS`. LED control command. Intensive experiment 3 debugging — RSSI fix, NACK dedup, OOW retx stride routing, tx semaphore pacing |
| 2026-04-03 | Heavy tasks moved to CPU1 (Xtensa dual-core). Control/data semaphore split. MQTT publish throttle tuning. NACK exponential backoff (4/8/16/32) |
| 2026-04-04 | SD card buffer pool tuning (1 MB→512 KB), graceful PSRAM degradation, ARQ window 128→64. SD card LRU read cache. RTT/RTO tuning, reduced cloud-admin NACK rates. Telemetry capture for experiment metrics. Cloud-to-node messaging feature |

## Team

| Name | SIT ID |
| --- | --- |
| Po Haoting | 2401280 |
| Ong Tun Siang | 2402091 |
| Kenny Leck | 2403543 |
| Chia Wei Sheng | 2400953 |

## License

Copyright 2026 Forest Link Protocol Team. Licensed under the [Apache License, Version 2.0](LICENSE).
