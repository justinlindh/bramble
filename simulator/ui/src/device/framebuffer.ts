// framebuffer.ts
//
// Decodes the packed 1bpp e-paper framebuffer the firmware node streams over
// emu-link (extnode.go handleFB -> "device_fb" { fb: base64 }).
//
// Wire format (emulator/node display driver, GDEY0213B74 250x122):
//   - 3904 bytes = 122 rows x 32 bytes/row.
//   - Row-major, top row first.
//   - 32 bytes per row cover 256 bit columns; only the first 250 are active,
//     the last 6 bits of the row (low bits of byte 31) are padding.
//   - bit 7 (MSB) of byte 0 is the top-left pixel; bits walk MSB->LSB, left->right.
//   - A set bit is a black (inked) pixel; a clear bit is white paper.

export const FB_WIDTH = 250;
export const FB_HEIGHT = 122;
export const FB_ROW_BYTES = 32; // ceil(250 / 8)
export const FB_BYTES = FB_ROW_BYTES * FB_HEIGHT; // 3904

// Ink/paper as opaque RGBA. Real e-paper is a warm off-white with near-black
// ink; these read well behind the glass reveal without looking like an LCD.
const INK: [number, number, number] = [0x1a, 0x1c, 0x18];
const PAPER: [number, number, number] = [0xdd, 0xdc, 0xd2];

function decodeBase64(b64: string): Uint8Array {
  // atob is provided by browsers and by the jsdom test environment.
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

// unpackFramebuffer decodes a base64 1bpp frame into RGBA suitable for
// ImageData. Returns null if the payload is not the expected size (a truncated
// or malformed frame is dropped rather than smeared). `out`, when supplied, is
// reused to avoid per-frame allocation.
export function unpackFramebuffer(
  b64: string,
  out?: Uint8ClampedArray,
): Uint8ClampedArray | null {
  return unpackFramebufferSized(b64, FB_WIDTH, FB_HEIGHT, INK, PAPER, out);
}

// unpackFramebufferSized is the geometry-parameterized decoder shared by every
// panel. The wire layout is identical across panels (row-major 1bpp, MSB =
// leftmost pixel of its byte group, a set bit = a drawn/foreground pixel); only
// the panel dimensions and the foreground/background colors differ. The
// SSD1680 e-paper (250x122, dark ink on warm paper) and the SSD1306 OLED
// (128x64, light pixels on a dark panel) both flow through here. The row stride
// is ceil(width/8) bytes; any pad bits past `width` in the last byte of a row
// are simply never read.
export function unpackFramebufferSized(
  b64: string,
  width: number,
  height: number,
  fg: readonly [number, number, number],
  bg: readonly [number, number, number],
  out?: Uint8ClampedArray,
): Uint8ClampedArray | null {
  let bytes: Uint8Array;
  try {
    bytes = decodeBase64(b64);
  } catch {
    return null;
  }
  const rowBytes = (width + 7) >> 3;
  const need = rowBytes * height;
  if (bytes.length < need) return null;

  const rgba = out && out.length === width * height * 4
    ? out
    : new Uint8ClampedArray(width * height * 4);

  for (let y = 0; y < height; y++) {
    const rowBase = y * rowBytes;
    for (let x = 0; x < width; x++) {
      const byte = bytes[rowBase + (x >> 3)];
      const bit = (byte >> (7 - (x & 7))) & 1; // MSB first
      const [r, g, b] = bit ? fg : bg;
      const o = (y * width + x) * 4;
      rgba[o] = r;
      rgba[o + 1] = g;
      rgba[o + 2] = b;
      rgba[o + 3] = 255;
    }
  }
  return rgba;
}
