#!/usr/bin/env python3
"""RAM/flash budget gate for the Bramble nRF52840 target (P0 exit gate).

Sums every ELF section placed in RAM (0x20000000 window: .data, .bss, any
.heap the linker script emits) plus the MSP stack reserved by the nrfx linker
script (__StackLimit..__StackTop), compares the total against the budget, and
fails the build when over. Prints the top .bss symbols so shrink work has a
target list.

Usage: size_report.py ELF [--budget-kb 200] [--json PATH]
"""

import argparse
import json
import re
import subprocess
import sys

RAM_BASE = 0x20000000
RAM_SIZE = 256 * 1024
FLASH_SIZE = 1024 * 1024
TOP_SYMBOLS = 20


def run(tool, *args):
    return subprocess.run(
        ["arm-none-eabi-" + tool, *args], check=True, capture_output=True, text=True
    ).stdout


def parse_sections(elf):
    """One `size -A` invocation parsed into (name, size, addr) rows."""
    rows = []
    for line in run("size", "-A", "-x", elf).splitlines():
        m = re.match(r"^(\S+)\s+(0x[0-9a-f]+|\d+)\s+(0x[0-9a-f]+|\d+)$", line.strip())
        if m:
            rows.append((m.group(1), int(m.group(2), 0), int(m.group(3), 0)))
    return rows


def parse_symbols(elf):
    """One `nm -S` invocation: (addr, size, kind, name) rows. Symbols without
    a size field (linker absolutes like __StackTop) get size 0."""
    rows = []
    for line in run("nm", "-S", elf).splitlines():
        parts = line.split()
        if len(parts) == 4:
            rows.append((int(parts[0], 16), int(parts[1], 16), parts[2], parts[3]))
        elif len(parts) == 3:
            rows.append((int(parts[0], 16), 0, parts[1], parts[2]))
    rows.sort(key=lambda r: -r[1])
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--budget-kb", type=int, default=200)
    ap.add_argument("--json")
    args = ap.parse_args()

    rows = parse_sections(args.elf)
    sections = {
        name: size for name, size, addr in rows if RAM_BASE <= addr < RAM_BASE + RAM_SIZE and size
    }
    # Flash from Berkeley text+data: summing System V rows by address both
    # misses .data's load image (its VMA is RAM but its initializers live in
    # flash) and risks counting non-allocated sections on some toolchains.
    berkeley = run("size", args.elf).splitlines()[-1].split()
    flash = int(berkeley[0]) + int(berkeley[1])

    symbols = parse_symbols(args.elf)
    stack_syms = {name: addr for addr, _size, _k, name in symbols if name in ("__StackTop", "__StackLimit")}
    stack = stack_syms.get("__StackTop", 0) - stack_syms.get("__StackLimit", 0)
    # .stack_dummy is the nrfx linker's placeholder for the same region the
    # __StackLimit/__StackTop symbols describe; keep exactly one accounting.
    if ".stack_dummy" in sections and stack > 0:
        del sections[".stack_dummy"]

    ram_total = sum(sections.values()) + stack
    budget = args.budget_kb * 1024

    print("=== bramble-nrf memory report ===")
    for name, size in sorted(sections.items(), key=lambda kv: -kv[1]):
        print(f"  {name:<12} {size:>8} bytes")
    print(f"  {'MSP stack':<12} {stack:>8} bytes (linker-reserved, not a section)")
    print(f"  RAM total  {ram_total:>8} / {RAM_SIZE} bytes ({100 * ram_total / RAM_SIZE:.1f}%)")
    print(f"  RAM budget {budget:>8} bytes (gate)")
    print(f"  Flash      {flash:>8} / {FLASH_SIZE} bytes ({100 * flash / FLASH_SIZE:.1f}%)")
    print(f"  Top {TOP_SYMBOLS} static symbols:")
    shown = 0
    for _addr, size, kind, name in symbols:
        if kind.lower() in ("b", "d") and size <= RAM_SIZE:
            print(f"    {size:>8} {kind} {name}")
            shown += 1
            if shown >= TOP_SYMBOLS:
                break

    if args.json:
        with open(args.json, "w") as f:
            json.dump(
                {
                    "sections": sections,
                    "msp_stack": stack,
                    "ram_total": ram_total,
                    "ram_budget": budget,
                    "flash_total": flash,
                },
                f,
                indent=2,
            )

    if ram_total > budget:
        print(
            f"FAIL: RAM demand {ram_total} exceeds budget {budget} "
            f"(over by {ram_total - budget} bytes). Shrink before proceeding; "
            "do not raise the budget (P1/P2 still need the slack).",
            file=sys.stderr,
        )
        return 1
    print(f"OK: {budget - ram_total} bytes under budget")
    return 0


if __name__ == "__main__":
    sys.exit(main())
