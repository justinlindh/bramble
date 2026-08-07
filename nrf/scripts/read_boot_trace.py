#!/usr/bin/env python3
"""Decode the Bramble boot-trace page out of the bootloader's CURRENT.UF2.

Consoleless nRF boards (SenseCAP T1000-E) record boot progress as (tag, aux)
word pairs in a reserved flash page (see src/boot_trace.h) and reboot into
the UF2 bootloader on any fatal condition. The bootloader's CURRENT.UF2
exposes all of flash, including that page, so a failed boot is diagnosable
from the host with no debugger attached:

  python3 nrf/scripts/read_boot_trace.py /run/media/$USER/T1000-E/CURRENT.UF2

Tag names below mirror src/boot_trace.h; keep the two in sync.
"""
import struct
import sys

TAGS = {
    0x01: "MAIN_ENTRY(vtor)", 0x02: "CRYPTO_CHECK", 0x03: "CRYPTO_OK",
    0x04: "NVS_INIT(rc)", 0x05: "IDENTITY(rc)", 0x06: "IDENTITY_ADDR",
    0x07: "MSG_STORE", 0x08: "RPC_READY", 0x09: "TOKEN_SEED(rc)",
    0x0A: "TOKEN_LOADED", 0x0C: "HFXO_OK", 0x0D: "LFCLK(xtal)",
    0x0E: "BLE_INIT(rc)", 0x0F: "BLE_START(rc)", 0x10: "ADV(rc)",
    0x11: "MESH_STARTED", 0x12: "BATTERY_INIT(probe)", 0x13: "BATTERY_MV(mv)",
    0xDD: "BOOT_DONE(heap)",
    0xE1: "FAIL_ASSERT(line)", 0xE2: "FAIL_STACK_OVERFLOW",
    0xE3: "FAIL_MALLOC", 0xE4: "FAIL_SENTINEL(last)", 0xEF: "FAIL_HARDFAULT(pc)",
}

def main(path):
    data = open(path, "rb").read()
    page = {}
    for off in range(0, len(data) - 511, 512):
        m0, m1, _fl, addr, size, _no, _bl, _fam = struct.unpack_from("<IIIIIIII", data, off)
        if m0 != 0x0A324655 or m1 != 0x9E5D5157:
            continue
        if 0xBF000 <= addr < 0xC0000:
            page[addr] = data[off + 32 : off + 32 + size]
    if not page:
        print("trace page not present in dump")
        return 1
    blob = b"".join(page[a] for a in sorted(page))
    (magic,) = struct.unpack_from("<I", blob, 0)
    if magic != 0x42545243:
        print(f"no trace magic (got 0x{magic:08x}): app never ran boot_trace_init")
        return 1
    print("boot trace:")
    i = 4
    while i + 8 <= len(blob):
        tag, aux = struct.unpack_from("<II", blob, i)
        if tag == 0xFFFFFFFF:
            break
        if (tag & 0xFFFFFF00) != 0xB0000000:
            print(f"  [corrupt entry 0x{tag:08x}]")
            break
        t = tag & 0xFF
        print(f"  {TAGS.get(t, f'tag 0x{t:02x}')} aux=0x{aux:08x} ({aux})")
        i += 8
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
