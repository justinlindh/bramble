#!/usr/bin/env python3
"""RAM/flash budget gate for the Bramble nRF52840 target.

Sums every ELF section placed in RAM (0x20000000 window: .data, .bss, any
.heap the linker script emits) plus the MSP stack reserved by the nrfx linker
script (__StackLimit..__StackTop), compares the total against the budget, and
fails the build when over. Prints the top .bss symbols so shrink work has a
target list.

The total covers the whole chip because the heap is a section: ucHeap is the
target's only pool (shim/malloc_freertos.c routes newlib's malloc to it), so
every allocation is inside a number this script can see. That was not true
before: plain calloc used to come from newlib's separate sbrk heap, which is
not a section and which the stock unbounded _sbrk let run past the end of
RAM, so this gate cheerfully reported 77% of a chip that was already ~14KB
over. Anything that reintroduces a second allocator reintroduces the blind
spot.

Because ucHeap dominates the total, a single total would only prove the image
fits, which the linker already enforces. So there are three gates. The total
must fit the chip with a margin; the STATIC half (everything that is not the
heap) has its own ceiling, which is the original "static data stays small"
pressure; and the heap has a FLOOR. The floor is the one that matters least
obviously and most in practice: without it, landing new statics and then
trimming configTOTAL_HEAP_SIZE to compensate turns the build green while
moving the shortfall to runtime, where this script cannot see it at all.

Runtime headroom within the heap is still a separate question, answered by
the free-heap figure in the boot log and heartbeats.

Every number here is stamped with the compiler that produced it. The margins on
this target are single-digit bytes, and GCC versions disagree by more than that
on the same source, so a report without a toolchain on it is not a number anyone
can compare to CI's. The version pin lives in `.arm-gcc-version` at the repo
root. Matching it is necessary but not sufficient for byte-exact agreement with
CI, since the bundled newlib differs between distributions of the same GCC; see
nrf/README.md for the container that reproduces CI's counts.

Usage: size_report.py ELF [--budget-kb 252] [--static-budget-kb 104]
                          [--heap-floor-kb 144] [--json PATH]
                          [--toolchain VERSION]
"""

import argparse
import json
import pathlib
import re
import subprocess
import sys

RAM_BASE = 0x20000000
RAM_SIZE = 256 * 1024
FLASH_SIZE = 1024 * 1024
TOP_SYMBOLS = 20
# nrf/scripts/size_report.py -> repo root.
PIN_FILE = pathlib.Path(__file__).resolve().parents[2] / ".arm-gcc-version"


def run(tool, *args):
    return subprocess.run(
        ["arm-none-eabi-" + tool, *args], check=True, capture_output=True, text=True
    ).stdout


def toolchain_version(explicit):
    """The compiler version behind these numbers. CMake passes the version it
    actually compiled with; the probe is the fallback for a hand-run report."""
    if explicit:
        return explicit
    try:
        return run("gcc", "-dumpversion").strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def pinned_version():
    try:
        return PIN_FILE.read_text().strip()
    except OSError:
        return ""


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
    # Two gates, because one total cannot catch both regressions. See the
    # module docstring.
    ap.add_argument("--budget-kb", type=int, default=252)
    # 104: raised from 100 when the NimBLE msys pool went 12 -> 24 blocks
    # (3.5KB of statics) after a real phone session exhausted the smaller
    # pool. The heap floor below is the real gate; this one only
    # catches static creep, and 104 restores the same ~2KB of slack the 100
    # figure had when it was set.
    ap.add_argument("--static-budget-kb", type=int, default=104)
    ap.add_argument("--heap-floor-kb", type=int, default=144)
    ap.add_argument("--json")
    ap.add_argument(
        "--toolchain",
        help="arm-none-eabi-gcc version that produced the ELF; probed from PATH when omitted",
    )
    args = ap.parse_args()

    toolchain = toolchain_version(args.toolchain)
    pin = pinned_version()
    toolchain_matches_pin = bool(pin) and toolchain == pin
    if not pin:
        toolchain_note = "no pin found in .arm-gcc-version"
    elif toolchain_matches_pin:
        toolchain_note = f"matches the CI version pin {pin}"
    else:
        toolchain_note = f"DOES NOT match the CI pin {pin}, so these bytes are not CI's bytes"

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

    # ucHeap is the whole runtime heap (shim/malloc_freertos.c routes newlib
    # there too), so splitting it out of the total is what separates "static
    # data grew" from "the heap shrank to hide it".
    heap = next((size for _addr, size, _k, name in symbols if name == "ucHeap"), 0)
    static_ram = ram_total - heap
    static_budget = args.static_budget_kb * 1024
    heap_floor = args.heap_floor_kb * 1024

    print("=== bramble-nrf memory report ===")
    print(f"  Toolchain  arm-none-eabi-gcc {toolchain} ({toolchain_note})")
    for name, size in sorted(sections.items(), key=lambda kv: -kv[1]):
        print(f"  {name:<12} {size:>8} bytes")
    print(f"  {'MSP stack':<12} {stack:>8} bytes (linker-reserved, not a section)")
    print(f"  RAM total  {ram_total:>8} / {RAM_SIZE} bytes ({100 * ram_total / RAM_SIZE:.1f}%)")
    print(f"  RAM budget {budget:>8} bytes (gate)")
    print(f"  of which heap (ucHeap) {heap:>8} bytes (floor {heap_floor})")
    print(f"  static (everything else) {static_ram:>8} bytes (gate {static_budget})")
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
                    "toolchain": toolchain,
                    "toolchain_pin": pin,
                    "toolchain_matches_pin": toolchain_matches_pin,
                    "sections": sections,
                    "msp_stack": stack,
                    "ram_total": ram_total,
                    "ram_budget": budget,
                    "heap": heap,
                    "heap_floor": heap_floor,
                    "static_ram": static_ram,
                    "static_budget": static_budget,
                    "flash_total": flash,
                },
                f,
                indent=2,
            )

    failed = False
    if ram_total > budget:
        print(
            f"FAIL: RAM demand {ram_total} exceeds budget {budget} "
            f"(over by {ram_total - budget} bytes). Shrink a tenant; raising "
            "the budget spends margin the chip does not have.",
            file=sys.stderr,
        )
        failed = True
    if static_ram > static_budget:
        print(
            f"FAIL: static RAM {static_ram} exceeds {static_budget}. This is "
            "the gate that stops new statics being paid for out of the heap.",
            file=sys.stderr,
        )
        failed = True
    if heap and heap < heap_floor:
        print(
            f"FAIL: heap {heap} is below the {heap_floor} floor. Shrinking "
            "configTOTAL_HEAP_SIZE to fit new statics moves the problem to "
            "runtime, where this script cannot see it.",
            file=sys.stderr,
        )
        failed = True
    if not heap:
        print("FAIL: ucHeap symbol not found; the heap gate cannot run.", file=sys.stderr)
        failed = True
    # The verdict line is the one that gets copied into PR bodies and chat, so it
    # carries the compiler with it. Without that, a local pass and a CI failure
    # on the same commit read as a contradiction rather than as two toolchains.
    if failed:
        print(
            f"(measured with arm-none-eabi-gcc {toolchain}, {toolchain_note})",
            file=sys.stderr,
        )
        return 1
    print(
        f"OK: {budget - ram_total} bytes under budget, static {static_ram}/{static_budget}, "
        f"heap {heap} >= {heap_floor} [arm-none-eabi-gcc {toolchain}, {toolchain_note}]"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
