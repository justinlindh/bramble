#!/usr/bin/env python3
"""Print "<app_flash_bytes> <static_ram_bytes>" from `idf.py size --format json`.

Both numbers are UNPADDED, so they move precisely with the code and data a change
adds and carry only a small (sub-KiB) delta between ESP-IDF patch versions:

  app flash  = flash_code + flash_rodata + flash_other (== used_flash_non_ram):
               the app's real flash footprint. The bramble.bin IMAGE is padded up
               to 64 KiB ESP32-S3 MMU pages, so its size jumps a whole page when a
               segment crosses a boundary; tracking the unpadded figure instead
               keeps the gate precise and free of that quantization. Partition fit
               is already enforced by the ESP-IDF build itself.
  static RAM = data + bss across the split (dram_*) and unified (diram_*) layouts:
               the RAM the app reserves before the heap (issue #94's headroom).

`idf.py size` prints ninja lines before the JSON object; this locates it by its
final standalone `{` line.
"""
import json
import sys


def main():
    lines = sys.stdin.read().splitlines()
    start = None
    for i, line in enumerate(lines):
        if line.strip() == "{":
            start = i
    if start is None:
        print("::error::no JSON object found in idf.py size output", file=sys.stderr)
        return 1
    try:
        doc = json.loads("\n".join(lines[start:]))
    except ValueError as exc:
        print(f"::error::could not parse idf.py size JSON: {exc}", file=sys.stderr)
        return 1

    app_flash = (
        doc.get("flash_code", 0)
        + doc.get("flash_rodata", 0)
        + doc.get("flash_other", 0)
    )
    static_ram = (
        doc.get("dram_data", 0)
        + doc.get("dram_bss", 0)
        + doc.get("diram_data", 0)
        + doc.get("diram_bss", 0)
    )
    print(f"{int(app_flash)} {int(static_ram)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
