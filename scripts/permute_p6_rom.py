#!/usr/bin/env python3
"""Permute the apple2js-layout Disk II P6 PROM bytes into MAME's layout.

Background
----------
The Disk II Logic State Sequencer is a 256x8 PROM (Apple part 341-0028-A)
indexed by an 8-bit address composed from (current state, Q7, Q6, QA, !PULSE).
The PROM contents are the same regardless of how those signals are arranged
into the 8 address pins, but the BYTES MUST BE STORED at the address that
the decoder will compute. Two well-known layouts exist for the same logical
PROM:

  apple2js layout (used by POM2 historically):
    bit 7..4 = state[3..0] (sequential)
    bit 3    = Q7  (mode_write)
    bit 2    = Q6  (mode_load)
    bit 1    = QA  (data_reg MSB)
    bit 0    = !PULSE (no flux transition this cell)

  MAME layout (faithful to the real 341-0028-A wiring; see
  src/devices/machine/wozfdc.cpp lss_sync()):
    bit 7    = state[3]
    bit 6    = state[2]
    bit 5    = state[0]   <-- swapped with state[1]
    bit 4    = !PULSE     <-- moved from bit 0
    bit 3    = Q7
    bit 2    = Q6
    bit 1    = QA
    bit 0    = state[1]   <-- swapped with state[0]

This script reads roms/diskii_p6.rom (apple2js layout, the file POM2 ships
with), permutes the bytes into MAME layout, and prints them as a C array
initializer ready to paste into DiskIICard.cpp's kP6RomDefault[].

Run from repo root:
    python3 scripts/permute_p6_rom.py            # print C array
    python3 scripts/permute_p6_rom.py --write    # also overwrite the .rom file
    python3 scripts/permute_p6_rom.py --inverse  # MAME -> apple2js (debug)
"""

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE = REPO_ROOT / "roms" / "diskii_p6.rom"


def apple2js_addr(state: int, q7: int, q6: int, qa: int, npulse: int) -> int:
    return (state << 4) | (q7 << 3) | (q6 << 2) | (qa << 1) | npulse


def mame_addr(state: int, q7: int, q6: int, qa: int, npulse: int) -> int:
    s3 = (state >> 3) & 1
    s2 = (state >> 2) & 1
    s1 = (state >> 1) & 1
    s0 = (state >> 0) & 1
    return (s3 << 7) | (s2 << 6) | (s0 << 5) | (npulse << 4) \
         | (q7 << 3) | (q6 << 2) | (qa << 1) | (s1 << 0)


def permute(src_bytes: bytes, *, src_layout: str, dst_layout: str) -> bytes:
    encoders = {"apple2js": apple2js_addr, "mame": mame_addr}
    enc_src, enc_dst = encoders[src_layout], encoders[dst_layout]
    out = bytearray(256)
    seen = [False] * 256
    for state in range(16):
        for q7 in (0, 1):
            for q6 in (0, 1):
                for qa in (0, 1):
                    for npulse in (0, 1):
                        a = enc_src(state, q7, q6, qa, npulse)
                        m = enc_dst(state, q7, q6, qa, npulse)
                        if seen[m]:
                            raise RuntimeError(
                                f"dst address {m:#x} written twice — "
                                f"permutation is not bijective"
                            )
                        seen[m] = True
                        out[m] = src_bytes[a]
    if not all(seen):
        missing = [i for i, v in enumerate(seen) if not v]
        raise RuntimeError(f"dst addresses not covered: {missing}")
    return bytes(out)


def format_c_array(data: bytes) -> str:
    lines = []
    for row in range(16):
        bytes_row = data[row * 16 : (row + 1) * 16]
        cells = ", ".join(f"0x{b:02X}" for b in bytes_row)
        lines.append(f"    {cells}, // row {row:X}")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--write", action="store_true",
                        help="overwrite roms/diskii_p6.rom with the result")
    parser.add_argument("--inverse", action="store_true",
                        help="permute MAME -> apple2js instead of apple2js -> MAME")
    args = parser.parse_args()

    if not SOURCE.exists():
        print(f"error: {SOURCE} not found", file=sys.stderr)
        return 1
    raw = SOURCE.read_bytes()
    if len(raw) != 256:
        print(f"error: expected 256 bytes, got {len(raw)}", file=sys.stderr)
        return 1

    src, dst = ("mame", "apple2js") if args.inverse else ("apple2js", "mame")
    permuted = permute(raw, src_layout=src, dst_layout=dst)

    if args.write:
        SOURCE.write_bytes(permuted)
        print(f"wrote {SOURCE} ({src} -> {dst})", file=sys.stderr)

    print("// MAME-layout P6 PROM bytes (Apple part 341-0028-A).")
    print("// Generated from roms/diskii_p6.rom (apple2js layout) by")
    print("// scripts/permute_p6_rom.py — re-run if the source ROM changes.")
    print("// Same 256 bytes as apple2js, indexed by MAME's wozfdc.cpp")
    print("// scrambled address scheme: bits [7,6,5,0] = state[3,2,0,1],")
    print("// bit 4 = !PULSE, bit 3 = Q7, bit 2 = Q6, bit 1 = QA.")
    print("constexpr uint8_t kP6RomDefault[256] = {")
    print(format_c_array(permuted))
    print("};")
    return 0


if __name__ == "__main__":
    sys.exit(main())
