"""
Selective Repeat ARQ — Cloud side (FR-MQTT2).

Tracks received chunks via bitmap, sends ACK for each received chunk,
and NACKs gaps within the sliding window.
"""

import time


class CloudSelectiveRepeat:
    def __init__(self, window_size=8, timeout_sec=5.0):
        self.window_size = window_size
        self.timeout_sec = timeout_sec
        self.expected_base = 0
        self.received_bitmap = None  # initialized when transfer starts
        self.total_chunks = 0
        self.last_received_time = {}  # seq -> timestamp
        self.ack_callback = None  # set by mqtt_admin to publish ACK/NACK

    def start_session(self, total_chunks):
        """Initialize state for a new file transfer session."""
        self.total_chunks = total_chunks
        bitmap_size = (total_chunks + 7) // 8
        self.received_bitmap = bytearray(bitmap_size)
        self.expected_base = 0
        self.last_received_time = {}

    def on_chunk_received(self, seq_num):
        """Mark chunk as received, send ACK, detect gaps and NACK."""
        # Set bit in bitmap
        byte_idx = seq_num // 8
        bit_idx = seq_num % 8
        self.received_bitmap[byte_idx] |= (1 << bit_idx)
        self.last_received_time[seq_num] = time.time()

        # Always ACK received chunk
        if self.ack_callback:
            self.ack_callback("ACK", seq_num)

        # Advance expected_base
        while self.expected_base < self.total_chunks:
            b = self.expected_base // 8
            bit = self.expected_base % 8
            if self.received_bitmap[b] & (1 << bit):
                self.expected_base += 1
            else:
                break

        # Check for gaps within window — NACK missing chunks
        for i in range(self.expected_base, min(self.expected_base + self.window_size, self.total_chunks)):
            b = i // 8
            bit = i % 8
            if not (self.received_bitmap[b] & (1 << bit)):
                if self.ack_callback:
                    self.ack_callback("NACK", i)

    def check_timeouts(self):
        """Check for chunks not received within timeout, send NACK."""
        now = time.time()
        for seq in range(self.expected_base, min(self.expected_base + self.window_size, self.total_chunks)):
            b = seq // 8
            bit = seq % 8
            if not (self.received_bitmap[b] & (1 << bit)):
                # Not received — NACK if we haven't recently
                last = self.last_received_time.get(seq)
                if last is None or (now - last) > self.timeout_sec:
                    if self.ack_callback:
                        self.ack_callback("NACK", seq)

    def is_complete(self):
        """Return True if all chunks have been received."""
        return self.expected_base >= self.total_chunks

    def progress(self):
        """Return transfer progress as a percentage."""
        if self.total_chunks == 0:
            return 0.0
        received = sum(bin(b).count('1') for b in self.received_bitmap) if self.received_bitmap else 0
        return received / self.total_chunks * 100.0
