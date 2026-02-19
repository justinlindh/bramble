#!/usr/bin/env python3
"""Convert Bramble logo SVG/PNG to LVGL-compatible C array (RGB565 + alpha).

Outputs:
  - 80x80 splash logo for T-Deck Plus (320x240 display)
  - C header with LVGL image descriptor

Source: bramble/webapp/public/favicon.svg (Bramble project, MIT license)
"""

import sys
import struct
from pathlib import Path

try:
    from PIL import Image
    import cairosvg
    import io
except ImportError:
    print("pip install pillow cairosvg")
    sys.exit(1)

def svg_to_png(svg_path, size):
    """Render SVG to PNG at given size."""
    png_data = cairosvg.svg2png(url=str(svg_path), output_width=size, output_height=size)
    return Image.open(io.BytesIO(png_data)).convert("RGBA")

def rgba_to_rgb565_alpha(img):
    """Convert RGBA image to RGB565 + 8-bit alpha arrays."""
    pixels = list(img.getdata())
    rgb565 = []
    alpha = []
    for r, g, b, a in pixels:
        # RGB565: RRRRR GGGGGG BBBBB
        val = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        rgb565.append(val)
        alpha.append(a)
    return rgb565, alpha

def write_c_array(rgb565, alpha, width, height, name, out_path):
    """Write LVGL image C source."""
    with open(out_path, 'w') as f:
        f.write(f"/* Auto-generated from Bramble logo - do not edit manually */\n")
        f.write(f"/* Source: bramble/webapp/public/favicon.svg (MIT license) */\n\n")
        f.write(f"#include \"lvgl.h\"\n\n")

        # CF_TRUE_COLOR_ALPHA: interleaved RGB565 + alpha
        f.write(f"static const uint8_t {name}_map[] = {{\n")
        for i, (px, a) in enumerate(zip(rgb565, alpha)):
            lo = px & 0xFF
            hi = (px >> 8) & 0xFF
            f.write(f"  0x{lo:02x}, 0x{hi:02x}, 0x{a:02x},")
            if (i + 1) % 8 == 0:
                f.write("\n")
        f.write(f"\n}};\n\n")

        f.write(f"const lv_image_dsc_t {name} = {{\n")
        f.write(f"  .header = {{\n")
        f.write(f"    .cf = LV_COLOR_FORMAT_RGB565A8,\n")
        f.write(f"    .w = {width},\n")
        f.write(f"    .h = {height},\n")
        f.write(f"  }},\n")
        f.write(f"  .data_size = {width * height * 3},\n")
        f.write(f"  .data = {name}_map,\n")
        f.write(f"}};\n")

    print(f"Written {out_path} ({width}x{height}, {width*height*3} bytes)")

def main():
    base = Path(__file__).parent.parent
    svg_path = base / "webapp" / "public" / "favicon.svg"
    png_fallback = base / "assets" / "bramble-logo.png"

    size = 80  # 80x80 for splash on 320x240

    if svg_path.exists():
        print(f"Converting SVG: {svg_path}")
        img = svg_to_png(svg_path, size)
    elif png_fallback.exists():
        print(f"Resizing PNG: {png_fallback}")
        img = Image.open(png_fallback).convert("RGBA").resize((size, size), Image.LANCZOS)
    else:
        print("No logo source found!")
        sys.exit(1)

    # Also save a preview PNG
    preview_path = base / "assets" / f"bramble-logo-{size}x{size}.png"
    img.save(preview_path)
    print(f"Preview saved: {preview_path}")

    rgb565, alpha = rgba_to_rgb565_alpha(img)
    out_path = base / "components" / "ui_graphics" / "assets" / "img_bramble_logo.c"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    write_c_array(rgb565, alpha, size, size, "img_bramble_logo", out_path)

if __name__ == "__main__":
    main()
