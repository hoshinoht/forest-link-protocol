# Forest Link Protocol (FLP) v3.7

A multi-protocol mesh networking solution for IoT sensor deployments in dense forest environments. Designed as a course project for **CSC2106 IoT Protocols and Networks** at Singapore Institute of Technology.

## Problem

Dense forest canopies (the "Green Wall Effect") severely attenuate 2.4 GHz WiFi signals, creating a **Bandwidth-Reach Paradox**: high-bandwidth protocols like WiFi cannot penetrate vegetation over long distances, while long-range protocols like LoRa lack the throughput to move large data (images, audio logs). Standard IoT deployments in these environments result in brittle, disconnected networks.

## What FLP Does

FLP bridges this gap with an **adaptive multi-protocol mesh** running on commodity ESP32-S3 nodes. It combines three communication layers:

| Layer            | Protocol            | Role                                                                 |
| ---------------- | ------------------- | -------------------------------------------------------------------- |
| Short-range mesh | **ESP-NOW**         | High-bandwidth inter-node data relay (~34 KB/s per hop, 242 B MTU)   |
| Long-range mesh  | **LoRa** (SX1276)   | Extended-range control signaling, discovery, route ads (247 B MTU)   |
| Cloud backend    | **WiFi + MQTT**     | Opportunistic data exfiltration to Mosquitto + cloud-admin when internet is available |

### How It Works

1. A source node **advertises** its intent to transfer data via LoRa broadcast (`TRANSFER_AD`, 9 bytes)
2. Nodes with internet access respond, and up to **4 exit nodes are elected**
3. The file is fragmented into 240-byte chunks and sent hop-by-hop via ESP-NOW or LoRa (selected adaptively per-packet based on RSSI, hop count, and payload size)
4. A **Selective Repeat ARQ** (window=32) with **FEC parity** (7+1) ensures reliable delivery with local retransmission
5. Exit nodes forward fragments to the **MQTT broker** over WiFi
6. The cloud-side MQTT Admin reconstructs the original file using session ID correlation and CRC verification

### Key Design Decisions

- **DSDV sequence-numbered routing** with ETX-weighted composite cost to prevent count-to-infinity loops and avoid lossy links
- **Route error propagation** via `ROUTE_ERROR` packets for fast failure detection (~15 s vs 30 s passive timeout)
- **Dual-priority packet queues**: control packets (ACK, NACK, route updates) processed before bulk data fragments
- **Adaptive LoRa spreading factor** (SF7-SF10) inspired by LoRaWAN ADR, adjusting every 10 s based on link quality
- **Compact packet header** (8 bytes) to maximize payload on LoRa's constrained MTU
- **O(1) memory-mapped fragment reassembly** using ESP32-S3's 8 MB PSRAM
- **Interrupt-driven packet processing** (no polling) for low-latency mesh routing
- **Filename-in-fragment-0**: transfer metadata is deferred until the first data fragment arrives, keeping `TRANSFER_AD` at 9 bytes
- Single file transfer at a time through the mesh (FIFO queue for pending transfers)

### Performance

| Condition                        | 3 MB / 3 hops (4 exits) |
| -------------------------------- | ------------------------ |
| Lab (clean RF)                   | ~50 s                    |
| Dense forest (full canopy, dry)  | ~85 s                    |
| Worst case (heavy rain)          | ~110 s                   |

All scenarios meet NFR-MESH1 (1 MB in <20 minutes) with significant margin.

## Repository Structure

```
forest-link-protocol/
├── flp-node/                     # ESP-IDF firmware for ESP32-S3 mesh nodes
│   ├── main/                     # Application entry point, Kconfig, demo tasks
│   │   ├── main.cpp
│   │   ├── Kconfig.projbuild
│   │   └── include/
│   │       └── flp_config.h      # Task priorities, stack sizes, mesh params
│   ├── components/
│   │   ├── espnow/               # ESP-NOW transport layer
│   │   ├── lora/                 # SX1276 SPI driver and LoRa transport
│   │   ├── mesh/                 # MeshManager, TransferEngine, route table
│   │   ├── protocol/             # Packet definitions, Selective Repeat ARQ, FEC codec
│   │   ├── mqtt_client/          # MQTT client and topic table
│   │   ├── display/              # SSD1306 OLED driver
│   │   ├── uart_api/             # UART file ingest
│   │   ├── sdcard/               # SD card (TF slot) file reader via SPI
│   │   └── diagnostics/          # Runtime diagnostics and heap monitor
│   ├── sdkconfig.defaults        # Shared ESP-IDF Kconfig defaults
│   └── CMakeLists.txt
├── cloud-admin/                  # Cloud-side MQTT admin (fragment reassembly)
├── paper/                        # LaTeX design review report (IEEE format)
│   └── main.tex
├── docs/                         # Performance analysis documents
│   ├── performance-lab.md
│   └── performance-forest.md
├── simulator/                    # Protocol simulator
├── docs.md                       # Full project specification and requirements
├── CLAUDE.md                     # AI assistant context file
└── LICENSE                       # Apache License 2.0
```

## Prerequisites

- **Hardware**: LILYGO T3-S3 (ESP32-S3 + SX1276 LoRa), 8 MB PSRAM
- **ESP-IDF v5.5+**: Espressif IoT Development Framework
- **Python 3.8+**: Required by ESP-IDF toolchain

## Quick Start

### 1. Install ESP-IDF

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.3 --recursive https://github.com/espressif/esp-idf.git esp-idf-v5.5.3
cd esp-idf-v5.5.3
./install.sh esp32s3
```

### 2. Build and Flash

```bash
cd flp-node
source ~/esp/esp-idf-v5.5.3/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### 3. Configure

```bash
idf.py menuconfig
```

Under **Forest Link Protocol**, configure:
- WiFi SSID and password for MQTT cloud connectivity
- MQTT broker URI
- Demo mode (button trigger or auto-demo every 30 s)
- GPIO pin assignments
- OLED display enable/disable

After changing `sdkconfig.defaults`, run `idf.py fullclean && idf.py build`.

## SPI Bus Assignments

| Bus       | Device   | Pins                          |
| --------- | -------- | ----------------------------- |
| SPI2_HOST | SD card  | CS=13, MOSI=11, SCK=14, MISO=2 |
| SPI3_HOST | LoRa     | CS=7, MOSI=6, SCK=5, MISO=3    |

## Cloud Backend

The `cloud-admin/` directory contains the cloud-side backend for FLP: a Mosquitto broker, the Go-based FLP admin dashboard/API, and Cloudflare Tunnel ingress for remote access.

| Component           | Description                        | Port     |
| ------------------- | ---------------------------------- | -------- |
| Mosquitto           | MQTT broker                        | TCP 1883 / WS 9001 |
| FLP Admin           | Fragment reassembly, metrics, API  | HTTP 5050 |
| Cloudflared         | Public tunnel ingress              | Hostname-mapped |

Typical workflow from `cloud-admin/`:

```bash
./gen-passwd.sh
./run.sh up
```

This exposes the dashboard locally at `http://localhost:5050` and, if configured, through the hostnames defined in `cloudflared-config.yml`.

## Paper

The `paper/` directory contains the LaTeX design review report in IEEE conference format. Build with:

```bash
cd paper
pdflatex main.tex
```

## Team

| Name           | SIT ID  |
| -------------- | ------- |
| Po Haoting     | 2401280 |
| Ong Tun Siang  | 2402091 |
| Kenny Leck     | 2403543 |
| Chia Wei Sheng | 2400953 |

## License

Copyright 2026 Forest Link Protocol Team. Licensed under the [Apache License, Version 2.0](LICENSE).
