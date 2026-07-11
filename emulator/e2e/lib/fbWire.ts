// fbWire.ts
//
// Independent decode of the raw emu-link "device_fb" wire payload (the exact
// base64 string captured off the browser's WebSocket, NOT the app's decoded
// canvas). Deliberately does not import simulator/ui/src/device/framebuffer.ts:
// the whole point of the display-correctness spec is to prove the UI's own
// decode+paint pipeline is correct by comparing it against an independently
// written reference, so sharing code with the thing under test would make the
// comparison tautological.
//
// Wire format (components/display/display_virt.c, ssd1680_engine.h): 250x122,
// 1bpp, row-major, 32 bytes/row, MSB = leftmost pixel, a set bit = black ink.

export const FB_WIDTH = 250;
export const FB_HEIGHT = 122;
export const FB_STRIDE = 32; // ceil(250/8)
export const FB_SIZE = FB_STRIDE * FB_HEIGHT; // 3904

// A decoded framebuffer: bit[y][x] === true means inked (black).
export type BitGrid = boolean[][];

function decodeBase64(b64: string): Uint8Array {
  const bin = Buffer.from(b64, 'base64');
  return new Uint8Array(bin.buffer, bin.byteOffset, bin.byteLength);
}

// decodeFbWire turns the base64 "fb" field of a device_fb event into a
// [height][width] boolean grid. Throws if the payload doesn't decode to at
// least FB_SIZE bytes (a truncated/malformed frame is a hard test failure,
// not a silently-skipped one, unlike the app's defensive framebuffer.ts).
export function decodeFbWire(b64: string): BitGrid {
  const bytes = decodeBase64(b64);
  if (bytes.length < FB_SIZE) {
    throw new Error(`fbWire: payload too short: ${bytes.length} bytes, want >= ${FB_SIZE}`);
  }
  const grid: BitGrid = [];
  for (let y = 0; y < FB_HEIGHT; y++) {
    const row: boolean[] = new Array(FB_WIDTH);
    const rowBase = y * FB_STRIDE;
    for (let x = 0; x < FB_WIDTH; x++) {
      const byte = bytes[rowBase + (x >> 3)];
      row[x] = ((byte >> (7 - (x & 7))) & 1) === 1;
    }
    grid.push(row);
  }
  return grid;
}

// gridEquals compares two grids pixel-for-pixel. Used for the strict
// fb-vs-canvas equality assertion (a decode/orientation/inversion bug in the
// app must make this fail).
export function gridEquals(a: BitGrid, b: BitGrid): boolean {
  if (a.length !== b.length) return false;
  for (let y = 0; y < a.length; y++) {
    if (a[y].length !== b[y].length) return false;
    for (let x = 0; x < a[y].length; x++) {
      if (a[y][x] !== b[y][x]) return false;
    }
  }
  return true;
}

// diffCount returns the number of pixels that differ, for a useful failure
// message (rather than a bare boolean).
export function diffCount(a: BitGrid, b: BitGrid): number {
  let n = 0;
  const h = Math.min(a.length, b.length);
  for (let y = 0; y < h; y++) {
    const w = Math.min(a[y].length, b[y].length);
    for (let x = 0; x < w; x++) {
      if (a[y][x] !== b[y][x]) n++;
    }
  }
  return n;
}
