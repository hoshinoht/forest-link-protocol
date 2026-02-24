# FLP Testing Guide

Hardware: 3x LilyGo T3-S3 V1.2 (ESP32-S3 + SX1280) + 1x Raspberry Pi

## Network Topology

```
[Board 3: Relay] --ESP-NOW/LoRa--> [Board 2: Relay] --ESP-NOW/LoRa--> [Board 1: Gateway] --WiFi--> [Pi: MQTT Broker]
```

- **Board 1 (Gateway):** WiFi connected to Pi, publishes to MQTT
- **Boards 2 & 3 (Relays):** WiFi disabled, mesh-only via ESP-NOW + LoRa
- **Raspberry Pi:** Runs Mosquitto MQTT broker (and optionally WiFi AP)

## 1. Raspberry Pi Setup

### Install Mosquitto

```bash
sudo apt update && sudo apt install -y mosquitto mosquitto-clients
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
```

### Verify broker is running

```bash
# Terminal 1: subscribe to all FLP topics
mosquitto_sub -v -t '#'

# Terminal 2: test publish
mosquitto_pub -t "test" -m "hello"
# You should see "test hello" in Terminal 1
```

### Note your Pi's IP address

```bash
hostname -I
# e.g. 192.168.1.100
```

### (Optional) Pi as WiFi AP

If you don't have an existing WiFi network, configure the Pi as a hotspot:

```bash
sudo apt install -y hostapd dnsmasq
```

Edit `/etc/hostapd/hostapd.conf`:
```
interface=wlan0
ssid=ForestLink_AP
hw_mode=g
channel=1
wpa=2
wpa_passphrase=forestlink
wpa_key_mgmt=WPA-PSK
```

Edit `/etc/dnsmasq.conf`:
```
interface=wlan0
dhcp-range=192.168.4.2,192.168.4.20,255.255.255.0,24h
```

Assign static IP and start:
```bash
sudo ip addr add 192.168.4.1/24 dev wlan0
sudo systemctl start hostapd
sudo systemctl start dnsmasq
```

## 2. Flash the Boards

All commands run from `flp-node/`.

### Board 1: Gateway

Edit `sdkconfig.defaults` — set your WiFi and broker:

```
CONFIG_FLP_WIFI_SSID="<your_wifi_ssid>"
CONFIG_FLP_WIFI_PASSWORD="<your_wifi_password>"
CONFIG_FLP_MQTT_BROKER_URI="mqtt://<pi_ip_address>"
```

Build and flash:

```bash
rm -f sdkconfig
idf.py build
idf.py -p /dev/<port> flash monitor
```

Confirm on serial monitor:
- `WiFi station initialized, connecting...`
- `Got IP: x.x.x.x`
- OLED shows `W:OK`

### Boards 2 & 3: Relay

Switch to relay config:

```bash
cp sdkconfig.defaults.relay sdkconfig.defaults
rm -f sdkconfig
idf.py build
idf.py -p /dev/<port> flash monitor
```

Confirm on serial monitor:
- `WiFi STA started (no AP) for ESP-NOW, ch=1`
- OLED shows `W:--`

**Important:** After flashing relays, restore the gateway config if needed:
```bash
git checkout sdkconfig.defaults
```

## 3. Test Scenarios

### Test A: Single Node Smoke Test

**What:** Verify one board boots and displays status.

1. Flash any board (gateway or relay)
2. Check OLED shows: node ID, WiFi status, heap, uptime ticking
3. Press the BOOT button (GPIO 0) — serial log should show:
   ```
   Demo transfer: demo.txt (33 bytes)
   ```
4. OLED line 5 should briefly show transfer progress

### Test B: Two-Node Mesh (1 hop)

**What:** Verify ESP-NOW/LoRa mesh discovery and forwarding.

1. Power Board 1 (gateway) + Board 2 (relay)
2. Wait 10-15 seconds for mesh discovery
3. Check OLEDs:
   - Board 1: `Nbrs:1 GW:0h` (it IS the gateway)
   - Board 2: `Nbrs:1 GW:1h` (1 hop to internet)
4. Press BOOT on Board 2 → demo.txt transfers to Board 1
5. Check `mosquitto_sub` on Pi — file data should appear

### Test C: Three-Node Mesh (2 hops)

**What:** Verify multi-hop forwarding (NFR-MESH1 target).

1. Power all 3 boards
2. **Arrange physically:** Board 3 far from Board 1, Board 2 in between
   - Ideally Board 3 can only reach Board 2 (not Board 1 directly)
3. Wait for mesh discovery (~15s)
4. Check OLEDs:
   - Board 1: `GW:0h` (gateway)
   - Board 2: `GW:1h`
   - Board 3: `GW:2h`
5. Press BOOT on Board 3
6. Watch transfer hop: Board 3 → Board 2 → Board 1 → MQTT → Pi
7. Verify on `mosquitto_sub`

### Test D: UART File Ingest

**What:** Send a file from laptop via UART to trigger mesh transfer.

1. Connect USB-to-serial adapter to Board 3: TX→GPIO 44 (RX), RX→GPIO 43 (TX)
2. Send file data over UART at 115200 baud
3. Board 3 should start a file transfer through the mesh
4. Verify arrival on Pi via `mosquitto_sub`

### Test E: Throughput / Performance

**What:** Measure transfer time for larger payloads.

1. Set up 3-node mesh (Test C)
2. Send larger payloads via UART or modify `DEMO_PAYLOAD` in `main.cpp`
3. Time from transfer start to MQTT arrival on Pi
4. Target: 1MB over 3 hops within 20 minutes (NFR-MESH1)

## 4. Troubleshooting

| Symptom | Check |
|---------|-------|
| OLED blank | Verify SDA=18, SCL=17 in sdkconfig. Delete `sdkconfig` and rebuild |
| `W:--` on gateway | WiFi SSID/password wrong, or Pi AP not reachable |
| `GW:--` on relay | Mesh discovery hasn't completed — wait longer, check boards are in range |
| `Nbrs:0` | ESP-NOW peers not found — verify all boards on same WiFi channel |
| No MQTT data on Pi | Check `mosquitto` is running, broker URI correct, gateway shows `W:OK` |
| `ESP_ERR_INVALID_STATE` in log | GPIO pin conflict — check no two peripherals share a pin |
| Transfer stuck at 0% | Check serial logs for ARQ retransmissions, verify relay is forwarding |

## 5. Monitoring

### Serial monitor (per board)
```bash
idf.py -p /dev/<port> monitor
```

### MQTT (on Pi)
```bash
# All topics
mosquitto_sub -v -t '#'

# FLP-specific (adjust topic prefix as needed)
mosquitto_sub -v -t 'flp/#'
```

### OLED display lines
```
Line 0: FLP-XXXX v0.1.0    (node ID + version)
Line 1: ─────────────────   (separator)
Line 2: W:OK N:2 L:OK       (WiFi, ESP-NOW peers, LoRa)
Line 3: Nbrs:2 GW:1h        (neighbors, hops to gateway)
Line 4: ─────────────────   (separator)
Line 5: demo.txt 45%        (active transfer)
Line 6: Heap:234kB          (free heap)
Line 7: Up 0:05:32          (uptime)
```
