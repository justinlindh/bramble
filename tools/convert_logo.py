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
    """Write LVGL image C source.
    
    LVGL v9 LV_COLOR_FORMAT_RGB565A8 is PLANAR:
      - First: all RGB565 pixels (width * height * 2 bytes)
      - Then:  all alpha values (width * height bytes)
    stride = width * 2 (bytes per row, RGB565 only)
    """
    stride = width * 2
    with open(out_path, 'w') as f:
        f.write(f"/* Auto-generated from bramble-logo.png — do not edit manually */\n")
        f.write(f"/* Flash-resident: static const keeps this in .rodata (flash, not RAM) */\n\n")
        f.write(f"#include \"lvgl.h\"\n\n")

        f.write(f"/* {width}x{height} px — {width * height * 3} bytes in flash */\n")
        f.write(f"static const uint8_t {name}_map[] __attribute__((aligned(4))) = {{\n")

        # Planar: all RGB565 pixel data first
        f.write(f"  /* RGB565 pixel data ({width * height * 2} bytes) */\n")
        for row in range(height):
            row_pixels = rgb565[row * width:(row + 1) * width]
            chunks = []
            for px in row_pixels:
                lo = px & 0xFF
                hi = (px >> 8) & 0xFF
                chunks.append(f"0x{lo:02x}, 0x{hi:02x}")
            # 8 pixels (16 bytes) per line
            for i in range(0, len(chunks), 8):
                f.write("  " + ",  ".join(chunks[i:i+8]) + ",\n")

        # Then all alpha values
        f.write(f"  /* Alpha data ({width * height} bytes) */\n")
        for i in range(0, len(alpha), 16):
            chunk = alpha[i:i+16]
            f.write("  " + ", ".join(f"0x{a:02x}" for a in chunk) + ",\n")

        f.write(f"}};\n\n")

        f.write(f"const lv_image_dsc_t {name} = {{\n")
        f.write(f"  .header = {{\n")
        f.write(f"    .cf = LV_COLOR_FORMAT_RGB565A8,\n")
        f.write(f"    .w = {width},\n")
        f.write(f"    .h = {height},\n")
        f.write(f"    .stride = {stride},\n")
        f.write(f"  }},\n")
        f.write(f"  .data_size = {width * height * 3},\n")
        f.write(f"  .data = {name}_map,\n")
        f.write(f"}};\n")

    print(f"Written {out_path} ({width}x{height}, {width*height*3} bytes, stride={stride})")

def main():
    base = Path(__file__).parent.parent
    # Prefer the full webapp logo (512x512 RGBA) over the small favicon SVG
    png_primary = base / "webapp" / "public" / "bramble-logo.png"
    svg_path    = base / "webapp" / "public" / "favicon.svg"
    png_fallback = base / "assets" / "bramble-logo.png"

    size = 100  # 100x100 for splash on 320x240 T-Deck Plus

    if png_primary.exists():
        print(f"Resizing PNG: {png_primary}")
        img = Image.open(png_primary).convert("RGBA").resize((size, size), Image.LANCZOS)
    elif svg_path.exists():
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
