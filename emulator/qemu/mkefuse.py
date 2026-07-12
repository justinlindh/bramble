#!/usr/bin/env python3
"""Generate a QEMU esp32s3 eFuse image that skips ADC self-calibration.

The pager firmware links esp_adc (battery monitor). Its calibration runs in a
global constructor BEFORE app_main. On a blank eFuse the IDF falls back to
hardware self-calibration, which busy-waits on the SAR ADC that QEMU does not
model: the boot wedges before the scheduler starts. On real silicon the eFuse
always carries ADC calibration data, so the self-cal path never runs.

Setting BLK_VERSION_MAJOR (EFUSE_BLK2, bits 128-129) to 1 ("ADC calib V1")
makes esp_efuse_rtc_calib_get_ver() return 1; init codes then read as zeros
from eFuse and the driver derives its default ICodes without touching the ADC.
Verified against ESP-IDF v5.4 esp32s3 esp_efuse_table.csv.

QEMU eFuse file layout: flat little-endian words, BLOCK0 = 6 words,
BLOCK1 = 6 words, BLOCK2+ = 8 words each. BLK2 bit 128 = BLK2 word 4 =
file word 16 = byte 64. QEMU expects a 4096-byte image.
"""

import argparse

EFUSE_SIZE = 4096
BLK2_WORD4_OFFSET = (6 + 6 + 4) * 4  # BLOCK0(6w) + BLOCK1(6w) + 4 words into BLK2


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("output", nargs="?", default="efuse.bin")
    args = parser.parse_args()

    buf = bytearray(EFUSE_SIZE)
    buf[BLK2_WORD4_OFFSET] = 0x01  # BLK_VERSION_MAJOR = 1 (ADC calib V1)
    with open(args.output, "wb") as f:
        f.write(buf)
    print(f"wrote {args.output} ({EFUSE_SIZE} bytes, BLK_VERSION_MAJOR=1)")


if __name__ == "__main__":
    main()
