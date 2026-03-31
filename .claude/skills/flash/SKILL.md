---
name: flash
description: Build and flash firmware to connected ESP32-S3, then open serial monitor
---

# Flash Skill

Build the FLP firmware and flash it to a connected ESP32-S3 board.

## Environment Detection

Detect the user's OS from the environment context:
- **macOS/Linux**: Use the `./idf` wrapper script in `flp-node/`
- **Windows**: Use raw `idf.py` commands in the ESP-IDF PowerShell terminal

## Steps (macOS / Linux)

1. `cd flp-node`
2. Detect serial port: `ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1`
3. If no port found, tell the user to connect the board and abort.
4. Run `./idf build` (exit node) or `./idf relay build` (relay node)
5. Run `./idf flash monitor` or `./idf -p <port> flash monitor`

## Steps (Windows)

1. Confirm the user is in an **ESP-IDF v5.5.3 PowerShell** terminal (created by EIM or the ESP-IDF Tools Installer). If `idf.py` is not found, they're in the wrong terminal.
2. `cd flp-node`
3. Detect COM port: Ask user to check **Device Manager > Ports (COM & LPT)** for "USB-SERIAL CH340" or "USB Serial Device" (e.g., `COM3`).
4. Delete stale sdkconfig to ensure clean config:
   ```powershell
   Remove-Item sdkconfig -ErrorAction SilentlyContinue
   ```
5. **Exit node build:**
   ```powershell
   idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults" set-target esp32s3
   idf.py build
   idf.py -p COM3 flash monitor
   ```
6. **Relay node build:**
   ```powershell
   idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.relay" set-target esp32s3
   idf.py build
   idf.py -p COM3 flash monitor
   ```
7. After flashing a relay, remind the user to delete `sdkconfig` before building an exit node again.

## Arguments

- If the user specifies a port (e.g., `/flash COM3` or `/flash /dev/ttyACM1`), use that instead of auto-detecting.
- If the user says "build only" or "just build", skip flash and monitor steps.
- If the user says "relay", use the relay overlay config.

## Verification

After flashing, verify in the serial monitor:
- **Exit node**: `Got IP: x.x.x.x` and `MQTT connected to broker`, OLED shows `W:OK`
- **Relay node**: `WiFi STA started (no AP)`, OLED shows `W:--`

## Troubleshooting (Windows)

| Symptom | Fix |
|---------|-----|
| `idf.py` not recognized | Not in ESP-IDF terminal. Open "ESP-IDF v5.5.3 PowerShell" from Start Menu |
| COM port not found | Check Device Manager. Install CH340/CP2102 driver. Try a different USB-C cable |
| "could not open port" | Close any other serial monitor holding the port (PuTTY, Arduino IDE, etc.) |
| Build error about `sdkconfig` | Delete sdkconfig: `Remove-Item sdkconfig -ErrorAction SilentlyContinue` |
| Semicolon in SDKCONFIG_DEFAULTS fails | Ensure you're using PowerShell (not CMD). Wrap in quotes: `"sdkconfig.defaults;sdkconfig.defaults.relay"` |
