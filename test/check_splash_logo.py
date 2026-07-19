#!/usr/bin/env python3
"""Fail the build if the generated splash logo asset regresses.

The splash logo used to be a 296 KB C source holding a full 100x100 RGB565A8
array, three quarters of which was fully transparent padding. tools/convert_logo.py
now crops that padding off and scr_splash.c adds it back as LVGL layout margins,
which halves the flash cost while rendering identically.

Three things can silently undo that, so all three are gated here:

1. Regenerating the asset without the matching layout margins in scr_splash.c.
   The image would still draw, just shifted up and left, and nothing would fail.
2. Dropping the clang-format off/on guards around the byte arrays. The repo
   clang-format config reflows the file to one byte per line, which is what made
   the old version 30022 lines long and distorted every LOC measurement.
3. Reintroducing the banned dash character the old generator emitted in its
   header comment.

Stdlib only, matching check_lvgl_glyphs.py.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASSET_C = ROOT / "components" / "ui_graphics" / "assets" / "img_bramble_logo.c"
ASSET_H = ROOT / "components" / "ui_graphics" / "assets" / "img_bramble_logo.h"
SPLASH_C = ROOT / "components" / "ui_graphics" / "screens" / "scr_splash.c"

# U+2014, banned repo-wide. The old generator emitted one in its header comment.
BANNED_DASH = chr(0x2014)

# The uncropped square the art is designed against. The cropped image plus twice
# its margins must add back up to this on both axes.
EXPECTED_NOMINAL = 100


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    return 1


def main():
    errors = 0

    for path in (ASSET_C, ASSET_H, SPLASH_C, ROOT / "tools" / "convert_logo.py"):
        if not path.exists():
            return fail(f"{path.relative_to(ROOT)} is missing")

    asset = ASSET_C.read_text()
    header = ASSET_H.read_text()
    splash = SPLASH_C.read_text()

    # 1. The generator must not emit the banned dash, and neither may the splash screen.
    for path, text in ((ASSET_C, asset), (ASSET_H, header), (SPLASH_C, splash)):
        if BANNED_DASH in text:
            errors += fail(f"{path.relative_to(ROOT)} contains a banned dash character")

    # 2. clang-format guards must bracket the byte arrays.
    if "clang-format off" not in asset or "clang-format on" not in asset:
        errors += fail(
            f"{ASSET_C.relative_to(ROOT)} lost its clang-format off/on guards; "
            "clang-format will reflow it to one byte per line"
        )

    # 3. Descriptor must be internally consistent: declared size, stride, and the
    #    actual number of bytes in the array must all agree for w*h*3 RGB565A8.
    def need(pattern, text, what, path):
        m = re.search(pattern, text)
        if not m:
            print(f"FAIL: could not find {what} in {path.relative_to(ROOT)}", file=sys.stderr)
            return None
        return int(m.group(1))

    w = need(r"\.w\s*=\s*(\d+)", asset, ".w", ASSET_C)
    h = need(r"\.h\s*=\s*(\d+)", asset, ".h", ASSET_C)
    stride = need(r"\.stride\s*=\s*(\d+)", asset, ".stride", ASSET_C)
    data_size = need(r"\.data_size\s*=\s*(\d+)", asset, ".data_size", ASSET_C)
    if None in (w, h, stride, data_size):
        return 1

    if "LV_COLOR_FORMAT_RGB565A8" not in asset:
        errors += fail(
            f"{ASSET_C.relative_to(ROOT)} is no longer RGB565A8. Indexed and "
            "LV_IMAGE_FLAGS_COMPRESSED formats make LVGL allocate a full decode "
            "buffer in RAM instead of drawing straight from flash; see #113."
        )

    body = asset[asset.index("_map[]"):asset.index("};")]
    actual_bytes = len(re.findall(r"0x[0-9a-fA-F]{2}\b", body))

    if stride != w * 2:
        errors += fail(f"stride {stride} != w*2 ({w * 2}) for planar RGB565A8")
    if data_size != w * h * 3:
        errors += fail(f".data_size {data_size} != w*h*3 ({w * h * 3})")
    if actual_bytes != w * h * 3:
        errors += fail(f"array holds {actual_bytes} bytes, descriptor claims {w * h * 3}")

    # 4. Cropped size plus margins must reproduce the nominal square, which is
    #    what keeps the rendered logo in the same place as the uncropped image.
    margin_x = need(r"#define\s+IMG_BRAMBLE_LOGO_MARGIN_X\s+(\d+)", header, "MARGIN_X", ASSET_H)
    margin_y = need(r"#define\s+IMG_BRAMBLE_LOGO_MARGIN_Y\s+(\d+)", header, "MARGIN_Y", ASSET_H)
    nominal = need(r"#define\s+IMG_BRAMBLE_LOGO_NOMINAL\s+(\d+)", header, "NOMINAL", ASSET_H)
    if None in (margin_x, margin_y, nominal):
        return 1

    if nominal != EXPECTED_NOMINAL:
        errors += fail(f"nominal square {nominal} != expected {EXPECTED_NOMINAL}")
    if w + 2 * margin_x != nominal:
        errors += fail(
            f"w {w} + 2*margin_x {margin_x} != nominal {nominal}: logo would shift horizontally"
        )
    if h + 2 * margin_y != nominal:
        errors += fail(
            f"h {h} + 2*margin_y {margin_y} != nominal {nominal}: logo would shift vertically"
        )

    # 5. The crop must be tight on at least one side per axis: the alpha plane's
    #    outermost row/column must carry ink somewhere, otherwise there is still
    #    transparent padding sitting in flash.
    all_bytes = [int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})\b", body)]
    alpha = all_bytes[w * h * 2:]
    if len(alpha) == w * h:
        left = any(alpha[y * w] for y in range(h))
        right = any(alpha[y * w + w - 1] for y in range(h))
        top = any(alpha[0:w])
        bottom = any(alpha[(h - 1) * w:h * w])
        if not (left or right):
            errors += fail("both outer columns are fully transparent: crop is not tight")
        if not (top or bottom):
            errors += fail("both outer rows are fully transparent: crop is not tight")

    # 6. scr_splash.c must actually apply all four margins.
    for macro, setter in (
        ("IMG_BRAMBLE_LOGO_MARGIN_X", "lv_obj_set_style_margin_left"),
        ("IMG_BRAMBLE_LOGO_MARGIN_X", "lv_obj_set_style_margin_right"),
        ("IMG_BRAMBLE_LOGO_MARGIN_Y", "lv_obj_set_style_margin_top"),
        ("IMG_BRAMBLE_LOGO_MARGIN_Y", "lv_obj_set_style_margin_bottom"),
    ):
        if not re.search(rf"{setter}\s*\(\s*logo\s*,\s*{macro}\s*,", splash):
            errors += fail(
                f"scr_splash.c does not call {setter}(logo, {macro}, ...); "
                "the cropped logo would render offset"
            )

    if errors:
        return 1

    saved = EXPECTED_NOMINAL * EXPECTED_NOMINAL * 3 - data_size
    print(
        f"check_splash_logo: OK ({w}x{h} RGB565A8, {data_size} bytes in flash, "
        f"{saved} bytes saved vs the uncropped {nominal}x{nominal}, "
        f"margins {margin_x}x/{margin_y}y restored in scr_splash.c)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
