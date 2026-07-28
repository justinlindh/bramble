#!/usr/bin/env python3
"""Minimal UF2 writer for the nRF52840 (drag-and-drop flashing via the
Adafruit UF2 bootloader on the SenseCAP T1000-E).

The UF2 container format is public domain / MIT (Microsoft,
github.com/microsoft/uf2): 512-byte blocks, 256 payload bytes each, with the
nRF52840 family ID 0xADA52840. This is a from-scratch implementation of that
spec, not a copy of uf2conv.py.

Usage: make_uf2.py input.bin --base 0x26000 -o output.uf2 [--info FILE.uf2]
"""

import argparse
import struct
import sys

MAGIC0 = 0x0A324655
MAGIC1 = 0x9E5D5157
MAGIC_END = 0x0AB16F30
FLAG_FAMILY_ID = 0x00002000
FAMILY_NRF52840 = 0xADA52840
PAYLOAD = 256

# Do not "correct" the family ID to the board-specific one that appears in the
# T1000-E bootloader's own CURRENT.UF2 header (0x28860057, its USB VID:PID).
# That value is how the bootloader TAGS the flash dump it exposes for reading;
# it is not what it accepts for writing. Seeed's stock bootloader accepts the
# generic ID above, which is what the official Meshtastic T1000-E image ships
# with (verified against firmware-tracker-t1000-e-2.7.26: base 0x27000,
# family 0xADA52840). A readback artifact is not an acceptance oracle.


def write_uf2(data: bytes, base: int, out_path: str) -> int:
    blocks = (len(data) + PAYLOAD - 1) // PAYLOAD
    with open(out_path, "wb") as f:
        for i in range(blocks):
            chunk = data[i * PAYLOAD : (i + 1) * PAYLOAD]
            block = struct.pack(
                "<IIIIIIII",
                MAGIC0,
                MAGIC1,
                FLAG_FAMILY_ID,
                base + i * PAYLOAD,
                len(chunk),
                i,
                blocks,
                FAMILY_NRF52840,
            )
            block += chunk + b"\x00" * (476 - len(chunk))
            block += struct.pack("<I", MAGIC_END)
            assert len(block) == 512
            f.write(block)
    return blocks


def info(path: str) -> int:
    with open(path, "rb") as f:
        first = f.read(512)
        f.seek(0, 2)
        total = f.tell()
    m0, m1, flags, addr, size, _no, blocks, family = struct.unpack_from("<IIIIIIII", first)
    ok = m0 == MAGIC0 and m1 == MAGIC1
    print(
        f"{path}: magic {'ok' if ok else 'BAD'}, base 0x{addr:x}, family 0x{family:x}, "
        f"{blocks} blocks ({total} bytes), payload {size}/block, flags 0x{flags:x}"
    )
    return 0 if ok and family == FAMILY_NRF52840 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", nargs="?")
    ap.add_argument("--base", type=lambda v: int(v, 0), default=0x26000)
    ap.add_argument("-o", "--output")
    ap.add_argument("--info", help="print header info for an existing .uf2 and exit")
    args = ap.parse_args()

    if args.info:
        return info(args.info)
    if not args.input or not args.output:
        ap.error("input and -o are required unless --info is used")
    with open(args.input, "rb") as f:
        data = f.read()
    blocks = write_uf2(data, args.base, args.output)
    print(f"{args.output}: {blocks} blocks, base 0x{args.base:x}, family 0x{FAMILY_NRF52840:x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
