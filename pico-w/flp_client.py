"""
FLP UART Client for Raspberry Pi Pico W

Sends files to an ESP32 FLP node over UART for mesh transfer.

Wiring:
    Pico W GP0 (UART0 TX) --> ESP32 GPIO16 (UART2 RX)
    Pico W GP1 (UART0 RX) <-- ESP32 GPIO17 (UART2 TX)
    Pico W GND -------------- ESP32 GND
"""

import struct
import time
from machine import UART, Pin

# Frame commands
CMD_FILE_BEGIN = 0x01
CMD_FILE_DATA  = 0x02
CMD_FILE_END   = 0x03
CMD_STATUS     = 0x04

RESP_ACK         = 0x80
RESP_NACK        = 0x81
RESP_STATUS_RESP = 0x82

SYNC1 = 0xAA
SYNC2 = 0x55

CHUNK_SIZE = 1024


class FLPClient:
    def __init__(self, uart_id=0, tx_pin=0, rx_pin=1, baud=115200):
        self.uart = UART(uart_id, baudrate=baud, tx=Pin(tx_pin), rx=Pin(rx_pin))

    def _send_frame(self, cmd, payload=b""):
        header = struct.pack("<BBBh", SYNC1, SYNC2, cmd, len(payload))
        self.uart.write(header)
        if payload:
            self.uart.write(payload)

    def _read_frame(self, timeout_ms=2000):
        """Read a response frame. Returns (cmd, payload) or None on timeout."""
        deadline = time.ticks_add(time.ticks_ms(), timeout_ms)
        state = 0  # 0=SYNC1, 1=SYNC2, 2=CMD, 3=LEN_LO, 4=LEN_HI, 5=PAYLOAD
        cmd = 0
        length = 0
        buf = bytearray()

        while time.ticks_diff(deadline, time.ticks_ms()) > 0:
            b = self.uart.read(1)
            if b is None:
                time.sleep_ms(1)
                continue
            b = b[0]

            if state == 0:
                if b == SYNC1:
                    state = 1
            elif state == 1:
                state = 2 if b == SYNC2 else 0
            elif state == 2:
                cmd = b
                state = 3
            elif state == 3:
                length = b
                state = 4
            elif state == 4:
                length |= b << 8
                if length == 0:
                    return (cmd, b"")
                buf = bytearray()
                state = 5
            elif state == 5:
                buf.append(b)
                if len(buf) >= length:
                    return (cmd, bytes(buf))

        return None

    def _expect_ack(self, for_cmd, timeout_ms=2000):
        resp = self._read_frame(timeout_ms)
        if resp is None:
            raise RuntimeError("Timeout waiting for ACK")
        cmd, payload = resp
        if cmd == RESP_NACK:
            err = payload[1] if len(payload) > 1 else 0xFF
            raise RuntimeError(f"NACK for 0x{for_cmd:02X}, error=0x{err:02X}")
        if cmd != RESP_ACK:
            raise RuntimeError(f"Unexpected response: 0x{cmd:02X}")

    def send_file(self, filename, data):
        """Send a file to the ESP32 for mesh transfer."""
        if isinstance(data, str):
            data = data.encode()
        if isinstance(filename, str):
            filename = filename.encode()

        # FILE_BEGIN: [file_size:4 LE][filename\0]
        payload = struct.pack("<I", len(data)) + filename + b"\x00"
        self._send_frame(CMD_FILE_BEGIN, payload)
        self._expect_ack(CMD_FILE_BEGIN)
        print(f"FILE_BEGIN accepted: {filename.decode()} ({len(data)} bytes)")

        # FILE_DATA: send in chunks
        offset = 0
        while offset < len(data):
            chunk = data[offset:offset + CHUNK_SIZE]
            self._send_frame(CMD_FILE_DATA, chunk)
            self._expect_ack(CMD_FILE_DATA)
            offset += len(chunk)
            print(f"  sent {offset}/{len(data)} bytes")

        # FILE_END
        self._send_frame(CMD_FILE_END)
        self._expect_ack(CMD_FILE_END, timeout_ms=5000)
        print("FILE_END acknowledged — mesh transfer initiated")

    def get_status(self):
        """Query node status. Returns dict with node info."""
        self._send_frame(CMD_STATUS)
        resp = self._read_frame(timeout_ms=2000)
        if resp is None:
            raise RuntimeError("Timeout waiting for STATUS_RESP")
        cmd, payload = resp
        if cmd != RESP_STATUS_RESP or len(payload) < 5:
            raise RuntimeError(f"Unexpected response: 0x{cmd:02X}")
        return {
            "has_internet":    bool(payload[0]),
            "neighbors":       payload[1],
            "transfer_active": bool(payload[2]),
            "node_addr":       payload[3] | (payload[4] << 8),
        }


# --- Demo usage ---
if __name__ == "__main__":
    client = FLPClient()

    # Query status
    status = client.get_status()
    print("Node status:", status)

    # Send a test file
    test_data = b"Hello from Pico W! Forest Link Protocol test payload."
    client.send_file("test.txt", test_data)
