# Pico W → ESP32 Wiring Guide

## Connections

Three wires total. UART is cross-wired (TX→RX, RX←TX).

```
Pico W                          ESP32
┌──────────┐                    ┌──────────┐
│ GP0 (TX) ├───────────────────►│ GPIO16 (RX) │  UART2 RX
│ GP1 (RX) │◄───────────────────┤ GPIO17 (TX) │  UART2 TX
│ GND      ├────────────────────┤ GND         │
└──────────┘                    └──────────────┘
```

| Pico W Pin | Wire | ESP32 Pin | Notes             |
| ---------- | ---- | --------- | ----------------- |
| GP0        | TX → | GPIO16    | Pico TX to ESP RX |
| GP1        | ← RX | GPIO17    | ESP TX to Pico RX |
| GND        | ——   | GND       | Common ground     |

> **No power connection.** Each board is powered by its own USB cable.
> Do NOT connect 3V3 between the two boards — both run 3.3V logic, but
> back-feeding power through GPIO can damage the voltage regulators.

## Pin Reference

### Pico W

GP0 and GP1 are UART0 by default in MicroPython. They sit at the top-left
corner of the board (pin 1 and pin 2), with GND on pin 3.

```
         USB
    ┌─────┴─────┐
  1 │ GP0 (TX)  │  ← wire to ESP32 GPIO16
  2 │ GP1 (RX)  │  ← wire from ESP32 GPIO17
  3 │ GND       │  ← wire to ESP32 GND
  4 │ GP2       │
    │    ...    │
    └───────────┘
```

### ESP32

GPIO16 and GPIO17 are the default UART2 pins. On most ESP32 dev boards they
are broken out and labeled. GND is available on multiple pins — use any one.

The UART pins are configurable via `menuconfig`:

```
Forest Link Protocol → UART Ingest API → TX GPIO  (default 17)
Forest Link Protocol → UART Ingest API → RX GPIO  (default 16)
```

## Setup

1. Flash the ESP32 with the FLP firmware (`./idf.sh flash`).
2. Copy `flp_client.py` to the Pico W (via Thonny, mpremote, or rshell).
3. Connect the three wires as shown above.
4. Power both boards via USB.

## Quick Test

On the Pico W, open a REPL (Thonny or `mpremote`) and run:

```python
from flp_client import FLPClient

client = FLPClient()
print(client.get_status())
client.send_file("hello.txt", b"Hello from Pico W!")
```

The ESP32 serial monitor should show:

```
I (uart_ingest): FILE_BEGIN: "hello.txt" (18 bytes)
I (uart_ingest): FILE_END: "hello.txt" received 18/18 bytes
```

## Troubleshooting

| Symptom                  | Fix                                                                        |
| ------------------------ | -------------------------------------------------------------------------- |
| Timeout waiting for ACK  | Check TX/RX aren't swapped. TX→RX means Pico GP0 goes to ESP32 GPIO16.     |
| Garbage / framing errors | Confirm both sides are 115200 baud, 8N1.                                   |
| No output on ESP32       | Make sure GND is connected. UART won't work without a common ground.       |
| NACK with error 0x01     | PSRAM allocation failed. Check the ESP32 has SPIRAM enabled in menuconfig. |
