#!/usr/bin/env python3
"""Artifact gate: prove the compiled C3 firmware configures the LR2021 SPI clock
at <= 16 MHz (the LR2021 datasheet maximum).

Companion to ``tests/test_c3_spi_clock.py`` (which gates the *source*). This one
gates the *binary*: it reconstructs the integer constants the compiler materialises
in the flrc-bench ELF and asserts that the clock handed to ``spi_bus_add_device()``
is 16 000 000 and that neither historical violation (18 MHz, 40 MHz) survives.

Why not DWARF: on ESP32-C3 (RISC-V) GCC splits the constant into a ``lui`` +
``addi`` pair and this build emits no ``DW_AT_GNU_call_site_value`` DIE holding
the struct field, so ``readelf --debug-dump=info | grep 16000000`` yields nothing
even when the code is correct (unlike the Xtensa/other paths where it works).
The disassembly is therefore the authoritative check.

Usage:
    python3 tools/verify_c3_spi_clock_elf.py <path/to/flrc-bench-espidf.elf> \
        [--objdump riscv32-esp-elf-objdump]

Exit 0 = compliant, 1 = violation, 2 = could not verify.
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys

EXPECTED_HZ = 16_000_000
DATASHEET_MAX_HZ = 16_000_000
HISTORICAL_VIOLATIONS = {
    18_000_000: "18 MHz legacy hard-coded literal",
    40_000_000: "40 MHz GDMA HAL (balloon-fresh P0.4)",
}

LUI_RE = re.compile(r"^\s*[0-9a-f]+:\s+[0-9a-f]+\s+lui\s+(\w+),0x([0-9a-f]+)")
ADDI_RE = re.compile(
    r"^\s*[0-9a-f]+:\s+[0-9a-f]+\s+addi\s+(\w+),(\w+),(-?\d+)"
)
CALL_RE = re.compile(r"^\s*([0-9a-f]+):\s+[0-9a-f]+\s+jal\s+[0-9a-f]+ <spi_bus_add_device>")


def raw_byte_violations(elf: bytes) -> list[str]:
    """Any 32-bit LE occurrence of a historical clock value is a red flag."""
    bad = []
    for hz, why in HISTORICAL_VIOLATIONS.items():
        n = elf.count(struct.pack("<I", hz))
        if n:
            bad.append(f"raw ELF bytes contain {hz} ({why}) x{n}")
    return bad


def disassemble(elf_path: str, objdump: str) -> str:
    return subprocess.run(
        [objdump, "-d", "-C", elf_path],
        check=True,
        capture_output=True,
        text=True,
    ).stdout


def reconstructed_constants(dis: str) -> set[int]:
    """Integer constants the compiler materialises as ``lui`` + ``addi`` pairs."""
    lines = dis.splitlines()
    consts: set[int] = set()
    for i, line in enumerate(lines):
        m = LUI_RE.match(line)
        if not m:
            continue
        reg, imm20 = m.group(1), int(m.group(2), 16)
        for nxt in lines[i + 1 : i + 5]:
            a = ADDI_RE.match(nxt)
            if a and a.group(2) == reg:
                value = (imm20 << 12) + int(a.group(3))
                if 0 < value <= 0xFFF_FFFF:
                    consts.add(value)
                break
    return consts


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument(
        "--objdump",
        default=os.environ.get("OBJDUMP", "riscv32-esp-elf-objdump"),
        help="RISC-V objdump binary (default: riscv32-esp-elf-objdump)",
    )
    args = ap.parse_args()

    if not os.path.isfile(args.elf):
        print(f"FAIL: no such ELF: {args.elf}")
        return 2

    with open(args.elf, "rb") as fh:
        blob = fh.read()

    problems = raw_byte_violations(blob)

    try:
        dis = disassemble(args.elf, args.objdump)
    except (FileNotFoundError, subprocess.CalledProcessError) as exc:
        print(f"WARN: disassembly unavailable ({exc}); raw-byte check only")
        if problems:
            print("\n".join(f"  - {p}" for p in problems))
            return 1
        print("OK (raw bytes only): no 18 MHz / 40 MHz constant in the ELF")
        return 0

    consts = reconstructed_constants(dis)
    for hz, why in HISTORICAL_VIOLATIONS.items():
        if hz in consts:
            problems.append(f"disassembly materialises {hz} ({why})")

    # Locate the clock store feeding spi_bus_add_device().
    lines = dis.splitlines()
    call_idx = [i for i, line in enumerate(lines) if CALL_RE.match(line)]
    call_ok = False
    for idx in call_idx:
        window = lines[max(0, idx - 40) : idx]
        values = set()
        for j, line in enumerate(window):
            m = LUI_RE.match(line)
            if not m:
                continue
            reg, imm20 = m.group(1), int(m.group(2), 16)
            for nxt in window[j + 1 : j + 5]:
                a = ADDI_RE.match(nxt)
                if a and a.group(2) == reg:
                    values.add((imm20 << 12) + int(a.group(3)))
                    break
        if EXPECTED_HZ in values and ("sw" in " ".join(window)):
            call_ok = True
            break

    if not call_ok:
        problems.append(
            f"no {EXPECTED_HZ} constant found in the instruction window leading to "
            f"a call of spi_bus_add_device() (call sites found: {len(call_idx)})"
        )

    print(f"ELF:                {args.elf}")
    print(f"spi_bus_add_device call sites: {len(call_idx)}")
    print(f"constants reconstructed: {len(consts)}")
    print(f"expected clock in call window: {EXPECTED_HZ} -> {'yes' if call_ok else 'NO'}")

    if problems:
        print("\nVIOLATIONS:")
        for p in problems:
            print(f"  - {p}")
        return 1

    print(
        f"\nOK: firmware configures the LR2021 SPI clock at {EXPECTED_HZ} Hz "
        f"(<= {DATASHEET_MAX_HZ} Hz datasheet max); no 18 MHz / 40 MHz constant."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
