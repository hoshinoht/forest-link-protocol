# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Forest Link Protocol v3.7** — A multi-protocol mesh networking solution for IoT sensor deployments in dense forest environments (the "Green Wall Effect"). Targets ESP32-S3 nodes with 8MB PSRAM. Course project for CSC2106 IoT Protocols and Networks.

Full specification is in `docs.md`.

## Architecture

Three communication layers with adaptive protocol selection:

1. **BLE (Short-Range Mesh):** High-bandwidth, short-range inter-node communication
2. **LoRa (Long-Range Mesh):** Extended range, lower bandwidth fallback
3. **WiFi + MQTT (External Gateway):** Opportunistic cloud connectivity via MQTT-SN

**Key design decisions:**
- Adaptive BLE/LoRa selection based on power budget, hop count, RSSI, and packet size
- Selective Repeat ARQ with sliding window for reliable transfer
- Single file transfer at a time through the mesh (FIFO queue for pending transfers)
- Hardware-accelerated AES-GCM encryption (ESP32-S3)
- O(1) memory-mapped fragment reassembly using PSRAM
- Interrupt-driven packet processing (NFR-MESH2 — no polling)
- All nodes run MQTT-SN client with synchronized topic lookup table

**Data flow (Forest → Cloud):** Source advertises intent via LoRa broadcast → exit node election → fragmented transfer via BLE or LoRa → exit node forwards to MQTT broker → cloud reconstructs fragments.

## Requirement Reference Format

Functional requirements use `[FR-MESH#]` and `[FR-MQTT#]` tags. Non-functional requirements use `[NFR-MESH#]`. See `docs.md` for the full list.

## Key Performance Targets

- Deliver 1MB file over 3+ hops within 20 minutes (NFR-MESH1)
- 3MB transfer across 3+ hops in deep forest conditions
- Broadcast retry max 3 attempts (FR-MESH5)
