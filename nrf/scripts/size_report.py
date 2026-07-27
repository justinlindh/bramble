#!/usr/bin/env python3
"""RAM/flash budget gate for the Bramble nRF52840 target (P0 exit gate).

Sums every ELF section placed in RAM (0x20000000 window: .data, .bss, any
.heap/.stack the linker script emits) plus the MSP stack reserved by the nrfx
linker script (__StackLimit..__StackTop, which lives at the top of RAM and is
not a section), compares the total against the budget, and fails the build
when over. Prints the top .bss symbols so shrink work has a target list.

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


def ram_sections(elf):
    sections = {}
    for line in run("size", "-A", "-x", elf).splitlines():
        m = re.match(r"^(\S+)\s+(0x[0-9a-f]+|\d+)\s+(0x[0-9a-f]+|\d+)$", line.strip())
        if not m:
            continue
        name, size, addr = m.group(1), int(m.group(2), 0), int(m.group(3), 0)
        if RAM_BASE <= addr < RAM_BASE + RAM_SIZE and size > 0:
            sections[name] = size
    return sections


def flash_usage(elf):
    total = 0
    for line in run("size", "-A", "-x", elf).splitlines():
        m = re.match(r"^(\S+)\s+(0x[0-9a-f]+|\d+)\s+(0x[0-9a-f]+|\d+)$", line.strip())
        if not m:
            continue
        addr, size = int(m.group(3), 0), int(m.group(2), 0)
        if addr < RAM_BASE and size > 0 and addr >= 0:
            name = m.group(1)
            if name not in (".comment", ".debug_info", ".ARM.attributes") and not name.startswith(
                ".debug"
            ):
                total += size
    return total


def msp_stack(elf):
    syms = {}
    for line in run("nm", elf).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in ("__StackTop", "__StackLimit"):
            syms[parts[2]] = int(parts[0], 16)
    if "__StackTop" in syms and "__StackLimit" in syms:
        return syms["__StackTop"] - syms["__StackLimit"]
    return 0


def top_bss_symbols(elf):
    out = []
    for line in run("nm", "-S", "--size-sort", "--reverse-sort", elf).splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2].lower() in ("b", "d"):
            size = int(parts[1], 16)
            if size > RAM_SIZE:
                continue  # linker bookkeeping absolutes, not real objects
            out.append((size, parts[2], parts[3]))
        if len(out) >= TOP_SYMBOLS:
            break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--budget-kb", type=int, default=200)
    ap.add_argument("--json")
    args = ap.parse_args()

    sections = ram_sections(args.elf)
    stack = msp_stack(args.elf)
    # .stack_dummy is the nrfx linker's placeholder for the same region the
    # __StackLimit/__StackTop symbols describe; keep exactly one accounting.
    if ".stack_dummy" in sections and stack > 0:
        del sections[".stack_dummy"]
    ram_total = sum(sections.values()) + stack
    budget = args.budget_kb * 1024
    flash = flash_usage(args.elf)

    print("=== bramble-nrf memory report ===")
    for name, size in sorted(sections.items(), key=lambda kv: -kv[1]):
        print(f"  {name:<12} {size:>8} bytes")
    print(f"  {'MSP stack':<12} {stack:>8} bytes (linker-reserved, not a section)")
    print(f"  RAM total  {ram_total:>8} / {RAM_SIZE} bytes ({100 * ram_total / RAM_SIZE:.1f}%)")
    print(f"  RAM budget {budget:>8} bytes (gate)")
    print(f"  Flash      {flash:>8} / {FLASH_SIZE} bytes ({100 * flash / FLASH_SIZE:.1f}%)")
    print(f"  Top {TOP_SYMBOLS} static symbols:")
    for size, kind, name in top_bss_symbols(args.elf):
        print(f"    {size:>8} {kind} {name}")

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
