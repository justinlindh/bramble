// paintLog.ts
//
// Test-only instrumentation for the e-paper canvas. When the E2E harness sets
// window.__EPAPER_CAPTURE__ = true (via an init script BEFORE the app mounts),
// Epaper.tsx records every CONTENT frame it paints to the canvas into a
// per-node ring buffer on window.__epaperPaints__.
//
// Why this exists: some rendered frames are TRANSIENT. The firmware boot splash
// ("BRAMBLE") is painted, then overpainted by the home screen a second or two
// later; the delivered-message screen, by contrast, is persistent. A test that
// samples the LIVE canvas for a transient frame races the overpaint and loses
// intermittently under CI CPU contention (issue #170: widening the sampling
// budget did not help, because the frame is gone from the live surface, not
// merely late). Recording the applied paint HISTORY lets a spec assert that a
// frame was genuinely painted at some point, event-driven and race-free,
// exactly the redesign display-correctness.spec.ts's removed flash test calls
// for.
//
// Disabled by default: with the flag unset (normal app and dev use), nothing is
// recorded and no memory is retained.

export const PAINT_CAPTURE_FLAG = '__EPAPER_CAPTURE__' as const;
export const PAINT_LOG_KEY = '__epaperPaints__' as const;

// Retain the last N content paints per node. The firmware boot sequence emits
// well under this many content frames before a spec reads the history, and a
// spec reads the boot history before any later message traffic, so the ring
// never evicts a frame a boot-time assertion still needs while still bounding
// memory across a whole session.
const RING = 32;

export interface PaintFrame {
  seq: number;
  // Red channel only, one entry per pixel (row-major, FB_WIDTH*FB_HEIGHT). The
  // e-paper is monochrome ink/paper, so red alone classifies every pixel; this
  // is a quarter the size of full RGBA to keep the window buffer and the CDP
  // transfer to the test small.
  red: number[];
}

export type PaintLog = Record<string, PaintFrame[]>;

interface CaptureWindow {
  [PAINT_CAPTURE_FLAG]?: boolean;
  [PAINT_LOG_KEY]?: PaintLog;
}

function captureWindow(): CaptureWindow {
  return window as unknown as CaptureWindow;
}

export function captureEnabled(): boolean {
  return typeof window !== 'undefined' && captureWindow()[PAINT_CAPTURE_FLAG] === true;
}

// recordContentPaint appends one painted content frame's red channel to the
// node's ring buffer. Called only from Epaper's content-paint path, and only
// when captureEnabled(); never in normal app use.
export function recordContentPaint(node: string, rgba: Uint8ClampedArray, seq: number): void {
  const w = captureWindow();
  const log: PaintLog = w[PAINT_LOG_KEY] ?? (w[PAINT_LOG_KEY] = {});
  const arr: PaintFrame[] = log[node] ?? (log[node] = []);
  const red: number[] = new Array(rgba.length >> 2);
  for (let i = 0, p = 0; i < rgba.length; i += 4, p++) red[p] = rgba[i];
  arr.push({ seq, red });
  if (arr.length > RING) arr.splice(0, arr.length - RING);
}
