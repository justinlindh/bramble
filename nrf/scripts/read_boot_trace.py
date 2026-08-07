#!/usr/bin/env python3
"""Decode the Bramble boot-trace page out of the bootloader's CURRENT.UF2.

Consoleless nRF boards (SenseCAP T1000-E) record boot progress as (tag, aux)
word pairs in a reserved flash page (see src/boot_trace.h) and reboot into
the UF2 bootloader on any fatal condition. The bootloader's CURRENT.UF2
exposes all of flash, including that page, so a failed boot is diagnosable
from the host with no debugger attached:

  python3 nrf/scripts/read_boot_trace.py /run/media/$USER/T1000-E/CURRENT.UF2

The page holds several boots, each opened by a BOOT_BEGIN record carrying
the reset reason of the boot before it, so a device that resets in a loop
shows up as the same stage sequence repeated rather than as a single
truncated one. Boots are numbered from the oldest still on the page.

Tag names below mirror src/boot_trace.h; keep the two in sync.
"""
import struct
import sys

TAGS = {
    0x01: "MAIN_ENTRY(vtor)", 0x02: "CRYPTO_CHECK", 0x03: "CRYPTO_OK",
    0x04: "NVS_INIT(rc)", 0x05: "IDENTITY(rc)", 0x06: "IDENTITY_ADDR",
    0x07: "MSG_STORE", 0x08: "RPC_READY", 0x09: "TOKEN_SEED(rc)",
    0x0A: "TOKEN_LOADED", 0x0B: "GPS_INIT(rc)", 0x0C: "HFXO_OK",
    0x0D: "LFCLK(xtal)", 0x0E: "BLE_INIT(rc)", 0x0F: "BLE_START(rc)",
    0x10: "ADV(rc)", 0x11: "MESH_STARTED",
    0x14: "BOOT_BEGIN(resetreas)", 0x15: "BOOT_CARRY(failed)",
    0xDD: "BOOT_DONE(heap)",
    0xE1: "FAIL_ASSERT(line)", 0xE2: "FAIL_STACK_OVERFLOW",
    0xE3: "FAIL_MALLOC", 0xE4: "FAIL_SENTINEL(last)",
    0xE5: "FAIL_NRFX(line)", 0xE6: "FAIL_BOOTLOOP(failed)",
    0xEF: "FAIL_HARDFAULT(pc)",
}

BT_BOOT_BEGIN = 0x14
BT_BOOT_DONE = 0xDD
BT_TOKEN_SEED = 0x09

# nrf_seed_auth_token_from_build's return codes (nrf/src/nrf_provision.h).
# 1 is not a failure: it is what every build without a dev token returns.
TOKEN_SEED_RC = {
    0: "token in place (seeded or already stored)",
    1: "no dev token in this build (normal, minted by ws_server_load_token next)",
    # An image built before the skipped code existed returned -1 for both
    # "no dev token in this build" and a genuine failure, so a -1 off such
    # an image cannot be read as a failure on its own.
    -1: "seeding failed, OR an image predating the skipped code (ambiguous)",
}

# nRF52840 POWER->RESETREAS bit positions.
RESETREAS_BITS = {
    0: "RESETPIN", 1: "DOG", 2: "SREQ", 3: "LOCKUP",
    16: "OFF", 17: "LPCOMP", 18: "DIF", 19: "NFC", 20: "VBUS",
}


def decode_resetreas(aux):
    """Name the bits in a RESETREAS value.

    No bits set is the one case worth spelling out: the hardware reports a
    power-on reset and a brownout identically, so aux==0 on a boot nobody
    power-cycled is the brownout signature.
    """
    if aux == 0:
        return "power-on or BROWNOUT (no bits set)"
    names = [n for b, n in sorted(RESETREAS_BITS.items()) if aux & (1 << b)]
    return "+".join(names) if names else f"unknown bits 0x{aux:08x}"


def read_page(path):
    """Pull the 4KB trace page out of a UF2 dump of the whole flash."""
    data = open(path, "rb").read()
    page = {}
    for off in range(0, len(data) - 511, 512):
        m0, m1, _fl, addr, size, _no, _bl, _fam = struct.unpack_from("<IIIIIIII", data, off)
        if m0 != 0x0A324655 or m1 != 0x9E5D5157:
            continue
        if 0xBF000 <= addr < 0xC0000:
            page[addr] = data[off + 32 : off + 32 + size]
    if not page:
        return None
    return b"".join(page[a] for a in sorted(page))


def records(blob):
    """Yield (tag, aux) up to the first erased slot.

    Only the tag word is tested for the marker. An aux word may legitimately
    be 0xFFFFFFFF: NVS_INIT stamps a negative return code as one.
    """
    i = 4
    while i + 8 <= len(blob):
        tag, aux = struct.unpack_from("<II", blob, i)
        if (tag & 0xF0000000) != 0xB0000000:
            if tag != 0xFFFFFFFF:
                print(f"  [corrupt entry 0x{tag:08x}]")
            return
        yield tag & 0xFF, aux
        i += 8


def main(path):
    blob = read_page(path)
    if blob is None:
        print("trace page not present in dump")
        return 1
    (magic,) = struct.unpack_from("<I", blob, 0)
    if magic != 0x42545243:
        print(f"no trace magic (got 0x{magic:08x}): app never ran boot_trace_init")
        return 1

    boots = []
    for tag, aux in records(blob):
        if tag == BT_BOOT_BEGIN or not boots:
            boots.append([])
        boots[-1].append((tag, aux))

    if not boots:
        print("trace page is empty")
        return 1

    for n, boot in enumerate(boots, 1):
        completed = any(tag == BT_BOOT_DONE for tag, _ in boot)
        print(f"boot {n} of {len(boots)}{'' if completed else '  (never reached BOOT_DONE)'}:")
        for tag, aux in boot:
            note = ""
            if tag == BT_BOOT_BEGIN:
                note = f"  [{decode_resetreas(aux)}]"
            elif tag == BT_TOKEN_SEED:
                rc = aux - (1 << 32) if aux >> 31 else aux
                note = f"  [{TOKEN_SEED_RC.get(rc, f'unknown rc {rc}')}]"
            print(f"  {TAGS.get(tag, f'tag 0x{tag:02x}')} aux=0x{aux:08x} ({aux}){note}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
