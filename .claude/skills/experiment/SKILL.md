---
name: experiment
description: Guide setup and execution of an FLP experiment topology (1-4)
---

# Experiment Skill

Guide the user through setting up and running one of the 4 FLP experiment topologies.

## Important Context

- The user may be a **new developer** unfamiliar with the FLP protocol, ESP-IDF, or ESP32 hardware.
- Their environment is **Windows x86** (not macOS). Adjust all paths, port names, and shell commands accordingly.
- They use **ESP-IDF v5.5.3** installed via the Espressif Installation Manager (EIM) or the legacy ESP-IDF Tools Installer.
- The `./idf` wrapper script is **bash-only (macOS/Linux)**. On Windows, use raw `idf.py` commands in the **ESP-IDF PowerShell** or **ESP-IDF CMD** terminal that EIM creates.
- The cloud backend is **already deployed and shared**. No self-hosting setup required.

## Activation

This skill activates when the user runs `/experiment` or asks about running FLP experiments.

## Arguments

- `/experiment` -- ask which experiment (1-4) and direction
- `/experiment 1` -- Experiment 1 (1:1 exit node direct)
- `/experiment 2` -- Experiment 2 (2:1 one relay)
- `/experiment 3` -- Experiment 3 (3:1 two relays)
- `/experiment 4` -- Experiment 4 (4:2 two exit nodes)
- `/experiment 3 downlink` -- only downlink for Experiment 3

## Steps

### Step 0: Read the source of truth

1. Read `TESTING.md` at the repo root for experiment topology diagrams, expected OLED output, serial log patterns, and the results template.
2. Read `flp-node/sdkconfig.defaults` for current WiFi/MQTT credentials.
3. Read `flp-node/sdkconfig.defaults.relay` for the relay overlay.

### Step 1: Confirm the environment

Ask the user to confirm:

1. **ESP-IDF installed?** They should have ESP-IDF v5.5.3. If using the EIM GUI, it creates an "ESP-IDF v5.5.3 PowerShell" shortcut. All `idf.py` commands must run inside this terminal.
2. **Board connected?** LilyGo T3-S3 V1.2 plugged in via USB-C. On Windows, the board appears as a COM port (e.g., `COM3`). Check in **Device Manager > Ports (COM & LPT)** -- look for "USB-SERIAL CH340" or "USB Serial Device".
3. **Working directory?** They must `cd` into the `flp-node` directory before running any `idf.py` commands.

If the user doesn't have ESP-IDF installed, walk them through:
```
# In PowerShell (admin)
winget install Espressif.EIM
# Then open EIM, select ESP-IDF v5.5.3, install with defaults
# After install, use the "ESP-IDF v5.5.3 PowerShell" shortcut
```

### Step 2: Flash the boards

**Critical: the user must flash boards one at a time, switching USB cables between boards.**

#### Exit node (WiFi + MQTT enabled)

The default `sdkconfig.defaults` already has the correct WiFi and MQTT credentials for the shared backend. No edits needed unless they're on a different WiFi network.

If they need to change WiFi credentials, tell them to edit `sdkconfig.defaults` lines:
```
CONFIG_FLP_WIFI_SSID="<their_ssid>"
CONFIG_FLP_WIFI_PASSWORD="<their_password>"
```

Flash commands (Windows, in ESP-IDF PowerShell):
```powershell
cd flp-node
Remove-Item sdkconfig -ErrorAction SilentlyContinue
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults" set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

Replace `COM3` with the actual COM port from Device Manager.

**Verify before proceeding:**
- Serial output shows: `Got IP: x.x.x.x` and `MQTT connected to broker`
- OLED shows: `W:OK` (WiFi connected)
- If OLED shows `W:--`, WiFi credentials are wrong or the network is unreachable

Press `Ctrl+]` to exit the monitor.

#### Relay / sensor node (WiFi disabled)

```powershell
Remove-Item sdkconfig -ErrorAction SilentlyContinue
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.relay" set-target esp32s3
idf.py build
idf.py -p COM4 flash monitor
```

The `-D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.relay"` flag layers the relay overlay which sets `CONFIG_FLP_WIFI_DISABLED=y`. The semicolon separates the base config from the overlay.

**Verify before proceeding:**
- Serial output shows: `WiFi STA started (no AP)` (this is correct -- relay nodes don't connect to WiFi)
- OLED shows: `W:--` (WiFi intentionally disabled)

**Important:** After flashing all relay nodes, if the next board needs to be an exit node, delete `sdkconfig` again and rebuild without the relay overlay:
```powershell
Remove-Item sdkconfig -ErrorAction SilentlyContinue
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults" set-target esp32s3
idf.py build
```

### Step 3: Ask which experiment

If not specified in the command arguments, ask the user:

1. Which experiment number (1-4)?
2. Which direction: uplink (sensor to cloud), downlink (cloud to sensor), or both?

Use the `mcp_question` tool to present the choices.

### Step 4: Walk through the experiment

Reference `TESTING.md` for the specific experiment. Guide the user through:

1. **Board roles** -- Clearly state which board is S (sensor), R (relay), X (exit), and how many of each.
2. **Physical arrangement** -- For Experiments 3 and 4, boards must be physically separated so multi-hop routing occurs. Explain that Board 3 (or 4) should be far enough from the exit node that it cannot directly reach it.
3. **Power-on order** -- Exit nodes first, then relays, then sensors. Wait ~15 seconds between each for mesh discovery.
4. **OLED verification** -- Before triggering a transfer, verify each board shows the expected OLED state:
   - Exit node: `GW:0h` (is the gateway, 0 hops to internet)
   - Relay 1 hop away: `GW:1h`
   - Sensor 2 hops away: `GW:2h`
   - If a board shows `GW:--`, discovery hasn't completed. Wait longer or move boards closer.
5. **Trigger** -- Auto-demo triggers every 60 seconds by default, or the user can press the BOOT button (GPIO 0) on the sensor board.
6. **What to observe** -- Walk through what they should see in:
   - Serial monitor (transfer start, fragment counts, ARQ retransmissions)
   - OLED (transfer progress percentage)
   - Cloud dashboard at `https://admin.hoshinoht.dev` (login: `admin` / the MQTT_ADMIN_PASS from sdkconfig.defaults)
7. **Downlink testing** -- For downlink, provide the exact `curl` command with the correct admin hostname and the node address from the OLED display. On Windows, `curl` is available in PowerShell natively. The node address on the OLED is `FLP-XXXX` where XXXX is hex; use it without the `0x` prefix in the API URL.

### Step 5: Record results

After the experiment completes, help the user fill in the results template from TESTING.md:

| Metric | Value |
|--------|-------|
| File size (bytes) | from serial log: "Demo payload: N bytes" |
| Total fragments | from serial log or dashboard |
| Transfer time (s) | from dashboard: started -> completed timestamps |
| Throughput (KB/s) | file_size / transfer_time / 1024 |
| Exit nodes used | 1 for Exp 1-3, 2 for Exp 4 |
| Hop count | 0/1/2/2 for Exp 1/2/3/4 |
| ARQ retransmissions | from serial log: count of "retransmit" lines |
| Fragments lost | from serial log: total_sent - total_received |
| Downlink RTT (ms) | time between curl send and telemetry appearing in dashboard |

### Step 6: Troubleshooting

If the user encounters issues, check the troubleshooting table in TESTING.md. Common Windows-specific issues:

| Symptom | Fix |
|---------|-----|
| `idf.py` not found | Not running in ESP-IDF terminal. Open "ESP-IDF v5.5.3 PowerShell" from Start Menu |
| COM port not found | Check Device Manager. Install CH340 driver if needed. Try a different USB cable |
| `Permission denied` on COM port | Close any other serial monitor (PuTTY, Arduino IDE) holding the port |
| Build fails with `sdkconfig` errors | Delete `sdkconfig` and rebuild: `Remove-Item sdkconfig` |
| Flash fails: "could not open port" | Another process has the port. Close serial monitors. Try unplugging and replugging |
| Monitor shows garbage characters | Baud rate mismatch. The default 115200 should work with `idf.py monitor` |
| `SDKCONFIG_DEFAULTS` with semicolon fails | On CMD (not PowerShell), use quotes: `idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.relay"` |

## Shared Backend Info

Pre-deployed and shared. The user does **not** need to run `cloud-admin/` themselves.

- **Admin dashboard**: `https://admin.hoshinoht.dev`
- **MQTT broker** (for nodes): `wss://mqtt.hoshinoht.dev`
- **MQTT credentials** (already in sdkconfig.defaults):
  - Username: `flp-node`
  - Password: `U82mWIrDmQCzbCPMDxcB4q3fBPiO2bZH`
- **Dashboard login**: `admin` / same password as `MQTT_ADMIN_PASS` in `cloud-admin/.env`
- **API base URL for downlink commands**: `https://admin.hoshinoht.dev/api/cmd/<node_addr>`

## Key Protocol Concepts (for new developers)

Briefly explain these if the user seems confused:

- **ESP-NOW**: Short-range (~200m) peer-to-peer WiFi protocol used for mesh data transfer between nodes. No access point needed.
- **LoRa (SX1280)**: Long-range radio at 2.4 GHz used for mesh discovery beacons and fallback data. Lower throughput but much longer range.
- **Exit node**: A node with WiFi connectivity that bridges the mesh to the cloud via MQTT over WebSocket (WSS).
- **Relay node**: Forwards packets between nodes without WiFi. Acts as a mesh hop.
- **ARQ (Automatic Repeat reQuest)**: The protocol's reliability layer. Fragments are acknowledged; lost ones are retransmitted. "Retransmit" lines in the serial log are normal and expected.
- **Transfer flow**: Sensor advertises a file -> exit node(s) respond -> sensor sends fragments hop-by-hop -> exit node publishes each fragment to MQTT -> cloud backend reassembles the file.
- **OLED shorthand**: `W:OK` = WiFi connected, `W:--` = WiFi disabled, `GW:Nh` = N hops to nearest gateway, `Nbrs:N` = N mesh neighbors discovered.
