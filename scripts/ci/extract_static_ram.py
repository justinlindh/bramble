#!/usr/bin/env python3
"""Print the static-DRAM byte count from `idf.py size --format json` output.

`idf.py size` prints ninja build lines before the JSON object, so the input on
stdin is a preamble followed by one pretty-printed JSON object. This locates the
JSON by its final standalone `{` line and sums the statically allocated data and
bss across both the split (dram_*) and unified (diram_*) memory layouts, which is
the RAM the app reserves before the heap: the RAM-headroom number issue #94
tracks. On ESP32-S3 the split dram_* fields are 0 and the diram_* fields carry
the real values, so summing both is correct for every board.
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
    static_ram = (
        doc.get("dram_data", 0)
        + doc.get("dram_bss", 0)
        + doc.get("diram_data", 0)
        + doc.get("diram_bss", 0)
    )
    print(int(static_ram))
    return 0


if __name__ == "__main__":
    sys.exit(main())
