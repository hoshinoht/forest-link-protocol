# FLP Experiment Guide

Hardware: LilyGo T3-S3 V1.2 (ESP32-S3 + SX1280) nodes + Raspberry Pi (MQTT broker)

## Prerequisites

### Raspberry Pi (MQTT Broker)

```bash
sudo apt update && sudo apt install -y mosquitto mosquitto-clients
sudo systemctl enable mosquitto && sudo systemctl start mosquitto
```

Note the Pi's IP: `hostname -I`

Open two terminals on the Pi:

```bash
# Terminal 1: monitor all MQTT traffic
mosquitto_sub -v -t '#'

# Terminal 2: for sending commands (used in reverse tests)
# (commands given per experiment below)
```

### Flashing Nodes

All commands from `flp-node/`. Source ESP-IDF first:

```bash
source ~/esp/esp-idf-v5.5.3/export.sh
```

**Exit node (WiFi enabled):**

Set WiFi and broker in `sdkconfig.defaults`:
```
CONFIG_FLP_WIFI_SSID="<your_ssid>"
CONFIG_FLP_WIFI_PASSWORD="<your_password>"
CONFIG_FLP_MQTT_BROKER_URI="mqtt://<pi_ip>"
```

```bash
rm -f sdkconfig && idf.py build && idf.py -p /dev/<port> flash monitor
```

Confirm: OLED shows `W:OK`, serial shows `Got IP: x.x.x.x`

**Relay / sensor node (WiFi disabled):**

```bash
rm -f sdkconfig && idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.relay" set-target esp32s3 && idf.py build && idf.py -p /dev/<port> flash monitor
```

The `-D SDKCONFIG_DEFAULTS=...` flag layers `sdkconfig.defaults.relay` on top of the base defaults, setting `CONFIG_FLP_WIFI_DISABLED=y`. No need to modify or restore `sdkconfig.defaults`.

Confirm: OLED shows `W:--`, serial shows `WiFi STA started (no AP)`

> **Note:** After flashing relay nodes, switch back to gateway mode for the next exit node build:
> ```bash
> rm -f sdkconfig && idf.py set-target esp32s3 && idf.py build
> ```

**Auto-demo mode** (no button press needed):

```bash
idf.py menuconfig
# Forest Link Protocol -> Auto demo mode = y
# Forest Link Protocol -> Auto demo interval = 30
```

---

## Topology Labels

Each experiment is labelled `N:E` where N = total ESP32 nodes, E = exit nodes.

Node roles:
- **S** = Sensor/source (WiFi disabled, initiates file transfer)
- **R** = Relay (WiFi disabled, forwards packets)
- **X** = Exit node (WiFi enabled, bridges mesh to MQTT)
- **Pi** = Raspberry Pi MQTT broker

---

## Experiment 1: Exit Node Direct (1:1)

### Uplink (Forest to Cloud)

```
[X] --WiFi--> [Pi]
```

One exit node sends a file directly to the cloud. No mesh hops. Baseline measurement.

**Setup:**
1. Flash 1 board as exit node (WiFi enabled, auto-demo or button)
2. Power on Pi + exit node
3. Wait for `W:OK` on OLED

**Trigger:** Press BOOT button (GPIO 1) or wait for auto-demo

**Observe on Pi (`mosquitto_sub`):**
- `flp/<addr>/file/meta` with transfer metadata (session_id, filename, size, chunks)
- `flp/<addr>/file/data` with fragment payloads
- All fragments arrive (seq 0 through N)

**Expected OLED:** `GW:0h` (is the gateway), transfer progress 0-100%

**Record:**
- Transfer time (start to last fragment received)
- Total fragments sent vs received
- Any retransmissions in serial log

### Downlink (Cloud to Forest)

```
[Pi] --MQTT--> [X]
```

Cloud sends a command to the exit node via MQTT.

**Trigger (on Pi Terminal 2):**

```bash
# Send a telemetry request to the exit node
# Topic: flp/admin/cmd (all exit nodes subscribe to this)
# Payload: [target_addr_le16][cmd_id][data...]
# For local command (target = self), use the exit node's own address
python3 -c "
import struct, sys
target = 0x<exit_addr>  # the exit node's own address
sys.stdout.buffer.write(struct.pack('<HB', target, 0x01))
" | mosquitto_pub -t "flp/admin/cmd" -s
```

**Observe:** Exit node serial log shows `MESH_CMD from 0x0000: cmd=1` (REQUEST_TELEMETRY). Node responds with telemetry on `flp/<addr>/metrics`.

---

## Experiment 2: One Relay (2:1)

### Uplink

```
[S] --ESP-NOW/LoRa--> [X] --WiFi--> [Pi]
```

**Setup:**
1. Flash Board 1 as exit node (WiFi enabled)
2. Flash Board 2 as sensor node (WiFi disabled, auto-demo or button)
3. Power on Pi, then Board 1, then Board 2
4. Wait ~15 s for mesh discovery

**Verify discovery:**
- Board 1 OLED: `Nbrs:1 GW:0h`
- Board 2 OLED: `Nbrs:1 GW:1h`

**Trigger:** Press BOOT on Board 2

**Observe:**
- Board 2 serial: `Starting file transfer`, `Transfer ad from`, exit node election
- Board 1 serial: `Exit node mode`, fragment forwarding to MQTT
- Pi `mosquitto_sub`: meta + data fragments

**Record:**
- Transfer time
- Hop count shown in serial logs
- ARQ retransmission count

### Downlink

```
[Pi] --MQTT--> [X] --ESP-NOW/LoRa--> [S]
```

**Trigger (on Pi):**

Option A — via `mosquitto_pub` directly:
```bash
# All exit nodes subscribe to flp/admin/cmd
# Payload: [target_addr:2LE][cmd_id:1]
python3 -c "
import struct, sys
target = 0x<board2_addr>  # e.g. 0x1A3F (check OLED 'FLP-XXXX')
sys.stdout.buffer.write(struct.pack('<HB', target, 0x01))
" | mosquitto_pub -t "flp/admin/cmd" -s
```

Option B — via cloud-admin API (if running on Pi):
```bash
# POST /api/cmd/<target_node_id> with cmd ID
curl -X POST http://<pi_ip>:8080/api/cmd/<board2_addr> \
  -H 'Content-Type: application/json' \
  -d '{"cmd": 1}'
```

**Observe:**
- Board 1 serial: `Routed MESH_CMD to 0x<board2_addr>`
- Board 2 serial: `MESH_CMD from 0x0000: cmd=1`
- Board 2 responds with telemetry, relayed back through Board 1 to MQTT

---

## Experiment 3: Two Relays (3:1)

### Uplink

```
[S] --ESP-NOW/LoRa--> [R] --ESP-NOW/LoRa--> [X] --WiFi--> [Pi]
```

**Setup:**
1. Flash Board 1 as exit node
2. Flash Boards 2 and 3 as relay/sensor nodes (WiFi disabled)
3. **Physical arrangement:** Place Board 3 far from Board 1 with Board 2 in between. Ideally Board 3 cannot directly reach Board 1 (use walls/distance to force 2-hop path)
4. Power on all nodes, wait ~15 s

**Verify discovery:**
- Board 1: `GW:0h`
- Board 2: `GW:1h`
- Board 3: `GW:2h`

**Trigger:** Press BOOT on Board 3

**Observe:**
- Board 3 serial: broadcasts `TRANSFER_AD`, election starts
- Board 1 serial: responds as exit node, `Exit node mode`
- Transfer flows: Board 3 -> Board 2 (relay) -> Board 1 (exit) -> Pi
- Pi receives all fragments

**Record:**
- Transfer time (compare to Experiment 2)
- Board 2 serial: forwarding logs (`fwd 0x%04X -> 0x%04X`)
- Retransmission count per hop

### Downlink

```
[Pi] --MQTT--> [X] --ESP-NOW/LoRa--> [R] --ESP-NOW/LoRa--> [S]
```

**Trigger (on Pi):**

Option A — via `mosquitto_pub`:
```bash
python3 -c "
import struct, sys
target = 0x<board3_addr>  # the sensor node
sys.stdout.buffer.write(struct.pack('<HB', target, 0x01))
" | mosquitto_pub -t "flp/admin/cmd" -s
```

Option B — via cloud-admin API:
```bash
curl -X POST http://<pi_ip>:8080/api/cmd/<board3_addr> \
  -H 'Content-Type: application/json' \
  -d '{"cmd": 1}'
```

**Observe:**
- Board 1: routes MESH_CMD toward Board 3
- Board 2: relays the packet
- Board 3: receives and processes the command
- Response travels back: Board 3 -> Board 2 -> Board 1 -> MQTT -> Pi

---

## Experiment 4: Two Exit Nodes (4:2)

### Uplink

```
                    /--> [X1] --WiFi--> [Pi]
[S] ---> [R] -----|
                    \--> [X2] --WiFi--> [Pi]
```

**Setup:**
1. Flash Boards 1 and 2 as exit nodes (WiFi enabled, same SSID/broker)
2. Flash Boards 3 and 4 as relay/sensor (WiFi disabled)
3. **Physical arrangement:** Board 4 (sensor) -> Board 3 (relay) -> Boards 1 & 2 (both reachable from Board 3)
4. Power on all nodes, wait ~15 s

**Verify discovery:**
- Boards 1, 2: `GW:0h`
- Board 3: `GW:1h`, `Nbrs:2` or `Nbrs:3`
- Board 4: `GW:2h`

**Trigger:** Press BOOT on Board 4

**Observe:**
- Board 4 serial: `TRANSFER_AD` broadcast, election collects 2 candidates
- Serial: `Elected 2 exit nodes` (both X1 and X2 respond)
- Fragments are striped round-robin across both exit nodes
- **Both** X1 and X2 forward fragments to Pi
- Pi receives fragments from two different exit node topics:
  - `flp/<x1_addr>/file/data`
  - `flp/<x2_addr>/file/data`

**Record:**
- Transfer time (should be faster than Experiment 3 due to parallel exit paths)
- Fragment distribution: how many fragments went through X1 vs X2
- Both exit nodes publish meta to `flp/<addr>/file/meta`

### Downlink

```
[Pi] --MQTT--> [X1] ---> [R] ---> [S]
```

All exit nodes subscribe to `flp/admin/cmd`. Both X1 and X2 receive the command and route it into the mesh. The target node deduplicates identical commands within a 2-second window.

**Trigger (on Pi):**

Option A — via `mosquitto_pub`:
```bash
python3 -c "
import struct, sys
target = 0x<board4_addr>  # the sensor node
sys.stdout.buffer.write(struct.pack('<HB', target, 0x01))
" | mosquitto_pub -t "flp/admin/cmd" -s
```

Option B — via cloud-admin API:
```bash
curl -X POST http://<pi_ip>:8080/api/cmd/<board4_addr> \
  -H 'Content-Type: application/json' \
  -d '{"cmd": 1}'
```

**Observe:**
- Both X1 and X2 route MESH_CMD through relay to sensor
- Board 4 receives and processes the command (duplicates deduped at target)
- Board 4 responds with telemetry via mesh relay to MQTT

---

## Results Template

Copy this table for each experiment run:

| Metric | Exp 1 (1:1) | Exp 2 (2:1) | Exp 3 (3:1) | Exp 4 (4:2) |
|--------|-------------|-------------|-------------|-------------|
| File size (bytes) | | | | |
| Total fragments | | | | |
| Transfer time (s) | | | | |
| Throughput (KB/s) | | | | |
| Exit nodes used | 1 | 1 | 1 | 2 |
| Hop count | 0 | 1 | 2 | 2 |
| ARQ retransmissions | | | | |
| Fragments lost | | | | |
| Downlink RTT (ms) | | | | |

### Expected Trends

- **Exp 1 -> 2 -> 3**: Transfer time increases slightly per hop, but ARQ pipelining limits the increase
- **Exp 3 -> 4**: Transfer time decreases with 2 exit nodes (parallel striping)
- **Downlink**: MESH_CMD delivery time increases with hop count (~30 ms per hop for ESP-NOW)
- All experiments should complete well within NFR-MESH1 (1 MB in <20 min)

---

## Troubleshooting

| Symptom | Check |
|---------|-------|
| OLED blank | Verify SDA=18, SCL=17. Delete `sdkconfig` and rebuild |
| `W:--` on exit node | WiFi SSID/password wrong, or Pi not reachable |
| `GW:--` on relay | Discovery not complete. Wait longer, check range |
| `Nbrs:0` | Boards not in range, or WiFi channel mismatch. Check `FLP_ESPNOW_CHANNEL` |
| No MQTT on Pi | Check `mosquitto` running, broker URI correct, exit shows `W:OK` |
| Transfer stuck 0% | No exit node elected. Check exit node is powered and in range |
| Only 1 exit in Exp 4 | Second exit node may be out of LoRa range for TRANSFER_AD. Move closer |
| Downlink cmd not received | Target address wrong. Check node addr on OLED (`FLP-XXXX`, hex) |
| Fragments on wrong topic | Each exit node publishes to its own `flp/<addr>/file/data`. This is expected |

## OLED Display Reference

```
Line 0: FLP-XXXX v0.1.0    (node ID + version)
Line 1: ────────────────    (separator)
Line 2: W:OK N:2 L:OK      (WiFi, ESP-NOW peers, LoRa status)
Line 3: Nbrs:2 GW:1h       (neighbor count, hops to gateway)
Line 4: ────────────────    (separator)
Line 5: demo.txt 45%       (active transfer progress)
Line 6: Heap:234kB          (free heap memory)
Line 7: Up 0:05:32          (uptime)
```
