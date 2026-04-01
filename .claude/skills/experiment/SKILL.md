---
name: experiment
description: Guide setup and execution of an FLP experiment topology (1-4)
---

# Experiment Skill / Tester Guide

This file is written to serve two purposes:

1. as the `/experiment` skill used by Claude
2. as a tester-facing guide for setting up, running, recording, and debugging FLP experiments

Use this as a practical handoff document. It should be readable by a tester without needing to inspect the code.

## Scope

This guide covers:

- experiment topologies 1-4
- uplink and downlink testing
- what hardware/software to prepare
- what to expect on OLED, serial, and backend
- what to record for results
- how to debug failures stage-by-stage

## Operating rules for the assistant

When this skill is used interactively:

1. do **not** assume the tester is on Windows only
2. do **not** assume the tester uses the shared backend
3. ask which backend mode they are using:
   - shared backend
   - self-hosted `cloud-admin`
4. ask which host OS they are using:
   - macOS / Linux
   - Windows
5. ask which experiment they are running and whether they want:
   - uplink
   - downlink
   - both
6. use `TESTING.md` as the companion repo document, but keep this skill detailed enough that it can be handed directly to a tester
7. if something fails, move through the failure-isolation checklist in this document instead of guessing

## Quick orientation

### Node roles

- **S** = source / sensor node (WiFi disabled, starts file transfer)
- **R** = relay node (WiFi disabled, forwards packets)
- **X** = exit node / gateway (WiFi enabled, bridges mesh to cloud)
- **C** = cloud backend (`cloud-admin` + Mosquitto)

### Current key runtime behavior

- exit builds use `sdkconfig.defaults`
- relay builds use `sdkconfig.defaults` layered with `sdkconfig.defaults.relay`
- current repo default has **auto-demo enabled** at **60 seconds**
- manual demo button default is **GPIO 1** when auto-demo is disabled
- current OLED layout uses:
  - title: `FLP-XXXX GATEWAY` or `FLP-XXXX  RELAY`
  - connectivity: `W:OK Lo P:02` or `W:-- Lo P:02`
  - mesh line: `N2 C1 D1`
  - transfer line: `Xfer: idle` or `demo.txt 45%`
  - cloud command banner: `>> Cloud CMD`

### Useful downlink commands

- **Telemetry request**: `cmd = 1`
- **LED control**: `cmd = 4`, payload `01` = on, `00` = off

---

## Preflight checklist

Before running any experiment, collect this information:

- repo branch / commit under test
- backend mode:
  - shared backend, or
  - self-hosted backend
- board-to-role mapping
  - Board A = X
  - Board B = R
  - Board C = S
- USB port for each board
- node address shown on OLED (`FLP-XXXX`)
- whether each board was flashed as:
  - exit build
  - relay build
- trigger mode:
  - auto-demo
  - manual button
- physical layout notes:
  - which boards are near each other
  - which boards should be out of direct range
  - walls / obstacles / distance

If a tester reports a failure without this information, ask for it first.

---

## Backend modes

### Mode A: shared backend

Use this when the team already provides a deployed MQTT/admin backend.

Tester should know:

- admin URL
- MQTT broker URL
- dashboard/API credentials
- whether the firmware defaults already point at the shared backend

Do **not** hardcode these values in verbal instructions unless you have confirmed they are still current.

### Mode B: self-hosted backend

Use this when the tester runs `cloud-admin/` locally or on a lab host.

Minimum checks:

- `cloud-admin/.env` populated
- `./gen-passwd.sh` run
- `./run.sh up` succeeds
- dashboard reachable
- broker reachable by exit nodes

When self-hosted, collect backend logs as part of every failure report.

---

## Flashing summary

For deep flashing instructions, see the `flash` skill. The short version is below.

### macOS / Linux

Use the in-repo wrapper from `flp-node/`:

```bash
./idf build
./idf flash monitor

./idf relay build
./idf relay flash monitor
```

Useful notes:

- the wrapper auto-sources ESP-IDF from `~/esp/esp-idf-v5.5.3/export.sh`
- it auto-picks a USB port if one is not specified
- it clears `sdkconfig` on `build` / `fresh` so role switches are clean

### Windows

Use raw `idf.py` commands in **ESP-IDF PowerShell** or **ESP-IDF CMD**.

Exit build:

```powershell
cd flp-node
Remove-Item sdkconfig -ErrorAction SilentlyContinue
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults" set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

Relay build:

```powershell
cd flp-node
Remove-Item sdkconfig -ErrorAction SilentlyContinue
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.relay" set-target esp32s3
idf.py build
idf.py -p COM4 flash monitor
```

### Role-switch rule

When switching a board between **exit** and **relay** firmware, always delete `sdkconfig` first and rebuild.

---

## Post-flash acceptance checks

### Exit node

Expected signs of success:

- serial shows WiFi association and IP acquisition
- serial shows MQTT connected
- OLED title shows `GATEWAY`
- OLED shows `W:OK`
- mesh line should eventually show `C0 D0`

### Relay / sensor node

Expected signs of success:

- serial shows WiFi STA started for ESP-NOW but **no AP connection**
- OLED title shows `RELAY`
- OLED shows `W:--`
- after discovery, mesh line should show non-`--` control/data hop counts

### Command-path indicator

When a cloud command is received, the OLED may briefly show:

```text
>> Cloud CMD
```

This is useful when validating downlink delivery.

---

## Experiment matrix

| Experiment | Nodes | Purpose |
|---|---:|---|
| 1 | 1:1 | direct exit-to-cloud baseline |
| 2 | 2:1 | single mesh hop: source -> exit |
| 3 | 3:1 | two-hop path: source -> relay -> exit |
| 4 | 4:2 | multi-exit striping: source -> relay -> two exits |

---

## Experiment 1 — direct exit node (1:1)

### Topology

```text
[X] --WiFi--> [C]
```

### Setup

1. flash one board as an exit node
2. confirm backend is reachable
3. power on the exit node
4. wait until OLED shows `W:OK`

### Uplink test

Trigger:

- wait for auto-demo, or
- if auto-demo is disabled, press the demo button on GPIO 1

Expected:

- transfer progress appears on OLED
- backend logs show transfer start and completion
- dashboard shows active transfer and completed transfer entry

Record:

- file size
- total fragments
- transfer duration
- retransmission count

### Downlink test

Use the node address from `FLP-XXXX` without `0x`.

Telemetry example:

```bash
curl -u admin:<ADMIN_PASS> -X POST https://<admin_hostname>/api/cmd/<node_addr> \
  -H 'Content-Type: application/json' \
  -d '{"cmd": 1}'
```

LED example:

```bash
# LED on
curl -u admin:<ADMIN_PASS> -X POST https://<admin_hostname>/api/cmd/<node_addr> \
  -H 'Content-Type: application/json' \
  -d '{"cmd": 4, "data": "01"}'

# LED off
curl -u admin:<ADMIN_PASS> -X POST https://<admin_hostname>/api/cmd/<node_addr> \
  -H 'Content-Type: application/json' \
  -d '{"cmd": 4, "data": "00"}'
```

Expected:

- serial shows `MESH_CMD from 0x0000: cmd=1` for telemetry, or `cmd=4` for LED control
- OLED may briefly show `>> Cloud CMD`
- telemetry appears on the dashboard for `cmd=1`
- LED visibly changes state for `cmd=4`

---

## Experiment 2 — single mesh hop (2:1)

### Topology

```text
[S] --ESP-NOW/LoRa--> [X] --WiFi--> [C]
```

### Setup

1. flash Board 1 as exit node
2. flash Board 2 as relay/sensor node
3. power on Board 1 first, then Board 2
4. wait for discovery to stabilize

Expected readiness:

- exit node title shows `GATEWAY`
- source node title shows `RELAY`
- source node mesh line should usually settle around `C1 D1`

### Uplink test

Trigger on Board 2.

Expected:

- Board 2 logs transfer start
- Board 1 logs exit-node forwarding / MQTT publish activity
- dashboard shows the transfer

Record:

- transfer duration
- observed hop count
- retransmissions

### Downlink test

Run either telemetry or LED command against Board 2.

Expected:

- Board 1 logs routed command
- Board 2 logs received command
- Board 2 OLED may show `>> Cloud CMD`
- telemetry or LED state change confirms end-to-end downlink path

---

## Experiment 3 — one relay / two-hop path (3:1)

### Topology

```text
[S] --ESP-NOW/LoRa--> [R] --ESP-NOW/LoRa--> [X] --WiFi--> [C]
```

### Setup

1. flash Board 1 as exit node
2. flash Board 2 as relay
3. flash Board 3 as source
4. physically place Board 3 far enough from Board 1 that Board 2 is needed as the intermediate hop

Expected readiness:

- Board 1: `GATEWAY`, likely `C0 D0`
- Board 2: `RELAY`, likely `C1 D1`
- Board 3: `RELAY`, likely `C2 D2`

If Board 3 can still directly reach Board 1, the topology may collapse into a shorter path and the experiment is invalid.

### Uplink test

Trigger on Board 3.

Expected:

- Board 3 advertises the transfer
- Board 2 relays traffic
- Board 1 acts as exit and forwards to MQTT
- dashboard shows the transfer completing

Record:

- transfer duration
- whether Board 2 clearly relayed packets
- retransmissions / stalls

### Downlink test

Send telemetry or LED command to Board 3.

Expected:

- exit routes command into mesh
- relay forwards it
- Board 3 processes it
- response or LED state change confirms two-hop downlink

---

## Experiment 4 — two exit nodes (4:2)

### Topology

```text
                    /--> [X1] --WiFi--> [C]
[S] ---> [R] -----|
                    \--> [X2] --WiFi--> [C]
```

### Setup

1. flash Boards 1 and 2 as exit nodes
2. flash Board 3 as relay
3. flash Board 4 as source
4. place Board 4 behind Board 3, with both exits reachable from Board 3

Expected readiness:

- Boards 1 and 2 show `GATEWAY`
- Board 3 shows `RELAY` with at least two useful neighbors
- Board 4 shows `RELAY` and two-hop control/data path to cloud

### Uplink test

Trigger on Board 4.

Expected:

- transfer advertisement reaches both exits
- both exits participate in forwarding to cloud
- backend receives fragments from both exit topics
- transfer time should improve versus a comparable single-exit path

Record:

- total transfer duration
- whether both exits participated
- fragment distribution across both exits

### Downlink test

Send telemetry or LED command to Board 4.

Expected:

- multiple exits may route the same command into the mesh
- target node should still process it once due to deduplication
- OLED / serial / backend confirms delivery

---

## What to record for every run

Minimum dataset:

- experiment number
- timestamp
- branch / commit
- backend mode
- board-role map
- node addresses
- trigger type (auto-demo or manual)
- file size
- chunk / fragment count
- transfer duration
- retransmission count
- whether downlink telemetry worked
- whether LED on/off worked
- notes on packet loss / stalls / duplicate commands / unexpected OLED state

---

## Failure isolation checklist

Work through failures in this order.

### 1. Build / flash failure

Check:

- correct terminal / ESP-IDF environment
- correct port
- no other program holding the port
- `sdkconfig` deleted before switching roles

Capture:

- exact flash command
- full terminal output
- port name

### 2. Node boots but wrong OLED state

Check:

- flashed the correct build type (exit vs relay)
- OLED title says expected role
- exit node reaches `W:OK`
- relay node intentionally stays `W:--`

Capture:

- OLED photo
- first boot log

### 3. Exit node never reaches cloud

Check:

- WiFi credentials
- broker URI / credentials
- backend is reachable
- serial contains IP acquisition and MQTT connection logs

Capture:

- serial log from boot to failure
- backend logs

### 4. Nodes do not discover each other

Check:

- power-on order
- wait longer for discovery
- boards are in range
- boards are on compatible channel / defaults
- topology is not too close or too far

Capture:

- OLED photo of every board
- serial logs from all boards
- physical layout notes

### 5. Transfer starts but does not complete

Check:

- active transfer progress on OLED and dashboard
- retransmissions / NACK behavior in serial logs
- exit node MQTT connectivity remained healthy
- backend logs for fragment receive / completion

Capture:

- source, relay, and exit serial logs
- backend logs
- dashboard screenshot

### 6. Downlink reaches exit but not target

Check:

- correct `<node_addr>` used in API URL
- command payload valid hex
- `cmd=4` uses `00` or `01`
- exit logs routed command
- target OLED briefly shows `>> Cloud CMD`

Capture:

- exact curl command
- HTTP response
- exit and target serial logs
- OLED photo after command

### 7. Experiment 4 only uses one exit

Check:

- both exits are powered and cloud-connected
- both exits are in range of the relay
- topology is not biased too strongly toward one exit

Capture:

- logs from both exits
- backend evidence showing which `flp/<addr>/file/data` topics were active

---

## Evidence package for bug reports

Ask testers to attach all of the following:

1. experiment number and direction
2. branch / commit
3. board-role map and node addresses
4. physical arrangement notes
5. exact flash commands used
6. exact API / curl command used
7. serial logs from all involved nodes
8. backend logs
9. OLED photos or screenshots
10. dashboard screenshots
11. clear statement of where the failure occurred:
    - flash
    - boot
    - discovery
    - uplink transfer
    - downlink command
    - multi-exit

---

## Assistant interaction pattern

If this skill is used interactively, guide the tester with this sequence:

1. identify OS
2. identify backend mode
3. identify experiment number
4. identify direction (uplink / downlink / both)
5. confirm board-role map
6. confirm which stage is failing, if any
7. provide only the commands and checks relevant to that stage

Do not dump all instructions at once unless the user explicitly wants the full guide.
