"""
File Reassembler — Reconstructs files from received chunks with CRC verification.

Writes chunks into a pre-allocated buffer at the correct offset, tracks
received chunks via bitmap, and verifies integrity with CRC32.
"""

import binascii
import os

CHUNK_SIZE = 500  # matches FLP_BLE_MTU - PacketHeader(12) = 500
FEC_GROUP_SIZE = 7  # 7 data + 1 parity per group


class FecDecoder:
    """XOR parity FEC decoder — mirrors ESP32 FecDecoder."""

    def __init__(self):
        self.groups = {}  # group_num -> {slots: {idx: (data, is_parity)}, count: int}

    def ingest(self, seq, data, is_parity=False):
        """Feed a fragment. Returns (recovered_seq, recovered_data) or None."""
        group = seq // (FEC_GROUP_SIZE + 1)
        idx = seq % (FEC_GROUP_SIZE + 1)

        if group not in self.groups:
            self.groups[group] = {"slots": {}, "count": 0}

        g = self.groups[group]
        if idx not in g["slots"]:
            g["slots"][idx] = (bytes(data), is_parity)
            g["count"] += 1

        # Try recovery: need exactly K of K+1
        if g["count"] == FEC_GROUP_SIZE:
            return self._try_recover(group)
        return None

    def _try_recover(self, group):
        g = self.groups[group]
        # Find missing slot
        missing = None
        for i in range(FEC_GROUP_SIZE + 1):
            if i not in g["slots"]:
                if missing is not None:
                    return None  # more than one missing
                missing = i
        if missing is None:
            return None  # all present
        if missing == FEC_GROUP_SIZE:
            # Missing parity — all data present, nothing to recover
            return None

        # XOR all present slots
        max_len = max(len(d) for d, _ in g["slots"].values())
        recovered = bytearray(max_len)
        for idx, (data, _) in g["slots"].items():
            for j in range(len(data)):
                recovered[j] ^= data[j]

        recovered_seq = group * (FEC_GROUP_SIZE + 1) + missing
        return (recovered_seq, bytes(recovered))


class FileReassembler:
    def __init__(self, session_id, filename, total_size, chunk_count, expected_crc,
                 fragment_size=None):
        self.session_id = session_id
        self.filename = filename
        self.total_size = total_size
        self.chunk_count = chunk_count
        self.expected_crc = expected_crc
        self.chunk_size = fragment_size if fragment_size else CHUNK_SIZE
        self.buffer = bytearray(total_size)
        self.bitmap = bytearray((chunk_count + 7) // 8)
        self.chunks_received = 0
        self.fec = FecDecoder()

    def write_chunk(self, seq, data):
        """Write chunk at correct offset. Returns True if new chunk."""
        # Detect FEC mode: if chunk_count includes parity slots, enable FEC.
        # Pure data transfers (e.g. local-exit) have no parity — every seq
        # is a data fragment and the FEC group check must be skipped.
        data_frags = (self.total_size + self.chunk_size - 1) // self.chunk_size
        fec_active = self.chunk_count > data_frags

        if fec_active:
            is_parity = (seq % (FEC_GROUP_SIZE + 1) == FEC_GROUP_SIZE)

            # Feed to FEC decoder
            result = self.fec.ingest(seq, data, is_parity=is_parity)

            if is_parity:
                # Don't write parity to output buffer
                # But try FEC recovery
                if result:
                    rec_seq, rec_data = result
                    return self.write_chunk(rec_seq, rec_data)
                return False

            # Map seq to data index (skip parity slots in sequence)
            group = seq // (FEC_GROUP_SIZE + 1)
            idx_in_group = seq % (FEC_GROUP_SIZE + 1)
            data_idx = group * FEC_GROUP_SIZE + idx_in_group
        else:
            result = None
            data_idx = seq

        # Normal data fragment
        byte_idx = seq // 8
        bit_idx = seq % 8
        if self.bitmap[byte_idx] & (1 << bit_idx):
            return False  # duplicate

        offset = data_idx * self.chunk_size
        end = min(offset + len(data), self.total_size)
        if offset >= self.total_size:
            return False
        self.buffer[offset:end] = data[:end - offset]
        self.bitmap[byte_idx] |= (1 << bit_idx)
        self.chunks_received += 1

        # If FEC recovered a fragment (from a non-parity ingest), write it too
        if fec_active and result:
            rec_seq, rec_data = result
            self.write_chunk(rec_seq, rec_data)

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
