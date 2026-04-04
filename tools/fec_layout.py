#!/usr/bin/env python3
"""
fec_layout.py — Reproduce the FLP FEC encoding layout for a given file.

Mirrors the exact logic in transfer_engine.cpp and fec_codec.hpp:
  - Fragment size: (ESPNOW_MAX_PAYLOAD // 8) * 8 = 1456
  - FEC_GROUP_SIZE = 7 (7 data + 1 parity per full group)
  - Trailing incomplete group has NO parity slot
  - Parity = byte-wise XOR of all data fragments in the group
  - CRC32 matches esp_rom_crc32_le (standard CRC-32/ISO-HDLC)

Usage:
    python3 tools/fec_layout.py [input_file] [output_file]

Defaults:
    input_file  = demo-data/demo.txt
    output_file = demo-data/fec_layout.txt
"""

import os
import sys
import struct
import zlib
from pathlib import Path

# ── Constants (must match packet.hpp) ────────────────────────────────────

MAX_MTU = 1470
PACKET_HEADER_SIZE = 8  # sizeof(PacketHeader)
ESPNOW_MAX_PAYLOAD = MAX_MTU - PACKET_HEADER_SIZE  # 1462
TRANSFER_FRAGMENT_ALIGN = 8
FRAGMENT_SIZE = (
    ESPNOW_MAX_PAYLOAD // TRANSFER_FRAGMENT_ALIGN
) * TRANSFER_FRAGMENT_ALIGN  # 1456
FEC_GROUP_SIZE = 7  # 7 data + 1 parity


def read_fragment(data: bytes, data_idx: int) -> bytes:
    """Read one data fragment by its data-space index (skipping parity slots)."""
    offset = data_idx * FRAGMENT_SIZE
    if offset >= len(data):
        return b""
    end = min(offset + FRAGMENT_SIZE, len(data))
    return data[offset:end]


def xor_bytes(a: bytes, b: bytes) -> bytes:
    """XOR two byte strings, zero-extending the shorter one."""
    longer = max(len(a), len(b))
    a = a.ljust(longer, b"\x00")
    b = b.ljust(longer, b"\x00")
    return bytes(x ^ y for x, y in zip(a, b))


def compute_crc32(data: bytes) -> int:
    """CRC-32 matching esp_rom_crc32_le (standard CRC-32/ISO-HDLC)."""
    return zlib.crc32(data) & 0xFFFFFFFF


def main():
    repo_root = Path(__file__).resolve().parent.parent
    default_input = repo_root / "demo-data" / "demo.txt"
    default_output = repo_root / "demo-data" / "fec_layout.txt"

    input_path = Path(sys.argv[1]) if len(sys.argv) > 1 else default_input
    output_path = Path(sys.argv[2]) if len(sys.argv) > 2 else default_output

    if not input_path.exists():
        print(f"Error: {input_path} not found", file=sys.stderr)
        sys.exit(1)

    file_data = input_path.read_bytes()
    file_size = len(file_data)
    crc32 = compute_crc32(file_data)

    # ── Fragment count (mirrors transfer_engine.cpp) ─────────────────────

    data_frags = (file_size + FRAGMENT_SIZE - 1) // FRAGMENT_SIZE
    full_groups = data_frags // FEC_GROUP_SIZE
    remaining = data_frags % FEC_GROUP_SIZE

    # Full groups contribute (FEC_GROUP_SIZE + 1) seqs each.
    # Trailing incomplete group contributes only its data seqs (no parity).
    total_seqs = full_groups * (FEC_GROUP_SIZE + 1) + remaining

    # ── Build seq table ──────────────────────────────────────────────────

    lines = []
    lines.append(f"FLP FEC Layout for: {input_path.name}")
    lines.append(f"{'=' * 72}")
    lines.append(f"File size:      {file_size} bytes")
    lines.append(f"CRC-32:         0x{crc32:08X} ({crc32})")
    lines.append(f"Fragment size:  {FRAGMENT_SIZE} bytes")
    lines.append(f"Data fragments: {data_frags}")
    lines.append(f"FEC group size: {FEC_GROUP_SIZE} data + 1 parity")
    lines.append(f"Full groups:    {full_groups}")
    lines.append(f"Trailing frags: {remaining} (no parity)")
    lines.append(f"Total seqs:     {total_seqs}")
    lines.append(f"{'=' * 72}")
    lines.append("")

    hdr = (
        f"{'seq':>5}  {'type':>7}  {'group':>5}  {'idx':>3}  "
        f"{'data_idx':>8}  {'offset':>10}  {'length':>6}  {'crc32':>10}  "
        f"{'first_8_hex'}"
    )
    lines.append(hdr)
    lines.append("-" * len(hdr) + "-" * 20)

    parity_buf = bytearray(FRAGMENT_SIZE)
    parity_len = 0
    parity_count = 0

    for seq in range(total_seqs):
        group = seq // (FEC_GROUP_SIZE + 1)
        idx_in_group = seq % (FEC_GROUP_SIZE + 1)
        is_parity = idx_in_group == FEC_GROUP_SIZE

        if is_parity:
            # Parity fragment: XOR of all data fragments in this group
            frag_data = bytes(parity_buf[:parity_len])
            frag_type = "PARITY"
            data_idx_str = "-"
            offset_str = "-"

            # Reset encoder for next group
            parity_buf = bytearray(FRAGMENT_SIZE)
            parity_len = 0
            parity_count = 0
        else:
            # Data fragment
            data_idx = group * FEC_GROUP_SIZE + idx_in_group
            frag_data = read_fragment(file_data, data_idx)

            if len(frag_data) == 0:
                frag_type = "SKIP"
                data_idx_str = str(data_idx)
                offset_str = str(data_idx * FRAGMENT_SIZE)
            else:
                frag_type = "DATA"
                data_idx_str = str(data_idx)
                offset_str = str(data_idx * FRAGMENT_SIZE)

                # Accumulate parity (XOR)
                for i in range(len(frag_data)):
                    parity_buf[i] ^= frag_data[i]
                if len(frag_data) > parity_len:
                    parity_len = len(frag_data)
                parity_count += 1

        frag_len = len(frag_data)
        frag_crc = compute_crc32(frag_data) if frag_len > 0 else 0
        first_8 = frag_data[:8].hex() if frag_len > 0 else ""

        lines.append(
            f"{seq:>5}  {frag_type:>7}  {group:>5}  {idx_in_group:>3}  "
            f"{data_idx_str:>8}  {offset_str:>10}  {frag_len:>6}  "
            f"0x{frag_crc:08X}  {first_8}"
        )

    # ── Summary ──────────────────────────────────────────────────────────

    data_count = sum(
        1 for s in range(total_seqs) if s % (FEC_GROUP_SIZE + 1) != FEC_GROUP_SIZE
    )
    parity_total = full_groups
    lines.append("")
    lines.append(f"{'=' * 72}")
    lines.append(
        f"Summary: {data_count} data + {parity_total} parity = {total_seqs} total seqs"
    )
    lines.append(
        f"Overhead: {parity_total / data_count * 100:.1f}% ({parity_total} parity fragments)"
    )

    output_text = "\n".join(lines) + "\n"

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(output_text)
    print(f"Written {len(lines)} lines to {output_path}")
    print(f"  {data_count} data + {parity_total} parity = {total_seqs} total seqs")
    print(f"  CRC-32: 0x{crc32:08X}")


if __name__ == "__main__":
    main()
