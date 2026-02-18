# Forest Link Protocol (FLP) v3.7

A multi-protocol mesh networking solution for IoT sensor deployments in dense forest environments. Designed as a course project for **CSC2106 IoT Protocols and Networks** at SIT.

## Problem

Dense forest canopies (the "Green Wall Effect") severely attenuate 2.4 GHz WiFi signals, creating a **Bandwidth-Reach Paradox**: high-bandwidth protocols like WiFi cannot penetrate vegetation over long distances, while long-range protocols like LoRa lack the throughput to move large data (images, audio logs). Standard IoT deployments in these environments result in brittle, disconnected networks.

## What FLP Does

FLP bridges this gap with an **adaptive multi-protocol mesh** running on ESP32 nodes. It combines three communication layers:

| Layer | Protocol | Role |
|-------|----------|------|
| Short-range mesh | **BLE** | High-bandwidth inter-node data transfer (up to 504 bytes/packet) |
| Long-range mesh | **LoRa** (SX1276) | Extended-range fallback for discovery and small payloads (up to 247 bytes/packet) |
| Cloud gateway | **WiFi + MQTT-SN** | Opportunistic data exfiltration when internet is available |

### How It Works

1. A source node **advertises** its intent to transfer data via LoRa broadcast
2. Nodes with internet access respond, and an **exit node is elected**
3. The file is fragmented and sent hop-by-hop via BLE or LoRa (selected adaptively per-packet based on RSSI, hop count, payload size, and battery level)
4. A **Selective Repeat ARQ** sliding window ensures reliable delivery with local retransmission
5. The exit node forwards reassembled data to an **MQTT broker** over WiFi
6. The cloud admin reconstructs the original file

### Key Design Decisions

- **Adaptive protocol selection** with a hard-gate: payloads exceeding 247 bytes always use BLE (LoRa hardware limit is 255 bytes total)
- **8-byte compact packet header** to maximise payload capacity on LoRa's constrained MTU
- **O(1) memory-mapped fragment reassembly** using the ESP32-S3's 8MB PSRAM
- **Interrupt-driven packet processing** (no polling) for low-latency mesh routing
- **FreeRTOS priority tuning**: LoRa ISR handler at highest priority (20), mesh routing (15), MQTT (10), protocol selector (5)
- Single file transfer at a time through the mesh (FIFO queue for pending transfers)

### Performance Targets

- Deliver a **1 MB file** over 3+ hops within **20 minutes**
- Support **3 MB transfers** across 3+ hops in deep forest conditions
- Broadcast retry maximum of **3 attempts**

## Repository Structure

```
forest-link-protocol/
├── flp-node/                   # ESP-IDF firmware for ESP32 mesh nodes
│   ├── main/                   # Application entry point and config
│   │   ├── main.cpp            # FreeRTOS task creation and WiFi init
│   │   └── include/
│   │       └── flp_config.h    # Task priorities, stack sizes, mesh params
│   ├── components/
│   │   ├── ble/                # NimBLE transport layer
│   │   ├── lora/               # SX1276 SPI driver and LoRa transport
│   │   ├── mesh/               # Mesh manager, route table, packet routing
│   │   ├── protocol/           # Packet definitions, protocol selector, Selective Repeat ARQ
│   │   └── mqtt_sn/            # MQTT-SN client and topic table
│   ├── idf.sh                  # Build wrapper script (see below)
│   ├── partitions.csv          # Custom partition table (2.5 MB app + SPIFFS)
│   ├── sdkconfig.defaults      # Shared ESP-IDF Kconfig defaults
│   └── sdkconfig.defaults.esp32  # ESP32-specific defaults (LilyGo T3)
├── cloud-admin/                # Cloud-side MQTT admin / broker tooling
├── docs.md                     # Full project specification and requirements
└── CLAUDE.md                   # AI assistant context file
```

## Prerequisites

### ESP32 Firmware Development

- **Hardware**: ESP32 or ESP32-S3 board with SX1276 LoRa module (tested on [LilyGo T3](http://www.lilygo.cn/prod_view.aspx?TypeId=50060&Id=1130))
- **ESP-IDF v5.5+**: The Espressif IoT Development Framework
- **Python 3.8+**: Required by ESP-IDF toolchain
- **Git**: For cloning the repository

### VS Code (Required for Contributors)

This project enforces the [Barr-C:2018 Embedded C Coding Standard](https://barrgroup.com/embedded-systems/books/embedded-c-coding-standard) via `.clang-format` and VS Code settings. Install the following extensions:

| Extension | ID | Purpose |
|---|---|---|
| **clangd** | `llvm-vs-code-extensions.vscode-clangd` | C language server, format-on-save using `.clang-format` |
| **C/C++** | `ms-vscode.cpptools` | C++ language server, format-on-save using `.clang-format` |

The `.vscode/settings.json` committed to the repo enables format-on-save so all C/C++ files are automatically formatted to Barr-C style (Allman braces, 4-space indent, 80-column limit, pointer-right alignment). No manual setup needed — just install the extensions and open the workspace.

### Raspberry Pi Gateway

- **Raspberry Pi** (3B+ or newer) running Raspberry Pi OS (Bookworm) or Ubuntu 22.04+
- **Network access**: The Pi must be on the same WiFi network as the ESP32 gateway nodes

## Setup Guide

### 1. Install ESP-IDF

Follow the official [ESP-IDF Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html) for your platform. The short version:

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32    # or esp32s3 for S3 targets
```

This installs the toolchain to `~/.espressif/`. The install script only needs to run once.

### 2. Clone This Repository

```bash
git clone https://github.com/<your-org>/forest-link-protocol.git
cd forest-link-protocol
```

### 3. Configure the Node

Navigate to the firmware directory and open the configuration menu:

```bash
cd flp-node
bash idf.sh menuconfig
```

Under **Forest Link Protocol**, configure:
- **Node role** (0 = sensor, 1 = relay, 2 = gateway)
- **LoRa SPI pin mapping** (defaults match LilyGo T3)
- **WiFi SSID and password** for MQTT cloud connectivity
- **MQTT broker URI**

### 4. Build

```bash
bash idf.sh build
```

### 5. Flash and Monitor

Connect your ESP32 board via USB, then:

```bash
bash idf.sh flash monitor
```

Press `Ctrl+]` to exit the serial monitor.

## Using `idf.sh`

The `idf.sh` wrapper script sources the ESP-IDF environment (`~/esp/esp-idf/export.sh`) and forwards all arguments to `idf.py`. This means you don't need to manually source the export script in every terminal session.

```bash
bash idf.sh <command> [options]
```

Common commands:

| Command | Description |
|---------|-------------|
| `bash idf.sh build` | Compile the firmware |
| `bash idf.sh flash` | Flash to connected ESP32 |
| `bash idf.sh monitor` | Open serial monitor |
| `bash idf.sh flash monitor` | Flash then immediately monitor |
| `bash idf.sh menuconfig` | Open Kconfig configuration UI |
| `bash idf.sh fullclean` | Delete build directory and start fresh |
| `bash idf.sh size-components` | Show per-component binary size breakdown |

If your ESP-IDF is installed somewhere other than `~/esp/esp-idf`, edit the `source` path at the top of `flp-node/idf.sh`.

## Raspberry Pi Gateway Deployment

The `cloud-admin/deploy.sh` script automates the full gateway stack on a Raspberry Pi:

| Component | Description | Port |
|-----------|-------------|------|
| **Mosquitto** | MQTT broker | TCP 1883 |
| **MQTT-SN Gateway** | Eclipse Paho UDP→MQTT translator | UDP 1885 |
| **FLP MQTT Admin** | Python file transfer manager | connects to localhost:1883 |

### Quick Start

```bash
# On the Raspberry Pi:
cd cloud-admin
sudo bash deploy.sh --install
```

This will:
1. Install and configure Mosquitto (anonymous access, port 1883)
2. Clone, build, and install the Eclipse Paho MQTT-SN gateway (UDP 1885 → MQTT 1883)
3. Create a Python venv and install the FLP MQTT Admin dependencies
4. Register all three as systemd services (auto-start on boot)

### Managing Services

```bash
sudo bash deploy.sh --start      # Start all services
sudo bash deploy.sh --stop       # Stop all services
sudo bash deploy.sh --status     # Check service status
sudo bash deploy.sh --uninstall  # Remove everything
```

Or manage individually:

```bash
sudo systemctl {start|stop|status|restart} mosquitto
sudo systemctl {start|stop|status|restart} mqtt-sn-gateway
sudo systemctl {start|stop|status|restart} flp-mqtt-admin
```

### Viewing Logs

```bash
journalctl -u mosquitto -f
journalctl -u mqtt-sn-gateway -f
journalctl -u flp-mqtt-admin -f
```

Received files are saved to `cloud-admin/received_files/`.

## Team

| Name | SIT ID |
|------|--------|
| Po Haoting | 2401280 |
| Ong Tun Siang | 2402091 |
| Kenny Leck | 2403543 |
| Chia Wei Sheng | 2400953 |

## License

Course project for CSC2106 IoT Protocols and Networks, Singapore Institute of Technology.
