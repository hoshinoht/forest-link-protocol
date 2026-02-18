"""
File Reassembler — Reconstructs files from received chunks with CRC verification.

Writes chunks into a pre-allocated buffer at the correct offset, tracks
received chunks via bitmap, and verifies integrity with CRC32.
"""

import binascii
import os

CHUNK_SIZE = 500  # matches FLP_BLE_MTU - PacketHeader(12) = 500


class FileReassembler:
    def __init__(self, session_id, filename, total_size, chunk_count, expected_crc):
        self.session_id = session_id
        self.filename = filename
        self.total_size = total_size
        self.chunk_count = chunk_count
        self.expected_crc = expected_crc
        self.buffer = bytearray(total_size)
        self.bitmap = bytearray((chunk_count + 7) // 8)
        self.chunks_received = 0

    def write_chunk(self, seq, data):
        """Write chunk at correct offset. Returns True if new chunk."""
        byte_idx = seq // 8
        bit_idx = seq % 8
        if self.bitmap[byte_idx] & (1 << bit_idx):
            return False  # duplicate

        offset = seq * CHUNK_SIZE
        end = min(offset + len(data), self.total_size)
        self.buffer[offset:end] = data[:end - offset]
        self.bitmap[byte_idx] |= (1 << bit_idx)
        self.chunks_received += 1
        return True

    def is_complete(self):
        """Return True if all chunks have been received."""
        return self.chunks_received == self.chunk_count

    def verify_crc(self):
        """Verify CRC32 of reassembled data against expected value."""
        actual = binascii.crc32(
            bytes(self.buffer[:self.total_size])) & 0xFFFFFFFF
        return actual == self.expected_crc

    def save(self, output_dir="./received_files"):
        """Save reassembled file to disk."""
        os.makedirs(output_dir, exist_ok=True)
        path = os.path.join(output_dir, self.filename)
        with open(path, 'wb') as f:
            f.write(self.buffer[:self.total_size])
        return path

    def progress(self):
        """Return reassembly progress as a percentage."""
        return self.chunks_received / self.chunk_count * 100.0 if self.chunk_count > 0 else 0.0
