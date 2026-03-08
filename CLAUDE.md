# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Forest Link Protocol v3.7** — A multi-protocol mesh networking solution for IoT sensor deployments in dense forest environments (the "Green Wall Effect"). Targets ESP32-S3 nodes with 8MB PSRAM. Course project for CSC2106 IoT Protocols and Networks.

Full specification is in `docs.md`.

## Build & Flash

```bash
cd flp-node
idf.py build              # Build firmware
idf.py -p /dev/ttyACM0 flash monitor  # Flash and open serial monitor
idf.py menuconfig         # Configure Kconfig options (GPIO pins, demo mode, OLED, WiFi)
```

- ESP-IDF v5.5.2, target: esp32s3
- Board config in `flp-node/sdkconfig.defaults`
- Custom Kconfig options in `flp-node/main/Kconfig.projbuild`

## Repository Structure

- `flp-node/` — ESP32 firmware (ESP-IDF project)
  - `components/espnow/` — ESP-NOW transport
  - `components/lora/` — LoRa (SX1276) transport
  - `components/mesh/` — MeshManager, TransferEngine, route table
  - `components/protocol/` — SelectiveRepeat ARQ, FEC codec, packet types
  - `components/mqtt_client/` — MQTT-SN client, cloud NACK handling
  - `components/uart_api/` — UART file ingest
  - `components/display/` — SSD1306 OLED driver
  - `components/diagnostics/` — Runtime diagnostics
  - `main/` — app_main, Kconfig, demo button/auto-demo task
- `cloud-admin/` — MQTT Admin (cloud-side reassembly)
- `simulator/` — Protocol simulator
- `docs.md` — Full protocol specification

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

## Hardware

- Board: LILYGO T3-S3 (ESP32-S3 + SX1276 LoRa)
- GPIO 0 is the BOOT button; avoid using it for application logic

## Demo Modes

- **Button mode** (default): Press GPIO 1 to trigger a file transfer
- **Auto demo mode**: Set `CONFIG_FLP_DEMO_AUTO=y` in menuconfig; transfers every 30s without hardware

## Key Performance Targets

- Deliver 1MB file over 3+ hops within 20 minutes (NFR-MESH1)
- 3MB transfer across 3+ hops in deep forest conditions
- Broadcast retry max 3 attempts (FR-MESH5)
