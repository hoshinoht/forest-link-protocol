# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Forest Link Protocol v3.7** — A multi-protocol mesh networking solution for IoT sensor deployments in dense forest environments (the "Green Wall Effect"). Targets ESP32-S3 nodes with 8MB PSRAM. Course project for CSC2106 IoT Protocols and Networks.

Full specification is in `docs.md`.

## Build & Flash

```bash
source ~/esp/esp-idf-v5.5.3/export.sh   # Required before any idf.py command
cd flp-node
idf.py build                             # Build firmware
idf.py -p /dev/ttyACM0 flash monitor    # Flash and open serial monitor
idf.py menuconfig                        # Configure Kconfig options
```

- ESP-IDF v5.5.3, target: esp32s3
- Board config in `flp-node/sdkconfig.defaults`
- Custom Kconfig options in `flp-node/main/Kconfig.projbuild`
- After changing `sdkconfig.defaults`, run `idf.py fullclean && idf.py build` to regenerate sdkconfig

**Flashing relay/sensor nodes** (WiFi disabled):
```bash
cp sdkconfig.defaults.relay sdkconfig.defaults
rm -f sdkconfig && idf.py build && idf.py -p /dev/<port> flash monitor
git checkout sdkconfig.defaults   # Restore after flashing relays
```

**Exit node WiFi config** — set in `sdkconfig.defaults` before building:
```
CONFIG_FLP_WIFI_SSID="<ssid>"
CONFIG_FLP_WIFI_PASSWORD="<password>"
CONFIG_FLP_MQTT_BROKER_URI="mqtt://<broker_ip>"
```

### Cloud Admin

```bash
cd cloud-admin
pip install -r requirements.txt   # paho-mqtt>=2.0, flask>=3.0
python mqtt_admin.py              # Start MQTT admin (fragment reassembly + web UI)
```

## Repository Structure

- `flp-node/` — ESP32 firmware (ESP-IDF project)
  - `components/espnow/` — ESP-NOW transport
  - `components/lora/` — LoRa (SX1280) transport
  - `components/mesh/` — MeshManager, TransferEngine, route table
  - `components/protocol/` — SelectiveRepeat ARQ, FEC codec, packet types
  - `components/mqtt_client/` — MQTT-SN client, cloud NACK handling
  - `components/uart_api/` — UART file ingest
  - `components/display/` — SSD1306 OLED driver
  - `components/diagnostics/` — Runtime diagnostics
  - `components/sdcard/` — SD card (TF slot) file reader via SPI
  - `main/` — app_main, Kconfig, demo button/auto-demo task
- `cloud-admin/` — MQTT Admin (cloud-side reassembly, Flask web UI)
- `simulator/` — Protocol simulator
- `paper/` — IEEE conference paper (LaTeX)
- `docs.md` — Full protocol specification
- `docs/performance-lab.md` — Lab throughput analysis
- `docs/performance-forest.md` — Forest propagation and range projections
- `TESTING.md` — Experiment guide (4 topologies, bidirectional tests)

## Architecture

Three communication layers with adaptive protocol selection:

1. **ESP-NOW (Short-Range Mesh):** High-bandwidth, short-range inter-node communication
2. **LoRa (Long-Range Mesh):** Extended range, lower bandwidth fallback
3. **WiFi + MQTT (External Gateway):** Opportunistic cloud connectivity via MQTT-SN

**Key design decisions:**
- Adaptive ESP-NOW/LoRa selection based on power budget, hop count, RSSI, and packet size
- Selective Repeat ARQ with sliding window for reliable transfer
- Single file transfer at a time through the mesh (FIFO queue for pending transfers)
- Hardware-accelerated AES-GCM encryption (ESP32-S3)
- O(1) memory-mapped fragment reassembly using PSRAM
- Interrupt-driven packet processing (NFR-MESH2 — no polling)
- All nodes run MQTT-SN client with synchronized topic lookup table

**Data flow (Forest → Cloud):** Source advertises intent via LoRa broadcast → exit node election → fragmented transfer via ESP-NOW or LoRa → exit node forwards to MQTT broker → cloud reconstructs fragments.

## Requirement Reference Format

Functional requirements use `[FR-MESH#]` and `[FR-MQTT#]` tags. Non-functional requirements use `[NFR-MESH#]`. See `docs.md` for the full list.

## SPI Bus Assignments

- SPI2_HOST: SD card (CS=13, MOSI=11, SCK=14, MISO=2)
- SPI3_HOST: LoRa SX1280 (CS=7, MOSI=6, SCK=5, MISO=3)

## Hardware

- Board: LILYGO T3-S3 (ESP32-S3 + SX1280 LoRa)
- PSRAM: 8MB octal, enabled via `CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_MODE_OCT=y`
- SD/TF card slot on SPI2 (no conflict with LoRa on SPI3)
- GPIO 0 is the BOOT button; avoid using it for application logic

## Component Dependency Notes

- `flp_config.h` lives in `main/include/` — components needing it must `REQUIRES main` (e.g. `display`)
- Avoid circular deps: if `main` depends on a component, that component cannot `REQUIRES main`. Use local constants instead.

## Demo Modes

- **Button mode** (default): Press GPIO 1 to trigger a file transfer
- **Auto demo mode**: Set `CONFIG_FLP_DEMO_AUTO=y` in menuconfig; transfers every 30s without hardware

## Key Performance Targets

- Deliver 1MB file over 3+ hops within 20 minutes (NFR-MESH1)
- 3MB transfer across 3+ hops in deep forest conditions
- Broadcast retry max 3 attempts (FR-MESH5)

## Agent Usage

| Task | Agent | Notes |
|------|-------|-------|
| Firmware development | `esp32-tdd-coder` | TDD-enforced; won't write code without tests |
| Test generation from design docs | `esp32-tdd-test-writer` | Generates Unity test suites |
| IEEE paper, reports | `document-writer` | LaTeX/pandoc document editing |
| Code with unfamiliar ESP-IDF APIs | `docs-first-coder` | Researches docs before coding |
| Post-implementation review | `code-checker` | Launch proactively after code changes |
| General feature work, bug fixes | `builder-prompt` | Primary coding agent |
| File/pattern search | `explore` | Read-only codebase navigation |

**Skills**: `/flash` (build + flash), `/experiment` (guided topology test)
