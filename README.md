# Forest Link Protocol (FLP)

Hybrid ESP-NOW + LoRa mesh networking for IoT sensor deployments in dense forest environments. Built as a course project for **CSC2106 IoT Protocols and Networks** at the Singapore Institute of Technology.

## Problem

Dense forest canopy creates a "Green Wall" effect that heavily attenuates 2.4 GHz wireless links. This creates a **Bandwidth-Reach Paradox**:

- high-bandwidth links such as WiFi / ESP-NOW lose reliability over distance and foliage
- long-range links such as LoRa extend reach, but do not have enough throughput for large files on their own

FLP addresses that gap with a hybrid mesh that separates the bulk data path from the longer-range control path.

## What FLP Does

| Layer | Transport | Current role |
| --- | --- | --- |
| Short-range data plane | **ESP-NOW v2** | Bulk fragment relay and most unicast forwarding, using the current large-MTU mesh packet path |
| Longer-range control plane | **LoRa (SX1280, 2.4 GHz)** | Discovery, route advertisements, exit-node election, transfer coordination, and small packets that fit the LoRa payload limit |
| Cloud uplink | **WiFi + MQTT** | Exit nodes publish received fragments to Mosquitto + `cloud-admin` when internet is available |

### How It Works

1. A source node broadcasts a `TRANSFER_AD` over LoRa to discover exit nodes.
2. Candidate exits reply, and up to **4 exit nodes** can be elected.
3. The file is split into **1456-byte aligned fragments** for mesh transfer.
4. Bulk fragments are forwarded over **ESP-NOW relay paths**; LoRa remains the control plane and is only used for packets small enough to fit its payload limit.
5. **Selective Repeat ARQ** with **window=64** and **FEC parity (7+1)** handles loss and local retransmission.
6. Exit nodes forward fragments to the MQTT broker, and `cloud-admin` reassembles and verifies the file with **CRC32**.

### Current Design Notes

- DSDV-style routing with ETX / queue-aware path cost
- `ROUTE_ERROR` propagation for faster failure handling
- 8-byte packet header
- Large-MTU mesh packets: **1470-byte max packet**, **1462-byte ESP-NOW payload**, **1456-byte default transfer fragment size**
- LoRa-only neighbors are penalized for bulk routing because large transfer fragments do not fit on LoRa
- Adaptive LoRa spreading factor (**SF7-SF10**)
- PSRAM-backed ARQ, buffering, and reassembly on the ESP32-S3 node
- Single active file transfer through the mesh at a time

## Hardware Target

The current firmware, default pins, and LoRa driver target the **LILYGO T3-S3 v1.2 SX1280 variant**:

- **MCU:** ESP32S3FH4R2
- **Flash:** 4 MB
- **PSRAM:** 2 MB
- **LoRa radio:** SX1280 (2.4 GHz)
- **Display:** 0.96" SSD1306 OLED
- **Storage:** TF / microSD slot

This matters because the T3-S3 product line is sold in multiple radio variants; this repo is configured for the **SX1280** board, not the older SX1276 / SX1262 variants.

## Repository Structure

```text
forest-link-protocol/
├── flp-node/                     # ESP-IDF firmware for FLP nodes
│   ├── main/                     # Entry point, Kconfig, project glue
│   ├── components/
│   │   ├── diagnostics/          # Runtime diagnostics
│   │   ├── display/              # SSD1306 OLED UI
│   │   ├── espnow/               # ESP-NOW transport
│   │   ├── lora/                 # SX1280 LoRa transport
│   │   ├── mesh/                 # Routing, discovery, transfer engine
│   │   ├── mqtt_client/          # MQTT uplink client
│   │   ├── protocol/             # Packet formats, ARQ, FEC, buffer pool
│   │   ├── sdcard/               # SD card reader / cache
│   │   └── uart_api/             # UART ingest path
│   ├── sdkconfig.defaults        # Base ESP32-S3 defaults
│   ├── sdkconfig.defaults.relay  # Relay-only overlay
│   └── CMakeLists.txt
├── cloud-admin/                  # Mosquitto + Go dashboard/API + tunnel config
├── docs/                         # Performance analysis and deployment notes
├── paper/                        # LaTeX report
├── demo-data/                    # Demo input files
├── pico-w/                       # Pico W companion experiments / wiring notes
├── TESTING.md                    # Experiment and topology guide
├── docs.md                       # Project proposal / requirements
├── monitor.sh                    # Serial monitor helper
└── LICENSE
```

## Prerequisites

- **Hardware:** LILYGO T3-S3 v1.2 with **SX1280** LoRa radio
- **ESP-IDF:** **v5.5.x**
- **Python:** 3.8+
- **Docker / Docker Compose:** required if you want to run `cloud-admin/` locally

## Quick Start

### 1. Install ESP-IDF

Use any current ESP-IDF **v5.5.x** install. Example:

```bash
mkdir -p ~/esp
git clone -b v5.5.3 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v5.5.3
~/esp/esp-idf-v5.5.3/install.sh esp32s3
source ~/esp/esp-idf-v5.5.3/export.sh
```

### 2. Build and Flash an FLP Node

```bash
cd flp-node
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/tty.usbmodemXXXX flash monitor
```

### 3. Configure the Node

```bash
idf.py menuconfig
```

Under **Forest Link Protocol**, configure:

- WiFi SSID and password for exit nodes
- MQTT broker URI / username / password
- auto-demo vs button-triggered mode
- GPIO assignments
- OLED and SD card options

For a **relay-only node**, use the relay overlay instead of editing the base defaults:

```bash
rm -f sdkconfig
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.relay" set-target esp32s3
idf.py build
```

For multi-node flashing workflows and test topologies, see [`TESTING.md`](TESTING.md).

## Default Pinout (LILYGO T3-S3 v1.2 SX1280)

### LoRa (SPI3)

| Signal | GPIO |
| --- | --- |
| MOSI | 6 |
| MISO | 3 |
| SCK | 5 |
| CS | 7 |
| RST | 8 |
| DIO1 | 9 |
| BUSY | 36 |

### SD Card (SPI2)

| Signal | GPIO |
| --- | --- |
| MOSI | 11 |
| MISO | 2 |
| SCK | 14 |
| CS | 13 |

### OLED / Demo / UART Defaults

| Function | GPIO |
| --- | --- |
| OLED SDA | 18 |
| OLED SCL | 17 |
| Demo button | 1 |
| UART TX | 43 |
| UART RX | 44 |

## Cloud Backend

`cloud-admin/` contains the cloud-side backend:

- Mosquitto broker
- Go-based FLP admin dashboard / API
- optional Cloudflare Tunnel ingress

Typical local setup:

```bash
cd cloud-admin
cp .env.example .env
# edit .env and set TUNNEL_TOKEN, MQTT_NODE_PASS, MQTT_ADMIN_PASS
./gen-passwd.sh
./run.sh up
```

Services:

| Component | Description | Port |
| --- | --- | --- |
| Mosquitto | MQTT broker | TCP 1883 / WS 9001 |
| FLP Admin | Fragment reassembly, metrics, API, dashboard | HTTP 5050 |
| Cloudflared | Optional public tunnel ingress | Hostname-mapped |

Local dashboard: `http://localhost:5050`

## Performance References

Current performance notes live in:

- [`docs/performance-lab.md`](docs/performance-lab.md)
- [`docs/performance-forest.md`](docs/performance-forest.md)

Those documents describe the current modeled / measured behavior of the post-optimization mesh, including:

- effective ESP-NOW relay throughput in lab conditions
- projected dense-forest transfer times
- network diameter and range trade-offs
- multi-exit scaling behavior

## Other Documentation

- [`TESTING.md`](TESTING.md) — experiment setups and flashing workflows
- [`docs.md`](docs.md) — proposal / requirements document
- `paper/main.tex` — report source
- `pico-w/WIRING.md` — Pico W companion notes

## Team

| Name | SIT ID |
| --- | --- |
| Po Haoting | 2401280 |
| Ong Tun Siang | 2402091 |
| Kenny Leck | 2403543 |
| Chia Wei Sheng | 2400953 |

## License

Copyright 2026 Forest Link Protocol Team. Licensed under the [Apache License, Version 2.0](LICENSE).
