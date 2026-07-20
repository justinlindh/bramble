// canvasCapture.ts
//
// Reads the e-paper canvas's applied paint HISTORY, not just its live pixels.
//
// readCanvasGrid (canvasRead.ts) samples whatever the canvas shows RIGHT NOW.
// That is correct for a persistent frame (a delivered message stays on screen),
// but it races a TRANSIENT frame: the firmware boot splash ("BRAMBLE") is
// painted and then overpainted by the home screen a second or two later, so a
// live readback can miss it entirely under CI CPU contention no matter how long
// it polls (issue #170: the frame is gone from the live surface, not late).
//
// The app (Epaper.tsx + paintLog.ts) records every content frame it paints into
// a per-node ring buffer on window.__epaperPaints__, but only when this module
// has armed capture via enableCanvasCapture() before load. readPaintHistory()
// pulls that buffer back as decoded BitGrids so a spec can search the frames
// that were actually painted, event-driven and free of the overpaint race.

import type { Page } from '@playwright/test';
import { FB_WIDTH, FB_HEIGHT, type BitGrid } from './fbWire';

// Same ink/paper midpoint canvasRead.ts uses: real ink is near-black, paper is
// near-white, so 128 on the red channel classifies every pixel.
const INK_THRESHOLD = 128;

// Arm the app's paint capture. MUST be called before page.goto: it installs an
// init script that sets the flag Epaper reads on its first paint, and init
// scripts run on every navigation before any app code.
export async function enableCanvasCapture(page: Page): Promise<void> {
  await page.addInitScript(() => {
    (window as unknown as { __EPAPER_CAPTURE__?: boolean }).__EPAPER_CAPTURE__ = true;
  });
}

// readPaintHistory returns every content frame the given node has painted (up
// to the app's ring-buffer cap), oldest first, decoded into the same BitGrid
// shape fbWire.ts and canvasRead.ts produce, so glyphMatch.findText works on
// them directly.
//
// This ships and scans the WHOLE history each poll (cost linear in frame
// count), which partly offsets readCanvasGrid's byte-per-pixel CDP saving.
// That is a deliberate trade of payload size for race-freedom: transferring
// the full history is what lets a spec find a transient frame that the live
// canvas has already overpainted, which is the entire point of this module.
// The boot sequence is ~8 content frames, so the linear factor is negligible;
// were a caller ever to poll this against a long-running frame stream, cap the
// scan to recent frames rather than reintroduce the live-canvas race.
export async function readPaintHistory(page: Page, node: string): Promise<BitGrid[]> {
  const frames = await page.evaluate((n) => {
    const log = (window as unknown as {
      __epaperPaints__?: Record<string, Array<{ seq: number; red: number[] }>>;
    }).__epaperPaints__;
    return log && log[n] ? log[n].map((f) => f.red) : [];
  }, node);

  return frames.map((red) => {
    const grid: BitGrid = [];
    for (let y = 0; y < FB_HEIGHT; y++) {
      const row: boolean[] = new Array(FB_WIDTH);
      for (let x = 0; x < FB_WIDTH; x++) {
        row[x] = red[y * FB_WIDTH + x] < INK_THRESHOLD;
      }
      grid.push(row);
    }
    return grid;
  });
}
