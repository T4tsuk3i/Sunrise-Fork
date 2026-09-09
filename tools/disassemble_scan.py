#!/usr/bin/env python3
"""Decodes `ev=stat_scan stage=code` byte dumps that Sunrise writes to its log.

The game image is VMProtect-encrypted on disk; the plaintext only ever exists inside the running
process, so `stat_block_scan` carries the bytes out through the log and this tool turns them back
into x86-64 instructions with RVAs restored.

Usage:
    python tools/disassemble_scan.py <sunrise.log> [--rva X] [--all]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import capstone

CODE_RE = re.compile(
    r"ev=stat_scan stage=code label=(\S+) rva=0x([0-9A-Fa-f]+) lead=(\d+) b=([0-9A-Fa-f]+)"
)

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="path to sunrise.log")
    parser.add_argument(
        "--rva",
        type=lambda s: int(s, 16),
        default=None,
        help="only decode dumps whose rva equals this (hex)",
    )
    parser.add_argument(
        "--all", action="store_true", help="decode every dump, not just unique rvas"
    )
    return parser.parse_args()


def decode(rva: int, lead: int, hexbytes: str) -> list[str]:
    data = bytes.fromhex(hexbytes)
    if lead > len(data):
        lead = len(data)
    origin = rva + lead
    lines = []
    for insn in md.disasm(data, rva):
        marker = "*" if insn.address == origin else " "
        lines.append(f"{marker} {insn.address:#010x}  {insn.mnemonic:<8} {insn.op_str}")
    return lines


def main() -> int:
    args = parse_args()
    if not args.log.exists():
        print(f"no such file: {args.log}", file=sys.stderr)
        return 1
    seen = set()
    matches = 0
    for raw in args.log.read_text(encoding="utf-8", errors="replace").splitlines():
        m = CODE_RE.search(raw)
        if not m:
            continue
        label, rva_s, lead_s, hexbytes = m.groups()
        rva = int(rva_s, 16)
        lead = int(lead_s)
        if args.rva is not None and args.rva != rva:
            continue
        matches += 1
        if not args.all and rva in seen:
            continue
        seen.add(rva)
        print(f"--- label={label} rva=0x{rva:X} lead={lead} ---")
        try:
            for line in decode(rva, lead, hexbytes):
                print("   " + line)
        except ValueError as error:
            # A dump can land truncated if the log line hit its 1024-byte cap; skip it rather
            # than losing every dump after it in the same run.
            print(f"   (skipped: malformed hex dump - {error})")
    print(f"({matches} dump lines matched, {len(seen)} shown)")
    return 0


if __name__ == "__main__":
    sys.exit(main())