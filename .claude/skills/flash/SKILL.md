---
name: flash
description: Build and flash firmware to connected ESP32-S3, then open serial monitor
---

# Flash Skill / Tester Guide

This file is both:

1. the `/flash` operational skill for Claude
2. a tester-facing guide for building, flashing, verifying, and debugging FLP firmware on LilyGo T3-S3 boards

## Scope

Use this guide when a tester needs to:

- build firmware
- flash an exit node
- flash a relay / sensor node
- switch a board from exit mode to relay mode or back
- capture boot / monitor logs for debugging

## Core concepts

### Build roles

- **Exit node / gateway build**
  - uses `sdkconfig.defaults`
  - WiFi + MQTT enabled
- **Relay / sensor build**
  - uses `sdkconfig.defaults` plus `sdkconfig.defaults.relay`
  - WiFi AP connection disabled
  - still uses WiFi STA / ESP-NOW for mesh participation

### Role switch rule

Whenever a board changes role between **exit** and **relay**, delete `sdkconfig` and rebuild.

This avoids stale configuration leaking from a previous build mode.

### Current defaults worth knowing

- target = `esp32s3`
- auto-demo currently defaults to **enabled** with **60 second** interval
- manual demo button default is **GPIO 1** when auto-demo is disabled

---

## What success looks like after flash

### Exit node

Expected indicators:

- serial shows WiFi startup and IP acquisition
- serial shows MQTT connected
- OLED title shows `GATEWAY`
- OLED shows `W:OK`

### Relay / sensor node

Expected indicators:

- serial shows WiFi STA started for ESP-NOW / mesh use
- serial does **not** connect to an AP
- OLED title shows `RELAY`
- OLED shows `W:--`

### Useful OLED fields

- `W:OK` = WiFi connected to AP
- `W:--` = no AP connection (expected for relay build)
- `Lo P:<n>` = LoRa present / ESP-NOW peer count line
- `N<n> C<h> D<h>` = neighbors, control-path hops, data-path hops
- `>> Cloud CMD` = recent cloud command received

---

## macOS / Linux workflow

Use the in-repo wrapper from `flp-node/`.

### Why use the wrapper

The `./idf` wrapper:

- sources `~/esp/esp-idf-v5.5.3/export.sh`
- tracks whether the current build is exit or relay mode
- clears `sdkconfig` on `build` / `fresh`
- auto-picks a serial port if one is not specified

### Exit node

```bash
cd flp-node
./idf build
./idf flash monitor
```

### Relay / sensor node

```bash
cd flp-node
./idf relay build
./idf relay flash monitor
```

### Optional commands

```bash
# full clean, rebuild, erase flash, then flash
./idf fresh

# relay variant of the same
./idf relay fresh

# specify a port manually
./idf flash -p /dev/cu.usbmodem12301 monitor
./idf relay flash -p /dev/cu.usbmodem12301 monitor
```

If multiple serial ports are detected, the wrapper will prompt for selection.

---

## Windows workflow

Run commands inside **ESP-IDF PowerShell** or **ESP-IDF CMD**.

If `idf.py` is not found, the tester is in the wrong terminal.

### Exit node

```powershell
cd flp-node
Remove-Item sdkconfig -ErrorAction SilentlyContinue
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults" set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

### Relay / sensor node

```powershell
cd flp-node
Remove-Item sdkconfig -ErrorAction SilentlyContinue
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.relay" set-target esp32s3
idf.py build
idf.py -p COM4 flash monitor
```

Replace `COM3` / `COM4` with the actual port shown in Device Manager.

---

## Port identification

### macOS / Linux

Common device names:

- `/dev/cu.usbmodem*`
- `/dev/cu.usbserial*`
- `/dev/cu.SLAB*`

### Windows

Check **Device Manager > Ports (COM & LPT)**.

Typical labels:

- USB Serial Device
- USB-SERIAL CH340
- CP210x USB to UART Bridge

If the port disappears during flash/monitor work, try a different USB cable first.

---

## Flashing one board at a time

Recommended tester workflow:

1. label the board physically
2. connect only one board when possible
3. flash it
4. record:
   - board role
   - USB port
   - node address from OLED
5. disconnect it
6. repeat for the next board

This avoids mixing up ports and addresses later in experiment runs.

---

## Post-flash checklist

After every flash, verify:

1. build mode was the intended one
2. serial monitor opened successfully
3. device booted without repeated crash/reboot loop
4. OLED came up
5. role shown on OLED matches intended role
6. connectivity state matches intended role
7. node address `FLP-XXXX` is recorded

Do not move on to experiment testing until these checks pass.

---

## Common failure modes and fixes

### `idf.py` not found

Cause:

- wrong shell / terminal

Fix:

- macOS/Linux: use the repo wrapper `./idf`
- Windows: open ESP-IDF PowerShell / CMD

Capture:

- exact terminal command used
- terminal output

### Serial port not found

Cause:

- bad cable
- driver missing
- wrong port selected
- board not enumerated

Fix:

- reconnect board
- change cable
- check Device Manager or `/dev/cu.*`
- close other tools holding the port

Capture:

- port list screenshot / output
- cable / adapter details if known

### `could not open port`

Cause:

- another serial monitor still owns the port

Fix:

- close Arduino IDE, PuTTY, screen, minicom, tmux monitor sessions, or other `idf.py monitor` instances

### Wrong role after flashing

Cause:

- `sdkconfig` was reused from the previous build mode

Fix:

- delete `sdkconfig`
- rebuild using the correct role workflow

### Exit node shows `W:--`

Cause:

- exit node did not connect to WiFi / AP

Check:

- WiFi credentials
- access point availability
- build role really is exit mode

### Relay node shows `W:OK`

Cause:

- wrong firmware image flashed (exit instead of relay)

Fix:

- rebuild using relay overlay

### OLED blank or missing

Check:

- board really finished booting
- build is current
- power is stable
- if needed, rebuild from clean state and retry

### Reboot loop immediately after flash

Check:

- full serial boot log
- whether the first boot after flash differs from the second boot
- power quality / cable stability

Do not summarize from memory; capture the full boot log.

---

## Evidence to capture when flashing fails

Ask the tester for:

1. exact command used
2. OS used
3. port used
4. full terminal output
5. OLED photo if the board boots at all
6. whether the board ever entered monitor
7. whether the same cable/port works with another board

---

## Suggested tester handoff format

When giving this to a tester, ask them to fill in:

- board label:
- intended role:
- OS used:
- flash command used:
- port used:
- OLED address after flash:
- OLED role shown:
- WiFi state shown:
- serial result summary:
- pass / fail:
- notes:

---

## Assistant interaction pattern

When used as a skill, guide the user in this order:

1. ask OS
2. ask board role to flash
3. ask whether they want build-only or flash+monitor
4. ask port if not obvious
5. provide only the matching command set
6. wait for the result
7. if flash fails, switch to the failure mode checklist above

Do not assume the same commands work unchanged across macOS/Linux and Windows.
