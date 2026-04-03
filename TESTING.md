# FLP Experiment Guide

Hardware: LilyGo T3-S3 V1.2 (ESP32-S3 + SX1280) nodes + cloud-admin backend (Mosquitto + Go dashboard/API) exposed via Cloudflare Tunnel

> This file is the tester-facing source of truth for experiment setup, execution, expected observations, and debugging. Keep it aligned with `.claude/skills/experiment/SKILL.md` and `.claude/skills/flash/SKILL.md`.

## Prerequisites

### Cloud Backend (Go admin + Cloudflare Tunnel)

Run the backend from `cloud-admin/` on an internet-connected host. If you are using an already-deployed shared backend, skip the startup steps and use its configured hostnames and credentials.

1. Set the backend secrets in `cloud-admin/.env`:
   - `TUNNEL_TOKEN`
   - `MQTT_NODE_PASS`
   - `MQTT_ADMIN_PASS`
2. Generate the Mosquitto password file:
   ```bash
   ./gen-passwd.sh
   ```
3. Start the stack:
   ```bash
   ./run.sh up
   ```

This starts:
- `mosquitto` (internal TCP `1883`, WebSocket `9001` for tunnel ingress)
- `flp-admin` dashboard/API on `5050`
- `cloudflared` exposing:
  - `https://<admin_hostname>` -> `http://admin:5050`
  - `wss://<mqtt_hostname>` -> `http://mosquitto:9001`

Verify:
- Dashboard opens at `http://localhost:5050` locally or `https://<admin_hostname>` through the tunnel
- Dashboard/API uses HTTP Basic Auth: username `admin`, password = `MQTT_ADMIN_PASS`
- Exit nodes can reach the broker at `wss://<mqtt_hostname>`

Useful backend monitoring:

```bash
# Follow Go backend logs (transfer start/completion, telemetry, saved files)
./run.sh logs

# Optional broker-level debugging
docker compose logs -f mosquitto
```

## Tester Preflight Checklist

Before starting an experiment, record:

- repo branch / commit under test
- backend mode:
  - shared backend, or
  - self-hosted `cloud-admin`
- board-to-role map
- USB port used for each board during flash
- node address shown on each OLED (`FLP-XXXX`)
- which boards were flashed as exit builds vs relay builds
- whether the run uses auto-demo or manual trigger
- physical placement notes (distance, walls, intended hops)

If a failure is reported without this information, collect it first.

### Flashing Nodes

All commands below are run from `flp-node/`.

### macOS / Linux

Prefer the in-repo wrapper:

```bash
./idf build
./idf flash monitor

./idf relay build
./idf relay flash monitor
```

The wrapper auto-sources ESP-IDF, clears `sdkconfig` on build/fresh, and prompts for a port if needed.

### Windows

Use raw `idf.py` commands inside **ESP-IDF PowerShell** / **ESP-IDF CMD**.

**Exit node (WiFi enabled):**

Set WiFi and cloud broker in `sdkconfig.defaults`:
```
CONFIG_FLP_WIFI_SSID="<your_ssid>"
CONFIG_FLP_WIFI_PASSWORD="<your_password>"
CONFIG_FLP_MQTT_BROKER_URI="wss://<mqtt_hostname>"
CONFIG_FLP_MQTT_USERNAME="flp-node"
CONFIG_FLP_MQTT_PASSWORD="<mqtt_node_password>"
```

```powershell
Remove-Item sdkconfig -ErrorAction SilentlyContinue
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults" set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

Confirm:
- OLED title shows `GATEWAY`
- OLED shows `W:OK`
- serial shows `Got IP: x.x.x.x` and MQTT connection success

**Relay / sensor node (WiFi disabled):**

```powershell
Remove-Item sdkconfig -ErrorAction SilentlyContinue
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.relay" set-target esp32s3
idf.py build
idf.py -p COM4 flash monitor
```

The `-D SDKCONFIG_DEFAULTS=...` flag layers `sdkconfig.defaults.relay` on top of the base defaults, setting `CONFIG_FLP_WIFI_DISABLED=y`. No need to modify or restore `sdkconfig.defaults`.

Confirm:
- OLED title shows `RELAY`
- OLED shows `W:--`
- serial shows WiFi STA/ESP-NOW startup without AP connection

> **Note:** After flashing relay nodes, switch back to gateway mode for the next exit node build:
> ```powershell
> Remove-Item sdkconfig -ErrorAction SilentlyContinue
> idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults" set-target esp32s3
> idf.py build
> ```

**Auto-demo mode** (no button press needed):

```bash
idf.py menuconfig
# Forest Link Protocol -> Auto demo mode = y
# Forest Link Protocol -> Auto demo interval = 60
```

Current repo default in `sdkconfig.defaults`: auto-demo is enabled with a 60-second interval.

---

## Topology Labels

Each experiment is labelled `N:E` where N = total ESP32 nodes, E = exit nodes.

Node roles:
- **S** = Sensor/source (WiFi disabled, initiates file transfer)
- **R** = Relay (WiFi disabled, forwards packets)
- **X** = Exit node (WiFi enabled, bridges mesh to MQTT)
- **C** = Cloud backend (Go admin + Mosquitto exposed via Cloudflare Tunnel)

---

## Experiment 1: Exit Node Direct (1:1)

### Uplink (Forest to Cloud)

```
[X] --WiFi--> [C]
```

One exit node sends a file directly to the cloud. No mesh hops. Baseline measurement.

**Setup:**
1. Flash 1 board as exit node (WiFi enabled, auto-demo or button)
2. Ensure the cloud backend is running, then power on the exit node
3. Wait for `W:OK` on OLED

**Trigger:** Press BOOT button (GPIO 1) or wait for auto-demo

**Observe on the backend dashboard/API:**
- `Active Transfer` progresses from 0-100%
- `Transfers` / `GET /api/transfers` shows transfer metadata (session_id, filename, size, chunks)
- `./run.sh logs` shows `[transfer] started session ...` and `[transfer] completed session ...`
- If self-hosting, the reassembled file is saved under `cloud-admin/received_files/`

**Expected OLED:** title shows `GATEWAY`, connectivity shows `W:OK`, and the transfer line progresses from idle to 0-100%

**Record:**
- Transfer time (start to last fragment received)
- Total fragments sent vs received
- Any retransmissions in serial log

### Downlink (Cloud to Forest)

```
[C] --MQTT--> [X]
```

Cloud sends a command to the exit node via MQTT.

**Trigger (via the cloud-admin API):**

```bash
curl -u admin:<MQTT_ADMIN_PASS> -X POST https://<admin_hostname>/api/cmd/<exit_addr> \
  -H 'Content-Type: application/json' \
  -d '{"cmd": 1}'
```

Use the node address from the OLED (`FLP-XXXX`) as `<exit_addr>` without the `0x` prefix.

**Observe:** Exit node serial log shows `MESH_CMD from 0x0000: cmd=1` (REQUEST_TELEMETRY). The response appears in the dashboard `Metrics` tab or via `GET /api/metrics/<exit_addr>`.

---

## Experiment 2: Single Mesh Hop (2:1)

### Uplink

```
[S] --ESP-NOW/LoRa--> [X] --WiFi--> [C]
```

**Setup:**
1. Flash Board 1 as exit node (WiFi enabled)
2. Flash Board 2 as sensor node (WiFi disabled, auto-demo or button)
3. Ensure the cloud backend is running, then power on Board 1, then Board 2
4. Wait ~15 s for mesh discovery

**Verify discovery:**
- Board 1 OLED title: `GATEWAY`, mesh line around `N1 C0 D0`
- Board 2 OLED title: `RELAY`, mesh line around `N1 C1 D1`

**Trigger:** Press BOOT on Board 2

**Observe:**
- Board 2 serial: `Starting file transfer`, `Transfer ad from`, exit node election
- Board 1 serial: `Exit node mode`, fragment forwarding to MQTT
- Dashboard `Active Transfer` / `Transfers` updates
- If self-hosting, the reassembled file appears under `cloud-admin/received_files/`

**Record:**
- Transfer time
- Hop count shown in serial logs
- ARQ retransmission count

### Downlink

```
[C] --MQTT--> [X] --ESP-NOW/LoRa--> [S]
```

**Trigger (via the cloud-admin API):**
```bash
curl -u admin:<MQTT_ADMIN_PASS> -X POST https://<admin_hostname>/api/cmd/<board2_addr> \
  -H 'Content-Type: application/json' \
  -d '{"cmd": 1}'
```

**Observe:**
- Board 1 serial: `Routed MESH_CMD to 0x<board2_addr>`
- Board 2 serial: `MESH_CMD from 0x0000: cmd=1`
- Board 2 responds with telemetry, relayed back through Board 1 to the backend and visible in the dashboard `Metrics` tab / `GET /api/metrics/<board2_addr>`

---

## Experiment 3: One Relay / Two-Hop Path (3:1)

### Uplink

```
[S] --ESP-NOW/LoRa--> [R] --ESP-NOW/LoRa--> [X] --WiFi--> [C]
```

**Setup:**
1. Flash Board 1 as exit node
2. Flash Boards 2 and 3 as relay/sensor nodes (WiFi disabled)
3. **Physical arrangement:** Place Board 3 far from Board 1 with Board 2 in between. Ideally Board 3 cannot directly reach Board 1 (use walls/distance to force 2-hop path)
4. Ensure the cloud backend is running, then power on all nodes and wait ~15 s

**Verify discovery:**
- Board 1 OLED title: `GATEWAY`, mesh line around `N1 C0 D0`
- Board 2 OLED title: `RELAY`, mesh line around `N2 C1 D1`
- Board 3 OLED title: `RELAY`, mesh line around `N1 C2 D2`

**Trigger:** Press BOOT on Board 3

> If Board 3 still reaches Board 1 directly, the path may collapse into a shorter topology. Use more distance / obstacles, or set `FLP_BLOCKED_PEER` for lab forcing when needed.

**Observe:**
- Board 3 serial: broadcasts `TRANSFER_AD`, election starts
- Board 1 serial: responds as exit node, `Exit node mode`
- Transfer flows: Board 3 -> Board 2 (relay) -> Board 1 (exit) -> cloud backend
- Dashboard `Active Transfer` / `Transfers` updates
- If self-hosting, the file is saved under `cloud-admin/received_files/`

**Record:**
- Transfer time (compare to Experiment 2)
- Board 2 serial: forwarding logs (`fwd 0x%04X -> 0x%04X`)
- Retransmission count per hop

### Downlink

```
[C] --MQTT--> [X] --ESP-NOW/LoRa--> [R] --ESP-NOW/LoRa--> [S]
```

**Trigger (via the cloud-admin API):**
```bash
curl -u admin:<MQTT_ADMIN_PASS> -X POST https://<admin_hostname>/api/cmd/<board3_addr> \
  -H 'Content-Type: application/json' \
  -d '{"cmd": 1}'
```

**Observe:**
- Board 1: routes MESH_CMD toward Board 3
- Board 2: relays the packet
- Board 3: receives and processes the command
- Response travels back: Board 3 -> Board 2 -> Board 1 -> MQTT -> cloud backend
- Telemetry becomes visible in the dashboard `Metrics` tab / `GET /api/metrics/<board3_addr>`

---

## Experiment 4: Two Exit Nodes (4:2)

### Uplink

```
                    /--> [X1] --WiFi--> [C]
[S] ---> [R] -----|
                    \--> [X2] --WiFi--> [C]
```

**Setup:**
1. Flash Boards 1 and 2 as exit nodes (WiFi enabled, same SSID/broker)
2. Flash Boards 3 and 4 as relay/sensor (WiFi disabled)
3. **Physical arrangement:** Board 4 (sensor) -> Board 3 (relay) -> Boards 1 & 2 (both reachable from Board 3)
4. Ensure the cloud backend is running, then power on all nodes and wait ~15 s

**Verify discovery:**
- Boards 1, 2 OLED titles: `GATEWAY`, mesh line around `C0 D0`
- Board 3 OLED title: `RELAY`, mesh line around `N2 C1 D1` or better
- Board 4 OLED title: `RELAY`, mesh line around `C2 D2`

**Trigger:** Press BOOT on Board 4

**Observe:**
- Board 4 serial: `TRANSFER_AD` broadcast, election collects 2 candidates
- Serial: `Elected 2 exit nodes` (both X1 and X2 respond)
- Fragments are striped round-robin across both exit nodes
- **Both** X1 and X2 forward fragments to the backend
- Dashboard `Active Transfer` / `Transfers` updates
- If self-hosting, the file is saved under `cloud-admin/received_files/`
- The backend receives fragments from two different exit node topics:
  - `flp/<x1_addr>/file/data`
  - `flp/<x2_addr>/file/data`

**Record:**
- Transfer time (should be faster than Experiment 3 due to parallel exit paths)
- Fragment distribution: how many fragments went through X1 vs X2
- Both exit nodes publish meta to `flp/<addr>/file/meta`

### Downlink

```
[C] --MQTT--> [X1] ---> [R] ---> [S]
```

All exit nodes subscribe to `flp/admin/cmd`. Both X1 and X2 receive the command and route it into the mesh. The target node deduplicates identical commands within a 2-second window.

**Trigger (via the cloud-admin API):**
```bash
curl -u admin:<MQTT_ADMIN_PASS> -X POST https://<admin_hostname>/api/cmd/<board4_addr> \
  -H 'Content-Type: application/json' \
  -d '{"cmd": 1}'
```

**Observe:**
- Both X1 and X2 route MESH_CMD through relay to sensor
- Board 4 receives and processes the command (duplicates deduped at target)
- Board 4 responds with telemetry via mesh relay to MQTT, and the result appears in the dashboard `Metrics` tab / `GET /api/metrics/<board4_addr>`

---

## Results Template

Copy this table for each experiment run:

| Metric              | Exp 1 (1:1) | Exp 2 (2:1) | Exp 3 (3:1) | Exp 4 (4:2) |
| ------------------- | ----------- | ----------- | ----------- | ----------- |
| File size (bytes)   |             |             |             |             |
| Total fragments     |             |             |             |             |
| Transfer time (s)   |             |             |             |             |
| Throughput (KB/s)   |             |             |             |             |
| Exit nodes used     | 1           | 1           | 1           | 2           |
| Hop count           | 0           | 1           | 2           | 2           |
| ARQ retransmissions |             |             |             |             |
| Fragments lost      |             |             |             |             |
| Downlink RTT (ms)   |             |             |             |             |

### Expected Trends

- **Exp 1 -> 2 -> 3**: Transfer time increases slightly per hop, but ARQ pipelining limits the increase
- **Exp 3 -> 4**: Transfer time decreases with 2 exit nodes (parallel striping)
- **Downlink**: MESH_CMD delivery time increases with hop count (~30 ms per hop for ESP-NOW)
- All experiments should complete well within NFR-MESH1 (1 MB in <20 min)

---

## Troubleshooting

| Symptom                                   | Check                                                                                                             |
| ----------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| OLED blank                                | Verify SDA=18, SCL=17. Delete `sdkconfig` and rebuild                                                             |
| Wrong role after flash                    | Delete `sdkconfig`, rebuild using the correct exit or relay workflow, then reflash                                |
| `W:--` on exit node                       | WiFi SSID/password wrong, or upstream internet unavailable                                                        |
| Relay shows `C--` / `D--`                 | Discovery not complete. Wait longer, check range, and verify the intended topology                                |
| Mesh line shows `N0`                      | Boards not in range, or WiFi channel mismatch. Check `FLP_ESPNOW_CHANNEL`                                         |
| Dashboard/API unreachable                 | Check `cloud-admin` is running, tunnel hostnames resolve, and Basic Auth uses user `admin` with `MQTT_ADMIN_PASS` |
| No transfer on backend                    | Check broker URI/credentials, backend is running, exit shows `W:OK`, and serial shows `MQTT connected to broker`  |
| Transfer stuck 0%                         | No exit node elected. Check exit node is powered and in range                                                     |
| Only 1 exit in Exp 4                      | Second exit node may be out of LoRa range for TRANSFER_AD. Move closer                                            |
| Downlink cmd not received                 | Target address wrong. Check node addr on OLED (`FLP-XXXX`, hex)                                                   |
| Downlink telemetry works but LED does not | Ensure firmware includes `cmd=4` support and `FLP_LED_GPIO` is not disabled                                       |
| Fragments on wrong topic                  | Each exit node publishes to its own `flp/<addr>/file/data`. This is expected                                      |

### Evidence to Collect for Debugging

For any failure, capture:

1. experiment number and direction (uplink / downlink)
2. branch / commit
3. board-role map and node addresses
4. physical placement notes
5. exact flash/build commands used
6. exact API / `curl` command used
7. serial logs from all involved nodes
8. backend logs (`./run.sh logs` or `docker compose logs -f mosquitto`)
9. OLED photos or screenshots
10. dashboard screenshots and HTTP responses

### Generic Downlink Command Examples

Use the node address from the OLED (`FLP-XXXX`) without `0x`.

```bash
# Telemetry request
curl -u admin:<MQTT_ADMIN_PASS> -X POST https://<admin_hostname>/api/cmd/<node_addr> \
  -H 'Content-Type: application/json' \
  -d '{"cmd": 1}'

# LED on
curl -u admin:<MQTT_ADMIN_PASS> -X POST https://<admin_hostname>/api/cmd/<node_addr> \
  -H 'Content-Type: application/json' \
  -d '{"cmd": 4, "data": "01"}'

# LED off
curl -u admin:<MQTT_ADMIN_PASS> -X POST https://<admin_hostname>/api/cmd/<node_addr> \
  -H 'Content-Type: application/json' \
  -d '{"cmd": 4, "data": "00"}'
```

For successful downlink, expect:

- exit node serial log showing routed or locally handled `MESH_CMD`
- target node serial log showing received command
- OLED briefly showing `>> Cloud CMD`
- telemetry appearing in dashboard for `cmd=1`, or visible LED state change for `cmd=4`

## OLED Display Reference

```
Line 0: FLP-XXXX GATEWAY   or FLP-XXXX  RELAY
Line 1: W:OK Lo P:02       or W:-- Lo P:02
Line 2: N2 C1 D1           (neighbors, control hops, data hops)
Line 3: Xfer: idle         or demo.txt 45%
Line 4: >> Cloud CMD       (briefly shown after downlink command)
Line 5: 234kB              (free heap)
Line 6: 00:05:32           (uptime)
```
